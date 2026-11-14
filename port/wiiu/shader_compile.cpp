// Shaders the shipped set lacks are compiled on the console itself, on a thread of their own, with CafeGLSL's library for the Wii U
// (third_party/cafeglsl_ppc: version 1.0, where the PC build runs 0.2, and its programs differ: latte_program.h). A miss draws with its
// fallback meanwhile and takes the real shader once the GPU thread installs it (gfx_pump). The GLSL of every miss goes to the save folder,
// so the next start compiles it again in the background before the game gets there. The first of the three things 3Dii U needs: no more
// builds for new areas (2026-11-06).
#include <cafeglsl/CafeGLSLCompiler.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>

#include <cctype>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "latte_program.h"
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
    u32 tries = 0;  // compiles that came back hollow (compile_thread)
};
OSMutex mutex;
std::deque<Job> jobs;
std::vector<CompileDone> done;
std::unordered_set<u64> known;  // asked for once: the cache and a live miss may name the same shader
constexpr u32 STACK = 4 << 20;  // the compiler recurses
OSThread *compiler;
volatile OSTime busy_since;  // when the compile in progress began, 0 between compiles
volatile u32 watched;        // reports written about the compile in progress

// How many of a generated vertex shader's inputs (v0 to v15) its code reads: named again after being declared
u32 inputs_read(const std::string &glsl) {
    u32 n = 0;
    for (u32 r = 0; r < 16; r++) {
        const std::string name = "v" + std::to_string(r);
        u32 seen = 0;
        for (size_t at = glsl.find(name); at != std::string::npos && seen < 2; at = glsl.find(name, at + 1)) {
            const bool starts = at == 0 || !(isalnum((unsigned char)glsl[at - 1]) || glsl[at - 1] == '_');
            const bool ends = at + name.size() >= glsl.size() || !(isalnum((unsigned char)glsl[at + name.size()]) || glsl[at + name.size()] == '_');
            seen += starts && ends;
        }
        n += seen >= 2;
    }
    return n;
}

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

// The file starts with these two words. Its GLSL is what the shader generator of the build that wrote it made of each state, so a build whose
// generator writes a state differently (kShaderGenRevision, shadergen.h) starts the file over: compiled from the old text, a shader would be
// the old generator's, mistakes and all, under the new build's key.
const u32 kCacheHead[2] = {0x53484443, kShaderGenRevision};  // 'SHDC'

// u8 vs, u8 pad[3], u32 inputs, u64 key, u32 length, the GLSL: this console's own bytes, nobody else reads it
void cache_append(const Job &j) {
    wiiu::fs_begin(0xC00B);  // its turn with every other file call
    if (FILE *f = fopen(cache_path().c_str(), "ab")) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) fwrite(kCacheHead, 1, sizeof kCacheHead, f);
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
    if (fread(head, 1, sizeof kCacheHead, f) != sizeof kCacheHead || memcmp(head, kCacheHead, sizeof kCacheHead)) {
        fclose(f);
        if ((f = fopen(cache_path().c_str(), "wb"))) fclose(f);  // another generator's, or from before the file said whose: emptied
        LOG("[glsl] the shader cache was another build's generator's: started over\n");
        return;
    }
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
                                      "void main() { vec4 p = aPos; for (int i = 0; i <= uInt[0].x; i++) p += f[i]; gl_Position = p; }\n";
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
        const auto failed = [&](const char *why) { LOG("[glsl] %s %016llx failed in %u ms: %.400s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, why); };
        if (mode == GLSL_SHADER_MODE_ERROR || (!d.vs_shader && !d.ps_shader)) {
            failed(info);
            continue;
        }
        LOG("[glsl] %s %016llx compiled in %u ms%s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, j.cached ? ", from the cache" : "");
        // A vertex shader that reads inputs and comes back with none, or as a 16-byte program, is hollow: the compiler hands those out for
        // what it's given in its first second or so after starting, and the same source compiles properly afterwards (seen in Cemu,
        // 2026-11-10). One is never installed. It's tried again for a couple of seconds, and then given up, so that a later miss can ask
        // afresh; the CPU shades those draws meanwhile. Fewer inputs than the code reads is no sign of it: the compiler rightly drops an
        // input that reaches no output, and the same shader comes back that way every time.
        if (j.vs && (d.vs_shader->size <= 16 || (inputs_read(j.glsl) && !d.vs_shader->attribVarCount))) {
            FreeVertexShader(d.vs_shader);
            const bool again = ++j.tries < 8;
            LOG("[glsl] vertex %016llx came back hollow: %s\n", (unsigned long long)j.key, again ? "trying again" : "given up");
            if (again) OSSleepTicks(OSMillisecondsToTicks(250));
            OSLockMutex(&mutex);
            if (again) jobs.push_front(std::move(j));
            else known.erase(j.key);
            OSUnlockMutex(&mutex);
            continue;
        }
        // what this compiler writes that a real GPU may not take, put right before anything draws with the program (latte_program.h)
        u32 &resources = j.vs ? d.vs_shader->regs.sq_pgm_resources_vs : d.ps_shader->regs.sq_pgm_resources_ps;
        u32 registers = resources & 0xFF;
        if (!fit_program(j.vs ? d.vs_shader->program : d.ps_shader->program, j.vs ? d.vs_shader->size : d.ps_shader->size, registers, j.vs ? kVertexRegisters : kPixelRegisters)) {
            failed("its program isn't laid out as this GPU's are, or has no register to spare");
            shader_compile_discard(d);
            continue;
        }
        resources = (resources & ~0xFFu) | registers;
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
    // Core 0, just below the card thread (20): a burst of compiles never holds up saves and the log, and only core 0's own threads (net,
    // watchdog, hid, card), which mostly sleep, come first. Not core 2 with the GPU thread: when shaders are missing that thread is at its
    // busiest, shading on the CPU what the missing shaders would, and the compiler below it got no time at all. One compile took 69 s there
    // against under 1 s here, at 7 fps against 57 (NSMB2 with every vertex shader from this compiler, in Cemu, 2026-11-11).
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

// A draw is waiting for this one, so it goes ahead of everything the cache queued at the start, and a shader the cache queued that a draw now
// asks for moves up to the front: after a start with a few hundred cached shaders a miss used to wait its turn behind all of them, its draws
// going through the fallback for as many seconds.
void shader_compile_request(bool vs, u64 key, std::string glsl, u32 inputs) {
    OSLockMutex(&mutex);
    if (known.insert(key).second) jobs.push_front(Job{vs, false, key, inputs, std::move(glsl)});
    else
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it->key == key && it->vs == vs) {
                Job j = std::move(*it);
                jobs.erase(it);
                jobs.push_front(std::move(j));
                break;
            }
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
