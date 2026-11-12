#!/usr/bin/env python3
"""Turns the console's shader_misses.bin into a log the shader generator reads: misses_to_log.py <shader_misses.bin> <out.bin> [fragment only.bin].
Records are already little-endian: this drops the tags and repeats that the generator does not expect in fs_log.bin.
Fragment shaders depend on render state, not on the game, so other games' builds take the fragment-only file too (build3ds.prepare_logs)."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
known = set()
out = bytearray()
only_fs = bytearray()
p = 0
fs = vs = 0
while p + 8 <= len(d):
    tag = d[p:p + 4]
    if tag == b'FS  ':
        n = struct.unpack_from('<I', d, p + 4)[0]
        key = d[p + 8:p + 8 + n]
        p += 8 + n
        if key not in known:
            known.add(key)
            out += struct.pack('<I', n) + key
            only_fs += struct.pack('<I', n) + key
            fs += 1
    elif tag == b'VS  ':
        entry, nc, ns = struct.unpack_from('<III', d, p + 4)
        body = d[p + 16:p + 16 + (nc + ns) * 4]
        tail = d[p + 16 + (nc + ns) * 4:p + 16 + (nc + ns) * 4 + 56]
        words = struct.unpack(f'<{nc + ns}I', body)
        rec = b'VS  ' + struct.pack('<III', entry, nc, ns) + struct.pack(f'<{nc + ns}I', *words) + tail
        if rec not in known:
            known.add(rec)
            out += rec
            vs += 1
        p += 16 + (nc + ns) * 4 + 56
    else:
        sys.exit(f'unknown record at {p}: {tag!r}')
open(sys.argv[2], 'wb').write(out)
if len(sys.argv) > 3:
    open(sys.argv[3], 'wb').write(only_fs)
print(f'{fs} fragment and {vs} vertex records')
