#include <atomic>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>

#include "procui.hpp"
#include "gpu/pica.h"
#include "rt.h"
#include "services/services.h"
#include "wiiu.h"

namespace wiiu::procui {
namespace {
std::atomic<bool> GpuPause{false};
std::atomic<bool> GpuParked{false};
std::atomic<bool> ForegroundBack{false};
std::atomic<bool> HomeSlow{false};
std::atomic<int> HomeReleaseMs{-1};

// Stop the GPU thread before WHB releases GX2 resources on core 2.
uint32_t OnRelease(void *) {
    const OSTime start = OSGetSystemTime();
    ForegroundBack.store(false, std::memory_order_release);
    kernel::gx_hold.store(true, std::memory_order_release);
    gpu::gx2_skip_pass.store(true, std::memory_order_release);
    GpuPause.store(true, std::memory_order_release);
    for (int i = 0; i < 500 && !GpuParked.load(std::memory_order_acquire); i++) {
        OSSleepTicks(OSMillisecondsToTicks(2));
        if (i == 100) HomeSlow.store(!GpuParked.load(std::memory_order_acquire), std::memory_order_release);
    }
    HomeReleaseMs.store((int)OSTicksToMilliseconds(OSGetSystemTime() - start), std::memory_order_release);
    return 0;
}

uint32_t OnAcquire(void *) {
    ForegroundBack.store(true, std::memory_order_release);
    return 0;
}
} // namespace

void RegisterCallbacks() {
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_RELEASE, OnRelease, nullptr, 1000, 2);
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_ACQUIRE, OnAcquire, nullptr, 50, 2);
}

bool IsPauseRequested() {
    return GpuPause.load(std::memory_order_acquire);
}
bool IsGpuParked() {
    return GpuParked.load(std::memory_order_acquire);
}
bool IsForegroundBack() {
    return ForegroundBack.load(std::memory_order_acquire);
}
void SetGpuParked(bool parked) {
    GpuParked.store(parked, std::memory_order_release);
}

void ResumeGpu() {
    for (int i = 0; i < 1000 && !GpuParked.load(std::memory_order_acquire); i++) OSSleepTicks(OSMillisecondsToTicks(2));
    gpu::gx2_skip_pass.store(false, std::memory_order_release);
    GpuPause.store(false, std::memory_order_release);
    kernel::gx_hold.store(false, std::memory_order_release);
    while (GpuParked.load(std::memory_order_acquire)) OSSleepTicks(OSMillisecondsToTicks(1));
    LOG("[wiiu] back in the foreground\n");
}

void ReportReleaseStatus() {
    if (HomeSlow.exchange(false, std::memory_order_acq_rel)) {
        LOG("[home] the GPU thread still hasn't stopped\n");
        wiiu::log_threads();
    }
    const int releaseMs = HomeReleaseMs.exchange(-1, std::memory_order_acq_rel);
    if (releaseMs >= 0) {
        LOG("[home] %s after %d ms\n", IsGpuParked() ? "the GPU thread stopped" : "gave up on the GPU thread", releaseMs);
    }
}

void SimulateRelease() {
    OnRelease(nullptr);
}
void SimulateAcquire() {
    OnAcquire(nullptr);
}
} // namespace wiiu::procui
