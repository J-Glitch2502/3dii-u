// fs:USER: RomFS from romfs.bin, save data / extdata / sdmc in host folders.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>

#include "../cloud.h"
#include "../loader.h"
#include "../service.h"
#include "services.h"

namespace stacks {
void save_written(const std::string &path);  // (runtime/stacks.h)
}

namespace fsys = std::filesystem;

#ifdef __WIIU__
namespace wiiu {
bool av_timing_enabled();
void note(const char *fmt, ...);  // frontend_wiiu.cpp
void fs_begin(u32 op);
void fs_end();
void romfs_begin();  // the game data's reads: a lock of their own (frontend_wiiu.cpp)
void romfs_end();
void direct_read_open(const char *path, int fd, u64 size);  // direct_read.cpp
bool direct_read(void *dst, u32 len, u64 off, bool *cached = nullptr);
long save_write(int fd, const void *data, u32 len, u64 at);  // save_storage.cpp
void save_resize(int fd, u64 size);
void save_dirty();
// the game's save files held in memory, written out by the card thread (save_storage.cpp)
struct SaveFile;
std::shared_ptr<SaveFile> save_file_open(const std::string &path, int fd);
void save_file_close(SaveFile &f);
void save_file_close_direct(const std::string &path);
long save_file_read(SaveFile &f, void *to, u32 len, u64 at);
long save_file_write(SaveFile &f, const void *from, u32 len, u64 at);
u64 save_file_size(SaveFile &f);
void save_file_resize(SaveFile &f, u64 size);
bool save_settle(const std::string &prefix, bool forget);
}
using wiiu::save_write;
using wiiu::save_resize;
#define FS_NOTE(...) wiiu::note(__VA_ARGS__)
#define SAVE_DIRTY() wiiu::save_dirty()
#define SAVE_SETTLE(prefix, forget) wiiu::save_settle(prefix, forget)
// a file operation of the game's: it takes its turn with the others, and a frozen game reports which one it's stuck in. Reads of the game's RomFS (0x0802, read
// only, from the title's content) take turns only among themselves, so a save being written out or flushed (seconds, for Tomodachi Life) never holds up loading.
struct FsIo {
    const bool romfs;
    explicit FsIo(u32 op) : romfs(op == 0x0802) { romfs ? wiiu::romfs_begin() : wiiu::fs_begin(op); }
    ~FsIo() { romfs ? wiiu::romfs_end() : wiiu::fs_end(); }
};
#else
#define FS_NOTE(...)
#define SAVE_DIRTY()
#define SAVE_SETTLE(prefix, forget) true
struct FsIo {
    explicit FsIo(u32) {}
};
static long save_write(int fd, const void *data, u32 len, u64 at) { return pwrite(fd, data, len, (off_t)at); }
static void save_resize(int fd, u64 size) {
    if (ftruncate(fd, (off_t)size)) {
    }
}
#endif

namespace kernel {

static u32 fs_err(u32 desc, u32 summary, u32 level = 25) { return (level << 27) | (summary << 21) | (17 << 10) | desc; }
static const u32 ERR_FILE_NOT_FOUND = fs_err(112, 4);
static const u32 ERR_PATH_NOT_FOUND = fs_err(113, 4);
static const u32 ERR_NOT_FOUND = fs_err(120, 4);
static const u32 ERR_NOT_FORMATTED = fs_err(340, 5);
static const u32 ERR_FILE_EXISTS = fs_err(180, 1);
static const u32 ERR_DIR_EXISTS = fs_err(185, 1);
static const u32 ERR_ARCHIVE = fs_err(101, 4);
static const u32 ERR_UNSUPPORTED = fs_err(760, 6, 28);
static const u32 ERR_FS_IO = fs_err(0, 11, 27);  // FS/Internal/Permanent: host cause has no verified CTR description
static const u32 ERR_FS_PERMISSION = fs_err(630, 8, 27);  // CommandNotAllowed

static u32 delete_error(const std::error_code &ec) {
    if (!ec || ec == std::errc::no_such_file_or_directory) return ERR_FILE_NOT_FOUND;
    if (ec == std::errc::not_a_directory) return ERR_PATH_NOT_FOUND;
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted ||
        ec == std::errc::read_only_file_system) return ERR_FS_PERMISSION;
    if (ec == std::errc::directory_not_empty) return fs_err(240, 9);
    if (ec == std::errc::is_a_directory) return fs_err(250, 9);
    if (ec == std::errc::no_space_on_device) return fs_err(210, 3);
    return ERR_FS_IO;
}

static u32 delete_host_file(const std::string &path, std::error_code &ec) {
    // Aroma versions can turn remove(ENOENT) into MEDIA_ERROR/EIO. Check
    // absence explicitly, so normal startup cleanup retains the missing-file
    // result without hiding an actual I/O or permission failure.
    const auto status = fsys::symlink_status(path, ec);
    if (ec || status.type() == fsys::file_type::not_found) return delete_error(ec);
    if (fsys::is_directory(status)) return fs_err(250, 9);  // DeleteFile must not remove directories
    const bool removed = fsys::remove(path, ec);
    return removed && !ec ? 0 : delete_error(ec);
}

static const u32 ERR_IO = fs_err(1007, 11);  // NoData, Internal: cached changes could not reach storage

}  // namespace kernel
extern std::string g_save_base;
namespace kernel {

std::string save_root() {
    char tid[32];
    snprintf(tid, sizeof tid, "%016llx", (unsigned long long)g_game.program_id);
    return g_save_base + "/" + std::string(tid);
}

}  // namespace kernel
// Held while the game writes its save, and while cloud saves read it (runtime/cloud.mm): never half a write
static std::mutex g_save_mx;
void save_lock() { g_save_mx.lock(); }
void save_unlock() { g_save_mx.unlock(); }
namespace kernel {
// A change to the game's own save (savedata/, extdata/): cloud saves upload it a little later
#ifdef RECOMP_RESUMABLE
static u64 checkpoint_write_epoch;
#endif
static void save_changed(const std::string &path) {
#ifdef RECOMP_RESUMABLE
    ++checkpoint_write_epoch;
#endif
    SAVE_DIRTY();
    static const std::string root = save_root() + "/";
    if (path.compare(0, root.size(), root)) return;
    stacks::save_written(path);  // (before the cloud copy is made: the stacks file goes with the save)
    cloud_save_written();
}

// ---- paths
struct LowPath {
    u32 type = 1;
    std::vector<u8> data;
    std::string str() const {
        if (type == 3) return std::string(data.begin(), std::find(data.begin(), data.end(), 0));
        if (type == 4) {
            std::string s;
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                u16 ch = data[i] | (data[i + 1] << 8);
                if (!ch) break;
                if (ch < 0x80) s += (char)ch;
                else if (ch < 0x800) {
                    s += (char)(0xC0 | (ch >> 6));
                    s += (char)(0x80 | (ch & 0x3F));
                } else {
                    s += (char)(0xE0 | (ch >> 12));
                    s += (char)(0x80 | ((ch >> 6) & 0x3F));
                    s += (char)(0x80 | (ch & 0x3F));
                }
            }
            return s;
        }
        if (type == 1) return "";
        std::string s;  // binary: hex
        char b[4];
        for (u8 c : data) {
            snprintf(b, sizeof b, "%02x", c);
            s += b;
        }
        return s;
    }
    u32 word(int i) const {
        u32 v = 0;
        if ((size_t)(4 * i + 4) <= data.size()) memcpy(&v, &data[4 * i], 4);
        return le32(v);
    }
};

static LowPath read_path(u32 type, u32 size, u32 ptr) {
    LowPath p;
    p.type = type;
    if (size && ptr) p.data.assign(gptr(ptr), gptr(ptr) + size);
    return p;
}

