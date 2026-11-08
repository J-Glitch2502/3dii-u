// Stand-ins for the runtime, to find out whether the whole recompiled game can load as a Wii U executable alongside its
// 512 MB of guest memory. Writes its progress to the SD card after every step, so a stall shows where it stopped.
#include <cpu.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/debug.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

u8 *g_mem;
volatile u32 rt_preempt_flag;
extern const GuestEntry guest_entries[];
extern const u32 guest_entry_count;

void rt_dispatch(CPU *c, u32 unused) { (void)c; (void)unused; }
void rt_svc(CPU *c, u32 imm) { (void)c; (void)imm; }
void rt_preempt(CPU *c) { (void)c; }
void rt_undefined(CPU *c, u32 addr, u32 word) { (void)c; (void)addr; (void)word; }

static char report[512];
static int report_len;

static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report_len += vsnprintf(report + report_len, sizeof report - report_len, fmt, ap);
    va_end(ap);
    report[report_len++] = '\n';
    FILE *f = fopen("fs:/vol/external01/3ds_wiiu_code_progress.txt", "w");
    if (f) {
        fwrite(report, 1, (size_t)report_len, f);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    WHBProcInit();
    step("main reached");
    WHBLogConsoleInit();
    step("console ready");
    // Printing the table's address also keeps all the game code in the link: --gc-sections drops whatever isn't reachable.
    step("CODE: PASS, the recompiled game loaded: %u functions, first at %p", (unsigned)guest_entry_count, (void *)guest_entries[0].fn);
    // The game needs its code and its flat 512 MB guest memory at the same time.
    void *guest = MEMAllocFromDefaultHeapEx(0x20000000, 0x1000);
    step("MEMORY: %s, 512 MB alongside the code", guest ? "PASS" : "FAIL");
    if (guest) {
        memset(guest, 0, 0x20000000);
        step("512 MB touched");
        MEMFreeToDefaultHeap(guest);
    }
    FILE *f = fopen("fs:/vol/external01/3ds_wiiu_code_test.txt", "w");
    if (f) {
        fwrite(report, 1, (size_t)report_len, f);
        fclose(f);
    }
    WHBLogPrint(report);
    WHBLogPrint(f ? "Saved to SD: 3ds_wiiu_code_test.txt. Press HOME to exit." : "SD save failed, please photograph this. Press HOME to exit.");
    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
