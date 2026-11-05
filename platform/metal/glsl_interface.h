#ifndef CANE_PLATFORM_METAL_GLSL_INTERFACE_H_
#define CANE_PLATFORM_METAL_GLSL_INTERFACE_H_

#include <string>
#include <string_view>
#include <vector>

namespace cane::gl_metal {

enum class GlslType {
  kUnknown,
  kFloat,
  kVec2,
  kVec3,
  kVec4,
  kInt,
  kIvec2,
  kIvec3,
  kIvec4,
  kUint,
  kUvec2,
  kUvec3,
  kUvec4,
  kBool,
  kMat4,
  kSampler2D,
  kSampler2DArray,
};

GlslType ParseGlslType(std::string_view token);
bool IsSamplerType(GlslType type);
const char* MslTypeName(GlslType type);

struct GlslUniform {
  std::string name;
  GlslType type = GlslType::kUnknown;
};

struct GlslAttribute {
  int location = -1;
  std::string name;
  GlslType type = GlslType::kUnknown;
};

struct GlslStorageBlock {
  int binding = -1;
  std::string name;
};

struct GlslInterface {
  std::vector<GlslUniform> uniforms;
  std::vector<GlslAttribute> attributes;
  std::vector<GlslStorageBlock> storage_blocks;

  const GlslUniform* FindUniform(std::string_view name) const;
  // Appends declarations absent from this interface (program link).
  void Merge(const GlslInterface& other);
};

// Reads the top-level declarations of a GLSL shader. Handles the preprocessor
// conditionals the sources use (#define, #ifdef, #ifndef, #else, #endif).
bool ParseGlslInterface(const std::string& source, GlslInterface* out, std::string* error);

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_GLSL_INTERFACE_H_
