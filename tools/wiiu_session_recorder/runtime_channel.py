"""TCP console capture and explicit debug controls, sharing the recorder session."""
import queue
import re
import select
import socket
import threading
import time


def checked_command(command, allow_renderer=False):
    command = command.strip()
    if command in {'st', 'ps', 'stats', 'diag', 'state status', 'list', 'prof'}:
        return command
    match = re.fullmatch(r'mem ([0-9a-fA-F]{1,8}) ([0-9a-fA-F]{1,4})', command)
    if match:
        address, size = (int(n, 16) for n in match.groups())
        if 0 < size <= 4096 and address + size <= 0x100000000:
            return command
    if allow_renderer:
        if command in {'quit', 'relaunch'}: return command
        match = re.fullmatch(r'in (A|B|X|Y|L|R|ZL|ZR|START|SELECT|DUP|DDOWN|DLEFT|DRIGHT|CUP|CDOWN|CLEFT|CRIGHT|WAIT)(?:\+(A|B|X|Y|L|R|ZL|ZR|START|SELECT|DUP|DDOWN|DLEFT|DRIGHT|CUP|CDOWN|CLEFT|CRIGHT))* ([0-9]{1,3})', command)
        if match and 1 <= int(command.rsplit(' ', 1)[1]) <= 600: return command
        if re.fullmatch(r'set fbfast [0123]', command): return command
        if re.fullmatch(r'set (fbuv|fbextra) [01]', command): return command
        match = re.fullmatch(r'ram (read|write) ([0-9a-fA-F]{1,8})(?: ([0-9a-fA-F]{1,8}) ([0-9a-fA-F]{1,8}))?', command)
        if match and int(match[2],16) % 4 == 0 and ((match[1]=='read' and match[3] is None) or (match[1]=='write' and match[3] is not None)): return command
        if command in {'state save', 'state save-ram', 'state load', 'state load-file'}: return command
        if re.fullmatch(r'set speed (100|200|400)', command): return command
        if re.fullmatch(r'set uberall [01]', command): return command
        if re.fullmatch(r'set indexcache [0-3]', command): return command
        match = re.fullmatch(r'set nudge ([0-9]{1,3})', command)
        if match and (int(match[1]) <= 50 or int(match[1]) == 100): return command

        if re.fullmatch(r'set diag [0-3]', command): return command
        match = re.fullmatch(r'set diagthread ([0-9]{1,5})', command)
        if match and int(match[1]) <= 65535: return command
        if re.fullmatch(r'set (cpuvs|vtxcache|drawtrace|inkrefresh|gputime|cputime|avtime|videoworker|bulkcopy|vssimd|poolspin|pause|lightguard|copyrect|gsshared|indexcache) [01]', command): return command
        if re.fullmatch(r'set toplevel [012]', command): return command
        match = re.fullmatch(r'set cpuattr ([0-9]{1,2})', command)
        if match and int(match[1]) <= 15: return command
        match = re.fullmatch(r'set trace ([0-9]{1,3})', command)
        if match and int(match[1]) <= 256: return command
    raise ValueError('Use st, ps, stats, or bounded mem. State changes require explicit debug controls. Explicit renderer controls allow cpuvs/vtxcache/drawtrace/inkrefresh/gputime/cputime/avtime 0|1, toplevel 0..2, cpuattr 0..15 and trace 0..256 only.')


class RuntimeReceiver(threading.Thread):
    STALE_TIMEOUT = 12.0
    def __init__(self, recorder, console, port=4405):
        super().__init__(daemon=True)
        self.recorder, self.console, self.port = recorder, console, port
        self.done = threading.Event()
        self.error = None
        self.commands = queue.Queue(maxsize=32)

    def request(self, command, allow_renderer=False):
        command = checked_command(command, allow_renderer=allow_renderer)
        try:
            self.commands.put_nowait(command)
        except queue.Full:
            raise RuntimeError('Runtime command queue is full.') from None
        return command

    def run(self):
        client = None
        next_connect = last_poll = last_threads = 0
        last_received = time.monotonic()
        pending = ''
        previous_session = None
        owner_samples = {}

        def send(command):
            client.sendall((command + '\n').encode())
            self.recorder.runtime_event('runtime_command', command=command)

        try:
            while not self.done.is_set():
                now = time.monotonic()
                if client is None:
                    if now < next_connect:
                        self.done.wait(min(0.2, next_connect - now))
                        continue
                    next_connect = now + 1
                    try:
                        client = socket.create_connection((self.console, self.port), timeout=0.4)
                        client.settimeout(0.4)
                    except OSError as exc:
                        self.error = str(exc)
                        if client:
                            client.close()
                        client = None
                        continue
                    self.error = None
                    last_received = time.monotonic()
                    self.recorder.runtime_connection(True)
                    previous_session = None
                    pending = ''
                    owner_samples = {}
                    last_poll = last_threads = 0
                try:
                    session = self.recorder.recording_context()
                    if session != previous_session:
                        previous_session = session
                        owner_samples = {}
                        last_poll = last_threads = 0
                    if session:
                        if now - last_poll >= 3:
                            send('st')
                            if getattr(self.recorder, 'target', 'wiiu') == 'cemu':
                                send('stats')
                            else:
                                send('mem 80000000 80')
                            for field in self.recorder.heap_owner_fields():
                                if field != 0x80000020: send(f'mem {field:x} 20')
                            last_poll = now
                        if now - last_threads >= 30:
                            send('ps')
                            last_threads = now
                    elif now - last_poll >= 10:
                        client.sendall(b'\n')  # Keep connected without requesting unsaved snapshots.
                        last_poll = now
                    while not self.commands.empty():
                        try:
                            command = self.commands.get_nowait()
                        except queue.Empty:
                            break
                        send(command)
                    if session and time.monotonic() - last_received > self.STALE_TIMEOUT:
                        raise TimeoutError('Runtime console stopped replying; reconnecting')
                    ready, _, _ = select.select([client], [], [], 0.2)
                    if not ready:
                        continue
                    data = client.recv(65536)
                    if not data:
                        raise ConnectionResetError('Runtime console closed.')
                    last_received = time.monotonic()
                    self.recorder.receive_runtime(data)
                    pending += data.decode(errors='replace')
                    lines = pending.split('\n')
                    pending = lines.pop()[-16384:]
                    if session:
                        for line in lines:
                            match = re.search(r'\[mem\]\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})', line)
                            if not match: continue
                            field, owner = int(match[1], 16), int(match[2], 16)
                            fields = self.recorder.heap_owner_fields()
                            if field not in fields: continue
                            previous_owner, samples, captured_owner = owner_samples.get(field, (0, 0, 0))
                            samples = samples + 1 if owner == previous_owner else 1
                            if not owner: captured_owner = 0
                            if owner and samples >= 2 and owner != captured_owner:
                                self.recorder.runtime_event('persistent_owner_observed', owner=hex(owner), lock=fields[field], owner_field=hex(field))
                                send('ps')
                                if owner <= 0xfffff900 and not (owner & 7): send(f'mem {owner:x} 700')
                                captured_owner = owner
                            owner_samples[field] = owner, samples, captured_owner
                except OSError as exc:
                    self.error = str(exc)
                    client.close()
                    client = None
                    self.recorder.runtime_connection(False)
        finally:
            if client:
                client.close()
            self.recorder.runtime_connection(False)
