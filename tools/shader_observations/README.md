# Portable shader observations and coverage baselines

This tracked catalog preserves the portable shader inputs previously shipped in
the Windows Builder. Source archives must contain them too; the ignored local
`games/` folder is optional and can contain private dumps.

The initial inventory contains 449 data files. Fragment `.bin` files hold render
state descriptions only. `.vsf` files identify vertex programs by fingerprints;
`.vsr` files describe where to reconstruct programs from the user's own dump.
They contain no captured vertex program words, game assets, keys, saves, network
addresses or raw console logs. The observed-GLSL JSON catalog is subject to its
existing generator-fingerprint check.

`confirmed.bin`, `models_fs.bin`, `variants.bin`, `neighborhood_fs.bin` and
`preserved_*` records include predictions or prior build coverage. They must not
be represented as gameplay observations. In particular, Planet Robobot's
`preserved_fs.bin` retains the union used to audit its older pack and the
November 25 update; it does not claim every state was exercised by Kirby.

## Checks

`observation_catalog.validate(root)` verifies every inventoried file's size and
SHA-256 before shader preparation and before/after Windows engine packaging.
Missing or changed data fails the operation. An intentional catalog update must
review the changed states and update `manifest.json`; do not regenerate the
inventory merely to hide a missing file.

The build stages title-specific records without overwriting private learned
files. Prior predictions are staged under `preserved_` names so they are compiled
but not used as new observations to generate further predictions. Shared
fragment observations are also available to source builds with no local game
directories. Modified-code and deliberately cold builds retain their explicit
learned-data exclusion.

After generation, `shader_coverage.check` checks every supplied fragment state
against the generated index, allowing exact or canonical-equivalent lookups.
It rejects missing mappings, duplicate keys and out-of-range program indices
before CafeGLSL compilation and writes `shader-coverage-report.json`.

Run `python tools/test_observation_catalog.py` and
`python tools/test_shader_coverage.py`. These include an incomplete source-tree
regression and deliberately removed input/mapping tests.

These checks preserve supplied coverage. They cannot prove that an unobserved
runtime state was predicted, or establish visual correctness or performance
throughout a game. Those still require fresh-cache hardware testing.
