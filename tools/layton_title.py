#!/usr/bin/env python3
"""Assembles one Layton game's title folder under build_layton/<game>/title, for Cemu and the WUP packer, by running
make_game_title.py with this game's names. Run with Windows Python: layton_title.py <layton_azran|layton_mask>.
Title ids stay clear of the benchmark, Tomodachi, the loader tests (0005000010334455-66) and the Mario games (71-80, 83, 84).
81 and 82 went on the console first. These ids stay from now on: the user uninstalls the old build before installing a new one, as reinstalling over a title is a suspect for hangs at its splash."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = {
    'layton_azran': ('0005000010334485', 'Professor Layton and the Azran Legacy', 'WUP-N-LTAL'),
    'layton_mask': ('0005000010334486', 'Professor Layton and the Miracle Mask', 'WUP-N-LTMM'),
}
game = sys.argv[1]
title_id, name, product = GAMES[game]
source = open(os.path.join(ROOT, 'tools', 'make_game_title.py'), encoding='utf-8', newline='').read()
for pattern, replacement in [
    (r"^TITLE = .*$", f"TITLE = os.path.join(ROOT, 'build_layton', {game!r}, 'title')"),
    (r"^GAME = .*$", f"GAME = os.path.join(ROOT, 'games', {game!r})"),
    (r"^TITLE_ID = .*$", f"TITLE_ID = {title_id!r}"),
    (r"^RPX_NAME = .*$", f"RPX_NAME = {game + '.rpx'!r}"),
    (r"^rpx = .*$", f"rpx = os.path.join(ROOT, 'build_layton', {game!r}, {game + '.rpx'!r})"),
    (r"\.replace\('3DS Recomp Code Test', '[^']*'\)", f".replace('3DS Recomp Code Test', {name!r})"),
    (r"'WUP-N-[A-Z0-9]{4}'", repr(product)),
    (r"os\.path\.join\(ROOT, 'build', 'shaders', f\)", f"os.path.join(ROOT, 'build_layton', {game!r}, 'shaders', f)"),
]:
    source, count = re.subn(pattern, lambda m: replacement, source, count=1, flags=re.M)
    if count != 1:
        sys.exit(f'make_game_title.py has changed: no match for {pattern}')
exec(compile(source, 'make_game_title.py', 'exec'), {'__file__': os.path.join(ROOT, 'tools', 'make_game_title.py'), '__name__': '__main__'})
