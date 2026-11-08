#!/usr/bin/env python3
"""One image of a run's numbered screen dumps, so a whole run can be seen at a glance: each cell is the top screen over the bottom.
contact_sheet.py <shots dir> <out.png> [columns] [first] [last]"""
import os
import struct
import sys
import zlib

shots, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
first = int(sys.argv[4]) if len(sys.argv) > 4 else 1
last = int(sys.argv[5]) if len(sys.argv) > 5 else 10**9


def ppm(path):
    d = open(path, 'rb').read()
    f = d.split(maxsplit=4)
    w, h = int(f[1]), int(f[2])
    return w, h, d[-w * h * 3:]


nums = sorted(int(n[3:-4]) for n in os.listdir(shots) if n.startswith('top') and n.endswith('.ppm') and n[3:-4].isdigit())
nums = [n for n in nums if first <= n <= last]
cw, ch = 200, 240
rows = (len(nums) + cols - 1) // cols
W, H = cols * (cw + 4), rows * (ch + 4)
img = bytearray(b'\x40' * (W * H * 3))


def blit(w, h, pix, x0, y0):
    for y in range(0, h, 2):
        s = y * w * 3
        d = ((y0 + y // 2) * W + x0) * 3
        for x in range(0, w, 2):
            img[d:d + 3] = pix[s + x * 3:s + x * 3 + 3]
            d += 3


for i, n in enumerate(nums):
    x0, y0 = (i % cols) * (cw + 4) + 2, (i // cols) * (ch + 4) + 2
    w, h, p = ppm(os.path.join(shots, f'top{n}.ppm'))
    blit(w, h, p, x0, y0)
    w, h, p = ppm(os.path.join(shots, f'bottom{n}.ppm'))
    blit(w, h, p, x0 + 20, y0 + 120)
raw = bytearray()
for y in range(H):
    raw += b'\0' + img[y * W * 3:(y + 1) * W * 3]


def chunk(tag, body):
    return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))


open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
print(len(nums), 'screens', W, 'x', H)
