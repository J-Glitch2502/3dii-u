#!/usr/bin/env python3
"""Every vertex program in a game's data, so no scene needs a test run to have reached it first.

Walks code.bin and the whole romfs, inside LZ10, LZ11 and Yaz0 streams and whatever they hold, for shader binaries (DVLB), and writes them as shader log records, which the shader
generator reads like any other log: one for each vertex program and each one that can draw an outline. Which output each register feeds comes from the program's own table, as a game
sets it up. With --lists and the shader generator's path, the programs a game loads with GPU command lists kept in its files go in too (Game Freak's model files: the generator
finds them, tools/shadergen_lists.cpp). dvlb_to_log.py <game dir> [--check] [--lists <shadergen> [--materials <stream>]]. Output: <game dir>/shader_logs/dvlb.bin,
dvlb.gsl with each geometry program whole (gs_record), and optional material records for tev_to_log.py. --check only compares with what runs have logged.
--geometry-only exports declared geometry entries to shader_logs/assets_geometry.gsl without fragment discovery.
"""
import bisect
import glob
from itertools import chain
import mmap
import os
import re
import struct
import subprocess
import sys
import tempfile

OUTLINE_SLOT = 0x80
SEMANTICS = {0: (0, 1, 2, 3), 1: (4, 5, 6, 7), 2: (8, 9, 10, 11), 3: (12, 13), 4: (16,), 5: (14, 15), 6: (22, 23), 8: (18, 19, 20)}
MAX_UNPACKED = 64 << 20
MAX_BINARY = 1 << 18
MAGIC = re.compile(rb'D.?V.?L.?B', re.DOTALL)


def lz(data, at):
    """An LZ10 or LZ11 stream at this place, under Nintendo's header or Level-5's (size << 3 | 1), or None if it isn't one."""
    if at < 0 or at + 8 > len(data):
        return None
    head = int.from_bytes(data[at:at + 4], 'little')
    if head & 7 == 1 and (head >> 3) >= 4:  # Level-5's first: 0x11 fits both, and a real LZ11 header that gets read this way fails on its size or its first copy
        out = unlz(data, at + 4, head >> 3, 0x10)
        if out is not None:
            return out
    kind = head & 255
    if kind not in (0x10, 0x11):
        return None
    size, p = head >> 8, at + 4
    if size == 0:
        size, p = int.from_bytes(data[p:p + 4], 'little'), p + 4
    return unlz(data, p, size, kind)


def unlz(data, p, size, kind):
    if not 4 <= size <= MAX_UNPACKED:
        return None
    out = bytearray()
    try:
        while len(out) < size:
            flags = data[p]
            p += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    b0 = data[p]
                    top = b0 >> 4
                    if kind == 0x10:
                        length, disp = top + 3, ((b0 & 15) << 8 | data[p + 1]) + 1
                        p += 2
                    elif top == 0:
                        length, disp = ((b0 & 15) << 4 | data[p + 1] >> 4) + 0x11, ((data[p + 1] & 15) << 8 | data[p + 2]) + 1
                        p += 3
                    elif top == 1:
                        length, disp = ((b0 & 15) << 12 | data[p + 1] << 4 | data[p + 2] >> 4) + 0x111, ((data[p + 2] & 15) << 8 | data[p + 3]) + 1
                        p += 4
                    else:
                        length, disp = top + 1, ((b0 & 15) << 8 | data[p + 1]) + 1
                        p += 2
                    if disp > len(out):
                        return None
                    for _ in range(length):
                        out.append(out[-disp])
                else:
                    out.append(data[p])
                    p += 1
    except IndexError:
        return None
    return bytes(out)


