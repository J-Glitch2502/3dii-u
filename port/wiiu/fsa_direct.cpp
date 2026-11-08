// Every file call in the game goes straight to the file system, past Aroma. ContentRedirection hooks coreinit's FSA functions in every title, with nothing to redirect or not, and
// takes memory from the one heap all of Aroma shares under one spin lock for each call. Every freeze caught on a console so far was on that lock with a file call under it, whichever
// thread held it (ftpiiu, a game thread, the shader compiler), FTP on or off. The hook is on the functions, not on IOS, so these make the request a function would have made and send
// it to IOS themselves. The linker points all callers here, the C library's file functions too (--wrap, tools/win/build3ds.py), so nothing can forget to.
// Once, at the first call: the same file through both routes has to agree, or everything stays on the system's functions. Once more when the save storage is up, for writing.
#include <coreinit/cache.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>

#include <cstring>

#include "rt.h"
#include "wiiu.h"

extern "C" {
FSError __real_FSAOpenFileEx(FSAClientHandle, const char *, const char *, FSMode, FSOpenFileFlags, uint32_t, FSAFileHandle *);
FSError __real_FSACloseFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSAReadFile(FSAClientHandle, void *, uint32_t, uint32_t, FSAFileHandle, FSAReadFlag);
FSError __real_FSAWriteFile(FSAClientHandle, const void *, uint32_t, uint32_t, FSAFileHandle, FSAWriteFlag);
FSError __real_FSASetPosFile(FSAClientHandle, FSAFileHandle, FSAFilePosition);
FSError __real_FSAGetStatFile(FSAClientHandle, FSAFileHandle, FSAStat *);
FSError __real_FSAGetStat(FSAClientHandle, const char *, FSAStat *);
FSError __real_FSAFlushFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSATruncateFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSARemove(FSAClientHandle, const char *);
FSError __real_FSAMakeDir(FSAClientHandle, const char *, FSMode);
FSError __real_FSARename(FSAClientHandle, const char *, const char *);
FSError __real_FSAOpenDir(FSAClientHandle, const char *, FSADirectoryHandle *);
FSError __real_FSAReadDir(FSAClientHandle, FSADirectoryHandle, FSADirectoryEntry *);
FSError __real_FSARewindDir(FSAClientHandle, FSADirectoryHandle);
FSError __real_FSACloseDir(FSAClientHandle, FSADirectoryHandle);
}

namespace {
const char *const PROBE_FILE = "/vol/content/shaders.idx";  // in every title of ours, and nothing writes it
enum { UNKNOWN, DIRECT, SYSTEM } route;
const char *why = "not tried";

// IOS answers in the same numbers as FSError, which is what the system's functions hand back
FSError ask(FSAClientHandle c, FSACommand command, FSAShimBuffer &s) {
    DCFlushRange(&s.request, sizeof s.request);
    DCFlushRange(&s.response, sizeof s.response);
    const int r = IOS_Ioctl(c, command, &s.request, sizeof s.request, &s.response, sizeof s.response);
    DCInvalidateRange(&s.response, sizeof s.response);
    return (FSError)r;
}

void put_path(char *to, const char *path) {
    strncpy(to, path, FS_MAX_PATH);
    to[FS_MAX_PATH] = 0;
}

// Read and write have the data in a vector of its own. The range is flushed, not invalidated, so that the rest of a last line that isn't all the file's survives: on a line the
// CPU hasn't written since, flushing does what invalidating would.
FSError transfer(FSAClientHandle c, FSACommand command, void *data, u32 bytes, FSAShimBuffer &s) {
    IOSVec *const v = s.ioctlvVec;
    v[0].vaddr = &s.request;
    v[0].len = sizeof s.request;
    v[1].vaddr = data;
    v[1].len = bytes;
    v[2].vaddr = &s.response;
    v[2].len = sizeof s.response;
    DCFlushRange(&s.request, sizeof s.request);
    DCFlushRange(&s.response, sizeof s.response);
    DCFlushRange(v, 3 * sizeof(IOSVec));
    DCFlushRange(data, bytes);
    const bool reading = command == FSA_COMMAND_READ_FILE;
    const int r = IOS_Ioctlv(c, command, reading ? 1 : 2, reading ? 2 : 1, v);
    if (reading) DCFlushRange(data, bytes);
    return (FSError)r;
}

FSError d_open(FSAClientHandle c, const char *path, const char *mode, FSMode create, FSOpenFileFlags flags, uint32_t prealloc, FSAFileHandle *out) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.openFile.path, path);
    strncpy(s.request.openFile.mode, mode, FS_MODE_LENGTH - 1);
    s.request.openFile.unk0x290 = create;
    s.request.openFile.unk0x294 = flags;
    s.request.openFile.unk0x298 = prealloc;
    s.response.openFile.handle = (FSAFileHandle)-1;
    const FSError r = ask(c, FSA_COMMAND_OPEN_FILE, s);
    *out = r == FS_ERROR_OK ? s.response.openFile.handle : (FSAFileHandle)-1;
    return r;
}

