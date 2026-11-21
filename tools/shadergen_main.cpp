// Build step: compiles every shader the game needs ahead of time, so nothing compiles at run time.
//   shadergen <pipelines.bin> <fs and vs log dir> <output dir> <glsl compiler>
// Inputs: the fragment state list shipped with the recompiler, plus what runs of the game recorded (vs_log.bin, fs_log.bin) and
// what the console logged as missing (shader_misses.bin, same records). Outputs shaders.gsh and shaders.idx.
//   shadergen --lists <out.bin> <romfs.bin>...
// finds the vertex programs a game's files set up from GPU command lists instead (shadergen_lists.cpp), as records for those logs.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "shadergen.h"

using namespace gpu;
namespace fsys = std::filesystem;

namespace {
std::vector<u8> read_file(const std::string &path) {
    std::vector<u8> d;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}
void write_file(const std::string &path, const std::string &s) {
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

struct VsRecord {
    u32 entry;
    std::vector<u32> code, swz;
    u8 b[16], iu[16];
    s8 slots[24];
};
// a vertex shader and the geometry shader after it (gen_gsx_glsl), as the console logs a draw it had no shader for (gx2_backend.cpp)
struct GsxRecord {
    VsRecord vs, gs;  // gs.slots: the geometry shader's output map
    u32 vs_outmask, gs_in, turns;
    u64 gs_perm;
};
std::vector<std::string> fs_keys;
std::vector<VsRecord> vs_records;
std::vector<GsxRecord> gsx_records;

// a log holds "VS  " records, or (fragment logs) length-prefixed keys, or "FS  " records among shader misses, or "GSX " records (their size
// after the tag first)
void read_log(const std::string &path) {
    const std::vector<u8> d = read_file(path);
    size_t p = 0;
    auto u32at = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    while (p + 8 <= d.size()) {
        if (!memcmp(&d[p], "GSX ", 4)) {
            const size_t end = p + 8 + u32at(p + 4);
            if (end > d.size()) break;
            size_t q = p + 8;
            GsxRecord r;
            const u32 vnc = u32at(q + 4), vns = u32at(q + 8), gnc = u32at(q + 16), gns = u32at(q + 20);
            if (44 + ((size_t)vnc + vns + gnc + gns) * 4 + 88 != end - q) break;
            r.vs.entry = u32at(q);
            r.gs.entry = u32at(q + 12);
            r.vs_outmask = u32at(q + 24);
            r.gs_in = u32at(q + 28);
            r.gs_perm = u32at(q + 32) | (u64)u32at(q + 36) << 32;
            r.turns = u32at(q + 40);
            q += 44;
            using Words = std::pair<std::vector<u32> *, u32>;
            for (const Words &w : {Words{&r.vs.code, vnc}, Words{&r.vs.swz, vns}, Words{&r.gs.code, gnc}, Words{&r.gs.swz, gns}}) {
                w.first->resize(w.second);
                memcpy(w.first->data(), &d[q], w.second * 4);
                q += w.second * 4;
            }
            memcpy(r.vs.b, &d[q], 16);
            memcpy(r.vs.iu, &d[q + 16], 16);
            memcpy(r.gs.b, &d[q + 32], 16);
            memcpy(r.gs.iu, &d[q + 48], 16);
            memcpy(r.gs.slots, &d[q + 64], 24);
            gsx_records.push_back(std::move(r));
            p = end;
        } else if (!memcmp(&d[p], "VS  ", 4)) {
            VsRecord r;
            r.entry = u32at(p + 4);
            const u32 nc = u32at(p + 8), ns = u32at(p + 12);
            p += 16;
            r.code.resize(4096);
            r.swz.resize(4096);
            memcpy(r.code.data(), &d[p], nc * 4);
            memcpy(r.swz.data(), &d[p + nc * 4], ns * 4);
            r.code.resize(nc);
            r.swz.resize(ns);
            p += (nc + ns) * 4;
            memcpy(r.b, &d[p], 16);
            memcpy(r.iu, &d[p + 16], 16);
            memcpy(r.slots, &d[p + 32], 24);
            p += 56;
            vs_records.push_back(std::move(r));
        } else {
            size_t q = p;
            if (!memcmp(&d[p], "FS  ", 4)) q += 4;
            const u32 n = u32at(q);
            fs_keys.emplace_back((const char *)&d[q + 4], n);
            p = q + 4 + n;
        }
    }
}

std::string hex64(u64 v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
}  // namespace

int lists_main(int argc, char **argv);  // shadergen_lists.cpp

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--lists")) return lists_main(argc, argv);
    if (argc < 5) {
        fprintf(stderr, "usage: shadergen <pipelines.bin> <log dir> <output dir> <compiler>\n       shadergen --lists <out.bin> <romfs.bin>...\n");
        return 2;
    }
    const std::string pipelines = argv[1], logs = argv[2], out = argv[3], compiler = argv[4];
    const std::string glsl = out + "/glsl";
    fsys::create_directories(glsl);

    const std::vector<u8> pl = read_file(pipelines);
    if (pl.size() >= 8)
        for (size_t p = 8; p + 4 <= pl.size();) {
            u32 n;
            memcpy(&n, &pl[p], 4);
            fs_keys.emplace_back((const char *)&pl[p + 4], n);
            p += 4 + n;
        }
    const size_t shipped = fs_keys.size();
    // a finished game's list first (builder/shaderlogs.py, confirm): its shaders keep their numbers, and what the logs add since comes after
    if (fsys::exists(fsys::path(logs) / "confirmed.bin")) read_log((fsys::path(logs) / "confirmed.bin").string());
    for (const auto &e : fsys::directory_iterator(logs))
        if (e.path().extension() == ".bin" && e.path().filename() != "confirmed.bin") read_log(e.path().string());
    printf("%zu fragment keys (%zu shipped), %zu vertex records\n", fs_keys.size(), shipped, vs_records.size());

    // fragment shaders: many states differ only in what the source doesn't depend on (blending, say)
    std::map<std::string, u32> ps_sources;
    std::vector<std::string> ps_files;
    std::map<u64, u32> ps_index;
    auto add_ps = [&](const std::string &src) {
        auto it = ps_sources.find(src);
        if (it != ps_sources.end()) return it->second;
        const u32 i = (u32)ps_files.size();
        const std::string file = glsl + "/ps" + std::to_string(i) + ".frag";
        write_file(file, src);
        ps_files.push_back(file);
        ps_sources[src] = i;
        return i;
    };
    const u32 blit_ps = add_ps(gen_blit_fs());
    ps_index[fnv64(kDepthCopyKey, strlen(kDepthCopyKey))] = add_ps(gen_depth_copy_fs());
    const u32 fallback_ps = add_ps("#version 450\nlayout(location = 0) in vec4 vColor;\nlayout(location = 0) out vec4 FragColor;\nvoid main() { FragColor = vColor; }\n");
    const u32 fallback_tex_ps = add_ps("#version 450\nlayout(location = 0) in vec4 vColor;\nlayout(location = 1) in vec4 vTc01;\nlayout(location = 0) out vec4 FragColor;\n"
                                       "layout(binding = 0) uniform sampler2D t0;\nvoid main() { FragColor = clamp(vColor, 0.0, 1.0) * texture(t0, vTc01.xy); }\n");
    if (fallback_tex_ps != fallback_ps + 1) {
        fprintf(stderr, "the renderer expects the two fallback shaders side by side\n");
        return 1;
    }
    for (const std::string &k : fs_keys) {
        const DrawState ds = fs_normal(decode_key(k));
        const u32 i = add_ps(gen_fs_glsl(ds));
        ps_index[fnv64(k.data(), k.size())] = i;
        const std::string normal = fs_key(ds);
        ps_index[fnv64(normal.data(), normal.size())] = i;  // what the console looks up when the exact state isn't here
    }
    ps_index[fnv64(kUberKey, strlen(kUberKey))] = add_ps(gen_uber_fs(false));
    ps_index[fnv64(kUberLitKey, strlen(kUberLitKey))] = add_ps(gen_uber_fs(true));
    ps_index[fnv64(kEtc1Key, strlen(kEtc1Key))] = add_ps(gen_etc1_fs(false));
    ps_index[fnv64(kEtc1A4Key, strlen(kEtc1A4Key))] = add_ps(gen_etc1_fs(true));
    for (u32 fmt = 0; fmt < 12; fmt++) {
        const std::string k = untile_key(fmt);
        ps_index[fnv64(k.data(), k.size())] = add_ps(gen_untile_fs(fmt));
    }

    // vertex shaders: every combination of the bool uniforms each program's flow control reads, if there aren't too many (kVsBoolsMade)
    std::map<std::string, u32> vs_sources;
    std::vector<std::string> vs_files;
    struct VsOut { u32 index, inputs; };
    std::map<u64, VsOut> vs_index;
    auto add_vs = [&](const std::string &src) {
        auto it = vs_sources.find(src);
        if (it != vs_sources.end()) return it->second;
        const u32 i = (u32)vs_files.size();
        const std::string file = glsl + "/vs" + std::to_string(i) + ".vert";
        write_file(file, src);
        vs_files.push_back(file);
        vs_sources[src] = i;
        return i;
    };
    const u32 passthrough = add_vs(gen_passthrough_vs());
    const u32 blit_vs = add_vs(gen_blit_vs());
    u32 failed = 0;
    std::set<std::pair<u32, u32>> failed_seen;
    for (const VsRecord &r : vs_records) {
        std::vector<u32> code = r.code, swz = r.swz;
        const u32 nc = (u32)code.size(), ns = (u32)swz.size();
        code.resize(4096);
        swz.resize(4096);
        ShaderProgram *p = shader_decode(code.data(), nc, swz.data());
        (void)ns;
        u64 identity;
        u32 mask, loops;
        shader_entry_info(p, r.entry, &identity, &mask, &loops);
        std::vector<u32> bits;
        for (u32 i = 0; i < 16; i++)
            if ((mask >> i) & 1) bits.push_back(i);
        const u32 combos = bits.size() <= kVsBoolsMade ? 1u << bits.size() : 1;
        const u8 (*iu)[4] = (const u8 (*)[4])r.iu;
        u32 own = 0;  // the record's own set
        for (u32 i = 0; i < 16; i++)
            if (r.b[i]) own |= 1u << i;
        for (u32 c = 0; c < combos; c++) {
            bool b[16];
            memcpy(b, r.b, 16);
            if (combos > 1)
                for (size_t k = 0; k < bits.size(); k++) b[bits[k]] = (c >> k) & 1;
            u32 bm = 0;
            for (u32 i = 0; i < 16; i++)
                if (b[i]) bm |= 1u << i;
            // a made-up set under which the program never ends: no game draws with it
            if (bm != own && !shader_ends(p, r.entry, bm)) continue;
            u32 inputs = 0;
            const std::string src = gen_vs_glsl(p, r.entry, bm, iu, r.slots, &inputs);
            if (src.empty()) {
                if (!failed_seen.count({r.entry, bm})) {
                    failed_seen.insert({r.entry, bm});
                    printf("not translated: entry %03x, bools %04x: shader.cpp line %d gave up at instruction %03x\n", r.entry, bm, gpu::shader_glsl_fail_line, gpu::shader_glsl_fail_at);
                }
                failed++;
                continue;
            }
            const u64 key = vs_variant_key(p, r.entry, b, r.slots);
            vs_index[key] = {add_vs(src), inputs};
        }
        // more bools than every set is made of: besides the record's own set, the shader that reads them as it runs, for every other set
        if (const u32 open = vs_open_bools(p, r.entry)) {
            u32 inputs = 0;
            const bool none[16] = {};  // open bools aren't in the key
            const std::string src = gen_vs_glsl(p, r.entry, 0, iu, r.slots, &inputs, open);
            if (src.empty()) {
                printf("not translated: open bools, entry %03x, mask %04x: shader.cpp line %d gave up at instruction %03x\n", r.entry, open, gpu::shader_glsl_fail_line, gpu::shader_glsl_fail_at);
                failed++;
            }
            else vs_index[vs_variant_key(p, r.entry, none, r.slots, open)] = {add_vs(src), inputs};
        }
    }
    // a vertex shader and the geometry shader after it, for the set of bools the run met: the index entry's inputs carry the triangles a run makes.
    // gsx.txt names them, for tools/win/build3ds.py: one a GPU can't take is left out of the index, and its draws stay with the CPU.
    u32 gsx = 0;
    std::set<u32> gsx_shaders;
    for (const GsxRecord &r : gsx_records) {
        auto decode = [](const VsRecord &v) {
            std::vector<u32> code = v.code, swz = v.swz;
            const u32 nc = (u32)code.size();
            code.resize(4096);
            swz.resize(4096);
            return shader_decode(code.data(), nc, swz.data());
        };
        ShaderProgram *vs = decode(r.vs), *gs = decode(r.gs);
        bool vb[16], gb[16];
        u32 vbm = 0, gbm = 0;
        for (u32 i = 0; i < 16; i++) {
            vb[i] = r.vs.b[i];
            gb[i] = r.gs.b[i];
            vbm |= (u32)vb[i] << i;
            gbm |= (u32)gb[i] << i;
        }
        u32 inputs = 0, tris = 0;
        const std::string src = gen_gsx_glsl(vs, r.vs.entry, vbm, (const u8 (*)[4])r.vs.iu, gs, r.gs.entry, gbm, (const u8 (*)[4])r.gs.iu, r.vs_outmask, r.gs_in,
                                             r.gs_perm, r.gs.slots, r.turns, &inputs, &tris);
        if (src.empty() || tris > 0xffff) {
            failed++;
            continue;
        }
        const u32 i = add_vs(src);
        const u64 key = gsx_variant_key(vs, r.vs.entry, vb, gs, r.gs.entry, gb, (const u8 (*)[4])r.gs.iu, r.vs_outmask, r.gs_in, r.gs_perm, r.gs.slots,
                                        r.turns);
        vs_index[key] = {i, (inputs & 0xffff) | tris << 16};
        gsx_shaders.insert(i);
        gsx++;
    }
    std::string listed;
    for (u32 i : gsx_shaders) listed += std::to_string(i) + "\n";
    write_file(glsl + "/gsx.txt", listed);
    printf("%zu fragment and %zu vertex shaders to compile (%u drawing geometry shaders); %u variants could not be translated\n", ps_files.size(), vs_files.size(),
           gsx, failed);

    // compiler "-": only list the files, in order, for tools/win/build3ds.py. Windows can't take thousands of file names on one command line, so it compiles them in batches.
    const bool list_only = compiler == "-";
    if (list_only) {
        std::string names;
        for (const auto *files : {&vs_files, &ps_files})
            for (const auto &f : *files) names += fsys::path(f).filename().string() + "\n";
        write_file(glsl + "/list.txt", names);
    }
    if (!list_only) {
    std::string cmd = compiler;
    for (const auto &f : vs_files) cmd += " -vs " + f;
    for (const auto &f : ps_files) cmd += " -ps " + f;
    cmd += " -o " + out + "/shaders.gsh > " + out + "/compile.log 2>&1";
    const int rc = system(cmd.c_str());
    if (rc) {
        printf("compiler failed (%d), see %s/compile.log\n", rc, out.c_str());
        return 1;
    }

    // GX2SetShaderMode leaves 48 registers for vertex shaders and 200 for pixel shaders. A shader asking for more can't start on a real GPU, and Cemu doesn't mind.
    {
        const std::vector<u8> g = read_file(out + "/shaders.gsh");
        auto word = [&](size_t at) { return (u32)g[at] << 24 | g[at + 1] << 16 | g[at + 2] << 8 | g[at + 3]; };
        u32 most_vs = 0, most_ps = 0;
        for (size_t p = word(4); p + 32 <= g.size() && !memcmp(&g[p], "BLK{", 4);) {
            const u32 head = word(p + 4), type = word(p + 16), size = word(p + 20);
            if (type == 3) most_vs = std::max(most_vs, word(p + head) & 255);
            if (type == 6) most_ps = std::max(most_ps, word(p + head) & 255);
            p += head + size;
        }
        printf("registers: vertex shaders use up to %u of 48, pixel shaders up to %u of 200\n", most_vs, most_ps);
        if (!most_vs || most_vs > 48 || most_ps > 200) return 1;
    }
    }

    // the index: big-endian, as the console reads it
    auto be = [](u32 v) { return __builtin_bswap32(v); };
    FILE *f = fopen((out + "/shaders.idx").c_str(), "wb");
    ShaderIndexHeader h{be(kShaderIndexMagic), be(kShaderIndexVersion), be((u32)ps_index.size()), be((u32)vs_index.size()),
                        be(passthrough), be(blit_vs), be(blit_ps), be(fallback_ps)};
    fwrite(&h, sizeof h, 1, f);
    for (const auto &[k, i] : ps_index) {
        ShaderIndexEntry e{be((u32)(k >> 32)), be((u32)k), be(i), 0};
        fwrite(&e, sizeof e, 1, f);
    }
    for (const auto &[k, v] : vs_index) {
        ShaderIndexEntry e{be((u32)(k >> 32)), be((u32)k), be(v.index), be(v.inputs)};
        fwrite(&e, sizeof e, 1, f);
    }
    fclose(f);
    printf("index: %zu fragment and %zu vertex keys\n", ps_index.size(), vs_index.size());
    return 0;
}
