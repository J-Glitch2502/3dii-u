"""The builder's window. ui/Main.qml draws it; App holds what it shows and does what its buttons ask, running pipeline.py for the build.

Questions come up inside the window one at a time: a choice that asks several things is a generator that yields each question and is sent the
answer (App.run_flow), and messages wait their turn in the same queue."""
import base64
import functools
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass

import shiboken6
from PySide6.QtCore import QBuffer, QIODevice, QLocale, QObject, Property, QSize, QStandardPaths, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QColor, QFontDatabase, QGuiApplication, QIcon, QImage, QPixmap
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickWindow
from PySide6.QtWidgets import QApplication, QFileDialog, QInputDialog

import covers
import credits
import decrypt
import dumps
import mods
import pipeline
import saves
import updater
import user_files

APP, VERSION = credits.APP, credits.VERSION
HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, 'frozen', False)
WIN = sys.platform == 'win32'
NO_WINDOW = 0x08000000 if WIN else 0  # the build's compilers would each open a console window otherwise
DUMPS = '3DS games (*.3ds *.cci *.cia *.cxi *.app);;All files (*.*)'
DUMP_EXTS = ('.3ds', '.cci', '.cia', '.cxi', '.app')
STAGE_OF = {n: i for i, (n, _) in enumerate(pipeline.STAGES)}
# What the art lines show of each choice: a word or two, so the Art menu's right column fits them whole
NO_ART = "Game's icon"
NO_ICON_ART = 'VC game icon'  # the game's own SMDH with the shared Virtual Console frame
NO_BEZEL = 'Black'  # the GamePad beside the screens without a picture
NO_PAD_ART = 'Same as TV'  # the GamePad's start-up screen without a picture of its own
NO_SAVE = 'None'  # no 3DS save packed in
NO_GAME_SOUND = 'Default'  # a game without a start-up sound of its own: the one for every game, if there is one
CUSTOM = 'Custom'  # a picture or sound the player chose
BOOT_SOUND = os.path.join(os.path.dirname(pipeline.ARCHIVES), 'boot_sound.btsnd')  # where every build picks it up (tools/make_game_title.py)
GAME_SOUNDS = os.path.join(os.path.dirname(BOOT_SOUND), 'boot_sounds')  # a game's own, <title id>.btsnd
LIBRARY_MAX = 40  # games the Games page remembers
# What each of "Your Files" is and how to get it from your own consoles: the Info buttons, and the message when one is missing.
# Testers asked what the system files were and how to get seeddb.bin (Nov 9 and 10).
HELP = {
    'keys': ('3DS keys',
             'Press "Folder" and choose the folder with boot9.bin from your own 3DS (and seeddb.bin, if you have it). Only encrypted game '
             'dumps need them.\n\n'
             'Get them with GodMode9 on a 3DS with custom firmware:\n'
             '•  boot9.bin: open [M:] MEMORY VIRTUAL, highlight boot9.bin and press Y. Open [0:] SDCARD, press Y again and choose Copy.\n'
             '•  seeddb.bin: press HOME, choose More..., then Build support files. It lands in gm9/out on the SD card. Some eShop games '
             'need it.\n\n'
             'One folder can hold everything from your 3DS: boot9.bin, seeddb.bin, and the 0004009b and 000400db folders. Choose it here or '
             'under 3DS system files, and the builder takes all of it.'),
    'otp': ('Wii U otp.bin',
            'A small file (exactly 1024 bytes) from your own Wii U. The game is packed with a key from it so your console installs it. The '
            'builder never shows or stores that key.\n\n'
            'It comes from a Wii U with Aroma: run Wii U NAND Dumper, choose Dump OTP, and otp.bin is written to the SD card. If you already '
            'made a NAND backup when setting up Aroma, otp.bin is in that backup.\n\n'
            'On the PC, press "File" and choose it. Keep it private: it is unique to your console.'),
    'system': ('3DS system files',
               'Press "Folder" and choose the folder with the 0004009b and 000400db folders from your own 3DS, or Azahar\'s user folder. '
               'They hold the 3DS font, the parts Miis are drawn from, and the word list games check names against. Games need them, and a '
               'game dump doesn\'t have them.\n\n'
               'Get them one of two ways:\n'
               '•  GodMode9 on your 3DS: open [1:] SYSNAND CTRNAND, then title. Highlight 0004009b, press Y, open [0:] SDCARD, press Y '
               'again and choose Copy (not Move). Copy 000400db the same way, next to it.\n'
               '•  Azahar: File > Set Up System Files, with the Artic Setup Tool running on your 3DS. The builder then finds them by itself.\n\n'
               'Copied with GodMode9 they are encrypted: put your boot9.bin in the same folder, and the builder opens them with it. It keeps '
               'them for every game after.'),
}


# ---- settings, the PC's drives, and words for what was chosen
def settings_file():
    base = os.environ.get('APPDATA') if WIN else os.path.join(os.path.expanduser('~'), '.config')
    return os.path.join(base or HERE, APP, 'settings.json')


def covers_folder():
    return os.path.join(os.path.dirname(settings_file()), 'covers')


FROM_SGDB, FROM_TDB = 'SteamGridDB', 'GameTDB'


def picked(path):
    """whether a picture is one the player chose, not one fetched into the covers folder (covers.py)"""
    return bool(path) and os.path.dirname(os.path.abspath(path)) != os.path.abspath(covers_folder())


def fetched_icon(path, game):
    """Only the downloader's exact names for this game inside its own cache are legacy automatic icons."""
    if not path or os.path.normcase(os.path.dirname(os.path.abspath(path))) != os.path.normcase(os.path.abspath(covers_folder())):
        return False
    stem, suffix = os.path.splitext(os.path.basename(path))
    return suffix.lower() in ('.png', '.jpg') and stem in (f'sgdb_{game.title_id:016x}_icon', covers.game_code(game.product))


def source(path):
    """a picture's line in the window: where it was fetched from, or that the player chose it"""
    if picked(path):
        return CUSTOM
    return FROM_SGDB if os.path.basename(path).startswith('sgdb_') else FROM_TDB


