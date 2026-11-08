#!/usr/bin/env python3
"""Turns a 3DS game dump into a WUP for WUP Installer GX2, with its update and DLC if given. The app (builder/app.py) runs this; it works on its own too:

    pipeline.py --game <dump> [--update <dump>] [--dlc <dump>] [--mod <folder or .zip> ...] [--keys <folder>] [--emulator <folder>] --otp <otp.bin> (--card D: | --out <folder>) [--keep] [--jobs N]

--keys: the folder with boot9.bin and seeddb.bin from your own 3DS, ~/.3ds by default; only encrypted dumps need it, and they're decrypted the way
cia_to_decrypted_3ds.py does it (decrypt.py). --emulator: an Azahar or Citra user folder, for the system archives (shared font, Mii data) if this PC
has none yet. --otp: your Wii U's otp.bin, for the common key the WUP is packed with.
Lines starting with @ are for the app: @stage <name>, @progress <0-100>, @done <where it went>, @error <why>.

It builds with tools/win/build3ds.py, in folders of its own (builder_work/, build_win_builder/), so it never touches the nine ports' builds."""
import argparse
import filecmp
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('THREEDII_ROOT') or os.path.dirname(HERE)  # the standalone app runs from elsewhere: app.py finds the repository
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'win'))
import decrypt  # noqa: E402
import dumps  # noqa: E402
import mods  # noqa: E402
import shaderlogs  # noqa: E402

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


