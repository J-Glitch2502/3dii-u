// port/wiiu/screen_layouts.h on every TV size a Wii U gives (480p, 720p, 1080p) and its GamePad: every screen a layout shows lies inside the
// display with its own shape, the two never overlap, the GamePad's rows a screen keeps black beside it are its alone and hold it, a touch's
// corners land on the bottom screen's own, and the menu steps through every TV layout once. Built and run by tools/selftest.py.
#include <cstdio>
#include <cstdlib>

#include "screen_layouts.h"

using screens::Rect;
static int failures;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            printf("FAILED %s: ", #c);                 \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
            failures++;                                \
        }                                              \
    } while (0)

static bool inside(const Rect &r, unsigned w, unsigned h) { return r.w && r.h && r.x + r.w <= w && r.y + r.h <= h; }
static bool overlap(const Rect &a, const Rect &b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
// the 3DS's top screen is 400x240, its bottom one 320x240: a rect of either keeps that shape within a pixel or two of rounding
static bool shaped(const Rect &r, int which) { return abs((int)(r.w * 240) - (int)(r.h * (which ? 320 : 400))) <= 2 * 240; }

int main() {
    const unsigned tvs[][2] = {{854, 480}, {1280, 720}, {1920, 1080}};
    for (const auto &tv : tvs)
        for (int layout = 0; layout < screens::kTvLayouts; layout++) {
            Rect r[2];
            bool shown[2];
            for (int which = 0; which < 2; which++) {
                shown[which] = screens::tv_rect(layout, which, tv[0], tv[1], r[which]);
                if (shown[which]) {
                    CHECK(inside(r[which], tv[0], tv[1]), "TV %ux%u layout %d screen %d at %u,%u %ux%u", tv[0], tv[1], layout, which, r[which].x, r[which].y, r[which].w, r[which].h);
                    CHECK(shaped(r[which], which), "TV %ux%u layout %d screen %d is %ux%u", tv[0], tv[1], layout, which, r[which].w, r[which].h);
                }
            }
            CHECK(shown[0] == (layout != 4) && shown[1] == (layout != 0), "TV layout %d shows top %d bottom %d", layout, shown[0], shown[1]);
            if (shown[0] && shown[1]) CHECK(!overlap(r[0], r[1]), "TV %ux%u layout %d: the screens overlap", tv[0], tv[1], layout);
        }

    const unsigned dw = 854, dh = 480;
    for (int layout = 0; layout < screens::kPadLayouts; layout++) {
        Rect r[2];
        unsigned b0[2], b1[2];
        bool shown[2];
        for (int which = 0; which < 2; which++) {
            shown[which] = screens::pad_rect(layout, which, dw, dh, r[which], b0[which], b1[which]);
            if (!shown[which]) continue;
            CHECK(inside(r[which], dw, dh), "GamePad layout %d screen %d at %u,%u %ux%u", layout, which, r[which].x, r[which].y, r[which].w, r[which].h);
            CHECK(shaped(r[which], which), "GamePad layout %d screen %d is %ux%u", layout, which, r[which].w, r[which].h);
            CHECK(b0[which] <= r[which].y && r[which].y + r[which].h <= b1[which] && b1[which] <= dh, "GamePad layout %d screen %d outside its rows %u-%u", layout, which,
                  b0[which], b1[which]);
            CHECK(r[which].y == b0[which] && r[which].y + r[which].h == b1[which], "GamePad layout %d screen %d leaves rows of its band unpainted", layout, which);
        }
        CHECK(shown[0] == (layout != 0) && shown[1] == (layout != 1), "GamePad layout %d shows top %d bottom %d", layout, shown[0], shown[1]);
        if (shown[0] && shown[1]) CHECK(b1[0] <= b0[1] || b1[1] <= b0[0], "GamePad layout %d: the screens' rows overlap", layout);

        unsigned tx, ty;
        if (shown[1]) {
            const Rect &b = r[1];
            CHECK(screens::pad_touch(layout, b.x, b.y, dw, dh, tx, ty) && tx == 0 && ty == 0, "GamePad layout %d: the bottom screen's corner is %u,%u", layout, tx, ty);
            CHECK(screens::pad_touch(layout, b.x + b.w - 1, b.y + b.h - 1, dw, dh, tx, ty) && tx == 319 && ty == 239, "GamePad layout %d: the far corner is %u,%u",
                  layout, tx, ty);
            CHECK(!screens::pad_touch(layout, b.x + b.w, b.y, dw, dh, tx, ty), "GamePad layout %d: a touch right of the bottom screen counts", layout);
            if (b.x) CHECK(!screens::pad_touch(layout, b.x - 1, b.y, dw, dh, tx, ty), "GamePad layout %d: a touch left of the bottom screen counts", layout);
            if (b.y) CHECK(!screens::pad_touch(layout, b.x, b.y - 1, dw, dh, tx, ty), "GamePad layout %d: a touch above the bottom screen counts", layout);
        } else {
            for (unsigned y = 0; y < dh; y += 7)
                for (unsigned x = 0; x < dw; x += 7) CHECK(!screens::pad_touch(layout, x, y, dw, dh, tx, ty), "GamePad layout %d touches at %u,%u", layout, x, y);
        }
    }

    // the menu's TV line: every layout once each way round, back where it started; a number no layout has goes to the first
    for (int dir : {1, -1}) {
        bool seen[screens::kTvLayouts] = {};
        int layout = 0;
        for (int i = 0; i < screens::kTvLayouts; i++) {
            CHECK(layout >= 0 && layout < screens::kTvLayouts && !seen[layout], "the TV line (step %d) comes to %d twice", dir, layout);
            if (layout >= 0 && layout < screens::kTvLayouts) seen[layout] = true;
            layout = screens::next_tv_layout(layout, dir);
        }
        CHECK(layout == 0, "the TV line (step %d) ends at %d", dir, layout);
    }
    CHECK(screens::next_tv_layout(9, 1) == screens::kTvOrder[0], "an unknown layout steps to %d", screens::next_tv_layout(9, 1));

    if (failures) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("screen layouts: all TV and GamePad layouts right\n");
    return 0;
}
