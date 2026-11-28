// Allocator and GPU operations are observable stand-ins for surface lifetime and layout tests.
#include "surface_readback.h"
#include "scaled_copy.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

using u32 = uint32_t;
using u64 = uint64_t;
using u8 = uint8_t;
struct Tex {
    u32 bytes, row_bytes;
    u32 slice[32], dirty_rows[4];
};
namespace kernel {
struct FramebufferInfo {
    bool valid;
    u32 addr_left, addr_right;
    u32 format = 0;
    u32 stride = 0;
};
static FramebufferInfo lcd[2];
static u32 swaps[2];
static FramebufferInfo gsp_framebuffer(int screen) { return lcd[screen]; }
static u32 gsp_swaps(int screen) { return swaps[screen]; }
}
namespace mem {
static constexpr u32 VRAM_PADDR = 0x18000000, VRAM_SIZE = 0x600000;
static constexpr u32 FCRAM_PADDR = 0x20000000, LINEAR_SIZE = 0x08000000;
static u32 vaddr_to_paddr(u32 addr) { return addr; }
}
struct DrawState {};
struct RTConfig {};
enum { GX2_SQ_SEL_A = 3, GX2_SQ_SEL_1 = 5, GX2_TILE_MODE_DEFAULT = 0, GX2_CLEAR_FLAGS_BOTH = 3 };
enum GX2InvalidateMode { GX2_INVALIDATE_MODE_DEPTH_BUFFER = 1, GX2_INVALIDATE_MODE_COLOR_BUFFER = 2, GX2_INVALIDATE_MODE_TEXTURE = 4 };
static u32 invalidated_size;
static void GX2Invalidate(GX2InvalidateMode, void *, u32 size) { invalidated_size = size; }
struct GX2Surface { u32 width, height, imageSize, alignment, tileMode; void *image; };
struct DepthBuffer { GX2Surface surface; float depthClear; u8 stencilClear; };
struct ColorBuffer { GX2Surface surface; };
struct Texture { u32 compMap = 0x00010203; GX2Surface surface{}; };
static void GX2InitTextureRegs(Texture *) {}
static void GX2InitColorBufferRegs(ColorBuffer *) {}
static void GX2InitDepthBufferRegs(DepthBuffer *) {}
static void GX2SetDepthBuffer(DepthBuffer *) {}
static void GX2ClearDepthStencilEx(DepthBuffer *, float, u8, u32) {}
static void GX2DrawDone() {}
static void DCFlushRange(void *, u32) {}
static void restore_context() {}
static void bind_targets(const RTConfig &) {}
static void tv_buffer_to_mem2() {}
static void GX2CalcSurfaceSizeAndAlignment(GX2Surface *f) {
    f->imageSize = f->width * f->height * 4;
    f->alignment = f->width >= 700 ? 4096 : 256;
    f->tileMode = 1;
}
struct Surface {
    u32 addr = 0, w = 400, h = 240, fmt = 0;
    bool depth = true, mem1 = true, shared_depth = true, in_list = false;
    float base_sx = 1, base_sy = 1;
    float sx = 1, sy = 1, dyn_f = 1;
    u32 sw, sh, row0, dyn_level = 0;
    u64 last_use = 0, drawn = 0, written_after = 0;
    bool gpu_newer = false, guest_newer = false, lcd_bottom = false, linear = false;
    bool copy = false, gpu_chain = false;
    bool clear_pending = false, written_since_flush = false, first_use = false;
    surface_readback::Spans written;
    Texture tex;
    ColorBuffer cb{};
    DepthBuffer db{};
    u32 length = 16, tex_h = 240;
    u32 bytes() const { return length; }
    void *image() { return depth ? db.surface.image : cb.surface.image; }
    u32 image_size() { return depth ? db.surface.imageSize : cb.surface.imageSize; }
};
std::vector<std::unique_ptr<Surface>> surfaces;
Surface *cur_color, *cur_depth, *depth_owner;
u64 use_counter;
bool state_sent_valid, state_lost;
unsigned heap_frees, mem1_frees, shared_frees, allocations;
size_t next_surface_retirement;
unsigned failures;
std::array<uint8_t, 64> depth_storage;
static void *depth_block;
static u32 depth_block_size;
static bool mem1_relief;
static void *(*GfxHeapAllocMEM1)(u32, u32);
static void *for_gpu_writes(void *p, u32) { return p; }
alignas(4096) static u8 shared_depth_block, private_depth;
static u32 allocated_depth_size, allocated_depth_alignment;
static void *alloc_surface_mem(u32 size, u32 alignment, bool &mem1, bool) {
    allocated_depth_size = size;
    allocated_depth_alignment = alignment;
    mem1 = false;
    return &private_depth;
}

