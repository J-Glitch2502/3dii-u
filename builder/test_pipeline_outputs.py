"""Failure injection for replacing a built WUP or its SD-card copy; all files are synthetic."""
import os
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from contextlib import ExitStack
from unittest import mock

import pipeline


def fake_tmd(marker, title_id=0x000500003d000863):
    issuer = b'Root-CA00000003-CP0000000b'.ljust(0x40, b'\0')
    record = struct.pack('>IHHQ', 0, 0, 1, len(marker)) + hashlib.sha1(marker).digest().ljust(0x20, b'\0')
    infos = (struct.pack('>HH', 0, 1) + hashlib.sha256(record).digest()).ljust(0x900, b'\0')
    header = bytearray(0x204)
    struct.pack_into('>I', header, 0, 0x00010004)
    header[0x140:0x180] = issuer
    struct.pack_into('>Q', header, 0x18C, title_id)
    struct.pack_into('>H', header, 0x1DE, 1)
    header[0x1E4:] = hashlib.sha256(infos).digest()
    return bytes(header) + infos + record


class OutputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='3diiu-output-test-')
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / 'source' / 'Game'
        self.target = self.root / 'output' / 'Game'
        for folder, data in ((self.source, b'new'), (self.target, b'old')):
            folder.mkdir(parents=True)
            (folder / 'title.tmd').write_bytes(fake_tmd(data))

    def tearDown(self):
        assert self.source.resolve().is_relative_to(self.root) and self.target.resolve().is_relative_to(self.root)
        self.temp.cleanup()

    def test_disk_estimate_for_unpacked_cemu_checks(self):
        gib = 1 << 30
        base = mock.Mock(size=3 * gib, kind='game')
        update = mock.Mock(size=gib // 2, kind='update')
        dlc = mock.Mock(size=gib // 4, kind='dlc')
        extras = [update, dlc]
        self.assertEqual(pipeline.required_disk(base, extras, [], True), 7 * gib)
        self.assertEqual(pipeline.required_disk(base, extras, [base, dlc], True), 10 * gib)
        self.assertEqual(pipeline.required_disk(base, extras, [base, dlc], False), 69 * gib // 4)

    def old_intact(self):
        self.assertTrue(self.target.is_dir(), 'the last good output was removed')
        self.assertEqual((self.target / 'title.tmd').read_bytes(), fake_tmd(b'old'))

    def test_copy_rename_failure(self):
        rename = os.rename

        def refuse_new(src, dst):
            if Path(dst) == self.target and Path(src).name != 'previous':
                raise PermissionError('injected destination lock')
            return rename(src, dst)

        with mock.patch.object(pipeline.os, 'rename', side_effect=refuse_new):
            with self.assertRaises(PermissionError):
                pipeline.copy_to(str(self.source), str(self.target.parent))
        self.old_intact()

    def test_failed_pack(self):
        def broken(argv, **kwargs):
            out = Path(argv[-1])
            out.mkdir(exist_ok=True)
            (out / 'title.tmd').write_bytes(b'incomplete')
            return subprocess.CompletedProcess(argv, 1, 'injected pack failure')

        with mock.patch.object(pipeline.subprocess, 'run', side_effect=broken):
            with self.assertRaises(SystemExit):
                pipeline.pack_wup('unused', 'unused', str(self.target))
        self.old_intact()

    def test_cancelled_pack(self):
        with mock.patch.object(pipeline.subprocess, 'run', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                pipeline.pack_wup('unused', 'unused', str(self.target))
        self.old_intact()

    def test_copy_success(self):
        pipeline.copy_to(str(self.source), str(self.target.parent))
        self.assertEqual((self.target / 'title.tmd').read_bytes(), fake_tmd(b'new'))
        self.assertEqual([p for p in self.target.parent.iterdir() if p.is_dir()], [self.target])

    def test_copy_verification_failure(self):
        with mock.patch.object(pipeline.filecmp, 'cmpfiles', return_value=([], ['title.tmd'], [])):
            with self.assertRaises(SystemExit):
                pipeline.copy_to(str(self.source), str(self.target.parent))
        self.old_intact()
        self.assertEqual([p for p in self.target.parent.iterdir() if p.is_dir()], [self.target])

    def test_copy_failed_rollback_keeps_backup(self):
        rename = os.rename

        def refuse_destination(src, dst):
            if Path(dst) == self.target:
                raise PermissionError('injected destination lock including rollback')
            return rename(src, dst)

        with mock.patch.object(pipeline.os, 'rename', side_effect=refuse_destination):
            with self.assertRaisesRegex(OSError, 'safe at'):
                pipeline.copy_to(str(self.source), str(self.target.parent))
        backups = list(self.target.parent.glob('.3diiu-*/previous/title.tmd'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), fake_tmd(b'old'))

    def test_hard_stop_during_swap_keeps_backup(self):
        script = '''
import os, sys
import pipeline
rename = os.rename
def stop_after_backup(src, dst):
    rename(src, dst)
    if os.path.basename(dst) == 'previous':
        os._exit(23)
pipeline.os.rename = stop_after_backup
pipeline.copy_to(sys.argv[1], sys.argv[2])
'''
        r = subprocess.run([pipeline.sys.executable, '-c', script, str(self.source), str(self.target.parent)],
                           cwd=Path(__file__).parent, capture_output=True, text=True)
        self.assertEqual(r.returncode, 23, r.stderr)
        backups = list(self.target.parent.glob('.3diiu-*/previous/title.tmd'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), fake_tmd(b'old'))
        self.assertIn(str(backups[0].parent), r.stdout)

    def test_pack_success(self):
        def packed(argv, **kwargs):
            out = Path(argv[-1])
            out.mkdir()
            (out / 'title.tmd').write_bytes(fake_tmd(b'new'))
            return subprocess.CompletedProcess(argv, 0, 'packed')

        with mock.patch.object(pipeline.subprocess, 'run', side_effect=packed):
            pipeline.pack_wup('unused', 'unused', str(self.target))
        self.assertEqual((self.target / 'title.tmd').read_bytes(), fake_tmd(b'new'))
        self.assertEqual([p for p in self.target.parent.iterdir() if p.is_dir()], [self.target])

    def test_failed_retry_preserves_wup_left_by_card_failure(self):
        engine, build = self.root / 'engine', self.root / 'build'
        fake_builder = engine / 'tools' / 'win' / 'build3ds.py'
        fake_builder.parent.mkdir(parents=True)
        (engine/'builder').mkdir(parents=True)
        (engine/'builder/multiplayer-defaults.json').write_text(
            Path(__file__).with_name('multiplayer-defaults.json').read_text())
        (fake_builder.parent / 'gcc.exe').touch()
        fake_builder.write_text('''
import os, threading
from types import SimpleNamespace
BIN = os.path.dirname(__file__) + os.sep
MEASURE = MEASURE_CONSOLE = False
CRO_BASE = '08000000'
context = threading.local()
def Game(name, directory):
    return SimpleNamespace(dir=directory, mode='abi', extra_functions=directory + '/functions.txt', upstream_functions=None)
def prepare_vertex_logs(*args): pass
def build_shadergen(*args): return 'unused'
def build_code(*args): pass
def build_shaders(*args): pass
''')

        class Game:
            path, kind, size = 'made-up.3ds', 'game', 0
            title_id, unique = 0x0004000000086300, 0x00086300
            product, name = 'CTR-P-TEST', 'Retry test'
            encrypted = lambda self: False
            dlp_source = lambda self: None
            read_names = lambda self: None
            f = mock.Mock()

        class ExtractionFailed(Exception):
            pass

        def extract(game, folder, progress):
            (Path(folder) / 'code.bin').write_bytes(b'synthetic code')

        def run(argv, **kwargs):
            if Path(argv[1]).name == 'pack_game_wup.py':
                out = Path(argv[-1])
                out.mkdir()
                (out / 'title.tmd').write_bytes(fake_tmd(b'completed WUP'))
            return subprocess.CompletedProcess(argv, 0, '')

        with ExitStack() as stack:
            for owner, name, value in ((pipeline, 'ROOT', str(engine)), (pipeline, 'BUILD', str(build)), (pipeline, 'WORK', str(build)),
                                      (pipeline, 'WORK_GAMES', str(build / 'builder_work')), (pipeline, 'ARCHIVES', str(build / 'archives')),
                                      (pipeline.sys, 'platform', 'win32'),
                                      (pipeline.sys, 'argv', ['pipeline.py', '--game', 'made-up.3ds', '--otp', 'unused', '--card', str(self.root / 'card')])):
                stack.enter_context(mock.patch.object(owner, name, value))
            for owner, name, value in ((pipeline, 'profile_for', None), (pipeline.user_files, 'check_otp', None),
                                      (pipeline.dumps, 'Dump', Game()), (pipeline.dumps, 'have_system_archives', True),
                                      (pipeline.dumps, 'archives_named', []), (pipeline.shaderlogs, 'restore', (0, 0)),
                                      (pipeline.shutil, 'disk_usage', mock.Mock(free=1 << 40))):
                stack.enter_context(mock.patch.object(owner, name, return_value=value))
            stack.enter_context(mock.patch.object(pipeline.wup_update, 'remember_base', return_value={}))
            stack.enter_context(mock.patch.object(pipeline, 'make_title'))
            stack.enter_context(mock.patch.object(pipeline.mods, 'apply'))
            extraction = stack.enter_context(mock.patch.object(pipeline.dumps, 'extract_program', side_effect=extract))
            stack.enter_context(mock.patch.object(pipeline.subprocess, 'run', side_effect=run))
            stack.enter_context(mock.patch.object(pipeline, 'copy_to', side_effect=OSError('injected card failure')))
            with self.assertRaises(SystemExit):
                pipeline.main()
            completed = list(build.rglob('title.tmd'))
            self.assertEqual(len(completed), 1)
            extraction.side_effect = ExtractionFailed
            with self.assertRaises(ExtractionFailed):
                pipeline.main()
            self.assertTrue(completed[0].is_file(), 'retry discarded the completed WUP before producing its replacement')
            self.assertEqual(completed[0].read_bytes(), fake_tmd(b'completed WUP'))

    def test_compiler_policy_and_discovery_without_learned_assets(self):
        import json
        import types
        with open(Path(__file__).with_name('compat.json'), encoding='utf-8') as f:
            profile = json.load(f)['0004000000033500']
        self.assertTrue(profile['mixed'])
        engine = self.root / 'engine'
        fake_builder = engine / 'tools/win/build3ds.py'
        fake_builder.parent.mkdir(parents=True)
        (engine/'builder').mkdir(parents=True)
        (engine/'builder/multiplayer-defaults.json').write_text(
            Path(__file__).with_name('multiplayer-defaults.json').read_text())
        fake_builder.with_name('gcc.exe').touch()
        fake_builder.write_text('''
import os, threading
BIN = os.path.dirname(__file__) + os.sep
MEASURE = MEASURE_CONSOLE = False
CRO_BASE = '08000000'
context = threading.local()
class Game:
    def __init__(self, name, directory):
        self.dir, self.mode, self.mixed_called = directory, 'safe', False
        self.extra_functions = directory + '/functions.txt'
        self.upstream_functions = os.path.dirname(BIN.rstrip(os.sep)) + '/upstream.txt'
        self.pipelines = 'upstream pipelines'
    def mixed(self): self.mixed_called, self.mode = True, 'abi'
def prepare_vertex_logs(*args): pass
def build_shadergen(*args): return 'unused'
def build_code(g, args):
    global built
    built = g
def build_shaders(*args): pass
''')
        upstream = engine / 'tools/upstream.txt'
        learned = engine / 'games' / profile['slug'] / 'functions.txt'
        learned.parent.mkdir(parents=True)

        class Dump:
            path, kind, size, version = 'made-up.3ds', 'game', 0, 1
            title_id, unique = 0x0004000000033500, 0x00033500
            product, name = 'CTR-P-AQEE', 'Policy test'
            encrypted = lambda self: False
            dlp_source = lambda self: None
            read_names = lambda self: None
            f = mock.Mock()

        game, update = Dump(), Dump()
        update.kind = 'update'
        # (cold, update, explicit mixed, code mod, local entries, upstream entries, mixed expected, discovery expected)
        cases = [
            (False, False, False, False, None, None, True, True),
            (False, False, False, False, '', '', True, True),
            (False, False, False, False, '0x100000\n', None, True, False),
            (False, False, False, False, None, '0x100000\n', True, False),
            (True, False, False, False, '0x100000\n', '0x100000\n', True, True),
            (False, True, False, False, '0x100000\n', '0x100000\n', False, True),
            (True, True, False, False, '0x100000\n', '0x100000\n', False, True),
            (False, True, True, False, None, None, True, True),
            (False, False, False, True, '0x100000\n', '0x100000\n', False, True),
            (False, False, True, True, None, None, True, True),
        ]
        for number, (cold, updated, explicit, changed, local, other, mixed, discover) in enumerate(cases):
            with self.subTest(case=number):
                for path, text in ((learned, local), (upstream, other)):
                    path.unlink(missing_ok=True)
                    if text is not None:
                        path.write_text(text)
                build = self.root / f'build-{number}'
                argv = ['pipeline.py', '--game', 'made-up.3ds', '--otp', 'unused', '--out', str(self.root / 'out'), '--keep']
                argv += (['--cold'] if cold else []) + (['--update', 'update.cia'] if updated else [])
                argv += (['--mixed'] if explicit else []) + (['--mod', 'mod.zip'] if changed else [])
                with ExitStack() as stack:
                    for owner, name, value in ((pipeline, 'ROOT', str(engine)), (pipeline, 'WORK', str(engine)),
                            (pipeline, 'BUILD', str(build)), (pipeline, 'WORK_GAMES', str(build / 'builder_work')),
                            (pipeline, 'ARCHIVES', str(build / 'archives')), (pipeline.sys, 'argv', argv), (pipeline.sys, 'platform', 'win32')):
                        stack.enter_context(mock.patch.object(owner, name, value))
                    for owner, name, value in ((pipeline, 'profile_for', profile), (pipeline.user_files, 'check_otp', None),
                            (pipeline.dumps, 'have_system_archives', True), (pipeline.dumps, 'archives_named', []),
                            (pipeline.dumps, 'add_update', {'program_id': 'synthetic'}), (pipeline.shaderlogs, 'restore', (0, 0)),
                            (pipeline.shutil, 'disk_usage', mock.Mock(free=1 << 40))):
                        stack.enter_context(mock.patch.object(owner, name, return_value=value))
                    stack.enter_context(mock.patch.object(pipeline.dumps, 'Dump', side_effect=lambda p: update if p == 'update.cia' else game))
                    stack.enter_context(mock.patch.object(pipeline.dumps, 'extract_program', side_effect=lambda d, p, _: (Path(p) / 'code.bin').write_bytes(b'code')))
                    stack.enter_context(mock.patch.object(pipeline.mods, 'Mod', return_value=types.SimpleNamespace(changes_code=lambda: changed, close=lambda: None)))
                    stack.enter_context(mock.patch.object(pipeline.mods, 'apply'))
                    stack.enter_context(mock.patch.object(pipeline.wup_update, 'remember_base', return_value={}))
                    stack.enter_context(mock.patch.object(pipeline, 'make_title'))
                    stack.enter_context(mock.patch.object(pipeline, 'pack_wup'))
                    commands = stack.enter_context(mock.patch.object(pipeline.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '')))
                    pipeline.main()
                    built = pipeline.sys.modules['build3ds'].built
                    self.assertEqual(built.mixed_called, mixed)
                    self.assertEqual([Path(c.args[0][1]).name for c in commands.call_args_list],
                                     ['find_entries.py', 'cro_entries.py'] if discover else [])
                    self.assertEqual(Path(built.extra_functions).exists(), local is not None and bool(local) and not (cold or updated or changed))
                    if cold or updated or changed:
                        self.assertIsNone(built.upstream_functions)
                    if cold:
                        self.assertIsNone(built.pipelines)


if __name__ == '__main__':
    unittest.main()
