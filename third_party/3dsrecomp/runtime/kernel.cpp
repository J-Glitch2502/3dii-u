// Kernel objects, scheduler and SVCs.
#include "kernel.h"

#ifdef __WIIU__
#include <coreinit/event.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
namespace wiiu { bool av_timing_enabled(); }
#else
#include <dlfcn.h>
#endif
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>

#include "loader.h"
#include "service.h"
#include "services/services.h"

extern u64 rt_lookups, rt_lookup_misses;

namespace kernel {

std::mutex kmutex;
static std::map<Handle, ObjPtr> handles;
static Handle next_handle = 0x00010001;
static std::vector<std::shared_ptr<Thread>> threads;
static Thread *running = nullptr;
static u64 ready_counter = 1;
static u32 next_thread_id = 1;
static std::set<u32> tls_used;
static u64 boot_ns;
#ifdef __WIIU__
static std::atomic<u32> diagnostics_level{1};
static std::atomic<u32> diagnostics_thread{0};
void set_diagnostics_thread(u32 thread) { diagnostics_thread.store(thread); }
static bool detailed_diagnostics(Thread *t) {
    const u32 selected = diagnostics_thread.load();
    return diagnostics_level.load() >= 2 && (!selected || selected == t->id);
}
void set_diagnostics_level(u32 level) { diagnostics_level.store(std::min(level, 3u)); }
struct SvcSample { u32 thread = 0, svc = 0, lr = 0, count = 0; const char *kind = "svc"; u64 total = 0, max = 0; };
static SvcSample svc_samples[128];
static u32 svc_samples_dropped;
#ifdef RECOMP_RESUMABLE
// One guest runner writes the cumulative SVC clock; reporting never resets it.
// Batch summaries are merged/read under kmutex and allocate no sample storage.
static u64 runner_svc_ns;
struct RunnerSample {
    u32 thread = 0, batches = 0, steps = 0;
    u64 elapsed = 0, service = 0, scheduler = 0, max_batch = 0, lookups = 0, misses = 0;
};
static RunnerSample runner_samples[32];
static void record_runner(u32 thread, u32 steps, u64 elapsed, u64 service,
                          u64 scheduler, u64 lookups, u64 misses) {
    for (auto &s : runner_samples) {
        if (s.batches && s.thread != thread) continue;
        s.thread = thread; ++s.batches; s.steps += steps;
        s.elapsed += elapsed; s.service += std::min(service, elapsed);
        s.scheduler += scheduler; s.max_batch = std::max(s.max_batch, elapsed);
        s.lookups += lookups; s.misses += misses;
        return;
    }
}
#endif
static void record_diagnostic(Thread *t, u32 svc, u32 lr, const char *kind, u64 elapsed) {
    for (auto &s : svc_samples) {
        if (s.count && (s.thread != t->id || s.svc != svc || s.lr != lr || strcmp(s.kind, kind))) continue;
        s.thread = t->id; s.svc = svc; s.lr = lr; s.kind = kind;
        ++s.count; s.total += elapsed; s.max = std::max(s.max, elapsed);
        return;
    }
    ++svc_samples_dropped;
}
void diagnostics_report_locked() {
#ifdef RECOMP_RESUMABLE
    for (auto &s : runner_samples) if (s.batches) {
        LOG("[guest-cpu] thread %u batches %u steps %u execute-us %llu svc-inclusive-us %llu scheduler-inclusive-us %llu max-batch-us %llu lookups %llu misses %llu\n",
            s.thread, s.batches, s.steps, (unsigned long long)((s.elapsed - s.service) / 1000),
            (unsigned long long)(s.service / 1000), (unsigned long long)(s.scheduler / 1000),
            (unsigned long long)(s.max_batch / 1000), (unsigned long long)s.lookups, (unsigned long long)s.misses);
        s = RunnerSample{};
    }
#endif
    for (auto &s : svc_samples) if (s.count) {
        LOG("[guest-svc] kind %s thread %u svc %02x lr %08x calls %u inclusive-us %llu max-us %llu\n",
            s.kind, s.thread, s.svc, s.lr, s.count, (unsigned long long)(s.total / 1000), (unsigned long long)(s.max / 1000));
        s = SvcSample{};
    }
    if (svc_samples_dropped) LOG("[guest-svc] sample table full: %u calls omitted\n", svc_samples_dropped);
    svc_samples_dropped = 0;
}
void diagnostics_report() { Lock lk(kmutex); diagnostics_report_locked(); }
#endif
static std::shared_ptr<Process> self_process = std::make_shared<Process>();
static std::shared_ptr<ResLimit> self_reslimit = std::make_shared<ResLimit>();
u32 heap_size, linear_used;
#ifndef __WIIU__
static thread_local Thread *tls_thread;
#endif
static void flush_deferred();
#ifdef RECOMP_RESUMABLE
static std::vector<std::weak_ptr<Object>> &checkpoint_objects() {
    static auto *objects = new std::vector<std::weak_ptr<Object>>;
    return *objects;
}
#endif
static std::atomic<int> lock_wanted;  // host threads in lock_soon: svc_dispatch lets them go first

bool lock_soon(Lock &lk) {
    static u64 without_since;  // (one host thread calls this, the sound's) since when it has gone without the lock, 0 if it had it last time
    static char holder[64];
    static u32 said;
    lock_wanted++;
    const u64 until = real_ns() + 1000000;
    bool got;
    while (!(got = lk.try_lock()) && real_ns() < until) {
#ifdef __WIIU__
        OSSleepTicks(OSMicrosecondsToTicks(50));
#else
        std::this_thread::sleep_for(std::chrono::microseconds(50));
#endif
    }
    lock_wanted--;
    if (!got && !without_since) {
        without_since = real_ns();
        const Thread *t = running;  // read without the lock, for the log alone: threads are never freed, and a stale answer costs a wrong name
        if (t && t->in_svc >= 0 && t->in_service) snprintf(holder, sizeof holder, "%s command %08x", t->in_service, t->in_cmd);
        else if (t && t->in_svc >= 0) snprintf(holder, sizeof holder, "svc %02x", (unsigned)t->in_svc);
        else snprintf(holder, sizeof holder, "a host thread");
    } else if (got && without_since) {
        if (said++ < 200) LOG("[lock] the kernel lock was held for about %u ms (%s): the sound went on without it\n", (u32)((real_ns() - without_since) / 1000000), holder);
        without_since = 0;
    }
    return got;
}

// ---------------------------------------------------------------- handles
Handle add_handle(ObjPtr o) {
#ifdef RECOMP_RESUMABLE
    checkpoint_objects().push_back(o);
#endif
    Handle h = next_handle++;
    handles[h] = std::move(o);
    return h;
}

ObjPtr get_object(Handle h) {
    if (h == CURRENT_THREAD) {
        Thread *t = current_thread();
        return t ? t->shared_from_this() : nullptr;
    }
    if (h == CURRENT_PROCESS) return self_process;
    auto it = handles.find(h);
    return it == handles.end() ? nullptr : it->second;
}

bool close_handle(Handle h) { return handles.erase(h) > 0; }

#ifdef __WIIU__  // no thread-local storage in an RPX
Thread *current_thread() { return (Thread *)OSGetThreadSpecific(OS_THREAD_SPECIFIC_0); }
#else
Thread *current_thread() { return tls_thread; }
#endif

std::shared_ptr<Event> make_event(ResetType rt, const char *name) {
    auto e = std::make_shared<Event>();
    e->reset = rt;
    e->name = name;
#ifdef RECOMP_RESUMABLE
    checkpoint_objects().push_back(e);
#endif
    return e;
}

// ---------------------------------------------------------------- time
u64 real_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The guest clock (timers, timeouts, the system tick): real time scaled by the game speed, so at any speed the
// game sees as much time pass per VBlank as on the 3DS
static std::mutex clock_m;
static u64 clock_vbase, clock_rbase;  // guest and real time at the last speed change
static u32 clock_pct = 100;

u64 now_ns() {
    std::lock_guard<std::mutex> lk(clock_m);
    const u64 r = real_ns();
    if (!clock_rbase) clock_vbase = clock_rbase = r;
    return clock_vbase + (r - clock_rbase) * clock_pct / 100;
}

static u64 to_real(u64 guest_ns) {
    std::lock_guard<std::mutex> lk(clock_m);
    return guest_ns * 100 / clock_pct;
}


u64 system_tick() {
    u64 ns = now_ns() - boot_ns;
    // 268111856 Hz
#ifdef __WIIU__
    return ns / 1000000000u * 268111856u + ns % 1000000000u * 268111856u / 1000000000u;
#else
    return (u64)((__uint128_t)ns * 268111856u / 1000000000u);
#endif
}

// timeouts run on a host thread
#ifdef RECOMP_RESUMABLE
static std::function<void()> restore_timeout(const TimeoutState &);
struct TimedCallback {
    std::function<void()> fn;
    TimeoutState state;
    void operator()() { fn(); }
    void state_fields(checkpoint::Archive &a) {
        a.fields(state.kind, state.object, state.service, state.generation, state.argument);
        if (state.kind == TimeoutKind::Unknown) { a.fail("unidentified timeout callback"); return; }
        if (a.reading && a.ok) {
            fn = restore_timeout(state);
            if (!fn) a.fail("timeout cannot be reconstructed");
        }
    }
};
using TimeoutCallback = TimedCallback;
#else
using TimeoutCallback = std::function<void()>;
#endif
struct TimeoutQueue {
    std::multimap<u64, TimeoutCallback> q;
    std::condition_variable cv;
    std::thread th;
};
static TimeoutQueue tq;

#ifdef __WIIU__
// wut's timed condition wait sets a timer, then sleeps. If the timer fires in between, its signal is lost and the thread sleeps until
// something else wakes it, and every guest thread waiting on a timeout stops with it: a loading screen that never ends, while the
// frames driven by VBlank carry on. The system's own event wait has no such gap.
static OSEvent tq_event;
static bool tq_event_ready;
static void tq_wake() {
    if (tq_event_ready) OSSignalEvent(&tq_event);
}
#else
static void tq_wake() { tq.cv.notify_one(); }
#endif

// until a timeout is added, or for this long in real time (0: no limit); the kernel lock is let go meanwhile
static void tq_sleep(Lock &lk, u64 real_ns) {
#ifdef __WIIU__
    lk.unlock();
    if (real_ns) OSWaitEventWithTimeout(&tq_event, real_ns);
    else OSWaitEvent(&tq_event);
    lk.lock();
#else
    if (real_ns) tq.cv.wait_for(lk, std::chrono::nanoseconds(real_ns));
    else tq.cv.wait(lk);
#endif
}

void set_clock_speed(u32 pct) {
    const u64 v = now_ns();
    {
        std::lock_guard<std::mutex> lk(clock_m);
        if (pct == clock_pct || !pct) return;
        clock_vbase = v;
        clock_rbase = real_ns();
        clock_pct = pct;
    }
    tq_wake();  // pending timeouts now fall due at another real time
}

void add_timeout(u64 deadline, std::function<void()> fn
#ifdef RECOMP_RESUMABLE
    , TimeoutState state
#endif
) {
#ifdef RECOMP_RESUMABLE
    tq.q.emplace(deadline, TimedCallback{std::move(fn), std::move(state)});
#else
    tq.q.emplace(deadline, std::move(fn));
#endif
    tq_wake();
}

void timers_poke() { tq_wake(); }

static void timeout_thread() {
    Lock lk(kmutex);
    for (;;) {
        if (tq.q.empty()) {
            tq_sleep(lk, 0);
            continue;
        }
        u64 t = tq.q.begin()->first;
        u64 n = now_ns();
        if (t > n) {
            tq_sleep(lk, std::max<u64>(to_real(t - n), 1));
            continue;
        }
        auto fn = std::move(tq.q.begin()->second);
        tq.q.erase(tq.q.begin());
        fn();
        schedule_async();
    }
}

// ---------------------------------------------------------------- scheduling
static Thread *best_ready() {
    Thread *best = nullptr;
    for (auto &t : threads) {
        if (t->status != TStatus::Ready) continue;
        if (!best || t->priority < best->priority ||
            (t->priority == best->priority && t->ready_seq < best->ready_seq))
            best = t.get();
    }
    return best;
}

static void make_ready(Thread *t, bool front = false) {
    t->status = TStatus::Ready;
    t->ready_seq = front ? 0 : ready_counter++;
}

// Gives the CPU to the best ready thread and, if that is not `cur`, waits until `cur` runs again.
static void switch_from(Thread *cur, Lock &lk) {
    flush_deferred();
    Thread *next = best_ready();
    running = next;
    if (next) {
        next->status = TStatus::Running;
        if (next != cur) next->cv.notify_one();
    }
    if (next == cur) return;
    kstats.switches++;
#ifdef RECOMP_RESUMABLE
    // The sole guest host returns to its dispatcher after the SVC. No guest
    // thread is suspended inside this C++ frame in the experimental runtime.
    return;
#endif
    cur->cv.wait(lk, [&] { return running == cur; });
}

void resume_thread(Thread *t) {
    if (t->status == TStatus::Dead || t->status == TStatus::Ready || t->status == TStatus::Running) return;
#ifdef RECOMP_RESUMABLE
    if (t->status == TStatus::WaitSync || t->status == TStatus::WaitArb) {
        t->cpu.r[0] = t->wait_result;
        if (t->status == TStatus::WaitSync && t->wait_out_index) t->cpu.r[1] = (u32)t->wait_index;
    }
#endif
#ifdef __WIIU__
    if ((t->id == 1 || detailed_diagnostics(t)) && wiiu::av_timing_enabled()) {
        t->diag_ready_at = real_ns();
        const Thread *waker = current_thread();
        t->diag_waker = waker ? waker->id : 0;
        t->diag_waker_lr = waker ? waker->cpu.r[14] : 0;
    }
#endif
    for (auto &o : t->wait_objs) o->remove_waiter(t);
    t->wait_objs.clear();
    t->wait_gen++;
    make_ready(t);
}

// Host-side updates of guest shared memory that guest code modifies with read-modify-write sequences (e.g. the
// GSP interrupt queue) must not interleave with guest code. Only the `running` thread executes guest code, and
// while the kernel lock is held it can only be doing so outside an SVC: such updates are deferred until it
// enters its next SVC (or blocks).
static std::vector<std::function<void()>> deferred_work;

void run_when_quiescent(std::function<void()> fn) {
    if (!running) fn();
    else {
        deferred_work.push_back(std::move(fn));
        rt_preempt_flag = 1;  // (a guest loop may be polling what fn updates)
    }
}

static void flush_deferred() {
    while (!deferred_work.empty()) {
        auto d = std::move(deferred_work);
        deferred_work.clear();
        for (auto &f : d) f();
    }
}

void schedule_async() {
    if (running) {
        // the running thread keeps the CPU until its next SVC unless it is told to give way
        Thread *best = best_ready();
        if (best && best->priority < running->priority) rt_preempt_flag = 1;
        return;
    }
    flush_deferred();
    Thread *next = best_ready();
    if (!next) return;
    running = next;
    next->status = TStatus::Running;
    kstats.wakes++;
    next->cv.notify_one();
}

void reschedule(Lock &lk) {
    Thread *cur = current_thread();
#ifdef RECOMP_RESUMABLE
    // A service can install a sleep before SendSyncRequest's final reschedule.
    // It has already yielded; making it ready here would cancel the wait and
    // strand the thread selected by switch_from in Running status.
    if (running != cur || cur->status != TStatus::Running) return;
#endif
    Thread *best = best_ready();
    if (!best || best->priority >= cur->priority) return;
    make_ready(cur, true);
    switch_from(cur, lk);
}

void block_current(Lock &lk) {
    Thread *cur = current_thread();
#ifdef __WIIU__
    const bool timed = (cur->id == 1 || detailed_diagnostics(cur)) && wiiu::av_timing_enabled();
    const u64 began = timed ? real_ns() : 0;
    const u32 caller = cur->cpu.r[14];
    const int svc = cur->in_svc;
    char object[48] = "sleep";
    if (timed) { cur->diag_ready_at = 0; cur->diag_waker = cur->diag_waker_lr = 0; }
    if (timed && cur->status == TStatus::WaitArb)
        snprintf(object, sizeof object, "arb@%08x", cur->arb_addr);
    if (timed && !cur->wait_objs.empty())
        snprintf(object, sizeof object, "%s", cur->wait_objs.front()->name.c_str());
#endif
    switch_from(cur, lk);
#ifdef __WIIU__
    if (timed) {
        struct Sample { u32 thread = 0, caller = 0, count = 0, waker = 0, waker_lr = 0; int svc = 0; u64 ns = 0, max_ns = 0, ready_ns = 0; char object[48] = {}; };
        static Sample samples[128];
        static u32 dropped;
        static u64 report_at = began;
        const u64 now = real_ns(), elapsed = now - began;
        bool recorded = false;
        for (auto &s : samples) {
            if (s.count && (s.thread != cur->id || s.caller != caller || s.svc != svc || strcmp(s.object, object) || s.waker != cur->diag_waker || s.waker_lr != cur->diag_waker_lr)) continue;
            s.thread = cur->id; s.caller = caller; s.svc = svc;
            s.waker = cur->diag_waker; s.waker_lr = cur->diag_waker_lr;
            if (cur->diag_ready_at >= began && cur->diag_ready_at <= now) s.ready_ns += now - cur->diag_ready_at;
            snprintf(s.object, sizeof s.object, "%s", object);
            ++s.count; s.ns += elapsed; s.max_ns = std::max(s.max_ns, elapsed);
            recorded = true;
            break;
        }
        if (!recorded) ++dropped;
        if (now - report_at >= 2000000000ULL) {
            for (auto &s : samples) if (s.count) {
                LOG("[guest-wait] thread %u lr %08x svc %02x object %s: %u waits total %llu us max %llu us ready-delay %llu us waker %u lr %08x in %llu ms\n",
                    s.thread, s.caller, s.svc, s.object, s.count, (unsigned long long)(s.ns / 1000),
                    (unsigned long long)(s.max_ns / 1000), (unsigned long long)(s.ready_ns / 1000),
                    s.waker, s.waker_lr, (unsigned long long)((now - report_at) / 1000000));
                s = Sample{};
            }
            if (dropped) LOG("[guest-wait] sample table full: %u waits omitted\n", dropped);
            dropped = 0;
            report_at = now;
        }
    }
#endif
}

static void yield_current(Lock &lk) {
    Thread *cur = current_thread();
    make_ready(cur);
    switch_from(cur, lk);
}

void sleep_current(u64 ns, Lock &lk) {
    Thread *t = current_thread();
    t->status = TStatus::WaitSleep;
    u64 gen = t->wait_gen;
    add_timeout(now_ns() + ns, [t, gen] {
        if (t->wait_gen == gen && t->status == TStatus::WaitSleep) resume_thread(t);
    } STATE_TIMEOUT(TimeoutKind::Sleep, t->shared_from_this(), nullptr, gen));
    block_current(lk);
}

// Storage is what this is for. A read used to keep the CPU: every other thread of the game, its sound thread among them, stood still for as
// long as the storage took, which on a console is most of a transition between scenes, and the sound stuttered through each one (2026-11-10).
void run_blocked(Lock &lk, const std::function<void()> &work) {
#ifdef RECOMP_RESUMABLE
    // Synchronous host I/O is deliberately slower but leaves no native
    // service continuation to serialize. Other host workers retain kmutex.
    lk.unlock();
    work();
    lk.lock();
    return;
#endif
    Thread *cur = current_thread();
#ifdef __WIIU__
    const bool timed = detailed_diagnostics(cur);
    const u32 caller = cur->cpu.r[14], svc = (u32)cur->in_svc;
#endif
    cur->status = TStatus::WaitSleep;  // with no timeout set: nothing resumes it but the end of this
    cur->wait_gen++;
    flush_deferred();
    running = best_ready();
    if (running) {
        running->status = TStatus::Running;
        running->cv.notify_one();
    }
    lk.unlock();
#ifdef __WIIU__
    const u64 began = timed ? real_ns() : 0;
#endif
    work();
#ifdef __WIIU__
    const u64 completed = timed ? real_ns() : 0;
#endif
    lk.lock();
    make_ready(cur, true);
    schedule_async();  // takes the CPU if it's free, and asks a thread of lower priority for it otherwise
    cur->cv.wait(lk, [&] { return running == cur; });
#ifdef __WIIU__
    if (timed) {
        record_diagnostic(cur, svc, caller, "native-work", completed - began);
        record_diagnostic(cur, svc, caller, "native-resume", real_ns() - completed);
    }
#endif
}

// ---------------------------------------------------------------- wait objects
void WaitObject::add_waiter(Thread *t) {
    if (std::find(waiters.begin(), waiters.end(), t) == waiters.end()) waiters.push_back(t);
}

void WaitObject::remove_waiter(Thread *t) { waiters.erase(std::remove(waiters.begin(), waiters.end(), t), waiters.end()); }

void WaitObject::wakeup_waiters() {
    for (;;) {
        Thread *cand = nullptr;
        for (Thread *t : waiters) {
            if (t->status != TStatus::WaitSync) continue;
            bool ok;
            if (t->wait_all) {
                ok = true;
                for (auto &o : t->wait_objs)
                    if (o->should_wait(t)) ok = false;
            } else {
                ok = !should_wait(t);
            }
            if (ok && (!cand || t->priority < cand->priority)) cand = t;
        }
        if (!cand) break;
        if (cand->wait_all) {
            for (auto &o : cand->wait_objs) o->acquire(cand);
            cand->wait_index = 0;
        } else {
            acquire(cand);
            for (size_t i = 0; i < cand->wait_objs.size(); i++)
                if (cand->wait_objs[i].get() == this) cand->wait_index = (s32)i;
        }
        cand->wait_result = RES_OK;
        resume_thread(cand);
    }
    after_wakeups();
}

void Event::signal() {
    signaled = true;
    wakeup_waiters();
}

void Mutex::acquire(Thread *t) {
    owner = t;
    lock_count++;
}

u32 Mutex::release(Thread *t) {
    if (owner != t) return 0xD8E0041F;  // not owner
    if (--lock_count == 0) {
        owner = nullptr;
        wakeup_waiters();
    }
    return RES_OK;
}

static void timer_schedule(std::shared_ptr<Timer> tm, s64 delay) {
    u64 gen = tm->generation;
    add_timeout(now_ns() + (u64)std::max<s64>(delay, 0), [tm, gen] {
        if (tm->generation != gen) return;
        tm->fire();
        if (tm->interval > 0) timer_schedule(tm, tm->interval);
    } STATE_TIMEOUT(TimeoutKind::Timer, tm, nullptr, gen));
}

void Timer::fire() {
    signaled = true;
    wakeup_waiters();
}

#ifdef RECOMP_RESUMABLE
void persistent_gsp_irq(u32);
static std::function<void()> restore_timeout(const TimeoutState &s) {
    if (s.kind == TimeoutKind::Sleep || s.kind == TimeoutKind::WaitSync || s.kind == TimeoutKind::WaitArb) {
        auto t = std::dynamic_pointer_cast<Thread>(s.object);
        if (!t) return {};
        return [t, s] {
            const TStatus expected = s.kind == TimeoutKind::Sleep ? TStatus::WaitSleep :
                                     s.kind == TimeoutKind::WaitSync ? TStatus::WaitSync : TStatus::WaitArb;
            if (t->wait_gen != s.generation || t->status != expected) return;
            if (s.kind != TimeoutKind::Sleep) t->wait_result = RES_TIMEOUT;
            if (s.kind == TimeoutKind::WaitSync) t->wait_index = -1;
            resume_thread(t.get());
        };
    }
    if (s.kind == TimeoutKind::Timer) {
        auto tm = std::dynamic_pointer_cast<Timer>(s.object);
        if (!tm) return {};
        return [tm, s] {
            if (tm->generation != s.generation) return;
            tm->fire();
            if (tm->interval > 0) timer_schedule(tm, tm->interval);
        };
    }
    if (s.kind == TimeoutKind::Service && s.service)
        return [s] { s.service->persistent_timeout(s.generation, s.argument); };
    if (s.kind == TimeoutKind::GspIrq && s.argument <= 6)
        return [s] { persistent_gsp_irq(s.argument); };
    return {};
}
#endif

// ---------------------------------------------------------------- threads
static u32 alloc_tls() {
    for (u32 i = 0;; i++)
        if (!tls_used.count(i)) {
            tls_used.insert(i);
            u32 a = mem::TLS_BASE + i * mem::TLS_SLOT;
            memset(gptr(a), 0, mem::TLS_SLOT);
            return a;
        }
}

static void thread_exit_locked(Thread *t, Lock &lk);
#ifdef RECOMP_RESUMABLE
static void resumable_runner();
#endif

#ifdef __WIIU__
static OSThread *guest_hosts[64];
void *guest_host_thread(u32 i) { return i < 64 ? guest_hosts[i] : nullptr; }
#endif

static void *thread_main(void *arg) {
    Thread *t = (Thread *)arg;
#ifdef __WIIU__
    OSSetThreadSpecific(OS_THREAD_SPECIFIC_0, t);
    guest_hosts[t->id % 64] = OSGetCurrentThread();
#else
    tls_thread = t;
#endif
    {
        Lock lk(kmutex);
        t->cv.wait(lk, [&] { return running == t; });
    }
#ifdef RECOMP_RESUMABLE
    resumable_runner();
#else
    run_guest(&t->cpu);
#endif
    Lock lk(kmutex);
    thread_exit_locked(t, lk);
    return nullptr;
}

#ifdef __WIIU__
// Pinned from birth. Asking from inside a pthread, with OSSetThreadAffinity, left guest code on cores 0 and 2 of a real console and core 1 idle (cos coretrace), though Cemu did as it was told.
// Core 1 has the 2 MB of L2, and the GPU thread needs core 2 to itself.
struct GuestHost {
    OSThread os;
    alignas(16) u8 stack[2 << 20];
};
static char guest_names[64][12];

static int guest_entry(int, void *t) {
    thread_main(t);
    return 0;
}

static void start_guest_host(Thread *t) {
    auto *host = (GuestHost *)mem::host_alloc(16, sizeof(GuestHost));
    if (!host) FATAL("no memory for a guest thread\n");
    if (!OSCreateThread(&host->os, guest_entry, 0, t, host->stack + sizeof host->stack, sizeof host->stack, 16, (OSThreadAttributes)(OS_THREAD_ATTRIB_AFFINITY_CPU1 | OS_THREAD_ATTRIB_DETACHED)))
        FATAL("cannot create a guest thread\n");
    char *name = guest_names[t->id % 64];
    snprintf(name, sizeof guest_names[0], "guest %u", t->id);
    OSSetThreadName(&host->os, name);
    OSSetThreadDeallocator(&host->os, [](OSThread *th, void *) { mem::host_free(th); });
    OSSetThreadRunQuantum(&host->os, 1000);  // as wut's pthreads have
    OSResumeThread(&host->os);
}
#endif

std::shared_ptr<Thread> create_thread(u32 entry, u32 arg, u32 stack_top, s32 prio, bool start) {
    auto t = std::make_shared<Thread>();
    t->id = next_thread_id++;
    t->priority = prio;
    t->entry = entry;
    t->tls = alloc_tls();
    t->cpu.r[0] = arg;
    t->cpu.r[13] = stack_top & ~7u;
    t->cpu.r[14] = EXIT_MAGIC;
    t->cpu.pc = entry;
    t->cpu.tls = t->tls;
    t->cpu.fpscr = 0x03C00000;  // RunFast: default NaN, flush to zero (what the SDK sets)
    t->cpu.thread = t.get();
    threads.push_back(t);
#ifdef __WIIU__
#ifdef RECOMP_RESUMABLE
    if (threads.size() == 1)
#endif
    start_guest_host(t.get());
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 << 20);
    pthread_t pt;
#ifdef RECOMP_RESUMABLE
    if (threads.size() == 1) {
#endif
    if (pthread_create(&pt, &attr, thread_main, t.get())) FATAL("pthread_create failed\n");
    t->mach = pthread_mach_thread_np(pt);
    pthread_detach(pt);
#ifdef RECOMP_RESUMABLE
    }
#endif
#endif
    if (start) make_ready(t.get());
    return t;
}

