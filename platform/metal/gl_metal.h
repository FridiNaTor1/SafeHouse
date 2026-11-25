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

// (iOS, display_link.mm) Waits for the display's next frame at fps (a whole fraction of the display's rate, or that
// rate): a CADisplayLink on its own thread, asked for fps (preferredFrameRateRange), wakes the game in step with the
// vblanks. Returns at once when a frame began while the game was busy. False: no display link (macOS; the link paused
// with the app in the background): the caller paces with its timer.
bool WaitForDisplayFrame(int fps);

// (a game with programs translated at build time, cmake/metal_shaders.py: Sly 1) the GL program name of the program
// made of these GLSL files, from the game's generated table (prebuilt_programs.mm, linked only into such a game); 0
// when the table has no such program or its Metal compile failed (logged).
unsigned Program(const char* vertex_file, const char* geometry_file, const char* fragment_file);

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_GL_METAL_H_
