// Internal interfaces of the DSP firmware HLE (codecs, resampling, filters, sources, mixers).
#pragma once
#include <array>
#include <vector>

#include "dsp_shared.h"

namespace audio::hle {

using StereoFrame = std::array<Sample, kFrameSamples>;
using QuadFrame = std::array<std::array<s32, 4>, kFrameSamples>;

// ---- codecs (codec.cpp)
struct AdpcmState {
    s16 yn1 = 0, yn2 = 0;
};
// GC-ADPCM: 8-byte frames (header + 14 nibbles). Decodes `count` mono samples into out (duplicated
// to both channels) and advances the history in `st`.
void decode_adpcm(const u8 *data, size_t count, const s16 coeffs[16], AdpcmState &st, Sample *out);
void decode_pcm8(unsigned channels, const u8 *data, size_t count, Sample *out);
void decode_pcm16(unsigned channels, const u8 *data, size_t count, Sample *out);
// Pointer to `bytes` bytes of guest physical memory starting at paddr, or nullptr if the range is
// not entirely inside one mapped region (FCRAM, VRAM, DSP RAM).
const u8 *phys_range(u32 paddr, u64 bytes);

// ---- resampling (interp.cpp)
// Fixed point position with 24 fractional bits. Output sample k is interpolated between the
// virtual input samples floor(pos) and floor(pos)+1 of [x[-2], x[-1], in...], i.e. there is a
// two-sample predelay and the history carries across calls and buffers.
struct InterpState {
    Sample xn3{}, xn2{}, xn1{};  // x[n-3], x[n-2], x[n-1]
    u64 fpos = 0;
};
// Writes output samples from out[outi] up to out[outn-1] (or until the input is exhausted) and
// returns how many input samples were consumed. When the input runs out it is fully consumed.
size_t interpolate(InterpState &st, Interp mode, float rate, const Sample *in, size_t n, Sample *out,
                   size_t &outi, size_t outn);
// Number of input samples needed to produce `outputs` more samples (so a caller can bound reads).
size_t interp_input_needed(const InterpState &st, float rate, size_t outputs);

// ---- per-source filters (source.cpp)
struct SourceFilters {
    bool simple_on = false, biquad_on = false;
    s32 s_b0 = 1 << 15, s_a1 = 0;
    std::array<s32, 2> s_y1{};
    s32 b_b0 = 1 << 14, b_b1 = 0, b_b2 = 0, b_a1 = 0, b_a2 = 0;
    std::array<s32, 2> b_x1{}, b_x2{}, b_y1{}, b_y2{};

