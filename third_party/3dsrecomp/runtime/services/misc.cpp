// Small services: ptm, ndm, ac, frd, boss, cecd, news, ir, mic, cam, y2r, ldr:ro, soc, ssl, http, ...
#include <algorithm>
#include <cstring>
#include <vector>

#include "../loader.h"
#include "../service.h"
#include "services.h"

// The host's microphone: silence, unless a port provides one
__attribute__((weak)) void mic_host_start() {}
__attribute__((weak)) void mic_host_stop() {}
__attribute__((weak)) void mic_host_read(s16 *out, u32 n, u32 rate) { memset(out, 0, n * 2); }

namespace kernel {

std::vector<std::array<u8, 0x60>> g_host_miis;
int g_host_my_mii = -1;

u16 mii_crc16(const u8 *p, size_t n) {
    u16 c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (u16)(p[i] << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (u16)((c << 1) ^ 0x1021) : (u16)(c << 1);
    }
    return c;
}

// A host Mii as a 3DS game reads it. The Wii U and the 3DS store Miis the same way (version 3: big-endian fields, UTF-16LE names,
// a CRC-16 over 0x00-0x5D at 0x5E), which is why QR codes go between them, so the bytes go across. If the first four come the other
// way round (version last), they're turned. Copying is allowed: Tomodachi Life's look-alikes need it.
bool mii_3ds(const std::array<u8, 0x60> &h, std::array<u8, 0x60> &m) {
    if (mii_crc16(h.data(), 0x5E) != (u16)(h[0x5E] << 8 | h[0x5F])) return false;  // the host's own copy must check out
    m = h;
    if (m[0] != 3 && m[3] == 3) std::reverse(m.begin(), m.begin() + 4);
    if (m[0] != 3 || !(m[0x0C] | m[0x0D] | m[0x0E] | m[0x0F])) return false;  // not version 3, or no Mii id (an empty slot)
    m[0x01] |= 1;
    // Born on a console that came after the 3DS (4: a Wii U, as its accounts' Miis are), it is said to be a 3DS's: Super Mario 3D Land's Mii
    // library, of 2011, counts no such Mii in Mii Maker ("You don't have any Mii characters") and drops its face from a save file at the
    // next start.
    if ((m[0x03] >> 4 & 7) > 3) m[0x03] = (u8)((m[0x03] & 0x8F) | 0x30);
    m[0x5C] = m[0x5D] = 0;
    const u16 crc = mii_crc16(m.data(), 0x5E);
    m[0x5E] = (u8)(crc >> 8);
    m[0x5F] = (u8)crc;
    return true;
}

// The player's Mii: the current account's, or a plain male Mii named "Mario" (made-up console and Mii ids and a placeholder MAC;
// games mostly check the flags and the checksum)
std::array<u8, 0x60> player_mii() {
    std::array<u8, 0x60> m{};
    if (g_host_my_mii >= 0 && g_host_my_mii < (int)g_host_miis.size() && mii_3ds(g_host_miis[g_host_my_mii], m)) return m;
    m.fill(0);
    auto be32 = [&](size_t o, u32 v) { m[o] = (u8)(v >> 24); m[o + 1] = (u8)(v >> 16); m[o + 2] = (u8)(v >> 8); m[o + 3] = (u8)v; };
    auto be16 = [&](size_t o, u16 v) { m[o] = (u8)(v >> 8); m[o + 1] = (u8)v; };
    m[0] = 0x03;
    m[1] = 0x01;  // copying allowed (games that make look-alikes, e.g. Tomodachi Life, refuse it otherwise)
    m[2] = 0x10;
    m[3] = 0x30;
    const u64 sys = 0x3D5000000000C0DEull;  // console id of the "author"
    for (int i = 0; i < 8; i++) m[4 + i] = (u8)(sys >> (56 - 8 * i));
    be32(0x0C, 0x98000001);  // Mii id: normal Mii flags, creation time 1
    const u8 mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};  // locally administered placeholder
    memcpy(&m[0x10], mac, 6);
    be16(0x18, 0xA600);
    const char *name = "Mario";
    for (int i = 0; name[i]; i++) m[0x1A + 2 * i] = (u8)name[i];
    m[0x2E] = 0x40;
    m[0x2F] = 0x40;
    m[0x32] = 0x21;
    m[0x33] = 0x01;
    be32(0x34, 0x02684418);
    be32(0x38, 0x26344614);
    be16(0x3C, 0x8112);
    be16(0x3E, 0x1768);
    be16(0x40, 0x0D00);
    be16(0x42, 0x0029);
    be16(0x44, 0x0052);
    be16(0x46, 0x4850);
    const char *author = "Player";
    for (int i = 0; author[i]; i++) m[0x48 + 2 * i] = (u8)author[i];
    be16(0x5E, mii_crc16(m.data(), 0x5E));
    return m;
}

