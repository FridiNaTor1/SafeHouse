#pragma once
// The Switch build's own screens (the first-boot installer, the launcher): the window, Dear ImGui through the standard
// GLFW and OpenGL 3 backends (deko/imgui_impl_deko.cpp), the console's system font, textures from PNG / JPEG bytes and
// the controllers' buttons with D-pad repeat. The layout is 1280x720 at any output size (docked 1920x1080 scales it;
// the font atlas is made at 1.5x so docked text stays sharp). The window outlives the screen: the game takes it over.
#include "deko/gl_deko.h"

#include <imgui/imgui.h>
#include <switch.h>

#include <chrono>
#include <cstddef>

struct GLFWwindow;

namespace CaneSwitch
{
class Ui
{
public:
    Ui();
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;
    bool Ok() const { return m_window != nullptr; }

    // A frame: the input (false: the program must end, from the HOME menu's close), then
    // ImGui::NewFrame at 1280x720. The caller draws, then End presents.
    bool Begin();
    void End();

    // The buttons (HidNpadButton_*) pressed this frame, the directions repeating while held.
    u64 Pressed() const { return m_pressed; }

    // A texture from PNG or JPEG bytes (0 when they do not decode).
    static GLuint Texture(const unsigned char* bytes, size_t size);
    static ImTextureID Id(GLuint texture) { return reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(texture)); }

private:
    GLFWwindow* m_window = nullptr;
    PadState m_pad{};
    bool m_sharedFont = false;
    u64 m_pressed = 0, m_repeatDirection = 0;
    std::chrono::steady_clock::time_point m_repeatAt{};
};
}  // namespace CaneSwitch
