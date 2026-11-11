// A headless frontend for debugging on a PC: no window, both screens written as image files, input scripted through the
// runtime's RECOMP_LIVE file. Mirrors runtime/frontend.cpp's threads and VBlank pacing, so what runs here behaves the same.
//   RECOMP_FRAMES=<n>       stop after n VBlanks
//   RECOMP_SHOT_EVERY=<n>   write top.ppm and bottom.ppm every n VBlanks, into RECOMP_SHOT_DIR (default: shots)
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "audio_iface.h"
#include "frontend.h"
#include "gpu_iface.h"
#include "kernel.h"
#include "loader.h"
#include "service.h"
#include "services/services.h"
#include "settings.h"
#include "swkbd.h"

using namespace std::chrono;

std::string g_save_base = "save";
char **g_argv;
std::string g_start_game;

static std::mutex gpu_m;
static std::condition_variable gpu_cv;
static bool gpu_wake;

namespace kernel {
void frontend_wake() {
    {
        std::lock_guard<std::mutex> lk(gpu_m);
        gpu_wake = true;
    }
    gpu_cv.notify_one();
}
}  // namespace kernel
void frontend_live_text(const std::string &) {}
void frontend_live_key(const std::string &) {}
u32 host_mask(const char *) { return 0; }
void watch_report() {}

void frontend_init() {
    gpu::init();
    audio::init();
}

static void gpu_thread() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(gpu_m);
            gpu_cv.wait_for(lk, milliseconds(2), [] { return gpu_wake; });
            gpu_wake = false;
        }
        kernel::gsp_process_gx();
    }
}

// RECOMP_PCSPEED=<percent>: VBlanks, input, audio frames and guest timers all run this much faster, which makes guest code as late as on a slow console
static int pc_speed() {
    static const int s = getenv("RECOMP_PCSPEED") ? atoi(getenv("RECOMP_PCSPEED")) : 100;
    return s;
}

// the play window's input (RECOMP_WINDOW, window_thread)
static std::atomic<u32> win_buttons, win_touch_xy;
static std::atomic<s32> win_cx, win_cy;
static std::atomic<bool> win_touch;
static bool window_on;

