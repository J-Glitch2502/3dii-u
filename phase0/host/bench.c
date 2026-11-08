// Phase 0 benchmark: how fast does recompiled ARM11 code run on a Wii U? Runs the same workload natively and as recompiled
// code in a flat 512 MB guest address space. Also builds for a PC or qemu-ppc, where --verify checks results only.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift_rt.h"
#include "counts.h"

#ifdef ESPRESSO
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>
#endif

#define GUEST_SPAN 0x20000000u  // an old-3DS game's memory map fits under this, so one flat block replaces page mapping
#define CODE_BASE 0x00100000u
#define STACK_TOP 0x0FF00000u
#define EXIT_MAGIC 0xFFFF0000u
#define NKERNEL 7
#define UNALIGNED_KERNEL 6  // reads values with both byte orders in play, so native results legitimately differ
#define TARGET_MIPS 536.0   // 2x an Old 3DS at one instruction per clock, which real ARM11 code doesn't reach

extern u32 native_main(u32 kernel, u32 iters);
extern const u8 guest_code[], guest_code_end[];
__asm__(".section .rodata\n.balign 4\n.global guest_code\nguest_code:\n.incbin \"build/guest/code.bin\"\n"
        ".global guest_code_end\nguest_code_end:\n.previous\n");

u8 *g_mem;
volatile u32 rt_preempt_flag;

static char report[8192];
static size_t report_len;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (report_len < sizeof report)
        report_len += (size_t)snprintf(report + report_len, sizeof report - report_len, "%s\n", line);
#ifdef ESPRESSO
    WHBLogPrint(line);
    WHBLogConsoleDraw();
#else
    puts(line);
#endif
}

#ifdef ESPRESSO
static double now_seconds(void) { return (double)OSTicksToMicroseconds(OSGetSystemTime()) / 1e6; }
#else
#include <time.h>
static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

// ---- recompiled code's runtime
extern const GuestEntry guest_entries_abi[], guest_entries_safe[];
extern const u32 guest_entry_count_abi, guest_entry_count_safe;

typedef struct {
    const GuestEntry **slots;
    u32 count;
} Table;

static Table table_abi, table_safe;
static const Table *table;  // the variant running now

static void build_table(Table *t, const GuestEntry *entries, u32 n) {
    u32 top = CODE_BASE;
    for (u32 i = 0; i < n; i++)
        if (entries[i].addr > top) top = entries[i].addr;
    t->count = ((top - CODE_BASE) >> 2) + 1;
    t->slots = calloc(t->count, sizeof *t->slots);
    for (u32 i = 0; i < n; i++) t->slots[(entries[i].addr - CODE_BASE) >> 2] = &entries[i];
}

static void build_tables(void) {
    build_table(&table_abi, guest_entries_abi, guest_entry_count_abi);
    build_table(&table_safe, guest_entries_safe, guest_entry_count_safe);
}

static const GuestEntry *lookup(u32 pc) {
    u32 slot = (pc - CODE_BASE) >> 2;
    return slot < table->count ? table->slots[slot] : NULL;
}

static void __attribute__((noreturn)) die(const char *what, u32 a, u32 b) {
    say("FATAL: %s %08x %08x", what, (unsigned)a, (unsigned)b);
#ifdef ESPRESSO
    for (;;) OSSleepTicks(OSMillisecondsToTicks(100));
#else
    exit(1);
#endif
}

void rt_dispatch(CPU *c) {
    const GuestEntry *e = lookup(c->pc);
    if (!e) {
        if (c->pc == EXIT_MAGIC) return;
        die("no code at", c->pc, c->r[14]);
    }
    MUSTTAIL return e->fn(c);
}
void rt_preempt(CPU *c) { (void)c; }
void rt_bad_jump(CPU *c, u32 target, u32 from) { (void)c; die("bad computed jump", target, from); }

