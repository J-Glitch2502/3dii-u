#!/bin/bash
# Runs inside the contained environment: builds one full-sync game (no --abi) for the Wii U in its own folders, without touching
# Tomodachi's build or the shared scripts. At most 4 low-priority compile jobs.
# layton_wiiu.sh <name under games/>. Needs a PC trial first (pc_try_game.sh), for the generated code and the shader logs.
# Output: build_layton/<name>/<name>.rpx and build_layton/<name>/shaders. Then run tools/layton_title.py with Windows Python.
N=$1
G=/project/games/$N
OUT=/project/build_layton/$N
RTOBJ=/root/wiiu_rt_layton
OBJ=/root/ppc_$N
LINK=/root/wiiu_link_layton
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
W=$DEVKITPRO/wut
mkdir -p "$OUT/shaders" "$LINK" "$G/shader_logs"

sed -e "s#^OBJ=/root/wiiu_rt\$#OBJ=$RTOBJ#" -e 's/xargs -P 8/xargs -P 4/' /project/tools/wiiu_runtime.sh > /root/wiiu_runtime_layton.sh
grep -q "OBJ=$RTOBJ" /root/wiiu_runtime_layton.sh || { echo "runtime script changed: no object folder line"; exit 1; }
sed -i "s#^FLAGS=\"#FLAGS=\"$EXTRA_FLAGS #" /root/wiiu_runtime_layton.sh
nice -n 10 bash /root/wiiu_runtime_layton.sh 2>&1 | tail -6 | cut -c1-170
ls "$RTOBJ" | grep -q 'services_y2r.cpp.o$' || { echo "runtime did not compile"; exit 1; }

sed 's/-P 8/-P 4/' /project/tools/compile_game_ppc.sh > /root/compile_ppc_layton.sh
# -O1: at -O2 gcc reuses a compare across a jump (cmpwi; bne L ... L: blt). That's valid and the console runs it, but Cemu's recompiler
# drops the "less than" bit there, so binary searches go the wrong way and the game breaks with no error.
nice -n 10 bash /root/compile_ppc_layton.sh "$G/gen_safe" "$OBJ" -O1 2>&1 | grep -E 'FAILED|TOTALS' | cut -c1-120
for c in "$G/gen_safe"/*.c; do
    [ -f "$OBJ/$(basename "$c" .c).o" ] || { echo "not compiled: $c"; exit 1; }
done

rm -f "$LINK/$N.elf"
powerpc-eabi-g++ -mcpu=750 -meabi -mhard-float -specs=/root/rpx.specs -Wl,--relax -Wl,--no-warn-rwx-segments \
    "$RTOBJ"/*.o -Wl,--start-group -L$W/lib -lwut -lstdc++ -lm -lc -lsysbase -lgcc -Wl,--end-group "$OBJ"/*.o \
    -o "$LINK/$N.elf" 2>&1 | grep -E 'undefined reference|error|multiple definition|truncated' | sed 's#^.*ld: ##' | sort -u | head -30 | cut -c1-150
[ -f "$LINK/$N.elf" ] || exit 1
powerpc-eabi-nm -n "$LINK/$N.elf" > "$OUT/game.nm"
elf2rpl "$LINK/$N.elf" "$OUT/$N.rpx" 2>&1 | tail -2
powerpc-eabi-size "$LINK/$N.elf" | tail -1 | cut -c1-70
rm -f "$LINK/$N.elf"
ls -la "$OUT/$N.rpx" | cut -c24-

# the generator, in its own folder, since Tomodachi's build uses /root/shadergen
RT=/project/third_party/3dsrecomp/runtime
SG=/root/shadergen_layton
mkdir -p "$SG"
SGFLAGS="-std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/gpu -include /project/port/pc/shim.h"
for s in /project/tools/shadergen_main.cpp /project/port/wiiu/shadergen.cpp /project/port/wiiu/shaderkeys.cpp $RT/gpu/shader.cpp; do
    o="$SG/$(basename "$s").o"
    if [ ! "$o" -nt "$s" ] || [ /project/port/wiiu/shadergen.h -nt "$o" ] || [ /project/port/wiiu/fs_key.inc -nt "$o" ]; then nice -n 10 clang++-18 $SGFLAGS -c "$s" -o "$o" || exit 1; fi
done
sed -n "/^cat > \"\$OBJ\/stubs.cpp\"/,/^EOF/p" /project/tools/shaders_build.sh | sed -e '1d' -e '$d' -e 's/fnv_host(p, n)/0/' > "$SG/stubs.cpp"
nice -n 10 clang++-18 $SGFLAGS -c "$SG/stubs.cpp" -o "$SG/stubs.o" || exit 1
clang++-18 -fuse-ld=lld-18 "$SG"/*.o -lpthread -lm -o "$SG/shadergen" 2>&1 | grep -E 'undefined symbol|error' | sort -u | head -20 | cut -c1-150

cp /root/run_$N/vs_log.bin /root/run_$N/fs_log.bin "$G/shader_logs/"
python3 /project/tools/dvlb_to_log.py "$G" | tail -2
# the same engine, so what one game logged may save the other the slow fallback in scenes no run reached
for o in /project/games/layton_*; do
    [ "$o" = "$G" ] || [ ! -f "$o/shader_logs/fs_log.bin" ] || cp "$o/shader_logs/fs_log.bin" "$G/shader_logs/other_$(basename "$o").bin"
done
"$SG/shadergen" /dev/null "$G/shader_logs" "$OUT/shaders" /project/third_party/cafeglsl/glslcompiler.elf 2>&1 | tail -4 | cut -c1-150
rm -rf "$OUT/shaders/glsl"
ls -la "$OUT/shaders" | cut -c24-70
