// The benchmark's guest program: my own code, no game code. The same source is compiled for ARM11 (then recompiled to
// C) and natively, so the two can be compared. Results must not depend on byte order, except k_unaligned.
typedef unsigned char u8;
typedef short s16;
typedef int s32;
typedef unsigned int u32;
typedef long long s64;

#define NOINLINE __attribute__((noinline))

static u32 fbits(float f) {
    union { float f; u32 u; } x;
    x.f = f;
    return x.u;
}

static u32 lfsr(u32 v) { return (v >> 1) ^ (-(v & 1u) & 0xD0000001u); }

// ---- pointer chasing and virtual calls: the shape of most game logic
typedef struct Ent {
    struct Ent *next;
    u32 (*update)(struct Ent *, u32);
    s32 x, y, vx, vy;
    float px, py, speed;
    u32 state, hp;
} Ent;

#define NENT 128
static Ent ents[NENT];

static NOINLINE u32 upd_walk(Ent *e, u32 t) {
    e->x += e->vx;
    if (e->x > 1000 || e->x < -1000) e->vx = -e->vx;
    e->px += e->speed;
    return e->state + t;
}
static NOINLINE u32 upd_jump(Ent *e, u32 t) {
    e->y += e->vy;
    e->vy -= 1;
    if (e->y < 0) { e->y = 0; e->vy = (s32)(t & 15) + 4; e->state ^= 1; }
    e->py = e->py * 0.5f + e->speed;
    return e->hp;
}
static NOINLINE u32 upd_idle(Ent *e, u32 t) {
    if ((t & 7) == 0 && e->hp) e->hp--;
    return e->hp ^ e->state;
}

static NOINLINE u32 k_entities(u32 iters) {
    static u32 (*const kinds[3])(Ent *, u32) = {upd_walk, upd_jump, upd_idle};
    u32 r = 0x1234567u, sum = 0;
    for (u32 i = 0; i < NENT; i++) {
        Ent *e = &ents[i];
        r = lfsr(r);
        e->next = &ents[(i + 37) % NENT];
        e->update = kinds[r % 3];
        e->x = (s32)(r & 255) - 128;
        e->y = (s32)((r >> 8) & 63);
        e->vx = (s32)(r & 3) - 1;
        e->vy = 3;
        e->px = (float)(r & 1023);
        e->py = 1.0f;
        e->speed = 0.25f + (float)(r & 7);
        e->state = r >> 28;
        e->hp = 100 + (r & 31);
    }
    for (u32 t = 0; t < iters; t++) {
        Ent *e = &ents[0];
        for (u32 i = 0; i < NENT; i++) {
            sum += e->update(e, t);
            e = e->next;
        }
    }
    for (u32 i = 0; i < NENT; i++) sum = sum * 31 + (u32)ents[i].x + fbits(ents[i].px);
    return sum;
}

// ---- matrices and vertex transforms: VFP loads, stores and arithmetic, with some square roots
#define NVERT 128
static float verts[NVERT][4];
static float out[NVERT][4];

static NOINLINE void mat_mul(float *d, const float *a, const float *b) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            d[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

static NOINLINE u32 k_math(u32 iters) {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float rot[16] = {0.998f, -0.05f, 0, 0, 0.05f, 0.998f, 0, 0, 0, 0, 1, 0, 0.125f, 0.25f, 0, 1};
    float tmp[16];
    u32 r = 0xACE1u, sum = 0;
    for (int i = 0; i < NVERT; i++) {
        for (int j = 0; j < 3; j++) {
            r = lfsr(r);
            verts[i][j] = (float)(s32)(r & 2047) / 256.0f - 4.0f;
        }
        verts[i][3] = 1.0f;
    }
    for (u32 t = 0; t < iters; t++) {
        mat_mul(tmp, m, rot);
        for (int i = 0; i < 16; i++) m[i] = tmp[i];
        for (int i = 0; i < NVERT; i++) {
            const float *v = verts[i];
            float *o = out[i];
            o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3];
            o[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7] * v[3];
            o[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11] * v[3];
            o[3] = m[12] * v[0] + m[13] * v[1] + m[14] * v[2] + m[15] * v[3];
            if ((i & 15) == 0) {
                float len = __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                if (len > 0.0001f) o[3] = 1.0f / len;
            }
        }
        sum = sum * 33 + fbits(out[t % NVERT][0]) + fbits(out[t % NVERT][3]);
    }
    return sum;
}

