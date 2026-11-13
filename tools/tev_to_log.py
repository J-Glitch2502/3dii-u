#!/usr/bin/env python3
"""Fragment states for every model material in a game's data, so no run has to reach a scene before its shader is compiled.

NW4C model files (CGFX .bcres, and the same blocks inside other files) keep each material's six combiner stages as the PICA register writes the game sends: six bursts at
a fixed stride, then the alpha test, buffer colour and buffer update writes. The rest of a fragment state (textures, lighting, fog) is set by the scene, so each material
is put into the settings the game's own runs logged around combiners with the same textures and lighting. Writes <game dir>/shader_logs/models_fs.bin, records like fs_log.bin.
tev_to_log.py <game dir>. NSMB2 on 2026-11-07: both fragment states the console missed that day came out of this, with no run having reached them.
Files are read as they are, or unpacked first: LZ10/LZ11/Level-5/Yaz0 (dvlb_to_log.py) or the 3DS's backward LZ (Sticker Star's .bcrez).
M&L's BG4 archives are opened entry by entry, nested ones too.
shortcut: Layton's XFSA archives aren't opened yet.
"""
import glob
import mmap
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dvlb_to_log import romfs_files, unpacked  # noqa: E402

TEV_REGS = (0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8)
HEADS = [0x804F0000 | r for r in TEV_REGS]  # all bytes, four more words, consecutive
FIRST = struct.pack('<I', HEADS[0])
OUT = 'models_fs.bin'


def materials(b):
    """(where, six stages of (source, operand, combine, colour, scale), {register: value} written after them)"""
    p = 0
    while True:
        p = b.find(FIRST, p)
        if p < 0:
            return
        at, p = p, p + 4
        stride = next((s for s in (0x1C, 0x18, 0x20) if at + 5 * s + 20 <= len(b) and
                       all(struct.unpack_from('<I', b, at + i * s)[0] == HEADS[i] for i in range(6))), None)
        if at % 4 or at < 4 or not stride:
            continue
        stages = [(struct.unpack_from('<I', b, at + i * stride - 4)[0],) + struct.unpack_from('<4I', b, at + i * stride + 4) for i in range(6)]
        extra, q = {}, at + 5 * stride + 20
        for _ in range(12):
            if q + 8 > len(b):
                break
            v, head = struct.unpack_from('<II', b, q)
            reg, mask, n = head & 0xFFFF, (head >> 16) & 15, (head >> 20) & 255
            if reg > 0x2FF or mask == 0:
                break
            extra[reg] = v
            q += 8 + 4 * (n + (n & 1))
        yield at, stages, extra


def blz(b):
    """the 3DS's backward LZ (code.bin's, and Sticker Star's .bcrez): decoded from the end, with a footer saying how far; None if it isn't one"""
    if len(b) < 8:
        return None
    top, grow = struct.unpack_from('<II', b, len(b) - 8)
    head, packed = top >> 24, top & 0xFFFFFF
    if not (8 <= head <= 11 and packed <= len(b) and 0 < grow < (64 << 20)):
        return None
    out = bytearray(b) + bytearray(grow)
    o, i, stop = len(out), len(b) - head, len(b) - packed
    try:
        while i > stop:
            i -= 1
            flags = out[i]
            for _ in range(8):
                if i <= stop:
                    break
                if flags & 0x80:
                    i -= 2
                    v = out[i] | out[i + 1] << 8
                    n, d = (v >> 12) + 3, (v & 0xFFF) + 3
                    if o + d > len(out) or o < n:
                        return None
                    if n <= d:
                        out[o - n:o] = out[o - n + d:o + d]
                        o -= n
                    else:
                        for _ in range(n):
                            o -= 1
                            out[o] = out[o + d]
                else:
                    i -= 1
                    o -= 1
                    out[o] = out[i]
                flags <<= 1
    except IndexError:
        return None
    return bytes(out) if o >= 0 else None


