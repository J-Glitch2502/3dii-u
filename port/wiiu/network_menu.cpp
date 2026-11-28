#include "network_menu.h"
#include "network_menu_model.h"
#include "network_settings.h"
#include "network_keyboard.h"
#include "wiiu.h"
#include "../../third_party/3dsrecomp/runtime/loader.h"
#include "azl/room_proto.hpp"
#include <vpad/input.h>
#include <mutex>
#include <atomic>
#include <cctype>
#include <functional>
namespace wiiu::network_menu {
namespace {
std::mutex mutex;
std::atomic<bool> shown{false};
enum class Page{Settings,Rooms,Filters,Connected,Members,Chat};
enum class Edit{None,Host,Port,Api,BrowseApi,Username,Token,Nickname,Search,Password,Chat};
Page page=Page::Settings;Edit edit=Edit::None;
network::Room password_room;
network::ListGesture list;
int selected=0,minimum=0,maximum=0,sort=0;
bool only_game=false,hide_full=false,hide_empty=false;
std::string search;
std::string lower(std::string s){for(char &c:s)c=std::tolower(static_cast<unsigned char>(c));return s;}
std::vector<network::Room> filtered(const network::View &v){
    std::vector<network::Room> rooms;
    for(auto r:v.rooms){
        if(only_game&&r.game_id!=g_game.program_id)continue;
        if((hide_empty&&!r.players)||(hide_full&&r.players>=r.capacity)||r.players<unsigned(minimum)||(maximum&&r.players>unsigned(maximum)))continue;
        if(!search.empty()&&lower(r.name+" "+r.game).find(lower(search))==std::string::npos)continue;
        rooms.push_back(std::move(r));
    }
    std::stable_sort(rooms.begin(),rooms.end(),[](const auto&a,const auto&b){
        if(sort==1)return a.players>b.players;
        if(sort==2)return (a.ping_ms<0?999999:a.ping_ms)<(b.ping_ms<0?999999:b.ping_ms);
        return lower(a.name)<lower(b.name);
    });return rooms;
}
void navigate(Page next){page=next;selected=0;list={};}
void request(Edit e,const char *label,const std::string &text,unsigned max,bool secret=false,bool numeric=false){
    if(wiiu::keyboard_open()){network::report("Close the game keyboard before editing network settings.");return;}
    edit=e;network_keyboard::request(label,text,max,secret,numeric);
}
void completed(){
    std::string text;bool accepted=false;
    if(!network_keyboard::take(text,accepted))return;
    Edit which=edit;edit=Edit::None;if(!accepted)return;
    auto c=network::view().config;
    switch(which){
    case Edit::Host:c.host=text;break;
    case Edit::Port:if(!network::port_number(text,c.port)){network::report("Port must be between 1 and 65535.");return;}break;
    case Edit::BrowseApi:
        c.web_api_url=text;
        if(auto error=network::validate(c);!error.empty()){network::report(error);return;}
        network::apply(c);navigate(Page::Rooms);network::browse();return;
    case Edit::Api:c.web_api_url=text;break;
    case Edit::Username:c.web_api_username=text;break;
    case Edit::Token:c.network_token=text;break;
    case Edit::Nickname:
        if(text.empty())text="auto";
        if(text!="auto"&&(text.size()<4||text.size()>20||text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-")!=std::string::npos)){
            network::report("Nickname needs 4-20 letters, numbers, spaces, '.', '_' or '-'. Blank uses your Mii.");return;
        }c.nickname=text;break;
    case Edit::Search:search=text;selected=0;list={};return;
    case Edit::Password:network::join(password_room,text);navigate(Page::Connected);return;
    case Edit::Chat:network::say(text);return;
    default:return;
    }
    network::apply(c);
}
View build(){
    View out;auto v=network::view(page==Page::Rooms);out.selected=selected;out.scroll=list.offset;out.status=v.status;
    out.hint="A: choose   B: back   Right stick / touch: scroll. Gameplay continues online.";
    const auto &c=v.config;
    switch(page){
    case Page::Settings:
        out.title="Network settings";
        out.rows={{"Wireless communication",c.enabled?"On":"Off"},{"Server address",c.host.empty()?"Not set":c.host},{"Server port",std::to_string(c.port)},
        {"Connect to server","A: join / reconnect"},{"Browse public rooms",""},{"Current room / players / chat",network::live().status},
        {"Room nickname",c.nickname.empty()||c.nickname=="auto"||c.nickname=="mii"?"Use active Mii":c.nickname},
        {"Network Web API URL",c.web_api_url.empty()?"Not set":c.web_api_url},
        {"Web API username",c.web_api_username},{"Network token",c.network_token.empty()?"Not set":"********"},{"GamePad chat",c.chat_side=="left"?"Left":c.chat_side=="off"?"Off":"Right"}};break;
    case Page::Rooms:{
        auto rooms=filtered(v);out.title="Public rooms ("+std::to_string(rooms.size())+")";
        out.rows.push_back({"Filters / search / sort",""});out.rows.push_back({"Refresh room list",""});
        for(const auto&r:rooms){
            std::string value=std::to_string(r.players)+"/"+std::to_string(r.capacity)+(r.password?"  [password]":"");
            if(r.ping_ms>=0)value+="  "+std::to_string(r.ping_ms)+" ms";
            if(r.version!=azl::room::kNetworkVersion)value+="  [incompatible]";
            out.rows.push_back({r.name+(r.game.empty()?"":" - "+r.game),value,r.version==azl::room::kNetworkVersion});
        }out.hint="A: join   Right: ping selected room   B: back   Touch / right stick: scroll";break;
    }
    case Page::Filters:
        out.title="Room filters";
        out.rows={{"Search",search},{"Only this game",only_game?"On":"Off"},{"Hide full rooms",hide_full?"On":"Off"},
        {"Hide empty rooms",hide_empty?"On":"Off"},{"Minimum players",std::to_string(minimum)},
        {"Maximum players",maximum?std::to_string(maximum):"Any"},{"Sort by",sort==1?"Most players":sort==2?"Measured ping":"Room name"},{"Reset filters",""}};break;
    case Page::Connected:{auto live=network::live();out.title=live.name.empty()?"Current room":live.name;out.status=live.status;
        out.rows={{"Players in room",std::to_string(live.members.size())},{"Room chat",""},{"Send chat message",""},{"Disconnect from room",""}};break;}
    case Page::Members:{auto live=network::live();out.title="Players ("+std::to_string(live.members.size())+")";
        for(const auto &s:live.members)out.rows.push_back({s,""});if(out.rows.empty())out.status="You are not in a room.";break;}
    case Page::Chat:{out.title="Room chat";out.rows.push_back({"Send a message",""});for(const auto &s:network::live().chat)out.rows.push_back({s,""});break;}
    }
    return out;
}
void activate(int dir=1,bool ping=false){
    auto v=network::view(page==Page::Rooms);auto c=v.config;
    if(page==Page::Settings){switch(selected){
    case 0:c.enabled=!c.enabled;network::apply(c);break;
    case 1:if(ping){network::Room r;r.host=c.host;r.port=c.port;network::ping(r);}else request(Edit::Host,"Room hostname or IPv4 address",c.host,253);break;
    case 2:request(Edit::Port,"Room UDP port",std::to_string(c.port),5,false,true);break;
    case 3:network::apply(c,true);navigate(Page::Connected);break;
    case 4:
        if(!c.enabled){network::report("Turn wireless communication on first.");break;}
        if(c.web_api_url.empty())request(Edit::BrowseApi,"Public room Web API URL",c.web_api_url,512);
        else {navigate(Page::Rooms);network::browse();}break;
    case 5:navigate(Page::Connected);break;
    case 6:request(Edit::Nickname,"Room nickname (blank uses Mii)",c.nickname=="auto"?"":c.nickname,20);break;
    case 7:request(Edit::Api,"Community Web API URL",c.web_api_url,512);break;
    case 8:request(Edit::Username,"Web API username",c.web_api_username,64);break;
    case 9:request(Edit::Token,"Network token",c.network_token,512,true);break;
    case 10:{
        static const char *const sides[]={"off","left","right"};
        const int current=c.chat_side=="left"?1:c.chat_side=="right"?2:0;
        c.chat_side=sides[(current+(dir<0?2:1))%3];network::apply(c);break;
    }
    }}else if(page==Page::Rooms){
        if(selected==0){navigate(Page::Filters);return;}if(selected==1){
            if(c.web_api_url.empty())request(Edit::BrowseApi,"Public room Web API URL",c.web_api_url,512);
            else network::browse();return;}
        auto rooms=filtered(v);int i=selected-2;if(i<0||i>=int(rooms.size()))return;
        auto r=rooms[i];if(ping){network::ping(r);return;}
        if(r.version!=azl::room::kNetworkVersion){network::report("This room uses an incompatible network protocol.");return;}
        if(r.password){password_room=r;request(Edit::Password,"Room password","",128,true);}else{network::join(r,{});navigate(Page::Connected);}
    }else if(page==Page::Filters){switch(selected){
        case 0:request(Edit::Search,"Search rooms or games",search,128);break;
        case 1:only_game=!only_game;break;case 2:hide_full=!hide_full;break;case 3:hide_empty=!hide_empty;break;
        case 4:minimum=std::clamp(minimum+dir,0,100);if(maximum&&maximum<minimum)maximum=minimum;break;
        case 5:maximum=std::clamp(maximum+dir,0,100);if(maximum&&minimum>maximum)minimum=maximum;break;
        case 6:sort=(sort+3+dir)%3;break;
        case 7:search.clear();minimum=maximum=sort=0;only_game=hide_full=hide_empty=false;break;
    }}else if(page==Page::Connected){
        if(selected==0)navigate(Page::Members);else if(selected==1)navigate(Page::Chat);
        else if(selected==2)request(Edit::Chat,"Send a room chat message","",128);
        else if(selected==3)network::disconnect();
    }else if(page==Page::Chat&&selected==0)request(Edit::Chat,"Send a room chat message","",128);
}
}
void open(){std::lock_guard lock(mutex);shown=true;navigate(Page::Settings);}
void close(){std::lock_guard lock(mutex);shown=false;}
bool active(){return shown.load();}
void buttons(uint32_t pressed){
    if(!shown)return;std::lock_guard lock(mutex);completed();if(network_keyboard::active())return;
    if(pressed&VPAD_BUTTON_B){
        if(page==Page::Settings){shown=false;return;}
        navigate(page==Page::Filters?Page::Rooms:page==Page::Members||page==Page::Chat?Page::Connected:Page::Settings);return;
    }
    auto rows=build().rows;int count=rows.size();if(!count)return;
    selected=std::clamp(selected,0,count-1);
    if(pressed&VPAD_BUTTON_DOWN){selected=std::min(count-1,selected+1);list.reveal(selected,count);}
    if(pressed&VPAD_BUTTON_UP){selected=std::max(0,selected-1);list.reveal(selected,count);}
    if(pressed&VPAD_BUTTON_RIGHT){if(page==Page::Rooms||page==Page::Settings&&selected==1)activate(1,true);else if(page==Page::Filters||page==Page::Settings&&(selected==0||selected==10))activate(1);}
    if(pressed&VPAD_BUTTON_LEFT&&(page==Page::Filters||page==Page::Settings&&(selected==0||selected==10)))activate(-1);
    if(pressed&VPAD_BUTTON_A)activate();
}
void pointer(bool down,float x,float y,float right_y,float seconds){
    if(!shown)return;std::lock_guard lock(mutex);completed();if(network_keyboard::active())return;
    auto rows=build().rows;auto tap=list.input(down,x,y,right_y,seconds,rows.size());
    if(tap){selected=*tap;activate();}
}
View view(){std::lock_guard lock(mutex);auto out=build();if(!out.rows.empty())selected=std::clamp(selected,0,int(out.rows.size())-1);list.clamp(out.rows.size());out.selected=selected;out.scroll=list.offset;return out;}
}