static bool contains(const Surface *s) {
    return std::any_of(surfaces.begin(), surfaces.end(), [&](const auto &p) { return p.get() == s; });
}
static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
static void heap_free(void *p) {
    ++heap_frees;
    if (p == depth_storage.data()) ++shared_frees;
}
static void mem1_free(void *p) {
    ++mem1_frees;
    if (p == depth_storage.data()) ++shared_frees;
}
void (*GfxHeapFreeMEM1)(void *) = mem1_free;
static u64 st_draws, g_frame, gpu_writes;
static constexpr bool kMeasure = false;
static bool left_done, right_follows_left, skip_seen;
static bool gxt() { return false; }
static void gx2_flush_cpu_batch() {}
static bool linear_copy_on_gpu(u32, u32, u32, u32, u32) { return false; }
static bool tile_run_copy(u32, u32, u32, u32, u32) { return false; }
static std::array<u32, 2> copy_size;
static Surface copy_target;
static Surface *staged_copy;
static bool gpu_copy_transfer(u32, u32 dst, u32, u32, u32, Surface **copied = nullptr) {
    if (!copied || !staged_copy || staged_copy->addr != dst) return false;
    staged_copy->linear = true;
    staged_copy->drawn = ++gpu_writes;
    *copied = staged_copy;
    return true;
}
static u32 cbuf_to_pixfmt(u32 fmt) { return fmt; } // RGBA8 fixture.
static u32 pixel_bytes(u32 fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }
static void LOG(const char *, ...) {}
static unsigned displays;
static int shown_screen;
static bool shown_flip;
static float shown_low, shown_high;
static Texture *shown_texture;
static void show_screen(int which, Texture *tex, bool flip, float lo, float hi, float = 1) {
    ++displays;
    shown_screen = which;
    shown_flip = flip;
    shown_low = lo;
    shown_high = hi;
    shown_texture = tex;
}
#define free heap_free
#include "surface_lifetime_production.inc"
#undef free

static void reset() {
    surfaces.clear();
    cur_color = cur_depth = depth_owner = nullptr;
    for (auto &t : last_transfer) t = {};
    use_counter = 0;
    next_surface_retirement = 0;
    state_sent_valid = true;
    state_lost = false;
    heap_frees = mem1_frees = shared_frees = allocations = 0;
    invalidated_size = 0;
}
static Surface *append(u32 key, bool depth = true, bool shared = true, bool mem1 = true) {
    auto p = std::make_unique<Surface>();
    p->addr = 0x18000000 + key * 0x1000;
    p->depth = depth;
    p->shared_depth = shared;
    p->mem1 = mem1;
    GX2Surface &storage = depth ? p->db.surface : p->cb.surface;
    storage.image = shared ? depth_storage.data() : reinterpret_cast<void *>(uintptr_t(key) * 64 + 0x10000);
    used(*p);
    Surface *s = p.get();
    surfaces.push_back(std::move(p));
    ++allocations;
    return s;
}
static Surface *reuse_or_append(u32 key) {
    // Only allocation is a fixture stand-in; matching and marking use are production code.
    if (auto *s = get_surface(0x18000000 + key * 0x1000, 400, 240, 0, true)) return s;
    return append(key);
}