static void thread_exit_locked(Thread *t, Lock &lk) {
    t->status = TStatus::Dead;
#ifdef __WIIU__
    guest_hosts[t->id % 64] = nullptr;
#endif
    tls_used.erase((t->tls - mem::TLS_BASE) / mem::TLS_SLOT);
    t->wakeup_waiters();
    // release mutexes it holds
    for (auto &kv : handles)
        if (auto m = std::dynamic_pointer_cast<Mutex>(kv.second))
            if (m->owner == t) {
                m->lock_count = 1;
                m->release(t);
            }
    Thread *next = best_ready();
    running = next;
    if (next) {
        next->status = TStatus::Running;
        next->cv.notify_one();
    }
#ifndef RECOMP_RESUMABLE
    lk.unlock();
    pthread_exit(nullptr);
#endif
}

#ifdef RECOMP_RESUMABLE
extern "C" const u32 guest_resumable;
static void resumable_runner() {
    if (!guest_resumable) FATAL("resumable kernel requires resumable generated code\n");
    LOG("[state] experimental single-runner scheduler active\n");
    for (;;) {
        std::unique_lock<checkpoint::Gate> checkpoint_lock(checkpoint::gate());
        // A reload may release a thread that was created after the checkpoint.
        // Timer callbacks must not inspect the previous step's TLS pointer.
#ifdef __WIIU__
        OSSetThreadSpecific(OS_THREAD_SPECIFIC_0, nullptr);
#else
        tls_thread = nullptr;
#endif
        Thread *t;
#ifdef __WIIU__
        const bool sample_runner = diagnostics_level.load(std::memory_order_relaxed) >= 2;
        const u64 scheduler_begin = sample_runner ? real_ns() : 0;
#endif
        {
            Lock lk(kmutex);
            while (!tq.q.empty() && tq.q.begin()->first <= now_ns()) {
                auto fn = std::move(tq.q.begin()->second);
                tq.q.erase(tq.q.begin());
                fn();
            }
            flush_deferred();
            if (running) {
                Thread *best = best_ready();
                if (best && best->priority < running->priority) {
                    make_ready(running, true);
                    running = nullptr;
                }
            }
            rt_preempt_flag = 0;
            schedule_async();
            t = running;
        }
        if (!t) {
            checkpoint_lock.unlock(); // video/audio/input must be able to make a waiting guest ready
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
#ifdef __WIIU__
        OSSetThreadSpecific(OS_THREAD_SPECIFIC_0, t);
#else
        tls_thread = t;
#endif
#ifdef __WIIU__
        const bool sampled = sample_runner && detailed_diagnostics(t);
        const u64 batch_begin = sampled ? real_ns() : 0;
        const u64 svc_begin = runner_svc_ns, lookups_begin = rt_lookups, misses_begin = rt_lookup_misses;
#endif
        unsigned steps = 0;
        for (; steps < 256;) {
            run_guest_step(&t->cpu); ++steps;
            if (t->cpu.pc == EXIT_MAGIC || running != t || t->status != TStatus::Running || rt_preempt_flag || rt_checkpoint_requested) break;
        }
#ifdef __WIIU__
        if (sampled) {
            const u64 elapsed = real_ns() - batch_begin;
            Lock lk(kmutex);
            record_runner(t->id, steps, elapsed, runner_svc_ns - svc_begin,
                          batch_begin - scheduler_begin, rt_lookups - lookups_begin, rt_lookup_misses - misses_begin);
        }
#endif
        if (t->cpu.pc == EXIT_MAGIC && t->status != TStatus::Dead) {
            Lock lk(kmutex);
            thread_exit_locked(t, lk);
        }
    }
}
#endif

// ---------------------------------------------------------------- memory bookkeeping
struct Region {
#ifdef RECOMP_RESUMABLE
    void state_fields(checkpoint::Archive &a) { a.fields(base, size, perm, state); }
#endif
    u32 base, size, perm, state;
};
static std::map<u32, Region> regions;  // by base

static void add_region(u32 base, u32 size, u32 perm, u32 state) { regions[base] = {base, size, perm, state}; }

static void remove_region(u32 base, u32 size) {
    // (only exact or splitting removal at region granularity)
    for (auto it = regions.begin(); it != regions.end();) {
        Region r = it->second;
        u32 rend = r.base + r.size, end = base + size;
        if (rend <= base || r.base >= end) {
            ++it;
            continue;
        }
        it = regions.erase(it);
        if (r.base < base) regions[r.base] = {r.base, base - r.base, r.perm, r.state};
        if (rend > end) regions[end] = {end, rend - end, r.perm, r.state};
    }
}

static const Region *find_region(u32 a) {
    auto it = regions.upper_bound(a);
    if (it == regions.begin()) return nullptr;
    --it;
    if (a < it->second.base + it->second.size) return &it->second;
    return nullptr;
}

bool guest_busy() {
    Lock lk(kmutex);
    return running != nullptr;
}

static bool guest_permitted(u32 a, u32 size, u32 permission) {
    const u64 end = (u64)a + size;
    if (!size || end > 0x100000000ull || mem::lent(a, size)) return false;
    const u64 program_end = (u64)g_game.data_addr + g_game.data_size + g_game.bss_size;
    const u32 program_start = permission == 1 ? g_game.text_addr : g_game.data_addr;
    u64 pos = a;
    while (pos < end) {
        if (pos >= program_start && pos < program_end) {
            pos = std::min(end, program_end);
        } else {
            const Region *r = find_region((u32)pos);
            if (!r || !(r->perm & permission)) return false;
            pos = std::min(end, (u64)r->base + r->size);
        }
    }
    return true;
}

bool guest_readable_locked(u32 a, u32 size) {
    return guest_permitted(a, size, 1);
}

bool guest_readable(u32 a, u32 size) {
    if (size && a >= g_game.text_addr && (u64)a + size <= (u64)g_game.data_addr + g_game.data_size + g_game.bss_size &&
        !mem::lent(a, size)) return true;
    Lock lk(kmutex);
    return guest_readable_locked(a, size);
}

bool guest_writable(u32 a, u32 size) {
    return guest_permitted(a, size, 2);
}

// linear heap allocator: first fit over free ranges
static std::map<u32, u32> linear_free;  // base -> size

static u32 linear_alloc(u32 size) {
    for (auto it = linear_free.begin(); it != linear_free.end(); ++it) {
        if (it->second >= size) {
            u32 a = it->first, s = it->second;
            linear_free.erase(it);
            if (s > size) linear_free[a + size] = s - size;
            linear_used += size;
            return a;
        }
    }
    return 0;
}

u32 alloc_linear(u32 size) { return linear_alloc((size + 0xFFF) & ~0xFFFu); }

static void linear_free_range(u32 a, u32 size) {
    linear_free[a] = size;
    linear_used -= size;
    // merge neighbours
    auto it = linear_free.find(a);
    if (it != linear_free.begin()) {
        auto p = std::prev(it);
        if (p->first + p->second == it->first) {
            p->second += it->second;
            linear_free.erase(it);
            it = p;
        }
    }
    auto n = std::next(it);
    if (n != linear_free.end() && it->first + it->second == n->first) {
        it->second += n->second;
        linear_free.erase(n);
    }
}

// ---------------------------------------------------------------- SVCs
static void wait_on(Thread *t, std::vector<std::shared_ptr<WaitObject>> objs, bool all, s64 timeout, Lock &lk,
                    CPU *c, bool out_index) {
    t->wait_objs = objs;
    t->wait_all = all;
    t->wait_out_index = out_index;
    t->status = TStatus::WaitSync;
    t->wait_result = RES_TIMEOUT;
    t->wait_index = -1;
    for (auto &o : objs) o->add_waiter(t);
    if (timeout >= 0) {
        u64 gen = t->wait_gen;
        add_timeout(now_ns() + (u64)timeout, [t, gen] {
            if (t->wait_gen != gen || t->status != TStatus::WaitSync) return;
            t->wait_result = RES_TIMEOUT;
            t->wait_index = -1;
            resume_thread(t);
        } STATE_TIMEOUT(TimeoutKind::WaitSync, t->shared_from_this(), nullptr, gen));
    }
    block_current(lk);
    c->r[0] = t->wait_result;
    if (out_index) c->r[1] = (u32)t->wait_index;
}

static u32 svc_control_memory(CPU *c) {
    u32 op = c->r[0], addr0 = c->r[1], addr1 = c->r[2], size = c->r[3], perm = c->r[4];
    u32 kind = op & 0xFF;
    bool linear = op & 0x10000;
    size = (size + 0xFFF) & ~0xFFFu;
    VLOG("[svc] ControlMemory op=%x addr0=%08x addr1=%08x size=%x perm=%x\n", op, addr0, addr1, size, perm);
    switch (kind) {
    case 3: {  // COMMIT
        u32 a;
        if (linear) {
            a = linear_alloc(size);
            if (!a) return RES_OUT_OF_MEMORY;
        } else {
            a = addr0;
            // a game in a larger memory mode (loader.cpp) can commit past the standard 64 MB heap
            const u32 heap_end = mem::HEAP_BASE + std::max(0x04000000u, g_game.app_memory);
            if (a < mem::HEAP_BASE || a > heap_end || size > heap_end - a) {
                LOG("[svc] ControlMemory: bad heap address %08x\n", a);
                return RES_INVALID_ADDRESS;
            }
            heap_size += size;
        }
        memset(gptr(a), 0, size);
        add_region(a, size, perm, linear ? 5 : 5);
        c->r[1] = a;
        return RES_OK;
    }
    case 1: {  // FREE
        if (addr0 >= mem::LINEAR_BASE && addr0 < mem::LINEAR_BASE + mem::LINEAR_SIZE) linear_free_range(addr0, size);
        else heap_size -= std::min(heap_size, size);
        remove_region(addr0, size);
        c->r[1] = addr0;
        return RES_OK;
    }
    case 4: {  // MAP addr1 -> addr0 (alias)
        if (mem::lent(addr0, size)) return RES_INVALID_ADDRESS;  // below the heap: a 3DS maps nothing there either
        LOG("[svc] ControlMemory MAP %08x -> %08x size %x (copying, no alias)\n", addr1, addr0, size);
        memmove(gptr(addr0), gptr(addr1), size);
        add_region(addr0, size, perm, 6);
        c->r[1] = addr0;
        return RES_OK;
    }
    case 5:
        remove_region(addr0, size);
        return RES_OK;
    case 6:
        return RES_OK;
    }
    LOG("[svc] ControlMemory: unknown op %x\n", op);
    return 0xE0E01BEE;
}

static u32 svc_query_memory(CPU *c) {
    u32 a = c->r[2];
    const Region *r = find_region(a);
    if (r) {
        c->r[1] = r->base;
        c->r[2] = r->size;
        c->r[3] = r->perm;
        c->r[4] = r->state;
    } else {
        // free gap: up to the next region
        auto it = regions.upper_bound(a);
        u32 base = a & ~0xFFFu;
        u32 end = it == regions.end() ? 0x40000000u : it->first;
        c->r[1] = base;
        c->r[2] = end - base;
        c->r[3] = 0;
        c->r[4] = 0;
    }
    c->r[5] = 0;
    return RES_OK;
}

static u32 svc_arbitrate(CPU *c, Lock &lk) {
    Handle h = c->r[0];
    u32 addr = c->r[1], type = c->r[2];
    s32 value = (s32)c->r[3];
    s64 ns = (s64)((u64)c->r[4] | ((u64)c->r[5] << 32));
    auto arb = get<Arbiter>(h);
    if (!arb) return RES_INVALID_HANDLE;
    Thread *t = current_thread();
    switch (type) {
    case 0: {  // signal
        std::vector<Thread *> ws;
        for (auto &th : threads)
            if (th->status == TStatus::WaitArb && th->arb == arb.get() && th->arb_addr == addr) ws.push_back(th.get());
        std::sort(ws.begin(), ws.end(), [](Thread *a, Thread *b) {
            return a->priority != b->priority ? a->priority < b->priority : a->ready_seq < b->ready_seq;
        });
        s32 n = value < 0 ? (s32)ws.size() : std::min<s32>(value, (s32)ws.size());
        if (g_verbose >= 3) LOG("[arb] signal %08x value %d: %zu waiting, waking %d (lr %08x)\n", addr, value, ws.size(), n, c->r[14]);
        for (s32 i = 0; i < n; i++) {
            ws[i]->wait_result = RES_OK;
            resume_thread(ws[i]);
        }
        reschedule(lk);
        return RES_OK;
    }
    case 1:
    case 2:
    case 3:
    case 4: {
        s32 mv = (s32)r32(addr);
        if (g_verbose >= 3) LOG("[arb] thread %u type %u addr %08x mem %d value %d (lr %08x)\n", t->id, type, addr, mv, value, c->r[14]);
        if (!(mv < value)) return RES_OK;
        if (type == 2 || type == 4) w32(addr, (u32)(mv - 1));
        t->status = TStatus::WaitArb;
        t->arb = arb.get();
        t->arb_addr = addr;
        t->wait_result = RES_OK;
        if (type >= 3 && ns >= 0) {
            u64 gen = t->wait_gen;
            t->wait_result = RES_TIMEOUT;
            add_timeout(now_ns() + (u64)ns, [t, gen] {
                if (t->wait_gen != gen || t->status != TStatus::WaitArb) return;
                t->wait_result = RES_TIMEOUT;
                resume_thread(t);
            } STATE_TIMEOUT(TimeoutKind::WaitArb, t->shared_from_this(), nullptr, gen));
        }
        block_current(lk);
        return t->wait_result == RES_TIMEOUT ? RES_TIMEOUT : RES_OK;
    }
    }
    return 0xD8E093ED;
}

static std::shared_ptr<WaitObject> waitable(Handle h) {
    return std::dynamic_pointer_cast<WaitObject>(get_object(h));
}

static void svc_locked(CPU *c, u32 imm, Lock &lk) {
    Thread *t = current_thread();
    switch (imm) {
    case 0x01: c->r[0] = svc_control_memory(c); break;
    case 0x02: c->r[0] = svc_query_memory(c); break;
    case 0x03:
        LOG("[svc] ExitProcess\n");
        _exit(0);
    case 0x08: {  // CreateThread(prio=r0, entry=r1, arg=r2, stack=r3, core=r4)
        s32 prio = (s32)c->r[0];
        auto nt = create_thread(c->r[1], c->r[2], c->r[3], prio, true);
        VLOG("[svc] CreateThread entry=%08x arg=%08x stack=%08x prio=%d core=%d -> id %u\n", c->r[1], c->r[2],
             c->r[3], prio, (s32)c->r[4], nt->id);
        c->r[1] = add_handle(nt);
        c->r[0] = RES_OK;
        reschedule(lk);
        break;
    }
    case 0x09:
        VLOG("[svc] ExitThread %u\n", t->id);
        thread_exit_locked(t, lk);
        break;
    case 0x0A: {  // SleepThread
        s64 ns = (s64)((u64)c->r[0] | ((u64)c->r[1] << 32));
        if (ns <= 0) yield_current(lk);
        else sleep_current((u64)ns, lk);
        break;
    }
    case 0x0B: {
        auto th = get<Thread>(c->r[1]);
        if (!th) { c->r[0] = RES_INVALID_HANDLE; break; }
        c->r[1] = (u32)th->priority;
        c->r[0] = RES_OK;
        break;
    }
    case 0x0C: {
        auto th = get<Thread>(c->r[0]);
        if (!th) { c->r[0] = RES_INVALID_HANDLE; break; }
        th->priority = (s32)c->r[1];
        c->r[0] = RES_OK;
        reschedule(lk);
        break;
    }
    case 0x13: {  // CreateMutex(initial=r1)
        auto m = std::make_shared<Mutex>();
        m->name = "mutex@" + std::to_string(c->r[14]);
        if (c->r[1]) m->acquire(t);
        c->r[1] = add_handle(m);
        c->r[0] = RES_OK;
        break;
    }
    case 0x14: {
        auto m = get<Mutex>(c->r[0]);
        if (!m) { c->r[0] = RES_INVALID_HANDLE; break; }
        c->r[0] = m->release(t);
        reschedule(lk);
        break;
    }
    case 0x15: {  // CreateSemaphore(initial=r1, max=r2)
        auto s = std::make_shared<Semaphore>();
        s->name = "sema@" + std::to_string(c->r[14]);
        s->count = (s32)c->r[1];
        s->max = (s32)c->r[2];
        c->r[1] = add_handle(s);
        c->r[0] = RES_OK;
        break;
    }
    case 0x16: {  // ReleaseSemaphore(handle=r1, count=r2) -> r1 old count
        auto s = get<Semaphore>(c->r[1]);
        if (!s) { c->r[0] = RES_INVALID_HANDLE; break; }
        s32 old = s->count;
        if (s->count + (s32)c->r[2] > s->max) { c->r[0] = 0xD8E007FD; break; }
        s->count += (s32)c->r[2];
        s->wakeup_waiters();
        c->r[1] = (u32)old;
        c->r[0] = RES_OK;
        reschedule(lk);
        break;
    }
    case 0x17: {  // CreateEvent(reset=r1)
        auto e = make_event((ResetType)c->r[1], "guest");
        char nm[32];
        snprintf(nm, sizeof nm, "event@%08x", c->r[14]);
        e->name = nm;
        c->r[1] = add_handle(e);
        c->r[0] = RES_OK;
        break;
    }
    case 0x18: {
        auto e = get<Event>(c->r[0]);
        if (!e) {
            LOG("[svc] SignalEvent: bad handle %08x (lr=%08x)\n", c->r[0], c->r[14]);
            c->r[0] = RES_INVALID_HANDLE;
            break;
        }
        e->signal();
        c->r[0] = RES_OK;
        reschedule(lk);
        break;
    }
    case 0x19: {
        auto e = get<Event>(c->r[0]);
        if (!e) { c->r[0] = RES_INVALID_HANDLE; break; }
        e->clear();
        c->r[0] = RES_OK;
        break;
    }
    case 0x1A: {  // CreateTimer(reset=r1)
        auto tm = std::make_shared<Timer>();
        tm->name = "timer@" + std::to_string(c->r[14]);
        tm->reset = (ResetType)c->r[1];
        c->r[1] = add_handle(tm);
        c->r[0] = RES_OK;
        break;
    }
    case 0x1B: {  // SetTimer(handle=r0, interval lo=r1, initial=r2:r3, interval hi=r4)
        auto tm = get<Timer>(c->r[0]);
        if (!tm) { c->r[0] = RES_INVALID_HANDLE; break; }
        tm->initial = (s64)((u64)c->r[2] | ((u64)c->r[3] << 32));
        tm->interval = (s64)((u64)c->r[1] | ((u64)c->r[4] << 32));
        tm->generation++;
        timer_schedule(tm, tm->initial);
        c->r[0] = RES_OK;
        break;
    }
    case 0x1C: {  // CancelTimer
        auto tm = get<Timer>(c->r[0]);
        if (tm) tm->generation++;
        c->r[0] = tm ? RES_OK : RES_INVALID_HANDLE;
        break;
    }
    case 0x1D: {  // ClearTimer
        auto tm = get<Timer>(c->r[0]);
        if (tm) tm->signaled = false;
        c->r[0] = tm ? RES_OK : RES_INVALID_HANDLE;
        break;
    }
    case 0x1E: {  // CreateMemoryBlock(other_perm=r0, addr=r1, size=r2, my_perm=r3)
        auto sm = std::make_shared<SharedMem>();
        sm->backing = c->r[1];
        sm->size = c->r[2];
        sm->my_perm = c->r[3];
        sm->other_perm = c->r[0];
        if (!sm->backing) {
            sm->backing = linear_alloc((sm->size + 0xFFF) & ~0xFFFu);
            memset(gptr(sm->backing), 0, sm->size);
        }
        c->r[1] = add_handle(sm);
        c->r[0] = RES_OK;
        break;
    }
    case 0x1F: {  // MapMemoryBlock(handle, addr, perm, other)
        auto sm = get<SharedMem>(c->r[0]);
        if (!sm) { c->r[0] = RES_INVALID_HANDLE; break; }
        u32 addr = c->r[1];
        if (addr && mem::lent(addr, sm->size)) { c->r[0] = RES_INVALID_ADDRESS; break; }
        VLOG("[svc] MapMemoryBlock %s at %08x (backing %08x size %x)\n", sm->name.c_str(), addr, sm->backing, sm->size);
        if (addr && sm->backing && addr != sm->backing) {
            // No aliasing: the block moves to where the game maps it.
            memmove(gptr(addr), gptr(sm->backing), sm->size);
            sm->backing = addr;
            if (sm->on_move) sm->on_move(addr);
        } else if (addr) {
            sm->backing = addr;
        }
        add_region(sm->backing, (sm->size + 0xFFF) & ~0xFFFu, c->r[2], 6);
        c->r[0] = RES_OK;
        break;
    }
    case 0x20:
        c->r[0] = RES_OK;
        break;
    case 0x21: {
        c->r[1] = add_handle(std::make_shared<Arbiter>());
        c->r[0] = RES_OK;
        break;
    }
    case 0x22: c->r[0] = svc_arbitrate(c, lk); break;
    case 0x23:
        c->r[0] = close_handle(c->r[0]) ? RES_OK : RES_INVALID_HANDLE;
        break;
    case 0x24: {  // WaitSynchronization1(handle=r0, ns=r2:r3)
        auto o = waitable(c->r[0]);
        s64 ns = (s64)((u64)c->r[2] | ((u64)c->r[3] << 32));
        if (!o) { c->r[0] = RES_INVALID_HANDLE; break; }
        if (!o->should_wait(t)) {
            o->acquire(t);
            o->after_wakeups();
            c->r[0] = RES_OK;
            break;
        }
        if (ns == 0) { c->r[0] = RES_TIMEOUT; break; }
        if (ns > 100000000) VLOG("[svc] thread %u waits on %s for %lld ms (lr %08x)\n", t->id, o->name.c_str(), (long long)(ns / 1000000), c->r[14]);
        wait_on(t, {o}, false, ns, lk, c, false);
        break;
    }
    case 0x25: {  // WaitSynchronizationN(ns lo=r0, handles=r1, count=r2, all=r3, ns hi=r4) -> r1 index
        s64 ns = (s64)((u64)c->r[0] | ((u64)c->r[4] << 32));
        u32 hp = c->r[1];
        s32 n = (s32)c->r[2];
        bool all = c->r[3] != 0;
        std::vector<std::shared_ptr<WaitObject>> objs;
        for (s32 i = 0; i < n; i++) {
            auto o = waitable(r32(hp + 4 * i));
            if (!o) {
                c->r[0] = RES_INVALID_HANDLE;
                return;
            }
            objs.push_back(o);
        }
        if (all) {
            bool ready = true;
            for (auto &o : objs)
                if (o->should_wait(t)) ready = false;
            if (ready) {
                for (auto &o : objs) o->acquire(t);
                c->r[0] = RES_OK;
                c->r[1] = 0;
                break;
            }
        } else {
            bool done = false;
            for (s32 i = 0; i < n; i++)
                if (!objs[i]->should_wait(t)) {
                    objs[i]->acquire(t);
                    objs[i]->after_wakeups();
                    c->r[0] = RES_OK;
                    c->r[1] = (u32)i;
                    done = true;
                    break;
                }
            if (done) break;
        }
        if (ns == 0 || n == 0) {
            c->r[0] = RES_TIMEOUT;
            if (n == 0 && ns != 0) {
                // waits for nothing: just sleep
                t->status = TStatus::WaitSleep;
                u64 gen = t->wait_gen;
                add_timeout(now_ns() + (u64)std::max<s64>(ns, 0), [t, gen] {
                    if (t->wait_gen == gen && t->status == TStatus::WaitSleep) resume_thread(t);
                } STATE_TIMEOUT(TimeoutKind::Sleep, t->shared_from_this(), nullptr, gen));
                block_current(lk);
            }
            break;
        }
        if (ns > 100000000) VLOG("[svc] thread %u waits on %d objects (%s) for %lld ms (lr %08x)\n", t->id, n, objs[0]->name.c_str(), (long long)(ns / 1000000), c->r[14]);
        wait_on(t, objs, all, ns, lk, c, true);
        break;
    }
    case 0x27: {  // DuplicateHandle(r1)
        auto o = get_object(c->r[1]);
        if (!o) { c->r[0] = RES_INVALID_HANDLE; break; }
        c->r[1] = add_handle(o);
        c->r[0] = RES_OK;
        break;
    }
    case 0x28: {
        u64 tk = system_tick();
        c->r[0] = (u32)tk;
        c->r[1] = (u32)(tk >> 32);
        break;
    }
    case 0x29:  // GetHandleInfo
        c->r[1] = 0;
        c->r[2] = 0;
        c->r[0] = RES_OK;
        break;
    case 0x2A: {  // GetSystemInfo(type=r1, param=r2) -> r1:r2
        u32 type = c->r[1], param = c->r[2];
        u64 v = 0;
        if (type == 0) {  // memory usage by region
            if (param == 1) v = g_game.app_memory;     // application
            else if (param == 2) v = 0x00A00000;       // system
            else if (param == 3) v = 0x00200000;       // base
            else v = g_game.app_memory + 0x00A00000 + 0x00200000;
        } else if (type == 2) {
            v = 0;
        } else if (type == 26) {
            v = 5;  // number of processes created by the kernel at boot
        }
        c->r[1] = (u32)v;
        c->r[2] = (u32)(v >> 32);
        c->r[0] = RES_OK;
        break;
    }
    case 0x2B: {  // GetProcessInfo(handle=r1, type=r2)
        u32 type = c->r[2];
        u64 v = 0;
        if (type == 0 || type == 2) v = heap_size + linear_used + 0x00400000;
        else if (type == 20) v = mem::FCRAM_PADDR - mem::LINEAR_BASE;  // linear address offset
        c->r[1] = (u32)v;
        c->r[2] = (u32)(v >> 32);
        c->r[0] = RES_OK;
        break;
    }
    case 0x2D: {  // ConnectToPort(name=r1)
        std::string name = read_cstr(c->r[1], 12);
        auto svc = find_port(name);
        if (!svc) {
            LOG("[svc] ConnectToPort(%s): no such port\n", name.c_str());
            c->r[0] = 0xD88007FA;
            break;
        }
        auto s = std::make_shared<Session>();
        s->service = svc;
        s->name = name;
        c->r[1] = add_handle(s);
        c->r[0] = RES_OK;
        break;
    }
    case 0x32: {  // SendSyncRequest(handle=r0)
        auto s = get<Session>(c->r[0]);
        if (!s) {
            LOG("[svc] SendSyncRequest: bad handle %08x\n", c->r[0]);
            c->r[0] = RES_INVALID_HANDLE;
            break;
        }
        c->r[0] = service_request(s, t, lk);
        reschedule(lk);
        break;
    }
    case 0x35:  // GetProcessId
        c->r[1] = 0x28;
        c->r[0] = RES_OK;
        break;
    case 0x37: {  // GetThreadId(r1)
        auto th = get<Thread>(c->r[1]);
        c->r[1] = th ? th->id : 0;
        c->r[0] = th ? RES_OK : RES_INVALID_HANDLE;
        break;
    }
    case 0x38:  // GetResourceLimit
        c->r[1] = add_handle(self_reslimit);
        c->r[0] = RES_OK;
        break;
    case 0x39:
    case 0x3A: {  // GetResourceLimit{Limit,Current}Values(values=r0, handle=r1, names=r2, count=r3)
        u32 vals = c->r[0], names = c->r[2], n = c->r[3];
        for (u32 i = 0; i < n; i++) {
            u32 name = r32(names + 4 * i);
            u64 v = 0;
            bool limit = imm == 0x39;
            switch (name) {
            case 0: v = limit ? 0x18 : 0x18; break;                                   // priority
            case 1: v = limit ? g_game.app_memory : heap_size + linear_used + 0x00400000; break;  // commit
            case 2: v = limit ? 32 : threads.size(); break;
            case 3: v = limit ? 32 : 8; break;
            case 4: v = limit ? 32 : 8; break;
            case 5: v = limit ? 32 : 8; break;
            case 6: v = limit ? 8 : 2; break;
            case 7: v = limit ? 16 : 4; break;
            case 8: v = limit ? 2 : 0; break;
            case 9: v = limit ? 32 : 2; break;
            case 10: v = limit ? 1000 : 0; break;
            }
            w64(vals + 8 * i, v);
        }
        c->r[0] = RES_OK;
        break;
    }
    case 0x3C: {  // Break: the game's panic (failed assertion, pure virtual call...)
        for (int i = 0; i < 16; i += 4)
            LOG("[svc] r%d-r%d: %08x %08x %08x %08x\n", i, i + 3, c->r[i], c->r[i + 1], c->r[i + 2], c->r[i + 3]);
        for (u32 a = c->r[13]; a < c->r[13] + 0x80; a += 16)
            LOG("[svc] stack %08x: %08x %08x %08x %08x\n", a, r32(a), r32(a + 4), r32(a + 8), r32(a + 12));
        // Said first, as the screen shows only the start: the last thing the game asked for was a system archive its build lacks, and it has
        // opened nothing since. Three testers' Tomodachi Life stopped here with only the numbers below (2026-11-10 to 12): the bad-word list.
        if (const u64 tid = g_missing_system_archive)
            FATAL("This game asked for %s from your 3DS (system archive %016llx), which its build doesn't have, and stopped. Build it again with "
                  "your 3DS system files.\n[svc] Break(%u) from thread %u, lr=%08x\n", system_archive_name(tid), (unsigned long long)tid, c->r[0], t->id, c->r[14]);
        FATAL("[svc] Break(%u) from thread %u, lr=%08x\n", c->r[0], t->id, c->r[14]);
    }
    case 0x3D: {
        std::string s((const char *)gptr(c->r[0]), (size_t)(s32)c->r[1]);
        LOG("[guest] %s\n", s.c_str());
        c->r[0] = RES_OK;
        break;
    }
    case 0x52:  // StoreProcessDataCache(process, address, size)
    case 0x53:  // FlushProcessDataCache
    case 0x54:  // InvalidateProcessDataCache
        VLOG("[svc] process cache %02x\n", imm);
        gsp_cache_op(c->r[1], c->r[2], imm == 0x54);
        c->r[0] = RES_OK;
        break;
    default:
        LOG("[svc] unimplemented SVC 0x%02x (lr=%08x)\n", imm, c->r[14]);
        c->r[0] = 0xF8C007F4;
        break;
    }
}

KStats kstats;
}  // namespace kernel
extern u64 rt_lookups, rt_lookup_misses;
namespace kernel {

std::string kstats_report(u64 frames) {
    Lock lk(kmutex);
    if (!frames) frames = 1;
    std::vector<std::pair<u64, u32>> s;
    u64 all = 0;
    for (u32 i = 0; i < 128; i++)
        if (kstats.svc[i]) s.push_back({kstats.svc[i], i}), all += kstats.svc[i];
    std::sort(s.rbegin(), s.rend());
    char b[160];
    snprintf(b, sizeof b, "[kstats] per frame: %.1f SVCs, %.1f switches, %.1f wakes from idle;", (double)all / frames, (double)kstats.switches / frames, (double)kstats.wakes / frames);
    std::string out = b;
    for (size_t i = 0; i < s.size() && i < 8; i++) {
        snprintf(b, sizeof b, " %02x:%.1f", s[i].second, (double)s[i].first / frames);
        out += b;
    }
    std::vector<std::pair<u64, std::string>> r;
    for (auto &[k, n] : kstats.requests) r.push_back({n, k});
    std::sort(r.rbegin(), r.rend());
    for (size_t i = 0; i < r.size() && i < 10; i++) {
        snprintf(b, sizeof b, "%s %s %.1f", i ? "," : " | requests:", r[i].second.c_str(), (double)r[i].first / frames);
        out += b;
    }
    static u64 previous_lookups, previous_misses;
    const u64 lookups = rt_lookups, misses = rt_lookup_misses;
    snprintf(b, sizeof b, " | indirect jumps %.1f, %.1f not in the cache", (double)(lookups - previous_lookups) / frames, (double)(misses - previous_misses) / frames);
    out += b;
    previous_lookups = lookups; previous_misses = misses;
    const bool ipc = kstats.ipc;
    kstats = KStats{};
    kstats.ipc = ipc;
    return out + "\n";
}

volatile u32 svcs;

void svc_dispatch(CPU *c, u32 imm) {
#ifdef __WIIU__
    const bool timed = detailed_diagnostics(current_thread());
    const u64 asked = timed ? real_ns() : 0;
    const u32 caller = c->r[14];
#endif
    while (lock_wanted.load(std::memory_order_relaxed)) std::this_thread::yield();  // lock_soon's caller first
    Lock lk(kmutex);
    Thread *const t = current_thread();
#ifdef __WIIU__
    const u64 entered = timed ? real_ns() : 0;
#endif
    t->in_svc = (s32)imm;
    t->in_service = nullptr;
    kstats.svc[imm & 127]++;
    svcs = svcs + 1;
    flush_deferred();
    static int svclog = getenv("RECOMP_SVCLOG") ? atoi(getenv("RECOMP_SVCLOG")) : -1;
    if (svclog >= 0 && (svclog == 0 || current_thread()->id == (u32)svclog))
        LOG("[svc %u] %02x lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", current_thread()->id, imm, c->r[14], c->r[0],
            c->r[1], c->r[2], c->r[3]);
    svc_locked(c, imm, lk);
#ifdef __WIIU__
    if (timed) {
        const u64 completed = real_ns();
#ifdef RECOMP_RESUMABLE
        runner_svc_ns += completed - asked;
#endif
        record_diagnostic(t, imm, caller, "lock-acquire", entered - asked);
        record_diagnostic(t, imm, caller, "svc", completed - entered);
        static u64 report_at;
        const u64 now = real_ns();
        if (now - report_at >= 2000000000ULL) {
            diagnostics_report_locked();
            report_at = now;
        }
    }
#endif
    t->in_svc = -1;
}

// The guest functions (f_<address>) on a thread's host stack, innermost first: a thread that spins without SVCs
// shows only a stale pc otherwise, and a waiting one shows where it waits from.
#ifdef __WIIU__
static std::string guest_backtrace(Thread *) { return ""; }
#else
static std::string guest_backtrace(Thread *t) {
    if (!t->mach || pthread_mach_thread_np(pthread_self()) == t->mach) return "";
    u64 pcs[64];
    int n = 0;
    if (thread_suspend(t->mach) != KERN_SUCCESS) return "";
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mach, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
        pcs[n++] = arm_thread_state64_get_pc(st);
        pcs[n++] = arm_thread_state64_get_lr(st);
        u64 fp = arm_thread_state64_get_fp(st);
        // frame records are {previous fp, return address}; stop at anything that does not look like one
        for (int i = 0; i < 60 && fp && !(fp & 7) && n < 64; i++) {
            u64 *rec = (u64 *)fp;
            pcs[n++] = rec[1];
            if (rec[0] <= fp) break;
            fp = rec[0];
        }
    }
    thread_resume(t->mach);
    std::string out;
    const void *last = nullptr;
    for (int i = 0; i < n; i++) {
        Dl_info di;
        if (!dladdr((void *)(pcs[i] & 0x0000FFFFFFFFFFFFull), &di) || !di.dli_sname) continue;
        if (strncmp(di.dli_sname, "f_", 2) || di.dli_saddr == last) continue;
        last = di.dli_saddr;
        out += out.empty() ? " in " : " < ";
        out += di.dli_sname + 2;
    }
    return out;
}
#endif

