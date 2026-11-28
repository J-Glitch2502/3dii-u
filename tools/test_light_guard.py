"""Zero-distance pruning retains the last light's attenuation-independent alpha."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include "shadergen.h"
unsigned guards(const std::string &s){unsigned n=0;for(size_t at=0;(at=s.find("if (uLscale1.w == 0.0 || dist != 0.0)",at))!=std::string::npos;at++)n++;return n;}
int main(){
 gpu::DrawState ds{};ds.need_shade=true;ds.lighting=true;ds.light.num_lights=8;
 for(auto &l:ds.light.lights)l.dist_atten=true;
 assert(guards(gpu::gen_fs_glsl(ds))==8);
 ds.light.fr.enabled=true;ds.light.primary_alpha=true;
 auto s=gpu::gen_fs_glsl(ds);assert(guards(s)==7);
 auto last=s.substr(s.find("// light 7"));assert(guards(last)==0);assert(last.find("dsum.a = fr;")!=std::string::npos);
 ds.light.primary_alpha=false;ds.light.secondary_alpha=true;
 s=gpu::gen_fs_glsl(ds);assert(guards(s)==7);assert(s.find("ssum.a = fr;")!=std::string::npos);
 ds.light.secondary_alpha=false;assert(guards(gpu::gen_fs_glsl(ds))==8);
 ds.light.fr.enabled=false;ds.light.primary_alpha=true;assert(guards(gpu::gen_fs_glsl(ds))==8);
 for(auto &l:ds.light.lights)l.dist_atten=false;
 assert(guards(gpu::gen_fs_glsl(ds))==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++20','-O1','-ffunction-sections','-fdata-sections','-I'+str(root/'port/wiiu'),'-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),str(root/'port/wiiu/shadergen.cpp'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: distance guard keeps Fresnel alpha; inactive distance attenuation receives no guard')