def yaz0(data):
    size = int.from_bytes(data[4:8], 'big')
    if not 4 <= size <= MAX_UNPACKED:
        return None
    out = bytearray()
    p = 16
    try:
        while len(out) < size:
            flags = data[p]
            p += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    out.append(data[p])
                    p += 1
                else:
                    b0, b1 = data[p], data[p + 1]
                    p += 2
                    disp = ((b0 & 15) << 8 | b1) + 1
                    length = b0 >> 4
                    if length == 0:
                        length = data[p] + 0x12
                        p += 1
                    else:
                        length += 2
                    if disp > len(out):
                        return None
                    for _ in range(length):
                        out.append(out[-disp])
    except IndexError:
        return None
    return bytes(out)


def unpacked(data, at):
    return yaz0(data[at:]) if data[at:at + 4] == b'Yaz0' else lz(data, at)


def romfs_files(data):
    """Where each file is, sorted, in a romfs with no hash layers."""
    head = struct.unpack_from('<10I', data, 0)
    if head[0] != 0x28:
        return []
    fmeta, flen, base = head[7], head[8], head[9]
    files = []
    p = 0
    while p + 32 <= flen:
        off, size = struct.unpack_from('<QQ', data, fmeta + p + 8)
        nlen = struct.unpack_from('<I', data, fmeta + p + 28)[0]
        files.append((base + off, size))
        p += 32 + ((nlen + 3) & ~3)
    return sorted(files)


def plain(data):
    """Shader binaries lying as they are in this memory."""
    p = 0
    while True:
        p = data.find(b'DVLB', p)
        if p < 0:
            return
        yield data[p:p + MAX_BINARY]
        p += 4


def readable(blob):
    try:
        parse(blob)
    except (ValueError, struct.error, IndexError):
        return False
    return True


def programs(game, romfs_name="romfs.bin", include_code=True):
    """Each shader binary in the game. The first time a compressed stream holds the magic it can't be a copy of anything, so it's there as literals, with at most one flag byte
    among them: shortcut: a stream that already had 3 of those letters in a row wouldn't show, which would need the whole romfs unpacked to rule out."""
    code = f'{game}/code.bin'
    if include_code and os.path.exists(code):
        yield from plain(open(code, 'rb').read())
    with open(os.path.join(game, romfs_name), 'rb') as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    files = romfs_files(data)
    starts = [f[0] for f in files]
    tried = set()
    for m in MAGIC.finditer(data):
        at = m.start()
        if m.group() == b'DVLB':
            blob = data[at:at + MAX_BINARY]
            if not readable(blob):  # the start of a stream, then: its header and one flag byte come just before
                blob = lz(data, at - 5)
            if blob and readable(blob):
                yield blob
                continue
        i = bisect.bisect_right(starts, at) - 1
        if i < 0 or i in tried or at >= files[i][0] + files[i][1]:
            continue
        tried.add(i)
        inner = unpacked(data, files[i][0])
        if inner:
            yield from plain(inner)


def material_records(game):
    """Shader selections in recognized tagged materials, independent of runs."""
    import tagged_materials
    for name in ('romfs.bin', 'update_romfs.bin'):
        path = os.path.join(game, name)
        if not os.path.isfile(path) or not os.path.getsize(path):
            continue
        with open(path, 'rb') as f:
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
                for at, size in romfs_files(data):
                    if data[at:at + 4] != tagged_materials.MAGIC:
                        continue
                    if size > 64 << 20 or at > len(data) or size > len(data) - at:
                        print('tagged material discovery skipped an oversized/truncated asset', file=sys.stderr)
                        continue
                    try:
                        yield from tagged_materials.shader_records(data[at:at + size], sys.modules[__name__])
                    except (ValueError, struct.error, IndexError) as e:
                        print(f'tagged material discovery incomplete: {e}', file=sys.stderr)


