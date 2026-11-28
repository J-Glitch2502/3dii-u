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
#include <coreinit/userconfig.h>
#include <gx2/event.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <sysapp/title.h>
#include <sys/stat.h>
#include <malloc.h>
#include <nn/act.h>
#include <nn/save/save.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <unistd.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>

#include <algorithm>
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
#include "menu_shortcut.h"
#include "network_menu.h"
#include "network_keyboard.h"
#include "network_settings.h"
#include "wup_update_guard.h"
#include "motion_input.h"
#include "service.h"
#include "services/services.h"
#include "settings.h"
#include "save_backup.h"
#include "wiiu.h"
#include "physical_file.h"
#include "shader_compile.h"
#include "checkpoint.h"

static const char *const CONTENT_DIR = "fs:/vol/content";
void rt_log_sync();
static volatile bool want_log_sync, want_dump, crash_pending;
static volatile u32 want_hang_dump, card_passes;  // want_hang_dump: seconds without drawing, plus one
static volatile u32 idle_samples, all_samples;
static char crash_msg[512];

// How start-up went, in the log: only memory until the PC or the end of the game takes it
static OSTime trace_start;
static const char *volatile trace_stage = "before main";

// (measuring builds) the start, step by step, in sd:/3dii-u/<title id>/start.txt, each line written and closed at once from the first line of
// main on, before the log is up: a title that stops starting (a console's error 199-9999 on a new title) still says how far it got
static void start_mark(const char *what) {
    static bool done;
    if (!kMeasure || done) return;
    static char path[80];
    if (!path[0]) {
        char dir[64];
        snprintf(dir, sizeof dir, "fs:/vol/external01/3dii-u/%016llx", (unsigned long long)OSGetTitleID());
        mkdir("fs:/vol/external01/3dii-u", 0777);
        mkdir(dir, 0777);
        snprintf(path, sizeof path, "%s/start.txt", dir);
        remove(path);
    }
    if (FILE *f = fopen(path, "a")) {
        fprintf(f, "%u ms %s\n", (u32)OSTicksToMilliseconds(OSGetSystemTime() - trace_start), what);
        fclose(f);
    }
    done = !strcmp(what, "the game is drawing");
}

static void stage(const char *name) {
    trace_stage = name;
    const u32 elapsed = OSTicksToMilliseconds(OSGetSystemTime() - trace_start);
    OSReport("[3dii-u startup] %s at %u ms\n", name, elapsed);
    LOG("[stage] %s at %u ms\n", name, elapsed);
    start_mark(name);
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
    const OSTime wait_started = kMeasure ? OSGetSystemTime() : 0;
    OSLockMutex(&file_m);
    const OSTime acquired = OSGetSystemTime();
    file_since = acquired;
    file_op = op;
    if (kMeasure) {
        const u32 waited = (u32)OSTicksToMilliseconds(acquired - wait_started);
        if (waited >= 16) LOG("[fs] operation %04x waited %u ms for storage\n", op, waited);
    }
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
    if (key == "menu") wiiu::menu_input(0, true);
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
    const OSThreadAttributes attr = core < 0 ? OS_THREAD_ATTRIB_AFFINITY_ANY : core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : core == 1 ? OS_THREAD_ATTRIB_AFFINITY_CPU1 : OS_THREAD_ATTRIB_AFFINITY_CPU2;
    if (!OSCreateThread(t, fn, 0, nullptr, (u8 *)stack + stack_size, stack_size, prio, attr)) FATAL("cannot create thread %s\n", name);
    OSSetThreadName(t, name);
    OSResumeThread(t);
    return t;
}

static volatile bool quitting, gpu_parked, audio_parked, gfx_may_close, gpu_done;
static OSEvent video_job, video_done;
static OSMutex video_mutex;
static void (*video_work)(void *);
static void *video_context;
static std::atomic<bool> video_worker_enabled{false};

void wiiu::set_video_worker(bool enabled) { video_worker_enabled.store(enabled); }

static int video_thread(int, void *) {
    for (;;) {
        OSWaitEvent(&video_job);
        video_work(video_context);
        OSSignalEvent(&video_done);
    }
    return 0;
}

bool wiiu::video_conversion_worker(void (*work)(void *), void *context) {
    if (!video_worker_enabled.load()) return false;
    // Called outside the guest kernel lock. The caller's context stays alive
    // until conversion completes; serialize requests before publishing it.
    OSLockMutex(&video_mutex);
    video_work = work;
    video_context = context;
    OSSignalEvent(&video_job);
    OSWaitEvent(&video_done);
    OSUnlockMutex(&video_mutex);
    return true;
}
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
    wiiu::camera_pause(true);
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
    wiiu::camera_pause(false);
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

// Opt-in hardware sample. Keep timing and reporting on the GPU thread.
static bool perf_trace;
static bool cpu_summary;  // per-title diagnostic marker; heavy kernel tracing remains opt-in
static std::atomic<bool> av_timing{false};
void wiiu::set_av_timing(bool on) { av_timing.store(on, std::memory_order_relaxed); }
bool wiiu::av_timing_enabled() { return av_timing.load(std::memory_order_relaxed); }
static std::atomic<OSTime> diagnostics_deadline{0};
void wiiu::set_diagnostics_level(u32 level) {
    level = std::min(level, 3u);
    kernel::set_diagnostics_level(level);
    set_av_timing(level != 0);
    gfx_set("cputime", level >= 2 || (cpu_summary && level != 0));
    gfx_set("gputime", level >= 3);
    diagnostics_deadline.store(level >= 2 ? OSGetSystemTime() + OSSecondsToTicks(10) : 0);
    LOG("[diag] level %u: summaries %u, all-thread/SVC/CPU timing %u, GPU stamps %u; detailed capture expires after 10 seconds\n",
        level, level != 0, level >= 2, level >= 3);
}

