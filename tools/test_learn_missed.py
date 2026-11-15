#!/usr/bin/env python3
"""check_ports.learn_missed against made-up files in a temporary folder: test_learn_missed.py"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as cp  # noqa: E402

d = tempfile.mkdtemp().replace('\\', '/')
cp.ROOT, cp.CEMU = d, d + '/cemu'
t = cp.Target('made_up', 'abcd0001', d + '/builder_work/g_test_v1/out', d + '/games/g_test')
os.makedirs(t.player + '/save')
with open(t.player + '/save/missing_functions.txt', 'w') as f:
    f.write('0x00123456\n0x00000010\n0x00123456\n')
assert cp.learn_missed(t) == 1, 'one entry: the address in the first megabyte is no code'
assert open(d + '/games/g_test_v1/functions.txt').read() == '0x00123456\n'
with open(t.player + '/save/missing_functions.txt', 'a') as f:
    f.write('0x00200000\n')
assert cp.learn_missed(t) == 1 and open(d + '/games/g_test_v1/functions.txt').read() == '0x00123456\n0x00200000\n'
assert cp.learn_missed(t) == 0, 'nothing new the second time'
cold = cp.Target('made_up', 'abcd0001', d + '/builder_work/g_test_v1_cold/out', d + '/games/g_test', like='made_up')
assert cp.learn_missed(cold) == 0 and not os.path.exists(d + '/games/g_test_v1_cold'), 'a first-build run adds nothing'
port = cp.Target('made_up', 'abcd0001', d + '/build_mario/x', d + '/games/x')
assert cp.learn_missed(port) == 0, 'a port keeps its own list'
shutil.rmtree(d)
print('learn_missed: ok')