static std::string sanitize(const std::string &s) {
    // resolve . and .. (homebrew passes paths like /3ds/../rom.nes), never leaving the archive folder
    std::vector<std::string> parts;
    for (const auto &part : fsys::path(s)) {
        std::string q = part.string();
        if (q == "/" || q == "." || q.empty()) continue;
        if (q == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(q);
    }
    std::string out;
    for (auto &q : parts) out += "/" + q;
    return out;
}

// ---- archives
struct Archive {
#ifdef RECOMP_RESUMABLE
    void state_fields(checkpoint::Archive &a) { a.fields(id, host, romfs, dlc); }
#endif
    u32 id;
    std::string host;  // host folder for directory-backed archives, or the RomFS image of a system archive
    bool romfs = false;
    bool dlc = false;  // add-on content: host is the prefix of its contents' RomFS images, dlc_<content index>.romfs (builder/dumps.py)
};

// System archives (NCCH archive 0x2345678A) come from system_archives/<title id>.romfs, next to the game files
// in the working directory or in the source tree (tools/import_system_archives.py makes them).
static std::string system_archive_path(u64 tid) {
    char name[64];
    snprintf(name, sizeof name, "%016llx.romfs", (unsigned long long)tid);
    #ifdef __WIIU__
    if (fsys::exists(g_game.dir + "/" + name)) return g_game.dir + "/" + name;
    return "";  // the others are relative to the working directory, which on the console is the SD card
#endif
    for (std::string dir : {g_game.dir + "/../system_archives/", g_game.dir + "/system_archives/", std::string("system_archives/")
#ifdef RECOMP_SOURCE_DIR
                            , std::string(RECOMP_SOURCE_DIR "/system_archives/")
#endif
         }) {
        if (fsys::exists(dir + name)) return dir + name;
    }
    return "";
}

std::vector<u8> system_archive_file(u64 tid, const std::string &want) {
    std::string host = system_archive_path(tid);
    FILE *f = host.empty() ? nullptr : fopen(host.c_str(), "rb");
    if (!f) return {};
    std::vector<u8> d;
    u8 buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t base = d.size() >= 4 && !memcmp(d.data(), "IVFC", 4) ? 0x1000 : 0;  // level 3 image
    auto u = [&](size_t o) { u32 v = 0; if (base + o + 4 <= d.size()) memcpy(&v, &d[base + o], 4); return le32(v); };
    u32 dmo = u(0x0C), fmo = u(0x1C), dato = u(0x24);
    auto name = [&](size_t o, u32 len) {
        std::string s;
        for (u32 i = 0; i + 1 < len && base + o + i < d.size(); i += 2) s += (char)d[base + o + i];
        return s;
    };
    std::vector<u8> out;
    std::function<void(u32, std::string)> walk = [&](u32 dir, std::string path) {
        for (u32 fe = u(dmo + dir + 12); fe != 0xFFFFFFFF && out.empty(); fe = u(fmo + fe + 4)) {
            if (path + "/" + name(fmo + fe + 32, u(fmo + fe + 28)) != want) continue;
            u64 off = base + dato + ((u64)u(fmo + fe + 8) | (u64)u(fmo + fe + 12) << 32);
            u64 size = (u64)u(fmo + fe + 16) | (u64)u(fmo + fe + 20) << 32;
            if (off + size <= d.size()) out.assign(d.begin() + off, d.begin() + off + size);
        }
        for (u32 c = u(dmo + dir + 8); c != 0xFFFFFFFF && out.empty(); c = u(dmo + c + 4))
            walk(c, path + "/" + name(dmo + c + 24, u(dmo + c + 20)));
    };
    walk(0, "");
    return out;
}

static std::map<u64, Archive> archives;
static u64 next_archive = 1;

static int romfs_fd = -1;
static u64 romfs_size;

// RomFS index, for logging which files the game reads
struct RomfsFile {
    u64 off, size;
    std::string path;
};
static std::vector<RomfsFile> romfs_index;
// RECOMP_ROMFS_SWAP="A=B,...": every RomFS file whose name contains A gets the data of the file named with B
// instead (by rewriting its metadata entry as the game reads it), e.g. FirstStage=CaveInkStage to load another
// level in place of World 1-1 when testing.
struct RomfsPatch { u64 at; u8 bytes[16]; };
static std::vector<RomfsPatch> romfs_patches;
// RECOMP_ROMFS_REPLACE="<path>=<host file>,...": serve a host file in place of a RomFS file (for mods and tests).
// Its data is placed past the end of the image, where reads are redirected to the host file.
struct RomfsExtra { u64 off, size; int fd; };
static std::vector<RomfsExtra> romfs_extra;

static ssize_t romfs_read_at(void *dst, size_t len, u64 off) {
#ifdef __WIIU__
    if (len <= UINT32_MAX && wiiu::direct_read(dst, (u32)len, off)) return (ssize_t)len;
#endif
    return pread(romfs_fd, dst, len, (off_t)off);
}

static void build_romfs_index() {
    std::vector<u8> hdr(0x28);
    if (romfs_read_at(hdr.data(), 0x28, 0) != 0x28) return;
    auto u32at = [](const std::vector<u8> &b, size_t o) { u32 v; memcpy(&v, &b[o], 4); return le32(v); };
    u32 dmeta_off = u32at(hdr, 0x0C), dmeta_len = u32at(hdr, 0x10);
    u32 fmeta_off = u32at(hdr, 0x1C), fmeta_len = u32at(hdr, 0x20);
    u32 data_off = u32at(hdr, 0x24);
    std::vector<u8> dm(dmeta_len), fm(fmeta_len);
    if (romfs_read_at(dm.data(), dmeta_len, dmeta_off) != (ssize_t)dmeta_len) return;
    if (romfs_read_at(fm.data(), fmeta_len, fmeta_off) != (ssize_t)fmeta_len) return;
    auto name = [](const std::vector<u8> &b, size_t o, u32 len) {
        std::string s;
        for (u32 i = 0; i + 1 < len; i += 2) s += (char)b[o + i];
        return s;
    };
    std::function<void(u32, std::string)> walk = [&](u32 d, std::string path) {
        u32 child = u32at(dm, d + 8), file = u32at(dm, d + 12);
        while (file != 0xFFFFFFFF) {
            u64 fo, fs;
            memcpy(&fo, &fm[file + 8], 8);
            memcpy(&fs, &fm[file + 16], 8);
            fo = le64(fo);
            fs = le64(fs);
            romfs_index.push_back({data_off + fo, fs, path + "/" + name(fm, file + 32, u32at(fm, file + 28))});
            file = u32at(fm, file + 4);
        }
        while (child != 0xFFFFFFFF) {
            walk(child, path + "/" + name(dm, child + 24, u32at(dm, child + 20)));
            child = u32at(dm, child + 4);
        }
    };
    walk(0, "");
    if (const char *rp = getenv("RECOMP_ROMFS_REPLACE")) {
        std::map<std::string, u32> entry;
        std::function<void(u32, std::string)> walk2 = [&](u32 d, std::string path) {
            for (u32 f = u32at(dm, d + 12); f != 0xFFFFFFFF; f = u32at(fm, f + 4))
                entry[path + "/" + name(fm, f + 32, u32at(fm, f + 28))] = f;
            for (u32 c = u32at(dm, d + 8); c != 0xFFFFFFFF; c = u32at(dm, c + 4))
                walk2(c, path + "/" + name(dm, c + 24, u32at(dm, c + 20)));
        };
        walk2(0, "");
        u64 end = (romfs_size + 0xFFF) & ~0xFFFull;
        std::string spec = rp;
        for (size_t pos = 0; pos < spec.size();) {
            size_t e = spec.find(',', pos);
            std::string item = spec.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? spec.size() : e + 1;
            size_t eq = item.find('=');
            auto it = eq == std::string::npos ? entry.end() : entry.find(item.substr(0, eq));
            int fd = eq == std::string::npos ? -1 : open(item.substr(eq + 1).c_str(), O_RDONLY);
            if (it == entry.end() || fd < 0) {
                LOG("[fs] romfs replace: cannot use '%s'\n", item.c_str());
                continue;
            }
            struct stat st;
            fstat(fd, &st);
            romfs_extra.push_back({end, (u64)st.st_size, fd});
            RomfsPatch p;
            p.at = fmeta_off + it->second + 8;
            u64 fo = end - data_off, sz = (u64)st.st_size;
            memcpy(p.bytes, &fo, 8);
            memcpy(p.bytes + 8, &sz, 8);
            romfs_patches.push_back(p);
            LOG("[fs] romfs replace: %s <- %s\n", it->first.c_str(), item.substr(eq + 1).c_str());
            end = (end + sz + 0xFFF) & ~0xFFFull;
        }
        romfs_size = end;
    }
    if (const char *sw = getenv("RECOMP_ROMFS_SWAP")) {
        std::map<std::string, u32> entry;  // path -> offset of its file metadata entry
        std::function<void(u32, std::string)> walk2 = [&](u32 d, std::string path) {
            for (u32 f = u32at(dm, d + 12); f != 0xFFFFFFFF; f = u32at(fm, f + 4))
                entry[path + "/" + name(fm, f + 32, u32at(fm, f + 28))] = f;
            for (u32 c = u32at(dm, d + 8); c != 0xFFFFFFFF; c = u32at(dm, c + 4))
                walk2(c, path + "/" + name(dm, c + 24, u32at(dm, c + 20)));
        };
        walk2(0, "");
        std::string spec = sw;
        for (size_t pos = 0; pos < spec.size();) {
            size_t e = spec.find(',', pos);
            std::string item = spec.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? spec.size() : e + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            std::string from = item.substr(0, eq), to = item.substr(eq + 1);
            for (auto &[path, f] : entry) {
                size_t at = path.find(from);
                if (at == std::string::npos) continue;
                auto it = entry.find(path.substr(0, at) + to + path.substr(at + from.size()));
                if (it == entry.end()) continue;
                RomfsPatch p;
                p.at = fmeta_off + f + 8;
                memcpy(p.bytes, &fm[it->second + 8], 16);
                romfs_patches.push_back(p);
                LOG("[fs] romfs swap: %s -> %s\n", path.c_str(), it->first.c_str());
            }
        }
    }
    std::sort(romfs_index.begin(), romfs_index.end(), [](auto &a, auto &b) { return a.off < b.off; });
}

// a RomFS file by path ("/LayoutData/x.szs"), empty if missing (host side: HUD icons)
static std::mutex romfs_mtx;
static bool open_romfs() {
    std::lock_guard<std::mutex> lk(romfs_mtx);
    if (romfs_fd >= 0) return true;
    const int fd = open((g_game.dir + "/romfs.bin").c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    romfs_size = (u64)st.st_size;
    romfs_fd = fd;
#ifdef __WIIU__
    {
        FsIo io(0x0809);
        wiiu::direct_read_open((g_game.dir + "/romfs.bin").c_str(), fd, romfs_size);
    }
#endif
    build_romfs_index();
    return true;
}

std::vector<u8> romfs_file(const std::string &path) {
    if (!open_romfs()) return {};
    std::lock_guard<std::mutex> lk(romfs_mtx);
    for (const auto &f : romfs_index)
        if (f.path == path) {
            std::vector<u8> d(f.size);
            if (romfs_read_at(d.data(), f.size, f.off) != (ssize_t)f.size) return {};
            return d;
        }
    return {};
}

static void log_romfs_read(u64 off) {
    if (!g_verbose) return;
    auto it = std::upper_bound(romfs_index.begin(), romfs_index.end(), off,
                               [](u64 v, const RomfsFile &f) { return v < f.off; });
    if (it == romfs_index.begin()) return;
    --it;
    static RomfsFile *last;
    if (off < it->off + it->size && &*it != last) {
        LOG("[fs] romfs: %s%s\n", it->path.c_str(), off == it->off ? "" : " (inside)");
        last = &*it;
    }
}

// A RomFS read takes as long as on a 3DS (the game card's latency and speed, as measured for Citra on an old 3DS
// and a 2DS: 94 ns per byte + 0.58 ms, at least 0.66 ms): the reading thread sleeps that long after the reply.
// Games load in the background and expect the rest of the frame to go on meanwhile; an instant read can finish a
// load before the code that started it has set up for its end (Animal Crossing: New Leaf's map process cleared
// its own "ready" flag after a synchronous load, leaving the village's bottom screen blank and untouchable).
// What the read itself took here (spent_ns) counts towards it: a console's storage is no faster than a game card.
// RECOMP_FS_DELAY=0 turns it off.
static u64 romfs_read_delay(IPC &ipc, u32 len, u64 spent_ns, bool cached = false) {
    static const bool on = !getenv("RECOMP_FS_DELAY") || atoi(getenv("RECOMP_FS_DELAY")) != 0;
    // An opt-in host read-ahead hit already has its bytes in RAM. Preserve the
    // asynchronous minimum, without charging another cartridge transfer for
    // data whose physical read was charged to the earlier miss.
    const u64 on_a_3ds = cached ? 663124 : std::max<u64>((u64)len * 94 + 582778, 663124);
    const u64 wait = on && spent_ns < on_a_3ds ? on_a_3ds - spent_ns : 0;
    if (wait) sleep_current(wait, *ipc.lk);
    return wait;
}

#ifdef __WIIU__
// All updates run with the kernel lock held, after run_blocked has rejoined
// the guest. IO includes waiting for the RomFS read lock; resume is the time
// outside that native operation. Neither is labelled as pure SD-device time.
static void report_read_timing(u32 bytes, u64 io_ns, u64 spent_ns, u64 wait_ns) {
    if (!wiiu::av_timing_enabled()) return;
    static u64 start, total_bytes, count, max_io, max_resume, max_wait, total_io, fast_reads;
    const u64 now = real_ns();
    if (!start) start = now;
    count++;
    total_bytes += bytes;
    total_io += io_ns;
    fast_reads += io_ns < 1000000;
    max_io = std::max(max_io, io_ns);
    max_resume = std::max(max_resume, spent_ns > io_ns ? spent_ns - io_ns : 0);
    max_wait = std::max(max_wait, wait_ns);
    if (now - start < 2000000000ULL) return;
    LOG("[fs-time] %llu reads, %llu KB in %llu ms; max native IO %llu us, resume %llu us, emulated wait %llu us; mean IO %llu us, fast reads %llu\n",
        (unsigned long long)count, (unsigned long long)(total_bytes >> 10), (unsigned long long)((now - start) / 1000000),
        (unsigned long long)(max_io / 1000), (unsigned long long)(max_resume / 1000), (unsigned long long)(max_wait / 1000),
        (unsigned long long)(total_io / count / 1000), (unsigned long long)fast_reads);
    start = now;
    total_bytes = count = max_io = max_resume = max_wait = total_io = fast_reads = 0;
}
#endif

static std::string archive_dir(u32 id, const LowPath &p) {
    std::string root = save_root();
    switch (id) {
    case 4: return root + "/savedata";
    case 6:
    case 7: {
        char b[64];
        snprintf(b, sizeof b, "/extdata/%08x%08x", p.word(2), p.word(1));
        return (id == 7 ? g_save_base + "/shared" : root) + b;
    }
    case 8: {
        char b[64];
        snprintf(b, sizeof b, "/system/%08x%08x", p.word(1), p.word(0));
        return g_save_base + b;
    }
    case 9:  // SDMC
    case 10: // SDMCWriteOnly
        return g_save_base + "/sdmc";
    }
    return "";
}

// ---- file and directory sessions
struct FileSession : Service {
    int fd = -1;
    int open_flags = O_RDONLY;
    bool romfs = false, written = false;
    bool extdata = false;
    bool gamedata = false;  // read-only game data in a file of its own (an update's RomFS, DLC, system archives, ExeFS files): the RomFS's lock, never behind a save write-out
    u64 base = 0, size = 0;
    std::string path;
#ifdef __WIIU__
    std::shared_ptr<wiiu::SaveFile> held;  // a save file of the game's, in memory: no storage behind its reads and writes
    bool direct_save = false;  // this session reserved an uncached save path
#endif
    FileSession() : Service("fs file") {}
#ifdef RECOMP_RESUMABLE
    bool persistent_supported() const override { return true; }
    bool capture(checkpoint::Image &im) override {
        if (im.archive) return persist(im);
        im.take(open_flags); im.take(fd); im.take(romfs); im.take(written); im.take(extdata); im.take(gamedata);
        im.take(base); im.take(size); im.take(path);
#ifdef __WIIU__
        im.take(held);
#endif
        return true;
    }
    bool persist(checkpoint::Image &im) {
        auto &a = *im.archive;
        bool saved_romfs = romfs, saved_written = written, saved_extdata = extdata, saved_gamedata = gamedata;
        u64 saved_base = base, saved_size = size, length = 0, fingerprint = 0;
        int flags = open_flags;
        std::string saved_path = path;
        a.fields(saved_romfs, saved_written, saved_extdata, saved_gamedata, saved_base, saved_size, flags, saved_path);
        if (!a.ok) return false;
        if (!a.reading && !saved_romfs) {
            if (saved_path.empty()) { a.fail("open file has no reopenable path"); return false; }
            if (!file_fingerprint(length, fingerprint)) { a.fail("could not fingerprint open file"); return false; }
        }
        a.fields(length, fingerprint);
        if (a.reading && a.ok) {
            // This instance was freshly constructed by PersistentGraph; no live
            // service is mutated until the staged image is committed.
            romfs = saved_romfs; extdata = saved_extdata; gamedata = saved_gamedata;
            base = saved_base; size = saved_size; path = saved_path; open_flags = flags & O_RDWR;
            if (!romfs) {
                const auto normalized = fsys::path(path).lexically_normal().generic_string();
                auto inside = [&](const std::string &root) { return !root.empty() && normalized.rfind(root + "/", 0) == 0; };
                if (path.find('\0') != std::string::npos || normalized != path || (!inside(g_game.dir) && !inside(g_save_base))) {
                    a.fail("saved file path is outside title storage"); return false;
                }
                fd = open(path.c_str(), open_flags); // never create/truncate a permanent save
                if (fd < 0) { a.fail("saved file cannot be reopened"); return false; }
#ifdef __WIIU__
                if (!path.compare(0,g_save_base.size(),g_save_base)) {
                    held = wiiu::save_file_open(path,fd);
                    if (held) { close(fd); fd=-1; }
                }
#endif
                u64 actual_length=0, actual_fingerprint=0;
                if (!file_fingerprint(actual_length,actual_fingerprint) || actual_length!=length || actual_fingerprint!=fingerprint) {
                    a.fail("permanent file changed since this state was saved"); return false;
                }
            }
        }
        // Retain the normal session callbacks too, so subsequent loads of this
        // newly reconstructed state can use the in-memory path.
        auto *stream = im.archive; im.archive = nullptr;
        const bool result = capture(im); im.archive = stream;
        if (a.reading) im.restore.push_back([this, saved_written] { written = saved_written; });
        return result && a.ok;
    }
    bool file_fingerprint(u64 &length, u64 &fingerprint) {
        length = file_size();
        // Immutable game files can be large; their executable/build identity
        // is checked globally, with a per-file size check here.
        if (gamedata) { fingerprint = 0; return true; }
        if (length > (16u << 20)) return false;
        fingerprint = 14695981039346656037ull;
        u8 buffer[4096];
        for (u64 at=0; at<length;) {
            const u32 count = std::min<u64>(sizeof buffer,length-at);
            long got;
#ifdef __WIIU__
            if (held) got = wiiu::save_file_read(*held,buffer,count,at);
            else
#endif
            got = pread(fd,buffer,count,at);
            if (got != (long)count) return false;
            for (u32 i=0;i<count;++i) fingerprint = (fingerprint ^ buffer[i]) * 1099511628211ull;
            at += count;
        }
        return true;
    }
#endif
    ~FileSession() override {
#ifdef __WIIU__
        if (held) {
            wiiu::save_file_close(*held);
            if (written) SAVE_DIRTY();
            return;
        }
#endif
        if (fd >= 0 && !romfs) {
            FsIo io(gamedata ? 0x0802 : 0x0808);
            close(fd);
            if (written) SAVE_DIRTY();  // a file's last changes may only go out as it closes
        }
#ifdef __WIIU__
        if (direct_save) wiiu::save_file_close_direct(path);
#endif
    }
    u64 file_size() {
        if (romfs) return size;
#ifdef __WIIU__
        if (held) return wiiu::save_file_size(*held);
#endif
        FsIo io(gamedata ? 0x0802 : 0x0804);
        struct stat st;
        fstat(fd, &st);
        return (u64)st.st_size;
    }
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0802: {  // Read(offset u64, size, mapped buffer)
            u64 off = (u64)ipc[1] | ((u64)ipc[2] << 32);
            u32 len = ipc[3];
            u32 dst = ipc[5];
            s64 n;
            // Storage can take a long time to answer: the game's other threads run meanwhile, as they do on a 3DS (run_blocked)
            const auto began = std::chrono::steady_clock::now();
            u64 io_ns = 0;
            bool cached = false;
            run_blocked(*ipc.lk, [&] {
            const auto io_began = std::chrono::steady_clock::now();
            if (romfs) {
                FsIo io(0x0802);
                log_romfs_read(base + off);
                if (off >= size) n = 0;
#ifdef __WIIU__
                else if (wiiu::direct_read(gptr(dst), (u32)std::min<u64>(len, size - off), base + off, &cached)) n = (s64)std::min<u64>(len, size - off);
#endif
                else n = pread(romfs_fd, gptr(dst), (size_t)std::min<u64>(len, size - off), (off_t)(base + off));
                for (auto &x : romfs_extra)
                    if (base + off >= x.off && base + off < x.off + x.size)
                        n = pread(x.fd, gptr(dst), (size_t)std::min<u64>(len, x.off + x.size - base - off),
                                  (off_t)(base + off - x.off));
                for (auto &p : romfs_patches)
                    for (u32 i = 0; i < 16; i++)
                        if (p.at + i >= base + off && p.at + i < base + off + (u64)std::max<s64>(n, 0))
                            w8(dst + (u32)(p.at + i - base - off), p.bytes[i]);
            } else {
#ifdef __WIIU__
                if (held) n = wiiu::save_file_read(*held, gptr(dst), len, off);
                else
#endif
                {
                    FsIo io(gamedata ? 0x0802 : 0x1802);
                    n = pread(fd, gptr(dst), len, (off_t)off);
                }
            }
            io_ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - io_began).count();
            });
            const u64 spent_ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count();
            if (n < 0) n = 0;
            if (n) gsp_cpu_wrote(dst, (u32)n);
            if (g_verbose >= 2) LOG("[fs] read %s off %llx len %x -> %lld\n", romfs ? "romfs" : path.c_str(), (unsigned long long)off, len, (long long)n);
            ipc.ok(2, 2);
            ipc.set(2, (u32)n);
            ipc.set(3, (len << 4) | 0xC);
            ipc.set(4, dst);
            if (romfs) {
                const u64 wait_ns = romfs_read_delay(ipc, len, spent_ns, cached);
#ifdef __WIIU__
                report_read_timing(len, io_ns, spent_ns, wait_ns);
#endif
            }
            if (extdata) {
                // Azahar's extdata timing keeps these reads asynchronous to
                // the game. Mario Maker advances its startup after the read.
                const u64 delay = std::max<u64>((u64)len * 183 + 524879, 631826);
                if (spent_ns < delay) sleep_current(delay - spent_ns, *ipc.lk);
            }
            break;
        }
        case 0x0803: {  // Write(offset u64, size, flush, mapped buffer)
            u64 off = (u64)ipc[1] | ((u64)ipc[2] << 32);
            u32 len = ipc[3];
            u32 src = ipc[6];
            s64 n;
            run_blocked(*ipc.lk, [&] {
            FS_NOTE("save: writing %x bytes to %s", len, path.c_str());
            save_lock();
#ifdef __WIIU__
            if (held) n = wiiu::save_file_write(*held, gptr(src), len, off);
            else
#endif
            {
                FsIo io(0x0803);
                n = romfs ? 0 : save_write(fd, gptr(src), len, off);
            }
            save_unlock();
            FS_NOTE("save: written, %d", (int)n);
            });
            if (n < 0) n = 0;
            if (g_verbose) LOG("[fs] write %s off %llx len %x\n", path.c_str(), (unsigned long long)off, len);
            if (!romfs && n > 0) {
                written = true;
                save_changed(path);
            }
            ipc.ok(2, 2);
            ipc.set(2, (u32)n);
            ipc.set(3, (len << 4) | 0xA);
            ipc.set(4, src);
            break;
        }
        case 0x0804: {  // GetSize
            u64 size;
            run_blocked(*ipc.lk, [&] { size = file_size(); });
            ipc.ok(3);
            ipc.set64(2, size);
            break;
        }
        case 0x0805: {  // SetSize
            if (!romfs) {
                const u64 size = (u64)ipc[1] | ((u64)ipc[2] << 32);
                run_blocked(*ipc.lk, [&] {
                save_lock();
#ifdef __WIIU__
                if (held) wiiu::save_file_resize(*held, size);
                else
#endif
                {
                    FsIo io(0x0805);
                    save_resize(fd, size);
                }
                save_unlock();
                });
                written = true;
                save_changed(path);
            }
            ipc.ok();
            break;
        }
        case 0x0808:  // Close
        case 0x0809:  // Flush
        case 0x080A:  // SetPriority
            ipc.ok();
            break;
        case 0x080B:
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x080C:  // OpenLinkFile
            ipc.ok(1, 2);
            ipc.handles(2, {get_object_for(shared_from_this())}, true);
            break;
        default:
            unimplemented(ipc);
        }
    }
    static ObjPtr get_object_for(std::shared_ptr<Service> s) {
        auto ses = std::make_shared<Session>();
        ses->service = s;
        return ses;
    }
};

