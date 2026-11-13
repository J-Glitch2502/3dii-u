// The Wii U frontend: guest threads share core 1 (the 2 MB L2), the GPU work runs on core 2, input and audio on core 0.
// VBlank timing mirrors runtime/frontend.cpp. Files: the game in the title's content, saves in the title's save storage on the console (save_storage.cpp); the log goes to the PC over the network (net_console.cpp).
#include <coreinit/context.h>
#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/event.h>
#include <coreinit/exception.h>
#include <coreinit/filesystem.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/systeminfo.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <gx2/event.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <sys/stat.h>
#include <malloc.h>
#include <nn/act.h>
#include <unistd.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>

#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>

#include "audio/host.h"
#include "audio_iface.h"
#include "frontend.h"
#include "gpu_iface.h"
#include "kernel.h"
#include "loader.h"
#include "service.h"
#include "services/services.h"
#include "settings.h"
#include "wiiu.h"

static const char *const CONTENT_DIR = "fs:/vol/content";
void rt_log_sync();
static volatile bool want_log_sync, want_dump, crash_pending;
static volatile u32 want_hang_dump, card_passes;  // want_hang_dump: seconds without drawing, plus one
static volatile u32 idle_samples, all_samples;
static char crash_msg[512];

// How start-up went, in the log: only memory until the PC or the end of the game takes it
static OSTime trace_start;
static const char *volatile trace_stage = "before main";

static void stage(const char *name) {
    trace_stage = name;
    LOG("[stage] %s at %u ms\n", name, (u32)OSTicksToMilliseconds(OSGetSystemTime() - trace_start));
}

static char last_note[80];
volatile u32 wiiu::last_vs, wiiu::last_ps;

void wiiu::note(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(last_note, sizeof last_note, fmt, a);
    va_end(a);
    LOG("[note] %s\n", last_note);  // memory: this runs on threads that must never wait for the card
}

// On a console in the middle of a freeze, listing the SD card hung while the network and the OS carried on, so the file system is what to keep away from. Only one
// thread at a time goes near it, and never the ones that draw. The game's own reads and saves have to wait their turn; everything else waits on the card thread.
static OSMutex file_m;
static volatile u32 file_op;
static volatile OSTime file_since;

void wiiu::fs_begin(u32 op) {
    OSLockMutex(&file_m);
    file_since = OSGetSystemTime();
    file_op = op;
}

void wiiu::fs_end() {
    file_op = 0;
    OSUnlockMutex(&file_m);
}

// The game's RomFS reads (fs.cpp's FsIo 0x0802) only wait for each other: the card thread holds file_m for as long as a save takes to write out and flush
// (6 s for Tomodachi Life's 2 MB saves, 0.4-1.2 s for Bowser's Inside Story's), and loading waited all that time (2026-11-08). The content is read only and on
// another device than the saves. The freeze that once made every file call take turns was the FTP plugin's heap lock, not the file system.
static OSMutex romfs_m;
void wiiu::romfs_begin() { OSLockMutex(&romfs_m); }
void wiiu::romfs_end() { OSUnlockMutex(&romfs_m); }

// Each game keeps its saves and logs in its own title's save storage, on the console wherever the game is installed
const std::string &wiiu::data_dir() {
    static const std::string dir = "fs:/vol/save/common";
    return dir;
}

std::string g_save_base;  // set at the start of main: files can't be read any earlier
char **g_argv;
std::string g_start_game;

static OSEvent gpu_event;  // an event, not a timed condition wait: see tq_event in kernel.cpp

namespace kernel {
extern const char *g_live_path;
void frontend_wake() { OSSignalEvent(&gpu_event); }
}  // namespace kernel
void frontend_live_text(const std::string &) {}
// A route's "KEY quit" quits as choosing Quit in the options menu does. Cemu has no Wii U Menu to leave for, so there the main loop ends
// itself, as if the system had said so: what a game does as it closes then runs in a check too.
static volatile bool route_quit;
void frontend_live_key(const std::string &key) {  // a route's "KEY <name>"
    if (key != "quit") return;
    if (wiiu::debug_flag("cemu")) route_quit = true;
    else wiiu::menu_quit_now();
}
namespace onescreen {
u32 host_mask(const char *) { return 0; }
}  // namespace onescreen

void frontend_init() {
    OSInitEvent(&gpu_event, FALSE, OS_EVENT_MODE_AUTO);
    gpu::init();
    audio::init();
}

static OSThread *spawn(OSThreadEntryPointFn fn, int core, int prio, u32 stack_size, const char *name) {
    auto *t = (OSThread *)memalign(16, sizeof(OSThread));
    void *stack = memalign(16, stack_size);
    const OSThreadAttributes attr = core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : core == 1 ? OS_THREAD_ATTRIB_AFFINITY_CPU1 : OS_THREAD_ATTRIB_AFFINITY_CPU2;
    if (!OSCreateThread(t, fn, 0, nullptr, (u8 *)stack + stack_size, stack_size, prio, attr)) FATAL("cannot create thread %s\n", name);
    OSSetThreadName(t, name);
    OSResumeThread(t);
    return t;
}

