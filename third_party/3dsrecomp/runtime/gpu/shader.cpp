// PICA200 shader unit: program decoder (to a compact pre-resolved form) and interpreter.
#include <unordered_map>

#include "gpu/pica.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace gpu {

#ifdef __clang__
typedef float f4 __attribute__((ext_vector_type(4)));
typedef u32 u4 __attribute__((ext_vector_type(4)));
typedef s32 i4 __attribute__((ext_vector_type(4)));
#else
typedef float f4 __attribute__((vector_size(16)));
typedef u32 u4 __attribute__((vector_size(16)));
typedef s32 i4 __attribute__((vector_size(16)));
#endif

enum Op : u8 {
    OP_ADD, OP_DP3, OP_DP4, OP_DPH, OP_DST, OP_EX2, OP_LG2, OP_LITP, OP_MUL, OP_SGE, OP_SLT,
    OP_FLR, OP_MAX, OP_MIN, OP_RCP, OP_RSQ, OP_MOVA, OP_MOV, OP_CMP, OP_MAD,
    OP_NOP, OP_END, OP_BREAK, OP_BREAKC, OP_CALL, OP_CALLC, OP_CALLU, OP_IFU, OP_IFC, OP_LOOP,
    OP_EMIT, OP_SETEMIT, OP_JMPC, OP_JMPU,
};

struct alignas(16) DInst {
    u8 tbl[3][16];  // byte shuffle tables implementing the source swizzles
    u4 neg[3];      // sign masks implementing source negation
    i4 mask;        // destination write mask
    u8 sel[3][4];
    u8 op, dst, dst_out, aidx;  // aidx: 0 none, 1 a0.x, 2 a0.y, 3 aL
    u8 src[3];
    s8 rel;  // index of the source using relative addressing (-1 none)
    u8 cmpx, cmpy;
    u8 fop, refx, refy, bid, iid;
    u8 vid, prim, wind;
    u16 fdst, fnum;
};

struct ShaderProgram {
    std::vector<DInst> ins;
    bool simd_ok = true;  // can run in the 4-wide SoA interpreter
    u64 hash = 0;
    std::vector<u32> raw_code, raw_swz;  // what it was decoded from (for shader_entry_info)
    struct EntryInfo { u64 id; u32 bmask, lmask, emask; };  // emask: the int uniforms of loops whose bodies EMIT
    std::unordered_map<u32, EntryInfo> entries;
    s8 outline = -1;  // shader_outline_gs: not looked at yet
    u32 outline_color = 0, outline_thick = 0;
};

static void set_swizzle(DInst &d, int k, u32 pat) {
    static const u32 negbit[3] = {4, 13, 22};
    static const u32 selbase[3] = {11, 20, 29};
    bool neg = (pat >> negbit[k]) & 1;
    for (int i = 0; i < 4; i++) {
        u32 s = (pat >> (selbase[k] - 2 * i)) & 3;
        d.sel[k][i] = (u8)s;
        for (int b = 0; b < 4; b++) d.tbl[k][i * 4 + b] = (u8)(s * 4 + b);
        d.neg[k][i] = neg ? 0x80000000u : 0;
    }
}

static u8 src7(u32 v) { return v < 0x10 ? REG_IN + v : v < 0x20 ? REG_TMP + (v - 0x10) : REG_UNI + (v - 0x20); }
static u8 src5(u32 v) { return v < 0x10 ? REG_IN + v : REG_TMP + (v - 0x10); }

static void decode_one(u32 w, const u32 *swz, DInst &d) {
    memset(&d, 0, sizeof(d));
    d.rel = -1;
    const u32 opc = w >> 26;
    auto common = [&](u8 op, bool inverted) {
        d.op = op;
        u32 pat = swz[w & 0x7f];
        set_swizzle(d, 0, pat);
        set_swizzle(d, 1, pat);
        for (int i = 0; i < 4; i++) d.mask[i] = (pat & (8 >> i)) ? -1 : 0;
        u32 dst = bits(w, 21, 5);
        d.dst = dst < 0x10 ? REG_OUT + dst : REG_TMP + (dst - 0x10);
        d.dst_out = dst < 0x10;
        d.aidx = bits(w, 19, 2);
        if (!inverted) {
            d.src[0] = src7(bits(w, 12, 7));
            d.src[1] = src5(bits(w, 7, 5));
            if (d.aidx && d.src[0] >= REG_UNI) d.rel = 0;
        } else {
            d.src[0] = src5(bits(w, 14, 5));
            d.src[1] = src7(bits(w, 7, 7));
            if (d.aidx && d.src[1] >= REG_UNI) d.rel = 1;
        }
        d.cmpx = bits(w, 24, 3);
        d.cmpy = bits(w, 21, 3);
    };
    auto flow = [&](u8 op) {
        d.op = op;
        d.fnum = bits(w, 0, 8);
        d.fdst = bits(w, 10, 12);
        d.fop = bits(w, 22, 2);
        d.refy = bits(w, 24, 1);
        d.refx = bits(w, 25, 1);
        d.bid = bits(w, 22, 4);
        d.iid = bits(w, 22, 2);
    };
    switch (opc) {
    case 0x00: common(OP_ADD, false); break;
    case 0x01: common(OP_DP3, false); break;
    case 0x02: common(OP_DP4, false); break;
    case 0x03: common(OP_DPH, false); break;
    case 0x04: common(OP_DST, false); break;
    case 0x05: common(OP_EX2, false); break;
    case 0x06: common(OP_LG2, false); break;
    case 0x07: common(OP_LITP, false); break;
    case 0x08: common(OP_MUL, false); break;
    case 0x09: common(OP_SGE, false); break;
    case 0x0a: common(OP_SLT, false); break;
    case 0x0b: common(OP_FLR, false); break;
    case 0x0c: common(OP_MAX, false); break;
    case 0x0d: common(OP_MIN, false); break;
    case 0x0e: common(OP_RCP, false); break;
    case 0x0f: common(OP_RSQ, false); break;
    case 0x12: common(OP_MOVA, false); break;
    case 0x13: common(OP_MOV, false); break;
    case 0x18: common(OP_DPH, true); break;
    case 0x19: common(OP_DST, true); break;
    case 0x1a: common(OP_SGE, true); break;
    case 0x1b: common(OP_SLT, true); break;
    case 0x20: d.op = OP_BREAK; break;
    case 0x21: d.op = OP_NOP; break;
    case 0x22: d.op = OP_END; break;
    case 0x23: flow(OP_BREAKC); break;
    case 0x24: flow(OP_CALL); break;
    case 0x25: flow(OP_CALLC); break;
    case 0x26: flow(OP_CALLU); break;
    case 0x27: flow(OP_IFU); break;
    case 0x28: flow(OP_IFC); break;
    case 0x29: flow(OP_LOOP); break;
    case 0x2a: d.op = OP_EMIT; break;
    case 0x2b:
        d.op = OP_SETEMIT;
        d.wind = bits(w, 22, 1);
        d.prim = bits(w, 23, 1);
        d.vid = bits(w, 24, 2);
        break;
    case 0x2c: flow(OP_JMPC); break;
    case 0x2d: flow(OP_JMPU); break;
    case 0x2e:
    case 0x2f: common(OP_CMP, false); break;
    default:
        if (opc >= 0x30) {
            const bool inv = opc < 0x38;  // MADI
            d.op = OP_MAD;
            u32 pat = swz[w & 0x1f];
            set_swizzle(d, 0, pat);
            set_swizzle(d, 1, pat);
            set_swizzle(d, 2, pat);
            for (int i = 0; i < 4; i++) d.mask[i] = (pat & (8 >> i)) ? -1 : 0;
            u32 dst = bits(w, 24, 5);
            d.dst = dst < 0x10 ? REG_OUT + dst : REG_TMP + (dst - 0x10);
            d.dst_out = dst < 0x10;
            d.aidx = bits(w, 22, 2);
            d.src[0] = src5(bits(w, 17, 5));
            if (!inv) {
                d.src[1] = src7(bits(w, 10, 7));
                d.src[2] = src5(bits(w, 5, 5));
                if (d.aidx && d.src[1] >= REG_UNI) d.rel = 1;
            } else {
                d.src[1] = src5(bits(w, 12, 5));
                d.src[2] = src7(bits(w, 5, 7));
                if (d.aidx && d.src[2] >= REG_UNI) d.rel = 2;
            }
        } else {
            d.op = OP_NOP;
        }
        break;
    }
}

ShaderProgram *shader_decode(const u32 *code, u32 code_size, const u32 *swizzle) {
    auto *p = new ShaderProgram();
    p->ins.resize(code_size);
    for (u32 i = 0; i < code_size; i++) {
        decode_one(code[i], swizzle, p->ins[i]);
        const u8 op = p->ins[i].op;
        if (op == OP_DST || op == OP_LITP || op == OP_EMIT || op == OP_SETEMIT) p->simd_ok = false;
    }
    p->raw_code.assign(code, code + code_size);
    p->raw_swz.assign(swizzle, swizzle + 128);
    return p;
}

// What an entry point actually runs: instruction memory keeps the tail of longer programs loaded earlier, so the words it was
// decoded from don't identify a program, only those reachable from the entry do. id doesn't depend on the host's byte order.
void shader_entry_info(const ShaderProgram *pc, u32 entry, u64 *id, u32 *bmask, u32 *lmask) {
    auto *p = const_cast<ShaderProgram *>(pc);
    auto it = p->entries.find(entry);
    if (it == p->entries.end()) {
        const u32 n = (u32)p->ins.size();
        std::vector<char> seen(n, 0);
        std::vector<u32> todo{entry};
        ShaderProgram::EntryInfo e{0xcbf29ce484222325ull, 0, 0, 0};
        while (!todo.empty()) {
            u32 i = todo.back();
            todo.pop_back();
            while (i < n && !seen[i]) {
                seen[i] = 1;
                const DInst &I = p->ins[i];
                if (I.op == OP_END) break;
                switch (I.op) {
                case OP_JMPU: case OP_CALLU: case OP_IFU: e.bmask |= 1u << I.bid; break;
                case OP_LOOP:
                    e.lmask |= 1u << I.iid;
                    for (u32 k = i + 1; k <= I.fdst && k < n; k++)
                        if (p->ins[k].op == OP_EMIT) e.emask |= 1u << I.iid;
                    break;
                default: break;
                }
                if (I.op == OP_JMPU || I.op == OP_JMPC || I.op == OP_CALL || I.op == OP_CALLC || I.op == OP_CALLU) todo.push_back(I.fdst);
                if (I.op == OP_IFU || I.op == OP_IFC) {
                    todo.push_back(I.fdst);
                    todo.push_back(I.fdst + I.fnum);
                }
                i++;
            }
        }
        auto mix = [&](u32 w) {
            for (u32 k = 0; k < 4; k++) e.id = (e.id ^ ((w >> (8 * k)) & 255)) * 0x100000001b3ull;
        };
        mix(entry);
        for (u32 i = 0; i < n; i++) {
            if (!seen[i]) continue;
            mix(i);
            mix(p->raw_code[i]);
            if (p->ins[i].op <= OP_MAD) mix(p->raw_swz[p->raw_code[i] >> 26 >= 0x30 ? p->raw_code[i] & 0x1f : p->raw_code[i] & 0x7f]);
        }
        it = p->entries.emplace(entry, e).first;
    }
    *id = it->second.id;
    *bmask = it->second.bmask;
    *lmask = it->second.lmask;
}

