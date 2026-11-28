"""Add shared, hardware-observed fragment states before the PC compiler runs.

Only generated fragment GLSL is accepted here, never captured vertex programs.
A generator fingerprint prevents silently reusing sources across renderer changes.
"""
import hashlib
import json
from pathlib import Path
import re
import struct

CATALOG = 'observed_fragments.json'
SOURCES = ('shadergen.cpp', 'shaderkeys.cpp', 'shadergen.h', 'fs_key.inc')

def fingerprint(root):
    h = hashlib.sha256()
    for name in SOURCES:
        h.update(name.encode()); h.update((Path(root)/'port/wiiu'/name).read_bytes())
    return h.hexdigest()

def inject(root, logs, output):
    catalog = Path(logs)/CATALOG
    if not catalog.is_file(): return 0
    data = json.loads(catalog.read_text(encoding='utf-8'))
    if data.get('version') != 1: raise ValueError('Unsupported observed fragment catalog')
    if data.get('generator') != fingerprint(root):
        print('Observed fragment catalog skipped: renderer changed; regenerate catalog', flush=True)
        return 0
    records = data.get('fragments')
    if not isinstance(records, list) or len(records)>4096: raise ValueError('Invalid observed fragment count')
    out = Path(output); raw = (out/'shaders.idx').read_bytes()
    if len(raw)<32: raise ValueError('Truncated shader index')
    header = list(struct.unpack_from('>8I', raw)); np,nv = header[2:4]
    if header[:2]!=[0x53484458,1] or len(raw)!=32+16*(np+nv): raise ValueError('Invalid shader index')
    ps = [raw[32+16*i:48+16*i] for i in range(np)]
    vs = raw[32+16*np:]
    known = {struct.unpack_from('>Q', e)[0] for e in ps}
    # Validate the whole catalog before touching generated output.
    selected = {}; total=0
    for record in records:
        key,text = record.get('key',''),record.get('glsl','')
        if not re.fullmatch('[0-9a-f]{16}',key) or not isinstance(text,str) or not text.startswith('#version'):
            raise ValueError('Invalid observed fragment source')
        total += len(text.encode())
        if len(text)>1<<20 or total>16<<20: raise ValueError('Observed fragment source budget exceeded')
        if key in selected and selected[key]!=text: raise ValueError('Conflicting observed fragment key')
        selected[key]=text
    names=[]
    original_names=(out/'glsl/list.txt').read_text().split()
    next_index=sum(name.endswith('.frag') for name in original_names)
    if any(struct.unpack('>QII',e)[1]>=next_index for e in ps):
        raise ValueError('Fragment index does not match generated source list')
    for key,text in sorted(selected.items()):
        k=int(key,16)
        if k in known: continue
        name='observed_'+key+'.frag'
        (out/'glsl'/name).write_text(text,encoding='utf-8')
        names.append(name);ps.append(struct.pack('>QII',k,next_index,0));known.add(k);next_index+=1
    if names:
        path=out/'glsl/list.txt';path.write_text(path.read_text().rstrip()+'\n'+'\n'.join(names)+'\n')
        header[2]=len(ps)
        (out/'shaders.idx').write_bytes(struct.pack('>8I',*header)+b''.join(ps)+vs)
    print(f'Observed hardware fragment states: {len(names)} additional shaders compiled on the PC',flush=True)
    return len(names)
