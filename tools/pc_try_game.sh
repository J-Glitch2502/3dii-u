#!/bin/bash
# Runs inside the contained environment: recompiles another game for this PC in its own folders and boots it headless with the
# autoplay input, to see how far it gets. Only reads the shared runtime objects in /root/pc_rt. Uses the generator's full-sync mode
# (no --abi): hand-written code such as video decoders doesn't follow the calling convention --abi trusts.
# pc_try_game.sh <name under games/> [frames, default 3600]. Output: games/<name>/shots/top.png and bottom.png, and the log's tail.
# Code found missing at run time is compiled on the spot, and added to games/<name>/functions.txt for the next full recompile.
N=$1
FRAMES=${2:-3600}
G=/project/games/$N
OBJ=/root/pc_$N
RUN=/root/run_$N
BIN=/root/${N}_pc
RT=/project/third_party/3dsrecomp/runtime
set -e
python3 /project/tools/strip_romfs.py "$G" > /dev/null
mkdir -p "$G/exefs"
[ -f "$G/icon.bin" ] && mv "$G/icon.bin" "$G/exefs/icon.bin"
[ -f "$G/functions.txt" ] || touch "$G/functions.txt"
if [ ! -f "$G/gen_safe/table.c" ] || [ "$G/functions.txt" -nt "$G/gen_safe/table.c" ]; then
    rm -rf "$OBJ"
    (cd /project/third_party/3dsrecomp && python3 -m tools.recomp.gen "$G" "$G/gen_safe" --extra "$G/functions.txt" 2>&1 | grep -E 'instructions:|^functions|undefined instructions' | cut -c1-120)
fi
# At most JOBS compiles at once, at low priority: eight at once starved the rest of the machine and ran it out of memory.
mkdir -p "$OBJ"
export OBJ CC_FLAGS="-O1 -w -ffp-contract=off -fno-strict-aliasing -std=gnu11 -I$RT -I$G/gen_safe"
ls "$G/gen_safe"/*.c | xargs -P "${JOBS:-4}" -I{} bash -c 'o="$OBJ/$(basename "$1" .c).o"; [ "$o" -nt "$1" ] || nice -n 10 clang-18 $CC_FLAGS -c "$1" -o "$o" || echo "FAILED $1"' _ {} | tail -3 | cut -c1-120
for c in "$G/gen_safe"/*.c; do
    [ -f "$OBJ/$(basename "$c" .c).o" ] || { echo "not compiled: $c"; exit 1; }
done
# A private copy of core.cpp points the runtime's on-the-spot compile at this game, on Linux.
sed -e 's#generated/" + game#" + game#g' -e 's#-undefined dynamic_lookup ##' -e 's#clang -O1#clang-18 -O1#' "$RT/core.cpp" > "$OBJ/core_lazy.cpp"
clang++-18 $EXTRA_FLAGS -std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h \
    -DRECOMP_GAME="\"$G/gen_safe\"" -DRECOMP_SOURCE_DIR="\"/project/third_party/3dsrecomp\"" -c "$OBJ/core_lazy.cpp" -o "$OBJ/core_lazy.o"
rm -f "$OBJ/core_lazy.cpp"
# services this trial needs newer than the shared objects
for s in misc gsp y2r; do
    clang++-18 -std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h -c "$RT/services/$s.cpp" -o "$OBJ/svc_$s.o"
done
clang++-18 -std=c++20 -O1 -g -w -DRECOMP_VS_LOG -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h -c /project/port/pc/frontend_pc.cpp -o "$OBJ/frontend_pc.o"
clang++-18 -std=c++20 -O1 -g -w -DRECOMP_VS_LOG -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h -c "$RT/gpu/vertex.cpp" -o "$OBJ/gpu_vertex.o"
clang++-18 -fuse-ld=lld-18 $(ls /root/pc_rt/*.o | grep -vE 'runtime_core.cpp|services_(misc|gsp|y2r).cpp|port_pc_frontend_pc.cpp|gpu_vertex.cpp') "$OBJ"/*.o -lpthread -ldl -lm -rdynamic -Wl,--error-limit=0 -o "$BIN" 2>&1 | grep -E 'undefined symbol|error' | sort -u | head -20 | cut -c1-150
mkdir -p "$RUN" "$G/shots"
cd "$RUN"
rm -rf save shots
set +e
RECOMP_AUTOPLAY=${AUTOPLAY:-1} RECOMP_NOAUDIO=1 RECOMP_FRAMES=$FRAMES RECOMP_SHOT_EVERY=${SHOT_EVERY:-300} RECOMP_SHOT_DIR="$RUN/shots" timeout $((FRAMES / 60 + 40)) nice -n 10 "$BIN" "$G" > run.log 2>&1
echo "exit $?"
grep -v '\[thread \|frame \|ControlMemory\|recompiling missing' run.log | tail -${LOGLINES:-22} | cut -c1-150
grep ' frame ' run.log | tail -1 | cut -c1-90
if [ -f save/missing_functions.txt ]; then
    cat save/missing_functions.txt "$G/functions.txt" | grep -v '^0x00000[0-9a-f]*$' | sort -u > "$RUN/functions.new" || true
    cmp -s "$RUN/functions.new" "$G/functions.txt" || cp "$RUN/functions.new" "$G/functions.txt"
    echo "functions found at run time so far: $(wc -l < "$G/functions.txt")"
fi
python3 /project/tools/contact_sheet.py shots "$G/shots/sheet.png" ${COLS:-8}
grep -c "\[y2r\]" run.log
