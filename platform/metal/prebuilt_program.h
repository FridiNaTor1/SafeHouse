#ifndef CANE_PLATFORM_METAL_PREBUILT_PROGRAM_H_
#define CANE_PLATFORM_METAL_PREBUILT_PROGRAM_H_

// Programs translated to Metal Shading Language at build time (cmake/metal_shaders.py: GLSL -> SPIR-V -> MSL), for a
// game whose GLSL the layer's hand-written ports (shaders/*.metal) do not cover: Sly 1. The generated
// metal_programs.cpp defines the table; cane::gl_metal::Program (prebuilt_programs.mm, linked only into the games
// that have such a table) makes a GL program name of an entry, as the Vulkan renderer's cane::vkgame::Program does.
// Plain C++: the game's code includes it (through gl_metal.h).

#include <cstddef>
#include <cstdint>

namespace cane::gl_metal::prebuilt {

// A uniform the game sets by name: a loose uniform, a struct uniform's member ("swp.fogType"), a length() helper
// (caneLen_<array>), or a sampler (texture_slot >= 0: glUniform1i sets its unit).
struct Field {
  const char* name;
  uint8_t type;      // GlslType (glsl_interface.h; 17 = mat3, std140: three vec4 columns)
  uint32_t offset;   // in the uniform block (after the layer's cane_clip)
  uint32_t size;     // of one element
  uint32_t count;    // array elements
  uint32_t stride;   // between elements (std140)
  int texture_slot;
};

// A runtime array's length(): the uniform at `offset` gets (bound range size - start) / stride of storage binding
// `binding`.
struct Length {
  uint32_t binding;
  uint32_t offset;
  uint32_t start;
  uint32_t stride;
};

struct Program {
  const char* key;
  const char* vertex_file;
  const char* fragment_file;
  const char* vertex_msl;     // entry point vertex_main
  const char* fragment_msl;   // entry point fragment_main
  const Field* fields;
  uint32_t field_count;
  uint32_t uniform_bytes;
  uint32_t storage_vertex;    // GL shader-storage bindings each stage reads (bit N: binding N)
  uint32_t storage_fragment;
  uint32_t blocks_vertex;     // GL uniform-buffer bindings
  uint32_t blocks_fragment;
  const char* block_names;    // "CMGL=0;BlipGroup=1": the uniform blocks' names and GL bindings
  uint32_t input_mask;        // vertex inputs (bit = location)
  uint32_t input_kinds;       // 2 bits a location: 0 float, 1 int, 2 uint
  const Length* lengths;
  uint32_t length_count;
  uint32_t texture_slots;
};

extern const Program programs[];
extern const size_t programCount;

}  // namespace cane::gl_metal::prebuilt

#endif  // CANE_PLATFORM_METAL_PREBUILT_PROGRAM_H_
