#!/bin/bash
# Runs inside the contained environment: compiles the runtime for the Wii U. Objects go to /root/wiiu_rt.
# Prints only the first errors from each file. Env: FORCE (recompile everything), ERRS (errors per file).
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
W=$DEVKITPRO/wut
RT=/project/third_party/3dsrecomp/runtime
OBJ=/root/wiiu_rt
mkdir -p "$OBJ"
FLAGS="-std=c++20 -O2 -g0 -w -mcpu=750 -meabi -mhard-float -ffunction-sections -fno-strict-aliasing -fno-exceptions -fno-if-conversion -fno-if-conversion2 -D__WIIU__ -D__WUT__ -DESPRESSO -I/project/port/wiiu -I/project/port -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -isystem $W/include -include /project/port/wiiu/shim.h -DRECOMP_GAME=\"tomodachi\""
sources=()
for s in "$RT"/*.cpp "$RT"/services/*.cpp "$RT"/gpu/*.cpp "$RT"/audio/*.cpp /project/port/wiiu/*.cpp; do
    case "$(basename "$s")" in
        frontend.cpp|main.cpp|host_sdl.cpp|watch.cpp|profile.cpp|shadergen.cpp) continue ;;
    esac
    sources+=("$s")
done
compile() {
    s=$1
    o="$OBJ/$(echo "$s" | tr '/' '_').o"
    if [ ! "$o" -nt "$s" ] || [ -n "$FORCE" ]; then
        powerpc-eabi-g++ $FLAGS -c "$s" -o "$o" 2> "$o.err" || { echo "=== $(basename "$s")"; grep -E 'error|Error' "$o.err" | head -${ERRS:-8} | cut -c1-190; rm -f "$o"; return 1; }
        rm -f "$o.err"
    fi
}
export -f compile
export FLAGS OBJ FORCE ERRS
printf '%s\n' "${sources[@]}" | xargs -P 8 -I{} bash -c 'compile {}' || { echo "compile errors above"; exit 1; }
echo "runtime compiled: $(ls "$OBJ"/*.o | wc -l) objects"
