# Mario Maker 3DS: Cemu performance investigation

The user reports lag in Cemu and Wii U; this investigation starts in Cemu.
No hardware performance improvement has been established.

In the ghost-house editor scene, a nine-second timed sample measured about
48 presentation updates per second. The CPU geometry path ran roughly once
per presentation, with 126 input vertices and 2.14 ms per presentation, of
which 1.65 ms was geometry-program interpretation. RomFS recorded four or
five 16 KiB reads per 2.6 seconds, with native reads around 70–135 us and
roughly 2 ms of emulated delay each. Those reads do not account for the
sustained frame-budget deficit in this sample. Other scenes still need their
own measurements; this does not prove every filesystem patch is harmless.

Four captured geometry records share one executable program. The native
compiler previously declined its IFC instruction. It now accepts bounded
IFU/IFC then-regions without an else, up to the interpreter's eight levels.
Else blocks, crossing regions, conditionals inside loops and loops inside
conditionals remain unsupported. Conditional programs are conservatively
marked dependent, retaining serial execution and register state.

The real captured records passed 8,000 differential runs covering 16,000
emitted primitives with no bitwise differences. Synthetic programs passed
42,000 runs with no differences, covering all condition/reference combinations,
uniform flags, nesting, empty regions and the existing loop path. Unsupported
control-flow rejection is also checked. Game program records and generated
code remain private build artifacts outside the repository.

The candidate adds the compiled CPU geometry program to the RPX. It uses the
original pixel/vertex shader pack and game assets. A verified live Cemu A/B comparison in the same scene measured:

| Path | FPS | CPU geometry per presentation |
| --- | ---: | ---: |
| Native CPU GS, first sample | 49.2 | 1.12 ms |
| Interpreter fallback | 47.4 | 2.53 ms |
| Native CPU GS, restored | 49.5 | 1.18 ms |

The shader entry bytes were read back before each phase and restored afterward.
Earlier comparisons that called the debugger's `patch()` context manager without
entering it did not apply a patch and are invalid. The native CPU path provides
a modest improvement; it does not resolve the full slowdown. The installed Wii U
version has not been replaced with this candidate.

A later NES-style editor sample measured 28–30 FPS, no fragment misses, and a
healthy output ring. It is a different workload and must not be compared directly
to the earlier ghost-house sample. CPU emission remained a substantial cost.

The captured GSX record translates with the existing GPU shader compiler, but
its key was absent from the original pack used in the CPU candidate. A new Cemu
candidate appends the compiled program and **one vertex key**, while preserving
all existing vertex and pixel mappings. Shader validation passes (22 maximum
vertex registers of 48; 27 pixel registers of 200). The pack is above 16 MiB and
requires the current streaming GSH runtime. `append_shader_pack.py` now requires
an explicit `--allow-streaming` flag for such packs; the default resident-size
limit remains enforced. No graphics correctness or hardware performance claim
is made until the new path is observed in the same editor scene.

Captured records were also placed in the private game's `shader_logs` directory
so subsequent full shader builds include them. No game shader bytes are stored
in this repository.


## Startup follow-up

In session `2026-11-19_22-34-54-263412`, the GPU geometry candidate was actually
selected (the `aCorner` instanced shader), and subsequent editor reports showed
zero CPU-shaded draws. The scene nevertheless remained near 30 FPS. Removing
CPU geometry therefore did not resolve the remaining steady slowdown.

That recording compiled 37 pixel shaders (1,163 ms summed compiler duration,
117 ms maximum). The early title/menu screen then reached about 60 FPS. A low
initial FPS report includes time spent loading before the first displayed game
picture; it must not be read as steady gameplay FPS. A separate 2.96-second gap
was near a save/editor transition. The save writer and journal flush reported
0 ms, so the gap is not established as a slow save write.

