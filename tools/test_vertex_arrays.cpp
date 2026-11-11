// Checks port/wiiu/vertex_arrays.h on a PC: every attribute of a draw lands in an array whose copy holds all the bytes its fetch reads, however
// the attributes are interleaved and in whatever order they come. `python tools/win/build3ds.py selftest` builds and runs it.
#include <cstdio>
#include <cstdlib>

#include "vertex_arrays.h"

using namespace gpu;

static int failures;

// one draw: the attributes in the order given
static void check(const char *what, const Gx2Attr *attrs, u32 n, u32 want_arrays) {
    VertexArray arrays[16];
    u32 count = 0, in[16];
    for (u32 i = 0; i < n; i++) in[i] = join_vertex_array(arrays, count, attrs[i]);
    bool ok = count == want_arrays;
    for (u32 i = 0; i < n && ok; i++) {
        const VertexArray &a = arrays[in[i]];
        const u32 lane = attrs[i].fmt < 2 ? attrs[i].offset & 3 : 0, at = attrs[i].offset - lane;
        // its stream's offset in the copy is 4-byte aligned for bytes, and what the fetch reads of the last vertex is inside the copy
        ok = a.stride == attrs[i].stride && at >= a.lo && a.lo % 4 == 0 && attrs[i].offset + fetched_bytes(attrs[i]) <= a.hi &&
             (at - a.lo) + (attrs[i].fmt < 2 ? 4 : fetched_bytes(attrs[i])) <= vertex_array_bytes(a, 1);
    }
    if (!ok) {
        failures++;
        printf("FAILED: %s (%u arrays, wanted %u)\n", what, count, want_arrays);
    }
}

int main() {
    // offset, stride, format (0 s8, 1 u8, 2 s16, 3 float), elements, default
    const Gx2Attr interleaved[] = {{0, 24, 3, 3, false}, {12, 24, 2, 3, false}, {20, 24, 2, 2, false}};
    check("position, normal and texture coordinates in one 24-byte vertex", interleaved, 3, 1);
    // the order that used to drop the draw: the array's start moves down under the attribute that joined first
    const Gx2Attr moving[] = {{20, 32, 3, 3, false}, {40, 32, 3, 2, false}, {0, 32, 3, 3, false}};
    check("offsets 20, 40, 0 with a stride of 32", moving, 3, 1);
    const Gx2Attr reversed[] = {{28, 32, 1, 4, false}, {12, 32, 3, 3, false}, {0, 32, 3, 3, false}};
    check("one array, last attribute first", reversed, 3, 1);
    const Gx2Attr based[] = {{0x1000, 16, 3, 3, false}, {0x100c, 16, 1, 4, false}, {0x9000, 8, 2, 2, false}, {0x9004, 8, 2, 2, false}};
    check("two arrays far from the base", based, 4, 2);
    const Gx2Attr sparse[] = {{0, 64, 3, 3, false}, {60, 64, 1, 2, false}};
    check("two attributes at the ends of a 64-byte vertex", sparse, 2, 1);
    const Gx2Attr bytes[] = {{0, 16, 3, 3, false}, {13, 16, 1, 3, false}};
    check("bytes at an odd offset, fetched as the word they're in", bytes, 2, 1);

    const Gx2Attr odd_stride{0, 7, 1, 3, false}, across{6, 16, 1, 4, false}, word{4, 16, 0, 4, false}, shorts{2, 6, 2, 3, false};
    if (fetchable(odd_stride) || fetchable(across) || !fetchable(word) || !fetchable(shorts)) {
        failures++;
        printf("FAILED: which attributes the GPU fetches\n");
    }
    printf(failures ? "vertex arrays: %d failed\n" : "vertex arrays: ok\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
