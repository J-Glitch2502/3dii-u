# Kid Icarus: Uprising diagnostics and observed shaders

Hardware program: `0004000000030100`, prepared input `g_akde`.
Baseline session: `2026-11-21_03-11-14-999045`.

The user's screenshot is a low-FPS marker, not a reported visual defect. Nearby
presentation windows show approximately 17–20 FPS and repeated 250–346 ms gaps.
The complete recorded session initially contained 119 dynamic pixel compilation
completions, with the slowest 317 ms. These observations do not prove compilation
explains the sustained slowdown: only one 43 ms compile appears in the bounded
marker excerpt. Detailed render-phase CPU timing was enabled later in another
scene, which measured ~29.6 FPS, ~181 draws/flip, and only ~0.05 ms of geometry
fallback per flip. Do not substitute that later scene for the slow one.

## Prepared update

The current WUHB's embedded shader index hashes to `dde26df98ab2a138`, matching
the console's `shader_cache_dde26df98ab2a138.bin`. The inputs for this update
come from that exact pair, rather than a nearby build directory with a different
index. Its cache contains 717 pixel records and no vertex records. All 717 are
compiled on the PC and appended; original GFD blocks and index mappings remain
unchanged. Combined shaders occupy 7,975,396 bytes, and the hardware instruction
and register checks pass. This is precompilation of observed states, not a new
claim of corrected artwork or guaranteed 30 FPS in every scene.

The builder's register checker previously rejected a valid pixel-only intermediate
pack merely because it had no vertex stage. It now accepts either nonempty stage
while retaining both register limits and instruction validation. The real 717-
pixel intermediate and the final combined pack passed. Cache/pack unit suites
(three each), twelve recorder tests, and 2,556 ARM copy equivalence cases passed.

The prepared game executable's SHA-256 matches the code embedded in the user's
WUHB. Publish only RPX, shaders, markers and the retained `direct-romfs-source.bin`;
do not replace the bundle, RomFS assets or saves. Preserve release
`kid-current-20261119-sd` for rollback. Upload uses `--no-readback` as requested.

## CPU timing behavior

An opt-in `content/cpu-timing-enable` marker enables render-phase wall-clock
measurements and an automatic renderer report every two seconds, alongside
frame gaps, conversion, storage and audio summaries. Worker elapsed-time
accounting now runs with timing enabled in release builds; the old zero-percent
report did not establish that the worker was idle. These are elapsed durations,
not an exclusive CPU instruction profiler or a measure of physical GPU saturation.

The recorder's existing Capture performance action selects `set diag 2` for ten
seconds: thread wait objects, wake sources, ready delays, kernel-lock acquisition,
SVC/native-service durations and CPU rendering costs. It then returns to level 1;
titles with this marker retain render-phase summaries. `set diag 0` disables the
probes for an overhead comparison; `set diag 1` restores the marker's CPU timing.
Averages can hide rare stalls, so correlate them with maximum frame gaps and
compile/I/O event times in the same session. Skip the first report's initial
counter baseline when calculating rates. GPU pass-stamp intervals can include
command-starvation gaps and must not be treated as exclusive GPU busy time.

Status: executable and matched shader pack built and checked, then uploaded
and activated as `kid-cpu-shaders-20261121-retry` (43,996,820 bytes, size checks
without readback). The first transfer was interrupted before activation. Hardware
performance of this new release remains untested. `set diag 0` also stops automatic renderer reports;
`set diag 1` restores them.
