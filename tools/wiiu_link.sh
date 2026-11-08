#!/bin/bash
# Runs inside the contained environment: links the runtime and the game's Wii U objects into an RPX.
# wiiu_link.sh [output rpx]. The runtime goes first so that its calls into the C library stay near.
OUT=${1:-/project/build/tomodachi.rpx}
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
W=$DEVKITPRO/wut
[ -f /root/rpx.specs ] || bash /project/tools/rpx_link_setup.sh >/dev/null
mkdir -p "$(dirname "$OUT")" /root/wiiu_link
powerpc-eabi-g++ -mcpu=750 -meabi -mhard-float -specs=/root/rpx.specs -Wl,--relax -Wl,--no-warn-rwx-segments $LINK_EXTRA \
    /root/wiiu_rt/*.o -Wl,--start-group -L$W/lib -lwut -lstdc++ -lm -lc -lsysbase -lgcc -Wl,--end-group /root/ppc_abi/*.o \
    -o /root/wiiu_link/game.elf 2>&1 | grep -E 'undefined reference|error|multiple definition|truncated' | sed 's#^.*ld: ##' | sort -u | head -${LINKERRS:-40} | cut -c1-150
[ -f /root/wiiu_link/game.elf ] || exit 1
elf2rpl /root/wiiu_link/game.elf "$OUT" 2>&1 | tail -2
powerpc-eabi-size /root/wiiu_link/game.elf | tail -1 | cut -c1-70
ls -la "$OUT" | cut -c24-
