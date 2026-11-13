// Checks port/wiiu/latte_program.h on a PC, against small programs put together here word by word: the two things the console's compiler
// writes are put right, and nothing else in a program changes, least of all a literal or a texture sample whose bits happen to look like
// them. `python tools/selftest.py` builds and runs it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "latte_program.h"

using namespace gpu;
typedef uint32_t u32;

static int failures;

// ---- the words of a program (the GPU's own layout)
static void cf(std::vector<u32> &w, u32 kind, u32 addr, u32 count = 1, bool end = false) {
    w.push_back(addr);
    w.push_back(kind << 23 | (count - 1) << 10 | (u32)end << 21 | 1u << 31);
}
static void cf_alu(std::vector<u32> &w, u32 addr, u32 count) {
    w.push_back(addr);
    w.push_back(8u << 26 | (count - 1) << 18 | 1u << 31);
}
static void cf_export(std::vector<u32> &w, u32 type, u32 reg, u32 burst, bool end) {
    w.push_back(type << 13 | reg << 15 | 3u << 30);
    w.push_back(0x688 | burst << 17 | (u32)end << 21 | 40u << 23 | 1u << 31);
}
struct Src {
    u32 sel, chan;
};
static void op2(std::vector<u32> &w, u32 op, u32 dst, u32 chan, bool write, Src a, Src b, bool last) {
    w.push_back(a.sel | a.chan << 10 | b.sel << 13 | b.chan << 23 | (u32)last << 31);
    w.push_back((u32)write << 4 | op << 7 | dst << 21 | chan << 29);
}
static void op3(std::vector<u32> &w, u32 op, u32 dst, u32 chan, Src a, Src b, Src c, bool last) {
    w.push_back(a.sel | a.chan << 10 | b.sel << 13 | b.chan << 23 | (u32)last << 31);
    w.push_back(c.sel | c.chan << 10 | op << 13 | dst << 21 | chan << 29);
}
// a fetch of four floats from a buffer, as both compilers write a uniform read by a computed index
static void fetch(std::vector<u32> &w, u32 buffer, u32 src, u32 dst, bool const_fields) {
    w.push_back(2u << 5 | buffer << 8 | src << 16 | 15u << 26);
    w.push_back(dst | 0u << 9 | 1u << 12 | 2u << 15 | 3u << 18 | (u32)const_fields << 21 | 35u << 22 | 2u << 28 | 1u << 30);
    w.push_back(1u << 19);
    w.push_back(0);
}

static std::vector<uint8_t> bytes_of(const std::vector<u32> &w) {
    std::vector<uint8_t> b(w.size() * 4);
    for (u32 i = 0; i < w.size(); i++) latte::set_word(b.data(), i, w[i]);
    return b;
}

// `program` through fit_program with `registers` declared: it has to answer `fits`, say `want_registers`, and leave exactly `want`
static void check(const char *what, const std::vector<u32> &program, u32 registers, u32 limit, bool fits, u32 want_registers, const std::vector<u32> &want) {
    std::vector<uint8_t> b = bytes_of(program);
    const bool got = fit_program(b.data(), (u32)b.size(), registers, limit);
    if (got != fits || registers != want_registers || b != bytes_of(want)) {
        failures++;
        printf("FAILED: %s (%s, %u registers)\n", what, got ? "fits" : "refused", registers);
        for (u32 i = 0; i < program.size() && got == fits; i++)
            if (latte::word(b.data(), i) != want[i]) printf("   word %u is %08x, wanted %08x\n", i, latte::word(b.data(), i), want[i]);
    }
}

enum { TEX = 1, CALL_FS = 19, MOV = 25, ADD = 0, MULADD = 16, PARAM = 2, POS = 1, PV = 254, T0 = 124, K0 = 128 };