def contents(b, depth=0):
    """the file, or what it unpacks to when it holds no materials as it is; M&L's BG4 archives entry by entry (bit 31 of an entry's offset: packed with the backward LZ)"""
    if depth > 4:
        return
    if b[:4] == b'BG4\0' and len(b) >= 16:
        count = struct.unpack_from('<H', b, 6)[0]
        for i in range(count):
            if 0x10 + 14 * i + 8 > len(b):
                break
            o, size = struct.unpack_from('<II', b, 0x10 + 14 * i)
            e = b[o & 0x7FFFFFFF:(o & 0x7FFFFFFF) + (size & 0x7FFFFFFF)]
            e = blz(bytes(e)) if o >> 31 else e
            if e:
                yield from contents(e, depth + 1)
        return
    if b[:4] == b'darc':  # layouts, and models stored in it
        yield b
        for e in darc(b):
            yield from contents(e, depth + 1)
        return
    if b.find(FIRST) >= 0:  # as it is, unless it's a backward-LZ file whose unpacked start happens to hold one
        u = blz(bytes(b)) if b[:4] == b'CGFX' else None
        yield u if u and u.find(FIRST) >= 0 else b
        return
    for unpack in (lambda: unpacked(b, 0), lambda: blz(b)):
        try:
            u = unpack()
        except (IndexError, ValueError, struct.error):
            u = None
        if u and (u.find(FIRST) >= 0 or u[:4] in (b'BG4\0', b'darc')):
            yield from contents(u, depth + 1)
            return


def stage(i, w, upd):
    """build_draw_state()'s reading of one stage (runtime/gpu/raster.cpp)"""
    src, mod, ops, _konst, sc = w
    t = dict(csrc=[(src >> 4 * k) & 15 for k in range(3)], asrc=[(src >> 16 + 4 * k) & 15 for k in range(3)],
             cmod=[(mod >> 4 * k) & 15 for k in range(3)], amod=[(mod >> 12 + 4 * k) & 7 for k in range(3)], cop=ops & 15, aop=(ops >> 16) & 15)
    if i == 0:
        for k in (0, 1):
            if t['csrc'][k] == 15:
                t['csrc'][k] = t['csrc'][2]
    cs, als = sc & 3, (sc >> 16) & 3
    t['cmul'], t['amul'] = (1 << cs if cs < 3 else 1), (1 << als if als < 3 else 1)
    t['urgb'], t['ua'] = int(i < 4 and (upd >> (8 + i)) & 1), int(i < 4 and (upd >> (12 + i)) & 1)
    return t


def identity(t):
    return t['cop'] == 0 and t['aop'] == 0 and t['csrc'][0] == 15 and t['cmod'][0] == 0 and t['asrc'][0] == 15 and t['amod'][0] == 0 and t['cmul'] == 1 and t['amul'] == 1


def operands(op):
    return 1 if op == 0 else 3 if op in (4, 8, 9) else 2


def tev_list(stages):
    """tev_prepare() and tev_finish_prepare() (runtime/gpu/fragment.cpp): the stages a key lists, each with its index"""
    n = max((i + 1 for i, t in enumerate(stages) if not identity(t)), default=0)
    const = lambda s: s in (13, 14, 15) or 7 <= s <= 12  # noqa: E731
    first = 0
    while first < n:
        t = stages[first]
        c = all(const(t['csrc'][k]) for k in range(operands(t['cop'])))
        if t['cop'] != 7:
            c = c and all(const(t['asrc'][k]) for k in range(operands(t['aop'])))
        if not c:
            break
        first += 1
    reads_buf = any(13 in stages[s]['csrc'] or 13 in stages[s]['asrc'] for s in range(first, n))
    return [dict(stages[s], si=s) for s in range(first, n) if reads_buf or not identity(stages[s])]


def uses_tex(stages):
    return [any(s == 3 + u for t in stages for s in t['csrc'] + t['asrc']) for u in range(3)]


