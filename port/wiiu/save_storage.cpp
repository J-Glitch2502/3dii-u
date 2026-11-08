// Saves live in the console's own save storage for the title, which the system keeps wherever the game is installed. The SD card plays no part.
// The game's saves go in the player's storage, /vol/save/<persistent id>, as a Wii U game's do: the system lists that as save data of its own and keeps it when the
// software is deleted. The common storage, /vol/save/common, went with the software (the user, 2026-11-07); it keeps the logs and the test runs.
// nn_save and nn_act come from OSDynLoad, as the sockets do in net_console.cpp: the game's imports stay as they are, and a failure here reaches the PC and the screen instead of hanging the boot.
// What's written there only becomes permanent when it's flushed: after a power-off the system goes back to the last flush.
#include <coreinit/dynload.h>
#include <coreinit/filesystem.h>
#include <coreinit/time.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rt.h"
#include "wiiu.h"

extern std::string g_save_base;

namespace wiiu {
// A save file of the game's, all of it in memory from the first time it's opened: the game's reads and writes never wait for storage. On a console Tomodachi's
// 1.9 MB writes took 4 s each, twice per save, then 4 s more to flush, at 3 to 8 fps (2026-11-06 log). The card thread writes changed files out (save_flush).
struct SaveFile {
    std::string path;
    std::vector<u8> data;
    u32 users = 0;  // file sessions that have it open
    bool dirty = false;
};
}  // namespace wiiu

namespace {
constexpr u8 COMMON = 0xFF;  // as an account number, this means /vol/save/common
FSStatus (*flush_quota)(FSClient *, FSCmdBlock *, u8, FSErrorFlag);
alignas(64) FSClient client;
alignas(64) FSCmdBlock block;
volatile bool dirty;
volatile OSTime first_change, last_change;
u8 account_slot = COMMON;
std::string account_dir;

std::mutex cache_m;
std::map<std::string, std::shared_ptr<wiiu::SaveFile>> cache;
u64 cached_bytes;
constexpr u64 CACHE_FILE_MAX = 16 << 20, CACHE_MAX = 48 << 20;  // bigger files go to storage as the game writes them

// Until it's flushed the journal holds what was written, and refuses more once it's full. wut says so with -1, or with a short count if it got partway.
long write_retrying(int fd, const void *data, size_t len, u64 at) {
    const long n = pwrite(fd, data, len, (off_t)at);
    if (n == (long)len) return n;
    LOG("[save] wrote %ld of %x bytes at %llx: error %d, flushing\n", n, (u32)len, (unsigned long long)at, errno);
    wiiu::save_flush(true);
    const long done = std::max(n, 0L), more = pwrite(fd, (const u8 *)data + done, len - done, (off_t)(at + done));
    if (more < 0) LOG("[save] still failing: error %d\n", errno);
    return more < 0 ? n : done + more;
}

// A file only gets longer by being written: the console refuses to seek past the end of one, and pwrite and ftruncate both seek first
void grow(int fd, u64 size) {
    struct stat st;
    if (fstat(fd, &st) || (u64)st.st_size >= size) return;
    std::vector<u8> zeros((size_t)std::min<u64>(size - st.st_size, 0x10000));
    for (u64 at = st.st_size; at < size;) {
        const long n = write_retrying(fd, zeros.data(), (size_t)std::min<u64>(zeros.size(), size - at), at);
        if (n <= 0) return;
        at += n;
    }
}

// One file out to storage, whole, from a 64-byte aligned copy: wut then hands the file system 256 KB at a time straight from it, at aligned places in the file.
// From a guest buffer it first wrote up to 63 bytes through a bounce buffer, and every piece after landed off alignment. The caller holds the file lock.
bool write_out(const std::shared_ptr<wiiu::SaveFile> &f) {
    u8 *copy;
    u32 n;
    {
        std::lock_guard<std::mutex> lk(cache_m);
        if (!f->dirty) return true;
        n = (u32)f->data.size();
        copy = (u8 *)memalign(64, std::max(n, 64u));
        if (!copy) return false;
        memcpy(copy, f->data.data(), n);
        f->dirty = false;
    }
    const OSTime t = OSGetSystemTime();
    const int fd = open(f->path.c_str(), O_RDWR | O_CREAT, 0644);
    bool ok = fd >= 0;
    if (ok) {
        if (n) ok = wiiu::save_write(fd, copy, n, 0) == (long)n;
        struct stat st;
        if (ok && !fstat(fd, &st) && (u64)st.st_size > n) wiiu::save_resize(fd, n);
        close(fd);
    }
    free(copy);
    LOG("[save] wrote %s, %u bytes, %s, %u ms\n", f->path.c_str(), n, ok ? "done" : "FAILED", (u32)OSTicksToMilliseconds(OSGetSystemTime() - t));
    if (!ok) {
        std::lock_guard<std::mutex> lk(cache_m);
        f->dirty = true;
    }
    return ok;
}

// Everything the game changed, one file at a time, so its other file calls (the game data's reads) wait for one file at most
bool write_out_all() {
    for (;;) {
        wiiu::fs_begin(0xC008);
        std::shared_ptr<wiiu::SaveFile> f;
        {
            std::lock_guard<std::mutex> lk(cache_m);
            for (auto &e : cache)
                if (e.second->dirty) {
                    f = e.second;
                    break;
                }
        }
        const bool ok = !f || write_out(f);
        wiiu::fs_end();
        if (!f) return true;
        if (!ok) return false;
    }
}

bool under(const std::string &path, const std::string &prefix) { return !path.compare(0, prefix.size(), prefix); }
}  // namespace

