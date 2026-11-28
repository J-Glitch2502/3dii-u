"""Run the actual GLSL generator; preserve lighting while removing idle TEV fetches."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include "shadergen.h"
int main(){
 for(bool lit:{false,true})for(unsigned mask=0;mask<8;mask++){
  auto s=gpu::gen_uber_fs(lit,mask);auto all=gpu::gen_uber_fs(lit);
  assert(s.find("precise float xy = x + y;")!=std::string::npos);
  assert(s.find(" = dot(")==std::string::npos);
  if(lit){assert(s.find("uLight[8]")!=std::string::npos);assert(s.find("if (uLit.x > 7) light(7);")!=std::string::npos);}
  assert(s.substr(0,s.find("void main()"))==all.substr(0,all.find("void main()")));
  for(unsigned i=0;i<3;i++){
   auto n=std::to_string(i);
   auto expected="tx"+n+" = "+(mask&(1<<i)?"sampled("+n+", u"+n+", texture(t"+n+", u"+n+"))":"uTexAdd["+n+"]")+";";
   assert(s.find(expected)!=std::string::npos);
  }
  assert(gpu::uber_family_key(lit,mask)!=gpu::uber_family_key(!lit,mask));
 }
 assert(gpu::gen_uber_fs(true,7)==gpu::gen_uber_fs(true));
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++20','-O1','-ffunction-sections','-fdata-sections','-I'+str(root/'port/wiiu'),'-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),str(root/'port/wiiu/shadergen.cpp'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: all 16 fallback families, unused/null TEV fetch removal, full-lighting source preservation, legacy full-mask identity')
