"""Mods in Luma3DS's layout, applied to a game's unpacked folder before it's recompiled, so a mod's code is recompiled with the game's.

A mod is a folder or a .zip holding luma/titles/<title id>/ (or just that folder's contents) with any of:
  romfs/        files that replace or add to the game's RomFS, as Luma's LayeredFS does (names match without regard to case, as on the SD card)
  code.bin      the whole code, decompressed; or code.ips / code.bps, a patch to it
  exheader.bin  the extended header, for a mod that changes the code's layout
Luma's plugins (luma/plugins/<id>/*.3gx, how CTGP-7 runs) patch the game in memory while it plays, which recompiled code never reads: refused.
Several mods go on in the order given, each over the last."""
import json
import os
import re
import shutil
import struct
import zipfile
import zlib

import dumps

CODE = ('code.bin', 'code.ips', 'code.bps')
TID = re.compile(r'^[0-9a-f]{16}$')


class ModError(Exception):
    pass


class Mod:
    """The files of one mod for one game: name -> how to open it, names relative to its title folder"""
    def __init__(self, path, title_id):
        self.path, self.zip = path, None
        if os.path.isdir(path):
            names = [os.path.relpath(os.path.join(d, f), path).replace('\\', '/') for d, _, fs in os.walk(path) for f in fs]
        elif zipfile.is_zipfile(path):
            self.zip = zipfile.ZipFile(path)
            names = [n for n in self.zip.namelist() if not n.endswith('/')]
        else:
            raise ModError(f'{os.path.basename(path)} isn\'t a mod: choose its folder or its .zip.')
        tid = '%016x' % title_id
        roots = {p.lower(): p for n in names for p in self._prefixes(n) if TID.match(p.lower().rsplit('/', 1)[-1])}
        if tid in {r.rsplit('/', 1)[-1] for r in roots}:
            root = next(v for k, v in roots.items() if k.rsplit('/', 1)[-1] == tid)
        elif roots:
            others = sorted({r.rsplit('/', 1)[-1].upper() for r in roots})
            raise ModError(f'This mod is for {", ".join(others)}, not this game ({tid.upper()}): it may be for another region or edition.')
        else:  # the title folder's contents, or a folder holding them: where code.* or romfs/ sits
            found = set()
            for n in names:
                parts = n.split('/')
                low = [x.lower() for x in parts]
                if low[-1] in CODE + ('exheader.bin',):
                    found.add('/'.join(parts[:-1]))
                if 'romfs' in low[:-1]:
                    found.add('/'.join(parts[:low.index('romfs')]))
            root = min(found, key=len) if found else None
            if root is None:
                if any(n.lower().endswith('.3gx') for n in names):
                    raise ModError('This mod is a Luma plugin (.3gx). Plugins change the game in memory while it runs, which a recompiled game '
                                   'never reads, so it can\'t be built in.')
                raise ModError('No mod files in there: a mod has a romfs folder, code.bin, code.ips or code.bps (luma/titles/<title id>/).')
        prefix = (root + '/') if root else ''
        self.files = {n[len(prefix):]: n for n in names if n.startswith(prefix)}
        self.romfs = {k[len('romfs/'):]: v for k, v in self.files.items() if k.lower().startswith('romfs/')}
        self.code = next((v for k, v in self.files.items() if k.lower() in CODE), None)  # member names, as open() takes them
        self.exheader = next((v for k, v in self.files.items() if k.lower() == 'exheader.bin'), None)
        if not (self.romfs or self.code or self.exheader):
            raise ModError('The mod\'s folder for this game is empty.')

    @staticmethod
    def _prefixes(name):
        parts = name.split('/')[:-1]
        return ['/'.join(parts[:i + 1]) for i in range(len(parts))]

    def close(self):
        if self.zip:
            self.zip.close()

    def open(self, name):
        return self.zip.open(name) if self.zip else open(os.path.join(self.path, name), 'rb')

    def size(self, name):
        return self.zip.getinfo(name).file_size if self.zip else os.path.getsize(os.path.join(self.path, name))

    def read(self, name):
        with self.open(name) as f:
            return f.read()

    def changes_code(self):
        """code, or CRO modules (the code most of some games is in): either way the code finders must look again"""
        return bool(self.code or self.exheader or any(n.lower().endswith(('.cro', '.crs')) for n in self.romfs))


