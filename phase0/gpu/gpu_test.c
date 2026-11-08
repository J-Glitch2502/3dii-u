// Phase 0 GPU test: can the Wii U draw 3DS-style scenes at 1800x1080 on the TV plus 640x480 on the GamePad, using a shader that
// does what the PICA200 does per pixel? The target is 8 ms per frame for a typical scene, leaving time at 60 fps for the rest.
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_MS 8.0
#define FRAMES 120
#define TV_W 1800
#define TV_H 1080
#define DRC_W 640
#define DRC_H 480
#define COVERAGE_SAMPLES 4096

extern const uint8_t pica_gsh[];
__asm__(".section .rodata\n.balign 32\n.global pica_gsh\npica_gsh:\n.incbin \"build/gpu/pica.gsh\"\n.previous\n");

typedef struct {
    float pos[3], normal[3], uv[2];
} Vertex;

typedef struct {
    const char *name;
    uint32_t grid, layers;
    Vertex *vertices;
    uint32_t *indices;
    uint32_t vertex_bytes, index_count;
} Scene;

static Scene scenes[] = {{"light", 50, 1}, {"typical", 100, 2}, {"heavy", 173, 3}};
#define NSCENES (sizeof scenes / sizeof scenes[0])

static char report[2048];
static size_t report_len;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report_len += (size_t)vsnprintf(report + report_len, sizeof report - report_len, fmt, ap);
    va_end(ap);
    if (report_len < sizeof report - 1) report[report_len++] = '\n';
}

// A grid across the whole viewport, once per layer. Layers draw back to front, so depth testing can't skip the overdraw.
static void build_scene(Scene *s) {
    uint32_t verts_per_layer = (s->grid + 1) * (s->grid + 1);
    s->vertex_bytes = verts_per_layer * s->layers * sizeof(Vertex);
    s->index_count = s->grid * s->grid * 6 * s->layers;
    s->vertices = MEMAllocFromDefaultHeapEx(s->vertex_bytes, GX2_VERTEX_BUFFER_ALIGNMENT);
    s->indices = MEMAllocFromDefaultHeapEx(s->index_count * sizeof(uint32_t), GX2_INDEX_BUFFER_ALIGNMENT);
    Vertex *v = s->vertices;
    uint32_t *ix = s->indices;
    for (uint32_t layer = 0; layer < s->layers; layer++) {
        uint32_t base = layer * verts_per_layer;
        float z = 0.9f - 0.2f * (float)layer;
        for (uint32_t y = 0; y <= s->grid; y++) {
            for (uint32_t x = 0; x <= s->grid; x++) {
                float fx = (float)x / (float)s->grid, fy = (float)y / (float)s->grid;
                float wave = sinf(fx * 12.0f + (float)layer) * cosf(fy * 9.0f);
                *v++ = (Vertex){{fx * 2.0f - 1.0f, fy * 2.0f - 1.0f, z},
                                {wave * 0.5f, sinf(fy * 7.0f) * 0.5f, 0.7f},
                                {fx * 4.0f, fy * 4.0f}};
            }
        }
        for (uint32_t y = 0; y < s->grid; y++) {
            for (uint32_t x = 0; x < s->grid; x++) {
                uint32_t a = base + y * (s->grid + 1) + x, b = a + 1, c = a + s->grid + 1, d = c + 1;
                *ix++ = a; *ix++ = b; *ix++ = c;
                *ix++ = b; *ix++ = d; *ix++ = c;
            }
        }
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, s->vertices, s->vertex_bytes);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, s->indices, s->index_count * sizeof(uint32_t));
}

static void make_texture(GX2Texture *t, uint32_t w, uint32_t h, uint32_t seed) {
    memset(t, 0, sizeof *t);
    t->surface.width = w;
    t->surface.height = h;
    t->surface.depth = 1;
    t->surface.mipLevels = 1;
    t->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    t->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    t->surface.use = GX2_SURFACE_USE_TEXTURE;
    t->surface.aa = GX2_AA_MODE1X;
    t->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;  // the slow case for sampling: the real port can tile textures
    t->viewNumMips = 1;
    t->viewNumSlices = 1;
    t->compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&t->surface);
    GX2InitTextureRegs(t);
    t->surface.image = MEMAllocFromDefaultHeapEx(t->surface.imageSize, t->surface.alignment);
    uint32_t *px = t->surface.image;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < t->surface.pitch; x++) {
            uint32_t r = (x * 255 / w) ^ seed, g = (y * 255 / h) ^ (seed >> 8), b = ((x ^ y) * 3) & 255;
            px[y * t->surface.pitch + x] = ((r & 255) << 24) | ((g & 255) << 16) | (b << 8) | 255;
        }
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, t->surface.image, t->surface.imageSize);
}

static uint32_t swap32(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return __builtin_bswap32(u);
}

typedef struct {
    uint32_t stage[6][4];
    uint32_t konst[6][4];
    uint32_t light0[4], light1[4], fog[4];
} TevBlock;

static TevBlock *make_tev(void) {
    // A plausible six-stage setup: texture times lighting, add a detail texture, add specular, then blends.
    static const int stages[6][4] = {{2, 0, 0, 1}, {5, 3, 6, 4}, {5, 1, 0, 2}, {5, 4, 6, 6}, {5, 6, 0, 3}, {5, 0, 3, 7}};
    TevBlock *t = MEMAllocFromDefaultHeapEx((sizeof(TevBlock) + 255) & ~255u, GX2_UNIFORM_BLOCK_ALIGNMENT);
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 4; j++) {
            t->stage[i][j] = __builtin_bswap32((uint32_t)stages[i][j]);
            t->konst[i][j] = swap32(0.4f + 0.1f * (float)j);
        }
    }
    const float l0[4] = {0.5f, 0.6f, 0.7f, 0}, l1[4] = {-0.6f, 0.2f, 0.5f, 0}, fog[4] = {0.2f, 0.3f, 0.5f, 1};
    for (int j = 0; j < 4; j++) {
        t->light0[j] = swap32(l0[j]);
        t->light1[j] = swap32(l1[j]);
        t->fog[j] = swap32(fog[j]);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK, t, sizeof *t);
    return t;
}

