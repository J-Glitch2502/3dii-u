#!/usr/bin/env python3
"""check_ports.mark_built against made-up work folders in a temporary folder: test_mark_built.py"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as cp  # noqa: E402

d = tempfile.mkdtemp().replace('\\', '/')
cp.ROOT = d
for slug, program in (('g_one_v1', '0004000000111100'), ('g_two_v1', '0004000000222200'), ('g_old', '0004000000111100')):
    os.makedirs(f'{d}/builder_work/{slug}/out')
    with open(f'{d}/builder_work/{slug}/info.json', 'w') as f:
        f.write('{"program_id": "%s"}' % program)
os.utime(d + '/builder_work/g_old/out', (1000, 1000))
name = lambda slug: open(f'{d}/builder_work/{slug}/check_name.txt').read() if os.path.exists(f'{d}/builder_work/{slug}/check_name.txt') else None
# two games built side by side: both folders are newer than either build's start
cp.mark_built('one', 2000, 'Game One (0004000000111100): Not tested, update v1\n@stage Decrypting\n')
assert (name('g_one_v1'), name('g_two_v1'), name('g_old')) == ('one', None, None), "only its own game's folder, and not one from an older build"
cp.mark_built('two', 2000, 'Game Two (0004000000222200): Not tested\n')
assert (name('g_one_v1'), name('g_two_v1')) == ('one', 'two'), "the other build leaves the first one's mark"
cp.mark_built('three', 2000, 'no title line: the build failed before it\n')
assert (name('g_one_v1'), name('g_two_v1')) == ('one', 'two'), 'nothing marked without the game named'
shutil.rmtree(d)
print('mark_built: ok')
