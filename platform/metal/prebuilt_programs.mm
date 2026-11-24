// cane::gl_metal::Program (gl_metal.h): the GL program name of a program translated to MSL at build time
// (cmake/metal_shaders.py writes the table, prebuilt_program.h). Built into the games that have such a table
// (cmake/sly1.cmake: Sly 1), not into cane_gl_metal: Sly 2 and Sly 3 link no table.
#include <cstring>
#include <string>
#include <unordered_map>

#include "context.h"
#include "gl_metal.h"
#include "prebuilt_program.h"

namespace cane::gl_metal {

unsigned Program(const char* vertex_file, const char* geometry_file, const char* fragment_file) {
  Context* context = CurrentContext();
  if (!context || !vertex_file || !fragment_file) return 0;
  if (geometry_file) {
    NSLog(@"Metal: %s + %s asks for a geometry stage (%s), which Metal does not have", vertex_file,
          fragment_file, geometry_file);
    return 0;
  }
  // (one GL program per entry: the game asks again after a renderer restart only with a new context)
  static Context* s_context = nullptr;
  static std::unordered_map<std::string, unsigned> s_names;
  if (s_context != context) {
    s_names.clear();
    s_context = context;
  }
  const std::string key = std::string(vertex_file) + "+" + fragment_file;
  if (const auto found = s_names.find(key); found != s_names.end() && context->IsProgram(found->second)) {
    return found->second;
  }
  for (size_t i = 0; i < prebuilt::programCount; ++i) {
    const prebuilt::Program& entry = prebuilt::programs[i];
    if (std::strcmp(entry.vertex_file, vertex_file) != 0 || std::strcmp(entry.fragment_file, fragment_file) != 0) {
      continue;
    }
    const unsigned name = context->LinkPrebuilt(entry);
    if (name) s_names[key] = name;
    return name;
  }
  NSLog(@"Metal: no prebuilt program for %s + %s", vertex_file, fragment_file);
  return 0;
}

}  // namespace cane::gl_metal
