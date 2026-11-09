#pragma once

#include <coreinit/filesystem_fsa.h>

#include "fsa_validation.hpp"

namespace wiiu::fsa {
FSError Ask(FSAClientHandle client, FSACommand command, FSAShimBuffer &shim);
FSError Transfer(FSAClientHandle client, FSACommand command, void *data, uint32_t bytes, FSAShimBuffer &shim);
FSError ReadAt(FSAClientHandle client, FSAFileHandle file, FSAShimBuffer &shim, void *data, uint32_t bytes, uint32_t position);
FSError PutPath(char *destination, const char *path);
FSError CheckBuffer(const void *data, uint32_t size, uint32_t count);
} // namespace wiiu::fsa
