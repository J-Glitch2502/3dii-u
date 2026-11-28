# Wii U Session Recorder

Open **Wii U Session Recorder** from the KDE application menu or run `python3 recorder.py --console YOUR_WII_U_IP` from this folder. Enable TCP logging in debug Aroma, then click **Connect**. You can connect before launching a game or rebooting the console. The recorder retries automatically while connected. Click **Disconnect** to stop listening. You can leave a recording running across a reconnect or stop it separately.

**F8** starts or stops recording. **F9** marks a graphics glitch. Both shortcuts work while the window is unfocused in KDE on X11. The buttons work without global shortcuts. Run with `--no-hotkeys` to leave those keys available to other apps.

Sessions default to `~/Wii U/Logs`; select a different location with `--output` or `WIIU_LOG_DIR`. Each session contains `console.log` for the raw Aroma stream and `events.jsonl` for start, stop, reconnect, and graphics markers. Markers have a PC timestamp and a byte offset into the log. Aroma can resend its retained startup log when reconnecting. A graphics marker records the moment you pressed the key. With the recorder build of ScreenshotWUPS active in debug Aroma and FTPiiU enabled, it requests the next available GamePad frame and saves a PNG in the same session folder. The PNG name includes the marker number and local time. The `graphics_glitch` event contains the marker timestamp, byte offset and PNG filename. The paired `graphics_screenshot` event reports success or failure and adds the PC completion time. A missing frame or FTP connection leaves the original marker intact. Closely spaced requests are processed in order and may capture later frames.

The recorder uses FTPiiU to create a request under `sd:/wiiu/screenshots/recorder/` and fetch the finished PNG. The console writes a temporary file and renames it after encoding so the PC only downloads completed PNGs. A frozen game may stop producing new frames. Run with `--no-screenshots` to record markers and logs without requesting images.

From another terminal, while the recorder is open:

```bash
python3 recorder.py --control status
python3 recorder.py --control connect
python3 recorder.py --control start
python3 recorder.py --control mark --note "Mecha Marathon floor"
python3 recorder.py --control stop
python3 recorder.py --control disconnect
```

Python can call the same local, user-only Unix socket with `from recorder import control` and `control("status")`. Status returns the current or last session path. The recorder needs Python 3 and Tk. Global shortcuts also need python-xlib and X11.

## Optional 3Dii U draw trace

A trace-enabled 3Dii U RPX reads an empty `drawtrace` file in its `/vol/save/common` directory at startup. The runtime samples one frame per 30 presents and reports up to 32 distinct draw states through Aroma TCP logging. Start recording before launching the title. Press F9 when the glitch appears and keep recording for at least a second afterward. That retains a sample on either side of the marker when the game is still presenting.

The trace identifies shaders by their vertex and fragment keys. `ps=7ffffffe` means a shader built at runtime. Values `ffffffff` through `fffffffc` identify the simple, generic, lit generic, and textured fallbacks, respectively. Other small numbers index the precompiled shader table. A zero vertex key indicates CPU vertex processing. The `flags` field includes W-buffer mode (bit 0), depth test (bit 1), depth write (bit 2), stencil (bit 3), alpha test (bit 4), blending (bit 5), early depth (bit 6), depth comparison (bits 8–15), and depth precision (bits 16 and up). `depth` logs the scale and offset as IEEE 754 hex words. A sample can omit draws if it hits the capacity limit.

A diagnostic build can also load `extra-pixel-shaders.bin` from the same save directory. A `ps=40000000`–`400003ff` entry comes from that additional precompiled pack. Removing the file with the title closed restores the original shader selection on the next launch.

## Shared Aroma and runtime capture

Connect starts both Aroma's syslog receiver and the read-only game runtime client. Runtime shows "waiting for game" on the Wii U Menu or with builds that do not expose the opt-in TCP console. It reconnects automatically when the game starts. Disconnect stops both receivers.

