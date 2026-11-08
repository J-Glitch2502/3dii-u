#!/usr/bin/env python3
"""Replaces romfs.bin with just its level 3 (the file system itself), dropping the IVFC hash layers, as the runtime expects.
strip_romfs.py <game dir>. Does nothing if the file is already stripped."""
import os
import struct
import sys

path = os.path.join(sys.argv[1], 'romfs.bin')
with open(path, 'rb') as f:
    head = f.read(0x60)
    if head[:4] != b'IVFC':
        print('already stripped')
        sys.exit(0)
    master = struct.unpack('<I', head[8:12])[0]
    l3_size = struct.unpack('<Q', head[0x44:0x4C])[0]
    l3_block = struct.unpack('<I', head[0x4C:0x50])[0]
    level3 = (0x60 + master + (1 << l3_block) - 1) & ~((1 << l3_block) - 1)
    tmp = path + '.tmp'
    with open(tmp, 'wb') as o:
        f.seek(level3)
        left = l3_size
        while left:
            block = f.read(min(1 << 22, left))
            assert block, 'romfs ended early'
            o.write(block)
            left -= len(block)
with open(tmp, 'rb') as f:
    assert struct.unpack('<I', f.read(4))[0] == 0x28, 'level 3 should start with a 0x28-byte header'
os.replace(tmp, path)
print(f'stripped to level 3: {l3_size} bytes')
