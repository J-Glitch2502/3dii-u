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
//
// A game that swaps its screens (services/gsp.cpp: the framebuffer it shows next, at a VBlank, as a 3DS's screens take it) is shown on its
// swaps. A 3DS shows a frame from the VBlank the game swapped at, however early its picture was ready, so a game making a frame every other
// VBlank is shown two refreshes a frame; shown as soon as its picture was drawn, a frame that came a little early or late against the
// moment of a VBlank stood one refresh and the one before it three. So a present waits for the game's swap, and goes out once the screens
// the game swapped are drawn: at the swap's VBlank, or the moment they are drawn after it, without waiting for the GPU thread to be idle,
// as the game has swapped. Only the screens it swapped (Mario Kart 7 swaps its bottom screen less often than its top), and of those only
// the ones its frames bring (frame): a screen this backend never draws can't hold the others up. A game that hasn't swapped for a while is
// shown as before.
#pragma once

struct PresentRule {
    bool due = false;    // a VBlank has come that no present has used yet
    unsigned fresh = 0;  // the screens drawn since the last present: bit 0 the top, bit 1 the bottom
    unsigned frame = 3;  // the screens a whole frame of this game brings: those drawn before any of the last few presents
    unsigned char absent[2] = {0, 0};  // presents in a row that came without that screen
    unsigned swap_due = 0;             // the screens the game swapped whose frame isn't out yet
    unsigned char since_swap = 255;    // VBlanks since the game last swapped
    static constexpr unsigned char kSwapping = 8;  // a game that swapped within so many VBlanks is shown on its swaps (one at 7.5 frames a second)

    void drew(int screen) { fresh |= 1u << screen; }
    // the first look after a VBlank: true if a present is to be made now. anything_new: a game screen, the menu or the keyboard was drawn;
    // swapped: the screens the game swapped at this VBlank; ours: the menu or the keyboard is up, which goes out at every VBlank
    bool vblank(bool anything_new, unsigned swapped = 0, bool ours = false) {
        due = true;
        const bool waited = swap_due;  // a swap of an earlier VBlank still waiting for its screens
        if (swapped) {
            swap_due |= swapped;
            since_swap = 0;
        } else if (since_swap < 255) {
            since_swap++;
        }
        if (ours || !on_swaps()) return anything_new && take();
        // once every swapped screen is drawn; a swap already a VBlank old goes out with what there is
        return swap_due && fresh && (swapped_drawn() || waited) && take();
    }
    // any other look: true if a frame is there and its turn is open: the screens the game swapped, or a whole frame at a VBlank's turn with
    // the GPU thread idle for its whole wait
    bool between(bool quiet) {
        if (on_swaps()) return swap_due && fresh && swapped_drawn() && take();
        return quiet && due && fresh && (fresh & frame) == frame && take();
    }
    bool swapped_drawn() const { return (fresh & swap_due & frame) == (swap_due & frame); }
    bool on_swaps() const { return since_swap < kSwapping; }
    void reset() {
        due = false;
        swap_due = fresh = 0;
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
        swap_due = fresh = 0;
        return true;
    }
};
