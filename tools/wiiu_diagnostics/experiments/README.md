# Optional Brain Age handwriting latency experiment

This source patch reproduces the hardware A/B measurement without shipping game data or adding profiling work to the permanent build. It is separate from the external-module patches in `../patches/manifest.json`.

The patch applies to `port/wiiu/gx2_backend.cpp` at source SHA-256 `29adcbc1efe17a55b2043e9351ed4d89a102c95e7d514ef409c6627bc4661494`. Use `git apply --check tools/wiiu_diagnostics/experiments/handwriting_latency.patch` first; an incompatible checkout requires review rather than a forced application.

1. Apply the patch, rebuild the RPX, and publish it as a new immutable dev release with its matched shader pair. Use the workflow in the parent README; keep the original release available for rollback.
2. Record both hardware log channels before launching. The experiment starts with the dedicated Brain Age refresh **off**; upstream's generic refresh remains enabled.
3. Start a handwriting round and write the first answer. The probe checks the 512x256 A4 canvas once per presented frame but does not mark it dirty in baseline mode. It logs when the bytes first differ from the cached texture and when that texture is uploaded.
4. Enable the dedicated check through the explicit recorder action: `python recorder.py --control renderer --note "set inkrefresh 1"`. Check the runtime acknowledgement. A full title restart resets this diagnostic switch to **off**.
5. Repeat first strokes after idle periods and on a new game launch, re-enabling the switch after each launch. The same probing hash runs in both modes. A continuously drawn stroke produces many updates; those are not independent first-stroke trials.
6. Summarize the session: `python tools/wiiu_diagnostics/analyze_handwriting_latency.py /path/to/session --output /path/to/result.json`.
7. Reverse the patch with `git apply -R tools/wiiu_diagnostics/experiments/handwriting_latency.patch`, rebuild, and publish the normal RPX. The permanent build retains the handwriting refresh and archive-retirement fix, without the probe or its runtime switch.

The probe logs at most 160 uploaded changes per launch. Its full-canvas hash adds measurement overhead in both modes. The timing starts when the renderer observes changed canvas bytes and ends after its upload call; it excludes pen sampling, game processing before that observation, GPU completion, display scanout, and panel latency. Zero delayed frames means same-frame texture upload, not zero end-to-end input latency. Hash collisions or changes between samples can be missed; a camera is needed for physical pen-to-display measurements.

Observed hardware comparison: baseline first update waited 20 frames / 333.862 ms. With the dedicated check enabled, 100 recorded updates across two launches uploaded in the same frame, 0.204–0.245 ms after observation. The user confirmed immediate drawing after the full relaunch. These results justify retaining the refresh for this tested executable, not a blanket claim about every title or all drawing delays.
