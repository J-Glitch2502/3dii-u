// Shaders the shipped set lacks are compiled on the console itself, on a thread of their own, with CafeGLSL's library for the Wii U
// (third_party/cafeglsl_ppc, the same compiler family the PC build runs). A miss draws with its fallback meanwhile and takes the real
// shader once the GPU thread installs it (gfx_pump). The GLSL of every miss goes to the save folder, so the next start compiles it again
// in the background before the game gets there. The first of the three things 3Dii U needs: no more builds for new areas (2026-11-06).
#include <cafeglsl/CafeGLSLCompiler.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>

#include <dirent.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "shader_compile.h"
#include "shadergen.h"
#include "wiiu.h"



namespace gpu {
namespace {
struct Job {
    bool vs, cached;
    u64 key;
    u32 inputs;
    std::string glsl;
};
OSMutex mutex;
std::deque<Job> jobs;
std::vector<CompileDone> done;
std::unordered_set<u64> known;  // asked for once: the cache and a live miss may name the same shader
constexpr u32 STACK = 4 << 20;  // the compiler recurses
OSThread *compiler;
volatile OSTime busy_since;  // when the compile in progress began, 0 between compiles
volatile u32 watched;        // reports written about the compile in progress

// One cache per shipped set: what an older build missed and compiled here is mostly in a newer one's set, and compiling it all again at every
// start kept this thread (and the heap lock it takes all the time) busy for minutes, through Luigi's Mansion: Dark Moon's intro, whose sound
// and pictures stuttered meanwhile (1129 shaders, at ~300 ms each, 2026-11-11). A new build starts its cache empty; the old file goes.
const std::string &cache_path() {
    static std::string p;
    if (p.empty()) {
        char name[48];
        snprintf(name, sizeof name, "/shader_cache_%016llx.bin", (unsigned long long)gpu::shipped_set_id);
        p = wiiu::data_dir() + name;
    }
    return p;
}

static void cache_drop_old() {
    const std::string dir = wiiu::data_dir();
    remove((dir + "/shader_cache.bin").c_str());
    if (DIR *d = opendir(dir.c_str())) {
        std::vector<std::string> old;
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("shader_cache_", 0) == 0 && dir + "/" + n != cache_path()) old.push_back(dir + "/" + n);
        }
        closedir(d);
        for (const std::string &o : old) {
            remove(o.c_str());
            LOG("[glsl] an older build's cache gone: %s\n", o.c_str());
        }
    }
}

// u8 vs, u8 pad[3], u32 inputs, u64 key, u32 length, the GLSL: this console's own bytes, nobody else reads it
void cache_append(const Job &j) {
    wiiu::fs_begin(0xC00B);  // its turn with every other file call
    if (FILE *f = fopen(cache_path().c_str(), "ab")) {
        const u32 head[5] = {(u32)j.vs << 24, j.inputs, (u32)(j.key >> 32), (u32)j.key, (u32)j.glsl.size()};
        fwrite(head, 1, sizeof head, f);
        fwrite(j.glsl.data(), 1, j.glsl.size(), f);
        fclose(f);
    }
    wiiu::fs_end();
    // No flush of its own: one takes ~400 ms on the card thread, and the game's next file read waits behind it, which stalled Bowser's Inside Story's
    // battles and NSMB2's menus as their shaders compiled (2026-11-08). The cache goes out with the game's next save or at quitting; lost to a power-off,
    // it's only compiled again. A journal that fills up first is flushed by the write that finds it full (write_retrying).
}

void cache_load() {
    cache_drop_old();
    FILE *f = fopen(cache_path().c_str(), "rb");
    if (!f) return;
    u32 n = 0, head[5];
    while (fread(head, 1, sizeof head, f) == sizeof head && head[4] < (1 << 20)) {
        Job j{(head[0] >> 24) != 0, true, (u64)head[2] << 32 | head[3], head[1], std::string(head[4], '\0')};
        if (fread(&j.glsl[0], 1, head[4], f) != head[4]) break;
        if (known.insert(j.key).second) {
            jobs.push_back(std::move(j));
            n++;
        }
    }
    fclose(f);
    LOG("[glsl] %u shaders from the cache to compile\n", n);
}

