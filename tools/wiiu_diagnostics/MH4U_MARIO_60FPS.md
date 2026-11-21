# MH4U shader registers and Mario 3D Land 60 FPS boot support

The initial port checks below are **Cemu boot/functionality results**. Later real-Wii-U movie validation is explicitly identified in its own section. The Mario game is **Super Mario 3D Land**, not Super Mario 3D World.

## Monster Hunter 4 Ultimate (USA)

The ARM-to-PowerPC compilation completed, but offline shader validation rejected vertex shader 266: CafeGLSL used **54 registers**, while the renderer's vertex allocation remains **48**.

The PICA relative-uniform helper previously selected between `vec4(1)` and a dynamically indexed buffer fetch. Rewriting it to fetch `f[min(u,95)]` unconditionally, then select constant one for indices 96–127, avoids excess register pressure. Indices are wrapped to seven bits after the existing signed-offset check. The fetched index is always valid. This changes neither the returned uniform nor the constant-one behavior; shader work was not removed, and the register limit was not relaxed.

Compiler comparison on the same failing source:

| Form | Registers |
|---|---:|
| Conditional buffer fetch | 54 |
| Equivalent clamped fetch before selection | 32 |

The complete rebuilt pack contains **604 vertex + 60 pixel shaders**. Maximum register counts are **37 vertex / 27 pixel**; all 664 programs pass `tools/gsh_rules.py`. The shader generator revision is raised to **5** so future runtime builds invalidate cached programs generated before this change.

The actual WUHB booted in custom Cemu, completed first-run save formatting and SD extra-data creation, and reached the main menu. The user confirmed the game was running. An apparent pause during the opening sequence was not a confirmed crash. Gameplay coverage, real-hardware rendering and sustained performance remain untested.

## MH4U opening movie: yellow-screen fix and conversion work

A later hardware test found the opening movie displayed solid yellow. The same defect reproduced in Cemu, while Azahar displayed the movie. This was a **3Dii U Y2R service defect**, not a shader-pack change or a missing movie asset.

A Python-controlled Cemu breakpoint traced the setup IPC sequence: MH4U requests `GetStandardCoefficient` (`0x21`, index 0), then passes the returned values to `SetCoefficient` (`0x1e`). The service lacked the query and returned success without its four coefficient words. The next request contained leftover command-buffer values such as `0x4000` instead of the standard table, causing conversion to saturate to yellow.

The service now implements both `GetStandardCoefficient` and `GetCoefficient`, packing signed 16-bit pairs into four little-endian IPC words independently of host byte order. A table query does not alter the active converter. Invalid standard-table indices return CAM's invalid-enum result. The actual rebuilt WUHB displayed the movie normally in Cemu; the user then confirmed normal video playback on **real Wii U** after an RPX-only FTP update (53,578,243 bytes, no payload readback). A hardware capture showed **27 FPS**, so this establishes the colour fix, not full-speed playback.

The video path also performed a separate guest-memory copy for every output pixel. The optimization packs each 8×8 tile before writing it and splits batches at DMA transfer-unit boundaries, preserving gaps and memory layout. For MH4U's 512×240 RGB24 conversion, this reduces calls from **122,880 per frame to 1,920**. No pixels or frames are discarded.

Validation:

- `tools/test_y2r_coefficients.py` compiles the actual complete service with kernel stubs: standard-table queries, signed IPC packing, Set/Get round trips, invalid enums, neutral black/white conversion and batched DMA boundary/gap checks pass. The old service fails the new query test.
- Differential native runs compared **640 combinations** of input/output formats, rotation, tiled/linear output and DMA units/gaps: all **2,621,440 output bytes** matched before/after batching.
- A synthetic 512×240 RGB24 tiled conversion benchmark measured **1.433–1.487 ms/frame before** versus **0.755–0.803 ms/frame after**, approximately **1.8–1.9× faster on the host CPU**. These are not Wii U timings or an end-to-end FPS claim.
- The optimized runtime linked and booted in Cemu. On real Wii U, a capture showed 29 FPS and most three-second counter windows were approximately 29.5–30.4 FPS during the movie. The earlier 27 FPS capture belongs to the colour-fix build. Gameplay is a separate workload and remains slower.

The runtime colour fix and batching were uploaded as separate immutable RPX-only releases so their results can be distinguished. Existing ROMFS assets, game code and shader packs remain unchanged. Debugger on/off was briefly tried by the user without an apparent improvement; this is not a controlled plugin-performance study.

## MH4U gameplay: native geometry shaders were silently disconnected

