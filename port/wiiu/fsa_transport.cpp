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

void PutPath(char *to, const char *path) {
    strncpy(to, path, FS_MAX_PATH);
    to[FS_MAX_PATH] = 0;
}

FSError Transfer(FSAClientHandle c, FSACommand command, void *data, uint32_t bytes, FSAShimBuffer &s) {
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

} // namespace wiiu::fsa