struct DirSession : Service {
    SERVICE_CHECKPOINT(DirSession, entries, pos)
    std::vector<fsys::directory_entry> entries;
    size_t pos = 0;
    DirSession() : Service("fs dir") {}
    void handle(IPC &ipc) override {
        FsIo io(0x2800);
        switch (ipc.cmd()) {
        case 0x0801: {  // Read(count, mapped buffer)
            u32 count = ipc[1];
            u32 dst = ipc[3];
            u32 n = 0;
            while (n < count && pos < entries.size()) {
                auto &e = entries[pos++];
                u32 a = dst + n * 0x228;
                memset(gptr(a), 0, 0x228);
                std::string name = e.path().filename().string();
                for (size_t i = 0; i < name.size() && i < 0x105; i++) w16(a + 2 * (u32)i, (u8)name[i]);
                std::string stem = e.path().stem().string(), ext = e.path().extension().string();
                for (size_t i = 0; i < 8 && i < stem.size(); i++) w8(a + 0x20C + (u32)i, (u8)toupper(stem[i]));
                for (size_t i = 1; i < 4 && i < ext.size(); i++) w8(a + 0x216 + (u32)i - 1, (u8)toupper(ext[i]));
                bool dir = e.is_directory();
                w8(a + 0x21C, dir);
                w8(a + 0x21E, !dir);
                w64(a + 0x220, dir ? 0 : (u64)e.file_size());
                n++;
            }
            VLOG("[fs] directory read %u into %08x (buffer %x bytes): %u%s%s\n", count, dst, ipc[2] >> 4, n, n ? ", first " : "", n ? entries[pos - n].path().filename().string().c_str() : "");
            ipc.ok(2, 2);
            ipc.set(2, n);
            ipc.set(3, ((count * 0x228) << 4) | 0xC);
            ipc.set(4, dst);
            break;
        }
        case 0x0802:
            ipc.ok();
            break;
        default:
            unimplemented(ipc);
        }
    }
};

