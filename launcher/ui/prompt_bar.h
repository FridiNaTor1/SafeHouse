#pragma once
// Footer button hints ("[A] Select  [B] Back"), drawn with the prompt atlas
// for the active controller family: Kenney pad icons for a controller,
// keycaps after keyboard input, nothing after mouse input. Missing textures
// fall back to text in a rounded outline.
#include <GLFW/glfw3.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include "button_prompts.h"
#include "controller_family.h"
#include "../l10n.h"
namespace cane::launcher {
struct Hint {
    input::Button button; // pad button (positional), or Button::Count for keyboard-only hints
    int key;              // GLFW key shown in keyboard mode (or input::kMouseLeftKey), 0 for none
    const char* label;    // action text, e.g. "Back"
};

namespace detail {
struct PromptPreferences {
    int promptStyle = 0;             // SystemSettings::promptStyle
    int alwaysOriginalPsPrompts = 0; // SystemSettings::alwaysOriginalPsPrompts
};
inline PromptPreferences& promptPreferences() {
    static PromptPreferences preferences;
    return preferences;
}
// Uploads the atlas on first use (needs the launcher's GL context, which is
// current while ImGui draws). Tried once; a failure keeps text fallbacks.
inline const input::PromptAtlas& launcherAtlas() {
    static bool attempted = false;
    auto& atlas = input::promptAtlas();
    if (!attempted && !atlas.uploaded()) {
        attempted = true;
        atlas.upload();
    }
    return atlas;
}
inline ImTextureID textureId(unsigned texture) {
    return reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture));
}
} // namespace detail

// The launcher's copy of the prompt settings, used by the on-screen keyboard
// and by launcherPromptFamily(). Call whenever the settings are loaded or
// changed (Settings page).
inline void setPromptPreferences(int promptStyle, int alwaysOriginalPsPrompts) {
    detail::promptPreferences() = { promptStyle, alwaysOriginalPsPrompts };
}
// input::promptFamily() for the stored preferences: "Original PlayStation
// Icons", then an explicit style, then the last used device.
inline input::Family launcherPromptFamily() {
    const auto& preferences = detail::promptPreferences();
    return input::promptFamily(preferences.promptStyle, preferences.alwaysOriginalPsPrompts);
}

// Short keycap text for keys without an icon (and for the text fallback).
inline std::string keyCapText(int key) {
    switch (key) {
    case 0: return {};
    case input::kMouseLeftKey: return tr("Click");
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: return tr("Enter");
    case GLFW_KEY_ESCAPE: return tr("Esc");
    case GLFW_KEY_TAB: return tr("Tab");
    case GLFW_KEY_SPACE: return tr("Space");
    case GLFW_KEY_BACKSPACE: return tr("Backspace");
    case GLFW_KEY_DELETE: return tr("Del");
    case GLFW_KEY_UP: return tr("Up");
    case GLFW_KEY_DOWN: return tr("Down");
    case GLFW_KEY_LEFT: return tr("Left");
    case GLFW_KEY_RIGHT: return tr("Right");
    case GLFW_KEY_LEFT_SHIFT: case GLFW_KEY_RIGHT_SHIFT: return tr("Shift");
    case GLFW_KEY_LEFT_CONTROL: case GLFW_KEY_RIGHT_CONTROL: return tr("Ctrl");
    case GLFW_KEY_LEFT_ALT: case GLFW_KEY_RIGHT_ALT: return tr("Alt");
    case GLFW_KEY_PAGE_UP: return tr("PgUp");
    case GLFW_KEY_PAGE_DOWN: return tr("PgDn");
    case GLFW_KEY_HOME: return tr("Home");
    case GLFW_KEY_END: return tr("End");
    case GLFW_KEY_INSERT: return tr("Ins");
    default: break;
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return trf("Num %d", key - GLFW_KEY_KP_0);
    if ((key >= GLFW_KEY_A && key <= GLFW_KEY_Z) || (key >= GLFW_KEY_0 && key <= GLFW_KEY_9))
        return std::string(1, static_cast<char>(key));
    if (glfwGetCurrentContext())
        if (const char* name = glfwGetKeyName(key, 0); name && *name) {
            std::string text = name;
            for (char& c : text) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
            return text;
        }
    return trf("Key %d", key);
}

namespace detail {
// Text in a rounded outline, vertically centred in a px-high slot.
inline float textCap(ImDrawList* drawList, ImVec2 pos, float px, const char* text, ImU32 color, bool draw) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float height = std::max(px * 0.78f, size.y + 4.0f), padding = std::max(px * 0.22f, 4.0f);
    const float width = std::max(size.x + padding * 2.0f, height);
    if (draw && drawList) {
        const ImVec2 min{ pos.x, pos.y + (px - height) * 0.5f }, max{ pos.x + width, min.y + height };
        drawList->AddRect(min, max, color, height * 0.3f, 0, std::max(1.0f, px / 22.0f));
        drawList->AddText({ min.x + (width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f }, color, text);
    }
    return width;
}
} // namespace detail

// Draws a pad button icon (or its buttonLabel() in an outline) with its top
// left at pos in a px-high slot and returns the width used. draw == false
// only measures.
inline float drawButtonPrompt(ImDrawList* drawList, ImVec2 pos, float px, input::Button button,
                              input::Family family, ImU32 fallback, bool draw = true) {
    if (button == input::Button::Count) return 0.0f;
    const auto& atlas = detail::launcherAtlas();
    const unsigned texture = atlas.texture(family);
    const input::IconUV uv = atlas.uv(family, button);
    if (texture && uv.valid) {
        if (draw && drawList)
            drawList->AddImage(detail::textureId(texture), pos, { pos.x + px, pos.y + px }, { uv.u0, uv.v1 },
                               { uv.u1, uv.v0 });
        return px;
    }
    return detail::textCap(drawList, pos, px, input::buttonLabel(button, family), fallback, draw);
}
// The same for a keyboard key (GLFW key code) or input::kMouseLeftKey.
inline float drawKeyPrompt(ImDrawList* drawList, ImVec2 pos, float px, int key, ImU32 fallback, bool draw = true) {
    if (key == 0) return 0.0f;
    const auto& atlas = detail::launcherAtlas();
    const unsigned texture = atlas.keyboardTexture();
    const input::IconUV uv = atlas.keyUV(key);
    if (texture && uv.valid) {
        if (draw && drawList)
            drawList->AddImage(detail::textureId(texture), pos, { pos.x + px, pos.y + px }, { uv.u0, uv.v1 },
                               { uv.u1, uv.v0 });
        return px;
    }
    return detail::textCap(drawList, pos, px, keyCapText(key).c_str(), fallback, draw);
}
// ImGui item versions (advance the cursor like ImGui::Image), e.g. for the
// binding tables on the Settings page.
inline void buttonPromptItem(input::Button button, input::Family family, float px, ImU32 fallback) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = drawButtonPrompt(nullptr, pos, px, button, family, fallback, false);
    ImGui::Dummy({ std::max(width, 1.0f), px });
    drawButtonPrompt(ImGui::GetWindowDrawList(), pos, px, button, family, fallback);
}
inline void keyPromptItem(int key, float px, ImU32 fallback) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = drawKeyPrompt(nullptr, pos, px, key, fallback, false);
    ImGui::Dummy({ std::max(width, 1.0f), px });
    drawKeyPrompt(ImGui::GetWindowDrawList(), pos, px, key, fallback);
}