struct Ptm : Service {
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0005:  // GetAdapterState
        case 0x0006:  // GetShellState (open)
        case 0x0008:  // GetBatteryChargeState
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        case 0x0007:  // GetBatteryLevel
            ipc.ok(2);
            ipc.set(2, 5);
            break;
        case 0x0009:  // GetPedometerState
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x000B: {  // GetStepHistory(hours, start time u64, mapped buffer)
            u32 n = ipc[1];
            memset(gptr(ipc[5]), 0, n * 2);
            ipc.ok(1, 2);
            ipc.set(2, ((n * 2) << 4) | 0xC);
            ipc.set(3, ipc[5]);
            break;
        }
        case 0x000C:  // GetTotalStepCount
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0818:  // ptm:sysm ConfigureNew3DSCPU (libctru osSetSpeedupEnable): an old 3DS ignores it
            ipc.ok();
            break;
        case 0x0001:
        case 0x0002:
        case 0x0003:
        case 0x0004:
        default:
            unimplemented(ipc);
        }
    }
};

struct Ac : Service {
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001: {  // CreateDefaultConfig -> static buffer 0x200
            u32 dst = ipc.static_buffer(0);
            memset(gptr(dst), 0, 0x200);
            ipc.ok(1, 2);
            ipc.static_desc(2, 0, 0x200);
            break;
        }
        case 0x0004:  // ConnectAsync
            ipc.ok();
            if (auto ev = get<Event>(ipc[5])) ev->signal();
            break;
        case 0x0005:  // GetConnectResult
            ipc.result(0xE0A09D2E);  // not connected
            break;
        case 0x000D:  // GetStatus
        case 0x000F:  // GetWifiStatus
        case 0x003E:  // IsConnected
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x000E:  // GetLastErrorCode
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        default:
            unimplemented(ipc);
        }
    }
};

struct Frd : Service {
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001:  // HasLoggedIn
        case 0x0002:  // IsOnline
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        // numbered as Azahar's frd_u.cpp has them (these were one or two off before, and GetMyMii was missing)
        case 0x0005:  // GetMyFriendKey
            ipc.ok(5);
            ipc.set(2, 0);
            ipc.set(3, 0);
            ipc.set64(4, 0);
            break;
        case 0x0006:  // GetMyPreference
            ipc.ok(4);
            ipc.set(2, 0);
            ipc.set(3, 0);
            ipc.set(4, 0);
            break;
        case 0x0007:  // GetMyProfile
            ipc.ok(3);
            ipc.set(2, 0);
            ipc.set(3, 0);
            break;
        case 0x0008: {  // GetMyPresence -> static buffer
            u32 dst = ipc.static_buffer(0);
            memset(gptr(dst), 0, 0x12C);
            ipc.ok(1, 2);
            ipc.static_desc(2, 0, 0x12C);
            break;
        }
        case 0x0009: {  // GetMyScreenName: the player's Mii's name
            const auto mii = player_mii();
            ipc.ok(7);
            memset(gptr(ipc.buf + 8), 0, 24);
            for (u32 i = 0; i < 10; i++) w16(ipc.buf + 8 + 2 * i, (u16)(mii[0x1A + 2 * i] | mii[0x1B + 2 * i] << 8));
            break;
        }
        case 0x000A: {  // GetMyMii: the player's Mii, 0x60 bytes with its checksum
            const auto mii = player_mii();
            ipc.ok(0x19);
            memcpy(gptr(ipc.buf + 8), mii.data(), 0x60);
            break;
        }
        case 0x000B:  // GetMyLocalAccountId
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        case 0x0011:  // GetFriendKeyList
            ipc.ok(2, 2);
            ipc.set(2, 0);
            ipc.static_desc(3, 0, 0);
            break;
        default:
            unimplemented(ipc);
        }
    }
};

