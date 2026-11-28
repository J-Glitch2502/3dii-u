# Fantasy Life video investigation

## Combined capture and copy trial

### Shared-chroma movie converter candidate

The next converter processes one 2x2 YUV420 block at a time. It shares chroma
products across four pixels, folds the two fixed-point shifts and constant
channel offsets into an equivalent single shift, and stores each ABGR pixel as
one native word with explicit byte-order handling. Rotation, other input/output
formats, partial blocks and DMA routing retain the existing fallback.

`test_y2r_movie_blocks.py` compares the actual fast path with the scalar fallback:
384 cases, 43,954,824 identical output/guard/DMA-gap bytes, including all four
standard tables, extreme signed coefficients, widths through 1024 and partial
single-block heights. The existing 640-configuration before/after comparison
also retained 2,621,440 identical bytes; coefficient and DMA tests pass. The
512x240 RGBA PC microbenchmark changed from ~0.63 ms to ~0.19 ms per conversion.
The later hardware comparison below measured ~8.5–8.7 ms conversion work;
the PC microbenchmark itself is not a console FPS measurement.

The general scalar fallback retains old tile scratch contents in unused rows
when a multi-block image ends part-way through a tile. The existing direct path
does not populate that scratch for full blocks, so unused padding rows differ
from that scalar fallback after a full block. This predates the shared-chroma
change; the 384-case scalar comparison excludes that padding-only difference.
Actual before/after byte equivalence remains required for changed paths.

### Valid native-copy comparison

The corrected `fantasy-bulk-copy-v2-20261120` executable (loader stamp 22:29)
contains both `rt_bulk_copy_enabled` load relocations and a `memcpy` call in
`f_001b8188`; disassembly is saved alongside its matching RPX and symbol map.
Session `2026-11-20_22-31-19-869257`, `bulkcopy-v2-comparison.json`, measured
22.29 FPS on, 16.68 FPS off, then 20.70 FPS on. Each block settles for 2.2 seconds
after command acknowledgment, then measures ten seconds. Audio queues remained
healthy with zero low/wrapped recoveries. Keep the fast path enabled for this
title. Consecutive movie intervals differ in content, so this reversal supports
an improvement but is not a matched-frame percentage or full-speed validation.

A subsequent resolution comparison crossed the end of the movie: the low-level
block stopped reporting Y2R conversions and both modes reached a 30 FPS menu.
Do not count that as a movie-performance improvement. Automatic resolution mode
was restored afterward.

The current GPU pass-timestamp accounting includes command-starvation gaps:
`next.bot - max(current.top,current.bot)` does not exclude the interval before
`next.top`. Its reported ~46 ms GPU busy per flip is not reliable evidence of
GPU saturation. CPU draw preparation during the bounded capture was around
1.5 ms per displayed flip, with no CPU vertex fallback or pixel-shader misses.

Session `2026-11-20_22-21-01-884210` recorded an invalid first copy trial.
Off/on summary windows measured 17.57/17.69 displayed FPS, but PPC object
inspection showed that generated game units do not define `__WIIU__`; the
candidate block had been excluded entirely. The command acknowledgment only
proved that the runtime switch existed. These numbers are baseline observations,
not evidence that native copying does or does not help. The corrected guard also
accepts the compiler's `__DEVKITPPC__` macro; the equivalence harness now exercises
that guard without defining `__WIIU__`. Verify both switch and memcpy relocations
in the generated PPC object before the next hardware comparison.

Native `ps` snapshots of a running thread contain its last saved OS context;
repeated identical PCs must not be treated as unbiased CPU samples. They identify
a reachable call chain, not its exclusive CPU cost.

Session `2026-11-20_21-53-02-194614` captured detailed timing with automatic
return to summary mode. Main-thread arbitration at `006a5ca4` was signaled by
guest thread 5 (`001690d8`); arbitration at `172dbfac` was signaled by the movie
thread 17 (`0016917c`). Most elapsed time preceded the signal; post-ready delay
was much smaller. This does not justify claiming that the scheduler is fixed or
that changing thread priorities will recover full speed.

Repeated samples and the guest call chain identify the aligned word-copy routine
at `001b8188`, called by the movie buffer path at `00478190`. A compiler candidate
recognizes its complete 29-instruction sequence without depending on the title
or address. It uses native copying for bounded, disjoint spans, retains the
original stack writes and tail logic, and falls back on overlap, oversized
copies, saved-stack overlap, or pending preemption. Dead overflow-flag writes
remain dead; flag-returning callers retain the original flag behavior.

`tools/test_recomp_bulk_copy.py` executes both translations and compares entire
CPU state and memory for 2,556 cases, including tails, alignment, large copies,
overlap, pending preemption and the disabled fast path. Mutating any instruction
in the signature must prevent recognition. The switch-helper and mixed-call
regressions pass. The hardware on/off comparison above supports a performance
improvement, not full-speed validation. `set bulkcopy 0/1` supports a live
comparison in the candidate build.

Normal regeneration emits the candidate automatically; `prepare_bulk_copy_trial.py`
can update an existing safe generated cache with the same checked transformation.

Movie metadata from the original RomFS, inspected with ffprobe: `movie/logo_l5.moflex`
is 400x240 at 30 FPS; `mov000.moflex` through `mov004.moflex` are 400x240
Mobiclip streams at 60 FPS, with IMA MOFLEX audio. Encoded rate does not establish displayed application FPS. Visible Azahar
captures of this intro at 100% speed show 30 application FPS; the current
display target for this sequence is 30 FPS. The 512-wide Y2R
operation includes texture padding. Coefficient lookup tables retained identical
pixels but showed no clear additional hardware performance gain (~9.3–9.8 ms per
conversion). They must not be reported as a confirmed performance fix.

