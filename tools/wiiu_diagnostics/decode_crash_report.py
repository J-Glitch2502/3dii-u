#!/usr/bin/env python3
"""Decode a 3DSAFE1 console report, including version 2 native stack records."""
import argparse
import json
from pathlib import Path
import struct


def decode(data):
    if len(data) < 320 or data[:7] != b'3DSAFE1':
        raise ValueError('Not a complete 3DSAFE1 report')
    words = struct.unpack_from('>80I', data)
    version, kind, count, stride, size = words[2], words[3], words[12], words[72], words[73]
    if version not in (1, 2) or count > 5 or stride != 800 or size != len(data):
        raise ValueError('Unsupported or truncated report layout')
    end = 320 + count * stride
    if end > size:
        raise ValueError('Truncated contexts')
    text = lambda a, b: data[a:b].split(b'\0', 1)[0].decode('utf-8', 'replace')
    result = dict(version=version, kind=kind, program_id=f'{words[4]:08x}{words[5]:08x}',
                  frame=words[8] << 32 | words[9], build=text(64, 128), reason=text(128, 288), contexts=[])
    for i in range(count):
        base = 320 + i * stride
        word = lambda offset: struct.unpack_from('>I', data, base + offset)[0]
        result['contexts'].append(dict(pc=f'{word(0x98):08x}', lr=f'{word(0x8c):08x}',
                                      dar=f'{word(0xa4):08x}', sp=f'{word(0xc):08x}',
                                      gpr=[f'{word(8 + r * 4):08x}' for r in range(32)]))
    if version == 2 and kind == 3:
        if end + 16 > size:
            raise ValueError('Missing native stack header')
        tag, n, low, high = struct.unpack_from('>4I', data, end)
        if tag != 0x53544b31 or n > 32 or end + 16 + n * 12 != size:
            raise ValueError('Invalid native stack section')
        result['native_stack'] = dict(low=f'{low:08x}', high=f'{high:08x}', frames=[
            dict(zip(('sp', 'next', 'lr'), (f'{v:08x}' for v in struct.unpack_from('>3I', data, end + 16 + j * 12))))
            for j in range(n)])
    elif end != size:
        raise ValueError('Unexpected trailing report data')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.report.read_bytes()), indent=2))