struct Ndm : Service {
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x000D:  // GetCurrentState
        case 0x000E:  // QueryStatus
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        default:
            ipc.ok();
        }
    }
};

struct Cecd : Service {
    using Service::Service;
    std::shared_ptr<Event> info = make_event(ONESHOT, "cecd info");
    std::shared_ptr<Event> state = make_event(ONESHOT, "cecd state");
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x000F:  // GetCecInfoEventHandle
            ipc.ok(1, 2);
            ipc.handles(2, {info});
            break;
        case 0x0010:  // GetChangeStateEventHandle
            ipc.ok(1, 2);
            ipc.handles(2, {state});
            break;
        case 0x000E:  // GetCecStateAbbreviated
            ipc.ok(2);
            ipc.set(2, 1);  // idle
            break;
        case 0x0001:  // Open -> fails: no message box
        case 0x0002:
            ipc.result(0xC8A11BFA, 2);
            ipc.set(2, 0);
            break;
        // Kid Icarus: Uprising reads and writes its StreetPass box at start-up; answered as "unimplemented" (the request's own words back), it
        // stopped drawing for good (2026-11-08). Nothing to read, writes taken, the mapped buffer handed back as Azahar does.
        case 0x0012: {  // OpenAndRead(size, program id, path type, ncch id, pid, buffer) -> nothing there
            const u32 desc = ipc[7], addr = ipc[8];
            ipc.result(0xC8A11BFA, 2, 2);
            ipc.set(2, 0);
            ipc.set(3, desc);
            ipc.set(4, addr);
            break;
        }
        case 0x0011: {  // OpenAndWrite(size, program id, path type, ncch id, pid, buffer)
            const u32 desc = ipc[7], addr = ipc[8];
            ipc.ok(1, 2);
            ipc.set(2, desc);
            ipc.set(3, addr);
            break;
        }
        case 0x0009: {  // SetData(program id, size, type, buffer)
            const u32 desc = ipc[4], addr = ipc[5];
            ipc.ok(1, 2);
            ipc.set(2, desc);
            ipc.set(3, addr);
            break;
        }
        case 0x0003:  // ReadMessage
        case 0x0004:  // ReadMessageWithHMAC: no messages
            echo(ipc, 0xC8A11BFA, true);
            break;
        default:  // Write, WriteMessage(WithHMAC), Delete (Super Mario 3D Land crashed after its answer was the request read back), ReadData,
                  // Start, Stop, GetCecInfoBuffer, ...: done, nothing to say
            echo(ipc, 0, false);
        }
    }
    // the result (and a zero size), then the request's buffer descriptors handed back, as Azahar answers: never the request's own words
    static void echo(IPC &ipc, u32 result, bool with_size) {
        const u32 hdr = ipc[0], n = (hdr >> 6) & 63, t = hdr & 63;
        u32 tw[16], k = 0;
        for (u32 i = 0; i < t && k + 1 < 16; i++) {
            const u32 d = ipc[1 + n + i];
            if (d == 0x20) {  // the calling process's id: not part of an answer
                i++;
                continue;
            }
            tw[k++] = d;
        }
        const u32 normal = with_size ? 2 : 1;
        ipc.result(result, normal, k);
        if (with_size) ipc.set(2, 0);
        for (u32 i = 0; i < k; i++) ipc.set(1 + normal + i, tw[i]);
    }
};

