# Planet Robobot: residual movie hitch and audio stutter

## Current status: experimental; stutter improvement unconfirmed

Kirby / Planet Robobot investigation is paused at the user's request. Preserve
the code, candidate builds and recordings. Do not describe these experiments as
a confirmed fix for Planet Robobot's movie stutter, or promote its mixed mode as
a validated performance default.

The corrected mixed build passed the user's Cemu movie replay. After deploying
it to the Wii U, the user reported the same lag at the same spots. The hardware
recording still contains short audio-source frames and presentation gaps. The
compiler-boundary regression test and individual timing improvements below are
separate findings; they do not establish an audible playback improvement.

## Evidence and distinction

The updated movie decoder retained the large speed improvement. During the
subsequent real-Wii-U replay, mostly 33–34 ms presentation intervals included
one 38.407 ms gap and a later 66.759 ms gap. The user heard intermittent audio
stutter. No `[audio]` near-empty-queue reports were recorded. This does not
establish glitch-free sound: those reports did not cover every underrun path.

Aroma trace streaming was paused; the runtime recorder stayed connected.
Earlier recordings and startup/loading pauses must not be substituted for the
specific replay. Presentation intervals do not measure unique decoded frames.

## Reproduced output-ring defect

The AX ring contains 8,192 samples, with a 1,600-sample target cushion. The
producer waits while `host_queued()` is at least that cushion. Its old modulo
subtraction returned a large positive value when the playback cursor passed
the writer. That value caused the producer to sleep instead of calling the
existing recovery code in `host_output()`. Stale ring audio could repeat until
the apparent queue drained, with no near-empty report.

For example, write position 3,200 and playback position 3,600 produced an
apparent 7,792 samples. The old producer would wait roughly 189 ms to fall
below the cushion, although no fresh samples remained. The new queue result
is zero, consistent with `host_output()` already treating distances above
half a ring as an underrun/backlog requiring recovery. Valid queue wrap still
works. This preserves the output rate, cushion, movie pixel arithmetic, shaders,
assets, save identity and the existing recovery behavior.

`tools/test_wiiu_audio_ring.py` compiles the actual output source against fake
AX cursors. It covers producer starvation/recovery, normal wrap, near-empty
recovery, every modular distance and shutdown. Restoring the old queue policy
fails the wrapped-underrun assertion; the new source passes. This establishes
the defect, not yet that it caused every audible glitch in this game.

## Bounded timing

`set avtime 1` enables two-second summaries on the runtime log:

- `[audio-time]`: longest audio-thread wake gap and DSP tick duration.
- `[audio-ring]`: queue extrema, low/wrapped recovery counts and observations
  of wrapped queue distances. Observations may count multiple reads per event;
  recoveries count actual reset operations.
- `[perf]`: longest presentation gap and GPU-pump work duration. A pump's wall
  time includes native calls/waits; it is not isolated shader execution time.

`set avtime 0` disables these summaries. Existing PC recorder clients can use
`set cputime 1/0`, which also toggles the summaries alongside CPU draw timers.
Discard the first timing window after enabling because counters may include
startup/previous disabled periods. No interrupt sampler or screenshot is used.

The always-on `[pace-gap]` message for gaps above 50 ms splits the interval into
time before `present()` and time within it. Presentation-gap reporting now uses
two clock reads per present. It cannot alone distinguish guest decoding from
other CPU work before submission.

New recorder versions accept bounded `avtime 0|1` through the explicit renderer
control action. Existing buttons, session folders and log files remain the same.

## Hardware validation status

The updated PowerPC runtime builds and links while reusing the already-tested
game code and original bundled shader pair. Subsequent hardware replays below
confirmed residual stutter. No complete stutter fix has been demonstrated.

## Follow-up source and IO evidence

The ring-only hardware test recorded a healthy queue (about 1,500–1,759 samples
in warmed windows), no low/wrapped recovery, and short DSP execution times. The
user still heard glitches. The proven ring defect therefore does not explain
this observed residual movie stutter by itself.