int compile_thread(int, void *) {
    InitGLSLCompiler();
    LOG("[glsl] compiler %s up\n", GetGLSLCompilerVersion());
    static char info[4096];
    {  // two known small shaders first: whether compiling works here at all, how long one takes, and that a loop (r600's jump tracker) gets through: in
        // Cemu it only does with tools/win/patch_cafeglsl_traps.py applied to the library
        const std::string plain = gen_passthrough_vs();
        const std::string with_loop = "#version 450\nlayout(location = 0) in vec4 aPos;\nlayout(binding = 0, std140) uniform VS { vec4 f[96]; vec4 uDraw; ivec4 uInt[4]; };\n"
                                      "void main() { vec4 p = aPos; for (int i = 0; i < uint(uInt[0].x) + 1; i++) p += f[i]; gl_Position = p; }\n";
        const struct {
            const char *name;
            const std::string *glsl;
        } steps[] = {{"plain", &plain}, {"loop", &with_loop}};
        for (const auto &s : steps) {
            LOG("[glsl] test %s: compiling\n", s.name);
            const OSTime t0 = OSGetSystemTime();
            GX2VertexShader *v = nullptr;
            info[0] = 0;
            watched = 0;
            busy_since = t0;
            const GX2ShaderMode m = CompileVertexShader(s.glsl->c_str(), GLSL_COMPILE_BLOCK, &v, info, sizeof info, GLSL_COMPILER_FLAG_NONE);
            busy_since = 0;
            LOG("[glsl] test %s: mode %d, %s, %u ms: %.200s\n", s.name, (int)m, v ? "shader made" : "no shader", (u32)OSTicksToMilliseconds(OSGetSystemTime() - t0), info);
            if (v) FreeVertexShader(v);
        }
    }
    for (;;) {
        Job j;
        OSLockMutex(&mutex);
        const bool any = !jobs.empty();
        if (any) {
            j = std::move(jobs.front());
            jobs.pop_front();
        }
        OSUnlockMutex(&mutex);
        if (!any) {
            OSSleepTicks(OSMillisecondsToTicks(20));
            continue;
        }
        const OSTime t0 = OSGetSystemTime();
        CompileDone d{j.vs, j.key, nullptr, nullptr, j.inputs};
        info[0] = 0;
        LOG("[glsl] compiling %s %016llx, %u bytes of GLSL\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, (u32)j.glsl.size());
        watched = 0;
        busy_since = t0;
        const GX2ShaderMode mode = j.vs ? CompileVertexShader(j.glsl.c_str(), GLSL_COMPILE_BLOCK, &d.vs_shader, info, sizeof info, GLSL_COMPILER_FLAG_NONE)
                                        : CompilePixelShader(j.glsl.c_str(), GLSL_COMPILE_BLOCK, &d.ps_shader, info, sizeof info, GLSL_COMPILER_FLAG_NONE);
        busy_since = 0;
        const u32 ms = (u32)(OSTicksToMilliseconds(OSGetSystemTime() - t0));
        if (mode == GLSL_SHADER_MODE_ERROR || (!d.vs_shader && !d.ps_shader)) {
            LOG("[glsl] %s %016llx failed in %u ms: %.400s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, info);
            continue;
        }
        LOG("[glsl] %s %016llx compiled in %u ms%s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, j.cached ? ", from the cache" : "");
        if (!j.cached) cache_append(j);
        OSLockMutex(&mutex);
        done.push_back(d);
        OSUnlockMutex(&mutex);
    }
    return 0;
}
}  // namespace

bool shader_compile_on() { return compiler != nullptr; }

// On unless a file named "noglslcompile" is in the data folder. Its first long run on a console froze (2026-11-06): ContentRedirection's I/O thread sat on
// Aroma's heap lock with this thread as the owner, since this thread's log and cache writes went through that hook, outside fs_begin. Every file call now goes
// straight to IOS (fsa_direct.cpp, on consoles since 2026-11-07); this thread writes only its cache, inside fs_begin, and leaves the log to the card thread.
void shader_compile_init() {
    wiiu::fs_begin(0xC00A);
    const bool off = wiiu::debug_flag("noglslcompile");
    if (!off) {
        OSInitMutex(&mutex);
        cache_load();
    }
    wiiu::fs_end();
    if (off) {
        LOG("[glsl] off: noglslcompile\n");
        return;
    }
    auto *t = (OSThread *)memalign(16, sizeof(OSThread));
    void *stack = memalign(16, STACK);
    // core 0, just below the card thread (20), so a burst of compiles never holds up saves and the log; the game's threads are on core 1 now, so
    // only core 0's own threads (net, watchdog, audio, hid, card), which mostly sleep, come first. At 25 it never ran back when the game shared core 0
    if (!t || !stack || !OSCreateThread(t, compile_thread, 0, nullptr, (u8 *)stack + STACK, STACK, 21, OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
        LOG("[glsl] no compiler thread\n");
        return;
    }
    OSSetThreadName(t, "glsl");
    compiler = t;
    OSResumeThread(t);
}

// (GPU thread) a compile that has run for 8 s, 16 s, 24 s, 32 s: every thread's state on the log, and the log on the file at once, since the
// card thread that normally writes it shares core 0 with the compiler and may be what the compiler keeps from running (or the other way round)
void shader_compile_watch() {
    const OSTime since = busy_since;
    if (!since || watched >= 4 || OSTicksToSeconds(OSGetSystemTime() - since) < 8 * (watched + 1)) return;
    watched = watched + 1;
    LOG("[glsl] the compile in progress has run for %u s: compiler thread ran %u ms, state %x, pc %08x\n", (u32)OSTicksToSeconds(OSGetSystemTime() - since), (u32)(compiler->coreTimeConsumedNs / 1000000),
        (u32)compiler->state, compiler->context.srr0);
    wiiu::log_threads();  // memory only: this is the GPU thread, which never waits for the card
}

void shader_compile_request(bool vs, u64 key, std::string glsl, u32 inputs) {
    OSLockMutex(&mutex);
    if (known.insert(key).second) jobs.push_back(Job{vs, false, key, inputs, std::move(glsl)});
    OSUnlockMutex(&mutex);
}

void shader_compile_discard(CompileDone &d) {
    if (d.vs_shader) FreeVertexShader(d.vs_shader);
    if (d.ps_shader) FreePixelShader(d.ps_shader);
    d.vs_shader = nullptr;
    d.ps_shader = nullptr;
}

bool shader_compile_take(CompileDone &out) {
    if (!compiler) return false;
    OSLockMutex(&mutex);
    const bool any = !done.empty();
    if (any) {
        out = done.back();
        done.pop_back();
    }
    OSUnlockMutex(&mutex);
    return any;
}
}  // namespace gpu
