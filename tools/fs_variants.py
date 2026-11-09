#!/usr/bin/env python3
"""Fragment states no run has reached yet, made from the parts of the lit states runs did reach, so a lit material doesn't fall back to the uber shader.

A game's lit materials come in some combiner setups, each seen with only some of the lighting setups (which lookup tables are on), texture wrap modes and
alpha tests. Emmy's clothes in the Laytons were one of those combinations: lit like Layton's, with the lookup table off, and no run had logged it. This
writes every combination of the parts seen on lit states as one log the shader generator reads. Blending and masks are left as logged, since the shaders
don't depend on them. The combinations multiply: a game with few lit setups gets them all, and one whose product passes --max gets none (millions, in the
bigger games), as compiling them all would cost more than the fallbacks they'd save.
fs_variants.py <output.bin> [--max N] <fragment log>..."""
import struct
import sys


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


def main():
    args = sys.argv[1:]
    out_path = args.pop(0)
    limit = None
    if args[:1] == ['--max']:
        limit = int(args[1])
        args = args[2:]
    parts = [p for path in args for p in map(split, keys(path)) if p]
    heads = {p[0] & 7 | 16 << 3 | 1 << 9 for p in parts}  # only shaded, w-buffered and scissored matter to a shader
    combiners = {p[2] for p in parts}
    lights = {p[3] for p in parts}
    alphas = {p[4] for p in parts}
    # a texture setup goes with the combiners seen with it, or it would ask for a unit they never read
    textures = {}
    for p in parts:
        textures.setdefault(p[2], set()).add(p[1])
    total = sum(len(textures[c]) for c in combiners) * len(lights) * len(alphas) * len(heads)
    summary = f'{len(parts)} lit states: {len(combiners)} combiner setups, {len(lights)} lighting setups, {len(alphas)} fog and alpha settings, {len(heads)} heads'
    out = bytearray()
    if parts and (limit is None or total <= limit):
        tails = parts[0][5]
        for c in sorted(combiners):
            for t in sorted(textures[c]):
                for li in sorted(lights):
                    for a in sorted(alphas):
                        for h in sorted(heads):
                            w = (h, *t, *c, *li, a, *tails)
                            out += struct.pack('<I', len(w) * 4) + struct.pack(f'<{len(w)}I', *w)
        print(f'{summary}: {total} states')
    else:
        print(f'{summary}: {total} combinations, over the {limit} that are worth compiling ahead: none written' if parts else 'no lit states: none written')
    open(out_path, 'wb').write(out)


if __name__ == '__main__':
    main()
