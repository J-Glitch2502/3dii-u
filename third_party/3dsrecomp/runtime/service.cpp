// Service registry, IPC helpers and srv: (the service manager).
#include "service.h"
#include "loader.h"

#include <set>
#ifdef __WIIU__
#include <coreinit/debug.h>
namespace kernel { bool room_wireless_available(); }
#endif

namespace kernel {

static std::map<std::string, std::shared_ptr<Service>> services;
static std::map<std::string, std::shared_ptr<Service>> ports;
#ifdef RECOMP_RESUMABLE
static std::map<Service *, u64> checkpoint_calls;
#endif

int IPC::handles(int pos, std::initializer_list<ObjPtr> objs, bool move) {
    set(pos++, (((u32)objs.size() - 1) << 26) | (move ? 0x10 : 0));
    for (auto &o : objs) set(pos++, o ? add_handle(o) : 0);
    return pos;
}

u32 IPC::static_buffer(int index, u32 *size) const {
    u32 d = r32(t->tls + 0x180 + 8 * index);
    if (size) *size = d >> 14;
    return r32(t->tls + 0x180 + 8 * index + 4);
}

void IPC::static_desc(int pos, int index, u32 size) {
    set(pos, (size << 14) | ((u32)index << 10) | 2);
    set(pos + 1, static_buffer(index));
}

void Service::unimplemented(IPC &ipc, bool succeed) {
    static std::set<std::pair<std::string, u32>> seen;
    if (seen.insert({name, ipc.word(0)}).second)
        LOG("[%s] unimplemented command 0x%08x (%s)\n", name.c_str(), ipc.word(0), succeed ? "returning success" : "failing");
    if (succeed) {
        // A query it answers this way reads as zero (no, none, empty), never as the words its own request left in the buffer: read as an
        // answer, those started Luigi's Mansion: Dark Moon as a Download Play child (dlp:SRVR IsChild, misc.cpp)
        ipc.ok(1);
        for (int i = 2; i < 64; i++) ipc.set(i, 0);
    } else {
        ipc.result(0xD900182F);
    }
}

void register_service(std::shared_ptr<Service> s) { services[s->name] = s; }

std::shared_ptr<Service> find_service(const std::string &name) {
    auto it = services.find(name);
    return it == services.end() ? nullptr : it->second;
}

std::shared_ptr<Service> find_port(const std::string &name) {
    auto it = ports.find(name);
    return it == ports.end() ? nullptr : it->second;
}

Handle new_session(std::shared_ptr<Service> s) {
    auto ses = std::make_shared<Session>();
    ses->service = s;
    ses->name = s->name;
    return add_handle(ses);
}

u32 service_request(std::shared_ptr<Session> s, Thread *t, Lock &lk) {
    IPC ipc(t, t->tls + 0x80, &lk);
    if (g_verbose >= 2) LOG("[ipc] %s cmd %08x\n", s->service->name.c_str(), ipc.word(0));
    u32 cmd = ipc.word(0);
    if (kstats.ipc) {
        char k[48];
        snprintf(k, sizeof k, "%s:%04x", s->service->name.c_str(), cmd >> 16);
        kstats.requests[k]++;
    }
    t->in_service = s->service->name.c_str();
    t->in_cmd = cmd;
#ifdef RECOMP_RESUMABLE
    ++checkpoint_calls[s->service.get()];
#endif
#ifdef __WIIU__
    const auto &service_name = s->service->name;
    static std::map<std::pair<std::string, u32>, u32> wireless_counts;
    const bool wireless_service = g_verbose && (g_game.program_id == 0x0004000000176f00ULL ||
        g_game.program_id == 0x0004000000030800ULL) &&
        (service_name.rfind("ac:", 0) == 0 || service_name.rfind("nwm:", 0) == 0 ||
         service_name.rfind("ndm:", 0) == 0 || service_name.rfind("dlp:", 0) == 0 ||
         service_name.rfind("APT:", 0) == 0 || service_name.rfind("cfg:", 0) == 0);
    const u32 count = wireless_service ? ++wireless_counts[{service_name, cmd}] : 0;
    const bool wireless_trace = wireless_service && (count <= 4 || (count & 255) == 0);
    if (wireless_trace)
        OSReport("[wireless-ipc] %s request=%08x arg1=%08x arg2=%08x guest-thread=%u\n",
            service_name.c_str(), cmd, ipc[1], ipc[2], t->id);
#endif
    s->service->handle(ipc);
#ifdef __WIIU__
    if (wireless_trace || (wireless_service && (r32(ipc.buf + 4) & 0x80000000)))
        OSReport("[wireless-ipc] %s reply=%08x result=%08x value=%08x room-ready=%u\n",
            service_name.c_str(), r32(ipc.buf), r32(ipc.buf + 4), r32(ipc.buf + 8), room_wireless_available());
#endif
    const u32 reply_result = r32(ipc.buf + 4);
    if (g_verbose && (reply_result & 0x80000000))
        LOG("[ipc] %s cmd %08x -> error %08x\n", s->service->name.c_str(), cmd, reply_result);
    return RES_OK;
}

// ---------------------------------------------------------------- srv:
struct Srv : Service {
    SERVICE_CHECKPOINT(Srv, notif, pending)
    std::shared_ptr<Semaphore> notif = std::make_shared<Semaphore>();
    std::vector<u32> pending;
    Srv() : Service("srv:") { notif->max = 64; }
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001:  // RegisterClient
            ipc.ok();
            break;
        case 0x0002:  // EnableNotification
            ipc.ok(1, 2);
            ipc.handles(2, {notif});
            break;
        case 0x0005: {  // GetServiceHandle(name[8], len, flags)
            char nm[9] = {};
            memcpy(nm, gptr(ipc.buf + 4), 8);
            u32 len = std::min<u32>(ipc[3], 8);
            std::string name(nm, len);
            auto svc = find_service(name);
            if (!svc) {
                LOG("[srv] GetServiceHandle(%s): not implemented, giving a stub\n", name.c_str());
                svc = make_stub(name);
            }
            VLOG("[srv] GetServiceHandle(%s)\n", name.c_str());
            auto ses = std::make_shared<Session>();
            ses->service = svc;
            ses->name = name;
            ipc.ok(1, 2);
            ipc.handles(2, {ses}, true);
            break;
        }
        case 0x0009:  // Subscribe
        case 0x000A:  // Unsubscribe
            ipc.ok();
            break;
        case 0x000B:  // ReceiveNotification
            ipc.ok(2);
            ipc.set(2, pending.empty() ? 0 : pending.front());
            if (!pending.empty()) pending.erase(pending.begin());
            break;
        case 0x000C:  // PublishToSubscriber
            ipc.ok();
            break;
        default:
            unimplemented(ipc);
        }
    }
    std::shared_ptr<Service> make_stub(const std::string &name);
};