static void report_gpu_timing(OSTime begin) {
    static OSTime start, last_flip;
    static u64 previous_flips;
    static u32 count, longest_gap, longest_pump;
    const OSTime now = OSGetSystemTime();
    const u64 flips = wiiu::gfx_flips();
    if (!start) { start = last_flip = now; previous_flips = flips; return; }
    if (flips != previous_flips) {
        if (last_flip)
            longest_gap = std::max(longest_gap, (u32)OSTicksToMicroseconds(now - last_flip));
        last_flip = now;
        count += (u32)(flips - previous_flips);
        previous_flips = flips;
    }
    longest_pump = std::max(longest_pump, (u32)OSTicksToMicroseconds(now - begin));
    if (now - start < OSSecondsToTicks(2)) return;
    const auto elapsed = (u32)OSTicksToMilliseconds(now - start);
    LOG("[perf] presents=%u window_ms=%u max_present_gap_us=%u max_gpu_work_us=%u\n",
        count, elapsed, longest_gap, longest_pump);
    OSReport("[3dii-u perf] presents=%u window_ms=%u max_present_gap_us=%u max_gpu_work_us=%u\n",
        count, elapsed, longest_gap, longest_pump);
    start = now;
    count = longest_gap = longest_pump = 0;
}

static int gpu_thread(int, void *) {
    wiiu::profile_this_core();
    wiiu::gfx_thread_init();
    stage("gpu thread ready");
    setjmp(gpu_restart);
    bool drained = false;
    while (!quitting) {
        const OSTime deadline = diagnostics_deadline.load();
        if (deadline && OSGetSystemTime() >= deadline) {
            // Expensive probes run only for a bounded sample. The summaries
            // stay active so the before/after measurements remain available.
            wiiu::set_diagnostics_level(1);
            kernel::diagnostics_report();
            wiiu::gfx_report();
        }
        if (gpu_pause) {
            if (gpu_tripped) LOG("[home] the GPU thread was still drawing when GX2 went away\n");
            else { wiiu::gfx_shutdown(); drained = true; }
            gpu_tripped = false;
            gpu_parked = true;
            while (gpu_pause && !quitting) OSSleepTicks(OSMillisecondsToTicks(2));
            if (quitting) break;
            wiiu::gfx_reacquire();
            gpu_parked = false;
            drained = false;
            continue;
        }
        // quiet: two milliseconds went by with nothing asked of the GPU, and nothing came in at the last moment either, which is when a
        // finished frame may be shown
        const bool waited = !OSWaitEventWithTimeout(&gpu_event, 2000000);  // nanoseconds
        const u64 done = kernel::gx_stats.cmdlists + kernel::gx_stats.transfers;
        const bool timed = perf_trace || av_timing.load(std::memory_order_relaxed);
        const OSTime t0 = (kMeasure || timed) ? OSGetSystemTime() : 0;
        {
            checkpoint::Scope state_lock(checkpoint::gate());
            kernel::gsp_process_gx();
            if (!gpu_pause) wiiu::gfx_pump(waited && done == kernel::gx_stats.cmdlists + kernel::gx_stats.transfers);
        }
#ifdef RECOMP_RESUMABLE
        checkpoint::pump(); // never upgrade a reader into an exclusive capture
#endif
        if (kMeasure || timed) wiiu::gfx_note_busy((u32)OSTicksToMicroseconds(OSGetSystemTime() - t0));
        if (timed) report_gpu_timing(t0);
        if (cpu_summary && av_timing.load(std::memory_order_relaxed)) {
            static OSTime report_at;
            const OSTime now = OSGetSystemTime();
            if (now >= report_at) {
                wiiu::gfx_report();
                report_at = now + OSSecondsToTicks(2);
            }
        }
    }
    if (!gpu_parked) { wiiu::gfx_shutdown(); drained = true; }
    if (kMeasure && drained) wiiu::gfx_report_final();
    gpu_parked = true;
    // GX2 has to close on the core it started on, once the system has taken the foreground back
    while (!gfx_may_close) OSSleepTicks(OSMillisecondsToTicks(2));
    WHBGfxShutdown();
    gpu_done = true;
    return 0;
}

// A Wii U Pro Controller's buttons, as the GamePad's are mapped (map_buttons)
static u32 map_pro(u32 h, bool z_too) {
    u32 b = 0;
    if (h & WPAD_PRO_BUTTON_A) b |= 1u << 0;
    if (h & WPAD_PRO_BUTTON_B) b |= 1u << 1;
    if (h & WPAD_PRO_BUTTON_MINUS) b |= 1u << 2;
    if (h & WPAD_PRO_BUTTON_PLUS) b |= 1u << 3;
    if (h & WPAD_PRO_BUTTON_RIGHT) b |= 1u << 4;
    if (h & WPAD_PRO_BUTTON_LEFT) b |= 1u << 5;
    if (h & WPAD_PRO_BUTTON_UP) b |= 1u << 6;
    if (h & WPAD_PRO_BUTTON_DOWN) b |= 1u << 7;
    if (h & (WPAD_PRO_BUTTON_R | (z_too ? WPAD_PRO_BUTTON_ZR : 0))) b |= 1u << 8;
    if (h & (WPAD_PRO_BUTTON_L | (z_too ? WPAD_PRO_BUTTON_ZL : 0))) b |= 1u << 9;
    if (h & WPAD_PRO_BUTTON_X) b |= 1u << 10;
    if (h & WPAD_PRO_BUTTON_Y) b |= 1u << 11;
    return b;
}

// Its buttons as the GamePad's for the options menu, keyboard and Mii selector.
static u32 pro_as_gamepad(u32 t) {
    u32 v = 0;
    if (t & WPAD_PRO_BUTTON_STICK_L) v |= VPAD_BUTTON_STICK_L;
    if (t & WPAD_PRO_BUTTON_STICK_R) v |= VPAD_BUTTON_STICK_R;
    if (t & WPAD_PRO_TRIGGER_ZL) v |= VPAD_BUTTON_ZL;
    if (t & WPAD_PRO_TRIGGER_ZR) v |= VPAD_BUTTON_ZR;
    if (t & WPAD_PRO_TRIGGER_L) v |= VPAD_BUTTON_L;
    if (t & WPAD_PRO_TRIGGER_R) v |= VPAD_BUTTON_R;
    if (t & WPAD_PRO_BUTTON_A) v |= VPAD_BUTTON_A;
    if (t & WPAD_PRO_BUTTON_B) v |= VPAD_BUTTON_B;
    if (t & WPAD_PRO_BUTTON_X) v |= VPAD_BUTTON_X;
    if (t & WPAD_PRO_BUTTON_Y) v |= VPAD_BUTTON_Y;
    if (t & WPAD_PRO_BUTTON_PLUS) v |= VPAD_BUTTON_PLUS;
    if (t & WPAD_PRO_BUTTON_MINUS) v |= VPAD_BUTTON_MINUS;
    if (t & WPAD_PRO_BUTTON_UP) v |= VPAD_BUTTON_UP;
    if (t & WPAD_PRO_BUTTON_DOWN) v |= VPAD_BUTTON_DOWN;
    if (t & WPAD_PRO_BUTTON_LEFT) v |= VPAD_BUTTON_LEFT;
    if (t & WPAD_PRO_BUTTON_RIGHT) v |= VPAD_BUTTON_RIGHT;
    return v;
}

