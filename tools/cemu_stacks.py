#!/usr/bin/env python3
"""Prints every guest thread's PC, LR and call stack from a running Cemu, through its GDB stub (Cemu.exe --enable-gdbstub).
Usage: cemu_stacks.py [nm file] [port]. With an nm listing (powerpc-eabi-nm -n game.elf), addresses are named."""
import bisect
import socket
import struct
import sys
import time

nm_path = sys.argv[1] if len(sys.argv) > 1 else None
port = int(sys.argv[2]) if len(sys.argv) > 2 else 1337
addrs, names = [], []
if nm_path:
    for line in open(nm_path, encoding='utf-8', errors='replace'):
        f = line.split()
        if len(f) == 3 and f[1] in 'TtWw':
            addrs.append(int(f[0], 16))
            names.append(f[2])


def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0 or a - addrs[i] > 0x100000:
        return f'{a:08x}'
    return f'{a:08x} {names[i]}+0x{a - addrs[i]:x}'


s = socket.create_connection(('127.0.0.1', port), timeout=10)
buf = b''


def recv_packet():
    global buf
    while True:
        i = buf.find(b'$')
        j = buf.find(b'#', i)
        if i >= 0 and j >= 0 and len(buf) >= j + 3:
            data = buf[i + 1:j]
            buf = buf[j + 3:]
            s.sendall(b'+')
            return data.decode('latin1')
        d = s.recv(65536)
        if not d:
            raise EOFError
        buf += d


def cmd(text):
    pkt = text.encode()
    s.sendall(b'$' + pkt + b'#' + b'%02x' % (sum(pkt) & 255))
    return recv_packet()


# The stub stops the game at its entry point: let it run, then interrupt it.
run_seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 8
print('why:', cmd('?'))
s.sendall(b'$c#63')
time.sleep(run_seconds)
s.sendall(b'\x03')
s.settimeout(5)
try:
    print('stop:', recv_packet())
except socket.timeout:
    print('no stop reply')
threads = []
r = cmd('qfThreadInfo')
while r.startswith('m'):
    threads += r[1:].split(',')
    r = cmd('qsThreadInfo')
print('threads:', len(threads))
for t in threads:
    cmd('Hg' + t)
    regs = cmd('g')
    words = [int(regs[i:i + 8], 16) for i in range(0, 256, 8)]
    # Cemu's register numbers: r0-r31, f0-f31 (read separately), then pc msr cr lr ctr xer. Probe both layouts.
    def reg(n):
        x = cmd('p%x' % n)
        return int(x, 16) if len(x) == 8 and 'x' not in x and not x.startswith('E') else None
    specials = [reg(0x40 + i) for i in range(6)]
    if specials[0] is None:
        specials = [reg(0x20 + i) for i in range(6)]
    if specials[0] is None:
        print(t, 'no pc')
        continue
    r1, pc, lr = words[1], specials[0], specials[3] or 0
    print(f'--- thread {t}: pc {name(pc)}  lr {name(lr)}  sp {r1:08x}')
    sp = r1
    for _ in range(24):
        m = cmd(f'm{sp:x},8')
        if len(m) != 16 or m.startswith('E'):
            break
        back, ret = struct.unpack('>II', bytes.fromhex(m))
        if not back or back <= sp:
            break
        m = cmd(f'm{back + 4:x},4')
        if len(m) != 8:
            break
        print('    ', name(int(m, 16)))
        sp = back
s.close()
