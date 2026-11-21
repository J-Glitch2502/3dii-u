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

Record starts one session folder containing console.log, runtime.log, and events.jsonl. Stop closes both logs together. Reconnection events, runtime commands, and markers share ISO timestamps and byte offsets for both files. Existing GamePad screenshots remain attached to Mark Glitch/F9 and are stored as timestamped PNGs beside the logs. There are no periodic screenshots.

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