static u32 overlay_combo;

static void load_overlay_combo() {
    constexpr menu_shortcut::Button buttons[] = {
        {"L", VPAD_BUTTON_L}, {"R", VPAD_BUTTON_R}, {"ZL", VPAD_BUTTON_ZL}, {"ZR", VPAD_BUTTON_ZR},
        {"L Stick", VPAD_BUTTON_STICK_L}, {"R Stick", VPAD_BUTTON_STICK_R},
        {"A", VPAD_BUTTON_A}, {"B", VPAD_BUTTON_B}, {"X", VPAD_BUTTON_X}, {"Y", VPAD_BUTTON_Y},
        {"Plus", VPAD_BUTTON_PLUS}, {"Minus", VPAD_BUTTON_MINUS}, {"D-Pad Up", VPAD_BUTTON_UP},
        {"D-Pad Down", VPAD_BUTTON_DOWN}, {"D-Pad Left", VPAD_BUTTON_LEFT}, {"D-Pad Right", VPAD_BUTTON_RIGHT},
    };
    if (FILE *f = fopen((std::string(CONTENT_DIR) + "/menu_shortcut.txt").c_str(), "rb")) {
        char data[40];
        const size_t size = fread(data, 1, sizeof data, f);
        if (!ferror(f) && size < sizeof data) {
            std::string_view chord(data, size);
            while (!chord.empty() && (chord.back() == '\n' || chord.back() == '\r')) chord.remove_suffix(1);
            overlay_combo = menu_shortcut::parse(chord, buttons);
        }
        fclose(f);
    }
}

static bool overlay_pressed(u32 down, u32 held) {
    if (overlay_combo) return menu_shortcut::pressed(overlay_combo, down, held);
    return (down & VPAD_BUTTON_STICK_L) || menu_shortcut::pressed(VPAD_BUTTON_ZL | VPAD_BUTTON_ZR, down, held);
}

// 3DS button bits as hid.cpp expects them: A B Select Start Right Left Up Down R L X Y
static u32 map_buttons(u32 h, bool z_too) {
    u32 b = 0;
    if (h & VPAD_BUTTON_A) b |= 1u << 0;
    if (h & VPAD_BUTTON_B) b |= 1u << 1;
    if (h & VPAD_BUTTON_MINUS) b |= 1u << 2;
    if (h & VPAD_BUTTON_PLUS) b |= 1u << 3;
    if (h & VPAD_BUTTON_RIGHT) b |= 1u << 4;
    if (h & VPAD_BUTTON_LEFT) b |= 1u << 5;
    if (h & VPAD_BUTTON_UP) b |= 1u << 6;
    if (h & VPAD_BUTTON_DOWN) b |= 1u << 7;
    if (h & (VPAD_BUTTON_R | (z_too ? VPAD_BUTTON_ZR : 0))) b |= 1u << 8;
    if (h & (VPAD_BUTTON_L | (z_too ? VPAD_BUTTON_ZL : 0))) b |= 1u << 9;
    if (h & VPAD_BUTTON_X) b |= 1u << 10;
    if (h & VPAD_BUTTON_Y) b |= 1u << 11;
    return b;
}

// whether the last read found a GamePad, which the keyboard and the Mii list are drawn on
static std::atomic<bool> gamepad_there{true};

