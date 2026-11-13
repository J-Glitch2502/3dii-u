// The PICA200 on GX2. Modeled on runtime/gpu/metal.mm, with the same division of labour: registers are decoded by the shared code
// in runtime/gpu, and this file turns each draw into GPU state. Shaders are compiled ahead of time (tools/shadergen_main.cpp); a
// draw whose vertex program has no compiled shader is shaded on the CPU instead, and logged so that a later build can include it.
//
// Surfaces hold PICA window rows: row 0 is y = 0, the bottom of the image, which is also how textures are sampled (t = 0 at row
// 0). So a render target can be sampled as a texture as it is. Screens are rotated into place when they are presented.
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gfd.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
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
#include <gx2/utils.h>
#include <malloc.h>
#include <vpad/input.h>
#include <whb/gfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpu/pica.h"
#include "audio_iface.h"
#include "gpu_iface.h"
#include "services/services.h"
#include "kernel.h"
#include "loader.h"
#include "present_rule.h"
#include "shader_compile.h"
#include "shadergen.h"
#include "swkbd.h"
#include "vertex_arrays.h"
#include "wiiu.h"

extern "C" void *GfxHeapAllocMEM1(uint32_t size, uint32_t alignment) __attribute__((weak));
extern "C" void GfxHeapFreeMEM1(void *block) __attribute__((weak));
// in gx2.rpl, and wut's import library, but not its headers
extern "C" void GX2SampleTopGPUCycle(uint64_t *cycle);
extern "C" void GX2SampleBottomGPUCycle(uint64_t *cycle);
extern "C" uint64_t GX2GPUTimeToCPUTime(uint64_t time);

namespace wiiu {
static void relieve_mem1();  // fast memory found full the first time (beside tv_buffer_to_mem2, which it uses)
}

namespace gpu {
u64 shipped_set_id;  // which shipped set this build has (a hash of shaders.idx): the console's own compile cache goes with it (shader_compile.cpp)
namespace {

// ---------------------------------------------------------------- timers: where a draw's time goes, for the log
enum { P_KEY, P_BIND, P_PS, P_STATE, P_UNI, P_TEX, P_VSUNI, P_RANGE, P_ATTR, P_DRAW, P_HASH, P_WAIT, P_PRESENT, P_SYNC, P_TEXUP, P_TEXFL, P_COUNT };
u32 prof[P_COUNT], st_syncs, st_luts, st_flushes, st_rehashes;
// The clock is read for these only in a build that measures (kMeasure): the time now, the time since then added to one of the counters
// above, and a draw's steps one after another (lap). "nolaps" in the data folder leaves the steps out of a measuring build, to see
// what reading the clock a dozen times a draw costs.
bool prof_laps = true;
inline OSTime prof_now() { return kMeasure ? OSGetSystemTime() : 0; }
inline void prof_add(int k, OSTime since) {
    if (kMeasure) prof[k] += (u32)(OSGetSystemTime() - since);
}
// ---- the GPU's own time, per pass: where the console's GPU spends a frame (the "[prof] GPU" line)
// A pass is the draws into one target, a clear, a copy, or the present. Each starts with two stamps from the GPU's clock: the cycle its command processor
// reached it (top of the pipe) and the cycle everything before it had finished (bottom). A pass took from the later of its own two to the next pass's
// bottom, which leaves out time the GPU sat waiting for commands. Read back three flips later, when the GPU is done with them.
enum { GT_DRAW, GT_FILL, GT_TRANSFER, GT_COPY, GT_LCD, GT_PRESENT, GT_END, GT_KINDS };
const char *const gt_kind_name[GT_KINDS] = {"draw", "clear", "transfer", "copy", "lcd", "present", "end"};
constexpr u32 GT_FRAMES = 4, GT_MARKS = 128, GT_SUMS = 48;
struct GtFrame {
    u64 *top = nullptr, *bot = nullptr;  // in memory the GPU writes
    u32 tag[GT_MARKS];
    u8 kind[GT_MARKS];
    u32 n = 0;
};
GtFrame gt[GT_FRAMES];
u32 gt_cur;
bool gt_on = true, gt_broken;
struct GtSum {
    u32 tag;
    u8 kind;
    u64 ticks;
    u32 count;
};
GtSum gt_sum[GT_SUMS];  // since the last report: written by the GPU thread, read by the report's (a stat, so a torn read now and then doesn't matter)
volatile u32 gt_nsum, gt_frames;
volatile u64 gt_busy, gt_span;
volatile bool gt_reset;
OSTime gt_last_end;

void gt_mark(u8 kind, u32 tag) {
    if (!kMeasure || !gt_on || gt_broken) return;
    GtFrame &f = gt[gt_cur];
    if (!f.top) {
        f.top = (u64 *)MEMAllocFromDefaultHeapEx(GT_MARKS * 16, 0x100);
        if (!f.top) {
            gt_broken = true;
            return;
        }
        f.bot = f.top + GT_MARKS;
        memset(f.top, 0, GT_MARKS * 16);
        DCFlushRange(f.top, GT_MARKS * 16);
    }
    if (f.n && f.kind[f.n - 1] == kind && f.tag[f.n - 1] == tag) return;  // the same pass, going on
    if (f.n >= GT_MARKS - 1 && kind != GT_END) return;
    if (f.n >= GT_MARKS) return;
    GX2SampleTopGPUCycle(&f.top[f.n]);
    GX2SampleBottomGPUCycle(&f.bot[f.n]);
    f.kind[f.n] = kind;
    f.tag[f.n] = tag;
    f.n++;
}

void gt_add(u8 kind, u32 tag, u64 ticks) {
    const u32 n = gt_nsum;
    for (u32 i = 0; i < n; i++)
        if (gt_sum[i].kind == kind && gt_sum[i].tag == tag) {
            gt_sum[i].ticks += ticks;
            gt_sum[i].count++;
            return;
        }
    if (n < GT_SUMS) {
        gt_sum[n] = {tag, kind, ticks, 1};
        gt_nsum = n + 1;
    }
}

// At the end of a present, once the GPU has finished the frame before last: the oldest frame's stamps are read, and its slot is the next frame's.
void gt_flip(u32 first_target) {
    if (!kMeasure || !gt_on || gt_broken) return;
    gt_mark(GT_END, 0);
    gt_cur = (gt_cur + 1) % GT_FRAMES;
    GtFrame &f = gt[gt_cur];
    if (gt_reset) {
        gt_nsum = 0;
        gt_frames = 0;
        gt_busy = gt_span = 0;
        gt_reset = false;
    }
    if (f.n >= 2 && f.top) {
        DCInvalidateRange(f.top, GT_MARKS * 16);
        bool ok = true;
        for (u32 i = 0; i < f.n; i++)
            if (!f.top[i] || !f.bot[i]) ok = false;  // not written: the GPU isn't done after all
        if (ok) {
            u64 busy = 0;
            for (u32 i = 0; i + 1 < f.n; i++) {
                const OSTime from = (OSTime)GX2GPUTimeToCPUTime(std::max(f.top[i], f.bot[i]));
                const OSTime to = (OSTime)GX2GPUTimeToCPUTime(f.bot[i + 1]);
                const u64 t = to > from ? (u64)(to - from) : 0;
                busy += t;
                gt_add(f.kind[i], f.tag[i], t);
            }
            const OSTime end = (OSTime)GX2GPUTimeToCPUTime(f.bot[f.n - 1]);
            if (gt_last_end && end > gt_last_end) gt_span = gt_span + (u64)(end - gt_last_end);
            gt_last_end = end;
            gt_busy = gt_busy + busy;
            gt_frames = gt_frames + 1;
        }
        memset(f.top, 0, GT_MARKS * 16);
        DCFlushRange(f.top, GT_MARKS * 16);
    }
    f.n = 0;
    gt_mark(GT_DRAW, first_target);  // draws before the first change of target go to the target still bound
}

OSTime prof_t;
inline void lap_start() {
    if (kMeasure && prof_laps) prof_t = OSGetSystemTime();
}
inline void lap(int k) {
    if (!kMeasure || !prof_laps) return;
    const OSTime n = OSGetSystemTime();
    prof[k] += (u32)(n - prof_t);
    prof_t = n;
}
// waits for the GPU to finish everything so far, which is what really costs when it happens during a frame
inline void draw_done() {
    const OSTime t = prof_now();
    GX2DrawDone();
    prof_add(P_SYNC, t);
    st_syncs++;
}

// ---------------------------------------------------------------- per-frame memory
// Vertex, index and uniform data the GPU reads later. Each frame uses its own third; before reusing one, wait for the GPU to
// have finished the frame that last used it.
constexpr u32 RING_SIZE = 24 << 20;
u8 *ring;
u32 ring_off, ring_end, ring_seg;
OSTime ring_fence[3];
u32 dyn_waited;  // ticks waited for the GPU since dyn_res_update last looked

void ring_next_segment() {
    // What has been submitted by now is the last that can read this segment, or the one before it: a draw whose data filled the end of that
    // one was only submitted from this one, after that one's fence was taken.
    const OSTime submitted = GX2GetLastSubmittedTimeStamp();
    ring_fence[ring_seg] = submitted;
    ring_fence[(ring_seg + 2) % 3] = submitted;
    ring_seg = (ring_seg + 1) % 3;
    const OSTime tw = OSGetSystemTime();
    if (ring_fence[ring_seg]) GX2WaitTimeStamp(ring_fence[ring_seg]);
    const u32 waited = (u32)(OSGetSystemTime() - tw);
    if (kMeasure) prof[P_WAIT] += waited;
    dyn_waited += waited;  // (this one the picture's size goes by: dyn_res_update)
    ring_off = RING_SIZE / 3 * ring_seg;
    ring_end = ring_off + RING_SIZE / 3;
    GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, ring, RING_SIZE);  // the GPU may have cached this memory's old contents
}

void *ring_alloc(u32 size, u32 align) {
    u32 at = (ring_off + align - 1) & ~(align - 1);
    if (size + align > RING_SIZE / 3) FATAL("%u bytes asked of the per-frame memory, more than a segment holds\n", size);
    if (at + size > ring_end) {
        GX2Flush();
        ring_next_segment();
        at = (ring_off + align - 1) & ~(align - 1);
    }
    ring_off = at + size;
    // all of this gets written, so the cache needn't read it from memory first, which on this CPU takes longer than writing it
    for (u32 p = (at + 31) & ~31u; p + 32 <= at + size; p += 32) asm volatile("dcbz 0,%0" : : "r"(ring + p));
    return ring + at;
}

inline u32 swap_float(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return __builtin_bswap32(u);
}

// ---------------------------------------------------------------- shaders
struct VsEntry {
    GX2VertexShader *shader = nullptr;
    u32 index = 0, inputs = 0;
    s32 location[16];  // input register -> attribute location
};
struct PsEntry {
    GX2PixelShader *shader = nullptr;
    u32 index = 0;
};
constexpr u32 COMPILED_HERE = 0x7ffffffe;  // a VsEntry or PsEntry index: the console compiled it, nothing to load from the shipped set
constexpr u32 MISSED = ~0u;                // the set lacks it: the fallback draws, or the CPU does, until the console has compiled it
std::unordered_map<u64, VsEntry> vs_table;
std::unordered_map<u64, PsEntry> ps_table;
GX2VertexShader *vs_passthrough, *vs_blit;
GX2PixelShader *ps_blit, *ps_fallback, *ps_fallback_tex, *ps_uber, *ps_uber_lit;
GX2PixelShader *ps_decode[14];        // by texture format: decodes it on the GPU (gpu_decode); null in a pack from before it had that one
// A bit per attribute format (s8, u8, s16, float) kept the old way: copied by itself, integers as floats made by the CPU. Files named "cpuattr"
// (all), "cpuattr8" and "cpuattr16" set them, and "cemu" the signed bytes. None by default: bytes fetched a 4-byte word at a time (gpu_fetched)
// draw NSMB2's skinned models as the CPU path does, in Cemu (2026-11-11); fetched a byte at a time they stretched across the screen there.
u32 gxt_cpu_attr;
bool in_cemu;  // check_ports.py's runs say so with a file named "cemu": where Cemu can't do what a console does, the runtime shows what the console would
bool gxt_cpu_tex;                     // a file named "cputex" in the data folder: every texture decoded by the CPU, as before
// A vertex shader the pack lacks is compiled on the console, as pixel shaders are; a file named "noglslvs" leaves its draws with the CPU.
// It was off: in Cemu the compiler hands back an empty program (16 bytes, no inputs) for whatever it compiles in its first second, and NSMB2's
// Koopalings drew untextured with those (2026-11-10). Empty ones are turned away and compiled again now (shader_compile.cpp), and what it
// then makes matches the pack's. Left with the CPU, a place no build's runs had reached is unplayable: in Kid Icarus's land battle the pack
// lacked eleven, and the CPU shaded 47,000 vertices a frame, 3.5 fps in Cemu and about 2 on the user's console (2026-11-11).
static bool glsl_vs = true;
static bool glsl_force;  // "glslforce": the console's compiler is asked for every pixel shader the shipped set has too, which puts it under a real load in Cemu (it keeps the shipped ones)
volatile bool uber_all;  // "uberall" (a file in the data folder, or set from the PC): every draw uses the uber shaders, which shows up a mistake in them
bool uber_fallback;  // a fragment state with no compiled shader draws with them
bool stale_check;  // a file named "stalecheck": hashes each texture once a flip and logs those whose bytes changed with no notice. The renderer only looks again when told.
// what the card thread still has to add to shader_misses.bin: this thread must never wait for the card
std::mutex miss_m;
std::vector<u8> miss_buf;
void miss_put(const void *p, size_t n) {
    std::lock_guard<std::mutex> lk(miss_m);
    miss_buf.insert(miss_buf.end(), (const u8 *)p, (const u8 *)p + n);
}
volatile u32 gpu_busy_us;  // time the GPU thread spent working, not waiting
u32 vs_ubo_loc, ps_ubo_loc;
u64 st_draws, st_cpu_draws, st_ps_misses, st_uploads, st_upload_bytes, st_flips;
double st_upload_ms;

std::vector<u8> read_whole(const char *path, u32 align_to) {
    std::vector<u8> d;
    FILE *f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f) + align_to);
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size() - align_to, f);
    fclose(f);
    d.resize(d.size() - align_to);
    return d;
}

// shaders.gsh is read as each shader is needed, not kept whole: with every lit state compiled ahead, Luigi's Mansion: Dark Moon's is 51 MB, which
// stayed in a heap that a long session filled up. At boot the file is read once to note where each shader's header and program blocks are; a shader
// is then two reads into a small GFD of its own, which WHB copies out of (its program in memory of its own), and which goes again.
struct GfdShader {
    u32 head_at, head_len, prog_at, prog_len;  // each block with its block header
};
std::vector<GfdShader> gfd_vs, gfd_ps;
std::vector<u8> gfd_file_head, gfd_end;  // the file's header and its end block
FILE *gfd_file;

static void gfd_scan(const u8 *d, u32 n) {
    constexpr u32 VS_HEAD = 3, VS_PROG = 5, PS_HEAD = 6, PS_PROG = 7, END = 1;
    auto be32 = [&](u32 at) { return (u32)d[at] << 24 | (u32)d[at + 1] << 16 | (u32)d[at + 2] << 8 | d[at + 3]; };
    if (n < 32 || memcmp(d, "Gfx2", 4)) FATAL("shaders.gsh is not a GFD file\n");
    const u32 first = be32(4);
    gfd_file_head.assign(d, d + first);
    for (u32 at = first; at + 32 <= n && !memcmp(d + at, "BLK{", 4);) {
        const u32 head = be32(at + 4), kind = be32(at + 16), size = be32(at + 20), len = head + size;
        if (kind == VS_HEAD) gfd_vs.push_back({at, len, 0, 0});
        else if (kind == VS_PROG && !gfd_vs.empty()) gfd_vs.back().prog_at = at, gfd_vs.back().prog_len = len;
        else if (kind == PS_HEAD) gfd_ps.push_back({at, len, 0, 0});
        else if (kind == PS_PROG && !gfd_ps.empty()) gfd_ps.back().prog_at = at, gfd_ps.back().prog_len = len;
        else if (kind == END) gfd_end.assign(d + at, d + at + len);
        at += len;
    }
}

// one shader's blocks as a GFD file of their own, its blocks numbered as the first of their kind
static std::vector<u8> gfd_one(const GfdShader &g) {
    std::vector<u8> out(gfd_file_head);
    const size_t at = out.size();
    out.resize(at + g.head_len + g.prog_len);
    wiiu::romfs_begin();  // the content's reads take turns (the game's RomFS is read the same way)
    fseek(gfd_file, g.head_at, SEEK_SET);
    const size_t a = fread(out.data() + at, 1, g.head_len, gfd_file);
    fseek(gfd_file, g.prog_at, SEEK_SET);
    const size_t b = fread(out.data() + at + g.head_len, 1, g.prog_len, gfd_file);
    wiiu::romfs_end();
    if (a != g.head_len || b != g.prog_len) FATAL("shaders.gsh could not be read\n");
    for (size_t blk : {at, at + g.head_len})
        for (int k = 0; k < 8; k++) out[blk + 24 + k] = 0;  // block id and index: 0
    out.insert(out.end(), gfd_end.begin(), gfd_end.end());
    return out;
}

GX2VertexShader *load_vs(u32 index) {
    wiiu::last_vs = index;
    if (index >= gfd_vs.size()) FATAL("no vertex shader %u\n", index);
    std::vector<u8> one = gfd_one(gfd_vs[index]);
    GX2VertexShader *s = WHBGfxLoadGFDVertexShader(0, one.data());
    if (!s) FATAL("cannot load vertex shader %u\n", index);
    return s;
}
GX2PixelShader *load_ps(u32 index) {
    wiiu::last_ps = index;
    if (index >= gfd_ps.size()) FATAL("no pixel shader %u\n", index);
    std::vector<u8> one = gfd_one(gfd_ps[index]);
    GX2PixelShader *s = WHBGfxLoadGFDPixelShader(0, one.data());
    if (!s) FATAL("cannot load pixel shader %u\n", index);
    return s;
}

void load_shader_tables() {
    static std::vector<u8> idx;
    idx = read_whole("fs:/vol/content/shaders.idx", 0);
    shipped_set_id = fnv64(idx.data(), idx.size());
    {
        std::vector<u8> file = read_whole("fs:/vol/content/shaders.gsh", 0);
        if (file.empty()) FATAL("shaders.gsh is missing\n");
        gfd_scan(file.data(), (u32)file.size());
    }  // and the memory goes back
    gfd_file = fopen("fs:/vol/content/shaders.gsh", "rb");
    if (!gfd_file) FATAL("shaders.gsh is missing\n");
    if (idx.size() < sizeof(ShaderIndexHeader)) FATAL("shaders.idx is missing\n");
    const auto *h = (const ShaderIndexHeader *)idx.data();
    if (h->magic != kShaderIndexMagic || h->version != kShaderIndexVersion) FATAL("shaders.idx is the wrong version\n");
    const auto *e = (const ShaderIndexEntry *)(h + 1);
    for (u32 i = 0; i < h->num_ps; i++, e++) ps_table[(u64)e->key_hi << 32 | e->key_lo].index = e->index;
    for (u32 i = 0; i < h->num_vs; i++, e++) {
        VsEntry &v = vs_table[(u64)e->key_hi << 32 | e->key_lo];
        v.index = e->index;
        v.inputs = e->inputs;
    }
    vs_passthrough = load_vs(h->passthrough_vs);
    vs_blit = load_vs(h->blit_vs);
    ps_blit = load_ps(h->blit_ps);
    ps_fallback = load_ps(h->pad);
    ps_fallback_tex = load_ps(h->pad + 1);
    const auto uber = ps_table.find(fnv64(kUberKey, strlen(kUberKey)));
    if (uber != ps_table.end()) ps_uber = load_ps(uber->second.index);
    const auto uber_lit = ps_table.find(fnv64(kUberLitKey, strlen(kUberLitKey)));
    if (uber_lit != ps_table.end()) ps_uber_lit = load_ps(uber_lit->second.index);
    // A state with no compiled shader draws through these until the console has compiled its own (shader_compile.cpp, about 24 ms), in every game. In the Laytons, before
    // that compiler, Emmy's materials came through the lit one see-through on a console, though every draw through it looks right in Cemu, and why was never found: their lit
    // states are compiled ahead (tools/fs_variants.py), so few draws come here. "set uberfb 0" draws misses solid instead, losing only their shading.
    uber_fallback = true;
    const auto etc1 = ps_table.find(fnv64(kEtc1Key, strlen(kEtc1Key))), etc1a4 = ps_table.find(fnv64(kEtc1A4Key, strlen(kEtc1A4Key)));
    if (etc1 != ps_table.end() && etc1a4 != ps_table.end()) {
        ps_decode[12] = load_ps(etc1->second.index);
        ps_decode[13] = load_ps(etc1a4->second.index);
        for (u32 fmt = 0; fmt < 12; fmt++) {
            const std::string k = untile_key(fmt);
            if (const auto it = ps_table.find(fnv64(k.data(), k.size())); it != ps_table.end()) ps_decode[fmt] = load_ps(it->second.index);
        }
    }
    LOG("[gx2] %u fragment and %u vertex shaders available\n", h->num_ps, h->num_vs);
}

// find_ps's memory of states seen, cleared when the console compiles a shader (gfx_pump)
const DrawState *ps_last_ds;
u64 ps_last_version;
GX2PixelShader *ps_last;
std::unordered_map<u64, GX2PixelShader *> ps_seen;

GX2PixelShader *find_ps(const DrawState &ds) {
    const DrawState *&last_ds = ps_last_ds;
    u64 &last_version = ps_last_version;
    GX2PixelShader *&last = ps_last;
    if (&ds == last_ds && g.ds_version == last_version) return last;
    last_ds = &ds;
    last_version = g.ds_version;
    GX2PixelShader *const uber = ds.lighting ? ps_uber_lit : ps_uber;
    if (uber_all && uber && ds.need_shade) return last = uber;
    // Nearly every state has been seen before, and a quick hash finds it. The shaders' own keys are hashed a byte at a time, slow on this CPU.
    u32 words[FS_KEY_WORDS];
    const u32 nwords = fs_key_words(ds, words);
    u32 ha = nwords, hb = 0x9E3779B9u;
    for (u32 i = 0; i < nwords; i++) {
        ha = (ha ^ words[i]) * 0x85EBCA6Bu;
        hb = (hb + words[i]) * 0xC2B2AE35u;
        ha ^= hb >> 15;
    }
    std::unordered_map<u64, GX2PixelShader *> &seen = ps_seen;
    const u64 quick = (u64)ha << 32 | hb;
    if (const auto s = seen.find(quick); s != seen.end()) return last = s->second;
    const std::string k((const char *)words, nwords * 4);
    const u64 h = fnv64(k.data(), k.size());
    auto it = ps_table.find(h);
    if (it == ps_table.end()) {
        const std::string n = fs_key(fs_normal(ds));
        it = ps_table.find(fnv64(n.data(), n.size()));
    }
    GX2PixelShader *const fallback = !ds.need_shade ? ps_fallback : uber && uber_fallback ? uber : ds.need_tex[0] && !ds.tex[0].null_tex ? ps_fallback_tex : ps_fallback;
    GX2PixelShader *r = fallback;
    if (it != ps_table.end()) {
        if (!it->second.shader) {
        if (glsl_force && ds.need_shade && shader_compile_on()) shader_compile_request(false, h, gen_fs_glsl(ds), 0);
        it->second.shader = load_ps(it->second.index);
        const GX2PixelShader *q = it->second.shader;
        static int shown;
        if (shown++ < 6) {
            LOG("[gx2] ps %u: %u blocks, %u samplers, %u uniform vars\n", it->second.index, q->uniformBlockCount, q->samplerVarCount, q->uniformVarCount);
            for (u32 i = 0; i < q->uniformBlockCount; i++) LOG("[gx2]   block %s at %u size %u\n", q->uniformBlocks[i].name, q->uniformBlocks[i].offset, q->uniformBlocks[i].size);
            for (u32 i = 0; i < q->samplerVarCount; i++) LOG("[gx2]   sampler %s at %u\n", q->samplerVars[i].name, q->samplerVars[i].location);
        }
    }
        r = it->second.shader;
    } else {
        st_ps_misses++;
        ps_table[h].shader = fallback;  // log it once
        ps_table[h].index = MISSED;
        const u32 n = le32((u32)k.size());
        miss_put("FS  ", 4);
        miss_put(&n, 4);
        miss_put(k.data(), k.size());
        if (ds.need_shade && shader_compile_on()) shader_compile_request(false, h, gen_fs_glsl(ds), 0);  // the console compiles it meanwhile: gfx_pump installs it
    }
    seen[quick] = r;
    last = r;
    return r;
}

// attribute location of input register r, from the name the shader generator gave it
void vs_locations(VsEntry &v) {
    for (auto &l : v.location) l = -1;
    for (u32 i = 0; i < v.shader->attribVarCount; i++) {
        const GX2AttribVar &a = v.shader->attribVars[i];
        if (a.name[0] == 'v') v.location[atoi(a.name + 1)] = a.location;
    }
}
// what the fetch shader is built from (attribVars) against what the GPU routes by (the semantic table): a compiler may number them differently
std::unordered_map<u64, u32> vs_set_aside;  // "glslvsforce": the shipped shaders the console's compiler stands in for, by key
void log_vs_tables(const char *what, u64 key, const GX2VertexShader *s) {
    static u32 shown;
    if (shown++ >= 96) return;
    char line[400];
    int n = snprintf(line, sizeof line, "[gx2] %s vs %016llx attribs", what, (unsigned long long)key);
    for (u32 i = 0; i < s->attribVarCount && n < 300; i++) n += snprintf(line + n, sizeof line - n, " %s@%u", s->attribVars[i].name, s->attribVars[i].location);
    n += snprintf(line + n, sizeof line - n, " | semantics(%u)", s->regs.num_sq_vtx_semantic);
    for (u32 i = 0; i < s->regs.num_sq_vtx_semantic && i < 32 && n < 380; i++) n += snprintf(line + n, sizeof line - n, " %u", s->regs.sq_vtx_semantic[i]);
    LOG("%s | block %s at %u size %u\n", line, s->uniformBlockCount ? s->uniformBlocks[0].name : "-", s->uniformBlockCount ? s->uniformBlocks[0].offset : 0, s->uniformBlockCount ? s->uniformBlocks[0].size : 0);
    // what it hands the pixel shader, which finds its inputs by these ids: two compilers must number them alike
    n = snprintf(line, sizeof line, "[gx2] %s vs %016llx outputs: config %08x, ids(%u)", what, (unsigned long long)key, s->regs.spi_vs_out_config, s->regs.num_spi_vs_out_id);
    for (u32 i = 0; i < s->regs.num_spi_vs_out_id && i < 10; i++) n += snprintf(line + n, sizeof line - n, " %08x", s->regs.spi_vs_out_id[i]);
    LOG("%s, clip %08x, %u bytes\n", line, s->regs.pa_cl_vs_out_cntl, s->size);
}
void prepare_vs(VsEntry &v) {
    v.shader = load_vs(v.index);
    vs_locations(v);
    log_vs_tables("shipped", v.index, v.shader);
}

// (GPU thread, every frame) shaders the console finished compiling go into the tables; the next draw with that state takes them.
// A shader the shipped set has stays: the cache holds what an earlier build lacked, and the PC's compiler is the one every game was checked with.
void install_compiled() {
    CompileDone d;
    while (shader_compile_take(d)) {
        if (d.vs) {
            // the cache of an earlier run compiles its vertex shaders whatever this run's switch says: they stop here
            if (const auto it = vs_table.find(d.key); !glsl_vs || (it != vs_table.end() && it->second.index != MISSED && it->second.index != COMPILED_HERE)) {
                shader_compile_discard(d);
                continue;
            }
            if (const auto shipped = vs_set_aside.find(d.key); shipped != vs_set_aside.end())  // "glslvsforce": the two side by side
                log_vs_tables("shipped", d.key, load_vs(shipped->second));
            VsEntry &v = vs_table[d.key];
            v.shader = d.vs_shader;
            v.index = COMPILED_HERE;
            v.inputs = d.inputs;
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, v.shader->program, v.shader->size);
            vs_locations(v);
            log_vs_tables("compiled", d.key, v.shader);
        } else {
            if (const auto it = ps_table.find(d.key); it != ps_table.end() && it->second.index != MISSED && it->second.index != COMPILED_HERE) {
                shader_compile_discard(d);
                continue;
            }
            PsEntry &p = ps_table[d.key];
            p.shader = d.ps_shader;
            p.index = COMPILED_HERE;
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.shader->program, p.shader->size);
            ps_seen.clear();  // states that found the fallback look again
            ps_last_ds = nullptr;
        }
    }
}

// ---------------------------------------------------------------- fetch shaders
struct FetchKey {
    u32 count;
    GX2AttribStream s[17];
    bool operator==(const FetchKey &o) const { return count == o.count && !memcmp(s, o.s, sizeof(GX2AttribStream) * count); }
};
struct FetchHash {
    size_t operator()(const FetchKey &k) const {
        u32 h = k.count;
        const u32 *w = (const u32 *)k.s;
        for (u32 i = 0; i < k.count * sizeof(GX2AttribStream) / 4; i++) h = (h ^ w[i]) * 0x9E3779B1u;
        return h ^ h >> 15;
    }
};
std::unordered_map<FetchKey, std::unique_ptr<GX2FetchShader>, FetchHash> fetch_shaders;

