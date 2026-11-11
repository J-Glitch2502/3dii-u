// The vertex arrays a draw's attributes lie in, worked out from the attributes alone: each array is copied once as it lies in the game's memory
// and the GPU fetches every attribute from the copy (gx2_backend.cpp's gx2_draw). Nothing of the GPU's here, so a PC can run it:
// tools/test_vertex_arrays.cpp.
#pragma once
#include <algorithm>

#include "gpu/pica.h"

namespace gpu {
struct VertexArray {
    u32 lo, hi, stride;  // within a vertex, from the draw's base: its first attribute's start, down to a 4-byte boundary, and its last one's end
};

// Whether the GPU can fetch an attribute from its array's copy. It fetches whole 4-byte words from 4-byte boundaries, so the array's vertices
// have to be a multiple of 4 bytes apart, and then: floats and 16-bit integers have to start on such a boundary; bytes may start anywhere in
// a word they lie wholly within, the mask routing the ones wanted (a byte at an odd place fetched by itself comes out shifted). A game packs
// its vertices as tightly as it likes, and whatever doesn't fit this the CPU turns into floats, as it always did: fetched regardless, Mario
// & Luigi: Superstar Saga's backgrounds came out as vertical stripes, a texture coordinate's second half read as one value (2026-11-11).
inline bool fetchable(const Gx2Attr &at) {
    if (at.stride % 4) return false;
    return at.fmt >= 2 ? at.offset % 4 == 0 : (at.offset & 3) + at.elems <= 4;
}

// The bytes the fetch reads from an attribute's place on: bytes as the word they're in, 3 x 16 bits as 4, a lone float as 2 (attr_format)
inline u32 fetched_bytes(const Gx2Attr &at) {
    return at.fmt == 3 ? (at.elems == 1 ? 2 : at.elems) * 4 : at.fmt == 2 ? (at.elems == 3 ? 4 : at.elems) * 2 : (at.offset & 3) + 4;
}

// Puts an attribute into the array it lies in, a new one if none has it, and returns which. The same array: the same stride, and within one
// vertex of what's known of it. An array's start moves down as attributes join, so the caller keeps what this returns: looking an attribute's
// array up again by its offset can find nothing, and that used to drop the draw.
inline u32 join_vertex_array(VertexArray *arrays, u32 &count, const Gx2Attr &at) {
    u32 k = 0;
    while (k < count && !(arrays[k].stride == at.stride && at.offset + at.stride > arrays[k].lo && at.offset < arrays[k].lo + at.stride)) k++;
    if (k == count) arrays[count++] = {at.offset & ~3u, at.offset + fetched_bytes(at), at.stride};
    arrays[k].lo = std::min(arrays[k].lo, at.offset & ~3u);
    arrays[k].hi = std::max(arrays[k].hi, at.offset + fetched_bytes(at));
    return k;
}

// the bytes of an array that vertices first to last take: whole vertices up to the last, and of that one only what's fetched
inline u32 vertex_array_bytes(const VertexArray &a, u32 vertices) { return (vertices - 1) * a.stride + (a.hi - a.lo); }
}  // namespace gpu
