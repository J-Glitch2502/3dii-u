#!/bin/bash
# Runs inside the contained environment: builds one Mario game for the Wii U in its own folders, without touching the Tomodachi or Layton
# builds. At most 4 low-priority compile jobs. Needs a PC trial first (mario_pc_trial.sh), for the shader logs and the code found at run time.
# mario_wiiu.sh <name under games/> [safe|abi]. safe: the generator's full-sync code, from the PC trial. abi: --abi, faster, trusts the calling convention.
# Output: build_mario/<name>/<name>.rpx, game.nm and shaders. Then run tools/mario_title.py with Windows Python.
# Cemu's recompiler gets some of gcc's compares wrong at -O1 as well as -O2 (Paper Jam stays on its clear colour), so test with --force-interpreter:
# that's right, and so is the console. OPT overrides -O2. Changing it needs the game's object folder deleted.
N=$1
MODE=${2:-safe}
G=/project/games/$N
OUT=/project/build_mario/$N
RTOBJ=/root/wiiu_rt_mario
OBJ=/root/ppc_${N}_$MODE
LINK=/root/wiiu_link_mario
export RECOMP_CRO_BASE=0x1C000000
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
W=$DEVKITPRO/wut
RT=/project/third_party/3dsrecomp/runtime
mkdir -p "$OUT/shaders" "$LINK" "$G/shader_logs"
[ -f /root/rpx.specs ] || bash /project/tools/rpx_link_setup.sh >/dev/null

# cond_fix's wrap isn't linked: a console build with it hung at the start-up splash. Uber shaders stay in: without them a missed shader draws wrong.
sed -e "s#^OBJ=/root/wiiu_rt\$#OBJ=$RTOBJ#" -e 's/xargs -P 8/xargs -P 4/' -e "s#^FLAGS=\"#FLAGS=\"$EXTRA_FLAGS #" /project/tools/wiiu_runtime.sh > /root/wiiu_runtime_mario.sh
grep -q "OBJ=$RTOBJ" /root/wiiu_runtime_mario.sh || { echo "runtime script changed: no object folder line"; exit 1; }
nice -n 10 bash /root/wiiu_runtime_mario.sh 2>&1 | tail -6 | cut -c1-170
ls "$RTOBJ" | grep -q 'mic_wiiu.cpp.o$' || { echo "runtime did not compile"; exit 1; }

GEN=$G/gen_safe
if [ "$MODE" = abi ]; then
    GEN=$G/gen_abi
    if [ ! -f "$GEN/table.c" ] || [ "$G/functions.txt" -nt "$GEN/table.c" ] || [ /project/third_party/3dsrecomp/tools/recomp/gen.py -nt "$GEN/table.c" ] || [ /project/third_party/3dsrecomp/tools/recomp/cro.py -nt "$GEN/table.c" ]; then
        rm -rf "$OBJ"
        (cd /project/third_party/3dsrecomp && python3 -m tools.recomp.gen "$G" "$GEN" --abi --extra "$G/functions.txt" 2>&1 | grep -E 'instructions:|^functions|undefined instructions' | cut -c1-120)
    fi
