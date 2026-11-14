// Checks, on a PC, the native geometry shaders tools/pica_to_c.py made against the PICA interpreter (shader.cpp): every program in the
// .gsl logs given that has a native function is run both ways on random uniforms, and every primitive each emits has to come out the
// same, bit for bit. Build and run: python tools/test_gs_native.py <game's shader_logs folder>.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "gpu/pica.h"

using namespace gpu;

extern "C" {
typedef void (*gs_emit_fn)(void *ctx, unsigned vid, unsigned prim, unsigned wind);
typedef int (*gs_native_fn)(float (*r)[4], unsigned *bank, unsigned char *cond, const unsigned char *b, gs_emit_fn emit, void *ctx);
struct gs_native {
    unsigned long long code_hash, swizzle_hash;
    unsigned entry, end, nswizzles;
    const unsigned short *swizzles;
    gs_native_fn fn;
    unsigned independent, outmask;
};
extern const gs_native gs_native_table[];
extern const unsigned gs_native_count;
}

struct Prim {
    V4 v[3][16];
    bool winding;
};
static std::vector<Prim> *sink;
static u32 outputs;
static void capture(Emitter *e) {
    Prim p;
    memcpy(p.v, e->buf, sizeof p.v);
    p.winding = e->winding;
    sink->push_back(p);
}

static void native_emit(void *ctx, unsigned vid, unsigned prim, unsigned wind) {
    ShaderUnit &u = *(ShaderUnit *)ctx;
    Emitter *e = u.emitter;
    e->vertex_id = vid;
    e->prim_emit = prim;
    e->winding = wind;
    if (vid < 3) {
        u32 oi = 0;
        for (u32 reg = 0; reg < 16; reg++)
            if (e->output_mask & (1u << reg)) e->buf[vid][oi++] = u.r[REG_OUT + u.out_bank + reg];
        if (prim && e->emit_prim) e->emit_prim(e);
    }
    u.out_bank ^= 16;
}

int main(int argc, char **argv) {
    int failures = 0, checked = 0;
    size_t prims = 0;
    std::mt19937 rng(1234);
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) continue;
        std::vector<u8> d;
        for (int c; (c = fgetc(f)) != EOF;) d.push_back((u8)c);
        fclose(f);
        for (size_t p = 0; p + 16 <= d.size() && !memcmp(&d[p], "GS  ", 4);) {
            u32 entry, nc, ns;
            memcpy(&entry, &d[p + 4], 4);
            memcpy(&nc, &d[p + 8], 4);
            memcpy(&ns, &d[p + 12], 4);
            static ShaderSetup s;
            memset(&s, 0, sizeof s);
            memcpy(s.code, &d[p + 16], nc * 4);
            memcpy(s.swizzle, &d[p + 16 + nc * 4], ns * 4);
            s.code_size = nc;
            s.swizzle_size = ns;
            s.dirty = true;
            p += 16 + (nc + ns) * 4;
            for (u32 t = 0; t < gs_native_count; t++) {
                if (gs_native_table[t].entry != entry) continue;
                const ShaderProgram *prog = shader_get_program(s);
                int bad = 0;
                for (int run = 0; run < 2000 && bad < 5; run++) {
                    std::uniform_real_distribution<float> big(-200.0f, 200.0f), small(0.0f, 2.0f);
                    for (u32 i = 0; i < 96; i++)
                        for (u32 k = 0; k < 4; k++) s.f[i].v[k] = (run & 1) ? big(rng) : small(rng);
                    s.f[95] = V4{{0.0f, 0.5f, 1.0f, (run % 7 == 0) ? s.f[13].v[0] : 0.0f}};  // the program's constants, and a skipped primitive now and then
                    for (u32 i = 0; i < 16; i++) s.b[i] = (rng() & 1) != 0;
                    s.b[14] = run % 11 != 0;  // the bool its stretch needs, missing now and then: the native one must decline
                    std::vector<Prim> want, got;
                    // the third way: native again, on a register file full of another run's leavings, which a program the generator
                    // calls independent must not notice (the runtime then runs it on two threads: vertex.cpp's gs_run_shared)
                    for (int way = 0; way < (gs_native_table[t].independent ? 3 : 2); way++) {
                        static ShaderUnit u;
                        static Emitter e;
                        memset(&u, 0, sizeof u);
                        memset(&e, 0, sizeof e);
                        shader_unit_init(u, s);
                        if (way == 2) {
                            if (!s.b[14]) break;
                            for (u32 i = REG_TMP; i < REG_UNI; i++)
                                for (u32 k = 0; k < 4; k++) u.r[i].v[k] = (float)(rng() % 1000) - 500.0f;
                            for (u32 i = REG_OUT; i < REG_OUT + 32; i++)
                                for (u32 k = 0; k < 4; k++) u.r[i].v[k] = (float)(rng() % 1000) - 500.0f;
                            u.out_bank = (rng() & 1) * 16;
                            got.clear();
                        }
                        e.output_mask = 0x3f & gs_native_table[t].outmask;
                        e.emit_prim = capture;
                        u.emitter = &e;
                        sink = way ? &got : &want;
                        if (way == 0) {
                            shader_run(prog, u, s, entry);
                        } else if (!gs_native_table[t].fn((float (*)[4])u.r, &u.out_bank, (unsigned char *)u.cond, (const unsigned char *)s.b,
                                                           native_emit, &u)) {
                            got = want;  // declined: the interpreter would run it
                            if (s.b[14]) {
                                printf("entry %#x run %d: declined with its bool set\n", entry, run);
                                bad++;
                            }
                        }
                    }
                    if (!s.b[14]) continue;
                    bool same = want.size() == got.size();
                    for (size_t i = 0; same && i < want.size(); i++)
                        same = !memcmp(want[i].v, got[i].v, sizeof want[i].v) && want[i].winding == got[i].winding;
                    if (!same) {
                        printf("entry %#x run %d: %zu primitives from the interpreter, %zu native, and they differ\n", entry, run, want.size(), got.size());
                        bad++;
                    }
                    checked++;
                    prims += want.size();
                }
                failures += bad;
            }
        }
    }
    printf("%d runs compared (%zu primitives), %d differ\n", checked, prims, failures);
    return failures ? 1 : 0;
}
