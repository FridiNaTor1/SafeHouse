#pragma once
// Launcher components: buttons, cards, settings-style rows (toggle, stepper,
// slider), fields with their label above, pills and tags, hand-drawn icons
// and controller button glyphs. Everything draws with the theme.h tokens.
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "brand.h"
#include "button_prompts.h"
#include "controller_family.h"
#include "face_buttons.h"
#include "osk.h"
#include "theme.h"

namespace cane::ui {
// ---------------------------------------------------------------- basics
struct FontScope {
    explicit FontScope(ImFont* font) { ImGui::PushFont(font); }
    ~FontScope() { ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};
struct ColorScope {
    explicit ColorScope(uint32_t rgb, float alpha = 1.0f) { ImGui::PushStyleColor(ImGuiCol_Text, vec(rgb, alpha)); }
    ~ColorScope() { ImGui::PopStyleColor(); }
    ColorScope(const ColorScope&) = delete;
    ColorScope& operator=(const ColorScope&) = delete;
};
inline float controlHeight() { return px(38); }
inline float largeHeight() { return px(46); }

inline void text(const char* value, ImFont* font, uint32_t rgb = color::text) {
    FontScope f(font);
    ColorScope c(rgb);
    ImGui::TextUnformatted(value);
}
inline void textWrapped(const char* value, ImFont* font, uint32_t rgb = color::text2) {
    FontScope f(font);
    ColorScope c(rgb);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value);
    ImGui::PopTextWrapPos();
}
inline ImVec2 measure(ImFont* font, const char* value) {
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, value);
}
// Pulls the next item closer, e.g. a field under its label.
inline void tighten(float amount) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(amount)); }
// Extra vertical space on top of the normal item spacing.
inline void gap(float amount) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(amount)); }
// Starts the next group of a row at rowStart + x with the same top edge.
// (SameLine() alone would push text down to the previous frame's baseline.)
inline void sameRow(ImVec2 rowStart, float x) {
    ImGui::SameLine();
    ImGui::SetCursorScreenPos({ rowStart.x + x, rowStart.y });
    ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0;
}

// ---------------------------------------------------------------- layout dump
// Hidden test hook (--dump-layout): records labelled rectangles so the
// screenshot checker can find rows without OCR.
struct LayoutEntry {
    std::string label;
    ImRect rect;
};
inline bool& layoutRecording() {
    static bool enabled = false;
    return enabled;
}
inline std::vector<LayoutEntry>& layoutEntries() {
    static std::vector<LayoutEntry> entries;
    return entries;
}
inline void recordLayout(const char* label, const ImRect& rect) {
    if (!layoutRecording() || !label || !*label) return;
    std::string clean(label);
    if (auto hash = clean.find("##"); hash != std::string::npos) clean.resize(hash);
    if (!clean.empty()) layoutEntries().push_back({ clean, rect });
}

// ---------------------------------------------------------------- focus
// The nav ring is drawn by ImGui for stock widgets (NavHighlight colour);
// custom items draw the same 2px blue ring offset 3px, with their own radius.
inline bool navFocused(ImGuiID id) {
    ImGuiContext& g = *GImGui;
    return id != 0 && g.NavId == id && !g.NavDisableHighlight && !g.CurrentWindow->DC.NavHideHighlightOneFrame;
}
inline void focusRing(const ImRect& bb, ImGuiID id, float rounding) {
    if (!navFocused(id)) return;
    const float t = px(2), d = px(3) + t * 0.5f;
    ImGui::GetWindowDrawList()->AddRect({ bb.Min.x - d, bb.Min.y - d }, { bb.Max.x + d, bb.Max.y + d },
                                        u32(color::focus), rounding + d, 0, t);
}
// Screen changes ask for the primary action to take focus; the first
// focusable primary item drawn afterwards claims it (see takeFocus()).
inline bool& focusRequested() {
    static bool requested = false;
    return requested;
}
// The frame requestFocus() was called in. A screen that changes while it is
// drawn would otherwise hand the focus to a later item of the old screen.
inline int& focusRequestFrame() {
    static int frame = -1;
    return frame;
}
inline void requestFocus() {
    focusRequested() = true;
    focusRequestFrame() = GImGui && GImGui->WithinFrameScope ? GImGui->FrameCount : -1;
}
inline void setFocus(ImGuiID id, ImGuiWindow* window, bool highlight);
// Call right after the item that should receive focus on screen entry. A
// request made while this frame's screens are drawn waits for the next frame.
inline void takeFocus(bool highlight) {
    if (!focusRequested() || GImGui->FrameCount == focusRequestFrame()) return;
    ImGuiContext& g = *GImGui;
    const ImGuiID id = g.LastItemData.ID;
    if (!id || (g.LastItemData.InFlags & ImGuiItemFlags_Disabled)) return;
    focusRequested() = false;
    setFocus(id, g.CurrentWindow, highlight);
    if (!g.CurrentWindow->Appearing && !g.CurrentWindow->ClipRect.Contains(g.LastItemData.Rect))
        ImGui::ScrollToRect(g.CurrentWindow, g.LastItemData.NavRect);
}
// Moves keyboard/pad focus to an item now, overriding ImGui's own pick of
// the first item when a window appears.
inline void setFocus(ImGuiID id, ImGuiWindow* window, bool highlight) {
    ImGuiContext& g = *GImGui;
    ImGui::FocusWindow(window);
    ImGui::SetFocusID(id, window);
    g.NavInitRequest = false;
    g.NavInitResult.ID = 0;
    g.NavDisableHighlight = !highlight;
    g.NavDisableMouseHover = highlight;
}

// Right on the keyboard, D-pad or left stick this frame. Moving right out of
// a list into the panel beside it is done by hand: ImGui 1.90 does not move
// from a window into a nav-flattened child beside it.
inline bool navRightPressed() {
    for (ImGuiKey key : { ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight })
        if (ImGui::IsKeyPressed(key, false)) return true;
    return false;
}