GX2FetchShader *get_fetch(const FetchKey &k) {
    auto it = fetch_shaders.find(k);
    if (it != fetch_shaders.end()) return it->second.get();
    auto fs = std::make_unique<GX2FetchShader>();
    memset(fs.get(), 0, sizeof(GX2FetchShader));
    const u32 size = GX2CalcFetchShaderSizeEx(k.count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    void *mem = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, size);
    GX2InitFetchShaderEx(fs.get(), (u8 *)mem, k.count, k.s, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, mem, size);
    GX2FetchShader *r = fs.get();
    fetch_shaders.emplace(k, std::move(fs));
    return r;
}

GX2AttribStream float4_stream(u32 location, u32 buffer, u32 offset, bool instance) {
    GX2AttribStream a{};
    a.location = location;
    a.buffer = buffer;
    a.offset = offset;
    a.format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
    a.type = instance ? GX2_ATTRIB_INDEX_PER_INSTANCE : GX2_ATTRIB_INDEX_PER_VERTEX;
    a.aluDivisor = instance ? 1 : 0;
    a.mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
    a.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
    return a;
}

// ---------------------------------------------------------------- surfaces
struct Surface {
    u32 addr = 0, w = 0, h = 0, fmt = 0;
    bool depth = false, mem1 = false;
    float sx = 1, sy = 1;  // surface pixels per PICA pixel
    float base_sx = 1, base_sy = 1;  // the same at full size, and which of those it's at: see dyn_res_update
    u32 dyn_level = 0;
    float dyn_f = 1;  // the share of base_sx/base_sy it's at
    u32 sw = 0, sh = 0;
    u32 tex_h = 0, row0 = 0;  // the height it's read at as a texture, and its window row 0 in the surface (see get_surface)
    GX2ColorBuffer cb;
    GX2DepthBuffer db;
    GX2Texture tex;
    bool gpu_newer = false, guest_newer = false, written_since_flush = false;
    bool first_use = true;                             // nothing has drawn into it yet: see upload_target
    bool gpu_chain = false;                            // made by gx2_texture_copy, or from one: the transfers that make its mipmaps stay on the GPU too
    u64 drawn = 0;                                     // when the GPU last wrote it (gpu_writes): of targets sharing memory, the latest holds what's there
    bool linear = false;                               // its memory holds the picture row by row, not in tiles: a display transfer's linear output (gpu_copy_transfer)
    bool shared_depth = false, clear_pending = false;  // see share_depth_block
    // For a target the game's own code may read (cpu_reads): the list being run has drawn to it; a draw to some other kind of target has sampled
    // the picture it holds; and the same of the picture it held before this list drew to it. See gx2_finish_list.
    bool in_list = false, sampled = false, was_sampled = false;
    u32 bytes() const {
        const u32 bpp = depth ? (fmt == 0 ? 2 : fmt == 2 ? 3 : 4) : (fmt == 0 ? 4 : fmt == 1 ? 3 : 2);
        return w * h * bpp;
    }
    void *image() { return depth ? db.surface.image : cb.surface.image; }
    u32 image_size() { return depth ? db.surface.imageSize : cb.surface.imageSize; }
};
std::vector<std::unique_ptr<Surface>> surfaces;
Surface *cur_color, *cur_depth;
// A colour target in memory the game's code reads as it likes, and smaller than a screen: a picture the game may have drawn to pick up itself,
// as Miitopia does a Mii's face (gx2_finish_list). A whole screen it asks for with a transfer.
inline bool cpu_reads(const Surface &s) { return !s.depth && s.addr >= mem::FCRAM_PADDR && s.w * s.h < 240 * 320; }
std::vector<Surface *> read_targets;  // those the list being run has drawn to, in order
GX2ContextState *ctx;
GX2DepthBuffer scratch_depth;
bool frame_started;
bool state_lost = true;  // the GPU's state no longer matches what was last set
bool state_sent_valid;   // set_state's record of what it last sent still holds (StateSent)

#ifdef GX2_JUNK  // test builds: fresh host memory filled with noise, as a console's is, so that Cemu shows what starts uninitialized. 1 surfaces, 2 textures, 4 the ring
static void *junk(void *p, u32 n, u32 bit) {
    if (p && (GX2_JUNK & bit)) {
        u32 x = 0x2545F491u ^ (u32)(uintptr_t)p;
        for (u32 i = 0; i < n / 4; i++) ((u32 *)p)[i] = x = x * 1664525u + 1013904223u;
        DCFlushRange(p, n);
    }
    return p;
}
#else
#define junk(p, n, bit) (p)
#endif

// New memory the GPU draws into: the CPU's cache may still hold lines of it, changed, from whatever had the memory before, and when one is evicted
// later it lands over what the GPU drew, as specks (Mario on NSMB2's map, Tomodachi's Mii faces). Write them back first, as WHB does for its buffers.
// Cemu has no cache, so it never shows there; junk() above flushes too, which is why junk-filled Cemu runs looked clean.
static void *for_gpu_writes(void *p, u32 n) {
    if (p) DCFlushRange(p, n);
    return p;
}

static bool mem1_relief;  // fast memory has been found full once (wiiu::relieve_mem1)

void relieve_memory(u32 wanted);  // below, where the caches are

// memalign for what the GPU reads, which on failure first makes room in the caches (relieve_memory) and tries again: after a long session through
// Luigi's Mansion: Dark Moon's rooms a texture found the heap full ("out of memory for a 356352 byte texture", Senne's Wii U, 2026-11-11)
void *gpu_memalign(u32 align, u32 size) {
    void *p = memalign(align, size);
    if (!p) {
        relieve_memory(size);
        p = memalign(align, size);
    }
    return p;
}

void *alloc_surface_mem(u32 size, u32 align, bool &mem1, bool want_mem1) {
    if (want_mem1 && GfxHeapAllocMEM1) {
        void *p = GfxHeapAllocMEM1(size, align);
        if (!p && !mem1_relief) {  // the first time fast memory is full: make room, and try again
            wiiu::relieve_mem1();
            p = GfxHeapAllocMEM1(size, align);
        }
        if (p) {
            mem1 = true;
            return for_gpu_writes(junk(p, size, 1), size);
        }
    }
    mem1 = false;
    void *p = gpu_memalign(align, size);
    if (!p) FATAL("out of memory for a %u byte surface\n", size);
    return for_gpu_writes(junk(p, size, 1), size);
}

// Fast memory is 32 MB, and some games want more than fits: the Laytons draw a 240x400 pair at boot, then a 512x800 pair that used to land in MEM2, where dialog scenes draw
// nearly everything, at a cost of about 25 ms of a 33 ms frame on a console. Once a screen's render target finds fast memory full (relieve_mem1), the screens' depth buffers
// share one block from then on: the pairs mostly work in different scenes, and a depth buffer's contents only last from its clear to the end of its pass. A clear waits for the
// first draw that needs it, which also saves clearing what nothing draws to. A game that fits never shares: one that samples a depth buffer as a texture would read another's.
void *depth_block;
u32 depth_block_size;
Surface *depth_owner;  // whose depth values are in the block

bool share_depth_block(GX2Surface &f) {
    if (!depth_block) {
        GX2Surface largest = f;
        largest.width = 1152;  // 512x800, drawn larger (rt_scale) and padded to a texture's height (get_surface)
        largest.height = 2304;
        GX2CalcSurfaceSizeAndAlignment(&largest);
        depth_block_size = std::max(largest.imageSize, f.imageSize);
        if (!GfxHeapAllocMEM1 || !(depth_block = GfxHeapAllocMEM1(depth_block_size, std::max(largest.alignment, f.alignment)))) return false;
        for_gpu_writes(depth_block, depth_block_size);
    }
    if (f.imageSize > depth_block_size || (uintptr_t)depth_block % f.alignment) return false;
    f.image = depth_block;
    return true;
}

// The top screen is drawn at 1200x720, three times the 3DS's, and the blit scales it up to the TV, as most Wii U games' pictures are (the user's choice, 2026-11-06). Under half the
// pixels of 1800x1080, targets small enough for nearly all to fit in fast memory, and a whole-number scale, so 3DS pixel edges never land on TV pixel centres.
// "top1080" (a file in the data folder, or a test run's flag) draws at 1800x1080 again, to compare.
bool top_720 = true;

// The screens' render targets are drawn larger: the top screen for the size above, the bottom one 1:1 on the GamePad. The game
// renders some of them twice as wide and scales them down (cheap antialiasing), so the two axes scale differently.
void rt_scale(u32 w, u32 h, float &sx, float &sy) {
    sx = sy = 1;
    // 256 and 512 wide, and 416 tall: some games draw a little more than the screen and crop it in the display transfer
    const u32 shown = w == 256 ? 240 : w == 512 ? 480 : w;
    if ((h == 400 || h == 416) && (shown == 240 || shown == 480)) {
        sx = (top_720 ? 720.0f : 1080.0f) / shown;
        sy = top_720 ? 3.0f : 4.5f;
    } else if (h == 320 && (shown == 240 || shown == 480)) {
        sx = 480.0f / shown;
        sy = 2.0f;
    } else if ((w & (w - 1)) == 0 && (h & (h - 1)) != 0 && h > 400 && h <= 1024) {
        sx = sy = (top_720 ? 1200.0f : 1800.0f) / h;  // a screen drawn off-screen at about twice the size, for the game to draw as a texture: as sharp as that screen
    }
}

// scale_as: a copy of that surface's picture (gx2_texture_copy and its mipmaps), made at its scale so what the game draws from it stays as sharp
Surface *get_surface(u32 addr, u32 w, u32 h, u32 fmt, bool depth, const Surface *scale_as = nullptr) {
    for (auto &s : surfaces)
        if (s->addr == addr && s->w == w && s->h == h && s->depth == depth) {
            s->fmt = fmt;
            return s.get();
        }
    auto s = std::make_unique<Surface>();
    s->addr = addr;
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->depth = depth;
    if (scale_as) {
        s->sx = scale_as->base_sx;
        s->sy = scale_as->base_sy;
    } else rt_scale(w, h, s->sx, s->sy);
    s->base_sx = s->sx;
    s->base_sy = s->sy;
    // Textures have power-of-two sizes, so a 512x800 target the game draws from is read as 512x1024, sharing its memory. In memory the
    // target's top row is the texture's, so its window row 0 is 224 rows up the texture: leave that much room below it, and it lines up.
    s->tex_h = h;
    if ((w & (w - 1)) == 0 && w >= 8 && w <= 1024)
        while (s->tex_h & (s->tex_h - 1)) s->tex_h += s->tex_h & -s->tex_h;
    s->sw = (u32)(w * s->sx);
    s->sh = (u32)(s->tex_h * s->sy);
    s->row0 = s->sh - (u32)(h * s->sy);
    const bool screen = s->sy > 1 && !scale_as;  // copies go in MEM2: Metroid's four are 4.5 MB each, and fast memory is for the targets it draws into
    if (depth) {
        memset(&s->db, 0, sizeof s->db);
        GX2Surface &f = s->db.surface;
        f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        f.width = s->sw;
        f.height = s->sh;
        f.depth = 1;
        f.mipLevels = 1;
        // 24 bits of fixed point, as the PICA's depth buffer, not the usual 24-bit float: that has a 20-bit mantissa, so from the middle of the range out two depths a few
        // PICA steps apart are the same to it, and a floor mat drawn after the characters standing on it covered them on the console. Cemu's host GPU had kept them apart.
        f.format = GX2_SURFACE_FORMAT_UNORM_R24_X8;
        f.aa = GX2_AA_MODE1X;
        f.use = GX2_SURFACE_USE_DEPTH_BUFFER;
        f.tileMode = GX2_TILE_MODE_DEFAULT;
        s->db.viewNumSlices = 1;
        s->db.depthClear = 1.0f;
        GX2CalcSurfaceSizeAndAlignment(&f);
        GX2InitDepthBufferRegs(&s->db);
        if (screen && mem1_relief && share_depth_block(f)) s->mem1 = s->shared_depth = s->clear_pending = true;
        else f.image = alloc_surface_mem(f.imageSize, f.alignment, s->mem1, screen);
    } else {
        memset(&s->cb, 0, sizeof s->cb);
        GX2Surface &f = s->cb.surface;
        f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        f.width = s->sw;
        f.height = s->sh;
        f.depth = 1;
        f.mipLevels = 1;
        f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        f.aa = GX2_AA_MODE1X;
        f.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
        f.tileMode = GX2_TILE_MODE_DEFAULT;
        s->cb.viewNumSlices = 1;
        GX2CalcSurfaceSizeAndAlignment(&f);
        GX2InitColorBufferRegs(&s->cb);
        f.image = alloc_surface_mem(f.imageSize, f.alignment, s->mem1, screen);
        memset(&s->tex, 0, sizeof s->tex);
        s->tex.surface = f;
        s->tex.viewNumMips = 1;
        s->tex.viewNumSlices = 1;
        s->tex.compMap = 0x00010203;
        GX2InitTextureRegs(&s->tex);
    }
    s->guest_newer = true;
    LOG("[gx2] new %s surface %08x %ux%u fmt %u, %ux%u, %u KB in %s\n", depth ? "depth" : "color", addr, w, h, fmt, s->sw, s->sh,
        s->image_size() >> 10, s->mem1 ? "MEM1" : "MEM2");
    wiiu::note("new %s surface %08x %ux%u, %u KB in %s", depth ? "depth" : "color", addr, w, h, s->image_size() >> 10, s->mem1 ? "MEM1" : "MEM2");
    surfaces.push_back(std::move(s));
    return surfaces.back().get();
}

// Dream Team's heaviest scenes take the GPU over 70 ms a frame at 1800x1080, 14 fps: for as long as it can't keep up the top screen is drawn at two thirds of that each
// way (1200x720, 44% of the pixels) and the blit to the TV scales it. dyn_res_update picks the level; a surface takes it on the next draw or fill that goes into it, and
// keeps its memory, the bigger block it was given when it was made. What's in it is lost: every game paints a screen over anyway (upload_target), and until then the
// surface is shown as it was. Depth buffers follow the same way. A depth block shared between surfaces (Layton's) couldn't: only Dream Team does this.
u32 dyn_level;
// The lower level's share of full size each way: two thirds, "set lowscale <percent>" to try others. At 1200x720 ("top_720") the level changes only
// when "set dynres 1" says so, or "set toplevel 0|1" holds it at one (dyn_force); Luigi's Mansion: Dark Moon draws its top screen in five or six passes
// of 768x1536 or so, and the console's GPU took ~37 ms a frame of that in the mansion.
float dyn_low = 2.0f / 3;
bool dyn_720;
int dyn_force = -1;
inline float dyn_factor(u32 level) { return level ? dyn_low : 1.0f; }
inline bool dyn_scaled(const Surface &s) { return s.base_sy > 2; }  // not the bottom screen, which is 2x on a screen a quarter of the size, nor a 1:1 target

void set_surface_scale(Surface &s, u32 level) {
    if (s.dyn_level == level && s.dyn_f == dyn_factor(level)) return;
    s.dyn_level = level;
    s.dyn_f = dyn_factor(level);
    s.sx = s.base_sx * s.dyn_f;
    s.sy = s.base_sy * s.dyn_f;
    s.sw = (u32)(s.w * s.sx);
    s.sh = (u32)(s.tex_h * s.sy);
    s.row0 = s.sh - (u32)(s.h * s.sy);
    GX2Surface &f = s.depth ? s.db.surface : s.cb.surface;
    void *const image = f.image;
    f.width = s.sw;
    f.height = s.sh;
    f.tileMode = GX2_TILE_MODE_DEFAULT;
    GX2CalcSurfaceSizeAndAlignment(&f);
    f.image = image;
    if (s.depth) {
        GX2InitDepthBufferRegs(&s.db);
    } else {
        GX2InitColorBufferRegs(&s.cb);
        s.tex.surface = f;
        GX2InitTextureRegs(&s.tex);
    }
    if (&s == cur_color) cur_color = nullptr;
    if (&s == cur_depth) cur_depth = nullptr;
}
inline void take_dyn_level(Surface &s) {
    if ((s.dyn_level != dyn_level || s.dyn_f != dyn_factor(dyn_level)) && dyn_scaled(s)) set_surface_scale(s, dyn_level);
}

u64 gpu_writes;  // a count of the GPU's writes into targets, for Surface::drawn

// Switches for trying a change on a console while it plays ("set <name> 0|1" over the network, gfx_set): each of these on by default
bool use_vtx_cache = true, fill_inside = true, linear_on_gpu = true;
volatile bool vtx_drop_all;

// ---- vertex data, converted once
// A draw's vertex data was copied (and integers made floats) at every draw, the whole span its indices reach: Luigi's Mansion: Dark Moon's draws reach 1300
// to 4100 vertices, 36 to 115 KB, 20 to 50 MB a frame, about 43 ms of each frame on a console. What a draw read stays converted, until the game says it
// may have written there: every way a game can make its writes seen by the GPU (GSP's cache calls, GX's, the kernel's, and the GPU's own copies) comes
// through metal_guest_written, in order with the commands, as the texture cache relies on too. In case a game writes without saying, an entry is checked
// against what's in memory now and then (kVtxRecheck flips).
struct VtxEntry {
    u32 addr = 0, bytes = 0;  // the source, in guest memory
    void *buf = nullptr;
    u32 size = 0;
    u64 hash = 0;
    u32 used = 0, checked = 0;  // flips
};
std::unordered_map<u64, VtxEntry> vtx_cache;
std::vector<std::pair<void *, u32>> vtx_retired;  // buffers the GPU may still read, and the flip from which they can go
u64 vtx_cache_size, st_vtx_hits, st_vtx_misses;
constexpr u32 kVtxRecheck = 60;
constexpr u64 kVtxCacheMax = 48u << 20;

void vtx_retire(VtxEntry &e) {
    if (e.buf) vtx_retired.push_back({e.buf, (u32)st_flips + 4});
    vtx_cache_size -= e.size;
    e.buf = nullptr;
    e.size = 0;
}

void vtx_invalidate(u32 paddr, u32 size) {
    for (auto it = vtx_cache.begin(); it != vtx_cache.end();)
        if (it->second.addr < paddr + size && paddr < it->second.addr + it->second.bytes) {
            vtx_retire(it->second);
            it = vtx_cache.erase(it);
        } else ++it;
}

// once a flip: free what the GPU is done with, and when the cache is full, what the last frames didn't use
void vtx_cache_flip() {
    const u32 now = (u32)st_flips;
    if (vtx_drop_all) {
        vtx_drop_all = false;
        vtx_invalidate(0, ~0u);
    }
    for (size_t i = 0; i < vtx_retired.size();)
        if ((s32)(now - vtx_retired[i].second) >= 0) {
            free(vtx_retired[i].first);
            vtx_retired[i] = vtx_retired.back();
            vtx_retired.pop_back();
        } else i++;
    if (vtx_cache_size > kVtxCacheMax)
        for (auto it = vtx_cache.begin(); it != vtx_cache.end();)
            if (now - it->second.used > 2) {
                vtx_retire(it->second);
                it = vtx_cache.erase(it);
            } else ++it;
}

// A buffer holding what fill makes of the bytes at src, as the GPU reads it: from the cache, or made now and kept. span: the bytes it's made from,
// if more than the last vertex's 16 (a whole vertex array's copy)
template <typename Fill>
const void *vtx_cached(u32 addr, const u8 *src, u32 src_stride, u32 count, u32 kind, u32 size, Fill fill, u32 span = 0) {
    if (!use_vtx_cache) {
        void *w = ring_alloc(size + 64, 64);
        fill(w, src);
        DCFlushRange(w, size);
        return w;
    }
    const u32 bytes = std::max((count - 1) * src_stride + 16, span);  // the last vertex's attribute reads no more than 16 bytes
    const u64 key = ((u64)addr << 32) ^ ((u64)src_stride << 20) ^ ((u64)kind << 12) ^ ((u64)count * 0x9E3779B97F4A7C15ull);
    const u32 now = (u32)st_flips;
    auto it = vtx_cache.find(key);
    if (it != vtx_cache.end()) {
        VtxEntry &e = it->second;
        if (e.addr == addr && e.size == size && e.buf && (now - e.checked < kVtxRecheck || hash_bytes(src, bytes) == e.hash)) {
            if (now - e.checked >= kVtxRecheck) e.checked = now;
            e.used = now;
            if (kMeasure) st_vtx_hits++;
            return e.buf;
        }
        vtx_retire(e);
        vtx_cache.erase(it);
    }
    if (kMeasure) st_vtx_misses++;
    void *buf = memalign(64, size + 64);
    if (!buf) {  // no room: this draw's own copy, as before
        void *w = ring_alloc(size + 64, 64);
        fill(w, src);
        DCFlushRange(w, size);
        return w;
    }
    fill(buf, src);
    DCFlushRange(buf, size);
    VtxEntry &e = vtx_cache[key];
    e.addr = addr;
    e.bytes = bytes;
    e.buf = buf;
    e.size = size;
    e.hash = hash_bytes(src, bytes);
    e.used = e.checked = now;
    vtx_cache_size += size;
    return buf;
}

// A surface at addr that match accepts. Targets of different sizes can share an address (get_surface tells them apart by size), so the first
// one there isn't always the one meant: a stale target used to stand in front of the one a texture read.
template <class Match>
Surface *find_surface(u32 addr, bool depth, Match match) {
    for (auto &s : surfaces)
        if (s->addr == addr && s->depth == depth && match(*s)) return s.get();
    return nullptr;
}
Surface *find_surface(u32 addr, bool depth) {
    return find_surface(addr, depth, [](const Surface &) { return true; });
}

void flush_surface(Surface &s) {
    if (!s.written_since_flush) return;
    s.written_since_flush = false;
    GX2Invalidate((GX2InvalidateMode)((s.depth ? GX2_INVALIDATE_MODE_DEPTH_BUFFER : GX2_INVALIDATE_MODE_COLOR_BUFFER) | GX2_INVALIDATE_MODE_TEXTURE), s.image(), s.image_size());
}

// a fill the game asked for before the surface exists, or while drawing was elsewhere
struct Fill {
    u32 start, end, value, control;
};
std::vector<Fill> pending_fills;

void restore_context() {
    GX2SetContextState(ctx);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    state_lost = true;
    cur_color = cur_depth = nullptr;
}

void apply_fill(Surface &s, const Fill &f) {
    gt_mark(GT_FILL, s.addr);
    take_dyn_level(s);
    if (s.depth) {
        const u32 v = f.value;
        const float d = s.fmt == 0 ? (v & 0xffff) / 65535.0f : (v & 0xffffff) / 16777215.0f;
        const u8 st = s.fmt == 3 ? (u8)(v >> 24) : 0;
        s.db.depthClear = d;
        s.db.stencilClear = st;
        if (s.shared_depth) {
            s.clear_pending = true;
            s.guest_newer = false;
            return;
        }
        GX2ClearDepthStencilEx(&s.db, d, st, GX2_CLEAR_FLAGS_BOTH);
    } else {
        const u32 v = f.value;
        float r, g, b, a = 1;
        switch (s.fmt) {
        case 0: r = (v >> 24) / 255.0f; g = ((v >> 16) & 255) / 255.0f; b = ((v >> 8) & 255) / 255.0f; a = (v & 255) / 255.0f; break;
        case 1: r = ((v >> 16) & 255) / 255.0f; g = ((v >> 8) & 255) / 255.0f; b = (v & 255) / 255.0f; break;
        case 2: r = ((v >> 11) & 31) / 31.0f; g = ((v >> 6) & 31) / 31.0f; b = ((v >> 1) & 31) / 31.0f; a = v & 1; break;
        case 3: r = ((v >> 11) & 31) / 31.0f; g = ((v >> 5) & 63) / 63.0f; b = (v & 31) / 31.0f; break;
        default: r = ((v >> 12) & 15) / 15.0f; g = ((v >> 8) & 15) / 15.0f; b = ((v >> 4) & 15) / 15.0f; a = (v & 15) / 15.0f; break;
        }
        GX2ClearColor(&s.cb, r, g, b, a);
    }
    restore_context();
    s.gpu_newer = true;
    s.guest_newer = false;
    s.written_since_flush = true;
    s.drawn = ++gpu_writes;
}

// ---------------------------------------------------------------- textures
struct Tex {
    u32 addr, w, h, fmt, levels, bytes;
    u32 hash, sample, heap_flushes;
    u32 slice[32];      // the hash's parts (hash_guest), and the next one a whole-heap flush checks
    u32 slice_next;
    bool maybe_dirty, dirty, hash_stale;
    // what a fill of the whole texture last left there, while nothing else has written it (gx2_memory_fill): the same fill again changes nothing
    bool filled = false, fill_pending = false;
    u32 fill_value = 0;
    u32 row_bytes;      // one row of 8x8 tiles of level 0
    u32 dirty_rows[4];  // a bit per tile row (up to 1024 rows of texels) that definite writes touched since the last upload
    u32 lag_rows[4];    // the tile rows the spare image lacks (see replace_image)
    u64 last_use;
    u32 mem;
    GX2Texture t;
    bool changed_before;  // see replace_image
    void *spare;
    OSTime spare_used_until;
    bool gpu;           // decoded by the GPU into a tiled image it renders (gpu_decode): no CPU writes, no spare image
    GX2ColorBuffer cb;  // that image as a render target
    GX2Texture stage;   // the bytes it decodes from, as they are in the game's memory
};
std::vector<std::unique_ptr<Tex>> textures;
std::unordered_map<u64, Tex *> tex_index;
inline u64 tex_key(u32 addr, u32 w, u32 h, u32 fmt, u32 levels) { return (u64)addr << 32 | w << 20 | h << 8 | fmt << 4 | levels; }
u64 use_counter;
u64 tex_memory;
constexpr u64 TEX_MEMORY_LIMIT = 128ull << 20;

constexpr u8 k_fmt_bits[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 8, 8};

// Whether the GPU decodes a texture into a tiled image it renders (gpu_decode), instead of the CPU into a linear one: any one-level texture
// from a size where switching targets costs less than the CPU's decode. The console's logs put the CPU's at 55 us a KB (2026-11-07), so 55 ms
// for a 1 MB texture when it first shows, and every time for one the game keeps rewriting. "cputex" turns it off.
inline bool gpu_decoded(u32 w, u32 h, u32 fmt, u32 levels) { return fmt < 14 && levels == 1 && w * h >= 128 * 128 && ps_decode[fmt] && !gxt_cpu_tex; }
// staging rows to a row of 8x8 tiles: two rows of ETC1 blocks, or eight equal parts of a plain format's bytes
inline u32 stage_rows(u32 fmt) { return fmt >= 12 ? 2 : 8; }

// A texture's words in SLICES runs, each hashed by itself, and the whole's hash made of those: after a flush of the whole heap a texture can be
// checked a run at a time (get_texture)
constexpr u32 SLICES = 32;
u32 hash_slice(const u8 *p, u32 n, u32 k) {
    const u32 *w = (const u32 *)p;
    const u32 words = n / 4;
    u32 h = k;
    for (u32 i = (u32)((u64)words * k / SLICES), end = (u32)((u64)words * (k + 1) / SLICES); i < end; i++) h = h * 31 + w[i];
    return h;
}
u32 hash_guest(const u8 *p, u32 n, u32 *slices) {
    u32 h = n;
    for (u32 k = 0; k < SLICES; k++) {
        slices[k] = hash_slice(p, n, k);
        h = h * 31 + slices[k];
    }
    return h;
}

// A word every 128 bytes, and at most 256 of them, for checking every texture in use after a flush of the whole heap: a character's art loaded over another's changes
// nearly all of them. Superstar Saga flushes its 54 MB heap every frame, so a word per 128 bytes of a 1 MB background would be 8000 cache misses a frame on a console.
u32 sample_guest(const u8 *p, u32 n) {
    u32 h = n;
    const u32 *w = (const u32 *)p;
    const u32 step = std::max(32u, n / 1024);
    for (u32 i = 0; i < n / 4; i += step) h = h * 31 + w[i];
    return h;
}

u64 heap_flush_flip, heap_flush_gap;  // the frame of the last whole-heap flush, and the frames between the last two
u32 heap_flushes;  // Most games flush what they changed. Some flush everything, after every file they read, and that says nothing about what changed.

// for the frame rate report: what keeps being uploaded, and how often the GPU's work comes back to guest memory
struct { u32 addr, w, h, fmt, levels; bool dirty; } last_reupload;
u32 st_readbacks, st_gpu_copies;
u64 st_attr_verts, st_attr_bytes, st_attr_streams;  // what draws copied of their vertex data: vertices, bytes read, attribute streams

inline u32 pack_rgba(u32 r, u32 g, u32 b, u32 a) { return r << 24 | g << 16 | b << 8 | a; }
inline u32 c4(u32 v) { return v * 17; }
inline u32 c5(u32 v) { return (v << 3) | (v >> 2); }
inline u32 c6(u32 v) { return (v << 2) | (v >> 4); }

inline u32 cl255(int v) { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); }

