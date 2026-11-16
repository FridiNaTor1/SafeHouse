#pragma once
// The SPIR-V programs of Sly 2's Vulkan renderer: cmake/vulkan_shaders.py compiles the game's GLSL at build time and
// writes vk_shaders.cpp, which defines `programs`; the GL layer (gl_vulkan.cpp) looks them up by their files and
// #defines (CreateProgram) and stands in for the driver's linker with these tables.
#include <cstddef>
#include <cstdint>

namespace CaneVk::Shaders
{
// A loose uniform (in the push-constant block, at its absolute offset) or a sampler (type 32+: binding is its slot;
// offset is where its pair of descriptor indices is pushed).
struct Uniform
{
    const char* name;
    uint16_t offset, size, count, stride;
    uint8_t type, binding, stages;
};
struct Program
{
    const char* key;
    const char* vert;
    const char* geom;
    const char* frag;
    const char* defines;               // "A=1;B=2" (the prefix's #defines, sorted)
    const uint32_t* spirv[3];          // vertex, geometry, fragment
    uint32_t spirvSize[3];             // (bytes)
    const Uniform* uniforms;
    uint32_t uniformCount;
    uint32_t pushSize;                 // the push-constant bytes the program reads (the samplers' pairs, then the constants)
    uint16_t storageMask[3];           // the storage buffer bindings (set 1) each stage reads
    uint8_t samplerCount;
    uint32_t inputMask;                // the vertex inputs' locations
    uint32_t inputKinds;               // 2 bits per location: 0 float, 1 int, 2 uint
    // (pushSize above Vulkan's minimum of 128 bytes) the same program reading its block from a uniform buffer with a
    // dynamic offset (set 2, binding 0) in place of push constants: devices with fewer push-constant bytes
    const uint32_t* spirvUbo[3];
    uint32_t spirvUboSize[3];
    // (VR multiview) the stages whose result depends on the eye, made for a two-view pass (CANE_MULTIVIEW: gl_ViewIndex
    // picks the eye's values from the storage block CaneViews, set 1 binding 11); nullptr: the stage above serves both
    // views. Their storage bindings (with CaneViews').
    const uint32_t* spirvMv[3];
    uint32_t spirvMvSize[3];
    uint16_t storageMaskMv[3];
    // (per-state specialization, cmake/vulkan_shaders.py SPEC) bit i: the program reads SPEC uniform i through
    // specialization constant i + 1 where bit i of constant 0 is set
    uint32_t specMask;
};
extern const Program programs[];
extern const size_t programCount;
}  // namespace CaneVk::Shaders