// ---------------------------------------------------------------- icons
enum class Icon {
    None, Plus, Search, ChevronLeft, ChevronRight, ChevronDown, Heart, Chat, Check, Copy, Refresh,
    Folder, Disc, Lock, Warning, Users, Info, External, Close, Play, Star, Cog, Trash, Stop, Wrench
};
inline void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
    const float t = std::max(1.5f, s * 0.09f);
    switch (icon) {
    case Icon::None: break;
    case Icon::Plus:
        dl->AddLine({ c.x - s * 0.32f, c.y }, { c.x + s * 0.32f, c.y }, col, t * 1.2f);
        dl->AddLine({ c.x, c.y - s * 0.32f }, { c.x, c.y + s * 0.32f }, col, t * 1.2f);
        break;
    case Icon::Search:
        dl->AddCircle({ c.x - s * 0.08f, c.y - s * 0.08f }, s * 0.26f, col, 24, t * 1.1f);
        dl->AddLine({ c.x + s * 0.11f, c.y + s * 0.11f }, { c.x + s * 0.34f, c.y + s * 0.34f }, col, t * 1.4f);
        break;
    case Icon::ChevronLeft:
    case Icon::ChevronRight:
    case Icon::ChevronDown: {
        const float k = s * 0.2f;
        ImVec2 p[3];
        if (icon == Icon::ChevronDown) p[0] = { c.x - k * 1.6f, c.y - k * 0.8f }, p[1] = { c.x, c.y + k * 0.8f }, p[2] = { c.x + k * 1.6f, c.y - k * 0.8f };
        else {
            const float dir = icon == Icon::ChevronRight ? 1.0f : -1.0f;
            p[0] = { c.x - dir * k * 0.8f, c.y - k * 1.6f }, p[1] = { c.x + dir * k * 0.8f, c.y }, p[2] = { c.x - dir * k * 0.8f, c.y + k * 1.6f };
        }
        dl->AddPolyline(p, 3, col, 0, t * 1.2f);
        break;
    }
    case Icon::Heart: {
        const float r = s * 0.2f;
        dl->AddCircleFilled({ c.x - r * 0.95f, c.y - r * 0.55f }, r, col, 20);
        dl->AddCircleFilled({ c.x + r * 0.95f, c.y - r * 0.55f }, r, col, 20);
        dl->AddTriangleFilled({ c.x - r * 1.92f, c.y - r * 0.32f }, { c.x + r * 1.92f, c.y - r * 0.32f },
                              { c.x, c.y + s * 0.36f }, col);
        break;
    }
    case Icon::Chat: {
        const ImVec2 a{ c.x - s * 0.36f, c.y - s * 0.3f }, b{ c.x + s * 0.36f, c.y + s * 0.18f };
        dl->AddRectFilled(a, b, col, s * 0.14f);
        dl->AddTriangleFilled({ c.x - s * 0.2f, b.y - 1 }, { c.x + s * 0.02f, b.y - 1 }, { c.x - s * 0.22f, c.y + s * 0.38f }, col);
        const ImU32 hole = u32(color::bg);
        for (int i = -1; i <= 1; ++i) dl->AddCircleFilled({ c.x + i * s * 0.17f, c.y - s * 0.06f }, s * 0.055f, hole, 10);
        break;
    }
    case Icon::Check: {
        ImVec2 p[3] = { { c.x - s * 0.3f, c.y }, { c.x - s * 0.08f, c.y + s * 0.22f }, { c.x + s * 0.32f, c.y - s * 0.22f } };
        dl->AddPolyline(p, 3, col, 0, t * 1.4f);
        break;
    }
    case Icon::Copy:
        dl->AddRect({ c.x - s * 0.3f, c.y - s * 0.3f }, { c.x + s * 0.12f, c.y + s * 0.12f }, col, s * 0.06f, 0, t);
        dl->AddRectFilled({ c.x - s * 0.1f, c.y - s * 0.1f }, { c.x + s * 0.32f, c.y + s * 0.32f }, col, s * 0.06f);
        break;
    case Icon::Refresh: {
        const float r = s * 0.3f;
        dl->PathArcTo(c, r, IM_PI * 0.15f, IM_PI * 1.75f, 24);
        dl->PathStroke(col, 0, t * 1.2f);
        const ImVec2 e{ c.x + r * std::cos(IM_PI * 0.15f), c.y + r * std::sin(IM_PI * 0.15f) };
        dl->AddTriangleFilled({ e.x - s * 0.14f, e.y - s * 0.02f }, { e.x + s * 0.14f, e.y - s * 0.02f }, { e.x, e.y + s * 0.16f }, col);
        break;
    }
    case Icon::Folder:
        dl->AddRectFilled({ c.x - s * 0.36f, c.y - s * 0.26f }, { c.x - s * 0.02f, c.y - s * 0.1f }, col, s * 0.05f);
        dl->AddRectFilled({ c.x - s * 0.36f, c.y - s * 0.16f }, { c.x + s * 0.36f, c.y + s * 0.28f }, col, s * 0.06f);
        break;
    case Icon::Disc:
        dl->AddCircle(c, s * 0.36f, col, 32, t);
        dl->AddCircleFilled(c, s * 0.1f, col, 16);
        dl->PathArcTo(c, s * 0.24f, IM_PI * 1.1f, IM_PI * 1.45f, 8);
        dl->PathStroke(col, 0, t * 0.9f);
        break;
    case Icon::Lock:
        dl->PathArcTo({ c.x, c.y - s * 0.08f }, s * 0.16f, IM_PI, IM_PI * 2.0f, 12);
        dl->PathStroke(col, 0, t * 1.1f);
        dl->AddLine({ c.x - s * 0.16f, c.y - s * 0.08f }, { c.x - s * 0.16f, c.y }, col, t * 1.1f);
        dl->AddLine({ c.x + s * 0.16f, c.y - s * 0.08f }, { c.x + s * 0.16f, c.y }, col, t * 1.1f);
        dl->AddRectFilled({ c.x - s * 0.26f, c.y - s * 0.02f }, { c.x + s * 0.26f, c.y + s * 0.32f }, col, s * 0.05f);
        break;
    case Icon::Warning:
        dl->AddTriangleFilled({ c.x, c.y - s * 0.36f }, { c.x + s * 0.38f, c.y + s * 0.3f }, { c.x - s * 0.38f, c.y + s * 0.3f }, col);
        dl->AddLine({ c.x, c.y - s * 0.12f }, { c.x, c.y + s * 0.08f }, u32(color::bg), t * 1.2f);
        dl->AddCircleFilled({ c.x, c.y + s * 0.18f }, t * 0.75f, u32(color::bg), 8);
        break;
    case Icon::Users:
        dl->AddCircleFilled({ c.x, c.y - s * 0.14f }, s * 0.15f, col, 16);
        dl->PathArcTo({ c.x, c.y + s * 0.36f }, s * 0.3f, IM_PI, IM_PI * 2.0f, 16);
        dl->PathFillConvex(col);
        break;
    case Icon::Info:
        dl->AddCircle(c, s * 0.36f, col, 32, t);
        dl->AddLine({ c.x, c.y - s * 0.02f }, { c.x, c.y + s * 0.18f }, col, t * 1.2f);
        dl->AddCircleFilled({ c.x, c.y - s * 0.16f }, t * 0.8f, col, 8);
        break;
    case Icon::External:
        dl->AddLine({ c.x - s * 0.18f, c.y + s * 0.18f }, { c.x + s * 0.24f, c.y - s * 0.24f }, col, t);
        dl->AddLine({ c.x - s * 0.02f, c.y - s * 0.24f }, { c.x + s * 0.24f, c.y - s * 0.24f }, col, t);
        dl->AddLine({ c.x + s * 0.24f, c.y - s * 0.24f }, { c.x + s * 0.24f, c.y + s * 0.02f }, col, t);
        break;
    case Icon::Close:
        dl->AddLine({ c.x - s * 0.24f, c.y - s * 0.24f }, { c.x + s * 0.24f, c.y + s * 0.24f }, col, t * 1.2f);
        dl->AddLine({ c.x + s * 0.24f, c.y - s * 0.24f }, { c.x - s * 0.24f, c.y + s * 0.24f }, col, t * 1.2f);
        break;
    case Icon::Play:
        dl->AddTriangleFilled({ c.x - s * 0.22f, c.y - s * 0.3f }, { c.x + s * 0.32f, c.y }, { c.x - s * 0.22f, c.y + s * 0.3f }, col);
        break;
    case Icon::Cog: {
        const float r = s * 0.24f;
        for (int i = 0; i < 8; ++i) {
            const float a = i * IM_PI / 4.0f;
            const ImVec2 d{ std::cos(a), std::sin(a) };
            dl->AddLine({ c.x + d.x * r * 0.8f, c.y + d.y * r * 0.8f }, { c.x + d.x * s * 0.38f, c.y + d.y * s * 0.38f }, col, t * 2.2f);
        }
        dl->AddCircle(c, r, col, 24, t * 1.6f);
        dl->AddCircleFilled(c, r * 0.4f, col, 16);
        break;
    }
    case Icon::Trash:
        dl->AddLine({ c.x - s * 0.3f, c.y - s * 0.22f }, { c.x + s * 0.3f, c.y - s * 0.22f }, col, t * 1.2f);
        dl->AddLine({ c.x - s * 0.08f, c.y - s * 0.3f }, { c.x + s * 0.08f, c.y - s * 0.3f }, col, t * 1.2f);
        dl->AddRect({ c.x - s * 0.22f, c.y - s * 0.16f }, { c.x + s * 0.22f, c.y + s * 0.34f }, col, s * 0.05f, 0, t * 1.1f);
        dl->AddLine({ c.x - s * 0.07f, c.y - s * 0.06f }, { c.x - s * 0.07f, c.y + s * 0.24f }, col, t);
        dl->AddLine({ c.x + s * 0.07f, c.y - s * 0.06f }, { c.x + s * 0.07f, c.y + s * 0.24f }, col, t);
        break;
    case Icon::Stop: dl->AddRectFilled({ c.x - s * 0.24f, c.y - s * 0.24f }, { c.x + s * 0.24f, c.y + s * 0.24f }, col, s * 0.05f); break;
    case Icon::Wrench:
        dl->AddLine({ c.x - s * 0.26f, c.y + s * 0.26f }, { c.x + s * 0.08f, c.y - s * 0.08f }, col, t * 2.2f);
        dl->PathArcTo({ c.x + s * 0.16f, c.y - s * 0.16f }, s * 0.17f, IM_PI * 0.75f, IM_PI * 2.25f, 16);
        dl->PathStroke(col, 0, t * 1.6f);
        break;
    case Icon::Star: {
        ImVec2 p[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -IM_PI * 0.5f + i * IM_PI / 5.0f, r = (i % 2 ? 0.17f : 0.38f) * s;
            p[i] = { c.x + std::cos(a) * r, c.y + std::sin(a) * r };
        }
        for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, p[i], p[(i + 1) % 10], col);
        break;
    }
    }
}

