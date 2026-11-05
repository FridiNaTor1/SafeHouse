#include "glsl_interface.h"

#include <cctype>
#include <sstream>
#include <unordered_set>

namespace cane::gl_metal {

namespace {

std::string StripComments(const std::string& source) {
  std::string out;
  out.reserve(source.size());
  for (size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      while (i < source.size() && source[i] != '\n') ++i;
      out.push_back('\n');
    } else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      i += 2;
      while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
        if (source[i] == '\n') out.push_back('\n');
        ++i;
      }
      ++i;
    } else {
      out.push_back(source[i]);
    }
  }
  return out;
}

std::string Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

// Resolves #define/#ifdef/#ifndef/#else/#endif, dropping every other
// directive, and returns the surviving non-directive lines.
std::string Preprocess(const std::string& source) {
  std::unordered_set<std::string> defines;
  std::vector<bool> active_stack{true};
  std::string out;
  std::istringstream in(source);
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = Trim(line);
    const bool active = active_stack.back();
    if (!trimmed.empty() && trimmed[0] == '#') {
      std::istringstream directive(trimmed.substr(1));
      std::string word;
      std::string arg;
      directive >> word >> arg;
      if (word == "define") {
        if (active) defines.insert(arg);
      } else if (word == "ifdef") {
        active_stack.push_back(active && defines.count(arg) > 0);
      } else if (word == "ifndef") {
        active_stack.push_back(active && defines.count(arg) == 0);
      } else if (word == "else") {
        if (active_stack.size() > 1) {
          const bool parent = active_stack[active_stack.size() - 2];
          active_stack.back() = parent && !active_stack.back();
        }
      } else if (word == "endif") {
        if (active_stack.size() > 1) active_stack.pop_back();
      }
      continue;
    }
    if (active) {
      out += line;
      out.push_back('\n');
    }
  }
  return out;
}

std::vector<std::string> Tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  auto flush = [&] {
    if (!current.empty()) tokens.push_back(current);
    current.clear();
  };
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
      current.push_back(c);
    } else {
      flush();
      if (!std::isspace(static_cast<unsigned char>(c))) tokens.emplace_back(1, c);
    }
  }
  flush();
  return tokens;
}

// Statement-level split of the shader: braces nest, semicolons end.
std::vector<std::vector<std::string>> TopLevelStatements(const std::vector<std::string>& tokens) {
  std::vector<std::vector<std::string>> statements;
  std::vector<std::string> current;
  int depth = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    current.push_back(token);
    if (token == "{") {
      ++depth;
    } else if (token == "}") {
      --depth;
      // A block followed by ';' is a declaration; a block ending a function
      // body stands alone.
      if (depth == 0 && !(i + 1 < tokens.size() && tokens[i + 1] == ";")) {
        statements.push_back(current);
        current.clear();
      }
    } else if (token == ";" && depth == 0) {
      statements.push_back(current);
      current.clear();
    }
  }
  return statements;
}

int LayoutValue(const std::vector<std::string>& tokens, size_t layout_index, std::string_view key) {
  // tokens[layout_index] == "layout", then "(", a comma-separated list, ")".
  for (size_t i = layout_index + 2; i < tokens.size(); ++i) {
    if (tokens[i] == ")") break;
    if (tokens[i] == key && i + 2 < tokens.size() && tokens[i + 1] == "=") {
      return std::atoi(tokens[i + 2].c_str());
    }
  }
  return -1;
}

size_t SkipLayout(const std::vector<std::string>& tokens, size_t index) {
  if (index >= tokens.size() || tokens[index] != "layout") return index;
  while (index < tokens.size() && tokens[index] != ")") ++index;
  return index + 1;
}

const std::unordered_set<std::string>& Qualifiers() {
  static const std::unordered_set<std::string> kSet = {
      "flat",     "noperspective", "smooth", "centroid", "restrict", "readonly", "writeonly",
      "coherent", "volatile",      "highp",  "mediump",  "lowp",     "const"};
  return kSet;
}

}  // namespace

