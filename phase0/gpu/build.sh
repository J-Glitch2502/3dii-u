#!/bin/bash
# Runs inside the contained environment: precompiles the shaders with CafeGLSL, then builds the GPU test app.
set -e
cd /project/phase0
mkdir -p build/gpu
../third_party/cafeglsl/glslcompiler.elf -vs gpu/pica.vert -ps gpu/pica.frag -o build/gpu/pica.gsh -v > build/gpu/shader.log 2>&1 || { cat build/gpu/shader.log | tail -30; exit 1; }
ls -l build/gpu/pica.gsh
echo "ALU slots in the pixel shader, as a rough cost:"; grep -c -E '^\s*[0-9]+ [xyzwt]:|^ +[0-9]+ +[xyzwt]:' build/gpu/shader.log || true
WUT=$DEVKITPRO/wut
powerpc-eabi-gcc -DESPRESSO -D__WIIU__ -D__WUT__ -mcpu=750 -meabi -mhard-float -O2 -Wall -Wno-unused-function -isystem $WUT/include -include stddef.h -c gpu/gpu_test.c -o build/gpu/gpu_test.o
powerpc-eabi-gcc -mcpu=750 -meabi -mhard-float -specs=$WUT/share/wut.specs -L$WUT/lib build/gpu/gpu_test.o -lwut -lm -o build/gpu/gpu_test.elf
elf2rpl build/gpu/gpu_test.elf build/gpu/gpu_test.rpx
ls -l build/gpu/gpu_test.rpx
