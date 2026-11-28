#pragma once
#include "network_config.h"
#include <atomic>
#include <vector>
namespace wiiu::network {
struct Room {
    std::string name, host, game, verify_uid;
    unsigned long long game_id = 0;
    unsigned port = 0, players = 0, capacity = 0, version = 0;
    bool password = false;
    int ping_ms = -1;
};
struct HttpResult { bool ok = false; std::string body, error; long status = 0; };
HttpResult web_request(const Config &, const std::string &path, const char *method,
                       const std::atomic<bool> &cancel, const std::string &bearer = {}, bool credentials = false);
bool decode_rooms(const std::string &, std::vector<Room> &, std::string &error);
std::string room_authentication(const Config &, const Room &, const std::atomic<bool> &, std::string &error);
}
