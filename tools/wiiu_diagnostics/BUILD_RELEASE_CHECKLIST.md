# Build and release checklist

This is a working checklist, not a declaration that a release is tested. Update
it when a required step, failure, or verification result changes. Keep generated
logs, screenshots, game data and console keys in the local build directory.

## Required shader build

- [x] Retain all catalog observations; run the coverage check before compilation.
- [x] Keep exact specialized shaders preferred in normal play.
- [x] Compile the 16 corrected general fallback families with CafeGLSL 1.0.0-rc1,
  block uniforms and the shared GPU instruction corrections.
- [x] Generate all 336 bounded stage/light-capacity fallback families.
- [x] Generate material and structural-lighting families from the discovered
  fragment states. Report optimization limits; do not remove exact shaders.
- [x] Enforce the runtime's 16 MiB optional-pack budget and 2,048-key limit.
- [x] Copy `fallback-shaders.bin` into both Homebrew and full WUP content.
  Shader-bearing native updates must include it too.
- [x] Verify this integrated path in the packaged Windows executable, rather
  than assuming a successful manual build verifies the GUI distribution.
- [x] Refresh the friend ZIP and upload link after these checks pass.
  Source `0a30736`; new link: https://buzzheavier.com/mkhbs0jow1j4.

The packaged Windows executable passed a real compiler smoke test under Steam
Proton: 16 general + 336 bounded + two material + one lighting family, unchanged
specialized/vertex shader index, and a validated final pack. Native generation,
material arithmetic, shadow semantics and title packaging checks also passed.
Hardware visual/performance validation is still pending.

Native source setup: run `python tools/setup_fallback_compiler.py` once.
Windows release packaging installs the pinned Windows compiler automatically.
The old compiler remains responsible for existing specialized shaders; it must
not substitute for the separate optimized fallback compiler.

## Native executable and WUP

- [x] Discover CRO entries in the same address space used by the Wii U
  recompiler; reject desktop-slot entry lists before compiling.
- [x] Check native update identity, matching base assets and shader contract.
- [x] Verify rebuilt shader-bearing update retains every base game asset link,
  the corrected Pokémon executable, and all three shader files.
- [ ] Test Pokémon normal / forced / normal rendering in the same scene on Wii U.
- [ ] Record actual material/lighting route counts, frame times and paired
  screenshots. A queued debug command is not evidence it was applied.
- [ ] Keep generation, package, upload, installation and hardware validation
  as separate statuses in each delivery report.

## Before sharing

- [ ] Compare the packaged engine with the chosen source snapshot; inspect
  compiler versions and generated helper capabilities too.
- [ ] Verify the GUI version, output options, public assets and required tools.
- [ ] No user dumps, keys, game saves, private network configuration or logs.
- [ ] Include setup instructions and list untested hardware behavior honestly.

## Current investigation: Pokémon X, 28 November 2026

Observed on Wii U: forced fallback rendered a yellow background and incorrectly
coloured characters at about 3 FPS. The recording is
`2026-11-28_02-34-57-850915`; screenshot 002 contains the visible fault.
The runtime reported forced fallback enabled, no material/lighting family draws,
and no material pack was present in the delivered Pokémon title. The default
shader build did not invoke the separately implemented optimized fallback step.
The newer fallback compiler and fitted general shaders were also absent from
that path. This was a build integration omission, not evidence that every
fallback graphics feature is supported once the missing pack is added.

Changes in progress: make fallback generation part of `build_shaders`, include
fallback generation in the shipped shader generator, supply the pinned separate
compiler, preserve all specialized programs/index entries, and reject packaging
that omits the optimized pack. The hardware correction remains unverified until
the rebuilt package is installed and compared. The complete update is uploaded
to `install/Pokemon_X_Optimized_Shader_Update_2026-11-28` on the SD card. It
retains the corrected executable, 16 corrected general families, 336 bounded
families, 368 material families and 357 lighting families. Remaining optional
optimizations are counted in `fallback-build-report.json`; exact shader
coverage and the index are preserved.

Local build evidence: `3Dii-U-build-work/pokemon-fallback-20261128/` contains the
compiler logs, input states, program validation reports and delivery results.

## Current delivery queue

- [x] Pokémon X complete native update uploaded to the SD install folder.
- [ ] Pokémon real-hardware forced/normal visual comparison after installation.
- [ ] Mario Kart 7 native update: executable + complete new shaders, upload by FTP.
- [ ] Monster Hunter 4 Ultimate: rebuild the full WUP including optimized shaders;
  retain on the PC, do not upload to the Wii U.
- [x] Keep bulk discovery, shared catalog staging, variant generation and coverage
  checking in the standard shader build and PR.
- [ ] Investigate Pokémon-specific asset material discovery: the earlier scan
  produced no own `models_fs.bin` states. Shared catalog coverage and optimized
  generic families are not proof its runtime materials were all discovered.

The new native network keyboard/chat overlay still needs its previously planned
hardware validation; Cemu menu tests alone are not sufficient. Keep that testing
separate from the shader build correction.

### Catalog identity and optimization priority

Native builds now resolve the canonical observation catalog through `program_id`
in `info.json` and compatibility profiles. A version suffix in an output folder
must not suppress the game's own records. Unknown profiles may fall back to the
unversioned build name. The GUI already assigns the canonical observation slug.
The real generator regression test deliberately creates borrowed inputs first
and verifies that optimization inputs still put own-game states first, retaining
both. Specialized shader generation retains its complete input set.

Mario Kart's corrected scan restored seven portable vertex records and produced
972 new model/template states using 102 logged configurations. These figures
are evidence of a working bulk pass, not exhaustive gameplay coverage.
