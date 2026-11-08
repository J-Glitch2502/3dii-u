#!/usr/bin/env python3
"""A first look at an extracted game: services, memory mode, RomFS layout, CRO modules, and how its code looks at a glance.
analyze_game.py <game dir>. Prints names and counts only, none of the game's data."""
import collections
import json
import os
import struct
import sys

d = sys.argv[1]
info = json.load(open(os.path.join(d, 'info.json')))
ex = open(os.path.join(d, 'exheader.bin'), 'rb').read()
code = open(os.path.join(d, 'code.bin'), 'rb').read()
aci = ex[0x200:]
services = [aci[0x50 + i * 8:0x58 + i * 8].split(b'\0')[0].decode('latin1') for i in range(32)]
print('services:', [s for s in services if s])
print('flags: new3ds', hex(aci[0xD]), 'old3ds-mode byte', hex(aci[0xE]), 'priority', aci[0xF])
print('text', round(info['text']['size'] / 2**20, 2), 'MiB, rodata', round(info['rodata']['size'] / 2**20, 2), 'MiB, data',
      round(info['data']['size'] / 2**20, 2), 'MiB, bss', round(info['bss'] / 2**20, 2), 'MiB')

# ---- ARM or Thumb? ARM words almost always start with the "always" condition, so measure that across text.
text = code[:info['text']['size']]
words = struct.unpack(f'<{len(text) // 4}I', text[:len(text) // 4 * 4])
cond = collections.Counter(w >> 28 for w in words)
total = len(words)
print(f'{cond[14] / total:.1%} of 32-bit words have the ARM "always" condition; ARM-mode code is typically above 70%')
kinds = collections.Counter()
for w in words:
    if w >> 28 == 15:
        continue
    top = (w >> 25) & 7
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        kinds['bx'] += 1
    elif top == 5:
        kinds['bl' if w & (1 << 24) else 'b'] += 1
    elif top == 4:
        kinds['ldm/stm'] += 1
    elif top == 2 or top == 3:
        kinds['load/store'] += 1
    elif top == 6 or top == 7:
        kinds['coprocessor/vfp/svc'] += 1
    else:
        kinds['data processing'] += 1
print('shape of the code, if it were all ARM:', dict(kinds.most_common()))

# ---- RomFS: IVFC container, then level 3 holds the file system
r = open(os.path.join(d, 'romfs.bin'), 'rb')
head = r.read(0x60)
assert head[:4] == b'IVFC', 'expected an IVFC container'
master = struct.unpack('<I', head[8:12])[0]
l3_block = struct.unpack('<I', head[0x4C:0x50])[0]  # log2 of level 3's block size
level3 = (0x60 + master + (1 << l3_block) - 1) & ~((1 << l3_block) - 1)
r.seek(level3)
h = struct.unpack('<10I', r.read(40))
dir_meta_off, dir_meta_size, file_meta_off, file_meta_size, data_off = h[3], h[4], h[7], h[8], h[9]
r.seek(level3 + dir_meta_off)
dirs = r.read(dir_meta_size)
r.seek(level3 + file_meta_off)
meta = r.read(file_meta_size)


def dir_name(off):
    parent, _, _, _, _, name_len = struct.unpack('<6I', dirs[off:off + 24])
    return parent, dirs[off + 24:off + 24 + name_len].decode('utf-16-le')


def path_of(off):
    parts = []
    while off != 0:
        parent, name = dir_name(off)
        parts.append(name)
        off = parent
    return '/'.join(reversed(parts))


files, off = [], 0
while off < len(meta):
    parent, sibling = struct.unpack('<II', meta[off:off + 8])
    data, size = struct.unpack('<QQ', meta[off + 8:off + 24])
    hash_next, name_len = struct.unpack('<II', meta[off + 24:off + 32])
    name = meta[off + 32:off + 32 + name_len].decode('utf-16-le')
    files.append((path_of(parent) + '/' + name, size))
    off += 32 + ((name_len + 3) & ~3)
print('romfs files:', len(files), 'total', round(sum(s for _, s in files) / 2**20, 1), 'MiB')
tops = collections.Counter()
sizes = collections.Counter()
exts = collections.Counter()
for path, size in files:
    top = path.lstrip('/').split('/')[0]
    tops[top] += 1
    sizes[top] += size
    exts[os.path.splitext(path)[1].lower()] += 1
print('top folders:', [(k, tops[k], f'{sizes[k] / 2**20:.1f} MiB') for k in sorted(tops, key=lambda k: -sizes[k])][:16])
print('extensions:', exts.most_common(16))
cros = [(p, s) for p, s in files if p.lower().endswith(('.cro', '.crs'))]
print('CRO/CRS modules:', len(cros), cros[:12])
