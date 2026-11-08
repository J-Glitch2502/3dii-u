#!/usr/bin/env python3
"""Writes a binary PPM (as the PC build's screen dumps) out as a PNG, doubled in size. ppm2png.py <in.ppm> <out.png>"""
import struct
import sys
import zlib

data = open(sys.argv[1], 'rb').read()
fields = data.split(maxsplit=4)
assert fields[0] == b'P6'
w, h = int(fields[1]), int(fields[2])
pix = data[-w * h * 3:]
rows = bytearray()
for y in range(h):
    row = pix[y * w * 3:(y + 1) * w * 3]
    wide = b''.join(row[i:i + 3] * 2 for i in range(0, len(row), 3))
    rows += (b'\0' + wide) * 2


def chunk(tag, body):
    return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * 2, h * 2, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
