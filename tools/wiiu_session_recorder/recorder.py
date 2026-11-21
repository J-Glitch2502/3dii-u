#!/usr/bin/env python3
"""Record Aroma and game runtime channels in a shared timestamped session."""
import argparse
from collections import deque
from datetime import datetime
from ftplib import FTP, all_errors, error_perm, error_temp
from io import BytesIO
import json
import os
from pathlib import Path
import secrets
import re
import select
import socket
import socketserver
import threading
import time

from runtime_channel import RuntimeReceiver

BEACON = b"WIIU_TCP_SYSLOG_BEACON_V1"
UDP_PORT, TCP_PORT = 4445, 7965
DEFAULT_DIR = Path(os.environ.get("WIIU_LOG_DIR", Path.home() / "Wii U" / "Logs"))
CONTROL = Path(os.environ.get("XDG_RUNTIME_DIR", Path.home() / ".cache")) / "wiiu-session-recorder.sock"
SCREENSHOT_DIR = "/fs/vol/external01/wiiu/screenshots/recorder"


def timestamp():
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def capture_gamepad(console, destination):
    token = f"{secrets.randbelow(0xffffffff) + 1:08x}"
    remote = token + ".png"
    pending = destination.with_suffix(".png.part")
    try:
        with FTP(console, timeout=3) as ftp:
            ftp.login()
            for folder in (SCREENSHOT_DIR.rsplit("/", 1)[0], SCREENSHOT_DIR):
                try:
                    ftp.mkd(folder)
                except error_perm:
                    pass
            ftp.cwd(SCREENSHOT_DIR)
            ftp.storbinary(f"STOR {token}.request", BytesIO(token.encode("ascii")))
            try:
                ftp.rename(token + ".request", "request")
                deadline = time.monotonic() + 10
                while True:
                    try:
                        with pending.open("wb") as stream:
                            ftp.retrbinary(f"RETR {remote}", stream.write)
                        break
                    except (error_perm, error_temp) as exc:
                        if not str(exc).startswith(("450", "550")):
                            raise
                        if time.monotonic() >= deadline:
                            raise TimeoutError("No GamePad frame received from debug Aroma") from exc
                        time.sleep(0.2)
                with pending.open("rb") as stream:
                    if stream.read(8) != b"\x89PNG\r\n\x1a\n":
                        raise ValueError("Console returned an invalid PNG")
                pending.replace(destination)
            finally:
                for filename in (token + ".request", "request", remote):
                    try:
                        ftp.delete(filename)
                    except all_errors:
                        pass  # The plugin may have consumed it or the console may be offline.
    finally:
        pending.unlink(missing_ok=True)


