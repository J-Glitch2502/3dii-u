3Dii U Builder {version} (beta)
=========================

3Dii U Builder turns your own 3DS game dumps into games you install on your Wii U. There's nothing else to install: this folder is the whole program.


WHAT YOU NEED

- A Windows 10 or 11 PC (64-bit). While a game builds, drive C: needs about three times the game's size free.
- Your 3DS game dump: a .3ds, .cci, .cia, .cxi or .app file, encrypted or decrypted. Its update and DLC too, if you want them in.
- For encrypted dumps: boot9.bin and seeddb.bin from your own 3DS (GodMode9 can dump both). Put them in C:\Users\<you>\.3ds, or choose their folder in the builder.
- Your Wii U's otp.bin, from your own console's NAND backup. The game is packed with it. The builder never shows the key and never stores it.
- A Wii U with Aroma and WUP Installer GX2, and its SD card.
- For games that use the 3DS's system font or its Mii data: Azahar or Citra on this PC, set up with your own 3DS's system files. The builder copies the font and Mii data from there. Without them, those games show no text.


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
- "Untested": every other game, and any game with a mod. It may play, it may not start, or it may stop partway.
  Reports on these games help the most.


REPORTING A PROBLEM

- If a build fails, press "Show details" and copy what it says.
- Say which game it is (title and region), and whether you added an update, DLC or a mod.
- For a game that goes wrong on the Wii U, say where in the game it happens and what you saw.


WHAT STAYS ON YOUR PC

- The builder works in %LOCALAPPDATA%\3Dii U Builder.
  It keeps its compiled runtime there for the next game, and deletes each game's work files when the build ends.
  Deleting that folder frees the space; the builder makes it again.
- Your dumps are never changed. Your keys are only read.
- The games you build are made from your dumps and your keys: don't share them.

The builder comes with no game, no game data and no keys. This is a beta, with no warranty.
Third-party notices.txt lists the open-source software it carries.
