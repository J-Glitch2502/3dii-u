# Dark Moon shader discovery and runtime compilation

## Current result and limits (November 25)

Fresh-cache hardware replays improved from 154 pixel compilations (54,733 ms
reported compilation time) to 31 (9,037 ms), without adding the held-out replay
to build inputs. One brief self-correcting white flash and some cutscene/audio
desynchronization remained. Capture lengths differ, so these results do not
establish a whole-cutscene FPS gain.

A separate stationary-lab off/on/off lighting comparison measured
20.8/25.2/20.7 FPS. A lower-scale, 30 FPS-limited configuration measured
29.6–29.7 presentations/second in that lab. Scale and pacing change the workload;
this is not proof of full-resolution 30 FPS throughout the game. The latest
persistent Balanced/Sharp/Fast menu policy was uploaded but still needs its own
boot/menu/cutscene validation. Other titles keep their existing policy.

The sections below retain the investigation sequence. Statements that a later
comparison was pending describe that point in the sequence; the later result
sections and this summary define the current evidence. Fragment predictions are
bounded heuristics from observed states, not a complete material/pass decoder.
The screen-slice scaling experiment remains off by default and is unverified on
hardware. Classic versus resumable execution has build/manifest support; a
controlled hardware performance comparison is still pending.

## Measured runtime problem

In the 2026-11-24 fresh-cache opening test, the vertex shaders with keys
`a0fe8d2b588dce55` (entry 327) and `ee4b6f52ab594d90` (entry 344) took
9,214 ms and 3,875 ms to compile. The existing vertex worker idled by polling
a 64-bit atomic up to 20,000 times. On Wii U that atomic uses a shared spinlock;
the worker also shares core 0 with the lower-priority shader compiler.

Parking idle vertex workers on Wii U, with the same shader pack and another
fresh-cache replay, reduced those compiler reports to 919 ms and 1,167 ms. The
user reported an improvement. This is a measured comparison of those shaders,
not a matched full-game benchmark or proof that every cutscene hitch is gone.
Desktop workers retain their previous spinning behavior.

The next revision prioritizes demanded vertex jobs, then demanded pixel jobs,
then speculative cache warm-up. Promotion retains cache identity without
duplicating a job. Host tests cover 1,000 pixel arrivals and cache promotion;
this queue policy is built but has not yet had its own Wii U comparison.

## What the game's files reveal

Both slow shaders reconstruct from the same program in
`/materials/material_new.data`. DVLE symbols name them `MorphGhostDepth` and
`MorphGhost`; their shared 31-instruction routine is `ApplyMorphs`. The captured
boolean-specialized structural analysis reaches 89 and 108 instruction sites,
respectively. Static sites are not dynamic instruction counts or CPU timings.

The ordinary DVLB scan found one vertex entry (two output-map records). All 34 captured vertex records
translate successfully, producing 50 distinct GPU vertex sources and 76 lookup
keys in the tested generator run. Thus these observations demonstrate a
discovery/first-use compilation gap, not missing GPU CALL support. The separate
native geometry translator rejects the captured vertex programs' calls/jumps;
it is not currently a native vertex backend, and its rejection must not be
presented as a GPU feature failure.

The material asset has 2,905 typed descriptors, 31 material headers and 294
pass descriptors. The existing NW4C material scanner finds no materials in it.
The new tagged-material parser follows the descriptor references and the
primitive/blob arenas, reads the shared DVLB and its boolean/int constants,
and selects the entry addresses actually named by material/pass headers. It
finds 38 vertex entries without a gameplay trace. They cover all 23 distinct
entries in the captured opening data; all 34 captured records agree on code,
used swizzles, integer defaults, fixed b14 and output mapping. Runtime boolean
variants remain the shader generator's responsibility.