// ---- memory copies and byte hashing: file parsing and string handling
#define NBYTES 8192
static u8 buf_a[NBYTES], buf_b[NBYTES];

static NOINLINE void copy_bytes(u8 *d, const u8 *s, u32 n) {
    while (n >= 4) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        d += 4; s += 4; n -= 4;
    }
    while (n--) *d++ = *s++;
}

static NOINLINE u32 k_bytes(u32 iters) {
    u32 r = 0xBEEFu, h = 2166136261u;
    for (u32 i = 0; i < NBYTES; i++) {
        r = lfsr(r);
        buf_a[i] = (u8)r;
    }
    for (u32 t = 0; t < iters; t++) {
        copy_bytes(buf_b, buf_a + (t & 7), NBYTES - 8);
        for (u32 i = 0; i < NBYTES - 8; i++) h = (h ^ buf_b[i]) * 16777619u;
    }
    return h;
}

// ---- sorting through a comparison callback: indirect calls and branch-heavy integer code
#define NSORT 512
static u32 items[NSORT];

static NOINLINE int cmp_asc(u32 a, u32 b) { return a < b ? -1 : a > b; }

static void quick(u32 *v, int lo, int hi, int (*cmp)(u32, u32)) {
    while (lo < hi) {
        u32 p = v[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (cmp(v[i], p) < 0) i++;
            while (cmp(v[j], p) > 0) j--;
            if (i <= j) { u32 x = v[i]; v[i] = v[j]; v[j] = x; i++; j--; }
        }
        if (j - lo < hi - i) { quick(v, lo, j, cmp); lo = i; }
        else { quick(v, i, hi, cmp); hi = j; }
    }
}

static NOINLINE u32 k_sort(u32 iters) {
    u32 r = 0xC0FFEEu, sum = 0;
    for (u32 t = 0; t < iters; t++) {
        for (int i = 0; i < NSORT; i++) {
            r = lfsr(r);
            items[i] = r;
        }
        quick(items, 0, NSORT - 1, cmp_asc);
        sum += items[t % NSORT];
    }
    return sum;
}

// ---- deep direct calls: return-address checks are the recompiler's per-call cost
static NOINLINE u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static NOINLINE u32 k_calls(u32 iters) {
    u32 sum = 0;
    for (u32 t = 0; t < iters; t++) sum += fib(14 + (t & 1));
    return sum;
}

// ---- audio-style mixing: 64-bit multiplies, saturation and division
#define NMIX 1024
static s16 mix_in[NMIX];
static s32 mix_out[NMIX];

static NOINLINE u32 k_mix(u32 iters) {
    u32 r = 0x51F15Eu, sum = 0;
    for (int i = 0; i < NMIX; i++) {
        r = lfsr(r);
        mix_in[i] = (s16)r;
    }
    for (u32 t = 0; t < iters; t++) {
        s32 vol = (s32)(0x8000u + (t & 0x3FFFu));
        for (int i = 0; i < NMIX; i++) {
            s32 v = (s32)(((s64)mix_in[i] * vol * 3) >> 15);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            mix_out[i] = v;
        }
        sum += (u32)(mix_out[t % NMIX] / (s32)(1 + (t & 15))) + (u32)mix_out[(t * 7) % NMIX];
    }
    return sum;
}

// ---- unaligned loads and stores, which ARM11 allows: tests the byte-reversing path on real hardware
typedef struct __attribute__((packed)) { u32 v; } Packed32;
typedef struct __attribute__((packed)) { unsigned short v; } Packed16;

static NOINLINE u32 k_unaligned(u32 iters) {
    u32 r = 0x7777u, sum = 0;
    for (u32 i = 0; i < 1024; i++) {
        r = lfsr(r);
        buf_a[i] = (u8)r;
    }
    for (u32 t = 0; t < iters; t++) {
        for (u32 i = 1; i < 1000; i += 3) {
            sum = sum * 3 + ((const Packed32 *)(buf_a + i))->v + ((const Packed16 *)(buf_a + i + 2))->v;
            ((Packed32 *)(buf_b + i))->v = sum;
        }
    }
    return sum;
}

__attribute__((section(".text.start"))) u32 guest_main(u32 kernel, u32 iters) {
    switch (kernel) {
    case 0: return k_entities(iters);
    case 1: return k_math(iters);
    case 2: return k_bytes(iters);
    case 3: return k_sort(iters);
    case 4: return k_calls(iters);
    case 5: return k_mix(iters);
    case 6: return k_unaligned(iters);
    default: return 0;
    }
}