static volatile bool quitting, gpu_parked, audio_parked, gfx_may_close;
static volatile bool gpu_pause, foreground_back;
#ifdef LAYTON_PROF
extern "C" BOOL GfxHeapInitMEM1();
extern "C" BOOL GfxHeapDestroyMEM1();
extern "C" void *GfxHeapAllocMEM1(uint32_t, uint32_t);
extern "C" void GfxHeapFreeMEM1(void *);
#endif

namespace gpu {
extern bool gx2_skip_pass;
}
static volatile bool home_slow;  // on_release's stack is small, so the watchdog does its logging
static volatile int home_release_ms = -1;

// The system is taking the GPU back, for the HOME Menu or for good: stop using it first. ProcUI waits for this before it goes on.
// This runs on the GPU thread's core with a higher priority than WHB's own callback, which frees the screens' memory and waits for the GPU from a thread that can stop the GPU thread
// anywhere, even in the middle of a command. On a console both used to run while the game kept drawing, and the GPU thread was still at it when GX2 went away a second later.
static uint32_t on_release(void *) {
    const OSTime t0 = OSGetSystemTime();
    foreground_back = false;
    kernel::gx_hold = true;
    gpu::gx2_skip_pass = true;  // what's left of the list it's on only sets registers
    gpu_pause = true;
    for (int i = 0; i < 500 && !gpu_parked; i++) {
        OSSleepTicks(OSMillisecondsToTicks(2));
        if (i == 100) home_slow = !gpu_parked;
    }
    home_release_ms = (int)OSTicksToMilliseconds(OSGetSystemTime() - t0);
    return 0;
}

static uint32_t on_acquire(void *) {
    foreground_back = true;
    return 0;
}

// on_exception sends the GPU thread here if GX2 went away under it anyway
static jmp_buf gpu_restart;
static volatile bool gpu_tripped;
static void gpu_trip() {
    gpu_tripped = true;
    longjmp(gpu_restart, 1);
}

static int gpu_thread(int, void *) {
    wiiu::profile_this_core();
    wiiu::gfx_thread_init();
    stage("gpu thread ready");
    setjmp(gpu_restart);
    while (!quitting) {
        if (gpu_pause) {
            if (gpu_tripped) LOG("[home] the GPU thread was still drawing when GX2 went away\n");
            else wiiu::gfx_shutdown();
            gpu_tripped = false;
            gpu_parked = true;
            while (gpu_pause && !quitting) OSSleepTicks(OSMillisecondsToTicks(2));
            if (quitting) break;
            wiiu::gfx_reacquire();
            gpu_parked = false;
            continue;
        }
        // quiet: two milliseconds went by with nothing asked of the GPU, and nothing came in at the last moment either, which is when a
        // finished frame may be shown
        const bool waited = !OSWaitEventWithTimeout(&gpu_event, 2000000);  // nanoseconds
        const u64 done = kernel::gx_stats.cmdlists + kernel::gx_stats.transfers;
        const OSTime t0 = kMeasure ? OSGetSystemTime() : 0;
        kernel::gsp_process_gx();
        if (!gpu_pause) wiiu::gfx_pump(waited && done == kernel::gx_stats.cmdlists + kernel::gx_stats.transfers);
        if (kMeasure) wiiu::gfx_note_busy((u32)OSTicksToMicroseconds(OSGetSystemTime() - t0));
    }
    if (!gpu_parked) wiiu::gfx_shutdown();
    gpu_parked = true;
    // GX2 has to close on the core it started on, once the system has taken the foreground back
    while (!gfx_may_close) OSSleepTicks(OSMillisecondsToTicks(2));
    WHBGfxShutdown();
    return 0;
}

// 3DS button bits as hid.cpp expects them: A B Select Start Right Left Up Down R L X Y
static u32 map_buttons(u32 h) {
    u32 b = 0;
    if (h & VPAD_BUTTON_A) b |= 1u << 0;
    if (h & VPAD_BUTTON_B) b |= 1u << 1;
    if (h & VPAD_BUTTON_MINUS) b |= 1u << 2;
    if (h & VPAD_BUTTON_PLUS) b |= 1u << 3;
    if (h & VPAD_BUTTON_RIGHT) b |= 1u << 4;
    if (h & VPAD_BUTTON_LEFT) b |= 1u << 5;
    if (h & VPAD_BUTTON_UP) b |= 1u << 6;
    if (h & VPAD_BUTTON_DOWN) b |= 1u << 7;
    if (h & (VPAD_BUTTON_R | VPAD_BUTTON_ZR)) b |= 1u << 8;
    if (h & (VPAD_BUTTON_L | VPAD_BUTTON_ZL)) b |= 1u << 9;
    if (h & VPAD_BUTTON_X) b |= 1u << 10;
    if (h & VPAD_BUTTON_Y) b |= 1u << 11;
    return b;
}

