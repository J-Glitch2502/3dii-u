// The GamePad's microphone for mic:u. Holding the right stick in counts as blowing into it, for when the mic isn't practical.
// mic.rpl comes from OSDynLoad, as the sockets do in net_console.cpp: only a game that asks for the mic loads it, and without it blowing still works. A big build importing it hung before main.
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <malloc.h>
#include <mic/mic.h>

#include <algorithm>
#include <cstring>

#include "services.h"
#include "wiiu.h"

namespace {
constexpr u32 SAMPLES = 0x2800;
// shortcut: assumed, mic.rpl doesn't report its rate; only pitch depends on it, so measure it if a game ever needs that
constexpr u32 HOST_RATE = 32000;
MICWorkMemory work;
MICHandle handle = -1;
volatile bool blowing;
u32 noise = 12345;
decltype(&MICInit) mic_init;
decltype(&MICOpen) mic_open;
decltype(&MICGetStatus) mic_get_status;
decltype(&MICSetDataConsumed) mic_set_data_consumed;
decltype(&MICClose) mic_close;

bool load_mic() {
    OSDynLoad_Module m;
    if (OSDynLoad_Acquire("mic.rpl", &m) != OS_DYNLOAD_OK) return false;
    const struct {
        const char *name;
        void **where;
    } wanted[] = {{"MICInit", (void **)&mic_init},
                  {"MICOpen", (void **)&mic_open},
                  {"MICGetStatus", (void **)&mic_get_status},
                  {"MICSetDataConsumed", (void **)&mic_set_data_consumed},
                  {"MICClose", (void **)&mic_close}};
    for (const auto &w : wanted)
        if (OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, w.name, w.where) != OS_DYNLOAD_OK || !*w.where) return false;
    return true;
}
}  // namespace

void wiiu::mic_blow(bool on) { blowing = on; }

void mic_host_start() {
    wiiu::note("starting the microphone");
    if (handle < 0) {
        if (!load_mic()) {
            LOG("[mic] mic.rpl didn't load\n");
            return;
        }
        work.sampleMaxCount = SAMPLES;
        work.sampleBuffer = memalign(0x40, SAMPLES * 2);
        MICError err = MIC_ERROR_INIT;
        if (work.sampleBuffer) handle = mic_init(MIC_INSTANCE_0, 0, &work, &err);
        if (err != MIC_ERROR_OK) {
            LOG("[mic] no GamePad microphone (error %d)\n", (int)err);
            handle = -1;
            return;
        }
    }
    const MICError err = mic_open(handle);
    if (err != MIC_ERROR_OK) LOG("[mic] could not open the GamePad microphone (error %d)\n", (int)err);
    wiiu::note("microphone started: %d", (int)err);
}

void mic_host_stop() {
    if (handle >= 0) mic_close(handle);
}

void mic_host_read(s16 *out, u32 n, u32 rate) {
    memset(out, 0, n * 2);
    MICStatus st;
    if (handle >= 0 && mic_get_status(handle, &st) == MIC_ERROR_OK && st.availableData > 0) {
        const u32 have = (u32)st.availableData, need = (u64)n * HOST_RATE / rate;
        // the newest samples only: a game that fell behind wants to hear now, not a backlog
        const u32 from = (u32)st.bufferPos + (have > need ? have - need : 0), got = std::min(have, need);
        DCInvalidateRange(work.sampleBuffer, SAMPLES * 2);
        const s16 *in = (const s16 *)work.sampleBuffer;
        for (u32 i = 0; i < n; i++) {
            const u32 at = (u64)i * HOST_RATE / rate;
            if (at >= got) break;
            out[i] = in[(from + at) % SAMPLES];
        }
        mic_set_data_consumed(handle, (int)have);
    }
    if (blowing) {
        for (u32 i = 0; i < n; i++) {
            noise = noise * 1664525u + 1013904223u;
            out[i] = (s16)(noise >> 16);
        }
    }
}
