#pragma once
// The precompiled programs of the shared GL layer (the launcher, the installers, Sly 2): cmake/switch_shaders.py compiles
// the games' GLSL offline with uam and writes deko_shaders.cpp, which defines `programs`, every game's in one table, each
// tagged with its game (games share file names such as glob.vert); the GL layer (gl_deko.cpp) looks them up by their
// game, files and #defines (CreateProgram, SetShaderSet) and stands in for the driver's linker with these tables.
#include <cstddef>
#include <cstdint>

namespace CaneDeko::Shaders
{
// A loose uniform (in the constant block at ConstantBinding) or a sampler (type 32+: binding is its texture unit slot).
struct Uniform
{
    const char* name;
    uint16_t offset, size, count, stride;
    uint8_t type, binding, stages;
};
// A uniform block the GLSL declares at an explicit binding (glGetUniformBlockIndex answers with it).
struct Block
{
    const char* name;
    uint8_t binding;
};
struct Program
{
    const char* game;                  // "sly2"; "" for every game's (the ImGui renderer's)
    const char* key;
    const char* vert;
    const char* geom;
    const char* frag;
    const char* defines;               // "A=1;B=2" (the prefix's #defines, sorted)
    const uint8_t* dksh[3];            // vertex, geometry, fragment
    uint32_t dkshSize[3];
    const Uniform* uniforms;
    uint32_t uniformCount;
    uint32_t constantSize;
    uint16_t storageMask[3];           // the storage buffer bindings each stage reads
    uint16_t uniformMask[3];           // the uniform block bindings each stage reads (not the constants)
    uint8_t constantStages;            // the stages that read the constants
    uint8_t samplerCount;
    const Block* blocks;
    uint32_t blockCount;
};
inline constexpr unsigned ConstantBinding = 15;
extern const Program programs[];
extern const size_t programCount;
}  // namespace CaneDeko::Shaders