// Debugging (RECOMP_MEMDUMP): the data/bss segment and every mapped region, one file each
// (<prefix>_<address>.bin), for diffing snapshots taken at different moments
void dump_memory(const char *prefix) {
    Lock lk(kmutex);
    auto put = [&](u32 base, u32 size) {
        char name[256];
        snprintf(name, sizeof name, "%s_%08x.bin", prefix, base);
        if (FILE *f = fopen(name, "wb")) {
            fwrite(gptr(base), 1, size, f);
            fclose(f);
        }
    };
    put(g_game.data_addr, g_game.data_size + g_game.bss_size);
    for (auto &[base, r] : regions)
        if (r.perm & 1) put(base, r.size);
}

bool guest_idle() { return running == nullptr; }

void dump_threads() {
    Lock lk(kmutex);
    static const char *st[] = {"ready", "running", "wait-sync", "sleep", "wait-arb", "dormant", "dead"};
    if (!tq.q.empty()) LOG("[timers] %u pending, the next one %lld us from now\n", (u32)tq.q.size(), (long long)(tq.q.begin()->first - now_ns()) / 1000);
    for (auto &t : threads) {
        std::string w;
        for (auto &o : t->wait_objs) w += " " + (o->name.empty() ? std::string("?") : o->name);
        if (t->status != TStatus::Dead && t->status != TStatus::Dormant) w += guest_backtrace(t.get());
        LOG("[thread %u] prio %d %s pc=%08x lr=%08x sp=%08x arb=%08x%s\n", t->id, t->priority, st[(int)t->status],
            t->cpu.pc, t->cpu.r[14], t->cpu.r[13], t->arb_addr, w.c_str());
    }
}

