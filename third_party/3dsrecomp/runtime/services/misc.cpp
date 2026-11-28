// Small services: ptm, ndm, ac, frd, boss, cecd, news, ir, mic, cam, y2r, ldr:ro, soc, ssl, http, ...
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <vector>

#include "../loader.h"
#include "../service.h"
#include "services.h"
#ifdef __WIIU__
#include "uds_service.h"
#endif

// The host's microphone: silence, unless a port provides one
__attribute__((weak)) void mic_host_start() {}
__attribute__((weak)) void mic_host_stop() {}
__attribute__((weak)) void mic_host_read(s16 *out, u32 n, u32 rate) { memset(out, 0, n * sizeof(*out)); }
__attribute__((weak)) u32 mic_host_read_available(s16 *out, u32 n, u32 rate) { mic_host_read(out, n, rate); return n; }

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

// A host Mii as a 3DS game reads it. Two layouts are taken, told apart by where the version (3) stands. The 3DS's own, which a Mii also
// has wherever the two consoles exchange one (a QR code, an account's Mii on its server): the version first, the ids as they read, every
// other field little-endian, UTF-16LE names, a CRC-16 over 0x00-0x5D at 0x5E. Its bytes go across. And the one the Wii U's own processor
// reads (wut's nn/ffl/miidata.h), which is what its software keeps in memory: the version last of the first four bytes, and every 16-bit
// field from 0x18 on the other way round, but for height and build, a byte each. That one is turned back first. Copying is allowed:
// Tomodachi Life's look-alikes need it.
bool mii_3ds(const std::array<u8, 0x60> &h, std::array<u8, 0x60> &m) {
    if (mii_crc16(h.data(), 0x5E) != (u16)(h[0x5E] << 8 | h[0x5F])) return false;  // the host's own copy must check out
    m = h;
    if (m[0] != 3 && m[3] == 3) {
        std::reverse(m.begin(), m.begin() + 4);
        for (size_t at = 0x18; at < 0x5C; at += 2)
            if (at != 0x2E) std::swap(m[at], m[at + 1]);  // height and build are a byte each
    }
    if (m[0] != 3 || mii_id_empty(m.data() + 0x0C)) return false;  // not version 3, or no Mii id (an empty slot)
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

// The console's Miis as 3DS games read them, the accounts' first and each one once (an account's Mii is in the console's Mii Maker too).
// Made once, at the first call: the frontend has read the console's Miis before the services start.
namespace {
struct ConsoleMiis {
    std::vector<std::array<u8, 0x60>> list;
    int mine = -1;  // the current account's
    ConsoleMiis() {
        for (size_t i = 0; i < g_host_miis.size(); i++) {
            std::array<u8, 0x60> m;
            if (!mii_3ds(g_host_miis[i], m)) continue;
            size_t at = 0;
            while (at < list.size() && memcmp(&list[at][0x0C], &m[0x0C], 10)) at++;  // the same Mii: its id and the console that made it
            if (at == list.size()) list.push_back(m);
            if ((int)i == g_host_my_mii) {
                mine = (int)at;
                list[at] = m;
            }
        }
    }
};
}  // namespace

const std::vector<std::array<u8, 0x60>> &console_miis(int *mine) {
    static const ConsoleMiis miis;
    if (mine) *mine = miis.mine;
    return miis.list;
}

// The player's Mii: the current account's (the console's first, if that account has none a game can take), naming the place it has in
// Mii Maker's database (fs.cpp). With no Mii on the console at all, a plain male Mii named "Mario" (made-up console and Mii ids and a
// placeholder MAC; games mostly check the flags and the checksum).
std::array<u8, 0x60> console_mii(u32 n) {
    std::array<u8, 0x60> mii = console_miis(nullptr)[n];
    if (const int place = mii_maker_place(n); place >= 0) mii[0x02] = (u8)(place / 10 | place % 10 << 4);
    const u16 crc = mii_crc16(mii.data(), 0x5E);
    mii[0x5E] = (u8)(crc >> 8);
    mii[0x5F] = (u8)crc;
    return mii;
}

std::array<u8, 0x60> player_mii() {
    int mine;
    if (!console_miis(&mine).empty()) return console_mii(mine < 0 ? 0 : (u32)mine);
    std::array<u8, 0x60> m{};
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
    SERVICE_CHECKPOINT(Ptm)
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
    SERVICE_CHECKPOINT(Ac)
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
        case 0x000C:  // GetStatus
        case 0x003E:  // IsConnected
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x000D:  // GetWifiStatus
            ipc.ok(2);
#ifdef __WIIU__
            ipc.set(2, room_wireless_available() ? 1 : 0);
#else
            ipc.set(2, 0);
#endif
            break;
        case 0x000A:  // GetLastErrorCode
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        default:
            unimplemented(ipc);
        }
    }
};

