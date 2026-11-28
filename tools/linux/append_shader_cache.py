#!/usr/bin/env python3
"""Append observed SHDC v2/v4/v5 shader states to a matched GSH/SHDX pair.

Requires the same trusted compiler/toolchain as the original pack. Output is a
new directory; never mutate a pack used by a running title. No game data ships
with this tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from native_build import load_builder


def records(data):
    if len(data) < 8:
        raise ValueError('Truncated SHDC header')
    magic, revision = struct.unpack_from('>2I', data)
    if magic != 0x53484443 or revision not in (2, 4, 5):
        raise ValueError('Expected SHDC revision 2, 4 or 5')
    at, result = 8, {}
    while at < len(data):
        if len(data) - at < 20: raise ValueError('Truncated cache record header')
        stage, inputs, hi, lo, size = struct.unpack_from('>5I', data, at)
        if stage >> 24 not in (0, 1) or size > 1 << 20 or size > len(data) - at - 20:
            raise ValueError('Invalid or incomplete cache record')
        vertex, key = bool(stage >> 24), hi << 32 | lo
        text = data[at + 20:at + 20 + size].decode('utf-8')
        value = dict(vertex=vertex, key=key, inputs=inputs, glsl=text)
        ident = vertex, key
        if ident in result and result[ident] != value: raise ValueError('Conflicting shader key')
        result[ident] = value
        at += 20 + size
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('cache', type=Path)
    ap.add_argument('--original', type=Path, help='Directory containing the original shaders.gsh and shaders.idx')
    ap.add_argument('--output', type=Path, help='New output directory')
    ap.add_argument('--work', type=Path, default=Path('build_native_shader_cache'))
    ap.add_argument('--compiler', type=Path)
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--inspect', action='store_true', help='Validate and count cache records without compiling')
    a = ap.parse_args()
    if a.jobs < 1: ap.error('--jobs must be positive')
    found = records(a.cache.read_bytes())
    if a.inspect:
        print(json.dumps(dict(records=len(found), vertex=sum(x[0] for x in found), pixel=sum(not x[0] for x in found))))
        return
    if a.original is None or a.output is None: ap.error('--original and --output are required')
    if a.output.exists(): ap.error('Output directory already exists; choose a new immutable release')
    idx = (a.original / 'shaders.idx').read_bytes()
    if len(idx) < 32: raise ValueError('Truncated SHDX header')
    header = list(struct.unpack_from('>8I', idx))
    np, nv = header[2:4]
    if header[:2] != [0x53484458, 1] or len(idx) != 32 + 16 * (np + nv):
        raise ValueError('Invalid SHDX index')
    entries = [struct.unpack_from('>4I', idx, 32 + 16 * i) for i in range(np + nv)]
    ps, vs = entries[:np], entries[np:]
    old = {(False, hi << 32 | lo) for hi, lo, _, _ in ps} | {(True, hi << 32 | lo) for hi, lo, _, _ in vs}
    added = sorted((v for ident, v in found.items() if ident not in old), key=lambda v: (not v['vertex'], v['key']))
    b = load_builder(a.work, a.compiler)
    extra = a.work / 'observed-extra'
    glsl = extra / 'glsl'
    if extra.exists(): raise ValueError('Compiler work directory exists; select a fresh --work')
    glsl.mkdir(parents=True)
    names = []
    for v in added:
        name = f"{v['key']:016x}." + ('vert' if v['vertex'] else 'frag')
        (glsl / name).write_text(v['glsl']); names.append(name)
    (glsl / 'list.txt').write_text('\n'.join(names) + '\n')
    original = (a.original / 'shaders.gsh').read_bytes()
    counts = {3: 0, 6: 0}
    for kind, _, _ in b.gfd_blocks(original):
        if kind in counts: counts[kind] += 1
    a.output.mkdir(parents=True)
    if added:
        b.compile_shaders(str(extra.resolve()), a.jobs)
        b.merge_gfd([str((a.original / 'shaders.gsh').resolve()), str((extra / 'shaders.gsh').resolve())], str(a.output / 'shaders.gsh'))
    else: (a.output / 'shaders.gsh').write_bytes(original)
    newps, newvs = list(ps), list(vs)
    vi, pi = counts[3], counts[6]
    for v in added:
        e = (v['key'] >> 32, v['key'] & 0xffffffff, vi if v['vertex'] else pi, v['inputs'] if v['vertex'] else 0)
        if v['vertex']: newvs.append(e); vi += 1
        else: newps.append(e); pi += 1
    header[2:4] = len(newps), len(newvs)
    (a.output / 'shaders.idx').write_bytes(struct.pack('>8I', *header) + b''.join(struct.pack('>4I', *e) for e in newps + newvs))
    b.check_registers(str(a.output / 'shaders.gsh'))
    original_blocks = [(k,h,d) for k,h,d in b.gfd_blocks(original) if k != 1]
    new_blocks = [(k,h,d) for k,h,d in b.gfd_blocks((a.output / 'shaders.gsh').read_bytes()) if k != 1]
    if original_blocks != new_blocks[:len(original_blocks)] or newps[:np] != ps or newvs[:nv] != vs:
        raise ValueError('Original shader mappings were not preserved; do not deploy output')
    if (a.output / 'shaders.gsh').stat().st_size > 16 << 20:
        raise ValueError('Pack exceeds resident loading limit; do not deploy output')
    receipt = dict(added_vs=vi-counts[3], added_ps=pi-counts[6], original_blocks_preserved=True,
        original_index_entries_preserved=True, hardware_status='not tested',
        sha256={n:hashlib.sha256((a.output/n).read_bytes()).hexdigest() for n in ('shaders.gsh','shaders.idx')})
    (a.output / 'build-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__': main()