static void churn() {
    reset();
    size_t peak = 0;
    for (unsigned frame = 0; frame < 6000; ++frame) {
        use_counter = frame;
        depth_owner = append(frame + 1);
        forget_unused();
        peak = std::max(peak, surfaces.size());
    }
    printf("distinct shared-depth keys=6000, retained=%zu, peak_after_retire=%zu\n", surfaces.size(), peak);
    check(surfaces.size() == 1800, "shared descriptor churn must obey the existing 1800-present age");
    check(peak == 1800, "shared descriptor churn must plateau at one new descriptor per present");
    for (unsigned i = 0; i < 2000; ++i) {
        ++use_counter;
        forget_unused();
    }
    check(surfaces.size() == 1 && surfaces.front().get() == depth_owner, "only the current shared-block owner remains pinned");
    check(heap_frees == 0 && mem1_frees == 0 && shared_frees == 0, "retiring borrowed descriptors must never free backing storage");
    check(!state_sent_valid && state_lost, "retirement invalidates the sent GPU-state record");
}
static void pins_and_ownership() {
    reset();
    auto *owner = append(1);
    auto *depth = append(2);
    auto *color = append(3, false, false);
    auto *in_list = append(4, false, false);
    auto *transfer = append(5, false, false);
    auto *recent = append(6);
    auto *stale = append(7);
    depth_owner = owner;
    cur_depth = depth;
    cur_color = color;
    in_list->in_list = true;
    last_transfer[0].src = transfer;
    use_counter = 10000;
    recent->last_use = 9000;
    forget_unused();
    check(!contains(stale), "an inactive borrowed descriptor is retired");
    for (auto *p : {owner, depth, color, in_list, transfer, recent}) check(contains(p), "all active/current/list/transfer/recent pins remain");
    check(mem1_frees == 0 && heap_frees == 0, "first borrowed retirement does not free storage");
    cur_color = cur_depth = nullptr;
    in_list->in_list = false;
    for (auto &t : last_transfer) t = {};
    use_counter += 1800;
    for (unsigned i = 0; i < 10; ++i) forget_unused();
    check(surfaces.size() == 1 && contains(owner), "released pins retire while the shared owner remains valid");
    check(mem1_frees == 3 && heap_frees == 0 && shared_frees == 0, "only the three owned color allocations are freed once");

    reset();
    auto *dirty = append(1, false, false, false);
    dirty->gpu_newer = true;
    append(2, false, false, true);
    use_counter = 1800;
    forget_unused();
    check(surfaces.size() == 2 && heap_frees == 0 && mem1_frees == 0,
          "the first check keeps the dirty target and leaves the next candidate for the next present");
    forget_unused();
    check(surfaces.size() == 1 && contains(dirty) && heap_frees == 0 && mem1_frees == 1,
          "only the synchronized target retires on a timeout");
    surface_readback::guest_wrote(*dirty, dirty->addr, dirty->bytes());
    forget_unused();
    check(surfaces.empty() && heap_frees == 1 && mem1_frees == 1 && shared_frees == 0,
          "a fully superseded target also retires and frees its allocation once");
}
static void repeated_keys() {
    reset();
    for (unsigned frame = 0; frame < 6000; ++frame) {
        use_counter = frame;
        depth_owner = reuse_or_append(1 + frame % 8);
        forget_unused();
    }
    printf("repeated keys=8, frames=6000, descriptors=%zu, allocations=%u\n", surfaces.size(), allocations);
    check(surfaces.size() == 8 && allocations == 8, "production matching reuses repeated keys without descriptor growth");
    check(mem1_frees == 0 && heap_frees == 0, "warm key reuse needs no retirement or free");
}