The 38 selections generate 80 distinct vertex sources and 120 lookup keys;
Windows CafeGLSL under Steam Proton compiled them in the tested run with at
most 28 vertex registers (48 available). Synthetic fixtures cover selection,
helper exclusion, arena alignment, constants, default-pass tokens and malformed
input rejection. The parser is in the shared PC shader-discovery path and uses
format tags, not a title ID or captured shader list. Its fresh-cache hardware
comparison is pending. It currently handles the recognized uncompressed tagged
assets in RomFS; other containers/layouts may need additional support.

Fragment-state decoding from these material fields remains an investigation.
Debug labels alone still cannot safely become drawable states. Rejected or
unrecognized material layouts retain the existing discovery/runtime fallback
paths, with a diagnostic rather than invented shaders.

The optional asset analyzer associates captured programs with recipe locations
and DVLE names. Raw programs and private reports stay in the local build area.
The repository's VSR records describe reconstruction offsets/fingerprints;
fragment catalogs contain generated fragment states, not game vertex programs.

## Generic changes and coverage limits

The PC builder can import a generator-fingerprinted fragment catalog before
CafeGLSL compilation. It preserves existing mappings, bounds input size and
rejects stale catalogs. The Dark Moon catalog contains 167 observed fragment
states. A real Windows CafeGLSL run under Steam Proton compiled them with at
most 39 pixel registers; the original mappings/payloads were checked intact.
This is partial observed coverage, not enumeration of every game state and not
the runtime starvation fix above. New runtime misses can still occur.

Actual GPU translator failures now produce a structured feature report and a
reviewable GitHub issue template through the shared GUI/CLI build pipeline.
Failures are keyed by program identity as well as entry/boolean variant, so
different programs at the same entry do not hide each other's diagnostics.
Reports contain no raw instructions or GLSL and are never uploaded automatically.
A clean report only covers variants discovered by that build.

The runtime separately counts missing vertex shaders, forced CPU draws, missing
geometry pipelines and GPU resource limits. Opt-in timing reports a bounded
top-six list of fallback keys/entries. Summed task wall durations can include
preemption and parallel work; they are not exclusive CPU milliseconds.

## Remaining performance work

One later hardware interval was predominantly GPU-bound, with about 25–26 ms
GPU work per presentation and little CPU fallback. In a warmed same-scene
comparison, temporarily disabling detailed fragment fallback did not materially
change roughly 39–40 FPS. Normal shading was restored. This does not exonerate
every first-use fragment state, but it prevents attributing all remaining lag
to CPU fallback or claiming a universal shader fix.

Further material decoding, representative scene coverage and fresh-cache Wii U
verification remain necessary. Emulator correctness checks are not Wii U
performance measurements. No raw game binaries, captures, saves or screenshots
are included in this report.

## Configuration prediction investigation (2026-11-25)

A newer hardware miss file was retrieved without uploading it as training input.
After normalization, it adds 43 distinct states to the older capture's 148.
All newer combiner sequences already occur in the older capture. The principal
new components are perspective rather than orthographic shadow sampling, a
three-light prefix, and different attenuation/directional/shadow combinations.
Thus additional TEV recipes alone do not explain these particular misses.

Private bounded ARM-sandbox experiments reconstructed fragment command setup
from virtual methods in the game executable. Both complete and partial combiner
writes were recovered; partial writes must retain inherited stages and fields.
Those experiments are not shipped as a general decoder: they require runtime
inputs, and their extra combiner sequences predicted none of the 43 newer
states. Raw executable extracts and recovered command streams remain local.

The shared build now generates a bounded configuration neighborhood from a
game's observed fragment states: light-count prefixes, alternate observed light
types on shadow lights, and both shadow projection modes. It retains physical
light numbers until after prediction. Generated candidates are excluded from
subsequent observation input and cross-game observation copying. Discovery emits
an explicit non-exhaustive JSON report; zero translator failures still does not
mean complete gameplay coverage.

Using only the older capture, this predicts 27 of the 43 held-out states with
2,572 candidates. This is a coverage result, not a measured speedup. The current
candidate combines these with existing discovery/variants and generates 3,499
fragment sources and 80 vertex sources, with zero translator rejections.
Native compiler validation and real-console rendering/timing remain required.

