// When a finished frame is shown (gx2_backend.cpp's gfx_pump; tools/test_present_rule.cpp runs it against made-up frame times).
// One present for each VBlank at most. It used to come only at the GPU thread's first look after a VBlank, and only if a screen had been
// drawn by then: a frame whose screens both got there a moment later waited for the next VBlank's look, and the frame after it was often
// there by then too and replaced it unseen. On a console New Super Mario Bros. 2's levels made a frame for every VBlank and showed three
// in four (the log of 2026-11-07: 3.00 lists a VBlank, 0.75 shown). The look after a VBlank still shows whatever is new, as it did. But a
// VBlank that finds nothing new leaves its turn open, and a frame is then shown the moment it is whole: every screen the game's frames
// bring has been drawn, so the two screens stay together, and the GPU thread has had nothing to do for its whole wait (quiet), so a game
// that draws a screen in two passes isn't caught between them. A frame that is ready before its VBlank goes out the same way, on the turn
// the last VBlank left, and reaches the TV a refresh sooner. Not every late frame is saved: one whose VBlank's turn already went to the
// late bottom screen of the frame before it still waits, and can be replaced. With frames ending up to 3 ms either side of the VBlank
// the old rule showed 79 in 100 and this one 95 (the test).
#pragma once

struct PresentRule {
    bool due = false;    // a VBlank has come that no present has used yet
    unsigned fresh = 0;  // the screens drawn since the last present: bit 0 the top, bit 1 the bottom
    unsigned frame = 3;  // the screens a whole frame of this game brings: those drawn before any of the last few presents
    unsigned char absent[2] = {0, 0};  // presents in a row that came without that screen

    void drew(int screen) { fresh |= 1u << screen; }
    // the first look after a VBlank: true if a present is to be made now (anything_new: a game screen, the menu or the keyboard was drawn)
    bool vblank(bool anything_new) {
        due = true;
        return anything_new && take();
    }
    // any other look: true if a whole frame is there, a VBlank's turn is open, and the GPU thread was idle for its whole wait
    bool between(bool quiet) { return quiet && due && fresh && (fresh & frame) == frame && take(); }
    void reset() {
        due = false;
        fresh = 0;
    }

  private:
    // A screen that stops coming (a game that leaves its bottom screen as it is) drops out of what a whole frame is after a few presents,
    // and one that comes joins at once. A look that caught half a frame changes nothing by itself.
    bool take() {
        for (unsigned s = 0; s < 2; s++) {
            if (fresh >> s & 1) {
                absent[s] = 0;
                frame |= 1u << s;
            } else if (fresh && absent[s] < 255 && ++absent[s] >= 8 && (frame & ~(1u << s))) {
                frame &= ~(1u << s);
            }
        }
        due = false;
        fresh = 0;
        return true;
    }
};
