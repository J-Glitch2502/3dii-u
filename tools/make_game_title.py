#!/usr/bin/env python3
"""Assembles the game's title folder (code, content, meta) under build/title, for Cemu and the WUP packer.
Run with Windows Python from the project root: make_game_title.py [rpx]. Game files are hard links, so they take no extra disk."""
import os
import re
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TITLE = os.path.join(ROOT, 'build', 'title')
TEMPLATE = os.path.join(ROOT, 'phase0', 'title_code')
GAME = os.path.join(ROOT, 'games', 'tomodachi')
ARCHIVES = os.path.join(ROOT, 'games', 'system_archives')
TITLE_ID = '0005000010334467'
RPX_NAME = 'tomodachi.rpx'
rpx = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'tomodachi.rpx')


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        if os.path.samefile(src, dst):
            return
        os.remove(dst)
    os.link(src, dst)


shutil.rmtree(os.path.join(TITLE, 'content'), ignore_errors=True)
for d in ('code', 'content', 'meta'):
    os.makedirs(os.path.join(TITLE, d), exist_ok=True)

for name in ('app.xml', 'cos.xml'):
    s = open(os.path.join(TEMPLATE, 'code', name), encoding='utf-8', newline='').read()
    s = s.replace('0005000010334455', TITLE_ID).replace('bench.rpx', RPX_NAME).replace('>0A000000<', '>0E000000<')
    # as retail titles have it: Minecraft on the user's console, and a working inject
    s = s.replace('<overlay_arena type="hexBinary" length="4">00000001<', '<overlay_arena type="hexBinary" length="4">00000000<')
    open(os.path.join(TITLE, 'code', name), 'w', encoding='utf-8', newline='').write(s)
shutil.copyfile(rpx, os.path.join(TITLE, 'code', RPX_NAME))

# ---- art for the Wii U Menu, from the game's own icon. The TGA forms and sizes are the ones the menu expects.
def morton(x, y):
    r = 0
    for i in range(3):
        r |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return r


smdh = open(os.path.join(GAME, 'exefs', 'icon.bin'), 'rb').read()
icon = [[(0, 0, 0)] * 48 for _ in range(48)]
for ty in range(6):
    for tx in range(6):
        for y in range(8):
            for x in range(8):
                v = struct.unpack_from('<H', smdh, 0x24C0 + (ty * 6 + tx) * 128 + morton(x, y) * 2)[0]
                icon[ty * 8 + y][tx * 8 + x] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def write_tga(name, w, h, bpp, scale, background):
    x0, y0 = (w - 48 * scale) // 2, (h - 48 * scale) // 2
    pixels = bytearray()
    for y in range(h - 1, -1, -1):  # bottom-up
        for x in range(w):
            r, g, b = background
            if scale and x0 <= x < x0 + 48 * scale and y0 <= y < y0 + 48 * scale:
                r, g, b = icon[(y - y0) // scale][(x - x0) // scale]
            pixels += bytes((b, g, r))
            if bpp == 32:
                pixels.append(255)
    header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, bpp, 8 if bpp == 32 else 0)
    footer = bytes(8) + b'TRUEVISION-XFILE.\0'
    data = header + bytes(pixels) + footer + (bytes(4) if bpp == 24 else b'')
    with open(os.path.join(TITLE, 'meta', name + '.tga'), 'wb') as f:
        f.write(data)


BG = (0xFF, 0xF4, 0xD0)
write_tga('iconTex', 128, 128, 32, 2, BG)
write_tga('bootLogoTex', 170, 42, 32, 0, (0, 0, 0))
write_tga('bootDrcTex', 854, 480, 24, 6, BG)
write_tga('bootTvTex', 1280, 720, 24, 10, BG)
shutil.copyfile(os.path.join(TEMPLATE, 'meta', 'bootMovie.h264'), os.path.join(TITLE, 'meta', 'bootMovie.h264'))
meta = open(os.path.join(TEMPLATE, 'meta', 'meta.xml'), encoding='utf-8', newline='').read()
meta = meta.replace('0005000010334455', TITLE_ID).replace('3DS Recomp Code Test', 'Tomodachi Life').replace('3DS-WiiU Recomp', 'Nintendo (recompiled)')
meta = re.sub(r'WUP-N-[A-Z0-9]+', 'WUP-N-TMDF', meta)
# the room for saves: the game's in the player's own storage, /vol/save/<persistent id>, which the system lists as save data and keeps when the software is deleted, and
# the logs' and test runs' in /vol/save/common. With none, the console has nowhere to put them, which Cemu doesn't care about.
meta = meta.replace('<common_save_size type="hexBinary" length="8">0000000000000000<', '<common_save_size type="hexBinary" length="8">0000000004000000<')
meta = meta.replace('<account_save_size type="hexBinary" length="8">0000000000000000<', '<account_save_size type="hexBinary" length="8">0000000004000000<')
assert '<common_save_size type="hexBinary" length="8">0000000004000000<' in meta and '<account_save_size type="hexBinary" length="8">0000000004000000<' in meta
open(os.path.join(TITLE, 'meta', 'meta.xml'), 'w', encoding='utf-8', newline='').write(meta)

for f in ('exheader.bin', 'code.bin', 'romfs.bin'):
    link(os.path.join(GAME, f), os.path.join(TITLE, 'content', f))
link(os.path.join(GAME, 'exefs', 'icon.bin'), os.path.join(TITLE, 'content', 'exefs_icon.bin'))
for f in os.listdir(ARCHIVES):
    link(os.path.join(ARCHIVES, f), os.path.join(TITLE, 'content', f))
for f in ('shaders.gsh', 'shaders.idx'):
    shutil.copyfile(os.path.join(ROOT, 'build', 'shaders', f), os.path.join(TITLE, 'content', f))
print('title folder:', TITLE)
