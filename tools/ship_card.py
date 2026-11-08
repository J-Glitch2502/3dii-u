#!/usr/bin/env python3
"""Packs titles into WUPs and puts them on the SD card, each copy compared with the original: ship_card.py <otp.bin> <card, like D:> [game ... | all]. The local copy goes once the card's is right.
The packer holds a whole game in memory, so each pack waits for the PC to have memory to spare. Its exit code is what counts: a failed one can leave a title.tmd behind.
Waits up to 12 hours for the card if it isn't in. WUP Installer's Install all takes everything in install/. The build it replaces goes to dist/previous on this PC.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
FOLDERS = {'sticker_star': 'Paper_Mario_Sticker_Star', 'nsmb2': 'New_Super_Mario_Bros_2', 'dream_team': 'Mario_Luigi_Dream_Team', 'paper_jam': 'Mario_Luigi_Paper_Jam',
           'superstar_saga': 'Mario_Luigi_Superstar_Saga', 'bowsers_inside': 'Mario_Luigi_Bowsers_Inside_Story', 'layton_azran': 'Layton_Azran_Legacy', 'layton_mask': 'Layton_Miracle_Mask',
           'tomodachi': 'Tomodachi_Life'}
OUT = {'layton_mask': 'build_layton/layton_mask', 'layton_azran': 'build_layton/layton_azran', 'tomodachi': 'build'}
FREE_MB = 1500
PREVIOUS = ROOT + '/dist/previous'  # the build each title had on the card before this one


def free_mb():
    r = subprocess.run(['powershell', '-NoProfile', '-Command', '[int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1KB)'], capture_output=True, text=True)
    return int(r.stdout.strip() or 0)


def pack(otp, game):
    target = f'{ROOT}/dist/{FOLDERS[game]}'
    title = f'{ROOT}/{OUT.get(game, "build_mario/" + game)}/title'
    for attempt in range(3):
        end = time.time() + 4 * 3600
        while free_mb() < FREE_MB and time.time() < end:
            time.sleep(60)
        shutil.rmtree(target, ignore_errors=True)
        r = subprocess.run([sys.executable, ROOT + '/tools/pack_game_wup.py', otp, title, target], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        if r.returncode == 0:
            return target
        print(f'{game}: pack try {attempt + 1} failed: {r.stdout[-200:]}', flush=True)
        shutil.rmtree(target, ignore_errors=True)
        time.sleep(120)
    return None


def copy(source, card):
    name = os.path.basename(source)
    target = f'{card}/install/{name}'
    # what was there is kept on this PC, compared, and leaves the card, so the card only ever holds one build of each (the user's rule, 2026-11-07): a build that
    # disappoints can go back without a rebuild
    if os.path.isdir(target):
        kept = f'{PREVIOUS}/{name}'
        shutil.rmtree(kept, ignore_errors=True)
        shutil.copytree(target, kept)
        if any(filecmp.cmpfiles(target, kept, os.listdir(target), shallow=False)[1:]):
            return f'{name}: SAVING THE PREVIOUS BUILD DIFFERS, the card is as it was'
        shutil.rmtree(target)
    os.makedirs(target)
    names = os.listdir(source)
    for n in names:
        shutil.copyfile(f'{source}/{n}', f'{target}/{n}')
    mismatch = filecmp.cmpfiles(source, target, names, shallow=False)[1:]
    if any(mismatch):
        return f'{name}: COPY DIFFERS {mismatch}'
    shutil.rmtree(source)
    return f'{name}: on the card, compared, {len(names)} files'


def main():
    otp, card, *games = sys.argv[1:]
    card = card.rstrip('/\\')
    games = list(FOLDERS) if games in ([], ['all']) else games
    for _ in range(2160):
        if os.path.isdir(card + '/install'):
            break
        time.sleep(20)
    else:
        sys.exit('no card after 12 hours')
    bad = 0
    for game in games:
        packed = pack(otp, game)
        said = copy(packed, card) if packed else f'{game}: PACK FAILED'
        bad += 'FAILED' in said or 'DIFFERS' in said
        print(said, flush=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
