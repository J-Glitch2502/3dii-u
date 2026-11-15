// port/wiiu/present_rule.h against made-up frame times: a game that makes a frame for every VBlank, each finished near the VBlank's own
// moment, a few milliseconds before or after. The old rule (a present only at the first look after a VBlank) loses the frames whose
// screens both land after that look; the new one must never show fewer frames than the old in any timing, lose a third as many at most
// where the old loses a fifth, never present twice for one VBlank, and put a top screen out without the bottom screen made with it no
// more often than the old rule did. Built and run by tools/selftest.py.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "present_rule.h"

struct Result {
    int made, shown, presents, split;
};

// phase: when a frame's last screen is drawn, in ms after the VBlank it was made for; swing: how far that moves from frame to frame;
// every: a frame for each so many VBlanks; bottom_every: the bottom screen is drawn in each so many frames
static Result run(bool early, double phase, double swing, int every = 1, int bottom_every = 1) {
    const double period = 1000.0 / 59.94, quiet_wait = 2.0;
    PresentRule rule;
    struct Event {
        double t;
        int kind;  // 0 a VBlank's look, 1 top drawn, 2 bottom drawn, 3 a quiet look
        int frame;
    };
    std::vector<Event> events;
    unsigned seed = 12345;
    const int vblanks = 6000;
    int made = 0;
    for (int k = 0; k < vblanks; k++) {
        events.push_back({k * period + 0.3, 0, 0});  // the GPU thread sees the flag a moment after the VBlank
        if (k % every) continue;
        seed = seed * 1103515245u + 12345u;
        const double done = k * period + phase + swing * ((int)((seed >> 8) % 2001) - 1000) / 1000.0;
        made++;
        events.push_back({done - 1.0, 1, made});
        if (made % bottom_every == 0) events.push_back({done, 2, made});
        events.push_back({done + quiet_wait, 3, made});
    }
    for (size_t i = 1; i < events.size(); i++)  // into time order
        for (size_t j = i; j > 0 && events[j].t < events[j - 1].t; j--) std::swap(events[j], events[j - 1]);
    Result r{made, 0, 0, 0};
    int top = 0, bottom = 0, shown_top = 0, presents_since_vblank = 0;
    bool new_screen = false;
    std::vector<bool> seen(made + 1);
    for (const Event &e : events) {
        bool present = false;
        if (e.kind == 1) {
            top = e.frame;
            rule.drew(0);
            new_screen = true;
        } else if (e.kind == 2) {
            bottom = e.frame;
            rule.drew(1);
            new_screen = true;
        } else if (e.kind == 0) {
            presents_since_vblank = 0;
            present = rule.vblank(new_screen);
        } else if (early) {
            present = rule.between(true);
        }
        if (!present) continue;
        new_screen = false;
        r.presents++;
        if (++presents_since_vblank > 1) {
            printf("two presents for one VBlank at %.1f ms\n", e.t);
            exit(1);
        }
        if (bottom_every == 1 && top != bottom) r.split++;
        if (top != shown_top && !seen[top]) {
            seen[top] = true;
            r.shown++;
        }
        shown_top = top;
    }
    return r;
}

// A game making a frame every so many VBlanks and swapping to it at that VBlank (Pokemon Ultra Sun's field: every other one), its picture
// drawn here from soon after the swap before to 10 ms after its own (the game's drawing time and the GPU thread's lag), the GPU thread busy
// at times, the bottom screen drawn (and swapped) with every frame, every other one (Mario Kart 7), or once only. A frame should stay up that
// many refreshes, as on a 3DS: how many didn't, and whether a VBlank ever saw two presents. swaps: the rule is told of the swaps.
struct Pace {
    int bottom_every = 1;        // the bottom screen swapped (and drawn) in each so many frames, 0: the first only
    bool bottom_drawn = true;    // false: swapped but never drawn here (a screen this backend doesn't take as one)
    int swaps_until = 1 << 30;   // the game stops swapping after this frame, and draws on
    int split = 0, longest = 0;  // out: presents of a top screen with another frame's bottom (bottom drawn with every frame); most VBlanks between presents
};
static int uneven(bool swaps, int *frames_shown, int every = 2, int bottom_every = 1, Pace *pace = nullptr) {
    Pace own;
    Pace &p = pace ? *pace : own;
    if (!pace) p.bottom_every = bottom_every;
    const double period = 1000.0 / 59.94;
    PresentRule rule;
    struct Event {
        double t;
        int kind;  // 0 a VBlank's look (1: with the game's swap), 2 top drawn, 3 bottom drawn, 4 a look after a quiet wait, 5 a busy look
        int frame;
    };
    std::vector<Event> events;
    const auto bottom = [&](int j) { return p.bottom_every ? j % p.bottom_every == 0 : j == 1; };
    unsigned seed = 777;
    const int frames = 3000;
    const double early = every * period - 3.4, late = 10.0;
    double before = 0;
    for (int j = 1; j <= frames; j++) {
        const double swap = every * j * period;
        for (int v = 0; v < every; v++) events.push_back({swap + v * period + 0.3, v || j > p.swaps_until ? 0 : 1, j});
        seed = seed * 1103515245u + 12345u;
        const double done = std::max(swap - early + (early + late) * ((seed >> 8) % 1001) / 1000.0, before + 5.0);  // in order, as the GPU thread draws them
        before = done;
        events.push_back({done - 1.0, 2, j});
        if (bottom(j) && p.bottom_drawn) events.push_back({done, 3, j});
        events.push_back({done + 2.0, (seed >> 20) % 3 ? 4 : 5, j});
    }
    for (size_t i = 1; i < events.size(); i++)
        for (size_t j = i; j > 0 && events[j].t < events[j - 1].t; j--) std::swap(events[j], events[j - 1]);
    int top = 0, bot = 0, last_top = 0, last_vsync = -1, last_present = -1, odd = 0, shown = 0, presents_since_vblank = 0;
    bool new_screen = false;
    for (const Event &e : events) {
        bool present = false;
        if (e.kind == 2 || e.kind == 3) {
            (e.kind == 2 ? top : bot) = e.frame;
            rule.drew(e.kind - 2);
            new_screen = true;
        } else if (e.kind <= 1) {
            presents_since_vblank = 0;
            present = rule.vblank(new_screen, swaps && e.kind == 1 ? 1u | (bottom(e.frame) ? 2u : 0u) : 0u);
        } else {
            present = rule.between(e.kind == 4);
        }
        if (!present) continue;
        new_screen = false;
        if (++presents_since_vblank > 1) return printf("two presents for one VBlank at %.1f ms\n", e.t), -1;
        const int vsync = (int)(e.t / period) + 1;  // a present reaches the TV at the next refresh
        if (last_present >= 0 && top > 20) p.longest = std::max(p.longest, vsync - last_present);
        last_present = vsync;
        if (p.bottom_every == 1 && p.bottom_drawn && top != bot) p.split++;
        if (top == last_top) continue;
        if (last_vsync >= 0 && vsync - last_vsync != every && top > 20 && top <= p.swaps_until) odd++;  // (once a screen drawn once only has dropped out of a frame: 8 presents)
        last_vsync = vsync;
        last_top = top;
        shown++;
    }
    *frames_shown = shown;
    return odd;
}