Hardware session: `2026-11-20_18-51-29-705793`, program `0004000000113200`.

The opening movie presents approximately 7–10 FPS while audio continues without
source shortages or output recoveries. Lower-resolution rendering did not remove
the slowdown. Renderer reports showed no CPU vertex fallback and screen pairs
arrived at roughly the same rate as presents. Y2R performed about 60 conversions
per second, each taking about 11 ms on the guest CPU core. Resuming the guest
after conversion took only tens of microseconds.

Direct tiled RGBA conversion retained identical output in the 640-configuration
comparison (2,621,440 bytes). It reduced hardware conversion time to approximately
9.3–9.6 ms and displayed FPS to approximately 9–10. Reader and tile-write batching
alone had no meaningful hardware performance effect.

`run_blocked` releases the emulated kernel lock but executes native work on the
calling guest host thread. All guest host threads are pinned to core 1, so Y2R
still competes with the decoder and other game code. A dedicated conversion worker
on core 0, below audio priority, moves that work off the guest core. The caller
waits outside the guest kernel lock, and the completion event and IPC reply retain
their original order. Requests are serialized while their service context is live.

Live `set videoworker 1/0` comparisons, excluding the timing window crossing each
switch, recorded 17.79 FPS on, 9.34 FPS off, and 17.63 FPS on again. These are
consecutive movie intervals, not identical frame sequences. The off/on reversal
supports a real improvement but does not establish full-speed playback. Audio
queues remained healthy. The initial baseline includes menus and must not be
used as a movie average. Raw data and `video-worker-comparison.json` are saved in
the session folder.

The worker defaults off. A per-title `content/video-worker-enable` marker enables
it on launch. Other titles require separate hardware validation. The recorder's
renderer command allowlist supports the live switch.

A separate correction reports the actual Y2R DMA output span to the renderer:
DMA unit size is a byte chunk, not a scanline. The prior unit-times-height
calculation reported approximately 3.9 MB for a 0.5 MB output. Correcting this had
no measured FPS benefit in this scene. DMA writer tests verify the last written
byte, including internal gaps and excluding a trailing gap.

One return-to-Menu hang stopped after audio shutdown. No nonempty saved crash
report was available; PC logs retained the event. Exit milestones now surround
final log synchronization and stream flushing. Later Menu exits succeeded. The
hang's cause is unresolved, and the milestones are diagnostic rather than a fix.


## FFmpeg source and current hardware result

Reference captures `azahar-intro-reference-{1,2,3}.png` in hardware session
`2026-11-20_22-55-22-229492` show 30 App FPS at 100% speed during the movie.
This corrects the earlier target inferred from its 60 FPS stream metadata.
The shared-chroma build measures ~8.5–8.7 ms conversion work, versus ~9.3–9.5 ms
previously, but still ~21–24 displayed FPS. This is a modest conversion gain,
not full-speed validation. The sampled audio queues have zero recoveries.

FFmpeg upstream references:
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/mobiclip.c
- https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/moflex.c
- https://github.com/FFmpeg/FFmpeg/blob/master/libswscale/yuv2rgb.c

Its decoder emits YUV420P and retains six reference frames. The first ten frames
of the user's `mov000.moflex` decode successfully; checksums are saved as
`ffmpeg-mov000-first10.framemd5` in the session folder. This is an independent
reference, not a native decoder integrated into the Wii U runtime. Such an
integration needs the actual compressed-packet boundary, persistent decoder
state, output plane strides/padding, guest register/return conventions, and
correct seek/reset handling. Existing `port/aac/aac_ffmpeg.cpp` demonstrates
native FFmpeg linkage, but its AAC-only libraries do not establish that Mobiclip
is already available. Any reused code must retain its LGPL notices and terms.

60 conversions per second already occur while presentation is slower. A native
decoder may free shared guest-core CPU time, but conversion throughput alone
neither identifies nor rules out decoder CPU contention. Keep measuring complete
frame flow and avoid declaring a native decoder a proven performance fix.


The subsequently rejected `fantasy-life-ppc-converter-20261120` trial removed unused per-frame
RGB/rotation/line scratch allocations on the direct movie path and uses exact
branchless channel saturation on the PPC target. The 384-case harness exercises
that arithmetic explicitly; all pixel/DMA checks pass. It packages successfully
with the prior fixes retained; the later hardware result below rejected it.
An attempted thread-local scratch reuse was abandoned because elf2rpl rejected
its TLS relocations; the candidate keeps private per-call storage and does not
contain that approach. PC timing stayed around 0.19–0.21 ms; only hardware can
establish whether the PPC branch reduction helps.


## Rejected PPC branchless/allocation experiment

Session `2026-11-20_23-10-35-924301` recorded 10,856 us mean conversion work
and 21.28 displayed FPS, versus 8,584 us and 21.86 FPS in the previous summary.
These are different movie intervals, so the small FPS difference is not a
matched-scene conclusion. The ~26% increase in repeated conversion work is a
clear regression in the target conversion path, with no convincing display-rate
gain. The `fantasy-life-ppc-converter-20261120` candidate is rejected; its source
and results remain saved for audit. Repository Y2R source is restored to the
faster shared-chroma implementation; hardware rollback uses its preserved RPX
and matching symbol map. Both the branchless saturation and allocation-layout
changes were combined, so this comparison does not identify which caused it.

No demonstrated hardware ceiling has been established. Remaining work should
measure guest-core CPU contention, buffer copying/graphics submission, and
native Mobiclip decoding alternatives against the observed 30 FPS reference.
Do not claim 30 FPS is guaranteed or the runtime is maximally optimized.