Record starts one session folder containing console.log, runtime.log, and events.jsonl. Stop closes both logs together. Reconnection events, runtime commands, and markers share ISO timestamps and byte offsets for both files. Existing GamePad screenshots remain attached to Mark Glitch/F9 and are stored as timestamped PNGs beside the logs. The optional **Capture GamePad continuously (best effort)** checkbox requests an image as soon as the previous request completes while recording. It starts off by default.

The runtime client requests status and the mapped heap header about every three seconds while recording, and thread snapshots about every thirty seconds. A repeatedly observed nonzero owner causes an additional thread listing and bounded owner-memory snapshot. This is an observation for investigation, not an automatic deadlock diagnosis. When not recording it keeps the connection alive without requesting unsaved automatic snapshots. An attached runtime client prevents the existing game's watchdog from forcing its fatal screen; it cannot prevent a kernel failure.

Python control uses the existing socket/API:

```sh
python3 recorder.py --control connect
python3 recorder.py --control start
python3 recorder.py --control runtime --note "ps"
python3 recorder.py --control runtime --note "mem 80000000 80"
python3 recorder.py --control mark --note "Graphics glitch here"
python3 recorder.py --control stop
python3 recorder.py --control disconnect
```

The integrated runtime client accepts only status, thread-list and bounded memory-read commands. It does not send input, renderer modifications, restart commands, filesystem probes or save-reset commands. The game must be rebuilt with the lightweight opt-in console support and have SD marker wiiu/3dii-u/debug/console-enable. Heavy RECOMP_MEASURE profiling is not required.

Validation: real TCP integration tests cover both channels in one folder, markers with both offsets, start/stop and session rotation, runtime reconnect, persistent-owner snapshot requests, operation with an unavailable runtime channel, and disconnect after the Aroma receiver has stopped. A live Cemu test also captured native thread addresses/names and read the running build stamp through the integrated receiver.

## Force fallback rendering

The unchecked-by-default **Force fallback rendering** checkbox routes fragment draws through the generic fallback for visual comparisons. It is a temporary diagnostic switch, can be very slow, and does not persist as a preference. It does not enable screenshots. Compare the same scene with the switch on and off; restore it to off after testing. A queued request is not proof that the console applied it: confirm the runtime acknowledgement.

Python can request the same switch with `control("fallback-rendering", "on")` or `control("fallback-rendering", "off")`. The console must be connected and running a compatible runtime. This controls fragment shading; it does not force CPU vertex rendering.

## Explicit renderer experiments

The `runtime` action stays read-only, including the one-shot `stats` performance report. A separate `renderer` action accepts only `set cpuvs 0|1`, `set vtxcache 0|1`, `set cpuattr 0..15`, `set drawtrace 0|1`, and `set trace 0..256`, `set gputime 0|1`, `set cputime 0|1`, and `set toplevel 0..2`. Each request is logged before the runtime sends it. These temporary live switches require a compatible 3Dii U runtime; they are for developer A/B tests, not an automatic graphics repair. Restore `cpuvs 0` and `vtxcache 1` after testing unless a deliberately chosen experiment requires otherwise.

```sh
python3 recorder.py --control renderer --note "set trace 64"
python3 recorder.py --control renderer --note "set cpuvs 1"
python3 recorder.py --control mark --note "CPU vertex path comparison"
python3 recorder.py --control renderer --note "set cpuvs 0"
```

`cpuattr` selects CPU conversion of vertex attribute formats while leaving vertex shader execution on the GPU: bit 0 signed 8-bit, bit 1 unsigned 8-bit, bit 2 signed 16-bit, bit 3 float. The hardware default is zero; Cemu may enable its signed-byte workaround. Restore the starting value after each experiment. `drawtrace` enables sampled draw-state reports; `trace` is a bounded graphics-event budget and does not guarantee that a draw will be captured.

## KDE launcher

Create `~/.local/share/applications/wiiu-session-recorder.desktop` with the following content, replacing both absolute paths for your checkout and logs location:

