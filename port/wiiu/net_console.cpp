// A way into the game on the console from the PC, over the network: tools/console_watch.py connects to port 4405, gets the log as it's written and can ask what every
// thread in the process is doing. Nothing here goes near a file unless asked to test it, and the network kept working on a console frozen in the file system.
// The socket library comes from OSDynLoad, not an import: one less thing for the system's loader to set up, and without it the game just goes on.
// A test run ("test [flags]") restarts the game once on a blank save of its own, so that nothing the PC plays can reach the player's: tools/console_tour.py.
#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/memorymap.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "rt.h"
#include "checkpoint.h"
#include "wiiu.h"

namespace kernel {
void live_feed(const char *line);  // services/hid.cpp
void diagnostics_report();
void set_diagnostics_thread(u32 thread);
void dump_threads();
}

u32 rt_log_read(u64 &pos, char *out, u32 max);  // core.cpp

namespace {
constexpr u16 PORT = 4405;
constexpr int NS_EWOULDBLOCK = 6, NS_ECONNRESET = 8, NS_ENOTCONN = 16, NS_SOL_SOCKET = -1, NS_SO_REUSEADDR = 0x0004, NS_SO_NONBLOCK = 0x1016, NS_MSG_DONTWAIT = 0x0020;

struct SockAddr {
    u16 family, port;
    u32 addr;
    u8 zero[8];
};

int (*n_lib_init)();
int (*n_socket)(int, int, int);
int (*n_setsockopt)(int, int, int, const void *, u32);
int (*n_bind)(int, const SockAddr *, u32);
int (*n_listen)(int, int);
int (*n_accept)(int, SockAddr *, u32 *);
int (*n_send)(int, const void *, int, int);
int (*n_recv)(int, void *, int, int);
int (*n_close)(int);
int (*n_lasterr)();

volatile bool client_on;

bool load_sockets() {
    OSDynLoad_Module m;
    if (OSDynLoad_Acquire("nsysnet.rpl", &m) != OS_DYNLOAD_OK) return false;
    const struct {
        const char *name;
        void **where;
    } wanted[] = {{"socket_lib_init", (void **)&n_lib_init}, {"socket", (void **)&n_socket},   {"setsockopt", (void **)&n_setsockopt},
                  {"bind", (void **)&n_bind},                {"listen", (void **)&n_listen},   {"accept", (void **)&n_accept},
                  {"send", (void **)&n_send},                {"recv", (void **)&n_recv},       {"socketclose", (void **)&n_close},
                  {"socketlasterr", (void **)&n_lasterr}};
    for (const auto &w : wanted)
        if (OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, w.name, w.where) != OS_DYNLOAD_OK || !*w.where) return false;
    return true;
}

bool mapped(u32 a) { return OSIsAddressValid(a) || OSEffectiveToPhysical(a); }  // Cemu only answers the second
bool valid(const void *p) { return p && mapped((u32)p); }