def parse(blob):
    n = struct.unpack_from('<I', blob, 4)[0]
    if not 1 <= n <= 256:
        raise ValueError('no program count')
    dvle = struct.unpack_from(f'<{n}I', blob, 8)
    dvlp = 8 + 4 * n
    if blob[dvlp:dvlp + 4] != b'DVLP':
        raise ValueError('no DVLP')
    code_at, nc, swz_at, ns = struct.unpack_from('<4I', blob, dvlp + 8)
    code = struct.unpack_from(f'<{nc}I', blob, dvlp + code_at)
    swz = [struct.unpack_from('<I', blob, dvlp + swz_at + 8 * i)[0] for i in range(ns)]
    entries = []
    for at in dvle:
        if blob[at:at + 4] != b'DVLE':
            raise ValueError('no DVLE')
        kind = blob[at + 6]
        entry = struct.unpack_from('<I', blob, at + 8)[0]
        out_at, out_n = struct.unpack_from('<II', blob, at + 40)
        outs = []
        for i in range(out_n):
            sem, reg, mask = struct.unpack_from('<HHB', blob, at + out_at + 8 * i)
            outs.append((sem, reg, mask))
        entries.append((kind, entry, outs))
    return code, swz, entries


def geometry_records(blobs):
    """Native-CPU candidates from declared DVLE geometry entries, without a run log.

    Keep the shared DVLP code memory: a geometry entry can be beyond word 512.
    The native translator still declines unsupported control flow, and the runtime
    matches instruction/swizzle hashes before using any generated function.
    """
    seen = set()
    for blob in blobs:
        try:
            code, swz, entries = parse(blob)
        except (ValueError, struct.error, IndexError):
            continue
        if not 0 < len(code) <= 4096 or len(swz) > 4096:
            continue
        for kind, entry, _outs in entries:
            if kind != 1 or not 0 <= entry < len(code):
                continue
            rec = (b'GS  ' + struct.pack('<III', entry, len(code), len(swz))
                   + struct.pack(f'<{len(code)}I', *code)
                   + struct.pack(f'<{len(swz)}I', *swz))
            if rec not in seen:
                seen.add(rec)
                yield rec


def discover_geometry(game):
    def blobs():
        for image in ('romfs.bin', 'update_romfs.bin'):
            if os.path.exists(os.path.join(game, image)):
                yield from programs(game, image, include_code=image == 'romfs.bin')
    records = list(geometry_records(blobs()))
    logs = os.path.join(game, 'shader_logs')
    os.makedirs(logs, exist_ok=True)
    path = os.path.join(logs, 'assets_geometry.gsl')
    # Replace only this derived file; preserve the user's recorded .gsl files.
    tmp = path + '.new'
    with open(tmp, 'wb') as f:
        f.write(b''.join(records))
    os.replace(tmp, path)
    print(f'{len(records)} geometry programs discovered in assets for native CPU translation')


def labels(blob):
    """DVLE debug labels: (stage, instruction offset, declared length, name).

    These include subroutines, not just drawable entry points. They are useful
    for asset analysis, but must not be turned into draw records without the
    material's uniforms and output mapping.
    """
    n = struct.unpack_from('<I', blob, 4)[0]
    if not 1 <= n <= 256:
        raise ValueError('no program count')
    for at in struct.unpack_from(f'<{n}I', blob, 8):
        if blob[at:at + 4] != b'DVLE':
            raise ValueError('no DVLE')
        table, count = struct.unpack_from('<II', blob, at + 32)
        symbols, size = struct.unpack_from('<II', blob, at + 56)
        if at + table + count * 16 > len(blob) or at + symbols + size > len(blob):
            raise ValueError('truncated DVLE labels')
        for i in range(count):
            _, start, length, offset = struct.unpack_from('<4I', blob, at + table + i * 16)
            if offset >= size:
                raise ValueError('label name outside symbol table')
            end = blob.find(b'\0', at + symbols + offset, at + symbols + size)
            if end < 0:
                raise ValueError('unterminated label name')
            yield blob[at + 6], start, length, blob[at + symbols + offset:end].decode('utf-8', errors='replace')


def slots_for(outs):
    # as the game's output registers would be set from this table: each semantic's parts, from the components that register writes
    s = [-1] * 24
    for sem, reg, mask in outs:
        comps = [c for c in range(4) if mask & (1 << c)]
        for slot, c in zip(SEMANTICS.get(sem, ()), comps):
            s[slot] = reg * 4 + c
    return s


