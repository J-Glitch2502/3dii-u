#include "surface_readback.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <random>

namespace sr = surface_readback;
struct Surface {
    uint32_t addr, size;
    uint64_t drawn, written_after = 0;
    uint8_t value;
    bool depth = false, gpu_newer = true, guest_newer = false, first_use = false;
    sr::Spans written;
    uint32_t bytes() const { return size; }
};
using Surfaces = std::vector<std::unique_ptr<Surface>>;
using Memory = std::array<uint8_t, 128>;
static Surface &add(Surfaces &surfaces, uint32_t addr, uint32_t size, uint64_t drawn, uint8_t value) {
    surfaces.push_back(std::make_unique<Surface>(Surface{addr, size, drawn, 0, value}));
    return *surfaces.back();
}
static void materialize(Surface &s, Memory &memory, const sr::Spans &owned) {
    std::vector<uint8_t> converted(s.size, s.value);
    sr::copy_owned(memory.data() + s.addr, converted.data(), s.addr, owned, [](uint32_t, uint32_t) {});
    s.gpu_newer = false;
    s.written.clear();
}
static int read(Surfaces &surfaces, Memory &memory, uint32_t addr, uint32_t size) {
    int count = 0;
    sr::read(surfaces, addr, size, [&](Surface &s, const sr::Spans &owned) {
        materialize(s, memory, owned);
        ++count;
    });
    return count;
}
static void retire(Surfaces &surfaces, size_t index) {
    assert(sr::retire(*surfaces[index], surfaces));
    surfaces.erase(surfaces.begin() + index);
}
static void cpu_write(Surfaces &surfaces, Memory &memory, uint32_t addr, uint32_t size, uint8_t value) {
    std::fill_n(memory.begin() + addr, size, value);
    for (auto &s : surfaces) sr::guest_wrote(*s, addr, size);
}

