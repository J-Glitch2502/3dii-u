// What the Wii U frontend and its graphics backend share.
#pragma once
#include <string>
#include <vector>

#include "cpu.h"

namespace audio {
void shutdown();
void pause(bool on);
}

namespace wiiu {
// this title's save storage on the console, for saves and logs
const std::string &data_dir();
// save_storage.cpp. Mount before anything goes near data_dir. A change to a save makes it dirty, and the card thread flushes it.
void save_mount();
void save_dirty();
void save_flush(bool now);
long save_write(int fd, const void *data, u32 len, u64 at);
void save_resize(int fd, u64 size);
const std::string &save_account_dir();  // the player's storage, /vol/save/<persistent id>, or empty without one
bool save_pending();                     // saved by the game and not flushed yet
bool save_copy_tree(const std::string &from, const std::string &to);
// (input thread) the right stick is held in: the microphone hears blowing
void mic_blow(bool on);
// a line in trace.txt, which survives a forced power-off, and the last one is on the screen if the game freezes: for first uses of what Cemu can't test
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// takes turns with every other file operation once the game runs; a frozen game reports what's been going on for how long
void fs_begin(u32 op);
void fs_end();
void romfs_begin();  // the game's RomFS reads: they take turns only with each other, never with saves being written out or flushed
void romfs_end();
void fsa_direct_check_writes();
void fsa_direct_report();  // fsa_direct.cpp: which way file calls go, for the log
bool fsa_direct_flush_quota(const char *path);
// direct_read.cpp: the game's RomFS read past Aroma's hook on the file calls, once three stretches of it have matched the ordinary path. false: read it the ordinary way.
void direct_read_open(const char *path, int fd, u64 size);
bool direct_read(void *dst, u32 len, u64 off);
void gfx_write_misses();  // (card thread) adds what the GPU thread has noted to shader_misses.bin
void gfx_take_misses(std::vector<u8> &out);  // the same notes, for the PC
int net_thread(int, void *);  // net_console.cpp: the PC's way in, over the network
bool net_connected();
void log_status();   // how things stand, into the log, from any thread
void log_threads();  // that and every thread in the process
// (GPU thread) set up GX2 and everything drawn with it
void gfx_thread_init();
// GX2 is up: the main thread can wait for the TV's VBlanks
bool gfx_ready();
// (GPU thread) present the screens if a new VBlank arrived, service in-game overlay
void gfx_pump(bool quiet);  // quiet: the GPU thread had nothing to do for its whole wait
void gfx_work_arrived(u64 now);  // the GPU thread has work again after such a wait, at this time in microseconds
void gfx_shutdown();
void gfx_reacquire();  // (GPU thread) back from the HOME Menu
void gfx_note_busy(u32 us);
// (main thread) a VBlank passed: present what is ready
void gfx_vblank();
// log timing and memory statistics; returns draws per flip since the last report
float gfx_report();
u64 gfx_flips();  // frames shown so far, from any thread
bool tv_shot(u32 step, std::vector<u8> &ppm, u32 addr = 0);  // (net thread) the TV or a target or texture at a guest address, at 1/step size, by the next present
bool gfx_set(const char *name, u32 v);  // (net thread) a renderer switch: uberall, uberfb, cpuvs, trace
void gfx_list();  // (net thread) every target, in the log
std::string save_folder();  // (at boot, once) where the game's saves go: see net_console.cpp
bool test_run();
bool debug_flag(const char *name);  // a file of that name in the data folder, or named by the PC when it started this test run
extern volatile u32 last_vs, last_ps;  // the shaders most recently loaded: a shader the GPU can't run hangs it, which only a real GPU shows
void profile_this_core();  // console_profile.cpp
void profile_stop();
void profile_write();
void profile_send();  // (net thread) "prof"
// GamePad touch position -> 3DS bottom-screen pixel, if the touch is on it
void touch_to_bottom(u16 x, u16 y, bool &touch, u16 &tx, u16 &ty);
// (input thread) the menu overlay takes input while it is open
void input_override(u32 &buttons, bool &touch, u16 &tx, u16 &ty);
// the options menu (click the left stick)
void menu_input(u32 pressed);
bool menu_open();
int game_speed();  // percent
bool menu_wants_quit();
void menu_quit_now();  // as choosing Quit there: a test route's "KEY quit", so a run in Cemu ends the way a player ends one
// the game's software keyboard (GamePad pixels)
void keyboard_input(u32 pressed, bool touch_began, u16 x, u16 y);
bool keyboard_open();
}  // namespace wiiu
