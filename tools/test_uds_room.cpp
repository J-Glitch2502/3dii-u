// Offline UDS wire contract against the actual production handler, not a duplicate room model.
#include <cassert>
#include <cstdlib>
#include <vector>
#include "../third_party/3dsrecomp/runtime/services/apt_wireless_reboot.h"
#include "../third_party/3dsrecomp/runtime/services/misc.cpp"

static std::vector<u8> memory(0x10000, 0xcc);
u8 *g_mem = memory.data();
int g_verbose;
u64 g_frame;
GameInfo g_game;
void rt_log(const char *, ...) {}
[[noreturn]] void rt_fatal(const char *, ...) { abort(); }
namespace kernel {
bool guest_writable(u32 addr, u32 size) {
    return size && addr >= 0x4000 && (u64)addr + size <= 0x5000;
}
bool guest_readable_locked(u32 addr, u32 size) { return guest_writable(addr, size); }
u64 system_tick() { return 123456789; }
unsigned unsupported_calls;
void Service::unimplemented(IPC &ipc, bool succeed) {
    assert(!succeed); ++unsupported_calls; ipc.result(0xD900182F);
}
std::shared_ptr<Event> make_event(ResetType reset, const char *name) {
    auto event = std::make_shared<Event>(); event->reset = reset; event->name = name; return event;
}
static std::vector<ObjPtr> returned_handles;
int IPC::handles(int pos, std::initializer_list<ObjPtr> objects, bool move) {
    assert(!move); set(pos++, ((u32)objects.size() - 1) << 26);
    for (const auto &object : objects) { returned_handles.push_back(object); set(pos++, 0x40 + returned_handles.size()); }
    return pos;
}
void Event::signal() { signaled = true; }
static u32 static_address = 0x4500, static_size = 64;
u32 IPC::static_buffer(int index, u32 *size) const { assert(index == 0); if (size) *size = static_size; return static_address; }
void IPC::static_desc(int pos, int index, u32 size) { set(pos, size << 14 | index << 10 | 2); set(pos + 1, static_buffer(index)); }
static u64 fake_time, scheduled;
void add_timeout(u64 deadline, std::function<void()>) { assert(fake_time); scheduled = deadline; }
u64 now_ns() { assert(fake_time); return fake_time; }
u32 alloc_linear(u32) { assert(false); return 0; }
ObjPtr get_object(Handle) { assert(false); return {}; }
void register_service(std::shared_ptr<Service>) { assert(false); }
void register_apt() { assert(false); }
void register_cam() { assert(false); }
void register_cfg() { assert(false); }
void register_dsp() { assert(false); }
void register_fs() { assert(false); }
void register_gsp() { assert(false); }
void register_hid() { assert(false); }
void register_ro() { assert(false); }
void register_y2r() { assert(false); }
int mii_maker_place(u32) { assert(false); return -1; }
std::vector<u8> system_archive_file(u64, const std::string &) { assert(false); return {}; }
}


