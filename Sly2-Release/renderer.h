#pragma once
// Which renderer draws Sly 2: OpenGL (gl.cpp, as always) or Vulkan (platform/vulkan/gl_vulkan.h: the same GL calls run
// on Vulkan; Linux and Windows builds with CANE_VULKAN). Chosen by Settings.ini ([Graphics] Renderer), --renderer or
// SLY2N_RENDERER (auto / vulkan / opengl); Vulkan falls back to OpenGL, with the reason in the log, when it cannot
// start. VR runs on OpenGL. notes/VULKAN_2026-11-10.md.
#include <string>

struct GLFWwindow;

enum class RENDERER { Auto = 0, Vulkan = 1, OpenGL = 2 };
// The user's choice (main.cpp: the settings, then the command line), read when the window is made.
extern RENDERER g_rendererWanted;
// Settings.ini's choice (written back with the settings; g_rendererWanted may be the command line's for this run)
extern RENDERER g_rendererSetting;
bool FParseRenderer(const char* sz, RENDERER* prenderer);
// True while the game renders on Vulkan.
bool FRendererVulkan();
// "Vulkan" or "OpenGL" (the log, the debug menu).
const char* SzRendererName();
// (gl.cpp GL::FInit) the game's window on Vulkan when Vulkan is chosen and starts; nullptr: OpenGL (why is logged).
GLFWwindow* PwindowVulkan(int width, int height, bool fVisible, const char* title);
// (gl.cpp GLSHADER) a program compiled at build time for Vulkan; 0 when that variant does not exist.
unsigned IdVulkanProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix);
