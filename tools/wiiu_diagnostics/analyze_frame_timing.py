#!/usr/bin/env python3
"""Summarize matched Wii U runtime windows without inventing frame percentiles.

Pass --start-frame/--end-frame for the same scene in each recording. Reports
presentation cadence, not the game's simulation rate. Screenshots and menu
activity must be excluded by the experiment operator.
"""
import argparse
import json
from pathlib import Path
import re

PACE = re.compile(r'\[pace\] (\d+) frames in (\d+) ms; max gap (\d+) us; gaps <=20/35/50/67/100/>100 ms: ((?:\d+\s*){6})')
COMPILE = re.compile(r'\[glsl\] (vertex|pixel) [0-9a-f]+ compiled in (\d+) ms(.*)')
FALLBACK = re.compile(r'\[fallback-route\] missing vertex (\d+); forced CPU (\d+); missing geometry (\d+); GPU resource limit (\d+)')

def analyze(text, start=0, end=None):
    buckets = [0]*6
    frames = milliseconds = longest = 0
    compile_ms = dict(vertex=[], pixel=[])
    routes = [0]*4
    route_rows = 0
    skipped = windows = 0
    previous_pace_frame = None
    for line in text.splitlines():
        prefix = re.match(r'\s*(\d+) ', line)
        if not prefix:
            continue
        frame = int(prefix[1])
        pace = PACE.search(line)
        # Each pace row covers the preceding interval. Never include a row
        # straddling the requested scene boundary.
        prior = previous_pace_frame
        if pace:
            previous_pace_frame = frame
        if frame < start or end is not None and frame > end:
            continue
        if pace:
            n, duration, gap = map(int, pace.group(1,2,3))
            counts = list(map(int, pace[4].split()))
            if sum(counts)!=n or duration<=0 or prior is not None and frame<=prior:
                skipped += 1
                continue
            if start and (prior is None or prior < start):
                skipped += 1
                continue
            windows += 1
            frames += n; milliseconds += duration; longest = max(longest,gap)
            buckets = [a+b for a,b in zip(buckets,counts)]
        compile = COMPILE.search(line)
        if compile and ', from the cache' not in compile[3]:
            compile_ms[compile[1]].append(int(compile[2]))
        route = FALLBACK.search(line)
        if route:
            route_rows += 1
            routes = [a+int(b) for a,b in zip(routes,route.groups())]
    return dict(pace_windows=windows, rejected_or_boundary_windows=skipped,
                frames=frames, measured_ms=milliseconds,
                presentations_per_second=1000*frames/milliseconds if milliseconds else None,
                max_gap_ms=longest/1000 if frames else None,
                gaps_over_50ms=sum(buckets[3:]), gaps_over_100ms=buckets[5],
                gap_buckets=buckets,
                cold_compiles={k:dict(count=len(v),max_ms=max(v,default=0),total_wall_ms=sum(v)) for k,v in compile_ms.items()},
                fallback_report_rows=route_rows,
                cpu_fallback=dict(zip(('missing_vertex','forced','missing_geometry','resource_limit'),routes)) if route_rows else None,
                warning='Presentation cadence is not simulation FPS; compile wall time may overlap rendering. No frame percentiles are inferred from buckets.')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log',type=Path);ap.add_argument('--start-frame',type=int,default=0)
    ap.add_argument('--end-frame',type=int);ap.add_argument('--output',type=Path)
    args=ap.parse_args()
    if args.start_frame<0 or args.end_frame is not None and args.end_frame<args.start_frame:
        ap.error('invalid frame window')
    result=analyze(args.log.read_text(errors='replace'),args.start_frame,args.end_frame)
    text=json.dumps(result,indent=2)+'\n'
    if args.output:args.output.write_text(text)
    else:print(text,end='')
if __name__=='__main__':main()
