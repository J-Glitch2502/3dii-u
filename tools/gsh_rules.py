#!/usr/bin/env python3
"""What a real Wii U GPU holds a compiled shader program to and an emulator doesn't, checked on every program of a .gsh file:
    gsh_rules.py <shaders.gsh>...        exit code 1 if any program breaks a rule
    gsh_rules.py --self-check

Cemu turns a program into GLSL and runs that, so it draws right with programs a console scrambles: scrambled geometry on testers' consoles
(2026-11-12) came from the shaders compiled there, by a later CafeGLSL than the one that makes a build's pack, and Cemu showed nothing. The
rules, from the R600 family's instruction set as Mesa's assembler keeps to it:
  - a program names only the registers it says it uses (its count is the low byte of sq_pgm_resources), the four of a clause aside
  - a clause's own registers (124 to 127) are read only after the same clause wrote them
  - PV and PS name a result the group before produced
  - a constant read lies inside the window its clause locked
  - a group has at most five instructions, one of them at most in the scalar unit, where the scalar-only operations go
  - a fetch from a buffer carries its own data format (USE_CONST_FIELDS has the GPU take the buffer's, and GX2 gives a uniform block's none)
tools/win/build3ds.py runs this on every pack it makes; port/wiiu/latte_program.h puts right the two the console's compiler breaks."""
import collections
import struct
import sys

VS_HEAD, VS_PROG, PS_HEAD, PS_PROG = 3, 5, 6, 7
# operations of two operands that read only the first (their second field is zero, and means register 0)
ONE_OPERAND = {16, 17, 18, 19, 20, 21, 22, 24, 25, 26, 51} | set(range(97, 112)) | {119, 120, 121}
SCALAR_ONLY = set(range(97, 112)) | set(range(115, 122))  # exp, log, reciprocals, roots, conversions, sin, cos, integer multiplies
VECTOR_ONLY = {80, 81, 82, 83, 21, 22, 24}  # the dot products, cube, max4 and the address moves
LITERAL, PV, PS = 253, 254, 255


def blocks(data):
    if data[:4] != b'Gfx2':
        raise ValueError('not a GFD file')
    p = struct.unpack('>I', data[4:8])[0]
    while p + 32 <= len(data) and data[p:p + 4] == b'BLK{':
        head, _, _, kind, size = struct.unpack('>5I', data[p + 4:p + 24])
        yield kind, data[p + head:p + head + size]
        p += head + size


