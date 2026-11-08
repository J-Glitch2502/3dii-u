// What recompiled code needs from its host. Guest memory is little-endian on every host: on the big-endian Wii U,
// lwbrx and friends take base+index, so M + addr costs one instruction, the same as a native access.
#pragma once
#include <math.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

// The Wii U's GCC guarantees tail calls. Older GCCs are only used for checks, where -O2 still makes them tail calls.
#if __GNUC__ >= 15
#define MUSTTAIL __attribute__((musttail))
#else
#define MUSTTAIL
#endif

typedef struct CPU {
    u32 r[16];
    u32 pc;  // where a return or indirect jump is headed
    float s[32];
} CPU;

typedef void (*GuestFn)(CPU *);
typedef struct {
    u32 addr;
    GuestFn fn;
} GuestEntry;

extern u8 *g_mem;
extern volatile u32 rt_preempt_flag;
void rt_dispatch(CPU *c);
void rt_preempt(CPU *c);
void rt_bad_jump(CPU *c, u32 target, u32 from);

typedef u16 __attribute__((aligned(1), may_alias)) u16_u;
typedef u32 __attribute__((aligned(1), may_alias)) u32_u;

#define RD8(a) (*(const u8 *)(M + (u32)(a)))
#define WR8(a, v) (*(u8 *)(M + (u32)(a)) = (u8)(v))

#if defined(__BIG_ENDIAN__)
static inline u32 ppc_rd32(const u8 *m, u32 a) {
    u32 v;
    __asm__("lwbrx %0,%y1" : "=r"(v) : "Z"(*(const u32_u *)(m + a)));
    return v;
}
static inline u32 ppc_rd16(const u8 *m, u32 a) {
    u32 v;
    __asm__("lhbrx %0,%y1" : "=r"(v) : "Z"(*(const u16_u *)(m + a)));
    return v;
}
static inline void ppc_wr32(u8 *m, u32 a, u32 v) { __asm__("stwbrx %1,%y0" : "=Z"(*(u32_u *)(m + a)) : "r"(v)); }
static inline void ppc_wr16(u8 *m, u32 a, u32 v) { __asm__("sthbrx %1,%y0" : "=Z"(*(u16_u *)(m + a)) : "r"(v)); }
#define RD16(a) ppc_rd16(M, (u32)(a))
#define RD32(a) ppc_rd32(M, (u32)(a))
#define WR16(a, v) ppc_wr16(M, (u32)(a), (u32)(v))
#define WR32(a, v) ppc_wr32(M, (u32)(a), (u32)(v))
#else
#define RD16(a) (*(const u16_u *)(M + (u32)(a)))
#define RD32(a) (*(const u32_u *)(M + (u32)(a)))
#define WR16(a, v) (*(u16_u *)(M + (u32)(a)) = (u16)(v))
#define WR32(a, v) (*(u32_u *)(M + (u32)(a)) = (u32)(v))
#endif

static inline u32 F2U(float f) {
    union { float f; u32 u; } x;
    x.f = f;
    return x.u;
}
static inline float U2F(u32 u) {
    union { float f; u32 u; } x;
    x.u = u;
    return x.f;
}
static inline u32 clz32(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
static inline u32 lsl_r(u32 x, u32 a) { a &= 255; return a >= 32 ? 0 : x << a; }
static inline u32 lsr_r(u32 x, u32 a) { a &= 255; return a >= 32 ? 0 : x >> a; }
static inline u32 asr_r(u32 x, u32 a) { a &= 255; return (u32)((s32)x >> (a >= 32 ? 31 : a)); }
static inline u32 ror_r(u32 x, u32 a) { a &= 31; return a ? (x >> a) | (x << (32 - a)) : x; }
// VFP's float-to-int conversions saturate, and NaN becomes 0.
static inline u32 f2s32z(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 0x7FFFFFFFu;
    if (f < -2147483648.0f) return 0x80000000u;
    return (u32)(s32)f;
}
static inline u32 f2u32z(float f) {
    if (f != f || f <= -1.0f) return 0;
    if (f >= 4294967296.0f) return 0xFFFFFFFFu;
    return (u32)f;
}
