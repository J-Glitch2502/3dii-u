"""Bounded PC predictions from observed fragment configurations.

These are candidate variants, not proof that an asset or scene uses them.
Inputs retain their physical light numbers until after variant construction.
No raw vertex programs or title-specific addresses are used.
"""
import struct
import json
from pathlib import Path
from tev_to_log import decode, encode, normal, records

OUT = 'neighborhood_fs.bin'
MAX_INPUT_STATES = 4096
MAX_CANDIDATES = 50000

def variants(keys, limit=3000):
    keys = sorted(set(keys))
    if len(keys) > MAX_INPUT_STATES:
        raise ValueError('fragment observation budget exceeded')
    if limit < 0:
        raise ValueError('negative fragment candidate limit')
    states = [decode(k) for k in keys if decode(k)['head'] & 1]
    known = {encode(normal(s)) for s in states}
    # Spotlight enable is uniform-only. A shadow light can switch between
    # positional and directional projection without changing its material.
    features = sorted({(w & ~(7 | 256 | 512)) ^ direction
                       for s in states if s.get('light') for w in s['lights']
                       for direction in (0, 8)})
    fogs = sorted({s['fogat'] & 3 for s in states})
    made = {}
    def add(s, distance):
        key = encode(normal(s))
        if key not in known and (key in made or len(made) < MAX_CANDIDATES):
            made[key] = min(distance, made.get(key, distance))
    for s in states:
        prefixes = [(s, 0)]
        if s.get('light'):
            prefixes += [(dict(s, lights=s['lights'][:n], lcfg=(s['lcfg'] & ~15) | n), 1)
                         for n in range(1, len(s['lights']))]
        for base, distance in prefixes:
            choices = [(base, distance)]
            if base.get('light'):
                for i, word in enumerate(base['lights']):
                    if not word & 512:
                        continue
                    for kind in features:
                        lights = list(base['lights'])
                        lights[i] = (word & (7 | 512)) | kind
                        choices.append((dict(base, lights=lights), distance + 1))
            for candidate, cost in choices:
                add(candidate, cost)
                for fog in fogs:
                    add(dict(candidate, fogat=(candidate['fogat'] & ~3) | fog), cost + 1)
                tex = candidate['tex']
                if tex[0] & 1 and (tex[0] >> 2) & 15 == 2:
                    textures = list(tex)
                    textures[0] ^= 1 << 14
                    add(dict(candidate, tex=textures), cost + 1)
                    for fog in fogs:
                        add(dict(candidate, tex=textures, fogat=(candidate['fogat'] & ~3) | fog), cost + 2)
    ordered = sorted(made, key=lambda key: (made[key], key))
    return ordered[:limit], len(ordered)

def write(logs, paths, limit=3000):
    keys = [key for path in paths for key in records(str(path))]
    result, total = variants(keys, limit)
    output = Path(logs) / OUT
    output.write_bytes(b''.join(struct.pack('<I', len(key)) + key for key in result))
    report = dict(version=1, observed_input_states=len(set(keys)), candidate_variants=len(result),
                  candidate_total_bounded=total, candidate_budget_reached=total == MAX_CANDIDATES,
                  exhaustive=False, methods=['light-count prefixes', 'shadow-light type/direction alternatives', 'shadow projection alternatives', 'observed fog combinations'],
                  warning='Candidates are predictions from observed configurations; missing runtime states and unsupported features may remain.')
    (Path(logs) / 'fragment-discovery-report.json').write_text(json.dumps(report, indent=2) + '\n')
    bound = ' (candidate budget reached)' if total == MAX_CANDIDATES else ''
    print(f'Fragment configuration discovery: {len(result)} of {total} candidate variants{bound}; coverage is not exhaustive', flush=True)
    return result
