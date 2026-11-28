"""Synthetic title IDs and fake NAND folders: rejected packs must not touch existing tickets."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'tools'))
sys.path.insert(0, str(ROOT/'builder'))
import pipeline
import ship_card
import wup_safety as safety
from test_pipeline_outputs import fake_tmd

KEY = '000102030405060708090a0b0c0d0e0f'


class TicketSafety(unittest.TestCase):
    def test_ids(self):
        compat = json.loads((ROOT/'builder/compat.json').read_text())
        for entry in compat.values():
            if isinstance(entry, dict) and 'title_id' in entry:
                title_id = int(entry['title_id'], 16)
                safety.check_title_id(title_id)
                safety.check_title_id(title_id | (0xe << 32))
        for title_id in (0x000500003d000000, 0x000500003dffffff, 0x0005000e3d008630):
            safety.check_title_id(title_id)
        for title_id in (0x0005000010101f00, 0x0005000e10101f00, 0x0005001010040100,
                         0x0005001b10054000, 0x0005000c3d008630, 0x000500103d008630,
                         0x0005000010334400, -1, 0x1000500003d008630):
            with self.assertRaises(ValueError):
                safety.check_title_id(title_id)

    def test_pack_rejections_preserve_files(self):
        with tempfile.TemporaryDirectory(prefix='3dii-ticket-') as directory:
            root = Path(directory)
            source = root/'source'
            (source/'code').mkdir(parents=True)
            (source/'code/cos.xml').write_text('<app/>')
            (source/'meta').mkdir()
            for name in ('bootMovie.h264', 'bootLogoTex.tga', 'iconTex.tga'):
                (source/'meta'/name).write_bytes(b'synthetic metadata image')
            (source/'content').mkdir()
            (source/'content/data.bin').write_bytes(b'synthetic game data')
            out = root/'wup'
            out.mkdir()
            old = out/'title.tik'
            old.write_bytes(b'existing synthetic ticket')

            def pack(title_id, meta_id=None, app_type='80000000', target=out, extra=''):
                fields = dict(title_id=title_id, group_id='00003d00', app_type=app_type,
                              os_version='000500101000400a', title_version='0000')
                app = '<app>'+''.join(f'<{key} type="hexBinary">{value}</{key}>' for key, value in fields.items())+extra+'</app>'
                (source/'code/app.xml').write_text(app)
                (source/'meta/meta.xml').write_text(f'<menu><title_id type="hexBinary">{meta_id or title_id}</title_id></menu>')
                return subprocess.run([sys.executable, '-E', '-P', str(ROOT/'phase0/tools/pack_wup.py'),
                    str(source), str(target)], input=KEY, text=True, capture_output=True)

            cases = [('0005000010101f00', None, '80000000'), ('0005001010040100', None, '80000000'),
                     ('000500003d008630', '0005000010101f00', '80000000'),
                     ('000500003d008630', None, '10000009'), ('0005000e3d008630', None, '80000000')]
            for title_id, meta_id, app_type in cases:
                result = pack(title_id, meta_id, app_type)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(old.read_bytes(), b'existing synthetic ticket')
                self.assertEqual(list(out.iterdir()), [old])
            result = pack('000500003d008630', extra='<title_id type="hexBinary">0005000010101f00</title_id>')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(old.read_bytes(), b'existing synthetic ticket')
            for target in (source, source/'nested', root, root/'NaNd'/'wup', root/'rights'/'TiCkEt'/'wup'):
                result = pack('000500003d008630', target=target)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue((source/'code/app.xml').is_file())
                self.assertEqual(old.read_bytes(), b'existing synthetic ticket')
            emulator = root/'Cemu'
            nested_ticket = emulator/'mlc01/rights/ticket/apps/existing.tik'
            nested_ticket.parent.mkdir(parents=True)
            nested_ticket.write_bytes(b'existing emulator NAND ticket')
            result = pack('000500003d008630', target=emulator)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(nested_ticket.read_bytes(), b'existing emulator NAND ticket')
            with self.assertRaises(ValueError):
                with pipeline.staged_output(str(emulator)):
                    self.fail('NAND parent was staged for replacement')
            self.assertEqual(nested_ticket.read_bytes(), b'existing emulator NAND ticket')
            # This is an ordinary generated update: its own ticket is expected and round-trip verified.
            (out/'title.tmd').write_bytes(fake_tmd(b'previous update', 0x0005000e3d008630))
            result = pack('0005000e3d008630', meta_id='000500003d008630', app_type='0800001b')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('round trip verified', result.stdout)
            self.assertEqual(int.from_bytes(old.read_bytes()[0x1dc:0x1e4], 'big'), 0x0005000e3d008630)
            # The same title/version with a different file table must get a new
            # content ID; otherwise native installation can retain the old FST.
            previous_fst_id = (out/'title.tmd').read_bytes()[0xb04:0xb08]
            import json
            (source/'.3dii-base-links.json').write_text(json.dumps({
                'base_title_id':'000500003d008630', 'files':{'romfs.bin':17<<20,'shaders.idx':1024}}))

            (source/'content/extra.bin').write_bytes(b'new update payload')
            result = pack('0005000e3d008630', meta_id='000500003d008630', app_type='0800001b')
            self.assertEqual(result.returncode, 0, result.stderr)
            tmd = (out/'title.tmd').read_bytes()
            self.assertNotEqual(tmd[0xb04:0xb08], previous_fst_id)
            self.assertNotEqual(int.from_bytes(tmd[0xb04:0xb08],'big'), int.from_bytes(tmd[0xb08:0xb0a],'big'))
            saved = {f.name: f.read_bytes() for f in out.iterdir()}
            result = pack('000500003d008631')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('A different Wii U title', result.stderr)
            self.assertEqual({f.name: f.read_bytes() for f in out.iterdir()}, saved)

    def test_builder_rejects_before_staging(self):
        with tempfile.TemporaryDirectory(prefix='3dii-ticket-') as directory:
            root = Path(directory)
            source = root/'game'
            source.mkdir()
            destination = root/'nand'/'tickets'
            destination.mkdir(parents=True)
            ticket = destination/'title.tik'
            ticket.write_bytes(b'keep this ticket')
            with mock.patch.object(pipeline, 'staged_output', side_effect=AssertionError('staging must not begin')):
                with self.assertRaises(SystemExit):
                    pipeline.pack_wup('unused', str(source), str(destination))
                with self.assertRaises(SystemExit):
                    pipeline.copy_to(str(source), str(destination))
                with self.assertRaises(SystemExit):
                    pipeline.check_card(str(root/'nand'))
            with mock.patch.object(ship_card.shutil, 'rmtree', side_effect=AssertionError('deletion must not begin')):
                with self.assertRaises(ValueError):
                    ship_card.copy(str(source), str(root/'nand'))
            self.assertEqual(ticket.read_bytes(), b'keep this ticket')
            self.assertEqual(list(destination.iterdir()), [ticket])

    def test_hardlinked_output_preserves_ticket_identity(self):
        with tempfile.TemporaryDirectory(prefix='3dii-ticket-') as directory:
            root = Path(directory)
            ticket = root/'original.tik'
            ticket.write_bytes(b'keep this synthetic ticket and its metadata')
            output = root/'wup'
            output.mkdir()
            os.link(ticket, output/'title.tik')
            before = ticket.stat()
            with self.assertRaises(ValueError):
                with pipeline.staged_output(str(output)):
                    self.fail('a shared ticket must be rejected before staging or cleanup')
            after = ticket.stat()
            self.assertEqual(ticket.read_bytes(), b'keep this synthetic ticket and its metadata')
            self.assertEqual((after.st_ino, after.st_nlink, after.st_mtime_ns),
                             (before.st_ino, before.st_nlink, before.st_mtime_ns))
            self.assertTrue(os.path.samefile(ticket, output/'title.tik'))
            self.assertEqual([p.name for p in output.iterdir()], ['title.tik'])

    def test_resolved_alias(self):
        with tempfile.TemporaryDirectory(prefix='3dii-ticket-') as directory:
            root = Path(directory)
            protected = root/'nand'
            protected.mkdir()
            alias = root/'ordinary-output'
            if sys.platform == 'win32':
                result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(protected)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                alias.symlink_to(protected, target_is_directory=True)
            try:
                with self.assertRaises(ValueError):
                    safety.check_output(alias/'Game')
            finally:
                if sys.platform == 'win32':
                    alias.rmdir()
                else:
                    alias.unlink()
            # An alias to an otherwise valid WUP is also not a replaceable output.
            ordinary = root/'ordinary'
            ordinary.mkdir()
            (ordinary/'title.tik').write_bytes(b'keep alias target')
            if sys.platform == 'win32':
                result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(ordinary)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                alias.symlink_to(ordinary, target_is_directory=True)
            try:
                with self.assertRaises(ValueError):
                    safety.check_output(alias, replace=True)
                self.assertEqual((ordinary/'title.tik').read_bytes(), b'keep alias target')
            finally:
                if sys.platform == 'win32':
                    alias.rmdir()
                else:
                    alias.unlink()


if __name__ == '__main__':
    unittest.main()
