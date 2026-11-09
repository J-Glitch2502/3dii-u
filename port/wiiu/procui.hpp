#pragma once

namespace wiiu::procui {
void RegisterCallbacks();
bool IsPauseRequested();
bool IsGpuParked();
bool IsForegroundBack();
void MarkGpuParked();
void MarkGpuResuming();
void MarkGpuRunning();
bool ResumeGpu();
void ReportReleaseStatus();
void SimulateRelease();
void SimulateAcquire();
} // namespace wiiu::procui
