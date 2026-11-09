#include "cpu.h"

#include <cstdint>

int32_t GuestAliasOffsets[GUEST_ALIAS_PAGE_COUNT];
static uint8_t GuestAliasOwners[GUEST_ALIAS_PAGE_COUNT];

static bool ValidAlias(uint32_t target, uint32_t backing, uint32_t size) {
    if (!size || ((target | backing | size) & (GUEST_ALIAS_PAGE_SIZE - 1))) return false;
    return target >= GUEST_ALIAS_BASE && uint64_t(target) + size <= GUEST_ALIAS_LIMIT &&
           (backing < GUEST_ALIAS_BASE || backing >= GUEST_ALIAS_LIMIT) && uint64_t(backing) + size <= 0x20000000ull && target != backing;
}

extern "C" bool GuestAliasMap(uint32_t target, uint32_t backing, uint32_t size, GuestAliasKind kind) {
    if (!ValidAlias(target, backing, size) || (kind != GuestAliasControl && kind != GuestAliasShared)) return false;
    const uint32_t first = (target - GUEST_ALIAS_BASE) / GUEST_ALIAS_PAGE_SIZE;
    const uint32_t pages = size / GUEST_ALIAS_PAGE_SIZE;
    const int32_t delta = int32_t(backing) - int32_t(target);
    for (uint32_t index = 0; index < pages; ++index)
        if (__atomic_load_n(&GuestAliasOffsets[first + index], __ATOMIC_ACQUIRE) || GuestAliasOwners[first + index]) return false;
    for (uint32_t index = 0; index < pages; ++index) {
        GuestAliasOwners[first + index] = kind;
        __atomic_store_n(&GuestAliasOffsets[first + index], delta, __ATOMIC_RELEASE);
    }
    return true;
}

extern "C" bool GuestAliasUnmap(uint32_t target, uint32_t backing, uint32_t size, GuestAliasKind kind) {
    if (!ValidAlias(target, backing, size) || (kind != GuestAliasControl && kind != GuestAliasShared)) return false;
    const uint32_t first = (target - GUEST_ALIAS_BASE) / GUEST_ALIAS_PAGE_SIZE;
    const uint32_t pages = size / GUEST_ALIAS_PAGE_SIZE;
    const int32_t delta = int32_t(backing) - int32_t(target);
    for (uint32_t index = 0; index < pages; ++index)
        if (__atomic_load_n(&GuestAliasOffsets[first + index], __ATOMIC_ACQUIRE) != delta || GuestAliasOwners[first + index] != kind)
            return false;
    for (uint32_t index = 0; index < pages; ++index) {
        __atomic_store_n(&GuestAliasOffsets[first + index], 0, __ATOMIC_RELEASE);
        GuestAliasOwners[first + index] = 0;
    }
    return true;
}

extern "C" bool GuestAliasSourceInUse(uint32_t backing, uint32_t size) {
    if (!size) return false;
    const uint64_t end = uint64_t(backing) + size;
    for (uint32_t index = 0; index < GUEST_ALIAS_PAGE_COUNT; ++index) {
        const int32_t delta = __atomic_load_n(&GuestAliasOffsets[index], __ATOMIC_ACQUIRE);
        if (!delta) continue;
        const uint32_t target = GUEST_ALIAS_BASE + index * GUEST_ALIAS_PAGE_SIZE;
        const uint64_t source = uint32_t(target + delta);
        if (source < end && source + GUEST_ALIAS_PAGE_SIZE > backing) return true;
    }
    return false;
}

extern "C" uint32_t GuestAliasSource(uint32_t target, GuestAliasKind kind) {
    if (target < GUEST_ALIAS_BASE || target >= GUEST_ALIAS_LIMIT || (target & (GUEST_ALIAS_PAGE_SIZE - 1))) return 0;
    const uint32_t index = (target - GUEST_ALIAS_BASE) / GUEST_ALIAS_PAGE_SIZE;
    if (GuestAliasOwners[index] != kind) return 0;
    const int32_t delta = __atomic_load_n(&GuestAliasOffsets[index], __ATOMIC_ACQUIRE);
    return delta ? uint32_t(target + delta) : 0;
}