// Shared extdata F000000B always exists on a console (made at system setup): it holds gamecoin.dat, the
// Play Coin counter, which some games read (or fail) without creating it.
// What an archive was formatted with: its size in bytes, how many directories and files it's for, and whether it keeps duplicate data. A game
// asks for these back (GetFormatInfo) and formats again when they aren't what it formats with: The Legend of Zelda: A Link Between Worlds
// wiped its save at every start while the answer was the same four numbers for every game (2026-11-11). Kept in a file beside the archive's
// folder, in the byte order of the machine that formatted it, and read in either: a save made by the PC build read as no save on the Wii U
// (Pokemon Ultra Sun went to its language choice, 2026-11-13).
struct FormatInfo {
    u32 bytes = 0x20000, dirs = 0x10, files = 0x10, duplicate = 0;  // what's answered for an archive formatted before this was kept
};
static bool keep_format_info(const std::string &dir, const FormatInfo &info) {
    FILE *f = fopen((dir + ".format").c_str(), "wb");
    if (!f) return false;
    const bool written = fwrite(&info, sizeof info, 1, f) == 1;
    const int closed = fclose(f);
    return written && !closed;
}
static FormatInfo format_info(const std::string &dir) {
    FormatInfo info, kept;
    if (FILE *f = fopen((dir + ".format").c_str(), "rb")) {
        if (fread(&kept, sizeof kept, 1, f) == 1) info = kept;
        fclose(f);
    }
    // the other byte order's: a count of directories or files, or the flag, reads as millions. shortcut: with all three zero nothing tells
    if ((info.dirs | info.files | info.duplicate) > 0xFFFF)
        info = {__builtin_bswap32(info.bytes), __builtin_bswap32(info.dirs), __builtin_bswap32(info.files), __builtin_bswap32(info.duplicate)};
    return info;
}