static int hid_thread(int, void *) {
    wiiu::profile_this_core();
    // Debug aid: with a file named "autoplay" in the data folder, presses A and taps around the bottom screen by itself
    bool autoplay = false;
    if (FILE *f = fopen((wiiu::data_dir() + "/autoplay").c_str(), "rb")) {
        fclose(f);
        autoplay = true;
    }
    // Debug aid: with a file named "live.txt" in the data folder, plays the input commands in it (services/hid.cpp)
    static const std::string live_path = wiiu::data_dir() + "/live.txt";
    if (FILE *f = fopen(live_path.c_str(), "rb")) {
        fclose(f);
        kernel::g_live_path = live_path.c_str();
    }
    u32 tick = 0;
    bool touch_was = false, leaving = false;
    VPADStatus st{};
    VPADTouchData cal{};
    u32 buttons = 0;
    s16 cx = 0, cy = 0;
    bool touch = false;
    u16 tx = 0, ty = 0;
    for (;;) {
        VPADReadError err = VPAD_READ_SUCCESS;
        VPADRead(VPAD_CHAN_0, &st, 1, &err);
        if (err == VPAD_READ_SUCCESS) {
            buttons = map_buttons(st.hold);
            wiiu::mic_blow(st.hold & VPAD_BUTTON_STICK_R);
            wiiu::menu_input(st.trigger);
            if (!leaving && wiiu::menu_wants_quit()) {
                leaving = true;
                SYSLaunchMenu();
            }
            cx = (s16)(st.leftStick.x * 156.0f);
            cy = (s16)(st.leftStick.y * 156.0f);
            // The GamePad's sensors in the 3DS's frame: y comes out of the screen and z towards the player. 512 is 1g; 14.375 is a degree a second, and the GamePad's 1.0 is a turn a second.
            auto to_s16 = [](float v) { return (s16)std::clamp(v, -32767.0f, 32767.0f); };
            const VPADVec3D &ac = st.accelerometer.acc;
            const s16 accel[3] = {to_s16(ac.x * 512), to_s16(ac.z * 512), to_s16(-ac.y * 512)};
            const s16 gyro[3] = {to_s16(st.gyro.x * 5175), to_s16(st.gyro.z * 5175), to_s16(-st.gyro.y * 5175)};
            kernel::hid_motion(accel, gyro);
            touch = false;
            bool down = false;
            u16 px = 0, py = 0;
            if (st.tpNormal.touched && !st.tpNormal.validity) {
                VPADGetTPCalibratedPoint(VPAD_CHAN_0, &cal, &st.tpNormal);
                px = (u16)(cal.x * 854 / 1280);
                py = (u16)(cal.y * 480 / 720);
                down = true;
                wiiu::touch_to_bottom(px, py, touch, tx, ty);
            }
            wiiu::keyboard_input(st.trigger, down && !touch_was, px, py);
            touch_was = down;
        }
        if (autoplay || kernel::g_live_path || wiiu::test_run()) {
            tick++;
            if (tick % 500 == 0 && wiiu::keyboard_open()) {
                wiiu::keyboard_input(0, true, 107 + 64 * 8 + 10, 150 + 62 * 4 + 10);  // OK, with what's typed
                if (wiiu::keyboard_open()) wiiu::keyboard_input(0, true, 107 + 10, 150 + 62 * 2 + 10);  // or type something first
            }
        }
        if (autoplay) {
            const u32 phase = tick % 468;
            if (phase < 12) buttons |= 1;
            if (phase >= 234 && phase < 246) {
                static const u16 spots[][2] = {{240, 206}, {160, 206}, {160, 120}, {240, 206}, {280, 30}, {40, 30}};
                const u16 *s = spots[(tick / 468) % 6];
                touch = true;
                tx = s[0];
                ty = s[1];
            }
        }
        wiiu::input_override(buttons, touch, tx, ty);
        kernel::hid_update(buttons, cx, cy, touch, tx, ty);
        if (kMeasure) {
            all_samples = all_samples + 1;
            if (kernel::guest_idle()) idle_samples = idle_samples + 1;
        }
        OSSleepTicks(OSMicrosecondsToTicks(4273));  // 234 Hz, as on the 3DS
    }
    return 0;
}

constexpr int AUDIO_CUSHION = 1600;  // samples kept queued, about 50 ms: audio_wiiu.cpp's CUSHION, which starts the voices at twice this