// one 4x4 ETC1 block, out[4 * x + y], alpha 255: the per-texel decoder works out the block's colours 16 times over
void decode_etc1_block(u64 blk, u32 *out) {
    static const int mod[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const bool flip = (blk >> 32) & 1, diff = (blk >> 33) & 1;
    int base[2][3];
    if (diff) {
        const int r = (int)((blk >> 59) & 31), g = (int)((blk >> 51) & 31), b = (int)((blk >> 43) & 31);
        base[0][0] = c5(r), base[0][1] = c5(g), base[0][2] = c5(b);
        const u32 hi = (u32)(blk >> 32);
        base[1][0] = c5((r + sbits(hi, 24, 3)) & 31), base[1][1] = c5((g + sbits(hi, 16, 3)) & 31), base[1][2] = c5((b + sbits(hi, 8, 3)) & 31);
    } else {
        base[0][0] = c4((blk >> 60) & 15), base[0][1] = c4((blk >> 52) & 15), base[0][2] = c4((blk >> 44) & 15);
        base[1][0] = c4((blk >> 56) & 15), base[1][1] = c4((blk >> 48) & 15), base[1][2] = c4((blk >> 40) & 15);
    }
    const int *tab[2] = {mod[(blk >> 37) & 7], mod[(blk >> 34) & 7]};
    const u32 bits = (u32)blk;  // the texel bits are all in the low word: 32-bit shifts, which a PowerPC does in one instruction
    for (u32 x = 0; x < 4; x++)
        for (u32 y = 0; y < 4; y++) {
            const u32 texel = 4 * x + y, half = flip ? y >> 1 : x >> 1;  // the second sub-block is the right half, or the bottom one when flipped
            int m = tab[half][(bits >> texel) & 1];
            if ((bits >> (16 + texel)) & 1) m = -m;
            const int *c = base[half];
            out[texel] = pack_rgba(cl255(c[0] + m), cl255(c[1] + m), cl255(c[2] + m), 255);
        }
}

// a row of 8 texels is one 32-byte cache line: claim it without reading it first, which a console does on a plain write
inline void claim_line(u32 *row) {
#ifdef __WIIU__
    if (!((uintptr_t)row & 31)) __asm__ volatile("dcbz 0,%0" ::"r"(row) : "memory");
#endif
}

// one level, memory rows top down, into rows bottom up (row 0 = t = 0). Whole 8x8 tiles at a time, which most formats do by table.
// Tile rows ty0 to ty1 only, when a write touched part of the memory.
void decode_level(const u8 *src, u32 w, u32 h, u32 fmt, u32 *dst, u32 pitch, u32 ty0 = 0, u32 ty1 = ~0u) {
    static u8 mx[8][8], ready;
    if (!ready) {
        for (u32 y = 0; y < 8; y++)
            for (u32 x = 0; x < 8; x++) mx[y][x] = (u8)morton8(x, y);
        ready = 1;
    }
    const u32 tile_bytes = 64 * k_fmt_bits[fmt & 15] / 8;
    const u32 tw = w / 8, th = std::min(h / 8, ty1);
    for (u32 ty = ty0; ty < th; ty++) {
        for (u32 tx = 0; tx < tw; tx++) {
            const u8 *t = src + (ty * tw + tx) * tile_bytes;
            if (fmt >= 12) {  // ETC1 and ETC1A4: four 4x4 blocks per tile, decoded once each
                const bool alpha = fmt == 13;
                u32 px[4][16];
                u32 pa[4][2];  // alpha nibbles, texels 0-7 in [0] and 8-15 in [1]
                for (u32 b = 0; b < 4; b++) {
                    const u8 *sp = t + b * (alpha ? 16 : 8);
                    u64 v;
                    if (alpha) {
                        memcpy(&v, sp, 8);
                        v = le64(v);
                        pa[b][0] = (u32)v;
                        pa[b][1] = (u32)(v >> 32);
                        sp += 8;
                    }
                    memcpy(&v, sp, 8);
                    decode_etc1_block(le64(v), px[b]);
                }
                for (u32 y = 0; y < 8; y++) {
                    u32 *row = dst + (u64)(h - 1 - (ty * 8 + y)) * pitch + tx * 8;
                    claim_line(row);
                    for (u32 x = 0; x < 8; x++) {
                        const u32 b = x / 4 + 2 * (y / 4), i = (x & 3) * 4 + (y & 3);
                        row[x] = (px[b][i] & ~255u) | (alpha ? c4((pa[b][i >> 3] >> (4 * (i & 7))) & 15) : 255);
                    }
                }
                continue;
            }
            for (u32 y = 0; y < 8; y++) {
                u32 *row = dst + (u64)(h - 1 - (ty * 8 + y)) * pitch + tx * 8;
                claim_line(row);
                const u8 *m = mx[y];
                switch (fmt) {
                case 0:
                    for (u32 x = 0; x < 8; x++) row[x] = __builtin_bswap32(*(const u32 *)(t + m[x] * 4));
                    break;
                case 1:
                    for (u32 x = 0; x < 8; x++) {
                        const u8 *p = t + m[x] * 3;
                        row[x] = pack_rgba(p[2], p[1], p[0], 255);
                    }
                    break;
                case 2:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 v = t[m[x] * 2] | t[m[x] * 2 + 1] << 8;
                        row[x] = pack_rgba(c5(v >> 11), c5((v >> 6) & 31), c5((v >> 1) & 31), (v & 1) * 255);
                    }
                    break;
                case 3:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 v = t[m[x] * 2] | t[m[x] * 2 + 1] << 8;
                        row[x] = pack_rgba(c5(v >> 11), c6((v >> 5) & 63), c5(v & 31), 255);
                    }
                    break;
                case 4:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 v = t[m[x] * 2] | t[m[x] * 2 + 1] << 8;
                        row[x] = pack_rgba(c4(v >> 12), c4((v >> 8) & 15), c4((v >> 4) & 15), c4(v & 15));
                    }
                    break;
                case 5:
                    for (u32 x = 0; x < 8; x++) {
                        const u8 *p = t + m[x] * 2;
                        row[x] = pack_rgba(p[1], p[1], p[1], p[0]);
                    }
                    break;
                case 6:
                    for (u32 x = 0; x < 8; x++) {
                        const u8 *p = t + m[x] * 2;
                        row[x] = pack_rgba(p[1], p[0], 0, 255);
                    }
                    break;
                case 7:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 i = t[m[x]];
                        row[x] = pack_rgba(i, i, i, 255);
                    }
                    break;
                case 8:
                    for (u32 x = 0; x < 8; x++) row[x] = t[m[x]];
                    break;
                case 9:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 v = t[m[x]], i = c4(v >> 4);
                        row[x] = pack_rgba(i, i, i, c4(v & 15));
                    }
                    break;
                case 10:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 b = t[m[x] / 2], i = c4((m[x] & 1) ? b >> 4 : b & 15);
                        row[x] = pack_rgba(i, i, i, 255);
                    }
                    break;
                case 11:
                    for (u32 x = 0; x < 8; x++) {
                        const u32 b = t[m[x] / 2];
                        row[x] = c4((m[x] & 1) ? b >> 4 : b & 15);
                    }
                    break;
                default: break;
                }
            }
        }
    }
}

// The heap is full: once the GPU is done, everything the vertex cache holds that this frame hasn't used, the buffers it let go, and the textures this
// frame hasn't used, the least recently used first, down to half of what they take, or until what's wanted is free
void relieve_memory(u32 wanted) {
    const u32 now = (u32)st_flips;
    draw_done();
    for (auto it = vtx_cache.begin(); it != vtx_cache.end();)
        if (it->second.used != now) {
            vtx_retire(it->second);
            it = vtx_cache.erase(it);
        } else ++it;
    u64 freed = 0;
    for (size_t i = 0; i < vtx_retired.size();)
        if (vtx_retired[i].second != now + 4) {  // let go before this frame, so no draw of this one reads it
            free(vtx_retired[i].first);
            vtx_retired[i] = vtx_retired.back();
            vtx_retired.pop_back();
        } else i++;
    const u64 tex_before = tex_memory, goal = tex_memory / 2;
    while (tex_memory > goal && !textures.empty()) {
        auto it = std::min_element(textures.begin(), textures.end(), [](const auto &a, const auto &b) { return a->last_use < b->last_use; });
        if ((*it)->last_use >= use_counter) break;  // this frame's
        const u64 m = (*it)->spare ? 2ull * (*it)->mem : (*it)->mem;
        tex_memory -= m;
        freed += m;
        mem::host_free((*it)->t.surface.image);
        mem::host_free((*it)->spare);
        tex_index.erase(tex_key((*it)->addr, (*it)->w, (*it)->h, (*it)->fmt, (*it)->levels));
        textures.erase(it);
        if (freed > 4ull * wanted && freed > (8u << 20)) break;
    }
    if (kMeasure) LOG("[gx2] memory full (%u bytes wanted): textures %u KB -> %u KB, the vertex cache now %u KB\n", wanted, (u32)(tex_before >> 10), (u32)(tex_memory >> 10),
        (u32)(vtx_cache_size >> 10));
}

void evict_textures(u64 keep = TEX_MEMORY_LIMIT) {
    while (tex_memory > keep && !textures.empty()) {
        auto it = std::min_element(textures.begin(), textures.end(), [](const auto &a, const auto &b) { return a->last_use < b->last_use; });
        if ((*it)->last_use + 2 >= use_counter) break;  // the GPU may still be reading it
        draw_done();
        tex_memory -= (*it)->spare ? 2ull * (*it)->mem : (*it)->mem;
        mem::host_free((*it)->t.surface.image);
        mem::host_free((*it)->spare);
        tex_index.erase(tex_key((*it)->addr, (*it)->w, (*it)->h, (*it)->fmt, (*it)->levels));
        textures.erase(it);
    }
}

// Before new contents go in, the GPU has to have finished with the old. That's fine for a texture changing once, in a scene loading, but one that keeps changing gets a second
// image: its old image stays as it is for the draws already queued, and the one written is the one those draws before them last used.
// True when the images were swapped: the one now current lacks what the last upload wrote (lag), and the other now lacks this upload's bytes.
bool replace_image(Tex &e) {
    GX2Surface &f = e.t.surface;
    if (!e.changed_before) {
        e.changed_before = true;
        draw_done();
        return false;
    }
    if (!e.spare) {
        e.spare = mem::host_alloc(f.alignment, e.mem);
        if (!e.spare) {
            draw_done();
            return false;
        }
        tex_memory += e.mem;
        memset(e.lag_rows, 0xff, sizeof e.lag_rows);  // nothing in it yet
    }
    if (e.spare_used_until && GX2GetRetiredTimeStamp() < e.spare_used_until) {
        const OSTime t = prof_now();
        GX2WaitTimeStamp(e.spare_used_until);
        prof_add(P_SYNC, t);
        st_syncs++;
    }
    std::swap(f.image, e.spare);
    if (f.mipmapSize) f.mipmaps = (u8 *)f.image + f.imageSize;
    const OSTime t = prof_now();
    GX2Flush();
    prof_add(P_TEXFL, t);
    e.spare_used_until = GX2GetLastSubmittedTimeStamp();
    return true;
}

// Only the tile rows set in rows, when given: the memory is tiled, so a write lands in whole 8-row bands. NSMB2 copies a few animated and
// scrolled-in tiles into a 1 MB atlas nearly every frame, at scattered rows, and decoding all of it each time took 77 ms on the console.
void gpu_decode(Tex &e, const u32 *rows);
void upload_texture(Tex &e, const u32 *rows = nullptr) {
    const OSTime t0 = prof_now();
    GX2Surface &f = e.t.surface;
    const u8 *src = pptr(e.addr);
    if (!src) return;
    if (e.gpu) {
        gpu_decode(e, rows);
    } else if (rows) {
        const u32 th = e.h / 8;
        for (u32 ty0 = 0; ty0 < th;) {
            if (!(rows[ty0 >> 5] >> (ty0 & 31) & 1)) {
                ty0++;
                continue;
            }
            u32 ty1 = ty0 + 1;
            while (ty1 < th && (rows[ty1 >> 5] >> (ty1 & 31) & 1)) ty1++;
            decode_level(src, e.w, e.h, e.fmt, (u32 *)f.image, f.pitch, ty0, ty1);
            // memory rows top down land bottom up: tile rows ty0 to ty1 are image rows h - 8 * ty1 on
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, (u32 *)f.image + (u64)(e.h - ty1 * 8) * f.pitch, (ty1 - ty0) * 8 * f.pitch * 4);
            st_upload_bytes += (ty1 - ty0) * e.row_bytes;
            ty0 = ty1;
        }
    } else {
        decode_level(src, e.w, e.h, e.fmt, (u32 *)f.image, f.pitch);
        u32 off = texture_level_bytes(e.w, e.h, e.fmt);
        for (u32 l = 1; l < e.levels; l++) {
            const u32 lw = std::max(e.w >> l, 8u), lh = std::max(e.h >> l, 8u);
            u8 *lp = l == 1 ? (u8 *)f.mipmaps : (u8 *)f.mipmaps + f.mipLevelOffset[l - 1];
            const u32 lpitch = std::max((f.pitch >> l), 1u);
            const u32 aligned = (lpitch + 63) & ~63u;
            decode_level(src + off, lw, lh, e.fmt, (u32 *)lp, aligned);
            off += texture_level_bytes(lw, lh, e.fmt);
        }
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize + f.mipmapSize);
        st_upload_bytes += e.bytes;
    }
    st_uploads++;
    if (kMeasure) {
        const u32 dt = (u32)(OSGetSystemTime() - t0);
        prof[P_TEXUP] += dt;
        st_upload_ms += OSTicksToMicroseconds((u64)dt) / 1000.0;
    }
}

// from: where to say which surface the texture is, when it's one
GX2Texture *get_texture(u32 addr, u32 w, u32 h, u32 fmt, u32 levels, Surface **from = nullptr) {
    // render to texture: sample the surface itself. Not if it's another format: that memory has since been given to an ordinary texture.
    const auto read_as_this = [&](const Surface &s) { return s.w == w && (s.h == h || s.tex_h == h) && s.fmt == fmt && !s.guest_newer; };
    if (Surface *s = find_surface(addr, false, read_as_this)) {
        if (from) *from = s;
        flush_surface(*s);
        if (gxt()) LOG("[gxt] tex %08x %ux%u fmt %u from surface\n", addr, w, h, fmt);
        return &s->tex;
    }
    const u64 tkey = tex_key(addr, w, h, fmt, levels);
    if (auto hit = tex_index.find(tkey); hit != tex_index.end()) {
        Tex &e = *hit->second;
        if (stale_check && e.last_use != use_counter && !e.dirty && !e.maybe_dirty && !e.hash_stale && hash_guest(pptr(addr), e.bytes, std::array<u32, SLICES>().data()) != e.hash) {
            static u32 logged;
            if (logged++ < 400) LOG("STALE tex %08x %ux%u fmt %u levels %u bytes %u, flip %llu\n", addr, w, h, fmt, levels, e.bytes, (unsigned long long)st_flips);
            e.maybe_dirty = true;
        }
        e.last_use = use_counter;
        if (e.heap_flushes != heap_flushes) {
            e.heap_flushes = heap_flushes;
            const OSTime th = prof_now();
            if (sample_guest(pptr(addr), e.bytes) != e.sample) {
                e.maybe_dirty = true;
            } else if (!e.hash_stale) {
                // The sample reads one word in 32 or fewer, so a few changed tiles can leave it as it was. Every byte gets checked too: all of
                // them when such flushes are rare (after a file is read), and a run of them per flush when a game flushes every frame, which
                // finds any change within SLICES flushes at a 32nd of a full hash's cost (4 ms a MB on a console).
                if (heap_flush_gap > SLICES) {
                    if (hash_guest(pptr(addr), e.bytes, std::array<u32, SLICES>().data()) != e.hash) e.maybe_dirty = true;
                } else {
                    const u32 k = e.slice_next++ % SLICES;
                    if (hash_slice(pptr(addr), e.bytes, k) != e.slice[k]) e.maybe_dirty = true;
                }
            }
            prof_add(P_HASH, th);
        }
        if (e.dirty || e.maybe_dirty) {
            st_rehashes++;
            const OSTime th = prof_now();
            const u32 sample = sample_guest(pptr(addr), e.bytes);
            // A full hash of a 1 MB texture takes about 4 ms on a console, which is wasted if a definite write or a different sample already says it changed
            const bool changed = e.dirty || sample != e.sample;
            // Hashed unless a definite write said so: an upload on a changed sample alone used to leave the hash unknown too, and the next
            // write whose sample happened to match (two characters' textures, transparent at the sampled points) was taken for no change.
            // Dream Team drew Goombas with Luigi's head that way (2026-11-08).
            const u32 hh = e.dirty ? 0 : hash_guest(pptr(addr), e.bytes, e.slice);
            e.sample = sample;
            prof_add(P_HASH, th);
            // A stale hash (the last upload took a definite write's word for it) with the same sample: the image is current, only the hash is
            // unknown, so it's taken now. Uploading instead redid NSMB2's whole 1 MB atlas after nearly every partial update.
            if (changed || (!e.hash_stale && hh != e.hash)) {
                // the GPU may be reading the old contents
                if (gxt()) LOG("[gxt] reupload %08x %ux%u fmt %u, %u bytes, dirty %d\n", addr, w, h, fmt, e.bytes, e.dirty);
                last_reupload = {addr, w, h, fmt, levels, e.dirty};
                // definite writes to part of a one-level texture: only the rows they touched, and the rows the other image missed meanwhile.
                // A GPU-decoded one is written in order with the draws that read it: nothing to wait for and nothing to spare.
                const bool part = e.dirty && !e.maybe_dirty && e.levels == 1 && e.h <= 1024;
                u32 rows[4];
                memcpy(rows, e.dirty_rows, sizeof rows);
                if (!e.gpu && replace_image(e)) {
                    for (u32 k = 0; k < 4; k++) rows[k] |= e.lag_rows[k];
                    if (part) memcpy(e.lag_rows, e.dirty_rows, sizeof rows);
                    else memset(e.lag_rows, 0xff, sizeof rows);
                }
                upload_texture(e, part ? rows : nullptr);
            }
            e.hash = hh;
            e.hash_stale = e.dirty;
            e.dirty = e.maybe_dirty = false;
            memset(e.dirty_rows, 0, sizeof e.dirty_rows);
        }
        return &e.t;
    }
    const u8 *src = pptr(addr);
    if (!src || w < 8 || h < 8) return nullptr;
    u32 total = 0;
    for (u32 l = 0; l < levels; l++) total += texture_level_bytes(std::max(w >> l, 8u), std::max(h >> l, 8u), fmt);
    if (!pptr(addr + total - 1)) return nullptr;
    auto p = std::make_unique<Tex>();
    Tex &e = *p;
    e.addr = addr;
    e.w = w;
    e.h = h;
    e.fmt = fmt;
    e.levels = levels;
    e.bytes = total;
    e.dirty = e.maybe_dirty = e.hash_stale = false;
    e.row_bytes = w / 8 * (64 * k_fmt_bits[fmt & 15] / 8);
    e.last_use = use_counter;
    memset(&e.t, 0, sizeof e.t);
    GX2Surface &f = e.t.surface;
    e.gpu = gpu_decoded(w, h, fmt, levels);
    f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    f.width = w;
    f.height = h;
    f.depth = 1;
    f.mipLevels = levels;
    f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    f.aa = GX2_AA_MODE1X;
    f.use = e.gpu ? (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE) : GX2_SURFACE_USE_TEXTURE;
    f.tileMode = e.gpu ? GX2_TILE_MODE_DEFAULT : GX2_TILE_MODE_LINEAR_ALIGNED;
    e.t.viewNumMips = levels;
    e.t.viewNumSlices = 1;
    e.t.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&f);
    GX2InitTextureRegs(&e.t);
    e.mem = f.imageSize + f.mipmapSize;
    f.image = mem::host_alloc(f.alignment, e.mem);
    if (!f.image) {  // the cache's limit is a budget, not what's left: half of what no queued draw reads goes, and it's asked again
        evict_textures(tex_memory / 2);
        f.image = mem::host_alloc(f.alignment, e.mem);
    }
    if (!f.image) {  // still none: the vertex cache and the textures no draw of this frame reads go too (relieve_memory)
        relieve_memory(e.mem);
        f.image = mem::host_alloc(f.alignment, e.mem);
    }
    if (!f.image) FATAL("out of memory for a %u byte texture\n", e.mem);
    junk(f.image, e.mem, 2);
    if (e.gpu) for_gpu_writes(f.image, e.mem);  // the CPU decoder's writes are flushed as it goes; the GPU's decode isn't
    if (f.mipmapSize) f.mipmaps = (u8 *)f.image + f.imageSize;
    if (e.gpu) {
        memset(&e.cb, 0, sizeof e.cb);
        e.cb.surface = f;
        e.cb.viewNumSlices = 1;
        GX2InitColorBufferRegs(&e.cb);
        // the texture's bytes, four to a staging texel: 2 texels a block for ETC1, 4 for ETC1A4
        memset(&e.stage, 0, sizeof e.stage);
        GX2Surface &s = e.stage.surface;
        s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        s.width = fmt >= 12 ? w / 4 * (fmt == 13 ? 4 : 2) : w * k_fmt_bits[fmt] / 32;
        s.height = h / 8 * stage_rows(fmt);
        s.depth = 1;
        s.mipLevels = 1;
        s.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        s.aa = GX2_AA_MODE1X;
        s.use = GX2_SURFACE_USE_TEXTURE;
        s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        e.stage.viewNumMips = e.stage.viewNumSlices = 1;
        e.stage.compMap = 0x00010203;
        GX2CalcSurfaceSizeAndAlignment(&s);
        GX2InitTextureRegs(&e.stage);
    }
    tex_memory += e.mem;
    e.hash = hash_guest(src, total, e.slice);
    e.sample = sample_guest(src, total);
    e.heap_flushes = heap_flushes;
    upload_texture(e);
    tex_index[tkey] = p.get();
    textures.push_back(std::move(p));
    if (tex_memory > TEX_MEMORY_LIMIT) evict_textures();
    return &textures.back()->t;
}

// samplers, by state
struct SamplerKey {
    u8 ws, wt, mag, min, mip;
    u8 levels;
    s16 bias;
    bool operator==(const SamplerKey &o) const { return !memcmp(this, &o, sizeof o); }
};
struct SamplerHash {
    size_t operator()(const SamplerKey &k) const { return (size_t)fnv64(&k, sizeof k); }
};
std::unordered_map<SamplerKey, std::unique_ptr<GX2Sampler>, SamplerHash> samplers;

GX2TexClampMode clamp_mode(u32 wrap) {
    switch (wrap) {
    case 2: case 6: return GX2_TEX_CLAMP_MODE_WRAP;
    case 3: case 7: return GX2_TEX_CLAMP_MODE_MIRROR;
    default: return GX2_TEX_CLAMP_MODE_CLAMP;  // border is handled in the shader
    }
}

GX2Sampler *get_sampler(const TexUnitState &t, u32 levels) {
    SamplerKey k{t.wrap_s, t.wrap_t, t.mag_linear, t.min_linear, t.mip_linear, (u8)levels, (s16)(levels > 1 ? t.bias * 16 : 0)};
    auto it = samplers.find(k);
    if (it != samplers.end()) return it->second.get();
    auto s = std::make_unique<GX2Sampler>();
    GX2InitSampler(s.get(), GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSamplerClamping(s.get(), clamp_mode(t.wrap_s), clamp_mode(t.wrap_t), GX2_TEX_CLAMP_MODE_CLAMP);
    GX2InitSamplerXYFilter(s.get(), t.mag_linear ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT,
                           t.min_linear ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT, GX2_TEX_ANISO_RATIO_NONE);
    GX2InitSamplerZMFilter(s.get(), GX2_TEX_Z_FILTER_MODE_POINT,
                           levels > 1 ? (t.mip_linear ? GX2_TEX_MIP_FILTER_MODE_LINEAR : GX2_TEX_MIP_FILTER_MODE_POINT) : GX2_TEX_MIP_FILTER_MODE_NONE);
    GX2InitSamplerLOD(s.get(), 0.0f, (float)(levels - 1), levels > 1 ? t.bias : 0.0f);
    GX2Sampler *r = s.get();
    samplers.emplace(k, std::move(s));
    return r;
}

GX2Texture black_tex;
GX2Sampler lut_sampler;

GX2Texture make_float_lut(u32 w, u32 h) {
    GX2Texture t;
    memset(&t, 0, sizeof t);
    GX2Surface &f = t.surface;
    f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    f.width = w;
    f.height = h;
    f.depth = 1;
    f.mipLevels = 1;
    f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;  // 16 bit value and difference: no byte order question, on any GPU
    f.aa = GX2_AA_MODE1X;
    f.use = GX2_SURFACE_USE_TEXTURE;
    f.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    t.viewNumMips = 1;
    t.viewNumSlices = 1;
    t.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&f);
    GX2InitTextureRegs(&t);
    f.image = memalign(f.alignment, f.imageSize);
    return t;
}

// a table entry for the shaders: value 0..1 in r, g and difference -1..1 in b, a
inline u32 lut_texel(float val, float diff) {
    const u32 v = (u32)(std::clamp(val, 0.0f, 1.0f) * 65535.0f + 0.5f);
    const u32 d = (u32)((std::clamp(diff, -1.0f, 1.0f) + 1.0f) * 0.5f * 65535.0f + 0.5f);
    return v << 16 | d;
}

// Lighting and fog tables, kept by what's in them: materials take turns rewriting the tables, so the same few keep coming back.
// The GPU may still be reading a recent one, so there are enough that the oldest is normally finished with; waiting for it is what costs.
struct LutTex {
    u64 key = 0;
    bool valid = false;
    GX2Texture t;
    u64 last_use = 0;
};
constexpr u32 LIGHT_POOL = 64, FOG_POOL = 16;
std::vector<LutTex> light_pool, fog_pool;

// true: it's new, and needs filling
bool find_lut(std::vector<LutTex> &pool, u32 max, u64 key, u32 w, u32 h, LutTex *&cur) {
    if (cur && cur->key == key) {
        cur->last_use = use_counter;
        return false;
    }
    for (auto &p : pool)
        if (p.valid && p.key == key) {
            p.last_use = use_counter;
            cur = &p;
            return false;
        }
    if (pool.capacity() < max) pool.reserve(max);  // draws hold pointers to these
    if (pool.size() < max) {
        pool.emplace_back();
        pool.back().t = make_float_lut(w, h);
    }
    auto it = std::min_element(pool.begin(), pool.end(), [](const LutTex &a, const LutTex &b) { return a.last_use < b.last_use; });
    st_luts++;
    if (it->valid && it->last_use + 2 >= use_counter) draw_done();
    it->key = key;
    it->valid = true;
    it->last_use = use_counter;
    cur = &*it;
    return true;
}
}  // namespace

u32 gxt_from = ~0u, gxt_left;
bool gxt_cpu_vs;  // a file named "cpuvs" in the data folder: every vertex program runs on the CPU
bool gxt() {
    if (g_frame < gxt_from || !gxt_left) return false;
    gxt_left--;
    return true;
}

void *gx2_light_lut() {
    static LutTex *cur;
    if (find_lut(light_pool, LIGHT_POOL, g.light_lut_hash, 256, 24, cur)) {
        GX2Surface &f = cur->t.surface;
        u32 *d = (u32 *)f.image;
        for (u32 y = 0; y < 24; y++, d += f.pitch)
            for (u32 x = 0; x < 256; x++) {
                // lut_texel of (raw & 0xfff) / 4095 and the 11 bit difference / 2047, sign in bit 23, in whole numbers
                const u32 raw = g.light_lut[y][x], v = raw & 0xfff, delta = (((raw >> 12) & 0x7ff) * 1049072u) >> 16;
                d[x] = (v << 4 | v >> 8) << 16 | ((raw >> 23) & 1 ? 32768 - delta : 32768 + delta);
            }
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize);
    }
    return cur;
}

void *gx2_fog_lut() {
    static LutTex *cur;
    if (find_lut(fog_pool, FOG_POOL, g.fog_lut_hash, 128, 1, cur)) {
        GX2Surface &f = cur->t.surface;
        u32 *d = (u32 *)f.image;
        for (u32 x = 0; x < 128; x++) d[x] = lut_texel(((g.fog_lut[x] >> 13) & 0x7ff) / 2047.0f, sbits(g.fog_lut[x], 0, 13) / 2047.0f);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize);
    }
    return cur;
}

static void upload_target(Surface &s);