Normalization now removes per-light spot attenuation when its LUT is disabled,
per-light RGB shadow gating when no enabled RGB shadow path reads it, and a
physical light number when no attenuation LUT reads it. Shadow alpha remains
independent of the per-light RGB flag. C++ and Python normalization are compared
byte-for-byte in the fixture. The generator/cache revision advances to 6.

## Fallback candidate

Sixteen lit/unlit texture-mask families retain the combiner, fog and lighting
paths but omit TEV texture fetches for unused/null units. Null textures retain
the uniform-provided value. Bump/shadow sampling retains its own lighting path.
The lit fallback previously clamped to four lights; the candidate extends its
uniform flags and light invocations to all eight. Its uniform layout and the
shader pack must match the executable. These changes do not establish that all
previous fallback limitations (such as procedural textures) are solved.

Source tests cover all texture masks and unchanged lighting code across masks.
CafeGLSL register/program checks and console image/performance comparisons gate
promotion. The previous correctly shaded fallback must not be replaced by a
solid-colour performance shortcut.

## Execution and release comparisons

The native build driver now accepts explicit classic/resumable execution and
an explicit game optimization setting, writing an execution manifest. It also
sets RECOMP_RESUMABLE whenever the actual selected mode is resumable, including
the automatic default. A classic -O1 build and resumable -O1 build provide a
controlled execution comparison; save states remain enabled in the primary
candidate. No CPU cost has yet been established by these builds alone.

`analyze_frame_timing.py` reports weighted presentation cadence, disjoint gap
buckets, long hitches, noncached shader compilation and CPU fallback reasons.
It rejects malformed pace rows and excludes intervals crossing a requested
scene start. It does not infer frame percentiles from buckets or equate
presentation FPS with simulation speed. The renderer's first statistics report
now uses elapsed time since rendering began instead of dividing accumulated
startup frames by a nominal second.

Hardware regression matrix before release:

1. Same intro and renderer resolution; fresh per-pack cache, timers off.
2. Replay with a warm cache; compare hitch counts, compilation and picture.
3. A still lab scene, brief CPU/GPU timing samples, then timers off again.
4. Matched classic -O1 versus resumable -O1, same shaders/settings/scene.
5. Homebrew and installed-title save/load, including relaunch, repeated
   replacement, and indicator show/clear on both screens.

Keep FTP/plugin configuration consistent within comparisons. Take screenshots
outside the main timing intervals. Preserve ordinary game saves. Record build,
shader-pack identity, scene and cache condition with every result. GPU pass
changes require a measured expensive target and a rendering-equivalence check;
no new pass elimination or global resolution reduction is justified yet.


## Native validation of the fragment-family candidate (2026-11-25)

The final compiler batch exposed two separate failures. Proton's Steam launch
path failed during DLL copying; retrying through Proton `runinprefix` exposed
CafeGLSL's actual ALU scheduling error in fallback texture-mask variant 6.
The eight-light fallback generated a DOT4 channel group the scheduler could
not place. Explicit, ordered `precise` scalar dot products avoid that lowering
in fallback sources. Specialized fragment programs remain unchanged by this
workaround. Floating-point rounding may differ from a contracted dot product;
console image comparisons are still required.

The rebuilt complete pack contains 80 vertex and 3,499 fragment programs and
passes `check_registers` and `gsh_rules`: maximum 28/48 vertex registers and
39/200 pixel registers. Shader binary size is 24,948,140 bytes. The matching
resumable executable and shader index bring the transfer to 51,047,849 bytes.
The primary candidate retains session/persistent states. The screen-slice
resolution experiment is off by default. Native compiler success establishes
program validity, not rendering correctness or a measured console speedup.


## Fresh-cache failure and spotlight-enable sharing (2026-11-25)

The fragment-family candidate's fresh run was recorded in
`2026-11-25_16-37-34-503073`. Its screenshot at 16:40:10 shows missing white
background regions and a 4 FPS overlay. Nearby pace reports contain 10
presentations in 2,181 ms and repeated long waits inside presentation. The full
recording contains 154 noncached pixel compiles (maximum 539 ms), no vertex
compiles, and no fallback-route report rows. These are not evidence that CPU
vertex fallback never occurred. The timing intervals also do not independently
measure audio/video clock drift.

