// The few POSIX calls the runtime uses that MinGW doesn't have, for the headless PC build on Windows. win_compat.cpp has the bodies, so windows.h stays out of the runtime.
#pragma once
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
#include <time.h>
#include <cstddef>

ssize_t pread(int fd, void *buf, size_t n, long long at);
ssize_t pwrite(int fd, const void *buf, size_t n, long long at);
char *realpath(const char *path, char *out);
int setenv(const char *name, const char *value, int overwrite);
inline int mkdir(const char *path, int) { return _mkdir(path); }
inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? nullptr : out; }
