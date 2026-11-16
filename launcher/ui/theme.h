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
#include <vector>
#include "../l10n.h"
#include "launcher_font.h"
#if __has_include("launcher_font_bold.h")
#include "launcher_font_bold.h"
#define CANE_LAUNCHER_HAS_BOLD 1
#endif
// The languages' other letters (launcher/assets/l10n, scripts/make-launcher-l10n-fonts.py): Noto Sans CJK JP for
// Japanese and Noto Sans for Cyrillic and the rest, subset to the characters launcher/lang uses, merged into each font
// for the glyphs Atkinson Hyperlegible lacks.
#if __has_include("launcher_l10n_fonts.h")
#include "launcher_l10n_fonts.h"
#define CANE_LAUNCHER_HAS_L10N_FONTS 1
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

// Atkinson Hyperlegible's letters: Latin-1 and Latin Extended-A (Polish, Czech, Turkish...) plus the punctuation the
// launcher prints (– — ‘ ’ “ ” „ • … ‹ › €).
inline const ImWchar* glyphRanges() {
    static const ImWchar ranges[] = { 0x0020, 0x017F, 0x2013, 0x2014, 0x2018, 0x201E, 0x2022, 0x2022,
                                      0x2026, 0x2026, 0x2039, 0x203A, 0x20AC, 0x20AC, 0 };
    return ranges;
}
// The active language's other characters (and every language's own name, for the language list): merged from the
// Noto fonts. Rebuilt when the language changes (loadFonts).
inline std::vector<ImWchar>& languageRanges() {
    static std::vector<ImWchar> ranges;
    return ranges;
}
inline void buildLanguageRanges(const l10n::Table& table) {
    auto& out = languageRanges();
    out.clear();
    std::u32string cps = l10n::codepoints(table);
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
    for (char32_t c : cps) {
        if (c > 0xFFFF || (c >= 0x20 && c <= 0x17F)) continue;
        if (!out.empty() && out.back() + 1 == static_cast<ImWchar>(c)) out.back() = static_cast<ImWchar>(c);
        else out.push_back(static_cast<ImWchar>(c)), out.push_back(static_cast<ImWchar>(c));
    }
    out.push_back(0);
}
// Adds one launcher font (Atkinson Hyperlegible plus the merged Noto letters) to an atlas.
inline ImFont* addLauncherFont(ImFontAtlas* atlas, bool bold, float pixels) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 2;
    config.OversampleV = 1;
#ifdef CANE_LAUNCHER_HAS_BOLD
    const unsigned char* data = bold ? cane_font_bold : cane_font;
    const size_t size = bold ? sizeof(cane_font_bold) : sizeof(cane_font);
#else
    (void)bold;
    const unsigned char* data = cane_font;
    const size_t size = sizeof(cane_font);
#endif
    ImFont* font = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size), pixels, &config, glyphRanges());
#ifdef CANE_LAUNCHER_HAS_L10N_FONTS
    if (languageRanges().size() > 1) {
        ImFontConfig merge;
        merge.FontDataOwnedByAtlas = false;
        merge.MergeMode = true;
        merge.OversampleH = 1;
        merge.OversampleV = 1;
        // (Noto's letters sit a little lower than Atkinson's at the same size: nudge them up to share its baseline)
        merge.GlyphOffset.y = 0;
        auto add = [&](const unsigned char* d, size_t n) {
            atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(d), static_cast<int>(n), pixels, &merge, languageRanges().data());
        };
        if (bold) add(cane_l10n_noto_bold, sizeof(cane_l10n_noto_bold)), add(cane_l10n_cjk_bold, sizeof(cane_l10n_cjk_bold));
        else add(cane_l10n_noto_regular, sizeof(cane_l10n_noto_regular)), add(cane_l10n_cjk_regular, sizeof(cane_l10n_cjk_regular));
    }
#endif
    return font;
}
// The characters of a language the launcher's fonts cannot draw (--l10n-check).
inline std::u32string missingGlyphs(const l10n::Table& table) {
    buildLanguageRanges(table);
    ImFontAtlas atlas;
    ImFont* regular = addLauncherFont(&atlas, false, 18);
    ImFont* bold = addLauncherFont(&atlas, true, 18);
    atlas.Build();
    std::u32string missing;
    for (char32_t c : l10n::codepoints(table))
        if (c <= 0xFFFF && (!regular->FindGlyphNoFallback(static_cast<ImWchar>(c)) || !bold->FindGlyphNoFallback(static_cast<ImWchar>(c))) &&
            missing.find(c) == std::u32string::npos)
            missing.push_back(c);
    return missing;
}

// Rebuilds the font atlas for a launcher scale. The caller re-creates the
// renderer's font texture afterwards.
// (the active language's letters too: call it again when l10n::generation() changes)
inline void loadFonts(float scale) {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    buildLanguageRanges(l10n::active());
    auto add = [&](bool bold, float pixels) { return addLauncherFont(io.Fonts, bold, std::round(pixels * scale)); };
    auto& f = theme().font;
    // The first font added is ImGui's default: body text.
    f.body = add(false, 18);
    f.caption = add(false, 15);
    f.title = add(true, 30);
    f.heading = add(true, 20);
    f.tab = add(true, 18);
    f.label = add(true, 13);
    f.display = add(true, 40);
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
