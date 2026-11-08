#!/usr/bin/env python3
"""Debug aid: rebuilds one generated C file with C statements inserted before a guest instruction, into the PC game objects.
dbg_inject.py <gen dir> <file.c> <hex addr> <C statements> [<hex addr> <C statements> ...]
Run with no injections to restore the file's object. Inside the statements, guest registers are the local variables r0..r14."""
import os
import subprocess
import sys

gen, name = sys.argv[1], sys.argv[2]
pairs = sys.argv[3:]
src = open(os.path.join(gen, name), encoding='utf-8').read().split('\n')
for addr, code in zip(pairs[0::2], pairs[1::2]):
    mark = f'/* {addr.lower()}:'
    hits = [i for i, line in enumerate(src) if mark in line]
    assert hits, f'no instruction at {addr}'
    for i in hits:
        src[i] = '{ extern int dprintf(int, const char *, ...); ' + code + ' } ' + src[i]
os.makedirs('/root/dbg', exist_ok=True)
out = os.path.join('/root/dbg', name)
open(out, 'w', encoding='utf-8').write('\n'.join(src))
subprocess.check_call(['clang-18', '-O1', '-w', '-ffp-contract=off', '-fno-strict-aliasing', '-std=gnu11',
                       '-I/project/third_party/3dsrecomp/runtime', '-I' + gen, '-c', out,
                       '-o', '/root/pc_game/' + name.replace('.c', '.o')])
os.remove(out)
