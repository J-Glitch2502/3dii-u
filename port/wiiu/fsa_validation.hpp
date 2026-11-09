#pragma once

#include <cstddef>
#include <cstdint>

namespace wiiu::fsa {
enum class BufferValidation { Ok, InvalidBuffer, InvalidAlignment, OutOfRange };

inline BufferValidation ValidateBuffer(const void *data, uint32_t size, uint32_t count) {
    constexpr uint32_t MaxBytes = 0x7FFFFFFF;
    if (size && count > MaxBytes / size) return BufferValidation::OutOfRange;
    if (!size || !count) return BufferValidation::Ok;
    if (!data) return BufferValidation::InvalidBuffer;
    if (reinterpret_cast<uintptr_t>(data) & 63) return BufferValidation::InvalidAlignment;
    return BufferValidation::Ok;
}

inline bool ValidRange(uint64_t offset, uint64_t length) {
    return offset <= UINT32_MAX && length <= UINT32_MAX - offset;
}

inline bool ValidString(const char *value, size_t maxLength) {
    if (!value || !*value) return false;
    for (size_t i = 1; i <= maxLength; ++i) {
        if (value[i] == 0) return true;
    }
    return false;
}
} // namespace wiiu::fsa