static bool create_gamecoin(const std::string &dir) {
    std::error_code ec;
    fsys::create_directories(dir, ec);
    if (ec) return false;
    struct { u32 magic; u16 coins, coins_today; u32 steps, last_steps; u16 year; u8 month, day; } gc{le32(0x4F00), le16(10), 0, 0, 0, le16(2014), 1, 1};
    const std::string path = dir + "/gamecoin.dat", fresh = path + ".3diiu-new";
    if (fsys::exists(path, ec)) return fsys::is_regular_file(path, ec) && !ec;
    if (ec) return false;
    FILE *f = fopen(fresh.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&gc, sizeof gc, 1, f) == 1;
    if (fflush(f)) ok = false;
    if (fclose(f)) ok = false;
    if (ok && !fsys::exists(path, ec) && !ec && !rename(fresh.c_str(), path.c_str())) {
        LOG("[fs] created shared extdata F000000B with gamecoin.dat (10 Play Coins)\n");
        return true;
    }
    fsys::remove(fresh, ec);
    return fsys::is_regular_file(path, ec) && !ec;
}

// Mii Maker's database (CFL_DB.dat, in the same extdata), with the console's own Miis put in, once a boot: games list them as Mii Maker's
// (Tomodachi Life's import, Mario Kart 7's drivers). Laid out as the games' own Mii library writes it when it's missing, which this
// matches byte for byte: "CFOG", 100 Mii slots of 0x5C at 0x8, "CFHE" with 3000 empty hidden-Mii entries, a CRC-16 of all of it at 0xC81E
// (big-endian), then "CFRA" with its own CRC at 0xE4BE. A Mii already there with the same id and console stays where it is, a new one
// takes the first free place, and each is written naming the place it lies at (page and slot, in its third byte). A 3DS keeps a Mii at
// the place it names, and games find it again by that: Super Mario 3D Land shows no face on a save file whose Mii names a place where the
// database holds another Mii. Existing entries stay in their slots. The player's own is imported first when there is room.
static std::vector<int> mii_places;  // where each of console_miis() lies, or -1: every place was taken
static bool read_mii_db(FILE *f, std::vector<u8> &db) {
    const bool ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)db.size() && fseek(f, 0, SEEK_SET) == 0 &&
                    fread(db.data(), 1, db.size(), f) == db.size() && !ferror(f);
    const int closed = fclose(f);
    return ok && !closed;
}