static void relocate_reduced_depth() {
    reset();
    auto *depth = append(1, true, false);
    depth->w = 240;
    depth->h = 400;
    depth->tex_h = 512;
    depth->base_sx = depth->base_sy = 3;
    apply_scale(*depth);
    const GX2Surface full = depth->db.surface;
    depth->dyn_level = 1;
    depth->dyn_f = .75f;
    depth->written_since_flush = true;
    apply_scale(*depth);
    const GX2Surface reduced = depth->db.surface;
    check(invalidated_size == full.imageSize && !depth->written_since_flush,
          "resizing flushes the entire old target before replacing the GX2 layout");

    depth_block = &shared_depth_block;
    depth_block_size = reduced.imageSize;
    GX2Surface reduced_only = reduced;
    check(share_depth_block(reduced_only) && full.imageSize > depth_block_size,
          "a reduced-only descriptor would borrow storage too small for the full pass");
    check(full.alignment > reduced.alignment, "the fixture requires stronger alignment at full resolution");
    relieve_mem1();
    check(mem1_relief && !depth->mem1 && !depth->shared_depth && allocated_depth_size >= full.imageSize &&
          allocated_depth_alignment >= full.alignment,
          "MEM1 relief rejects the small shared block and reserves the full allocation in MEM2");
    check(depth->db.surface.imageSize == reduced.imageSize && depth->db.surface.alignment == reduced.alignment,
          "MEM1 relief preserves the active reduced layout");

    depth->mem1 = true;
    depth_block_size = full.imageSize;
    relieve_mem1();
    check(depth->shared_depth && depth->db.surface.image == depth_block, "MEM1 relief can borrow a full-size block");
    allocated_depth_size = allocated_depth_alignment = 0;
    depth->clear_pending = false;
    cur_depth = depth;
    take_depth_block({});
    check(!depth->shared_depth && depth->db.surface.image != depth_block &&
          allocated_depth_size >= full.imageSize && allocated_depth_alignment >= full.alignment,
          "detaching a reduced depth target reserves its full size and alignment");
    check(depth->db.surface.imageSize == reduced.imageSize && depth->db.surface.alignment == reduced.alignment,
          "detaching preserves the active reduced layout");

    void *replacement = depth->db.surface.image;
    depth->dyn_level = 0;
    depth->dyn_f = 1;
    apply_scale(*depth);
    const GX2Surface &restored = depth->db.surface;
    check(restored.image == replacement && restored.imageSize == full.imageSize && restored.alignment == full.alignment &&
          restored.imageSize <= allocated_depth_size && uintptr_t(restored.image) % restored.alignment == 0,
          "returning to full resolution reuses the detached allocation safely");
    depth->db.surface.image = nullptr;
    depth->written_since_flush = true;
    invalidated_size = 0;
    apply_scale(*depth);
    check(!invalidated_size, "reacquiring lost MEM1 skips invalidation of the unmapped image");
}

static void dirty_churn() {
    reset();
    constexpr size_t count = 32;
    for (unsigned key = 1; key <= count; ++key) append(key, false, false)->gpu_newer = true;
    auto *superseded = surfaces.back().get();
    surface_readback::guest_wrote(*superseded, superseded->addr, superseded->bytes());
    use_counter = 1800;
    for (size_t i = 0; i + 1 < count; ++i) forget_unused();
    check(surfaces.size() == count && mem1_frees == 0, "only one idle dirty target is examined per present");
    forget_unused();
    check(surfaces.size() == count - 1 && mem1_frees == 1, "round robin reaches the superseded target");
}

static void reuse_latest_color() {
    reset();
    auto *old = append(1, false, false);
    auto *latest = append(1, false, false);
    auto *depth = append(1);
    old->drawn = 2;
    latest->drawn = 3;
    depth->drawn = 4;
    use_counter = 10;
    auto compatible = [](const Surface &s) { return !s.guest_newer; };
    check(find_surface(old->addr, false, compatible) == latest && latest->last_use == 10,
          "texture lookup must select the latest compatible surface");
    latest->guest_newer = true;
    check(find_surface(old->addr, false, compatible) == old, "an incompatible alias must not hide the matching view");
    check(get_surface(old->addr, old->w, old->h, 3, false) == old && old->tex.compMap == 0x00010205,
          "a reused RGB565 target samples constant one for alpha");
    check(get_surface(old->addr, old->w, old->h, 4, false) == old && old->tex.compMap == 0x00010203,
          "RGBA4444 reads the stored alpha again");
}

