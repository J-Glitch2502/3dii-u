#include <coreinit/filesystem_fsa.h>

#include <cstring>

#include "fsa_operations.hpp"
#include "fsa_route.hpp"
#include "fsa_transport.hpp"

extern "C" {
FSError __real_FSAOpenFileEx(FSAClientHandle, const char *, const char *, FSMode, FSOpenFileFlags, uint32_t, FSAFileHandle *);
FSError __real_FSACloseFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSAReadFile(FSAClientHandle, void *, uint32_t, uint32_t, FSAFileHandle, FSAReadFlag);
FSError __real_FSAWriteFile(FSAClientHandle, const void *, uint32_t, uint32_t, FSAFileHandle, FSAWriteFlag);
FSError __real_FSASetPosFile(FSAClientHandle, FSAFileHandle, FSAFilePosition);
FSError __real_FSAGetStatFile(FSAClientHandle, FSAFileHandle, FSAStat *);
FSError __real_FSAGetStat(FSAClientHandle, const char *, FSAStat *);
FSError __real_FSAFlushFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSATruncateFile(FSAClientHandle, FSAFileHandle);
FSError __real_FSARemove(FSAClientHandle, const char *);
FSError __real_FSAMakeDir(FSAClientHandle, const char *, FSMode);
FSError __real_FSARename(FSAClientHandle, const char *, const char *);
FSError __real_FSAOpenDir(FSAClientHandle, const char *, FSADirectoryHandle *);
FSError __real_FSAReadDir(FSAClientHandle, FSADirectoryHandle, FSADirectoryEntry *);
FSError __real_FSARewindDir(FSAClientHandle, FSADirectoryHandle);
FSError __real_FSACloseDir(FSAClientHandle, FSADirectoryHandle);
}

extern "C" {
FSError __wrap_FSAOpenFileEx(FSAClientHandle c, const char *path, const char *mode, FSMode create, FSOpenFileFlags flags, uint32_t prealloc, FSAFileHandle *out) {
    return wiiu::fsa::UseDirect(c) ? wiiu::fsa::Open(c, path, mode, create, flags, prealloc, out) : __real_FSAOpenFileEx(c, path, mode, create, flags, prealloc, out);
}
FSError __wrap_FSACloseFile(FSAClientHandle c, FSAFileHandle file) { return wiiu::fsa::UseDirect(c) ? wiiu::fsa::Close(c, file) : __real_FSACloseFile(c, file); }
FSError __wrap_FSAReadFile(FSAClientHandle c, void *to, uint32_t size, uint32_t count, FSAFileHandle file, FSAReadFlag flags) {
    return wiiu::fsa::UseDirect(c) ? wiiu::fsa::Read(c, to, size, count, file, flags) : __real_FSAReadFile(c, to, size, count, file, flags);
}
FSError __wrap_FSAWriteFile(FSAClientHandle c, const void *from, uint32_t size, uint32_t count, FSAFileHandle file, FSAWriteFlag flags) {
    return wiiu::fsa::UseDirect(c) ? wiiu::fsa::Write(c, from, size, count, file, flags) : __real_FSAWriteFile(c, from, size, count, file, flags);
}
FSError __wrap_FSASetPosFile(FSAClientHandle c, FSAFileHandle file, FSAFilePosition pos) { return wiiu::fsa::UseDirect(c) ? wiiu::fsa::Seek(c, file, pos) : __real_FSASetPosFile(c, file, pos); }
FSError __wrap_FSAGetStatFile(FSAClientHandle c, FSAFileHandle file, FSAStat *out) { return wiiu::fsa::UseDirect(c) ? wiiu::fsa::StatFile(c, file, out) : __real_FSAGetStatFile(c, file, out); }
FSError __wrap_FSAGetStat(FSAClientHandle c, const char *path, FSAStat *out) { return wiiu::fsa::UseDirect(c) ? wiiu::fsa::Stat(c, path, out) : __real_FSAGetStat(c, path, out); }
FSError __wrap_FSAFlushFile(FSAClientHandle c, FSAFileHandle file) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSAFlushFile(c, file);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.flushFile.handle = file;
    return wiiu::fsa::Ask(c, FSA_COMMAND_FLUSH_FILE, s);
}
FSError __wrap_FSATruncateFile(FSAClientHandle c, FSAFileHandle file) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSATruncateFile(c, file);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.truncateFile.handle = file;
    return wiiu::fsa::Ask(c, FSA_COMMAND_TRUNCATE_FILE, s);
}
FSError __wrap_FSARemove(FSAClientHandle c, const char *path) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSARemove(c, path);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    wiiu::fsa::PutPath(s.request.remove.path, path);
    return wiiu::fsa::Ask(c, FSA_COMMAND_REMOVE, s);
}
FSError __wrap_FSAMakeDir(FSAClientHandle c, const char *path, FSMode mode) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSAMakeDir(c, path, mode);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    wiiu::fsa::PutPath(s.request.makeDir.path, path);
    s.request.makeDir.permission = mode;
    return wiiu::fsa::Ask(c, FSA_COMMAND_MAKE_DIR, s);
}
FSError __wrap_FSARename(FSAClientHandle c, const char *from, const char *to) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSARename(c, from, to);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    wiiu::fsa::PutPath(s.request.rename.oldPath, from);
    wiiu::fsa::PutPath(s.request.rename.newPath, to);
    return wiiu::fsa::Ask(c, FSA_COMMAND_RENAME, s);
}
FSError __wrap_FSAOpenDir(FSAClientHandle c, const char *path, FSADirectoryHandle *out) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSAOpenDir(c, path, out);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    wiiu::fsa::PutPath(s.request.openDir.path, path);
    s.response.openDir.handle = (FSADirectoryHandle)-1;
    const FSError r = wiiu::fsa::Ask(c, FSA_COMMAND_OPEN_DIR, s);
    *out = r == FS_ERROR_OK ? s.response.openDir.handle : (FSADirectoryHandle)-1;
    return r;
}
FSError __wrap_FSAReadDir(FSAClientHandle c, FSADirectoryHandle dir, FSADirectoryEntry *out) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSAReadDir(c, dir, out);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.readDir.handle = dir;
    const FSError r = wiiu::fsa::Ask(c, FSA_COMMAND_READ_DIR, s);
    if (r == FS_ERROR_OK) memcpy((void *)out, (const void *)&s.response.readDir.entry, sizeof *out);
    return r;
}
FSError __wrap_FSARewindDir(FSAClientHandle c, FSADirectoryHandle dir) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSARewindDir(c, dir);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.rewindDir.handle = dir;
    return wiiu::fsa::Ask(c, FSA_COMMAND_REWIND_DIR, s);
}
FSError __wrap_FSACloseDir(FSAClientHandle c, FSADirectoryHandle dir) {
    if (!wiiu::fsa::UseDirect(c)) return __real_FSACloseDir(c, dir);
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.closeDir.handle = dir;
    return wiiu::fsa::Ask(c, FSA_COMMAND_CLOSE_DIR, s);
}
}