// Read-only debugger snapshots. Scheduler-owned pointers can change during the
// validity check itself: never validate one read then dereference a fresh read.
bool valid_span(const void *p, u32 length) {
    const u32 at = (u32)p;
    if (!at || !length || length - 1 > UINT32_MAX - at) return false;
    if (!mapped(at) || !mapped(at + length - 1)) return false;
    for (u32 offset = 4096 - (at & 4095); offset < length; offset += 4096)
        if (!mapped(at + offset)) return false;
    return true;
}
bool copy_thread(const OSThread *live, OSThread &snapshot) {
    if (!valid_span(live, sizeof snapshot)) return false;
    memcpy(&snapshot, live, sizeof snapshot);
    return snapshot.tag == OS_THREAD_TAG;
}
void copy_thread_name(const char *name, char (&out)[64]) {
    strcpy(out, "(no name)");
    if (!name || (u32)name > UINT32_MAX - 63) return;
    for (unsigned i = 0; i < 63; ++i) {
        if (!mapped((u32)name + i)) { out[i] = 0; return; }
        const char c = name[i];
        out[i] = c;
        if (!c) return;
    }
    out[63] = 0;
}
// Snapshots are best effort, not a globally consistent scheduler freeze. Copy
// the thread before inspecting fields, then cache every optional nested pointer.
void dump_thread(const OSThread *live, const OSThread &t) {
    char name[64], wait[80] = "";
    copy_thread_name(t.name, name);
    const OSMutex *const mutex = t.mutex;
    const OSThreadQueue *const queue = t.queue;
    if (valid_span(mutex, sizeof *mutex)) {
        const OSThread *const owner = mutex->owner;
        snprintf(wait, sizeof wait, "mutex %08x owner=%08x", (u32)mutex, (u32)owner);
    } else if (valid_span(queue, sizeof *queue)) {
        const void *const parent = queue->parent;
        const u32 tag = valid_span(parent, sizeof(u32)) ? *(const u32 *)parent : 0;
        snprintf(wait, sizeof wait, "queue %08x tag %08x", (u32)queue, tag);
    }
    char frames[220];
    int used = 0;
    const u32 lo = (u32)t.stackEnd, hi = (u32)t.stackStart;
    u32 sp = t.context.gpr[1];
    for (int i = 0; i < 18 && sp >= lo && hi >= 8 && sp <= hi - 8 && !(sp & 3); ++i) {
        if (!valid_span((const void *)sp, 8)) break;
        u32 frame[2];
        memcpy(frame, (const void *)sp, sizeof frame);
        used += snprintf(frames + used, sizeof frames - used, " %08x", frame[1]);
        if (frame[0] <= sp) break;
        sp = frame[0];
    }
    frames[used] = 0;
    LOG("[ps] thread=%08x %s id %u attr %x state %x prio %d/%d susp %d ran %u ms woke %u pc %08x lr %08x sp %08x spin=%u cancel=%x [%s] |%s\n", (u32)live, name, t.id, (u32)t.attr, (u32)t.state,
        t.priority, t.basePriority, t.suspendCounter, (u32)(t.coreTimeConsumedNs / 1000000), (u32)t.wakeCount, t.context.srr0, t.context.lr, t.context.gpr[1], t.context.spinLockCount, (u32)t.cancelState, wait, frames);
}

// ---- file system tests from the PC, each on its own thread so a stuck one costs only itself
constexpr u32 PROBES = 6, PROBE_STACK = 64 << 10;
struct Probe {
    OSThread thread;
    char path[128];
    u32 core;
    bool write;
    volatile bool busy;
};
Probe *probes;
u8 *probe_stacks;

u32 ms_since(OSTime t) { return (u32)OSTicksToMilliseconds(OSGetSystemTime() - t); }

