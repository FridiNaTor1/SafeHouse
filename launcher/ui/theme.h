#pragma once
// SafeHouse launcher look: midnight navy surfaces taken from the
// old ProjectCane logo, its sky blue for actions and the cane's gold for the main
// call to action; the type scale (Atkinson Hyperlegible Regular/Bold) and the
// spacing metrics. Every pixel size goes through px() so the launcher scale
// setting (100, 125 or 150%) resizes the whole interface.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui/imgui.h>
#include "launcher_font.h"
#if __has_include("launcher_font_bold.h")
#include "launcher_font_bold.h"
#define CANE_LAUNCHER_HAS_BOLD 1
#endif

namespace cane::ui {
namespace color {
// Surfaces, darkest to lightest.
inline constexpr uint32_t bg = 0x070B16;       // page background
inline constexpr uint32_t bgTop = 0x0B1530;    // top of the page gradient
inline constexpr uint32_t glow = 0x123A73;     // soft oval behind hero logos
inline constexpr uint32_t sidebar = 0x050810;  // game list rail
inline constexpr uint32_t surface = 0x0E172B;  // cards, footer
inline constexpr uint32_t raised = 0x16233F;   // hovered rows, selected rail item
inline constexpr uint32_t input = 0x101C34;
inline constexpr uint32_t border = 0x1F3357;
inline constexpr uint32_t borderStrong = 0x2C4774;
inline constexpr uint32_t divider = 0x172746;
// Text.
inline constexpr uint32_t text = 0xF2F6FF;
inline constexpr uint32_t text2 = 0xA8B9D6;
inline constexpr uint32_t muted = 0x6C80A3;
// Actions: logo sky blue for ordinary buttons, cane gold for Play/Install.
inline constexpr uint32_t primary = 0x1F6BC9;
inline constexpr uint32_t primaryHover = 0x2C84EC;
inline constexpr uint32_t primaryActive = 0x164B8E;
inline constexpr uint32_t cta = 0xF4C531;
inline constexpr uint32_t ctaHover = 0xFFD75E;
inline constexpr uint32_t ctaActive = 0xD9AA14;
inline constexpr uint32_t ctaText = 0x1A1405;
inline constexpr uint32_t accent = 0x3AA3F0;   // toggles, sliders, selection
inline constexpr uint32_t focus = 0x5EC0FF;    // keyboard/pad focus ring
// States.
inline constexpr uint32_t success = 0x3CC66B;
inline constexpr uint32_t warning = 0xF4C531;
inline constexpr uint32_t danger = 0xF0506E;
} // namespace color

inline ImVec4 vec(uint32_t rgb, float alpha = 1.0f) {
    return { static_cast<float>((rgb >> 16) & 255) / 255.0f, static_cast<float>((rgb >> 8) & 255) / 255.0f,
             static_cast<float>(rgb & 255) / 255.0f, alpha };
}
inline ImU32 u32(uint32_t rgb, float alpha = 1.0f) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255,
                    static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
}
// Colour tokens honour ImGui's disabled alpha, so custom-drawn widgets fade
// exactly like the stock ones inside BeginDisabled().
inline ImU32 ink(uint32_t rgb, float alpha = 1.0f) {
    return u32(rgb, alpha * ImGui::GetStyle().Alpha);
}

struct Fonts {
    ImFont* title = nullptr;    // Bold 30: screen titles
    ImFont* heading = nullptr;  // Bold 20: section and card titles
    ImFont* tab = nullptr;      // Bold 18: tabs, buttons, emphasised body
    ImFont* body = nullptr;     // Regular 18: body text and controls
    ImFont* caption = nullptr;  // Regular 15: captions and field labels
    ImFont* label = nullptr;    // Bold 13: uppercase section labels, tags
    ImFont* display = nullptr;  // Bold 40: game titles on the game page
};

struct Theme {
    float scale = 1.0f;
    Fonts font;
};
inline Theme& theme() {
    static Theme instance;
    return instance;
}
// Spacing scale: 4/8/12/16/24/32/48, multiplied by the launcher scale.
inline float px(float value) { return std::round(value * theme().scale); }
inline Fonts& fonts() { return theme().font; }

// Latin-1 plus the punctuation the launcher prints: – — • … ‹ ›.
inline const ImWchar* glyphRanges() {
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2014, 0x2022, 0x2022,
                                      0x2026, 0x2026, 0x2039, 0x203A, 0 };
    return ranges;
}

// Rebuilds the font atlas for a launcher scale. The caller re-creates the
// renderer's font texture afterwards.
inline void loadFonts(float scale) {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    auto add = [&](const unsigned char* data, size_t size, float pixels) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        config.OversampleV = 1;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size),
                                              std::round(pixels * scale), &config, glyphRanges());
    };
#ifdef CANE_LAUNCHER_HAS_BOLD
    const unsigned char* bold = cane_font_bold;
    const size_t boldSize = sizeof(cane_font_bold);
#else
    const unsigned char* bold = cane_font;
    const size_t boldSize = sizeof(cane_font);
