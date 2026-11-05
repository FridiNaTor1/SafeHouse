#ifndef CANE_PLATFORM_METAL_GL_METAL_H_
#define CANE_PLATFORM_METAL_GL_METAL_H_

#include <string>

struct GLFWwindow;

namespace cane::gl_metal {

// Attaches a Metal layer to the GLFW window (created with GLFW_NO_API) and
// makes the OpenGL entry points of this backend live. One context per process.
bool CreateContext(GLFWwindow* window, std::string* error);
void DestroyContext();

// The GL function pointer for a name, or nullptr when the subset lacks it.
void* ProcAddress(const char* name);

void SetSwapInterval(int interval);
void SwapBuffers();

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_GL_METAL_H_
