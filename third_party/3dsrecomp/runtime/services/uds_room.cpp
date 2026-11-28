#ifdef __WIIU__
#include "uds_service.h"
#include "services.h"
#include "../loader.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <coreinit/debug.h>
#include <coreinit/time.h>
#include "azl/log.hpp"
#include "azl/stats.hpp"
#include "../../../../port/wiiu/network_settings.h"
#include "azl/chat_core.hpp"

extern "C" void enet_wiiu_trace(int kind, int result, int error, u32 host, u32 port, u32 word) {
    static u32 samples[2][2]{};
    const u32 index = kind == 1 ? 0 : 1, ready = result >= 0 && (kind == 1 || result > 0) ? 1 : 0;
    if (++samples[index][ready] > 6) return;
    OSReport("[uds-udp] %s result=%d errno=%d from=%08x:%u head=%08x\n",
        kind == 1 ? "recv" : "select", result, error, host, port, word);
}

namespace wiiu { const std::string &data_dir(); }

namespace kernel {
namespace {
std::shared_ptr<UdsContext> live_room;
std::atomic<bool> room_shutting_down{false};
bool events_timer_pending;
std::mutex room_config_mutex;
std::atomic<azl::ChatSink *> room_chat{nullptr};

// Kernel timer callbacks run with kmutex held. Networking only touches the
// engine and the bridge's own mutex, so kernel callbacks never wait on sockets.
void publish_room_events() {
    events_timer_pending = false;
    if (!live_room || room_shutting_down.load(std::memory_order_acquire)) return;
    const bool enabled = wiiu::network::wireless_enabled();
    w8(mem::SHARED_PAGE + 0x67, enabled ? 1 : 0);
    const bool joined = enabled && live_room->room.joined();
    w8(mem::SHARED_PAGE + 0x66, joined ? 3 : 0);
    if (!enabled) return; // Off has no recurring timer or networking worker.
    live_room->events.Publish();
    if (joined) {
        const auto mac = live_room->room.mac();
        std::copy(mac.begin(), mac.end(), gptr(mem::SHARED_PAGE + 0x60));
    }
    events_timer_pending = true;
    add_timeout(now_ns() + 10000000, publish_room_events);
}
}

namespace {
class RoomDlpService final : public Service {
    std::shared_ptr<UdsContext> context;
    azl::DlpKind kind;
public:
    RoomDlpService(std::shared_ptr<UdsContext> ctx, azl::DlpKind which, const char *name)
        : Service(name), context(std::move(ctx)), kind(which) {}
    void handle(IPC &ipc) override {
        // The engine's command words are host integers; game buffers stay LE.
        u32 words[64];
        std::copy_n(ipc.req, 64, words);
        u32 size = 0;
        const u32 address = ipc.static_buffer(0, &size);
        const u32 sbuf[2] = {(size << 14) | 2u, address};
        auto serve = [&] {
            auto outcome = context->dlp->Serve(kind, words, sbuf);
            if (outcome == azl::DlpOutcome::JoinPending) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!context->dlp->PollPending(words)) {
                    if (!context->room.joined() || std::chrono::steady_clock::now() >= deadline) {
                        context->dlp->OnSessionClosed(kind);
                        words[0] = (u32(ipc.cmd()) << 16) | 0x40;
                        words[1] = azl::DlpHub::kResultWirelessOff;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        };
        // Inline commands resolve buffers on this thread under kmutex. Posted
        // commands run on the room worker under the same kernel lock;
        // release it while waiting so that worker can finish the request.
        if (azl::DlpHub::IsInline(kind, ipc.cmd())) serve();
        else run_blocked(*ipc.lk, serve);
        const u32 count = 1 + ((words[0] >> 6) & 63) + (words[0] & 63);
        if (count > 64) { ipc.result(0xD9001830); return; }
        for (u32 i = 0; i < count; ++i) ipc.set(i, words[i]);
        context->events.Publish();
    }
};
}

void register_room_dlp() {
    if (!live_room) return;
    const u32 low = u32(g_game.program_id);
    if (low != 0x00030600 && low != 0x00030700 && low != 0x00030800 &&
        low != 0x00030a00 && low != 0x0008b400) return;
    azl::LobbyConfig config{};
    config.game_tid = g_game.program_id;
    config.friend_seed = 0x0000123456789ABCull; // matches CFG block 0x00090000
    config.max_players = 8;
    const auto mii = player_mii();
    for (u32 i = 0; i < 10; ++i)
        config.name[i] = u16(mii[0x1a + 2 * i]) | (u16(mii[0x1b + 2 * i]) << 8);
    const azl::IpcResolver resolver{nullptr, [](void*, u32 address, u32 size) -> u8* {
        // Called by inline IPC or the room worker, both with kmutex.
        return guest_writable(address, size) ? gptr(address) : nullptr;
    }};
    live_room->dlp_network = std::make_unique<azl::RealDlpNet>(&live_room->engine);
    live_room->dlp_network->SetPlayers(8);
    live_room->dlp = std::make_unique<azl::DlpHub>(&live_room->engine, &live_room->events,
        config, resolver, live_room->dlp_network.get());
    register_service(std::make_shared<RoomDlpService>(live_room, azl::DlpKind::Fkcl, "dlp:FKCL"));
    register_service(std::make_shared<RoomDlpService>(live_room, azl::DlpKind::Srvr, "dlp:SRVR"));
    OSReport("[uds-room] Mario Kart Download Play lobby bridge enabled (8 players)\n");
}

void start_room_uds() {
    // Install optional adapters before the worker can observe their pointers.
    if (!wiiu::network::wireless_enabled()) return;
    if(!wiiu::network::initialize_sockets()){wiiu::network::report("Could not initialize Wii U networking.");return;}
    wiiu::network::start();
    if(!live_room)return;
    if(!room_chat){room_chat=new azl::ChatSink;live_room->room.SetChatSink(room_chat.load());}
    if (live_room && !live_room->configuration.host.empty() && !live_room->room.Start(live_room->configuration))
        LOG("[uds-room] worker failed to start\n");
}

bool room_wireless_available() {
    return wiiu::network::wireless_enabled() && live_room && live_room->room.joined();
}

static std::string persistent_room_identity() {
    const std::string path = wiiu::data_dir() + "/multiplayer-console-id.txt";
    std::ifstream input(path);
    std::string id;
    if (std::getline(input, id) && id.size() == 32 &&
        id.find_first_not_of("0123456789abcdef") == std::string::npos) return id;
    // Public uniqueness token, not an authentication key or console secret.
    char generated[33];
    snprintf(generated, sizeof generated, "%016llx%016llx",
        (unsigned long long)OSGetSystemTime(),
        (unsigned long long)(OSGetTime() ^ (u64)(uintptr_t)&live_room));
    std::ofstream output(path, std::ios::trunc);
    output << generated << '\n';
    output.close();
    return output ? std::string(generated) : std::string{};
}

static std::string mii_room_nickname() {
    const auto mii = player_mii();
    std::string name;
    for (unsigned i = 0; i < 10; ++i) {
        const u16 c = u16(mii[0x1a + 2 * i]) | (u16(mii[0x1b + 2 * i]) << 8);
        if (!c) break;
        // Azahar rooms accept 4-20 ASCII letters, digits, spaces, '.', '_' and '-'.
        name += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-' ? char(c) : '_';
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) name = "WiiU";
    if (name.size() < 4) name += "-WiiU";
    return name;
}

bool register_room_uds() {
    wiiu::network::initialize();
    const auto settings = wiiu::network::view().config;
    azl::RoomClient::Config config;
    config.host = settings.host; config.port = settings.port; config.password = settings.password;
    config.nickname = settings.nickname;
    config.console_id_hash = settings.console_id;
    if (config.nickname.empty() || config.nickname == "auto" || config.nickname == "mii")
        config.nickname = mii_room_nickname();
    // The persistent identity is needed only when wireless is enabled.
    if (wiiu::network::wireless_enabled() && (config.console_id_hash.empty() || config.console_id_hash == "auto"))
        config.console_id_hash = persistent_room_identity();
    config.game_id = g_game.program_id;
    // load_game has already read the original SMDH title for both packaging formats.
    config.game_name = g_game.name.empty() ? g_game.title : g_game.name;
    if (config.game_name.empty()) config.game_name = "3Dii U";
    azl::g_debug_level = 1;
    OSReport("[uds-room] target %s:%u as %s\n", config.host.c_str(), unsigned(config.port), config.nickname.c_str());
    OSReport("[uds-room] playing %s (%016llx)\n", config.game_name.c_str(), (unsigned long long)config.game_id);
    azl::SetLogSink([](const char *line) { OSReport("[uds-link] %s\n", line); });
    live_room = std::make_shared<UdsContext>();
    live_room->configuration = config;
    live_room->room.SetChatSink(room_chat.load());
    live_room->room.SetTickCallback([](u64 now) {
        if (room_shutting_down.load(std::memory_order_acquire) || !wiiu::network::wireless_enabled()) return;
        live_room->engine.Tick(now);
        // Resumable guest timeouts run on the sole dispatcher. A service
        // waiting for DLP cannot also pump that queue: use the room worker.
        // Acquire kmutex before the hub lock, as inline IPC does, so posted
        // buffer writes and the guest mapping table are protected consistently.
        if (live_room->dlp) {
            Lock lk(kmutex);
            live_room->dlp->Tick();
        }
    });
    live_room->room.SetStateCallback([](azl::RoomClient::State state, azl::RoomClient::Error error) {
        OSReport("[uds-room] %s: %s\n", azl::RoomClient::StateName(state), azl::RoomClient::ErrorName(error));
        wiiu::network::connection_changed(state==azl::RoomClient::State::Joined);
        wiiu::network::report(std::string(azl::RoomClient::StateName(state)) + (error == azl::RoomClient::Error::None ? "" : std::string(": ") + azl::RoomClient::ErrorName(error)));
        if (state == azl::RoomClient::State::Idle && !room_shutting_down.load(std::memory_order_acquire))
            live_room->engine.OnTransportDown(azl::NetworkStatusChangeReason::ConnectionLost);
    });
    register_service(std::make_shared<UdsService>(live_room));

    publish_room_events();
    return true;
}
wiiu::network::Live room_menu_view() {
    wiiu::network::Live result;
    if (!live_room) {result.status="Room service is not initialized.";return result;}
    result.joined=live_room->room.joined();
    const auto info=live_room->room.room_info();result.name=info.name;
    {std::lock_guard lock(room_config_mutex);result.nickname=live_room->configuration.nickname;}
    result.status=azl::RoomClient::StateName(live_room->room.state());
    if(auto error=live_room->room.error();error!=azl::RoomClient::Error::None)
        result.status=azl::RoomClient::ErrorName(error);
    for(const auto &member:info.members)
        result.members.push_back(member.nickname+"  -  "+(member.game_name.empty()?"Not running a game":member.game_name));
    azl::chat::View chat{};
    if(room_chat && room_chat.load()->ring.ReadInto(&chat)) for(unsigned i=0;i<chat.count;i++) {
        const auto &line=chat.lines[i];
        result.chat.push_back((line.nick_len?std::string(line.nick)+": ":"")+line.text);
    }
    return result;
}
std::vector<wiiu::network::ChatLine> room_chat_view() {
    std::vector<wiiu::network::ChatLine> result;
    auto *sink=room_chat.load();azl::chat::View view{};
    if(sink && sink->ring.ReadInto(&view)) for(unsigned i=0;i<view.count;i++) {
        const auto &line=view.lines[i];
        result.push_back({line.t_ms,(line.nick_len?std::string(line.nick)+": ":"")+line.text});
    }
    return result;
}
void room_chat_send(const std::string &text) {
    if(!live_room || !live_room->room.joined()) {wiiu::network::report("Join a room before sending chat.");return;}
    if(!room_chat || !room_chat.load()->Post(text.c_str(),text.size())) wiiu::network::report("Chat message is empty or another message is still sending.");
}
std::string disconnect_room_uds() {
    if (!live_room) return "Room service is unavailable.";
    if (!live_room->room.Stop()) return "Room worker did not stop; restart the game before reconnecting.";
    live_room->engine.OnTransportDown(azl::NetworkStatusChangeReason::ConnectionLost);
    return "Disconnected from the room.";
}
std::string configure_room_uds(const wiiu::network::Config &settings, const std::string &authentication) {
    if (!live_room || room_shutting_down.load()) return "Room service is shutting down.";
    if (!live_room->room.Stop()) return "Room worker did not stop; restart the game before reconnecting.";
    live_room->engine.OnTransportDown(azl::NetworkStatusChangeReason::ConnectionLost);
    azl::RoomClient::Config config;{std::lock_guard lock(room_config_mutex);config=live_room->configuration;}
    config.host=settings.host;config.port=settings.port;config.password=settings.password;
    config.nickname=settings.nickname.empty()||settings.nickname=="auto"||settings.nickname=="mii" ? mii_room_nickname() : settings.nickname;
    config.console_id_hash=settings.console_id.empty()||settings.console_id=="auto" ? persistent_room_identity() : settings.console_id;
    config.authentication_token=authentication;
    {std::lock_guard lock(room_config_mutex);live_room->configuration=config;}
    {
        Lock lock(kmutex);
        w8(mem::SHARED_PAGE+0x67, settings.enabled ? 1 : 0);
        w8(mem::SHARED_PAGE+0x66,0);
        if(settings.enabled && !events_timer_pending) publish_room_events();
    }
    if(!settings.enabled) return "Wireless communication is off.";
    if(config.host.empty()) return "Wireless is on. Enter a server or browse public rooms.";
    if(config.console_id_hash.empty()) return "Could not create the room identity in save storage.";
    if(!room_chat){room_chat=new azl::ChatSink;live_room->room.SetChatSink(room_chat.load());}
    if(!live_room->room.Start(config)) return "Could not start the room connection.";
    return "Connecting to the room...";
}
// Stop while the application's socket services and callbacks still exist.
// Never hold kmutex here: the worker can be completing a DLP tick with it.
void shutdown_room_uds() {
    if (!live_room || room_shutting_down.exchange(true, std::memory_order_acq_rel)) return;
    if(!wiiu::network::shutdown()) {live_room->room.RequestStop(true);return;}
    OSReport("[uds-room] shutdown: stopping room worker before process cleanup\n");
    const bool stopped = live_room->room.StopWithin(1000);
    OSReport("[uds-room] shutdown: worker %s\n", stopped ? "stopped" : "exceeded 1000 ms; process teardown will reclaim it");
}
}
#endif
