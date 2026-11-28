#include "../port/wiiu/network_config.h"
#include "../port/wiiu/network_menu_model.h"
#include "../port/wiiu/chat_border.h"
#include <cassert>
#include <iostream>
using namespace wiiu::network;
int main(){
    Config c;std::string error;
    assert(!c.enabled&&c.web_api_url==kDefaultWebApi);
    assert(parse("host=example.org\r\nport=64000\r\nnickname=auto\r\n",c,error));
    assert(!c.enabled&&c.port==64000&&c.web_api_url==kDefaultWebApi); // Older files default off, too.
    c.enabled=true;c.web_api_url="https://example.org/api";c.network_token="secret";c.extra["future_setting"]="yes";
    Config restored;assert(parse(serialize(c),restored,error));assert(restored.enabled&&restored.network_token=="secret"&&restored.extra.at("future_setting")=="yes");
    for(const auto &s:{"port=0","port=65536","port=12x","enabled=yes","host=example.org:4","web_api_url=ftp://example.org","web_api_url=https://user:token@example.org"})assert(!parse(s,restored,error));
    assert(!header_valid("value\r\nx-token: leak"));
    assert(api_valid("http://127.0.0.1:5000"));assert(!api_valid("http://:1"));
    ListGesture g;
    assert(!g.input(true,50,120,0,.016,1000));auto tap=g.input(false,50,120,0,.016,1000);assert(tap&&*tap==0);
    g={};g.input(true,50,300,0,.016,1000);g.input(true,50,180,0,.016,1000);assert(g.offset>100);assert(!g.input(false,50,180,0,.016,1000));
    g={};g.input(true,832,400,0,.016,1000);assert(g.offset>40000);assert(!g.input(false,832,400,0,.016,1000));
    g={};g.input(false,0,0,-1,.05,1000);assert(g.offset>0);g.reveal(999,1000);assert(g.offset>47000);g.clamp(2);assert(g.offset==0);
    g={};g.input(true,20,120,0,.016,1000);assert(!g.input(false,20,120,0,.016,1000));
    // Every card must remain wholly outside both screens in all five layouts.
    for(int layout=0;layout<screens::kPadLayouts;layout++)for(bool left:{false,true}) {
        screens::Rect box{};bool visible=chat_border(layout,left,854,480,box);
        assert(visible==(layout==0||layout==2||layout==3));
        if(!visible)continue;
        assert(box.w>=90&&box.h>=52&&box.x+box.w<=854&&box.y+box.h<=480);
        if(left)assert(box.x<427);else assert(box.x>=427);
        for(int which=0;which<2;which++) {
            screens::Rect game,own;
            if(screens::pad_rect(layout,which,854,480,game,own))
                assert(box.x+box.w<=game.x||game.x+game.w<=box.x||box.y+box.h<=game.y||game.y+game.h<=box.y);
        }
    }
    std::cout<<"network configuration and scroll/gesture checks passed\n";
}
