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

u64 gsx_variant_key(const ShaderProgram *vs, u32 vs_entry, const bool *vb, const ShaderProgram *gs, u32 gs_entry, const bool *gb, const u8 (*gi)[4],
                    u32 vs_outmask, u32 gs_in, u64 gs_perm, const s8 *slot_src, bool turns) {
    u8 d[4 + 2 * 12 + 2 + 4 + 8 + 1 + 24 + 4 * 3];  // a fixed-primitive one's with four emitting loops: the most put below
    u32 at = 0;
    auto put = [&](u64 v, u32 bytes) {
        for (u32 i = 0; i < bytes; i++) d[at++] = (u8)(v >> (8 * i));
    };
    auto program = [&](const ShaderProgram *p, u32 entry, const bool *b) {
        u64 ph;
        u32 bm, lm, bits = 0;
        shader_entry_info(p, entry, &ph, &bm, &lm);
        for (u32 i = 0; i < 16; i++)
            if (((bm >> i) & 1) && b[i]) bits |= 1u << i;
        put(ph, 8);
        put(entry, 2);
        put(bits, 2);
    };
    put(0x31585347, 4);  // "GSX1": no vertex shader's key starts like it
    program(vs, vs_entry, vb);
    program(gs, gs_entry, gb);
    put(vs_outmask, 2);
    // a fixed-primitive one's vertices a run, first uniform and outputs a vertex (Gx2Draw::gs_in), without the input permutation it doesn't
    // read; a point one's key as before
    put(gs_in, gs_in & kGsxPrim ? 4 : 1);
    put(gs_in & kGsxPrim ? 0 : gs_perm, 8);
    put(turns, 1);
    for (u32 i = 0; i < 24; i++) d[at++] = (u8)slot_src[i];
    // each loop that emits (shader_glsl_gs): its count, written into the shader, and its first aL and step, which pick the reads that skip a
    // fixed-primitive run's vertices. A loop the bools skip is in too, which only makes more variants. Other programs' keys are as they were.
    // shortcut: each count, first aL and step a game draws with is a variant a build must have logged, or the draw stays on the CPU; key the
    // first aL and step only in fixed-primitive mode if a game turns out to vary them.
    const u32 emit_loops = shader_emit_loops(gs, gs_entry);
    for (u32 i = 0; i < 4; i++)
        if ((emit_loops >> i) & 1) put(gi[i][0] | gi[i][1] << 8 | gi[i][2] << 16, 3);
    return fnv64(d, at);
}
}  // namespace gpu