struct Frd : Service {
    SERVICE_CHECKPOINT(Frd)
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
    SERVICE_CHECKPOINT(Ndm)
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
    SERVICE_CHECKPOINT(Cecd, info, state)
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
    SERVICE_CHECKPOINT(Boss)
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
    static constexpr u64 tick_period = 10000000;
#ifdef RECOMP_RESUMABLE
    bool persistent_supported() const override { return true; }
    bool capture(checkpoint::Image &im) override {
        // Host microphone input stays live. Rewind the guest buffer/cursor and
        // its saved timeout, then reopen/close the host stream to match it.
        // Do not copy a native microphone handle or enqueue a second tick.
        bool saved_sampling = sampling;
        if (im.archive) im.archive->io(saved_sampling);
        im.restore.push_back([this, saved_sampling] {
            // Microphone input is live host state. An already-running stream
            // does not need teardown/reinitialization when rewinding the game.
            if (sampling && !saved_sampling) mic_host_stop();
            else if (!sampling && saved_sampling) mic_host_start();
        });
        im.take(buf_full); im.take(mem);
        im.take(next_tick); im.take(start); im.take(end); im.take(pos); im.take(rate); im.take(generation);
        im.take(encoding); im.take(gain); im.take(power); im.take(clamp);
        im.take(loop); im.take(sampling);
        return true;
    }
#endif
#ifdef RECOMP_RESUMABLE
    void persistent_timeout(u64 g, u32) override { tick(static_cast<u32>(g)); }
#endif
    std::shared_ptr<Event> buf_full = make_event(ONESHOT, "mic");
    std::shared_ptr<SharedMem> mem;
    u64 next_tick = 0;
    u32 start = 0, end = 0, pos = 0, rate = 32728, generation = 0, sample_remainder = 0;
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
        const u32 budget = rate + sample_remainder, step = encoding & 1 ? 2 : 1;
        sample_remainder = budget % 100;
        const u32 n = mic_host_read_available(in, budget / 100, rate);
        for (u32 i = 0; i < n && sampling; i++) {
            if (end - pos < step) pos = start;
            store(mem->backing + pos, in[i]);
            pos += step;
            if (end - pos < step) {
                buf_full->signal();
                if (!loop) stop();
            }
        }
        w32(mem->backing + mem->size - 4, pos);
        if (sampling) {
            // Keep the sampling clock across a late callback, with bounded catch-up after a long pause.
            const u64 now = now_ns(), catch_up = 9 * tick_period;
            next_tick = std::max(next_tick + tick_period, now > catch_up ? now - catch_up : 0);
            add_timeout(next_tick, [this, g] { tick(g); } STATE_TIMEOUT(TimeoutKind::Service, nullptr, shared_from_this(), g));
        }
    }
    void handle(IPC &ipc) override {
        static const u32 rates[4] = {32728, 16364, 10909, 8182};
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
            if (mem && mem->size > 4 && ipc[3] < mem->size - 4) {
                encoding = (u8)ipc[1] & 3;
                rate = rates[ipc[2] & 3];
                start = pos = ipc[3];
                end = start + std::min(ipc[4], mem->size - 4 - start);
                if (end - start < (encoding & 1 ? 2u : 1u)) {
                    ipc.ok();
                    break;
                }
                loop = ipc[5] & 1;
                sampling = true;
                sample_remainder = 0;
                LOG("[mic] sampling at %u Hz, encoding %u, %u bytes%s\n", rate, encoding, end - start, loop ? ", looped" : "");
                mic_host_start();
                const u32 g = generation;
                next_tick = now_ns() + tick_period;
                add_timeout(next_tick, [this, g] { tick(g); } STATE_TIMEOUT(TimeoutKind::Service, nullptr, shared_from_this(), g));
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

// The host's second stick and ZL/ZR (input thread): the Circle Pad Pro's C-stick and triggers, as a New 3DS gives its own to a game
namespace {
std::atomic<int> extra_x, extra_y;  // -0x7FF..0x7FF
std::atomic<u32> extra_buttons;     // 1 ZL, 2 ZR, 4 R
std::atomic<u32> extra_live_buttons;  // latched separately so physical refreshes cannot erase scripted holds
std::atomic<unsigned> extra_connections;
}  // namespace
void hid_extra_pad(float x, float y, bool zl, bool zr, bool r) {
    extra_x = std::isfinite(x) ? (int)(std::clamp(x, -1.0f, 1.0f) * 0x7FF) : 0;
    extra_y = std::isfinite(y) ? (int)(std::clamp(y, -1.0f, 1.0f) * 0x7FF) : 0;
    extra_buttons = (zl ? 1u : 0u) | (zr ? 2u : 0u) | (r ? 4u : 0u);
}
bool extra_pad_connected() { return extra_connections != 0; }
void hid_extra_pad_live(u32 buttons) {
    extra_live_buttons = (buttons & (1u << 14) ? 1u : 0u) | (buttons & (1u << 15) ? 2u : 0u) |
                         (buttons & (1u << 8) ? 4u : 0u);
}

// Circle Pad Pro over ir:USER: status at 0, receive info at 0x10, descriptors at 0x20, then packet data.
// Keep queue accounting private: shared memory belongs to the guest and cannot supply trusted write offsets.
struct Ir : Service {
    using Service::Service;
#ifdef RECOMP_RESUMABLE
    bool persistent_supported() const override { return true; }
    bool capture(checkpoint::Image &im) override {
        // Avoid copying this resource-owning service: its destructor adjusts
        // the global count used to detect a connected Circle Pad Pro.
        im.take(conn); im.take(recv); im.take(send); im.take(mem);
        im.take(shared_size); im.take(packets_max); im.take(data_max);
        im.take(first); im.take(next); im.take(used); im.take(head); im.take(lengths);
        im.take(period_ms); im.take(generation);
        bool saved_connected = connected;
        if (im.archive) im.archive->io(saved_connected);
        im.restore.push_back([this, saved_connected] { connection(saved_connected); });
        // The kernel snapshot already owns the matching poll timeout. Physical
        // stick/button input remains live; do not create a duplicate poll.
        return true;
    }
#endif
#ifdef RECOMP_RESUMABLE
    void persistent_timeout(u64 g, u32) override { poll(static_cast<u32>(g)); }
#endif
    std::shared_ptr<Event> conn = make_event(ONESHOT, "ir conn");
    std::shared_ptr<Event> recv = make_event(ONESHOT, "ir recv");
    std::shared_ptr<Event> send = make_event(ONESHOT, "ir send");
    std::shared_ptr<SharedMem> mem;
    u32 shared_size = 0, packets_max = 0, data_max = 0;
    u32 first = 0, next = 0, used = 0, head = 0;
    std::deque<u32> lengths;
    bool connected = false;
    u32 period_ms = 0, generation = 0;  // the pad's reports, every period_ms while connected

    static constexpr u32 NOT_CONNECTED = 0xC8A10C0D;  // Status / InvalidState / IR / 13
    static constexpr u32 NO_DATA = 0xC8810FEF;  // Status / NotFound / IR / NoData

    ~Ir() override { connection(false); }
    void connection(bool on) {
        if (connected != on) {
            if (on) ++extra_connections;
            else --extra_connections;
            connected = on;
        }
    }
    bool mapped() const {
        return mem && shared_size <= mem->size && guest_readable_locked(mem->backing, shared_size) &&
               guest_writable(mem->backing, shared_size);
    }
    bool ring_valid() const {
        if (!mapped()) return false;
        const u32 info = mem->backing + 0x10, list = mem->backing + 0x20;
        if (r32(info) != first || r32(info + 4) != next || r32(info + 8) != lengths.size() || r32(info + 12)) return false;
        u32 index = first, offset = head;
        for (u32 size : lengths) {
            if (r32(list + index * 8) != offset || r32(list + index * 8 + 4) != size) return false;
            index = (index + 1) % packets_max;
            offset = ((u64)offset + size) % data_max;
        }
        return true;
    }
    void ring_info() {
        const u32 info = mem->backing + 0x10;
        w32(info, first); w32(info + 4, next); w32(info + 8, (u32)lengths.size()); w32(info + 12, 0);
    }
    void stop() {
        ++generation;
        period_ms = 0;
        connection(false);
    }
    void status(u8 value, bool notify = true) {
        connection(value == 2);
        if (mapped()) {
            w8(mem->backing + 0x9, value == 1 ? 2 : 0);
            w8(mem->backing + 0x8, value);
            w8(mem->backing + 0xA, connected ? 2 : 0);
            w8(mem->backing + 0xC, connected ? 1 : 0);
        }
        if (notify && value != 1) conn->signal();
    }
    static u8 crc8(const std::vector<u8> &p) {
        u8 c = 0;
        for (u8 b : p) {
            c ^= b;
            for (int i = 0; i < 8; i++) c = (c & 0x80) ? (u8)((c << 1) ^ 0x07) : (u8)(c << 1);
        }
        return c;
    }
    void put(const std::vector<u8> &payload) {  // one reply into the receive buffer
        if (!ring_valid() || payload.size() >= 0x4000) return;
        const u32 b = mem->backing, list = b + 0x20, data = list + packets_max * 8;
        std::vector<u8> p = {0xA5, r8(b + 0xD)};
        if (payload.size() < 0x40) {
            p.push_back((u8)payload.size());
        } else {
            p.push_back((u8)(0x40 | (payload.size() >> 8)));
            p.push_back((u8)payload.size());
        }
        p.insert(p.end(), payload.begin(), payload.end());
        p.push_back(crc8(p));
        if (lengths.size() >= packets_max || p.size() > data_max - used) return;
        const u32 at = lengths.empty() ? 0 : ((u64)head + used) % data_max;
        for (size_t i = 0; i < p.size(); i++) w8(data + ((u64)at + i) % data_max, p[i]);
        w32(list + next * 8, at);
        w32(list + next * 8 + 4, (u32)p.size());
        if (lengths.empty()) head = at;
        lengths.push_back((u32)p.size());
        next = (next + 1) % packets_max;
        used += (u32)p.size();
        ring_info();
        recv->signal();
    }
    u32 release(u32 n) {
        if (!mapped()) return RES_INVALID_ADDRESS;
        if (!ring_valid()) return RES_INVALID_COMBINATION;
        if (n > lengths.size()) return NO_DATA;
        for (; n; --n) {
            used -= lengths.front();
            head = ((u64)head + lengths.front()) % data_max;
            lengths.pop_front();
            first = (first + 1) % packets_max;
        }
        ring_info();
        return RES_OK;
    }
    void report() {  // the C-stick (12 bits each, 0x800 the middle), a full battery, and ZL, ZR and R, set when not held
        const u32 x = (u32)(0x800 + extra_x), y = (u32)(0x800 + extra_y), held = extra_buttons | extra_live_buttons;
        const u32 stick = 0x10 | (x & 0xFFF) << 8 | (y & 0xFFF) << 20;
        put({(u8)stick, (u8)(stick >> 8), (u8)(stick >> 16), (u8)(stick >> 24),
             (u8)(0x1F | (held & 1 ? 0 : 0x20) | (held & 2 ? 0 : 0x40) | (held & 4 ? 0 : 0x80)), 0});
    }
    void schedule() {
        std::weak_ptr<Service> weak = weak_from_this();
        const u32 g = generation;
        add_timeout(now_ns() + period_ms * 1000000ull, [weak, g] {
            if (auto service = weak.lock()) static_cast<Ir *>(service.get())->poll(g);
        } STATE_TIMEOUT(TimeoutKind::Service, nullptr, shared_from_this(), g));
    }
    void poll(u32 g) {  // (kernel lock held)
        if (g != generation || !connected || !period_ms || !mapped()) return;
        report();
        schedule();
    }
    u32 request(const u8 *d, u32 size) {
        if (!size) return RES_INVALID_COMBINATION;
        if (d[0] == 1 && size == 3) {  // the reports' period in ms
            if (d[1] != period_ms) LOG("[ir] Circle Pad Pro: the game reads it every %u ms\n", d[1]);
            period_ms = d[1];
            ++generation;
            if (period_ms) schedule();
        } else if (d[0] == 2 && size == 6) {  // calibration: offset and size, in 16s, of the device's 64 bytes
            static const u8 calibration[0x40] = {
                0x00, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0xF5,
                0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
                0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
                0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F, 0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65};
            const u32 off = d[2] | d[3] << 8, n = d[4] | d[5] << 8;
            if (off % 16 || n % 16 || off + n > sizeof calibration) return RES_INVALID_COMBINATION;
            std::vector<u8> r = {0x11, d[2], d[3], d[4], d[5]};
            r.insert(r.end(), calibration + off, calibration + off + n);
            put(r);
        } else return RES_INVALID_COMBINATION;
        return RES_OK;
    }
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0018: {  // InitializeIrNopShared(shared size, receive size, receive count, send size, send count, baud rate, shared memory)
            if ((ipc[0] & 0xffff) != 0x182 || ipc[7] != 0) { ipc.result(RES_INVALID_COMBINATION); break; }
            auto candidate = get<SharedMem>(ipc[8]);
            if (!candidate) { ipc.result(RES_INVALID_HANDLE); break; }
            if (!ipc[3] || (u64)ipc[3] * 8 >= ipc[2] || (u64)ipc[5] * 8 > ipc[4] ||
                (u64)0x30 + ipc[2] + ipc[4] > ipc[1] || ipc[1] > candidate->size) {
                ipc.result(RES_INVALID_COMBINATION); break;
            }
            if (!guest_readable_locked(candidate->backing, ipc[1]) || !guest_writable(candidate->backing, ipc[1])) {
                ipc.result(RES_INVALID_ADDRESS); break;
            }
            stop();
            mem = candidate;
            shared_size = ipc[1]; packets_max = ipc[3]; data_max = ipc[2] - packets_max * 8;
            first = next = used = head = 0;
            lengths.clear();
            conn->clear(); recv->clear(); send->clear();
            for (u32 i = 0; i < 0x20; i++) w8(mem->backing + i, 0);
            w8(mem->backing + 0xE, 1);
            ipc.ok();
            break;
        }
        case 0x0006:  // RequireConnection(device): the Circle Pad Pro (1) answers at once
            if ((ipc[0] & 0xffff) != 0x40) { ipc.result(RES_INVALID_COMBINATION); break; }
            if (!mapped()) { ipc.result(RES_INVALID_ADDRESS); break; }
            if ((ipc[1] & 0xFF) != 1) stop();
            status((ipc[1] & 0xFF) == 1 ? 2 : 1);
            ipc.ok();
            break;
        case 0x0009:  // Disconnect
            if (ipc[0] & 0xffff) { ipc.result(RES_INVALID_COMBINATION); break; }
            status(0, connected);
            stop();
            ipc.ok();
            break;
        case 0x0002:  // FinalizeIrNop
            if (ipc[0] & 0xffff) { ipc.result(RES_INVALID_COMBINATION); break; }
            if (connected) status(0);
            stop();
            if (mapped()) w8(mem->backing + 0xE, 0);
            mem = nullptr;
            lengths.clear();
            ipc.ok();
            break;
        case 0x000D:    // SendIrNop(size, static buffer)
        case 0x000E: {  // SendIrNopLarge(size, mapped buffer)
            const bool large = ipc.cmd() == 0xE;
            const u32 size = ipc[1], desc = ipc[2], addr = ipc[3];
            const u32 capacity = large ? desc >> 4 : desc >> 14;
            const bool readable = large ? ((desc & 15) == 10 || (desc & 15) == 14) : (desc & 0x3fff) == 2;
            u32 error = RES_OK;
            if ((ipc[0] & 0xffff) != 0x42 || !readable || size > capacity || (size != 3 && size != 6))
                error = RES_INVALID_COMBINATION;
            else if (!connected) error = NOT_CONNECTED;
            else if (!guest_readable_locked(addr, size) || !mapped()) error = RES_INVALID_ADDRESS;
            else if (!ring_valid()) error = RES_INVALID_COMBINATION;
            else {
                u8 d[6];
                for (u32 i = 0; i < size; i++) d[i] = r8(addr + i);
                error = request(d, size);
                if (!error) send->signal();
            }
            // Return a valid mapped descriptor even on device errors so the caller can finish its transfer.
            const bool mapped_reply = large && readable && (ipc[0] & 0xffff) == 0x42;
            ipc.result(error, 1, mapped_reply ? 2 : 0);
            if (mapped_reply) { ipc.set(2, desc); ipc.set(3, addr); }
            break;
        }
        case 0x0019:  // ReleaseReceivedData(count)
            ipc.result((ipc[0] & 0xffff) == 0x40 ? release(ipc[1]) : RES_INVALID_COMBINATION);
            break;
        case 0x000A:  // GetReceiveEvent
        case 0x000B:  // GetSendEvent
        case 0x000C:  // GetConnectionStatusEvent
            if (ipc[0] & 0xffff) { ipc.result(RES_INVALID_COMBINATION); break; }
            ipc.ok(1, 2);
            ipc.handles(2, {ipc.cmd() == 0xA ? recv : ipc.cmd() == 0xB ? send : conn});
            break;
        default:
            VLOG("[ir] command %04x\n", ipc.cmd());
            ipc.ok();
        }
    }
};

