import os
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.linux import native_build


class EntryAddressTests(unittest.TestCase):
    def test_versioned_build_uses_program_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root/'builder').mkdir()
            (root/'info.json').write_text(json.dumps({'program_id':'0004000000030800'}))
            (root/'builder/compat.json').write_text(json.dumps({'0004000000030800':{'slug':'g_amke'}}))
            with patch.object(native_build, 'ROOT', root):
                self.assertEqual(native_build.observation_slug(root,'arbitrary_v3152'),'g_amke')
                (root/'builder/compat.json').write_text('{}')
                self.assertEqual(native_build.observation_slug(root,'g_amke_v3152'),'g_amke')

    def test_rejects_desktop_module_addresses_before_building(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'functions.txt'
            path.write_text('0x00100000\n0x40b9a82c\n')
            with patch.object(native_build.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'desktop CRO slots'):
                    native_build.prepare_entries(directory, 0x1c000000)
                run.assert_not_called()
            self.assertIn('0x40b9a82c', path.read_text())

    def test_scanners_share_wiiu_slots_and_preserve_unchanged_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'functions.txt'
            content = '0x00100000\n0x1cb9a82c\n'
            path.write_text(content)
            os.utime(path, ns=(1000000000, 2000000000))
            def scan(command, **kwargs):
                self.assertEqual(kwargs['env']['RECOMP_CRO_BASE'], str(0x1c000000))
                self.assertEqual(command[-1], '--write')
                path.write_text(content)
                return type('Result', (), {'stdout': ''})()
            with patch.object(native_build.subprocess, 'run', side_effect=scan) as run:
                native_build.prepare_entries(directory, 0x1c000000)
                self.assertEqual([Path(c.args[0][1]).name for c in run.call_args_list],
                                 ['find_entries.py', 'cro_entries.py'])
            self.assertEqual(path.stat().st_mtime_ns, 2000000000)

    def test_new_entries_invalidate_code_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'functions.txt'
            path.write_text('0x00100000\n')
            os.utime(path, ns=(1000000000, 2000000000))
            def scan(*args, **kwargs):
                path.write_text('0x00100000\n0x1cb9a82c\n')
                return type('Result', (), {'stdout': ''})()
            with patch.object(native_build.subprocess, 'run', side_effect=scan):
                native_build.prepare_entries(directory, 0x1c000000)
            self.assertNotEqual(path.stat().st_mtime_ns, 2000000000)


if __name__ == '__main__':
    unittest.main()
