// Build step: translates discovered shader states ahead of time. States only
// assembled during gameplay may still need runtime compilation or fallback.
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
#include <tuple>
#include <vector>

#include "shadergen.h"
#include "latte_program.h"

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

constexpr u32 kShaderWords = 4096;

bool read_fs_key(const std::vector<u8> &data, size_t &p) {
    if (data.size() - p < 4) return false;
    u32 n;
    memcpy(&n, data.data() + p, 4);
    if (!n || n % 4 || n > 4 * FS_KEY_WORDS || n > data.size() - p - 4) return false;
    fs_keys.emplace_back((const char *)data.data() + p + 4, n);
    p += 4 + n;
    return true;
}

// A shader miss log can also contain standalone "GS  " records for the CPU compiler.
void read_log(const std::string &path) {
    const std::vector<u8> d = read_file(path);
    size_t p = 0;
    auto u32at = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    while (d.size() - p >= 4) {
        if (!memcmp(&d[p], "GSX ", 4)) {
            if (d.size() - p < 8) break;
            const u32 size = u32at(p + 4);
            if (size < 44 + 88 || size > d.size() - p - 8) break;
            const size_t end = p + 8 + size;
            size_t q = p + 8;
            GsxRecord r;
            const u32 vnc = u32at(q + 4), vns = u32at(q + 8), gnc = u32at(q + 16), gns = u32at(q + 20);
            if (std::max({vnc, vns, gnc, gns}) > kShaderWords || 44 + (vnc + vns + gnc + gns) * 4 + 88 != size) break;
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
        } else if (!memcmp(&d[p], "GS  ", 4)) {
            // Legacy CPU geometry records are not fragment keys. Native
            // geometry compilation consumes their separate .gsl representation.
            if (p + 16 > d.size()) break;
            const u32 nc = u32at(p + 8), ns = u32at(p + 12);
            if (nc > 4096 || ns > 4096) break;
            const size_t size = 16 + ((size_t)nc + ns) * 4;
            if (size > d.size() - p) break;
            p += size;
        } else if (!memcmp(&d[p], "VS  ", 4)) {
            if (d.size() - p < 16 + 56) break;
            VsRecord r;
            r.entry = u32at(p + 4);
            const u32 nc = u32at(p + 8), ns = u32at(p + 12);
            if (std::max(nc, ns) > kShaderWords || (nc + ns) * 4 > d.size() - p - 16 - 56) break;
            p += 16;
            r.code.resize(nc);
            r.swz.resize(ns);
            if (nc) memcpy(r.code.data(), &d[p], nc * 4);
            if (ns) memcpy(r.swz.data(), &d[p + nc * 4], ns * 4);
            p += (nc + ns) * 4;
            memcpy(r.b, &d[p], 16);
            memcpy(r.iu, &d[p + 16], 16);
            memcpy(r.slots, &d[p + 32], 24);
            p += 56;
            vs_records.push_back(std::move(r));
        } else if (!memcmp(&d[p], "GS  ", 4)) {
            if (d.size() - p < 16) break;
            const u32 nc = u32at(p + 8), ns = u32at(p + 12);
            if (std::max(nc, ns) > kShaderWords || (nc + ns) * 4 > d.size() - p - 16) break;
            p += 16 + (nc + ns) * 4;
        } else {
            if (!memcmp(&d[p], "FS  ", 4)) p += 4;
            if (!read_fs_key(d, p)) break;
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

int fallbackgen_main(int argc, char **argv);

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--fallback")) return fallbackgen_main(argc - 1, argv + 1);
    if (argc >= 2 && !strcmp(argv[1], "--lists")) return lists_main(argc, argv);
    if (argc < 5) {
        fprintf(stderr, "usage: shadergen <pipelines.bin> <log dir> <output dir> <compiler>\n       shadergen --lists <out.bin> <romfs.bin>...\n");
        return 2;
    }
    const std::string pipelines = argv[1], logs = argv[2], out = argv[3], compiler = argv[4];
    const std::string glsl = out + "/glsl";
    fsys::create_directories(glsl);

    const std::vector<u8> pl = read_file(pipelines);
    for (size_t p = 8; p < pl.size() && read_fs_key(pl, p);) {}
    const size_t shipped = fs_keys.size();
    std::vector<std::string> preferred_fs_keys = fs_keys;
    // a finished game's list first (builder/shaderlogs.py, confirm): its shaders keep their numbers, and what the logs add since comes after
    if (fsys::exists(fsys::path(logs) / "confirmed.bin")) {
        read_log((fsys::path(logs) / "confirmed.bin").string());
        preferred_fs_keys = fs_keys;
    }
    for (const auto &e : fsys::directory_iterator(logs)) {
        if (e.path().extension() != ".bin" || e.path().filename() == "confirmed.bin") continue;
        const auto begin = fs_keys.size();
        read_log(e.path().string());
        if (!e.path().filename().string().starts_with("other_"))
            preferred_fs_keys.insert(preferred_fs_keys.end(), fs_keys.begin() + begin, fs_keys.end());
    }
    printf("%zu fragment keys (%zu shipped), %zu vertex records\n", fs_keys.size(), shipped, vs_records.size());

    // Preserve the exact discovered inputs for material/lighting family generation.
    std::string fragment_inputs;
    const std::set<std::string> own_states(preferred_fs_keys.begin(), preferred_fs_keys.end());
    preferred_fs_keys.insert(preferred_fs_keys.end(), fs_keys.begin(), fs_keys.end());
    std::set<std::string> written;
    for (const auto &key : preferred_fs_keys) {
        if (!written.insert(key).second) continue;
        const u32 n = (u32)key.size();
        for (unsigned i = 0; i < 4; ++i) fragment_inputs += char(n >> (8 * i));
        fragment_inputs += key;
    }
    write_file(out + "/fragment-states.bin", fragment_inputs);
    write_file(out + "/fallback-input-report.json", "{\"preferred_game_states\":" +
        std::to_string(own_states.size()) + ",\"total_unique_states\":" + std::to_string(written.size()) + "}\n");
    printf("Fallback discovery inputs: %zu preferred game states, %zu total unique states\n", own_states.size(), written.size());

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
    for (bool lit : {false, true}) for (u32 mask = 0; mask < 8; mask++) {
        const std::string key = uber_family_key(lit, mask);
        ps_index[fnv64(key.data(), key.size())] = add_ps(gen_uber_fs(lit, mask));
    }
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
    // Machine-readable failures survive the abbreviated build log. No shader
    // words or generated source belong in a user-submitted diagnostic report.
    std::string diagnostics = "stage\tprogram_id\tentry\tvariant\tpc\ttranslator_line\tfeature\n";
    auto diagnostic = [&](const char *stage, u64 program_id, u32 entry, u32 variant, u32 pc, int line, const char *feature) {
        diagnostics += std::string(stage) + "\t" + std::to_string(program_id) + "\t" + std::to_string(entry) + "\t" + std::to_string(variant) + "\t" +
                       std::to_string(pc) + "\t" + std::to_string(line) + "\t" + feature + "\n";
    };
    std::set<std::tuple<u64, u32, u32>> failed_seen;
    for (const VsRecord &r : vs_records) {
        std::vector<u32> code = r.code, swz = r.swz;
        const u32 nc = (u32)code.size(), ns = (u32)swz.size();
        code.resize(kShaderWords);
        swz.resize(kShaderWords);
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
                if (!failed_seen.count({identity, r.entry, bm})) {
                    failed_seen.insert({identity, r.entry, bm});
                    printf("not translated: entry %03x, bools %04x: shader.cpp line %d gave up at instruction %03x\n", r.entry, bm, gpu::shader_glsl_fail_line, gpu::shader_glsl_fail_at);
                    diagnostic("vertex", identity, r.entry, bm, gpu::shader_glsl_fail_at, gpu::shader_glsl_fail_line, "translation_rejected");
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
                diagnostic("vertex_open_bools", identity, r.entry, open, gpu::shader_glsl_fail_at, gpu::shader_glsl_fail_line, "translation_rejected");
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
            code.resize(kShaderWords);
            swz.resize(kShaderWords);
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
            // GSX may fail before the vertex translator's diagnostic variables
            // are updated, so do not attach a stale instruction/line to it.
            u64 geometry_id;
            u32 geometry_bools, geometry_loops;
            shader_entry_info(gs, r.gs.entry, &geometry_id, &geometry_bools, &geometry_loops);
            diagnostic("geometry_pipeline", geometry_id, r.gs.entry, gbm, 0, 0,
                       src.empty() ? "translation_rejected" : "triangle_count_limit");
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
    write_file(out + "/shader-diagnostics.tsv", diagnostics);
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

    // The runtime reserves this VS maximum once; both compiler stages have at most124 ordinary GPRs.
    {
        std::vector<u8> g = read_file(out + "/shaders.gsh");
        if (g.size() < 32 || memcmp(g.data(), "Gfx2", 4)) return 1;
        auto word = [&](size_t at) { return (u32)g[at] << 24 | g[at + 1] << 16 | g[at + 2] << 8 | g[at + 3]; };
        u32 most_vs = 0, most_ps = 0, declared_vs = 0, declared_ps = 0, vertex_count = 0, pixel_count = 0;
        bool have_vs_header = false, have_ps_header = false;
        const u32 first = word(4);
        if (first < 32 || first > g.size()) return 1;
        for (size_t p = first; p + 32 <= g.size() && !memcmp(&g[p], "BLK{", 4);) {
            const u32 head = word(p + 4), type = word(p + 16), size = word(p + 20);
            if (head < 32 || head > g.size() - p || size > g.size() - p - head) return 1;
            if (type == 3 || type == 6) {
                if (size < 4) return 1;
                (type == 3 ? declared_vs : declared_ps) = word(p + head) & 255;
                (type == 3 ? have_vs_header : have_ps_header) = true;
            }
            if (type == 5 || type == 7) {
                const bool vertex = type == 5;
                const u32 declared = vertex ? declared_vs : declared_ps;
                const u32 index = vertex ? vertex_count++ : pixel_count++;
                const auto &files = vertex ? vs_files : ps_files;
                const char *file = index < files.size() ? files[index].c_str() : "shaders.gsh";
                bool &have_header = vertex ? have_vs_header : have_ps_header;
                if (!have_header) {
                    printf("%s program %u (%s): missing matching shader header\n", vertex ? "vertex" : "pixel", index, file);
                    return 1;
                }
                have_header = false;
                u32 required;
                if (!program_registers(g.data() + p + head, size, declared, required) || required != declared) {
                    if (!declared) printf("%s program %u (%s): zero-register metadata references ordinary registers\n", vertex ? "vertex" : "pixel", index, file);
                    printf("%s program %u (%s): invalid layout/metadata or ordinary registers exceed124 (declared %u)\n", vertex ? "vertex" : "pixel", index, file, declared);
                    return 1;
                }
                (vertex ? most_vs : most_ps) = std::max(vertex ? most_vs : most_ps, required);
            }
            p += head + size;
        }
        if (!vertex_count) { printf("no vertex programs discovered (not register pressure)\n"); return 1; }
        const u32 allocation = std::max(shader_budget::kDefaultVertex, most_vs);
        printf("registers: vertex programs require up to %u/124, pixel up to %u/124; runtime partition VS %u, PS %u\n", most_vs, most_ps, allocation, shader_budget::pixel(allocation));
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
