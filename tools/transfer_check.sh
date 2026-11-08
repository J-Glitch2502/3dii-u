#!/bin/bash
# Checks transfer_convert's tile-at-a-time 2x2 path against its pixel-by-pixel one, on random images: both are compiled from the real source.
# Runs in the contained environment: bash /project/tools/transfer_check.sh
GPU=/project/third_party/3dsrecomp/runtime/gpu
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fn() { sed -n "/^$1/,/^}/p" "$2"; }
{
    cat <<'EOF'
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using u8 = uint8_t;
using u32 = uint32_t;
inline u32 morton8(u32 x, u32 y) {
    static constexpr u8 xl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static constexpr u8 yl[8] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return xl[x & 7] + yl[y & 7];
}
inline u32 tiled_offset(u32 x, u32 y, u32 width, u32 bpp) { return ((y & ~7u) * width + (x & ~7u) * 8 + morton8(x, y)) * bpp; }
inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }
inline u32 pixel_bytes(u32 fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }
static inline u32 c5to8(u32 v) { return (v << 3) | (v >> 2); }
static inline u32 c6to8(u32 v) { return (v << 2) | (v >> 4); }
static inline u32 c4to8(u32 v) { return (v << 4) | v; }
EOF
    fn 'u32 decode_pixel' "$GPU/gpu.cpp"
    fn 'void encode_pixel' "$GPU/gpu.cpp"
    fn 'static void halve_tiled_rgba8' "$GPU/transfer.cpp"
    fn 'void transfer_convert' "$GPU/transfer.cpp"
    fn 'void transfer_convert' "$GPU/transfer.cpp" | sed -e 's/^void transfer_convert/void slow_convert/' -e 's/const bool halve = .*/const bool halve = false;/'
    cat <<'EOF'
static int compared;
static bool same(u32 w, u32 h, u32 flags) {
    const u32 dims = w | h << 16;
    // the pixel-by-pixel path reads a little past the end when a size isn't a multiple of 8
    std::vector<u8> in(w * h * 4 + 65536), a(w * h * 4, 0x55), b(w * h * 4, 0x55);
    for (auto &v : in) v = (u8)rand();
    transfer_convert(in.data(), a.data(), dims, dims, flags);
    slow_convert(in.data(), b.data(), dims, dims, flags);
    compared++;
    if (a != b) printf("different: %ux%u flags %08x\n", w, h, flags);
    return a == b;
}
int main() {
    static const u32 sizes[][2] = {{256, 512}, {128, 256}, {64, 128}, {240, 400}, {512, 512}, {32, 16}, {256, 100}, {100, 256}};
    for (auto &s : sizes)
        for (u32 extra : {0x20u, 0x21u, 0x22u, 0x23u, 0x00u, 0x01u})
            if (!same(s[0], s[1], 0x02000000 | extra)) return 1;
    // other scalings and formats stay on the old path
    for (u32 flags : {0x01000020u, 0x00000020u, 0x02001020u, 0x02000120u, 0x02002220u})
        if (!same(64, 64, flags)) return 1;
    printf("ok: %d transfers match\n", compared);
    return 0;
}
EOF
} > "$T/check.cpp"
clang++-18 -std=c++20 -O1 -w "$T/check.cpp" -o "$T/check" && "$T/check"
