#!/usr/bin/env python3
"""ARM11 to C, ahead of time: lift.py <workload.elf> <outdir>. Guest registers and flags become C locals, so the compiler
removes dead ones; each guest function becomes one C function, and tail branches are true tail calls.

Emits two variants, so one run on hardware measures both. "safe" syncs every register a function uses around every call.
"abi" trusts the ARM calling convention: r4-r11, sp and s16-s31 survive calls, so they stay in locals, and only argument
and return registers go through memory. Hand-written code that breaks the convention would need the safe form.

shortcut: reads functions and code/data boundaries from ELF symbols, and handles ARM mode only. Retail games have no
symbols and use Thumb too, so a real game needs code discovery and a Thumb decoder. Anything it can't translate is a
hard error naming the address, never a silent skip."""
import os
import re
import subprocess
import sys

COND = ['zf', '!zf', 'cf', '!cf', 'nf', '!nf', 'vf', '!vf', '(cf && !zf)', '(!cf || zf)', '(nf == vf)', '(nf != vf)',
        '(!zf && nf == vf)', '(zf || nf != vf)']
LOGICAL = {0, 1, 8, 9, 12, 13, 14, 15}
SP, LR, PC = 13, 14, 15


class Unsupported(Exception):
    pass


class Image:
    def __init__(self, elf):
        self.base = 0x00100000
        with open(os.path.join(os.path.dirname(elf), 'code.bin'), 'rb') as f:
            self.code = f.read()
        syms = []
        for line in subprocess.check_output(['arm-none-eabi-readelf', '-sW', elf], text=True).splitlines():
            p = line.split()
            if len(p) == 8 and p[0].endswith(':') and p[6].isdigit():
                syms.append((int(p[1], 16), p[3], p[7]))
        v = {name: addr for addr, _, name in syms}
        self.text_start, self.text_end = v['__text_start'], v['__text_end']
        in_text = lambda a: self.text_start <= a < self.text_end
        self.marks = sorted((a, n) for a, _, n in syms if n in ('$a', '$d') and in_text(a))
        assert self.marks and self.marks[0][0] == self.text_start
        self.functions = sorted({a for a, t, n in syms if in_text(a) and t in ('FUNC', 'NOTYPE')
                                 and not n.startswith(('$', '__text_'))})
        assert self.text_start in self.functions

    def word(self, a):
        return int.from_bytes(self.code[a - self.base:a - self.base + 4], 'little')

    def is_code(self, a):
        kind = '$a'
        for at, n in self.marks:
            if at > a:
                break
            kind = n
        return kind == '$a'

    def is_literal(self, a):
        return self.text_start <= a < self.text_end and not self.is_code(a)


def R(n, addr):
    return f'0x{addr + 8:x}u' if n == PC else f'r{n}'


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def fill(expr, a, b):
    """Puts operands into an expression in one pass, so an operand's own text is never rewritten."""
    return re.sub(r'\b[ab]\b', lambda m: a if m.group(0) == 'a' else b, expr)


def operand2(w, addr, want_carry):
    """The flexible second operand, and the shifter's carry out (None means the carry flag is unchanged)."""
    if w & (1 << 25):
        rot = ((w >> 8) & 15) * 2
        val = ror(w & 255, rot)
        return f'0x{val:x}u', (str(val >> 31) if rot and want_carry else None)
    rm, kind = R(w & 15, addr), (w >> 5) & 3
    if w & (1 << 4):
        rs = f'r{(w >> 8) & 15}'
        if want_carry:
            raise Unsupported('flag-setting shift by register')
        return f'{["lsl_r", "lsr_r", "asr_r", "ror_r"][kind]}({rm}, {rs})', None
    n = (w >> 7) & 31
    if kind == 0:
        return (rm, None) if n == 0 else (f'({rm} << {n})', f'(({rm} >> {32 - n}) & 1)')
    if kind == 1:
        return ('0u', f'({rm} >> 31)') if n == 0 else (f'({rm} >> {n})', f'(({rm} >> {n - 1}) & 1)')
    if kind == 2:
        n = n or 32
        return f'(u32)((s32){rm} >> {min(n, 31)})', f'(((u32)((s32){rm} >> {min(n - 1, 31)})) & 1)'
    if n == 0:
        return f'((cf << 31) | ({rm} >> 1))', f'({rm} & 1)'
    return f'(({rm} >> {n}) | ({rm} << {32 - n}))', f'(({rm} >> {n - 1}) & 1)'