static void hid_thread() {
    const auto period = nanoseconds(1000000000ll / 234 * 100 / pc_speed());
    auto next = steady_clock::now();
    for (;;) {
        next += period;
        std::this_thread::sleep_until(next);
        // RECOMP_AUTOPLAY: presses A now and then and taps around the bottom screen, to walk through the game unattended
        static const bool autoplay = getenv("RECOMP_AUTOPLAY");
        static u32 tick;
        u32 buttons = 0;
        bool touch = false;
        u16 tx = 0, ty = 0;
        s16 cx = 0, cy = 0;
        // RECOMP_AUTOKB: only finishes the game's keyboard, for runs driven by RECOMP_LIVE
        static const bool autokb = getenv("RECOMP_AUTOKB");
        // RECOMP_WANDER_AFTER=<frame>: follow the RECOMP_LIVE script until then, and RECOMP_AUTOPLAY from there
        static const u64 wander_after = getenv("RECOMP_WANDER_AFTER") ? strtoull(getenv("RECOMP_WANDER_AFTER"), nullptr, 10) : ~0ull;
        if (autokb && g_frame < wander_after) {
            tick++;
            swkbd::Request kb;
            if (tick % 468 == 100 && swkbd::pending(&kb)) swkbd::finish(kb.numpad ? "1234" : "Test", true);
        } else if (autoplay) {
            tick++;
            swkbd::Request kb;
            if (tick % 468 == 100 && swkbd::pending(&kb)) swkbd::finish(kb.numpad ? "1234" : "Test", true);
            const u32 phase = tick % 468;
            if (phase < 12) buttons = 1;  // A
            if (phase >= 234 && phase < 246) {
                static const u16 spots[][2] = {{240, 206}, {160, 206}, {160, 120}, {240, 206}, {280, 30}, {40, 30}};
                const u16 *s = spots[(tick / 468) % 6];
                touch = true;
                tx = s[0];
                ty = s[1];
            }
            // RECOMP_AUTOPLAY=2 also taps at random in between and drags slowly across the screen, to wander into more of the game
            static const bool wander = atoi(getenv("RECOMP_AUTOPLAY") ? getenv("RECOMP_AUTOPLAY") : "0") == 2;
            static u32 seed = getenv("RECOMP_SEED") ? atoi(getenv("RECOMP_SEED")) : 12345;
            auto next_random = [&](u32 limit) {
                seed = seed * 1664525u + 1013904223u;
                return (seed >> 12) % limit;
            };
            if (wander && tick % 702 < 12) {
                static u32 button;
                if (tick % 702 == 0) button = 1u << next_random(12);
                buttons |= button;
            }
            // RECOMP_AUTOPLAY=3 walks about with the circle pad and keeps pressing things, like someone who's never seen the game: games steered by stick reach their rooms and battles
            static const bool walk = atoi(getenv("RECOMP_AUTOPLAY") ? getenv("RECOMP_AUTOPLAY") : "0") == 3;
            if (walk) {
                static const s16 dirs[9][2] = {{0, 0}, {155, 0}, {-155, 0}, {0, 155}, {0, -155}, {110, 110}, {-110, 110}, {110, -110}, {-110, -110}};
                static u32 dir, change;
                if (tick >= change) {
                    dir = next_random(9);
                    change = tick + 234 + next_random(702);
                }
                cx = dirs[dir][0];
                cy = dirs[dir][1];
                if (tick % 351 < 12) buttons |= 1 << 1;   // B
                if (tick % 117 < 12) buttons |= 1;        // A, more often: menus and text
                static u32 other;
                if (tick % 234 == 0) other = next_random(4);
                if (tick % 234 < 24) buttons |= 1u << (other + 8);  // R L X Y
            }
            const u32 drag = tick % 1872;
            if (wander && drag < 468) {
                static u32 x0, y0, x1, y1;
                if (drag == 0) {
                    x0 = next_random(320);
                    y0 = next_random(240);
                    x1 = next_random(320);
                    y1 = next_random(240);
                }
                touch = true;
                tx = (u16)(x0 + ((s32)x1 - (s32)x0) * (s32)drag / 468);
                ty = (u16)(y0 + ((s32)y1 - (s32)y0) * (s32)drag / 468);
            } else if ((wander || walk) && !touch && tick % (walk ? 351 : 60) < 12) {
                static u32 sx, sy;
                if (tick % (walk ? 351 : 60) == 0) {
                    sx = next_random(320);
                    sy = next_random(240);
                }
                touch = true;
                tx = (u16)sx;
                ty = (u16)sy;
            }
        }
#ifdef _WIN32
        if (window_on) {
            buttons = win_buttons;
            cx = (s16)win_cx;
            cy = (s16)win_cy;
            touch = win_touch;
            tx = (u16)(win_touch_xy >> 16);
            ty = (u16)(win_touch_xy & 0xffff);
        }
#endif
        kernel::hid_update(buttons, cx, cy, touch, tx, ty);  // scripted input is merged in by the HID service
    }
}

static void audio_thread() {
    auto next = steady_clock::now();
    for (;;) {
        const auto period = nanoseconds(1000000000ll * 160 / 32728 * 100 / pc_speed());
        next += period;
        std::this_thread::sleep_until(next);
        // as on the Wii U: a long stall isn't made up for, or the sound thread takes all the time from lower priority threads (the game itself) until it has caught up
        if (steady_clock::now() - next > period * 8) next = steady_clock::now();
        // Debugging: RECOMP_AUDIOSTALL=N stalls this thread 12-20 ms about once every N frames, as a busy console's audio thread can be
        static const int stall = getenv("RECOMP_AUDIOSTALL") ? atoi(getenv("RECOMP_AUDIOSTALL")) : 0;
        if (stall > 0 && std::rand() % stall == 0) std::this_thread::sleep_for(milliseconds(12 + std::rand() % 9));
        while (!kernel::dsp_tick()) std::this_thread::sleep_for(microseconds(500));
    }
}

