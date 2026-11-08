#!/bin/bash
# Runs inside the contained environment: compiles the runtime for this PC and links it with the game's code.
# Prints only the first errors from each file, and stops short of linking if anything fails to compile.
RT=/project/third_party/3dsrecomp/runtime
OBJ=/root/pc_rt
mkdir -p "$OBJ"
FLAGS="-std=c++20 -O1 -g -w -DRECOMP_VS_LOG -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h -DRECOMP_GAME=\"tomodachi\" -DRECOMP_SOURCE_DIR=\"/project/third_party/3dsrecomp\""
fail=0
sources=()
for s in "$RT"/*.cpp "$RT"/services/*.cpp "$RT"/gpu/*.cpp "$RT"/audio/*.cpp /project/port/pc/*.cpp; do
    case "$(basename "$s")" in
        frontend.cpp|main.cpp|host_sdl.cpp|watch.cpp) continue ;;
    esac
    sources+=("$s")
done
compile() {
    s=$1
    o="$OBJ/$(echo "$s" | tr '/' '_').o"
    if [ ! "$o" -nt "$s" ] || [ -n "$FORCE" ]; then
        clang++-18 $FLAGS -c "$s" -o "$o" 2> "$o.err" || { echo "=== $(basename "$s")"; grep -E 'error' "$o.err" | head -${ERRS:-8} | cut -c1-170; return 1; }
    fi
}
export -f compile
export FLAGS OBJ FORCE ERRS
printf '%s\n' "${sources[@]}" | xargs -P 12 -I{} bash -c 'compile {}' || fail=1
if [ "$fail" != 0 ]; then echo "compile errors above"; exit 1; fi
GAME_OBJ=${GAME_OBJ:-/root/pc_game}
clang++-18 -fuse-ld=lld-18 "$OBJ"/*.o "$GAME_OBJ"/*.o -lpthread -ldl -lm -rdynamic -Wl,--error-limit=0 -o /root/tomodachi_pc 2>&1 | grep -E 'undefined symbol|error' | sed 's/^ld.lld-18: //' | sort -u | head -${LINKERRS:-60} | cut -c1-150
ls -la /root/tomodachi_pc 2>/dev/null | cut -c20-
