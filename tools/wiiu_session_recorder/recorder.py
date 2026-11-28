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
from discovery import discover

BEACON = b"WIIU_TCP_SYSLOG_BEACON_V1"
UDP_PORT, TCP_PORT = 4445, 7965
DEFAULT_DIR = Path(os.environ.get("WIIU_LOG_DIR", Path.home() / "Wii U" / "Logs"))
CONTROL = Path(os.environ.get("XDG_RUNTIME_DIR", Path.home() / ".cache")) / "wiiu-session-recorder.sock"
SCREENSHOT_DIR = "/fs/vol/external01/wiiu/screenshots/recorder"
PERIODIC_CAPTURE_SECONDS = 0.2
CAPTURE_RETRY_SECONDS = 2.0


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
                return {"capture_token": token, "image_received_at": timestamp()}
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
        self.periodic_capture = False
        self.periodic_worker = None
        self.periodic_wake = threading.Event()
        self.periodic_saved = self.periodic_failed = self.periodic_skipped = self.periodic_sequence = 0
        self.capture_not_before = 0.0
        self.capture_failures_in_a_row = 0
        self.last_capture_seconds = None
        self.capture_times = deque(maxlen=12)
        self.runtime_frame = None
        self.runtime_frame_observed_at = None
        self.runtime_pending = ""
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
            self.periodic_wake.set()
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
            self.periodic_wake.set()
            if not connected:
                self.runtime_last_data = None
                self.runtime_frame = self.runtime_frame_observed_at = None
                self.runtime_pending = ""

    def receive_runtime(self, data):
        if not data:
            return
        with self.lock:
            self.runtime_last_data = time.monotonic()
            self.runtime_pending += data.decode(errors="replace")
            lines = self.runtime_pending.split("\n")
            self.runtime_pending = lines.pop()[-4096:]
            for line in lines:
                match = re.search(r'\[st\] frame (\d+)', line)
                if match:
                    self.runtime_frame = int(match[1])
                    self.runtime_frame_observed_at = timestamp()
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
        self.periodic_saved = self.periodic_failed = self.periodic_skipped = self.periodic_sequence = 0
        self.capture_not_before = 0.0
        self.capture_failures_in_a_row = 0
        self.last_capture_seconds = None
        self.capture_times.clear()
        self.periodic_wake.set()
        self.event("started", target=self.target, connected=self.connected, runtime_connected=self.runtime_connected)
        for name in ("console.log", "runtime.log", "events.jsonl"):
            (path / name).chmod(0o600)

    def _stop(self):
        self.event("stopped")
        self.log.close()
        self.runtime_log.close()
        self.events.close()
        self.log = self.runtime_log = self.events = None
        self.periodic_wake.set()

    def _queue_capture(self, session, filename, marker):
        # Called with self.lock held. Manual and periodic captures share one worker.
        self.screenshot_queue.append((session, filename, marker, self.screenshot_console))
        if self.screenshot_worker is None:
            self.screenshot_worker = threading.Thread(target=self._capture_pending, daemon=True)
            self.screenshot_worker.start()

    def _periodic_loop(self):
        while True:
            with self.lock:
                if not self.periodic_capture:
                    self.periodic_worker = None
                    return
                self.periodic_wake.clear()
                delay = None  # A busy capture wakes us when its worker finishes.
                if self.log and self.target == "wiiu" and self.screenshot_console and (self.connected or self.runtime_connected):
                    if self.screenshot_worker is None:
                        delay = max(0.0, self.capture_not_before - time.monotonic())
                        if delay <= 0:
                            self.periodic_sequence += 1
                            now = datetime.now().astimezone()
                            filename = f"captures/gamepad-{self.periodic_sequence:06d}-{now:%H-%M-%S-%f}.png"
                            marker = {"periodic": True, "number": self.periodic_sequence,
                                      "capture_requested_at": now.isoformat(timespec="milliseconds"),
                                      "request_log_offset": self.bytes, "request_runtime_offset": self.runtime_bytes,
                                      "request_runtime_frame": self.runtime_frame,
                                      "request_runtime_frame_observed_at": self.runtime_frame_observed_at}
                            self.log.flush(); self.runtime_log.flush()
                            self.event("periodic_screenshot_requested", screenshot=filename, **marker)
                            self._queue_capture(self.session, filename, marker)
                            delay = None
                    else:
                        self.periodic_skipped += 1
            self.periodic_wake.wait(delay)

    def _capture(self, session, filename, marker, console):
        began = time.monotonic()
        try:
            destination = session / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            transport = capture_gamepad(console, destination) or {}
            result = {"status": "saved", "screenshot": filename, **transport}
            message = f"GamePad screenshot saved: {filename}"
        except Exception as exc:
            result = {"status": "failed", "screenshot": filename, "error": str(exc)}
            message = f"Screenshot unavailable; periodic capture will retry: {exc}"
        with self.lock:
            current = self.session == session
            details = {**marker, **result, "capture_completed_at": timestamp(),
                       "duration_seconds": round(time.monotonic() - began, 3),
                       "capture_completed_log_offset": self.bytes if current else None,
                       "capture_completed_runtime_offset": self.runtime_bytes if current else None,
                       "completion_runtime_frame": self.runtime_frame if current else None,
                       "completion_runtime_frame_observed_at": self.runtime_frame_observed_at if current else None,
                       "timing_note": "Request/completion bound an asynchronous capture; runtime frames are last observed status, not exact image-frame IDs."}
            kind = "periodic_screenshot" if marker.get("periodic") else "graphics_screenshot"
            if current and self.events:
                self.event(kind, **details)
            else:
                with (session / "events.jsonl").open("a", encoding="utf-8") as events:
                    events.write(json.dumps({"event": kind, **details}) + "\n")
            try:
                (session / filename).with_suffix('.json').write_text(json.dumps(details, indent=2) + "\n", encoding="utf-8")
            except OSError:
                pass  # The event log still carries the mapping if the sidecar cannot be written.
            if current:
                self.screenshot_status = message
                if marker.get("periodic"):
                    self.last_capture_seconds = round(time.monotonic() - began, 3)
                    if result['status'] == 'saved':
                        self.periodic_saved += 1
                        self.capture_failures_in_a_row = 0
                        self.capture_times.append(time.monotonic())
                        cooldown = PERIODIC_CAPTURE_SECONDS
                    else:
                        self.periodic_failed += 1
                        self.capture_failures_in_a_row += 1
                        cooldown = CAPTURE_RETRY_SECONDS * 2 ** min(self.capture_failures_in_a_row - 1, 2)
                    self.capture_not_before = time.monotonic() + cooldown

    def _capture_pending(self):
        while True:
            with self.lock:
                if not self.screenshot_queue:
                    self.screenshot_worker = None
                    self.periodic_wake.set()
                    return
                session, filename, marker, console = self.screenshot_queue.popleft()
            self._capture(session, filename, marker, console)

    def command(self, action, note=""):
        with self.lock:
            if action == "toggle":
                action = "stop" if self.log else "start"
            if action == "start" and not self.log:
                self._start()
            elif action == "stop" and self.log:
                self._stop()
            elif action == "screenshots":
                if note not in ("on", "off"):
                    raise ValueError("Use screenshots on or off")
                if note == "on" and (self.target != "wiiu" or not self.screenshot_console):
                    raise ValueError("Periodic GamePad capture requires Wii U screenshots to be enabled.")
                self.periodic_capture = note == "on"
                if self.events:
                    self.event("periodic_screenshot_mode", enabled=self.periodic_capture,
                               mode="continuous_best_effort", minimum_cooldown_seconds=PERIODIC_CAPTURE_SECONDS)
                if self.periodic_capture and self.periodic_worker is None:
                    self.periodic_worker = threading.Thread(target=self._periodic_loop, daemon=True)
                    self.periodic_worker.start()
                self.periodic_wake.set()
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
                    marker.update(capture_requested_at=marker['time'], request_log_offset=self.bytes,
                                  request_runtime_offset=self.runtime_bytes, request_runtime_frame=self.runtime_frame,
                                  request_runtime_frame_observed_at=self.runtime_frame_observed_at)
                    self._queue_capture(self.session, filename, marker)
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
                "periodic_capture": self.periodic_capture,
                "periodic_capture_available": self.target == "wiiu" and bool(self.screenshot_console),
                "periodic_interval_seconds": PERIODIC_CAPTURE_SECONDS,
                "periodic_capture_mode": "continuous_best_effort",
                "last_capture_seconds": self.last_capture_seconds,
                "captures_per_second": (round((len(self.capture_times)-1)/(self.capture_times[-1]-self.capture_times[0]), 2)
                                        if len(self.capture_times)>1 and self.capture_times[-1]>self.capture_times[0] else None),
                "periodic_saved": self.periodic_saved,
                "periodic_failed": self.periodic_failed,
                "periodic_skipped": self.periodic_skipped,
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
            if action in ('state-save', 'state-load', 'state-status', 'game-pause', 'game-resume', 'game-speed', 'fallback-rendering', 'game-input', 'game-menu', 'game-relaunch'):
                if not self.runtime_receiver or not self.runtime_receiver.is_alive() or not self.recorder.runtime_connected:
                    raise RuntimeError("Connect to the running game's debugger first.")
                commands = {'state-save': 'state save', 'state-load': 'state load', 'state-status': 'state status',
                            'game-pause': 'set pause 1', 'game-resume': 'set pause 0',
                            'game-menu': 'quit', 'game-relaunch': 'relaunch'}
                if action == 'game-speed': command = 'set speed ' + note
                elif action == 'game-input': command = 'in ' + note
                elif action == 'fallback-rendering':
                    if note not in ('on', 'off'): raise ValueError('Use fallback-rendering on or off')
                    command = 'set uberall ' + ('1' if note == 'on' else '0')
                else: command = commands[action]
                self.recorder.runtime_event('debug_control_requested', action=action, command=command)
                action, note = 'renderer', command
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
            if action == 'screenshots' and self.preferences_path:
                self.preferences_path.parent.mkdir(parents=True, exist_ok=True)
                values = read_preferences(self.preferences_path)
                values['periodic_screenshots'] = result['periodic_capture']
                pending = self.preferences_path.with_suffix('.tmp')
                pending.write_text(json.dumps(values) + '\n')
                pending.replace(self.preferences_path)
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
            expected = getattr(self.server, "token", None)
            if expected is not None and not secrets.compare_digest(str(request.get("token", "")), expected):
                raise ValueError("Invalid local control token")
            result = self.server.controller.command(request["action"], request.get("note", ""))
        except (ValueError, TypeError, KeyError, AttributeError, OSError, RuntimeError) as exc:
            result = {"error": str(exc)}
        self.wfile.write(json.dumps(result).encode() + b"\n")


