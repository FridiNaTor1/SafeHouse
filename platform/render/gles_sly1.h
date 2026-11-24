#pragma once
// Sly 1's OpenGL renderer on an OpenGL ES 3.1 / 3.2 context: Android phones and headsets without (working) Vulkan, and on
// Linux a test mode (SLY1_RENDERER=gles: an ES context from the desktop driver, the same code path as on Android).
//
// The game's renderer is written for desktop OpenGL 4.3 (Sly1/Sly1-Release/gl.cpp, glstate.h, the GLSL 4.30 programs).
// Everything it needs is in OpenGL ES 3.1 except a few desktop-only entry points; this module maps them after glad has
// loaded (Init), so the game's GL calls stay as they are:
// - glClearDepth / glDepthRange / glGetDoublev: the float forms; glPointSize: kept for glGetFloatv(GL_POINT_SIZE)
//   (ES takes the size from gl_PointSize only: blot.vert's u_pointSize); glDrawBuffer: glDrawBuffers;
//   glGetTexImage (the developer window's texture export): a framebuffer read.
// - base instance (the draw record index, gl.cpp BindDrawIndexAttribute; the particles' instance offset):
//   EXT_base_instance where the driver has it, else the instanced attributes' vertex buffer bindings moved by
//   baseinstance * stride around the draw (ES 3.1 vertex attribute bindings). Base vertex: ES 3.2 / EXT / OES
//   draw_elements_base_vertex, else the per-vertex bindings moved the same way.
// - BC1 / BC3 textures (the installed levels' textures): decoded to RGBA8 on the CPU, as the Vulkan and Metal layers do
//   (the game makes their mips with glGenerateMipmap, which ES refuses for compressed formats).
// - the programs: the GLSL 4.30 sources with an ES header (TranslateShader); Dear ImGui through imgui_gl with a
//   program made here (ImguiProgram).
// The lowest context that runs the game: OpenGL ES 3.1 with 12 vertex-stage storage blocks (glob.vert's lights,
// shadows, bones, poses, cached lighting); gl.cpp checks the limits and reports a device below them.
#include <string>

namespace cane::gles1 {
// Linux: SLY1_RENDERER=gles (or opengles) asks for the ES test mode; Android: always true (no desktop OpenGL there).
bool Wanted();
// Before glfwCreateWindow: the ES context hints (Linux; Android's GLFW layer makes an ES context by itself).
// minor: 2 first, then 1 if the window could not be made.
void ApplyWindowHints(int minor);
// After gladLoadGLLoader: true when the current context is OpenGL ES (and Init then has to run).
bool ContextIsEs();
// The ES entry points under their desktop names, the shims above. False with a reason when the context is below ES 3.1
// or an entry point the game needs is missing.
bool Init(std::string& error);
// Init ran and succeeded: the game is on OpenGL ES.
bool Active();
// "OpenGL ES 3.2" etc. (for the renderer's name in logs and the debug window)
const char* Name();
// A GLSL 4.30 core source as GLSL ES 3.10 / 3.20 (the context's version).
std::string TranslateShader(const std::string& source);
// Dear ImGui's program (platform/vulkan/shaders/imgui.vert / .frag's GLSL as ES), 0 when it does not build.
unsigned ImguiProgram();
// glBufferStorage for persistently mapped streams (EXT_buffer_storage), nullptr without it.
void* BufferStorageProc();
}  // namespace cane::gles1
