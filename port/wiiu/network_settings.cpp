#include "network_settings.h"
#include "../../third_party/3dsrecomp/runtime/services/uds_service.h"
#include "azl/platform.hpp"
#include "enet/enet.h"
#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <filesystem>
namespace wiiu::network {
namespace {
constexpr const char *path = "fs:/vol/external01/3dii-u/multiplayer-room.txt";
enum class Action { Apply, Browse, Join, Ping, Disconnect, Save };
struct Command { Action action; Config config; Room room; std::string password; unsigned serial; };
struct State {
    std::mutex mutex; View current; std::optional<Command> pending;
    std::atomic<bool> enabled{false}, stop{false}, cancel{false}, connected{false};
    std::condition_variable changed;
    std::atomic<bool> started{false};
    std::atomic<int> chat_side{2};
    unsigned serial = 0; azl::platform::Thread worker;
};
State &state() { static State *s = new State; return *s; }
bool persist(const Config &c) {
    std::error_code error;
    std::filesystem::create_directories("fs:/vol/external01/3dii-u", error);
    if (error) return false;
    std::ofstream f(std::string(path) + ".tmp", std::ios::binary | std::ios::trunc);
    const auto text = serialize(c); f.write(text.data(), text.size()); f.flush(); f.close();
    if (!f) { std::remove((std::string(path) + ".tmp").c_str()); return false; }
    if (std::rename((std::string(path) + ".tmp").c_str(), path) != 0) {
        std::remove((std::string(path) + ".tmp").c_str()); return false;
    }
    return true;
}
void queue(Action action, const Config &c, const Room &r = {}, const std::string &password = {}) {
    start();
    auto &s = state(); std::lock_guard lock(s.mutex);
    if (s.stop || !s.started) {s.current.busy=false;return;}
    s.cancel = true;
    s.pending = Command{action,c,r,password,++s.serial};
    s.changed.notify_one();
    s.current.busy = true;
    s.current.status = action == Action::Browse ? "Loading public rooms..." : action == Action::Ping ? "Measuring server ping..." : "Applying network settings...";
}
std::string ping_server(const Room &r, std::atomic<bool> &cancel, int &ms) {
    ms = -1;
    ENetAddress address{}; address.port = r.port;
    if (enet_address_set_host(&address, r.host.c_str()) < 0) return "Could not resolve this server.";
    if (cancel) return "Ping cancelled.";
    ENetHost *host = enet_host_create(nullptr,1,1,0,0);
    if (!host) return "Could not create a ping socket.";
    ENetPeer *peer = enet_host_connect(host,&address,1,0);
    const auto begin = azl::platform::NowMs();
    ENetEvent event{};
    bool connected = false;
    if (peer) while (!cancel && azl::platform::NowMs()-begin<3000) {
        int result = enet_host_service(host,&event,20);
        if (result<0) break;
        if (result>0 && event.type==ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        if (result>0 && event.type==ENET_EVENT_TYPE_CONNECT) { connected=true; ms=azl::platform::NowMs()-begin; break; }
    }
    if (peer) enet_peer_disconnect_now(peer,0);
    enet_host_destroy(host);
    return connected ? "Server round trip: " + std::to_string(ms) + " ms" : cancel ? "Ping cancelled." : "No ENet response within 3 seconds.";
}
void run() {
    auto &s=state();
    while (!s.stop) {
        std::optional<Command> command;
        { std::unique_lock lock(s.mutex); s.changed.wait(lock,[&]{return s.stop || s.pending.has_value();});
          if(s.stop) break; command=std::move(s.pending);s.pending.reset();s.cancel=false; }
        const auto &c=*command;
        std::string message; std::vector<Room> rooms; bool replace_rooms=false; int measured=-1;
        if(c.config.enabled && c.action!=Action::Save && c.action!=Action::Disconnect && !initialize_sockets()) {
            message="Could not initialize Wii U networking.";
        } else if (c.action==Action::Apply || c.action==Action::Join) {
            std::string token;
            if (c.action==Action::Join) token=room_authentication(c.config,c.room,s.cancel,message);
            if (message.empty() && !s.cancel) {
                if(!c.config.enabled) {
                    s.enabled=false;message=kernel::configure_room_uds(c.config,{});
                    if(!persist(c.config))message+=" Could not save the choice to SD.";
                } else if (!persist(c.config)) message="Could not save network settings to SD; connection was not changed.";
                else {
                    bool current;
                    {std::lock_guard lock(s.mutex);current=c.serial==s.serial&&!s.stop;if(current)s.enabled=c.config.enabled;}
                    if(current)message=kernel::configure_room_uds(c.config,token);
                }
            }
        } else if(c.action==Action::Save) message=persist(c.config)?"Network settings saved.":"Could not save network settings to SD.";
        else if(c.action==Action::Disconnect) message=kernel::disconnect_room_uds();
        else if (c.action==Action::Browse) {
            // A refresh may replace a queued settings-save command. Persist
            // the latest configuration here as well before fetching its URL.
            const bool saved=persist(c.config);
            auto reply=web_request(c.config,"/lobby","GET",s.cancel);
            if (!reply.ok) message=reply.error;
            else if (decode_rooms(reply.body,rooms,message)) {replace_rooms=true;message=std::to_string(rooms.size())+" public rooms. Right: ping. A: join. B: back.";}
            if(!saved)message+=" Settings could not be saved to SD.";
        } else message=ping_server(c.room,s.cancel,measured);
        { std::lock_guard lock(s.mutex);
          if (c.serial!=s.serial || s.stop) continue;
          s.current.busy=false;s.current.status=message;
          if (replace_rooms) s.current.rooms=std::move(rooms);
          if (c.action==Action::Ping) for(auto &r:s.current.rooms) if(r.host==c.room.host&&r.port==c.room.port)r.ping_ms=measured;
        }
    }
}
}
bool initialize_sockets() {
    // Networking must also work without the optional PC debugger initializing
    // nsysnet for us. Off never calls this routine.
    static std::mutex mutex;static bool ready;
    std::lock_guard lock(mutex);if(ready)return true;
    OSDynLoad_Module module{};void *entry=nullptr;
    if(OSDynLoad_Acquire("nsysnet.rpl",&module)!=OS_DYNLOAD_OK)return false;
    const bool found=OSDynLoad_FindExport(module,OS_DYNLOAD_EXPORT_FUNC,"socket_lib_init",&entry)==OS_DYNLOAD_OK&&entry;
    if(found)ready=reinterpret_cast<int(*)()>(entry)()>=0;
    OSDynLoad_Release(module);return ready;
}
void initialize() {
    auto &s=state(); Config c; std::string error;
    std::ifstream f(path,std::ios::binary);
    std::string text;
    if (f) { char bytes[32769];f.read(bytes,sizeof bytes);text.assign(bytes,f.gcount()); }
    const bool missing=!f.is_open();
    const bool valid=missing || parse(text,c,error);
    if(!valid)c.enabled=false;
    if(valid) {
        const bool fill_api=c.web_api_url.empty() || text.find("web_api_url=")==std::string::npos;
        if(c.web_api_url.empty())c.web_api_url=kDefaultWebApi;
        if((missing||fill_api) && !persist(c))error="Using default public directory; could not save settings to SD.";
        else if(missing)error="Wireless is off. Enable it to browse public rooms.";
    }
    std::lock_guard lock(s.mutex);s.chat_side=c.chat_side=="off"?0:c.chat_side=="left"?1:2;s.current.config=c;s.current.status=error;s.enabled=c.enabled;
}
void start() {
    auto &s=state();if(s.started.exchange(true))return;s.stop=false;
    if (!s.worker.Start(run,-1,47)) {s.started=false;report("Could not start the network settings worker.");return;}
}
bool shutdown() { auto &s=state();s.stop=true;s.cancel=true;s.changed.notify_all();return !s.started || s.worker.JoinTimeout(1200); }
std::vector<ChatLine> chat(){return kernel::room_chat_view();}
int chat_side(){return state().chat_side.load();}
Live live(){return kernel::room_menu_view();}
void say(const std::string &s){kernel::room_chat_send(s);}
void disconnect(){state().connected=false;queue(Action::Disconnect,view().config);}
View view(bool include_rooms) {auto &s=state();std::lock_guard lock(s.mutex);return View{s.current.config,include_rooms?s.current.rooms:std::vector<Room>{},s.current.status,s.current.busy};}
bool wireless_enabled(){return state().enabled.load();}
bool connected(){return state().connected.load();}
void connection_changed(bool value){state().connected=value&&state().enabled.load();}
void report(const std::string &text){auto &s=state();std::lock_guard lock(s.mutex);s.current.status=text;}
void apply(const Config &c,bool reconnect) {
    const auto old=view().config;
    reconnect|=old.enabled!=c.enabled||old.host!=c.host||old.port!=c.port||old.nickname!=c.nickname||old.password!=c.password;
    if (auto error=validate(c);!error.empty()) {report(error);return;}
    {auto &s=state();std::lock_guard lock(s.mutex);s.chat_side=c.chat_side=="off"?0:c.chat_side=="left"?1:2;s.current.config=c;if(!c.enabled){s.enabled=false;s.connected=false;}}
    queue(reconnect?Action::Apply:Action::Save,c);
}
void browse(){auto v=view();if(!v.config.enabled){report("Turn wireless communication on first.");return;}queue(Action::Browse,v.config);}
void join(const Room &r,const std::string &password) {
    if(r.version!=azl::room::kNetworkVersion){report("This room uses an incompatible network protocol.");return;}
    auto c=view().config;c.host=r.host;c.port=r.port;c.password=password;c.enabled=true;
    {auto &s=state();std::lock_guard lock(s.mutex);s.current.config=c;}
    queue(Action::Join,c,r,password);
}
void ping(const Room &r){if(r.host.empty()||!r.port){report("Set a server address and port first.");return;}if(!view().config.enabled){report("Turn wireless communication on first.");return;}queue(Action::Ping,view().config,r);}
}
