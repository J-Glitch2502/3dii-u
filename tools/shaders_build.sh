#!/bin/bash
# Runs inside the contained environment: builds the shader generator and precompiles the game's shaders into build/shaders.
# Logs of what the game used are in games/tomodachi/shader_logs (vs_log.bin and fs_log.bin from the PC build, shader_misses.bin
# from the console).
RT=/project/third_party/3dsrecomp/runtime
P=/project/port
OBJ=/root/shadergen
mkdir -p "$OBJ" /project/build/shaders /project/games/tomodachi/shader_logs
FLAGS="-std=c++20 -O1 -g -w -I$P/wiiu -I$P/pc -I$RT -I$RT/gpu -include $P/pc/shim.h"
set -e
for s in /project/tools/shadergen_main.cpp $P/wiiu/shadergen.cpp $P/wiiu/shaderkeys.cpp $RT/gpu/shader.cpp; do
    o="$OBJ/$(basename "$s").o"
    if [ ! "$o" -nt "$s" ] || [ "$P/wiiu/shadergen.h" -nt "$o" ] || [ "$P/wiiu/fs_key.inc" -nt "$o" ]; then clang++-18 $FLAGS -c "$s" -o "$o"; fi
done
cat > "$OBJ/stubs.cpp" <<'EOF'
#include <cstdarg>
#include <cstdio>
#include "gpu/pica.h"
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
void rt_fatal(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); exit(1); }
int g_verbose;
namespace gpu { GpuState g; bool g_stats, g_trace; u64 hash_bytes(const u8 *p, size_t n) { return fnv_host(p, n); } }
EOF
sed -i 's/fnv_host(p, n)/0/' "$OBJ/stubs.cpp"
clang++-18 $FLAGS -c "$OBJ/stubs.cpp" -o "$OBJ/stubs.o"
clang++-18 -fuse-ld=lld-18 "$OBJ"/*.o -lpthread -lm -o /root/shadergen/shadergen 2>&1 | grep -E 'undefined symbol|error' | sort -u | head -20 | cut -c1-150
/root/shadergen/shadergen /project/third_party/3dsrecomp/games/000400000008c300/pipelines.bin /project/games/tomodachi/shader_logs /project/build/shaders /project/third_party/cafeglsl/glslcompiler.elf