# ---------------------------------------------------------------- keys: fs_key_words() in port/wiiu/fs_key.inc, and back
def decode(k):
    w = list(struct.unpack(f'<{len(k) // 4}I', k))
    it = iter(w)
    get = lambda: next(it, 0)  # noqa: E731
    s = {'head': get()}
    if not s['head'] & 1:
        return s
    s['tex'] = [get() for _ in range(3)]
    s['tev'] = []
    for _ in range(min(get(), 6)):
        si, src, op = get(), [get() for _ in range(3)], get()
        s['tev'].append(dict(si=si % 6, csrc=[a & 15 for a in src], asrc=[a >> 4 & 15 for a in src], cmod=[a >> 8 & 15 for a in src], amod=[a >> 12 & 15 for a in src],
                             cop=op & 15, aop=op >> 4 & 15, cmul=op >> 8 & 15, amul=op >> 12 & 15, urgb=op >> 16 & 1, ua=op >> 17 & 1))
    s['light'] = get()
    if s['light']:
        s['lcfg'] = get()
        s['lsel'] = [get() for _ in range(7)]
        s['lights'] = [get() for _ in range(min(s['lcfg'] & 15, 8))]
    s['fogat'], s['blend'], s['mask'] = get(), get(), get()
    return s


def encode(s):
    w = [s['head']]
    if s['head'] & 1:
        w += s['tex'] + [len(s['tev'])]
        for t in s['tev']:
            w.append(t['si'])
            w += [t['csrc'][j] | t['asrc'][j] << 4 | t['cmod'][j] << 8 | t['amod'][j] << 12 for j in range(3)]
            w.append(t['cop'] | t['aop'] << 4 | t['cmul'] << 8 | t['amul'] << 12 | t['urgb'] << 16 | t['ua'] << 17)
        w.append(s['light'])
        if s['light']:
            w += [s['lcfg']] + s['lsel'] + s['lights']
        w += [s['fogat'], s['blend'], s['mask']]
    return struct.pack(f'<{len(w)}I', *w)


def normal(s):
    """fs_normal() in fs_key.inc: only what the compiled shader reads, so states that share a shader become one key"""
    s = dict(s, head=(s['head'] & 7) | 1 << 10)  # no depth bits or colour write; hw_blend
    if not s['head'] & 1:
        return s
    tex = []
    for i, w in enumerate(s['tex']):
        need, null, typ, coord, ws, wt, sho = w & 1, w >> 1 & 1, w >> 2 & 15, w >> 6 & 3, w >> 8 & 7, w >> 11 & 7, w >> 14 & 1
        if not need or null:
            typ = coord = ws = wt = sho = 0
        else:
            typ = 0 if i else typ
            sho = sho if typ == 2 else 0
            ws, wt = (ws if ws in (1, 5) else 0), (wt if wt in (1, 5) else 0)
            if typ in (1, 2, 4):
                coord = ws = wt = 0
        tex.append(need | null << 1 | typ << 2 | coord << 6 | ws << 8 | wt << 11 | sho << 14)
    tev = []
    for t in s['tev']:
        t = dict(t, csrc=list(t['csrc']), asrc=list(t['asrc']), cmod=list(t['cmod']), amod=list(t['amod']))
        n = lambda op: 3 if op in (4, 8, 9) else 2 if op in (1, 2, 3, 5, 6, 7) else 1  # noqa: E731
        whole = t['cop'] == 7 or t['aop'] in (6, 7)
        nc, na = n(t['cop']), 0 if whole else n(t['aop'])
        for j in range(3):
            if j >= nc:
                t['csrc'][j] = t['cmod'][j] = 0
            if j >= na:
                t['asrc'][j] = t['amod'][j] = 0
        if whole:
            t['aop'] = 7 if t['cop'] == 7 else 6
        tev.append(t)
    fogat = s['fogat']
    if not fogat & 4:
        fogat &= ~(7 << 3)
    if not fogat & 1:
        fogat &= ~2
    s = dict(s, tex=tex, tev=tev, fogat=fogat, blend=3 << 24, mask=0)
    if s['light']:
        s['lsel'] = [w if w & 1 else 0 for w in s['lsel']]
    return s


