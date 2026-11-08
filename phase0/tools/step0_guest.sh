#!/bin/bash
# Runs inside the contained environment: ARM workload -> code.bin + info.json. Uses only the installed ARM compiler.
set -e
cd /project/phase0
B=build/guest
mkdir -p "$B"
arm-none-eabi-gcc -O2 -marm -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard -ffreestanding -fno-builtin \
    -ffp-contract=off -fno-math-errno -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -Wall -nostartfiles -Wl,--gc-sections -T guest/link.ld guest/workload.c -lgcc -o "$B/workload.elf"
arm-none-eabi-size "$B/workload.elf"
arm-none-eabi-objcopy -O binary -R .bss "$B/workload.elf" "$B/code.bin"
python3 tools/mkguest.py "$B/workload.elf" "$B"
