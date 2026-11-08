#!/bin/bash
# Self-check of f24's short path in gpu/pica.h against float_from_raw, for all 2^24 values. Runs in the contained environment.
set -e
RT=/project/third_party/3dsrecomp/runtime
T=$(mktemp -d)
sed -n '/^float float_from_raw(u32 hex, u32 M, u32 E) {/,/^}/p' $RT/gpu/gpu.cpp > "$T/raw.inc"
sed -n '/^inline float f24(u32 v) {/,/^}/p' $RT/gpu/pica.h > "$T/f24.inc"
cat > "$T/check.cpp" <<EOF
#include <cstdint>
#include <cstdio>
#include <cstring>
typedef uint32_t u32;
typedef int32_t s32;
#include "raw.inc"
#include "f24.inc"
int main() {
    for (u32 v = 0; v < 1u << 24; v++) {
        const float a = f24(v), b = float_from_raw(v, 16, 7);
        if (memcmp(&a, &b, 4)) {
            printf("different at %06x\n", v);
            return 1;
        }
    }
    puts("f24: all 2^24 values match");
}
EOF
clang++-18 -std=c++20 -O1 -I"$T" "$T/check.cpp" -o "$T/check"
"$T/check"
rm -rf "$T"
