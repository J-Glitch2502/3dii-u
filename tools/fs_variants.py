#!/usr/bin/env python3
"""Fragment states no run has reached yet, made from the parts of the lit states runs did reach, so a lit material doesn't fall back to the uber shader.

A game's lit materials come in some combiner setups, each seen with only some of the lighting setups (which lookup tables are on), texture wrap modes and
alpha tests. Emmy's clothes in the Laytons were one of those combinations: lit like Layton's, with the lookup table off, and no run had logged it. This
writes combinations of the parts seen on lit states as one log the shader generator reads. Blending and masks are left as logged, since the shaders
don't depend on them. The combinations multiply: a game with few lit setups gets them all, and one whose product passes --max (millions, in the bigger
games) gets the --max likeliest: a combination ranks by how many logged states each of its parts was seen in, so the setups a game uses everywhere
come before the ones it used once. Getting none, as such a game used to, left every unlogged material to the fallback: Super Mario 3D Land's castles.
fs_variants.py <output.bin> [--max N] <fragment log>...
fs_variants.py --self-check"""
import heapq
import struct
import sys
from collections import Counter

MOST_RANKED = 4_000_000  # combinations worth walking through to rank; past it the rarest parts are left out first


def keys(path):
    d = open(path, 'rb').read()
    p = 0
    while p + 4 <= len(d):
        if d[p:p + 4] == b'FS  ':
            p += 4
        elif d[p:p + 4] == b'VS  ':
            _, nc, ns = struct.unpack_from('<III', d, p + 4)
            p += 16 + (nc + ns) * 4 + 56
            continue
        n = struct.unpack_from('<I', d, p)[0]
        yield struct.unpack_from(f'<{n // 4}I', d, p + 4)
        p += 4 + n


def split(w):
    """(head, textures, combiners, lighting, fog and alpha, blend and mask), or None for a state that isn't shaded or lit."""
    if not w[0] & 1:
        return None
    i = 5 + min(w[4], 6) * 5
    if not w[i]:
        return None
    n = 1 + 1 + 7 + (w[i + 1] & 15)
    light = w[i:i + n]
    return w[0], w[1:4], w[4:i], light, w[i + n], w[i + n + 1:]


def variants(parts, limit=None):
    """(the states to compile, likeliest first; how many combinations there are; a line about the parts)"""
    if not parts:
        return [], 0, 'no lit states'
    head = lambda p: p[0] & 7 | 16 << 3 | 1 << 9  # only shaded, w-buffered and scissored matter to a shader
    heads = Counter(head(p) for p in parts)
    combiners = Counter(p[2] for p in parts)
    lights = Counter(p[3] for p in parts)
    alphas = Counter(p[4] for p in parts)
    # a texture setup goes with the combiners seen with it, or it would ask for a unit they never read
    textures = Counter((p[2], p[1]) for p in parts)
    pairs = [(c, t, n) for (c, t), n in textures.items()]
    summary = f'{len(parts)} lit states: {len(combiners)} combiner setups, {len(lights)} lighting setups, {len(alphas)} fog and alpha settings, {len(heads)} heads'
    total = len(pairs) * len(lights) * len(alphas) * len(heads)
    lists = [sorted(pairs, key=lambda x: (-x[2], x)), sorted(lights.items(), key=lambda x: (-x[1], x)),
             sorted(alphas.items(), key=lambda x: (-x[1], x)), sorted(heads.items(), key=lambda x: (-x[1], x))]
    while len(lists[0]) * len(lists[1]) * len(lists[2]) * len(lists[3]) > MOST_RANKED:
        max(lists, key=len).pop()  # the rarest of the longest list
    tails = parts[0][5]

    def ranked():
        for c, t, n_ct in lists[0]:
            for li, n_l in lists[1]:
                for a, n_a in lists[2]:
                    for h, n_h in lists[3]:
                        yield n_ct * combiners[c] * n_l * n_a * n_h, (h, *t, *c, *li, a, *tails)

    every = ranked()
    chosen = [w for _, w in (every if limit is None or total <= limit else heapq.nlargest(limit, every))]
    return chosen, total, summary


def self_check():
    def state(combiner, light, alpha):
        return (1 | 16 << 3 | 1 << 9, 0, 0, 0, 1, combiner, 0, 0, 0, 0, 1, 0, light, 0, 0, 0, 0, 0, 0, alpha, 9, 9)

    logged = [state(1, 1, 0)] * 5 + [state(2, 2, 7)]  # combiner 1 with light 1 everywhere, the others once
    parts = [split(w) for w in logged]
    assert all(parts) and split((0,) * 22) is None
    everything, total, _ = variants(parts)
    assert total == len(everything) == 8 and len(set(everything)) == 8
    top, total, _ = variants(parts, 3)
    assert total == 8 and len(top) == 3 and top[0] == state(1, 1, 0), top[0]
    assert set(top) <= set(everything) and state(2, 2, 7) not in top
    assert variants([]) == ([], 0, 'no lit states')
    print('fs_variants: ok')


def main():
    args = sys.argv[1:]
    if args == ['--self-check']:
        return self_check()
    out_path = args.pop(0)
    limit = None
    if args[:1] == ['--max']:
        limit = int(args[1])
        args = args[2:]
    chosen, total, summary = variants([p for path in args for p in map(split, keys(path)) if p], limit)
    out = bytearray()
    for w in chosen:
        out += struct.pack('<I', len(w) * 4) + struct.pack(f'<{len(w)}I', *w)
    print(f'{summary}: {total} combinations, {"all" if len(chosen) == total else f"the {len(chosen)} likeliest"} written' if chosen else f'{summary}: none written')
    open(out_path, 'wb').write(out)


if __name__ == '__main__':
    main()
