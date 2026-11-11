3Dii U Builder {version} (beta)
=========================

3Dii U Builder turns your own 3DS game dumps into games you install on your Wii U. There's nothing else to install: this folder is the whole program.


WHAT YOU NEED

- A Windows 10 or 11 PC (64-bit). While a game builds, drive C: needs about three times the game's size free.
- Your 3DS game dump: a .3ds, .cci, .cia, .cxi or .app file, encrypted or decrypted. Its update and DLC too, if you want them in.
- For encrypted dumps: boot9.bin and seeddb.bin from your own 3DS, from GodMode9. Put them in C:\Users\<you>\.3ds, or choose their folder in the builder.
  - boot9.bin: open [M:] MEMORY VIRTUAL, highlight boot9.bin and press Y, then open [0:] SDCARD, press Y again and choose Copy.
  - seeddb.bin: press HOME, choose More..., then Build support files. It lands in gm9/out on the SD card.
  Each row under "Your files" in the builder has a "How do I get this?" button with these steps.
- Your Wii U's otp.bin, from your own console's NAND backup. The game is packed with it. The builder never shows the key and never stores it.
- A Wii U with Aroma and WUP Installer GX2, and its SD card.
- Your 3DS's system files: a few small files every 3DS keeps in its own memory, the system font many games write their text in and the
  parts Miis are drawn from. Games load them from the console, so dumps don't have them, and Mii games stop without them. From your own 3DS, either way:
  - Azahar: File > Set Up System Files, with the Artic Setup Tool running on your 3DS. The builder then finds them by itself.
  - GodMode9: in [1:] SYSNAND CTRNAND > title, highlight the 0004009b folder and press Y, then open [0:] SDCARD, press Y again and choose Copy (not Move).
    In the builder, press "Folder..." next to "3DS system files" and choose that 0004009b folder.
  The builder also looks in Citra's user folder, and in Downloads, Desktop and Documents. It copies them once and keeps them for every game after.


GETTING STARTED

1. Unzip the whole "3Dii U Builder" folder to a short path such as C:\Games. Keep the engine folder next to the exe, and don't run it from inside the zip.
   If Windows says a path is too long while unzipping, or the builder says files are missing, unzip it again into a shorter folder.
2. Open "3Dii U Builder.exe". This beta isn't signed, so Windows may say it "protected your PC": click "More info", then "Run anyway".
3. Click the empty game card (or press Ctrl+O) and choose your dump. The builder then asks about the game's update, DLC and mods.
4. Under "Your files", each dot turns green once the builder has that file.
5. Under "Put it on", choose your SD card or a folder, then press "Build it!".
   Big games take a while: the game's code is recompiled for the Wii U and its shaders are compiled ahead of time.
   The first build also compiles the builder's own runtime, so it takes a few minutes longer than the rest.
6. Put the SD card in the Wii U and install the game with WUP Installer GX2.
   If you installed an earlier build of the same game, delete it first (System Settings > Data Management), then install the new one. Deleting a game deletes its saves.


WHICH GAMES WORK

- "Plays on Wii U": the games tested on a real Wii U. The badge shows when your dump is the same edition that was tested:
  Tomodachi Life, New Super Mario Bros. 2, Paper Mario: Sticker Star, Mario & Luigi: Dream Team, Mario & Luigi: Paper Jam,
  Mario & Luigi: Superstar Saga + Bowser's Minions, Mario & Luigi: Bowser's Inside Story + Bowser Jr.'s Journey,
  Professor Layton and the Azran Legacy, and Professor Layton and the Miracle Mask. They were tested without updates.
- "Runs in Cemu" (the Games page): built with this builder and played in the Cemu emulator, not yet on a real Wii U:
  Mario Kart 7, The Legend of Zelda: A Link Between Worlds, Metroid: Samus Returns, Kirby: Triple Deluxe, Rhythm Heaven Megamix,
  Phoenix Wright: Ace Attorney Trilogy, Animal Crossing: New Leaf, Kid Icarus: Uprising, Super Mario 3D Land and Miitopia.
- "Untested": every other game, and any game with a mod. It may play, it may not start, or it may stop partway.
  Reports on these games help the most.


IF A GAME RUNS SLOWLY OR FREEZES ON THE WII U

- Turn off the SDCafiine and FTP (ftpiiu) plugins while you play: open Aroma's plugin menu (L + D-pad Down + Minus), switch them off, then
  start the game again. With SDCafiine on, a tester's New Super Mario Bros. 2 ran very slowly in the castles and ran well with it off; with
  FTP on, games have frozen.
- The first time a game shows a new kind of scene it can stutter for a moment while the console makes that scene's shaders. It keeps them,
  so the next time is smooth.


REPORTING A PROBLEM

- If a build fails, press "Show details" and copy what it says.
- Say which game it is (title and region), and whether you added an update, DLC or a mod.
- For a game that goes wrong on the Wii U, say where in the game it happens and what you saw.


WHAT STAYS ON YOUR PC

- The builder works in %LOCALAPPDATA%\3Dii U Builder.
  It keeps its compiled runtime there for the next game, plus up to 2 GB of each game's compiled code, so building a game again
  (with its update, or with a newer builder) only compiles what changed. It deletes each game's other work files when the build ends.
  Deleting that folder frees the space; the builder makes it again.
- Your dumps are never changed. Your keys are only read.
- The games you build are made from your dumps and your keys: don't share them.

- When you pick a game, the builder downloads its cover art from GameTDB (art.gametdb.com) for the game's start-up screens and Wii U Menu icon.
  It sends only the four letters of the game's product code. Under Extras, "Banner art" lets you choose your own picture, or remove it to use the game's icon.

The builder comes with no game, no game data and no keys. This is a beta, with no warranty.
Third-party notices.txt lists the open-source software it carries.
