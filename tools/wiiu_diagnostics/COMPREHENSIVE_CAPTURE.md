# Runtime diagnostics without another rebuild

The Fantasy Life diagnostic executable includes the video-worker fix and runtime
probes for the remaining performance investigation. The PC recorder saves game
runtime output and Aroma system output in the same session directory. Game
performance conclusions come from the runtime channel; Aroma captures system
faults and title transitions. A connected socket with no replies for 12 seconds
is reconnected automatically while recording.

## Capture

Connect and record as usual. In the recorder, **Capture performance (10s)** starts
recording if needed, requests guest and host thread snapshots, and enables ten
seconds of detailed timing. The Python equivalent is:

```python
import recorder
recorder.control('diagnose')
```

The title automatically returns to low-overhead summaries after the sample,
flushes remaining kernel timing records and prints renderer statistics. The
automatic stop is checked by the GPU worker; a fully wedged process cannot be
guaranteed to execute it. This instrumentation is diagnostic and is not a
crash-recovery mechanism.

## Runtime controls

| Command | Behavior |
|---|---|
| `set diag 0` | Disable these timing probes and GPU timestamps |
| `set diag 1` | Frame gaps, presentation flow, conversion, storage, audio, main-thread waits |
| `set diag 2` | Ten-second all-thread waits, wakers, ready delays, kernel-lock acquisition, SVC/native service durations and renderer CPU timing |
| `set diag 3` | Same ten-second sample plus GPU timestamps |
| `set diagthread 0` | Detailed timing for all guest threads |
| `set diagthread N` | Focus detailed timing on guest thread N; main wait summaries remain available |
| `diag` | Snapshot guest thread state/registers, native thread stacks, timing tables and renderer counters |
| `stats` / `ps` | Renderer report / native thread snapshots |
| `mem ADDRESS SIZE` | Existing bounded memory inspection; recorder limits size to 4096 bytes |
| `set trace N` | Existing bounded draw trace, up to 256 draws through the recorder |
| `set videoworker 0/1` | Compare conversion on the guest core or the dedicated core-0 worker |

Existing screenshots, texture/surface inspection, shader traces, renderer
switches, save/storage timing, heap-owner snapshots and exit milestones remain
available. The recorder's `renderer` action explicitly validates diagnostic and
renderer controls; ordinary `runtime` commands remain read-only.

## Interpretation

`[guest-wait]` records thread ID, guest return address, wait object or arbitration
address, total/max wait, total delay after becoming ready, and the waking guest
thread/return address (0 for a non-guest source). All timing-table updates happen
under the emulated kernel lock. Fixed-size tables report omitted samples if they
fill; use `diagthread` to narrow the capture rather than silently assuming full
coverage.

`[guest-svc]` distinguishes kernel-lock acquisition, inclusive system-call time,
native work outside the emulated kernel lock, and native-work completion-to-resume
delay. These durations overlap; do not add them together as exclusive CPU time.
The call sites can be mapped to generated guest code. `ps` supplies host stack
addresses that must be resolved against this exact build's symbol map and load
address. Detailed timing can affect performance: compare summary-only intervals
before and after each sample.

No build can anticipate every unknown defect. This covers the current video,
scheduling, rendering, I/O, audio and shutdown questions with live selection;
additional probes are warranted only if these reveal a specific missing signal.
