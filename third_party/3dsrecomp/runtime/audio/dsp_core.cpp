// DSP firmware HLE: pipes, power state changes and the per-frame processing (audio:: interface).
#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "../audio_iface.h"
#include "dsp_hle.h"
#include "host.h"

namespace audio::hle {

namespace {

enum class Power { Off, On, Sleeping };

struct Dsp {
    std::mutex lock;  // guards everything below (pipe calls come from game threads, tick from a timer)
    Power power = Power::Off;
    std::vector<u8> pipes[kNumPipes];
    std::array<Source, kNumSources> sources;
    Mixer mixer;
    // state saved across sleep
    std::vector<u8> backup_ram;
    std::array<Source, kNumSources> backup_sources;
    Mixer backup_mixer;
    u64 frames = 0;
    u16 last_counter = 0;  // the game's newest frame counter at the last frame
    bool fresh = true;     // the last frame worked on a frame the game submitted since the one before
};

Dsp &dsp() {
    static Dsp *d = new Dsp;  // never destroyed: the timer thread may still tick at exit
    return *d;
}

u8 *dsp_ram() { return g_mem + mem::DSP_BASE; }
SharedMemory &region(int i) { return *(SharedMemory *)(dsp_ram() + (i ? kRegion1 : kRegion0)); }

u16 frame_counter(int i) {
    u16 v;
    memcpy(&v, (const u8 *)&region(i) + offsetof(SharedMemory, frame_counter), 2);
    return le16(v);
}

// The application submits the region whose frame counter is newer (with wraparound).
int read_region_index() {
    const u16 c0 = frame_counter(0), c1 = frame_counter(1);
    if (c0 == 0xFFFF && c1 != 0xFFFE) return 1;
    if (c1 == 0xFFFF && c0 != 0xFFFE) return 0;
    return c0 > c1 ? 0 : 1;
}

void push_u16(std::vector<u8> &p, u16 v) {
    p.push_back((u8)v);
    p.push_back((u8)(v >> 8));
}

void write_struct_addresses(Dsp &d) {
    static constexpr u16 addrs[] = {
        dsp_word_addr(offsetof(SharedMemory, frame_counter)),
        dsp_word_addr(offsetof(SharedMemory, source_config)),
        dsp_word_addr(offsetof(SharedMemory, source_status)),
        dsp_word_addr(offsetof(SharedMemory, adpcm_coeffs)),
        dsp_word_addr(offsetof(SharedMemory, dsp_config)),
        dsp_word_addr(offsetof(SharedMemory, dsp_status)),
        dsp_word_addr(offsetof(SharedMemory, final_samples)),
        dsp_word_addr(offsetof(SharedMemory, intermediate)),
        dsp_word_addr(offsetof(SharedMemory, compressor)),
        dsp_word_addr(offsetof(SharedMemory, dsp_debug)),
        dsp_word_addr(offsetof(SharedMemory, unknown10)),
        dsp_word_addr(offsetof(SharedMemory, unknown11)),
        dsp_word_addr(offsetof(SharedMemory, unknown12)),
        dsp_word_addr(offsetof(SharedMemory, unknown13)),
        dsp_word_addr(offsetof(SharedMemory, unknown14)),
    };
    static_assert(sizeof(addrs) / sizeof(addrs[0]) == 15);
    auto &p = d.pipes[2];
    push_u16(p, 15);
    for (u16 a : addrs) push_u16(p, a);
}

void reset_pipes(Dsp &d) {
    for (auto &p : d.pipes) p.clear();
}

void initialize(Dsp &d) {
    memset(dsp_ram(), 0, kDspRamSize);
    d.mixer.reset();
    for (auto &s : d.sources) s.reset();
    reset_pipes(d);
}

void sleep(Dsp &d) {
    d.backup_ram.assign(dsp_ram(), dsp_ram() + kDspRamSize);
    d.backup_sources = d.sources;
    d.backup_mixer = d.mixer;
}

void wakeup(Dsp &d) {
    if (d.backup_ram.size() == kDspRamSize) memcpy(dsp_ram(), d.backup_ram.data(), kDspRamSize);
    d.backup_ram.clear();
    d.sources = d.backup_sources;
    d.mixer = d.backup_mixer;
    for (auto &s : d.backup_sources) s.reset();
    d.backup_mixer.reset();
    reset_pipes(d);
}

// seen: the game is waiting for this frame. One it isn't waiting for (it's late: reading a file, held up by another thread) still plays what
// its voices have queued, as a 3DS's DSP goes on playing whatever the game does, but leaves the shared memory's settings and status alone: the
// game may be in the middle of them, and a status it never read would lose it a "buffer finished" (a stream then reloads a buffer still
// queued and the game hangs walking the voice's list). The next frame it takes reports what happened meanwhile.
StereoFrame generate_frame(Dsp &d, bool seen) {
    const int ri = read_region_index();
    SharedMemory &rd = region(ri);
    SharedMemory &wr = region(ri ^ 1);

    // Has the game submitted a frame since the last one (or none yet)? Only such a frame's status goes to the game, with an interrupt
    // (services/dsp.cpp: dsp_tick), as on a 3DS, where every frame works on one the game submitted just before. A frame between two of
    // its submissions only plays on: its status would go into the region the game may still be reading (it reads the one it doesn't
    // submit next), and a status changing halfway through gave Luigi's Mansion: Dark Moon one channel of a stereo stream newer news
    // than the other. The stream then queued a buffer again that was still in the second channel's list, and the game hung walking it.
    // A frame the game doesn't take (seen) is never fresh, and leaves the game's submission to the frame it does take.
    const u16 counter = frame_counter(ri);
    d.fresh = seen && (counter != d.last_counter || (frame_counter(0) == 0 && frame_counter(1) == 0));
    if (seen) d.last_counter = counter;

    std::array<QuadFrame, 3> mixes{};
    for (int i = 0; i < kNumSources; i++) {
        s16 coeffs[16];
        for (int c = 0; c < 16; c++) coeffs[c] = rd.adpcm_coeffs[i][c];
        SourceStatus s = d.sources[i].tick(rd.source_config[i], coeffs, seen, d.fresh);
        if (d.fresh) memcpy(&wr.source_status[i], &s, sizeof(s));
        for (int m = 0; m < 3; m++) d.sources[i].mix_into(mixes[m], m);
    }

    d.mixer.tick(rd.dsp_config, rd.intermediate, wr.intermediate, mixes);
    DspStatusBlock status{};
    if (d.fresh) memcpy(&wr.dsp_status, &status, sizeof(status));

    StereoFrame out = d.mixer.output();
    for (int f = 0; f < kFrameSamples; f++) {
        wr.final_samples[f][0] = out[f][0];
        wr.final_samples[f][1] = out[f][1];
    }
    d.frames++;
    return out;
}

}  // namespace

}  // namespace audio::hle

