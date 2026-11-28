# Wii U development workflow and experimental stability patches

This source-only handoff collects the local debugging workflow alongside the 3Dii U runtime fixes. ChatGPT assisted with implementation, investigation, and documentation; hardware observations below came from the user's Wii U. These are reviewable experiments, not a claim of complete compatibility or full-speed execution.

## What changed, and which project owns it

| Component | Change | Reason / limits |
| --- | --- | --- |
| 3Dii U runtime | Optional lightweight startup/runtime console, safer thread snapshots, bounded sampled draw reports | Distinguish guest progress, native threads, shader selection, GPU retirement and filesystem waits. An attached client can suppress the application's watchdog fatal screen; it cannot recover a hung kernel. |
| Shader loading | Validate GFD/index bounds, retain small packs in memory, support additional pixel shaders, physical-SD compilation cache | Avoid repeatedly allocating through redirected file paths; reject malformed pack offsets. Does not guarantee every shader exists or is correct. |
| Direct file access | Direct WUHB RomFS source descriptor and physical file helper | Read the original assets without repeatedly entering the redirected wrapper. Descriptor must match the existing bundle. |
| Save / services | Save directory and filesystem compatibility, language configuration, guest service corrections | Preserve native save ownership and expose the expected 3DS services. See source diff for exact behavior; no bundled saves. |
| Builder | Per-game language and native Linux build adapter | Host language when supported, English fallback; GUI needs visual/UX polish. Windows remains the existing distribution route. |
| Aroma RPXLoadingModule | Generic adjacent `.wuhb.dev` companion override, selected before launch | Replace RPX and selected content without repacking assets or moving saves. Requires this patched module, not stock Aroma alone. |
| Aroma ContentRedirectionModule | Bounded flight recording and request allocation changes | Diagnose redirected FS calls without making the recorder itself depend on the observed mapped heap. |
| Aroma MemoryMappingModule | Lock transition recorder plus experimental mapped-root-heap mutex guard | Hardware had a persistent mapped heap spinlock owner. The original corrupting/abandoned-owner operation remains unproven. This is an experimentally successful workaround, not proof FTPiiU itself is faulty. |
| ScreenshotWUPS | Explicit FTP screenshot request/response | A marker requests the next available GamePad frame; no continuous capture. |
| Cemu fork | Python/Aroma support fixes and tools exported as a pinned patch | Useful for reproducible debugger experiments; native heap hooks and GPU/cache behavior still differ from real hardware. |
| PC recorder | One Connect and Record flow for Aroma and game runtime | Both logs, timestamps, offsets, and requested screenshots belong to the same session. |

The broad compatibility changes here overlap earlier PRs #12 and #13; Fantasy Life SD and Mario Party CRO startup work also had earlier merged PRs #14 and #16. Review against current master instead of reapplying already merged fixes. This branch starts from master `479b55a`. Changes under `third_party/3dsrecomp` retain their EUPL-1.2 licensing.

## Current investigation reports

[Filesystem and render-target review hardening](REVIEW_HARDENING.md) explains
the latest source changes, binary evidence and pending hardware validation.

