"""python builder/selftest.py: a synthetic DLC CIA (three contents, the middle one left out of the file) through dumps.add_dlc, checking the RomFS images
and the am:app records it writes. Needs no game files or keys."""
import hashlib
import os
import struct
import sys
import tempfile
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumps  # noqa: E402

DLC = 0x0004008C0007AE00


def ncch(level3):
    """a plain NCCH holding just a RomFS: IVFC header, level 3 at 0x1000"""
    romfs = bytearray(0x1000)
    romfs[0:4] = b'IVFC'
    struct.pack_into('<I', romfs, 8, 0x20)  # master hash size
    struct.pack_into('<Q', romfs, 0x44, len(level3))
    struct.pack_into('<I', romfs, 0x4C, 12)  # level 3 blocks of 4096
    romfs += level3 + bytes(-len(level3) % 0x200)
    h = bytearray(0x200)
    h[0x100:0x104] = b'NCCH'
    struct.pack_into('<I', h, 0x104, (0x200 + len(romfs)) // 0x200)
    struct.pack_into('<Q', h, 0x118, DLC)
    h[0x18F] = 4  # NoCrypto
    struct.pack_into('<III', h, 0x1B0, 1, len(romfs) // 0x200, 1)
    h[0x1E0:0x200] = hashlib.sha256(romfs[:0x200]).digest()
    return bytes(h) + bytes(romfs)


def main():
    images = [os.urandom(70000 + 512 * i) for i in range(3)]
    contents = [(0x10 + i, i, ncch(images[i])) for i in range(3)]
    present = {0, 2}
    sig = struct.pack('>I', 0x10004) + bytes(0x13C)
    tik = bytearray(0x210)
    struct.pack_into('>Q', tik, 0x90, 0x123456789)
    struct.pack_into('>Q', tik, 0x9C, DLC)
    struct.pack_into('>H', tik, 0xA6, 0x0410)
    tik = sig + bytes(tik)
    tmd = bytearray(0xC4 + 64 * 0x24 + 0x30 * len(contents))
    struct.pack_into('>Q', tmd, 0x4C, DLC)
    struct.pack_into('>I', tmd, 0x54, 0x40)
    struct.pack_into('>H', tmd, 0x9C, 0x0410)
    struct.pack_into('>H', tmd, 0x9E, len(contents))
    for i, (cid, index, data) in enumerate(contents):
        struct.pack_into('>IHHQ', tmd, 0xC4 + 64 * 0x24 + 0x30 * i, cid, index, 0x4000, len(data))
    tmd = sig + bytes(tmd)
    body = b''.join(d for _, i, d in contents if i in present)
    hdr = bytearray(0x2020)
    struct.pack_into('<IHHIIIIQ', hdr, 0, 0x2020, 0, 0, 0, len(tik), len(tmd), 0, len(body))
    for i in present:
        hdr[0x20 + (i >> 3)] |= 0x80 >> (i & 7)
    al = lambda b: b + bytes(-len(b) % 64)
    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, 'dlc.cia')
        open(path, 'wb').write(al(bytes(hdr)) + al(tik) + al(tmd) + body)
        d = dumps.Dump(path)
        assert d.kind == 'dlc' and d.unique == 0x0007AE00 and d.version == 0x0410
        assert [c[4] is not None for c in d.cia.contents] == [True, False, True]
        assert not d.encrypted()
        out = os.path.join(work, 'game')
        os.makedirs(out)
        assert dumps.add_dlc(d, dumps.plain_dlc_sources(d, work), out) == 2
        d.f.close()
        for i in present:
            assert open(os.path.join(out, f'dlc_{i:08x}.romfs'), 'rb').read() == images[i], f'content {i}'
        assert not os.path.exists(os.path.join(out, 'dlc_00000001.romfs'))
        recs = open(os.path.join(out, 'dlc_contents.bin'), 'rb').read()
        assert len(recs) == 3 * 0x18
        for k, (cid, index, data) in enumerate(contents):
            assert struct.unpack_from('<HHIQB', recs, k * 0x18) == (index, 0x4000, cid, len(data), 3 if index in present else 0), k
        assert struct.unpack('<QQHHI', open(os.path.join(out, 'dlc_title.bin'), 'rb').read())[::2] == (DLC, 0x0410, 0x40)
        assert struct.unpack('<QQHHI', open(os.path.join(out, 'dlc_ticket.bin'), 'rb').read())[:3] == (DLC, 0x123456789, 0x0410)
    mods_test()
    print('self-test: all good')


def bps(source, target, actions=None):
    """a BPS patch: the whole target as one TargetRead, or the actions given (kind, length, bytes or offset delta)"""
    def num(v):
        out = bytearray()
        while True:
            x = v & 0x7F
            v >>= 7
            if not v:
                return bytes(out + bytes([0x80 | x]))
            out.append(x)
            v -= 1
    body = b'BPS1' + num(len(source)) + num(len(target)) + num(0)
    for kind, n, arg in actions or [(1, len(target), target)]:
        body += num((n - 1) << 2 | kind)
        if kind == 1:
            body += arg
        elif kind > 1:
            body += num(abs(arg) << 1 | (arg < 0))
    body += struct.pack('<II', zlib.crc32(source), zlib.crc32(target))
    return body + struct.pack('<I', zlib.crc32(body))


def mods_test():
    """a mod (a .zip in Luma's layout) over a made-up game: a RomFS file replaced (named in other case), one added in a new folder, code.ips"""
    import mods
    tid = '0004000000123400'
    with tempfile.TemporaryDirectory() as work:
        base = os.path.join(work, 'base', 'romfs')
        for name, data in (('a.bin', b'A' * 100), ('dir/b.bin', b'B' * 33), ('dir/sub/c.bin', b'C' * 7)):
            os.makedirs(os.path.dirname(os.path.join(base, name)), exist_ok=True)
            open(os.path.join(base, name), 'wb').write(data)
        game = os.path.join(work, 'game')
        os.makedirs(game)
        src = mods.Mod(os.path.join(work, 'base'), int(tid, 16))
        root = mods.Dir('')
        for name, member in sorted(src.romfs.items()):
            mods.add_file(root, name, ('mod', src, member))
        mods.write_romfs(root, None, os.path.join(game, 'romfs.bin'))
        ex = bytearray(0x800)
        struct.pack_into('<III', ex, 0x10, 0x100000, 1, 0x1000)  # text: one page
        struct.pack_into('<Q', ex, 0x200, int(tid, 16))
        open(os.path.join(game, 'exheader.bin'), 'wb').write(ex)
        open(os.path.join(game, 'code.bin'), 'wb').write(bytes(0x1000))
        mod = os.path.join(work, 'mod.zip')
        with zipfile.ZipFile(mod, 'w') as z:
            z.writestr(f'luma/titles/{tid.upper()}/romfs/DIR/B.BIN', b'new b')
            z.writestr(f'luma/titles/{tid.upper()}/romfs/added/d.bin', b'D' * 5000)
            z.writestr(f'luma/titles/{tid.upper()}/code.ips', b'PATCH' + b'\x00\x00\x10\x00\x04' + b'\xE1\xA0\x00\x00' + b'\x00\x00\x20\x00\x00\x00\x03\x7F' + b'EOF')
        m = mods.Mod(mod, int(tid, 16))
        assert mods.apply([m], game, log=lambda s: None) == 2
        m.close()
        code = open(os.path.join(game, 'code.bin'), 'rb').read()
        assert code[0x10:0x14] == b'\xE1\xA0\x00\x00' and code[0x20:0x23] == b'\x7F' * 3 and len(code) == 0x1000
        with open(os.path.join(game, 'romfs.bin'), 'rb') as f:
            tree = mods.read_romfs(f)
            got = {}
            for name in ('a.bin', 'dir/b.bin', 'dir/sub/c.bin', 'added/d.bin'):
                e = mods.find_file(tree, name)
                assert e, name
                f.seek(e[1][0])
                got[name] = f.read(e[1][1])
        assert got == {'a.bin': b'A' * 100, 'dir/b.bin': b'new b', 'dir/sub/c.bin': b'C' * 7, 'added/d.bin': b'D' * 5000}, got
        assert mods.apply_bps(b'old code', bps(b'old code', b'brand new code')) == b'brand new code'
        # all four actions: source read, target read, source copy, and a target copy reading what it writes
        all4 = [(0, 3, None), (1, 3, b'XYZ'), (2, 3, 2), (3, 6, 3)]
        assert mods.apply_bps(b'abcdefgh', bps(b'abcdefgh', b'abcXYZcdeXYZcde', all4)) == b'abcXYZcdeXYZcde'
        for bad, why in ((b'other code', 'different version'),):
            try:
                mods.apply_bps(bad, bps(b'old code', b'x'))
                raise AssertionError('a BPS patch for other code went on')
            except mods.ModError as e:
                assert why in str(e), e
        for name, files in (('other', {'luma/titles/0004000000999900/romfs/x': b'1'}), ('plugin', {'luma/plugins/0004000000123400/p.3gx': b'1'})):
            p = os.path.join(work, name + '.zip')
            with zipfile.ZipFile(p, 'w') as z:
                for n, d in files.items():
                    z.writestr(n, d)
            try:
                mods.Mod(p, int(tid, 16)).close()
                raise AssertionError(f'{name}: taken as a mod for this game')
            except mods.ModError:
                pass


if __name__ == '__main__':
    main()