static u32 run_lifted(u32 kernel, u32 iters) {
    CPU c;
    memset(&c, 0, sizeof c);
    c.r[0] = kernel;
    c.r[1] = iters;
    c.r[13] = STACK_TOP;
    c.r[14] = EXIT_MAGIC;
    c.pc = CODE_BASE;
    while (c.pc != EXIT_MAGIC) {
        const GuestEntry *e = lookup(c.pc);
        if (!e) die("no code at", c.pc, c.r[14]);
        e->fn(&c);
    }
    return c.r[0];
}
static u32 lifted_abi(u32 kernel, u32 iters) {
    table = &table_abi;
    return run_lifted(kernel, iters);
}
static u32 lifted_safe(u32 kernel, u32 iters) {
    table = &table_safe;
    return run_lifted(kernel, iters);
}

// ---- memory: the flat block is itself a Phase 0 question
static int alloc_guest(void) {
#ifdef ESPRESSO
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
    unsigned free_mb = (unsigned)(MEMGetTotalFreeSizeForExpHeap(heap) >> 20);
    unsigned largest_mb = (unsigned)(MEMGetAllocatableSizeForExpHeapEx(heap, 0x1000) >> 20);
    g_mem = MEMAllocFromDefaultHeapEx(GUEST_SPAN, 0x1000);
    if (g_mem) memset(g_mem, 0, GUEST_SPAN);
    say("MEMORY: %s, flat %u MB block (MEM2 free %u MB, largest %u MB)", g_mem ? "PASS" : "FAIL", GUEST_SPAN >> 20, free_mb,
        largest_mb);
#else
    g_mem = calloc(1, GUEST_SPAN);
    say("MEMORY: %s, flat %u MB block", g_mem ? "PASS" : "FAIL", GUEST_SPAN >> 20);
#endif
    if (!g_mem) return 0;
    memcpy(g_mem + CODE_BASE, guest_code, (size_t)(guest_code_end - guest_code));
    return 1;
}

// ---- tests
typedef u32 (*Runner)(u32, u32);

typedef struct {
    Runner run;
    u32 kernel;
    double seconds, instructions;
} Timed;

static double instructions_for(u32 kernel, u32 iters) {
    return (double)kernel_counts[kernel].base + (double)kernel_counts[kernel].per_iter * iters;
}

// Doubles the work until a run lasts a full second, so timer resolution can't skew the result.
static void timed_run(Timed *t) {
    u32 iters = 1;
    for (;;) {
        double start = now_seconds();
        (void)t->run(t->kernel, iters);
        t->seconds = now_seconds() - start;
        if (t->seconds >= 1.0 || iters >= 1u << 28) break;
        iters *= 2;
    }
    t->instructions = instructions_for(t->kernel, iters);
}

#ifdef ESPRESSO
static int timed_thread(int argc, void *arg) {
    (void)argc;
    timed_run(arg);
    return 0;
}

// Pinned to one core with the main thread blocked, so nothing else on that core touches the numbers.
static void timed_on_core(Timed *t, int core) {
    static OSThread thread __attribute__((aligned(16)));
    enum { STACK = 1 << 20 };
    static u8 *stack;
    if (!stack) stack = MEMAllocFromDefaultHeapEx(STACK, 16);
    const OSThreadAttributes affinity[3] = {OS_THREAD_ATTRIB_AFFINITY_CPU0, OS_THREAD_ATTRIB_AFFINITY_CPU1,
                                            OS_THREAD_ATTRIB_AFFINITY_CPU2};
    int result;
    memset(&thread, 0, sizeof thread);
    OSCreateThread(&thread, timed_thread, 0, t, stack + STACK, STACK, 12, affinity[core]);
    OSResumeThread(&thread);
    OSJoinThread(&thread, &result);
}
#else
static void timed_on_core(Timed *t, int core) {
    (void)core;
    timed_run(t);
}
#endif

