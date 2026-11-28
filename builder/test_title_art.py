"""Self-check for the start-up screens in the Virtual Console's manner (tools/make_game_title.py, console_screens): their sizes, the band and the
art where they go, a GameTDB case scan's strip left out and its whole height kept, the release line, and the names read from a made-up SMDH;
and the GamePad's bezel picture (bezel_pixels): its size, scale, cut, row order and see-through parts; a whole title's start-up screens and
sound (make_title), with a GamePad picture and a sound of the game's own or without. Run: python builder/test_title_art.py"""
import os
import struct
import sys
import tempfile

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')  # no window needed; the text isn't checked
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import make_game_title as title  # noqa: E402
from PySide6.QtGui import QColor, QImage, QPainter  # noqa: E402


def smdh(english, publisher):
    """an SMDH with a Japanese name, an English one if given, and a green 48x48 icon"""
    d = bytearray(0x36C0)
    d[:4] = b'SMDH'
    for lang, name, pub in ((0, 'ゲーム', 'P-ja'), (1, english, publisher)):
        at = 8 + 0x200 * lang
        for off, text in ((0x80, name), (0x180, pub)):
            raw = text.encode('utf-16-le')
            d[at + off:at + off + len(raw)] = raw
    for i in range(48 * 48):
        struct.pack_into('<H', d, 0x24C0 + 2 * i, 0x07E0)
    return bytes(d)


def near(rgb, want, slack=40):
    return all(abs(a - b) <= slack for a, b in zip(rgb, want))