static bool write_mii_db(const std::string &path, const std::vector<u8> &db, bool had) {
    const std::string fresh = path + ".3diiu-new", old = path + ".3diiu-old";
    std::error_code ec;
    fsys::remove(fresh, ec);
    if (ec) return false;
    FILE *f = fopen(fresh.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(db.data(), 1, db.size(), f) == db.size();
    if (fflush(f)) ok = false;
    if (fclose(f)) ok = false;
    if (ok) {
        std::vector<u8> check(db.size());
        f = fopen(fresh.c_str(), "rb");
        ok = f && read_mii_db(f, check) && check == db;
    }
    if (!ok) {
        fsys::remove(fresh, ec);
        return false;
    }
    if (had) {
        fsys::remove(old, ec);
        if (ec || rename(path.c_str(), old.c_str())) {
            fsys::remove(fresh, ec);
            return false;
        }
    } else if (fsys::exists(path, ec) || ec) {
        fsys::remove(fresh, ec);
        return false;  // a database appeared after the missing-file read
    }
    if (rename(fresh.c_str(), path.c_str())) {
        if (!had || !rename(old.c_str(), path.c_str())) fsys::remove(fresh, ec);
        return false;  // a failed rollback keeps the prior database in .3diiu-old
    }
    if (had) fsys::remove(old, ec);
    return true;
}

static void sync_mii_db(const std::string &dir) {
    static bool done;
    if (done) return;
    static std::chrono::steady_clock::time_point retry_after;
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_after) return;
    struct Cooldown {
        std::chrono::steady_clock::time_point &after;
        ~Cooldown() { after = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
    } cooldown{retry_after};  // include slow failed I/O before the next lookup can retry
    std::error_code ec;
    if (!create_gamecoin(dir)) return;
    int mine;
    const auto &miis = console_miis(&mine);
    std::vector<int> places(miis.size(), -1);
    if (miis.empty()) { mii_places.clear(); done = true; return; }
    const size_t players = mine < 0 ? 0 : (size_t)mine;  // as player_mii() has it
    const std::string path = dir + "/CFL_DB.dat";
    const std::string old = path + ".3diiu-old";
    const bool had_old = fsys::exists(old, ec);
    if (ec) return;
    const bool exists = fsys::exists(path, ec);
    if (ec) return;
    if (had_old && !exists && rename(old.c_str(), path.c_str())) return;
    std::vector<u8> db(0x4BD20, 0);
    bool had = false;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        had = read_mii_db(f, db) && !memcmp(db.data(), "CFOG", 4);
        if (!had) { LOG("[fs] Mii Maker's existing database could not be read; kept unchanged\n"); return; }
    } else if (errno != ENOENT || exists || had_old) {
        LOG("[fs] Mii Maker's database could not be opened; kept unchanged\n");
        return;
    }
    auto put_crc = [&](u32 from, u32 at) {
        const u16 c = mii_crc16(&db[from], at - from);
        db[at] = (u8)(c >> 8);
        db[at + 1] = (u8)c;
    };
    if (!had) {
        std::fill(db.begin(), db.end(), 0);
        memcpy(&db[0], "CFOG\0\x01\0\0", 8);
        memcpy(&db[0x23F8], "CFHE\xff\xff\xff\xff", 8);
        for (u32 i = 0; i < 3000; i++) memcpy(&db[0x2400 + i * 0xE + 10], "\xff\x7f\xff\x7f", 4);
        memcpy(&db[0xC820], "CFRA", 4);
        put_crc(0xC820, 0xE4BE);
    }
    const std::vector<u8> before(db.begin(), db.begin() + 0xC820);
    constexpr int official_slots = 100;
    constexpr int reserved_for_game = 10;  // Leave one page for Miis the game creates or copies to Mii Maker.
    int free_slots = 0;
    for (int i = 0; i < official_slots; i++) {
        if (mii_id_empty(&db[8 + i * 0x5C + 0x0C])) free_slots++;
    }
    u32 placed = 0;
    for (size_t k = 0; k < miis.size(); k++) {
        const size_t n = k == 0 ? players : k <= players ? k - 1 : k;  // the player's, then the rest in their order
        int slot = -1, empty = -1;
        for (int i = 0; i < official_slots && slot < 0; i++) {
            const u8 *s = &db[8 + i * 0x5C];
            if (!memcmp(s + 0x0C, &miis[n][0x0C], 10)) slot = i;  // the same Mii: its id and the console that made it
            else if (empty < 0 && mii_id_empty(s + 0x0C)) empty = i;
        }
        if (slot < 0) {
            if (empty < 0 || free_slots <= reserved_for_game) continue;
            slot = empty;
            free_slots--;
        }
        u8 *s = &db[8 + slot * 0x5C];
        memcpy(s, miis[n].data(), 0x5C);
        s[0x02] = (u8)(slot / 10 | slot % 10 << 4);
        places[n] = slot;
        placed++;
    }
    put_crc(0, 0xC81E);
    if (!(had && std::equal(before.begin(), before.end(), db.begin()))) {
        fsys::create_directories(dir, ec);
        if (ec || !write_mii_db(path, db, had)) {
            LOG("[fs] Mii Maker's database update failed; prior database kept\n");
            return;
        }
    }
    mii_places = std::move(places);
    done = true;
    LOG("[fs] Mii Maker's database: %u Mii%s from this console\n", placed, placed == 1 ? "" : "s");
}

// A Mii handed out elsewhere (the player's: misc.cpp) has to name the same place as its copy in the database, so the database is
// brought up to date here if the game hasn't opened it yet: in the folder a console has from the start, as open_archive makes it.
int mii_maker_place(u32 n) {
    const std::string dir = g_save_base + "/shared/extdata/00000000f000000b";
    sync_mii_db(dir);
    return n < mii_places.size() ? mii_places[n] : -1;
}

// ---- fs:USER
struct FsUser : Service {
    SERVICE_CHECKPOINT(FsUser)
    using Service::Service;

    u32 open_archive(u32 id, const LowPath &p, u64 *out) {
        Archive a{id, "", false};
        if (id == 3) {
            a.romfs = true;
        } else if (id == 0x2345678A) {
            u64 tid = (u64)p.word(0) | ((u64)p.word(1) << 32);
            a.dlc = (tid >> 32) == 0x0004008C;
            a.host = !a.dlc ? system_archive_path(tid) : fsys::exists(g_game.dir + "/dlc_contents.bin") ? g_game.dir + "/dlc_" : "";
            if (a.host.empty()) {
                if (!a.dlc && g_missing_system_archive != tid) {  // a game without DLC asking for it is normal
                    LOG("[fs] system archive %016llx isn't in this build\n", (unsigned long long)tid);
                    g_missing_system_archive = tid;
                }
                return ERR_NOT_FOUND;
            }
        } else {
            a.host = archive_dir(id, p);
            if (a.host.empty()) {
                LOG("[fs] OpenArchive: unsupported archive %x path %s\n", id, p.str().c_str());
                return ERR_NOT_FOUND;
            }
            std::error_code ec;
            if (id == 9 || id == 10) fsys::create_directories(a.host, ec);
            if (ec) return ERR_IO;
            bool exists = fsys::exists(a.host, ec);
            if (ec) return ERR_IO;
            if (id == 7 && p.word(1) == 0xF000000B) {
                if (!create_gamecoin(a.host)) return ERR_IO;
                exists = true;
                sync_mii_db(a.host);
            }
            if (!exists) {
                VLOG("[fs] archive %x (%s) not formatted\n", id, a.host.c_str());
                return id == 4 ? ERR_NOT_FORMATTED : ERR_NOT_FOUND;
            }
            if (!fsys::is_directory(a.host, ec) || ec) return ERR_IO;
        }
        *out = next_archive++;
        archives[*out] = a;
        VLOG("[fs] OpenArchive %x -> %llu (%s)\n", id, (unsigned long long)*out, a.host.c_str());
        g_missing_system_archive = 0;  // the game went on past a missing one it only looked for (a regional font, say): err:f mustn't blame it
        return 0;
    }

    Archive *arch(u32 lo, u32 hi) {
        auto it = archives.find((u64)lo | ((u64)hi << 32));
        return it == archives.end() ? nullptr : &it->second;
    }