namespace audio {
using namespace hle;

// Builds without a decoder give silence, so that choosing those tracks can't leave the game waiting: port/aac/aac_ffmpeg.cpp has the real ones.
extern "C" __attribute__((weak, noinline)) void aac_host_reset() {}
extern "C" __attribute__((weak, noinline)) int aac_host_decode(const unsigned char *, unsigned, short *, short *, unsigned *, unsigned *) { return 0; }

// Pipe 3 is the firmware's AAC decoder, used only for the Mario & Luigi remakes' optional classic soundtracks. A request is 32 bytes: codec and
// command (u16 each), result (u32), then for a decode: source, size and the two channels' buffers, by physical address. A reply repeats the codec and
// command, then gives the rate as an index, the channels, the size it was given and, in the last word, the samples per channel.
static void aac_request(Dsp &d, const u8 *data) {
    static short out[2][2048];
    static const u32 rates[9] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    u8 reply[32] = {};
    memcpy(reply, data, 4);
    VLOG("dsp: AAC codec %u command %u, %08x %08x %08x %08x\n", data[0] | data[1] << 8, data[2] | data[3] << 8, le32(*(u32 *)(data + 8)), le32(*(u32 *)(data + 12)), le32(*(u32 *)(data + 16)), le32(*(u32 *)(data + 20)));
    if ((data[2] | data[3] << 8) == 1) {
        u32 w[4];  // source, size, buffers
        memcpy(w, data + 8, sizeof w);
        const u32 size = le32(w[1]);
        const u8 *in = mem::paddr_ptr(le32(w[0]));
        unsigned rate = 44100, channels = 2;
        int n = in ? aac_host_decode(in, size, out[0], out[1], &rate, &channels) : 0;
        if (n <= 0) {
            static bool logged;
            if (!logged) LOG("dsp: an AAC frame wasn't decoded: silence instead\n");
            logged = true;
            n = 1024;
            memset(out, 0, sizeof out);
        } else {
            // the loudest sample of the first 200 frames: a log from a console shows there's sound, and it should match a PC's
            static u32 frames;
            static int peak;
            if (frames == 0) LOG("dsp: AAC decoding, %u Hz, %u channels, %d samples a frame\n", rate, channels, n);
            if (frames < 200)
                for (int i = 0; i < n; i++) peak = std::max(peak, std::abs((int)out[0][i]));
            if (++frames == 200) LOG("dsp: AAC peak level %d\n", peak);
        }
        for (u32 ch = 0; ch < 2; ch++)
            if (u8 *p = mem::paddr_ptr(le32(w[2 + ch])))
                for (int i = 0; i < n; i++) {
                    const u16 v = le16((u16)out[ch][i]);
                    memcpy(p + i * 2, &v, 2);
                }
        u32 index = 1;
        for (u32 i = 0; i < 9; i++)
            if (rates[i] == rate) index = i;
        const u32 r[6] = {le32(index), le32(channels), w[1], 0, 0, le32((u32)n)};
        memcpy(reply + 8, r, sizeof r);
    } else {
        aac_host_reset();
    }
    d.pipes[3].insert(d.pipes[3].end(), reply, reply + sizeof reply);
}

void pipe_write(u32 pipe, const u8 *data, u32 size) {
    Dsp &d = dsp();
    std::lock_guard lk(d.lock);
    if (pipe == 3 && size == 32) {
        aac_request(d, data);
        return;
    }
    if (pipe != 2) {
        LOG("dsp: write of %u bytes to unsupported pipe %u ignored\n", size, pipe);
        return;
    }
    if (size != 4) {
        LOG("dsp: unexpected audio pipe write of %u bytes\n", size);
        return;
    }
    switch (data[0]) {
    case 0:  // initialize
        VLOG("dsp: initialize\n");
        initialize(d);
        write_struct_addresses(d);
        d.power = Power::On;
        break;
    case 1:  // shutdown
        VLOG("dsp: shutdown\n");
        d.power = Power::Off;
        break;
    case 2:  // wakeup
        VLOG("dsp: wakeup\n");
        wakeup(d);
        write_struct_addresses(d);
        d.power = Power::On;
        break;
    case 3:  // sleep
        VLOG("dsp: sleep\n");
        sleep(d);
        write_struct_addresses(d);
        d.power = Power::Sleeping;
        break;
    default:
        LOG("dsp: unknown state change %u\n", data[0]);
        d.power = Power::Off;
        break;
    }
}

std::vector<u8> pipe_read(u32 pipe, u32 size) {
    Dsp &d = dsp();
    std::lock_guard lk(d.lock);
    if (pipe >= kNumPipes) return {};
    auto &p = d.pipes[pipe];
    size = std::min<u32>({size, (u32)p.size(), 0xFFFFu});
    std::vector<u8> out(p.begin(), p.begin() + size);
    p.erase(p.begin(), p.begin() + size);
    return out;
}

u32 pipe_readable(u32 pipe) {
    Dsp &d = dsp();
    std::lock_guard lk(d.lock);
    return pipe < kNumPipes ? (u32)d.pipes[pipe].size() : 0;
}

u16 recv_data(u32 /*reg*/) {
    Dsp &d = dsp();
    std::lock_guard lk(d.lock);
    return d.power == Power::On ? 0 : 1;
}

bool tick(bool seen, bool *fresh) {
    Dsp &d = dsp();
    StereoFrame out;
    {
        std::lock_guard lk(d.lock);
        if (d.power != Power::On) return false;
        out = generate_frame(d, seen);
        if (fresh) *fresh = d.fresh;
    }
    host_output(out.data(), out.size());
    return true;
}

}  // namespace audio
