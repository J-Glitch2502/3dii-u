#!/usr/bin/env python3
"""Writes the info.json the recompiler reads, from the guest ELF's section symbols: mkguest.py <workload.elf> <outdir>"""
import json
import os
import subprocess
import sys

elf, out = sys.argv[1:3]
syms = {}
for line in subprocess.check_output(['arm-none-eabi-nm', elf], text=True).splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16)


def code_set(name):
    start, end = syms[f'__{name}_start'], syms[f'__{name}_end']
    assert start & 0xfff == 0, f'{name} must start on a page'
    return {'addr': start, 'size': end - start}


info = {
    'program_id': '0004000000F00000',  # homebrew range: this is the benchmark, not a title
    'text': code_set('text'),
    'rodata': code_set('rodata'),
    'data': code_set('data'),
    'bss': syms['__bss_end'] - syms['__bss_start'],
}
assert info['text']['addr'] == syms['guest_main'], 'guest_main must be the first thing in .text'
with open(os.path.join(out, 'info.json'), 'w') as f:
    json.dump(info, f, indent=1)
print(info)
