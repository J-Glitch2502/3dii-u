"""Shader logs without the games' code, for the standalone app.

A run's shader log (port/pc/vs_log.cpp, or the console's shader_misses.bin) holds "VS  " records, each with the GPU's whole vertex shader memory,
which is the game's own code, and fragment records (render state keys), which aren't. The app ships the fragment records as they are and each
vertex record as a fingerprint of the program of the game's DVLB its memory starts with (tools/dvlb_to_log.py finds those in a dump), in a .vsf
file beside the log. The build finds the same programs in the user's own dump and puts the records back together: the same entry, uniforms and
output map, so the shader generator makes the same shader. A vertex shader the console lacks is drawn the CPU way, so a record left out costs
speed in that scene, never the picture.

A memory no DVLB program starts goes as a recipe instead (.vsr): where its words lie in the game's own files, as runs, so the build puts it back
together from the user's dump and checks it against the fingerprint. The Mario & Luigi engine keeps a shader as plain words in its code.bin and
loads a romfs program after it (115 records the app used to leave out, Nov 9), and Zelda keeps some in an SPBD3 inside a compressed archive. Only
a long run may lie anywhere; a short one must lie near a long one, so a recipe can't spell code out of coincidences, and a memory whose words
aren't in the dump (another game's, in a log that collects every game) gets none.

A game confirmed finished on a console keeps the list of fragment states that build compiled ahead (confirmed.bin, confirm below). A build
that takes the game's logs compiles the list whatever the others hold: the states it guesses beyond the logs are picked up to a limit, in
an order one more logged state changes, so without the list a later build could lose shaders the finished one had."""
import functools
import hashlib
import importlib.util
import mmap
import os
import struct

TAIL = 56  # bool uniforms, int uniforms, output map
VSF = 4 + 4 + 16 + TAIL  # b'VSF ', entry, fingerprint, tail
VSR = 4 + 8 + 16 + TAIL  # b'VSR ', entry, code and swizzle piece counts, fingerprint, tail; then the pieces
PIECE = struct.Struct('<BBHQ')  # source, stride (4, or 8 for a DVLB's swizzle table), words, byte offset (UNPACKED: file index << 32 | offset)
SOURCES = ('code.bin', 'romfs.bin')  # where a recipe's pieces lie in a game's folder, as this repository and the build both extract them
UNPACKED = len(SOURCES)  # or in a romfs file's unpacked contents (Yaz0, LZ10, LZ11)
LONG = 3  # words in a run that may lie anywhere in a file
NEAR = 64 << 10  # how close to a long run a shorter one must lie
CONFIRMED = 'confirmed.bin'  # a finished game's fragment states, in its shader_logs (confirm)
ROOT = os.environ.get('THREEDII_ROOT') or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # as pipeline.py finds it


# ---------------------------------------------------------------- logs and fingerprints
def records(d):
    """('vs', entry, code, swizzles, tail) or ('fs', raw bytes) for each record of a log, as tools/shadergen_main.cpp reads them"""
    p = 0
    while p + 8 <= len(d):
        if d[p:p + 4] == b'VS  ':
            entry, nc, ns = struct.unpack_from('<III', d, p + 4)
            q = p + 16 + (nc + ns) * 4
            yield 'vs', entry, d[p + 16:p + 16 + nc * 4], d[p + 16 + nc * 4:q], d[q:q + TAIL]
            p = q + TAIL
        else:
            q = p + 4 if d[p:p + 4] == b'FS  ' else p
            if q + 4 > len(d):
                break
            end = q + 4 + struct.unpack_from('<I', d, q)[0]
            yield 'fs', d[p:end]
            p = end


def programs(dvlb):
    """the game's vertex programs as (code, swizzles), from dvlb_to_log.py's dvlb.bin"""
    with open(dvlb, 'rb') as f:
        return {(r[2], r[3]) for r in records(f.read()) if r[0] == 'vs'}


def fingerprint(code, swz):
    return hashlib.sha256(code + b'|' + swz).digest()[:16]


