// third_party/3dsrecomp/runtime/services/ro_tree.h against an export tree made up here for three names: each name reaches its own
// export, a name that isn't exported still ends at some leaf (the loader then compares the names), a name is read no further than its
// length, no entry past the tree is read, and a child out of range, a loop and a module's own count that is far too large all give -1
// in a bounded walk. Built and run by tools/selftest.py.
#include <cstdio>
#include <cstring>

#include "ro_tree.h"

static int failed;
static void expect(bool ok, const char *what) {
    if (ok) return;
    printf("FAILED: %s\n", what);
    failed++;
}

// test bit, left, right, export index. The names: 0 "Abc", 1 "Abd", 2 "B". "A" and "B" differ at bit 0 of byte 0 (0x41, 0x42),
// "Abc" and "Abd" at bit 0 of byte 2 (0x63, 0x64). As in a game's trees, the tests carry export indices too, which only a leaf's count.
static uint16_t tree[6][4] = {
    {0, 0x0001, 0, 1},        // the start: its left child
    {0x00, 0x8003, 0x0002, 0},  // byte 0, bit 0: clear to "B", set to the next test
    {0x10, 0x8005, 0x8004, 2},  // byte 2, bit 0: clear to "Abd", set to "Abc"
    {0, 0, 0, 2},
    {0, 0, 0, 0},
    {0, 0, 0, 1},
};
static int lookups, outside;
static void entry(uint32_t i, uint16_t *e) {
    lookups++;
    if (i >= 6) {
        outside++;
        memset(e, 0, 8);
        return;
    }
    memcpy(e, tree[i], sizeof tree[i]);
}
static int32_t leaf(const char *name, uint32_t nodes = 6, size_t len = ~size_t(0)) {
    lookups = 0;
    return export_tree_leaf(entry, nodes, name, len == ~size_t(0) ? strlen(name) : len);
}

int main() {
    expect(leaf("Abc") == 0, "Abc is export 0");
    expect(leaf("Abd") == 1, "Abd is export 1");
    expect(leaf("B") == 2, "B is export 2");
    expect(leaf("Ab") == 1, "a name that isn't there ends at a leaf all the same: past its end a name reads as zeros");
    expect(leaf("Abc", 6, 2) == 1, "a name is read no further than its length");
    expect(leaf("Abc", 0) == -1, "a module with no tree");

    tree[2][2] = 0x0009;
    expect(leaf("Abc") == -1, "a child past the tree's end");
    tree[2][2] = 0x8004;

    tree[1][1] = 0x0001;  // "B"'s way loops back to the same test
    expect(leaf("B") == -1, "a loop ends");
    expect(leaf("B", 0xFFFFFFFF) == -1 && lookups <= 0x8001, "and in a bounded number of steps, whatever count the module gives");
    tree[1][1] = 0x8003;
    expect(leaf("B") == 2, "and B is found again once the tree is sound");
    expect(!outside, "no entry past the tree is read");

    if (!failed) printf("ok\n");
    return failed ? 1 : 0;
}