static constexpr u32 cb = 0x1000;
static void call(kernel::Service &service, u32 cmd, std::initializer_list<u32> args = {}) {
    memset(gptr(cb), 0xa5, 0x100);
    w32(cb, cmd << 16 | (u32)args.size() << 6);
    unsigned at = 1;
    for (u32 arg : args) w32(cb + 4 * at++, arg);
    kernel::IPC ipc(nullptr, cb, nullptr);
    service.handle(ipc);
}
static void reply(u32 cmd, u32 normal, u32 result, u32 translate = 0) {
    assert(r32(cb) == (cmd << 16 | normal << 6 | translate));
    assert(r32(cb + 4) == result);
    // IPC::set must emit guest little-endian bytes, including on the Wii U.
    assert(memory[cb] == (translate | normal << 6) % 256);
    assert(memory[cb + 4] == (result & 255));
    assert(memory[cb + 7] == (result >> 24));
}
static void offline(kernel::Uds &uds) {
    call(uds, 0xb);
    reply(0xb, 13, 0);
    assert(r32(cb + 8) == 3);
    for (unsigned at = 12; at < 56; ++at) assert(memory[cb + at] == 0);
    assert(memory[cb + 56] == 0xa5); // exactly 0x30 bytes of ConnectionStatus
}
static void scan(kernel::Uds &uds) {
    auto request = [&](u32 size, u32 capacity, u32 rights, u32 address) {
        std::fill(memory.begin(), memory.end(), 0xcc);
        w32(cb, 0xf0404); w32(cb + 4, size);
        w32(cb + 76, capacity << 4 | rights); w32(cb + 80, address);
        kernel::IPC ipc(nullptr, cb, nullptr); uds.handle(ipc);
    };
    auto reject = [&](u32 size, u32 cap, u32 rights, u32 addr, u32 error) {
        request(size, cap, rights, addr); reply(0xf, 1, error);
        for (unsigned at = 0; at < memory.size(); ++at)
            if (at < cb || at >= cb + 0x100) assert(memory[at] == 0xcc);
    };
    for (u32 size = 0; size < 12; ++size) reject(size, 12, 12, 0x4000, kernel::RES_INVALID_COMBINATION);
    reject(12, 11, 12, 0x4000, kernel::RES_INVALID_COMBINATION);
    for (u32 rights : {0u, 2u, 8u, 10u, 13u, 15u}) reject(12, 12, rights, 0x4000, kernel::RES_INVALID_COMBINATION);
    for (u32 addr : {0u, 0x4ffcu, 0x6000u, 0xfffffff8u}) reject(12, 12, 12, addr, kernel::RES_INVALID_ADDRESS);
    reject(64, 64, 12, 0x4fe8, kernel::RES_INVALID_ADDRESS);
    for (u32 rights : {12u, 14u}) {
        request(64, 80, rights, 0x4001); reply(0xf, 1, 0, 2);
        assert(r32(cb + 8) == (80 << 4 | rights) && r32(cb + 12) == 0x4001);
        assert(r32(0x4001) == 64 && r32(0x4005) == 12 && r32(0x4009) == 0);
        assert(memory[0x4000] == 0xcc);
        for (unsigned at = 0x400d; at < 0x4100; ++at) assert(memory[at] == 0xcc);
    }
}
static void microphone_clock() {
    constexpr u64 ms = 1000000;
    kernel::Mic mic("mic");
    mic.mem = std::make_shared<kernel::SharedMem>();
    mic.mem->backing = 0x2000;
    mic.mem->size = 0x1000;
    mic.rate = 8182;
    mic.end = 163;
    mic.sampling = true;
    mic.next_tick = 10 * ms;
    kernel::fake_time = 35 * ms;
    mic.tick(mic.generation);
    assert(kernel::scheduled == 20 * ms && mic.pos == 81 && mic.sampling);
    mic.tick(mic.generation);
    assert(mic.pos == 163 && r32(0x2ffc) == 163 && mic.buf_full->signaled && !mic.sampling);

    mic.loop = mic.sampling = true;
    mic.next_tick = 30 * ms;
    kernel::fake_time = 200 * ms;
    mic.tick(mic.generation);
    assert(kernel::scheduled == 110 * ms && mic.pos == 82);
    kernel::fake_time = 0;
}

