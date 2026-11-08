// A timed condition wait that can't touch a dead stack frame. wut's __syscall_cond_wait keeps its alarm and its data on the waiter's
// stack and cancels the alarm when it wakes, but an alarm already firing still runs afterwards, on data that's gone, and signals
// whatever it finds there. Cemu crashes on that within seconds in games that wait with timeouts a lot; the console could too.
// Here the alarms live in a static table, and a waiter doesn't leave while its alarm's callback is running. The alarm repeats after
// the timeout, so a signal that arrives just before the waiter sleeps can't be lost for good.
// Only with -DWIIU_COND_WRAP and -Wl,--wrap=__syscall_cond_wait at link time.
#ifdef WIIU_COND_WRAP
#include <coreinit/alarm.h>
#include <coreinit/spinlock.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <stdint.h>
#include <sys/lock.h>

extern "C" {
int __real___syscall_cond_wait(_COND_T *cond, _LOCK_T *lock, uint64_t timeout_ns);
int __syscall_cond_signal(_COND_T *cond);
}

namespace {
struct Slot {
    OSAlarm alarm;
    _COND_T *cond;
    bool created, used, active, busy, timed_out;
};
Slot slots[48];
OSSpinLock table_lock;

void on_alarm(OSAlarm *alarm, OSContext *) {
    auto *s = (Slot *)OSGetAlarmUserData(alarm);
    if (!s) return;
    OSUninterruptibleSpinLock_Acquire(&table_lock);
    _COND_T *c = s->active ? s->cond : nullptr;
    if (c) {
        s->busy = true;
        s->timed_out = true;
    }
    OSUninterruptibleSpinLock_Release(&table_lock);
    if (!c) return;
    __syscall_cond_signal(c);
    OSUninterruptibleSpinLock_Acquire(&table_lock);
    s->busy = false;
    OSUninterruptibleSpinLock_Release(&table_lock);
}
}  // namespace

extern "C" int __wrap___syscall_cond_wait(_COND_T *cond, _LOCK_T *lock, uint64_t timeout_ns) {
    if (timeout_ns == UINT64_MAX) return __real___syscall_cond_wait(cond, lock, timeout_ns);
    if (!timeout_ns) return ETIMEDOUT;
    if (!*cond) __syscall_cond_signal(cond);  // creates it here, not in the alarm callback
    Slot *s = nullptr;
    OSUninterruptibleSpinLock_Acquire(&table_lock);
    for (Slot &x : slots)
        if (!x.used) {
            s = &x;
            s->used = true;
            s->cond = cond;
            s->active = true;
            s->timed_out = false;
            break;
        }
    OSUninterruptibleSpinLock_Release(&table_lock);
    if (!s) return __real___syscall_cond_wait(cond, lock, timeout_ns);  // table full: wut's own, as before
    if (!s->created) {
        OSCreateAlarm(&s->alarm);
        OSSetAlarmUserData(&s->alarm, s);
        s->created = true;
    }
    OSSetPeriodicAlarm(&s->alarm, OSGetTime() + (OSTime)OSNanosecondsToTicks(timeout_ns), OSMillisecondsToTicks(1), on_alarm);
    __real___syscall_cond_wait(cond, lock, UINT64_MAX);
    OSCancelAlarm(&s->alarm);
    bool timed_out;
    for (;;) {
        OSUninterruptibleSpinLock_Acquire(&table_lock);
        if (!s->busy) {
            s->active = false;
            timed_out = s->timed_out;
            s->used = false;
            OSUninterruptibleSpinLock_Release(&table_lock);
            break;
        }
        OSUninterruptibleSpinLock_Release(&table_lock);
        OSYieldThread();
    }
    return timed_out ? ETIMEDOUT : 0;
}
#endif
