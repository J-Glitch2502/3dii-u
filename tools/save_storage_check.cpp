// Self-check of save_storage.cpp's file handling, against a console's files as wut's devoptab meets them. A seek past a file's end may be refused, ftruncate moves the system's position without
// telling wut's own count of it, a seek to wut's count does nothing, and a full journal cuts a write short until a flush. Cemu shows none of this.
// Runs inside the contained environment, from /root:
//   mkdir -p fake/coreinit; touch fake/coreinit/dynload.h fake/coreinit/filesystem.h fake/coreinit/time.h
//   RT=/project/third_party/3dsrecomp/runtime; clang++-18 -std=c++20 -O1 -g -w -Ifake -I/project/port/wiiu -I/project/port/pc -I$RT -include /project/port/pc/shim.h /project/tools/save_storage_check.cpp -o save_storage_check && ./save_storage_check
#undef NDEBUG
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rt.h"
#include "wiiu.h"

// what the wut headers would give (the ones the command above makes are empty)
struct FSClient {};
struct FSCmdBlock {};
typedef int FSStatus;
typedef int FSErrorFlag;
typedef long long OSTime;
typedef void *OSDynLoad_Module;
enum { OS_DYNLOAD_OK = 0, OS_DYNLOAD_EXPORT_FUNC = 0, FS_ERROR_FLAG_ALL = -1, FS_STATUS_OK = 0 };
static void FSInit() {}
static int FSAddClient(FSClient *, int) { return 0; }
static void FSInitCmdBlock(FSCmdBlock *) {}
static OSTime OSGetSystemTime() { return 0; }
static long long OSTicksToMilliseconds(OSTime t) { return t; }
static long long OSTicksToSeconds(OSTime t) { return t; }

// one file on the console
static std::vector<u8> disk;
static u32 sys_pos, wut_count, journal, journal_after_flush;
static bool seek_past_end_ok, truncate_lengthens;
static int flushes;

static bool sys_set_pos(u32 pos) {
    if (pos > disk.size() && !seek_past_end_ok) return false;
    sys_pos = pos;
    return true;
}

// wut's devoptab_fsa_seek.cpp
static off_t fake_lseek(int, off_t pos, int) {
    if ((u32)pos == wut_count) return pos;
    if (!sys_set_pos((u32)pos)) return -1;
    wut_count = (u32)pos;
    return pos;
}

// pwrite in port/wiiu/stubs.cpp, then wut's devoptab_fsa_write.cpp
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (fake_lseek(fd, off, SEEK_SET) < 0) return -1;
    size_t done = 0;
    while (done < n) {
        const size_t chunk = std::min<size_t>(n - done, 0x40000);
        if (chunk > journal) return done ? (ssize_t)done : -1;
        journal -= (u32)chunk;
        if (sys_pos + chunk > disk.size()) disk.resize(sys_pos + chunk);
        memcpy(&disk[sys_pos], (const u8 *)buf + done, chunk);
        sys_pos += (u32)chunk;
        wut_count += (u32)chunk;
        done += chunk;
    }
    return (ssize_t)done;
}

// wut's devoptab_fsa_truncate.cpp
static int fake_ftruncate(int, off_t len) {
    if (!sys_set_pos((u32)len)) return -1;
    if ((size_t)len < disk.size() || truncate_lengthens) disk.resize((size_t)len);
    return 0;
}

static int fake_fstat(int, struct stat *st) {
    st->st_size = (off_t)disk.size();
    return 0;
}

#define lseek fake_lseek
#define pwrite fake_pwrite
#define ftruncate fake_ftruncate
#define fstat fake_fstat

static int OSDynLoad_Acquire(const char *, OSDynLoad_Module *) { return OS_DYNLOAD_OK; }
static int return_0() { return 0; }
static int return_0_slot(u8) { return 0; }
static FSStatus fake_flush(FSClient *, FSCmdBlock *, u8, FSErrorFlag) {
    flushes++;
    journal = journal_after_flush;
    return 0;
}
static int OSDynLoad_FindExport(OSDynLoad_Module, int, const char *name, void **out) {
    if (!strcmp(name, "SAVEInit")) *out = (void *)return_0;
    if (!strcmp(name, "SAVEInitSaveDir")) *out = (void *)return_0_slot;
    if (!strcmp(name, "SAVEFlushQuota")) *out = (void *)fake_flush;
    return OS_DYNLOAD_OK;
}

#include "../port/wiiu/save_storage.cpp"

void rt_log(const char *, ...) {}
void wiiu::note(const char *, ...) {}
void wiiu::fs_begin(u32) {}
void wiiu::fs_end() {}

static std::vector<u8> want;

static void write_at(u64 off, u32 len, u8 value) {
    const std::vector<u8> data(len, value);
    assert(wiiu::save_write(1, data.data(), len, off) == (long)len);
    if (off + len > want.size()) want.resize(off + len);
    std::fill_n(want.begin() + (long)off, len, value);
    assert(disk == want);
}

static void resize_to(u64 size) {
    wiiu::save_resize(1, size);
    want.resize(size);
    assert(disk == want);
}

static void fresh() {
    disk.clear();
    want.clear();
    sys_pos = wut_count = 0;
    journal = journal_after_flush = 1u << 30;
    flushes = 0;
}

int main() {
    wiiu::save_mount();
    const bool ways[3][2] = {{false, false}, {true, false}, {true, true}};  // what a console does with these isn't known for sure, and every way has to come out the same
    for (const auto &way : ways) {
        seek_past_end_ok = way[0];
        truncate_lengthens = way[1];

        // Superstar Saga's: a file made at its full size, then written out of order
        fresh();
        resize_to(75024);
        write_at(5000, 100, 1);
        write_at(0, 4, 2);
        write_at(75020, 4, 3);
        // resizing moves the position: the next write goes where it's aimed all the same
        write_at(100, 4, 4);
        resize_to(2000);
        write_at(104, 4, 5);
        resize_to(90000);
        write_at(108, 4, 6);
        write_at(90000, 4, 7);  // right at the end
        resize_to(0);
        write_at(4000, 16, 8);  // past the end: the gap reads as zeros
        assert(disk.size() == 4016);
        assert(!flushes);

        // a journal that fills up partway through a write, at its start, and while a file grows
        fresh();
        journal = 0x50000;
        write_at(0, 0x90000, 9);
        assert(flushes == 1);
        journal = 0;
        write_at(16, 16, 10);
        assert(flushes == 2);
        journal = 0x18000;
        resize_to(0x100000);
        assert(flushes == (truncate_lengthens ? 2 : 3));

        // really out of room: the game is told what happened
        fresh();
        journal = 0x50000;
        journal_after_flush = 0;
        const std::vector<u8> data(0x90000, 11);
        assert(wiiu::save_write(1, data.data(), (u32)data.size(), 0) == 0x40000);
        assert(wiiu::save_write(1, data.data(), (u32)data.size(), 0x40000) == -1);
        assert(disk.size() == 0x40000);
    }
    puts("save_storage_check: all good");
    return 0;
}
