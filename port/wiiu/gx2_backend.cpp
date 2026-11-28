// The PICA200 on GX2. Modeled on runtime/gpu/metal.mm, with the same division of labour: registers are decoded by the shared code
// in runtime/gpu, and this file turns each draw into GPU state. Shaders are compiled ahead of time (tools/shadergen_main.cpp); a
// draw whose vertex program has no compiled shader is shaded on the CPU instead, and logged so that a later build can include it.
//
// Surfaces hold PICA window rows: row 0 is y = 0, the bottom of the image, which is also how textures are sampled (t = 0 at row
// 0). So a render target can be sampled as a texture as it is. Screens are rotated into place when they are presented.
#include <coreinit/cache.h>
#include <coreinit/debug.h>
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

#include "uber_family.h"
#include "network_menu.h"
#include "chat_border.h"
#include "azl/platform.hpp"
#include "network_settings.h"
#include "network_keyboard.h"
#include "network_menu_model.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/pica.h"
#include "depth_viewport.h"
#include "audio_iface.h"
#include "gpu_iface.h"
#include "services/services.h"
#include "kernel.h"
#include "latte_program.h"
#include "gfd_index.h"
#include "memory_fills.h"
#include "checkpoint.h"
#include "loader.h"
#include "present_rule.h"
#include "scaled_copy.h"
#include "screen_layouts.h"
#include "index_ranges.h"
#include "darkmoon_policy.h"
#include "surface_readback.h"
#include "settings.h"
#include "save_backup.h"
#include "shader_compile.h"
#include "shadergen.h"
#include "mii_face.h"
#include "mii_select.h"
#include "swkbd.h"
#include "ui_font.h"
#include "vertex_arrays.h"
#include "wiiu.h"

extern "C" void *GfxHeapAllocMEM1(uint32_t size, uint32_t alignment) __attribute__((weak));
extern "C" void GfxHeapFreeMEM1(void *block) __attribute__((weak));
extern "C" void *GfxHeapAllocForeground(uint32_t size, uint32_t alignment) __attribute__((weak));
// in gx2.rpl, and wut's import library, but not its headers
extern "C" void GX2SampleTopGPUCycle(uint64_t *cycle);
extern "C" void GX2SampleBottomGPUCycle(uint64_t *cycle);
extern "C" uint64_t GX2GPUTimeToCPUTime(uint64_t time);

namespace wiiu {
bool keyboard_open();
bool chat_visible();
void draw_chat_on(GX2ColorBuffer *);
#ifdef RECOMP_RESUMABLE
void draw_state_saving_on(GX2ColorBuffer *cb);
bool state_loading_visible();
#endif
static void relieve_mem1();  // fast memory found full the first time (beside tv_buffer_to_mem2, which it uses)
}

