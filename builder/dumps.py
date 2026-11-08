"""What a 3DS dump is, and the files a port is built from. A dump can come as .3ds/.cci (NCSD), .cxi/.app (NCCH) or .cia. This reads plain
ones; decrypt.py makes encrypted ones plain first (your converter: makerom and pyctr with your own 3DS's keys). Every part is checked against
the hashes in its own header, so a bad decryption or a damaged dump stops here instead of making a broken game. Some tools decrypt a dump
and leave its "encrypted" flags on: the hashes decide, not the flags.

A game's folder gets what tools/win/build3ds.py and the runtime read: exheader.bin, code.bin, romfs.bin (the RomFS level 3 image), exefs/<name>.bin,
info.json. An update adds its own code and exheader and update_romfs.bin, which the game lays over the base RomFS itself (SelfNCCH path type 5).
DLC adds dlc_<content index>.romfs per content and the records am:app hands out (runtime/services/misc.cpp)."""
import hashlib
import json
import os
import struct

import decrypt

MU = 0x200
KINDS = {0x00040000: 'game', 0x00040002: 'demo', 0x0004000E: 'update', 0x0004008C: 'dlc'}
OWNED, DOWNLOADED = 2, 1  # a DLC content's ownership bits, as am:app reports them
SIG_SIZES = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80, 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}  # signature type -> bytes, type included


class DumpError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


class FileSlice:
    """part of an open file, read with pread"""
    def __init__(self, f, off, size):
        self.f, self.off, self.size = f, off, size

    def pread(self, pos, n):
        self.f.seek(self.off + pos)
        return self.f.read(max(0, min(n, self.size - pos)))


def blz_decompress(data):
    """Nintendo's backward LZ, used for the ExeFS .code section (tools/extract_game.py)"""
    top_bottom, extra = struct.unpack('<II', data[-8:])
    out = bytearray(data) + bytearray(extra)
    index = len(data) - (top_bottom >> 24)
    stop = len(data) - (top_bottom & 0xFFFFFF)
    at = len(out)
    while index > stop:
        index -= 1
        control = data[index]
        for _ in range(8):
            if index <= stop or at <= 0:
                break
            if control & 0x80:
                index -= 2
                pair = data[index] | (data[index + 1] << 8)
                count = ((pair >> 12) & 15) + 3
                back = (pair & 0x0FFF) + 3  # distance from the byte about to be written, after at -= 1
                for _ in range(count):
                    at -= 1
                    out[at] = out[at + back]
            else:
                index -= 1
                at -= 1
                out[at] = data[index]
            control = (control << 1) & 0xFF
    if at > stop:
        raise DumpError('The code didn\'t decompress: a damaged dump.')
    return bytes(out)


class Cia:
    """A CIA's ticket, TMD and every content it holds"""
    def __init__(self, f):
        hdr = f.read(0x2020)
        hdr_size, _, _, cert_size, tik_size, tmd_size, _ = struct.unpack_from('<IHHIIII', hdr)
        bitmap = hdr[0x20:0x2020]
        al = lambda n: (n + 63) & ~63
        tik_off = al(hdr_size) + al(cert_size)
        tmd_off = tik_off + al(tik_size)
        content_off = tmd_off + al(tmd_size)
        f.seek(tik_off)
        tik = f.read(tik_size)
        self.ticket = tik[SIG_SIZES.get(struct.unpack_from('>I', tik)[0], 0x140):]
        self.ticket_size = tik_size
        f.seek(tmd_off)
        tmd = f.read(tmd_size)
        self.tmd = tmd[SIG_SIZES.get(struct.unpack_from('>I', tmd)[0], 0x140):]
        self.title_id = struct.unpack_from('>Q', self.tmd, 0x4C)[0]
        self.title_type = struct.unpack_from('>I', self.tmd, 0x54)[0]
        self.version = struct.unpack_from('>H', self.tmd, 0x9C)[0]
        self.f = f
        self.contents = []  # (content id, index, type, size, offset in the file or None if this CIA leaves it out)
        off = content_off
        for i in range(struct.unpack_from('>H', self.tmd, 0x9E)[0]):
            cid, index, ctype, size = struct.unpack_from('>IHHQ', self.tmd, 0xC4 + 64 * 36 + 0x30 * i)
            present = bitmap[index >> 3] & (0x80 >> (index & 7))
            self.contents.append((cid, index, ctype, size, off if present else None))
            if present:
                off += al(size)

    def content(self, n):
        """content n of the TMD as it's stored: plain only if the CIA was decrypted"""
        return FileSlice(self.f, self.contents[n][4], self.contents[n][3])


