// The 3DS Mii Selector applet, as a picker the frontend draws: services/apt.cpp holds the game's request until the player picks one of
// the console's Miis or cancels. A frontend that has a picker says so (host_picker); without one the game gets the player's Mii at
// once, as it always did. No kernel types here, as in swkbd.h.
#pragma once
#include <string>
#include <vector>

namespace miisel {
struct Entry {
    std::string name;        // UTF-8
    int color = 0;           // the Mii's favourite colour, 0-11
    bool selectable = true;  // false: the game's list leaves this one out, and the 3DS shows it greyed
    int face = -1;           // the host's picture of its face (services.h: g_host_mii_faces), or -1: it has none
};
struct Request {
    std::string title;       // UTF-8, "" for the applet's own
    bool can_cancel = false;
    int initial = 0;         // the entry the cursor starts on
    std::vector<Entry> miis;
    unsigned serial = 0;     // changes with every request
};
extern bool host_picker;                  // set by the frontend before the game starts
bool pending(Request *out);               // (any thread) a request waits for the player
void finish(int index, bool ok);          // (host thread, kernel lock not held) the entry picked, or ok false: cancelled
}  // namespace miisel
