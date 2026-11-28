#!/usr/bin/env python3
"""Scan recognized RomFS containers for PICA combiner programs, without inventing scene state."""
import argparse
from collections import Counter
import hashlib
import json
import mmap
from pathlib import Path
import struct

from dvlb_to_log import romfs_files
from tev_to_log import contents, materials, stage, tev_list


def inventory(romfs, output):
    magic = Counter()
    programs = {}
    total = containers = blocks = failures = 0
    with romfs.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if len(data) < 40 or struct.unpack_from('<I', data)[0] != 0x28:
            raise ValueError('Expected unpacked RomFS level-3 image')
        for offset, length in romfs_files(data):
            if offset > len(data) or length > len(data) - offset:
                raise ValueError('RomFS file extent exceeds image')
            total += 1
            magic[bytes(data[offset:offset+min(length,4)]).hex()] += 1
            try:
                for decoded in contents(data[offset:offset+length]):
                    containers += 1
                    for at, raw, extra in materials(decoded):
                        blocks += 1
                        tev = tev_list([stage(i, registers, extra.get(0xE0,0)) for i,registers in enumerate(raw)])
                        encoded = json.dumps(tev,sort_keys=True,separators=(',',':'))
                        signature = hashlib.sha256(encoded.encode()).hexdigest()[:16]
                        entry = programs.setdefault(signature,dict(signature=signature,stages=tev,occurrences=0,
                            alpha_test_register=extra.get(0x104),sample_file_offset=offset,sample_command_offset=at))
                        entry['occurrences'] += 1
            except (ValueError,IndexError,struct.error):
                failures += 1
    result=dict(romfs=str(romfs),files=total,recognized_decoded_containers=containers,material_blocks=blocks,
                unique_combiner_programs=len(programs),unpack_failures=failures,header_signatures=dict(magic.most_common(24)),
                programs=sorted(programs.values(),key=lambda p:(-p['occurrences'],p['signature'])),
                exhaustive=False,limitations=['Recognizes only the existing container/material parsers; zero matches is not proof of no shaders.',
                'Lighting, textures, fog, uniforms and render passes can be configured by runtime code separately.'])
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('files','recognized_decoded_containers','material_blocks','unique_combiner_programs','unpack_failures','header_signatures')},indent=2),flush=True)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('romfs',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();inventory(a.romfs,a.output)

if __name__=='__main__':main()
