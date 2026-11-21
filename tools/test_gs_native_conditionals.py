#!/usr/bin/env python3
"""Differential checks for native PICA conditionals using synthetic programs only."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

import pica_to_c as pica

ROOT = Path(__file__).resolve().parents[1]
IDENTITY = 15 | (0x1b << 5) | (0x1b << 14) | (0x1b << 23)
END = 0x22 << 26
NOP = 0x21 << 26

def mov(dst, src):
    return (0x13 << 26) | (dst << 21) | (src << 12)

def compare():
    return (0x2e << 26) | (2 << 24) | (4 << 21) | (1 << 7)

def ifc(dest, fop=2, x=1, y=1, count=0):
    return (0x28 << 26) | (x << 25) | (y << 24) | (fop << 22) | (dest << 10) | count

def ifu(dest, bid=0):
    return (0x27 << 26) | (bid << 22) | (dest << 10)

def finish():
    code = []
    for vertex in range(3):
        code += [mov(0, 0x10), (0x2b << 26) | (vertex << 24) | ((vertex == 2) << 23), 0x2a << 26]
    return code + [END]

programs = []
for fop in range(4):
    for x in range(2):
        for y in range(2):
            programs.append([mov(0x10, 0), compare(), ifc(4, fop, x, y), mov(0x10, 0x20)] + finish())
for bid in (0, 14):
    programs.append([mov(0x10, 0), ifu(3, bid), mov(0x10, 0x20)] + finish())
programs += [
    [mov(0x10, 0), compare(), ifu(5), ifc(5), mov(0x10, 0x20)] + finish(),
    [mov(0x10, 0), compare(), ifc(3)] + finish(),  # empty then-region
    [mov(0x10, 0), (0x29 << 26) | (3 << 10), mov(0x10, 0x20), NOP] + finish(),
]
for code in programs:
    _, end, _ = pica.translate(0, code, [IDENTITY], 'check')
    assert end == len(code)
    assert not pica.independence(0, end, code, [IDENTITY])[0]

for code in (
    [ifc(2, count=1), NOP, NOP, END],
    [ifu(3), END, NOP, END],
    [ifu(3), ifu(4), NOP, NOP, END],
    [(0x29 << 26) | (4 << 10), ifu(3), NOP, NOP, NOP, END],
    [ifu(4), (0x29 << 26) | (3 << 10), NOP, NOP, END],
):
    try:
        pica.translate(0, code, [IDENTITY], 'declined')
    except pica.Unsupported:
        pass
    else:
        raise AssertionError('Unsupported conditional control flow was accepted')

with tempfile.TemporaryDirectory(prefix='gs-conditional-') as temp:
    path = Path(temp) / 'synthetic.gsl'
    path.write_bytes(b''.join(b'GS  ' + struct.pack('<III', 0, len(code), 1)
                             + struct.pack('<' + 'I' * len(code), *code) + struct.pack('<I', IDENTITY)
                             for code in programs))
    result = subprocess.run([sys.executable, str(ROOT/'tools/test_gs_native.py'), temp],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end='')
    result.check_returncode()
    match = re.search(r'(\d+) runs compared \((\d+) primitives\), (\d+) differ', result.stdout)
    assert match and int(match[1]) >= len(programs)*2000 and int(match[2]) > 0 and int(match[3]) == 0
print('Native conditionals: all reference combinations, uniform flags, nesting, empty regions, loop regression and rejection checks passed')
