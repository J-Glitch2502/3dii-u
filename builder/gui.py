"""The builder's window. ui/Main.qml draws it; App holds what it shows and does what its buttons ask, running pipeline.py for the build.

Questions come up inside the window one at a time: a choice that asks several things is a generator that yields each question and is sent the
answer (App.run_flow), and messages wait their turn in the same queue."""
import base64
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading

from PySide6.QtCore import QBuffer, QIODevice, QObject, Property, QStandardPaths, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QColor, QFontDatabase, QIcon, QImage, QPixmap
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtWidgets import QApplication, QFileDialog

import decrypt
import dumps
import mods
import pipeline

APP, VERSION = '3Dii U Builder', '1.0'
HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, 'frozen', False)
WIN = sys.platform == 'win32'
NO_WINDOW = 0x08000000 if WIN else 0  # the build's compilers would each open a console window otherwise
DUMPS = '3DS games (*.3ds *.cci *.cia *.cxi *.app);;All files (*.*)'
DUMP_EXTS = ('.3ds', '.cci', '.cia', '.cxi', '.app')
STAGE_OF = {n: i for i, (n, _) in enumerate(pipeline.STAGES)}
# what the 3DS system files are and where they come from: "What are these?", and when they're missing or a folder has none (a tester asked, Nov 9)
SYSTEM_HELP = ('A few small files every 3DS keeps in its own memory: the system font many games write their text in, and the parts (faces, eyes, '
               'hair) Miis are drawn from. Games load them from the console, so a game dump doesn\'t have them. The builder puts them in each game '
               'it makes and won\'t build without them: Mii games stop without the Mii parts.\n\n'
               'They come from your own 3DS, either way:\n'
               '•  Azahar: File > Set Up System Files, with the Artic Setup Tool running on your 3DS. The builder then finds them by itself.\n'
               '•  GodMode9 on your 3DS: in [1:] SYSNAND CTRNAND > title, highlight the 0004009b folder and press Y, then open [0:] SDCARD, '
               'press Y again and choose Copy (not Move). Back on the PC, press "Folder..." here and choose that 0004009b folder on the SD card.\n\n'
               'The builder copies them once and keeps them for every game after.')


# ---- settings, the PC's drives, and words for what was chosen
def settings_file():
    base = os.environ.get('APPDATA') if WIN else os.path.join(os.path.expanduser('~'), '.config')
    return os.path.join(base or HERE, APP, 'settings.json')


