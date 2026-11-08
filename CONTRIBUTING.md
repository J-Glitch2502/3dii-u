# Contributing

Thanks for helping. This page covers what to install, how to build, how to check a change, and what must never go into
the repository. [ARCHITECTURE.md](ARCHITECTURE.md) explains how the parts fit together.

## What never goes into the repository

`.gitignore` is a whitelist: only the source folders are tracked, and everything else at the top level stays out. Keep
it that way, and don't add exceptions for any of these:

| What | Where it lives on your PC | Why it stays out |
|---|---|---|
| Game files: code, RomFS, ExeFS, exheader, icons | `games/<name>/` | They're the game publisher's copyrighted work |
| Generated C | `games/<name>/gen_abi/`, `gen_safe/` | It's the game's code, translated |
| Shader logs with vertex programs (`vs_log.bin`, `dvlb.bin`) | `games/<name>/shader_logs/` | Vertex programs are the game's code. The builder ships fingerprints of them instead (`builder/shaderlogs.py`) |
| Screenshots and contact sheets | `games/<name>/shots/` | The game's imagery |
| 3DS system archives (shared font, Mii data) | `games/system_archives/` | They come from a 3DS |
| Keys: `boot9.bin`, `seeddb.bin`, `otp.bin` | anywhere outside the repository | They're your consoles' secrets |
| Build outputs: objects, RPXs, title folders, WUPs, the standalone app | `build*/`, `dist/`, `builder/dist/`, `builder_work/`, `phase0/build/` | Large, made again by a build, and they contain game data |
| Logs and console captures | `games/<name>/console/`, `*.log` | Game output, memory contents and your network's addresses |
| The console tools | `tools/console_*` | They hold one developer's console address and PC paths. They come back once those are arguments |
| The old WSL environment | `env/` | It holds one developer's PC paths, and the build no longer uses it |
| Third-party binaries | `third_party/cafeglsl*/`, `toolchain/aac_ppc/`, `builder/bin/makerom.exe` | Not this project's code. Get them as described below |

Machine-specific settings, such as paths and your console's address, belong in arguments or environment variables,
never in the code.

## What you need

Windows 10 or 11, 64-bit. A Linux route is planned.

| Tool | Where it goes | Version tested | Used for |
|---|---|---|---|
| Python | on `PATH` | 3.14 | everything scripted |
| `pip install PySide6-Essentials pyctr pycryptodome cryptography` | | PySide6 6.12, pyctr 0.7.6 | the builder, decrypting, packing WUPs |
| MSYS2, with `mingw-w64-x86_64-gcc` | `C:\msys64` | GCC 16.2 | the toolchain installer, and the C library for the PC builds |
| LLVM | `C:\Program Files\LLVM` | clang 23.1 | the shader generator and the PC trial |
| devkitPPC and wut | `C:\devkitPro`, from `tools/win/install_toolchain.sh` | see below | the Wii U build |
| CafeGLSL v0.2.0 for Windows | `third_party/cafeglsl/`, from `tools/win/build_cafeglsl.ps1` | v0.2.0 | compiling shaders ahead of time |
| CafeGLSL v1.0.0-rc1 for the Wii U | `third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc/` | v1.0.0-rc1 | compiling missing shaders on the console |
| FFmpeg's AAC decoder, built for the Wii U | `toolchain/aac_ppc/` | FFmpeg 4.4 | games with AAC music, and every untested game |
| makerom | `builder/bin/makerom.exe` | v0.19.0 | turning a game's CIA into a `.3ds` before decrypting |
| Cemu, portable | `build_win/cemu/` | | `tools/check_ports.py` |

**devkitPPC and wut.** Run this once from the repository root:

    C:\msys64\usr\bin\bash.exe --noprofile --norc tools/win/install_toolchain.sh devkitppc-gcc devkitppc-binutils devkitppc-newlib devkitppc-crtls wut wut-tools general-tools

The script uses MSYS2's pacman, but keeps its own settings, database and keys under `C:\devkitPro`, so MSYS2's setup
isn't changed. The versions tested are devkitppc-gcc 16.2.0-1, devkitppc-binutils 2.47-2,
devkitppc-newlib 4.6.0.20260123-4, devkitppc-crtls 2.1.0-1, wut 1.9.2-1, wut-tools 1.3.5-1 and general-tools 1.4.4-1.
devkitPro's server only keeps the newest packages, so you may get later ones. Builds made with those haven't been
tried on a console yet.

**CafeGLSL v0.2.0 for Windows.** `powershell -File tools/win/build_cafeglsl.ps1` builds it from source. It needs MSYS2
with `flex`, `bison` and `ninja`, and takes about half an hour. Upstream's own Windows build is a later version, and
its vertex shaders draw some scenes wrong, so don't use it in place of this one.

