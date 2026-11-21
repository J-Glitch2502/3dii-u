"""Recognize one exact ARM aligned-copy implementation, including its tail.

Checking the whole routine avoids treating a similar block with different flag
or register semantics as memcpy. The generated fast path retains the original
prologue, stack writes, tail handling, register restoration and return.
"""

COPY32_WORDS = (
    0xe92d47f0, 0xe2522020, 0x3a000009, 0xe8b10078, 0xf5d1f040,
    0xe8b10780, 0xe8a00078, 0xe2522020, 0xe8a00780, 0x28b10078,
    0xf5d1f040, 0x28b10780, 0x2afffff8, 0xe1b0ce02, 0x28b15018,
    0x28a05018, 0x48b10018, 0x48a00018, 0xe8bd47f0, 0xe1b0cf02,
    0x24913004, 0x24803004, 0x012fff1e, 0xe1b02f82, 0x20d130b2,
    0x44d12001, 0x20c030b2, 0x44c02001, 0xe12fff1e,
)


def matches_copy32(image, start, addrs):
    if image is None or len(addrs) != len(COPY32_WORDS) or addrs != list(range(start, start + 0x74, 4)):
        return False
    return all(image.read32(start + 4 * i) == word for i, word in enumerate(COPY32_WORDS))


def copy32_fastpath(start, writes_vf=True):
    # r2 has already had 32 subtracted. The conditional branch for sizes <32
    # remains outside this block. Restrict the native operation to 1 MiB, avoid
    # overlap (including the saved stack frame), and honor a pending preemption
    # by falling back to the original loop with its per-iteration poll.
    return '''#if defined(__WIIU__) || defined(__DEVKITPPC__)
  if (__atomic_load_n(&rt_bulk_copy_enabled, __ATOMIC_RELAXED) && !rt_preempt_flag && r2 <= 0xfffe0u) {
    const u32 bytes = (r2 & ~31u) + 32u;
    if (r0 <= 0xffffffffu - bytes && r1 <= 0xffffffffu - bytes &&
        (r0 + bytes <= r1 || r1 + bytes <= r0) &&
        (r0 + bytes <= r13 || (r13 <= 0xffffffdfu && r0 >= r13 + 32u))) {
      __builtin_memcpy(M + r0, M + r1, bytes);
      r3 = RD32(r1 + bytes - 32u);
      r0 += bytes; r1 += bytes; r2 -= bytes;
      cf = 0; %s
      goto L_%x;
    }
  }
#endif''' % ('vf = 0;' if writes_vf else '', start + 0x34)
