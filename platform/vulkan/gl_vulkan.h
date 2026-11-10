#pragma once
// Sly 2's OpenGL on Vulkan (Linux, Windows). The game renders through the OpenGL 4.3 subset it uses (render.cpp,
// hud.cpp, emitter.cpp, overlay.cpp, moviegl.cpp, rtgt.cpp, texmod.cpp, gl.cpp); with the Vulkan renderer chosen, the GL
// entry points glad loads are this layer's (ProcAddress), which records Vulkan command buffers directly: the same
// split as the Switch's deko3d layer (platform/switch/deko/gl_deko.cpp), so both renderers share every line of the
// game's scene and draw-list work and differ only in submission, and the OpenGL renderer stays exactly as it was.
//
// Coordinates: the framebuffer objects' memory is GL's (clip y -1 on row 0, GL's viewport and scissor rectangles, GL's
// gl_FragCoord), so render-to-texture, blits and glReadPixels carry over unchanged. Vulkan decides the front face with
// the opposite sign from GL for the same window coordinates (GL CCW is Vulkan CLOCKWISE). The window (framebuffer 0)
// is an offscreen image in the same convention, copied flipped into the swapchain at the present.
//
// Shaders are compiled to SPIR-V at build time (cmake/vulkan_shaders.py -> vk_shaders.cpp); GLSHADER (Sly2-Release
// gl.cpp) asks for a program with CreateProgram. notes/VULKAN_2026-11-10.md.
#include <glad/glad.h>

#include <string>

struct GLFWwindow;

namespace CaneVk
{
struct Options
{
    bool visible = true;              // false: no surface or swapchain (headless screenshots, tests)
    int swapInterval = 1;
    std::string pipelineCachePath;    // the driver's pipeline cache between runs ("" none)
};
// The instance, device and (visible) swapchain for the window (created with GLFW_CLIENT_API = GLFW_NO_API). False
// (and why) when Vulkan cannot run the game here: the caller falls back to OpenGL.
bool Initialize(GLFWwindow* window, const Options& options, std::string* error);
bool Initialized();
void Shutdown();
// glad's loader: the implemented GL functions by name (nullptr for the others).
void* ProcAddress(const char* name);
// The frame: submits the commands and presents the window's picture (glfwSwapBuffers).
void Present();
// 0: no vsync, 1: every vblank (2: as 1).
void SetSwapInterval(int interval);
// A program compiled at build time: the GLSL files and the prefix gl.cpp inserts after #version; 0 when that variant
// does not exist here (the caller falls back as after a link failure).
GLuint CreateProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix = "");
// The device, for the log and the debug menu ("AMD Radeon RX 9070 XT (RADV GFX1201)").
const char* DeviceName();
// (tests) render-pass instances begun and transfer batches recorded inside the frame since the last call
struct Stats { unsigned renderPasses = 0, splits = 0, pipelines = 0, pipelinesCreated = 0, draws = 0, submits = 0, pushes = 0, pushBytes = 0, descriptorPushes = 0; };
Stats TakeStats();
}  // namespace CaneVk
