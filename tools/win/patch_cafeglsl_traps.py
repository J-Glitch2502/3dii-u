"""Turns the conditional trap instructions in the Wii U CafeGLSL library into no-ops. Run once after installing the library; running it again changes nothing.

Why: GCC put five `tweq` (trap if equal) instructions into r600's ConditionalJumpTracker, right after the atomic adds on its shared_ptr counts: a
refcount-overflow check that never fires on hardware. Cemu, though, treats every `tw` instruction as one of its own debugger breakpoints
(PPCInterpreter_TW) and parks the thread there, so the first shader with real control flow (a loop) hung the console compiler for good in Cemu,
in the interpreter and the recompiler alike (2026-11-06). A trap whose condition is false is a no-op on the console either way.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace('\\', '/')
LIB = ROOT + '/third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc/lib/libcafeglsl.a'
AR = 'C:/devkitPro/opt/devkitpro/devkitPPC/bin/powerpc-eabi-ar.exe'
MEMBER = 'sfn_sfn_conditionaljumptracker.cpp.o'
NOP = bytes.fromhex('60000000')


def is_tw(word):
    """Opcode 31, extended opcode 4: tw TO,rA,rB. TO 0 is Cemu's own breakpoint encoding and never comes from the compiler."""
    return word >> 26 == 31 and (word & 0x7FF) == 8 and (word >> 21) & 31 != 0


def main():
    work = os.path.dirname(LIB)
    subprocess.run([AR, 'x', LIB, MEMBER], cwd=work, check=True)
    path = work + '/' + MEMBER
    d = bytearray(open(path, 'rb').read())
    n = 0
    for i in range(0, len(d) - 3, 4):
        if is_tw(int.from_bytes(d[i:i + 4], 'big')):
            d[i:i + 4] = NOP
            n += 1
    if n:
        open(path, 'wb').write(d)
        subprocess.run([AR, 'r', LIB, MEMBER], cwd=work, check=True)
    os.remove(path)
    print(f'{MEMBER}: {n} trap instructions made no-ops' if n else f'{MEMBER}: no trap instructions, nothing to do')


if __name__ == '__main__':
    sys.exit(main())