- [Mario Maker startup, rendering and save-layer findings](MARIO_MAKER_PERFORMANCE.md)
- [Reconciliation of superseded Mario Maker draft #13](PR13_RECONCILIATION.md)
- [Fantasy Life conversion worker, native copy and remaining movie bottleneck](FANTASY_LIFE_VIDEO.md)
- [Planet Robobot experiments: performance benefit unconfirmed](ROBOBOT_AUDIO_PACING.md)
- [Kid Icarus diagnostics and observed shader precompilation](KID_ICARUS_PERFORMANCE.md)
- [Live bounded diagnostics and interpreting elapsed timings](COMPREHENSIVE_CAPTURE.md)

## Reproduce the binary-only update workflow

1. Build a normal homebrew WUHB once with the required original game assets. Put it at `sd:/wiiu/apps/Example Dev.wuhb`. Preserve its identity so its existing saves remain associated with it.
2. Build RPXLoadingModule from the pinned source and patch listed below, using that project's devkitPPC/WUT dependencies and build instructions. Back up the existing module and test in a separate debug Aroma environment. A module replacement takes effect after reloading that environment.
3. Rebuild only code with the usual builder, or `tools/linux/native_build.py code`. Keep the generated game code/object cache for incremental builds. `tools/linux/append_shader_cache.py` validates SHDC v2 captures and appends missing states to a new matched pack using the trusted compiler. It preserves old block/index mappings, validates generated registers and refuses packs above the resident limit. `--inspect` does not compile. The generalized tool was checked against the 1,552-record capture and its already-extended pack; the original local compilation produced the hardware-tested pack.

The native adapter accepts explicit SDK, WUT and optional CafeGLSL paths; run `--help`. It does not download or supply a toolchain, ROM, shader compiler, or keys.
4. If needed, place only changed content in a temporary directory. `shaders.gsh` and `shaders.idx` must be supplied together. A changed executable must still match its assets and shader ABI. For direct asset reads, generate `direct-romfs-source.bin` with `tools/wuhb_romfs_source.py --help` against the exact installed base bundle.
5. Enable FTPiiU, close the game, and publish:

```sh
python3 tools/ftp_homebrew_dev.py WII_U_IP \
  'wiiu/apps/Example Dev.wuhb' /path/to/rebuilt.rpx \
  --content /path/to/changed-content --release test-001
```

Omit `--content` for an RPX-only update. Releases are immutable; use a new release ID on retry. Uploads are read back and hashed, and the activation marker is changed last. This keeps a new RPX and changed shader pair together. It is a pre-launch replacement, not an in-process hot reload.

```text
sd:/wiiu/apps/Example Dev.wuhb
sd:/wiiu/apps/Example Dev.wuhb.dev/
  active.txt                         # test-001
  builds/test-001/
    root.rpx
    content/                         # optional changed files only
      shaders.gsh
      shaders.idx
      direct-romfs-source.bin
```

Absent content uses the original bundle. The module resolves the selected companion directory for the launched WUHB; it is not a hard-coded Brain Age/Mario Party title list. Saves remain in the original homebrew save location. To roll back, close the title and select a previously verified release, or disable the companion marker to use the bundle. Keep matching shader/executable versions together. Do not modify a running title's pack in place.

## Record and inspect a session

Install Python 3/Tk; optional python-xlib gives X11 global F8/F9. Run `python3 tools/wiiu_session_recorder/recorder.py --console WII_U_IP --output '/path/to/Wii U/Logs'`. See the recorder README for full controls and KDE launcher setup.

Enable debug Aroma TCP logging and allow the PC receiver through the local firewall. Enable the opt-in game runtime with an empty SD file `wiiu/3dii-u/debug/console-enable`. Connect listens for Aroma syslog on TCP 7965 (beacon UDP 4445) and connects to the game's TCP 4405 console when it appears. Record writes `console.log`, `runtime.log`, and `events.jsonl` together. Runtime status is sampled about every three seconds; thread lists about every thirty seconds. Repeated nonzero heap ownership triggers additional bounded inspection. An owner being nonzero is not by itself a deadlock.

F9 / Mark Glitch stores both log offsets and requests a screenshot through the patched ScreenshotWUPS and FTPiiU. Request and completion times are distinct. A hung renderer may never produce the requested frame. This is not a full RAM dump or a video recording. Emergency reports written to SD after native faults must still be retrieved; a full lockup may prevent any final report.

The recorder's local Unix socket also supports Python/CLI status, connect/disconnect, start/stop, markers, and bounded read-only runtime commands. An explicitly separate renderer action allows limited logged developer A/B tests. It never silently enables CPU fallback as an optimization. Restore experiment settings before measuring normal performance.

## Rebuild the external modules

`patches/manifest.json` pins every base commit and original repository. For each entry:

```sh
git clone REPOSITORY module-source
git -C module-source checkout BASE_COMMIT
git -C module-source apply --check /absolute/path/to/PATCH
git -C module-source apply /absolute/path/to/PATCH
```

Then follow the upstream build/dependency instructions. Preserve each repository's own license; the copied project license here does not relicense external source patches. Cemu's patch is based on the public Cemu-Aroma fork, which already contains its Python control baseline; it is not directly applicable to arbitrary upstream Cemu commits. Native GamePad input additions in that patch are experimental and unnecessary for 3Dii U loading.

The native probes in `probes/` cover ring-buffer behavior, spinlock behavior, and concurrent mapped heap allocation. Their relative includes expect an aggregation directory with `MemoryMappingModule/source` next to a `tests` folder containing the probes. Build against the same WUT headers as the patched module. Do not treat host-only simulation as a hardware concurrency proof.

## Evidence and remaining problems

* Cemu concurrent mapped-heap probe: 30,000 allocation/write/resize-attempt/free operations across three workers, free space restored. Cemu's substituted native heap callbacks cannot establish hardware cache coherence.
* A real Kirby crash was mapped to `dump_thread`: a scheduler-owned queue pointer was reread after validation and became null. The runtime now snapshots the thread and optional pointer fields instead of rereading them. A Cemu stress run completed 297 thread-list requests while frames advanced from 1232 to 4830.
* With the mapped-heap workaround and safer debugger, the user completed two Kirby levels with responsive controls. This is substantially better than earlier slow freezes, but not exhaustive stability testing.
* Kirby's observed shader pack gained 1,545 pixel and 7 vertex states while retaining the old entries; hardware median presentation was about 59.84 FPS. One transient green GamePad flash was still reported. No game shader payload is included here.
* Brain Age hardware follow-up: sampled draw keys identified eight active vertex states missing from the shipped pack. Precompiling those exact hardware-observed sources (plus 70 missing pixel states) restored the professor's hair and glasses on the real Wii U, with the lower screen visible and a 60 FPS capture, without full CPU vertex fallback. The log confirmed the new shipped GPU states were selected. This localizes the observed difference to the runtime-versus-offline compilation/loading path; it does not yet establish the exact compiler defect or prove all scenes correct. Earlier full CPU vertex execution measured about 19–20 FPS; disabling the vertex data cache and CPU attribute conversion did not repair the artifacts. The user clarified that the black unused areas in combined top/bottom 3DS captures were expected; those captures do not establish a lower-screen rendering or capture defect. A vertex-only isolation pack was built but not deployed after that mistaken interpretation was corrected. Game shader payloads stay local. Broader gameplay and performance validation remain open.
* Six recorder integration tests, nine FTP publisher tests, five WUHB descriptor tests and the offscreen builder window check passed. The broad selftest was attempted on Linux, with many portable checks passing. It did **not** pass in full: Windows toolchain-dependent subprocess tests, Windows-only file-lock/share behavior and compiler policy mocks require their supported environment. Early Qt dependency failures were followed by a successful window rerun after installing PySide6. Normal/cold full port regression routes have not been completed for every title.
* Historical Mario Maker builds reached gameplay on hardware but remained slow. Boot fixes are useful compatibility work; they do not establish acceptable performance. Mecha Marathon GPU rendering/performance and random hardware failures also remain open investigations.

No ROMs, compiled game shaders, private memory/log captures, console keys, or game binaries belong in this submission. The Ever Oasis evidence report includes selected gameplay screenshots explicitly authorized by the user, plus bounded performance excerpts. Share those locally only when needed to reproduce with a lawful dump. Reviewers can split the core fixes from the optional experimental debug environment before merging.

Further Brain Age testing reached the professor transformation: hardware logs recorded a missing vertex state taking 3,363 ms to compile, plus multiple pixel compiles. A follow-up pack adds two observed vertex states and 38 pixel states. The right-side horn appearance and transformation stutter require retesting on hardware; compilation is a demonstrated contributor, not proof of the only performance bottleneck.

Brain Age's repeatable 1-Back tutorial crash was reproduced in visible Cemu using the hardware RPX. The guest null call (`blx r1` at ARM `0x0039B928`, LR `0x0039B92C`) followed a failed lookup of `NbackU.bclyt`. A native write watchpoint caught `write_back(Surface&)` overwriting the archive, called from `present()` through `forget_unused()`: after 1,800 idle presents, cache retirement wrote an old colour target into guest memory that the game had reused for its layout archive. This is a renderer cache-lifetime bug, not an invalid original asset or a dropped ARM compatibility range.

Retirement now keeps unsynchronized colour targets rather than forcing an unsolicited writeback. Synchronized colour targets and eligible depth targets still retire; explicit readbacks remain available. This conservative fix preserves GPU-only contents for later reads, but can retain more GPU memory in long sessions; long-session memory/performance testing remains open. It does not establish that every possible stale-cache/readback path is resolved.

Validation: a focused test compiles the actual retirement loop with simulated memory and GPU allocation stubs, verifies that reused archive bytes and unsynchronized GPU targets survive, and checks synchronized/depth retirement, age, active references, and the one-target-per-present limit. On the fixed Cemu run, the same transition loaded `NbackU.bclyt` with valid `CLYT` bytes and its full 736,232-byte archive matched the original byte for byte. The user confirmed the transition worked. The fixed renderer was subsequently used on the real Wii U through Mii selection and handwriting tests. The byte-for-byte archive check was performed in Cemu; extended hardware stability remains unverified. Game archives, generated game code, and private trace captures stay local.

The bounded guest dispatch diagnostic still logs up to 256 bytes only when SP is within the configured main stack, then retains fatal behavior. ABI-local register snapshots can be stale. The native emergency file was empty for the guest fatal; no unconditional null-call bypass was added.

Brain Age handwriting A/B correction: the user's real Wii U test without the dedicated canvas refresh initially looked functional, so we incorrectly concluded that the per-title refresh was unnecessary. The user subsequently noticed that the first strokes appeared late. An instrumented hardware run, with identical probe overhead in both modes, observed the canvas changing before its first upload: refresh off delayed that upload by 20 frames (333,862 microseconds), while subsequent warmed-up updates were same-frame. With the dedicated per-frame refresh enabled, 100 recorded updates across the initial run and a full game relaunch uploaded in the same frame (204–245 microseconds from the probe's change observation to upload completion); the user confirmed immediate drawing after the relaunch. The dedicated Brain Age refresh is therefore retained. This measures renderer texture-update latency, not physical pen-to-display latency. The probe and runtime A/B switch are local diagnostic instrumentation and are removed from the permanent build. Upstream's generic refresh remains enabled in both modes. The earlier claim that the fix was unnecessary on hardware was premature; these measurements support retaining it.