Real-hardware town/dialogue captures showed approximately 33–35 FPS with normal graphics. A warm counter interval had no new texture uploads or fragment misses, and approximately 345 CPU geometry-shader draws / 633 input vertices per flip. Release-build CPU timers are disabled: the printed zero milliseconds and zero-percent GPU-thread busy fields are not performance measurements.

The optional generated C geometry-shader tables were present in the executable, but GCC gave the runtime's weak declarations anonymous-namespace names. They did not resolve to the generated `gs_native_table` / `gs_native_count`, so the interpreter remained active. Move the C ABI declarations outside the anonymous namespace and validate linked symbols before producing the RPX. Games without generated tables retain the interpreter fallback.

Validation and tooling:

- The actual PowerPC build's symbol map now has the two defined C symbols and no namespace-mangled table references. The previous executable had unresolved namespace-mangled weak references.
- Two captured geometry programs translated successfully; a third unsupported program stays interpreted. The captured programs and generated C remain private build inputs, not repository assets.
- The differential test now runs natively on Linux, matches code and used-swizzle hashes before comparison, randomizes shader input registers, and checks dependent programs' output lanes. Its shader-generator stub now hashes bytes instead of returning zero, which previously reused the wrong decoded program when different programs had equal lengths.
- 14,000 randomized runs / 28,000 emitted primitives matched the interpreter bit for bit. This validates shader arithmetic/output on the host; the corrected hardware run logs confirm native execution. A first interval reached 43.2 FPS, but later intervals were 34.7–37 FPS and the user still reported stutter. These are not proof of a sustained speedup.
- The PC recorder allows the read-only `stats` command in its existing recorded runtime channel. The observed-cache append tool accepts SHDC revisions 2, 4 and 5; a separately prepared append-only pack has not been deployed as part of this RPX test.

The first native-program update still had the linkage defect and showed no clear improvement. The corrected executable is an RPX-only update; ROMFS assets and the existing shader pack are retained. No full-speed gameplay claim is made yet. Short GPU-timestamp windows attributed about 13.2 ms to the main target and 11 ms to the present interval; these buckets can include submission gaps and presentation waits and must not be read as isolated shader execution times. A temporary lower-level resolution test produced no useful improvement and was restored. The user confirmed 480p was already selected. A subsequent RPX adds opt-in `cputime` counters for renderer stages and geometry work, disabled by default; it does not enable interrupt sampling or shader disassembly.

## MH4U follow-up: combined GPU programs and logging-induced audio gaps

The live CPU sample identified 8.40 ms/flip in CPU geometry processing in one gameplay interval, including 5.89 ms of vertex interpretation and 1.71 ms of geometry execution. Capture the missing `GSX` records, convert them with the existing shader generator, and compile the combined vertex/geometry programs. This produced eleven programs with a maximum of 17 vertex registers; nine new vertex keys were appended to the original pack. The complete merged pack stays within 37 vertex / 27 pixel registers. It adds **zero pixel keys**, preserves every original program block and key mapping, and totals 2,055,744 bytes including its index. `tools/linux/append_shader_pack.py` reproduces the append while validating program indices and the original prefix.

On real hardware, startup reports 1,521 vertex keys rather than 1,512. A warmed interval recorded 51.8 FPS and CPU geometry time of 0.80 ms/flip, with approximately five residual geometry draws rather than hundreds. The captured town/dialogue frame remains visually intact and shows 52 FPS; a comparable earlier frame showed 33 FPS. These observations cover this scene, not every gameplay area. The user initially perceived a possible regression and then clarified that audio interruptions were their main indication of stutter.

The audio log continued reporting recurring approximately 50 ms stalls. A twelve-second disconnect of both PC logging connections nearly eliminated the gap reports, without changing average rendering rate. A subsequent channel-isolation test used the same running game, no FTP transfers and no screenshots: runtime-only had one reported audio underrun in its twelve-second phase, while the separate Aroma-only phase had 21. This implicates high-volume Aroma streaming in this setup; it does not identify which module or transport operation causes the delay.

The PC recorder now has a **Stream Aroma trace** checkbox and `--control aroma --note on|off`. Pausing it leaves the runtime debugger connected and records into the same session; Connect respects the selection and the preference survives restart. The status explicitly identifies a deliberate pause. Re-enabling it remains available for module/crash investigations; system trace output is not captured while paused.

With Aroma streaming paused, two consecutive twenty-second gameplay windows recorded **zero audio underruns**, at 52.8 and 53.0 FPS. The user subsequently confirmed the audio improvement held much better. CPU/GPU timing switches were off, and no screenshot or FTP transfer ran during these measurements. The current hardware setup retains the nine-key GPU pack and lightweight runtime recording. Further frame-rate optimization remains open.

