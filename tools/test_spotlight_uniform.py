"""Spotlight enables share programs; their physical LUT identity remains distinct."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include <algorithm>
#include "shadergen.h"
namespace gpu {
#include "fs_key.inc"
}
int main(){
 gpu::DrawState ds{};ds.need_shade=true;ds.lighting=true;
 ds.light.num_lights=8;ds.light.sp.enabled=true;ds.light.sp.input=4;
 for(unsigned i=0;i<8;i++)ds.light.lights[i].num=i;
 const auto key=gpu::fs_key(gpu::fs_normal(ds));
 const auto source=gpu::gen_fs_glsl(gpu::fs_normal(ds));
 assert(source.find("uLights[7].att.z != 0.0")!=std::string::npos);
 assert(source.find("lutv(15,")!=std::string::npos);
 for(unsigned mask=0;mask<256;mask++){
  for(unsigned i=0;i<8;i++)ds.light.lights[i].spot_atten=mask&(1u<<i);
  assert(gpu::fs_key(gpu::fs_normal(ds))==key);
  assert(gpu::gen_fs_glsl(gpu::fs_normal(ds))==source);
 }
 // An inactive spot can be enabled next draw: retain its LUT row even
 // when distance attenuation does not currently need the physical index.
 ds.light.lights[7].num=6;
 assert(gpu::fs_key(gpu::fs_normal(ds))!=key);
 ds.light.sp.enabled=false;
 auto unlit=gpu::gen_fs_glsl(gpu::fs_normal(ds));
 assert(unlit.find("att.z != 0.0")==std::string::npos);
 for(const auto &l:gpu::fs_normal(ds).light.lights)assert(l.num==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++20','-O1','-ffunction-sections','-fdata-sections','-I'+str(root/'port/wiiu'),'-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),str(root/'port/wiiu/shadergen.cpp'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: all 256 spotlight masks share a program; disabled LUT elimination and physical LUT identity retained')
