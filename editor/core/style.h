#pragma once
// The editor's look: the launcher's palette (launcher/ui/theme.h color::) with denser spacing for tool panels.
// Header-only; needs ImGui. Used by Sly2-Release --editor (the launcher's own page uses launcher/ui/theme.h).
#include <imgui/imgui.h>

#include <cstdint>

namespace cane::editor::style {

inline constexpr uint32_t bg = 0x070B16, surface = 0x0E172B, raised = 0x16233F, input = 0x101C34, border = 0x1F3357,
                          borderStrong = 0x2C4774, text = 0xF2F6FF, text2 = 0xA8B9D6, muted = 0x6C80A3, primary = 0x1F6BC9,
                          primaryHover = 0x2C84EC, primaryActive = 0x164B8E, cta = 0xF4C531, ctaHover = 0xFFD75E,
                          ctaActive = 0xD9AA14, ctaText = 0x1A1405, accent = 0x3AA3F0, focus = 0x5EC0FF, success = 0x3CC66B,
                          danger = 0xF0506E;

inline ImVec4 vec(uint32_t rgb, float a = 1.0f) {
    return {float((rgb >> 16) & 255) / 255.0f, float((rgb >> 8) & 255) / 255.0f, float(rgb & 255) / 255.0f, a};
}
inline ImU32 u32(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(vec(rgb, a)); }

inline void apply(float scale) {
    ImGuiStyle s;
    s.WindowPadding = {12, 12};
    s.WindowRounding = 8;
    s.WindowBorderSize = 1;
    s.ChildRounding = 6;
    s.PopupRounding = 8;
    s.PopupBorderSize = 1;
    s.FramePadding = {8, 5};
    s.FrameRounding = 5;
    s.FrameBorderSize = 1;
    s.ItemSpacing = {8, 6};
    s.ItemInnerSpacing = {6, 4};
    s.CellPadding = {8, 4};
    s.IndentSpacing = 16;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 5;
    s.GrabMinSize = 12;
    s.GrabRounding = 5;
    s.TabRounding = 5;
    s.SeparatorTextBorderSize = 1;
    auto* c = s.Colors;
    c[ImGuiCol_Text] = vec(text);
    c[ImGuiCol_TextDisabled] = vec(muted);
    c[ImGuiCol_WindowBg] = vec(bg, 0.94f);
    c[ImGuiCol_ChildBg] = vec(surface);
    c[ImGuiCol_PopupBg] = vec(0x0F1A31, 0.98f);
    c[ImGuiCol_Border] = vec(border);
    c[ImGuiCol_BorderShadow] = vec(0, 0);
    c[ImGuiCol_FrameBg] = vec(input);
    c[ImGuiCol_FrameBgHovered] = vec(0x132447);
    c[ImGuiCol_FrameBgActive] = vec(0x15294F);
    c[ImGuiCol_TitleBg] = vec(surface);
    c[ImGuiCol_TitleBgActive] = vec(raised);
    c[ImGuiCol_TitleBgCollapsed] = vec(surface);
    c[ImGuiCol_MenuBarBg] = vec(surface);
    c[ImGuiCol_ScrollbarBg] = vec(0, 0);
    c[ImGuiCol_ScrollbarGrab] = vec(border);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(borderStrong);
    c[ImGuiCol_ScrollbarGrabActive] = vec(primary);
    c[ImGuiCol_CheckMark] = vec(accent);
    c[ImGuiCol_SliderGrab] = vec(accent);
    c[ImGuiCol_SliderGrabActive] = vec(focus);
    c[ImGuiCol_Button] = vec(primary);
    c[ImGuiCol_ButtonHovered] = vec(primaryHover);
    c[ImGuiCol_ButtonActive] = vec(primaryActive);
    c[ImGuiCol_Header] = vec(accent, 0.18f);
    c[ImGuiCol_HeaderHovered] = vec(raised);
    c[ImGuiCol_HeaderActive] = vec(accent, 0.26f);
    c[ImGuiCol_Separator] = vec(border);
    c[ImGuiCol_Tab] = vec(surface);
    c[ImGuiCol_TabHovered] = vec(raised);
    c[ImGuiCol_TabActive] = vec(raised);
    c[ImGuiCol_TableHeaderBg] = vec(surface);
    c[ImGuiCol_TableBorderStrong] = vec(border);
    c[ImGuiCol_TableBorderLight] = vec(0x172746);
    c[ImGuiCol_TableRowBgAlt] = vec(0xFFFFFF, 0.02f);
    c[ImGuiCol_TextSelectedBg] = vec(accent, 0.35f);
    s.ScaleAllSizes(scale);
    ImGui::GetStyle() = s;
}

}  // namespace cane::editor::style
