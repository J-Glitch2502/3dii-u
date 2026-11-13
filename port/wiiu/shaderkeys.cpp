// The keys that identify compiled shaders. Used by the shader generator on the PC and by the renderer on the console, so that both
// agree byte for byte; integers go into the hash in little-endian order on either.
#include "gpu/pica.h"
#include "shadergen.h"

namespace gpu {
#include "fs_key.inc"

u64 fnv64(const void *p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

u32 vs_open_bools(const ShaderProgram *p, u32 entry) {
    u64 ph;
    u32 bm, lm;
    shader_entry_info(p, entry, &ph, &bm, &lm);
    return (u32)__builtin_popcount(bm) > kVsBoolsMade ? bm : 0;
}

// Integer registers aren't in it: loops read them from a uniform, so the same shader serves whatever a game sets them to. Nor are the
// bools a shader leaves open; which ones those are goes in after the rest, so that every other shader's key stays what it was.
u64 vs_variant_key(const ShaderProgram *p, u32 entry, const bool *b, const s8 *slot_src, u32 open) {
    u64 ph;
    u32 bm, lm;
    shader_entry_info(p, entry, &ph, &bm, &lm);
    u32 bits = 0;
    for (u32 i = 0; i < 16; i++)
        if ((((bm & ~open) >> i) & 1) && b[i]) bits |= 1u << i;
    u8 d[12 + 24 + 8];
    u32 at = 0;
    for (u32 i = 0; i < 8; i++) d[at++] = (u8)(ph >> (8 * i));
    for (u32 i = 0; i < 2; i++) d[at++] = (u8)(entry >> (8 * i));
    for (u32 i = 0; i < 2; i++) d[at++] = (u8)(bits >> (8 * i));
    for (u32 i = 0; i < 24; i++) d[at++] = (u8)slot_src[i];
    if (open)
        for (u32 i = 0; i < 2; i++) d[at++] = (u8)(open >> (8 * i));
    return fnv64(d, at);
}
}  // namespace gpu