int main() {
    {
        int shown_old = 0, shown_new = 0;
        const int old_odd = uneven(false, &shown_old), new_odd = uneven(true, &shown_new);
        printf("a frame every other VBlank: shown for other than two refreshes %d of %d times by the old rule, %d of %d on the game's swaps\n", old_odd, shown_old,
               new_odd, shown_new);
        if (old_odd < 0 || new_odd < 0) return 1;
        if (old_odd < shown_old / 20) return printf("the old rule kept the pace: the test no longer shows the fault\n"), 1;
        if (new_odd || shown_new < 2990) return printf("on the game's swaps, frames still stood other than two refreshes, or went unshown\n"), 1;
        // a frame for every VBlank, a bottom screen every other frame, and one drawn once and then left: on the swaps every frame still goes
        // out on its refresh
        for (int every = 1; every <= 2; every++)
            for (int bottom_every : {1, 2, 0}) {
                Pace p;
                p.bottom_every = bottom_every;
                const int odd = uneven(true, &shown_new, every, bottom_every, &p);
                if (odd || shown_new < 2990 || p.split)
                    return printf("a frame every %d VBlank(s), the bottom screen every %d frames (0: once): %d of 3000 shown, %d not %d refreshes, %d with "
                                  "another frame's bottom screen\n", every, bottom_every, shown_new, odd, every, p.split), 1;
            }
        // a bottom screen swapped with every frame but never drawn here: the top isn't held back for it
        Pace undrawn;
        undrawn.bottom_drawn = false;
        if (uneven(true, &shown_new, 1, 1, &undrawn) || shown_new < 2990)
            return printf("a bottom screen swapped but never drawn: %d of 3000 shown, or not one refresh each\n", shown_new), 1;
        // a game that stops swapping halfway and draws on: shown the old way once it hasn't swapped for a while, after a pause no longer than
        // that, so the second half loses no more than the old rule does there and the frames of that pause
        Pace stops;
        stops.swaps_until = 1500;
        uneven(true, &shown_new, 2, 1, &stops);
        if (shown_new < 1500 + shown_old / 2 - PresentRule::kSwapping || stops.longest > PresentRule::kSwapping + 2)
            return printf("swaps that stop: %d of 3000 shown (the old rule's half %d), %d refreshes without a present\n", shown_new, shown_old / 2, stops.longest), 1;
    }
    // frames finished around the VBlank, up to 3 ms either side of it
    const Result before = run(false, 16.7, 3.0), after = run(true, 16.7, 3.0);
    printf("finished within 3 ms of the VBlank: the old rule shows %d of %d (%d with the screens apart), the new %d of %d (%d)\n", before.shown, before.made,
           before.split, after.shown, after.made, after.split);
    if (before.shown > before.made * 9 / 10) return printf("the old rule lost little: the test no longer shows the fault\n"), 1;
    // What is still lost: a late frame whose VBlank's turn went to the late bottom screen of the frame before it. A third of the old loss at most.
    if (after.shown < before.shown || (after.made - after.shown) * 3 > before.made - before.shown) return printf("frames lost\n"), 1;
    if (after.split > before.split) return printf("screens apart more often than before\n"), 1;
    // every phase of a frame's end within two VBlanks, steady and swinging; a 30-frames-a-second game; a bottom screen drawn every
    // other frame, and one drawn once and then left
    for (int every = 1; every <= 2; every++)
        for (int bottom_every : {1, 2, 100000})
            for (double phase = 1.0; phase < 34.0; phase += 0.7)
                for (double swing = 0; swing <= 3.0; swing += 1.5) {
                    const Result r = run(true, phase, swing, every, bottom_every), old = run(false, phase, swing, every, bottom_every);
                    if (r.shown < old.shown || r.split > old.split)
                        return printf("phase %.1f swing %.1f every %d bottom every %d: %d of %d shown (the old rule %d), screens apart %d (%d)\n", phase, swing, every,
                                      bottom_every, r.shown, r.made, old.shown, r.split, old.split), 1;
                }
    printf("ok\n");
    return 0;
}
