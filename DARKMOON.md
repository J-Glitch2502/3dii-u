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
- The shader generator says "108 variants could not be translated": not looked into yet.

## Next step

1. Senne builds it with the repository's Builder and plays it in Cemu and on his console. Random input doesn't really play the
   game, so him playing it says more than the trials can; how fast it runs on the Wii U is only known then.
2. PR to `main` with an explanation for Josiah.

## Other loose ends

- The PC trial's lazy compile of a missed function crashed after loading its DLL (exit `0xC0000005`), so a run only
  records one missed function at a time. Not looked into.
- `tools/setup.py` crashes instead of stopping when MSYS2 is missing (`install_mingw` runs `C:/msys64/usr/bin/bash.exe`).
- The Wii U build (`all darkmoon`) got as far as the title step before fix 5 (shaders: 4 vertex, 8 pixel, 8 variants not
  translated). It hasn't been run in Cemu or on a console yet.
- The Wii U falls back to the CPU for vertex shaders it hasn't compiled yet. Dark Moon has no shader set yet, so expect
  that on a first run. Upstream's advice: measure how often and how expensive before changing it.