**CafeGLSL for the Wii U.** Unpack `cafeglsl-1.0.0-rc1-wiiu-ppc` from
[CafeGLSL's releases](https://github.com/Exzap/CafeGLSL/releases) into `third_party/cafeglsl_ppc/`, then run
`python tools/win/patch_cafeglsl_traps.py` once.

**FFmpeg's AAC decoder.** `tools/aac_build.sh` builds only the AAC decoder from an FFmpeg 4.4 source tree, for the PC
and for the Wii U. For now it runs only under Linux (WSL). A native Windows route is still to be written.

**makerom.** Download v0.19.0 for Windows from [Project_CTR's releases](https://github.com/3DSGuy/Project_CTR/releases)
into `builder/bin/`.

## Building a game

You need a dump of a game you own, and your Wii U's `otp.bin`. The easiest route is the builder's pipeline, which
works for any game:

    python builder/pipeline.py --game <dump> --otp <otp.bin> --out <folder>

It decrypts and unpacks the dump, finds the code, recompiles, compiles the shaders, and packs a WUP into `<folder>`.
`--update`, `--dlc` and `--mod` add those, and `--help` lists the rest. To run the builder's window from the source,
open `builder/3Dii U Builder.pyw`.

To work on a game step by step:

    python tools/extract_game.py <decrypted .3ds> games/<name>
    python tools/win/build3ds.py trial <name> 72000 --autoplay 3 --seed 3101
    python tools/find_entries.py games/<name> --write
    python tools/win/build3ds.py all <name>
    python tools/win/build3ds.py pack <name> <otp.bin>

The trial plays the game headless on the PC, records code the recompiler missed, and logs the shaders it meets. Its
screenshots land in `games/<name>/shots/`. A game that loads CRO modules also needs
`python tools/cro_entries.py games/<name>`, run with `RECOMP_CRO_BASE=0x1C000000` set. New games need an entry with a
title ID and a name in `tools/mario_title.py`. Moving that into one table of game profiles is planned.

`build3ds.py --help` lists every option. The ones you'll use most are `--jobs N`, `--force`, `--opt -O1`, and
`--out <folder>`, which keeps a test build apart.

To build the standalone app, which bundles Python, Qt, devkitPPC and the shader compilers, first run
`pip install pyinstaller pillow`. Then:

    python builder/make_app.py

It writes `builder/dist/3Dii U Builder/`, and a zip of it for testers.

## Checking a change

These need no game files and run in seconds:

    python builder/test_app.py
    python builder/test_shaderlogs.py
    python builder/selftest.py

`builder/selftest.py` builds a synthetic DLC in memory and runs it through the dump reader. The other two check the
app's compiled-cache reset and the shader-log fingerprint round trip.

This one needs your own dumps and Cemu, and checks the ports end to end:

    python tools/check_ports.py [game ... | all]

For each game, it builds the code, shaders and title, checks the RPX against what the console's loader wants, then
plays a scripted route in a private Cemu copy on a blank save and restarts on what that saved. A game fails on a crash,
missed code, a vertex shader that wasn't compiled, or a save problem. Cemu runs off-screen, so it doesn't take over
your desktop.

Run it on the games you have whenever you change the runtime, the recompiler or `port/wiiu`. Cemu has no real GPU and
forgives things a console doesn't (see the lessons at the end of ARCHITECTURE.md), so say in your pull request whether
you also tried a console.

## Trying it on a console

- Install with WUP Installer GX2. Uninstall any older build of the same game first: installing over it hangs at the
  banner.
- Turn Aroma's FTP plugin off while playing. With it on, games can freeze.
- The game talks to the PC over TCP port 4405: logs, thread dumps and debug commands (`port/wiiu/net_console.cpp`).
  The PC side is `tools/console_watch.py`, which isn't in the repository until it takes the console's address as an
  argument.
- Debug switches are empty files in the title's save folder (`/vol/save/common`), for example `profile` (a CPU
  profile) and `noglslcompile` (no shader compiling on the console).

## Sending changes

- Keep a pull request to one topic. Describe what it changes, why, and how you tested it.
- Keep behaviour the same unless the change is meant to alter it, and say when it is.
- Write comments that explain why the code is the way it is. The story of how a bug was found belongs in the commit
  message, not in the code.
- Changes under `third_party/3dsrecomp/` are changes to nathsou's EUPL-1.2 code and stay under that licence. Keep them
  separate from other changes, so they can be offered upstream.

## Pulling a newer 3dsrecomp

`third_party/3dsrecomp` is a git subtree: upstream's code arrived as one squashed commit, and this project's changes are
ordinary commits on top. To bring in a newer upstream:

    git subtree pull --prefix=third_party/3dsrecomp https://github.com/nathsou/3dsrecomp main --squash

Then build and check the ports, as for any runtime change. `git log -- third_party/3dsrecomp` lists this project's
changes.
