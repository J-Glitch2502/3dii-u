#!/usr/bin/env python3
"""find_entries.py's "saves lr" scan, for a game's CRO modules: functions reached only through pointers, which the console can't compile on
the spot, so a miss there is a crash. Run it in the contained environment with the slots the game was generated for.
RECOMP_CRO_BASE=0x1C000000 cro_entries.py <game dir>. Adds what it finds to the game's functions.txt."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'third_party', '3dsrecomp'))
from tools.recomp import cro  # noqa: E402

game = sys.argv[1]
cs = cro.load(game)
found = set()
for m in cs.mods if cs else []:
    for k, (off, size, typ) in enumerate(m.segs):
        if typ != 0 or not size:
            continue
        words = struct.unpack_from(f'<{size // 4}I', m.d, off)
        for i, w in enumerate(words):
            if w & 0xFFFF4000 == 0xE92D4000 or w == 0xE52DE004:  # push {..., lr}, str lr, [sp, #-4]!
                found.add(m.seg_addr[k] + i * 4)
            elif w & 0xFFFF0000 == 0xE92D0000 and i and words[i - 1] & 0xFFFF8000 == 0xE8BD8000:  # any push right after a pop {..., pc}
                found.add(m.seg_addr[k] + i * 4)
path = os.path.join(game, 'functions.txt')
known = set()
if os.path.exists(path):
    known = {int(line, 16) for line in open(path) if line.strip()}
with open(path, 'w', newline='\n') as f:
    for a in sorted(known | found):
        f.write(f'0x{a:08x}\n')
print(f'{len(found)} in {len(cs.mods) if cs else 0} modules, {len(found - known)} new; functions.txt: {len(known | found)} entries')
