#include <coreinit/cache.h>
#include <coreinit/ios.h>

#include <cstring>

#include "fsa_transport.hpp"

namespace wiiu::fsa {
FSError Ask(FSAClientHandle c, FSACommand command, FSAShimBuffer &s) {
    DCFlushRange(&s.request, sizeof s.request);
    DCFlushRange(&s.response, sizeof s.response);
    const int r = IOS_Ioctl(c, command, &s.request, sizeof s.request, &s.response, sizeof s.response);
    DCInvalidateRange(&s.response, sizeof s.response);
    return (FSError)r;
}

FSError PutPath(char *to, const char *path) {
    if (!ValidString(path, FS_MAX_PATH)) return FS_ERROR_INVALID_PATH;
    strcpy(to, path);
    return FS_ERROR_OK;
}

FSError CheckBuffer(const void *data, uint32_t size, uint32_t count) {
    switch (ValidateBuffer(data, size, count)) {
        case BufferValidation::InvalidBuffer:
            return FS_ERROR_INVALID_BUFFER;
        case BufferValidation::InvalidAlignment:
            return FS_ERROR_INVALID_ALIGNMENT;
        case BufferValidation::OutOfRange:
            return FS_ERROR_OUT_OF_RANGE;
        default:
            return FS_ERROR_OK;
    }
}

FSError Transfer(FSAClientHandle c, FSACommand command, void *data, uint32_t bytes, FSAShimBuffer &s) {
    const FSError valid = CheckBuffer(data, bytes, 1);
    if (valid != FS_ERROR_OK) return valid;
    if (bytes == 0) return FS_ERROR_OK;
    IOSVec *const v = s.ioctlvVec;
    v[0].vaddr = &s.request;
    v[0].len = sizeof s.request;
    v[1].vaddr = data;
    v[1].len = bytes;
    v[2].vaddr = &s.response;
    v[2].len = sizeof s.response;
    DCFlushRange(&s.request, sizeof s.request);
    DCFlushRange(&s.response, sizeof s.response);
    DCFlushRange(v, 3 * sizeof(IOSVec));
    DCFlushRange(data, bytes);
    const bool reading = command == FSA_COMMAND_READ_FILE;
    const int r = IOS_Ioctlv(c, command, reading ? 1 : 2, reading ? 2 : 1, v);
    if (reading) DCFlushRange(data, bytes);
    return (FSError)r;
}

FSError ReadAt(FSAClientHandle client, FSAFileHandle file, FSAShimBuffer &shim, void *data, uint32_t bytes, uint32_t position) {
    if (!ValidRange(position, bytes)) return FS_ERROR_OUT_OF_RANGE;
    const FSError valid = CheckBuffer(data, bytes, 1);
    if (valid != FS_ERROR_OK) return valid;
    if (!bytes) return FS_ERROR_OK;
    memset(&shim.request, 0, sizeof shim.request);
    shim.request.readFile.buffer = (uint8_t *)data;
    shim.request.readFile.size = 1;
    shim.request.readFile.count = bytes;
    shim.request.readFile.pos = position;
    shim.request.readFile.handle = file;
    shim.request.readFile.readFlags = FSA_READ_FLAG_READ_WITH_POS;
    IOSVec *v = shim.ioctlvVec;
    v[0].vaddr = &shim.request;
    v[0].len = sizeof(FSARequest);
    v[1].vaddr = data;
    v[1].len = bytes;
    v[2].vaddr = &shim.response;
    v[2].len = sizeof(FSAResponse);
    DCFlushRange(&shim.request, sizeof(FSARequest));
    DCFlushRange(v, 3 * sizeof(IOSVec));
    DCInvalidateRange(data, bytes);
    DCInvalidateRange(&shim.response, sizeof(FSAResponse));
    const int got = IOS_Ioctlv(client, FSA_COMMAND_READ_FILE, 1, 2, v);
    DCInvalidateRange(data, bytes);
    return (FSError)got;
}

} // namespace wiiu::fsa
