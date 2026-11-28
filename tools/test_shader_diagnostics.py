#!/usr/bin/env python3
"""Synthetic checks: call boundaries, report categories and shareable output."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import struct
from urllib.parse import parse_qs, urlparse

from analyze_pica_shaders import reachable
from shader_diagnostics import write_report
from dvlb_to_log import labels

END, NOP = 0x22 << 26, 0x21 << 26
code = [(0x24 << 26) | (4 << 10) | 2, END, NOP, NOP, NOP, NOP, NOP, END]
sites, calls, bounded = reachable(code, 0)
assert sites == {0, 1, 4, 5} and calls == {(0, 4, 2)} and not bounded
code[0] = (0x26 << 26) | (4 << 10) | 2
assert reachable(code, 0)[0] == {0, 1, 4, 5}
assert reachable([(0x24 << 26) | 1], 0)[2]  # recursive call bounded
branch = [(0x2d << 26) | (14 << 22) | (2 << 10), NOP, END]
assert reachable(branch, 0, 1 << 14)[0] == {0, 2}
assert reachable(branch, 0, 0)[0] == {0, 1, 2}

blob = bytearray(112)
blob[:4] = b'DVLB'
struct.pack_into('<II', blob, 4, 1, 12)
blob[12:16] = b'DVLE'
struct.pack_into('<II', blob, 12+32, 64, 1)
struct.pack_into('<II', blob, 12+56, 80, 20)
struct.pack_into('<4I', blob, 12+64, 0x10000, 74, 31, 0)
blob[92:104] = b'ApplyMorphs\0'
assert list(labels(blob)) == [(0, 74, 31, 'ApplyMorphs')]
try:
    list(labels(blob[:95]))
except ValueError:
    pass
else:
    raise AssertionError('truncated label table accepted')

with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
    folder = Path(temp)
    header = 'stage\tprogram_id\tentry\tvariant\tpc\ttranslator_line\tfeature\n'
    row = 'vertex\t12345\t327\t0\t74\t1500\ttranslation_rejected\n'
    (folder/'shader-diagnostics.tsv').write_text(header + row + row)
    target = write_report(folder, '/private/game')
    report = json.loads(target.read_text())
    assert len(report['failures']) == 1
    issue = (folder/'report-shader-issue.txt').read_text().splitlines()[0]
    query = parse_qs(urlparse(issue).query)
    assert '/private' not in issue and 'game' in query['body'][0]
    assert urlparse(issue).netloc == 'github.com'
    (folder/'shader-diagnostics.tsv').write_text(header)
    assert json.loads(write_report(folder, 'game').read_text())['failures'] == []
    assert not (folder/'report-shader-issue.txt').exists()
    (folder/'shader-diagnostics.tsv').write_text(header + row.replace('vertex', 'made_up'))
    try:
        write_report(folder, 'game')
    except ValueError:
        pass
    else:
        raise AssertionError('unknown stage accepted')
print('Shader diagnostics: call bounds, recursion, deduplication, categories and GitHub template checks passed')
