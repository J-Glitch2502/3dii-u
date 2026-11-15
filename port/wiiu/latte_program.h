// A program from the console's own shader compiler, made fit for a real GPU: shader_compile.cpp runs every one through fit_program.
// CafeGLSL 1.0, which the console runs, writes two things that 0.2 never does. 0.2 is the PC's: it compiles every shader a build ships, and
// consoles have drawn with its programs since the first port. An emulator takes both, so Cemu showed nothing, while testers' consoles drew
// scrambled what the vertex shaders compiled there shaded, and right what the build had shipped (3D Land, New Leaf, Omega Ruby, 2026-11-12).
//   - A uniform read by a computed index (a bone's matrix) is a fetch from the uniform block's buffer, and 1.0 sets the fetch's
//     USE_CONST_FIELDS: the format of the data then comes from the buffer and not from the instruction, which says four floats. GX2 gives
//     a uniform block's buffer no format. With the bit cleared the instruction is field for field what 0.2 writes.
//   - An instruction with three operands always stores its result, and where only the next group reads it (through PV) 1.0 stores it in
//     register 123. A vertex program has 48 registers and a pixel program 200 (GX2SetShaderMode). Here the store goes to the first register
//     the program doesn't use, and the program says it uses one more.
//   - Two instructions of a group may store to the same component of register 123: one in a vector unit and one in the scalar unit, each
//     read by the next group through its own latch (PV, PS). A GPU keeps the two apart whatever becomes of the register. Cemu reads both
//     back from the register and gets the later result twice: Pokemon Ultra Sun's road drew in one flat colour, its light table's entry
//     taken for the alpha test's value (2026-11-13). The second of such a pair goes to a register of its own, the one after the first's.
//     This one is for Cemu: a console needs it no more than it minds it.
// Which of the first two a console minds isn't known. Every vertex program 1.0 makes for a game has the second and all that index a uniform the
// first; pixel programs, which consoles have drawn right from 1.0, almost never have the first, and their 123 is inside their 200. They are
// the only two differences that comparing both compilers' programs for all 632 of 3D Land's shaders found among the rules a GPU holds a
// program to and an emulator doesn't (tools/gsh_rules.py checks those). No console has run this yet.
// Nothing of the console's here, so a PC can run it: tools/test_latte_program.cpp.
#pragma once
#include <cstdint>
#include <cstring>

