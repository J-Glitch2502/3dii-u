#!/usr/bin/env python3
"""Summarize the optional canvas-change-to-upload probe; no game data required."""
import argparse
import json
from pathlib import Path
import re
import statistics


def analyze(text):
    run = 0
    changed = {}
    samples = []
    for line in text.splitlines():
        if '[loader] build' in line:
            run += 1
            changed.clear()
        match = re.search(r'\[ink probe\] (changed|upload) (.*)', line)
        if not match:
            continue
        fields = {key: int(value, 16 if key == 'addr' else 10)
                  for key, value in re.findall(r'(\w+)=([0-9a-f]+)', match[2])}
        key = fields['addr'], fields['mode']
        if match[1] == 'changed':
            changed[key] = fields
            continue
        fields['run'] = run
        fields['unflushed'] = changed.pop(key, {}).get('unflushed')
        samples.append(fields)
    modes = {}
    for mode in (0, 1):
        group = [sample for sample in samples if sample['mode'] == mode]
        if not group:
            continue
        times = [sample['delay_us'] / 1000 for sample in group]
        modes[str(mode)] = {
            'recorded_updates': len(group),
            'min_ms': min(times),
            'median_ms': statistics.median(times),
            'max_ms': max(times),
            'max_delayed_frames': max(sample['delay_frames'] for sample in group),
            'updates_without_learned_unflushed_flag': sum(sample['unflushed'] == 0 for sample in group),
        }
    return {
        'metric': 'renderer observation of changed canvas bytes to upload completion; not physical pen-to-display latency',
        'samples': samples,
        'modes': modes,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='runtime.log or its session directory')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    log = args.log / 'runtime.log' if args.log.is_dir() else args.log
    result = analyze(log.read_text(errors='replace'))
    result['log'] = str(log)
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
