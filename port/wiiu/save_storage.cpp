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
#include <cstring>
#include <filesystem>
#include <limits>
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
bool save_client_ready;
volatile bool dirty;
volatile OSTime first_change, last_change;
u8 account_slot = COMMON;
std::string account_dir;

std::mutex cache_m;
std::map<std::string, std::shared_ptr<wiiu::SaveFile>> cache;
struct DirectUsers { u32 users = 0; bool shared = false; };
std::map<std::string, DirectUsers> direct_users;  // keep a path direct until every uncached file session closes
u64 cached_bytes;
constexpr u64 CACHE_FILE_MAX = 16 << 20, CACHE_MAX = 48 << 20;  // bigger files go to storage as the game writes them

bool under(const std::string &path, const std::string &prefix) { return !path.compare(0, prefix.size(), prefix); }

// Unknown storage keeps strict quota commits; only a verified ordinary directory
// (Aroma's SD-backed homebrew save mapping) can omit them.
bool checkpoint_quota_required = true;
std::mutex quota_mutex;
int flush_save_quota(const char *path, u8 slot) {
    int result;
    if (wiiu::fsa_direct_flush_quota(path, result)) return result;
    return flush_quota ? flush_quota(&client, &block, slot, FS_ERROR_FLAG_ALL) : -1;
}
int flush_save_quotas(bool now) {
    std::lock_guard<std::mutex> lock(quota_mutex);
    if (!flush_quota) return 0;
    const bool mine = !account_dir.empty() && !g_save_base.compare(0, account_dir.size(), account_dir);
    wiiu::fs_begin(0xC007);
    int result = 0;
    if (!account_dir.empty() && (mine || now) && !wiiu::fsa_direct_flush_quota(account_dir.c_str(), result))
        result = flush_quota(&client, &block, account_slot, FS_ERROR_FLAG_ALL);
    if (!mine || now) {
        int common_result;
        if (!wiiu::fsa_direct_flush_quota("/vol/save/common", common_result))
            common_result = flush_quota(&client, &block, COMMON, FS_ERROR_FLAG_ALL);
        if (!result) result = common_result;
    }
    wiiu::fs_end();
    return result;
}

