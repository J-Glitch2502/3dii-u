#!/usr/bin/env python3
"""Names what a PC build's RECOMP_PROFILE samples hit: pc_profile.py <exe> <samples> [top N].
Per thread, then the functions that thread was in: guest code is f_<3DS address>, the runtime is everything else."""
import bisect
import collections
import re
import struct
import subprocess
import sys

NM = 'C:/Program Files/LLVM/bin/llvm-nm.exe'


def image_base(exe):
    """ImageBase and SizeOfImage from the PE32+ optional header"""
    d = open(exe, 'rb').read(4096)
    opt = struct.unpack_from('<I', d, 0x3C)[0] + 24
    return struct.unpack_from('<Q', d, opt + 24)[0], struct.unpack_from('<I', d, opt + 56)[0]


def symbols(exe):
    out = subprocess.run([NM, '--defined-only', '-n', exe], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        m = re.match(r'([0-9a-f]+) [tTwW] (\S+)', line)
        if m:
            addrs.append(int(m.group(1), 16))
            names.append(m.group(2))
    return addrs, names


def main():
    exe, path = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    base, size = image_base(exe)
    addrs, names = symbols(exe)
    raw = open(path, 'rb').read()
    per = collections.defaultdict(collections.Counter)
    for tid, lo, hi in struct.iter_unpack('<III', raw[:len(raw) // 12 * 12]):
        rva = lo | hi << 32
        if rva >= size:  # outside the program: the system, mostly waiting
            per[tid]['(system)'] += 1
            continue
        i = bisect.bisect_right(addrs, base + rva) - 1
        per[tid][names[i] if i >= 0 else '?'] += 1
    for tid, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        total = sum(c.values())
        busy = total - c['(system)']
        guest = sum(n for k, n in c.items() if k.startswith('f_'))
        print(f'thread {tid}: {total} samples, {busy} running ({100 * busy // max(total, 1)}%), {guest} in game code')
        if busy < total // 50:
            continue
        for name, n in c.most_common(top):
            if name != '(system)':
                print(f'  {100 * n / max(busy, 1):5.1f}%  {name}')


if __name__ == '__main__':
    main()
