"""Validated fallback-pack assembly, preserving existing keys and specialized programs."""
import argparse
from pathlib import Path
import struct
import tempfile

from build_fallback_pack import builder_module
import gsh_rules


def read_packet(path):
    data=Path(path).read_bytes()
    if len(data)<12:
        raise ValueError('Truncated fallback pack')
    magic,count,size=struct.unpack_from('>III',data)
    if magic!=0x55465031 or not 1<=count<=2048 or not 32<=size<=16<<20 or len(data)!=12+8*count+size:
        raise ValueError('Invalid fallback pack header or extent')
    keys=list(struct.unpack_from('>'+str(count)+'Q',data,12))
    if len(set(keys))!=count:
        raise ValueError('Duplicate fallback keys')
    payload=data[12+8*count:]
    programs=list(gsh_rules.programs(payload))
    if len(programs)!=count or any(kind!='pixel' or regs>200 or gsh_rules.check(words,regs) for kind,regs,words in programs):
        raise ValueError('Fallback program count or GPU rules failed')
    return keys,payload


def write_packet(path,keys,payload):
    if not keys or len(keys)>2048 or len(payload)>16<<20 or len(set(keys))!=len(keys):
        raise ValueError('Fallback memory/key budget exceeded')
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_suffix('.tmp')
    temporary.write_bytes(struct.pack('>III',0x55465031,len(keys),len(payload))+struct.pack('>'+str(len(keys))+'Q',*keys)+payload)
    read_packet(temporary)
    temporary.replace(path)


def merge_packets(original,extra,output):
    keys,old=read_packet(original);added,new=read_packet(extra)
    if set(keys)&set(added):raise ValueError('Refusing to replace an existing fallback key implicitly')
    builder=builder_module()
    with tempfile.TemporaryDirectory() as directory:
        p=Path(directory);(p/'old.gsh').write_bytes(old);(p/'new.gsh').write_bytes(new)
        counts=builder.merge_gfd([str(p/'old.gsh'),str(p/'new.gsh')],str(p/'merged.gsh'))
        if counts!={3:0,6:len(keys)+len(added)}:raise ValueError('Fallback merge changed program count')
        payload=(p/'merged.gsh').read_bytes()
    old_blocks=[x for x in builder.gfd_blocks(old) if x[0]!=1]
    merged_blocks=[x for x in builder.gfd_blocks(payload) if x[0]!=1]
    if merged_blocks[:len(old_blocks)]!=old_blocks:raise ValueError('Existing fallback programs changed')
    write_packet(output,keys+added,payload)
    return dict(preserved_keys=len(keys),added_keys=len(added),bytes=Path(output).stat().st_size)


def replace_generics(original,packet,output):
    original,output=Path(original),Path(output)
    keys,new=read_packet(packet)
    if len(keys)!=16:raise ValueError('Expected 16 full fallback families')
    index=(original/'shaders.idx').read_bytes()
    if len(index)<32:raise ValueError('Truncated shader index')
    magic,version,np,nv,*_=struct.unpack_from('>8I',index)
    if magic!=0x53484458 or version!=1 or len(index)!=32+16*(np+nv):raise ValueError('Invalid shader index')
    entries={struct.unpack_from('>Q',index,32+16*i)[0]:struct.unpack_from('>I',index,40+16*i)[0] for i in range(np)}
    if any(key not in entries for key in keys):raise ValueError('Missing fallback family index')
    mapping={entries[key]:i for i,key in enumerate(keys)}
    if len(mapping)!=16:raise ValueError('Fallback families unexpectedly alias program indices')
    builder=builder_module()
    blocks=list(builder.gfd_blocks((original/'shaders.gsh').read_bytes()))
    replacements={(kind,struct.unpack_from('>I',head,28)[0]):(head,body) for kind,head,body in builder.gfd_blocks(new) if kind in (6,7)}
    pieces=[(original/'shaders.gsh').read_bytes()[:32]];replaced=0
    for kind,head,body in blocks:
        i=struct.unpack_from('>I',head,28)[0]
        if kind in (6,7) and i in mapping:
            nh,body=replacements[kind,mapping[i]]
            head=nh[:24]+head[24:32]+nh[32:];replaced+=1
        pieces.extend((head,body))
    if replaced!=32:raise ValueError('Incomplete generic shader replacement')
    payload=b''.join(pieces)
    if len(payload)>16<<20 and (original/'shaders.gsh').stat().st_size<=16<<20:
        raise ValueError('Replacement would push a resident shader pack onto streamed reads')
    assembled=list(builder.gfd_blocks(payload))
    if len(assembled)!=len(blocks):raise ValueError('Shader blocks changed count')
    for old,updated in zip(blocks,assembled):
        kind,head,_=old;i=struct.unpack_from('>I',head,28)[0]
        if not (kind in (6,7) and i in mapping) and old!=updated:raise ValueError('Specialized/vertex shader changed')
    if any(gsh_rules.check(words,regs) or regs>(124 if kind=='vertex' else 200) for kind,regs,words in gsh_rules.programs(payload)):
        raise ValueError('Final shader pack fails GPU rules')
    output.mkdir(parents=True,exist_ok=True)
    (output/'shaders.gsh').write_bytes(payload);(output/'shaders.idx').write_bytes(index)
    return dict(replaced_families=16,other_programs_unchanged=True,index_unchanged=True,bytes=len(payload))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('operation',choices=('merge','replace-generics'))
    p.add_argument('original',type=Path);p.add_argument('extra',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();print(merge_packets(a.original,a.extra,a.output) if a.operation=='merge' else replace_generics(a.original,a.extra,a.output))

if __name__=='__main__':main()
