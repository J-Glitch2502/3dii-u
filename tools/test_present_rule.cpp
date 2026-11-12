// port/wiiu/present_rule.h against made-up frame times: a game that makes a frame for every VBlank, each finished near the VBlank's own
// moment, a few milliseconds before or after. The old rule (a present only at the first look after a VBlank) loses the frames whose
// screens both land after that look; the new one must never show fewer frames than the old in any timing, lose a third as many at most
// where the old loses a fifth, never present twice for one VBlank, and put a top screen out without the bottom screen made with it no
// more often than the old rule did. Built and run by tools/selftest.py.
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

int main() {
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
