#!/usr/bin/env python3
"""Decode AFR1 lines from the existing Wii U Session Recorder console.log."""
import argparse,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('log',type=Path);p.add_argument('--output',type=Path)
a=p.parse_args(); pending={};events=[];loss=[];beats=[];completed=[]
for line in a.log.read_text(errors='replace').replace('\r','\n').splitlines():
 m=re.search(r'\[AFR1 (MM|FS)\] (.*)',line)
 if not m:continue
 module,body=m.groups()
 if body.startswith('START '):
  pending={k:v for k,v in pending.items() if k[0]!=module}
 elif body.startswith('LOST '):loss.append({'module':module,'details':body})
 elif body.startswith('HEARTBEAT'):beats.append({'module':module,'details':body})
 elif body.startswith('E '):
  v=body.split()[1:]
  if len(v)!=10:continue
  core,seq,event=map(int,v[:3]);thread,ticks,caller,x,y,z,token=[int(n,16) for n in v[3:]]
  e=dict(module=module,core=core,sequence=seq,event=event,thread=hex(thread),ticks=ticks,caller=hex(caller),a=hex(x),b=hex(y),c=hex(z),token=hex(token))
  events.append(e)
  if event in (10,12,14,16,18,20,22,24,32,34,36,38,40,42,44,46,48): pending[(module,(seq&0x3fffffff)|(core<<30))]=e
  elif event in (11,13,15,17,19,21,23,25,33,35,37,39,41,43,45,47,49):
   begin=pending.pop((module,token),None)
   if begin:completed.append({'entry':begin,'completion':e,'elapsed_ticks':ticks-begin['ticks']})
r={'event_count':len(events),'completed_count':len(completed),'longest_completed':sorted(completed,key=lambda x:x['elapsed_ticks'],reverse=True)[:20],'loss':loss,'last_heartbeats':beats[-6:],'unmatched_entries':list(pending.values()),'events':events,
 'interpretation':'Unmatched entries are candidates, not proven deadlocks: recording may end mid-call or omit overwritten events. Lock snapshots are observations, not acquire/release hooks.'}
s=json.dumps(r,indent=2)+'\n'
if a.output:a.output.write_text(s)
else:print(s,end='')
