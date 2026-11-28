// Guest-byte ownership for GPU readback; pixel conversion and tiling stay in gx2_backend.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace surface_readback {
struct Span {
    uint64_t lo, hi;
    template<class Archive> void state_fields(Archive &a) { a.fields(lo, hi); }
};
using Spans = std::vector<Span>;
inline Span bounds(uint32_t addr, uint32_t size) { return {addr, std::min(uint64_t(addr) + size, uint64_t(1) << 32)}; }
inline bool overlaps(Span a, Span b) { return a.lo < a.hi && b.lo < b.hi && a.lo < b.hi && b.lo < a.hi; }

inline void add(Spans &spans, Span s) {
    if (s.lo >= s.hi) return;
    auto at = spans.begin();
    while (at != spans.end() && at->hi < s.lo) ++at;
    while (at != spans.end() && at->lo <= s.hi) {
        s.lo = std::min(s.lo, at->lo);
        s.hi = std::max(s.hi, at->hi);
        at = spans.erase(at);
    }
    spans.insert(at, s);
}

inline void subtract(Spans &spans, Span cut) {
    for (size_t i = 0; i < spans.size(); ++i) {
        const Span s = spans[i];
        if (!overlaps(s, cut)) continue;
        if (s.lo < cut.lo) {
            spans[i].hi = cut.lo;
            if (cut.hi < s.hi) spans.insert(spans.begin() + ++i, {cut.hi, s.hi});
        } else if (cut.hi < s.hi) spans[i].lo = cut.hi;
        else spans.erase(spans.begin() + i--);
    }
}

template<class Surface>
void guest_wrote(Surface &s, uint32_t addr, uint32_t size) {
    const Span write = bounds(addr, size), target = bounds(s.addr, s.bytes());
    if (!overlaps(write, target)) return;
    if (!s.guest_newer || s.written_after != s.drawn) s.written.clear();
    s.written_after = s.drawn;
    add(s.written, {std::max(write.lo, target.lo), std::min(write.hi, target.hi)});
    s.guest_newer = true;
    if (write.lo <= target.lo && write.hi >= target.hi) {
        s.gpu_newer = false;
        s.first_use = true;
    }
}

template<class Surface, class Surfaces>
Spans owned(const Surface &s, const Surfaces &surfaces) {
    const Span target = bounds(s.addr, s.bytes());
    if (target.lo >= target.hi) return {};
    Spans spans{target};
    // A newer alias still owns its bytes after they have reached guest memory.
    for (const auto &other : surfaces)
        if (!other->depth && other->drawn > s.drawn) subtract(spans, bounds(other->addr, other->bytes()));
    if (s.guest_newer && s.written_after == s.drawn)
        for (const Span span : s.written) subtract(spans, span);
    return spans;
}

template<class Surfaces, class WriteBack>
void read(Surfaces &surfaces, uint32_t addr, uint32_t size, WriteBack write_back) {
    const Span request = bounds(addr, size);
    for (auto &s : surfaces) {
        if (s->depth || !s->gpu_newer || !overlaps(bounds(s->addr, s->bytes()), request)) continue;
        const Spans spans = owned(*s, surfaces);
        if (std::any_of(spans.begin(), spans.end(), [&](Span span) { return overlaps(span, request); })) write_back(*s, spans);
    }
}

template<class Surface, class Surfaces>
bool retire(const Surface &s, Surfaces &surfaces) {
    if (s.depth) return true;
    // An idle timeout cannot establish that the guest still uses this address for pixels.
    if (s.gpu_newer && !owned(s, surfaces).empty()) return false;
    // Removing an alias must not give its bytes back to older GPU pictures.
    // Their exclusions survive eviction, until a fresh draw reclaims them.
    for (auto &older : surfaces)
        if (!older->depth && older->drawn < s.drawn) guest_wrote(*older, s.addr, s.bytes());
    return true;
}

// Publishing GPU bytes also invalidates cached attributes/indices made from the
// previous guest bytes, including bytes beyond the CPU's triggering read request.
template<class Invalidate>
void copy_owned(uint8_t *guest, const uint8_t *converted, uint32_t addr, const Spans &spans, Invalidate invalidate) {
    for (const Span span : spans) {
        const size_t offset = size_t(span.lo - addr);
        if (guest != converted) memcpy(guest + offset, converted + offset, size_t(span.hi - span.lo));
        invalidate(uint32_t(span.lo), uint32_t(span.hi - span.lo));
    }
}
}  // namespace surface_readback