// Without a GamePad the game gets the player's Mii at once, as it does from ports without a list (services/apt.cpp)
bool mii_select_shown() { return gamepad_there; }

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
    const bool hold_mii_picker = wiiu::debug_flag("hold_mii_picker");  // scripted capture of the picker itself
    // Pro Controllers beside the GamePad: what either holds counts, and the stick pushed further is the circle pad
    KPADInit();
    WPADEnableURCC(TRUE);
    u32 tick = 0;
    bool touch_was = false, leaving = false;
    VPADStatus st{};
    VPADTouchData cal{};
    u32 buttons = 0, pad_held = 0, pro_hold[4] = {};
    float pad_x = 0, pad_y = 0, pro_x[4] = {}, pro_y[4] = {};
    float pad_rx = 0, pad_ry = 0, pro_rx[4] = {}, pro_ry[4] = {};  // the right sticks: the C-stick
    s16 cx = 0, cy = 0;
    bool touch = false;
    u16 tx = 0, ty = 0;
    u16 network_px=0,network_py=0;
    bool network_touch=false;
    for (;;) {
        u32 presses = 0;  // the GamePad's way, from either, for the options menu
        bool toggle_menu = false;
        float network_ry = pad_ry;
        bool tap = false;  // a touch on the GamePad began, at px, py
        u16 px = 0, py = 0;
        VPADReadError err = VPAD_READ_SUCCESS;
        VPADRead(VPAD_CHAN_0, &st, 1, &err);
        gamepad_there = err != VPAD_READ_INVALID_CONTROLLER;
        if (err == VPAD_READ_INVALID_CONTROLLER) {  // no GamePad: nothing held on it
            pad_held = 0;
            pad_x = pad_y = pad_rx = pad_ry = 0;
            touch = touch_was = false;
            network_touch=false;
            wiiu::mic_blow(false);
        }
        if (err == VPAD_READ_SUCCESS) {
            pad_held = st.hold;
            pad_rx = st.rightStick.x;
            pad_ry = st.rightStick.y;
            wiiu::network_keyboard::input(st);
            network_ry = st.rightStick.y;
            presses |= st.trigger;
            toggle_menu = overlay_pressed(st.trigger, st.hold);
            wiiu::mic_blow(st.hold & VPAD_BUTTON_STICK_R);
            pad_x = st.leftStick.x;
            pad_y = st.leftStick.y;
            s16 accel[3], gyro[3];
            wiiu::motion_input(st.accelerometer.acc, st.gyro, accel, gyro);
            kernel::hid_motion(accel, gyro);
            touch = false;
            bool down = false;
            if (st.tpNormal.touched && !st.tpNormal.validity) {
                VPADGetTPCalibratedPoint(VPAD_CHAN_0, &cal, &st.tpNormal);
                px = (u16)(cal.x * 854 / 1280);
                py = (u16)(cal.y * 480 / 720);
                down = true;
                wiiu::touch_to_bottom(px, py, touch, tx, ty);
            }
            if(down){network_px=px;network_py=py;}
            network_touch = down;
            tap = down && !touch_was;
            touch_was = down;
        }
        const bool z_too = !kernel::extra_pad_connected();
        buttons = map_buttons(pad_held, z_too);
        float sx = pad_x, sy = pad_y;
        for (int ch = 0; ch < 4; ch++) {
            KPADStatus ks{};
            KPADError kerr = KPAD_ERROR_OK;
            const bool read = KPADReadEx((KPADChan)ch, &ks, 1, &kerr) > 0 && kerr == KPAD_ERROR_OK;
            if (read && ks.extensionType == WPAD_EXT_PRO_CONTROLLER) {
                pro_hold[ch] = ks.pro.hold;
                pro_x[ch] = ks.pro.leftStick.x;
                pro_y[ch] = ks.pro.leftStick.y;
                pro_rx[ch] = ks.pro.rightStick.x;
                pro_ry[ch] = ks.pro.rightStick.y;
                if(std::abs(pro_ry[ch])>std::abs(network_ry))network_ry=pro_ry[ch];
                const u32 down = pro_as_gamepad(ks.pro.trigger);
                presses |= down;
                toggle_menu |= overlay_pressed(down, pro_as_gamepad(pro_hold[ch]));
            } else if (read || kerr == KPAD_ERROR_INVALID_CONTROLLER) {  // another kind of controller there, or none
                pro_hold[ch] = 0;
                pro_x[ch] = pro_y[ch] = pro_rx[ch] = pro_ry[ch] = 0;
            }
            buttons |= map_pro(pro_hold[ch], z_too);
            if (std::fabs(pro_x[ch]) + std::fabs(pro_y[ch]) > std::fabs(sx) + std::fabs(sy)) {
                sx = pro_x[ch];
                sy = pro_y[ch];
            }
        }
        float rx = pad_rx, ry = pad_ry;
        bool zl = pad_held & VPAD_BUTTON_ZL, zr = pad_held & VPAD_BUTTON_ZR, r = pad_held & VPAD_BUTTON_R;
        for (int ch = 0; ch < 4; ch++) {
            if (std::fabs(pro_rx[ch]) + std::fabs(pro_ry[ch]) > std::fabs(rx) + std::fabs(ry)) {
                rx = pro_rx[ch];
                ry = pro_ry[ch];
            }
            zl |= (pro_hold[ch] & WPAD_PRO_BUTTON_ZL) != 0;
            zr |= (pro_hold[ch] & WPAD_PRO_BUTTON_ZR) != 0;
            r |= (pro_hold[ch] & WPAD_PRO_BUTTON_R) != 0;
        }
        cx = (s16)(sx * 156.0f);
        cy = (s16)(sy * 156.0f);
        const bool menu_was_open = wiiu::menu_open();
        if(wiiu::network_menu::active()) {
            static OSTime last_network_input;
            const auto now=OSGetSystemTime();
            float seconds=last_network_input?OSTicksToMicroseconds(now-last_network_input)/1000000.f:0.016f;
            last_network_input=now;
            wiiu::network_menu::pointer(network_touch,network_px,network_py,network_ry,seconds);
        }
        wiiu::menu_input(presses, toggle_menu);
        const bool menu_took_input = menu_was_open || wiiu::menu_open();
        if (!menu_took_input || wiiu::network_keyboard::active()) {
            wiiu::keyboard_input(presses, tap, px, py);
            wiiu::mii_selector_input(presses, tap, px, py);
        }
        if (!leaving && wiiu::menu_wants_quit()) {
            leaving = true;
            SYSLaunchMenu();
        }
        if (autoplay || kernel::g_live_path || wiiu::test_run()) {
            tick++;
            if (tick % 500 == 0 && wiiu::keyboard_open()) {
                wiiu::keyboard_input(VPAD_BUTTON_PLUS, false, 0, 0);
                if (wiiu::keyboard_open()) wiiu::keyboard_input(0, true, 117, 250);  // a letter or the first digit
            }
            if (!hold_mii_picker && wiiu::mii_selector_open()) wiiu::mii_selector_input(VPAD_BUTTON_A, false, 0, 0);
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
        if (menu_took_input) {
            buttons = 0;
            cx = cy = 0;
            touch = false;
            kernel::hid_extra_pad(0, 0, false, false, false);
        }
        else kernel::hid_extra_pad(rx, ry, zl, zr, r);
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
    OSTime timing_report = round;
    OSTime timing_away = 0, timing_making = 0;
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
        timing_away = std::max(timing_away, now - round);
        round = now;
        if (av_timing.load(std::memory_order_relaxed) && now - timing_report >= OSSecondsToTicks(2)) {
            LOG("[audio-time] max wake gap %u us; max DSP tick %u us\n",
                (u32)OSTicksToMicroseconds(timing_away), (u32)OSTicksToMicroseconds(timing_making));
            audio::report_output_timing();
            audio::report_dsp_timing();
            timing_report = now;
            timing_away = timing_making = 0;
        }
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
        const OSTime tick_begin = OSGetSystemTime();
        kernel::dsp_tick();
        const OSTime tick_time = OSGetSystemTime() - tick_begin;
        making = std::max(making, tick_time);
        timing_making = std::max(timing_making, tick_time);
        if (gaps && now - gaps_logged > (OSTime)OSSecondsToTicks(1)) {
            LOG("[audio] %u gaps: the sound ran out before the next frame of it was made (this thread was away for up to %u ms, a frame took up to %u ms)\n", gaps,
                (u32)OSTicksToMilliseconds(away), (u32)OSTicksToMilliseconds(making));
            gaps = 0;
            away = making = 0;
            gaps_logged = now;
        }
        // Give the guest time to submit between frames when AX needs several at once.
        OSSleepTicks(OSMillisecondsToTicks(1));
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
            OSReport("[3dii-u crash] %s\n", crash_msg);
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
        const u32 asking = kernel::svcs + (u32)kernel::gx_stats.transfers + wiiu::files_copied;  // and saves copied as it starts (save_copy_tree, save_from_content)
        quiet = asking == asked ? quiet + 1 : 0;
        asked = asking;
        if (++seconds >= 41 && !drawn && (quiet >= 30 || seconds >= 180) && !wiiu::net_connected()) report_stuck("The game got stuck starting up.");
        const u64 lists = kernel::gx_stats.cmdlists;
        if (lists != last || gpu_pause || wiiu::menu_open() || wiiu::keyboard_open() || wiiu::mii_selector_open()) {
            if (lists && !drawn) {
                drawn = true;
                stage("the game is drawing");
            }
            last = lists;
            still = 0;
            next_dump = 10;
        } else if (drawn) {
            if (++still == next_dump) {
                OSReport("[3dii-u watchdog] no new GX2 lists for %u s, frame=%llu, lists=%llu, shown=%llu, file op=%x, shaders=%08x/%08x\n",
                    still, (unsigned long long)g_frame, (unsigned long long)lists,
                    (unsigned long long)wiiu::gfx_flips(), (u32)file_op, wiiu::last_vs, wiiu::last_ps);
                next_dump *= 2;
                want_hang_dump = still + 1;
            }
            if (still >= 30 && (quiet >= 30 || still >= 180) && !wiiu::net_connected()) report_stuck("The game has frozen.");
        }
    }
    return 0;
}

