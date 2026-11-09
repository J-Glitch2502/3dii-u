// Logging, guest memory reservation, and the address -> recompiled function dispatcher.
#ifdef __WIIU__
#include <coreinit/memdefaultheap.h>
#else
#include <sys/mman.h>
#include <dlfcn.h>
#endif
#include <unistd.h>

#include <TargetConditionals.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "kernel.h"
#include "loader.h"
#include "rt.h"

u8 *g_mem;
int g_verbose;
static std::mutex log_mutex;
static FILE *log_file;
void (*g_fatal_hook)(const char *msg);
void watch_report();

u64 g_frame;

static std::string log_path;

#ifdef __WIIU__
// On the console a line only goes into memory here. The PC reads it over the network (rt_log_read, port/wiiu/net_console.cpp), and rt_log_sync takes it to the card once, when the
// game ends. Threads that can't wait log too, holding locks the rest of the game needs (the GPU thread's surface lines come in bursts at every scene load), and the card can
// take a long time to answer, so nothing about logging touches it while the game runs, including at start-up.
static constexpr u32 LOG_RING = 1 << 18;
static char *log_ring, *log_copy;  // from the heap, not half a megabyte more for the system's loader to set up
static u64 log_in, log_out;
static std::mutex log_card_mutex;

static void log_put(const char *s, u32 n) {
    std::lock_guard<std::mutex> lk(log_mutex);
    if (!log_ring) {
        log_ring = (char *)malloc(LOG_RING * 2);
        if (!log_ring) return;
        log_copy = log_ring + LOG_RING;
    }
    for (u32 i = 0; i < n; i++) log_ring[((u32)log_in + i) & (LOG_RING - 1)] = s[i];
    log_in += n;
}

void rt_log_open(const std::string &path) { log_path = path; }

void rt_log_sync() {
    std::lock_guard<std::mutex> card(log_card_mutex);
    u32 n;
    {
        std::lock_guard<std::mutex> lk(log_mutex);
        if (!log_ring) return;
        if (log_in - log_out > LOG_RING) log_out = log_in - LOG_RING;  // the oldest lines are gone
        n = (u32)(log_in - log_out);
        for (u32 i = 0; i < n; i++) log_copy[i] = log_ring[((u32)log_out + i) & (LOG_RING - 1)];
        log_out = log_in;
    }
    if (!n || log_path.empty()) return;
    static bool started;
    if (!started) {
        started = true;
        remove((log_path + ".prev").c_str());
        rename(log_path.c_str(), (log_path + ".prev").c_str());
    }
    if (FILE *f = fopen(log_path.c_str(), "a")) {
        fwrite(log_copy, 1, n, f);
        fclose(f);
    }
}

// What's new since *pos, for a reader with its own place in the log
u32 rt_log_read(u64 &pos, char *out, u32 max) {
    std::lock_guard<std::mutex> lk(log_mutex);
    if (!log_ring) return 0;
    if (log_in - pos > LOG_RING) pos = log_in - LOG_RING;
    const u32 n = (u32)std::min<u64>(log_in - pos, max);
    for (u32 i = 0; i < n; i++) out[i] = log_ring[((u32)pos + i) & (LOG_RING - 1)];
    pos += n;
    return n;
}

// Debug aid ("logsync" in the data folder, frontend_wiiu.cpp): every line goes to the card as it's logged, so a crash that takes the whole
// program down (Cemu, which dies on a bad guest access) still leaves the lines that led up to it. Slow; never for play.
bool g_log_each_line;

void rt_log(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char line[2080];
    const int n = snprintf(line, sizeof line, "%6llu %s", (unsigned long long)g_frame, msg);
    log_put(line, std::min<u32>((u32)std::max(n, 0), sizeof line - 1));
    if (g_log_each_line) rt_log_sync();
}
#else
void rt_log_open(const std::string &path) {
    std::lock_guard<std::mutex> lk(log_mutex);
    rename(path.c_str(), (path + ".prev").c_str());
    log_file = fopen(path.c_str(), "w");
    log_path = path;
}

void rt_log_sync() {
    std::lock_guard<std::mutex> lk(log_mutex);
    if (!log_file) return;
    fclose(log_file);
    log_file = fopen(log_path.c_str(), "a");
}

void rt_log(const char *fmt, ...) {
    std::lock_guard<std::mutex> lk(log_mutex);
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%6llu %s", (unsigned long long)g_frame, msg);
    fflush(stderr);
    if (log_file) {
        fprintf(log_file, "%6llu %s", (unsigned long long)g_frame, msg);
        fflush(log_file);
    }
}
#endif

