// Differential checks for ARM byte-lane arithmetic used by movie filters.
#include "../third_party/3dsrecomp/runtime/cpu.h"
#include <cstdio>
#include <cstdlib>

static u32 reference(int kind, int sub, u32 a, u32 b, u32 *ge) {
    u32 out = 0;
    if (kind == 0 || kind == 3) *ge = 0;
    for (int i = 0; i < 4; ++i) {
        int x = kind < 3 ? (s8)(a >> (8*i)) : (u8)(a >> (8*i));
        int y = kind < 3 ? (s8)(b >> (8*i)) : (u8)(b >> (8*i));
        int r = sub ? x-y : x+y;
        if (kind == 0 && r >= 0) *ge |= 1u << i;
        if (kind == 3 && (sub ? r >= 0 : r >= 256)) *ge |= 1u << i;
        if (kind == 1) r = r > 127 ? 127 : r < -128 ? -128 : r;
        if (kind == 4) r = r > 255 ? 255 : r < 0 ? 0 : r;
        if (kind == 2 || kind == 5) r >>= 1;
        out |= (u32)(r & 255) << (8*i);
    }
    return out;
}
static void check(u32 a, u32 b, u32 initial) {
    for (int kind = 0; kind < 6; ++kind) for (int sub = 0; sub < 2; ++sub) {
        u32 eg = initial, ag = initial;
        u32 expected = reference(kind, sub, a, b, &eg), actual = pas8(kind, sub, a, b, &ag);
        if (actual != expected || ag != eg) {
            std::fprintf(stderr, "kind=%d sub=%d a=%08x b=%08x result=%08x/%08x GE=%x/%x\n",kind,sub,a,b,actual,expected,ag,eg);
            std::exit(1);
        }
    }
    for (u32 ge=0; ge<16; ++ge) {
        u32 expected=0;
        for (int i=0;i<4;++i) expected |= ((ge>>i&1) ? a : b) & (255u<<(8*i));
        if (sel(a,b,ge) != expected) std::exit(2);
    }
}
int main() {
    // Every pair of byte values in all lanes; neighbouring lanes vary to expose
    // accidental carry/borrow propagation between packed operations.
    for (u32 a=0;a<256;++a) for (u32 b=0;b<256;++b) {
        check(a*0x01010101u, b*0x01010101u, 0xa);
        check(a | ((255-a)<<8) | (b<<16) | ((255-b)<<24), b | (a<<8) | ((255-b)<<16) | ((255-a)<<24), 0x5);
    }
    u32 seed=0x87bc9012;
    auto next=[&] { seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; return seed; };
    for (int i=0;i<500000;++i) { u32 a=next(), b=next(), ge=next()&15; check(a,b,ge); }
    u32 ge=9,q=1;
    (void)pas_uqadd8(~0u,~0u,&ge,&q);
    if (ge!=9 || q!=1) return 3;
    std::puts("Passed: 631072 packed-word cases, all 12 byte operations, GE preservation, and all SEL masks");
}
