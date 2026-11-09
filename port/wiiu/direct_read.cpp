#include <coreinit/filesystem_fsa.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include "fsa_operations.hpp"
#include "fsa_route.hpp"
#include "fsa_transport.hpp"
#include "rt.h"
#include "wiiu.h"

namespace wiiu {
namespace {
constexpr u32 CHUNK = 1 << 20;
FSAClientHandle client = -1;
FSAFileHandle file = (FSAFileHandle)-1;
FSAShimBuffer *shim;
u8 *chunk;
bool on;
std::mutex ReadMutex;

void ResetDirectRead() {
    on = false;
    if (client >= 0 && file != (FSAFileHandle)-1) fsa::Close(client, file);
    file = (FSAFileHandle)-1;
    if (client >= 0) FSADelClient(client);
    client = -1;
    std::free(chunk);
    chunk = nullptr;
    std::free(shim);
    shim = nullptr;
}

bool ReadChunk(u32 n, u32 pos) {
    return fsa::ReadAt(client, file, *shim, chunk, n, pos) == (FSError)n;
}

} // namespace

void direct_read_open(const char *path, int fd, u64 size) {
    std::lock_guard<std::mutex> lock(ReadMutex);
    ResetDirectRead();
    fsa_direct_report();
    if (!path || !fsa::IsDirectRoute() || size > UINT32_MAX || fd < 0) return;
    shim = (FSAShimBuffer *)memalign(0x40, sizeof(FSAShimBuffer));
    chunk = (u8 *)memalign(0x40, CHUNK);
    if (!shim || !chunk) {
        ResetDirectRead();
        return;
    }
    memset(shim, 0, sizeof *shim);
    client = FSAAddClient(nullptr);
    if (client < 0) {
        LOG("[fs] direct reads off: no FSA client, %d\n", (int)client);
        ResetDirectRead();
        return;
    }
    const FSError opened =
        FSAOpenFileEx(client, !strncmp(path, "fs:", 3) ? path + 3 : path, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &file);
    if (opened != FS_ERROR_OK) {
        LOG("[fs] direct reads off: open failed, %d\n", (int)opened);
        ResetDirectRead();
        return;
    }
    static u8 want[4096];
    const u32 n = (u32)std::min<u64>(size, sizeof want);
    const u32 at[3] = {0, (u32)(size / 2) & ~63u, (u32)(size - n)};
    for (u32 i = 0; i < 3; i++) {
        if (pread(fd, want, n, (off_t)at[i]) != (ssize_t)n || !ReadChunk(n, at[i]) || memcmp(want, chunk, n)) {
            LOG("[fs] direct reads off: stretch %u at %08x didn't match\n", i, at[i]);
            ResetDirectRead();
            return;
        }
    }
    on = true;
    LOG("[fs] game data read directly, past Aroma's hook\n");
}

bool direct_read(void *dst, u32 len, u64 off) {
    std::lock_guard<std::mutex> lock(ReadMutex);
    if (!on) return false;
    if (!fsa::IsDirectRoute()) {
        ResetDirectRead();
        return false;
    }
    if (!fsa::ValidRange(off, len) || (len && !dst)) return false;
    for (u32 done = 0; done < len;) {
        if (!fsa::IsDirectRoute()) {
            ResetDirectRead();
            return false;
        }
        const u32 n = std::min(len - done, CHUNK);
        if (!ReadChunk(n, (u32)off + done)) {
            LOG("[fs] direct read of %u at %08x failed: back to the system's file calls\n", n, (u32)off + done);
            ResetDirectRead();
            return false;
        }
        memcpy((u8 *)dst + done, chunk, n);
        done += n;
    }
    return true;
}
} // namespace wiiu