volatile bool wiiu::settings_due;
wiiu::save_backup::Requests wiiu::save_backup::requests;

// The log of a measuring build played on a console: on the SD card, sd:/3dii-u/<title id>/log.txt, for the PC to read, kept up to date every
// 10 s (card_thread) so a freeze leaves what led up to it. In storage of its own in a test run the PC drives (live.txt), or with no card to write.
static std::string log_path() {
    const std::string own = wiiu::data_dir() + "/log.txt";
    struct stat st;
    if (!kMeasure || stat((wiiu::data_dir() + "/live.txt").c_str(), &st) == 0) return own;
    char dir[64];
    snprintf(dir, sizeof dir, "fs:/vol/external01/3dii-u/%016llx", (unsigned long long)OSGetTitleID());
    wiiu::fs_begin(0xC00D);
    mkdir("fs:/vol/external01/3dii-u", 0777);
    mkdir(dir, 0777);
    const bool there = stat(dir, &st) == 0;
    wiiu::fs_end();
    return there ? std::string(dir) + "/log.txt" : own;
}

// Slow work for later, at the lowest priority on its core: flushing saves, and the log, which only reaches storage when the game ends: the PC and the report on the screen don't need it.
static int card_thread(int, void *) {
    for (u32 pass = 1;; pass++) {
        OSSleepTicks(OSMillisecondsToTicks(200));
        checkpoint::Scope state_lock(checkpoint::gate());
        wiiu::gfx_report_draw_trace();
        const auto backup = wiiu::save_backup::requests.take();
        if (backup != wiiu::save_backup::Action::None) {
            std::string message;
            const bool ok = backup == wiiu::save_backup::Action::Export ? wiiu::save_backup::export_to_card(message) : wiiu::save_backup::stage_restore(message);
            LOG("[save backup] %s: %s\n", ok ? "done" : "not completed", message.c_str());
            wiiu::save_backup::requests.complete(message);
        }
        if (want_hang_dump) {
            const u32 seconds = want_hang_dump - 1;
            want_hang_dump = 0;
            if (perf_trace && seconds) {
                OSReport("[3dii-u perf] no new GPU command lists for %u s\n", seconds);
                wiiu::log_status();
                rt_log_sync();
            }
            dump_hang(seconds);
            if (perf_trace && seconds) rt_log_sync();
        }
        if (wiiu::settings_due) {  // before the flush below, and so before the one a quit asks for
            wiiu::settings_due = false;
            wiiu::fs_begin(0xC00E);
            settings_save();
            wiiu::fs_end();
            wiiu::save_dirty();
        }
        if (kMeasure && pass % 300 == 0) {
            const struct mallinfo m = mallinfo();
            LOG("[wiiu] memory: %u KB in use, %u KB taken from the system, top at %p\n", (u32)m.uordblks >> 10, (u32)m.arena >> 10, sbrk(0));
        }
        wiiu::save_flush(false);
        // a live.txt means a test run, likely in Cemu, which gets killed rather than quit
        if (want_log_sync || (kernel::g_live_path && pass % 150 == 0) || (kMeasure && pass % 50 == 0)) {
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

// A recovery report uses a separate, pre-opened physical SD file. No log mutex,
// redirected fopen, heap allocation, thread cancellation or forced lock release.
extern "C" { volatile u32 g_3dii_safe_fault_test = 0; }
extern "C" const char g_build_stamp[];
static wiiu::PhysicalFile recovery_file;
static std::atomic<u32> recovery_kind{0};
static std::atomic<bool> recovery_report_done{false};
static OSContext recovery_fault_context;
// Capture the faulting thread's own stack while it is stopped, including
// Aroma module stacks in mapped memory. Never follow an arbitrary address.
static u32 recovery_stack_count, recovery_stack_frames[32][3];
static u32 recovery_stack_low, recovery_stack_high;
static void recovery_capture_stack(const OSContext *ctx) {
    recovery_stack_count = 0;
    const auto *thread = OSGetCurrentThread();
    recovery_stack_low = (u32)thread->stackEnd;
    recovery_stack_high = (u32)thread->stackStart;
    u32 sp = ctx->gpr[1];
    while (recovery_stack_count < 32 && !(sp & 3) &&
           recovery_stack_high >= 8 && sp >= recovery_stack_low && sp <= recovery_stack_high - 8) {
        const auto *frame = reinterpret_cast<const u32 *>(sp);
        const u32 next = frame[0];
        auto *out = recovery_stack_frames[recovery_stack_count++];
        out[0] = sp; out[1] = next; out[2] = frame[1];
        if (next <= sp) break;
        sp = next;
    }
}
alignas(64) static u8 recovery_bytes[8192];
static_assert(320 + 5 * sizeof(OSContext) + 16 + sizeof recovery_stack_frames <= sizeof recovery_bytes);
static char recovery_path[192];
static bool recovery_file_open;

static void recovery_prepare() {
    snprintf(recovery_path, sizeof recovery_path,
        "/vol/external01/wiiu/3dii-u/reports/%016llx_%016llx.bin",
        (unsigned long long)g_game.program_id, (unsigned long long)OSGetSystemTime());
    recovery_file_open = recovery_file.open(recovery_path, "w");
    OSReport("[3dii-u safety] report storage %s: %s\n", recovery_file_open ? "ready" : "unavailable", recovery_path);
}

static void recovery_write_report(u32 kind) {
    memset(recovery_bytes, 0, sizeof recovery_bytes);
    memcpy(recovery_bytes, "3DSAFE1", 7);
    auto *words = (u32 *)recovery_bytes;
    words[2] = 2; words[3] = kind;
    words[4] = (u32)(g_game.program_id >> 32); words[5] = (u32)g_game.program_id;
    const u64 time = OSGetSystemTime(), frame = g_frame, lists = kernel::gx_stats.cmdlists;
    words[6] = (u32)(time >> 32); words[7] = (u32)time;
    words[8] = (u32)(frame >> 32); words[9] = (u32)frame;
    words[10] = (u32)(lists >> 32); words[11] = (u32)lists;
    words[13] = file_op; words[14] = wiiu::last_vs; words[15] = wiiu::last_ps;
    strncpy((char*)recovery_bytes + 64, g_build_stamp, 63);
    const char *reason = kind == 1 ? "SYNTHETIC fault requested by debugger" :
                         kind == 2 ? "physical shader-cache I/O stalled" : "CPU exception; native dump handler follows";
    strncpy((char*)recovery_bytes + 128, reason, 159);
    words[72] = sizeof(OSContext);
    unsigned contexts = 0;
    if (kind == 3) {
        memcpy(recovery_bytes + 320, &recovery_fault_context, sizeof(OSContext)); contexts++;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!host_threads[i]) continue;
        memcpy(recovery_bytes + 320 + contexts * sizeof(OSContext), &host_threads[i]->context, sizeof(OSContext));
        contexts++;
    }
    words[12] = contexts;
    unsigned size = 320 + contexts * sizeof(OSContext);
    if (kind == 3) {
        auto *stack = reinterpret_cast<u32 *>(recovery_bytes + size);
        stack[0] = 0x53544B31; // STK1: bounded native stack records
        stack[1] = recovery_stack_count;
        stack[2] = recovery_stack_low; stack[3] = recovery_stack_high;
        memcpy(stack + 4, recovery_stack_frames, recovery_stack_count * sizeof recovery_stack_frames[0]);
        size += 16 + recovery_stack_count * sizeof recovery_stack_frames[0];
    }
    words[73] = size;
    const bool saved = recovery_file_open && recovery_file.write(recovery_bytes, size) && recovery_file.flush();
    recovery_file.close();
    recovery_file_open = false;
    OSReport("[3dii-u safety] kind=%u frame=%llu contexts=%u report=%s path=%s\n", kind,
        (unsigned long long)frame, contexts, saved ? "SAVED" : "FAILED", recovery_path);
    recovery_report_done.store(true, std::memory_order_release);
}

static int recovery_thread(int, void*) {
    for (;;) {
        OSSleepTicks(OSMillisecondsToTicks(100));
        u32 kind = recovery_kind.load(std::memory_order_acquire);
        if (g_3dii_safe_fault_test == 1) { g_3dii_safe_fault_test = 0; kind = 1; }
        const bool cache_stalled = gpu::shader_cache_io_stalled(OSSecondsToTicks(4));
        if (!kind && cache_stalled) kind = 2;
        if (!kind) continue;
        recovery_write_report(kind);
        if (kind != 3) {
            OSReport("[3dii-u safety] requesting Wii U Menu; no fatal exception was raised\n");
            SYSLaunchMenu();
        }
        // Actual exceptions retain the native handler and its normal crash dump.
        // A wedged kernel/core cannot be guaranteed to honor a menu request.
        return 0;
    }
}

// the last of the saves and what led up to the end are still in memory: give the card thread a moment, but the screen comes first. A save the game made that isn't
// in storage yet gets as long as it takes, up to 30 s: the game showed the player it saved.
static void let_card_catch_up() {
    const u32 seen = card_passes;
    want_log_sync = true;
    const u32 ms = wiiu::save_pending() ? 30000 : 3000;
    for (const OSTime until = OSGetSystemTime() + OSMillisecondsToTicks(ms); card_passes == seen && OSGetSystemTime() < until;) OSSleepTicks(OSMillisecondsToTicks(5));
}

// Whether what a database slot held, as a 3DS game would read it, is a Mii: a name of printable characters, an author's name of them, the
// padding empty, a birthday and a favourite colour that exist, a height and a build within their 128
static bool looks_like_mii(const std::array<u8, 0x60> &m) {
    const u32 about = m[0x18] | m[0x19] << 8;
    if (m[0x16] || m[0x17] || (about >> 1 & 15) > 12 || (about >> 10 & 15) > 11 || m[0x2E] > 128 || m[0x2F] > 128) return false;
    for (u32 name : {0x1Au, 0x48u})
        for (u32 at = name; at < name + 20; at += 2) {
            const u32 c = m[at] | m[at + 1] << 8;
            if (!c) break;
            if (c < 0x20 || (c >= 0xDC00 && c < 0xE000) || c >= 0xFFFE) return false;
            if (c >= 0xD800 && c < 0xDC00) {
                if (at + 2 == name + 20) return false;
                at += 2;
                const u32 low = m[at] | m[at + 1] << 8;
                if (low < 0xDC00 || low >= 0xE000) return false;
            }
        }
    return m[0x1A] || m[0x1B];
}

// FFLiDatabaseFileOfficial: big-endian FFOC, 3000 Official records, a regular-Mii list and a checksum at EOF.
// https://github.com/aboood40091/ffl/blob/master/include/nn/ffl/FFLiDatabaseFileOfficial.h
static bool read_mii_maker(FILE *f) {
    constexpr long size = 0x43840;
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) != size || fseek(f, 0, SEEK_SET) != 0) return false;
    std::vector<u8> db(size);
    if (fread(db.data(), 1, db.size(), f) != db.size() ||
        memcmp(db.data(), "FFOC", 4) || kernel::mii_crc16(db.data(), db.size()) != 0)
        return false;
    for (u32 slot = 0; slot < 3000; slot++) {
        std::array<u8, 0x60> m{};
        memcpy(m.data(), db.data() + 8 + slot * 0x5C, 0x5C);
        if (kernel::mii_id_empty(m.data() + 0x0C) || (m[0x0C] & 0x20)) continue;
        // Camera-created Wii U Miis can carry version 0. The 3DS expects version 3.
        if (m[3] != 3 && !(m[3] == 0 && (m[0] & 0x70) == 0x40)) continue;
        std::reverse(m.begin(), m.begin() + 4);
        m[0] = 3;
        for (size_t at = 0x18; at < 0x5C; at += 2)
            if (at != 0x2E) std::swap(m[at], m[at + 1]);  // height and build are bytes
        if (!looks_like_mii(m)) continue;
        const u16 crc = kernel::mii_crc16(m.data(), 0x5E);
        m[0x5E] = (u8)(crc >> 8);
        m[0x5F] = (u8)crc;
        kernel::g_host_miis.push_back(m);
    }
    return true;
}

