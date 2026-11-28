#pragma once
#include <cstdint>

namespace gpu {
constexpr std::uint64_t kMaterialExtensionTag = 0x6d61742d65787431ull;  // optional PC-discovered material families
// The selected family must retain every active light. These are capacities,
// not reduced-quality modes; each shader still checks the actual light count.
constexpr unsigned kUberLightCaps[] = {0, 1, 2, 4, 8};
constexpr unsigned uber_light_bucket(unsigned lights) {
    for (unsigned i = 0; i < 5; ++i)
        if (lights <= kUberLightCaps[i]) return i;
    return 4;
}
}  // namespace gpu
