#!/usr/bin/env python3
"""Inventory asset-derived and observed fragment material states without claiming exhaustive discovery."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct

from tev_to_log import records, decode, encode, normal


def fnv64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    return f'{value:016x}'


def describe(data):
    if len(data) < 4 or len(data) % 4:
        raise ValueError('Truncated fragment key')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    head = words[0]
    if not head & 1:
        return None
    if len(words) < 9 or words[4] > 6:
        raise ValueError('Invalid shaded fragment key')
    count = words[4]
    end = 5 + count * 5
    if len(words) < end + 4:
        raise ValueError('Truncated combiner program')
    canonical = encode(normal(decode(data)))
    canonical_words = struct.unpack('<' + 'I' * (len(canonical) // 4), canonical)
    combiner = canonical_words[4:end]
    lit = words[end] != 0
    lights = words[end + 1] & 15 if lit else 0
    alpha_at = end + 1 + (8 + lights if lit else 0)
    if lights > 8 or alpha_at + 3 != len(words):
        raise ValueError('Inconsistent lighting/fragment key length')
    mask = sum(1 << i for i, w in enumerate(words[1:4]) if w & 1 and not w & 2)
    hazards = []
    if head & 2:
        hazards.append('w_buffer_depth_not_written_by_generic_shader')
    if head & 4:
        hazards.append('exclusion_scissor_not_tested_by_generic_shader')
    for i, texture in enumerate(words[1:4]):
        if texture & 1 and not texture & 2 and i == 0 and (texture >> 2 & 15) in (1, 4):
            hazards.append('cube_texture_not_supported_by_generic_shader')
    procedural = False
    operations = []
    for i in range(count):
        stage, a, b, c, op = canonical_words[5 + 5*i:10 + 5*i]
        if stage >= 6:
            raise ValueError('Invalid combiner stage index')
        procedural |= any(v & 15 == 6 or v >> 4 & 15 == 6 for v in (a, b, c))
        operations.append(dict(stage=stage, color_op=op & 15, alpha_op=op >> 4 & 15))
    if procedural:
        hazards.append('procedural_source_requires_separate_support')
    # This signature intentionally excludes light/LUT enables, wrapping, alpha
    # tests and fog, which a material-family shader would continue to read live.
    material = (int(lit), mask, *combiner)
    signature = hashlib.sha256(struct.pack('<'+'I'*len(material), *material)).hexdigest()[:16]
    return dict(key=fnv64(data), material=signature, stages=count, lit=lit, lights=lights,
                texture_mask=mask, hazards=sorted(set(hazards)), operations=operations)


def analyze(observed, assets, traces, output, preserved=()):
    states = {}
    sources = []
    for provenance, paths in (('observed', observed), ('asset_derived', assets), ('preserved_catalog', preserved)):
        for path in paths:
            count = 0
            for data in records(str(path)):
                description = describe(data)
                if description is None:
                    continue
                count += 1
                key = description['key']
                item = states.setdefault(key, dict(**description, provenance=[], data=data))
                if item['data'] != data:
                    raise ValueError('Fragment-key collision')
                if provenance not in item['provenance']:
                    item['provenance'].append(provenance)
            sources.append(dict(path=str(path), provenance=provenance, records=count))
    live = Counter()
    for trace in traces:
        # Each row is one captured distinct draw-state, not draw frequency.
        for line in trace.read_text(errors='replace').splitlines():
            if '[3dii-u draw]' in line:
                match = re.search(r'fs=([0-9a-fA-F]{16})', line)
                if match:
                    live[match[1].lower()] += 1
    groups = defaultdict(list)
    for item in states.values():
        groups[item['material']].append(item)
    families = []
    seeds = []
    for signature, entries in sorted(groups.items()):
        sample = min(entries, key=lambda e: e['key'])
        seeds.append(sample['data'])
        families.append(dict(signature=signature, states=len(entries), stages=sample['stages'], lit=sample['lit'],
                             light_counts=sorted({e['lights'] for e in entries}), texture_mask=sample['texture_mask'],
                             live_trace_rows=sum(live[e['key']] for e in entries),
                             hazards=sorted({h for e in entries for h in e['hazards']})))
    families.sort(key=lambda e: (-e['live_trace_rows'], -e['states'], e['signature']))
    hazards = Counter(h for e in states.values() for h in e['hazards'])
    live_issues = [dict(key=key, trace_rows=rows, **{k:v for k,v in states[key].items() if k not in ('key','data')})
                   for key, rows in live.items() if key in states and states[key]['hazards']]
    report = dict(unique_states=len(states), material_families=len(groups), sources=sources,
                  observed_states=sum('observed' in e['provenance'] for e in states.values()),
                  asset_derived_states=sum('asset_derived' in e['provenance'] for e in states.values()),
                  preserved_catalog_states=sum('preserved_catalog' in e['provenance'] for e in states.values()),
                  known_generic_feature_gaps=dict(hazards), live_known_keys=len(live.keys() & states.keys()),
                  live_unknown_keys=sorted(live.keys()-states.keys()), live_feature_gap_candidates=live_issues,
                  families=families,
                  limitations=['Asset-derived states include reconstructed scene settings, not confirmed runtime combinations.',
                               'Trace rows count sampled distinct states, not pixel cost or draw frequency.',
                               'Feature gaps are code-supported candidates, not proof of the missing UI cause.',
                               'Runtime-generated material programs may be absent from the asset inventory.',
                               'Preserved catalog entries retain old shader coverage and are not all confirmed runtime observations.'])
    output.mkdir(parents=True, exist_ok=True)
    (output/'fallback-material-report.json').write_text(json.dumps(report, indent=2)+'\n')
    (output/'material-seeds.bin').write_bytes(b''.join(struct.pack('<I', len(data))+data for data in seeds))
    print(json.dumps({k:report[k] for k in ('unique_states','material_families','observed_states','asset_derived_states','known_generic_feature_gaps','live_known_keys','live_unknown_keys')},indent=2))
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--observed', type=Path, action='append', default=[])
    p.add_argument('--asset-log', type=Path, action='append', default=[])
    p.add_argument('--preserved', type=Path, action='append', default=[])
    p.add_argument('--trace', type=Path, action='append', default=[])
    p.add_argument('--output', type=Path, required=True)
    a=p.parse_args()
    if not (a.observed or a.asset_log or a.preserved):p.error('Supply at least one observed or asset-derived fragment log')
    analyze(a.observed,a.asset_log,a.trace,a.output,a.preserved)

if __name__=='__main__':main()
