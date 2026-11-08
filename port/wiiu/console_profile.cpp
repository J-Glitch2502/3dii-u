// A profile for the real console, where no tool can see in: a timer on each core notes where it interrupted the code, 100 times a second.
// "prof" from the PC puts the busiest addresses of each core on the log; tools/profile_report.py names the functions from that build's
// game.nm. Cemu doesn't say where its timers interrupted, so a profile from it means nothing. Only runs with "profile" as a debug flag
// ("test profile" from the PC: one boot on a save of its own): it's never been confirmed safe on the console, and a build that had it
// always on hung at start-up there.
#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/core.h>
#include <coreinit/time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "rt.h"
#include "wiiu.h"

namespace {
struct Slot {
    u32 key, count;
};
constexpr u32 SLOTS = 1 << 15;
Slot *table[3];
OSAlarm alarms[3];

bool wanted() {
    static const bool on = [] {
        wiiu::fs_begin(0xC00C);
        const bool f = wiiu::debug_flag("profile");
        wiiu::fs_end();
        return f;
    }();
    return on;
}

void hit(u32 core, u32 key) {
    Slot *t = table[core];
    u32 i = ((key >> 4) * 0x9E3779B1u) >> 17;
    for (u32 n = 0; n < 64; n++, i = (i + 1) & (SLOTS - 1)) {
        if (t[i].key == key) {
            t[i].count++;
            return;
        }
        if (!t[i].key) {
            t[i].key = key;
            t[i].count = 1;
            return;
        }
    }
}

void on_alarm(OSAlarm *, OSContext *c) {
    // our code starts at 0x02000000; in the system's, what counts is who called it (the low bit says so)
    const u32 pc = c->srr0;
    hit(OSGetCoreId(), pc >= 0x02000000 && pc < 0x10000000 ? pc & ~15u : c->lr | 1);
}
}  // namespace

namespace wiiu {
void profile_this_core() {
    if (!wanted()) return;
    const u32 core = OSGetCoreId();
    table[core] = (Slot *)calloc(SLOTS, sizeof(Slot));
    OSAlarm &a = alarms[core];
    OSCreateAlarm(&a);
    OSSetPeriodicAlarm(&a, OSGetTime() + (OSTime)OSMillisecondsToTicks(10), OSMillisecondsToTicks(10), on_alarm);
}

void profile_stop() {
    if (!wanted()) return;
    for (OSAlarm &a : alarms) OSCancelAlarm(&a);
}

// (net thread) "prof": each core's busiest addresses on the log, as "[prof] core <n> <address> <samples>"; the counts go on while it reads
void profile_send() {
    if (!wanted()) {
        LOG("[prof] off: \"test profile\" turns it on for one boot\n");
        return;
    }
    for (u32 core = 0; core < 3; core++) {
        if (!table[core]) continue;
        std::vector<Slot> s;
        u64 all = 0;
        for (u32 i = 0; i < SLOTS; i++)
            if (table[core][i].count) s.push_back(table[core][i]), all += table[core][i].count;
        std::sort(s.begin(), s.end(), [](const Slot &a, const Slot &b) { return a.count > b.count; });
        LOG("[prof] core %u: %llu samples\n", core, (unsigned long long)all);
        for (size_t i = 0; i < s.size() && i < 300; i++) LOG("[prof] core %u %08x %u\n", core, s[i].key, s[i].count);
    }
}

void profile_write() {
    if (!wanted()) return;
    std::vector<u32> out{0x50524F46u};
    for (u32 core = 0; core < 3; core++) {
        const size_t at = out.size();
        out.push_back(0);
        for (u32 i = 0; table[core] && i < SLOTS; i++)
            if (table[core][i].count) {
                out.push_back(table[core][i].key);
                out.push_back(table[core][i].count);
                out[at]++;
            }
    }
    if (FILE *f = fopen((data_dir() + "/profile.bin").c_str(), "wb")) {
        fwrite(out.data(), 4, out.size(), f);
        fclose(f);
    }
}
}  // namespace wiiu