struct Boss : Service {
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001:  // InitializeSession
        case 0x0002:  // SetStorageInfo
        case 0x0003:  // UnregisterStorage
            ipc.ok();
            break;
        case 0x0004:  // GetStorageInfo
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0006:  // GetOptoutFlag
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x000D: {  // GetTaskIdList
            ipc.ok();
            break;
        }
        default:
            unimplemented(ipc);
        }
    }
};

// The game's microphone buffer is a block of its own memory. Samples arrive in 10 ms batches; the last word of the block says how far
// they have got (3dbrew: MIC Shared Memory).
struct Mic : Service {
    using Service::Service;
    std::shared_ptr<Event> buf_full = make_event(ONESHOT, "mic");
    std::shared_ptr<SharedMem> mem;
    u32 start = 0, end = 0, pos = 0, rate = 32730, generation = 0;
    u8 encoding = 0, gain = 0, power = 0, clamp = 0;
    bool loop = false, sampling = false;

    void stop() {
        if (sampling) mic_host_stop();
        sampling = false;
        generation++;
    }
    void store(u32 a, s16 s) {
        switch (encoding) {
        case 0: *(u8 *)gptr(a) = (u8)((s >> 8) + 128); break;  // unsigned 8-bit
        case 1: w16(a, (u16)(s + 32768)); break;               // unsigned 16-bit
        case 2: *(u8 *)gptr(a) = (u8)(s >> 8); break;          // signed 8-bit
        default: w16(a, (u16)s);                               // signed 16-bit
        }
    }
    void tick(u32 g) {
        if (g != generation || !sampling) return;
        s16 in[512];
        const u32 n = rate / 100, step = encoding & 1 ? 2 : 1;
        mic_host_read(in, n, rate);
        for (u32 i = 0; i < n && sampling; i++) {
            if (pos + step > end) {
                buf_full->signal();
                if (!loop) {
                    stop();
                    break;
                }
                pos = start;
            }
            store(mem->backing + pos, in[i]);
            pos += step;
        }
        w32(mem->backing + mem->size - 4, pos);
        if (sampling) add_timeout(now_ns() + 10000000, [this, g] { tick(g); });
    }
    void handle(IPC &ipc) override {
        static const u32 rates[4] = {32730, 16360, 10910, 8180};
        switch (ipc.cmd()) {
        case 0x0001:  // MapSharedMemory(size, handle)
            mem = get<SharedMem>(ipc[3]);
            ipc.ok();
            break;
        case 0x0002:  // UnmapSharedMemory
            stop();
            mem = nullptr;
            ipc.ok();
            break;
        case 0x0003:  // StartSampling(encoding, rate, offset, size, loop)
            stop();
            if (mem) {
                encoding = (u8)ipc[1] & 3;
                rate = rates[ipc[2] & 3];
                start = pos = ipc[3];
                end = std::min(start + ipc[4], mem->size - 4);
                loop = ipc[5] & 1;
                sampling = true;
                LOG("[mic] sampling at %u Hz, encoding %u, %u bytes%s\n", rate, encoding, end - start, loop ? ", looped" : "");
                mic_host_start();
                const u32 g = generation;
                add_timeout(now_ns() + 10000000, [this, g] { tick(g); });
            }
            ipc.ok();
            break;
        case 0x0004:  // AdjustSampling(rate)
            rate = rates[ipc[1] & 3];
            ipc.ok();
            break;
        case 0x0005:  // StopSampling
            stop();
            ipc.ok();
            break;
        case 0x0006:  // IsSampling
            ipc.ok(2);
            ipc.set(2, sampling);
            break;
        case 0x0007:  // GetBufferFullEvent
            ipc.ok(1, 2);
            ipc.handles(2, {buf_full});
            break;
        case 0x0008:  // SetGain
            gain = (u8)ipc[1];
            ipc.ok();
            break;
        case 0x0009:  // GetGain
            ipc.ok(2);
            ipc.set(2, gain);
            break;
        case 0x000A:  // SetPower
            power = (u8)ipc[1];
            ipc.ok();
            break;
        case 0x000B:  // GetPower
            ipc.ok(2);
            ipc.set(2, power);
            break;
        case 0x000D:  // SetClamp
            clamp = (u8)ipc[1];
            ipc.ok();
            break;
        case 0x000E:  // GetClamp
            ipc.ok(2);
            ipc.set(2, clamp);
            break;
        default:  // the filter, SetAllowShellClosed, SetClientVersion
            ipc.ok();
        }
    }
};

