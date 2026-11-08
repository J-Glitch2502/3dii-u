#!/bin/bash
# Runs inside the contained environment: finds guest memory accesses the Wii U build can't serve. The PC build maps 4 GiB of guest
# address space, but the Wii U's flat block ends at 0x20010000, so this run makes everything above that fault and stops in gdb there.
# pc_guard.sh <name under games/> [frames]. Needs a PC trial first (pc_try_game.sh).
N=$1
FRAMES=${2:-3600}
G=/project/games/$N
OBJ=/root/pc_$N
RUN=/root/run_$N
RT=/project/third_party/3dsrecomp/runtime
BIN=/root/${N}_guard
GUARD=/root/guard_$N
mkdir -p "$GUARD"
sed -e 's#generated/" + game#" + game#g' -e 's#-undefined dynamic_lookup ##' -e 's#clang -O1#clang-18 -O1#' \
    -e 's#^    g_mem = (u8 \*)p;#    g_mem = (u8 *)p; mprotect((u8 *)p + 0x20010000, size - 0x20010000, PROT_NONE);#' "$RT/core.cpp" > "$GUARD/core_guard.cpp"
grep -q 'PROT_NONE);' "$GUARD/core_guard.cpp" || { echo "could not add the guard"; exit 1; }
clang++-18 -std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h \
    -DRECOMP_GAME="\"$G/gen_safe\"" -DRECOMP_SOURCE_DIR="\"/project/third_party/3dsrecomp\"" -c "$GUARD/core_guard.cpp" -o "$GUARD/core_guard.o" || exit 1
# and the Wii U's host stacks: 2 MB per guest thread instead of 64
sed -e 's#pthread_attr_setstacksize(&attr, 64 << 20)#pthread_attr_setstacksize(\&attr, ${STACK_KB:-2048} << 10)#' "$RT/kernel.cpp" | sed "s#\${STACK_KB:-2048}#${STACK_KB:-2048}#" > "$GUARD/kernel_guard.cpp"
grep -q "${STACK_KB:-2048} << 10" "$GUARD/kernel_guard.cpp" || { echo "could not shrink the stacks"; exit 1; }
clang++-18 -std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h -c "$GUARD/kernel_guard.cpp" -o "$GUARD/kernel_guard.o" || exit 1
clang++-18 -fuse-ld=lld-18 $(ls /root/pc_rt/*.o | grep -vE 'runtime_core.cpp|runtime_kernel.cpp|services_(misc|gsp|y2r).cpp') $(ls "$OBJ"/*.o | grep -v core_lazy.o) "$GUARD/core_guard.o" "$GUARD/kernel_guard.o" \
    -lpthread -ldl -lm -rdynamic -Wl,--error-limit=0 -o "$BIN" 2>&1 | grep -E 'undefined symbol|error' | sort -u | head | cut -c1-150
cat > "$GUARD/run.gdb" <<'EOF'
set pagination off
handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass
run
printf "fault at host address %p, guest address %08x\n", $_siginfo._sifields._sigfault.si_addr, (unsigned)((char *)$_siginfo._sifields._sigfault.si_addr - (char *)g_mem)
bt 10
x/6i $pc-16
info registers rax rbx rcx rdx rsi rdi
quit
EOF
cd "$RUN"
rm -rf save shots
RECOMP_AUTOPLAY=1 RECOMP_NOAUDIO=1 RECOMP_FRAMES=$FRAMES timeout $((FRAMES / 60 + 60)) gdb -q -batch -x "$GUARD/run.gdb" --args "$BIN" "$G" 2>&1 | grep -vE 'New Thread|exited\]|libthread_db|\[fs\]|\[boss|\[thread |frame |ControlMemory|Inferior' | tail -32 | cut -c1-140
rm -f "$BIN"
