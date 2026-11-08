// Generated code includes "cpu.h". This one finds the upstream header, then makes guest memory little-endian on a big-endian
// host. lwbrx and friends take base+index, so M + addr costs one instruction, the same as a native access.
#pragma once
#include_next <cpu.h>

#if defined(__BIG_ENDIAN__)
#undef RD16
#undef RD32
#undef WR16
#undef WR32

// %y prints a memory operand in the indexed form these instructions require; Z constrains it to that form.
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
static inline void ppc_wr32(u8 *m, u32 a, u32 v) {
    __asm__("stwbrx %1,%y0" : "=Z"(*(u32_u *)(m + a)) : "r"(v));
}
static inline void ppc_wr16(u8 *m, u32 a, u32 v) {
    __asm__("sthbrx %1,%y0" : "=Z"(*(u16_u *)(m + a)) : "r"(v));
}

#define RD16(a) ppc_rd16(M, (u32)(a))
#define RD32(a) ppc_rd32(M, (u32)(a))
#define WR16(a, v) ppc_wr16(M, (u32)(a), (u32)(v))
#define WR32(a, v) ppc_wr32(M, (u32)(a), (u32)(v))
#endif