FSError d_read(FSAClientHandle c, void *to, uint32_t size, uint32_t count, FSAFileHandle file, FSAReadFlag flags) {
    if ((uintptr_t)to & 0x3F) return FS_ERROR_INVALID_ALIGNMENT;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.readFile.buffer = (uint8_t *)to;
    s.request.readFile.size = size;
    s.request.readFile.count = count;
    s.request.readFile.handle = file;
    s.request.readFile.readFlags = (FSAReadFlag)(flags & ~FSA_READ_FLAG_READ_WITH_POS);
    return transfer(c, FSA_COMMAND_READ_FILE, to, size * count, s);
}

FSError d_write(FSAClientHandle c, const void *from, uint32_t size, uint32_t count, FSAFileHandle file, FSAWriteFlag flags) {
    if ((uintptr_t)from & 0x3F) return FS_ERROR_INVALID_ALIGNMENT;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.writeFile.buffer = (const uint8_t *)from;
    s.request.writeFile.size = size;
    s.request.writeFile.count = count;
    s.request.writeFile.handle = file;
    s.request.writeFile.writeFlags = (FSAWriteFlag)(flags & ~FSA_WRITE_FLAG_READ_WITH_POS);
    return transfer(c, FSA_COMMAND_WRITE_FILE, (void *)from, size * count, s);
}

FSError d_close(FSAClientHandle c, FSAFileHandle file) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.closeFile.handle = file;
    return ask(c, FSA_COMMAND_CLOSE_FILE, s);
}

FSError d_seek(FSAClientHandle c, FSAFileHandle file, FSAFilePosition pos) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.setPosFile.handle = file;
    s.request.setPosFile.pos = pos;
    return ask(c, FSA_COMMAND_SET_POS_FILE, s);
}

FSError d_statfile(FSAClientHandle c, FSAFileHandle file, FSAStat *out) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.statFile.handle = file;
    const FSError r = ask(c, FSA_COMMAND_STAT_FILE, s);
    if (r == FS_ERROR_OK) memcpy((void *)out, (const void *)&s.response.statFile.stat, sizeof *out);
    return r;
}

FSError d_stat(FSAClientHandle c, const char *path, FSAStat *out) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.getInfoByQuery.path, path);
    s.request.getInfoByQuery.type = FSA_QUERY_INFO_STAT;
    const FSError r = ask(c, FSA_COMMAND_GET_INFO_BY_QUERY, s);
    if (r == FS_ERROR_OK) memcpy((void *)out, (const void *)&s.response.getInfoByQuery.stat, sizeof *out);
    return r;
}

// the same file both ways: if what comes back differs, IOS and these requests don't agree on this console, and the system's functions it is
void probe(FSAClientHandle c) {
    route = SYSTEM;
    alignas(64) static u8 a[256], b[256];
    FSAFileHandle real = (FSAFileHandle)-1, mine = (FSAFileHandle)-1;
    if (__real_FSAOpenFileEx(c, PROBE_FILE, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &real) != FS_ERROR_OK) {
        why = "the check file wouldn't open";
        return;
    }
    if (d_open(c, PROBE_FILE, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &mine) != FS_ERROR_OK) {
        why = "open failed";
    } else {
        FSAStat st_real, st_mine;
        memset(a, 0, sizeof a);
        memset(b, 1, sizeof b);
        const FSError ra = __real_FSAReadFile(c, a, 1, sizeof a, real, FSA_READ_FLAG_NONE), rb = d_read(c, b, 1, sizeof b, mine, FSA_READ_FLAG_NONE);
        if (ra <= 0 || ra != rb) why = "read counts differ";
        else if (memcmp(a, b, (size_t)ra)) why = "read data differs";
        else if (__real_FSAGetStatFile(c, real, &st_real) != FS_ERROR_OK || d_statfile(c, mine, &st_mine) != FS_ERROR_OK || st_real.size != st_mine.size) why = "stat differs";
        else if (d_seek(c, mine, 64) != FS_ERROR_OK || d_read(c, b, 1, 64, mine, FSA_READ_FLAG_NONE) != 64 || memcmp(a + 64, b, 64)) why = "seek differs";
        else {
            route = DIRECT;
            why = nullptr;
        }
        d_close(c, mine);
    }
    __real_FSACloseFile(c, real);
}