namespace wiiu {
void save_mount() {
    OSDynLoad_Module m;
    const int loaded = OSDynLoad_Acquire("nn_save.rpl", &m);
    if (loaded != OS_DYNLOAD_OK) {
        note("save storage: no nn_save, %x", loaded);
        return;
    }
    int (*init)() = nullptr, (*init_dir)(u8) = nullptr, (*init_common)() = nullptr;
    OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "SAVEInit", (void **)&init);
    OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "SAVEInitSaveDir", (void **)&init_dir);
    OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "SAVEInitCommonSaveDir", (void **)&init_common);
    OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "SAVEFlushQuota", (void **)&flush_quota);
    if (!init || !init_dir) {
        note("save storage: nn_save lacks what it needs");
        return;
    }
    FSInit();
    const int added = FSAddClient(&client, FS_ERROR_FLAG_ALL);
    FSInitCmdBlock(&block);
    if (added != FS_STATUS_OK) flush_quota = nullptr;
    const int inited = init();
    int folder = init_dir(COMMON);
    if (folder && init_common) folder = init_common();
    // the player the game was started for: their slot and the persistent id their storage is named by
    OSDynLoad_Module act;
    int (*act_init)() = nullptr;
    u8 (*get_slot)() = nullptr;
    u32 (*get_id)() = nullptr;
    if (OSDynLoad_Acquire("nn_act.rpl", &act) == OS_DYNLOAD_OK) {
        OSDynLoad_FindExport(act, OS_DYNLOAD_EXPORT_FUNC, "Initialize__Q2_2nn3actFv", (void **)&act_init);
        OSDynLoad_FindExport(act, OS_DYNLOAD_EXPORT_FUNC, "GetSlotNo__Q2_2nn3actFv", (void **)&get_slot);
        OSDynLoad_FindExport(act, OS_DYNLOAD_EXPORT_FUNC, "GetPersistentId__Q2_2nn3actFv", (void **)&get_id);
    }
    int player = -1;
    if (act_init && get_slot && get_id) {
        act_init();
        const u8 slot = get_slot();
        const u32 id = get_id();
        player = slot >= 1 && slot <= 12 && id ? init_dir(slot) : -2;
        if (!player) {
            char dir[32];
            snprintf(dir, sizeof dir, "/vol/save/%08x", id);
            account_slot = slot;
            account_dir = dir;
        }
    }
    note("save storage: client %d, init %d, folder %d, player %d %s, flush %s", added, inited, folder, player, account_dir.empty() ? "(saves in common)" : account_dir.c_str(),
         flush_quota ? "yes" : "no");
    fsa_direct_check_writes();
}