class Fn:
    def __init__(self, img, start, end, functions, abi, prefix):
        self.img, self.start, self.end, self.functions = img, start, end, functions
        self.abi, self.prefix = abi, prefix
        self.insts = [a for a in range(start, end, 4) if img.is_code(a)]
        self.labels = set()

    def name(self, a):
        return f'{self.prefix}_{a:08x}'

    def tail(self, target):
        if target not in self.functions:
            raise Unsupported(f'branch to {target:#x}, which is not a known function')
        return f'SO(); MUSTTAIL return {self.name(target)}(c);'

    def indirect_tail(self, value):
        return f'c->pc = {value}; SO(); MUSTTAIL return rt_dispatch(c);'

    def ret(self, value):
        return f'c->pc = {value}; SOR(); return;'

    def switch(self, addr, value, targets):
        for t in targets:
            assert self.start <= t < self.end, hex(t)
            self.labels.add(t)
        cases = ' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in sorted(set(targets)))
        return f'{{ u32 t = {value}; switch (t) {{ {cases} default: rt_bad_jump(c, t, 0x{addr:x}u); return; }} }}'

    def after_call(self, ret_addr):
        return f'SIC(); if (__builtin_expect(c->pc != 0x{ret_addr:x}u, 0)) return;'

    # ---- data processing
    def dp(self, addr, w):
        op, s, rn, rd = (w >> 21) & 15, (w >> 20) & 1, (w >> 16) & 15, (w >> 12) & 15
        b, sc = operand2(w, addr, s and op in LOGICAL)
        a = R(rn, addr)
        expr = {0: 'a & b', 1: 'a ^ b', 2: 'a - b', 3: 'b - a', 4: 'a + b', 5: 'a + b + cf', 6: 'a - b - !cf',
                7: 'b - a - !cf', 8: 'a & b', 9: 'a ^ b', 10: 'a - b', 11: 'a + b', 12: 'a | b', 13: 'b', 14: 'a & ~b',
                15: '~b'}[op]
        if rd == PC and not (8 <= op <= 11):
            if s:
                raise Unsupported('flag-setting write to pc')
            value = fill(expr, a, b)
            if op == 13 and (w & 0x0FFFFFFF) == 0x01A0F00E:  # mov pc, lr
                return [self.ret('r14')]
            if rn == PC and op == 4:  # add pc, pc, rX, lsl #n: a computed jump within this function
                return [self.switch(addr, value, [t for t in self.insts if t > addr])]
            return [self.indirect_tail(value)]
        if not s:
            if 8 <= op <= 11:
                raise Unsupported('compare without flags')
            return [f'r{rd} = {fill(expr, a, b)};']
        out = ['{']
        if op not in (13, 15):
            out.append(f'u32 a = {a};')
        out.append(f'u32 b = {b};')
        if op in LOGICAL:
            if sc is not None:
                out.append(f'u32 sc = {sc};')
            out.append(f'u32 v = {expr};')
            if sc is not None:
                out.append('cf = sc;')
        elif op in (2, 10):
            out += ['u32 v = a - b;', 'cf = a >= b;', 'vf = ((a ^ b) & (a ^ v)) >> 31;']
        elif op == 3:
            out += ['u32 v = b - a;', 'cf = b >= a;', 'vf = ((b ^ a) & (b ^ v)) >> 31;']
        elif op in (4, 11):
            out += ['u32 v = a + b;', 'cf = v < a;', 'vf = ((a ^ v) & (b ^ v)) >> 31;']
        elif op == 5:
            out += ['u32 ci = cf;', 'u32 v = a + b + ci;', 'cf = ci ? v <= a : v < a;', 'vf = ((a ^ v) & (b ^ v)) >> 31;']
        elif op == 6:
            out += ['u32 ci = cf;', 'u32 v = a - b - !ci;', 'cf = ci ? a >= b : a > b;', 'vf = ((a ^ b) & (a ^ v)) >> 31;']
        else:
            out += ['u32 ci = cf;', 'u32 v = b - a - !ci;', 'cf = ci ? b >= a : b > a;', 'vf = ((b ^ a) & (b ^ v)) >> 31;']
        out += ['nf = v >> 31;', 'zf = v == 0;']
        if not 8 <= op <= 11:
            out.append(f'r{rd} = v;')
        out.append('}')
        return [' '.join(out)]

    def mul(self, addr, w):
        rd, rn, rs, rm, s = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15, (w >> 20) & 1
        expr = f'r{rm} * r{rs}' + (f' + r{rn}' if w & (1 << 21) else '')
        flags = f' nf = r{rd} >> 31; zf = r{rd} == 0;' if s else ''
        return [f'r{rd} = {expr};{flags}']

    def mull(self, addr, w):
        hi, lo, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        signed, accumulate, s = (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        if s:
            raise Unsupported('flag-setting long multiply')
        ty, cast = ('s64', 's32') if signed else ('u64', 'u32')
        add = f' + (((u64)r{hi} << 32) | r{lo})' if accumulate else ''
        return [f'{{ u64 v = (u64)(({ty})({cast})r{rm} * ({ty})({cast})r{rs}){add}; r{lo} = (u32)v; r{hi} = (u32)(v >> 32); }}']

    # ---- memory
    def literal(self, a):
        return self.img.word(a) if self.img.is_literal(a) else None

    def ldst(self, addr, w):
        p, u, b, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd = (w >> 16) & 15, (w >> 12) & 15
        if w & (1 << 25):
            if w & (1 << 4):
                raise Unsupported('media instruction')
            off, _ = operand2(w & ~(1 << 25), addr, False)
            if rd == PC and rn == PC and load and p:  # ldr pc, [pc, rX, lsl #2]: a jump table right after this
                table = addr + 8
                targets = []
                while self.img.is_literal(table) and self.start <= self.img.word(table) < self.end:
                    targets.append(self.img.word(table))
                    table += 4
                assert targets, hex(addr)
                return [self.switch(addr, f'RD32(0x{addr + 8:x}u + {off})', targets)]
        else:
            off = f'0x{w & 0xFFF:x}u'
            if rn == PC and load and p and not wb:
                value = self.literal(addr + 8 + ((w & 0xFFF) if u else -(w & 0xFFF)))
                if value is not None and not b and rd != PC:
                    return [f'r{rd} = 0x{value:x}u;']
        base = R(rn, addr)
        new = f'{base} {"+" if u else "-"} {off}'
        ea = new if p else base
        writeback = f'r{rn} = {new};' if (not p or wb) else ''
        if rn == PC and writeback:
            raise Unsupported('pc write-back')
        if load:
            read = f'RD8(ea)' if b else 'RD32(ea)'
            if rd == PC:
                if rn == SP and not p and u:  # pop {pc}
                    return [f'{{ u32 ea = {ea}; u32 v = RD32(ea); {writeback} {self.ret("v")} }}']
                raise Unsupported('load to pc')
            return [f'{{ u32 ea = {ea}; u32 v = {read}; {writeback} r{rd} = v; }}']
        if rd == PC:
            raise Unsupported('store of pc')
        return [f'{{ u32 ea = {ea}; {"WR8" if b else "WR32"}(ea, r{rd}); {writeback} }}']

    def ldst_extra(self, addr, w):
        p, u, imm, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd, sh = (w >> 16) & 15, (w >> 12) & 15, (w >> 5) & 3
        off = f'0x{((w >> 4) & 0xF0) | (w & 15):x}u' if imm else f'r{w & 15}'
        base = R(rn, addr)
        new = f'{base} {"+" if u else "-"} {off}'
        ea = new if p else base
        writeback = f'r{rn} = {new};' if (not p or wb) else ''
        if rn == PC and writeback:
            raise Unsupported('pc write-back')
        if load:
            if sh == 1:
                return [f'{{ u32 ea = {ea}; u32 v = RD16(ea); {writeback} r{rd} = v; }}']
            if sh == 2:
                return [f'{{ u32 ea = {ea}; u32 v = (u32)(s32)(s8)RD8(ea); {writeback} r{rd} = v; }}']
            return [f'{{ u32 ea = {ea}; u32 v = (u32)(s32)(s16)RD16(ea); {writeback} r{rd} = v; }}']
        if sh == 1:
            return [f'{{ u32 ea = {ea}; WR16(ea, r{rd}); {writeback} }}']
        if sh == 2:  # ldrd shares this encoding with stores
            return [f'{{ u32 ea = {ea}; u32 v0 = RD32(ea), v1 = RD32(ea + 4); {writeback} r{rd} = v0; r{rd + 1} = v1; }}']
        return [f'{{ u32 ea = {ea}; WR32(ea, r{rd}); WR32(ea + 4, r{rd + 1}); {writeback} }}']

    def ldm(self, addr, w):
        p, u, s, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        if s:
            raise Unsupported('ldm/stm with user registers')
        rn, regs = (w >> 16) & 15, [r for r in range(16) if w & (1 << r)]
        n = len(regs)
        base = f'r{rn}'
        first = {(0, 1): '', (1, 1): ' + 4', (0, 0): f' - {4 * n - 4}', (1, 0): f' - {4 * n}'}[(p, u)]
        out = [f'{{ u32 ea = {base}{first};']
        new = f'{base} {"+" if u else "-"} {4 * n}'
        if load:
            if wb:
                out.append(f'r{rn} = {new};')
            pc_value = None
            for i, r in enumerate(regs):
                if r == PC:
                    pc_value = f'RD32(ea + {4 * i})'
                else:
                    out.append(f'r{r} = RD32(ea + {4 * i});')
            if pc_value:
                out.append(f'{{ u32 t = {pc_value}; {self.ret("t")} }}')
        else:
            if PC in regs:
                raise Unsupported('stm with pc')
            for i, r in enumerate(regs):
                out.append(f'WR32(ea + {4 * i}, r{r});')
            if wb:
                out.append(f'r{rn} = {new};')
        out.append('}')
        return [' '.join(out)]

    # ---- control flow
    def branch(self, addr, w):
        imm = w & 0xFFFFFF
        target = addr + 8 + ((imm - (1 << 24) if imm & (1 << 23) else imm) << 2)
        ret_addr = addr + 4
        if w & (1 << 24):
            if target not in self.functions:
                raise Unsupported(f'call to {target:#x}, which is not a known function')
            return [f'r14 = 0x{ret_addr:x}u; SOC(); {self.name(target)}(c); {self.after_call(ret_addr)}']
        if self.start <= target < self.end:
            self.labels.add(target)
            if target <= addr:  # the 3DS kernel preempts busy loops, so real code needs this check on back-branches
                return [f'if (__builtin_expect(rt_preempt_flag, 0)) {{ SO(); rt_preempt(c); SI(); }} goto L_{target:x};']
            return [f'goto L_{target:x};']
        return [self.tail(target)]

    # ---- VFP (single precision)
    def vfp_ldst(self, addr, w):
        p, u, d, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, vd, cp, imm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 255
        if cp not in (10, 11):
            raise Unsupported('coprocessor load/store')
        if p and not wb:
            if cp != 10:
                raise Unsupported('double-precision load/store')
            sd = (vd << 1) | d
            off = imm * 4
            ea_int = addr + 8 + (off if u else -off) if rn == PC else None
            if load and ea_int is not None and self.literal(ea_int) is not None:
                return [f'S{sd} = U2F(0x{self.literal(ea_int):x}u);']
            ea = f'{R(rn, addr)} {"+" if u else "-"} 0x{off:x}u'
            return [f'S{sd} = U2F(RD32({ea}));' if load else f'WR32({ea}, F2U(S{sd}));']
        if cp == 10:
            regs = [((vd << 1) | d) + i for i in range(imm)]
        else:  # d registers, as a pair of s registers each: vpush and vpop
            regs = [s for i in range(imm // 2) for s in (((vd | (d << 4)) + i) * 2, ((vd | (d << 4)) + i) * 2 + 1)]
        n = len(regs)
        if max(regs) > 31:
            raise Unsupported('registers above d15')
        base = f'r{rn}'
        out = [f'{{ u32 ea = {base}{" - " + str(4 * n) if p else ""};']
        for i, s in enumerate(regs):
            out.append(f'S{s} = U2F(RD32(ea + {4 * i}));' if load else f'WR32(ea + {4 * i}, F2U(S{s}));')
        if wb:
            out.append(f'r{rn} = {base} {"+" if u else "-"} {4 * n};')
        out.append('}')
        return [' '.join(out)]

    def vfp_dp(self, addr, w):
        if ((w >> 8) & 15) != 10:
            raise Unsupported('double-precision or other coprocessor operation')
        p, d, q, r_, s = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1, (w >> 6) & 1
        vn, vd, n, m = (w >> 16) & 15, (w >> 12) & 15, (w >> 7) & 1, (w >> 5) & 1
        sd, sn, sm = (vd << 1) | d, (vn << 1) | n, ((w & 15) << 1) | m
        D, N, M_ = f'S{sd}', f'S{sn}', f'S{sm}'
        if not p:
            table = {(0, 0, 0): f'{D} = {D} + {N} * {M_};', (0, 0, 1): f'{D} = {D} - {N} * {M_};',
                     (0, 1, 0): f'{D} = -{D} + {N} * {M_};', (0, 1, 1): f'{D} = -{D} - {N} * {M_};',
                     (1, 0, 0): f'{D} = {N} * {M_};', (1, 0, 1): f'{D} = -({N} * {M_});',
                     (1, 1, 0): f'{D} = {N} + {M_};', (1, 1, 1): f'{D} = {N} - {M_};'}
            return [table[(q, r_, s)]]
        if not q and not r_ and not s:
            return [f'{D} = {N} / {M_};']
        if not (q and r_ and s):
            raise Unsupported('VFP operation')
        ext = (vn << 1) | n
        if ext == 0:
            return [f'{D} = {M_};']
        if ext == 1:
            return [f'{D} = __builtin_fabsf({M_});']
        if ext == 2:
            return [f'{D} = -{M_};']
        if ext == 3:
            return [f'{D} = __builtin_sqrtf({M_});']
        if ext in (8, 9, 10, 11):
            other = M_ if ext in (8, 9) else '0.0f'
            return [f'{{ float a = {D}, b = {other}; fn = a < b; fz = a == b; fc = !(a < b); fv = a != a || b != b; }}']
        if ext == 16:
            return [f'{D} = (float)F2U({M_});']
        if ext == 17:
            return [f'{D} = (float)(s32)F2U({M_});']
        if ext in (24, 25):  # both rounding modes: round-to-nearest is unused by compiled code
            if ext == 24:
                raise Unsupported('float to unsigned using the current rounding mode')
            return [f'{D} = U2F(f2u32z({M_}));']
        if ext in (26, 27):
            if ext == 26:
                raise Unsupported('float to signed using the current rounding mode')
            return [f'{D} = U2F(f2s32z({M_}));']
        raise Unsupported(f'VFP extension operation {ext}')

    # ---- decode
    def translate(self, addr, w):
        top = (w >> 25) & 7
        if (w & 0x0FFFFFF0) == 0x012FFF10:  # bx
            rm = w & 15
            return [self.ret('r14')] if rm == LR else [self.indirect_tail(f'r{rm}')]
        if (w & 0x0FFFFFF0) == 0x012FFF30:  # blx reg
            ret_addr = addr + 4
            return [f'{{ u32 t = r{w & 15}; r14 = 0x{ret_addr:x}u; SOC(); c->pc = t; rt_dispatch(c); {self.after_call(ret_addr)} }}']
        if (w & 0x0FFF0FF0) == 0x016F0F10:  # clz
            return [f'r{(w >> 12) & 15} = clz32(r{w & 15});']
        if top == 0:
            if (w & 0x0FC000F0) == 0x00000090:
                return self.mul(addr, w)
            if (w & 0x0F8000F0) == 0x00800090:
                return self.mull(addr, w)
            if (w & 0x0E000090) == 0x00000090 and (w & 0x60):
                return self.ldst_extra(addr, w)
            if (w & 0x0F900000) == 0x01000000:
                raise Unsupported('miscellaneous instruction')
            return self.dp(addr, w)
        if top == 1:
            if (w & 0x0FFFFF00) == 0x0320F000:  # hints such as nop
                return []
            if (w & 0x0F900000) == 0x03000000:
                raise Unsupported('msr or movw')
            return self.dp(addr, w)
        if top == 2:
            return self.ldst(addr, w)
        if top == 3:
            if w & (1 << 4):
                ext = {0x06A00070: 'sxtb', 0x06B00070: 'sxth', 0x06E00070: 'uxtb', 0x06F00070: 'uxth'}.get(w & 0x0FF000F0)
                if (w & 0x000F0000) != 0x000F0000 or ext is None:
                    raise Unsupported('media instruction')
                rot = ((w >> 10) & 3) * 8
                value = f'ror_r(r{w & 15}, {rot})' if rot else f'r{w & 15}'
                cast = {'sxtb': '(u32)(s32)(s8)', 'sxth': '(u32)(s32)(s16)', 'uxtb': '(u32)(u8)', 'uxth': '(u32)(u16)'}[ext]
                return [f'r{(w >> 12) & 15} = {cast}{value};']
            return self.ldst(addr, w)
        if top == 4:
            return self.ldm(addr, w)
        if top == 5:
            return self.branch(addr, w)
        if top == 6:
            return self.vfp_ldst(addr, w)
        if w & (1 << 24):
            raise Unsupported('svc: the benchmark makes no system calls')
        if (w & 0x0FE00F7F) == 0x0E000A10:  # vmov between core and s register
            sn, rt = (((w >> 16) & 15) << 1) | ((w >> 7) & 1), (w >> 12) & 15
            return [f'r{rt} = F2U(S{sn});'] if w & (1 << 20) else [f'S{sn} = U2F(r{rt});']
        if (w & 0x0FFFFFFF) == 0x0EF1FA10:  # vmrs APSR_nzcv, fpscr
            return ['nf = fn; zf = fz; cf = fc; vf = fv;']
        if w & (1 << 4):
            raise Unsupported('coprocessor transfer')
        return self.vfp_dp(addr, w)

    def emit(self, errors):
        body = []
        for addr in self.insts:
            w = self.img.word(addr)
            cond = w >> 28
            try:
                if cond == 15:
                    raise Unsupported('unconditional-space instruction')
                lines = self.translate(addr, w)
            except Unsupported as e:
                errors.append(f'{addr:#x} ({w:08x}): {e}')
                continue
            text = ' '.join(lines)
            body.append((addr, f'if ({COND[cond]}) {{ {text} }}' if cond != 14 else text))
        last = self.insts[-1]
        word = self.img.word(last)
        ends = word >> 28 == 14 and (((word >> 25) & 7 == 5 and not word & (1 << 24)) or (word & 0x0FFFFFF0) == 0x012FFF10
                                     or ((word >> 25) & 7 == 4 and word & (1 << 20) and word & (1 << PC))
                                     or (word & 0x0FFFFFFF) == 0x01A0F00E)
        tail = []
        if not ends:
            tail.append(self.tail(self.end))
        out = []
        for addr, text in body:
            if addr in self.labels:
                out.append(f'L_{addr:x}:;')
            out.append(f'  {text}')
        out += [f'  {t}' for t in tail]
        code = '\n'.join(out)
        regs = sorted({int(x) for x in re.findall(r'\br(\d+)\b', code)})
        floats = sorted({int(x) for x in re.findall(r'\bS(\d+)\b', code)})
        load = lambda rs, fs: ' '.join([f'r{r} = c->r[{r}];' for r in rs] + [f'S{s} = c->s[{s}];' for s in fs])
        store = lambda rs, fs: ' '.join([f'c->r[{r}] = r{r};' for r in rs] + [f'c->s[{s}] = S{s};' for s in fs])
        si, so = load(regs, floats), store(regs, floats)
        if self.abi:
            pick = lambda have, want: [x for x in have if x in want]
            sie = load(*self.entry_sets(regs, floats))
            soc = store(pick(regs, {0, 1, 2, 3, 12, 13, 14}), pick(floats, set(range(16))))
            sic = load(pick(regs, {0, 1, 2, 3, 12}), pick(floats, {0, 1, 2, 3}))
            sor = store(pick(regs, {0, 1, 2, 3}), pick(floats, {0, 1, 2, 3}))
        else:
            sie, soc, sic, sor = si, so, si, so
        decl = ['u8 *const M = g_mem; (void)M;', 'u32 nf = 0, zf = 0, cf = 0, vf = 0, fn = 0, fz = 0, fc = 0, fv = 0;']
        if regs:
            decl.append('u32 ' + ', '.join(f'r{r} = 0' for r in regs) + ';')
        if floats:
            decl.append('float ' + ', '.join(f'S{s} = 0' for s in floats) + ';')
        head = '\n'.join(f'  {d}' for d in decl)
        macros = {'SI': si, 'SO': so, 'SIE': sie, 'SOC': soc, 'SIC': sic, 'SOR': sor}
        defs = ''.join(f'#define {k}() do {{ {v} }} while (0)\n' for k, v in macros.items())
        undefs = ''.join(f'#undef {k}\n' for k in macros)
        return f'{defs}void {self.name(self.start)}(CPU *c) {{\n{head}\n  SIE();\n{code}\n}}\n{undefs}'

    def entry_sets(self, regs, floats):
        """A register saved in the prologue only holds its caller's value, which the caller keeps in its own local
        under the convention. So its entry value is never needed, and isn't loaded."""
        skip_r, skip_s = set(), set()
        for addr in self.insts[:3]:
            w = self.img.word(addr)
            if (w & 0xFFFF0000) == 0xE92D0000:  # push
                skip_r |= {r for r in range(4, 12) if w & (1 << r)}
            elif (w & 0xFFBF0F00) == 0xED2D0B00:  # vpush of d registers
                first = ((w >> 12) & 15) | (((w >> 22) & 1) << 4)
                skip_s |= {x for i in range((w & 255) // 2) for x in ((first + i) * 2, (first + i) * 2 + 1) if x >= 16}
            else:
                break
        return [r for r in regs if r not in skip_r], [x for x in floats if x not in skip_s]


def split_at_foreign_branches(img):
    """A branch into the middle of another function makes its target a function of its own, reached by a tail call."""
    starts = set(img.functions)
    while True:
        ordered = sorted(starts)
        added = set()
        for a, b in zip(ordered, ordered[1:] + [img.text_end]):
            for addr in range(a, b, 4):
                w = img.word(addr)
                if img.is_code(addr) and (w >> 25) & 7 == 5 and not w & (1 << 24) and w >> 28 != 15:
                    imm = w & 0xFFFFFF
                    target = addr + 8 + ((imm - (1 << 24) if imm & (1 << 23) else imm) << 2)
                    if not a <= target < b and target not in starts:
                        added.add(target)
        if not added:
            return ordered
        starts |= added


def main():
    elf, out_dir = sys.argv[1:3]
    img = Image(elf)
    starts = split_at_foreign_branches(img)
    ends = starts[1:] + [img.text_end]
    os.makedirs(out_dir, exist_ok=True)
    for abi, prefix, label in ((True, 'fa', 'abi'), (False, 'fs', 'safe')):
        functions = {a: Fn(img, a, b, set(starts), abi, prefix) for a, b in zip(starts, ends)}
        errors, chunks = [], []
        for a in starts:
            if functions[a].insts:
                chunks.append(functions[a].emit(errors))
            else:
                errors.append(f'{a:#x}: a function with no code')
        if errors:
            sys.exit('cannot lift:\n  ' + '\n  '.join(errors))
        with open(os.path.join(out_dir, f'lifted_{label}.c'), 'w', newline='\n') as f:
            f.write(f"// Generated by tools/lift.py from the benchmark's own ARM workload: {label} variant.\n#include \"lift_rt.h\"\n\n")
            for a in starts:
                f.write(f'void {prefix}_{a:08x}(CPU *c);\n')
            f.write('\n' + '\n'.join(chunks))
            f.write(f'\nconst GuestEntry guest_entries_{label}[] = {{\n')
            for a in starts:
                f.write(f'  {{0x{a:x}u, {prefix}_{a:08x}}},\n')
            f.write(f'}};\nconst u32 guest_entry_count_{label} = {len(starts)};\n')
    count = sum(len(fn.insts) for fn in functions.values())
    print(f'lifted {count} instructions in {len(starts)} functions, two variants')


if __name__ == '__main__':
    main()
