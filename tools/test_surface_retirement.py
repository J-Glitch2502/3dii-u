#!/usr/bin/env python3
"""Run the renderer's actual retirement loop against simulated reused guest memory.

GPU allocation calls are replaced by stubs; no ROM, Cemu, or Wii U SDK is needed.
CXX may select a native C++ compiler. The Windows builder's compiler is used there.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

PREAMBLE = r'''
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <memory>
#include <cassert>
#include <cstdio>
#include "surface_readback.h"
using surface_readback::guest_wrote;
using u64=uint64_t;using u32=uint32_t;
struct Surface {u64 drawn=0,written_after=0; surface_readback::Spans written; u64 last_use=0; u32 addr=0x1000,written_lo=0,written_hi=0;bool first_use=false;u32 bytes()const{return 16;}bool depth=false,in_list=false,shared_depth=false,gpu_newer=false,guest_newer=false,mem1=false; void *image(){return nullptr;} };
std::vector<std::unique_ptr<Surface>> surfaces;
Surface *cur_color=nullptr,*cur_depth=nullptr,*depth_owner=nullptr;
size_t next_surface_retirement;
bool transfer_source(const Surface&){return false;}
u64 use_counter=1900; bool state_sent_valid=true,state_lost=false;
void (*GfxHeapFreeMEM1)(void*)=nullptr;
unsigned writes=0; unsigned char archive[4]={'d','a','r','c'};
void write_back(Surface&){writes++;for(auto &b:archive)b=0xff;}
'''

TESTS = r'''
Surface* add(bool dirty,bool depth=false){auto p=std::make_unique<Surface>();p->gpu_newer=dirty;p->depth=depth;auto *s=p.get();surfaces.push_back(std::move(p));return s;}
int main(){
 auto *dirty=add(true);forget_unused();assert(surfaces.size()==1 && surfaces[0].get()==dirty);assert(writes==0 && archive[0]=='d');
 dirty->guest_newer=true;forget_unused();assert(surfaces.size()==1); // Partial CPU writes do not make the whole GPU image disposable.
 dirty->gpu_newer=false;forget_unused();assert(surfaces.empty());assert(!state_sent_valid && state_lost);
 add(true,true);forget_unused();assert(surfaces.empty());
 auto *s=add(false);s->last_use=101;forget_unused();assert(surfaces.size()==1);s->last_use=100;forget_unused();assert(surfaces.empty());
 s=add(false);cur_color=s;forget_unused();assert(surfaces.size()==1);cur_color=nullptr;cur_depth=s;forget_unused();assert(surfaces.size()==1);cur_depth=nullptr;
 s->in_list=true;forget_unused();assert(surfaces.size()==1);s->in_list=false;s->shared_depth=true;depth_owner=s;forget_unused();assert(surfaces.size()==1);depth_owner=nullptr;forget_unused();assert(surfaces.empty());
 add(false);add(false);forget_unused();assert(surfaces.size()==1);forget_unused();assert(surfaces.empty());
 // Many completed scene lifetimes can retire once a definite write supersedes every byte.
 for(int i=0;i<10000;++i){s=add(true);guest_wrote(*s,0x1000,16);assert(!s->gpu_newer && s->first_use);forget_unused();assert(surfaces.empty());}
 s=add(true);guest_wrote(*s,0xff0,32);forget_unused();assert(surfaces.empty());
 // A bounding span made from separated writes is not proof the gap was overwritten.
 s=add(true);guest_wrote(*s,0x1000,4);guest_wrote(*s,0x100c,4);forget_unused();assert(surfaces.size()==1 && s->gpu_newer);
 guest_wrote(*s,0x1000,15);forget_unused();assert(surfaces.empty());
 s=add(true);
 guest_wrote(*s,0x1001,16);forget_unused();assert(surfaces.size()==1);
 guest_wrote(*s,0x1000,16);forget_unused();assert(surfaces.empty());
 s=add(true);guest_wrote(*s,0x1000,16);s->gpu_newer=true;s->guest_newer=false;guest_wrote(*s,0x1000,4);forget_unused();assert(surfaces.size()==1);surfaces.clear();
 assert(writes==0 && archive[0]=='d');puts("PASS: real retirement function preserves reused archive and GPU-dirty surfaces, retires synchronized/depth targets, respects age and references; 10000 full-overwrite cycles reclaim without accumulation, partial/gapped writes retain GPU data");
}
'''


def main():
    source = (ROOT / 'port/wiiu/gx2_backend.cpp').read_text()
    start = source.index('void forget_unused() {')
    end = source.index('\n}\n', start) + 3
    if os.environ.get('CXX'):
        compiler = shlex.split(os.environ['CXX'])
    elif os.name == 'nt':
        sys.path.insert(0, str(ROOT / 'tools/win'))
        import build3ds
        compiler = build3ds.HOST_CXX
    else:
        compiler = ['c++']
    with tempfile.TemporaryDirectory(prefix='3dii_retirement_') as tmp:
        tmp = Path(tmp)
        test = tmp / 'test.cpp'
        exe = tmp / ('test.exe' if os.name == 'nt' else 'test')
        test.write_text(PREAMBLE + source[start:end] + TESTS)
        subprocess.run(compiler + ['-std=c++20', '-O1', '-I'+str(ROOT/'port/wiiu'), str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
