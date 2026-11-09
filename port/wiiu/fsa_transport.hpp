#pragma once

#include <coreinit/filesystem_fsa.h>

namespace wiiu::fsa {
FSError Ask(FSAClientHandle client, FSACommand command, FSAShimBuffer &shim);
FSError Transfer(FSAClientHandle client, FSACommand command, void *data, uint32_t bytes, FSAShimBuffer &shim);
FSError ReadAt(FSAClientHandle client, FSAFileHandle file, FSAShimBuffer &shim, void *data, uint32_t bytes, uint32_t position);
void PutPath(char *destination, const char *path);
} // namespace wiiu::fsa
