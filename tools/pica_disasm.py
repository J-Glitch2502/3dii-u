#!/usr/bin/env python3
"""A PICA200 shader disassembler, for reading a program the runtime logged: pica_disasm.py <log> [entry]
The log holds "[gscode] <word offset> <words...>" and "[gsswz] <offset> <words...>" lines, as a measuring build writes them for a
geometry shader it runs on the CPU (third_party/3dsrecomp/runtime/gpu/vertex.cpp). Prints from entry (default 0) to the first END."""
import re
import sys

ARITH = {0x00: 'add', 0x01: 'dp3', 0x02: 'dp4', 0x03: 'dph', 0x04: 'dst', 0x05: 'ex2', 0x06: 'lg2', 0x07: 'litp', 0x08: 'mul',
         0x09: 'sge', 0x0a: 'slt', 0x0b: 'flr', 0x0c: 'max', 0x0d: 'min', 0x0e: 'rcp', 0x0f: 'rsq', 0x12: 'mova', 0x13: 'mov'}
ARITHI = {0x18: 'dphi', 0x1a: 'sgei', 0x1b: 'slti'}
FLOW = {0x20: 'break', 0x21: 'nop', 0x22: 'end', 0x23: 'breakc', 0x24: 'call', 0x25: 'callc', 0x26: 'callu', 0x27: 'ifu',
        0x28: 'ifc', 0x29: 'loop', 0x2a: 'emit', 0x2b: 'setemit', 0x2c: 'jmpc', 0x2d: 'jmpu'}
CMPOPS = ['eq', 'ne', 'lt', 'le', 'gt', 'ge', '?6', '?7']
IDX = ['', '[a0.x]', '[a0.y]', '[aL]']


def src(r, idx=0):
    if r < 0x10:
        name = f'v{r}'
    elif r < 0x20:
        name = f'r{r - 0x10}'
    else:
        name = f'c{r - 0x20}'
    return name + (IDX[idx] if r >= 0x20 else '')


def dst(r):
    return f'o{r}' if r < 0x10 else f'r{r - 0x10}'


def swz(desc, which):
    shift, negbit = [(5, 4), (14, 13), (23, 22)][which]
    sel = (desc >> shift) & 0xff
    s = ''.join('xyzw'[(sel >> (6 - 2 * i)) & 3] for i in range(4))
    return ('-' if desc >> negbit & 1 else '') + ('' if s == 'xyzw' else '.' + s)


def mask(desc):
    m = desc & 0xf
    return '.' + ''.join(c for i, c in enumerate('xyzw') if m >> (3 - i) & 1)


def disasm(w, sw):
    op = w >> 26
    if op >= 0x30:  # mad / madi
        imm = op < 0x38
        d = sw[w & 0x1f] if (w & 0x1f) < len(sw) else 0
        if imm:
            s3, s2, s1 = (w >> 5) & 0x7f, (w >> 12) & 0x1f, (w >> 17) & 0x1f
        else:
            s3, s2, s1 = (w >> 5) & 0x1f, (w >> 10) & 0x7f, (w >> 17) & 0x1f
        idx = (w >> 22) & 3
        return (f'mad{"i" if imm else ""} {dst((w >> 24) & 0x1f)}{mask(d)}, {src(s1)}{swz(d, 0)}, '
                f'{src(s2, 0 if imm else idx)}{swz(d, 1)}, {src(s3, idx if imm else 0)}{swz(d, 2)}')
    if op in ARITH or op in ARITHI:
        d = sw[w & 0x7f] if (w & 0x7f) < len(sw) else 0
        idx = (w >> 19) & 3
        if op in ARITHI:
            s1, s2 = (w >> 14) & 0x1f, (w >> 7) & 0x7f
            a, b = src(s1), src(s2, idx)
            name = ARITHI[op]
        else:
            s1, s2 = (w >> 12) & 0x7f, (w >> 7) & 0x1f
            a, b = src(s1, idx), src(s2)
            name = ARITH[op]
        one = name in ('mov', 'mova', 'ex2', 'lg2', 'flr', 'rcp', 'rsq', 'litp')
        return f'{name} {dst((w >> 21) & 0x1f)}{mask(d)}, {a}{swz(d, 0)}' + ('' if one else f', {b}{swz(d, 1)}')
    if op in (0x2e, 0x2f):
        d = sw[w & 0x7f] if (w & 0x7f) < len(sw) else 0
        s1, s2, idx = (w >> 12) & 0x7f, (w >> 7) & 0x1f, (w >> 19) & 3
        return f'cmp {src(s1, idx)}{swz(d, 0)}, {CMPOPS[(w >> 24) & 7]} {CMPOPS[(w >> 21) & 7]}, {src(s2)}{swz(d, 1)}'
    name = FLOW.get(op, f'op{op:02x}')
    dest, num = (w >> 10) & 0xfff, w & 0xff
    if name in ('ifu', 'callu', 'jmpu'):
        return f'{name} b{(w >> 22) & 0xf}, to {dest:03x} num {num}'
    if name in ('ifc', 'callc', 'jmpc', 'breakc'):
        cond = ['or', 'and', 'x', 'y'][(w >> 22) & 3]
        return f'{name} {"!" if not w >> 25 & 1 else ""}cc.x {cond} {"!" if not w >> 24 & 1 else ""}cc.y, to {dest:03x} num {num}'
    if name == 'loop':
        return f'loop i{(w >> 22) & 3}, to {dest:03x}'
    if name == 'call':
        return f'call {dest:03x} num {num}'
    if name == 'setemit':
        return f'setemit vtx {(w >> 24) & 3}{" prim" if w >> 23 & 1 else ""}{" inv" if w >> 22 & 1 else ""}'
    return name


def main():
    code, sw = {}, {}
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        m = re.search(r'\[(gscode|gsswz)\] ([0-9a-f]+)((?: [0-9a-f]{8})+)', line)
        if m:
            base = int(m.group(2), 16)
            for i, x in enumerate(m.group(3).split()):
                (code if m.group(1) == 'gscode' else sw)[base + i] = int(x, 16)
    sw = [sw.get(i, 0) for i in range(max(sw) + 1 if sw else 0)]
    pc = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    while pc in code:
        w = code[pc]
        print(f'{pc:03x}: {w:08x}  {disasm(w, sw)}')
        if w >> 26 == 0x22:
            break
        pc += 1


if __name__ == '__main__':
    main()