#endif
    auto& f = theme().font;
    // The first font added is ImGui's default: body text.
    f.body = add(cane_font, sizeof(cane_font), 18);
    f.caption = add(cane_font, sizeof(cane_font), 15);
    f.title = add(bold, boldSize, 30);
    f.heading = add(bold, boldSize, 20);
    f.tab = add(bold, boldSize, 18);
    f.label = add(bold, boldSize, 13);
    f.display = add(bold, boldSize, 40);
    theme().scale = scale;
}

inline void applyStyle(float scale) {
    ImGuiStyle s;
    s.WindowPadding = { 24, 24 };
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildRounding = 12;
    s.ChildBorderSize = 1;
    s.PopupRounding = 10;
    s.PopupBorderSize = 1;
    s.FramePadding = { 12, 8 };
    s.FrameRounding = 6;
    s.FrameBorderSize = 1;
    s.ItemSpacing = { 12, 12 };
    s.ItemInnerSpacing = { 8, 6 };
    s.CellPadding = { 12, 8 };
    s.IndentSpacing = 20;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 5;
    s.GrabMinSize = 16;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.DisabledAlpha = 0.42f;
    s.SeparatorTextBorderSize = 1;
    s.SelectableTextAlign = { 0, 0.5f };
    auto* c = s.Colors;
    c[ImGuiCol_Text] = vec(color::text);
    c[ImGuiCol_TextDisabled] = vec(color::muted);
    c[ImGuiCol_WindowBg] = vec(color::bg);
    c[ImGuiCol_ChildBg] = vec(color::surface);
    c[ImGuiCol_PopupBg] = vec(0x0F1A31);
    c[ImGuiCol_Border] = vec(color::border);
    c[ImGuiCol_BorderShadow] = vec(0, 0);
    c[ImGuiCol_FrameBg] = vec(color::input);
    c[ImGuiCol_FrameBgHovered] = vec(0x132447);
    c[ImGuiCol_FrameBgActive] = vec(0x15294F);
    c[ImGuiCol_TitleBg] = vec(color::surface);
    c[ImGuiCol_TitleBgActive] = vec(color::raised);
    c[ImGuiCol_TitleBgCollapsed] = vec(color::surface);
    c[ImGuiCol_MenuBarBg] = vec(color::surface);
    c[ImGuiCol_ScrollbarBg] = vec(0, 0);
    c[ImGuiCol_ScrollbarGrab] = vec(color::border);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(color::borderStrong);
    c[ImGuiCol_ScrollbarGrabActive] = vec(color::primary);
    c[ImGuiCol_CheckMark] = vec(color::accent);
    c[ImGuiCol_SliderGrab] = vec(color::accent);
    c[ImGuiCol_SliderGrabActive] = vec(color::focus);
    c[ImGuiCol_Button] = vec(color::primary);
    c[ImGuiCol_ButtonHovered] = vec(color::primaryHover);
    c[ImGuiCol_ButtonActive] = vec(color::primaryActive);
    c[ImGuiCol_Header] = vec(color::accent, 0.18f);
    c[ImGuiCol_HeaderHovered] = vec(color::raised);
    c[ImGuiCol_HeaderActive] = vec(color::accent, 0.26f);
    c[ImGuiCol_Separator] = vec(color::divider);
    c[ImGuiCol_SeparatorHovered] = vec(color::borderStrong);
    c[ImGuiCol_SeparatorActive] = vec(color::accent);
    c[ImGuiCol_ResizeGrip] = vec(0, 0);
    c[ImGuiCol_ResizeGripHovered] = vec(0, 0);
    c[ImGuiCol_ResizeGripActive] = vec(0, 0);
    c[ImGuiCol_Tab] = vec(color::surface);
    c[ImGuiCol_TabHovered] = vec(color::raised);
    c[ImGuiCol_TabActive] = vec(color::raised);
    c[ImGuiCol_TabUnfocused] = vec(color::surface);
    c[ImGuiCol_TabUnfocusedActive] = vec(color::raised);
    c[ImGuiCol_PlotLines] = vec(color::accent);
    c[ImGuiCol_PlotLinesHovered] = vec(color::focus);
    c[ImGuiCol_PlotHistogram] = vec(color::accent);
    c[ImGuiCol_PlotHistogramHovered] = vec(color::focus);
    c[ImGuiCol_TableHeaderBg] = vec(0, 0);
    c[ImGuiCol_TableBorderStrong] = vec(color::border);
    c[ImGuiCol_TableBorderLight] = vec(color::divider);
    c[ImGuiCol_TableRowBg] = vec(0, 0);
    c[ImGuiCol_TableRowBgAlt] = vec(0xFFFFFF, 0.018f);
    c[ImGuiCol_TextSelectedBg] = vec(color::accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = vec(color::accent);
    c[ImGuiCol_NavHighlight] = vec(color::focus);
    c[ImGuiCol_NavWindowingHighlight] = vec(color::focus, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = vec(color::bg, 0.6f);
    c[ImGuiCol_ModalWindowDimBg] = vec(color::bg, 0.72f);
    s.ScaleAllSizes(scale);
    ImGui::GetStyle() = s;
}
} // namespace cane::ui
