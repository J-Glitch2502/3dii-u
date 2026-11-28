# Mario Maker draft #13 reconciliation

PR #13 (`c6e12e65fb42b566d98110d37c1186b0cf1d942e`) is superseded by PR #54.
Its useful behavior was compared with the current source before closing it;
the old commit need not be applied again.

| Draft behavior | Current implementation |
| --- | --- |
| Extended application memory from the executable header | `loader.cpp` reads the header system mode into `g_game.system_mode` / `app_memory`; mode 3 supplies 80 MiB; configuration memory and kernel commit limits use it. |
| Extdata open/read scheduling | `services/fs.cpp` retains the archive 6/7 classification, asynchronous read delay and OpenFile delay. |
| Verified Mario Maker safe translation | `build3ds.py` retains the g_ajhe executable SHA-256 gate for safe translation, preserving nonlocal return sites. |
| Direct quota retry without reentering a failed hooked path | `fsa_direct_flush_quota(path, result)` retains explicit result propagation; save flushing uses the ordinary route only when direct routing was not selected. |

The new hardware startup failure is separate: a throwing host DeleteFile call
terminated a build with C++ exceptions disabled. The error-code overload now
returns the existing guest error result and logs failures. Real Wii U testing
handled 221 course-slot delete errors, continued startup, reached the title/editor
and wrote saves successfully. The underlying deletion I/O errors are unresolved;
this is containment of a verified fatal path, not an SD repair claim.

See [Mario Maker measurements and limitations](MARIO_MAKER_PERFORMANCE.md),
[bounded diagnostics](COMPREHENSIVE_CAPTURE.md), and
[workflow documentation](README.md). Only reusable source, synthetic tests and
summarized observations are published. Game binaries, ROMs, generated game C,
shader programs, private saves and raw console recordings are excluded.