static void write_screen(const char *path, const kernel::FramebufferInfo &f, int width) {
    FILE *out = fopen(path, "wb");
    if (!out) return;
    const int height = 240;
    fprintf(out, "P6\n%d %d\n255\n", width, height);
    const u32 fmt = f.format & 7;
    const int bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    const u32 stride = f.stride ? f.stride : 240 * bpp;
    const u8 *base = f.valid && f.addr_left ? gptr(f.addr_left) : nullptr;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            u8 rgb[3] = {0, 0, 0};
            if (base) {
                const u8 *p = base + (size_t)x * stride + (size_t)(height - 1 - y) * bpp;
                const u16 v = (u16)(p[0] | (p[1] << 8));
                switch (fmt) {
                case 0: rgb[0] = p[3]; rgb[1] = p[2]; rgb[2] = p[1]; break;
                case 1: rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; break;
                case 2: rgb[0] = ((v >> 11) & 31) * 255 / 31; rgb[1] = ((v >> 5) & 63) * 255 / 63; rgb[2] = (v & 31) * 255 / 31; break;
                case 3: rgb[0] = ((v >> 11) & 31) * 255 / 31; rgb[1] = ((v >> 6) & 31) * 255 / 31; rgb[2] = ((v >> 1) & 31) * 255 / 31; break;
                default: rgb[0] = ((v >> 12) & 15) * 17; rgb[1] = ((v >> 8) & 15) * 17; rgb[2] = ((v >> 4) & 15) * 17; break;
                }
            }
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
}

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

// RECOMP_WINDOW=1: a window to play in, both screens at twice their size, the top over the bottom. Keyboard: arrows the circle pad, A/S/Z/X the
// A/B/X/Y buttons, Q/W L and R, Enter Start, Backspace Select, I/J/K/L the d-pad; an XInput controller as a 3DS's; the mouse on the bottom screen
// is the stylus. Closing it ends the run. For playing a game through on the PC, so its shaders get logged (vs_log.bin, fs_log.bin) for the
// Wii U build without a console session for every room.
static HWND win_hwnd;
constexpr int WIN_SCALE = 2, WIN_W = 400 * WIN_SCALE, WIN_H = 480 * WIN_SCALE;

