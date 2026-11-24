#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "context.h"

namespace cane::gl_metal {

namespace {

void CopyLog(const std::string& log, int max_length, int* length, char* out) {
  if (max_length <= 0 || !out) {
    if (length) *length = 0;
    return;
  }
  const int count =
      static_cast<int>(std::min<size_t>(log.size(), static_cast<size_t>(max_length - 1)));
  std::memcpy(out, log.data(), static_cast<size_t>(count));
  out[count] = '\0';
  if (length) *length = count;
}

uint32_t ComponentCount(GlslType type) {
  switch (type) {
    case GlslType::kVec2:
    case GlslType::kIvec2:
    case GlslType::kUvec2:
      return 2;
    case GlslType::kVec3:
    case GlslType::kIvec3:
    case GlslType::kUvec3:
      return 3;
    case GlslType::kVec4:
    case GlslType::kIvec4:
    case GlslType::kUvec4:
      return 4;
    case GlslType::kMat4:
      return 16;
    case GlslType::kMat3:
      return 9;
    default:
      return 1;
  }
}

}  // namespace

uint32_t Context::CreateShader(GLenum type) {
  if (type != gl::kVertexShader && type != gl::kFragmentShader) {
    SetError(gl::kInvalidEnum, __func__, type);
    return 0;
  }
  const uint32_t name = shaders_table_.Create();
  shaders_table_.Find(name)->type = type;
  return name;
}

void Context::DeleteShader(uint32_t name) { shaders_table_.Destroy(name); }

void Context::ShaderSource(uint32_t name, int count, const char* const* strings,
                           const int* lengths) {
  ShaderRecord* shader = shaders_table_.Find(name);
  if (!shader) {
    SetError(gl::kInvalidValue);
    return;
  }
  shader->source.clear();
  for (int i = 0; i < count; ++i) {
    if (!strings[i]) continue;
    if (lengths && lengths[i] >= 0) {
      shader->source.append(strings[i], static_cast<size_t>(lengths[i]));
    } else {
      shader->source.append(strings[i]);
    }
  }
}

void Context::CompileShader(uint32_t name) {
  ShaderRecord* shader = shaders_table_.Find(name);
  if (!shader) {
    SetError(gl::kInvalidValue);
    return;
  }
  shader->interface = GlslInterface{};
  shader->log.clear();
  shader->compiled = ParseGlslInterface(shader->source, &shader->interface, &shader->log);
}

bool Context::GetShaderInt(uint32_t name, GLenum pname, int* out) {
  ShaderRecord* shader = shaders_table_.Find(name);
  if (!shader) return false;
  switch (pname) {
    case gl::kCompileStatus:
      *out = shader->compiled ? 1 : 0;
      return true;
    case gl::kInfoLogLength:
      *out = static_cast<int>(shader->log.size() + 1);
      return true;
    case gl::kShaderSourceLength:
      *out = static_cast<int>(shader->source.size() + 1);
      return true;
    case gl::kDeleteStatus:
      *out = 0;
      return true;
    default:
      return false;
  }
}

void Context::GetShaderInfoLog(uint32_t name, int max_length, int* length, char* out) {
  ShaderRecord* shader = shaders_table_.Find(name);
  CopyLog(shader ? shader->log : std::string(), max_length, length, out);
}

uint32_t Context::CreateProgram() { return programs_.Create(); }

void Context::DeleteProgram(uint32_t name) {
  if (!programs_.Find(name)) return;
  pipelines_->ForgetProgram(name);
  programs_.Destroy(name);
  if (state_.program == name) state_.program = 0;
}

void Context::AttachShader(uint32_t program_name, uint32_t shader_name) {
  ProgramRecord* program = programs_.Find(program_name);
  ShaderRecord* shader = shaders_table_.Find(shader_name);
  if (!program || !shader) {
    SetError(gl::kInvalidValue);
    return;
  }
  if (shader->type == gl::kVertexShader) {
    program->vertex_shader = shader_name;
  } else {
    program->fragment_shader = shader_name;
  }
}

void Context::LinkProgram(uint32_t name) {
  ProgramRecord* program = programs_.Find(name);
  if (!program) {
    SetError(gl::kInvalidValue);
    return;
  }
  ShaderRecord* vertex = shaders_table_.Find(program->vertex_shader);
  ShaderRecord* fragment = shaders_table_.Find(program->fragment_shader);
  program->linked = false;
  program->log.clear();
  if (!vertex || !fragment || !vertex->compiled || !fragment->compiled) {
    program->log = "both a compiled vertex and fragment shader are required";
    return;
  }
  CompiledProgram compiled;
  if (!shaders_->Link(vertex->interface, fragment->interface, &compiled, &program->log)) {
    return;
  }
  pipelines_->ForgetProgram(name);
  program->compiled = std::move(compiled);
  program->uniforms.assign(program->compiled.layout.total_bytes, 0);
  program->sampler_units.fill(0);
  program->uniforms_dirty = true;
  program->linked = true;
}

bool Context::GetProgramInt(uint32_t name, GLenum pname, int* out) {
  ProgramRecord* program = programs_.Find(name);
  if (!program) return false;
  switch (pname) {
    case gl::kLinkStatus:
    case gl::kValidateStatus:
      *out = program->linked ? 1 : 0;
      return true;
    case gl::kInfoLogLength:
      *out = static_cast<int>(program->log.size() + 1);
      return true;
    case gl::kActiveUniforms:
      *out = static_cast<int>(program->compiled.layout.fields.size());
      return true;
    case gl::kAttachedShaders:
      *out = (program->vertex_shader ? 1 : 0) + (program->fragment_shader ? 1 : 0);
      return true;
    case gl::kDeleteStatus:
      *out = 0;
      return true;
    default:
      return false;
  }
}

void Context::GetProgramInfoLog(uint32_t name, int max_length, int* length, char* out) {
  ProgramRecord* program = programs_.Find(name);
  CopyLog(program ? program->log : std::string(), max_length, length, out);
}

void Context::UseProgram(uint32_t name) {
  if (name && !programs_.Find(name)) {
    SetError(gl::kInvalidValue);
    return;
  }
  state_.program = name;
}

ProgramRecord* Context::CurrentProgram() {
  ProgramRecord* program = programs_.Find(state_.program);
  if (!program || !program->linked) {
    SetError(gl::kInvalidOperation);
    return nullptr;
  }
  return program;
}

int Context::GetUniformLocation(uint32_t program_name, const char* name) {
  ProgramRecord* program = programs_.Find(program_name);
  if (!program || !program->linked || !name) {
    SetError(gl::kInvalidOperation);
    return -1;
  }
  // (an array's element: "name[i]" is the field's location with the element in the high bits; "name[0]" = "name")
  std::string base = name;
  uint32_t element = 0;
  const size_t bracket = base.find('[');
  if (bracket != std::string::npos && base.back() == ']') {
    element = static_cast<uint32_t>(std::strtoul(base.c_str() + bracket + 1, nullptr, 10));
    base.resize(bracket);
  }
  const std::vector<UniformField>& fields = program->compiled.layout.fields;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name != base) continue;
    if (element >= fields[i].count || element >= 0x8000) return -1;
    return static_cast<int>(i | (element << 16));
  }
  return -1;
}

