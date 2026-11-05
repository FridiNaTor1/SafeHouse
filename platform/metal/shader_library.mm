#include "shader_library.h"

#include <algorithm>
#include <initializer_list>
#include <sstream>

#include "binding_slots.h"
#include "metal_shaders.h"

namespace cane::gl_metal {

namespace {

std::string Joined(std::vector<std::string> names) {
  std::sort(names.begin(), names.end());
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out.push_back(',');
    out += names[i];
  }
  return out;
}

std::string Signature(std::initializer_list<const char*> attributes,
                      std::initializer_list<const char*> uniforms,
                      std::initializer_list<const char*> blocks) {
  return "a:" + Joined({attributes.begin(), attributes.end()}) +
         ";u:" + Joined({uniforms.begin(), uniforms.end()}) +
         ";b:" + Joined({blocks.begin(), blocks.end()});
}

struct ProgramTemplate {
  const char* name;
  const char* file;
  std::string vertex_signature;
  std::string fragment_signature;
};

const std::vector<ProgramTemplate>& Catalog() {
  static const std::vector<ProgramTemplate> kCatalog = {
      {"glob", "glob.metal",
       Signature({"aPos", "aUv", "aRgbaA", "aRgbaB", "aNormal", "aBone", "aDraw", "aWeight"},
                 {"matWorldToClip", "icolor", "rgbaConst", "posCamera", "dirCamera"},
                 {"RECS", "BONES", "WARPS", "WARPMATS", "DRAWS"}),
       Signature({},
                 {"tex", "fTextured", "ate", "atst", "aref", "afailMode", "fogColor", "fFogBlack",
                  "fDate", "texDate"},
                 {})},
      {"celborder", "celborder.metal",
       Signature({},
                 {"matWorldToClip", "matModel", "halfWidthNdc", "edgeBase", "cbone", "iboneFirst"},
                 {"EDGEBUFFER", "EDGESKINBUFFER", "BONES"}),
       Signature({}, {"celRgba"}, {})},
      {"hud", "hud.metal", Signature({"aPos", "aRgba", "aZ", "aUv"}, {"uCanvas"}, {}),
       Signature({}, {"tex", "fTextured", "fAlphaNonZero"}, {})},
      {"overlay", "overlay.metal", Signature({"aPos", "aStq"}, {"uCanvas"}, {}),
       Signature({}, {"tex"}, {})},
      {"particle", "particle.metal", Signature({"aClip", "aUv", "aLayer", "aRgba"}, {}, {}),
       Signature({}, {"tex", "fTextured", "afailMode"}, {})},
      {"screen", "screen.metal", Signature({}, {}, {}), Signature({}, {"tex"}, {})},
      {"shadowcombine", "shadowcombine.metal", Signature({}, {}, {}),
       Signature({}, {"texAlpha", "gain", "m8", "fReset"}, {})},
      {"shadowvol", "shadowvol.metal", Signature({"aPos"}, {"matWorldToClip"}, {}),
       Signature({}, {"rgba", "fDate", "texDate"}, {})},
  };
  return kCatalog;
}

const ProgramTemplate* FindTemplate(const std::string& vertex_signature,
                                    const std::string& fragment_signature) {
  for (const ProgramTemplate& entry : Catalog()) {
    if (entry.vertex_signature == vertex_signature &&
        entry.fragment_signature == fragment_signature) {
      return &entry;
    }
  }
  return nullptr;
}

uint32_t FieldSize(GlslType type) {
  switch (type) {
    case GlslType::kFloat:
    case GlslType::kInt:
    case GlslType::kUint:
    case GlslType::kBool:
      return 4;
    case GlslType::kVec2:
    case GlslType::kIvec2:
    case GlslType::kUvec2:
      return 8;
    case GlslType::kMat4:
      return 64;
    default:
      return 16;
  }
}

uint32_t FieldAlignment(GlslType type) {
  const uint32_t size = FieldSize(type);
  return size >= 16 ? 16 : size;
}

std::string GenerateUniformStruct(const UniformLayout& layout) {
  std::ostringstream out;
  out << "struct Uniforms {\n  float4 cane_clip;\n";
  for (const UniformField& field : layout.fields) {
    if (field.texture_slot >= 0) {
      out << "#define CANE_TEXTURE_SLOT_" << field.name << " " << field.texture_slot << "\n";
    } else {
      out << "  " << MslTypeName(field.type) << " " << field.name << ";\n";
    }
  }
  out << "};\n";
  return out.str();
}

}  // namespace

const UniformField* UniformLayout::Find(const std::string& name) const {
  for (const UniformField& field : fields) {
    if (field.name == name) return &field;
  }
  return nullptr;
}

