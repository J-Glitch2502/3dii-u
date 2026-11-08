// Self-check for port/aac/aac_ffmpeg.cpp: decodes an ADTS file frame by frame, as pipe 3 is fed, to raw 16 bit stereo, for comparing with a reference decoder.
// clang++-18 -std=c++20 -O1 -I/root/aac_pc/include tools/aac_check.cpp port/aac/aac_ffmpeg.cpp /root/aac_pc/lib/libavcodec.a /root/aac_pc/lib/libavutil.a -lm -o /root/aac_check
// aac_check <in.aac> <out.raw>
#include <cstdio>
#include <vector>

extern "C" int aac_host_decode(const unsigned char *src, unsigned size, short *left, short *right, unsigned *rate, unsigned *channels);

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    if (!f || !o) return 2;
    std::vector<unsigned char> d;
    for (int c; (c = fgetc(f)) != EOF;) d.push_back((unsigned char)c);
    static short l[2048], r[2048];
    unsigned rate = 0, channels = 0, frames = 0, bad = 0;
    for (size_t p = 0; p + 7 <= d.size();) {
        if (d[p] != 0xff || (d[p + 1] & 0xf6) != 0xf0) { printf("lost sync at %zu\n", p); return 1; }
        const unsigned len = (d[p + 3] & 3) << 11 | d[p + 4] << 3 | d[p + 5] >> 5;
        const int n = aac_host_decode(&d[p], len, l, r, &rate, &channels);
        if (n <= 0) bad++;
        for (int i = 0; i < n; i++) {
            const short s[2] = {l[i], r[i]};
            fwrite(s, 2, 2, o);
        }
        frames++;
        p += len;
    }
    printf("%u frames, %u undecoded, %u Hz, %u channels\n", frames, bad, rate, channels);
    return bad ? 1 : 0;
}
