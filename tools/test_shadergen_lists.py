"""Self-check for shadergen --lists (tools/shadergen_lists.cpp): a romfs made here, with an archive whose files hold command lists made up
word by word, gives exactly the records draws after those lists would log. The lists end in each way one can (its jump, the next program's
code transfer, words that are no command), write registers in parts and in groups, and lie packed (both LZ11 headers, every form of copy)
and unpacked; a list that sets no entry point, one that leaves an output's map out and a second copy of a list give nothing. Then
tools/dvlb_to_log.py, given the generator, puts the same records into a game's dvlb.bin. python tools/test_shadergen_lists.py <shadergen>"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from check_ports import gsx_misses
from dvlb_to_log import MAX_UNPACKED, framed_records, vertex_records
from fs_variants import keys
from gsx_to_gsl import gs_records
from tev_to_log import TEV_REGS, encode, materials, records
from win.play_pc import merge


def cmd(reg, *values, mask=0xF, group=False):
    """one command: its first value and its header, then the other values, padded to whole pairs of words"""
    words = [values[0], reg | mask << 16 | (len(values) - 1) << 20 | group << 31, *values[1:]]
    return struct.pack(f'<{len(words)}I', *words) + bytes(4 * (len(words) & 1))


def upload(code, swz, split):
    """a vertex program's code, in two commands, and its swizzles"""
    return cmd(0x2CB, 0) + cmd(0x2CC, *code[:split]) + cmd(0x2CC, *code[split:]) + cmd(0x2BF, 1) + cmd(0x2D5, 0) + cmd(0x2D6, *swz)


def record(entry, code, swz, bools, ints, slots):
    """what a run logs for a draw: tools/shadergen_main.cpp's read_log reads it"""
    full = [slots.get(i, -1) & 255 for i in range(24)]
    return (b'VS  ' + struct.pack('<III', entry, len(code), len(swz)) + struct.pack(f'<{len(code)}I', *code) + struct.pack(f'<{len(swz)}I', *swz)
            + bytes(bools >> i & 1 for i in range(16)) + struct.pack('<4I', *ints) + bytes(full))


def lz11(data, long_header=False):
    """(an LZ11 stream of data, the kinds of copy it uses): each copy from the last place its first three bytes were"""
    out = bytearray(b'\x11' + (bytes(3) + struct.pack('<I', len(data)) if long_header else len(data).to_bytes(3, 'little')))
    p, kinds = 0, set()
    while p < len(data):
        flags, items = 0, bytearray()
        for bit in range(8):
            if p >= len(data):
                break
            at = data.rfind(data[p:p + 3], max(0, p - 0x1000), p + 2) if p + 3 <= len(data) else -1
            n = 0
            while at >= 0 and p + n < len(data) and n < 0x1000 and data[p + n] == data[at + n]:
                n += 1
            if n < 3:
                items.append(data[p])
                p += 1
                continue
            flags |= 0x80 >> bit
            back = p - at - 1
            if n <= 0x10:
                items += bytes([(n - 1) << 4 | back >> 8, back & 255])
            elif n <= 0x110:
                items += bytes([(n - 0x11) >> 4, ((n - 0x11) & 15) << 4 | back >> 8, back & 255])
            else:
                items += bytes([0x10 | (n - 0x111) >> 12, (n - 0x111) >> 4 & 255, ((n - 0x111) & 15) << 4 | back >> 8, back & 255])
            kinds.add(2 if n <= 0x10 else 3 if n <= 0x110 else 4)
            p += n
        out += bytes([flags]) + items
    return bytes(out), kinds


def garc(entries):
    """a GARC archive of these entries, each a list of (bit, file)"""
    fato, fatb, data = b'', b'', b''
    for files in entries:
        fato += struct.pack('<I', len(fatb))
        rows = b''
        for _, blob in files:
            start = len(data)
            data += blob + bytes(-len(blob) % 4)
            rows += struct.pack('<III', start, len(data), len(blob))
        fatb += struct.pack('<I', sum(1 << bit for bit, _ in files)) + rows
    tables = b'OTAF' + struct.pack('<IHH', 12 + len(fato), len(entries), 0xFFFF) + fato + b'BTAF' + struct.pack('<II', 12 + len(fatb), len(entries)) + fatb
    at = 0x24 + len(tables)
    return b'CRAG' + struct.pack('<IHHIIIIII', 0x24, 0xFEFF, 0x0600, 4, at, at + len(data), 0, 0, 4) + tables + data


