# Architecture

This is how a 3DS dump becomes a Wii U title, and where each step lives in the code. Paths are relative to the
repository root. `3dsrecomp/` is short for `third_party/3dsrecomp/`.

## From a dump to a WUP

```
 your dump (.3ds .cci .cia .cxi .app)
   │  builder/decrypt.py     makerom + pyctr, with your own 3DS keys (only for encrypted dumps)
   │  builder/dumps.py       checks every part against the hashes in its own header, then unpacks it
   │                         (tools/extract_game.py does the same from the command line)
   ▼
 games/<name>/               exheader.bin, code.bin, romfs.bin, exefs/, info.json
   │  builder/mods.py        optional Luma-style mods: RomFS files, IPS/BPS code patches
   │  3dsrecomp/tools/recomp ARM and Thumb code to C, one C function per guest function
   ▼
 games/<name>/gen_abi/       the game as C (never committed: it's derived from the game's code)
   │  devkitPPC (GCC)        compiles the game's C, the runtime and port/wiiu for the Wii U's CPU
   │  ld + elf2rpl (wut)     links one ELF, converts it to an RPX
   ▼
 <out>/<name>.rpx            plus game.nm, the symbol map for reading console logs
   │  shadergen + CafeGLSL   every shader the game needs, compiled ahead of time
   ▼
 <out>/shaders/              shaders.gsh (compiled shaders) and shaders.idx (which key is which shader)
   │  tools/make_game_title.py   code/, content/ and meta/ folders
   │  tools/pack_game_wup.py     encrypts with the common key from your Wii U's otp.bin
   ▼
 dist/<folder>/              a WUP: title.tmd, title.tik, title.cert and the .app/.h3 files
     → SD card → WUP Installer GX2 → the Wii U Menu
```

`tools/win/build3ds.py` runs the middle steps (`gen`, `code`, `shaders`, `title`, `pack`). `builder/pipeline.py` runs
the whole thing for the builder app, in work folders of its own.

## The recompiler: `3dsrecomp/tools/recomp/`

The recompiler is static: all of the game's code is translated before the game runs.

- `arm.py` and `thumb.py` decode ARMv6K instructions, VFPv2 included, and lower each one to C. Guest registers become
  C locals and flags become separate 0/1 locals, so the C compiler can drop the ones nobody reads.
- `gen.py` finds the functions and writes the C. It starts from the entry point, `bl` targets, tail branches, code
  pointers in data and jump tables. A call is a C call followed by a check that the callee returned where expected.
  When it didn't, the code unwinds to a dispatcher that resumes at the right instruction.
- `cro.py` handles CRO modules, the relocatable code some games load from their RomFS. Each module gets a fixed slot,
  so recompiled code can know its addresses. The Wii U build puts the slots at `0x1C000000` (`RECOMP_CRO_BASE`), inside
  the 512 MB the guest has there.

This project adds an `--abi` mode (the `gen_abi` output). It trusts the ARM calling convention: r4-r11, sp and
s16-s31 survive calls, so they stay in C locals and only argument and return registers go through memory. That makes
the code about 30% smaller. Hand-written code that breaks the convention (soft-float helpers, video decoders) is
detected, and its call sites sync every register they need. Games whose code breaks it in ways that can't be detected
build in the older safe mode (`gen_safe`): every register goes through memory at every call. `tools/win/build3ds.py`
picks it from the game's own code, the same way for every game:
- 8 or more of Mobiclip's offset-table jumps: its video decoder keeps values in any register across its own calls.
- a call to setjmp followed within a few instructions by a call to longjmp: tasks that switch stacks, as Ace Attorney
  Trilogy's script threads do. Their longjmp lands in a frame the C code has already left. Safe mode has a resume point
  at every return site, so the dispatcher can carry on there.

**Code the analysis missed.** On a PC, a jump to code that wasn't recompiled is compiled on the spot and recorded in
`functions.txt`. A console can't do that, so missed code there is a crash. That's why every game goes through these
steps before its first console build:

- `tools/find_entries.py` and `tools/cro_entries.py` look for code reached only in hand-written ways, such as computed
  `lr` values and calls through tables.
- A PC trial (`build3ds.py trial`) plays the game headless and adds what it hits to `functions.txt`.

## Guest memory and the CPU

- The 3DS address map fits under `0x20000000`. On the Wii U, `3dsrecomp/runtime/core.cpp` allocates one flat block of
  `0x20010000` bytes for it. 64-bit hosts reserve 4 GiB instead.
- Guest memory stays little-endian, as on a 3DS. [`port/cpu.h`](port/cpu.h) replaces the memory macros on the
  big-endian Wii U with byte-reversed loads and stores (`lwbrx` and friends). They take base plus index, so a guest
  access costs one instruction, the same as a native one.