void rt_fatal(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
#ifdef __WIIU__
    rt_log("FATAL: %s", msg);
#else
    {
        std::lock_guard<std::mutex> lk(log_mutex);
        fprintf(stderr, "FATAL: %s", msg);
        fflush(stderr);
        if (log_file) {
            fprintf(log_file, "%6llu FATAL: %s", (unsigned long long)g_frame, msg);
            fflush(log_file);
        }
    }
#endif
    watch_report();  // (RECOMP_WATCH) who touched the watched page before the failure
    if (g_fatal_hook) g_fatal_hook(msg);
    _exit(1);
}

std::string read_cstr(u32 a, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)*GuestMemoryPointer(a + i);
        if (!ch) break;
        s += ch;
    }
    return s;
}

namespace mem {
static u32 hle_next = HLE_BASE;

void init() {
#ifdef __WIIU__
    // The old-3DS address map fits under 0x20000000, so a flat 512 MB block stands in for the 64-bit hosts' 4 GiB reservation.
    g_mem = (u8 *)MEMAllocFromDefaultHeapEx(0x20010000, 0x10000);
    if (!g_mem) FATAL("cannot allocate guest memory\n");
    memset(g_mem, 0, 0x20010000);
    return;
#endif
    // 4 GiB for the guest address space plus a guard so unaligned accesses at the top stay mapped
    size_t size = (1ull << 32) + 0x10000;
#ifndef __WIIU__
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        FATAL(TARGET_OS_IOS ? "cannot reserve guest memory (an iPhone or iPad needs the app signed with the Extended "
                              "Virtual Addressing capability)\n"
                            : "cannot reserve guest memory\n");
    g_mem = (u8 *)p;
#endif
}

u32 paddr_to_vaddr(u32 pa) {
    if (pa >= FCRAM_PADDR && pa < FCRAM_PADDR + LINEAR_SIZE) return pa - FCRAM_PADDR + LINEAR_BASE;
    if (pa >= VRAM_PADDR && pa < VRAM_PADDR + VRAM_SIZE) return pa - VRAM_PADDR + VRAM_BASE;
    if (pa >= DSP_PADDR && pa < DSP_PADDR + DSP_SIZE) return pa - DSP_PADDR + DSP_BASE;
    return 0;
}

u8 *paddr_ptr(u32 pa) {
    u32 va = paddr_to_vaddr(pa);
    return va ? g_mem + va : nullptr;
}

u32 vaddr_to_paddr(u32 va) {
    if (va >= GUEST_ALIAS_BASE && va < GUEST_ALIAS_LIMIT) va = (u32)(GuestMemoryPointer(va) - g_mem);
    if (va >= LINEAR_BASE && va < LINEAR_BASE + LINEAR_SIZE) return va - LINEAR_BASE + FCRAM_PADDR;
    if (va >= VRAM_BASE && va < VRAM_BASE + VRAM_SIZE) return va - VRAM_BASE + VRAM_PADDR;
    if (va >= DSP_BASE && va < DSP_BASE + DSP_SIZE) return va - DSP_BASE + DSP_PADDR;
    return 0;
}

u32 hle_alloc(u32 size, u32 align) {
    u32 a = (hle_next + align - 1) & ~(align - 1);
    hle_next = a + ((size + 0xFFF) & ~0xFFFu);
    if (hle_next > HLE_BASE + HLE_SIZE) FATAL("HLE memory exhausted\n");
    memset(g_mem + a, 0, size);
    return a;
}
}  // namespace mem

// ---------------------------------------------------------------- dispatch
#ifdef RECOMP_MODULES  // several games in one app: the chosen game's module gives its table (set_game_code)
static const GuestEntry *guest_entries;
static u32 guest_entry_count;
void set_game_code(const GuestEntry *entries, u32 count) {
    guest_entries = entries;
    guest_entry_count = count;
}
#else
extern "C" const GuestEntry guest_entries[];
extern "C" const u32 guest_entry_count;
#endif