def load_settings():
    try:
        return json.load(open(settings_file(), encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_settings(s):
    os.makedirs(os.path.dirname(settings_file()), exist_ok=True)
    json.dump(s, open(settings_file(), 'w', encoding='utf-8'), indent=1)


def emulator_folders(everywhere=True):
    """Folders with a 3DS NAND, where the system archives (shared font, Mii data) can come from: Azahar's, Citra's and Lime3DS's user folders,
    then (everywhere) any folder a few levels down in Downloads, Desktop and Documents, whatever its name: a portable emulator's user folder,
    or a copy of one. A Miitopia built without them stopped on the console, the files being in such a copy in Downloads (Nov 9)."""
    home = os.path.expanduser('~')
    base = os.environ.get('APPDATA', '') if WIN else os.path.join(home, '.local', 'share')
    names = ('Azahar', 'Citra', 'Lime3DS') if WIN else ('azahar-emu', 'citra-emu', 'lime3ds-emu')
    found = [p for p in (os.path.join(base, n) for n in names) if dumps.nand_titles(p)]
    if not everywhere:
        return found
    places = (QStandardPaths.StandardLocation.DownloadLocation, QStandardPaths.StandardLocation.DesktopLocation,
              QStandardPaths.StandardLocation.DocumentsLocation)
    tops = [QStandardPaths.writableLocation(p) for p in places]  # where Windows really keeps them (OneDrive, moved), then where they usually are
    tops += [os.path.join(home, d) for d in ('Downloads', 'Desktop', 'Documents')]
    seen = set()
    for top in tops:
        if top and os.path.normcase(os.path.abspath(top)) not in seen:
            seen.add(os.path.normcase(os.path.abspath(top)))
            found += nands_below(top, 3)  # deeper costs seconds on a big Downloads folder, and these sit near the top
    return found


def nands_below(top, depth):
    """the folders holding a 3DS NAND up to depth levels below top; one that holds a NAND isn't searched further"""
    found, level = [], [top]
    for _ in range(depth):
        below = []
        for folder in level:
            try:
                subs = [e.path for e in os.scandir(folder) if e.is_dir(follow_symlinks=False)]
            except OSError:
                continue
            for sub in subs:
                if dumps.nand_titles(sub):
                    found.append(sub)
                else:
                    below.append(sub)
        level = below
    return found


DRIVE_REMOVABLE, DRIVE_FIXED = 2, 3
IOCTL_STORAGE_QUERY_PROPERTY = 0x2D1400
CARD_BUSES = (7, 12, 13)  # STORAGE_BUS_TYPE: BusTypeUsb, BusTypeSd, BusTypeMmc


@functools.cache
def kernel32():
    """kernel32 with the types card_reader_disk needs, in its own copy: ctypes.windll's stays as other code expects it"""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32')
    k32.CreateFileW.restype = wintypes.HANDLE  # so a failure comes back as INVALID_HANDLE_VALUE, not a truncated int
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    return k32


def card_reader_disk(k32, root):
    """A FAT or FAT32 disk on a USB, SD or MMC bus: many card readers call the card in them a fixed disk. A Wii U card is FAT32, as an
    internal disk never is."""
    import ctypes
    from ctypes import wintypes
    name = ctypes.create_unicode_buffer(16)
    if not k32.GetVolumeInformationW(root, None, 0, None, None, None, name, 16) or name.value not in ('FAT32', 'FAT'):
        return False
    disk = k32.CreateFileW(f'\\\\.\\{root[:2]}', 0, 3, None, 3, 0, None)  # no access asked for: what a disk is needs no rights
    if disk in (None, ctypes.c_void_p(-1).value):  # INVALID_HANDLE_VALUE
        return False
    try:
        query = (ctypes.c_uint32 * 3)()  # STORAGE_PROPERTY_QUERY: StorageDeviceProperty, PropertyStandardQuery
        descriptor, got = ctypes.create_string_buffer(64), wintypes.DWORD()
        if not k32.DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, query, 12, descriptor, 64, ctypes.byref(got), None) or got.value < 32:
            return False
        return int.from_bytes(descriptor.raw[28:32], 'little') in CARD_BUSES  # STORAGE_DEVICE_DESCRIPTOR.BusType
    finally:
        k32.CloseHandle(disk)


def wiiu_card(path):
    return os.path.isdir(os.path.join(path, 'wiiu'))


def sd_cards():
    """(path, removable) for removable drives and card readers' disks: the ones already set up for a Wii U first, then removable ones"""
    found = []
    if WIN:
        k32 = kernel32()
        mask = k32.GetLogicalDrives()
        for i in range(26):
            root = f'{chr(65 + i)}:\\'
            if not mask >> i & 1:
                continue
            kind = k32.GetDriveTypeW(root)
            if kind == DRIVE_REMOVABLE or (kind == DRIVE_FIXED and card_reader_disk(k32, root)):
                found.append((root, kind == DRIVE_REMOVABLE))
    else:
        user = os.environ.get('USER', '')
        for base in (f'/media/{user}', f'/run/media/{user}'):
            if os.path.isdir(base):
                found += [(os.path.join(base, d), True) for d in sorted(os.listdir(base))]
    return sorted(found, key=lambda card: (not wiiu_card(card[0]), not card[1]))


def describe(d):
    if d is None:
        return ''
    v = f'v{d.version >> 10}.{d.version >> 4 & 63}.{d.version & 15}'
    return f'{v}, {d.size >> 20} MB' if d.kind == 'update' else f'{len([c for c in d.cia.contents if c[4] is not None])} contents, {d.size >> 20} MB' \
        if d.cia else f'{d.size >> 20} MB'


def otp_ok(path):
    return user_files.check_otp(path) is None


def short(path, n=46, end=False):
    """a path cut to n characters from the middle (or the end), for labels"""
    if len(path) <= n:
        return path
    return path[:n - 3] + '...' if end else path[:n // 2 - 1] + '...' + path[-(n // 2 - 2):]


# ---- pictures
def png_url(img):
    buf = QBuffer()
    buf.open(QIODevice.WriteOnly)
    img.save(buf, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(bytes(buf.data())).decode()


def icon_url(rows):
    """An icon's RGB rows as a picture the window can show, including the generated 128x128 menu icon."""
    img = QImage(len(rows[0]), len(rows), QImage.Format_RGB32)
    for y, row in enumerate(rows):
        for x, (r, g, b) in enumerate(row):
            img.setPixel(x, y, QColor(r, g, b).rgb())
    return png_url(img)


def draw_icon(size):
    """the app's icon, the 3Dii U logo (icon.png, the user's, 2026-11-09), at size x size: the window's, the rail's and the exe's (make_app.py)"""
    img = QImage(os.path.join(HERE, 'icon.png'))
    return img.scaled(size, size, Qt.KeepAspectRatio, Qt.SmoothTransformation).convertToFormat(QImage.Format_ARGB32)


def theme():
    families = set(QFontDatabase.families())
    body = next((f for f in ('Segoe UI Variable Text', 'Segoe UI', 'Cantarell', 'Noto Sans', 'Arial', 'Tahoma') if f in families), QFontDatabase.systemFont(QFontDatabase.SystemFont.GeneralFont).family())
    display = next((f for f in ('Segoe UI Variable Display', 'Segoe UI', 'Cantarell') if f in families), body)
    # the Wii U Menu's glass: grey cards with a curved gloss, grey text, cyan for chosen and red for missing; dark is the same glass inverted
    light = {'pageTop': '#ffffff', 'pageBottom': '#eef1f0', 'card': '#cbd2d0', 'gloss': '#dde1e0', 'shadow': '#40000000',
             'title': '#7e8b88', 'sub': '#a8b3b0', 'label': '#6c7976', 'subLabel': '#a3aeab',
             'cyanBase': '#9fdde8', 'cyanText': '#35b5cb', 'redBase': '#eba3aa', 'redText': '#c93a4c', 'greyBase': '#bfc7c5', 'greyText': '#6c7976',
             'lightRed': '#c9283c', 'lightCyan': '#2fb4cc', 'lightGreen': '#3aa856', 'lightGrey': '#aab4b1', 'socket': '#98a3a0',
             'tileGloss': '#ffffff', 'tileBase': '#cdd3d1', 'plus': '#8b9996', 'buildText': '#3c4846',
             'rail': '#e6e9e8', 'railTop': '#dde1e0', 'railActive': '#f3f5f4', 'railLine': '#fbfcfc', 'railEdge': '#cfd5d3', 'railText': '#7b8885',
             'help': '#5c6a67', 'helpActive': '#6e7d7a', 'helpText': '#ffffff', 'helpRing': '#3c4846', 'helpFace': '#ffffff',
             'keycap': '#ffffff', 'keycapBase': '#c3cac8', 'keycapSide': '#a3aeab', 'keycapGlyph': '#a9b4b1', 'tileRim': '#e4e8e7', 'tileEdge': '#b9c3c0',
             'red': '#d6394b', 'cyan': '#3dbdd6', 'builder': '#7b8885', 'ds': '#e5959f', 'screen': '#ffffff', 'dotFrom': '#e8a4ad', 'dotMid': '#c9c3d8',
             'dotTo': '#a9dde7', 'pad': '#8fdbe7', 'dim': '#73000000', 'logBg': '#e9eceb', 'logText': '#4a5654'}
    dark = dict(light, pageTop='#20272a', pageBottom='#1a2023', card='#313b3e', gloss='#3b4649', shadow='#80000000',
                title='#d7dedc', sub='#8f9c99', label='#c5cecb', subLabel='#8b9895',
                cyanBase='#1f6f80', cyanText='#9fe6f2', redBase='#7a2a34', redText='#ffb3bb', greyBase='#4a5558', greyText='#d0d7d5',
                lightGrey='#5f6c6f', socket='#1d2427', tileGloss='#5f6c6f', tileBase='#2e383b', plus='#5a6769', buildText='#d7dedc',
                rail='#2a3336', railTop='#252d30', railActive='#344043', railLine='#1d2427', railEdge='#1d2427', railText='#c5cecb',
                help='#11181a', helpActive='#1f2a2d', helpRing='#000000', helpFace='#d7dedc',
                keycap='#5a6669', keycapBase='#3a4548', keycapSide='#263033', keycapGlyph='#8f9c99', tileRim='#45524f', tileEdge='#252e31', builder='#c9d1cf', screen='#2a3336',
                dim='#a6000000', logBg='#1d2427', logText='#c5cecb')
    return {'font': body, 'display': display, 'mono': 'Cascadia Mono' if 'Cascadia Mono' in families else 'Consolas', 'light': light, 'dark': dark}


def build_compat_list(compat):
    """Unified cross-platform compatibility records for Wii U and Cemu."""
    wiiu = {program_id: g for program_id, g in compat.items() if not program_id.startswith('_')}
    wiiu.update(compat.get('_wiiu', {}))
    cemu = compat.get('_cemu', {})
    rows = []
    for program_id in wiiu.keys() | cemu.keys():
        console = wiiu.get(program_id, {})
        emulator = cemu.get(program_id, {})
        name = (console or emulator)['name']
        if console.get('edition'):
            name += f' ({console["edition"]})'
        rows.append(dict(name=name, program_id=program_id,
                         wiiu_status=console.get('status', 'Untested'), wiiu_note=console.get('note', ''),
                         cemu_status=emulator.get('status', 'Plays') if emulator else 'Untested', cemu_note=emulator.get('note', '')))
    rows.sort(key=lambda r: (r['name'].casefold(), r['program_id']))
    return rows


# ---- what the window shows, and what its buttons do
@dataclass(eq=False)
class BuildJob:
    name: str
    argv: tuple[str, ...]
    destination: str
    card: bool
    env: dict[str, str]
    system_folder: str
    output_format: str = 'wup'
    homebrew_parts: str = 'full'
    wup_update_parts: str = 'both'
    output: str = ''
    status: str = 'Queued'
    detail: str = ''


class App(QObject):
    changed = Signal()
    coverReady = Signal(str, str, str)  # title id, the picture's path, the icon's: from fetch_cover's thread
    layoutWanted = Signal(str)  # setLayout: run() swaps the window
    keyRefused = Signal()  # SteamGridDB turned the key down: from fetch_cover's thread
    updateEvent = Signal(str, object)  # what, value: from the updater's worker threads, the value passed as it is
    logChanged = Signal()
    destWanted = Signal()  # build() has nowhere to put the game: the window brings its choice into view

    def _p(kind, name, notify=changed):
        return Property(kind, lambda self: self._s[name], notify=notify)

    hasGame, gameName, maker, badge, known = _p(bool, 'hasGame'), _p(str, 'gameName'), _p(str, 'maker'), _p(str, 'badge'), _p(bool, 'known')
    note, meta, icon = _p(str, 'note'), _p(str, 'meta'), _p(str, 'icon')
    updateText, hasUpdate, dlcText, hasDlc = _p(str, 'updateText'), _p(bool, 'hasUpdate'), _p(str, 'dlcText'), _p(bool, 'hasDlc')
    modText, hasMod = _p(str, 'modText'), _p(bool, 'hasMod')
    artText, hasArt, artUrl, artOwn = _p(str, 'artText'), _p(bool, 'hasArt'), _p(str, 'artUrl'), _p(bool, 'artOwn')
    iconText, hasIconArt, iconArtUrl, tileUrl = _p(str, 'iconText'), _p(bool, 'hasIconArt'), _p(str, 'iconArtUrl'), _p(str, 'tileUrl')
    bezelText, hasBezel = _p(str, 'bezelText'), _p(bool, 'hasBezel')
    padArtText, hasPadArt, gameSoundText, hasGameSound = _p(str, 'padArtText'), _p(bool, 'hasPadArt'), _p(str, 'gameSoundText'), _p(bool, 'hasGameSound')
    saveText, hasSave = _p(str, 'saveText'), _p(bool, 'hasSave')
    artKeySet, soundSet, artStyle = _p(bool, 'artKeySet'), _p(bool, 'soundSet'), _p(str, 'artStyle')
    gameMenu = _p(bool, 'gameMenu')
    menuStyle = _p(str, 'menuStyle')
    menuShortcut = _p(str, 'menuShortcut')
    menuButtons = Property('QVariantList', lambda self: list(pipeline.MENU_BUTTONS), constant=True)
    gameLanguage = _p(str, 'gameLanguage')
    gameLanguages = Property('QVariantList', lambda self: ['Console default', *pipeline.LANGUAGES], constant=True)
    language, languages = _p(str, 'language'), _p('QVariantList', 'languages')
    library = _p('QVariantList', 'library')
    keysOk, keysNeeded, keysText, otpOk, otpText = _p(bool, 'keysOk'), _p(bool, 'keysNeeded'), _p(str, 'keysText'), _p(bool, 'otpOk'), _p(str, 'otpText')
    sysOk, sysText = _p(bool, 'sysOk'), _p(str, 'sysText')
    buildText, buildCustom = _p(str, 'buildText'), _p(bool, 'buildCustom')
    outputFormat, homebrewParts = _p(str, 'outputFormat'), _p(str, 'homebrewParts')
    wupUpdateParts = _p(str, 'wupUpdateParts')
    wupBaseText = _p(str, 'wupBaseText')
    hasWupBase = _p(bool, 'hasWupBase')
    target, destText, cards = _p(str, 'target'), _p(str, 'destText'), _p('QVariantList', 'cards')
    building, view = _p(bool, 'building'), _p(str, 'view')
    buildName = _p(str, 'buildName')
    queueItems, queuePending, queueRunning = _p('QVariantList', 'queueItems'), _p(int, 'queuePending'), _p(bool, 'queueRunning')
    stages, stage, failed, progress, detail = _p('QVariantList', 'stages'), _p(int, 'stage'), _p(bool, 'failed'), _p(float, 'progress'), _p(str, 'detail')
    log = _p(str, 'log', logChanged)
    resultText, where, dialog = _p(str, 'resultText'), _p(str, 'where'), _p('QVariant', 'dialog')
    compatList, version, rootOk = _p('QVariantList', 'compatList'), _p(str, 'version'), _p(bool, 'rootOk')
    faq, themeMode, dark = _p('QVariantList', 'faq'), _p(str, 'themeMode'), _p(bool, 'dark')
    updateState, updateTitle, updateNotes, updateProgress = _p(str, 'updateState'), _p(str, 'updateTitle'), _p(str, 'updateNotes'), _p(int, 'updateProgress')
    updateMessage, releaseTag, updateSetAside = _p(str, 'updateMessage'), _p(str, 'releaseTag'), _p(bool, 'updateSetAside')
    del _p

    def __init__(self):
        super().__init__()
        self.st = load_settings()
        if not self.st.get('keys'):
            self.st['keys'] = decrypt.KEYS_DEFAULT
        decrypt.use_keys(self.st['keys'])
        self.game = self.update = self.dlc = self.mod = self.art = self.icon_art = None  # the mod, the banner art and the icon art are paths
        self.bezel = None  # the GamePad's picture beside the screens, a path
        self.pad_art = self.game_sound = None  # the GamePad's start-up picture and the game's start-up sound (a .btsnd), paths
        self.save_in = None  # the folder of a 3DS save to pack into the build (saves.py)
        self.refused_key = None  # the SteamGridDB key it turned down, if it did (fetch_cover)
        self.proc, self.result, self.cancelled = None, None, False
        self.jobs, self.active_build = [], None
        self.lines, self.log_lines, self.dialogs, self.flow = queue.Queue(), [], [], None
        compat = json.load(open(os.path.join(HERE, 'compat.json'), encoding='utf-8'))
        compat_list = build_compat_list(compat)
        self.compat_by_id = {row['program_id']: row for row in compat_list}
        try:  # the Help page's questions and answers: without them the app still runs
            with open(os.path.join(HERE, 'faq.json'), encoding='utf-8') as f:
                faq = json.load(f)
        except (OSError, ValueError):
            faq = []
        faq = [e for e in faq if isinstance(e, dict) and isinstance(e.get('q'), str) and isinstance(e.get('a'), str) and e['q'] and e['a']]             if isinstance(faq, list) else []
        theme_mode = self.st.get('theme')
        system_locale = QLocale.system()
        self.system_language = pipeline.language_for_locale(next(iter(system_locale.uiLanguages()), system_locale.name()))
        shortcut = self.st.get('menu_shortcut')
        self._s = dict(hasGame=False, gameName='', gameLanguage='', language='', languages=[f'System ({self.system_language})', *pipeline.LANGUAGES], maker='', badge='', known=False, icon='', meta='',
                       menuStyle='classic' if self.st.get('menu_style') == 'classic' else 'new',
                       menuShortcut=shortcut if pipeline.valid_menu_shortcut(shortcut) else '',
                       note='Pick one of your 3DS games to start. The builder reads its name and icon, and asks about its update and DLC.',
                       updateText='None', hasUpdate=False, dlcText='None', hasDlc=False, modText='None', hasMod=False, artText=NO_ART, hasArt=False, artUrl='', artOwn=False, iconText=NO_ART, hasIconArt=False, iconArtUrl='', tileUrl='', bezelText=NO_BEZEL, hasBezel=False, padArtText=NO_PAD_ART, hasPadArt=False, gameSoundText=NO_GAME_SOUND, hasGameSound=False, saveText=NO_SAVE, hasSave=False, artKeySet=bool(self.st.get('sgdb_key')), soundSet=os.path.isfile(BOOT_SOUND), artStyle=self.st.get('art_style', 'console'), gameMenu=self.st.get('game_menu', True) is not False, library=[], keysOk=False, keysNeeded=False, keysText='', otpOk=False,
                       otpText='', sysOk=False, sysText='', outputFormat=self.st.get('output_format', 'wup') if self.st.get('output_format') in ('wup','wup-update','homebrew') else 'wup', homebrewParts=self.st.get('homebrew_parts', 'full') if self.st.get('homebrew_parts') in ('full','binary','shaders','both') else 'full', wupBaseText=self.st.get('wup_base_manifest') or 'Last full base built in this work folder', hasWupBase=bool(self.st.get('wup_base_manifest')), wupUpdateParts=self.st.get('wup_update_parts','both') if self.st.get('wup_update_parts') in ('binary','both') else 'both', target=self.st.get('target', 'card'), destText='', cards=[], building=False, view='form', stages=[],
                       stage=-1, failed=False, progress=0.0, detail='', log='', resultText='', where='', dialog=None, version=VERSION,
                       buildName='', queueItems=[], queuePending=0, queueRunning=False,
                       rootOk=bool(pipeline.ROOT) and os.path.isfile(os.path.join(pipeline.ROOT, 'tools', 'win', 'build3ds.py')),
                       compatList=compat_list,
                       faq=faq, themeMode=theme_mode if theme_mode in ('system', 'light', 'dark') else 'system', dark=False,
                       updateState='', updateTitle='', updateNotes='', updateProgress=0, updateMessage='', releaseTag=updater.own_tag(),
                       updateSetAside=False, buildText='', buildCustom=False)
        self.release, self.update_job, self.check_job, self.update_stop, self.shown, self.ask_again = None, None, None, threading.Event(), False, False
        self.set(updateSetAside=self.set_aside(updater.failed_tags()))
        ready = updater.activated()  # a version downloaded before (Later, or a start with Shift held) is ready without a check
        if updater.tag_key(self._s['releaseTag']) and updater.tag_key(ready) and updater.tag_key(ready) > updater.tag_key(self._s['releaseTag']):
            self.set(updateState='ready', updateTitle=ready)
        self.updateEvent.connect(self.update_event)
        self.auto_check = QTimer(self, singleShot=True, interval=4000)  # a few seconds after the window shows: the start isn't slowed
        self.auto_check.timeout.connect(lambda: self.check_update(False))
        QGuiApplication.styleHints().colorSchemeChanged.connect(self.apply_theme)  # "system" follows Windows switching while the window is open
        self.apply_theme()
        self.coverReady.connect(self.cover_ready)
        self.keyRefused.connect(self.key_refused)
        self.refresh_library()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.refresh_cards()
        self.refresh_files()
        self.refresh_build()

    def set(self, **kw):
        self._s.update(kw)
        self.changed.emit()

    # ---- questions and messages, in the window
    def run_flow(self, gen):
        self.flow = gen
        self.step(None)

    def step(self, value):
        try:
            q = self.flow.send(value)
        except StopIteration:
            self.flow = None
            return
        self.dialogs.append(dict(q, flow=True))
        self.show_dialog()

    def tell(self, title, message, tone='info'):
        self.dialogs.append(dict(title=title, message=message, yes='OK', no='', tone=tone, flow=False))
        self.show_dialog()

    def show_dialog(self):
        head = self.dialogs[0] if self.dialogs else None
        self.set(dialog={k: v for k, v in head.items() if k != 'flow'} if head else None)

    @Slot(bool)
    def answer(self, yes):
        if not self.dialogs:
            return
        head = self.dialogs.pop(0)
        self.show_dialog()
        if head['flow'] and self.flow:
            self.step(yes)

    @Slot()
    def about(self):
        self.tell(f'About {APP}', credits.about(self._s['releaseTag']))

    @Slot(str)
    def setLayout(self, name):
        """the modern window (ui/Main.qml) or the simple one (ui/Simple.qml): kept in the settings, and the window swapped at once (run)"""
        if name not in ('modern', 'simple') or self.busy():
            return
        self.st['layout'] = name
        save_settings(self.st)
        self.layoutWanted.emit(name)

    @Slot(bool)
    def setGameMenu(self, on):
        """Whether to include the overlay in future builds."""
        self.st['game_menu'] = bool(on)
        save_settings(self.st)
        self.set(gameMenu=bool(on))

    @Slot(str)
    def setMenuStyle(self, style):
        if style not in ('new', 'classic'):
            return
        self.st['menu_style'] = style
        save_settings(self.st)
        self.set(menuStyle=style)

    @Slot(str)
    def setMenuShortcut(self, shortcut):
        if shortcut and not pipeline.valid_menu_shortcut(shortcut):
            return
        self.st['menu_shortcut'] = shortcut
        save_settings(self.st)
        self.set(menuShortcut=shortcut)

    @Slot(str)
    def setArtStyle(self, style):
        """the start-up screens: the art as it is, or laid out as the Wii U's Virtual Console shows a game (tools/make_game_title.py, console_screens)"""
        if style not in ('picture', 'console'):
            return
        self.st['art_style'] = style
        save_settings(self.st)
        self.set(artStyle=style)

    @Slot(str)
    def setGameLanguage(self, language):
        if not self.game:
            return
        language = language if language in pipeline.LANGUAGES else ''
        self.st.setdefault('game_languages', {})[f'{self.game.title_id:016x}'] = language
        save_settings(self.st)
        self.set(gameLanguage=language, language=language)

    @Slot(str)
    def setLanguage(self, language):
        self.setGameLanguage(language)

    @Slot(str)
    def setThemeMode(self, mode):
        if mode not in ('system', 'light', 'dark'):
            return
        self.st['theme'] = mode
        save_settings(self.st)
        self.set(themeMode=mode)
        self.apply_theme()

    def apply_theme(self, *_):
        """asks Qt for the chosen scheme, so the native title bar follows it too; Unknown hands "system" back to Windows"""
        hints, mode = QGuiApplication.styleHints(), self._s['themeMode']
        asked = {'dark': Qt.ColorScheme.Dark, 'light': Qt.ColorScheme.Light}.get(mode, Qt.ColorScheme.Unknown)
        if getattr(self, 'asked', None) != asked:  # setColorScheme signals colorSchemeChanged, which calls this again
            self.asked = asked
            hints.setColorScheme(asked)
        # the mode, not colorScheme() alone: a platform without schemes (offscreen) ignores the request
        self.set(dark=mode == 'dark' or mode == 'system' and hints.colorScheme() == Qt.ColorScheme.Dark)

    # ---- choosing
    def open_dump(self, path):
        try:
            return dumps.Dump(path)
        except Exception as e:  # a truncated or foreign file can fail anywhere in its headers
            self.tell('Can\'t read that file', f'{os.path.basename(path)}: {e if isinstance(e, (dumps.DumpError, OSError)) else "not a 3DS dump this can read"}',
                      'error')
            return None

    @Slot()
    def chooseGame(self):
        if self._s['building']:
            return
        path = QFileDialog.getOpenFileName(None, 'Choose a 3DS game', self.st.get('last_dir', ''), DUMPS)[0]
        if path:
            self.open_game(path)

    def open_game(self, path, ask=True):
        self.st['last_dir'] = os.path.dirname(path)
        d = self.open_dump(path)
        if d is None:
            return
        if d.kind in ('update', 'dlc'):
            self.tell(f'That\'s {"an update" if d.kind == "update" else "DLC"}', 'Choose the game itself first: you\'re asked about its update and DLC right after.')
            return
        if d.kind not in ('game', 'demo'):
            self.tell('That isn\'t a game', 'It looks like a system title.', 'error')
            return
        if d.encrypted() and not decrypt.have_keys(self.st['keys']):
            self.tell('This dump is encrypted', 'The builder needs the keys from your own 3DS to read it. Choose the folder with boot9.bin and seeddb.bin '
                                                'under 3DS keys, then pick the game again.', 'warn')
            return
        try:
            decrypt.use_keys(self.st['keys'])
            d.read_names()
        except dumps.DumpError as e:
            self.tell('Couldn\'t read this game', str(e), 'error')
            return
        self.game, self.update, self.dlc, self.mod = d, None, None, None
        language = self.st.get('game_languages', {}).get(f'{d.title_id:016x}')
        info = pipeline.profile_for(d) or pipeline.derived(d)
        known = info['status'] in ('Plays', 'Complete')  # compat.json: tested on a real Wii U, or finished there
        self.set(hasGame=True, gameName=info['name'], maker=d.publisher or '', known=known, badge=f'{info["status"]} on Wii U' if known else 'Untested',
                 icon=icon_url(d.icon) if d.icon else '', meta=f'{d.product}  ·  {d.title_id:016X}  ·  {describe(d)}',
                 language=language if language in pipeline.LANGUAGES else '', gameLanguage=language if language in pipeline.LANGUAGES else '', view='form')
        self.set_extra('update', None)
        self.set_extra('dlc', None)
        self.set_extra('mod', None)
        saved = self.st.get('art', {}).get(f'{d.title_id:016x}')  # a game keeps the banner art chosen for it; '' is "none, on purpose"
        icon = self.stored_icon(d)
        self.set_art(saved if saved and os.path.isfile(saved) else None, icon if icon and os.path.isfile(icon) else None, remember=False)
        bezel = self.st.get('pad_bezel', {}).get(f'{d.title_id:016x}')
        self.set_bezel(bezel if bezel and os.path.isfile(bezel) else None, remember=False)
        pad_art = self.st.get('pad_art', {}).get(f'{d.title_id:016x}')
        self.set_pad_art(pad_art if pad_art and os.path.isfile(pad_art) else None, remember=False)
        sound = self.st.get('boot_sound', {}).get(f'{d.title_id:016x}')
        self.set_game_sound(sound if sound and os.path.isfile(sound) else None, remember=False)
        save = self.st.get('save_in', {}).get(f'{d.title_id:016x}')
        self.set_save(save if save and os.path.isdir(save) else None, remember=False)
        if saved is None:  # nothing chosen yet: its cover art, fetched off the window's thread
            threading.Thread(target=self.fetch_cover, args=(d, info['name']), daemon=True).start()
        self.refresh_files()
        self.remember_game(d, info['name'])
        if ask:  # a game picked again from the library keeps quiet
            self.run_flow(self.extras())

    # ---- the library: the games picked before, as the tiles on the Games page
    def remember_game(self, d, name):
        games = [g for g in self.st.get('library', []) if g['path'] != d.path]
        games.append(dict(path=d.path, name=name, id=f'{d.title_id:016x}', product=d.product or '', icon=icon_url(d.icon) if d.icon else ''))
        self.st['library'] = games[-LIBRARY_MAX:]
        save_settings(self.st)
        self.refresh_library()

    def refresh_library(self):
        """what the window shows of each remembered game: its name, its picture (the cover if this PC has it, else its own icon), and
        which one is open"""
        covers_dir = covers_folder()
        shown = []
        for g in self.st.get('library', []):
            art = (self.st.get('icon_art', {}).get(g['id']) or self.st.get('art', {}).get(g['id'])  # a square icon fills a tile best
                   or os.path.join(covers_dir, (covers.game_code(g['product']) or '-') + '.jpg'))
            cover = QUrl.fromLocalFile(art).toString() if os.path.isfile(art) else ''
            shown.append(dict(path=g['path'], name=g['name'], icon=g['icon'], cover=cover, current=bool(self.game and self.game.path == g['path'])))
        self.set(library=shown)

    @Slot(str)
    def openRecent(self, path):
        if self._s['building'] or (self.game and self.game.path == path):
            return
        if not os.path.isfile(path):
            self.tell('That game has moved', 'Its file is no longer where it was. Choose it again on the Build page (Ctrl+O).', 'warn')
            self.forgetGame(path)
            return
        self.open_game(path, ask=False)

    @Slot(str)
    def forgetGame(self, path):
        self.st['library'] = [g for g in self.st.get('library', []) if g['path'] != path]
        save_settings(self.st)
        self.refresh_library()

    def refresh_note(self):
        result = self.compat_by_id.get(f'{self.game.title_id:016X}', {})
        if result.get('wiiu_status') in ('Plays', 'Complete'):
            badge, note = f'{result["wiiu_status"]} on Wii U', result['wiiu_note']
        elif result.get('cemu_status') == 'Plays':
            badge, note = 'Plays in Cemu', 'Wii U play is unverified. ' + result['cemu_note']
        else:
            badge = 'Untested'
            note = result.get('wiiu_note') or result.get('cemu_note') or 'Wii U play is unverified. The game may fail to start or stop during play.'
        if self.mod:
            badge = 'Untested with mod'
            note += ' The playtest status applies to the game without this mod.'
        self.set(badge=badge, note=note, known=badge.startswith(('Plays', 'Complete')))

    def nearby(self):
        """updates and DLC for this game in the folder it came from"""
        found = {}
        folder = os.path.dirname(self.game.path)
        for n in sorted(os.listdir(folder))[:400]:
            p = os.path.join(folder, n)
            if p == self.game.path or not n.lower().endswith(DUMP_EXTS):
                continue
            try:
                d = dumps.Dump(p)
            except Exception:  # anything else in the folder
                continue
            if d.unique == self.game.unique and d.kind in ('update', 'dlc') and (d.kind not in found or d.version > found[d.kind].version):
                found[d.kind] = d
        return found

    def extras(self):
        """the questions after a game is chosen: its update, then its DLC"""
        found, name = self.nearby(), self._s['gameName']
        for kind, what, why in (('update', 'an update', 'the latest version of the game, with its fixes'),
                                ('dlc', 'DLC', 'its downloadable content: extra levels, items and so on')):
            d = found.get(kind)
            if d:
                if (yield dict(title=f'Found {what} for {name}', message=f'{describe(d)}  ·  {os.path.basename(d.path)}\n\nUse it? It adds {why}.',
                               yes='Use it', no='No thanks', tone='ask')):
                    self.set_extra(kind, d)
            elif (yield dict(title=f'Do you have {what} for {name}?', message=f'Using it adds {why}. Choose "I have it" to pick the file.',
                             yes='I have it', no='No', tone='ask')):
                self.chooseExtra(kind)
        if (yield dict(title=f'Do you have a mod for {name}?', message='A mod made for Luma on a 3DS: a .zip or a folder with luma/titles/<title id>/ '
                       'in it (new files, code.ips, code.bps or code.bin). The builder puts it in before recompiling. Plugins (.3gx, like CTGP-7) '
                       'can\'t be built in.', yes='I have one', no='No', tone='ask')):
            self.chooseMod('zip' if (yield dict(title='A .zip or a folder?', message='How did the mod come?', yes='A .zip', no='A folder', tone='ask'))
                           else 'folder')

    @Slot(str)
    def chooseExtra(self, kind):
        what = 'update' if kind == 'update' else 'DLC'
        path = QFileDialog.getOpenFileName(None, f'Choose the {what} for {self._s["gameName"]}', os.path.dirname(self.game.path), DUMPS)[0]
        if not path:
            return
        d = self.open_dump(path)
        if d is None:
            return
        if d.kind != kind or d.unique != self.game.unique:
            self.tell(f'That isn\'t the {what}', f'It isn\'t the {what} for {self._s["gameName"]}.', 'error')
            return
        self.set_extra(kind, d)

    @Slot(str)
    def chooseMod(self, how):
        start = os.path.dirname(self.game.path)
        if how == 'zip':
            path = QFileDialog.getOpenFileName(None, f'Choose the mod for {self._s["gameName"]}', start, 'Mods (*.zip);;All files (*.*)')[0]
        else:
            path = QFileDialog.getExistingDirectory(None, f'Choose the mod folder for {self._s["gameName"]}', start)
        if not path:
            return
        try:
            m = mods.Mod(path, self.game.title_id)
        except (mods.ModError, OSError) as e:
            self.tell('That mod can\'t be used', str(e), 'error')
            return
        parts = ([f'{len(m.romfs)} files'] if m.romfs else []) + ([m.code.rsplit('/', 1)[-1]] if m.code else []) + (['exheader'] if m.exheader else [])
        m.close()
        self.set_extra('mod', path, ', '.join(parts))

    def ask_picture(self, title):
        """a picture the player picks, or None"""
        path = QFileDialog.getOpenFileName(None, title, self.st.get('art_folder', ''), 'Pictures (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*.*)')[0]
        if not path:
            return None
        if QImage(path).isNull():
            self.tell("That picture can't be read", 'Choose a PNG or JPEG picture.', 'error')
            return None
        self.st['art_folder'] = os.path.dirname(path)
        return path

    @Slot()
    def chooseArt(self):
        path = self.ask_picture(f'Choose start-up art for {self._s["gameName"]}')
        if path:
            self.set_art(path, self.icon_art)  # automatic downloads no longer populate the menu icon

    @Slot()
    def chooseIcon(self):
        path = self.ask_picture(f'Choose a Wii U Menu icon for {self._s["gameName"]}')
        if path:
            if self.game:
                self.st.setdefault('icon_picked', {})[f'{self.game.title_id:016x}'] = path
            self.set_art(self.art, path)

    @Slot()
    def removeArt(self):
        self.set_art(None, removed=True)

    @Slot()
    def removeIcon(self):
        self.set_art(self.art, None)

    @Slot()
    def findArt(self):
        """the game's fetched art and icon again (fetch_cover), in place of pictures the player chose or removed"""
        self.set_art(None, None)  # the game's choice forgotten, so its art is looked for again
        threading.Thread(target=self.fetch_cover, args=(self.game, self._s['gameName']), daemon=True).start()

    @Slot()
    def chooseBezel(self):
        path = self.ask_picture(f'Choose a GamePad bezel for {self._s["gameName"]}')
        if path:
            self.set_bezel(path)

    @Slot()
    def removeBezel(self):
        self.set_bezel(None)

    def set_bezel(self, path, remember=True):
        """the picture the GamePad shows where it shows no screen (tools/make_game_title.py, bezel_pixels), remembered for the game"""
        self.bezel = path
        if self.game and remember:
            self.keep_for_game('pad_bezel', path)
        self.set(bezelText=CUSTOM if path else NO_BEZEL, hasBezel=bool(path))

    def keep_for_game(self, key, path):
        """a path chosen for the open game (settings[key][title id]), or forgotten with None"""
        chosen = self.st.setdefault(key, {})
        if path:
            chosen[f'{self.game.title_id:016x}'] = path
        else:
            chosen.pop(f'{self.game.title_id:016x}', None)
        save_settings(self.st)

    @Slot()
    def choosePadArt(self):
        path = self.ask_picture(f'Choose a GamePad start-up picture for {self._s["gameName"]}')
        if path:
            self.set_pad_art(path)

    @Slot()
    def removePadArt(self):
        self.set_pad_art(None)

    def set_pad_art(self, path, remember=True):
        """the GamePad's start-up screen of its own (tools/make_game_title.py, pad_art), remembered for the game"""
        self.pad_art = path
        if self.game and remember:
            self.keep_for_game('pad_art', path)
        self.set(padArtText=CUSTOM if path else NO_PAD_ART, hasPadArt=bool(path))

    @Slot()
    def chooseGameSound(self):
        sound = os.path.join(GAME_SOUNDS, f'{self.game.title_id:016x}.btsnd')
        if self.ask_sound(f'Choose a start-up sound for {self._s["gameName"]}', sound):
            self.set_game_sound(sound)

    @Slot()
    def removeGameSound(self):
        if self.game_sound:
            try:
                os.remove(self.game_sound)
            except OSError:
                pass  # forgotten either way: the build goes back to the sound for every game
        self.set_game_sound(None)

    def set_game_sound(self, path, remember=True):
        """the game's own start-up sound, a .btsnd made from the player's WAV (tools/make_boot_sound.py), remembered for the game"""
        self.game_sound = path
        if self.game and remember:
            self.keep_for_game('boot_sound', path)
        self.set(gameSoundText=CUSTOM if path else NO_GAME_SOUND, hasGameSound=bool(path))

    @Slot()
    def bringSave(self):
        """a 3DS save to pack into the game's build, which the game takes in on the console (saves.py)"""
        chosen = QFileDialog.getExistingDirectory(None, f"Choose the folder with your 3DS save of {self._s['gameName']}",
                                                  self.st.get('save_folder', ''))
        if not chosen:
            return
        self.st['save_folder'] = os.path.dirname(chosen)
        save_settings(self.st)
        try:
            saves.checked(chosen)
        except (saves.SaveError, OSError) as e:
            self.tell("That folder can't be used", str(e), 'error')
            return
        self.set_save(chosen)

    @Slot()
    def removeSave(self):
        self.set_save(None)

    def set_save(self, path, remember=True):
        """the folder of the save to pack into the build, remembered for the game"""
        self.save_in = path
        if self.game and remember:
            self.keep_for_game('save_in', path)
        self.set(saveText=os.path.basename(os.path.normpath(path)) if path else NO_SAVE, hasSave=bool(path))

    def fetch_cover(self, game, name):
        """on a thread of its own: the game's banner from SteamGridDB where the player gave a key, its cover from GameTDB otherwise
        and for what SteamGridDB lacks (covers.py), handed to the window's thread when they're here"""
        folder, title_id, key = covers_folder(), f'{game.title_id:016x}', self.st.get('sgdb_key')
        found = {}
        try:
            # a game SteamGridDB doesn't know by this region's name is looked for by the others' (GameTDB's list): Luigi's Mansion 2 is Dark Moon
            also = lambda: covers.other_names(game.product, folder)  # noqa: E731
            found = (covers.steamgriddb(name, key, folder, title_id, also=also) if key and key != self.refused_key else None) or {}
        except covers.BadKey:
            self.refused_key = key  # not asked with again until it's changed, and said once
            self.keyRefused.emit()
        banner = found.get('banner') or covers.cover(game.product, folder)
        if banner:
            self.coverReady.emit(title_id, banner, '')

    def cover_ready(self, title_id, path, icon):
        # fetched art where the game has none; an icon the player picked stays
        if self.game and f'{self.game.title_id:016x}' == title_id and not self.art and not self._s['building']:
            self.set_art(path or None, self.icon_art)
        self.refresh_library()

    def stored_icon(self, game):
        key = f'{game.title_id:016x}'
        icon = self.st.get('icon_art', {}).get(key)
        # Keep explicit choices, including ones inside the cache. Old cache files remain on disk and in settings.
        if icon and self.st.get('icon_picked', {}).get(key) != icon and fetched_icon(icon, game):
            return None
        return icon

    def key_refused(self):
        self.tell("SteamGridDB didn't take the key", 'Check it on the Help page, under Art and sound: it is the long code on steamgriddb.com under '
                  'Preferences, API. Until then the art comes from GameTDB.', 'warn')

    def set_art(self, path, icon=None, remember=True, removed=False):
        """Remember start-up art and an optional complete menu icon. Without an override, preview the shared SMDH composition.
        removed: the player took the art away, remembered as '' so none is fetched again"""
        self.art, self.icon_art = path, icon
        if self.game and remember:
            title, arts, icons = f'{self.game.title_id:016x}', self.st.setdefault('art', {}), self.st.setdefault('icon_art', {})
            if path or removed:
                arts[title] = path or ''
            else:
                arts.pop(title, None)  # none chosen yet: the next time the game opens, its art is looked for again
            if icon:
                icons[title] = icon
            else:
                icons.pop(title, None)
            if self.st.get('icon_picked', {}).get(title) != icon:
                self.st.get('icon_picked', {}).pop(title, None)
            save_settings(self.st)
        url = lambda p: QUrl.fromLocalFile(p).toString() if p else ''  # noqa: E731
        default_icon = icon_url(pipeline.tool('make_game_title').vc_icon_pixels(self.game.icon)) if self.game and self.game.icon and not icon else ''
        own = bool(self.game) and (picked(path) or bool(icon) or self.st.get('art', {}).get(f'{self.game.title_id:016x}') == '')  # findArt undoes it
        self.set(artText=source(path) if path else NO_ART, hasArt=bool(path or icon), artUrl=url(path), artOwn=own,
                 iconText=CUSTOM if icon else NO_ICON_ART, hasIconArt=bool(icon), iconArtUrl=url(icon) or default_icon,
                 tileUrl=url(icon) or default_icon or url(path))
        if remember:
            self.refresh_library()

    @Slot()
    def setArtKey(self):
        key, ok = QInputDialog.getText(None, 'SteamGridDB', 'Paste your SteamGridDB API key (on steamgriddb.com, under Preferences > API).\n'
                                       'It is only saved on this PC. Leave it empty to stop using SteamGridDB.')
        if not ok:
            return
        self.st['sgdb_key'] = key.strip()
        save_settings(self.st)
        self.set(artKeySet=bool(self.st['sgdb_key']))
        removed = self.game and self.st.get('art', {}).get(f'{self.game.title_id:016x}') == ''  # on purpose: it stays removed
        if self.game and self.st['sgdb_key'] and not self._s['building'] and not removed and (not self.art or source(self.art) == FROM_TDB):
            # the open game's fetched art, again from the new place; a picture the player chose stays
            self.set_art(None, self.icon_art, remember=False)
            threading.Thread(target=self.fetch_cover, args=(self.game, self._s['gameName']), daemon=True).start()

    def ask_sound(self, title, to):
        """a sound the player picks, made into a start-up sound at to (tools/make_boot_sound.py); False if none, or after saying why not"""
        path = QFileDialog.getOpenFileName(None, title, self.st.get('sound_folder', ''), 'Sounds (*.wav *.btsnd);;All files (*.*)')[0]
        if not path:
            return False
        self.st['sound_folder'] = os.path.dirname(path)
        save_settings(self.st)
        try:
            data = pipeline.tool('make_boot_sound').boot_sound(path)
            os.makedirs(os.path.dirname(to), exist_ok=True)
            with open(to, 'wb') as f:
                f.write(data)
        except (OSError, ValueError, EOFError, ImportError) as e:
            self.tell("That sound can't be used", f'{e}. Choose a WAV file: most sound programs can save one.', 'error')
            return False
        return True

    @Slot()
    def chooseSound(self):
        if self.ask_sound('Choose a start-up sound for all games', BOOT_SOUND):
            self.set(soundSet=True)

    @Slot()
    def removeSound(self):
        try:
            if os.path.isfile(BOOT_SOUND):
                os.remove(BOOT_SOUND)
        except OSError as e:
            self.tell("The sound couldn't be removed", str(e), 'error')
        self.set(soundSet=os.path.isfile(BOOT_SOUND))

    @Slot(str)
    def removeExtra(self, kind):
        self.set_extra(kind, None)

    def set_extra(self, kind, d, what=''):
        setattr(self, kind, d)
        if kind == 'mod':
            self.set(modText=f'{what}  ·  {os.path.basename(d)}' if d else 'None', hasMod=d is not None)
            if self.game:
                self.refresh_note()
            return
        text = f'{describe(d)}  ·  {os.path.basename(d.path)}' if d else 'None'  # the window cuts it to fit
        if kind == 'update':
            self.set(updateText=text, hasUpdate=d is not None)
            if self.game:
                self.refresh_note()
        else:
            self.set(dlcText=text, hasDlc=d is not None)

    @Slot()
    def chooseKeys(self):
        p = QFileDialog.getExistingDirectory(None, 'Choose the folder with your boot9.bin', self.st['keys'])
        if p:
            if not decrypt.have_keys(p):
                self.tell('No keys there', 'There\'s no boot9.bin in that folder.', 'warn')
            self.st['keys'] = p
            decrypt.use_keys(p)
            # the system files, chosen before the keys that open them, or beside boot9.bin in one folder of the 3DS's files
            for folder in (self.st.get('system'), p):
                if folder and not dumps.have_system_archives(pipeline.ARCHIVES) and dumps.nand_titles(folder):
                    self.import_system(folder)
            self.refresh_files()

    @Slot()
    def chooseSystem(self):
        p = QFileDialog.getExistingDirectory(None, 'Choose the folder with 0004009b and 000400db, or Azahar\'s user folder', self.st.get('system', ''))
        if p:
            got = self.import_system(p, report_busy=True)
            if got is not None and not got:
                if not self.system_locked:
                    self.tell('No 3DS system files there', HELP['system'][1], 'warn')
                elif decrypt.have_keys(self.st['keys']):
                    self.tell('Those files didn\'t open', f'That is the right folder, but its files couldn\'t be decrypted with the boot9.bin in '
                              f'{self.st["keys"]}. They open with the keys of the same 3DS they were copied from.', 'warn')
                else:
                    self.tell('Those files need your boot9.bin', 'That is the right folder, but a 3DS keeps these files encrypted, and they '
                              'open with boot9.bin from your own 3DS.\n\nPut boot9.bin in this folder and choose it again, or press "Folder" '
                              'next to 3DS keys and choose the folder boot9.bin is in.', 'warn')
            self.refresh_files()

    @Slot(str)
    def explain(self, what):
        self.tell(*HELP[what])

    def import_system(self, folder, report_busy=False):
        """the system archives from a folder into the ones every build takes (pipeline.ARCHIVES), once: they stay for the next games. A
        boot9.bin there, or beside the 0004009b folder chosen, is taken for the keys when the builder has none: a tester kept all of the
        3DS's files in one folder and chose it for the system files only"""
        self.system_locked = False
        try:
            with pipeline.workspace_lock(pipeline.WORK):
                return self.import_system_locked(folder)
        except (OSError, ValueError) as e:
            if report_busy:
                self.tell('3DS system files could not be imported', str(e), 'warn')
            return None

    def import_system_locked(self, folder):
        if not decrypt.have_keys(self.st['keys']):
            for keys in (folder, os.path.dirname(os.path.normpath(folder))):
                if decrypt.have_keys(keys):
                    self.st['keys'] = keys
                    decrypt.use_keys(keys)
                    save_settings(self.st)
                    break
        tmp = tempfile.mkdtemp(prefix='3diiu_sys_')  # for contents that need decrypting first
        try:
            got = dumps.import_system_archives(folder, pipeline.ARCHIVES, tmp)
        except decrypt.DecryptError:
            got, self.system_locked = [], True
        except (OSError, dumps.DumpError):
            got = []
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        if got or self.system_locked:  # a locked folder is kept: it's read again once the keys are chosen
            self.st['system'] = folder
            save_settings(self.st)
        return got

    def find_system(self, everywhere):
        """the system files from an emulator's folder on this PC, if the builder has none yet"""
        for folder in [] if dumps.have_system_archives(pipeline.ARCHIVES) else emulator_folders(everywhere):
            if self.import_system(folder) and dumps.have_system_archives(pipeline.ARCHIVES):
                break
        self.refresh_files()

    def came_back(self, state):
        """Back in this window, from Azahar's File > Set Up System Files say: what that left is taken without being pointed at, as the help
        says it is. Only the emulators' own folders are looked in, which costs nothing."""
        if state == Qt.ApplicationState.ApplicationActive and not self._s['building']:
            self.find_system(False)

    @Slot()
    def chooseOtp(self):
        p = QFileDialog.getOpenFileName(None, 'Choose your Wii U\'s otp.bin', os.path.dirname(self.st.get('otp', '')), 'otp.bin (otp.bin);;All files (*.*)')[0]
        if p:
            self.st['otp'] = p
            self.refresh_files()

    @Slot(str)
    def setOutputFormat(self, value):
        if self._s['building'] or value not in ('wup','wup-update','homebrew'): return
        self.st['output_format'] = value
        self.set(outputFormat=value)
        save_settings(self.st)

    @Slot(str)
    def setWupUpdateParts(self, value):
        if self._s['building'] or value not in ('binary','both'): return
        self.st['wup_update_parts'] = value
        self.set(wupUpdateParts=value)
        save_settings(self.st)

    @Slot()
    def chooseWupBase(self):
        if self._s['building']: return
        path = QFileDialog.getOpenFileName(None, 'Choose the matching full WUP base manifest',
                       os.path.dirname(self.st.get('wup_base_manifest','')), 'WUP base manifests (*.base.json);;JSON files (*.json)')[0]
        if path:
            self.st['wup_base_manifest'] = path
            self.set(wupBaseText=path, hasWupBase=True)
            save_settings(self.st)

    @Slot()
    def clearWupBase(self):
        if self._s['building']: return
        self.st.pop('wup_base_manifest',None)
        self.set(wupBaseText='Last full base built in this work folder',hasWupBase=False)
        save_settings(self.st)

    @Slot(str)
    def setHomebrewParts(self, value):
        if self._s['building'] or value not in ('full','binary','shaders','both'): return
        self.st['homebrew_parts'] = value
        self.set(homebrewParts=value)
        save_settings(self.st)

    @Slot()
    def homebrewGuide(self):
        path = os.path.join(HERE, 'HOMEBREW_GUIDE.txt')
        if WIN: os.startfile(path)
        else: subprocess.Popen(['xdg-open', path])

    @Slot(str)
    def setTarget(self, t):
        self.st['target'] = t
        self.set(target=t)
        self.refresh_files()

    @Slot()
    def chooseFolder(self):
        p = QFileDialog.getExistingDirectory(None, 'Choose where to save the game', self.st.get('folder', ''))
        if p:
            self.st['folder'] = p
            self.setTarget('folder')

    # ---- where games are built (pipeline.BUILD): gigabytes of build files, on a drive the player chooses
    def build_folder(self):
        return os.environ.get('THREEDII_BUILD') or pipeline.BUILD

    def refresh_build(self):
        folder = self.build_folder()
        try:
            free = f'{shutil.disk_usage(folder).free / 2 ** 30:.0f} GB free'
        except OSError:
            free = 'not there yet'
        self.set(buildText=f'{short(folder, 60)}  ·  {free}', buildCustom=bool(self.st.get('build_folder')))

    @Slot()
    def chooseBuild(self):
        p = QFileDialog.getExistingDirectory(None, 'Choose a folder for building games', self.build_folder())
        if p:
            self.set_build(os.path.normpath(p))

    @Slot()
    def defaultBuild(self):
        self.set_build('')

    def set_build(self, chosen):
        """Builds go to the chosen folder from the next one on ('' for the usual one). The old folder's build files are only a cache, so
        deleting them is offered: the point was room on that drive."""
        if self.busy():
            return
        old, new = self.build_folder(), chosen or os.environ.get('THREEDII_WORK') or pipeline.WORK
        try:
            os.makedirs(new, exist_ok=True)
            probe = os.path.join(new, '.3diiu_check')
            open(probe, 'wb').close()
            os.remove(probe)
        except OSError as e:
            self.tell('That folder can\'t be used', f'The builder can\'t write there: {e.strerror or e}', 'warn')
            return
        self.st['build_folder'] = chosen
        save_settings(self.st)
        os.environ['THREEDII_BUILD'] = new
        self.refresh_build()
        if os.path.normcase(os.path.abspath(old)) != os.path.normcase(os.path.abspath(new)):
            stale = [os.path.join(old, n) for n in pipeline.BUILD_FILES if os.path.isdir(os.path.join(old, n))]
            size = sum(os.path.getsize(os.path.join(d, f)) for s in stale for d, _, fs in os.walk(s) for f in fs)
            if size >> 20:
                self.run_flow(self.drop_build_files(stale, size))

    def drop_build_files(self, folders, size):
        if (yield dict(title='Delete the old build files?', message=f'{size / 2 ** 30:.1f} GB in {os.path.dirname(folders[0])}. The builder makes '
                       'them again as it needs them.', yes='Delete', no='Keep', tone='ask')):
            try:
                with pipeline.workspace_lock(os.path.dirname(folders[0])):
                    for f in folders:
                        folder, _ = pipeline._checked(f, directory=True)
                        shutil.rmtree(folder, ignore_errors=True)
            except (OSError, ValueError) as e:
                self.tell('Build files could not be deleted', str(e), 'warn')

    @Slot(str)
    def pickCard(self, path):
        self.st['card'] = path
        self.setTarget('card')

    @Slot()
    def refreshCards(self):
        self.refresh_cards()
        self.refresh_files()

    def refresh_cards(self):
        cards = sd_cards()
        if self.st.get('card') not in [path for path, _ in cards]:
            # chosen without asking only where it can't be some other disk: removable, or set up for a Wii U, never a USB hard drive
            self.st['card'] = next((path for path, removable in cards if removable or wiiu_card(path)), '')
        self.set(cards=[dict(path=path, label=f'{path}    {"set up for a Wii U" if wiiu_card(path) else "removable drive" if removable else "card reader"}')
                        for path, removable in cards])

    def refresh_files(self):
        keys, otp = self.st['keys'], self.st.get('otp', '')
        needed = bool(self.game and self.game.encrypted())
        card, folder = self.st.get('card', ''), self.st.get('folder', '')
        if self._s['target'] == 'card':
            dest = f'{card}   {"Wii U card" if os.path.isdir(os.path.join(card, "wiiu")) else "SD card"}' if card else 'No SD card found'
        else:
            dest = short(folder, 40) if folder else 'Choose a folder...'
        have = decrypt.have_keys(keys)
        system = dumps.have_system_archives(pipeline.ARCHIVES)
        source = short(self.st['system'], 26) if self.st.get('system') else ''
        # short, and what to choose while there's nothing: the row has its pills beside it
        self.set(sysOk=system, sysText=(source or 'Found') if system else 'Folder with 0004009b')
        self.set(keysOk=have, keysNeeded=needed and not have,
                 keysText=short(keys, 26) if have else 'Folder with boot9.bin' if needed else 'Only for encrypted dumps',
                 otpOk=otp_ok(otp), otpText=short(otp, 26) if otp_ok(otp) else 'Not found yet', destText=dest)

    # ---- updates (updater.py): every call that reaches the network or the disk runs on a worker, which reports through updateEvent
    @Slot()
    def windowShown(self):
        """The window's first frame, or run()'s timer when no frame comes: tells updater a downloaded version came up (or every update would
        be set aside after its first use), then, in a released app only, looks for a newer release a few seconds later"""
        if self.shown:
            return
        self.shown = True
        try:  # create()'s connection to every frame: the first was the one wanted
            QObject.disconnect(self.frames)
        except Exception:  # started() must run whatever happens to the connection
            pass
        threading.Thread(target=updater.started, daemon=True).start()  # it waits on the launch record's lock
        if FROZEN and updater.tag_key(self._s['releaseTag']):  # a development build checks only when asked
            self.auto_check.start()

    def set_aside(self, failed):
        """whether a release newer than this app failed to start here: older failures it has moved past aren't worth a note"""
        mine = updater.tag_key(self._s['releaseTag'])
        return any(mine is None or updater.tag_key(t) > mine for t in failed)

    @Slot()
    def checkUpdate(self):
        self.check_update(True)

    def check_update(self, asked):
        """asked: the person pressed Check for updates, so a release that failed to start here before is offered again"""
        if self._s['updateState'] in ('downloading', 'unpacking', 'ready', 'restarting'):
            return  # the About line says what is under way
        if self.check_job and self.check_job.is_alive():
            if asked:  # the automatic check is under way: an asked one follows it, so its answer is shown
                self.ask_again = True
                self.set(updateState='checking', updateMessage='')
            return
        if asked:
            self.set(updateState='checking', updateMessage='')
        own = self._s['releaseTag']

        def job():
            release = updater.check(own, skip_failed=False) if asked else updater.check(own)
            self.updateEvent.emit('checked', (release, asked, updater.failed_tags(), updater.activated()))
        self.check_job = threading.Thread(target=job, daemon=True)
        self.check_job.start()

    @Slot()
    def startUpdate(self):
        """Update, or Try again: the whole update, which also clears a failed mark left on that release"""
        if self._s['updateState'] not in ('available', 'failed') or (self.update_job and self.update_job.is_alive()):
            return
        if not self.release:  # a version made ready before this start, whose restart failed: find its release again
            self.check_update(True)
            return
        release = self.release
        self.update_stop.clear()
        self.set(updateState='downloading', updateProgress=0, updateMessage='')

        def job():
            shown = [-1]

            def progress(done, size):
                percent = min(100, done * 100 // max(1, size))
                if percent != shown[0]:  # a signal per percent, not per piece
                    shown[0] = percent
                    self.updateEvent.emit('progress', percent)
            try:
                zip_path = updater.download(release, updater.home(), progress, self.update_stop.is_set)
                if zip_path is None:
                    self.updateEvent.emit('cancelled', None)
                    return
                self.updateEvent.emit('unpacking', None)
                updater.activate(updater.unpack(zip_path, updater.home()))
                self.updateEvent.emit('ready', updater.failed_tags())
            except updater.UpdateError as e:
                self.updateEvent.emit('failed', str(e))
            except Exception as e:  # anything else is said the same way, and the app carries on
                self.updateEvent.emit('failed', f'The update stopped: {e}')
        self.update_job = threading.Thread(target=job, daemon=True)
        self.update_job.start()

    @Slot()
    def cancelUpdate(self):
        self.update_stop.set()

    @Slot()
    def laterUpdate(self):
        self.set(updateState='')

    @Slot()
    def restartForUpdate(self):
        if self._s['updateState'] != 'ready':
            return
        if self.busy():
            self.tell('A build is running', 'Restart once the build has finished, or cancel it first.', 'warn')
            return
        self.set(updateState='restarting', updateMessage='Starting the new version...')  # no build starts meanwhile

        def job():
            exe = updater.current(updater.own_tag())
            self.updateEvent.emit('quit' if exe and updater.hand_over(exe, []) else 'restart failed', None)
        self.update_job = threading.Thread(target=job, daemon=True)
        self.update_job.start()

    @Slot(str, object)
    def update_event(self, what, value):
        """a worker's result, on the window's thread"""
        if what == 'checked':
            self.check_job = None  # its last act was to send this: a check asked for meanwhile can start, though the thread may not have ended
            release, asked, failed, ready = value
            self.set(updateSetAside=self.set_aside(failed))
            if self.ask_again and not asked:
                self.ask_again = False
                self.check_update(True)
                return
            if self._s['updateState'] in ('downloading', 'unpacking', 'ready', 'restarting'):
                return
            if release:
                self.release = release
                self.set(updateState='ready' if release.tag == ready else 'available', updateTitle=release.title,
                         updateNotes=release.notes[:400], updateMessage='')  # ready: downloaded already, and Later was pressed
            elif asked:
                self.set(updateState='none')
        elif what == 'progress':
            self.set(updateProgress=value)
        elif what == 'cancelled':
            self.set(updateState='available', updateProgress=0)
        elif what == 'unpacking':
            self.set(updateState='unpacking', updateProgress=100)
        elif what == 'ready':
            self.set(updateState='ready', updateSetAside=self.set_aside(value), updateMessage='')
        elif what == 'failed':
            self.set(updateState='failed', updateMessage=value)
        elif what == 'restart failed':
            self.set(updateState='failed', updateMessage='The new version didn\'t start, so this one keeps running.')
        elif what == 'quit':
            self.stop_for_close()  # a build left running would go on behind the new window
            QApplication.quit()

    # ---- building
    @Slot()
    def build(self):
        if job := self.build_job():
            self.launch_build(job)

    def build_job(self):
        if not self.game or self.busy() or self._s['updateState'] == 'restarting':
            return
        if not self._s['rootOk']:
            self.tell('The build engine is missing', 'The engine folder next to 3Dii U Builder.exe holds the recompiler and its toolchain. Copy the '
                                                     'whole 3Dii U Builder folder again, engine folder included.', 'error')
            return
        if self._s['outputFormat'] != 'homebrew' and not otp_ok(self.st.get('otp', '')):
            self.tell('otp.bin is missing', 'Choose your Wii U\'s otp.bin: it should be exactly 1024 bytes.', 'warn')
            return
        dest = self.st.get('card', '') if self._s['target'] == 'card' else self.st.get('folder', '')
        if not dest or not os.path.isdir(dest):
            self.destWanted.emit()
            self.tell('Where should it go?', 'Choose an SD card or a folder for the game.', 'warn')
            return
        partial = self._s['outputFormat'] == 'homebrew' and self._s['homebrewParts'] != 'full'
        if not partial: self.find_system(True)
        if not partial and not dumps.have_system_archives(pipeline.ARCHIVES):
            self.tell('The 3DS system files are missing', HELP['system'][1], 'warn')
            return
        save_settings(self.st)
        argv = [sys.executable, '--pipeline'] if FROZEN else [sys.executable.replace('pythonw.exe', 'python.exe'), '-u', os.path.join(HERE, 'pipeline.py')]
        card = self._s['target'] == 'card'
        argv += ['--game', self.game.path, '--output-format', self._s['outputFormat'],
                 '--card' if self._s['target'] == 'card' else '--out', dest, '--keys', self.st['keys']]
        if self._s['outputFormat'] != 'homebrew': argv += ['--otp', self.st['otp']]
        if self._s['outputFormat'] == 'wup-update':
            argv += ['--wup-update-parts', self._s['wupUpdateParts']]
            if self.st.get('wup_base_manifest'): argv += ['--wup-base-manifest', self.st['wup_base_manifest']]
        elif self._s['outputFormat'] == 'homebrew': argv += ['--homebrew-parts', self._s['homebrewParts']]
        system = self.st.get('system', '')
        if system:
            argv += ['--emulator', system]
        for kind in ('update', 'dlc'):
            if getattr(self, kind):
                argv += [f'--{kind}', getattr(self, kind).path]
        if self.mod:
            argv += ['--mod', self.mod]
        argv += ['--art-style', self._s['artStyle']]
        if self.art:
            argv += ['--art', self.art, '--tdb', covers_folder()]
        if self.icon_art:
            argv += ['--icon-art', self.icon_art]
        if self.bezel:
            argv += ['--pad-bezel', self.bezel]
        if self.pad_art:
            argv += ['--pad-art', self.pad_art]
        if self.game_sound:
            argv += ['--boot-sound', self.game_sound]
        if self.save_in:
            argv += ['--save', self.save_in]
        if self._s['gameMenu'] and self._s['menuStyle'] == 'classic':
            argv += ['--menu-style', 'classic']
        if not self._s['gameMenu']:
            argv += ['--no-menu']
        elif self._s['menuShortcut']:
            argv += ['--menu-shortcut', self._s['menuShortcut']]
        argv += ['--language', self._s['gameLanguage'] or self.system_language]
        env = dict(os.environ, PYTHONIOENCODING='utf-8', THREEDII_BUILD=self.build_folder())
        return BuildJob(self._s['gameName'], tuple(argv), dest, card, env, system, output_format=self._s['outputFormat'], homebrew_parts=self._s['homebrewParts'], wup_update_parts=self._s['wupUpdateParts'])

    def refresh_queue(self):
        rows = [dict(name=job.name, status=job.status, detail=job.detail) for job in self.jobs]
        self.set(queueItems=rows, queuePending=sum(job.status == 'Queued' for job in self.jobs))

    @Slot()
    def enqueueBuild(self):
        job = self.build_job()
        if not job:
            return
        info = dict(pipeline.profile_for(self.game) or pipeline.derived(self.game))
        if job.output_format in ('wup', 'wup-update'):
            original = int(info['title_id'], 16)
            try: native_id = pipeline.wup_update.native_base_id(original, self.game.title_id)
            except ValueError as error:
                self.tell('This WUP identity is unsupported', str(error), 'error')
                return
            if native_id != original:
                info['folder'] += '_NativeWUP'
        dest = os.path.join(job.destination, 'install') if job.card else job.destination
        if job.output_format == 'homebrew':
            dest = os.path.join(job.destination, 'wiiu', 'apps') if job.card else job.destination
        job.output = os.path.normcase(os.path.realpath(os.path.join(dest, info['folder'] + ('_Update' if job.output_format == 'wup-update' else ''))))
        if any(queued.status == 'Queued' and queued.output == job.output for queued in self.jobs):
            self.tell('That output is already queued', 'Remove the earlier entry or choose a different destination for this build.', 'warn')
            return
        job.detail = job.destination
        self.jobs.append(job)
        self.refresh_queue()

    @Slot()
    def startQueue(self):
        if self.busy() or not self._s['queuePending'] or self._s['updateState'] == 'restarting':
            return
        self.set(queueRunning=True)
        self.start_next_build()

    def start_next_build(self):
        if not self._s['queueRunning']:
            return
        job = next((job for job in self.jobs if job.status == 'Queued'), None)
        if job is None:
            self.set(queueRunning=False)
            return
        job.status = 'Building'
        self.refresh_queue()
        self.launch_build(job)

    @Slot(int)
    def removeQueued(self, index):
        if not self.busy() and 0 <= index < len(self.jobs) and self.jobs[index].status == 'Queued':
            del self.jobs[index]
            self.refresh_queue()

    @Slot()
    def clearQueue(self):
        if not self.busy():
            self.jobs.clear()
            self.refresh_queue()

    def launch_build(self, job):
        self.active_build = job
        self.log_lines, self.result, self.cancelled = [], None, False
        stages = [n for n, _ in pipeline.stages_for(job.output_format, job.wup_update_parts if job.output_format == 'wup-update' else job.homebrew_parts if job.output_format == 'homebrew' else 'full') if n != 'Copying' or job.card]
        self.set(building=True, buildName=job.name, view='building', stages=stages, stage=0,
                 failed=False, progress=0.0, detail='Starting...', log='', resultText='', where='')
        if job.system_folder and not os.path.isdir(job.system_folder):
            self.lines.put('The selected 3DS system folder is unavailable. Using cached files; reconnect it or choose its new location under 3DS system files.')
        try:
            if not os.path.isdir(job.destination):
                raise OSError(f'The destination is unavailable: {job.destination}')
            self.proc = subprocess.Popen(job.argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace',
                                         creationflags=NO_WINDOW, start_new_session=not WIN, env=job.env)
        except OSError as e:
            self.lines.put('@error ' + ' '.join(str(e).splitlines()))
            self.lines.put(None)
        else:
            threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()
        self.timer.start(100)

    def read(self, proc):
        try:
            with proc.stdout:
                for line in proc.stdout:
                    self.lines.put(line.rstrip('\n'))
        finally:
            proc.wait()
            self.lines.put(None)

    def poll(self):
        done, logged = False, False
        try:
            while True:
                line = self.lines.get_nowait()
                if line is None:
                    done = True
                    break
                if line.startswith('@stage '):
                    name = line[7:]
                    if name in self._s['stages']:
                        self.set(stage=self._s['stages'].index(name))
                elif line.startswith('@progress '):
                    self.set(progress=float(line.split()[1]) / 100)  # the pipeline's 0-100; the window's bars go 0 to 1
                elif line.startswith(('@done ', '@error ')):
                    self.result = line
                else:
                    self.log_lines.append(line)
                    logged = True
                    if line.strip():
                        self.set(detail=short(line.strip(), 96, end=True))
        except queue.Empty:
            pass
        if logged:
            self._s['log'] = '\n'.join(self.log_lines[-500:])
            self.logChanged.emit()
        if not done:
            return
        self.timer.stop()
        job, self.active_build = self.active_build, None
        self.proc = None
        if self.result and self.result.startswith('@done '):
            where = self.result[6:]
            job.status, job.detail = 'Done', where
            msg = (f'{job.name} is on your SD card, in install\\{os.path.basename(where)}. Put the card in your Wii U and install it with WUP '
                   f'Installer GX2.' if job.card else
                   f'The WUP is in {where}. Copy it to your SD card\'s install folder and install it with WUP Installer GX2.')
            if job.output_format == 'homebrew':
                msg = (f'Homebrew is ready at {where}. Launch its icon with Aroma.' if job.homebrew_parts == 'full' else
                       f'Homebrew update files are ready in {where}. Follow the included Homebrew import guide to apply them to your existing WUHB.')
            elif job.output_format == 'wup-update':
                msg += ' Install the update to the same device as the matching base game. Keep the base game and its saves installed.'
            else: msg += ' If an older build of it is installed, delete that first.'
            self.set(building=False, view='done', stage=len(self._s['stages']), where=where, resultText=msg)
        elif self.cancelled:
            job.status = 'Cancelled'
            self.set(building=False, view='form')
        else:
            job.status = 'Failed'
            job.detail = self.result[7:] if self.result else self.log_lines[-1] if self.log_lines else 'The build stopped.'
            self.set(building=False, view='failed', failed=True, resultText=job.detail)
        if job in self.jobs:
            self.refresh_queue()
            self.start_next_build()

    @Slot()
    def cancel(self):
        self.set(queueRunning=False)
        if self.proc and self.proc.poll() is None:
            self.cancelled = True
            self.set(detail='Stopping...')
            if WIN:  # the build's own compilers go too
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.pid)], creationflags=NO_WINDOW, capture_output=True)
            else:
                import signal
                try:
                    os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
                except ProcessLookupError:
                    pass

    @Slot()
    def back(self):
        self.set(view='form')

    @Slot()
    def openFolder(self):
        where = self._s['where']
        if os.path.isfile(where): where = os.path.dirname(where)
        if os.path.isdir(where):
            os.startfile(where) if WIN else subprocess.Popen(['xdg-open', where])

    @Slot(result=bool)
    def busy(self):
        return self._s['building'] or self._s['queueRunning']

    def stop_for_close(self):
        """the app is quitting: a build and an update download stop with it"""
        self.cancel()
        self.cancelUpdate()


def window_qml(layout):
    """the window the settings' layout asks for: the simple one, or the modern one"""
    return QUrl.fromLocalFile(os.path.join(HERE, 'ui', 'Simple.qml' if layout == 'simple' else 'Main.qml'))


def place(win):
    """a window sized to its screen's free area and centred on it: the modern one 16:9 up to 1280 wide, the simple one at its own size"""
    area = win.screen().availableGeometry()  # its own screen's free area, not the whole desktop: "Build It!" stays above the taskbar
    if win.objectName() == 'simple':
        w, h = min(win.width(), area.width()), min(win.height(), area.height())
        win.setMinimumSize(QSize(min(640, area.width()), min(480, area.height())))
    else:
        w = int(min(1280, area.width() * 0.9, area.height() * 0.9 * 16 / 9))
        h = w * 9 // 16
        win.setMinimumSize(QSize(min(960, area.width()), min(540, area.height())))
    win.setGeometry(area.x() + (area.width() - w) // 2, area.y() + (area.height() - h) // 2, w, h)


def create(argv=None):
    """the application, its window and the App behind it, not yet running"""
    QApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    qt = QApplication(argv or sys.argv)
    qt.setApplicationName(APP)
    qt.setWindowIcon(QIcon(QPixmap.fromImage(draw_icon(256))))
    if WIN:
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('3DiiU.Builder')  # the taskbar shows this app's icon, not Python's
    state = App()
    qt.applicationStateChanged.connect(state.came_back)
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty('app', state)
    engine.rootContext().setContextProperty('T', theme())
    engine.rootContext().setContextProperty('appIcon', png_url(draw_icon(512)))  # the rail shows it at 162: 512 stays sharp up to a 4K window
    engine.load(window_qml(state.st.get('layout')))
    if not engine.rootObjects():  # the saved layout's window didn't load: the modern one, so the app always shows one
        state.st['layout'] = 'modern'
        save_settings(state.st)
        engine.load(window_qml('modern'))
    for win in engine.rootObjects():  # its first frame on screen is when it has really been shown (App.windowShown)
        quick = shiboken6.wrapInstance(shiboken6.getCppPointer(win)[0], QQuickWindow)
        # it comes at every frame, from the render thread: windowShown ends the connection at the first
        state.frames = quick.frameSwapped.connect(state.windowShown, Qt.ConnectionType.QueuedConnection)
    return qt, engine, state


def run():
    qt, engine, state = create()
    if not engine.rootObjects():
        return 1
    win = engine.rootObjects()[0]  # loaded hidden, so a check can load it without showing it
    place(win)
    win.show()

    def swap(layout):  # the other window, shown before this one goes, so the app never has no window
        old = engine.rootObjects()[-1]
        engine.load(window_qml(layout))
        new = engine.rootObjects()[-1]
        if new is old:  # it didn't load: the window stays as it was
            return
        place(new)
        new.show()
        old.close()
        old.deleteLater()
    state.layoutWanted.connect(swap)
    # The first frame is the sign the window came up. A window started minimised draws none, so a timer says it for one that isn't showing.
    # One that is showing and has still drawn nothing is broken, and is left unconfirmed: updater sets that version aside.
    qt.aboutToQuit.connect(state.stop_for_close)  # not a swap: the new window is shown before the old one closes
    QTimer.singleShot(10000, lambda: None if not engine.rootObjects() or engine.rootObjects()[-1].isExposed() else state.windowShown())
    code = qt.exec()
    del engine  # before the App it shows, or its bindings read a gone app on the way out
    return code