def plain(src, nh):
    """whether an NCCH's data is plain, whatever its flags say: the first region with a hash decides"""
    regions = [(0x200, 0x400, nh[0x160:0x180])] if u32(nh, 0x180) else []
    regions += [(u32(nh, o) * MU, u32(nh, o + 8) * MU, nh[h:h + 32]) for o, h in ((0x1A0, 0x1C0), (0x1B0, 0x1E0)) if u32(nh, o + 8)]
    return bool(regions) and hashlib.sha256(src.pread(regions[0][0], regions[0][1])).digest() == regions[0][2]


def ncch(src):
    """a plain NCCH; an encrypted one is decrypt.py's job first"""
    nh = src.pread(0, 0x200)
    if nh[0x100:0x104] != b'NCCH' or not (nh[0x18F] & 4 or plain(src, nh)):
        raise DumpError('This dump is still encrypted.')
    return src


class Dump:
    """A 3DS dump: what it is, read from its headers"""
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        head = self.f.read(0x200)
        self.cia = None
        self.version = 0
        if len(head) >= 4 and u32(head, 0) == 0x2020:
            self.f.seek(0)
            self.cia = Cia(self.f)
            self.title_id, self.version = self.cia.title_id, self.cia.version
            if not any(c[4] is not None for c in self.cia.contents):
                raise DumpError('This CIA holds no contents.')
            self.size = sum(c[3] for c in self.cia.contents if c[4] is not None)
        elif head[0x100:0x104] == b'NCSD':
            self._offset, self.size = u32(head, 0x120) * MU, u32(head, 0x124) * MU  # partition 0: the program
            self.f.seek(self._offset)
            self.title_id = struct.unpack_from('<Q', self.f.read(0x200), 0x118)[0]
        elif head[0x100:0x104] == b'NCCH':
            self._offset, self.size = 0, u32(head, 0x104) * MU
            self.title_id = struct.unpack_from('<Q', head, 0x118)[0]
        else:
            raise DumpError('Not a 3DS dump this can read (.3ds, .cci, .cxi, .app or .cia).')
        self.kind = KINDS.get(self.title_id >> 32, 'other')
        self.name, self.publisher, self.product, self.icon = None, None, None, None

    @property
    def unique(self):
        """the low half of the title id, shared by a game, its update and its DLC"""
        return self.title_id & 0xFFFFFFFF

    def program_source(self):
        """the program as it's stored: content 0 of a CIA, partition 0 of a card image"""
        if self.cia:
            return self.cia.content(next(i for i, c in enumerate(self.cia.contents) if c[4] is not None))
        return FileSlice(self.f, self._offset, self.size)

    def encrypted(self):
        """whether this needs decrypt.py first: the CIA's layer or the NCCH's is still on"""
        try:
            ncch(self.program_source())
            return False
        except DumpError:
            return True

    def program(self):
        return ncch(self.program_source())

    def read_names(self):
        """name, publisher, product code and icon, from the program's header and ExeFS (decrypted on the fly if needed)"""
        if self.encrypted():
            try:
                self.product, smdh = decrypt.program_icon(self.path)
            except decrypt.DecryptError as e:
                raise DumpError(str(e)) from None
        else:
            p = self.program()
            nh = p.pread(0, 0x200)
            self.product = nh[0x150:0x160].rstrip(b'\0').decode('ascii', 'replace')
            smdh = exefs_files(p, nh, only=('icon',)).get('icon')
        if smdh and smdh[:4] == b'SMDH':
            for lang in (1, 0):  # English, else Japanese
                short = smdh[8 + lang * 0x200:8 + lang * 0x200 + 0x80].decode('utf-16-le', 'replace').split('\0')[0]
                long_ = smdh[8 + lang * 0x200 + 0x80:8 + lang * 0x200 + 0x180].decode('utf-16-le', 'replace').split('\0')[0]
                if short:
                    self.name = long_.replace('\n', ' ') if len(long_) > len(short) else short
                    self.publisher = smdh[8 + lang * 0x200 + 0x180:8 + lang * 0x200 + 0x200].decode('utf-16-le', 'replace').split('\0')[0]
                    break
            self.icon = smdh_icon(smdh)
        return self


