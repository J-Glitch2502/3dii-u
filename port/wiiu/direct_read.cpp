// The game's data, read past Aroma. ContentRedirection hooks coreinit's FSA calls in every title, even with nothing to redirect, and each FSAReadFile it sees takes
// memory from the one heap all of Aroma shares under one spin lock. When that lock gets stuck, the read never comes back: a friend's NSMB2 froze entering 1-2 with a
// RomFS read 30 s old (2026-11-06), and Layton's freeze of 11-05 was caught in the same place. The hook is on the function, not on IOS, so this makes the request
// FSAReadFile would have made and sends it to IOS itself. The file is opened the ordinary way, once. A mod redirecting this title would hand back a handle IOS doesn't know.
// So: three stretches of the file have to read the same as through the ordinary path before this is used at all, and any failure later goes back to that path for good.
#include <coreinit/cache.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "rt.h"
#include "wiiu.h"

namespace wiiu {
namespace {
constexpr u32 CHUNK = 1 << 20;  // IOS writes whole cache lines into it: the game's own buffers can start anywhere
FSAClientHandle client;
FSAFileHandle file;
FSAShimBuffer *shim;
u8 *chunk;
bool on;

bool read_chunk(u32 n, u32 pos) {
    memset(&shim->request, 0, sizeof shim->request);
    shim->request.readFile.buffer = chunk;
    shim->request.readFile.size = 1;
    shim->request.readFile.count = n;
    shim->request.readFile.pos = pos;
    shim->request.readFile.handle = file;
    shim->request.readFile.readFlags = FSA_READ_FLAG_READ_WITH_POS;
    IOSVec *v = shim->ioctlvVec;
    v[0].vaddr = &shim->request;
    v[0].len = sizeof(FSARequest);
    v[1].vaddr = chunk;
    v[1].len = n;
    v[2].vaddr = &shim->response;
    v[2].len = sizeof(FSAResponse);
    DCFlushRange(&shim->request, sizeof(FSARequest));
    DCFlushRange(v, 3 * sizeof(IOSVec));
    DCInvalidateRange(chunk, n);  // what this read lands on, not the whole megabyte: a 32-line read would otherwise cost 32,000 lines
    DCInvalidateRange(&shim->response, sizeof(FSAResponse));
    const int got = IOS_Ioctlv(client, FSA_COMMAND_READ_FILE, 1, 2, v);
    DCInvalidateRange(chunk, n);
    return got == (int)n;
}
}  // namespace

void direct_read_open(const char *path, int fd, u64 size) {
    fsa_direct_report();  // the game data file is open by now, so the route has been settled
    shim = (FSAShimBuffer *)memalign(0x40, sizeof(FSAShimBuffer));
    chunk = (u8 *)memalign(0x40, CHUNK);
    if (!shim || !chunk || size > 0xFFFFFFFFull) return;
    memset(shim, 0, sizeof *shim);
    client = FSAAddClient(nullptr);
    if (client < 0) {
        LOG("[fs] direct reads off: no FSA client, %d\n", (int)client);
        return;
    }
    const FSError opened = FSAOpenFileEx(client, !strncmp(path, "fs:", 3) ? path + 3 : path, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &file);
    if (opened != FS_ERROR_OK) {
        LOG("[fs] direct reads off: open failed, %d\n", (int)opened);
        return;
    }
    static u8 want[4096];
    const u32 n = (u32)std::min<u64>(size, sizeof want);
    const u32 at[3] = {0, (u32)(size / 2) & ~63u, (u32)(size - n)};
    for (u32 i = 0; i < 3; i++) {
        if (pread(fd, want, n, (off_t)at[i]) != (ssize_t)n || !read_chunk(n, at[i]) || memcmp(want, chunk, n)) {
            LOG("[fs] direct reads off: stretch %u at %08x didn't match\n", i, at[i]);
            return;
        }
    }
    on = true;
    LOG("[fs] game data read directly, past Aroma's hook\n");
}

bool direct_read(void *dst, u32 len, u64 off) {
    if (!on) return false;
    for (u32 done = 0; done < len;) {
        const u32 n = std::min(len - done, CHUNK);
        if (!read_chunk(n, (u32)off + done)) {
            LOG("[fs] direct read of %u at %08x failed: back to the system's file calls\n", n, (u32)off + done);
            on = false;
            return false;
        }
        memcpy((u8 *)dst + done, chunk, n);
        done += n;
    }
    return true;
}
}  // namespace wiiu