class UnixControlServer(getattr(socketserver, "UnixStreamServer", socketserver.TCPServer)):
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


class WindowsControlServer(socketserver.TCPServer):
    """Authenticated loopback control; no remote debugger listener on the PC."""
    def __init__(self, path, controller):
        self.path, self.controller = Path(path), controller
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            try:
                control("status", path=self.path, windows=True)
            except (OSError, ValueError, KeyError):
                self.path.unlink(missing_ok=True)
            else:
                raise RuntimeError("Wii U Session Recorder is already running.")
        self.token = secrets.token_hex(32)
        super().__init__(("127.0.0.1", 0), ControlHandler)
        self.path.write_text(json.dumps({"port": self.server_address[1], "token": self.token}))

    def server_close(self):
        super().server_close()
        self.path.unlink(missing_ok=True)


ControlServer = WindowsControlServer if os.name == "nt" else UnixControlServer


def control(action, note="", path=CONTROL, windows=None):
    windows = os.name == "nt" if windows is None else windows
    request = {"action": action, "note": note}
    if windows:
        endpoint = json.loads(Path(path).read_text())
        address = ("127.0.0.1", int(endpoint["port"]))
        request["token"] = endpoint["token"]
        family = socket.AF_INET
    else:
        address, family = str(path), socket.AF_UNIX
    with socket.socket(family) as client:
        client.settimeout(3)
        client.connect(address)
        client.sendall(json.dumps(request).encode() + b"\n")
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
    root.geometry("1000x610")
    root.minsize(850, 495)
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
    host_row = ttk.Frame(panel)
    host_row.pack(anchor="w", before=buttons, pady=(8, 0))
    ttk.Label(host_row, text="Wii U IP: ").pack(side="left")
    host_value = tk.StringVar(value=controller.console)
    host_entry = ttk.Combobox(host_row, textvariable=host_value, width=18, values=[])
    host_entry.pack(side="left")
    def apply_host():
        try:
            host = str(__import__('ipaddress').IPv4Address(host_value.get().strip()))
            controller.command("disconnect")
            controller.command("stop")
            controller.console = host
            controller.hardware_screenshot_console = host if controller.hardware_screenshot_console else None
            if controller.target == "wiiu": controller.recorder.screenshot_console = controller.hardware_screenshot_console
            if controller.preferences_path:
                values = read_preferences(controller.preferences_path)
                values['console'] = host
                controller.preferences_path.parent.mkdir(parents=True, exist_ok=True)
                controller.preferences_path.write_text(json.dumps(values))
            message.set("Wii U address saved. Click Connect, then Start recording.")
        except (OSError, ValueError, RuntimeError) as exc:
            message.set(str(exc))
    ttk.Button(host_row, text="Apply IP", command=apply_host).pack(side="left", padx=8)
    ttk.Button(host_row, text="Open logs folder", command=lambda: open_logs_folder(controller.recorder.directory)).pack(side="left")
    scanning = False
    scan_origin = None
    discovery_results = queue.SimpleQueue()
    auto_scan = tk.BooleanVar(value=bool(read_preferences(controller.preferences_path).get('auto_discover', False)) if controller.preferences_path else False)
    def save_auto_scan():
        try:
            if controller.preferences_path:
                values = read_preferences(controller.preferences_path)
                values['auto_discover'] = auto_scan.get()
                controller.preferences_path.parent.mkdir(parents=True, exist_ok=True)
                controller.preferences_path.write_text(json.dumps(values))
        except OSError as exc:
            message.set(str(exc))
    def scan():
        nonlocal scanning, scan_origin
        if scanning: return
        state = controller.command('status')
        if state['recording']:
            message.set('Stop recording before scanning; manual IP and Connect remain available.')
            return
        if controller.target != 'wiiu':
            message.set('Select Wii U before scanning; Cemu uses its own address.')
            return
        scanning = True
        scan_origin = (controller.console, host_value.get())
        scan_button.state(['disabled'])
        message.set('Scanning the local network for Aroma or FTPiiU...')
        preferred = controller.console
        def worker():
            try: discovery_results.put((discover(preferred), None))
            except Exception as exc: discovery_results.put(([], str(exc)))
        threading.Thread(target=worker, daemon=True).start()
        root.after(100, finish_scan)
    def finish_scan():
        nonlocal scanning
        try: addresses, error = discovery_results.get_nowait()
        except queue.Empty:
            root.after(100, finish_scan)
            return
        scanning = False
        scan_button.state(['!disabled'])
        host_entry.configure(values=addresses)
        if error: message.set('Scan failed: ' + error + '. Enter the IP manually.')
        elif len(addresses) == 1:
            if (controller.command('status')['recording'] or controller.target != 'wiiu'
                    or (controller.console, host_value.get()) != scan_origin):
                message.set('Wii U found: ' + addresses[0] + '. Current recording/address left unchanged; choose it from the list when ready.')
            else:
                host_value.set(addresses[0])
                apply_host()
                message.set('Wii U found and address saved: ' + addresses[0] + '. Click Connect.')
        elif addresses: message.set('Multiple Wii Us found. Choose an IP from the list, then Apply IP.')
        else: message.set('No Wii U found. Enable Aroma TCP logging or FTPiiU, or enter its IP manually.')
    discovery_row = ttk.Frame(panel)
    discovery_row.pack(anchor='w', before=buttons, pady=(5, 0))
    scan_button = ttk.Button(discovery_row, text='Scan for Wii U', command=scan)
    scan_button.pack(side='left')
    ttk.Checkbutton(discovery_row, text='Auto-detect on startup', variable=auto_scan, command=save_auto_scan).pack(side='left', padx=10)
    if auto_scan.get(): root.after(400, scan)

    keys = Hotkeys(pending.put) if hotkey_enabled and os.name != "nt" else None
    if os.name == "nt":
        root.bind('<F8>', lambda event: pending.put('toggle'))
        root.bind('<F9>', lambda event: pending.put('mark'))
    if keys:
        keys.start()

    def act(action):
        try:
            result = controller.command(action)
            if action == "mark":
                message.set(f"Graphics glitch marked ({result['markers']})")
            elif action in ('state-save', 'state-load', 'state-status', 'game-pause', 'game-resume'):
                message.set("Debug request queued; completion and state results appear in the runtime log.")
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
    debug_buttons = ttk.Frame(panel)
    debug_buttons.pack(anchor="w", pady=(0, 8))
    for label, action in (("Save state", "state-save"), ("Load state", "state-load"), ("State status", "state-status"),
                          ("Pause (menu)", "game-pause"), ("Resume", "game-resume")):
        ttk.Button(debug_buttons, text=label, command=lambda a=action: act(a)).pack(side="left", padx=(0, 6))
    speed_choice = tk.StringVar(value="100")
    speed_box = ttk.Combobox(debug_buttons, textvariable=speed_choice, values=("100", "200", "400"), state="readonly", width=5)
    speed_box.pack(side="left")
    ttk.Label(debug_buttons, text="% speed").pack(side="left")
    def change_speed(_event):
        try: controller.command('game-speed', speed_choice.get()); message.set('Game speed request queued; see runtime log.')
        except (OSError, ValueError, RuntimeError) as exc: message.set(str(exc))
    speed_box.bind("<<ComboboxSelected>>", change_speed)
    fallback_choice = tk.BooleanVar(value=False)
    def toggle_fallback():
        requested = fallback_choice.get()
        try:
            controller.command('fallback-rendering', 'on' if requested else 'off')
            message.set('Forced fallback rendering request queued.' if requested else 'Normal rendering request queued.')
        except (OSError, ValueError, RuntimeError) as exc:
            fallback_choice.set(not requested)
            message.set(str(exc))
    ttk.Checkbutton(panel, text='Force fallback rendering (diagnostic; may be slow or show wrong colours)',
                    variable=fallback_choice, command=toggle_fallback).pack(anchor='w', pady=(0, 4))
    aroma_choice = tk.BooleanVar(value=controller.aroma_enabled)
    def toggle_aroma():
        try: controller.command('aroma', 'on' if aroma_choice.get() else 'off')
        except (OSError, ValueError, RuntimeError) as exc: message.set(str(exc))
    aroma_toggle = ttk.Checkbutton(panel, text="Stream Aroma trace", variable=aroma_choice, command=toggle_aroma)
    aroma_toggle.pack(anchor="w")
    capture_choice = tk.BooleanVar(value=controller.recorder.periodic_capture)
    def toggle_capture():
        try: controller.command('screenshots', 'on' if capture_choice.get() else 'off')
        except (OSError, ValueError, RuntimeError) as exc: message.set(str(exc))
    capture_toggle = ttk.Checkbutton(panel, text="Capture GamePad continuously (best effort)",
                                     variable=capture_choice, command=toggle_capture)
    capture_toggle.pack(anchor="w", pady=(4, 0))
    capture_note = tk.StringVar()
    ttk.Label(panel, textvariable=capture_note).pack(anchor="w")
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
        capture_choice.set(state['periodic_capture'])
        capture_toggle.state(['!disabled'] if state['periodic_capture_available'] else ['disabled'])
        capture_note.set((f"{state['periodic_saved']} saved · {state['periodic_failed']} failed · "
                          + (f"{state['captures_per_second']:.2f} captures/s · " if state['captures_per_second'] is not None else "")
                          + "One request at a time; timestamps and log offsets saved."
                          if state['periodic_capture'] else "Captures run only while recording. Failures retry with backoff; logging continues."))
        if keys and keys.error:
            key_note.set("Global keys unavailable. Use the buttons or Python control.")
        elif keys and keys.ready.is_set():
            key_note.set("F8 starts or stops recording. F9 marks a graphics glitch.")
        root.after(100, refresh)

    if os.name == "nt": key_note.set("F8/F9 work while this window has focus. Buttons are always available.")
    refresh()
    try:
        root.mainloop()
    finally:
        if keys:
            keys.done.set()
            keys.join(1)