The previous warmed-cache observations cannot establish fresh-start correctness.
A shader pack passing native instruction/register validation is insufficient to
establish fallback rendering equivalence. A later forced-fallback comparison in
the lab rendered substantially the same room as specialized shaders; it does
not reproduce or exonerate the eight-light intro materials from the marked shot.

A read-only live miss capture recovered all 154 fragment requests (129 distinct
states under revision 6 normalization). Spotlight enables on individual lights
account for a major variant explosion: the old predictor varied shadow lights,
but ordinary lights also changed spotlight enables independently.

The new specialized shader reads spotlight enable from `Light.att.z`, a formerly
reserved float component, while specializing the LUT identity and other lighting
features. Both CPU/PC normalization paths omit this enable bit, retaining the
physical light number whenever the shared spotlight LUT is enabled, even if
this particular draw has it switched off. The real draw still supplies its
spotlight enable; decoded build states must not determine that uniform. The
cache revision is 7. The existing fully dynamic fallback still reads its own
spotlight flag. A synthetic check covers all 256 eight-light masks, source/key
sharing, disabled-LUT elimination, and physical-LUT identity retention.

Without adding this fresh capture to build inputs, the actual new index covers
108 of the 154 previously missed requests. Under the new normalization they
collapse to 88 distinct states, of which 57 find shipped programs. The remaining
31 distinct states include new combiners, fog variants and a lighting setup.
This is a coverage result, not a measured console speedup or a complete fix.
The pack shrinks from 3,499 to 1,214 fragment programs and passes native checks
at maximum 28/48 vertex and 36/200 pixel registers. Cold console replay, picture
checks and timing comparison remain required. Clear generated caches before
every cold-start comparison; label warm-cache tests separately.


The missing-program path now also requests/stores console compilations under
`fs_key(fs_normal(ds))`, matching the shipped-program lookup, and generates the
source from that normalized state. The miss log retains the first raw state.
Previously the lookup tried a canonical key but scheduled each absent raw key
separately, so fixed-function differences and uniform-only flags could compile
the same source repeatedly. Installing the completed canonical shader clears
the existing draw caches as before. With the spotlight change, the recorded
154 requests imply 31 distinct remaining compile keys, rather than 154 queued
raw-key compiles. This prediction still needs cold-console confirmation.

The new pack is 8,909,680 bytes, below the existing 16 MiB resident-pack limit;
it therefore also avoids the previous candidate's on-demand shader file seeks.
That is a second performance variable to record when attributing any speedup.
The matched resumable RPX, pack and index transfer is 34,977,990 bytes.


## Spotlight-sharing cold-console result

The fresh run `2026-11-25_16-53-34-800684` contains exactly 31 pixel compilations,
matching the prediction from the earlier capture; total reported compiler wall
time is 9,037 ms (previous recording: 154 compilations, 54,733 ms). No cached
compilations or vertex compilations appear. The user reports one brief white
background which corrected itself, with the remainder visually substantially
improved, but continued lag and slight cutscene audio desynchronization. There
is no marked screenshot in this recording, so the white flash cannot be tied
to a particular draw or shader from this log alone.

Long gaps above 100 ms fell from 189 to 46 across these recordings, but the
captures differ in length (267.610 versus 228.567 seconds of pace windows) and
scene residence; they are not a controlled average-FPS benchmark. Moderate
50–100 ms gaps remain numerous, including periods after compilation completed.
A 688.501 ms maximum in the new log is at frame 107 during startup, before the
main cutscene; it must not be presented as a remaining movie shader stall.
Both runs allocate the same main and transfer target sizes, including a
768x1536 main target and 369x1476 half-screen target. These support a further
GPU-cost investigation; they do not establish that reducing effect resolution
is visually acceptable. A short separate CPU/GPU sample follows the replay.


