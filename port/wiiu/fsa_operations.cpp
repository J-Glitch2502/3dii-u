#include <cstring>

#include "fsa_operations.hpp"
#include "fsa_transport.hpp"

namespace wiiu::fsa {
FSError Open(FSAClientHandle c, const char *path, const char *mode, FSMode create, FSOpenFileFlags flags, uint32_t prealloc,
             FSAFileHandle *out) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    if (!out) return FS_ERROR_INVALID_PARAM;
    *out = (FSAFileHandle)-1;
    const FSError pathResult = PutPath(s.request.openFile.path, path);
    if (pathResult != FS_ERROR_OK) return pathResult;
    if (!ValidString(mode, FS_MODE_LENGTH - 1)) return FS_ERROR_INVALID_PARAM;
    strcpy(s.request.openFile.mode, mode);
    s.request.openFile.unk0x290 = create;
    s.request.openFile.unk0x294 = flags;
    s.request.openFile.unk0x298 = prealloc;
    s.response.openFile.handle = (FSAFileHandle)-1;
    const FSError r = wiiu::fsa::Ask(c, FSA_COMMAND_OPEN_FILE, s);
    if (r == FS_ERROR_OK && s.response.openFile.handle == (FSAFileHandle)-1) return FS_ERROR_INVALID_FILEHANDLE;
    *out = r == FS_ERROR_OK ? s.response.openFile.handle : (FSAFileHandle)-1;
    return r;
}

FSError Read(FSAClientHandle c, void *to, uint32_t size, uint32_t count, FSAFileHandle file, FSAReadFlag flags) {
    const FSError valid = CheckBuffer(to, size, count);
    if (valid != FS_ERROR_OK) return valid;
    if (!size || !count) return FS_ERROR_OK;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.readFile.buffer = (uint8_t *)to;
    s.request.readFile.size = size;
    s.request.readFile.count = count;
    s.request.readFile.handle = file;
    s.request.readFile.readFlags = (FSAReadFlag)(flags & ~FSA_READ_FLAG_READ_WITH_POS);
    return wiiu::fsa::Transfer(c, FSA_COMMAND_READ_FILE, to, size * count, s);
}

FSError Write(FSAClientHandle c, const void *from, uint32_t size, uint32_t count, FSAFileHandle file, FSAWriteFlag flags) {
    const FSError valid = CheckBuffer(from, size, count);
    if (valid != FS_ERROR_OK) return valid;
    if (!size || !count) return FS_ERROR_OK;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.writeFile.buffer = (const uint8_t *)from;
    s.request.writeFile.size = size;
    s.request.writeFile.count = count;
    s.request.writeFile.handle = file;
    s.request.writeFile.writeFlags = (FSAWriteFlag)(flags & ~FSA_WRITE_FLAG_READ_WITH_POS);
    return wiiu::fsa::Transfer(c, FSA_COMMAND_WRITE_FILE, (void *)from, size * count, s);
}

FSError Close(FSAClientHandle c, FSAFileHandle file) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.closeFile.handle = file;
    return wiiu::fsa::Ask(c, FSA_COMMAND_CLOSE_FILE, s);
}

FSError Seek(FSAClientHandle c, FSAFileHandle file, FSAFilePosition pos) {
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.setPosFile.handle = file;
    s.request.setPosFile.pos = pos;
    return wiiu::fsa::Ask(c, FSA_COMMAND_SET_POS_FILE, s);
}

FSError StatFile(FSAClientHandle c, FSAFileHandle file, FSAStat *out) {
    if (!out) return FS_ERROR_INVALID_BUFFER;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    s.request.statFile.handle = file;
    const FSError r = wiiu::fsa::Ask(c, FSA_COMMAND_STAT_FILE, s);
    if (r == FS_ERROR_OK) memcpy((void *)out, (const void *)&s.response.statFile.stat, sizeof *out);
    return r;
}

FSError Stat(FSAClientHandle c, const char *path, FSAStat *out) {
    if (!out) return FS_ERROR_INVALID_BUFFER;
    alignas(64) FSAShimBuffer s;
    memset(&s, 0, sizeof s);
    const FSError pathResult = PutPath(s.request.getInfoByQuery.path, path);
    if (pathResult != FS_ERROR_OK) return pathResult;
    s.request.getInfoByQuery.type = FSA_QUERY_INFO_STAT;
    const FSError r = wiiu::fsa::Ask(c, FSA_COMMAND_GET_INFO_BY_QUERY, s);
    if (r == FS_ERROR_OK) memcpy((void *)out, (const void *)&s.response.getInfoByQuery.stat, sizeof *out);
    return r;
}

} // namespace wiiu::fsa
