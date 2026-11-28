"""Fractional row/column copies cover adjacent regions without gaps."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include <cmath>
#include "scaled_copy.h"
int main(){
 for(float scale:{0.75f,0.96153846f,1.0f,1.9230769f,2.8846154f,3.0f}){
  for(unsigned total:{240u,400u,416u,512u}){
   for(unsigned chunk:{1u,7u,16u,64u,128u}){
    unsigned end=0,sum=0;
    for(unsigned first=0;first<total;first+=chunk){
     auto span=gx2_copy::span(first,std::min(chunk,total-first),scale);
     assert(span.first==end);sum+=span.size();end=span.last;
    }
    assert(sum==static_cast<unsigned>(total*scale));
   }
  }
 }
 // The observed Fast-mode raster scissors split a padded target at x=128.
 // Separately flooring the first rectangle's width leaves one black pixel.
 float fast = (720.0f / 416) * (5.0f / 3) * (2.0f / 3) / 2;
 auto left=gx2_copy::span(8,120,fast), right=gx2_copy::span(128,120,fast);
 assert(left.last==right.first);
 assert(static_cast<unsigned>(8*fast)+static_cast<unsigned>(120*fast)<right.first);
 // The captured Fast source had column 230 black even though its centre
 // lies inside the transformed raster clip ending at 240 * scale + .25.
 auto raster=gx2_copy::raster_span(0,240,fast,.25f);
 assert(static_cast<unsigned>(240*fast)==230 && raster.last==231);
 for(float nudge:{0.f,.25f,.5f}) for(float scale:{.75f,.96153846f,1.f,1.9230769f,3.f}){
  auto whole=gx2_copy::raster_span(8,240,scale,nudge);
  auto a=gx2_copy::raster_span(8,120,scale,nudge),b=gx2_copy::raster_span(128,120,scale,nudge);
  assert(a.first==whole.first && a.last==b.first && b.last==whole.last);
  for(unsigned pixel=0;pixel<1000;pixel++){
   const float centre=pixel+.5f;
   bool inside=centre>=8*scale+nudge && centre<248*scale+nudge;
   assert(inside==(pixel>=whole.first && pixel<whole.last));
  }
 }
 // Source padding uses rounded physical extents, not logical tex_h ratios.
 const float scale=1.9230769f;unsigned height=416,texheight=512;
 unsigned sh=texheight*scale,row0=sh-static_cast<unsigned>(height*scale);
 auto s=gx2_copy::span(0,height,scale);
 assert(gx2_copy::uv(row0+s.last,sh)==1.0f);
 assert(gx2_copy::uv(row0+s.first,sh)>0.0f);
 assert(std::abs(gx2_copy::uv(row0,sh)-float(texheight-height)/texheight)>0.0001f);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text('#include <algorithm>\n#include <initializer_list>\n'+cpp)
 subprocess.run(['c++','-std=c++20','-I'+str(root/'port/wiiu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: adjacent fractional copies cover their complete extent; source UVs respect rounded padding')