// ir:u is the raw infrared protocol, not the Circle Pad Pro service. Retain its idle event stub separately.
struct Iru : Service {
    using Service::Service;
    std::shared_ptr<Event> conn = make_event(ONESHOT, "ir:u conn");
    std::shared_ptr<Event> recv = make_event(ONESHOT, "ir:u recv");
    std::shared_ptr<Event> send = make_event(ONESHOT, "ir:u send");
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0xC:  // retained legacy event stub
        case 0xD:  // GetSendFinishedEvent
        case 0xE:  // GetRecvFinishedEvent
            ipc.ok(1, 2);
            ipc.handles(2, {ipc.cmd() == 0xD ? send : ipc.cmd() == 0xE ? recv : conn});
            break;
        default:
            ipc.ok();
        }
    }
};

// Offline local wireless: games can obtain their status/bind events and scan, but there is no transport or peer.
// Command IDs and reply layouts: Azahar nwm_uds.cpp / nwm_uds.h, commit11eceda5e0e682a1f44e396bf954a1b9137097ca.
struct Uds : Service {
    SERVICE_CHECKPOINT(Uds, status, bind)
    using Service::Service;
    std::shared_ptr<Event> status = make_event(ONESHOT, "uds status");
    // shortcut: one offline bind event; use per-bind events when a transport is added.
    std::shared_ptr<Event> bind = make_event(ONESHOT, "uds bind");
    // CTR Result fields: UDS module68; Status/InvalidState/NotAuthorized, and Permanent/NotSupported/NotImplemented.
    static constexpr u32 NOT_CONNECTED = 0xC8A113EA;
    static constexpr u32 UNSUPPORTED = 0xD8C113F4;

    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001:  // Initialize (deprecated)
        case 0x001B:  // InitializeWithVersion
            status->clear();
            bind->clear();
            ipc.ok(1, 2);
            ipc.handles(2, {status});
            break;
        case 0x0004:  // BeginHostingNetwork (deprecated)
        case 0x0009:  // ConnectToNetwork (deprecated)
        case 0x001D:  // BeginHostingNetwork
        case 0x001E:  // ConnectToNetwork
            unimplemented(ipc, false);
            ipc.result(UNSUPPORTED);  // Cannot create a working host/client without a transport.
            break;
        case 0x0005:  // EjectClient: only a host can eject, and node1 is never ejectable.
            ipc.result((u16)ipc[1] == 1 ? 0xE10113EA : 0xE0A113EA);
            break;
        case 0x0006:  // EjectSpectators: no spectators exist.
        case 0x0007:  // UpdateNetworkAttribute: no network exists to update.
            ipc.ok();
            break;
        case 0x0008:  // DestroyNetwork: Status / InvalidState / UDS / WrongStatus490
            ipc.result(0xC8A111EA);
            break;
        case 0x000A:  // DisconnectNetwork: remains offline, but notify status waiters.
            ipc.ok();
            status->signal();
            break;
        case 0x000B:  // GetConnectionStatus: result +0x30-byte ConnectionStatus, all zero except NotConnected3.
            ipc.ok(13);
            ipc.set(2, 3);
            for (int i = 3; i < 14; i++) ipc.set(i, 0);
            break;
        case 0x000D:  // GetNodeInformation: no peer node is known.
            ipc.result(0xC90113FA);  // Status / WrongArgument / UDS / NotFound
            break;
        case 0x000F: {  // RecvBeaconBroadcastData: max buffer size, bytes used, entries (little endian).
            const u32 desc = ipc[19], buf = ipc[20];
            const u32 error = ipc[1] < 12 ? RES_INVALID_COMBINATION : ipc.mapped_output_error(19, ipc[1]);
            if (error) { ipc.result(error); break; }
            w32(buf, ipc[1]);
            w32(buf + 4, 12);
            w32(buf + 8, 0);
            ipc.ok(1, 2);
            ipc.set(2, desc);
            ipc.set(3, buf);
            break;
        }
        case 0x0011:  // GetApplicationData: no network/application data; return an empty static buffer.
            ipc.ok(2, 2);
            ipc.set(2, 0);
            ipc.static_desc(3, 0, 0);
            break;
        case 0x0012:  // Bind: a local packet event; packets never arrive while offline.
            if (!ipc[1] || !(u8)ipc[3]) { ipc.result(0xE10113EA); break; }
            if (ipc[2] < 0x5F4) { ipc.result(0xE10113E9); break; }
            bind->clear();
            ipc.ok(1, 2);
            ipc.handles(2, {bind});
            break;
        case 0x0003:  // Shutdown
            status->clear();
            bind->signal();  // Release a guest waiting for a packet, as Unbind does.
            ipc.ok();
            break;
        case 0x0013:  // Unbind: result, bind ID, three reserved words.
            if (!ipc[1]) { ipc.result(0xE10113EA); break; }
            bind->signal();  // Release a guest waiting on the local binding being cancelled.
            ipc.ok(5);
            ipc.set(2, ipc[1]);
            for (int i = 3; i < 6; i++) ipc.set(i, 0);
            break;
        case 0x0014:  // PullPacket
        case 0x0017:  // SendTo
            ipc.result(NOT_CONNECTED);
            break;
        case 0x001A:  // GetChannel: zero when NotConnected.
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        default:
            // Includes deprecated/modern DecryptBeaconData0xE/0x1F and SetApplicationData0x10.
            unimplemented(ipc, false);
            ipc.result(UNSUPPORTED);
        }
    }
};

