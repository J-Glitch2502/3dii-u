// Windows bodies for what compat.h, sys/mman.h and dlfcn.h declare, and what has to be set up before anything runs.
#include <windows.h>
#include <timeapi.h>
#include <stdlib.h>

#include <cstdio>
#include <cstring>

#include "compat.h"
#include "dlfcn.h"
#include "sys/mman.h"

// Files are binary unless said otherwise, as on Linux, and sleeps are good to a millisecond rather than 15: frames, input and audio are all paced by them.
static const int startup = [] {
    _fmode = _O_BINARY;
    timeBeginPeriod(1);
    return 0;
}();

static char *guest_base;
static size_t guest_size;
static const size_t WIIU_BLOCK = 0x20010000;  // the most the Wii U build has
static bool guard;

// Like Linux's MAP_NORESERVE: the 4 GiB is only reserved, and the part games use is committed up front, since the kernel can't read a file into memory that isn't committed.
// A touch above that, which the Wii U build couldn't serve anyway, gets its page here. RECOMP_GUARD=1 stops there and says where instead, which is what tools/pc_guard.sh is for.
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    char *at = (char *)r->ExceptionInformation[1];
    if (at < guest_base || at >= guest_base + guest_size) return EXCEPTION_CONTINUE_SEARCH;
    if (guard) {
        fprintf(stderr, "guest access the Wii U build can't serve: %08llx, from host code at %p\n", (unsigned long long)(at - guest_base), r->ExceptionAddress);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return VirtualAlloc(at, 1, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void *mmap(void *, size_t size, int, int, int, long long) {
    char *p = (char *)VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
    if (!p || !VirtualAlloc(p, WIIU_BLOCK, MEM_COMMIT, PAGE_READWRITE)) return MAP_FAILED;
    guest_base = p;
    guest_size = size;
    guard = getenv("RECOMP_GUARD") != nullptr;
    AddVectoredExceptionHandler(1, commit_on_touch);
    return p;
}

ssize_t pread(int fd, void *buf, size_t n, long long at) {
    OVERLAPPED o{};
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)(at >> 32);
    DWORD done = 0;
    if (!ReadFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)n, &done, &o)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return done;
}

ssize_t pwrite(int fd, const void *buf, size_t n, long long at) {
    OVERLAPPED o{};
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)(at >> 32);
    DWORD done = 0;
    return WriteFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)n, &done, &o) ? (ssize_t)done : -1;
}

char *realpath(const char *path, char *out) {
    return _fullpath(out, path, 4096);
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}

void *dlopen(const char *path, int) { return LoadLibraryA(path); }
void *dlsym(void *handle, const char *name) { return (void *)GetProcAddress((HMODULE)handle, name); }
const char *dlerror() {
    static char text[32];
    snprintf(text, sizeof text, "Windows error %lu", GetLastError());
    return text;
}
