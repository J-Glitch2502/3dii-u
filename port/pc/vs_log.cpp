// Records which vertex programs and fragment states the game uses, for the shader generator: vs_log.bin in the run directory.
#include <cstdio>
#include <set>

#include "../wiiu/shaderkeys.cpp"

namespace gpu {
void vs_log(const ShaderProgram *p, u32 entry, const ShaderSetup &s, const s8 *slot_src) {
    static std::set<u64> seen;
    static FILE *f = fopen("vs_log.bin", "ab");
    const u64 k = vs_variant_key(p, entry, s.b, slot_src);
    if (!f || !seen.insert(k).second) return;
    fwrite("VS  ", 1, 4, f);
    const u32 hdr[3] = {entry, s.code_size, s.swizzle_size};
    fwrite(hdr, 4, 3, f);
    fwrite(s.code, 4, s.code_size, f);
    fwrite(s.swizzle, 4, s.swizzle_size, f);
    fwrite(s.b, 1, 16, f);
    fwrite(s.i, 1, 16, f);
    fwrite(slot_src, 1, 24, f);
    fflush(f);
}
void fs_log(const DrawState &ds) {
    static std::set<std::string> seen;
    static FILE *f = fopen("fs_log.bin", "ab");
    const std::string k = fs_key(ds);
    if (!f || !seen.insert(k).second) return;
    const u32 n = (u32)k.size();
    fwrite(&n, 4, 1, f);
    fwrite(k.data(), 1, n, f);
    fflush(f);
}
}  // namespace gpu