```ini
[Desktop Entry]
Type=Application
Name=Wii U Session Recorder
Comment=Record Aroma and 3Dii U runtime logs together
Exec=python3 /absolute/path/to/3dii-u/tools/wiiu_session_recorder/recorder.py --console WII_U_IP --output "/absolute/path/to/Wii U/Logs"
Icon=utilities-system-monitor
Terminal=false
Categories=Development;Utility;
```

KDE discovers the desktop entry through its application menu. On Wayland, use the buttons if global X11 shortcuts are unavailable.

GPU timestamps are opt-in with `renderer --note "set gputime 1"`; disable them after measurement. `toplevel 0` holds the original internal rendering size, `1` tests the lower level, and `2` restores automatic selection. To restore the original size after a diagnostic at 720p with automatic scaling disabled, send `toplevel 0`, allow a frame, then `toplevel 2`. These controls alter rendering resolution, not the console display mode.

`cputime` enables opt-in renderer and CPU geometry timing counters in compatible release builds. It does not enable shader disassembly or an interrupt sampler. Leave it off outside short diagnostic windows; compare timing-enabled and disabled runs before claiming an FPS improvement.

### Low-overhead playback recording

The **Stream Aroma trace** checkbox pauses only the Aroma TCP stream, keeping the runtime debugger and recording active in the same session. Connect/Disconnect respects this setting, which is remembered for the next launch. Python control: `--control aroma --note off` or `--control aroma --note on`. The status reports `aroma_enabled` so a deliberate pause is distinguishable from a failed connection. Enable the stream when system/module trace evidence is needed.

On one MH4U hardware session, a 12-second runtime-only test had one audio underrun while a separate 12-second Aroma-only window had 21. This implicates the high-volume system trace stream in that session; it does not identify an individual plugin or prove all gameplay slowdown has been fixed. Pausing the stream also means system trace messages are not captured during that interval; the game's buffered runtime log remains available.

### Audio/presentation timing

`--control renderer --note 'set avtime 1'` enables two-second audio wake/DSP-tick,
AX queue/recovery and GPU-pump summaries without per-draw profiling or screenshots.
Use `set avtime 0` afterward. Existing clients can use `set cputime 1` / `0`, which
also controls these summaries alongside the existing per-draw CPU counters.
Discard the first summary after enabling: it can contain earlier startup activity.
`[pace-gap]` reports gaps above 50 ms and separates time before entering present
from time spent inside it. This does not isolate decoding by itself.
`[audio-ring]` wrapped observations/recoveries expose playback overtaking the
writer; `[audio]` alone previously missed that case because modular queue depth
looked like a full ring. A source/mixer discontinuity can still occur without an
AX underrun. All these lines are saved in the ordinary `runtime.log`.

### Cemu sessions

Select **Cemu** in the Target selector, then use the same **Connect** and
**Start recording** buttons. This connects to the custom Cemu build's forwarded
3Dii U runtime console at `127.0.0.1:4405`; it does not attach GDB or pause the game.
A running 3Dii U title with runtime console forwarding is required. Stock Cemu
without that endpoint is not supported by this connection option.

Cemu sessions go under `<output>/Cemu/<timestamp>/`, containing `runtime.log`
and timestamped `events.jsonl`. `console.log` is empty because Aroma streaming
and FTP GamePad captures apply only to the Wii U target. F9 still records a
marker. Runtime state and statistics are requested every three seconds, with
thread snapshots every thirty seconds; renderer timing switches remain opt-in.
Hardware-specific heap-memory polling is disabled for Cemu.

Changing targets disconnects and saves the current session so logs never mix.
The Wii U address, Aroma preference and output root are preserved. The selected
target is remembered. CLI options: `--target cemu`, `--cemu-host 127.0.0.1`,
`--cemu-port 4405`; local control: `--control target --note cemu` (or `wiiu`).


## Periodic GamePad capture