def records(path):
    """fragment keys in a log, or in a console's miss list (tagged records, as misses_to_log.py reads them)"""
    d, p, out = open(path, 'rb').read(), 0, []
    while p + 4 <= len(d):
        tag = d[p:p + 4]
        if tag == b'FS  ':
            n = struct.unpack_from('<I', d, p + 4)[0]
            out.append(d[p + 8:p + 8 + n])
            p += 8 + n
            continue
        if tag == b'VS  ':
            _, nc, ns = struct.unpack_from('<III', d, p + 4)
            p += 16 + (nc + ns) * 4 + 56
            continue
        n = struct.unpack_from('<I', d, p)[0]
        if n > 4096 or n % 4:
            raise ValueError(f'{path}: not a fragment log at {p}')
        out.append(d[p + 4:p + 4 + n])
        p += 4 + n
    return out


def tex_needed(s):
    return [bool(t & 1) and not t & 2 for t in s['tex']]


REG_STAGE = {r: i for i, r in enumerate(TEV_REGS)}
IDENT = dict(csrc=[15, 0, 0], asrc=[15, 0, 0], cmod=[0, 0, 0], amod=[0, 0, 0], cop=0, aop=0, cmul=1, amul=1, urgb=0, ua=0)
LIMIT = 3000  # new states a game may add: the most common logged settings first


def code_templates(cb):
    """texenv writes the game's code keeps as templates (particle presets, text): each C0 burst and the stage writes after it, {stage: [source, operand, combine, colour, scale]}, None where not written"""
    out, p = [], 0
    while (p := cb.find(FIRST, p)) >= 0:
        at, p = p, p + 4
        if at % 4 or at < 4:
            continue
        q, ent = at - 4, {}
        for _ in range(16):
            if q + 8 > len(cb):
                break
            v, head = struct.unpack_from('<II', cb, q)
            reg, mask, n = head & 0xFFFF, (head >> 16) & 15, (head >> 20) & 255
            if mask == 0 or reg > 0x2FF or n > 16 or (ent and reg in REG_STAGE and REG_STAGE[reg] in ent):
                break  # not a command, or the next template
            vals = [v] + list(struct.unpack_from(f'<{n}I', cb, q + 8))
            for k, x in enumerate(vals if head >> 31 else vals[:1]):
                base = max((b for b in REG_STAGE if b <= reg + k), default=None)
                if base is not None and reg + k - base < 5:
                    ent.setdefault(REG_STAGE[base], [None] * 5)[reg + k - base] = x
            q += 8 + 4 * (n + (n & 1))
            if not 0xC0 <= reg <= 0xFD:
                break
        if ent:
            out.append((at, ent))
    groups = []
    for at, ent in out:  # siblings: a table of templates writing the same stages
        if groups and at - groups[-1][-1][0] <= 0x40 and set(ent) == set(groups[-1][-1][1]):
            groups[-1].append((at, ent))
        else:
            groups.append([(at, ent)])
    return groups


def raw_stages(s):
    st = [dict(IDENT, csrc=list(IDENT['csrc']), asrc=list(IDENT['asrc'])) for _ in range(6)]
    for t in s['tev']:
        st[t['si']] = {k: v for k, v in t.items() if k != 'si'}
    return st


def apply_template(ent, s):
    st = raw_stages(s)
    for i, (src, op, comb, _col, sc) in ent.items():
        new = stage(i, (src or 0, op or 0, comb or 0, 0, sc or 0), 0)
        if sc is None:
            new['cmul'], new['amul'] = st[i]['cmul'], st[i]['amul']
        new['urgb'], new['ua'] = st[i]['urgb'], st[i]['ua']
        st[i] = new
    return encode(normal(dict(s, tev=tev_list(st))))


