#!/usr/bin/env python3
"""Prepare the supported USA Rev 1 60 FPS mod for static recompilation.

Supply your own extracted game and upstream code.ips. No game files or patch
payloads are distributed here. Output must be a new directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import os
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'builder'))
import dumps
import mods

CODE_HASH = 'c705711154b1c514d7a0b5d133fabff42834110b198bd4cf86397d0d1c1597e9'
HEADER_HASH = '01f86be6de4921dfda7aa74c390ab3243ba981482140bc8efb0c62fc51697b70'
PATCH_HASH = '6cdc315748eb2eb1461ed5447bb9fdb65b195d49f4237bd760bbf5166bd2004f'
UPSTREAM = 'https://github.com/AlonBorodAHub/mario-3d-land-60fps'
REVISION = 'eab11f6cc03c9df6ba06cd56af1ac28e7cf64a09'
HOOKS = (0x11abf8, 0x129064, 0x129350, 0x13e21c, 0x162e80,
         0x1dcba0, 0x224ae0, 0x265b88, 0x29d9c8, 0x2a0404,
         0x2a073c, 0x2afd24, 0x3187b4, 0x3358c0)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(game, patch, output):
    code = (game / 'code.bin').read_bytes()
    header = (game / 'exheader.bin').read_bytes()
    ips = patch.read_bytes()
    for label, data, expected in (('game code', code, CODE_HASH),
                                  ('exheader', header, HEADER_HASH),
                                  ('IPS patch', ips, PATCH_HASH)):
        if digest(data) != expected:
            raise ValueError(f'Unsupported {label}; expected the documented USA Rev 1/mod revision')
    if output.exists():
        raise ValueError('Output must be a new directory')
    patched = mods.apply_ips(code, ips)
    header = bytearray(header)
    # The upstream installer expands BSS to fit its private interpolation cache.
    struct.pack_into('<I', header, 0x3c, 0x7e684)
    text_addr = struct.unpack_from('<I', header, 0x10)[0]
    ro_addr = struct.unpack_from('<I', header, 0x20)[0]
    data_addr, _, data_size = struct.unpack_from('<III', header, 0x30)
    bss = struct.unpack_from('<I', header, 0x3c)[0]
    data_end = 0x4320c0
    new_size = data_end - data_addr
    assert text_addr == 0x100000 and ro_addr == 0x3a2000
    assert 0 <= new_size - data_size <= bss
    assert len(patched) == data_end - text_addr
    assert struct.unpack_from('<I', patched, 0x432000-text_addr)[0] == 0x4d363050
    # The hook island occupies existing text-page padding. Explicit coverage
    # is needed by both Image.in_text and the runtime segment loader.
    struct.pack_into('<I', header, 0x18, ro_addr-text_addr)
    # Load initialized cache bytes instead of clearing them as BSS. Keep the
    # original total data+BSS footprint, including zeroed old BSS variables.
    struct.pack_into('<III', header, 0x34, (new_size+4095)//4096,
                     new_size, bss-(new_size-data_size))
    shutil.copytree(game, output, ignore=shutil.ignore_patterns(
        'romfs.bin', 'gen_*', 'out', '*.o', '*.rpx', '*.elf', '*.wuhb'))
    romfs = game / 'romfs.bin'
    if romfs.exists():
        try:
            os.link(romfs, output / 'romfs.bin')
        except OSError:
            shutil.copyfile(romfs, output / 'romfs.bin')
    (output / 'code.bin').write_bytes(patched)
    (output / 'exheader.bin').write_bytes(header)
    (output / 'info.json').write_text(json.dumps(dumps.program_info(header), indent=2)+'\n')
    entries = output / 'functions.txt'
    prior = entries.read_text() if entries.exists() else ''
    entries.write_text(prior.rstrip()+'\n'+''.join(f'0x{a:08x}\n' for a in HOOKS))
    receipt = {'upstream': UPSTREAM, 'revision': REVISION,
               'original_code_sha256': CODE_HASH, 'ips_sha256': PATCH_HASH,
               'adapted_code_sha256': digest(patched),
               'adapted_header_sha256': digest(header),
               'render_target_hz': 60, 'simulation_hz': 30,
               'romfs_unchanged': True, 'hooks': [hex(a) for a in HOOKS]}
    (output / '60fps-preparation.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path, help='Unmodified extracted USA Rev 1 game directory')
    ap.add_argument('patch', type=Path, help='Upstream patch/code.ips')
    ap.add_argument('output', type=Path, help='New extracted-game directory for the normal builder')
    args = ap.parse_args()
    try:
        result = prepare(args.game, args.patch, args.output)
    except (OSError, ValueError, mods.ModError) as error:
        ap.exit(1, f'Preparation failed: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
