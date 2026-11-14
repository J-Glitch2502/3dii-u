#!/usr/bin/env python3
"""Fragment states from an Azahar shader cache, for the shader generator: azahar_to_log.py <..._fs.vkch> <out.bin> [--check <fs_log.bin>...]

Azahar (and so a cache someone shares for a game) keeps, for every fragment shader it made, the PICA state it made it from (FSConfig,
src/video_core/shader/generator/pica_fs_config.h in Azahar): the six combiner stages, the combiner buffer, fog, the alpha test, the
lighting setup and its lookup tables. Its file (shaders/vulkan/transferable/<program id>_fs.vkch) is a list of entries, each a 16-byte
header (0x42, flags with bit 0 zstd, type, size, id), the payload, and a 16-byte footer; FS_CONFIG entries (type 4) hold an 8-byte version
and the 272-byte FSConfig. Each becomes the key fs_key() makes on a console (port/wiiu/fs_key.inc), normalized as fs_normal() does,
following runtime/gpu/raster.cpp's build_draw_state(): the texture units a combiner reads count as enabled and bound (Azahar keeps no
record of which are), and every state as drawing colour. Writes records as fs_log.bin has them. --check says how many of a run's own
logged states the cache has, to show the conversion matches the runtime's.
Needs the zstandard module (pip install zstandard)."""
import struct
import sys

import zstandard

FS_CONFIG = 4


def entries(path):
    d = open(path, 'rb').read()
    dz = zstandard.ZstdDecompressor()
    p = 0
    while p + 16 <= len(d):
        ver, flags, typ, size, _id = struct.unpack_from('<BBHIQ', d, p)
        if ver != 0x42 or size < 32 or p + size > len(d):
            break
        payload = d[p + 16:p + size - 16]
        if flags & 1:
            payload = dz.decompress(payload, max_output_size=4 << 20)
        yield typ, payload
        p += size


def sampler_supported(config, sampler):
    return {0: config != 1, 1: config not in (0, 1, 5), 8: config not in (2, 3), 3: config not in (0, 2, 4), 6: config != 3,
            5: config in (4, 5, 8), 4: config in (4, 5, 8)}.get(sampler, False)


def operands_norm(op):  # fs_normal's
    return 3 if op in (4, 8, 9) else 2 if op in (1, 2, 3, 5, 6, 7) else 1


def operands_fold(op):  # tev_finish_prepare's
    return 1 if op == 0 else 3 if op in (4, 8, 9) else 2