// ---------------------------------------------------------------- buttons
enum class Tone { Primary, Cta, Secondary, Danger, Quiet };
struct ButtonColors {
    uint32_t fill, hover, active, text;
    float fillAlpha = 1, hoverAlpha = 1, activeAlpha = 1;
    uint32_t border = 0;
    float borderAlpha = 0;
};
inline ButtonColors buttonColors(Tone tone) {
    switch (tone) {
    case Tone::Cta: return { color::cta, color::ctaHover, color::ctaActive, color::ctaText };
    case Tone::Secondary: return { color::surface, color::raised, color::primaryActive, color::text, 1, 1, 1, color::borderStrong, 1 };
    case Tone::Danger: return { color::danger, color::danger, color::danger, color::danger, 0.0f, 0.14f, 0.24f, color::danger, 0.55f };
    case Tone::Quiet: return { color::raised, color::raised, color::border, color::text2, 0.0f, 1, 1 };
    case Tone::Primary: break;
    }
    return { color::primary, color::primaryHover, color::primaryActive, color::text };
}
// A button with an optional leading icon. size.x == 0 fits the label;
// size.y == 0 uses the standard control height.
inline bool button(const char* label, Tone tone = Tone::Primary, ImVec2 size = { 0, 0 }, Icon icon = Icon::None,
                   ImFont* fontOverride = nullptr) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);
    ImFont* font = fontOverride ? fontOverride : (tone == Tone::Quiet || tone == Tone::Secondary) ? fonts().body : fonts().tab;
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, label, end);
    const float iconSize = icon == Icon::None ? 0.0f : px(fontOverride ? 22 : 18);
    const float iconGap = icon == Icon::None || label == end ? 0.0f : px(10);
    float padX = px(tone == Tone::Quiet ? 12 : 20);
    // A fixed width a longer translation does not fit: the smaller caption type, then tighter padding.
    if (size.x > 0 && !fontOverride && textSize.x + iconSize + iconGap + padX * 2 > size.x) {
        font = fonts().caption;
        textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, label, end);
        padX = std::max(px(6), std::min(padX, std::floor((size.x - textSize.x - iconSize - iconGap) * 0.5f)));
    }
    if (size.x <= 0) size.x = textSize.x + iconSize + iconGap + padX * 2;
    if (size.y <= 0) size.y = controlHeight();
    const ImRect bb(window->DC.CursorPos, { window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y });
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const auto c = buttonColors(tone);
    const bool disabled = (g.LastItemData.InFlags & ImGuiItemFlags_Disabled) != 0;
    uint32_t fill = c.fill, textRgb = c.text;
    float alpha = c.fillAlpha;
    if (!disabled && held && hovered) fill = c.active, alpha = c.activeAlpha;
    else if (!disabled && hovered) fill = c.hover, alpha = c.hoverAlpha;
    // Disabled filled buttons turn neutral instead of a washed-out accent.
    if (disabled && (tone == Tone::Cta || tone == Tone::Primary)) fill = color::borderStrong, textRgb = color::text2;
    auto* dl = window->DrawList;
    const float rounding = px(6);
    if (alpha > 0) dl->AddRectFilled(bb.Min, bb.Max, ink(fill, alpha), rounding);
    if (c.borderAlpha > 0) dl->AddRect(bb.Min, bb.Max, ink(c.border, c.borderAlpha), rounding, 0, px(1));
    const float contentWidth = textSize.x + iconSize + iconGap;
    float x = std::round(bb.Min.x + (size.x - contentWidth) * 0.5f);
    const float cy = (bb.Min.y + bb.Max.y) * 0.5f;
    const ImU32 textColor = ink(textRgb);
    if (icon != Icon::None) {
        drawIcon(dl, icon, { x + iconSize * 0.5f, cy }, iconSize, textColor);
        x += iconSize + iconGap;
    }
    dl->AddText(font, font->FontSize, { x, std::round(cy - textSize.y * 0.5f) }, textColor, label, end);
    focusRing(bb, id, rounding);
    recordLayout(label, bb);
    return pressed;
}
// A round, icon-only button (Copy, Close, …) with an accessible label id.
inline bool iconButton(const char* id, Icon icon, float size = 0, uint32_t rgb = color::text2) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    if (size <= 0) size = controlHeight();
    const ImGuiID gid = window->GetID(id);
    const ImRect bb(window->DC.CursorPos, { window->DC.CursorPos.x + size, window->DC.CursorPos.y + size });
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    auto* dl = window->DrawList;
    if (hovered) dl->AddRectFilled(bb.Min, bb.Max, ink(held ? color::border : color::raised), px(6));
    drawIcon(dl, icon, bb.GetCenter(), size * 0.5f, ink(hovered ? color::text : rgb));
    focusRing(bb, gid, px(6));
    if (hovered && GImGui->HoveredIdTimer > 0.5f) {
        std::string tip(id);
        if (auto hash = tip.find("##"); hash != std::string::npos) tip.resize(hash);
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    }
    return pressed;
}