struct Ir : Service {
    using Service::Service;
    std::shared_ptr<Event> conn = make_event(ONESHOT, "ir conn");
    std::shared_ptr<Event> recv = make_event(ONESHOT, "ir recv");
    std::shared_ptr<Event> send = make_event(ONESHOT, "ir send");
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x000C:  // GetConnectionStatusEvent
            ipc.ok(1, 2);
            ipc.handles(2, {conn});
            break;
        case 0x000D:  // GetReceiveEvent
            ipc.ok(1, 2);
            ipc.handles(2, {recv});
            break;
        case 0x000E:  // GetSendEvent
            ipc.ok(1, 2);
            ipc.handles(2, {send});
            break;
        default:
            ipc.ok();
        }
    }
};

// Local wireless with nobody around: a game can look for other players and find none. New Super Mario Bros. 2's Co-op menu stops with a
// fatal error if it doesn't get its status event.
struct Uds : Service {
    using Service::Service;
    std::shared_ptr<Event> status = make_event(ONESHOT, "uds status");
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x001B:  // InitializeWithVersion
            ipc.ok(1, 2);
            ipc.handles(2, {status});
            break;
        case 0x000B:  // GetConnectionStatus
            ipc.ok(13);
            ipc.set(2, 3);  // not connected
            for (int i = 3; i < 14; i++) ipc.set(i, 0);
            break;
        case 0x000D:  // GetNodeInformation: no such node
            ipc.result(0xC90113FA);
            break;
        case 0x000F: {  // RecvBeaconBroadcastData(size, ..., mapped buffer): no beacons. Header: size of the buffer, bytes used, entries
            const u32 desc = ipc[19], buf = ipc[20];
            w32(buf, ipc[1]);
            w32(buf + 4, 12);
            w32(buf + 8, 0);
            ipc.ok(1, 2);
            ipc.set(2, desc);
            ipc.set(3, buf);
            break;
        }
        case 0x0012:  // Bind: an event for packets that never come (New Super Mario Bros. 2's Coin Rush)
            ipc.ok(1, 2);
            ipc.handles(2, {bind});
            break;
        case 0x0003:  // Shutdown
        case 0x000A:  // DisconnectNetwork
        case 0x0013:  // Unbind
            ipc.ok();
            break;
        default:
            unimplemented(ipc);
        }
    }
    std::shared_ptr<Event> bind = make_event(ONESHOT, "uds bind");
};

struct Generic : Service {
    using Service::Service;
    void handle(IPC &ipc) override { unimplemented(ipc); }
};

