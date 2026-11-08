"""Decrypting dumps for the builder, the way cia_to_decrypted_3ds.py does it: pyctr, with the keys from your own 3DS (boot9.bin, seeddb.bin) in a key folder,
~/.3ds by default (C:\\Users\\<you>\\.3ds on Windows).
- A game's or an update's CIA: makerom -ciatocci, then pyctr decrypts the .3ds it makes in place (convert_and_decrypt).
- An encrypted .3ds: pyctr in place, on a copy (decrypt_in_place), so the dump itself is never changed.
- A lone NCCH (.cxi, .app) and each content of a DLC CIA: pyctr's FullDecrypted.
What comes out is a plain dump, which dumps.py then checks against its own hashes. pyctr is loaded only when something needs decrypting."""
import os
import sys

KEYS_DEFAULT = os.path.join(os.path.expanduser('~'), '.3ds')


class DecryptError(Exception):
    pass


def use_keys(folder):
    """point pyctr at a key folder other than ~/.3ds (it reads BOOT9_PATH and SEEDDB_PATH, and its boot9 search list once loaded)"""
    if not folder or os.path.normcase(os.path.abspath(folder)) == os.path.normcase(KEYS_DEFAULT):
        return
    boot9, seeddb = os.path.join(folder, 'boot9.bin'), os.path.join(folder, 'seeddb.bin')
    if os.path.isfile(boot9):
        os.environ['BOOT9_PATH'] = boot9
        if 'pyctr.crypto.engine' in sys.modules:
            sys.modules['pyctr.crypto.engine'].b9_paths.insert(0, boot9)
    if os.path.isfile(seeddb):
        os.environ['SEEDDB_PATH'] = seeddb
        if 'pyctr.crypto.seeddb' in sys.modules:
            sys.modules['pyctr.crypto.seeddb'].load_seeddb(seeddb)


def have_keys(folder):
    return os.path.isfile(os.path.join(folder or KEYS_DEFAULT, 'boot9.bin'))


def converter():
    """the converter, with pyctr loaded (it stops the process when pyctr isn't installed)"""
    try:
        import cia_to_decrypted_3ds
    except SystemExit:
        raise DecryptError('Decrypting needs pyctr: run  pip install pyctr pycryptodome  once.') from None
    return cia_to_decrypted_3ds


def _reader(path):
    """pyctr's reader of a dump, and the NCCH of its program"""
    converter()
    from pyctr.type.cci import CCIReader
    from pyctr.type.cia import CIAReader
    from pyctr.type.ncch import NCCHReader
    with open(path, 'rb') as f:
        head = f.read(0x200)
    if head[:4] == b'\x20\x20\x00\x00':
        r = CIAReader(path)
        return r, r.contents[min(r.contents)]
    if head[0x100:0x104] == b'NCSD':
        r = CCIReader(path)
        return r, r.contents[0]
    r = NCCHReader(path)
    return r, r


def program_icon(path):
    """product code and SMDH of an encrypted dump's program, decrypted on the fly"""
    try:
        r, ncch = _reader(path)
        with r:
            smdh = b''
            if ncch.exefs and 'icon' in ncch.exefs.entries:
                with ncch.exefs.open('icon') as f:
                    smdh = f.read()
            return ncch.product_code, smdh
    except DecryptError:
        raise
    except Exception as e:
        raise DecryptError(f'pyctr couldn\'t read it: {e}. Check that boot9.bin and seeddb.bin from your own 3DS are in the key folder.') from None


def to_plain(path, out_base, kind='game'):
    """a plain copy of an encrypted game, update or lone NCCH, at out_base plus .3ds or .cxi. Returns its path.
    makerom only turns a game's CIA into a card image ("This CIA cannot be converted to CCI" for an update), so an update's program goes through
    pyctr alone, as DLC does."""
    conv = converter()
    with open(path, 'rb') as f:
        head = f.read(0x200)
    if head[:4] == b'\x20\x20\x00\x00' and kind in ('game', 'demo'):
        out = out_base + '.3ds'
        if not conv.convert_and_decrypt(path, out):
            raise DecryptError('Decrypting this CIA failed: the log above says where.')
        return out
    if head[0x100:0x104] == b'NCSD':
        import shutil
        out = out_base + '.3ds'
        shutil.copyfile(path, out)
        if not (conv.decrypt_in_place(out) and conv.verify(out)):
            raise DecryptError('Decrypting this .3ds failed: the log above says where.')
        return out
    out = out_base + '.cxi'
    _full_decrypted(path, None, out)
    return out


def _full_decrypted(path, index, out):
    try:
        r, ncch = _reader(path)
        with r:
            if index is not None:
                ncch = r.contents[index]
            from pyctr.type.ncch import NCCHSection
            s = ncch.open_raw_section(NCCHSection.FullDecrypted)
            with open(out, 'wb') as o:
                while chunk := s.read(4 << 20):
                    o.write(chunk)
    except DecryptError:
        raise
    except Exception as e:
        raise DecryptError(f'pyctr couldn\'t decrypt it: {e}') from None


def dlc_contents(path, out_dir):
    """every content of a DLC CIA, decrypted, as plain NCCH files: {content index: path}"""
    converter()
    from pyctr.type.cia import CIAReader
    try:
        with CIAReader(path) as r:
            indices = sorted(r.contents)
    except Exception as e:
        raise DecryptError(f'pyctr couldn\'t read this DLC: {e}') from None
    out = {}
    for i in indices:
        out[i] = os.path.join(out_dir, f'dlc_content_{i:08x}.ncch')
        _full_decrypted(path, i, out[i])
    return out
