#!/usr/bin/env python3
"""Assembles one Mario game's title folder under build_mario/<game>/title, for Cemu and the WUP packer, by running make_game_title.py
with this game's names. Run with Windows Python from anywhere: mario_title.py <game>. Title ids stay clear of the benchmark, Tomodachi,
Layton and the loader tests (0005000010334455-66). 71-76 was the first set on the console; these ids replaced them when reinstalling over a title turned out to hang at the
banner in the system's loader. They stay now: the user uninstalls the old build before installing a new one, and the Menu names carry no letter."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = {
    'sticker_star': ('0005000010334477', 'Paper Mario: Sticker Star', 'WUP-N-MPSS'),
    'nsmb2': ('0005000010334478', 'New Super Mario Bros. 2', 'WUP-N-MNS2'),
    'dream_team': ('0005000010334479', 'Mario & Luigi: Dream Team', 'WUP-N-MLDT'),
    'paper_jam': ('0005000010334480', 'Mario & Luigi: Paper Jam', 'WUP-N-MLPJ'),
    'superstar_saga': ('0005000010334483', "Mario & Luigi: Superstar Saga + Bowser's Minions", 'WUP-N-MLSS'),
    'bowsers_inside': ('0005000010334484', "Mario & Luigi: Bowser's Inside Story + Bowser Jr.'s Journey", 'WUP-N-MLBI'),
}
game = sys.argv[1]
title_id, name, product = GAMES[game]
out = ['build_mario', game]
source = open(os.path.join(ROOT, 'tools', 'make_game_title.py'), encoding='utf-8', newline='').read()
for pattern, replacement in [
    (r"^TITLE = .*$", f"TITLE = os.path.join(ROOT, {out[0]!r}, {game!r}, 'title')"),
    (r"^GAME = .*$", f"GAME = os.path.join(ROOT, 'games', {game!r})"),
    (r"^TITLE_ID = .*$", f"TITLE_ID = {title_id!r}"),
    (r"^RPX_NAME = .*$", f"RPX_NAME = {game + '.rpx'!r}"),
    (r"^rpx = .*$", f"rpx = os.path.join(ROOT, {out[0]!r}, {game!r}, {game + '.rpx'!r})"),
    (r"\.replace\('3DS Recomp Code Test', '[^']*'\)", f".replace('3DS Recomp Code Test', {name.replace('&', '&amp;')!r})"),
    (r"'WUP-N-[A-Z0-9]{4}'", repr(product)),
    (r"os\.path\.join\(ROOT, 'build', 'shaders', f\)", f"os.path.join(ROOT, {out[0]!r}, {game!r}, 'shaders', f)"),
]:
    source, count = re.subn(pattern, lambda m: replacement, source, count=1, flags=re.M)
    if count != 1:
        sys.exit(f'make_game_title.py has changed: no match for {pattern}')
exec(compile(source, 'make_game_title.py', 'exec'), {'__file__': os.path.join(ROOT, 'tools', 'make_game_title.py'), '__name__': '__main__'})