// am:app: the add-on content and update the builder put beside the game (builder/dumps.py), as Azahar's AM hands them out: dlc_contents.bin
// holds the DLC title's content infos (index, type, id, size, ownership; 0x18 bytes each), dlc_title.bin and update_title.bin a title info
// each (title id, size, version, type), dlc_ticket.bin a ticket info. With none of them nothing is installed.
struct Am : Service {
    using Service::Service;
    static std::vector<u8> records(const char *name) {
        std::vector<u8> d;
        if (FILE *f = fopen((g_game.dir + "/" + name).c_str(), "rb")) {
            u8 buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
            fclose(f);
        }
        d.resize(d.size() / 0x18 * 0x18);
        return d;
    }
    static u64 tid_of(const std::vector<u8> &r, size_t i = 0) {
        u64 v;
        memcpy(&v, &r[i * 0x18], 8);
        return le64(v);
    }
    // the DLC title's records, if the request is about it
    static std::vector<u8> dlc_contents(u64 tid) {
        auto t = records("dlc_title.bin");
        return !t.empty() && tid_of(t) == tid ? records("dlc_contents.bin") : std::vector<u8>{};
    }
    static u32 put(const std::vector<u8> &r, u32 start, u32 count, u32 dst) {
        u32 n = 0;
        for (size_t i = start; i < r.size() / 0x18 && n < count; i++, n++) memcpy(gptr(dst + n * 0x18), &r[i * 0x18], 0x18);
        return n;
    }
    // Nothing installed answers "not found", and a count of none: "success" with nothing written would leave the request's own words where the
    // answer goes. New Super Mario Bros. 2's Coin Rush read its DLC title id's high word (0x0004008C) back as the number of DLC tickets it owns (2026-11-07).
    void handle(IPC &ipc) override {
        const u32 not_found = 0xD8A08078;  // AM, invalid state, not found
        switch (ipc.cmd()) {
        case 0x1001: {  // GetDLCContentInfoCount(media type, title id)
            auto r = dlc_contents((u64)ipc[2] | (u64)ipc[3] << 32);
            if (r.empty()) {
                ipc.result(not_found);
                break;
            }
            ipc.ok(2);
            ipc.set(2, (u32)(r.size() / 0x18));
            break;
        }
        case 0x1002: {  // FindDLCContentInfos(media type, title id, count, [content indices], [content infos])
            auto r = dlc_contents((u64)ipc[2] | (u64)ipc[3] << 32);
            if (r.empty()) {
                ipc.result(not_found);
                break;
            }
            const u32 count = ipc[4], ids = ipc[6], dst = ipc[8];
            for (u32 i = 0; i < count; i++)
                for (size_t k = 0; k < r.size() / 0x18; k++)
                    if ((r[k * 0x18] | r[k * 0x18 + 1] << 8) == r16(ids + 2 * i)) memcpy(gptr(dst + i * 0x18), &r[k * 0x18], 0x18);
            ipc.ok(1, 4);
            ipc.set(2, ipc[5]);
            ipc.set(3, ids);
            ipc.set(4, ipc[7]);
            ipc.set(5, dst);
            break;
        }
        case 0x1003: {  // ListDLCContentInfos(count, media type, title id, start, [content infos]) -> how many
            auto r = dlc_contents((u64)ipc[3] | (u64)ipc[4] << 32);
            if (r.empty()) {
                ipc.result(not_found);
                break;
            }
            const u32 n = put(r, ipc[5], ipc[1], ipc[7]);
            ipc.ok(2, 2);
            ipc.set(2, n);
            ipc.set(3, ipc[6]);
            ipc.set(4, ipc[7]);
            break;
        }
        case 0x1005:    // GetDLCTitleInfos(media type, count, [title ids], [title infos])
        case 0x100D: {  // GetPatchTitleInfos(media type, count, [title ids], [title infos])
            auto r = records(ipc.cmd() == 0x1005 ? "dlc_title.bin" : "update_title.bin");
            const u32 count = ipc[2], ids = ipc[4], dst = ipc[6];
            u32 res = r.empty() ? not_found : 0;
            for (u32 i = 0; i < count && !res; i++) {
                const u64 tid = (u64)r32(ids + 8 * i) | (u64)r32(ids + 8 * i + 4) << 32;
                if (tid_of(r) == tid) memcpy(gptr(dst + i * 0x18), r.data(), 0x18);
                else res = not_found;
            }
            if (res) {
                ipc.result(res);
                break;
            }
            ipc.ok(1, 4);
            ipc.set(2, ipc[3]);
            ipc.set(3, ids);
            ipc.set(4, ipc[5]);
            ipc.set(5, dst);
            break;
        }
        case 0x1006: {  // GetNumDataTitleTickets(title id) -> count
            auto r = records("dlc_ticket.bin");
            ipc.ok(2);
            ipc.set(2, !r.empty() && tid_of(r) == ((u64)ipc[1] | (u64)ipc[2] << 32) ? (u32)(r.size() / 0x18) : 0);
            break;
        }
        case 0x1007:    // ListDLCTicketInfos(count, title id, start, [ticket infos]) -> how many
        case 0x1008: {  // ListDataTitleTicketInfos, the same shape
            auto r = records("dlc_ticket.bin");
            const u64 tid = (u64)ipc[2] | (u64)ipc[3] << 32;
            const u32 n = !r.empty() && tid_of(r) == tid ? put(r, ipc[4], ipc[1], ipc[6]) : 0;
            ipc.ok(2, 2);
            ipc.set(2, n);
            ipc.set(3, ipc[5]);
            ipc.set(4, ipc[6]);
            break;
        }
        default:
            unimplemented(ipc);
        }
    }
};