struct StubService : Service {
    SERVICE_CHECKPOINT(StubService)
    explicit StubService(std::string n) : Service(std::move(n)) {}
    void handle(IPC &ipc) override { unimplemented(ipc); }
};

std::shared_ptr<Service> Srv::make_stub(const std::string &name) {
    auto s = std::make_shared<StubService>(name);
    register_service(s);
    return s;
}

u64 g_missing_system_archive;

// 14002 is the shared font, and 14102 to 14302 China's, Korea's and Taiwan's
const char *system_archive_name(u64 tid) {
    return tid == 0x0004009B00010202ull ? "the Mii data" : tid == 0x000400DB00010302ull ? "the bad-word list" :
           (tid & ~0xF00ull) == 0x0004009B00014002ull ? "the shared font" : "a system file";
}

struct ErrF : Service {
    SERVICE_CHECKPOINT(ErrF)
    ErrF() : Service("err:f") {}
    void handle(IPC &ipc) override {
        if (ipc.cmd() == 1) {  // ThrowFatalError
            u32 type = ipc[1] & 0xFF;
            u32 code = ipc[2];
            u32 pc = ipc[3];
            // where it was called from: words on the thread's stack that point just past a call in the game's code (innermost first)
            std::string calls;
            const u32 sp = ipc.t->cpu.r[13], end = sp < mem::STACK_TOP ? std::min(sp + 0x1000, mem::STACK_TOP) : sp + 0x1000;
            for (u32 a = sp & ~3u, n = 0; a < end && n < 12; a += 4) {
                const u32 v = r32(a);
                if (v & 3 || v < g_game.text_addr + 4 || v >= g_game.text_addr + g_game.text_size) continue;
                const u32 w = r32(v - 4);  // bl, blx immediate, blx register
                if ((w & 0x0F000000) != 0x0B000000 && (w & 0xFE000000) != 0xFA000000 && (w & 0x0FFFFFF0) != 0x012FFF30) continue;
                char b[12];
                snprintf(b, sizeof b, " %08x", v - 4);
                calls += b;
                n++;
            }
            // said first, as a console's screen shows only the start of the message; only for FS's "not found", the failure a missing archive gives
            if (const u64 tid = g_missing_system_archive; tid && code == 0xC8804478u)
                FATAL("This game needs %s from your 3DS (system archive %016llx), and its build doesn't have it. Build it again with your 3DS system "
                      "files.\n[err:f] fatal error type %u result %08x pc %08x lr %08x, calls from%s\n", system_archive_name(tid), (unsigned long long)tid,
                      type, code, pc, ipc[0x14 / 4], calls.c_str());
            FATAL("[err:f] fatal error type %u result %08x pc %08x lr %08x, calls from%s\n", type, code, pc, ipc[0x14 / 4], calls.c_str());
        }
        ipc.ok();
    }
};

void register_all_services();

void init_services() {
    ports["srv:"] = std::make_shared<Srv>();
    ports["err:f"] = std::make_shared<ErrF>();
    register_all_services();
}


#ifdef RECOMP_RESUMABLE
void checkpoint_services(checkpoint::Image &im) {
    if (im.archive) {
        std::vector<std::shared_ptr<Service>> selected;
        if (!im.archive->reading) {
            for (auto &[name, service] : services) {
                if (service->persistent_supported()) selected.push_back(service);
                else if (checkpoint_calls[service.get()]) im.archive->fail("an unsupported service was used before saving");
            }
        }
        im.archive->io(selected);
        if (!im.archive->ok) im.ok = false;
        return;
    }
    for (auto &[name, service] : services) {
        if (!im.objects.insert(service.get()).second) continue;
        if (!service->capture(im)) {
            const auto calls = checkpoint_calls[service.get()];
            im.validate.push_back([service, calls] {
                if (checkpoint_calls[service.get()] == calls) return true;
                LOG("[state] validation failed: unsupported service %s changed (%llu -> %llu)\n", service->name.c_str(), (unsigned long long)calls, (unsigned long long)checkpoint_calls[service.get()]);
                return false;
            });
        }
    }
}
#endif

}  // namespace kernel

#ifdef RECOMP_RESUMABLE
namespace kernel {
std::shared_ptr<Service> persistent_service(const std::string &name) {
    if (auto s = find_service(name)) return s;
    if (auto s = find_port(name)) return s;
    return persistent_file_service(name);
}
}
#endif
