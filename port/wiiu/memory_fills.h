// The memory fills that went to guest memory and still stand there (gx2_backend.cpp's gx2_memory_fill; tools/test_memory_fills.cpp runs
// it against made-up fills and writes). A fill of bytes that hold it already changes nothing, in memory or in a texture made from it, and
// costs real time: Pokémon Ultra Sun clears a depth buffer several times a frame that other surfaces share and a texture reads, 1.6 MB
// written and 512 KB uploaded a frame. Whatever else is written there forgets the fill. A store the game never tells of is caught only if
// it lands on one of the units looked at (holds).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// a memory fill as a game asks for it: control bit 0x100 for 24-bit units, 0x200 for 32-bit ones, neither for 16-bit ones
struct Fill {
    uint32_t start, end, value, control;
};

struct MemoryFills {
    std::vector<Fill> standing;
    bool notice_due = false;  // the notice of the newest one's own write comes next (written), and mustn't forget it

    void forget(uint32_t paddr, uint32_t size) {
        std::erase_if(standing, [&](const Fill &m) { return m.start < paddr + size && paddr < m.end; });
    }
    // a fill that is about to be written to guest memory
    void remember(const Fill &f) {
        forget(f.start, f.end - f.start);
        if (standing.size() >= 8) standing.erase(standing.begin());
        standing.push_back(f);
        notice_due = true;
    }
    // the runtime's notice that guest memory was written, or may have been
    void written(uint32_t paddr, uint32_t size) {
        const bool own = notice_due && !standing.empty() && paddr == standing.back().start && paddr + size == standing.back().end;
        notice_due = false;
        if (!own) forget(paddr, size);
    }
    // Whether a fill would change nothing: its bytes all lie in a standing fill of the same value, and 256 of its units in guest memory
    // (bytes: where f.start lies), the first and last among them, still read as it.
    bool holds(const Fill &f, const uint8_t *bytes) const {
        const uint32_t unit = f.control & 0x100 ? 3 : f.control & 0x200 ? 4 : 2, n = (f.end - f.start) / unit;
        const auto covers = [&](const Fill &m) {
            return m.value == f.value && m.control == f.control && m.start <= f.start && f.end <= m.end && (f.start - m.start) % unit == 0;
        };
        if (!bytes || !n || std::none_of(standing.begin(), standing.end(), covers)) return false;
        uint8_t want[4];
        for (uint32_t i = 0; i < 4; i++) want[i] = (uint8_t)(f.value >> (8 * i));
        for (uint32_t k = 0; k < 256; k++)
            if (memcmp(bytes + (size_t)((uint64_t)(n - 1) * k / 255) * unit, want, unit)) return false;
        return true;
    }
};