inline bool direct(FSAClientHandle c) {
    if (route == UNKNOWN) probe(c);  // the first file call of the run, on the main thread, before there are others
    return route == DIRECT;
}
}  // namespace

extern "C" {
FSError __wrap_FSAOpenFileEx(FSAClientHandle c, const char *path, const char *mode, FSMode create, FSOpenFileFlags flags, uint32_t prealloc, FSAFileHandle *out) {
    return direct(c) ? d_open(c, path, mode, create, flags, prealloc, out) : __real_FSAOpenFileEx(c, path, mode, create, flags, prealloc, out);
}
FSError __wrap_FSACloseFile(FSAClientHandle c, FSAFileHandle file) { return direct(c) ? d_close(c, file) : __real_FSACloseFile(c, file); }
FSError __wrap_FSAReadFile(FSAClientHandle c, void *to, uint32_t size, uint32_t count, FSAFileHandle file, FSAReadFlag flags) {
    return direct(c) ? d_read(c, to, size, count, file, flags) : __real_FSAReadFile(c, to, size, count, file, flags);
}
FSError __wrap_FSAWriteFile(FSAClientHandle c, const void *from, uint32_t size, uint32_t count, FSAFileHandle file, FSAWriteFlag flags) {
    return direct(c) ? d_write(c, from, size, count, file, flags) : __real_FSAWriteFile(c, from, size, count, file, flags);
}
FSError __wrap_FSASetPosFile(FSAClientHandle c, FSAFileHandle file, FSAFilePosition pos) { return direct(c) ? d_seek(c, file, pos) : __real_FSASetPosFile(c, file, pos); }
FSError __wrap_FSAGetStatFile(FSAClientHandle c, FSAFileHandle file, FSAStat *out) { return direct(c) ? d_statfile(c, file, out) : __real_FSAGetStatFile(c, file, out); }
FSError __wrap_FSAGetStat(FSAClientHandle c, const char *path, FSAStat *out) { return direct(c) ? d_stat(c, path, out) : __real_FSAGetStat(c, path, out); }
FSError __wrap_FSAFlushFile(FSAClientHandle c, FSAFileHandle file) {
    if (!direct(c)) return __real_FSAFlushFile(c, file);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.flushFile.handle = file;
    return ask(c, FSA_COMMAND_FLUSH_FILE, s);
}
FSError __wrap_FSATruncateFile(FSAClientHandle c, FSAFileHandle file) {
    if (!direct(c)) return __real_FSATruncateFile(c, file);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.truncateFile.handle = file;
    return ask(c, FSA_COMMAND_TRUNCATE_FILE, s);
}
FSError __wrap_FSARemove(FSAClientHandle c, const char *path) {
    if (!direct(c)) return __real_FSARemove(c, path);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.remove.path, path);
    return ask(c, FSA_COMMAND_REMOVE, s);
}
FSError __wrap_FSAMakeDir(FSAClientHandle c, const char *path, FSMode mode) {
    if (!direct(c)) return __real_FSAMakeDir(c, path, mode);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.makeDir.path, path);
    s.request.makeDir.permission = mode;
    return ask(c, FSA_COMMAND_MAKE_DIR, s);
}
FSError __wrap_FSARename(FSAClientHandle c, const char *from, const char *to) {
    if (!direct(c)) return __real_FSARename(c, from, to);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.rename.oldPath, from);
    put_path(s.request.rename.newPath, to);
    return ask(c, FSA_COMMAND_RENAME, s);
}
FSError __wrap_FSAOpenDir(FSAClientHandle c, const char *path, FSADirectoryHandle *out) {
    if (!direct(c)) return __real_FSAOpenDir(c, path, out);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.openDir.path, path);
    s.response.openDir.handle = (FSADirectoryHandle)-1;
    const FSError r = ask(c, FSA_COMMAND_OPEN_DIR, s);
    *out = r == FS_ERROR_OK ? s.response.openDir.handle : (FSADirectoryHandle)-1;
    return r;
}
FSError __wrap_FSAReadDir(FSAClientHandle c, FSADirectoryHandle dir, FSADirectoryEntry *out) {
    if (!direct(c)) return __real_FSAReadDir(c, dir, out);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.readDir.handle = dir;
    const FSError r = ask(c, FSA_COMMAND_READ_DIR, s);
    if (r == FS_ERROR_OK) memcpy((void *)out, (const void *)&s.response.readDir.entry, sizeof *out);
    return r;
}
FSError __wrap_FSARewindDir(FSAClientHandle c, FSADirectoryHandle dir) {
    if (!direct(c)) return __real_FSARewindDir(c, dir);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.rewindDir.handle = dir;
    return ask(c, FSA_COMMAND_REWIND_DIR, s);
}
FSError __wrap_FSACloseDir(FSAClientHandle c, FSADirectoryHandle dir) {
    if (!direct(c)) return __real_FSACloseDir(c, dir);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.closeDir.handle = dir;
    return ask(c, FSA_COMMAND_CLOSE_DIR, s);
}
}