def romfs(files):
    """a romfs with no hash layers: its header, the file table, the files"""
    table, data = b'', b''
    for name, blob in files:
        name = name.encode('utf-16-le')
        data += bytes(-len(data) % 16)
        table += struct.pack('<IIQQII', 0, 0xFFFFFFFF, len(data), len(blob), 0xFFFFFFFF, len(name)) + name + bytes(-len(name) % 4)
        data += blob
    base = (0x28 + len(table) + 15) & ~15
    return (struct.pack('<10I', 0x28, 0, 0, 0, 0, 0, 0, 0x28, len(table), base) + table).ljust(base, b'\0') + data


def shader_binary(code, swizzles):
    instructions = struct.pack(f'<{len(code)}I', *code)
    table = b''.join(struct.pack('<II', word, 0) for word in swizzles)
    program = struct.pack('<4s4x4I', b'DVLP', 24, len(code), 24 + len(instructions), len(swizzles))
    program += instructions + table
    entry = bytearray(56)
    entry[:4] = b'DVLE'
    struct.pack_into('<IIHHB', entry, 40, 48, 1, 0, 0, 0xF)
    return struct.pack('<4sII', b'DVLB', 1, 12 + len(program)) + program + entry


def crilayla(data):
    """A CRILAYLA stream with literals and a three-byte back-reference."""
    assert len(data) > 256
    data += b'ABCABC'
    body = data[256:][::-1]
    bits = ''.join(f'{byte:09b}' for byte in body[:3]) + '1' + '0' * 15
    bits += ''.join(f'{byte:09b}' for byte in body[6:])
    bits += '0' * (-len(bits) % 8)
    packed = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))[::-1]
    return b'CRILAYLA' + struct.pack('<II', len(data) - 256, len(packed)) + packed + data[:256]


FILLER = cmd(0x229, 0, mask=1) + cmd(0x200, *[0xEAD0FEAD] * 150, mask=0) + cmd(0x244, 0, mask=1)  # what a list has before its program: no part of it

# A: bools written whole and then one byte of them, the four ints and the output map as groups, outputs 0, 1 and 3; ends in its jump
code_a, swz_a = [0x4E000000 + 0x101 * i for i in range(40)], [0x8DA14C1F, 0x0000036F, 0x55, 0x66, 0x77]
ints_a = [0x00010203, 0x04050607, 0, 0x7F]
list_a = (upload(code_a, swz_a, 26) + cmd(0x2C0, 0x5D, 1, 2, 3) + cmd(0x2B0, 0x7FFF0005) + cmd(0x2B0, 0x12348134, mask=2) + cmd(0x2B1, *ints_a, group=True)
          + cmd(0x2BA, 0x7FFF0007) + cmd(0x2BD, 0xB) + cmd(0x04F, 3) + cmd(0x050, 0x03020100, 0x1F1F0D0C, 0x17161F08, 0x0B0A0908, group=True)
          + cmd(0x23D, 1) + cmd(0x2BA, 0x7FFF0001) + cmd(0x2BD, 0xFF))
want_a = record(7, code_a, swz_a, 0x8105, ints_a, {0: 0, 1: 1, 2: 2, 3: 3, 12: 4, 13: 5, 8: 12, 22: 14, 23: 15})

# B: zeros at the end of its code and swizzles, more outputs mapped than its mask enables; ends where C's code transfer begins
code_b, swz_b = [0x90000000 + i for i in range(12)] + [0x88000000], [0x11, 0x22]
list_b = (upload(code_b + [0, 0], swz_b + [0], 8) + cmd(0x2B0, 0x7FFF0000) + cmd(0x2BA, 0x7FFF0003) + cmd(0x2BD, 0x5) + cmd(0x04F, 3)
          + cmd(0x050, 0x03020100) + cmd(0x051, 0x07060504) + cmd(0x052, 0x0B0A0908))
