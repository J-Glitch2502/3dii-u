"""Fail a build if any supplied fragment state has no compiled lookup.

Counts alone are insufficient: canonical aliases may share a program. Check
each state's canonical key as well as its exact key, and retain an audit report.
"""
import json
from pathlib import Path
import struct
from tev_to_log import records, decode, encode, normal


def fnv64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def check(logs, output, pipelines=None):
    output = Path(output)
    data = (output / 'shaders.idx').read_bytes()
    if len(data) < 32:
        raise ValueError('Truncated shader index')
    header = struct.unpack_from('>8I', data)
    np, nv = header[2:4]
    if header[:2] != (0x53484458, 1) or len(data) != 32 + 16 * (np + nv):
        raise ValueError('Invalid shader index')
    entries = [struct.unpack_from('>QII', data, 32 + 16 * i) for i in range(np)]
    names = (output / 'glsl/list.txt').read_text().split()
    programs = sum(name.endswith('.frag') for name in names)
    if any(index >= programs for _, index, _ in entries):
        raise ValueError('Fragment lookup points outside the generated program list')
    keys = {key for key, _, _ in entries}
    if len(keys) != len(entries):
        raise ValueError('Duplicate fragment lookup keys')
    states = set()
    inputs = {}
    for path in sorted(Path(logs).glob('*.bin')):
        found = records(str(path))
        inputs[path.name] = len(found)
        states.update(found)
    if pipelines:
        raw = Path(pipelines).read_bytes()
        at = 8
        while at < len(raw):
            if at + 4 > len(raw):
                raise ValueError('Truncated pipeline fragment state')
            size = struct.unpack_from('<I', raw, at)[0]
            if size < 4 or size > 4096 or size % 4 or at + 4 + size > len(raw):
                raise ValueError('Invalid pipeline fragment state')
            states.add(raw[at + 4:at + 4 + size])
            at += 4 + size
    missing = [f'{fnv64(state):016x}' for state in sorted(states)
               if fnv64(state) not in keys and fnv64(encode(normal(decode(state)))) not in keys]
    report = dict(version=1, input_files=inputs, distinct_input_states=len(states),
                  fragment_lookup_keys=np, fragment_programs=programs, missing=missing,
                  scope='Input-state lookup coverage, not proof of exhaustive gameplay or visual/performance equivalence')
    (output / 'shader-coverage-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if missing:
        raise ValueError(f'Shader coverage regression: {len(missing)} input states lack a compiled lookup; see shader-coverage-report.json')
    print(f'Shader coverage: all {len(states)} input fragment states have compiled lookups', flush=True)
    return report
