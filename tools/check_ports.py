#!/usr/bin/env python3
"""Builds games and checks each one in Cemu before anything goes near a console: check_ports.py [--no-build] [--seconds N] [--bless] [game ... | all]
"all" is every game this PC has: the nine ports (GAMES, built by tools/win/build3ds.py), the scenes of them a route can't play its way to
(SCENES: NSMB2's tower and castle) and every game in games/sources.json, built from its dump by the builder's pipeline as a tester's would be.
sources.json is this PC's own (git-ignored):
    {"otp": "<otp.bin>", "games": {"<name>": {"game": "<dump>", "update": "<dump>", "dlc": "<dump>", "route": "<a port's name>"}}}
For each game: code, shaders and title; a port's RPX against what the console's loader wants; then its route from test_routes.py (GENERIC if it
has none) on a blank save, and a restart on what that saved, which ends by quitting the game as a player does: it has to close.
The games are built as the builder makes them, without what only measures a game: then a log has no frame rates or costs, and the pictures,
the crashes, the sound's gaps and the rest decide. --measure builds and checks the measuring kind, which has them, and the scenes.
Each leg's screenshots, of the TV and of the GamePad, are kept in games/<folder>/check_shots/<game>/<leg> and held to the good runs in
games/<folder>/good_shots/<game>/<leg>: a game fails when, against the nearest good run, two or more pictures of a screen are in one and not in
the other (pictures()), and so also when it draws nothing where a good run drew. A game with no good run says so. --bless runs nothing: it keeps
the last run's pictures as a good run, for once they've been looked at (tools/contact_sheet.py lays them out, a sheet per game).
A game also fails on a crash, code the translation missed, a vertex shader that wasn't compiled, a failed save flush, a save that formats itself twice, a save in common storage
instead of the player's, or files not going straight to the file system. Fragment misses and frame rates are reported, not judged: Cemu has no real GPU. Shaders a run missed go to shader_logs, for the next build. Exit code 0 if all passed.
Cemu is a private copy in build_win/cemu (a renamed exe, so other sessions' scripts leave it alone), run by tools/win/cemu_run.ps1.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(__file__))
from test_routes import GENERIC, ROUTES

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
CEMU = ROOT + '/build_win/cemu'
GAMES = {'sticker_star': '77', 'nsmb2': '78', 'dream_team': '79', 'paper_jam': '80', 'superstar_saga': '83', 'bowsers_inside': '84', 'layton_azran': '85', 'layton_mask': '86', 'tomodachi': '67'}
OUT = {'layton_mask': 'build_layton/layton_mask', 'layton_azran': 'build_layton/layton_azran', 'tomodachi': 'build'}
MEASURE = False  # --measure: the measuring kind of build, which build3ds.py and the builder's pipeline keep in folders of their own
SOURCES = ROOT + '/games/sources.json'
# A scene is a port run once more with switches of the runtime set (env.txt in its data folder, frontend_wiiu.cpp): a level a route can't reach
# by playing, loaded in place of the one it enters. NSMB2's files: <world>-14 is a world's tower, -16 its castle, -12 its ghost house.
SCENES = {'nsmb2_tower': ('nsmb2', {'RECOMP_ROMFS_SWAP': 'Course/1-1.sarc=Course/1-14.sarc'}),
          'nsmb2_castle': ('nsmb2', {'RECOMP_ROMFS_SWAP': 'Course/1-1.sarc=Course/1-16.sarc'})}
CRASH =r'FATAL|no recompiled code at|\[crash\]'  # the game's log on a crash of its own


@dataclass
class Target:
    """a built game, as Cemu runs it"""
    name: str
    low: str       # the low half of its Wii U title ID: where Cemu keeps its saves
    out: str       # the folder its title folder is in
    data: str      # its folder in games/, for screenshots and the shaders a run missed
    interp: bool = False  # Cemu's own recompiler can't run this one's code: its interpreter does
    route: str = ''       # whose route in test_routes.py it plays, if not one under its own name
    env: dict = field(default_factory=dict)  # a scene's switches (SCENES)

    @property
    def common(self):
        return f'{CEMU}/portable/mlc01/usr/save/00050000/{self.low}/user/common'

    @property
    def player(self):  # the player's own storage, where the game's saves go (save_storage.cpp): Cemu's first account
        return f'{CEMU}/portable/mlc01/usr/save/00050000/{self.low}/user/80000001'

    @property
    def rpx(self):
        code = self.out + '/title/code'
        return code + '/' + next(f for f in os.listdir(code) if f.endswith('.rpx'))


def sources():
    if not os.path.exists(SOURCES):
        return {'games': {}}
    with open(SOURCES, encoding='utf-8') as f:
        return json.load(f)


def run(*cmd):
    return subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')


def port(game):
    out = f'build_measure/{game}' if MEASURE else OUT.get(game, f'build_mario/{game}')
    return Target(game, '103344' + GAMES[game], ROOT + '/' + out, f'{ROOT}/games/{game}', game == 'paper_jam')


def build_port(game):
    """the target, or what went wrong as a string"""
    r = run(sys.executable, 'tools/win/build3ds.py', 'all', game)
    if r.returncode:
        return 'build failed: ' + r.stdout[-400:]
    t = port(game)
    r = run(sys.executable, 'tools/rpx_check.py', t.rpx, t.out + '/title/code/cos.xml')
    return t if 'no problems' in r.stdout else 'RPX check: ' + r.stdout[-300:]


def built_from_dump(name, since=0):
    """the builder's work folder for a game of sources.json, if it's there (and newer than since)"""
    for out in sorted(glob.glob(ROOT + '/builder_work/*/' + ('out_measure' if MEASURE else 'out')), key=os.path.getmtime, reverse=True):
        mark = os.path.dirname(out) + '/check_name.txt'
        if os.path.exists(mark) and open(mark).read() == name and os.path.getmtime(out) >= since and os.path.exists(out + '/title/code/app.xml'):
            tid = re.search(r'00050000([0-9A-Fa-f]{8})', open(out + '/title/code/app.xml').read())
            slug = os.path.basename(os.path.dirname(out))
            return Target(name, tid.group(1).lower(), out.replace('\\', '/'), f'{ROOT}/games/{slug.split("_v")[0]}')
    return None


def build_from_dump(name, src, otp):
    started = time.time()
    argv = [sys.executable, '-u', 'builder/pipeline.py', '--game', src['game'], '--otp', otp, '--out', ROOT + '/dist/builder', '--keep']
    for part in ('update', 'dlc'):
        if src.get(part):
            argv += ['--' + part, src[part]]
    os.makedirs(ROOT + '/dist/builder', exist_ok=True)
    r = run(*argv)
    err = re.findall(r'@error (.*)', r.stdout)
    if r.returncode or err:
        return 'build failed: ' + (err[-1] if err else r.stdout[-400:])
    for out in glob.glob(ROOT + '/builder_work/*/out*'):  # the pipeline names the folder after the game's code; this says which check it's for
        if os.path.getmtime(out) >= started:
            with open(os.path.dirname(out) + '/check_name.txt', 'w') as f:
                f.write(name)
    return built_from_dump(name, started) or 'build failed: no work folder made'


SHOTS = 6  # pictures of each screen a leg (--shots): more of them follow a transition step by step


def cemu(t, seconds, interp):
    ps = ['powershell', '-NoProfile', '-File', ROOT + '/tools/win/cemu_run.ps1', '-Rpx', t.rpx, '-Low', t.low, '-Seconds', str(seconds), '-Shots', str(SHOTS), '-Out', t.data + '/shots/cemu']
    if interp:
        ps.append('-Interpreter')
    return run(*ps).stdout


UNLIKE = 45  # mean difference per colour channel, of 255, between two pictures shrunk to 64x36: a frame of the same scene a moment apart
#              comes to 2 to 35, the same scene with its background drawn as stripes to 50 to 80 (Superstar Saga, 2026-11-11)


def kept(t, what):
    """where a check keeps a game's logs or pictures: under the check's own name, as a game built with its update shares its folder with the game alone"""
    return f'{t.data}/{what}/{t.name}'


def small(path):
    """a picture shrunk to 64x36, as bytes, or None if it can't be read"""
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QImage
    img = QImage(path)
    if img.isNull():
        return None
    img = img.scaled(64, 36, Qt.IgnoreAspectRatio, Qt.SmoothTransformation).convertToFormat(QImage.Format_RGB888)
    return b''.join(bytes(img.constScanLine(y))[:64 * 3] for y in range(36))