    u32 open_file(Archive *a, const LowPath &p, u32 flags, ObjPtr *out) {
        if (a->id == 10 && (flags & 1)) return fs_err(700, 7, 28);
        auto fs = std::make_shared<FileSession>();
        fs->extdata = a->id == 6 || a->id == 7;
        if (a->id == 0x2345678A) {
            std::string host = a->host;
            if (a->dlc) {  // the file path: open type, content index, section (0 RomFS), ExeFS name
                char nm[24];
                snprintf(nm, sizeof nm, "%08x.romfs", p.data.size() >= 8 ? p.word(1) : 0);
                host += nm;
            }
            fs->path = host;
            fs->fd = open(host.c_str(), O_RDONLY);
            if (fs->fd < 0) return ERR_NOT_FOUND;
            fs->gamedata = true;
            VLOG("[fs] system archive file %s\n", host.c_str());
        } else if (a->romfs) {
            // binary path: [type, ...]; type 0 = the RomFS
            open_romfs();
            fs->romfs = true;
            fs->base = 0;
            fs->size = romfs_size;
            if (p.data.size() >= 4 && p.word(0) == 2) {
                // an ExeFS file by name ("icon", "banner", "logo")
                char nm[9] = {};
                memcpy(nm, p.data.data() + 4, std::min<size_t>(8, p.data.size() - 4));
                std::string name = nm;
                if (!name.empty() && name[0] == '.') name = name.substr(1);
                fs->romfs = false;
                fs->gamedata = true;
                fs->path = g_game.dir + EXEFS_PREFIX + name + ".bin";
                fs->fd = open(fs->path.c_str(), O_RDONLY);
                VLOG("[fs] SelfNCCH ExeFS %s -> %s\n", nm, fs->fd >= 0 ? "ok" : "missing");
                if (fs->fd < 0) return ERR_NOT_FOUND;
            } else if (p.data.size() >= 4 && p.word(0) == 5) {
                // an update's own RomFS (builder/dumps.py), which the game lays over its base RomFS itself
                fs->romfs = false;
                fs->gamedata = true;
                fs->path = g_game.dir + "/update_romfs.bin";
                fs->fd = open(fs->path.c_str(), O_RDONLY);
                VLOG("[fs] SelfNCCH update RomFS -> %s\n", fs->fd >= 0 ? "ok" : "missing");
                if (fs->fd < 0) return ERR_NOT_FOUND;
            } else if (p.data.size() >= 4 && p.word(0) != 0) {
                LOG("[fs] SelfNCCH open of non-RomFS section %u\n", p.word(0));
                return ERR_NOT_FOUND;
            }
        } else {
            std::string path = a->host + sanitize(p.str());
            fs->path = path;
            int oflags = (flags & 2) ? O_RDWR : O_RDONLY;
            if (flags & 4) oflags |= O_CREAT;
            if (fsys::is_directory(path)) return fs_err(250, 7, 28);
            fs->open_flags = oflags & O_RDWR;
            fs->fd = open(path.c_str(), oflags, 0644);
            if (fs->fd < 0) {
                VLOG("[fs] open %s failed\n", path.c_str());
                return fsys::exists(fsys::path(path).parent_path()) ? ERR_FILE_NOT_FOUND : ERR_PATH_NOT_FOUND;
            }
            VLOG("[fs] open %s\n", path.c_str());
#ifdef __WIIU__
            // the game's own saves are held in memory from here on (save_storage.cpp)
            if (!path.compare(0, g_save_base.size(), g_save_base)) {
                fs->held = wiiu::save_file_open(path, fs->fd);
                fs->direct_save = !fs->held;
                if (fs->held) {
                    close(fs->fd);
                    fs->fd = -1;
                }
            }
#endif
        }
        auto ses = std::make_shared<Session>();
        ses->service = fs;
        *out = ses;
        return 0;
    }

