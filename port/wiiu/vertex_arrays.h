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

// Whether the GPU can fetch an attribute from its array's copy. Floats and 16-bit integers always, where they lie: the CPU never converts
// those. Bytes are fetched as the 4-byte word they lie in, the mask routing the ones wanted: so only from an array whose vertices are a
// multiple of 4 bytes apart, and only if the attribute's bytes lie within one such word.
inline bool fetchable(const Gx2Attr &at) { return at.fmt >= 2 || (at.stride % 4 == 0 && (at.offset & 3) + at.elems <= 4); }

// The bytes the fetch reads from an attribute's place on: bytes as the word they're in, 16-bit integers as four of them, a lone float as two
// (attr_format)
inline u32 fetched_bytes(const Gx2Attr &at) { return at.fmt == 3 ? (at.elems == 1 ? 2 : at.elems) * 4 : at.fmt == 2 ? 8 : (at.offset & 3) + 4; }

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
