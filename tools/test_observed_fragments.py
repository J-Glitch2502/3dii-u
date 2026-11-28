"""Exercise catalog import and preserve the existing shader lookup table."""
import json,struct,tempfile
from pathlib import Path
from observed_fragments import inject,fingerprint,CATALOG
from harvest_observed_fragments import harvest
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);logs=p/'logs';out=p/'out';logs.mkdir();(out/'glsl').mkdir(parents=True)
 ps=struct.pack('>QII',1,0,0);vs=struct.pack('>QII',9,0,4)
 (out/'shaders.idx').write_bytes(struct.pack('>8I',0x53484458,1,1,1,0,0,0,0)+ps+vs)
 (out/'glsl/list.txt').write_text('old.vert\nold.frag\n')
 source='#version 330\nvoid main() {}\n'
 def rec(stage,key,text):
  b=text.encode();return struct.pack('>5I',stage,0,0,key,len(b))+b
 cache=struct.pack('>II',0x53484443,5)+rec(1<<24,17,'vertex game program')+rec(0,1,source)+rec(0,2,source)
 cat=harvest(cache,root);assert len(cat['fragments'])==2
 assert 'vertex game program' not in json.dumps(cat)
 path=logs/CATALOG;path.write_text(json.dumps(cat))
 assert inject(root,logs,out)==1
 raw=(out/'shaders.idx').read_bytes();assert raw[32:48]==ps and raw[-16:]==vs
 assert struct.unpack_from('>QII',raw,48)==(2,1,0)
 assert inject(root,logs,out)==0 and (out/'shaders.idx').read_bytes()==raw
 # Multiple keys may share one compiled shader. New shader IDs follow the
 # compiler list count, not the number of lookup keys.
 alias=struct.pack('>QII',3,0,0)
 (out/'shaders.idx').write_bytes(struct.pack('>8I',0x53484458,1,2,1,0,0,0,0)+ps+alias+vs)
 (out/'glsl/list.txt').write_text('old.vert\nold.frag\n')
 assert inject(root,logs,out)==1
 assert struct.unpack_from('>QII',(out/'shaders.idx').read_bytes(),64)==(2,1,0)
 cat['generator']='old';path.write_text(json.dumps(cat));assert inject(root,logs,out)==0
 cat['generator']=fingerprint(root);cat['fragments'][0]['key']='../escape';path.write_text(json.dumps(cat))
 before=(out/'shaders.idx').read_bytes()
 try:inject(root,logs,out)
 except ValueError:pass
 else:raise AssertionError('malformed catalog accepted')
 assert (out/'shaders.idx').read_bytes()==before
 try:harvest(cache[:-1],root)
 except ValueError:pass
 else:raise AssertionError('truncated cache accepted')
print('PASS: fragment-only export, preserved vertex/pixel mappings, missing-key import, idempotence, generator invalidation and malformed input rejection')
