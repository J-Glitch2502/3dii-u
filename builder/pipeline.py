#!/usr/bin/env python3
"""Turns a 3DS game dump into a WUP install or an Aroma Homebrew bundle, with its update and DLC if given. The app (builder/app.py) runs this; it works on its own too:

    pipeline.py --game <dump> [--update <dump>] [--dlc <dump>] [--mod <folder or .zip> ...] [--keys <folder>] [--emulator <folder>] --otp <otp.bin> (--card D: | --out <folder>) [--keep] [--jobs N]

--keys: the folder with boot9.bin and seeddb.bin from your own 3DS, ~/.3ds by default; only encrypted dumps need it, and they're decrypted the way
cia_to_decrypted_3ds.py does it (decrypt.py). --emulator: a 3DS emulator's user folder (Azahar, Citra) or the 0004009b folder GodMode9 copies off
your 3DS, for the system archives (shared font, Mii data) if this PC has none yet. --otp: your Wii U's otp.bin, for the common key the WUP is
packed with; Homebrew output does not require otp.bin. Choose --output-format homebrew for a full WUHB, or add
--homebrew-parts binary, shaders or both to export a small update for an existing matching Homebrew game.
Lines starting with @ are for the app: @stage <name>, @progress <0-100>, @done <where it went>, @error <why>.

It builds with tools/win/build3ds.py, in folders of its own (builder_work/, build_win_builder/), so it never touches the nine ports' builds."""
import argparse
from contextlib import ExitStack, contextmanager
import errno
import filecmp
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('THREEDII_ROOT') or os.path.dirname(HERE)  # the standalone app runs from elsewhere: app.py finds the repository
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'win'))
sys.path.append(os.path.join(ROOT, 'tools'))
import wup_safety  # noqa: E402
import credits  # noqa: E402
import decrypt  # noqa: E402
import dumps  # noqa: E402
import mods  # noqa: E402
import shaderlogs  # noqa: E402
import homebrew  # noqa: E402
import wup_update  # noqa: E402
import user_files  # noqa: E402
from objcache import _checked, workspace_lock  # noqa: E402

WORK = os.environ.get('THREEDII_WORK') or ROOT  # the standalone app's engine may be read-only: then it builds in a folder of the user's
# Where games are built: each game's work while it builds and the compiled code kept between builds, gigabytes of files the build makes
# again as it needs them. A folder the player chose (a drive with room: a tester's C: had none), or the work folder.
BUILD = os.environ.get('THREEDII_BUILD') or WORK
BUILD_FILES = ('builder_work', 'build_win_builder', 'objcache')  # what it holds, all of it: build3ds puts objcache beside build_win_builder
WORK_GAMES = os.path.join(BUILD, 'builder_work')
ARCHIVES = os.path.join(WORK, 'system_archives') if WORK != ROOT else os.path.join(ROOT, 'games', 'system_archives')
STAGES = [('Reading your game', 2), ('Decrypting', 8), ('Unpacking', 7), ('Finding the game\'s code', 2), ('Recompiling', 48), ('Shaders', 14),
          ('Making the title', 2), ('Packing the WUP', 10), ('Copying', 7)]


def stages_for(output_format='wup', parts='full'):
    names = list(STAGES[:3])
    if parts != 'shaders': names += STAGES[3:5]
    if parts != 'binary': names += [STAGES[5]]
    if output_format in ('wup','wup-update'): names += STAGES[6:]
    elif parts == 'full': names += [('Making the title', 2), ('Packing Homebrew', 10), ('Copying', 7)]
    else: names += [('Exporting Homebrew update', 10), ('Copying', 7)]
    return names


class Progress:
    """@stage and @progress lines over the whole run, from each stage's share"""
    def __init__(self):
        self.done, self.share = 0, 0

    def stage(self, name):
        stages = getattr(self, 'stages', STAGES)
        for n, (s, w) in enumerate(stages):
            if s == name:
                self.done = sum(x for _, x in stages[:n])
                self.share = w
        print(f'@stage {name}', flush=True)
        self.at(0)

    def at(self, fraction):
        print(f'@progress {min(100, self.done + self.share * max(0.0, min(1.0, fraction))):.1f}', flush=True)


class Watch:
    """build3ds prints 'game code: 120 of 1300, 50 s': that's the recompile stage's progress"""
    def __init__(self, out, progress):
        self.out, self.progress, self.parts = out, progress, ''

    def write(self, s):
        self.out.write(s)
        self.parts += s
        while '\n' in self.parts:
            line, self.parts = self.parts.split('\n', 1)
            m = re.search(r'(runtime|game code): (\d+) of (\d+)', line)
            if m:  # the runtime is the first third, and only compiles once
                f = int(m.group(2)) / int(m.group(3))
                self.progress.at(f * 0.3 if m.group(1) == 'runtime' else 0.3 + f * 0.7)
        return len(s)

    def flush(self):
        self.out.flush()


class Held:
    """stdout while the shaders are made alongside the code: what that work prints (build3ds.context.tag 'shaders', handed down to its workers)
    waits here until the Shaders stage, as the app shows one stage at a time"""
    def __init__(self, out, context):
        self.out, self.context, self.held = out, context, io.StringIO()

    def write(self, s):
        return (self.held if getattr(self.context, 'tag', None) == 'shaders' else self.out).write(s)

    def flush(self):
        self.out.flush()


def fail(why):
    print(f'@error {why}', flush=True)
    sys.exit(1)


def profile_for(dump):
    compat = json.load(open(os.path.join(HERE, 'compat.json'), encoding='utf-8'))
    return compat.get('%016X' % dump.title_id)