def vs_record(entry, code, swz, tail):
    return b'VS  ' + struct.pack('<III', entry, len(code) // 4, len(swz) // 4) + code + swz + tail


# ---------------------------------------------------------------- the game's own files
@functools.cache
def _dvlb_tool():
    """tools/dvlb_to_log.py, for its romfs listing and its unpackers"""
    spec = importlib.util.spec_from_file_location('dvlb_to_log', os.path.join(ROOT, 'tools', 'dvlb_to_log.py'))
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    return tool


class Sources(list):
    """A game folder's files in a recipe's numbering: code.bin and romfs.bin (b'' for one it lacks), then its romfs files unpacked (UNPACKED),
    each unpacked when a recipe first needs it."""
    def __init__(self, files):
        super().__init__(files)
        self._files, self._unpacked = None, {}

    def romfs_files(self):
        """(offset, size) of each file in the romfs, by index (tools/dvlb_to_log.py's order)"""
        if self._files is None:
            self._files = _dvlb_tool().romfs_files(self[1]) if self[1] else []
        return self._files

    def unpacked(self, index, keep=True):
        """the unpacked contents of the romfs file of this index, b'' if it isn't a compressed one"""
        if index in self._unpacked:
            return self._unpacked[index]
        data = b''
        if index < len(self.romfs_files()):
            at, size = self.romfs_files()[index]
            blob, tool = self[1][at:at + size], _dvlb_tool()
            data = (tool.yaz0(blob) if blob[:4] == b'Yaz0' else tool.lz(blob, 0)) or b''
        if keep:
            self._unpacked[index] = data
        return data

    def holding(self, probes):
        """the indices of the romfs files whose unpacked contents hold any of these byte strings: each is unpacked once, and only those are kept"""
        found = []
        for i in range(len(self.romfs_files())):
            data = self.unpacked(i, keep=False)
            if data and any(p in data for p in probes):
                self._unpacked[i] = data
                found.append(i)
        return found


def sources(game_dir):
    """a game folder's files recipes take their pieces from, or None if it has neither"""
    files = []
    for name in SOURCES:
        p = os.path.join(game_dir, name)
        if os.path.isfile(p) and os.path.getsize(p):
            with open(p, 'rb') as f:
                files.append(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
        else:
            files.append(b'')
    return Sources(files) if any(files) else None


def close(srcs):
    """a game's files closed: Windows won't delete a file while a map of it is open"""
    for f in srcs or ():
        if isinstance(f, mmap.mmap):
            f.close()


# ---------------------------------------------------------------- recipes
def _words(b):
    return [b[i:i + 4] for i in range(0, len(b), 4)]


def _run_near(buf, words, i, windows, strides):
    """the longest run of words[i:] in buf that starts at an aligned place in one of the windows, its words stride bytes apart:
    (words, offset, stride)"""
    best = (0, -1, 4)
    for lo, hi in windows:
        at = buf.find(words[i], lo, hi)
        while at >= 0:
            if at % 4 == 0:
                for st in strides:
                    k = 1
                    while i + k < len(words) and buf[at + st * k:at + st * k + 4] == words[i + k]:
                        k += 1
                    if k > best[0]:
                        best = (k, at, st)
            at = buf.find(words[i], at + 1, hi)
    return best


def _run_anywhere(buf, words, i):
    """the longest run of words[i:], at least LONG of them, lying whole and aligned anywhere in buf: (words, offset), or (0, -1)"""
    def where(k):
        key = b''.join(words[i:i + k])
        at = buf.find(key)
        while at >= 0 and at % 4:
            at = buf.find(key, at + 1)
        return at
    if len(words) - i < LONG or (found := where(LONG)) < 0:  # one pass over the file when there's nothing
        return 0, -1
    lo, hi = LONG, LONG
    while hi < len(words) - i:  # a run that's there has every shorter start there too: double, then halve
        hi = min(2 * hi, len(words) - i)
        at = where(hi)
        if at < 0:
            break
        lo, found = hi, at
    else:
        return lo, found
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        at = where(mid)
        if at >= 0:
            lo, found = mid, at
        else:
            hi = mid
    return lo, found


def _cover(words, bufs, strides, windows, most, far_below):
    """Pieces (source, stride, words, offset) that lay words out of bufs, at most `most` of them, or None. First the runs long enough to say where
    they're from: in a window, or else anywhere and contiguous, looked for when the longest run in the windows is shorter than far_below (each
    look is a pass over the file). Then the words between them, from near those runs."""
    def near(i):
        best = (0, 0, 4, -1)  # words, source, stride, offset
        for s, buf in enumerate(bufs):
            if buf:
                k, at, st = _run_near(buf, words, i, windows[s], strides)
                if k > best[0]:
                    best = (k, s, st, at)
        return best

    long_runs, i, short = {}, 0, 0
    while i < len(words):
        best = near(i)
        if best[0] < far_below:
            for s, buf in enumerate(bufs):
                if buf:
                    k, at = _run_anywhere(buf, words, i)
                    if k > best[0]:
                        best = (k, s, 4, at)
        k, s, st, at = best
        if k >= LONG:
            long_runs[i] = (s, st, k, at)
            windows[s].append((max(0, at - NEAR), at + st * k + NEAR))
            i += k
            continue
        short += 1
        if short > most:  # words that aren't in this dump: another game's, in a log that collects every game
            return None
        i += 1
    pieces, i = [], 0
    while i < len(words):
        if i in long_runs:
            piece = long_runs[i]
        else:
            k, s, st, at = near(i)
            if not k:
                return None
            piece = (s, st, min([k] + [j - i for j in long_runs if j > i]), at)
        pieces.append(piece)
        if len(pieces) > most:
            return None
        i += piece[2]
    return pieces


def _assemble(pieces, srcs, most):
    """the bytes a recipe's pieces lay out, at most `most` words, or None if a piece is out of bounds"""
    out = bytearray()
    for s, st, k, at in pieces:
        if s == UNPACKED:
            buf, at = srcs.unpacked(at >> 32), at & 0xFFFFFFFF
        elif s < UNPACKED:
            buf = srcs[s]
        else:
            return None
        if st not in (4, 8) or not k or at + st * (k - 1) + 4 > len(buf) or len(out) // 4 + k > most:
            return None
        for j in range(k):
            out += buf[at + st * j:at + st * j + 4]
    return bytes(out)


def _probes(code):
    """a few runs of a memory's code, LONG words each, to find which unpacked files hold it"""
    cw = _words(code)
    return [b''.join(cw[i:i + LONG]) for i in range(0, len(cw) - LONG + 1, max(1, len(cw) // 8))]


def _layout(code, swz, bufs):
    """(code pieces, swizzle pieces) over these buffers, or None. Swizzles are looked for near the code first, where a program keeps its table:
    plain words, or every other word in a DVLB."""
    windows = [[] for _ in bufs]
    cw, sw = _words(code), _words(swz)
    pc = _cover(cw, bufs, (4,), windows, len(cw) // 16 + 4, far_below=LONG)
    ps = _cover(sw, bufs, (4, 8), windows, len(sw) // 4 + 4, far_below=1) if pc else None
    return (pc, ps) if pc and ps else None


def recipe(code, swz, srcs, unpacked=()):
    """(code pieces, swizzle pieces) that put this shader memory together from a game's own files, or None: from code.bin and the romfs as they
    lie, or else from these romfs files unpacked (Sources.holding finds them)"""
    got = _layout(code, swz, list(srcs))
    if not got and unpacked:
        probes = _probes(code)
        files = [i for i in unpacked if any(p in srcs.unpacked(i) for p in probes)]
        got = _layout(code, swz, list(srcs) + [srcs.unpacked(i) for i in files]) if files else None
        if got:  # a piece from an unpacked file names it by its index in the romfs
            got = tuple([p if p[0] < UNPACKED else (UNPACKED, p[1], p[2], files[p[0] - UNPACKED] << 32 | p[3]) for p in part] for part in got)
    if got and (_assemble(got[0], srcs, 512) != code or _assemble(got[1], srcs, 128) != swz):
        return None
    return got


def recipes(d):
    """(entry, fingerprint, tail, code pieces, swizzle pieces) for each record of a .vsr file, as long as they're whole"""
    p = 0
    while p + VSR <= len(d) and d[p:p + 4] == b'VSR ':
        entry, nc, ns = struct.unpack_from('<IHH', d, p + 4)
        q = p + VSR + PIECE.size * (nc + ns)
        if q > len(d):
            return
        pieces = [PIECE.unpack_from(d, p + VSR + PIECE.size * j) for j in range(nc + ns)]
        yield entry, d[p + 12:p + 28], d[p + 28:p + VSR], pieces[:nc], pieces[nc:]
        p = q


# ---------------------------------------------------------------- what the app ships, and what a build makes of it
def split(d, progs, srcs=None, cache=None, unpacked=()):
    """(fragment records as they are, vertex records as fingerprints (.vsf), as recipes (.vsr), and the vertex records left out, as (code,
    swizzles)). A record whose memory starts with none of the game's programs goes as a recipe if the game's own files (srcs) hold its words.
    cache keeps each memory's recipe across a game's logs."""
    cache = {} if cache is None else cache
    frag, vsf, vsr, left = bytearray(), bytearray(), bytearray(), []
    for r in records(d):
        if r[0] == 'fs':
            frag += r[1]
            continue
        _, entry, code, swz, tail = r
        found = [p for p in progs if entry < len(p[0]) // 4 and code.startswith(p[0]) and swz.startswith(p[1])]
        if found:
            vsf += b'VSF ' + struct.pack('<I', entry) + fingerprint(*max(found, key=lambda p: len(p[0]))) + tail
            continue
        if srcs and (code, swz) not in cache:
            cache[(code, swz)] = recipe(code, swz, srcs, unpacked)
        how = cache.get((code, swz)) if srcs else None
        if how:
            pc, ps = how
            vsr += b'VSR ' + struct.pack('<IHH', entry, len(pc), len(ps)) + fingerprint(code, swz) + tail
            vsr += b''.join(PIECE.pack(*p) for p in pc + ps)
        else:
            left.append((code, swz))
    return bytes(frag), bytes(vsf), bytes(vsr), left


def _restore_fingerprints(logs):
    """each .vsf file back into "VS  " records (<name>_vsf.bin), with the programs of this dump's dvlb.bin: (made, left out)"""
    names = [f for f in os.listdir(logs) if f.endswith('.vsf')]
    dvlb = os.path.join(logs, 'dvlb.bin')
    found = {fingerprint(c, s): (c, s) for c, s in programs(dvlb)} if names and os.path.isfile(dvlb) else {}
    made = left = 0
    for name in names:
        with open(os.path.join(logs, name), 'rb') as f:
            d = f.read()
        out = bytearray()
        for p in range(0, len(d) - VSF + 1, VSF):
            if d[p:p + 4] != b'VSF ':
                break
            prog = found.get(d[p + 8:p + 24])
            if prog is None:
                left += 1
                continue
            out += vs_record(struct.unpack_from('<I', d, p + 4)[0], *prog, d[p + 24:p + VSF])
            made += 1
        with open(os.path.join(logs, name[:-4] + '_vsf.bin'), 'wb') as f:
            f.write(out)
        os.remove(os.path.join(logs, name))
    return made, left


def _restore_recipes(logs):
    """each .vsr file back into "VS  " records (<name>_vsr.bin), from this dump's code.bin and romfs.bin in the game folder: (made, left out)"""
    names = [f for f in os.listdir(logs) if f.endswith('.vsr')]
    srcs = sources(os.path.dirname(os.path.abspath(logs))) if names else None
    made = left = 0
    for name in names:
        with open(os.path.join(logs, name), 'rb') as f:
            d = f.read()
        out = bytearray()
        for entry, fp, tail, pc, ps in recipes(d):
            code, swz = (_assemble(pc, srcs, 512), _assemble(ps, srcs, 128)) if srcs else (None, None)
            if code is None or swz is None or fingerprint(code, swz) != fp:  # another version of the game, or an update's code
                left += 1
                continue
            out += vs_record(entry, code, swz, tail)
            made += 1
        with open(os.path.join(logs, name[:-4] + '_vsr.bin'), 'wb') as f:
            f.write(out)
        os.remove(os.path.join(logs, name))
    close(srcs)
    return made, left


def restore(logs):
    """A game's shipped logs back into "VS  " records from this dump: fingerprints with its dvlb.bin's programs, recipes with its code.bin and
    romfs.bin. A record this dump can't make (another version of the game, an update's code) is left out. Returns (records made, left out)."""
    made, left = _restore_fingerprints(logs)
    more, less = _restore_recipes(logs)
    return made + more, left + less


def write_recipes(game_dir, logs):
    """recipes.vsr in a game's shader_logs, for the records no dvlb.bin program starts, from a dump's code.bin and romfs.bin in game_dir: for the
    games this repository keeps no dump of (games/g_*). make_app.py ships it. Returns (recipes, records left out)."""
    srcs = sources(game_dir)
    if not srcs:
        raise SystemExit(f'{game_dir} has no code.bin or romfs.bin')
    dvlb = os.path.join(logs, 'dvlb.bin')
    progs = programs(dvlb) if os.path.isfile(dvlb) else set()
    logged = [f for f in sorted(os.listdir(logs)) if f.endswith('.bin') and f != 'dvlb.bin' and not f.startswith('other_')]

    def each_split(**kw):
        for f in logged:
            with open(os.path.join(logs, f), 'rb') as h:
                yield split(h.read(), progs, srcs, **kw)

    missed = {m for *_, missing in each_split(cache={}) for m in missing}  # from the files as they lie first
    unpacked = srcs.holding([p for code, _ in missed for p in _probes(code)]) if missed else []  # then the romfs files that hold the rest
    out, left = bytearray(), 0
    for _, _, vsr, missing in each_split(cache={}, unpacked=unpacked):
        out += vsr
        left += len(missing)
    close(srcs)
    path = os.path.join(logs, 'recipes.vsr')
    if out:
        with open(path, 'wb') as h:
            h.write(out)
    elif os.path.exists(path):
        os.remove(path)
    return sum(1 for _ in recipes(bytes(out))), left


def confirm(build_logs, game_logs):
    """confirmed.bin in a game's shader_logs: every fragment state the build the game was confirmed finished with compiled ahead, read from that
    build's own shader_logs folder in the order the shader generator met them (tools/shadergen_main.cpp: an earlier list first, then the folder
    as it lists), so a build with the same logs numbers its shaders the same. Later builds compile the list first, and no tool learns from it:
    it holds earlier guesses, not what runs met. Returns how many states it holds."""
    if not os.path.isfile(os.path.join(build_logs, 'variants.bin')):  # what a build's shader stage leaves (prepare_logs in tools/win/build3ds.py)
        raise SystemExit(f'{build_logs} is not a finished build\'s shader_logs: a list from it would lack what the build guessed')
    names = [f for f in os.listdir(build_logs) if f.endswith('.bin')]
    names.sort(key=lambda f: f != CONFIRMED)  # a stable sort: the rest stay as the folder lists them
    seen, out = set(), bytearray()
    for name in names:
        with open(os.path.join(build_logs, name), 'rb') as f:
            for kind, raw, *_ in records(f.read()):
                if kind != 'fs':
                    continue
                key = raw[4:] if raw[:4] == b'FS  ' else raw  # its length and the key, without a miss list's tag
                if len(key) != 4 + struct.unpack_from('<I', key)[0]:
                    continue  # the last record of a log cut short as it was written: in one file, it would run into the next record
                if key not in seen:
                    seen.add(key)
                    out += key
    if not seen:
        raise SystemExit(f'{build_logs} holds no fragment states: is it a build\'s shader_logs folder?')
    with open(os.path.join(game_logs, CONFIRMED), 'wb') as f:
        f.write(out)
    return len(seen)


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 4 and sys.argv[1] == '--recipes':
        made, left = write_recipes(sys.argv[2], sys.argv[3])
        print(f'{made} vertex records as recipes, {left} left out')
    elif len(sys.argv) == 4 and sys.argv[1] == '--confirm':
        print(f'{confirm(sys.argv[2], sys.argv[3])} fragment states confirmed')
    else:
        raise SystemExit('shaderlogs.py --recipes <folder with the game\'s code.bin and romfs.bin> <its games/<slug>/shader_logs>\n'
                         'shaderlogs.py --confirm <the shader_logs of the build confirmed finished> <the game\'s games/<slug>/shader_logs>')
