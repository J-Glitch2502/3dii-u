# Luigi's Mansion: Dark Moon on 3Dii U: where the work stands

A handover for whoever picks this up next (a person, or Claude Code on Senne's PC). Read this first, then carry on from
**Next step**. Work on the `darkmoon` branch; nothing goes to `main` until the game no longer freezes, and then as a pull
request with an explanation for Josiah.

Working with Senne: talk to him in Dutch. Make code changes yourself instead of telling him which lines to change. Never
share or commit game data, generated C, dumps or keys (CONTRIBUTING.md); text logs and a few screenshots are fine.

## The game and the files

- Dump: Luigi's Mansion: Dark Moon (USA), program ID `0004000000055F00`. On Senne's PC:
  `D:\Documents\Wii U Modding\darkmoon.3ds` (encrypted) and `darkmoon_dec.3ds` (decrypted).
- Keys: `D:\Documents\Wii U Modding\keys\` (`boot9.bin`, `seeddb.bin`). The Wii U `otp.bin` is not there yet.
- Unpacked game: `games/darkmoon/` (code.bin, exheader.bin, icon.bin, romfs.bin stripped to level 3).
- System archives: `games/system_archives/*.romfs` (not in git). Without the shared font (`0004009b00014002`) the game calls
  `svcBreak` at frame ~87. First made from the open-source replacements in Azahar's `externals/open_source_archives` (four
  files) with `third_party/3dsrecomp/tools/import_system_archives.py`; the folder now holds seven, the other regions' fonts
  included, so they were since made from a real NAND: Senne's 3DS backup
  (`D:\Documents\3DS SD CARD BACKUP\gm9\backups\260126_QEF10725872_sysnand_00.bin`) through
  `D:\Documents\Wii U Modding\extract_system_archives.py`, written for this. Ask him if it matters.
- `builder/compat.json` has Dark Moon as Untested, slug `darkmoon`, title ID `000500003D00055F` (the ID the Builder gives an
  untested game: `0x3D000000 | unique >> 8`).

## Commands

```
python tools/extract_game.py <darkmoon_dec.3ds> games/darkmoon
python tools/strip_romfs.py games/darkmoon      # not in CONTRIBUTING.md: without it gen.py's CRO scan crashes on the IVFC levels
python tools/win/build3ds.py trial darkmoon 3600 --autoplay 1      # PC trial, ~1 min of play; log in build_win/run_darkmoon/run.log
python tools/win/build3ds.py all darkmoon       # Wii U build: build_mario/darkmoon/title, load code/darkmoon.rpx in Cemu
```

Trial options used: `--autoplay 1` (A only; 2 = random buttons, ends up in the same place), `--run <name>` (own folder
`build_win/run_darkmoon_<name>`), `--speed 50`, `--log-lines N`, `--env NAME=VALUE`. Debug variables:

- `RECOMP_VERBOSE=1|2|3`: more logging. 3 logs every DSP buffer (`[dsp] src ... queue/play/empty`) and every change of the
  DSP region and frame counters. **Level 3 changes the timing enough that the freeze often doesn't happen.**
- `RECOMP_SVCLOG=0`: every SVC of thread 0.
- `RECOMP_MEMDUMP=900`: guest memory at that frame, `mem_900_<addr>.bin` in the run folder (added to the PC frontend here).
- `RECOMP_VOICEWATCH=9ae970,9ae9ec`: logs Dark Moon's two stream voices next to the DSP status of their sources whenever
  either changes (added here, see below). Barely changes the timing.

## Fixed (on this branch)

1. `runtime/loader.cpp`: shared page byte `0x1FF81086` bit 0 set. Dark Moon sleeps in a loop at `0x10b094` until it is.
2. `tools/recomp/gen.py` `is_trap`: `bkpt` with any condition counts as a deliberate trap. armcc's asserts end in
   `cmp; bkpthi`, which made whole functions be rejected as data (+52 functions, e.g. `0x3b4cc8`).
3. `tools/recomp/gen.py`: code pointers from `sub rX, pc, #imm` too, not only `add` (1348 in Dark Moon, +109 functions,
   e.g. callbacks `0x4270c8`, `0x427910`, and `0x4e0ed4`, which `0x4e1208` hands to the array constructor at `0x101978`).
   Checked on Nov 11: the analysis without any `functions.txt` finds all four, so a fresh Builder run needs nothing from
   an earlier run.
4. `port/pc/frontend_pc.cpp`: `RECOMP_MEMDUMP` in the headless trial.
5. `tools/make_game_title.py`: finds `icon.bin` where `extract_game.py` puts it (the Wii U build stopped at the title step).
6. **The freeze** (commit "DSP: give the game a status and an interrupt only after its own frame"), see below.
7. `services/misc.cpp`: `dlp:SRVR` `IsChild` answers no. As an unimplemented command it left the answer word out and the
   game read "yes": it started as a Download Play child in Thrill Tower ("Connection error", "Waiting for player") instead
   of at its title screen. Everything before Nov 11 ("the Thrill Tower screen") was that mode.

State on Nov 11 (PC trials): "Preparing to start game", title screen, file select, save, the intro (E. Gadd's lab, the Dark
Moon breaking, Luigi at home, the Pixelshifter), E. Gadd's bunker, then Gloomy Manor: Luigi walking outside the mansion,
the Dual Scream map on the bottom screen, the pause menu. trial28: 20 minutes (72000 frames) of `--autoplay 2` without a
freeze or a missing function, GPU lists rising all the way (57500).

## The freeze, solved (Nov 11)

Every trial used to end with thread 8 spinning at `0x8304e4`, walking a voice's wave buffer list that had become a loop.

How Dark Moon's sound works (addresses in its code.bin; it is nn::snd):

- Thread 8 (prio 45) runs the sound frame `0x1fd130`: `0x20bdfc` waits up to ~9.8 ms for the DSP interrupt (event at
  `0x9ac984`, through thread 2, prio 24), clears it and reads the status (`0x20bfec`), then submits (`0x20beb4`).
- Submit: writes the config and its counter `c` into region `c & 1` (its counter at `+0x131a`, region at `+0x131e`), `c++`,
  signals the DSP. Read: resyncs `c` from the counter of the region it submitted last, then reads the status of the 24
  sources from region `c & 1`, the one it will submit next, which is where the DSP writes after working on its submission.
- `0x25bb88`: a voice's status update; only with "current buffer changed" set it walks the list, marking the buffers
  before the current one done (3) and the current one playing (2). Voices `0x9ae970` / `0x9ae9ec` are the two channels of
  one stereo stream; the stream refills both channels from what the first one freed, so the two must see the same news.
- `0x83048c` appends a buffer (`next = 0`, then links it at the end): appending one still in the list makes it point at
  itself. That is the loop.

What went wrong (trials 19-25, logging put into the generated C of `0x25bb88`, `0x83048c`, `0x20bfa0` and `0x20c058`): our
DSP runs on its own host thread at a fixed rate. A frame could come while thread 8 was still at work; its interrupt then
stayed pending, so the game read the status right after submitting, before any frame had worked on that submission, from
a region two frames old (it took an old "stopped, no buffer" for news, marked playing buffers done and queued them again).
A frame between two submissions also rewrote the region the game was reading, so one channel got newer news than the other.

The fix (`audio/dsp_core.cpp`, `audio/source.cpp`, `services/dsp.cpp`): as on a 3DS, where every frame works on one the
game submitted just before, a frame gives its status and the interrupt only when the game has submitted since the last
one (or never has). Frames in between only play on; a buffer change waits for the next status the game is given. Sound
keeps its rate. This is in the shared runtime, so it changes every game that uses nn::snd: worth a check on the tested
games before the PR is merged (none of them are on Senne's PC).

Tried before and dropped: writing the status into both regions (froze earlier, reverted), holding frames until the
counter moves (stalled audio), holding frames while threads below priority 0x20 run (thread 8 is 45).

Debugging aids kept: `RECOMP_VOICEWATCH`, and `rt_debug_log` (C-callable, prefixes the guest thread) for logging put into
generated C by hand: edit `games/darkmoon/gen_safe/code_NNN.c` before the `/* addr: */` line, run a trial (the file is
recompiled), and put the file back afterwards (keep a copy; `games/` is not in git).

## Building it for the Wii U (Nov 11)

- Use the Builder **from this repository** (`builder/3Dii U Builder.pyw`, on the `darkmoon` branch). The packaged
  `3Dii U Builder 1.0 beta` in `D:\Documents\Wii U Modding\` carries its own copy of the engine without these fixes:
  with it Dark Moon still starts in Thrill Tower and freezes.
- Checked with `builder/pipeline.py` on the encrypted `darkmoon.3ds` (keys from `keys\`): reading, decrypting, unpacking,
  code (rpx 25.8 MB), shaders and the title all go through. Packing stopped only because the run used a blank OTP (there is
  no `otp.bin` on this PC yet); with Senne's own it packs.
- Shaders compiled ahead come from `games/darkmoon/shader_logs` (not in git). It held only the Thrill Tower menu's states
  (8 pixel, 4 vertex shaders); the logs of the Nov 11 runs are merged in (41 vertex records, 564 fragment states, mostly
  the 20-minute run), which gives 441 pixel and 24 vertex shaders. Parts of the game no PC run reached still compile on
  first use: the console writes those to `shader_misses.bin` in the game's save (`fs:/vol/save/common`, e.g. with SaveMii);
  that file put in `games/darkmoon/shader_logs` is compiled ahead by the next build.
- The shader generator's "108 variants could not be translated" were Dark Moon's vertex shaders: see below.

## On the console and in Cemu (Nov 11)

The first console build (Senne's Wii U, Aroma) booted to the file select, then froze after A with the music stuttering
("The game has frozen" screen: guest busy, all host threads waiting); it ran at 1.6 fps, and the TV showed one third of the
top screen, stretched. Fixed since, each its own commit:

- **Vertex shaders on the CPU** (1.6 fps): the GLSL translator (`runtime/gpu/shader.cpp`, `SpecGen`) gave up on every
  backward jump. Dark Moon's one vertex program jumps back on b14 (`0e5` to `002`, `141` to `124`, ...), so 108 variants
  ran on the console's CPU. A `JMPU` the variant's bools settle is now followed. 60 vertex shaders instead of 24; the 37
  left would loop forever on a 3DS too (b0 without b14). `tools/shadergen_main.cpp` now prints why a variant failed.
- **The TV picture**: Dark Moon draws its top screen in three passes at `0x18000000`, transfers each to a linear picture at
  `0x18300000`, moves columns with texture copies (an 8-column shift through a strip at `0x180b3000`) and copies the pieces
  into its LCD buffer in FCRAM (`0x2112e3f0`, stride 768 = 256 px RGB8, 416 rows from 16 rows before what the LCD shows).
  `screen_of` remembered `0x18300000` as a top LCD buffer from "Preparing to start game" forever; no path showed a screen
  that no display transfer brought; linear transfer outputs were written back tiled; `metal_guest_read` read back the
  bottom screen, which shares `0x18000000`. Now (`port/wiiu/gx2_backend.cpp`): buffers no LCD has shown for 240
  transfers are forgotten; texture copies of whole rows out of a linear surface are drawn on the GPU into a linear surface
  at the destination (`linear_copy_on_gpu`), the LCD buffer included; at VBlank a top screen no transfer brought is shown
  from that surface (or from guest memory). No read backs: on the console those were ~34 ms of waiting a frame.
- **Vertex data**: copied and converted at every draw, 36-115 KB a draw, 108 us a draw on the console (~43 ms a frame).
  Now converted once and kept until the game flushes that memory (`vtx_cached`). In Cemu 16-50 us a draw became 1.8-3.6.
- Not looked into yet: the freeze after A at the file select happened at 1.6 fps and hasn't been seen since; the "108
  variants" line is the vertex shader issue above. 13 fragment misses in a 7-minute Cemu run.

Cemu at 60 fps (title screen, intro, E. Gadd's bunker, 7 minutes) says the picture is right, not how fast a console is:
its CPU is far faster. The `[prof]` lines (microseconds per draw for each step) are what to compare: a step that falls
by a factor in Cemu falls about as much on a console.

### Testing without the SD card

- **Cemu**: a private copy in `build_win/cemu` (Senne's Cemu 2.6 exe as `CemuMario.exe`, its own `portable` folder with his
  settings but its own `mlc01`; `mlc01/sys` copied from his Wii U Menu dump, 740 MB, so system files and fonts are there).
  `tools/win/cemu_run.ps1` runs it on a desktop of its own. Before each run delete `portable/shaderCache`, put back
  `portable/settings.template.xml` as `settings.xml`, and delete `portable/title_list_cache.xml` and
  `portable/mlc01/usr/save/system/pdm`: a Cemu stopped while writing those crashes at the next start (`0xc0000409`).
  Cemu reads nothing the GPU drew back into memory, so anything that depends on read backs is black there.
- **The game's own log, live**: the build listens on port 4405 (`port/wiiu/net_console.cpp`), in Cemu (`127.0.0.1`) and
  on the console (`192.168.0.149` on Senne's network). `tools/console_watch.py <ip> [commands]` (local: `.gitignore`
  keeps `tools/console_*` out) saves the log, and sends commands: `st`, `ps`, `misses` (saves `shader_misses.bin`),
  `shot`, `surf <hex address>` (a picture of a target), `set trace <n>` (the next n draws and copies in the log), and
  `in <buttons> <frames>` / `in WAIT <frames>` (input). Only one PC can be connected at a time.
- The scripts used: `console_runs/cemu_cycle.sh <name> [seconds]` (build with the Builder, run in Cemu, read the log;
  `NOBUILD=1` to skip the build) and `console_runs/cemu_play.sh <name> <seconds> <shots>` (A every second or so).
  `console_runs/` is not in git.

### Later on Nov 11: playable on the console, ~27-28 fps

State with the build of 21:56 (Senne's Wii U, FTPiiU off): plays through the intro into the mansion, buttons and HOME work,
~27-28 fps in the mansion (GPU thread 97% busy), far fewer hitches in new rooms. The intro still stutters at moments.

- **FTPiiU must stay off.** With the plugin on, its busy threads (same priority 16, core 1) kept a guest thread ready but never
  run, and the game froze with its sound looping. Raising the guest threads to 15 made the HOME button and the GamePad stop
  working (they starved the system's own threads), so that was reverted. Not solved in code yet.
- **Memory fills on the GPU** (`gx2_memory_fill`): targets wholly inside a fill are filled too; before, Dark Moon's fills of
  `0x18000000` were done in guest memory by the CPU, a quarter of the GPU thread's time.
- **Out of memory after a long session** ("out of memory for a 356352 byte texture"): shaders.gsh is now read per shader as
  needed instead of whole (`gfd_scan`/`gfd_one`), and a full heap first makes room (`relieve_memory`: the vertex cache, then
  the least recently used textures). The `[prof]` lines say what the renderer holds. 697 MB in use after ~7 minutes.
- **Shaders from an Azahar cache**: `tools/azahar_to_log.py` turns a shared Azahar cache (`<id>_fs.vkch`) into fragment
  states; a shared Dark Moon cache (`D:\Documents\Wii U Modding\lm2_azahar_shaders\`, from Senne) gave 17988, now in
  `games/darkmoon/shader_logs/azahar_fs.bin`. 414 of 433 of our own logged states are among them. The console now misses 6.
- **The PC version in a window**: `python tools/win/play_pc.py darkmoon` (see its docstring for the keys) plays the game on
  the PC and adds the shaders it meets to `games/darkmoon/shader_logs`.
- Switches over the network: `set vtxcache 0|1`, `set fillinside 0|1`, `set lineargpu 0|1`. The bottom screen sometimes
  shows only its moving parts (map marker, hearts); not tried with the switches yet.
- **Don't send the `fs` command**: sent at connect, the game's network console stopped answering (port 4405 closed).

Where the time goes now (console, mansion, `[prof]`): about 180 draws a frame at ~82 us each (~15 ms): **tex 44 us** (of
which hash 2, upload 5: the rest not yet explained), attr 10, range 9, bind 5; plus `sync` 6.5 ms a flip and 1.5 draws a
flip through a geometry shader on the CPU (500-1500 vertices). Two 256x256 RGBA8 textures, `0x183a8000` and `0x183e8000`,
are uploaded again every frame as definitely written, with no transfer or copy to them in the trace: probably a memory fill
the game clears them with each frame, done by the CPU since they're textures, not targets.

**The fills, traced** (Cemu, the intro, `[gxt] fill` lines): per frame two 416 KB clears of the targets at `0x18000000` and
`0x18300000`, two 256 KB ones of the targets at `0x18368000`/`0x183a8000`, a 300 KB clear of the bottom screen at
`0x18000000` (inside the top screen's target, so before: done in guest memory as well), and 256 KB of white at `0x183e8000`,
a texture and no target (so before: the CPU, and an upload). Now (`gx2_memory_fill`): a fill whose exact target matched
ignores other targets it overlaps in part unless a texture lies there, and a fill of a whole texture with the value its last
fill left, nothing else having written it since, is skipped. In the trace every fill is on the GPU or skipped; on the console
`memory_fill` was 11% of the GPU thread in the intro. Not measured on the console yet.

## Nov 12-13: main merged in, and a crash at start (open)

`darkmoon` now has Josiah's main of Nov 12 merged in (491ff97, 76590ad, c3c0466; not pushed: GitHub refuses Senne's account
since the repository moved to J-Glitch2502/3dii-u, 403). Conflicts: the DSP (main's "seen" with our "fresh": fresh only if seen),
main's whole-array vertex copies through our vertex cache, the shader cache (per set, with main's header), set_state/gfx_pump.
Builds no longer measure unless asked: `tools/win/builder_measuring.bat` (THREEDII_MEASURE=console) packs a measuring build for
the console; the Cemu scripts set THREEDII_MEASURE=1 (out_measure). A Cemu-only measuring build skips the otp check.

**The merged build crashes in Cemu at frame ~330, every time**, before any picture: `FATAL: undefined instruction 81200a76 at
003b4cf4 (lr=003fd8e0)`. That's the game's own `bkpt` in a bump allocator (f_003b4cc8: out of room), asked for count*4 bytes by
an array copy (f_003fd8a0), so a count is garbage. Not the cause (each tried alone, crash stayed): main's file reads letting
other threads run (run_blocked), the DSP merge (pre-merge DSP back), and probably not the unpacked shared font (that run died in
Cemu itself, inconclusive). Being tried when the PC went off: the game code at -O2 instead of main's new -Os (build3ds.py
GAME_OPT). Next: that test; then bisect main's commits (y2r, gpu.cpp register runs, kernel lock_soon, frontend core changes).
**Don't build the merged darkmoon for the console until this is found**; 34ad909 is the last good one.

## Next step

1. **The console's GPU is now what holds it back** (profile of Nov 11, late, mansion, build 98237c6): tex 2.5 us a draw (was 44),
   sync 0, memory_fill 1%; the GPU thread waits 9 ms a flip for the GPU itself (`wait`), still ~27-31 fps. The top screen's
   targets are 768x1536 (and 738x1476 for the game's own offscreen passes, five or six of them), at 720p, where the size never
   changes. New switches (not yet tried on the console; `set` over the network): `set toplevel 1` holds the lower size,
   `set toplevel 0` full, `set toplevel 2` follows the GPU; `set lowscale <percent>` the lower size (default 67);
   `set dynres 1` lets it follow the GPU at 720p too. Next: rebuild, in the mansion try `toplevel 1` with lowscale 67, 80,
   90 and read the fps (`st`); if one holds 30, make that the default for Dark Moon (or `dynres` on by default).
   Tried in Cemu (`set toplevel 1`): the game goes on, the picture is right but for a thin blue line along the top edge
   (the LCD composition from row 16, `linear_copy_on_gpu`, probably still assumes full size): to fix before it's a default.
2. The `sync` (6.5 ms a flip: which read backs are left) and the geometry-shader draws on the CPU.
3. The intro's stutters: a console profile (`test profile`, then `prof`) during the intro with this build.
4. Something for FTPiiU that doesn't starve the system (a guest thread that yields, say), or a line in the README.
5. PR to `main` with an explanation for Josiah: the DSP change and the Wii U renderer changes touch every game, so the
   tested games want a run before it's merged (`tools/check_ports.py` runs them in Cemu, given their dumps).

## Other loose ends

- The PC trial's lazy compile of a missed function crashed after loading its DLL (exit `0xC0000005`), so a run only
  records one missed function at a time. Not looked into.
- `tools/setup.py` crashes instead of stopping when MSYS2 is missing (`install_mingw` runs `C:/msys64/usr/bin/bash.exe`).
- The Wii U falls back to the CPU for vertex shaders it hasn't compiled yet: put the console's `shader_misses.bin` (the
  `misses` command above) in `games/darkmoon/shader_logs` and rebuild.