static void whole_tiled_copy() {
    reset();
    auto *src = append(1, false, false);
    src->gpu_newer = true;
    src->sy = 2;
    src->w = 240;
    src->h = 320;
    src->length = src->w * src->h * 4;
    src->drawn = 1;
    constexpr u32 target = mem::VRAM_PADDR + 0x300000;
    constexpr u32 limit = mem::VRAM_PADDR + mem::VRAM_SIZE;
    copy_target.linear = copy_target.lcd_bottom = true;
    check(gx2_texture_copy(src->addr, target, src->bytes(), 60, 60) &&
          copy_size == std::array<u32, 2>{240, 320}, "a complete tiled LCD copy preserves 240x320");
    check(copy_target.copy && copy_target.gpu_chain && !copy_target.linear && !copy_target.lcd_bottom,
          "a full tile copy replaces a reused destination's linear LCD layout");
    check(gx2_texture_copy(src->addr, limit - src->bytes(), src->bytes(), 60, 60),
          "the complete output may end exactly at the VRAM boundary");
    check(!gx2_texture_copy(src->addr, limit - src->bytes() + 16, src->bytes(), 60, 60),
          "a whole tiled copy must stay inside VRAM");
    check(!gx2_texture_copy(src->addr, target, src->bytes() - 16, 60, 60),
          "a partial copy cannot replace the target");
    src->linear = true;
    check(!gx2_texture_copy(src->addr, target, src->bytes(), 60, 60),
          "a linear source does not replace a tiled target");
    src->linear = false;
    auto *overlap = append(2, false, false);
    overlap->drawn = 2;
    check(!gx2_texture_copy(src->addr, target, src->bytes(), 60, 60),
          "the later overlapping color surface needs guest-byte materialization");
    overlap->depth = true;
    check(gx2_texture_copy(src->addr, target, src->bytes(), 60, 60),
          "depth buffers do not own the tiled color bytes");
    src->w = src->h = 256;
    src->length = src->w * src->h * 4;
    constexpr u32 row_units = 256 * 8 * 4 / 16;
    check(gx2_texture_copy(src->addr, target, src->bytes(), row_units, (row_units << 16) | row_units) &&
          copy_size == std::array<u32, 2>{512, 256}, "gapped tile rows keep their power-of-two allocation");
}

static void keep_verified_texture_hash() {
    constexpr u32 row_bytes = 16384, tile_rows = 64;
    std::vector<u32> guest(row_bytes / 4 * tile_rows);
    std::vector<u32> uploaded = guest;
    auto bytes = [](const auto &words) { return reinterpret_cast<const u8 *>(words.data()); };
    Tex tex{row_bytes * tile_rows, row_bytes, {}, {1, 0, 0, 0}};
    hash_guest(bytes(guest), tex.bytes, tex.slice);
    guest[1] = 11; // The notified row.
    guest[row_bytes / 4 + 1] = 22; // Another row in the slice we rehash.
    u32 rows[4]{1, 0, 0, 0};
    const u32 partial_hash = hash_partial_upload(tex, bytes(guest), rows);
    check(rows[0] == 3 && !rows[1] && !rows[2] && !rows[3],
          "a partial hash covers its whole slice and leaves other slices untouched");
    for (u32 r = 0; r < tile_rows; r++)
        if ((rows[r >> 5] >> (r & 31)) & 1)
            std::copy_n(guest.begin() + r * (row_bytes / 4), row_bytes / 4, uploaded.begin() + r * (row_bytes / 4));
    u32 slices[32];
    check(hash_guest(bytes(uploaded), tex.bytes, slices) == partial_hash, "recorded hash matches the rows actually uploaded");
    guest[13 * row_bytes / 4 + 1] = 33;
    check(hash_guest(bytes(guest), tex.bytes, slices) != partial_hash,
          "a later unsampled write in the same texture can still be detected");
}