// ---------------------------------------------------------------- boot
void start(const BootInfo &bi) {
    boot_ns = now_ns();
    linear_free[mem::LINEAR_BASE] = mem::LINEAR_SIZE - 0x00200000;  // (a little kept for the system)
#ifdef __WIIU__
    OSInitEvent(&tq_event, FALSE, OS_EVENT_MODE_AUTO);
    tq_event_ready = true;
    // From a megabyte clear of the program (its data and bss come last) to the heap. A game with modules has ldr:ro put them where it says,
    // anywhere in the code's region, which ends at 0x04000000: only what is past that is lent then. Pokemon Ultra Sun's field modules
    // landed on the host's threads, and the game froze as it loaded its first route (2026-11-13). Cold builds can lack generated slots,
    // so the original service ACL also decides whether the guest retains its module region.
    const u32 clear = ((g_game.data_addr + g_game.data_size + g_game.bss_size + 0xFFFFF) & ~0xFFFFFu) + 0x100000;
    mem::lend(code_memory_lend_begin(clear, ro_has_modules()), mem::HEAP_BASE);
#endif
#ifndef RECOMP_RESUMABLE
    tq.th = std::thread(timeout_thread);
#else
    // Interrupt long guest-only loops so the dispatcher can process timers.
    // This host pulse owns no guest state and is not part of a checkpoint.
    tq.th = std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            rt_preempt_flag = 1;
        }
    });
