#pragma once
#include <cstdint>
namespace wiiu_render {
inline bool darkmoon(std::uint64_t id) { return id == 0x0004000000055f00ULL; }
struct Policy { unsigned fps; float scale; int force; };
inline Policy darkmoon_policy(int mode, unsigned rows) {
    if (mode < 0 || mode > 2) mode = 0;
    const unsigned fps = mode == 2 ? 0 : 30;
    if (rows == 480) return {fps, 1.0f, 0};
    if (rows != 720) return {fps, 2.0f / 3, mode == 1 ? 0 : -1};
    if (mode == 1) return {fps, 1.0f, 0};
    return {fps, mode == 2 ? 2.0f / 3 : 0.85f, 1};
}
}