static void screen_pixels(const kernel::FramebufferInfo &f, int width, u32 *out, int out_stride) {
    const int height = 240;
    const u32 fmt = f.format & 7;
    const int bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    const u32 stride = f.stride ? f.stride : 240 * bpp;
    const u8 *base = f.valid && f.addr_left ? gptr(f.addr_left) : nullptr;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            u32 c = 0;
            if (base) {
                const u8 *q = base + (size_t)x * stride + (size_t)(height - 1 - y) * bpp;
                const u16 v = (u16)(q[0] | (q[1] << 8));
                u32 r, g, b;
                switch (fmt) {
                case 0: r = q[3]; g = q[2]; b = q[1]; break;
                case 1: r = q[2]; g = q[1]; b = q[0]; break;
                case 2: r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break;
                case 3: r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 6) & 31) * 255 / 31; b = ((v >> 1) & 31) * 255 / 31; break;
                default: r = ((v >> 12) & 15) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; break;
                }
                c = r << 16 | g << 8 | b;
            }
            out[y * out_stride + x] = c;
        }
}

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
        fflush(nullptr);
        _exit(0);
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
        const int x = (short)LOWORD(l) / WIN_SCALE - 40, y = (short)HIWORD(l) / WIN_SCALE - 240;
        const bool down = m != WM_LBUTTONUP && (w & MK_LBUTTON) && x >= 0 && x < 320 && y >= 0 && y < 240;
        if (down) win_touch_xy = (u32)x << 16 | (u32)y;
        win_touch = down;
        if (m == WM_LBUTTONDOWN) SetCapture(h);
        if (m == WM_LBUTTONUP) ReleaseCapture();
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, void *);
struct XPad {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

static void window_thread() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = win_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "3dsrecomp";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r{0, 0, WIN_W, WIN_H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);
    win_hwnd = CreateWindowA("3dsrecomp", (g_game.name.empty() ? std::string("3DS game") : g_game.name).c_str(), (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    auto xget = xi ? (XInputGetStateFn)GetProcAddress(xi, "XInputGetState") : nullptr;
    std::vector<u32> pic(400 * 480);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 400;
    bi.bmiHeader.biHeight = -480;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        // input: the keyboard while the window has the focus, and the first controller
        u32 b = 0;
        s32 cx = 0, cy = 0;
        if (GetForegroundWindow() == win_hwnd) {
            auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
            const struct { int vk; u32 bit; } map[] = {{'A', 1 << 0}, {'S', 1 << 1}, {VK_BACK, 1 << 2}, {VK_RETURN, 1 << 3}, {'L', 1 << 4}, {'J', 1 << 5},
                                                         {'I', 1 << 6}, {'K', 1 << 7}, {'W', 1 << 8}, {'Q', 1 << 9}, {'Z', 1 << 10}, {'X', 1 << 11}};
            for (const auto &e : map)
                if (k(e.vk)) b |= e.bit;
            if (k(VK_LEFT)) cx -= 155;
            if (k(VK_RIGHT)) cx += 155;
            if (k(VK_UP)) cy += 155;
            if (k(VK_DOWN)) cy -= 155;
        }
        XPad x{};
        if (xget && xget(0, &x) == 0) {
            const struct { WORD xb; u32 bit; } map[] = {{0x1000, 1 << 1}, {0x2000, 1 << 0}, {0x4000, 1 << 11}, {0x8000, 1 << 10}, {0x0010, 1 << 3}, {0x0020, 1 << 2},
                                                          {0x0008, 1 << 4}, {0x0004, 1 << 5}, {0x0001, 1 << 6}, {0x0002, 1 << 7}, {0x0200, 1 << 8}, {0x0100, 1 << 9}};
            for (const auto &e : map)
                if (x.buttons & e.xb) b |= e.bit;  // the Xbox layout's A (bottom) as the 3DS's B, as on a 3DS's own layout
            if (x.rt > 64) b |= 1 << 8;
            if (x.lt > 64) b |= 1 << 9;
            auto axis = [](SHORT v) { return std::abs(v) < 7000 ? 0 : (s32)v * 155 / 32767; };
            if (axis(x.lx) || axis(x.ly)) {
                cx = axis(x.lx);
                cy = axis(x.ly);
            }
        }
        win_buttons = b;
        win_cx = cx;
        win_cy = cy;
        // the screens
        screen_pixels(kernel::gsp_framebuffer(0), 400, pic.data(), 400);
        std::fill(pic.begin() + 400 * 240, pic.end(), 0u);
        screen_pixels(kernel::gsp_framebuffer(1), 320, pic.data() + 400 * 240 + 40, 400);
        if (HDC dc = GetDC(win_hwnd)) {
            StretchDIBits(dc, 0, 0, WIN_W, WIN_H, 0, 0, 400, 480, pic.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            ReleaseDC(win_hwnd, dc);
        }
        Sleep(16);
    }
}

#include <vector>
// RECOMP_PROFILE=<file>: about every millisecond, where each of this program's threads is: (thread id, address minus where the program loaded) as
// three words. tools/pc_profile.py names the functions. Written only while no thread is held, so nothing it takes can be held by one.
static void profile_thread(std::string path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const u64 base = (u64)GetModuleHandleA(nullptr);
    std::vector<std::pair<DWORD, HANDLE>> ts;
    std::vector<u32> got;
    // RECOMP_PROFILE_AFTER=<frame>: from then on, past the menus a route goes through
    const u64 after = getenv("RECOMP_PROFILE_AFTER") ? strtoull(getenv("RECOMP_PROFILE_AFTER"), nullptr, 10) : 0;
    while (g_frame < after) Sleep(50);
    for (u64 round = 0;; round++) {
        if (round % 500 == 0) {  // threads come and go
            for (auto &t : ts) CloseHandle(t.second);
            ts.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) ts.push_back({te.th32ThreadID, h});
            CloseHandle(snap);
        }
        got.clear();
        for (auto &[id, h] : ts) {
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            const bool ok = GetThreadContext(h, &c);
            ResumeThread(h);
            if (ok) got.insert(got.end(), {(u32)id, (u32)(c.Rip - base), (u32)((c.Rip - base) >> 32)});
        }
        fwrite(got.data(), 4, got.size(), f);
        if (round % 1000 == 0) fflush(f);
        Sleep(1);
    }
}
#endif

void frontend_run() {
#ifdef _WIN32
    if (const char *p = getenv("RECOMP_PROFILE")) std::thread(profile_thread, std::string(p)).detach();
    window_on = getenv("RECOMP_WINDOW") && atoi(getenv("RECOMP_WINDOW"));
    if (window_on) std::thread(window_thread).detach();
#endif
    std::thread(gpu_thread).detach();
    std::thread(hid_thread).detach();
    std::thread(audio_thread).detach();
    const u64 max_frames = getenv("RECOMP_FRAMES") ? strtoull(getenv("RECOMP_FRAMES"), nullptr, 10) : 0;
    const u64 shot_every = getenv("RECOMP_SHOT_EVERY") ? strtoull(getenv("RECOMP_SHOT_EVERY"), nullptr, 10) : 0;
    const std::string shots = getenv("RECOMP_SHOT_DIR") ? getenv("RECOMP_SHOT_DIR") : "shots";
    if (shot_every) std::filesystem::create_directories(shots);
    const auto frame = nanoseconds(16715000ll * 100 / pc_speed());
    kernel::set_clock_speed(pc_speed());
    auto next_vblank = steady_clock::now() + frame;
    auto next_time = steady_clock::now();
    u64 frames = 0;
    for (;;) {
        std::this_thread::sleep_until(next_vblank);
        kernel::gsp_vblank();
        frames++;
        g_frame = frames;
        if (steady_clock::now() >= next_time) {
            update_shared_page_time();
            next_time = steady_clock::now() + seconds(1);
        }
        if (shot_every && frames % shot_every == 0) {
            const std::string n = std::to_string(frames / shot_every);
            write_screen((shots + "/top" + n + ".ppm").c_str(), kernel::gsp_framebuffer(0), 400);
            write_screen((shots + "/bottom" + n + ".ppm").c_str(), kernel::gsp_framebuffer(1), 320);
            LOG("[pc] frame %llu: gx lists %llu, transfers %llu\n", (unsigned long long)frames,
                (unsigned long long)kernel::gx_stats.cmdlists, (unsigned long long)kernel::gx_stats.transfers);
        }
        // RECOMP_MEMDUMP="f1,f2,...": guest memory snapshots (mem_<frame>_<address>.bin), as the SDL frontend's
        static const char *memdump = getenv("RECOMP_MEMDUMP");
        if (memdump && (std::string(",") + memdump + ",").find("," + std::to_string(frames) + ",") != std::string::npos)
            kernel::dump_memory(("mem_" + std::to_string(frames)).c_str());
        if (max_frames && frames >= max_frames) {
            kernel::dump_threads();
            fflush(nullptr);
            _exit(0);
        }
        next_vblank += frame;
    }
}

int main(int argc, char **argv) {
    g_argv = argv;
    std::string dir = "data";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") g_verbose = 1;
        else if (a == "-vv") g_verbose = 2;
        else dir = a;
    }
    if (const char *v = getenv("RECOMP_VERBOSE")) g_verbose = atoi(v);
    mem::init();
    load_game(dir);
    std::filesystem::create_directories(kernel::save_root());
    rt_log_open(kernel::save_root() + "/log.txt");
    settings_load();
    kernel::init_services();
    frontend_init();
    kernel::BootInfo bi{g_game.text_addr, g_game.stack_size, g_game.priority, g_game.program_id};
    kernel::start(bi);
    frontend_run();
    return 0;
}
