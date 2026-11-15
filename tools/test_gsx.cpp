// Checks, on a PC, the GLSL a geometry shader's draw gets (shader.cpp's shader_glsl_gs, shadergen.cpp's gen_gsx_glsl): small programs made up
// here that emit a quad, and ones whose result would depend on what an earlier run left, which must stay with the CPU.
// `python tools/selftest.py` builds it with the generator's own objects.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shadergen.h"

using namespace gpu;

static int failures;
static void expect(bool ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAILED: %s\n", what);
    }
}

// ---- PICA instructions (shader.cpp's decode_one has the layouts). Operand descriptor 0 writes xyzw, 1 only x; both read xyzw.
static u32 mov(u32 dst, u32 src, u32 desc = 0) { return 0x13u << 26 | dst << 21 | src << 12 | desc; }
static u32 add(u32 dst, u32 a, u32 b) { return 0x00u << 26 | dst << 21 | a << 12 | b << 7; }
static u32 setemit(u32 vid, u32 prim, u32 wind) { return 0x2bu << 26 | vid << 24 | prim << 23 | wind << 22; }
enum : u32 { EMIT = 0x2au << 26, END = 0x22u << 26, O0 = 0, T0 = 0x10, T1, T2, T3, C0 = 0x20, C1, C2, C3, C4 };
static u32 loop(u32 last) { return 0x29u << 26 | last << 10; }

static ShaderProgram *program(std::vector<u32> c) {
    static u32 swz[4096];
    const u32 same = 3u << 5 | 2u << 7 | 1u << 9 | 3u << 14 | 2u << 16 | 1u << 18 | 3u << 23 | 2u << 25 | 1u << 27;  // every source xyzw
    swz[0] = same | 0xf;
    swz[1] = same | 0x8;
    const u32 n = (u32)c.size();
    c.resize(4096);
    return shader_decode(c.data(), n, swz);
}

// a quad from four inputs: two triangles, the second with the winding flag
static std::vector<u32> quad() {
    return {mov(O0, 0), setemit(0, 0, 0), EMIT, mov(O0, 1), setemit(1, 0, 0), EMIT, mov(O0, 2), setemit(2, 1, 0), EMIT,
            mov(O0, 3), setemit(0, 1, 1), EMIT, END};
}

static const u8 no_ints[4][4] = {};
static std::string gs(const std::vector<u32> &c, bool turns, u32 *tris, u32 in_regs = 0xf, const u8 (*iu)[4] = no_ints) {
    return shader_glsl_gs(program(c), 0, 0, iu, 0xf, in_regs, turns, tris);  // the output map reads o0.xyzw
}

// A loop of three turns (i0: count 2, start 0, step 1) that emits a quad a turn, its first corner where the turn before left off (t1, set
// from t2 at the end of each turn, c0 before the first), like Luigi's Mansion 2's rain trail.
static const u8 three_turns[4][4] = {{2, 0, 1, 0}};
static std::vector<u32> trail() {
    return {mov(T1, C0),
            loop(15),  // the body: the next 14 instructions
            mov(T2, C1),
            mov(O0, T1), setemit(0, 0, 0), EMIT,
            mov(O0, T2), setemit(1, 0, 0), EMIT,
            mov(O0, C2), setemit(2, 1, 0), EMIT,
            mov(O0, C3), setemit(0, 1, 1), EMIT,
            mov(T1, T2),
            END};
}

