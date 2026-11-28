#!/usr/bin/env python3
"""Extract complete bounded runtime fallback captures as fragment observations."""
import argparse
import json
from pathlib import Path
import re
import struct
from analyze_fallback_materials import fnv64, describe


def decode(path, output):
    current=None
    captures=[]
    invalid=0
    start=re.compile(r'\[fbprobe\] frame (\d+) key ([0-9a-f]+) rt ([0-9a-f]+) (\d+)x(\d+) uber (\d+) lighting (\d+) bytes (\d+)')
    for line in path.read_text(errors='replace').splitlines():
        match=start.search(line)
        if match:
            if current is not None:invalid+=1
            f,key,target,w,h,uber,lit,size=match.groups()
            current=dict(frame=int(f),key=key,target=target,width=int(w),height=int(h),generic=bool(int(uber)),lit=bool(int(lit)),uniform_size=int(size),key_bytes=bytearray(),uniform_bytes=bytearray())
        elif current is not None:
            match=re.search(r'\[(fbkey|fbuni)\] (\d+) ([0-9a-f]+)',line)
            if match:
                kind,offset,data=match.groups()
                dest=current['key_bytes' if kind=='fbkey' else 'uniform_bytes']
                if int(offset)!=len(dest):invalid+=1;current=None;continue
                try:dest.extend(bytes.fromhex(data))
                except ValueError:invalid+=1;current=None
            elif '[fbprobe] end' in line:
                key=bytes(current['key_bytes']);uni=bytes(current['uniform_bytes'])
                if fnv64(key)!=current['key'] or len(uni)!=current['uniform_size']:
                    invalid+=1
                else:
                    try:
                        current['description']=describe(key)
                    except ValueError:
                        invalid+=1
                    else:
                        captures.append(current)
                current=None
    if current is not None:invalid+=1
    output.mkdir(parents=True,exist_ok=True)
    unique={bytes(c['key_bytes']) for c in captures}
    (output/'captured_fs.bin').write_bytes(b''.join(struct.pack('<I',len(k))+k for k in sorted(unique)))
    rows=[]
    for c in captures:
        rows.append({k:v for k,v in c.items() if k not in ('key_bytes','uniform_bytes')})
    report=dict(complete_captures=len(captures),unique_states=len(unique),incomplete_or_invalid=invalid,captures=rows)
    (output/'capture-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('complete_captures','unique_states','incomplete_or_invalid')},indent=2))
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();decode(a.log,a.output)

if __name__=='__main__':main()