u32 shader_emit_loops(const ShaderProgram *p, u32 entry) {
    u64 id;
    u32 bmask, lmask;
    shader_entry_info(p, entry, &id, &bmask, &lmask);  // made and kept there, from the code entry reaches
    return p->entries.find(entry)->second.emask;
}

void shader_free(const ShaderProgram *p) { delete p; }

// Level-5's outlines walk a triangle strip with adjacency and, at each silhouette edge, emit a quad from the edge out along an offset each
// vertex carries. Nothing else has its emit routine, and the offset comes from the one multiply of an input by a uniform into a temporary.
bool shader_outline_gs(const ShaderProgram *cp, u32 *color_reg, u32 *thick_reg) {
    auto *p = const_cast<ShaderProgram *>(cp);
    if (p->outline < 0) {
        p->outline = 0;
        static const u8 ops[18] = {OP_MOV, OP_ADD, OP_MOV, OP_SETEMIT, OP_EMIT, OP_MOV, OP_ADD, OP_MOV, OP_SETEMIT, OP_EMIT,
                                   OP_MOV, OP_MOV, OP_SETEMIT, OP_EMIT, OP_MOV, OP_MOV, OP_SETEMIT, OP_EMIT};
        static const u8 dsts[18] = {0, 0, 1, 255, 255, 0, 0, 1, 255, 255, 0, 1, 255, 255, 0, 1, 255, 255};
        static const u8 vids[18] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0};
        const auto &v = p->ins;
        for (size_t i = 0; i + 18 <= v.size() && !p->outline; i++) {
            bool match = true;
            for (u32 k = 0; k < 18 && match; k++) {
                const DInst &d = v[i + k];
                match = d.op == ops[k] && (dsts[k] == 255 || d.dst == REG_OUT + dsts[k]) && (d.op != OP_SETEMIT || (d.vid == vids[k] && d.prim == (k >= 12)));
            }
            if (!match || v[i + 2].src[0] < REG_UNI || v[i + 2].src[0] >= REG_OUT || v[i + 7].src[0] != v[i + 2].src[0]) continue;
            for (const DInst &d : v)
                if (d.op == OP_MUL && d.dst >= REG_TMP && d.dst < REG_UNI && d.src[0] >= REG_UNI && d.src[0] < REG_OUT && d.src[1] == REG_IN + 2) {
                    p->outline = 1;
                    p->outline_color = v[i + 2].src[0] - REG_UNI;
                    p->outline_thick = d.src[0] - REG_UNI;
                    break;
                }
        }
    }
    *color_reg = p->outline_color;
    *thick_reg = p->outline_thick;
    return p->outline == 1;
}

// The vertex programs that go with it normalise the normal in view space, then copy its x and y to make the offset. Which temporary it's
// still in when the program ends, or -1.
int shader_outline_normal(const ShaderProgram *p, u32 entry) {
    int reg = -1;
    for (u32 i = entry; i < p->ins.size() && p->ins[i].op != OP_END; i++) {
        const DInst &d = p->ins[i];
        if (d.op > OP_MAD || d.dst < REG_TMP || d.dst >= REG_UNI) continue;
        if (reg == d.dst - REG_TMP) reg = -1;
        if (d.op == OP_MOV && d.src[0] >= REG_TMP && d.src[0] < REG_UNI && d.mask[0] && d.mask[1] && !d.mask[2] && !d.mask[3]) reg = d.src[0] - REG_TMP;
    }
    return reg;
}

const ShaderProgram *shader_get_program(ShaderSetup &s) {
    static std::unordered_map<u64, std::unique_ptr<ShaderProgram>> cache;
    if (s.prog && !s.dirty) return s.prog;
    u64 h = hash_bytes((const u8 *)s.code, s.code_size * 4) * 31 +
            hash_bytes((const u8 *)s.swizzle, s.swizzle_size * 4) + s.code_size;
    auto it = cache.find(h);
    if (it == cache.end()) {
        if (cache.size() > 512) {
            // programs referenced by setups must survive: keep them
            std::unordered_map<u64, std::unique_ptr<ShaderProgram>> keep;
            for (auto &kv : cache)
                if (kv.second.get() == g.vs.prog || kv.second.get() == g.gs.prog)
                    keep.emplace(kv.first, std::move(kv.second));
            cache.swap(keep);
        }
        it = cache.emplace(h, std::unique_ptr<ShaderProgram>(shader_decode(s.code, s.code_size, s.swizzle))).first;
        it->second->hash = h;
    }
    s.prog = it->second.get();
    s.dirty = false;
    return s.prog;
}

void shader_copy_uniforms(ShaderUnit &u, const ShaderSetup &s) {
    memcpy(&u.r[REG_UNI], s.f, sizeof(s.f));
}

void shader_unit_init(ShaderUnit &u, const ShaderSetup &s) {
    memset(&u, 0, sizeof(u));
    shader_copy_uniforms(u, s);
    for (int i = 0; i < 4; i++) u.r[REG_ONE].v[i] = 1.0f;
}

// ---------------------------------------------------------------- interpreter
static inline f4 ld(const V4 &v) { return *(const f4 *)v.v; }

static inline f4 swizzle(f4 a, const DInst &I, int k) {
#if defined(__aarch64__)
    uint8x16_t t = vld1q_u8(I.tbl[k]);
    f4 r = (f4)vqtbl1q_u8((uint8x16_t)a, t);
#else
    f4 r = {a[I.sel[k][0]], a[I.sel[k][1]], a[I.sel[k][2]], a[I.sel[k][3]]};
#endif
    return (f4)((u4)r ^ I.neg[k]);
}

// PICA multiplication: 0 * inf = 0 (instead of NaN)
static inline f4 pmul(f4 a, f4 b) {
    f4 r = a * b;
    i4 bad = (r != r) & (a == a) & (b == b);
    return (f4)((i4)r & ~bad);
}
static inline float pmul1(float a, float b) {
    float r = a * b;
    if (r != r && a == a && b == b) r = 0;
    return r;
}

static inline s32 to_s32(float f) {
    if (!(f == f)) return 0;
    if (f >= 2147483647.0f) return 0x7fffffff;
    if (f <= -2147483648.0f) return (s32)0x80000000;
    return (s32)f;
}

namespace {
struct IfE { u32 else_addr, end_addr; };
struct CallE { u32 end_addr, ret_addr; };
struct LoopE { u32 entry, end; u8 cnt, inc, prev; };

template <class T, u32 N>
inline void push(T (&a)[N], u32 &n, const T &v) {
    if (n == N) {
        memmove(&a[0], &a[1], sizeof(T) * (N - 1));
        n = N - 1;
    }
    a[n++] = v;
}

const V4 ones_v4 = {{1, 1, 1, 1}};
const DInst end_inst = [] {
    DInst d;
    memset(&d, 0, sizeof(d));
    d.op = OP_END;
    return d;
}();
}  // namespace

std::atomic<u64> g_sh_instr{0}, g_sh_runs{0};
void shader_run(const ShaderProgram *p, ShaderUnit &u, const ShaderSetup &s, u32 entry) {
    shader_run(p, u, s.b, s.i, entry);
}

