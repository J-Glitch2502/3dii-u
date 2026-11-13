// Cross-service hooks.
#pragma once
#include <array>
#include <vector>

#include "../kernel.h"

namespace kernel {
// The host console's own Miis (the Wii U's accounts: frontend_wiiu.cpp), as 0x60-byte store data, filled before the services start.
// Games see them in Mii Maker's database (fs.cpp), as the player's Mii (frd:u) and in the Mii Selector (apt.cpp).
extern std::vector<std::array<u8, 0x60>> g_host_miis;
extern int g_host_my_mii;  // the current account's, or -1
bool mii_3ds(const std::array<u8, 0x60> &host, std::array<u8, 0x60> &out);  // false: not a usable Mii
const std::vector<std::array<u8, 0x60>> &console_miis(int *mine);  // the ones a game can take, each once; mine: the current account's number among them, or -1
int mii_maker_place(u32 n);            // where the nth of console_miis() lies in the game's Mii Maker database (fs.cpp), or -1: no place was free
std::array<u8, 0x60> player_mii();     // the current account's Mii, or a stand-in "Mario" when it has no usable one
u16 mii_crc16(const u8 *p, size_t n);  // CRC-16/XMODEM, as Mii data and Mii Maker's database use
void register_apt();
void register_fs();
void register_cfg();
void register_gsp();
void register_hid();
void register_dsp();
void register_misc();
void register_ro();

std::shared_ptr<SharedMem> load_shared_font();
// a file of a system archive (system_archives/<title id>.romfs), empty if missing
std::vector<u8> system_archive_file(u64 tid, const std::string &path);
// a file of the game's RomFS by path ("/LayoutData/x.szs"), empty if missing
std::vector<u8> romfs_file(const std::string &path);
void apt_notify(u32 signal);

// gsp
struct FramebufferInfo {
    u32 addr_left, addr_right, stride, format;
    bool valid;
};
FramebufferInfo gsp_framebuffer(int screen);  // what the LCD shows (any thread)
void gsp_vblank();                             // (host thread, lock not held) VBlank interrupts
void gsp_process_gx();                         // (GPU/host thread) run queued GX commands
extern volatile bool gx_hold;                  // while set, gsp_process_gx leaves the rest of the queue for later
void gsp_cpu_wrote(u32 vaddr, u32 size);       // a service wrote guest memory the GPU may read
void gsp_cache_op(u32 vaddr, u32 size, bool cpu_will_read);  // the game flushed or invalidated its cache, by any of the calls that do that
void register_y2r();
void register_cam();
bool gsp_gx_pending();
struct GxStats { u64 cmdlists, gpu_ns, transfers; };
extern GxStats gx_stats;
// hid
void hid_update(u32 buttons, s16 cx, s16 cy, bool touch, u16 tx, u16 ty);  // (host thread, lock not held)
void hid_motion(const s16 *accel, const s16 *gyro);                        // (the thread that calls hid_update) x, y, z in the 3DS's units
// dsp
void dsp_tick();  // (host thread, lock not held) one audio frame, played whether or not the game has come for the last one shortly
}
// The host's microphone, for mic:u (lock held). misc.cpp has weak defaults that give silence.
void mic_host_start();
void mic_host_stop();
void mic_host_read(s16 *out, u32 n, u32 rate);  // the latest n samples, at this rate
