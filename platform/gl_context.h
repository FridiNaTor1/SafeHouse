#ifndef CANE_PLATFORM_GL_CONTEXT_H_
#define CANE_PLATFORM_GL_CONTEXT_H_

#include <GLFW/glfw3.h>

#include <string>

#if defined(__APPLE__)
#include "metal/gl_metal.h"
#endif

#if defined(_WIN32)
// The games' frames on Windows: a DXGI flip-model swap chain (platform/win32/gl_present_dxgi.cpp). Start after the
// context is current, before glad loads; each call returns false (nullptr) when it is not presenting, and the
// OpenGL driver presents instead.
namespace cane::gl::dxgi {
bool Start(GLFWwindow* window);
void Stop();
GLFWglproc ProcAddress(const char* name);
bool SetSwapInterval(int interval);
bool SwapBuffers(GLFWwindow* window);
}  // namespace cane::gl::dxgi
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

#if defined(__linux__)
// After a context could not be created: GLFW's reason and what the system's OpenGL does offer, for the log.
inline std::string DescribeContextFailure() {
  const char* message = nullptr;
  const int code = glfwGetError(&message);
  std::string text = code != GLFW_NO_ERROR && message ? std::string("GLFW: ") + message : std::string("GLFW gave no reason");
  glfwDefaultWindowHints();
  ApplyNativeWindowHints(3, 3);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  if (GLFWwindow* probe = glfwCreateWindow(64, 64, "probe", nullptr, nullptr)) {
    glfwMakeContextCurrent(probe);
    using GetString = const unsigned char* (*)(unsigned int);
    if (auto get = reinterpret_cast<GetString>(glfwGetProcAddress("glGetString"))) {
      const unsigned char* renderer = get(0x1F01), * version = get(0x1F02);
      text += "; offered: ";
      text += renderer ? reinterpret_cast<const char*>(renderer) : "unknown renderer";
      text += " / ";
      text += version ? reinterpret_cast<const char*>(version) : "unknown version";
    }
    glfwMakeContextCurrent(nullptr);
    glfwDestroyWindow(probe);
  } else {
    text += "; an OpenGL 3.3 context failed too";
  }
  return text;
}
#endif

inline bool CreateContext(GLFWwindow* window, std::string* error) {
#if defined(__APPLE__)
  return gl_metal::CreateContext(window, error);
#else
  (void)error;
  glfwMakeContextCurrent(window);
#if defined(_WIN32)
  dxgi::Start(window);
#endif
  return true;
#endif
}

inline void DestroyContext(GLFWwindow* window) {
  (void)window;
#if defined(__APPLE__)
  gl_metal::DestroyContext();
#elif defined(_WIN32)
  dxgi::Stop();
#endif
}

inline GLFWglproc ProcAddress(const char* name) {
#if defined(__APPLE__)
  return reinterpret_cast<GLFWglproc>(gl_metal::ProcAddress(name));
#else
#if defined(_WIN32)
  if (GLFWglproc wrapper = dxgi::ProcAddress(name)) return wrapper;
#endif
  return glfwGetProcAddress(name);
#endif
}

inline void SwapInterval(int interval) {
#if defined(__APPLE__)
  gl_metal::SetSwapInterval(interval);
#else
#if defined(_WIN32)
  if (dxgi::SetSwapInterval(interval)) return;
#endif
  glfwSwapInterval(interval);
#endif
}

inline void SwapBuffers(GLFWwindow* window) {
#if defined(__APPLE__)
  (void)window;
  gl_metal::SwapBuffers();
#else
#if defined(_WIN32)
  if (dxgi::SwapBuffers(window)) return;
#endif
  glfwSwapBuffers(window);
#endif
}

}  // namespace cane::gl

#endif  // CANE_PLATFORM_GL_CONTEXT_H_