void shader_run(const ShaderProgram *P, ShaderUnit &u, const bool *ub, const u8 (*ui)[4], u32 entry) {
    u32 icount = 0;
    struct Cnt {
        u32 &c;
        ~Cnt() {
            if (g_stats) {
                g_sh_instr += c;
                g_sh_runs++;
            }
        }
    } cnt{icount};
    IfE ifs[8];
    CallE calls[4];
    LoopE loops[4];
    u32 nif = 0, ncall = 0, nloop = 0;
    u32 pc = entry;
    const DInst *ins = P->ins.data();
    const u32 n = (u32)P->ins.size();
    V4 *r = u.r;

    auto cond = [&](const DInst &I) {
        bool x = I.refx == u.cond[0], y = I.refy == u.cond[1];
        switch (I.fop) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
        }
    };
    auto srcreg = [&](const DInst &I, int k) -> const V4 & {
        u32 idx = I.src[k];
        if (I.rel == k) {
            s32 off = u.addr[I.aidx - 1];
            if (off < -128 || off > 127) off = 0;
            u32 uni = (u32)((s32)(idx - REG_UNI) + off) & 0x7f;
            idx = uni >= 96 ? REG_ONE : REG_UNI + uni;
        }
        return r[idx];
    };

    for (u32 budget = 1u << 20; budget; budget--) {
        const DInst &I = pc < n ? ins[pc] : end_inst;
        icount++;
        const u32 old = pc;
        bool brk = false;
        switch (I.op) {
#define SRC(k) swizzle(ld(srcreg(I, k)), I, k)
#define WRITE(expr)                                                     \
    do {                                                                \
        const f4 res_ = (expr);                                         \
        f4 &d_ = *(f4 *)r[I.dst + (I.dst_out ? u.out_bank : 0)].v;      \
        d_ = (f4)(((i4)res_ & I.mask) | ((i4)d_ & ~I.mask));            \
    } while (0)
#define SPLAT(v) ((f4){(v), (v), (v), (v)})
        case OP_ADD: WRITE(SRC(0) + SRC(1)); break;
        case OP_MUL: WRITE(pmul(SRC(0), SRC(1))); break;
        case OP_MAD: {
            const f4 a = SRC(0), b = SRC(1), c = SRC(2);
            WRITE(pmul(a, b) + c);
            break;
        }
        case OP_MOV: WRITE(SRC(0)); break;
        case OP_DP3:
        case OP_DP4:
        case OP_DPH: {
            f4 a = SRC(0);
            const f4 b = SRC(1);
            if (I.op == OP_DPH) a[3] = 1.0f;
            const f4 p = pmul(a, b);
            float d = 0.0f;
            d += p[0];
            d += p[1];
            d += p[2];
            if (I.op != OP_DP3) d += p[3];
            WRITE(SPLAT(d));
            break;
        }
        case OP_DST: {
            const f4 a = SRC(0), b = SRC(1);
            WRITE(((f4){1.0f, pmul1(a[1], b[1]), a[2], b[3]}));
            break;
        }
        case OP_EX2: { const float v = exp2f(SRC(0)[0]); WRITE(SPLAT(v)); break; }
        case OP_LG2: { const float v = log2f(SRC(0)[0]); WRITE(SPLAT(v)); break; }
        case OP_RCP: { const float v = 1.0f / SRC(0)[0]; WRITE(SPLAT(v)); break; }
        case OP_RSQ: { const float v = 1.0f / sqrtf(SRC(0)[0]); WRITE(SPLAT(v)); break; }
        case OP_FLR: {
            const f4 a = SRC(0);
            WRITE(((f4){floorf(a[0]), floorf(a[1]), floorf(a[2]), floorf(a[3])}));
            break;
        }
        case OP_MAX: {
            const f4 a = SRC(0), b = SRC(1);
            WRITE((f4)(((a > b) & (i4)a) | (~(a > b) & (i4)b)));
            break;
        }
        case OP_MIN: {
            const f4 a = SRC(0), b = SRC(1);
            WRITE((f4)(((a < b) & (i4)a) | (~(a < b) & (i4)b)));
            break;
        }
        case OP_SGE: {
            const f4 a = SRC(0), b = SRC(1);
            WRITE((f4)((a >= b) & (i4)(f4){1, 1, 1, 1}));
            break;
        }
        case OP_SLT: {
            const f4 a = SRC(0), b = SRC(1);
            WRITE((f4)((a < b) & (i4)(f4){1, 1, 1, 1}));
            break;
        }
        case OP_LITP: {
            // partial lighting: clamp inputs as described on 3dbrew
            const f4 a = SRC(0);
            float x = a[0] > 0 ? a[0] : 0, y = a[1] > 0 ? a[1] : 0;
            float w = a[3] < -127.9961f ? -127.9961f : a[3] > 127.9961f ? 127.9961f : a[3];
            u.cond[0] = a[0] > 0;
            u.cond[1] = a[3] > 0;
            WRITE(((f4){x, y, a[2], w}));
            break;
        }
#undef SPLAT
        case OP_MOVA: {
            const f4 a = swizzle(ld(srcreg(I, 0)), I, 0);
            if (I.mask[0]) u.addr[0] = to_s32(a[0]);
            if (I.mask[1]) u.addr[1] = to_s32(a[1]);
            break;
        }
        case OP_CMP: {
            const f4 a = swizzle(ld(srcreg(I, 0)), I, 0);
            const f4 b = swizzle(ld(srcreg(I, 1)), I, 1);
            for (int i = 0; i < 2; i++) {
                u32 op = i == 0 ? I.cmpx : I.cmpy;
                float x = a[i], y = b[i];
                bool c = u.cond[i];
                switch (op) {
                case 0: c = x == y; break;
                case 1: c = x != y; break;
                case 2: c = x < y; break;
                case 3: c = x <= y; break;
                case 4: c = x > y; break;
                case 5: c = x >= y; break;
                default: break;
                }
                u.cond[i] = c;
            }
            break;
        }
        case OP_NOP: break;
        case OP_END: return;
        case OP_BREAK: brk = true; break;
        case OP_BREAKC: brk = cond(I); break;
        case OP_JMPC:
            if (cond(I)) pc = I.fdst - 1;
            break;
        case OP_JMPU:
            if (ub[I.bid] == !(I.fnum & 1)) pc = I.fdst - 1;
            break;
        case OP_CALL:
        case OP_CALLC:
        case OP_CALLU: {
            bool c = I.op == OP_CALL ? true : I.op == OP_CALLC ? cond(I) : ub[I.bid];
            if (c) {
                push(calls, ncall, CallE{(u32)I.fdst + I.fnum, pc + 1});
                pc = I.fdst - 1;
            }
            break;
        }
        case OP_IFU:
        case OP_IFC: {
            bool c = I.op == OP_IFU ? ub[I.bid] : cond(I);
            if (c)
                push(ifs, nif, IfE{I.fdst, (u32)I.fdst + I.fnum});
            else
                pc = I.fdst - 1;
            break;
        }
        case OP_LOOP: {
            const u8 *lp = ui[I.iid];
            u.addr[2] = lp[1];
            push(loops, nloop, LoopE{pc + 1, (u32)I.fdst + 1, lp[0], lp[2], (u8)u.addr[2]});
            u.addr[2] = lp[1];
            break;
        }
        case OP_EMIT: {
            Emitter *e = u.emitter;
            if (e && e->vertex_id < 3) {
                u32 oi = 0;
                for (u32 reg = 0; reg < 16; reg++)
                    if (e->output_mask & (1u << reg)) e->buf[e->vertex_id][oi++] = r[REG_OUT + u.out_bank + reg];
                if (e->prim_emit && e->emit_prim) e->emit_prim(e);
            }
            u.out_bank ^= 16;
            break;
        }
        case OP_SETEMIT:
            if (u.emitter) {
                u.emitter->vertex_id = I.vid;
                u.emitter->prim_emit = I.prim;
                u.emitter->winding = I.wind;
            }
            break;
        default: break;
        }
        pc++;
        if (ncall | nif | nloop) {
            u32 next = old + 1;
            for (u32 i = 0; i < 4; i++) {
                if (ncall == 0 || calls[ncall - 1].end_addr != next) break;
                if (i < 3) {
                    pc = calls[ncall - 1].ret_addr;
                    next = pc;
                }
                ncall--;
            }
            if (nif && ifs[nif - 1].else_addr == old + 1) {
                pc = ifs[nif - 1].end_addr;
                nif--;
            }
            if (nloop && (loops[nloop - 1].end == old + 1 || brk)) {
                LoopE &L = loops[nloop - 1];
                u.addr[2] += L.inc;
                if (!brk && L.cnt-- != 0) {
                    pc = L.entry;
                } else {
                    pc = L.end;
                    if (nloop > 1) u.addr[2] = L.prev;
                    nloop--;
                }
            }
        }
    }
}

// ---------------------------------------------------------------- 4-wide SoA interpreter
// Runs four vertices through a program at once: every register component holds the four lanes.
// Per-lane arithmetic is identical to shader_run(). Flow control that depends on condition codes
// must agree across lanes; otherwise the run is abandoned (returns false) and the caller uses the
// scalar interpreter for these vertices.
struct ShaderUnit4 {
    f4 r[REG_OUT + 16][4];  // inputs, temporaries, (uniform slots unused), outputs bank 0
    s32 addr[3][4];
    bool cond[2][4];
};

ShaderUnit4 *shader_unit4_new() {
    auto *u = new ShaderUnit4;
    shader_unit4_reset(u);
    return u;
}
void shader_unit4_reset(ShaderUnit4 *u) { memset(u, 0, sizeof(*u)); }
bool shader_program_simd(const ShaderProgram *p) { return p->simd_ok; }
u64 shader_program_hash(const ShaderProgram *p) { return p->hash; }

static inline f4 pmul4(f4 a, f4 b) { return pmul(a, b); }

