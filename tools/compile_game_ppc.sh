#!/bin/bash
# Runs inside the contained environment: compiles generated game C for the Wii U and reports the size of the code.
# compile_game_ppc.sh <generated dir> <object dir> [extra flags]. Objects go on the Linux disk, which is much faster.
GEN=$1; OBJ=$2; shift 2
mkdir -p "$OBJ"
export OBJ
export FLAGS="-O2 -mcpu=750 -meabi -mhard-float -ffp-contract=off -fno-math-errno -fno-strict-aliasing -std=gnu11 -w -I/project/port -I/project/third_party/3dsrecomp/runtime -I$GEN $*"
compile() {
    s=$1
    o="$OBJ/$(basename "$s" .c).o"
    [ "$o" -nt "$s" ] || powerpc-eabi-gcc $FLAGS -c "$s" -o "$o" || echo "FAILED $s"
}
export -f compile
time (ls "$GEN"/*.c | xargs -P 8 -I{} bash -c 'compile {}')
powerpc-eabi-size -t "$OBJ"/*.o | tail -1
ls "$OBJ"/*.o | wc -l
