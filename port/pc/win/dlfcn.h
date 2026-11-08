// dlopen for the code compiled on the spot during a PC trial. See win_compat.cpp.
#pragma once
#define RTLD_NOW 2
struct Dl_info { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; };
void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
const char *dlerror();
inline int dladdr(const void *, Dl_info *) { return 0; }  // only for host backtraces, which this build doesn't have