The separate live sample `2026-11-25_17-00-11-402747` reports 23.4
presentations/second across the measured interval, 479 draws/presentation,
zero CPU-rendered draws, no new texture uploads, and the same 31 fragment
misses. GPU timestamps cover 232 flips: 42.7 ms busy per 42.6 ms interval,
including 31.39 ms drawing the main target (3.0 passes), 3.13 ms presentation,
1.74 ms LCD work, 1.53 ms main clears, and 1.32 ms half-screen transfers.
This demonstrates GPU saturation in that later sampled scene, not the exact
unmarked white flash. The first report averages earlier execution and its
initial 0% busy/zero CPU lap timings are not a valid timed baseline. Both
CPU and GPU timers were acknowledged off afterward. Main-target shading and
pass cost are the next measured targets; CPU fallback is not the bottleneck
in this particular sample.


## Controlled rendering-work probe and next experimental build

The stable lab dialogue was held during `2026-11-25_17-06-24-743595`.
Clean full/lower/full intervals measured 23.4/41.5/23.3 presentations per second,
with GPU busy time 42.8/24.1/42.8 ms and main-target drawing 31.45/16.14/31.36 ms.
Each reports zero CPU draws and no new texture uploads. Initial reset intervals
include screenshots, transitions and stale GPU timestamp spans and are excluded.
The two-thirds scale probe changes pixel workload, not shader coverage. Its
screenshot has visible horizontal seams absent from both full-size images, so
the speedup is not yet a shippable image-equivalent improvement. The live setting
was restored to full, then automatic; timers were turned off.

A new candidate retains spotlight sharing/canonical compilation and adds two
independent switches, both off by default. `lightguard` skips a distance-attenuated
light's remaining RGB calculations when its distance contribution is exactly
zero. Negative attenuation still takes the original path. The last light is
not skipped when its Fresnel LUT can write primary or secondary alpha, which is
independent of attenuation. The switch is passed in previously unused
`uLscale1.w`; generated-cache revision is 8. Source tests protect that alpha
exception and absence of guards on lights without distance attenuation.
All 1,214 fragment and 80 vertex programs pass native validation, at maxima
27/200 pixel and 28/48 vertex registers. These are validity checks, not hardware
speed or image-equivalence results.

`copyrect` changes linear GPU row copies to round rectangle endpoints rather
than separately rounding their extents. Source UVs use actual rounded surface
padding and dimensions, rather than logical texture-size ratios. Fractional
coverage tests verify adjacent rows/columns share boundaries and cover their
complete extent, including the observed scales. Hardware seam removal remains
unverified. Turning off automatic scaling at the selected resolution now returns
a previously forced lower level to full size instead of leaving it stuck lower.
A matched executable and shader pack is required for `lightguard`; the geometry
experiment does not alter game saves. The prior working build is retained locally
and in the immutable console release directory.


## Lighting/copy hardware tests and persistent Dark Moon modes

`live-lighting-171418` holds the "Where to, Luigi?" dialogue rather than the
previous "Greetings, Luigi!" dialogue. In an off/on/off test the new lighting
guard measured 20.8/25.2/20.7 FPS and 48.1/39.7/48.2 ms per frame; main-target
work fell 36.98 to 28.50 ms. Screenshots show no obvious missing room lighting.
This proves a gain from the switch within the candidate, not a controlled
comparison against the previous executable in this later dialogue.

`live-copy-171529` captures the older and corrected row-copy paths at low scale.
Both native TV screenshots appear coherent. Consequently the earlier GamePad
capture's seams are not conclusively attributed to the copy math; capture timing
may also be involved. The corrected interval has 42.7 presentations but only
30.1 top-screen swap requests per second and 47.3 VBlanks per second, so it is
not accepted as evidence of a 42.7 FPS game-rate improvement. Its before/after
neighbor intervals are consistent. Endpoint/padding correctness is established
by the coverage tests; broader hardware image testing remains required.

