// Rebuildable caches and emergency reports on the physical SD, without libc's
// redirected FS hooks. Never use this for the title's virtual content or saves.
#pragma once
#include <coreinit/cache.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace wiiu {
class PhysicalFile {
    FSAClientHandle client_ = (FSAClientHandle)-1;
    FSAFileHandle file_ = (FSAFileHandle)-1;
    alignas(64) FSAShimBuffer shim_{};
    alignas(64) unsigned char bounce_[16384]{};
    int command(FSACommand operation) {
        DCFlushRange(&shim_.request, sizeof shim_.request);
        DCFlushRange(&shim_.response, sizeof shim_.response);
        const int result = IOS_Ioctl(client_, operation, &shim_.request, sizeof shim_.request,
                                     &shim_.response, sizeof shim_.response);
        DCInvalidateRange(&shim_.response, sizeof shim_.response);
        return result;
    }
    int transfer(bool writing, unsigned count) {
        auto* v = shim_.ioctlvVec;
        v[0].vaddr = &shim_.request; v[0].len = sizeof shim_.request;
        v[1].vaddr = bounce_; v[1].len = count;
        v[2].vaddr = &shim_.response; v[2].len = sizeof shim_.response;
        DCFlushRange(&shim_.request, sizeof shim_.request);
        DCFlushRange(&shim_.response, sizeof shim_.response);
        DCFlushRange(v, 3 * sizeof(IOSVec));
        if (writing) DCFlushRange(bounce_, count);
        else DCInvalidateRange(bounce_, count);
        const int result = IOS_Ioctlv(client_, writing ? FSA_COMMAND_WRITE_FILE : FSA_COMMAND_READ_FILE,
                                     writing ? 2 : 1, writing ? 1 : 2, v);
        if (!writing) DCInvalidateRange(bounce_, count);
        DCInvalidateRange(&shim_.response, sizeof shim_.response);
        return result;
    }
public:
    PhysicalFile() = default;
    PhysicalFile(const PhysicalFile&) = delete;
    PhysicalFile& operator=(const PhysicalFile&) = delete;
    ~PhysicalFile() { close(); }
    bool open(const std::string& path, const char* mode) {
        close();
        const std::string root = "/vol/external01/wiiu/3dii-u/";
        if (path.rfind(root, 0) || path.find("/../") != std::string::npos || path.size() > FS_MAX_PATH) return false;
        client_ = FSAAddClient(nullptr);
        if ((int)client_ < 0) return false;
        if (mode[0] != 'r') {
            for (size_t pos = path.find('/', 1); pos != std::string::npos; pos = path.find('/', pos + 1)) {
                if (pos < strlen("/vol/external01/")) continue;
                memset(&shim_, 0, sizeof shim_);
                memcpy(shim_.request.makeDir.path, path.data(), pos);
                shim_.request.makeDir.permission = (FSMode)0x660;
                const int result = command(FSA_COMMAND_MAKE_DIR);
                if (result != FS_ERROR_OK && result != FS_ERROR_ALREADY_EXISTS) { close(); return false; }
            }
        }
        memset(&shim_, 0, sizeof shim_);
        strcpy(shim_.request.openFile.path, path.c_str());
        strncpy(shim_.request.openFile.mode, mode, FS_MODE_LENGTH - 1);
        shim_.request.openFile.unk0x290 = (FSMode)0x660;
        if (command(FSA_COMMAND_OPEN_FILE) != FS_ERROR_OK) { close(); return false; }
        file_ = shim_.response.openFile.handle;
        return true;
    }
    unsigned read(void* output, unsigned size) {
        unsigned done = 0;
        while (done < size && (int)file_ >= 0) {
            const unsigned n = std::min<unsigned>(size - done, sizeof bounce_);
            memset(&shim_, 0, sizeof shim_);
            shim_.request.readFile.buffer = bounce_; shim_.request.readFile.size = 1;
            shim_.request.readFile.count = n; shim_.request.readFile.handle = file_;
            const int got = transfer(false, n);
            if (got <= 0 || (unsigned)got > n) break;
            memcpy((unsigned char*)output + done, bounce_, got); done += got;
            if ((unsigned)got < n) break;
        }
        return done;
    }
    bool write(const void* input, unsigned size) {
        unsigned done = 0;
        while (done < size && (int)file_ >= 0) {
            const unsigned n = std::min<unsigned>(size - done, sizeof bounce_);
            memcpy(bounce_, (const unsigned char*)input + done, n);
            memset(&shim_, 0, sizeof shim_);
            shim_.request.writeFile.buffer = bounce_; shim_.request.writeFile.size = 1;
            shim_.request.writeFile.count = n; shim_.request.writeFile.handle = file_;
            const int wrote = transfer(true, n);
            if (wrote <= 0 || (unsigned)wrote > n) return false;
            done += wrote;
        }
        return done == size;
    }
    bool empty() {
        memset(&shim_, 0, sizeof shim_); shim_.request.statFile.handle = file_;
        return command(FSA_COMMAND_STAT_FILE) == FS_ERROR_OK && shim_.response.statFile.stat.size == 0;
    }
    bool flush() {
        if ((int)file_ < 0) return false;
        memset(&shim_, 0, sizeof shim_); shim_.request.flushFile.handle = file_;
        return command(FSA_COMMAND_FLUSH_FILE) == FS_ERROR_OK;
    }
    void close() {
        if ((int)file_ >= 0) {
            memset(&shim_, 0, sizeof shim_); shim_.request.closeFile.handle = file_;
            command(FSA_COMMAND_CLOSE_FILE); file_ = (FSAFileHandle)-1;
        }
        if ((int)client_ >= 0) { FSADelClient(client_); client_ = (FSAClientHandle)-1; }
    }
};
}