namespace detail {
inline float hints(ImDrawList* drawList, ImVec2 rightBaseline, float iconPx, std::span<const Hint> hints, ImU32 text,
                   ImU32 muted, input::Family family, bool draw) {
    if (input::tracker().lastWasMouse()) return 0.0f;
#if defined(CANE_ANDROID)
    // (a phone or tablet before any key or controller was used: touch needs no key hints)
    if (input::tracker().idle()) return 0.0f;
#endif
    const bool keyboard = family == input::Family::Keyboard;
    const float iconGap = std::round(iconPx * 0.3f), hintGap = std::round(iconPx * 0.9f);
    float x = rightBaseline.x;
    bool first = true;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        if (keyboard ? it->key == 0 : it->button == input::Button::Count) continue;
        // A hint without a label shares the next hint's label ("LB RB Move").
        const char* label = it->label ? tr(it->label) : "";
        if (!first) x -= *label ? hintGap : std::round(iconGap * 0.5f);
        first = false;
        if (*label) {
            const ImVec2 labelSize = ImGui::CalcTextSize(label);
            x -= labelSize.x;
            if (draw) drawList->AddText({ x, std::round(rightBaseline.y - labelSize.y * 0.5f) }, text, label);
            x -= iconGap;
        }
        const float width = keyboard ? drawKeyPrompt(nullptr, {}, iconPx, it->key, muted, false)
                                     : drawButtonPrompt(nullptr, {}, iconPx, it->button, family, muted, false);
        x -= width;
        const ImVec2 iconPos{ std::round(x), std::round(rightBaseline.y - iconPx * 0.5f) };
        if (draw) {
            if (keyboard) drawKeyPrompt(drawList, iconPos, iconPx, it->key, muted);
            else drawButtonPrompt(drawList, iconPos, iconPx, it->button, family, muted);
        }
    }
    return rightBaseline.x - x;
}
} // namespace detail

// Draws hints right-to-left ending at rightBaseline (x = right edge, y =
// vertical centre). iconPx is the icon height. family is normally
// launcherPromptFamily(), i.e. input::promptFamily(settings.promptStyle,
// settings.alwaysOriginalPsPrompts). Keyboard draws keycaps (hints with
// key == 0 are skipped); nothing is drawn after mouse input. A hint with an
// empty label is drawn right next to the following one, which labels both.
inline void drawPromptHints(ImDrawList* drawList, ImVec2 rightBaseline, float iconPx, std::span<const Hint> hints,
                            ImU32 text, ImU32 muted, input::Family family) {
    if (!drawList) return;
    detail::hints(drawList, rightBaseline, iconPx, hints, text, muted, family, true);
}
// Width drawPromptHints() would use (0 after mouse input).
inline float measurePromptHints(float iconPx, std::span<const Hint> hints, input::Family family) {
    return detail::hints(nullptr, {}, iconPx, hints, 0, 0, family, false);
}
} // namespace cane::launcher