`live-balanced-171849` combines both switches and the existing 30 FPS limiter.
At 90%, 85%, then 90% scale it measured 29.6/29.7/29.7 presentations/second.
The 85% setting leaves more margin in main-target work (22.29 versus 23.87 ms),
with no obvious picture defect in the native TV captures. Timing reports under
a cap can include paced idle time and should not be interpreted as uncapped
GPU saturation. The code previously declared cap_game/cap_fps but never
initialized a title policy; this explains why the documented limiter was idle.

A persistent policy is now limited to the tested program ID 0004000000055f00:
Balanced is 85% of the selected 720p scale with a 30 FPS limit; Sharp is full
selected resolution with a 30 FPS limit; Fast uses the previous two-thirds
scale, uncapped. At selected 480p there is no further scale reduction. The
existing options row displays MODE and cycles these choices, stored in the
per-game wiiu_render_mode setting; Save/Load/Quit indices account for that row.
Other titles do not acquire this policy. Custom 1080p retains automatic scaling
except in Sharp. The lighting/copy switches start on for this tested title,
while remaining opt-in for other titles. Persistent-mode boot, menu selection,
save/load navigation and cutscene synchronization remain hardware test items;
a near-30 FPS lab test does not establish perfect timing throughout the game.


## Later gameplay, fallback shadows and Fast-mode seam candidates

The long November 25 recording includes multiple launches and idle menu time;
frame numbers repeat. The later gameplay must be selected by launch/record order,
not by averaging the whole recording. Continuous screenshots subsequently
captured the Front Yard sky alternating between flat green and normal dark sky,
alongside new fragment compilations and 5–11 FPS shown in individual images.
This supports a first-use/fallback investigation, not proof of a particular sky
shader. Screenshot/FTP work perturbs timing and is labelled separately.

Code inspection found that the fallback sampled a type-2 shadow map as ordinary
RGBA. Its GBA channels contain packed depth, not material colour. The candidate
uses the same depth decode, bias and perspective/orthographic comparison as the
specialized shader, and applies the zero-distance guard while retaining the
Fresnel-alpha exception. Generator/native shader checks pass; the corrected
fallback's hardware picture and speed remain unverified. Cache revision is 9.

The marked lab image also contains a dark row at 120 of the 240-row top image.
The user describes a vertical Fast-mode line; that specific observation still
needs confirmation on hardware. The candidate corrects both raster-scissor axes:
round the endpoints of adjacent rectangles instead of independently truncating
the start and width. A fractional-scale fixture reproduces an uncovered pixel
with the old arithmetic. The candidate keeps Fast's scale and uses the existing
copyrect switch for an A/B comparison. Host coverage tests are not proof that
this removes every visible line.

PC configuration prediction now varies shadow-light direction/type and combines
observed fog settings without replacing material alpha tests. Earlier inputs
produce 73 of the 183 examined later miss states, versus zero with the preceding
predictor. Those examined misses guided rule development, so this is development
coverage rather than an independent unseen-scene validation. New material
combiners still require recognized asset decoding or additional observations.
Mixed-log readers now skip legacy CPU geometry records instead of treating them
as fragment lengths, and reject truncated/oversized records.

The private hardware candidate retains all 2,379 earlier fragment lookup keys,
all 120 vertex keys and the 183 examined later fragment states. The initial
expanded pack was 32,036,520 bytes, above the 16 MiB resident threshold. A local
bounded repack retains protected mappings and removes only speculative mappings;
its GFD is 16,777,072 bytes, with 2,099 pixel and 80 vertex programs. Native
validation reports maxima of 28 pixel and 28 vertex registers. This local repack
is not yet a general PC-builder budget policy. Reused earlier specialized
binaries were allowed only after checking that specialized generation and key
normalization were unchanged; the modified fallback programs come from the new
compilation. Real-console regression testing is pending.

The regular profile/settings and a completed 112,604,400-byte checkpoint were
backed up privately on the PC. No save or generated shader payload is published.


## Outdoor geometry discovery (2026-11-25, hardware candidate)

