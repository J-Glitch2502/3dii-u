#pragma once
#include "screen_layouts.h"
namespace wiiu::network {
// Reserve only space outside every visible game screen, never a scaled/cropped screen edge.
inline bool chat_border(int layout,bool left,uint32_t w,uint32_t h,screens::Rect &box){
    uint32_t x0=w,y0=h,x1=0,y1=0;bool any=false;
    for(int which=0;which<2;which++){
        screens::Rect r,own;if(!screens::pad_rect(layout,which,w,h,r,own))continue;
        x0=std::min(x0,r.x);y0=std::min(y0,r.y);x1=std::max(x1,r.x+r.w);y1=std::max(y1,r.y+r.h);any=true;
    }
    if(!any)return false;
    uint32_t side=left?x0:w-x1;
    if(side>=90){box={left?6:x1+6,6,side-12,h-12};return true;}
    uint32_t vertical=h-y1;bool above=false;
    if(y0>vertical){vertical=y0;above=true;}
    if(vertical<64)return false;
    box={left?6:w/2+6,above?6:y1+6,w/2-12,vertical-12};return true;
}
}
