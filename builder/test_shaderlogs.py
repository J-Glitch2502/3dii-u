"""Self-check for shaderlogs.py's round trips: python builder/test_shaderlogs.py
- A record whose memory starts with a program of the game's DVLB ships as a fingerprint and comes back with the dump's program.
- One laid out from the game's code.bin (plain words, and a swizzle table of plain words) and romfs.bin (a program and every other word of a DVLB's
  table) ships as a recipe and comes back the same from the dump, and not at all from another version of it; one whose words aren't in the dump
  gets neither. Synthetic files, so it runs anywhere."""
import os
import random
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderlogs  # noqa: E402


def vs(entry, code, swz, tail):
    return b'VS  ' + struct.pack('<III', entry, len(code), len(swz)) + struct.pack(f'<{len(code)}I', *code) + struct.pack(f'<{len(swz)}I', *swz) + tail


prog = ([0x11, 0x22, 0x88000000], [0xA, 0xB])  # a program of the game's DVLB
other = ([0x33, 0x44], [0xC])  # one the DVLB scan never found
tail = bytes(range(56))
frag = struct.pack('<I', 3) + b'key' + b'FS  ' + struct.pack('<I', 2) + b'xy'
log = vs(1, prog[0] + [0x99, 0x99], prog[1] + [0x77], tail) + frag + vs(0, *other, tail)  # memory: the program, then another's leftovers

base = tempfile.mkdtemp()
try:
    shipped, vsf, vsr, left = shaderlogs.split(log, {(struct.pack('<3I', *prog[0]), struct.pack('<2I', *prog[1]))})
    assert shipped == frag, 'fragment records ship as they are'
    assert len(left) == 1 and not vsr and len(vsf) == shaderlogs.VSF and b'\x11\x00\x00\x00' not in vsf, 'one fingerprint, no code; the unknown program left out'
    with open(os.path.join(base, 'dvlb.bin'), 'wb') as f:
        f.write(vs(1, *prog, bytes(56)))
    with open(os.path.join(base, 'run.vsf'), 'wb') as f:
        f.write(vsf)
    assert shaderlogs.restore(base) == (1, 0)
    with open(os.path.join(base, 'run_vsf.bin'), 'rb') as f:
        assert f.read() == vs(1, *prog, tail), 'the record comes back with the dump\'s program and its own uniforms and output map'
    assert not os.path.exists(os.path.join(base, 'run.vsf'))
finally:
    shutil.rmtree(base)

rnd = random.Random(9)


def words(n):
    return b''.join(struct.pack('<I', rnd.getrandbits(32)) for _ in range(n))