With continuous screenshots disabled, two consecutive stable outdoor samples
reported 13.6–13.8 presents/s, roughly 437 draws/flip and two CPU geometry draws
processing 2,088 vertices/flip. Those geometry draws cost 39.86–39.87 ms/flip,
including 34.63 ms in geometry execution and 3.62 ms in vertex execution. No
textures uploaded in these intervals. The reported zero primitive count comes
from native emission instrumentation; it does not count interpreter emissions
and must not be read as proof that these draws produce nothing. GPU timestamps
around presentation can include upstream starvation; they are not independent
costs to add to the CPU breakdown.

The active executable lacked a native geometry table. Asset discovery previously
kept vertex entries from DVLB but never exported declared geometry entries for
native CPU translation. Code builds now extract those entries from recognized
base/update asset binaries into a derived `assets_geometry.gsl` before compiling
native functions. This applies to code-only exports too. Shared instruction
memory is retained rather than truncated to 512 words. Runtime instruction and
swizzle hashes still select the function, and unsupported programs retain the
interpreter. Existing user-recorded `.gsl` files are preserved.

Dark Moon's asset-extracted entry 505 matches the hardware-recorded native lookup
key. The existing differential harness compared 2,000 randomized runs and 49,684
emitted primitives with zero differences. Synthetic discovery checks cover stage,
entries beyond 512 words, deduplication, updates, malformed inputs and stale
output replacement. Conditional-translator regressions pass 42,000 comparisons.
The RPX links with externally visible native table/count symbols. This is a
validated candidate; its outdoor FPS benefit still requires a matched hardware
sample after installation. Geometry extracted through this recognized asset
format does not cover every game's dynamically assembled programs.


### Native outdoor hardware result and next candidate

The console logged entry 505 running as native code. Two post-warmup samples
reported 20.1/19.8 presents/s (19.7/19.8 top-screen picture swaps/s), with roughly
433–438 draws/flip and 2,070–2,088 CPU geometry vertices. Geometry processing
fell to 18.00–18.15 ms/flip, including 13.33–13.44 ms in the native geometry runs
and 3.34–3.36 ms in vertex execution. This agrees with the user's report of an
improvement, while leaving the outdoor scene below full speed. These scene
samples are not a controlled input replay. No fragment misses or texture uploads
occurred in the measured intervals, so cache compilation is not the current
steady-scene explanation.

The next candidate compiles generated native geometry functions at `-O2`,
independently of resumable ARM game code's `-O1`. It preserves strict arithmetic,
disables floating-point contraction, and does not change game execution mode.
The geometry differential harness now tests that optimization level too: 2,000
Dark Moon comparisons plus 42,000 synthetic conditional/loop comparisons passed
with zero differences. The RPX grows by 2,965 bytes. Hardware performance of
this second change remains unverified until the next matched outdoor sample.


### Follow-up outdoor probes and remote debugger controls

The `-O2` native candidate measured 19.9–20.0 presents/s, with CPU geometry
17.90–17.96 ms versus the prior 18.00–18.15 ms. This is a small timing difference
without a meaningful frame-rate gain; it is not evidence of another substantial
optimization. Live Fast/full/Fast probes returned 20.0/19.2/19.9 presents/s.
Lightguard off/on returned 19.7/19.8 presents/s. These tests retained the same
outdoor camera and restored Fast + lightguard afterward. Screenshots were off
during timing. The GPU pass timestamps move idle time between pass labels and
must not be interpreted as independent CPU-free shader execution costs.

A subsequent manual GamePad capture confirms a left-to-right dark seam near
the middle of the top image. The scissor-endpoint correction did not fully
resolve this observation. The next uploaded debugger candidate adds a live
viewport-nudge switch for image comparisons, retaining the automatic default.
No claim of a seam fix is made yet. The completed outdoor checkpoint was
retrieved privately for repeatable tests.

