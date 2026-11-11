// GLSL generation (build time) and the keys that find the compiled shaders (build time and run time).
#pragma once
#include <string>

#include "gpu/pica.h"

namespace gpu {
std::string gen_fs_glsl(const DrawState &ds);
std::string gen_uber_fs(bool lit);
constexpr char kUberKey[] = "uber", kUberLitKey[] = "uber lit";  // their entries in the fragment index, hashed like any key
std::string gen_vs_glsl(const ShaderProgram *p, u32 entry, u32 b, const u8 (*iu)[4], const s8 *slot_src, u32 *inputs);
std::string gen_passthrough_vs();
std::string gen_blit_vs();
std::string gen_blit_fs();
std::string gen_etc1_fs(bool alpha);                                     // decodes an ETC1 (ETC1A4 with alpha) texture on the GPU: see gx2_backend's gpu_decode
constexpr char kEtc1Key[] = "etc1", kEtc1A4Key[] = "etc1a4";              // their entries in the fragment index
std::string gen_untile_fs(u32 fmt);                                       // the same for a plain format, 0 to 11
std::string untile_key(u32 fmt);                                          // its entry in the fragment index

std::string fs_key(const DrawState &ds);
constexpr u32 FS_KEY_WORDS = 64;
u32 fs_key_words(const DrawState &ds, u32 *w);  // the same key as words, without making a string; returns how many
DrawState fs_normal(const DrawState &ds);
DrawState decode_key(const std::string &k);
u64 fnv64(const void *p, size_t n);
// a combiner reads the procedural texture. shortcut: not lighting's bump or shadow input, which still reads zero there; add it if a game does that
inline bool uses_proctex(const DrawState &ds) {
    for (u32 i = 0; i < ds.tev_count; i++) {
        const TevStageState &t = ds.tev[ds.tev_list[i]];
        for (int k = 0; k < 3; k++)
            if (t.csrc[k] == 6 || t.asrc[k] == 6) return true;
    }
    return false;
}
// the key of a vertex shader: the program, its entry point, the uniforms its flow control reads, and its output map
u64 vs_variant_key(const ShaderProgram *p, u32 entry, const bool *b, const s8 *slot_src);

// the compiled shaders' index, shaders.idx: this header, then the fragment and vertex keys with their indices in shaders.gsh
struct ShaderIndexHeader {
    u32 magic;  // 'SHDX'
    u32 version, num_ps, num_vs;
    u32 passthrough_vs, blit_vs, blit_ps, pad;
};
struct ShaderIndexEntry {
    u32 key_hi, key_lo;
    u32 index, inputs;  // inputs: the input registers a vertex shader reads
};
constexpr u32 kShaderIndexMagic = 0x53484458;
constexpr u32 kShaderIndexVersion = 1;
}  // namespace gpu
