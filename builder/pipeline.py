#!/usr/bin/env python3
"""Turns a 3DS game dump into a WUP for WUP Installer GX2, with its update and DLC if given. The app (builder/app.py) runs this; it works on its own too:

    pipeline.py --game <dump> [--update <dump>] [--dlc <dump>] [--mod <folder or .zip> ...] [--keys <folder>] [--emulator <folder>] --otp <otp.bin> (--card D: | --out <folder>) [--keep] [--jobs N]

--keys: the folder with boot9.bin and seeddb.bin from your own 3DS, ~/.3ds by default; only encrypted dumps need it, and they're decrypted the way
cia_to_decrypted_3ds.py does it (decrypt.py). --emulator: a 3DS emulator's user folder (Azahar, Citra) or the 0004009b folder GodMode9 copies off
your 3DS, for the system archives (shared font, Mii data) if this PC has none yet. --otp: your Wii U's otp.bin, for the common key the WUP is
packed with.
Lines starting with @ are for the app: @stage <name>, @progress <0-100>, @done <where it went>, @error <why>.

It builds with tools/win/build3ds.py, in folders of its own (builder_work/, build_win_builder/), so it never touches the nine ports' builds."""
import argparse
import filecmp
import io
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('THREEDII_ROOT') or os.path.dirname(HERE)  # the standalone app runs from elsewhere: app.py finds the repository
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'win'))
import credits  # noqa: E402
import decrypt  # noqa: E402
import dumps  # noqa: E402
import mods  # noqa: E402
import shaderlogs  # noqa: E402
import user_files  # noqa: E402

WORK = os.environ.get('THREEDII_WORK') or ROOT  # the standalone app's engine may be read-only: then it builds in a folder of the user's
WORK_GAMES = os.path.join(WORK, 'builder_work')
ARCHIVES = os.path.join(WORK, 'system_archives') if WORK != ROOT else os.path.join(ROOT, 'games', 'system_archives')
STAGES = [('Reading your game', 2), ('Decrypting', 8), ('Unpacking', 7), ('Finding the game\'s code', 2), ('Recompiling', 48), ('Shaders', 14),
          ('Making the title', 2), ('Packing the WUP', 10), ('Copying', 7)]


class Progress:
    """@stage and @progress lines over the whole run, from each stage's share"""
    def __init__(self):
        self.done, self.share = 0, 0

    def stage(self, name):
        for n, (s, w) in enumerate(STAGES):
            if s == name:
                self.done = sum(x for _, x in STAGES[:n])
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
    code = (dump.product or 'CTR-X-XXXX').split('-')[-1][:4].upper().ljust(4, 'X')
    folder = re.sub(r'[^A-Za-z0-9]+', '_', dump.name or code).strip('_') or code
    return {'slug': 'g_' + code.lower(), 'title_id': '00050000%08X' % (0x3D000000 | (dump.unique >> 8)), 'name': dump.name or code, 'product': 'WUP-N-' + code,
            'folder': folder, 'status': 'Untested', 'note': 'Not tested on a console yet: it may not start, or stop partway.'}


def make_title(game_dir, out, info, art=None):
    """the title every game gets (tools/make_game_title.py), then the update's and DLC's files beside the game's"""
    import importlib.util
    spec = importlib.util.spec_from_file_location('make_game_title', os.path.join(ROOT, 'tools', 'make_game_title.py'))
    titles = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(titles)
    titles.make_title(os.path.join(out, 'title'), game_dir, os.path.join(out, info['slug'] + '.rpx'), os.path.join(out, 'shaders'),
                      info['title_id'], info['name'], info['product'], ARCHIVES, art)
    content = os.path.join(out, 'title', 'content')
    for f in os.listdir(game_dir):
        if f.startswith(('update_', 'dlc_')):
            os.link(os.path.join(game_dir, f), os.path.join(content, f))


def copy_to(source, target_dir):
    """the WUP folder into target_dir, compared file by file; what was there is replaced only once the new copy is right"""
    name = os.path.basename(source)
    target = os.path.join(target_dir, name)
    new = target + '_new'
    shutil.rmtree(new, ignore_errors=True)
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
        shutil.rmtree(new, ignore_errors=True)
        fail('The copy came out different from the original: the card may be failing. Nothing was replaced.')
    if os.path.isdir(target):
        shutil.rmtree(target)
    os.rename(new, target)
    return target


PROGRESS = Progress()


