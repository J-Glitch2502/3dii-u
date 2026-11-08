#!/bin/bash
# Runs inside the contained environment: builds only FFmpeg's AAC decoder, for this PC (/root/aac_pc) and the Wii U (/root/aac_ppc), from an unpacked
# FFmpeg source in the folder given. For the Mario & Luigi remakes' classic soundtracks. LGPL: no --enable-gpl, and these builds stay private.
SRC=$1
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
W=$DEVKITPRO/wut
[ -f /root/rpx.specs ] || bash /project/tools/rpx_link_setup.sh >/dev/null
COMMON="--disable-all --enable-avcodec --enable-decoder=aac --disable-autodetect --disable-pthreads --disable-network --disable-debug --disable-doc --disable-programs --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-stripping --disable-runtime-cpudetect --disable-asm"
build() {
    B=/root/aac_$1
    shift
    rm -rf "$B"
    mkdir -p "$B/obj"
    (cd "$B/obj" && "$SRC/configure" --prefix="$B" $COMMON "$@" > configure.log 2>&1) || { echo "configure failed:"; tail -5 "$B/obj/configure.log"; tail -25 "$B/obj/ffbuild/config.log" | cut -c1-170; return 1; }
    (cd "$B/obj" && nice -n 10 make -j4 install-libs install-headers > make.log 2>&1) || { echo "make failed:"; grep -E 'error|Error' "$B/obj/make.log" | head -12 | cut -c1-170; return 1; }
    rm -rf "$B/obj"
    ls -la "$B/lib"/*.a | cut -c24-
}
build pc --cc=clang-18 --extra-cflags=-O2 || exit 1
build ppc --enable-cross-compile --arch=ppc --cpu=750 --target-os=none --cross-prefix=powerpc-eabi- --disable-altivec --disable-vsx --disable-power8 \
    --extra-cflags="-mcpu=750 -meabi -mhard-float -O2 -ffunction-sections -fdata-sections -D__WIIU__ -D__WUT__ -I$W/include" \
    --extra-ldflags="-specs=/root/rpx.specs -L$W/lib -Wl,--no-warn-rwx-segments" --extra-libs="-Wl,--start-group -lwut -lstdc++ -lm -lc -lsysbase -lgcc -Wl,--end-group" || exit 1
echo aac build done
