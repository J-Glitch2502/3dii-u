#pragma once

namespace wiiu::procui {
void RegisterCallbacks();
bool IsPauseRequested();
bool IsGpuParked();
bool IsForegroundBack();
void SetGpuParked(bool parked);
void ResumeGpu();
void ReportReleaseStatus();
void SimulateRelease();
void SimulateAcquire();
} // namespace wiiu::procui