const GuestEntry *dispatch_lookup(u32 addr) {
    u32 lo = 0, hi = guest_entry_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (guest_entries[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < guest_entry_count && guest_entries[lo].addr == addr) return &guest_entries[lo];
    return nullptr;
}

u32 function_start(u32 addr) {  // (entries are sorted; a function's own start has entry 0)
    u32 lo = 0, hi = guest_entry_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (guest_entries[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    while (lo > 0 && guest_entries[lo - 1].entry != 0) lo--;
    return lo > 0 ? guest_entries[lo - 1].addr & ~1u : 0;
}

#define DCACHE 8192
#ifdef __WIIU__  // no thread-local storage in an RPX; shared is safe, as one guest thread runs at a time
static struct { u32 addr; const GuestEntry *e; } dcache[DCACHE];
#else
static thread_local struct { u32 addr; const GuestEntry *e; } dcache[DCACHE];
#endif

static const GuestEntry *lazy_lookup(u32 addr);

u64 rt_lookups, rt_lookup_misses;  // for kstats_report

static const GuestEntry *lookup_cached(u32 addr) {
    rt_lookups++;
    auto &slot = dcache[(addr >> 2) & (DCACHE - 1)];
    if (slot.e && slot.addr == addr) return slot.e;
    rt_lookup_misses++;
    static u32 pretend_missing = getenv("RECOMP_TEST_LAZY") ? (u32)strtoul(getenv("RECOMP_TEST_LAZY"), nullptr, 16) : 0;
    const GuestEntry *e = addr == pretend_missing ? nullptr : dispatch_lookup(addr);
    if (!e && !(addr & 1) && addr != pretend_missing) e = dispatch_lookup(addr | 1);  // a Thumb target reached without bit 0
    if (!e) e = lazy_lookup(addr);
    slot.addr = addr;
    slot.e = e;
    return e;
}

// ---- lazy recompilation: code the static analysis missed is recompiled into a dylib on first use
static std::mutex lazy_m;
static std::map<u32, GuestEntry> lazy_map;

static const GuestEntry *lazy_lookup(u32 addr) {
    std::lock_guard<std::mutex> lk(lazy_m);
    auto it = lazy_map.find(addr);
    return it == lazy_map.end() ? nullptr : &it->second;
}

static bool lazy_compile(u32 addr) {
#ifdef _WIN32
    // tools/win/build3ds.py leaves this in the run's folder: it makes lazy/lazy_<address>.dll there, built against the program's own exports
    char hexaddr[16], dll[64];
    snprintf(hexaddr, sizeof hexaddr, "%x", addr);
    snprintf(dll, sizeof dll, "lazy\\lazy_%x.dll", addr);
    LOG("[lazy] recompiling missing code at %08x\n", addr);
    if (system((std::string(".\\lazy.cmd ") + hexaddr).c_str()) != 0) return false;
    void *h = dlopen(dll, RTLD_NOW);
    if (!h) {
        LOG("[lazy] dlopen: %s\n", dlerror());
        return false;
    }
    auto ents = (const GuestEntry *)dlsym(h, "guest_lazy_entries");
    auto count = (const u32 *)dlsym(h, "guest_lazy_count");
    if (!ents || !count) return false;
    std::lock_guard<std::mutex> lk(lazy_m);
    for (u32 i = 0; i < *count; i++) lazy_map[ents[i].addr] = ents[i];
    return true;
#elif defined(RECOMP_SOURCE_DIR) && !TARGET_OS_IOS && !defined(RECOMP_MODULES)  // (iOS has no compiler to run)
    std::string root = RECOMP_SOURCE_DIR;
    std::string game = RECOMP_GAME;
    char gamedir[4096];
    if (!realpath(g_game.dir.c_str(), gamedir)) return false;
    char base[256];
    snprintf(base, sizeof base, "%s/build/lazy/lazy_%08x", root.c_str(), addr);
    char hexaddr[16];
    snprintf(hexaddr, sizeof hexaddr, "%x", addr);
    std::string cmd = "mkdir -p '" + root + "/build/lazy' && cd '" + root + "' && $( [ -x .venv/bin/python ] && echo .venv/bin/python || echo python3 ) -m tools.recomp.gen '" +
                      gamedir + "' generated/" + game + " --lazy " + hexaddr + " --lazy-out " + base +
                      ".c > /dev/null && clang -O1 -w -ffp-contract=off -fno-strict-aliasing -shared -fPIC "
                      "-undefined dynamic_lookup -Iruntime -Igenerated/" + game + " " + base + ".c -o " + base + ".dylib";
    LOG("[lazy] recompiling missing code at %08x\n", addr);
    if (system(cmd.c_str()) != 0) return false;
    void *h = dlopen((std::string(base) + ".dylib").c_str(), RTLD_NOW);
    if (!h) {
        LOG("[lazy] dlopen: %s\n", dlerror());
        return false;
    }
    auto ents = (const GuestEntry *)dlsym(h, "guest_lazy_entries");
    auto count = (const u32 *)dlsym(h, "guest_lazy_count");
    if (!ents || !count) return false;
    std::lock_guard<std::mutex> lk(lazy_m);
    for (u32 i = 0; i < *count; i++) lazy_map[ents[i].addr] = ents[i];
    return true;
#else
    (void)addr;
    return false;
#endif
}

static const GuestEntry *dispatch_miss(CPU *c) {
    u32 addr = c->pc;
    if (auto e = lazy_lookup(addr)) return e;
    FILE *f = fopen((g_save_base + "/missing_functions.txt").c_str(), "a");
    if (f) {
        fprintf(f, "0x%08x\n", addr);
        fclose(f);
    }
    LOG("[lazy] no code at %08x (lr=%08x)\n", addr, c->r[14]);
    if (addr < mem::CODE_BASE)  // a null (or near-null) function pointer: nothing to recompile
        FATAL("jump to %08x (lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x)\n", addr, c->r[14], c->r[13],
              c->r[0], c->r[1], c->r[2], c->r[3], c->r[4]);
    if (lazy_compile(addr) || ((addr & 1) && lazy_compile(addr & ~1u))) {
        if (auto e = lazy_lookup(addr)) return e;
    }
    FATAL("no recompiled code at 0x%08x (lr=%08x sp=%08x r0-r12 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x); added to %s/missing_functions.txt\n",
          addr, c->r[14], c->r[13], c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12],
          g_save_base.c_str());
}

static int trace_left = -1;
static void trace(CPU *c, const char *why) {
    if (trace_left < 0) {
        const char *t = getenv("RECOMP_TRACE");
        trace_left = t ? atoi(t) : 0;
    }
    if (trace_left > 0) {
        trace_left--;
        LOG("[trace] %s %08x r0=%08x r1=%08x r4=%08x r5=%08x sp=%08x lr=%08x\n", why, c->pc, c->r[0], c->r[1], c->r[4],
            c->r[5], c->r[13], c->r[14]);
    }
}

constexpr int kHooks = 4;
static struct {
    u32 addr;
    void (*fn)(CPU *);
} hooks[kHooks];
static std::atomic<int> hook_count;
constexpr u32 CALL_RETURN = 0xFFFF0004u;  // where rt_guest_call's function returns to

void rt_add_dispatch_hook(u32 addr, void (*hook)(CPU *c)) {
    const int n = hook_count;
    if (n == kHooks) FATAL("more than %d dispatch hooks\n", kHooks);
    hooks[n] = {addr, hook};
    hook_count = n + 1;  // (published once the entry is in)
}

u32 rt_guest_call(CPU *c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    const CPU saved = *c;
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    c->r[14] = CALL_RETURN;
    c->pc = fn;
    while (c->pc != CALL_RETURN) {
        const GuestEntry *e = lookup_cached(c->pc);
        if (!e) e = dispatch_miss(c);
        e->fn(c, e->entry);
    }
    const u32 r = c->r[0];
    *c = saved;
    return r;
}

extern "C" void rt_dispatch(CPU *c, u32) {
    trace(c, "dispatch");
    if (const int n = hook_count.load(std::memory_order_acquire); __builtin_expect(n, 0))
        for (int i = 0; i < n; i++)
            if (c->pc == hooks[i].addr) hooks[i].fn(c);
    if (__builtin_expect(c->pc == CALL_RETURN, 0)) return;  // (an indirect return from rt_guest_call's function)
    const GuestEntry *e = lookup_cached(c->pc);
    if (__builtin_expect(!e, 0)) {
        if (c->pc == EXIT_MAGIC) return;
        e = dispatch_miss(c);
    }
    e->fn(c, e->entry);
}

void run_guest(CPU *c) {
    while (c->pc != EXIT_MAGIC) {
        trace(c, "top");
        const GuestEntry *e = lookup_cached(c->pc);
        if (!e) e = dispatch_miss(c);
        e->fn(c, e->entry);
    }
}

extern "C" void rt_undefined(CPU *c, u32 addr, u32 word) {
    FATAL("undefined instruction %08x at %08x (lr=%08x)\n", word, addr, c->r[14]);
}
