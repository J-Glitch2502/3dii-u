#!/usr/bin/env python3
"""Asset geometry discovery preserves shared code and respects DVLE stage/entry."""
import struct
import tempfile
from pathlib import Path
from unittest.mock import patch
import dvlb_to_log as d
from pica_to_c import records

def blob(kind=1, entry=600):
    nc = 602
    # DVLB offsets, DVLP shared instructions/swizzles, then one DVLE.
    dv = 40 + nc * 4 + 8
    b = bytearray(dv + 64)
    b[:4] = b'DVLB'
    struct.pack_into('<II', b, 4, 1, dv)
    b[12:16] = b'DVLP'
    struct.pack_into('<4I', b, 20, 28, nc, 28 + nc * 4, 1)
    struct.pack_into('<I', b, 40 + entry * 4 if entry < nc else 40, 0x22 << 26)
    b[dv:dv+4] = b'DVLE'
    b[dv+6] = kind
    struct.pack_into('<I', b, dv+8, entry)
    return bytes(b)

valid = blob()
rs = list(d.geometry_records([valid, valid, blob(0), blob(1, 700), valid[:50], b'bad']))
assert len(rs) == 1
entry, code, swz = next(records(rs[0]))
assert entry == 600 and len(code) == 602 and code[600] == 0x22 << 26
assert len(swz) == 1
with tempfile.TemporaryDirectory() as tmp:
    g = Path(tmp)
    (g/'romfs.bin').touch(); (g/'update_romfs.bin').touch()
    (g/'shader_logs').mkdir()
    user = g/'shader_logs/user.gsl'; user.write_bytes(b'preserve')
    calls = []
    def programs(game, image, include_code):
        calls.append((image, include_code))
        yield valid
    with patch.object(d, 'programs', programs):
        d.discover_geometry(tmp)
    assert calls == [('romfs.bin', True), ('update_romfs.bin', False)]
    assert user.read_bytes() == b'preserve'
    assert (g/'shader_logs/assets_geometry.gsl').read_bytes() == rs[0]
    with patch.object(d, 'programs', return_value=iter([])):
        d.discover_geometry(tmp)
    assert not (g/'shader_logs/assets_geometry.gsl').read_bytes()
print('Geometry discovery: stage, high entry, deduplication, update, malformed and stale-output checks passed')