UniformLayout BuildUniformLayout(const GlslInterface& merged) {
  UniformLayout layout;
  uint32_t cursor = 16;
  for (const GlslUniform& uniform : merged.uniforms) {
    UniformField field;
    field.name = uniform.name;
    field.type = uniform.type;
    if (IsSamplerType(uniform.type)) {
      field.texture_slot = layout.texture_slot_count++;
    } else {
      const uint32_t alignment = FieldAlignment(uniform.type);
      cursor = (cursor + alignment - 1) / alignment * alignment;
      field.offset = cursor;
      field.size = FieldSize(uniform.type);
      cursor += field.size;
    }
    layout.fields.push_back(field);
  }
  layout.total_bytes = (cursor + 15) / 16 * 16;
  return layout;
}

std::string StageSignature(const GlslInterface& stage) {
  std::vector<std::string> attributes;
  std::vector<std::string> uniforms;
  std::vector<std::string> blocks;
  for (const GlslAttribute& attribute : stage.attributes) attributes.push_back(attribute.name);
  for (const GlslUniform& uniform : stage.uniforms) uniforms.push_back(uniform.name);
  for (const GlslStorageBlock& block : stage.storage_blocks) blocks.push_back(block.name);
  return "a:" + Joined(attributes) + ";u:" + Joined(uniforms) + ";b:" + Joined(blocks);
}

ShaderLibrary::ShaderLibrary(id<MTLDevice> device) : device_(device) {}

std::string ShaderLibrary::Prefix() const {
  std::ostringstream out;
  out << "#define CANE_MTL_UNIFORM_BUFFER_SLOT " << kUniformBufferSlot << "\n"
      << "#define CANE_MTL_STORAGE_BUFFER_BASE " << kStorageBufferBase << "\n"
      << CaneMetalShader("prelude.metal") << "\n";
  return out.str();
}

id<MTLLibrary> ShaderLibrary::CompileSource(const std::string& source, std::string* log) {
  MTLCompileOptions* options = [MTLCompileOptions new];
  // mathMode is only declared by the macOS 15 SDK (Xcode 16); older SDKs build the fallback alone.
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15.0, *)) {
    options.mathMode = MTLMathModeSafe;
  } else
#endif
  {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
  }
  NSError* error = nil;
  id<MTLLibrary> library =
      [device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                            options:options
                              error:&error];
  if (!library && log) {
    *log = error ? error.localizedDescription.UTF8String : "unknown Metal compile error";
  }
  return library;
}

bool ShaderLibrary::Link(const GlslInterface& vertex_stage, const GlslInterface& fragment_stage,
                         CompiledProgram* out, std::string* log) {
  const std::string vertex_signature = StageSignature(vertex_stage);
  const std::string fragment_signature = StageSignature(fragment_stage);
  const ProgramTemplate* entry = FindTemplate(vertex_signature, fragment_signature);
  if (!entry) {
    if (log) {
      *log = "no Metal port for this GLSL program\n  vertex: " + vertex_signature +
             "\n  fragment: " + fragment_signature;
    }
    return false;
  }
  GlslInterface merged = vertex_stage;
  merged.Merge(fragment_stage);
  out->name = entry->name;
  out->layout = BuildUniformLayout(merged);
  out->storage_bindings.clear();
  for (const GlslStorageBlock& block : merged.storage_blocks) {
    if (block.binding >= 0) out->storage_bindings.push_back(block.binding);
  }
  const std::string source =
      Prefix() + GenerateUniformStruct(out->layout) + CaneMetalShader(entry->file);
  id<MTLLibrary> library = nil;
  const auto cached = libraries_.find(source);
  if (cached != libraries_.end()) {
    library = cached->second;
  } else {
    library = CompileSource(source, log);
    if (!library) return false;
    libraries_[source] = library;
  }
  out->vertex = [library newFunctionWithName:@"vertex_main"];
  out->fragment = [library newFunctionWithName:@"fragment_main"];
  if (!out->vertex || !out->fragment) {
    if (log) *log = std::string("missing vertex_main/fragment_main in ") + entry->file;
    return false;
  }
  return true;
}

bool ShaderLibrary::CompileUtilities(UtilityFunctions* out, std::string* log) {
  id<MTLLibrary> library = CompileSource(Prefix() + CaneMetalShader("utility.metal"), log);
  if (!library) return false;
  out->clear_vertex = [library newFunctionWithName:@"clear_vertex"];
  out->clear_fragment = [library newFunctionWithName:@"clear_fragment"];
  out->blit_vertex = [library newFunctionWithName:@"blit_vertex"];
  out->blit_fragment = [library newFunctionWithName:@"blit_fragment"];
  return out->clear_vertex && out->clear_fragment && out->blit_vertex && out->blit_fragment;
}

}  // namespace cane::gl_metal
