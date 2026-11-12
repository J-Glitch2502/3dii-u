// Thirty frames a second: which of a game's frames are left undrawn (gx2_backend.cpp's left_out and frame_left_out;
// tools/test_frame_cap.cpp plays made-up games against it).
// Of a game that makes a frame for every VBlank, every other frame of each screen is left out: nothing is drawn to that screen's surface
// from the transfer before it to its own, its clear is dropped, and its transfer shows nothing, so the frame before stays up for both
// VBlanks. The game itself runs as it did: its lists are read, its other targets drawn, its interrupts sent. Only half the drawing is
// done, which is what holds a game back when it can't keep up. A game made for thirty is left as it is: half of thirty is fifteen.
//
// So the one thing to know is the game's own pace, and the only place it shows is where the drawing thread was waiting for the game: a
// frame whose first work came after such a wait began when the game chose, and from one such beginning to the next is the game's pace,
// whatever the drawing in between took (over two frames, where the one between was still being drawn when its work came). Six of the last
// eight paces under SIXTY, and the game is one that makes sixty.
// It used to go by when frames ended. A frame that is left out ends as soon as its work comes, which is the game's 33 ms after the frame
// before it began but only 33 ms less that frame's drawing time after it ended: on a console, where drawing takes 15 to 25 ms of a
// frame, every game made for thirty looked like one of sixty and kept half its frames (Super Mario 3D Land, 2026-11-12: "speed is fine,
// it just feels like 20fps"). In Cemu drawing takes no time to speak of, and it never showed.
//
// A surface both screens are drawn in goes by the top screen's frames, as which screen a draw there is for isn't known until its transfer.
// shortcut: a game that stops drawing altogether right after a marked frame (a long load) keeps the frame before it on that screen until
// it draws again; if one shows that (a fade's last step), keep the marked frame's lists and draw them late.
// shortcut: in a surface both screens are drawn in, a top screen that stops coming right after it marked a frame leaves the bottom
// screen's draws dropped until it draws again; no game checked does that. A mark that runs out by the clock would end it.
#pragma once

struct FrameCap {
    static constexpr unsigned long long SIXTY = 30000;   // microseconds from one frame's beginning to the next: more than 33 a second
    static constexpr unsigned long long PAUSE = 100000;  // longer than this between two is a load, and says nothing about the pace

    struct Screen {
        unsigned long long anchor = 0;  // when the last of its frames to begin after a wait began; 0 before the first
        unsigned frames = 0;            // its frames that have ended since
        unsigned seen = 0;              // how many times work had come after a wait when that one ended
        unsigned char paces = 0;        // the last eight paces, a bit each: set for one under SIXTY
        bool sixty = false;             // six of the eight: the game makes a frame of this screen for every VBlank
        bool skip = false;              // the frame being made for it is marked: its draws are dropped
    };
    Screen screen[2];                   // the top screen's and the bottom's
    unsigned long long arrived_at = 0;  // when the drawing thread last got work after waiting for the game, in microseconds
    unsigned arrivals = 0;              // and how many times it has

    // the drawing thread, which had been waiting with nothing to do, has work again
    void work_arrived(unsigned long long now) {
        arrived_at = now;
        arrivals++;
    }

    bool marking() const { return screen[0].skip || screen[1].skip; }

    // whether a draw to a surface is dropped. lcds: the screens its picture has been shown on (bit 0 the top, bit 1 the bottom; both: it
    // goes by the top)
    bool drops(unsigned lcds) const { return lcds && screen[lcds == 2].skip; }

    // A screen's transfer ends a frame of it: true if that frame was left out, and nothing is to be shown. shared: both screens are drawn
    // in the transfer's surface; redrawn: the game is sure to draw this picture again next frame (a frame left out and never drawn again
    // would be lost for good: a menu drawn once, then only sent).
    bool frame_ends(int which, bool shared, bool redrawn) {
        if (which == 1 && shared) return screen[0].skip;
        Screen &c = screen[which];
        c.frames++;
        if (c.seen != arrivals) {  // work came after a wait since its last frame ended: this frame began when the game chose
            const unsigned long long pace = c.anchor ? (arrived_at - c.anchor) / c.frames : PAUSE;
            if (pace < PAUSE) {
                c.paces = (unsigned char)(c.paces << 1 | (pace < SIXTY));
                c.sixty = __builtin_popcount(c.paces) >= 6;
            }
            c.anchor = arrived_at;
            c.frames = 0;
            c.seen = arrivals;
        }
        const bool was = c.skip;
        c.skip = !was && c.sixty && redrawn;
        return was;
    }
};