def programs(data):
    """(kind, registers, words) of every program: a header block's first word, low byte, then the program block that follows it"""
    registers = 0
    for kind, body in blocks(data):
        if kind in (VS_HEAD, PS_HEAD):
            registers = struct.unpack('>I', body[:4])[0] & 255
        elif kind in (VS_PROG, PS_PROG):
            yield 'vertex' if kind == VS_PROG else 'pixel', registers, struct.unpack(f'<{len(body) // 4}I', body[:len(body) // 4 * 4])


def check(words, registers):
    """the rules one program breaks, each with how often"""
    found = collections.Counter()

    def register(r, what):
        if r < 124 and r >= registers:
            found[f'{what} register {r}, and the program says it uses {registers}'] += 1

    pc = 0
    while 2 * pc + 1 < len(words):
        w0, w1 = words[2 * pc], words[2 * pc + 1]
        pc += 1
        if w1 & 1 << 29:  # arithmetic: instructions of two words, a group's literals after its last one
            i, end = w0 & 0x3FFFFF, (w0 & 0x3FFFFF) + (w1 >> 18 & 0x7F) + 1
            locked = (w0 >> 30 & 3, w1 & 3)  # per constant window: 16 entries, 32, or (3) the loop's
            written, before, group = set(), None, []
            while i < end:
                a0, a1 = words[2 * i], words[2 * i + 1]
                i += 1
                three = bool(a1 >> 15 & 7)
                op = None if three else a1 >> 7 & 0x7FF
                sources = [(a0 & 0x1FF, a0 >> 10 & 3), (a0 >> 13 & 0x1FF, a0 >> 23 & 3)]
                if three:
                    sources.append((a1 & 0x1FF, a1 >> 10 & 3))
                elif op in ONE_OPERAND:
                    del sources[1]
                group.append((op, sources, a1 >> 21 & 0x7F, a1 >> 29 & 3, three or bool(a1 & 0x10)))
                if not a0 >> 31:
                    continue
                vector, scalar = set(), 0
                for op, sources, dst, chan, stores in group:
                    for sel, c in sources:
                        register(sel, 'reads')
                        if 124 <= sel < 128 and (sel, c) not in written:
                            found['reads a clause register the clause has not written'] += 1
                        if sel == PV and (before is None or c not in before[0]):
                            found['PV names a channel the group before did not produce'] += 1
                        if sel == PS and (before is None or not before[1]):
                            found['PS with no scalar result in the group before'] += 1
                        for window, base in enumerate((128, 160)):
                            if base <= sel < base + 32 and locked[window] != 3 and sel - base >= 16 * locked[window]:
                                found['reads a constant outside the window its clause locked'] += 1
                    if op in SCALAR_ONLY or (chan in vector and op not in VECTOR_ONLY):
                        scalar += 1
                    elif chan in vector:
                        found['two vector-only instructions on one channel'] += 1
                    else:
                        vector.add(chan)
                    if stores:
                        register(dst, 'writes')
                        if dst >= 124:
                            written.add((dst, chan))
                if scalar > 1:
                    found['more than one instruction for the scalar unit in a group'] += 1
                if len(group) > 5:
                    found['a group of more than five instructions'] += 1
                before = (vector, scalar > 0)
                i += (max([c + 1 for g in group for sel, c in g[1] if sel == LITERAL] or [0]) + 1) // 2
                group = []
            continue
        kind = w1 >> 23 & 0x7F
        if kind in (39, 40):  # exports: a register, and with a burst the ones after it; a swizzle of constants alone reads none
            if any((w1 >> 3 * k & 7) < 4 for k in range(4)):
                register((w0 >> 15 & 0x7F) + (w1 >> 17 & 15), 'exports')
        elif kind in (1, 2, 3):  # fetches, four words each
            for k in range((w1 >> 10 & 7 | (w1 >> 19 & 1) << 3) + 1):
                f0, f1 = words[2 * (w0 + 2 * k)], words[2 * (w0 + 2 * k) + 1]
                if f0 & 31 == 0:  # from a buffer (the others are texture samples, laid out differently)
                    register(f0 >> 16 & 0x7F, 'fetches by')
                    register(f1 & 0x7F, 'fetches into')
                    if f1 >> 21 & 1:
                        found['a fetch takes its data format from the buffer (USE_CONST_FIELDS)'] += 1
        if w1 & 1 << 21:  # END_OF_PROGRAM
            break
    return found


def check_file(path):
    """(programs, lines saying what broke which rule)"""
    with open(path, 'rb') as f:
        data = f.read()
    n, lines = 0, []
    for index, (kind, registers, words) in enumerate(programs(data)):
        n += 1
        for what, times in sorted(check(words, registers).items()):
            lines.append(f'{kind} program {index}: {what}' + (f' ({times} times)' if times > 1 else ''))
    return n, lines


def self_check():
    def op2(op, dst, chan, a, b=(0, 0), last=True):
        return [a[0] | a[1] << 10 | b[0] << 13 | b[1] << 23 | last << 31, 1 << 4 | op << 7 | dst << 21 | chan << 29]

    ADD, MOV, RECIP, MULADD, X, Y, W = 0, 25, 102, 16, 0, 1, 3
    # A program as the PC's compiler makes them, of three registers. Its list: a clause of five at 3 with constants 0 to 15 locked, a fetch at 8,
    # and the export of a constant, which ends it.
    good = [3 | 1 << 30, 8 << 26 | 4 << 18 | 1 << 31, 8, 1 << 23 | 1 << 31, 1 << 13, 0xB24 | 1 << 21 | 40 << 23 | 1 << 31]
    good += [1 | (128 + 15) << 13 | 1 << 31, 2 | MULADD << 13 | 124 << 21]  # 3: T0.x = R1.x * K[15].x + R2.x
    good += op2(MOV, 2, Y, (124, X), last=False)                             # 4: R2.y = T0.x
    good += op2(RECIP, 2, X, (PV, X))                                        # 5: R2.x = 1 / PV.x, in the scalar unit
    good += op2(ADD, 0, X, (PS, X), (LITERAL, X))                            # 6: R0.x = PS + a literal
    good += [0x3F800000, 0]                                                  # 7: the literal
    good += [2 << 5 | 0x80 << 8 | 15 << 26, 2 | 35 << 22, 1 << 19, 0]        # 8, 9: four floats of the uniform block into R2, by R0.x

    def broken(word, clear, put):
        w = list(good)
        w[word] = w[word] & ~clear | put
        return ' / '.join(check(w, 3))

    assert not check(good, 3), check(good, 3)
    assert 'reads register 2' in ' / '.join(check(good, 2))
    assert 'writes register 123' in broken(7, 0x7F << 21, 123 << 21)  # the console compiler's throwaway register
    assert 'USE_CONST_FIELDS' in broken(17, 0, 1 << 21)
    assert 'outside the window' in broken(0, 3 << 30, 0)
    assert 'has not written' in broken(8, 0x1FF, 125)
    assert 'PV names a channel' in broken(10, 3 << 10, W << 10)
    assert 'scalar unit' in broken(9, 0x7FF << 7, RECIP << 7)  # a second scalar-only operation in the reciprocal's group
    print('gsh rules: ok')


def main():
    if '--self-check' in sys.argv:
        return self_check()
    failed = False
    for path in sys.argv[1:]:
        n, lines = check_file(path)
        print(f'{path}: {n} programs, ' + (f'{len(lines)} findings' if lines else 'all as a GPU takes them'))
        for line in lines[:40]:
            print('   ' + line)
        failed |= bool(lines)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
