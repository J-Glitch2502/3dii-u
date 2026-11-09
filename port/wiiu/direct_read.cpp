#include <coreinit/filesystem_fsa.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "fsa_transport.hpp"
#include "rt.h"
#include "wiiu.h"

namespace wiiu {
namespace {
constexpr u32 CHUNK = 1 << 20;
FSAClientHandle client;
FSAFileHandle file;
FSAShimBuffer *shim;
u8 *chunk;
bool on;

bool ReadChunk(u32 n, u32 pos) {
    return fsa::ReadAt(client, file, *shim, chunk, n, pos) == (FSError)n;
}

}  // namespace

void direct_read_open(const char *path, int fd, u64 size) {
    fsa_direct_report();
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
        if (pread(fd, want, n, (off_t)at[i]) != (ssize_t)n || !ReadChunk(n, at[i]) || memcmp(want, chunk, n)) {
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
        if (!ReadChunk(n, (u32)off + done)) {
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
