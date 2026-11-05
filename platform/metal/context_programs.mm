#include <algorithm>
#include <cstring>

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
    default:
      return 1;
  }
}

}  // namespace

uint32_t Context::CreateShader(GLenum type) {
  if (type != gl::kVertexShader && type != gl::kFragmentShader) {
    SetError(gl::kInvalidEnum);
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
  const std::vector<UniformField>& fields = program->compiled.layout.fields;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

void Context::WriteUniform(ProgramRecord* program, const UniformField& field, const void* data,
                           size_t bytes) {
  if (field.texture_slot >= 0) {
    int unit = 0;
    std::memcpy(&unit, data, sizeof(unit));
    if (program->sampler_units[field.texture_slot] != unit) {
      program->sampler_units[field.texture_slot] = unit;
    }
    return;
  }
  bytes = std::min<size_t>(bytes, field.size);
  if (std::memcmp(program->uniforms.data() + field.offset, data, bytes) == 0) return;
  std::memcpy(program->uniforms.data() + field.offset, data, bytes);
  program->uniforms_dirty = true;
}

void Context::UniformInts(int location, int count, const int* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0) return;
  const std::vector<UniformField>& fields = program->compiled.layout.fields;
  if (static_cast<size_t>(location) >= fields.size()) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const UniformField& field = fields[static_cast<size_t>(location)];
  WriteUniform(program, field, values, static_cast<size_t>(count) * sizeof(int));
}

void Context::UniformFloats(int location, int count, const float* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0) return;
  const std::vector<UniformField>& fields = program->compiled.layout.fields;
  if (static_cast<size_t>(location) >= fields.size()) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const UniformField& field = fields[static_cast<size_t>(location)];
  const uint32_t expected = ComponentCount(field.type);
  WriteUniform(program, field, values,
               static_cast<size_t>(std::min<uint32_t>(static_cast<uint32_t>(count), expected)) *
                   sizeof(float));
}

void Context::UniformMatrix4(int location, int count, bool transpose, const float* values) {
  ProgramRecord* program = CurrentProgram();
  if (!program || location < 0 || count < 1) return;
  const std::vector<UniformField>& fields = program->compiled.layout.fields;
  if (static_cast<size_t>(location) >= fields.size()) {
    SetError(gl::kInvalidOperation);
    return;
  }
  const UniformField& field = fields[static_cast<size_t>(location)];
  if (!transpose) {
    WriteUniform(program, field, values, 16 * sizeof(float));
    return;
  }
  float transposed[16];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      transposed[column * 4 + row] = values[row * 4 + column];
    }
  }
  WriteUniform(program, field, transposed, sizeof(transposed));
}

}  // namespace cane::gl_metal
