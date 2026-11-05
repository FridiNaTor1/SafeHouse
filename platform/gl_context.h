#ifndef CANE_PLATFORM_GL_CONTEXT_H_
#define CANE_PLATFORM_GL_CONTEXT_H_

#include <GLFW/glfw3.h>

#include <string>

#if defined(__APPLE__)
#include "metal/gl_metal.h"
#endif

namespace cane::gl {

// A core-profile context from the platform's own OpenGL (the launcher).
inline void ApplyNativeWindowHints(int major, int minor) {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
}

// The games' context: OpenGL where the platform has a recent one, Metal on macOS.
inline void ApplyWindowHints(int major, int minor) {
#if defined(__APPLE__)
  (void)major;
  (void)minor;
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
  ApplyNativeWindowHints(major, minor);
#endif
}

inline bool CreateContext(GLFWwindow* window, std::string* error) {
#if defined(__APPLE__)
  return gl_metal::CreateContext(window, error);
#else
  (void)error;
  glfwMakeContextCurrent(window);
  return true;
#endif
}

inline void DestroyContext(GLFWwindow* window) {
  (void)window;
#if defined(__APPLE__)
  gl_metal::DestroyContext();
#endif
}

inline GLFWglproc ProcAddress(const char* name) {
#if defined(__APPLE__)
  return reinterpret_cast<GLFWglproc>(gl_metal::ProcAddress(name));
#else
  return glfwGetProcAddress(name);
#endif
}

inline void SwapInterval(int interval) {
#if defined(__APPLE__)
  gl_metal::SetSwapInterval(interval);
#else
  glfwSwapInterval(interval);
#endif
}

inline void SwapBuffers(GLFWwindow* window) {
#if defined(__APPLE__)
  (void)window;
  gl_metal::SwapBuffers();
#else
  glfwSwapBuffers(window);
#endif
}

}  // namespace cane::gl

#endif  // CANE_PLATFORM_GL_CONTEXT_H_