A separate hardware follow-up found displaced Mii face/hair triangles while the selector still ran at 59–60 FPS. Disabling the vertex-data cache did not correct it and was reverted. Logs identified a newly generated vertex state (`0aa98471b417840a`, runtime compilation 655 ms); appending its offline-compiled state plus 49 observed pixel states preserved all previous shader blocks and index mappings. The user reported normal Miis after relaunch, corroborated by a 60 FPS hardware capture. The exact runtime-versus-offline compilation defect remains unproven. The archive-retirement fix is retained alongside these shaders. No compiled game shader payload is included in the repository.

## Reproduce the handwriting comparison

The optional [source patch and procedure](experiments/README.md) reproduce the baseline/enabled hardware experiment without shipping a game payload. The permanent build does not include its extra hashing, measurement logs, or runtime switch. The PC recorder permits the bounded `set inkrefresh 0|1` control only through its explicit renderer action; that command requires the optional instrumented RPX. [The analyzer](analyze_handwriting_latency.py) accepts a session directory or `runtime.log` and reports recorded updates separately from launches. It does not count every update as an independent first-stroke trial.

First-time FTPiiU publication also accepts its specific `553 No such file or directory` response when a new title has no prior activation marker. Other 553 errors still abort publication. The two additional publisher tests cover both cases; the first full Urban Champion deployment exercised the missing-marker case on hardware.

For transfers that must not download payloads a second time, pass `--no-readback` to `tools/ftp_homebrew_dev.py`. This mode issues no `RETR` for uploaded files or the activation marker; it checks remote sizes and still activates the new release last, with rollback of the old marker on activation failure. It does not provide a remote content-hash comparison. The no-readback regression test rejects every attempted download.

## Ever Oasis hardware follow-up

[Rendering fixes, shader compiler diagnosis, 20 to 30 FPS hardware measurements, and screenshots](EVER_OASIS.md) document the latest Ever Oasis work. The report distinguishes measured Garden results from broader performance/stability claims and includes the costs and limits of the depth/stencil path.