want_b = record(3, code_b, swz_b, 0, [0] * 4, {0: 0, 1: 1, 2: 2, 3: 3, 4: 8, 5: 9, 6: 10, 7: 11})

code_c, swz_c = [0xA0000000 + 3 * i for i in range(20)], [7, 8, 9, 10]
ints_c = [0x01020304, 0, 0, 0xFF000000]
list_c = (upload(code_c, swz_c, 5) + cmd(0x2B0, 0x7FFF8000) + cmd(0x2B1, ints_c[0]) + cmd(0x2B4, ints_c[3]) + cmd(0x2BA, 0x7FFF0000) + cmd(0x2BD, 1)
          + cmd(0x04F, 1) + cmd(0x050, 0x12111000) + cmd(0x23D, 1))
want_c = record(0, code_c, swz_c, 0x8000, ints_c, {0: 0, 16: 1, 17: 2, 18: 3})

# D sets no entry point: no vertex program a draw could run
list_d = upload([0xD0000000 + i for i in range(6)], [1, 2], 3) + cmd(0x2B0, 0x7FFF0000) + cmd(0x2BD, 1) + cmd(0x04F, 1) + cmd(0x050, 0x03020100) + cmd(0x23D, 1)

# F says two outputs and maps one
list_f = upload([0xF0000000 + i for i in range(6)], [3], 3) + cmd(0x2BA, 0x7FFF0000) + cmd(0x2BD, 3) + cmd(0x04F, 2) + cmd(0x050, 0x03020100) + cmd(0x23D, 1)

# E ends at words that are no command, and what comes after isn't its own
code_e, swz_e = [0xE0000000 + i for i in range(9)], [0xABCDEF01]
list_e = (upload(code_e, swz_e, 4) + cmd(0x2BA, 0x7FFF0008) + cmd(0x2BD, 3) + cmd(0x04F, 2) + cmd(0x050, 0x03020100, 0x1F1F1F04, group=True)
          + struct.pack('<II', 0, 0x70000000) + cmd(0x2BA, 0x7FFF0002))
want_e = record(8, code_e, swz_e, 0, [0] * 4, {0: 0, 1: 1, 2: 2, 3: 3, 4: 4})

packed_a, kinds_a = lz11(b'model\0\0\0' + FILLER + list_a + bytes(40) + list_a[:64])
packed_e, kinds_e = lz11(FILLER + list_e, long_header=True)
assert kinds_a == {2, 3, 4}, f'the packed file should hold every kind of copy, not {kinds_a}'
archive = garc([[(0, packed_a)], [(0, bytes(12) + list_b + list_c), (2, b'junk' + list_d + list_f + FILLER + list_a)], [(0, packed_e)]])
image = romfs([('readme.txt', b'not an archive'), ('a/0/0', archive), ('empty', b'')])