def derived(dump):
    """title id, product code, slug and folder for a game the list doesn't have: kept clear of real Wii U titles (high byte 0x3D), and the 3DS id stays readable"""
    product = (dump.product or '').split('-')[-1]
    if re.fullmatch(r'[A-Za-z0-9]{1,4}', product):
        code = product.upper().ljust(4, 'X')
        slug = 'g_' + code.lower()
        fallback_folder = code
    else:
        code = f'{(dump.unique >> 8) & 0xffff:04X}'
        slug = f'g_{dump.title_id:016x}'
        fallback_folder = f'Game_{dump.title_id:016X}'
    folder = re.sub(r'[^A-Za-z0-9]+', '_', dump.name or '').strip('_') or fallback_folder
    if not safe_component(folder):
        folder = f'Game_{dump.title_id:016X}'
    return {'slug': slug, 'title_id': '00050000%08X' % (0x3D000000 | (dump.unique >> 8)), 'name': dump.name or code, 'product': 'WUP-N-' + code,
            'folder': folder, 'status': 'Untested', 'note': 'Not tested on a console yet: it may not start, or stop partway.'}


def safe_component(name):
    return (isinstance(name, str) and re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_-]*', name) is not None
            and re.fullmatch(r'CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]', name, re.IGNORECASE) is None)


def workspace_child(parent, name):
    if not safe_component(name):
        fail(f'Invalid Builder folder name: {name!r}')
    try:
        root, _ = _checked(parent, directory=True)
        path, _ = _checked(root / name, directory=True)
        if path.resolve().parent != root.resolve():
            fail(f'The build folder must be directly inside {root}: {path}')
    except (OSError, ValueError) as e:
        fail(str(e))
    return str(path)


def console_learned(slug):
    """What consoles learned playing this game, as shader logs for its build: what earlier builds read off SD cards, where games left the
    list of what a console had to compile until 2026-11-15 (they use no card now). Kept with the game's other logs in the repository, in the
    work folder for the standalone app."""
    kept = os.path.join(ROOT, 'games', slug, 'shader_logs') if WORK == ROOT else os.path.join(WORK, 'learned', slug)
    return [os.path.join(kept, f) for f in sorted(os.listdir(kept)) if f.startswith('console_card_')] if os.path.isdir(kept) else []


LANGUAGES = ('Japanese', 'English', 'French', 'German', 'Italian', 'Spanish',
             'Simplified Chinese', 'Korean', 'Dutch', 'Portuguese', 'Russian', 'Traditional Chinese')

def language_for_locale(locale):
    language, *qualifiers = locale.lower().replace('_', '-').split('-')
    if language == 'zh':
        traditional = 'hant' in qualifiers or ('hans' not in qualifiers and any(
            region in qualifiers for region in ('tw', 'hk', 'mo')))
        return 'Traditional Chinese' if traditional else 'Simplified Chinese'
    codes = dict(zip(('ja', 'en', 'fr', 'de', 'it', 'es', 'zh', 'ko', 'nl', 'pt', 'ru'), LANGUAGES))
    return codes.get(language, 'English')


def tool(name):
    """one of the repository's tool scripts, loaded by its path: the app carries them as files, not as modules"""
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, 'tools', name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


LANGUAGES = ('Japanese', 'English', 'French', 'German', 'Italian', 'Spanish',
             'Simplified Chinese', 'Korean', 'Dutch', 'Portuguese', 'Russian', 'Traditional Chinese')
MENU_BUTTONS = ('L', 'R', 'ZL', 'ZR', 'L Stick', 'R Stick', 'A', 'B', 'X', 'Y', 'Plus', 'Minus',
                'D-Pad Up', 'D-Pad Down', 'D-Pad Left', 'D-Pad Right')


def valid_menu_shortcut(value):
    if not isinstance(value, str):
        return False
    buttons = value.split('+')
    return len(buttons) == 2 and buttons[0] != buttons[1] and all(button in MENU_BUTTONS for button in buttons)


def make_title(game_dir, out, info, art=None, icon_art=None, style='picture', released='', case_scan=False, pad_bezel=None, pad_art=None,
               boot_sound=None, screen_art=None, *, language=None, include_shaders=True):
    """the title every game gets, then its update, DLC and optional Download Play archive"""
    tool('make_game_title').make_title(os.path.join(out, 'title'), game_dir, os.path.join(out, info['slug'] + '.rpx'), os.path.join(out, 'shaders'),
                                       info['title_id'], info['name'], info['product'], ARCHIVES, art, icon_art, style, released, case_scan, pad_bezel,
                                       pad_art, boot_sound, screen_art, include_shaders=include_shaders)
    content = os.path.join(out, 'title', 'content')
    if language is not None:
        with open(os.path.join(content, 'language.txt'), 'w', encoding='ascii') as f:
            f.write(f'{LANGUAGES.index(language)}\n')
    for f in os.listdir(game_dir):
        if f.startswith(('update_', 'dlc_')) or f == 'dlp_child.romfs':
            os.link(os.path.join(game_dir, f), os.path.join(content, f))
    if language:
        with open(os.path.join(content, 'language.txt'), 'w', encoding='ascii') as f:
            f.write(f'{LANGUAGES.index(language)}\n')


@contextmanager
def staged_output(target):
    with wup_safety.staged_outputs(target) as (new,):
        yield new


