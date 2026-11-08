#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>

#include "kernel.h"
#include "procui.hpp"
#include "rt.h"
#include "services/services.h"
#include "wiiu.h"

namespace gpu {
extern bool gx2_skip_pass;
}

namespace wiiu::procui {
namespace {
volatile bool GpuPause;
volatile bool GpuParked;
volatile bool ForegroundBack;
volatile bool HomeSlow;
volatile int HomeReleaseMs = -1;

// Stop the GPU thread before WHB releases GX2 resources on core 2.
uint32_t OnRelease(void *) {
    const OSTime start = OSGetSystemTime();
    ForegroundBack = false;
    kernel::gx_hold = true;
    gpu::gx2_skip_pass = true;
    GpuPause = true;
    for (int i = 0; i < 500 && !GpuParked; i++) {
        OSSleepTicks(OSMillisecondsToTicks(2));
        if (i == 100) HomeSlow = !GpuParked;
    }
    HomeReleaseMs = (int)OSTicksToMilliseconds(OSGetSystemTime() - start);
    return 0;
}

uint32_t OnAcquire(void *) {
    ForegroundBack = true;
    return 0;
}
} // namespace

void RegisterCallbacks() {
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_RELEASE, OnRelease, nullptr, 1000, 2);
    ProcUIRegisterCallbackCore(PROCUI_CALLBACK_ACQUIRE, OnAcquire, nullptr, 50, 2);
}

bool IsPauseRequested() {
    return GpuPause;
}
bool IsGpuParked() {
    return GpuParked;
}
bool IsForegroundBack() {
    return ForegroundBack;
}
void SetGpuParked(bool parked) {
    GpuParked = parked;
}

void ResumeGpu() {
    for (int i = 0; i < 1000 && !GpuParked; i++) OSSleepTicks(OSMillisecondsToTicks(2));
    gpu::gx2_skip_pass = false;
    GpuPause = false;
    kernel::gx_hold = false;
    while (GpuParked) OSSleepTicks(OSMillisecondsToTicks(1));
    LOG("[wiiu] back in the foreground\n");
}

void ReportReleaseStatus() {
    if (HomeSlow) {
        HomeSlow = false;
        LOG("[home] the GPU thread still hasn't stopped\n");
        wiiu::log_threads();
    }
    if (HomeReleaseMs >= 0) {
        LOG("[home] %s after %d ms\n", GpuParked ? "the GPU thread stopped" : "gave up on the GPU thread", HomeReleaseMs);
        HomeReleaseMs = -1;
    }
}

void SimulateRelease() {
    OnRelease(nullptr);
}
void SimulateAcquire() {
    OnAcquire(nullptr);
}
} // namespace wiiu::procui
