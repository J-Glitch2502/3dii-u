#!/bin/bash
# Runs inside the contained environment: compiles generated game C for this PC (x86-64), for debugging the runtime.
# compile_game_pc.sh <generated dir> <object dir>. Objects go on the Linux disk, which is much faster.
GEN=$1; OBJ=$2
mkdir -p "$OBJ"
export OBJ
export FLAGS="-O1 -w -ffp-contract=off -fno-strict-aliasing -std=gnu11 -I/project/third_party/3dsrecomp/runtime -I$GEN"
compile() {
    s=$1
    o="$OBJ/$(basename "$s" .c).o"
    [ "$o" -nt "$s" ] || clang-18 $FLAGS -c "$s" -o "$o" || echo "FAILED $s"
}
export -f compile
time (ls "$GEN"/*.c | xargs -P 8 -I{} bash -c 'compile {}')
ls "$OBJ"/*.o | wc -l