def key_from_config(c):
    """the normalized fragment key, as little-endian words, of one FSConfig (272 bytes)"""
    fb_raw = struct.unpack_from('<I', c, 0)[0]
    tex_raw = struct.unpack_from('<I', c, 32)[0]
    stages = [struct.unpack_from('<4I', c, 48 + 16 * i) for i in range(6)]
    wraps = [struct.unpack_from('<2I', c, 144 + 8 * i) for i in range(3)]
    li_raw = struct.unpack_from('<I', c, 168)[0]
    luts = [struct.unpack_from('<2I', c, 172 + 8 * i) for i in range(7)]  # d0 d1 sp fr rr rg rb
    lights = struct.unpack_from('<8H', c, 228)
    pt_raw = struct.unpack_from('<I', c, 244)[0]

    alpha_func = fb_raw & 7
    scissor = (fb_raw >> 3) & 3
    depthmap = (fb_raw >> 5) & 1
    shadow_rendering = (fb_raw >> 10) & 1
    tex0_type = tex_raw & 7
    tex2_coord1 = (tex_raw >> 3) & 1
    bufin = (tex_raw >> 4) & 0xff
    fog_mode = (tex_raw >> 12) & 7
    fog_flip = (tex_raw >> 15) & 1
    shadow_ortho = (tex_raw >> 16) & 1

    alpha_test = alpha_func != 1  # Azahar keeps Always for a test that's off
    need_shade = True
    uses = [False] * 4
    tev = []
    for s, (src, mod, ops, sc) in enumerate(stages):
        t = dict(csrc=[(src >> 4 * k) & 15 for k in range(3)], asrc=[(src >> 16 + 4 * k) & 15 for k in range(3)],
                 cmod=[(mod >> 4 * k) & 15 for k in range(3)], amod=[(mod >> 12 + 4 * k) & 7 for k in range(3)],
                 cop=ops & 15, aop=(ops >> 16) & 15)
        if s == 0:
            for k in (0, 1):
                if t['csrc'][k] == 15:
                    t['csrc'][k] = t['csrc'][2]
        cs, as_ = sc & 3, (sc >> 16) & 3
        t['cmul'], t['amul'] = (1 << cs if cs < 3 else 1), (1 << as_ if as_ < 3 else 1)
        t['upd_rgb'] = int(s < 4 and (bufin >> s) & 1)
        t['upd_a'] = int(s < 4 and (bufin >> (4 + s)) & 1)
        for k in range(3):
            if 3 <= t['csrc'][k] <= 6:
                uses[t['csrc'][k] - 3] = True
            if 3 <= t['asrc'][k] <= 6:
                uses[t['asrc'][k] - 3] = True
        tev.append(t)

    def identity(t):
        return t['cop'] == 0 and t['aop'] == 0 and t['csrc'][0] == 15 and t['cmod'][0] == 0 and t['asrc'][0] == 15 and t['amod'][0] == 0 \
            and t['cmul'] == 1 and t['amul'] == 1
    tev_n = 0
    for si, t in enumerate(tev):
        if not identity(t):
            tev_n = si + 1
    # tev_finish_prepare: leading stages of constants only are folded
    first = 0
    out_const, buf_const = True, True
    while first < tev_n:
        t = tev[first]

        def is_const(src):
            return src == 14 or (src == 15 and out_const) or (src == 13 and buf_const) or 7 <= src <= 12
        ok = all(is_const(t['csrc'][k]) for k in range(operands_fold(t['cop'])))
        if t['cop'] != 7:
            ok = ok and all(is_const(t['asrc'][k]) for k in range(operands_fold(t['aop'])))
        if not ok:
            break
        first += 1
    reads_buf = any(13 in (tev[si]['csrc'][k], tev[si]['asrc'][k]) for si in range(first, tev_n) for k in range(3))
    tev_list = [si for si in range(first, tev_n) if not (identity(tev[si]) and not reads_buf)]

    lighting = bool(li_raw & 1)
    L = {}
    if lighting:
        L = dict(num=(li_raw >> 1) & 15, bump_mode=(li_raw >> 5) & 3, bump_sel=(li_raw >> 7) & 3, bump_renorm=(li_raw >> 9) & 1,
                 clamp=(li_raw >> 10) & 1, config=(li_raw >> 11) & 15, prim_a=(li_raw >> 15) & 1, sec_a=(li_raw >> 16) & 1,
                 shadow=(li_raw >> 17) & 1, sh_prim=(li_raw >> 18) & 1, sh_sec=(li_raw >> 19) & 1, sh_inv=(li_raw >> 20) & 1,
                 sh_alpha=(li_raw >> 21) & 1, sh_sel=(li_raw >> 22) & 3)
        if L['bump_mode'] in (1, 2):
            uses[L['bump_sel'] & 3] = True
        if L['shadow']:
            uses[L['sh_sel'] & 3] = True

    words = []
    put = words.append
    # textures, as setup_texunit then fs_normal leave them
    texw = []
    for i in range(3):
        enabled = uses[i]
        ttype = tex0_type if i == 0 else 0
        if enabled and ttype == 5:
            enabled = False
        if not enabled:
            texw.append(0)
            continue
        coord = 1 if i == 2 and tex2_coord1 else i
        ws, wt = wraps[i]
        so = shadow_ortho
        if ttype != 2:
            so = 0
        if ws not in (1, 5):
            ws = 0
        if wt not in (1, 5):
            wt = 0
        if ttype in (1, 2, 4):
            coord = ws = wt = 0
        texw.append(1 | ttype << 2 | coord << 6 | ws << 8 | wt << 11 | so << 14)
    hw_blend = 1  # fs_normal: logic op copy, no blending
    put(int(need_shade) | int(not depthmap) << 1 | int(scissor == 1) << 2 | hw_blend << 10)
    for w in texw:
        put(w)
    put(len(tev_list))
    for si in tev_list:
        t = dict(tev[si], csrc=list(tev[si]['csrc']), asrc=list(tev[si]['asrc']), cmod=list(tev[si]['cmod']), amod=list(tev[si]['amod']))
        nc = operands_norm(t['cop'])
        na = 0 if t['cop'] == 7 or t['aop'] in (6, 7) else operands_norm(t['aop'])
        for j in range(3):
            if j >= nc:
                t['csrc'][j] = t['cmod'][j] = 0
            if j >= na:
                t['asrc'][j] = t['amod'][j] = 0
        if t['cop'] == 7 or t['aop'] in (6, 7):
            t['aop'] = 7 if t['cop'] == 7 else 6
        put(si)
        for j in range(3):
            put(t['csrc'][j] | t['asrc'][j] << 4 | t['cmod'][j] << 8 | t['amod'][j] << 12)
        put(t['cop'] | t['aop'] << 4 | t['cmul'] << 8 | t['amul'] << 12 | t['upd_rgb'] << 16 | t['upd_a'] << 17)
    put(int(lighting))
    if lighting:
        put(L['num'] | L['config'] << 4 | L['bump_mode'] << 8 | L['bump_sel'] << 10 | L['sh_sel'] << 12 | L['bump_renorm'] << 14 |
            L['shadow'] << 15 | L['sh_inv'] << 16 | L['sh_prim'] << 17 | L['sh_sec'] << 18 | L['sh_alpha'] << 19 | L['prim_a'] << 20 |
            L['sec_a'] << 21 | L['clamp'] << 22)
        # our order: d0 d1 sp fr rb rg rr; Azahar's: d0 d1 sp fr rr rg rb. Samplers as raster.cpp: 0 1 8 3 4 5 6
        az = {'d0': luts[0], 'd1': luts[1], 'sp': luts[2], 'fr': luts[3], 'rr': luts[4], 'rg': luts[5], 'rb': luts[6]}
        for name, sampler in (('d0', 0), ('d1', 1), ('sp', 8), ('fr', 3), ('rb', 4), ('rg', 5), ('rr', 6)):
            raw = az[name][0]
            en = bool(raw & 1) and sampler_supported(L['config'], sampler)
            put(int(en) | ((raw >> 1) & 1) << 1 | ((raw >> 2) & 7) << 2 if en else 0)
        for i in range(L['num']):
            lt = lights[i]
            put((lt & 7) | ((lt >> 3) & 1) << 3 | ((lt >> 4) & 1) << 4 | ((lt >> 7) & 1) << 5 | ((lt >> 8) & 1) << 6 | ((lt >> 5) & 1) << 7 |
                ((lt >> 6) & 1) << 8 | ((lt >> 9) & 1) << 9)
    fog = fog_mode == 5
    at = alpha_test
    put(int(fog) | (int(fog_flip) if fog else 0) << 1 | int(at) << 2 | (alpha_func if at else 0) << 3)
    put(3 << 24)  # no blending, logic op copy
    put(0)  # colour mask
    if shadow_rendering:
        return None  # a shadow map pass: our runtime draws those its own way
    return struct.pack(f'<{len(words)}I', *words)