def sig(tev):
    return repr([sorted(t.items()) for t in tev])


def learned_tails(states, model_sigs):
    """stages the game's code puts after a model material's own (a fade, a tint), learned from the logs: a stage 4 or 5 seen after at least 10 different earlier
    sets, 2 of them models' own (counting what comes before stage 4), and every pairing of a stage 4 with a stage 5, which come and go independently"""
    after = {4: {}, 5: {}}
    for s in states:
        base = sig([t for t in s['tev'] if t['si'] < 4])
        for t in s['tev']:
            if t['si'] in after:
                e = after[t['si']].setdefault(repr(sorted(t.items())), (t, set(), set()))
                e[1].add(sig([u for u in s['tev'] if u['si'] < t['si']]))
                e[2].add(base)
    pick = {i: [t for t, pres, bases in after[i].values() if len(pres) >= 10 and len(bases & model_sigs) >= 2] for i in after}
    return [[a] for a in pick[4]] + [[b] for b in pick[5]] + [[a, b] for a in pick[4] for b in pick[5]]


# ---------------------------------------------------------------- CTR layouts (bclyt in darc archives): the layout library writes a material's own combiner stages at the
# end of the six and leaves the stages before them as the last draw set them, so a layout state is a leftover prefix (learned from the logs) plus the material's stages
LYT_SRC = {0: 3, 1: 4, 2: 5, 3: 6, 4: 14, 5: 0, 6: 15, 7: 13}  # texture 0-3, constant, vertex colour, previous, previous buffer
LYT_CMOD = [0, 1, 2, 3, 4, 5, 8, 9, 12, 13]
LYT_MODE = {6: 9, 7: 8}  # the rest are the PICA's own numbers; 7 -> 8 is in the logs (NSMB2's title screen coin counter)


