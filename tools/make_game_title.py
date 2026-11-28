#!/usr/bin/env python3
"""Assembles a game's Wii U title folder (code, content, meta), for Cemu and the WUP packer. Game files are hard links, so they take no extra disk.
tools/win/build3ds.py and builder/pipeline.py call make_title() for every game. By hand:
    make_game_title.py <title folder> <game folder> <rpx> <shaders folder> <title id> <name> <product code> [system archives folder]"""
import math
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


def resample_taps(src, dst, a=3):
    """For each of dst pixels across: the src pixels it takes, with their Lanczos-a weights, and the two src pixels it lies between."""
    taps = []
    for o in range(dst):
        c = (o + 0.5) * src / dst - 0.5
        f = math.floor(c)
        ws = [(i, 1.0 if c == i else a * math.sin(math.pi * (c - i)) * math.sin(math.pi * (c - i) / a) / (math.pi * (c - i)) ** 2)
              for i in range(f - a + 1, f + a + 1)]
        total = sum(w for _, w in ws)
        taps.append(([(min(max(i, 0), src - 1), w / total) for i, w in ws], min(max(f, 0), src - 1), min(max(f + 1, 0), src - 1)))
    return taps


def sharp_icon(icon, size=128):
    """The 48x48 icon at size x size, as sharp as its pixels allow: Lanczos-3, a light unsharp mask, and each pixel kept within the four it
    lies between, so edges come out crisp without halos. Pure Python, as the Builder has no numpy or Pillow: a quarter of a second."""
    tx, ty = resample_taps(len(icon[0]), size), resample_taps(len(icon), size)
    across = [[[sum(row[i][ch] * k for i, k in taps) for ch in range(3)] for taps, _, _ in tx] for row in icon]
    up = [[[sum(across[i][x][ch] * k for i, k in taps) for ch in range(3)] for x in range(size)] for taps, _, _ in ty]

    def blur(line):  # [1 2 1] / 4 along a line, its ends repeated
        n = len(line)
        return [[(line[max(j - 1, 0)][ch] + 2 * line[j][ch] + line[min(j + 1, n - 1)][ch]) / 4 for ch in range(3)] for j in range(n)]
    rows = [blur(line) for line in up]
    soft = [blur([rows[y][x] for y in range(size)]) for x in range(size)]  # soft[x][y]
    out = []
    for y in range(size):
        y0, y1 = ty[y][1], ty[y][2]
        line = []
        for x in range(size):
            x0, x1 = tx[x][1], tx[x][2]
            near = (icon[y0][x0], icon[y0][x1], icon[y1][x0], icon[y1][x1])
            line.append(tuple(min(max(round(1.5 * up[y][x][ch] - 0.5 * soft[x][y][ch]), min(p[ch] for p in near)), max(p[ch] for p in near))
                              for ch in range(3)))
        out.append(line)
    return out


def vc_icon_pixels(icon):
    """Each game's SMDH under Walegy's VC-inspired bands (issue #45); no game art is bundled."""
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QImage, QPainter
    data = bytes(channel for row in sharp_icon(icon) for pixel in row for channel in pixel)
    canvas = QImage(data, 128, 128, 128 * 3, QImage.Format_RGB888).copy()  # its own pixels: data goes when this returns
    frame = QImage(os.path.join(os.path.dirname(__file__), 'art', 'vc_icon_frame.png'))
    if frame.isNull():
        raise ValueError('The shared Virtual Console icon frame is missing or unreadable')
    painter = QPainter(canvas)
    painter.drawImage(0, 0, frame.scaled(128, 128, Qt.IgnoreAspectRatio, Qt.SmoothTransformation))
    painter.end()
    stride, data = canvas.bytesPerLine(), bytes(canvas.constBits())
    return [[tuple(data[y * stride + 3 * x:y * stride + 3 * x + 3]) for x in range(128)] for y in range(128)]


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


def smdh_names(smdh_path):
    """the game's long name and its publisher from its SMDH: the English ones, or the Japanese where a game has no English"""
    smdh = open(smdh_path, 'rb').read()
    for lang in (1, 0):
        at = 8 + 0x200 * lang
        text = lambda o, n: smdh[at + o:at + o + n].decode('utf-16-le', 'replace').split('\0')[0]
        name = ' '.join(text(0x80, 0x100).split())
        if name:
            return name, text(0x180, 0x80).strip()
    return '', ''


_qt = None  # the application Qt draws text with, made once


# the console behind the Virtual Console's screens: the user's photo of a 3DS (2026-11-16), its screens blanked, cut out and faded to
# light greys as the Virtual Console shows a DS (made once from the photo: the png's alpha is the console)
CONSOLE_ART = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'art', 'vc_console_3ds.png')


