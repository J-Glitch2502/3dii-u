#pragma once
#include <cstdint>

namespace audio_ring {
// This output path keeps less than half a ring queued. A distance in the
// other half means playback passed the writer, not thousands of new samples.
// Treat it exactly as host_output's recovery path does, so the producer runs
// that path instead of sleeping while AX repeats stale ring contents.
constexpr uint32_t distance(uint32_t write, uint32_t play, uint32_t ring) {
    return (write + ring - play) % ring;
}
constexpr uint32_t queued(uint32_t raw, uint32_t ring) {
    return raw > ring / 2 ? 0 : raw;
}
} // namespace audio_ring
