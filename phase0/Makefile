# Run inside the contained environment, from phase0/.
#   make counts   reference results and instruction counts from the ARM workload
#   make lift     ARM workload -> build/generated/lifted.c
#   make check    runs the recompiled code on this PC and on a big-endian PowerPC emulator
#   make wiiu     the Wii U app
WUT := $(DEVKITPRO)/wut
FP := -ffp-contract=off -fno-math-errno
COMMON := -O2 $(FP) -Wall -Wno-unused-function -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized
INC := -Ihost -Ibuild/generated -Ibuild/host
WORKLOAD := -Dguest_main=native_main -ffreestanding -fno-builtin
LIFTED_ABI := build/generated/lifted_abi.c
LIFTED_SAFE := build/generated/lifted_safe.c
ESPRESSO := -DESPRESSO -D__WIIU__ -D__WUT__ -mcpu=750 -meabi -mhard-float $(COMMON)
PPC_LINUX := -mcpu=750 -mhard-float -static $(COMMON)

.PHONY: wiiu check counts lift
counts: build/host/counts.h
build/host/counts.h: build/guest/code.bin tools/count.py
	mkdir -p build/host
	python3 tools/count.py $< $@

lift: $(LIFTED_ABI)
$(LIFTED_ABI): build/guest/workload.elf tools/lift.py
	python3 tools/lift.py $< build/generated

check: counts lift
	mkdir -p build/check
	gcc $(COMMON) -DLIFTED $(INC) -c host/bench.c -o build/check/bench.o
	gcc $(COMMON) $(INC) -c $(LIFTED_ABI) -o build/check/abi.o
	gcc $(COMMON) $(INC) -c $(LIFTED_SAFE) -o build/check/safe.o
	gcc $(COMMON) $(WORKLOAD) -c guest/workload.c -o build/check/workload.o
	gcc build/check/bench.o build/check/abi.o build/check/safe.o build/check/workload.o -lm -o build/check/bench_pc
	build/check/bench_pc --verify
	powerpc-linux-gnu-gcc $(PPC_LINUX) -DLIFTED $(INC) -c host/bench.c -o build/check/bench_ppc.o
	powerpc-linux-gnu-gcc $(PPC_LINUX) $(INC) -c $(LIFTED_ABI) -o build/check/abi_ppc.o
	powerpc-linux-gnu-gcc $(PPC_LINUX) $(INC) -c $(LIFTED_SAFE) -o build/check/safe_ppc.o
	powerpc-linux-gnu-gcc $(PPC_LINUX) $(WORKLOAD) -c guest/workload.c -o build/check/workload_ppc.o
	powerpc-linux-gnu-gcc -static build/check/bench_ppc.o build/check/abi_ppc.o build/check/safe_ppc.o build/check/workload_ppc.o -lm -o build/check/bench_ppc
	qemu-ppc-static -cpu 750 build/check/bench_ppc --verify

wiiu: counts lift
	mkdir -p build/wiiu
	powerpc-eabi-gcc $(ESPRESSO) -DLIFTED $(INC) -isystem $(WUT)/include -c host/bench.c -o build/wiiu/bench.o
	powerpc-eabi-gcc $(ESPRESSO) $(INC) -c $(LIFTED_ABI) -o build/wiiu/abi.o
	powerpc-eabi-gcc $(ESPRESSO) $(INC) -c $(LIFTED_SAFE) -o build/wiiu/safe.o
	powerpc-eabi-gcc $(ESPRESSO) $(WORKLOAD) -c guest/workload.c -o build/wiiu/workload.o
	powerpc-eabi-gcc -mcpu=750 -meabi -mhard-float -specs=$(WUT)/share/wut.specs -L$(WUT)/lib \
	    build/wiiu/bench.o build/wiiu/abi.o build/wiiu/safe.o build/wiiu/workload.o -lwut -lm -o build/wiiu/bench.elf
	elf2rpl build/wiiu/bench.elf build/wiiu/bench.rpx
	ls -l build/wiiu/bench.rpx