#endif
    tq.th.detach();
    u32 stack_top = mem::STACK_TOP;
    add_region(stack_top - bi.stack_size, bi.stack_size, 3, 7);
    Lock lk(kmutex);
    auto t = create_thread(bi.entry, 0, stack_top, bi.priority, true);
    t->name = "main";
    schedule_async();
}

}  // namespace kernel

#ifdef RECOMP_RESUMABLE
namespace kernel {
u32 persistent_sharedmem_callback(const SharedMem &);
bool persistent_sharedmem_callback(SharedMem &, u32);
bool capture_object(checkpoint::Image &im, const ObjPtr &o) {
    if (!o || !im.objects.insert(o.get()).second) return true;
    im.restore.push_back([o] {});  // pin identity until the session checkpoint is discarded
    im.take(o->name);
    if (auto w = std::dynamic_pointer_cast<WaitObject>(o)) im.take(w->waiters);
#define TAKE(P, F) im.take(P->F)
    switch (o->type()) {
    case Type::Event: { auto p = std::static_pointer_cast<Event>(o); TAKE(p, reset); TAKE(p, signaled); break; }
    case Type::Mutex: { auto p = std::static_pointer_cast<Mutex>(o); TAKE(p, owner); TAKE(p, lock_count); break; }
    case Type::Semaphore: { auto p = std::static_pointer_cast<Semaphore>(o); TAKE(p, count); TAKE(p, max); break; }
    case Type::Timer: { auto p = std::static_pointer_cast<Timer>(o); TAKE(p, reset); TAKE(p, signaled); TAKE(p, initial); TAKE(p, interval); TAKE(p, generation); break; }
    case Type::SharedMem: {
        auto p = std::static_pointer_cast<SharedMem>(o);
        TAKE(p, backing); TAKE(p, size); TAKE(p, my_perm); TAKE(p, other_perm);
        if (im.archive) {
            u32 kind = im.archive->reading ? 0 : persistent_sharedmem_callback(*p);
            im.archive->io(kind);
            if (kind > 1) { im.archive->fail("unidentified shared-memory callback"); return false; }
            if (im.archive->reading) im.restore.push_back([p, kind] { persistent_sharedmem_callback(*p, kind); });
        }
        break;
    }
    case Type::Thread: {
        auto p = std::static_pointer_cast<Thread>(o);
        if (p->in_svc != -1) return false;
        TAKE(p, id); TAKE(p, priority); TAKE(p, status); TAKE(p, cpu); TAKE(p, tls); TAKE(p, entry); TAKE(p, ready_seq);
        TAKE(p, wait_objs); TAKE(p, wait_all); TAKE(p, wait_out_index); TAKE(p, wait_result); TAKE(p, wait_index);
        TAKE(p, wait_gen); TAKE(p, arb_addr); TAKE(p, arb);
        if (im.archive && im.archive->reading) im.restore.push_back([p] { p->cpu.thread = p.get(); });
        for (auto &w : p->wait_objs) if (!capture_object(im, w)) return false;
        break;
    }
    case Type::Session: {
        auto p = std::static_pointer_cast<Session>(o);
        if (im.archive) { im.take(p->service); break; }
        if (im.objects.insert(p->service.get()).second && !p->service->capture(im)) {
            LOG("[state] unsupported service: %s\n", p->service->name.c_str()); return false;
        }
        break;
    }
    case Type::Arbiter: case Type::Process: case Type::ResLimit: break;
    default: return false;
    }
#undef TAKE
    return true;
}
}

