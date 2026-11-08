#!/bin/bash
# Runs inside the contained environment: links the stand-in runtime with the game's Wii U objects, to test that it loads.
# link_code_test.sh <object dir> <output dir>
OBJS=$1; OUT=$2
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
W=$DEVKITPRO/wut
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
mkdir -p "$OUT"
FLAGS="-mcpu=750 -meabi -mhard-float -O2 -D__WIIU__ -D__WUT__ -DESPRESSO -I/project/port -I/project/third_party/3dsrecomp/runtime -isystem $W/include"
powerpc-eabi-gcc $FLAGS -c /project/port/link_probe.c -o "$OUT/link_probe.o" || exit 1
powerpc-eabi-gcc -specs=$W/share/wut.specs -Wl,--no-gc-sections "$OUT/link_probe.o" "$OBJS"/*.o -L$W/lib -lwut -lm -o "$OUT/bench.elf" 2>&1 | tail -6 | cut -c1-160
elf2rpl "$OUT/bench.elf" "$OUT/bench.rpx" 2>&1 | tail -2
ls -la "$OUT/bench.rpx" | cut -c24-
powerpc-eabi-size "$OUT/bench.elf" | cut -c1-70
rm -f "$OUT/bench.elf" "$OUT/link_probe.o"