class ShowCredits(argparse.Action):
    """--credits: who the builder is built on, then exit, before the required options are asked for (as --version does)"""

    def __call__(self, parser, namespace, values, option_string=None):
        print(credits.text())
        parser.exit()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', action='version', version=f'{credits.APP} {credits.VERSION} ({credits.LICENCE}, {credits.SOURCE})')
    ap.add_argument('--credits', nargs=0, action=ShowCredits, help='the projects the builder is built on, and their licences')
    ap.add_argument('--game', required=True)
    ap.add_argument('--update')
    ap.add_argument('--dlc')
    ap.add_argument('--mod', action='append', default=[], help="a mod in Luma's layout (luma/titles/<title id>/: romfs, code.ips/.bps/.bin); several go on in order")
    ap.add_argument('--keys', help='boot9.bin and seeddb.bin from your own 3DS (default ~/.3ds)')
    ap.add_argument('--emulator', help='a 3DS emulator\'s user folder (Azahar, Citra) or the 0004009b folder GodMode9 copies off your 3DS '
                                         '(title, in CTRNAND), for the system archives')
    ap.add_argument('--otp', required=True)
    ap.add_argument('--card', help='an SD card: the WUP goes in its install folder')
    ap.add_argument('--out', help='a folder to put the WUP in instead')
    ap.add_argument('--keep', action='store_true', help='keep the work files, for a quicker rebuild')
    ap.add_argument('--art', help="a picture (the cover art, say) for the start-up screens and the Wii U Menu icon; the game's own icon otherwise")
    ap.add_argument('--mixed', action='store_true', help='trial: fast code with only the video decoder in safe mode (build3ds.Game.mixed)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    if sys.platform != 'win32':
        fail('Building on Linux isn\'t ready yet: this version builds on Windows.')
    if not (a.card or a.out):
        fail('Choose an SD card or a folder for the WUP.')
    problem = user_files.check_otp(a.otp)
    if problem:
        fail(problem)

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
    known = profile_for(game)
    info = dict(known or derived(game))
    if update:
        info['slug'] += f'_v{update.version}'
    print(f'{info["name"]} ({game.title_id:016X}): {info["status"]}' + (f', update v{update.version}' if update else '') + (', with DLC' if dlc else '')
          + (f', {len(mod_list)} mod{"s" * (len(mod_list) > 1)}' if mod_list else ''), flush=True)

    game_dir = os.path.join(WORK_GAMES, info['slug'])
    tmp = game_dir + '_decrypting'  # plain copies of what came encrypted, gone once unpacked
    need = sum(d.size * (4 if d is game else 3) for d in [game] + extras) + (3 << 30)
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
        dumps.extract_program(plain[game], game_dir, lambda d, t: PROGRESS.at(d / t * (0.6 if extras else 1)))
        if update:
            info_u = dumps.add_update(update, plain[update], game_dir, lambda d, t: PROGRESS.at(0.6 + d / t * 0.2))
            print(f'update v{update.version}: its code runs as {info_u["program_id"]}', flush=True)
        if dlc:
            print(f'DLC: {dumps.add_dlc(dlc, sources, game_dir, lambda d, t: PROGRESS.at(0.8 + d / t * 0.2))} contents', flush=True)
        archives = ARCHIVES
        os.makedirs(archives, exist_ok=True)  # the title takes whatever is in it, even if that's nothing
        if len(os.listdir(archives)) < len(dumps.SYSTEM_ARCHIVES) and a.emulator:
            got = dumps.import_system_archives(a.emulator, archives, tmp)
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
    if not dumps.have_system_archives(archives):  # a Miitopia built without them stopped on the console with FS "not found" (Nov 9)
        fail('The 3DS system files are missing: the Mii data, without which Mii games stop, and the shared font. Give --emulator Azahar\'s '
             'user folder (after its File > Set Up System Files) or the 0004009b folder GodMode9 copies off your 3DS (title, in CTRNAND).')
    # what earlier runs of a game learned (render states it uses; code only static analysis misses) goes with it, if the code is the same.
    # An untested game's live under its own slug (games/g_<code>/shader_logs): a run's shader_misses.bin put there is compiled ahead next time.
    logs = os.path.join(ROOT, 'games', (known or derived(game))['slug'], 'shader_logs')
    if os.path.isdir(logs):
        os.makedirs(os.path.join(game_dir, 'shader_logs'), exist_ok=True)
        for f in os.listdir(logs):
            if not f.startswith('other_') and os.path.isfile(os.path.join(logs, f)):
                shutil.copyfile(os.path.join(logs, f), os.path.join(game_dir, 'shader_logs', f))

    # after the dumps, as it sets the toolchain up as it loads; by path, so the standalone app uses the repository's copy and its paths
    import importlib.util
    spec = importlib.util.spec_from_file_location('build3ds', os.path.join(ROOT, 'tools', 'win', 'build3ds.py'))
    b3 = importlib.util.module_from_spec(spec)
    sys.modules['build3ds'] = b3
    spec.loader.exec_module(b3)
    b3.WORK = os.path.join(WORK, 'build_win_builder').replace('\\', '/')
    prepare_logs = b3.prepare_logs

    def with_vertex_programs(g):  # the standalone app's logs name vertex programs by fingerprint: this dump's own dvlb.bin has their code
        prepare_logs(g)
        made, left = shaderlogs.restore(g.dir + '/shader_logs')
        if made or left:
            print(f'vertex records from earlier runs: {made}, and {left} whose program this dump hasn\'t', flush=True)
    b3.prepare_logs = with_vertex_programs
    if not os.path.exists(b3.BIN + 'gcc.exe'):
        fail('The build tools aren\'t installed: see builder/README.md.')
    g = b3.Game(info['slug'], game_dir)  # the recipe every game gets, read from this game's own code (build3ds.py)
    if a.mixed:
        g.mixed()
    if g.mode == 'safe':
        print('hand-written code that keeps values in any register across its calls (Mobiclip\'s video decoder): every register goes through memory', flush=True)
    if update:
        g.upstream_functions = None  # the 3dsrecomp project's lists are of the base game's code, which the update replaces
    out = os.path.join(game_dir, 'out')
    g.out, g.rpx, g.nm, g.shaders = out, os.path.join(out, info['slug'] + '.rpx'), os.path.join(out, 'game.nm'), os.path.join(out, 'shaders')
    os.makedirs(out, exist_ok=True)

    PROGRESS.stage('Finding the game\'s code')
    # What earlier runs reached stays, and the finders add to it: a tested game's list (its base code's alone), or an untested game's own
    # games/g_<code>[_v<update>]/functions.txt, the entries its runs found missing (Kirby: Triple Deluxe's first call at 0x232670, 2026-11-08)
    learned = os.path.join(ROOT, 'games', known['slug'] if known else info['slug'], 'functions.txt')
    if not (known and update) and os.path.isfile(learned):
        shutil.copyfile(learned, g.extra_functions)
    if known and not update and not any(m.changes_code() for m in mod_list):
        print('code entries: what this game\'s tested builds found', flush=True)
    else:
        env = dict(os.environ, RECOMP_CRO_BASE=b3.CRO_BASE)
        for tool in ('find_entries.py', 'cro_entries.py'):
            r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', tool), g.dir] + (['--write'] if tool == 'find_entries.py' else []), env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            print(r.stdout.strip()[-300:], flush=True)
            if r.returncode:
                fail(f'{tool} failed')

    args = types.SimpleNamespace(jobs=a.jobs, force=False, extra_flags=None, only=None, opt=None)
    sys.stdout = held = Held(Watch(sys.__stdout__, PROGRESS), b3.context)
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

    alongside = threading.Thread(target=shaders, daemon=True)
    try:
        PROGRESS.stage('Recompiling')
        alongside.start()
        b3.build_code(g, args)
        PROGRESS.stage('Shaders')
        alongside.join()
        print(held.held.getvalue(), end='', flush=True)
        if shader_stop:
            raise shader_stop[0] if isinstance(shader_stop[0], SystemExit) else SystemExit(f'shaders: {shader_stop[0]!r}')
    except SystemExit as e:
        sys.stdout = sys.__stdout__
        fail(f'The build stopped: {e}')
    sys.stdout = sys.__stdout__
    learned, dvlb = os.path.join(ROOT, 'games', derived(game)['slug'], 'shader_logs'), os.path.join(game_dir, 'shader_logs', 'dvlb.bin')
    if not known and WORK == ROOT and os.path.isfile(dvlb):  # its vertex programs, where its runs' logs go: make_app.py fingerprints the
        os.makedirs(learned, exist_ok=True)  # shipped logs' vertex records by them (shaderlogs.py), and ships no dvlb.bin
        shutil.copyfile(dvlb, os.path.join(learned, 'dvlb.bin'))

    PROGRESS.stage('Making the title')
    make_title(game_dir, out, info, a.art)

    PROGRESS.stage('Packing the WUP')
    wup = os.path.join(a.out or game_dir, info['folder'])
    for _ in range(120):  # the packer holds the whole game in memory
        if b3.free_mb() > 1500:
            break
        time.sleep(15)
    shutil.rmtree(wup, ignore_errors=True)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'pack_game_wup.py'), a.otp, os.path.join(out, 'title'), wup], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(r.stdout.strip()[-400:], flush=True)
    if r.returncode:  # a failed pack can leave a title.tmd behind: only the exit code counts
        shutil.rmtree(wup, ignore_errors=True)
        fail('Packing the WUP failed.')

    where = wup
    if a.card:
        PROGRESS.stage('Copying')
        install = os.path.join(a.card.rstrip('\\/') + os.sep, 'install')
        os.makedirs(install, exist_ok=True)
        size = sum(os.path.getsize(os.path.join(wup, f)) for f in os.listdir(wup))
        if shutil.disk_usage(install).free < size + (64 << 20):
            fail(f'The SD card needs {size >> 20} MB free for this game.')
        where = copy_to(wup, install)
        shutil.rmtree(wup, ignore_errors=True)
    if not a.keep:  # the runtime's objects stay: every game shares them
        shutil.rmtree(game_dir, ignore_errors=True)
        shutil.rmtree(os.path.join(b3.WORK, f'ppc_{info["slug"]}'), ignore_errors=True)
    PROGRESS.done, PROGRESS.share = 100, 0
    PROGRESS.at(1)
    print(f'@done {where}', flush=True)


if __name__ == '__main__':
    main()
