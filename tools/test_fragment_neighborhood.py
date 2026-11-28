"""Synthetic predictions and shared C++/Python normalization parity."""
import subprocess,tempfile
from pathlib import Path
from fragment_neighborhood import variants
from tev_to_log import encode,decode,normal,IDENT
root=Path(__file__).resolve().parents[1]
s=dict(head=1025,tex=[16393,0,0],tev=[dict(IDENT,si=0,csrc=[3,0,0],asrc=[3,0,0])],
       light=1,lcfg=2|(1<<15)|(1<<19),lsel=[0]*7,lights=[128,521],fogat=0,blend=3<<24,mask=0)
a=encode(s)
# One non-shadow point light teaches attenuation mode for the shadow light.
made,total=variants([a])
assert len(made)==total and total>0
assert any(decode(k)['tex'][0]==9 for k in made)
assert any(decode(k)['lcfg']&15==1 for k in made)
assert any(decode(k)['lights']==[128,129] for k in made)
assert any(decode(k)['lights']==[128,0] for k in made) # positional shadow light without distance attenuation
# Fog is combined with material/lighting independently; alpha-test bits survive.
b=dict(s,fogat=3 | 4 | (6<<3), tex=[0,0,0])
fogged,_=variants([a,encode(b)])
assert any(decode(k)['tex']==s['tex'] and decode(k)['fogat']==3 and decode(k)['lights']==[128,0] for k in fogged)
assert all(decode(k)['fogat']&~3 in (0,b['fogat']&~3) for k in fogged)
assert variants([a],1)[0]==made[:1] and variants([a],0)[0]==[]
assert variants([a,a])[0]==made
# Do not erase physical light numbers before constructing attenuation variants.
x=normal(s);assert x['lights']==[128,8]
y=dict(s,lsel=[0,0,1,0,0,0,0],lights=[128,897],lcfg=s['lcfg']|(1<<17))
assert normal(y)['lights']==[128,641]
cases=[s,x,y,dict(y,lcfg=y['lcfg']&~(1<<15)),dict(y,lsel=[0]*7)]
cpp=r'''
#include <algorithm>
#include <iostream>
#include "shadergen.h"
namespace gpu {
#include "fs_key.inc"
}
int main(){std::string line;while(std::getline(std::cin,line)){
 std::string key;for(size_t i=0;i<line.size();i+=2)key+=(char)std::stoi(line.substr(i,2),nullptr,16);
 key=gpu::fs_key(gpu::fs_normal(gpu::decode_key(key)));
 const char *hex="0123456789abcdef";for(unsigned char c:key)std::cout<<hex[c>>4]<<hex[c&15];std::cout<<"\n";
}}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++20','-I'+str(root/'port/wiiu'),'-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 result=subprocess.run([str(p/'test')],input=''.join(encode(z).hex()+'\n' for z in cases),text=True,capture_output=True,check=True)
 assert result.stdout.splitlines()==[encode(normal(z)).hex() for z in cases]
print('PASS: bounded deterministic fragment predictions, light identity preserved, C++/Python normalization parity')