def mobiclip_tables(game_dir):
    """Offset-table jumps (`sub rB, pc, #n` ... `add pc, rX, rB`) in the game's code. Mobiclip's hand-written video decoder is made of them
    (168 in Mario Kart 7, the Laytons and Paper Jam): it keeps values in any register across its own calls, which only code generated
    without --abi follows. Mario Kart 7's menu videos stopped the game otherwise."""
    with open(os.path.join(game_dir, 'exheader.bin'), 'rb') as f:
        size = struct.unpack_from('<I', f.read(0x40), 0x18)[0]
    with open(os.path.join(game_dir, 'code.bin'), 'rb') as f:
        code = f.read(size)
    words = struct.unpack_from('<%dI' % (len(code) // 4), code)
    n = 0
    for i, w in enumerate(words):
        if w >> 28 != 0xE or (w & 0x0FEF0000) != 0x024F0000:  # sub/add rB, pc, #n
            continue
        rb = (w >> 12) & 15
        for x in words[i + 1:i + 16]:
            if (x & 0xFFF0FFF0) == 0xE080F000 and rb in ((x >> 16) & 15, x & 15):  # add pc, rX, rB
                n += 1
                break
            if (x >> 26) & 3 == 0 and (x >> 12) & 15 == rb:  # rB written again first
                break
    return n


def make_title(game_dir, out, info):
    """tools/make_game_title.py with this game's names, as tools/mario_title.py does, then the update's and DLC's files beside the game's"""
    source = open(os.path.join(ROOT, 'tools', 'make_game_title.py'), encoding='utf-8', newline='').read()
    name = info['name'].replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')
    rpx_name = info['slug'] + '.rpx'
    for pattern, replacement in [
        (r"^TITLE = .*$", f"TITLE = {os.path.join(out, 'title')!r}"),
        (r"^GAME = .*$", f"GAME = {game_dir!r}"),
        (r"^TITLE_ID = .*$", f"TITLE_ID = {info['title_id']!r}"),
        (r"^RPX_NAME = .*$", f"RPX_NAME = {rpx_name!r}"),
        (r"^rpx = .*$", f"rpx = {os.path.join(out, rpx_name)!r}"),
        (r"\.replace\('3DS Recomp Code Test', '[^']*'\)", f".replace('3DS Recomp Code Test', {name!r})"),
        (r"'WUP-N-[A-Z0-9]{4}'", repr(info['product'])),
        (r"os\.path\.join\(ROOT, 'build', 'shaders', f\)", f"os.path.join({os.path.join(out, 'shaders')!r}, f)"),
        (r"^ARCHIVES = .*$", f"ARCHIVES = {ARCHIVES!r}"),
    ]:
        source, count = re.subn(pattern, lambda m: replacement, source, count=1, flags=re.M)
        if count != 1:
            fail(f'tools/make_game_title.py has changed: no match for {pattern}')
    exec(compile(source, 'make_game_title.py', 'exec'), {'__file__': os.path.join(ROOT, 'tools', 'make_game_title.py'), '__name__': '__main__'})
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', required=True)
    ap.add_argument('--update')
    ap.add_argument('--dlc')
    ap.add_argument('--mod', action='append', default=[], help="a mod in Luma's layout (luma/titles/<title id>/: romfs, code.ips/.bps/.bin); several go on in order")
    ap.add_argument('--keys', help='boot9.bin and seeddb.bin from your own 3DS (default ~/.3ds)')
    ap.add_argument('--emulator', help='an Azahar or Citra user folder, for the system archives')
    ap.add_argument('--otp', required=True)
    ap.add_argument('--card', help='an SD card: the WUP goes in its install folder')
    ap.add_argument('--out', help='a folder to put the WUP in instead')
    ap.add_argument('--keep', action='store_true', help='keep the work files, for a quicker rebuild')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    if sys.platform != 'win32':
        fail('Building on Linux isn\'t ready yet: this version builds on Windows.')
    if not (a.card or a.out):
        fail('Choose an SD card or a folder for the WUP.')
    if not os.path.isfile(a.otp) or os.path.getsize(a.otp) != 1024:
        fail('That otp.bin isn\'t a Wii U OTP dump: it should be exactly 1024 bytes.')

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
    if not os.path.isdir(archives) or not os.listdir(archives):
        print('No system archives (the 3DS\'s shared font and Mii data): games that use them will show no text. Your emulator\'s user folder has them.', flush=True)
    # what earlier runs of a game learned (render states it uses; code only static analysis misses) goes with it, if the code is the same.
    # An untested game's live under its own slug (games/g_<code>/shader_logs): a run's shader_misses.bin put there is compiled ahead next time.
    if True:
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
    g = b3.Game(known['slug'] if known else info['slug'])
    g.name = info['slug']
    g.dir = game_dir.replace('\\', '/')
    if not known and mobiclip_tables(game_dir) >= 8:
        # built the way the Layton ports run the same decoder: every register through memory at calls, -O1 (see build3ds.py's Layton family)
        g.gen, g.mode, g.opt = 'gen_safe', 'safe', ['-O1']
        print('a hand-written video decoder (Mobiclip): built the way the Layton ports are', flush=True)
    g.gen_dir = f'{g.dir}/{g.gen}'
    out = os.path.join(game_dir, 'out')
    g.out, g.rpx, g.nm, g.shaders = out, os.path.join(out, info['slug'] + '.rpx'), os.path.join(out, 'game.nm'), os.path.join(out, 'shaders')
    os.makedirs(out, exist_ok=True)

    PROGRESS.stage('Finding the game\'s code')
    if known and not update and not any(m.changes_code() for m in mod_list):
        print('code entries: what this game\'s tested builds found', flush=True)  # g.extra_functions as build3ds has it
    else:
        # What earlier runs reached stays and the finders add to it: a tested game's list (its base code only), or an untested game's own
        # games/g_<code>[_v<update>]/functions.txt, the entries its runs found missing (Kirby: Triple Deluxe's first call at 0x232670, 2026-11-08)
        if not (known and update) and os.path.isfile(g.extra_functions):
            shutil.copyfile(g.extra_functions, f'{g.dir}/functions.txt')
        g.extra_functions = f'{g.dir}/functions.txt'
        # and what the upstream project's own runs of this game found (third_party/3dsrecomp/games/<title id>): Metroid: Samus Returns' one
        # entry there, 0x1010bb, was the call it stopped at without it (2026-11-08)
        upstream = os.path.join(ROOT, 'third_party', '3dsrecomp', 'games', '%016x' % game.title_id, 'functions.txt')
        if not update and os.path.isfile(upstream):
            with open(g.extra_functions, 'a', encoding='utf-8') as o:
                o.write('\n' + open(upstream, encoding='utf-8').read().strip() + '\n')
        env = dict(os.environ, RECOMP_CRO_BASE=g.cro_base or '0x1C000000')
        for tool in ('find_entries.py', 'cro_entries.py'):
            r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', tool), g.dir] + (['--write'] if tool == 'find_entries.py' else []), env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            print(r.stdout.strip()[-300:], flush=True)
            if r.returncode:
                fail(f'{tool} failed')

    args = types.SimpleNamespace(jobs=a.jobs, force=False, extra_flags=None, only=None, opt=None)
    sys.stdout = Watch(sys.__stdout__, PROGRESS)
    try:
        PROGRESS.stage('Recompiling')
        b3.build_code(g, args)
        PROGRESS.stage('Shaders')
        b3.build_shaders(g, args)
    except SystemExit as e:
        sys.stdout = sys.__stdout__
        fail(f'The build stopped: {e}')
    sys.stdout = sys.__stdout__
    learned, dvlb = os.path.join(ROOT, 'games', derived(game)['slug'], 'shader_logs'), os.path.join(game_dir, 'shader_logs', 'dvlb.bin')
    if not known and WORK == ROOT and os.path.isfile(dvlb):  # its vertex programs, where its runs' logs go: make_app.py fingerprints the
        os.makedirs(learned, exist_ok=True)  # shipped logs' vertex records by them (shaderlogs.py), and ships no dvlb.bin
        shutil.copyfile(dvlb, os.path.join(learned, 'dvlb.bin'))

    PROGRESS.stage('Making the title')
    make_title(game_dir, out, info)

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