// ---------------------------------------------------------------- text bits
// Small uppercase label above a group ("GAMES").
inline void overline(const char* value) {
    FontScope f(fonts().label);
    ColorScope c(color::text2);
    std::string upper(value);
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    ImGui::TextUnformatted(upper.c_str());
}
inline void heading(const char* value) { text(value, fonts().heading); }
// The label above a field, with optional muted help after it ("Optional").
inline void field(const char* label, const char* help = nullptr) {
    FontScope f(fonts().caption);
    {
        ColorScope c(color::text2);
        ImGui::TextUnformatted(label);
    }
    if (help && *help) {
        ImGui::SameLine(0, px(8));
        ColorScope c(color::muted);
        ImGui::TextUnformatted(help);
    }
    tighten(6);
}
inline void caption(const char* value, uint32_t rgb = color::muted) { textWrapped(value, fonts().caption, rgb); }

// Rounded tag ("INSTALLED") in a state colour.
inline ImVec2 tagSize(const char* value) {
    const auto size = measure(fonts().label, value);
    return { size.x + px(16), px(22) };
}
inline ImVec2 drawTag(ImDrawList* dl, ImVec2 pos, const char* value, uint32_t rgb) {
    std::string upper(value);
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto size = tagSize(upper.c_str());
    dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y }, ink(rgb, 0.16f), size.y * 0.5f);
    const auto t = measure(fonts().label, upper.c_str());
    dl->AddText(fonts().label, fonts().label->FontSize, { pos.x + px(8), std::round(pos.y + (size.y - t.y) * 0.5f) }, ink(rgb), upper.c_str());
    return size;
}
inline void tag(const char* value, uint32_t rgb) {
    std::string upper(value);
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto pos = ImGui::GetCursorScreenPos();
    const auto size = tagSize(upper.c_str());
    drawTag(ImGui::GetWindowDrawList(), pos, value, rgb);
    ImGui::Dummy(size);
}
// Status pill with a leading dot ("● Online").
inline ImVec2 pillSize(const char* value) {
    const auto t = measure(fonts().caption, value);
    return { t.x + px(36), px(30) };
}
inline void drawPill(ImDrawList* dl, ImVec2 pos, const char* value, uint32_t rgb) {
    const auto size = pillSize(value);
    dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y }, ink(rgb, 0.12f), size.y * 0.5f);
    dl->AddRect(pos, { pos.x + size.x, pos.y + size.y }, ink(rgb, 0.35f), size.y * 0.5f, 0, px(1));
    dl->AddCircleFilled({ pos.x + px(15), pos.y + size.y * 0.5f }, px(4), ink(rgb), 12);
    const auto t = measure(fonts().caption, value);
    dl->AddText(fonts().caption, fonts().caption->FontSize, { pos.x + px(25), std::round(pos.y + (size.y - t.y) * 0.5f) }, ink(color::text), value);
}
// ---------------------------------------------------------------- banners
enum class BannerTone { Info, Warning, Error, Success };
// Inline message strip. Returns true when its close button was pressed.
inline bool banner(const char* id, const std::string& message, BannerTone tone, bool dismissible = true) {
    const uint32_t rgb = tone == BannerTone::Error ? color::danger
                         : tone == BannerTone::Warning ? color::warning
                         : tone == BannerTone::Success ? color::success
                                                       : color::accent;
    const Icon icon = tone == BannerTone::Error || tone == BannerTone::Warning ? Icon::Warning
                      : tone == BannerTone::Success ? Icon::Check
                                                    : Icon::Info;
    ImGui::PushID(id);
    const float width = ImGui::GetContentRegionAvail().x;
    const float wrap = width - px(dismissible ? 104 : 64);
    ImFont* font = fonts().body;
    const auto t = font->CalcTextSizeA(font->FontSize, FLT_MAX, wrap, message.c_str());
    const float height = std::max(px(48), t.y + px(24));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, { pos.x + width, pos.y + height }, ink(rgb, 0.1f), px(8));
    dl->AddRectFilled(pos, { pos.x + px(4), pos.y + height }, ink(rgb), px(8), ImDrawFlags_RoundCornersLeft);
    drawIcon(dl, icon, { pos.x + px(30), pos.y + height * 0.5f }, px(20), ink(rgb));
    dl->AddText(font, font->FontSize, { pos.x + px(52), std::round(pos.y + (height - t.y) * 0.5f) }, ink(color::text),
                message.c_str(), nullptr, wrap);
    bool closed = false;
    if (dismissible) {
        ImGui::SetCursorScreenPos({ pos.x + width - px(44), pos.y + (height - px(36)) * 0.5f });
        closed = iconButton(tr("Dismiss"), Icon::Close, px(36));
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy({ width, height });
    ImGui::PopID();
    return closed;
}

// ---------------------------------------------------------------- cards
// A padded surface. Pair with endCard(). height 0 grows with the content.
inline void beginCard(const char* id, ImVec2 size = { 0, 0 }, float padding = 24, bool scroll = false) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(padding), px(padding) });
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(12));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(color::surface));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(color::border));
    ImGuiChildFlags flags = ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (size.y == 0) flags |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, size, flags,
                      ImGuiWindowFlags_NavFlattened | (scroll ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
}
inline void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}
// A large clickable card with an icon, a title and a caption (Home).
inline bool actionCard(const char* id, const char* title, const char* body, Icon icon, ImVec2 size,
                       uint32_t accent = color::accent) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiID gid = window->GetID(id);
    const ImRect bb(window->DC.CursorPos, { window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y });
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    const bool lit = hovered || navFocused(gid);
    auto* dl = window->DrawList;
    const float r = px(14);
    dl->AddRectFilled(bb.Min, bb.Max, ink(lit ? color::raised : color::surface), r);
    dl->AddRect(bb.Min, bb.Max, ink(lit ? color::borderStrong : color::border), r, 0, px(1));
    const float pad = px(24), badge = px(52);
    const ImVec2 badgeCenter{ bb.Min.x + pad + badge * 0.5f, bb.Min.y + size.y * 0.5f };
    dl->AddCircleFilled(badgeCenter, badge * 0.5f, ink(accent, held ? 0.3f : 0.16f), 40);
    drawIcon(dl, icon, badgeCenter, px(28), ink(accent));
    const float textX = bb.Min.x + pad + badge + px(20);
    const float textW = bb.Max.x - textX - px(48);
    ImFont* titleFont = fonts().heading;
    ImFont* bodyFont = fonts().caption;
    const auto bodySize = bodyFont->CalcTextSizeA(bodyFont->FontSize, FLT_MAX, textW, body);
    const float block = titleFont->FontSize + px(6) + bodySize.y;
    float y = std::round(bb.Min.y + (size.y - block) * 0.5f);
    dl->AddText(titleFont, titleFont->FontSize, { textX, y }, ink(color::text), title);
    y += titleFont->FontSize + px(6);
    dl->AddText(bodyFont, bodyFont->FontSize, { textX, y }, ink(color::text2), body, nullptr, textW);
    drawIcon(dl, Icon::ChevronRight, { bb.Max.x - px(28), bb.Min.y + size.y * 0.5f }, px(20),
             ink(lit ? color::text : color::muted));
    focusRing(bb, gid, r);
    recordLayout(title, bb);
    return pressed;
}

