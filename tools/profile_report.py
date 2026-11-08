#!/usr/bin/env python3
"""Where each core of the real console spent its time: profile_report.py <profile.bin or a console log> <game.nm from the same build> [lines per core].
game.nm holds that build's symbols (powerpc-eabi-nm -n). Samples in the system's code count for the function that called it.
A log holds the "[prof] core <n> <address> <samples>" lines the console sends for "prof" (the last set of them is used)."""
import bisect
import re
import struct
import sys
from collections import Counter

data = open(sys.argv[1], 'rb').read()
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
if data[:4] != b'PROF':  # a log: rebuild profile.bin's layout from its last "prof" answer
    per = [{} for _ in range(3)]
    for line in data.decode('utf-8', 'replace').splitlines():
        m = re.search(r'\[prof\] core (\d) ([0-9a-f]{8}) (\d+)', line)
        if m:
            per[int(m.group(1))][int(m.group(2), 16)] = int(m.group(3))
    data = b'PROF' + b''.join(struct.pack('>I', len(d)) + b''.join(struct.pack('>II', k, v) for k, v in d.items()) for d in per)
addrs, names = [], []
for line in open(sys.argv[2], errors='replace'):
    m = re.match(r'([0-9a-f]{8}) [tTwW] (\S+)', line)
    if m:
        addrs.append(int(m.group(1), 16))
        names.append(m.group(2))


def name_of(a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else '?'


assert data[:4] == b'PROF', 'not a profile'
p = 4
for core in range(3):
    n = struct.unpack_from('>I', data, p)[0]
    p += 4
    by_func = Counter()
    for _ in range(n):
        key, count = struct.unpack_from('>II', data, p)
        p += 8
        in_system = key & 1
        code = key & ~1
        if in_system and not 0x02000000 <= code < 0x10000000:
            by_func['(system, idle or waiting)'] += count
        else:
            by_func[name_of(code) + (' (in system calls)' if in_system else '')] += count
    total = sum(by_func.values())
    print(f'--- core {core}: {total} samples')
    for name, count in by_func.most_common(top):
        print(f'{count * 100 / max(total, 1):5.1f}%  {name}')


assert p == len(data), 'trailing data'
