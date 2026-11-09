#pragma once

#include <coreinit/filesystem_fsa.h>

namespace wiiu::fsa {
FSError Open(FSAClientHandle client, const char *path, const char *mode, FSMode create, FSOpenFileFlags flags, uint32_t prealloc,
             FSAFileHandle *out);
FSError Read(FSAClientHandle client, void *to, uint32_t size, uint32_t count, FSAFileHandle file, FSAReadFlag flags);
FSError Write(FSAClientHandle client, const void *from, uint32_t size, uint32_t count, FSAFileHandle file, FSAWriteFlag flags);
FSError Close(FSAClientHandle client, FSAFileHandle file);
FSError Seek(FSAClientHandle client, FSAFileHandle file, FSAFilePosition position);
FSError StatFile(FSAClientHandle client, FSAFileHandle file, FSAStat *out);
FSError Stat(FSAClientHandle client, const char *path, FSAStat *out);
} // namespace wiiu::fsa