static void read_console_mii_maker() {
    // Resolve the shared path as FFL does. save_mount() has already initialized nn_save.
    const u64 maker = _SYSGetSystemApplicationTitleId(SYSTEM_APP_ID_MII_MAKER);
    OSDynLoad_Module save;
    if (!maker || OSDynLoad_Acquire("nn_save.rpl", &save) != OS_DYNLOAD_OK) return;
    decltype(&SAVEGetSharedSaveDataPath) shared_path = nullptr;
    OSDynLoad_FindExport(save, OS_DYNLOAD_EXPORT_FUNC, "SAVEGetSharedSaveDataPath", (void **)&shared_path);
    if (shared_path) {
        for (const char *name : {"db/FFL_ODB.dat", "db/FFL_ODB_OLD.dat"}) {
            char path[256] = "fs:";
            if (shared_path(maker, name, path + 3, sizeof path - 3) != SAVE_STATUS_OK || path[3] != '/') continue;
            if (FILE *f = fopen(path, "rb")) {
                const size_t before = kernel::g_host_miis.size();
                const bool valid = read_mii_maker(f);
                fclose(f);
                if (valid) {
                    LOG("[wiiu] %s: %zu Mii(s) read\n", name, kernel::g_host_miis.size() - before);
                    break;
                }
                LOG("[wiiu] %s: invalid Mii database\n", name);
            }
        }
    }
    OSDynLoad_Release(save);
}

