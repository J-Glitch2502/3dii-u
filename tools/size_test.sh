#!/bin/bash
# Runs inside the contained environment: builds code-test RPXes with empty filler in .text, to find what stops large code
# from loading. size_test.sh <name> <filler MB> <first|last> [extra link flags]. Output: phase0/build/sizetest/<name>.rpx
NAME=$1; MB=$2; WHERE=$3; shift 3
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
W=$DEVKITPRO/wut
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
T=/root/sizetest; OUT=/project/phase0/build/sizetest
mkdir -p $T $OUT
FLAGS="-mcpu=750 -meabi -mhard-float -O2 -D__WIIU__ -D__WUT__ -DESPRESSO -I/project/port -I/project/third_party/3dsrecomp/runtime -isystem $W/include"
powerpc-eabi-gcc $FLAGS -c /project/port/link_probe.c -o $T/probe.o || exit 1
cat > $T/filler.c <<EOF
#include <cpu.h>
const GuestEntry guest_entries[1];
const u32 guest_entry_count = 1;
__asm__(".text\n.space $((MB * 1024 * 1024))\n");
EOF
powerpc-eabi-gcc $FLAGS -c $T/filler.c -o $T/filler.o || exit 1
LIBS="-Wl,--start-group -L$W/lib -lwut -lm -lstdc++ -lc -lsysbase -lgcc -Wl,--end-group"
if [ "$WHERE" = first ]; then
    powerpc-eabi-gcc -specs=/root/rpx.specs -Wl,--no-gc-sections "$@" $T/probe.o $T/filler.o $LIBS -o $T/$NAME.elf 2>&1 | tail -4 | cut -c1-150
else
    powerpc-eabi-gcc -specs=/root/rpx.specs -Wl,--no-gc-sections "$@" $T/probe.o $LIBS $T/filler.o -o $T/$NAME.elf 2>&1 | tail -4 | cut -c1-150
fi
elf2rpl $T/$NAME.elf $OUT/$NAME.rpx 2>&1 | tail -2
powerpc-eabi-size $T/$NAME.elf | tail -1 | cut -c1-60
ls -la $OUT/$NAME.rpx | cut -c24-