// ---------------------------------------------------------------- rows
// One focusable row (settings style): the label sits on the left, the value
// control on the right. Left/right on a focused row step its value instead of
// moving focus; A or a click activates it.
struct Row {
    ImRect bb, control;
    ImGuiID id = 0;
    bool visible = false, hovered = false, held = false, focused = false, activated = false, clicked = false;
    bool disabled = false;
    int step = 0;
};
// The first enabled row drawn since the caller last cleared it, so a pane
// can put the focus on its top row when the pad enters it.
struct FirstRow {
    ImGuiID id = 0;
    ImGuiWindow* window = nullptr;
};
inline FirstRow& firstRow() {
    static FirstRow first;
    return first;
}
inline Row rowBehavior(const char* strId, float height, bool horizontal, float controlFraction = 0.6f) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    Row r;
    r.id = window->GetID(strId);
    const ImVec2 pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    r.bb = ImRect(pos, { pos.x + width, pos.y + height });
    const float controlWidth = std::min(px(320), std::round(width * controlFraction));
    r.control = ImRect({ r.bb.Max.x - px(16) - controlWidth, r.bb.Min.y }, { r.bb.Max.x - px(16), r.bb.Max.y });
    ImGui::ItemSize(r.bb.GetSize());
    if (!ImGui::ItemAdd(r.bb, r.id)) return r;
    r.visible = true;
    r.disabled = (g.LastItemData.InFlags & ImGuiItemFlags_Disabled) != 0;
    if (!r.disabled && !firstRow().id) firstRow() = { r.id, window };
    r.activated = ImGui::ButtonBehavior(r.bb, r.id, &r.hovered, &r.held, ImGuiButtonFlags_PressedOnClick);
    r.clicked = r.activated && r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    r.focused = g.NavId == r.id && g.NavWindow == window;
    if (horizontal && r.focused && !r.disabled) {
        for (ImGuiKey key : { ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                              ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight }) {
            ImGui::SetKeyOwner(key, r.id);
            if (ImGui::IsKeyPressed(key, r.id, ImGuiInputFlags_Repeat))
                r.step += (key == ImGuiKey_LeftArrow || key == ImGuiKey_GamepadDpadLeft || key == ImGuiKey_GamepadLStickLeft) ? -1 : 1;
        }
        r.step = std::clamp(r.step, -1, 1);
    }
    auto* dl = window->DrawList;
    const bool lit = (r.hovered && !g.NavDisableMouseHover) || navFocused(r.id);
    if (lit && !r.disabled) dl->AddRectFilled(r.bb.Min, r.bb.Max, ink(color::raised), px(8));
    focusRing(r.bb, r.id, px(8));
    return r;
}
// Draws the row's label (and an optional caption under it) on the left, up to the control (or `right`, an absolute x,
// when the control leaves more room). A label too long for one line (a longer translation) drops to the caption size,
// then wraps onto two lines.
inline void rowLabel(const Row& r, const char* label, const char* sub = nullptr, uint32_t rgb = color::text, float right = 0) {
    auto* dl = ImGui::GetWindowDrawList();
    ImFont* font = fonts().body;
    ImFont* subFont = fonts().caption;
    const float maxWidth = (right > 0 ? std::max(right, r.bb.Min.x + r.bb.GetWidth() * 0.4f) : r.control.Min.x) - r.bb.Min.x - px(32);
    float wrap = 0;
    if (!sub && measure(font, label).x > maxWidth) {
        font = fonts().caption;
        if (measure(font, label).x > maxWidth) wrap = maxWidth;
    }
    const float labelH = wrap > 0 ? font->CalcTextSizeA(font->FontSize, FLT_MAX, wrap, label).y : font->FontSize;
    const float block = labelH + (sub ? px(2) + subFont->FontSize : 0);
    float y = std::round(r.bb.Min.y + (r.bb.GetHeight() - block) * 0.5f);
    dl->PushClipRect(r.bb.Min, { r.bb.Min.x + px(16) + maxWidth, r.bb.Max.y }, true);
    dl->AddText(font, font->FontSize, { r.bb.Min.x + px(16), y }, ink(rgb), label, nullptr, wrap);
    if (sub) dl->AddText(subFont, subFont->FontSize, { r.bb.Min.x + px(16), y + font->FontSize + px(2) }, ink(color::muted), sub);
    dl->PopClipRect();
    recordLayout(label, r.bb);
}
inline void drawSwitch(ImDrawList* dl, ImVec2 center, bool on) {
    const float w = px(44), h = px(24);
    const ImVec2 a{ center.x - w * 0.5f, center.y - h * 0.5f }, b{ center.x + w * 0.5f, center.y + h * 0.5f };
    dl->AddRectFilled(a, b, ink(on ? color::accent : color::border), h * 0.5f);
    if (!on) dl->AddRect(a, b, ink(color::borderStrong), h * 0.5f, 0, px(1));
    const float knob = h * 0.5f - px(4);
    dl->AddCircleFilled({ on ? b.x - h * 0.5f : a.x + h * 0.5f, center.y }, knob, ink(on ? color::bg : color::text2), 24);
}
// Label + switch. Returns true when the value changed.
inline bool toggleRow(const char* label, bool* value, const char* sub = nullptr, float height = 0) {
    const Row r = rowBehavior(label, height > 0 ? height : px(sub ? 64 : 56), true, 0.2f);
    if (!r.visible) return false;
    bool changed = false;
    if (!r.disabled && (r.activated || (r.step < 0 && *value) || (r.step > 0 && !*value))) {
        *value = !*value;
        changed = true;
    }
    rowLabel(r, label, sub, color::text, r.control.Max.x - px(56));
    drawSwitch(ImGui::GetWindowDrawList(), { r.control.Max.x - px(22), r.bb.GetCenter().y }, *value);
    return changed;
}
// "‹ value ›" stepper over a list of labels. Wraps around. Returns true on change.
inline bool stepperRow(const char* label, int* index, std::span<const std::string> values, const char* sub = nullptr,
                       const char* overrideValue = nullptr, float height = 0) {
    const Row r = rowBehavior(label, height > 0 ? height : px(sub ? 64 : 56), true);
    if (!r.visible || values.empty()) return false;
    const int count = static_cast<int>(values.size());
    int step = r.step;
    const float arrow = px(36);
    const ImRect left(r.control.Min, { r.control.Min.x + arrow, r.control.Max.y });
    const ImRect right({ r.control.Max.x - arrow, r.control.Min.y }, r.control.Max);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (r.clicked) step = left.Contains(mouse) ? -1 : 1;
    else if (r.activated) step = 1;
    bool changed = false;
    if (step && !r.disabled) {
        *index = ((*index < 0 ? 0 : *index + step) % count + count) % count;
        changed = true;
    }
    rowLabel(r, label, sub);
    auto* dl = ImGui::GetWindowDrawList();
    const float cy = r.bb.GetCenter().y;
    const bool lit = r.hovered || r.focused;
    const auto arrowColor = [&](const ImRect& box) {
        return ink(r.hovered && box.Contains(mouse) ? color::accent : lit ? color::text2 : color::muted);
    };
    drawIcon(dl, Icon::ChevronLeft, { left.GetCenter().x, cy }, px(22), arrowColor(left));
    drawIcon(dl, Icon::ChevronRight, { right.GetCenter().x, cy }, px(22), arrowColor(right));
    const char* shown = overrideValue ? overrideValue
                        : (*index >= 0 && *index < count) ? values[static_cast<size_t>(*index)].c_str()
                                                          : "";
    ImFont* font = fonts().tab;
    const auto t = measure(font, shown);
    dl->AddText(font, font->FontSize, { std::round(r.control.GetCenter().x - t.x * 0.5f), std::round(cy - t.y * 0.5f) },
                ink(color::text), shown);
    // Position dots under the value for short lists.
    if (count > 1 && count <= 8) {
        const float dot = px(5), spacing = px(10);
        float x = r.control.GetCenter().x - (count - 1) * spacing * 0.5f;
        for (int i = 0; i < count; ++i, x += spacing)
            dl->AddCircleFilled({ x, cy + t.y * 0.5f + px(9) }, dot * 0.5f, ink(i == *index ? color::accent : color::border), 10);
    }
    return changed;
}
// 0..1 slider in fixed steps (0.1 = 10%). Returns true while the value changes.
inline bool sliderRow(const char* label, float* value, float stepSize = 0.1f, const char* sub = nullptr,
                      const char* format = "%.0f%%", float scale = 100.0f, float height = 0, float dragStep = -1,
                      uint32_t accent = color::accent) {
    const Row r = rowBehavior(label, height > 0 ? height : px(sub ? 64 : 56), true);
    if (!r.visible) return false;
    if (dragStep <= 0) dragStep = stepSize;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID dragKey = r.id + 1;
    const float valueWidth = px(64);
    const ImRect track({ r.control.Min.x + px(8), r.bb.GetCenter().y - px(3) }, { r.control.Max.x - valueWidth, r.bb.GetCenter().y + px(3) });
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    bool changed = false;
    float v = std::clamp(*value, 0.0f, 1.0f);
    if (!r.disabled) {
        if (r.clicked && mouse.x >= track.Min.x - px(10) && mouse.x <= track.Max.x + px(10)) storage->SetBool(dragKey, true);
        if (!r.held) storage->SetBool(dragKey, false);
        if (storage->GetBool(dragKey)) {
            float t = std::clamp((mouse.x - track.Min.x) / track.GetWidth(), 0.0f, 1.0f);
            t = std::round(t / dragStep) * dragStep;
            if (std::fabs(t - v) > 1e-4f) v = t, changed = true;
        }
        if (r.step) {
            const float next = std::clamp(std::round((v + r.step * stepSize) / stepSize) * stepSize, 0.0f, 1.0f);
            if (std::fabs(next - v) > 1e-4f) v = next, changed = true;
        }
    }
    if (changed) *value = v;
    rowLabel(r, label, sub);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(track.Min, track.Max, ink(color::border), px(3));
    const float x = track.Min.x + track.GetWidth() * v;
    dl->AddRectFilled(track.Min, { x, track.Max.y }, ink(accent), px(3));
    dl->AddCircleFilled({ x, track.GetCenter().y }, px(9), ink(color::text), 24);
    dl->AddCircle({ x, track.GetCenter().y }, px(9), ink(accent), 24, px(2));
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), format, v * scale);
    ImFont* font = fonts().tab;
    const auto t = measure(font, buffer);
    dl->AddText(font, font->FontSize, { r.control.Max.x - t.x, std::round(r.bb.GetCenter().y - t.y * 0.5f) }, ink(color::text), buffer);
    return changed;
}
// A row whose control is a value with a trailing chevron (opens something).
inline bool linkRow(const char* label, const char* value, const char* sub = nullptr, Icon trailing = Icon::ChevronRight,
                    float height = 0) {
    const Row r = rowBehavior(label, height > 0 ? height : px(sub ? 64 : 56), false);
    if (!r.visible) return false;
    auto* dl = ImGui::GetWindowDrawList();
    const float cy = r.bb.GetCenter().y;
    float right = r.control.Max.x;
    if (trailing != Icon::None) {
        drawIcon(dl, trailing, { right - px(10), cy }, px(20), ink(r.hovered || r.focused ? color::text : color::muted));
        right -= px(30);
    }
    // (the label may use the room the value leaves; a long value and a long label share it)
    float labelRight = right;
    if (value && *value) {
        ImFont* font = fonts().body;
        const auto t = measure(font, value);
        labelRight = right - t.x - px(8);
        dl->AddText(font, font->FontSize, { right - t.x, std::round(cy - t.y * 0.5f) }, ink(color::text2), value);
    }
    rowLabel(r, label, sub, color::text, labelRight);
    return r.activated && !r.disabled;
}

