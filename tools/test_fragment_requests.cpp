// The production selector and GLSL generator are extracted by check.py.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include "shadergen.h"

void rt_log(const char *, ...) {}
int g_verbose;
namespace gpu {
GpuState g;
#include "fs_key.inc"
#include "fragment_key_hash.inc"
}
#include "fragment_generator.inc"

using namespace gpu;
struct Block { const char *name; u32 offset, size; };
struct Sampler { const char *name; u32 location; };
struct GX2PixelShader {
    u32 uniformBlockCount = 0, samplerVarCount = 0, uniformVarCount = 0;
    Block *uniformBlocks = nullptr;
    Sampler *samplerVars = nullptr;
};
#include "ps_entry_production.inc"
constexpr u32 MISSED = ~0u;
GX2PixelShader fallback, texture_fallback, uber, lit_uber, shipped;
GX2PixelShader *ps_fallback = &fallback, *ps_fallback_tex = &texture_fallback;
GX2PixelShader *ps_uber = &uber, *ps_uber_lit = &lit_uber;
#include "ps_cache_production.inc"
std::unordered_map<u64, PsEntry> ps_table;
std::atomic<bool> uber_all{false};
bool uber_fallback = true, glsl_force, compiler_enabled = true;
GX2PixelShader *test_uber_override;
u64 st_ps_misses;
std::vector<std::pair<u64, std::string>> requests;
bool shader_compile_on() { return compiler_enabled; }
void shader_compile_request(bool vertex, u64 key, const std::string &source, u32 inputs) {
    if (vertex || inputs || source.empty()) std::abort();
    requests.emplace_back(key, source);
}
GX2PixelShader *load_ps(u32) { return &shipped; }
void miss_put(const void *, size_t) {}
GX2PixelShader *uber_for(const DrawState &ds) { return test_uber_override ? test_uber_override : ds.lighting ? ps_uber_lit : ps_uber; }
#include "find_ps_production.inc"

static int checks, failures;
static void expect(bool value, const char *message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
static void reset() {
    ps_last_ds = nullptr;
    ps_last = nullptr;
    ps_last_version = 0;
    ps_seen.clear();
    ps_fallback_epoch = 1;
    ps_last_forced = false;
    test_uber_override = nullptr;
    ps_table.clear();
    requests.clear();
    compiler_enabled = true;
    glsl_force = uber_all = false;
    st_ps_misses = 0;
    ++g.ds_version;
}
static DrawState state(bool shade, bool wbuffer = false, u32 scissor = 0) {
    DrawState ds{};
    ds.need_shade = ds.color_write = shade;
    ds.depth_write = true;
    ds.wbuffer = wbuffer;
    ds.scissor_mode = scissor;
    ds.logic_op = 3;
    ds.tev_count = 1;
    ds.tev_list[0] = 0;
    ds.tev[0].cmul = ds.tev[0].amul = 1;
    return ds;
}
int main() {
    struct Case { const char *name; DrawState ds; bool request; };
    const Case cases[] = {
        {"plain Z depth-only", state(false), false},
        {"W-buffer depth-only", state(false, true), true},
        {"exclusion depth-only", state(false, false, 1), true},
        {"W-buffer plus exclusion depth-only", state(false, true, 1), true},
        {"unlit color", state(true), true},
        {"lit color", [] { auto d = state(true); d.lighting = true; return d; }(), true},
    };
    for (const Case &c : cases) {
        reset();
        const auto result = find_ps(c.ds);
        expect(requests.size() == size_t(c.request), c.name);
        expect(result == (c.ds.need_shade ? (c.ds.lighting ? &lit_uber : &uber) : &fallback), "fallback selection preserved");
        if (!requests.empty()) {
            const auto &source = requests[0].second;
            expect(source == gen_fs_glsl(fs_normal(c.ds)), "request contains exact production GLSL");
            expect(requests[0].first == fnv64(fs_key(fs_normal(c.ds)).data(), fs_key(fs_normal(c.ds)).size()), "request uses exact raw state key");
        }
        find_ps(c.ds);
        DrawState equivalent = c.ds;
        ++g.ds_version;
        find_ps(equivalent);
        expect(requests.size() == size_t(c.request) && st_ps_misses == 1, "same-object and equivalent-key hits never repeat request");

        reset();
        compiler_enabled = false;
        find_ps(c.ds);
        expect(requests.empty(), "disabled compiler never enqueues");

        for (bool force : {false, true}) {
            reset();
            glsl_force = force;
            const std::string key = fs_key(fs_normal(c.ds));
            ps_table[fnv64(key.data(), key.size())].index = 7;
            expect(find_ps(c.ds) == &shipped, "shipped normalized shader selected");
            expect(requests.size() == size_t(force && c.request), "force requests only states with fragment work");
            find_ps(c.ds);
            expect(requests.size() == size_t(force && c.request), "force cache hit never repeats");
        }
    }
    // Installing unrelated shaders must keep exact hits and refresh fallback hits.
    reset();
    auto color = state(true);
    auto key = fs_key(fs_normal(color));
    auto &exact = ps_table[fnv64(key.data(), key.size())];
    exact.index = 7;
    expect(find_ps(color) == &shipped, "cache an exact program");
    ps_table.clear();  // A table lookup now could not find it: the exact cache must survive.
    ++ps_fallback_epoch;
    ps_last_ds = nullptr;
    expect(find_ps(color) == &shipped && requests.empty(), "epoch keeps exact cached programs");

    reset();
    expect(find_ps(color) == &uber, "cache a fallback program");
    test_uber_override = &lit_uber;
    ++ps_fallback_epoch;
    ps_last_ds = nullptr;
    expect(find_ps(color) == &lit_uber, "epoch refreshes fallback placeholders");
    expect(requests.size() == 1, "fallback refresh does not requeue the same shader");

    reset();
    ps_table[fnv64(key.data(), key.size())].index = 7;
    expect(find_ps(color) == &shipped, "exact before force toggle");
    uber_all = true;
    expect(find_ps(color) == &uber, "force takes effect without a draw-state change");
    uber_all = false;
    expect(find_ps(color) == &shipped, "force off restores the cached exact program");

    auto plain = state(false), w = state(false, true), exclusion = state(false, false, 1);
    const std::string p = gen_fs_glsl(plain), ws = gen_fs_glsl(w), es = gen_fs_glsl(exclusion);
    expect(ws.find("gl_FragDepth = zd;") < ws.find("FragColor = vec4(0.0);"), "actual unshaded GLSL preserves W-buffer before color skip");
    expect(es.find("discard;") < es.find("FragColor = vec4(0.0);"), "actual unshaded GLSL preserves exclusion before color skip");
    expect(p.find("gl_FragDepth") == std::string::npos && p.find("discard;") == std::string::npos, "plain depth-only shader has no extra fragment work");
    expect(fs_key(fs_normal(plain)) != fs_key(fs_normal(w)) && fs_key(fs_normal(plain)) != fs_key(fs_normal(exclusion)), "production normalized keys retain depth/scissor semantic identity");
    expect(!state(true).lighting && state(true).need_shade, "unlit color is already shading work");
    std::printf("{\"cases\":6,\"checks\":%d,\"failures\":%d}\n", checks, failures);
    return failures ? 1 : 0;
}