namespace {

// ---------------------------------------------------------------- render state
void bind_targets(const RTConfig &rt) {
    Surface *c = get_surface(rt.color_addr, rt.w, rt.h, rt.color_fmt, false);
    c->linear = false;  // what the GPU draws into is tiled
    Surface *d = rt.depth_addr && pptr(rt.depth_addr) ? get_surface(rt.depth_addr, rt.w, rt.h, rt.depth_fmt, true) : nullptr;
    take_dyn_level(*c);
    if (d) take_dyn_level(*d);
    // fills the game asked for while these surfaces did not exist yet
    for (size_t i = 0; i < pending_fills.size();) {
        const Fill &f = pending_fills[i];
        Surface *hit = f.start == c->addr ? c : d && f.start == d->addr ? d : nullptr;
        if (hit) {
            const Fill copy = f;
            pending_fills.erase(pending_fills.begin() + i);
            apply_fill(*hit, copy);
        } else {
            i++;
        }
    }
    if (c->first_use) {
        c->first_use = false;
        if (c->guest_newer) upload_target(*c);
    }
    c->guest_newer = false;  // shortcut: a target the game writes into after its first use keeps what the GPU drew, as in Cemu; upload it too once a game shows it must
    if (d && d->guest_newer) d->guest_newer = false;
    if (c != cur_color) {
        if (gxt()) LOG("[gxt] target %08x %ux%u fmt %u, depth %08x\n", rt.color_addr, rt.w, rt.h, rt.color_fmt, rt.depth_addr);
        if (cur_color) flush_surface(*cur_color);
        GX2SetColorBuffer(&c->cb, GX2_RENDER_TARGET_0);
        cur_color = c;
        GX2SetViewport(0, 0, (float)c->sw, (float)c->sh, 0, 1);
        GX2SetScissor(0, 0, c->sw, c->sh);
        state_sent_valid = false;  // those two aren't the draw's, even if this target is the one its state was last sent for
    }
    if (d != cur_depth) {
        if (cur_depth) flush_surface(*cur_depth);
        if (d) GX2SetDepthBuffer(&d->db);
        cur_depth = d;
    }
    gt_mark(GT_DRAW, c->addr);
}

// A draw is about to use the depth buffer just bound. If it's in the shared block, it needs its pending clear now. If it has neither that nor the block's contents, the game
// came back to it without clearing it and these two can't share: this frame's depth is lost, and from now on it has memory of its own.
void take_depth_block(const RTConfig &rt) {
    Surface &d = *cur_depth;
    if (!d.shared_depth) return;
    if (depth_owner != &d && !d.clear_pending) {
        LOG("[gx2] depth %08x %ux%u came back with no clear: giving it its own memory\n", d.addr, d.w, d.h);
        GX2Surface &f = d.db.surface;
        f.image = alloc_surface_mem(f.imageSize, f.alignment, d.mem1, false);
        DCFlushRange(f.image, f.imageSize);
        d.shared_depth = false;
        d.clear_pending = true;
        GX2SetDepthBuffer(&d.db);
    }
    if (d.shared_depth) depth_owner = &d;
    if (!d.clear_pending) return;
    d.clear_pending = false;
    GX2ClearDepthStencilEx(&d.db, d.db.depthClear, d.db.stencilClear, GX2_CLEAR_FLAGS_BOTH);
    d.gpu_newer = d.written_since_flush = true;
    restore_context();
    bind_targets(rt);
}

void send_state(const DrawState &ds, Surface &c, bool has_depth, bool stencil) {
    const float Sx = c.sx, Sy = c.sy;
    // At 4.5 times the 3DS's size every other 3DS pixel edge lands on the centre of a TV pixel, where two quads meeting at that edge can both leave the pixel to the other: the
    // console drew Dream Team's text boxes with a thin line where their pieces meet, and Cemu's GPU never did. A quarter of a pixel moves every edge off the centres.
    const float nudge = Sx != floorf(Sx) || Sy != floorf(Sy) ? 0.25f : 0.0f;
    GX2SetViewport(ds.vp_x * Sx + nudge, ds.vp_y * Sy + c.row0 + nudge, 2 * ds.vp_hw * Sx, 2 * ds.vp_hh * Sy, 0.0f, 1.0f);
    if (ds.scissor_mode == 3) {
        const s32 x1 = std::clamp<s32>(ds.sc_x1, 0, c.w), x2 = std::clamp<s32>(ds.sc_x2, 0, c.w);
        const s32 y1 = std::clamp<s32>(ds.sc_y1, 0, c.h), y2 = std::clamp<s32>(ds.sc_y2, 0, c.h);
        GX2SetScissor((u32)(x1 * Sx), (u32)(y1 * Sy) + c.row0, (u32)((x2 - x1) * Sx), (u32)((y2 - y1) * Sy));
    } else {
        GX2SetScissor(0, c.row0, c.sw, c.sh - c.row0);
    }
    // PICA culling is by winding in window coordinates, y up; the vertex shader negates y
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CW, ds.cull == 1, ds.cull == 2);
    // colour
    const u32 m = ds.color_mask;
    u32 mask = 0;
    if (m & 0xff) mask |= GX2_CHANNEL_MASK_R;
    if (m & 0xff00) mask |= GX2_CHANNEL_MASK_G;
    if (m & 0xff0000) mask |= GX2_CHANNEL_MASK_B;
    if (m & 0xff000000u) mask |= GX2_CHANNEL_MASK_A;
    if (!ds.color_write) mask = 0;
    GX2SetTargetChannelMasks((GX2ChannelMask)mask, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0);
    static const u8 k_rop[16] = {0x00, 0x88, 0x44, 0xCC, 0xFF, 0x33, 0xAA, 0x55, 0x77, 0xEE, 0x11, 0x66, 0x99, 0x22, 0xDD, 0xBB};
    const GX2LogicOp rop = ds.blend ? GX2_LOGIC_OP_COPY : (GX2LogicOp)k_rop[ds.logic_op & 15];
    GX2SetColorControl(rop, ds.blend ? 1 : 0, FALSE, mask != 0);
    if (ds.blend) {
        static const GX2BlendMode k_factor[15] = {
            GX2_BLEND_MODE_ZERO,      GX2_BLEND_MODE_ONE,        GX2_BLEND_MODE_SRC_COLOR, GX2_BLEND_MODE_INV_SRC_COLOR, GX2_BLEND_MODE_DST_COLOR,
            GX2_BLEND_MODE_INV_DST_COLOR, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA, GX2_BLEND_MODE_INV_DST_ALPHA,
            GX2_BLEND_MODE_BLEND_FACTOR, GX2_BLEND_MODE_INV_BLEND_FACTOR, GX2_BLEND_MODE_CONSTANT_ALPHA, GX2_BLEND_MODE_INV_CONSTANT_ALPHA, GX2_BLEND_MODE_SRC_ALPHA_SAT};
        static const GX2BlendCombineMode k_eq[5] = {GX2_BLEND_COMBINE_MODE_ADD, GX2_BLEND_COMBINE_MODE_SUB, GX2_BLEND_COMBINE_MODE_REV_SUB, GX2_BLEND_COMBINE_MODE_MIN, GX2_BLEND_COMBINE_MODE_MAX};
        auto alpha_factor = [&](u32 f) {
            // in the alpha channel, colour factors act on alpha
            if (f == 2) return GX2_BLEND_MODE_SRC_ALPHA;
            if (f == 3) return GX2_BLEND_MODE_INV_SRC_ALPHA;
            if (f == 4) return GX2_BLEND_MODE_DST_ALPHA;
            if (f == 5) return GX2_BLEND_MODE_INV_DST_ALPHA;
            if (f == 10) return GX2_BLEND_MODE_CONSTANT_ALPHA;
            if (f == 11) return GX2_BLEND_MODE_INV_CONSTANT_ALPHA;
            return k_factor[f & 15 ? std::min<u32>(f, 14) : 0];
        };
        GX2SetBlendControl(GX2_RENDER_TARGET_0, k_factor[std::min<u32>(ds.sf_rgb, 14)], k_factor[std::min<u32>(ds.df_rgb, 14)], k_eq[std::min<u32>(ds.eq_rgb, 4)], TRUE,
                           alpha_factor(ds.sf_a), alpha_factor(ds.df_a), k_eq[std::min<u32>(ds.eq_a, 4)]);
        const u32 bc = ds.blend_color;
        GX2SetBlendConstantColor((bc & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, ((bc >> 16) & 255) / 255.0f, (bc >> 24) / 255.0f);
    }
    // depth and stencil
    static const GX2CompareFunction k_cmp[8] = {GX2_COMPARE_FUNC_NEVER, GX2_COMPARE_FUNC_ALWAYS, GX2_COMPARE_FUNC_EQUAL, GX2_COMPARE_FUNC_NOT_EQUAL,
                                                GX2_COMPARE_FUNC_LESS, GX2_COMPARE_FUNC_LEQUAL, GX2_COMPARE_FUNC_GREATER, GX2_COMPARE_FUNC_GEQUAL};
    // The PICA writes depth with its test off, and games clear depth that way. On GX2 no test means no writes, so the test stays on and always passes.
    GX2SetDepthStencilControl(has_depth && (ds.depth_test || ds.depth_write), has_depth && ds.depth_write, ds.depth_test ? k_cmp[ds.depth_func & 7] : GX2_COMPARE_FUNC_ALWAYS, stencil, stencil,
                              k_cmp[ds.st_func & 7], (GX2StencilFunction)ds.st_zpass, (GX2StencilFunction)ds.st_zfail, (GX2StencilFunction)ds.st_fail,
                              k_cmp[ds.st_func & 7], (GX2StencilFunction)ds.st_zpass, (GX2StencilFunction)ds.st_zfail, (GX2StencilFunction)ds.st_fail);
    if (stencil) GX2SetStencilMask(ds.st_mask_in, ds.st_mask_w, ds.st_ref, ds.st_mask_in, ds.st_mask_w, ds.st_ref);
}

// Everything send_state gives the GPU comes from these. A draw whose values are the last draw's sends nothing: a batch of sprites or tiles
// repeats them draw after draw, and each of the eight or so calls costs on a console whether or not it changes anything. The record is
// dropped wherever something else sets render state (state_lost, a change of target) or leaves some of it changed behind a draw (an
// outline's polygon offset).
struct StateSent {
    const Surface *c, *d;
    float sx, sy, vp[4];
    u32 row0, sw, sh, w, h;
    s32 sc[4];
    u32 color_mask, blend_color, st[3];
    u8 scissor_mode, cull, color_write, blend, logic_op, f[6], depth_test, depth_write, depth_func, stencil, st_ops[4];
};
StateSent state_sent;

void set_state(const DrawState &ds) {
    Surface &c = *cur_color;
    const bool has_depth = cur_depth != nullptr;
    const bool stencil = ds.stencil && has_depth;
    StateSent now;
    memset(&now, 0, sizeof now);  // the padding too: the two are compared as bytes
    now.c = &c;
    now.d = cur_depth;
    now.sx = c.sx, now.sy = c.sy;
    now.vp[0] = ds.vp_x, now.vp[1] = ds.vp_y, now.vp[2] = ds.vp_hw, now.vp[3] = ds.vp_hh;
    now.row0 = c.row0, now.sw = c.sw, now.sh = c.sh, now.w = c.w, now.h = c.h;
    now.scissor_mode = (u8)ds.scissor_mode;
    if (ds.scissor_mode == 3) now.sc[0] = ds.sc_x1, now.sc[1] = ds.sc_y1, now.sc[2] = ds.sc_x2, now.sc[3] = ds.sc_y2;
    now.cull = (u8)ds.cull;
    now.color_mask = ds.color_mask;
    now.color_write = ds.color_write;
    now.blend = ds.blend;
    now.logic_op = (u8)ds.logic_op;
    if (ds.blend) {
        now.f[0] = (u8)ds.sf_rgb, now.f[1] = (u8)ds.df_rgb, now.f[2] = (u8)ds.eq_rgb, now.f[3] = (u8)ds.sf_a, now.f[4] = (u8)ds.df_a, now.f[5] = (u8)ds.eq_a;
        now.blend_color = ds.blend_color;
    }
    now.depth_test = ds.depth_test, now.depth_write = ds.depth_write, now.depth_func = (u8)ds.depth_func;
    now.stencil = stencil;
    if (stencil) {
        now.st_ops[0] = (u8)ds.st_func, now.st_ops[1] = (u8)ds.st_zpass, now.st_ops[2] = (u8)ds.st_zfail, now.st_ops[3] = (u8)ds.st_fail;
        now.st[0] = ds.st_mask_in, now.st[1] = ds.st_mask_w, now.st[2] = ds.st_ref;
    }
    if (!state_sent_valid || memcmp(&now, &state_sent, sizeof now)) {
        state_sent = now;
        state_sent_valid = true;
        send_state(ds, c, has_depth, stencil);
    }
    if (ds.color_write) {
        c.written_since_flush = c.gpu_newer = true;
        c.drawn = ++gpu_writes;
        if (cpu_reads(c) && !c.in_list) {
            c.in_list = true;
            c.was_sampled = c.sampled;
            c.sampled = false;
            read_targets.push_back(&c);
        }
    }
    if (has_depth && (ds.depth_write || (stencil && ds.st_mask_w))) cur_depth->written_since_flush = cur_depth->gpu_newer = true;
}

struct PsUni {
    u32 depth[4];
    u32 scissor[4];
    u32 konst[6][4];
    u32 tevinit[3][4];
    u32 fog[4];
    u32 border[3][4];
    u32 shadow[4];
    u32 lscale0[4], lscale1[4], ambient[4];
    struct { u32 spec0[4], spec1[4], diffuse[4], ambient[4], pos[4], spot[4], att[4]; } lights[8];
};
// what gen_uber_fs() reads after that, in place of a compiled-in state
struct PsUber {
    u32 csrc[6][4], cmod[6][4], asrc[6][4], amod[6][4], stage[6][4];
    u32 tex_coord[3][4], tex_add[3][4], wrap[3][4];
    u32 uber[4];
    u32 lit[4], lit2[4], lit3[4], lit4[4], sel[7][4], light[4][4], light2[4][4];  // only the lit one reads these
};
// the procedural texture's settings, after PsUni for the shaders that read it (uPt in shadergen.cpp)
struct PsProctex {
    u32 noise[4];    // amplitude u, v, phase u, v (no amplitude: no noise)
    u32 scale[4];    // noise frequency u, v, lookup offset, lookup width - 1
    u32 coord[4];    // 1 for the texture coordinate set in use, then 1 if the unit is on
    u32 wrap[4];     // clamp u, v, shift u, v
    u32 combine[4];  // colour map, alpha map, separate alpha, interpolate colours
};

// The procedural texture's tables in a texture, one row each: noise, colour map, alpha map, colours, colour differences.
std::vector<LutTex> proctex_pool;
GX2Texture *proctex_lut(const std::shared_ptr<const ProcTexLuts> &luts) {
    static std::shared_ptr<const ProcTexLuts> held;  // held, so that a new table can't turn up at the same address
    static LutTex *cur;
    static u64 uploads;
    if (held == luts) {
        cur->last_use = use_counter;
        return &cur->t;
    }
    held = luts;
    find_lut(proctex_pool, FOG_POOL, ++uploads, 256, 5, cur);
    GX2Surface &f = cur->t.surface;
    u32 *d = (u32 *)f.image;
    for (u32 x = 0; x < 128; x++) {
        d[x] = lut_texel(luts->noise_v[x], luts->noise_d[x]);
        d[f.pitch + x] = lut_texel(luts->cmap_v[x], luts->cmap_d[x]);
        d[f.pitch * 2 + x] = lut_texel(luts->amap_v[x], luts->amap_d[x]);
    }
    for (u32 x = 0; x < 256; x++) {
        d[f.pitch * 3 + x] = __builtin_bswap32(luts->color[x]);
        u32 diff = 0;
        for (u32 k = 0; k < 4; k++) diff = diff << 8 | (u32)(luts->cdiff[x][k] / 2 + 128);
        d[f.pitch * 4 + x] = diff;
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize);
    return &cur->t;
}
GX2Texture *proctex_rand_tex() {
    static GX2Texture t = [] {
        GX2Texture n = make_float_lut(144, 144);
        GX2Surface &f = n.surface;
        u32 *d = (u32 *)f.image;
        for (u32 y = 0; y < 144; y++, d += f.pitch)
            for (u32 x = 0; x < 144; x++) d[x] = (u32)((proctex_rand(x, y) + 1.0f) * 127.5f + 0.5f) << 24;
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize);
        return n;
    }();
    return &t;
}

// Whole numbers as floats, many times a draw: a conversion goes through memory on this CPU, which a table lookup doesn't.
struct IntFloats {
    float u8s[256], s8s[256], s8_times_256[256];
    u32 unit_swapped[256];  // n / 255, as a uniform block wants it
    void fill() {
        for (int i = 0; i < 256; i++) {
            u8s[i] = (float)i;
            s8s[i] = (float)(s8)i;
            s8_times_256[i] = (float)(s8)i * 256.0f;
            unit_swapped[i] = swap_float(i / 255.0f);
        }
    }
} int_floats;

inline void put_rgba(u32 *d, u32 c) {
    d[0] = int_floats.unit_swapped[c & 255];
    d[1] = int_floats.unit_swapped[(c >> 8) & 255];
    d[2] = int_floats.unit_swapped[(c >> 16) & 255];
    d[3] = int_floats.unit_swapped[c >> 24];
}
inline void put3(u32 *d, const float *v, float w = 0) {
    d[0] = swap_float(v[0]);
    d[1] = swap_float(v[1]);
    d[2] = swap_float(v[2]);
    d[3] = swap_float(w);
}

// fragment uniforms and textures for this draw
void set_fragment(const DrawState &ds, bool uber) {
    const u32 lights = !ds.lighting ? 0 : uber ? std::min<u32>(ds.light.num_lights, 4) : ds.light.num_lights;
    const bool proctex = !uber && ds.need_shade && uses_proctex(ds);
    const u32 size = uber ? sizeof(PsUni) + sizeof(PsUber) : proctex ? sizeof(PsUni) + sizeof(PsProctex) : offsetof(PsUni, lights) + lights * sizeof(PsUni::lights[0]);
    auto *u = (PsUni *)ring_alloc((std::max<u32>(size, 256) + 255) & ~255u, 256);
    u->depth[0] = swap_float(ds.depth_scale);
    u->depth[1] = swap_float(ds.depth_offset);
    u->depth[2] = swap_float((float)ds.alpha_ref);
    u->depth[3] = swap_float(cur_color->sy);
    u->scissor[0] = __builtin_bswap32(ds.sc_x1);
    u->scissor[1] = __builtin_bswap32(ds.sc_y1);
    u->scissor[2] = __builtin_bswap32(ds.sc_x2);
    u->scissor[3] = __builtin_bswap32(ds.sc_y2);
    for (u32 k = 0; k < 6; k++) put_rgba(u->konst[k], ds.tev[k].konst);
    put_rgba(u->tevinit[0], ds.tev_init_out);
    put_rgba(u->tevinit[1], ds.tev_init_buf);
    put_rgba(u->tevinit[2], ds.tev_init_next);
    put_rgba(u->fog, ds.fog_color | 0xff000000u);
    for (u32 k = 0; k < 3; k++) put_rgba(u->border[k], ds.tex[k].border);
    u->shadow[0] = swap_float(ds.tex[0].shadow_bias / 16777215.0f * 2);
    u->shadow[1] = u->shadow[2] = u->shadow[3] = 0;
    if (lights) {
        const LightingState &l = ds.light;
        const LutSel *sels[7] = {&l.d0, &l.d1, &l.sp, &l.fr, &l.rb, &l.rg, &l.rr};
        for (u32 k = 0; k < 4; k++) u->lscale0[k] = swap_float(sels[k]->scale);
        for (u32 k = 0; k < 3; k++) u->lscale1[k] = swap_float(sels[k + 4]->scale);
        u->lscale1[3] = 0;
        put3(u->ambient, l.global_ambient);
        for (u32 k = 0; k < lights; k++) {
            const LightState &s = l.lights[k];
            auto &d = u->lights[k];
            put3(d.spec0, s.spec0);
            put3(d.spec1, s.spec1);
            put3(d.diffuse, s.diffuse);
            put3(d.ambient, s.ambient);
            put3(d.pos, s.pos);
            put3(d.spot, s.spot);
            d.att[0] = swap_float(s.da_scale);
            d.att[1] = swap_float(s.da_bias);
            d.att[2] = d.att[3] = 0;
        }
    }
    if (uber) {
        auto *b = (PsUber *)(u + 1);
        auto put_i = [](u32 *d, u32 x, u32 y, u32 z, u32 w) {
            d[0] = __builtin_bswap32(x);
            d[1] = __builtin_bswap32(y);
            d[2] = __builtin_bswap32(z);
            d[3] = __builtin_bswap32(w);
        };
        for (u32 i = 0; i < ds.tev_count; i++) {
            const TevStageState &t = ds.tev[ds.tev_list[i]];
            put_i(b->csrc[i], t.csrc[0], t.csrc[1], t.csrc[2], t.cop);
            put_i(b->cmod[i], t.cmod[0], t.cmod[1], t.cmod[2], t.cmul);
            put_i(b->asrc[i], t.asrc[0], t.asrc[1], t.asrc[2], t.aop);
            put_i(b->amod[i], t.amod[0], t.amod[1], t.amod[2], t.amul);
            put_i(b->stage[i], ds.tev_list[i], t.upd_rgb, t.upd_a, 0);
        }
        for (u32 k = 0; k < 3; k++) {
            const TexUnitState &t = ds.tex[k];
            const bool on = ds.need_tex[k] && !t.null_tex;
            const float use[4] = {t.coord == 0 ? 1.0f : 0.0f, t.coord == 1 ? 1.0f : 0.0f, t.coord >= 2 ? 1.0f : 0.0f, on ? 1.0f : 0.0f};
            for (u32 c = 0; c < 4; c++) b->tex_coord[k][c] = swap_float(use[c]);
            put_rgba(b->tex_add[k], ds.need_tex[k] && t.null_tex ? 0xff000000u : 0);
            put_i(b->wrap[k], on ? t.wrap_s : 0, on ? t.wrap_t : 0, 0, 0);
        }
        put_i(b->uber, ds.tev_count, ds.alpha_test ? ds.alpha_func : ~0u, ds.fog ? (ds.fog_flip ? 2 : 1) : 0, ds.need_tex[0] && !ds.tex[0].null_tex && ds.tex[0].type == 3);
        if (ds.lighting) {
            const LightingState &l = ds.light;
            put_i(b->lit, lights, l.bump_mode, l.bump_sel & 3, l.bump_renorm);
            put_i(b->lit2, l.config == 8, l.shadow_enable ? l.shadow_sel & 3 : ~0u, l.shadow_invert, l.shadow_alpha);
            put_i(b->lit3, l.primary_alpha, l.secondary_alpha, l.clamp_highlights, 0);
            put_i(b->lit4, l.shadow_primary, l.shadow_secondary, 0, 0);
            const LutSel *sels[7] = {&l.d0, &l.d1, &l.sp, &l.fr, &l.rb, &l.rg, &l.rr};
            for (u32 k = 0; k < 7; k++) put_i(b->sel[k], sels[k]->enabled, sels[k]->abs, sels[k]->input, 0);
            for (u32 k = 0; k < lights; k++) {
                const LightState &s = l.lights[k];
                put_i(b->light[k], s.num, s.directional, s.two_sided, s.shadow);
                put_i(b->light2[k], s.geo0, s.geo1, s.dist_atten, s.spot_atten);
            }
        }
    }
    if (proctex) {
        const ProcTexState &p = ds.proctex;
        const bool on = ds.need_tex[3], noise = on && p.noise;
        const float v[5][4] = {
            {noise ? p.noise_amp_u : 0, noise ? p.noise_amp_v : 0, p.noise_phase_u, p.noise_phase_v},
            {9 * p.noise_freq_u, 9 * p.noise_freq_v, (float)p.lut_offset, (float)p.lut_width - 1},
            {p.coord == 0 ? 1.0f : 0.0f, p.coord == 1 ? 1.0f : 0.0f, p.coord >= 2 ? 1.0f : 0.0f, on ? 1.0f : 0.0f},
            {(float)p.u_clamp, (float)p.v_clamp, (float)p.u_shift, (float)p.v_shift},
            {(float)p.color_comb, (float)p.alpha_comb, p.separate_alpha ? 1.0f : 0.0f, p.filter == 1 || p.filter == 3 || p.filter == 5 ? 1.0f : 0.0f},
        };
        for (u32 k = 0; k < 5; k++) put3((u32 *)(u + 1) + k * 4, v[k], v[k][3]);
    }
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, size);
    GX2SetPixelUniformBlock(ps_ubo_loc, size, u);
    lap(P_UNI);
    if (!ds.need_shade) return;
    for (u32 k = 0; k < 3; k++) {
        const TexUnitState &t = ds.tex[k];
        if (!ds.need_tex[k] || t.null_tex) {
            if (uber) {  // it samples all three whatever the state, and must not read a texture that's gone
                GX2SetPixelTexture(&black_tex, k);
                GX2SetPixelSampler(&lut_sampler, k);
            }
            continue;
        }
        Surface *drawn = nullptr;  // a picture the game drew, sampled where it lies
        GX2Texture *tex = get_texture(t.face_addr[0], t.w, t.h, t.fmt, t.levels, &drawn);
        if (drawn && !cpu_reads(*cur_color)) drawn->sampled = true;
        if (!tex) tex = &black_tex;
        GX2SetPixelTexture(tex, k);
        GX2SetPixelSampler(get_sampler(t, tex->surface.mipLevels), k);
        if (k == 0 && (t.type == 1 || t.type == 4))
            for (u32 f = 1; f < 6; f++) {
                GX2Texture *ft = get_texture(t.face_addr[f], t.w, t.h, t.fmt, t.levels);
                GX2SetPixelTexture(ft ? ft : tex, 2 + f);
                GX2SetPixelSampler(get_sampler(t, tex->surface.mipLevels), 2 + f);
            }
    }
    if (ds.lighting && ds.light.lut_tex) {
        auto *p = (LutTex *)ds.light.lut_tex;
        p->last_use = use_counter;
        GX2SetPixelTexture(&p->t, 8);
        GX2SetPixelSampler(&lut_sampler, 8);
    }
    if (ds.fog && ds.fog_tex) {
        auto *p = (LutTex *)ds.fog_tex;
        p->last_use = use_counter;
        GX2SetPixelTexture(&p->t, 9);
        GX2SetPixelSampler(&lut_sampler, 9);
    }
    if (proctex) {
        GX2SetPixelTexture(ds.need_tex[3] ? proctex_lut(ds.proctex.luts) : &black_tex, 10);
        GX2SetPixelSampler(&lut_sampler, 10);
        GX2SetPixelTexture(proctex_rand_tex(), 11);
        GX2SetPixelSampler(&lut_sampler, 11);
    }
    lap(P_TEX);
}

// The vertex uniform block: f[96], uDraw, uInt[4] (the integer uniforms: loop counts, starts and steps, read by the shader's loops since 11-06),
// then the outline's two vectors when a draw has them (shadergen.cpp). An integer uniform has three parts, so uInt[0].w is free: it holds the
// bool uniforms, a bit each, for the shaders that read them as they run (kVsBoolsMade).
constexpr u32 VS_BLOCK_WORDS = (96 + 1 + 4) * 4;
inline void put_vs_ints(u32 *u) {
    for (u32 n = 0; n < 4; n++)
        for (u32 c = 0; c < 4; c++) u[388 + n * 4 + c] = __builtin_bswap32((u32)g.vs.i[n][c]);
    u32 bools = 0;
    for (u32 i = 0; i < 16; i++) bools |= (u32)g.vs.b[i] << i;
    u[388 + 3] = __builtin_bswap32(bools);
}

// the vertex program's uniforms; often the same as for the last draw
void set_vertex_uniforms(const DrawState &ds, const V4 *f) {
    static V4 last_f[96];
    static float last_depth[2];
    static u8 last_i[4][4];
    static bool last_b[16];
    static u32 *last;
    static u32 last_seg = ~0u;
    const float dp[2] = {ds.depth_scale, ds.depth_offset};
    if (!(last && last_seg == ring_seg && !memcmp(last_f, f, sizeof last_f) && !memcmp(last_depth, dp, sizeof dp) && !memcmp(last_i, g.vs.i, sizeof last_i) &&
          !memcmp(last_b, g.vs.b, sizeof last_b))) {
        constexpr u32 size = VS_BLOCK_WORDS * 4;
        u32 *u = (u32 *)ring_alloc((size + 255) & ~255u, 256);
        const u32 *src = (const u32 *)f;
        for (u32 i = 0; i < 96 * 4; i++) u[i] = __builtin_bswap32(src[i]);
        u[384] = swap_float(ds.depth_scale);
        u[385] = swap_float(ds.depth_offset);
        u[386] = u[387] = 0;
        put_vs_ints(u);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, size);
        memcpy(last_f, f, sizeof last_f);
        memcpy(last_depth, dp, sizeof dp);
        memcpy(last_i, g.vs.i, sizeof last_i);
        memcpy(last_b, g.vs.b, sizeof last_b);
        last = u;
        last_seg = ring_seg;
    }
    GX2SetVertexUniformBlock(vs_ubo_loc, VS_BLOCK_WORDS * 4, last);
}

// whether the GPU fetches an attribute from its array's copy (vertex_arrays.h), or the CPU makes floats of it the old way
inline bool gpu_fetched(const Gx2Attr &at) { return !(gxt_cpu_attr >> at.fmt & 1) && fetchable(at); }

GX2AttribFormat attr_format(u32 fmt, u32 elems) {
    // 3 x 8 or 16 bits don't exist: fetch 4, and the mask supplies w. 16-bit integers are fetched as four however many there are, the mask
    // dropping the rest: the console's GPU fetches one or two as well, but Cemu has no decoder for those, leaves the attribute out without a
    // word on the game's side, and its pictures then show what no console draws (see check_ports.py's look at Cemu's own log).
    static const u32 base[4][4] = {
        {GX2_ATTRIB_TYPE_8, GX2_ATTRIB_TYPE_8_8, GX2_ATTRIB_TYPE_8_8_8_8, GX2_ATTRIB_TYPE_8_8_8_8},
        {GX2_ATTRIB_TYPE_8, GX2_ATTRIB_TYPE_8_8, GX2_ATTRIB_TYPE_8_8_8_8, GX2_ATTRIB_TYPE_8_8_8_8},
        {GX2_ATTRIB_TYPE_16_16_16_16, GX2_ATTRIB_TYPE_16_16_16_16, GX2_ATTRIB_TYPE_16_16_16_16, GX2_ATTRIB_TYPE_16_16_16_16},
        {GX2_ATTRIB_FORMAT_FLOAT_32, GX2_ATTRIB_FORMAT_FLOAT_32_32, GX2_ATTRIB_FORMAT_FLOAT_32_32_32, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32}};
    u32 f = base[fmt & 3][(elems - 1) & 3];
    if (fmt != 3) f |= GX2_ATTRIB_FLAG_SCALED;
    if (fmt == 0 || fmt == 2) f |= GX2_ATTRIB_FLAG_SIGNED;
    return (GX2AttribFormat)f;
}

// A texture the GPU decodes takes a draw of its own, with its own target and state: these have to go before anything of the draw that uses them is
// set. set_fragment's look at the same textures then finds them current.
void decode_textures_first(const DrawState &ds) {
    if (!ds.need_shade) return;
    for (u32 k = 0; k < 3; k++) {
        const TexUnitState &t = ds.tex[k];
        if (!ds.need_tex[k] || t.null_tex || !gpu_decoded(t.w, t.h, t.fmt, t.levels)) continue;
        get_texture(t.face_addr[0], t.w, t.h, t.fmt, t.levels);
        if (k == 0 && (t.type == 1 || t.type == 4))
            for (u32 f = 1; f < 6; f++) get_texture(t.face_addr[f], t.w, t.h, t.fmt, t.levels);
    }
}

