"""Convert a console SHDC cache to shareable fragment-only PC build metadata."""
import argparse,json,struct,hashlib
from pathlib import Path
from observed_fragments import fingerprint

def harvest(data,root):
    if len(data)<8 or struct.unpack_from('>II',data)[0]!=0x53484443 or struct.unpack_from('>II',data)[1] not in (2,4,5,6,7,8,9):
        raise ValueError('Unsupported or truncated SHDC cache')
    at=8;found={}
    while at<len(data):
        if at+20>len(data):raise ValueError('Truncated record')
        stage,inputs,hi,lo,size=struct.unpack_from('>5I',data,at);at+=20
        if stage>>24 not in (0,1) or size>1<<20 or at+size>len(data):raise ValueError('Invalid record')
        text=data[at:at+size].decode('utf-8');at+=size
        if stage>>24:continue # vertex GLSL can contain translated game programs
        key=f'{hi:08x}{lo:08x}'
        if key in found and found[key]!=text:raise ValueError('Conflicting fragment key')
        found[key]=text
    return dict(version=1,generator=fingerprint(root),source_cache_sha256=hashlib.sha256(data).hexdigest(),fragments=[dict(key=k,glsl=v) for k,v in sorted(found.items())])

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('cache',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=harvest(a.cache.read_bytes(),Path(__file__).resolve().parents[1]);a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('Exported',len(result['fragments']),'fragment states; vertex programs excluded')