void register_misc() {
    register_service(std::make_shared<Ptm>("ptm:u"));
    register_service(std::make_shared<Ptm>("ptm:sysm"));
    register_service(std::make_shared<Ac>("ac:u"));
    register_service(std::make_shared<Ac>("ac:i"));
    register_service(std::make_shared<Frd>("frd:u"));
    register_service(std::make_shared<Frd>("frd:a"));
    register_service(std::make_shared<Ndm>("ndm:u"));
    register_service(std::make_shared<Cecd>("cecd:u"));
    register_service(std::make_shared<Boss>("boss:U"));
    register_service(std::make_shared<Mic>("mic:u"));
    register_service(std::make_shared<Ir>("ir:USER"));
    register_service(std::make_shared<Ir>("ir:u"));
    register_service(std::make_shared<Uds>("nwm::UDS"));
    register_service(std::make_shared<Am>("am:app"));  // was Generic: NSMB2's Coin Rush read its own request back as thousands of DLC tickets (2026-11-07)
    register_y2r();
    register_cam();
    for (const char *n : {"news:u", "soc:U", "ssl:C", "http:C", "dlp:FKCL", "dlp:SRVR",
                          "pxi:dev", "act:u", "nim:aoc", "qtm:u", "csnd:SND", "nfc:u"})
        register_service(std::make_shared<Generic>(n));
}

void register_all_services() {
    register_apt();
    register_fs();
    register_cfg();
    register_gsp();
    register_hid();
    register_dsp();
    register_misc();
    register_ro();
}

// ---- shared system font (APT GetSharedFont): the BCFNT from system archive 0004009B00014002, LZ11-decompressed
// after a 0x80-byte header (status 2 = loaded, region, size), with its section pointers relocated to the
// address where the game sees the block. The block is in the linear heap: the GPU samples the glyph sheets.
static std::vector<u8> lz11(const std::vector<u8> &d) {
    if (d.size() < 4 || d[0] != 0x11) return {};
    size_t size = d[1] | d[2] << 8 | d[3] << 16, p = 4;
    if (!size && d.size() >= 8) size = d[4] | d[5] << 8 | d[6] << 16 | (size_t)d[7] << 24, p = 8;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size && p < d.size()) {
        u8 flags = d[p++];
        for (int b = 0; b < 8 && out.size() < size && p < d.size(); b++) {
            if (!(flags & (0x80 >> b))) {
                out.push_back(d[p++]);
                continue;
            }
            size_t len, disp;
            u8 x = d[p];
            if (x >> 4 == 0) {
                len = ((x & 15) << 4 | d[p + 1] >> 4) + 0x11;
                disp = ((d[p + 1] & 15) << 8 | d[p + 2]) + 1;
                p += 3;
            } else if (x >> 4 == 1) {
                len = ((x & 15) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111;
                disp = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1;
                p += 4;
            } else {
                len = (x >> 4) + 1;
                disp = ((x & 15) << 8 | d[p + 1]) + 1;
                p += 2;
            }
            if (disp > out.size()) return {};
            for (size_t i = 0; i < len && out.size() < size; i++) out.push_back(out[out.size() - disp]);
        }
    }
    return out;
}