void begin_draw_common(const DrawState &ds, GX2PixelShader *ps) {
    if (state_lost) {
        GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
        state_lost = false;
        state_sent_valid = false;
    }
    set_state(ds);
    GX2SetPixelShader(ps);
    lap(P_STATE);
    set_fragment(ds, ps && (ps == ps_uber || ps == ps_uber_lit));
}
}  // namespace

// ---------------------------------------------------------------- a draw with a compiled vertex shader
// The triangles of a mesh drawn by Level-5's outline shader, in its own walk of the index list: a strip with adjacency, where after the
// first two vertices every other one only says what's across an edge. A repeated vertex ends a strip, as does a vertex at the same place,
// since the shader compares positions. Returns the list, in the ring. Which way each triangle faces isn't known here: the shader finds
// silhouette edges whichever way a strip winds, and these rigs mirror half of a model with their bones.
static const u16 *outline_triangles(const Gx2Draw &d, const u8 *ip, u32 &count) {
    const Gx2Attr *pa = nullptr;
    for (u32 a = 0; a <= d.in_maxi && a < d.num_attr; a++)
        if (((d.in_map >> (4 * a)) & 0xf) == 0 && !d.attr[a].is_default && d.attr[a].stride) pa = &d.attr[a];
    const u8 *pos = pa ? pptr(d.base + pa->offset) : nullptr;
    const u32 bytes = pa ? pa->elems * (pa->fmt == 3 ? 4 : pa->fmt == 2 ? 2 : 1) : 0;
    auto same = [&](u32 x, u32 y) { return x == y || (pos && !memcmp(pos + pa->stride * x, pos + pa->stride * y, bytes)); };
    u16 *list = (u16 *)ring_alloc(d.count * 6 + 64, 64);
    u32 n = 0, state = 0, p1 = 0, p2 = 0, p3 = 0;
    bool ended = false, first = false;
    for (u32 i = 0; i < d.count; i++) {
        u32 v = ip[i];
        if (d.index_u16) {
            u16 x;
            memcpy(&x, ip + 2 * i, 2);
            v = le16(x);
        }
        switch (state) {
        case 0:  // a strip starts
            ended = false;
            first = true;
            p1 = v;
            state = 1;
            break;
        case 1: {
            const bool repeat = same(p1, v);
            p2 = p1;
            p1 = v;
            if (!repeat) state = 2;
            break;
        }
        case 2:  // across the first edge
            p3 = v;
            state = 3;
            break;
        case 3:  // the next vertex of the strip
            if (same(p1, v)) {
                ended = true;
            } else {
                // back onto the vertex before: the last triangle again
                if (first || !same(p3, v)) {
                    list[n++] = le16((u16)p2);
                    list[n++] = le16((u16)p1);
                    list[n++] = le16((u16)v);
                }
                p3 = p2;
                p2 = p1;
                p1 = v;
                first = false;
            }
            state = 4;
            break;
        default:  // across the newest edge
            state = ended ? 0 : 3;
            break;
        }
    }
    count = n;
    return list;
}

bool gx2_draw(const Gx2Draw &d) {
    const DrawState &ds = *d.ds;
    if (ds.skip || ds.shadow_mode || ds.gas || !ds.rt.color_addr || !pptr(ds.rt.color_addr) || (ds.rt.color_fmt & 0x80)) return true;
    if (gxt_cpu_vs) return false;  // debug aid: tells a wrong vertex shader from anything else
    lap_start();
    // usually the same vertex program as the last draw
    static struct {
        const ShaderProgram *prog;
        u32 entry;
        bool b[16];
        s8 slots[24];
        u64 key;
    } vk;
    if (vk.prog != d.prog || vk.entry != d.entry || memcmp(vk.b, g.vs.b, sizeof vk.b) || memcmp(vk.slots, d.slot_src, sizeof vk.slots)) {
        vk.prog = d.prog;
        vk.entry = d.entry;
        memcpy(vk.b, g.vs.b, sizeof vk.b);
        memcpy(vk.slots, d.slot_src, sizeof vk.slots);
        vk.key = vs_variant_key(d.prog, d.entry, g.vs.b, d.slot_src);
    }
    const u64 key = vk.key;
    auto it = vs_table.find(key);
    if (it == vs_table.end()) {
        // No shader for this draw's own set of bools. Where its entry reads more of them than a build makes every set of, the pack has one
        // that reads them as it runs (kVsBoolsMade), and this draw's key gets that shader.
        if (const u32 open = vs_open_bools(d.prog, d.entry)) {
            const auto any = vs_table.find(vs_variant_key(d.prog, d.entry, g.vs.b, d.slot_src, open));
            if (any != vs_table.end() && any->second.index != MISSED) {
                if (!any->second.shader) prepare_vs(any->second);
                const VsEntry shared = any->second;
                it = vs_table.emplace(key, shared).first;
            }
        }
    }
    if (it == vs_table.end()) {
        vs_table[key];  // log it once
        vs_table.find(key)->second.index = ~0u;
        // the shader generator on the PC reads these: little-endian
        miss_put("VS  ", 4);
        const u32 hdr[3] = {le32(d.entry), le32(g.vs.code_size), le32(g.vs.swizzle_size)};
        miss_put(hdr, sizeof hdr);
        for (u32 i = 0; i < g.vs.code_size; i++) {
            const u32 w = le32(g.vs.code[i]);
            miss_put(&w, 4);
        }
        for (u32 i = 0; i < g.vs.swizzle_size; i++) {
            const u32 w = le32(g.vs.swizzle[i]);
            miss_put(&w, 4);
        }
        miss_put(g.vs.b, 16);
        miss_put(g.vs.i, 16);
        miss_put(d.slot_src, 24);
        LOG("[gx2] no compiled vertex shader %016llx, entry %u\n", (unsigned long long)key, d.entry);
        // The console compiles the shader itself, as it does pixel shaders (install_compiled puts it in the table), and this draw and the
        // next ones go the CPU way meanwhile. A build's dump scan misses programs a game keeps outside shader binaries: 63 of
        // the 64 vertex shaders runs met and builds lacked (2026-11-10).
        if (glsl_vs && shader_compile_on()) {
            u32 bools = 0, inputs = 0;
            for (u32 i = 0; i < 16; i++)
                if (g.vs.b[i]) bools |= 1u << i;
            std::string glsl = gen_vs_glsl(d.prog, d.entry, bools, g.vs.i, d.slot_src, &inputs);
            if (!glsl.empty()) shader_compile_request(true, key, std::move(glsl), inputs);
        }
        return false;
    }
    VsEntry &vs = it->second;
    if (vs.index == ~0u) return false;
    if (!vs.shader) prepare_vs(vs);
    gx2_flush_cpu_batch();
    const u8 *ip = nullptr;
    if (d.indexed) {
        ip = pptr(d.index_addr);
        if (!ip) return true;
    }
    u32 draw_count = d.count;
    bool index_u16 = d.index_u16;
    if (d.outline_color) {
        ip = (const u8 *)outline_triangles(d, ip, draw_count);
        index_u16 = true;
        if (!draw_count) return true;
    }
    lap(P_KEY);
    decode_textures_first(ds);
    bind_targets(ds.rt);
    if (cur_depth && (ds.depth_test || ds.depth_write || ds.stencil)) take_depth_block(ds.rt);
    lap(P_BIND);
    GX2PixelShader *const ps = find_ps(ds);
    lap(P_PS);
    begin_draw_common(ds, ps);
    GX2SetVertexShader(vs.shader);
    if (d.outline_color) {
        // Both sides, pushed back by as much as the mesh's slope gives over the outline's width, plus enough to cover two shaders rounding
        // the same position differently: the model stays in front, and what's left is the part past its edge. set_state's cull write turns
        // this off again for the next draw.
        const float away = (ds.depth_func & 7) >= 6 ? -1.0f : 1.0f;
        GX2SetPolygonOffset(64.0f * away, 12.0f * away, 64.0f * away, 12.0f * away, 0.0f);
        GX2SetPolygonControl(GX2_FRONT_FACE_CW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, TRUE, TRUE, FALSE);
        state_sent_valid = false;  // the next draw's cull write has to go, to turn this off
        constexpr u32 size = (VS_BLOCK_WORDS + 8) * 4;
        u32 *u = (u32 *)ring_alloc((size + 255) & ~255u, 256);
        const u32 *src = (const u32 *)g.vs.f;
        for (u32 i = 0; i < 96 * 4; i++) u[i] = __builtin_bswap32(src[i]);
        u[384] = swap_float(ds.depth_scale);
        u[385] = swap_float(ds.depth_offset);
        u[386] = swap_float(0.003f * away);  // how far back what faces the viewer goes: gen_vs_glsl
        u[387] = 0;
        put_vs_ints(u);
        u32 *const o = u + VS_BLOCK_WORDS;  // uOutline, after uInt
        o[0] = swap_float(d.outline_thick[0]);
        o[1] = swap_float(d.outline_thick[1]);
        o[2] = swap_float(d.outline_scale_w ? 1.0f : 0.0f);
        o[3] = 0;
        for (u32 i = 0; i < 4; i++) o[4 + i] = swap_float(d.outline_color[i]);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, size);
        GX2SetVertexUniformBlock(vs_ubo_loc, size, u);
    } else {
        set_vertex_uniforms(ds, g.vs.f);
    }
    lap(P_VSUNI);

    // the range of vertices used, to make sure the GPU sees them
    u32 minv, maxv;
    if (d.indexed) {
        minv = 0xffff;
        maxv = 0;
        if (index_u16) {
            for (u32 i = 0; i < draw_count; i++) {
                u16 v;
                memcpy(&v, ip + 2 * i, 2);
                v = le16(v);
                if (v < minv) minv = v;
                if (v > maxv) maxv = v;
            }
        } else {
            for (u32 i = 0; i < draw_count; i++) {
                const u32 v = ip[i];
                if (v < minv) minv = v;
                if (v > maxv) maxv = v;
            }
        }
    } else {
        minv = d.first;
        maxv = d.first + d.count - 1;
    }

    lap(P_RANGE);
    s32 reg_attr[16];
    for (auto &r : reg_attr) r = -1;
    for (u32 a = 0; a <= d.in_maxi; a++) reg_attr[(d.in_map >> (4 * a)) & 0xf] = a < d.num_attr ? (s32)a : -1;
    FetchKey fk;
    fk.count = 0;
    u32 buf = 0;
    // The vertex arrays this draw reads, each copied once as it lies in the game's memory (one memcpy for all the attributes interleaved in it),
    // and the GPU fetches every attribute from the copy in its own format: floats, and 8- and 16-bit integers it turns into floats itself.
    // Each attribute used to get a copy of its own, and integers a float per component from the CPU: 1 to 5 ms a frame on the GPU thread in
    // busy scenes, the largest named cost in the user's console logs (2026-11-10). "cpuattr" brings that back.
    VertexArray arrays[16];
    u32 n_arrays = 0;
    u8 reg_array[16] = {};  // the array each input register's attribute went into
    for (u32 r = 0; r < 16; r++) {
        if (!(vs.inputs & (1u << r)) || vs.location[r] < 0 || reg_attr[r] < 0) continue;
        const Gx2Attr &at = d.attr[reg_attr[r]];
        if (!at.is_default && at.elems && at.stride && gpu_fetched(at)) reg_array[r] = (u8)join_vertex_array(arrays, n_arrays, at);
    }
    s32 array_buf[16];  // the attribute buffer each array's copy is bound as, once an attribute has asked for it
    std::fill(array_buf, array_buf + 16, -1);
    for (u32 r = 0; r < 16; r++) {
        if (!(vs.inputs & (1u << r)) || vs.location[r] < 0) continue;
        const s32 a = reg_attr[r];
        const Gx2Attr *at = a >= 0 ? &d.attr[a] : nullptr;
        const bool constant = !at || at->is_default || at->elems == 0 || at->stride == 0;
        if (constant) {
            static const float k_zero[4] = {0, 0, 0, 0}, k_unit[4] = {0, 0, 0, 1};
            const float *v = !at ? k_zero : at->is_default ? g.default_attr[a].v : k_unit;
            float *c = (float *)ring_alloc(64, 64);
            memcpy(c, v, 16);
            DCFlushRange(c, 16);
            GX2SetAttribBuffer(buf, 16, 16, c);
            fk.s[fk.count++] = float4_stream(vs.location[r], buf, 0, true);
            buf++;
            continue;
        }
        if (gpu_fetched(*at)) {
            const VertexArray &x = arrays[reg_array[r]];
            s32 &bound = array_buf[reg_array[r]];
            if (bound < 0) {
                const u8 *src = pptr(d.base + x.lo);
                if (!src) return true;
                // Copied, not read where it is: the game may rewrite it as soon as its list's interrupt comes, and a real GPU only gets to it later.
                // In steps of a multiple of 4 bytes, which is all a real GPU takes (vertex_arrays.h). The copy is kept (vtx_cached) until the game says it
                // wrote there: Luigi's Mansion: Dark Moon's draws reach 1300 to 4100 vertices, and copying each draw's arrays again took 20 to 50 MB a frame.
                const u32 count = maxv - minv + 1, bytes = gpu_array_bytes(x, count), step = gpu_stride(x.stride);
                if (kMeasure) {  // for the [prof] lines
                    st_attr_verts += count;
                    st_attr_bytes += bytes;
                    st_attr_streams++;
                }
                const u8 *copy = (const u8 *)vtx_cached(d.base + x.lo + x.stride * minv, src + x.stride * minv, x.stride, count, 0x80, bytes,
                                                        [&](void *out, const u8 *from) { copy_vertex_array((u8 *)out, from, x, count); },
                                                        vertex_array_bytes(x, count));
                bound = (s32)buf++;
                // addressed as if it held vertex 0 too, as the indices count from there
                GX2SetAttribBuffer((u32)bound, (maxv + 1) * step + 64, step, copy - minv * step);
            }
            GX2AttribStream s{};
            s.location = vs.location[r];
            s.buffer = (u32)bound;
            // the word an attribute's first component lies in is fetched from its start, and the attribute begins at this component of it
            const u32 lane = fetch_lane(*at);
            s.offset = fetch_offset(*at) - x.lo;
            // Cemu can't fetch one little-endian float: its sprites all got corner 0. Two do the same here, and the mask drops the second.
            s.format = at->fmt == 3 && at->elems == 1 ? GX2_ATTRIB_FORMAT_FLOAT_32_32 : attr_format(at->fmt, at->fmt < 2 ? 4 : at->elems);
            s.type = GX2_ATTRIB_INDEX_PER_VERTEX;
            s.aluDivisor = 0;
            // component k of the attribute is component lane + k of what's fetched (GX2_SQ_SEL_X to _W are 0 to 3); the rest are 0, 0, 1
            u32 sel[4] = {GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1};
            for (u32 k = 0; k < at->elems; k++) sel[k] = GX2_SQ_SEL_X + lane + k;
            s.mask = GX2_SEL_MASK(sel[0], sel[1], sel[2], sel[3]);
            s.endianSwap = GX2_ENDIAN_SWAP_NONE;  // guest memory is little-endian, as the GPU wants
            fk.s[fk.count++] = s;
            continue;
        }
        const u32 addr = d.base + at->offset;
        const u8 *p = pptr(addr);
        if (!p) return true;
        if (at->fmt != 3) {
            // Integer attributes (normals, colours, often texture coordinates) become floats here, a few microseconds per draw,
            // rather than depending on how each GPU path fetches little-endian integers. Only floats are read in place.
            const u32 stride = at->elems * 4;
            const u32 count = maxv - minv + 1;
            if (kMeasure) {  // for the [prof] lines
                st_attr_verts += count;
                st_attr_bytes += (u64)count * at->stride;
                st_attr_streams++;
            }
            const u32 src_stride = at->stride;
            const u32 elems = at->elems, fmt = at->fmt;
            const float *w = (const float *)vtx_cached(addr + src_stride * minv, p + src_stride * minv, src_stride, count, fmt | elems << 4, count * stride,
                                                       [&](void *out, const u8 *src) {
                float *dst = (float *)out;
                if (fmt == 2) {
                    for (u32 v = 0; v < count; v++, src += src_stride)
                        for (u32 c = 0; c < elems; c++) *dst++ = int_floats.s8_times_256[src[2 * c + 1]] + int_floats.u8s[src[2 * c]];
                } else if (fmt == 1) {
                    for (u32 v = 0; v < count; v++, src += src_stride)
                        for (u32 c = 0; c < elems; c++) *dst++ = int_floats.u8s[src[c]];
                } else {
                    for (u32 v = 0; v < count; v++, src += src_stride)
                        for (u32 c = 0; c < elems; c++) *dst++ = int_floats.s8s[src[c]];
                }
            });
            // the buffer is addressed as if it held vertex 0 too, as the indices count from there
            GX2SetAttribBuffer(buf, (maxv + 1) * stride, stride, (const u8 *)w - minv * stride);
            GX2AttribStream s{};
            s.location = vs.location[r];
            s.buffer = buf;
            s.offset = 0;
            s.format = attr_format(3, elems);
            s.type = GX2_ATTRIB_INDEX_PER_VERTEX;
            s.aluDivisor = 0;
            const u32 w1 = GX2_SQ_SEL_1;
            s.mask = elems == 1   ? GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, w1)
                     : elems == 2 ? GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, w1)
                     : elems == 3 ? GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, w1)
                                  : GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
            s.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
            fk.s[fk.count++] = s;
            buf++;
            continue;
        }
        GX2AttribStream s{};
        s.location = vs.location[r];
        s.buffer = buf;
        // Cemu can't fetch one little-endian float: its sprites all got corner 0. Two do the same here, and the mask drops the second.
        s.format = at->elems == 1 ? GX2_ATTRIB_FORMAT_FLOAT_32_32 : attr_format(at->fmt, at->elems);
        {
            // Copied, not read where it is: the game may rewrite it as soon as its list's interrupt comes, and a real GPU only gets to it later (see gx2_finish_list).
            const u32 elems = at->elems, stride = (elems == 1 ? 2 : elems) * 4, count = maxv - minv + 1, src_stride = at->stride;
            if (kMeasure) {  // for the [prof] lines
                st_attr_verts += count;
                st_attr_bytes += (u64)count * src_stride;
                st_attr_streams++;
            }
            const u32 *copy = (const u32 *)vtx_cached(addr + src_stride * minv, p + src_stride * minv, src_stride, count, 3 | elems << 4, count * stride,
                                                      [&](void *out, const u8 *src) {
                u32 *dst = (u32 *)out;
                for (u32 v = 0; v < count; v++, src += src_stride) {
                    memcpy(dst, src, elems * 4);  // from wherever the game keeps them, a multiple of 4 or not
                    dst += elems;
                    if (elems == 1) *dst++ = 0;
                }
            });
            GX2SetAttribBuffer(buf, (maxv + 1) * stride, stride, (const u8 *)copy - minv * stride);
            s.offset = 0;
        }
        s.type = GX2_ATTRIB_INDEX_PER_VERTEX;
        s.aluDivisor = 0;
        const u32 w = GX2_SQ_SEL_1;
        s.mask = at->elems == 1   ? GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, w)
                 : at->elems == 2 ? GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, w)
                 : at->elems == 3 ? GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, w)
                                  : GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
        s.endianSwap = GX2_ENDIAN_SWAP_NONE;  // guest memory is little-endian, as the GPU wants
        fk.s[fk.count++] = s;
        buf++;
    }
    lap(P_ATTR);
    GX2SetFetchShader(get_fetch(fk));
    const GX2PrimitiveMode mode = d.topology == 1 ? GX2_PRIMITIVE_MODE_TRIANGLE_STRIP : d.topology == 2 ? GX2_PRIMITIVE_MODE_TRIANGLE_FAN : GX2_PRIMITIVE_MODE_TRIANGLES;
    if (d.indexed) {
        if (index_u16) {
            void *w = (void *)ip;  // an outline's list is in the ring already
            if (!d.outline_color) {
                w = ring_alloc(draw_count * 2 + 64, 64);
                memcpy(w, ip, draw_count * 2);
            }
            DCFlushRange(w, draw_count * 2);
            GX2DrawIndexedEx(mode, draw_count, GX2_INDEX_TYPE_U16_LE, w, 0, 1);
        } else {
            u16 *w = (u16 *)ring_alloc(d.count * 2 + 64, 64);
            for (u32 i = 0; i < d.count; i++) w[i] = ip[i];
            DCFlushRange(w, d.count * 2);
            GX2DrawIndexedEx(mode, d.count, GX2_INDEX_TYPE_U16, w, 0, 1);
        }
    } else {
        GX2DrawEx(mode, d.count, d.first, 1);
    }
    lap(P_DRAW);
    st_draws++;
    return true;
}

// ---------------------------------------------------------------- draws shaded on the CPU
void gx2_flush_cpu_batch() {
    MBatch &mb = raster_mbatch();
    if (!mb.segs.empty()) {
        vtx_finish_jobs();
        const RTConfig &rt = mb.segs[0].ds->rt;
        metal_flush(rt, mb);
    }
    mb.clear();
}

void metal_flush(const RTConfig &rt, const MBatch &mb) {
    if (mb.segs.empty() || !rt.color_addr || !pptr(rt.color_addr) || (rt.color_fmt & 0x80)) return;
    // Textures first: a decode takes ring memory of its own, megabytes for a large one, and the batch's vertices mustn't already be lying in a
    // segment that the decodes go on to leave behind
    for (const MSeg &seg : mb.segs)
        if (!seg.ds->skip && !seg.ds->shadow_mode && !seg.ds->gas) decode_textures_first(*seg.ds);
    const u32 bytes = (u32)mb.cverts.size() * sizeof(OutVertex);
    auto *v = (u32 *)ring_alloc(bytes + 64, 64);
    memcpy(v, mb.cverts.data(), bytes);
    DCFlushRange(v, bytes);
    const u32 nidx = (u32)mb.idx.size();
    auto *ix = (u32 *)ring_alloc(nidx * 4 + 64, 64);
    for (u32 i = 0; i < nidx; i++) ix[i] = mb.idx[i] & 0x7fffffffu;
    DCFlushRange(ix, nidx * 4);
    static GX2FetchShader *fetch;
    if (!fetch) {
        FetchKey k{};
        static const u32 offs[7] = {0, 4 * 4, 8 * 4, 12 * 4, 22 * 4, 18 * 4, 16 * 4};
        for (u32 i = 0; i < 7; i++) k.s[k.count++] = float4_stream(i, 0, offs[i], false);
        fetch = get_fetch(k);
    }
    bind_targets(rt);
    for (const MSeg &seg : mb.segs)
        if (cur_depth && (seg.ds->depth_test || seg.ds->depth_write || seg.ds->stencil)) {
            take_depth_block(rt);
            break;
        }
    GX2SetVertexShader(vs_passthrough);
    GX2SetFetchShader(fetch);
    GX2SetAttribBuffer(0, bytes, sizeof(OutVertex), v);
    for (const MSeg &seg : mb.segs) {
        const DrawState &ds = *seg.ds;
        if (ds.skip || ds.shadow_mode || ds.gas) continue;
        lap_start();
        begin_draw_common(ds, find_ps(ds));
        static const V4 k_none[96] = {};
        set_vertex_uniforms(ds, k_none);
        GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, seg.count, GX2_INDEX_TYPE_U32, ix + seg.first, 0, 1);
        st_cpu_draws++;
    }
}

bool metal_active() { return true; }
bool metal_init() { return true; }  // GX2 starts on the GPU thread: wiiu::gfx_thread_init
const void *metal_vs_kernel(const ShaderProgram *, u32, const ShaderSetup &) { return nullptr; }
void metal_poll() {}
bool metal_gpu_made(u32, u32) { return false; }
void metal_note_gpu_output(u32, u32) {}
void metal_writeback_all() {}

static void mark_textures(u32 paddr, u32 size, bool definite) {
    for (auto &p : textures) {
        Tex &e = *p;
        if (e.addr < paddr + size && paddr < e.addr + e.bytes) {
            if (e.fill_pending) e.fill_pending = false;  // the fill gx2_memory_fill just let through: what it left is known
            else e.filled = false;
            if (definite) {
                e.dirty = true;
                const u32 lo = std::max(paddr, e.addr) - e.addr, hi = std::min(paddr + size, e.addr + e.bytes) - e.addr;
                for (u32 r = lo / e.row_bytes, last = std::min((hi - 1) / e.row_bytes, 127u); r <= last; r++) e.dirty_rows[r >> 5] |= 1u << (r & 31);
            } else e.maybe_dirty = true;
        }
    }
}

// CPU or DMA writes the game told us about. Nothing needs the CPU's cache flushed here: what the GPU reads in place is flushed at the draw, and on a
// real CPU flushing a whole heap, as some games ask for after every file, would take tens of milliseconds.
// What the game's texture copies and display transfers put into guest memory since the last VBlank (gpu/transfer.cpp): a top screen the game put together
// in its LCD buffer itself shows from there (show_lcd_from_memory). Fills don't count: a game may clear an LCD buffer that a transfer fills later.
struct MemoryWrite {
    u32 pa, size, seq;
};
MemoryWrite lcd_writes[16];
u32 lcd_write_seq;
void gx2_copied_to_memory(u32 paddr, u32 size) {
    if (size < 4096) return;
    lcd_write_seq++;
    lcd_writes[lcd_write_seq % 16] = {paddr, size, lcd_write_seq};
}

void metal_guest_written(u32 paddr, u32 size, bool definite) {
    st_flushes++;
    vtx_invalidate(paddr, size);
    if (!definite && size >= 1 << 20) {
        heap_flushes++;
        heap_flush_gap = st_flips - heap_flush_flip;
        heap_flush_flip = st_flips;
        return;
    }
    mark_textures(paddr, size, definite);
    if (definite)
        for (auto &s : surfaces)
            if (s->addr < paddr + size && paddr < s->addr + s->bytes()) s->guest_newer = true;
}

// ---------------------------------------------------------------- reading the GPU's work back to guest memory
namespace {
void write_back(Surface &s) {
    if (s.depth || !s.gpu_newer) return;
    st_readbacks++;
    static u32 notes;
    static u32 told;
    if (gxt() || (kMeasure && told++ < 8)) LOG("[gxt] read back %08x %ux%u fmt %u%s, %ux%u at %.2fx%.2f\n", s.addr, s.w, s.h, s.fmt, s.linear ? " linear" : "", s.sw, s.sh, s.sx, s.sy);
    if (notes < 4) wiiu::note("reading back surface %08x, %ux%u", s.addr, s.w, s.h);
    flush_surface(s);
    GX2Surface st;
    memset(&st, 0, sizeof st);
    st.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    st.width = s.sw;
    st.height = s.sh;
    st.depth = 1;
    st.mipLevels = 1;
    st.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    st.aa = GX2_AA_MODE1X;
    st.use = GX2_SURFACE_USE_TEXTURE;
    // A console's copy is in memory when GX2DrawDone returns. Cemu hands a copy's pixels over some draws later (its LatteSurfaceCopy.cpp), which
    // read here as whatever the memory held: a Mii's face came back blank. A copy to this tile mode it finishes at once, though not in the
    // queue's turn, so the queue is emptied first.
    st.tileMode = in_cemu ? GX2_TILE_MODE_LINEAR_SPECIAL : GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&st);
    st.image = memalign(st.alignment, st.imageSize);
    if (!st.image) return;  // still newer on the GPU: the next read back tries again
    s.gpu_newer = false;
    DCInvalidateRange(st.image, st.imageSize);
    if (in_cemu) draw_done();
    GX2CopySurface(&s.cb.surface, 0, 0, &st, 0, 0);
    restore_context();
    draw_done();
    if (notes < 4) wiiu::note("read back %u done", ++notes);
    DCInvalidateRange(st.image, st.imageSize);
    u8 *dst = pptr(s.addr);
    const u32 pf = cbuf_to_pixfmt(s.fmt);
    const u32 bpp = pixel_bytes(pf);
    if (dst) {
        // Azran's title and save screens read the whole screen back every frame, so the usual format doesn't go through a pixel at a time
        u16 column[1024];
        if (pf == 0)
            for (u32 x = 0; x < s.w; x++) column[x] = (u16)(x * s.sx);
        for (u32 y = 0; y < s.h; y++) {
            const u32 row = (u32)(y * s.sy) + s.row0;
            const u32 *src = (const u32 *)st.image + (u64)row * st.pitch;
            const u32 my = s.h - 1 - y;  // guest memory rows run top down
            if (s.linear) {
                u8 *line = dst + (size_t)my * s.w * bpp;
                for (u32 x = 0; x < s.w; x++) {
                    const u32 c = src[(u32)(x * s.sx)];
                    encode_pixel(pf, rgba(c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255), line + x * bpp);
                }
                continue;
            }
            if (pf == 0) {
                for (u32 x = 0; x < s.w; x++) *(u32 *)(dst + tiled_offset(x, my, s.w, 4)) = le32(src[column[x]]);
                continue;
            }
            for (u32 x = 0; x < s.w; x++) {
                const u32 c = src[(u32)(x * s.sx)];
                encode_pixel(pf, rgba(c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255), dst + tiled_offset(x, my, s.w, bpp));
            }
        }
        DCFlushRange(dst, s.bytes());
    }
    free(st.image);
}
}  // namespace

void metal_guest_read(u32 paddr, u32 size) {
    // Of the targets there, the one the GPU wrote last holds what the memory would: Luigi's Mansion: Dark Moon draws its bottom screen (240x320) and each half
    // of its top screen (256x416, then 128x416) at 0x18000000, and the top screen's texture copies to its LCD buffer read the bottom screen back, so the TV showed black.
    Surface *best = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->gpu_newer && s->addr < paddr + size && paddr < s->addr + s->bytes() && (!best || s->drawn > best->drawn)) best = s.get();
    if (best) write_back(*best);
}
void metal_guest_prewrite(u32, u32) {}

