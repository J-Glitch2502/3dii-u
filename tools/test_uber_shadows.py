"""Compare shadow fallback generation with the actual specialized generator."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include "shadergen.h"
int main(){
 const std::string depth="b.a * 0.99609375 + b.b * 0.00389099 + b.g * 0.0000152";
 for(bool lit:{false,true}){
  auto fallback=gpu::gen_uber_fs(lit,1);
  assert(fallback.find("if (uWrap[0].z == 2)")!=std::string::npos);
  assert(fallback.find("uWrap[0].w != 0 ? c0 : c0 / vTc2w.z")!=std::string::npos);
  assert(fallback.find("textureLod(t0, uvs, 0.0)")!=std::string::npos);
  assert(fallback.find(depth)!=std::string::npos);
  assert(fallback.find("tx0 = vec4(zref >= zz ? b.r : 0.0)")!=std::string::npos);
  if(lit){assert(fallback.find("dist == 0.0 && !alpha_light")!=std::string::npos);}
  assert(gpu::gen_uber_fs(lit,0).find("if (uWrap[0].z == 2)")==std::string::npos);
  for(bool ortho:{false,true}){
   gpu::DrawState ds{};ds.need_shade=true;ds.need_tex[0]=true;ds.tex[0].type=2;ds.tex[0].shadow_ortho=ortho;
   auto specialized=gpu::gen_fs_glsl(ds);
   assert(specialized.find(depth)!=std::string::npos);
   assert(specialized.find("tx0 = vec4(zref >= zz ? b.r : 0.0)")!=std::string::npos);
   assert(specialized.find(ortho ? "uvs = c0;" : "uvs = c0 / tw;")!=std::string::npos);
  }
 }
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++20','-O1','-ffunction-sections','-fdata-sections','-I'+str(root/'port/wiiu'),'-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),str(root/'port/wiiu/shadergen.cpp'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: perspective/orthographic 2D shadow fallback matches specialized depth decode and comparison; Fresnel guard retained')