def console_screens(art, smdh_path, released='', case_scan=False, top_screen=False, icon=None):
    """The start-up screens as the Wii U's Virtual Console draws a DS game's (the user's picture of one, 2026-11-14): "Virtual Console" at the
    top left, a large faint 3DS behind it all on the right (CONSOLE_ART), and across the middle a white band holding the game's picture, the
    console's name, the game's name from its SMDH, and released ("Released in Europe: 2011", say), or its publisher without one. The picture
    is the game's title screen where there is one, as the Virtual Console shows (top_screen: libretro's picture of both screens, one above the
    other, of which the top one shows), else its art or SMDH icon. A GameTDB cover (case_scan: 768x680, the case's front) gives the case without
    the strip with the console's name down its side, shown whole. Returns both boot textures, or None if no usable art or icon was supplied."""
    global _qt
    from PySide6.QtCore import QRectF, Qt
    from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QLinearGradient, QPainter, QPainterPath, QPen
    _qt = QGuiApplication.instance() or QGuiApplication([sys.argv[0]])
    cover = QImage(art) if art else QImage()
    if cover.isNull():
        if icon is None:
            return None
        pixels = bytes(channel for row in icon for pixel in row for channel in pixel)
        cover = QImage(pixels, 48, 48, 48 * 3, QImage.Format_RGB888)
        case_scan = False
    if case_scan:  # a GameTDB case scan: its white strip down the right goes, and the whole height stays (a logo can sit low on it)
        cover = cover.copy(0, 0, 684, 680)
    if top_screen and cover.height() > cover.width() * 0.9:  # both screens, the bottom one under the top: the top one, 5:3, alone
        cover = cover.copy(0, 0, cover.width(), round(cover.width() * 3 / 5))
    name, publisher = smdh_names(smdh_path)
    img = QImage(1280, 720, QImage.Format_RGB888)
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing)
    p.setRenderHint(QPainter.SmoothPixmapTransform)

    def text(x, y, w, h, px, rgb, words, weight=QFont.Normal, italic=False, spacing=0.0):
        f = QFont('Segoe UI')
        f.setPixelSize(px)
        f.setWeight(weight)
        f.setItalic(italic)
        f.setLetterSpacing(QFont.AbsoluteSpacing, spacing)
        p.setFont(f)
        p.setPen(QColor(*rgb))
        p.drawText(QRectF(x, y, w, h), Qt.AlignLeft | Qt.AlignVCenter | Qt.TextWordWrap, words)
        return p.fontMetrics().horizontalAdvance(words)

    bg = QLinearGradient(0, 0, 0, 720)
    bg.setColorAt(0, QColor(247, 247, 247))
    bg.setColorAt(1, QColor(212, 212, 212))
    p.fillRect(0, 0, 1280, 720, bg)
    # the console, faded, behind everything on the right and running off the edge
    console = QImage(CONSOLE_ART)
    if not console.isNull():
        p.drawImage(QRectF(742, 22, 744, 744 * console.height() / console.width()), console)
    # the band across the middle, on a soft shadow
    for k in range(14):
        p.fillRect(QRectF(0, 564 + k, 1280, 1), QColor(0, 0, 0, 34 - 34 * k // 14))
    p.fillRect(QRectF(0, 180, 1280, 384), QColor(255, 255, 255, 236))
    p.fillRect(QRectF(0, 180, 1280, 2), QColor(255, 255, 255))
    # the game's art, filling its frame, vertically centred in the band
    if case_scan:  # shown whole: its own shape, inside the band with a margin
        scale = min(344 / cover.width(), 344 / cover.height())
        frame = QRectF(72, 372 - cover.height() * scale / 2, cover.width() * scale, cover.height() * scale)
    else:
        height = max(200.0, min(300.0, 344 * cover.height() / cover.width()))  # the art's own shape, within reason
        frame = QRectF(72, 372 - height / 2, 344, height)
    fit = cover.scaled(int(frame.width()), int(frame.height()), Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation)
    clip = QPainterPath()
    clip.addRoundedRect(frame, 8, 8)
    p.save()
    p.setClipPath(clip)
    p.drawImage(frame, fit, QRectF((fit.width() - frame.width()) / 2, 0, frame.width(), frame.height()))
    p.restore()
    p.setPen(QPen(QColor(200, 200, 200), 1.5))
    p.drawPath(clip)
    # the console's name, the game's, and when it came out or who made it
    shift = text(552, 262, 300, 70, 30, (88, 88, 88), 'NINTENDO', QFont.Light, spacing=3.0)
    text(552 + shift + 16, 250, 300, 80, 56, (58, 58, 58), '3DS', QFont.Bold)
    text(552, 344, 690, 104, 40, (52, 52, 52), name)
    text(552, 456, 690, 48, 26, (84, 84, 84), released or publisher)
    text(52, 48, 600, 70, 46, (156, 156, 156), 'Virtual Console', italic=True)
    p.end()

    def rows(image):
        stride, data = image.bytesPerLine(), bytes(image.constBits())
        return [[tuple(data[y * stride + 3 * x:y * stride + 3 * x + 3]) for x in range(image.width())] for y in range(image.height())]
    return {'bootTvTex.tga': rows(img), 'bootDrcTex.tga': rows(img.scaled(854, 480, Qt.IgnoreAspectRatio, Qt.SmoothTransformation))}


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


PAD_W, PAD_H = 854, 480  # the GamePad's screen


def bezel_pixels(path):
    """A picture for the GamePad where it shows no screen (gx2_backend.cpp, pad_bezel), filling 854x480 (scaled to cover it, centred, the
    overflow cut off) over black, as RGBA bytes row by row; None if it can't be read. Read with Qt, which the builder carries."""
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QColor, QImage, QPainter
    src = QImage(path)
    if src.isNull():
        return None
    cover = src.scaled(PAD_W, PAD_H, Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation)
    canvas = QImage(PAD_W, PAD_H, QImage.Format_RGBA8888)
    canvas.fill(QColor(0, 0, 0))  # what a picture leaves see-through is black, as without one
    painter = QPainter(canvas)
    painter.drawImage(0, 0, cover, (cover.width() - PAD_W) // 2, (cover.height() - PAD_H) // 2, PAD_W, PAD_H)
    painter.end()
    stride, data = canvas.bytesPerLine(), bytes(canvas.constBits())
    return b''.join(data[y * stride:y * stride + PAD_W * 4] for y in range(PAD_H))


def make_title(title, game, rpx, shaders, title_id, name, product, archives=ARCHIVES, art=None, icon_art=None, style='picture', released='', case_scan=False,
               pad_bezel=None, pad_art=None, boot_sound=None, screen_art=None, *, include_shaders=True):
    """title: the folder to make. game: the unpacked game (exheader.bin, code.bin, romfs.bin, exefs/icon.bin). rpx and shaders: what the build made.
    art: a picture for the start-up screens. The Wii U Menu icon uses the game's SMDH with Virtual Console bands unless icon_art supplies
    a complete custom icon (shown as supplied, without bands). style: 'picture' shows art as it is on the start-up screens,
    'console' lays it out as the Wii U's Virtual Console shows a game (console_screens), with released ("Released in Europe: 2011") under its name; case_scan says art is a GameTDB case scan.
    pad_art: a picture of its own for the GamePad's start-up screen, shown as it is. boot_sound: the game's own start-up sound (a .btsnd,
    tools/make_boot_sound.py), in place of the one this PC has for every game. screen_art: the game's title screen (covers.title_screen), the
    Virtual Console's picture in its screens in place of art."""
    # Reject a title's content directory passed as system archives. Copying it
    # after the game files would silently replace this game's code and header.
    reserved = {'exheader.bin', 'code.bin', 'romfs.bin', 'exefs_icon.bin', 'shaders.gsh', 'shaders.idx', 'live.txt'}
    collisions = reserved.intersection(os.listdir(archives))
    if collisions:
        raise ValueError('system archives folder contains game content: ' + ', '.join(sorted(collisions)))
    if include_shaders and not os.path.isfile(os.path.join(shaders, 'fallback-shaders.bin')):
        raise ValueError('Optimized fallback shader pack is missing; rebuild shaders with the current Builder')
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
    write_art_tga(os.path.join(meta_dir, 'iconTex.tga'), vc_icon_pixels(icon), 32)
    write_tga(os.path.join(meta_dir, 'bootLogoTex.tga'), icon, 170, 42, 32, 0, (0, 0, 0))
    write_tga(os.path.join(meta_dir, 'bootDrcTex.tga'), icon, 854, 480, 24, 6, BACKGROUND)
    write_tga(os.path.join(meta_dir, 'bootTvTex.tga'), icon, 1280, 720, 24, 10, BACKGROUND)
    screens = None
    if style == 'console':
        if screen_art:
            screens = console_screens(screen_art, icon_bin, released, top_screen=True)
        if screens is None:
            screens = console_screens(art, icon_bin, released, case_scan, icon=icon)
    for tga, w, h, bpp, picture in (('iconTex.tga', 128, 128, 32, icon_art), ('bootDrcTex.tga', 854, 480, 24, art), ('bootTvTex.tga', 1280, 720, 24, art)):
        rows = screens[tga] if screens and tga in screens else art_pixels(picture, w, h, (0, 0, 0)) if picture else None
        if rows:
            write_art_tga(os.path.join(meta_dir, tga), rows, bpp)
        elif picture:
            print(f'the picture {picture} could not be read: the game\'s own icon is used for {tga}')
    pad = art_pixels(pad_art, PAD_W, PAD_H, (0, 0, 0)) if pad_art else None
    if pad:  # the GamePad's start-up screen of its own, over the one made above
        write_art_tga(os.path.join(meta_dir, 'bootDrcTex.tga'), pad, 24)
    elif pad_art:
        print(f"the picture {pad_art} could not be read: the GamePad shows the TV's start-up screen")
    # the sound while the game starts: its own, or the one this PC has for every game (tools/make_boot_sound.py); a title without either starts silently
    sound, every_game = None, os.path.join(os.path.dirname(os.path.abspath(archives)), 'boot_sound.btsnd')
    if boot_sound:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import make_boot_sound  # checks a .btsnd, and makes one of a WAV
        try:
            sound = make_boot_sound.boot_sound(boot_sound)
        except (OSError, ValueError, EOFError) as e:
            print(f"the sound {boot_sound} can't be used ({e}): the one for every game is used")
    if sound is None and os.path.isfile(every_game):
        sound = open(every_game, 'rb').read()
    if sound is not None:
        with open(os.path.join(meta_dir, 'bootSound.btsnd'), 'wb') as f:
            f.write(sound)
    elif os.path.exists(os.path.join(meta_dir, 'bootSound.btsnd')):
        os.remove(os.path.join(meta_dir, 'bootSound.btsnd'))
    shutil.copyfile(os.path.join(TEMPLATE, 'meta', 'bootMovie.h264'), os.path.join(meta_dir, 'bootMovie.h264'))
    meta = open(os.path.join(TEMPLATE, 'meta', 'meta.xml'), encoding='utf-8', newline='').read()
    xml_name = name.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')
    meta = meta.replace('0005000010334455', title_id).replace('3DS Recomp Code Test', xml_name).replace('3DS-WiiU Recomp', 'Nintendo (recompiled)')
    meta = re.sub(r'WUP-N-[A-Z0-9]+', lambda m: product, meta)
    # the room for saves: the game's in the player's own storage, /vol/save/<persistent id>, which the system lists as save data and keeps when the software is deleted, and
    # the logs' and test runs' in /vol/save/common. With none, the console has nowhere to put them, which Cemu doesn't care about.
    # Native save quotas reserve real storage. Start small; the runtime grows
    # them through SAVEUpdateSaveDir when a pending write needs more space.
    meta = meta.replace('<common_save_size type="hexBinary" length="8">0000000000000000<', '<common_save_size type="hexBinary" length="8">0000000001000000<')
    # Mario Maker creates 221 course files of up to 256 KiB each. Leave room
    # for filesystem overhead and rewrites during save quota flushes.
    account_save_size = 0x04000000 if title_id.upper() in ('000500003D001A04', '000500001D1A0400') else 0x00800000
    account_save = f'<account_save_size type="hexBinary" length="8">{account_save_size:016X}<'
    meta = meta.replace('<account_save_size type="hexBinary" length="8">0000000000000000<', account_save)
    assert '<common_save_size type="hexBinary" length="8">0000000001000000<' in meta and account_save in meta
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
    for f in (('shaders.gsh', 'shaders.idx') if include_shaders else ()):
        shutil.copyfile(os.path.join(shaders, f), os.path.join(content, f))
    if include_shaders:
        optimized = os.path.join(shaders, 'fallback-shaders.bin')
        shutil.copyfile(optimized, os.path.join(content, 'fallback-shaders.bin'))
    bezel = bezel_pixels(pad_bezel) if pad_bezel else None
    if bezel:  # the GamePad's picture where no screen is: the console reads its bytes as they are
        with open(os.path.join(content, 'pad_bezel.rgba'), 'wb') as f:
            f.write(bezel)
    elif pad_bezel:
        print(f'the picture {pad_bezel} could not be read: the GamePad stays black beside the screens')
    print('title folder:', title)


if __name__ == '__main__':
    if len(sys.argv) not in (8, 9):
        sys.exit(__doc__)
    make_title(*sys.argv[1:])