// the last transfer was the top screen's left eye, and a right eye has been seen straight after one (see right_eye_transfer)
static bool left_done, right_follows_left;

// Some games draw a right eye with 3D off, and that's half of each frame's work. Once a top-screen-sized transfer to the right eye has come straight after a left
// eye, the next right eye isn't drawn, up to its transfer. Where the right eye went says where its pass starts:
// - into the right LCD's framebuffer (New Super Mario Bros. 2, Animal Crossing): its pass follows the left eye's transfer at once (right_eye_transfer);
// - into a framebuffer no screen shows (the Laytons, which clear their targets, draw the scene again, and on the title and save screens read it back and make
//   mipmaps of it too): it starts with the first clear after the left eye, unless that's the bottom screen's, which those games draw first when there's no right eye.
static bool right_eye_hidden;

// a fill of a whole render target is a clear
bool gx2_memory_fill(u32 start, u32 end, u32 value, u32 control) {
    if (left_done && right_follows_left && right_eye_hidden) {
        const u32 size = end - start;
        if (size == 240 * 320 * 4 || size == 240 * 320 * 3) left_done = false;  // on to the bottom screen, so no right eye this time
        else gx2_skip_pass = gx2_skip_copies = true;
    }
    if (gx2_skip_pass) return true;
    const Fill f{start, end, value, control};
    bool any = false, other = false;
    // A target that lies wholly inside the fill is filled too, as its memory would be: Luigi's Mansion: Dark Moon keeps its top screen's target, its bottom
    // screen's and two linear pictures at 0x18000000, and clearing one meant clearing 416 KB in guest memory with the CPU, a few times a frame
    // (a quarter of the GPU thread's time on a console). Only a target the fill covers in part still needs guest memory filled.
    for (auto &s : surfaces)
        if (s->addr == start && end - start >= s->bytes() && end - start < s->bytes() + 64) {
            apply_fill(*s, f);
            any = true;
        } else if (s->addr < end && start < s->addr + s->bytes()) {
            if (fill_inside && s->addr >= start && s->addr + s->bytes() <= end) apply_fill(*s, f);
            else other = true;
        }
    // A target the fill covers only in part needs its memory filled only if something reads that memory: a texture there. Dark Moon clears its bottom
    // screen (300 KB at 0x18000000) inside its top screen's target, which it clears on the GPU before every use: that was 300 KB with the CPU a frame.
    if (any && other && fill_inside) {
        bool read = false;
        for (const auto &t : textures)
            if (t->addr < end && start < t->addr + t->bytes) read = true;
        if (!read) other = false;
    }
    // A texture the game fills whole with the same value every frame (Dark Moon: 256 KB of white at 0x183e8000), which nothing else wrote since: its memory
    // and its image already hold that, so the fill is done. Otherwise it's filled in guest memory, and noted, so the next one can be skipped.
    if (!any && fill_inside)
        for (auto &t : textures)
            if (t->addr == start && t->bytes == end - start) {
                if (t->filled && t->fill_value == value && !t->dirty && !t->maybe_dirty) {
                    if (gxt()) LOG("[gxt] fill %08x-%08x: the texture there holds it already\n", start, end);
                    return true;
                }
                t->filled = true;
                t->fill_pending = true;
                t->fill_value = value;
            }
    if (gxt()) LOG("[gxt] fill %08x-%08x (%u KB) value %08x control %08x: %s\n", start, end, (end - start) >> 10, value, control,
                   !any ? "guest memory (no target there)" : other ? "a target, and guest memory (others in part)" : "on the GPU");
    if (!any && pending_fills.size() < 16 && start >= mem::VRAM_PADDR) pending_fills.push_back(f);
    if (any) mark_textures(start, end - start, true);
    return any && !other;  // Nothing reads a render target from guest memory without asking, and filling it there costs real time: New Super Mario Bros. 2 does 2.5 MB a frame
}

// ---------------------------------------------------------------- presenting
namespace {
struct Screen {
    GX2Texture *tex = nullptr;
    bool flip = false;
    float v_lo = 0, v_hi = 1;  // the part of the surface it shows: some games draw both screens in one surface
    float u_hi = 1;            // and some draw wider than the screen and crop the rest
};
GX2ColorBuffer *tv_cb, *drc_cb;
float *quad;
GX2FetchShader *blit_fetch;
GX2Sampler blit_sampler;
volatile bool vblank_pending;

// a quad on the current colour buffer showing a screen's surface: rotated, as the LCDs are mounted sideways
void draw_screen(const Screen &s, u32 x, u32 y, u32 w, u32 h) {
    GX2SetViewport((float)x, (float)y, (float)w, (float)h, 0, 1);
    GX2SetScissor(x, y, w, h);
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    // positions, then texture coordinates: u across the surface runs down the screen, v runs right to left
    const float v0 = s.flip ? s.v_lo : s.v_hi, v1 = s.flip ? s.v_hi : s.v_lo;
    const float data[16] = {-1, 1, s.u_hi, v0, 1, 1, s.u_hi, v1, -1, -1, 0, v0, 1, -1, 0, v1};
    memcpy(q, data, sizeof data);
    DCFlushRange(q, sizeof data);
    GX2SetAttribBuffer(0, sizeof data, 16, q);
    GX2SetPixelTexture(s.tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
}

void begin_blit() {
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetVertexShader(vs_blit);
    GX2SetPixelShader(ps_blit);
    GX2SetFetchShader(blit_fetch);
    auto *u = (u32 *)ring_alloc(256, 256);
    const float tint[8] = {1, 1, 1, 1, 0, 0, 0, 0};
    for (u32 i = 0; i < 8; i++) u[i] = swap_float(tint[i]);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, 32);
    GX2SetPixelUniformBlock(0, 32, u);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetStencilMask(0, 0, 0, 0, 0, 0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0);
}

// A texture that changed: the tile rows that did (rows, or all of them), as their bytes are in the game's memory, to the ring, and a draw of
// its format's decoder (gen_etc1_fs, gen_untile_fs) over those rows of the texture's own image, in order with the draws that read it. The CPU decoder took 55 ms for NSMB2's 1 MB
// atlas, which the game rewrites every few frames in some areas, at a quarter of the frame rate. It sets a target and state of its own, so a draw
// has to get here before it sets up anything: decode_textures_first.
u64 st_gpu_decodes;
void gpu_decode(Tex &e, const u32 *rows) {
    GX2Surface &s = e.stage.surface;
    const u8 *src = pptr(e.addr);
    const u32 th = e.h / 8, row_bytes = s.width * 4, stride = s.pitch * 4, per_tile = stage_rows(e.fmt);
    if (cur_color) flush_surface(*cur_color);
    GX2SetColorBuffer(&e.cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetPixelShader(ps_decode[e.fmt]);
    auto *u = (u32 *)ring_alloc(256, 256);  // the texture's size, in the blit block's tint: the shader may not ask the texture
    const float dims[8] = {(float)e.w, (float)e.h, 1.0f / s.width, (float)s.width, 0, 0, 0, 0};
    for (u32 i = 0; i < 8; i++) u[i] = swap_float(dims[i]);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, 32);
    GX2SetPixelUniformBlock(0, 32, u);
    GX2SetViewport(0, 0, (float)e.w, (float)e.h, 0, 1);
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    const float w = (float)e.w, h = (float)e.h;
    const float data[16] = {-1, 1, 0, 0, 1, 1, w, 0, -1, -1, 0, h, 1, -1, w, h};  // clip +1 is the image's first row: vUv counts pixels from it
    memcpy(q, data, sizeof data);
    DCFlushRange(q, sizeof data);
    GX2SetAttribBuffer(0, sizeof data, 16, q);
    GX2SetPixelSampler(&lut_sampler, 0);
    for (u32 ty0 = 0; ty0 < th;) {
        if (rows && !(rows[ty0 >> 5] >> (ty0 & 31) & 1)) {
            ty0++;
            continue;
        }
        u32 ty1 = ty0 + 1;
        while (ty1 < th && (!rows || (rows[ty1 >> 5] >> (ty1 & 31) & 1))) ty1++;
        const u32 n_rows = (ty1 - ty0) * per_tile, bytes = n_rows * stride;
        u8 *buf = (u8 *)ring_alloc(bytes, 256);
        for (u32 r = 0; r < n_rows; r++) memcpy(buf + r * stride, src + (ty0 * per_tile + r) * row_bytes, row_bytes);
        DCFlushRange(buf, bytes);
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, buf, bytes);
        s.image = buf - ty0 * per_tile * stride;  // where its first row would be: the shader reads these rows by their place in the whole texture
        GX2SetPixelTexture(&e.stage, 0);
        // memory rows top down land bottom up
        GX2SetScissor(0, e.h - ty1 * 8, e.w, (ty1 - ty0) * 8);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
        st_upload_bytes += (ty1 - ty0) * e.row_bytes;
        ty0 = ty1;
    }
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE), e.t.surface.image, e.t.surface.imageSize);
    state_lost = true;
    st_gpu_decodes++;
}

// Debug aid: with a file named "dump" in the data folder, writes what the TV and GamePad show as images, every 2 seconds
bool dump_enabled;
std::vector<u8> surface_ppm(GX2Surface &src, u32 step) {
    std::vector<u8> out;
    GX2Surface st;
    memset(&st, 0, sizeof st);
    st.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    st.width = src.width;
    st.height = src.height;
    st.depth = 1;
    st.mipLevels = 1;
    st.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    st.aa = GX2_AA_MODE1X;
    st.use = GX2_SURFACE_USE_TEXTURE;
    st.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&st);
    st.image = memalign(st.alignment, st.imageSize);
    if (!st.image) return out;
    memset(st.image, 0, st.imageSize);
    DCFlushRange(st.image, st.imageSize);
    GX2CopySurface(&src, 0, 0, &st, 0, 0);
    GX2DrawDone();
    DCInvalidateRange(st.image, st.imageSize);
    const u32 w = st.width / step, h = st.height / step;
    char head[32];
    const int head_len = snprintf(head, sizeof head, "P6\n%u %u\n255\n", w, h);
    out.resize(head_len + (size_t)w * h * 3);
    memcpy(out.data(), head, head_len);
    u8 *to = out.data() + head_len;
    for (u32 y = 0; y < h; y++) {
        const u32 *px = (const u32 *)st.image + (u64)(y * step) * st.pitch;
        for (u32 x = 0; x < w; x++, to += 3) {
            const u32 c = px[x * step];
            to[0] = c >> 24;
            to[1] = c >> 16;
            to[2] = c >> 8;
        }
    }
    free(st.image);
    return out;
}

void dump_surface(GX2Surface &src, const char *path, u32 step) {
    const std::vector<u8> ppm = surface_ppm(src, step);
    if (FILE *f = fopen(path, "wb")) {
        fwrite(ppm.data(), 1, ppm.size(), f);
        fclose(f);
    }
}

volatile u32 shot_step, shot_done, shot_addr;  // the PC asked for a picture of the TV, or of a target or texture at a guest address (wiiu::tv_shot)
std::vector<u8> shot_ppm;
volatile bool list_targets, forget_ps;  // the PC asked (gfx_list, gfx_set): done between frames, on this thread

bool dyn_test;  // a file named "dynres" in the data folder: changes level every 16 windows whatever the GPU does, for looking at both and at the changes between them

// Once a flip. The CPU waits at the end of a frame only when the GPU is two frames behind, so a wait means the GPU is what's holding the game back; a slow CPU never waits here.
// At full size, half a second of that goes to the lower level. There, nothing says how much room there is, so it tries full size again after a while, and each try that
// fails (back below within 20 seconds) makes the next wait twice as long, up to about 3 minutes.
void dyn_res_update() {
    if (dyn_force >= 0 || (top_720 && !dyn_720)) {  // at 1080p ("top1080"), or when asked: 720p targets fit, and mostly draw at full size
        if (dyn_force >= 0 && (u32)dyn_force != dyn_level) {
            if (kMeasure) LOG("[gx2] top screen held at %s size\n", dyn_force ? "the lower" : "full");
            dyn_level = (u32)dyn_force;
        }
        dyn_waited = 0;
        return;
    }
    static u32 flips, hold = 20, low_windows, since_raise = ~0u, test_windows;
    if (++flips < 16) return;
    const float wait_ms = OSTicksToMicroseconds((u64)dyn_waited) / 1000.0f / flips;
    flips = dyn_waited = 0;
    u32 level = dyn_level;
    if (dyn_test) {
        if (++test_windows % 16 == 0) level = !level;
    } else if (!level) {
        if (wait_ms > 3) {
            level = 1;
            low_windows = 0;
            if (since_raise < 40) hold = std::min(hold * 2, 320u);
            since_raise = ~0u;
        } else if (since_raise != ~0u && ++since_raise >= 40) {
            hold = 20;
            since_raise = ~0u;
        }
    } else if (wait_ms > 3) {
        low_windows = 0;
    } else if (++low_windows >= hold) {
        level = 0;
        since_raise = 0;
    }
    if (level != dyn_level) {
        LOG("[gx2] top screen to %s size, the CPU waited %.1f ms a frame for the GPU\n", level ? "the lower" : "full", wait_ms);
        dyn_level = level;
    }
}

// What goes to a screen is a copy. Aroma draws its notifications, and its plugins their overlays, onto the buffer it is handed on its way
// to the screen, and the buffers here are kept from frame to frame and only redrawn where and when the game draws: what was written beside
// the picture stayed for good, letters of a notification in the corner of both screens (four testers, 2026-11-12).
void to_screen(GX2ColorBuffer *from, GX2ScanTarget target) {
    static GX2ColorBuffer copies[2];
    GX2ColorBuffer &to = copies[target == GX2_SCAN_TARGET_DRC];
    if (to.surface.width != from->surface.width || to.surface.height != from->surface.height || to.surface.format != from->surface.format) {
        mem::host_free(to.surface.image);
        to = *from;
        to.surface.image = mem::host_alloc(to.surface.alignment, to.surface.imageSize);
        if (!to.surface.image) to.surface.width = 0;  // no memory for it: the buffer itself goes, as before, and it's asked for again next time
        else GX2InitColorBufferRegs(&to);
    }
    if (to.surface.image) GX2CopySurface(&from->surface, 0, 0, &to.surface, 0, 0);
    GX2CopyColorBufferToScanBuffer(to.surface.image ? &to : from, target);
}

void present() {
    const OSTime tp = prof_now();
    gt_mark(GT_PRESENT, 0);
    if (dump_enabled && st_flips % 120 == 0) {
        dump_surface(tv_cb->surface, (wiiu::data_dir() + "/tv.ppm").c_str(), 2);
        dump_surface(drc_cb->surface, (wiiu::data_dir() + "/drc.ppm").c_str(), 1);
    }
    if (shot_step) {
        GX2Surface *from = &tv_cb->surface;
        if (shot_addr) {
            from = nullptr;
            if (Surface *s = find_surface(shot_addr, false)) {
                flush_surface(*s);
                from = &s->cb.surface;
            } else {
                for (const auto &t : textures)
                    if (t->addr == shot_addr) from = &t->t.surface;
            }
        }
        shot_ppm.clear();
        if (from) shot_ppm = surface_ppm(*from, shot_step);
        shot_step = 0;
        shot_done = 1;
    }
    if (list_targets) {
        list_targets = false;
        for (const auto &s : surfaces)
            LOG("[target] %08x %ux%u fmt %u %s, %ux%u, %s, level %u\n", s->addr, s->w, s->h, s->fmt, s->depth ? "depth" : "colour", s->sw, s->sh, s->mem1 ? "MEM1" : "MEM2", s->dyn_level);
        LOG("[target] %u textures, %u KB\n", (u32)textures.size(), (u32)(tex_memory >> 10));
    }
    if (forget_ps) {
        forget_ps = false;
        ps_seen.clear();
        ps_last_ds = nullptr;
    }
    to_screen(tv_cb, GX2_SCAN_TARGET_TV);
    to_screen(drc_cb, GX2_SCAN_TARGET_DRC);
    restore_context();
    GX2SwapScanBuffers();
    GX2Flush();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    st_flips++;
    use_counter++;
    ring_next_segment();
    gt_flip(cur_color ? cur_color->addr : 0);
    dyn_res_update();
    vtx_cache_flip();
    prof_add(P_PRESENT, tp);
}

bool screen_fresh;
bool top_shown;  // a display transfer brought the top screen since the last VBlank
PresentRule present_rule;  // when a finished frame is shown (present_rule.h, gfx_pump)

void show_screen(int which, GX2Texture *tex, bool flip, float v_lo = 0, float v_hi = 1, float u_hi = 1) {
    if (which == 0) top_shown = true;
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, tex->surface.image, tex->surface.imageSize);
    GX2ColorBuffer *cb = which == 0 ? tv_cb : drc_cb;
    GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    Screen sc;
    sc.tex = tex;
    sc.flip = flip;
    sc.v_lo = v_lo;
    sc.v_hi = v_hi;
    sc.u_hi = u_hi;
    if (which == 0) {
        const u32 w = cb->surface.height * 5 / 3;
        draw_screen(sc, (cb->surface.width - w) / 2, 0, w, cb->surface.height);
    } else {
        draw_screen(sc, (cb->surface.width - 640) / 2, 0, 640, 480);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
    state_lost = true;
    screen_fresh = true;
    present_rule.drew(which);
}

// ---- the options menu: drawn over the bottom screen on the GamePad, with a small built-in font
constexpr char k_font_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%:.-/>abcdefghijklmnopqrstuvwxyz,'!?()&+_<";
const u8 k_font[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14}, {1, 1, 1, 1, 17, 17, 14}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 27, 17}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 6, 8, 16, 31}, {30, 1, 1, 14, 1, 1, 30}, {2, 6, 10, 18, 31, 2, 2},
    {31, 16, 30, 1, 1, 17, 14}, {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
    {25, 25, 2, 4, 8, 19, 19}, {0, 4, 4, 0, 4, 4, 0}, {0, 0, 0, 0, 0, 12, 12}, {0, 0, 0, 31, 0, 0, 0}, {1, 1, 2, 4, 8, 16, 16}, {8, 4, 2, 1, 2, 4, 8},
    {0, 0, 14, 1, 15, 17, 15}, {16, 16, 22, 25, 17, 17, 30}, {0, 0, 14, 16, 16, 17, 14}, {1, 1, 13, 19, 17, 17, 15}, {0, 0, 14, 17, 31, 16, 14},
    {6, 9, 8, 28, 8, 8, 8}, {0, 15, 17, 17, 15, 1, 14}, {16, 16, 22, 25, 17, 17, 17}, {4, 0, 12, 4, 4, 4, 14}, {2, 0, 6, 2, 2, 18, 12},
    {16, 16, 18, 20, 24, 20, 18}, {12, 4, 4, 4, 4, 4, 14}, {0, 0, 26, 21, 21, 17, 17}, {0, 0, 22, 25, 17, 17, 17}, {0, 0, 14, 17, 17, 17, 14},
    {0, 0, 30, 17, 30, 16, 16}, {0, 0, 13, 19, 15, 1, 1}, {0, 0, 22, 25, 16, 16, 16}, {0, 0, 14, 16, 14, 1, 30}, {8, 8, 28, 8, 8, 9, 6},
    {0, 0, 17, 17, 17, 19, 13}, {0, 0, 17, 17, 17, 10, 4}, {0, 0, 17, 17, 21, 21, 10}, {0, 0, 17, 10, 4, 10, 17}, {0, 0, 17, 17, 15, 1, 14},
    {0, 0, 31, 2, 4, 8, 31}, {0, 0, 0, 0, 12, 4, 8}, {12, 4, 8, 0, 0, 0, 0}, {4, 4, 4, 4, 4, 0, 4}, {14, 17, 1, 2, 4, 0, 4},
    {2, 4, 8, 8, 8, 4, 2}, {8, 4, 2, 2, 2, 4, 8}, {12, 18, 20, 8, 21, 18, 13}, {0, 4, 4, 31, 4, 4, 0}, {0, 0, 0, 0, 0, 0, 31},
    {2, 4, 8, 16, 8, 4, 2},
};
GX2Texture font_tex;

void make_font() {
    const u32 n = sizeof(k_font) / sizeof(k_font[0]);
    memset(&font_tex, 0, sizeof font_tex);
    GX2Surface &f = font_tex.surface;
    f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    f.width = n * 6;
    f.height = 8;
    f.depth = 1;
    f.mipLevels = 1;
    f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    f.aa = GX2_AA_MODE1X;
    f.use = GX2_SURFACE_USE_TEXTURE;
    f.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    font_tex.viewNumMips = font_tex.viewNumSlices = 1;
    font_tex.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&f);
    GX2InitTextureRegs(&font_tex);
    f.image = memalign(f.alignment, f.imageSize);
    memset(f.image, 0, f.imageSize);
    for (u32 g2 = 0; g2 < n; g2++)
        for (u32 y = 0; y < 7; y++)
            for (u32 x = 0; x < 5; x++)
                if (k_font[g2][y] & (16 >> x)) ((u32 *)f.image)[(y + 0) * f.pitch + g2 * 6 + x] = 0xffffffffu;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize);
}

void set_blit_colors(float r, float g2, float b, float add_r, float add_g, float add_b) {
    auto *u = (u32 *)ring_alloc(256, 256);
    const float v[8] = {r, g2, b, 1, add_r, add_g, add_b, add_r + add_g + add_b > 0 ? 1.0f : 0.0f};
    for (u32 i = 0; i < 8; i++) u[i] = swap_float(v[i]);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, 32);
    GX2SetPixelUniformBlock(0, 32, u);
}

// a rectangle in pixels on a buffer w x h, mapped onto [u0, u1] x [v0, v1] of the bound texture
void draw_rect(u32 w, u32 h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const float px0 = x0 / w * 2 - 1, px1 = x1 / w * 2 - 1, py0 = 1 - y0 / h * 2, py1 = 1 - y1 / h * 2;
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    const float data[16] = {px0, py0, u0, v0, px1, py0, u1, v0, px0, py1, u0, v1, px1, py1, u1, v1};
    memcpy(q, data, sizeof data);
    DCFlushRange(q, sizeof data);
    GX2SetAttribBuffer(0, sizeof data, 16, q);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
}

void draw_text(u32 w, u32 h, const char *text, float x, float y, float scale) {
    const u32 n = sizeof(k_font) / sizeof(k_font[0]);
    const float cu = 6.0f / (n * 6);
    for (; *text; text++, x += 6 * scale) {
        const char *hit = strchr(k_font_chars, *text);
        if (!hit || *text == ' ') continue;
        const u32 g2 = (u32)(hit - k_font_chars);
        draw_rect(w, h, x, y, x + 5 * scale, y + 7 * scale, g2 * cu, 0, g2 * cu + 5.0f / (n * 6), 7.0f / 8);
    }
}
}  // namespace

void metal_display_transfer(u32, u32, u32, u32, u32) {}

// a display transfer from a render target to a framebuffer is a frame finished: no need for the pixels in guest memory
// the last frame sent to each screen, and the last to a buffer no screen shows, in case the game reads it back (for a photo, say)
struct LastTransfer {
    Surface *src = nullptr;
    u32 in_paddr = 0, out_paddr = 0, in_dims = 0, out_dims = 0, flags = 0, out_size = 0;
} last_transfer[3];

// A transfer between two surfaces the game draws at screen size stays on the GPU, at their higher resolution. Paper Mario draws its 3D at
// double width, scales it down into a buffer, and copies that into the target it draws the 2D on. Through guest memory that would be a
// readback every frame at the original resolution, and Cemu doesn't read the GPU's work back at all.
static bool gpu_copy_transfer(u32 in_paddr, u32 out_paddr, u32 in_dims, u32 out_dims, u32 flags) {
    gt_mark(GT_TRANSFER, out_paddr);
    const u32 in_w = in_dims & 0xffff, in_h = in_dims >> 16, out_w0 = out_dims & 0xffff, out_h0 = out_dims >> 16;
    const u32 scaling = (flags >> 24) & 3;
    const u32 out_w = out_w0 >> (scaling != 0), out_h = out_h0 >> (scaling == 2);
    const bool flip = flags & 1, crop = flags & 4;
    float sx, sy;
    rt_scale(out_w, out_h, sx, sy);
    if ((flip && crop) || in_w < out_w0 || in_h < out_h0) return false;
    Surface *src = nullptr;
    for (auto &x : surfaces)
        if (!x->depth && x->addr == in_paddr && x->w == in_w && x->h == in_h && (x->sy > 1 || x->gpu_chain) && !x->guest_newer) src = x.get();
    if (!src || (sy <= 1 && !src->gpu_chain)) return false;
    Surface *dst = get_surface(out_paddr, out_w, out_h, cbuf_to_pixfmt((flags >> 12) & 7), false, src->gpu_chain ? src : nullptr);
    if (dst == src) return false;
    dst->gpu_chain |= src->gpu_chain;
    // Neither bit 1 (linear in) nor bit 5 (left tiled): the picture lands in memory row by row, for whatever reads its bytes. Paper Mario's goes on to another
    // transfer here on the GPU; Luigi's Mansion: Dark Moon's top screen halves go to texture copies into its LCD buffer, which read them back, and tiled they came out black.
    dst->linear = !(flags & 0x22);
    take_dyn_level(*dst);
    static u32 notes;
    if (gxt()) LOG("[gxt] gpu copy %08x -> %08x, %08x %08x flags %08x\n", in_paddr, out_paddr, in_dims, out_dims, flags);
    if (notes++ < 2) wiiu::note("copying surface %08x to %08x on the GPU", in_paddr, out_paddr);
    st_gpu_copies++;
    gx2_flush_cpu_batch();
    flush_surface(*src);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, src->tex.surface.image, src->tex.surface.imageSize);
    GX2SetColorBuffer(&dst->cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, (float)dst->row0, (float)dst->sw, (float)(dst->sh - dst->row0), 0, 1);
    GX2SetScissor(0, dst->row0, dst->sw, dst->sh - dst->row0);
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    // Memory rows run top down, surface rows bottom up, so the rows it reads end at the top of the texture. Clip space +1 is a surface's lowest row.
    const float u_hi = (float)out_w0 / in_w, v_low = (float)(src->tex_h - out_h0) / src->tex_h;
    const float v_first = flip ? 1 : v_low, v_last = flip ? v_low : 1;
    const float data[16] = {-1, 1, 0, v_first, 1, 1, u_hi, v_first, -1, -1, 0, v_last, 1, -1, u_hi, v_last};
    memcpy(q, data, sizeof data);
    DCFlushRange(q, sizeof data);
    GX2SetAttribBuffer(0, sizeof data, 16, q);
    GX2SetPixelTexture(&src->tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
    state_lost = true;
    dst->gpu_newer = true;
    dst->drawn = ++gpu_writes;
    dst->guest_newer = false;
    dst->written_since_flush = true;
    return true;
}

// The top screen's LCD buffers lately (show_lcd_from_memory notes them at each VBlank), and how their rows are laid out
u32 recent_top[4], recent_top_stride, recent_top_bpp, recent_top_fmt;

