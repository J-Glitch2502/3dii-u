#pragma once
#include <cstdint>
#include <limits>
namespace wiiu::quota_policy {
constexpr uint64_t unit=4ull<<20, headroom=1ull<<20, maximum=4ull<<30;
inline uint64_t required(uint64_t quota,uint64_t free,uint64_t old_file,uint64_t incoming) {
    if(free>quota || quota>maximum || incoming>maximum) return 0;
    const uint64_t extra=incoming>old_file?incoming-old_file:0;
    if(extra<=free && headroom<=free-extra) return quota;
    const uint64_t used=quota-free;
    if(extra>maximum-used || headroom>maximum-used-extra) return 0;
    const uint64_t need=used+extra+headroom;
    if(need>maximum-(unit-1)) return 0;
    return (need+unit-1)&~(unit-1);
}
}