# ---------------------------------------------------------------- code patches
def apply_ips(data, patch):
    """IPS, and IPS32 (4-byte offsets)"""
    wide = patch[:5] == b'IPS32'
    if not (wide or patch[:5] == b'PATCH'):
        raise ModError('code.ips isn\'t an IPS patch.')
    out, at, w = bytearray(data), 5, 4 if wide else 3
    end = b'EEOF' if wide else b'EOF'
    while patch[at:at + len(end)] != end:
        if at + w + 2 > len(patch):
            raise ModError('code.ips ends early.')
        off = int.from_bytes(patch[at:at + w], 'big')
        size = int.from_bytes(patch[at + w:at + w + 2], 'big')
        at += w + 2
        if size:
            chunk = patch[at:at + size]
            at += size
        else:  # a run of one byte
            n = int.from_bytes(patch[at:at + 2], 'big')
            chunk = patch[at + 2:at + 3] * n
            at += 3
        if off + len(chunk) > len(out):
            out.extend(bytes(off + len(chunk) - len(out)))
        out[off:off + len(chunk)] = chunk
    return bytes(out)


def apply_bps(data, patch):
    """BPS (beat), checked against its source's CRC so a patch for another version of the game says so"""
    if patch[:4] != b'BPS1':
        raise ModError('code.bps isn\'t a BPS patch.')
    src_crc, dst_crc, own_crc = struct.unpack('<III', patch[-12:])
    if zlib.crc32(patch[:-4]) != own_crc:
        raise ModError('code.bps is damaged (its checksum is wrong).')
    if zlib.crc32(data) != src_crc:
        raise ModError('code.bps was made for a different version of this game\'s code (an update, or another region).')
    at = 4

    def num():
        nonlocal at
        v, shift = 0, 1
        while True:
            b = patch[at]
            at += 1
            v += (b & 0x7F) * shift
            if b & 0x80:
                return v
            shift <<= 7
            v += shift

    num()
    out = bytearray(num())
    meta = num()  # (not at += num(): that reads at before num() moves it)
    at += meta
    o = src_rel = dst_rel = 0
    while at < len(patch) - 12:
        d = num()
        kind, n = d & 3, (d >> 2) + 1
        if kind == 0:  # source read
            out[o:o + n] = data[o:o + n]
        elif kind == 1:  # target read
            out[o:o + n] = patch[at:at + n]
            at += n
        else:
            r = num()
            delta = (-1 if r & 1 else 1) * (r >> 1)
            if kind == 2:  # source copy
                src_rel += delta
                out[o:o + n] = data[src_rel:src_rel + n]
                src_rel += n
            else:  # target copy, which can overlap what it writes
                dst_rel += delta
                for i in range(n):
                    out[o + i] = out[dst_rel + i]
                dst_rel += n
        o += n
    if zlib.crc32(out) != dst_crc:
        raise ModError('code.bps didn\'t produce the code it should have.')
    return bytes(out)


# ---------------------------------------------------------------- RomFS (level 3: header, hash tables, directory and file entries, data)
class Dir:
    def __init__(self, name):
        self.name, self.dirs, self.files = name, [], []  # files: [name, (romfs offset, size) or a Mod file to read]

    def child(self, name, create=False):
        for d in self.dirs:
            if d.name.lower() == name.lower():
                return d
        if create:
            self.dirs.append(Dir(name))
            return self.dirs[-1]
        return None


def read_romfs(f):
    """the tree of a level 3 image, file data left where it is"""
    f.seek(0)
    h = struct.unpack('<10I', f.read(0x28))
    f.seek(h[3])
    dm = f.read(h[4])
    f.seek(h[7])
    fm = f.read(h[8])
    u = lambda b, o: struct.unpack_from('<I', b, o)[0]
    name = lambda b, o, n: b[o:o + n].decode('utf-16-le')

    def walk(off, node):
        c = u(dm, off + 12)
        while c != 0xFFFFFFFF:
            fo, fs = struct.unpack_from('<QQ', fm, c + 8)
            node.files.append([name(fm, c + 32, u(fm, c + 28)), (h[9] + fo, fs)])
            c = u(fm, c + 4)
        c = u(dm, off + 8)
        while c != 0xFFFFFFFF:
            node.dirs.append(walk(c, Dir(name(dm, c + 24, u(dm, c + 20)))))
            c = u(dm, c + 4)
        return node
    return walk(0, Dir(''))