static void locate_complete_lcd() {
    const u32 top = 0x18000000, bottom = 0x18100000, row = 720;
    kernel::lcd[0] = {true, top + 16 * row, top + 0x80000};
    kernel::lcd[1] = {true, bottom, bottom};
    u32 skip = 0;
    check(screen_of(top, row * 416, 416, skip) == 0 && skip == 16 * row,
          "cropped top transfer includes all 400 LCD rows");
    check(screen_of(top + 16 * row, row * 399, 399, skip) < 0,
          "partial top transfer cannot be shown as a complete frame");
    check(screen_of(top + 0x80000, row * 400, 400, skip) == 2, "registered stereo transfer still routes to the right eye");
    check(screen_of(bottom, row * 319, 319, skip) < 0, "partial bottom transfer cannot overwrite the full GamePad");
    check(screen_of(bottom - 1, row * 320, 320, skip) < 0, "LCD offsets must align to transfer rows");
    check(screen_of(bottom - 80 * row, row * 400, 400, skip) == 1 && skip == 80 * row,
          "full padded GamePad transfer still routes to its registered LCD");
}

static void show_scaled_crop_and_keep_offscreen() {
    reset();
    auto *target = append(1, false, false);
    target->w = 480;
    target->h = target->tex_h = 800;
    target->length = 480 * 800 * 4;
    const u32 bottom = 0x19000000, row = 240 * 4, raw = 480 | (800 << 16);
    kernel::lcd[1] = {true, bottom + 60 * row, bottom + 60 * row};
    check(metal_defer_transfer(target->addr, bottom, raw, raw, (2u << 24) | 1, row * 400), "scaled GamePad transfer is consumed");
    check(shown_screen == 1 && shown_flip && std::abs(shown_low - .15f) < .00001f && std::abs(shown_high - .95f) < .00001f,
          "flip and 2x downscale map all 320 visible rows back to their 640 source rows");

    target->w = 240;
    target->h = target->tex_h = 400;
    target->length = 240 * 400 * 4;
    const u32 left = 0x19500000, spare = 0x19600000, size = row * 400, dims = 240 | (400 << 16);
    kernel::lcd[0] = {true, left, left};
    left_done = gx2_skip_pass = false;
    displays = 0;
    for (unsigned i = 0; i < 80; ++i) {
        st_draws += 16;
        check(metal_defer_transfer(target->addr, left, dims, dims, 0, size), "registered left screen is shown");
        ++kernel::swaps[0];
        check(!gx2_skip_pass, "a history of unregistered transfers cannot disable guest draws");
        st_draws += 16;
        check(metal_defer_transfer(target->addr, spare, dims, dims, 0, size), "off-screen transfer remains available");
    }
    check(displays == 81, "only the first unknown transfer is provisionally displayed");
    check(last_transfer[2].src == target && last_transfer[2].out_paddr == spare,
          "later texture reads can still use an off-screen transfer");
}

