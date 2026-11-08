#!/bin/bash
# Runs inside the contained environment: rebuilds from the workload, re-checks everything, predicts speed, builds the Wii U app.
set -e
cd /project/phase0
bash tools/step0_guest.sh >/dev/null
make check 2>&1 | grep -E 'VERIFY|FATAL|got .*want|rror|lifted ' || true
python3 tools/ratio.py
make wiiu 2>&1 | grep -v -E '^(mkdir|powerpc-eabi-gcc|elf2rpl|    build|make: warning|make: Warning|python3|entities|math|bytes|sort|calls|mix|unaligned)'
powerpc-eabi-size build/wiiu/abi.o build/wiiu/safe.o build/wiiu/workload.o