extern "C" u16 GuestReadCross16(u32 address) {
    u8 bytes[2];
    for (u32 i = 0; i < 2; ++i) bytes[i] = *GuestMemoryPointer(address + i);
    u16 value;
    memcpy(&value, bytes, 2);
    return value;
}

extern "C" u32 GuestReadCross32(u32 address) {
    u8 bytes[4];
    for (u32 i = 0; i < 4; ++i) bytes[i] = *GuestMemoryPointer(address + i);
    u32 value;
    memcpy(&value, bytes, 4);
    return value;
}

extern "C" u64 GuestReadCross64(u32 address) {
    u8 bytes[8];
    for (u32 i = 0; i < 8; ++i) bytes[i] = *GuestMemoryPointer(address + i);
    u64 value;
    memcpy(&value, bytes, 8);
    return value;
}

extern "C" u16 GuestWriteCross16(u32 address, u16 value) {
    const u8 *bytes = (const u8 *)&value;
    for (u32 i = 0; i < 2; ++i) *GuestMemoryPointer(address + i) = bytes[i];
    return value;
}

extern "C" u32 GuestWriteCross32(u32 address, u32 value) {
    const u8 *bytes = (const u8 *)&value;
    for (u32 i = 0; i < 4; ++i) *GuestMemoryPointer(address + i) = bytes[i];
    return value;
}

extern "C" void GuestWriteCross64(u32 address, u64 value) {
    const u8 *bytes = (const u8 *)&value;
    for (u32 i = 0; i < 8; ++i) *GuestMemoryPointer(address + i) = bytes[i];
}

extern "C" bool GuestMemoryRangeCheck(u32 address, size_t size) {
    const u64 end = (u64)address + size;
    const u8 *start = GuestMemoryPointer(address);
    for (u64 page = ((u64)address | (GUEST_ALIAS_PAGE_SIZE - 1)) + 1; page < end; page += GUEST_ALIAS_PAGE_SIZE)
        if (GuestMemoryPointer((u32)page) != start + (size_t)(page - address)) return false;
    return true;
}

extern "C" void GuestCopyFrom(void *destination, u32 source, size_t size) {
    if (!size) return;
    if (GuestMemoryRangeContiguous(source, size)) {
        memcpy(destination, GuestMemoryPointer(source), size);
        return;
    }
    u8 *out = (u8 *)destination;
    while (size) {
        const size_t run = size < (GUEST_ALIAS_PAGE_SIZE - (source & (GUEST_ALIAS_PAGE_SIZE - 1)))
                               ? size
                               : GUEST_ALIAS_PAGE_SIZE - (source & (GUEST_ALIAS_PAGE_SIZE - 1));
        memcpy(out, GuestMemoryPointer(source), run);
        out += run;
        source += (u32)run;
        size -= run;
    }
}

extern "C" void GuestCopyTo(u32 destination, const void *source, size_t size) {
    if (!size) return;
    if (GuestMemoryRangeContiguous(destination, size)) {
        memcpy(GuestMemoryPointer(destination), source, size);
        return;
    }
    const u8 *in = (const u8 *)source;
    while (size) {
        const size_t run = size < (GUEST_ALIAS_PAGE_SIZE - (destination & (GUEST_ALIAS_PAGE_SIZE - 1)))
                               ? size
                               : GUEST_ALIAS_PAGE_SIZE - (destination & (GUEST_ALIAS_PAGE_SIZE - 1));
        memcpy(GuestMemoryPointer(destination), in, run);
        in += run;
        destination += (u32)run;
        size -= run;
    }
}

extern "C" void GuestFill(u32 destination, u8 value, size_t size) {
    if (!size) return;
    if (GuestMemoryRangeContiguous(destination, size)) {
        memset(GuestMemoryPointer(destination), value, size);
        return;
    }
    while (size) {
        const size_t run = size < (GUEST_ALIAS_PAGE_SIZE - (destination & (GUEST_ALIAS_PAGE_SIZE - 1)))
                               ? size
                               : GUEST_ALIAS_PAGE_SIZE - (destination & (GUEST_ALIAS_PAGE_SIZE - 1));
        memset(GuestMemoryPointer(destination), value, run);
        destination += (u32)run;
        size -= run;
    }
}