bool checkpoint::kernel_capture(Image &im) {
    using namespace kernel;
    Lock lk(kmutex);
    if (ro_has_modules()) { LOG("[state] dynamic modules are unsupported\n"); return false; }
    im.take(handles); im.take(threads); im.take(running); im.take(next_handle); im.take(next_thread_id);
    im.take(ready_counter); im.take(tls_used); im.take(heap_size); im.take(linear_used);
    im.take(regions); im.take(linear_free); im.take(tq.q); im.take(deferred_work); im.take(boot_ns);
    const u64 clock = now_ns();
    im.restore.push_back([clock] {
        std::lock_guard<std::mutex> lk(clock_m); clock_vbase = clock; clock_rbase = real_ns();
    });
    im.take(clock_pct);
    // Retain all live object identities, including service-owned events whose
    // guest handle has been closed. Dead weak references cost no payload.
    for (auto &weak : checkpoint_objects()) if (auto o = weak.lock()) if (!capture_object(im, o)) return false;
    for (auto &t : threads) if (!capture_object(im, t)) return false;
    // Capture canonical guest ranges only. The unused code/heap gap is lent
    // to the host allocator on Wii U and must never be rewound as guest RAM.
    std::map<u32, u32> spans;
    auto span = [&](u32 base, u32 size) { if (size && !mem::lent(base, size)) spans[base] = std::max(spans[base], size); };
    for (auto &[base, r] : regions) if (r.perm & 1) span(base, r.size);
    span(g_game.text_addr, g_game.text_size); span(g_game.ro_addr, g_game.ro_size); span(g_game.data_addr, g_game.data_size + g_game.bss_size);
    // The free map covers the linear allocator's unused ranges. Capture its
    // allocated complement and the system reservation, not all 128 MiB.
    u32 linear_at = mem::LINEAR_BASE;
    for (auto [base, size] : linear_free) {
        if (base > linear_at) span(linear_at, base - linear_at);
        linear_at = std::max(linear_at, base + size);
    }
    if (linear_at < mem::LINEAR_BASE + mem::LINEAR_SIZE)
        span(linear_at, mem::LINEAR_BASE + mem::LINEAR_SIZE - linear_at);
    span(mem::VRAM_BASE, mem::VRAM_SIZE);
    span(mem::DSP_BASE, 0x90000); span(mem::HLE_BASE, mem::HLE_SIZE);
    u32 first = 0, end = 0;
    for (auto [base, size] : spans) {
        if (end && base > end) { im.memory(gptr(first), end - first); first = base; }
        if (!end) first = base;
        end = std::max(end, base + size);
    }
    if (end > first) im.memory(gptr(first), end - first);
    return im.ok;
}
#endif

