#!/usr/bin/env python3
"""Packs a title's code/content/meta folders into a WUP folder for WUP Installer GX2, then proves the result by decrypting
everything it wrote and comparing it back. Both go through the files a block at a time, so a game of any size packs in little memory. The layout follows NUSPacker's (Maschell, GPL-3.0), which I read but don't run.

pack_wup.py <title dir> <output dir>. The common key arrives as hex on stdin, so it's never in argv, a file or a log."""
import hashlib
import os
import re
import shutil
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAD_UNIT = 0x8000     # non-hashed contents are padded to this
BLOCK_DATA = 0xFC00   # hashed contents: data per block...
BLOCK_HASHES = 0x400  # ...after this much of hash tree
FST_ALIGN_FILE = 0x20
FLAGS_CODE, FLAGS_META, FLAGS_CONTENT = 0x0000, 0x0040, 0x0400
GROUP_META = 0x0400
TYPE_PLAIN, TYPE_HASHED = 0x2001, 0x2003


def cbc(key, iv, data, decrypt=False):
    c = Cipher(algorithms.AES(key), modes.CBC(iv))
    op = c.decryptor() if decrypt else c.encryptor()
    return op.update(data) + op.finalize()


def align(n, unit):
    return (n + unit - 1) & ~(unit - 1)


def sha1(b):
    return hashlib.sha1(b).digest()


def content_iv(content_id):
    return struct.pack('>H', content_id) + bytes(14)


