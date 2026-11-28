#pragma once
#include "network_lobby.h"
namespace wiiu::network {
struct View { Config config; std::vector<Room> rooms; std::string status; bool busy = false; };
struct Live { bool joined = false; std::string name, status, nickname; std::vector<std::string> members, chat; };
struct ChatLine { uint32_t when; std::string text; };
std::vector<ChatLine> chat();
int chat_side(); // 0 off, 1 left, 2 right
Live live();
void say(const std::string &);
void disconnect();
bool initialize_sockets();
void initialize();
void start();
bool shutdown();
View view(bool include_rooms=false);
bool wireless_enabled();
bool connected();
void connection_changed(bool);
void apply(const Config &,bool reconnect=false);
void browse();
void join(const Room &, const std::string &password);
void ping(const Room &);
void report(const std::string &);
}