// What 1.0 makes of a vertex shader, in small: two throwaway stores, a store-less instruction and a literal that both name 123 too, a
// uniform fetch with USE_CONST_FIELDS and a texture sample with the same bit set (part of its LOD bias). `fixed` says how it should leave.
static std::vector<u32> from_new_compiler(bool fixed, u32 spare) {
    const u32 away = fixed ? spare : 123;
    std::vector<u32> w;
    cf(w, CALL_FS, 0);                     // 0
    cf_alu(w, 6, 4);                       // 1
    cf(w, TEX, 10);                        // 2: the uniform fetch
    cf(w, TEX, 12);                        // 3: the sample
    cf_alu(w, 14, 1);                      // 4
    cf_export(w, POS, 1, 0, true);         // 5
    op3(w, MULADD, away, 0, {1, 0}, {253, 1}, {2, 0}, false);  // 6: read by the next group through PV alone
    op2(w, MOV, 123, 1, false, {K0 + 3, 2}, {0, 0}, true);     // 7: stores nothing, so its register field means nothing
    w.push_back(0x0F600000);               // 8: the group's literals, the first looking like a store to 123
    w.push_back(0x0F608000);
    op2(w, ADD, away, 2, true, {PV, 0}, {T0, 1}, true);        // 9
    fetch(w, 0x80, 5, 6, !fixed);          // 10, 11
    // 12, 13: a sample, opcode 16, into register 3 with a LOD bias whose lowest bit is bit 21. Its resource number is one no sample has, so
    // that its opcode alone keeps it from being taken for a uniform fetch
    w.push_back(0x10 | 0x85u << 8 | 2u << 16);
    w.push_back(3 | 1u << 21);
    w.push_back(0);
    w.push_back(0);
    op2(w, MOV, 1, 0, true, {6, 0}, {0, 0}, true);             // 14
    return w;
}

int main() {
    check("the console compiler's two habits", from_new_compiler(false, 0), 7, kVertexRegisters, true, 8, from_new_compiler(true, 7));
    check("a program that says it uses fewer registers than it does", from_new_compiler(false, 0), 3, kVertexRegisters, true, 8, from_new_compiler(true, 7));
    check("a program that says it uses more", from_new_compiler(false, 0), 20, kVertexRegisters, true, 21, from_new_compiler(true, 20));
    check("no register left for the throwaway store", from_new_compiler(false, 0), kVertexRegisters, kVertexRegisters, false, kVertexRegisters, from_new_compiler(false, 0));
    check("the same program with a pixel program's registers", from_new_compiler(false, 0), kVertexRegisters, kPixelRegisters, true, kVertexRegisters + 1,
          from_new_compiler(true, kVertexRegisters));
    check("a program already right", from_new_compiler(true, 7), 8, kVertexRegisters, true, 8, from_new_compiler(true, 7));

    // what the PC's compiler writes: a burst of exports, the list ended by an instruction of its own
    std::vector<u32> old;
    cf(old, CALL_FS, 0);
    cf_alu(old, 4, 2);
    cf_export(old, PARAM, 2, 3, false);  // registers 2 to 5
    cf(old, 0, 0, 1, true);
    op3(old, MULADD, T0, 0, {1, 0}, {K0, 0}, {K0 + 1, 0}, false);
    op2(old, MOV, 2, 1, true, {T0, 0}, {0, 0}, true);
    check("the PC compiler's kind of program", old, 6, kVertexRegisters, true, 6, old);

    std::vector<u32> open = from_new_compiler(false, 0);
    open[2 * 14] &= ~(1u << 31);  // the last group never ends
    check("a group that runs off its clause", open, 7, kVertexRegisters, false, 7, open);
    std::vector<u32> endless = from_new_compiler(false, 0);
    endless[2 * 5 + 1] &= ~(1u << 21);  // no END_OF_PROGRAM: the list runs into the first clause
    check("a list without an end", endless, 7, kVertexRegisters, false, 7, endless);
    std::vector<u32> outside = from_new_compiler(false, 0);
    outside[2 * 4] = 40;  // a clause past the end of the program
    check("a clause outside the program", outside, 7, kVertexRegisters, false, 7, outside);

    printf(failures ? "compiled programs: %d failed\n" : "compiled programs: ok\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