// What saves rely on that the console's files don't do by themselves (save_storage.cpp): writes past the end, resizing both ways, and writes straight after a resize. Returns what's wrong.
const char *check_save_file(const char *name) {
    const int fd = open(name, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return "can't open";
    const auto holds = [&](u64 size, u64 at, const char *want) {
        struct stat st {};
        char got[4] = {};
        return !fstat(fd, &st) && (u64)st.st_size == size && pread(fd, got, 4, (off_t)at) == 4 && !memcmp(got, want, 4);
    };
    const char *wrong = nullptr, zeros[4] = {};
    wiiu::save_write(fd, "abcd", 4, 0x20000);
    if (!holds(0x20004, 0x20000, "abcd") || !holds(0x20004, 0x100, zeros)) wrong = "a write past the end";
    wiiu::save_resize(fd, 0x30000);
    if (!wrong && (!holds(0x30000, 0x20000, "abcd") || !holds(0x30000, 0x2FFFC, zeros))) wrong = "growing";
    wiiu::save_resize(fd, 0x20002);
    wiiu::save_write(fd, "wxyz", 4, 0x20002);
    if (!wrong && (!holds(0x20006, 0x20002, "wxyz") || !holds(0x20006, 0x20000, "abwx"))) wrong = "shrinking";
    close(fd);
    return wrong;
}

int probe_entry(int i, void *) {
    Probe &p = probes[i];
    OSTime t = OSGetSystemTime();
    if (p.write) {
        // only ever this one file, and it's removed again
        std::string name = wiiu::data_dir() + "/net_probe.bin";
        const char *wrong = check_save_file(name.c_str());
        remove(name.c_str());
        LOG("[fs] save file checks: %s, %u ms\n", wrong ? wrong : "all good", ms_since(t));
        t = OSGetSystemTime();
        static u8 *const zeros = (u8 *)calloc(1, 32 << 10);
        LOG("[fs] core %u write test starts\n", p.core);
        FILE *f = fopen(name.c_str(), "wb");
        LOG("[fs] open for writing: %s, %u ms\n", f ? "ok" : "failed", ms_since(t));
        if (f) {
            for (int n = 0; n < 8; n++) {
                t = OSGetSystemTime();
                const size_t w = fwrite(zeros, 1, 32 << 10, f);
                fflush(f);
                LOG("[fs] wrote %u, %u ms\n", (u32)w, ms_since(t));
            }
            t = OSGetSystemTime();
            fclose(f);
            LOG("[fs] closed, %u ms\n", ms_since(t));
            t = OSGetSystemTime();
            remove(name.c_str());
            LOG("[fs] removed, %u ms\n", ms_since(t));
        }
    } else {
        LOG("[fs] core %u looks at %s\n", p.core, p.path);
        struct stat st {};
        const int r = stat(p.path, &st);
        LOG("[fs] stat: %d, %u bytes, %u ms\n", r, (u32)st.st_size, ms_since(t));
        t = OSGetSystemTime();
        if (!r && S_ISDIR(st.st_mode)) {
            if (DIR *d = opendir(p.path)) {
                LOG("[fs] opendir: %u ms\n", ms_since(t));
                for (int n = 0; n < 4; n++)
                    if (const dirent *e = readdir(d)) LOG("[fs]   %s, %u ms\n", e->d_name, ms_since(t));
                closedir(d);
            }
        } else if (!r) {
            if (FILE *f = fopen(p.path, "rb")) {
                LOG("[fs] open: %u ms\n", ms_since(t));
                t = OSGetSystemTime();
                static u8 *const buf = (u8 *)memalign(64, 64 << 10);
                u32 total = 0;
                for (int n = 0; n < 16; n++) total += (u32)fread(buf, 1, 64 << 10, f);
                LOG("[fs] read %u bytes, %u ms\n", total, ms_since(t));
                fclose(f);
            }
        }
    }
    LOG("[fs] finished\n");
    p.busy = false;
    return 0;
}

void start_probe(u32 core, const char *path, bool write) {
    for (u32 i = 0; i < PROBES; i++) {
        Probe &p = probes[i];
        if (p.busy || core > 2) continue;
        p.busy = true;
        p.core = core;
        p.write = write;
        snprintf(p.path, sizeof p.path, "fs:%s", path);
        OSCreateThread(&p.thread, probe_entry, i, nullptr, probe_stacks + (i + 1) * PROBE_STACK, PROBE_STACK, 12, (OSThreadAttributes)(1 << core));
        OSSetThreadName(&p.thread, "net probe");
        OSResumeThread(&p.thread);
        return;
    }
    LOG("[fs] no free probe\n");
}

bool send_all(int c, const char *p, u32 n) {
    for (u32 tries = 0; n;) {
        const int sent = n_send(c, p, (int)n, 0);
        if (sent > 0) {
            p += sent;
            n -= sent;
            tries = 0;
            continue;
        }
        if (sent == 0 || n_lasterr() != NS_EWOULDBLOCK || ++tries > 2000) return false;
        OSSleepTicks(OSMillisecondsToTicks(5));
    }
    return true;
}

// The shader misses go to the PC as hex, so that nothing needs the card for them
bool send_misses(int c) {
    std::vector<u8> out;
    wiiu::gfx_take_misses(out);
    static char line[8 + 1024 + 2];
    for (size_t at = 0; at < out.size(); at += 512) {
        const size_t n = std::min<size_t>(512, out.size() - at);
        char *w = line + sprintf(line, "MISS ");
        for (size_t i = 0; i < n; i++) {
            *w++ = "0123456789abcdef"[out[at + i] >> 4];
            *w++ = "0123456789abcdef"[out[at + i] & 15];
        }
        *w++ = '\n';
        if (!send_all(c, line, (u32)(w - line))) return false;
    }
    return true;
}

// a picture as SHOTB, SHOT <hex> lines and SHOTE: console_watch.py writes it out as a file
bool send_shot(int c, const std::vector<u8> &ppm) {
    static char line[8 + 2 * 2048];
    bool ok = send_all(c, "SHOTB\n", 6);
    for (size_t at = 0; ok && at < ppm.size(); at += 2048) {
        const size_t n = std::min<size_t>(2048, ppm.size() - at);
        char *w = line + sprintf(line, "SHOT ");
        for (size_t i = 0; i < n; i++) {
            *w++ = "0123456789abcdef"[ppm[at + i] >> 4];
            *w++ = "0123456789abcdef"[ppm[at + i] & 15];
        }
        *w++ = '\n';
        ok = send_all(c, line, (u32)(w - line));
    }
    return ok && send_all(c, "SHOTE\n", 6);
}

const char *const TEST_MARKER = "/test_run_once", *const TEST_FOLDER = "/test_run";
bool in_test;
std::string test_flags;

bool run_command(int c, char *line) {
    char word[16] = "", path[128] = "", name[16] = "";
    u32 a = 0, b = 0, value = 0;
    char trailing = 0;
    unsigned long long title = 0;
    sscanf(line, "%15s", word);
    if (!strcmp(word, "ps")) {
        wiiu::log_threads();
    } else if (!strcmp(word, "diag")) {
        kernel::diagnostics_report();
        kernel::dump_threads();
        wiiu::log_threads();
        wiiu::gfx_report();
    } else if (!strcmp(word, "ram")) {
#ifdef RECOMP_RESUMABLE
        if (sscanf(line,"ram read %x %c",&a,&trailing)==1) {
            LOG("[net-ram] read %08x: %s\n",a,checkpoint::request_memory32(a,false) ? "queued" : "unaligned or busy");
        } else if (sscanf(line,"ram write %x %x %x %c",&a,&b,&value,&trailing)==3) {
            LOG("[net-ram] compare/write %08x: %s\n",a,checkpoint::request_memory32(a,true,b,value) ? "queued" : "unaligned or busy");
        } else LOG("[net-ram] use read <guest-address> or write <guest-address> <expected-u32> <new-u32>\n");
#else
        LOG("[net-ram] unavailable: this executable lacks checkpoint coordination\n");
#endif
    } else if (!strcmp(word, "state")) {
#ifdef RECOMP_RESUMABLE
        // Queue on the same GPU-thread checkpoint pump used by the menu.
        // Acknowledgement is not completion; state status/logs report that.
        if (!strcmp(line, "state save")) { checkpoint::request(4); LOG("[net-state] persistent save queued\n"); }
        else if (!strcmp(line, "state save-ram")) { checkpoint::request(1); LOG("[net-state] RAM save queued\n"); }
        else if (!strcmp(line, "state load")) { checkpoint::request(2); LOG("[net-state] automatic load queued\n"); }
        else if (!strcmp(line, "state load-file")) { checkpoint::request(5); LOG("[net-state] file load queued\n"); }
        else if (!strcmp(line, "state status")) {
            const char *message = checkpoint::result_message();
            LOG("[net-state] available %u writing %u; %s\n", (u32)checkpoint::has_state(), (u32)checkpoint::saving_to_disk(), message ? message : "no completed operation");
        } else LOG("[net-state] use save, save-ram, load, load-file, or status\n");
#else
        LOG("[net-state] unavailable: this executable lacks checkpoint execution\n");
#endif
    } else if (!strcmp(word, "stats")) {
        wiiu::gfx_report();  // counters and any enabled GPU timestamps, published between frames
    } else if (!strcmp(word, "st")) {
        wiiu::log_status();
    } else if (!strcmp(word, "misses")) {
        return send_misses(c);
    } else if (!strcmp(word, "quit")) {  // to the Wii U Menu, the way HOME's Close goes
        LOG("[net] quit: leaving for the Wii U Menu\n");
        SYSLaunchMenu();
    } else if (!strcmp(word, "relaunch")) {
        LOG("[net] relaunch\n");
        SYSRelaunchTitle(0, nullptr);
    } else if (sscanf(line, "launch %llx", &title) == 1) {  // another installed title, this one quitting cleanly first
        LOG("[net] launch %016llx\n", title);
        SYSLaunchTitle(title);
    } else if (!strncmp(line, "in ", 3)) {  // an input command as in services/hid.cpp, played after the ones before it
        kernel::live_feed(line + 3);
    } else if (!strcmp(word, "test")) {  // "test [flags]": once, on a save of its own
        if (FILE *f = fopen((wiiu::data_dir() + TEST_MARKER).c_str(), "wb")) {
            fprintf(f, "%llx %s", (unsigned long long)OSGetTime(), line[4] ? line + 5 : "");  // when, so that one the restart never used goes stale
            fclose(f);
            wiiu::save_flush(true);
            LOG("[net] restarting for a test run\n");
            SYSRelaunchTitle(0, nullptr);
        }
    } else if (sscanf(line, "set %15s %u", name, &a) == 2) {  // a renderer switch, from now on: uberall, uberfb, cpuvs 0|1, trace <draws>
        bool accepted;
        if (!strcmp(name, "avtime") && a <= 1) { wiiu::set_av_timing(a != 0); accepted = true; }
        else if (!strcmp(name, "diag") && a <= 3) { wiiu::set_diagnostics_level(a); accepted = true; }
        else if (!strcmp(name, "diagthread") && a <= 65535) { kernel::set_diagnostics_thread(a); accepted = true; }
        else if (!strcmp(name, "bulkcopy") && a <= 1) { __atomic_store_n(&rt_bulk_copy_enabled, a, __ATOMIC_RELAXED); accepted = true; }
        else if (!strcmp(name, "videoworker") && a <= 1) { wiiu::set_video_worker(a != 0); accepted = true; }
        else {
            accepted = wiiu::gfx_set(name, a);
            // Existing recorder clients can request the expanded CPU timing
            // sample without an application restart or another TCP connection.
            if (accepted && !strcmp(name, "cputime")) wiiu::set_av_timing(a != 0);
        }
        LOG("[net] set %s %u: %s\n", name, a, accepted ? "done" : "no such switch");
    } else if (sscanf(line, "keep %15s %u", name, &a) == 2) {  // a debug file in the data folder, made or removed, for every boot from the next one on: no reinstall needed
        const std::string kept = wiiu::data_dir() + "/" + name;
        if (!a) remove(kept.c_str());
        else if (FILE *f = fopen(kept.c_str(), "wb")) fclose(f);
        wiiu::save_flush(true);
        LOG("[net] keep %s %u\n", name, a);
    } else if (!strcmp(word, "prof")) {  // where each core has been, if this boot profiles ("test profile")
        wiiu::profile_send();
    } else if (!strcmp(word, "list")) {
        wiiu::gfx_list();
    } else if (sscanf(line, "surf %x %u", &b, &a) >= 1) {  // a picture of the target or texture at this guest address: "surf <hex address> [step]"
        std::vector<u8> ppm;
        if (wiiu::tv_shot(a ? a : 1, ppm, b) && !ppm.empty()) return send_shot(c, ppm);
        LOG("[net] surf %08x: nothing there\n", b);
    } else if (sscanf(line, "shot %u", &a) == 1 || !strcmp(word, "shot")) {
        std::vector<u8> ppm;
        if (wiiu::tv_shot(a ? a : 3, ppm)) return send_shot(c, ppm);
        LOG("[net] shot: no present came\n");
    } else if (sscanf(line, "fs %u %127s", &a, path) == 2) {
        start_probe(a, path, false);
    } else if (sscanf(line, "fw %u", &a) == 1) {
        start_probe(a, "", true);
    } else if (sscanf(line, "sym %x", &a) == 1) {
        char name[96] = "";
        OSGetSymbolName(a, name, sizeof name);
        LOG("[sym] %08x %s\n", a, name);
    } else if (sscanf(line, "mem %x %x", &a, &b) == 2) {
        for (u32 at = a & ~3u; at < a + b && at < a + 4096; at += 32) {
            if (!mapped(at) || !mapped(at + 28)) {
                LOG("[mem] %08x not mapped\n", at);
                break;
            }
            const u32 *w = (const u32 *)at;
            LOG("[mem] %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", at, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    return true;
}

void serve(int c) {
    u64 pos = 0;  // from the oldest line still in memory
    static char out[8192];
    char in[256];
    u32 have = 0;
    OSTime heard = OSGetSystemTime();
    for (;;) {
        // whole lines go into the log at once, so a command's output never lands in the middle of one
        while (const u32 n = rt_log_read(pos, out, sizeof out))
            if (!send_all(c, out, n)) return;
        const int got = n_recv(c, in + have, sizeof in - 1 - have, NS_MSG_DONTWAIT);
        if (got == 0) return;
        if (got < 0) {
            const int err = n_lasterr();
            if (err == NS_ECONNRESET || err == NS_ENOTCONN) return;
            // the PC asks for something every few seconds, so silence means it's gone, and with it the only way anyone else gets in
            if (OSTicksToSeconds(OSGetSystemTime() - heard) > 30) return;
            OSSleepTicks(OSMillisecondsToTicks(50));
            continue;
        }
        heard = OSGetSystemTime();
        have += got;
        in[have] = 0;
        char *start = in;
        while (char *end = strchr(start, '\n')) {
            *end = 0;
            if (!run_command(c, start)) return;
            start = end + 1;
        }
        have = (u32)strlen(start);
        memmove(in, start, have + 1);
        if (have >= sizeof in - 1) have = 0;
    }
}
}  // namespace

namespace wiiu {
bool net_connected() { return client_on; }

// The marker is gone for good before the game starts, flushed, or a power cut after a freeze would bring it back and the player's next game would land on the test save.
// The next ordinary boot removes what a test run left.
std::string save_folder() {
    const std::string marker = data_dir() + TEST_MARKER, folder = data_dir() + TEST_FOLDER;
    // A marker the restart never used, because that restart hung (Tomodachi, 2026-11-07), is stale after 10 minutes: the player's next start of the game is theirs, not a test
    if (FILE *f = fopen(marker.c_str(), "rb")) {
        char text[160] = "";
        fread(text, 1, sizeof text - 1, f);
        fclose(f);
        remove(marker.c_str());
        unsigned long long when = 0;
        int used = 0;
        const bool fresh = sscanf(text, "%llx %n", &when, &used) == 1 && (u64)(OSGetTime() - (OSTime)when) < (u64)OSSecondsToTicks(600);
        if (fresh) {
            test_flags = text + used;
            in_test = true;
        }
        LOG("[test] a test marker, %s\n", fresh ? "used" : "stale, so not a test run");
    }
    std::error_code ec;
    const bool left = std::filesystem::exists(folder, ec);
    if (left) std::filesystem::remove_all(folder, ec);
    if (in_test || left) save_flush(true);
    if (in_test) return folder;
    const std::string common = data_dir() + "/save";
    // a game's own save is <program id>/savedata; the shared extdata a game makes at its first start doesn't count
    enum class SaveProbe { Missing, Present, Unreadable };
    auto has_save = [](const std::string &dir) {
        namespace fs = std::filesystem;
        std::error_code e;
        const auto root = fs::status(dir, e);
        if (root.type() == fs::file_type::not_found && (!e || e == std::errc::no_such_file_or_directory)) return SaveProbe::Missing;
        if (e || !fs::is_directory(root)) return SaveProbe::Unreadable;
        for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) {
            const bool directory = it->is_directory(e);
            if (e) break;
            if (!directory) continue;
            const auto save = it->path() / "savedata";
            const bool exists = fs::exists(save, e);
            if (e) break;
            if (!exists) continue;
            if (!fs::is_directory(save, e) || e) return SaveProbe::Unreadable;
            const bool empty = fs::is_empty(save, e);
            if (e) break;
            if (!empty) return SaveProbe::Present;
        }
        return e ? SaveProbe::Unreadable : SaveProbe::Missing;
    };
    const auto checked = [&](const std::string &dir) {
        const auto probe = has_save(dir);
        if (probe == SaveProbe::Unreadable) FATAL("[save] Cannot read the save directory at %s.\n", dir.c_str());
        return probe == SaveProbe::Present;
    };
    if (save_account_dir().empty()) {
        checked(common);
        return common;
    }
    // Copy older saves out of common storage only when the account has yet to make its own save.
    const std::string mine = save_account_dir() + "/save";
    if (checked(mine)) {
        save_finish_migration(mine);
    } else if (checked(common)) {
        const OSTime t = OSGetSystemTime();
        const bool copied = save_migrate_common(common, mine);
        LOG("[save] the save in common storage copied to the player's: %s, %u ms\n", copied ? "done" : "FAILED", (u32)OSTicksToMilliseconds(OSGetSystemTime() - t));
        if (!copied) return common;  // keep playing the intact legacy save until migration can succeed
    }
    return mine;
}

bool test_run() { return in_test; }

bool debug_flag(const char *name) {
    if (test_flags.find(name) != std::string::npos) return true;
    FILE *f = fopen((data_dir() + "/" + name).c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

void log_threads() {
    log_status();
    OSThread *t = OSGetCurrentThread();
    OSThread snapshot;
    for (u32 n = 0; n < 256; ++n) {
        if (!copy_thread(t, snapshot)) break;
        OSThread *const previous = snapshot.activeLink.prev;
        if (!previous || !valid_span(previous, sizeof snapshot)) break;
        t = previous;
    }
    for (u32 n = 0; n < 256; ++n) {
        if (!copy_thread(t, snapshot)) break;
        OSThread *const next = snapshot.activeLink.next;
        dump_thread(t, snapshot);
        t = next;
    }
    LOG("[ps] end\n");
}

int net_thread(int, void *) {
    if (!load_sockets()) {
        LOG("[net] no socket library\n");
        return 0;
    }
    probes = (Probe *)memalign(64, sizeof(Probe) * PROBES);
    probe_stacks = (u8 *)memalign(64, PROBE_STACK * PROBES);
    if (!probes || !probe_stacks) return 0;
    memset(probes, 0, sizeof(Probe) * PROBES);
    n_lib_init();
    int server;
    // the network can come up after the game does
    for (bool told = false;; OSSleepTicks(OSSecondsToTicks(1))) {
        server = n_socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 6 /* TCP */);
        if (server >= 0) {
            const int one = 1;
            n_setsockopt(server, NS_SOL_SOCKET, NS_SO_REUSEADDR, &one, sizeof one);
            n_setsockopt(server, NS_SOL_SOCKET, NS_SO_NONBLOCK, &one, sizeof one);  // Cemu can't do a blocking accept, and stops if asked to
            const SockAddr me{2, PORT, 0, {}};
            if (!n_bind(server, &me, sizeof me) && !n_listen(server, 1)) break;
            n_close(server);
        }
        if (!told) LOG("[net] waiting for the network: %d\n", n_lasterr());
        told = true;
    }
    LOG("[net] listening on port %u\n", PORT);
    for (;;) {
        SockAddr peer;
        u32 size = sizeof peer;
        const int c = n_accept(server, &peer, &size);
        if (c < 0) {
            OSSleepTicks(OSMillisecondsToTicks(200));
            continue;
        }
        client_on = true;
        LOG("[net] the PC is connected\n");
        serve(c);
        client_on = false;
        n_close(c);
        LOG("[net] the PC went away\n");
    }
    return 0;
}
}  // namespace wiiu