def open_logs_folder(directory):
    directory.mkdir(parents=True, exist_ok=True)
    if os.name == "nt": os.startfile(str(directory))
    else:
        import subprocess
        subprocess.Popen(["xdg-open", str(directory)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console", default=os.environ.get("WIIU_HOST"))
    parser.add_argument("--target", choices=("wiiu", "cemu"))
    parser.add_argument("--cemu-host", default="127.0.0.1")
    parser.add_argument("--cemu-port", type=int, default=4405)
    parser.add_argument("--output", type=Path, default=DEFAULT_DIR)
    parser.add_argument("--no-hotkeys", action="store_true")
    parser.add_argument("--no-screenshots", action="store_true")
    parser.add_argument("--control", choices=("status", "connect", "disconnect", "start", "stop", "toggle", "mark", "runtime", "renderer", "aroma", "target", "diagnose", "screenshots", "state-save", "state-load", "state-status", "game-pause", "game-resume", "game-speed", "fallback-rendering", "game-input", "game-menu", "game-relaunch"))
    parser.add_argument("--note", default="")
    args = parser.parse_args()
    if args.control:
        result = control(args.control, args.note)
        print(json.dumps(result))
        return int("error" in result)
    preferences = Path(os.environ.get('XDG_CONFIG_HOME', Path.home()/'.config'))/'wiiu-session-recorder/settings.json'
    args.console = args.console or read_preferences(preferences).get("console", "127.0.0.1")
    enabled = read_preferences(preferences).get('aroma_enabled', True)
    if not isinstance(enabled, bool): enabled = True
    target = args.target or read_preferences(preferences).get('target', 'wiiu')
    if target not in ('wiiu', 'cemu'): target = 'wiiu'
    controller = Controller(Recorder(args.output, None if args.no_screenshots else args.console), args.console,
                            aroma_enabled=enabled, preferences_path=preferences, target=target,
                            cemu_host=args.cemu_host, cemu_port=args.cemu_port)
    # Continuous screenshots always require an explicit click in this session.
    with ControlServer(CONTROL, controller) as server:
        control_thread = threading.Thread(target=server.serve_forever, daemon=True)
        control_thread.start()
        try:
            gui(controller, not args.no_hotkeys)
        finally:
            controller.recorder.command('screenshots', 'off')
            server.shutdown()
            controller.command("disconnect")
            controller.command("stop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
