// Thirty frames a second: which of a game's frames are left undrawn (gx2_backend.cpp's left_out and frame_left_out;
// tools/test_frame_cap.cpp plays made-up games against it).
// Of a game that makes a frame for every VBlank, every other frame of each screen is left out: nothing is drawn to that screen's surface
// from the transfer before it to its own, its clear is dropped, and its transfer shows nothing, so the frame before stays up for both
// VBlanks. The game itself runs as it did: its lists are read, its other targets drawn, its interrupts sent. Only half the drawing is
// done, which is what holds a game back when it can't keep up.
// Which frames: once a frame has ended soon after the one before (FAST), the frame after each drawn one is marked to be left out. A marked
// frame says whether to go on: it costs nothing to draw, so one that still ends late is the game's own pace (a game or a scene made for
// thirty), and the marking stops.
// "Soon" is by the clock, not by counting VBlanks. A game at thirty whose frames end near a VBlank's moment has them counted one, two and
// three VBlanks apart though they are 33 ms apart every time, and going by the count took that for sixty again and again: each time one
// frame of a thirty-a-second game was left out, and the picture stood for four VBlanks and then jumped (Super Mario 3D Land on a console,
// 2026-11-12: "the screen is jerking back", all the time). A frame of thirty that comes early after a late one can still look like sixty
// for a moment, so marking that fails before it has held (HOLDS marked frames in a row that ended soon) rests, twice as long each time
// it fails so again: a game made for thirty loses a frame a dozen times in its first minutes, then one in two minutes. A game at sixty
// holds, and what it stumbles over after that (a load, a slow frame of its own) costs it no rest.
// A surface both screens are drawn in goes by the top screen's frames, as which screen a draw there is for isn't known until its transfer.
// shortcut: a game that stops drawing altogether right after a marked frame (a long load) keeps the frame before it on that screen until
// it draws again; if one shows that (a fade's last step), keep the marked frame's lists and draw them late.
// shortcut: in a surface both screens are drawn in, a top screen that stops coming right after it marked a frame leaves the bottom
// screen's draws dropped until it draws again; no game checked does that. A mark that runs out by the clock would end it.
#pragma once

struct FrameCap {
    static constexpr unsigned long long FAST = 29000;  // microseconds: 1.75 VBlanks. Frames at sixty end 17 ms apart, at thirty 33
    static constexpr unsigned long long REST = 100000, LONGEST_REST = 120000000;
    static constexpr unsigned HOLDS = 30;  // marked frames: a second of a game at sixty

    struct Screen {
        unsigned long long last = 0;   // when its last frame ended, in microseconds
        unsigned long long until = 0;  // no marking before then
        unsigned long long rest = 0;   // how long the last marking that failed at once made it wait
        unsigned held = 0;             // marked frames in a row that ended soon
        bool skip = false;             // the frame being made for it is marked: its draws are dropped
        bool engaged = false;          // its frames have been ending soon after one another
    };
    Screen screen[2];  // the top screen's and the bottom's

    bool marking() const { return screen[0].skip || screen[1].skip; }

    // whether a draw to a surface is dropped. lcds: the screens its picture has been shown on (bit 0 the top, bit 1 the bottom; both: it
    // goes by the top)
    bool drops(unsigned lcds) const { return lcds && screen[lcds == 2].skip; }

    // A screen's transfer ends a frame of it: true if that frame was left out, and nothing is to be shown. shared: both screens are drawn
    // in the transfer's surface; redrawn: the game is sure to draw this picture again next frame (a frame left out and never drawn again
    // would be lost for good: a menu drawn once, then only sent); now: the time in microseconds.
    bool frame_ends(int which, bool shared, bool redrawn, unsigned long long now) {
        if (which == 1 && shared) return screen[0].skip;
        Screen &c = screen[which];
        const bool was = c.skip, fast = now - c.last < FAST;
        c.last = now;
        if (was) {
            c.engaged = fast;
            if (fast) {
                if (++c.held == HOLDS) c.rest = 0;
            } else {
                if (c.held < HOLDS) {
                    c.rest = c.rest ? (c.rest * 2 < LONGEST_REST ? c.rest * 2 : LONGEST_REST) : REST;
                    c.until = now + c.rest;
                }
                c.held = 0;
            }
        } else if (fast && now >= c.until) {
            c.engaged = true;
        }
        c.skip = !was && c.engaged && redrawn;
        return was;
    }
};