int main() {
    microphone_clock();
    auto wireless = [](u32 size, u32 capacity, u32 address, u32 error) {
        kernel::static_size = capacity;
        kernel::static_address = address;
        std::fill(memory.begin(), memory.end(), 0xcc);
        w32(cb, 0x00450040);
        w32(cb + 4, size);
        kernel::IPC ipc(nullptr, cb, nullptr);
        kernel::get_wireless_reboot_info(ipc);
        reply(0x45, 1, error, error ? 0 : 2);
        if (!error) {
            assert(r32(cb + 8) == (size << 14 | 2));
            assert(r32(cb + 12) == address);
        }
        for (u32 at = 0x4000; at < 0x5000; ++at)
            assert(memory[at] == (!error && at >= address && at - address < size ? 0 : 0xcc));
    };
    wireless(16, 15, 0x4500, kernel::RES_INVALID_COMBINATION);
    wireless(17, 64, 0x4500, kernel::RES_INVALID_COMBINATION);
    wireless(16, 64, 0x4ff8, kernel::RES_INVALID_ADDRESS);
    wireless(16, 64, 0xfffffff8, kernel::RES_INVALID_ADDRESS);
    wireless(16, 64, 0x4500, 0);
    kernel::DlpSrvr dlp("dlp:SRVR");
    for (u64 id : {0ull, 0x0004000000030800ull, 0x0004000100030801ull}) {
        g_game.program_id = id;
        call(dlp, 0xe, {0});
        reply(0xe, 2, 0);
        assert(r32(cb + 8) == (id == 0x0004000100030801ull));
    }
    // With no room configuration, initialization must not pretend to start
    // a Download Play server that can never leave state zero.
    call(dlp, 0x0f, {1, 2, 3});
    reply(0x0f, 1, 0xc940ac04);
    call(dlp, 3);
    reply(3, 1, 0xc940ac04);
    call(dlp, 2); reply(2, 1, 0);
    kernel::DlpFkcl fkcl("dlp:FKCL");
    for (u32 cmd : {1u, 0x11u, 5u, 7u, 8u, 0xbu, 0xcu}) {
        call(fkcl, cmd, {0xdeadbeef}); reply(cmd, 1, 0xc940ac04);
    }
    for (u32 cmd : {2u, 6u, 0x10u}) {
        call(fkcl, cmd); reply(cmd, 1, 0);
    }
    kernel::Uds uds("nwm::UDS");
    call(uds, 6); // EjectSpectators must never become ConnectNetwork.
    offline(uds); // red on the old production scaffold
    for (u32 init : {1u, 0x1bu}) {
        call(uds, init); reply(init, 1, 0, 2);
        assert(r32(cb + 8) == 0 && r32(cb + 12) != 0);
        assert(kernel::returned_handles.back() == uds.status && !uds.status->signaled);
        offline(uds);
    }
    for (u32 cmd : {4u, 9u, 0x1du, 0x1eu, 0xeu, 0x1fu, 0x10u, 0x15u, 0x16u}) {
        const auto calls_before = kernel::unsupported_calls;
        call(uds, cmd); reply(cmd, 1, 0xd8c113f4); offline(uds);
        assert(kernel::unsupported_calls == calls_before + 1);
    }
    call(uds, 5, {2}); reply(5, 1, 0xe0a113ea); offline(uds);
    call(uds, 5, {1}); reply(5, 1, 0xe10113ea); offline(uds);
    for (u32 cmd : {6u, 7u}) { call(uds, cmd); reply(cmd, 1, 0); offline(uds); }
    assert(!uds.status->signaled && !uds.bind->signaled);
    call(uds, 8); reply(8, 1, 0xc8a111ea); offline(uds);
    for (u32 cmd : {0x14u, 0x17u}) {
        std::fill(memory.begin() + 0x4000, memory.begin() + 0x5000, 0xcc);
        call(uds, cmd, {1, 64, 64}); reply(cmd, 1, 0xc8a113ea);
        for (unsigned at = 0x4000; at < 0x5000; ++at) assert(memory[at] == 0xcc);
        offline(uds);
    }
    call(uds, 0xd, {2}); reply(0xd, 1, 0xc90113fa);
    call(uds, 0x1a); reply(0x1a, 2, 0); assert(r32(cb + 8) == 0);
    call(uds, 0x11, {64}); reply(0x11, 2, 0, 2);
    assert(r32(cb + 8) == 0 && r32(cb + 12) == 2 && r32(cb + 16) == 0x4500);
    for (auto args : {std::vector<u32>{0, 0x5f4, 1}, {1, 0x5f4, 0}}) {
        call(uds, 0x12, {args[0], args[1], args[2], 0xffff}); reply(0x12, 1, 0xe10113ea);
    }
    call(uds, 0x12, {1, 0x5f3, 1, 0xffff}); reply(0x12, 1, 0xe10113e9);
    call(uds, 0x12, {1, 0x5f4, 1, 0xffff}); reply(0x12, 1, 0, 2);
    assert(kernel::returned_handles.back() == uds.bind && !uds.bind->signaled);
    call(uds, 0x13, {0}); reply(0x13, 1, 0xe10113ea);
    assert(!uds.bind->signaled);
    call(uds, 0x13, {0x12345678}); reply(0x13, 5, 0);
    assert(r32(cb + 8) == 0x12345678);
    for (unsigned at = 12; at < 24; ++at) assert(memory[cb + at] == 0);
    assert(uds.bind->signaled);
    call(uds, 0x13, {0}); reply(0x13, 1, 0xe10113ea);
    assert(uds.bind->signaled); // Invalid Unbind must neither clear nor signal the event.
    call(uds, 0x12, {1, 0x5f4, 1, 0xffff}); reply(0x12, 1, 0, 2);
    assert(!uds.bind->signaled);
    scan(uds);
    call(uds, 0xa); reply(0xa, 1, 0); offline(uds); assert(uds.status->signaled);
    call(uds, 0x13, {1}); reply(0x13, 5, 0); assert(uds.bind->signaled);
    call(uds, 0x12, {1, 0x5f4, 1, 0xffff}); reply(0x12, 1, 0, 2);
    assert(!uds.bind->signaled);
    call(uds, 3); reply(3, 1, 0); offline(uds);
    assert(uds.bind->signaled && !uds.status->signaled); // Shutdown must release a packet waiter.
    call(uds, 0x1b); reply(0x1b, 1, 0, 2);
    assert(!uds.bind->signaled); // A new session has no cancelled packet event.
    puts("offline UDS: real IPC command IDs, no false connections, errors, reply layout, events and bounded empty scans passed");
}
