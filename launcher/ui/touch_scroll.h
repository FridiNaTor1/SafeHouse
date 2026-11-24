// SafeHouse launcher: a finger drags the page it is on (touch screens: Android, iOS, the Steam Deck's screen).
//
// ImGui scrolls a region only by its scroll bar or the mouse wheel; a finger press is a mouse click. Once a press on a
// touch screen moves vertically past a small threshold, the innermost scrollable region under it follows the finger,
// the item it started on is let go (so releasing never clicks it), and on release the region keeps the flick's speed
// and slows down. A mostly horizontal drag (a slider) is left alone, and regions that opt out of the mouse wheel
// (ImGuiWindowFlags_NoScrollWithMouse, e.g. the game page) are skipped. The mouse is unaffected.
#pragma once

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace cane::ui {

class TouchScroll {
public:
    // Once per frame, after the windows are submitted and before ImGui::Render().
    void update() {
        ImGuiContext& g = *GImGui;
        const ImGuiIO& io = g.IO;
        const float dt = std::max(io.DeltaTime, 1.0f / 1000.0f);
        if (io.MouseClicked[0]) {
            m_target = m_fling = nullptr, m_dragging = false, m_velocity = 0;
            if (io.MouseSource == ImGuiMouseSource_TouchScreen) m_target = scrollableAt(g.HoveredWindow);
            // (a finger on the scroll bar itself drags the bar as before)
            if (m_target && g.ActiveId && g.ActiveId == ImGui::GetWindowScrollbarID(m_target, ImGuiAxis_Y)) m_target = nullptr;
        }
        if (m_target && io.MouseDown[0]) {
            if (!m_dragging) {
                const float dx = io.MousePos.x - io.MouseClickedPos[0].x, dy = io.MousePos.y - io.MouseClickedPos[0].y;
                const float threshold = std::max(io.MouseDragThreshold, 10.0f * scale());
                if (std::fabs(dx) > threshold && std::fabs(dx) > std::fabs(dy)) m_target = nullptr;  // (a slider)
                else if (std::fabs(dy) > threshold) {
                    m_dragging = true;
                    if (g.ActiveId) ImGui::ClearActiveID();  // the item under the finger is not clicked on release
                }
            }
            if (m_target && m_dragging) {
                const float delta = -io.MouseDelta.y;
                scrollBy(m_target, delta);
                const float v = delta / dt;
                m_velocity = m_velocity * 0.6f + v * 0.4f;  // (smoothed over the last few frames)
                if (std::fabs(delta) < 0.01f) m_velocity *= 0.5f;  // (a finger that stopped before lifting)
            }
        } else if (m_target) {
            if (m_dragging && std::fabs(m_velocity) > 60.0f * scale()) m_fling = m_target;
            m_target = nullptr, m_dragging = false;
        }
        if (m_fling && !m_target) {
            // still part of the context (a window closed meanwhile is dropped)
            if (g.Windows.contains(m_fling) && m_fling->Active && scrollBy(m_fling, m_velocity * dt)) {
                m_velocity *= std::pow(0.04f, dt);  // (about 4 % of the speed left after a second)
                if (std::fabs(m_velocity) < 20.0f * scale()) m_fling = nullptr;
            } else
                m_fling = nullptr;
        }
        // (an item activated by a press that has turned into a drag stays let go)
        if (m_dragging && g.ActiveId && g.ActiveIdIsJustActivated) ImGui::ClearActiveID();
    }
    // A press on a touch screen is currently scrolling a region (callers can ignore taps then).
    bool dragging() const { return m_dragging; }

private:
    static float scale() { return std::max(1.0f, ImGui::GetIO().DisplaySize.y / 720.0f); }
    // The innermost region under the finger that can scroll vertically (walking out through child windows only).
    static ImGuiWindow* scrollableAt(ImGuiWindow* w) {
        for (; w; w = w->ParentWindow) {
            if (w->ScrollMax.y > 0 && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse)) return w;
            if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) break;
        }
        return nullptr;
    }
    // Scrolls a region by delta pixels; false once it is at an end.
    static bool scrollBy(ImGuiWindow* w, float delta) {
        const float from = w->ScrollTarget.y < FLT_MAX ? w->ScrollTarget.y : w->Scroll.y;
        const float to = std::clamp(from + delta, 0.0f, w->ScrollMax.y);
        ImGui::SetScrollY(w, to);
        return to != from;
    }

    ImGuiWindow* m_target = nullptr;
    ImGuiWindow* m_fling = nullptr;
    bool m_dragging = false;
    float m_velocity = 0;
};

}  // namespace cane::ui
