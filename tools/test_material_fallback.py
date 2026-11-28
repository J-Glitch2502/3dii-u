"""Material families must ignore only settings still read dynamically by the shader."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include "shadergen.h"
int main(){
 gpu::DrawState s{};s.need_shade=true;s.lighting=true;s.light.num_lights=1;
 s.need_tex[0]=true;s.tev_count=1;s.tev_list[0]=2;
 auto &t=s.tev[2];t.csrc[0]=3;t.asrc[0]=3;t.csrc[1]=1;t.asrc[1]=0;
 t.cop=t.aop=1;t.cmul=t.amul=1;
 auto key=gpu::uber_material_key(s,1);auto code=gpu::gen_uber_material_fs(s,1);
 assert(code.find("uKonst[2]")!=std::string::npos);
 assert(code.find("uTevInit[0]")!=std::string::npos);
 assert(code.find("combine_colour(uCsrc[i].w")!=std::string::npos); // unused generic helper may remain, compiler removes it
 auto main=code.substr(code.find("void main()"));
 assert(main.find("stage(")==std::string::npos);
 assert(main.find("// stage 2")!=std::string::npos);
 assert(main.find("vec3 r = (cs0 * cs1)")!=std::string::npos);
 auto dynamic=s;dynamic.alpha_test=true;dynamic.alpha_func=4;dynamic.fog=true;dynamic.fog_flip=true;
 dynamic.tex[0].coord=2;dynamic.tex[0].wrap_s=5;dynamic.light.lights[0].directional=true;
 dynamic.light.global_ambient[0]=.8f;dynamic.tev[2].konst=0x11223344;
 assert(gpu::uber_material_key(dynamic,1)==key);
 assert(gpu::gen_uber_material_fs(dynamic,1)==code);
 auto unused=s;unused.tev[2].csrc[2]=6;unused.tev[2].cmod[2]=13;
 assert(gpu::uber_material_key(unused,1)==key);
 assert(gpu::gen_uber_material_fs(unused,1)==code);
 auto lightkey=gpu::uber_lighting_material_key(s,1);
 auto lightcode=gpu::gen_uber_lighting_material_fs(s,1);
 assert(lightkey!=0 && lightkey!=key);
 assert(lightcode.substr(lightcode.find("void main()")).find("lighting();")==std::string::npos);
 assert(gpu::uber_lighting_material_key(dynamic,1)!=lightkey); // direction is a structural lighting feature
 auto uniform=s;uniform.light.lights[0].pos[0]=4.0f;uniform.light.lights[0].diffuse[1]=.3f;
 uniform.alpha_test=true;uniform.fog=true;uniform.tex[0].wrap_s=5;
 uniform.light.lights[0].spot_atten=true;
 assert(gpu::uber_lighting_material_key(uniform,1)==lightkey);
 assert(gpu::gen_uber_lighting_material_fs(uniform,1)==lightcode);
 auto special=s;special.light.d0.enabled=true;
 assert(gpu::uber_lighting_material_key(special,1)!=lightkey);
 special=s;special.light.num_lights=2;
 assert(gpu::uber_lighting_material_key(special,2)!=lightkey);
 auto changed=s;changed.tev[2].cop=2;
 assert(gpu::uber_material_key(changed,1)!=key);
 changed=s;changed.tev_list[0]=3;changed.tev[3]=s.tev[2];
 assert(gpu::uber_material_key(changed,1)!=key);
 assert(gpu::uber_material_key(s,2)!=key);
 changed=s;changed.tev[2].csrc[0]=6;
 assert(gpu::gen_uber_material_fs(changed,1).empty());
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++20','-O1','-ffunction-sections','-fdata-sections','-I'+str(root/'port/wiiu'),'-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),str(root/'port/wiiu/shadergen.cpp'),str(root/'port/wiiu/shaderkeys.cpp'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: material key tracks program/stage/texture/light capacity while fog, alpha, uniforms and lighting flags stay dynamic')
