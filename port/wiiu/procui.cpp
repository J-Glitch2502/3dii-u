#include <atomic>

#include <coreinit/debug.h>
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
enum class GpuPhase : unsigned { Running, PauseRequested, Parked, ResumeRequested, Rebuilding, Failed };

std::atomic<GpuPhase> Phase{GpuPhase::Running};
std::atomic<bool> ForegroundBack{false};
std::atomic<bool> HomeSlow{false};
std::atomic<int> HomeReleaseMs{-1};
std::atomic<bool> HomeReleaseParked{false};

constexpr uint32_t ReleaseTimeoutMs = 1500;
constexpr uint32_t ResumeTimeoutMs = 3000;

bool WaitForPhase(GpuPhase expected, uint32_t timeoutMs) {
    const OSTime deadline = OSGetSystemTime() + OSMillisecondsToTicks(timeoutMs);
    while (Phase.load(std::memory_order_acquire) != expected) {
        if (OSGetSystemTime() >= deadline) return false;
        OSSleepTicks(OSMillisecondsToTicks(2));
    }
    return true;
}

// ProcUI runs this before WHB releases display buffers. Returning without the
// GPU acknowledgment lets WHB free memory that the GPU thread still uses.
uint32_t OnRelease(void *) {
    const OSTime start = OSGetSystemTime();
    ForegroundBack.store(false, std::memory_order_release);
    kernel::gx_hold.store(true, std::memory_order_release);
    gpu::gx2_skip_pass.store(true, std::memory_order_release);
    Phase.store(GpuPhase::PauseRequested, std::memory_order_release);
    GpuPause.store(true, std::memory_order_release);

    const OSTime slowAt = start + OSMillisecondsToTicks(200);
    const OSTime deadline = start + OSMillisecondsToTicks(ReleaseTimeoutMs);
    bool slowReported = false;
    while (Phase.load(std::memory_order_acquire) != GpuPhase::Parked) {
        const OSTime now = OSGetSystemTime();
        if (!slowReported && now >= slowAt) {
            HomeSlow.store(true, std::memory_order_release);
            slowReported = true;
        }
        if (now >= deadline) {
            Phase.store(GpuPhase::Failed, std::memory_order_release);
            OSFatal("3Dii U: GPU did not stop for HOME Menu. Foreground release aborted.");
            __builtin_trap();
        }
        OSSleepTicks(OSMillisecondsToTicks(2));
    }
    HomeReleaseParked.store(true, std::memory_order_release);
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
    return Phase.load(std::memory_order_acquire) == GpuPhase::Parked;
}
bool IsForegroundBack() {
    return ForegroundBack.load(std::memory_order_acquire);
}
void MarkGpuParked() {
    Phase.store(GpuPhase::Parked, std::memory_order_release);
}
void MarkGpuResuming() {
    Phase.store(GpuPhase::Rebuilding, std::memory_order_release);
}
void MarkGpuRunning() {
    Phase.store(GpuPhase::Running, std::memory_order_release);
}

bool ResumeGpu() {
    if (!ForegroundBack.load(std::memory_order_acquire) || !IsGpuParked()) {
        LOG("[home] resume rejected: foreground=%d phase=%u\n", IsForegroundBack(), (unsigned)Phase.load(std::memory_order_acquire));
        return false;
    }
    gpu::gx2_skip_pass.store(false, std::memory_order_release);
    Phase.store(GpuPhase::ResumeRequested, std::memory_order_release);
    GpuPause.store(false, std::memory_order_release);
    if (!WaitForPhase(GpuPhase::Running, ResumeTimeoutMs)) {
        LOG("[home] GPU resume timeout: phase=%u\n", (unsigned)Phase.load(std::memory_order_acquire));
        return false;
    }
    kernel::gx_hold.store(false, std::memory_order_release);
    LOG("[wiiu] back in the foreground\n");
    return true;
}

void ReportReleaseStatus() {
    if (HomeSlow.exchange(false, std::memory_order_acq_rel)) {
        LOG("[home] the GPU thread still hasn't stopped\n");
        wiiu::log_threads();
    }
    const int releaseMs = HomeReleaseMs.exchange(-1, std::memory_order_acq_rel);
    if (releaseMs >= 0) {
        LOG("[home] %s after %d ms\n", HomeReleaseParked.exchange(false, std::memory_order_acq_rel) ? "GPU parked" : "GPU not parked",
            releaseMs);
    }
}

void SimulateRelease() {
    OnRelease(nullptr);
}
void SimulateAcquire() {
    OnAcquire(nullptr);
}
} // namespace wiiu::procui