def gs_record(entry, code, swz):
    """a geometry program as a .gsl file's "GS  " record (tools/pica_to_c.py): the whole code and swizzles, as its unit holds them, past the
    512 words a vertex shader's unit takes: Luigi's Mansion 2's geometry shader starts at word 505 of its binary's 1338"""
    out = b'GS  ' + struct.pack('<III', entry, len(code), len(swz))
    return out + struct.pack(f'<{len(code)}I', *code) + struct.pack(f'<{len(swz)}I', *swz)


def record(entry, code, swz, b, slots, integers=bytes(16)):
    out = b'VS  ' + struct.pack('<III', entry, len(code), len(swz))
    out += struct.pack(f'<{len(code)}I', *code) + struct.pack(f'<{len(swz)}I', *swz)
    out += bytes(b) + bytes(integers) + bytes(x & 255 for x in slots)
    return out


def log_spans(d):
    """Record boundaries in a mixed shader log, through the last complete record."""
    p = 0
    while p + 4 <= len(d):
        tag = d[p:p + 4]
        if tag in (b'VS  ', b'GS  '):
            if p + 16 > len(d):
                break
            nc, ns = struct.unpack_from('<II', d, p + 8)
            if max(nc, ns) > 4096:
                break
            end = p + 16 + (nc + ns) * 4 + (56 if tag == b'VS  ' else 0)
        else:
            q = p + 4 if tag in (b'FS  ', b'GSX ') else p
            if q + 4 > len(d):
                break
            end = q + 4 + struct.unpack_from('<I', d, q)[0]
            if tag != b'GSX ':
                tag = b'FS  '
        if end > len(d):
            break
        if tag == b'GSX ':
            if end - p < 8 + 44 + 88:
                break
            nc, ns, _, gc, gs = struct.unpack_from('<5I', d, p + 12)
            if max(nc, ns, gc, gs) > 4096 or end - p != 8 + 44 + (nc + ns + gc + gs) * 4 + 88:
                break
        yield tag, p, end
        p = end


def fragment_keys(d):
    for tag, p, end in log_spans(d):
        if tag == b'FS  ':
            yield d[p + (8 if d[p:p + 4] == b'FS  ' else 4):end]


def vertex_records(d):
    """(entry, code, swizzles, output map) of each vertex record among a log's records"""
    for tag, p, end in log_spans(d):
        if tag == b'VS  ':
            entry, nc, ns = struct.unpack_from('<III', d, p + 4)
            code = struct.unpack_from(f'<{nc}I', d, p + 16)
            swz = struct.unpack_from(f'<{ns}I', d, p + 16 + nc * 4)
            yield entry, code, swz, [x - 256 if x >= 128 else x for x in d[end - 24:end]]


def logged(game):
    for path in glob.glob(f'{game}/shader_logs/*.bin'):
        name = os.path.basename(path)
        if name == 'dvlb.bin' or name.startswith('other_') or name.endswith('_fs.bin'):
            continue
        yield from vertex_records(open(path, 'rb').read())


