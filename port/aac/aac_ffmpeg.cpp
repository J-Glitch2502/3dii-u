// FFmpeg's AAC decoder behind the firmware's pipe 3 (runtime/audio/dsp_core.cpp), linked only into the games with AAC music. tools/aac_build.sh builds
// the libraries. One ADTS frame in, 16 bit samples out per channel.
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <cstdio>
#include <cstring>

#ifdef __WIIU__
// libavutil's file opening, which decoding never reaches, asks for this, and wut's brings in its socket library: network imports and a connection attempt at every start of the game
extern "C" int fcntl(int, int, ...) { return -1; }
#endif

static AVCodecContext *ctx;
static AVPacket *pkt;
static AVFrame *frame;

static short to_s16(float x) {
    const int v = (int)(x * 32768.0f + (x >= 0 ? 0.5f : -0.5f));
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (short)v;
}

extern "C" void aac_host_reset() {
    if (ctx) avcodec_flush_buffers(ctx);
}

// the samples per channel written, or 0 if the frame can't be decoded
extern "C" int aac_host_decode(const unsigned char *src, unsigned size, short *left, short *right, unsigned *rate, unsigned *channels) {
    static unsigned char in[8192 + AV_INPUT_BUFFER_PADDING_SIZE];  // the decoder reads a little past its input; an ADTS frame's length is 13 bits
    if (!ctx) {
        av_log_set_level(AV_LOG_QUIET);  // a bad frame gives silence, and the caller says so
        const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_AAC);
        ctx = avcodec_alloc_context3(c);
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        if (!ctx || !pkt || !frame || avcodec_open2(ctx, c, nullptr) < 0) {
            fprintf(stderr, "aac: the decoder didn't start\n");
            avcodec_free_context(&ctx);
            return 0;
        }
    }
    if (size == 0 || size > 8192) return 0;
    memcpy(in, src, size);
    memset(in + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt->data = in;
    pkt->size = size;
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0 || frame->format != AV_SAMPLE_FMT_FLTP || frame->nb_samples > 2048) return 0;
    const float *l = (const float *)frame->extended_data[0], *r = frame->channels > 1 ? (const float *)frame->extended_data[1] : l;
    for (int i = 0; i < frame->nb_samples; i++) {
        left[i] = to_s16(l[i]);
        right[i] = to_s16(r[i]);
    }
    *rate = frame->sample_rate;
    *channels = frame->channels;
    return frame->nb_samples;
}
