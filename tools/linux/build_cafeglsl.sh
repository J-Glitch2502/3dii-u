#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
source_dir="${THREEDII_CAFEGLSL_SOURCE:-$root/third_party/cafeglsl-source}"
output="${THREEDII_GLSLC:-$root/third_party/cafeglsl/glslcompiler.elf}"
for tool in git meson ninja flex bison; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing %s. Install git, meson, ninja-build, flex, bison, a C compiler, and the Mesa build dependencies.\n' "$tool" >&2
        exit 1
    fi
done
if [ ! -f "$source_dir/meson.build" ]; then
    mkdir -p "$(dirname "$source_dir")"
    git clone --depth 1 --branch v0.2.0 https://github.com/Exzap/CafeGLSL.git "$source_dir"
fi
if [ ! -f "$source_dir/build-linux/build.ninja" ]; then
    meson setup "$source_dir/build-linux" "$source_dir" \
        -Db_sanitize=none -Dtools=glsl -Dvulkan-drivers= -Dgallium-drivers=r600 \
        -Db_lundef=false -Dglx=disabled -Degl=disabled -Dplatforms= \
        -Dllvm=disabled -Dzstd=disabled --buildtype=release
fi
ninja -C "$source_dir/build-linux" -j "${JOBS:-4}" cafecompiler/glslcompiler.elf
mkdir -p "$(dirname "$output")"
cp "$source_dir/build-linux/cafecompiler/glslcompiler.elf" "$output"
chmod +x "$output"
printf 'CafeGLSL v0.2.0 ready: %s\n' "$output"