def darc(b):
    if b[:4] != b'darc' or len(b) < 0x1C:
        return
    tab = struct.unpack_from('<I', b, 0x10)[0]
    if tab + 12 > len(b):
        return
    n = struct.unpack_from('<I', b, tab + 8)[0]
    for i in range(min(n, (len(b) - tab) // 12)):
        no, do, size = struct.unpack_from('<III', b, tab + i * 12)
        if not no & 0x01000000 and do + size <= len(b):
            yield b[do:do + size]


def lyt_materials(d):
    """each bclyt material's custom combiner stages (rgb word, alpha word), when it has any"""
    p = d.find(b'mat1')
    if p < 0 or d[:4] != b'CLYT':
        return
    for i in range(min(struct.unpack_from('<I', d, p + 8)[0], 4096)):
        if p + 16 + 4 * i > len(d) or p + struct.unpack_from('<I', d, p + 12 + 4 * i)[0] + 52 > len(d):
            return
        mo = p + struct.unpack_from('<I', d, p + 12 + 4 * i)[0]
        flags = struct.unpack_from('<I', d, mo + 48)[0]
        q = mo + 52 + 4 * (flags & 3) + 20 * ((flags >> 2) & 3) + 4 * ((flags >> 4) & 3)
        tevs = [struct.unpack_from('<II', d, q + 12 * k) for k in range((flags >> 6) & 7)]
        if tevs:
            yield tevs


def lyt_stage(rgb, alpha):
    c = [[LYT_SRC.get((w >> 4 * k) & 15, 15) for k in range(3)] for w in (rgb, alpha)]
    ops = [[(w >> 12 + 4 * k) & 15 for k in range(3)] for w in (rgb, alpha)]
    mode = [LYT_MODE.get((w >> 24) & 15, (w >> 24) & 15) for w in (rgb, alpha)]
    sc = [(w >> 28) & 3 for w in (rgb, alpha)]
    return dict(csrc=c[0], asrc=c[1], cmod=[LYT_CMOD[o] if o < 10 else 0 for o in ops[0]], amod=[o & 7 for o in ops[1]], cop=mode[0], aop=mode[1],
                cmul=1 << sc[0] if sc[0] < 3 else 1, amul=1 << sc[1] if sc[1] < 3 else 1, urgb=0, ua=0)


def layouts(blobs):
    """distinct custom layout materials, as their stage dicts placed at the end of the six"""
    out = {}
    for b in blobs:
        for d in darc(b):
            for tevs in lyt_materials(d):
                st = [dict(lyt_stage(*w), si=6 - len(tevs) + j) for j, w in enumerate(tevs)]
                out[repr(st)] = st
    return list(out.values())


def layout_states(mats, states, limit):
    """each material after every leftover prefix the logs show in front of a material of the same length"""
    seen = {}
    for m in mats:
        seen.setdefault(len(m), set()).add(sig(normal(dict(head=1, tex=[0, 0, 0], tev=tev_list(raw_with(m)), light=0, fogat=0, blend=0, mask=0))['tev'][-len(m):]))
    prefixes = {n: {} for n in seen}
    for s in states:
        for n in seen:
            tail = [t for t in s['tev'] if t['si'] >= 6 - n]
            if len(tail) == n and sig(tail) in seen[n]:
                pre = [t for t in s['tev'] if t['si'] < 6 - n]
                ctx = dict(s, tev=[])
                e = prefixes[n].setdefault(sig(pre) + encode(ctx).hex(), [pre, ctx, 0, n])
                e[2] += 1
    made = set()
    for pre, ctx, _, n in sorted((e for n in prefixes for e in prefixes[n].values()), key=lambda e: -e[2]):  # the most common leftovers first, up to the limit
        if len(made) >= limit:
            break
        for m in mats:
            if len(m) != n:
                continue
            st = raw_with([dict(t) for t in pre] + m)
            tev = tev_list(st)
            need = uses_tex(st)
            if tex_needed(ctx) != need:
                continue
            made.add(encode(normal(dict(ctx, tev=tev))))
    return made


def raw_with(stages):
    st = [dict(IDENT, csrc=list(IDENT['csrc']), asrc=list(IDENT['asrc'])) for _ in range(6)]
    for t in stages:
        st[t['si']] = {k: v for k, v in t.items() if k != 'si'}
    return st


def main():
    game = sys.argv[1].rstrip('/\\')
    logs = game + '/shader_logs'
    # the game's own runs and console misses: settings around combiners, and what is already compiled
    # --holdout: leave the console's miss lists out and say how many of them this would have predicted; writes nothing
    # A finished game's confirmed.bin (builder/shaderlogs.py) is left out: it holds an earlier build's guesses, and learning from them as if
    # runs had met them would change which states this one makes.
    holdout, held = '--holdout' in sys.argv, set()
    keys = set()
    for f in glob.glob(logs + '/*.bin'):
        b = os.path.basename(f)
        if b.startswith('other_') or b in (OUT, 'dvlb.bin', 'vs_log.bin', 'abi_vs_log.bin', 'variants.bin', 'confirmed.bin') or b.endswith('_vs.bin'):
            continue
        (held if holdout and 'console' in b else keys).update(records(f))
    for k in keys:
        assert encode(decode(k)) == k, 'key layout changed: update decode() and encode() from fs_key.inc'
    states = [normal(decode(k)) for k in keys]
    states = [s for s in states if s['head'] & 1]
    keys = {encode(normal(decode(k))) for k in keys}
    uses = {}
    for s in states:
        c = dict(s, tev=[], fogat=s['fogat'] & 3)  # alpha test comes from the material
        e = encode(c)
        uses[e] = (uses.get(e, (c, 0))[0], uses.get(e, (c, 0))[1] + 1)
    contexts = [c for c, n in sorted(uses.values(), key=lambda u: -u[1])]
    bases, lyt_blobs = [], []
    with open(game + '/romfs.bin', 'rb') as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for off, size in romfs_files(data):
            for b in contents(data[off:off + size]):
                if b[:4] == b'darc':
                    lyt_blobs.append(bytes(b))  # and its models are read as it is, and again from its files, unpacked
                for at, stages, extra in materials(b):
                    st = [stage(i, w, extra.get(0xE0, 0)) for i, w in enumerate(stages)]
                    m = b.rfind(b'MTOB', 0, at)
                    # CGFX: the material's flag for fragment lighting (bit 0) and its lighting layer config. Its fog flag (bit 4) is not gone by:
                    # Super Mario 3D Land draws materials in fog whose flag says none, and with it believed not one of the 64 states a console
                    # had to compile there was made ahead; without it, 19 (2026-11-12, --holdout).
                    if m >= 0:
                        flags = struct.unpack_from('<I', b, m + 0x14)[0]
                        lit, layer = bool(flags & 1), struct.unpack_from('<I', b, at - 0x20)[0]
                    else:  # BCH and the rest: lit if a stage reads the lighting, otherwise either way
                        lit = True if any(x in (1, 2) for t in st for x in t['csrc'] + t['asrc']) else None
                        layer = None
                    at_reg = extra.get(0x104, 0)
                    fogat = ((at_reg & 1) << 2 | ((at_reg >> 4) & 7) << 3) if at_reg & 1 else 0
                    bases.append((lit, layer, tev_list(st), fogat))
    bases = list({repr(x): x for x in bases}.values())
    model_sigs = {sig(normal(dict(head=1, tex=[0, 0, 0], tev=tev, light=0, fogat=0, blend=0, mask=0))['tev']) for _, _, tev, _ in bases}
    tails = [[]] + learned_tails(states, model_sigs)
    made = {}  # state -> which part found it
    for c in contexts:  # most common settings first, until the limit
        for lit, layer, tev, fogat in bases:
            if (lit is not None and bool(c['light']) != lit) or (c['light'] and layer is not None and (c['lcfg'] >> 4) & 15 != layer):
                continue
            top = max((t['si'] for t in tev), default=-1)
            for tail in tails:
                if tail and tail[0]['si'] <= top:
                    continue
                full = tev + tail
                if tex_needed(c) != uses_tex(full):
                    continue
                made.setdefault(encode(normal(dict(c, tev=full, fogat=c['fogat'] | fogat))), 'tail' if tail else 'model')
        if len(set(made) - keys) >= LIMIT:
            break
    code = f'{game}/code.bin'
    groups = code_templates(open(code, 'rb').read()) if os.path.exists(code) else []
    for g in groups:
        family = [s for s in states if any(apply_template(e, s) == encode(s) for _, e in g)]
        for s in family:
            for _, e in g:
                made.setdefault(apply_template(e, s), 'code')
    lmats = layouts(lyt_blobs)
    for k in layout_states(lmats, states, LIMIT):
        made.setdefault(k, 'layout')
    new = sorted(set(made) - keys)
    if holdout:
        miss = {encode(normal(decode(k))) for k in held} - keys
        print(f'{len(new)} states made; of {len(miss)} console misses no run had, {len(miss & set(new))} predicted')
        return
    with open(f'{logs}/{OUT}', 'wb') as f:
        for k in new:
            f.write(struct.pack('<I', len(k)) + k)
    parts = {p: sum(1 for k in new if made[k] == p) for p in ('model', 'tail', 'code', 'layout')}
    print(f'{len(bases)} model combiner sets, {len(tails) - 1} learned tails, {sum(len(g) for g in groups)} code templates, {len(lmats)} layout materials, {len(contexts)} logged settings: '
          f'{len(new)} fragment states no log had ({parts["model"]} models, {parts["tail"]} models with a tail, {parts["code"]} code templates, {parts["layout"]} layouts)')


if __name__ == '__main__':
    main()