namespace wiiu {
// What saves rely on: a file made, written, closed and removed this way has to look right through the system's functions, before the game writes anything of the player's.
// It isn't flushed, so it never reaches the storage for good.
void fsa_direct_check_writes() {
    static const FSAClientHandle c = FSAAddClient(nullptr);
    if (c < 0 || !direct(c)) return;
    const char *const name = "/vol/save/common/fsa_check.bin";
    alignas(64) static u8 out[256], back[256];
    for (u32 i = 0; i < sizeof out; i++) out[i] = (u8)(i * 7 + 1);
    FSAFileHandle f = (FSAFileHandle)-1;
    FSAStat st;
    const char *wrong = nullptr;
    if (d_open(c, name, "w", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &f) != FS_ERROR_OK) wrong = "making a file failed";
    else {
        if (d_write(c, out, 1, sizeof out, f, FSA_WRITE_FLAG_NONE) != (FSError)sizeof out) wrong = "writing failed";
        d_close(c, f);
    }
    if (!wrong) {
        memset(back, 0, sizeof back);
        if (__real_FSAOpenFileEx(c, name, "r", (FSMode)0x660, FS_OPEN_FLAG_NONE, 0, &f) != FS_ERROR_OK) wrong = "what was written isn't there";
        else {
            if (__real_FSAReadFile(c, back, 1, sizeof back, f, FSA_READ_FLAG_NONE) != (FSError)sizeof back || memcmp(out, back, sizeof out)) wrong = "what was written reads differently";
            __real_FSACloseFile(c, f);
        }
    }
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.remove.path, name);
    const FSError removed = ask(c, FSA_COMMAND_REMOVE, s);
    if (!wrong && (removed != FS_ERROR_OK || __real_FSAGetStat(c, name, &st) == FS_ERROR_OK)) wrong = "removing failed";
    if (wrong) {
        __real_FSARemove(c, name);
        route = SYSTEM;
        why = wrong;
    }
}

void fsa_direct_report() {
    if (route == DIRECT) LOG("[fs] every file call goes straight to the file system, past Aroma's hook\n");
    else LOG("[fs] file calls go through the system's functions: %s\n", why);
}

// What SAVEFlushQuota asks for in the end, without the FS layer's hook. false: it wasn't done this way, so do it that way.
bool fsa_direct_flush_quota(const char *path) {
    if (route != DIRECT) return false;
    static FSAClientHandle client = FSAAddClient(nullptr);
    if (client < 0) return false;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    put_path(s.request.flushQuota.path, path);
    return ask(client, FSA_COMMAND_FLUSH_QUOTA, s) == FS_ERROR_OK;
}
}  // namespace wiiu
