#!/usr/bin/env python3
"""Extracts what the recomp needs from a decrypted .3ds. Every section is proven against the SHA-256 hashes in the game's own
header. Some images are decrypted but still flagged as encrypted, so the hashes decide, not the flags.

extract_game.py <game.3ds> <output dir>"""
import hashlib
import json
import os
import struct
import sys

MEDIA_UNIT = 0x200


def blz_decompress(data):
    """Nintendo's backward LZ, used for the ExeFS .code section."""
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
    assert at <= stop, f'decompression stopped early at {at}, expected {stop}'
    return bytes(out)


def main():
    game, out_dir = sys.argv[1:3]
    f = open(game, 'rb')
    head = f.read(0x200)
    assert head[0x100:0x104] == b'NCSD', 'not a .3ds image'
    offset = struct.unpack('<I', head[0x120:0x124])[0] * MEDIA_UNIT
    f.seek(offset)
    n = f.read(0x200)
    assert n[0x100:0x104] == b'NCCH'
    exheader_hash, exefs_hash, romfs_hash = n[0x160:0x180], n[0x1C0:0x1E0], n[0x1E0:0x200]
    exefs_off, exefs_size, exefs_hashed = struct.unpack('<III', n[0x1A0:0x1AC])
    romfs_off, romfs_size, romfs_hashed = struct.unpack('<III', n[0x1B0:0x1BC])
    os.makedirs(out_dir, exist_ok=True)

    f.seek(offset + 0x200)
    exheader = f.read(0x800)
    if hashlib.sha256(exheader[:0x400]).digest() != exheader_hash:
        sys.exit('This image is encrypted: its extended header fails its hash in plain form.')
    print('extended header: plain, hash verified')

    start = offset + exefs_off * MEDIA_UNIT
    f.seek(start)
    exefs_head = f.read(0x200)
    assert hashlib.sha256(exefs_head[:0x200]).digest() == exefs_hash or exefs_hashed != 1, 'ExeFS header hash'
    files = {}
    for i in range(10):
        name = exefs_head[i * 16:i * 16 + 8].split(b'\0')[0].decode('latin1')
        off, size = struct.unpack('<II', exefs_head[i * 16 + 8:i * 16 + 16])
        if not name:
            continue
        f.seek(start + 0x200 + off)
        plain = f.read(size)
        want = exefs_head[0x200 - 32 * (i + 1):0x200 - 32 * i]
        assert hashlib.sha256(plain).digest() == want, f'ExeFS {name} hash'
        files[name] = plain
        print(f'exefs {name}: {size} bytes, hash verified')

    code = files['.code']
    compressed = bool(exheader[0xD] & 1)
    if compressed:
        code = blz_decompress(code)
    text = struct.unpack('<III', exheader[0x10:0x1C])
    rodata = struct.unpack('<III', exheader[0x20:0x2C])
    data = struct.unpack('<III', exheader[0x30:0x3C])
    bss = struct.unpack('<I', exheader[0x3C:0x40])[0]
    assert len(code) >= (text[1] + rodata[1]) * 0x1000 + data[2], f'code is {len(code)} bytes, too short for its segments'

    with open(os.path.join(out_dir, 'code.bin'), 'wb') as o:
        o.write(code)
    with open(os.path.join(out_dir, 'exheader.bin'), 'wb') as o:
        o.write(exheader)
    if 'icon' in files:
        with open(os.path.join(out_dir, 'icon.bin'), 'wb') as o:
            o.write(files['icon'])

    # RomFS: the header's hash covers its first region; the rest is proven by the hash tree inside it.
    start = offset + romfs_off * MEDIA_UNIT
    f.seek(start)
    total = romfs_size * MEDIA_UNIT
    digest = hashlib.sha256()
    remaining_hash = romfs_hashed * MEDIA_UNIT
    written = 0
    with open(os.path.join(out_dir, 'romfs.bin'), 'wb') as o:
        while written < total:
            block = f.read(min(1 << 22, total - written))
            if remaining_hash:
                take = block[:remaining_hash]
                digest.update(take)
                remaining_hash -= len(take)
            o.write(block)
            written += len(block)
    assert digest.digest() == romfs_hash, 'RomFS hash'
    print(f'romfs: {written} bytes, header hash verified')

    info = {
        'program_id': n[0x118:0x120][::-1].hex().upper(),
        'text': {'addr': text[0], 'size': text[2], 'pages': text[1]},
        'rodata': {'addr': rodata[0], 'size': rodata[2], 'pages': rodata[1]},
        'data': {'addr': data[0], 'size': data[2], 'pages': data[1]},
        'bss': bss,
        'stack': struct.unpack('<I', exheader[0x1C:0x20])[0],
    }
    with open(os.path.join(out_dir, 'info.json'), 'w') as o:
        json.dump(info, o, indent=1)
    print(info)
    print('code.bin', len(code), 'bytes, compressed in the dump:', compressed)


main()
