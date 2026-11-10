// See switch_ui.h.
#include "switch_ui.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui/imgui_impl_glfw.h>
#include <imgui/imgui_impl_opengl3.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb/stb_image.h>

void CaneSwitchReleaseWindow();   // glfw_switch.cpp

namespace CaneSwitch
{
namespace
{
constexpr float kFontPixels = 24.0f;    // ProjectCane's installer size, at the 1280x720 layout
constexpr float kAtlasScale = 1.5f;     // (the atlas for 1080p, shown at 1 / 1.5)
constexpr u64 kDirections = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
}

Ui::Ui()
{
    m_window = glfwCreateWindow(1280, 720, "SafeHouse", nullptr, nullptr);
    if (!m_window) return;
    glfwSwapInterval(1);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // the console's own typeface (ProjectCane's installer), else ImGui's
    PlFontData font{};
    m_sharedFont = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (m_sharedFont && R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) && font.address)
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        io.Fonts->AddFontFromMemoryTTF(font.address, int(font.size), kFontPixels * kAtlasScale, &config);
    }
    else
    {
        ImFontConfig config;
        config.SizePixels = kFontPixels * kAtlasScale;
        io.Fonts->AddFontDefault(&config);
    }
    io.FontGlobalScale = 1.0f / kAtlasScale;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(m_window, true);   // (the touch screen: the mouse)
    if (!ImGui_ImplOpenGL3_Init())
    {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        CaneSwitchReleaseWindow();
        m_window = nullptr;
        return;
    }
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&m_pad);
}

Ui::~Ui()
{
    if (m_window)
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        CaneSwitchReleaseWindow();
    }
    if (m_sharedFont) plExit();
}

bool Ui::Begin()
{
    glfwPollEvents();
    if (glfwWindowShouldClose(m_window)) return false;
    padUpdate(&m_pad);
    m_pressed = padGetButtonsDown(&m_pad);
    // (held directions repeat: after 300 ms, every 100 ms)
    const u64 direction = padGetButtons(&m_pad) & kDirections;
    const auto now = std::chrono::steady_clock::now();
    if (direction != m_repeatDirection)
    {
        m_repeatDirection = direction;
        m_repeatAt = now + std::chrono::milliseconds(300);
    }
    else if (direction && now >= m_repeatAt)
    {
        m_pressed |= direction;
        m_repeatAt = now + std::chrono::milliseconds(100);
    }

    int width = 1280, height = 720;
    glfwGetFramebufferSize(m_window, &width, &height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.03f, 0.05f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DisplayFramebufferScale = ImVec2(float(width) / 1280.0f, float(height) / 720.0f);
    ImGui::NewFrame();
    return true;
}

void Ui::End()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(m_window);
}

GLuint Ui::Texture(const unsigned char* bytes, size_t size)
{
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, int(size), &w, &h, &n, 4);
    if (!pixels) return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    return texture;
}
}  // namespace CaneSwitch
