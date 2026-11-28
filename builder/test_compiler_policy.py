"""Actual builder compiler policy and learned-input isolation; title construction is mocked."""
from contextlib import ExitStack
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import pipeline

class PolicyComplete(Exception):
    pass


class CompilerPolicy(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix='3dii_compiler_policy_')
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)

    def test_compiler_policy_and_discovery_without_learned_assets(self):
        import json
        import types
        with open(Path(__file__).with_name('compat.json'), encoding='utf-8') as f:
            profile = json.load(f)['0004000000033500']
        self.assertTrue(profile['mixed'])
        engine = self.root / 'engine'
        (engine/'builder').mkdir(parents=True)
        (engine/'builder/multiplayer-defaults.json').write_text(
            Path(__file__).with_name('multiplayer-defaults.json').read_text())
        fake_builder = engine / 'tools/win/build3ds.py'
        fake_builder.parent.mkdir(parents=True)
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
def prepare_logs(*args): pass
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
                    stack.enter_context(mock.patch.object(pipeline, 'make_title', side_effect=PolicyComplete))
                    commands = stack.enter_context(mock.patch.object(pipeline.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '')))
                    with self.assertRaises(PolicyComplete):
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