namespace gpu {
u64 shipped_set_id;  // which shipped set this build has (a hash of shaders.idx): the console's own compile cache goes with it (shader_compile.cpp)
namespace {

// ---------------------------------------------------------------- timers: where a draw's time goes, for the log
enum { P_KEY, P_BIND, P_PS, P_STATE, P_UNI, P_TEX, P_VSUNI, P_RANGE, P_ATTR, P_DRAW, P_HASH, P_WAIT, P_PRESENT, P_SYNC, P_TEXUP, P_TEXFL, P_COUNT };
u32 prof[P_COUNT], st_syncs, st_luts, st_flushes, st_rehashes;
u64 st_top_pictures;  // top screen pictures shown so far (show_screen)
u32 st_pace[5];       // (measuring builds) the top screen's pictures by the VBlanks each stayed up, 4 for 4 and more (present)
// The clock is read for these only in a build that measures (kMeasure): the time now, the time since then added to one of the counters
// above, and a draw's steps one after another (lap). "nolaps" in the data folder leaves the steps out of a measuring build, to see
// what reading the clock a dozen times a draw costs.
bool prof_laps = true;
inline OSTime prof_now() { return cpu_timing_enabled() ? OSGetSystemTime() : 0; }
inline void prof_add(int k, OSTime since) {
    if (since) prof[k] += (u32)(OSGetSystemTime() - since);
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
std::atomic<bool> live_gpu_timing{false};  // opt-in GPU timestamps in a release build; no alarm-based CPU sampler
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
    if ((!kMeasure && !live_gpu_timing.load(std::memory_order_relaxed)) || !gt_on || gt_broken) return;
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
    if ((!kMeasure && !live_gpu_timing.load(std::memory_order_relaxed)) || !gt_on || gt_broken) return;
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
    if (cpu_timing_enabled() && prof_laps) prof_t = OSGetSystemTime();
}
inline void lap(int k) {
    if (!cpu_timing_enabled() || !prof_laps) return;
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
float *ring_constant;
float ring_constant_value[4];

void ring_next_segment() {
    ring_constant = nullptr;
    // What has been submitted by now is the last that can read this segment, or the one before it: a draw whose data filled the end of that
    // one was only submitted from this one, after that one's fence was taken.
    const OSTime submitted = GX2GetLastSubmittedTimeStamp();
    ring_fence[ring_seg] = submitted;
    ring_fence[(ring_seg + 2) % 3] = submitted;
    ring_seg = (ring_seg + 1) % 3;
    const OSTime tw = OSGetSystemTime();
    if (ring_fence[ring_seg]) GX2WaitTimeStamp(ring_fence[ring_seg]);
    const u32 waited = (u32)(OSGetSystemTime() - tw);
    if (cpu_timing_enabled()) prof[P_WAIT] += waited;
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

const float *constant_vertex(const float *value) {
    if (!ring_constant || memcmp(ring_constant_value, value, sizeof ring_constant_value)) {
        ring_constant = (float *)ring_alloc(64, 64);
        memcpy(ring_constant_value, value, sizeof ring_constant_value);
        memcpy(ring_constant, value, sizeof ring_constant_value);
        DCFlushRange(ring_constant, sizeof ring_constant_value);
    }
    return ring_constant;
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
    s32 corner = -1;   // a geometry shader's draw (gen_gsx_glsl): where its corner numbers go
    s32 vertex_loc[3][16];  // input register -> attribute location for a fixed-primitive run's second to fourth vertices (w, x, y)
};
struct PsEntry {
    GX2PixelShader *shader = nullptr;
    u32 index = 0;
};
constexpr u32 COMPILED_HERE = 0x7ffffffe;  // a VsEntry or PsEntry index: the console compiled it, nothing to load from the shipped set
constexpr u32 MISSED = ~0u;                // the set lacks it: the fallback draws, or the CPU does, until the console has compiled it
const void *extra_ps_gsh;
constexpr u32 EXTRA_PS_INDEX = 0x40000000;
// GPU-thread owned, monotonic for this session: queued draws finish before a larger VS partition is published.
uint32_t vertex_registers = shader_budget::kDefaultVertex;
bool state_lost = true;  // the GPU's state no longer matches what was last set
void shader_mode() {
    GX2SetShaderModeEx(GX2_SHADER_MODE_UNIFORM_BLOCK, vertex_registers, 64, 0, 0, shader_budget::pixel(vertex_registers), 192);
}
void require_vs_registers(uint32_t required) {
    const uint32_t next = shader_budget::vertex(vertex_registers, required);
    if (!next) FATAL("vertex shader needs %u ordinary registers (supported 1..%u)\n", required, shader_budget::kOrdinary);
    if (next == vertex_registers) return;
    GX2DrawDone();
    vertex_registers = next;
    shader_mode();
    state_lost = true;
    LOG("[gx2] shader registers: VS %u, PS %u\n", vertex_registers, shader_budget::pixel(vertex_registers));
}
std::unordered_map<u64, VsEntry> vs_table;
std::unordered_map<u64, PsEntry> ps_table;
GX2VertexShader *vs_passthrough, *vs_blit;
GX2PixelShader *ps_blit, *ps_fallback, *ps_fallback_tex, *ps_uber, *ps_uber_lit;
GX2PixelShader *ps_uber_family[2][8];
GX2PixelShader *ps_uber_bounded[2][8][7][5];
u32 ps_uber_bounded_index[2][8][7][5];
void *bounded_ps_gsh;
std::unordered_map<u64, PsEntry> material_fallbacks;
std::unordered_set<GX2PixelShader *> generic_pixel_shaders, material_pixel_shaders, lighting_pixel_shaders, extension_pixel_shaders;
u64 st_material_draws{}, st_lighting_draws{}, st_extension_draws{};
std::atomic<bool> fallback_uv_select{true}, material_extension{true};
std::atomic<unsigned> bounded_fallback{3};  // exact lighting family when available; otherwise material/general fallback
u64 st_fragment_fallback[2]{}, st_fragment_specialized{}, st_fragment_simple{};
u64 st_fallback_shape[2][7][5]{};
std::atomic<bool> copy_rect_experiment{false};
std::atomic<unsigned> index_range_experiment{0};
gx2_indices::Ranges index_ranges;
u64 index_range_hits=0,index_range_misses=0;
std::atomic<int> viewport_nudge_override{-1};
GX2PixelShader *uber_for(const DrawState &ds) {
    u32 mask = 0;
    for (u32 i = 0; i < 3; i++) if (ds.need_tex[i] && !ds.tex[i].null_tex) mask |= 1u << i;
    unsigned mode = bounded_fallback.load(std::memory_order_relaxed);
    if (g_game.program_id == 0x000400000010bf00ULL && !ds.lighting && ds.alpha_test && ds.tev_count == 6) {
        // Hardware draw trace: this Triple Deluxe UI combiner loses artwork
        // with full and material fallback, while the bounded shader draws it.
        u32 words[FS_KEY_WORDS];
        const u32 count = fs_key_words(ds, words);
        const u64 key = fnv64(words, count * sizeof(u32));
        if ((key == 0x598ffe1f5c6d8746ULL || key == 0xd55e3b5a791ff5b0ULL)
            && bounded_ps_gsh && ps_uber_bounded_index[0][mask][6][0] != MISSED)
            mode = 1;
    }
    if (mode && ds.tev_count <= 6) {
        const u32 bucket = ds.lighting ? uber_light_bucket(std::min<u32>(ds.light.num_lights, 8)) : 0;
        if (mode >= 2) {
            const bool lighting = mode >= 3 && ds.lighting;
            const u64 material = uber_material_key(ds, kUberLightCaps[bucket]);
            const u64 keys[] = {lighting ? uber_lighting_material_key(ds, kUberLightCaps[bucket]) : 0,
                                material_extension.load(std::memory_order_relaxed) ? material ^ kMaterialExtensionTag : 0,
                                material};
            for (u32 tier = 0; tier < 3; ++tier) {
                if (!keys[tier]) continue;
                const auto it = material_fallbacks.find(keys[tier]);
                if (it == material_fallbacks.end()) continue;
                auto &entry = it->second;
                if (!entry.shader) {
                    entry.shader = WHBGfxLoadGFDPixelShader(entry.index, bounded_ps_gsh);
                    if (!entry.shader) FATAL("cannot load material fallback %u\n", entry.index);
                    generic_pixel_shaders.insert(entry.shader);
                    material_pixel_shaders.insert(entry.shader);
                    if (tier == 0) lighting_pixel_shaders.insert(entry.shader);
                    if (tier == 1) extension_pixel_shaders.insert(entry.shader);
                }
                return entry.shader;
            }
        }
        auto &p = ps_uber_bounded[ds.lighting != 0][mask][ds.tev_count][bucket];
        const u32 index = ps_uber_bounded_index[ds.lighting != 0][mask][ds.tev_count][bucket];
        if (bounded_ps_gsh && index != MISSED) {
            if (!p) {
                p = WHBGfxLoadGFDPixelShader(index, bounded_ps_gsh);
                generic_pixel_shaders.insert(p);
            }
            if (!p) FATAL("cannot load bounded fallback %u\n", index);
            return p;
        }
    }
    GX2PixelShader *p = ps_uber_family[ds.lighting != 0][mask];
    return p ? p : ds.lighting ? ps_uber_lit : ps_uber;
}
GX2PixelShader *ps_depth_copy;
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
std::atomic<bool> uber_all{false};  // "uberall" (a file in the data folder, or set from the PC): every draw uses the uber shaders, which shows up a mistake in them
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
u64 st_immediate_draws;
u64 st_draws, st_cpu_draws, st_ps_misses, st_uploads, st_upload_bytes, st_flips;
double st_upload_ms;

std::vector<u8> read_whole(const char *path, u32 align_to) {
    std::vector<u8> d;
    FILE *f = fopen(path, "rb");
    if (!f) return d;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return d; }
    const long length = ftell(f);
    if (length < 0 || (size_t)length > SIZE_MAX - align_to || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return d;
    }
    d.resize((size_t)length + align_to);
    const bool read_ok = fread(d.data(), 1, (size_t)length, f) == (size_t)length && !ferror(f);
    fclose(f);
    if (read_ok) d.resize((size_t)length);
    else d.clear();
    return d;
}

// shaders.gsh is read as each shader is needed, not kept whole: with every lit state compiled ahead, Luigi's Mansion: Dark Moon's is 51 MB, which
// stayed in a heap that a long session filled up. At boot the file is read once to note where each shader's header and program blocks are; a shader
// is then two reads into a small GFD of its own, which WHB copies out of (its program in memory of its own), and which goes again.
using GfdShader = gfd_index::Shader;
std::vector<GfdShader> gfd_vs, gfd_ps;
std::vector<u8> gfd_file_head, gfd_end;  // the file's header and its end block
FILE *gfd_file;
// Small packs stay in RAM: a WUHB's streamed seeks pass through Aroma's
// allocating hooks, and a hardware heartbeat dump caught that heap lock in
// gfd_one. Keep the memory-saving stream path for packs above this fixed cap.
constexpr size_t GFD_RESIDENT_LIMIT = 16u << 20;
std::vector<u8> gfd_resident;
u32 st_shipped_vs_reuses, st_shipped_ps_reuses;

static void gfd_scan(const u8 *d, u32 n, const u8 *idx, u32 idx_size) {
    auto be32 = [&](u32 at) { return (u32)d[at] << 24 | (u32)d[at + 1] << 16 | (u32)d[at + 2] << 8 | d[at + 3]; };
    gfd_index::Index parsed;
    if (const char *error = gfd_index::scan(d, n, parsed)) FATAL("shaders.gsh: %s\n", error);
    gfd_vs = std::move(parsed.vs);
    gfd_ps = std::move(parsed.ps);
    gfd_file_head.assign(d, d + parsed.header_len);
    gfd_end.assign(d + parsed.end_at, d + parsed.end_at + parsed.end_len);
    if (!idx || idx_size < 32) FATAL("shaders.idx is missing\n");
    auto index_word = [&](u32 at) { return (u32)idx[at] << 24 | (u32)idx[at + 1] << 16 | (u32)idx[at + 2] << 8 | idx[at + 3]; };
    if (index_word(0) != kShaderIndexMagic || index_word(4) != kShaderIndexVersion) FATAL("shaders.idx is the wrong version\n");
    const u32 pixels = index_word(8), vertices = index_word(12), entries = (idx_size - 32) / 16;
    if (pixels > entries || vertices > entries - pixels) FATAL("shaders.idx is truncated\n");
    u32 most_vs = shader_budget::kDefaultVertex;
    std::vector<bool> used_vs(gfd_vs.size()), used_ps(gfd_ps.size());
    auto use = [&](bool vertex, u32 index) {
        const auto &shaders = vertex ? gfd_vs : gfd_ps;
        auto &used = vertex ? used_vs : used_ps;
        if (index >= shaders.size()) FATAL("shaders.idx references missing %s program %u\n", vertex ? "vertex" : "pixel", index);
        if (used[index]) return;
        used[index] = true;
        const auto &s = shaders[index];
        const u32 h = be32(s.head_at + 4), p = s.prog_len ? be32(s.prog_at + 4) : 0;
        if (s.head_len - h < 4 || !s.prog_len) FATAL("shaders.gsh has an incomplete shader %u\n", index);
        const u32 declared = be32(s.head_at + h) & 255;
        u32 required;
        if (!program_registers(d + s.prog_at + p, s.prog_len - p, declared, required) || required != declared)
            FATAL("shaders.gsh %s program %u has invalid layout/register references (declared %u, maximum %u)\n", vertex ? "vertex" : "pixel", index, declared, shader_budget::kOrdinary);
        if (vertex) most_vs = std::max(most_vs, required);
    };
    // Invalid GSX bodies remain in GSH after Builder removes their IDX entries; only reachable programs need a partition.
    use(true, index_word(16)); use(true, index_word(20));
    use(false, index_word(24)); use(false, index_word(28)); use(false, index_word(28) + 1);
    for (u32 i = 0; i < pixels + vertices; i++) use(i >= pixels, index_word(32 + 16 * i + 8));
    require_vs_registers(most_vs);
}

// one shader's blocks as a GFD file of their own, its blocks numbered as the first of their kind
static std::vector<u8> gfd_one(const GfdShader &g) {
    std::vector<u8> out(gfd_file_head);
    const size_t at = out.size();
    out.resize(at + g.head_len + g.prog_len);
    if (!gfd_resident.empty()) {
        // gfd_scan checked both extents before this pack became resident.
        memcpy(out.data() + at, gfd_resident.data() + g.head_at, g.head_len);
        memcpy(out.data() + at + g.head_len, gfd_resident.data() + g.prog_at, g.prog_len);
    } else {
        wiiu::romfs_begin();
        const bool head_seek = fseek(gfd_file, g.head_at, SEEK_SET) == 0;
        const size_t a = head_seek ? fread(out.data() + at, 1, g.head_len, gfd_file) : 0;
        const bool prog_seek = fseek(gfd_file, g.prog_at, SEEK_SET) == 0;
        const size_t b = prog_seek ? fread(out.data() + at + g.head_len, 1, g.prog_len, gfd_file) : 0;
        wiiu::romfs_end();
        if (a != g.head_len || b != g.prog_len) FATAL("shaders.gsh could not be read\n");
    }
    for (size_t blk : {at, at + g.head_len})
        for (int k = 0; k < 8; k++) out[blk + 24 + k] = 0;  // block id and index: 0
    out.insert(out.end(), gfd_end.begin(), gfd_end.end());
    return out;
}

GX2VertexShader *load_vs(u32 index) {
    wiiu::last_vs = index;
    if (index >= gfd_vs.size()) FATAL("no vertex shader %u\n", index);
    // The pack can map many guest keys to this same program. Share its GX2
    // allocation while keeping the input metadata in each guest key's entry.
    static std::vector<GX2VertexShader *> programs(gfd_vs.size());
    GX2VertexShader *&s = programs[index];
    if (s) {
        if (kMeasure) st_shipped_vs_reuses++;
        return s;
    }
    std::vector<u8> one = gfd_one(gfd_vs[index]);
    s = WHBGfxLoadGFDVertexShader(0, one.data());
    if (!s) FATAL("cannot load vertex shader %u\n", index);
    return s;
}
GX2PixelShader *load_ps(u32 index) {
    wiiu::last_ps = index;
    if (index & EXTRA_PS_INDEX) {
        GX2PixelShader *shader = WHBGfxLoadGFDPixelShader(index & ~EXTRA_PS_INDEX, extra_ps_gsh);
        if (!shader) FATAL("cannot load extra pixel shader %u\n", index);
        return shader;
    }
    if (index >= gfd_ps.size()) FATAL("no pixel shader %u\n", index);
    static std::vector<GX2PixelShader *> programs(gfd_ps.size());
    GX2PixelShader *&s = programs[index];
    if (s) {
        if (kMeasure) st_shipped_ps_reuses++;
        return s;
    }
    std::vector<u8> one = gfd_one(gfd_ps[index]);
    s = WHBGfxLoadGFDPixelShader(0, one.data());
    if (!s) FATAL("cannot load pixel shader %u\n", index);
    return s;
}

// This separate optional pack keeps shaders.gsh below its resident-size cap.
// Its programs are copied to GX2 memory lazily; no SD reads occur during draws.
void load_bounded_fallbacks() {
    for (auto &lit : ps_uber_bounded_index) for (auto &mask : lit)
        for (auto &stages : mask) for (auto &index : stages) index = MISSED;
    FILE *file = fopen("fs:/vol/content/fallback-shaders.bin", "rb");
    if (!file) return;  // packs from before bounded fallbacks remain valid
    u32 header[3];
    std::array<u64, 2048> keys;
    if (fread(header, sizeof(u32), 3, file) != 3 || header[0] != 0x55465031 ||
        !header[1] || header[1] > keys.size() || header[2] < 32 || header[2] > (16u << 20) ||
        fread(keys.data(), sizeof(u64), header[1], file) != header[1]) {
        fclose(file);
        OSReport("[3dii-u shader] ignoring invalid bounded fallback header\n");
        return;
    }
    void *pack = memalign(64, header[2]);
    const bool valid = pack && fread(pack, 1, header[2], file) == header[2] && fgetc(file) == EOF;
    fclose(file);
    gfd_index::Index parsed;
    const char *error = valid ? gfd_index::scan((const u8 *)pack, header[2], parsed) : "short read";
    if (error || parsed.ps.size() != header[1] || !parsed.vs.empty()) {
        free(pack);
        OSReport("[3dii-u shader] ignoring invalid bounded fallback programs\n");
        return;
    }
    std::unordered_map<u64, u32> indices;
    for (u32 i = 0; i < header[1]; ++i) {
        if (!indices.emplace(keys[i], i).second) {
            free(pack);
            OSReport("[3dii-u shader] ignoring duplicate bounded fallback key\n");
            return;
        }
    }
    u32 found = 0;
    for (bool lit : {false, true}) for (u32 mask = 0; mask < 8; ++mask)
        for (int stages = 0; stages <= 6; ++stages) for (u32 bucket = 0; bucket < (lit ? 5u : 1u); ++bucket) {
            const std::string key = uber_family_key(lit, mask, stages, kUberLightCaps[bucket]);
            const auto it = indices.find(fnv64(key.data(), key.size()));
            if (it != indices.end()) {
                ps_uber_bounded_index[lit][mask][stages][bucket] = it->second;
                ++found;
            }
        }
    for (const auto &[key, index] : indices) material_fallbacks.emplace(key, PsEntry{nullptr, index});
    bounded_ps_gsh = pack;
    OSReport("[3dii-u shader] %u fallback entries resident (%u bounded shapes, %u KB), programs loaded on demand\n", header[1], found, header[2] >> 10);
}

// An optional developer pack in the save directory adds previously missed pixel
// shaders by key, while the bundle remains the source of the original tables.
void load_extra_pixel_shaders() {
    const std::string path = wiiu::data_dir() + "/extra-pixel-shaders.bin";
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return;
    u32 header[3];
    std::array<u64, 1024> keys;
    if (fread(header, sizeof(u32), 3, file) != 3 || header[0] != 0x4D505331 ||
        !header[1] || header[1] > keys.size() || header[2] < 32 || header[2] > (16u << 20) ||
        fread(keys.data(), sizeof(u64), header[1], file) != header[1]) {
        fclose(file);
        OSReport("[3dii-u shader] extra pixel shader pack has an invalid header\n");
        return;
    }
    void *pack = memalign(64, header[2]);
    const bool valid = pack && fread(pack, 1, header[2], file) == header[2] &&
                       memcmp(pack, "Gfx2", 4) == 0 && fgetc(file) == EOF;
    fclose(file);
    if (!valid) {
        free(pack);
        OSReport("[3dii-u shader] extra pixel shader pack could not be read\n");
        return;
    }
    gfd_index::Index parsed;
    const char *error = gfd_index::scan((const u8 *)pack, header[2], parsed);
    if (error || parsed.ps.size() != header[1]) {
        OSReport("[3dii-u shader] ignoring extra shader pack: %s\n", error ? error : "pixel shader count mismatch");
        free(pack);
        return;
    }
    extra_ps_gsh = pack;
    u32 added = 0;
    for (u32 i = 0; i < header[1]; ++i)
        added += ps_table.try_emplace(keys[i], PsEntry{nullptr, EXTRA_PS_INDEX | i}).second;
    OSReport("[3dii-u shader] added %u precompiled pixel shaders\n", added);
}

void load_shader_tables() {
    static std::vector<u8> idx;
    idx = read_whole("fs:/vol/content/shaders.idx", 0);
    shipped_set_id = fnv64(idx.data(), idx.size());
    {
        std::vector<u8> file = read_whole("fs:/vol/content/shaders.gsh", 0);
        if (file.empty()) FATAL("shaders.gsh is missing\n");
        gfd_scan(file.data(), (u32)file.size(), idx.data(), (u32)idx.size());
        if (file.size() <= GFD_RESIDENT_LIMIT) gfd_resident = std::move(file);
    }
    if (gfd_resident.empty()) {
        gfd_file = fopen("fs:/vol/content/shaders.gsh", "rb");
        if (!gfd_file) FATAL("shaders.gsh is missing\n");
        OSReport("[3dii-u shader] large pack uses streamed reads\n");
    } else {
        OSReport("[3dii-u shader] resident pack: %u bytes; no shader file seeks during drawing\n", (u32)gfd_resident.size());
    }
    if (idx.size() < sizeof(ShaderIndexHeader)) FATAL("shaders.idx is missing\n");
    const auto *h = (const ShaderIndexHeader *)idx.data();
    if (h->magic != kShaderIndexMagic || h->version != kShaderIndexVersion) FATAL("shaders.idx is the wrong version\n");
    if (!gfd_index::table_fits(idx.size(), sizeof(*h), sizeof(ShaderIndexEntry), h->num_ps, h->num_vs))
        FATAL("shaders.idx has a truncated entry table\n");
    const auto *e = (const ShaderIndexEntry *)(h + 1);
    for (u32 i = 0; i < h->num_ps; ++i)
        if (e[i].index >= gfd_ps.size()) FATAL("shaders.idx pixel index %u is out of range\n", e[i].index);
    for (u32 i = 0; i < h->num_vs; ++i)
        if (e[h->num_ps + i].index >= gfd_vs.size()) FATAL("shaders.idx vertex index %u is out of range\n", e[h->num_ps + i].index);
    for (u32 i = 0; i < h->num_ps; i++, e++) ps_table[(u64)e->key_hi << 32 | e->key_lo].index = e->index;
    for (u32 i = 0; i < h->num_vs; i++, e++) {
        VsEntry &v = vs_table[(u64)e->key_hi << 32 | e->key_lo];
        v.index = e->index;
        v.inputs = e->inputs;
    }
    vs_passthrough = load_vs(h->passthrough_vs);
    vs_blit = load_vs(h->blit_vs);
    ps_blit = load_ps(h->blit_ps);
    const auto depth_copy = ps_table.find(fnv64(kDepthCopyKey, strlen(kDepthCopyKey)));
    if (depth_copy != ps_table.end()) ps_depth_copy = load_ps(depth_copy->second.index);
    if (h->pad >= gfd_ps.size() || gfd_ps.size() - h->pad < 2) FATAL("shaders.idx fallback indices are out of range\n");
    ps_fallback = load_ps(h->pad);
    ps_fallback_tex = load_ps(h->pad + 1);
    const auto uber = ps_table.find(fnv64(kUberKey, strlen(kUberKey)));
    if (uber != ps_table.end()) ps_uber = load_ps(uber->second.index);
    const auto uber_lit = ps_table.find(fnv64(kUberLitKey, strlen(kUberLitKey)));
    if (uber_lit != ps_table.end()) ps_uber_lit = load_ps(uber_lit->second.index);
    for (bool lit : {false, true}) for (u32 mask = 0; mask < 8; mask++) {
        const std::string key = uber_family_key(lit, mask);
        const auto it = ps_table.find(fnv64(key.data(), key.size()));
        if (it != ps_table.end()) ps_uber_family[lit][mask] = load_ps(it->second.index);
    }
    for (auto &lit : ps_uber_family) for (auto *shader : lit) if (shader) generic_pixel_shaders.insert(shader);
    if (ps_uber) generic_pixel_shaders.insert(ps_uber);
    if (ps_uber_lit) generic_pixel_shaders.insert(ps_uber_lit);
    load_bounded_fallbacks();
    // Missing exact states use a generic fragment shader while the compiler
    // prepares a specialized one. Older packs without bounded families retain
    // the full generic shader, including all active lighting and TEV stages.
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
    load_extra_pixel_shaders();
    LOG("[gx2] %u fragment and %u vertex shader keys available\n", (u32)ps_table.size(), h->num_vs);
}

// find_ps's memory of states seen, cleared when the console compiles a shader (gfx_pump)
const DrawState *ps_last_ds;
u64 ps_last_version;
GX2PixelShader *ps_last;
struct SeenPixelShader {
    GX2PixelShader *shader;
    u64 fallback_epoch;  // zero for specialized programs, which survive fallback changes
};
std::unordered_map<u64, SeenPixelShader> ps_seen;
u64 ps_fallback_epoch = 1;
bool ps_last_forced;

GX2PixelShader *find_ps(const DrawState &ds) {
    const DrawState *&last_ds = ps_last_ds;
    u64 &last_version = ps_last_version;
    GX2PixelShader *&last = ps_last;
    const bool forced = uber_all.load(std::memory_order_relaxed);
    if (&ds == last_ds && g.ds_version == last_version && forced == ps_last_forced) return last;
    ps_last_forced = forced;
    last_ds = &ds;
    last_version = g.ds_version;
    GX2PixelShader *uber = forced && ds.need_shade ? uber_for(ds) : nullptr;
    if (uber) return last = uber;
    // Nearly every state has been seen before, and a quick hash finds it. The shaders' own keys are hashed a byte at a time, slow on this CPU.
    u32 words[FS_KEY_WORDS];
    const u32 nwords = fs_key_words(ds, words);
    u32 ha = nwords, hb = 0x9E3779B9u;
    for (u32 i = 0; i < nwords; i++) {
        ha = (ha ^ words[i]) * 0x85EBCA6Bu;
        hb = (hb + words[i]) * 0xC2B2AE35u;
        ha ^= hb >> 15;
    }
    const u64 quick = (u64)ha << 32 | hb;
    if (const auto s = ps_seen.find(quick); s != ps_seen.end()
        && (!s->second.fallback_epoch || s->second.fallback_epoch == ps_fallback_epoch))
        return last = s->second.shader;
    const std::string k((const char *)words, nwords * 4);
    const u64 h = fnv64(k.data(), k.size());
    auto it = ps_table.find(h);
    u64 compile_key = h;
    DrawState normalized;
    if (it == ps_table.end()) {
        normalized = fs_normal(ds);
        const std::string n = fs_key(normalized);
        compile_key = fnv64(n.data(), n.size());
        it = ps_table.find(compile_key);
    }
    GX2PixelShader *fallback = nullptr;
    if (it == ps_table.end() || it->second.index == MISSED) {
        if (ds.need_shade && uber_fallback && !uber) uber = uber_for(ds);
        fallback = !ds.need_shade ? ps_fallback : uber && uber_fallback ? uber : ds.need_tex[0] && !ds.tex[0].null_tex ? ps_fallback_tex : ps_fallback;
    }
    GX2PixelShader *r = fallback;
    if (it != ps_table.end()) {
        // A placeholder is a missing state, not a compiled program. Resolve it
        // again after switching fallback families so its uniforms still match.
        if (it->second.index == MISSED) it->second.shader = fallback;
        if (!it->second.shader) {
        if (glsl_force && (ds.need_shade || ds.wbuffer || ds.scissor_mode == 1) && shader_compile_on()) shader_compile_request(false, h, gen_fs_glsl(ds), 0);
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
        // Unknown states share the same canonical key as shipped programs.
        // Otherwise changes to fixed-function state or uniform-only flags
        // repeatedly compile identical fragment programs during a cold run.
        ps_table[compile_key].shader = fallback;  // log the first raw state once
        ps_table[compile_key].index = MISSED;
        const u32 n = le32((u32)k.size());
        miss_put("FS  ", 4);
        miss_put(&n, 4);
        miss_put(k.data(), k.size());
        if ((ds.need_shade || ds.wbuffer || ds.scissor_mode == 1) && shader_compile_on()) shader_compile_request(false, compile_key, gen_fs_glsl(normalized), 0);  // gfx_pump installs it under the same canonical key
    }
    ps_seen[quick] = {r, fallback ? ps_fallback_epoch : 0};
    last = r;
    return r;
}

// attribute location of input register r, from the name the shader generator gave it
void vs_locations(VsEntry &v) {
    for (auto &l : v.location) l = -1;
    for (auto &k : v.vertex_loc)
        for (auto &l : k) l = -1;
    v.corner = -1;
    for (u32 i = 0; i < v.shader->attribVarCount; i++) {
        const GX2AttribVar &a = v.shader->attribVars[i];
        const u32 r = (u32)atoi(a.name + 1) & 15;
        if (a.name[0] == 'v') v.location[r] = a.location;
        else if (a.name[0] >= 'w' && a.name[0] <= 'y') v.vertex_loc[a.name[0] - 'w'][r] = a.location;  // gen_gsx_glsl's names
        else if (!strcmp(a.name, "aCorner")) v.corner = a.location;
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
            require_vs_registers(d.vs_shader->regs.sq_pgm_resources_vs & 255);
            if (const auto shipped = vs_set_aside.find(d.key); shipped != vs_set_aside.end())  // "glslvsforce": the two side by side
                log_vs_tables("shipped", d.key, load_vs(shipped->second));
            VsEntry &v = vs_table[d.key];
            v.shader = d.vs_shader;
            v.index = COMPILED_HERE;
            v.inputs = d.inputs;
            if (in_cemu) {  // Cemu loses all but the first register of a burst of exports (latte_program.h): the program without them, in a block of its own
                const u8 *const made = (const u8 *)v.shader->program;
                const u32 words = unburst(made, v.shader->size / 4, nullptr);
                if (u8 *const longer = words ? (u8 *)memalign(GX2_SHADER_PROGRAM_ALIGNMENT, words * 4) : nullptr) {
                    unburst(made, v.shader->size / 4, longer);
                    v.shader->program = longer;
                    v.shader->size = words * 4;
                    if (d.from_gfd) free((void *)made);  // CafeGLSL owns its block; a restored program is a separate allocation
                }
            }
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
            ++ps_fallback_epoch;  // fallback states look again; exact programs stay cached
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
    // Entries live for the session, so their addresses survive insertions and rehashing.
    static const decltype(fetch_shaders)::value_type *last = nullptr;
    if (last && last->first == k) return last->second.get();
    auto it = fetch_shaders.find(k);
    if (it == fetch_shaders.end()) {
        auto fs = std::make_unique<GX2FetchShader>();
        memset(fs.get(), 0, sizeof(GX2FetchShader));
        const u32 size = GX2CalcFetchShaderSizeEx(k.count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
        void *mem = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, size);
        GX2InitFetchShaderEx(fs.get(), (u8 *)mem, k.count, k.s, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, mem, size);
        it = fetch_shaders.emplace(k, std::move(fs)).first;
    }
    last = &*it;
    return last->second.get();
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
#ifdef RECOMP_RESUMABLE
static u64 checkpoint_surface_identity;
#endif
struct Surface {
#ifdef RECOMP_RESUMABLE
    u64 checkpoint_identity = ++checkpoint_surface_identity;
    u32 checkpoint_pins = 0;
#endif
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
    bool copy = false;                                 // a texture a transfer or a copy made on the GPU, its bytes not in guest memory (gpu_copy_transfer, gx2_texture_copy): whatever the game says it wrote there is newer
    surface_readback::Spans written;  // newer guest bytes, including retired aliases, since the GPU last drew this surface
    u64 written_after = 0;
    u64 drawn = 0;                                     // when the GPU last wrote it (gpu_writes): of targets sharing memory, the latest holds what's there
    u64 last_use = 0;                                  // use_counter when it was last drawn into, sampled, copied or shown (forget_unused)
    bool linear = false;                               // its memory holds the picture row by row, not in tiles: a display transfer's linear output (gpu_copy_transfer)
    bool lcd_bottom = false;
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
#ifdef RECOMP_RESUMABLE
static std::shared_ptr<Surface> checkpoint_pin(Surface *surface) {
    ++surface->checkpoint_pins;
    return {surface, [](Surface *p) { --p->checkpoint_pins; }};
}
#endif
std::vector<std::unique_ptr<Surface>> surfaces;
u64 use_counter;  // presents so far: when a texture or a target was last used
inline void used(Surface &s) { s.last_use = use_counter; }
Surface *cur_color, *cur_depth;
// A colour target in memory the game's code reads as it likes, and smaller than a screen: a picture the game may have drawn to pick up itself,
// as Miitopia does a Mii's face (gx2_finish_list). A whole screen it asks for with a transfer.
inline bool cpu_reads(const Surface &s) { return !s.depth && s.addr >= mem::FCRAM_PADDR && s.w * s.h < 240 * 320; }
std::vector<Surface *> read_targets;  // those the list being run has drawn to, in order
GX2ContextState *ctx;
GX2DepthBuffer scratch_depth;
bool frame_started;

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
// The options menu's RESOLUTION line also offers 240, 440 and 480 rows, for a GPU with less to do (settings.ini, from the next start).
// "top1080" (a file in the data folder, or a test run's flag) draws at 1800x1080 again, to compare.
u32 top_rows = 720;
int start_resolution = 720;  // settings.ini's resolution as this start took it: the menu says when a change waits for the next

// The screens' render targets are drawn larger: the top screen for the size above, the bottom one 1:1 on the GamePad. The game
// renders some of them twice as wide and scales them down (cheap antialiasing), so the two axes scale differently.

// vw, vh: the viewport of the draw a target is made for, when there is one
void rt_scale(u32 w, u32 h, float &sx, float &sy, u32 vw = 0, u32 vh = 0) {
    sx = sy = 1;
    // Cave Story 3D's pixel-aligned UI and transition strips develop seams
    // when rasterized into enlarged targets.
    if (g_game.program_id == 0x000400000004a100ULL) return;
    // 256 and 512 wide, and 416 tall: some games draw a little more than the screen and crop it in the display transfer
    const u32 shown = w == 256 ? 240 : w == 512 ? 480 : w;
    if ((h == 400 || h == 416) && (shown == 240 || shown == 480)) {
        sx = (float)top_rows / shown;
        sy = top_rows / 240.0f;
    } else if (h == 320 && (shown == 240 || shown == 480)) {
        sx = 480.0f / shown;
        sy = 2.0f;
    } else if ((w & (w - 1)) == 0 && (h & (h - 1)) != 0 && h > 400 && h <= 1024) {
        // a screen drawn off-screen at about twice the size, for the game to draw as a texture: as sharp as that screen. Not below 720's
        // scale: at 480 rows an 800-1024-row target would come out 1:1 or less, which the rest takes for a target not drawn larger, whose
        // copies go through guest memory
        sx = sy = std::max(top_rows, 720u) * 5 / 3.0f / h;
    } else if (w == 256 && h == 512 && vw == 240 && (vh == 400 || vh == 320)) {
        // Pokemon Ultra Sun and Moon draw both screens, one after the other, into one 256x512 target, and textures into others of that size:
        // a screen's viewport on the draw it's made for says which it is. The top screen's scale, whichever screen that draw is.
        sx = sy = top_rows / 240.0f;
    }
}

// targets their display transfers scale down, drawn at the size they're used (note_downscaled): by address and size, for a target made again
bool half_ss = true;
struct HalfKey {
    u32 addr, w, h;
    float sx, sy;
};
std::vector<HalfKey> half_keys;

constexpr bool color_has_alpha(u32 fmt) { return (fmt & 7) != 1 && (fmt & 7) != 3; }  // RGB8 and RGB565 store no alpha.
constexpr u32 surface_comp_map(u32 fmt) { return 0x00010200 | (color_has_alpha(fmt) ? GX2_SQ_SEL_A : GX2_SQ_SEL_1); }

// scale_as: a copy of that surface's picture (gx2_texture_copy and its mipmaps), made at its scale so what the game draws from it stays as sharp.
// first: the draw a colour target is made for. with: the colour target a depth buffer is bound with, whose scale it has to have: a game uses one
// depth buffer's memory with targets this draws at different scales. linear: a display transfer's picture in rows (Surface::linear), which no
// draw can go into.
Surface *get_surface(u32 addr, u32 w, u32 h, u32 fmt, bool depth, const Surface *scale_as = nullptr, const DrawState *first = nullptr, const Surface *with = nullptr,
                     bool linear = false) {
    for (auto &s : surfaces)
        if (s->addr == addr && s->w == w && s->h == h && s->depth == depth && (!with || (s->base_sx == with->base_sx && s->base_sy == with->base_sy))) {
            if (!depth && color_has_alpha(s->fmt) != color_has_alpha(fmt)) {
                s->tex.compMap = surface_comp_map(fmt);
                GX2InitTextureRegs(&s->tex);
            }
            s->fmt = fmt;
            used(*s);
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
    } else if (with) {
        s->sx = with->base_sx;
        s->sy = with->base_sy;
    } else rt_scale(w, h, s->sx, s->sy, first ? (u32)lroundf(2 * first->vp_hw) : 0, first ? (u32)lroundf(2 * first->vp_hh) : 0);
    if (!depth && !scale_as && half_ss)
        for (const HalfKey &k : half_keys)
            if (k.addr == addr && k.w == w && k.h == h && k.sx <= s->sx && k.sy <= s->sy) s->sx = k.sx, s->sy = k.sy;
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
    // copies and pictures in rows go in MEM2: fast memory is for the targets a game draws into (Metroid's four copies are 4.5 MB each)
    const bool screen = s->sy > 1 && !scale_as && !linear;
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
        // Ever Oasis samples depth for its focus mask and uses stencil to
        // clip effects in dialogue. Plain R32 fixed the sampled depth but
        // lost stencil, exposing an opaque rectangular effect. GX2's legacy
        // FLOAT_X8_X24 name is the 64-bit D32_FLOAT_S8_X24 format (0x81c):
        // retain R32's 24-bit mantissa and the stencil channel together.
        const bool sampleable_depth = fmt == 3 && g_game.program_id == 0x00040000001a4800ULL;
        f.format = sampleable_depth ? GX2_SURFACE_FORMAT_FLOAT_X8_X24 : GX2_SURFACE_FORMAT_UNORM_R24_X8;
        f.aa = GX2_AA_MODE1X;
        f.use = sampleable_depth ? (GX2SurfaceUse)(GX2_SURFACE_USE_DEPTH_BUFFER | GX2_SURFACE_USE_TEXTURE)
                                 : GX2_SURFACE_USE_DEPTH_BUFFER;
        f.tileMode = GX2_TILE_MODE_DEFAULT;
        s->db.viewNumSlices = 1;
        // A depth buffer starts as its guest memory, as on a 3DS: a game that never fills its depth (Pokédex 3D clears only by drawing)
        // finds what VRAM held there, zeros from boot, not the GPU's usual 1.0 or a console's fresh memory. One that fills first overwrites it.
        const u8 *g = pptr(addr);
        const u32 v = g ? (u32)g[0] | (u32)g[1] << 8 | (u32)g[2] << 16 | (u32)g[3] << 24 : 0;
        s->db.depthClear = fmt == 0 ? (v & 0xffff) / 65535.0f : (v & 0xffffff) / 16777215.0f;
        s->db.stencilClear = fmt == 3 ? (u8)(v >> 24) : 0;
        s->clear_pending = true;  // take_depth_block, at the first draw that uses it
        GX2CalcSurfaceSizeAndAlignment(&f);
        GX2InitDepthBufferRegs(&s->db);
        if (screen && mem1_relief && share_depth_block(f)) s->mem1 = s->shared_depth = true;
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
        s->tex.compMap = surface_comp_map(fmt);
        GX2InitTextureRegs(&s->tex);
    }
    s->guest_newer = true;
    used(*s);
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

void flush_surface(Surface &s);

// the surface at base_sx, base_sy times its dyn_f, in the memory it has (which holds its largest size)
void apply_scale(Surface &s) {
    s.sx = s.base_sx * s.dyn_f;
    s.sy = s.base_sy * s.dyn_f;
    s.sw = (u32)(s.w * s.sx);
    s.sh = (u32)(s.tex_h * s.sy);
    s.row0 = s.sh - (u32)(s.h * s.sy);
    GX2Surface &f = s.depth ? s.db.surface : s.cb.surface;
    void *const image = f.image;
    if (image) flush_surface(s);  // Pending writes still use the old pitch and extent.
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
void set_surface_scale(Surface &s, u32 level) {
    if (s.dyn_level == level && s.dyn_f == dyn_factor(level)) return;
    s.dyn_level = level;
    s.dyn_f = dyn_factor(level);
    apply_scale(s);
}

// a surface at another base scale, in the memory it has (bind_targets: a depth buffer after its colour)
void set_base_scale(Surface &s, float bsx, float bsy) {
    if (s.base_sx == bsx && s.base_sy == bsy) return;
    s.base_sx = bsx;
    s.base_sy = bsy;
    apply_scale(s);
}
inline void take_dyn_level(Surface &s) {
    if ((s.dyn_level != dyn_level || s.dyn_f != dyn_factor(dyn_level)) && dyn_scaled(s)) set_surface_scale(s, dyn_level);
}

GX2Surface depth_storage_layout(const Surface &s) {
    GX2Surface layout = s.db.surface;
    if (!s.dyn_level) return layout;
    // The next full-size pass reuses this allocation.
    layout.width = (u32)(s.w * s.base_sx);
    layout.height = (u32)(s.tex_h * s.base_sy);
    layout.tileMode = GX2_TILE_MODE_DEFAULT;
    GX2CalcSurfaceSizeAndAlignment(&layout);
    layout.imageSize = std::max(layout.imageSize, s.db.surface.imageSize);
    layout.alignment = std::max(layout.alignment, s.db.surface.alignment);
    return layout;
}

// Switches for trying a change on a console while it plays ("set <name> 0|1" over the network, gfx_set): each of these on by default
bool use_vtx_cache = true, fill_inside = true, linear_on_gpu = true;
volatile bool vtx_drop_all;

// ---- vertex data, converted once
// A draw's vertex data was copied (and integers made floats) at every draw, the whole span its indices reach: Luigi's Mansion: Dark Moon's draws reach 1300
// to 4100 vertices, 36 to 115 KB, 20 to 50 MB a frame, about 43 ms of each frame on a console. What a draw read stays converted, until the game says it
// may have written there: every way a game can make its writes seen by the GPU (GSP's cache calls, GX's, the kernel's, and the GPU's own copies) comes
// through metal_guest_written, in order with the commands, as the texture cache relies on too. Ordinary CPU stores need not say so: check an entry's
// bytes at its first use in each command list. Repeated draws in that list reuse the check; changed bytes get a new copy, leaving queued draws' copies intact.
struct VtxEntry {
    u32 addr = 0, bytes = 0;  // the source, in guest memory
    void *buf = nullptr;
    u32 size = 0;
    u64 hash = 0;
    u32 used = 0, checked = 0;  // flips, command lists
};
std::unordered_map<u64, VtxEntry> vtx_cache;
std::vector<std::pair<void *, u32>> vtx_retired;  // buffers the GPU may still read, and the flip from which they can go
u64 vtx_cache_size, st_vtx_hits, st_vtx_misses;
u32 vtx_list;
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
        if (e.addr == addr && e.size == size && e.buf && (e.checked == vtx_list || hash_bytes(src, bytes) == e.hash)) {
            e.checked = vtx_list;
            e.used = now;
            if (cpu_timing_enabled()) st_vtx_hits++;
            return e.buf;
        }
        vtx_retire(e);
        vtx_cache.erase(it);
    }
    if (cpu_timing_enabled()) st_vtx_misses++;
    void *buf = memalign(64, size + 64);
    if (!buf) {  // no room: this draw's own copy, as before
        void *w = ring_alloc(size + 64, 64);
        fill(w, src);
        DCFlushRange(w, size);
        return w;
    }
    fill(buf, src);
    // A retired mesh's heap address can still be in the GPU attribute cache.
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, buf, size);
    VtxEntry &e = vtx_cache[key];
    e.addr = addr;
    e.bytes = bytes;
    e.buf = buf;
    e.size = size;
    e.hash = hash_bytes(src, bytes);
    e.used = now;
    e.checked = vtx_list;
    vtx_cache_size += size;
    return buf;
}

// A draw's indices as the GPU reads them, kept like vertex data: 16-bit ones as the game has them (little-endian), 8-bit ones made 16-bit, and
// after them the lowest and highest vertex they use. Reading them for that range and copying them were 1.9 to 3 us of every draw of Luigi's
// Mansion 2 in Cemu on a PC. Kinds 0x90 and 0x91 keep them apart from the attribute copies' (0x80, and fmt | elems << 4).
const u16 *draw_indices(u32 addr, const u8 *ip, u32 count, bool u16_in, u32 &minv, u32 &maxv) {
    const u16 *w = (const u16 *)vtx_cached(addr, ip, u16_in ? 2 : 1, count, u16_in ? 0x90 : 0x91, count * 2 + 4, [&](void *out, const u8 *src) {
        u16 *dst = (u16 *)out;
        u32 lo = 0xffff, hi = 0;
        if (u16_in) {
            memcpy(dst, src, count * 2);
            for (u32 i = 0; i < count; i++) {
                const u32 v = le16(dst[i]);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        } else {
            for (u32 i = 0; i < count; i++) {
                const u32 v = src[i];
                dst[i] = (u16)v;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        dst[count] = (u16)lo;
        dst[count + 1] = (u16)hi;
    });
    minv = w[count];
    maxv = w[count + 1];
    return w;
}

// A target the game draws at twice the size it uses and scales down with a display transfer (its own antialiasing): Luigi's Mansion 2 draws
// each half of its top screen 256 wide for 128 columns of the LCD, twice a frame. At the screens' scale that is 768x1248 for a strip the TV
// shows as 369x1200, 2.2 times the pixels, and those draws took 54 of the GPU's 64 ms a frame on the title screen and in the rain on a
// console (Senne's measure). Such a target is drawn from its next frame at the scale its transfer's destination has, so the transfer copies
// one to one; the game's antialiasing that way goes, the picture is still three times a 3DS's. Its depth buffers (its size, its old scale)
// shrink with it, in their own memory: bind_targets would otherwise make one of the new scale beside the old, and the fast memory the old
// one kept sent Luigi's Mansion 2's next target to slow memory. Kept by address and size for a target made again (get_surface). "set halfss
// 0" stops it.
void note_downscaled(Surface &src, const Surface &dst, u32 scaling) {
    if (!half_ss || !scaling || src.depth) return;
    const float nx = dst.base_sx / 2, ny = std::min(src.base_sy, scaling == 2 ? dst.base_sy / 2 : dst.base_sy);
    if (nx < 1 || ny < 1 || src.base_sx < nx * 1.2f) return;
    if (kMeasure) LOG("[gx2] target %08x %ux%u is scaled down by its transfer: drawn at %.2fx%.2f, not %.2fx%.2f\n", src.addr, src.w, src.h, nx, ny,
                      src.base_sx, src.base_sy);
    bool known = false;
    for (HalfKey &k : half_keys)
        if (k.addr == src.addr && k.w == src.w && k.h == src.h) {
            k.sx = nx, k.sy = ny;
            known = true;
        }
    if (!known && half_keys.size() < 16) half_keys.push_back({src.addr, src.w, src.h, nx, ny});
    for (auto &d : surfaces)
        if (d->depth && d->w == src.w && d->h == src.h && d->base_sx == src.base_sx && d->base_sy == src.base_sy) {
            d->base_sx = nx;
            d->base_sy = ny;
            apply_scale(*d);
        }
    src.base_sx = nx;
    src.base_sy = ny;
    apply_scale(src);
}

u64 gpu_writes;  // a count of the GPU's writes into targets, for Surface::drawn

// Targets of different sizes can alias. Use the most recent write among compatible views.
template <class Match>
Surface *find_surface(u32 addr, bool depth, Match match) {
    Surface *latest = nullptr;
    for (auto &s : surfaces)
        if (s->addr == addr && s->depth == depth && match(*s) && (!latest || s->drawn > latest->drawn))
            latest = s.get();
    if (latest) used(*latest);
    return latest;
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
std::vector<Fill> pending_fills;

MemoryFills memory_fills;

void restore_context() {
    GX2SetContextState(ctx);
    shader_mode();
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
        s.clear_pending = false;  // filled: nothing left of its start-up clear
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
    u64 next_turn;      // when it is hashed in turn next (get_texture)
    bool unflushed;     // it changed with no notice: hashed at its first use every flip, until TURN_QUIET flips pass with no change
    u64 changed_at;     // when it last did
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
u64 tex_memory;
constexpr u64 TEX_MEMORY_LIMIT = 128ull << 20;
// A game may write a texture with the CPU and never flush it, as a 3DS's small cache puts the bytes in memory within a frame anyway: Animal
// Crossing's design canvas, Brain Age's handwriting. So small textures are hashed in turn, up to TURN_BYTES a flip (one up to TURN_LARGEST
// when it's the flip's first), each at most every TURN_FLIPS; one found changed with no notice is hashed at its first use every flip, until
// it stays the same for TURN_QUIET flips.
constexpr u32 TURN_BYTES = 32 << 10, TURN_LARGEST = 64 << 10, TURN_FLIPS = 30, TURN_QUIET = 300;
u32 turn_budget = TURN_BYTES;

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

// Expand the upload to cover every byte whose hash is being replaced.
u32 hash_partial_upload(Tex &e, const u8 *p, u32 *rows) {
    const u32 words = e.bytes / 4;
    u32 h = e.bytes;
    for (u32 k = 0; k < SLICES; k++) {
        const u32 begin = (u32)((u64)words * k / SLICES), end = (u32)((u64)words * (k + 1) / SLICES);
        if (begin != end) {
            const u32 first = begin * 4 / e.row_bytes, last = (end * 4 - 1) / e.row_bytes;
            bool dirty = false;
            for (u32 r = first; r <= last; r++)
                dirty |= (e.dirty_rows[r >> 5] >> (r & 31)) & 1;
            if (dirty) {
                e.slice[k] = hash_slice(p, e.bytes, k);
                for (u32 r = first; r <= last; r++) rows[r >> 5] |= 1u << (r & 31);
            }
        }
        h = h * 31 + e.slice[k];
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
    if (t0) {
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
        // Brain Age's A4 handwriting canvas changes without a flush. The generic
        // periodic check can delay the first stroke; check this canvas every flip.
        const bool handwriting_canvas = g_game.program_id == 0x00040000000b3c00ULL
            && w == 512 && h == 256 && fmt == 11 && levels == 1;
        if ((stale_check || handwriting_canvas) && e.last_use != use_counter && !e.dirty && !e.maybe_dirty && !e.hash_stale
            && hash_guest(pptr(addr), e.bytes, std::array<u32, SLICES>().data()) != e.hash) {
            static u32 logged;
            if (stale_check && logged++ < 400) LOG("STALE tex %08x %ux%u fmt %u levels %u bytes %u, flip %llu\n", addr, w, h, fmt, levels, e.bytes, (unsigned long long)st_flips);
            e.maybe_dirty = true;
        }
        // A later scene can reuse the same guest allocation without flushing it.
        if (use_counter - e.last_use >= TURN_FLIPS) e.maybe_dirty = true;
        bool turn = false;
        if (e.unflushed && use_counter - e.changed_at > TURN_QUIET) e.unflushed = false;
        if (e.last_use != use_counter && !e.dirty && !e.maybe_dirty
            && (e.unflushed || (e.bytes <= TURN_LARGEST && use_counter >= e.next_turn && (e.bytes <= turn_budget || turn_budget == TURN_BYTES)))) {
            if (!e.unflushed) {
                turn_budget -= std::min(turn_budget, e.bytes);
                e.next_turn = use_counter + TURN_FLIPS;
            }
            turn = e.maybe_dirty = true;  // checked below as after a flush
        }
        e.last_use = use_counter;
        if (e.heap_flushes != heap_flushes) {
            e.heap_flushes = heap_flushes;
            const OSTime th = prof_now();
            if (e.hash_stale || sample_guest(pptr(addr), e.bytes) != e.sample) {
                e.maybe_dirty = true;
            } else {
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
            const bool part = e.dirty && !e.maybe_dirty && e.levels == 1 && e.h <= 1024 && !((e.w | e.h) & 7);
            const bool partial_hash = part && !e.hash_stale;
            u32 rows[4];
            memcpy(rows, e.dirty_rows, sizeof rows);
            const u32 hh = partial_hash ? hash_partial_upload(e, pptr(addr), rows)
                         : e.dirty && !e.maybe_dirty ? 0 : hash_guest(pptr(addr), e.bytes, e.slice);
            e.sample = sample;
            prof_add(P_HASH, th);
            // A sample match cannot prove the contents of a texture with no valid baseline.
            if (changed || e.hash_stale || hh != e.hash) {
                if (turn) {
                    e.unflushed = true;
                    e.changed_at = use_counter;
                }
                // the GPU may be reading the old contents
                if (gxt()) LOG("[gxt] reupload %08x %ux%u fmt %u, %u bytes, dirty %d\n", addr, w, h, fmt, e.bytes, e.dirty);
                last_reupload = {addr, w, h, fmt, levels, e.dirty};
                // definite writes to part of a one-level texture: only the rows they touched, and the rows the other image missed meanwhile.
                // A GPU-decoded one is written in order with the draws that read it: nothing to wait for and nothing to spare.
                if (!e.gpu && replace_image(e)) {
                    u32 uploaded[4];
                    memcpy(uploaded, rows, sizeof rows);
                    for (u32 k = 0; k < 4; k++) rows[k] |= e.lag_rows[k];
                    if (part) memcpy(e.lag_rows, uploaded, sizeof uploaded);
                    else memset(e.lag_rows, 0xff, sizeof rows);
                }
                upload_texture(e, part ? rows : nullptr);
            }
            e.hash = hh;
            e.hash_stale = e.dirty && !e.maybe_dirty && !partial_hash;
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
    e.next_turn = use_counter + TURN_FLIPS;
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
// The picture the GamePad shows where no screen is: a bezel or overlay the player chose in the builder, which tools/make_game_title.py puts in
// the title as 854x480 RGBA bytes (pad_bezel.rgba). Without one its image stays null, and those parts are black.
GX2Texture pad_bezel;
constexpr u32 PAD_W = 854, PAD_H = 480;  // the GamePad's screen, the bezel's size
void load_pad_bezel() {
    const std::vector<u8> b = read_whole("fs:/vol/content/pad_bezel.rgba", 0);
    if (b.size() != PAD_W * PAD_H * 4) return;
    GX2Surface &s = pad_bezel.surface;
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.width = PAD_W;
    s.height = PAD_H;
    s.depth = s.mipLevels = 1;
    s.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    s.aa = GX2_AA_MODE1X;
    s.use = GX2_SURFACE_USE_TEXTURE;
    s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    pad_bezel.viewNumMips = pad_bezel.viewNumSlices = 1;
    pad_bezel.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&s);
    GX2InitTextureRegs(&pad_bezel);
    s.image = memalign(s.alignment, s.imageSize);
    if (!s.image) return;
    for (u32 y = 0; y < PAD_H; y++) memcpy((u8 *)s.image + (size_t)y * s.pitch * 4, &b[(size_t)y * PAD_W * 4], PAD_W * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s.image, s.imageSize);
}
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
    f.image = gpu_memalign(f.alignment, f.imageSize);
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
        GX2Texture texture = make_float_lut(w, h);
        if (texture.surface.image) {
            pool.emplace_back();
            pool.back().t = texture;
        }
    }
    if (pool.empty()) FATAL("out of memory for a %ux%u lookup texture\n", w, h);
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
// ds: the draw they're bound for, when it's the one that may make them
void bind_targets(const RTConfig &rt, const DrawState *ds = nullptr) {
    Surface *c = get_surface(rt.color_addr, rt.w, rt.h, rt.color_fmt, false, nullptr, ds);
    c->linear = false;  // what the GPU draws into is tiled
    Surface *d = rt.depth_addr && pptr(rt.depth_addr) ? get_surface(rt.depth_addr, rt.w, rt.h, rt.depth_fmt, true, nullptr, nullptr, c) : nullptr;
    // a depth buffer is the size of the colour it's drawn with: smaller when that one is (note_downscaled), never larger than its memory
    if (d && (d->base_sx != c->base_sx || d->base_sy != c->base_sy) && c->base_sx <= d->base_sx && c->base_sy <= d->base_sy && c->base_sy > 1)
        set_base_scale(*d, c->base_sx, c->base_sy);
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
    c->copy = false;         // drawn into: a target like any other
    c->guest_newer = false;  // shortcut: a target the game writes into after its first use keeps what the GPU drew, as in Cemu; upload it too once a game shows it must
    c->written.clear();
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

// A draw is about to use the depth buffer just bound. If it's in the shared block, or new and never filled, it needs its pending clear now. If a shared one has neither
// that nor the block's contents, the game came back to it without clearing it and these two can't share: this frame's depth is lost, and from now on it has memory of its own.
void take_depth_block(const RTConfig &rt) {
    Surface &d = *cur_depth;
    if (!d.shared_depth && !d.clear_pending) return;
    if (d.shared_depth && depth_owner != &d && !d.clear_pending) {
        LOG("[gx2] depth %08x %ux%u came back with no clear: giving it its own memory\n", d.addr, d.w, d.h);
        GX2Surface &f = d.db.surface;
        const GX2Surface storage = depth_storage_layout(d);
        f.image = alloc_surface_mem(storage.imageSize, storage.alignment, d.mem1, false);
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
    const int requested_nudge = viewport_nudge_override.load(std::memory_order_relaxed);
    const float nudge = requested_nudge >= 0 ? requested_nudge / 100.0f :
        (Sx != floorf(Sx) || Sy != floorf(Sy) ? 0.25f : 0.0f);
    GX2ViewportReg viewport;
    GX2InitViewportReg(&viewport, ds.vp_x * Sx + nudge, ds.vp_y * Sy + c.row0 + nudge, 2 * ds.vp_hw * Sx, 2 * ds.vp_hh * Sy, 0.0f, 1.0f);
    depth_viewport::pica_range(viewport);
    GX2SetViewportReg(&viewport);
    if (ds.scissor_mode == 3) {
        const s32 x1 = std::clamp<s32>(ds.sc_x1, 0, c.w), x2 = std::clamp<s32>(ds.sc_x2, 0, c.w);
        const s32 y1 = std::clamp<s32>(ds.sc_y1, 0, c.h), y2 = std::clamp<s32>(ds.sc_y2, 0, c.h);
        if (copy_rect_experiment.load(std::memory_order_relaxed)) {
            // Clip pixel centres, including the viewport's alignment offset.
            // Copy extents use floor endpoints; that is too small for a
            // fractional raster clip and can turn a slice's last pixel black.
            auto x = gx2_copy::raster_span(x1, x2 - x1, Sx, nudge);
            auto y = gx2_copy::raster_span(y1, y2 - y1, Sy, nudge);
            x.first=std::min(x.first,c.sw); x.last=std::min(x.last,c.sw);
            y.first=std::min(y.first,c.sh-c.row0); y.last=std::min(y.last,c.sh-c.row0);
            GX2SetScissor(x.first, y.first + c.row0, x.size(), y.size());
        } else {
            GX2SetScissor((u32)(x1 * Sx), (u32)(y1 * Sy) + c.row0, (u32)((x2 - x1) * Sx), (u32)((y2 - y1) * Sy));
        }
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
    const bool alpha = color_has_alpha(c.fmt);
    if (alpha && (m & 0xff000000u)) mask |= GX2_CHANNEL_MASK_A;
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
        auto rgb_factor = [&](u32 f) {
            if (!alpha) {
                if (f == 8) return GX2_BLEND_MODE_ONE;  // destination alpha is one
                if (f == 9 || f == 14) return GX2_BLEND_MODE_ZERO;
            }
            return k_factor[std::min<u32>(f, 14)];
        };
        GX2SetBlendControl(GX2_RENDER_TARGET_0, rgb_factor(ds.sf_rgb), rgb_factor(ds.df_rgb), k_eq[std::min<u32>(ds.eq_rgb, 4)], TRUE,
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
    u8 scissor_mode, cull, color_write, color_fmt, blend, logic_op, f[6], depth_test, depth_write, depth_func, stencil, st_ops[4];
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
    now.color_fmt = (u8)c.fmt;
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

// Opt-in globally; enabled by the measured Dark Moon title policy below.
std::atomic<bool> light_guard_experiment{false};

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
    u32 lit[4], lit2[4], lit3[4], lit4[4], sel[7][4], light[8][4], light2[8][4];  // only the lit one reads these
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
        if (!f.image) FATAL("out of memory for the procedural noise texture\n");
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
    const u32 lights = !ds.lighting ? 0 : std::min<u32>(ds.light.num_lights, 8);
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
        u->lscale1[3] = swap_float(light_guard_experiment.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
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
            // Shared specialized shaders branch on this per-draw enable.
            d.att[2] = swap_float(s.spot_atten ? 1.0f : 0.0f);
            d.att[3] = 0;
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
            put_i(b->wrap[k], on ? t.wrap_s : 0, on ? t.wrap_t : 0,
                  k == 0 && on ? t.type : 0, k == 2 ? (u32)fallback_uv_select.load(std::memory_order_relaxed) : (u32)(k == 0 && on && t.shadow_ortho));
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
        if (drawn) used(*drawn);
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

// Sample one frame every 30 presents. Only the card thread emits the trace.
constexpr u32 DRAW_TRACE_INTERVAL = 30;
constexpr u32 DRAW_TRACE_CAPACITY = 64;
bool draw_trace_enabled;
struct DrawTraceState {
    u64 vs, fs;
    u32 color, depth, width, height, depth_scale, depth_offset, flags, blend, color_mask;
    u32 shader;
    std::array<u32, 12> textures;
    std::array<float, 4> viewport;
    bool operator==(const DrawTraceState &) const = default;
};
struct DrawTraceFrame {
    u64 frame;
    u32 count, overflow;
    std::array<DrawTraceState, DRAW_TRACE_CAPACITY> draws;
};
DrawTraceFrame draw_trace_current{}, draw_trace_ready{};
std::mutex draw_trace_mutex;

void trace_draw(const DrawState &ds, GX2PixelShader *ps, u64 vs) {
    if (!draw_trace_enabled || st_flips % DRAW_TRACE_INTERVAL)
        return;
    u32 words[FS_KEY_WORDS];
    const auto word_count = fs_key_words(ds, words);
    const u64 key = fnv64(words, word_count * sizeof(u32));
    u32 shader = MISSED;
    if (ps && generic_pixel_shaders.contains(ps)) shader = ds.lighting ? MISSED - 2 : MISSED - 1;
    else if (ps == ps_fallback_tex) shader = MISSED - 3;
    else if (ps && ps != ps_fallback) {
        auto entry = ps_table.find(key);
        if (entry == ps_table.end() || entry->second.shader != ps) {
            const std::string normal = fs_key(fs_normal(ds));
            entry = ps_table.find(fnv64(normal.data(), normal.size()));
        }
        if (entry != ps_table.end() && entry->second.shader == ps)
            shader = entry->second.index;
        else
            shader = MISSED - 4;  // a specialized shader with no matching table entry
    }

    u32 scale, offset;
    memcpy(&scale, &ds.depth_scale, sizeof scale);
    memcpy(&offset, &ds.depth_offset, sizeof offset);
    const u32 flags = (u32)ds.wbuffer | (u32)ds.depth_test << 1 | (u32)ds.depth_write << 2 |
                      (u32)ds.stencil << 3 | (u32)ds.alpha_test << 4 | (u32)ds.blend << 5 |
                      (u32)ds.early_z << 6 | (u32)ds.depth_func << 8 | ds.depth_bits << 16;
    const u32 blend = (u32)ds.eq_rgb | (u32)ds.eq_a << 4 | (u32)ds.sf_rgb << 8 |
                      (u32)ds.df_rgb << 12 | (u32)ds.sf_a << 16 | (u32)ds.df_a << 20;
    const DrawTraceState state{vs, key, ds.rt.color_addr, ds.rt.depth_addr, ds.rt.w, ds.rt.h,
                              scale, offset, flags, blend, ds.color_mask, shader,
                              {ds.tex[0].face_addr[0], ds.tex[0].w, ds.tex[0].h, ds.tex[0].fmt,
                               ds.tex[1].face_addr[0], ds.tex[1].w, ds.tex[1].h, ds.tex[1].fmt,
                               ds.tex[2].face_addr[0], ds.tex[2].w, ds.tex[2].h, ds.tex[2].fmt},
                              {ds.vp_x, ds.vp_y, 2 * ds.vp_hw, 2 * ds.vp_hh}};
    auto &frame = draw_trace_current;
    for (u32 i = 0; i < frame.count; ++i)
        if (frame.draws[i] == state)
            return;
    if (frame.count == frame.draws.size()) {
        ++frame.overflow;
        return;
    }
    frame.draws[frame.count++] = state;
}

void publish_draw_trace() {
    auto &frame = draw_trace_current;
    if (!frame.count)
        return;
    frame.frame = st_flips;
    if (std::unique_lock lock(draw_trace_mutex, std::try_to_lock); lock.owns_lock())
        draw_trace_ready = frame;
    frame.count = frame.overflow = 0;
}

void begin_draw_common(const DrawState &ds, GX2PixelShader *ps, u64 vs = 0) {
    const bool generic = ps && generic_pixel_shaders.contains(ps);
    if (generic) {
        ++st_fragment_fallback[ds.lighting != 0];
        st_material_draws += material_pixel_shaders.contains(ps);
        st_lighting_draws += lighting_pixel_shaders.contains(ps);
        st_extension_draws += extension_pixel_shaders.contains(ps);
        if (ds.tev_count <= 6) ++st_fallback_shape[ds.lighting != 0][ds.tev_count]
            [ds.lighting ? uber_light_bucket(std::min<u32>(ds.light.num_lights, 8)) : 0];
    } else if (ps && ps != ps_fallback && ps != ps_fallback_tex) ++st_fragment_specialized;
    else ++st_fragment_simple;
    trace_draw(ds, ps, vs);
    if (state_lost) {
        shader_mode();
        state_lost = false;
        state_sent_valid = false;
    }
    set_state(ds);
    GX2SetPixelShader(ps);
    lap(P_STATE);
    set_fragment(ds, generic);
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

// a draw of a vertex shader and the geometry shader after it (gen_gsx_glsl). pa_submit turns a geometry shader's triangle round on its
// winding flag where the topology is the geometry shader's own.
static u64 gsx_key(const Gx2Draw &d) {
    return gsx_variant_key(d.prog, d.entry, g.vs.b, d.gs, d.gs_entry, g.gs.b, g.gs.i, d.vs_outmask, d.gs_in, d.gs_perm, d.slot_src, d.topology == 3);
}

bool gx2_gsx_ready(const Gx2Draw &d) {
    const u64 key = gsx_key(d);
    if (const auto it = vs_table.find(key); it != vs_table.end()) return it->second.index != MISSED;
    vs_table[key].index = MISSED;  // in the miss list once, for the next build (tools/shadergen_main.cpp reads it): little-endian
    std::vector<u32> rec = {le32(d.entry), le32(g.vs.code_size), le32(g.vs.swizzle_size), le32(d.gs_entry), le32(g.gs.code_size), le32(g.gs.swizzle_size),
                            le32(d.vs_outmask), le32(d.gs_in), le32((u32)d.gs_perm), le32((u32)(d.gs_perm >> 32)), le32(d.topology == 3)};
    for (const ShaderSetup *s : {&g.vs, &g.gs}) {
        for (u32 i = 0; i < s->code_size; i++) rec.push_back(le32(s->code[i]));
        for (u32 i = 0; i < s->swizzle_size; i++) rec.push_back(le32(s->swizzle[i]));
    }
    const u32 size = le32((u32)rec.size() * 4 + 88);  // after the tag: what a log reader steps over
    miss_put("GSX ", 4);
    miss_put(&size, 4);
    miss_put(rec.data(), rec.size() * 4);
    miss_put(g.vs.b, 16);
    miss_put(g.vs.i, 16);
    miss_put(g.gs.b, 16);
    miss_put(g.gs.i, 16);
    miss_put(d.slot_src, 24);
    return false;
}

bool gx2_draw(const Gx2Draw &d, const V4 *immediate) {
    const DrawState &ds = *d.ds;
    if (ds.skip || ds.shadow_mode || ds.gas || !ds.rt.color_addr || !pptr(ds.rt.color_addr) || (ds.rt.color_fmt & 0x80)) return true;
    gpu_fallback_why=GPUF_MISSING_VS;gpu_fallback_key=0;
    if (gxt_cpu_vs) {gpu_fallback_why=GPUF_FORCED;return false;}  // debug aid: tells a wrong vertex shader from anything else
    lap_start();
    // usually the same vertex program as the last draw
    static struct {
        const ShaderProgram *prog;
        u32 entry;
        bool b[16];
        s8 slots[24];
        u64 key;
    } vk;
    if (!d.gs && (vk.prog != d.prog || vk.entry != d.entry || memcmp(vk.b, g.vs.b, sizeof vk.b) || memcmp(vk.slots, d.slot_src, sizeof vk.slots))) {
        vk.prog = d.prog;
        vk.entry = d.entry;
        memcpy(vk.b, g.vs.b, sizeof vk.b);
        memcpy(vk.slots, d.slot_src, sizeof vk.slots);
        vk.key = vs_variant_key(d.prog, d.entry, g.vs.b, d.slot_src);
    }
    const u64 key = d.gs ? gsx_key(d) : vk.key;
    gpu_fallback_key=key;
    auto it = vs_table.find(key);
    if (d.gs && (it == vs_table.end() || it->second.index == MISSED)) {gpu_fallback_why=GPUF_MISSING_GS;return false;}  // (gx2_gsx_ready has logged the miss)
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
        const bool compiler_on = shader_compile_on();
        if (glsl_vs && compiler_on) {
            u32 bools = 0, inputs = 0;
            for (u32 i = 0; i < 16; i++)
                if (g.vs.b[i]) bools |= 1u << i;
            shader_glsl_fail_line = 0;
            shader_glsl_fail_at = 0;
            std::string glsl = gen_vs_glsl(d.prog, d.entry, bools, g.vs.i, d.slot_src, &inputs);
            if (!glsl.empty()) shader_compile_request(true, key, std::move(glsl), inputs);
            else LOG("[gx2] vertex %016llx entry %u not requested: empty GLSL, shader.cpp line %d instruction %u\n",
                     (unsigned long long)key, d.entry, shader_glsl_fail_line, shader_glsl_fail_at);
        } else {
            LOG("[gx2] vertex %016llx entry %u not requested: glsl_vs=%u compiler_on=%u\n",
                (unsigned long long)key, d.entry, (u32)glsl_vs, (u32)compiler_on);
        }
        return false;
    }
    VsEntry &vs = it->second;
    if (vs.index == ~0u) return false;
    if (!vs.shader) prepare_vs(vs);
    // a geometry shader's draw (below): an instance a run, of a vertex for every corner it can emit, each of the run's inputs a stream (in
    // fixed-primitive mode, each input of each of a run's vertices). One that would take more than half a ring segment goes the CPU way,
    // before anything is set up for it.
    const u32 run_vertices = d.gs_in & kGsxPrim ? d.gs_in & 0xff : 1;
    if (d.gs) {
        u32 streams = 0;
        for (u32 k = 0; k < run_vertices; k++)
            for (u32 r = 0; r < 16; r++) streams += (vs.inputs >> r & 1) && (k ? vs.vertex_loc[k - 1][r] : vs.location[r]) >= 0;
        if (vs.corner < 0 || streams > 15 || ((u64)(d.count / run_vertices) * streams + 3 * (vs.inputs >> 16)) * 16 > (4u << 20)) {gpu_fallback_why=GPUF_LIMIT;return false;}
    }
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
    bind_targets(ds.rt, &ds);
    if (cur_depth && (ds.depth_test || ds.depth_write || ds.stencil)) take_depth_block(ds.rt);
    lap(P_BIND);
    GX2PixelShader *const ps = find_ps(ds);
    lap(P_PS);
    begin_draw_common(ds, ps, key);
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
    } else if (d.gs) {
        constexpr u32 size = (VS_BLOCK_WORDS + 96 * 4 + 16) * 4;  // and the geometry shader's gf[96] and gInt[4] after uInt (gen_gsx_glsl)
        u32 *u = (u32 *)ring_alloc((size + 255) & ~255u, 256);
        const u32 *src = (const u32 *)g.vs.f, *gsrc = (const u32 *)g.gs.f;
        for (u32 i = 0; i < 96 * 4; i++) u[i] = __builtin_bswap32(src[i]);
        u[384] = swap_float(ds.depth_scale);
        u[385] = swap_float(ds.depth_offset);
        u[386] = u[387] = 0;
        put_vs_ints(u);
        u32 *const gu = u + VS_BLOCK_WORDS;
        for (u32 i = 0; i < 96 * 4; i++) gu[i] = __builtin_bswap32(gsrc[i]);
        for (u32 n = 0; n < 4; n++)
            for (u32 c = 0; c < 4; c++) gu[384 + n * 4 + c] = __builtin_bswap32((u32)g.gs.i[n][c]);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, size);
        GX2SetVertexUniformBlock(vs_ubo_loc, size, u);
    } else {
        set_vertex_uniforms(ds, g.vs.f);
    }
    lap(P_VSUNI);
    if (d.gs) {
        // A run is an instance: its game vertices' inputs as the CPU read them, one element each, and every corner of every triangle the
        // geometry shader can emit is a vertex of it, saying which triangle and which of its corners it is (gen_gsx_glsl). The index entry's
        // inputs: the input registers in bits 0-15, the triangles a run makes above them (tools/shadergen_main.cpp). Sizes checked above.
        const u32 runs = d.count / run_vertices, per = 3 * (vs.inputs >> 16);
        if (!runs) return true;  // nothing to run: no draw of no instances
        s32 src_of[16];
        std::fill(src_of, src_of + 16, -1);
        for (u32 a = 0; a <= d.in_maxi; a++)  // the last attribute a register is given wins, as in load_inputs; past the draw's attributes, zero
            src_of[(d.in_map >> (4 * a)) & 0xf] = a < d.num_attr ? (s32)a : -1;
        FetchKey fk;
        fk.count = 0;
        u32 buf = 0;
        auto bind = [&](s32 location, V4 *c, u32 elements, bool instance) {
            DCFlushRange(c, elements * 16);
            GX2SetAttribBuffer(buf, elements * 16, 16, c);
            fk.s[fk.count++] = float4_stream(location, buf++, 0, instance);
        };
        for (u32 j = 0; j < run_vertices; j++)
            for (u32 r = 0; r < 16; r++) {
                const s32 location = j ? vs.vertex_loc[j - 1][r] : vs.location[r];
                if (!(vs.inputs & (1u << r)) || location < 0) continue;
                V4 *c = (V4 *)ring_alloc(runs * 16 + 64, 64);
                for (u32 i = 0; i < runs; i++) c[i] = src_of[r] >= 0 ? d.points[(size_t)src_of[r] * d.count + i * run_vertices + j] : V4{};
                bind(location, c, runs, true);
            }
        V4 *c = (V4 *)ring_alloc(per * 16 + 64, 64);
        for (u32 k = 0; k < per; k++) c[k] = V4{{(float)(k / 3), (float)(k % 3), 0, 1}};
        bind(vs.corner, c, per, false);
        lap(P_ATTR);
        GX2SetFetchShader(get_fetch(fk));
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, per, 0, runs);
        lap(P_DRAW);
        st_draws++;
        return true;
    }

    // the range of vertices used, to make sure the GPU sees them, and the indices as it reads them
    if (!draw_count) return true;  // no vertices
    u32 minv = d.first, maxv = d.first + d.count - 1;
    const void *gpu_ip = ip;
    if (d.indexed && d.outline_color) {  // an outline's list, in the ring already
        minv = 0xffff;
        maxv = 0;
        for (u32 i = 0; i < draw_count; i++) {
            u16 v;
            memcpy(&v, ip + 2 * i, 2);
            v = le16(v);
            if (v < minv) minv = v;
            if (v > maxv) maxv = v;
        }
        DCFlushRange((void *)ip, draw_count * 2);
    } else if (d.indexed) {
        gpu_ip = draw_indices(d.index_addr, ip, draw_count, index_u16, minv, maxv);
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
    if (!immediate) for (u32 r = 0; r < 16; r++) {
        if (!(vs.inputs & (1u << r)) || vs.location[r] < 0 || reg_attr[r] < 0) continue;
        const Gx2Attr &at = d.attr[reg_attr[r]];
        if (!at.is_default && at.elems && at.stride && gpu_fetched(at)) reg_array[r] = (u8)join_vertex_array(arrays, n_arrays, at);
    }
    s32 array_buf[16];  // the attribute buffer each array's copy is bound as, once an attribute has asked for it
    std::fill(array_buf, array_buf + 16, -1);
    for (u32 r = 0; r < 16; r++) {
        if (!(vs.inputs & (1u << r)) || vs.location[r] < 0) continue;
        const s32 a = reg_attr[r];
        if (immediate) {
            V4 *values = (V4 *)ring_alloc(d.count * sizeof(V4), 64);
            for (u32 vertex = 0; vertex < d.count; vertex++) values[vertex] = a >= 0 ? immediate[vertex * 16 + a] : V4{};
            DCFlushRange(values, d.count * sizeof(V4));
            GX2SetAttribBuffer(buf, d.count * sizeof(V4), sizeof(V4), values);
            fk.s[fk.count++] = float4_stream(vs.location[r], buf++, 0, false);
            continue;
        }
        const Gx2Attr *at = a >= 0 ? &d.attr[a] : nullptr;
        const bool constant = !at || at->is_default || at->elems == 0 || at->stride == 0;
        if (constant) {
            static const float k_zero[4] = {0, 0, 0, 0}, k_unit[4] = {0, 0, 0, 1};
            const float *v = !at ? (memcmp(g.default_attr[r].v, k_zero, 16) ? g.default_attr[r].v : k_unit)
                                 : at->is_default ? g.default_attr[a].v : k_unit;
            GX2SetAttribBuffer(buf, 16, 16, constant_vertex(v));
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
                if (cpu_timing_enabled()) {  // for the [prof] lines
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
            if (cpu_timing_enabled()) {  // for the [prof] lines
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
            if (cpu_timing_enabled()) {  // for the [prof] lines
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
    if (d.indexed)
        GX2DrawIndexedEx(mode, draw_count, index_u16 ? GX2_INDEX_TYPE_U16_LE : GX2_INDEX_TYPE_U16, (void *)gpu_ip, 0, 1);
    else
        GX2DrawEx(mode, d.count, d.first, 1);
    lap(P_DRAW);
    st_draws++;
    if (immediate) st_immediate_draws++;
    return true;
}

bool gx2_draw(const Gx2Draw &d) { return gx2_draw(d, nullptr); }

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
    const u32 bytes = (u32)mb.cverts.size() * sizeof(OutVertex), nidx = (u32)mb.idx.size();
    // A batch too big for a third of the ring (a geometry shader's output shaded here: with its loop, Luigi's Mansion 2's rain made 33 MB in
    // one draw) goes in parts, below.
    const bool whole = bytes + 128 <= RING_SIZE / 3 && nidx * 4 + 128 <= RING_SIZE / 3;
    u32 *v = nullptr, *ix = nullptr;
    if (whole) {
        v = (u32 *)ring_alloc(bytes + 64, 64);
        memcpy(v, mb.cverts.data(), bytes);
        DCFlushRange(v, bytes);
        ix = (u32 *)ring_alloc(nidx * 4 + 64, 64);
        for (u32 i = 0; i < nidx; i++) ix[i] = mb.idx[i] & 0x7fffffffu;
        DCFlushRange(ix, nidx * 4);
    }
    static GX2FetchShader *fetch;
    if (!fetch) {
        FetchKey k{};
        static const u32 offs[7] = {0, 4 * 4, 8 * 4, 12 * 4, 22 * 4, 18 * 4, 16 * 4};
        for (u32 i = 0; i < 7; i++) k.s[k.count++] = float4_stream(i, 0, offs[i], false);
        fetch = get_fetch(k);
    }
    bind_targets(rt, mb.segs[0].ds);
    for (const MSeg &seg : mb.segs)
        if (cur_depth && (seg.ds->depth_test || seg.ds->depth_write || seg.ds->stencil)) {
            take_depth_block(rt);
            break;
        }
    GX2SetVertexShader(vs_passthrough);
    GX2SetFetchShader(fetch);
    if (whole) GX2SetAttribBuffer(0, bytes, sizeof(OutVertex), v);
    static const V4 k_none[96] = {};
    // A part: a run of a segment's triangles with just the vertices they use, renumbered, PART_VERTICES at most (and twice as many
    // triangles, for meshes that share vertices). Each part sets its draw's state and uniforms again, so all a draw reads lies in the ring's
    // segment it was submitted from or the one before (ring_next_segment).
    constexpr u32 PART_VERTICES = 32768;
    static std::vector<u32> part_of;  // a batch vertex's number in the part, ~0u if it has none
    std::vector<u32> used, pidx;
    if (!whole && part_of.size() < mb.cverts.size()) part_of.resize(mb.cverts.size(), ~0u);
    for (const MSeg &seg : mb.segs) {
        const DrawState &ds = *seg.ds;
        if (ds.skip || ds.shadow_mode || ds.gas) continue;
        if (whole) {
            lap_start();
            begin_draw_common(ds, find_ps(ds));
            set_vertex_uniforms(ds, k_none);
            GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, seg.count, GX2_INDEX_TYPE_U32, ix + seg.first, 0, 1);
            st_cpu_draws++;
            continue;
        }
        for (u32 t = seg.first, end = seg.first + seg.count; t < end;) {
            used.clear();
            pidx.clear();
            for (; t < end && used.size() + 3 <= PART_VERTICES && pidx.size() + 3 <= 6 * PART_VERTICES; t += 3)
                for (u32 k = 0; k < 3; k++) {
                    const u32 i = mb.idx[t + k] & 0x7fffffffu;
                    if (part_of[i] == ~0u) {
                        part_of[i] = (u32)used.size();
                        used.push_back(i);
                    }
                    pidx.push_back(part_of[i]);
                }
            auto *pv = (OutVertex *)ring_alloc((u32)used.size() * sizeof(OutVertex) + 64, 64);
            for (u32 k = 0; k < used.size(); k++) {
                pv[k] = mb.cverts[used[k]];
                part_of[used[k]] = ~0u;
            }
            DCFlushRange(pv, (u32)used.size() * sizeof(OutVertex));
            auto *pi = (u32 *)ring_alloc((u32)pidx.size() * 4 + 64, 64);
            memcpy(pi, pidx.data(), pidx.size() * 4);
            DCFlushRange(pi, (u32)pidx.size() * 4);
            lap_start();
            begin_draw_common(ds, find_ps(ds));
            set_vertex_uniforms(ds, k_none);
            GX2SetAttribBuffer(0, (u32)used.size() * sizeof(OutVertex), sizeof(OutVertex), pv);
            GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, (u32)pidx.size(), GX2_INDEX_TYPE_U32, pi, 0, 1);
        }
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

// The game's last texture copies and display transfers through guest memory (gpu/transfer.cpp), numbered: a top screen the game puts together
// in its LCD buffer itself is shown from there (show_lcd_from_memory). Copies under 4 KB are a few tiles, never a screen's rows, and fills
// don't count: a game may clear an LCD buffer that a transfer fills later.
struct MemoryWrite {
    u32 pa, size, seq;
};
MemoryWrite memory_writes[16];
u32 memory_write_seq;
void gx2_copied_to_memory(u32 paddr, u32 size) {
    if (size < 4096) return;
    memory_write_seq++;
    memory_writes[memory_write_seq % 16] = {paddr, size, memory_write_seq};
}

// CPU or DMA writes the game told us about. Nothing needs the CPU's cache flushed here: what the GPU reads in place is flushed at the draw, and on a
// real CPU flushing a whole heap, as some games ask for after every file, would take tens of milliseconds.
// A geometry shader the CPU runs, once per program and entry, for the next build (tools/pica_to_c.py makes C of it): a "GS  " record among the
// misses, as a "VS  " one but with no tail. tools/misses_to_log.py puts these in a .gsl file of their own.
void gx2_note_gs(u32 entry, const ShaderSetup &s) {
    miss_put("GS  ", 4);
    const u32 hdr[3] = {le32(entry), le32(s.code_size), le32(s.swizzle_size)};
    miss_put(hdr, sizeof hdr);
    for (u32 i = 0; i < s.code_size; i++) {
        const u32 w = le32(s.code[i]);
        miss_put(&w, 4);
    }
    for (u32 i = 0; i < s.swizzle_size; i++) {
        const u32 w = le32(s.swizzle[i]);
        miss_put(&w, 4);
    }
}
void metal_guest_written(u32 paddr, u32 size, bool definite) {
    st_flushes++;
    vtx_invalidate(paddr, size);
    memory_fills.written(paddr, size);
    for (auto &s : surfaces)  // a maybe counts here: the game has likely given that VRAM to an ordinary texture
        if (s->copy && s->addr < paddr + size && paddr < s->addr + s->bytes()) s->guest_newer = true;
    if (!definite && size >= 1 << 20) {
        heap_flushes++;
        heap_flush_gap = st_flips - heap_flush_flip;
        heap_flush_flip = st_flips;
        return;
    }
    mark_textures(paddr, size, definite);
    if (definite)
        for (auto &s : surfaces)
            if (surface_readback::overlaps(surface_readback::bounds(s->addr, s->bytes()), surface_readback::bounds(paddr, size)))
                surface_readback::guest_wrote(*s, paddr, size);
}

// ---------------------------------------------------------------- reading the GPU's work back to guest memory
namespace {
bool read_back_pixels(Surface &s, u8 *dst) {
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
    if (!st.image) return false;  // still newer on the GPU: the next read back tries again
    DCInvalidateRange(st.image, st.imageSize);
    if (in_cemu) draw_done();
    GX2CopySurface(&s.cb.surface, 0, 0, &st, 0, 0);
    restore_context();
    draw_done();
    if (notes < 4) wiiu::note("read back %u done", ++notes);
    DCInvalidateRange(st.image, st.imageSize);
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
    }
    free(st.image);
    return true;
}

void write_back(Surface &s, const surface_readback::Spans &owned) {
    if (s.depth || !s.gpu_newer || owned.empty()) return;
    u8 *guest = pptr(s.addr);
    if (!guest) return;
    const bool whole = owned.size() == 1 && owned[0].lo == s.addr && owned[0].hi == (u64)s.addr + s.bytes();
    std::unique_ptr<u8, decltype(&free)> converted(whole ? nullptr : (u8 *)malloc(s.bytes()), &free);
    if (!whole && !converted) return;  // keep GPU ownership if a partial readback cannot be staged
    if (!read_back_pixels(s, whole ? guest : converted.get())) return;
    surface_readback::copy_owned(guest, whole ? guest : converted.get(), s.addr, owned, vtx_invalidate);
    memory_fills.forget(s.addr, s.bytes());
    DCFlushRange(guest, s.bytes());
    s.gpu_newer = false;
    s.written.clear();
}
}  // namespace

// Read every target contributing bytes, keeping newer aliases and later guest writes intact. A target is converted in full so a partial
// request also materializes its other owned bytes before gpu_newer is cleared.
void metal_guest_read(u32 paddr, u32 size) {
    surface_readback::read(surfaces, paddr, size, [](Surface &s, const surface_readback::Spans &owned) {
        used(s);
        write_back(s, owned);
    });
}
void metal_guest_prewrite(u32, u32) {}

// the last transfer was the top screen's left eye, and a right eye has been seen straight after one (see right_eye_transfer)
static bool left_done, right_follows_left;

static bool skip_seen;  // a pass was left undrawn since the last top screen picture (note_top_picture)

// a fill of a whole render target is a clear
bool gx2_memory_fill(u32 start, u32 end, u32 value, u32 control) {
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
    if (any && !other) {
        mark_textures(start, end - start, true);
        return true;  // Nothing reads a render target from guest memory without asking, and filling it there costs real time: New Super Mario Bros. 2 does 2.5 MB a frame
    }
    if (memory_fills.holds(f, pptr(start))) {  // nothing to write, and a texture made from those bytes is current; surfaces there are older than memory, as metal_guest_written would mark them
        for (auto &s : surfaces)
            if (s->addr < end && start < s->addr + s->bytes()) surface_readback::guest_wrote(*s, start, end - start);
        return true;
    }
    if (any) mark_textures(start, end - start, true);
    memory_fills.remember(f);
    return false;
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
    shader_mode();
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
    // In Cemu a copy to a LINEAR_ALIGNED surface lands some draws later (its read-back is scheduled), so a render target came back black
    // or stale; LINEAR_SPECIAL finishes at once once the queue is drained, as write_back does
    st.tileMode = in_cemu ? GX2_TILE_MODE_LINEAR_SPECIAL : GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&st);
    st.image = memalign(st.alignment, st.imageSize);
    if (!st.image) return out;
    memset(st.image, 0, st.imageSize);
    DCFlushRange(st.image, st.imageSize);
    if (in_cemu) draw_done();
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
    if (dyn_force >= 0 || (top_rows <= 720 && !dyn_720)) {  // at 1080p ("top1080"), or when asked: 720p targets fit, and mostly draw at full size
        // Returning from a forced lower level must also restore full size
        // when automatic scaling is disabled at the selected resolution.
        const u32 wanted = dyn_force >= 0 ? (u32)dyn_force : 0;
        if (wanted != dyn_level) {
            if (kMeasure) LOG("[gx2] top screen held at %s size\n", wanted ? "the lower" : "full");
            dyn_level = wanted;
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
#ifdef RECOMP_RESUMABLE
    const bool saving_overlay = checkpoint::saving_to_disk() || wiiu::state_loading_visible();
#else
    const bool saving_overlay = false;
#endif
    const bool chat_overlay = target == GX2_SCAN_TARGET_DRC && wiiu::chat_visible();
    const bool overlay = saving_overlay || chat_overlay;
    static bool visible[2];
    const unsigned screen = target == GX2_SCAN_TARGET_DRC;
    if (overlay != visible[screen]) {
        LOG("[state-overlay] %s %s: %ux%u, backup %s\n", screen ? "GamePad" : "TV",
            overlay ? "show" : "clear", from->surface.width, from->surface.height,
            to.surface.image ? "ready" : "unavailable");
        visible[screen] = overlay;
    }
    if (to.surface.image && overlay) {
        // Use the actual screen target, just like the working options menu.
        // The copy above retains its clean picture. Scan out the overlay, then
        // restore that picture in GPU command order so paused games stay clean.
        // Native GX2CopySurface uses its own rendering state. Rebind our
        // context after that copy, before the menu's small blit setup; Cemu's
        // HLE copy does not reproduce this state change.
        restore_context();
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, from->surface.image, from->surface.imageSize);
        if(chat_overlay) wiiu::draw_chat_on(from);
#ifdef RECOMP_RESUMABLE
        if(saving_overlay) wiiu::draw_state_saving_on(from);
#endif
        GX2CopyColorBufferToScanBuffer(from, target);
        GX2CopySurface(&to.surface, 0, 0, &from->surface, 0, 0);
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, from->surface.image, from->surface.imageSize);
        return;
    }
    GX2CopyColorBufferToScanBuffer(to.surface.image ? &to : from, target);
}

// Check one idle target per present for retirement. Writing pixels back on a timeout can overwrite reused guest memory.
bool transfer_source(const Surface &s);
size_t next_surface_retirement;

void forget_unused() {
    constexpr u64 kForgetAfter = 1800;  // presents: half a minute at 60, a minute at 30
    for (size_t checked = 0; checked < surfaces.size(); ++checked) {
        if (next_surface_retirement >= surfaces.size()) next_surface_retirement = 0;
        auto it = surfaces.begin() + next_surface_retirement++;
        Surface &s = **it;
#ifdef RECOMP_RESUMABLE
        // A live session checkpoint owns this target until replaced/discarded.
        // Idle cache eviction must not invalidate a previously successful save.
        if (s.checkpoint_pins) continue;
#endif
        if (s.last_use + kForgetAfter > use_counter || &s == cur_color || &s == cur_depth || s.in_list || &s == depth_owner || transfer_source(s)) continue;
        if (!surface_readback::retire(s, surfaces)) return;
        // Shared depth descriptors borrow depth_block; retiring one must not free it.
        if (void *image = s.shared_depth ? nullptr : s.image()) {
            if (!s.mem1) free(image);
            else if (GfxHeapFreeMEM1) GfxHeapFreeMEM1(image);
        }
        surfaces.erase(it);
        --next_surface_retirement;
        state_sent_valid = false;
        state_lost = true;
        return;
    }
}

void draw_fps();  // the frame rate counter (below, beside the text it draws)

// Two clock reads per present, independent of the expensive draw timers. Average
// FPS alone hides a movie alternating normal frames and 15 FPS stretches. These
// disjoint gap buckets remain in the PC runtime log without taking screenshots.
static void report_frame_pacing(OSTime began) {
    static OSTime previous, start;
    static u32 buckets[6], count, longest;
    const OSTime now = OSGetSystemTime();
    if (!previous) { previous = start = now; return; }
    const u32 gap = (u32)OSTicksToMicroseconds(now - previous);
    if (gap > 50000) {
        LOG("[pace-gap] gap %u us; before present %u us; present %u us\n", gap,
            (u32)OSTicksToMicroseconds(began - previous), (u32)OSTicksToMicroseconds(now - began));
        if (wiiu::av_timing_enabled()) kernel::gsp_report_frame_gap();
    }
    previous = now;
    longest = std::max(longest, gap);
    const u32 bucket = gap <= 20000 ? 0 : gap <= 35000 ? 1 : gap <= 50000 ? 2 : gap <= 67000 ? 3 : gap <= 100000 ? 4 : 5;
    buckets[bucket]++;
    count++;
    if (now - start < OSSecondsToTicks(2)) return;
    LOG("[pace] %u frames in %u ms; max gap %u us; gaps <=20/35/50/67/100/>100 ms: %u %u %u %u %u %u\n",
        count, (u32)OSTicksToMilliseconds(now - start), longest,
        buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
    start = now;
    count = longest = 0;
    memset(buckets, 0, sizeof buckets);
}

static OSTime renderer_started;
void present() {
    const OSTime pacing_begin = OSGetSystemTime();
    if (!renderer_started) renderer_started = pacing_begin;
    const OSTime tp = cpu_timing_enabled() ? pacing_begin : 0;
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
        ++ps_fallback_epoch;
        ps_last_ds = nullptr;
    }
    if (g_settings.show_fps) draw_fps();
    to_screen(tv_cb, GX2_SCAN_TARGET_TV);
    to_screen(drc_cb, GX2_SCAN_TARGET_DRC);
    restore_context();
    GX2SwapScanBuffers();
    GX2Flush();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    publish_draw_trace();
    st_flips++;
    if (st_flips == 1) OSReport("[3dii-u startup] first GPU present submitted\n");
    use_counter++;
    turn_budget = TURN_BYTES;
    forget_unused();
    ring_next_segment();
    gt_flip(cur_color ? cur_color->addr : 0);
    dyn_res_update();
    vtx_cache_flip();
    if (kMeasure) {  // how long the last new picture stayed up: a steady game's are all alike (2 at thirty a second), 0 never reached the TV
        static u64 seen;
        static u32 since;
        if (st_top_pictures != seen) {
            st_pace[std::min((u32)g_frame - since, 4u)]++;
            seen = st_top_pictures;
            since = (u32)g_frame;
        }
    }
    prof_add(P_PRESENT, tp);
    report_frame_pacing(pacing_begin);
}

bool screen_fresh;
bool top_shown;  // a display transfer brought the top screen since the last VBlank
PresentRule present_rule;  // when a finished frame is shown (present_rule.h, gfx_pump)

// A game's frames a second, held to what a 3DS gives it. Luigi's Mansion: Dark Moon waits for one VBlank a frame and no more: a 3DS's GPU takes
// longer than that over each frame, so it runs at thirty there, and its cutscenes are timed for that. Here it ran as fast as the console
// managed, 60 in light scenes and 22 in heavy ones (in Cemu 60 to 100): the picture ran ahead of the sound through the intro, by turns fast
// and slow (Senne's console, 2026-11-14). The bottom screen's picture ends a frame, and the GPU thread waits there until the frame's time
// is up, with what it has queued sent to the GPU first; the game waits for the GPU thread as it does for a 3DS's GPU.
// Which games: by program ID, until there's a way to know a game's own rate. "set capfps N" (0: none) from the PC.
u32 cap_fps, cap_game;  // what it's held to now, and what this game is held to when it is (the options menu turns it off and on)
bool has_darkmoon_profile() { return wiiu_render::darkmoon(g_game.program_id); }
void apply_darkmoon_profile() {
    if (!has_darkmoon_profile()) return;
    const auto p = wiiu_render::darkmoon_policy(g_settings.wiiu_render_mode, top_rows);
    cap_game = 30;
    cap_fps = p.fps;
    dyn_low = p.scale;
    dyn_force = p.force;
}
void frame_cap_wait() {
    if (!cap_fps) return;
    static OSTime next;
    const OSTime period = (OSTime)OSSecondsToTicks(1) / cap_fps, now = OSGetSystemTime();
    if (next > now && next - now <= period) {
        GX2Flush();
        OSSleepTicks(next - now);
        next += period;
    } else {
        next = now + period;  // late, or the first: from here
    }
}

u32 st_pictures[2];  // screen compositions since the last report, including multiple transfers to the same screen
// What the TV and the GamePad show (screen_layouts.h): the options menu's TV and GAMEPAD lines change them at once, and settings.ini keeps them.
// A touch reaches the game where the GamePad shows the bottom screen.
volatile int tv_layout, pad_layout;  // the menu's (input thread)
int tv_layout_shown = -1;            // what the TV buffer has been drawn for (GPU thread): another clears it first
using screens::Rect;

void draw_rect(u32 w, u32 h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1);

void show_screen(int which, GX2Texture *tex, bool flip, float v_lo = 0, float v_hi = 1, float u_hi = 1) {
    if (which == 0) top_shown = true, st_top_pictures++;
    else frame_cap_wait();
    st_pictures[which]++;
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, tex->surface.image, tex->surface.imageSize);
    Screen sc;
    sc.tex = tex;
    sc.flip = flip;
    sc.v_lo = v_lo;
    sc.v_hi = v_hi;
    sc.u_hi = u_hi;
    const int layout = tv_layout;
    if (layout != tv_layout_shown) {  // what was beside the pictures before goes
        GX2ClearColor(tv_cb, 0, 0, 0, 1);
        restore_context();
        tv_layout_shown = layout;
    }
    Rect r;
    if (screens::tv_rect(layout, which, tv_cb->surface.width, tv_cb->surface.height, r)) {
        GX2SetColorBuffer(tv_cb, GX2_RENDER_TARGET_0);
        cur_color = nullptr;
        begin_blit();
        draw_screen(sc, r.x, r.y, r.w, r.h);
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, tv_cb->surface.image, tv_cb->surface.imageSize);
    }
    const u32 dw = drc_cb->surface.width, dh = drc_cb->surface.height;
    Rect own;
    if (screens::pad_rect(pad_layout, which, dw, dh, r, own)) {
        GX2SetColorBuffer(drc_cb, GX2_RENDER_TARGET_0);
        cur_color = nullptr;
        begin_blit();
        // around the screen in the part of the GamePad it alone has, the bezel picture, or black: what another layout, the menu or the keyboard
        // left there goes, and never the other screen
        GX2SetViewport(0, 0, (float)dw, (float)dh, 0, 1);
        GX2SetScissor(0, 0, dw, dh);
        GX2SetPixelTexture(pad_bezel.surface.image ? &pad_bezel : &black_tex, 0);
        GX2SetPixelSampler(&blit_sampler, 0);
        auto paint = [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            if (x0 < x1 && y0 < y1) draw_rect(dw, dh, (float)x0, (float)y0, (float)x1, (float)y1, (float)x0 / dw, (float)y0 / dh, (float)x1 / dw, (float)y1 / dh);
        };
        const u32 ox1 = own.x + own.w, oy1 = own.y + own.h, rx1 = r.x + r.w, ry1 = r.y + r.h;
        paint(own.x, own.y, r.x, oy1);  // left of it
        paint(rx1, own.y, ox1, oy1);    // right
        paint(r.x, own.y, rx1, r.y);    // above
        paint(r.x, ry1, rx1, oy1);      // below
        draw_screen(sc, r.x, r.y, r.w, r.h);
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, drc_cb->surface.image, drc_cb->surface.imageSize);
    }
    state_lost = true;
    screen_fresh = true;
    present_rule.drew(which);
}

// ---- the options menu: drawn over whatever the GamePad shows, with a small built-in font
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

// The menu's round shapes, white with their coverage as alpha, smooth at the edges: a disk, whose quarters are the corners of round
// rectangles and whose middle is solid, and an arrow pointing right
GX2Texture disk_tex, arrow_tex;
void make_shape(GX2Texture &t, u32 n, float (*coverage)(float x, float y, float n)) {
    GX2Surface &s = t.surface;
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.width = s.height = n;
    s.depth = s.mipLevels = 1;
    s.format = GX2_SURFACE_FORMAT_UNORM_R8;
    s.aa = GX2_AA_MODE1X;
    s.use = GX2_SURFACE_USE_TEXTURE;
    s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    t.viewNumMips = t.viewNumSlices = 1;
    t.compMap = 0x05050500;  // white, the coverage as alpha
    GX2CalcSurfaceSizeAndAlignment(&s);
    GX2InitTextureRegs(&t);
    s.image = memalign(s.alignment, s.imageSize);
    if (!s.image) return;
    for (u32 y = 0; y < n; y++)
        for (u32 x = 0; x < n; x++) ((u8 *)s.image)[y * s.pitch + x] = (u8)(std::clamp(coverage(x + 0.5f, y + 0.5f, (float)n), 0.0f, 1.0f) * 255);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s.image, s.imageSize);
}
void make_shapes() {
    make_shape(disk_tex, 64, [](float x, float y, float n) { return n / 2 - std::hypot(x - n / 2, y - n / 2); });
    make_shape(arrow_tex, 32, [](float x, float y, float) {  // the triangle (4,4) (28,16) (4,28)
        const float left = x - 4, top = (y - 4) - (x - 4) / 2, bottom = (28 - y) - (x - 4) / 2;
        return std::min({left, top * 0.894f, bottom * 0.894f}) + 0.5f;  // 0.894: 1/|(1, 1/2)|, a distance from the slanted sides
    });
}

// a rectangle with round corners of radius r, in the colour set_blit_colors gave, disk_tex bound
void draw_round_rect(u32 w, u32 h, float x0, float y0, float x1, float y1, float r) {
    r = std::max(0.0f, std::min(r, std::min(x1 - x0, y1 - y0) / 2));
    const float c = 0.5f;  // the disk's middle
    draw_rect(w, h, x0 + r, y0, x1 - r, y1, c, c, c, c);
    draw_rect(w, h, x0, y0 + r, x0 + r, y1 - r, c, c, c, c);
    draw_rect(w, h, x1 - r, y0 + r, x1, y1 - r, c, c, c, c);
    draw_rect(w, h, x0, y0, x0 + r, y0 + r, 0, 0, c, c);
    draw_rect(w, h, x1 - r, y0, x1, y0 + r, c, 0, 1, c);
    draw_rect(w, h, x0, y1 - r, x0 + r, y1, 0, c, c, 1);
    draw_rect(w, h, x1 - r, y1 - r, x1, y1, c, c, 1, 1);
}
// The frame rate counter, the menu's FPS COUNTER: frames shown in the last second, white on black in the top left of the TV and the GamePad
void draw_fps() {
    static OSTime since;
    static u64 flips_then;
    static u32 fps;
    const OSTime now = OSGetSystemTime();
    if (!since || OSTicksToMilliseconds(now - since) >= 1000) {
        if (since) fps = (u32)((st_flips - flips_then) * 1000 / OSTicksToMilliseconds(now - since));
        since = now;
        flips_then = st_flips;
    }
    char text[16];
    snprintf(text, sizeof text, "%u FPS", fps);
    const float wide = 6 * 3 * strlen(text);
    for (GX2ColorBuffer *cb : {tv_cb, drc_cb}) {
        if (cb == drc_cb && wiiu::mii_selector_open()) continue;
        GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
        cur_color = nullptr;
        begin_blit();
        GX2SetViewport(0, 0, (float)cb->surface.width, (float)cb->surface.height, 0, 1);
        GX2SetScissor(0, 0, cb->surface.width, cb->surface.height);
        GX2SetPixelTexture(&black_tex, 0);
        GX2SetPixelSampler(&blit_sampler, 0);
        set_blit_colors(0, 0, 0, 0.02f, 0.02f, 0.02f);
        draw_rect(854, 480, 6, 6, 18 + wide, 35, 0, 0, 1, 1);
        GX2SetPixelTexture(&font_tex, 0);
        GX2SetPixelSampler(&lut_sampler, 0);
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
        set_blit_colors(1, 1, 1, 0, 0, 0);
        draw_text(854, 480, text, 12, 10, 3);
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
    }
    state_lost = true;
}
}  // namespace

void metal_display_transfer(u32, u32, u32, u32, u32) {}

// a display transfer from a render target to a framebuffer is a frame finished: no need for the pixels in guest memory
// the last frame sent to each screen, and the last to a buffer no screen shows, in case the game reads it back (for a photo, say)
struct LastTransfer {
    Surface *src = nullptr;
    u32 in_paddr = 0, out_paddr = 0, in_dims = 0, out_dims = 0, flags = 0, out_size = 0;
} last_transfer[3];

// Opt-in hardware comparison. Disable and relaunch to restore existing targets.
std::atomic<bool> screen_slice_experiment{false};
float screen_slice_scale(u32 in_w, u32 in_h, u32 out_w, u32 out_h, u32 scaling, u32 rows) {
    if (rows != 480 || scaling != 1 || in_w != out_w * 2 || in_h != out_h ||
        (in_w != 240 && in_w != 256) || (in_h != 400 && in_h != 416)) return 0;
    return rows / 240.0f;
}

namespace {
bool transfer_source(const Surface &s) {
    // These three delayed copies still need their source image if the CPU reads the output.
    for (const LastTransfer &t : last_transfer)
        if (t.src == &s) return true;
    return false;
}
}  // namespace

// A transfer between two surfaces the game draws at screen size stays on the GPU, at their higher resolution. Paper Mario draws its 3D at
// double width, scales it down into a buffer, and copies that into the target it draws the 2D on. Through guest memory that would be a
// readback every frame at the original resolution, and Cemu doesn't read the GPU's work back at all.
// So does a picture drawn at its own size that a transfer makes a texture of in VRAM, also at its own size, which a game's code doesn't
// read: Pokémon Ultra Sun halves a small picture of its scene into one every frame, and through guest memory that is a read back, which
// waits for the GPU, and an upload. The texture's bytes aren't in guest memory then: a lookup there of another size or format than the
// transfer's reads what was there before, and what the game puts there later counts as newer (Surface::copy).
static bool gpu_copy_transfer(u32 in_paddr, u32 out_paddr, u32 in_dims, u32 out_dims, u32 flags, Surface **copied = nullptr) {
    gx2_flush_cpu_batch();
    gt_mark(GT_TRANSFER, out_paddr);
    const u32 in_w = in_dims & 0xffff, in_h = in_dims >> 16, out_w0 = out_dims & 0xffff, out_h0 = out_dims >> 16;
    const u32 scaling = (flags >> 24) & 3;
    const u32 out_w = out_w0 >> (scaling != 0), out_h = out_h0 >> (scaling == 2);
    const bool flip = flags & 1, crop = flags & 4;
    float sx, sy;
    rt_scale(out_w, out_h, sx, sy);
    // tiles in, tiles out (0x20 without 0x02): a texture
    const bool own_size = sy <= 1 && (flags & 0x22) == 0x20 && out_paddr >= mem::VRAM_PADDR && out_paddr < mem::VRAM_PADDR + mem::VRAM_SIZE;
    if ((flip && crop) || in_w < out_w0 || in_h < out_h0) return false;
    Surface *src = nullptr;
    // Ever Oasis copies D24S8 as RGBA8 for its focus mask. Guest memory
    // contains the clear value, not the GPU's depth; a CPU transfer therefore
    // blurs the whole scene. Keep this experimental path limited to the
    // observed title/format until stencil-preserving transfers are supported.
    const bool depth_copy = ps_depth_copy && g_game.program_id == 0x00040000001a4800ULL
        && (flags & 0x7722) == 0x20;
    for (auto &x : surfaces)
        if ((!x->depth || (depth_copy && x->fmt == 3)) && x->addr == in_paddr && x->w == in_w && x->h == in_h && !x->guest_newer &&
            (x->sy > 1 || x->gpu_chain || (own_size && x->gpu_newer)))
            src = x.get();
    if (!src) return false;
    used(*src);
    // A small destination still accepts a downsample of an enlarged source.
    // Requiring the source to be 1:1 rejected every such effect pass and
    // forced a GPU readback followed by a CPU conversion and texture upload.
    const bool own_size_copy = own_size && !src->gpu_chain;
    if (sy <= 1 && !src->gpu_chain && !own_size_copy) return false;
    // Neither bit 1 (linear in) nor bit 5 (left tiled): the picture lands in memory row by row, for whatever reads its bytes. Paper Mario's goes on to another
    // transfer here on the GPU; Luigi's Mansion 2's top screen halves go to texture copies into its LCD buffer, which read them back, and tiled they came out black.
    const bool linear_out = !(flags & 0x22);
    Surface *dst = get_surface(out_paddr, out_w, out_h, cbuf_to_pixfmt((flags >> 12) & 7), false, src->gpu_chain ? src : nullptr, nullptr, nullptr, linear_out);
    if (dst == src) return false;
    if (half_ss && screen_slice_experiment.load(std::memory_order_relaxed)) {
        const float scale = screen_slice_scale(src->w, src->h, out_w, out_h, scaling, top_rows);
        if (scale && dst->base_sx >= scale && dst->base_sy >= scale &&
            (dst->base_sx > scale || dst->base_sy > scale)) {
            // Shrink only: the original allocation remains large enough. The
            // transfer below rewrites its picture; clear padding before it does.
            GX2DrawDone();
            dst->base_sx = dst->base_sy = scale;
            apply_scale(*dst);
            dst->first_use = true;
            state_lost = true;
            LOG("[gx2] screen-slice experiment: %08x %ux%u at %.1fx selected scale\n", out_paddr, out_w, out_h, scale);
        }
    }
    dst->gpu_chain |= src->gpu_chain;
    dst->copy = own_size_copy;
    dst->linear = linear_out;
    take_dyn_level(*dst);
    static u32 notes;
    if (gxt()) LOG("[gxt] gpu copy %08x -> %08x, %08x %08x flags %08x\n", in_paddr, out_paddr, in_dims, out_dims, flags);
    if (notes++ < 2) wiiu::note("copying surface %08x to %08x on the GPU", in_paddr, out_paddr);
    st_gpu_copies++;
    gx2_flush_cpu_batch();
    // The copied window can be shorter than its power-of-two texture.
    // Initialize the untouched rows too: real MEM2 allocations are not zeroed,
    // and Ever Oasis's blur/bloom samples otherwise spread stale padding into
    // the foreground. The other GPU texture-copy paths do this as well.
    if (dst->first_use) {
        dst->first_use = false;
        GX2ClearColor(&dst->cb, 0, 0, 0, 0);
        restore_context();
    }
    flush_surface(*src);
    if (!src->depth) GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, src->tex.surface.image, src->tex.surface.imageSize);
    GX2SetColorBuffer(&dst->cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2Texture depth_tex;
    if (src->depth) {
        memset(&depth_tex, 0, sizeof depth_tex);
        depth_tex.surface = src->db.surface;
        depth_tex.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_DEPTH_BUFFER | GX2_SURFACE_USE_TEXTURE);
        depth_tex.viewNumMips = depth_tex.viewNumSlices = 1;
        depth_tex.compMap = 0x00010203;
        GX2InitTextureRegs(&depth_tex);
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, depth_tex.surface.image, depth_tex.surface.imageSize);
        GX2SetPixelShader(ps_depth_copy);
        auto *u = (u32 *)ring_alloc(256, 256);
        memset(u, 0, 256);
        u[0] = swap_float(scaling ? 0.5f / in_w : 0.0f);
        u[1] = swap_float(scaling == 2 ? 0.5f / src->tex_h : 0.0f);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), u, 32);
        GX2SetPixelUniformBlock(0, 32, u);
    }
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
    GX2SetPixelTexture(src->depth ? &depth_tex : &src->tex, 0);
    GX2SetPixelSampler(src->depth ? &lut_sampler : &blit_sampler, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
    state_lost = true;
    dst->gpu_newer = true;
    dst->drawn = ++gpu_writes;
    dst->guest_newer = false;
    dst->written_since_flush = true;
    if (copied) *copied = dst;
    note_downscaled(*src, *dst, scaling);  // from its next frame on
    return true;
}

// The top screen's LCD buffers lately (show_lcd_from_memory notes them at each VBlank), and how their rows are laid out
u32 recent_top[4], recent_top_stride, recent_top_bpp, recent_top_fmt;

// Luigi's Mansion 2 puts its top screen together with the copy engine, from linear pictures its transfers make (gpu_copy_transfer): each third
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
    // what the GPU wrote there last, if that is a linear picture the game hasn't written over since
    Surface *src = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->addr <= in_paddr && in_paddr < s->addr + s->bytes() && (!src || s->drawn > src->drawn)) src = s.get();
    if (!src || !src->linear || src->guest_newer) return false;
    used(*src);
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
    // a copy starting up to 33 rows before an LCD buffer is that buffer's: Luigi's Mansion 2's start 16 before it, its pictures being 416 rows
    if (recent_top_stride == out_stride && recent_top_bpp == bpp)
        for (u32 pa : recent_top)
            if (pa && out_paddr < pa + 400 * out_stride && out_paddr + rows * out_stride > pa && pa - out_paddr <= 33 * out_stride) lcd = pa;
    if (lcd) {
        const u32 back = out_paddr < lcd ? (lcd - out_paddr + out_stride - 1) / out_stride : 0;
        base = lcd - back * out_stride;
        height = back + 400;
    } else {
        for (auto &s : surfaces)
            if (!s->depth && s->linear && s.get() != src && s->w * bpp == out_stride && s->addr <= out_paddr &&
                out_paddr + (rows - 1) * out_stride < s->addr + s->bytes() && (!dst || s->drawn > dst->drawn))
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
    used(*dst);
    take_dyn_level(*dst);
    const auto dx = gx2_copy::span(first_col, cols, dst->sx), dy = gx2_copy::span(first_row, rows, dst->sy);
    const auto sx = gx2_copy::span(src_col0, cols, src->sx), sy = gx2_copy::span(src_row0, rows, src->sy);
    if (!dx.size() || !dy.size() || !sx.size() || !sy.size()) return false;
    static u32 notes;
    if (notes++ < 4) wiiu::note("copying rows on the GPU: %08x into %08x, %u columns from %u", in_paddr, base, cols, first_col);
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
    const u32 x0 = dx.first, y0 = dst->row0 + dy.first, w = dx.size(), h = dy.size();
    GX2SetViewport((float)x0, (float)y0, (float)w, (float)h, 0, 1);
    GX2SetScissor(x0, y0, w, h);
    float *q = (float *)ring_alloc(4 * 16 + 64, 64);
    const float u0 = (float)sx.first / src->sw, u1 = (float)sx.last / src->sw;
    const float v0 = (float)(src->row0 + sy.first) / src->sh, v1 = (float)(src->row0 + sy.last) / src->sh;
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
// So is a copy of the whole target as it lies, with no gaps, into VRAM, when the target has a texture's height: a texture of the target's
// own size, which Pokémon Ultra Sun makes of its screen every frame of a battle's opening. What the game later says it wrote there counts
// as newer (Surface::copy).
// A run of whole tiles from inside a target into a VRAM texture as wide, no gaps, starting partway along a row of tiles: Mario Kart 7 copies
// N64 Luigi Raceway's big screen out of the target it drew it in, 1024 tiles from 351 tiles in, every frame. Through guest memory that was
// a read back, which waits for the GPU, and a 128 KB upload, and the race ran at 20-30 fps in Cemu. Tile i of the run is tile i of the
// texture, so the texture's rows of tiles are the target's from where the run starts, wrapping round a row: two rectangles, drawn on the
// GPU at the target's scale as a whole copy is.
static bool tile_run_copy(u32 in_paddr, u32 out_paddr, u32 size, u32 in_width_gap, u32 out_width_gap) {
    if ((in_width_gap >> 16) || (out_width_gap >> 16) || out_paddr < mem::VRAM_PADDR || out_paddr >= mem::VRAM_PADDR + mem::VRAM_SIZE) return false;
    Surface *src = nullptr;
    for (auto &x : surfaces)
        if (!x->depth && x->gpu_newer && !x->guest_newer && x->sy > 1 && in_paddr >= x->addr && in_paddr + size <= x->addr + x->bytes()) src = x.get();
    if (!src || src->w % 8) return false;
    used(*src);
    const u32 tile = src->bytes() / (src->w * src->h) * 64, per_row = src->w / 8, offset = in_paddr - src->addr;
    if (offset % tile || size % (tile * per_row)) return false;
    const u32 first = offset / tile, q = first / per_row, m = first % per_row, w = src->w, h = size / (tile * per_row) * 8;
    if ((h & (h - 1)) || h > 1024) return false;  // a texture's height
    Surface *dst = get_surface(out_paddr, w, h, src->fmt, false, src);
    if (dst == src) return false;
    take_dyn_level(*dst);
    if (gxt()) LOG("[gxt] tile run on the GPU %08x -> %08x, %u tiles from tile %u\n", in_paddr, out_paddr, size / tile, first);
    static u32 notes;
    if (notes++ < 2) wiiu::note("copying tiles of surface %08x into texture %08x on the GPU", src->addr, out_paddr);
    st_gpu_copies++;
    gx2_flush_cpu_batch();
    dst->gpu_chain = dst->copy = true;
    flush_surface(*src);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, src->tex.surface.image, src->tex.surface.imageSize);
    GX2SetColorBuffer(&dst->cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetPixelTexture(&src->tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    // columns dx0-dx1 of the texture's rows from columns sx0-sx1 of the target's rows from sy0 on, in pixels of memory; as in gx2_texture_copy,
    // a surface's rows run bottom up from its first row in memory
    const auto blit = [&](u32 dx0, u32 dx1, u32 sx0, u32 sx1, u32 sy0) {
        if (dx1 <= dx0) return;
        const u32 x = (u32)(dx0 * dst->sx), y = dst->sh - (u32)(h * dst->sy), cw = (u32)((dx1 - dx0) * dst->sx), ch = (u32)(h * dst->sy);
        GX2SetViewport((float)x, (float)y, (float)cw, (float)ch, 0, 1);
        GX2SetScissor(x, y, cw, ch);
        const float u0 = (float)sx0 / src->w, u1 = (float)sx1 / src->w;
        const float v_top = (float)(src->tex_h - (sy0 + h)) / src->tex_h, v_bottom = (float)(src->tex_h - sy0) / src->tex_h;
        float *quad = (float *)ring_alloc(4 * 16 + 64, 64);
        const float data[16] = {-1, 1, u0, v_top, 1, 1, u1, v_top, -1, -1, u0, v_bottom, 1, -1, u1, v_bottom};
        memcpy(quad, data, sizeof data);
        DCFlushRange(quad, sizeof data);
        GX2SetAttribBuffer(0, sizeof data, 16, quad);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
    };
    blit(0, w - 8 * m, 8 * m, w, 8 * q);            // the rest of the row of tiles the run starts in, and the same columns of the rows after
    blit(w - 8 * m, w, 0, 8 * m, 8 * (q + 1));      // the start of each next row, which the run wraps round to
    state_lost = true;
    dst->gpu_newer = true;
    dst->guest_newer = false;
    dst->written_since_flush = true;
    dst->drawn = ++gpu_writes;
    return true;
}

bool gx2_texture_copy(u32 in_paddr, u32 out_paddr, u32 size, u32 in_width_gap, u32 out_width_gap) {
    if (linear_copy_on_gpu(in_paddr, out_paddr, size, in_width_gap, out_width_gap)) return true;
    Surface *src = find_surface(in_paddr, false, [&](const Surface &t) { return t.gpu_newer && !t.guest_newer && t.sy > 1 && size == t.bytes(); });
    if (!src) return tile_run_copy(in_paddr, out_paddr, size, in_width_gap, out_width_gap);
    const u32 bpp = src->bytes() / (src->w * src->h), tile_row = src->w * 8 * bpp;
    const u32 in_w = (in_width_gap & 0xffff) * 16, in_gap = (in_width_gap >> 16) * 16;
    const u32 out_w = (out_width_gap & 0xffff) * 16, out_gap = (out_width_gap >> 16) * 16;
    // A complete tiled framebuffer copy keeps its logical size, including non-power-of-two LCD targets.
    const bool whole = !in_gap && !out_gap && !src->linear && out_paddr >= mem::VRAM_PADDR &&
                       (u64)out_paddr + size <= mem::VRAM_PADDR + mem::VRAM_SIZE;
    if (whole) {
        const auto source = surface_readback::bounds(in_paddr, size);
        for (const auto &alias : surfaces)
            if (!alias->depth && alias->drawn > src->drawn &&
                surface_readback::overlaps(source, surface_readback::bounds(alias->addr, alias->bytes())))
                return false;
    }
    if (!whole && ((in_gap && in_w != tile_row) || out_w != tile_row || !out_gap || (out_w + out_gap) % (8 * bpp))) return false;
    const u32 w = whole ? src->w : (out_w + out_gap) / (8 * bpp);
    u32 h = src->h;
    while (!whole && (h & (h - 1))) h += h & -h;
    if ((!whole && (w & (w - 1))) || w > 1024 || h > 1024) return false;
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
    if (whole) {
        dst->copy = true;
        dst->linear = dst->lcd_bottom = false;
    }
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

// Which screen shows these bytes, or -1, and how far into them its picture starts: New Super Mario Bros. 2 transfers 416 rows and its LCD
// shows the last 400. Games alternate between buffers, so every buffer each screen has shown is remembered. Screen 2 is the top screen's right eye.
// Each screen's last few framebuffers, as a game draws into the ones not shown. When a transfer lands on addresses of more than one screen, the one shown there
// most recently wins: NSMB2's third top buffer, 18300000, is where its bottom screen was during start-up, so every third top picture went to the GamePad
// (the user, 2026-11-08: "the bottom screen flashing in and out with the top showing on it briefly").
// The picture's height rules out the other LCD whatever the addresses say: the top's are 400 rows (NSMB2's 416), the bottom's 320. A Link Between Worlds' second
// top buffer is 18300000 too, and its top kept flashing onto the GamePad on a console (2026-11-09).
// A buffer its screen stopped showing more than 8 swaps ago is no screen's any more (a game turns over two or three buffers): Luigi's Mansion 2's
// top LCD is at 18300000 only at start-up, then each half of its top screen is transferred there and put together in its LCD buffers in FCRAM
// (show_lcd_from_memory). Taken as a screen's, the first half went to the TV as the whole top screen, twice its height.
static int screen_of(u32 paddr, u32 size, u32 rows, u32 &skip) {
    struct Seen {
        u32 pa, when, swaps;  // swaps: its screen's buffer changes so far, when it last showed this one
    };
    static Seen seen[3][4];
    static u32 next[3], clock;
    if (!rows || size < rows || size % rows) return -1;
    const u32 row_bytes = size / rows;
    const auto covers = [&](u32 pa, u32 visible_rows) {
        return rows >= visible_rows && pa >= paddr && (pa - paddr) % row_bytes == 0 &&
               (pa - paddr) / row_bytes <= rows - visible_rows;
    };
    clock++;
    u32 current_bottom = 0;
    const u32 swaps[3] = {kernel::gsp_swaps(0), kernel::gsp_swaps(1), kernel::gsp_swaps(0)};
    for (int s = 0; s < 3; s++) {
        const kernel::FramebufferInfo f = kernel::gsp_framebuffer(s == 2 ? 0 : s);
        const u32 addr = s == 2 && f.addr_right != f.addr_left ? f.addr_right : s == 2 ? 0 : f.addr_left;
        const u32 pa = f.valid && addr ? mem::vaddr_to_paddr(addr) : 0;
        if (!pa) continue;
        if (s == 1) current_bottom = pa;
        Seen *e = nullptr;
        for (Seen &x : seen[s])
            if (x.pa == pa) e = &x;
        if (!e) e = &seen[s][next[s]++ % 4];
        *e = {pa, clock, swaps[s]};
    }
    int found = -1;
    u32 best = 0;
    const bool bottom_rows = rows <= 320;
    for (int s = 0; s < 3; s++) {
        if ((s == 1) != bottom_rows) continue;  // only the screens of the picture's height
        for (const Seen &x : seen[s])
            if (x.pa && swaps[s] - x.swaps <= 8 && covers(x.pa, s == 1 ? 320 : 400) && x.when >= best) {  // a tie goes to the later screen
                found = s;
                best = x.when;
                skip = x.pa - paddr;
            }
    }
    // Some games, including Extreme Butoden, give both LCDs a padded 400-row buffer.
    // With no ordinary match, the current bottom address can identify its 320 visible rows.
    if (found < 0 && !bottom_rows && rows <= 400 && current_bottom && covers(current_bottom, 320)) {
        skip = current_bottom - paddr;
        return 1;
    }
    return found;
}

bool gx2_skip_pass;
static u64 shown_bottom_draw;

static bool packed_bottom(const kernel::FramebufferInfo &f, u32 fmt) {
    return f.valid && fmt == (f.format & 7) && (!f.stride || f.stride == 240 * pixel_bytes(fmt));
}

// New Super Mario Bros. 2 draws a right eye every frame though the slider says 3D is off, and the Wii U couldn't show it: half its draws.
// The pass between a left eye's transfer and the right eye's after it isn't drawn once 30 frames in a row have drawn both eyes, each with
// draws of its own and the left at least a quarter of the right. Metroid: Samus Returns sends a picture to the right LCD and then to the
// left, its scene drawn before the first and next to nothing between them: there the pass after a left eye is the next frame's scene, and
// leaving it undrawn showed the TV runs of frames with nothing in them (2026-11-16). A left eye with next to nothing drawn since the right
// one stops it, as does a pass left undrawn that no right eye follows. Returns true if this transfer needs nothing more.
static bool right_eye_transfer(int screen) {
    static u64 at_left, at_right;  // st_draws at the last left and right eye transfers
    static u32 right_draws;        // the draws of the last right eye drawn
    static u32 stereo;             // frames in a row that drew both eyes in full
    static bool right_seen;        // a right eye straight after the last left one
    const bool was_skipping = gx2_skip_pass;
    gx2_skip_pass = false;
    if (screen == 2) {
        if (left_done && !was_skipping) right_draws = (u32)(st_draws - at_left);
        right_seen = left_done;
        right_follows_left |= left_done;
        at_right = st_draws;
        left_done = false;
        return true;
    }
    if (was_skipping) {
        right_follows_left = false;
        stereo = 0;
    }
    if (screen == 0) {
        const u32 left_draws = (u32)(st_draws - at_right);
        stereo = right_seen && right_draws >= 4 && left_draws >= 4 && left_draws * 4 >= right_draws ? stereo + 1 : 0;
        right_seen = false;
        at_left = st_draws;
    }
    left_done = screen == 0;
    gx2_skip_pass = left_done && right_follows_left && stereo >= 30;
    skip_seen |= gx2_skip_pass;
    return false;
}

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

// (measuring builds) a top screen picture with far fewer draws behind it than the ones before: what a frame missing its scene looks like on
// the TV, with what might have left it out
static void note_top_picture(u32 src, u32 out_paddr, int known_screen) {
    static u64 last_draws;
    static u32 last_src;
    static float usual;
    static u32 told;
    const u32 n = (u32)(st_draws - last_draws);
    last_draws = st_draws;
    const bool again = src == last_src && !n;  // the same picture sent to the other buffer too (Metroid: Samus Returns): nothing missing
    last_src = src;
    if (again) return;
    if (usual > 20 && n < usual / 4 && told++ < 300)
        LOG("[gx2] thin picture at frame %u: %u draws (usually %.0f), %08x to %08x screen %d, pass skipped %d, right eye %d %d\n", (u32)g_frame, n,
            usual, src, out_paddr, known_screen, skip_seen, right_follows_left, left_done);
    usual = usual ? usual * 0.9f + n * 0.1f : (float)n;
    skip_seen = false;
}

bool metal_defer_transfer(u32 in_paddr, u32 out_paddr, u32 in_dims, u32 out_dims, u32 flags, u32 out_size) {
    const u32 in_w = in_dims & 0xffff, in_h = in_dims >> 16, out_w = out_dims & 0xffff, out_h = out_dims >> 16;
    // the output size is given before the transfer's downscale (the flags' bits 24-25), so a 2x wide target has out_w 480 too
    const bool crop = flags & 4, flip = flags & 1;
    const u32 scaling = (flags >> 24) & 3, hs = scaling != 0, vs = scaling == 2;
    const u32 rows = out_h >> vs;
    if (!rows || out_size < rows) return false;
    u32 skip_bytes = 0;
    const int known_screen = screen_of(out_paddr, out_size, rows, skip_bytes);
    if (gxt()) {
        const kernel::FramebufferInfo t = kernel::gsp_framebuffer(0), b = kernel::gsp_framebuffer(1);
        LOG("[gxt] transfer %08x -> %08x in %ux%u out %ux%u flags %08x screen %d (LCDs %08x %08x)\n", in_paddr, out_paddr, in_w, in_h, out_w, out_h, flags, known_screen,
            t.valid ? mem::vaddr_to_paddr(t.addr_left) : 0, b.valid ? mem::vaddr_to_paddr(b.addr_left) : 0);
    }
    // An unregistered LCD-sized destination can be a future framebuffer or a texture the game still needs.
    if (right_eye_transfer(known_screen)) return true;
    // Preserve both GamePad framebuffer images when a game writes more than one between LCD refreshes.
    if (known_screen == 1 && !skip_bytes && !scaling && !(flags & 0x22) && in_dims == out_dims && out_w == 240 && rows == 320 &&
        packed_bottom(kernel::gsp_framebuffer(1), (flags >> 12) & 7)) {
        Surface *dst = nullptr;
        if (gpu_copy_transfer(in_paddr, out_paddr, in_dims, out_dims, flags, &dst)) {
            // A deferred CPU read must not restore an older image over this new destination.
            for (LastTransfer &t : last_transfer)
                if (t.out_paddr == out_paddr && t.out_size <= out_size) t.src = nullptr;
            dst->lcd_bottom = true;
            return true;
        }
    }
    // New Super Mario Bros. 2's framebuffers are 256 wide, and the LCD shows 240 columns of them
    const u32 output_w = out_w >> hs;
    const u32 shown_w = output_w == 256 && (crop || known_screen >= 0) ? 240 << hs : out_w;
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
        if (off % row_bytes || off / row_bytes + out_h > x->h || (s && x->drawn <= s->drawn)) continue;
        s = x.get();
        first_row = off / row_bytes;
    }
    if (!s) return false;
    used(*s);
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
    const u32 source_rows = known_screen >= 0 ? shown_h << vs : shown_h;
    const u32 source_skip = flip && known_screen >= 0 ? ((rows - skip_rows - shown_h) << vs) : skip_rows << vs;
    const u32 start_row = first_row + source_skip;
    if (gxt()) LOG("[gxt] transfer source %08x %ux%u fmt %u draw %llu rows %u+%u, %s%s\n", s->addr, s->w, s->h, s->fmt,
                   (unsigned long long)s->drawn, start_row, source_rows, spare ? "off-screen" : which == 0 ? "TV" : "GamePad",
                   s->guest_newer ? " guest-written" : "");
    if (!spare) {
        if (which == 1) {
            shown_bottom_draw = 0;
            const auto written = surface_readback::bounds(out_paddr, out_size);
            for (auto &surface : surfaces)
                if (surface->lcd_bottom && surface_readback::overlaps(written, surface_readback::bounds(surface->addr, surface->bytes())))
                    surface->lcd_bottom = false;
        }
        show_screen(which, &s->tex, flip, (h - start_row - source_rows) / h, (h - start_row) / h, (float)shown_w / in_w);
    }
    if (kMeasure && !spare && which == 0) note_top_picture(in_paddr, out_paddr, known_screen);
    last_transfer[spare ? 2 : which] = {s, in_paddr, out_paddr, in_dims, out_dims, flags, out_size};
    return true;
}

void gx2_cpu_will_read(u32 paddr, u32 size) {
    for (LastTransfer &t : last_transfer) {
        if (!t.src || !surface_readback::overlaps(surface_readback::bounds(t.out_paddr, t.out_size), surface_readback::bounds(paddr, size))) continue;
        // This is the transfer's source picture, not necessarily the newest view of its old guest address.
        if (t.in_paddr < t.src->addr) continue;
        const u64 offset = (u64)t.in_paddr - t.src->addr;
        const u64 source_bytes = (u64)(t.in_dims & 0xffff) * (t.out_dims >> 16) * pixel_bytes((t.flags >> 8) & 7);
        if (offset + source_bytes > t.src->bytes()) continue;
        std::unique_ptr<u8, decltype(&free)> snapshot((u8 *)malloc(t.src->bytes()), &free);
        if (!snapshot || !read_back_pixels(*t.src, snapshot.get())) continue;
        const u8 *src = snapshot.get() + offset;
        u8 *dst = pptr(t.out_paddr);
        if (src && dst) {
            memory_fills.forget(t.out_paddr, t.out_size);
            transfer_convert(src, dst, t.in_dims, t.out_dims, t.flags);
            DCFlushRange(dst, t.out_size);
            t.src = nullptr;  // The whole copy is now in memory; replaying it would undo newer writes.
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
        write_back(*s, surface_readback::owned(*s, surfaces));
        mark_textures(s->addr, s->bytes(), true);  // other textures in that memory are whatever it holds now
    }
    read_targets.clear();
    GX2Flush();
    if (!++vtx_list) vtx_invalidate(0, ~0u);
}

bool get_display_image_for(u32, u32, u32, const u32 **, int *, int *) { return false; }


#ifdef RECOMP_RESUMABLE
bool gx2_checkpoint(checkpoint::Image &im);
static void release_state_surface(Surface &s) {
    if (!s.shared_depth && s.image()) {
        if (!s.mem1) free(s.image()); else if (GfxHeapFreeMEM1) GfxHeapFreeMEM1(s.image());
    }
}
static void portable_surface_fields(checkpoint::Archive &a, Surface &s) {
    a.fields(s.addr, s.w, s.h, s.fmt, s.depth, s.sx, s.sy, s.base_sx, s.base_sy,
             s.dyn_level, s.dyn_f, s.sw, s.sh, s.tex_h, s.row0, s.gpu_newer, s.guest_newer,
             s.written_since_flush, s.first_use, s.gpu_chain, s.copy, s.written, s.written_after,
             s.drawn, s.last_use, s.linear, s.lcd_bottom, s.clear_pending, s.in_list, s.sampled, s.was_sampled);
}
struct PersistentTargets {
    std::vector<std::unique_ptr<Surface>> owned;
    std::vector<Surface *> pointers;
    bool installed = false;
    ~PersistentTargets() { for (auto &s : owned) release_state_surface(*s); }
};
static bool allocate_state_surface(Surface &s) {
    // Fresh native resources: the file contains no GX2 object layouts/pointers.
    GX2Surface &f = s.depth ? s.db.surface : s.cb.surface;
    f = {}; f.dim = GX2_SURFACE_DIM_TEXTURE_2D; f.width = s.sw; f.height = s.sh;
    f.depth = 1; f.mipLevels = 1; f.aa = GX2_AA_MODE1X; f.tileMode = GX2_TILE_MODE_DEFAULT;
    if (s.depth) {
        const bool sampled = s.fmt == 3 && g_game.program_id == 0x00040000001a4800ULL;
        f.format = sampled ? GX2_SURFACE_FORMAT_FLOAT_X8_X24 : GX2_SURFACE_FORMAT_UNORM_R24_X8;
        f.use = sampled ? (GX2SurfaceUse)(GX2_SURFACE_USE_DEPTH_BUFFER | GX2_SURFACE_USE_TEXTURE) : GX2_SURFACE_USE_DEPTH_BUFFER;
        s.db.viewNumSlices = 1; s.db.depthClear = 1.0f;
    } else {
        f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        f.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
        s.cb.viewNumSlices = 1;
    }
    GX2CalcSurfaceSizeAndAlignment(&f);
    if (!f.imageSize || f.imageSize > 64u << 20) return false;
    f.image = memalign(f.alignment, f.imageSize);
    if (!f.image) return false;
    for_gpu_writes(f.image, f.imageSize);
    s.mem1 = s.shared_depth = false;
    if (s.depth) GX2InitDepthBufferRegs(&s.db);
    else {
        GX2InitColorBufferRegs(&s.cb);
        s.tex = {}; s.tex.surface = f; s.tex.viewNumMips = 1; s.tex.viewNumSlices = 1; s.tex.compMap = 0x00010203;
        GX2InitTextureRegs(&s.tex);
    }
    return true;
}
static bool gx2_persist(checkpoint::Image &im) {
    auto &a = *im.archive;
    gx2_flush_cpu_batch(); GX2DrawDone();
    u32 count = a.reading ? 0 : (u32)surfaces.size(); a.io(count);
    if (count > 256) { a.fail("too many saved GPU surfaces"); return false; }
    auto targets = std::make_shared<PersistentTargets>();
    for (u32 i=0; a.ok && i<count; ++i) {
        auto fresh = std::make_unique<Surface>();
        Surface *p = a.reading ? fresh.get() : surfaces[i].get();
        portable_surface_fields(a, *p);
        if (!a.ok) return false;
        if (!p->w || !p->h || p->w>4096 || p->h>4096 || !p->sw || !p->sh || p->sw>4096 || p->sh>4096 ||
            p->tex_h < p->h || p->fmt>(p->depth ? 3u : 4u) || !std::isfinite(p->sx) || !std::isfinite(p->sy) || p->sx<=0 || p->sy<=0) {
            a.fail("invalid saved GPU surface"); return false;
        }
        if (a.reading) {
            if (!allocate_state_surface(*p)) { a.fail("cannot allocate saved GPU surface"); return false; }
            targets->pointers.push_back(p); targets->owned.push_back(std::move(fresh));
        }
        GX2Surface linear = p->depth ? p->db.surface : p->cb.surface;
        linear.tileMode = in_cemu ? GX2_TILE_MODE_LINEAR_SPECIAL : GX2_TILE_MODE_LINEAR_ALIGNED;
        linear.image = linear.mipmaps = nullptr;
        GX2CalcSurfaceSizeAndAlignment(&linear);
        u32 size = linear.imageSize; a.io(size);
        if (!a.ok || size != linear.imageSize || size>64u<<20) { a.fail("invalid saved GPU image size"); return false; }
        if (!a.reading) {
            linear.image = memalign(linear.alignment,size);
            if (!linear.image) { a.fail("cannot stage saved GPU image"); return false; }
            DCInvalidateRange(linear.image,size);
            GX2CopySurface(p->depth ? &p->db.surface : &p->cb.surface,0,0,&linear,0,0);
            restore_context(); draw_done(); DCInvalidateRange(linear.image,size);
            a.raw(linear.image,size); free(linear.image); linear.image=nullptr;
        } else {
            auto pixels = im.copy_bytes(nullptr,size);
            if (!pixels) return false;
            a.raw(pixels->data,size);
            if (!a.ok) return false;
            auto metadata = std::make_shared<Surface>(*p);
            auto upload = [p, linear, pixels, metadata] {
                // Ownership fields describe the freshly allocated resource, not the old process.
                const u32 pins = p->checkpoint_pins;
                *p = *metadata; p->checkpoint_pins = pins;
                GX2Surface from = linear;
                from.image = memalign(from.alignment,from.imageSize);
                if (!from.image) FATAL("no memory for restored GPU upload\n");
                memcpy(from.image,pixels->data,from.imageSize); DCFlushRange(from.image,from.imageSize);
                GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,from.image,from.imageSize);
                GX2CopySurface(&from,0,0,p->depth ? &p->db.surface : &p->cb.surface,0,0);
                restore_context(); draw_done(); free(from.image);
            };
            upload(); // Allocate/upload before any guest state is changed.
            im.restore.push_back(upload);
            ++p->checkpoint_pins;
            auto pin = std::shared_ptr<Surface>(p,[targets](Surface *surface) { --surface->checkpoint_pins; });
            im.validate.push_back([pin, p, targets, metadata] {
                if (!targets->installed) return true;
                return std::any_of(surfaces.begin(),surfaces.end(),[p,metadata](const auto &s){
                    return s.get()==p && s->checkpoint_identity==metadata->checkpoint_identity &&
                           s->image()==metadata->image() && s->image_size()==metadata->image_size();
                });
            });
        }
    }
    auto index = [&](Surface *p) {
        if (!p) return 0u;
        for (u32 i=0;i<count;++i) if ((a.reading ? targets->pointers[i] : surfaces[i].get())==p) return i+1;
        a.fail("saved target reference is not in the surface table"); return 0u;
    };
    u32 color = a.reading ? 0 : index(cur_color), depth = a.reading ? 0 : index(cur_depth), owner = a.reading ? 0 : index(depth_owner);
    a.fields(color,depth,owner);
    std::vector<u32> reads;
    if (!a.reading) for (auto *p : read_targets) reads.push_back(index(p));
    a.io(reads);
    std::array<LastTransfer,3> transfers{};
    u32 transfer_indices[3]{};
    for (u32 i=0;i<3;++i) {
        if (!a.reading) { transfers[i]=last_transfer[i]; transfer_indices[i]=index(transfers[i].src); }
        auto &t=transfers[i];
        a.fields(transfer_indices[i],t.in_paddr,t.out_paddr,t.in_dims,t.out_dims,t.flags,t.out_size);
    }
    if (color>count || depth>count || owner>count || reads.size()>256) a.fail("invalid GPU target reference");
    for (auto i:reads) if (i>count) a.fail("invalid GPU read-target reference");
    for (auto i:transfer_indices) if (i>count) a.fail("invalid GPU transfer reference");
    im.fields(gpu_writes,use_counter,memory_write_seq);
    im.memory(memory_writes,sizeof memory_writes); // numeric physical addresses/lengths only
    if (a.reading && a.ok) {
        im.restore.push_back([targets,color,depth,owner,reads,transfers,indices=std::array<u32,3>{transfer_indices[0],transfer_indices[1],transfer_indices[2]}] {
            std::set<Surface *> keep(targets->pointers.begin(),targets->pointers.end());
            for (auto it=surfaces.begin();it!=surfaces.end();) {
                if (keep.count(it->get())) {++it;continue;}
                release_state_surface(**it); it=surfaces.erase(it);
            }
            if (!targets->installed) {
                for (auto &s:targets->owned) surfaces.push_back(std::move(s));
                targets->owned.clear(); targets->installed=true;
            }
            auto target=[&](u32 i)->Surface* {return i ? targets->pointers[i-1] : nullptr;};
            cur_color=target(color); cur_depth=target(depth); depth_owner=target(owner);
            read_targets.clear();for(auto i:reads) read_targets.push_back(target(i));
            for(u32 i=0;i<3;++i){last_transfer[i]=transfers[i];last_transfer[i].src=target(indices[i]);}
            memory_fills.standing.clear();memory_fills.notice_due=false; // derived acceleration cache
            vtx_invalidate(0,~0u);
            mark_textures(mem::FCRAM_PADDR,mem::LINEAR_SIZE,true);mark_textures(mem::VRAM_PADDR,mem::VRAM_SIZE,true);
            state_sent_valid=false;state_lost=true;
        });
    }
    if (!a.ok) return false;
    if (!a.reading) {
        auto *stream=im.archive;im.archive=nullptr;
        const bool result=gx2_checkpoint(im);im.archive=stream;
        return result;
    }
    return im.ok;
}

bool gx2_checkpoint(checkpoint::Image &im) {
    if (im.archive) return gx2_persist(im);
    gx2_flush_cpu_batch();
    GX2DrawDone();
    std::set<Surface *> saved_targets;
    for (auto &surface : surfaces) {
        Surface *p = surface.get();
        saved_targets.insert(p);
        const void *image = p->image();
        const u32 bytes = p->image_size();
        const u64 identity = p->checkpoint_identity;
        auto pin = checkpoint_pin(p);
        im.validate.push_back([pin, p, image, bytes, identity] {
            for (auto &s : surfaces) if (s.get() == p) {
                const bool valid = s->checkpoint_identity == identity && s->image() == image && s->image_size() == bytes;
                if (!valid) LOG("[state] validation failed: GPU surface %08x changed identity/image/size\n", (u32)p);
                return valid;
            }
            LOG("[state] validation failed: saved GPU surface %08x was retired\n", (u32)p);
            return false;
        });
        // Surface has no owning destructor; native image ownership stays with
        // the renderer. Validate its lifetime before any restore writes.
        auto metadata = std::make_shared<Surface>(*p);
        im.restore.push_back([p, metadata] { *p = *metadata; });
        if (image && bytes) {
            // Cemu keeps rendered pixels in host GPU textures. Reading its
            // emulated tiled allocation directly can return stale CPU bytes.
            // An explicit linear staging copy works on Cemu and hardware.
            GX2Surface staging = p->depth ? p->db.surface : p->cb.surface;
            staging.tileMode = in_cemu ? GX2_TILE_MODE_LINEAR_SPECIAL : GX2_TILE_MODE_LINEAR_ALIGNED;
            staging.image = nullptr;
            staging.mipmaps = nullptr;
            GX2CalcSurfaceSizeAndAlignment(&staging);
            staging.image = memalign(staging.alignment, staging.imageSize);
            if (!staging.image) return false;
            DCInvalidateRange(staging.image, staging.imageSize);
            GX2CopySurface(p->depth ? &p->db.surface : &p->cb.surface, 0, 0, &staging, 0, 0);
            restore_context(); draw_done();
            DCInvalidateRange(staging.image, staging.imageSize);
            auto pixels = im.copy_bytes(staging.image, staging.imageSize);
            free(staging.image); staging.image = nullptr;
            if (!pixels) return false;
            im.restore.push_back([p, staging, pixels] {
                GX2Surface upload = staging;
                upload.image = memalign(upload.alignment, upload.imageSize);
                if (!upload.image) FATAL("no memory for checkpoint GPU upload\n");
                memcpy(upload.image, pixels->data, upload.imageSize);
                DCFlushRange(upload.image, upload.imageSize);
                GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, upload.image, upload.imageSize);
                GX2CopySurface(&upload, 0, 0, p->depth ? &p->db.surface : &p->cb.surface, 0, 0);
                restore_context(); draw_done();
                free(upload.image);
            });
        }
    }
    im.take(cur_color); im.take(cur_depth); im.take(depth_owner); im.take(read_targets);
    im.take(gpu_writes); im.take(use_counter); im.take(memory_fills);
    im.memory(last_transfer, sizeof last_transfer);
    im.memory(memory_writes, sizeof memory_writes); im.take(memory_write_seq);
    im.restore.push_back([saved_targets] {
        for (auto it = surfaces.begin(); it != surfaces.end();) {
            if (saved_targets.count(it->get())) { ++it; continue; }
            Surface &s = **it;
            if (!s.shared_depth && s.image()) {
                if (!s.mem1) free(s.image()); else if (GfxHeapFreeMEM1) GfxHeapFreeMEM1(s.image());
            }
            it = surfaces.erase(it);
        }
        vtx_invalidate(0, ~0u);
        mark_textures(mem::FCRAM_PADDR, mem::LINEAR_SIZE, true);
        mark_textures(mem::VRAM_PADDR, mem::VRAM_SIZE, true);
        state_sent_valid = false;
    });
    return true;
}
#endif

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
static bool tv_720;  // the TV is sent a 1280x720 picture, which the console scales to the TV's 1080 lines (tv_to_720)

static void tv_buffer_to_mem2() {
    static GX2ColorBuffer in_mem2;
    if (tv_720 || !mem1_relief || !GfxHeapFreeMEM1) return;
    GX2ColorBuffer *whb = WHBGfxGetTVColourBuffer();
    if (!in_mem2.surface.image) {
        void *p = memalign(whb->surface.alignment, whb->surface.imageSize);
        if (!p) return;
        memset(p, 0, whb->surface.imageSize);  // the bars beside the picture are never drawn: black, not what the memory held before
        DCFlushRange(p, whb->surface.imageSize);
        in_mem2 = *whb;
        in_mem2.surface.image = p;
    }
    if (whb->surface.image) GfxHeapFreeMEM1(whb->surface.image);
    whb->surface.image = nullptr;
    tv_cb = &in_mem2;
}

// A TV at 1080 lines is sent a 1280x720 picture, and the console's own scaler makes the 1080 lines: the top screen is drawn at 1200x720
// anyway (top_720), so a 1920x1080 buffer only held it scaled up. Every present copies the TV's buffer twice (to_screen, then to the scan
// buffer), and in Luigi's Mansion: Dark Moon, where that buffer had gone to MEM2, the present took 6.5 ms of each frame on the console's
// GPU (2026-11-13); this has 44% of the pixels, and WHB's 8 MB of fast memory for its 1080 buffer goes to the game's targets. Not with
// "top1080" or "tv1080" (debug flags). Again after the HOME Menu, which takes the foreground memory the scan buffer is in.
static bool tv_to_720() {
    if (!GfxHeapAllocForeground || wiiu::debug_flag("top1080") || wiiu::debug_flag("tv1080")) return false;
    const GX2TVScanMode mode = GX2GetSystemTVScanMode();
    if (mode != GX2_TV_SCAN_MODE_1080I && mode != GX2_TV_SCAN_MODE_1080P) return false;
    uint32_t size = 0, unk = 0;
    GX2CalcTVSize(GX2_TV_RENDER_MODE_WIDE_720P, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &size, &unk);
    void *scan = GfxHeapAllocForeground(size, 0x1000);
    if (!scan) return false;
    static GX2ColorBuffer cb;
    if (!cb.surface.image) {
        GX2Surface &f = cb.surface;
        f.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        f.width = 1280;
        f.height = 720;
        f.depth = 1;
        f.mipLevels = 1;
        f.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        f.aa = GX2_AA_MODE1X;
        f.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
        f.tileMode = GX2_TILE_MODE_DEFAULT;
        cb.viewNumSlices = 1;
        GX2CalcSurfaceSizeAndAlignment(&f);
        GX2InitColorBufferRegs(&cb);
        f.image = memalign(f.alignment, f.imageSize);
        if (!f.image) return false;
        memset(f.image, 0, f.imageSize);
        DCFlushRange(f.image, f.imageSize);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, scan, size);
    GX2SetTVBuffer(scan, size, GX2_TV_RENDER_MODE_WIDE_720P, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
    GX2SetTVScale(1280, 720);
    GX2ColorBuffer *whb = WHBGfxGetTVColourBuffer();  // nothing draws into it now: WHB frees only what isn't null
    if (whb->surface.image && GfxHeapFreeMEM1) GfxHeapFreeMEM1(whb->surface.image);
    whb->surface.image = nullptr;
    tv_cb = &cb;
    static bool told;
    if (!told) LOG("[gx2] the TV is sent 1280x720, which the console scales to 1080 lines\n");
    told = true;
    return true;
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
        GX2Surface storage = depth_storage_layout(*s);
        s->shared_depth = share_depth_block(storage);
        f.image = s->shared_depth ? storage.image : alloc_surface_mem(storage.imageSize, storage.alignment, s->mem1, false);
        s->clear_pending = true;
        if (cur_depth == s.get()) GX2SetDepthBuffer(&s->db);
    }
}

void gfx_thread_init() {
    OSReport("[3dii-u startup] initializing GX2\n");
    int_floats.fill();
    WHBGfxInit();
    free_whb_depth();
    ctx = (GX2ContextState *)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    GX2SetupContextStateEx(ctx, TRUE);
    GX2SetContextState(ctx);
    shader_mode();
    GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);
    GX2SetSwapInterval(1);
    ring = (u8 *)memalign(0x1000, RING_SIZE);
    if (!ring) FATAL("out of memory for the GPU ring\n");
    junk(ring, RING_SIZE, 4);
    ring_off = 0;
    ring_constant = nullptr;
    ring_end = RING_SIZE / 3;
    tv_cb = WHBGfxGetTVColourBuffer();
    drc_cb = WHBGfxGetDRCColourBuffer();
    tv_720 = tv_to_720();
    tv_buffer_to_mem2();
    OSReport("[3dii-u startup] loading shader tables\n");
    load_shader_tables();
    OSReport("[3dii-u startup] shader tables loaded\n");
    load_pad_bezel();
    shader_compile_init();  // what the set lacks, the console compiles as it goes
    vs_ubo_loc = vs_passthrough->uniformBlockCount ? vs_passthrough->uniformBlocks[0].offset : 0;
    ps_ubo_loc = ps_blit->uniformBlockCount ? ps_blit->uniformBlocks[0].offset : 0;
    for (u32 i = 0; i < vs_passthrough->uniformBlockCount; i++) LOG("[gx2] vs block %s at %u size %u\n", vs_passthrough->uniformBlocks[i].name, vs_passthrough->uniformBlocks[i].offset, vs_passthrough->uniformBlocks[i].size);
    for (u32 i = 0; i < vs_passthrough->attribVarCount; i++) LOG("[gx2] vs attrib %s at %u\n", vs_passthrough->attribVars[i].name, vs_passthrough->attribVars[i].location);
    for (u32 i = 0; i < ps_blit->uniformBlockCount; i++) LOG("[gx2] ps block %s at %u size %u\n", ps_blit->uniformBlocks[i].name, ps_blit->uniformBlocks[i].offset, ps_blit->uniformBlocks[i].size);
    for (u32 i = 0; i < ps_blit->samplerVarCount; i++) LOG("[gx2] ps sampler %s at %u\n", ps_blit->samplerVars[i].name, ps_blit->samplerVars[i].location);
    make_font();
    make_shapes();
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
    draw_trace_enabled = wiiu::debug_flag("drawtrace");
    if (draw_trace_enabled)
        OSReport("[3dii-u draw] enabled: one frame per %u presents, up to %u distinct states\n",
                 DRAW_TRACE_INTERVAL, DRAW_TRACE_CAPACITY);
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
    tv_layout = std::clamp(g_settings.tv_layout, 0, screens::kTvLayouts - 1);
    pad_layout = std::clamp(g_settings.pad_layout, 0, screens::kPadLayouts - 1);
    top_rows = start_resolution = g_settings.resolution == 240 || g_settings.resolution == 440 || g_settings.resolution == 480 ? g_settings.resolution : 720;  // "top1080" below can still change top_rows
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
        top_rows = 1080;
        LOG("[gx2] top screen drawn at 1800x1080\n");
    }
    if (has_darkmoon_profile()) {
        light_guard_experiment.store(true, std::memory_order_relaxed);
        copy_rect_experiment.store(true, std::memory_order_relaxed);
        apply_darkmoon_profile();
        LOG("[gx2] Dark Moon render mode %d: limit %u, selected scale %.2f\n",
            std::clamp(g_settings.wiiu_render_mode, 0, 2), cap_fps, dyn_low);
    }
    if (wiiu::debug_flag("nocap")) cap_fps = 0;
    if (cap_fps) LOG("[gx2] this game is held to %u frames a second\n", cap_fps);
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

void gfx_report_draw_trace() {
    DrawTraceFrame frame;
    {
        std::lock_guard lock(draw_trace_mutex);
        if (!draw_trace_ready.count)
            return;
        frame = draw_trace_ready;
        draw_trace_ready.count = 0;
    }
    OSReport("[3dii-u draw] frame=%llu states=%u omitted_draws=%u\n",
             (unsigned long long)frame.frame, frame.count, frame.overflow);
    for (u32 i = 0; i < frame.count; ++i) {
        const auto &d = frame.draws[i];
        OSReport("[3dii-u draw] %u vs=%016llx fs=%016llx ps=%08x rt=%08x/%08x %ux%u depth=%08x/%08x flags=%06x blend=%06x mask=%08x\n",
                 i, (unsigned long long)d.vs, (unsigned long long)d.fs, d.shader,
                 d.color, d.depth, d.width, d.height, d.depth_scale, d.depth_offset,
                 d.flags, d.blend, d.color_mask);
        OSReport("[3dii-u draw] %u viewport=%.1f,%.1f,%.1f,%.1f tex0=%08x/%ux%u/%u tex1=%08x/%ux%u/%u tex2=%08x/%ux%u/%u\n",
                 i, d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3],
                 d.textures[0], d.textures[1], d.textures[2], d.textures[3],
                 d.textures[4], d.textures[5], d.textures[6], d.textures[7],
                 d.textures[8], d.textures[9], d.textures[10], d.textures[11]);
    }
}

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
// A game that's held to a frame rate (cap_game) has a line to let it run free, between the volume and quitting: its cutscenes are timed for
// that rate, the rest of it may look better without (the user, 2026-11-14)
static int menu_items() {
    return (cap_game ? 11 : 10)
#ifdef RECOMP_RESUMABLE
        + 2
#endif
#ifndef RECOMP_CLASSIC_MENU
        + 2
#endif
        ;
}
static volatile bool menu_is_open;
static volatile int menu_item, menu_speed = 100, menu_volume = 100;
static std::atomic<int> debug_menu_request{-1}, debug_speed_request{0};
static volatile bool menu_quit;

// The game's language from its next start, as cfg.cpp will pick it (services/languages.h). settings.ini keeps a choice; the card thread
// writes it (frontend_wiiu.cpp), as this one, the input thread, would wait out any save being written.
static int menu_language() { return kernel::cfg_menu_language(g_settings.language); }
static void next_language(int dir) {
    const u32 offered = kernel::cfg_languages();
    int l = menu_language();
    do l = (l + dir + 12) % 12;
    while (!(offered >> l & 1));
    const int keep = l == kernel::cfg_menu_language(-1) ? -1 : l;
    if (keep == g_settings.language) return;
    g_settings.language = keep;
    wiiu::settings_due = true;
}

// what the TV and the GamePad show (screen_layouts.h): at once, and from the next start too
static void next_tv_layout(int dir) {
    tv_layout = g_settings.tv_layout = screens::next_tv_layout(tv_layout, dir);
    wiiu::settings_due = true;
}
static void next_pad_layout(int dir) {
    pad_layout = g_settings.pad_layout = screens::next_pad_layout(pad_layout, dir);
    wiiu::settings_due = true;
}

// the rows the top screen is drawn at (top_rows), from the next start: the targets already made keep their size
static void next_resolution(int dir) {
    const int choices[] = {240, 440, 480, 720};
    const int *it = std::find(choices, choices + 4, g_settings.resolution);
    const int index = it == choices + 4 ? 3 : it - choices;
    g_settings.resolution = choices[(index + (dir > 0 ? 1 : 3)) % 4];
    wiiu::settings_due = true;
}

static bool menu_allowed = true;
bool menu_open() { return menu_is_open; }
void menu_disable() { menu_allowed = false; }
int game_speed() { return menu_speed; }
bool menu_wants_quit() { return menu_quit; }
void menu_quit_now() { menu_quit = true; }

#ifndef RECOMP_CLASSIC_MENU
static bool menu_restore_confirm;
#endif
void menu_input(u32 pressed, bool toggle) {
#ifndef RECOMP_CLASSIC_MENU
    if (save_backup::requests.view().busy) return;
    if (toggle || (pressed & (VPAD_BUTTON_UP | VPAD_BUTTON_DOWN | VPAD_BUTTON_LEFT | VPAD_BUTTON_RIGHT)))
        menu_restore_confirm = false;
    if (menu_is_open && (pressed & VPAD_BUTTON_B) && menu_restore_confirm) {
        menu_restore_confirm = false;
        return;
    }
#endif
    if (!menu_is_open) {
        if (menu_allowed && toggle) {
            menu_item = 0;
            menu_is_open = true;
        }
        return;
    }
    if (network_menu::active()) {
        if(toggle && !network_keyboard::active()) {network_menu::close();return;}
        network_menu::buttons(pressed);return;
    }
    if (toggle || (pressed & VPAD_BUTTON_B)) { menu_is_open = false; return; }
    const int count = menu_items();
#ifdef RECOMP_CLASSIC_MENU
    const int network_item=count-2;
#else
    const int network_item=count-4;
#endif
    if(menu_item==network_item && (pressed&VPAD_BUTTON_A)) {network_menu::open();return;}
    if (pressed & VPAD_BUTTON_UP) menu_item = (menu_item + count - 1) % count;
    if (pressed & VPAD_BUTTON_DOWN) menu_item = (menu_item + 1) % count;
    const int dir = (pressed & VPAD_BUTTON_RIGHT) ? 1 : (pressed & VPAD_BUTTON_LEFT) ? -1 : 0;
    if (menu_item == 1 && dir) {
        menu_speed = dir > 0 ? (menu_speed >= 400 ? 400 : menu_speed * 2) : (menu_speed <= 100 ? 100 : menu_speed / 2);
        kernel::set_clock_speed(menu_speed);
    }
    if (menu_item == 2 && dir) {
        menu_volume = std::clamp(menu_volume + dir * 10, 0, 100);
        audio::set_volume(menu_volume / 100.0f);
    }
    if (menu_item == 3 && dir) next_language(dir);
    if (menu_item == 4 && dir) next_tv_layout(dir);
    if (menu_item == 5 && dir) next_pad_layout(dir);
    if (menu_item == 6 && dir) next_resolution(dir);
    if (menu_item == 7 && (dir || (pressed & VPAD_BUTTON_A))) {  // the frame rate counter (draw_fps), kept in settings.ini
        g_settings.show_fps = !g_settings.show_fps;
        wiiu::settings_due = true;
    }
    if (cap_game && menu_item == 8 && (dir || (pressed & VPAD_BUTTON_A))) {
        if (has_darkmoon_profile()) {
            g_settings.wiiu_render_mode = (std::clamp(g_settings.wiiu_render_mode, 0, 2) + (dir < 0 ? 2 : 1)) % 3;
            apply_darkmoon_profile();
            wiiu::settings_due = true;
        } else cap_fps = cap_fps ? 0 : cap_game;
    }
#ifdef RECOMP_RESUMABLE
    if (pressed & VPAD_BUTTON_A) {
        const int save_item = cap_game ? 9 : 8;
        if (menu_item == save_item || menu_item == save_item + 1) {
            checkpoint::request(menu_item == save_item ? 4 : 2);
            menu_is_open = false;
        }
    }
#endif
    if (pressed & VPAD_BUTTON_A) {
        if (menu_item == 0) menu_is_open = false;
#ifndef RECOMP_CLASSIC_MENU
        if (menu_item == count - 3) save_backup::requests.request(save_backup::Action::Export);
        if (menu_item == count - 2) {
            if (menu_restore_confirm) {
                save_backup::requests.request(save_backup::Action::StageRestore);
                menu_restore_confirm = false;
            } else menu_restore_confirm = true;
        }
#endif
        if (menu_item == count - 1) menu_quit = true;
    }
}

#ifdef RECOMP_CLASSIC_MENU
// Keep the original left-stick options overlay, including checkpoint controls.
static void draw_menu_on(GX2ColorBuffer *cb) {
    const u32 w = 854, h = 480;
    GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, 0, (float)cb->surface.width, (float)cb->surface.height, 0, 1);
    GX2SetScissor(0, 0, cb->surface.width, cb->surface.height);
    GX2SetPixelTexture(&black_tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    set_blit_colors(0, 0, 0, 0.98f, 0.9f, 0.62f);
    draw_rect(w, h, 127, 60, 727, 470, 0, 0, 1, 1);
    set_blit_colors(0, 0, 0, 0.42f, 0.27f, 0.08f);
    draw_rect(w, h, 127, 60, 727, 104, 0, 0, 1, 1);
    const int count = menu_items();
    const float rowh = count > 12 ? 24 : count > 10 ? 27 : count == 10 ? 33 : 36, top = 114;  // a tenth line (the frame limit): closer together, in the same box
    set_blit_colors(0, 0, 0, 1.0f, 0.45f, 0.0f);
    draw_rect(w, h, 145, top + menu_item * rowh - 4, 709, top + menu_item * rowh + (count > 10 ? 27 : count == 10 ? 32 : 38), 0, 0, 1, 1);
    GX2SetPixelTexture(&font_tex, 0);
    GX2SetPixelSampler(&lut_sampler, 0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    set_blit_colors(1, 1, 1, 0, 0, 0);
    draw_text(w, h, "OPTIONS", 165, 68, 4);
    set_blit_colors(0.2f, 0.1f, 0.0f, 0, 0, 0);
    static const char *const k_languages[12] = {"JAPANESE", "ENGLISH", "FRENCH",  "GERMAN",     "ITALIAN", "SPANISH",
                                                "CHINESE",  "KOREAN",  "DUTCH",   "PORTUGUESE", "RUSSIAN", "CHINESE (TW)"};
    static const char *const k_tv_layouts[screens::kTvLayouts] = {"TOP SCREEN",      "SIDE BY SIDE",  "STACKED",
                                                                  "BIG TOP + SMALL", "BOTTOM SCREEN", "BIG BOTTOM + SMALL", "TOP SCREEN (16:9)"};
    static const char *const k_pad_layouts[screens::kPadLayouts] = {"BOTTOM SCREEN", "TOP SCREEN", "STACKED", "SIDE BY SIDE", "TOP SCREEN (16:9)"};
    char line[48], vol[32], lang[40], tv[40], pad[40], res[32], cap[40];
    snprintf(line, sizeof line, "GAME SPEED: %d%%", menu_speed);
    snprintf(vol, sizeof vol, "VOLUME: %d%%", menu_volume);
    snprintf(lang, sizeof lang, "LANGUAGE: %s", k_languages[menu_language()]);
    snprintf(tv, sizeof tv, "TV: %s", k_tv_layouts[tv_layout % screens::kTvLayouts]);
    snprintf(pad, sizeof pad, "GAMEPAD: %s", k_pad_layouts[pad_layout % screens::kPadLayouts]);
    snprintf(res, sizeof res, "RESOLUTION: %dP", g_settings.resolution);
    if (has_darkmoon_profile()) {
        const char *modes[] = {"BALANCED", "SHARP", "FAST"};
        snprintf(cap, sizeof cap, "MODE: %s", modes[std::clamp(g_settings.wiiu_render_mode, 0, 2)]);
    } else if (cap_fps) snprintf(cap, sizeof cap, "FRAME LIMIT: %u", (unsigned)cap_fps);
    else snprintf(cap, sizeof cap, "FRAME LIMIT: OFF");
    const char *items[16] = {"BACK TO THE GAME", line, vol, lang, tv, pad, res, g_settings.show_fps ? "FPS COUNTER: ON" : "FPS COUNTER: OFF"};
    int item_count = 8;
    if (cap_game) items[item_count++] = cap;
#ifdef RECOMP_RESUMABLE
    items[item_count++] = "SAVE STATE";
    items[item_count++] = checkpoint::has_state() ? "LOAD STATE" : "LOAD STATE (EMPTY)";
#endif
    items[item_count++] = "NETWORK SETTINGS";
    items[item_count++] = "QUIT TO WII U MENU";
    for (int i = 0; i < item_count; i++) {
        draw_text(w, h, items[i], 165, top + i * rowh + 4, count > 10 ? 3 : 4);
    }
    set_blit_colors(0.45f, 0.3f, 0.15f, 0, 0, 0);
    const char *hint = "A: CHOOSE   LEFT/RIGHT: CHANGE   B: BACK";
    if (menu_item == 3 && menu_language() != kernel::cfg_language()) hint = "THE GAME CHANGES LANGUAGE WHEN IT NEXT STARTS";  // a game takes its language as it starts
    if (menu_item == 5 && pad_layout == 1) hint = "TOUCH IS OFF WHILE THE GAMEPAD SHOWS THE TOP";
    if (menu_item == 8 && has_darkmoon_profile()) {
        const char *hints[] = {top_rows == 720 ? "BALANCED: 85% SCALE, 30 FPS LIMIT" : "BALANCED: 30 FPS LIMIT",
                               "SHARP: FULL RESOLUTION, 30 FPS LIMIT", "FAST: UNCAPPED; TIMING MAY CHANGE"};
        hint = hints[std::clamp(g_settings.wiiu_render_mode, 0, 2)];
    }
    if (menu_item == 6 && g_settings.resolution != start_resolution) hint = "RESOLUTION CHANGES WHEN THE GAME NEXT STARTS";  // targets keep their size
#ifdef RECOMP_RESUMABLE
    const int state_item = cap_game ? 9 : 8;
    if (menu_item == state_item || menu_item == state_item + 1)
        if (const char *result = checkpoint::result_message()) hint = result;
#endif
    draw_text(w, h, hint, 165, 446, 2);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
}
#else
static void draw_menu_on(GX2ColorBuffer *cb) {
    const float W = (float)cb->surface.width, H = (float)cb->surface.height, k = H / 480;
    const u32 w = (u32)W, h = (u32)H;
    const auto at = [&](float v) { return std::floor(v * k + 0.5f); };
    const bool font = ui_font::load();
    const auto pixel_scale = [&](float size) { return std::max(1.0f, std::floor(size * k / 8)); };  // the built-in font, without the console's
    const auto text_w = [&](const char *s, float size) { return font ? ui_font::width(s, size * k) : (6.0f * strlen(s) - 1) * pixel_scale(size); };
    const auto text = [&](const char *s, float x, float y, float size, float r, float g, float b) {
        set_blit_colors(r, g, b, 0, 0, 0);
        if (!font) {
            GX2SetPixelTexture(&font_tex, 0);
            GX2SetPixelSampler(&lut_sampler, 0);
            draw_text(w, h, s, std::floor(x), std::floor(y + (size * k - 7 * pixel_scale(size)) / 2), pixel_scale(size));
            return;
        }
        GX2SetPixelTexture(&ui_font::texture(), 0);
        GX2SetPixelSampler(&blit_sampler, 0);
        ui_font::Quad q[64];
        const int n = ui_font::quads(s, size * k, std::floor(x), std::floor(y), q, 64);
        for (int i = 0; i < n; i++) draw_rect(w, h, q[i].x0, q[i].y0, q[i].x1, q[i].y1, q[i].u0, q[i].v0, q[i].u1, q[i].v1);
    };
    const auto paint = [&](float r, float g, float b) {  // round shapes and plain areas after it
        GX2SetPixelTexture(&disk_tex, 0);
        GX2SetPixelSampler(&blit_sampler, 0);
        set_blit_colors(r, g, b, 0, 0, 0);
    };
    const auto fill = [&](float x0, float y0, float x1, float y1) { draw_rect(w, h, x0, y0, x1, y1, 0.5f, 0.5f, 0.5f, 0.5f); };
    const auto arrow = [&](float x, float y, float size, bool left, float r, float g, float b) {
        GX2SetPixelTexture(&arrow_tex, 0);
        GX2SetPixelSampler(&blit_sampler, 0);
        set_blit_colors(r, g, b, 0, 0, 0);
        draw_rect(w, h, x, y, x + size, y + size, left ? 1 : 0, 0, left ? 0 : 1, 1);
    };

    // the lines: what each is, and what it's set to (nothing for the two that act)
    static const char *const k_languages[12] = {"Japanese", "English", "French",  "German",     "Italian", "Spanish",
                                                "Chinese",  "Korean",  "Dutch",   "Portuguese", "Russian", "Chinese (Taiwan)"};
    static const char *const k_tv_layouts[screens::kTvLayouts] = {"Top Screen",      "Side by Side",  "Stacked",           "Big Top + Small",
                                                                  "Bottom Screen",   "Big Bottom + Small", "Top Screen (16:9 stretch)"};
    static const char *const k_pad_layouts[screens::kPadLayouts] = {"Bottom Screen", "Top Screen", "Stacked", "Side by Side", "Top Screen (16:9 stretch)"};
    const int count = menu_items(), quit = count - 1;
    char speed[16], vol[16], res[16], cap[16];
    snprintf(speed, sizeof speed, "%d%%", menu_speed);
    snprintf(vol, sizeof vol, "%d%%", menu_volume);
    snprintf(res, sizeof res, "%dp", g_settings.resolution);
    if (cap_fps) snprintf(cap, sizeof cap, "%u fps", (unsigned)cap_fps);
    else snprintf(cap, sizeof cap, "Off");
    const char *labels[16] = {"Back to Game", "Game Speed", "Volume", "Language", "TV Display", "GamePad Display", "Resolution", "FPS Counter", "Frame Limit"};
    const char *values[16] = {"", speed, vol, k_languages[menu_language()], k_tv_layouts[tv_layout % screens::kTvLayouts],
                              k_pad_layouts[pad_layout % screens::kPadLayouts], res, g_settings.show_fps ? "On" : "Off", cap};
    if (cap_game && has_darkmoon_profile()) {
        const char *modes[] = {"Balanced", "Sharp", "Fast"};
        labels[8] = "Rendering Mode";
        values[8] = modes[std::clamp(g_settings.wiiu_render_mode, 0, 2)];
    }
#ifdef RECOMP_RESUMABLE
    const int state_item = cap_game ? 9 : 8;
    labels[state_item] = "Save State";
    labels[state_item + 1] = checkpoint::has_state() ? "Load State" : "Load State (Empty)";
#endif
    labels[count - 4] = "Network settings";
    labels[count - 3] = "Back up Save to SD Card";
    labels[count - 2] = menu_restore_confirm ? "Confirm Restore Next Start (A)" : "Restore Newest Backup Next Start";
    labels[quit] = "Return to Wii U Menu";
    values[quit] = "";

    GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, 0, W, H, 0, 1);
    GX2SetScissor(0, 0, w, h);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE,
                       GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    // the blue ground, lighter at the top
    for (int b = 0; b < 24; b++) {
        const float f = b / 23.0f;
        paint(0.45f - 0.18f * f, 0.59f - 0.20f * f, 0.86f - 0.16f * f);
        fill(0, std::floor(H * b / 24), W, std::floor(H * (b + 1) / 24));
    }
    // the heading band, its light edge, and the title on a shadow
    for (int b = 0; b < 4; b++) {
        paint(0.17f - 0.015f * b, 0.24f - 0.02f * b, 0.44f - 0.03f * b);
        fill(0, at(14 * b), W, at(14 * (b + 1)));
    }
    paint(0.64f, 0.73f, 0.93f);
    fill(0, at(56), W, at(58));
    const char *title = "3Dii U Menu";
    const float title_x = std::floor((W - text_w(title, 30)) / 2);
    text(title, title_x + at(2), at(11) + at(2), 30, 0.04f, 0.07f, 0.16f);
    text(title, title_x, at(11), 30, 1, 1, 1);
    std::string status = save_backup::requests.view().message;
#ifdef RECOMP_RESUMABLE
    if (const char *result = checkpoint::result_message(); status.empty() && result) status = result;
#endif
    if (menu_restore_confirm) status = "The newest SD backup will replace this save next start. A confirms; B cancels.";
    if (!status.empty()) {
        const bool shorten = text_w(status.c_str(), 11) > W - at(80);
        while (!status.empty() && text_w((status + (shorten ? "..." : "")).c_str(), 11) > W - at(80)) {
            size_t end = status.size() - 1;
            while (end && ((unsigned char)status[end] & 0xc0) == 0x80) --end;
            status.resize(end);
        }
        if (shorten) status += "...";
        text(status.c_str(), (W - text_w(status.c_str(), 11)) / 2, at(43), 11, 1, 1, 1);
    }
    // the dark frame and its tiles
    paint(0.47f, 0.58f, 0.84f);
    draw_round_rect(w, h, at(24), at(68), W - at(24), H - at(56), at(16));
    paint(0.11f, 0.17f, 0.35f);
    draw_round_rect(w, h, at(27), at(71), W - at(27), H - at(59), at(13));
    const float gap = 5, first = 82, room = 480 - 68 - first, tile = (room - (count - 1) * gap) / count;
    const float x0 = at(40), x1 = W - at(40), side = at(16), bar = at(9), bar_gap = at(3), arrow_size = at(11);
    for (int i = 0; i < count; i++) {
        const float y0 = at(first + i * (tile + gap)), y1 = at(first + i * (tile + gap) + tile), mid = (y0 + y1) / 2;
        const bool chosen = i == menu_item, adjustable = i != 0 && i < (cap_game ? 9 : 8);
        if (chosen) {
            paint(0.18f, 0.76f, 1.0f);
            draw_round_rect(w, h, x0 - at(4), y0 - at(4), x1 + at(4), y1 + at(4), at(11));
            paint(1, 1, 1);
            draw_round_rect(w, h, x0, y0, x1, y1, at(7));
        } else {
            paint(0.63f, 0.67f, 0.77f);
            draw_round_rect(w, h, x0, y0, x1, y1, at(7));
            paint(0.93f, 0.94f, 0.96f);
            draw_round_rect(w, h, x0 + at(1.5f), y0 + at(1.5f), x1 - at(1.5f), y1 - at(1.5f), at(6));
        }
        const float ty = mid - 19 * k / 2;
        if (i == quit) text(labels[i], x0 + side, ty, 19, 0.78f, 0.16f, 0.12f);
        else text(labels[i], x0 + side, ty, 19, 0.17f, 0.19f, 0.24f);
        if (!adjustable) continue;
        float end = x1 - side;
        if (chosen) {
            arrow(end - arrow_size, mid - arrow_size / 2, arrow_size, false, 0.10f, 0.60f, 0.92f);
            end -= arrow_size + at(10);
        }
        const float vw = text_w(values[i], 19);
        text(values[i], end - vw, ty, 19, chosen ? 0.04f : 0.33f, chosen ? 0.38f : 0.37f, chosen ? 0.68f : 0.47f);
        float start = end - vw;
        if (i == 2) {  // the volume, as ten blocks before its number
            for (int s = 9; s >= 0; s--) {
                const float bx1 = start - at(12) - (9 - s) * (bar + bar_gap);
                if (s < menu_volume / 10) paint(0.12f, 0.65f, 0.95f);
                else paint(0.78f, 0.80f, 0.86f);
                draw_round_rect(w, h, bx1 - bar, mid - at(7), bx1, mid + at(7), at(2));
            }
            start -= at(12) + 10 * (bar + bar_gap) - bar_gap;
        }
        if (chosen) arrow(start - at(10) - arrow_size, mid - arrow_size / 2, arrow_size, true, 0.10f, 0.60f, 0.92f);
    }
    // the dark bar at the foot: B to close, A to choose, left and right to change
    const float by0 = H - at(46), by1 = H - at(10), bmid = (by0 + by1) / 2, ball = at(24), note = 16;
    const char *const k_words[3] = {"Close", "Choose", "Change"};
    float wide_bar = at(18);
    for (int i = 0; i < 3; i++) wide_bar += ball + at(8) + text_w(k_words[i], note) + at(22);
    paint(0.06f, 0.06f, 0.08f);
    draw_round_rect(w, h, -at(30), by0, wide_bar, by1, (by1 - by0) / 2);
    float x = at(18);
    for (int i = 0; i < 3; i++) {
        if (i < 2) {
            paint(1, 1, 1);
            draw_round_rect(w, h, x, bmid - ball / 2, x + ball, bmid + ball / 2, ball / 2);
            const char *letter = i == 0 ? "B" : "A";
            text(letter, x + (ball - text_w(letter, note)) / 2, bmid - note * k / 2, note, 0.12f, 0.12f, 0.15f);
        } else {
            arrow(x, bmid - ball / 4, ball / 2, true, 1, 1, 1);
            arrow(x + ball / 2, bmid - ball / 4, ball / 2, false, 1, 1, 1);
        }
        x += ball + at(8);
        text(k_words[i], x, bmid - note * k / 2, note, 1, 1, 1);
        x += text_w(k_words[i], note) + at(22);
    }
    // what a change waits for, beside the bar
    const char *hint = nullptr;
    if (menu_item == 3 && menu_language() != kernel::cfg_language()) hint = "The game changes language when it next starts.";  // a game takes its language as it starts
    if (menu_item == 5 && (pad_layout == 1 || pad_layout == 4)) hint = "Touch is off in top-only layouts.";
    if (menu_item == 6 && g_settings.resolution != start_resolution) hint = "The resolution changes when the game next starts.";  // targets keep their size
    if (hint) {
        const float hx = wide_bar + at(16), hy = bmid - note * k / 2;
        text(hint, hx + at(1), hy + at(1), note, 0.04f, 0.07f, 0.16f);
        text(hint, hx, hy, note, 1, 1, 1);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
}
#endif
#ifdef RECOMP_RESUMABLE
static bool loading_state_overlay;
bool state_loading_visible() { return loading_state_overlay; }
void show_state_loading(bool visible) {
    loading_state_overlay=visible;
    gpu::screen_fresh=true;
    if(visible && tv_cb && drc_cb) {
        // Submit before disk I/O blocks this GPU thread. Scanout keeps this
        // immutable copy visible while state reconstruction is paused.
        gpu::present();
        GX2DrawDone();
    }
}
void draw_state_saving_on(GX2ColorBuffer *cb) {
    const u32 w=854,h=480;
    GX2SetColorBuffer(cb,GX2_RENDER_TARGET_0);cur_color=nullptr;begin_blit();
    GX2SetViewport(0,0,(float)cb->surface.width,(float)cb->surface.height,0,1);
    GX2SetScissor(0,0,cb->surface.width,cb->surface.height);
    GX2SetPixelTexture(&black_tex,0);GX2SetPixelSampler(&blit_sampler,0);
    set_blit_colors(0,0,0,0.04f,0.04f,0.04f);draw_rect(w,h,630,444,848,478,0,0,1,1);
    GX2SetPixelTexture(&font_tex,0);GX2SetPixelSampler(&lut_sampler,0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY,1,FALSE,TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0,GX2_BLEND_MODE_SRC_ALPHA,GX2_BLEND_MODE_INV_SRC_ALPHA,GX2_BLEND_COMBINE_MODE_ADD,FALSE,GX2_BLEND_MODE_ONE,GX2_BLEND_MODE_ZERO,GX2_BLEND_COMBINE_MODE_ADD);
    set_blit_colors(1,1,1,0,0,0);
    char label[]="SAVING STATE  /";
    label[14]="/-_>"[(OSTicksToMilliseconds(OSGetSystemTime())/150)%4];
    draw_text(w,h,loading_state_overlay ? "LOADING STATE..." : label,640,452,2);
    // Publish render-cache writes before the scan-buffer copy, as the menu
    // and FPS overlays do. Cemu can appear coherent without this on hardware.
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER,cb->surface.image,cb->surface.imageSize);
}
#endif
// Render on the temporary scanout copy, then restore the clean picture. A
// paused game therefore cannot retain an expired chat card in its borders.
static std::vector<std::string> border_chat;
static screens::Rect border_chat_box;
bool chat_visible() {
    static u32 last_poll;
    static int last_layout=-1,last_side;
    static std::vector<network::ChatLine> lines;
    if(!network::connected()){last_poll=0;return false;}
    if(!network::chat_side() || menu_is_open || keyboard_open() || mii_selector_open())return false;
    if(!network::chat_border(pad_layout,network::chat_side()==1,854,480,border_chat_box))return false;
    const u32 now=azl::platform::NowMs();
    const bool poll=!last_poll || now-last_poll>=250;
    if(poll){lines=network::chat();last_poll=now;}
    if(!poll && last_layout==pad_layout && last_side==network::chat_side())return !border_chat.empty();
    last_layout=pad_layout;last_side=network::chat_side();
    border_chat.clear();
    const unsigned scale=border_chat_box.w<180?1:2;
    const unsigned columns=std::max(1u,(border_chat_box.w-12)/(6*scale));
    const unsigned rows=(border_chat_box.h-12)/(9*scale);
    for(const auto &line:lines) if(now-line.when<15000) {
        for(size_t at=0;at<line.text.size();at+=columns)
            border_chat.push_back(line.text.substr(at,columns));
    }
    if(border_chat.size()>rows)border_chat.erase(border_chat.begin(),border_chat.end()-rows);
    return !border_chat.empty();
}
void draw_chat_on(GX2ColorBuffer *cb) {
    const auto &r=border_chat_box;
    const unsigned scale=r.w<180?1:2;
    const u32 height=12+border_chat.size()*9*scale,y=r.y+r.h-height;
    GX2SetColorBuffer(cb,GX2_RENDER_TARGET_0);cur_color=nullptr;begin_blit();
    GX2SetViewport(0,0,cb->surface.width,cb->surface.height,0,1);
    GX2SetScissor(0,0,cb->surface.width,cb->surface.height);
    GX2SetPixelTexture(&black_tex,0);GX2SetPixelSampler(&blit_sampler,0);
    set_blit_colors(0,0,0,1,1,1);draw_rect(854,480,r.x,y,r.x+r.w,y+height,0,0,1,1);
    GX2SetPixelTexture(&font_tex,0);GX2SetPixelSampler(&lut_sampler,0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY,1,FALSE,TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0,GX2_BLEND_MODE_SRC_ALPHA,GX2_BLEND_MODE_INV_SRC_ALPHA,GX2_BLEND_COMBINE_MODE_ADD,FALSE,GX2_BLEND_MODE_ONE,GX2_BLEND_MODE_ZERO,GX2_BLEND_COMBINE_MODE_ADD);
    set_blit_colors(0,0,0,0,0,0);
    for(size_t i=0;i<border_chat.size();i++)draw_text(854,480,border_chat[i].c_str(),r.x+6,y+6+i*9*scale,scale);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER,cb->surface.image,cb->surface.imageSize);
}
static void draw_network_on(GX2ColorBuffer *cb) {
    const auto view=network_menu::view();
    const float w=854,h=480;
    GX2SetColorBuffer(cb,GX2_RENDER_TARGET_0);cur_color=nullptr;begin_blit();
    GX2SetViewport(0,0,cb->surface.width,cb->surface.height,0,1);
    GX2SetScissor(0,0,cb->surface.width,cb->surface.height);
    GX2SetColorControl(GX2_LOGIC_OP_COPY,1,FALSE,TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0,GX2_BLEND_MODE_SRC_ALPHA,GX2_BLEND_MODE_INV_SRC_ALPHA,GX2_BLEND_COMBINE_MODE_ADD,FALSE,GX2_BLEND_MODE_ONE,GX2_BLEND_MODE_ZERO,GX2_BLEND_COMBINE_MODE_ADD);
    auto rect=[&](float x,float y,float width,float height,float r,float g,float b){
        GX2SetPixelTexture(&black_tex,0);GX2SetPixelSampler(&blit_sampler,0);set_blit_colors(0,0,0,r,g,b);draw_rect(w,h,x,y,x+width,y+height,0,0,1,1);
    };
    auto text=[&](std::string value,float x,float y,int scale,float r,float g,float b,unsigned max){
        if(value.size()>max)value=value.substr(0,max-3)+"...";
        for(char &c:value)if(static_cast<unsigned char>(c)<32)c=' ';
        GX2SetPixelTexture(&font_tex,0);GX2SetPixelSampler(&lut_sampler,0);set_blit_colors(r,g,b,0,0,0);draw_text(w,h,value.c_str(),x,y,scale);
    };
    rect(0,0,w,h,.08f,.13f,.23f);rect(0,0,w,58,.12f,.24f,.41f);
    text(view.title,24,16,3,1,1,1,43);
    text(view.status,24,70,2,.68f,.84f,1,65);
    using G=network::ListGesture;
    GX2SetScissor(0,unsigned(G::top*cb->surface.height/h),cb->surface.width,unsigned((G::bottom-G::top)*cb->surface.height/h));
    for(unsigned i=0;i<view.rows.size();i++) {
        const float y=G::top+i*G::row-view.scroll;if(y+G::row<G::top||y>G::bottom)continue;
        const auto &row=view.rows[i];bool selected=int(i)==view.selected;
        rect(24,y,788,45,selected?.14f:.12f,selected?.43f:.20f,selected?.64f:.31f);
        text(row.label,34,y+5,2,row.enabled?1:.5f,row.enabled?1:.5f,row.enabled?1:.5f,63);
        text(row.value,34,y+25,2,.64f,.82f,.98f,63);
    }
    GX2SetScissor(0,0,cb->surface.width,cb->surface.height);
    const float area=G::bottom-G::top,total=std::max(area,view.rows.size()*G::row);
    const float thumb=std::clamp(area*area/total,24.f,area);
    const float y=G::top+(area-thumb)*(view.scroll/std::max(1.f,total-area));
    rect(818,G::top,30,area,.16f,.23f,.34f);rect(820,y,26,thumb,.35f,.77f,1.f);
    text(view.hint,24,432,2,.8f,.88f,1,65);
    text("B: BACK     A: CHOOSE     RIGHT STICK / TOUCH: SCROLL",24,460,2,.65f,.75f,.9f,65);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER,cb->surface.image,cb->surface.imageSize);
}
static void draw_menu() {
    if(network_menu::active()) {draw_network_on(drc_cb);draw_network_on(tv_cb);tv_layout_shown=-1;state_lost=true;return;}
    draw_menu_on(drc_cb);
    draw_menu_on(tv_cb);
    tv_layout_shown = -1;  // once it closes, the TV is cleared before the game's next picture: the panel stood over the gaps between screens
    state_lost = true;
}

// ---- the software keyboard the game asks for (names, mostly), on the GamePad: touch, or the D-pad and A
static const char *const k_kb_rows[2][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"},
    {"!?()&+_:/%", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM,.-"},
};
static constexpr int KB_COLS = 10, KB_ROWS = 5;  // row 4: shift, space, delete, cancel, OK
static constexpr float KB_X = 107, KB_Y = 150, KB_W = 64, KB_H = 62;
static constexpr float KB_NUMPAD_OFFSET = 90;
static std::mutex kb_m;
static std::atomic<bool> kb_active;
static bool kb_network;
static int kb_row = 1, kb_col = 0;
static bool kb_shift;
static std::string kb_text;
static swkbd::Request kb_req;
static constexpr int kb_columns[] = {0, 2, 6, 7, 8, KB_COLS};
static constexpr int kb_numeric_columns[] = {0, 3, 6, KB_COLS};

// row 4's keys, by column
static int kb_special(int col) { return col < 2 ? 0 : col < 6 ? 1 : col < 7 ? 2 : col < 8 ? 3 : 4; }

static int kb_units(const std::string &text) {
    int units = 0;
    for (unsigned char c : text)
        if ((c & 0xc0) != 0x80) units += c >= 0xf0 ? 2 : 1;
    return units;
}

static bool kb_confirmable(const swkbd::Request &request, const std::string &text) {
    switch (request.valid) {
    case 1: return !text.empty();
    case 2:
    case 3: return text.find_first_not_of(' ') != std::string::npos;
    case 4: return kb_units(text) == request.max_len;
    default: return true;
    }
}

static bool kb_accepts(const swkbd::Request &request, const std::string &text, char c) {
    if (text.size() >= (kb_network ? network_keyboard::limit() : 63) || (request.max_len > 0 && kb_units(text) >= request.max_len)) return false;
    if (c >= '0' && c <= '9') {
        const auto digits = std::count_if(text.begin(), text.end(), [](char d) { return d >= '0' && d <= '9'; });
        return !request.digits_limited || digits < request.max_digits;
    }
    return !request.numpad && !(c == '%' && request.no_percent);
}

static int kb_press(int row, int col) {
    if (row < 4) {
        const char c = k_kb_rows[kb_shift][row][col];
        if (kb_accepts(kb_req, kb_text, c)) {
            kb_text += c;
            if (row > 0) kb_shift = false;
        }
        return 0;
    }
    switch (kb_special(col)) {
    case 0: if (!kb_req.numpad) kb_shift = !kb_shift; break;
    case 1: if (kb_accepts(kb_req, kb_text, ' ')) kb_text += ' '; break;
    case 2:
        if (!kb_text.empty()) {
            size_t end = kb_text.size() - 1;
            while (end && ((unsigned char)kb_text[end] & 0xc0) == 0x80) --end;
            kb_text.resize(end);
        }
        break;
    case 3: return kb_req.can_cancel ? -1 : 0;
    default: return kb_confirmable(kb_req, kb_text) ? 1 : 0;
    }
    return 0;
}

bool keyboard_open() { return kb_active; }

void keyboard_input(u32 pressed, bool touch_began, u16 x, u16 y) {
    int choice = 0;
    std::string result;
    {
        std::lock_guard<std::mutex> lock(kb_m);
        if (!kb_active) {
            if (network_keyboard::fallback_pending()) {
                kb_req={};kb_req.hint=network_keyboard::label();kb_req.initial=network_keyboard::initial();
                kb_req.max_len=network_keyboard::limit();kb_req.password=network_keyboard::secret();kb_req.numpad=network_keyboard::numeric();kb_req.can_cancel=true;
                kb_text=kb_req.initial;kb_network=true;kb_active=true;kb_row=kb_req.numpad?0:1;kb_col=0;kb_shift=false;
            } else if (swkbd::pending(&kb_req)) {
                kb_network=false;
                size_t n = std::min<size_t>(kb_req.initial.size(), 63);
                while (n && ((unsigned char)kb_req.initial[n] & 0xc0) == 0x80) --n;
                kb_text.assign(kb_req.initial, 0, n);
                kb_row = kb_req.numpad ? 0 : 1;
                kb_col = 0;
                kb_shift = !kb_req.numpad;
                kb_active = true;
            }
            return;
        }
        if (touch_began) {
            const float top = KB_Y + (kb_req.numpad ? KB_NUMPAD_OFFSET : 0);
            if (x >= KB_X && x < KB_X + KB_COLS * KB_W && y >= top) {
                const int c = (int)((x - KB_X) / KB_W), r = (int)((y - top) / KB_H);
                if (kb_req.numpad && r == 1) {
                    kb_row = 4;
                    kb_col = kb_columns[2 + std::min(c / 3, 2)];
                    choice = kb_press(kb_row, kb_col);
                } else if (r < (kb_req.numpad ? 1 : KB_ROWS)) {
                    kb_row = r;
                    kb_col = c;
                    choice = kb_press(r, c);
                }
            }
        } else {
            const int dy = !!(pressed & VPAD_BUTTON_DOWN) - !!(pressed & VPAD_BUTTON_UP);
            const int dx = !!(pressed & VPAD_BUTTON_RIGHT) - !!(pressed & VPAD_BUTTON_LEFT);
            if (dy) {
                kb_row = kb_req.numpad ? (kb_row == 0 ? 4 : 0) : (kb_row + KB_ROWS + dy) % KB_ROWS;
                if (kb_req.numpad && kb_row == 4) kb_col = std::max(kb_col, kb_columns[2]);
            }
            if (dx) {
                if (kb_row != 4) kb_col = (kb_col + KB_COLS + dx) % KB_COLS;
                else if (kb_req.numpad) kb_col = kb_columns[2 + (kb_special(kb_col) + 1 + dx) % 3];
                else kb_col = kb_columns[(kb_special(kb_col) + 5 + dx) % 5];
            }
            if (pressed & VPAD_BUTTON_A) choice = kb_press(kb_row, kb_col);
            else if (pressed & VPAD_BUTTON_B) choice = kb_network ? -1 : kb_press(4, 6);    // delete
            else if (pressed & VPAD_BUTTON_Y) choice = kb_press(4, 0);    // shift
            else if (pressed & VPAD_BUTTON_X) choice = kb_press(4, 2);    // space
            else if (pressed & VPAD_BUTTON_PLUS) choice = kb_press(4, 8); // OK
        }
        if (choice) {
            if (choice > 0) result = std::move(kb_text);
            else kb_text.clear();
            kb_active = false;
        }
    }
    if (choice) {if(kb_network){kb_network=false;network_keyboard::fallback_finish(result,choice>0);}else swkbd::finish(result,choice>0);}
}

static bool draw_keyboard() {
    if (!kb_active) return false;
    swkbd::Request request;
    std::string value;
    int row, col;
    bool shifted, confirm;
    {
        std::lock_guard<std::mutex> lock(kb_m);
        if (!kb_active) return false;
        request = kb_req;
        value = request.password ? std::string(kb_units(kb_text), '*') : kb_text;
        row = kb_row;
        col = kb_col;
        shifted = kb_shift;
        confirm = kb_confirmable(request, kb_text);
    }
    const u32 w = drc_cb->surface.width, h = drc_cb->surface.height;
    GX2SetColorBuffer(drc_cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, 0, (float)w, (float)h, 0, 1);
    GX2SetScissor(0, 0, w, h);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE,
                       GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);

    GX2SetPixelTexture(&disk_tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    const float right = KB_X + KB_COLS * KB_W;
    const float offset = request.numpad ? KB_NUMPAD_OFFSET : 0;
    const float bottom = request.numpad ? 289 + offset : 476;
    const auto paint = [&](float r, float g, float b) { set_blit_colors(r, g, b, 0, 0, 0); };
    const auto fill = [&](float x0, float y0, float x1, float y1) {
        draw_rect(w, h, x0, y0, x1, y1, 0.5f, 0.5f, 0.5f, 0.5f);
    };
    for (int band = 0; band < 16; ++band) {
        const float f = band / 15.0f;
        paint(0.45f - 0.18f * f, 0.59f - 0.20f * f, 0.86f - 0.16f * f);
        fill(0, 30.0f * band, (float)w, 30.0f * (band + 1));
    }
    paint(0.47f, 0.58f, 0.84f);
    draw_round_rect(w, h, KB_X - 16, 20 + offset, right + 16, bottom, 16);
    paint(0.11f, 0.17f, 0.35f);
    draw_round_rect(w, h, KB_X - 13, 23 + offset, right + 13, bottom - 3, 13);
    paint(0.96f, 0.97f, 1.0f);
    draw_round_rect(w, h, KB_X + 6, 64 + offset, right - 6, 134 + offset, 10);

    const auto key = [&](float x0, float y0, float x1, bool selected, bool enabled) {
        if (selected) {
            paint(0.18f, 0.76f, 1.0f);
            draw_round_rect(w, h, x0 + 2, y0 + 3, x1 - 2, y0 + KB_H - 3, 9);
        }
        if (enabled) paint(0.93f, 0.94f, 0.97f);
        else paint(0.26f, 0.32f, 0.48f);
        fill(x0 + 6, y0 + 7, x1 - 6, y0 + KB_H - 7);
    };
    const int rows = request.numpad ? 1 : 4;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < KB_COLS; ++c)
            key(KB_X + c * KB_W, KB_Y + offset + r * KB_H, KB_X + (c + 1) * KB_W, r == row && c == col, true);
    const float y4 = KB_Y + offset + (request.numpad ? 1 : 4) * KB_H;
    const auto action_x = [&](int i) { return KB_X + (request.numpad ? kb_numeric_columns[i - 2] : kb_columns[i]) * KB_W; };
    const int first_action = request.numpad ? 2 : 0;
    const bool enabled[5] = {!request.numpad, !request.numpad, !value.empty(), request.can_cancel, confirm};
    for (int i = first_action; i < 5; ++i)
        key(action_x(i), y4, action_x(i + 1), row == 4 && kb_special(col) == i, enabled[i]);

    const bool font = ui_font::load();
    GX2SetPixelTexture(font ? &ui_font::texture() : &font_tex, 0);
    GX2SetPixelSampler(font ? &blit_sampler : &lut_sampler, 0);
    const auto pixel_scale = [](float size) { return std::max(1.0f, std::floor(size / 8)); };
    const auto width = [&](const char *s, float size) { return font ? ui_font::width(s, size) : strlen(s) * 6 * pixel_scale(size); };
    const auto text = [&](const char *s, float x, float y, float size) {
        if (!font) {
            draw_text(w, h, s, x, y + (size - 7 * pixel_scale(size)) / 2, pixel_scale(size));
            return;
        }
        ui_font::Quad glyphs[64];
        const int count = ui_font::quads(s, size, x, y, glyphs, 64);
        for (int i = 0; i < count; ++i) {
            const auto &q = glyphs[i];
            draw_rect(w, h, q.x0, q.y0, q.x1, q.y1, q.u0, q.v0, q.u1, q.v1);
        }
    };
    paint(1, 1, 1);
    GX2SetScissor((u32)(KB_X + 10), (u32)(30 + offset), (u32)(right - KB_X - 20), 30);
    const char *hint = request.hint.empty() ? (request.numpad ? "Enter a number" : "Enter text") : request.hint.c_str();
    text(hint, KB_X + 10, 35 + offset, 23);
    GX2SetScissor((u32)(KB_X + 18), (u32)(69 + offset), (u32)(right - KB_X - 36), 60);
    paint(0.08f, 0.12f, 0.25f);
    text(value.c_str(), std::min(KB_X + 18, right - 22 - width(value.c_str(), 32)), 83 + offset, 32);
    GX2SetScissor(0, 0, w, h);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < KB_COLS; ++c) {
            const char label[2] = {k_kb_rows[shifted][r][c], 0};
            text(label, KB_X + (c + 0.5f) * KB_W - width(label, 29) / 2, KB_Y + offset + r * KB_H + 16, 29);
        }
    const char *actions[5] = {shifted ? "ABC" : "abc", "Space", "Delete", request.cancel.empty() ? "Cancel" : request.cancel.c_str(),
                              request.ok.empty() ? "OK" : request.ok.c_str()};
    for (int i = first_action; i < 5; ++i) {
        const float x0 = action_x(i), x1 = action_x(i + 1);
        const float size = !request.numpad && (i == 2 || i == 3) ? 16.0f : 21.0f;
        const float label_width = width(actions[i], size);
        if (enabled[i]) paint(0.08f, 0.12f, 0.25f);
        else paint(0.78f, 0.83f, 0.94f);
        GX2SetScissor((u32)(x0 + 7), (u32)y4, (u32)(x1 - x0 - 14), (u32)KB_H);
        text(actions[i], std::max(x0 + 7, (x0 + x1 - label_width) / 2), y4 + (KB_H - size) / 2, size);
    }
    GX2SetScissor(0, 0, w, h);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, drc_cb->surface.image, drc_cb->surface.imageSize);
    state_lost = true;
    return true;
}

// ---- the Mii selector the game asks for (mii_select.h), on the GamePad
static constexpr float MS_X = 28, MS_Y = 66, MS_W = 152, MS_H = 172, MS_GAP = 10;
static constexpr float MS_FOOTER = 442, MS_L = 28, MS_R = 199, MS_SORT = 512, MS_BACK = 641, MS_CHOOSE = 741;
static constexpr int MS_COLS = 5, MS_ROWS = 2, MS_PAGE = MS_COLS * MS_ROWS;
static std::mutex ms_m;
static u32 ms_next_layout;
static std::atomic<u32> ms_presented_layout;
static struct {
    std::shared_ptr<const mii_select::Request> request;
    std::shared_ptr<const std::vector<int>> by_name;
    u32 layout = 0;
    int sel = 0;
    int row(int pos) const { return by_name ? (*by_name)[pos] : pos; }
    int top() const { return sel / MS_PAGE * MS_PAGE; }
} ms;

bool mii_selector_open() {
    std::lock_guard<std::mutex> lock(ms_m);
    return !!ms.request;
}

void mii_selector_input(u32 pressed, bool touch_began, u16 x, u16 y) {
    const auto request = mii_select::pending();
    int pick = -2;
    {
        std::lock_guard<std::mutex> lock(ms_m);
        if (request != ms.request) {
            ms = {};
            if (request && !request->entries.empty()) {
                ms.request = request;
                ms.layout = ++ms_next_layout;
                ms.sel = std::clamp(request->initial, 0, (int)request->entries.size() - 1);
            }
            return;
        }
        if (!request) return;
        const int n = (int)request->entries.size();
        const bool layout_visible = ms.layout == ms_presented_layout.load();
        if (touch_began && layout_visible && x >= MS_X && y >= MS_Y && y < MS_FOOTER) {
            const float dx = x - MS_X, dy = y - MS_Y;
            const int col = (int)(dx / (MS_W + MS_GAP)), row = (int)(dy / (MS_H + MS_GAP));
            const int pos = ms.top() + row * MS_COLS + col;
            if (col < MS_COLS && row < MS_ROWS && pos < n &&
                dx - col * (MS_W + MS_GAP) < MS_W && dy - row * (MS_H + MS_GAP) < MS_H)
                pick = ms.row(pos);
        }
        if (touch_began && y >= MS_FOOTER && y < 480) {
            if (x >= MS_L && x < MS_L + 36) pressed |= VPAD_BUTTON_L;
            if (x >= MS_R && x < MS_R + 36) pressed |= VPAD_BUTTON_R;
            if (x >= MS_SORT && x < MS_BACK) pressed |= VPAD_BUTTON_Y;
            if (x >= MS_BACK && x < MS_CHOOSE) pressed |= VPAD_BUTTON_B;
            if (layout_visible && x >= MS_CHOOSE && x < 828) pressed |= VPAD_BUTTON_A;
        }
        if (pick == -2) {
            const auto old_order = ms.by_name;
            const int top = ms.top();
            if ((pressed & VPAD_BUTTON_Y) && n > 1) {
                const int row = ms.row(ms.sel);
                if (ms.by_name) {
                    ms.by_name.reset();
                    ms.sel = row;
                } else {
                    auto sorted = std::make_shared<std::vector<int>>(n);
                    for (int i = 0; i < n; ++i) (*sorted)[i] = i;
                    std::stable_sort(sorted->begin(), sorted->end(), [&](int a, int b) {
                        return request->entries[a].name < request->entries[b].name;
                    });
                    ms.sel = (int)(std::find(sorted->begin(), sorted->end(), row) - sorted->begin());
                    ms.by_name = std::move(sorted);
                }
            }
            const int dx = !!(pressed & VPAD_BUTTON_RIGHT) - !!(pressed & VPAD_BUTTON_LEFT);
            const int dy = !!(pressed & VPAD_BUTTON_DOWN) - !!(pressed & VPAD_BUTTON_UP);
            const int pos = ms.sel % MS_PAGE;
            const int rows = (std::min(MS_PAGE, n - ms.top()) - 1) / MS_COLS + 1;
            const int row = std::clamp(pos / MS_COLS + dy, 0, rows - 1);
            const int col = std::clamp(pos % MS_COLS + dx, 0, MS_COLS - 1);
            ms.sel = std::min(ms.top() + row * MS_COLS + col, n - 1);
            const int page = !!(pressed & VPAD_BUTTON_R) - !!(pressed & VPAD_BUTTON_L);
            const int next_page = std::clamp(ms.sel / MS_PAGE + page, 0, (n - 1) / MS_PAGE);
            ms.sel = std::min(next_page * MS_PAGE + ms.sel % MS_PAGE, n - 1);
            if (top != ms.top() || old_order != ms.by_name) ms.layout = ++ms_next_layout;
            if (pressed & VPAD_BUTTON_A) pick = ms.row(ms.sel);
            else if ((pressed & VPAD_BUTTON_B) && request->can_cancel) pick = -1;
        }
        if (pick != -2) ms = {};
    }
    if (pick != -2) mii_select::finish(*request, pick);
}

static bool draw_mii_selector(u32 &layout) {
    const auto view = [] {
        std::lock_guard<std::mutex> lock(ms_m);
        return ms;
    }();
    if (!view.request) return false;
    layout = view.layout;
    const auto &request = *view.request;
    const u32 w = drc_cb->surface.width, h = drc_cb->surface.height;
    const int n = (int)request.entries.size(), top = view.top(), count = std::min(MS_PAGE, n - top);
    std::array<const char *, MS_PAGE + 1> labels{};
    labels[0] = request.title.empty() ? "Choose a Mii" : request.title.c_str();
    for (int i = 0; i < count; ++i) {
        const auto &name = request.entries[view.row(top + i)].name;
        labels[1 + i] = name.empty() ? "Unnamed Mii" : name.c_str();
    }
#ifdef RECOMP_FFL_TRIAL
    std::array<const mii_select::Request::Entry *, MS_PAGE> entries;
    for (int i = 0; i < count; ++i) entries[i] = &request.entries[view.row(top + i)];
    if (mii_face::prepare(view.layout, entries.data(), count)) restore_context();
#endif
    const bool font = ui_font::load();
    if (font) ui_font::prepare(labels.data(), count + 1);
    GX2SetColorBuffer(drc_cb, GX2_RENDER_TARGET_0);
    cur_color = nullptr;
    begin_blit();
    GX2SetViewport(0, 0, (float)w, (float)h, 0, 1);
    GX2SetScissor(0, 0, w, h);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD, FALSE,
                       GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetPixelTexture(&disk_tex, 0);
    GX2SetPixelSampler(&blit_sampler, 0);
    const auto paint = [](float r, float g, float b) { set_blit_colors(r, g, b, 0, 0, 0); };
    const auto fill = [&](float x0, float y0, float x1, float y1) {
        draw_rect(w, h, x0, y0, x1, y1, 0.5f, 0.5f, 0.5f, 0.5f);
    };
    const auto rounded = [&](float x, float y, float wide, float tall, float radius) {
        draw_round_rect(w, h, x, y, x + wide, y + tall, radius);
    };
    paint(0.79f, 0.91f, 0.97f);
    fill(0, 0, (float)w, (float)h);
    paint(0.48f, 0.66f, 0.78f);
    fill(MS_X, 49, w - MS_X, 51);
    fill(MS_X, MS_FOOTER - 6, w - MS_X, MS_FOOTER - 4);
    for (int i = 0; i < count; ++i) {
        const float x = MS_X + i % MS_COLS * (MS_W + MS_GAP), y = MS_Y + i / MS_COLS * (MS_H + MS_GAP);
        const bool focused = top + i == view.sel;
        if (focused) {
            paint(0.0f, 0.49f, 0.47f);
            rounded(x - 3, y - 3, MS_W + 6, MS_H + 6, 16);
            paint(0.82f, 0.98f, 0.96f);
        } else {
            paint(0.95f, 0.98f, 1.0f);
        }
        rounded(x + 2, y + 2, MS_W - 4, MS_H - 4, 12);
        const float middle = x + MS_W / 2;
#ifdef RECOMP_FFL_TRIAL
        if (const auto *face = mii_face::texture(i)) {
            paint(1, 1, 1);
            GX2SetPixelTexture(face, 0);
            draw_rect(w, h, middle - 63, y + 2, middle + 63, y + 128, 0, 0, 1, 1);
            GX2SetPixelTexture(&disk_tex, 0);
            continue;
        }
#endif
        paint(0.84f, 0.91f, 0.96f);
        draw_rect(w, h, middle - 48, y + 21, middle + 48, y + 117, 0, 0, 1, 1);
        paint(0.34f, 0.49f, 0.62f);
        draw_rect(w, h, middle - 21, y + 39, middle + 21, y + 81, 0, 0, 1, 1);
        rounded(middle - 29, y + 87, 58, 20, 10);
    }
    paint(0.24f, 0.34f, 0.48f);
    if (n > MS_PAGE) {
        rounded(MS_L, MS_FOOTER + 5, 30, 25, 7);
        rounded(MS_R, MS_FOOTER + 5, 30, 25, 7);
    }
    if (n > 1) rounded(MS_SORT, MS_FOOTER + 5, 25, 25, 12);
    if (request.can_cancel) rounded(MS_BACK, MS_FOOTER + 5, 25, 25, 12);
    rounded(MS_CHOOSE, MS_FOOTER + 5, 25, 25, 12);
    GX2SetPixelTexture(font ? &ui_font::texture() : &font_tex, 0);
    GX2SetPixelSampler(font ? &blit_sampler : &lut_sampler, 0);
    const auto pixel_scale = [](float size) { return std::max(1.0f, size / 12); };
    const auto width = [&](const char *s, float size) {
        if (font) return ui_font::width(s, size);
        int glyphs = 0;
        for (; *s; ++s) glyphs += ((u8)*s & 0xC0) != 0x80;
        return glyphs * 6 * pixel_scale(size);
    };
    const auto text = [&](const char *s, float x, float y, float size) {
        if (!font) {
            std::string ascii;
            for (; *s; ++s) {
                const u8 c = (u8)*s;
                if (c < 0x80) ascii += *s;
                else if (c >= 0xC0) ascii += '?';
            }
            const float scale = pixel_scale(size);
            draw_text(w, h, ascii.c_str(), x, y + (size - 7 * scale) / 2, scale);
            return;
        }
        ui_font::Quad q[64];
        const int count = ui_font::quads(s, size, x, y, q, 64);
        for (int i = 0; i < count; ++i)
            draw_rect(w, h, q[i].x0, q[i].y0, q[i].x1, q[i].y1, q[i].u0, q[i].v0, q[i].u1, q[i].v1);
    };
    const auto centered = [&](const char *s, float x, float y, float size) { text(s, x - width(s, size) / 2, y, size); };
    char number[32];
    if (n >= 1000) snprintf(number, sizeof number, "%d,%03d Miis", n / 1000, n % 1000);
    else snprintf(number, sizeof number, n == 1 ? "%d Mii" : "%d Miis", n);
    const float xnumber = w - MS_X - width(number, 24);
    paint(0.09f, 0.17f, 0.32f);
    text(number, xnumber, 15, 24);
    GX2SetScissor((u32)MS_X, 0, (u32)(xnumber - MS_X - 16), 49);
    text(labels[0], MS_X, 11, 30);
    for (int i = 0; i < count; ++i) {
        const char *name = labels[1 + i];
        const float x = MS_X + i % MS_COLS * (MS_W + MS_GAP), y = MS_Y + i / MS_COLS * (MS_H + MS_GAP);
        const float size = std::clamp(24 * (MS_W - 16) / std::max(1.0f, width(name, 24)), 14.0f, 24.0f);
        GX2SetScissor((u32)(x + 8), (u32)(y + 120), (u32)(MS_W - 16), (u32)(MS_H - 120));
        centered(name, x + MS_W / 2, y + 131 + (24 - size) / 2, size);
    }
    GX2SetScissor(0, 0, w, h);
    snprintf(number, sizeof number, "Page %d / %d", top / MS_PAGE + 1, (n - 1) / MS_PAGE + 1);
    centered(number, (MS_L + 30 + MS_R) / 2, MS_FOOTER + 7, 19);
    if (n > 1) text(view.by_name ? "Reset" : "Sort", MS_SORT + 31, MS_FOOTER + 7, 19);
    if (request.can_cancel) text("Back", MS_BACK + 31, MS_FOOTER + 7, 19);
    text("Choose", MS_CHOOSE + 31, MS_FOOTER + 7, 19);
    paint(1, 1, 1);
    if (n > MS_PAGE) {
        centered("L", MS_L + 15, MS_FOOTER + 7, 19);
        centered("R", MS_R + 15, MS_FOOTER + 7, 19);
    }
    if (n > 1) centered("Y", MS_SORT + 12.5f, MS_FOOTER + 7, 19);
    if (request.can_cancel) centered("B", MS_BACK + 12.5f, MS_FOOTER + 7, 19);
    centered("A", MS_CHOOSE + 12.5f, MS_FOOTER + 7, 19);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, drc_cb->surface.image, drc_cb->surface.imageSize);
    state_lost = true;
    return true;
}

static void mii_selector_scanout(u32 submitted_layout) {
    static u32 layout, layout_swap;
    if (submitted_layout == layout && layout == ms_presented_layout.load()) return;
    u32 swaps = 0, flips = 0;
    OSTime last_flip = 0, last_vsync = 0;
    GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
    if (submitted_layout != layout || (s32)(swaps - layout_swap) < 0) {
        layout = submitted_layout;
        layout_swap = swaps;
    }
    // Touch must use the page and sort order already on screen.
    if ((s32)(flips - layout_swap) >= 0) ms_presented_layout.store(layout);
}

// The report is asked for by the frontend's thread and written by this one, between frames: the counters it prints and starts over are this
// thread's, and read from the other they came out torn (a count taken mid-draw, a reset that lost what was added meanwhile).
static std::atomic<bool> report_due;
static std::atomic<float> report_draws_per_flip;
static void write_report();

static u32 lcd_buffer_size(u32 pa, u32 stride, u32 row_bytes) {
    if (stride < row_bytes) return 0;
    const u64 end = u64(pa) + u64(stride) * 399 + row_bytes;
    if ((pa >= mem::VRAM_PADDR && end <= mem::VRAM_PADDR + mem::VRAM_SIZE) ||
        (pa >= mem::FCRAM_PADDR && end <= mem::FCRAM_PADDR + mem::LINEAR_SIZE))
        return (u32)(end - pa);
    return 0;
}

// Observe each buffer independently so alternating stale GPU buffers don't look like CPU video.
struct CpuLcdFrames {
    struct Buffer {
        u32 addr, layout;
        u64 hash;
        bool changed;
    };
    std::array<Buffer, 4> buffers{};
    u32 next = 0;

    bool observe(u32 addr, u32 layout, u64 hash) {
        for (Buffer &b : buffers)
            if (b.addr == addr) {
                b.changed = b.layout == layout && (b.changed || b.hash != hash);
                b.layout = layout;
                b.hash = hash;
                return b.changed;
            }
        buffers[next++ % buffers.size()] = {addr, layout, hash, false};
        return false;
    }
};

// The top screen as its LCD buffer holds it, for a game that brings it by no display transfer: Luigi's Mansion 2 draws its top screen in parts and
// puts them together in the LCD buffer with texture copies. A 3DS's LCD just shows the buffer, so this does too: 240x400, read as a render target's
// texture is (its last memory row the texture's first), so show_screen takes it as one. A game writes the buffer the LCD doesn't show and swaps to
// it later, often after the next VBlank: what's shown is whichever buffer the LCD shows now, as last written, each time that changes.
static void show_lcd_from_memory() {
    gt_mark(GT_LCD, 0);
    static u32 shown_pa, shown_seq;
    static u64 shown_drawn, shown_hash;
    static u32 shown_layout;
    static CpuLcdFrames cpu_frames;
    const kernel::FramebufferInfo f = kernel::gsp_framebuffer(0);
    const u32 pa = f.valid && f.addr_left ? mem::vaddr_to_paddr(f.addr_left) : 0;
    const u32 fmt = f.format & 7, bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    const u32 row_bytes = 240 * bpp, stride = f.stride ? f.stride : row_bytes;
    if (pa && std::find(std::begin(recent_top), std::end(recent_top), pa) == std::end(recent_top)) {
        static u32 next_top;
        recent_top[next_top++ % 4] = pa;
    }
    recent_top_stride = stride;
    recent_top_bpp = bpp;
    recent_top_fmt = cbuf_to_pixfmt(fmt);  // the display format as a colour buffer's (the table is its own inverse)
    // Not for a game that brought its top screen by transfer within the last second, and only what was written after its last such picture: at
    // thirty a second there's a VBlank without a transfer between two pictures, and New Super Mario Bros. 2 and Tomodachi Life had once written
    // into their LCD buffers: that memory, black, showed between their pictures, and the whole TV flickered.
    static u32 transfer_seq, quiet;  // quiet: VBlanks since a transfer last brought the top screen
    static u64 transfer_drawn;
    if (top_shown) {
        top_shown = false;
        shown_pa = 0;
        transfer_seq = memory_write_seq;
        transfer_drawn = gpu_writes;
        quiet = 0;
        cpu_frames = {};
        return;
    }
    if (quiet < 60) {
        quiet++;
        return;
    }
    const u32 size = fmt <= 4 ? lcd_buffer_size(pa, stride, row_bytes) : 0;
    const u8 *src = size ? pptr(pa) : nullptr;
    if (!src) return;
    // put together on the GPU (linear_copy_on_gpu): shown from there, never read back, unless the game has written over it since
    Surface *lcd = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->linear && s->addr <= pa && pa + size <= s->addr + s->w * bpp * s->h && s->w * bpp == stride &&
            (!lcd || s->drawn > lcd->drawn))
            lcd = s.get();
    if (lcd && !lcd->guest_newer) {
        used(*lcd);
        if (lcd->drawn <= transfer_drawn || (pa == shown_pa && lcd->drawn == shown_drawn)) return;
        shown_pa = pa;
        shown_drawn = lcd->drawn;
        shown_seq = 0;
        shown_layout = 0;
        cpu_frames = {};
        flush_surface(*lcd);
        const float h = (float)lcd->tex_h, first_row = (float)((pa - lcd->addr) / stride);
        if (gxt()) LOG("[gxt] top screen from the LCD buffer put together on the GPU, %08x, from row %u of %08x\n", pa, (u32)first_row, lcd->addr);
        show_screen(0, &lcd->tex, false, (h - first_row - 400) / h, (h - first_row) / h, 240.0f / lcd->w);
        top_shown = false;
        return;
    }
    u32 latest = 0;
    for (const MemoryWrite &w : memory_writes)
        if (w.seq && w.pa < pa + size && pa < w.pa + w.size) latest = std::max(latest, w.seq);
    const u32 layout = stride * 8 + fmt;
    u64 hash = 0;
    if (!latest || latest <= transfer_seq) {
        if (pa < mem::FCRAM_PADDR) return;
        latest = 0;
        // The CPU can decode directly into the LCD buffer without issuing a GPU command.
        if (stride == row_bytes)
            hash = hash_bytes(src, size);
        else
            for (u32 row = 0; row < 400; row++)
                hash = hash * 31 + hash_bytes(src + size_t(row) * stride, row_bytes);
        if (!cpu_frames.observe(pa, layout, hash)) return;
        if (pa == shown_pa && shown_seq == 0 && shown_layout == layout && shown_hash == hash) return;
    } else {
        if (pa == shown_pa && latest == shown_seq) return;
        cpu_frames = {};
    }
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
    shown_pa = pa;
    shown_seq = latest;
    shown_layout = layout;
    shown_hash = hash;
    shown_drawn = 0;
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
    if (gxt()) LOG("[gxt] top screen from its LCD buffer in memory %08x, format %u, stride %u\n", pa, fmt, stride);
    show_screen(0, &t, false);
    top_shown = false;
}

static void show_bottom_lcd() {
    const kernel::FramebufferInfo f = kernel::gsp_framebuffer(1);
    const u32 pa = f.valid && f.addr_left ? mem::vaddr_to_paddr(f.addr_left) : 0;
    if (!pa || !packed_bottom(f, f.format & 7)) return;
    const u32 fmt = cbuf_to_pixfmt(f.format & 7);
    Surface *shown = find_surface(pa, false, [&](const Surface &s) {
        return s.lcd_bottom && s.linear && s.w == 240 && s.h == 320 && s.fmt == fmt && !s.guest_newer;
    });
    if (!shown || shown->drawn == shown_bottom_draw) return;
    shown_bottom_draw = shown->drawn;
    flush_surface(*shown);
    const float height = (float)shown->tex_h;
    if (gxt()) LOG("[gxt] GamePad from selected LCD %08x draw %llu\n", pa, (unsigned long long)shown_bottom_draw);
    show_screen(1, &shown->tex, false, (height - 320) / height, 1);
}

// (the GPU thread, after each look at its queue) quiet: it had nothing to do for its whole wait. present_rule.h says when a frame is shown.
void gfx_pump(bool quiet) {
    const int pause = debug_menu_request.exchange(-1, std::memory_order_relaxed);
    if (pause >= 0) { menu_is_open = pause != 0; LOG("[debug] menu pause %s\n", pause ? "on" : "off"); }
    const int speed = debug_speed_request.exchange(0, std::memory_order_relaxed);
    if (speed) { menu_speed = speed; kernel::set_clock_speed(speed); LOG("[debug] game speed %d%%\n", speed); }

    if (av_timing_enabled()) {
        static OSTime since;
        static u32 polls, blanks, idle, held;
        static u32 pictures[2];
        const OSTime now = OSGetSystemTime();
        ++polls;
        blanks += vblank_pending ? 1 : 0;
        idle += quiet ? 1 : 0;
        held += present_rule.fresh && present_rule.on_swaps() && !present_rule.swapped_drawn() ? 1 : 0;
        if (!since) since = now;
        if (now - since >= OSSecondsToTicks(2)) {
            LOG("[present-time] polls %u vblanks %u quiet %u held %u pictures %u/%u fresh %x frame %x swap_due %x since_swap %u\n",
                polls, blanks, idle, held,
                st_pictures[0] >= pictures[0] ? st_pictures[0] - pictures[0] : st_pictures[0],
                st_pictures[1] >= pictures[1] ? st_pictures[1] - pictures[1] : st_pictures[1],
                present_rule.fresh, present_rule.frame, present_rule.swap_due, present_rule.since_swap);
            pictures[0] = st_pictures[0]; pictures[1] = st_pictures[1];
            polls = blanks = idle = held = 0;
            since = now;
        }
    }
    if (!vblank_pending) {
        // the menu, the keyboard and the Mii list are drawn at a VBlank's look only, so while one is up nothing is shown in between
        if (!menu_is_open && !kb_active && !mii_selector_open() && present_rule.between(quiet)) {
            screen_fresh = false;
            present();
        }
        return;
    }
    vblank_pending = false;
    show_lcd_from_memory();
    show_bottom_lcd();
    const OSTime install_start = wiiu::av_timing_enabled() ? OSGetSystemTime() : 0;
    install_compiled();
    if (install_start) {
        const auto elapsed = OSTicksToMicroseconds(OSGetSystemTime() - install_start);
        if (elapsed >= 5000)
            LOG("[pace-shader-install] %llu us\n", (unsigned long long)elapsed);
    }
    if (report_due.exchange(false)) write_report();
    shader_compile_watch();
    static bool chat_was_visible;
    const bool chat_now=chat_visible();
    if(chat_now || chat_was_visible)screen_fresh=true;
    chat_was_visible=chat_now;
    u32 mii_layout = 0;
    if (menu_is_open) {
        draw_menu();
        if(network_menu::active() && network_keyboard::active()) {
            if(!network_keyboard::draw()) draw_keyboard();
            GX2SetContextState(ctx);cur_color=nullptr;
        }
        screen_fresh = true;
    } else if (draw_keyboard() || draw_mii_selector(mii_layout)) {
        screen_fresh = true;
    }
#ifdef RECOMP_RESUMABLE
    static bool saving_was_visible;
    const bool saving_now=checkpoint::saving_to_disk();
    if (saving_now || saving_was_visible) screen_fresh=true;
    saving_was_visible=saving_now;
#endif
    static u32 swaps_seen[2];  // each screen's swaps (services/gsp.cpp) up to the last VBlank's look
    unsigned swapped = 0;
    for (int s = 0; s < 2; s++) {
        const u32 swaps = kernel::gsp_swaps(s);
        if (swaps != swaps_seen[s]) swapped |= 1u << s;
        swaps_seen[s] = swaps;
    }
    if (present_rule.vblank(screen_fresh, swapped, menu_is_open || kb_active || mii_selector_open())) {
        screen_fresh = false;
        present();
        if (mii_layout) mii_selector_scanout(mii_layout);
    }
}

void gfx_shutdown() {
    gpu::vtx_flush_immediate();
    gpu::vtx_finish_jobs();
    gpu::gx2_flush_cpu_batch();
    GX2DrawDone();
#ifdef RECOMP_FFL_TRIAL
    mii_face::release();
#endif
}

void gfx_report_final() {
    LOG("[gx2 final] native draws %llu, CPU batches %llu, fragment misses %llu, immediate native draws %llu, immediate CPU runs %llu\n",
        (unsigned long long)gpu::st_draws, (unsigned long long)gpu::st_cpu_draws, (unsigned long long)gpu::st_ps_misses,
        (unsigned long long)gpu::st_immediate_draws, (unsigned long long)gpu::vtx_immediate_cpu_runs);
}

// Back from the HOME Menu: the system took MEM1 and WHB has made its own buffers again, so the screens' render targets need
// new memory too, in the order they first got it. What was in them is gone; the game draws them again every frame.
void gfx_reacquire() {
    free_whb_depth();
    if (tv_720) tv_720 = tv_to_720();
    if (!tv_720) tv_cb = WHBGfxGetTVColourBuffer();
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
    if (!strcmp(name, "slicescale") && v <= 1) {
        screen_slice_experiment.store(v != 0, std::memory_order_relaxed);
        LOG("[gx2] screen-slice experiment %s; relaunch to restore already resized targets\n", v ? "enabled" : "disabled");
    }
    else if (!strcmp(name, "fbuv") && v <= 1) fallback_uv_select.store(v != 0, std::memory_order_relaxed);
    else if (!strcmp(name, "fbextra") && v <= 1) { material_extension.store(v != 0, std::memory_order_relaxed); forget_ps = true; }
    else if (!strcmp(name, "fbfast") && v <= 3) {
        bounded_fallback.store(v, std::memory_order_relaxed);
        forget_ps = true;
    }
    else if (!strcmp(name, "uberall") && v <= 1) {
        uber_all.store(v != 0, std::memory_order_relaxed);
    }
    else if (!strcmp(name, "uberfb")) {
        uber_fallback = v;
        forget_ps = true;
    } else if (!strcmp(name, "pause") && v <= 1) debug_menu_request.store((int)v, std::memory_order_relaxed);
    else if (!strcmp(name, "speed") && (v == 100 || v == 200 || v == 400)) debug_speed_request.store((int)v, std::memory_order_relaxed);
    else if (!strcmp(name, "cpuvs")) gxt_cpu_vs = v;
    else if (!strcmp(name, "poolspin") && v<=1) g_pool_idle_spin.store(v!=0,std::memory_order_relaxed);
    else if (!strcmp(name, "vssimd") && v<=1) g_cpu_vs_simd.store(v!=0,std::memory_order_relaxed);
    else if (!strcmp(name, "cpuattr") && v <= 15) gxt_cpu_attr = v;
    else if (!strcmp(name, "drawtrace") && v <= 1) draw_trace_enabled = v;
    else if (!strcmp(name, "vtxcache")) {  // the vertex data cache (vtx_cached); off drops what it holds
        use_vtx_cache = v;
        if (!v) vtx_drop_all = true;  // by the GPU thread, at its next flip
    } else if (!strcmp(name, "fillinside")) fill_inside = v;  // a fill fills the targets wholly inside it (gx2_memory_fill)
    else if (!strcmp(name, "lineargpu")) linear_on_gpu = v;  // texture copies out of linear surfaces on the GPU (linear_copy_on_gpu)
    else if (!strcmp(name, "halfss")) half_ss = v;  // a target its transfer scales down is drawn at the smaller size (note_downscaled); from the next one made
    else if (!strcmp(name, "capfps")) cap_fps = v;  // the game's frames a second (frame_cap_wait); 0 for as fast as it goes
    else if (!strcmp(name, "gsshared")) g_gs_shared = v;  // geometry shader runs on two threads (vertex.cpp: gs_run_shared)
    else if (!strcmp(name, "indexcache") && v <= 3) index_range_experiment.store(v, std::memory_order_relaxed);
    else if (!strcmp(name, "nudge") && (v <= 50 || v == 100)) viewport_nudge_override.store(v == 100 ? -1 : (int)v, std::memory_order_relaxed);
    else if (!strcmp(name, "copyrect") && v <= 1) copy_rect_experiment.store(v != 0, std::memory_order_relaxed);
    else if (!strcmp(name, "lightguard") && v <= 1) light_guard_experiment.store(v != 0, std::memory_order_relaxed);
    else if (!strcmp(name, "cputime") && v <= 1) g_cpu_timing.store(v != 0, std::memory_order_relaxed);
    else if (!strcmp(name, "gputime")) {  // the GPU's own time per pass (gt_mark), in the [prof] lines
        gt_on = v;
        live_gpu_timing.store(v != 0, std::memory_order_relaxed);
    }
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
    static u32 last_lcd_swaps[2];
    const OSTime now = OSGetSystemTime();
    const float secs = std::max(0.001f, OSTicksToMicroseconds(now - (last_t ? last_t : renderer_started)) / 1e6f);
    const float fps = (st_flips - last_flips) / secs;
    const float busy = last_t ? (gpu_busy_us - last_busy) / (secs * 1e6f) * 100 : 0;
    LOG("[gx2] %.1f fps, gpu thread %.0f%% busy, %.0f draws/s, %llu on CPU, %llu fragment misses, %llu textures uploaded (%llu KB, %.1f ms, %llu decoded on the GPU), %u KB textures, %u surfaces\n",
        fps, busy, (st_draws - last_draws) / secs, (unsigned long long)(st_cpu_draws - last_cpu_draws), (unsigned long long)st_ps_misses, (unsigned long long)st_uploads,
        (unsigned long long)(st_upload_bytes >> 10), st_upload_ms, (unsigned long long)st_gpu_decodes, (u32)(tex_memory >> 10), (u32)surfaces.size());
    LOG("[gx2 immediate] native draws %llu, CPU shader runs %llu\n", (unsigned long long)st_immediate_draws, (unsigned long long)vtx_immediate_cpu_runs);
    if (st_shipped_vs_reuses || st_shipped_ps_reuses)
        LOG("[prof] duplicate shipped shader loads avoided: %u vertex, %u pixel\n", st_shipped_vs_reuses, st_shipped_ps_reuses);
    st_shipped_vs_reuses = st_shipped_ps_reuses = 0;
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
    LOG("[prof] the game's pictures a second: top screen %.1f, bottom screen %.1f\n", st_pictures[0] / secs, st_pictures[1] / secs);
    // Swap requests (Azahar's application-FPS counter), changed LCD buffers,
    // and copied pictures are different events. Log them together before
    // treating a 30-picture rate as 30 frames of guest execution.
    static kernel::GspFrameStats previous_flow{};
    const auto flow = kernel::gsp_frame_stats();
    LOG("[frame-flow] per second: vblank %.1f; swap requests top %.1f bottom %.1f; buffer changes top %.1f bottom %.1f; copies top %.1f bottom %.1f; presents %.1f\n",
        (u32)(flow.vblanks - previous_flow.vblanks) / secs,
        (u32)(flow.requests[0] - previous_flow.requests[0]) / secs, (u32)(flow.requests[1] - previous_flow.requests[1]) / secs,
        (u32)(flow.changes[0] - previous_flow.changes[0]) / secs, (u32)(flow.changes[1] - previous_flow.changes[1]) / secs,
        st_pictures[0] / secs, st_pictures[1] / secs, fps);
    previous_flow = flow;
    const u32 top_swaps = kernel::gsp_swaps(0), bottom_swaps = kernel::gsp_swaps(1);
    LOG("[prof] compositions/s: top %.1f, bottom %.1f | guest LCD swaps/s: top %.1f, bottom %.1f\n",
        st_pictures[0] / secs, st_pictures[1] / secs, (top_swaps - last_lcd_swaps[0]) / secs, (bottom_swaps - last_lcd_swaps[1]) / secs);
    last_lcd_swaps[0] = top_swaps;
    last_lcd_swaps[1] = bottom_swaps;
    st_pictures[0] = st_pictures[1] = 0;
    LOG("[prof] VBlanks a top screen picture stayed up: 0 (never shown) %u, 1 %u, 2 %u, 3 %u, 4+ %u\n", st_pace[0], st_pace[1], st_pace[2], st_pace[3], st_pace[4]);
    memset(st_pace, 0, sizeof st_pace);
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
        if (vtx_cpu.draws[VCPU_GEOMETRY])
            n += snprintf(line + n, sizeof line - n, " (of which the vertex shader %.2f ms, the geometry shader's runs %.2f ms, of those about %.2f ms its EMITs: %.0f primitives)",
                          vtx_cpu.geometry_vs_ms / flips, vtx_cpu.geometry_gs_ms / flips, vtx_cpu.geometry_emit_ms / flips, vtx_cpu.geometry_prims / flips);
        if (n) LOG("[vtx] shaded on the CPU, per flip%s\n", line);
        if (gpu_fallback_draws[0] || gpu_fallback_draws[1] || gpu_fallback_draws[2] || gpu_fallback_draws[3])
            LOG("[fallback-route] missing vertex %llu; forced CPU %llu; missing geometry %llu; GPU resource limit %llu; CPU shader SIMD %u\n",
                (unsigned long long)gpu_fallback_draws[0],(unsigned long long)gpu_fallback_draws[1],(unsigned long long)gpu_fallback_draws[2],(unsigned long long)gpu_fallback_draws[3],(unsigned)g_cpu_vs_simd.load());
        for(auto &count:gpu_fallback_draws)count=0;
        vtx_report_hotspots();
        vtx_cpu = {};
    }
    LOG("[fragment-route] specialized %llu; generic unlit %llu lit %llu; simple %llu; material %llu; lighting %llu; extension %llu; bounded %u; forced %u\n",
        (unsigned long long)st_fragment_specialized, (unsigned long long)st_fragment_fallback[0],
        (unsigned long long)st_fragment_fallback[1], (unsigned long long)st_fragment_simple, (unsigned long long)st_material_draws, (unsigned long long)st_lighting_draws, (unsigned long long)st_extension_draws,
        (unsigned)bounded_fallback.load(std::memory_order_relaxed), (unsigned)uber_all.load(std::memory_order_relaxed));
    for (u32 lit = 0; lit < 2; ++lit) for (u32 stages = 0; stages <= 6; ++stages) for (u32 bucket = 0; bucket < 5; ++bucket) {
        auto &count = st_fallback_shape[lit][stages][bucket];
        if (count) LOG("[fragment-family] lit %u stages %u light_cap %u draws %llu\n", lit, stages, kUberLightCaps[bucket], (unsigned long long)count);
        count = 0;
    }
    st_extension_draws = st_lighting_draws = st_material_draws = st_fragment_specialized = st_fragment_simple = st_fragment_fallback[0] = st_fragment_fallback[1] = 0;
    LOG("[prof] vertex data per draw: %.1f streams, %.0f vertices, %.1f KB; %.0f%% of streams from the cache (%u entries, %u KB)\n", st_attr_streams / draws,
        st_attr_verts / draws, st_attr_bytes / 1024.0 / draws, 100.0 * st_vtx_hits / std::max<u64>(st_vtx_hits + st_vtx_misses, 1), (u32)vtx_cache.size(), (u32)(vtx_cache_size >> 10));
    LOG("[index-range] hits %llu misses %llu; CPU storage %u KB; enabled %u\n",
        (unsigned long long)index_range_hits,(unsigned long long)index_range_misses,
        (unsigned)(index_ranges.bytes()>>10),(unsigned)index_range_experiment.load(std::memory_order_relaxed));
    index_range_hits=index_range_misses=0;
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
    u32 px, py;
    if (!screens::pad_touch(pad_layout, x, y, 854, 480, px, py)) return;  // not on the bottom screen
    touch = true;
    tx = (u16)px;
    ty = (u16)py;
}
// The game gets nothing while the menu, the keyboard or the Mii list is up, nor the press or touch that closed one (an A that picked a Mii
// would be the game's next A) until everything is let go
void input_override(u32 &buttons, bool &touch, u16 &tx, u16 &ty) {
    (void)tx;
    (void)ty;
    static bool held_back;
    if (menu_is_open || kb_active || mii_selector_open()) held_back = true;
    else if (!buttons && !touch) held_back = false;
    if (held_back) {
        buttons = 0;
        touch = false;
    }
}
}  // namespace wiiu
