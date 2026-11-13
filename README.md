# 3Dii U

3Dii U turns Nintendo 3DS games you own into Wii U games. It recompiles the game's ARM code to C, compiles that for the
Wii U's PowerPC CPU, translates the 3DS GPU's work to the Wii U GPU, and packs the result as a title that WUP Installer GX2
installs on a modded Wii U.

## Built on

3Dii U exists because of these open-source projects. Their full notices and licence texts are in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

- **[3dsrecomp](https://github.com/nathsou/3dsrecomp)** by **Nathan Soufflet (nathsou)**, EUPL-1.2: the 3DS recompiler
  and runtime 3Dii U is built on. It lives in `third_party/3dsrecomp`, with this project's changes for the Wii U.
- **[devkitPro](https://devkitpro.org/)**: devkitPPC (GCC, Binutils, newlib), [wut](https://github.com/devkitPro/wut) and
  [wut-tools](https://github.com/devkitPro/wut-tools), the Wii U toolchain.
- **[CafeGLSL](https://github.com/Exzap/CafeGLSL)** by **Exzap**, on Mesa (MIT): the shader compiler for the Wii U GPU.
- **[FFmpeg](https://ffmpeg.org/)** (LGPL-2.1+): its AAC decoder, for games with AAC music.
- **[makerom](https://github.com/3DSGuy/Project_CTR)** from Project_CTR by **3DSGuy, applestash and Jakcron** (MIT), and
  **[pyctr](https://github.com/ihaveamac/pyctr)** by **Ian Burgwin (ihaveamac)** (MIT): reading and decrypting your dumps.
- **[Qt and PySide6](https://www.qt.io/)** (LGPL-3.0), **[Python](https://www.python.org/)**,
  **[PyCryptodome](https://www.pycryptodome.org/)**, **[cryptography](https://cryptography.io/)** and
  **[PyInstaller](https://pyinstaller.org/)**: the builder app.
- **[woomïnstaller](https://github.com/shinyquagsire23/woominstaller)** by **shinyquagsire23** (LGPL-2.1): the boot movie
  every title carries.

Its documentation draws on [3dbrew](https://www.3dbrew.org/) and [WiiUBrew](https://wiiubrew.org/). The WUP layout was
learned from Maschell's [NUSPacker](https://github.com/Maschell/NUSPacker), but none of NUSPacker's code is used.

## Free and open-source

3Dii U is **100% free and open-source**, under the [European Union Public Licence v1.2](LICENSE). Every build is free:
there are no paid builds, no paid beta tiers and no paywalled features. You may use, study, share and change it under the
EUPL-1.2. If you distribute a changed version, you must offer its source under the same licence.

**Status: beta, Windows only.** [`builder/compat.json`](builder/compat.json) lists the games that have been played on a
real Wii U. Any other game builds as "untested": it may not start, or it may stop partway.

## What you need to supply

3Dii U contains **no game data, no keys and no Nintendo files**. There's no game code, no assets and no generated C in
this repository or in the app. Everything comes from your own consoles and your own games, which you dump yourself:

| File | What it is | Where it comes from |
|---|---|---|
| Your game dumps (`.3ds`, `.cci`, `.cia`) | The 3DS games you own, plus their updates and DLC if you have them | Dumped from your own 3DS, e.g. with [GodMode9](https://3ds.hacks.guide/) |
| `otp.bin` | Your Wii U's one-time-programmable memory. The WUP is packed with the key in it | Your own Wii U's NAND backup ([Wii U Hacks Guide](https://wiiu.hacks.guide/)) |
| `boot9.bin` | Your 3DS's ARM9 boot ROM. Only needed for encrypted dumps | Dumped from your own 3DS with GodMode9 |
| `seeddb.bin` | Your 3DS's seed database. Only some digital games need it | Dumped from your own 3DS with GodMode9 |
| 3DS system files (`0004009b` and `000400db`) | Your 3DS's shared font, Mii data and bad-word list, which games load from the console itself | Your own 3DS: GodMode9, or Azahar's "Set Up System Files" with the Artic Setup Tool |

The builder checks each file before a build uses it. It never prints, stores or uploads what's in them, and it never
bundles them: the app refuses to package if one of these files is inside it. Don't ask for these files in the issue
tracker, and don't post them. Links to copyrighted games or keys aren't allowed anywhere in this project.

To play what you build, you need a Wii U running [Aroma](https://aroma.foryour.cafe/) **with its sigpatches module**
(`01_sigpatches.rpn`). Without that module, built games stop with error 199-9999. You also need
[WUP Installer GX2](https://github.com/Fangal-Airbag/wup-installer-gx2).

## Using it

Most people want **3Dii U Builder**, the desktop app in [`builder/`](builder/README.md). You pick a dump, and it builds
the game and puts it on your SD card. Releases of the standalone app need no Python and no toolchain. Its About button,
and `pipeline.py --credits`, list the projects it's built on.

## Working on it

- [ARCHITECTURE.md](ARCHITECTURE.md) explains how a dump becomes a WUP, and what each part of the code does.
- [CONTRIBUTING.md](CONTRIBUTING.md) covers the toolchain, building and the self-checks.

| Folder | What's in it |
|---|---|
| `third_party/3dsrecomp/` | nathsou's 3DS recompiler and runtime (EUPL-1.2), with this project's changes |
| `port/wiiu/` | The Wii U side: threads, GX2 graphics, shaders, audio, saves, input, the in-game menu |
| `port/pc/` | A headless Windows build of the same runtime. It finds code the recompiler missed and logs the shaders a game uses |
| `tools/` | The build (`tools/win/build3ds.py`), the title and WUP steps, the shader tools and developer tools |
| `builder/` | 3Dii U Builder: the window, the build pipeline, dump reading, decryption and mods |
| `phase0/` | The first feasibility benchmark. The WUP packer and the title templates still live here |

## Licence

Copyright © 2026 josiahg2502-glitch and the 3Dii U contributors.

Licensed under the [EUPL-1.2](LICENSE). nathsou's 3dsrecomp, in `third_party/3dsrecomp`, is under the same licence, and
its own copy is in [`third_party/3dsrecomp/LICENSE`](third_party/3dsrecomp/LICENSE). Everything else 3Dii U uses or ships,
and what each licence asks of a release, is in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Legal

3Dii U is an independent project. It is **not affiliated with, endorsed by or sponsored by Nintendo**. Nintendo 3DS, Wii U
and the names of Nintendo's games are trademarks of Nintendo, used here only to say what the software works with.

3Dii U is intended solely for games and system files you have dumped yourself from consoles and games you own, and for
homebrew. Don't use it with copies you don't own. Games it builds contain code from your dumps, so they're for your own
console only: don't share them. The software is provided as is, without warranty, as set out in the licence.
