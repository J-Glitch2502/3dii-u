#undef NDEBUG
#include "latte_program.h"
#include "gfd_index.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gpu;
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
static unsigned failures, cases;
static void expect(bool yes, const char* name) {
    ++cases;
    if (!yes) {
        ++failures;
        std::printf("FAILED: %s\n", name);
    }
}
static std::vector<u8> bytes(const std::vector<u32>& words) {
    std::vector<u8> v(words.size() * 4);
    for (u32 i = 0; i < words.size(); ++i)
        gpu::latte::set_word(v.data(), i, words[i]);
    return v;
}
static std::vector<u32> exported(u32 reg = 0, u32 burst = 0) {
    return {reg << 15, 0x688u | burst << 17 | 40u << 23 | 1u << 21};
}
static void fit(const char* name, std::vector<u32> w, u32 declared, bool accept, u32 wanted, u32 limit = 124) {
    auto v = bytes(w), before = v;
    u32 count = declared;
    bool ok = gpu::fit_program(v.data(), (u32)v.size(), count, limit);
    expect(ok == accept && count == wanted && (ok || v == before), name);
}
static std::vector<u32> alu(u32 count, u32 source = 0, u32 destination = 123) {
    std::vector<u32> w{2, 8u << 26 | (count - 1) << 18, 0, 0x688u | 40u << 23 | 1u << 21};
    for (u32 i = 0; i < count; i++) {
        w.push_back(source | (i + 1 == count ? 1u << 31 : 0));
        w.push_back(0x10u | destination << 21);
    }
    return w;
}
static std::vector<u32> tex(u32 src, u32 dst, u32 opcode = 16) {
    return {2, 1u << 23, 0, 0x688u | 40u << 23 | 1u << 21, opcode | src << 16, dst | 0x688u << 9 | 1u << 21, 0, 0};
}
static std::vector<u32> constant_pos() {
    return {0, 0, 0, 0, 0xc0002000, 0xb24u | 40u << 23 | 1u << 21};
}
static std::vector<u32> escaped_jump() {
    return {2,         10u << 23,
            0,         0x688u | 40u << 23 | 1u << 21,
            4,         8u << 26,
            48u << 15, 0x688u | 40u << 23 | 1u << 21,
            1u << 31,  0x10u | 25u << 7 | 48u << 21};
}
#ifndef BUDGET_BASELINE
constexpr int GX2_SHADER_MODE_UNIFORM_BLOCK = 1;
static u32 vertex_registers = 48;
static bool state_lost = false;
static std::string events;
static void GX2DrawDone() {
    events += 'F';
}
static void GX2SetShaderModeEx(int mode, u32 vs, u32 vstack, u32 gs, u32 gstack, u32 ps, u32 pstack) {
    assert(mode == 1 && vstack == 64 && gs == 0 && gstack == 0 && pstack == 192 && vs == vertex_registers && ps >= 124 && vs + ps == 248);
    events += 'M';
}
#define LOG(...) ((void)0)
#define FATAL(...) throw std::runtime_error("invalid shader")
#include "shader_budget_gpu.inc"
struct GX2VertexShader {
    struct {
        u32 sq_pgm_resources_vs;
    } regs;
    void* program;
    u32 size;
};
struct GX2PixelShader {
    void* program;
    u32 size;
};
struct VsEntry {
    GX2VertexShader* shader = nullptr;
    u32 index = ~0u, inputs = 0;
};
struct PsEntry {
    GX2PixelShader* shader = nullptr;
    u32 index = ~0u;
};
struct CompileDone {
    bool vs;
    u64 key;
    u32 inputs;
    GX2VertexShader* vs_shader;
    GX2PixelShader* ps_shader;
    bool from_gfd = false;
};
static std::map<u64, VsEntry> vs_table;
static std::map<u64, PsEntry> ps_table;
static std::map<u64, u32> vs_set_aside;
static bool glsl_vs = true, in_cemu = false;
constexpr u32 MISSED = ~0u, COMPILED_HERE = 0x7ffffffe, GX2_INVALIDATE_MODE_CPU_SHADER = 1, GX2_SHADER_PROGRAM_ALIGNMENT = 256;
static std::vector<CompileDone> completions;
static std::map<int, int> ps_seen;
static const void* ps_last_ds;
static unsigned discarded;
static bool shader_compile_take(CompileDone& d) {
    if (completions.empty())
        return false;
    d = completions.front();
    completions.erase(completions.begin());
    return true;
}
static void shader_compile_discard(CompileDone&) {
    ++discarded;
}
static GX2VertexShader* load_vs(u32) {
    return nullptr;
}
static void vs_locations(VsEntry&) {}
static void log_vs_tables(const char*, u64, GX2VertexShader*) {}
static void GX2Invalidate(u32, const void*, u32) {
    events += 'I';
}
static void* memalign(size_t, size_t) {
    return nullptr;
}
#include "shader_budget_install.inc"
using GfdShader = gfd_index::Shader;
constexpr u32 kShaderIndexMagic = 0x53484458, kShaderIndexVersion = 1;
static std::vector<GfdShader> gfd_vs, gfd_ps;
static std::vector<u8> gfd_file_head, gfd_end;
#include "shader_budget_scan.inc"
static void be(std::vector<u8>& v, u32 x) {
    for (int i = 3; i >= 0; --i)
        v.push_back((u8)(x >> (8 * i)));
}
static void block(std::vector<u8>& v, u32 kind, const std::vector<u8>& body) {
    be(v, 0x424c4b7b);
    be(v, 32);
    be(v, 1);
    be(v, 0);
    be(v, kind);
    be(v, (u32)body.size());
    be(v, 0);
    be(v, 0);
    v.insert(v.end(), body.begin(), body.end());
}
static std::vector<u8> gfd(u32 vs, u32 ps = 1, bool constant_vertex = false) {
    std::vector<u8> v;
    be(v, 0x47667832);
    be(v, 32);
    v.resize(32);
    for (auto pair : {std::pair<u32, u32>{3, vs}, {6, ps}}) {
        if (pair.first == 3 && vs == 999)
            continue;
        std::vector<u8> h;
        be(h, pair.second);
        block(v, pair.first, h);
        block(v, pair.first == 3 ? 5 : 7,
              bytes(pair.first == 3 && constant_vertex ? std::vector<u32>{0xc0000000, 0x94200924}
                                                       : exported(pair.second ? pair.second - 1 : 0)));
    }
    std::vector<u8> h;
    be(h, 1);
    block(v, 6, h);
    block(v, 7, bytes(exported()));
    return v;
}
static std::vector<u8> index_file() {
    std::vector<u8> v;
    for (u32 x : {kShaderIndexMagic, 1u, 0u, 0u, 0u, 0u, 0u, 0u})
        be(v, x);
    return v;
}
static void scan(const std::vector<u8>& v, const std::vector<u8>& idx = index_file()) {
    gfd_vs.clear();
    gfd_ps.clear();
    auto complete = v;
    block(complete, 1, {});  // Synthetic fixtures include the END block required by the production loader.
    gfd_scan(complete.data(), (u32)complete.size(), idx.data(), (u32)idx.size());
}
static std::vector<u8> current_file;
static std::vector<u8> read_file(const std::string&) {
    return current_file;
}
static int standalone() {
    std::string out = "fixture";
    std::vector<std::string> vs_files{"v0.vert"}, ps_files{"p0.frag"};
#include "shader_budget_standalone.inc"
    return 0;
}
static std::vector<u8> standalone_headers(u32 omitted = 0) {
    std::vector<u8> file;
    be(file, 0x47667832);
    be(file, 32);
    file.resize(32);
    for (u32 head : {3u, 6u}) {
        std::vector<u8> metadata;
        be(metadata, 0);
        if (head != omitted)
            block(file, head, metadata);
        block(file, head == 3 ? 5 : 7, bytes(head == 3 ? constant_pos() : std::vector<u32>{0xc0000000, 0x94200924}));
    }
    return file;
}
static void install_tests() {
    auto position = bytes(constant_pos());
    u32 registers = 0;
    expect(fit_program(position.data(), (u32)position.size(), registers, 124) && registers == 0,
           "24-byte constant POS VS validates with zero GPRs");
    GX2VertexShader shader{{0}, position.data(), (u32)position.size()};
    for (u32 current : {48u, 49u, 124u}) {
        vertex_registers = current;
        events.clear();
        vs_table.clear();
        discarded = 0;
        completions = {{true, 7, 0, &shader, nullptr}};
        bool installed = true;
        try {
            install_compiled();
        } catch (...) {
            installed = false;
        }
        expect(installed && vs_table[7].shader == &shader && vs_table[7].index == COMPILED_HERE && vertex_registers == current &&
                   events == "I" && !discarded,
               "actual install publishes zero-GPR constant POS VS without promotion");
    }
    for (u32 current : {0u, 125u}) {
        vertex_registers = current;
        events.clear();
        bool refused = false;
        try {
            require_vs_registers(0);
        } catch (...) {
            refused = true;
        }
        expect(refused && events.empty() && vertex_registers == current, "zero demand still rejects invalid current partition");
    }
    vertex_registers = 48;
}
static void runtime_tests() {
    events.clear();
    require_vs_registers(48);
    expect(events.empty() && vertex_registers == 48, "default allocation stays48/200");
    require_vs_registers(49);
    expect(events == "FM" && vertex_registers == 49 && state_lost, "fence before49 allocation");
    require_vs_registers(48);
    expect(events == "FM" && vertex_registers == 49, "partition never shrinks");
    require_vs_registers(124);
    expect(events == "FMFM" && vertex_registers == 124, "maximum allocation124/124");
    bool zero_valid = true;
    try {
        require_vs_registers(0);
    } catch (...) {
        zero_valid = false;
    }
    expect(zero_valid && events == "FMFM" && vertex_registers == 124, "validated zero demand preserves allocation");
    for (u32 n : {125u}) {
        auto prior = events;
        bool rejected = false;
        try {
            require_vs_registers(n);
        } catch (...) {
            rejected = true;
        }
        expect(rejected && events == prior && vertex_registers == 124, "invalid promotion unchanged");
    }
    const auto before_resets = events;
    for (unsigned i = 0; i < 4; i++)
        shader_mode();
    expect(events == before_resets + "MMMM", "all resets preserve current partition");
    for (u32 n = 1; n <= 124; n++)
        for (u32 current = 48; current <= 124; current++)
            expect(gpu::shader_budget::vertex(current, n) == std::max(current, n) && gpu::shader_budget::pixel(std::max(current, n)) >= 124,
                   "monotonic pool invariant");
    for (u32 n : {48u, 49u, 124u}) {
        vertex_registers = 48;
        events.clear();
        auto file = gfd(n);
        scan(file);
        expect(vertex_registers == std::max(48u, n) && events == (n > 48 ? "FM" : ""), "actual GFD boot maximum");
        current_file = file;
        expect(standalone() == 0, "actual standalone accepts valid ordinary count");
    }
    for (auto pair : {std::pair<u32, u32>{125, 1}, {0, 1}, {49, 125}, {100, 150}, {999, 1}}) {
        vertex_registers = 48;
        events.clear();
        auto file = gfd(pair.first, pair.second);
        bool rejected = false;
        try {
            scan(file);
        } catch (...) {
            rejected = true;
        }
        expect(rejected && vertex_registers == 48 && events.empty(), "invalid known pack never changes allocation");
        current_file = file;
        expect(standalone() != 0, "standalone rejects pressure/metadata/empty discovery");
    }
    auto malformed = gfd(49);
    malformed[32 + 4] = 0xff;
    vertex_registers = 48;
    events.clear();
    bool rejected = false;
    try {
        scan(malformed);
    } catch (...) {
        rejected = true;
    }
    expect(rejected && events.empty(), "GFD bounds precede budget metadata access");
    auto constant = gfd(0, 1, true);
    scan(constant);
    expect(vertex_registers == 48 && events.empty(), "zero-reference VS is present and needs no promotion");
    current_file = constant;
    expect(standalone() == 0, "standalone distinguishes constant-only VS from empty discovery");
    auto dropped = gfd(49);
    std::vector<u8> h;
    be(h, 125);
    block(dropped, 3, h);
    block(dropped, 5, bytes(exported()));
    scan(dropped);
    expect(vertex_registers == 49, "unreferenced invalid GSX body stays excluded");
    auto kept = index_file();
    kept[15] = 1;
    for (u32 x : {0u, 0u, 1u, 0u})
        be(kept, x);
    vertex_registers = 48;
    events.clear();
    rejected = false;
    try {
        scan(dropped, kept);
    } catch (...) {
        rejected = true;
    }
    expect(rejected && events.empty() && vertex_registers == 48, "referenced invalid body is never admitted");
    for (u32 size : {0u, 31u}) {
        auto idx = index_file();
        idx.resize(size);
        rejected = false;
        try {
            scan(gfd(49), idx);
        } catch (...) {
            rejected = true;
        }
        expect(rejected && events.empty(), "short IDX rejected before promotion");
    }
    current_file = standalone_headers();
    expect(standalone() == 0, "standalone accepts explicit zero-GPR headers for both stages");
    for (u32 omitted : {3u, 6u}) {
        current_file = standalone_headers(omitted);
        expect(standalone() != 0, "standalone rejects orphan program without its matching header");
    }
}
#endif
int main(int argc, char** argv) {
    for (u32 n : {48u, 49u, 124u})
        fit("ordinary48/49/124 accepted", exported(n - 1), n, true, n);
    fit("125 rejected without repair spares", exported(), 125, false, 125);
    fit("125 physical ceiling independent of caller limit", exported(), 125, false, 125, 200);
    fit("PS150 is invalid even with a larger allocation", exported(), 150, false, 150, 200);
    fit("zero metadata distinct invalid", exported(), 0, false, 0);
    fit("constant-only zero GPR pixel program valid", {0xc0000000, 0x94200924}, 0, true, 0);
    fit("constant POS zero GPR VS valid", constant_pos(), 0, true, 0);
    fit("jump beyond first EOP cannot hide R48 references", escaped_jump(), 1, false, 1);
    fit("jump outside program refused unchanged", {0xffffffff, 10u << 23, 0, 0x688u | 40u << 23 | 1u << 21}, 1, false, 1);
    for (u32 kind : {18u, 20u})
        fit("ordinary CALL and RET unsupported", {0, kind << 23, 0, 0x688u | 40u << 23 | 1u << 21}, 1, false, 1);
    fit("noncanonical CALL_FS address rejected", {100, 19u << 23, 0, 0x688u | 40u << 23 | 1u << 21}, 1, false, 1);
    fit("noncanonical CALL_FS count rejected", {0, 19u << 23 | 1u << 13, 0, 0x688u | 40u << 23 | 1u << 21}, 1, false, 1);
    const auto control = [](u32 kind, u32 target) { return std::vector<u32>{target, kind << 23}; };
    std::vector<u32> nested;
    for (auto cf :
         {control(6, 7), control(10, 4), control(10, 4), control(0, 0xffffffff), control(13, 6), control(9, 6), control(5, 1), exported()})
        nested.insert(nested.end(), cf.begin(), cf.end());
    fit("nested branches inside loop stay in inspected prefix", nested, 1, true, 1);
    nested.clear();
    for (auto cf : {control(6, 7), control(6, 5), control(8, 4), control(9, 4), control(5, 2), control(8, 6), control(5, 1), exported()})
        nested.insert(nested.end(), cf.begin(), cf.end());
    fit("nested loops keep break and continue targets in prefix", nested, 1, true, 1);
    auto unary = alu(1, 248, 124);
    unary[3] = (unary[3] & ~0xfffu) | 0x924u;
    unary[5] |= 25u << 7;
    fit("unary constant to clause temporary needs no ordinary GPR", unary, 0, true, 0);
    unary[4] |= 253u << 13 | 3u << 23;
    fit("unused unary selector is not a literal or GPR reference", unary, 0, true, 0);
    fit("underdeclared ordinary reference normalized", exported(48), 1, true, 49);
    fit("real R123 use is preserved at declared124", alu(1, 0, 123), 124, true, 124);
    fit("ambiguous R123 source underdeclared refuses unchanged", alu(1, 123, 0), 4, false, 4);
    fit("repair can finish at124", alu(1), 123, true, 124);
    fit("two repair spares cannot cross124", alu(2), 123, false, 123);
    fit("export burst pastR127 rejects unchanged", exported(127, 1), 124, false, 124);
    fit("export burst may not enter clause temporaries", exported(123, 1), 124, false, 124);
    fit("real R123 export accepted", exported(123), 124, true, 124);
    fit("underdeclared R123 export ambiguous", exported(123), 123, false, 123);
    fit("texture source counted", tex(48, 0), 1, true, 49);
    fit("texture destination counted", tex(0, 48), 1, true, 49);
    fit("texture genuine R123 accepted", tex(123, 123), 124, true, 124);
    fit("texture R123 underdeclared refuses unchanged", tex(123, 0), 4, false, 4);
    fit("texture clause temporary invalid", tex(124, 0), 124, false, 124);
    fit("semantic fetch unsupported unchanged", tex(0, 0, 1), 4, false, 4);
    auto relative = tex(0, 0);
    relative[4] |= 1u << 23;
    fit("relative texture source unsupported unchanged", relative, 4, false, 4);
    relative = tex(0, 0);
    relative[5] |= 1u << 7;
    fit("relative texture destination unsupported unchanged", relative, 4, false, 4);
    auto clause = alu(2, 124, 124);
    fit("R124 clause temporary is not ordinary pressure", clause, 4, true, 4);
    auto relative_alu = alu(1, 0, 0);
    relative_alu[4] |= 1u << 9;
    fit("relative ALU source cannot be bounded", relative_alu, 4, false, 4);
    relative_alu = alu(1, 0, 0);
    relative_alu[5] |= 1u << 28;
    fit("relative ALU destination cannot be bounded", relative_alu, 4, false, 4);
    auto invalid = alu(1);
    invalid[0] = 100;
    fit("ALU outside program rejected", invalid, 4, false, 4);
    invalid = alu(1);
    invalid[4] &= ~(1u << 31);
    fit("unterminated group rejected", invalid, 4, false, 4);
    invalid = exported();
    invalid[1] &= ~(1u << 21);
    fit("missing end rejected", invalid, 4, false, 4);
    invalid = exported();
    invalid[0] |= 1u << 22;
    fit("relative export cannot be bounded", invalid, 4, false, 4);
    fit("unsupported memory control flow is not silently valid", {0, 32u << 23 | 1u << 21}, 4, false, 4);
#ifndef BUDGET_BASELINE
    invalid = tex(0, 0);
    invalid[0] = 0xffffffff;
    fit("fetch address overflow rejected", invalid, 4, false, 4);
    auto raw = bytes(exported());
    const auto before = raw;
    for (u32 n : {0u, 1u, 4u, 7u, 9u}) {
        u32 r = 4;
        expect(!gpu::fit_program(raw.data(), n, r, 124) && r == 4 && raw == before, "invalid sizes unchanged");
    }
    {
        u32 r = 4;
        expect(!gpu::fit_program(nullptr, 8, r, 124) && r == 4, "null unchanged");
    }
    runtime_tests();
    install_tests();
    if (argc == 3) {
        const auto read = [](const char* path) {
            std::ifstream f(path, std::ios::binary);
            return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
        };
        vertex_registers = 48;
        events.clear();
        const auto file = read(argv[1]), idx = read(argv[2]);
        try {
            scan(file, idx);
            std::printf("actual pack: %zu VS, %zu PS, partition %u/%u, events %s\n", gfd_vs.size(), gfd_ps.size(), vertex_registers,
                        248 - vertex_registers, events.c_str());
        } catch (const std::exception& e) {
            expect(false, e.what());
        }
    }
#endif
    std::printf("shader budget: %u assertions, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