extern "C" void rt_svc(CPU *c, u32 imm) { kernel::svc_dispatch(c, imm); }

volatile u32 rt_preempt_flag;

extern "C" void rt_preempt(CPU *) {
    using namespace kernel;
    Lock lk(kmutex);
    rt_preempt_flag = 0;
    flush_deferred();
    reschedule(lk);
}

#ifdef RECOMP_RESUMABLE
namespace kernel {
class PersistentGraph {
    checkpoint::Image &image;
    checkpoint::Archive &archive;
    std::vector<ObjPtr> objects{nullptr};
    std::vector<std::shared_ptr<Service>> services{nullptr};
    std::map<const Object *, u32> object_ids;
    std::map<const Service *, u32> service_ids;
public:
    explicit PersistentGraph(checkpoint::Image &im) : image(im), archive(*im.archive) {
        archive.object = [this](ObjPtr &p) { object(p); };
        archive.service = [this](std::shared_ptr<Service> &p) { service(p); };
    }
    ~PersistentGraph() { archive.object = {}; archive.service = {}; }
    void object(ObjPtr &p) {
        u32 id = 0;
        bool fresh = false;
        if (!archive.reading && p) {
            auto [it, inserted] = object_ids.emplace(p.get(), objects.size());
            id = it->second; fresh = inserted;
            if (fresh) objects.push_back(p);
        }
        archive.io(id);
        if (!archive.ok) return;
        if (!id) { if (archive.reading) p.reset(); return; }
        if (archive.reading) {
            if (id < objects.size()) { p = objects[id]; return; }
            if (id != objects.size() || id > 16384) { archive.fail("invalid object ID"); return; }
            fresh = true;
        }
        if (!fresh) return;
        Type type = archive.reading ? Type::Event : p->type();
        std::string name = archive.reading ? std::string{} : p->name;
        archive.fields(type, name);
        if (!archive.ok) return;
        if (archive.reading) {
            switch (type) {
            case Type::Event: p = std::make_shared<Event>(); break;
            case Type::Mutex: p = std::make_shared<Mutex>(); break;
            case Type::Semaphore: p = std::make_shared<Semaphore>(); break;
            case Type::Timer: p = std::make_shared<Timer>(); break;
            case Type::Thread: p = std::make_shared<Thread>(); break;
            case Type::SharedMem: p = std::make_shared<SharedMem>(); break;
            case Type::Session: p = std::make_shared<Session>(); break;
            case Type::Arbiter: p = std::make_shared<Arbiter>(); break;
            case Type::Process: p = std::make_shared<Process>(); break;
            case Type::ResLimit: p = std::make_shared<ResLimit>(); break;
            default: archive.fail("unsupported object kind"); return;
            }
            p->name = name;
            objects.push_back(p); // cycles can refer back before their fields are decoded
            checkpoint_objects().push_back(p);
        }
        if (!capture_object(image, p)) archive.fail("object cannot be persisted");
    }
    void service(std::shared_ptr<Service> &p) {
        u32 id = 0; bool fresh = false;
        if (!archive.reading && p) {
            auto [it, inserted] = service_ids.emplace(p.get(), services.size());
            id = it->second; fresh = inserted;
            if (fresh) services.push_back(p);
        }
        archive.io(id);
        if (!archive.ok) return;
        if (!id) { if (archive.reading) p.reset(); return; }
        if (archive.reading) {
            if (id < services.size()) { p = services[id]; return; }
            if (id != services.size() || id > 4096) { archive.fail("invalid service ID"); return; }
            fresh = true;
        }
        if (!fresh) return;
        std::string name = archive.reading ? std::string{} : p->name;
        archive.io(name);
        if (archive.reading && archive.ok) {
            p = persistent_service(name);
            if (!p) { archive.fail("service cannot be reconstructed"); return; }
            services.push_back(p);
        }
        if (archive.ok && !p->capture(image)) archive.fail("service cannot be persisted");
    }
};
}
#endif

