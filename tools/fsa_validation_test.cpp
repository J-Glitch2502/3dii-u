#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

#include "../port/wiiu/fsa_validation.hpp"

int main() {
    using namespace wiiu::fsa;
    alignas(64) uint8_t buffer[128]{};
    assert(ValidateBuffer(buffer, 1, 128) == BufferValidation::Ok);
    assert(ValidateBuffer(buffer + 64, 64, 1) == BufferValidation::Ok);
    assert(ValidateBuffer(buffer + 1, 1, 1) == BufferValidation::InvalidAlignment);
    assert(ValidateBuffer(nullptr, 1, 1) == BufferValidation::InvalidBuffer);
    assert(ValidateBuffer(nullptr, 0, 1) == BufferValidation::Ok);
    assert(ValidateBuffer(nullptr, 1, 0) == BufferValidation::Ok);
    assert(ValidateBuffer(buffer, UINT32_MAX, 2) == BufferValidation::OutOfRange);
    assert(ValidateBuffer(buffer, UINT32_MAX, 1) == BufferValidation::OutOfRange);
    assert(ValidateBuffer(buffer, 0x7FFFFFFF, 1) == BufferValidation::Ok);
    assert(ValidateBuffer(buffer, 0x40000000, 2) == BufferValidation::OutOfRange);
    assert(ValidateBuffer(buffer, 0x3FFFFFFF, 2) == BufferValidation::Ok);
    assert(ValidRange(0, 1));
    assert(ValidRange(UINT32_MAX, 0));
    assert(ValidRange(UINT32_MAX - 1, 1));
    assert(!ValidRange(UINT32_MAX, 1));
    assert(!ValidRange(UINT32_MAX + 1ull, 0));
    assert(!ValidRange(UINT64_MAX, 1));
    assert(!ValidRange(0, UINT32_MAX + 1ull));
    assert(ValidString("a", 1));
    assert(ValidString("ab", 2));
    assert(!ValidString("ab", 1));
    assert(!ValidString("", 10));
    assert(!ValidString(nullptr, 10));
    const std::string path(0x27F, 'x');
    assert(ValidString(path.c_str(), 0x27F));
    assert(!ValidString((path + 'y').c_str(), 0x27F));
}
