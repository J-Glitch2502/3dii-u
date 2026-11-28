#include <atomic>
// Shaders the shipped set lacks are compiled on the console itself, on a thread of their own, with CafeGLSL's library for the Wii U
// (third_party/cafeglsl_ppc: version 1.0, where the PC build runs 0.2, and its programs differ: latte_program.h). A miss draws with its
// fallback meanwhile and takes the real shader once the GPU thread installs it (gfx_pump). Validated native programs are restored on later
// starts; the GLSL journal remains the recovery path when a native entry is missing, corrupt, or from another compiler/generator.
#include <cafeglsl/CafeGLSLCompiler.h>
#include <cafeglsl/gfd.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gfd.h>
#include <malloc.h>
#include <sys/stat.h>

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
#include "shader_queue.h"
#include "shadergen.h"
#include "wiiu.h"
#include "physical_file.h"
#include "loader.h"



namespace gpu {
namespace {
struct Job {
    bool vs, cached;
    u64 key;
    u32 inputs;
    std::string glsl;
    bool demanded = false;
    u32 tries = 0;  // compiles that came back hollow (compile_thread)
};
OSMutex mutex;
std::deque<Job> jobs;
std::vector<CompileDone> done;
std::unordered_set<u64> known;  // asked for once: the cache and a live miss may name the same shader
constexpr u32 STACK = 4 << 20;  // the compiler recurses
OSThread *compiler;
volatile OSTime busy_since;  // when the compile in progress began, 0 between compiles
std::atomic<u32> cache_io_since_ms{0};
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
        char name[180];
        snprintf(name, sizeof name, "/vol/external01/wiiu/3dii-u/cache/%016llx/shader_cache_%016llx.bin",
                 (unsigned long long)g_game.program_id, (unsigned long long)gpu::shipped_set_id);
        p = name;
    }
    return p;
}

// The file starts with these two words. Its GLSL is what the shader generator of the build that wrote it made of each state, so a build whose
// generator writes a state differently (kShaderGenRevision, shadergen.h) starts the file over: compiled from the old text, a shader would be
// the old generator's, mistakes and all, under the new build's key.
const u32 kCacheHead[2] = {0x53484443, kShaderGenRevision};  // 'SHDC'

// u8 vs, u8 pad[3], u32 inputs, u64 key, u32 length, the GLSL: this console's own bytes, nobody else reads it
void cache_append(const Job &j) {
    cache_io_since_ms.store((u32)OSTicksToMilliseconds(OSGetSystemTime()) + 1, std::memory_order_release);
    {
        wiiu::PhysicalFile f;
        if (f.open(cache_path(), "a")) {
            const u32 head[5] = {(u32)j.vs << 24, j.inputs, (u32)(j.key >> 32), (u32)j.key, (u32)j.glsl.size()};
            if ((!f.empty() || f.write(kCacheHead, sizeof kCacheHead)) && f.write(head, sizeof head))
                f.write(j.glsl.data(), (unsigned)j.glsl.size());
        }
        // A missing SD/cache disables persistence, never falls back to Aroma's
        // redirected fopen path. The compiled shader remains usable in memory.
    }
    cache_io_since_ms.store(0, std::memory_order_release);
}

void cache_load() {
    wiiu::PhysicalFile f;
    if (!f.open(cache_path(), "r")) return;
    u32 n = 0, head[5];
    if (f.read(head, sizeof kCacheHead) != sizeof kCacheHead || memcmp(head, kCacheHead, sizeof kCacheHead)) {
        f.close();
        if (f.open(cache_path(), "w")) f.close();
        return;
    }
    while (f.read(head, sizeof head) == sizeof head && head[4] < (1 << 20)) {
        Job j{(head[0] >> 24) != 0, true, (u64)head[2] << 32 | head[3], head[1], std::string(head[4], '\0')};
        if (f.read(j.glsl.data(), head[4]) != head[4]) break;
        if (known.insert(j.key).second) { jobs.push_back(std::move(j)); n++; }
    }
    LOG("[glsl] %u shaders from physical SD cache; redirected cache I/O disabled\n", n);
}

