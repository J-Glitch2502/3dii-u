#!/usr/bin/env python3
"""Counts the PowerPC instructions each kernel really executes, under qemu, against its ARM instruction count. This predicts
the benchmark without hardware: speed = 1243 MHz * instructions-per-clock / PowerPC instructions per ARM instruction."""
import os
import re
import subprocess

BIN, LOG = 'build/check/bench_ppc', '/tmp/ratio.log'
CLOCK_MHZ, TARGET_MIPS = 1243.0, 536.0
rows = re.findall(r'\{"(\w+)", (\d+)u, (\d+)u,', open('build/host/counts.h').read())


def executed(mode, kernel, iters):
    subprocess.run(['qemu-ppc-static', '-cpu', '750', '-one-insn-per-tb', '-d', 'nochain,exec', '-D', LOG, BIN, '--run', mode,
                    str(kernel), str(iters)], check=True, capture_output=True)
    with open(LOG, 'rb') as f:
        count = sum(chunk.count(b'Trace ') for chunk in iter(lambda: f.read(1 << 20), b''))
    os.remove(LOG)
    return count


totals = {'abi': 0, 'safe': 0}
total_arm = 0
print(f'{"kernel":10} {"ARM/iter":>9} {"abi":>9} {"safe":>9} {"native":>9} {"abi/ARM":>8} {"safe/ARM":>9} {"MIPS@IPC1":>10}')
for k, (name, _, per_iter) in enumerate(rows):
    arm = int(per_iter)
    per = {m: (executed(m, k, 4) - executed(m, k, 2)) / 2 for m in ('abi', 'safe', 'native')}
    total_arm += arm
    totals['abi'] += per['abi']
    totals['safe'] += per['safe']
    print(f'{name:10} {arm:>9} {per["abi"]:>9.0f} {per["safe"]:>9.0f} {per["native"]:>9.0f} {per["abi"] / arm:>8.2f} {per["safe"] / arm:>9.2f} {CLOCK_MHZ / (per["abi"] / arm):>10.0f}')
for variant in ('abi', 'safe'):
    ratio = totals[variant] / total_arm
    print(f'{variant}: {ratio:.2f} PowerPC instructions per ARM instruction')
    for ipc in (0.7, 1.0, 1.3):
        mips = CLOCK_MHZ * ipc / ratio
        print(f'  at {ipc} instructions per clock: {mips:.0f} MIPS, {mips / 268:.2f}x an Old 3DS at 268 MIPS, target {"met" if mips >= TARGET_MIPS else "missed"}')