- Generated C is compiled with `-ffp-contract=off` and `-fno-strict-aliasing`, so floating-point results match a 3DS
  and the casts in the lowering behave.

## The runtime: `3dsrecomp/runtime/`

The runtime replaces the 3DS operating system and hardware. The same sources build for the Wii U and for the PC.

- `core.cpp`: guest memory, the dispatcher and the log. On the console, the log lives in memory and goes to the PC
  over the network (see `net_console.cpp` below).
- `kernel.cpp`: threads, events, mutexes, semaphores, timers, address arbiters, shared memory and the SVC interface.
  Each guest thread is a host thread, but only one runs at a time, under strict priority scheduling like the 3DS's
  application core.
- `service.cpp` and `services/`: the system services the game talks to over IPC. These include srv, APT, fs (RomFS,
  saves, and an update's or DLC's RomFS), gsp (the GPU command queue, framebuffers and LCD registers), hid (buttons,
  touch, scripted input), cfg, dsp, mic, cam, y2r, am:app (add-on content) and ldr:ro (CRO modules). The rest are stubs.
- `gpu/`: the PICA200. It decodes registers into draw state, interprets vertex programs, decodes textures and does
  display transfers. Its software rasterizer is the reference renderer, and the PC build uses it.
- `audio/`: an HLE of the DSP's audio firmware. AAC streams decode through FFmpeg (`port/aac/aac_ffmpeg.cpp`).

The `.mm` files, `frontend.cpp`, `main.cpp` and `audio/host_sdl.cpp` are upstream's macOS and iOS frontend, Metal
renderer and SDL audio output. The Wii U and PC builds leave them out.

## Graphics on the Wii U: `port/wiiu/gx2_backend.cpp`

The backend splits the work the way upstream's Metal renderer does. `runtime/gpu` decodes the PICA200's registers into
a `DrawState`, and the backend turns each draw into GX2 state and a draw call.

**Shaders are compiled ahead of time.** Nothing is compiled when the game first needs it, so there's no stutter.

- [`shadergen.cpp`](port/wiiu/shadergen.cpp) writes GLSL. A fragment shader covers one fragment-pipeline state: TEV
  combiners, fragment lighting, fog, alpha test. A vertex shader covers one PICA vertex program.
- [`shaderkeys.cpp`](port/wiiu/shaderkeys.cpp) defines the keys that name each shader. The generator on the PC and the
  renderer on the console compile the same code, so both compute the same key.
- [`tools/shadergen_main.cpp`](tools/shadergen_main.cpp) generates every shader a game needs. CafeGLSL v0.2.0
  (`third_party/cafeglsl/glslcompiler.exe`) compiles them, and `build3ds.py` merges the results into `shaders.gsh`.
- The shaders a game needs come from several places:
  - the PC trial's logs (`vs_log.bin`, `fs_log.bin`)
  - every vertex program found in the game's files (`tools/dvlb_to_log.py`)
  - every material in its model files (`tools/tev_to_log.py`)
  - other games' logs, because fragment states don't depend on the game
  - misses reported by consoles (`tools/misses_to_log.py`)

**When a shader is missing,** a fragment state draws with a generic "uber" shader or a plain fallback, and is logged.
A vertex program without a shader is shaded on the CPU. The console can also compile missing pixel shaders itself, on a
thread of its own, with CafeGLSL's Wii U library ([`shader_compile.cpp`](port/wiiu/shader_compile.cpp)). The GLSL of
each miss is kept in the save folder, so the next start compiles it before it's needed. A file named `noglslcompile`
in the data folder turns this off.

The rest of the backend, in the order of the file's sections:

- surfaces and render targets
- textures
- render state
- draws with a compiled vertex shader
- draws shaded on the CPU
- reading the GPU's work back to guest memory
- presenting: the top screen is drawn at 1200x720 and scaled to the TV, and the bottom screen goes to the GamePad
- the options menu, with its own small font
- the software keyboard

## The Wii U frontend: `port/wiiu/`

| File | What it does |
|---|---|
| `frontend_wiiu.cpp` | Start-up, threads and cores, VBlank, ProcUI and the HOME Menu, GamePad input, a watchdog that puts the start-up stage and thread addresses on screen if nothing is drawn |
| `audio_wiiu.cpp` | Two looping AX voices that play what the DSP HLE mixes |
| `save_storage.cpp` | Saves in the title's own save storage on the console, through `nn_save` |
| `fsa_direct.cpp`, `direct_read.cpp` | File calls that skip Aroma's ContentRedirection hooks (see the lessons below) |
| `net_console.cpp` | The log, thread dumps and debug commands over TCP port 4405 |
| `console_profile.cpp` | A sampling profiler for the real console: an empty `profile` file in the save folder turns it on |
| `mic_wiiu.cpp` | The GamePad microphone for mic:u. Holding the right stick in counts as blowing |
| `stubs.cpp`, `shim.h`, `TargetConditionals.h`, `mach/` | Stand-ins for the macOS calls the upstream runtime makes |

