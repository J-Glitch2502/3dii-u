#!/usr/bin/env python3
"""Append a compiled shader pack without replacing existing programs or key mappings."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def read_index(data):
    if len(data) < 32:
        raise ValueError('Truncated SHDX header')
    header = list(struct.unpack_from('>8I', data))
    if header[:2] != [0x53484458, 1] or len(data) != 32 + 16 * sum(header[2:4]):
        raise ValueError('Invalid SHDX index')
    entries = [struct.unpack_from('>4I', data, at) for at in range(32, len(data), 16)]
    return header, entries[:header[2]], entries[header[2]:]


def merged_index(original, extra, old_counts, extra_counts):
    header, ps, vs = read_index(original)
    _, ep, ev = read_index(extra)
    output = []
    added = []
    for old, new, kind in ((ps, ep, 6), (vs, ev, 3)):
        if any(e[2] >= old_counts[kind] for e in old) or any(e[2] >= extra_counts[kind] for e in new):
            raise ValueError('Shader index references a missing program')
        result = list(old)
        keys = {e[:2] for e in old}
        for hi, lo, index, inputs in new:
            if (hi, lo) not in keys:
                result.append((hi, lo, index + old_counts[kind], inputs))
                keys.add((hi, lo))
        output.extend(result)
        added.append(len(result) - len(old))
    header[2:4] = len(ps) + added[0], len(vs) + added[1]
    return struct.pack('>8I', *header) + b''.join(struct.pack('>4I', *e) for e in output), added


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('original', type=Path)
    ap.add_argument('extra', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--allow-streaming', action='store_true',
                    help='Allow packs above 16 MiB for the runtime that streams GSH programs on demand')
    a = ap.parse_args()
    if a.output.exists(): ap.error('Output must be a new immutable directory')
    from native_build import load_builder
    b = load_builder(a.output.parent / 'pack-merge-work')
    def blocks(folder): return list(b.gfd_blocks((folder/'shaders.gsh').read_bytes()))
    old, extra = blocks(a.original), blocks(a.extra)
    counts = lambda values: {kind: sum(k == kind for k, _, _ in values) for kind in (3, 6)}
    index, added = merged_index((a.original/'shaders.idx').read_bytes(), (a.extra/'shaders.idx').read_bytes(), counts(old), counts(extra))
    a.output.mkdir(parents=True)
    b.merge_gfd([str(a.original/'shaders.gsh'), str(a.extra/'shaders.gsh')], str(a.output/'shaders.gsh'))
    (a.output/'shaders.idx').write_bytes(index)
    b.check_registers(str(a.output/'shaders.gsh'))
    prefix = [(k, h, d) for k, h, d in old if k != 1]
    merged = [(k, h, d) for k, h, d in blocks(a.output) if k != 1]
    if merged[:len(prefix)] != prefix: raise ValueError('Original shader blocks changed; do not deploy')
    if (a.output/'shaders.gsh').stat().st_size > 16 << 20 and not a.allow_streaming:
        raise ValueError('Pack exceeds resident limit; use --allow-streaming only with a streaming runtime')
    receipt = dict(streaming_allowed=a.allow_streaming, added_pixel_keys=added[0], added_vertex_keys=added[1], original_blocks_preserved=True,
                   hardware_status='not tested', sha256={n: hashlib.sha256((a.output/n).read_bytes()).hexdigest()
                   for n in ('shaders.gsh', 'shaders.idx')})
    (a.output/'build-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__': main()