int main() {
    u32 tris = 0;
    const std::string straight = gs(quad(), false, &tris);
    expect(!straight.empty() && tris == 2, "a quad: two triangles");
    const std::string turned = gs(quad(), true, &tris);
    // the second triangle's first corner: slot 0 as emitted, slot 1 where the topology turns it round on the winding flag (pa_submit)
    const size_t second = straight.rfind("if (gtri == wantT)"), second_t = turned.rfind("if (gtri == wantT)");
    expect(second != std::string::npos && straight.find("if (wantJ == 0) { co[0] = s0_0; }", second) != std::string::npos, "the second triangle as emitted");
    expect(second_t != std::string::npos && turned.find("if (wantJ == 0) { co[0] = s1_0; }", second_t) != std::string::npos, "the second triangle turned round");
    expect(turned.find("go1[0]") != std::string::npos, "every other emit writes the second output bank");

    // what an earlier run left, read: each stays with the CPU
    std::vector<u32> c = quad();
    c.erase(c.begin() + 1);  // the first emit before any SETEMIT
    expect(gs(c, false, &tris).empty(), "an emit before SETEMIT");
    c = quad();
    c[0] = mov(O0, 0, 1);  // the first vertex's output written in x alone
    expect(gs(c, false, &tris).empty(), "an output written in part");
    c = quad();
    c[0] = add(O0, 0, T0);  // a temporary read before this run wrote it
    expect(gs(c, false, &tris).empty(), "a temporary from the run before");
    expect(gs(quad(), false, &tris, 0x7).empty(), "an input register no attribute gave");
    c = {mov(O0, 0), setemit(2, 1, 0), EMIT, END};  // a triangle of slots this run didn't fill
    expect(gs(c, false, &tris).empty(), "slots from the run before");
    c = {loop(4), mov(O0, 0), setemit(0, 0, 0), EMIT, mov(O0, 0), END};  // an emit in a loop: one place in the text, several emits
    expect(gs(c, false, &tris).empty(), "an emit in a loop that makes no triangle");

    // a loop that emits: each vertex runs the turn holding its triangle, after the part of the turn before that sets t1
    const std::string looped = gs(trail(), false, &tris, 0xf, three_turns);
    expect(!looped.empty() && tris == 6, "a loop of three turns, two triangles each");
    const size_t skip = looped.find("int tt0 = int((float(wantT - gtri) + 0.5) / 2.0);"), part = looped.find("if (tt0 > 0) {"),
                 end_part = looped.find("    }\n    gtri += int(float(tt0) * 2.0);");
    const std::string set_t1 = part < end_part && end_part != std::string::npos ? looped.substr(part, end_part - part) : "";
    expect(skip != std::string::npos && looped.find("if (tt0 > 2) tt0 = 2;") != std::string::npos && !set_t1.empty(), "the turn a vertex wants");
    expect(set_t1.find("gt2 = gf[1];") != std::string::npos && set_t1.find("gt1 = gt2;") != std::string::npos && set_t1.find("go0") == std::string::npos,
           "the turn before only sets what the next carries over");
    expect(looped.find("if (!got) { int tt0") != std::string::npos && looped.find("got = true;\n        return;") == std::string::npos,
           "a vertex with its triangle already skips the loop, and no triangle returns early");
    expect(looped.find("    gtri += int(float(tt0) * 2.0);\n    gaL = gInt[0].y + int(float(tt0) * float(gInt[0].z));") != std::string::npos,
           "the turn runs with its own aL");
    c = trail();
    c[15] = add(T1, T1, T2);  // t1 adds up over the turns: a turn's corner depends on every turn before
    expect(gs(c, false, &tris, 0xf, three_turns).empty(), "what a turn carries over depends on the turn before's");
    c = trail();
    c[0] = mov(T3, C0);  // t1 not set before the loop: the first turn's first corner would be what the run before left
    expect(gs(c, false, &tris, 0xf, three_turns).empty(), "what the first turn carries in, from the run before");
    c = trail();
    c.erase(c.begin() + 12, c.begin() + 15);  // three emits a turn: the banks don't come round
    c[1] = loop(12);
    expect(gs(c, false, &tris, 0xf, three_turns).empty(), "an odd number of emits a turn");
    // two corners emitted before the loop, and each turn's triangles made of them: right for the first turn, but a later turn's would be the
    // turn before's corners
    c = {mov(O0, C0), setemit(0, 0, 0), EMIT, mov(O0, C1), setemit(1, 0, 0), EMIT, loop(13), mov(O0, C2), setemit(2, 1, 0), EMIT,
         mov(O0, C3), setemit(0, 1, 0), EMIT, mov(T0, C0), END};
    expect(gs(c, false, &tris, 0xf, three_turns).empty(), "a triangle of corners from the turn before");
    c = trail();
    c.erase(c.begin() + 4);                 // the body's first SETEMIT
    c.insert(c.begin(), setemit(0, 0, 0));  // set before the loop instead: right for the first turn, not for those after
    expect(gs(c, false, &tris, 0xf, three_turns).empty(), "a turn's first emit named by the turn before");
    auto part_of = [](const std::string &glsl) {
        const size_t from = glsl.find("if (tt0 > 0) {"), to = glsl.find("    }\n    gtri += int(float(tt0) * 2.0);");
        return from < to && to != std::string::npos ? glsl.substr(from, to - from) : std::string();
    };
    // what a turn carries over in the second output bank: o0 there, read by the turn's second emit before the turn writes it (c4, at its end)
    c = {mov(O0, C0), setemit(0, 0, 0), EMIT, mov(O0, C0), setemit(1, 0, 0), EMIT,
         loop(18), mov(O0, C2), setemit(0, 0, 0), EMIT, setemit(1, 0, 0), EMIT, mov(O0, C3), setemit(2, 1, 0), EMIT, mov(O0, C4), setemit(0, 1, 1), EMIT,
         mov(T0, C0), END};
    const std::string banked = gs(c, false, &tris, 0xf, three_turns);
    expect(!banked.empty() && tris == 6 && part_of(banked).find("go1[0] = gf[4];") != std::string::npos, "the part writes an output in its own bank");
    // what a turn carries over read relative to aL: the turn before's aL
    c = trail();
    c[15] = mov(T1, C1) | 3u << 19;  // t1 = c1[aL]
    const std::string relative = part_of(gs(c, false, &tris, 0xf, three_turns));
    const size_t al = relative.find("gaL = gInt[0].y + int(float(tt0 - 1) * float(gInt[0].z));"), t1 = relative.find("gt1 = gs_rel(1, gaL);");
    expect(al != std::string::npos && t1 != std::string::npos && al < t1, "the part reads with the turn before's aL");
    c = quad();
    c.insert(c.begin(), mov(T0, 0));  // a temporary written first is fine
    c[1] = add(O0, 0, T0);
    expect(!gs(c, false, &tris).empty(), "a temporary this run wrote");

    // the whole shader and its key
    // The vertex shader writes o1, o3, o4 and o6 (its output mask), which the geometry shader takes in that order as v3, v2, v1 and v0 (its
    // permutation). It reads v15, so the corner goes to location 14.
    const u8 iu[4][4] = {};
    ShaderProgram *vs = program({mov(1, 0), mov(3, 1), mov(4, 2), mov(6, 15), END}), *g = program(quad());
    s8 slots[24];
    memset(slots, -1, sizeof slots);
    for (int i = 0; i < 4; i++) slots[i] = (s8)i;  // the position: o0
    u32 inputs = 0;
    const std::string glsl = gen_gsx_glsl(vs, 0, 0, iu, g, 0, 0, iu, 0x5a, 4, 0x0123, slots, true, &inputs, &tris);
    expect(!glsl.empty() && tris == 2 && inputs == 0x8007, "the vertex and geometry shaders as one");
    expect(glsl.find("layout(location = 14) in vec4 aCorner;") != std::string::npos && glsl.find("wantJ = int(aCorner.y + 0.5);") != std::string::npos &&
               glsl.find("gl_VertexID") == std::string::npos,
           "the triangle and corner an attribute, at a location no input takes");
    expect(glsl.find("gv3 = o[1];") != std::string::npos && glsl.find("gv2 = o[3];") != std::string::npos && glsl.find("gv1 = o[4];") != std::string::npos &&
               glsl.find("gv0 = o[6];") != std::string::npos,
           "the vertex shader's outputs are the geometry shader's inputs, by output mask and permutation");
    expect(glsl.find("ivec4 uInt[4]; vec4 gf[96]; ivec4 gInt[4]; };") != std::string::npos, "the uniform block in gx2_draw's order");
    bool b[16] = {};
    expect(gsx_variant_key(vs, 0, b, g, 0, b, iu, 0x5a, 4, 0x0123, slots, true) != gsx_variant_key(vs, 0, b, g, 0, b, iu, 0x5a, 4, 0x0123, slots, false),
           "the turn rule is in the key");
    auto key = [&](const ShaderProgram *p, const u8 (*gi)[4]) { return gsx_variant_key(vs, 0, b, p, 0, b, gi, 0x5a, 4, 0x0123, slots, false); };
    ShaderProgram *looping = program(trail()), *counting = program({loop(1), add(T0, T0, C0), mov(O0, T0), setemit(0, 0, 0), EMIT, END});
    const u8 count[4][4] = {{3, 0, 1, 0}}, start[4][4] = {{2, 1, 1, 0}}, step[4][4] = {{2, 0, 2, 0}};
    expect(key(looping, count) != key(looping, three_turns) && key(looping, start) != key(looping, three_turns) &&
               key(looping, step) != key(looping, three_turns),
           "the count, first aL and step of a loop that emits are each in the key");
    expect(key(g, three_turns) == key(g, iu) && key(counting, three_turns) == key(counting, iu),
           "nothing of the ints of a program without a loop, or with one that doesn't emit");

    // Fixed-primitive mode (Luigi's Mansion 2's rain): a run takes a triangle's three vertices, the vertex shader's two outputs each, in
    // c10 to c15, and emits one triangle of their first outputs. The vertex shader reads v0 and v1.
    // It reads c16 too, past the primitive: that stays the uniform block's.
    const u32 C10 = 0x20 + 10, C12 = 0x20 + 12, C14 = 0x20 + 14, C16 = 0x20 + 16;
    ShaderProgram *vs2 = program({mov(0, 0), mov(1, 1), END});
    const std::vector<u32> tri = {mov(T0, C16),          mov(O0, C10), setemit(0, 0, 0), EMIT, mov(O0, C12), setemit(1, 0, 0), EMIT,
                                  mov(O0, C14), setemit(2, 1, 0), EMIT, END};
    ShaderProgram *g2 = program(tri);
    const u32 fixed = kGsxPrim | 3 | 10 << 8 | 2 << 16;
    const std::string glsl2 = gen_gsx_glsl(vs2, 0, 0, iu, g2, 0, 0, iu, 0x3, fixed, 0, slots, false, &inputs, &tris);
    expect(!glsl2.empty() && tris == 1 && inputs == 0x3, "a fixed-primitive geometry shader: one triangle");
    expect(glsl2.find("layout(location = 0) in vec4 v0;") != std::string::npos && glsl2.find("layout(location = 3) in vec4 w1;") != std::string::npos &&
               glsl2.find("layout(location = 5) in vec4 x1;") != std::string::npos && glsl2.find("layout(location = 6) in vec4 aCorner;") != std::string::npos,
           "each vertex's inputs by its place in the primitive, then the corner");
    expect(glsl2.find("    cv0 = w0;\n    cv1 = w1;\n    vs_main();\n    gp[2] = o[0];\n    gp[3] = o[1];\n") != std::string::npos,
           "the vertex shader runs for the second vertex, its outputs to that vertex's uniforms");
    const size_t vs_at = glsl2.find("void vs_main() {"), gs_at = glsl2.find("void gs_main() {"), main_at = glsl2.find("void main() {");
    const std::string vs_body = vs_at < gs_at && gs_at != std::string::npos ? glsl2.substr(vs_at, gs_at - vs_at) : "";
    const std::string gs_body = gs_at < main_at && main_at != std::string::npos ? glsl2.substr(gs_at, main_at - gs_at) : "";
    // each emit's output from its own vertex's first output (the banks alternate), c16 still the uniform block's
    expect(gs_body.find("go0[0] = gp[0];") != std::string::npos && gs_body.find("go1[0] = gp[2];") != std::string::npos &&
               gs_body.find("go0[0] = gp[4];") != std::string::npos && gs_body.find("gf[16]") != std::string::npos && gs_body.find("gf[10]") == std::string::npos,
           "the geometry shader reads the primitive from the vertex shader, and the rest from the uniform block");
    expect(vs_body.find("cv0") != std::string::npos && vs_body.find("cv1") != std::string::npos && vs_body.find(" v0") == std::string::npos &&
               vs_body.find("(v0") == std::string::npos && vs_body.find(" v1") == std::string::npos,
           "the vertex shader reads the variables main sets, not an input");
    expect(glsl2.find("if (u >= 10 && u < 16) return gp[u - 10];") != std::string::npos, "relative reads find the primitive too");
    // the rain trail's shape in fixed-primitive mode: a loop's read relative to aL that no aL of the loop takes into the primitive (c20 to c22)
    // reads the uniform block alone; one that can (c8 to c10) still finds the primitive
    auto prim_trail = [&](u32 base) {
        return std::vector<u32>{mov(T1, C10),
                                loop(15),
                                mov(T2, C0 + base) | 3u << 19,
                                mov(O0, T1), setemit(0, 0, 0), EMIT,
                                mov(O0, T2), setemit(1, 0, 0), EMIT,
                                mov(O0, C12), setemit(2, 1, 0), EMIT,
                                mov(O0, C14), setemit(0, 1, 1), EMIT,
                                mov(T1, T2),
                                END};
    };
    const std::string past = gen_gsx_glsl(vs2, 0, 0, iu, program(prim_trail(20)), 0, 0, three_turns, 0x3, fixed, 0, slots, false, &inputs, &tris);
    const std::string into = gen_gsx_glsl(vs2, 0, 0, iu, program(prim_trail(8)), 0, 0, three_turns, 0x3, fixed, 0, slots, false, &inputs, &tris);
    expect(past.find("gs_ru(20, gaL)") != std::string::npos && past.find("gs_rel(20, gaL)") == std::string::npos &&
               into.find("gs_rel(8, gaL)") != std::string::npos && into.find("gs_ru(8, gaL)") == std::string::npos,
           "a loop's read past the primitive leaves it out, one that can reach it doesn't");
    expect(past.find("vec4 gs_ru(int base") != std::string::npos && into.find("vec4 gs_ru(") == std::string::npos, "gs_ru only where it's read");
    // c14 with aLs of 30, 80 and 130: the last is out of range, read as 0 (gs_rel); c90 with 50, 60 and 70: the first wraps round to c12
    const u8 clamped[4][4] = {{2, 30, 50, 0}}, wrapped[4][4] = {{2, 50, 10, 0}};
    const std::string at_clamp = gen_gsx_glsl(vs2, 0, 0, iu, program(prim_trail(14)), 0, 0, clamped, 0x3, fixed, 0, slots, false, &inputs, &tris);
    const std::string at_wrap = gen_gsx_glsl(vs2, 0, 0, iu, program(prim_trail(90)), 0, 0, wrapped, 0x3, fixed, 0, slots, false, &inputs, &tris);
    expect(at_clamp.find("gs_rel(14, gaL)") != std::string::npos && at_wrap.find("gs_rel(90, gaL)") != std::string::npos &&
               at_clamp.find("gs_ru(") == std::string::npos && at_wrap.find("gs_ru(") == std::string::npos,
           "an aL out of range, or one that wraps round, still finds the primitive");
    const u32 moved = kGsxPrim | 3 | 11 << 8 | 2 << 16;
    expect(gsx_variant_key(vs2, 0, b, g2, 0, b, iu, 0x3, fixed, 0, slots, false) != gsx_variant_key(vs2, 0, b, g2, 0, b, iu, 0x3, moved, 0, slots, false),
           "where the primitive starts is in the key");
    expect(gsx_variant_key(vs2, 0, b, g2, 0, b, iu, 0x3, fixed, 0, slots, false) == gsx_variant_key(vs2, 0, b, g2, 0, b, iu, 0x3, fixed, 0x0123, slots, false),
           "the input permutation a fixed-primitive run doesn't read isn't in the key");
    std::vector<u32> reads_input = tri;
    reads_input[1] = mov(O0, 0);  // its first vertex's output from an input register
    expect(gen_gsx_glsl(vs2, 0, 0, iu, program(reads_input), 0, 0, iu, 0x3, fixed, 0, slots, false, &inputs, &tris).empty(),
           "a fixed-primitive run reads no input registers: one that does stays with the CPU");
    expect(gen_gsx_glsl(vs2, 0, 0, iu, g2, 0, 0, iu, 0x3, kGsxPrim | 3 | 10 << 8 | 3 << 16, 0, slots, false, &inputs, &tris).empty(),
           "more outputs a vertex than the vertex shader has");
    // a program whose other code (past this entry's END) reads v5: only v0 and v1 take locations, three vertices' worth
    ShaderProgram *vs3 = program({mov(0, 0), mov(1, 1), END, mov(0, 5), END});
    const std::string glsl3 = gen_gsx_glsl(vs3, 0, 0, iu, g2, 0, 0, iu, 0x3, fixed, 0, slots, false, &inputs, &tris);
    expect(!glsl3.empty() && inputs == 0x3 && glsl3.find("in vec4 v5;") == std::string::npos && glsl3.find("in vec4 x5;") == std::string::npos &&
               glsl3.find("layout(location = 6) in vec4 aCorner;") != std::string::npos,
           "only the inputs the entry's code reads");
    if (failures) return 1;
    printf("geometry shaders on the GPU ok\n");
    return 0;
}