def find_file(root, path):
    """[name, source] for a path, matched without regard to case, or None"""
    *dirs, leaf = path.split('/')
    node = root
    for d in dirs:
        node = node.child(d)
        if node is None:
            return None
    return next((e for e in node.files if e[0].lower() == leaf.lower()), None)


def add_file(root, path, source):
    *dirs, leaf = path.split('/')
    node = root
    for d in dirs:
        node = node.child(d, create=True)
    node.files.append([leaf, source])


def buckets(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


def name_hash(parent, name):
    h = parent ^ 123456789
    for c in struct.unpack(f'<{len(name)}H', name.encode('utf-16-le')):
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def write_romfs(root, src, out, mod_files=None):
    """root's tree as a level 3 image in out (a path); a source is (offset, size) in src, or ('mod', mod, name)"""
    # Nintendo's order, so an image with nothing changed comes out byte for byte the same: a directory's children together, then each child's
    # in turn; files depth first, each directory's together
    dirs, files, kids = [(root, 0)], [], [[]]  # (node, parent index); (entry, parent dir index); each directory's child directories

    def place_children(i):
        for d in dirs[i][0].dirs:
            dirs.append((d, i))
            kids.append([])
            kids[i].append(len(dirs) - 1)
        for k in kids[i]:
            place_children(k)

    def place_files(i):
        files.extend((e, i) for e in dirs[i][0].files)
        for k in kids[i]:
            place_files(k)
    place_children(0)
    place_files(0)
    al4 = lambda n: (n + 3) & ~3
    doff, at = [], 0
    for node, _ in dirs:
        doff.append(at)
        at += 0x18 + al4(len(node.name) * 2)
    dmeta_len = at
    foff, at = [], 0
    for e, _ in files:
        foff.append(at)
        at += 0x20 + al4(len(e[0]) * 2)
    fmeta_len = at
    sizes = []
    for e, _ in files:
        s = e[1]
        sizes.append(s[1] if s[0] != 'mod' else s[1].size(s[2]))
    data_rel, at = [], 0
    for n in sizes:
        at = (at + 15) & ~15
        data_rel.append(at)
        at += n
    nd, nf = buckets(len(dirs)), buckets(len(files))
    dh, fh = [0xFFFFFFFF] * nd, [0xFFFFFFFF] * nf
    dmeta, fmeta = bytearray(dmeta_len), bytearray(fmeta_len)
    first_dir = {i: k[0] for i, k in enumerate(kids) if k}
    nxt_dir = {a: b for k in kids for a, b in zip(k, k[1:])}
    first_file = {}
    for j, (_, p) in enumerate(files):
        first_file.setdefault(p, j)
    for j, (node, p) in enumerate(dirs):
        b = name_hash(doff[p], node.name) % nd
        sib = nxt_dir.get(j)
        fd, ff = first_dir.get(j), first_file.get(j)
        nm = node.name.encode('utf-16-le')
        struct.pack_into('<6I', dmeta, doff[j], doff[p], doff[sib] if sib is not None else 0xFFFFFFFF, doff[fd] if fd is not None else 0xFFFFFFFF,
                         foff[ff] if ff is not None else 0xFFFFFFFF, dh[b], len(nm))
        dmeta[doff[j] + 0x18:doff[j] + 0x18 + len(nm)] = nm
        dh[b] = doff[j]
    for j, (e, p) in enumerate(files):
        b = name_hash(doff[p], e[0]) % nf
        sib = j + 1 if j + 1 < len(files) and files[j + 1][1] == p else None
        nm = e[0].encode('utf-16-le')
        struct.pack_into('<IIQQII', fmeta, foff[j], doff[p], foff[sib] if sib is not None else 0xFFFFFFFF, data_rel[j], sizes[j], fh[b], len(nm))
        fmeta[foff[j] + 0x20:foff[j] + 0x20 + len(nm)] = nm
        fh[b] = foff[j]
    dh_off = 0x28
    dm_off = dh_off + nd * 4
    fh_off = dm_off + dmeta_len
    fm_off = fh_off + nf * 4
    data_off = (fm_off + fmeta_len + 15) & ~15
    tmp = out + '.part'
    with open(tmp, 'wb') as o:
        o.write(struct.pack('<10I', 0x28, dh_off, nd * 4, dm_off, dmeta_len, fh_off, nf * 4, fm_off, fmeta_len, data_off))
        o.write(struct.pack(f'<{nd}I', *dh) + dmeta + struct.pack(f'<{nf}I', *fh) + fmeta)
        for j, (e, _) in enumerate(files):
            o.write(bytes(data_off + data_rel[j] - o.tell()))
            s = e[1]
            if s[0] == 'mod':
                with s[1].open(s[2]) as i:
                    shutil.copyfileobj(i, o, 8 << 20)
            else:
                src.seek(s[0])
                left = s[1]
                while left:
                    chunk = src.read(min(8 << 20, left))
                    if not chunk:
                        raise ModError('The game\'s RomFS ended early.')
                    o.write(chunk)
                    left -= len(chunk)
    os.replace(tmp, out)



# ---------------------------------------------------------------- putting mods into a game's folder
def apply(mods, game_dir, log=print):
    """Every mod over the game's unpacked files, in order. A file an update has goes into the update's RomFS, which the game reads first."""
    code_path = os.path.join(game_dir, 'code.bin')
    ex_path = os.path.join(game_dir, 'exheader.bin')
    replaced = {}
    for mod in mods:
        if mod.exheader:
            ex = bytearray(open(ex_path, 'rb').read())
            new = mod.read(mod.exheader)
            if len(new) not in (0x400, 0x800):
                raise ModError(f'{os.path.basename(mod.path)}: exheader.bin should be 1 or 2 KB, not {len(new)} bytes.')
            ex[:len(new)] = new
            open(ex_path, 'wb').write(ex)
        if mod.code:
            code = open(code_path, 'rb').read()
            patch = mod.read(mod.code)
            kind = mod.code.rsplit('/', 1)[-1].lower()
            code = patch if kind == 'code.bin' else apply_ips(code, patch) if kind == 'code.ips' else apply_bps(code, patch)
            open(code_path, 'wb').write(code)
            log(f'mod {os.path.basename(mod.path)}: {kind} applied')
        for name, member in mod.romfs.items():
            replaced[name.lower()] = (name, mod, member)
    if mods and (any(m.code or m.exheader for m in mods)):
        ex = open(ex_path, 'rb').read()
        info = dumps.program_info(ex)
        code = open(code_path, 'rb').read()
        if len(code) < (info['text']['pages'] + info['rodata']['pages']) * 0x1000 + info['data']['size']:
            raise ModError('The modded code is shorter than its header says: the mod may need its own exheader.bin, or it\'s for another version.')
        json.dump(info, open(os.path.join(game_dir, 'info.json'), 'w'), indent=1)
    if not replaced:
        return 0
    images = [p for p in (os.path.join(game_dir, 'update_romfs.bin'), os.path.join(game_dir, 'romfs.bin')) if os.path.exists(p)]
    if not images:
        raise ModError('This game has no RomFS for the mod\'s files to go in.')
    trees = []
    for p in images:
        with open(p, 'rb') as f:
            trees.append(read_romfs(f))
    touched = set()
    for key, (name, mod, member) in replaced.items():
        for i, tree in enumerate(trees):
            e = find_file(tree, name)
            if e:
                e[1] = ('mod', mod, member)
                touched.add(i)
                break
        else:  # a new file: the base RomFS gets it
            add_file(trees[-1], name, ('mod', mod, member))
            touched.add(len(trees) - 1)
    for i in touched:
        with open(images[i], 'rb') as src:
            write_romfs(trees[i], src, images[i] + '.new')
        os.replace(images[i] + '.new', images[i])
    log(f'mods: {len(replaced)} RomFS files')
    return len(replaced)