struct Generic : Service {
    SERVICE_CHECKPOINT(Generic)
    using Service::Service;
    void handle(IPC &ipc) override { unimplemented(ipc); }
};

// ssl:C has no network behind it, but games take their random numbers from it: Pokemon Ultra Sun seeds its own generator with them at
// every start and save. Unanswered, the buffer stayed as the game left it, and every start rolled the same.
struct Ssl : Service {
    SERVICE_CHECKPOINT(Ssl)
    using Service::Service;
    void handle(IPC &ipc) override {
        if (ipc.cmd() != 0x0011) return unimplemented(ipc);
        const u32 error = ipc.mapped_output_error(2, ipc[1]);
        if (error) return ipc.result(error);
        // GenerateRandomData(size, [buffer]): the clock and the tick it's first asked at, stirred (splitmix64)
        static u64 state = (u64)time(nullptr) << 32 ^ system_tick();
        u64 z = 0;
        for (u32 i = 0; i < ipc[1]; i++) {
            if (i % 8 == 0) {
                z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ z >> 27) * 0x94D049BB133111EBull;
                z ^= z >> 31;
            }
            w8(ipc[3] + i, (u8)(z >> (i % 8 * 8)));
        }
        ipc.ok(1, 2);  // (the buffer's two words go back as they came)
    }
};