Source summaries identified the stereo PCM16 movie voice (rate 1.347470,
44,100 / 32,728), whose source number changes between launches. One two-second
window contained 13 short frames, 13 blank boundary starts and 10 empty-source
transitions while AX stayed fed. A generated blank frame is not an AX underrun.
The source refill change consumes an available buffer immediately when an
already-playing stream resumes, preserving first-start/reset behavior. Its new
regression fails before the fix and passes after; all 531 DSP checks pass. Some
actual input starvation remains in hardware tests, so this is not a complete
solution by itself.

The read summaries measured native IO around 10–11 ms and guest resumption up
to roughly 5–6 ms during affected playback; the large emulated read delay was
then zero because real work had already exceeded the budget. A measured 66.666
ms frame gap spent 66.353 ms before present and only 0.313 ms inside present.
These measurements do not uniquely attribute the time to storage, CPU decoding
or scheduling, and do not establish the SD card is too slow.

## Opt-in sequential read-ahead experiment

An immutable release marker `romfs-readahead-enable` enables a cache using the
existing 1 MiB direct-IO scratch buffer, without extra asset copies or threads.
After a seek the first read is exact; a sequential successor can fetch up to
256 KiB (or the original larger request), bounded by the RomFS end. Cached bytes
are only from this read-only, verified bundle source. Any native failure clears
the cache and returns to ordinary filesystem reads. Other releases are unchanged
when the marker is absent.

Actual-loop tests verify byte equality for sequential requests, random seeks,
cache boundaries, large reads, EOF and IO failure. Twenty sequential 64 KiB
requests use six physical reads instead of twenty. On Wii U the cache enabled
successfully; one sample had ten fast reads out of thirteen and mean native IO
4.983 ms, but physical misses peaked around 20 ms and source shortages persisted.
This was not yet a successful stutter fix.

The trace also exposed a redundant delay on cache hits: the runtime added
approximately 6 ms of cartridge transfer timing after data was copied from RAM.
The next candidate identifies fully cached reads and preserves the asynchronous
0.663124 ms minimum without charging the byte-transfer term again. Uncached
reads retain their previous policy. The actual-loop/timing test checks both
RECOMP_FS_DELAY modes, hit/miss classification, minimum latency and subtraction
of time already spent. The full hardware replay reduced the extra wait on warmed cache hits from about
6 ms to roughly 61–62 microseconds. The movie still recorded 108 short source
audio frames and one 66.260 ms presentation gap. The user reported “Better, but
some stutter.” This confirms the redundant delay was removed, not that the
movie's remaining source starvation is fixed. Cemu already plays the movie at
full speed without these changes; emulator playback speed is not evidence of
Wii U performance. Use it for candidate correctness checks, and measure timing
and audio on hardware.

## Mixed-compilation trial: indirect decoder boundary

The first experimental mixed build reached drawing in Cemu, then failed with
`no recompiled code at 0x0028a5e0`, the return site of the indirect ARM call at
`0x0028a5dc`. Its pointer table selects `0x00367400`, within the configured safe
movie-decoder range. Direct calls into that range already fully synchronize
registers and dispatch intermediate safe entries until the original return
address is reached. ABI-mode indirect calls did neither: they used the normal
ABI spill and unwound if the first dispatch returned elsewhere.

The candidate compiler correction applies the existing direct safe-call
protocol to indirect targets inside the safe range, including Thumb pointers.
Indirect targets outside the range retain the previous ABI path. The synthetic
execution regression fails against the old emitter and passes with the change;
it checks a two-stage safe callback, register propagation and an ordinary ABI
callback. The switch-helper regression also passes. The corrected game passed
the user's Cemu movie replay and was deployed as
`robobot-mixed-icall-20261119` (17,191,462 transfer bytes). The user reported the
same lag at the same spots on Wii U. Mixed mode therefore has no confirmed
stutter benefit; the installed candidate is retained, with no further Kirby
changes while the investigation is paused.
