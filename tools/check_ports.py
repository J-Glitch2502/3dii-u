#!/usr/bin/env python3
"""Builds ports and checks each one in Cemu before anything goes near a console: check_ports.py [--no-build] [--seconds N] [game ... | all]
For each game: code, shaders and title; the RPX against what the console's loader wants; then its route from test_routes.py on a blank save, and a restart on what that saved.
A game fails on a crash, code the translation missed, a vertex shader that wasn't compiled, a failed save flush, a save that formats itself twice, a save in common storage
instead of the player's, or files not going straight to the file system. Fragment misses and frame rates are reported, not judged: Cemu has no real GPU. Shaders a run missed go to shader_logs, for the next build. Exit code 0 if all passed.
Cemu is a private copy in build_win/cemu (a renamed exe, so other sessions' scripts leave it alone), run by tools/win/cemu_run.ps1.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from test_routes import ROUTES

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
CEMU = ROOT + '/build_win/cemu'
GAMES = {'sticker_star': '77', 'nsmb2': '78', 'dream_team': '79', 'paper_jam': '80', 'superstar_saga': '83', 'bowsers_inside': '84', 'layton_azran': '85', 'layton_mask': '86', 'tomodachi': '67'}
OUT = {'layton_mask': 'build_layton/layton_mask', 'layton_azran': 'build_layton/layton_azran', 'tomodachi': 'build'}


def folder(game):
    return f'{CEMU}/portable/mlc01/usr/save/00050000/103344{GAMES[game]}/user/common'


def player(game):  # the player's own storage, where the game's saves go (save_storage.cpp): Cemu's first account
    return f'{CEMU}/portable/mlc01/usr/save/00050000/103344{GAMES[game]}/user/80000001'


def run(*cmd):
    return subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')


def build(game):
    r = run(sys.executable, 'tools/win/build3ds.py', 'all', game)
    if r.returncode:
        return 'build failed: ' + r.stdout[-400:]
    out = OUT.get(game, f'build_mario/{game}')
    rpx = f'{out}/title/code/{game}.rpx'
    if not os.path.exists(f'{ROOT}/{rpx}'):
        rpx = f'{out}/title/code/' + next(f for f in os.listdir(f'{ROOT}/{out}/title/code') if f.endswith('.rpx'))
    r = run(sys.executable, 'tools/rpx_check.py', rpx, f'{out}/title/code/cos.xml')
    return None if 'no problems' in r.stdout else 'RPX check: ' + r.stdout[-300:]


def cemu(game, seconds):
    out = OUT.get(game, f'build_mario/{game}')
    code = f'{ROOT}/{out}/title/code'
    rpx = code + '/' + next(f for f in os.listdir(code) if f.endswith('.rpx'))
    ps = ['powershell', '-NoProfile', '-File', ROOT + '/tools/win/cemu_run.ps1', '-Rpx', rpx, '-Low', '103344' + GAMES[game], '-Seconds', str(seconds), '-Out', f'{ROOT}/games/{game}/shots/cemu']
    if game == 'paper_jam':  # Cemu's recompiler can't run this one
        ps.append('-Interpreter')
    return run(*ps).stdout


def save_files(game):
    got = {}
    base = player(game) + '/save'
    for d, _, names in os.walk(base):
        for n in names:
            got[os.path.relpath(os.path.join(d, n), base)] = hashlib.md5(open(os.path.join(d, n), 'rb').read()).hexdigest()[:8]
    return got


def judge(game, seconds, flags):
    faults, notes = [], []
    d = folder(game)
    shutil.rmtree(d, ignore_errors=True)
    shutil.rmtree(player(game), ignore_errors=True)
    os.makedirs(d)
    with open(d + '/live.txt', 'w', newline='\n') as f:
        f.write('\n'.join(ROUTES[game]) + '\n')
    for flag in flags:
        open(f'{d}/{flag}', 'w').close()
    log = d + '/log.txt'
    for name, secs in (('blank save', seconds), ('restart', max(40, seconds // 4))):
        said = cemu(game, secs)
        text = open(log, 'rb').read().decode('utf-8', 'replace') if os.path.exists(log) else ''
        if 'exited by itself' in said:
            faults.append(f'{name}: Cemu stopped: ' + said.strip()[-120:])
        for pat, what in ((r'FATAL|no recompiled code at', 'crash'), (r'\[lazy\] no code', 'code the translation missed')):
            n = len(re.findall(pat, text))
            if n:
                faults.append(f'{name}: {what} ({n}): ' + next(l for l in text.split('\n') if re.search(pat, l))[:120])
        if 'every file call goes straight' not in text:
            faults.append(f'{name}: files not direct: ' + ' / '.join(l[:90] for l in text.split('\n') if '[fs] ' in l)[:200])
        vs = len(re.findall(r'no compiled vertex shader', text))
        if vs:
            faults.append(f'{name}: {vs} vertex shaders not compiled')
        flushes = re.findall(r'\[save\] flushed: (-?\d+)', text)
        if any(x != '0' for x in flushes):
            faults.append(f'{name}: save flush failed')
        formats = len(re.findall(r'save: formatting', text))
        if name == 'restart' and formats:
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
        if fps:
            s = sorted(fps)
            notes.append(f'{name}: {s[len(s) // 2]:.0f} fps (low {s[0]:.0f}), {ps[-1] if ps else 0} fragment misses, {len(flushes)} flushes')
        else:
            faults.append(f'{name}: nothing drawn')
        misses = d + '/shader_misses.bin'
        if os.path.exists(misses) and os.path.getsize(misses):
            run(sys.executable, 'tools/misses_to_log.py', misses, f'games/{game}/shader_logs/cemu_{time.strftime("%m%d_%H%M%S")}.bin')
            os.remove(misses)
    if os.path.isdir(d + '/save'):
        faults.append("the game saved in common storage, not the player's")
    elif not save_files(game):
        notes.append('no save written on this route')
    return faults, notes


def main():
    p = argparse.ArgumentParser()
    p.add_argument('games', nargs='*', default=['all'])
    p.add_argument('--no-build', action='store_true')
    p.add_argument('--seconds', type=int, default=200)
    p.add_argument('--flag', action='append', default=[], help='a debug file in the data folder, like top1080')
    args = p.parse_args()
    games = list(GAMES) if args.games == ['all'] else args.games
    bad = 0
    for game in games:
        started = time.time()
        fault = None if args.no_build else build(game)
        faults, notes = ([fault], []) if fault else judge(game, args.seconds, args.flag)
        bad += bool(faults)
        print(f'{"FAIL" if faults else "ok  "} {game} ({int(time.time() - started)} s): ' + '; '.join(notes), flush=True)
        for f in faults:
            print('     ' + f, flush=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