def copy_to(source, target_dir):
    """the WUP folder into target_dir, compared file by file; what was there is replaced only once the new copy is right"""
    name = os.path.basename(source)
    target = os.path.join(target_dir, name)
    try:
        wup_safety.check_output(target, source, replace=True)
        wup_safety.check_output(source, replace=True)
        with staged_output(target) as new:
            wup_safety.check_title_replacement(target, wup_safety.wup_title_id(source))
            os.makedirs(new)
            names = sorted(os.listdir(source))
            total, done = sum(os.path.getsize(os.path.join(source, n)) for n in names), 0
            for n in names:
                with open(os.path.join(source, n), 'rb') as i, open(os.path.join(new, n), 'wb') as o:
                    while chunk := i.read(8 << 20):
                        o.write(chunk)
                        done += len(chunk)
                        PROGRESS.at(done / total * 0.8)
            if any(filecmp.cmpfiles(source, new, names, shallow=False)[1:]):
                fail('The copy came out different from the original: the card may be failing. Nothing was replaced.')
    except ValueError as e:
        fail(str(e))
    return target


def pack_wup(otp, title, target):
    try:
        wup_safety.check_output(target, title, replace=True)
        with staged_output(target) as new:
            r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'pack_game_wup.py'), otp, title, new], cwd=ROOT,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            print(r.stdout.strip()[-400:], flush=True)
            if r.returncode:  # a failed pack can leave a title.tmd behind: only the exit code counts
                fail('Packing the WUP failed.')
    except (OSError, ValueError) as e:
        fail(f'The WUP could not be replaced: {e}')


def card_error(e):
    """what to tell the player when the SD card refuses a write (a tester's card was locked: Windows says WinError 19)"""
    if getattr(e, 'winerror', None) == 19 or e.errno == errno.EROFS:
        return 'The SD card is locked. Slide the little switch on its side up, away from "Lock", then put the card back in.'
    return f'The SD card couldn\'t be written to: {e.strerror or e}'


def check_card(card, relative="install"):
    """a build that ends on the SD card finds out it can't write there first, not after minutes of building"""
    install = os.path.join(card.rstrip('\\/') + os.sep, relative)
    try:
        wup_safety.check_output(install)
        os.makedirs(install, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=install, prefix='.3diiu-check-') as f:
            f.write(b'3Dii U')
    except ValueError as e:
        fail(str(e))
    except OSError as e:
        fail(card_error(e))


PROGRESS = Progress()


class ShowCredits(argparse.Action):
    """--credits: who the builder is built on, then exit, before the required options are asked for (as --version does)"""

    def __call__(self, parser, namespace, values, option_string=None):
        print(credits.text())
        parser.exit()