int compile_thread(int, void *) {
    static char info[4096];
    // Shipped shader packs may cover every draw. Do not initialize cafeglsl
    // or compile its diagnostic shaders until a real missing shader needs it.
    // Keep the same warm-up and validation before that first compilation.
    bool initialized = false;
    const auto initialize = [&] {
        InitGLSLCompiler();
        LOG("[glsl] compiler %s up\n", GetGLSLCompilerVersion());
        if (g_verbose >= 2) {  // two known small shaders first: whether compiling works here at all, how long one takes, and that a loop (r600's jump tracker) gets through: in
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
    };
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
        if (!initialized) {
            initialize();
            initialized = true;
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
            shader_compile_discard(d);
            continue;
        }
        LOG("[glsl] %s %016llx compiled in %u ms%s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, j.cached ? ", from the cache" : "");
        // A vertex shader that reads inputs and comes back with none, or as a 16-byte program, is hollow: the compiler hands those out for
        // what it's given in its first second or so after starting, and the same source compiles properly afterwards (seen in Cemu,
        // 2026-11-10). One is never installed. It's tried again for a couple of seconds, and then given up, so that a later miss can ask
        // afresh; the CPU shades those draws meanwhile. Fewer inputs than the code reads is no sign of it: the compiler rightly drops an
        // input that reaches no output, and the same shader comes back that way every time. Pixel shaders can instead come back as
        // one fully masked export (AC cold builds); alpha-NEVER draws are already skipped by raster.cpp, before requesting a shader.
        const bool hollow = j.vs ? d.vs_shader->size <= 16 || (inputs_read(j.glsl) && !d.vs_shader->attribVarCount)
                                 : latte::hollow_pixel_program(d.ps_shader->program, d.ps_shader->size);
        if (hollow) {
            shader_compile_discard(d);
            const bool again = ++j.tries < 8;
            LOG("[glsl] %s %016llx came back hollow: %s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, again ? "trying again" : "given up");
            if (again) OSSleepTicks(OSMillisecondsToTicks(250));
            OSLockMutex(&mutex);
            if (again) compile_queue::enqueue(jobs,std::move(j));
            else known.erase(j.key);
            OSUnlockMutex(&mutex);
            continue;
        }
        // what this compiler writes that a real GPU may not take, put right before anything draws with the program (latte_program.h)
        u32 &resources = j.vs ? d.vs_shader->regs.sq_pgm_resources_vs : d.ps_shader->regs.sq_pgm_resources_ps;
        u32 registers = resources & 0xFF;
        if (!fit_program(j.vs ? d.vs_shader->program : d.ps_shader->program, j.vs ? d.vs_shader->size : d.ps_shader->size, registers, shader_budget::kOrdinary)) {
            char why[160];
            snprintf(why, sizeof why, "invalid program/metadata or ordinary-register requirement exceeds %u (declared %u, %u bytes)",
                     shader_budget::kOrdinary, resources & 255, j.vs ? d.vs_shader->size : d.ps_shader->size);
            failed(why);
            shader_compile_discard(d);
            continue;
        }
        resources = (resources & ~0xFFu) | registers;
        LOG("[glsl] %s %016llx requires %u ordinary registers\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, registers);
        if (!j.cached) cache_append(j);
        OSLockMutex(&mutex);
        done.push_back(d);
        OSUnlockMutex(&mutex);
    }
    return 0;
}
}  // namespace

bool shader_cache_io_stalled(OSTime timeout) {
    const u32 start = cache_io_since_ms.load(std::memory_order_acquire);
    return start && (u32)((u32)OSTicksToMilliseconds(OSGetSystemTime()) - (start - 1)) > (u32)OSTicksToMilliseconds(timeout);
}

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
    LOG("[glsl] compiler thread ready\n");
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

// Prioritize demanded vertex work, then demanded pixels, then cache warm-up.
// Repeated demand promotes a cached record without duplicating its compile.
void shader_compile_request(bool vs, u64 key, std::string glsl, u32 inputs) {
    static std::unordered_set<u64> reported_vs;  // at most one request diagnostic per vertex key, even if a caller repeats it
    const char *disposition = "already known (no queued job)";
    OSLockMutex(&mutex);
    const bool report = vs && reported_vs.insert(key).second;
    if (known.insert(key).second) {
        Job j{vs,false,key,inputs,std::move(glsl)};j.demanded=true;
        compile_queue::enqueue(jobs,std::move(j));
        disposition = "queued";
    }
    else
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it->key == key && it->vs == vs) {
                Job j = std::move(*it);
                jobs.erase(it);
                j.demanded=true;
                compile_queue::enqueue(jobs,std::move(j));
                disposition = "promoted";
                break;
            }
    OSUnlockMutex(&mutex);
    if (report) LOG("[glsl] vertex %016llx request: %s\n", (unsigned long long)key, disposition);
}

void shader_compile_discard(CompileDone &d) {
    if (d.vs_shader) { if (d.from_gfd) { free(d.vs_shader->program); free(d.vs_shader); } else FreeVertexShader(d.vs_shader); }
    if (d.ps_shader) { if (d.from_gfd) { free(d.ps_shader->program); free(d.ps_shader); } else FreePixelShader(d.ps_shader); }
    d.vs_shader = nullptr;
    d.ps_shader = nullptr;
    d.from_gfd = false;
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
