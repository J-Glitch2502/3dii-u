# Kirby Triple Deluxe: movie pacing investigation

The console capture showed 15 FPS during the opening movie; earlier captures
showed 29–30 FPS. A runtime report spanning mixed playback averaged 24 FPS.
This is intermittent and cannot be ruled out by a screenshot at normal speed.

## Evidence before the change

With Aroma trace streaming paused and the runtime debugger connected:

- A short CPU-timed interval reached 30 FPS, with about 11 draws per present,
  no CPU geometry fallback, and no recorded renderer waits/readbacks.
- Its target/texture binding cost was about 450 microseconds per draw,
  including about 147 microseconds hashing and 304 uploading. These costs
  overlap; do not add the hash/upload subtotals to binding again.
- Thread snapshots included the translated movie-filter region, including
  `f_0030fa70`, which repeatedly executes unsigned ARM byte operations and SEL.
  Snapshots establish that the path runs, not its fraction of total CPU time.
- The `gpu thread 0% busy` field is not a utilization measurement in this
  non-RECOMP_MEASURE build. CPU timers are opt-in; zeroes while off mean
  unmeasured, not free.

## Changes being tested

`cpu.h` processes unsigned byte add/subtract, saturation, and halving using two
separated lanes per word, preserving ARM GE and Q semantics. SEL constructs a
byte mask without four conditional lane selections. Signed operations retain
the existing implementation. The original movie stream and colour conversion
are unchanged; no H.264 replacement or external video assets are required.

`gx2_backend.cpp` logs `[pace]` every two seconds. It records the longest gap
between presents and disjoint gap buckets: <=20, (20,35], (35,50], (50,67],
(67,100], and >100 milliseconds. It uses one clock read per present and does
not require screenshots or the per-draw CPU/GPU timers. A 66 ms interval now
remains visible even when the average for a longer interval looks acceptable.
These are presentation intervals, not counts of unique decoded video frames;
HOME/suspension or a real loading pause can also create long gaps.

## Validation

`g++ -O1 -std=c++17 tools/test_cpu_parallel.cpp -o /tmp/test_cpu_parallel`
and `/tmp/test_cpu_parallel` compare 631,072 packed-word inputs, all twelve
byte operations, GE preservation, and all sixteen SEL masks with a scalar
reference. The test also passes at -O2 with undefined-behaviour sanitization.

PowerPC GCC -O1 emits straight-line unsigned byte operations instead of the
previous four-iteration lane loops. This is a code-generation improvement,
not by itself a measured claim of full-speed playback on the console.

## Hardware comparison procedure

1. Preserve the hardware-tested shader pair and direct-ROMFS descriptor when
   publishing a new immutable companion release; keep the base bundle/saves.
2. Transfer only while at the Wii U Menu. Activate the new release last.
3. Replay the opening movie without screenshot/FTP traffic. Keep Aroma trace
   streaming paused and the runtime connection recording.
4. Compare `[pace]` windows through the full movie, especially the captured
   15 FPS scene. Record whether colours, audio synchronization and skipping
   still work. Do not conclude that the dips are solved from a single 30 FPS
   image or interval.

## Hardware result, 2026-11-19

The published `kirby-movie-pixel-ops-20261119-r2` release was tested on the
real Wii U with Aroma streaming paused and the runtime logger recording.
The user reported the cutscene no longer dipped. One continuous 30 FPS
playback run contained 4,501 presents in 150.125 seconds (29.98 FPS), with
a longest present gap of 35.291 ms and no gaps above 50 ms. Its buckets were
1 / 4497 / 3 / 0 / 0 / 0. A later 40.045-second run contained 1,200 presents,
with a longest gap of 49.558 ms (one gap above 35 ms, none above 50 ms).

Longer gaps existed outside these continuous playback runs around changes
between 60 FPS and 30 FPS output. Do not describe the entire boot/session as
having no stalls, or presentation counts as unique decoded frames. No audio
underrun lines were recorded in this session. The evidence and user observation
support fixing the previously reported repeated 15 FPS movie dips on this run;
they do not establish every movie or game is fixed.
