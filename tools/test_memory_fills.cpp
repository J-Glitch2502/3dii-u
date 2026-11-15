// port/wiiu/memory_fills.h against made-up fills of a made-up memory. A fill that was written stands: the same fill again, or one of
// bytes inside it with the same value, changes nothing. Another value, another unit size, bytes outside it, a write the runtime tells of,
// or a unit that reads otherwise where it is looked at, and the fill has to be written. Built and run by tools/selftest.py.
#include <cstdio>
#include <vector>

#include "memory_fills.h"

static int failed;
static void expect(bool ok, const char *what) {
    if (ok) return;
    printf("FAILED: %s\n", what);
    failed++;
}

static std::vector<uint8_t> mem(0x4000, 0xEE);
static MemoryFills fills;

// what the runtime does with a fill the renderer doesn't take whole (runtime/gpu/transfer.cpp's memory_fill): the renderer notes it,
// the bytes are written, the runtime says so
static void write_fill(const Fill &f) {
    const uint32_t unit = f.control & 0x100 ? 3 : f.control & 0x200 ? 4 : 2;
    fills.remember(f);
    for (uint32_t i = f.start; i + unit <= f.end; i += unit)
        for (uint32_t b = 0; b < unit; b++) mem[i + b] = (uint8_t)(f.value >> (8 * b));
    fills.written(f.start, f.end - f.start);
}
static bool holds(const Fill &f) { return fills.holds(f, &mem[f.start]); }

int main() {
    const Fill depth{0x1000, 0x3000, 0xFFFFFF00, 0x201};
    expect(!holds(depth), "a fill nobody wrote isn't held");
    write_fill(depth);
    expect(holds(depth), "the same fill again changes nothing");
    expect(holds({0x1000, 0x1800, 0xFFFFFF00, 0x201}), "nor one of its first bytes alone");
    expect(holds({0x1400, 0x1800, 0xFFFFFF00, 0x201}), "nor one of bytes in its middle");
    expect(holds({0x2FFC, 0x3000, 0xFFFFFF00, 0x201}), "nor one of its last unit");
    expect(!holds({0x1002, 0x1802, 0xFFFFFF00, 0x201}), "one that starts between two of its units has other bytes to write");
    expect(!holds({0x1000, 0x3004, 0xFFFFFF00, 0x201}), "one that reaches past it has bytes to write");
    expect(!holds({0x0FFC, 0x3000, 0xFFFFFF00, 0x201}), "so has one that starts before it");
    expect(!holds({0x1000, 0x3000, 0xFFFFFF01, 0x201}), "another value has to be written");
    expect(!holds({0x1000, 0x3000, 0xFFFFFF00, 0x001}), "and the same value in units of another size");
    // The units looked at aren't all of them, so what they read can't stand in for the rule. 511 16-bit units are looked at at every
    // second one, each the first half of a 32-bit unit, which reads as the 16-bit value: the other halves don't.
    expect(!holds({0x1000, 0x1000 + 2 * 511, 0xFFFFFF00, 0x001}), "units of another size that read right wherever they are looked at");
    // Bytes past the standing fill that read as it at the units looked at, and as something else between them
    const Fill longer{0x1000, 0x3400, 0xFFFFFF00, 0x201};
    for (uint32_t k = 0, n = (longer.end - longer.start) / 4; k < 256; k++) {
        const uint32_t at = longer.start + 4 * (uint32_t)((uint64_t)(n - 1) * k / 255);
        if (at >= depth.end) mem[at] = 0x00, mem[at + 1] = mem[at + 2] = mem[at + 3] = 0xFF;
    }
    expect(!holds(longer), "bytes past it that read right wherever they are looked at");

    // a unit that reads otherwise, where it is looked at: the first and the last always are
    mem[0x1001] ^= 1;
    expect(!holds(depth), "a first unit that reads otherwise");
    mem[0x1001] ^= 1;
    mem[0x2FFF] ^= 1;
    expect(!holds(depth), "a last unit that reads otherwise");
    expect(holds({0x1000, 0x2000, 0xFFFFFF00, 0x201}), "which a fill of the bytes before it doesn't look at");
    mem[0x2FFF] ^= 1;
    expect(holds(depth), "and put right, the fill stands as before");

    // writes the runtime tells of
    fills.written(0x3000, 0x100);
    fills.written(0x0F00, 0x100);
    expect(holds(depth), "a write beside it leaves it standing");
    fills.written(0x2FFF, 0x100);
    expect(!holds(depth), "a write over its last byte forgets it, whatever the bytes read");
    write_fill(depth);
    fills.forget(0x0800, 0x801);
    expect(!holds(depth), "and so does one over its first byte");

    // the newest fill's own notice is the one that mustn't forget it: any other notice in its place is a write like the rest
    fills.remember(depth);
    fills.written(depth.start, 0x100);
    expect(!holds(depth), "a notice of other bytes than the fill's, straight after it, is another write");
    write_fill(depth);
    fills.written(0x3800, 0x10);
    fills.written(depth.start, depth.end - depth.start);
    expect(!holds(depth), "and the same bytes written later, after its own notice, forget it too");

    // the other unit sizes, in the byte order the runtime writes them
    const Fill red{0x0000, 0x0800, 0xF800, 0x001};
    write_fill(red);
    expect(holds(red) && mem[0] == 0x00 && mem[1] == 0xF8, "16-bit units");
    const Fill grey{0x0800, 0x0800 + 3 * 100 + 2, 0x123456, 0x101};  // two bytes over a whole number of units: they aren't written
    write_fill(grey);
    expect(holds(grey) && mem[0x800] == 0x56 && mem[0x802] == 0x12 && mem[0x800 + 300] == 0xEE, "24-bit units");
    mem[0x800 + 3 * 99 + 2] ^= 1;
    expect(!holds(grey), "the last 24-bit unit's last byte is looked at");
    const Fill one{0x3F00, 0x3F04, 0xAABBCCDD, 0x201};
    write_fill(one);
    expect(holds(one), "a fill of one unit");

    // a fill over part of a standing one of another value: the old one is no longer whole
    write_fill(depth);
    write_fill({0x1800, 0x1900, 0, 0x201});
    expect(!holds(depth) && !holds({0x1000, 0x1800, 0xFFFFFF00, 0x201}), "a fill overwritten in part stands nowhere");

    // eight stand at most: the oldest goes
    fills = MemoryFills();
    for (uint32_t i = 0; i < 9; i++) write_fill({0x100 * i, 0x100 * i + 0x100, 0x11111111 * (i + 1), 0x201});
    expect(!holds({0x000, 0x100, 0x11111111, 0x201}) && holds({0x100, 0x200, 0x22222222, 0x201}) && holds({0x800, 0x900, 0x99999999, 0x201}),
           "eight fills stand at most, the newest ones");

    if (!failed) printf("ok\n");
    return failed ? 1 : 0;
}