const std::string &save_account_dir() { return account_dir; }

void save_dirty() {
    const OSTime now = OSGetSystemTime();
    if (!dirty) first_change = now;
    last_change = now;
    dirty = true;
}

// something the game saved that isn't flushed yet: quitting waits for it
bool save_pending() {
    if (dirty) return true;
    std::lock_guard<std::mutex> lk(cache_m);
    for (auto &e : cache)
        if (e.second->dirty) return true;
    return false;
}

// Once a burst of writes has settled, or every few seconds if one doesn't. On the card thread, so that a slow flush only makes the game's next file operation wait.
// The game's storage, and the common one too when asked for at once: the logs, test runs and markers are written there just before.
void save_flush(bool now) {
    if (!flush_quota) return;
    const OSTime t = OSGetSystemTime();
    if (!now && (!dirty || (OSTicksToMilliseconds(t - last_change) < 1000 && OSTicksToSeconds(t - first_change) < 5))) return;
    dirty = false;
    const bool written = write_out_all();
    const bool mine = !account_dir.empty() && under(g_save_base, account_dir);
    fs_begin(0xC007);
    int r = 0;
    if (!account_dir.empty() && (mine || now)) r = fsa_direct_flush_quota(account_dir.c_str()) ? 0 : flush_quota(&client, &block, account_slot, FS_ERROR_FLAG_ALL);
    if (!mine || now) {
        const int c = fsa_direct_flush_quota("/vol/save/common") ? 0 : flush_quota(&client, &block, COMMON, FS_ERROR_FLAG_ALL);
        if (!r) r = c;
    }
    fs_end();
    LOG("[save] flushed: %d, %u ms\n", r, (u32)OSTicksToMilliseconds(OSGetSystemTime() - t));
    if (r < 0 || !written) save_dirty();
}

long save_write(int fd, const void *data, u32 len, u64 at) {
    if (!len) return 0;
    if (lseek(fd, (off_t)at, SEEK_SET) < 0) grow(fd, at);
    return write_retrying(fd, data, len, at);
}

// The seek comes first because wut's ftruncate moves the file's position without telling its own count of it, and the next write at the old position would land here
void save_resize(int fd, u64 size) {
    if (lseek(fd, (off_t)size, SEEK_SET) >= 0 && ftruncate(fd, (off_t)size)) LOG("[save] resizing to %llx: error %d\n", (unsigned long long)size, errno);
    grow(fd, size);  // whichever of those turned it down, or a truncate that doesn't lengthen a file
}

// A folder and all in it, each file read and written whole from an aligned buffer (write_out says why): the C library's copy goes a little at a time. false: something
// didn't copy.
bool save_copy_tree(const std::string &from, const std::string &to) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec) return false;
    for (const auto &e : std::filesystem::directory_iterator(from, ec)) {
        const std::string name = e.path().filename().string(), src = from + "/" + name, dst = to + "/" + name;
        if (e.is_directory(ec)) {
            if (!save_copy_tree(src, dst)) return false;
            continue;
        }
        const int in = open(src.c_str(), O_RDONLY);
        if (in < 0) return false;
        struct stat st;
        bool ok = !fstat(in, &st);
        const u32 n = ok ? (u32)st.st_size : 0;
        u8 *buf = ok ? (u8 *)memalign(64, std::max(n, 64u)) : nullptr;
        ok = buf && (!n || pread(in, buf, n, 0) == (long)n);
        close(in);
        const int out = ok ? open(dst.c_str(), O_RDWR | O_CREAT, 0644) : -1;
        ok = out >= 0 && (!n || save_write(out, buf, n, 0) == (long)n);
        if (out >= 0) close(out);
        free(buf);
        if (!ok) return false;
    }
    return !ec;
}