class Recorder:
    def __init__(self, directory, screenshot_console=None):
        self.directory = directory
        self.target = "wiiu"
        self.screenshot_console = screenshot_console
        self.screenshot_queue = deque()
        self.screenshot_worker = None
        self.screenshot_status = ""
        self.lock = threading.Lock()
        self.connected = False
        self.last_data = None
        self.session = None
        self.log = self.runtime_log = self.events = None
        self.bytes = 0
        self.markers = 0
        self.runtime_connected = False
        self.runtime_last_data = None
        self.runtime_bytes = 0
        self.mapped_mutexes = {}
        self.aroma_pending = ""

    def event(self, kind, **details):
        record = {"event": kind, "time": timestamp(), "log_offset": self.bytes, "runtime_offset": self.runtime_bytes, **details}
        self.events.write(json.dumps(record, ensure_ascii=False) + "\n")
        self.events.flush()

    def connection(self, connected):
        with self.lock:
            if self.connected != connected and self.events:
                self.event("connected" if connected else "disconnected")
            self.connected = connected
            if not connected:
                self.last_data = None
                self.mapped_mutexes.clear()
                self.aroma_pending = ""

    def heap_owner_fields(self):
        with self.lock:
            return {} if self.target == "cemu" else {0x80000020: "heap-spinlock", **{address + 0x1c: f"heap-mutex-{index}" for index, address in self.mapped_mutexes.items()}}

    def receive(self, data):
        # Heartbeat padding is NUL. Preserve the console's original CR/LF.
        data = data.replace(b"\0", b"")
        if not data:
            return
        with self.lock:
            self.last_data = time.monotonic()
            self.aroma_pending += data.decode(errors="replace")
            lines = re.split(r"[\r\n]", self.aroma_pending)
            self.aroma_pending = lines.pop()[-4096:]
            for line in lines:
                if "Running MemoryMappingModule " in line:
                    self.mapped_mutexes.clear()
                match = re.search(r"\[AFR4 MM\] MUTEX index=([0-4]) address=([0-9A-Fa-f]{8}) owner_offset=1C size=2C", line)
                if match:
                    index, address = int(match[1]), int(match[2], 16)
                    if address and not (address & 3) and address <= 0xffffffd0:
                        self.mapped_mutexes[index] = address
                        if self.events: self.event("heap_mutex_discovered", index=index, address=hex(address))
            if self.log:
                self.log.write(data)
                self.bytes += len(data)

    def recording_context(self):
        with self.lock:
            return str(self.session) if self.log else None

    def runtime_event(self, kind, **details):
        with self.lock:
            if self.events:
                self.event(kind, **details)

    def runtime_connection(self, connected):
        with self.lock:
            if self.runtime_connected != connected and self.events:
                self.event("runtime_connected" if connected else "runtime_disconnected")
            self.runtime_connected = connected
            if not connected:
                self.runtime_last_data = None

    def receive_runtime(self, data):
        if not data:
            return
        with self.lock:
            self.runtime_last_data = time.monotonic()
            if self.runtime_log:
                self.runtime_log.write(data)
                self.runtime_log.flush()
                self.runtime_bytes += len(data)

    def flush(self):
        with self.lock:
            if self.log:
                self.log.flush()
            if self.runtime_log:
                self.runtime_log.flush()

    def _start(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        path = self.directory / datetime.now().strftime("%Y-%m-%d_%H-%M-%S-%f")
        path.mkdir(mode=0o700)
        log = (path / "console.log").open("xb")
        try:
            events = (path / "events.jsonl").open("x", encoding="utf-8")
            try:
                runtime_log = (path / "runtime.log").open("xb")
            except BaseException:
                events.close()
                raise
        except BaseException:
            log.close()
            raise
        self.log, self.events, self.runtime_log = log, events, runtime_log
        self.session = path
        self.bytes = self.runtime_bytes = self.markers = 0
        self.screenshot_status = ""
        self.event("started", target=self.target, connected=self.connected, runtime_connected=self.runtime_connected)
        for name in ("console.log", "runtime.log", "events.jsonl"):
            (path / name).chmod(0o600)

    def _stop(self):
        self.event("stopped")
        self.log.close()
        self.runtime_log.close()
        self.events.close()
        self.log = self.runtime_log = self.events = None

    def _capture(self, session, filename, marker):
        try:
            capture_gamepad(self.screenshot_console, session / filename)
            result = {"status": "saved", "screenshot": filename}
            message = f"GamePad screenshot saved: {filename}"
        except all_errors + (ValueError,) as exc:
            result = {"status": "failed", "error": str(exc)}
            message = f"Screenshot unavailable: {exc}"
        with self.lock:
            details = {**marker, **result, "capture_completed_at": timestamp()}
            if self.session == session and self.events:
                self.event("graphics_screenshot", **details)
            else:
                with (session / "events.jsonl").open("a", encoding="utf-8") as events:
                    events.write(json.dumps({"event": "graphics_screenshot", **details}) + "\n")
            if self.session == session:
                self.screenshot_status = message

    def _capture_pending(self):
        while True:
            with self.lock:
                if not self.screenshot_queue:
                    self.screenshot_worker = None
                    return
                session, filename, marker = self.screenshot_queue.popleft()
            self._capture(session, filename, marker)

    def command(self, action, note=""):
        with self.lock:
            if action == "toggle":
                action = "stop" if self.log else "start"
            if action == "start" and not self.log:
                self._start()
            elif action == "stop" and self.log:
                self._stop()
            elif action == "mark":
                if not self.log:
                    raise ValueError("Start recording before marking a glitch.")
                self.log.flush()
                self.markers += 1
                instant = datetime.now().astimezone()
                marker = {"number": self.markers, "time": instant.isoformat(timespec="milliseconds"),
                          "log_offset": self.bytes, "runtime_offset": self.runtime_bytes}
                details = {}
                if self.screenshot_console:
                    filename = f"glitch-{self.markers:03d}-{instant:%H-%M-%S-%f}.png"
                    details = {"screenshot": filename, "screenshot_status": "requested"}
                    self.screenshot_status = "Waiting for GamePad screenshot…"
                    self.screenshot_queue.append((self.session, filename, marker))
                    if self.screenshot_worker is None:
                        self.screenshot_worker = threading.Thread(target=self._capture_pending)
                        self.screenshot_worker.start()
                self.event("graphics_glitch", **marker, **details, note=note[:500])
            elif action not in {"status", "start", "stop"}:
                raise ValueError("Unknown recorder action.")
            return {
                "recording": self.log is not None,
                "connected": self.connected,
                "seconds_since_data": round(time.monotonic() - self.last_data, 1) if self.last_data else None,
                "session": str(self.session) if self.session else None,
                "bytes": self.bytes,
                "markers": self.markers,
                "screenshot_status": self.screenshot_status,
                "runtime_connected": self.runtime_connected,
                "runtime_seconds_since_data": round(time.monotonic() - self.runtime_last_data, 1) if self.runtime_last_data else None,
                "runtime_bytes": self.runtime_bytes,
            }


class AromaReceiver(threading.Thread):
    def __init__(self, recorder, console, tcp_port=TCP_PORT, udp_port=UDP_PORT):
        super().__init__(daemon=True)
        self.recorder = recorder
        self.console = console
        self.done = threading.Event()
        self.error = None
        self.beacon = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listener = socket.socket()
        try:
            self.beacon.connect((console, udp_port))
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.listener.bind((self.beacon.getsockname()[0], tcp_port))
            self.listener.listen(4)
        except BaseException:
            self.listener.close()
            self.beacon.close()
            raise

    def run(self):
        client = None
        next_tick = 0
        try:
            while not self.done.is_set():
                now = time.monotonic()
                if now >= next_tick:
                    next_tick = now + 1
                    try:
                        self.beacon.send(BEACON)
                    except OSError:
                        pass  # Wi-Fi may drop temporarily.
                    if client:
                        try:
                            client.send(b"\0")
                        except BlockingIOError:
                            pass
                        except OSError:
                            client.close()
                            client = None
                            self.recorder.connection(False)
                    self.recorder.flush()
                ready, _, _ = select.select([self.listener] + ([client] if client else []), [], [], 0.2)
                if self.listener in ready:
                    new, peer = self.listener.accept()
                    if peer[0] == self.console:
                        if client:
                            client.close()
                            self.recorder.connection(False)
                        client = new
                        client.setblocking(False)
                        self.recorder.connection(True)
                    else:
                        new.close()
                if client and client in ready:
                    try:
                        data = client.recv(65536)
                    except BlockingIOError:
                        continue
                    except OSError:
                        data = b""
                    if data:
                        self.recorder.receive(data)
                    else:
                        client.close()
                        client = None
                        self.recorder.connection(False)
        except OSError as exc:
            self.error = str(exc)
        finally:
            if client:
                client.close()
            self.listener.close()
            self.beacon.close()
            self.recorder.connection(False)


def read_preferences(path):
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError): return {}