fi
[ -f "$GEN/table.c" ] || { echo "no generated code in $GEN"; exit 1; }
sed "s/-P 8/-P ${WJOBS:-4}/" /project/tools/compile_game_ppc.sh > /root/compile_ppc_mario.sh
nice -n 10 bash /root/compile_ppc_mario.sh "$GEN" "$OBJ" ${OPT:--O2} 2>&1 | grep -E 'FAILED|TOTALS' | cut -c1-120
for c in "$GEN"/*.c; do
    [ -f "$OBJ/$(basename "$c" .c).o" ] || { echo "not compiled: $c"; exit 1; }
done

rm -f "$LINK/$N.elf"
# their classic soundtracks (tools/aac_build.sh)
AAC=
case $N in superstar_saga|bowsers_inside)
    powerpc-eabi-g++ -mcpu=750 -meabi -mhard-float -O2 -std=c++20 -D__WIIU__ -I/root/aac_ppc/include -c /project/port/aac/aac_ffmpeg.cpp -o "$LINK/aac_ffmpeg.o" || exit 1
    AAC="$LINK/aac_ffmpeg.o /root/aac_ppc/lib/libavcodec.a /root/aac_ppc/lib/libavutil.a"
esac
powerpc-eabi-g++ -mcpu=750 -meabi -mhard-float -specs=/root/rpx.specs -Wl,--relax -Wl,--no-warn-rwx-segments \
    "$RTOBJ"/*.o $AAC -Wl,--start-group -L$W/lib -lwut -lstdc++ -lm -lc -lsysbase -lgcc -Wl,--end-group "$OBJ"/*.o \
    -o "$LINK/$N.elf" 2>&1 | grep -E 'undefined reference|error|multiple definition|truncated' | sed 's#^.*ld: ##' | sort -u | head -30 | cut -c1-150
[ -f "$LINK/$N.elf" ] || exit 1
powerpc-eabi-nm -n "$LINK/$N.elf" > "$OUT/game.nm"
elf2rpl "$LINK/$N.elf" "$OUT/$N.rpx" 2>&1 | tail -2
powerpc-eabi-size "$LINK/$N.elf" | tail -1 | cut -c1-70
# the last build known to boot on the console imported coreinit gx2 proc_ui sndcore2 sysapp vpad: anything else here is a suspect if a build hangs at its splash
echo "imports: $(powerpc-eabi-objdump -h "$LINK/$N.elf" | grep -oE '\.fimport_[a-zA-Z0-9_]+' | sort -u | sed 's/.fimport_//' | tr '\n' ' ')"
rm -f "$LINK/$N.elf"
ls -la "$OUT/$N.rpx" | cut -c24-

# The generator, in its own folder, since Tomodachi's build uses /root/shadergen. Shaders this game has used so far, plus its known
# pipelines where the base project has them.
SG=/root/shadergen_mario
mkdir -p "$SG"
SGFLAGS="-std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/gpu -include /project/port/pc/shim.h"
for s in /project/tools/shadergen_main.cpp /project/port/wiiu/shadergen.cpp /project/port/wiiu/shaderkeys.cpp $RT/gpu/shader.cpp; do
    o="$SG/$(basename "$s").o"
    if [ ! "$o" -nt "$s" ] || [ /project/port/wiiu/shadergen.h -nt "$o" ] || [ /project/port/wiiu/fs_key.inc -nt "$o" ]; then nice -n 10 clang++-18 $SGFLAGS -c "$s" -o "$o" || exit 1; fi
done
sed -n "/^cat > \"\$OBJ\/stubs.cpp\"/,/^EOF/p" /project/tools/shaders_build.sh | sed -e '1d' -e '$d' -e 's/fnv_host(p, n)/0/' > "$SG/stubs.cpp"
nice -n 10 clang++-18 $SGFLAGS -c "$SG/stubs.cpp" -o "$SG/stubs.o" || exit 1
clang++-18 -fuse-ld=lld-18 "$SG"/*.o -lpthread -lm -o "$SG/shadergen" 2>&1 | grep -E 'undefined symbol|error' | sort -u | head -20 | cut -c1-150

for f in vs_log.bin fs_log.bin; do
    [ -f "/root/run_$N/$f" ] && cp "/root/run_$N/$f" "$G/shader_logs/"
    [ -f "/root/runa_$N/$f" ] && cp "/root/runa_$N/$f" "$G/shader_logs/abi_$f"
done
# Fragment shaders depend on render state, not on the game, so what another game logged may save this one a plain-coloured fallback in scenes no run reached
for o in /project/games/*/; do
    o=${o%/}
    if [ "$o" != "$G" ] && [ -f "$o/shader_logs/fs_log.bin" ]; then cp "$o/shader_logs/fs_log.bin" "$G/shader_logs/other_$(basename "$o").bin"; fi
    if [ "$o" != "$G" ]; then
        for f in "$o"/shader_logs/*_fs.bin; do
            [ -f "$f" ] && cp "$f" "$G/shader_logs/other_$(basename "$o")_$(basename "$f")"
        done
    fi
done
TID=$(python3 -c "import json;print(json.load(open('$G/info.json'))['program_id'].lower())")
PIPE=/project/third_party/3dsrecomp/games/$TID/pipelines.bin
[ -f "$PIPE" ] || PIPE=/dev/null
"$SG/shadergen" "$PIPE" "$G/shader_logs" "$OUT/shaders" /project/third_party/cafeglsl/glslcompiler.elf 2>&1 | tail -5 | cut -c1-150
rm -rf "$OUT/shaders/glsl"
ls -la "$OUT/shaders" | cut -c24-70
