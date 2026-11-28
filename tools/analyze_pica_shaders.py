#!/usr/bin/env python3
"""Explain captured PICA programs and their asset recipes; never modify a game.

Reachability is conservative: unknown boolean/compare branches include both
paths. Counts are static instruction sites, not executed work or predicted FPS.
The native probe checks the existing geometry translator's vocabulary only;
passing it does not establish a native vertex backend or GPU compatibility.
Raw game instructions stay in the user's input, outside the JSON report.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from builder import shaderlogs
from pica_disasm import ARITH, ARITHI, FLOW
import dvlb_to_log
from pica_to_c import translate, Unsupported


def reachable(code, entry, bool_mask=None):
    pending, visited, sites, calls = [(entry, ())], set(), set(), set()
    bounded = False
    while pending:
        pc, stack = pending.pop()
        while stack and pc == stack[-1][0]:
            pc, stack = stack[-1][1], stack[:-1]
        state = (pc, stack)
        if state in visited or not 0 <= pc < len(code):
            continue
        visited.add(state)
        sites.add(pc)
        w = code[pc]
        op, target, count = w >> 26, (w >> 10) & 0xfff, w & 255
        boolean = None if bool_mask is None else bool(bool_mask & (1 << ((w >> 22) & 15)))
        if op == 0x22:
            continue
        if op in (0x24, 0x25, 0x26):
            if op == 0x26 and boolean is False:
                pending.append((pc + 1, stack))
                continue
            calls.add((pc, target, count))
            if len(stack) < 16:
                pending.append((target, stack + ((target + count, pc + 1),)))
            else:
                bounded = True
            if op == 0x24 or (op == 0x26 and boolean is True):
                continue
        elif op in (0x27, 0x28):
            pending.extend([(target, stack), (target + count, stack)])
        elif op in (0x2c, 0x2d):
            if op == 0x2d and boolean is not None:
                pending.append((target if boolean == (not (count & 1)) else pc + 1, stack))
                continue
            pending.append((target, stack))
        pending.append((pc + 1, stack))
        if len(visited) > 100000:
            bounded = True
            break
    return sites, calls, bounded


def analyze(entry, code, swz):
    sites, calls, bounded = reachable(code, entry)
    counts = Counter()
    for pc in sites:
        op = code[pc] >> 26
        name = ('madi' if op < 0x38 else 'mad') if op >= 0x30 else (
            ARITH.get(op) or ARITHI.get(op) or FLOW.get(op) or
            ('cmp' if op in (0x2e, 0x2f) else f'unknown_{op:02x}'))
        counts[name] += 1
    try:
        translate(entry, code, swz, 'probe')
        native = {'supported': True}
    except Unsupported as e:
        native = {'supported': False, 'first_rejection': str(e)}
    return {'entry': entry, 'instruction_memory_words': len(code),
            'conservative_instruction_sites': len(sites),
            'opcodes': dict(sorted(counts.items())), 'analysis_limit_reached': bounded,
            'calls': [{'pc': pc, 'target': t, 'length': n} for pc, t, n in sorted(calls)],
            'existing_geometry_translator_probe': native}


def asset_files(data):
    """File names and extents in an extracted RomFS; no payload in the report."""
    h = struct.unpack_from('<10I', data)
    if h[0] != 0x28:
        raise ValueError('expected extracted RomFS without hash layers')
    dm, fm, length, base = h[3], h[7], h[8], h[9]
    def directory(at, seen=()):
        if at == 0:
            return ''
        if at in seen or len(seen) > 128:
            raise ValueError('cyclic or excessive RomFS directory depth')
        parent = struct.unpack_from('<I', data, dm + at)[0]
        n = struct.unpack_from('<I', data, dm + at + 20)[0]
        return directory(parent, seen + (at,)) + '/' + data[dm + at + 24:dm + at + 24 + n].decode('utf-16le')
    p, files = 0, []
    while p + 32 <= length:
        parent = struct.unpack_from('<I', data, fm + p)[0]
        off, size = struct.unpack_from('<QQ', data, fm + p + 8)
        n = struct.unpack_from('<I', data, fm + p + 28)[0]
        name = data[fm + p + 32:fm + p + 32 + n].decode('utf-16le')
        files.append((base + off, size, directory(parent) + '/' + name))
        p += 32 + ((n + 3) & ~3)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--recipes', type=Path)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    files = []
    if args.game:
        import mmap
        with (args.game / 'romfs.bin').open('rb') as f:
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
                files = asset_files(data)
    recipes = {}
    if args.recipes:
        for entry, fp, tail, pc, ps in shaderlogs.recipes(args.recipes.read_bytes()):
            pieces = []
            for kind, parts in [('instructions', pc), ('swizzles', ps)]:
                for source, stride, words, offset in parts:
                    piece = dict(kind=kind, source=source, offset=offset, words=words, stride=stride)
                    if source == 1:
                        for at, size, name in files:
                            if at <= offset and offset + (words - 1) * stride + 4 <= at + size:
                                piece.update(asset=name, offset_in_asset=offset - at)
                                break
                    elif source == shaderlogs.UNPACKED:
                        index, inner = offset >> 32, offset & 0xffffffff
                        if index < len(files):
                            piece.update(asset=files[index][2], unpacked_offset=inner)
                    pieces.append(piece)
            recipes[(entry, fp.hex())] = pieces
    assets, asset_seen = [], set()
    if args.game:
        for blob in dvlb_to_log.programs(str(args.game)):
            try:
                code, swz, entries = dvlb_to_log.parse(blob)
                names = list(dvlb_to_log.labels(blob))
            except (ValueError, struct.error, IndexError):
                continue
            key = (tuple(code), tuple(swz))
            if key not in asset_seen:
                asset_seen.add(key)
                assets.append((code, swz, names))
    result, seen = [], set()
    for r in shaderlogs.records(args.log.read_bytes()):
        if r[0] != 'vs':
            continue
        _, entry, code, swz, tail = r
        fp = shaderlogs.fingerprint(code, swz).hex()
        bm = sum(bool(v) << i for i, v in enumerate(tail[:16]))
        if (entry, fp, bm) in seen:
            continue
        seen.add((entry, fp, bm))
        row = analyze(entry, struct.unpack(f'<{len(code)//4}I', code), struct.unpack(f'<{len(swz)//4}I', swz))
        row.update(program_fingerprint=fp, asset_pieces=recipes.get((entry, fp), []))
        words = struct.unpack(f'<{len(code)//4}I', code)
        descriptors = struct.unpack(f'<{len(swz)//4}I', swz)
        observed_sites, observed_calls, observed_limit = reachable(words, entry, bm)
        row['observed_boolean_specialization'] = {'mask': bm, 'instruction_sites': len(observed_sites),
                                                   'analysis_limit_reached': observed_limit,
                                                   'call_targets': sorted({c[1] for c in observed_calls})}
        for ac, az, names in assets:
            if tuple(ac[:len(words)]) == words and tuple(az[:len(descriptors)]) == descriptors[:len(az)]:
                row['asset_labels'] = [name for stage, start, length, name in names if stage == 0 and start == entry]
                row['call_target_labels'] = {str(c['target']): [name for stage, start, length, name in names
                                                               if stage == 0 and start == c['target']]
                                             for c in row['calls']}
                break
        result.append(row)
    report = {'format': 1, 'count_semantics': 'conservative static sites, not executed instructions or predicted FPS',
              'vertex_entries': result,
              'asset_symbols': [{'stage': stage, 'entry': start, 'length': length, 'name': name}
                                for _, _, names in assets for stage, start, length, name in names],
              'asset_symbol_warning': 'Labels include subroutines. They do not establish a drawable render state or complete coverage.'}
    args.out.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(result)} vertex entries; {len(set(r["program_fingerprint"] for r in result))} instruction-memory images; report: {args.out}')


if __name__ == '__main__':
    main()
