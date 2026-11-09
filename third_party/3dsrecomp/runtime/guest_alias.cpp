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