def normalize_logged(k):
    """one of our own logged keys (raw), normalized as fs_normal() would: for --check"""
    w = list(struct.unpack(f'<{len(k) // 4}I', k))
    p = 0
    head = w[p]
    p += 1
    need_shade = head & 1
    out = [(head & 1) | (head & 2) | (head & 4) | 1 << 10]
    if not need_shade:
        return None
    texw = []
    for i in range(3):
        t = w[p]
        p += 1
        need, null, ttype, coord, ws, wt, so = t & 1, (t >> 1) & 1, (t >> 2) & 15, (t >> 6) & 3, (t >> 8) & 7, (t >> 11) & 7, (t >> 14) & 1
        if not need or null:
            texw.append(0 if not need else (need | 1 << 1))
            if need and null:
                texw[-1] = need | null << 1
            continue
        if i != 0:
            ttype = 0
        if ttype != 2:
            so = 0
        if ws not in (1, 5):
            ws = 0
        if wt not in (1, 5):
            wt = 0
        if ttype in (1, 2, 4):
            coord = ws = wt = 0
        texw.append(1 | ttype << 2 | coord << 6 | ws << 8 | wt << 11 | so << 14)
    out += texw
    n = w[p]
    p += 1
    out.append(n)
    for _ in range(n):
        si = w[p]
        srcs = w[p + 1:p + 4]
        opw = w[p + 4]
        p += 5
        cs = [s & 15 for s in srcs]
        as_ = [(s >> 4) & 15 for s in srcs]
        cm = [(s >> 8) & 15 for s in srcs]
        am = [(s >> 12) & 7 for s in srcs]
        cop, aop = opw & 15, (opw >> 4) & 15
        nc = operands_norm(cop)
        na = 0 if cop == 7 or aop in (6, 7) else operands_norm(aop)
        for j in range(3):
            if j >= nc:
                cs[j] = cm[j] = 0
            if j >= na:
                as_[j] = am[j] = 0
        if cop == 7 or aop in (6, 7):
            aop = 7 if cop == 7 else 6
        out.append(si)
        for j in range(3):
            out.append(cs[j] | as_[j] << 4 | cm[j] << 8 | am[j] << 12)
        out.append(cop | aop << 4 | (opw & ~0xff))
    lighting = w[p]
    p += 1
    out.append(lighting)
    if lighting:
        lw = w[p]
        p += 1
        out.append(lw)
        for _ in range(7):
            s = w[p]
            p += 1
            out.append(s if s & 1 else 0)
        for _ in range(lw & 15):
            out.append(w[p])
            p += 1
    fw = w[p]
    fog, flip, at, af = fw & 1, (fw >> 1) & 1, (fw >> 2) & 1, (fw >> 3) & 7
    out.append(fog | (flip if fog else 0) << 1 | at << 2 | (af if at else 0) << 3)
    out.append(3 << 24)
    out.append(0)
    return struct.pack(f'<{len(out)}I', *out)


def logged_keys(path):
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
        yield d[p + 4:p + 4 + n]
        p += 4 + n


def main():
    args = sys.argv[1:]
    check = []
    if '--check' in args:
        i = args.index('--check')
        check, args = args[i + 1:], args[:i]
    src, out = args
    keys = set()
    configs = skipped = 0
    for typ, payload in entries(src):
        if typ != FS_CONFIG:
            continue
        configs += 1
        if len(payload) != 280:
            skipped += 1
            continue
        k = key_from_config(payload[8:])
        if k is None:
            skipped += 1
            continue
        keys.add(k)
    with open(out, 'wb') as f:
        for k in sorted(keys):
            f.write(struct.pack('<I', len(k)) + k)
    print(f'{configs} fragment configs in the cache, {len(keys)} distinct states written to {out} ({skipped} left out: shadow map passes or another layout)')
    for c in check:
        mine = {normalize_logged(k) for k in logged_keys(c)} - {None}
        print(f'{c}: {len(mine & keys)} of {len(mine)} of its states are in the cache')


if __name__ == '__main__':
    main()
