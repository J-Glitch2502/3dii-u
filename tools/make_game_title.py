#!/usr/bin/env python3
"""Assembles a game's Wii U title folder (code, content, meta), for Cemu and the WUP packer. Game files are hard links, so they take no extra disk.
tools/win/build3ds.py and builder/pipeline.py call make_title() for every game. By hand:
    make_game_title.py <title folder> <game folder> <rpx> <shaders folder> <title id> <name> <product code> [system archives folder]"""
import os
import re
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, 'phase0', 'title_code')
ARCHIVES = os.path.join(ROOT, 'games', 'system_archives')
BACKGROUND = (0xFF, 0xF4, 0xD0)


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        if os.path.samefile(src, dst):
            return
        os.remove(dst)
    os.link(src, dst)


def morton(x, y):
    r = 0
    for i in range(3):
        r |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return r


def icon_pixels(smdh_path):
    """the game's own 48x48 icon from its SMDH: RGB565 in 8x8 tiles, each tile in Morton order"""
    smdh = open(smdh_path, 'rb').read()
    icon = [[(0, 0, 0)] * 48 for _ in range(48)]
    for ty in range(6):
        for tx in range(6):
            for y in range(8):
                for x in range(8):
                    v = struct.unpack_from('<H', smdh, 0x24C0 + (ty * 6 + tx) * 128 + morton(x, y) * 2)[0]
                    icon[ty * 8 + y][tx * 8 + x] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    return icon


def write_tga(path, icon, w, h, bpp, scale, background):
    """the icon scaled up and centred on a plain background, in the TGA forms and sizes the Wii U Menu expects"""
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
    with open(path, 'wb') as f:
        f.write(header + bytes(pixels) + TGA_FOOTER + (bytes(4) if bpp == 24 else b''))


TGA_FOOTER = bytes(8) + b'TRUEVISION-XFILE.\0'  # the console's loader wants it; Cemu doesn't care


SHARED_FONT = '0004009b00014002'  # the system archive with the font games write their text in


def unpacked_shared_font(archive):
    """The shared font out of its system archive, decompressed: 3 MB the console would otherwise unpack from 1.4 MB every time a game starts.
    None if the archive isn't there or holds no font."""
    if not os.path.isfile(archive):
        return None
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import dvlb_to_log  # its romfs listing and its LZ11
    with open(archive, 'rb') as f:
        data = f.read()
    for offset, _ in dvlb_to_log.romfs_files(data):
        font = dvlb_to_log.lz(data, offset)
        if font and font[:4] == b'CFNT':
            return font
    return None


def art_pixels(path, w, h, background):
    """A picture (cover art, say) as rows of (r, g, b), scaled to fit w x h and centred on the background. Read with Qt, which the builder
    carries; None if it can't be read."""
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QColor, QImage, QPainter
    src = QImage(path)
    if src.isNull():
        return None
    fit = src.scaled(w, h, Qt.KeepAspectRatio, Qt.SmoothTransformation)
    canvas = QImage(w, h, QImage.Format_RGB888)
    canvas.fill(QColor(*background))
    painter = QPainter(canvas)
    painter.drawImage((w - fit.width()) // 2, (h - fit.height()) // 2, fit)
    painter.end()
    stride, data = canvas.bytesPerLine(), bytes(canvas.constBits())
    return [[tuple(data[y * stride + 3 * x:y * stride + 3 * x + 3]) for x in range(w)] for y in range(h)]


def write_art_tga(path, rows, bpp):
    """rows of (r, g, b), top first, as the TGA the Wii U Menu expects (write_tga's layout)"""
    h, w = len(rows), len(rows[0])
    pixels = bytearray()
    for row in reversed(rows):  # bottom-up
        for r, g, b in row:
            pixels += bytes((b, g, r, 255) if bpp == 32 else (b, g, r))
    header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, bpp, 8 if bpp == 32 else 0)
    with open(path, 'wb') as f:
        f.write(header + bytes(pixels) + TGA_FOOTER + (bytes(4) if bpp == 24 else b''))