BLUE, WHITE, GREEN = (20, 40, 220), (255, 255, 255), (20, 200, 20)
with tempfile.TemporaryDirectory() as tmp:
    icon, japanese = os.path.join(tmp, 'icon.bin'), os.path.join(tmp, 'ja.bin')
    open(icon, 'wb').write(smdh('Some\nGame  Name', ' Maker '))
    open(japanese, 'wb').write(smdh('', ''))
    assert title.smdh_names(icon) == ('Some Game Name', 'Maker'), title.smdh_names(icon)
    assert title.smdh_names(japanese) == ('ゲーム', 'P-ja'), title.smdh_names(japanese)

    # The common bands leave the game's own image visible, in the right orientation, for any SMDH.
    quadrants = [[((255, 0, 0) if x < 24 else (0, 255, 0)) if y < 24 else
                  ((0, 0, 255) if x < 24 else (255, 255, 0)) for x in range(48)] for y in range(48)]
    framed = title.vc_icon_pixels(quadrants)
    assert (len(framed), len(framed[0])) == (128, 128)
    assert [framed[y][x] for x, y in ((16, 40), (112, 40), (16, 88), (112, 88))] == [
        (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]
    assert 0 < framed[2][2][0] < 255 and 0 < framed[126][126][0] < 255, 'both bands must darken the game art'
    assert any(min(pixel) > 220 for row in framed[109:120] for pixel in row), 'the footer label is missing'
    assert any(min(pixel) > 200 for row in framed[6:13] for pixel in row[9:68]), 'the NINTENDO label is missing'
    # The game's 48 pixels become 128 more sharply than Qt's smooth scaling took them (its steepest step on this edge was 48), and
    # nothing rings past the edge: every pixel stays between the two greys
    edge = [pixel[0] for pixel in title.sharp_icon([[(64,) * 3 if x < 24 else (192,) * 3 for x in range(48)] for _ in range(48)])[64]]
    assert max(abs(b - a) for a, b in zip(edge, edge[1:])) >= 56, edge[56:72]
    assert edge[:60] == [64] * 60 and edge[68:] == [192] * 60, f'the edge rings: {edge[56:72]}'

    # a picture of any other size fills its frame whole: 344 wide, as tall as its shape asks within 200-300, centred in the band
    cover = os.path.join(tmp, 'cover.png')
    picture = QImage(300, 400, QImage.Format_RGB888)
    picture.fill(QColor(*BLUE))
    assert picture.save(cover)
    screens = title.console_screens(cover, icon, 'Released in Europe: 2011')
    tv, pad = screens['bootTvTex.tga'], screens['bootDrcTex.tga']
    assert (len(tv), len(tv[0]), len(pad), len(pad[0])) == (720, 1280, 480, 854)
    assert near(tv[372][244], BLUE) and near(tv[230][80], BLUE) and near(tv[514][408], BLUE), (tv[372][244], tv[230][80], tv[514][408])
    assert near(tv[200][480], WHITE, 12), tv[200][480]                 # the band
    assert near(tv[10][10], (247, 247, 247), 12), tv[10][10]          # the background above it
    assert near(tv[700][400], (214, 214, 214), 16), tv[700][400]      # and below
    assert near(pad[248][163], BLUE), pad[248][163]                    # the GamePad's is the TV's, smaller
    assert sum(tv[78][844]) < sum(tv[78][400]) - 30, (tv[78][844], tv[78][400])  # the console behind it on the right, its lid's face

    # a title screen as libretro keeps it, both screens one above the other (top_screen): the top one alone fills the frame, 344x206
    stacked = os.path.join(tmp, 'stacked.png')
    picture = QImage(512, 614, QImage.Format_RGB888)
    picture.fill(QColor(*BLUE))
    paint = QPainter(picture)
    paint.fillRect(0, 307, 512, 307, QColor(*GREEN))
    paint.end()
    assert picture.save(stacked)
    tv = title.console_screens(stacked, icon, top_screen=True)['bootTvTex.tga']
    for y in (372 - 98, 372, 372 + 98):
        assert near(tv[y][244], BLUE), (y, tv[y][244])
    assert near(tv[372 - 110][244], WHITE, 12) and near(tv[372 + 110][244], WHITE, 12), 'the frame is not the top screen\'s shape'

    # a GameTDB case scan (768x680, case_scan): its white strip down the right is left out, the whole height stays (the green foot, where a
    # logo can sit), and it shows whole in its frame, 344 across and as tall as its shape asks
    case = os.path.join(tmp, 'case.jpg')
    picture = QImage(768, 680, QImage.Format_RGB888)
    picture.fill(QColor(*BLUE))
    paint = QPainter(picture)
    paint.fillRect(684, 0, 84, 680, QColor(*WHITE))
    paint.fillRect(0, 600, 684, 80, QColor(*GREEN))
    paint.end()
    assert picture.save(case, quality=95)
    tv = title.console_screens(case, icon, case_scan=True)['bootTvTex.tga']
    top, bottom = round(372 - 342 / 2) + 3, round(372 + 342 / 2) - 4
    for y, x in ((top, 76), (top, 412), (372, 244), (372, 412)):
        assert near(tv[y][x], BLUE), (y, x, tv[y][x])
    for y, x in ((bottom, 76), (bottom, 412), (bottom, 244)):
        assert near(tv[y][x], GREEN), (y, x, tv[y][x])
    assert near(tv[372][424], WHITE, 12) and near(tv[190][244], WHITE, 12), 'the frame is not the picture whole'
    # the same picture from the user (not a case scan) isn't cut: its white strip is still at its right
    tv = title.console_screens(case, icon)['bootTvTex.tga']
    assert near(tv[372][410], WHITE, 20), tv[372][410]
    # a tall case scan fits the band's height with a margin
    tall = QImage(768, 1400, QImage.Format_RGB888)
    tall.fill(QColor(*BLUE))
    assert tall.save(os.path.join(tmp, 'tall.png'))
    tv = title.console_screens(os.path.join(tmp, 'tall.png'), icon, case_scan=True)['bootTvTex.tga']
    assert near(tv[372][200], BLUE) and near(tv[210][200], BLUE) and near(tv[530][200], BLUE), 'a tall picture is cut'
    assert near(tv[190][200], WHITE, 12) and near(tv[550][200], WHITE, 12), 'a tall picture runs to the band edge'

    # the line under the game's name: released's words, or the maker, or nothing where there is neither
    nameless = os.path.join(tmp, 'nameless.bin')
    open(nameless, 'wb').write(smdh('Some Game', ''))
    with_year = title.console_screens(cover, nameless, 'Released in Europe: 2011')['bootTvTex.tga']
    without = title.console_screens(cover, nameless)['bootTvTex.tga']
    line = lambda tv, a, b: [row[552:1240] for row in tv[a:b]]
    assert line(with_year, 456, 504) != line(without, 456, 504), 'the release line is not drawn'
    assert line(with_year, 344, 440) == line(without, 344, 440) and line(with_year, 510, 560) == line(without, 510, 560)

    open(os.path.join(tmp, 'bad.png'), 'wb').write(b'not a picture')
    assert title.console_screens(os.path.join(tmp, 'bad.png'), icon) is None

    # the GamePad's bezel: 854x480 RGBA, row 0 the top. A 3416x960 picture is halved, then cut at its sides and centred; its top rows yellow,
    # a see-through part black.
    wide = QImage(3416, 960, QImage.Format_RGBA8888)
    wide.fill(QColor(0, 0, 0, 0))
    painter = QPainter(wide)
    painter.fillRect(0, 0, 854, 960, QColor(255, 0, 0))  # cut off
    painter.fillRect(854, 0, 1708, 960, QColor(0, 0, 255))  # kept: the middle
    painter.fillRect(2562, 0, 854, 960, QColor(255, 0, 0))  # cut off
    painter.fillRect(854, 0, 1708, 40, QColor(255, 255, 0))  # the top rows
    painter.setCompositionMode(QPainter.CompositionMode_Source)
    painter.fillRect(854, 0, 200, 960, QColor(0, 255, 0, 0))  # see-through within it
    painter.end()
    wide_path = os.path.join(tmp, 'bezel.png')
    assert wide.save(wide_path)
    px = title.bezel_pixels(wide_path)
    assert px and len(px) == 854 * 480 * 4, len(px or b'')
    at = lambda x, y: tuple(px[(y * 854 + x) * 4:(y * 854 + x) * 4 + 4])
    assert at(2, 2)[:3] == (0, 0, 0) and at(50, 240) == (0, 0, 0, 255), 'a see-through part is not black'
    assert near(at(400, 5)[:3], (255, 255, 0)) and near(at(400, 470)[:3], (0, 0, 255)), ('upside down, or not scaled', at(400, 5), at(400, 470))
    assert near(at(850, 240)[:3], (0, 0, 255)), ('not cut at the sides', at(850, 240))
    assert title.bezel_pixels(os.path.join(tmp, 'bad.png')) is None

    # A whole title (make_title) from a made-up game: the GamePad's start-up screen from a picture of its own (green), shown as it is,
    # beside the TV's art (red) as it is or in the Virtual Console's layout; without one, or with one that can't be read, the TV's picture
    # on both. The game's own start-up sound (a .btsnd, or a WAV made into one) in place of the one for every game, which a title without
    # one, or with one gone or not a sound, gets.
    game, shaders, archives = (os.path.join(tmp, d) for d in ('game', 'shaders', 'archives'))
    for d in (os.path.join(game, 'exefs'), shaders, archives):
        os.makedirs(d, exist_ok=True)
    every, own = struct.pack('>II', 2, 0) + b'EVRY' * 4, struct.pack('>II', 2, 0) + b'OWN!' * 4  # a boot sound's head, then samples
    for f, body in (('game/exheader.bin', b''), ('game/code.bin', b''), ('game/romfs.bin', b''), ('game/exefs/icon.bin', smdh('Some Game', 'Maker')),
                    ('g.rpx', b'rpx'), ('shaders/shaders.gsh', b''), ('shaders/shaders.idx', b''), ('shaders/fallback-shaders.bin', b'fallback fixture'), ('boot_sound.btsnd', every),
                    ('own.btsnd', own), ('bad.btsnd', b'not a sound')):
        with open(os.path.join(tmp, f), 'wb') as fh:
            fh.write(body)
    import wave
    with wave.open(os.path.join(tmp, 'start.wav'), 'wb') as w:  # half a second of 48 kHz stereo
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(bytes(4 * 24000))
    pictures = {}
    for name, w, h, rgb in (('red', 1280, 720, (255, 0, 0)), ('green', 854, 480, (0, 255, 0))):
        img = QImage(w, h, QImage.Format_RGB888)
        img.fill(QColor(*rgb))
        pictures[name] = os.path.join(tmp, name + '.png')
        assert img.save(pictures[name])

    def made(style, pad_art=None, boot_sound=None, icon_art=None, art='red'):
        out = os.path.join(tmp, 'title')
        title.make_title(out, game, os.path.join(tmp, 'g.rpx'), shaders, '0005000010334455', 'A game', 'WUP-N-ABCD', archives,
                         pictures.get(art, art), icon_art, style, pad_art=pad_art, boot_sound=boot_sound)
        middle = {}
        for tga, w, h in (('bootTvTex.tga', 1280, 720), ('bootDrcTex.tga', 854, 480)):
            data = open(os.path.join(out, 'meta', tga), 'rb').read()
            at = 18 + ((h - 1 - h // 2) * w + w // 2) * 3  # 24-bit, bottom row first
            middle[tga] = tuple(data[at + 2:at - 1:-1])
        return middle, open(os.path.join(out, 'meta', 'bootSound.btsnd'), 'rb').read()

    def menu_pixel(x, y):
        data = open(os.path.join(tmp, 'title', 'meta', 'iconTex.tga'), 'rb').read()
        assert struct.unpack_from('<HHBB', data, 12) == (128, 128, 32, 8)
        assert len(data) == 18 + 128 * 128 * 4 + len(title.TGA_FOOTER) and data.endswith(title.TGA_FOOTER)
        at = 18 + ((127 - y) * 128 + x) * 4
        b, g, r, a = data[at:at + 4]
        assert a == 255
        return r, g, b

    middle, sound = made('picture')
    assert near(middle['bootTvTex.tga'], (255, 0, 0)) and near(middle['bootDrcTex.tga'], (255, 0, 0)) and sound == every, (middle, sound)
    assert menu_pixel(64, 64) == (0, 255, 0), 'bootscreen art must not replace the default SMDH icon'
    assert 0 < menu_pixel(2, 2)[1] < 255, 'the default menu icon has no VC band'
    made('picture', icon_art=pictures['red'])
    assert menu_pixel(64, 64) == (255, 0, 0) and menu_pixel(2, 2) == (0, 0, 0), 'a custom icon was framed or replaced'
    made('picture', icon_art=os.path.join(tmp, 'bad.png'))
    assert menu_pixel(64, 64) == (0, 255, 0) and 0 < menu_pixel(2, 2)[1] < 255, 'invalid custom art must fall back to the framed SMDH'
    middle, sound = made('picture', pictures['green'], os.path.join(tmp, 'own.btsnd'))
    assert near(middle['bootTvTex.tga'], (255, 0, 0)) and near(middle['bootDrcTex.tga'], (0, 255, 0)) and sound == own, (middle, sound)
    middle, sound = made('console', pictures['green'], os.path.join(tmp, 'gone.btsnd'))
    assert near(middle['bootTvTex.tga'], (255, 255, 255), 12), ("the TV lost the Virtual Console's layout", middle)  # its white band
    assert near(middle['bootDrcTex.tga'], (0, 255, 0)) and sound == every, ('the GamePad picture laid out, or a lost sound kept', middle, sound)
    for missing in (None, os.path.join(tmp, 'bad.png')):
        middle, _ = made('console', art=missing)
        assert all(near(pixel, (255, 255, 255), 12) for pixel in middle.values()), 'missing art removed the VC layout'
    middle, _ = made('picture', art=None)
    assert all(near(pixel, (0, 255, 0)) for pixel in middle.values()), 'Picture style lost its icon fallback'
    middle, sound = made('picture', os.path.join(tmp, 'bad.png'), os.path.join(tmp, 'bad.btsnd'))
    assert near(middle['bootDrcTex.tga'], (255, 0, 0)) and sound == every, ("a picture or sound that can't be used wasn't left out", middle, sound)
    sound = made('picture', boot_sound=os.path.join(tmp, 'start.wav'))[1]
    assert sound[:4] == struct.pack('>I', 2) and len(sound) > 8 + 4 * 24000 // 2, 'a WAV given as the sound was not made into one'
    assert open(os.path.join(tmp, 'title', 'content', 'fallback-shaders.bin'), 'rb').read() == b'fallback fixture'
    os.remove(os.path.join(shaders, 'fallback-shaders.bin'))
    before = open(os.path.join(tmp, 'title', 'content', 'romfs.bin'), 'rb').read()
    try:
        made('picture')
    except ValueError as error:
        assert 'Optimized fallback shader pack is missing' in str(error)
    else:
        raise AssertionError('A full title silently omitted optimized fallback shaders')
    assert open(os.path.join(tmp, 'title', 'content', 'romfs.bin'), 'rb').read() == before
print('title art: ok')
