#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
namespace wiiu::network {
// Geometry is in the same 854x480 space as the calibrated GamePad touch point.
struct ListGesture {
    float offset = 0;
    bool touched = false, dragging = false, bar = false;
    float start_y = 0, previous_y = 0, distance = 0;
    int pressed_row = -1;
    static constexpr float top=100,bottom=408,row=48,left=24,right=812,scrollbar=818;
    float max_scroll(int count)const{return std::max(0.f,count*row-(bottom-top));}
    void clamp(int count){offset=std::clamp(offset,0.f,max_scroll(count));}
    void reveal(int selected,int count){offset=std::clamp(offset,selected*row-(bottom-top-row),selected*row);clamp(count);}
    std::optional<int> input(bool down,float x,float y,float axis,float seconds,int count){
        std::optional<int> tap;
        if(!down&&std::abs(axis)>0.2f)offset-=axis*700.f*std::clamp(seconds,0.f,0.05f);
        if(down&&!touched){
            bar=x>=scrollbar&&x<=850&&y>=top&&y<=bottom;
            dragging=false;distance=0;start_y=previous_y=y;
            pressed_row=x>=left&&x<right&&y>=top&&y<=bottom?int((offset+y-top)/row):-1;
        }
        if(down){
            if(bar){
                float height=bottom-top,thumb=std::clamp(height*height/std::max(height,count*row),24.f,height);
                float fraction=(y-top-thumb/2)/std::max(1.f,height-thumb);
                offset=std::clamp(fraction,0.f,1.f)*max_scroll(count);dragging=true;
            }else if(pressed_row>=0){
                distance=std::max(distance,std::abs(y-start_y));
                if(distance>8)dragging=true;
                if(dragging)offset-=y-previous_y;
            }
            previous_y=y;
        }else if(touched&&!dragging&&pressed_row>=0&&pressed_row<count&&y>=top&&y<=bottom&&x>=left&&x<right){
            int released=int((offset+y-top)/row);if(released==pressed_row)tap=released;
        }
        touched=down;clamp(count);return tap;
    }
};
}