The 37 exact shader keys were added ahead of time; a later boot logged zero
fragment misses. Six more cached pixel programs were then compiled into the
pack. Existing mappings and original GFD blocks remain unchanged; the pack
passes hardware register/clause validation. All shader sources and cache files
remain private local build inputs. This pack has not been tested on Wii U.

`shader_compile.cpp` now initializes CafeGLSL and runs its existing plain/loop
self-tests only when there is an actual queued compilation. Previously those
checks ran on every launch, including fully precompiled starts. The recorded
loop self-test took roughly 1,050 ms. Cached jobs still trigger normal compiler
initialization and background warming: this change does not silently disable
cache prewarming or missing-shader fallback. A Cemu run with six cached jobs
verified both self-tests and successful compilation after deferred startup.


The final cold-start candidate in session `2026-11-19_22-42-48-537217`
verified zero pixel compilations and no CafeGLSL initialization/self-tests during
the observed startup. The menu reached 59.8–60.2 FPS. Pre-render/loading gaps of
about 195–245 ms remained, as did a later roughly 1.4-second transition gap.
Thus compilation work was removed, but boot/loading hitching is **not fully
resolved**, and the editor's roughly 30 FPS steady rate remains under investigation.
This was a different boot/scene sequence; no controlled whole-boot speedup is claimed.


## Controlled editor comparison

A clean comparison was run with the user leaving the NES-style ghost-house
editor untouched. Six screenshots (start/end of each phase) show the same
viewport, editor UI and course. Renderer timing probes were off.

| Geometry path | FPS samples | Median |
| --- | --- | ---: |
| GPU, before | 60.1, 60.1, 60.1 | 60.1 |
| Previous native CPU fallback | 52.6, 52.3, 54.7, 50.3 | 52.45 |
| GPU, restored | 58.8, 60.1, 60.2, 60.2 | 60.15 |

The temporary RAM patch made only `gx2_gsx_ready` return false. Other compiled
vertex shaders stayed enabled. Original entry bytes were checked before every
phase and restored in `finally`, with readback verification. No patch remains
in Cemu or in the file. This establishes a geometry-path gain for this editor
scene; it does not establish that every earlier 30 FPS sample was the same
workload or that every style holds 60 FPS.

An earlier comparison had the style selector visible in two phases and closed
in the restored phase. Its three phases must not be presented as an unchanged
editor scene. The clean comparison above supersedes it.

Twenty-eight additional pixel keys observed while switching styles were then
precompiled and appended without replacing existing mappings. The updated pack
passes shader validation and retains the native CPU fallback. The active Cemu
candidate is recorded in the private game's `current-candidate.json`; the Wii U
installation remains unchanged.


## Hardware startup terminate: 2026-11-21

The complete PC recording in session `2026-11-21_03-33-59-977361`
contains the native fatal stack even though both SD renderer report files
were empty. The on-screen `read back 3 done` note is stale context, not the
faulting operation. The Aroma stream identifies guest thread 8 and this call
chain:

```
std::filesystem::remove(path) + 0xa4
kernel::FsUser::handle(IPC&) + 0x928
kernel::service_request / svc_dispatch
f_0018104c / f_001803b8
f_0010cedc
f_00288b40
f_00287b7c
__cxa_throw / std::terminate / OSFatal
```

`FsUser` command `0x0804` (DeleteFile) was using the throwing overload of
`std::filesystem::remove`. The Wii U runtime is built with `-fno-exceptions`;
a host filesystem error therefore stopped the application rather than being
returned to the guest. The saved PCs of the other sleeping threads are not
the crash stack and should not be used to infer a rendering fault.

The fix uses the `std::error_code` overload, returns the existing guest
file-not-found result on failure, and logs the archive ID, host path and
host error. Save-change notification occurs only after a successful removal.
This contains the confirmed fatal failure; it does not establish why the
original host operation failed. Its path and error were not captured in that
recording, and SD corruption, locking or permissions are not proven.