def unlike(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def strangers(mine, theirs):
    """how many of mine are unlike every one of theirs"""
    return sum(1 for m in mine if all(unlike(m, g) > UNLIKE for g in theirs))


def pictures(t, leg):
    """Keeps the leg's screenshots, of both screens, and says what's wrong with them against the good runs: (fault or None, note or None)"""
    mine, good = f'{kept(t, "check_shots")}/{leg}', f'{kept(t, "good_shots")}/{leg}'
    shutil.rmtree(mine, ignore_errors=True)
    os.makedirs(mine)
    for f in glob.glob(t.data + '/shots/cemu/*.png'):  # tv_NN.png and pad_NN.png (cemu_run.ps1)
        shutil.move(f, mine)
    count = {s: len(glob.glob(f'{mine}/{s}_*.png')) for s in ('tv', 'pad')}
    for screen, name in (('tv', 'TV'), ('pad', 'GamePad')):
        if not count[screen]:
            return f'{leg}: no pictures of the {name}', None
    runs = glob.glob(good + '/*/')
    if not runs:
        return None, f'{leg}: no good pictures to compare with yet'
    try:
        shrunk = {s: [x for x in map(small, sorted(glob.glob(f'{mine}/{s}_*.png'))) if x] for s in count}

        def apart(run):  # the most pictures of one screen that this run or that good one has and the other hasn't
            n = 0
            for s in count:
                theirs = [x for x in map(small, glob.glob(f'{run}/{s}_*.png')) if x]
                n = max(n, strangers(shrunk[s], theirs), strangers(theirs, shrunk[s]))
            return n
        nearest = min(map(apart, runs))
    except ImportError:
        return None, f'{leg}: pictures not compared (PySide6 is not installed)'
    if nearest >= 2:
        return f'{leg}: {nearest} pictures of a screen are in this run or the nearest good run and not in the other (see {mine})', None
    return None, f'{leg}: pictures like a good run\'s'


def bless(t):
    """The pictures the last run kept become a good run: small copies. To start a game's good runs over, delete its good_shots folder."""
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QImage
    stamp, n = time.strftime('%m%d_%H%M%S'), 0
    for f in glob.glob(f'{kept(t, "check_shots")}/*/*.png'):
        to = f'{kept(t, "good_shots")}/{os.path.basename(os.path.dirname(f))}/{stamp}'
        os.makedirs(to, exist_ok=True)
        n += QImage(f).scaled(320, 180, Qt.IgnoreAspectRatio, Qt.SmoothTransformation).save(f'{to}/{os.path.basename(f)}')
    return f'{n} pictures kept as a good run'


def until(lines, seconds):
    """a route's lines up to that many seconds in: each takes its frames and a few released ones (services/hid.cpp)"""
    out, frames = [], 0
    for line in lines:
        n = line.split()[-1]
        frames += (int(n) if n.isdigit() else 1) + 4
        if frames > seconds * 60:
            break
        out.append(line)
    return out


def save_files(t):
    got = {}
    base = t.player + '/save'
    for d, _, names in os.walk(base):
        for n in names:
            got[os.path.relpath(os.path.join(d, n), base)] = hashlib.md5(open(os.path.join(d, n), 'rb').read()).hexdigest()[:8]
    return got


def judge(t, seconds, flags, measured=False):
    faults, notes = [], []
    d = t.common
    shutil.rmtree(d, ignore_errors=True)
    shutil.rmtree(t.player, ignore_errors=True)
    # a run stopped while Cemu wrote its shader cache leaves a file that crashes the next start
    for f in glob.glob(f'{CEMU}/portable/shaderCache/transferable/00050000{t.low}_*'):
        os.remove(f)
    os.makedirs(d)
    route = ROUTES.get(t.route or t.name, GENERIC)
    for flag in ['cemu'] + flags:  # cemu: the runtime's word that it isn't a console running it (gx2_backend.cpp); name=text for a file with something in it
        with open(f'{d}/{flag.split("=")[0]}', 'w') as f:
            f.write(flag.partition('=')[2])
    if t.env:
        with open(d + '/env.txt', 'w', newline='\n') as f:
            f.write(''.join(f'{k}={v}\n' for k, v in t.env.items()))
    log = d + '/log.txt'
    interp = t.interp
    # a scene is its level alone: a restart would stop before the route is back in it
    for name, secs in (('blank save', seconds), ('restart', max(40, seconds // 4)))[:1 if t.env else 2]:
        read_log = lambda: open(log, 'rb').read().decode('utf-8', 'replace') if os.path.exists(log) else ''
        # The restart ends as a player ends a game, quit from its menu (a route's KEY quit), not killed: closing is where consoles have hung
        closing = name == 'restart'
        with open(d + '/live.txt', 'w', newline='\n') as f:
            f.write('\n'.join(until(route, secs - 15) + ['KEY quit'] if closing else route) + '\n')
        saved_before = bool(save_files(t))
        said = cemu(t, secs, interp)
        text = read_log()
        if 'exited by itself' in said and not text.strip():
            # Cemu died before the game wrote a line: one of its own caches for the title, left damaged by the run before, which can happen even
            # when that run closed as asked (NSMB2's restart, 2026-11-11). cemu_run.ps1 has dropped them, so the same again starts clean.
            notes.append(f'{name}: Cemu died as it started; run again on fresh caches')
            said = cemu(t, secs, interp)
            text = read_log()
        elif not text.strip() and not glob.glob(t.data + '/shots/cemu/tv_*.png'):
            # Or it stayed up and never started the game: no line on the game's log and not one picture (A Link Between Worlds' restart,
            # 2026-11-11, right after Kirby's died the other way). The same caches, which cemu_run.ps1 only drops when Cemu exits or won't close.
            notes.append(f'{name}: Cemu never started the game; run again on fresh caches')
            for f in glob.glob(f'{CEMU}/portable/shaderCache/transferable/00050000{t.low}_*'):
                os.remove(f)
            said = cemu(t, secs, interp)
            text = read_log()
        if 'exited by itself' in said and not interp and not re.search(CRASH, text):
            # Cemu's recompiler can die on a game's code, at the start or minutes in, with nothing wrong on the game's own log (3D Land, Paper
            # Jam): its interpreter runs them. A crash of the game's would come back in the interpreter too.
            notes.append(f"{name}: Cemu's recompiler died with nothing on the game's log; run with its interpreter")
            interp = True
            said = cemu(t, secs * 2, interp)
            text = read_log()
        if 'exited by itself' in said:
            faults.append(f'{name}: Cemu stopped: ' + said.strip()[-120:])
        if text:  # each leg's log is kept: the next leg starts the game's own log over
            os.makedirs(kept(t, 'check_logs'), exist_ok=True)
            with open(f'{kept(t, "check_logs")}/{name.replace(" ", "_")}.txt', 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        if closing and '[quit] sound system shut down' not in text:
            faults.append(f'{name}: the game did not close when it was quit')
        for k, v in t.env.items():
            if f'[test] {k} is {v}' not in text or (k == 'RECOMP_ROMFS_SWAP' and '[fs] romfs swap' not in text):
                faults.append(f'{name}: {k} did not take: this is not the scene')
        for pat, what in ((CRASH, 'crash'), (r'\[lazy\] no code', 'code the translation missed')):
            n = len(re.findall(pat, text))
            if n:
                faults.append(f'{name}: {what} ({n}): ' + next(l for l in text.split('\n') if re.search(pat, l))[:120])
        if 'every file call goes straight' not in text:
            faults.append(f'{name}: files not direct: ' + ' / '.join(l[:90] for l in text.split('\n') if '[fs] ' in l)[:200])
        # A vertex shader the pack lacks is compiled by the console, and its draws are the CPU's until it's there: a fault only if it never came
        lacked, came = len(re.findall(r'no compiled vertex shader', text)), len(re.findall(r'\[glsl\] vertex [0-9a-f]+ compiled', text))
        if lacked > came:
            faults.append(f'{name}: {lacked - came} vertex shaders the pack lacks and the console did not compile')
        elif lacked:
            notes.append(f'{name}: {lacked} vertex shaders the pack lacked, compiled by the console')
        # A vertex attribute in a layout Cemu has no decoder for is left out, with nothing on the game's log, and the pictures then show what no
        # console draws: NSMB2's world map without its ground, Superstar Saga's backgrounds as stripes (2026-11-11). Cemu's own log says so.
        with open(CEMU + '/portable/log.txt', encoding='utf-8', errors='replace') as f:
            undecoded = sorted(set(re.findall(r'Unsupported fmt \w+ nfa \d+ signed \d+ endian \d+', f.read())))
        if undecoded:
            faults.append(f"{name}: Cemu can't decode vertex data this game drew with, so these aren't a console's pictures: " + '; '.join(undecoded))
        gaps = sum(int(x) for x in re.findall(r'\[audio\] (\d+) gaps', text))
        if gaps:
            faults.append(f'{name}: {gaps} gaps in the sound')
        flushes = re.findall(r'\[save\] flushed: (-?\d+)', text)
        if any(x != '0' for x in flushes):
            faults.append(f'{name}: save flush failed')
        formats = len(re.findall(r'save: formatting', text))
        if name == 'restart' and formats and saved_before:  # a game that saved nothing on its route formats again, rightly
            faults.append(f'restart: formatted the save again, {formats} times')
        made = re.findall(r'\[glsl\] (?:pixel|vertex) [0-9a-f]+ compiled in (\d+) ms', text)
        bad = re.findall(r'\[glsl\] (?:pixel|vertex) [0-9a-f]+ failed', text)
        if 'compiler' not in text or '[glsl] off' in text:
            faults.append(f'{name}: the console compiler is not running')
        if bad:
            faults.append(f'{name}: the console compiler failed {len(bad)} times')
        if made:
            ms = sorted(int(x) for x in made)
            notes.append(f'{name}: console compiler {len(made)} shaders, {ms[len(ms) // 2]} ms each, slowest {ms[-1]} ms')
        fps = [float(x) for x in re.findall(r'\[gx2\] ([0-9.]+) fps', text)]
        ps = re.findall(r'(\d+) fragment misses', text)
        cpu = re.findall(r'(\d+) on CPU', text)
        if fps:
            s = sorted(fps)
            notes.append(f'{name}: {s[len(s) // 2]:.0f} fps (low {s[0]:.0f}), {ps[-1] if ps else 0} fragment misses, {sum(map(int, cpu))} draws shaded by the CPU, {len(flushes)} flushes')
        elif measured:
            faults.append(f'{name}: nothing drawn')
        fault, note = pictures(t, name.replace(' ', '_'))
        if fault:
            faults.append(fault)
        if note:
            notes.append(note)
        misses = d + '/shader_misses.bin'
        if os.path.exists(misses) and os.path.getsize(misses):
            os.makedirs(t.data + '/shader_logs', exist_ok=True)
            run(sys.executable, 'tools/misses_to_log.py', misses, f'{t.data}/shader_logs/cemu_{time.strftime("%m%d_%H%M%S")}.bin')
            os.remove(misses)
    if os.path.isdir(d + '/save'):
        faults.append("the game saved in common storage, not the player's")
    elif not save_files(t):
        notes.append('no save written on this route')
    return faults, notes


def main():
    global SHOTS
    p = argparse.ArgumentParser()
    p.add_argument('games', nargs='*', default=['all'])
    p.add_argument('--no-build', action='store_true')
    p.add_argument('--seconds', type=int, default=200)
    p.add_argument('--flag', action='append', default=[], help='a debug file in the data folder, like top1080, or with what it holds: "gxtrace=3000 400"')
    p.add_argument('--route', help="a route of test_routes.py for every game named, instead of its own: nsmb2_stand to time a level's first screen")
    p.add_argument('--bless', action='store_true', help="runs nothing: the pictures the last run kept become a good run, once they've been looked at")
    p.add_argument('--measure', action='store_true', help="the measuring kind of build (build3ds.py's THREEDII_MEASURE): frame rates and costs on the log, and the scenes")
    p.add_argument('--shots', type=int, default=6, help='pictures of each screen a leg; more than the usual six are for looking at, and fail the comparison with a good run')
    args = p.parse_args()
    SHOTS = args.shots
    if args.measure:
        global MEASURE
        MEASURE = True
        os.environ['THREEDII_MEASURE'] = '1'  # the builds this run starts inherit it
    src = sources()
    games = list(GAMES) + (list(SCENES) if args.measure else []) + list(src['games']) if args.games == ['all'] else args.games
    built = args.no_build or args.bless
    bad = 0
    ports_built = set()
    for game in games:
        started = time.time()
        if game in SCENES and not args.measure:
            t = 'a scene needs --measure: its switches reach the game through the measuring build alone'
        elif game in GAMES or game in SCENES:
            of, env = SCENES.get(game, (game, {}))
            t = port(of) if built or of in ports_built else build_port(of)  # a scene's port as this run has built it already
            if not isinstance(t, str):
                ports_built.add(of)
                t.name, t.route, t.env = game, of if env else '', env
        elif game in src['games']:
            t = (built_from_dump(game) or 'not built yet') if built else build_from_dump(game, src['games'][game], src['otp'])
        else:
            t = f'no such game: not one of the ports and not in {SOURCES}'
        if not isinstance(t, str) and game in src['games']:
            t.route = src['games'][game].get('route', '')
        if args.route and not isinstance(t, str):
            t.route = args.route
        if args.bless and not isinstance(t, str):
            print(f'{game}: {bless(t)}', flush=True)
            continue
        faults, notes = ([t], []) if isinstance(t, str) else judge(t, args.seconds, args.flag, args.measure)
        bad += bool(faults)
        print(f'{"FAIL" if faults else "ok  "} {game} ({int(time.time() - started)} s): ' + '; '.join(notes), flush=True)
        for f in faults:
            print('     ' + f, flush=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
