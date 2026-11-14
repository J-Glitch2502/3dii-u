#!/usr/bin/env python3
"""Geometry shaders a game runs on the CPU, as C compiled into its build: pica_to_c.py <out.c> <gs logs (.gsl)...>

A geometry shader has no GPU path on the Wii U: the runtime runs it in its PICA interpreter on the GPU thread, a couple of hundred cycles an
instruction. Luigi's Mansion: Dark Moon's rain (90 instructions for each of 1600 vertices a frame) took 28 ms of every frame that way on a
console (2026-11-13). A program logged in a .gsl file ("GS  " records: entry, code and swizzle word counts, the GS unit's whole code and swizzle
memory, as port/wiiu/gx2_backend.cpp writes them) whose stretch from its entry runs straight through is written here as plain C: the same
arithmetic as the interpreter's (pmul1, a dot product summed in the same order), on the same register file. A JMPC or JMPU to an END
returns; a LOOP (not nested) is a loop, and a uniform addressed by its counter is found as the interpreter finds it. Anything else (calls,
ifs, jumps elsewhere, addressing by a0) leaves that program to the interpreter.

The output is the game's own code translated, so it goes only into the build made from the user's own logs (build3ds.py), never into this
repository. The runtime finds a function by its entry and a hash of the words it was made from (vertex.cpp: gs_native_find); check one
against the interpreter with tools/test_gs_native.cpp."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pica_disasm import ARITH, ARITHI  # noqa: E402

REG_IN, REG_TMP, REG_UNI, REG_OUT = 0, 16, 32, 128


def records(d):
    p = 0
    while p + 16 <= len(d) and d[p:p + 4] == b'GS  ':
        entry, nc, ns = struct.unpack_from('<III', d, p + 4)
        code = struct.unpack_from(f'<{nc}I', d, p + 16)
        swz = struct.unpack_from(f'<{ns}I', d, p + 16 + nc * 4)
        yield entry, code, swz
        p += 16 + (nc + ns) * 4


def reg_src(r, idx=0):
    """the register file's index for a source: with idx 3 a uniform's number has the loop counter added (rel, as shader.cpp's srcreg)"""
    if idx and r >= 0x20:
        if idx != 3:
            raise Unsupported('addressing relative to a0')
        return f'rel({r - 0x20}, aL)'
    return r + (REG_IN if r < 0x10 else REG_TMP - 0x10 if r < 0x20 else REG_UNI - 0x20)


def operand(name, desc, which, i):
    shift, negbit = [(5, 4), (14, 13), (23, 22)][which]
    c = (desc >> shift >> (6 - 2 * i)) & 3
    return f'{"-" if desc >> negbit & 1 else ""}{name}[{c}]'


def dst(r):
    return f'r[{REG_OUT} + *bank + {r}]' if r < 0x10 else f'r[{REG_TMP + r - 0x10}]'


class Unsupported(Exception):
    pass


def translate(entry, code, swz, fname):
    """(C source, end, swizzle entries used) for the program from entry, or Unsupported"""
    out, used, end, loop_end = [], set(), None, None
    pc = entry
    while True:
        if pc >= len(code):
            raise Unsupported('runs off the end')
        w = code[pc]
        op = w >> 26
        o = []
        if op >= 0x30:
            imm = op < 0x38
            di = w & 0x1f
            if imm:
                s3, s2, s1 = (w >> 5) & 0x7f, (w >> 12) & 0x1f, (w >> 17) & 0x1f
            else:
                s3, s2, s1 = (w >> 5) & 0x1f, (w >> 10) & 0x7f, (w >> 17) & 0x1f
            idx = (w >> 22) & 3
            d = swz[di] if di < len(swz) else 0
            used.add(di)
            o.append(f'const float *A = r[{reg_src(s1)}], *B = r[{reg_src(s2, 0 if imm else idx)}], *C = r[{reg_src(s3, idx if imm else 0)}]; '
                     f'float *D = {dst((w >> 24) & 0x1f)};')
            lanes = [i for i in range(4) if d >> (3 - i) & 1]
            o += [f'const float t{i} = pmul1({operand("A", d, 0, i)}, {operand("B", d, 1, i)}) + {operand("C", d, 2, i)};' for i in lanes]
            o += [f'D[{i}] = t{i};' for i in lanes]
        elif op in ARITH or op in ARITHI:
            di = w & 0x7f
            idx = (w >> 19) & 3
            d = swz[di] if di < len(swz) else 0
            used.add(di)
            if op in ARITHI:
                s1, s2, name = (w >> 14) & 0x1f, (w >> 7) & 0x7f, ARITHI[op]
                a_at, b_at = reg_src(s1), reg_src(s2, idx)
            else:
                s1, s2, name = (w >> 12) & 0x7f, (w >> 7) & 0x1f, ARITH[op]
                a_at, b_at = reg_src(s1, idx), reg_src(s2)
            o.append(f'const float *A = r[{a_at}], *B = r[{b_at}]; float *D = {dst((w >> 21) & 0x1f)}; (void)B;')
            lanes = [i for i in range(4) if d >> (3 - i) & 1]
            if name in ('dp3', 'dp4'):
                o.append('float s = 0.0f;')
                o += [f's += pmul1({operand("A", d, 0, i)}, {operand("B", d, 1, i)});' for i in range(3 if name == 'dp3' else 4)]
                o += [f'D[{i}] = s;' for i in lanes]
            elif name in ('rcp', 'rsq'):
                x = operand('A', d, 0, 0)
                o.append(f'const float s = 1.0f / {x if name == "rcp" else "sqrtf(" + x + ")"};')
                o += [f'D[{i}] = s;' for i in lanes]
            else:
                for i in lanes:
                    a, b = operand('A', d, 0, i), operand('B', d, 1, i)
                    expr = {'mov': a, 'add': f'{a} + {b}', 'mul': f'pmul1({a}, {b})', 'flr': f'floorf({a})',
                            'min': f'({a} < {b} ? {a} : {b})', 'max': f'({a} > {b} ? {a} : {b})'}.get(name)
                    if expr is None:
                        raise Unsupported(name)
                    o.append(f'const float t{i} = {expr};')
                o += [f'D[{i}] = t{i};' for i in lanes]
        elif op in (0x2e, 0x2f):
            di = w & 0x7f
            d = swz[di] if di < len(swz) else 0
            used.add(di)
            o.append(f'const float *A = r[{reg_src((w >> 12) & 0x7f, (w >> 19) & 3)}], *B = r[{reg_src((w >> 7) & 0x1f)}];')
            for i, cop in ((0, (w >> 24) & 7), (1, (w >> 21) & 7)):
                if cop < 6:
                    o.append(f'cond[{i}] = {operand("A", d, 0, i)} {["==", "!=", "<", "<=", ">", ">="][cop]} {operand("B", d, 1, i)};')
        elif op in (0x2c, 0x2d):  # JMPC, JMPU: only to the END
            dest = (w >> 10) & 0xfff
            if dest >= len(code) or code[dest] >> 26 != 0x22:
                raise Unsupported('a jump elsewhere than an END')
            if op == 0x2d:  # jumps on its bool being set, or with the low bit of num on its being clear
                o.append(f'if (b[{(w >> 22) & 0xf}] == {int(not (w & 1))}) return 1;')
            else:
                fop, refy, refx = (w >> 22) & 3, w >> 24 & 1, w >> 25 & 1
                x, y = f'cond[0] == {refx}', f'cond[1] == {refy}'
                o.append(f'if ({[f"({x}) || ({y})", f"({x}) && ({y})", x, y][fop]}) return 1;')
        elif op == 0x2b:  # SETEMIT
            o.append(f'vid = {(w >> 24) & 3}; prim = {w >> 23 & 1}; wind = {w >> 22 & 1};')
        elif op == 0x2a:  # EMIT: the runtime copies the outputs, assembles, and turns the bank over
            o.append('emit(ctx, vid, prim, wind);')
        elif op == 0x29:  # LOOP: the body, to its last word, runs the int uniform's count + 1 times, the counter going up by its step
            if loop_end is not None:
                raise Unsupported('a loop in a loop')
            loop_end = (w >> 10) & 0xfff
            n = (w >> 22) & 3
            if loop_end <= pc or loop_end >= len(code):
                raise Unsupported('a loop that ends before it starts')
            out.append(f'    aL = ui[{n}][1];\n    for (unsigned left = ui[{n}][0];; left--, aL += ui[{n}][2]) {{  /* {pc:03x} */')
        elif op == 0x22:  # END
            if loop_end is not None:
                raise Unsupported('an END inside a loop')
            end = pc + 1
        elif op != 0x21:  # NOP
            raise Unsupported(f'opcode {op:02x}')
        if o:
            out.append(f'    {{  /* {pc:03x} */\n        ' + '\n        '.join(o) + '\n    }')
        if pc == loop_end:
            out.append('    if (!left) break;\n    }')
            loop_end = None
        pc += 1
        if end is not None:
            break
    src = [f'/* the program from {entry:#x} to {end - 1:#x} */',
           f'static int {fname}(float (*r)[4], unsigned *bank, unsigned char *cond, const unsigned char *b, const unsigned char (*ui)[4], '
           f'gs_emit_fn emit, void *ctx) {{',
           f'    unsigned vid = 0, prim = 0, wind = 0;',
           f'    int aL = 0;',
           f'    (void)vid; (void)prim; (void)wind; (void)cond; (void)b; (void)ui; (void)aL;']
    return '\n'.join(src + out + ['    return 1;', '}']), end, sorted(used)


def independence(entry, end, code, swz):
    """Whether a run of the stretch reads only what it wrote itself (and uniforms), so runs can be made in any order or at once, and
    the output registers that hold this run's own values at every EMIT, as a mask: (independent, mask). The runtime runs a program's
    primitives on two threads only if so, and only if the outputs it gathers are within the mask (vertex.cpp)."""
    tmp = [0] * 16
    out = [[0] * 16, [0] * 16]
    bank, ok, mask = 0, True, 0xffff

    def comp(d, which, i):
        return (d >> [5, 14, 23][which] >> (6 - 2 * i)) & 3

    def read(reg, lane):
        nonlocal ok
        if reg < 0x10 or (reg < 0x20 and not tmp[reg - 0x10] >> lane & 1):
            ok = False  # an input register, or a temporary's lane from before this run

    for pc in range(entry, end):
        w = code[pc]
        op = w >> 26
        if op >= 0x30:
            d = swz[w & 0x1f] if (w & 0x1f) < len(swz) else 0
            srcs = [(w >> 17) & 0x1f, (w >> 12) & 0x1f, (w >> 5) & 0x7f] if op < 0x38 else [(w >> 17) & 0x1f, (w >> 10) & 0x7f, (w >> 5) & 0x1f]
            lanes = [i for i in range(4) if d >> (3 - i) & 1]
            for which in range(3):
                for i in lanes:
                    read(srcs[which], comp(d, which, i))
            dn = (w >> 24) & 0x1f
        elif op in ARITH or op in ARITHI:
            d = swz[w & 0x7f] if (w & 0x7f) < len(swz) else 0
            name = ARITHI[op] if op in ARITHI else ARITH[op]
            srcs = [(w >> 14) & 0x1f, (w >> 7) & 0x7f] if op in ARITHI else [(w >> 12) & 0x7f, (w >> 7) & 0x1f]
            lanes = [i for i in range(4) if d >> (3 - i) & 1]
            if name in ('dp3', 'dp4'):
                uses = [(which, i) for which in (0, 1) for i in range(3 if name == 'dp3' else 4)]
            elif name in ('rcp', 'rsq'):
                uses = [(0, 0)]
            elif name in ('mov', 'flr'):
                uses = [(0, i) for i in lanes]
            else:
                uses = [(which, i) for which in (0, 1) for i in lanes]
            for which, i in uses:
                read(srcs[which], comp(d, which, i))
            dn = (w >> 21) & 0x1f
        elif op in (0x2e, 0x2f):
            d = swz[w & 0x7f] if (w & 0x7f) < len(swz) else 0
            for which, reg in ((0, (w >> 12) & 0x7f), (1, (w >> 7) & 0x1f)):
                for i in (0, 1):
                    read(reg, comp(d, which, i))
            continue
        elif op == 0x2a:
            mask &= sum(1 << r for r in range(16) if out[bank][r] == 0xf)
            bank ^= 1
            continue
        elif op in (0x29, 0x2d):  # a loop, or a bool's jump past some of it: what a run wrote isn't the same on every path. Not shared.
            ok = False
            continue
        else:
            continue
        bits = sum(1 << i for i in lanes)
        if dn < 0x10:
            out[bank][dn] |= bits
        else:
            tmp[dn - 0x10] |= bits
    return ok, mask if ok else 0


def fnv(words):
    h = 0xcbf29ce484222325
    for w in words:
        for byte in struct.pack('<I', w):
            h = ((h ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return h


def main():
    recs = []
    for log in sys.argv[2:]:
        with open(log, 'rb') as f:
            recs += list(records(f.read()))
    funcs, table, seen = [], [], set()
    for entry, code, swz in recs:
        name = f'gs_native_{len(funcs)}'
        try:
            src, end, used = translate(entry, code, swz, name)
        except Unsupported as e:
            print(f'geometry shader at {entry:#x}: left to the interpreter ({e})', file=sys.stderr)
            continue
        key = (entry, fnv(code[entry:end]), fnv(swz[d] if d < len(swz) else 0 for d in used))
        if key in seen:
            continue
        seen.add(key)
        funcs.append(src + f'\nstatic const unsigned short {name}_swizzles[] = {{{", ".join(str(d) for d in used)}}};')
        indep, outmask = independence(entry, end, code, swz)
        table.append(f'    {{{key[1]:#x}ull, {key[2]:#x}ull, {entry:#x}, {end:#x}, {len(used)}, {name}_swizzles, {name}, {int(indep)}, {outmask:#x}}},')
    with open(sys.argv[1], 'w', newline='\n') as f:
        f.write('/* Generated by tools/pica_to_c.py from this game\'s logged geometry shaders: the game\'s own code, never to be shared. */\n')
        f.write('#include <math.h>\n\n')
        f.write('typedef void (*gs_emit_fn)(void *ctx, unsigned vid, unsigned prim, unsigned wind);\n')
        f.write('typedef int (*gs_native_fn)(float (*r)[4], unsigned *bank, unsigned char *cond, const unsigned char *b, const unsigned char (*ui)[4], gs_emit_fn emit, void *ctx);\n')
        f.write('struct gs_native { unsigned long long code_hash, swizzle_hash; unsigned entry, end, nswizzles; const unsigned short *swizzles; gs_native_fn fn; unsigned independent, outmask; };\n')
        f.write('static inline float pmul1(float a, float b) { float r = a * b; if (r != r && a == a && b == b) r = 0; return r; }\n')
        f.write('/* where a uniform is in the register file with the loop counter added: as the interpreter finds it (shader.cpp: srcreg) */\n')
        f.write('static inline unsigned rel(unsigned uni, int off) { if (off < -128 || off > 127) off = 0; uni = (uni + (unsigned)off) & 0x7f; '
                'return uni >= 96 ? 161 : 32 + uni; }\n\n')
        f.write('\n\n'.join(funcs) + '\n\n')
        f.write('extern const struct gs_native gs_native_table[];  /* external in C++ too */\nextern const unsigned gs_native_count;\n')
        f.write('const struct gs_native gs_native_table[] = {\n' + '\n'.join(table) + ('\n' if table else '    {0}\n') + '};\n')
        f.write(f'const unsigned gs_native_count = {len(table)};\n')
    print(f'{len(table)} geometry shaders made native', file=sys.stderr)


if __name__ == '__main__':
    main()