Inspection of the game's private translated binary confirms that
`f_0010cedc` sends the delete through its mounted archive. `f_00288b40`
converts the provided name to UTF-16, invokes that wrapper, and inspects the
returned error. `f_00287b7c` calls this from a file-operation state machine
with recovery branches for prior filesystem errors. The captured guest
return address `0x00287ee8` corresponds to its call at `0x00287ee4`.
The filename is supplied dynamically; no particular temporary/cache/save
filename is established by the static call chain.

`tools/test_fs_delete_file.py` compiles and executes the actual IPC case with
`-fno-exceptions`. It checks successful removal, missing files, real
ENOTEMPTY/EEXIST and ELOOP errors, and an invalid archive. Failed operations
preserve the test data and do not notify save changes. This test is included
in `tools/selftest.py`.

The private `mario-maker-deletefix-20261121` candidate retains all 116 game
objects and 43 of 44 runtime objects from the prior build. Only `fs.cpp` was
recompiled, and the original shader pack and direct-RomFS descriptor are
retained. Compilation and link pass. Hardware startup validation is pending;
this is not yet a claim that every SD-related failure is resolved.


### Hardware result

The Wii U loaded stamp `2026-11-21 03:45 g_ajhe` from release
`maker-deletefix-20261121`. Session `2026-11-21_03-48-09-757594` recorded
221 nonfatal DeleteFile failures for archive 6, the game's extdata archive:
`/vol/save/80000001/save/00040000001a0400/extdata/0000000000001a04/course000`
through `course220`, each with host error 5 (`I/O error`). The game continued
initializing its course slots and writing their data. The user confirmed it
reached the title/editor. A subsequent recording,
`2026-11-21_03-49-50-757728`, shows successful Progress writes and save flushes.
No terminate report appeared in these captured samples.

This verifies that the actual hardware failure is contained and startup can
continue. It does **not** prove the underlying deletion failures are repaired,
or that the SD card is corrupt. The virtual `/vol/save` path alone does not
identify a physical storage fault. Any future work on those errors should
preserve the player's courses and capture the lower filesystem result before
changing error mappings or deleting/recreating extdata.


### Post-fix playback and save-layer seek review

The completed `2026-11-21_03-49-50-757728` run contains no runtime crash
report, console OSPanic or AppPanic. Median two-second presentation rate was
59.94 FPS across 236 reports; 204 reports were at least 59 FPS. Forty-eight
new pixel shaders compiled successfully (1,283 ms summed, 179 ms maximum),
and loading/transition presentation gaps reached 2.813 s. These logs alone
do not identify the brief visual defects the user saw during loading; the
user reported that the picture was fine once loading finished. Precompiling
observed shaders can remove their compilation work, but is not established
as a complete visual fix.

One Aroma save-wrapper seek warning targeted `0x4bd20` (310,560 bytes).
The event offsets place it during Mii extra-data activity; the game binary
contains `CFL_OldDB.dat` and routines using this exact size. The deployed
PowerPC save object confirms that a failed initial `lseek` in `save_resize`
branches to `grow`, which writes zero blocks to extend the file. The runtime
subsequently records the full 310,560-byte `CFL_OldDB.dat` write as done,
followed by a successful flush at frame 2234. Later Progress writes and
flushes also succeed. A host simulation of the actual resize/grow functions,
with seek-beyond-EOF rejected, preserves prior bytes and extends correctly
from three initial lengths. This supports a recovered file-extension probe,
not damaged-save evidence. The wrapper warning lacks filename and errno,
so the exact descriptor identification remains a strong correlation rather
than a captured fact.

A separate final shutdown flush returned `-6` (`FS_STATUS_NOT_FOUND`). It
must not be silently described as a successful final flush or confused with
the earlier recovered seek. Earlier successful writes/flushes do not prove
that every future save/exit path is correct. Raw game data, compiled shaders,
private binary excerpts and complete PC logs remain outside the repository.