Guest threads share core 1, which has the 2 MB L2 cache. GPU work runs on core 2, and input and audio on core 0.

### The console's Miis

A game that takes Miis gets the Wii U's own. That is a rule for every game: the hand-over is shared code, and nothing about it is set per game.

- At start, `frontend_wiiu.cpp` reads each account's Mii (`nn::act`) and then Mii Maker's own database (`FFL_ODB.dat`, in its save).
  - The database is not proven on a console yet: whether a console lets a game open Mii Maker's save, and the file's layout, which is taken from the 3DS's. If it can't be opened, the game gets the accounts' Miis alone. A slot is only taken if what it holds looks like a Mii.
- `mii_3ds()` in `services/misc.cpp` turns each one into what a 3DS game reads:
  - A Mii comes either the way a 3DS keeps one or the way the Wii U's processor reads it, with its fields the other way round. The second kind is turned round, all of it.
  - A Mii made on a console newer than the 3DS is said to be a 3DS's. Super Mario 3D Land, of 2011, takes no other: it said "You don't have any Mii characters", and a save file lost its face at the next start.
- They go into the game's own Mii Maker database, 100 at most (`sync_mii_db()` in `services/fs.cpp`), where a game that lists Miis finds them.
  - A Mii stays at the place it had at an earlier start, a new one takes the first free place, and each names the place it lies at. A 3DS keeps a Mii at the place it names, and games find it again by that: 3D Land showed no face on a save file whose Mii named a place where another Mii lay.
- The current account's Mii is the player's. The friends service and the Mii selector applet hand it out, naming the same place as its copy in the database, where it is placed first and always has a place. The selector answers at once and shows no choice.
- `tools/check_ports.py` gives every run an account Mii and a small Mii Maker database, made up, with Miis in both layouts and a slot that holds none. A run fails if the game's Mii Maker ends up with more or fewer Miis than those. Cemu's own account Mii is a placeholder, so without them a run tested a console with no Mii at all.

## The PC build: `port/pc/`

`frontend_pc.cpp` runs the same runtime headless on Windows, with the software rasterizer. It mirrors the console's
threads and VBlank pacing. Input comes from a script (`RECOMP_AUTOPLAY`, or lines in a `live.txt` file) and both screens
are written as image files. `build3ds.py trial` builds it with LLVM and MinGW and plays a game for a set number of
frames. A trial does three jobs:

- it records code the recompiler missed, in `functions.txt`
- it logs the vertex programs and fragment states it meets (`vs_log.cpp`)
- it leaves a contact sheet of screenshots, to see at a glance how far the game got

## The build: `tools/win/build3ds.py`

| Command | What it does |
|---|---|
| `gen <game>` | The game's ARM code to C again |
| `code <game>` | The runtime, the game's C and the link: the RPX and `game.nm` |
| `shaders <game>` | The shader pack |
| `title <game>` | The title folder |
| `all <game>` | `code`, `shaders` and `title` |
| `pack <game> <otp.bin>` | The WUP |
| `trial <game> [frames]` | The PC trial |

Compiles run at below-normal priority, at most `--jobs` at once (4 by default), and none starts while the PC is short
of free memory. Objects go in `build_win/`. A build only recompiles what changed: each object remembers its command
line, and the compiler's dependency files cover headers.

Games come in three families, which differ in a few switches (`Game` in `build3ds.py`):

- Tomodachi Life (`build/`)
- the Mario games and the builder's untested games (`build_mario/<game>/`)
- the Layton games (`build_layton/<game>/`, safe mode at `-O1`)

## Titles and WUPs

`tools/make_game_title.py` assembles three folders:

- `code/`: the RPX, `app.xml` and `cos.xml`. The code heap (`max_codesize`) is set to the maximum, 224 MB, because it
  also holds the system's libraries.
- `content/`: the game's `exheader.bin`, `code.bin` and `romfs.bin` (hard links, not copies), its icon, the 3DS system
  archives, and `shaders.gsh` and `shaders.idx`. An update's or DLC's RomFS goes here too.
- `meta/`: `meta.xml` (title ID, names, save sizes), the icons and boot images it draws itself, and `bootMovie.h264`.

The templates are in `phase0/title_code/`, and `tools/make_game_title.py` makes every game's title from them. Tested
games keep their title IDs and names in `builder/compat.json`. Untested games get
`000500003Dxxxxxx`, which is kept clear of real Wii U titles.

