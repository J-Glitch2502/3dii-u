#!/usr/bin/env python3
"""Extracts the game's PICA shader binaries from its RomFS and describes them (entries, outputs, flow control).
shbin_info.py <game dir> <output dir>"""
import os
import struct
import sys

game, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
rom = open(os.path.join(game, 'romfs.bin'), 'rb')


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


hdr = rom.read(0x28)
dmo, dml, fmo, fml, dat = u32(hdr, 0x0C), u32(hdr, 0x10), u32(hdr, 0x1C), u32(hdr, 0x20), u32(hdr, 0x24)
rom.seek(dmo)
dm = rom.read(dml)
rom.seek(fmo)
fm = rom.read(fml)
found = []


def walk(d, path):
    file = u32(dm, d + 12)
    while file != 0xFFFFFFFF:
        off, size = struct.unpack_from('<QQ', fm, file + 8)
        n = u32(fm, file + 28)
        name = fm[file + 32:file + 32 + n].decode('utf-16le')
        if name.lower().endswith(('.shbin', '.bcsdr')):
            found.append((path + '/' + name, dat + off, size))
        file = u32(fm, file + 4)
    child = u32(dm, d + 8)
    while child != 0xFFFFFFFF:
        n = u32(dm, child + 20)
        walk(child, path + '/' + dm[child + 24:child + 24 + n].decode('utf-16le'))
        child = u32(dm, child + 4)


walk(0, '')
FLOW = {0x21: 'break', 0x22: 'end', 0x23: 'breakc', 0x24: 'call', 0x25: 'callc', 0x26: 'callu', 0x27: 'ifu', 0x28: 'ifc', 0x29: 'loop', 0x2a: 'emit', 0x2b: 'setemit', 0x2c: 'jmpc', 0x2d: 'jmpu'}
for path, off, size in found:
    rom.seek(off)
    d = rom.read(size)
    name = path.strip('/').replace('/', '_')
    open(os.path.join(out, name), 'wb').write(d)
    print(f'{path} {size} bytes')
    if d[:4] != b'DVLB':
        print('  not DVLB:', d[:8])
        continue
    n = u32(d, 4)
    dvles = [u32(d, 8 + 4 * i) for i in range(n)]
    dvlp = 8 + 4 * n
    code_off, code_n, desc_off, desc_n = u32(d, dvlp + 8), u32(d, dvlp + 12), u32(d, dvlp + 16), u32(d, dvlp + 20)
    code = struct.unpack_from(f'<{code_n}I', d, dvlp + code_off)
    flow = {}
    for w in code:
        op = w >> 26
        if op in FLOW:
            flow[FLOW[op]] = flow.get(FLOW[op], 0) + 1
    print(f'  code {code_n} words, swizzle {desc_n}, flow {flow}')
    for i, e in enumerate(dvles):
        b = d[e:]
        kind = b[6]
        main_start, main_end = u32(b, 8), u32(b, 12)
        out_off, out_n = u32(b, 0x20), u32(b, 0x24)
        outs = []
        for k in range(out_n):
            word = struct.unpack_from('<Q', b, out_off + 8 * k)[0]
            outs.append((word & 0xF, (word >> 16) & 0xF, hex((word >> 32) & 0xF)))
        const_off, const_n = u32(b, 0x18), u32(b, 0x1C)
        print(f'  entry {i}: {"geometry" if kind else "vertex"} main {main_start}..{main_end}, {const_n} constants, outputs (type, reg, mask) {outs}')
print(len(found), 'shader files')