Enable **Capture GamePad continuously (best effort)** beside the Aroma checkbox. It uses the
same ScreenshotWUPS/FTP transport as F9. Only one capture is in flight; a busy
attempt waits rather than adding to a queue. Successful captures have a short
200 ms cooldown; there is no five-second polling delay. The achieved rate is
shown in the window and depends on the console, encoding and FTP transfer.
Failures are recorded and retry after 2, 4, then 8 seconds. Stopping recording or disconnecting pauses new requests; one
already in flight may finish. Turning the checkbox off stops future attempts.
The setting is remembered and is unavailable for Cemu or --no-screenshots.

Images go in the current session's `captures/` folder. Each PNG has a JSON
sidecar with request/completion timestamps, offsets into console.log and
runtime.log, the most recently observed runtime status frames, and the request
token. The same information is indexed in events.jsonl. These offsets describe
the capture window, not a guaranteed exact GPU frame: capture, encoding and
transfer are asynchronous. Last-observed runtime frames can lag the image.
Manual F9 markers keep their existing names and also receive timeline sidecars.

This is periodic diagnosis, not video or an every-frame recorder. Captures add
GPU/CPU/SD/network work; disable the checkbox for clean performance comparisons.
The status line shows saved images, failures and the achieved capture rate.

```sh
python3 recorder.py --control screenshots --note on
python3 recorder.py --control screenshots --note off
```


## Explicit game and checkpoint controls

Compatible updated builds accept recorder control actions `state-save`,
`state-load`, `state-status`, `game-pause`, `game-resume`, and `game-speed`
(with note `100`, `200`, or `400`). The GUI exposes the same actions. Requests
are recorded in the session timeline. A queued response is not completion:
`[net-state]`, `[state-time]` and `[debug]` lines show the actual outcome.
Save uses the normal persistent checkpoint path; Load prefers the completed RAM
snapshot, otherwise storage. `renderer` also accepts `state save-ram` and
`state load-file` for explicit session/file tests. Older builds do not implement
these commands. Checkpoint coordination is required for state and RAM commands.

Pause opens the existing options menu, with its existing VBlank/input pause
behavior; Resume closes it. It is not an instruction-level CPU halt. Speed
uses the same 100/200/400% guest-clock settings as that menu and cannot create
CPU/GPU throughput the hardware lacks. Restore 100% after experiments.

For Python experiments, `renderer` accepts `ram read <guest-address>` and
`ram write <guest-address> <expected-u32> <replacement-u32>`. Addresses are aligned
32-bit guest virtual addresses; words are hexadecimal, interpreted in the guest's
little-endian order. The GPU-thread checkpoint barrier quiesces runtime workers
before the operation. Writes check readable/writable guest mappings and compare
the expected old word, then flush/invalidate GPU-visible data when applicable.
No host-native code or allocator metadata is patched through this command. A
single pending memory operation is supported; read `[debug-ram]` completion
before submitting the next. To reverse a successful edit, compare against the
replacement value and write the old value back. Patching data is not a renderer
fix and must remain an explicitly recorded diagnostic experiment.

Additional recorded A/B controls include `set lightguard 0|1`,
`set copyrect 0|1`, and `set gsshared 0|1`. `set nudge 0..50` selects a viewport
pixel offset in hundredths, and `set nudge 100` restores the automatic policy.
These are temporary developer controls, not persistent display settings.
Manual screenshots remain separate from timing windows. Automatic capture is
not enabled by any debug/state action.


### Find and remember a Wii U

The address field accepts an editable IPv4 address (four numbers, each 0–255).
**Apply IP** remembers it for the next launch. **Scan for Wii U** tries Aroma's
standard UDP logging beacon, then bounded read-only FTP checks on nearby local
IPv4 subnets. An open FTP port alone is insufficient: the scan checks for the
Aroma SD layout. If multiple consoles respond, choose one from the address list
and apply it. **Auto-detect on startup** is optional and remembered separately.
Scanning does not start recording or screenshots, and is blocked during a
recording so it cannot interrupt a capture. Manual addressing remains available
when logging/FTP is disabled, broadcast traffic is filtered, or a network's
subnet layout is outside the scan's bounded range.

The discovery path found the user's real Wii U in under one second during the
November 27, 2026 test. This does not establish detection on every router/VLAN.