#ifdef RECOMP_RESUMABLE
bool checkpoint::kernel_persist(Image &im) {
    using namespace kernel;
    Lock lk(kmutex);
    if (!im.archive || ro_has_modules()) return false;
    flush_deferred();
    im.archive_graph = std::make_shared<PersistentGraph>(im);
    im.fields(handles, threads, running, self_process, self_reslimit, next_handle, next_thread_id,
              ready_counter, tls_used, heap_size, linear_used, regions, linear_free, tq.q, boot_ns, clock_pct);
    u64 clock = now_ns();
    im.archive->io(clock);
    im.restore.push_back([clock] {
        std::lock_guard<std::mutex> lk(clock_m); clock_vbase = clock; clock_rbase = real_ns();
    });
    std::vector<std::pair<u32,u32>> spans;
    if (!im.archive->reading) {
        std::map<u32,u32> selected;
        auto span = [&](u32 base, u32 size) { if (size && !mem::lent(base,size)) selected[base] = std::max(selected[base],size); };
        for (auto &[base,r] : regions) if (r.perm & 1) span(base,r.size);
        span(g_game.text_addr,g_game.text_size); span(g_game.ro_addr,g_game.ro_size);
        span(g_game.data_addr,g_game.data_size+g_game.bss_size);
        u32 at=mem::LINEAR_BASE;
        for (auto [base,size] : linear_free) { if (base>at) span(at,base-at); at=std::max(at,base+size); }
        if (at<mem::LINEAR_BASE+mem::LINEAR_SIZE) span(at,mem::LINEAR_BASE+mem::LINEAR_SIZE-at);
        span(mem::VRAM_BASE,mem::VRAM_SIZE); span(mem::DSP_BASE,0x90000); span(mem::HLE_BASE,mem::HLE_SIZE);
        u32 begin=0,end=0;
        for (auto [base,size] : selected) {
            if (end && base>end) { spans.emplace_back(begin,end-begin); begin=base; }
            if (!end) begin=base;
            end=std::max(end,base+size);
        }
        if (end>begin) spans.emplace_back(begin,end-begin);
    }
    im.archive->io(spans);
    u64 end=0;
    for (auto [base,size] : spans) {
        if (!im.archive->ok) break;
        if (!size || base<0x100000 || base<end || (u64)base+size>0x20000000ull || mem::lent(base,size)) {
            im.archive->fail("invalid guest memory span"); break;
        }
        end=(u64)base+size;
        im.memory(gptr(base),size);
    }
    return im.ok && im.archive->ok;
}
#endif
