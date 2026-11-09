#pragma once

#include <algorithm>
#include <cstdint>
#include <map>

namespace kernel {
struct MemoryRegion {
    uint32_t Base;
    uint32_t Size;
    uint32_t Permission;
    uint32_t State;

    uint64_t End() const { return uint64_t(Base) + Size; }
};

class MemoryRegions {
public:
    static constexpr uint64_t Limit = 0x40000000ull;

    static bool ValidRange(uint32_t base, uint32_t size) { return size && uint64_t(base) + size <= Limit; }

    static bool InRange(uint32_t base, uint32_t size, uint32_t lower, uint32_t upper) {
        return ValidRange(base, size) && base >= lower && uint64_t(base) + size <= upper;
    }

    static bool PageAligned(uint32_t value) { return (value & 0xFFFu) == 0; }

    bool Insert(uint32_t base, uint32_t size, uint32_t permission, uint32_t state) {
        if (!ValidRange(base, size)) return false;
        auto next = Entries.lower_bound(base);
        if (next != Entries.end() && next->first < uint64_t(base) + size) return false;
        if (next != Entries.begin() && std::prev(next)->second.End() > base) return false;
        Entries.emplace(base, MemoryRegion{base, size, permission, state});
        return true;
    }

    const MemoryRegion *Find(uint32_t address) const {
        auto next = Entries.upper_bound(address);
        if (next == Entries.begin()) return nullptr;
        const MemoryRegion &region = std::prev(next)->second;
        return address < region.End() ? &region : nullptr;
    }

    bool Covers(uint32_t base, uint32_t size, uint32_t state = UINT32_MAX, uint32_t requiredPermission = 0) const {
        if (!ValidRange(base, size)) return false;
        const uint64_t end = uint64_t(base) + size;
        uint64_t address = base;
        while (address < end) {
            const MemoryRegion *region = Find(uint32_t(address));
            if (!region || (state != UINT32_MAX && region->State != state) ||
                (region->Permission & requiredPermission) != requiredPermission)
                return false;
            address = std::min(end, region->End());
        }
        return true;
    }

    bool Remove(uint32_t base, uint32_t size, uint32_t state = UINT32_MAX) {
        if (!Covers(base, size, state)) return false;
        const uint32_t end = base + size;
        Split(base);
        Split(end);
        Entries.erase(Entries.lower_bound(base), Entries.lower_bound(end));
        return true;
    }

    bool Protect(uint32_t base, uint32_t size, uint32_t permission, uint32_t state = UINT32_MAX) {
        if (!Covers(base, size, state)) return false;
        const uint32_t end = base + size;
        Split(base);
        Split(end);
        for (auto it = Entries.lower_bound(base); it != Entries.lower_bound(end); ++it) it->second.Permission = permission;
        return true;
    }

    MemoryRegion Query(uint32_t address) const {
        if (const MemoryRegion *region = Find(address)) return *region;
        auto next = Entries.upper_bound(address);
        uint32_t begin = 0;
        if (next != Entries.begin()) begin = uint32_t(std::prev(next)->second.End());
        const uint32_t end = next == Entries.end() ? uint32_t(Limit) : next->first;
        return {begin, end - begin, 0, 0};
    }

    const std::map<uint32_t, MemoryRegion> &All() const { return Entries; }

private:
    void Split(uint32_t address) {
        auto next = Entries.upper_bound(address);
        if (next == Entries.begin()) return;
        auto it = std::prev(next);
        const MemoryRegion region = it->second;
        if (address <= region.Base || address >= region.End()) return;
        it->second.Size = address - region.Base;
        Entries.emplace(address, MemoryRegion{address, uint32_t(region.End() - address), region.Permission, region.State});
    }

    std::map<uint32_t, MemoryRegion> Entries;
};
} // namespace kernel