static void compose_selected_bottom_buffer() {
    reset();
    auto *source = append(2, false, false);
    auto *front = append(3, false, false);
    auto *back = append(4, false, false);
    for (auto *s : {source, front, back}) {
        s->w = 240;
        s->h = s->tex_h = 320;
        s->length = 240 * 320 * 4;
    }
    const u32 dims = 240 | (320 << 16);
    kernel::lcd[1] = {true, back->addr, back->addr};
    u32 skip = 0;
    screen_of(back->addr, back->length, 320, skip); // The game has previously displayed both.
    kernel::lcd[1] = {true, front->addr, front->addr};
    displays = 0;
    last_transfer[0] = {source, source->addr, front->addr, dims, dims, 0, front->length};
    last_transfer[2] = {source, source->addr, source->addr, dims, dims, 0, source->length};
    for (auto *out : {front, back}) {
        staged_copy = out;
        check(metal_defer_transfer(source->addr, out->addr, dims, dims, 0, out->length) && out->lcd_bottom,
              "GamePad transfer preserves its selected destination");
    }
    staged_copy = nullptr;
    check(!last_transfer[0].src && last_transfer[2].src, "a new framebuffer copy supersedes only its stale deferred transfer");
    check(displays == 0, "two writes between refreshes do not composite twice");
    show_bottom_lcd();
    check(displays == 1 && shown_texture == &front->tex, "the LCD selects the front buffer despite the newer back transfer");
    show_bottom_lcd();
    check(displays == 1, "an untouched visible picture is not redrawn");
    kernel::lcd[1] = {true, back->addr, back->addr};
    kernel::lcd[1].stride = 256 * 4;
    show_bottom_lcd();
    check(displays == 1, "a differently pitched LCD does not reuse a tightly packed copy");
    kernel::lcd[1].stride = 240 * 4;
    show_bottom_lcd();
    check(displays == 2 && shown_texture == &back->tex, "a later LCD swap presents the preserved back buffer");
    const u32 other = 0x19700000;
    check(metal_defer_transfer(source->addr, other, dims, dims, 0, front->length), "unknown GamePad transfer uses legacy presentation");
    show_bottom_lcd();
    check(displays == 4 && shown_texture == &back->tex,
          "a fallback transfer to a different address does not discard the LCD's retained image");
    check(metal_defer_transfer(source->addr, back->addr, dims, dims, 0, back->length), "direct transfer updates the current destination");
    show_bottom_lcd();
    check(displays == 5 && shown_texture == &source->tex, "a fallback write supersedes an older retained copy of that destination");
}
static void detect_cpu_lcd_frames() {
    constexpr u32 row = 720, frame = row * 400;
    constexpr u32 a = mem::FCRAM_PADDR, b = a + frame, end = a + mem::LINEAR_SIZE;
    check(lcd_buffer_size(end - frame, row, row) == frame, "a frame ending at FCRAM's boundary is readable");
    check(lcd_buffer_size(end - frame + 1, row, row) == 0, "a frame crossing FCRAM's boundary is rejected");
    check(lcd_buffer_size(a, row - 1, row) == 0 && lcd_buffer_size(a, UINT32_MAX, row) == 0,
          "undersized and overflowing strides are rejected");
    check(lcd_buffer_size(end - frame - 399 * 8, row + 8, row) == frame + 399 * 8,
          "row padding after the visible pixels of the last row is unnecessary");
    check(lcd_buffer_size(mem::VRAM_PADDR, row, row) == frame, "tracked video writes to VRAM still have a valid span");
    CpuLcdFrames frames{};
    check(!frames.observe(a, row, 1) && !frames.observe(b, row, 2) &&
          !frames.observe(a, row, 1) && !frames.observe(b, row, 2),
          "alternating stale buffers don't activate CPU video");
    check(frames.observe(a, row, 3) && frames.observe(a, row, 3) && frames.observe(b, row, 4),
          "pixel changes activate each buffer and allow a presentation retry");
    check(!frames.observe(a, row + 8, 5), "a format or stride change starts a fresh baseline");
    frames = {};
    check(!frames.observe(b, row, 5), "a display transfer resets CPU video detection");
}

static void cover_fractional_copy_edges() {
    for (float scale : {0.75f, 1.0f, 1.85f, 2.75f, 3.0f}) {
        u32 end = 0;
        for (u32 row = 0; row < 416; row += 16) {
            const auto span = gx2_copy::span(row, 16, scale);
            check(span.first == end, "adjacent scaled copies share their rounded top and bottom edges");
            end = span.first + span.size();
        }
        check(end == (u32)(416 * scale), "the sum of adjacent copies reaches the last texel");
    }
}

int main() {
    reuse_latest_color();
    whole_tiled_copy();
    keep_verified_texture_hash();
    locate_complete_lcd();
    show_scaled_crop_and_keep_offscreen();
    compose_selected_bottom_buffer();
    detect_cpu_lcd_frames();
    cover_fractional_copy_edges();
    churn();
    pins_and_ownership();
    dirty_churn();
    repeated_keys();
    relocate_reduced_depth();
    printf("surface descriptor lifetime: failures=%u\n", failures);
    return failures ? 1 : 0;
}
