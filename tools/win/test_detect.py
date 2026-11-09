"""Self-check of the detections that pick how a game builds (build3ds.py): coroutine_switches finds a setjmp call followed by a longjmp call
(Ace Attorney Trilogy's task switch), and not a setjmp alone (an error handler, as in Metroid). Synthetic code, so it runs anywhere.
Run: python tools/win/test_detect.py"""
import importlib.util
import os
import shutil
import struct
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('b3', os.path.join(HERE, 'build3ds.py'))
b3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b3)

SETJMP = [0xE8A04F00, 0xE8A000F0, 0xE480D004, 0xE3B00000, 0xE12FFF1E]  # stm r0!, {r8-r11, lr}; stm r0!, {r4-r7}; str sp, [r0], #4; movs r0, #0; bx lr
LONGJMP = [0xE280802C, 0xE1A04001, 0xE5380004, 0xE1A0D000, 0xE1B00004, 0xE93800F0, 0x03A00001, 0xE9184F00, 0xE12FFF1E]  # ... mov sp, r0 ... ldmdb r8, {r8-r11, lr}; bx lr
NOP = 0xE1A00000


def bl(at, to):
    return 0xEB000000 | (((to - at - 8) >> 2) & 0xFFFFFF)


made = []


def game(words):
    d = tempfile.mkdtemp(prefix='detect_test_')
    made.append(d)
    code = struct.pack(f'<{len(words)}I', *words)
    with open(os.path.join(d, 'code.bin'), 'wb') as f:
        f.write(code)
    head = bytearray(0x40)
    struct.pack_into('<I', head, 0x18, len(code))  # the text size build3ds reads
    with open(os.path.join(d, 'exheader.bin'), 'wb') as f:
        f.write(head)
    return d


def layout(switch):
    """setjmp at 0x100000, longjmp after it, then a caller: setjmp, then (for a switch) longjmp three instructions later"""
    words = SETJMP + LONGJMP
    sj, lj = 0x100000, 0x100000 + 4 * len(SETJMP)
    at = 0x100000 + 4 * len(words)
    words += [bl(at, sj), NOP, NOP]
    if switch:
        words += [bl(at + 12, lj)]
    return words + [NOP] * 20


try:
    assert b3.coroutine_switches(game(layout(True))) == 1, 'a setjmp followed by a longjmp is a task switch'
    assert b3.coroutine_switches(game(layout(False))) == 0, 'a setjmp on its own is an error handler'
    assert b3.mobiclip_tables(game(layout(True))) == 0
    print('build mode detections: ok')
finally:
    for d in made:
        shutil.rmtree(d, ignore_errors=True)
