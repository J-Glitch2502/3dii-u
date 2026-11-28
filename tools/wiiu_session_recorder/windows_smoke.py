"""Runs without connecting to a user's console or touching their settings."""
import json
from pathlib import Path
import tempfile
import threading
import socket
import tkinter as tk
from unittest.mock import patch
from recorder import Recorder, Controller, WindowsControlServer, control, gui

def run(report):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        controller = Controller(Recorder(root / 'logs'), '127.0.0.1', preferences_path=root/'preferences.json')
        with WindowsControlServer(root / 'control.json', controller) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                assert control('start', path=root/'control.json', windows=True)['recording']
                assert control('mark', 'test marker', path=root/'control.json', windows=True)['markers'] == 1
                assert not control('stop', path=root/'control.json', windows=True)['recording']
                with socket.create_connection(server.server_address, timeout=3) as bad:
                    bad.sendall(b'{"action":"start","token":"wrong"}\n')
                    assert 'error' in json.loads(bad.makefile('rb').readline())
            finally:
                server.shutdown()
                thread.join(2)
        assert not (root/'control.json').exists()
        original = tk.Tk.mainloop
        failures = []
        def limited(window):
            def descendants(widget):
                yield widget
                for child in widget.winfo_children(): yield from descendants(child)
            buttons = {w.cget('text'): w for w in descendants(window) if w.winfo_class() == 'TButton'}
            apply = buttons['Apply IP']
            address = next(w for w in apply.master.winfo_children() if w.winfo_class() == 'TCombobox')
            def later(delay, callback):
                def checked():
                    try: callback()
                    except BaseException as exc:
                        failures.append(exc)
                        window.destroy()
                window.after(delay, checked)
            def check_scan_after_recording():
                assert controller.command('status')['recording']
                assert controller.console == '255.255.255.255'
                controller.command('stop')
                buttons['Scan for Wii U'].invoke()
                later(300, check_scan_applied)
            def check_scan_applied():
                assert controller.console == '192.0.2.49'
                assert json.loads((root/'preferences.json').read_text())['console'] == '192.0.2.49'
                window.destroy()
            def exercise():
                address.set('255.255.255.255'); apply.invoke()
                assert controller.console == '255.255.255.255'
                address.set('256.1.1.1'); apply.invoke()
                assert controller.console == '255.255.255.255'
                address.set('255.255.255.255')
                buttons['Scan for Wii U'].invoke()
                controller.command('start')
                later(300, check_scan_after_recording)
            later(50, exercise)
            window.after(4000, window.destroy)
            original(window)
        with patch('recorder.discover', return_value=['192.0.2.49']), patch.object(tk.Tk, 'mainloop', limited):
            gui(controller, False)
        if failures: raise failures[0]
        sessions = list((root/'logs').glob('*'))
        assert len(sessions) == 2
        assert (sessions[0]/'events.jsonl').exists()
        report.write_text(json.dumps({'platform':__import__('sys').platform, 'gui':'passed', 'ipv4_bounds':'passed', 'scan_button_and_recording_race':'passed', 'control':'authenticated loopback passed', 'record_start_mark_stop':'passed', 'continuous_screenshots':controller.recorder.periodic_capture})+'\n')