class Controller:
    def __init__(self, recorder, console, aroma_tcp_port=TCP_PORT, aroma_udp_port=UDP_PORT, runtime_port=4405, aroma_enabled=True, preferences_path=None, target="wiiu", cemu_host="127.0.0.1", cemu_port=4405):
        if target not in ("wiiu", "cemu"):
            raise ValueError("Target must be wiiu or cemu")
        self.target = target
        self.base_directory = recorder.directory
        self.hardware_screenshot_console = recorder.screenshot_console
        self.cemu_host, self.cemu_port = cemu_host, cemu_port
        recorder.target = target
        recorder.directory = self.base_directory / "Cemu" if target == "cemu" else self.base_directory
        if target == "cemu": recorder.screenshot_console = None
        self.aroma_enabled = aroma_enabled
        self.preferences_path = preferences_path
        self.aroma_tcp_port, self.aroma_udp_port, self.runtime_port = aroma_tcp_port, aroma_udp_port, runtime_port
        self.runtime_receiver = None
        self.recorder = recorder
        self.console = console
        self.receiver = None
        self.lock = threading.Lock()

    def _disconnect(self):
        if self.runtime_receiver:
            self.runtime_receiver.done.set()
            self.runtime_receiver.join(2)
            if self.runtime_receiver.is_alive():
                raise RuntimeError("Runtime receiver is still shutting down.")
            self.runtime_receiver = None
        if self.receiver:
            self.receiver.done.set()
            self.receiver.join(2)
            if self.receiver.is_alive():
                raise RuntimeError("The receiver is still shutting down.")
            self.receiver = None

    def command(self, action, note=""):
        with self.lock:
            if action == "diagnose":
                if not self.runtime_receiver or not self.runtime_receiver.is_alive() or not self.recorder.runtime_connected:
                    raise RuntimeError("Connect to the running game's debugger before capturing performance.")
                self.recorder.command('start')
                self.runtime_receiver.request('diag')
                self.recorder.runtime_event('diagnostic_capture', level=2, duration_seconds=10)
                action, note = 'renderer', 'set diag 2'
            if action == "target":
                if note not in ("wiiu", "cemu"):
                    raise ValueError("Use target wiiu or cemu")
                if note != self.target:
                    self._disconnect()
                    self.recorder.command("stop")
                    self.target = self.recorder.target = note
                    self.recorder.directory = self.base_directory / "Cemu" if note == "cemu" else self.base_directory
                    self.recorder.screenshot_console = None if note == "cemu" else self.hardware_screenshot_console
                    self.recorder.session = None
                    self.recorder.bytes = self.recorder.runtime_bytes = self.recorder.markers = 0
                    self.recorder.screenshot_status = ""
                    if self.preferences_path:
                        self.preferences_path.parent.mkdir(parents=True, exist_ok=True)
                        values = read_preferences(self.preferences_path)
                        values['target'] = note
                        pending = self.preferences_path.with_suffix('.tmp')
                        pending.write_text(json.dumps(values) + '\n')
                        pending.replace(self.preferences_path)
                action = "status"
            if action == "toggle_connection":
                action = "disconnect" if (self.receiver and self.receiver.is_alive()) or (self.runtime_receiver and self.runtime_receiver.is_alive()) else "connect"
            if action == "aroma":
                if self.target == "cemu": raise ValueError("Aroma streaming is available for Wii U only")
                if note not in ("on", "off"): raise ValueError('Use aroma on or off')
                self.aroma_enabled = note == "on"
                if not self.aroma_enabled and self.receiver:
                    self.receiver.done.set()
                    self.receiver.join(2)
                    if self.receiver.is_alive(): raise RuntimeError('Aroma receiver is still shutting down')
                    self.receiver = None
                elif self.aroma_enabled and self.runtime_receiver and self.runtime_receiver.is_alive() and not self.receiver:
                    self.receiver = AromaReceiver(self.recorder, self.console, self.aroma_tcp_port, self.aroma_udp_port)
                    self.receiver.start()
                if self.preferences_path:
                    self.preferences_path.parent.mkdir(parents=True, exist_ok=True)
                    pending = self.preferences_path.with_suffix('.tmp')
                    values = read_preferences(self.preferences_path)
                    values['aroma_enabled'] = self.aroma_enabled
                    pending.write_text(json.dumps(values) + '\n')
                    pending.replace(self.preferences_path)
                self.recorder.runtime_event('aroma_stream_mode', enabled=self.aroma_enabled)
                action = "status"
            if action == "connect":
                if self.target == "wiiu" and self.aroma_enabled and (not self.receiver or not self.receiver.is_alive()):
                    self.receiver = AromaReceiver(self.recorder, self.console, self.aroma_tcp_port, self.aroma_udp_port)
                    self.receiver.start()
                if not self.runtime_receiver or not self.runtime_receiver.is_alive():
                    self.runtime_receiver = RuntimeReceiver(self.recorder, self.cemu_host if self.target == "cemu" else self.console,
                                                            self.cemu_port if self.target == "cemu" else self.runtime_port)
                    self.runtime_receiver.start()
                action = "status"
            elif action == "disconnect":
                self._disconnect()
                action = "status"
            if action in ("runtime", "renderer"):
                if not self.runtime_receiver or not self.runtime_receiver.is_alive():
                    raise RuntimeError("Connect before sending runtime commands.")
                queued = self.runtime_receiver.request(note, allow_renderer=action == "renderer")
                if action == "renderer": self.recorder.runtime_event("renderer_command_queued", command=queued)
                action = "status"
            else:
                queued = None
            result = self.recorder.command(action, note)
            if queued:
                result["runtime_command_queued"] = queued
            result["listening"] = bool(self.receiver and self.receiver.is_alive())
            result["receiver_error"] = self.receiver.error if self.receiver else None
            result["runtime_listening"] = bool(self.runtime_receiver and self.runtime_receiver.is_alive())
            result["runtime_error"] = self.runtime_receiver.error if self.runtime_receiver else None
            result["aroma_enabled"] = self.aroma_enabled
            result["console"] = self.cemu_host if self.target == "cemu" else self.console
            result["target"] = self.target
            result["runtime_port"] = self.cemu_port if self.target == "cemu" else self.runtime_port
            return result


class ControlHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(2)
        try:
            request = json.loads(self.rfile.readline(4096))
            result = self.server.controller.command(request["action"], request.get("note", ""))
        except (ValueError, TypeError, KeyError, AttributeError, OSError, RuntimeError) as exc:
            result = {"error": str(exc)}
        self.wfile.write(json.dumps(result).encode() + b"\n")


class ControlServer(socketserver.UnixStreamServer):
    def __init__(self, path, controller):
        self.path = path
        self.controller = controller
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            try:
                control("status", path=path)
            except ConnectionRefusedError:
                path.unlink()  # A previous instance was terminated.
            else:
                raise RuntimeError("Wii U Session Recorder is already running.")
        super().__init__(str(path), ControlHandler)
        path.chmod(0o600)

    def server_close(self):
        super().server_close()
        self.path.unlink(missing_ok=True)


def control(action, note="", path=CONTROL):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3)
        client.connect(str(path))
        client.sendall(json.dumps({"action": action, "note": note}).encode() + b"\n")
        with client.makefile("rb") as stream:
            return json.loads(stream.readline())


class Hotkeys(threading.Thread):
    def __init__(self, notify):
        super().__init__(daemon=True)
        self.notify = notify
        self.ready = threading.Event()
        self.done = threading.Event()
        self.error = None

    def run(self):
        try:
            from Xlib import X, XK, display, error
            with _display(display.Display()) as screen:
                root = screen.screen().root
                actions = {
                    screen.keysym_to_keycode(XK.string_to_keysym("F8")): "toggle",
                    screen.keysym_to_keycode(XK.string_to_keysym("F9")): "mark",
                }
                grab_error = error.CatchError(error.BadAccess)
                for key in actions:
                    for locks in (0, X.LockMask, X.Mod2Mask, X.LockMask | X.Mod2Mask):
                        root.grab_key(key, locks, False, X.GrabModeAsync, X.GrabModeAsync, onerror=grab_error)
                screen.sync()
                if grab_error.get_error():
                    raise RuntimeError("F8 or F9 is already assigned by another app.")
                self.ready.set()
                pressed_at = {}
                while not self.done.is_set():
                    if not screen.pending_events():
                        select.select([screen], [], [], 0.2)
                        continue
                    event = screen.next_event()
                    if event.type == X.KeyPress and event.detail in actions:
                        last = pressed_at.get(event.detail)
                        pressed_at[event.detail] = event.time
                        if last is None or (event.time - last) & 0xFFFFFFFF > 250:
                            self.notify(actions[event.detail])
        except Exception as exc:
            self.error = str(exc)
        finally:
            self.ready.set()