static WHBGfxShaderGroup group;
static GX2Texture textures[3];
static GX2Sampler sampler;
static TevBlock *tev;

static void draw_scene(const Scene *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetViewport((float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f);
    GX2SetScissor(x, y, w, h);
    GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAttribBuffer(0, s->vertex_bytes, sizeof(Vertex), s->vertices);
    GX2SetPixelUniformBlock(0, sizeof(TevBlock), tev);
    for (uint32_t i = 0; i < 3; i++) {
        GX2SetPixelTexture(&textures[i], i);
        GX2SetPixelSampler(&sampler, i);
    }
    GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, s->index_count, GX2_INDEX_TYPE_U32, s->indices, 0, 1);
}

// Proves pixels were really drawn: a blank screen must not pass as a fast one.
static uint32_t coverage_percent(const GX2ColorBuffer *b, uint32_t clear) {
    const uint32_t *image = b->surface.image;
    uint32_t words = b->surface.imageSize / 4, hit = 0;
    DCInvalidateRange((void *)image, b->surface.imageSize);
    for (uint32_t i = 0; i < COVERAGE_SAMPLES; i++) {
        if (image[(uint64_t)i * words / COVERAGE_SAMPLES] != clear) hit++;
    }
    return hit * 100 / COVERAGE_SAMPLES;
}

static int run_scene(Scene *s) {
    double total = 0, worst = 0;
    uint32_t coverage = 0;
    const GX2ColorBuffer *tv = WHBGfxGetTVColourBuffer();
    for (uint32_t frame = 0; frame < FRAMES; frame++) {
        if (!WHBProcIsRunning()) return 0;
        WHBGfxBeginRender();
        OSTime start = OSGetSystemTime();
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0, 0, 0, 1);
        draw_scene(s, (tv->surface.width - TV_W) / 2, (tv->surface.height - TV_H) / 2, TV_W, TV_H);
        WHBGfxFinishRenderTV();
        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0, 0, 0, 1);
        draw_scene(s, (854 - DRC_W) / 2, 0, DRC_W, DRC_H);
        WHBGfxFinishRenderDRC();
        GX2Flush();
        GX2DrawDone();
        double ms = (double)OSTicksToMicroseconds(OSGetSystemTime() - start) / 1000.0;
        if (frame >= 10) {  // the first frames include shader and state warm-up
            total += ms;
            if (ms > worst) worst = ms;
        }
        if (frame == FRAMES - 1) coverage = coverage_percent(tv, 0x000000FF);
        WHBGfxFinishRender();
    }
    double avg = total / (FRAMES - 10);
    note("%-8s %6u tris x%u: avg %6.2f ms, worst %6.2f ms, drawn %u%%", s->name, s->index_count / 3 / s->layers, s->layers, avg, worst,
         coverage);
    if (s == &scenes[1]) {
        note("GPU: %s (target %.0f ms for a typical scene)", coverage < 50 ? "INVALID, the frame wasn't drawn" : avg <= TARGET_MS ? "PASS" : "FAIL",
             TARGET_MS);
    }
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBGfxInit();
    const GX2ColorBuffer *tv = WHBGfxGetTVColourBuffer();
    note("TV buffer %ux%u, drawing %dx%d plus GamePad %dx%d", tv->surface.width, tv->surface.height, TV_W, TV_H, DRC_W, DRC_H);
    int ok = tv->surface.height >= TV_H;
    if (!ok) note("Your Wii U's TV output isn't set to 1080p, so this can't test it. Set it in System Settings, then rerun.");
    if (ok && !WHBGfxLoadGFDShaderGroup(&group, 0, pica_gsh)) {
        note("Couldn't load the shaders.");
        ok = 0;
    }
    if (ok) {
        WHBGfxInitShaderAttribute(&group, "aPos", 0, offsetof(Vertex, pos), GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
        WHBGfxInitShaderAttribute(&group, "aNormal", 0, offsetof(Vertex, normal), GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
        WHBGfxInitShaderAttribute(&group, "aUv", 0, offsetof(Vertex, uv), GX2_ATTRIB_FORMAT_FLOAT_32_32);
        WHBGfxInitFetchShader(&group);
        make_texture(&textures[0], 256, 256, 0x51);
        make_texture(&textures[1], 128, 128, 0xA7);
        make_texture(&textures[2], 256, 2, 0x13);
        GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);
        tev = make_tev();
        for (size_t i = 0; i < NSCENES; i++) build_scene(&scenes[i]);
        for (size_t i = 0; i < NSCENES && run_scene(&scenes[i]); i++) {
        }
    }
    WHBGfxShutdown();

    WHBLogConsoleInit();
    FILE *f = fopen("fs:/vol/external01/3ds_wiiu_gpu_test.txt", "w");
    if (f) {
        fwrite(report, 1, report_len, f);
        fclose(f);
    }
    char *line = strtok(report, "\n");
    while (line) {
        WHBLogPrint(line);
        line = strtok(NULL, "\n");
    }
    WHBLogPrint(f ? "Saved to SD: 3ds_wiiu_gpu_test.txt. Press HOME to exit." : "SD save failed, please photograph this. Press HOME to exit.");
    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
