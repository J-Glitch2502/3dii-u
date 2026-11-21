import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from ftplib import error_perm
import ftp_homebrew_dev as dev

class FakeFTP:
    def __init__(self):
        self.dirs = {'/'}
        self.files = {dev.SD + 'wiiu/apps/App.wuhb': b'base assets'}
        self.here = '/'
        self.corrupt = False
        self.fail_activation = False
        self.missing_reply = '550 missing'
    def voidcmd(self, cmd): pass
    def pwd(self): return self.here
    def cwd(self, path):
        if path not in self.dirs: raise error_perm('550 missing')
        self.here = path
    def mkd(self, path):
        if path in self.dirs: raise error_perm('550 exists')
        self.dirs.add(path)
    def size(self, path):
        if path not in self.files: raise error_perm(self.missing_reply)
        return len(self.files[path])
    def storbinary(self, cmd, stream, **kw):
        self.files[cmd[5:]] = stream.read() + (b'bad' if self.corrupt else b'')
    def retrbinary(self, cmd, callback, **kw): callback(self.files[cmd[5:]])
    def rename(self, a, b):
        if self.fail_activation and b.endswith('/active.txt') and a.endswith('.upload'):
            raise error_perm('550 injected activation failure')
        self.files[b] = self.files.pop(a)

class PublishTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        header = bytearray(52);header[:9] = b'\x7fELF\x01\x02\x01\xca\xfe';header[16:20] = b'\xfe\x01\x00\x14'
        self.rpx = self.root/'root.rpx';self.rpx.write_bytes(header)
        self.content = self.root/'content';self.content.mkdir()
        (self.content/'shaders.gsh').write_bytes(b'shader data')
        (self.content/'shaders.idx').write_bytes(b'index')
        self.ftp=FakeFTP()
        self.marker=dev.SD+'wiiu/apps/App.wuhb.dev/active.txt'
        self.ftp.files[self.marker]=b'old\n'
    def publish(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return dev.publish(self.ftp,'wiiu/apps/App.wuhb',self.rpx,self.content,'new-1')
    def test_success(self):
        self.publish();self.assertEqual(self.ftp.files[self.marker],b'new-1\n')
        self.assertEqual(self.ftp.files[dev.SD+'wiiu/apps/App.wuhb'],b'base assets')
        self.assertFalse(any('/save/' in p for p in self.ftp.files))
    def test_readback_failure_leaves_active_unchanged(self):
        self.ftp.corrupt=True
        with self.assertRaises(ValueError):self.publish()
        self.assertEqual(self.ftp.files[self.marker],b'old\n')
    def test_activation_failure_rolls_back(self):
        self.ftp.fail_activation=True
        with self.assertRaises(error_perm):self.publish()
        self.assertEqual(self.ftp.files[self.marker],b'old\n')
    def test_half_shader_pair(self):
        (self.content/'shaders.idx').unlink()
        with self.assertRaises(ValueError):self.publish()
        self.assertEqual(self.ftp.files[self.marker],b'old\n')
    def test_immutable_release(self):
        self.publish()
        with self.assertRaises(error_perm):self.publish()
    def test_first_title_ftpiiu_missing_marker(self):
        del self.ftp.files[self.marker]
        self.ftp.missing_reply='553 No such file or directory'
        self.publish()
        self.assertEqual(self.ftp.files[self.marker],b'new-1\n')
    def test_other_553_errors_are_not_missing(self):
        del self.ftp.files[self.marker]
        self.ftp.missing_reply='553 File name not allowed'
        with self.assertRaises(error_perm):self.publish()
        self.assertNotIn(self.marker,self.ftp.files)
    def test_no_readback_never_downloads_payload_or_marker(self):
        def forbidden(*args,**kwargs):raise AssertionError('No RETR allowed')
        self.ftp.retrbinary=forbidden
        with contextlib.redirect_stdout(io.StringIO()):
            dev.publish(self.ftp,'wiiu/apps/App.wuhb',self.rpx,self.content,'new-1',readback=False)
        self.assertEqual(self.ftp.files[self.marker],b'new-1\n')
    def test_bad_paths(self):
        for p in ['wiiu/apps/../App.wuhb','wiiu/apps//App.wuhb','/wiiu/apps/App.wuhb','wiiu/apps/A\\B.wuhb']:
            with self.assertRaises(ValueError):dev.validate_bundle(p)

if __name__=='__main__':unittest.main()
