#!/usr/bin/env python3
"""Compare a supplied pre-batching Y2R source with the current service.

Example: save the older y2r.cpp with git show, then pass its path. This uses
synthetic frames only. Host timing is not a Wii U FPS benchmark.
"""
import argparse
from pathlib import Path
import os, shlex, subprocess, tempfile
from test_y2r_coefficients import ROOT, STUBS

HARNESS = r'''
#include <chrono>
int main(int argc,char**){
 using namespace kernel;
 if(argc>1){
  Y2rService s("y2r:u");s.input_format=1;s.output_format=1;s.width=512;s.lines=240;s.block=1;s.alpha=255;memcpy(s.co,kStandard[0],sizeof s.co);
  s.y={0x10000,122880,512,0};s.u={0x40000,30720,256,0};s.v={0x50000,30720,256,0};s.out={0x60000,368640,12288,0};
  for(u32 i=0;i<memory.size();i++)memory[i]=(i*17+(i>>7))&255;
  auto start=std::chrono::steady_clock::now();for(int i=0;i<50;i++)s.convert();
  fprintf(stderr,"ms/frame %.6f\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/50);return 0;
 }
 for(u32 inf=0;inf<5;inf++)for(u32 fmt=0;fmt<4;fmt++)for(u32 rot=0;rot<4;rot++)for(u32 block=0;block<2;block++)for(u32 mult:{0u,8u,64u,256u}){
  for(u32 i=0;i<memory.size();i++)memory[i]=(i*17+(i>>7))&255;
  Y2rService s("y2r:u");s.input_format=inf;s.output_format=fmt;s.width=16;s.lines=16;s.block=block;s.rotation=rot;s.alpha=231;memcpy(s.co,kStandard[0],sizeof s.co);
  u32 step=(inf==2||inf==3)?2:1,bytes=fmt==0?4:fmt==1?3:2;
  s.y={128,1024,16*step,4};s.u={2048,512,8*step,4};s.v={4096,512,8*step,4};s.yuyv={128,1024,32,4};s.out={8192,4096,bytes*mult,7};s.convert();
  fwrite(memory.data()+8192,1,4096,stdout);
 }
}
'''

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('baseline_source',type=Path)
    ap.add_argument('--output-format',type=int,choices=(0,1),default=1,
                    help='timed movie format: 0 RGBA8 (Fantasy Life), 1 RGB8')
    args=ap.parse_args()
    harness=HARNESS
    if args.output_format==0:
        harness=harness.replace('s.output_format=1;s.width=512','s.output_format=0;s.width=512')
        harness=harness.replace('s.out={0x60000,368640,12288,0}',
                                's.out={0x60000,491520,16384,0}')
    compiler=shlex.split(os.environ.get('CXX','c++'))
    if os.name=='nt' and 'CXX' not in os.environ:
        import sys
        sys.path.insert(0,str(ROOT/'tools/win'));import build3ds
        compiler=build3ds.HOST_CXX
    with tempfile.TemporaryDirectory(prefix='3dii_y2r_bench_') as tmp:
        tmp=Path(tmp);executables={};results={}
        for name,path in [('before',args.baseline_source),('after',ROOT/'third_party/3dsrecomp/runtime/services/y2r.cpp')]:
            source=path.read_text().replace('#include "../service.h"','').replace('#include "services.h"','')
            cpp=tmp/(name+'.cpp');exe=tmp/(name+('.exe' if os.name=='nt' else ''))
            cpp.write_text(STUBS.replace('16384','2097152')+source+harness)
            subprocess.run(compiler+['-std=c++20','-O2',str(cpp),'-o',str(exe)],check=True)
            executables[name]=exe;results[name]=subprocess.check_output([str(exe)])
        if results['before']!=results['after']:
            raise RuntimeError('Converted frame or DMA-gap bytes differ')
        print(f'PASS: 640 configurations, {len(results["after"]):,} identical output bytes')
        for _ in range(3):
            for name,exe in executables.items():
                run=subprocess.run([str(exe),'bench'],capture_output=True,text=True,check=True)
                print(name,run.stderr.strip())

if __name__=='__main__':main()