tmp = tempfile.mkdtemp(prefix='shaderlogs_test_')
try:
    game = os.path.join(tmp, 'game')
    os.makedirs(os.path.join(game, 'shader_logs'))
    first, gap, engine, table = words(1), words(1), words(40), words(6)  # the engine's shader in code.bin: a word, another, then the rest
    code_bin = bytearray(words(30000))
    code_bin[40000:40000 + 4 * 42] = first + gap + engine
    code_bin[40400:40400 + 28] = table[:4] + table[4:]  # its swizzles, plain
    program, dvle_swz = words(60), words(5)
    romfs = bytearray(words(250000))
    romfs[600000:600000 + 240] = program
    for j in range(5):  # a DVLB's table: each swizzle with a word after it
        romfs[600400 + 8 * j:600404 + 8 * j] = dvle_swz[4 * j:4 * j + 4]
    for name, data in (('code.bin', code_bin), ('romfs.bin', romfs)):
        with open(os.path.join(game, name), 'wb') as f:
            f.write(data)

    code, swz, tail = first + engine + program, table + dvle_swz, words(14)
    stranger = words(80)  # another game's memory, in a mixed log
    log = (b'VS  ' + struct.pack('<III', 3, len(code) // 4, len(swz) // 4) + code + swz + tail
           + b'VS  ' + struct.pack('<III', 0, 60, 20) + stranger + tail
           + b'FS  ' + struct.pack('<I', 8) + words(2))
    srcs = shaderlogs.sources(game)
    frag, vsf, vsr, left = shaderlogs.split(log, set(), srcs)
    shaderlogs.close(srcs)
    got = list(shaderlogs.recipes(vsr))
    assert left == [(stranger[:240], stranger[240:])] and len(got) == 1 and not vsf and frag.startswith(b'FS  '), (len(left), len(got))
    entry, fp, t, pc, ps = got[0]
    assert entry == 3 and t == tail and fp == shaderlogs.fingerprint(code, swz)
    assert len(pc) <= 4 and len(ps) <= 4, (pc, ps)  # a few runs, not a word at a time
    assert all(at < 40000 + 4096 or at >= 600000 for _, _, _, at in pc + ps)  # each piece where that part really is

    with open(os.path.join(game, 'shader_logs', 'x.vsr'), 'wb') as f:
        f.write(vsr)
    assert shaderlogs.restore(os.path.join(game, 'shader_logs')) == (1, 0)
    back = open(os.path.join(game, 'shader_logs', 'x_vsr.bin'), 'rb').read()
    assert back == log[:len(back)] and len(back) == 16 + len(code) + len(swz) + len(tail), 'the record comes back as it was logged'

    code_bin[40000 + 4 * 10] ^= 1  # another version of the game: the record is left out, not made wrong
    with open(os.path.join(game, 'code.bin'), 'wb') as f:
        f.write(code_bin)
    with open(os.path.join(game, 'shader_logs', 'x.vsr'), 'wb') as f:
        f.write(vsr)
    assert shaderlogs.restore(os.path.join(game, 'shader_logs')) == (0, 1)
    assert list(shaderlogs.recipes(vsr[:-3])) == []  # a cut file gives nothing

    # a shader kept in a compressed romfs file (Zelda's SPBD3 in RegionBoot.szs): found unpacked by --recipes, unpacked again by the build
    packed_prog, packed_swz = words(50), words(6)
    inner = words(3000) + packed_prog + words(10) + packed_swz + words(100)
    yaz0 = bytearray(b'Yaz0' + struct.pack('>I', len(inner)) + bytes(8))
    for j in range(0, len(inner), 8):  # every byte a literal
        yaz0 += b'\xff' + inner[j:j + 8]
    head = struct.pack('<10I', 0x28, 0, 0, 0, 0, 0, 0, 0x28, 32, 0x80)
    entry_meta = struct.pack('<IIQQII', 0, 0xFFFFFFFF, 16, len(yaz0), 0xFFFFFFFF, 0)  # one file, no name, 16 bytes into the data
    romfs2 = head + entry_meta + bytes(0x80 - 0x28 - 32) + bytes(16) + yaz0
    game2 = os.path.join(tmp, 'game2')
    os.makedirs(os.path.join(game2, 'shader_logs'))
    with open(os.path.join(game2, 'romfs.bin'), 'wb') as f:
        f.write(romfs2)
    with open(os.path.join(game2, 'shader_logs', 'run.bin'), 'wb') as f:
        f.write(b'VS  ' + struct.pack('<III', 0, 50, 6) + packed_prog + packed_swz + tail)
    assert shaderlogs.write_recipes(game2, os.path.join(game2, 'shader_logs')) == (1, 0)
    assert shaderlogs.restore(os.path.join(game2, 'shader_logs')) == (1, 0)
    assert open(os.path.join(game2, 'shader_logs', 'recipes_vsr.bin'), 'rb').read()[16:16 + 224] == packed_prog + packed_swz

    # a finished game's list (confirm): a build's fragment states once each, an earlier list's first, without a miss list's tags, the
    # vertex records, a file with another extension, or the last record of a log cut short
    def state(name):
        return struct.pack('<I', len(name)) + name

    built, kept = os.path.join(tmp, 'built'), os.path.join(tmp, 'kept')
    os.makedirs(built)
    os.makedirs(kept)
    for name, data in (('a.bin', state(b'state one') + vs(0, *other, tail) + b'FS  ' + state(b'state two')),
                       ('confirmed.bin', state(b'state old') + state(b'state two')), ('variants.bin', state(b'state guessed')),
                       ('cut.bin', state(b'state three') + state(b'state cut short')[:-3]), ('notes.txt', state(b'state four'))):
        with open(os.path.join(built, name), 'wb') as f:
            f.write(data)
    assert shaderlogs.confirm(built, kept) == 5
    with open(os.path.join(kept, 'confirmed.bin'), 'rb') as f:
        made = f.read()
    first = state(b'state old') + state(b'state two')
    assert made.startswith(first), 'the earlier list comes first, in its own order'
    rest = {r[1] for r in shaderlogs.records(made[len(first):])}
    assert rest == {state(b'state one'), state(b'state guessed'), state(b'state three')} and len(made) == len(first) + sum(map(len, rest)), rest
    assert shaderlogs.confirm(built, built) == 5, 'in the build\'s own folder too'
    with open(os.path.join(built, 'confirmed.bin'), 'rb') as f:
        assert f.read() == made
    open(os.path.join(game2, 'shader_logs', 'variants.bin'), 'wb').close()
    for folder, why in ((kept, 'a folder no build left'), (os.path.join(game2, 'shader_logs'), 'logs with vertex records alone')):
        try:
            shaderlogs.confirm(folder, kept)
        except SystemExit:
            continue
        raise AssertionError(why + ' was confirmed')
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print('shaderlogs ok: fingerprints, recipes and a finished game\'s list')
