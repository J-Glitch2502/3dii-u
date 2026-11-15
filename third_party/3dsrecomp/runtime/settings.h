// Per-game user settings (<save dir>/<title id>/settings.ini), changed from the in-game menu (runtime/menu.mm).
// Environment variables given at launch (GPU_MSAA, GPU_ANISO, ...) take precedence over the file.
#pragma once
#include <string>
#include <vector>

#include <TargetConditionals.h>

#include "rt.h"

struct Settings {
    int scale = 0;          // internal resolution: 0 auto (from the window), 1-4
    int msaa = 4;           // samples per pixel: 1 (off), 2, 4, 8 (applies at launch)
    int aniso = 16;         // max anisotropy: 1 (off), 2, 4, 8, 16 (applies at launch)
    int layout = TARGET_OS_IOS ? 6 : 0;  // the layout table in frontend.cpp (0 stacked, 1 top screen only, ... 6 touch)
    bool fullscreen = false;
    bool split = false;        // separate windows: the top screen in the main one, the bottom screen in a second
    bool fullscreen2 = false;  // the bottom screen's window
    int geom[4] = {}, geom2[4] = {};  // window positions and sizes (x, y, w, h; w 0: not saved yet)
    int speed = 100;        // game speed in percent of the 3DS's (VBlank and audio clocks)
    int volume = 100;       // percent
    bool show_fps = false;  // frame rate counter in the corner
    s64 clock_offset = 0;   // seconds added to the Mac's clock for the 3DS clock
    bool auto_backup = true;  // copy the save into backups/ at each launch (the last 5 are kept)
    bool forgive_unsaved = true;  // clear the profile's "unsaved" marks at launch (no scolding after a crash)
    // single-screen layouts
    int pip = 1;              // the other screen in a corner: 0 off, 1 while it changes, 2 always
    int pip_corner = 0;       // 0 top right, 1 top left, 2 bottom right, 3 bottom left
    int pip_x = -1, pip_y = -1;  // where it was dragged: its top left in thousandths of the window (-1: the corner)
    int pip_size = 38;        // its height in percent of the window's
    bool auto_focus = true;   // the game's profile picks the screen (e.g. the bottom one on world maps)
    int hud = 1;              // the profile's HUD over the top screen: 0 off, 1 when the bottom screen is hidden, 2 always
    int tex_filter = 0;       // texture upscaling (gpu::texture_filter_name)
    int language = -1;        // from the game's next start (services/languages.h): 0 Japanese .. 11; -1 the console's own
    int tv_layout = 0;        // Wii U: what the TV shows (gx2_backend.cpp tv_rect): 0 the top screen, 1 side by side, 2 stacked, 3 the top large and
                              // the bottom small, 4 the bottom screen, 5 the bottom large and the top small
    int pad_layout = 0;       // Wii U: what the GamePad shows (gx2_backend.cpp pad_rect): 0 the bottom screen, 1 the top screen, 2 both, top above bottom
    int resolution = 720;     // Wii U: the rows the top screen is drawn at (gx2_backend.cpp rt_scale), 720 or 480, from the game's next start
    bool stack_items = false;  // stacks in the pockets, for games whose profile has them (runtime/stacks.mm)
};

extern Settings g_settings;

void settings_load();  // also exports the launch-time settings to the GPU backend's environment variables
void settings_save();

// Save management (the menu's Saves page): the game's save data is <save root>/savedata and extdata/.
struct SaveBackup {
    std::string name, path;  // name: "2026-10-27 19.42.03" (+ " auto" / " before restore" / " before delete")
    u64 bytes;
};
std::vector<SaveBackup> save_backups();  // newest first
bool save_exists();
std::string save_summary();               // "18 KB, written 27 Oct 19:04" or "no save yet"
bool save_backup_now(const std::string &suffix = "");
bool save_restore(const SaveBackup &b);   // backs up the current save first; the game must be restarted
bool save_delete_current();               // backs it up first; the game must be restarted
bool save_delete_backup(const SaveBackup &b);
void save_auto_backup();                  // at launch, if enabled
void save_forgive_unsaved();              // at launch, if enabled: clears the profile's "unsaved" marks
