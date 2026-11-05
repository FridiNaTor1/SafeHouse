#pragma once
// Which face button confirms and which goes back in the launcher.
//
// Pads report buttons by position (controller_family.h): GLFW A is always
// the south button. Xbox, PlayStation and Steam pads confirm with south (A /
// Cross) and go back with east (B / Circle). Nintendo confirms with A and
// goes back with B, and its A sits east and its B south, so for the Nintendo
// prompt family the two are swapped. The swap follows the family the footer
// shows (input::promptFamily), so a forced prompt style or "Original PlayStation
// Icons" picks the convention the prompts show.
//
// Steam's "Use Nintendo Button Layout" maps by label instead of position and
// cannot be detected; with it on, set Button prompts to Xbox.
#include <utility>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "controller_family.h"
namespace cane::ui {
inline bool eastConfirms(input::Family family) { return family == input::Family::Nintendo; }
// The family the launcher's prompts and confirm/back buttons follow this
// frame (main.cpp sets it before the screens are drawn).
inline input::Family& faceButtonFamily() {
    static input::Family family = input::Family::Xbox;
    return family;
}
inline input::Button confirmButton() { return eastConfirms(faceButtonFamily()) ? input::Button::East : input::Button::South; }
inline input::Button backButton() { return eastConfirms(faceButtonFamily()) ? input::Button::South : input::Button::East; }
// Name of the back button for help text ("B", "Circle").
inline const char* backButtonLabel() { return input::buttonLabel(backButton(), faceButtonFamily()); }

// Dear ImGui (1.90) always activates with GamepadFaceDown and cancels with
// GamepadFaceRight. For a swapped family this exchanges what ImGui sees of
// the two keys. begin() and end() wrap the code that feeds pad input (the GLFW
// backend's NewFrame and GamepadNav): begin() puts ImGui's state of the two
// keys back to the physical positions, so the feeding code's duplicate-event
// filter compares against what it fed before; end() swaps the newly queued
// events and the state again. The pair is an involution, so switching the
// family between frames needs only the releases in begin().
class FaceButtonSwap {
public:
    void begin(input::Family family) {
        m_swapped = eastConfirms(family);
        if (m_applied != m_swapped) {
            // The family changed: release both keys so neither stays held
            // under the wrong meaning. A key still held is pressed again by
            // the feeding code, because these releases are now its latest
            // events.
            ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
            ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
            m_applied = m_swapped;
        }
        exchange(m_swapped);
    }
    void end() { exchange(m_swapped); }

private:
    static void exchange(bool swapped) {
        if (!swapped) return;
        ImGuiContext& g = *GImGui;
        std::swap(*ImGui::GetKeyData(ImGuiKey_GamepadFaceDown), *ImGui::GetKeyData(ImGuiKey_GamepadFaceRight));
        for (ImGuiInputEvent& e : g.InputEventsQueue)
            if (e.Type == ImGuiInputEventType_Key) {
                if (e.Key.Key == ImGuiKey_GamepadFaceDown) e.Key.Key = ImGuiKey_GamepadFaceRight;
                else if (e.Key.Key == ImGuiKey_GamepadFaceRight) e.Key.Key = ImGuiKey_GamepadFaceDown;
            }
    }
    bool m_swapped = false, m_applied = false;
};
} // namespace cane::ui
