"""Mixed hardware logs keep fragments after CPU geometry records; malformed input fails."""
from pathlib import Path
import struct,tempfile
from tev_to_log import records
from fs_variants import keys
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'mixed.bin';key=struct.pack('<I',1024)
 fs=b'FS  '+struct.pack('<I',len(key))+key
 gs=b'GS  '+struct.pack('<III',505,2,1)+bytes(12)
 vs=b'VS  '+struct.pack('<III',0,1,1)+bytes(8+56)
 gsx=b'GSX '+struct.pack('<I',44)+bytes(44)
 p.write_bytes(fs+gs+vs+gsx+struct.pack('<I',len(key))+key)
 assert records(p)==[key,key] and list(keys(p))==[(1024,),(1024,)]
 for invalid in [fs[:-1],gs[:-1],vs[:-1],gsx[:-1],b'FS  ',struct.pack('<I',0),b'GS  '+struct.pack('<III',0,4097,0),b'x']:
  p.write_bytes(invalid)
  try:records(p)
  except ValueError:pass
  else:raise AssertionError('malformed mixed log accepted')
print('PASS: mixed fragment/vertex/geometry records and truncation/size rejection')
