// Checks, on a PC, the vertex shader that reads its bool uniforms as it runs (shadergen.h's kVsBoolsMade): for a small program made up here
// that tests eleven bools, one more than a build makes every set of. `python tools/selftest.py` builds it with the generator's own objects.
#include <cstdio>
#include <cstdlib>
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

// ---- PICA instructions (shader.cpp's decode_one has the layouts)
static u32 mov(u32 out, u32 in) { return 0x13u << 26 | out << 21 | in << 12; }  // o<out> = v<in>, with operand descriptor 0
static u32 flow(u32 op, u32 bid, u32 dst, u32 num) { return op << 26 | bid << 22 | dst << 10 | num; }
enum : u32 { NOP = 0x21u << 26, END = 0x22u << 26, CALLU = 0x26, IFU = 0x27, JMPU = 0x2d };

// Bools 0 to `bools` - 1 each decide something: an if, an if with an else, a call, a jump taken when its bool is clear, then plain ifs.
static std::vector<u32> program(u32 bools) {
    std::vector<u32> c = {
        flow(IFU, 0, 2, 0), mov(0, 0),             //  0: if (b0) o0 = v0
        flow(IFU, 1, 4, 1), mov(1, 1), mov(1, 2),  //  2: if (b1) o1 = v1; else o1 = v2
        flow(CALLU, 2, 27, 1),                     //  5: if (b2) call 27
        flow(JMPU, 3, 8, 1), mov(2, 3), NOP,       //  6: if (!b3) skip the next: o2 = v3
    };
    for (u32 b = 4; b < 11; b++) {                 //  9: if (b4) ... if (b10), two words each
        c.push_back(b < bools ? flow(IFU, b, (u32)c.size() + 2, 0) : NOP);
        c.push_back(mov(3, b & 3));
    }
    c.insert(c.end(), {NOP, END, NOP, NOP, mov(5, 0), NOP});  // 23; the called routine is word 27
    return c;
}

int main() {
    u32 swizzle[512] = {0x36F};  // descriptor 0: all four components, in order
    const u8 iu[4][4] = {};
    s8 slots[24];
    memset(slots, -1, sizeof slots);
    for (int i = 0; i < 4; i++) slots[i] = (s8)i;  // the position is output register 0

    std::vector<u32> eleven = program(11), ten = program(10);
    expect(eleven.size() == 29 && eleven[27] == mov(5, 0), "the made-up program's layout");
    eleven.resize(512);
    ten.resize(512);
    ShaderProgram *p = shader_decode(eleven.data(), 29, swizzle), *q = shader_decode(ten.data(), 29, swizzle);
    expect(vs_open_bools(p, 0) == 0x7FF, "eleven bools are more than a build makes every set of: all eleven are left open");
    expect(vs_open_bools(q, 0) == 0, "ten bools: every set is made, none left open");

    u32 inputs = 0;
    const std::string open = gen_vs_glsl(p, 0, 0, iu, slots, &inputs, 0x7FF);
    expect(!open.empty(), "the program is written with its bools left open");
    for (u32 b = 0; b < 11; b++) expect(open.find("((uInt[0].w >> " + std::to_string(b) + ") & 1) != 0") != std::string::npos, "every open bool is read from the uniform block");
    // the sense of each kind of test: what follows it is what the program does when the test holds
    const size_t if1 = open.find("if (((uInt[0].w >> 1) & 1) != 0) {"), else1 = open.find("} else {", if1), v1 = open.find("v1", if1), v2 = open.find("v2", if1);
    expect(if1 != std::string::npos && v1 < else1 && else1 < v2, "an if on an open bool: its own branch first, the other after the else");
    expect(open.find("if (!(((uInt[0].w >> 3) & 1) != 0)) {") != std::string::npos, "a jump taken when its bool is clear tests for clear");

    // decided the usual way, nothing is read as it runs, and each set of bools gives its own code
    bool none[16] = {}, some[16] = {}, others[16] = {};
    some[1] = some[3] = true;
    others[0] = others[2] = others[10] = true;
    const std::string a = gen_vs_glsl(p, 0, 0, iu, slots, &inputs), b = gen_vs_glsl(p, 0, 1u << 1 | 1u << 3, iu, slots, &inputs);
    expect(!a.empty() && !b.empty() && a != b, "the same program with its bools decided");
    expect(a.find("uInt[0].w") == std::string::npos && b.find("uInt[0].w") == std::string::npos, "a shader with its bools decided reads none of them");
    // (the program's own code: every input is declared above main, read or not)
    const std::string code_a = a.substr(a.find("void main()")), code_b = b.substr(b.find("void main()"));
    expect(code_a.find("v2") != std::string::npos && code_a.find("v1") == std::string::npos && code_b.find("v1") != std::string::npos && code_b.find("v2") == std::string::npos,
           "decided, only the branch a bool selects is there");

    // the open shader has one key whatever the bools are, and no decided shader's key
    const u64 k = vs_variant_key(p, 0, none, slots, 0x7FF);
    expect(k == vs_variant_key(p, 0, some, slots, 0x7FF) && k == vs_variant_key(p, 0, others, slots, 0x7FF), "open bools aren't in the open shader's key");
    expect(k != vs_variant_key(p, 0, none, slots) && k != vs_variant_key(p, 0, some, slots), "the open shader's key is no decided shader's");
    expect(vs_variant_key(p, 0, some, slots) != vs_variant_key(p, 0, others, slots), "decided shaders keep a key for each set");

    printf(failures ? "open bools: %d failed\n" : "open bools: ok\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
