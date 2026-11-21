#!/usr/bin/env python3
"""Describe the original RomFS byte range inside a WUHB, without extracting assets."""
from pathlib import Path
import argparse
import struct


def locate(bundle, name='content/romfs.bin'):
    total = bundle.stat().st_size
    with bundle.open('rb') as f:
        header = f.read(80)
        if len(header) != 80:
            raise ValueError('Truncated WUHB header')
        magic, size, dh, dhn, dt, dtn, fh, fhn, ft, ftn, data = struct.unpack('>II9Q', header)
        if magic != 0x57554842 or size != 80 or total > 0xffffffff or data > total:
            raise ValueError('Unsupported WUHB header/size')
        for offset, length in [(dh,dhn),(dt,dtn),(fh,fhn),(ft,ftn)]:
            if offset > total or length > total-offset or length > 32 << 20:
                raise ValueError('WUHB metadata exceeds file bounds')
        f.seek(dt); directories=f.read(dtn)
        f.seek(ft); files=f.read(ftn)
        def entry(table, at, directory):
            minimum = 24 if directory else 32
            if at < 0 or at > len(table)-minimum:
                raise ValueError('Invalid WUHB entry offset')
            if directory:
                parent,sibling,child,first,nexth,nlen=struct.unpack_from('>6I',table,at)
                result=(parent,sibling,child,first)
            else:
                parent,sibling,offset,length,nexth,nlen=struct.unpack_from('>IIQQII',table,at)
                result=(parent,sibling,offset,length)
            if nlen > len(table)-at-minimum:
                raise ValueError('Truncated WUHB entry name')
            return result, table[at+minimum:at+minimum+nlen].decode('utf-8')
        parent=0
        parts=name.split('/')
        for part in parts[:-1]:
            current=entry(directories,parent,True)[0][2]
            visited=set()
            while current != 0xffffffff:
                if current in visited:raise ValueError('Cyclic WUHB directory list')
                visited.add(current)
                values,label=entry(directories,current,True)
                if values[0] != parent:raise ValueError('Invalid WUHB directory parent')
                if label==part:parent=current;break
                current=values[1]
            else:raise ValueError('WUHB directory not found: '+part)
        current=entry(directories,parent,True)[0][3];visited=set()
        while current != 0xffffffff:
            if current in visited:raise ValueError('Cyclic WUHB file list')
            visited.add(current);values,label=entry(files,current,False)
            if values[0] != parent:raise ValueError('Invalid WUHB file parent')
            if label==parts[-1]:
                offset,length=values[2:]
                if offset > total-data or length > total-data-offset:
                    raise ValueError('WUHB data exceeds file bounds')
                return data+offset,length,total
            current=values[1]
        raise ValueError('WUHB file not found: '+name)


def descriptor(bundle, sd_path):
    from ftp_homebrew_dev import validate_bundle
    validate_bundle(sd_path)
    off,length,total=locate(bundle)
    path=('/vol/external01/'+sd_path).encode('utf-8')+b'\0'
    if len(path)>640:raise ValueError('Bundle path is too long')
    return b'DROMFS01'+struct.pack('>QQQ',off,length,total)+path


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('bundle',type=Path);ap.add_argument('sd_path');ap.add_argument('output',type=Path)
    a=ap.parse_args();result=descriptor(a.bundle,a.sd_path);a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(result)
    print('RomFS source:',locate(a.bundle),'descriptor:',len(result),'bytes')

if __name__=='__main__':main()
