#!/usr/bin/env python3
"""Finds a retail game's code without symbols, by following control flow from its entry point and from code pointers in its data,
then reports how much of it my lifter can already translate. discover.py <game dir>. Prints counts and addresses only."""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'phase0', 'tools'))
from lift import Fn, Unsupported  # noqa: E402

UDF = 0xE7F000F0


class GameImage:
    def __init__(self, game_dir):
        info = json.load(open(os.path.join(game_dir, 'info.json')))
        self.code = open(os.path.join(game_dir, 'code.bin'), 'rb').read()
        self.base = info['text']['addr']
        self.text_start, self.text_end = self.base, self.base + info['text']['size']
        self.rodata = info['rodata']
        self.data = info['data']
        self.seen, self.literals = set(), set()

    def word(self, a):
        return int.from_bytes(self.code[a - self.base:a - self.base + 4], 'little')

    def in_text(self, a):
        return self.text_start <= a < self.text_end

    def is_code(self, a):
        return a in self.seen

    def is_literal(self, a):
        return a in self.literals


def target_of(addr, w):
    imm = w & 0xFFFFFF
    return addr + 8 + ((imm - (1 << 24) if imm & (1 << 23) else imm) << 2)


def walk(img, roots):
    """Recursive descent over ARM code. Returns function starts and the Thumb entries it meets."""
    funcs, thumb = set(roots), set()
    todo = list(roots)
    while todo:
        a = todo.pop()
        while img.in_text(a) and a not in img.seen and a not in img.literals:
            w = img.word(a)
            img.seen.add(a)
            cond, top = w >> 28, (w >> 25) & 7
            if w == UDF:
                break
            if cond == 15:
                if top == 5:  # blx immediate: into Thumb
                    t = addr_thumb = target_of(a, w) + ((w >> 24) & 1) * 2
                    thumb.add(t)
                a += 4
                continue
            if top == 5:
                t = target_of(a, w)
                if w & (1 << 24):
                    if t not in funcs:
                        funcs.add(t)
                        todo.append(t)
                else:
                    todo.append(t)
                    if cond == 14:
                        break
                a += 4
                continue
            if (w & 0x0FFFFFF0) == 0x012FFF10:  # bx
                if cond == 14:
                    break
                a += 4
                continue
            if top == 4 and w & (1 << 20) and w & (1 << 15):  # ldm with pc
                if cond == 14:
                    break
                a += 4
                continue
            if top in (2, 3) and w & (1 << 20) and not (top == 3 and w & 16):  # ldr
                rn, rd = (w >> 16) & 15, (w >> 12) & 15
                if rn == 15 and top == 2 and (w >> 24) & 1:
                    off = w & 0xFFF
                    lit = a + 8 + (off if (w >> 23) & 1 else -off)
                    img.literals.add(lit)
                    value = img.word(lit) if img.in_text(lit) else 0
                    if img.in_text(value & ~1) and value & 3 in (0, 1):
                        if value & 1:
                            thumb.add(value & ~1)
                        elif value not in funcs:
                            funcs.add(value)
                            todo.append(value)
                if rd == 15:
                    if rn == 15 and top == 3:  # ldr pc, [pc, rX, lsl #2]: a table of code addresses follows
                        t = a + 8
                        while img.in_text(img.word(t)) and img.word(t) % 4 == 0:
                            img.literals.add(t)
                            todo.append(img.word(t))
                            t += 4
                    if cond == 14:
                        break
                a += 4
                continue
            if top == 6 and ((w >> 8) & 14) == 10 and (w >> 24) & 1 and not (w >> 21) & 1:  # vldr/vstr
                if (w >> 16) & 15 == 15:
                    off = (w & 255) * 4
                    img.literals.add(a + 8 + (off if (w >> 23) & 1 else -off))
                a += 4
                continue
            if top in (0, 1) and (w >> 12) & 15 == 15 and not 8 <= (w >> 21) & 15 <= 11:  # data processing into pc
                if (w & 0x0FE0F000) == 0x0080F000 and (w >> 16) & 15 == 15:  # add pc, pc, rX: a branch table follows
                    t = a + 8
                    while (img.word(t) >> 25) & 7 == 5 and not img.word(t) & (1 << 24):
                        todo.append(t)
                        t += 4
                if cond == 14:
                    break
            a += 4
    return funcs, thumb


def main():
    img = GameImage(sys.argv[1])
    funcs, thumb = walk(img, [img.text_start])
    from_flow = len(img.seen)
    # Code pointers in rodata and data reach what direct control flow can't: vtables and callback tables.
    pointer_words = 0
    for seg in (img.rodata, img.data):
        for at in range(seg['addr'], seg['addr'] + seg['size'] - 3, 4):
            v = img.word(at)
            if img.in_text(v & ~1):
                if v & 3 == 0:
                    pointer_words += 1
                    more, t = walk(img, [v])
                    funcs |= more | {v}
                    thumb |= t
                elif v & 3 == 1:
                    thumb.add(v & ~1)
    total_words = (img.text_end - img.text_start) // 4
    print(f'text {total_words} words; reached by control flow {from_flow}, adding data pointers {len(img.seen)} '
          f'({len(img.seen) / total_words:.1%}); literal words {len(img.literals)} ({len(img.literals) / total_words:.1%})')
    print(f'functions {len(funcs)}, pointers followed {pointer_words}, Thumb entry points seen {len(thumb)}')
    gaps = total_words - len(img.seen) - len(img.literals)
    print(f'unreached and not literal: {gaps} words ({gaps / total_words:.1%})')

    any_set = type('AnySet', (), {'__contains__': lambda self, x: True})()
    fn = Fn(img, img.text_start, img.text_end, any_set, True, 'f')
    fn.insts = sorted(img.seen)
    ok = 0
    bad = collections.Counter()
    sample = {}
    for a in sorted(img.seen):
        w = img.word(a)
        try:
            if w >> 28 == 15:
                raise Unsupported(f'unconditional space {w >> 20 & 0xFF:02x}')
            if w == UDF:
                ok += 1
                continue
            fn.translate(a, w)
            ok += 1
        except Unsupported as e:
            reason = str(e)
            if 'not a known' in reason:
                ok += 1
                continue
            key = reason if 'instruction' not in reason or 'media' in reason else reason
            if reason in ('media instruction', 'miscellaneous instruction', 'coprocessor transfer', 'VFP operation') or 'coprocessor' in reason or 'double' in reason:
                key = f'{reason} [{(w >> 20) & 0xFF:02x}:{(w >> 4) & 0xF:x}:{(w >> 8) & 0xF:x}]'
            bad[key] += 1
            sample.setdefault(key, f'{a:#x}={w:08x}')
        except Exception as e:
            key = f'lifter error {type(e).__name__}'
            bad[key] += 1
            sample.setdefault(key, f'{a:#x}={w:08x}')
    print(f'translatable now: {ok} of {len(img.seen)} ({ok / len(img.seen):.2%})')
    for key, n in bad.most_common(40):
        print(f'{n:>8} {key}   e.g. {sample[key]}')


main()
