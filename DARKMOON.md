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
   e.g. callbacks `0x4270c8`, `0x427910`).
4. `port/pc/frontend_pc.cpp`: `RECOMP_MEMDUMP` in the headless trial.
5. `tools/make_game_title.py`: finds `icon.bin` where `extract_game.py` puts it (the Wii U build stopped at the title step).
6. Not code: `games/darkmoon/functions.txt` (not in git) holds the functions found at run time. One the analysis still misses
   is `0x4e0ed4`, called from Thumb code at `0x101978`.

With these the game boots, calls the services, starts 9 threads and draws: a lantern loading screen, then the
"Thrill Tower" (ScareScraper) screen with a menu on the bottom screen, ~1000 GPU lists in the first 600 frames.

## The open problem: a freeze after ~10 s (frame ~600, DSP frame ~2190)

Every full-speed trial ends with thread 8 spinning at `0x8304e4`, the GPU counters stopped, all other threads waiting on
the lock it holds. Same with `--speed 50`.

What the game does (all addresses in Dark Moon's code.bin):

- `0x83048c`: appends a wave buffer to a voice's list: `node->next (+0x14) = 0`, `status (+0x11) = 1`, lock, walk to the
  end, link. Thread 8 is stuck walking a list that has become a loop: a node whose `next` is itself (found at `0x15d276a4`
  in a memory dump, status 1). `0x830528` removes one. Lock enter/leave: `0x3a291c`, `0x37ccf8`.
- Wave buffer node: `+0x10` ?, `+0x11` status (1 queued, 2 playing, 3 done), `+0x12` id (u16), `+0x14` next, buffers of
  `0x380` bytes, ids counting up.
- Voice objects (static, same address every run): `0x9ae970` and `0x9ae9ec`, the two channels of one stereo stream.
  `+0` DSP source number, `+4` sync count, `+0xc` on, `+0x30` list head, `+0x34` buffers given to the DSP.
- `0x25bb88`: the voice's status update (called through `0x23a984` from `0x20bfec`, for 24 sources). It ignores a status
  whose sync count doesn't match; only when "current buffer id dirty" is set does it walk the list, marking the buffers
  before the current one 3 and the current one 2; it then checks `is_enabled`.
- `0x20bfec` picks the DSP region it reads the status from by the frame counters (`ldrh [r5,#0x1a]`, `bic` with 1), then
  increments its counter.
- The game does not submit a frame for every DSP frame: by DSP frame 944 it had submitted ~807 (counter logging). It
  submits nothing during the first ~55 DSP frames.

What a `RECOMP_VOICEWATCH` run showed (trial17): at DSP frame 2174 voice `9ec`'s list is `199:2 200:1 201:1`, 3 given,
DSP playing 200 with dirty set; at 2175 the list is empty and 0 given, while the DSP still plays 200. Voice `970` handled
the same status correctly (200 playing, 201 and 202 waiting). About 20 frames later the list loops on buffer 206.

Current theory: the game handled an interrupt late, read the status from the other region, which is two frames old, so its
current buffer is no longer in the list; the update then walked the whole list without finding it and marked everything
done. A real 3DS (and Azahar, whose DSP and CPU run in lockstep) is never that late.

Not enough, or tried and **did not** fix it (reverted, not on this branch):

- `services/dsp.cpp` already holds DSP frames while no thread waits on the pipe event. Dark Moon's interrupt thread
  (priority 24) only wakes its sound thread and waits again at once, so that gate sees the game as ready.
- Holding DSP frames until the game's frame counter moves: the game doesn't submit every frame, so this stalled audio
  (1022 give-ups in 700 frames) and it still froze; a version that disarms itself when the game stops submitting also
  still froze.

On this branch, **not yet tested together**:

- `audio/source.cpp`: "current buffer changed" stays set until the game has submitted its next frame (tested alone in
  trial16: still froze).
- `audio/dsp_core.cpp`: the newest source and DSP status is written into the region the game submitted as well, so a late
  read gets the newest status (never tested: trial18).

## Next step

Run trial18 and read the voice log:

```
python tools/win/build3ds.py trial darkmoon 3600 --autoplay 1 --run watch --env RECOMP_VOICEWATCH=9ae970,9ae9ec --log-lines 5
```

- No freeze (GPU counters still rising after frame 600, thread 8 not at `0x8304e4`): keep the fix, check the stream lists
  in the log stay sane, try a longer run and the Wii U build, then a PR to `main`.
- Still freezes: find the frame where a voice's list goes wrong in the log (`grep "voice 009ae9ec"`), then look at which
  status the game read at that moment. Logging which region `0x20bfec` reads and the status it saw at that moment would
  settle the theory; check `services/dsp.cpp`'s comment above `dsp_tick`, which
  describes this same bug from another game.

## Other loose ends

- The PC trial's lazy compile of a missed function crashed after loading its DLL (exit `0xC0000005`), so a run only
  records one missed function at a time. Not looked into.
- `tools/setup.py` crashes instead of stopping when MSYS2 is missing (`install_mingw` runs `C:/msys64/usr/bin/bash.exe`).
- The Wii U build (`all darkmoon`) got as far as the title step before fix 5 (shaders: 4 vertex, 8 pixel, 8 variants not
  translated). It hasn't been run in Cemu or on a console yet.
- The Wii U falls back to the CPU for vertex shaders it hasn't compiled yet. Dark Moon has no shader set yet, so expect
  that on a first run. Upstream's advice: measure how often and how expensive before changing it.