// Rebases the section pointers of the font at addr + 0x80 so that they point inside the block at addr, and marks
// it resolved ("CFNU", as the system's copy is: the font library would otherwise add its address again).
static void relocate_shared_font(u32 addr) {
    u32 cfnt = addr + 0x80;
    if (memcmp(gptr(cfnt), "CFNT", 4) && memcmp(gptr(cfnt), "CFNU", 4)) return;
    memcpy(gptr(cfnt), "CFNU", 4);
    u32 nblocks = r32(cfnt + 0x10), first = cfnt + r16(cfnt + 6);
    u32 cmap = 0, cmap_ptr = 0, at = first;
    for (u32 i = 0; i < nblocks; i++, at += r32(at + 4)) {
        if (!cmap && !memcmp(gptr(at), "CMAP", 4)) cmap = at;
        if (!memcmp(gptr(at), "FINF", 4)) cmap_ptr = r32(at + 24);
    }
    if (!cmap || !cmap_ptr) return;
    u32 delta = (cmap + 8) - cmap_ptr;  // where the pointers assume the font is vs where it is
    if (!delta) return;
    at = first;
    for (u32 i = 0; i < nblocks; i++, at += r32(at + 4)) {
        auto fix = [&](u32 a) { if (r32(a)) w32(a, r32(a) + delta); };
        if (!memcmp(gptr(at), "FINF", 4)) { fix(at + 16); fix(at + 20); fix(at + 24); }
        else if (!memcmp(gptr(at), "TGLP", 4)) fix(at + 28);
        else if (!memcmp(gptr(at), "CWDH", 4)) fix(at + 12);
        else if (!memcmp(gptr(at), "CMAP", 4)) fix(at + 16);
    }
}

std::shared_ptr<SharedMem> load_shared_font() {
    // A build may bring the font already unpacked (shared_font.bcfnt beside the game's files, tools/make_game_title.py): the console then reads it
    // as it is, with no archive to search and nothing to decompress while the game starts. Without the file it comes from the system archive.
    std::vector<u8> font;
    if (FILE *f = fopen((g_game.dir + "/shared_font.bcfnt").c_str(), "rb")) {
        u8 buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) font.insert(font.end(), buf, buf + n);
        fclose(f);
    }
    if (font.empty()) font = lz11(system_archive_file(0x0004009B00014002ull, "/cbf_std.bcfnt.lz"));
    const u32 size = 0x332000;
    if (font.empty() || font.size() > size - 0x80) {
        LOG("[apt] shared font: system archive 0004009b00014002 missing or unreadable (%zu bytes)\n", font.size());
        return nullptr;
    }
    auto sm = std::make_shared<SharedMem>();
    sm->name = "shared font";
    sm->size = size;
    sm->backing = alloc_linear(size);
    if (!sm->backing) {
        LOG("[apt] shared font: no linear memory left\n");
        return nullptr;
    }
    memset(gptr(sm->backing), 0, size);
    memcpy(gptr(sm->backing + 0x80), font.data(), font.size());
    w32(sm->backing, 2);  // loaded
    w32(sm->backing + 4, 1);
    w32(sm->backing + 8, (u32)font.size());
    relocate_shared_font(sm->backing);
    sm->on_move = relocate_shared_font;
    LOG("[apt] shared font: %zu bytes at %08x\n", font.size(), sm->backing);
    return sm;
}

}  // namespace kernel