static int verify(const char *label, Runner run) {
    int failed = 0;
    for (u32 k = 0; k < NKERNEL; k++) {
        if (run == native_main && k == UNALIGNED_KERNEL) continue;
        for (u32 iters = 1; iters <= 2; iters++) {
            u32 got = run(k, iters), want = kernel_counts[k].check[iters - 1];
            if (got != want) {
                say("%s %s(%u): got %08x, want %08x", label, kernel_counts[k].name, (unsigned)iters, (unsigned)got,
                    (unsigned)want);
                failed++;
            }
        }
    }
    return !failed;
}

static double measure(Runner run, u32 kernel, int core, double *instructions, double *seconds) {
    Timed t = {run, kernel, 0, 0};
    timed_on_core(&t, core);
    if (instructions) *instructions += t.instructions;
    if (seconds) *seconds += t.seconds;
    return t.instructions / t.seconds / 1e6;
}

static void benchmark(void) {
    double abi_instr = 0, abi_secs = 0, safe_instr = 0, safe_secs = 0, slowest = 1e30;
    say("MIPS on core 1:        abi     safe   native-equivalent");
    for (u32 k = 0; k < NKERNEL; k++) {
        double abi = measure(lifted_abi, k, 1, &abi_instr, &abi_secs);
        double safe = measure(lifted_safe, k, 1, &safe_instr, &safe_secs);
        double native = measure(native_main, k, 1, NULL, NULL);
        if (abi < slowest) slowest = abi;
        say("%-14s %9.1f %8.1f %8.1f", kernel_counts[k].name, abi, safe, native);
    }
    double overall = abi_instr / abi_secs / 1e6, overall_safe = safe_instr / safe_secs / 1e6;
    say("OVERALL abi %.1f MIPS (%.2fx Old 3DS at 268), safe %.1f MIPS (%.2fx)", overall, overall / 268.0, overall_safe,
        overall_safe / 268.0);
    say("Slowest kernel, abi: %.1f MIPS", slowest);
    double core0 = measure(lifted_abi, 0, 0, NULL, NULL), core2 = measure(lifted_abi, 0, 2, NULL, NULL);
    say("Entities on other cores (smaller cache): core 0 %.1f, core 2 %.1f MIPS", core0, core2);
    say("CPU: %s (target %.0f MIPS)", overall >= TARGET_MIPS ? "PASS" : "FAIL", TARGET_MIPS);
}

static int run_all(int verify_only) {
    if (!alloc_guest()) return 0;
    build_tables();
    int ok = verify("native", native_main);
    ok &= verify("abi", lifted_abi);
    ok &= verify("safe", lifted_safe);
    say("VERIFY native, abi, safe: %s", ok ? "PASS, every result matches the ARM reference" : "FAIL");
    if (!ok) {
        say("Results are wrong, so timings would mean nothing. Skipping them.");
        return 0;
    }
    if (!verify_only) benchmark();
    return 1;
}

#ifdef ESPRESSO
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBLogConsoleInit();
    say("Running: about a minute. HOME won't respond until it's done.");
    run_all(0);
    FILE *f = fopen("fs:/vol/external01/3ds_wiiu_benchmark.txt", "w");
    if (f) {
        fwrite(report, 1, report_len, f);
        fclose(f);
    }
    // The console shows 16 lines, so everything above fits on one screen.
    say(f ? "Saved to SD: 3ds_wiiu_benchmark.txt. Press HOME to exit." : "SD save failed, please photograph this. Press HOME to exit.");
    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
#else
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--run")) {  // one kernel, for tools/ratio.py to count instructions under qemu
        if (!alloc_guest()) return 1;
        build_tables();
        Runner run = !strcmp(argv[2], "native") ? native_main : !strcmp(argv[2], "safe") ? lifted_safe : lifted_abi;
        printf("%08x\n", (unsigned)run((u32)atoi(argv[3]), (u32)atoi(argv[4])));
        return 0;
    }
    return run_all(argc > 1 && !strcmp(argv[1], "--verify")) ? 0 : 1;
}
#endif
