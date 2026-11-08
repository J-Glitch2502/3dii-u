// Audio output: two looping AX voices (left and right) play ring buffers that the DSP HLE fills, on the TV and the GamePad.
#include <coreinit/cache.h>
#include <malloc.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "audio/host.h"
#include "audio_iface.h"

namespace {
constexpr u32 RING = 8192;     // samples per channel
constexpr u32 CUSHION = 1600;  // aimed distance from what plays to what is written: about 50 ms
s16 *ring[2];
AXVoice *voice[2];
u32 write_pos;
bool playing;
std::atomic<float> gain{1.0f};

void start_voices() {
    for (int c = 0; c < 2; c++) {
        AXVoiceBegin(voice[c]);
        AXSetVoiceState(voice[c], AX_VOICE_STATE_PLAYING);
        AXVoiceEnd(voice[c]);
    }
    playing = true;
}

u32 play_pos() {
    AXVoiceOffsets off;
    AXGetVoiceOffsets(voice[0], &off);
    return off.currentOffset % RING;
}
}  // namespace

namespace audio {
void init() {
    AXInitParams params{};
    params.renderer = AX_INIT_RENDERER_48KHZ;
    params.pipeline = AX_INIT_PIPELINE_SINGLE;
    AXInitWithParams(&params);
    for (int c = 0; c < 2; c++) {
        ring[c] = (s16 *)memalign(64, RING * 2);
        memset(ring[c], 0, RING * 2);
        DCFlushRange(ring[c], RING * 2);
        voice[c] = AXAcquireVoice(31, nullptr, nullptr);
        if (!voice[c]) {
            LOG("[audio] no voice available\n");
            return;
        }
        AXVoiceBegin(voice[c]);
        AXSetVoiceType(voice[c], 0);
        AXVoiceVeData ve{};
        ve.volume = 0x8000;
        AXSetVoiceVe(voice[c], &ve);
        // channel 0 is left, channel 1 is right, on both devices
        AXVoiceDeviceMixData mix[6];
        memset(mix, 0, sizeof mix);
        mix[c].bus[0].volume = 0x8000;
        AXSetVoiceDeviceMix(voice[c], AX_DEVICE_TYPE_TV, 0, mix);
        AXSetVoiceDeviceMix(voice[c], AX_DEVICE_TYPE_DRC, 0, mix);
        AXVoiceOffsets off{};
        off.dataType = AX_VOICE_FORMAT_LPCM16;
        off.loopingEnabled = AX_VOICE_LOOP_ENABLED;
        off.loopOffset = 0;
        off.endOffset = RING - 1;
        off.currentOffset = 0;
        off.data = ring[c];
        AXSetVoiceOffsets(voice[c], &off);
        AXSetVoiceSrcType(voice[c], AX_VOICE_SRC_TYPE_LINEAR);
        AXSetVoiceSrcRatio(voice[c], 32728.0f / AXGetInputSamplesPerSec());
        AXVoiceEnd(voice[c]);
    }
    write_pos = CUSHION;
}
void set_volume(float v) { gain = v; }
// silence while the HOME Menu is up: the voices would otherwise repeat what's in the ring
void pause(bool on) {
    if (!playing || !voice[0] || !voice[1]) return;
    for (int c = 0; c < 2; c++) {
        AXVoiceBegin(voice[c]);
        AXSetVoiceState(voice[c], on ? AX_VOICE_STATE_STOPPED : AX_VOICE_STATE_PLAYING);
        AXVoiceEnd(voice[c]);
    }
}
void shutdown() {
    for (AXVoice *&v : voice)
        if (v) {
            AXFreeVoice(v);
            v = nullptr;
        }
    AXQuit();
}

namespace hle {
void host_output(const Sample *samples, size_t n) {
    if (!voice[0] || !voice[1]) return;
    if (playing) {
        // keep a steady distance: on an underrun or a pile-up, jump to where the cushion should be
        const u32 distance = (write_pos + RING - play_pos()) % RING;
        if (distance < 160 || distance > RING / 2) write_pos = (play_pos() + CUSHION) % RING;
    }
    const float g = gain;
    for (size_t i = 0; i < n; i++) {
        const u32 at = write_pos;
        ring[0][at] = g == 1.0f ? samples[i][0] : (s16)(samples[i][0] * g);
        ring[1][at] = g == 1.0f ? samples[i][1] : (s16)(samples[i][1] * g);
        write_pos = (write_pos + 1) % RING;
    }
    // the written span, which may wrap
    const u32 end = write_pos, start = (end + RING - (u32)n) % RING;
    for (int c = 0; c < 2; c++) {
        if (start < end) {
            DCFlushRange(ring[c] + start, n * 2);
        } else {
            DCFlushRange(ring[c] + start, (RING - start) * 2);
            DCFlushRange(ring[c], end * 2);
        }
    }
    if (!playing && write_pos >= CUSHION * 2) start_voices();
}
int host_queued() { return playing ? (int)((write_pos + RING - play_pos()) % RING) : -1; }
}  // namespace hle
}  // namespace audio