class _display:
    def __init__(self, connection):
        self.connection = connection

    def __enter__(self):
        return self.connection

    def __exit__(self, *_):
        self.connection.close()


def gui(controller, hotkey_enabled):
    import queue
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title("Wii U Session Recorder")
    root.geometry("780x380")
    root.minsize(650, 310)
    panel = ttk.Frame(root, padding=18)
    panel.pack(fill="both", expand=True)
    target_row = ttk.Frame(panel)
    target_row.pack(anchor="w", pady=(0, 8))
    ttk.Label(target_row, text="Target: ").pack(side="left")
    target_choice = tk.StringVar(value="Cemu" if controller.target == "cemu" else "Wii U")
    target_box = ttk.Combobox(target_row, textvariable=target_choice, values=("Wii U", "Cemu"), state="readonly", width=12)
    target_box.pack(side="left")
    ttk.Label(target_row, text="  Switching targets stops and saves the current session.").pack(side="left")
    connection = tk.StringVar(value="Waiting for Aroma TCP logging")
    summary = tk.StringVar(value="Ready to record")
    location = tk.StringVar(value=str(controller.recorder.directory))
    message = tk.StringVar()
    ttk.Label(panel, textvariable=connection, font=("TkDefaultFont", 12, "bold")).pack(anchor="w")
    ttk.Label(panel, textvariable=summary).pack(anchor="w", pady=(8, 3))
    ttk.Label(panel, textvariable=location, wraplength=490).pack(anchor="w")
    buttons = ttk.Frame(panel)
    buttons.pack(anchor="w", pady=13)
    pending = queue.SimpleQueue()
    keys = Hotkeys(pending.put) if hotkey_enabled else None
    if keys:
        keys.start()

    def act(action):
        try:
            result = controller.command(action)
            if action == "mark":
                message.set(f"Graphics glitch marked ({result['markers']})")
            elif action == "diagnose":
                message.set("Capturing performance for 10 seconds; results go to this session.")
            else:
                message.set("")
        except (OSError, ValueError, RuntimeError) as exc:
            message.set(str(exc))

    def change_target(_event):
        try:
            controller.command("target", "cemu" if target_choice.get() == "Cemu" else "wiiu")
            message.set("Target changed. Connect, then start recording.")
        except (OSError, ValueError, RuntimeError) as exc:
            message.set(str(exc))
    target_box.bind("<<ComboboxSelected>>", change_target)

    link = ttk.Button(buttons, text="Connect", command=lambda: act("toggle_connection"))
    link.pack(side="left", padx=(0, 10))
    record = ttk.Button(buttons, text="Start recording (F8)", command=lambda: act("toggle"))
    mark = ttk.Button(buttons, text="Mark graphics glitch (F9)", command=lambda: act("mark"))
    record.pack(side="left", padx=(0, 10))
    mark.pack(side="left")
    ttk.Button(buttons, text="Capture performance (10s)", command=lambda: act("diagnose")).pack(side="left", padx=(10, 0))
    aroma_choice = tk.BooleanVar(value=controller.aroma_enabled)
    def toggle_aroma():
        try: controller.command('aroma', 'on' if aroma_choice.get() else 'off')
        except (OSError, ValueError, RuntimeError) as exc: message.set(str(exc))
    aroma_toggle = ttk.Checkbutton(panel, text="Stream Aroma trace", variable=aroma_choice, command=toggle_aroma)
    aroma_toggle.pack(anchor="w")
    ttk.Label(panel, textvariable=message).pack(anchor="w")
    shot_note = tk.StringVar()
    ttk.Label(panel, textvariable=shot_note, wraplength=610).pack(anchor="w")
    key_note = tk.StringVar()
    ttk.Label(panel, textvariable=key_note).pack(anchor="w")

    def refresh():
        while not pending.empty():
            act(pending.get())
        state = controller.command("status")
        is_cemu = state["target"] == "cemu"
        target_choice.set("Cemu" if is_cemu else "Wii U")
        aroma_toggle.state(["disabled"] if is_cemu else ["!disabled"])
        if is_cemu:
            runtime = "connected" if state["runtime_connected"] else "waiting for game" if state["runtime_listening"] else "disconnected"
            connection.set(f"Cemu runtime: {runtime} · {state['console']}:{state['runtime_port']}")
        elif state["listening"] or state["runtime_listening"]:
            aroma_choice.set(state["aroma_enabled"])
            aroma = "paused" if not state["aroma_enabled"] else "connected" if state["connected"] else "waiting"
            runtime = "connected" if state["runtime_connected"] else "waiting for game"
            connection.set(f"Aroma: {aroma}   ·   Runtime: {runtime}")
        else:
            connection.set("Disconnected")
        link.configure(text="Disconnect" if state["listening"] or state["runtime_listening"] else "Connect")
        record.configure(text="Stop recording (F8)" if state["recording"] else "Start recording (F8)")
        mark.state(["!disabled"] if state["recording"] else ["disabled"])
        status = "Recording" if state["recording"] else "Last session" if state["session"] else "Ready to record"
        summary.set(f"{status}   ·   Aroma {state['bytes']:,} B   ·   Runtime {state['runtime_bytes']:,} B   ·   {state['markers']} markers")
        location.set(state["session"] or str(controller.recorder.directory))
        shot_note.set(state["screenshot_status"])
        if keys and keys.error:
            key_note.set("Global keys unavailable. Use the buttons or Python control.")
        elif keys and keys.ready.is_set():
            key_note.set("F8 starts or stops recording. F9 marks a graphics glitch.")
        root.after(100, refresh)

    refresh()
    try:
        root.mainloop()
    finally:
        if keys:
            keys.done.set()
            keys.join(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console", default=os.environ.get("WIIU_HOST", "127.0.0.1"))
    parser.add_argument("--target", choices=("wiiu", "cemu"))
    parser.add_argument("--cemu-host", default="127.0.0.1")
    parser.add_argument("--cemu-port", type=int, default=4405)
    parser.add_argument("--output", type=Path, default=DEFAULT_DIR)
    parser.add_argument("--no-hotkeys", action="store_true")
    parser.add_argument("--no-screenshots", action="store_true")
    parser.add_argument("--control", choices=("status", "connect", "disconnect", "start", "stop", "toggle", "mark", "runtime", "renderer", "aroma", "target", "diagnose"))
    parser.add_argument("--note", default="")
    args = parser.parse_args()
    if args.control:
        result = control(args.control, args.note)
        print(json.dumps(result))
        return int("error" in result)
    preferences = Path(os.environ.get('XDG_CONFIG_HOME', Path.home()/'.config'))/'wiiu-session-recorder/settings.json'
    enabled = read_preferences(preferences).get('aroma_enabled', True)
    if not isinstance(enabled, bool): enabled = True
    target = args.target or read_preferences(preferences).get('target', 'wiiu')
    if target not in ('wiiu', 'cemu'): target = 'wiiu'
    controller = Controller(Recorder(args.output, None if args.no_screenshots else args.console), args.console,
                            aroma_enabled=enabled, preferences_path=preferences, target=target,
                            cemu_host=args.cemu_host, cemu_port=args.cemu_port)
    with ControlServer(CONTROL, controller) as server:
        control_thread = threading.Thread(target=server.serve_forever, daemon=True)
        control_thread.start()
        try:
            gui(controller, not args.no_hotkeys)
        finally:
            server.shutdown()
            controller.command("disconnect")
            controller.command("stop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