void frontend_run() {
    wiiu::profile_this_core();
    OSInitEvent(&video_job, FALSE, OS_EVENT_MODE_AUTO);
    OSInitEvent(&video_done, FALSE, OS_EVENT_MODE_AUTO);
    OSInitMutex(&video_mutex);
    spawn(video_thread, 0, 18, 256 << 10, "video converter");
    LOG("[diag] comprehensive capture available: set diag 0..3, diag, stats, ps, bounded mem, screenshots and renderer switches\n");
    if (FILE *f = fopen("fs:/vol/content/video-worker-enable", "rb")) {
        fclose(f);
        wiiu::set_video_worker(true);
        LOG("[video] conversion worker enabled from content marker\n");
    }
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
        if (!wiiu::menu_open() || wiiu::network_menu::active() || wiiu::network::connected()) {
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
    kernel::shutdown_room_uds();
    want_hang_dump = 1;  // a player who shuts down a frozen game shows what it was stuck on
    for (int i = 0; i < 100 && want_hang_dump; i++) OSSleepTicks(OSMillisecondsToTicks(10));
    let_card_catch_up();
    wiiu::profile_stop();
    quitting = true;
    gfx_may_close = true;
    // A GPU thread stuck in a draw kept the game from closing at all (Nintendogs' check, 2026-11-17: blocked in the vertex pool's lock). It gets
    // 2 s, as the sound does, and the game closes either way; GX2 then goes with the process, as when HOME gives up on it (on_release).
    for (int i = 0; i < 1000 && !gpu_done; i++) OSSleepTicks(OSMillisecondsToTicks(2));
    if (gpu_done) OSJoinThread(gpu, nullptr);
#ifdef RECOMP_RESUMABLE
    checkpoint::finish_pending_write();
#endif
    // The sound system is always shut down before the game goes: left running, its voices play from memory the system is about to take back, and
    // the Wii U Menu hangs as it starts (testers' NSMB2 and 3D Land, 2026-11-09). The audio thread used to get 100 ms to stop, and a late one
    // meant no shutdown at all, with nothing logged. A busy game's can be late, so it gets 2 s, and the shutdown happens either way.
    const OSTime audio_asked = OSGetSystemTime();
    for (int i = 0; i < 1000 && !audio_parked; i++) OSSleepTicks(OSMillisecondsToTicks(2));
    LOG("[quit] audio thread %s after %d ms\n", audio_parked ? "stopped" : "still running", (int)OSTicksToMilliseconds(OSGetSystemTime() - audio_asked));
    audio::shutdown();
    wiiu::camera_shutdown();
    LOG("[quit] sound system shut down\n");
    OSReport("[3dii-u quit] entering final log sync\n");
    wiiu::fs_begin(0xC001);
    rt_log_sync();  // the lines since the card thread's last look, these [quit] ones among them: they never reached the file before
    wiiu::fs_end();
    OSReport("[3dii-u quit] final log sync complete; flushing streams\n");
    fflush(nullptr);
    OSReport("[3dii-u quit] streams flushed; returning from frontend\n");
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
    memcpy(&recovery_fault_context, ctx, sizeof(OSContext));
    recovery_capture_stack(ctx);
    recovery_kind.store(3, std::memory_order_release);
    OSReport("[3dii-u crash] %s", msg);
    for (const OSTime until = OSGetSystemTime() + OSMillisecondsToTicks(1500);
         !recovery_report_done.load(std::memory_order_acquire) && OSGetSystemTime() < until;) {}
    return FALSE;  // native exception/crash-dump policy; never hide a real CPU fault

}
static BOOL on_dsi(OSContext *c) { return on_exception(c, "bad memory access"); }
static BOOL on_isi(OSContext *c) { return on_exception(c, "bad jump"); }
static BOOL on_program(OSContext *c) { return on_exception(c, "program exception"); }

extern "C" const char g_build_stamp[];  // tools/win/build3ds.py writes it at every link

int main(int argc, char **argv) {
    OSReport("[3dii-u startup] main entered, build %s\n", g_build_stamp);
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
    start_mark(g_build_stamp);
    stage("starting the watchdog");
    spawn(watchdog_thread, 0, 13, 64 << 10, "watchdog");
    stage("initializing system libraries");
    WHBProcInit();
    start_mark("system libraries up");
    // WHB registers its own at priority 100 from the GPU thread, on core 2. Each core has its own list, highest first: the GPU stops before WHB's goes, and starts after WHB's has made the screens again.
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_RELEASE, on_release, nullptr, 1000, 2);
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_ACQUIRE, on_acquire, nullptr, 50, 2);
    VPADInit();
    OSEnableHomeButtonMenu(TRUE);
    stage("mounting save storage");
    wiiu::save_mount();
    const auto update_match = wiiu::wup_update::check(CONTENT_DIR);
    if (update_match == wiiu::wup_update::Match::Invalid || update_match == wiiu::wup_update::Match::WrongBase)
        report_stuck("This WUP update does not match the installed base. Install the matching full base and update on the same device. Your saves have not been replaced.");
    start_mark(last_note);  // what the save storage said
    stage("system up");
    // A physical SD marker enables the lightweight console independently of the
    // per-frame profiling counters controlled by RECOMP_MEASURE.
    bool remote_console = kMeasure;
    {
        wiiu::PhysicalFile marker;
        remote_console |= marker.open("/vol/external01/wiiu/3dii-u/debug/console-enable", "r");
    }
    if (remote_console) {
        OSReport("[3dii-u debug] lightweight console enabled on TCP 4405\n");
        spawn(wiiu::net_thread, -1, 12, 64 << 10, "net");
    }
    std::error_code no_save;
    g_save_base = wiiu::save_folder();
    std::filesystem::create_directories(g_save_base, no_save);
    // better now than after hours of play that couldn't be kept
    if (no_save) report_stuck("Can't use this game's save storage on the console.");
    OSSetThreadPriority(OSGetCurrentThread(), 14);
    stage("initializing guest memory");
    mem::init();
    stage("memory up");
    stage("loading game content");
    load_game(CONTENT_DIR);
    stage("game loaded");