    void handle(IPC &ipc) override {
        if (ipc.cmd() == 0x080E) {  // CloseArchive only changes the kernel-owned handle table.
            archives.erase((u64)ipc[1] | ((u64)ipc[2] << 32));
            ipc.ok();
            return;
        }
        if (ipc.cmd() == 0x0802) {  // OpenFile: snapshot archive state before storage can block.
            Archive *a = arch(ipc[2], ipc[3]);
            if (!a) { ipc.result(ERR_ARCHIVE); return; }
            Archive archive = *a;
            const LowPath path = read_path(ipc[4], ipc[5], ipc[9]);
            const u32 flags = ipc[6];
            ObjPtr file;
            u32 result;
            run_blocked(*ipc.lk, [&] {
                FsIo io(0x3802);
                result = open_file(&archive, path, flags, &file);
                if (result) file.reset();
            });
            ipc.result(result, 1, 2);
            ipc.handles(2, {file}, true);
            return;
        }
        FsIo io(0x3000 | ipc.cmd());
        switch (ipc.cmd()) {
        case 0x0801:  // Initialize
        case 0x0861:  // InitializeWithSdkVersion
        case 0x0862:  // SetPriority
            ipc.ok();
            break;
        case 0x0863:
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x080C: {  // OpenArchive(id, path type, path size, static desc, ptr)
            LowPath p = read_path(ipc[2], ipc[3], ipc[5]);
            u64 h = 0;
            u32 r = open_archive(ipc[1], p, &h);
            ipc.result(r, 3);
            ipc.set64(2, h);
            break;
        }
        case 0x080D:  // ControlArchive(archive u64, action, ...)
            if (g_verbose) LOG("[fs] control archive action %u\n", ipc[3]);
            ipc.ok();
            break;
        case 0x0803: {  // OpenFileDirectly
            LowPath ap = read_path(ipc[3], ipc[4], ipc[10]);
            LowPath fp = read_path(ipc[5], ipc[6], ipc[12]);
            u64 h;
            u32 r = open_archive(ipc[2], ap, &h);
            ObjPtr f;
            if (!r) {
                r = open_file(&archives[h], fp, ipc[7], &f);
                archives.erase(h);
            }
            ipc.result(r, 1, 2);
            ipc.handles(2, {f}, true);
            break;
        }
        case 0x0804: {  // DeleteFile(transaction, archive, path type, size, desc, ptr)
            Archive *a = arch(ipc[2], ipc[3]);
            if (!a) { ipc.result(ERR_ARCHIVE); break; }
            std::string path = a->host + sanitize(read_path(ipc[4], ipc[5], ipc[7]).str());
            if (!SAVE_SETTLE(path, false)) { ipc.result(ERR_IO); break; }
            std::error_code ec;
            const u32 result = delete_host_file(path, ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                LOG("[fs] DeleteFile failed archive=%u path=%s error=%d (%s) result=%08x\n",
                    a->id, path.c_str(), ec.value(), ec.message().c_str(), result);
            ipc.result(result);
            if (!result) {
                save_changed(path);
                if (!SAVE_SETTLE(path, true)) ipc.result(ERR_IO);
            }
            break;
        }
        case 0x0805: {  // RenameFile(transaction, src archive, src type, src size, dst archive, dst type, dst size, ...)
            Archive *a = arch(ipc[2], ipc[3]);
            Archive *b = arch(ipc[6], ipc[7]);
            if (!a || !b) { ipc.result(ERR_ARCHIVE); break; }
            std::string s = a->host + sanitize(read_path(ipc[4], ipc[5], ipc[11]).str());
            std::string d = b->host + sanitize(read_path(ipc[8], ipc[9], ipc[13]).str());
            if (!SAVE_SETTLE(s, true) || !SAVE_SETTLE(d, true)) { ipc.result(ERR_IO); break; }
            std::error_code ec;
            fsys::rename(s, d, ec);
            ipc.result(ec ? ERR_FILE_NOT_FOUND : 0);
            save_changed(d);
            break;
        }
        case 0x0806:    // DeleteDirectory
        case 0x0807: {  // DeleteDirectoryRecursively
            Archive *a = arch(ipc[2], ipc[3]);
            if (!a) { ipc.result(ERR_ARCHIVE); break; }
            std::string path = a->host + sanitize(read_path(ipc[4], ipc[5], ipc[7]).str());
            if (!SAVE_SETTLE(path, true)) { ipc.result(ERR_IO); break; }
            std::error_code ec;
            if (ipc.cmd() == 0x0807) fsys::remove_all(path, ec);
            else fsys::remove(path, ec);
            ipc.result(ec ? ERR_PATH_NOT_FOUND : 0);
            save_changed(path);
            break;
        }
        case 0x0808: {  // CreateFile(transaction, archive, path type, size, attrs, filesize u64, desc, ptr)
            Archive *a = arch(ipc[2], ipc[3]);
            if (!a) { ipc.result(ERR_ARCHIVE); break; }
            std::string path = a->host + sanitize(read_path(ipc[4], ipc[5], ipc[10]).str());
            u64 size = (u64)ipc[7] | ((u64)ipc[8] << 32);
            if (fsys::exists(path)) { ipc.result(ERR_FILE_EXISTS); break; }
            if (!fsys::exists(fsys::path(path).parent_path())) { ipc.result(ERR_PATH_NOT_FOUND); break; }
            int fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
            if (fd >= 0) {
                save_resize(fd, size);
                close(fd);
            }
            VLOG("[fs] CreateFile %s size %llx\n", path.c_str(), (unsigned long long)size);
            save_changed(path);
            ipc.result(fd >= 0 ? 0 : ERR_PATH_NOT_FOUND);
            break;
        }
        case 0x0809: {  // CreateDirectory(transaction, archive, type, size, attrs, desc, ptr)
            Archive *a = arch(ipc[2], ipc[3]);
            if (!a) { ipc.result(ERR_ARCHIVE); break; }
            std::string path = a->host + sanitize(read_path(ipc[4], ipc[5], ipc[8]).str());
            if (fsys::exists(path)) { ipc.result(ERR_DIR_EXISTS); break; }
            std::error_code ec;
            fsys::create_directory(path, ec);
            ipc.result(ec ? ERR_PATH_NOT_FOUND : 0);
            save_changed(path);
            break;
        }
        case 0x080A: {  // RenameDirectory
            Archive *a = arch(ipc[2], ipc[3]);
            Archive *b = arch(ipc[6], ipc[7]);
            if (!a || !b) { ipc.result(ERR_ARCHIVE); break; }
            std::string s = a->host + sanitize(read_path(ipc[4], ipc[5], ipc[11]).str());
            std::string d = b->host + sanitize(read_path(ipc[8], ipc[9], ipc[13]).str());
            if (!SAVE_SETTLE(s, true) || !SAVE_SETTLE(d, true)) { ipc.result(ERR_IO); break; }
            std::error_code ec;
            fsys::rename(s, d, ec);
            ipc.result(ec ? ERR_PATH_NOT_FOUND : 0);
            save_changed(d);
            break;
        }
        case 0x080B: {  // OpenDirectory(archive u64, type, size, desc, ptr)
            Archive *a = arch(ipc[1], ipc[2]);
            if (!a || a->romfs) { ipc.result(ERR_ARCHIVE); break; }
            if (a->id == 10) { ipc.result(ERR_UNSUPPORTED, 1, 2); ipc.handles(2, {nullptr}, true); break; }
            std::string path = a->host + sanitize(read_path(ipc[3], ipc[4], ipc[6]).str());
            if (!fsys::is_directory(path)) { VLOG("[fs] OpenDirectory %s: not found\n", path.c_str()); ipc.result(ERR_PATH_NOT_FOUND, 1, 2); ipc.handles(2, {nullptr}, true); break; }
            // The sizes it lists come from storage, which must contain the cached changes.
            if (!SAVE_SETTLE(path, false)) { ipc.result(ERR_IO, 1, 2); ipc.handles(2, {nullptr}, true); break; }
            auto ds = std::make_shared<DirSession>();
            for (auto &e : fsys::directory_iterator(path)) ds->entries.push_back(e);
            VLOG("[fs] OpenDirectory %s: %zu entries\n", path.c_str(), ds->entries.size());
            auto ses = std::make_shared<Session>();
            ses->service = ds;
            ipc.ok(1, 2);
            ipc.handles(2, {ses}, true);
            break;
        }
        case 0x080F:    // FormatThisUserSaveData
        case 0x084C: {  // FormatSaveData
            std::string d = save_root() + "/savedata";
            std::error_code ec;
            // size in 512-byte blocks, directories, files, two bucket counts, duplicate data: after the archive's path in FormatSaveData
            const int at = ipc.cmd() == 0x084C ? 4 : 1;
            FS_NOTE("save: formatting");
            if (!SAVE_SETTLE(d, true)) { ipc.result(ERR_IO); break; }
            fsys::remove_all(d, ec);
            if (ec) { ipc.result(ERR_IO); break; }
            const bool created = fsys::create_directories(d, ec);
            if (ec || !keep_format_info(d, {ipc[at] * 512, ipc[at + 1], ipc[at + 2], ipc[at + 5] & 0xff})) {
                if (created) fsys::remove(d, ec);
                ipc.result(ERR_IO);
                break;
            }
            FS_NOTE("save: formatted");
            LOG("[fs] formatted save data at %s\n", d.c_str());
            save_changed(d);
            ipc.ok();
            break;
        }
        case 0x0845: {  // GetFormatInfo(archive id, path type, size, desc, ptr)
            std::string d = archive_dir(ipc[1], read_path(ipc[2], ipc[3], ipc[5]));
            if (d.empty() || !fsys::exists(d)) {
                ipc.result(ERR_NOT_FORMATTED, 5);
                break;
            }
            const FormatInfo info = format_info(d);
            ipc.ok(5);
            ipc.set(2, info.bytes);
            ipc.set(3, info.dirs);
            ipc.set(4, info.files);
            ipc.set(5, info.duplicate);
            break;
        }
        case 0x0830:    // legacy CreateExtSaveData(media, save id u64, icon size, directories, files, [icon])
        case 0x0835:    // legacy DeleteExtSaveData(media, save id low)
        case 0x0851:    // CreateExtSaveData(info: media type, save id u64, unknown; directories, files, size limit u64, icon size, [icon])
        case 0x0852: {  // DeleteExtSaveData(info)
            const bool legacy_create = ipc.cmd() == 0x0830;
            const bool deleting = ipc.cmd() == 0x0835 || ipc.cmd() == 0x0852;
            char b[64];
            snprintf(b, sizeof b, "/extdata/%08x%08x", ipc.cmd() == 0x0835 ? 0u : ipc[3], ipc[2]);
            const std::string d = save_root() + b;
            std::error_code ec;
            if (deleting) {  // what a game does before it makes its extra data anew ("initialize")
                if (!SAVE_SETTLE(d, true)) { ipc.result(ERR_IO); break; }
                fsys::remove_all(d, ec);
                if (!ec) fsys::remove(d + ".format", ec);
                if (ec) { ipc.result(ERR_IO); break; }
            } else {
                // The counts are asked back (GetFormatInfo), with no size, as for a save: answered with a save's defaults, Pokemon Ultra Sun
                // called its extra data corrupted at every start after the first (2026-11-13).
                const bool created = fsys::create_directories(d, ec);
                if (ec || !keep_format_info(d, {legacy_create ? ~0u : 0u, ipc[5], ipc[6], 0})) {
                    if (created) fsys::remove(d, ec);
                    ipc.result(ERR_IO);
                    break;
                }
            }
            save_changed(d);
            ipc.ok(1, legacy_create ? 2 : 0);
            if (legacy_create) {
                ipc.set(2, ipc[7]);
                ipc.set(3, ipc[8]);
            }
            break;
        }
        case 0x0812:  // GetFreeBytes
            ipc.ok(3);
            ipc.set64(2, 0x10000000ull);
            break;
        case 0x0813:  // GetCardType
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0814:    // GetSdmcArchiveResource
        case 0x0815:    // GetNandArchiveResource
        case 0x0849: {  // GetArchiveResource
            ipc.ok(5);
            ipc.set(2, 0x200);      // sector size
            ipc.set(3, 0x20000);    // cluster size
            ipc.set(4, 0x80000);    // partition capacity in clusters
            ipc.set(5, 0x80000);    // free clusters
            break;
        }
        case 0x0817:  // IsSdmcDetected
        case 0x0818:  // IsSdmcWritable
        case 0x0821:  // CardSlotIsInserted
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        case 0x082E:  // GetProductInfo(process id)
            ipc.ok(6);
            memset(gptr(ipc.buf + 8), 0, 16);
            break;
        case 0x082F:  // GetProgramLaunchInfo
            ipc.ok(5);
            ipc.set64(2, g_game.program_id);
            ipc.set(4, 2);  // game card
            ipc.set(5, 0);
            break;
        case 0x086E:  // SetThisSaveDataSecureValue
        case 0x0867:  // ControlSecureSave
            ipc.ok();
            break;
        case 0x086F:  // GetThisSaveDataSecureValue
            ipc.ok(5);
            ipc.set(2, 0);  // exists
            ipc.set(3, 0);
            ipc.set64(4, 0);
            break;
        default:
            unimplemented(ipc);
        }
    }
};

void register_fs() { register_service(std::make_shared<FsUser>("fs:USER")); }


#ifdef RECOMP_RESUMABLE
void checkpoint_fs(checkpoint::Image &im) {
    im.take(archives); im.take(next_archive);
    const auto epoch = checkpoint_write_epoch;
    im.validate.push_back([epoch] {
        if (checkpoint_write_epoch == epoch) return true;
        LOG("[state] validation failed: permanent-save epoch %llu -> %llu\n", (unsigned long long)epoch, (unsigned long long)checkpoint_write_epoch);
        return false;
    });
}
#endif

}  // namespace kernel

#ifdef RECOMP_RESUMABLE
namespace kernel {
std::shared_ptr<Service> persistent_file_service(const std::string &name) {
    if (name == "fs file") return std::make_shared<FileSession>();
    if (name == "fs dir") return std::make_shared<DirSession>();
    return nullptr;
}
}
#endif