GlslType ParseGlslType(std::string_view token) {
  if (token == "float") return GlslType::kFloat;
  if (token == "vec2") return GlslType::kVec2;
  if (token == "vec3") return GlslType::kVec3;
  if (token == "vec4") return GlslType::kVec4;
  if (token == "int") return GlslType::kInt;
  if (token == "ivec2") return GlslType::kIvec2;
  if (token == "ivec3") return GlslType::kIvec3;
  if (token == "ivec4") return GlslType::kIvec4;
  if (token == "uint") return GlslType::kUint;
  if (token == "uvec2") return GlslType::kUvec2;
  if (token == "uvec3") return GlslType::kUvec3;
  if (token == "uvec4") return GlslType::kUvec4;
  if (token == "bool") return GlslType::kBool;
  if (token == "mat4") return GlslType::kMat4;
  if (token == "sampler2D") return GlslType::kSampler2D;
  if (token == "sampler2DArray") return GlslType::kSampler2DArray;
  return GlslType::kUnknown;
}

bool IsSamplerType(GlslType type) {
  return type == GlslType::kSampler2D || type == GlslType::kSampler2DArray;
}

const char* MslTypeName(GlslType type) {
  switch (type) {
    case GlslType::kFloat:
      return "float";
    case GlslType::kVec2:
      return "float2";
    case GlslType::kVec3:
      return "float3";
    case GlslType::kVec4:
      return "float4";
    case GlslType::kInt:
    case GlslType::kBool:
      return "int";
    case GlslType::kIvec2:
      return "int2";
    case GlslType::kIvec3:
      return "int3";
    case GlslType::kIvec4:
      return "int4";
    case GlslType::kUint:
      return "uint";
    case GlslType::kUvec2:
      return "uint2";
    case GlslType::kUvec3:
      return "uint3";
    case GlslType::kUvec4:
      return "uint4";
    case GlslType::kMat4:
      return "float4x4";
    default:
      return "";
  }
}

const GlslUniform* GlslInterface::FindUniform(std::string_view name) const {
  for (const GlslUniform& uniform : uniforms) {
    if (uniform.name == name) return &uniform;
  }
  return nullptr;
}

void GlslInterface::Merge(const GlslInterface& other) {
  for (const GlslUniform& uniform : other.uniforms) {
    if (!FindUniform(uniform.name)) uniforms.push_back(uniform);
  }
  for (const GlslAttribute& attribute : other.attributes) {
    bool present = false;
    for (const GlslAttribute& mine : attributes) present |= mine.name == attribute.name;
    if (!present) attributes.push_back(attribute);
  }
  for (const GlslStorageBlock& block : other.storage_blocks) {
    bool present = false;
    for (const GlslStorageBlock& mine : storage_blocks) present |= mine.name == block.name;
    if (!present) storage_blocks.push_back(block);
  }
}

bool ParseGlslInterface(const std::string& source, GlslInterface* out, std::string* error) {
  const std::vector<std::string> tokens = Tokenize(Preprocess(StripComments(source)));
  for (const std::vector<std::string>& statement : TopLevelStatements(tokens)) {
    if (statement.empty()) continue;
    size_t i = 0;
    int location = -1;
    int binding = -1;
    if (statement[i] == "layout") {
      location = LayoutValue(statement, i, "location");
      binding = LayoutValue(statement, i, "binding");
      i = SkipLayout(statement, i);
    }
    while (i < statement.size() && Qualifiers().count(statement[i])) ++i;
    if (i >= statement.size()) continue;
    const std::string& storage = statement[i];
    if (storage == "uniform" && i + 2 < statement.size()) {
      const GlslType type = ParseGlslType(statement[i + 1]);
      if (type == GlslType::kUnknown) {
        if (error) *error = "unsupported uniform type " + statement[i + 1];
        return false;
      }
      out->uniforms.push_back({statement[i + 2], type});
    } else if (storage == "in" && i + 2 < statement.size()) {
      // Vertex inputs carry a location; stage varyings do not.
      if (location >= 0) {
        out->attributes.push_back({location, statement[i + 2], ParseGlslType(statement[i + 1])});
      }
    } else if (storage == "buffer" && i + 1 < statement.size()) {
      out->storage_blocks.push_back({binding, statement[i + 1]});
    }
  }
  return true;
}

}  // namespace cane::gl_metal
