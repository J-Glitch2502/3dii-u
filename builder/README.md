# 3Dii U Builder

Turns your own 3DS game dumps into games for your Wii U. Pick a game, say whether to add its update, DLC and a mod, press **Build It!**, and the game lands on your SD card ready for WUP Installer GX2.

No game data and no keys come with the builder. Everything it builds from is yours: your dumps, your 3DS's keys if the dumps are encrypted, and your Wii U's `otp.bin`.

## Using it

1. Open **`3Dii U Builder.exe`** in `dist/3Dii U Builder`. That folder is the whole app: copy it anywhere, as it needs no Python and no toolchain. To run the builder from the source instead, double-click `3Dii U Builder.pyw`.
2. Click **Choose a game** (the big **+** tile, or Ctrl+O) and pick a dump. It can be a `.3ds`, `.cci`, `.cia`, `.cxi` or `.app` file, encrypted or already decrypted. The tile shows the game's art, and under it its name and maker, and a badge says how well it's known to run: **Complete on Wii U** for a finished game, **Plays on Wii U** for the other tested games, **Untested** for the rest.
3. The builder then asks about the **update** and the **DLC**, right in its window:
   - If they're in the same folder as the game, it names them and asks whether to use them.
   - If not, it asks whether you have them, and lets you pick the files.
   - You can change or remove either later, under **Extras**.
   - Then it asks about a **mod** (see [Mods](#mods)): pick its `.zip` or its folder. **Extras** has it too.
4. **Your Files**: the lights turn cyan when the builder has what it needs.
   - **3DS keys** are only needed for encrypted dumps. The builder decrypts them the way `cia_to_decrypted_3ds.py` does: makerom turns a game's CIA into a `.3ds`, then pyctr decrypts it, with `boot9.bin` and `seeddb.bin` from your own 3DS. It looks for them in `.3ds` in your user folder (`C:\Users\<you>\.3ds`), pyctr's own place; choose another folder if yours are elsewhere. GodMode9 can dump both from a 3DS.
   - **Wii U otp.bin** comes from your own console's NAND backup. The WUP is packed with the key inside it; the builder never shows or stores that key.
5. **Put it on**: your SD card (cards set up for a Wii U come first), or a folder.
6. **Build It!** It takes a while, longer for bigger games: the game's code is recompiled for the Wii U and its shaders are compiled ahead of time. You see each stage tick off. Your PC stays usable meanwhile, **Cancel** stops it cleanly, and **Details** shows everything the build prints.
7. Put the card in your Wii U and install with **WUP Installer GX2**. If you installed an older build of the same game, delete that one first (System Settings, Data Management), then install the new one.

**Games** in the bar on the left holds the games you picked before and lists the games tested on a real Wii U; **Help** answers questions about what you need, and holds the version, the update check and the light or dark look.

Saves live in the game's own save storage on the console, like any installed game. They show up in Data Management.

## Which games work

`compat.json` lists the games tested on a real Wii U. They build with the same title IDs as the builds already handed out, so a rebuild replaces the old one. All of these play:

- Tomodachi Life
- New Super Mario Bros. 2
- Paper Mario: Sticker Star
- Mario & Luigi: Dream Team
- Mario & Luigi: Paper Jam
- Mario & Luigi: Superstar Saga + Bowser's Minions
- Mario & Luigi: Bowser's Inside Story + Bowser Jr.'s Journey
- Professor Layton and the Azran Legacy
- Professor Layton and the Miracle Mask

Any other game builds as **untested**. It gets a title ID of its own (`000500003Dxxxxxx`, kept clear of real Wii U titles), and code finders run on it, but nobody has played it yet: it may not start, or it may stop partway.

**Updates and DLC** are read the way a 3DS reads them:
- An update brings its own code and its own RomFS, which the game lays over the original files itself.
- DLC brings one RomFS per content, which the game finds through the same calls it makes on a 3DS.
- The nine tested games were tested without updates, so with one they count as untested too.

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
- The 3DS system archives (its shared font and Mii data) don't come with it: they're your own 3DS's files.
  - The builder copies them from Azahar's or Citra's user folder, or from any folder holding a 3DS NAND up to three levels down in Downloads, Desktop or Documents, whatever it's called. It keeps them in its work folder.
  - Off a 3DS, GodMode9 copies `title/0004009b` (font, Mii data) and `title/000400db` (the bad-word list) out of `[1:] SYSNAND CTRNAND`; the user picks either folder (or the one holding them) with "Folder". A build stops if the game's code names an archive the files lack (`dumps.archives_named`): Tomodachi Life stops at start without the word list. Azahar gets them with File > Set Up System Files and the Artic Setup Tool.
  - The NAND's contents may be NCCH (`<n>.app`, encrypted as on the 3DS or not) or RomFS images already taken out (`<n>.app.romfs`). A folder of `<title id>.romfs` files works too.
  - Each row under "Your Files" has an "Info" button that explains its file to testers (`gui.HELP`).
  - Without the Mii data and the shared font, the app and `pipeline.py` won't build. A Miitopia built without them stopped on the console with `[err:f] result c8804478`.
  - If a build ever lacks one, the runtime's error screen names the missing archive.
- Free disk space of about three times the game's size while it builds. Work files are deleted at the end.
- From the source: Python 3.10 or newer, `pip install PySide6-Essentials pyctr pycryptodome` once, and the build tools `tools/win/build3ds.py` uses. Install each once:
  - devkitPro's PowerPC toolchain and wut, from `tools/win/install_toolchain.sh`, into `C:\devkitPro`.
  - LLVM, in `C:\Program Files\LLVM`.
  - MSYS2's `mingw-w64-x86_64-gcc`, in `C:\msys64`.
  - The CafeGLSL shader compiler, from `tools/win/build_cafeglsl.ps1`.

## How it works

- The window is Qt Quick: `ui/Main.qml` draws it (Wii U Menu glass laid out on a 1280x720 surface scaled to the window, light and dark, the in-window questions, the update card), and `gui.py` holds what it shows and does what its buttons ask. A choice that asks several things is a generator that yields each question.
- `app.py` is the one program: the window, the build when run with `--pipeline`, and `python.exe`'s part for the repository's tool scripts the build runs. In the standalone app, the build's compilers start without console windows.
- `make_app.py` builds the standalone app with PyInstaller (`python builder/make_app.py`, on a PC with the build tools above).
  - It leaves out the parts of Qt the window never loads.
  - It puts the engine beside the app, in this repository's own layout. That includes what runs of the games learned: where their code starts, and the render states they met.
  - None of the games' code goes in. A run's shader log keeps each vertex record's whole shader memory, which is the game's own code. So `shaderlogs.py` ships each record as a fingerprint of the program of the game's DVLB it ran, beside the log in a `.vsf` file, and a build finds the program in the user's own dump again (dvlb.bin). Checked on the nine games: the shader generator makes the same shaders from these as from the full logs, except that it declares fewer unused inputs.
  - A memory no DVLB program starts ships as a recipe instead (`.vsr`): where its words lie in the game's own `code.bin` and `romfs.bin`, or in a romfs file once unpacked (Yaz0, LZ10, LZ11). The build lays it out from the user's dump and checks it against the record's fingerprint, so another version of the game just leaves it out. Short runs must lie near long ones, so a recipe can't spell code out of coincidences.
    - The Mario & Luigi engine keeps a shader as plain words in its `code.bin` and loads a romfs program after it. That's 115 records the app used to leave out, and all of them come back now.
    - For a game this repository keeps no dump of, `python builder/shaderlogs.py --recipes <extracted game folder> games/g_<code>/shader_logs` writes its `recipes.vsr`, which the app ships.
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