with tempfile.TemporaryDirectory() as tmp:
    rom, other, out = os.path.join(tmp, 'romfs.bin'), os.path.join(tmp, 'other.bin'), os.path.join(tmp, 'lists.bin')
    with open(rom, 'wb') as f:
        f.write(image)
    with open(other, 'wb') as f:
        f.write(b'no romfs at all')
    for images in ([rom], [rom, other, rom]):  # the second time with a file that is no romfs, and every list again
        r = subprocess.run([sys.argv[1], '--lists', out, *images], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        assert r.returncode == 0, r.stdout
        with open(out, 'rb') as f:
            got = f.read()
        assert got == want_a + want_b + want_c + want_e, f'not the records expected\n{r.stdout}'
    # the image holds no shader binary, so the game's dvlb.bin is those records alone
    game = os.path.join(tmp, 'game')
    os.makedirs(os.path.join(game, 'shader_logs'))
    os.replace(rom, os.path.join(game, 'romfs.bin'))
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dvlb_to_log.py')
    r = subprocess.run([sys.executable, tool, game, '--lists', sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    assert r.returncode == 0 and '4 from command lists' in r.stdout, r.stdout
    with open(os.path.join(game, 'shader_logs', 'dvlb.bin'), 'rb') as f:
        assert f.read() == want_a + want_b + want_c + want_e, 'dvlb.bin does not hold the records'

    endings = ([0x88000000], [0x4E000000, 0x88000000])
    programs = [crilayla(bytes(260) + shader_binary(code, [0x1234])) for code in endings]
    broken = programs[1][:20]
    cri = b'CPK '.ljust((1 << 20) - 3, b'\0') + programs[1]
    Path(game, 'romfs.bin').write_bytes(romfs([('flat.cmp', programs[0]), ('archive.bin', cri), ('truncated.cmp', broken)]))
    r = subprocess.run([sys.executable, tool, game, '--lists', sys.argv[1]], capture_output=True, text=True, errors='replace')
    assert r.returncode == 0 and '2 shader binaries, 2 vertex programs' in r.stdout, r.stdout + r.stderr
    expected = b''.join(record(0, code, [0x1234], 0, [0] * 4, {n: n for n in range(4)}) for code in endings)
    assert Path(game, 'shader_logs/dvlb.bin').read_bytes() == expected

    garc_shader = shader_binary(endings[0], [0x1234])
    ga, _ = lz11(garc_shader)
    Path(game, 'romfs.bin').write_bytes(romfs([('archive.garc', garc([[(0, ga)]]))]))
    discovered = Path(tmp, 'garc-dvlb.bin')
    subprocess.run([sys.argv[1], '--lists', out, '--dvlb', str(discovered), str(Path(game, 'romfs.bin'))], capture_output=True, check=True)
    assert list(framed_records(discovered, MAX_UNPACKED)) == [garc_shader]
    r = subprocess.run([sys.executable, tool, game, '--lists', sys.argv[1]], capture_output=True, text=True, errors='replace')
    assert r.returncode == 0 and '1 vertex programs, 0 from command lists' in r.stdout, r.stdout + r.stderr
    assert Path(game, 'shader_logs/dvlb.bin').read_bytes() == record(0, endings[0], [0x1234], 0, [0] * 4, {n: n for n in range(4)})

    tev = b''.join(cmd(reg, 0x00030003 if i == 0 else 0x000F000F, 0, 0, 0, 0, group=True) for i, reg in enumerate(TEV_REGS))
    prefix = bytearray(b'CGFX'.ljust(260, b'\0'))
    prefix[8:12] = b'MTOB'
    struct.pack_into('<I', prefix, 8 + 0x14, 0x10)
    struct.pack_into('<I', prefix, 264 - 0x20, 5)
    body = bytes(prefix) + tev + cmd(0x104, 0x61)
    cri = b'CPK ' + crilayla(body)
    lz, _ = lz11(body)
    Path(game, 'romfs.bin').write_bytes(romfs([('archive.cpk', cri), ('archive.garc', garc([[(0, lz)]]))]))
    stream = Path(tmp, 'materials.raw')
    subprocess.run([sys.executable, tool, game, '--lists', sys.argv[1], '--materials', str(stream)], capture_output=True, check=True)
    frames = list(framed_records(stream, MAX_UNPACKED))
    plain = list(materials(body))
    assert len(plain) == 1 and plain[0][2:] == (0x10, 5)
    assert len(frames) == 2 and all(frame.startswith(b'MTEV') and len(frame) < len(body) for frame in frames)
    assert [list(materials(frame)) for frame in frames] == [plain, plain]

    context = dict(head=1025, tex=[1, 0, 0], tev=[], light=0, fogat=0, blend=3 << 24, mask=0)
    seed = encode(context)
    expected_tev = dict(si=0, csrc=[3, 0, 0], asrc=[3, 0, 0], cmod=[0] * 3, amod=[0] * 3,
                        cop=0, aop=0, cmul=1, amul=1, urgb=0, ua=0)
    Path(game, 'shader_logs/fs_log.bin').write_bytes(struct.pack('<I', len(seed)) + seed)
    model_log = Path(game, 'shader_logs/models_fs.bin')
    tev_tool = Path(__file__).with_name('tev_to_log.py')
    subprocess.run([sys.executable, str(tev_tool), game], capture_output=True, check=True)
    assert records(model_log) == []
    subprocess.run([sys.executable, str(tev_tool), game, '--materials', str(stream)], capture_output=True, check=True)
    assert records(model_log) == [encode(dict(context, tev=[expected_tev], fogat=52))]

    Path(game, 'romfs.bin').write_bytes(romfs([('archive.garc', garc([[(0, body)]]))]))
    subprocess.run([sys.argv[1], '--lists', out, '--materials', str(stream), str(Path(game, 'romfs.bin'))], capture_output=True, check=True)
    assert list(framed_records(stream, MAX_UNPACKED)) == [frames[-1]]

with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    logs = work / 'logs'
    logs.mkdir()
    pipelines = work / 'pipelines.bin'
    fragment = struct.pack('<II', 4, 0)
    first = fragment + want_e
    geometry = b'GS  ' + want_e[4:-56]
    tails = (want_a[:11], want_a[:-1], fragment[:-1], b'FS  ', b'FS  ' + fragment[:-1],
             b'GSX ' + struct.pack('<I', 8) + bytes(8), b'GSX ' + struct.pack('<I', 132) + bytes(40), geometry[:-1])
    for i, tail in enumerate(tails):
        data = geometry + first + tail
        log = logs / f'{i}.bin'
        log.write_bytes(data)
        assert list(vertex_records(data)) == list(vertex_records(first))
        assert list(keys(log)) == [(0,)]
        assert records(log, allow_partial=True) == [bytes(4)]
    pipelines.write_bytes(bytes(8) + fragment + fragment[:-1])
    out = work / 'out'
    result = subprocess.run([sys.argv[1], str(pipelines), str(logs), str(out), '-'], capture_output=True, text=True, errors='replace')
    assert result.returncode == 0, result.stdout + result.stderr
    assert f'{len(tails) + 1} fragment keys (1 shipped), {len(tails)} vertex records' in result.stdout, result.stdout
    generated = (out / 'shaders.idx').read_bytes()
    for log in logs.glob('*.bin'):
        log.write_bytes(first)
    pipelines.write_bytes(bytes(8) + fragment)
    subprocess.run([sys.argv[1], str(pipelines), str(logs), str(out), '-'], capture_output=True, check=True)
    assert (out / 'shaders.idx').read_bytes() == generated

    misses, converted, fragments = (work / name for name in ('misses.bin', 'converted.bin', 'fragments.bin'))
    tool = Path(__file__).with_name('misses_to_log.py')
    for pending in (*tails[:2], *tails[3:]):
        misses.write_bytes(b'FS  ' + fragment + want_e + geometry + pending)
        subprocess.run([sys.executable, str(tool), str(misses), str(converted), str(fragments)], capture_output=True, check=True)
        assert converted.read_bytes() == first
        assert fragments.read_bytes() == fragment
        assert converted.with_suffix('.gsl').read_bytes() == geometry

    combined = b'GSX ' + struct.pack('<I11I', 136, 0, 0, 0, 3, 1, 0, 0, 0, 0, 0, 0) + bytes(4 + 88)
    for pending in (combined[:-1], *tails):
        data = combined + pending
        misses.write_bytes(data)
        assert gsx_misses(misses) == 1
        assert gs_records([data]) == (b'GS  ' + struct.pack('<4I', 3, 1, 0, 0), 1)

    converted.write_bytes(fragment + want_e[:-1])
    misses.write_bytes(fragment + want_e + want_e + want_a[:11])
    assert merge(misses, converted) == 1 and converted.read_bytes() == first
    assert merge(misses, converted) == 0 and converted.read_bytes() == first
print('ok')
