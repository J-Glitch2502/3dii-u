// What the headless PC build leaves out: the Metal renderer (the software rasterizer stands in), cloud saves, audio output.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "audio/host.h"
#include "audio_iface.h"
#include "gpu/pica.h"
#include "rt.h"

void cloud_deleted() {}
void cloud_restored() {}
void cloud_save_written() {}
std::atomic<u32> g_live_host;
std::atomic<int> g_winshot_request;

namespace gpu {
bool metal_active() { return false; }
bool metal_init() { return false; }
void metal_flush(const RTConfig &, const MBatch &) {}
const void *metal_vs_kernel(const ShaderProgram *, u32, const ShaderSetup &) { return nullptr; }
void metal_guest_read(u32, u32) {}
void metal_guest_written(u32, u32, bool) {}
void metal_guest_prewrite(u32, u32) {}
void metal_writeback_all() {}
void metal_poll() {}
bool metal_defer_transfer(u32, u32, u32, u32, u32, u32) { return false; }
void metal_display_transfer(u32, u32, u32, u32, u32) {}
bool metal_gpu_made(u32, u32) { return false; }
void metal_note_gpu_output(u32, u32) {}
}  // namespace gpu

namespace onescreen {
u32 host_mask(const char *) { return 0; }
}
namespace stacks {
void save_written(const std::string &) {}
}

namespace audio {
void init() {}
void set_volume(float) {}
namespace hle {
// RECOMP_AUDIO_DUMP=<file>: what would have played, as raw stereo at 32728 Hz
void host_output(const Sample *samples, size_t n) {
    static FILE *f = getenv("RECOMP_AUDIO_DUMP") ? fopen(getenv("RECOMP_AUDIO_DUMP"), "wb") : nullptr;
    if (f) fwrite(samples, sizeof *samples, n, f);
}
int host_queued() { return -1; }
}  // namespace hle
}  // namespace audio
