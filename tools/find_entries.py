#!/usr/bin/env python3
"""Finds code entry points static analysis in the generator misses, in hand-written ARM such as video decoders. The console can't
compile missing code on the spot, so whatever is missed there is a crash. Two forms are recognised:
  add lr, pc, #n ... a call through a register: the return point is a jump target.
  adr rB, table ... ldr rX, [rB, rI] ... add pc, rX, rB: a table of offsets from its own start.
find_entries.py <game dir> [--check] [--write]
--check compares with functions.txt, which holds what runs really reached. --write adds what was found to functions.txt."""
import os
import struct
import sys

CODE_BASE = 0x100000


def rotated_imm(word):
    imm, rot = word & 0xFF, ((word >> 8) & 0xF) * 2
    return ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm


def plausible_code(d, text_end, t):
    """an aligned address in the text, holding something ARM could run"""
    if t & 3 or not CODE_BASE <= t < text_end:
        return False
    w = struct.unpack_from('<I', d, t - CODE_BASE)[0]
    return w >> 28 != 0xF and w != 0


def text_size(game):
    with open(os.path.join(game, 'exheader.bin'), 'rb') as f:
        h = f.read(0x40)
    return struct.unpack_from('<I', h, 0x18)[0]  # the text section's size in bytes (SCI: name, flags, then text info)


def find(game):
    d = open(os.path.join(game, 'code.bin'), 'rb').read()
    size = text_size(game)
    if not 0x1000 <= size <= len(d):
        sys.exit(f'odd text size {size:#x}')
    text_end = CODE_BASE + size
    found = {}
    n = size // 4
    words = struct.unpack_from(f'<{n}I', d, 0)
    for i, w in enumerate(words):
        a = CODE_BASE + i * 4
        if w & 0xFFFF4000 == 0xE92D4000 or w == 0xE52DE004:  # push {..., lr}: functions reached only through pointers
            found.setdefault(a, 'saves lr')
            continue
        if w & 0xFFFF0000 == 0xE92D0000 and i and words[i - 1] & 0xFFFF8000 == 0xE8BD8000:  # any push right after a pop {..., pc}
            found.setdefault(a, 'after a return')
            continue
        if w >> 28 != 0xF and (w & 0x0FFFF000 in (0x028FE000, 0x024FE000) or w & 0x0FFFFFFF == 0x01A0E00F):  # add, sub or mov to lr from pc, with any condition
            t = a + 8 + (0 if w & 0x0FFFFFFF == 0x01A0E00F else rotated_imm(w) if w & 0x0FFFF000 == 0x028FE000 else -rotated_imm(w))
            if plausible_code(d, text_end, t):
                found[t] = 'return point'
            continue
        if w & 0xFEFF0000 != 0xE24F0000 & 0xFEFF0000 or w >> 28 != 0xE:
            continue
        op = (w >> 21) & 0xF
        if op not in (2, 4):  # sub or add, Rn = pc
            continue
        rb = (w >> 12) & 0xF
        imm = rotated_imm(w)
        table = a + 8 - imm if op == 2 else a + 8 + imm
        # a jump through it within a few instructions: add pc, rX, rB (either way round), with rB not written meanwhile
        hit = False
        for j in range(i + 1, min(i + 16, n)):
            x = words[j]
            if x & 0xFFF0FFF0 == 0xE080F000 and rb in ((x >> 16) & 0xF, x & 0xF):
                hit = True
                break
            if (x >> 26) & 3 == 0 and (x >> 12) & 0xF == rb and x >> 28 == 0xE:
                break
        if not hit or not CODE_BASE <= table < text_end or table & 3:
            continue
        p = table
        count = 0
        while p + 4 <= text_end and count < 4096:
            off = struct.unpack_from('<I', d, p - CODE_BASE)[0]
            t = (table + off) & 0xFFFFFFFF
            if off == 0 or not plausible_code(d, text_end, t) or (off > 0x100000 and off < 0xFFF00000):
                break
            found[t] = 'table at %08x' % table
            p += 4
            count += 1
    # hand-written code is where the tables are, and there code after any unconditional jump can be a target, as can a function past its push
    targets = [t for t, why in found.items() if why.startswith('table')]
    if targets:
        low, high = (min(targets) - 0x1000 - CODE_BASE) // 4, (max(targets) + 0x1000 - CODE_BASE) // 4
        for i in range(max(low, 1), min(high, n)):
            w, before = words[i], words[i - 1]
            jumped = before & 0xFFFF8000 == 0xE8BD8000 or before >> 24 == 0xEA or before == 0xE12FFF1E or before == 0xE49DF004 or before & 0xFFF0F000 == 0xE080F000
            if w >> 28 == 0xE and (jumped or before & 0xFFFF4000 == 0xE92D4000 or before == 0xE52DE004):
                found.setdefault(CODE_BASE + i * 4, 'in hand-written code')
    return found


def main():
    game = sys.argv[1]
    found = find(game)
    path = os.path.join(game, 'functions.txt')
    known = set()
    if os.path.exists(path):
        known = {int(x, 16) for x in open(path).read().split()}
    print(f'{len(found)} entries found, {len(known)} known from runs')
    if '--check' in sys.argv:
        got = known & set(found)
        print(f'{len(got)} of the known ones were predicted; not predicted:')
        for a in sorted(known - set(found))[:60]:
            print(f'  {a:08x}')
        tables = {v for v in found.values() if v != 'return point'}
        print(f'{len(tables)} tables, {sum(1 for v in found.values() if v == "return point")} return points')
    if '--write' in sys.argv:
        all_ = sorted(known | set(found))
        with open(path, 'w', newline='\n') as f:
            f.writelines(f'0x{a:08x}\n' for a in all_)
        print(f'functions.txt: {len(all_)} entries')


main()
