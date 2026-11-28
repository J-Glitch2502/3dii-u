from pathlib import Path
import struct,tempfile,unittest
from wuhb_romfs_source import locate,descriptor
class SourceTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.p=Path(self.temp.name)/'a.wuhb'
  dirs=struct.pack('>6I',0,0xffffffff,24,0xffffffff,0xffffffff,0)+struct.pack('>6I',0,0xffffffff,0xffffffff,0,0xffffffff,7)+b'content\0'
  files=struct.pack('>IIQQII',24,0xffffffff,0,4,0xffffffff,9)+b'romfs.bin'+b'\0'*3
  self.b=bytearray(struct.pack('>II9Q',0x57554842,80,80,0,80,len(dirs),80,0,80+len(dirs),len(files),256)+dirs+files)
  self.b.extend(bytes(256-len(self.b)));self.b.extend(b'data');self.p.write_bytes(self.b)
 def test_valid(self):
  self.assertEqual(locate(self.p),(256,4,260));self.assertTrue(descriptor(self.p,'wiiu/apps/Test.wuhb').startswith(b'DROMFS01'))
 def test_short_header(self):
  self.p.write_bytes(b'WUHB')
  with self.assertRaises(ValueError):locate(self.p)
 def test_metadata_bounds(self):
  struct.pack_into('>Q',self.b,56,0xffffffffffffffff);self.p.write_bytes(self.b)
  with self.assertRaises(ValueError):locate(self.p)
 def test_data_bounds(self):
  struct.pack_into('>Q',self.b,136+16,999999);self.p.write_bytes(self.b)
  with self.assertRaises(ValueError):locate(self.p)
 def test_directory_cycle(self):
  struct.pack_into('>I',self.b,104+4,24);self.p.write_bytes(self.b)
  with self.assertRaises(ValueError):locate(self.p,'missing/romfs.bin')
if __name__=='__main__':unittest.main()
