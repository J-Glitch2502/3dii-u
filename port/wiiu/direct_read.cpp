// The game's data, read past Aroma. ContentRedirection hooks coreinit's FSA calls in every title, even with nothing to redirect, and each FSAReadFile it sees takes
// memory from the one heap all of Aroma shares under one spin lock. When that lock gets stuck, the read never comes back: a friend's NSMB2 froze entering 1-2 with a
// RomFS read 30 s old (2026-11-06), and Layton's freeze of 11-05 was caught in the same place. The hook is on the function, not on IOS, so this makes the request
// FSAReadFile would have made and sends it to IOS itself. The file is opened the ordinary way, once. A mod redirecting this title would hand back a handle IOS doesn't know.
// So: three stretches of the file have to read the same as through the ordinary path before this is used at all, and any failure later goes back to that path for good.
#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "rt.h"
#include "direct_source.h"
#include "wiiu.h"

namespace wiiu {
namespace {
constexpr u32 CHUNK = 1 << 20;  // IOS writes whole cache lines into it: the game's own buffers can start anywhere
FSAClientHandle client = (FSAClientHandle)-1;
FSAFileHandle file = (FSAFileHandle)-1;
FSAShimBuffer *shim;
u8 *chunk;
bool on;
u64 source_offset, source_size;
std::mutex read_m;
bool read_ahead;  // opt-in for this immutable development release
u64 cached_at, previous_end = ~0ULL;
u32 cached_len;

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
    if (on) return;
    fsa_direct_report();
    // A WUHB's /vol/content is virtual. Its FSA handles are not IOS handles.
    // The publisher records the byte range of this same romfs.bin in the existing
    // bundle. Three comparisons against the mounted file must agree before use.
    std::string physical = !strncmp(path, "fs:", 3) ? path + 3 : path;
    source_offset = 0;
    source_size = size;
    u64 expected_backing = 0;
    if (FILE *source = fopen("fs:/vol/content/direct-romfs-source.bin", "rb")) {
        u8 descriptor[673];
        const size_t n = fread(descriptor, 1, sizeof descriptor, source);
        const bool read_ok = !ferror(source);
        fclose(source);
        direct_source::Source parsed;
        if (!read_ok || !direct_source::parse(descriptor, n, parsed) || parsed.length != size) {
            OSReport("[3dii-u fs] direct reads off: invalid bundle source descriptor\n");
            return;
        }
        physical = parsed.path;
        source_offset = parsed.offset;
        expected_backing = parsed.backing_size;
    }
    if (!size || size > UINT32_MAX) return;
    shim = (FSAShimBuffer *)memalign(0x40, sizeof(FSAShimBuffer));
    chunk = (u8 *)memalign(0x40, CHUNK);
    auto cleanup = [&] {
        if (file != (FSAFileHandle)-1 && client >= 0) FSACloseFile(client, file);
        if (client >= 0) FSADelClient(client);
        file = (FSAFileHandle)-1; client = (FSAClientHandle)-1;
        free(shim); free(chunk); shim = nullptr; chunk = nullptr;
    };
    if (!shim || !chunk) { cleanup(); return; }
    memset(shim, 0, sizeof *shim);
    client = FSAAddClient(nullptr);
    if (client < 0) {
        OSReport("[3dii-u fs] direct reads off: no FSA client, %d\n", (int)client);
        cleanup(); return;
    }
    const FSError opened = FSAOpenFileEx(client, physical.c_str(), "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &file);
    if (opened != FS_ERROR_OK) {
        file = (FSAFileHandle)-1;
        OSReport("[3dii-u fs] direct reads off: open %s failed, %d\n", physical.c_str(), (int)opened);
        cleanup(); return;
    }
    if (expected_backing) {
        FSAStat st {};
        if (FSAGetStatFile(client, file, &st) != FS_ERROR_OK || st.size != expected_backing) {
            OSReport("[3dii-u fs] direct reads off: bundle size changed\n");
            cleanup(); return;
        }
    }
    static u8 want[4096];
    const u32 n = (u32)std::min<u64>(size, sizeof want);
    const u32 at[3] = {0, (u32)((size - n) / 2) & ~63u, (u32)(size - n)};
    for (u32 i = 0; i < 3; i++) {
        if (pread(fd, want, n, (off_t)at[i]) != (ssize_t)n || !read_chunk(n, (u32)(source_offset + at[i])) || memcmp(want, chunk, n)) {
            OSReport("[3dii-u fs] direct reads off: sample %u at %08x didn't match\n", i, at[i]);
            cleanup(); return;
        }
    }
    on = true;
    if (FILE *flag = fopen("fs:/vol/content/romfs-readahead-enable", "rb")) {
        fclose(flag);
        read_ahead = true;
        LOG("[romfs-cache] bounded sequential read-ahead enabled (256 KB); immutable RomFS only\n");
    }
    OSReport("[3dii-u fs] direct RomFS enabled: %s offset=%llu size=%llu, all 3 samples matched\n",
             physical.c_str(), (unsigned long long)source_offset, (unsigned long long)source_size);
}

bool direct_read(void *dst, u32 len, u64 off, bool *cached) {
    if (cached) *cached = false;
    std::lock_guard<std::mutex> lock(read_m);
    if (!on || !direct_source::fits(source_size, off, len)) return false;
    bool cache_only = read_ahead;
    for (u32 done = 0; done < len;) {
        const u64 at = off + done;
        if (read_ahead && at >= cached_at && at - cached_at < cached_len) {
            const u32 inside = (u32)(at - cached_at);
            const u32 n = std::min(len - done, cached_len - inside);
            memcpy((u8 *)dst + done, chunk + inside, n);
            done += n;
            continue;
        }
        const u32 n = std::min(len - done, CHUNK);
        cache_only = false;
        // Begin with the exact request after a seek. Only an observed
        // sequential successor fetches extra data, bounded by the RomFS end.
        const u32 wanted = read_ahead && at == previous_end ? std::max(n, 256u << 10) : n;
        const u32 fetched = (u32)std::min<u64>(source_size - at, wanted);
        cached_len = 0;  // IOS may partially overwrite the buffer on failure
        if (!read_chunk(fetched, (u32)(source_offset + at))) {
            OSReport("[3dii-u fs] direct read of %u at %08x failed: back to the system's file calls\n", fetched, (u32)at);
            on = false;
            return false;
        }
        cached_at = at;
        cached_len = fetched;
        memcpy((u8 *)dst + done, chunk, n);
        done += n;
    }
    previous_end = off + len;
    if (cached) *cached = cache_only;
    return true;
}
}  // namespace wiiu