`tools/pack_game_wup.py` reads the Wii U common key from your `otp.bin` and hands it to `phase0/tools/pack_wup.py`
over stdin, so it's never printed or stored. The packer writes the TMD, ticket, certificates and encrypted contents
with their hash trees. Then it decrypts everything it wrote and compares it with the input.

## The builder: `builder/`

- `app.py` is the single program. It runs the window, the build when started with `--pipeline`, and Python's part for
  the tool scripts a build runs.
- `gui.py` and `ui/Main.qml` are the window: Qt Quick, with the state and actions in Python.
- `pipeline.py` goes through the stages: read, decrypt, unpack, find code, recompile, shaders, title, pack, copy. It
  prints `@stage`, `@progress`, `@done` and `@error` lines for the window to follow.
- `dumps.py` reads CIA, NCSD and NCCH containers, and checks every part against its header's hashes. `decrypt.py` and
  `cia_to_decrypted_3ds.py` decrypt.
- `mods.py` applies Luma-style mods. Its RomFS writer rebuilds each tested game's RomFS byte for byte when nothing
  changes.
- `shaderlogs.py` swaps the vertex program code in shader logs for fingerprints, because that code is the game's own.
  A build finds each program again in the user's dump. A shader memory no DVLB program starts goes as a recipe instead:
  where its words lie in the game's code.bin and romfs, checked against the fingerprint when the build lays it out.
- `make_app.py` builds the standalone app with PyInstaller. The app carries an `engine/` folder: the recompiler and
  runtime sources, the build scripts, devkitPPC and the shader compilers.

[`builder/README.md`](builder/README.md) has the details.

## Lessons from real hardware

Cemu is good for fast checks, but it differs from a console in ways that matter:

- **Loading.** Cemu loads RPX files the console refuses. Error 160-2203 means the console's loader gave up before
  `main`. [`tools/rpx_check.py`](tools/rpx_check.py) runs the loader's checks that can be made off the console. Never
  strip relocations from the RPX: the console refuses even a tiny program without them.
- **Linking big code.** Past 32 MB of `.text`, `ld --relax` puts far-branch stubs into the C runtime's `.init`, which
  falls through into them and loops before `main`. The build's linker script keeps `.init`, `.fini` and the crt code
  together.
- **Traps.** Cemu parks a thread at any conditional trap instruction. The runtime builds with `-fno-if-conversion`, and
  `tools/win/patch_cafeglsl_traps.py` removes the five traps in CafeGLSL's Wii U library.
- **Compiler patterns.** Cemu mis-executes a compare saved across a call with `mfcr`/`mtcrf` when the branch after the
  call tests another condition bit. If Cemu and the console disagree, look for this first.
- **GPU timing.** A real GPU starts a draw when GX2 flushes, well after it's issued, while Cemu starts it at once.
  Vertices are read in place in guest memory, which the game rewrites as soon as it hears a list is done. So the
  backend waits for the GPU before it reports a list as done.
- **Uninitialised memory.** Cemu's memory starts zeroed and a console's doesn't. Render targets start from what the
  game left in guest memory, as on a 3DS. `-DGX2_JUNK=7` fills fresh allocations with noise, so Cemu shows what a
  console would.
- **Shader registers.** `GX2SetShaderMode` leaves 48 registers for vertex shaders and 200 for pixel shaders. A shader
  that needs more can't start on a real GPU, though Cemu doesn't mind. `build3ds.py` checks every shader pack.
- **Condition waits.** wut's timed condition wait sets an alarm, then sleeps, so an alarm firing in between loses its
  wake-up. Use `OSWaitEventWithTimeout`.
- **Files and Aroma.** Aroma's ContentRedirection hooks every file call in every title, and allocates from a heap all
  of Aroma shares under one spin lock. With the FTP plugin holding that lock, games froze. So file calls skip the
  hooks, and nothing about logging touches the SD card while a game runs. Keep Aroma's FTP plugin off while playing.
- **HOME Menu.** ProcUI keeps one callback list per core. The release callback has to run on the GPU's core, at a
  higher priority, or it races WHB's own callback while the game is still drawing.
- **Saves.** Writes only last once flushed (`SAVEFlushQuota`). The console won't seek past the end of a file, so files
  grow by writing zeros.
- **Installing.** Installing a build over an older build of the same title hangs at the banner, in the system's
  loader, before any game code runs. Uninstall the old build first.
- **Imports.** Every system library an RPX imports is one more thing the loader sets up. Libraries the game only
  sometimes needs (sockets, the microphone, saves) come from `OSDynLoad` instead.
- **3DS timing.** The game's GPU wait has a timeout (`nngxSetTimeout`) that a busy host can hit and a 3DS never does,
  so the runtime keeps it at zero. The DSP HLE never starts a frame before the game has finished with the last one.
- **PC frame numbers aren't real time.** Under load, the same screen arrives many frames later, so judge a PC run by
  its pictures.