    void enable(bool simple, bool biquad);
    void reset_simple();
    void reset_biquad();
    void process(StereoFrame &f);
};

// ---- a voice (source.cpp)
class Source {
public:
    void reset() { st = {}; frame = {}; }
    // Parses the configuration (clearing its dirty flags), generates this frame and returns the
    // status to report.
    // seen: the game takes this frame (generate_frame); if not, its settings are left unread. publish: this frame's status goes to the game (the game
    // took it, and submitted since the last one): "buffer changed" is kept for the next one until then
    SourceStatus tick(SourceConfig &cfg, const s16 coeffs[16], bool seen, bool publish);
    void mix_into(QuadFrame &dst, int mix);
    const StereoFrame &output() const { return frame; }
    void report_timing(unsigned index);  // DSP lock held; bounded source summary

private:
    struct Buffer {
        u32 paddr = 0, length = 0;
        u16 adpcm_yn[2]{};
        bool adpcm_dirty = false, looping = false;
        u16 id = 0;
        unsigned channels = 1;
        Format format = Format::PCM16;
        bool from_queue = false;
        u32 play_position = 0;
        bool has_played = false;
    };
    struct State {
        bool enabled = false;
        bool has_emitted = false;  // distinguish a running stream from first start
        u16 sync_count = 0;
        std::array<std::array<float, 4>, 3> gain{}, gain_ramp_start{};
        std::array<bool, 3> gain_ramp{};
        std::vector<Buffer> queue;  // sorted by buffer id (lowest first), FIFO among equal ids
        unsigned channels = 1;
        Format format = Format::ADPCM;
        // current buffer
        bool have_buffer = false;   // a buffer is loaded and has samples left
        u32 pos = 0;                // samples of the current buffer consumed so far
        u32 cur_paddr = 0, cur_length = 0;
        unsigned cur_channels = 1;
        Format cur_format = Format::ADPCM;
        bool cur_looping = false;
        std::vector<Sample> decoded;  // ADPCM samples decoded so far (decoded on demand)
        u32 adpcm_decoded = 0;        // a multiple of 14 or cur_length; PCM is read live
        bool buffer_update = false;
        bool update_unpublished = false;  // the current buffer changed since the game was last given a status
        u16 last_buffer_id = 0, current_buffer_id = 0;
        std::array<s16, 16> adpcm_coeffs{};
        AdpcmState adpcm{};
        float rate = 1.0f;
        Interp interp = Interp::Polyphase;
        InterpState interp_state{};
        SourceFilters filters{};
    };
    State st;
    StereoFrame frame{};
    struct {
        u32 frames = 0, blank_starts = 0, boundary_blanks = 0, empty = 0, short_frames = 0;
    } timing;

#ifdef RECOMP_RESUMABLE
public:
    template<class A> void state_fields(A &a) {
        a.fields(st.enabled, st.has_emitted, st.sync_count, st.gain, st.gain_ramp_start, st.gain_ramp);
        u32 count = a.length(st.queue.size());
        if (a.reading && a.ok) st.queue.resize(count);
        for (auto &b : st.queue) a.fields(b.paddr, b.length, b.adpcm_yn, b.adpcm_dirty, b.looping, b.id, b.channels, b.format, b.from_queue, b.play_position, b.has_played);
        a.fields(st.channels, st.format, st.have_buffer, st.pos, st.cur_paddr, st.cur_length, st.cur_channels, st.cur_format, st.cur_looping);
        a.fields(st.decoded, st.adpcm_decoded, st.buffer_update, st.update_unpublished, st.last_buffer_id, st.current_buffer_id, st.adpcm_coeffs);
        a.fields(st.adpcm.yn1, st.adpcm.yn2, st.rate, st.interp, st.interp_state.xn3, st.interp_state.xn2, st.interp_state.xn1, st.interp_state.fpos);
        auto &f = st.filters;
        a.fields(f.simple_on, f.biquad_on, f.s_b0, f.s_a1, f.s_y1, f.b_b0, f.b_b1, f.b_b2, f.b_a1, f.b_a2, f.b_x1, f.b_x2, f.b_y1, f.b_y2, frame);
    }
private:
#endif
    void parse_config(SourceConfig &cfg, const s16 coeffs[16]);
    void enqueue(const Buffer &b);
    bool dequeue();
    void generate();
    size_t fetch(Sample *out, size_t max);  // copies upcoming samples of the current buffer
    u32 cur_remaining() const { return st.pos < st.cur_length ? st.cur_length - st.pos : 0; }
};

// ---- final mixer (mixer.cpp)
class Mixer {
public:
    void reset() { st = {}; frame = {}; }
    void tick(DspConfig &cfg, const IntermediateMix (&read)[2], IntermediateMix (&write)[2],
              const std::array<QuadFrame, 3> &input, bool fresh = true);
    const StereoFrame &output() const { return frame; }

private:
    struct Delay {
        bool enabled = false;
        float g = 0, a = 0, b = 0;
        std::vector<float> line[4];
        std::array<float, 4> last{};
        size_t pos = 0;
        void parse(DelayConfig &c);
        void process(QuadFrame &f);
    };
    struct State {
        std::array<float, 3> volume{};
        std::array<bool, 2> aux_enable{};
        std::array<QuadFrame, 3> mix{};
        std::array<QuadFrame, 2> aux_return{};
        Delay delay[2];
        u16 output_format = 1;
    };
    State st;
    StereoFrame frame{};
#ifdef RECOMP_RESUMABLE
public:
    template<class A> void state_fields(A &a) {
        a.fields(st.volume, st.aux_enable, st.mix, st.output_format, frame);
        for (auto &d : st.delay) a.fields(d.enabled, d.g, d.a, d.b, d.line, d.last, d.pos);
    }
#endif
};

}  // namespace audio::hle