def load_settings():
    try:
        return json.load(open(settings_file(), encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_settings(s):
    os.makedirs(os.path.dirname(settings_file()), exist_ok=True)
    json.dump(s, open(settings_file(), 'w', encoding='utf-8'), indent=1)


def emulator_folders():
    """Folders with a 3DS NAND, where the system archives (shared font, Mii data) can come from: Azahar's, Citra's and Lime3DS's user folders,
    then any folder a few levels down in Downloads, Desktop and Documents, whatever its name: a portable emulator's user folder, or a copy of
    one. A Miitopia built without them stopped on the console, the files being in such a copy in Downloads (Nov 9)."""
    home = os.path.expanduser('~')
    base = os.environ.get('APPDATA', '') if WIN else os.path.join(home, '.local', 'share')
    names = ('Azahar', 'Citra', 'Lime3DS') if WIN else ('azahar-emu', 'citra-emu', 'lime3ds-emu')
    found = [p for p in (os.path.join(base, n) for n in names) if dumps.nand_titles(p)]
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


def sd_cards():
    """removable drives, the ones already set up for a Wii U first"""
    found = []
    if WIN:
        import ctypes
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        for i in range(26):
            root = f'{chr(65 + i)}:\\'
            if mask >> i & 1 and ctypes.windll.kernel32.GetDriveTypeW(root) == 2:  # DRIVE_REMOVABLE
                found.append(root)
    else:
        user = os.environ.get('USER', '')
        for base in (f'/media/{user}', f'/run/media/{user}'):
            if os.path.isdir(base):
                found += [os.path.join(base, d) for d in sorted(os.listdir(base))]
    return sorted(found, key=lambda d: not os.path.isdir(os.path.join(d, 'wiiu')))


def describe(d):
    if d is None:
        return ''
    v = f'v{d.version >> 10}.{d.version >> 4 & 63}.{d.version & 15}'
    return f'{v}, {d.size >> 20} MB' if d.kind == 'update' else f'{len([c for c in d.cia.contents if c[4] is not None])} contents, {d.size >> 20} MB' \
        if d.cia else f'{d.size >> 20} MB'


def otp_ok(path):
    return os.path.isfile(path) and os.path.getsize(path) == 1024


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
    """a game's 48x48 icon (rows of RGB) as a picture the window can show"""
    img = QImage(48, 48, QImage.Format_RGB32)
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
    body = next((f for f in ('Segoe UI Variable Text', 'Segoe UI', 'Cantarell', 'Noto Sans') if f in families), '')
    display = next((f for f in ('Bahnschrift', 'Segoe UI Variable Display', 'Segoe UI') if f in families), body)
    return {'bg': '#0b0e22', 'rail': '#080a1a', 'surface': '#161a3d', 'surface2': '#1c2150', 'edge': '#282f6c', 'text': '#f4f6ff', 'dim': '#a9b1d8',
            'faint': '#6b73a6', 'red': '#ff5a73', 'violet': '#a07bff', 'blue': '#3fd0ff', 'green': '#45e3a8', 'amber': '#ffc95e', 'ink': '#14183a',
            'paper': '#f6f7ff', 'font': body, 'display': display, 'mono': 'Cascadia Mono' if 'Cascadia Mono' in families else 'Consolas'}


# ---- what the window shows, and what its buttons do
class App(QObject):
    changed = Signal()
    logChanged = Signal()

    def _p(kind, name, notify=changed):
        return Property(kind, lambda self: self._s[name], notify=notify)

    hasGame, gameName, maker, badge, known = _p(bool, 'hasGame'), _p(str, 'gameName'), _p(str, 'maker'), _p(str, 'badge'), _p(bool, 'known')
    note, meta, icon = _p(str, 'note'), _p(str, 'meta'), _p(str, 'icon')
    updateText, hasUpdate, dlcText, hasDlc = _p(str, 'updateText'), _p(bool, 'hasUpdate'), _p(str, 'dlcText'), _p(bool, 'hasDlc')
    modText, hasMod = _p(str, 'modText'), _p(bool, 'hasMod')
    keysOk, keysNeeded, keysText, otpOk, otpText = _p(bool, 'keysOk'), _p(bool, 'keysNeeded'), _p(str, 'keysText'), _p(bool, 'otpOk'), _p(str, 'otpText')
    sysOk, sysText = _p(bool, 'sysOk'), _p(str, 'sysText')
    target, destText, cards = _p(str, 'target'), _p(str, 'destText'), _p('QVariantList', 'cards')
    canBuild, building, view = _p(bool, 'canBuild'), _p(bool, 'building'), _p(str, 'view')
    stages, stage, failed, progress, detail = _p('QVariantList', 'stages'), _p(int, 'stage'), _p(bool, 'failed'), _p(float, 'progress'), _p(str, 'detail')
    log = _p(str, 'log', logChanged)
    resultText, where, dialog = _p(str, 'resultText'), _p(str, 'where'), _p('QVariant', 'dialog')
    games, others, version, rootOk = _p('QVariantList', 'games'), _p('QVariantList', 'others'), _p(str, 'version'), _p(bool, 'rootOk')
    del _p

    def __init__(self):
        super().__init__()
        self.st = load_settings()
        if not self.st.get('keys'):
            self.st['keys'] = decrypt.KEYS_DEFAULT
        decrypt.use_keys(self.st['keys'])
        self.game = self.update = self.dlc = self.mod = None  # the mod is a path
        self.proc, self.result, self.cancelled = None, None, False
        self.lines, self.log_lines, self.dialogs, self.flow = queue.Queue(), [], [], None
        compat = json.load(open(os.path.join(HERE, 'compat.json'), encoding='utf-8'))
        self._s = dict(hasGame=False, gameName='', maker='', badge='', known=False, icon='', meta='',
                       note='Pick one of your 3DS games to start. The builder reads its name and icon, and asks about its update and DLC.',
                       updateText='None', hasUpdate=False, dlcText='None', hasDlc=False, modText='None', hasMod=False, keysOk=False, keysNeeded=False, keysText='', otpOk=False,
                       otpText='', sysOk=False, sysText='', target=self.st.get('target', 'card'), destText='', cards=[], canBuild=False, building=False, view='form', stages=[],
                       stage=-1, failed=False, progress=0.0, detail='', log='', resultText='', where='', dialog=None, version=VERSION,
                       rootOk=bool(pipeline.ROOT) and os.path.isfile(os.path.join(pipeline.ROOT, 'tools', 'win', 'build3ds.py')),
                       games=[dict(name=g['name'], status=g['status'], note=g['note']) for k, g in compat.items() if not k.startswith('_')],
                       others=compat.get('_cemu', []))  # shown only: they build as untested games, by their own code
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.refresh_cards()
        self.refresh_files()

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

    def open_game(self, path):
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
        info = pipeline.profile_for(d) or pipeline.derived(d)
        known = info['status'] == 'Plays'
        self.set(hasGame=True, gameName=info['name'], maker=d.publisher or '', known=known, badge='Plays on Wii U' if known else 'Untested',
                 icon=icon_url(d.icon) if d.icon else '', meta=f'{d.product}  ·  {d.title_id:016X}  ·  {describe(d)}', view='form')
        self.set_extra('update', None)
        self.set_extra('dlc', None)
        self.set_extra('mod', None)
        self.refresh_files()
        self.run_flow(self.extras())

    def refresh_note(self):
        info = pipeline.profile_for(self.game)
        if info:
            note = f'Tested on a real Wii U: {info["note"][0].lower() + info["note"][1:]}'
            if self.update:
                note += ' It was tested without its update: with it, it should play the same, but nobody has checked yet.'
        else:
            note = 'Nobody has played this one on a Wii U yet: it may not start, or it may stop partway.'
        if self.mod:
            note += ' With a mod it\'s untested: the mod is built in before the game is recompiled, so its code runs like the game\'s.'
        self.set(note=note)

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
            path = QFileDialog.getExistingDirectory(None, f'The mod\'s folder for {self._s["gameName"]}', start)
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
        p = QFileDialog.getExistingDirectory(None, 'The folder with your 3DS keys (boot9.bin, seeddb.bin)', self.st['keys'])
        if p:
            if not decrypt.have_keys(p):
                self.tell('No keys there', 'There\'s no boot9.bin in that folder.', 'warn')
            self.st['keys'] = p
            decrypt.use_keys(p)
            self.refresh_files()

    @Slot()
    def chooseSystem(self):
        p = QFileDialog.getExistingDirectory(None, 'Your 3DS system files: Azahar\'s user folder, or the 0004009b folder GodMode9 copied off your 3DS',
                                             self.st.get('system', ''))
        if p:
            if not self.import_system(p):
                self.tell('No 3DS system files there', SYSTEM_HELP, 'warn')
            self.refresh_files()

    @Slot()
    def explainSystem(self):
        self.tell('3DS system files', SYSTEM_HELP)

    def import_system(self, folder):
        """the system archives from a folder into the ones every build takes (pipeline.ARCHIVES), once: they stay for the next games"""
        tmp = tempfile.mkdtemp(prefix='3diiu_sys_')  # for contents that need decrypting first
        try:
            got = dumps.import_system_archives(folder, pipeline.ARCHIVES, tmp)
        except (OSError, dumps.DumpError, decrypt.DecryptError):
            got = []
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        if got:
            self.st['system'] = folder
            save_settings(self.st)
        return got

    @Slot()
    def chooseOtp(self):
        p = QFileDialog.getOpenFileName(None, 'Your Wii U\'s otp.bin', os.path.dirname(self.st.get('otp', '')), 'otp.bin (otp.bin);;All files (*.*)')[0]
        if p:
            self.st['otp'] = p
            self.refresh_files()

    @Slot(str)
    def setTarget(self, t):
        self.st['target'] = t
        self.set(target=t)
        self.refresh_files()

    @Slot()
    def chooseFolder(self):
        p = QFileDialog.getExistingDirectory(None, 'Where the WUP goes', self.st.get('folder', ''))
        if p:
            self.st['folder'] = p
            self.setTarget('folder')

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
        if self.st.get('card') not in cards:
            self.st['card'] = cards[0] if cards else ''
        self.set(cards=[dict(path=c, label=f'{c}    {"set up for a Wii U" if os.path.isdir(os.path.join(c, "wiiu")) else "removable drive"}') for c in cards])

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
        source = f', from {short(self.st["system"], 36)}' if self.st.get('system') else ''
        self.set(sysOk=system, sysText=f'Ready: the shared font and Mii data{source}' if system else
                 'Needed: the font and Mii parts games load from the 3DS itself')  # short: the window's first width cuts the line
        self.set(keysOk=have, keysNeeded=needed and not have,
                 keysText=f'Found in {short(keys, 44)}' if have else 'Needed for this encrypted game' if needed else 'Only needed for encrypted dumps',
                 otpOk=otp_ok(otp), otpText=short(otp, 52) if otp_ok(otp) else 'Needed: it comes from your Wii U\'s NAND backup', destText=dest,
                 canBuild=self.game is not None)

    # ---- building
    @Slot()
    def build(self):
        if not self.game or self._s['building']:
            return
        if not self._s['rootOk']:
            self.tell('The build engine is missing', 'The engine folder next to 3Dii U Builder.exe holds the recompiler and its toolchain. Copy the '
                                                     'whole 3Dii U Builder folder again, engine folder included.', 'error')
            return
        if not otp_ok(self.st.get('otp', '')):
            self.tell('otp.bin is missing', 'Choose your Wii U\'s otp.bin: it should be exactly 1024 bytes.', 'warn')
            return
        dest = self.st.get('card', '') if self._s['target'] == 'card' else self.st.get('folder', '')
        if not dest or not os.path.isdir(dest):
            self.tell('Where should it go?', 'Choose an SD card or a folder for the game.', 'warn')
            return
        if not dumps.have_system_archives(pipeline.ARCHIVES):
            for folder in emulator_folders():
                if self.import_system(folder) and dumps.have_system_archives(pipeline.ARCHIVES):
                    break
            self.refresh_files()
            if not dumps.have_system_archives(pipeline.ARCHIVES):
                self.tell('The 3DS system files are missing', SYSTEM_HELP, 'warn')
                return
        save_settings(self.st)
        argv = [sys.executable, '--pipeline'] if FROZEN else [sys.executable.replace('pythonw.exe', 'python.exe'), '-u', os.path.join(HERE, 'pipeline.py')]
        argv += ['--game', self.game.path, '--otp', self.st['otp'], '--card' if self._s['target'] == 'card' else '--out', dest, '--keys', self.st['keys']]
        for kind in ('update', 'dlc'):
            if getattr(self, kind):
                argv += [f'--{kind}', getattr(self, kind).path]
        if self.mod:
            argv += ['--mod', self.mod]
        self.log_lines, self.result, self.cancelled = [], None, False
        stages = [n for n, _ in pipeline.STAGES if n != 'Copying' or self._s['target'] == 'card']
        self.set(building=True, view='building', stages=stages, stage=0, failed=False, progress=0.0, detail='Starting...', log='')
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace',
                                     creationflags=NO_WINDOW, start_new_session=not WIN, env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()
        self.timer.start(100)

    def read(self, proc):
        for line in proc.stdout:
            self.lines.put(line.rstrip('\n'))
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
                    self.set(progress=float(line.split()[1]))
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
        if self.result and self.result.startswith('@done '):
            where = self.result[6:]
            msg = (f'{self._s["gameName"]} is on your SD card, in install\\{os.path.basename(where)}. Put the card in your Wii U and install it with WUP '
                   f'Installer GX2.' if self._s['target'] == 'card' else
                   f'The WUP is in {where}. Copy it to your SD card\'s install folder and install it with WUP Installer GX2.')
            self.set(building=False, view='done', stage=len(self._s['stages']), where=where, resultText=msg + ' If an older build of it is installed, '
                                                                                                            'delete that first.')
        elif self.cancelled:
            self.set(building=False, view='form')
        else:
            self.set(building=False, view='failed', failed=True,
                     resultText=self.result[7:] if self.result else 'The build stopped. Show details says where.')

    @Slot()
    def cancel(self):
        if self.proc and self.proc.poll() is None:
            self.cancelled = True
            self.set(detail='Stopping...')
            if WIN:  # the build's own compilers go too
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.pid)], creationflags=NO_WINDOW, capture_output=True)
            else:
                import signal
                os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)

    @Slot()
    def back(self):
        self.set(view='form')

    @Slot()
    def openFolder(self):
        where = self._s['where']
        if os.path.isdir(where):
            os.startfile(where) if WIN else subprocess.Popen(['xdg-open', where])

    @Slot(result=bool)
    def busy(self):
        return bool(self.proc and self.proc.poll() is None)

    def stop_for_close(self):
        self.cancel()


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
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty('app', state)
    engine.rootContext().setContextProperty('T', theme())
    engine.rootContext().setContextProperty('appIcon', png_url(draw_icon(132)))
    engine.load(QUrl.fromLocalFile(os.path.join(HERE, 'ui', 'Main.qml')))
    return qt, engine, state


def run():
    qt, engine, state = create()
    if not engine.rootObjects():
        return 1
    return qt.exec()
