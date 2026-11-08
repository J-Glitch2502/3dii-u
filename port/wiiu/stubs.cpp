// What the Wii U build leaves out: cloud saves, and the desktop-only hooks of the original frontend.
#include <atomic>
#include <string>

#include "rt.h"

void cloud_deleted() {}
void cloud_restored() {}
void cloud_save_written() {}
std::atomic<u32> g_live_host;
std::atomic<int> g_winshot_request;
void watch_report() {}

namespace stacks {
void save_written(const std::string &) {}
}

// The C library here has neither of these. Reads and writes from several threads can't interleave a seek with another's read.
#include <coreinit/spinlock.h>
#include <unistd.h>

#include <mutex>

static std::mutex file_lock;
extern "C" ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    std::lock_guard<std::mutex> lk(file_lock);
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    return read(fd, buf, n);
}
extern "C" ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    std::lock_guard<std::mutex> lk(file_lock);
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    return write(fd, buf, n);
}

// 64-bit atomics: the 750 has none, and there's no libatomic. One lock is enough for the few counters that use them.
static OSSpinLock atomic_lock;
struct AtomicGuard {
    AtomicGuard() { OSUninterruptibleSpinLock_Acquire(&atomic_lock); }
    ~AtomicGuard() { OSUninterruptibleSpinLock_Release(&atomic_lock); }
};
extern "C" {
u64 atomic_load8(const volatile void *p, int) __asm__("__atomic_load_8");
void atomic_store8(volatile void *p, u64 v, int) __asm__("__atomic_store_8");
bool atomic_cas8(volatile void *p, void *expected, u64 desired, int, int) __asm__("__atomic_compare_exchange_8");
u64 atomic_add8(volatile void *p, u64 v, int) __asm__("__atomic_fetch_add_8");
u64 atomic_load8(const volatile void *p, int) {
    AtomicGuard g;
    return *(const volatile u64 *)p;
}
void atomic_store8(volatile void *p, u64 v, int) {
    AtomicGuard g;
    *(volatile u64 *)p = v;
}
bool atomic_cas8(volatile void *p, void *expected, u64 desired, int, int) {
    AtomicGuard g;
    if (*(volatile u64 *)p == *(u64 *)expected) {
        *(volatile u64 *)p = desired;
        return true;
    }
    *(u64 *)expected = *(volatile u64 *)p;
    return false;
}
u64 atomic_add8(volatile void *p, u64 v, int) {
    AtomicGuard g;
    const u64 old = *(volatile u64 *)p;
    *(volatile u64 *)p = old + v;
    return old;
}
}

// Debug aid (link with --wrap=__cxa_atexit --wrap=__cxa_guard_acquire): shows how far static construction gets, in Cemu's log.
#ifdef WIIU_TRACE_CTORS
#include <coreinit/debug.h>
extern "C" {
int __real___cxa_atexit(void (*f)(void *), void *a, void *d);
int __wrap___cxa_atexit(void (*f)(void *), void *a, void *d) {
    OSReport("ATEXIT %p;", __builtin_return_address(0));
    return __real___cxa_atexit(f, a, d);
}
int __real___cxa_guard_acquire(void *g);
int __wrap___cxa_guard_acquire(void *g) {
    OSReport("GUARD %p;", __builtin_return_address(0));
    const int r = __real___cxa_guard_acquire(g);
    OSReport("GUARD OK;");
    return r;
}
}
#endif
