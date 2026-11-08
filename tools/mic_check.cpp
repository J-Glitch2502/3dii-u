// Self-check of mic:u's sample writing: encodings, wrapping, the end of an unlooped buffer. The host microphone is the default silence.
// Runs inside the contained environment, with any PC runtime objects (see tools/pc_runtime.sh), from /root:
//   RT=/project/third_party/3dsrecomp/runtime; clang++-18 -std=c++20 -O1 -g -w -I/project/port/wiiu -I/project/port/pc -I$RT -I$RT/audio -I$RT/gpu -I$RT/services -include /project/port/pc/shim.h /project/tools/mic_check.cpp $(ls /root/pc_rt_mario/*.o | grep -vE 'services_misc|frontend_pc') -fuse-ld=lld-18 -static -Wl,--unresolved-symbols=ignore-all -lpthread -ldl -lm -o /root/mic_check && /root/mic_check; rm -f /root/mic_check
#undef NDEBUG
#include <cassert>

#include "services/misc.cpp"

int main() {
    g_mem = (u8 *)calloc(1, 0x10000);
    kernel::Mic m("test");
    m.mem = std::make_shared<kernel::SharedMem>();
    m.mem->backing = 0x1000;
    m.mem->size = 0x1000;
    const u32 last_word = 0x1FFC;

    m.encoding = 0;
    m.store(0x100, 0x1234);
    assert(r8(0x100) == 0x92);
    m.encoding = 1;
    m.store(0x100, 0x1234);
    assert(r16(0x100) == 0x9234);
    m.encoding = 2;
    m.store(0x100, -0x100);
    assert(r8(0x100) == 0xFF);
    m.encoding = 3;
    m.store(0x100, -2);
    assert(r16(0x100) == 0xFFFE);

    // looped: 81 8-bit samples per tick into 100 bytes from offset 16, so the second tick wraps
    m.encoding = 0;
    m.rate = 8180;
    m.start = m.pos = 16;
    m.end = 116;
    m.loop = m.sampling = true;
    m.tick(m.generation);
    assert(m.pos == 97 && r32(last_word) == 97 && !m.buf_full->signaled && r8(0x1000 + 16) == 128);
    m.tick(m.generation);
    assert(m.pos == 16 + 62 && r32(last_word) == 78 && m.buf_full->signaled && m.sampling);

    // a tick left over from an earlier StartSampling does nothing
    m.tick(m.generation - 1);
    assert(m.pos == 78);

    // unlooped: 16-bit samples stop at the end of a 200-byte buffer, and report that position
    m.buf_full->signaled = false;
    m.encoding = 1;
    m.loop = false;
    m.pos = 16;
    m.end = 216;
    m.tick(m.generation);
    assert(m.pos == 16 + 162 && m.sampling);
    m.tick(m.generation);
    assert(m.pos == 216 && r32(last_word) == 216 && m.buf_full->signaled && !m.sampling);
    assert(r16(0x1000 + 214) == 0x8000);

    puts("mic check passed");
    fflush(stdout);
    _exit(0);  // the timer queue still holds the ticks this test scheduled
}
