// The runtime's thread backtrace uses Mach calls. On this PC build those calls simply report failure, so backtraces are empty.
#pragma once
#include <cstdint>
#include <pthread.h>
typedef unsigned mach_port_t;
typedef unsigned mach_msg_type_number_t;
typedef int *thread_state_t;
struct arm_thread_state64_t { uint64_t pc, lr, fp; };
enum { KERN_SUCCESS = 0, ARM_THREAD_STATE64_COUNT = 0, ARM_THREAD_STATE64 = 0 };
inline mach_port_t pthread_mach_thread_np(pthread_t) { return 0; }
inline int thread_suspend(mach_port_t) { return 1; }
inline int thread_resume(mach_port_t) { return 1; }
inline int thread_get_state(mach_port_t, int, thread_state_t, mach_msg_type_number_t *) { return 1; }
inline uint64_t arm_thread_state64_get_pc(const arm_thread_state64_t &s) { return s.pc; }
inline uint64_t arm_thread_state64_get_lr(const arm_thread_state64_t &s) { return s.lr; }
inline uint64_t arm_thread_state64_get_fp(const arm_thread_state64_t &s) { return s.fp; }
