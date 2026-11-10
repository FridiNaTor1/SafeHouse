#ifndef CANE_PLATFORM_GL_BACKEND_H_
#define CANE_PLATFORM_GL_BACKEND_H_
// A renderer that stands in for the platform's OpenGL while it runs (Sly 2's Vulkan renderer, platform/vulkan/gl_vulkan.h:
// the game's GL calls on Vulkan). Set by the game after it started; nullptr: OpenGL as always. gl_context.h's
// CreateContext / DestroyContext / ProcAddress / SwapInterval / SwapBuffers go to it, as does the prompt atlas's loader.
#include <GLFW/glfw3.h>

#include <string>

namespace cane::gl {
struct Backend {
  bool (*createContext)(GLFWwindow* window, std::string* error);
  void (*destroyContext)(GLFWwindow* window);
  GLFWglproc (*procAddress)(const char* name);
  void (*swapInterval)(int interval);
  void (*swapBuffers)(GLFWwindow* window);
};
inline const Backend*& ActiveBackend() {
  static const Backend* backend = nullptr;
  return backend;
}
}  // namespace cane::gl

#endif  // CANE_PLATFORM_GL_BACKEND_H_