#ifdef RECOMP_RESUMABLE
    checkpoint::initialize_persistent();
#endif
#ifdef RECOMP_RESUMABLE
    if (FILE *state_check = fopen("fs:/vol/content/state-heap-check", "rb")) {
        fclose(state_check);
        checkpoint::set_heap_checks(true);
        OSReport("[state-heap] phased mapped-heap validation enabled\n");
    }
#endif
    recovery_prepare();
    spawn(recovery_thread, -1, 12, 128 << 10, "safety guard");
    std::filesystem::create_directories(kernel::save_root(), no_save);
    if (!wiiu::test_run()) {  // before services/guest file handles, including cached save files
        std::string message;
        const auto restored = wiiu::save_backup::take_staged_restore(message);
        if (!message.empty()) {
            LOG("[save backup] %s\n", message.c_str());
            wiiu::save_backup::requests.complete(message);
        }
        if (restored == wiiu::save_backup::Restore::Unsafe) report_stuck(message.c_str());
        // A pending explicit restore keeps the current save. A successful one marks save_imported.
        if (restored == wiiu::save_backup::Restore::None)
            wiiu::save_from_content(std::string(CONTENT_DIR) + "/save_import", kernel::save_root() + "/savedata", kernel::save_root() + "/save_imported");
    }
    struct stat no_menu;
    if (!wiiu::wup_update::menu_enabled(CONTENT_DIR, stat((std::string(CONTENT_DIR) + "/no_menu").c_str(), &no_menu) != 0)) wiiu::menu_disable();
    else load_overlay_combo();
    rt_log_open(log_path());
    perf_trace = wiiu::debug_flag("perftrace");
    // An immutable diagnostic release can enable timing before its opening
    // movie, without relying on a PC command arriving after boot.
    if (FILE *timing = fopen("fs:/vol/content/avtiming-enable", "rb")) {
        fclose(timing);
        wiiu::set_av_timing(true);
        LOG("[timing] audio/source and GPU-pump timing enabled by content marker\n");
    }
    if (FILE *timing = fopen("fs:/vol/content/cpu-timing-enable", "rb")) {
        fclose(timing);
        cpu_summary = true;
        wiiu::set_av_timing(true);
        wiiu::gfx_set("cputime", 1);
        LOG("[timing] CPU render phases and two-second summaries enabled by content marker; detailed thread/SVC capture remains bounded\n");
    }
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
    if (wiiu::debug_flag("showfps")) g_settings.show_fps = true;  // a test run's picture of the counter, as the menu's FPS COUNTER: ON gives it
    // the Wii U's language: a game takes it when its region has it, until the player picks another in the options menu (services/cfg.cpp)
    if (const UCHandle uc = UCOpen(); uc >= 0) {
        u32 language = 0;
        UCSysConfig c = {"cafe.language", 0x777, UC_DATATYPE_UNSIGNED_INT, 0, sizeof language, &language};
        if (UCReadSysConfig(uc, 1, &c) == 0 && c.error == 0) kernel::g_system_language = (int)language;
        UCClose(uc);
    }
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
    if (FILE *f = wiiu::debug_flag("cemu") ? fopen((wiiu::data_dir() + "/test_mii.bin").c_str(), "rb") : nullptr) {
        std::array<u8, 0x60> m;
        if (fread(m.data(), 1, 0x60, f) == 0x60) {
            kernel::g_host_my_mii = (int)kernel::g_host_miis.size();
            kernel::g_host_miis.push_back(m);
            LOG("[wiiu] test Mii from test_mii.bin\n");
        }
        fclose(f);
    }
    // The rest of the console's Miis are in Mii Maker's shared save. Cemu checks can supply a fixture using the same file format.
    if (FILE *f = wiiu::debug_flag("cemu") ? fopen((wiiu::data_dir() + "/test_mii_maker.bin").c_str(), "rb") : nullptr) {
        read_mii_maker(f);
        fclose(f);
    }
    if (!wiiu::debug_flag("nowiimiis")) read_console_mii_maker();
    kernel::init_services();
    stage("services up");
    frontend_init();
    stage("frontend up");
    kernel::BootInfo bi{g_game.text_addr, g_game.stack_size, g_game.priority, g_game.program_id};
    kernel::start(bi);
    stage("game started");
    frontend_run();
    OSReport("[3dii-u quit] entering WHBProcShutdown\n");
    WHBProcShutdown();
    OSReport("[3dii-u quit] WHBProcShutdown complete; exiting process\n");
    if (route_quit) OSExitThread(0);  // Cemu 2.6 spins in process exit, which prevents the runner from closing it.
    _exit(0);
    return 0;
}
