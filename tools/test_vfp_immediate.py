#!/usr/bin/env python3
"""Validate every VFP modified immediate against its IEEE encoding.

Retail ARM11 code uses VFPv2; imported emulator-targeted mods can use the
VFPv3 immediate instruction. No host floating-point arithmetic is required
at runtime for these exactly representable constants.
"""
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/3dsrecomp'))
from tools.recomp import arm
from tools.recomp.ins import OP, UDF


def main():
    count = 0
    for double in (False, True):
        for destination in ((0, 7, 15) if double else (0, 1, 30, 31)):
            for immediate in range(256):
                vd = destination if double else destination >> 1
                db = 0 if double else destination & 1
                word = (0xEEB00A00 | int(double) << 8 | db << 22 | vd << 12
                        | (immediate >> 4) << 16 | (immediate & 15))
                ins = arm.decode(0x100000, word, arm.Ctx(lambda _: None))
                assert ins.kind == OP, ins
                value = float(re.search(r'(-?\d+\.\d+)f?', ins.code[0]).group(1))
                # ARM VFPExpandImm: sign, inverted exponent bit, replicated
                # exponent bit, then the six remaining immediate bits.
                fraction = 52 if double else 23
                expbits = 11 if double else 8
                sign = immediate >> 7
                b = (immediate >> 6) & 1
                expected = (sign << (fraction + expbits)
                            | (1-b) << (fraction + expbits-1)
                            | ((1 << (expbits-3))-1 if b else 0) << (fraction+2)
                            | (immediate & 63) << (fraction-4))
                packed = struct.pack('<d' if double else '<f', value)
                assert int.from_bytes(packed, 'little') == expected, (hex(word), value, hex(expected))
                assert ins.vregs() == ({2*destination, 2*destination+1} if double else {destination})
                count += 1
    for reserved in (0x10, 0x20, 0x80, 0xA0):
        assert arm.decode(0x100000, 0xEEB00A00 | reserved, arm.Ctx(lambda _: None)).kind == UDF
    print(f'PASS: {count} immediate/register combinations and reserved encodings')


if __name__ == '__main__':
    main()