static int audio_thread(int, void *) {
    const OSTime period = OSNanosecondsToTicks(1000000000ll * 160 / 32728);
    OSTime next = OSGetSystemTime();
    bool silent = false;
    u32 gaps = 0;
    OSTime gaps_logged = 0;
    // for the report of a gap, since the last one: the longest this thread went between two rounds, and the longest a frame took to make
    OSTime round = OSGetSystemTime(), away = 0, making = 0;
    while (!quitting) {
        if (gpu_pause != silent) {
            silent = gpu_pause;
            audio::pause(silent);
            next = OSGetSystemTime();
        }
        if (silent) {
            OSSleepTicks(OSMillisecondsToTicks(5));
            continue;
        }
        const OSTime now = OSGetSystemTime();
        away = std::max(away, now - round);
        round = now;
        if (const int queued = audio::hle::host_queued(); queued >= 0) {
            // Once the voices play, a frame is made whenever what's queued falls under the cushion, so the sound keeps to the console's audio
            // clock. Made by the timer alone, production ran half a percent behind what the voices played, and the queue ran dry about once
            // a second with the game at full speed and the CPU mostly idle (NSMB2 in Cemu, 2026-11-10).
            if (queued >= AUDIO_CUSHION) {
                OSSleepTicks(OSMillisecondsToTicks(1));
                continue;
            }
        } else {  // before the voices start: by the timer
            next += period;
            if (next > now) OSSleepTicks(next - now);
            else if (now - next > period * 8) next = now;  // never try to catch up on more than a short stall
        }
        // Nearly nothing left queued: this thread came too late, and the voices are about to play old sound again. Every time is on the log, so
        // a run can be held to none (tools/check_ports.py).
        if (const int queued = audio::hle::host_queued(); queued >= 0 && queued < 160) gaps++;
        kernel::dsp_tick();
        making = std::max(making, OSGetSystemTime() - now);
        if (gaps && now - gaps_logged > (OSTime)OSSecondsToTicks(1)) {
            LOG("[audio] %u gaps: the sound ran out before the next frame of it was made (this thread was away for up to %u ms, a frame took up to %u ms)\n", gaps,
                (u32)OSTicksToMilliseconds(away), (u32)OSTicksToMilliseconds(making));
            gaps = 0;
            away = making = 0;
            gaps_logged = now;
        }
    }
    audio_parked = true;
    return 0;
}

// The threads this file knows, for hang reports: a thread's saved state and return addresses say where it's stuck
static OSThread *host_threads[4];
static const char *const host_names[4] = {"main", "gpu", "hid", "audio"};

void wiiu::log_status() {
    const u32 op_now = file_op;
    LOG("[st] frame %llu, gx lists %llu, shown %llu, guest %s, stage \"%s\", note \"%s\", file op %x for %u s, gpu retired %llu of %llu, shaders %08x %08x\n", (unsigned long long)g_frame,
        (unsigned long long)kernel::gx_stats.cmdlists, (unsigned long long)wiiu::gfx_flips(), kernel::guest_idle() ? "idle" : "busy", trace_stage, last_note, op_now,
        op_now ? (u32)OSTicksToSeconds(OSGetSystemTime() - file_since) : 0, (unsigned long long)GX2GetRetiredTimeStamp(), (unsigned long long)GX2GetLastSubmittedTimeStamp(), wiiu::last_vs,
        wiiu::last_ps);
}

static void dump_hang(u32 seconds) {
    LOG("[hang] no new drawing for %u s, or asked for\n", seconds);
    wiiu::log_threads();
    // this waits for locks a stuck thread may hold, which is why what's above goes first
    kernel::dump_threads();
}

