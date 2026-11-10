#pragma once
// The games' OpenGL on deko3d (Nintendo Switch). Sly2-Release renders through OpenGL 4.3 (glad); on the Switch the
// GL entry points glad loads are this layer's (ProcAddress), which records deko3d command lists directly: the GL
// subset the game uses (render.cpp, hud.cpp, emitter.cpp, overlay.cpp, moviegl.cpp, gl.cpp) and the installer's ImGui.
//
// Conventions (gl_deko.cpp "Coordinates"): the device runs with deko3d's OpenGL conventions, the lower-left origin and
// [0, 1] depth (the game's glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)). Clip -1 lands on memory row 0 as in a GL
// framebuffer object, the front-face test is GL's, and the swapchain is flipped by the display itself, so viewports,
// scissors, blits, glReadPixels rows and winding carry over from GL unchanged, the window (framebuffer 0) included.
//
// Shaders are compiled offline (cmake/switch_shaders.py -> deko_shaders.h); GLSHADER (Sly2-Release gl.cpp) asks for
// a program with CreateProgram.
#include <glad/glad.h>

namespace CaneDeko
{
// The device, queue and swapchain at the window's size (glfwCreateWindow). False when deko3d cannot start.
bool Initialize(unsigned width, unsigned height);
// The swapchain follows the console's mode (1280x720 handheld, 1920x1080 docked).
void ResizeOutput(unsigned width, unsigned height);
void Shutdown();
bool Initialized();
// glad's loader: the implemented GL functions by name (nullptr for the others).
void* ProcAddress(const char* name);
// The frame: submits the command list and queues the swapchain image (glfwSwapBuffers); reports it to SaltyNX.
void Present();
// The render size of the frame presented next, for SaltyNX's overlays (the games' internal resolution); frames without
// one (the installer, the launcher, movies) report the output size.
void SetFrameResolution(unsigned width, unsigned height);
// 0: no vsync (the image is shown at once), 1: every vblank, 2: every other.
void SetSwapInterval(int interval);
// A precompiled program: the GLSL files and the prefix gl.cpp inserts after #version ("#define GEOM 1\n"); 0 when the
// variant was not compiled (the caller falls back, as after a link failure).
GLuint CreateProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix = "");
// Whose programs CreateProgram finds (the running game's key; the shared ones always).
void SetShaderSet(const char* game);
// The GL state back at its defaults (after the installer, before the game).
void ResetState();
}
