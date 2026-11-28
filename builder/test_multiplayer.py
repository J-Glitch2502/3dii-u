import json
from pathlib import Path
import tempfile
import unittest
import multiplayer

class MultiplayerDefaultsTests(unittest.TestCase):
    def test_shared_build_does_not_embed_one_players_identity(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            cfg = root / 'defaults.json'
            cfg.write_text(json.dumps({'host':'example.invalid','port':64000}))
            result = multiplayer.write_config(root/'content', cfg)
            text = result.read_text()
            self.assertEqual(result, root/'content'/'3dii-u'/'multiplayer-room.txt')
            self.assertIn('host=example.invalid\nport=64000\n', text)
            self.assertIn('nickname=auto\nconsole_id=auto\n', text)
            # No title identifier or allowlist: every title gets the same endpoint.
            self.assertEqual(multiplayer.write_config(root/'other', cfg).read_text(), text)


    def test_blank_template_defaults_off_with_public_directory(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg=root/'defaults.json'
            cfg.write_text(json.dumps({'host':'','port':64000}))
            result=multiplayer.write_config(root/'card',cfg)
            self.assertTrue(result.read_text().startswith('enabled=0\nhost=\nport=64000\n'))
            self.assertIn(f'web_api_url={multiplayer.DEFAULT_WEB_API}\nweb_api_username=\nnetwork_token=\n',result.read_text())

    def test_existing_shared_endpoint_is_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            cfg = root/'defaults.json'
            cfg.write_text(json.dumps({'host':'example.invalid','port':64000}))
            existing = root/'card'/'3dii-u'/'multiplayer-room.txt'
            existing.parent.mkdir(parents=True)
            existing.write_text('host=127.0.0.1\nport=24872\nweb_api_url=https://custom.example\nnetwork_token=private\n')
            self.assertEqual(multiplayer.write_config(root/'card', cfg), existing)
            self.assertEqual(existing.read_text(), 'host=127.0.0.1\nport=24872\nweb_api_url=https://custom.example\nnetwork_token=private\n')

    def test_missing_or_empty_directory_is_repaired(self):
        for suffix in ('', 'web_api_url=\n'):
            with tempfile.TemporaryDirectory() as d:
                root=Path(d);cfg=root/'defaults.json'
                cfg.write_text(json.dumps({'host':'','port':24872}))
                target=root/'card/3dii-u/multiplayer-room.txt';target.parent.mkdir(parents=True)
                target.write_text('host=custom.example\nnetwork_token=private\n'+suffix)
                multiplayer.write_config(root/'card',cfg)
                self.assertIn('host=custom.example\nnetwork_token=private\n',target.read_text())
                self.assertIn(f'web_api_url={multiplayer.DEFAULT_WEB_API}\n',target.read_text())
                original=target.read_text();multiplayer.write_config(root/'card',cfg)
                self.assertEqual(original,target.read_text())

    def test_bad_endpoint_cannot_inject_room_settings(self):
        for host, port in [('host\nnickname=spoofed',64000), ('host',0), ('host',65536), ('host',True)]:
            with tempfile.TemporaryDirectory() as d:
                root=Path(d);cfg=root/'defaults.json'
                cfg.write_text(json.dumps({'host':host,'port':port}))
                with self.assertRaises(ValueError):
                    multiplayer.write_config(root/'content',cfg)
                self.assertFalse((root/'content').exists())

if __name__ == '__main__': unittest.main()