# ---- the hash tree of a hashed content
def tree_level(lower, block_count, level):
    out = []
    for j in range(block_count // 16 ** level + 1):
        group = b''.join(lower[i] if i < len(lower) else bytes(20) for i in range(j * 16, j * 16 + 16))
        out.append(sha1(group))
    return out


def hash_tree(h0):
    h1 = tree_level(h0, len(h0), 1)
    h2 = tree_level(h1, len(h0), 2)
    h3 = tree_level(h2, len(h0), 3)
    return h1, h2, b''.join(h3)


def block_hashes(block, h0, h1, h2):
    def table(hashes, group):
        return b''.join(hashes[i] if i < len(hashes) else bytes(20) for i in range(group * 16, group * 16 + 16))
    out = table(h0, block // 16) + table(h1, block // 256) + table(h2, block // 4096)
    return out.ljust(BLOCK_HASHES, b'\0')


# ---- a content's bytes, a piece at a time: a game is gigabytes, and a PC with less free memory than that packed nothing (a tester's
# Pokemon Super Mystery Dungeon stopped here with MemoryError, 2026-11-12)
def pieces(title, files):
    """the files of a content one after another, each padded to the file table's alignment"""
    for d, n in files:
        size = 0
        with open(os.path.join(title, d, n), 'rb') as f:
            while chunk := f.read(1 << 20):
                size += len(chunk)
                yield chunk
        if size % FST_ALIGN_FILE:
            yield bytes(-size % FST_ALIGN_FILE)


def blocks(parts, size, never_none=False):
    """the same bytes cut into blocks of `size`, the last one padded with zeros (never_none: an empty content is one block of zeros)"""
    held, any_block = bytearray(), False
    for p in parts:
        held += p
        while len(held) >= size:
            yield bytes(held[:size])
            del held[:size]
            any_block = True
    if held or (never_none and not any_block):
        yield bytes(held).ljust(size, b'\0')


def encrypt_hashed(title_key, content_id, parts, out):
    """twice through the content: every block's hash first, as each block carries its neighbours' hashes"""
    h0 = [sha1(b) for b in blocks(parts(), BLOCK_DATA, True)]
    h1, h2, h3 = hash_tree(h0)
    for n, plain in enumerate(blocks(parts(), BLOCK_DATA, True)):
        hashes = bytearray(block_hashes(n, h0, h1, h2))
        data_iv = bytes(hashes[(n % 16) * 20:(n % 16) * 20 + 16])  # this block's own H0 is its data IV
        hashes[1] ^= content_id & 0xFF
        out.write(cbc(title_key, content_iv(content_id), bytes(hashes)))
        out.write(cbc(title_key, data_iv, plain))
    return len(h0) * (BLOCK_HASHES + BLOCK_DATA), h3


# ---- which file goes in which content: NUSPacker's standard rules, in its order
class Content:
    def __init__(self, cid, hashed, group, parent, flags):
        self.id, self.hashed, self.group, self.parent, self.flags = cid, hashed, group, parent, flags
        self.files, self.size, self.h3, self.digest = [], 0, None, b''  # size: of the encrypted content

    @property
    def type(self):
        return TYPE_HASHED if self.hashed else TYPE_PLAIN


def assign_contents(title, group, parent_id):
    tree = {d: sorted(os.listdir(os.path.join(title, d))) for d in ('code', 'content', 'meta')}
    for d, names in tree.items():
        assert names, f'{d} must not be empty'
        for n in names:
            assert os.path.isfile(os.path.join(title, d, n)), f'subfolders are not supported yet: {d}/{n}'
    fst = Content(0, False, 0, 0, FLAGS_CODE)
    contents = [fst]

    def new(hashed, g, parent, flags):
        c = Content(len(contents), hashed, g, parent, flags)
        contents.append(c)
        return c

    assert {'app.xml', 'cos.xml'} <= set(tree['code']) and 'meta.xml' in tree['meta']
    new(False, 0, 0, FLAGS_CODE).files.append(('code', 'app.xml'))
    new(False, 0, 0, FLAGS_CODE).files.append(('code', 'cos.xml'))
    new(True, GROUP_META, 0, FLAGS_META).files.append(('meta', 'meta.xml'))
    special = {'meta.xml', 'bootMovie.h264', 'bootLogoTex.tga'}
    rest = new(True, GROUP_META, 0, FLAGS_META)
    rest.files += [('meta', n) for n in tree['meta'] if n not in special]
    for n in ('bootMovie.h264', 'bootLogoTex.tga'):
        assert n in tree['meta'], n
        new(True, GROUP_META, 0, FLAGS_META).files.append(('meta', n))
    for n in tree['code']:
        if n.endswith(('.rpx', '.rpl')):
            new(False, 0, 0, FLAGS_CODE).files.append(('code', n))
    other_code = [n for n in tree['code'] if n not in ('app.xml', 'cos.xml') and not n.endswith(('.rpx', '.rpl'))]
    assert not other_code, f'no rule for {other_code}'
    data = new(True, group, parent_id, FLAGS_CONTENT)
    data.files += [('content', n) for n in tree['content']]
    return contents, tree


def build_contents(title, contents, title_key, out_dir):
    offsets = {}
    for c in contents[1:]:
        at = 0
        for d, n in c.files:
            size = os.path.getsize(os.path.join(title, d, n))
            offsets[(d, n)] = (c, at, size)
            at += align(size, FST_ALIGN_FILE)
        parts = lambda: pieces(title, c.files)
        with open(os.path.join(out_dir, f'{c.id:08X}.app'), 'wb') as out:
            if c.hashed:
                c.size, c.h3 = encrypt_hashed(title_key, c.id, parts, out)
                c.digest = sha1(c.h3)
                with open(os.path.join(out_dir, f'{c.id:08X}.h3'), 'wb') as f:
                    f.write(c.h3)
            else:
                enc, digest = Cipher(algorithms.AES(title_key), modes.CBC(content_iv(c.id))).encryptor(), hashlib.sha1()
                for b in blocks(parts(), PAD_UNIT):
                    digest.update(b)
                    out.write(enc.update(b))
                    c.size += len(b)
                c.digest = digest.digest()
    return offsets


# ---- the file table
def build_fst(contents, tree, offsets):
    strings = bytearray()

    def name(s):
        at = len(strings)
        strings.extend(s.encode() + b'\0')
        return at

    entries = []
    root_name = name('')
    assert root_name == 0
    index = 1
    for d in ('code', 'content', 'meta'):
        # A folder's content is the last rule that matched in it, as NUSPacker leaves it.
        owner = max((c for c in contents if any(f[0] == d for f in c.files)), key=lambda c: c.id)
        entry_at = index
        following = index + 1 + len(tree[d])
        entries.append(struct.pack('>I', 0x01000000 | name(d)) + struct.pack('>IIHH', 0, following, owner.flags, owner.id))
        index += 1
        for n in tree[d]:
            c, off, size = offsets[(d, n)]
            assert off % FST_ALIGN_FILE == 0
            entries.append(struct.pack('>I', name(n)) + struct.pack('>IIHH', off >> 5, size, c.flags, c.id))
            index += 1
        assert entry_at < index
    root = b'\x01' + bytes(7) + struct.pack('>I', index) + bytes(4)

    headers = bytearray()
    offset = 0
    for c in contents:
        if c.id == 0:
            # The FST describes itself as empty at offset 0, and real data starts two units in.
            headers += struct.pack('>IIQIB', 0, 0, 0, 0, 0).ljust(0x20, b'\0')
            offset = 2
            continue
        units = c.size // PAD_UNIT
        written = max(units - ((units // 64) + 1) * 2, 0) if c.hashed else units
        headers += struct.pack('>IIQIB', offset, written, c.parent, c.group, 2 if c.hashed else 1).ljust(0x20, b'\0')
        offset += units
    head = b'FST\0' + struct.pack('>II', 0x20, len(contents)) + bytes(0x14)
    data = head + bytes(headers) + root + b''.join(entries) + bytes(strings)
    return data.ljust(align(len(data), PAD_UNIT), b'\0')


# ---- metadata files
def build_tmd(contents, title_id, group, app_type, os_version, title_version=0):
    records = b''.join(struct.pack('>IHHQ', c.id, c.id, c.type, c.size) + c.digest.ljust(0x20, b'\0') for c in contents)
    infos = struct.pack('>HH', 0, len(contents)) + hashlib.sha256(records).digest()
    infos = infos.ljust(0x24 * 0x40, b'\0')
    issuer = b'Root-CA00000003-CP0000000b'.ljust(0x40, b'\0')
    head = struct.pack('>I', 0x00010004) + bytes(0x100 + 0x3C) + issuer + bytes([1, 0, 0, 0])
    head += struct.pack('>QQIHIII', os_version, title_id, 0x100, group, app_type, 0, 0) + bytes(50)
    head += struct.pack('>IHHH', 0, title_version, len(contents), 0) + bytes(2) + hashlib.sha256(infos).digest()
    assert len(head) == 0x204, hex(len(head))
    return head + infos + records


def build_ticket(title_id, title_key, common_key):
    iv = struct.pack('>Q', title_id) + bytes(8)
    t = bytes.fromhex('00010004') + os.urandom(0x100) + bytes(0x3C)
    t += b'Root-CA00000003-XS0000000c'.ljust(0x20, b'\0') + bytes(0x5C) + bytes.fromhex('010000')
    t += cbc(common_key, iv, title_key) + bytes.fromhex('000005') + os.urandom(6) + bytes(4)
    t += struct.pack('>Q', title_id) + bytes.fromhex('00000011000000000000000000000005') + bytes(0xB0)
    t += bytes.fromhex('00010014000000AC000000140001001400000000000000280000000100000084000000840003000000000000FFFFFF01')
    t += bytes(0x7C)
    assert len(t) == 0x350, hex(len(t))
    return t


def build_cert():
    c = bytearray(0xA00)
    struct.pack_into('>I', c, 0x000, 0x010003)
    struct.pack_into('>I', c, 0x400, 0x010004)
    struct.pack_into('>I', c, 0x700, 0x010004)
    c[0x240:0x250] = b'Root'.ljust(16, b'\0')
    c[0x280:0x290] = b'\0\0\0\1CA00000003\0\0'
    c[0x540:0x550] = b'Root-CA00000003\0'
    c[0x580:0x590] = b'\0\0\0\1CP0000000b\0\0'
    c[0x840:0x850] = b'Root-CA00000003\0'
    c[0x880:0x890] = b'\0\0\0\1XS0000000c\0\0'
    return bytes(c)


# ---- proof: read the output back the way the console would
def decrypted(out_dir, cid, ctype, size, digest, title_key):
    """a content's plain bytes block by block, each checked against its hash as it comes"""
    path = os.path.join(out_dir, f'{cid:08X}.app')
    assert os.path.getsize(path) == size, f'content {cid} size'
    with open(path, 'rb') as f:
        if ctype == TYPE_HASHED:
            with open(os.path.join(out_dir, f'{cid:08X}.h3'), 'rb') as h:
                assert sha1(h.read()) == digest, f'content {cid} h3'
            for n in range(size // 0x10000):
                block = f.read(0x10000)
                hashes = bytearray(cbc(title_key, content_iv(cid), block[:BLOCK_HASHES], decrypt=True))
                hashes[1] ^= cid & 0xFF
                data = cbc(title_key, bytes(hashes[(n % 16) * 20:(n % 16) * 20 + 16]), block[BLOCK_HASHES:], decrypt=True)
                assert sha1(data) == bytes(hashes[(n % 16) * 20:(n % 16) * 20 + 20]), f'content {cid} block {n}'
                yield data
        else:
            assert ctype == TYPE_PLAIN and size % PAD_UNIT == 0
            dec, whole = Cipher(algorithms.AES(title_key), modes.CBC(content_iv(cid))).decryptor(), hashlib.sha1()
            while block := f.read(PAD_UNIT):
                data = dec.update(block)
                whole.update(data)
                yield data
            assert whole.digest() == digest, f'content {cid} hash'


def same_files(parts, files, title):
    """the files a content holds, each at its place in the content's bytes, against the files they were made from"""
    held, start = b'', 0  # what's read of the content and not yet compared, and where in the content it starts
    for off, size, d, n in sorted(files):
        with open(os.path.join(title, d, n), 'rb') as f:
            assert os.fstat(f.fileno()).st_size == size, f'{d}/{n} size'
            while size:
                if off >= start + len(held):
                    start, held = start + len(held), next(parts, None)
                    assert held is not None, f'{d}/{n} ends past its content'
                    continue
                got = held[off - start:off - start + size]
                assert got == f.read(len(got)), f'{d}/{n} differs after a round trip'
                off, size = off + len(got), size - len(got)
    for _ in parts:  # the rest still has its hashes checked
        pass


def verify(out_dir, title, tree, common_key, title_id):
    read = lambda n: open(os.path.join(out_dir, n), 'rb').read()
    tik, tmd = read('title.tik'), read('title.tmd')
    assert len(read('title.cert')) == 0xA00 and len(tik) == 0x350
    assert struct.unpack('>Q', tik[0x1DC:0x1E4])[0] == title_id, 'ticket title id'
    title_key = cbc(common_key, struct.pack('>Q', title_id) + bytes(8), tik[0x1BF:0x1CF], decrypt=True)

    assert struct.unpack('>Q', tmd[0x18C:0x194])[0] == title_id, 'tmd title id'
    count = struct.unpack('>H', tmd[0x1DE:0x1E0])[0]
    infos, records = tmd[0x204:0xB04], tmd[0xB04:]
    assert len(records) == count * 0x30, 'tmd record count'
    assert hashlib.sha256(infos).digest() == tmd[0x1E4:0x204], 'content info hash'
    assert hashlib.sha256(records).digest() == infos[4:0x24], 'record hash'

    recs = {}
    for i in range(count):
        cid, idx, ctype, size = struct.unpack('>IHHQ', records[i * 0x30:i * 0x30 + 16])
        assert cid == idx == i
        recs[cid] = (ctype, size, records[i * 0x30 + 16:i * 0x30 + 36])

    fst = b''.join(decrypted(out_dir, 0, *recs[0], title_key))
    assert fst[:4] == b'FST\0' and struct.unpack('>I', fst[8:12])[0] == count
    base = 0x20 + 0x20 * count
    total = struct.unpack('>I', fst[base + 8:base + 12])[0]
    strings = fst[base + total * 0x10:]
    name_at = lambda off: strings[off:strings.index(b'\0', off)].decode()
    held, folder, end = {}, None, 0
    for i in range(1, total):
        w, a, b, flags, cid = struct.unpack('>IIIHH', fst[base + i * 0x10:base + i * 0x10 + 0x10])
        nm = name_at(w & 0xFFFFFF)
        if w >> 24 == 1:
            assert a == 0 and b <= total and i < b, f'folder {nm} links'
            folder, end = nm, b
        else:
            assert i < end and folder, f'{nm} is outside any folder'
            held.setdefault(cid, []).append((a << 5, b, folder, nm))
    found = {(d, n) for files in held.values() for _, _, d, n in files}
    want = {(d, n) for d, names in tree.items() for n in names}
    assert found == want and sum(map(len, held.values())) == len(want), found ^ want
    assert set(held) == set(recs) - {0}, 'a content holds no file'
    for cid, files in held.items():
        same_files(decrypted(out_dir, cid, *recs[cid], title_key), files, title)
    return len(found), count


def field(xml, tag):
    m = re.search(r'<%s [^>]*>\s*([0-9A-Fa-f]+)\s*<' % tag, xml)
    assert m, tag
    return int(m.group(1), 16)


def main():
    title, out_dir = sys.argv[1:3]
    key_hex = sys.stdin.read().strip()
    assert re.fullmatch(r'[0-9a-fA-F]{32}', key_hex), 'expected a 32-digit hex key on stdin'
    common_key = bytes.fromhex(key_hex)
    with open(os.path.join(title, 'code', 'app.xml'), encoding='utf8') as f:
        app = f.read()
    title_id, group = field(app, 'title_id'), field(app, 'group_id')
    app_type, os_version = field(app, 'app_type'), field(app, 'os_version')
    title_version = field(app, 'title_version')  # an update title (0005000E) carries a version above the base's 0; the system runs the highest installed
    assert title_id >> 32 in (0x00050000, 0x0005000E), 'expected a Wii U game or update title id'
    # The file table names the title that holds each content: this one. For an update that is the 0005000E title itself; naming the base
    # instead (versions 1-3, 2026-11-06) made the console look for the update's files in the base's data: "cannot open fs:/vol/content/exheader.bin".
    parent_id = title_id
    title_key = hashlib.sha256(b'3ds-wiiu-recomp title key' + struct.pack('>Q', title_id)).digest()[:16]

    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)
    contents, tree = assign_contents(title, group, parent_id)
    offsets = build_contents(title, contents, title_key, out_dir)

    fst = build_fst(contents, tree, offsets)
    contents[0].size, contents[0].digest = len(fst), sha1(fst)
    with open(os.path.join(out_dir, '00000000.app'), 'wb') as f:
        f.write(cbc(title_key, content_iv(0), fst))

    with open(os.path.join(out_dir, 'title.tmd'), 'wb') as f:
        f.write(build_tmd(contents, title_id, group, app_type, os_version, title_version))
    with open(os.path.join(out_dir, 'title.tik'), 'wb') as f:
        f.write(build_ticket(title_id, title_key, common_key))
    with open(os.path.join(out_dir, 'title.cert'), 'wb') as f:
        f.write(build_cert())

    files, count = verify(out_dir, title, tree, common_key, title_id)
    print(f'title {title_id:016X}: {count} contents, {files} files, round trip verified')
    for n in sorted(os.listdir(out_dir)):
        print(f'{os.path.getsize(os.path.join(out_dir, n)):>9} {n}')


main()