// A game's save file, from memory when it's been opened before: null for one too big to hold, which then goes to storage as before
std::shared_ptr<SaveFile> save_file_open(const std::string &path, int fd) {
    {
        std::lock_guard<std::mutex> lk(cache_m);
        auto it = cache.find(path);
        if (it != cache.end()) {
            it->second->users++;
            return it->second;
        }
    }
    struct stat st;
    if (fstat(fd, &st) || (u64)st.st_size > CACHE_FILE_MAX) return nullptr;
    auto f = std::make_shared<SaveFile>();
    f->path = path;
    f->data.resize((size_t)st.st_size);
    if (st.st_size) {
        u8 *buf = (u8 *)memalign(64, (size_t)st.st_size);
        const bool ok = buf && pread(fd, buf, (size_t)st.st_size, 0) == (long)st.st_size;
        if (ok) memcpy(f->data.data(), buf, (size_t)st.st_size);
        free(buf);
        if (!ok) return nullptr;
    }
    std::lock_guard<std::mutex> lk(cache_m);
    auto it = cache.find(path);  // another thread's open may have got there first
    if (it != cache.end()) {
        it->second->users++;
        return it->second;
    }
    for (auto e = cache.begin(); e != cache.end() && cached_bytes + f->data.size() > CACHE_MAX;)
        if (!e->second->users && !e->second->dirty) {
            cached_bytes -= e->second->data.size();
            e = cache.erase(e);
        } else {
            ++e;
        }
    f->users = 1;
    cached_bytes += f->data.size();
    cache[path] = f;
    return f;
}

void save_file_close(SaveFile &f) {
    std::lock_guard<std::mutex> lk(cache_m);
    if (f.users) f.users--;
}

long save_file_read(SaveFile &f, void *to, u32 len, u64 at) {
    std::lock_guard<std::mutex> lk(cache_m);
    if (at >= f.data.size()) return 0;
    const u32 n = (u32)std::min<u64>(len, f.data.size() - at);
    memcpy(to, f.data.data() + at, n);
    return n;
}

// past the end is zeros, as the console's own files grow (grow, above)
long save_file_write(SaveFile &f, const void *from, u32 len, u64 at) {
    if (!len) return 0;
    std::lock_guard<std::mutex> lk(cache_m);
    if (at + len > f.data.size()) {
        cached_bytes += at + len - f.data.size();
        f.data.resize((size_t)(at + len));
    }
    memcpy(f.data.data() + at, from, len);
    f.dirty = true;
    return len;
}

u64 save_file_size(SaveFile &f) {
    std::lock_guard<std::mutex> lk(cache_m);
    return f.data.size();
}

void save_file_resize(SaveFile &f, u64 size) {
    std::lock_guard<std::mutex> lk(cache_m);
    cached_bytes = cached_bytes - f.data.size() + size;
    f.data.resize((size_t)size);
    f.dirty = true;
}

// Before the game deletes, renames or lists anything under prefix, storage has to hold what it wrote there. forget: it's going away or moving, so memory lets go too.
void save_settle(const std::string &prefix, bool forget) {
    fs_begin(0xC009);
    std::vector<std::shared_ptr<SaveFile>> hit;
    {
        std::lock_guard<std::mutex> lk(cache_m);
        for (auto &e : cache)
            if (under(e.first, prefix) && e.second->dirty) hit.push_back(e.second);
    }
    for (auto &f : hit) write_out(f);
    if (forget) {
        std::lock_guard<std::mutex> lk(cache_m);
        for (auto e = cache.begin(); e != cache.end();)
            if (under(e->first, prefix)) {
                cached_bytes -= e->second->data.size();
                e = cache.erase(e);
            } else {
                ++e;
            }
    }
    fs_end();
}
}  // namespace wiiu
