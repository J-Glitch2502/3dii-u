// The vertex arrays a draw's attributes lie in, worked out from the attributes alone: each array is copied once as it lies in the game's memory
// and the GPU fetches every attribute from the copy (gx2_backend.cpp's gx2_draw). Nothing of the GPU's here, so a PC can run it:
// tools/test_vertex_arrays.cpp.
//
// What a real GPU of this family holds a fetch to, and Cemu doesn't: it steps through an array a multiple of 4 bytes at a time, and fetches
// from a multiple of 4 bytes into each vertex (Mesa's r600 driver says so of the hardware: its vertex buffers' offsets and strides and its
// vertex elements' offsets are "4BYTE_ALIGNED_ONLY"). Given anything else, a console drew two models whose vertices lie 22 and 14 bytes
// apart as shards, with everything around them right (Super Mario 3D Land's map of World 3, 2026-11-13).
#pragma once
#include <algorithm>
#include <cstring>

#include "gpu/pica.h"

namespace gpu {
struct VertexArray {
    u32 lo, hi, stride;  // within a vertex, from the draw's base: its first attribute's start, down to a 4-byte boundary, and its last one's end
};

// The component of what's fetched that an attribute starts at. Bytes are fetched as the 4-byte word they lie in and 16-bit integers as the
// four from a word's start on, the stream's mask routing the ones wanted: so every fetch starts a multiple of 4 bytes into the vertex.
inline u32 fetch_lane(const Gx2Attr &at) { return at.fmt < 2 ? at.offset & 3 : at.fmt == 2 ? (at.offset & 2) / 2 : 0; }
// where in the vertex that fetch starts
inline u32 fetch_offset(const Gx2Attr &at) { return at.offset - fetch_lane(at) * (at.fmt == 2 ? 2 : 1); }

// Whether the GPU can fetch an attribute from its array's copy: all of it has to lie in what one fetch from a word's start reads. A float
// only at a multiple of 4 bytes; 16-bit integers at an even offset, three at most when they start in a word's second half; bytes within
// one word. The CPU makes floats of the rest, the old way.
inline bool fetchable(const Gx2Attr &at) {
    if (at.fmt == 3) return at.offset % 4 == 0;
    if (at.fmt == 2) return at.offset % 2 == 0 && fetch_lane(at) + at.elems <= 4;
    return (at.offset & 3) + at.elems <= 4;
}

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

// How far apart the vertices lie in the copy the GPU steps through: as in the game's memory where that is a multiple of 4 bytes, else in
// slots that are, each with room for the 3 bytes an attribute may lie past where the array's start was rounded down to.
// shortcut: a slot holds one vertex from its start on, so attributes joined into one such array from more than a slot apart would be
// fetched from the wrong bytes; make the slots as wide as the array's span when a game has one.
inline u32 gpu_stride(u32 stride) { return stride % 4 == 0 ? stride : (stride + 6) & ~3u; }
inline u32 gpu_array_bytes(const VertexArray &a, u32 vertices) { return (vertices - 1) * gpu_stride(a.stride) + (a.hi - a.lo); }

// The copy: the lot at once, or vertex by vertex into those slots, each filled from its vertex on. What a fetch reads past a vertex's own
// attributes is another vertex's bytes either way, and the mask drops it.
inline void copy_vertex_array(u8 *dst, const u8 *src, const VertexArray &a, u32 vertices) {
    const u32 bytes = vertex_array_bytes(a, vertices), slot = gpu_stride(a.stride);
    if (slot == a.stride) {
        memcpy(dst, src, bytes);
        return;
    }
    for (u32 v = 0; v < vertices; v++) memcpy(dst + v * slot, src + v * a.stride, std::min(slot, bytes - v * a.stride));
}
}  // namespace gpu