The new runtime queues network save/load requests through the existing
checkpoint pump. Remote pause opens the normal options menu; speed selects its
existing 100/200/400% modes. Explicit guest word reads and compare/write edits
execute under the checkpoint barrier, validate guest mappings and permissions,
and invalidate mapped GPU data after writes. They do not provide host-native
instruction stepping or hardware watchpoints. The recorder exposes state/menu
controls and records explicit renderer/RAM commands. Its automatic screenshot
setting remains disabled. Unit/integration checks pass; these added controls
still require the first live-console verification.


### User-selected default policy

New per-game settings default to Fast (`wiiu_render_mode=2`), as requested by
the hardware tester. Existing settings.ini choices remain authoritative.
Only titles with a recognized profile use this field; the tested Dark Moon
profile currently matches USA 0004000000055f00. Fast retains its existing
resolution policy and uncapped rate; it can exceed the original game's rate
in light scenes. Balanced and Sharp remain selectable. This default change
is independent of the experimental parallel-geometry switch.


### Scissor-centre and parallel-rain candidate (hardware pending)

Manual nudge 0/default/0.5/default captures retained the dark row; the nudge
setting was restored. Disabling exact copy rectangles made the row darker,
so that change was retained. Native surface captures show the half-slice's
column 230 and later as black, and a darkened column in the assembled LCD.
Raster clipping previously used floor-scaled copy extents. With the observed
0.96153846 scale and 0.25 viewport offset, pixel centre 230.5 belongs inside
the raster window; floor(240*scale) excludes it. The candidate uses
ceil(edge*scale+offset-0.5) for raster scissors, preserving floor endpoints for
stored copy rectangles and clamping scissors to their target. Host fixtures
check pixel-centre coverage and shared boundaries. Resolution is unchanged.
The user's fresh-launch hardware image still needs checking.

Native geometry dependency analysis previously declined every loop. The
candidate conservatively models structured loop bank parity and returns to
END, while rejecting prior scratch/condition values, b15 and unmodelled IFs.
It also requires all three emitted vertices to belong to the current invocation.
Dirty-register/emitter and extreme-loop differential checks pass for the actual
rain program; synthetic positive/negative fixtures pass. The parallel path now
preserves the last complete primitive's input uniforms and avoids updating
shared emission timing accumulators from worker threads. Its enable switch
remains off by default, and is an experimental live comparison, not a verified
FPS gain or a general promise about all games' cross-program scratch behavior.

Remote pause/resume and a guest RAM read/compare-write command were observed on
hardware (the write test retained the same word). Remote persistent load also
succeeded in 9055 ms for a private test copy whose timestamp-only build identity
was adjusted after checking the serialized format was unchanged. The original
checkpoint and payload were preserved, and normal compatibility guards remain.
This private diagnostic rebind is not release compatibility across arbitrary
builds. At the user's subsequent request, the next test will use a normal fresh
launch without loading any checkpoint; an optional checkpoint transfer was
cancelled after the executable and shaders had been published successfully.


### Fresh-launch seam result and rejected parallel experiment

The fresh-launch manual GamePad capture no longer shows the middle dark row.
In the same camera view, average row brightness (8-bit channels, x235..619)
changed from 27.76/16.86/28.64 across rows 119..121 to 27.07/26.94/28.00.
The middle-row deficit fell from 11.34 to 0.59. This confirms the observed seam
in that scene, not every possible fractional clip in every title.

Actual shared-rain A/B/A reported 19.7/18.5/19.7 presents/s and geometry
18.09/20.83/18.14 ms. The switch was restored off. Shared emission buffering
and merging made this path slower; the dependency proof is not itself a
performance gain. CPU/GPU timers were disabled afterward.

The next opt-in candidate reuses mesh-index bounds after an exact comparison
of the current source bytes. It does not depend on guest cache flushes, so an
unannounced write or state restore is detected on the next lookup. Auxiliary
storage is bounded to 2 MiB plus entry metadata and one temporary <=64 KiB
replacement allocation; allocation failure keeps the normal scan. It defaults
off until the hardware A/B comparison. Host sanitizer tests cover modified
interior indices, replayed bytes, cache collisions, empty/oversized records and
storage limits. This has not yet demonstrated an FPS improvement.