void Context::WriteUniform(ProgramRecord* program, const UniformField& field, const void* data,
                           size_t bytes) {
  WriteUniformElements(program, field, 0, data, bytes);
}

void Context::WriteUniformElements(ProgramRecord* program, const UniformField& field,
                                   uint32_t element, const void* data, size_t bytes) {
  if (field.texture_slot >= 0) {
    int unit = 0;
    std::memcpy(&unit, data, sizeof(unit));
    if (program->sampler_units[field.texture_slot] != unit) {
      program->sampler_units[field.texture_slot] = unit;
    }
    return;
  }
  const uint8_t* in = static_cast<const uint8_t*>(data);
  // (one element, or an array's elements at their std140 stride: glUniform1fv(poseWeights, n, ...))
  const size_t stride = field.stride ? field.stride : field.size;
  while (bytes > 0 && element < field.count) {
    const size_t chunk = std::min<size_t>(bytes, field.size);
    uint8_t* out = program->uniforms.data() + field.offset + element * stride;
    if (out + chunk > program->uniforms.data() + program->uniforms.size()) break;
    if (std::memcmp(out, in, chunk) != 0) {
      std::memcpy(out, in, chunk);
      program->uniforms_dirty = true;
    }
    in += chunk;
    bytes -= chunk;
    ++element;
  }
}

const UniformField* Context::FieldAt(ProgramRecord* program, int location, uint32_t* element) {
  const std::vector<UniformField>& fields = program->compiled.layout.fields;
  const size_t index = static_cast<size_t>(location) & 0xFFFF;
  if (index >= fields.size()) {
    SetError(gl::kInvalidOperation);
    return nullptr;
  }
  *element = static_cast<uint32_t>(location) >> 16;
  return &fields[index];
}

