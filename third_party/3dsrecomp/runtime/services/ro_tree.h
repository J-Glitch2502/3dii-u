// A CRO module's export tree, which ldr:ro (ro.cpp) looks named exports up with, as the 3DS's RO module does. An entry is four u16s:
// the bit a name is tested at (its byte << 3 | the bit), the left and the right child (bit 15: a leaf), and a leaf's index in the named
// export table. The walk starts at entry 0's left child. A child names one of 0x8000 entries, so a walk of a sound tree takes no more steps.
#pragma once
#include <cstddef>
#include <cstdint>

// The named export a name leads to, or -1 if the tree is malformed. Any name ends at some leaf: the caller checks the export is this one.
// entry(i, e): entry i's four u16s into e, for i < nodes.
template <class Entry>
int32_t export_tree_leaf(Entry entry, uint32_t nodes, const char *name, size_t len) {
    if (!nodes) return -1;
    uint16_t e[4];
    entry(0, e);
    uint32_t child = e[1];
    for (uint32_t steps = 0; steps < 0x8000; steps++) {
        const uint32_t i = child & 0x7FFF;
        if (i >= nodes) return -1;
        entry(i, e);
        if (child & 0x8000) return e[3];
        const uint32_t byte = e[0] >> 3;
        const bool bit = byte < len && ((uint8_t)name[byte] >> (e[0] & 7) & 1);
        child = bit ? e[2] : e[1];
    }
    return -1;  // it loops
}