namespace gpu {
constexpr uint32_t kVertexRegisters = 48, kPixelRegisters = 200;  // what GX2SetShaderMode gives each kind of program

namespace latte {
// a program is 32-bit words, least significant byte first, whatever the CPU reading it
inline uint32_t word(const uint8_t *p, uint32_t i) { return p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24; }
inline void set_word(uint8_t *p, uint32_t i, uint32_t v) {
    for (uint32_t b = 0; b < 4; b++) p[4 * i + b] = (uint8_t)(v >> (8 * b));
}

constexpr uint32_t kThrowaway = 123, kLiteral = 253;

// One pass over a program: its control flow list from the start, and each clause the list names. Returns whether it is laid out as a program is
// (every clause inside it, every instruction group ended, the list ended by its END_OF_PROGRAM before the first clause begins). `top` gets
// the highest register it reads or writes, the throwaway one aside, and `spares` how many registers the stores to that one need: one, or
// two if two of a group store to the same component of it. With `to`, those stores go to register `to`, the second of such a pair to the
// one after it, and uniform fetches lose their USE_CONST_FIELDS.
inline bool walk(uint8_t *p, uint32_t words, uint32_t &top, uint32_t &spares, const uint32_t *to) {
    top = spares = 0;
    uint32_t first_clause = words / 2;  // in instructions (two words each), as the control flow list counts
    auto reg = [&](uint32_t r) {
        if (r < kThrowaway && r > top) top = r;
    };
    for (uint32_t pc = 0; pc < first_clause; pc++) {
        const uint32_t w0 = word(p, 2 * pc), w1 = word(p, 2 * pc + 1);
        if (w1 & (1u << 29)) {  // arithmetic: address and count, then instructions two words each, a group's literals after its last one
            uint32_t i = w0 & 0x3FFFFF;
            const uint32_t end = i + ((w1 >> 18) & 0x7F) + 1;
            if (i <= pc || end > words / 2) return false;
            if (i < first_clause) first_clause = i;
            uint32_t literals = 0;  // of the group so far: the highest literal component named, plus one
            uint32_t thrown = 0;    // and the components of the throwaway register stored to
            bool ended = true;
            while (i < end) {
                const uint32_t a0 = word(p, 2 * i), a1 = word(p, 2 * i + 1);
                const bool three = (a1 >> 15) & 7;
                const uint32_t dst = (a1 >> 21) & 0x7F, sources = three ? 3 : 2;
                for (uint32_t s = 0; s < sources; s++) {
                    const uint32_t sel = s == 0 ? a0 & 0x1FF : s == 1 ? (a0 >> 13) & 0x1FF : a1 & 0x1FF;
                    const uint32_t chan = s == 0 ? (a0 >> 10) & 3 : s == 1 ? (a0 >> 23) & 3 : (a1 >> 10) & 3;
                    if (sel == kLiteral && chan + 1 > literals) literals = chan + 1;
                    reg(sel);  // 124 to 127 are a clause's own, and 128 up constants: neither counts
                }
                if (three || (a1 & 0x10)) {  // it stores: two-operand instructions only with their write bit
                    if (dst == kThrowaway) {
                        const uint32_t component = 1u << ((a1 >> 29) & 3), again = (thrown & component) != 0;
                        thrown |= component;
                        if (spares < 1 + again) spares = 1 + again;
                        if (to) set_word(p, 2 * i + 1, (a1 & ~(0x7Fu << 21)) | ((*to + again) << 21));
                    }
                    reg(dst);
                }
                i++;
                ended = a0 >> 31;
                if (ended) {
                    i += (literals + 1) / 2;
                    literals = thrown = 0;
                }
            }
            if (i != end || !ended) return false;
            continue;
        }
        const uint32_t kind = (w1 >> 23) & 0x7F;
        if (kind == 39 || kind == 40) {  // exports: a register, and with a burst the ones after it
            reg(((w0 >> 15) & 0x7F) + ((w1 >> 17) & 15));
        } else if (kind >= 1 && kind <= 3) {  // fetches: four words each
            const uint32_t count = (((w1 >> 10) & 7) | ((w1 >> 19) & 1) << 3) + 1;
            if (w0 <= pc || w0 + 2 * count > words / 2) return false;
            if (w0 < first_clause) first_clause = w0;
            for (uint32_t k = 0; k < count; k++) {
                const uint32_t at = 2 * (w0 + 2 * k), f0 = word(p, at), f1 = word(p, at + 1);
                if (f0 & 0x1F) continue;  // a texture sample, whose words are laid out differently
                reg((f0 >> 16) & 0x7F);
                reg(f1 & 0x7F);
                if (to && ((f0 >> 8) & 0xFF) >= 0x80) set_word(p, at + 1, f1 & ~(1u << 21));  // a uniform block's buffer: 128 and up
            }
        }
        if (w1 & (1u << 21)) return true;  // END_OF_PROGRAM
    }
    return false;
}
}  // namespace latte

// `registers` is how many the program says it uses (the low byte of its sq_pgm_resources), and comes back higher by the one or two its
// throwaway stores took. False, with the program as it was, if it isn't laid out as expected or hasn't those registers left under `limit`.
inline bool fit_program(void *program, uint32_t bytes, uint32_t &registers, uint32_t limit) {
    uint8_t *const p = (uint8_t *)program;
    uint32_t top, spares;
    if (!latte::walk(p, bytes / 4, top, spares, nullptr)) return false;
    const uint32_t spare = registers > top + 1 ? registers : top + 1;
    if (spares && spare + spares > limit) return false;
    latte::walk(p, bytes / 4, top, spares, &spare);
    if (spares) registers = spare + spares;
    return true;
}

// A vertex program's bursts of exports written out as single exports: for runs in Cemu alone. Cemu up to 2.6 hands the pixel shader only the
// first register of a burst (its own fix, "Latte: Support burst for vertex exports" of 2026-10-02, is in no release yet), and the console's
// compiler writes bursts where the PC's never does (none in the 11,800 vertex programs of fourteen packs). So in Cemu what a shader
// compiled at play time drew lost its texture coordinates or its colour: Pokemon Ultra Sun's tall grass was gone, its road and its shadows
// a layer short (2026-11-13). A console takes bursts as they are.
// Returns the program's length in words without bursts, or 0 if it has none, and writes it to `out` if given. Its list is longer by an
// instruction for each register past a burst's first, and the clauses lie that much further on, made even: a fetch clause has to begin at
// an even place. For a program fit_program has passed.
inline uint32_t unburst(const uint8_t *p, uint32_t words, uint8_t *out) {
    const auto burst_of = [&](uint32_t i) {  // how many registers past its first an instruction of the list exports
        const uint32_t w1 = latte::word(p, 2 * i + 1), kind = (w1 >> 23) & 0x7F;
        return !(w1 & (1u << 29)) && (kind == 39 || kind == 40) ? (w1 >> 17) & 15 : 0u;
    };
    uint32_t list = 0, extra = 0;
    for (bool last = false; !last; list++) {
        const uint32_t w1 = latte::word(p, 2 * list + 1);
        extra += burst_of(list);
        last = !(w1 & (1u << 29)) && (w1 & (1u << 21));  // END_OF_PROGRAM
    }
    if (!extra) return 0;
    const uint32_t shift = extra + (extra & 1);  // how much further on the clauses lie: an odd count leaves an empty place after the list
    if (!out) return words + 2 * shift;
    const auto lies_at = [&](uint32_t i) {  // where an instruction of the list comes to lie
        uint32_t at = i;
        for (uint32_t k = 0; k < i && k < list; k++) at += burst_of(k);
        return at;
    };
    uint32_t o = 0;
    const auto put = [&](uint32_t w0, uint32_t w1) {
        latte::set_word(out, 2 * o, w0);
        latte::set_word(out, 2 * o + 1, w1);
        o++;
    };
    for (uint32_t i = 0; i < list; i++) {
        const uint32_t w0 = latte::word(p, 2 * i), w1 = latte::word(p, 2 * i + 1), kind = (w1 >> 23) & 0x7F;
        if (w1 & (1u << 29)) put((w0 & ~0x3FFFFFu) | ((w0 & 0x3FFFFF) + shift), w1);  // an arithmetic clause
        else if (kind == 39 || kind == 40) {
            const uint32_t burst = burst_of(i);
            for (uint32_t k = 0; k <= burst; k++)  // the next place from the next register; all but the last a plain export that ends nothing
                put(w0 + k * (1u | 1u << 15), k < burst ? (w1 & ~(0xFu << 17 | 1u << 21 | 0x7Fu << 23)) | 39u << 23 : w1 & ~(0xFu << 17));
        } else if (kind >= 1 && kind <= 3) put(w0 + shift, w1);  // a fetch clause
        else if (kind >= 4 && kind <= 18) put(lies_at(w0), w1);  // loops, jumps and calls name an instruction of the list
        else put(w0, w1);
    }
    if (shift > extra) put(0, 0);
    memcpy(out + 8 * (list + shift), p + 8 * list, 4 * words - 8 * list);  // the clauses, as they are
    return words + 2 * shift;
}
}  // namespace gpu
