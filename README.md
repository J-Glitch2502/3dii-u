# 3Dii U

3Dii U turns Nintendo 3DS games you own into Wii U games. It recompiles the game's ARM code to C, compiles that for the
Wii U's PowerPC CPU, translates the 3DS GPU's work to the Wii U GPU, and packs the result as a title that WUP Installer GX2
installs on a modded Wii U.

It's not an emulator. The game's code runs natively, and a runtime stands in for the 3DS operating system, its services,
its GPU and its DSP. The TV shows the top screen, and the GamePad shows the bottom screen and takes touch.

**Status: beta. Windows standalone release; native Linux builds from source.** [`builder/compat.json`](builder/compat.json) lists the games that have been played on a
real Wii U. Any other game builds as "untested": it may not start, or it may stop partway.

**No game data is in this repository.** There's no game code, no assets, no keys and no generated C. Everything is built
from your own decrypted dumps, and the WUP is packed with the key from your own Wii U's `otp.bin`.

## Using it

Most people want **3Dii U Builder**, the desktop app in [`builder/`](builder/README.md). You pick a dump, and it builds
the game and puts it on your SD card. Releases of the standalone app need no Python and no toolchain.

## Working on it

- [ARCHITECTURE.md](ARCHITECTURE.md) explains how a dump becomes a WUP, and what each part of the code does.
- [CONTRIBUTING.md](CONTRIBUTING.md) covers the toolchain, building and the self-checks.

| Folder | What's in it |
|---|---|
| `third_party/3dsrecomp/` | nathsou's 3DS recompiler and runtime (EUPL-1.2), with this project's changes |
| `port/wiiu/` | The Wii U side: threads, GX2 graphics, shaders, audio, saves, input, the in-game menu |
| `port/pc/` | A headless Windows build of the same runtime. It finds code the recompiler missed and logs the shaders a game uses |
| `tools/` | The build (`tools/build3ds.py`), the title and WUP steps, the shader tools and developer tools |
| `builder/` | 3Dii U Builder: the window, the build pipeline, dump reading, decryption and mods |
| `phase0/` | The first feasibility benchmark. The WUP packer and the title templates still live here |

## Licence

nathsou/3dsrecomp, in `third_party/3dsrecomp`, is under the [European Union Public Licence v1.2](third_party/3dsrecomp/LICENSE).
The licence for the rest of the repository is still being chosen. Until it is, ask before reusing that code.

## Credits

- [nathsou/3dsrecomp](https://github.com/nathsou/3dsrecomp) (EUPL-1.2): the recompiler and runtime this project is built on.
- [Exzap's CafeGLSL](https://github.com/Exzap/CafeGLSL) (MIT): the GLSL compiler for the Wii U GPU, used ahead of time and on the console.
- [devkitPro](https://devkitpro.org/): devkitPPC and wut.
- [Project_CTR](https://github.com/3DSGuy/Project_CTR)'s makerom (MIT) and [pyctr](https://github.com/ihaveamac/pyctr) (MIT): reading and decrypting dumps.
- The WUP layout was learned from Maschell's NUSPacker (GPL-3.0). None of its code is used.
- The `app.xml`, `cos.xml` and `meta.xml` fields follow shinyquagsire23's homebrew titles. `bootMovie.h264` comes from woominstaller (LGPL-2.1).
- FFmpeg's AAC decoder (LGPL-2.1 or later), for games with AAC music.
