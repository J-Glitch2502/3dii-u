"""Self-check for shaderlogs.py's round trip: python builder/test_shaderlogs.py"""
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderlogs  # noqa: E402


def vs(entry, code, swz, tail):
    return b'VS  ' + struct.pack('<III', entry, len(code), len(swz)) + struct.pack(f'<{len(code)}I', *code) + struct.pack(f'<{len(swz)}I', *swz) + tail


prog = ([0x11, 0x22, 0x88000000], [0xA, 0xB])  # a program of the game's DVLB
other = ([0x33, 0x44], [0xC])  # one the DVLB scan never found
tail = bytes(range(56))
frag = struct.pack('<I', 3) + b'key' + b'FS  ' + struct.pack('<I', 2) + b'xy'
log = vs(1, prog[0] + [0x99, 0x99], prog[1] + [0x77], tail) + frag + vs(0, *other, tail)  # memory: the program, then another's leftovers

base = tempfile.mkdtemp()
try:
    shipped, vsf, left = shaderlogs.split(log, {(struct.pack('<3I', *prog[0]), struct.pack('<2I', *prog[1]))})
    assert shipped == frag, 'fragment records ship as they are'
    assert left == 1 and len(vsf) == shaderlogs.VSF and b'\x11\x00\x00\x00' not in vsf, 'one fingerprint, no code; the unknown program left out'
    with open(os.path.join(base, 'dvlb.bin'), 'wb') as f:
        f.write(vs(1, *prog, bytes(56)))
    with open(os.path.join(base, 'run.vsf'), 'wb') as f:
        f.write(vsf)
    assert shaderlogs.restore(base) == (1, 0)
    with open(os.path.join(base, 'run_vsf.bin'), 'rb') as f:
        assert f.read() == vs(1, *prog, tail), 'the record comes back with the dump\'s program and its own uniforms and output map'
    assert not os.path.exists(os.path.join(base, 'run.vsf'))
    print('shaderlogs ok')
finally:
    shutil.rmtree(base)