def make_title(title, game, rpx, shaders, title_id, name, product, archives=ARCHIVES, art=None):
    """title: the folder to make. game: the unpacked game (exheader.bin, code.bin, romfs.bin, exefs/icon.bin). rpx and shaders: what the build made.
    art: a picture for the start-up screens and the Wii U Menu icon, in place of the game's own 48x48 icon scaled up."""
    rpx_name = os.path.basename(rpx)
    shutil.rmtree(os.path.join(title, 'content'), ignore_errors=True)
    for d in ('code', 'content', 'meta'):
        os.makedirs(os.path.join(title, d), exist_ok=True)

    for xml in ('app.xml', 'cos.xml'):
        s = open(os.path.join(TEMPLATE, 'code', xml), encoding='utf-8', newline='').read()
        s = s.replace('0005000010334455', title_id).replace('bench.rpx', rpx_name).replace('>0A000000<', '>0E000000<')
        # as retail titles have it: Minecraft on the user's console, and a working inject
        s = s.replace('<overlay_arena type="hexBinary" length="4">00000001<', '<overlay_arena type="hexBinary" length="4">00000000<')
        open(os.path.join(title, 'code', xml), 'w', encoding='utf-8', newline='').write(s)
    shutil.copyfile(rpx, os.path.join(title, 'code', rpx_name))

    # art for the Wii U Menu, from the game's own icon. tools/extract_game.py puts it beside code.bin, the Builder in exefs/
    icon_bin = os.path.join(game, 'exefs', 'icon.bin')
    if not os.path.isfile(icon_bin):
        icon_bin = os.path.join(game, 'icon.bin')
    icon = icon_pixels(icon_bin)
    meta_dir = os.path.join(title, 'meta')
    write_tga(os.path.join(meta_dir, 'iconTex.tga'), icon, 128, 128, 32, 2, BACKGROUND)
    write_tga(os.path.join(meta_dir, 'bootLogoTex.tga'), icon, 170, 42, 32, 0, (0, 0, 0))
    write_tga(os.path.join(meta_dir, 'bootDrcTex.tga'), icon, 854, 480, 24, 6, BACKGROUND)
    write_tga(os.path.join(meta_dir, 'bootTvTex.tga'), icon, 1280, 720, 24, 10, BACKGROUND)
    if art:
        for tga, w, h, bpp in (('iconTex.tga', 128, 128, 32), ('bootDrcTex.tga', 854, 480, 24), ('bootTvTex.tga', 1280, 720, 24)):
            rows = art_pixels(art, w, h, (0, 0, 0))
            if rows is None:
                print(f'the banner picture {art} could not be read: the game\'s own icon is used')
                break
            write_art_tga(os.path.join(meta_dir, tga), rows, bpp)
    # the sound while the game starts, if this PC has one (tools/make_boot_sound.py); a title without the file starts silently
    boot_sound = os.path.join(os.path.dirname(os.path.abspath(archives)), 'boot_sound.btsnd')
    if os.path.isfile(boot_sound):
        shutil.copyfile(boot_sound, os.path.join(meta_dir, 'bootSound.btsnd'))
    elif os.path.exists(os.path.join(meta_dir, 'bootSound.btsnd')):
        os.remove(os.path.join(meta_dir, 'bootSound.btsnd'))
    shutil.copyfile(os.path.join(TEMPLATE, 'meta', 'bootMovie.h264'), os.path.join(meta_dir, 'bootMovie.h264'))
    meta = open(os.path.join(TEMPLATE, 'meta', 'meta.xml'), encoding='utf-8', newline='').read()
    xml_name = name.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')
    meta = meta.replace('0005000010334455', title_id).replace('3DS Recomp Code Test', xml_name).replace('3DS-WiiU Recomp', 'Nintendo (recompiled)')
    meta = re.sub(r'WUP-N-[A-Z0-9]+', lambda m: product, meta)
    # the room for saves: the game's in the player's own storage, /vol/save/<persistent id>, which the system lists as save data and keeps when the software is deleted, and
    # the logs' and test runs' in /vol/save/common. With none, the console has nowhere to put them, which Cemu doesn't care about.
    meta = meta.replace('<common_save_size type="hexBinary" length="8">0000000000000000<', '<common_save_size type="hexBinary" length="8">0000000004000000<')
    meta = meta.replace('<account_save_size type="hexBinary" length="8">0000000000000000<', '<account_save_size type="hexBinary" length="8">0000000004000000<')
    assert '<common_save_size type="hexBinary" length="8">0000000004000000<' in meta and '<account_save_size type="hexBinary" length="8">0000000004000000<' in meta
    open(os.path.join(meta_dir, 'meta.xml'), 'w', encoding='utf-8', newline='').write(meta)

    content = os.path.join(title, 'content')
    for f in ('exheader.bin', 'code.bin', 'romfs.bin'):
        link(os.path.join(game, f), os.path.join(content, f))
    link(icon_bin, os.path.join(content, 'exefs_icon.bin'))
    for f in os.listdir(archives):
        link(os.path.join(archives, f), os.path.join(content, f))
    font = unpacked_shared_font(os.path.join(archives, SHARED_FONT + '.romfs'))
    if font:  # the console reads it as it is, with nothing to search or decompress while the game starts (services/misc.cpp)
        with open(os.path.join(content, 'shared_font.bcfnt'), 'wb') as f:
            f.write(font)
    for f in ('shaders.gsh', 'shaders.idx'):
        shutil.copyfile(os.path.join(shaders, f), os.path.join(content, f))
    print('title folder:', title)


if __name__ == '__main__':
    if len(sys.argv) not in (8, 9):
        sys.exit(__doc__)
    make_title(*sys.argv[1:])