Reproduction workflow (using the user's own captured shader data):

1. Fetch `shader_misses.bin` from the game's save/log directory after reaching the affected scene. Keep game-derived records private.
2. Use `tools/misses_to_log.py` to make shader-generator input; `GSX` records describe the paired stages and their runtime arguments.
3. Run the native shader generator with the captured records and compile the resulting GLSL with the project's trusted CafeGLSL toolchain. The normal register/program validation remains required.
4. Append the compiled pack with `python3 tools/linux/append_shader_pack.py ORIGINAL EXTRA NEW_OUTPUT`; retain the generated matching GSH/SHDX pair. Never replace a running title's pack.
5. Publish the RPX and changed shader pair with `tools/ftp_homebrew_dev.py ... --content NEW_CONTENT --release UNIQUE_ID --no-readback` while at the Wii U Menu. Existing assets and saves stay in the base WUHB.
6. Relaunch, discard the first cumulative `stats` interval, and measure warmed intervals. Enable `cputime` only for short diagnostic windows and turn it off afterward. Compare audio gaps with Aroma streaming on/off independently of screenshots, transfers and shader compilation.

## Super Mario 3D Land: upstream 60 FPS mod

Credit: [AlonBorodAHub/mario-3d-land-60fps](https://github.com/AlonBorodAHub/mario-3d-land-60fps), tested commit `eab11f6cc03c9df6ba06cd56af1ac28e7cf64a09`. This is an experimental USA Rev 1 interpolation mod targeting 60 Hz render updates while retaining 30 Hz gameplay updates. This PR supplies adaptation tooling and recompiler support, **not the upstream patch payload, game code or assets**.

Simply applying its IPS before recompiling did not boot: replaced prologues stopped some entries from being discovered, branches targeted the original text segment's padding, and the initialized interpolation cache was outside the original initialized-data section. The first Cemu attempt reported no recompiled code at `0x003187b4`.

`tools/prepare_mario_3d_land_60fps.py` handles the supported, hash-checked revision:

1. Verify original USA Rev 1 code/header and the separately supplied IPS against known SHA-256 values. Other revisions fail before output is created.
2. Apply the supplied IPS using the existing builder helper.
3. Extend executable text to the end of its existing final page (`0x003a2000`), including the injected hook island.
4. Expand initialized data through `0x004320c0` so the mod's initialized private cache is loaded instead of zeroed as BSS. Reduce BSS by the same byte count; total data+BSS footprint remains unchanged.
5. Seed the replaced hook entries explicitly and regenerate `info.json` from the adjusted header. Existing ROMFS assets remain unchanged.
6. Recompile the prepared ARM program normally. The decoder now lowers the mod's VFP modified-immediate constant moves; these are VFPv3 instructions used by an emulator-targeted mod, not an assertion that retail ARM11 supports VFPv3.

Example, after extracting your own matching game and obtaining the upstream patch separately:

```sh
python tools/prepare_mario_3d_land_60fps.py \
  /path/to/unmodified/extracted/g_aree \
  /path/to/mario-3d-land-60fps/patch/code.ips \
  /path/to/new/prepared/g_aree_60fps
```

Use that new extracted-game directory as the input to the normal builder/native build workflow. Recompile the code; an IPS alone cannot patch already generated PowerPC functions. Deploy the RPX with the prepared `code.bin` and `exheader.bin`, plus a matched shader GSH/index pair when changed. The existing adjacent `.wuhb.dev` release mechanism can carry these files while retaining the original WUHB's assets, icon and saves. It requires the PR's modified RPX loading module.

### Validation

- The portable preparation tool reproduced the locally tested code, header and `info.json` byte for byte.
- `tools/test_vfp_immediate.py`: **1,792** single/double immediate/register combinations checked against independent IEEE bit expansion, plus reserved-encoding rejection. Included in `tools/selftest.py`.
- Corrected Mario WUHB reached its title screen without the prior missing-code error; all **4,274** compiled shader programs passed the GPU rule checker.
- Read-only Cemu debugger samples of the mod's own counters confirmed **two render updates per simulation update**. Most sampled five-second title-screen intervals were approximately **59–60 / 29–30 Hz**; one interval dipped to approximately **52 / 26 Hz**. This does not prove every presented frame is distinct, full-game 60 FPS, or hardware performance.
- The user started gameplay and reported that it worked correctly. The counters above were not a controlled gameplay benchmark.
- The IF-stack and command-list regression checks also passed. `git diff --check` passed.
- The final cache-version-only RPX refresh is separate from these initial full-WUHB checks; it does not change game translation or shader arithmetic.

No ROMs, game-derived shader source/binaries, generated game C, keys, memory dumps or raw private session logs are included in this report or PR.