// Until it's flushed the journal holds what was written, and refuses more once it's full. wut says so with -1, or with a short count if it got partway.
long write_retrying(int fd, const void *data, size_t len, u64 at) {
    const long n = pwrite(fd, data, len, (off_t)at);
    if (n == (long)len) return n;
    LOG("[save] wrote %ld of %x bytes at %llx: error %d, flushing\n", n, (u32)len, (unsigned long long)at, errno);
    // A retry can run inside write_out_all. Flush the journal without
    // reentering the cache writeback loop while this file is being written.
    if (flush_save_quotas(true) < 0) wiiu::save_dirty();
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
    if (!wiiu::ensure_save_capacity(f->path,n)) {
        free(copy);
        std::lock_guard<std::mutex> lk(cache_m);f->dirty=true;return false;
    }
    const int fd = open(f->path.c_str(), O_RDWR | O_CREAT, 0644);
    bool ok = fd >= 0;
    if (ok) {
        if (n) ok = wiiu::save_write(fd, copy, n, 0) == (long)n;
        struct stat st;
        if (ok) {
            ok = !fstat(fd, &st);
            if (ok && (u64)st.st_size > n) wiiu::save_resize(fd, n);
            if (ok) ok = !fstat(fd, &st) && (u64)st.st_size == n;
        }
        if (close(fd)) ok = false;
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
    save_client_ready = added == FS_STATUS_OK;
    if (!save_client_ready) flush_quota = nullptr;
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
    if (!account_dir.empty()) fsa_report_save_quota(account_dir.c_str());
    checkpoint_quota_required = fsa_report_save_quota("/vol/save/common") != 0;
}

const std::string &save_account_dir() { return account_dir; }

bool save_dir_lacks(const std::string &path, const char *first, const char *second) {
    if (!save_client_ready) return false;
    const char *native = path.c_str() + (path.starts_with("fs:") ? 3 : 0);
    FSDirectoryHandle handle;
    if (FSOpenDir(&client, &block, native, &handle, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) return false;
    FSDirectoryEntry entry;
    FSStatus status;
    while ((status = FSReadDir(&client, &block, handle, &entry, FS_ERROR_FLAG_ALL)) == FS_STATUS_OK) {
        if (!std::strncmp(entry.name, first, sizeof entry.name) || !std::strncmp(entry.name, second, sizeof entry.name)) break;
    }
    const FSStatus closed = FSCloseDir(&client, &block, handle, FS_ERROR_FLAG_ALL);
    return status == FS_STATUS_END && closed == FS_STATUS_OK;
}

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
int flush_checkpoint_storage() {
    // Checkpoints only write common storage. Do not commit the unrelated account
    // directory or ask nn_save to flush an SD directory as a title quota.
    // fflush/fclose still check the actual file writes on every backend.
    if (!checkpoint_quota_required) return 0;
    std::lock_guard<std::mutex> lock(quota_mutex);
    if (!flush_quota) return -1;
    fs_begin(0xC007);
    int result;
    if (!fsa_direct_flush_quota("/vol/save/common", result))
        result = flush_quota(&client, &block, COMMON, FS_ERROR_FLAG_ALL);
    fs_end();
    return result;
}

void save_flush(bool now) {
    if (!flush_quota) return;
    const OSTime t = OSGetSystemTime();
    if (!now && (!dirty || (OSTicksToMilliseconds(t - last_change) < 1000 && OSTicksToSeconds(t - first_change) < 5))) return;
    dirty = false;
    const bool written = write_out_all();
    const int r = flush_save_quotas(now);
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

volatile u32 files_copied;

// A folder and all in it, each file read and written whole from an aligned buffer (write_out says why): the C library's copy goes a little at a time. false: something
// didn't copy.
bool save_copy_tree(const std::string &from, const std::string &to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return false;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string(), src = from + "/" + name, dst = to + "/" + name;
        const bool directory = it->is_directory(ec);
        if (ec) return false;
        if (directory) {
            if (!save_copy_tree(src, dst)) return false;
            continue;
        }
        const int in = open(src.c_str(), O_RDONLY);
        if (in < 0) return false;
        struct stat st;
        bool ok = !fstat(in, &st) && st.st_size >= 0 && (u64)st.st_size <= std::numeric_limits<u32>::max();
        const u32 n = ok ? (u32)st.st_size : 0;
        u8 *buf = ok ? (u8 *)memalign(64, std::max(n, 64u)) : nullptr;
        ok = buf && (!n || pread(in, buf, n, 0) == (long)n);
        if (close(in)) ok = false;
        const int out = ok ? open(dst.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644) : -1;
        ok = out >= 0 && (!n || save_write(out, buf, n, 0) == (long)n);
        if (out >= 0 && close(out)) ok = false;
        free(buf);
        if (!ok) return false;
        files_copied = files_copied + 1;
    }
    return !ec;
}

static bool flush_migrated_account() {
    if (account_dir.empty()) return false;
    fs_begin(0xC007);
    const bool flushed = flush_save_quota(account_dir.c_str(), account_slot) >= 0;
    fs_end();
    return flushed;
}

void save_finish_migration(const std::string &to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string old = to + "_old";
    const bool pending = fs::exists(old, ec);
    if (ec) FATAL("[save] Cannot read migration state at %s.\n", old.c_str());
    if (!pending) return;
    if (!flush_migrated_account())
        FATAL("[save] Cannot confirm the account save at %s. The previous save is still at %s.\n", to.c_str(), old.c_str());
    fs::remove_all(old, ec);
}

// A copy made whole beside where it goes, then put in its place, what was there set aside until it is: a copy cut short, or a flush
// partway through one (a full journal, write_retrying), never stands for a save
static bool copy_into_place(const std::string &from, const std::string &to, bool preserve_existing = false) {
    namespace fs = std::filesystem;
    std::error_code ec, aside;
    const std::string fresh = to + "_new", old = to + "_old";
    const bool had_old = fs::exists(old, ec);
    if (ec) return false;
    bool had_to = fs::exists(to, ec);
    if (ec) return false;
    if (had_old && !had_to) {
        fs::rename(old, to, ec);  // a previous swap stopped before its rollback
        if (ec) return false;
        had_to = true;
    }
    fs::remove_all(fresh, ec);
    if (ec) return false;
    if (!save_copy_tree(from, fresh) || (preserve_existing && had_to && !save_copy_tree(to, fresh))) {
        fs::remove_all(fresh, ec);
        return false;
    }
    fs::remove_all(old, ec);
    if (ec) return false;
    if (had_to) fs::rename(to, old, aside);
    else if (preserve_existing) fs::create_directory(old, aside);  // mark a pending commit even for a new account save
    if (aside) {
        fs::remove_all(fresh, ec);
        return false;
    }
    fs::rename(fresh, to, ec);
    if (ec) {
        if (had_to) fs::rename(old, to, aside);
        else if (preserve_existing) fs::remove_all(old, aside);
        if (!aside) fs::remove_all(fresh, ec);  // a failed rollback keeps both recoverable trees
        return false;
    }
    if (preserve_existing && !flush_migrated_account()) {
        fs::rename(to, fresh, ec);
        if (ec) FATAL("[save] Cannot roll back the unconfirmed account save at %s. The previous save is still at %s.\n", to.c_str(), old.c_str());
        if (had_to) fs::rename(old, to, ec);
        else fs::remove_all(old, ec);
        if (!ec) fs::remove_all(fresh, ec);
        return false;
    }
    fs::remove_all(old, ec);
    return true;
}

bool save_migrate_common(const std::string &from, const std::string &to) {
    return copy_into_place(from, to, true);  // the account's newer extdata wins collisions
}

// A 3DS save the builder packed into the game (builder/saves.py, the title's save_import), taken in once, before the game starts: its
// files take the place of the game's save, which is kept as previous beside it first; nothing is replaced if that copy fails. It waits
// for the game's save folder, there once the game has formatted on this console (services/fs.cpp, OpenArchive), whose first format would
// wipe it: so the game starts once, then takes the save at its next start. done marks it taken, beside the save: it stays when the
// software is deleted, as the save does, so a reinstall with the same save packed in leaves the progress made since alone.
void save_from_content(const std::string &packed, const std::string &save, const std::string &done) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(packed, ec) || fs::is_empty(packed, ec) || !fs::is_directory(save, ec) || fs::exists(done, ec)) return;
    if (!copy_into_place(save, save + "_previous")) return;
    const bool in = copy_into_place(packed, save);
    if (in) {
        save_dirty();
        save_flush(true);
    }
    if (FILE *f = fopen(done.c_str(), "wb")) {  // taken, or tried once: a save that can't be copied isn't tried at every start
        fputs(in ? "taken" : "failed", f);
        fclose(f);
    }
    LOG("[save] the 3DS save packed into the game: %s\n", in ? "taken in" : "COULD NOT BE COPIED, the game's own kept");
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
        // Reserve before loading: another open must not cache a path with a live direct session.
        auto &direct = direct_users[path];
        if (++direct.users != 1) { direct.shared = true; return nullptr; }
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
    const auto &direct = direct_users.at(path);
    if (direct.users != 1 || direct.shared) return nullptr;
    for (auto e = cache.begin(); e != cache.end() && cached_bytes + f->data.capacity() > CACHE_MAX;)
        if (!e->second->users && !e->second->dirty) {
            cached_bytes -= e->second->data.capacity();
            e = cache.erase(e);
        } else {
            ++e;
        }
    if (cached_bytes + f->data.capacity() > CACHE_MAX) return nullptr;
    direct_users.erase(path);
    f->users = 1;
    cached_bytes += f->data.capacity();
    cache[path] = f;
    return f;
}