// Built by hand: this has to work whatever a stuck thread holds, so no C library and no card, and the screen is the one way out left.
static char *put_text(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *put_hex(char *p, u32 v) {
    for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789abcdef"[(v >> i) & 15];
    *p++ = ' ';
    return p;
}

static void report_stuck(const char *what) {
    static char msg[2048];
    char *p = put_text(msg, what);
    p = put_text(p, "\nPlease send a photo of this screen.\nStage: ");
    p = put_text(p, trace_stage);
    p = put_text(p, "\nLast note: ");
    p = put_text(p, last_note);
    p = put_text(p, "\nframe ");
    p = put_hex(p, (u32)g_frame);
    p = put_text(p, "lists ");
    p = put_hex(p, (u32)kernel::gx_stats.cmdlists);
    p = put_text(p, "shown ");
    p = put_hex(p, (u32)wiiu::gfx_flips());
    p = put_text(p, kernel::guest_idle() ? "idle\n" : "busy\n");
    p = put_text(p, "shaders ");
    p = put_hex(p, wiiu::last_vs);
    p = put_hex(p, wiiu::last_ps);
    p = put_text(p, "\nfile op ");
    const u32 op_now = file_op;
    p = put_hex(p, op_now);
    p = put_hex(p, op_now ? (u32)OSTicksToSeconds(OSGetSystemTime() - file_since) : 0);
    p = put_text(p, "\n");
    for (u32 i = 0; i < 4; i++) {
        if (!host_threads[i]) continue;
        const OSContext &c = host_threads[i]->context;
        p = put_text(p, host_names[i]);
        p = put_text(p, " ");
        p = put_hex(p, c.srr0);
        p = put_hex(p, c.lr);
        u32 sp = c.gpr[1];
        for (int n = 0; n < 4 && sp >= 0x10000000 && sp < 0x42000000 && !(sp & 3); n++) {
            const u32 *frame = (const u32 *)sp;
            p = put_hex(p, frame[1]);
            sp = frame[0];
        }
        p = put_text(p, "\n");
    }
    // the game's own threads, two to a line: a thread's number, then what it's doing, then where
    u32 shown = 0;
    for (u32 i = 0; i < 64 && shown < 24; i++) {
        const auto *t = (const OSThread *)kernel::guest_host_thread(i);
        if (!t) continue;
        p = put_text(p, "g");
        p = put_hex(p, i << 8 | t->state);
        p = put_hex(p, t->context.srr0);
        p = put_hex(p, t->context.lr);
        p = put_text(p, ++shown % 2 ? " " : "\n");
    }
    *p = 0;
    // the screen is about to be all there is, and this thread may hold anything: only a word to the PC if it's listening
    if (wiiu::net_connected()) {
        LOG("[report] %s\n", msg);
        OSSleepTicks(OSMillisecondsToTicks(500));
    }
    OSFatal(msg);
}

// Nothing here can be stuck behind what it watches: it has its own thread, above the others on its core. It goes near neither the card nor the memory heap, so a frozen game
// ends on the screen even if one of those is what's stuck, unless the PC is connected: a frozen game is what it most wants to look at.
static int watchdog_thread(int, void *) {
    u64 last = ~0ull;
    u32 tenths = 0, seconds = 0, still = 0, next_dump = 10;
    u32 asked = 0, quiet = 0;  // what the game had asked of the system a second ago, and for how many seconds that hasn't moved
    bool drawn = false;
    for (;;) {
        OSSleepTicks(OSMillisecondsToTicks(200));
        if (crash_pending) {
            LOG("[crash] %s\n", crash_msg);
            wiiu::log_threads();
            rt_log_sync();  // the log to the card too: a crash took everything that led up to it with it (Super Mario 3D Land in Cemu, 2026-11-08)
            crash_pending = false;
            return 0;
        }
        if (home_slow) {
            home_slow = false;
            LOG("[home] the GPU thread still hasn't stopped\n");
            wiiu::log_threads();
        }
        if (home_release_ms >= 0) {
            LOG("[home] %s after %d ms\n", gpu_parked ? "the GPU thread stopped" : "gave up on the GPU thread", home_release_ms);
            home_release_ms = -1;
        }
        if (want_dump) {
            want_dump = false;
            want_hang_dump = 1;
        }
        if (++tenths < 5) continue;
        tenths = 0;
        // Drawing nothing isn't being stopped. A game that asks nothing of the system for half a minute has stopped; one that goes on asking
        // (reading its files, setting up its save on a first start, which a console's storage takes its time over) is at work, and gets three
        // minutes before the screen says otherwise. Counted by drawing alone, any start slower than 30 s ended as "frozen": a tester's Mario
        // Kart 7 ended 30 s after its second list on its first start, its last note the save it had just written, and started the second
        // time (2026-11-12). Whether that one was at work or stopped, the screen couldn't say.
        const u32 asking = kernel::svcs + (u32)kernel::gx_stats.transfers;
        quiet = asking == asked ? quiet + 1 : 0;
        asked = asking;
        if (++seconds >= 41 && !drawn && (quiet >= 30 || seconds >= 180) && !wiiu::net_connected()) report_stuck("The game got stuck starting up.");
        const u64 lists = kernel::gx_stats.cmdlists;
        if (lists != last || gpu_pause || wiiu::menu_open() || wiiu::keyboard_open()) {
            if (lists && !drawn) {
                drawn = true;
                stage("the game is drawing");
            }
            last = lists;
            still = 0;
            next_dump = 10;
        } else if (drawn) {
            if (++still == next_dump) {
                next_dump *= 2;
                want_hang_dump = still + 1;
            }
            if (still >= 30 && (quiet >= 30 || still >= 180) && !wiiu::net_connected()) report_stuck("The game has frozen.");
        }
    }
    return 0;
}

// Slow work for later, at the lowest priority on its core: flushing saves, and the log, which only reaches storage when the game ends: the PC and the report on the screen don't need it.
static int card_thread(int, void *) {
    for (u32 pass = 1;; pass++) {
        OSSleepTicks(OSMillisecondsToTicks(200));
        if (want_hang_dump) {
            const u32 seconds = want_hang_dump - 1;
            want_hang_dump = 0;
            dump_hang(seconds);
        }
        if (kMeasure && pass % 300 == 0) {
            const struct mallinfo m = mallinfo();
            LOG("[wiiu] memory: %u KB in use, %u KB taken from the system, top at %p\n", (u32)m.uordblks >> 10, (u32)m.arena >> 10, sbrk(0));
        }
        wiiu::save_flush(false);
        // a live.txt means a test run, likely in Cemu, which gets killed rather than quit
        if (want_log_sync || (kernel::g_live_path && pass % 150 == 0)) {
            const bool leaving = want_log_sync;
            want_log_sync = false;
            wiiu::fs_begin(0xC001);
            rt_log_sync();
            wiiu::fs_end();
            wiiu::fs_begin(0xC004);
            wiiu::gfx_write_misses();
            wiiu::fs_end();
            if (leaving) wiiu::save_flush(true);
            card_passes = card_passes + 1;
        }
        if (pass % 300 == 0) {
            wiiu::fs_begin(0xC005);
            wiiu::profile_write();
            wiiu::fs_end();
        }
    }
    return 0;
}

// the last of the saves and what led up to the end are still in memory: give the card thread a moment, but the screen comes first. A save the game made that isn't
// in storage yet gets as long as it takes, up to 30 s: the game showed the player it saved.
static void let_card_catch_up() {
    const u32 seen = card_passes;
    want_log_sync = true;
    const u32 ms = wiiu::save_pending() ? 30000 : 3000;
    for (const OSTime until = OSGetSystemTime() + OSMillisecondsToTicks(ms); card_passes == seen && OSGetSystemTime() < until;) OSSleepTicks(OSMillisecondsToTicks(5));
}

// What this console has had to compile for the game, copied to the SD card as the game closes: sd:/3dii-u/<title id>/learned.bin. It lives in
// the title's save, which nothing on a PC reads. The builder folds it into its next build of the game (builder/pipeline.py). With no card
// in, or no room on it, nothing happens. The game's log stays in its save: it is not copied anywhere.
static void copy_to_card(const std::string &from, const std::string &to) {
    FILE *in = fopen(from.c_str(), "rb");
    if (!in) return;
    if (FILE *out = fopen(to.c_str(), "wb")) {
        alignas(64) static char block[64 << 10];
        for (size_t n; (n = fread(block, 1, sizeof block, in)) > 0;)
            if (fwrite(block, 1, n, out) != n) break;
        fclose(out);
    }
    fclose(in);
}

static void leave_on_card() {
    char dir[64];
    snprintf(dir, sizeof dir, "fs:/vol/external01/3dii-u/%016llx", (unsigned long long)OSGetTitleID());
    const std::string to = dir, from = wiiu::data_dir();
    wiiu::fs_begin(0xC00D);
    mkdir("fs:/vol/external01/3dii-u", 0777);
    mkdir(dir, 0777);
    copy_to_card(from + "/shader_misses.bin", to + "/learned.bin");
    wiiu::fs_end();
}

void frontend_run() {
    wiiu::profile_this_core();
    OSThread *gpu = spawn(gpu_thread, 2, 16, 2 << 20, "gpu");
    host_threads[1] = gpu;
    host_threads[2] = spawn(hid_thread, 0, 16, 256 << 10, "hid");
    // The cores: 0 for files, input, the network, the log, the sound and the shader compiler; 1, with the large cache, for the game's code
    // alone; 2 for the GPU thread alone. The sound was tried on core 2, ahead of the GPU thread: NSMB2's slowest frames fell from 50 fps to
    // 14 to 16 and its restart from 60 fps to 39 to 44, twice over, and came back with the sound here (Cemu, 2026-11-11). Core 0's own
    // threads mostly sleep, and the sound has had no gaps from here.
    host_threads[3] = spawn(audio_thread, 0, 15, 512 << 10, "audio");
    spawn(card_thread, 0, 20, 256 << 10, "card");
    stage("threads started");
    while (!wiiu::gfx_ready()) OSSleepTicks(OSMillisecondsToTicks(5));
    stage("graphics ready");
    OSTime next_clock = OSGetSystemTime();
    u64 frames = 0;
    while (WHBProcIsRunning() && !route_quit) {
        // While the HOME Menu is up nothing may touch GX2, and with no VBlanks the game waits. WHBProcIsRunning blocks until we're back.
        if (gpu_pause) {
            if (!foreground_back) continue;
            for (int i = 0; i < 1000 && !gpu_parked; i++) OSSleepTicks(OSMillisecondsToTicks(2));
            gpu::gx2_skip_pass = false;
            gpu_pause = false;
            kernel::gx_hold = false;
            while (gpu_parked) OSSleepTicks(OSMillisecondsToTicks(1));
            LOG("[wiiu] back in the foreground\n");
        }
        GX2WaitForVsync();  // the guest's VBlank is the TV's, so frames line up with its refresh
        kernel::timers_poke();  // a timer whose wake-up went missing, whatever the reason, costs a frame rather than the game: frames carry on even when everything else waits
        // the game waits for VBlank, so it pauses while the menu is open; faster speeds get more VBlanks per TV frame
        if (!wiiu::menu_open()) {
            const int extra = wiiu::game_speed() / 100 - 1;
            for (int i = 0; i < extra; i++) {
                kernel::gsp_vblank();
                OSSleepTicks(OSMillisecondsToTicks(16 / (extra + 1)));
            }
            kernel::gsp_vblank();
        }
        wiiu::gfx_vblank();
        g_frame = ++frames;
        if (frames == 1) stage("first frame");
#ifdef LAYTON_PROF
        if (frames % 1800 == 0) {
            LOG("[homesim] release at frame %llu\n", (unsigned long long)frames);
            on_release(nullptr);
            GX2ColorBuffer *const cbs[2] = {WHBGfxGetTVColourBuffer(), WHBGfxGetDRCColourBuffer()};
            GX2DepthBuffer *const dbs[2] = {WHBGfxGetTVDepthBuffer(), WHBGfxGetDRCDepthBuffer()};
            for (auto *c : cbs)
                if (c->surface.image) { GfxHeapFreeMEM1(c->surface.image); c->surface.image = nullptr; }
            for (auto *d : dbs)
                if (d->surface.image) { GfxHeapFreeMEM1(d->surface.image); d->surface.image = nullptr; }
            GfxHeapDestroyMEM1();
            OSSleepTicks(OSMillisecondsToTicks(500));
            GfxHeapInitMEM1();
            for (auto *c : cbs) c->surface.image = GfxHeapAllocMEM1(c->surface.imageSize, c->surface.alignment);
            for (auto *d : dbs) d->surface.image = GfxHeapAllocMEM1(d->surface.imageSize, d->surface.alignment);
            LOG("[homesim] acquire\n");
            on_acquire(nullptr);
        }
#endif
        if (OSGetSystemTime() >= next_clock) {
            update_shared_page_time();
            next_clock = OSGetSystemTime() + OSSecondsToTicks(1);
        }
        if (kMeasure && frames % 300 == 0) {
            const u32 idle = all_samples ? idle_samples * 100 / all_samples : 0;
            LOG("[wiiu] frame %llu: gx lists %llu, transfers %llu, guest threads idle %u%%\n", (unsigned long long)frames, (unsigned long long)kernel::gx_stats.cmdlists,
                (unsigned long long)kernel::gx_stats.transfers, idle);
            idle_samples = all_samples = 0;
            if (kernel::kstats.ipc) LOG("%s", kernel::kstats_report(300).c_str());
            // still drawing a frame or two of nothing, with nobody busy: stuck waiting for something, which only the threads can show
            static u32 quiet;
            quiet = wiiu::gfx_report() < 4 && idle >= 90 ? quiet + 1 : 0;
            if (quiet == 4) want_dump = true;
        }
    }
    want_hang_dump = 1;  // a player who shuts down a frozen game shows what it was stuck on
    for (int i = 0; i < 100 && want_hang_dump; i++) OSSleepTicks(OSMillisecondsToTicks(10));
    let_card_catch_up();
    wiiu::profile_stop();
    quitting = true;
    gfx_may_close = true;
    OSJoinThread(gpu, nullptr);
    // The sound system is always shut down before the game goes: left running, its voices play from memory the system is about to take back, and
    // the Wii U Menu hangs as it starts (testers' NSMB2 and 3D Land, 2026-11-09). The audio thread used to get 100 ms to stop, and a late one
    // meant no shutdown at all, with nothing logged. A busy game's can be late, so it gets 2 s, and the shutdown happens either way.
    const OSTime audio_asked = OSGetSystemTime();
    for (int i = 0; i < 1000 && !audio_parked; i++) OSSleepTicks(OSMillisecondsToTicks(2));
    LOG("[quit] audio thread %s after %d ms\n", audio_parked ? "stopped" : "still running", (int)OSTicksToMilliseconds(OSGetSystemTime() - audio_asked));
    audio::shutdown();
    LOG("[quit] sound system shut down\n");
    wiiu::fs_begin(0xC001);
    rt_log_sync();  // the lines since the card thread's last look, these [quit] ones among them: they never reached the file before
    wiiu::fs_end();
    fflush(nullptr);
    leave_on_card();
}

// a crash shows what happened on the screen, so it can be reported: the addresses can be looked up against build/game.nm
static BOOL on_exception(OSContext *ctx, const char *what) {
    static char msg[512];
    const u32 *sp = (const u32 *)ctx->gpr[1];
    u32 lr1 = 0, lr2 = 0;
    if ((uintptr_t)sp > 0x10000000 && (uintptr_t)sp < 0x50000000) {
        const u32 *b1 = (const u32 *)sp[0];
        if ((uintptr_t)b1 > 0x10000000 && (uintptr_t)b1 < 0x50000000) {
            lr1 = b1[1];
            const u32 *b2 = (const u32 *)b1[0];
            if ((uintptr_t)b2 > 0x10000000 && (uintptr_t)b2 < 0x50000000) lr2 = b2[1];
        }
    }
    // the process is closing: threads that can't be stopped trip over what's been freed, and nothing is lost
    if (quitting)
        for (;;) OSYieldThread();
    // the same when the HOME Menu took GX2 from a GPU thread on_release couldn't stop in time: it just waits for the game to come back
    if (kernel::gx_hold && OSGetCurrentThread() == host_threads[1]) {
        ctx->srr0 = (u32)gpu_trip;
        return TRUE;
    }
    snprintf(msg, sizeof msg, "The game crashed: %s\npc %08x lr %08x\ncalled from %08x %08x\ndar %08x sp %08x\nframe %llu\n", what, ctx->srr0, ctx->lr, lr1, lr2, ctx->dar, ctx->gpr[1], (unsigned long long)g_frame);
    // the watchdog thread logs it: the exception's own stack is small, and this thread may hold the log's lock
    memcpy(crash_msg, msg, sizeof crash_msg);
    crash_pending = true;
    for (const OSTime until = OSGetSystemTime() + OSMillisecondsToTicks(4000); crash_pending && OSGetSystemTime() < until;) {}
    OSFatal(msg);
    return FALSE;
}
static BOOL on_dsi(OSContext *c) { return on_exception(c, "bad memory access"); }
static BOOL on_isi(OSContext *c) { return on_exception(c, "bad jump"); }
static BOOL on_program(OSContext *c) { return on_exception(c, "program exception"); }

extern "C" const char g_build_stamp[];  // tools/win/build3ds.py writes it at every link

int main(int argc, char **argv) {
    LOG("[loader] build %s\n", g_build_stamp);
    g_argv = argv;
    OSInitMutex(&file_m);
    OSInitMutex(&romfs_m);
    trace_start = OSGetSystemTime();
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
    // both end in abort otherwise, which on a console stops everything with nothing on the screen
    std::set_new_handler([] { report_stuck("Out of memory."); });
    std::set_terminate([] { report_stuck("The game stopped: terminate."); });
    g_fatal_hook = [](const char *m) {
        let_card_catch_up();
        OSFatal(m);
    };
    host_threads[0] = OSGetCurrentThread();
    trace_stage = "starting the watchdog";
    spawn(watchdog_thread, 0, 13, 64 << 10, "watchdog");
    if (kMeasure) spawn(wiiu::net_thread, 0, 12, 64 << 10, "net");
    trace_stage = "system libraries";
    WHBProcInit();
    // WHB registers its own at priority 100 from the GPU thread, on core 2. Each core has its own list, highest first: the GPU stops before WHB's goes, and starts after WHB's has made the screens again.
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_RELEASE, on_release, nullptr, 1000, 2);
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_ACQUIRE, on_acquire, nullptr, 50, 2);
    VPADInit();
    OSEnableHomeButtonMenu(TRUE);
    trace_stage = "save storage";
    wiiu::save_mount();
    stage("system up");
    std::error_code no_save;
    g_save_base = wiiu::save_folder();
    std::filesystem::create_directories(g_save_base, no_save);
    // better now than after hours of play that couldn't be kept
    if (no_save) report_stuck("Can't use this game's save storage on the console.");
    OSSetThreadPriority(OSGetCurrentThread(), 14);
    mem::init();
    stage("memory up");
    load_game(CONTENT_DIR);
    stage("game loaded");
    std::filesystem::create_directories(kernel::save_root(), no_save);
    rt_log_open(wiiu::data_dir() + "/log.txt");
    if (FILE *f = fopen((wiiu::data_dir() + "/logsync").c_str(), "rb")) {  // debug aid: each line to the card as it's logged (core.cpp)
        fclose(f);
        extern bool g_log_each_line;
        g_log_each_line = true;
        rt_log_sync();
    }
    if (FILE *f = fopen((wiiu::data_dir() + "/verbose").c_str(), "rb")) {  // debug aid: 1 (or empty) logs file opens and service errors, 2 every service call
        const int c = fgetc(f);
        g_verbose = c >= '1' && c <= '9' ? c - '0' : 1;
        fclose(f);
    }
    if (wiiu::test_run()) LOG("[test] a test run from the PC: this boot has a save of its own\n");
    // debug aid: "env.txt" in the data folder sets the runtime's own test switches, a NAME=value on each line. RECOMP_ROMFS_SWAP=<file>=<other>
    // (services/fs.cpp) loads a level in place of the one a test route enters: a tower or a castle no route reaches by playing.
    if (FILE *f = kMeasure ? fopen((wiiu::data_dir() + "/env.txt").c_str(), "r") : nullptr) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (char *eq = strchr(line, '=')) {
                *eq = 0;
                setenv(line, eq + 1, 1);
                LOG("[test] %s is %s\n", line, eq + 1);
            }
        }
        fclose(f);
    }
    LOG("[wiiu] guest memory at %p\n", g_mem);
    settings_load();
    // The console's Miis, one per account: the game finds them in Mii Maker and as the player's Mii (services/misc.cpp, fs.cpp). On since
    // the user's yes on 2026-11-08 ("Yes, next build"); a `nowiimiis` flag turns it off, and the games then get the stand-in Mii as before.
    if (!wiiu::debug_flag("nowiimiis") && nn::act::Initialize().IsSuccess()) {
        const nn::act::SlotNo me = nn::act::GetSlotNo();
        for (nn::act::SlotNo s = 1; s <= 12; s++) {
            FFLStoreData d;
            if (!nn::act::IsSlotOccupied(s) || nn::act::GetMiiEx(&d, s).IsFailure()) continue;
            std::array<u8, 0x60> m;
            memcpy(m.data(), &d, 0x60);
            std::array<u8, 0x60> usable;
            LOG("[wiiu] account %u's Mii: %02x %02x %02x %02x, %s%s\n", s, m[0], m[1], m[2], m[3], kernel::mii_3ds(m, usable) ? "usable" : "not usable",
                s == me ? ", the current account" : "");
            if (s == me) kernel::g_host_my_mii = (int)kernel::g_host_miis.size();
            kernel::g_host_miis.push_back(m);
        }
        nn::act::Finalize();
    }
    // Test aid: a Mii's store data (0x60 bytes) in a file named "test_mii.bin" in the data folder is taken as the current account's,
    // for Cemu, whose own account Mii is a placeholder
    if (FILE *f = fopen((wiiu::data_dir() + "/test_mii.bin").c_str(), "rb")) {
        std::array<u8, 0x60> m;
        if (fread(m.data(), 1, 0x60, f) == 0x60) {
            kernel::g_host_my_mii = (int)kernel::g_host_miis.size();
            kernel::g_host_miis.push_back(m);
            LOG("[wiiu] test Mii from test_mii.bin\n");
        }
        fclose(f);
    }
    kernel::init_services();
    stage("services up");
    frontend_init();
    stage("frontend up");
    kernel::BootInfo bi{g_game.text_addr, g_game.stack_size, g_game.priority, g_game.program_id};
    kernel::start(bi);
    stage("game started");
    frontend_run();
    WHBProcShutdown();
    _exit(0);
    return 0;
}
