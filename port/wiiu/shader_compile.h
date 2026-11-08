// Shaders compiled on the console itself: see shader_compile.cpp
#pragma once
#include <gx2/shaders.h>
#include <string>
#include "rt.h"

namespace gpu {
struct CompileDone {
    bool vs;
    u64 key;
    GX2VertexShader *vs_shader;
    GX2PixelShader *ps_shader;
    u32 inputs;
};
void shader_compile_init();                                                     // (GPU thread, after the shipped set) starts the compiler thread and queues the cache
bool shader_compile_on();                                                       // whether it did: on unless a "noglslcompile" file is in the data folder
void shader_compile_request(bool vs, u64 key, std::string glsl, u32 inputs);  // (GPU thread) a miss: compile this in the background
bool shader_compile_take(CompileDone &out);                                     // (GPU thread) one finished shader, if any
void shader_compile_discard(CompileDone &d);                                    // (GPU thread) frees a finished shader the tables don't want
void shader_compile_watch();                                                    // (GPU thread, every frame) reports a compile that runs for too long
}  // namespace gpu
