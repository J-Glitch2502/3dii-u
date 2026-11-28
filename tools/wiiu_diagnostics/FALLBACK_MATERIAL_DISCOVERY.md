# Fallback material discovery experiments

The generic fragment shader currently interprets TEV operands, operations and lighting per pixel. Correctness on Cemu does not establish correctness or acceptable cost on GX2. Keep specialized fragment shaders preferred and compare candidate families on real hardware.

## Hardware result so far

In a stationary Planet Robobot file-selection scene, full generic fallback measured 4.2 FPS, bounded stage/light-capacity families measured 4.5 FPS, and a repeat of full fallback measured 4.1 FPS. Both normal-path samples measured about 59 FPS. The normal warmed scene reported zero generic draws. All forced-path samples reported zero CPU vertex draws and no texture uploads. The large instruction-size reduction from bounded families was not a large speedup; that candidate stays opt-in. Material families subsequently measured 48.8 FPS versus 4.2 FPS on the full generic path, with 98.4% of forced draws using a material family. The missing selection arrow and B/Back glyph were fixed separately by replacing weighted texture-coordinate selection with direct selection. An off/on/off hardware comparison reproduced the fault and correction at roughly 47 FPS. Nonfinite unused coordinates are a possible mechanism; they were not directly measured.

The corrected PC compiler comparison and these performance comparisons are separate. The test pack uses CafeGLSL 1.0.0-rc1 in block-uniform mode plus the runtime's `fit_program` corrections. The original experimental packages did not update the production Windows build path; the integration below now makes this step required.

A stationary gameplay ABAB comparison on 2026-11-26 measured material fallback at 33.8 / 33.6 FPS and structural-lighting families at 54.0 / 54.0 FPS. All sampled lit draws matched the new lighting family after settling. Resolution settings were unchanged, and paired screenshots showed no obvious visual loss. This is evidence for this scene, not complete graphics validation or a promised gain in other games.

## Bulk analysis

`inventory_fallback_assets.py` walks the unpacked RomFS through existing recognized container and PICA material parsers. It exports combiner instructions separately from scene configuration. It found 5,485 material blocks / 1,175 unique combiner programs in the local Kirby input. This is not a claim that every runtime render state was discovered. Missing containers and runtime-created materials remain possible.

`analyze_fallback_materials.py` groups fragment logs into material programs, retains provenance, exports seed states, matches sampled draw-trace keys and flags known generic feature gaps. Use `--observed` only for actual runtime records, `--preserved` for catalog coverage snapshots, and `--asset-log` for reconstructed asset-derived states. Empty `models_fs.bin` does not imply no material commands exist: the context-reconstruction step can have no usable scene contexts.

The generic shader does not yet implement cube sampling, procedural textures, exclusion-scissor fragment tests or W-buffer depth writes. A state requiring one is a candidate for investigation, not proof of a particular screenshot's cause. Report unknown live keys rather than silently excluding them.

## Candidates

`fallbackgen.cpp` emits a 336-family stage/light-capacity experiment, or combiner-specialized material families from length-prefixed fragment keys. Material families share the specialized TEV arithmetic emitter while retaining live lighting flags, alpha tests, fog, texture coordinates/wrapping, constants and TEV initial buffers. Procedural-source cases are rejected and retain the previous fallback. Keys omit only values still read dynamically, including unused TEV operands after normalization. Structural-lighting families use the existing specialized lighting emitter and key light count, LUT selection and other compile-time lighting flags; colours, positions and other changing values stay in uniforms. Unsupported generation cases decline the family and retain existing fallback. Feed all distinct lighting states to `--lighting`; the one-seed-per-material analysis output alone can omit lighting configurations.

`build_fallback_pack.py` compiles those sources on the PC with the newer host compiler, applies the shared GPU corrections, checks every program and writes a separate `fallback-shaders.bin`. This avoids pushing the main pack over the 16 MiB resident limit. Programs load lazily from resident bytes. These were originally separate experiments; production integration is described below.

The experimental runtime accepts `set fbfast 0` for full generic fallback, `1` for bounded families, `2` for material families, and `3` for structural-lighting families followed by material/bounded/full fallback if the exact family is absent. Mode 3 is the runtime default; the production builder now generates and packages the required families as described below. Exact specialized shaders remain preferred unless the diagnostic force override is enabled. `set uberall 1` deliberately forces fallback for all shaded draws; restore `set uberall 0` after comparisons. `[fragment-route]` counts actual draw submissions through each path; fragment-miss totals alone do not measure current fallback usage. Switching families invalidates cached missing-state placeholders and last-shader selection.

## Validation

Run `tools/test_uber_families.py`, `tools/test_bounded_uber.py`, `tools/test_material_fallback.py`, `tools/test_uber_shadows.py`, and `python3 -m unittest discover -s tools -p test_fallback_material_analysis.py`. Compiled candidate packs must also pass `tools/gsh_rules.py` before upload. Compare the same stationary scene with screenshot capture outside the timing window. Repeat the baseline and restore the normal path in a `finally` block. Do not turn on continuous screenshots as part of the test.

## Mario Kart 7: wider PC discovery inputs

The first supplemental pack used recorded observations, while the main specialized pack also used asset reconstruction and generated state variants. Reusing those wider inputs produced 1,100 unlit material families. A switchable extension added the 915 absent from the original pack, preserving all 521 existing programs and keys. The combined resident packet has 1,436 entries and is about 11.4 MB.

On a stationary Mario Circuit scene, hardware ABAB testing with forced fallback measured 18.0 / 16.3 FPS without the extension and 59.5 / 58.7 FPS with it. GPU drawing fell from about 52–58 ms to 9.6 ms. No resolution control was changed. Paired old/new/normal screenshots showed no obvious visual loss; this is a short scene check, not exhaustive graphics validation. The repeated new sample covered about 99.2% of generic draws with material/lighting families.

`build_fallback_pack.py --exclude-pack ORIGINAL --material-extension --materials INPUT` compiles additional families in the shared extension namespace; already present raw or extension keys are excluded. Merge with `fallback_pack.py merge`, which checks that original shader blocks are unchanged. `set fbextra 0|1` compares extension selection without replacing files or bypassing exact specialized shaders. `[fragment-route] extension` counts actual new-family draws and is a subset of `material`.

## Production integration, 28 November 2026

The Pokémon investigation exposed the still-missing packaging integration above. The common build now invokes `production_fallbacks.integrate` and title packaging requires its optional pack. See [the build and release checklist](BUILD_RELEASE_CHECKLIST.md) for current verification status; experimental hardware results above do not automatically validate a newly packaged game.