// Luigi's Mansion: Dark Moon puts its top screen together with the copy engine, from linear pictures its transfers make (gpu_copy_transfer): each third
// of the screen is drawn, transferred to a linear picture and copied a row at a time, a few columns over or into its columns of the LCD buffer. Through guest
// memory that was read backs every frame, each waiting for the GPU, and in Cemu, which reads nothing back, a black screen. A copy of whole rows out of a linear
// surface is drawn on the GPU instead, into a linear surface at the destination: one already there whose rows it fits, or the top screen's LCD buffer (whole
// rows from at or before the copy's first, down to the LCD's last), or a new one. The TV shows the buffer from there (show_lcd_from_memory).
static bool linear_copy_on_gpu(u32 in_paddr, u32 out_paddr, u32 size, u32 in_width_gap, u32 out_width_gap) {
    if (!linear_on_gpu) return false;
    gt_mark(GT_COPY, out_paddr);
    const u32 in_w = (in_width_gap & 0xffff) * 16, in_gap = (in_width_gap >> 16) * 16;
    const u32 out_w = (out_width_gap & 0xffff) * 16, out_gap = (out_width_gap >> 16) * 16;
    if (!in_w || in_w != out_w || size % in_w) return false;
    Surface *src = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->linear && s->addr <= in_paddr && in_paddr < s->addr + s->bytes() && (!src || s->drawn > src->drawn)) src = s.get();
    if (!src) return false;
    const u32 bpp = pixel_bytes(cbuf_to_pixfmt(src->fmt)), src_stride = src->w * bpp, rows = size / in_w;
    const u32 in_off = in_paddr - src->addr;
    if (in_w + in_gap != src_stride && !(in_gap == 0 && in_w == src_stride)) return false;
    if (in_w % bpp || (in_off % src_stride) % bpp) return false;
    const u32 src_row0 = in_off / src_stride, src_col0 = (in_off % src_stride) / bpp, cols = in_w / bpp;
    if (src_row0 + rows > src->h || src_col0 + cols > src->w) return false;
    const u32 out_stride = out_gap ? out_w + out_gap : out_w;
    if (out_stride % bpp) return false;
    // where it lands
    Surface *dst = nullptr;
    u32 base = out_paddr, height = rows;
    u32 lcd = 0;
    if (recent_top_stride == out_stride && recent_top_bpp == bpp)
        for (u32 pa : recent_top)
            if (pa && out_paddr < pa + 400 * out_stride && out_paddr + rows * out_stride > pa && pa - out_paddr <= 33 * out_stride) lcd = pa;
    if (lcd) {
        const u32 back = out_paddr < lcd ? (lcd - out_paddr + out_stride - 1) / out_stride : 0;
        base = lcd - back * out_stride;
        height = back + 400;
    } else {
        for (auto &s : surfaces)
            if (!s->depth && s->linear && s.get() != src && s->w * bpp == out_stride && s->addr <= out_paddr && out_paddr + (rows - 1) * out_stride < s->addr + s->bytes() &&
                (!dst || s->drawn > dst->drawn))
                dst = s.get();
        if (dst) {
            base = dst->addr;
            height = dst->h;
        }
    }
    const u32 off = out_paddr - base, first_row = off / out_stride;
    if ((off % out_stride) % bpp || first_row + rows > height) return false;
    const u32 first_col = (off % out_stride) / bpp;
    if (!dst) dst = get_surface(base, out_stride / bpp, height, lcd ? recent_top_fmt : src->fmt, false, src);
    if (dst == src || first_col + cols > dst->w) return false;
    take_dyn_level(*dst);
    static u32 notes;
    if (kMeasure && notes++ < 4) wiiu::note("copying rows on the GPU: %08x into %08x, %u columns from %u", in_paddr, base, cols, first_col);
    st_gpu_copies++;
    gx2_flush_cpu_batch();
    if (dst->first_use) {
        dst->first_use = false;
        GX2ClearColor(&dst->cb, 0, 0, 0, 1);
        restore_context();
    }
    flush_surface(*src);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, src->tex.surface.image, src->tex.surface.imageSize);
    GX2SetColorBuffer(&dst->cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    // memory row 0 at the top of a surface's rows, as gpu_copy_transfer draws them
    const u32 x0 = (u32)(first_col * dst->sx), y0 = dst->row0 + (u32)(first_row * dst->sy), w = (u32)(cols * dst->sx), h = (u32)(rows * dst->sy);
    GX2SetViewport((float)x0, (float)y0, (float)w, (float)h, 0, 1);
    GX2SetScissor(x0, y0, w, h);
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    const float th = (float)src->tex_h, u0 = (float)src_col0 / src->w, u1 = (float)(src_col0 + cols) / src->w;
    const float v0 = (th - src->h + src_row0) / th, v1 = (th - src->h + src_row0 + rows) / th;
    const float data[16] = {-1, 1, u0, v0, 1, 1, u1, v0, -1, -1, u0, v1, 1, -1, u1, v1};
    memcpy(q, data, sizeof data);
    DCFlushRange(q, sizeof data);
    GX2SetAttribBuffer(0, sizeof data, 16, q);
    GX2SetPixelTexture(&src->tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
    state_lost = true;
    dst->gpu_newer = true;
    dst->guest_newer = false;
    dst->written_since_flush = true;
    dst->linear = true;
    dst->gpu_chain = true;
    dst->drawn = ++gpu_writes;
    return true;
}

// Metroid copies its two 240x400 views into 256x512 textures six times a frame with the copy engine, and makes mipmaps of two of them with transfers. Through guest
// memory each was a read back, which waits for the GPU, then a 512 KB upload. A copy of a whole screen target, a row of tiles at a time into a wider power-of-two
// texture, is drawn on the GPU into a surface at that address instead, at the target's scale, which the game then samples (get_texture) or makes mipmaps of
// (gpu_copy_transfer). Metroid draws its final picture from these copies, so at 1:1 the whole screen came out at the 3DS's resolution.
bool gx2_texture_copy(u32 in_paddr, u32 out_paddr, u32 size, u32 in_width_gap, u32 out_width_gap) {
    if (linear_copy_on_gpu(in_paddr, out_paddr, size, in_width_gap, out_width_gap)) return true;
    Surface *src = find_surface(in_paddr, false, [&](const Surface &t) { return t.gpu_newer && !t.guest_newer && t.sy > 1 && size == t.bytes(); });
    if (!src) return false;
    const u32 bpp = src->bytes() / (src->w * src->h), tile_row = src->w * 8 * bpp;
    const u32 in_w = (in_width_gap & 0xffff) * 16, in_gap = (in_width_gap >> 16) * 16;
    const u32 out_w = (out_width_gap & 0xffff) * 16, out_gap = (out_width_gap >> 16) * 16;
    if ((in_gap && in_w != tile_row) || out_w != tile_row || !out_gap || (out_w + out_gap) % (8 * bpp)) return false;
    const u32 w = (out_w + out_gap) / (8 * bpp);
    u32 h = src->h;
    while (h & (h - 1)) h += h & -h;
    if ((w & (w - 1)) || w > 1024 || h > 1024) return false;
    Surface *dst = get_surface(out_paddr, w, h, src->fmt, false, src);
    if (dst == src) return false;
    take_dyn_level(*dst);
    if (gxt()) LOG("[gxt] texture copy on the GPU %08x -> %08x as %ux%u\n", in_paddr, out_paddr, w, h);
    static u32 notes;
    if (notes++ < 2) wiiu::note("copying surface %08x into texture %08x on the GPU", in_paddr, out_paddr);
    st_gpu_copies++;
    gx2_flush_cpu_batch();
    if (dst->first_use) {  // what the copy leaves alone (the columns past the target's) starts clear, not as whatever the memory held: a console's isn't zeroed
        dst->first_use = false;
        GX2ClearColor(&dst->cb, 0, 0, 0, 0);
        restore_context();
    }
    dst->gpu_chain = true;
    flush_surface(*src);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, src->tex.surface.image, src->tex.surface.imageSize);
    GX2SetColorBuffer(&dst->cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    // the copied rows are the texture's first in memory, so its top rows, as a surface (see gpu_copy_transfer): src->w columns of them
    const u32 rows = (u32)(src->h * dst->sy), cols = (u32)(src->w * dst->sx);
    GX2SetViewport(0, (float)(dst->sh - rows), (float)cols, (float)rows, 0, 1);
    GX2SetScissor(0, dst->sh - rows, cols, rows);
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    const float v_low = (float)(src->tex_h - src->h) / src->tex_h;
    const float data[16] = {-1, 1, 0, v_low, 1, 1, 1, v_low, -1, -1, 0, 1, 1, -1, 1, 1};
    memcpy(q, data, sizeof data);
    DCFlushRange(q, sizeof data);
    GX2SetAttribBuffer(0, sizeof data, 16, q);
    GX2SetPixelTexture(&src->tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
    state_lost = true;
    dst->gpu_newer = true;
    dst->drawn = ++gpu_writes;
    dst->guest_newer = false;
    dst->written_since_flush = true;
    return true;
}

// The first draw into a target starts from what the game left in its memory, as on a 3DS: Tomodachi composes each Mii's face over a transparent texture it never
// clears with the GPU. Cemu's memory starts zeroed, so it looked right there; a console's holds whatever was freed last, and faces came out speckled with it.
// A screen-sized target is drawn larger than its memory and every game paints it over anyway, so only 1:1 targets are started this way.
static void upload_target(Surface &s) {
    const u8 *src = pptr(s.addr);
    if (s.depth || !src || s.sy > 1) return;
    GX2Surface st;
    memset(&st, 0, sizeof st);
    st.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    st.width = s.sw;
    st.height = s.sh;
    st.depth = 1;
    st.mipLevels = 1;
    st.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    st.aa = GX2_AA_MODE1X;
    st.use = GX2_SURFACE_USE_TEXTURE;
    st.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&st);
    st.image = ring_alloc(st.imageSize, st.alignment);  // the GPU is done with it by the time the ring comes round
    // decode_level puts the last memory row in its row 0, which write_back reads from the target's row row0 (the rows above are a texture's padding)
    memset(st.image, 0, (size_t)s.row0 * st.pitch * 4);
    decode_level(src, s.w, s.h, s.fmt, (u32 *)st.image + (size_t)s.row0 * st.pitch, st.pitch);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, st.image, st.imageSize);
    LOG("[gx2] target %08x %ux%u starts from guest memory, image %08x\n", s.addr, s.w, s.h, (u32)(uintptr_t)s.cb.surface.image);
    GX2CopySurface(&st, 0, 0, &s.cb.surface, 0, 0);  // the GPU's own copy, as write_back uses the other way: no draw state of ours to get wrong on a console
    restore_context();
    s.written_since_flush = true;
}

// Which screen shows these bytes, or -1, and how far into them the LCD starts: New Super Mario Bros. 2 transfers 416 rows and shows the
// last 400. Games alternate between buffers, so every buffer each screen has shown is remembered. Screen 2 is the top screen's right eye.
// Each screen's last few framebuffers, as a game draws into the ones not shown. When a transfer lands on addresses of more than one screen, the one shown there
// most recently wins: NSMB2's third top buffer, 18300000, is where its bottom screen was during start-up, so every third top picture went to the GamePad
// (the user, 2026-11-08: "the bottom screen flashing in and out with the top showing on it briefly").
// The picture's height rules out the other LCD whatever the addresses say: the top's are 400 rows (NSMB2's 416), the bottom's 320. A Link Between Worlds' second
// top buffer is 18300000 too, and its top kept flashing onto the GamePad on a console (2026-11-09).
static int screen_of(u32 paddr, u32 size, u32 rows, u32 &skip) {
    struct Seen {
        u32 pa, when;
    };
    static Seen seen[3][4];
    static u32 next[3], clock;
    clock++;
    for (int s = 0; s < 3; s++) {
        const kernel::FramebufferInfo f = kernel::gsp_framebuffer(s == 2 ? 0 : s);
        const u32 addr = s == 2 && f.addr_right != f.addr_left ? f.addr_right : s == 2 ? 0 : f.addr_left;
        const u32 pa = f.valid && addr ? mem::vaddr_to_paddr(addr) : 0;
        if (!pa) continue;
        Seen *e = nullptr;
        for (Seen &x : seen[s])
            if (x.pa == pa) e = &x;
        if (!e) e = &seen[s][next[s]++ % 4];
        *e = {pa, clock};
    }
    int found = -1;
    u32 best = 0;
    const bool bottom_rows = rows <= 320;
    // A buffer an LCD hasn't shown for a while is no screen's any more. Luigi's Mansion: Dark Moon shows "Preparing to start game" from VRAM at 0x18300000,
    // later draws each half of its top screen there and puts the halves together in its LCD buffers in FCRAM: remembered for good, the first half went to the
    // TV as the whole top screen, stretched to twice its height. Games that take turns between buffers show each of them every frame or two.
    constexpr u32 kForgetAfter = 240;  // transfers: a few seconds, at the most a game makes
    for (int s = 0; s < 3; s++) {
        if ((s == 1) != bottom_rows) continue;  // only the screens of the picture's height
        for (const Seen &x : seen[s])
            if (x.pa && clock - x.when <= kForgetAfter && x.pa >= paddr && x.pa - paddr < size && x.when >= best) {  // a tie goes to the later screen, as before
                found = s;
                best = x.when;
                skip = x.pa - paddr;
            }
    }
    return found;
}

bool gx2_skip_pass, gx2_skip_copies;

// New Super Mario Bros. 2 draws a right eye every frame though the slider says 3D is off, and the Wii U couldn't show it: half its draws.
// Once a right eye is seen right after a left one, the pass between those transfers isn't drawn. If something else turns up there, it stops.
// Returns true if this transfer needs nothing more.
static bool right_eye_transfer(int screen) {
    const bool was_skipping = gx2_skip_pass;
    gx2_skip_pass = gx2_skip_copies = false;
    if (screen == 2) {
        right_follows_left |= left_done;
        left_done = false;
        return true;
    }
    if (was_skipping) right_follows_left = false;
    left_done = screen == 0;
    gx2_skip_pass = left_done && right_follows_left && !right_eye_hidden;
    return false;
}

// A right eye drawn where no screen shows (the Laytons) is the first screen-sized picture after a left eye, transferred the way the left eye was. Copies in
// between don't count (Azran's mipmaps); it may go to either of two buffers (both Laytons), and Miracle Mask sends every other one to the right LCD. Other
// pictures come that way now and then: a top screen's next buffer before its LCD first shows it (Metroid: Samus Returns at start-up, which lost that picture
// and the copies after it, its objects vanishing for a moment on a console, 2026-11-09). So it takes 60 of them, no more than two left eyes apart, and a
// buffer an LCD shows ends it.
struct HiddenRightEye {
    enum Kind { OTHER, CANDIDATE, RIGHT_EYE, ON_A_SCREEN };
    bool after_left = false;  // a left eye has gone, and no screen-sized picture since
    u32 left_flags = 0, lefts_since = 0, runs = 0, dst[4] = {}, next = 0;

    bool remembered(u32 pa) const { return std::find(std::begin(dst), std::end(dst), pa) != std::end(dst); }

    // What a transfer is: a right eye to leave undrawn (the 60th of a run, or one whose pass was skipped already, as that drew nothing to show), one
    // more of a run, or a remembered buffer an LCD shows after all, which ends the run
    Kind see(int screen, bool top_sized, bool screen_sized, u32 flags, u32 pa, bool pass_skipped) {
        Kind kind = OTHER;
        if (after_left && top_sized && screen < 0 && flags == left_flags) {
            runs = lefts_since <= 2 ? runs + 1 : 1;  // frames without a right eye in between start the count again
            lefts_since = 0;
            if (!remembered(pa)) dst[next++ % 4] = pa;
            kind = runs >= 60 || pass_skipped ? RIGHT_EYE : CANDIDATE;
        } else if (screen >= 0 && remembered(pa)) {
            runs = 0;
            std::fill(std::begin(dst), std::end(dst), 0u);
            kind = ON_A_SCREEN;
        }
        if (screen == 0) {
            lefts_since++;
            left_flags = flags;
        }
        if (screen_sized) after_left = screen == 0;
        return kind;
    }
};
static HiddenRightEye hidden_right_eye;

// whether a destination no LCD has shown has come before, when its screen's count of pictures stood elsewhere
static bool passed_over(u32 pa, u32 count) {
    struct Spare {
        u32 pa, count;
    };
    static Spare spares[4];
    static u32 next;
    for (const Spare &x : spares)
        if (x.pa == pa) return x.count != count;
    spares[next++ % 4] = {pa, count};
    return false;
}

bool metal_defer_transfer(u32 in_paddr, u32 out_paddr, u32 in_dims, u32 out_dims, u32 flags, u32 out_size) {
    const u32 in_w = in_dims & 0xffff, in_h = in_dims >> 16, out_w = out_dims & 0xffff, out_h = out_dims >> 16;
    // the output size is given before the transfer's downscale (the flags' bits 24-25), so a 2x wide target has out_w 480 too
    const bool crop = flags & 4;
    const u32 rows = out_h >> ((flags >> 24 & 3) == 2);  // the picture's height, after a 2x2 downscale
    u32 skip_bytes = 0;
    const int known_screen = screen_of(out_paddr, out_size, rows, skip_bytes);
    if (gxt()) {
        const kernel::FramebufferInfo t = kernel::gsp_framebuffer(0), b = kernel::gsp_framebuffer(1);
        LOG("[gxt] transfer %08x -> %08x in %ux%u out %ux%u flags %08x screen %d (LCDs %08x %08x)\n", in_paddr, out_paddr, in_w, in_h, out_w, out_h, flags, known_screen,
            t.valid ? mem::vaddr_to_paddr(t.addr_left) : 0, b.valid ? mem::vaddr_to_paddr(b.addr_left) : 0);
    }
    const bool top_sized = in_w == 240 && in_h == 400 && out_w == 240 && out_h == 400;
    if (gx2_skip_copies && !top_sized && known_screen < 0) return true;  // the mipmaps of a right eye that wasn't drawn
    const bool screen_sized = top_sized || known_screen >= 0 || rows == 320;
    switch (hidden_right_eye.see(known_screen, top_sized, screen_sized, flags, out_paddr, gx2_skip_pass)) {
    case HiddenRightEye::RIGHT_EYE:  // nothing to draw or copy from its next clear to this transfer (gx2_memory_fill)
        if (!right_eye_hidden) {
            static u32 logged;
            if (logged++ < 20) LOG("[gx2] right eye goes where no screen shows: transfer %08x -> %08x\n", in_paddr, out_paddr);
        }
        right_eye_hidden = right_follows_left = true;
        gx2_skip_pass = gx2_skip_copies = left_done = false;
        return true;
    case HiddenRightEye::ON_A_SCREEN:
        if (right_eye_hidden) {
            static u32 logged;
            if (logged++ < 20) LOG("[gx2] right eye: %08x is a screen's buffer after all\n", out_paddr);
            right_eye_hidden = right_follows_left = false;
        }
        break;
    default:
        break;
    }
    if (known_screen == 2 && right_eye_hidden) {
        static u32 logged;
        if (logged++ < 20) LOG("[gx2] right eye goes to the right LCD: transfer %08x -> %08x\n", in_paddr, out_paddr);
        right_eye_hidden = false;
    }
    if (right_eye_transfer(known_screen)) return true;
    // New Super Mario Bros. 2's framebuffers are 256 wide, and the LCD shows 240 columns of them
    const u32 shown_w = out_w == 256 && (crop || known_screen >= 0) ? 240 : out_w;
    const u32 shown_h = known_screen == 0 ? 400 : known_screen == 1 ? 320 : out_h;
    const u32 skip_rows = skip_bytes / (out_size / rows);
    if ((crop ? in_w < out_w : in_w != out_w) || (shown_w != 240 && shown_w != 480) || (shown_h != 400 && shown_h != 320) || (in_h != out_h && !crop)) {
        if (gpu_copy_transfer(in_paddr, out_paddr, in_dims, out_dims, flags)) return true;
        // not a whole screen, as far as this knows: it goes through guest memory, which is slow if it happens every frame
        static u32 logged;
        if (logged++ < 12) LOG("[gx2] transfer %ux%u to %ux%u, flags %08x: not treated as a screen\n", in_w, in_h, out_w, out_h, flags);
        return false;
    }
    // the source may start partway into a surface: Layton draws both screens in one 240x400 surface, the bottom one in its last 320 rows
    Surface *s = nullptr;
    u32 first_row = 0;
    for (auto &x : surfaces) {
        if (x->depth || x->w != in_w || in_paddr < x->addr || in_paddr >= x->addr + x->bytes()) continue;
        const u32 row_bytes = x->bytes() / x->h, off = in_paddr - x->addr;
        if (off % row_bytes || off / row_bytes + out_h > x->h) continue;
        s = x.get();
        first_row = off / row_bytes;
    }
    if (!s) return false;
    gx2_flush_cpu_batch();
    flush_surface(*s);
    // memory rows run top down, surface rows bottom up; a surface read as a taller texture has its window at the top of that texture
    const float h = (float)s->tex_h;
    const int which = shown_h == 400 ? 0 : 1;
    // A picture for a buffer no LCD has shown is taken for the screen of its height, as a game fills a buffer before its LCD first shows it. One that
    // comes again after that screen has had a picture in a buffer it does show is no screen's. Miitopia sends its cleared target to a spare buffer
    // after each top screen, and the TV stayed black from start to finish (2026-11-11). It's kept in case the game reads it back (the Laytons do).
    static u32 shown[2];  // pictures sent to each screen's own buffers
    const bool spare = known_screen < 0 && passed_over(out_paddr, shown[which]);
    if (known_screen >= 0) shown[which]++;
    if (!spare) show_screen(which, &s->tex, flags & 1, (h - first_row - skip_rows - shown_h) / h, (h - first_row - skip_rows) / h, (float)shown_w / in_w);
    last_transfer[spare ? 2 : which] = {s, in_paddr, out_paddr, in_dims, out_dims, flags, out_size};
    return true;
}

void gx2_cpu_will_read(u32 paddr, u32 size) {
    for (const LastTransfer &t : last_transfer) {
        if (!t.src || !(t.out_paddr < paddr + size && paddr < t.out_paddr + t.out_size)) continue;
        t.src->gpu_newer = true;
        write_back(*t.src);
        const u8 *src = pptr(t.in_paddr);
        u8 *dst = pptr(t.out_paddr);
        if (src && dst) {
            transfer_convert(src, dst, t.in_dims, t.out_dims, t.flags);
            DCFlushRange(dst, t.out_size);
        }
    }
    metal_guest_read(paddr, size);
}

// A list's floats and indices are read in place in guest memory, and a real GPU only gets to them some time after they're queued. The game takes the list's
// interrupt to mean the GPU has finished with them, and may rewrite them at once, so a draw came out with the next frame's data: a Mii's textures on the wrong
// part, objects drawn over what's in front of them. Cemu takes commands as they're issued, so it never shows there. A 3DS's GPU has finished by then too.
// Waiting for the GPU at each list would mean the CPU and GPU never work at the same time, which on a console costs a quarter or more of each frame. So gx2_draw copies
// what the GPU reads, in every game, and the only wait is ring_next_segment's, which stops the CPU getting more than a couple of frames ahead.
//
// The game also takes it to mean that what the list drew is in memory. A picture it drew to pick up with its own code is put there first: Miitopia
// draws a Mii's eyes and mouth into a scratch buffer, shrinks that twice, and copies the three sizes into the face's texture itself, which found
// only what the memory held before, and the face was bare (2026-11-11). Not a target whose last picture the scene sampled where it lay: that is a
// texture the game draws to (Tomodachi Life's faces), and reading it back each time would only hold the GPU up.
void gx2_finish_list() {
    for (Surface *s : read_targets) {
        s->in_list = false;
        if (s->was_sampled) continue;
        write_back(*s);
        mark_textures(s->addr, s->bytes(), true);  // other textures in that memory are whatever it holds now
    }
    read_targets.clear();
    GX2Flush();
}

bool get_display_image_for(u32, u32, u32, const u32 **, int *, int *) { return false; }

}  // namespace gpu