def smdh_icon(smdh):
    """the 48x48 icon as rows of (r, g, b): 8x8 tiles of RGB565 in Morton order"""
    icon = [[(0, 0, 0)] * 48 for _ in range(48)]
    for t in range(36):
        for k in range(64):
            x = sum(((k >> (2 * b)) & 1) << b for b in range(3))
            y = sum(((k >> (2 * b + 1)) & 1) << b for b in range(3))
            v = struct.unpack_from('<H', smdh, 0x24C0 + (t * 64 + k) * 2)[0]
            icon[t // 6 * 8 + y][t % 6 * 8 + x] = ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
    return icon


def exefs_files(p, nh, only=None):
    off, size, hashed = u32(nh, 0x1A0) * MU, u32(nh, 0x1A4) * MU, u32(nh, 0x1A8) * MU
    if not size:
        return {}
    head = p.pread(off, 0x200)
    if hashlib.sha256(p.pread(off, hashed)).digest() != nh[0x1C0:0x1E0]:
        raise DumpError('The ExeFS fails its hash: a bad decryption or a damaged dump.')
    files = {}
    for i in range(10):
        name = head[i * 16:i * 16 + 8].rstrip(b'\0').decode('ascii', 'replace')
        if not name or (only and name not in only):
            continue
        foff, fsize = struct.unpack_from('<II', head, i * 16 + 8)
        data = p.pread(off + 0x200 + foff, fsize)
        if hashlib.sha256(data).digest() != head[0x200 - 32 * (i + 1):0x200 - 32 * i]:
            raise DumpError(f'ExeFS {name} fails its hash: a bad decryption or a damaged dump.')
        files[name] = data
    return files


def copy_romfs(p, nh, out_path, progress=None):
    """The RomFS's level 3 image (what fs reads), checked against the header's hash. Returns False if there's no RomFS."""
    off, size, hashed = u32(nh, 0x1B0) * MU, u32(nh, 0x1B4) * MU, u32(nh, 0x1B8) * MU
    if not size:
        return False
    if hashlib.sha256(p.pread(off, hashed)).digest() != nh[0x1E0:0x200]:
        raise DumpError('The RomFS fails its hash: a bad decryption or a damaged dump.')
    ivfc = p.pread(off, 0x60)
    if ivfc[:4] != b'IVFC':
        raise DumpError('The RomFS has no IVFC header.')
    block = 1 << u32(ivfc, 0x4C)
    l3_size = struct.unpack_from('<Q', ivfc, 0x44)[0]
    l3_off = off + ((0x60 + u32(ivfc, 8) + block - 1) & ~(block - 1))
    tmp = out_path + '.part'
    with open(tmp, 'wb') as o:
        done = 0
        while done < l3_size:
            n = min(8 << 20, l3_size - done)
            o.write(p.pread(l3_off + done, n))
            done += n
            if progress:
                progress(done, l3_size)
    os.replace(tmp, out_path)
    return True


def extract_program(dump, out_dir, progress=None, romfs_name='romfs.bin'):
    """A game's or an update's program, from a plain dump, into out_dir. Returns the info written to info.json."""
    p = dump.program()
    nh = p.pread(0, 0x200)
    ex = p.pread(0x200, 0x800)
    if hashlib.sha256(ex[:0x400]).digest() != nh[0x160:0x180]:
        raise DumpError('The extended header fails its hash: a bad decryption or a damaged dump.')
    files = exefs_files(p, nh)
    if '.code' not in files:
        raise DumpError('This program has no code (.code missing from its ExeFS).')
    code = blz_decompress(files['.code']) if ex[0xD] & 1 else files['.code']
    text, rodata, data = (struct.unpack_from('<III', ex, o) for o in (0x10, 0x20, 0x30))
    if len(code) < (text[1] + rodata[1]) * 0x1000 + data[2]:
        raise DumpError('The code is shorter than its own layout says.')
    os.makedirs(os.path.join(out_dir, 'exefs'), exist_ok=True)
    for name, d in files.items():
        if name != '.code':
            with open(os.path.join(out_dir, 'exefs', name.lstrip('.') + '.bin'), 'wb') as o:
                o.write(d)
    with open(os.path.join(out_dir, 'code.bin'), 'wb') as o:
        o.write(code)
    with open(os.path.join(out_dir, 'exheader.bin'), 'wb') as o:
        o.write(ex)
    copy_romfs(p, nh, os.path.join(out_dir, romfs_name), progress)
    info = program_info(ex)
    with open(os.path.join(out_dir, 'info.json'), 'w') as o:
        json.dump(info, o, indent=1)
    return info


def program_info(ex):
    """info.json from an extended header, as tools/extract_game.py writes it (mods.py rewrites it after a mod changes the header)"""
    text, rodata, data = (struct.unpack_from('<III', ex, o) for o in (0x10, 0x20, 0x30))
    return {
        'program_id': '%016X' % struct.unpack_from('<Q', ex, 0x200)[0],
        'text': {'addr': text[0], 'size': text[2], 'pages': text[1]},
        'rodata': {'addr': rodata[0], 'size': rodata[2], 'pages': rodata[1]},
        'data': {'addr': data[0], 'size': data[2], 'pages': data[1]},
        'bss': u32(ex, 0x3C),
        'stack': u32(ex, 0x1C),
    }


def title_info(dump):
    """am:app's title info: title id, size, version, type"""
    return struct.pack('<QQHHI', dump.title_id, dump.size, dump.version, 0, dump.cia.title_type if dump.cia else 0x40)


def add_update(update, plain_update, game_dir, progress=None):
    """The update's code, exheader and ExeFS in place of the base's, and its RomFS beside the base's. update is the dump as given (for its
    version), plain_update a plain copy of it (the same dump if it was plain already)."""
    for f in ('code.bin', 'exheader.bin', 'info.json'):
        if os.path.exists(os.path.join(game_dir, f)):
            os.remove(os.path.join(game_dir, f))
    info = extract_program(plain_update, game_dir, progress, romfs_name='update_romfs.bin')
    with open(os.path.join(game_dir, 'update_title.bin'), 'wb') as o:
        o.write(title_info(update))
    return info


def add_dlc(dlc, sources, game_dir, progress=None):
    """Every content the DLC's CIA holds, as RomFS images, and the records am:app reads. sources: content index -> a plain NCCH (a file
    path) for each content in the CIA. Returns how many contents came out."""
    if not dlc.cia:
        raise DumpError('DLC comes as a CIA: this one is another kind of file.')
    records, n = [], 0
    for i, (cid, index, ctype, size, off) in enumerate(dlc.cia.contents):
        owned = 0
        if index in sources:
            with open(sources[index], 'rb') as f:
                p = ncch(FileSlice(f, 0, os.path.getsize(sources[index])))
                if copy_romfs(p, p.pread(0, 0x200), os.path.join(game_dir, f'dlc_{index:08x}.romfs')):
                    owned, n = OWNED | DOWNLOADED, n + 1
        records.append(struct.pack('<HHIQB7x', index, ctype, cid, size, owned))
        if progress:
            progress(i + 1, len(dlc.cia.contents))
    t = dlc.cia.ticket
    with open(os.path.join(game_dir, 'dlc_contents.bin'), 'wb') as o:
        o.write(b''.join(records))
    with open(os.path.join(game_dir, 'dlc_title.bin'), 'wb') as o:
        o.write(title_info(dlc))
    with open(os.path.join(game_dir, 'dlc_ticket.bin'), 'wb') as o:
        o.write(struct.pack('<QQHHI', dlc.title_id, struct.unpack_from('>Q', t, 0x90)[0], struct.unpack_from('>H', t, 0xA6)[0], 0, dlc.cia.ticket_size))
    return n


def plain_dlc_sources(dlc, work_dir):
    """a plain DLC CIA's contents as files, for add_dlc: {content index: path}"""
    out = {}
    for i, (cid, index, ctype, size, off) in enumerate(dlc.cia.contents):
        if off is not None:
            out[index] = os.path.join(work_dir, f'dlc_content_{index:08x}.ncch')
            src = dlc.cia.content(i)
            with open(out[index], 'wb') as o:
                for at in range(0, size, 8 << 20):
                    o.write(src.pread(at, 8 << 20))
    return out


SYSTEM_ARCHIVES = ('0004009b00010202', '0004009b00010402', '0004009b00014002', '0004009b00014102', '0004009b00014202', '0004009b00014302', '000400db00010302')


def import_system_archives(user_dir, out_dir, work_dir):
    """The shared font, Mii data and the other system archives games read, from an Azahar or Citra user folder's NAND. Returns which came out."""
    got = []
    for tid in SYSTEM_ARCHIVES:
        content = os.path.join(user_dir, 'nand', '00000000000000000000000000000000', 'title', tid[:8], tid[8:], 'content')
        for app in sorted(f for f in os.listdir(content) if f.endswith('.app')) if os.path.isdir(content) else []:
            d = Dump(os.path.join(content, app))
            path = d.path if not d.encrypted() else decrypt.to_plain(d.path, os.path.join(work_dir, 'sysarc_' + tid))
            with open(path, 'rb') as f:
                p = ncch(FileSlice(f, 0, os.path.getsize(path)))
                os.makedirs(out_dir, exist_ok=True)
                if copy_romfs(p, p.pread(0, 0x200), os.path.join(out_dir, tid + '.romfs')):
                    got.append(tid)
            if path != d.path:
                os.remove(path)
            break
    return got