// ---------------------------------------------------------------- segmented
// Equal-width options in one rounded track.
inline bool segmented(const char* id, int* value, std::span<const char* const> labels, float width = 0) {
    ImGui::PushID(id);
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    const float h = controlHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, { origin.x + width, origin.y + h }, ink(color::input), px(8));
    dl->AddRect(origin, { origin.x + width, origin.y + h }, ink(color::border), px(8), 0, px(1));
    const float part = (width - px(8)) / static_cast<float>(labels.size());
    bool changed = false;
    for (size_t i = 0; i < labels.size(); ++i) {
        ImGui::SetCursorScreenPos({ origin.x + px(4) + part * i, origin.y + px(4) });
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImGuiID gid = window->GetID(static_cast<int>(i));
        const ImRect bb(window->DC.CursorPos, { window->DC.CursorPos.x + part, window->DC.CursorPos.y + h - px(8) });
        ImGui::ItemSize(bb.GetSize());
        if (!ImGui::ItemAdd(bb, gid)) continue;
        bool hovered = false, held = false;
        if (ImGui::ButtonBehavior(bb, gid, &hovered, &held) && *value != static_cast<int>(i)) {
            *value = static_cast<int>(i);
            changed = true;
        }
        const bool selected = *value == static_cast<int>(i);
        if (selected) dl->AddRectFilled(bb.Min, bb.Max, ink(color::primary), px(6));
        else if (hovered) dl->AddRectFilled(bb.Min, bb.Max, ink(color::raised), px(6));
        ImFont* font = selected ? fonts().tab : fonts().body;
        const auto t = measure(font, labels[i]);
        dl->AddText(font, font->FontSize, { std::round(bb.GetCenter().x - t.x * 0.5f), std::round(bb.GetCenter().y - t.y * 0.5f) },
                    ink(selected ? color::text : color::text2), labels[i]);
        focusRing(bb, gid, px(6));
        recordLayout(labels[i], bb);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({ width, h });
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------- text input
// A text field with a placeholder; pad users get the on-screen keyboard.
inline bool inputField(const char* id, char* buffer, size_t capacity, launcher::Charset charset, const char* hint,
                       float width = 0, ImGuiInputTextFlags flags = 0) {
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { px(12), std::round((controlHeight() - ImGui::GetFontSize()) * 0.5f) });
    const bool changed = launcher::padInputText(id, buffer, capacity, charset, flags);
    ImGui::PopStyleVar();
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    auto* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive()) dl->AddRect(bb.Min, bb.Max, ink(color::accent), px(6), 0, px(1.5f));
    else if (!buffer[0] && hint && *hint) {
        const float y = std::round(bb.GetCenter().y - ImGui::GetFontSize() * 0.5f);
        dl->PushClipRect(bb.Min, bb.Max, true);
        dl->AddText({ bb.Min.x + px(12), y }, ink(color::muted), hint);
        dl->PopClipRect();
    }
    recordLayout(id, bb);
    return changed;
}

