#include <cstring>

#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "fsa_operations.hpp"
#include "fsa_route.hpp"
#include "fsa_route_state.hpp"
#include "fsa_transport.hpp"
#include "rt.h"
#include "wiiu.h"

extern "C" {
FSError __real_FSAOpenFileEx(FSAClientHandle, const char *, const char *, FSMode, FSOpenFileFlags, uint32_t, FSAFileHandle *);
FSError __real_FSACloseFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSAReadFile(FSAClientHandle, void *, uint32_t, uint32_t, FSAFileHandle, FSAReadFlag);
FSError __real_FSAGetStatFile(FSAClientHandle, FSAFileHandle, FSAStat *);
FSError __real_FSAGetStat(FSAClientHandle, const char *, FSAStat *);
FSError __real_FSARemove(FSAClientHandle, const char *);
}

namespace wiiu::fsa {
const char *const ProbeFile = "/vol/content/shaders.idx";
RouteController Routing;
std::atomic<OSThread *> ProbeOwner{nullptr};

const char *Probe(FSAClientHandle c) {
    alignas(64) static u8 a[256], b[256];
    FSAFileHandle real = (FSAFileHandle)-1, mine = (FSAFileHandle)-1;
    if (__real_FSAOpenFileEx(c, ProbeFile, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &real) != FS_ERROR_OK) {
        return "the check file wouldn't open";
    }
    const char *reason = "open failed";
    if (wiiu::fsa::Open(c, ProbeFile, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &mine) == FS_ERROR_OK) {
        FSAStat st_real, st_mine;
        memset(a, 0, sizeof a);
        memset(b, 1, sizeof b);
        const FSError ra = __real_FSAReadFile(c, a, 1, sizeof a, real, FSA_READ_FLAG_NONE),
                      rb = wiiu::fsa::Read(c, b, 1, sizeof b, mine, FSA_READ_FLAG_NONE);
        if (ra <= 0 || ra != rb)
            reason = "read counts differ";
        else if (memcmp(a, b, (size_t)ra))
            reason = "read data differs";
        else if (__real_FSAGetStatFile(c, real, &st_real) != FS_ERROR_OK || wiiu::fsa::StatFile(c, mine, &st_mine) != FS_ERROR_OK ||
                 st_real.size != st_mine.size)
            reason = "stat differs";
        else if (wiiu::fsa::Seek(c, mine, 64) != FS_ERROR_OK || wiiu::fsa::Read(c, b, 1, 64, mine, FSA_READ_FLAG_NONE) != 64 ||
                 memcmp(a + 64, b, 64))
            reason = "seek differs";
        else
            reason = nullptr;
        wiiu::fsa::Close(c, mine);
    }
    __real_FSACloseFile(c, real);
    return reason;
}

bool UseDirect(FSAClientHandle c) {
    const RouteKind route = Routing.GetRoute();
    if (route == RouteKind::Direct) return true;
    if (route == RouteKind::System) return false;
    if (route == RouteKind::Unknown && Routing.TryBeginProbe()) {
        ProbeOwner.store(OSGetCurrentThread(), std::memory_order_release);
        Routing.CompleteProbe(Probe(c));
        ProbeOwner.store(nullptr, std::memory_order_release);
    } else if (Routing.GetRoute() == RouteKind::Checking) {
        if (ProbeOwner.load(std::memory_order_acquire) == OSGetCurrentThread()) {
            Routing.CancelProbe("recursive route probe");
            return false;
        }
        for (int i = 0; i < 1000 && Routing.GetRoute() == RouteKind::Checking; ++i) OSSleepTicks(OSMillisecondsToTicks(2));
        if (Routing.GetRoute() == RouteKind::Checking) Routing.CancelProbe("route probe timed out");
    }
    return Routing.IsDirect();
}

} // namespace wiiu::fsa

namespace wiiu {
void fsa_direct_check_writes() {
    static const FSAClientHandle c = FSAAddClient(nullptr);
    if (c < 0 || !fsa::UseDirect(c)) return;
    const char *const name = "/vol/save/common/fsa_check.bin";
    alignas(64) static u8 out[256], back[256];
    for (u32 i = 0; i < sizeof out; i++) out[i] = (u8)(i * 7 + 1);
    FSAFileHandle f = (FSAFileHandle)-1;
    FSAStat st;
    const char *wrong = nullptr;
    if (wiiu::fsa::Open(c, name, "w", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &f) != FS_ERROR_OK)
        wrong = "making a file failed";
    else {
        if (wiiu::fsa::Write(c, out, 1, sizeof out, f, FSA_WRITE_FLAG_NONE) != (FSError)sizeof out) wrong = "writing failed";
        wiiu::fsa::Close(c, f);
    }
    if (!wrong) {
        memset(back, 0, sizeof back);
        if (__real_FSAOpenFileEx(c, name, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &f) != FS_ERROR_OK)
            wrong = "what was written isn't there";
        else {
            if (__real_FSAReadFile(c, back, 1, sizeof back, f, FSA_READ_FLAG_NONE) != (FSError)sizeof back || memcmp(out, back, sizeof out))
                wrong = "what was written reads differently";
            __real_FSACloseFile(c, f);
        }
    }
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    wiiu::fsa::PutPath(s.request.remove.path, name);
    const FSError removed = wiiu::fsa::Ask(c, FSA_COMMAND_REMOVE, s);
    if (!wrong && (removed != FS_ERROR_OK || __real_FSAGetStat(c, name, &st) == FS_ERROR_OK)) wrong = "removing failed";
    if (wrong) {
        __real_FSARemove(c, name);
        fsa::Routing.DisableDirect(wrong);
    }
}

void fsa_direct_report() {
    const fsa::RouteStatus status = fsa::Routing.GetStatus();
    if (status.Route == fsa::RouteKind::Direct)
        LOG("[fs] every file call goes straight to the file system, past Aroma's hook\n");
    else
        LOG("[fs] file calls go through the system's functions: %s\n", status.Reason);
}

bool fsa_direct_flush_quota(const char *path) {
    if (!fsa::Routing.IsDirect()) return false;
    static FSAClientHandle client = FSAAddClient(nullptr);
    if (client < 0) return false;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    wiiu::fsa::PutPath(s.request.flushQuota.path, path);
    return wiiu::fsa::Ask(client, FSA_COMMAND_FLUSH_QUOTA, s) == FS_ERROR_OK;
}
} // namespace wiiu