namespace wiiu {
using namespace gpu;

static std::atomic<bool> gfx_is_ready;

// nothing draws into WHB's depth buffers: their fast memory goes to the game's render targets
static void free_whb_depth() {
    if (!GfxHeapFreeMEM1) return;
    for (GX2DepthBuffer *d : {WHBGfxGetTVDepthBuffer(), WHBGfxGetDRCDepthBuffer()}) {
        if (d->surface.image) GfxHeapFreeMEM1(d->surface.image);
        d->surface.image = nullptr;
    }
}

// The TV's colour buffer only gets one blit per present, so once fast memory has been found full it gives its 8 MB to what's drawn thousands of times (relieve_mem1).
// WHB keeps no pointer to it, as its depth buffers: it only frees what isn't null, and allocates a new one every time the foreground comes back.
static void tv_buffer_to_mem2() {
    static GX2ColorBuffer in_mem2;
    if (!mem1_relief || !GfxHeapFreeMEM1) return;
    GX2ColorBuffer *whb = WHBGfxGetTVColourBuffer();
    if (!in_mem2.surface.image) {
        void *p = memalign(whb->surface.alignment, whb->surface.imageSize);
        if (!p) return;
        DCFlushRange(p, whb->surface.imageSize);
        in_mem2 = *whb;
        in_mem2.surface.image = p;
    }
    if (whb->surface.image) GfxHeapFreeMEM1(whb->surface.image);
    whb->surface.image = nullptr;
    tv_cb = &in_mem2;
}

// The first time a screen's render target finds fast memory full: the TV's buffer moves to MEM2, and the screens' depth buffers go into one shared block, those that have
// fast memory now giving theirs back (share_depth_block). Their contents are lost, so each starts from a clear; the game clears them every frame anyway.
static void relieve_mem1() {
    mem1_relief = true;
    LOG("[gx2] fast memory is full: the TV's buffer goes to MEM2, and the screens' depth buffers share one block\n");
    GX2DrawDone();  // nothing the GPU still has queued may use what moves
    tv_buffer_to_mem2();
    depth_owner = nullptr;
    for (auto &s : surfaces) {
        GX2Surface &f = s->db.surface;
        // only screens' targets have fast memory; none means its memory is being made (gfx_reacquire), so it goes where that puts it
        if (!s->depth || !s->mem1 || s->shared_depth || !f.image) continue;
        GfxHeapFreeMEM1(f.image);
        if (share_depth_block(f)) s->shared_depth = s->clear_pending = true;
        else f.image = alloc_surface_mem(f.imageSize, f.alignment, s->mem1, false);
        if (cur_depth == s.get()) GX2SetDepthBuffer(&s->db);
    }
}

void gfx_thread_init() {
    int_floats.fill();
    WHBGfxInit();
    free_whb_depth();
    ctx = (GX2ContextState *)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    GX2SetupContextStateEx(ctx, TRUE);
    GX2SetContextState(ctx);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);
    GX2SetSwapInterval(1);
    ring = (u8 *)memalign(0x1000, RING_SIZE);
    if (!ring) FATAL("out of memory for the GPU ring\n");
    junk(ring, RING_SIZE, 4);
    ring_off = 0;
    ring_end = RING_SIZE / 3;
    tv_cb = WHBGfxGetTVColourBuffer();
    drc_cb = WHBGfxGetDRCColourBuffer();
    tv_buffer_to_mem2();
    load_shader_tables();
    shader_compile_init();  // what the set lacks, the console compiles as it goes
    vs_ubo_loc = vs_passthrough->uniformBlockCount ? vs_passthrough->uniformBlocks[0].offset : 0;
    ps_ubo_loc = ps_blit->uniformBlockCount ? ps_blit->uniformBlocks[0].offset : 0;
    for (u32 i = 0; i < vs_passthrough->uniformBlockCount; i++) LOG("[gx2] vs block %s at %u size %u\n", vs_passthrough->uniformBlocks[i].name, vs_passthrough->uniformBlocks[i].offset, vs_passthrough->uniformBlocks[i].size);
    for (u32 i = 0; i < vs_passthrough->attribVarCount; i++) LOG("[gx2] vs attrib %s at %u\n", vs_passthrough->attribVars[i].name, vs_passthrough->attribVars[i].location);
    for (u32 i = 0; i < ps_blit->uniformBlockCount; i++) LOG("[gx2] ps block %s at %u size %u\n", ps_blit->uniformBlocks[i].name, ps_blit->uniformBlocks[i].offset, ps_blit->uniformBlocks[i].size);
    for (u32 i = 0; i < ps_blit->samplerVarCount; i++) LOG("[gx2] ps sampler %s at %u\n", ps_blit->samplerVars[i].name, ps_blit->samplerVars[i].location);
    make_font();
    GX2InitSampler(&blit_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSampler(&lut_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    {
        FetchKey k{};
        k.s[k.count++] = float4_stream(0, 0, 0, false);
        blit_fetch = get_fetch(k);
    }
    memset(&black_tex, 0, sizeof black_tex);
    GX2Surface &f = black_tex.surface;
    f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    f.width = f.height = f.depth = f.mipLevels = 1;
    f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    f.aa = GX2_AA_MODE1X;
    f.use = GX2_SURFACE_USE_TEXTURE;
    f.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    black_tex.viewNumMips = black_tex.viewNumSlices = 1;
    black_tex.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&f);
    GX2InitTextureRegs(&black_tex);
    f.image = memalign(f.alignment, f.imageSize);
    memset(f.image, 0, f.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f.image, f.imageSize);
    GX2ClearColor(tv_cb, 0, 0, 0, 1);
    GX2ClearColor(drc_cb, 0, 0, 0, 1);
    restore_context();
    wiiu::fs_begin(0xC006);
    if (wiiu::debug_flag("dump")) dump_enabled = true;
    if (wiiu::debug_flag("cpuvs")) gxt_cpu_vs = true;
    if (wiiu::debug_flag("cputex")) gxt_cpu_tex = true;
    if (wiiu::debug_flag("cpuattr")) gxt_cpu_attr = 0xf;
    if (wiiu::debug_flag("cpuattr8")) gxt_cpu_attr |= 3;
    if (wiiu::debug_flag("cpuattr16")) gxt_cpu_attr |= 4;
    // check_ports.py's runs say they're in Cemu, which has no decoder for signed bytes made into floats and leaves such an attribute out
    // (NSMB2's world map lost its ground, 2026-11-11): there the CPU makes them. The console's GPU fetches them itself.
    in_cemu = wiiu::debug_flag("cemu");
    if (in_cemu) gxt_cpu_attr |= 1;
    if (wiiu::debug_flag("dynres")) dyn_test = true;
    if (wiiu::debug_flag("uberall")) uber_all = true;
    if (wiiu::debug_flag("glslforce")) glsl_force = true;
    if (wiiu::debug_flag("noglslvs")) glsl_vs = false;
    if (wiiu::debug_flag("glslvsforce")) {
        glsl_vs = true;  // a test: every vertex shader comes from the console's compiler, none from the pack
        LOG("[gx2] glslvsforce: %u shipped vertex shaders set aside\n", (u32)vs_table.size());
        for (const auto &e : vs_table) vs_set_aside[e.first] = e.second.index;
        vs_table.clear();
    }
    prof_laps = !wiiu::debug_flag("nolaps");
    if (wiiu::debug_flag("kstats")) kernel::kstats.ipc = true;  // SVCs, switches and service requests per frame, on the log
    if (wiiu::debug_flag("top1080")) {
        top_720 = false;
        LOG("[gx2] top screen drawn at 1800x1080\n");
    }
    if (wiiu::debug_flag("stalecheck")) stale_check = true;
    if (FILE *f = fopen((wiiu::data_dir() + "/gxtrace").c_str(), "rb")) {
        if (fscanf(f, "%u %u", &gxt_from, &gxt_left) != 2) gxt_left = 0;
        fclose(f);
    }
    wiiu::fs_end();
    gfx_is_ready = true;
    LOG("[gx2] ready: TV %ux%u, GamePad %ux%u\n", tv_cb->surface.width, tv_cb->surface.height, drc_cb->surface.width, drc_cb->surface.height);
}

bool gfx_ready() { return gfx_is_ready; }

void gfx_take_misses(std::vector<u8> &out) {
    std::lock_guard<std::mutex> lk(miss_m);
    out.swap(miss_buf);
}

void gfx_write_misses() {
    std::vector<u8> out;
    gfx_take_misses(out);
    if (out.empty()) return;
    if (FILE *f = fopen((wiiu::data_dir() + "/shader_misses.bin").c_str(), "ab")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}
void gfx_vblank() { vblank_pending = true; }

// ---- options menu state; the input thread changes it, the GPU thread draws it
static constexpr int MENU_ITEMS = 4;
static volatile bool menu_is_open;
static volatile int menu_item, menu_speed = 100, menu_volume = 100;
static volatile bool menu_quit;

bool menu_open() { return menu_is_open; }
int game_speed() { return menu_speed; }
bool menu_wants_quit() { return menu_quit; }
void menu_quit_now() { menu_quit = true; }

void menu_input(u32 pressed) {
    if (!menu_is_open) {
        if (pressed & VPAD_BUTTON_STICK_L) {
            menu_item = 0;
            menu_is_open = true;
        }
        return;
    }
    if (pressed & (VPAD_BUTTON_STICK_L | VPAD_BUTTON_B)) menu_is_open = false;
    if (pressed & VPAD_BUTTON_UP) menu_item = (menu_item + MENU_ITEMS - 1) % MENU_ITEMS;
    if (pressed & VPAD_BUTTON_DOWN) menu_item = (menu_item + 1) % MENU_ITEMS;
    const int dir = (pressed & VPAD_BUTTON_RIGHT) ? 1 : (pressed & VPAD_BUTTON_LEFT) ? -1 : 0;
    if (menu_item == 1 && dir) {
        menu_speed = dir > 0 ? (menu_speed >= 400 ? 400 : menu_speed * 2) : (menu_speed <= 100 ? 100 : menu_speed / 2);
        kernel::set_clock_speed(menu_speed);
    }
    if (menu_item == 2 && dir) {
        menu_volume = std::clamp(menu_volume + dir * 10, 0, 100);
        audio::set_volume(menu_volume / 100.0f);
    }
    if (pressed & VPAD_BUTTON_A) {
        if (menu_item == 0) menu_is_open = false;
        if (menu_item == 3) menu_quit = true;
    }
}

static void draw_menu() {
    const u32 w = drc_cb->surface.width, h = drc_cb->surface.height;
    GX2SetColorBuffer(drc_cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, 0, (float)w, (float)h, 0, 1);
    GX2SetScissor(0, 0, w, h);
    GX2SetPixelTexture(&black_tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    set_blit_colors(0, 0, 0, 0.98f, 0.9f, 0.62f);
    draw_rect(w, h, 127, 60, 727, 420, 0, 0, 1, 1);
    set_blit_colors(0, 0, 0, 0.42f, 0.27f, 0.08f);
    draw_rect(w, h, 127, 60, 727, 104, 0, 0, 1, 1);
    const float rowh = 58, top = 130;
    set_blit_colors(0, 0, 0, 1.0f, 0.45f, 0.0f);
    draw_rect(w, h, 145, top + menu_item * rowh - 6, 709, top + menu_item * rowh + 42, 0, 0, 1, 1);
    GX2SetPixelTexture(&font_tex, 0);
    GX2SetPixelSampler(&lut_sampler, 0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    set_blit_colors(1, 1, 1, 0, 0, 0);
    draw_text(w, h, "OPTIONS", 165, 68, 4);
    set_blit_colors(0.2f, 0.1f, 0.0f, 0, 0, 0);
    char line[48];
    const char *items[MENU_ITEMS] = {"BACK TO THE GAME", line, nullptr, "QUIT TO WII U MENU"};
    char vol[32];
    snprintf(line, sizeof line, "GAME SPEED: %d%%", menu_speed);
    snprintf(vol, sizeof vol, "VOLUME: %d%%", menu_volume);
    items[2] = vol;
    for (int i = 0; i < MENU_ITEMS; i++) {
        draw_text(w, h, items[i], 165, top + i * rowh + 4, 4);
    }
    set_blit_colors(0.45f, 0.3f, 0.15f, 0, 0, 0);
    draw_text(w, h, "A: CHOOSE   LEFT/RIGHT: CHANGE   B: BACK", 165, 375, 2);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, drc_cb->surface.image, drc_cb->surface.imageSize);
    state_lost = true;
}

// ---- the software keyboard the game asks for (names, mostly), on the GamePad: touch, or the D-pad and A
static const char *const k_kb_rows[2][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"},
    {"!?()&+_:/%", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM,.-"},
};
static constexpr int KB_COLS = 10, KB_ROWS = 5;  // row 4: shift, space, delete, cancel, OK
static constexpr float KB_X = 107, KB_Y = 150, KB_W = 64, KB_H = 62;
static volatile bool kb_active;
static volatile int kb_row = 1, kb_col = 0, kb_shift;
static char kb_text[64];
static volatile int kb_len;
static swkbd::Request kb_req;

// row 4's keys, by column
static int kb_special(int col) { return col < 2 ? 0 : col < 6 ? 1 : col < 7 ? 2 : col < 8 ? 3 : 4; }

static void kb_press(int row, int col) {
    const bool numpad = kb_req.numpad;
    const int maxlen = kb_req.max_len > 0 ? std::min<int>(kb_req.max_len, sizeof kb_text - 1) : (int)sizeof kb_text - 1;
    if (row < 4) {
        const char c = k_kb_rows[kb_shift][row][col];
        if (numpad && !(c >= '0' && c <= '9')) return;
        if (kb_len < maxlen) {
            kb_text[kb_len] = c;
            kb_len = kb_len + 1;
            kb_text[kb_len] = 0;
        }
        if (kb_shift && row > 0) kb_shift = 0;
        return;
    }
    switch (kb_special(col)) {
    case 0: kb_shift = !kb_shift; break;
    case 1:
        if (kb_len < maxlen && !numpad) {
            kb_text[kb_len] = ' ';
            kb_len = kb_len + 1;
            kb_text[kb_len] = 0;
        }
        break;
    case 2:
        if (kb_len > 0) {
            kb_len = kb_len - 1;
            kb_text[kb_len] = 0;
        }
        break;
    case 3:
        if (kb_req.can_cancel) {
            kb_active = false;
            swkbd::finish("", false);
        }
        break;
    default: {
        // the game's own rules for what it accepts: don't offer to confirm what it would reject
        bool blank = true;
        for (int i = 0; i < kb_len; i++) blank &= kb_text[i] == ' ';
        if ((kb_req.valid == 1 && kb_len == 0) || ((kb_req.valid == 2 || kb_req.valid == 3) && blank)) break;
        if (kb_req.valid == 4 && kb_len != kb_req.max_len) break;
        kb_active = false;
        swkbd::finish(kb_text, true);
        break;
    }
    }
}

bool keyboard_open() { return kb_active; }

void keyboard_input(u32 pressed, bool touch_began, u16 x, u16 y) {
    if (!kb_active) {
        if (swkbd::pending(&kb_req)) {
            size_t n = std::min(kb_req.initial.size(), sizeof kb_text - 1);
            memcpy(kb_text, kb_req.initial.data(), n);
            kb_text[n] = 0;
            kb_len = (int)n;
            kb_row = 1;
            kb_col = 0;
            kb_shift = 1;
            kb_active = true;
        }
        return;
    }
    if (touch_began) {
        const int c = (int)((x - KB_X) / KB_W), r = (int)((y - KB_Y) / KB_H);
        if (x >= KB_X && c < KB_COLS && y >= KB_Y && r < KB_ROWS) kb_press(r, c);
    }
    if (pressed & VPAD_BUTTON_UP) kb_row = (kb_row + KB_ROWS - 1) % KB_ROWS;
    if (pressed & VPAD_BUTTON_DOWN) kb_row = (kb_row + 1) % KB_ROWS;
    if (pressed & VPAD_BUTTON_LEFT) kb_col = (kb_col + KB_COLS - 1) % KB_COLS;
    if (pressed & VPAD_BUTTON_RIGHT) kb_col = (kb_col + 1) % KB_COLS;
    if (pressed & VPAD_BUTTON_A) kb_press(kb_row, kb_col);
    if (pressed & VPAD_BUTTON_B) kb_press(4, 6);       // delete
    if (pressed & VPAD_BUTTON_Y) kb_press(4, 0);       // shift
    if (pressed & VPAD_BUTTON_X) kb_press(4, 2);       // space
    if (pressed & VPAD_BUTTON_PLUS) kb_press(4, 8);    // OK
}

static void draw_keyboard() {
    const u32 w = drc_cb->surface.width, h = drc_cb->surface.height;
    GX2SetColorBuffer(drc_cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, 0, (float)w, (float)h, 0, 1);
    GX2SetScissor(0, 0, w, h);
    GX2SetPixelTexture(&black_tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    set_blit_colors(0, 0, 0, 0.98f, 0.9f, 0.62f);
    draw_rect(w, h, KB_X, 0, KB_X + 640, 480, 0, 0, 1, 1);
    set_blit_colors(0, 0, 0, 1, 1, 1);
    draw_rect(w, h, KB_X + 16, 56, KB_X + 624, 136, 0, 0, 1, 1);
    const auto key_rect = [&](float x0, float y0, float x1, float y1, bool selected) {
        set_blit_colors(0, 0, 0, 0.35f, 0.2f, 0.05f);
        draw_rect(w, h, x0 + 2, y0 + 2, x1 - 2, y1 - 2, 0, 0, 1, 1);
        if (selected) set_blit_colors(0, 0, 0, 1.0f, 0.75f, 0.2f);
        else set_blit_colors(0, 0, 0, 1.0f, 0.45f, 0.0f);
        draw_rect(w, h, x0 + 5, y0 + 5, x1 - 5, y1 - 5, 0, 0, 1, 1);
    };
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < KB_COLS; c++) key_rect(KB_X + c * KB_W, KB_Y + r * KB_H, KB_X + (c + 1) * KB_W, KB_Y + (r + 1) * KB_H, r == kb_row && c == kb_col);
    const int sel = kb_row == 4 ? kb_special(kb_col) : -1;
    const float y4 = KB_Y + 4 * KB_H;
    key_rect(KB_X, y4, KB_X + 2 * KB_W, y4 + KB_H, sel == 0);
    key_rect(KB_X + 2 * KB_W, y4, KB_X + 6 * KB_W, y4 + KB_H, sel == 1);
    key_rect(KB_X + 6 * KB_W, y4, KB_X + 7 * KB_W, y4 + KB_H, sel == 2);
    key_rect(KB_X + 7 * KB_W, y4, KB_X + 8 * KB_W, y4 + KB_H, sel == 3);
    key_rect(KB_X + 8 * KB_W, y4, KB_X + 10 * KB_W, y4 + KB_H, sel == 4);
    GX2SetPixelTexture(&font_tex, 0);
    GX2SetPixelSampler(&lut_sampler, 0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    set_blit_colors(0.1f, 0.05f, 0.0f, 0, 0, 0);
    draw_text(w, h, kb_req.hint.empty() ? "Type here:" : kb_req.hint.c_str(), KB_X + 20, 22, 3);
    draw_text(w, h, kb_text, KB_X + 30, 76, 6);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < KB_COLS; c++) {
            const char t[2] = {k_kb_rows[kb_shift][r][c], 0};
            draw_text(w, h, t, KB_X + c * KB_W + 20, KB_Y + r * KB_H + 12, 5);
        }
    draw_text(w, h, kb_shift ? "ABC" : "abc", KB_X + 28, y4 + 16, 4);
    draw_text(w, h, "Space", KB_X + 2 * KB_W + 68, y4 + 16, 4);
    draw_text(w, h, "<", KB_X + 6 * KB_W + 18, y4 + 12, 5);
    if (kb_req.can_cancel) draw_text(w, h, "X", KB_X + 7 * KB_W + 18, y4 + 12, 5);
    draw_text(w, h, kb_req.ok.empty() ? "OK" : "OK", KB_X + 8 * KB_W + 42, y4 + 16, 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, drc_cb->surface.image, drc_cb->surface.imageSize);
    state_lost = true;
}

// The top screen as its LCD buffer holds it in guest memory, when no display transfer brought it this frame but the GPU's commands wrote into that buffer:
// Luigi's Mansion: Dark Moon draws its top screen in two halves and puts them side by side in the LCD buffer with two texture copies. A 3DS's LCD just shows
// the buffer, so this does too: 240x400, read as a render target's texture is (its last memory row the texture's first), so show_screen takes it as one.
// A game writes the buffer the LCD doesn't show and swaps to it later, often after the next VBlank: what's shown is whichever buffer the LCD shows now,
// as last written, each time that changes.
static void show_lcd_from_memory() {
    gt_mark(GT_LCD, 0);
    static u32 shown_pa, shown_seq;
    static u64 shown_drawn;
    const kernel::FramebufferInfo f = kernel::gsp_framebuffer(0);
    const u32 pa = f.valid && f.addr_left ? mem::vaddr_to_paddr(f.addr_left) : 0;
    const u32 fmt = f.format & 7, bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    const u32 stride = f.stride ? f.stride : 240 * bpp, size = stride * 400;
    if (pa && std::find(std::begin(recent_top), std::end(recent_top), pa) == std::end(recent_top)) {
        static u32 next_top;
        recent_top[next_top++ % 4] = pa;
    }
    recent_top_stride = stride;
    recent_top_bpp = bpp;
    recent_top_fmt = cbuf_to_pixfmt(fmt);  // the display format as a colour buffer's (the table is its own inverse)
    const bool by_transfer = top_shown;
    top_shown = false;
    if (by_transfer) {
        shown_pa = 0;
        return;
    }
    const u8 *src = pa ? pptr(pa) : nullptr;
    if (!src || stride < 240 * bpp) return;
    // put together on the GPU (linear_copy_on_gpu): shown from there, never read back
    Surface *lcd = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->linear && s->addr <= pa && pa + size <= s->addr + s->w * bpp * s->h && s->w * bpp == stride && (!lcd || s->drawn > lcd->drawn)) lcd = s.get();
    if (lcd) {
        if (pa == shown_pa && lcd->drawn == shown_drawn) return;
        shown_pa = pa;
        shown_drawn = lcd->drawn;
        flush_surface(*lcd);
        const float h = (float)lcd->tex_h, first_row = (float)((pa - lcd->addr) / stride);
        static u32 told;
        if (kMeasure && told++ % 300 == 0) LOG("[gx2] top screen from the LCD buffer put together on the GPU, %08x, from row %u of %08x\n", pa, (u32)first_row, lcd->addr);
        show_screen(0, &lcd->tex, false, (h - first_row - 400) / h, (h - first_row) / h, 240.0f / lcd->w);
        top_shown = false;
        return;
    }
    u32 latest = 0;
    for (const MemoryWrite &w : lcd_writes)
        if (w.seq && w.pa < pa + size && pa < w.pa + w.size) latest = std::max(latest, w.seq);
    if (!latest || (pa == shown_pa && latest == shown_seq)) return;
    shown_pa = pa;
    shown_seq = latest;
    static GX2Texture tex[2];
    static u32 next;
    GX2Texture &t = tex[next++ & 1];  // the other may still be on its way to the TV
    if (!t.surface.image) {
        GX2Surface &s = t.surface;
        s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        s.width = 240;
        s.height = 400;
        s.depth = 1;
        s.mipLevels = 1;
        s.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        s.aa = GX2_AA_MODE1X;
        s.use = GX2_SURFACE_USE_TEXTURE;
        s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        t.viewNumMips = t.viewNumSlices = 1;
        t.compMap = 0x00010203;
        GX2CalcSurfaceSizeAndAlignment(&s);
        GX2InitTextureRegs(&t);
        s.image = memalign(s.alignment, s.imageSize);
        if (!s.image) return;
    }
    u8 *out = (u8 *)t.surface.image;
    const u32 pitch = t.surface.pitch * 4;
    for (u32 r = 0; r < 400; r++) {
        const u8 *p = src + (size_t)(399 - r) * stride;
        u8 *o = out + (size_t)r * pitch;
        for (u32 c = 0; c < 240; c++, p += bpp, o += 4) {
            const u16 v = (u16)(p[0] | (p[1] << 8));
            switch (fmt) {
            case 0: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; break;
            case 1: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; break;
            case 2: o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; break;
            case 3: o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 6) & 31) * 255 / 31; o[2] = ((v >> 1) & 31) * 255 / 31; break;
            default: o[0] = ((v >> 12) & 15) * 17; o[1] = ((v >> 8) & 15) * 17; o[2] = ((v >> 4) & 15) * 17; break;
            }
            o[3] = 255;
        }
    }
    DCFlushRange(out, t.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, out, t.surface.imageSize);
    static u32 notes;
    if (kMeasure && notes++ % 100 == 0) {
        u32 lit = 0;
        for (u32 i = 0; i < size; i += 4) lit += src[i] != 0;
        LOG("[gx2] top screen from its LCD buffer %08x, format %u, stride %u: %u of %u samples not black\n", pa, fmt, stride, lit, size / 4);
    }
    show_screen(0, &t, false);
    top_shown = false;
}

// The report is asked for by the frontend's thread and written by this one, between frames: the counters it prints and starts over are this
// thread's, and read from the other they came out torn (a count taken mid-draw, a reset that lost what was added meanwhile).
static std::atomic<bool> report_due;
static std::atomic<float> report_draws_per_flip;
static void write_report();

// (the GPU thread, after each look at its queue) quiet: it had nothing to do for its whole wait. present_rule.h says when a frame is shown.
void gfx_pump(bool quiet) {
    if (!vblank_pending) {
        // the menu and the keyboard are drawn at a VBlank's look only, so while one is up nothing is shown in between
        if (!menu_is_open && !kb_active && present_rule.between(quiet)) {
            screen_fresh = false;
            present();
        }
        return;
    }
    vblank_pending = false;
    show_lcd_from_memory();
    install_compiled();
    if (report_due.exchange(false)) write_report();
    shader_compile_watch();
    if (menu_is_open) {
        draw_menu();
        screen_fresh = true;
    } else if (kb_active) {
        draw_keyboard();
        screen_fresh = true;
    }
    if (present_rule.vblank(screen_fresh)) {
        screen_fresh = false;
        present();
    }
}

void gfx_shutdown() { GX2DrawDone(); }

// Back from the HOME Menu: the system took MEM1 and WHB has made its own buffers again, so the screens' render targets need
// new memory too, in the order they first got it. What was in them is gone; the game draws them again every frame.
void gfx_reacquire() {
    free_whb_depth();
    tv_buffer_to_mem2();
    depth_block = nullptr;
    depth_owner = nullptr;
    for (auto &s : surfaces)  // what they pointed at is the system's now
        if (s->mem1) (s->depth ? s->db.surface : s->cb.surface).image = nullptr;
    for (auto &s : surfaces) {
        set_surface_scale(*s, 0);  // the block it gets back has to hold full size
        if (!s->mem1) continue;
        GX2Surface &f = s->depth ? s->db.surface : s->cb.surface;
        if (s->shared_depth) {
            if (share_depth_block(f)) {
                s->clear_pending = true;
                continue;
            }
            s->shared_depth = false;
        }
        f.image = alloc_surface_mem(f.imageSize, f.alignment, s->mem1, true);
        if (s->depth) {
            GX2ClearDepthStencilEx(&s->db, s->db.depthClear, s->db.stencilClear, GX2_CLEAR_FLAGS_BOTH);
        } else {
            s->tex.surface.image = f.image;
            GX2ClearColor(&s->cb, 0, 0, 0, 1);
        }
        s->written_since_flush = true;
        s->gpu_newer = false;
    }
    GX2ClearColor(tv_cb, 0, 0, 0, 1);
    GX2ClearColor(drc_cb, 0, 0, 0, 1);
    GX2SetSwapInterval(1);
    restore_context();
    vblank_pending = false;
    screen_fresh = false;
    present_rule.reset();
}

void gfx_note_busy(u32 us) { gpu_busy_us = gpu_busy_us + us; }

u64 gfx_flips() { return st_flips; }

// (net thread) a switch from the PC, from now on
bool gfx_set(const char *name, u32 v) {
    if (!strcmp(name, "uberall")) uber_all = v;
    else if (!strcmp(name, "uberfb")) {
        uber_fallback = v;
        forget_ps = true;
    } else if (!strcmp(name, "cpuvs")) gxt_cpu_vs = v;
    else if (!strcmp(name, "vtxcache")) {  // the vertex data cache (vtx_cached); off drops what it holds
        use_vtx_cache = v;
        if (!v) vtx_drop_all = true;  // by the GPU thread, at its next flip
    } else if (!strcmp(name, "fillinside")) fill_inside = v;  // a fill fills the targets wholly inside it (gx2_memory_fill)
    else if (!strcmp(name, "lineargpu")) linear_on_gpu = v;  // texture copies out of linear surfaces on the GPU (linear_copy_on_gpu)
    else if (!strcmp(name, "gputime")) gt_on = v;  // the GPU's own time per pass (gt_mark), in the [prof] lines
    else if (!strcmp(name, "dynres")) dyn_720 = v;           // the top screen's size follows the GPU at 720p too (dyn_res_update)
    else if (!strcmp(name, "toplevel")) dyn_force = v > 1 ? -1 : (int)v;  // 0 full size, 1 the lower, 2 as the GPU keeps up
    else if (!strcmp(name, "lowscale") && v >= 30 && v <= 100) dyn_low = v / 100.0f;  // the lower level's percent of full size each way
    else if (!strcmp(name, "trace")) {  // the next v draws and what they use, in the log
        gxt_from = (u32)g_frame;
        gxt_left = v;
    } else return false;
    return true;
}

void gfx_list() { list_targets = true; }

// (net thread) a picture of the TV at 1/step size, or of the target or texture at a guest address, by the next present; true if it came
bool tv_shot(u32 step, std::vector<u8> &ppm, u32 addr) {
    shot_done = 0;
    shot_addr = addr;
    shot_step = std::max(step, 1u);
    for (u32 i = 0; i < 250 && !shot_done; i++) OSSleepTicks(OSMillisecondsToTicks(20));
    if (shot_done) ppm.swap(shot_ppm);
    return shot_done;
}

float gfx_report() {
    report_due = true;
    return report_draws_per_flip;  // of the report before: what the frontend asks is whether anything is being drawn
}

static void write_report() {
    static OSTime last_t;
    static u64 last_flips, last_draws, last_cpu_draws;
    static u32 last_busy;
    const OSTime now = OSGetSystemTime();
    const float secs = last_t ? OSTicksToMicroseconds(now - last_t) / 1e6f : 1;
    const float fps = (st_flips - last_flips) / secs;
    const float busy = last_t ? (gpu_busy_us - last_busy) / (secs * 1e6f) * 100 : 0;
    LOG("[gx2] %.1f fps, gpu thread %.0f%% busy, %.0f draws/s, %llu on CPU, %llu fragment misses, %llu textures uploaded (%llu KB, %.1f ms, %llu decoded on the GPU), %u KB textures, %u surfaces\n",
        fps, busy, (st_draws - last_draws) / secs, (unsigned long long)(st_cpu_draws - last_cpu_draws), (unsigned long long)st_ps_misses, (unsigned long long)st_uploads,
        (unsigned long long)(st_upload_bytes >> 10), st_upload_ms, (unsigned long long)st_gpu_decodes, (u32)(tex_memory >> 10), (u32)surfaces.size());
    const float draws = std::max<float>((float)(st_draws - last_draws), 1.0f);
    auto us = [&](int k) { return (float)OSTicksToMicroseconds((u64)prof[k]) / draws; };
    const float flips = std::max<float>((float)(st_flips - last_flips), 1.0f);
    auto ms = [&](int k) { return (float)OSTicksToMicroseconds((u64)prof[k]) / 1000.0f / flips; };
    static u64 last_list_ns;
    const u64 list_ns = kernel::gx_stats.gpu_ns;
    LOG("[prof] us per draw: key %.1f bind %.1f ps %.1f state %.1f uni %.1f tex %.1f (hash %.1f up %.1f flush %.1f) vsuni %.1f range %.1f attr %.1f draw %.1f | ms per flip: lists %.1f present %.1f wait %.1f sync %.1f (%.1f times) | per flip: %.1f new luts, %.0f draws, %.0f states built, %.1f cache ops, %.1f texture checks | %u textures\n",
        us(P_KEY), us(P_BIND), us(P_PS), us(P_STATE), us(P_UNI), us(P_TEX), us(P_HASH), us(P_TEXUP), us(P_TEXFL), us(P_VSUNI), us(P_RANGE), us(P_ATTR), us(P_DRAW),
        (list_ns - last_list_ns) / 1e6 / flips, ms(P_PRESENT), ms(P_WAIT), ms(P_SYNC), st_syncs / flips, st_luts / flips, draws / flips, ds_built / flips, st_flushes / flips, st_rehashes / flips, (u32)textures.size());
    last_list_ns = list_ns;
    {
        u64 surf = 0;
        for (const auto &x : surfaces) surf += x->mem1 ? 0 : (x->depth ? x->db.surface.imageSize : x->cb.surface.imageSize);
        LOG("[prof] memory the renderer holds: textures %u KB, vertex cache %u KB (%u buffers let go), surfaces outside MEM1 %u KB (%u surfaces)\n", (u32)(tex_memory >> 10),
            (u32)(vtx_cache_size >> 10), (u32)(vtx_retired.size()), (u32)(surf >> 10), (u32)surfaces.size());
        // vertices the CPU shaded instead of the GPU, by why (vertex.cpp)
        static const char *const why[VCPU_COUNT] = {"no compiled shader", "geometry shader", "continued strip"};
        char line[300] = "";
        int n = 0;
        for (int k = 0; k < VCPU_COUNT; k++)
            if (vtx_cpu.draws[k])
                n += snprintf(line + n, sizeof line - n, " | %s: %.1f draws, %.0f vertices, %.2f ms", why[k], vtx_cpu.draws[k] / flips, vtx_cpu.verts[k] / flips,
                              vtx_cpu.ms[k] / flips);
        if (n) LOG("[vtx] shaded on the CPU, per flip%s\n", line);
        vtx_cpu = {};
    }
    LOG("[prof] vertex data per draw: %.1f streams, %.0f vertices, %.1f KB; %.0f%% of streams from the cache (%u entries, %u KB)\n", st_attr_streams / draws,
        st_attr_verts / draws, st_attr_bytes / 1024.0 / draws, 100.0 * st_vtx_hits / std::max<u64>(st_vtx_hits + st_vtx_misses, 1), (u32)vtx_cache.size(), (u32)(vtx_cache_size >> 10));
    st_attr_verts = st_attr_bytes = st_attr_streams = st_vtx_hits = st_vtx_misses = 0;
    if (ds_built > 10 * flips) {  // states built by the hundred per flip: which registers keep changing between the builds
        char line[200] = "";
        int n = 0;
        for (u32 k = 0; k < 8 && n < 170; k++) {
            u32 best = 0;
            for (u32 i = 1; i < 0x200; i++)
                if (ds_churn[i] > ds_churn[best]) best = i;
            if (!ds_churn[best]) break;
            n += snprintf(line + n, sizeof line - n, " %03x:%u", best, ds_churn[best]);
            ds_churn[best] = 0;
        }
        LOG("[prof] %u states built, the registers that changed most between builds:%s\n", ds_built, line);
    }
    memset(ds_churn, 0, sizeof ds_churn);
    ds_built = 0;
    if (gt_frames && !gt_reset) {  // the GPU's own time per flip, the costliest passes first
        const float n = (float)gt_frames;
        auto ms = [&](u64 t) { return (float)OSTicksToMicroseconds(t) / 1000.0f / n; };
        GtSum sum[GT_SUMS];
        const u32 k = std::min<u32>((u32)gt_nsum, GT_SUMS);
        memcpy(sum, gt_sum, k * sizeof *sum);
        std::sort(sum, sum + k, [](const GtSum &a, const GtSum &b) { return a.ticks > b.ticks; });
        char line[400] = "";
        int w = 0;
        for (u32 i = 0; i < k && i < 12 && w < 360; i++) {
            w += snprintf(line + w, sizeof line - w, " | %s %08x %.2f ms (%.1fx)", gt_kind_name[sum[i].kind], sum[i].tag, ms(sum[i].ticks), sum[i].count / n);
        }
        LOG("[prof] GPU per flip: busy %.1f ms of %.1f ms, over %u flips%s\n", ms(gt_busy), ms(gt_span), (u32)gt_frames, line);
        gt_reset = true;
    } else if (gt_broken) LOG("[prof] GPU per flip: no memory for its stamps\n");
    LOG("[gx2] %u read backs, %u gpu copies, last upload %08x %ux%u fmt %u levels %u dirty %d\n", st_readbacks, st_gpu_copies, last_reupload.addr, last_reupload.w, last_reupload.h, last_reupload.fmt,
        last_reupload.levels, last_reupload.dirty);
    st_readbacks = st_gpu_copies = 0;
    memset(prof, 0, sizeof prof);
    st_syncs = st_luts = st_flushes = st_rehashes = 0;
    last_t = now;
    last_flips = st_flips;
    last_draws = st_draws;
    last_cpu_draws = st_cpu_draws;
    last_busy = gpu_busy_us;
    st_uploads = st_upload_bytes = st_gpu_decodes = 0;
    st_upload_ms = 0;
    report_draws_per_flip = draws / flips;
}

void touch_to_bottom(u16 x, u16 y, bool &touch, u16 &tx, u16 &ty) {
    const u32 margin = (854 - 640) / 2;
    if (x < margin || x >= margin + 640) return;
    touch = true;
    tx = (u16)((x - margin) / 2);
    ty = (u16)(y / 2);
}
void input_override(u32 &buttons, bool &touch, u16 &tx, u16 &ty) {
    (void)tx;
    (void)ty;
    if (menu_is_open || kb_active) {
        buttons = 0;
        touch = false;
    }
}
}  // namespace wiiu