void save_file_close(SaveFile &f) {
    std::lock_guard<std::mutex> lk(cache_m);
    if (f.users) f.users--;
}

void save_file_close_direct(const std::string &path) {
    std::lock_guard<std::mutex> lk(cache_m);
    auto it = direct_users.find(path);
    if (it != direct_users.end() && !--it->second.users) direct_users.erase(it);
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
        const size_t old_capacity = f.data.capacity();
        f.data.resize((size_t)(at + len));
        cached_bytes = cached_bytes - old_capacity + f.data.capacity();
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
    const size_t old_capacity = f.data.capacity();
    f.data.resize((size_t)size);
    cached_bytes = cached_bytes - old_capacity + f.data.capacity();
    f.dirty = true;
}

// Before the game deletes, renames or lists anything under prefix, storage has to hold what it wrote there. forget: it's going away or moving, so memory lets go too.
bool save_settle(const std::string &prefix, bool forget) {
    fs_begin(0xC009);
    std::vector<std::shared_ptr<SaveFile>> hit;
    bool ok = true;
    {
        std::lock_guard<std::mutex> lk(cache_m);
        if (forget)
            for (const auto &e : direct_users)
                if (under(e.first, prefix)) ok = false;
        for (auto &e : cache)
            if (under(e.first, prefix)) {
                // shortcut: live handles prevent moving/deleting; rebind ownership if a title needs live-handle rename.
                if (forget && e.second->users) ok = false;
                if (e.second->dirty) hit.push_back(e.second);
            }
    }
    if (!ok) { fs_end(); return false; }
    for (auto &f : hit) if (!write_out(f)) ok = false;
    if (ok) {
        std::lock_guard<std::mutex> lk(cache_m);
        // A write that arrived during the snapshot must stay cached for the next attempt.
        if (forget)
            for (const auto &e : direct_users)
                if (under(e.first, prefix)) ok = false;
        for (auto &e : cache)
            if (under(e.first, prefix) && (e.second->dirty || (forget && e.second->users))) ok = false;
        if (forget && ok)
            for (auto e = cache.begin(); e != cache.end();)
                if (under(e->first, prefix)) {
                    cached_bytes -= e->second->data.capacity();
                    e = cache.erase(e);
                } else {
                    ++e;
                }
    }
    fs_end();
    return ok;
}
}  // namespace wiiu
