#pragma once
#include "../service.h"
#include "uds_events.h"
#include "azl/room_client.hpp"
#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"

namespace wiiu::network { struct Config; struct Live; struct ChatLine; bool wireless_enabled(); }
namespace kernel {
wiiu::network::Live room_menu_view();
std::vector<wiiu::network::ChatLine> room_chat_view();
void room_chat_send(const std::string &);
std::string configure_room_uds(const wiiu::network::Config &, const std::string &);
std::string disconnect_room_uds();
bool register_room_uds();
void register_room_dlp();
void start_room_uds();
bool room_wireless_available();
// The owner keeps this context alive until the room worker has stopped.
struct UdsContext {
    UdsEvents events;
    azl::RoomClient::Config configuration;
    azl::UdsEngine engine{
        &events, {
            [this](azl::WifiPacket &packet) {
                if (!wiiu::network::wireless_enabled() || !room.joined()) return;
                packet.transmitter_address = room.mac();
                room.SendWifiPacket(packet);
            },
            [this] { return room.mac(); }
        }
    };
    std::unique_ptr<azl::RealDlpNet> dlp_network;
    std::unique_ptr<azl::DlpHub> dlp;
    // Destroy the worker before the engine and DLP objects its callbacks use.
    azl::RoomClient room;
    UdsContext() {
        room.SetWifiCallback([this](const azl::WifiPacket &packet) { if(wiiu::network::wireless_enabled())engine.OnWifiPacket(packet); });
    }
};

class UdsService final : public Service {
    std::shared_ptr<UdsContext> context;
    static constexpr u32 bad_buffer = 0xD9001830;
    const u8 *read_buffer(u32 address, u32 size) {
        return guest_readable_locked(address, size) ? gptr(address) : nullptr;
    }
public:
    explicit UdsService(std::shared_ptr<UdsContext> ctx)
        : Service("nwm::UDS"), context(std::move(ctx)) {}
    void handle(IPC &ipc) override {
        if (!wiiu::network::wireless_enabled() && ipc.cmd() != 0x03 && ipc.cmd() != 0x0b) { ipc.result(azl::res::kWirelessOff); return; }
        auto &engine = context->engine;
        auto &events = context->events;
        events.Publish();
        switch (ipc.cmd()) {
        case 0x01:
        case 0x1b: {
            if (context->dlp) {
                run_blocked(*ipc.lk, [&] { context->dlp->OnGameUdsStarting(); });
            }
            u8 bytes[40];
            for (u32 i = 0; i < 10; ++i) azl::StoreLE32(bytes + 4 * i, ipc[2 + i]);
            azl::NodeInfo node;
            node.Load(bytes);
            u32 event = 0;
            const u32 result = engine.Initialize(node, ipc[1], ipc.cmd() == 0x1b ? ipc[12] : 0x100, &event);
            if (result) { ipc.result(result); break; }
            ipc.ok(1, 2);
            ipc.handles(2, {events.GuestEvent(event)});
            break;
        }
        case 0x03: ipc.result(engine.Shutdown()); break;
        case 0x05: ipc.result(engine.EjectClient(ipc[1])); break;
        case 0x06: ipc.result(engine.EjectSpectators()); break;
        case 0x07: ipc.result(engine.UpdateNetworkAttribute(ipc[1], ipc[2])); break;
        case 0x08: ipc.result(engine.DestroyNetwork()); break;
        case 0x0a: ipc.result(engine.DisconnectNetwork()); break;
        case 0x0b: {
            u8 bytes[48];
            engine.GetConnectionStatus().Store(bytes);
            ipc.ok(13);
            for (u32 i = 0; i < 12; ++i) ipc.set(2 + i, azl::LoadLE32(bytes + i * 4));
            break;
        }
        case 0x0c: {
            u16 table[azl::UdsEngine::kPeerSignalEntries]{};
            const u32 result = engine.GetPeerSignal(table);
            if (result) { ipc.result(result); break; }
            ipc.ok(10);
            ipc.set(2, table[0]);
            for (u32 i = 0; i < 8; ++i)
                ipc.set(3 + i, u32(table[1 + 2 * i]) | (u32(table[2 + 2 * i]) << 16));
            break;
        }
        case 0x0e:
        case 0x1f: {
            const u32 network_size = ipc[1] >> 14;
            const auto *network_data = read_buffer(ipc[2], sizeof(azl::NetworkInfo));
            const u32 tag0_size = ipc[3] >> 14, tag1_size = ipc[5] >> 14;
            const auto *tag0 = read_buffer(ipc[4], tag0_size);
            const auto *tag1 = read_buffer(ipc[6], tag1_size);
            u32 capacity = 0;
            const u32 address = ipc.static_buffer(0, &capacity);
            if (network_size < sizeof(azl::NetworkInfo) || !network_data || !tag0 || !tag1 ||
                !guest_writable(address, capacity)) { ipc.result(bad_buffer); break; }
            azl::NetworkInfo network;
            std::memcpy(&network, network_data, sizeof network);
            std::vector<u8> nodes;
            const u32 result = engine.DecryptBeaconData(network, tag0, tag0_size, tag1, tag1_size, &nodes);
            if (result) { ipc.result(result); break; }
            const u32 size = std::min<u32>(capacity, nodes.size());
            std::memcpy(gptr(address), nodes.data(), size);
            ipc.ok(1, 2);
            ipc.static_desc(2, 0, size);
            break;
        }
        case 0x0d: {
            azl::NodeInfo node;
            const u32 result = engine.GetNodeInformation(ipc[1], &node);
            if (result) { ipc.result(result); break; }
            u8 bytes[40];
            node.Store(bytes);
            ipc.ok(11);
            for (u32 i = 0; i < 10; ++i) ipc.set(2 + i, azl::LoadLE32(bytes + i * 4));
            break;
        }
        case 0x0f:
        case 0x22: {
            const bool connected = ipc.cmd() == 0x22;
            const u32 desc_word = connected ? 17 : 19;
            const u32 size = ipc[desc_word] >> 4, address = ipc[desc_word + 1];
            if (!guest_writable(address, size) || size > (1u << 20)) { ipc.result(bad_buffer); break; }
            azl::ScanParams scan;
            scan.type = ipc[2];
            scan.mode = ipc[2] >> 16;
            scan.channel_mask = ipc[3];
            scan.dwell_ms = ipc[3] >> 16;
            u8 bssid[8];
            azl::StoreLE32(bssid, ipc[4]);
            azl::StoreLE32(bssid + 4, ipc[5]);
            std::copy_n(bssid, 6, scan.bssid.begin());
            scan.ssid_len = ipc[14];
            scan.wlan_comm_id = ipc[15];
            scan.id8 = ipc[16];
            std::vector<u8> data(size);
            u32 result = bad_buffer;
            run_blocked(*ipc.lk, [&] {
                result = connected ? engine.ScanOnConnection(data.data(), size, scan)
                    : engine.RecvBeaconBroadcastData(data.data(), size, scan);
            });
            if (result) { ipc.result(result); break; }
            if (!guest_writable(address, size)) { ipc.result(bad_buffer); break; }
            memcpy(gptr(address), data.data(), size);
            ipc.ok(1, 2);
            ipc.set(2, ipc[desc_word]);
            ipc.set(3, address);
            break;
        }
        case 0x14: {
            u32 capacity = 0;
            const u32 address = ipc.static_buffer(0, &capacity);
            capacity = std::min(capacity, std::min<u32>(ipc[2], 0x172) << 2);
            if (!guest_writable(address, capacity)) { ipc.result(bad_buffer); break; }
            u32 size = 0;
            u16 sender = 0;
            const u32 result = engine.PullPacket(ipc[1], ipc[3], gptr(address), capacity, &size, &sender);
            if (result) { ipc.result(result); break; }
            ipc.ok(3, 2);
            ipc.set(2, size);
            ipc.set(3, sender);
            ipc.static_desc(4, 0, capacity);
            break;
        }
        case 0x10: {
            const u32 size = ipc[2] >> 14;
            const auto *data = read_buffer(ipc[3], size);
            ipc.result(data ? engine.SetApplicationData(data, size) : bad_buffer);
            break;
        }
        case 0x11: {
            u32 capacity = 0;
            const u32 address = ipc.static_buffer(0, &capacity);
            capacity = std::min(capacity, ipc[1]);
            if (!guest_writable(address, capacity)) { ipc.result(bad_buffer); break; }
            u32 actual = 0;
            const u32 result = engine.GetApplicationData(gptr(address), capacity, &actual);
            ipc.result(result, 2, 2);
            ipc.set(2, actual);
            ipc.static_desc(3, 0, actual);
            break;
        }
        case 0x12: {
            u32 event = 0;
            const u32 result = engine.Bind(ipc[1], ipc[2], ipc[3], ipc[4], &event);
            if (result) { ipc.result(result); break; }
            ipc.ok(1, 2);
            ipc.handles(2, {events.GuestEvent(event)});
            break;
        }
        case 0x13: {
            azl::UdsEngine::UnbindInfo info{};
            const u32 result = engine.Unbind(ipc[1], &info);
            if (result) { ipc.result(result); break; }
            ipc.ok(5);
            ipc.set(2, ipc[1]);
            ipc.set(3, (u32(info.channel) << 16) | info.src);
            ipc.set(4, info.enqueued);
            ipc.set(5, info.overwritten);
            break;
        }
        case 0x15: ipc.result(engine.SetMaxSendDelay(u64(ipc[1]) | (u64(ipc[2]) << 32))); break;
        case 0x16: ipc.result(engine.ApplyRadioProfile(ipc[1])); break;
        case 0x17: {
            const u32 available = ipc[7] >> 14;
            const auto *data = read_buffer(ipc[8], available);
            ipc.result(data ? engine.SendTo(ipc[2], ipc[3], ipc[5], ipc[6], data, available) : bad_buffer);
            break;
        }
        case 0x18: ipc.result(engine.SetBeaconMissTime(ipc[1])); break;
        case 0x19: ipc.result(engine.SetApParam(ipc[1])); break;
        case 0x1a: {
            u8 channel = 0;
            const u32 result = engine.GetChannel(&channel);
            ipc.result(result, 2);
            ipc.set(2, channel);
            break;
        }
        case 0x1c: ipc.result(engine.SetDeauthOnDestroy((ipc[1] & 0xff) != 0)); break;
        case 0x1d: {
            const u32 size = std::min<u32>(ipc[2] >> 14, sizeof(azl::NetworkInfo));
            const auto *data = read_buffer(ipc[3], size);
            ipc.result(data ? engine.BeginHostingNetwork(data, size) : bad_buffer);
            break;
        }
        case 0x1e: {
            const u32 size = std::min<u32>(ipc[3] >> 14, sizeof(azl::NetworkInfo));
            const auto *data = read_buffer(ipc[4], size);
            if (!data) { ipc.result(bad_buffer); break; }
            const std::vector<u8> network(data, data + size);
            const u8 type = ipc[1];
            u32 result = bad_buffer;
            run_blocked(*ipc.lk, [&] { result = engine.ConnectToNetwork(network.data(), network.size(), type); });
            ipc.result(result);
            break;
        }
        case 0x20: ipc.result(engine.Flush(ipc[1])); break;
        case 0x21: ipc.result(engine.SetProbeResponseParam()); break;
        case 0x23: ipc.ok(2); ipc.set(2, 0x7fff); break;
        default: unimplemented(ipc, false); break;
        }
        events.Publish();
    }
};
}
