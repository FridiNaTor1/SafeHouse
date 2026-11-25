#pragma once
// A game's window on Vulkan (platform/vulkan/gl_vulkan.h: the game's OpenGL calls run on Vulkan), for a game that has no
// renderer.cpp of its own (Sly 1, from the Sly1/ submodule: its gl.cpp calls this through <safehouse/vulkan/vk_game.h>).
// The rules are platform/renderer_choice.h's, as Sly 2's and Sly 3's renderer.cpp apply them: Auto is Vulkan with
// OpenGL as the fallback, a Vulkan start that did not finish is remembered in <user dir>/vulkan-start.txt so the next
// Auto start uses OpenGL, and a Vulkan error within the first frames restarts the game on OpenGL (Linux desktops).
//
// Builds without the Vulkan renderer (CANE_VULKAN unset: no glslangValidator at build time, macOS, the Switch) get the
// same functions, answering "OpenGL".
#include <GLFW/glfw3.h>

namespace cane::vkgame {

enum class Choice { Auto = 0, Vulkan = 1, OpenGL = 2 };

struct Config {
    const char* version = "";            // the build's version (the start marker: a new version tries Auto's Vulkan again)
    const char* envRenderer = nullptr;   // an environment variable that overrides the choice ("auto", "vulkan", "opengl")
    // the programs every frame draws with (vertex, fragment file names): Vulkan without them cannot run the game
    const char* const (*programs)[2] = nullptr;
    int programCount = 0;
    // Android: the game has an OpenGL ES path (Sly 1: platform/render/gles_sly1.h). Then OpenGL / Auto's fallback is
    // OpenGL ES there as on the desktops; without it Android is Vulkan only and a failed start throws.
    bool glesFallback = false;
};

// "auto", "vulkan" / "vk", "opengl" / "gl" / "gles" / "opengles" (any case; spaces, '-' and '_' ignored)
bool ParseChoice(const char* text, Choice* choice);
// The command line, for a restart on OpenGL after a Vulkan error in the first frames (Linux).
void SetRelaunch(int argc, char** argv);
// The game's window on Vulkan when the choice (or Config::envRenderer) asks for it and Vulkan starts here; nullptr: the
// caller makes its OpenGL window (OpenGL ES on Android with Config::glesFallback; why is logged). On success cane::gl::ActiveBackend() is Vulkan's, so gl_context.h's
// CreateContext / ProcAddress / SwapInterval / SwapBuffers / DestroyContext go to it.
GLFWwindow* OpenWindow(int width, int height, bool visible, const char* title, Choice wanted, const Config& config);
// True while the game renders on Vulkan.
bool Active();
// "Vulkan" or "OpenGL"
const char* Name();
// The device's name on Vulkan ("" on OpenGL).
const char* DeviceName();
// A program compiled at build time (cmake/vulkan_shaders.py); 0 when it does not exist (or not on Vulkan).
unsigned Program(const char* vertexFile, const char* geometryFile, const char* fragmentFile);

}  // namespace cane::vkgame