bool shader_run4(const ShaderProgram *P, ShaderUnit4 &u, const V4 *uni, const bool *ub,
                 const u8 (*ui)[4], u32 entry, const V4 (*in)[16], u32 inmask, V4 (*out)[16],
                 u32 outmask) {
    for (u32 reg = 0; reg < 16; reg++) {
        if (!(inmask & (1u << reg))) continue;
        for (u32 c = 0; c < 4; c++)
            u.r[REG_IN + reg][c] = (f4){in[0][reg].v[c], in[1][reg].v[c], in[2][reg].v[c], in[3][reg].v[c]};
    }
    IfE ifs[8];
    CallE calls[4];
    LoopE loops[4];
    u32 nif = 0, ncall = 0, nloop = 0;
    u32 pc = entry;
    const DInst *ins = P->ins.data();
    const u32 n = (u32)P->ins.size();
    const f4 ones = {1, 1, 1, 1};

    // condition evaluation; *ok = false when lanes disagree
    auto cond = [&](const DInst &I, bool *ok) {
        bool v[4];
        for (int l = 0; l < 4; l++) {
            const bool x = I.refx == u.cond[0][l], y = I.refy == u.cond[1][l];
            switch (I.fop) {
            case 0: v[l] = x || y; break;
            case 1: v[l] = x && y; break;
            case 2: v[l] = x; break;
            default: v[l] = y; break;
            }
        }
        if (v[1] != v[0] || v[2] != v[0] || v[3] != v[0]) *ok = false;
        return v[0];
    };
    auto fetch = [&](const DInst &I, int k, f4 s[4]) {
        const u32 idx = I.src[k];
        const u8 *sel = I.sel[k];
        if (I.rel == k) {
            for (int l = 0; l < 4; l++) {
                s32 off = u.addr[I.aidx - 1][l];
                if (off < -128 || off > 127) off = 0;
                const u32 un = (u32)((s32)(idx - REG_UNI) + off) & 0x7f;
                const float *v = un >= 96 ? ones_v4.v : uni[un].v;
                for (int c = 0; c < 4; c++) s[c][l] = v[sel[c]];
            }
        } else if (idx >= REG_UNI && idx < REG_OUT) {
            const float *v = uni[idx - REG_UNI].v;
            for (int c = 0; c < 4; c++) s[c] = (f4){v[sel[c]], v[sel[c]], v[sel[c]], v[sel[c]]};
        } else {
            for (int c = 0; c < 4; c++) s[c] = u.r[idx][sel[c]];
        }
        if (I.neg[k][0])
            for (int c = 0; c < 4; c++) s[c] = (f4)((u4)s[c] ^ 0x80000000u);
    };
    auto write = [&](const DInst &I, const f4 res[4]) {
        f4 *d = u.r[I.dst];
        for (int c = 0; c < 4; c++)
            if (I.mask[c]) d[c] = res[c];
    };
    auto splat_lanes = [](f4 a, float (*fn)(float)) { return (f4){fn(a[0]), fn(a[1]), fn(a[2]), fn(a[3])}; };

    for (u32 budget = 1u << 20; budget; budget--) {
        const DInst &I = pc < n ? ins[pc] : end_inst;
        const u32 old = pc;
        bool brk = false, ok = true;
        f4 a[4], b[4], c3[4], res[4];
        switch (I.op) {
        case OP_ADD:
            fetch(I, 0, a);
            fetch(I, 1, b);
            for (int c = 0; c < 4; c++) res[c] = a[c] + b[c];
            write(I, res);
            break;
        case OP_MUL:
            fetch(I, 0, a);
            fetch(I, 1, b);
            for (int c = 0; c < 4; c++) res[c] = pmul4(a[c], b[c]);
            write(I, res);
            break;
        case OP_MAD:
            fetch(I, 0, a);
            fetch(I, 1, b);
            fetch(I, 2, c3);
            for (int c = 0; c < 4; c++) res[c] = pmul4(a[c], b[c]) + c3[c];
            write(I, res);
            break;
        case OP_MOV:
            fetch(I, 0, a);
            write(I, a);
            break;
        case OP_DP3:
        case OP_DP4:
        case OP_DPH: {
            fetch(I, 0, a);
            fetch(I, 1, b);
            if (I.op == OP_DPH) a[3] = ones;
            f4 d = {0, 0, 0, 0};
            d += pmul4(a[0], b[0]);
            d += pmul4(a[1], b[1]);
            d += pmul4(a[2], b[2]);
            if (I.op != OP_DP3) d += pmul4(a[3], b[3]);
            for (int c = 0; c < 4; c++) res[c] = d;
            write(I, res);
            break;
        }
        case OP_EX2:
        case OP_LG2:
        case OP_RCP:
        case OP_RSQ: {
            fetch(I, 0, a);
            f4 v;
            if (I.op == OP_EX2) v = splat_lanes(a[0], exp2f);
            else if (I.op == OP_LG2) v = splat_lanes(a[0], log2f);
            else if (I.op == OP_RCP) v = (f4){1.0f / a[0][0], 1.0f / a[0][1], 1.0f / a[0][2], 1.0f / a[0][3]};
            else v = (f4){1.0f / sqrtf(a[0][0]), 1.0f / sqrtf(a[0][1]), 1.0f / sqrtf(a[0][2]), 1.0f / sqrtf(a[0][3])};
            for (int c = 0; c < 4; c++) res[c] = v;
            write(I, res);
            break;
        }
        case OP_FLR:
            fetch(I, 0, a);
            for (int c = 0; c < 4; c++) res[c] = splat_lanes(a[c], floorf);
            write(I, res);
            break;
        case OP_MAX:
        case OP_MIN:
        case OP_SGE:
        case OP_SLT:
            fetch(I, 0, a);
            fetch(I, 1, b);
            for (int c = 0; c < 4; c++) {
                const f4 x = a[c], y = b[c];
                switch (I.op) {
                case OP_MAX: res[c] = (f4)(((x > y) & (i4)x) | (~(x > y) & (i4)y)); break;
                case OP_MIN: res[c] = (f4)(((x < y) & (i4)x) | (~(x < y) & (i4)y)); break;
                case OP_SGE: res[c] = (f4)((x >= y) & (i4)ones); break;
                default: res[c] = (f4)((x < y) & (i4)ones); break;
                }
            }
            write(I, res);
            break;
        case OP_MOVA:
            fetch(I, 0, a);
            for (int l = 0; l < 4; l++) {
                if (I.mask[0]) u.addr[0][l] = to_s32(a[0][l]);
                if (I.mask[1]) u.addr[1][l] = to_s32(a[1][l]);
            }
            break;
        case OP_CMP:
            fetch(I, 0, a);
            fetch(I, 1, b);
            for (int i = 0; i < 2; i++) {
                const u32 op = i == 0 ? I.cmpx : I.cmpy;
                for (int l = 0; l < 4; l++) {
                    const float x = a[i][l], y = b[i][l];
                    bool cc = u.cond[i][l];
                    switch (op) {
                    case 0: cc = x == y; break;
                    case 1: cc = x != y; break;
                    case 2: cc = x < y; break;
                    case 3: cc = x <= y; break;
                    case 4: cc = x > y; break;
                    case 5: cc = x >= y; break;
                    default: break;
                    }
                    u.cond[i][l] = cc;
                }
            }
            break;
        case OP_NOP: break;
        case OP_END:
            for (u32 reg = 0; reg < 16; reg++) {
                if (!(outmask & (1u << reg))) continue;
                for (u32 c = 0; c < 4; c++) {
                    const f4 v = u.r[REG_OUT + reg][c];
                    for (int l = 0; l < 4; l++) out[l][reg].v[c] = v[l];
                }
            }
            return true;
        case OP_BREAK: brk = true; break;
        case OP_BREAKC: brk = cond(I, &ok); break;
        case OP_JMPC:
            if (cond(I, &ok)) pc = I.fdst - 1;
            break;
        case OP_JMPU:
            if (ub[I.bid] == !(I.fnum & 1)) pc = I.fdst - 1;
            break;
        case OP_CALL:
        case OP_CALLC:
        case OP_CALLU: {
            const bool c = I.op == OP_CALL ? true : I.op == OP_CALLC ? cond(I, &ok) : ub[I.bid];
            if (c) {
                push(calls, ncall, CallE{(u32)I.fdst + I.fnum, pc + 1});
                pc = I.fdst - 1;
            }
            break;
        }
        case OP_IFU:
        case OP_IFC: {
            const bool c = I.op == OP_IFU ? ub[I.bid] : cond(I, &ok);
            if (c)
                push(ifs, nif, IfE{I.fdst, (u32)I.fdst + I.fnum});
            else
                pc = I.fdst - 1;
            break;
        }
        case OP_LOOP: {
            const u8 *lp = ui[I.iid];
            for (int l = 0; l < 4; l++) u.addr[2][l] = lp[1];
            push(loops, nloop, LoopE{pc + 1, (u32)I.fdst + 1, lp[0], lp[2], (u8)lp[1]});
            break;
        }
        default: return false;  // DST / LITP / EMIT / SETEMIT: not supported here
        }
        if (!ok) return false;
        pc++;
        if (ncall | nif | nloop) {
            u32 next = old + 1;
            for (u32 i = 0; i < 4; i++) {
                if (ncall == 0 || calls[ncall - 1].end_addr != next) break;
                if (i < 3) {
                    pc = calls[ncall - 1].ret_addr;
                    next = pc;
                }
                ncall--;
            }
            if (nif && ifs[nif - 1].else_addr == old + 1) {
                pc = ifs[nif - 1].end_addr;
                nif--;
            }
            if (nloop && (loops[nloop - 1].end == old + 1 || brk)) {
                LoopE &L = loops[nloop - 1];
                for (int l = 0; l < 4; l++) u.addr[2][l] += L.inc;
                if (!brk && L.cnt-- != 0) {
                    pc = L.entry;
                } else {
                    pc = L.end;
                    if (nloop > 1)
                        for (int l = 0; l < 4; l++) u.addr[2][l] = L.prev;
                    nloop--;
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- Metal translation
namespace {
// Metal source for the vertex program's registers, sources and arithmetic, shared by the two kernel
// generators below.
struct MslWriter {
    const ShaderProgram *P;
    std::string o;
    const char *al = "0";  // the loop counter register's expression
    // a geometry shader's registers are named apart from its vertex shader's, in the one GLSL shader both make (shader_glsl_gs): "g" before
    // them, its uniforms gf, and its outputs in the bank its emits have come to (shader_run switches banks at every EMIT); -1: a vertex shader's o
    std::string pre;
    const char *uni = "f";
    int bank = -1;
    // A geometry shader's float uniforms prim_lo to prim_hi are a fixed-primitive run's vertices (gen_gsx_glsl's gp), which gs_rel tells apart.
    // An aL-relative read that none of al_values (the aLs of the loop being written) takes into them reads the uniforms alone: gs_ru, which
    // leaves gp unread, so the compiler needn't keep it in registers through the loop.
    u32 prim_lo = 0, prim_hi = 0;
    std::vector<int> al_values;
    bool off_primitive(int base, int aidx) const {
        if (aidx != 3 || al_values.empty() || prim_lo >= prim_hi) return false;
        for (int off : al_values) {
            const u32 u = (u32)(base + (off < -128 || off > 127 ? 0 : off)) & 127;  // as gs_rel takes it
            if (u >= prim_lo && u < prim_hi) return false;
        }
        return true;
    }
    char b[512];
    template <class... A> void f(const char *fmt, A... a) {
        snprintf(b, sizeof b, fmt, a...);
        o += b;
    }
    std::string reg(u32 idx) {
        if (idx < REG_TMP) return pre + "v" + std::to_string(idx);
        if (idx < REG_UNI) return pre + "t" + std::to_string(idx - REG_TMP);
        if (idx < REG_OUT) return std::string("P.") + uni + "[" + std::to_string(idx - REG_UNI) + "]";
        if (idx < REG_ZERO) return (bank < 0 ? std::string("o[") : "go" + std::to_string(bank) + "[") + std::to_string((idx - REG_OUT) & 15) + "]";
        return idx == REG_ONE ? "float4(1.0)" : "float4(0.0)";
    }
    std::string src(const DInst &I, int k) {
        const std::string aname[4] = {"0", pre + "a0", pre + "a1", al};
        std::string r;
        if (I.rel == k)
            r = (pre.empty() ? "vs_rel(P, " : off_primitive((int)I.src[k] - (int)REG_UNI, I.aidx) ? "gs_ru(P, " : "gs_rel(P, ") +
                std::to_string((int)I.src[k] - (int)REG_UNI) + ", " + aname[I.aidx] + ")";
        else
            r = reg(I.src[k]);
        static const char xyzw[] = "xyzw";
        std::string sw;
        for (int c = 0; c < 4; c++) sw += xyzw[I.sel[k][c]];
        if (sw != "xyzw") r = "(" + r + ")." + sw;
        if (I.neg[k][0]) r = "(-" + r + ")";
        return r;
    }
    void write(const DInst &I, const std::string &expr) {
        static const char xyzw[] = "xyzw";
        std::string m;
        for (int c = 0; c < 4; c++)
            if (I.mask[c]) m += xyzw[c];
        if (m.empty()) return;
        const std::string d = reg(I.dst);
        if (m == "xyzw")
            o += "    " + d + " = " + expr + ";\n";
        else
            o += "    { const float4 r_ = " + expr + "; " + d + "." + m + " = r_." + m + "; }\n";
    }
    std::string cond(const DInst &I) {
        snprintf(b, sizeof b, "(%sc0 == %s)", pre.c_str(), I.refx ? "true" : "false");
        const std::string x = b;
        snprintf(b, sizeof b, "(%sc1 == %s)", pre.c_str(), I.refy ? "true" : "false");
        const std::string y = b;
        switch (I.fop) {
        case 0: return "(" + x + " || " + y + ")";
        case 1: return "(" + x + " && " + y + ")";
        case 2: return x;
        default: return y;
        }
    }
    // an arithmetic (or NOP) instruction; false for flow control and END
    bool op(const DInst &I) {
        switch (I.op) {
        case OP_ADD: write(I, src(I, 0) + " + " + src(I, 1)); break;
        case OP_MUL: write(I, "vs_mul(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_MAD: write(I, "vs_mul(" + src(I, 0) + ", " + src(I, 1) + ") + " + src(I, 2)); break;
        case OP_MOV: write(I, src(I, 0)); break;
        case OP_DP3: write(I, "vs_dp3(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_DP4: write(I, "vs_dp4(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_DPH: write(I, "vs_dph(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_DST: write(I, "vs_dst(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_EX2: write(I, "float4(precise::exp2(" + src(I, 0) + ".x))"); break;
        case OP_LG2: write(I, "float4(precise::log2(" + src(I, 0) + ".x))"); break;
        case OP_RCP: write(I, "float4(1.0 / " + src(I, 0) + ".x)"); break;
        case OP_RSQ: write(I, "float4(1.0 / precise::sqrt(" + src(I, 0) + ".x))"); break;
        case OP_FLR: write(I, "floor(" + src(I, 0) + ")"); break;
        case OP_MAX: write(I, "vs_max(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_MIN: write(I, "vs_min(" + src(I, 0) + ", " + src(I, 1) + ")"); break;
        case OP_SGE: write(I, "select(float4(0.0), float4(1.0), " + src(I, 0) + " >= " + src(I, 1) + ")"); break;
        case OP_SLT: write(I, "select(float4(0.0), float4(1.0), " + src(I, 0) + " < " + src(I, 1) + ")"); break;
        case OP_LITP:
            o += "    { const float4 a_ = " + src(I, 0) + "; " + pre + "c0 = a_.x > 0.0; " + pre + "c1 = a_.w > 0.0;\n";
            o += "      const float4 l_ = float4(max(a_.x, 0.0), max(a_.y, 0.0), a_.z, clamp(a_.w, -127.9961, 127.9961));\n";
            write(I, "l_");
            o += "    }\n";
            break;
        case OP_MOVA:
            o += "    { const float4 a_ = " + src(I, 0) + ";";
            if (I.mask[0]) o += " " + pre + "a0 = vs_s32(a_.x);";
            if (I.mask[1]) o += " " + pre + "a1 = vs_s32(a_.y);";
            o += " }\n";
            break;
        case OP_CMP: {
            o += "    { const float4 a_ = " + src(I, 0) + ", b_ = " + src(I, 1) + ";";
            static const char *ops[6] = {"==", "!=", "<", "<=", ">", ">="};
            if (I.cmpx < 6) f(" %sc0 = a_.x %s b_.x;", pre.c_str(), ops[I.cmpx]);
            if (I.cmpy < 6) f(" %sc1 = a_.y %s b_.y;", pre.c_str(), ops[I.cmpy]);
            o += " }\n";
            break;
        }
        case OP_NOP: break;
        default: return false;
        }
        return true;
    }
    // kernel signature, inputs and registers
    void prologue(const char *name) {
        u32 in_used = 0;
        for (const DInst &I : P->ins) {
            const int ns = I.op == OP_MAD ? 3 : 2;
            if (I.op <= OP_MAD)
                for (int k = 0; k < ns; k++)
                    if (I.src[k] < REG_TMP) in_used |= 1u << I.src[k];
        }
        // one thread per vertex of all the draws using this kernel in a batch: tj gives the draw (its
        // parameters), whose first thread is P.first
        f("kernel void %s(device const uchar *vd [[buffer(0)]], device const uint *vids [[buffer(1)]],\n"
          "    constant VsParams *PS [[buffer(2)]], device float *outv [[buffer(3)]], device const uint *tj [[buffer(4)]],\n"
          "    uint tid [[thread_position_in_grid]]) {\n"
          "  constant VsParams &P = PS[tj[tid]];\n  const uint vid = vids[tid], li = tid - P.first;\n",
          name);
        for (u32 r = 0; r < 16; r++)
            if (in_used & (1u << r)) f("  const float4 v%u = vs_input(vd, P, %u, vid);\n", r, r);
        o += "  float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0, t8 = 0, t9 = 0, t10 = 0, t11 = 0, "
             "t12 = 0, t13 = 0, t14 = 0, t15 = 0;\n"
             "  float4 o[16]; for (int i = 0; i < 16; i++) o[i] = 0;\n"
             "  int a0 = 0, a1 = 0; bool c0 = false, c1 = false;\n";
    }
};
}  // namespace

// A vertex program as a Metal compute kernel shading one vertex per thread (see kVsCommonMSL in
// metal.mm for the parameter block and helpers). Flow control reproduces shader_run() exactly: a
// program-counter loop over a switch whose cases are the possible flow targets, with straight-line
// code falling through between them, and the same if/call/loop stacks and end-address checks.
std::string shader_msl(const ShaderProgram *P, u32 entry, const char *name) {
    const u32 n = (u32)P->ins.size();
    if (entry >= n) return "";
    bool flow = false;
    std::vector<bool> label(n + 1, false), endset(n + 2, false);
    for (u32 i = 0; i < n; i++) {
        const DInst &I = P->ins[i];
        switch (I.op) {
        case OP_EMIT:
        case OP_SETEMIT: return "";  // geometry shader only
        case OP_BREAK: case OP_BREAKC: case OP_JMPC: case OP_JMPU: case OP_CALL: case OP_CALLC: case OP_CALLU:
        case OP_IFU: case OP_IFC: case OP_LOOP: {
            flow = true;
            auto mark = [&](u32 a) { if (a <= n) label[a] = true; };
            mark(i + 1);
            mark(I.fdst);
            mark(I.fdst + I.fnum);
            mark(I.fdst + 1);
            if (I.op == OP_CALL || I.op == OP_CALLC || I.op == OP_CALLU) {
                if (I.fdst + I.fnum <= n + 1) endset[I.fdst + I.fnum] = true;
            } else if (I.op == OP_IFU || I.op == OP_IFC) {
                if (I.fdst <= n + 1) endset[I.fdst] = true;
            } else if (I.op == OP_LOOP) {
                if (I.fdst + 1 <= n + 1) endset[I.fdst + 1] = true;
            }
            break;
        }
        default: break;
        }
    }
    label[entry] = true;
    MslWriter w{P};
    std::string &o = w.o;
    w.prologue(name);
    if (flow) o += "  VsFlow S; S.nif = 0; S.ncall = 0; S.nloop = 0; S.aL = 0;\n";
    w.al = flow ? "S.aL" : "0";
    auto ub = [&](u32 bid) { return "((P.b >> " + std::to_string(bid) + "u) & 1u) != 0u"; };
    if (flow) o += "  uint pc = " + std::to_string(entry) + ";\n  for (uint guard_ = 0; guard_ < 262144u; guard_++) {\n  switch (pc) {\n";
    const u32 start = flow ? 0 : entry;
    for (u32 i = start; i < n; i++) {
        const DInst &I = P->ins[i];
        if (flow && label[i]) w.f("  case %u:\n", i);
        // next pc / break flag for flow instructions ("" = not a flow instruction)
        std::string npc, brk = "false";
        const std::string next = std::to_string(i + 1), dst = std::to_string(I.fdst);
        if (!w.op(I)) switch (I.op) {
        case OP_END: o += flow ? "    pc = 0xffffffffu; break;\n" : "    goto_end_;\n"; break;
        case OP_BREAK: npc = next; brk = "true"; break;
        case OP_BREAKC: npc = next; brk = w.cond(I); break;
        case OP_JMPC: npc = "(" + w.cond(I) + " ? " + dst + "u : " + next + "u)"; break;
        case OP_JMPU:
            npc = std::string("((") + ub(I.bid) + ") == " + ((I.fnum & 1) ? "false" : "true") + " ? " + dst + "u : " + next + "u)";
            break;
        case OP_CALL:
        case OP_CALLC:
        case OP_CALLU: {
            const std::string c = I.op == OP_CALL ? "true" : I.op == OP_CALLC ? w.cond(I) : ub(I.bid);
            o += "    { const bool c_ = " + c + "; if (c_) vs_push_call(S, " + std::to_string(I.fdst + I.fnum) + "u, " + next + "u);\n";
            o += "      uint np_ = c_ ? " + dst + "u : " + next + "u; pc = vs_flow(S, " + std::to_string(i) + "u, np_, false); continue; }\n";
            break;
        }
        case OP_IFU:
        case OP_IFC: {
            const std::string c = I.op == OP_IFU ? ub(I.bid) : w.cond(I);
            o += "    { const bool c_ = " + c + "; if (c_) vs_push_if(S, " + dst + "u, " + std::to_string(I.fdst + I.fnum) + "u);\n";
            o += "      uint np_ = c_ ? " + next + "u : " + dst + "u; pc = vs_flow(S, " + std::to_string(i) + "u, np_, false); continue; }\n";
            break;
        }
        case OP_LOOP:
            w.f("    { const uint4 l_ = P.iu[%u]; S.aL = int(l_.y); vs_push_loop(S, %uu, %uu, l_.x, l_.z, l_.y); S.aL = int(l_.y);\n",
                (u32)I.iid, i + 1, (u32)I.fdst + 1);
            w.f("      pc = vs_flow(S, %uu, %uu, false); continue; }\n", i, i + 1);
            break;
        default: break;
        }
        if (!npc.empty()) {
            o += "    pc = vs_flow(S, " + std::to_string(i) + "u, " + npc + ", " + brk + "); continue;\n";
        } else if (flow && I.op != OP_END && I.op != OP_CALL && I.op != OP_CALLC && I.op != OP_CALLU &&
                   I.op != OP_IFU && I.op != OP_IFC && I.op != OP_LOOP && endset[i + 1]) {
            w.f("    pc = vs_flow(S, %uu, %uu, false); continue;\n", i, i + 1);
        }
        if (!flow && I.op == OP_END) break;
    }
    if (flow) o += "  default: pc = 0xffffffffu; break;\n  }\n  if (pc == 0xffffffffu) break;\n  }\n";
    // goto_end_ (straight-line programs): everything after END is dead code, so END just ends the body
    for (size_t p; (p = o.find("    goto_end_;\n")) != std::string::npos;) o.erase(p, 15);
    o += "  vs_output(P, o, outv, li);\n}\n";
    return o;
}

u32 shader_static_uniforms(const ShaderProgram *P, u32 *loops) {
    u32 bmask = 0, lmask = 0;
    for (const DInst &I : P->ins) {
        if (I.op == OP_IFU || I.op == OP_CALLU || I.op == OP_JMPU) bmask |= 1u << I.bid;
        if (I.op == OP_LOOP) lmask |= 1u << I.iid;
    }
    *loops = lmask;
    return bmask;
}

// Over every way a run could go (each condition both ways, every call's body, both ends of every if, every loop's exit), so false only where
// no run ends: the jumps the bools take go round for ever.
bool shader_ends(const ShaderProgram *P, u32 entry, u32 b) {
    const u32 n = (u32)P->ins.size();
    std::vector<char> seen(n + 1, 0);
    std::vector<u32> todo{std::min(entry, n)};
    while (!todo.empty()) {
        u32 i = todo.back();
        todo.pop_back();
        while (!seen[i]) {
            seen[i] = 1;
            if (i == n || P->ins[i].op == OP_END) return true;
            const DInst &I = P->ins[i];
            auto at = [&](u32 a) { return std::min(a, n); };
            if (I.op == OP_JMPU && ((b >> I.bid) & 1) == !(I.fnum & 1)) {
                i = at(I.fdst);
                continue;
            }
            if (I.op == OP_JMPC || I.op == OP_CALL || I.op == OP_CALLC || I.op == OP_CALLU) todo.push_back(at(I.fdst));
            if (I.op == OP_IFU || I.op == OP_IFC) todo.insert(todo.end(), {at(I.fdst), at(I.fdst + I.fnum)});
            if (I.op == OP_LOOP) todo.push_back(at(I.fdst + 1));
            i++;
        }
    }
    return false;
}

// The same program specialized for one set of bool and int uniforms: their if/call/jump decisions and
// loop counts are resolved here, so the kernel is plain structured code (if/else, for, break) with only
// the per-vertex conditions left, which the GPU runs far faster than the program-counter loop. Programs
// whose flow is not properly nested (where shader_run's end-address checks would do something else than
// structured code would) give "" and use shader_msl().
namespace {
struct SpecGen {
    MslWriter &w;
    const ShaderProgram *P;
    u32 n, b;
    const u8 (*iu)[4];
    u32 open = 0;  // the bools left to run time (shader_glsl_spec): a test of one is written out, like a test of a per-vertex condition
    bool decided(const DInst &I) const { return !((open >> I.bid) & 1); }
    static std::string open_bool(const DInst &I) { return "((uInt[0].w >> " + std::to_string(I.bid) + ") & 1) != 0"; }
    int loops = 0, ifs = 0, calls = 0, counter = 0;
    bool fail = false;
    int why = 0;  // the source line that gave up, and the instruction (shader_glsl_fail)
    u32 at = 0;
    // A geometry shader (shader_glsl_gs): each vertex of the GLSL shader is one corner of one triangle the program emits, so every EMIT is
    // written out once, keeping that corner if it's this vertex's (wantT, wantJ). Whatever a run of shader_run carries over from the run before
    // (an emit's vertex id and flags, the triangle slots, an output bank's registers, temporaries, address registers, condition flags, input
    // registers no attribute loads) has to be set in the same run before it's read, or the program stays with the CPU: es says what is set,
    // along the code being written.
    bool gs = false;
    bool emitting = false;  // writing the body of a loop whose EMITs emitting_loop allows
    u64 gs_need = 0;       // the output components the output map reads (register * 4 + component)
    u32 gs_out = 0;        // their registers, which an emit copies
    u32 gs_in = 0;         // the input registers a run is given
    bool gs_wind = false;  // the draw's topology turns a triangle round on SETEMIT's winding flag (pa_submit)
    u32 tris = 0;          // the triangles written out, the most a run can emit
    struct Emit {
        int bank = -1;                        // the output bank (the writer's)
        int vid = -1, prim = -1, wind = -1;  // what SETEMIT last set (-1: not known here)
        u32 filled = 0;                       // the triangle slots an emit filled
        u64 written[2] = {0, 0};              // the output components each bank had written (register * 4 + component)
        u64 temps = 0;                        // the temporaries' components written
        u32 flags = 0;                        // a0, a1, c0, c1 written
    } es;
    static int sources(u8 op) {
        switch (op) {
        case OP_NOP: return 0;
        case OP_EX2: case OP_LG2: case OP_LITP: case OP_FLR: case OP_RCP: case OP_RSQ: case OP_MOVA: case OP_MOV: return 1;
        case OP_MAD: return 3;
        default: return 2;
        }
    }
    // an arithmetic instruction of a geometry shader: false if it reads what this run hasn't set; else what it sets is noted
    bool reads_set(const DInst &I) {
        for (int k = 0; k < sources(I.op); k++) {
            const u32 r = I.src[k];
            if (I.rel == k && !(I.aidx == 3 ? loops > 0 : (es.flags >> (I.aidx - 1)) & 1)) return false;
            if (r < REG_TMP && !((gs_in >> r) & 1)) return false;
            if (r >= REG_TMP && r < REG_UNI)
                for (int c = 0; c < 4; c++)
                    if (!((es.temps >> ((r - REG_TMP) * 4 + I.sel[k][c])) & 1)) return false;
        }
        const Comps out = writes(I, w.bank);
        es.written[w.bank] |= out.o[w.bank];
        es.temps |= out.t;
        es.flags |= out.f;
        return true;
    }
    // a condition the flags this run set decide
    bool cond_set(const DInst &I) const {
        const u32 needs = I.fop == 2 ? 4 : I.fop == 3 ? 8 : 12;
        return !gs || (es.flags & needs) == needs;
    }
    Emit now() const {
        Emit e = es;
        e.bank = w.bank;
        return e;
    }
    void set(const Emit &e) {
        es = e;
        w.bank = e.bank;
    }
    // where two ways through the code meet (a: the first, which may not reach the meeting; b: the second): what both have set. Their banks
    // must agree, or which registers the code after it writes would depend on the vertex.
    void join(const Emit &a, bool a_reaches, const Emit &b, bool b_reaches) {
        if (!a_reaches || !b_reaches) return set(a_reaches ? a : b);
        if (a.bank != b.bank) fail = true;
        Emit m = a;
        m.vid = a.vid == b.vid ? a.vid : -1;
        m.prim = a.prim == b.prim ? a.prim : -1;
        m.wind = a.wind == b.wind ? a.wind : -1;
        m.filled = a.filled & b.filled;
        m.written[0] = a.written[0] & b.written[0];
        m.written[1] = a.written[1] & b.written[1];
        m.temps = a.temps & b.temps;
        m.flags = a.flags & b.flags;
        set(m);
    }
    // EMIT, as shader_run's: the outputs into the slot SETEMIT named, and with its primitive flag a triangle of the three slots, which
    // pa_submit turns round on the winding flag where the draw's topology says so. False: it depends on what a run before left.
    bool emit() {
        if (es.vid < 0) return false;
        if (es.vid < 3) {
            if ((es.written[w.bank] & gs_need) != gs_need || es.prim < 0 || (es.prim && es.wind < 0)) return false;
            for (u32 r = 0; r < 16; r++)
                if ((gs_out >> r) & 1) w.f("    s%d_%u = go%d[%u];\n", es.vid, r, w.bank, r);
            es.filled |= 1u << es.vid;
            if (es.prim) {
                if (es.filled != 7) return false;
                static const int straight[3] = {0, 1, 2}, turned[3] = {1, 0, 2};
                const int *order = es.wind && gs_wind ? turned : straight;
                w.o += "    if (gtri == wantT) {\n";
                for (int j = 0; j < 3; j++) {
                    w.f("        %sif (wantJ == %d) {", j ? "else " : "", j);
                    for (u32 r = 0; r < 16; r++)
                        if ((gs_out >> r) & 1) w.f(" co[%u] = s%d_%u;", r, order[j], r);
                    w.o += " }\n";
                }
                w.o += "        got = true;\n    }\n    gtri++;\n";
                tris++;
            }
        }
        w.bank ^= 1;
        return true;
    }
    // end-check addresses of the active regions: shader_run tests every region type against the address
    // after the last executed instruction, so two active regions must never share one
    std::vector<u32> ends;
    bool enter(u32 x) {
        for (u32 a : ends)
            if (a == x) return false;
        ends.push_back(x);
        return true;
    }
    // Whether an active region ends at x. Control that arrives there other than by running the word before it (a jump onto it, a return
    // to it, the end of an inner region) pops nothing in shader_run, so the region stays open; anywhere else, the code just goes on from x.
    bool region_end(u32 x) const { return std::find(ends.begin(), ends.end(), x) != ends.end(); }
    static bool plain(const DInst &I) { return I.op <= OP_MAD || I.op == OP_NOP; }
    // whether every jump, if and loop in [s, d) stays within [s, d]: then a forward jump from s - 1 to d is an if around [s, d)
    bool nests(u32 s, u32 d) const {
        for (u32 x = s; x < d && x < n; x++) {
            const DInst &J = P->ins[x];
            switch (J.op) {
            case OP_JMPC:
            case OP_JMPU:
                if (J.fdst <= x || J.fdst > d) return false;
                break;
            case OP_IFC:
            case OP_IFU:
                if (J.fdst < x + 1 || J.fdst + J.fnum > d) return false;
                break;
            case OP_LOOP:
                if (J.fdst < x + 1 || J.fdst + 1 > d) return false;
                break;
            default:
                break;
            }
        }
        return true;
    }
    bool emits_in(u32 s, u32 e) const {
        for (u32 k = s; k < e && k < n; k++)
            if (P->ins[k].op == OP_EMIT) return true;
        return false;
    }

    // Register components, as a turn of a loop reads and writes them: temporaries (register * 4 + component), outputs in each bank,
    // a0 a1 c0 c1
    struct Comps {
        u64 t = 0, o[2] = {0, 0};
        u32 f = 0;
        bool any() const { return t || o[0] || o[1] || f; }
        Comps operator&(const Comps &x) const {
            Comps r;
            r.t = t & x.t, r.o[0] = o[0] & x.o[0], r.o[1] = o[1] & x.o[1], r.f = f & x.f;
            return r;
        }
        Comps operator~() const {
            Comps r;
            r.t = ~t, r.o[0] = ~o[0], r.o[1] = ~o[1], r.f = ~f;
            return r;
        }
        Comps &operator|=(const Comps &x) {
            t |= x.t, o[0] |= x.o[0], o[1] |= x.o[1], f |= x.f;
            return *this;
        }
    };
    // what an arithmetic instruction reads (every component its swizzles name) and writes, its outputs in bank bk
    Comps reads(const DInst &I) const {
        Comps c;
        for (int k = 0; k < sources(I.op); k++) {
            const u32 r = I.src[k];
            if (r >= REG_TMP && r < REG_UNI)
                for (int x = 0; x < 4; x++) c.t |= 1ull << ((r - REG_TMP) * 4 + I.sel[k][x]);
            if (I.rel == k && (I.aidx == 1 || I.aidx == 2)) c.f |= 1u << (I.aidx - 1);
        }
        return c;
    }
    static Comps writes(const DInst &I, int bk) {
        Comps c;
        u64 m = 0;
        for (int x = 0; x < 4; x++)
            if (I.mask[x]) m |= 1ull << x;
        switch (I.op) {
        case OP_NOP: break;
        case OP_MOVA: c.f = (u32)(m & 3); break;
        case OP_CMP: c.f = (I.cmpx < 6 ? 4u : 0u) | (I.cmpy < 6 ? 8u : 0u); break;
        default:
            if (I.op == OP_LITP) c.f = 12;
            if (I.dst >= REG_OUT && I.dst < REG_ZERO) c.o[bk] = m << (((I.dst - REG_OUT) & 15) * 4);
            else if (I.dst >= REG_TMP && I.dst < REG_UNI) c.t = m << ((I.dst - REG_TMP) * 4);
        }
        return c;
    }

    // A loop whose body emits (Luigi's Mansion 2's rain trail: 19 turns of 22 emits, each turn's corners from its own uniforms and from what
    // the turn before left). Each vertex of the GLSL shader wants one triangle, so it runs the turn holding that triangle (the last turn, for one
    // after the loop) after just the part of the turn before that sets what a turn carries over. That is shader_run's result where the body is
    // straight code of arithmetic, SETEMITs and EMITs at the top level, with an even number of emits (the banks come round), its first SETEMIT
    // before its first EMIT, every triangle of corners its own turn emitted, and everything a turn carries over set from what that turn
    // reads alone. Anything else stays with the CPU. The count, first aL and step are in the key (gsx_variant_key).
    bool emitting_loop(const DInst &L, u32 s, u32 e) {
        if (loops || ifs || calls || !iu) return false;
        const u32 turns = iu[L.iid][0] + 1u;
        std::vector<int> bank_at(e - s);
        Comps body;  // what the body writes
        int bk = w.bank;
        u32 emits = 0, per_turn = 0, filled = 0;
        for (u32 k = s; k < e; k++) {
            const DInst &I = P->ins[k];
            bank_at[k - s] = bk;
            if (I.op == OP_SETEMIT) {
                if (I.vid > 2) return false;
                continue;
            }
            if (I.op == OP_EMIT) {
                emits++;
                bk ^= 1;
                continue;
            }
            if (!plain(I)) return false;
            body |= writes(I, bk);
        }
        // the triangles a turn makes, of corners it emitted itself, each emit named by the turn's own SETEMIT
        int vid = -1, prim = 0;
        for (u32 k = s; k < e; k++) {
            const DInst &I = P->ins[k];
            if (I.op == OP_SETEMIT) vid = I.vid, prim = I.prim;
            if (I.op != OP_EMIT) continue;
            if (vid < 0) return false;
            filled |= 1u << vid;
            if (prim) {
                if (filled != 7) return false;
                per_turn++;
            }
        }
        if ((emits & 1) || !per_turn) return false;
        // what a turn carries over (read before the turn writes it, and written in it), and whether a component's value at the turn's end
        // depends on any of it
        Comps done, tainted, carried;
        for (u32 k = s; k < e; k++) {
            const DInst &I = P->ins[k];
            if (I.op == OP_SETEMIT) continue;
            Comps in;
            if (I.op == OP_EMIT) in.o[bank_at[k - s]] = gs_need;
            else in = reads(I);
            const Comps old = in & body & ~done;
            carried |= old;
            if (I.op == OP_EMIT) continue;
            const Comps out = writes(I, bank_at[k - s]);
            const bool from_old = old.any() || (in & tainted).any();
            done |= out;
            tainted = tainted & ~out;
            if (from_old) tainted |= out;
        }
        if ((carried & tainted).any()) return false;
        // the part of a turn that sets what the next carries over, found backwards from it
        std::vector<char> part(e - s, 0);
        Comps need = carried;
        for (u32 k = e; k-- > s;) {
            const DInst &I = P->ins[k];
            if (I.op == OP_SETEMIT || I.op == OP_EMIT) continue;
            const Comps out = writes(I, bank_at[k - s]);
            if (!(out & need).any()) continue;
            part[k - s] = 1;
            need = need & ~out;
            need |= reads(I);
        }
        // Integer arithmetic as floats, exact at these sizes: no integer divide or multiply, which no shader here has had, for the console's
        // compiler or Cemu to meet first (each has failed on an integer operation before: the unsigned compare at OP_LOOP below).
        const int k = counter++;
        const int saved = w.bank;
        for (u32 t = 0; t < turns; t++) w.al_values.push_back((int)iu[L.iid][1] + (int)t * (int)iu[L.iid][2]);  // as the GLSL counts them
        // A vertex whose triangle came before the loop has it already and skips the loop. (Not a return at each triangle: each nests the
        // rest of the program a level deeper in the compiled branches.)
        w.f("    if (!got) { int tt%d = int((float(wantT - gtri) + 0.5) / %u.0);\n", k, per_turn);
        w.f("    if (tt%d > %u) tt%d = %u;\n", k, turns - 1, k, turns - 1);
        w.f("    if (tt%d > 0) {\n    %s = uInt[%u].y + int(float(tt%d - 1) * float(uInt[%u].z));\n", k, w.al, L.iid, k, L.iid);
        for (u32 x = s; x < e; x++)
            if (part[x - s]) {
                w.bank = bank_at[x - s];
                w.op(P->ins[x]);
            }
        w.bank = saved;
        w.f("    }\n    gtri += int(float(tt%d) * %u.0);\n", k, per_turn);
        w.f("    %s = uInt[%u].y + int(float(tt%d) * float(uInt[%u].z));\n", w.al, L.iid, k, L.iid);
        const u32 tris_before = tris;
        loops++;
        emitting = true;
        const bool reaches = block(s, e, true, L.iid);
        emitting = false;
        loops--;
        w.al_values.clear();
        if (!reaches || fail || w.bank != saved || tris - tris_before != per_turn) return false;
        tris = tris_before + per_turn * turns;
        w.f("    %s += uInt[%u].z; }\n", w.al, L.iid);
        return true;
    }
    // the last instruction of a region must be plain (or END), so that shader_run pops exactly that region there
    bool ends_plain(u32 s, u32 e) {
        return e <= s || (e - 1 < n && (plain(P->ins[e - 1]) || P->ins[e - 1].op == OP_END));
    }
    // emits [s, e); in_loop: directly in a loop body, where BREAK/BREAKC end the loop (inc: which integer uniform
    // holds its counter increment). Returns whether control can reach e.
    bool block(u32 s, u32 e, bool in_loop, int inc) {
        u32 i = s;
        while (i < e && !fail) {
            if (w.o.size() > 400000) {
                fail = true; why = __LINE__; at = i;
                break;
            }
            if (i >= n) {  // past the program: shader_run stops
                w.o += "    vs_output(P, o, outv, li); return;\n";
                return false;
            }
            const DInst &I = P->ins[i];
            if (w.op(I)) {
                if (gs && !reads_set(I)) {
                    fail = true;
                    return false;
                }
                i++;
                continue;
            }
            const u32 dst = I.fdst, num = I.fnum;
            u32 cont = i + 1;
            if (((I.op == OP_BREAKC && loops) || I.op == OP_JMPC || I.op == OP_CALLC || I.op == OP_IFC) && !cond_set(I)) {
                fail = true;
                return false;
            }
            switch (I.op) {
            case OP_END:
                w.o += "    vs_output(P, o, outv, li); return;\n";
                return false;
            case OP_BREAK:
            case OP_BREAKC:
                if (!loops) break;  // outside loops: no effect
                if (!in_loop || ifs || calls) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                if (I.op == OP_BREAK) {
                    w.f("    %s += uInt[%d].z; break;\n", w.al, inc);
                    return false;
                }
                w.o += "    if (" + w.cond(I) + ") { " + w.al + " += uInt[" + std::to_string(inc) + "].z; break; }\n";
                break;
            case OP_JMPU:
            case OP_JMPC: {
                // A JMPU the variant's bools settle (an open one is written out below): one not taken does nothing, and one taken is a goto the
                // code can follow, backwards too, outside every call, if and loop, where no region ends between the two. Luigi's Mansion 2's one
                // vertex program jumps back on b14: every variant reaching one ran on the CPU, 1.6 fps on a console (Senne, darkmoon 965b3d5, 4bb53fe).
                if (I.op == OP_JMPU && decided(I) && ((b >> I.bid) & 1) != !(num & 1)) break;
                if (I.op == OP_JMPU && decided(I) && dst < i && dst < n && ends.empty() && !loops && !ifs && !calls) {
                    cont = dst;
                    break;
                }
                // to e itself, where no region ends (the end of a stretch written as an if, or of the program): the rest of it skipped
                if (dst <= i || dst > e || (dst == e && region_end(e))) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                if (I.op == OP_JMPU && decided(I)) {
                    if (((b >> I.bid) & 1) == !(num & 1)) cont = dst;
                    break;
                }
                // JMPU jumps on its bool being set, or with the low bit of num on its being clear
                const std::string jumps = I.op == OP_JMPC ? w.cond(I) : (num & 1) ? "!(" + open_bool(I) + ")" : open_bool(I);
                const Emit before = now();
                // Over a stretch that nests (nests()): the stretch when the jump isn't taken, then the code from dst once. Writing the rest
                // on both ways, as below, doubles it at each such jump after another, until the program is past what a shader can be.
                if (nests(i + 1, dst)) {
                    w.o += "    if (!(" + jumps + ")) {\n";
                    const bool r = block(i + 1, dst, in_loop, inc);
                    w.o += "    }\n";
                    join(before, true, now(), r);
                    cont = dst;
                    break;
                }
                w.o += "    if (" + jumps + ") {\n";
                const bool r1 = block(dst, e, in_loop, inc);
                const Emit jumped = now();
                set(before);
                w.o += "    } else {\n";
                const bool r2 = block(i + 1, e, in_loop, inc);
                w.o += "    }\n";
                join(jumped, r1, now(), r2);
                return r1 || r2;
            }
            case OP_CALL:
            case OP_CALLC:
            case OP_CALLU: {
                if (num == 0 || dst + num == i + 1 || dst + num > n || calls >= 3 || !ends_plain(dst, dst + num)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                if (I.op == OP_CALLU && decided(I) && !((b >> I.bid) & 1)) break;
                const bool dyn = I.op == OP_CALLC || (I.op == OP_CALLU && !decided(I));
                if (!enter(dst + num)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                const Emit before = now();
                if (dyn) w.o += "    if (" + (I.op == OP_CALLC ? w.cond(I) : open_bool(I)) + ") {\n";
                calls++;
                const bool returns = block(dst, dst + num, false, 0);
                calls--;
                ends.pop_back();
                if (dyn) {
                    w.o += "    }\n";
                    join(now(), returns, before, true);
                }
                if (cont == e && region_end(e)) {  // shader_run would not pop the enclosing region on the return
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                break;
            }
            case OP_IFU:
            case OP_IFC: {
                // Only a taken IFU pushes the PICA stack. A known-false
                // IFU can run at depth eight without consuming a ninth slot.
                const bool known = I.op == OP_IFU && decided(I);
                const bool taken = !known || ((b >> I.bid) & 1);
                if (dst < i + 1 || dst + num > e || (taken && ifs >= 8) || !ends_plain(i + 1, dst) || !ends_plain(dst, dst + num)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                cont = dst + num;
                if (taken && !enter(dst)) {  // the true branch is a region ending at dst
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                if (taken) ifs++;
                if (known) {
                    if (taken) block(i + 1, dst, false, 0);
                    else block(dst, dst + num, false, 0);
                } else {
                    const Emit before = now();
                    w.o += "    if (" + (I.op == OP_IFC ? w.cond(I) : open_bool(I)) + ") {\n";
                    const bool r1 = block(i + 1, dst, false, 0);
                    const Emit then = now();
                    set(before);
                    bool r2 = true;
                    if (num) {
                        w.o += "    } else {\n";
                        ends.pop_back();
                        // A false IFC/IFU jumps straight to its else branch;
                        // it never pushes the then branch's IF stack entry.
                        ifs--;
                        r2 = block(dst, dst + num, false, 0);
                        ifs++;
                        ends.push_back(dst);
                    }
                    w.o += "    }\n";
                    join(then, r1, now(), r2);
                }
                if (taken) { ends.pop_back(); ifs--; }
                if (cont == e && cont != i + 1 && region_end(e)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                break;
            }
            case OP_LOOP: {
                if (dst < i + 1 || dst + 1 > e || loops >= 3 || !(plain(P->ins[dst]) || P->ins[dst].op == OP_BREAKC)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                if (!enter(dst + 1)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                if (gs && emits_in(i + 1, dst + 1)) {
                    const bool made = emitting_loop(I, i + 1, dst + 1);
                    ends.pop_back();
                    cont = dst + 1;
                    if (!made || (cont == e && region_end(e))) {
                        fail = true;
                        return false;
                    }
                    break;
                }
                // The count, start and step come from the uniform block (uInt, the integer uniforms the key already fixes), not the number itself:
                // a bound the compiler can see gets the loop unrolled, and a 7-turn body of relative reads never came out of CafeGLSL on a console.
                // Counted with signed numbers (the count is a byte): from an unsigned compare the console's compiler makes PRED_SETGE_UINT, which
                // no program the PC's compiler makes holds and Cemu doesn't know, so there the draws of a shader compiled on the console vanished.
                const int k = counter++;
                w.f("    %s = uInt[%u].y;\n    for (int it%d = 0; it%d <= uInt[%u].x; it%d++) {\n", w.al, I.iid, k, k, I.iid, k);
                loops++;
                const int saved_ifs = ifs, saved_calls = calls;
                ifs = calls = 0;  // a BREAKC directly in this body ends this loop
                const Emit before = now();
                block(i + 1, dst + 1, true, I.iid);
                ifs = saved_ifs;
                calls = saved_calls;
                loops--;
                ends.pop_back();
                w.f("    %s += uInt[%u].z;\n    }\n", w.al, I.iid);
                if (loops) w.f("    %s = uInt[%u].y;\n", w.al, I.iid);  // shader_run restores the counter it saved: this loop's start
                // a BREAKC may have left the body before any of it: only what was set before the loop is sure (emits stay out of loops)
                join(before, true, now(), true);
                es.written[0] = before.written[0];
                es.written[1] = before.written[1];
                es.temps = before.temps;
                es.flags = before.flags;
                cont = dst + 1;
                if (cont == e && region_end(e)) {
                    fail = true; why = __LINE__; at = i;
                    return false;
                }
                break;
            }
            case OP_SETEMIT:
                if (!gs) {
                    fail = true;
                    return false;
                }
                es.vid = I.vid;
                es.prim = I.prim;
                es.wind = I.wind;
                break;
            case OP_EMIT:
                if (!gs || (loops && !emitting) || !emit()) {
                    fail = true;
                    return false;
                }
                break;
            default:
                fail = true;
                return false;
            }
            i = cont;
        }
        return !fail;
    }
};
}  // namespace

static u32 inputs_read(const ShaderProgram *P) {
    u32 in_used = 0;
    for (const DInst &I : P->ins) {
        const int ns = I.op == OP_MAD ? 3 : 2;
        if (I.op <= OP_MAD)
            for (int k = 0; k < ns; k++)
                if (I.src[k] < REG_TMP) in_used |= 1u << I.src[k];
    }
    return in_used;
}

// The Metal-flavoured text SpecGen writes, as GLSL. gs: a geometry shader's (shader_glsl_gs), whose END ends its own function.
static std::string glsl_text(std::string s, bool gs) {
    auto replace_all = [&](const char *from, const char *to) {
        const std::string f = from, t = to;
        for (size_t at = s.find(f); at != std::string::npos; at = s.find(f, at + t.size())) s.replace(at, f.size(), t);
    };
    replace_all("vs_output(P, o, outv, li); return;", gs ? "return;" : "vs_output(); return;");
    if (gs) replace_all("uInt[", "gInt[");
    replace_all("const float4", "vec4");
    replace_all("float4(1.0 / ", "vs_lim(1.0 / ");  // gen_vs_glsl says why
    replace_all("float4", "vec4");
    replace_all("precise::", "");
    replace_all("P.f[", "f[");
    replace_all("P.gf[", "gf[");
    replace_all("vs_rel(P, ", "vs_rel(");
    replace_all("gs_rel(P, ", "gs_rel(");
    replace_all("gs_ru(P, ", "gs_ru(");
    replace_all("select(vec4(0.0), vec4(1.0), ", "SEL(");
    replace_all("uint it", "int it");
    for (char op : {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}) {
        const std::string u = std::string(1, op) + "u; it";
        replace_all(u.c_str(), (std::string(1, op) + "; it").c_str());
    }
    // SEL(a >= b) and SEL(a < b): GLSL compares vectors with functions
    for (size_t at = s.find("SEL("); at != std::string::npos; at = s.find("SEL(", at)) {
        size_t end = at + 4;
        for (int depth = 1; end < s.size(); end++) {
            if (s[end] == '(') depth++;
            if (s[end] == ')' && --depth == 0) break;
        }
        end++;
        const std::string inner = s.substr(at + 4, end - at - 5);
        size_t op = inner.find(" >= ");
        const bool ge = op != std::string::npos;
        if (!ge) op = inner.find(" < ");
        const std::string lhs = inner.substr(0, op), rhs = inner.substr(op + (ge ? 4 : 3));
        s.replace(at, end - at, std::string(ge ? "sel_ge(" : "sel_lt(") + lhs + ", " + rhs + ")");
    }
    return s;
}

// The body of main() for GLSL (port/wiiu/shadergen.cpp declares the registers and helpers).
int shader_glsl_fail_line;  // why the last shader_glsl_spec gave up: the line in this file, and the instruction
u32 shader_glsl_fail_at;

std::string shader_glsl_spec(const ShaderProgram *P, u32 entry, u32 b, const u8 (*iu)[4], u32 *inputs, u32 open) {
    const u32 n = (u32)P->ins.size();
    if (entry >= n) return "";
    *inputs = inputs_read(P);
    MslWriter w{P};
    w.al = "aL";
    SpecGen g{w, P, n, b, iu, open};
    g.block(entry, n, false, 0);
    if (g.fail) {
        shader_glsl_fail_line = g.why;
        shader_glsl_fail_at = g.at;
        return "";
    }
    return glsl_text(w.o, false);
}

std::string shader_glsl_gs(const ShaderProgram *P, u32 entry, u32 b, const u8 (*iu)[4], u64 out_comps, u32 in_regs, bool turns, u32 *tris,
                           u32 prim_lo, u32 prim_hi) {
    const u32 n = (u32)P->ins.size();
    if (entry >= n || !out_comps) return "";
    MslWriter w{P};
    w.al = "gaL";
    w.pre = "g";
    w.uni = "gf";
    w.bank = 0;
    w.prim_lo = prim_lo;
    w.prim_hi = prim_hi;
    SpecGen g{w, P, n, b, iu, 0};
    g.gs = true;
    g.gs_need = out_comps;
    for (u32 r = 0; r < 16; r++)
        if ((out_comps >> (4 * r)) & 15) g.gs_out |= 1u << r;
    g.gs_in = in_regs;
    g.gs_wind = turns;
    g.block(entry, n, false, 0);
    if (g.fail || !g.tris) return "";
    *tris = g.tris;
    return glsl_text(w.o, true);
}

std::string shader_msl_spec(const ShaderProgram *P, u32 entry, const char *name, u32 b, const u8 (*iu)[4]) {
    const u32 n = (u32)P->ins.size();
    if (entry >= n) return "";
    MslWriter w{P};
    w.prologue(name);
    w.al = "aL";
    w.o += "  int aL = 0;\n";
    SpecGen g{w, P, n, b, iu};
    if (g.block(entry, n, false, 0)) w.o += "  vs_output(P, o, outv, li);\n";
    if (g.fail) return "";
    w.o += "}\n";
    return w.o;
}

}  // namespace gpu