// Combo boxes: same height as text fields, with a quiet arrow box.
inline void pushComboStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { px(12), std::round((controlHeight() - ImGui::GetFontSize()) * 0.5f) });
    ImGui::PushStyleColor(ImGuiCol_Button, vec(color::input));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(color::raised));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(color::raised));
}
inline void popComboStyle() {
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
}
// ImGui::BeginCombo with the launcher's chevron instead of ImGui's arrow
// button. Call between pushComboStyle() and popComboStyle(); EndCombo() when
// it returns true.
inline bool beginCombo(const char* id, const char* preview) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoArrowButton);
    drawIcon(dl, Icon::ChevronDown, { at.x + w - px(22), at.y + h * 0.5f }, px(18), u32(color::text2));
    return open;
}

// ---------------------------------------------------------------- spinner
inline void spinner(ImDrawList* dl, ImVec2 center, float radius, uint32_t rgb = color::accent) {
    const float t = static_cast<float>(ImGui::GetTime());
    const float start = t * 5.0f;
    dl->AddCircle(center, radius, ink(color::border), 32, px(3));
    dl->PathArcTo(center, radius, start, start + IM_PI * 0.6f, 16);
    dl->PathStroke(ink(rgb), 0, px(3));
}

// ---------------------------------------------------------------- pad glyphs
// Controller button glyph. Uses the Kenney atlas when it is available, and a
// drawn fallback (shapes for PlayStation, lettered buttons for the others)
// otherwise.
inline void drawPadButton(ImDrawList* dl, ImVec2 center, float size, input::Family family, input::Button button) {
    using input::Button;
    using input::Family;
    if (family == Family::Keyboard) family = Family::Xbox;
    auto& atlas = input::promptAtlas();
    if (atlas.uploaded()) {
        const auto uv = atlas.uv(family, button);
        if (const unsigned texture = atlas.texture(family); texture && uv.valid) {
            const float h = size * 0.5f;
            dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture)), { center.x - h, center.y - h },
                         { center.x + h, center.y + h }, { uv.u0, uv.v1 }, { uv.u1, uv.v0 }, ink(0xFFFFFF));
            return;
        }
    }
    const float r = size * 0.46f;
    const bool face = button == Button::South || button == Button::East || button == Button::West || button == Button::North;
    const ImU32 plate = ink(0x1A2848), rim = ink(color::borderStrong), fg = ink(color::text);
    if (face) {
        dl->AddCircleFilled(center, r, plate, 32);
        dl->AddCircle(center, r, rim, 32, px(1));
        if (family == Family::PlayStation) {
            const float k = r * 0.48f, t = std::max(1.5f, size * 0.085f);
            switch (button) {
            case Button::South:
                dl->AddLine({ center.x - k, center.y - k }, { center.x + k, center.y + k }, ink(0x7FA7F5), t);
                dl->AddLine({ center.x + k, center.y - k }, { center.x - k, center.y + k }, ink(0x7FA7F5), t);
                break;
            case Button::East: dl->AddCircle(center, k * 1.05f, ink(0xF26B6B), 24, t); break;
            case Button::West: dl->AddRect({ center.x - k, center.y - k }, { center.x + k, center.y + k }, ink(0xE48AD5), 1, 0, t); break;
            default: dl->AddTriangle({ center.x, center.y - k * 1.15f }, { center.x + k * 1.1f, center.y + k * 0.75f },
                                     { center.x - k * 1.1f, center.y + k * 0.75f }, ink(0x45C9A8), t); break;
            }
            return;
        }
        uint32_t letterColor = color::text;
        if (family == Family::Xbox) {
            static constexpr uint32_t xbox[4] = { 0x6CC24A, 0xE5484D, 0x3B8FEA, 0xF2C230 };
            letterColor = xbox[static_cast<int>(button)];
        }
        const char* letter = input::buttonLabel(button, family);
        ImFont* font = fonts().tab;
        const float fs = size * 0.52f;
        const auto t = font->CalcTextSizeA(fs, FLT_MAX, 0, letter);
        dl->AddText(font, fs, { std::round(center.x - t.x * 0.5f), std::round(center.y - t.y * 0.5f) }, ink(letterColor), letter);
        return;
    }
    if (button == Button::Dpad || button == Button::DpadUp || button == Button::DpadDown || button == Button::DpadLeft ||
        button == Button::DpadRight) {
        const float a = r * 0.34f, b = r * 1.0f;
        const ImU32 arm = ink(0x33466E);
        dl->AddRectFilled({ center.x - a, center.y - b }, { center.x + a, center.y + b }, arm, px(2));
        dl->AddRectFilled({ center.x - b, center.y - a }, { center.x + b, center.y + a }, arm, px(2));
        if (button == Button::Dpad) return;
        // The pressed direction: a light arm with an arrow notch.
        ImVec2 lit[2] = { { center.x - a, center.y - b }, { center.x + a, center.y - a * 0.6f } };
        if (button == Button::DpadDown) lit[0] = { center.x - a, center.y + a * 0.6f }, lit[1] = { center.x + a, center.y + b };
        if (button == Button::DpadLeft) lit[0] = { center.x - b, center.y - a }, lit[1] = { center.x - a * 0.6f, center.y + a };
        if (button == Button::DpadRight) lit[0] = { center.x + a * 0.6f, center.y - a }, lit[1] = { center.x + b, center.y + a };
        dl->AddRectFilled(lit[0], lit[1], fg, px(2));
        return;
    }
    if (button == Button::LS || button == Button::RS || button == Button::LSPress || button == Button::RSPress) {
        dl->AddCircleFilled(center, r, plate, 32);
        dl->AddCircle(center, r * 0.62f, rim, 32, px(1.5f));
        const char* letter = (button == Button::LS || button == Button::LSPress) ? "L" : "R";
        ImFont* font = fonts().tab;
        const float fs = size * 0.44f;
        const auto t = font->CalcTextSizeA(fs, FLT_MAX, 0, letter);
        dl->AddText(font, fs, { std::round(center.x - t.x * 0.5f), std::round(center.y - t.y * 0.5f) }, fg, letter);
        return;
    }
    // Shoulders, triggers, Start/Select and the guide button: a lettered plate.
    const char* label = input::buttonLabel(button, family);
    ImFont* font = fonts().label;
    const float fs = size * 0.42f;
    const auto t = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
    const float w = std::max(size * 1.1f, t.x + size * 0.5f), h = size * 0.72f;
    const bool trigger = button == Button::LT || button == Button::RT;
    dl->AddRectFilled({ center.x - w * 0.5f, center.y - h * 0.5f }, { center.x + w * 0.5f, center.y + h * 0.5f }, plate,
                      trigger ? h * 0.2f : h * 0.5f, trigger ? ImDrawFlags_RoundCornersBottom : 0);
    dl->AddRect({ center.x - w * 0.5f, center.y - h * 0.5f }, { center.x + w * 0.5f, center.y + h * 0.5f }, rim,
                trigger ? h * 0.2f : h * 0.5f, trigger ? ImDrawFlags_RoundCornersBottom : 0, px(1));
    dl->AddText(font, fs, { std::round(center.x - t.x * 0.5f), std::round(center.y - t.y * 0.5f) }, fg, label);
}
// Width that drawPadButton() uses for a glyph of this height.
inline float padButtonWidth(float size, input::Family family, input::Button button) {
    using input::Button;
    if (input::promptAtlas().uploaded() && input::promptAtlas().texture(family == input::Family::Keyboard ? input::Family::Xbox : family))
        return size;
    switch (button) {
    case Button::LB: case Button::RB: case Button::LT: case Button::RT: case Button::Back: case Button::Start: case Button::Guide: {
        const char* label = input::buttonLabel(button, family == input::Family::Keyboard ? input::Family::Xbox : family);
        const auto t = fonts().label->CalcTextSizeA(size * 0.42f, FLT_MAX, 0, label);
        return std::max(size * 1.1f, t.x + size * 0.5f);
    }
    default: return size;
    }
}
// Keyboard key cap ("Enter", "W").
inline float keycapWidth(const char* label, float height) {
    const auto t = fonts().label->CalcTextSizeA(height * 0.48f, FLT_MAX, 0, label);
    return std::max(height, t.x + height * 0.6f);
}
inline void drawKeycap(ImDrawList* dl, ImVec2 min, float height, const char* label) {
    const float w = keycapWidth(label, height);
    const ImVec2 max{ min.x + w, min.y + height };
    dl->AddRectFilled({ min.x, min.y + px(2) }, { max.x, max.y + px(2) }, ink(0x050913), px(5));
    dl->AddRectFilled(min, max, ink(0x1A2848), px(5));
    dl->AddRect(min, max, ink(color::borderStrong), px(5), 0, px(1));
    const float fs = height * 0.48f;
    const auto t = fonts().label->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(fonts().label, fs, { std::round(min.x + (w - t.x) * 0.5f), std::round(min.y + (height - t.y) * 0.5f) }, ink(color::text), label);
}
// ---------------------------------------------------------------- artwork
// Textures uploaded by main.cpp: the SafeHouse logo (the emblem), the app icon and the
// game logos (index == GameId).
struct Image {
    ImTextureID texture = nullptr;
    float aspect = 1.0f; // width / height
};
struct Images {
    ImTextureID logo = nullptr, icon = nullptr;
    float logoAspect = 1.04f; // width / height (launcher/assets/safehouse-logo.png)
    Image games[5];
};
inline Images& images() {
    static Images instance;
    return instance;
}
// Soft radial glow (the logo's oval) built from stacked translucent ellipses.
inline void drawGlow(ImDrawList* dl, ImVec2 center, float rx, float ry, uint32_t rgb = color::glow, float alpha = 0.35f) {
    constexpr int layers = 36;
    for (int i = 0; i < layers; ++i) {
        const float k = 1.0f - static_cast<float>(i) / layers;
        dl->AddEllipseFilled(center, rx * k, ry * k, u32(rgb, alpha / layers * 1.6f), 0.0f, 96);
    }
}
inline void drawLogo(ImDrawList* dl, ImVec2 min, float height) {
    if (!images().logo) return;
    const float width = std::round(height * images().logoAspect);
    dl->AddImage(images().logo, min, { min.x + width, min.y + height });
}
// The brand lockup: the logo (the mask and cane emblem, no lettering) with "SafeHouse" set beside it.
inline float brandGap(float height) { return std::round(height * 0.14f); }
inline float brandWidth(float height, ImFont* font) {
    return std::round(height * images().logoAspect) + brandGap(height) + font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, brand::appName).x;
}
inline void drawBrand(ImDrawList* dl, ImVec2 min, float height, ImFont* font) {
    drawLogo(dl, min, height);
    const float x = min.x + std::round(height * images().logoAspect) + brandGap(height);
    // The text's centre sits a little below the emblem's middle, level with the mask.
    dl->AddText(font, font->FontSize, { x, std::round(min.y + height * 0.62f - font->FontSize * 0.5f) }, u32(color::text), brand::appName);
}
// Screen title with an optional back arrow. Returns true when back was pressed.
inline bool titleBar(const char* title, bool back, const char* sub = nullptr) {
    bool pressed = false;
    const float h = px(40);
    const ImVec2 start = ImGui::GetCursorPos();
    if (back) {
        pressed = iconButton(tr("Back##title"), Icon::ChevronLeft, h, color::text);
        ImGui::SameLine(0, px(10));
    }
    ImFont* font = fonts().title;
    ImGui::SetCursorPosY(start.y + std::round((h - font->FontSize) * 0.5f) - px(1));
    text(title, font);
    ImGui::SetCursorPosY(start.y + h + px(sub ? 4 : 16));
    if (sub) {
        caption(sub, color::text2);
        gap(4);
    }
    return pressed;
}
// Thin rule between groups.
// spacing: distance from the previous item to the rule and from the rule to
// the next item.
inline void divider(float spacing = 12) {
    const float before = std::max(0.0f, px(spacing) - ImGui::GetStyle().ItemSpacing.y);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + before);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, { p.x + ImGui::GetContentRegionAvail().x, p.y }, ink(color::divider), px(1));
    ImGui::Dummy({ 1.0f, px(1) });
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + before);
}
inline std::string upper(std::string value) {
    for (auto& ch : value) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return value;
}
} // namespace cane::ui
