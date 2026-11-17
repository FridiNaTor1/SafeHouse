#ifndef CANE_PLATFORM_VR_PAD_MAP_H_
#define CANE_PLATFORM_VR_PAD_MAP_H_
// The VR controllers as one PS2-style pad (Sly 2 / Sly 3 vr.cpp SyncActions): the OpenXR actions' raw states in, a
// GLFW gamepad state out (GLFW's layout, the games' PS2 names: A Cross, B Circle, X Square, Y Triangle, LB / RB L1 /
// R1, the triggers' axes L2 / R2, the sticks' clicks L3 / R3, Back Select, Start Start). No OpenXR or GLFW calls
// here, so the rules are tested without a headset (tools/controller_selftest.cpp).
//
// The default layout (the user's, 2026-11-11, second version after playing; the prompts' VR families in
// controller_family.h follow it):
//   Touch layout (Meta Quest Touch, Valve Index, Pico, Vive Focus 3 / Cosmos, HP Reverb G2: four face buttons):
//     right A Cross, right B Square, the left grip Circle, the right grip Triangle (pressed past half: no delay),
//     left X L2, left Y R2 (Index: left A / B), the triggers L1 / R1 (pressed past half);
//     Start = Touch's left menu button, or right grip + right stick click; Select = left grip + left stick click
//     (the HP Reverb G2: its left / right menu buttons are Select / Start).
//   Pad layout (Windows Mixed Reality, HTC Vive wands, the generic profile: one face button a hand, real Start /
//   Select): the right trackpad's click Cross, the left one's Square (Sly's attack), the grips Circle / Triangle, the
//   triggers L1 / R1 (no L2 / R2 buttons); the menu buttons Start / Select.
//   Gamepad layout (the Steam Frame's controllers: a whole gamepad split in two hands, the Steam Controller (2026)'s
//   controls; XR_VALVE_frame_controller_interaction): as a standard pad by position, A Cross, B Circle, X Square,
//   Y Triangle (all four on the right hand), the bumpers L1 / R1, the triggers L2 / R2 (analog), the stick clicks
//   L3 / R3, the left hand's d-pad the d-pad, View Select, Menu Start. The grips are left free (they are squeezed to
//   hold the controllers; no game action, so no accidental presses).
//
// The grip + stick-click combos: a grip held (its Circle / Triangle already went to the game on the press: the grips are
// basic moves, so no delay), then that hand's stick clicked: Start / Select while the click is held. From the click on,
// the grip stays away from the game until it is let go (its button is released at once), and the click until it is
// released. A stick clicked first and the grip pressed after is L3 / R3 + Circle / Triangle.
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cane::vrpad {

// GLFW's gamepad indices (glfw3.h), here so the header needs no GLFW
enum : int { kA = 0, kB = 1, kX = 2, kY = 3, kLB = 4, kRB = 5, kBack = 6, kStart = 7, kGuide = 8, kLThumb = 9,
             kRThumb = 10, kDpadUp = 11, kDpadRight = 12, kDpadDown = 13, kDpadLeft = 14, kButtonCount = 15 };
enum : int { kAxisLX = 0, kAxisLY = 1, kAxisRX = 2, kAxisRY = 3, kAxisLT = 4, kAxisRT = 5, kAxisCount = 6 };

inline constexpr float kGripOn = 0.5f, kGripOff = 0.35f, kTriggerOn = 0.55f, kTriggerOff = 0.4f;

enum class Layout { Touch, Pads, Gamepad };

// The Steam Frame's controllers (Valve, Steamworks "Steam Frame > Input"): the instance extension and the interaction
// profile (the second name is the one SteamVR used before 2.15.1). Without the extension SteamVR presents them as
// Touch controllers (the Touch layout).
inline constexpr const char* kFrameExtension = "XR_VALVE_frame_controller_interaction";
inline constexpr const char* kFrameProfile = "/interaction_profiles/valve/frame_controller_valve";
inline constexpr const char* kFrameProfileOld = "/interaction_profiles/valve/frame_controller";

// The controllers an interaction profile path names (XrPath string of xrGetCurrentInteractionProfile), the layout
// they get and whether they have real Start / Select buttons (no grip + stick-click combos). The values are kept (Sly 2's
// Controls page pictures: prompt_atlas_rects.inc ids 64 + the value); new ones go at the end.
//   Touch       Meta Quest Touch (Quest 2 / 3 / 3S / Pro, Rift S; Touch Plus / Touch Pro), and any profile not known here
//   Index       Valve Index        Frame   the Steam Frame's controllers
//   Wmr         Windows Mixed Reality motion controllers (also Samsung Odyssey)
//   Vive        HTC Vive wands     Simple  the generic khr/simple_controller
//   Pico        Pico 4 / 4 Ultra / Neo 3 (XR_BD_controller_interaction)
//   ViveFocus3  HTC Vive Focus 3 / XR Elite (XR_HTC_vive_focus3_controller_interaction)
//   ViveCosmos  HTC Vive Cosmos (XR_HTC_vive_cosmos_controller_interaction)
//   HpReverb    HP Reverb G2 (XR_EXT_hp_mixed_reality_controller): A / B / X / Y like Touch, menu buttons on both hands
enum class Controllers { None, Touch, Index, Frame, Wmr, Vive, Simple, Pico, ViveFocus3, ViveCosmos, HpReverb };
inline constexpr int kControllersCount = 11;
inline Controllers controllersOf(const char* profile) {
    if (!profile || !*profile) return Controllers::None;
    const auto has = [&](const char* sz) { return std::strstr(profile, sz) != nullptr; };
    if (has("valve/frame_controller")) return Controllers::Frame;
    if (has("valve/index")) return Controllers::Index;
    if (has("microsoft/motion_controller") || has("samsung/odyssey")) return Controllers::Wmr;
    if (has("hp/mixed_reality")) return Controllers::HpReverb;
    if (has("htc/vive_focus3")) return Controllers::ViveFocus3;
    if (has("htc/vive_cosmos")) return Controllers::ViveCosmos;
    if (has("htc/vive_controller")) return Controllers::Vive;
    if (has("khr/simple")) return Controllers::Simple;
    if (has("pico") || has("bytedance")) return Controllers::Pico;
    return Controllers::Touch;
}
// The OpenXR extension a profile needs (nullptr: core) and the profiles the games suggest bindings for, besides the
// core ones (oculus/touch, valve/index, microsoft/motion, htc/vive, khr/simple) and the Steam Frame's.
inline constexpr const char* kViveFocus3Extension = "XR_HTC_vive_focus3_controller_interaction";
inline constexpr const char* kViveFocus3Profile = "/interaction_profiles/htc/vive_focus3_controller";
inline constexpr const char* kViveCosmosExtension = "XR_HTC_vive_cosmos_controller_interaction";
inline constexpr const char* kViveCosmosProfile = "/interaction_profiles/htc/vive_cosmos_controller";
inline constexpr const char* kHpExtension = "XR_EXT_hp_mixed_reality_controller";
inline constexpr const char* kHpProfile = "/interaction_profiles/hp/mixed_reality_controller";
inline constexpr const char* kOdysseyExtension = "XR_EXT_samsung_odyssey_controller";
inline constexpr const char* kOdysseyProfile = "/interaction_profiles/samsung/odyssey_controller";
inline constexpr const char* kPicoExtension = "XR_BD_controller_interaction";
inline constexpr const char* kPico4Profile = "/interaction_profiles/bytedance/pico4_controller";
inline constexpr const char* kPicoNeo3Profile = "/interaction_profiles/bytedance/pico_neo3_controller";
inline Layout layoutOf(Controllers c) {
    switch (c) {
    case Controllers::Frame: return Layout::Gamepad;
    case Controllers::Wmr: case Controllers::Vive: case Controllers::Simple: return Layout::Pads;
    default: return Layout::Touch;
    }
}
inline bool realStartSelectOf(Controllers c) {
    return c == Controllers::Frame || c == Controllers::Wmr || c == Controllers::Vive || c == Controllers::Simple ||
           c == Controllers::HpReverb;
}

// The actions' states this frame. Touch layout: a / b the right hand's lower / upper face button, x / y the left
// hand's; Pads layout: a the right trackpad's click, x the left one's (b / y unbound); Gamepad layout: a / b / x / y
// the face buttons by their own names (south / east / west / north), the bumpers and the d-pad.
struct Raw {
    bool a = false, b = false, x = false, y = false;
    bool bumperL = false, bumperR = false;               // (Gamepad layout)
    bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
    float gripL = 0, gripR = 0, trigL = 0, trigR = 0;    // 0..1
    float lx = 0, ly = 0, rx = 0, ry = 0;                // the sticks, OpenXR's (y up)
    bool clickL = false, clickR = false;                 // the sticks' clicks
    bool menu = false;                                   // Touch's left menu button (Start)
    bool start = false, select = false;                  // real Start / Select buttons (Pads layout)
};

struct Pad {
    bool buttons[kButtonCount] = {};
    float axes[kAxisCount] = {0, 0, 0, 0, -1, -1};
};

struct State {
    bool grip[2] = {};           // the grip down (with hysteresis)
    bool gripAway[2] = {};       // the grip used by a combo: away from the game until it is let go
    bool combo[2] = {};          // the click's combo (Start / Select held) until the click is released
    bool clickPrev[2] = {};      // the click last frame (raw): a combo starts on a click's press
    bool trig[2] = {};           // L1 / R1 (with hysteresis)
};

// One frame. realStartSelect: the controllers have real Start / Select buttons (no combos). (t: unused, kept for the
// callers' signature)
inline Pad map(const Raw& raw, Layout layout, bool realStartSelect, double t, State& s) {
    (void)t;
    Pad p;
    const float agrip[2] = {raw.gripL, raw.gripR};
    const bool aclick[2] = {raw.clickL, raw.clickR};
    bool aclickOut[2] = {raw.clickL, raw.clickR};
    bool agripOut[2] = {false, false};
    bool fStart = raw.start || raw.menu, fSelect = raw.select;
    for (int h = 0; h < 2; ++h) {
        s.grip[h] = s.grip[h] ? agrip[h] > kGripOff : agrip[h] > kGripOn;
        const bool clickPress = aclick[h] && !s.clickPrev[h];
        if (realStartSelect) {
            s.combo[h] = s.gripAway[h] = false;
        } else {
            if (!aclick[h]) s.combo[h] = false;
            if (!s.grip[h]) s.gripAway[h] = false;
            if (clickPress && s.grip[h]) s.combo[h] = s.gripAway[h] = true;
        }
        agripOut[h] = s.grip[h] && !s.gripAway[h];
        if (s.combo[h]) aclickOut[h] = false, (h ? fStart : fSelect) = true;
        s.clickPrev[h] = aclick[h];
    }
    const float atrig[2] = {raw.trigL, raw.trigR};
    for (int h = 0; h < 2; ++h) s.trig[h] = s.trig[h] ? atrig[h] > kTriggerOff : atrig[h] > kTriggerOn;
    if (layout == Layout::Gamepad) {
        // a standard pad by position (the grips free: they hold the controllers)
        p.buttons[kA] = raw.a, p.buttons[kB] = raw.b, p.buttons[kX] = raw.x, p.buttons[kY] = raw.y;
        p.buttons[kLB] = raw.bumperL, p.buttons[kRB] = raw.bumperR;
        p.axes[kAxisLT] = std::clamp(raw.trigL, 0.0f, 1.0f) * 2.0f - 1.0f;
        p.axes[kAxisRT] = std::clamp(raw.trigR, 0.0f, 1.0f) * 2.0f - 1.0f;
        p.buttons[kDpadUp] = raw.dpadUp, p.buttons[kDpadDown] = raw.dpadDown;
        p.buttons[kDpadLeft] = raw.dpadLeft, p.buttons[kDpadRight] = raw.dpadRight;
        p.buttons[kBack] = raw.select;
        p.buttons[kStart] = raw.start || raw.menu;
        p.buttons[kLThumb] = raw.clickL, p.buttons[kRThumb] = raw.clickR;
        p.axes[kAxisLX] = raw.lx, p.axes[kAxisLY] = -raw.ly, p.axes[kAxisRX] = raw.rx, p.axes[kAxisRY] = -raw.ry;
        return p;
    }
    p.buttons[kA] = raw.a;
    p.buttons[kB] = agripOut[0];   // Circle: the left grip
    p.buttons[kY] = agripOut[1];   // Triangle: the right grip
    if (layout == Layout::Touch) {
        p.buttons[kX] = raw.b;                       // Square: right B
        p.axes[kAxisLT] = raw.x ? 1.0f : -1.0f;      // L2: left X (Index: left A)
        p.axes[kAxisRT] = raw.y ? 1.0f : -1.0f;      // R2: left Y (Index: left B)
    } else {
        p.buttons[kX] = raw.x;                       // Square: the left trackpad's click
    }
    p.buttons[kLB] = s.trig[0];
    p.buttons[kRB] = s.trig[1];
    p.buttons[kBack] = fSelect;
    p.buttons[kStart] = fStart;
    p.buttons[kLThumb] = aclickOut[0];
    p.buttons[kRThumb] = aclickOut[1];
    p.axes[kAxisLX] = raw.lx;
    p.axes[kAxisLY] = -raw.ly;   // (GLFW: down is positive)
    p.axes[kAxisRX] = raw.rx;
    p.axes[kAxisRY] = -raw.ry;
    return p;
}

}  // namespace cane::vrpad

#endif