def main():
    if os.name == 'nt':  # a tool that can't load something ends with an error code, not a Windows box that holds the build until clicked
        import ctypes  # (issue 1); the build tools inherit it
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)  # SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX, SEM_NOOPENFILEERRORBOX
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', action='version', version=f'{credits.APP} {credits.VERSION} ({credits.LICENCE}, {credits.SOURCE})')
    ap.add_argument('--credits', nargs=0, action=ShowCredits, help='the projects the builder is built on, and their licences')
    ap.add_argument('--game', required=True)
    ap.add_argument('--update')
    ap.add_argument('--dlc')
    ap.add_argument('--download-play-child', type=int, metavar='INDEX',
                    help='build the selected embedded child in the Builder work folder for Cemu (THREEDII_MEASURE=1)')
    ap.add_argument('--mod', action='append', default=[], help="a mod in Luma's layout (luma/titles/<title id>/: romfs, code.ips/.bps/.bin); several go on in order")
    ap.add_argument('--keys', help='boot9.bin and seeddb.bin from your own 3DS (default ~/.3ds)')
    ap.add_argument('--emulator', help='a 3DS emulator\'s user folder (Azahar, Citra) or the 0004009b folder GodMode9 copies off your 3DS '
                                         '(title, in CTRNAND), for the system archives')
    ap.add_argument('--otp')
    ap.add_argument('--output-format', choices=('wup','wup-update','homebrew'), default='wup')
    ap.add_argument('--wup-update-parts', choices=wup_update.PARTS, default='both')
    ap.add_argument('--wup-base-manifest', help='matching .base.json saved beside the original full WUP build')
    ap.add_argument('--homebrew-parts', choices=homebrew.PARTS, default='full')
    ap.add_argument('--card', help='an SD card: WUPs go in install, full Homebrew in wiiu/apps, update exports in wiiu/3dii-u/updates')
    ap.add_argument('--out', help='a folder to put the selected output in instead')
    ap.add_argument('--keep', action='store_true', help='keep the work files, for a quicker rebuild')
    ap.add_argument('--art', help="a picture (the cover art, say) for the start-up screens; the game's own icon otherwise")
    ap.add_argument('--icon-art', help="a complete custom Wii U Menu icon; otherwise the game's own icon with a Virtual Console frame")
    ap.add_argument('--pad-bezel', help="a picture (854x480, say) the GamePad shows where it shows no screen, a bezel or overlay; black otherwise")
    ap.add_argument('--pad-art', help="a picture of its own for the GamePad's start-up screen, shown as it is; the TV's otherwise")
    ap.add_argument('--boot-sound', help="the game's own start-up sound, a .btsnd (tools/make_boot_sound.py); the one for every game otherwise")
    ap.add_argument('--save', help="a 3DS save to pack in, which the game takes on the console (saves.py): Checkpoint's, JKSM's or an emulator's folder")
    menu = ap.add_mutually_exclusive_group()
    ap.add_argument('--menu-style', choices=('new', 'classic'), default='new', help='choose the in-game options layout when building code')
    menu.add_argument('--no-menu', action='store_true', help='disable the in-game options menu')
    menu.add_argument('--menu-shortcut', metavar='BUTTON+BUTTON', help='two different buttons to open the in-game menu, for example L+R')
    ap.add_argument('--language', choices=LANGUAGES, help='default game language, when supported (otherwise follows the console)')
    ap.add_argument('--art-style', choices=('picture', 'console'), default='console',
                    help='the start-up screens: laid out as the Virtual Console shows a game, or --art as it is')
    ap.add_argument('--tdb', help="a folder for GameTDB's list of 3DS games and libretro's of title screens, where the Virtual Console's screens "
                                  "find the year a game came out and its title screen")
    ap.add_argument('--mixed', action='store_true', help='fast code with only the video decoder in safe mode; selected automatically for tested base editions')
    ap.add_argument('--cold', action='store_true', help="for the checks: build it as a game nobody has built before, with none of this PC's lists of "
                                                        'its code and shaders (tools/check_ports.py --cold)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    if a.menu_shortcut is not None and not valid_menu_shortcut(a.menu_shortcut):
        ap.error('--menu-shortcut requires two different buttons from ' + ', '.join(MENU_BUTTONS))
    for s in (sys.stdout, sys.stderr):  # run from a console, whose code page may lack a character of a game's name: shown escaped, not fatal
        if s:
            s.reconfigure(errors='backslashreplace')
    if sys.platform != 'win32':
        fail('Building on Linux isn\'t ready yet: this version builds on Windows.')
    if not (a.card or a.out) and a.download_play_child is None:
        fail('Choose an SD card or a folder for the WUP.')
    with ExitStack() as stack:
        try:
            stack.enter_context(workspace_lock(BUILD, WORK))
            from app import fresh_cache
            fresh_cache(ROOT, BUILD)
        except (OSError, ValueError) as e:
            fail(str(e))
        build(a)


def required_disk(game, extras, encrypted, cemu_only):
    reserve = 3 << 30
    if cemu_only:
        # The unpacked title hardlinks its assets and the Cemu build skips WUP packing.
        dumps = [game, *extras]
        temporary = sum(d.size for d in dumps if d.kind == 'dlc' or d in encrypted)
        return reserve + sum(d.size for d in dumps) + temporary
    return reserve + game.size * 4 + sum(d.size * 3 for d in extras)


def extract_dlp_child(parent, title_id, game_dir, tmp, keys):
    if not dumps.copy_dlp(parent, tmp):
        raise dumps.DumpError('This game dump has no Download Play archive.')
    path = os.path.join(tmp, 'child.cia')
    name = f'{title_id:016x}.cia'
    with open(os.path.join(tmp, 'dlp_child.romfs'), 'rb') as archive:
        matches = [source for filename, source in mods.read_romfs(archive).files if filename.lower() == name]
        if len(matches) != 1:
            raise dumps.DumpError(f'Expected one Download Play child named {name}.')
        offset, size = matches[0]
        archive.seek(offset)
        with open(path, 'wb') as output:
            for done in range(0, size, 4 << 20):
                length = min(size - done, 4 << 20)
                data = archive.read(length)
                if len(data) != length:
                    raise dumps.DumpError('The Download Play child was cut short.')
                output.write(data)

    with ExitStack() as files:
        child = dumps.Dump(path)
        files.enter_context(child.f)
        if not child.cia or child.title_id != title_id:
            raise dumps.DumpError('The Download Play CIA has the wrong title ID.')
        if child.encrypted():
            problem = user_files.check_key_folder(keys or decrypt.KEYS_DEFAULT)
            if problem:
                raise decrypt.DecryptError(problem)
            plain = decrypt.to_plain(path, os.path.join(tmp, 'plain_child'), 'child')
            child = dumps.Dump(plain)
            files.enter_context(child.f)
        if int.from_bytes(child.program().pread(0x118, 8), 'little') != title_id:
            raise dumps.DumpError('The Download Play program has the wrong title ID.')
        info = dumps.extract_program(child, game_dir)
        if int(info['program_id'], 16) != title_id:
            raise dumps.DumpError('The Download Play extended header has the wrong program ID.')


def build(a):
    if a.output_format == 'wup' and a.homebrew_parts != 'full':
        fail('Small update exports require Output: Homebrew.')
    partial = a.output_format == 'homebrew' and a.homebrew_parts != 'full'
    need_code = not partial or a.homebrew_parts != 'shaders'
    is_update = a.output_format == 'wup-update'
    if is_update and a.save:
        fail('WUP updates contain executable/shader changes only. Import saves with a full base build or a save manager.')
    need_shaders = (not partial or a.homebrew_parts != 'binary') and not (is_update and a.wup_update_parts == 'binary')
    PROGRESS.stages = stages_for(a.output_format, a.wup_update_parts if is_update else a.homebrew_parts)
    try:
        for path in (WORK_GAMES, ARCHIVES, a.out):
            if path:
                wup_safety.check_output(path)
    except ValueError as e:
        fail(str(e))
    # a measuring build for Cemu is never packed (THREEDII_MEASURE, build3ds.py), so it needs no common key
    cemu_only = os.environ.get('THREEDII_MEASURE', '') not in ('', 'console')
    child_index = a.download_play_child
    if child_index is not None:
        if not 0 <= child_index <= 255:
            fail('A Download Play child index must be between 0 and 255.')
        if os.environ.get('THREEDII_MEASURE') != '1' or a.card or a.update or a.dlc or a.mod or a.save:
            fail('Download Play child checks require a base game and THREEDII_MEASURE=1.')
    problem = None if cemu_only or a.output_format == 'homebrew' else user_files.check_otp(a.otp or '')
    if problem:
        fail(problem)
    if a.card:
        check_card(a.card, 'wiiu/apps' if a.output_format == 'homebrew' else 'install')

    PROGRESS.stage('Reading your game')
    decrypt.use_keys(a.keys)
    try:
        game = dumps.Dump(a.game)
        if game.kind not in ('game', 'demo'):
            fail(f'That file is {"an update" if game.kind == "update" else "DLC" if game.kind == "dlc" else "not a game"}. Choose the game itself first.')
        extras = []
        for path, kind in ((a.update, 'update'), (a.dlc, 'dlc')):
            if path:
                d = dumps.Dump(path)
                if d.kind != kind or d.unique != game.unique:
                    fail(f'{os.path.basename(path)} isn\'t the {"update" if kind == "update" else "DLC"} for this game.')
                extras.append(d)
        encrypted = [d for d in [game] + extras if d.encrypted()]
        if encrypted and not decrypt.have_keys(a.keys):
            fail(f'{os.path.basename(encrypted[0].path)} is encrypted. Put boot9.bin and seeddb.bin from your own 3DS in {a.keys or decrypt.KEYS_DEFAULT}.')
        problem = user_files.check_key_folder(a.keys or decrypt.KEYS_DEFAULT) if encrypted else None
        if problem:
            fail(problem)
        game.read_names()
        mod_list = [mods.Mod(p, game.title_id) for p in a.mod]
    except (dumps.DumpError, decrypt.DecryptError, mods.ModError) as e:
        fail(str(e))
    update = next((d for d in extras if d.kind == 'update'), None)
    dlc = next((d for d in extras if d.kind == 'dlc'), None)
    profile = profile_for(game) if child_index is None else None
    known = None if a.cold else profile
    code_mods = any(m.changes_code() for m in mod_list)
    info = dict(known or derived(game))
    if a.output_format in ('wup','wup-update'):
        original_id=int(info['title_id'],16)
        native_id=wup_update.native_base_id(original_id, game.title_id)
        info['title_id']=f'{native_id:016X}'
        if native_id!=original_id:
            info['folder'] += '_NativeWUP'
            print(f'Native WUP title ID: {original_id:016X} -> {native_id:016X}. Existing legacy installs remain separate.',flush=True)

    input_summary = dict(program_id=f'{game.title_id:016X}', game_version=getattr(game,'version',0),
                         update=dict(title_id=f'{update.title_id:016X}',version=update.version) if update else None,
                         dlc=dict(title_id=f'{dlc.title_id:016X}',version=dlc.version) if dlc else None)
    inventory_path = os.path.join(WORK_GAMES, '_wup', 'base-inventories', info['title_id'] + '.json')
    selected_manifest = getattr(a, 'wup_base_manifest', None) or inventory_path
    if is_update:
        try:
            base_record = wup_update.read_manifest(selected_manifest, int(info['title_id'],16))
            if input_summary != base_record['inputs']:
                fail('The game/update/DLC selection differs from the selected base build. Choose its matching inputs or build a new full WUP base.')
            if a.wup_update_parts == 'binary' and wup_update.shader_contract(ROOT) != base_record['shader_contract']:
                fail('Shader code changed since the selected base. Choose Binary + shaders.')
        except ValueError as error: fail(str(error))

    child_id = None
    if child_index is not None:
        child_id = 0x0004000100000000 | (game.unique & ~0xFF) | child_index
        info.update(slug=f'dlp_{child_id:016x}', title_id=f'000500003E{child_id & 0xFFFFFF:06X}',
                    name=info['name'] + ' (Download Play)')
    if update:
        info['slug'] += f'_v{update.version}'
    if a.cold:
        info['slug'] += '_cold'  # work folders of its own, and no list of entries under that name
    if not safe_component(info['folder']):
        fail(f'Invalid WUP folder name: {info["folder"]!r}')
    print(f'{info["name"]} ({game.title_id:016X}): {info["status"]}' + (f', update v{update.version}' if update else '') + (', with DLC' if dlc else '')
          + (f', {len(mod_list)} mod{"s" * (len(mod_list) > 1)}' if mod_list else ''), flush=True)

    game_dir = workspace_child(WORK_GAMES, info['slug'])
    tmp = workspace_child(WORK_GAMES, info['slug'] + '_decrypting')  # plain copies of what came encrypted
    need = required_disk(game, extras, encrypted, cemu_only)
    os.makedirs(WORK_GAMES, exist_ok=True)
    if shutil.disk_usage(WORK_GAMES).free < need:
        fail(f'Not enough free space: building this takes about {need >> 30} GB on the drive with {WORK}.')

    PROGRESS.stage('Decrypting')
    for d in (game_dir, tmp):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    try:
        plain = {d: dumps.Dump(decrypt.to_plain(d.path, os.path.join(tmp, d.kind), d.kind)) if d.encrypted() and d.kind != 'dlc' else d for d in [game] + extras}
        PROGRESS.at(0.8)
        sources = {}
        if dlc:
            sources = decrypt.dlc_contents(dlc.path, tmp) if dlc.encrypted() else dumps.plain_dlc_sources(dlc, tmp)

        PROGRESS.stage('Unpacking')
        if child_id is not None:
            extract_dlp_child(plain[game], child_id, game_dir, tmp, a.keys)
        else:
            dumps.extract_program(plain[game], game_dir, lambda d, t: PROGRESS.at(d / t * (0.6 if extras else 1)))
            try:
                dumps.copy_dlp(plain[game], game_dir)
            except (dumps.DumpError, decrypt.DecryptError) as e:
                print(f'Warning: skipping unreadable optional Download Play data. {e}', flush=True)
        if update:
            info_u = dumps.add_update(update, plain[update], game_dir, lambda d, t: PROGRESS.at(0.6 + d / t * 0.2))
            print(f'update v{update.version}: its code runs as {info_u["program_id"]}', flush=True)
        if dlc:
            print(f'DLC: {dumps.add_dlc(dlc, sources, game_dir, lambda d, t: PROGRESS.at(0.8 + d / t * 0.2))} contents', flush=True)
        archives = ARCHIVES
        os.makedirs(archives, exist_ok=True)  # the title takes whatever is in it, even if that's nothing
        if a.emulator:
            try:
                got = dumps.import_system_archives(a.emulator, archives, tmp)
            except decrypt.DecryptError as e:
                fail(f'Could not decrypt the selected 3DS system files. {e}')
            print(f'system archives from your emulator\'s files: {len(got)}', flush=True)
        # mods go on before anything's recompiled, so their code is recompiled with the game's
        mods.apply(mod_list, game_dir, lambda s: print(s, flush=True))
        for m in mod_list:
            m.close()
    except (dumps.DumpError, decrypt.DecryptError, mods.ModError) as e:
        fail(str(e))
    for d in plain.values():
        d.f.close()
    shutil.rmtree(tmp, ignore_errors=True)
    with open(os.path.join(game_dir, 'code.bin'), 'rb') as source:
        required_archives = dumps.archives_named(source.read())
    try:
        supplied = dumps.install_system_fallbacks(archives, required_archives, ROOT)
        if supplied: print('Bundled open-source system archives: ' + ', '.join(supplied), flush=True)
    except (OSError, dumps.DumpError) as e:
        fail(str(e))
    if not partial and not dumps.have_system_archives(archives):  # a Miitopia built without them stopped on the console with FS "not found" (Nov 9)
        fail('The 3DS system files are missing: the Mii data, without which Mii games stop, and the shared font. Give --emulator Azahar\'s '
             'user folder (after its File > Set Up System Files) or the 0004009b folder GodMode9 copies off your 3DS (title, in CTRNAND).')
    lacking = [t for t in required_archives if not dumps.have_system_archives(archives, (t,))]
    if lacking and not partial:
        t = lacking[0]
        fail(f'This game needs {dumps.ARCHIVE_NAMES.get(t, "a font")} to start. Your 3DS system files are missing a readable copy. '
             f'With GodMode9, copy the {t[:8]} folder off your 3DS ([1:] SYSNAND CTRNAND > title), '
             f'next to the 0004009b one, and choose the folder again under "3DS system files". Azahar\'s File > Set Up System Files brings it too.')
    # what earlier runs of a game learned (render states it uses; code only static analysis misses) goes with it, if the code is the same.
    # An untested game's live under its own slug (games/g_<code>/shader_logs): a run's shader_misses.bin put there is compiled ahead next time.
    log_slug = info['slug'] if child_id is not None else (known or derived(game))['slug']
    logs = os.path.join(ROOT, 'games', log_slug, 'shader_logs')
    if os.path.isdir(logs) and not a.cold and not code_mods:
        os.makedirs(os.path.join(game_dir, 'shader_logs'), exist_ok=True)
        for f in os.listdir(logs):
            if not f.startswith('other_') and os.path.isfile(os.path.join(logs, f)):
                shutil.copyfile(os.path.join(logs, f), os.path.join(game_dir, 'shader_logs', f))
    for f in [] if a.cold or code_mods else console_learned(log_slug):
        os.makedirs(os.path.join(game_dir, 'shader_logs'), exist_ok=True)
        shutil.copyfile(f, os.path.join(game_dir, 'shader_logs', os.path.basename(f)))

    # after the dumps, as it sets the toolchain up as it loads; by path, so the standalone app uses the repository's copy and its paths
    import importlib.util
    spec = importlib.util.spec_from_file_location('build3ds', os.path.join(ROOT, 'tools', 'win', 'build3ds.py'))
    b3 = importlib.util.module_from_spec(spec)
    sys.modules['build3ds'] = b3
    spec.loader.exec_module(b3)
    b3.WORK = os.path.join(BUILD, 'build_win_builder').replace('\\', '/')
    prepare_vertex_logs = b3.prepare_vertex_logs

    def with_vertex_programs(g, shadergen):  # the standalone app's logs name vertex programs by fingerprint: this dump's own dvlb.bin has their code
        prepare_vertex_logs(g, shadergen)
        made, left = shaderlogs.restore(g.dir + '/shader_logs')
        if made or left:
            print(f'vertex records from earlier runs: {made}, and {left} whose program this dump hasn\'t', flush=True)
    if not os.path.exists(b3.BIN + 'gcc.exe'):
        fail('The build tools aren\'t installed: see builder/README.md.')
    g = b3.Game(info['slug'], game_dir)  # the recipe every game gets, read from this game's own code (build3ds.py)
    g.observation_slug = (known or derived(game))['slug']
    g.skip_catalog = bool(a.cold or code_mods)
    if a.cold:
        g.twin = derived(game)['slug']  # its own folder in games/ is no other game's: nothing of it is borrowed
    # Cold checks exclude learned data, while testing the same compiler policy as a normal build.
    if a.mixed or (profile and profile.get('mixed') and not update and not code_mods):
        g.mixed()
    if g.mode == 'safe':
        print('hand-written code that keeps values in any register across its calls (Mobiclip\'s video decoder): every register goes through memory', flush=True)
    if update or a.cold or code_mods:
        g.upstream_functions = None  # learned base-code entries do not describe changed code or a cold build
    if a.cold:
        g.pipelines = None
    out = os.path.join(game_dir, 'out_measure' if b3.MEASURE else 'out')  # a measuring build apart from the one for a console
    g.out, g.rpx, g.nm, g.shaders = out, os.path.join(out, info['slug'] + '.rpx'), os.path.join(out, 'game.nm'), os.path.join(out, 'shaders')
    os.makedirs(out, exist_ok=True)

    if need_code:
        PROGRESS.stage('Finding the game\'s code')
        # What earlier runs reached stays, and the finders add to it: a tested game's list (its base code's alone), or an untested game's own
        # games/g_<code>[_v<update>]/functions.txt, the entries its runs found missing (Kirby: Triple Deluxe's first call at 0x232670, 2026-11-08)
        learned = os.path.join(ROOT, 'games', known['slug'] if known else info['slug'], 'functions.txt')
        have_learned = not a.cold and not code_mods and not (known and update) and os.path.isfile(learned) and os.path.getsize(learned) > 0
        if have_learned:
            shutil.copyfile(learned, g.extra_functions)
        have_upstream = g.upstream_functions and os.path.isfile(g.upstream_functions) and os.path.getsize(g.upstream_functions) > 0
        if known and not update and not code_mods and (have_learned or have_upstream):
            print('code entries: what this game\'s tested builds found', flush=True)
        else:
            env = dict(os.environ, RECOMP_CRO_BASE=b3.CRO_BASE)
            for tool in ('find_entries.py', 'cro_entries.py'):
                r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', tool), g.dir] + (['--write'] if tool == 'find_entries.py' else []), env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
                print(r.stdout.strip()[-300:], flush=True)
                if r.returncode:
                    fail(f'{tool} failed')

    args = types.SimpleNamespace(jobs=a.jobs, force=False, extra_flags='-DRECOMP_CLASSIC_MENU' if a.menu_style == 'classic' else None, only=None, opt=None)
    original_stdout = sys.stdout
    sys.stdout = held = Held(Watch(original_stdout, PROGRESS), b3.context)
    shader_stop = []

    def shaders():  # they need none of the code, so they're made while it compiles (Zelda's took 21 of its 254 s)
        b3.context.tag = 'shaders'
        try:
            b3.build_shaders(g, args)
        except BaseException as e:  # build3ds stops with SystemExit; anything else keeps its traceback, shown with the stage's output
            if not isinstance(e, SystemExit):
                import traceback
                traceback.print_exc(file=held.held)
            shader_stop.append(e)

    alongside = threading.Thread(target=shaders)
    try:
        # Resolve portable vertex/geometry records before parallel code and fragment compilation.
        if need_code or need_shaders:
            with_vertex_programs(g, b3.build_shadergen(args))
            b3.prepare_vertex_logs = lambda game, shadergen: None
        if need_code:
            PROGRESS.stage('Recompiling')
            if need_shaders: alongside.start()
            try:
                b3.build_code(g, args)
            finally:
                if need_shaders: alongside.join()
        if need_shaders:
            PROGRESS.stage('Shaders')
            if need_code: alongside.join()
            else: shaders()
            print(held.held.getvalue(), end='', flush=True, file=sys.__stdout__)
            if shader_stop:
                raise shader_stop[0] if isinstance(shader_stop[0], SystemExit) else SystemExit(f'shaders: {shader_stop[0]!r}')
    except SystemExit as e:
        fail(f'The build stopped: {e}')
    finally:
        sys.stdout = original_stdout
    learned, dvlb = os.path.join(ROOT, 'games', log_slug, 'shader_logs'), os.path.join(game_dir, 'shader_logs', 'dvlb.bin')
    if not known and not a.cold and not code_mods and WORK == ROOT and os.path.isfile(dvlb):  # its vertex programs, where its runs' logs go: make_app.py fingerprints the
        os.makedirs(learned, exist_ok=True)  # shipped logs' vertex records by them (shaderlogs.py), and ships no dvlb.bin
        shutil.copyfile(dvlb, os.path.join(learned, 'dvlb.bin'))
        gsl = os.path.splitext(dvlb)[0] + '.gsl'  # and its geometry programs, the same way
        if os.path.isfile(gsl):
            shutil.copyfile(gsl, os.path.join(learned, 'dvlb.gsl'))

    # Every game and packaging mode uses the same optional SD-root file.
    import multiplayer
    try:
        room_file = multiplayer.write_config(a.card or a.out,
                         os.path.join(ROOT, 'builder', 'multiplayer-defaults.json'))
        print(f'Shared multiplayer settings: {room_file} (copy 3dii-u to the SD root)', flush=True)
    except (OSError, ValueError, KeyError) as e:
        fail(f'Multiplayer configuration could not be written: {e}')

    if partial:
        PROGRESS.stage('Exporting Homebrew update')
        guide = os.path.join(HERE, 'HOMEBREW_GUIDE.txt')
        meta = homebrew.identity(game_dir, game.title_id, info['name'], ROOT)
        destination = os.path.join(a.card, 'wiiu', '3dii-u', 'updates') if a.card else a.out
        if a.card: PROGRESS.stage('Copying')
        try:
            where = homebrew.export_update(g.rpx, g.shaders, destination, info['name'], a.homebrew_parts, meta, guide)
        except (OSError, ValueError) as e: fail(f'Homebrew export failed: {e}')
        if not a.keep: shutil.rmtree(game_dir, ignore_errors=True)
        PROGRESS.done, PROGRESS.share = 100, 0
        PROGRESS.at(1)
        print(f'@done {where}', flush=True)
        return

    PROGRESS.stage('Making the title')
    released, case_scan, title_shot = '', False, None
    if a.art_style == 'console' and a.tdb:
        import covers  # only here: the downloads a build may make, and only with the Virtual Console's screens chosen
        released = covers.released(game.product, a.tdb)
        case_scan = bool(a.art) and covers.is_case_scan(a.art, a.tdb)  # the folder is the covers' own
        title_shot = covers.title_screen(game.product, [game.name] + covers.other_names(game.product, a.tdb), a.tdb)
    make_title(game_dir, out, info, a.art, a.icon_art, a.art_style, released, case_scan, a.pad_bezel, a.pad_art, a.boot_sound, title_shot,
               language=a.language, include_shaders=need_shaders)
    if a.save:
        import saves
        try:
            saves.pack(a.save, os.path.join(out, 'title', 'content'))
        except (saves.SaveError, OSError) as e:
            fail(f"The 3DS save couldn't be packed in: {e}")
    os.makedirs(os.path.join(out,'title','content'),exist_ok=True)
    with open(os.path.join(out,'title','content','menu_enabled.txt'),'w',encoding='ascii',newline='\n') as f:
        f.write('0\n' if a.no_menu else '1\n')
    with open(os.path.join(out,'title','content','menu_shortcut.txt'),'w',encoding='ascii') as f:
        f.write(((a.menu_shortcut or '') if not a.no_menu else '') + '\n')
    if a.no_menu:  # the game looks for it as it starts (frontend_wiiu.cpp)
        open(os.path.join(out, 'title', 'content', 'no_menu'), 'wb').close()
    elif a.menu_shortcut:
        with open(os.path.join(out, 'title', 'content', 'menu_shortcut.txt'), 'w', encoding='ascii', newline='\n') as f:
            f.write(a.menu_shortcut + '\n')

    if b3.MEASURE and not b3.MEASURE_CONSOLE:  # the checks' own kind of build, for Cemu (build3ds.py): packed only when asked by name
        print('a measuring build: not packed', flush=True)
        PROGRESS.done, PROGRESS.share = 100, 0
        PROGRESS.at(1)
        print(f'@done {out}', flush=True)
        return

    if a.output_format == 'homebrew':
        PROGRESS.stage('Packing Homebrew')
        destination = os.path.join(a.card, 'wiiu', 'apps') if a.card else a.out
        path = os.path.join(destination, homebrew.filename(info['name']) + '.wuhb')
        guide = os.path.join(HERE, 'HOMEBREW_GUIDE.txt')
        meta = homebrew.identity(game_dir, game.title_id, info['name'], ROOT)
        try:
            where = homebrew.bundle(g.rpx, os.path.join(out, 'title'), path, info['name'],
                                   os.path.join(b3.DKP, 'tools', 'bin', 'wuhbtool.exe'), meta, guide)
        except (OSError, ValueError, subprocess.CalledProcessError) as e: fail(f'Homebrew packaging failed: {e}')
        if not a.keep: shutil.rmtree(game_dir, ignore_errors=True)
        PROGRESS.done, PROGRESS.share = 100, 0
        PROGRESS.at(1)
        print(f'@done {where}', flush=True)
        return

    PROGRESS.stage('Packing the WUP')
    # A failed card copy leaves this completed output outside the extraction folder that the next attempt clears.
    if a.out:
        wup = os.path.join(a.out, info['folder'])
    else:
        folder = workspace_child(workspace_child(WORK_GAMES, '_wup'), info['slug'])
        wup = workspace_child(folder, info['folder'])
    title = os.path.join(out, 'title')
    if is_update:
        import xml.etree.ElementTree as ET
        app_xml = ET.parse(os.path.join(title,'code','app.xml'))
        base_id = app_xml.findtext('title_id')
        base_version = int(app_xml.findtext('title_version'),16)
        version, now = wup_update.update_build_info()
        if base_version>version:
            fail('This base version exceeds the fixed native update version. Use a matching base made with this Builder.')
        update_title = os.path.join(out,'update')
        try:
            wup_update.make_update(title,update_title,a.wup_update_parts,version,now,base_manifest=selected_manifest,inputs=input_summary,contract=wup_update.shader_contract(ROOT))
        except (ValueError,OSError) as error: fail('WUP update refused: '+str(error))
        wup += '_Update_' + now.strftime('%Y%m%d_%H%M%S') + f'_v{version}'
        title = update_title
        print(f'Native WUP update: {base_id}, version {version}, built {now.isoformat()}',flush=True)

    if not is_update:
        pending_manifest = os.path.join(out,'base-manifest.json')
        base_record = wup_update.remember_base(pending_manifest,title,int(info['title_id'],16),inputs=input_summary,root=ROOT)
    pack_wup(a.otp, title, wup)
    if not is_update:
        wup_update.write_manifest(inventory_path,base_record)
        wup_update.write_manifest(wup+'.base.json',base_record)
        print('Base build manifest: '+wup+'.base.json',flush=True)

    where = wup
    if a.card:
        PROGRESS.stage('Copying')
        install = os.path.join(a.card.rstrip('\\/') + os.sep, 'install')
        try:
            os.makedirs(install, exist_ok=True)
            size = sum(os.path.getsize(os.path.join(wup, f)) for f in os.listdir(wup))
            if shutil.disk_usage(install).free < size + (64 << 20):
                fail(f'The SD card needs {size >> 20} MB free for this game.')
            where = copy_to(wup, install)
            if not is_update: wup_update.write_manifest(where+'.base.json',base_record)
        except OSError as e:  # the WUP stays in the work folder, as with a folder build
            fail(card_error(e) + f' The game is built: it is in {wup}.')  # one line: the window reads @error lines
        if not a.out:
            shutil.rmtree(wup, ignore_errors=True)
    if not a.keep:  # the runtime's objects stay: every game shares them
        shutil.rmtree(workspace_child(WORK_GAMES, info['slug']), ignore_errors=True)
        shutil.rmtree(workspace_child(b3.WORK, f'ppc_{info["slug"]}'), ignore_errors=True)
    PROGRESS.done, PROGRESS.share = 100, 0
    PROGRESS.at(1)
    print(f'@done {where}', flush=True)


if __name__ == '__main__':
    main()
