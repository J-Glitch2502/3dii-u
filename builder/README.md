# 3Dii U Builder

Turns your own 3DS game dumps into games for your Wii U. Pick a game, say whether to add its update, DLC and a mod, choose WUP install or Aroma Homebrew output, and press **Build It!**.

UI and art design by syntcity.

No game data and no keys come with the builder. Everything it builds from is yours: your dumps, your 3DS's keys if the dumps are encrypted, and your Wii U's `otp.bin` for WUP output only.

## Using it

1. Open **`3Dii U Builder.exe`** in `dist/3Dii U Builder`. That folder is the whole app: copy it anywhere, as it needs no Python and no toolchain. To run the builder from the source instead, double-click `3Dii U Builder.pyw`.
2. Click **Choose a game** (the big **+** tile, or Ctrl+O) and pick a dump. It can be a `.3ds`, `.cci`, `.cia`, `.cxi` or `.app` file, encrypted or already decrypted. The tile shows the game's art, name, maker, and compatibility badge.
3. The builder then asks about the **update** and the **DLC**, right in its window:
   - If they're in the same folder as the game, it names them and asks whether to use them.
   - If not, it asks whether you have them, and lets you pick the files.
   - You can change or remove either later, under **Extras**.
   - Then it asks about a **mod** (see [Mods](#mods)): pick its `.zip` or its folder. **Extras** has it too.
4. **Your Files**: the lights turn cyan when the builder has what it needs.
   - **3DS keys** are only needed for encrypted dumps. The builder decrypts them the way `cia_to_decrypted_3ds.py` does: makerom turns a game's CIA into a `.3ds`, then pyctr decrypts it, with `boot9.bin` and `seeddb.bin` from your own 3DS. It looks for them in `.3ds` in your user folder (`C:\Users\<you>\.3ds`), pyctr's own place; choose another folder if yours are elsewhere. GodMode9 can dump both from a 3DS.
   - **Wii U otp.bin** comes from your own console's NAND backup. The WUP is packed with the key inside it; the builder never shows or stores that key.
5. **Output**: choose **WUP install** or **Homebrew (.wuhb)**. Homebrew supports full games and small executable/shader exports; see [Homebrew output and small exports](#homebrew-output-and-small-exports). **Put it on**: your SD card (cards set up for a Wii U come first), or a folder.
6. **Build It!** It takes a while, longer for bigger games: the game's code is recompiled for the Wii U and its shaders are compiled ahead of time. You see each stage tick off. Your PC stays usable meanwhile, **Cancel** stops it cleanly, and **Details** shows everything the build prints.
   - To build several games, configure each one and press **Add to queue**. **Run queue** builds them in order using the options and destination each entry had when you added it. A failed build leaves a result in the list and the next one starts. **Stop queue** cancels the active build and leaves the waiting entries ready to run again. The list lasts until you close the Builder.
7. Put the card in your Wii U. Launch a complete Homebrew WUHB from the Aroma Menu, or install WUP output with **WUP Installer GX2**. If you installed an older build of the same game, delete that one first (System Settings, Data Management), then install the new one.

**Games** in the bar on the left holds the games you picked before and the **Compatibility List**. **Help** covers what you need, app updates, and the light and dark themes.

**Game language** defaults to the computer's display language, with English as the fallback. Choose a language to override it for the selected title. The game must already include that language.

WUP saves use the installed title's NAND/USB save storage and appear in Data Management. Aroma Homebrew saves use the loader's SD-backed save mapping. Save states follow the same backend; small executable/shader imports preserve the original WUHB's save identity.

### Native WUP updates

Choose **WUP update** under Package, then **Binary only** or **Binary + shaders** (the default).
Install the resulting update on the same NAND/USB device as the matching base game. Keep the base game and its saves installed.
The package uses an update title ID (`0005000E`); the Menu keeps the original game icon.
New WUP base titles encode the 3DS game's unique number as `000500001Dxxxx00`.
The `1D` prefix supports Nintendo's native update lookup, and the final `00` lets
Data Management group software and saves under the same entry. The full unique
number is retained; adjacent games receive different IDs. Inputs outside this
reserved range are rejected rather than truncated.
Older `3D` titles and earlier `1D` titles with a nonzero final byte need a one-time
separate base rebuild. Updates cannot change an installed base title's identity.
The builder uses a distinct `_NativeWUP` output folder and leaves existing installs
and saves intact. Move saves using a save manager when changing title IDs; the
builder does not silently migrate console data. Homebrew identity is unchanged.

Every full WUP build now writes a matching `.base.json` beside its installer
folder and keeps a copy in the Builder work folder. In **WUP update → Matching
base**, choose that file, or use the last full base from the same work folder.
Both update modes require this manifest. Old size-only inventories are rejected;
make a full base with this Builder once to register its assets. Keep the manifest
when moving a base build to another PC.

The PC checks the selected 3DS update/DLC versions and the resulting game assets,
including applied mods, against that base. Changes require a full base rebuild.
Binary-only updates also check the shader code contract, then link the ORIGINAL
base shaders without compiling/copying them. If graphics code changed, choose
**Binary + shaders**. A binary-only update does not retain shaders from an earlier
shader update: native update packages replace each other.

Language defaults, menu enabled/disabled and menu shortcuts are copied as update
content, with explicit defaults so old base flags do not win. Saved per-user
language preferences still take precedence over the build's default language.
A tiny startup marker check rejects an update paired with a different installed
base before loading game content. It does not scan or checksum ROM assets on Wii U;
asset comparisons run on the PC. Base reinstallations keep the same corrected
native title ID, so this change does not require another ID migration.

The preceding binary-only path passed repeated real Wii U installation tests.
The new manifest/startup guard and binary + shader path still need a hardware
comparison; host tests cover matching/mismatched assets and native update layout.

Native update version stays fixed at **5**. Date/time labels identify the build folder and are recorded in the package metadata;
there is no build counter, clock-derived version or rollover. Changed contents receive new content IDs, so a manually installed
package can replace an earlier version-5 package without depending on a higher version number.
On November 27, 2026, Urban Champion was tested on a real Wii U: a binary-only version-5 update launched, then a second version-5
update replaced its executable and displayed a temporary menu label. The runtime log and installed metadata confirmed the new
binary with version 5 unchanged. This validates that manual reinstall path on the tested Wii U; it does not establish automatic
update discovery or behavior across every installer. The temporary menu text was excluded from production source.

The corrected `1Dxxxx00` Urban Champion base was also installed and launched on real hardware.
Existing user saves were restored while keeping Nintendo's new save metadata intact. The user confirmed
that software, user saves and shared saves now appear together in Data Management. Existing titles are
not renamed or migrated by the PC Builder; retain the old save until a manual migration is verified.

### Rebuild just the executable for hardware testing

Keep the prepared files from a normal build with the pipeline's `--keep` flag. Use the same Builder work directory and devkitPro environment for subsequent builds:

```sh
python tools/win/build3ds.py code g_asre --prepared-game WORK/builder_work/g_asre --work-dir WORK/build_win_builder --out RPX_OUTPUT
```

This reuses the extracted game and compiled objects. Use a full build when changing the ROM, game assets, shader generator, or shader pack.

For Cemu, close the running title and copy the new RPX into the `code/` directory of a test copy of the unpacked title. Launch that RPX directly in Cemu. The existing `content/` and `meta/` folders supply the assets and title metadata, so this test skips WUP packaging.

Install the matching base WUP once on the Wii U. Close the game, enable FTPiiU and its system-file access in debug Aroma, then run:

```sh
python3 tools/ftp_dev_rpx.py WII_U_IP WORK/builder_work/g_asre/out/title RPX_OUTPUT/g_asre.rpx --volume storage_usb
```

Use `storage_mlc` if the test title is installed to internal storage. The tool checks the installed title ID and shader files, uploads and verifies the new RPX, and keeps the previous RPX as a uniquely named backup in the same code directory. Launch the title again from the Wii U Menu to run the new executable.

### Update a homebrew WUHB without replacing its assets

With our generic development build of RPXLoadingModule installed in Aroma, any
WUHB under `sd:/wiiu/apps/` can have a companion `<full-name>.wuhb.dev` directory.
No game-specific loader entry is needed. Reload Aroma once after installing the
module; later updates are selected automatically at the next application launch.

```sh
python3 tools/ftp_homebrew_dev.py WII_U_IP \
  'wiiu/apps/Kirby Triple Deluxe.wuhb' RPX_OUTPUT/g_bale.rpx \
  --content CHANGED_CONTENT --release build-20261118-1
```

`CHANGED_CONTENT` holds only changed content files, such as `shaders.gsh` and
`shaders.idx` from the same build. Omit `--content` for an RPX-only update. Both
shader files must be supplied together. The publisher writes an immutable release,
verifies every upload, and switches the small `active.txt` marker last. The module
loads that release's `root.rpx` and merges its optional `content/` over the WUHB.
The original bundle supplies other assets and keeps the same save identity.
Nothing in this workflow writes to the installed USB title or to save folders.
The original installed-title FTP updater above remains a separate alternative.

## Which games work

Open **Games → Compatibility List** for Wii U and Cemu playtests. The records in [`compat.json`](compat.json) are keyed by 3DS program ID so different editions can have separate results. Read the notes for the tested settings and known issues. The top-level profiles also preserve the Wii U title IDs of existing builds.

**Updates and DLC** are read the way a 3DS reads them:
- An update brings its own code and its own RomFS, which the game lays over the original files itself.
- DLC brings one RomFS per content, which the game finds through the same calls it makes on a 3DS.
- Check the game's playtest note for any update or DLC used in testing.

## Mods

Mods made for Luma3DS on a 3DS work as they come: a `.zip` or a folder with `luma/titles/<title id>/` inside (or just that folder's contents). The builder puts the mod into the game before recompiling it, so a mod's code runs as recompiled code, like the game's own.
- `romfs/`: files that replace the game's files or add new ones, as Luma's LayeredFS does. Names match whatever their case, as they do on the SD card. A file the game's update also has goes into the update's files, which the game reads first.
- `code.ips` or `code.bps`: a patch to the game's code. A BPS patch made for another version of the code (another region, or with or without the update) is refused with that reason.
- `code.bin`: the whole code, replaced. `exheader.bin`: the extended header, for a mod that changes the code's size.
- A mod that changes code, or the CRO modules some games keep most of their code in, has the code finders look again. For a tested game, they add to what its tested builds found.
- The title folder must match the game: a mod for another region or edition is refused, naming the title IDs it's for. With an update installed, Luma mods use the game's own title ID, and so does the builder.
- Plugins (`luma/plugins/<id>/*.3gx`, which is how CTGP-7 runs) aren't mods of this kind. They change the game in memory while it runs, and recompiled code never reads its ARM code again, so they can't be built in.
- With a mod, every game counts as untested.

`python builder/pipeline.py --mod <zip or folder>` takes several mods, applied in order, each over the last.

## What it needs on the PC

Windows 10 or 11 for now (Linux is planned):
- **The standalone app needs nothing else.**
  - It carries its own Python, Qt, pyctr and makerom.
  - Its `engine` folder holds the rest: the recompiler, the runtime's sources, devkitPro's PowerPC toolchain with wut, the CafeGLSL shader compiler, and a ready-built shader generator.
  - It builds in `%LOCALAPPDATA%\3Dii U Builder`, which keeps the compiled runtime for the next game.
  - Beside it, `objcache` keeps up to 2 GB of compiled game code by what went into it (`tools/win/objcache.py`). A game built again by a newer app, or with its update, then compiles only the files whose code changed.
- A game's art comes from the web when the game is picked (`covers.py`); the user's own pictures, one for the start-up screens and one for the icon (the Art pill's menu), or removing them override it. The same menu takes a GamePad bezel, a picture the build scales to 854x480 (`make_game_title.bezel_pixels`), which the GamePad shows beside the screens in place of black. It also takes a GamePad start-up picture of the game's own (`--pad-art`, shown as it is) and a start-up sound for that game alone (`--boot-sound`, a .btsnd in the work folder's `boot_sounds`).
  - With the user's SteamGridDB key (Help page, "Art and sound"; kept in `settings.json`, never in the repository): a wide banner for the start-up screens, found by the game's name, or by its other regions' names where SteamGridDB doesn't know this one (Luigi's Mansion 2 is Dark Moon there). Automatic artwork does not replace the Wii U Menu icon: the default is the game's own SMDH with Walegy's Virtual Console-style frame ([#45](https://github.com/J-Glitch2502/3dii-u/issues/45)); an explicitly chosen icon overrides it.
  - Without a key, or where SteamGridDB has nothing: the cover from GameTDB, by the product code's four letters.
  - GameTDB's list of 3DS games (`3dstdb.zip`), fetched once, gives the year a game came out, for the Virtual Console's start-up screens (a build goes on without a year if it can't be had), and its names in other regions.
  - The Virtual Console's start-up screens show the game's title screen, as the real Virtual Console does: libretro's (thumbnails.libretro.com), found in its list of 3DS title screens (fetched once) by the game's names, where it was sold first; the top screen of it. Without one, the art above. Behind them is a 3DS (`tools/art/vc_console_3ds.png`, from the user's photo, faded).
- The start-up sound is the user's own too (Help page, "Art and sound", a WAV file): `tools/make_boot_sound.py` turns it into `boot_sound.btsnd` in the work folder, and every title built after carries it, unless the game has a sound of its own.
- The 3DS system archives (its shared font and Mii data) don't come with it: they're your own 3DS's files.
  - The builder copies them from Azahar's or Citra's user folder, or from any folder holding a 3DS NAND up to three levels down in Downloads, Desktop or Documents, whatever it's called. It keeps them in its work folder.
  - Off a 3DS, GodMode9 copies `title/0004009b` (font, Mii data) and `title/000400db` (the bad-word list) out of `[1:] SYSNAND CTRNAND`; the user picks either folder (or the one holding them) with "Folder". A build stops if the game's code names an archive the files lack (`dumps.archives_named`): Tomodachi Life stops at start without the word list. Azahar gets them with File > Set Up System Files and the Artic Setup Tool.
  - The NAND's contents may be NCCH (`<n>.app`, encrypted as on the 3DS or not) or RomFS images already taken out (`<n>.app.romfs`). A folder of `<title id>.romfs` files works too.
  - Each row under "Your Files" has an "Info" button that explains its file to testers (`gui.HELP`).
  - Without the Mii data and the shared font, the app and `pipeline.py` won't build. A Miitopia built without them stopped on the console with `[err:f] result c8804478`.
  - To scan or generate standard 3DS Mii QR codes on Wii U, put the `slot0x31KeyN` line from your own 3DS key file in `3dii-u/mii_qr.key` on the Wii U SD card before launching the game. The file can also contain just the 32 hexadecimal digits. It stays on your card outside the install package.
  - If a build ever lacks one, the runtime's error screen names the missing archive.
- Free disk space of about three times the game's size while it builds. Work files are deleted at the end.
- From the source: Python 3.10 or newer, `pip install PySide6-Essentials pyctr pycryptodome` once, and the build tools `tools/win/build3ds.py` uses. Install each once:
  - devkitPro's PowerPC toolchain and wut, from `tools/win/install_toolchain.sh`, into `C:\devkitPro`.
  - LLVM, in `C:\Program Files\LLVM`.
  - MSYS2's `mingw-w64-x86_64-gcc`, in `C:\msys64`.
  - The CafeGLSL shader compiler, from `tools/win/build_cafeglsl.ps1`.

## How it works

- The window is Qt Quick: `ui/Main.qml` draws it (Wii U Menu glass laid out on a 1280x720 surface scaled to the window, light and dark, the in-window questions, the update card), `ui/Simple.qml` is the plainer window the About card can switch to and back from (the same `app`, one window at a time), and `gui.py` holds what it shows and does what its buttons ask. A choice that asks several things is a generator that yields each question.
- `app.py` is the one program: the window, the build when run with `--pipeline`, and `python.exe`'s part for the repository's tool scripts the build runs. In the standalone app, the build's compilers start without console windows.
- `make_app.py` builds the standalone app with PyInstaller (`python builder/make_app.py`, on a PC with the build tools above).
  - For Windows packages, install `python -m pip install -r builder/requirements-windows.txt`. This pins Qt 6.11.3: Qt 6.12 aborts during display-profile discovery under Steam Proton 11 because its `WcsCreateIccProfile` implementation is still a stub. The same current Builder GUI runs with the pinned runtime; no native Windows DLL replacement is needed.
  - On Linux, place `builder/proton.sh` beside the packaged executable and run it with Bash. It uses Steam Proton 11 and a separate `~/.local/share/3dii-u-builder/proton` prefix, and aliases Proton's own ICU implementation to the `icuuc.dll` name Qt imports. Set `THREEDII_PROTON` for a different Steam library. `test_proton_gui.py` checks real window rendering, navigation and output choices; pass its path to the executable to run it against the frozen application.
  - It leaves out the parts of Qt the window never loads.
  - It puts the engine beside the app, in this repository's own layout. That includes what runs of the games learned: where their code starts, and the render states they met.
  - None of the games' code goes in. A run's shader log keeps each vertex record's whole shader memory, which is the game's own code. So `shaderlogs.py` ships each record as a fingerprint of the program of the game's DVLB it ran, beside the log in a `.vsf` file, and a build finds the program in the user's own dump again (dvlb.bin). Checked on the nine games: the shader generator makes the same shaders from these as from the full logs, except that it declares fewer unused inputs.
  - A capture can also ship as a recipe (`.vsr`) made of offsets in the game's own files or in programs discovered in those files. The scanner finds shader binaries in LZ11-compressed GARC entries as well as plain files. For a capture assembled from several programs, the recipe records the source of every word and the build checks the fingerprint of the completed memory.
    - The Mario & Luigi engine keeps a shader as plain words in its `code.bin` and loads a romfs program after it. That's 115 records the app used to leave out, and all of them come back now.
    - With a current extracted build, `python builder/shaderlogs.py --recipes <extracted game folder> games/g_<code>/shader_logs` writes the recipes for those captures. It uses the build's own `shader_logs/dvlb.bin` to find combinations of discovered programs.
  - A game `compat.json` calls Complete also ships `confirmed.bin`: every fragment state the build it was confirmed with compiled ahead. A build compiles that list first, whatever its other logs hold (only a `--cold` build, which takes nothing a run learned, goes without it). Without it a later app could drop some of them: the states a build guesses beyond the logs are picked up to a limit, in an order that one more logged state changes (56 guesses out and 12 in for NSMB2, from a single new record). After a game is confirmed finished on a console, `python builder/shaderlogs.py --confirm <that build's shader_logs> games/<slug>/shader_logs` writes the list (a Builder build keeps its folder with `--keep`), and `make_app.py` won't make an app with a Complete game that has none.
  - A record still left out is drawn the CPU way on the console. Most of those are another game's, from a console log that collects every game: NSMB2's 17, for one, which NSMB2 never draws.
  - For an untested game built from the source, the build leaves its `dvlb.bin` where its runs' logs go (`games/g_<code>/shader_logs`), so the next app can fingerprint them. The console tools (`tools/console_*`), with this PC's paths and its console's address, stay out.
  - It ends with `dist/3Dii U Builder <version> beta.zip` for testers: the app folder with `Read me first.txt` (`app_readme.txt`) and `Third-party notices.txt` (`app_notices.txt`). A new build replaces the old zip.
  - It writes how many files the app has to `_internal/files.txt`. Before Qt loads, `app.py` counts them again: a zip unpacked only in part (Windows' Extract All stops at a path too long for it, or an antivirus takes a file) gets a message to unzip again into a short folder, instead of a Python traceback. Only the Basic style of Qt Quick Controls ships, which keeps the deepest path in the zip at 121 characters.
  - The app finds that engine, points `tools/win/build3ds.py` at its devkitPro and shader generator, and builds in the user's folder (`THREEDII_ROOT`, `THREEDII_DEVKITPRO`, `THREEDII_SHADERGEN`, `THREEDII_WORK`). Run from the source, none of these are set and everything stays in the repository.
  - devkitPPC's compiler can't open a file whose path passes 260 characters, and it reaches its headers by long relative paths. So the app builds through a directory junction, `%LOCALAPPDATA%\3Dii U Builder\engine`, pointing at its own engine folder. It works wherever the app is kept, and is made again if the app moves.
  - The work folder keeps the compiled runtime for the next game. A newer app's sources can be older than what an earlier app compiled there, so when the engine changes (its `stamp.txt`), the compiled runtime starts over.
  - An error in a build is printed to the log and ends it; the standalone app never stops in a message box of its own.
- `decrypt.py` makes encrypted dumps plain first, with `cia_to_decrypted_3ds.py`, the converter this builder was designed around:
  - A game's CIA: `makerom -ciatocci`, then pyctr decrypts every partition of the `.3ds` it makes, in place.
  - An encrypted `.3ds`: the same pyctr step, on a copy, so your dump is never changed.
  - An update's CIA: pyctr alone. makerom won't make a card image from an update ("This CIA cannot be converted to CCI").
  - DLC: pyctr alone, content by content.
  - Seeds for games from firmware 9.6 on come from `seeddb.bin`.
- `dumps.py` reads the plain dump and checks every part against the hashes in its own header. A bad decryption or a damaged dump stops the build there, before anything is built. Dumps that were decrypted by another tool but still carry their "encrypted" flags are recognised by those same hashes.
- `pipeline.py` does the rest, in folders of its own (`builder_work/`, `build_win_builder/`):
  1. Unpacks the game, its update and its DLC, then puts the mods in (`mods.py`: IPS and BPS patches, and a RomFS writer that rebuilds each of the nine tested games' RomFS byte for byte the same when nothing changes).
  2. Recompiles the code with `tools/win/build3ds.py` and compiles the shaders.
  3. Assembles the title with `tools/make_game_title.py`.
  4. Packs it with `tools/pack_game_wup.py`.
  5. Copies it to the card, compares every file, and replaces the old copy only once the new one checks out.

  It runs on its own too: `python builder/pipeline.py --help`.
- In the runtime:
  - `services/fs.cpp` serves the update's RomFS (`update_romfs.bin`, SelfNCCH path type 5) and the DLC's (`dlc_<content index>.romfs`, archive 0x2345678A for `0004008C` title IDs).
  - `services/misc.cpp`'s `am:app` answers the add-on content calls from `dlc_contents.bin`, `dlc_title.bin`, `dlc_ticket.bin` and `update_title.bin`.
  - The GPU-timeout fix (`services/gsp.cpp`) finds nngxSetTimeout in whatever code is loaded, so updated code and new games get it too.

## Optional hardware development tools

See [the source-only hardware debugging handoff](../tools/wiiu_diagnostics/README.md) for incremental RPX/content transfers, the dual-channel PC recorder, pinned Aroma/Cemu patches, validation results and known limitations. These tools are opt-in developer features; the experimental Aroma heap workaround needs separate review. The language GUI is functional but could use visual and UX polish before release.

### Shared hardware-observed fragment coverage

The normal GUI shader stage also reads `observed_fragments.json` beside a
known title's shader logs. It compiles missing fragment keys on the PC and
adds them to the generated pack, preserving existing shader mappings. No
console warm-up or separate Wii U program is required. Dark Moon currently
includes 167 captured fragment states; this is coverage of tested states,
not a claim that every runtime combination is known.

Maintainers can export the fragment-only metadata from a console SHDC cache:

```sh
python tools/harvest_observed_fragments.py CACHE.bin games/TITLE/shader_logs/observed_fragments.json
```

Captured vertex GLSL is deliberately excluded: it may contain translated game
programs. Fragment catalogs contain generated renderer GLSL and keys, plus the
source cache hash. Their renderer-source fingerprint must match the builder;
a changed renderer skips stale catalogs with a build-log explanation. Regenerate
and validate catalogs when changing shader generation or key definitions.
The packer retains the catalog in standalone Windows builds. The additional
shaders pass the same CafeGLSL and Wii U register checks as other PC shaders.
Hardware behavior still needs testing against a fresh runtime cache.

## Shader feature reports

The shader build writes `shader-feature-report.json` beside the shader pack.
It records actual GPU translator rejections, by shader identity, entry, boolean
variant and translator location. CPU fallback remains available; a rejection is
not proof that the game will crash. A clean report covers only discovered shader
variants, not all states that the game may construct during play.

When there are rejections, `report-shader-issue.txt` contains a GitHub issue link
with a short template. Review and submit it, attaching the JSON report and adding
the Builder version, game region/update and affected scene. Reports contain no
shader instructions, generated GLSL or game assets. Nothing is uploaded
automatically. The GUI and command-line build use the same report step.

For developer asset analysis, `tools/analyze_pica_shaders.py` accepts a captured
shader log, optional `--recipes` and `--game` paths, and a required `--out` JSON
path. It connects instruction-memory fingerprints and RomFS recipe offsets to
DVLE symbol names. Its static counts are structural estimates, not timings.
Symbol tables include helper functions, so a label alone must not be made into
a draw variant without the material's uniforms and output mapping. The separate
geometry-native translator probe does not describe GPU translator support.

The asset scan also recognizes the tagged 0x1b3/0x1b4 material layout. It follows
material/pass shader selections and shared DVLB constants/output declarations
before compiling vertex variants. This does not infer arbitrary fragment states
or promise complete coverage; recognized-but-invalid assets print a discovery
diagnostic and leave the existing fallback available.

## Homebrew output and small exports

Both window layouts have an **Output** choice: **WUP install** or **Homebrew (.wuhb)**.
Homebrew does not require `otp.bin`. Its **Build** choice selects a complete playable
WUHB, just the executable, just the two shader files, or executable and shaders.
Binary-only exports skip shader compilation; shader-only exports skip game-code
compilation. Partial exports contain no ROM assets and require an existing WUHB.
The included **Homebrew import guide** explains the companion release layout,
retaining compatible components, and installing the custom Aroma loader once.
The loader and its corresponding GPL source are in **Aroma support** in the ZIP.

The pipeline exposes the same choices through `--output-format homebrew` and
`--homebrew-parts full|binary|shaders|both`. WUP remains the default. A complete
Homebrew build written to an SD card goes under `wiiu/apps`; small exports go
under `wiiu/3dii-u/updates` for explicit import. Game saves are not replaced.

To package the Windows app with the custom loader, set `THREEDII_AROMA_MODULE`
to its built `RPXLoadingModule.wms` and `THREEDII_AROMA_SOURCE` to its matching
source checkout before running `make_app.py`.


### Validation of the November 25 Windows test package

Portable shader observations now live in the tracked
`tools/shader_observations/` catalog. Source and Windows builds verify its
inventory before preparing shaders; Windows packaging also checks the copied
inventory. Shader generation must cover every supplied fragment state through
an exact or canonical-equivalent lookup, or the build fails with
`shader-coverage-report.json`. This prevents an incomplete source archive from
silently shipping reduced precompiled coverage. It does not promise discovery
of all runtime states or full-speed rendering in every game.

The packaged Windows executable was checked under Steam Proton 11: its frozen
CLI, both QML layouts and all Homebrew choices, export/artwork/failure handling,
and compiler-policy scenarios passed. Windows devkitPPC compiled the resumable
checkpoint, renderer and kernel units. The bundled Windows wuhbtool produced a
WUHB whose embedded content was checked. The ZIP passed CRC, source-hash and
payload checks, and includes the Aroma module with corresponding GPL source.
This is Proton validation, not a native Windows test or a full game build through
the GUI. Test exports should use the same base game, update, mods and shader
revision as their original WUHB; rebuild the full game when those assets change.

## Experimental room networking

Every output format supplies a blank `3dii-u/multiplayer-room.txt` template for the SD root.
Homebrew and WUP runtimes read that same optional SD file; absent or blank hosts
leave networking disabled. Existing card settings are preserved. See
[configuration and tested compatibility](../tools/wiiu_diagnostics/TRIFORCE_MULTIPLAYER_STATUS.md).
