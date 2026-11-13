// Checks port/wiiu/vertex_arrays.h on a PC: every attribute of a draw lands in an array whose copy holds all the bytes its fetch reads, however
// the attributes are interleaved and in whatever order they come, and every fetch is one a real GPU can make: from a multiple of 4 bytes into
// a vertex, in steps of a multiple of 4 bytes. `python tools/win/build3ds.py selftest` builds and runs it.
#include <cstdio>
#include <cstdlib>
#include <vector>

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
        const u32 size = attrs[i].fmt == 3 ? 4 : attrs[i].fmt == 2 ? 2 : 1, at = fetch_offset(attrs[i]);
        // what the fetch reads of the last vertex is inside the copy
        ok = a.stride == attrs[i].stride && at >= a.lo && a.lo % 4 == 0 && attrs[i].offset + fetched_bytes(attrs[i]) <= a.hi &&
             (at - a.lo) + (attrs[i].fmt < 2 ? 4 : fetched_bytes(attrs[i])) <= vertex_array_bytes(a, 1);
        if (!fetchable(attrs[i])) continue;
        // a real GPU's fetch: it starts a multiple of 4 bytes into the vertex, the copy's vertices are a multiple of 4 bytes apart, and each
        // vertex's own bytes of the attribute are in the copy where the fetch looks for them
        const u32 slot = gpu_stride(a.stride);
        ok = ok && (at - a.lo) % 4 == 0 && slot % 4 == 0 && slot >= a.stride && fetch_lane(attrs[i]) + attrs[i].elems <= 4;
        // a draw of all the array's vertices, of its one vertex, and of its last two (as gx2_draw copies them: from the first one drawn on)
        const u32 draws[][2] = {{0, 5}, {0, 1}, {3, 5}};
        for (const auto &[first, vertices] : draws) {
            std::vector<u8> src(vertex_array_bytes(a, vertices)), copy(gpu_array_bytes(a, vertices - first) + 64, 0xEE);
            for (size_t k = 0; k < src.size(); k++) src[k] = (u8)(k * 7 + 3);
            copy_vertex_array(copy.data(), src.data() + first * a.stride, a, vertices - first);
            for (u32 v = first; v < vertices && ok; v++)
                for (u32 k = 0; k < attrs[i].elems * size && ok; k++)
                    ok = copy[(v - first) * slot + (attrs[i].offset - a.lo) + k] == src[v * a.stride + (attrs[i].offset - a.lo) + k];
            ok = ok && copy[gpu_array_bytes(a, vertices - first)] == 0xEE;  // and nothing written past what was asked for
        }
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
    // the two layouts a console drew as shards: 16-bit position, normal and texture coordinates, a colour, and one byte for the bone
    const Gx2Attr boned[] = {{0, 22, 2, 3, false}, {6, 22, 2, 3, false}, {12, 22, 2, 2, false}, {16, 22, 1, 4, false}, {20, 22, 1, 1, false}};
    check("vertices 22 bytes apart", boned, 5, 1);
    const Gx2Attr boned_plain[] = {{0, 14, 2, 3, false}, {6, 14, 2, 3, false}, {12, 14, 1, 1, false}};
    check("vertices 14 bytes apart", boned_plain, 3, 1);
    // each attribute in an array of its own, as a library that hands them over one by one leaves them: three 16-bit numbers every 6 bytes
    const Gx2Attr apart[] = {{0x100, 6, 2, 3, false}, {0x800, 6, 2, 3, false}, {0x1000, 4, 2, 2, false}};
    check("position, normal and texture coordinates each in an array of its own, 6, 6 and 4 bytes apart", apart, 3, 3);
    const Gx2Attr thirteen[] = {{0, 13, 2, 3, false}, {6, 13, 2, 3, false}, {12, 13, 0, 1, false}};
    check("vertices 13 bytes apart", thirteen, 3, 1);
    const Gx2Attr odd[] = {{3, 7, 1, 1, false}, {4, 7, 1, 3, false}};
    check("vertices 7 bytes apart, the first attribute 3 bytes past the array's start", odd, 2, 1);
    const Gx2Attr packed[] = {{2, 6, 2, 2, false}};
    check("two 16-bit numbers in the second half of a word, 6 bytes apart", packed, 1, 1);
    const Gx2Attr one[] = {{0, 1, 1, 1, false}}, two[] = {{0, 2, 2, 1, false}}, three[] = {{0, 3, 1, 3, false}};
    check("vertices 1 byte apart", one, 1, 1);
    check("vertices 2 bytes apart", two, 1, 1);
    check("vertices 3 bytes apart", three, 1, 1);

    const Gx2Attr odd_stride{0, 7, 1, 3, false}, across{6, 16, 1, 4, false}, word{4, 16, 0, 4, false}, packed_shorts{2, 6, 2, 3, false};
    const Gx2Attr shorts{8, 12, 2, 2, false}, off_word_shorts{6, 12, 2, 2, false}, floats{12, 24, 3, 3, false};
    const Gx2Attr four_off_word{6, 16, 2, 4, false}, odd_shorts{3, 8, 2, 2, false}, off_word_float{6, 16, 3, 2, false};
    // bytes only as the word they lie in, whatever the stride; 16-bit integers from an even offset, three at most from a word's second half;
    // floats from a word's start
    if (!fetchable(odd_stride) || fetchable(across) || !fetchable(word) || !fetchable(packed_shorts) || !fetchable(shorts) || !fetchable(off_word_shorts) ||
        !fetchable(floats) || fetchable(four_off_word) || fetchable(odd_shorts) || fetchable(off_word_float) || fetched_bytes(shorts) != 8 ||
        fetched_bytes(packed_shorts) != 8 || fetch_lane(off_word_shorts) != 1 || fetch_lane(shorts) != 0 || fetch_lane(across) != 2 ||
        fetch_offset(off_word_shorts) != 4 || fetch_offset(shorts) != 8 || fetch_offset(across) != 4 || fetch_offset(floats) != 12 ||
        gpu_stride(20) != 20 || gpu_stride(22) != 28 || gpu_stride(14) != 20 || gpu_stride(13) != 16 || gpu_stride(7) != 12 || gpu_stride(6) != 12 ||
        gpu_stride(1) != 4 || gpu_stride(2) != 8 || gpu_stride(3) != 8) {
        failures++;
        printf("FAILED: which attributes the GPU fetches\n");
    }
    printf(failures ? "vertex arrays: %d failed\n" : "vertex arrays: ok\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
