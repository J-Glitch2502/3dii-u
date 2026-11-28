"""Portable checks for partial exports, artwork and replacement failure handling."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

import homebrew


class HomebrewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.rpx = self.base / 'source.rpx'
        self.rpx.write_bytes(b'example executable')
        self.shaders = self.base / 'shaders'
        self.shaders.mkdir()
        for name in ('shaders.gsh', 'shaders.idx'):
            (self.shaders / name).write_bytes(name.encode())
        self.guide = self.base / 'guide.txt'
        self.guide.write_text('Import guide')

    def test_selected_components_only(self):
        for parts in ('binary', 'shaders', 'both'):
            with self.subTest(parts=parts):
                folder = homebrew.export_update(
                    self.rpx if parts != 'shaders' else self.base / 'missing.rpx',
                    self.shaders if parts != 'binary' else self.base / 'missing-shaders',
                    self.base / 'out', 'Example', parts, {'program_id': '123'}, self.guide)
                meta = json.loads((folder / 'update.json').read_text())
                expected = set()
                if parts != 'shaders': expected.add('root.rpx')
                if parts != 'binary': expected.update(('content/shaders.gsh', 'content/shaders.idx'))
                self.assertEqual(set(meta['files']), expected)
                for name, digest in meta['files'].items():
                    self.assertEqual(homebrew.digest(folder / name), digest)
                self.assertEqual(meta['parts'], parts)

    def test_missing_shader_does_not_publish_partial_export(self):
        (self.shaders / 'shaders.idx').unlink()
        with self.assertRaises(ValueError):
            homebrew.export_update(self.rpx, self.shaders, self.base / 'out', 'Example', 'both', {}, self.guide)
        self.assertEqual(list((self.base / 'out').iterdir()), [])

    def test_artwork_origin_and_channels(self):
        # 2x2 image: red, green / blue, white. Test both TGA origins and alpha.
        colors = [(255,0,0,10),(0,255,0,20),(0,0,255,30),(255,255,255,40)]
        for channels in (3,4):
            for flags in (0,16,32,48):
                with self.subTest(channels=channels, flags=flags):
                    pixels = bytearray()
                    for y in range(2):
                        for x in range(2):
                            sx = 1-x if flags&16 else x
                            sy = y if flags&32 else 1-y
                            r,g,b,a = colors[sy*2+sx]
                            pixels.extend((b,g,r,a)[:channels])
                    tga = self.base / 'art.tga'
                    tga.write_bytes(struct.pack('<BBBHHBHHHHBB',0,0,2,0,0,0,0,0,2,2,channels*8,flags)+pixels)
                    png = self.base / 'art.png'
                    homebrew.tga_png(tga,png)
                    data = png.read_bytes(); at = 8; compressed = b''
                    while at < len(data):
                        length, tag = struct.unpack_from('>I4s',data,at)
                        if tag == b'IDAT': compressed += data[at+8:at+8+length]
                        at += 12+length
                    expected = b''.join(b'\0'+bytes(v for color in colors[y*2:y*2+2] for v in color[:channels]) for y in range(2))
                    self.assertEqual(zlib.decompress(compressed),expected)

    def test_failed_bundle_preserves_existing_selection(self):
        title = self.base / 'title'
        (title / 'content').mkdir(parents=True)
        (title / 'meta').mkdir()
        output = self.base / 'Example.wuhb'
        output.write_bytes(b'previous bundle')
        marker = Path(str(output)+'.dev') / 'active.txt'
        marker.parent.mkdir(); marker.write_text('previous-release')
        tool = self.base / 'wuhbtool'; tool.touch()
        with patch.object(homebrew,'tga_png'), patch.object(homebrew.subprocess,'run',side_effect=subprocess.CalledProcessError(1,'tool')):
            with self.assertRaises(subprocess.CalledProcessError):
                homebrew.bundle(self.rpx,title,output,'Example',tool,{},self.guide)
        self.assertEqual(output.read_bytes(),b'previous bundle')
        self.assertEqual(marker.read_text(),'previous-release')
        self.assertFalse(list(self.base.glob('*.new')))


if __name__ == '__main__':
    unittest.main()
