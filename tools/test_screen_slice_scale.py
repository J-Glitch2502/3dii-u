"""Recognize only a proved 2:1 half-screen transfer at the affected setting."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'port/wiiu/gx2_backend.cpp').read_text();a=s.index('float screen_slice_scale(');b=s.index('\n}',a)+2
cpp='using u32=unsigned;\n'+s[a:b]+'''
#include <cassert>
int main(){
 assert(screen_slice_scale(256,416,128,416,1,480)==2);
 assert(screen_slice_scale(240,400,120,400,1,480)==2);
 assert(screen_slice_scale(256,416,128,416,1,720)==0);
 assert(screen_slice_scale(256,416,128,416,1,1080)==0);
 assert(screen_slice_scale(256,320,128,320,1,480)==0);
 assert(screen_slice_scale(256,416,128,208,2,480)==0);
 assert(screen_slice_scale(256,416,256,416,0,480)==0);
 assert(screen_slice_scale(512,800,256,800,1,480)==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: selected-480p 2:1 screen slices only; other resolutions, bottom screens and effect downscales excluded')
