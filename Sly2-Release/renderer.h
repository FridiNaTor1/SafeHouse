#pragma once
// Which renderer draws the game: Vulkan (platform/vulkan/gl_vulkan.h: the same GL calls run on Vulkan; Linux and Windows
// builds with CANE_VULKAN) or OpenGL (gl.cpp, as always). Chosen by Settings.ini ([Graphics] Renderer), --renderer or
// SLY2N_RENDERER (auto / vulkan / opengl). Auto is Vulkan with OpenGL as the fallback (platform/renderer_choice.h): when
// Vulkan cannot start the game comes up on OpenGL in the same run, with the reason in the log, and a start that did not
// finish is remembered so the next Auto start uses OpenGL. VR runs on OpenGL. notes/VULKAN_2026-11-10.md,
// notes/VULKAN_DEFAULT_2026-11-10.md.
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
// (game.cpp) a frame shown without the window's swap (VR on Vulkan: the headset's frame): counts toward the start being
// trusted (the start marker, platform/renderer_choice.h), as the swap does
void RendererFrameShown();
// "Vulkan" or "OpenGL" (the log, the debug menu).
const char* SzRendererName();
// (main.cpp) the command line, so that a Vulkan error in the first seconds restarts the game on OpenGL (Linux)
void SetRendererRelaunch(int argc, char** argv);
// (gl.cpp GL::FInit) the game's window on Vulkan when Vulkan is chosen and starts; nullptr: OpenGL (why is logged).
GLFWwindow* PwindowVulkan(int width, int height, bool fVisible, const char* title);
// (render.cpp, the shadow phases) whether the bound draw framebuffer's colour can be read in place on Vulkan
// (CANE_FETCH programs: CaneVk::FramebufferFetchUsable)
bool FVulkanFramebufferFetch();
// (measurement, Vulkan: SLY2N_VKGPUTIME + SLY2N_VKGPUMARKS) GPU timestamps between labelled parts of the frame
bool FGpuMarks();
// (Vulkan) the layer takes glEnable(GL_DEPTH_BOUNDS_TEST_EXT) / glDepthBoundsEXT (render.cpp DrawShadow2)
bool FVulkanDepthBounds();
void GpuMark(const char* label);
// (game.cpp SLY2N_FPSLOG) the Vulkan submission worker's thread CPU time so far, ms (all, recording + submission); false
// without one
bool FVulkanWorkerCpuMs(double* pmsAll, double* pmsRecord);
// True while the game renders on Vulkan on a tile-based GPU (CaneVk::TileGpu: Adreno / Mali / PowerVR / Apple; every
// Android device): the tilers' paths (vertex pulling, the in-place shadow reads, VR's passes without the near-clip stage)
bool FVulkanTileGpu();
// (gl.cpp GLSHADER) a program compiled at build time for Vulkan; 0 when that variant does not exist.
unsigned IdVulkanProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix);
