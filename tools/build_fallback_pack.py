#!/usr/bin/env python3
"""Compile bounded fallback families on the PC, retaining full material semantics."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def builder_module():
    previous = os.environ.get('PATH', '')
    sys.path.insert(0, str(ROOT / 'tools/win'))
    try:
        import build3ds
        return build3ds
    finally:
        os.environ['PATH'] = previous


def build(generator, compiler, output, jobs=4, materials=None, full=False, lighting=False, exclude_pack=None, material_extension=False, generator_args=(), max_families=None):
    output.mkdir(parents=True, exist_ok=True)
    glsl = output / 'glsl'
    subprocess.run([str(generator), *generator_args] + (['--full', str(glsl)] if full else ['--lighting', str(glsl), str(materials)] if lighting else [str(glsl)] + ([str(materials)] if materials else [])), check=True)
    entries = [line.split('\t') for line in (glsl / 'families.tsv').read_text().splitlines()]
    candidates = len(entries)
    if not entries: raise ValueError('No fallback families generated')
    if max_families is not None: entries = entries[:max_families]
    if not entries or len(entries) > 2048 or (not materials and len(entries) != (16 if full else 336)) or len({key for key, _ in entries}) != len(entries):
        raise ValueError('Incomplete or oversized fallback family manifest')
    for key, name in entries:
        if len(key) != 16 or Path(name).name != name or not name.endswith('.frag') or not (glsl / name).is_file():
            raise ValueError('Invalid fallback source manifest')
        int(key, 16)
    excluded = 0
    if exclude_pack:
        from fallback_pack import read_packet
        previous, _ = read_packet(exclude_pack)
        previous = set(previous)
        kept = [entry for entry in entries if int(entry[0], 16) not in previous]
        excluded = len(entries) - len(kept)
        entries = kept
    if not entries:
        raise ValueError('No new material families remain after excluding the existing pack')
    if material_extension:
        if not materials or full or lighting:
            raise ValueError('Material extension requires material input, not full/lighting mode')
        match = re.search(r'kMaterialExtensionTag\s*=\s*(0x[0-9a-fA-F]+)ull', (ROOT/'port/wiiu/uber_family.h').read_text())
        if not match:
            raise ValueError('Missing shared material extension namespace')
        tag = int(match[1], 16)
        entries = [(f'{int(key,16)^tag:016x}', name) for key, name in entries]
        if exclude_pack:
            kept = [entry for entry in entries if int(entry[0],16) not in previous]
            excluded += len(entries) - len(kept)
            entries = kept
            if not entries:
                raise ValueError('All extension families already exist in the supplied pack')
    (output/'compiled-families.tsv').write_text(''.join(key+'\t'+name+'\n' for key,name in entries))
    batches = []
    for start in range(0, len(entries), 32):
        dest = output / f'batch-{start:04}.gsh'
        command = [str(compiler), '--uniform-mode', 'block']
        for _, name in entries[start:start + 32]:
            command += ['-ps', name]
        command += ['-o', str(dest)]
        batches.append((command, dest))
    def compile_batch(item):
        command, dest = item
        result = subprocess.run(command, cwd=glsl, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        dest.with_suffix('.log').write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'Fallback compiler failed: {dest.with_suffix(".log")}')
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(compile_batch, batches))
    builder = builder_module()
    raw, fixed = output / 'families.gsh', output / 'families-fixed.gsh'
    counts = builder.merge_gfd([str(dest) for _, dest in batches], str(raw))
    if counts != {3: 0, 6: len(entries)}:
        raise ValueError('Fallback compiler changed family count/order')
    subprocess.run([str(generator), *generator_args, '--fit', str(raw), str(fixed)], check=True)
    spec = importlib.util.spec_from_file_location('fallback_gsh_rules', ROOT / 'tools/gsh_rules.py')
    rules = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rules)
    programs = list(rules.programs(fixed.read_bytes()))
    if len(programs) != len(entries):
        raise ValueError('Finalized fallback program count changed')
    for i, (kind, registers, words) in enumerate(programs):
        if kind != 'pixel' or registers > 200 or rules.check(words, registers):
            raise ValueError(f'Fallback family {i} fails GPU instruction checks')
    data = fixed.read_bytes()
    if len(data) > 16 << 20:
        raise ValueError('Bounded fallback pack exceeds runtime memory cap')
    payload = struct.pack('>III', 0x55465031, len(entries), len(data))
    payload += b''.join(struct.pack('>Q', int(key, 16)) for key, _ in entries) + data
    destination = output / 'fallback-shaders.bin'
    temporary = destination.with_suffix('.tmp')
    temporary.write_bytes(payload)
    temporary.replace(destination)
    report = dict(families=len(entries), bytes=len(payload), max_registers=max(p[1] for p in programs),
                  uncompiled_families=candidates-len(entries), hardware_status='not tested', preserves_material_features=True, excluded_existing=excluded, material_extension=material_extension)
    (output / 'fallback-build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return destination


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--generator', type=Path, required=True)
    p.add_argument('--compiler', type=Path, required=True, help='CafeGLSL 1.0.0-rc1 host compiler')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--materials', type=Path, help='Length-prefixed fragment material observations')
    p.add_argument('--full', action='store_true', help='Generate the 16 full generic fallback programs')
    p.add_argument('--lighting', action='store_true', help='Specialize structural lighting from the supplied observations')
    p.add_argument('--exclude-pack', type=Path, help='Preserve existing family keys/programs; compile only missing families')
    p.add_argument('--material-extension', action='store_true', help='Use the switchable material-extension namespace shared with the runtime')
    a = p.parse_args()
    if a.jobs < 1:
        p.error('--jobs must be positive')
    build(a.generator.resolve(), a.compiler.resolve(), a.output.resolve(), a.jobs, a.materials.resolve() if a.materials else None, a.full, a.lighting, a.exclude_pack.resolve() if a.exclude_pack else None, a.material_extension)


if __name__ == '__main__':
    main()
