#pragma once
// Launcher gamepad feed for Dear ImGui. The vendored imgui_impl_glfw only
// reads GLFW_JOYSTICK_1; this layer feeds the pad chosen by
// cane::input::selectActivePad() instead (Steam may park an idle virtual pad
// in slot 1) and gates input while the launcher is hidden behind a running
// game. The vendored backend is not modified.
//
// What ImGui receives: D-pad (the left stick also moves focus, like a D-pad),
// South = activate, East = cancel, LB/RB/LT/RT, Start, View, L3/R3, and the
// right stick as ImGui's manual scroll stick (so the focused window scrolls).
// West and North are not fed (ImGui would start window switching or text
// input with them); they are reported through PadEdges::x and ::y.
#include <GLFW/glfw3.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "controller_family.h"
namespace cane::launcher {
// Rising edges (with key repeat) of pad buttons that ImGui does not consume
// for navigation, plus the right stick for scrolling.
struct PadEdges {
    bool back = false;   // East (B / Circle): go back
    bool start = false;  // Start: the screen's primary action
    bool select = false; // Back/View: toggle the logs drawer
    bool x = false;      // West: screen action (e.g. refresh); not fed to ImGui
    bool y = false;      // North: screen action (e.g. copy code); not fed to ImGui
    bool lb = false, rb = false, lt = false, rt = false;
    float scrollY = 0.0f; // right stick Y after a 0.25 dead zone, -1..1 (positive = down)
};
namespace detail {
// Timings for repeated edges (x, y, bumpers, triggers). back, start and select
// never repeat, so holding B cannot skip through several screens.
inline constexpr double kPadRepeatDelay = 0.400, kPadRepeatRate = 0.080;
inline constexpr float kStickNavPress = 0.50f, kStickNavRelease = 0.35f, kScrollDeadZone = 0.25f;
inline constexpr float kTriggerPress = 0.50f, kTriggerRelease = 0.30f;
// Virtual buttons beyond GLFW's 15: the triggers as buttons and the left
// stick as a D-pad.
enum PadBit : int {
    Pad_LeftTrigger = GLFW_GAMEPAD_BUTTON_LAST + 1,
    Pad_RightTrigger,
    Pad_StickUp,
    Pad_StickDown,
    Pad_StickLeft,
    Pad_StickRight,
    Pad_BitCount
};
// Everything one frame of pad input produced. Shared with osk.h, which reads
// the raw edges while it owns the pad.
struct PadFrame {
    PadEdges edges;             // raw edges (screens receive empty edges while the keyboard is open)
    bool south = false;         // South edge (also fed to ImGui)
    bool l3 = false, r3 = false; // stick-click edges
    bool dpadLeft = false, dpadRight = false, dpadUp = false, dpadDown = false; // D-pad or left stick edges
    uint32_t held = 0;          // PadBit mask of buttons held (after gating)
    int jid = -1;               // pad being read, -1 for none
    bool live = false;          // input accepted this frame and a pad is present
    bool oskOpen = false;       // set by osk.h while the on-screen keyboard owns the pad
};
inline PadFrame& padFrame() {
    static PadFrame frame;
    return frame;
}
// Edge detector with typematic repeat.
struct PadRepeat {
    double next = 0;
    bool update(bool down, bool wasDown, double now, bool repeat) {
        if (!down) return false;
        if (!wasDown) {
            next = now + kPadRepeatDelay;
            return true;
        }
        if (repeat && now >= next) {
            next = now + kPadRepeatRate;
            return true;
        }
        return false;
    }
};
// Previous callbacks of the window (ImGui's, chained to anything before them).
struct PadCallbacks {
    GLFWkeyfun key = nullptr;
    GLFWcharfun character = nullptr;
    GLFWcursorposfun cursor = nullptr;
    GLFWcursorenterfun enter = nullptr;
    GLFWmousebuttonfun mouseButton = nullptr;
    GLFWscrollfun scroll = nullptr;
    double cursorX = 0, cursorY = 0;
    bool cursorKnown = false;
};
inline PadCallbacks& padCallbacks() {
    static PadCallbacks callbacks;
    return callbacks;
}
} // namespace detail

class GamepadNav {
public:
    // Call after ImGui_ImplGlfw_InitForOpenGL(window, true). Chains the key,
    // char, cursor and mouse-button callbacks into
    // input::tracker().noteKeyboard()/noteMouse().
    void init(GLFWwindow* window) {
        m_window = window;
        if (!window) return;
        auto& previous = detail::padCallbacks();
        previous.key = glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            if (action != GLFW_RELEASE) input::tracker().noteKeyboard();
            if (auto next = detail::padCallbacks().key) next(w, key, scancode, action, mods);
        });
        previous.character = glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int codepoint) {
            if (auto next = detail::padCallbacks().character) next(w, codepoint);
        });
        previous.cursor = glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
            auto& state = detail::padCallbacks();
            // Ignore the synthetic event when the pointer enters or the window
            // maps, and sub-pixel jitter: only real movement means "mouse".
            if (state.cursorKnown && std::abs(x - state.cursorX) + std::abs(y - state.cursorY) >= 3.0)
                input::tracker().noteMouse();
            if (!state.cursorKnown || std::abs(x - state.cursorX) + std::abs(y - state.cursorY) >= 3.0) {
                state.cursorX = x;
                state.cursorY = y;
                state.cursorKnown = true;
            }
            if (state.cursor) state.cursor(w, x, y);
        });
        previous.enter = glfwSetCursorEnterCallback(window, [](GLFWwindow* w, int entered) {
            detail::padCallbacks().cursorKnown = false;
            if (auto next = detail::padCallbacks().enter) next(w, entered);
        });
        previous.mouseButton = glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
            if (action == GLFW_PRESS) input::tracker().noteMouse();
            if (auto next = detail::padCallbacks().mouseButton) next(w, button, action, mods);
        });
        previous.scroll = glfwSetScrollCallback(window, [](GLFWwindow* w, double dx, double dy) {
            input::tracker().noteMouse();
            if (auto next = detail::padCallbacks().scroll) next(w, dx, dy);
        });
    }
    // Call right before ImGui_ImplGlfw_NewFrame(): hides the gamepad flag so
    // the backend skips its own joystick-1 polling.
    void beforeBackendNewFrame() {
        ImGuiIO& io = ImGui::GetIO();
        m_navGamepad = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        m_restore = true;
    }
    // Call right after ImGui_ImplGlfw_NewFrame(). acceptInput must be false
    // while the window is unfocused or iconified or a game window is shown;
    // all pad keys are then released, and buttons still held when input
    // resumes stay ignored until they are released.
    PadEdges afterBackendNewFrame(bool acceptInput) {
        ImGuiIO& io = ImGui::GetIO();
        if (m_restore && m_navGamepad) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else if (!m_restore) m_navGamepad = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
        m_restore = false;

        input::tracker().pollGamepads();
        GLFWgamepadstate state{};
        m_jid = input::selectActivePad(m_jid, &state);
        return update(m_jid >= 0 ? &state : nullptr, acceptInput, glfwGetTime());
    }
    // One frame of pad input from an explicit state (nullptr: no pad); what
    // afterBackendNewFrame() runs after reading the active pad. Exposed for
    // tests.
    PadEdges update(const GLFWgamepadstate* pad, bool acceptInput, double now) {
        ImGuiIO& io = ImGui::GetIO();
        GLFWgamepadstate state = pad ? *pad : GLFWgamepadstate{};
        const uint32_t raw = pad ? heldMask(state) : 0;
        auto& frame = detail::padFrame();
        frame.jid = pad ? m_jid : -1;
        frame.live = acceptInput && pad;
        if (!frame.live) {
            m_blocked = raw;
            state = GLFWgamepadstate{};
        } else {
            m_blocked &= raw; // released buttons are usable again
        }
        const uint32_t held = frame.live ? raw & ~m_blocked : 0;
        if (m_blocked) { // keep blocked buttons and sticks out of ImGui as well
            for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; ++button)
                if (m_blocked & (1u << button)) state.buttons[button] = GLFW_RELEASE;
            if (m_blocked & (1u << detail::Pad_LeftTrigger)) state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
            if (m_blocked & (1u << detail::Pad_RightTrigger)) state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
        }
        if (!frame.live) state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;

        if (m_navGamepad) {
            if (pad) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
            feedImGui(io, state, held);
        }

        const uint32_t previous = m_held;
        m_held = held;
        frame.held = held;
        auto edge = [&](int bit, bool repeat) {
            const uint32_t mask = 1u << bit;
            return m_repeat[bit].update((held & mask) != 0, (previous & mask) != 0, now, repeat);
        };
        PadEdges edges;
        edges.back = edge(GLFW_GAMEPAD_BUTTON_B, false);
        edges.start = edge(GLFW_GAMEPAD_BUTTON_START, false);
        edges.select = edge(GLFW_GAMEPAD_BUTTON_BACK, false);
        edges.x = edge(GLFW_GAMEPAD_BUTTON_X, true);
        edges.y = edge(GLFW_GAMEPAD_BUTTON_Y, true);
        edges.lb = edge(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, true);
        edges.rb = edge(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, true);
        edges.lt = edge(detail::Pad_LeftTrigger, true);
        edges.rt = edge(detail::Pad_RightTrigger, true);
        const float stick = frame.live ? state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] : 0.0f;
        edges.scrollY = std::abs(stick) <= detail::kScrollDeadZone
                            ? 0.0f
                            : std::copysign((std::abs(stick) - detail::kScrollDeadZone) / (1.0f - detail::kScrollDeadZone),
                                            stick);
        edges.scrollY = std::clamp(edges.scrollY, -1.0f, 1.0f);
        frame.south = edge(GLFW_GAMEPAD_BUTTON_A, false);
        frame.l3 = edge(GLFW_GAMEPAD_BUTTON_LEFT_THUMB, false);
        frame.r3 = edge(GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, false);
        auto direction = [&](int button, int stickBit) {
            const bool down = (held & ((1u << button) | (1u << stickBit))) != 0;
            const bool wasDown = (previous & ((1u << button) | (1u << stickBit))) != 0;
            return m_repeat[button].update(down, wasDown, now, true);
        };
        frame.dpadUp = direction(GLFW_GAMEPAD_BUTTON_DPAD_UP, detail::Pad_StickUp);
        frame.dpadDown = direction(GLFW_GAMEPAD_BUTTON_DPAD_DOWN, detail::Pad_StickDown);
        frame.dpadLeft = direction(GLFW_GAMEPAD_BUTTON_DPAD_LEFT, detail::Pad_StickLeft);
        frame.dpadRight = direction(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, detail::Pad_StickRight);
        frame.edges = edges;
        return frame.oskOpen ? PadEdges{} : edges;
    }
    // The pad currently read (-1 for none).
    int activePad() const { return m_jid; }