// Disabled networking must fail initialization rather than claim a lobby is
// starting. In particular, MK7 polls GetServerState after a successful init.
static constexpr u32 DLP_WIRELESS_OFF = 0xC940AC04;
struct DlpFkcl : Service {
    SERVICE_CHECKPOINT(DlpFkcl)
    using Service::Service;
    void handle(IPC &ipc) override {
        // Cleanup must still succeed after an initialization failure.
        const u32 cmd = ipc.cmd();
        ipc.result(cmd == 2 || cmd == 6 || cmd == 0x10 ? 0 : DLP_WIRELESS_OFF);
    }
};

// Download Play children are a separate 3DS title category. Use the current guest
// program ID so retail games take their normal startup path.
struct DlpSrvr : Service {
    SERVICE_CHECKPOINT(DlpSrvr)
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x000E:  // IsChild(process id)
            ipc.ok(2);
            ipc.set(2, (g_game.program_id >> 32) == 0x00040001);
            break;
        case 0x0002:  // Finalize must succeed even when wireless is unavailable.
            ipc.ok();
            break;
        default:
            ipc.result(DLP_WIRELESS_OFF);
        }
    }
};

// am:app: the add-on content and update the builder put beside the game (builder/dumps.py), as Azahar's AM hands them out: dlc_contents.bin
// holds the DLC title's content infos (index, type, id, size, ownership; 0x18 bytes each), dlc_title.bin and update_title.bin a title info
// each (title id, size, version, type), dlc_ticket.bin a ticket info. With none of them nothing is installed.
struct Am : Service {
    SERVICE_CHECKPOINT(Am)
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
    register_service(std::make_shared<Iru>("ir:u"));
#ifdef __WIIU__
    if (!register_room_uds())
#endif
    register_service(std::make_shared<Uds>("nwm::UDS"));
    register_service(std::make_shared<Am>("am:app"));  // was Generic: NSMB2's Coin Rush read its own request back as thousands of DLC tickets (2026-11-07)
    register_service(std::make_shared<DlpSrvr>("dlp:SRVR"));
    register_service(std::make_shared<DlpFkcl>("dlp:FKCL"));
    register_y2r();
    register_cam();
    register_service(std::make_shared<Ssl>("ssl:C"));
    for (const char *n : {"news:u", "soc:U", "http:C",
                          "pxi:dev", "act:u", "nim:aoc", "qtm:u", "csnd:SND", "nfc:u"})
        register_service(std::make_shared<Generic>(n));
#ifdef __WIIU__
    register_room_dlp();
    start_room_uds();
#endif
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

#ifdef RECOMP_RESUMABLE
namespace kernel {
u32 persistent_sharedmem_callback(const SharedMem &sm) {
    return !sm.on_move ? 0 : sm.on_move == relocate_shared_font ? 1 : 0xffffffffu;
}
bool persistent_sharedmem_callback(SharedMem &sm, u32 kind) {
    if (kind > 1) return false;
    sm.on_move = kind ? relocate_shared_font : nullptr;
    return true;
}
}
#endif
