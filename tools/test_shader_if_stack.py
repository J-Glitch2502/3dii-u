#!/usr/bin/env python3
"""Check specialized PICA IF stack accounting against real shader generation."""
from pathlib import Path
import os,shlex,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include "gpu/pica.h"
#include <cassert>
#include <cstdio>
using namespace gpu;
int main(){
 u32 code[22]={};u32 swizzle[128]={};u8 iu[4][4]={};
 for(auto &w:code)w=0x21u<<26; // NOP
 for(u32 k=0;k<8;k++)code[k]=(0x27u<<26)|((20-k)<<10); // eight taken IFU b0 scopes
 code[8]=(0x27u<<26)|(1u<<22)|(11u<<10); // ninth scope only when b1 is true
 code[21]=0x22u<<26;
 auto *p=shader_decode(code,22,swizzle);u32 inputs=0;
 assert(!shader_glsl_spec(p,0,1,iu,&inputs).empty()); // known false b1 consumes no slot
 assert(shader_glsl_spec(p,0,3,iu,&inputs).empty()); // actual ninth push remains rejected
 assert(!shader_glsl_spec(p,0,0,iu,&inputs).empty()); // skipped outer scope
 shader_free(p);
 // Nine condition-code branches nested through their ELSE bodies: at most
 // one IF entry is live, because a false condition pushes nothing.
 u32 otherwise[35];for(auto &w:otherwise)w=0x21u<<26;
 for(u32 k=0;k<9;k++){
  const u32 at=2*k,dst=at+2,end=34-2*k;
  otherwise[at]=(0x28u<<26)|(dst<<10)|(end-dst);
 }
 otherwise[34]=0x22u<<26;
 p=shader_decode(otherwise,35,swizzle);
 assert(!shader_glsl_spec(p,0,0,iu,&inputs).empty());
 shader_free(p);puts("PASS: false and ELSE paths consume no IF slot; a real ninth push stays rejected");
}
'''
def main():
 with tempfile.TemporaryDirectory(prefix='3dii_if_stack_') as tmp:
  tmp=Path(tmp);src=tmp/'test.cpp';src.write_text(HARNESS);exe=tmp/('test.exe' if os.name=='nt' else 'test')
  if os.environ.get('CXX'):
   cxx=shlex.split(os.environ['CXX'])
  elif os.name=='nt':
   sys.path.insert(0,str(ROOT/'tools/win'));import build3ds
   cxx=build3ds.HOST_CXX
  else:
   cxx=['c++']
  subprocess.run(cxx+['-std=c++20','-O1','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'third_party/3dsrecomp/runtime'),str(src),str(ROOT/'third_party/3dsrecomp/runtime/gpu/shader.cpp'),'-Wl,--gc-sections','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
