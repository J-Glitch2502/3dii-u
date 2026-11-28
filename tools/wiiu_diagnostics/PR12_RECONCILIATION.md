# PR #12 reconciliation with the consolidated PR

Reviewed PR #12 (`6bf143538630481970fdb61dd12f3644b2066a08`) against upstream
`main` (`479b55a`) and this branch on 2026-11-19. The useful compatibility and
builder changes were already present in the consolidated branch. No historical
commit was blindly cherry-picked over newer upstream code.

| PR #12 change | Current disposition |
|---|---|
| Per-title builder language selector, persisted choice and CLI option | Retained in `builder/gui.py`, `builder/ui/Main.qml`, and `builder/pipeline.py`. |
| Locale mapping, including Chinese script/region handling | Retained in `builder/pipeline.py`. |
| Language content file and validated environment override | Retained in `cfg.cpp`, integrated with upstream console/settings language selection. |
| Prefer USA whenever both Japan and USA SMDH bits are set | Superseded: keep upstream's complete region mask and `pick_region`/`pick_language` rules. A Japanese choice may correctly select Japan; English selects an eligible English region. Do not reinstate a blanket USA preference. |
| Cave Story verified continuation addresses | All four entries retained in `games/g_acve/functions.txt`. |
| Cave Story register-preserving compilation | Retained, scoped to the checked executable SHA-256 in `tools/win/build3ds.py`. |
| Cave Story native-sized render targets | Retained for the tested title ID in `gx2_backend.cpp`. Clean enlarged/HD rendering is still not established. |
| Brain Age verified continuations and safe compilation ranges | All three entries and the hash-scoped ranges retained in `games/g_asre/functions.txt` and `tools/win/build3ds.py`. |
| Brain Age A4 handwriting canvas refresh | Retained for the tested title/texture shape. Hardware testing later demonstrated a first-stroke delay without it; it is not an emulator-only workaround. |
| Early startup stage reports and opt-in GPU-pump timing | Retained in `frontend_wiiu.cpp` and `gx2_backend.cpp`; later diagnostics build on these. |
| Wine launcher | Retained as `tools/linux/build-with-wine.sh`; native incremental builds are additionally available. |
| CPU test harness links libm | Retained in `third_party/3dsrecomp/tools/test_cpu.py`. |
| Extra verified function entries are merged with discovered entries | Retained in `functions_list()`; newer title-scoped entries are also preserved. |

## Relationship to newer fixes

The Brain Age renderer archive overwrite was a separate cache-lifetime defect.
Its repair and regression test remain alongside the older continuation fixes.
The measured hardware handwriting comparison and the Mii/professor shader
workflow are documented in the main diagnostics README and PR description.

The same consolidated branch includes Ever Oasis depth/stencil and shader
control-flow fixes, MH4U Y2R/native geometry fixes, the hash-checked Mario 3D Land
mod preparation, and the reproducible incremental loading/debugger tools.
Kirby's newly tested movie optimization and presentation-gap logging are
explained in `KIRBY_VIDEO_PACING.md`.

PR #12 remains useful historical evidence. This consolidated PR supersedes its
retained implementation changes; merging both independently is unnecessary.
External Aroma/Cemu modifications are exported source patches, not binaries or
changes automatically applied to their upstream repositories. Their optional
nature, licenses, pinned revisions and hardware limitations remain documented.

## Checks at readiness

- Runtime language/region tests and six builder locale mappings passed.
- Builder compiler-policy test passed its ten scenarios with only the Windows
  platform gate mocked after import; this is not a native Windows end-to-end test.
- Surface-retirement, Y2R coefficient/DMA, IF-stack, and 1,792 VFP immediate
  cases passed.
- 23 FTP/shader-cache/native-link/ROMFS-descriptor unit tests and all eight
  dual-channel recorder integration tests passed.
- The new CPU byte-operation differential test passed 631,072 packed-word
  cases and is registered in the normal selftest runner.
- Python syntax, Wine shell syntax and `git diff --check` passed.
- The changed PowerPC runtime and all 101 Kirby generated-code units rebuilt
  and linked; the immutable hardware release was tested as described in the
  Kirby report. No full all-title regression or native Windows GUI test is
  claimed. The earlier broad Linux selftest limitations remain documented.
