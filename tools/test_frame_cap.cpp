// port/wiiu/frame_cap.h against made-up games, used the way gx2_backend.cpp uses it: the drawing thread says when work comes after it has
// waited, a draw to a surface is dropped while the rule says so, and a transfer that ends a left-out frame shows nothing. What has to
// hold for every game here: a picture that is shown is that screen's own and whole; a screen the game keeps drawing is never left
// standing; a game at sixty has half its frames drawn; a game made for thirty keeps its frames, however long they take to draw and
// however unevenly they come. Cemu on a PC draws in no time and at an even pace, so a console's timing can only be played here.
// Built and run by tools/selftest.py.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#include "frame_cap.h"

typedef unsigned long long u64;
static const u64 VBLANK = 16683;  // microseconds
static const u64 WAIT = 2000;     // the drawing thread has waited once it has had nothing to do for this long (frontend_wiiu.cpp)

// The rule as it was released twice on 2026-11-12 (edebc80): a frame came "soon" if it ended under 29 ms after the one before, and a
// marking that failed before it had held rested. Kept so the test shows the fault that the game's own pace is there for.
struct EndsCap {
    struct Screen {
        u64 last = 0, until = 0, rest = 0;
        unsigned held = 0;
        bool skip = false, engaged = false;
    };
    Screen screen[2];
    bool drops(unsigned lcds) const { return lcds && screen[lcds == 2].skip; }
    bool frame_ends(int which, bool shared, bool redrawn, u64 now) {
        if (which == 1 && shared) return screen[0].skip;
        Screen &c = screen[which];
        const bool was = c.skip, fast = now - c.last < 29000;
        c.last = now;
        if (was) {
            c.engaged = fast;
            if (fast) {
                if (++c.held == 30) c.rest = 0;
            } else {
                if (c.held < 30) {
                    c.rest = c.rest ? std::min<u64>(c.rest * 2, 120000000) : 100000;
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
static void arrived(EndsCap &, u64) {}
static bool ended(EndsCap &cap, int which, bool shared, bool redrawn, u64 now) { return cap.frame_ends(which, shared, redrawn, now); }
static void arrived(FrameCap &cap, u64 now) { cap.work_arrived(now); }
static bool ended(FrameCap &cap, int which, bool shared, bool redrawn, u64) { return cap.frame_ends(which, shared, redrawn); }

struct Surface {
    unsigned lcds = 0;
    int screen = -1, frame = -1, parts = 0;  // whose picture it holds, of which frame, and how many of that frame's two draws
};

struct Game {
    bool shared = true;    // both screens are drawn in one surface
    int order = 0;         // 0: the top is drawn and sent first, 1: the bottom
    bool cleared = false;  // screens with surfaces of their own: the game clears them every frame
    int frames = 600;
    std::function<u64(int)> period = [](int) { return VBLANK; };  // from when a frame's work comes to when the next one's does, by frame number
    std::function<u64(int)> draw_time = [](int) { return (u64)0; };  // what drawing a top screen takes: next to nothing in Cemu, 15 to 30 ms on a console
    int top_stops = 1 << 30, bottom_stops = 1 << 30;  // the frame from which the game leaves that screen as it is
    int bottoms = 1;     // bottom screens drawn and sent a frame
    u64 screen_lag = 0;  // from the first screen's transfer to when the second screen's work comes
    u64 begin = 10 * VBLANK + 5000;  // when its first frame's work comes: 5 ms after a VBlank's moment
};

struct Seen {
    std::vector<char> shown[2];  // for each frame the game made of a screen: whether it was shown
    int count(int s, int from, int to) const {
        int n = 0;
        for (int f = from; f < to && f < (int)shown[s].size(); f++) n += shown[s][f];
        return n;
    }
    int lost(int s, int from = 0) const { return (int)shown[s].size() - from - count(s, from, 1 << 30); }
    // the longest run of frames of a screen that weren't shown, among those made from `from` on
    int gap(int s, int from) const {
        int worst = 0, run = 0;
        for (int f = from; f < (int)shown[s].size(); f++) {
            run = shown[s][f] ? 0 : run + 1;
            worst = std::max(worst, run);
        }
        return worst;
    }
};

static const char *g_name = "";

static void fail(const char *what, int screen, int frame) {
    printf("%s: %s (%s screen, its frame %d)\n", g_name, what, screen ? "bottom" : "top", frame);
    exit(1);
}

template <class Rule>
static Seen play(const Game &g) {
    Rule cap;
    Surface surfaces[2];
    Seen seen;
    u64 now = g.begin, busy_until = 0;  // the drawing thread is at work until then
    int count[2] = {0, 0};              // frames made of each screen so far
    for (int f = 0; f < g.frames; f++) {
        const u64 start = now;
        bool first = true;
        for (int step = 0; step < 2; step++) {
            const int screen = g.order ? 1 - step : step;
            if (f >= (screen ? g.bottom_stops : g.top_stops)) continue;
            for (int k = 0; k < (screen ? g.bottoms : 1); k++) {
                if (!first) now += g.screen_lag;
                first = false;
                if (now >= busy_until + WAIT) arrived(cap, now);
                Surface &s = surfaces[g.shared ? 0 : screen];
                const int frame = count[screen]++;
                bool dropped = false;
                for (int part = 0; part < 2; part++) {  // two draws, with the rule asked at each as left_out is
                    if (cap.drops(s.lcds)) {
                        dropped = true;
                        continue;
                    }
                    if (s.screen != screen || s.frame != frame) s = {s.lcds, screen, frame, 0};
                    s.parts++;
                }
                if (screen == 0 && !dropped) now += g.draw_time(f);
                // the transfer, as metal_defer_transfer and frame_left_out make it
                s.lcds |= 1u << screen;
                const bool out = ended(cap, screen, s.lcds == 3, s.lcds == 3 || g.cleared, now);
                busy_until = now;
                if (!out) {
                    if (s.screen != screen || s.frame != frame) fail("a picture shown that is another screen's or an older frame's", screen, frame);
                    if (s.parts != 2) fail("a picture shown with half its draws", screen, frame);
                } else if (!dropped) {
                    fail("a frame drawn and then not shown", screen, frame);
                }
                seen.shown[screen].push_back(!out);
            }
        }
        now = std::max(now, start + g.period(f));  // a game whose frame isn't drawn yet waits for it
    }
    return seen;
}

static void check(bool ok, const char *what) {
    if (ok) return;
    printf("%s: %s\n", g_name, what);
    exit(1);
}

// a number from a frame's number, the same every run
static unsigned noise(int f, unsigned salt) {
    unsigned x = ((unsigned)f + salt) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    return x ^ (x >> 13);
}

int main() {
    char name[200];
    g_name = name;
    const int W = 40;  // frames a game is given to settle before it is counted
    for (int shared = 0; shared <= 1; shared++)
        for (int order = 0; order <= 1; order++) {
            const char *how = shared ? "one surface" : "two surfaces";
            const char *ord = order ? "bottom first" : "top first";
            const bool say = shared && !order;  // the numbers once, for the commonest layout
            Game base;
            base.shared = shared;
            base.order = order;
            base.cleared = !shared;

            snprintf(name, sizeof name, "sixty a second, %s, %s", how, ord);
            Seen s = play<FrameCap>(base);
            for (int sc = 0; sc < 2; sc++) {
                check(s.gap(sc, W) == 1, "a screen was left for more than one frame, or never left out");
                check(std::abs(s.count(sc, W, 600) * 2 - (600 - W)) <= 2, "not half the frames drawn");
            }

            snprintf(name, sizeof name, "thirty a second, even, %s, %s", how, ord);
            Game g = base;
            g.period = [](int) { return 2 * VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a thirty-a-second game was left out");

            // Super Mario 3D Land on a console (2026-11-12): made for thirty, 15 to 25 ms to draw a top screen. "Speed is fine, it just
            // feels like 20fps": the rule that went by frames' ends kept half of them, as it has to here.
            snprintf(name, sizeof name, "thirty a second, 15 to 25 ms to draw, %s, %s", how, ord);
            g = base;
            g.frames = 3000;
            g.period = [](int) { return 2 * VBLANK; };
            g.draw_time = [](int f) { return (u64)15000 + noise(f, 3) % 10001; };
            const Seen old = play<EndsCap>(g);
            check(old.lost(0) > 900, "the rule that went by frames' ends loses few frames here: the test no longer shows its fault");
            s = play<FrameCap>(g);
            if (say) printf("thirty a second, 15 to 25 ms to draw: the rule that went by frames' ends leaves out %d of %d, this one %d\n", old.lost(0), g.frames, s.lost(0));
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a thirty-a-second game was left out for the time it takes to draw");

            snprintf(name, sizeof name, "thirty a second that the console takes 30 ms to draw, %s, %s", how, ord);
            g = base;
            g.period = [](int) { return 2 * VBLANK; };
            g.draw_time = [](int) { return (u64)30000; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a thirty-a-second game was left out");

            snprintf(name, sizeof name, "thirty a second that the console takes 40 ms to draw, so twenty-five, %s, %s", how, ord);
            g = base;
            g.period = [](int) { return 2 * VBLANK; };
            g.draw_time = [](int) { return (u64)40000; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a game that can't keep its thirty was left out");

            // thirty a second, each frame's work coming within a millisecond and a half of a VBlank's moment (what counting VBlanks got wrong)
            snprintf(name, sizeof name, "thirty a second, uneven by a millisecond or two, %s, %s", how, ord);
            g = base;
            g.frames = 3000;
            g.begin = 100 * VBLANK;
            g.period = [](int f) { return 2 * VBLANK + noise(f, 1) % 3001 - (f ? noise(f - 1, 1) % 3001 : 1500); };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a thirty-a-second game was left out");

            snprintf(name, sizeof name, "thirty a second, a late frame every two seconds and the next one early, %s, %s", how, ord);
            g = base;
            g.frames = 9000;
            g.period = [](int f) { return f % 60 == 30 ? 2 * VBLANK + 12000 : f % 60 == 31 ? 2 * VBLANK - 12000 : 2 * VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a thirty-a-second game was left out");

            // thirty a second that stalls for 100 ms every four seconds and makes its next three frames a VBlank apart to catch up
            snprintf(name, sizeof name, "thirty a second, a stall every four seconds and three quick frames after it, %s, %s", how, ord);
            g = base;
            g.frames = 9000;
            g.period = [](int f) { return f % 120 == 60 ? (u64)100000 : f % 120 > 60 && f % 120 <= 63 ? VBLANK : 2 * VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a thirty-a-second game was left out");

            snprintf(name, sizeof name, "sixty, then thirty from frame 200, %s, %s", how, ord);
            g = base;
            g.period = [](int f) { return f < 200 ? VBLANK : 2 * VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) {
                check(s.gap(sc, 200) <= 1, "a screen was left standing once the game slowed down");
                check(s.lost(sc, 206) == 0, "frames still left out six frames after the game slowed down");
            }

            snprintf(name, sizeof name, "thirty, then sixty from frame 200, %s, %s", how, ord);
            g = base;
            g.period = [](int f) { return f < 200 ? 2 * VBLANK : VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) {
                check(s.lost(sc, 0) - s.lost(sc, 200) == 0, "a frame left out while the game made thirty");
                check(std::abs(s.count(sc, 220, 600) * 2 - 380) <= 2, "not half the frames drawn within twenty frames of the game making sixty");
            }

            snprintf(name, sizeof name, "twenty a second with the second screen two VBlanks behind the first, %s, %s", how, ord);
            g = base;
            g.period = [](int) { return 3 * VBLANK; };
            g.screen_lag = 2 * VBLANK;
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame of a slow game was left out");

            // A game made for sixty on a console that takes 30 ms to draw its top screen (New Super Mario Bros. 2's castles): the left-out
            // frames cost nothing, the game gets its sixty back, and the leaving out has to hold. A bottom screen with a surface of its own
            // is paced by the top's drawing.
            for (u64 ms : {20, 30, 40}) {
                snprintf(name, sizeof name, "sixty, then a console that takes %d ms to draw the top screen, %s, %s", (int)ms, how, ord);
                g = base;
                g.draw_time = [ms](int f) { return f < 200 ? (u64)0 : ms * 1000; };
                s = play<FrameCap>(g);
                check(s.gap(0, W) == 1, "the top screen was left for more than one frame, or the leaving out stopped where it is needed");
                check(std::abs(s.count(0, 200, 600) * 2 - 400) <= 2, "not half the top screen's frames drawn where the console can't keep up");
                check(s.gap(1, W) <= 1, "the bottom screen was left for more than one frame");
            }

            // a game at sixty that takes 40 ms over a frame every two seconds: one slow pace in many
            snprintf(name, sizeof name, "sixty a second with a 40 ms frame every two seconds, %s, %s", how, ord);
            g = base;
            g.frames = 6000;
            g.period = [](int f) { return f % 120 == 60 ? (u64)40000 : VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) {
                check(s.gap(sc, W) <= 1, "a screen was left for more than one frame");
                check(std::abs(s.count(sc, W, 6000) * 2 - (6000 - W)) <= 4, "a stumble stops the leaving out: not half the frames drawn");
            }

            for (int phase = 0; phase <= 1; phase++) {
                // frame_cap.h's second shortcut: in one surface the bottom screen stands once the top has stopped on a marked frame
                snprintf(name, sizeof name, "the top screen stops at frame %d, %s, %s", 200 + phase, how, ord);
                g = base;
                g.top_stops = 200 + phase;
                s = play<FrameCap>(g);
                if (!shared) check(s.gap(1, W) <= 1, "the bottom screen was left for more than one frame");

                snprintf(name, sizeof name, "the bottom screen stops at frame %d, %s, %s", 200 + phase, how, ord);
                g = base;
                g.bottom_stops = 200 + phase;
                s = play<FrameCap>(g);
                check(s.gap(0, W) == 1, "the top screen was left for more than one frame, or never left out");

                // a load: nothing drawn for a second, one frame, nothing for a second, then on. frame_cap.h's first shortcut: the frame
                // right before a pause, or the one frame between two, can be a marked one; never more than that one.
                snprintf(name, sizeof name, "a second's pause after frame %d, one frame, another pause, %s, %s", 200 + phase, how, ord);
                g = base;
                const int p = 200 + phase;
                g.period = [p](int f) { return f == p || f == p + 1 ? 60 * VBLANK : VBLANK; };
                s = play<FrameCap>(g);
                for (int sc = 0; sc < 2; sc++) {
                    check(s.shown[sc][p] || s.shown[sc][p - 1], "a pause began with a picture two frames old or older");
                    check(s.gap(sc, W) <= 1, "a screen was left for more than one frame");
                    check(std::abs(s.count(sc, 260, 600) * 2 - 340) <= 2, "the leaving out didn't go on after the pauses");
                }
            }

            // a game made for sixty that takes two VBlanks for a frame one time in three: whichever way it is taken, nothing shown wrong
            snprintf(name, sizeof name, "frames one or two VBlanks apart at random, %s, %s", how, ord);
            g = base;
            g.frames = 4000;
            g.period = [](int f) { return noise(f, 7) % 3 == 0 ? 2 * VBLANK : VBLANK; };
            s = play<FrameCap>(g);
            for (int sc = 0; sc < 2; sc++) check(s.gap(sc, W) <= 1, "a screen was left for more than one frame");
        }

    snprintf(name, sizeof name, "two bottom screens sent for each top screen, one surface");
    Game g;
    g.bottoms = 2;
    Seen s = play<FrameCap>(g);
    check(s.gap(1, 40) <= 2 && s.gap(0, 40) <= 1, "a screen was left standing");

    snprintf(name, sizeof name, "screens of their own that the game doesn't clear");
    g = Game();
    g.shared = false;
    s = play<FrameCap>(g);
    for (int sc = 0; sc < 2; sc++) check(s.lost(sc) == 0, "a frame was left out of a picture the game isn't sure to draw again");

    printf("ok\n");
    return 0;
}
