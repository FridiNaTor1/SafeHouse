#ifndef CANE_PLATFORM_METAL_SHADER_LIBRARY_H_
#define CANE_PLATFORM_METAL_SHADER_LIBRARY_H_

#import <Metal/Metal.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "glsl_interface.h"
#include "prebuilt_program.h"

namespace cane::gl_metal {

struct UniformField {
  std::string name;
  GlslType type = GlslType::kUnknown;
  uint32_t offset = 0;
  uint32_t size = 0;
  // Sampler uniforms live outside the block: this is their texture slot.
  int texture_slot = -1;
  // (prebuilt programs) array elements and the bytes between them (std140); 1 / size otherwise
  uint32_t count = 1;
  uint32_t stride = 0;
};

// Byte layout of the uniform block handed to the MSL `Uniforms` struct. The
// first 16 bytes are the backend's own `cane_clip`; GL uniforms follow in
// declaration order with Metal's natural alignment.
struct UniformLayout {
  std::vector<UniformField> fields;
  uint32_t total_bytes = 16;
  int texture_slot_count = 0;

  const UniformField* Find(const std::string& name) const;
};

UniformLayout BuildUniformLayout(const GlslInterface& merged);

// Canonical description of a stage's declarations, used to pair the GLSL the
// game compiles with the MSL port that implements it.
std::string StageSignature(const GlslInterface& stage);

struct CompiledProgram {
  std::string name;
  id<MTLFunction> vertex = nil;
  id<MTLFunction> fragment = nil;
  UniformLayout layout;
  std::vector<int> storage_bindings;
  // (prebuilt programs, prebuilt_program.h: the resources each stage reads, the vertex inputs, the length() helpers)
  bool prebuilt = false;
  uint32_t storage_vertex = 0;
  uint32_t storage_fragment = 0;
  uint32_t blocks_vertex = 0;
  uint32_t blocks_fragment = 0;
  std::string block_names;
  uint32_t input_mask = 0;
  uint32_t input_kinds = 0;
  std::vector<prebuilt::Length> lengths;
};

struct UtilityFunctions {
  id<MTLFunction> clear_vertex = nil;
  id<MTLFunction> clear_fragment = nil;
  id<MTLFunction> blit_vertex = nil;
  id<MTLFunction> blit_fragment = nil;
};

class ShaderLibrary {
 public:
  explicit ShaderLibrary(id<MTLDevice> device);

  // Finds the MSL program matching the two stages, generates its uniform block
  // and compiles it. `log` receives the reason on failure.
  bool Link(const GlslInterface& vertex_stage, const GlslInterface& fragment_stage,
            CompiledProgram* out, std::string* log);

  bool CompileUtilities(UtilityFunctions* out, std::string* log);

  // A program translated at build time (cmake/metal_shaders.py): its two stages' MSL, its table.
  bool LinkPrebuilt(const prebuilt::Program& program, CompiledProgram* out, std::string* log);

 private:
  std::string Prefix() const;
  id<MTLLibrary> CompileSource(const std::string& source, std::string* log);

  id<MTLDevice> device_;
  std::unordered_map<std::string, id<MTLLibrary>> libraries_;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_SHADER_LIBRARY_H_