private:
    // Held buttons, triggers and left-stick directions as a PadBit mask. The
    // stick uses hysteresis so a stick resting near the threshold does not
    // chatter.
    uint32_t heldMask(const GLFWgamepadstate& state) const {
        uint32_t mask = 0;
        for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; ++button)
            if (state.buttons[button] == GLFW_PRESS) mask |= 1u << button;
        auto trigger = [&](int axis, int bit) {
            const float value = (state.axes[axis] + 1.0f) * 0.5f;
            const bool was = (m_held & (1u << bit)) != 0;
            if (value > (was ? detail::kTriggerRelease : detail::kTriggerPress)) mask |= 1u << bit;
        };
        trigger(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, detail::Pad_LeftTrigger);
        trigger(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, detail::Pad_RightTrigger);
        auto direction = [&](float value, int bit) {
            const bool was = (m_held & (1u << bit)) != 0;
            if (value > (was ? detail::kStickNavRelease : detail::kStickNavPress)) mask |= 1u << bit;
        };
        const float x = state.axes[GLFW_GAMEPAD_AXIS_LEFT_X], y = state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        // Only the dominant axis counts, so diagonals do not move twice.
        if (std::abs(x) >= std::abs(y)) {
            direction(-x, detail::Pad_StickLeft);
            direction(x, detail::Pad_StickRight);
        } else {
            direction(-y, detail::Pad_StickUp);
            direction(y, detail::Pad_StickDown);
        }
        return mask;
    }
    static void feedImGui(ImGuiIO& io, const GLFWgamepadstate& state, uint32_t held) {
        auto button = [&](ImGuiKey key, int glfwButton) { io.AddKeyEvent(key, (held & (1u << glfwButton)) != 0); };
        auto analog = [&](ImGuiKey key, float value, float v0, float v1) {
            float v = (value - v0) / (v1 - v0);
            v = std::clamp(v, 0.0f, 1.0f);
            io.AddKeyAnalogEvent(key, v > 0.10f, v);
        };
        button(ImGuiKey_GamepadStart, GLFW_GAMEPAD_BUTTON_START);
        button(ImGuiKey_GamepadBack, GLFW_GAMEPAD_BUTTON_BACK);
        button(ImGuiKey_GamepadFaceRight, GLFW_GAMEPAD_BUTTON_B);
        button(ImGuiKey_GamepadFaceDown, GLFW_GAMEPAD_BUTTON_A);
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp, false);
        auto dpad = [&](ImGuiKey key, int glfwButton, int stickBit) {
            io.AddKeyEvent(key, (held & ((1u << glfwButton) | (1u << stickBit))) != 0);
        };
        dpad(ImGuiKey_GamepadDpadLeft, GLFW_GAMEPAD_BUTTON_DPAD_LEFT, detail::Pad_StickLeft);
        dpad(ImGuiKey_GamepadDpadRight, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, detail::Pad_StickRight);
        dpad(ImGuiKey_GamepadDpadUp, GLFW_GAMEPAD_BUTTON_DPAD_UP, detail::Pad_StickUp);
        dpad(ImGuiKey_GamepadDpadDown, GLFW_GAMEPAD_BUTTON_DPAD_DOWN, detail::Pad_StickDown);
        button(ImGuiKey_GamepadL1, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
        button(ImGuiKey_GamepadR1, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
        analog(ImGuiKey_GamepadL2, state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER], -0.75f, 1.0f);
        analog(ImGuiKey_GamepadR2, state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER], -0.75f, 1.0f);
        button(ImGuiKey_GamepadL3, GLFW_GAMEPAD_BUTTON_LEFT_THUMB);
        button(ImGuiKey_GamepadR3, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB);
        // ImGui scrolls the focused window with its "left stick" keys; give it
        // the right stick, since the left one already moves focus.
        const float rx = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], ry = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        analog(ImGuiKey_GamepadLStickLeft, rx, -detail::kScrollDeadZone, -1.0f);
        analog(ImGuiKey_GamepadLStickRight, rx, detail::kScrollDeadZone, 1.0f);
        analog(ImGuiKey_GamepadLStickUp, ry, -detail::kScrollDeadZone, -1.0f);
        analog(ImGuiKey_GamepadLStickDown, ry, detail::kScrollDeadZone, 1.0f);
        for (ImGuiKey key : { ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, ImGuiKey_GamepadRStickUp,
                              ImGuiKey_GamepadRStickDown })
            io.AddKeyAnalogEvent(key, false, 0.0f);
    }

    GLFWwindow* m_window = nullptr;
    int m_jid = -1;
    bool m_navGamepad = true, m_restore = false;
    uint32_t m_held = 0, m_blocked = 0;
    std::array<detail::PadRepeat, detail::Pad_BitCount> m_repeat{};
};
} // namespace cane::launcher