void Context::UniformInts(int location, int count, const int* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0) return;
  uint32_t element = 0;
  const UniformField* field = FieldAt(program, location, &element);
  if (!field) return;
  WriteUniformElements(program, *field, element, values, static_cast<size_t>(count) * sizeof(int));
}

void Context::UniformFloats(int location, int count, const float* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0) return;
  uint32_t element = 0;
  const UniformField* field = FieldAt(program, location, &element);
  if (!field) return;
  // (count: floats; an array takes as many elements as they fill)
  const uint32_t expected = ComponentCount(field->type) * (field->count - std::min(element, field->count - 1));
  WriteUniformElements(program, *field, element, values,
                       static_cast<size_t>(std::min<uint32_t>(static_cast<uint32_t>(count), expected)) *
                           sizeof(float));
}

void Context::UniformMatrix4(int location, int count, bool transpose, const float* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0 || count < 1) return;
  uint32_t element = 0;
  const UniformField* found = FieldAt(program, location, &element);
  if (!found) return;
  const UniformField& field = *found;
  if (!transpose) {
    WriteUniformElements(program, field, element, values, 16 * sizeof(float));
    return;
  }
  float transposed[16];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      transposed[column * 4 + row] = values[row * 4 + column];
    }
  }
  WriteUniformElements(program, field, element, transposed, sizeof(transposed));
}

void Context::UniformMatrix3(int location, int count, bool transpose, const float* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0 || count < 1) return;
  uint32_t element = 0;
  const UniformField* field = FieldAt(program, location, &element);
  if (!field) return;
  // (std140: three vec4 columns)
  float columns[12] = {};
  for (int column = 0; column < 3; ++column) {
    for (int row = 0; row < 3; ++row) {
      columns[column * 4 + row] = transpose ? values[row * 3 + column] : values[column * 3 + row];
    }
  }
  WriteUniformElements(program, *field, element, columns, sizeof(columns));
}

void Context::GetUniform(uint32_t program_name, int location, void* out) {
  ProgramRecord* program = programs_.Find(program_name);
  if (!program || !program->linked || location < 0) {
    SetError(gl::kInvalidOperation);
    return;
  }
  uint32_t element = 0;
  const UniformField* field = FieldAt(program, location, &element);
  if (!field) return;
  if (field->texture_slot >= 0) {
    std::memcpy(out, &program->sampler_units[field->texture_slot], sizeof(int));
    return;
  }
  const size_t stride = field->stride ? field->stride : field->size;
  std::memcpy(out, program->uniforms.data() + field->offset + element * stride, field->size);
}

uint32_t Context::GetUniformBlockIndex(uint32_t program_name, const char* name) {
  // (prebuilt programs keep their blocks at their declared bindings: the index is reported as the binding, which
  // glUniformBlockBinding then confirms; cmake/metal_shaders.py places binding i at kUniformBlockBase + i)
  ProgramRecord* program = programs_.Find(program_name);
  if (!program || !program->linked || !name) return gl::kInvalidIndex;
  const uint32_t blocks = program->compiled.blocks_vertex | program->compiled.blocks_fragment;
  if (!blocks) return gl::kInvalidIndex;
  const std::string& names = program->compiled.block_names;
  const std::string key = std::string(name) + "=";
  for (size_t at = 0; at < names.size();) {
    const size_t end = std::min(names.find(';', at), names.size());
    if (names.compare(at, key.size(), key) == 0) {
      const uint32_t binding = static_cast<uint32_t>(std::strtoul(names.c_str() + at + key.size(), nullptr, 10));
      return (blocks >> binding) & 1u ? binding : gl::kInvalidIndex;
    }
    at = end + 1;
  }
  return gl::kInvalidIndex;
}

bool Context::IsProgram(uint32_t name) { return name && programs_.Find(name) != nullptr; }

uint32_t Context::LinkPrebuilt(const prebuilt::Program& entry) {
  const uint32_t name = programs_.Create();
  ProgramRecord* program = programs_.Find(name);
  CompiledProgram compiled;
  std::string log;
  if (!shaders_->LinkPrebuilt(entry, &compiled, &log)) {
    NSLog(@"Metal: prebuilt program %s failed: %s", entry.key, log.c_str());
    programs_.Destroy(name);
    return 0;
  }
  program->compiled = std::move(compiled);
  program->uniforms.assign(program->compiled.layout.total_bytes, 0);
  program->sampler_units.fill(0);
  program->uniforms_dirty = true;
  program->linked = true;
  return name;
}

}  // namespace cane::gl_metal