int main() {
    for (bool partial : {false, true}) {
        Surfaces surfaces;
        add(surfaces, 16, 80, 1, 7);
        Memory memory{};
        if (partial) {
            cpu_write(surfaces, memory, 32, 8, 9);
            add(surfaces, 48, 8, 2, 5).gpu_newer = false;
            std::fill_n(memory.begin() + 48, 8, 5);
        }
        // Both attribute and index entries lie beyond the four-byte CPU read.
        const sr::Span entries[] = {{72, 80}, {80, 84}, {32, 40}, {48, 56}, {0, 8}};
        bool cached[] = {true, true, true, true, true};
        sr::Spans invalidated;
        sr::read(surfaces, 16, 4, [&](Surface &surface, const sr::Spans &owned) {
            std::vector<uint8_t> converted(surface.bytes(), surface.value);
            auto *guest = memory.data() + surface.addr;
            if (!partial) std::fill_n(guest, surface.bytes(), surface.value); // direct readback's whole-surface path
            sr::copy_owned(guest, partial ? converted.data() : guest, surface.addr, owned, [&](uint32_t addr, uint32_t size) {
                const auto span = sr::bounds(addr, size);
                // Invalidation observes already-published bytes.
                for (uint64_t i = span.lo; i < span.hi; ++i) assert(memory[i] == 7);
                invalidated.push_back(span);
                for (unsigned i = 0; i < 5; ++i) if (sr::overlaps(entries[i], span)) cached[i] = false;
            });
        });
        assert(!cached[0] && !cached[1] && cached[4]);
        assert(cached[2] == partial && cached[3] == partial);
        if (partial) {
            assert(invalidated.size() == 3);
            assert(invalidated[0].lo == 16 && invalidated[0].hi == 32);
            assert(invalidated[1].lo == 40 && invalidated[1].hi == 48);
            assert(invalidated[2].lo == 56 && invalidated[2].hi == 96);
            assert(memory[32] == 9 && memory[48] == 5);
        } else assert(invalidated.size() == 1 && invalidated[0].lo == 16 && invalidated[0].hi == 96);
    }
    {
        Surfaces surfaces;
        auto &s = add(surfaces, 16, 32, 1, 1);
        Memory memory{};
        cpu_write(surfaces, memory, 19, 2, 9);  // not pixel aligned
        cpu_write(surfaces, memory, 27, 3, 8);
        assert(read(surfaces, memory, 19, 2) == 0 && s.gpu_newer);
        assert(read(surfaces, memory, 16, 1) == 1 && !s.gpu_newer);
        for (unsigned i = 16; i < 48; ++i)
            assert(memory[i] == (i >= 19 && i < 21 ? 9 : i >= 27 && i < 30 ? 8 : 1));
        // The tiny request materializes every other owned byte, including the gap between CPU writes.
        s.drawn = 2; s.gpu_newer = true; s.value = 2;
        cpu_write(surfaces, memory, 40, 1, 7);
        assert(read(surfaces, memory, 16, 32) == 1);
        for (unsigned i = 16; i < 48; ++i) assert(memory[i] == (i == 40 ? 7 : 2));
    }
    {
        Surfaces surfaces;
        add(surfaces, 16, 16, 1, 1);
        add(surfaces, 48, 16, 2, 2);
        Memory memory{};
        assert(read(surfaces, memory, 16, 48) == 2);
        for (unsigned i = 0; i < memory.size(); ++i)
            assert(memory[i] == (i >= 16 && i < 32 ? 1 : i >= 48 && i < 64 ? 2 : 0));
    }
    for (bool newer_already_read : {false, true}) {
        Surfaces surfaces;
        auto &old = add(surfaces, 16, 48, 1, 1);
        add(surfaces, 32, 16, 2, 2);
        Memory memory{};
        if (newer_already_read) assert(read(surfaces, memory, 32, 16) == 1 && old.gpu_newer);
        assert(read(surfaces, memory, 16, 48) == (newer_already_read ? 1 : 2));
        for (unsigned i = 16; i < 64; ++i) assert(memory[i] == (i >= 32 && i < 48 ? 2 : 1));
    }
    for (bool newer_already_read : {false, true}) {
        Surfaces surfaces;
        auto &old = add(surfaces, 16, 48, 1, 1);
        add(surfaces, 32, 16, 2, 2);
        Memory memory{};
        cpu_write(surfaces, memory, 35, 1, 9);
        if (newer_already_read) assert(read(surfaces, memory, 32, 16) == 1);
        else {
            assert(!sr::retire(*surfaces[1], surfaces));
            assert(read(surfaces, memory, 16, 48) == 2);
        }
        retire(surfaces, 1);
        assert(read(surfaces, memory, 16, 48) == (newer_already_read ? 1 : 0));
        for (unsigned i = 16; i < 64; ++i) assert(memory[i] == (i == 35 ? 9 : i >= 32 && i < 48 ? 2 : 1));
        old.drawn = 3; old.gpu_newer = true; old.value = 3;
        assert(read(surfaces, memory, 16, 48) == 1);  // a later draw reclaims retired ownership
        for (unsigned i = 16; i < 64; ++i) assert(memory[i] == 3);
    }
    {
        Surfaces surfaces;
        auto &surface = add(surfaces, 16, 32, 1, 1);
        Memory memory{};
        cpu_write(surfaces, memory, 19, 2, 9);
        cpu_write(surfaces, memory, 27, 3, 8);
        assert(!sr::retire(surface, surfaces));  // partial CPU writes leave GPU-owned gaps
        assert(read(surfaces, memory, 16, 32) == 1);
        retire(surfaces, 0);
        for (unsigned i = 16; i < 48; ++i)
            assert(memory[i] == (i >= 19 && i < 21 ? 9 : i >= 27 && i < 30 ? 8 : 1));
    }
    {
        Surfaces surfaces;
        auto &old = add(surfaces, 16, 48, 1, 1);
        auto &middle = add(surfaces, 32, 16, 2, 2);
        Memory memory{};
        assert(!sr::retire(middle, surfaces)); // live GPU bytes survive, even with no readback buffer
        assert(old.written.empty() && !old.guest_newer && middle.gpu_newer);
        add(surfaces, 32, 16, 3, 3);
        retire(surfaces, 1); // fully superseded by a newer alias
        assert(read(surfaces, memory, 32, 16) == 1);
        retire(surfaces, 1);
        read(surfaces, memory, 16, 48);
        for (unsigned i = 16; i < 64; ++i) assert(memory[i] == (i >= 32 && i < 48 ? 3 : 1));
    }
    {
        Surfaces surfaces;
        auto &surface = add(surfaces, 16, 32, 1, 1);
        Memory memory{};
        // Ordinary CPU stores can reuse a target address without notifying the renderer.
        std::fill_n(memory.begin() + 16, 32, 9);
        assert(!sr::retire(surface, surfaces));
        for (unsigned i = 16; i < 48; ++i) assert(memory[i] == 9);
        cpu_write(surfaces, memory, 16, 32, 9);
        retire(surfaces, 0);
        for (unsigned i = 16; i < 48; ++i) assert(memory[i] == 9);
    }
    {
        Surfaces surfaces;
        auto &s = add(surfaces, 0xfffffff0, 16, 1, 1);
        sr::guest_wrote(s, 0xfffffff8, 16);
        assert(s.written.size() == 1 && s.written[0].hi == (uint64_t(1) << 32));
        int count = 0;
        auto callback = [&](auto &, const auto &) { ++count; };
        sr::read(surfaces, 0xfffffff0, 0, callback);
        sr::read(surfaces, 0xfffffff8, 16, callback);  // all requested bytes belong to the CPU
        sr::read(surfaces, 0, 16, callback);           // overflow must not wrap into low memory
        assert(count == 0);
        sr::read(surfaces, 0xfffffff0, 16, callback);
        assert(count == 1);
        s.drawn++;  // a new GPU write reclaims the old CPU span even if guest_newer is still set
        sr::guest_wrote(s, 0xfffffff0, 1);
        assert(s.written.size() == 1 && s.written[0].lo == 0xfffffff0 && s.written[0].hi == 0xfffffff1);
    }
    // Compare the production planner with a byte-by-byte ownership oracle, including clean aliases and separated CPU writes.
    std::minstd_rand random(1234);
    for (int trial = 0; trial < 200; ++trial) {
        Surfaces surfaces;
        Memory memory{}, expected{};
        for (unsigned i = 0; i < 6; ++i) {
            auto &s = add(surfaces, random() % 64, 1 + random() % 32, i + 1, i + 1);
            s.gpu_newer = random() % 2;
            s.depth = i == 5;
        }
        for (unsigned byte = 0; byte < memory.size(); ++byte) {
            Surface *owner = nullptr;
            for (const auto &s : surfaces)
                if (!s->depth && s->addr <= byte && byte < s->addr + s->size && (!owner || s->drawn > owner->drawn)) owner = s.get();
            if (owner) {
                expected[byte] = owner->value;
                if (!owner->gpu_newer) memory[byte] = owner->value;
            }
        }
        for (unsigned write = 0; write < 4; ++write) {
            const unsigned addr = random() % 96, size = 1 + random() % 8;
            cpu_write(surfaces, memory, addr, size, 0xaa);
            std::fill_n(expected.begin() + addr, size, 0xaa);
        }
        read(surfaces, memory, 0, memory.size());
        assert(memory == expected);
    }
    puts("surface readback: CPU spans, disjoint targets, safe retirement, bounds and 200 ownership cases passed");
}