def listed(game, shadergen, archive_binaries=None, archive_materials=None):
    """The records of the vertex programs the game's files and its update load with GPU command lists, as the shader generator finds them.
    (A file the update replaces is looked through all the same: a program no draw uses costs a build a little time.)"""
    images = [p for p in (f'{game}/romfs.bin', f'{game}/update_romfs.bin') if os.path.exists(p)]
    if not images:
        return b''
    with tempfile.TemporaryDirectory() as tmp:
        archive_args = ['--dvlb', archive_binaries] if archive_binaries else []
        if archive_materials:
            archive_args += ['--materials', archive_materials]
        r = subprocess.run([os.path.abspath(shadergen), '--lists', tmp + '/lists.bin', *archive_args, *images],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        if r.returncode:
            sys.exit(f'Shader command-list discovery failed: {r.stdout[-200:].strip()}')
        with open(tmp + '/lists.bin', 'rb') as f:
            return f.read()

def framed_records(path, limit):
    with open(path, 'rb') as f:
        while header := f.read(4):
            size = int.from_bytes(header, 'little')
            if len(header) != 4 or not 4 <= size <= limit:
                raise ValueError('Invalid shader discovery record size')
            data = f.read(size)
            if len(data) != size:
                raise ValueError('Truncated shader discovery record')
            yield data


def start_of(entry, code):
    # up to its first END: a shorter program leaves the end of the one before it in the log's copy of the shader memory
    words = code[entry:entry + 8]
    for i, w in enumerate(words):
        if w >> 26 == 0x22:
            words = words[:i + 1]
            break
    return entry, tuple(words)


def write_records(game, check, lists=b'', archived=()):
    out = gs_out = b''
    table = {}
    done = set()
    binaries = kept = geometry = 0
    for blob in chain(programs(game), archived):
        try:
            code, swz, entries = parse(blob)
        except (ValueError, struct.error, IndexError):
            continue
        binaries += 1
        vs = code[:512]
        for kind, entry, outs in entries:
            gkey = (entry, code, tuple(swz))
            if kind == 1 and entry < len(code) and gkey not in done:
                done.add(gkey)
                gs_out += gs_record(entry, code, swz)
                geometry += 1
            if kind != 0 or entry >= len(vs):
                continue
            slots = slots_for(outs)
            key = (entry, tuple(vs), tuple(swz), tuple(slots))
            if key in done:
                continue
            done.add(key)
            kept += 1
            table.setdefault(start_of(entry, vs), slots)
            out += record(entry, vs, swz, bytes(16), slots)
            if any(sem == 9 for sem, _, _ in outs):  # an output for the geometry shader alone: this one can draw an outline
                out += record(entry, vs, swz, bytes(16), [OUTLINE_SLOT] * 24)
    loaded = 0
    for entry, code, _swz, slots in vertex_records(lists):
        loaded += 1
        table.setdefault(start_of(entry, code), slots)
    out += lists
    discovered = 0
    for material in material_records(game):
        for entry, code, _swz, slots in vertex_records(material):
            table.setdefault(start_of(entry, code), slots)
        out += material
        discovered += 1
    agree = differ = unknown = 0
    missing = set()
    for entry, code, _swz, got in logged(game):
        if got[0] == -128:
            continue
        want = table.get(start_of(entry, code))
        if want is None:
            unknown += 1
            missing.add((entry, code[entry]))
        elif want == got:
            agree += 1
        else:
            differ += 1
    print(f'{os.path.basename(game.rstrip("/"))}: {binaries} shader binaries, {kept} vertex programs, {loaded} from command lists, {discovered} material selections; logged records: {agree} agree, {differ} other output map, {unknown} not found ({len(missing)} programs)')
    if not check:
        with open(f'{game}/shader_logs/dvlb.bin', 'wb') as f:
            f.write(out)
        with open(f'{game}/shader_logs/dvlb.gsl', 'wb') as f:  # even empty: it says this dvlb.bin was made since (make_app.py)
            f.write(gs_out)
        print(f'{len(out) >> 10} KB written, and {geometry} geometry programs')


def main():
    game = sys.argv[1]
    check = '--check' in sys.argv
    if '--geometry-only' in sys.argv:
        discover_geometry(game)
        return
    if '--lists' not in sys.argv[:-1]:
        write_records(game, check)
        return
    with tempfile.TemporaryDirectory() as tmp:
        archived = os.path.join(tmp, 'compressed-dvlb.bin')
        materials = sys.argv[sys.argv.index('--materials') + 1] if not check and '--materials' in sys.argv[:-1] else None
        lists = listed(game, sys.argv[sys.argv.index('--lists') + 1], archived, materials)
        write_records(game, check, lists, framed_records(archived, MAX_BINARY))


if __name__ == '__main__':
    main()
