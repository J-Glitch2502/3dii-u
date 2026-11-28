#include "../port/wiiu/network_lobby.h"
#include <cassert>
#include <iostream>
using namespace wiiu::network;
int main(int argc,char **argv){
    std::vector<Room> rooms;std::string error;
    assert(!decode_rooms("<html>not a directory</html>",rooms,error));
    assert(!decode_rooms("{\"rooms\":1}",rooms,error));
    assert(decode_rooms(R"({"rooms":[{"name":"Test Room","address":"127.0.0.1","port":24872,"maxPlayers":8,"netVersion":4,"hasPassword":false,"preferredGameName":"MARIO KART 7","preferredGameId":1125899906842624,"players":[{}]},
    {"name":"Invalid port","address":"127.0.0.1","port":65536,"maxPlayers":8},
    {"name":"Wrong shape","address":false,"port":123,"maxPlayers":8}]})",rooms,error));
    assert(rooms.size()==1&&rooms[0].port==24872&&rooms[0].players==1&&rooms[0].game=="MARIO KART 7");
    assert(!decode_rooms(std::string(25,'[')+std::string(25,']'),rooms,error));
    assert(!decode_rooms(std::string((1<<20)+1,' '),rooms,error));
    if(argc>1){
        Config c;c.web_api_url=argv[1];std::atomic<bool> cancel{false};
        auto reply=web_request(c,"/lobby","GET",cancel);assert(reply.ok);
        assert(decode_rooms(reply.body,rooms,error));
        std::cout<<"HTTP directory: "<<rooms.size()<<" valid rooms\n";
        if(argc>2){auto bad=web_request(c,"/missing","GET",cancel);assert(!bad.ok&&bad.status==404);
          cancel=true;assert(!web_request(c,"/lobby","GET",cancel).ok);}
    }
    std::cout<<"network lobby checks passed\n";
}
