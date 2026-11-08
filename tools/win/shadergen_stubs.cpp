// What the shader generator's sources need from the rest of the runtime, which it doesn't link. Same as the stubs tools/shaders_build.sh writes.
#include <cstdarg>
#include <cstdio>
#include "gpu/pica.h"
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
void rt_fatal(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); exit(1); }
int g_verbose;
namespace gpu { GpuState g; bool g_stats, g_trace; u64 hash_bytes(const u8 *p, size_t n) { return 0; } }
