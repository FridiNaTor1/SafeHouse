#include "actioninput.h"
#include "actor.h"
#include "gang.h"
#include "joy.h"
#include "lo.h"
#include "po.h"
#include "pcinput.h"
#include "pcrc.h"
#include "clock.h"
#include "cls.h"
#include "debugmenu.h"
#include "step.h"
#include "hackmouse.h"
#include "sw.h"
#include "vehicle.h"
#include "../platform/controller_family.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <bit>
#include <cfloat>

extern int g_frameJoy;
extern SW* g_psw;
namespace sly2actions {
namespace input = cane::sly2input;
Profile profile = input::defaults();
namespace {
std::array<bool, input::kActionCount> s_keys{}, s_pad{}, s_held{};
std::array<float, input::kActionCount> s_pressed = [] { std::array<float, input::kActionCount> a; a.fill(-FLT_MAX); return a; }();
bool s_usable = false, s_scripted = false;
int s_frame = -1;
int s_stampedFrame = -1;
std::array<int, input::kActionCount> s_pressFrame = [] { std::array<int, input::kActionCount> a; a.fill(-1); return a; }();
constexpr uint32_t s_buttons[PCA_Max] = {0x1000, 0x4000, 0x8000, 0x2000, 0x80, 0x20, 0x40, 0x10,
                                    0x800, 0x100, 4, 8, 1, 2, 0x200, 0x400};

bool ReconActive(const LO* owner)
{
    const auto* actor = owner ? dynamic_cast<const ACTOR*>(PstepFromLo(const_cast<LO*>(owner))) : nullptr;
    return actor && actor->nF20 == 0 && actor->nF04 >= 0 && actor->nF04 < 3;
}

bool IsShowdownTank(const LO* owner)
{
    return owner && owner->oid == 72886 && FCidDerivedFrom(owner->Cid(), 0x32);
}

bool PlayerZoomActive()
{
    const LO* owner = PloPoCurrent(g_psw);
    return ReconActive(owner) || (owner && FCidDerivedFrom(owner->Cid(), 0x45));
}

bool ReconAction(Action action)
{
    switch (action) {
    case Action::Pause:
    case Action::ReconClose:
    case Action::Photo:
    case Action::BentleyDart:
    case Action::ZoomIn:
    case Action::ZoomOut:
        return true;
    default:
        return false;
    }
}

bool WheelBinding(int binding)
{
    return binding == cane::settings::kMouseWheelUp || binding == cane::settings::kMouseWheelDown;
}

bool KeyboardWheelBound(int binding)
{
    if (!WheelBinding(binding)) return false;
    for (size_t i = 0; i < input::kActionCount; ++i)
        if (!input::isNoclipAction(Action(i)) && profile.keyboard[i] == binding) return true;
    return false;
}

void ClearPress(size_t index)
{
    s_pressed[index] = -FLT_MAX;
    s_pressFrame[index] = -1;
}

bool Recent(Action action, float dt, const LO* owner) {
    if (ReconActive(owner) && !ReconAction(action)) return false;
    // UpdateJoy samples before UpdateClock. Stamp on the first gameplay query, after the clock advances,
    // so zero-length press windows retain the engine's original one-frame meaning.
    if (s_stampedFrame != g_frameJoy) {
        for (size_t i = 0; i < input::kActionCount; ++i)
            if (s_pressFrame[i] == g_frameJoy) s_pressed[i] = g_clock.t;
        s_stampedFrame = g_frameJoy;
    }
    const float elapsed = g_clock.t - s_pressed[size_t(action)];
    return elapsed >= 0 && elapsed <= dt;
}
void Consume(Action action) {
    const size_t a = size_t(action);
    // One physical press must not become a second jump or an attack after a same-frame state change.
    for (size_t i = 0; i < input::kActionCount; ++i)
        if (i == a || (s_keys[a] && profile.keyboard[a] >= 0 && profile.keyboard[i] == profile.keyboard[a]) ||
                      (s_pad[a] && profile.gamepad[a] >= 0 && profile.gamepad[i] == profile.gamepad[a]))
            ClearPress(i);
}
uint32_t Forced(const PADCTL* pad) {
    return pad && pad->plo ? pad->plo->U32Field(0, 0x154, 0) : 0;
}
uint32_t ForcedPressed(const PADCTL* pad, uint32_t mask, float dt) {
    uint32_t result = 0;
    mask &= Forced(pad);
    for (int i = 0; i < 16; ++i) {
        const float age = g_clock.t - pad->atPressed[i];
        if ((mask & (1u << i)) && age >= 0 && age <= dt) result |= 1u << i;
    }
    return result;
}

}
void Load(const std::filesystem::path& dir) {
    profile = input::load(dir);
    s_usable = false;
    s_pressed.fill(-FLT_MAX);
    s_pressFrame.fill(-1);
    s_keys.fill(false); s_pad.fill(false); s_held.fill(false);
    s_stampedFrame = -1;
}
bool Save(const std::filesystem::path& dir) { return input::save(dir, profile); }
void Sample(const std::array<bool, input::kActionCount>& keys, const std::array<bool, input::kActionCount>& pad, bool usable, bool gadgetMenu) {
    s_frame = g_frameJoy;
    s_scripted = false;
    const bool recon = ReconActive(PloPoCurrent(g_psw));
    for (size_t i = 0; i < input::kActionCount; ++i) {
        const bool held = keys[i] || pad[i];
        if (profile.enabled && keys[i] && !s_keys[i] &&
            ((usable && (!input::isNoclipAction(Action(i)) || FDebugNoclipCamera())) ||
             (gadgetMenu && input::isGadgetSlot(Action(i)))))
            cane::input::tracker().noteKeyboard();
        if (!usable || (recon && !ReconAction(Action(i)))) ClearPress(i);
        else if ((!s_held[i] && held) || (keys[i] && WheelBinding(profile.keyboard[i])))
            s_pressFrame[i] = g_frameJoy;
        s_held[i] = held;
    }
    s_keys = keys; s_pad = pad; s_usable = usable;
}
void ScriptedFrame(bool scripted) { s_scripted = scripted; }
bool Active() { return profile.enabled && s_usable && !s_scripted && s_frame == g_frameJoy; }
bool Held(Action action) {
    if (!Active() || !(g_grfjoyt & 2) || !s_held[size_t(action)]) return false;
    if (input::isNoclipAction(action)) return FDebugNoclipCamera();
    return !ReconActive(PloPoCurrent(g_psw)) || ReconAction(action);
}
int ZoomDirection()
{
    if (!Active() || !(g_grfjoyt & 2) || !PlayerZoomActive()) return 0;
    const auto down = [](Action action) {
        const size_t i = size_t(action);
        return s_pad[i] || (s_keys[i] && !WheelBinding(profile.keyboard[i]));
    };
    return int(down(Action::ZoomIn)) - int(down(Action::ZoomOut));
}
int WheelZoom(int binding, int notches, bool automaticZoom)
{
    if (!notches) return 0;
    if (!KeyboardWheelBound(binding))
        return automaticZoom ? (binding == cane::settings::kMouseWheelUp ? notches : -notches) : 0;
    if (!PlayerZoomActive()) return 0;
    const auto mapped = [&](Action action) {
        return profile.keyboard[size_t(action)] == binding && Held(action);
    };
    return notches * (int(mapped(Action::ZoomIn)) - int(mapped(Action::ZoomOut)));
}
bool Pressed(Action action, float dt) {
    if (!Active() || !(g_grfjoyt & 2) || !Recent(action, dt, PloPoCurrent(g_psw))) return false;
    Consume(action);
    return true;
}
bool Applies(const PADCTL* pad) { return Active() && pad && pad->nControl == 1 && (g_grfjoyt & 2); }

bool TankTrackAxes(float& left, float& right)
{
    if (!Active() || !(g_grfjoyt & 2) || !IsShowdownTank(PloPoCurrent(g_psw))) return false;
    const bool bothForward = Held(Action::TankBothForward), bothReverse = Held(Action::TankBothReverse);
    const auto apply = [&](float& axis, Action forward, Action reverse) {
        // An individual track can counter the shared direction for a turn.
        const bool up = Held(forward), down = Held(reverse);
        if (up || down) axis = float(int(up) - int(down));
        else if (bothForward || bothReverse) axis = float(int(bothForward) - int(bothReverse));
    };
    apply(left, Action::TankLeftForward, Action::TankLeftReverse);
    apply(right, Action::TankRightForward, Action::TankRightReverse);
    return true;
}

void BeginRecon(PADCTL* pad)
{
    if (!Applies(pad)) return;
    // Opening the view can happen after the player controller was already updated this frame.
    for (size_t i = 0; i < input::kActionCount; ++i)
        if (!ReconAction(Action(i))) ClearPress(i);
    pad->grfbtn &= Forced(pad);
    pad->x = pad->y = pad->uJoy = pad->uJoyDecay = 0.0f;
}

Action ForButton(const LO* owner, int button) {
    if (button < 0 || button >= PCA_Max) return Action::Count;
    // The Palace and dam gun scripts check held R1 to fire and track overheating.
    if (owner && FCidDerivedFrom(owner->Cid(), 0x45) && button == PCA_R1) return Action::TurretFire;
    // Tank Showdown uses a scripted PO with a single cannon on R1.
    if (IsShowdownTank(owner) && button == PCA_R1)
        return Action::VehiclePrimary;
    // Story RC missions read the chopper's PADCTL directly from their scripts.
    if (owner && owner->oid == kRcChopperOid && FCidDerivedFrom(owner->Cid(), 0x32)) {
        switch (button) {
        case PCA_Square: return Action::RcFire;
        case PCA_Cross: return Action::RcBomb;
        case PCA_R2: return Action::RcUp;
        case PCA_L2: return Action::RcDown;
        case PCA_R3: return Action::RcCamera;
        }
    }
    if (FHackMouseMode() && button == 4) return Action::HackFire;
    ACTOR* actor = owner ? dynamic_cast<ACTOR*>(PstepFromLo(const_cast<LO*>(owner))) : nullptr;
    if (actor) {
        if (actor->nF04 >= 0 && actor->nF04 < 3 && actor->nF20 == 0) {
            if (button == 15) return Action::ReconClose;
            if (button == 12) return Action::ZoomOut;
            if (button == 13) return Action::ZoomIn;
            if (button == 11) return FCidDerivedFrom(actor->palo->Cid(), 0x37) ? Action::BentleyDart : Action::Photo;
        }
        if (auto* sly = dynamic_cast<SLY*>(actor)) {
            if (button == 6 && (sly->state == 3 || sly->state == 22)) return Action::HangJump;
            if (button == 6 && (sly->state == 8 || sly->state == 21)) return Action::ClimbJump;
            if (button == 7) return sly->state == 2 ? Action::SlyDive : Action::SlyUppercut;
            if (button == 4 && sly->state == 2) return Action::SlyAirAttack;
        } else if (FCidDerivedFrom(actor->palo->Cid(), 0x37)) {
            if (button == 4) return Action::BentleySwing;
            if (button == 7) return Action::BentleySpecial;
        } else if (FCidDerivedFrom(actor->palo->Cid(), 0x36)) {
            auto* murray = dynamic_cast<MURRAY*>(actor);
            if (murray && murray->state == 5 && button == PCA_Triangle) return Action::CarrySlam;
            if (murray && murray->state == 2 && (button == 4 || button == 7)) return Action::MurrayFlop;
            if (button == 7) return Action::MurrayUppercut;
        }
    } else if (owner && FCidDerivedFrom(owner->Cid(), 0x4D)) {
        if (button == 6) return Action::VehicleDrive;
        if (button == 4) return Action::VehiclePrimary;
        if (button == 5) return Action::VehicleSecondary;
    }
    return input::legacyButtonAction(button);
}
Action VehicleWeaponAction(uint32_t buttons)
{
    return buttons & 0x80 ? Action::VehiclePrimary : Action::VehicleSecondary;
}

Action ForPromptButton(int button)
{
    LO* owner = g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (owner && FCidDerivedFrom(owner->Cid(), 0x4E)) {
        if (button == PCA_Cross || button == PCA_Square) return Action::VaultConfirm;
        if (button == PCA_Triangle || button == PCA_Circle) return Action::VaultCancel;
    }
    if (button >= 0 && button < PCA_Max) {
        if (VEHICLE* vehicle = Pvehicle(owner)) {
            for (const auto& attachment : vehicle->attachments) {
                const uint32_t buttons = attachment->heldButtons | attachment->pressedButtons;
                if (buttons & s_buttons[button]) return VehicleWeaponAction(buttons);
            }
        }
    }
    return ForButton(owner, button);
}

int Binding(Action action, int legacyButton, bool controller)
{
    if (profile.enabled && action != Action::Count)
        return (controller ? profile.gamepad : profile.keyboard)[size_t(action)];
    if (legacyButton < 0 || legacyButton >= PCA_Max) return cane::settings::kKeyUnknown;
    return (controller ? g_pcinput.gamepad : g_pcinput.keyboard)[size_t(legacyButton)];
}

int Binding(int button, bool controller, const LO* owner)
{
    if (!profile.enabled) return Binding(Action::Count, button, controller);
    if (!owner) owner = PloPoCurrent(g_psw);
    const Action action = ForButton(owner, button);
    if (ReconActive(owner) && !ReconAction(action)) return cane::settings::kKeyUnknown;
    const int binding = Binding(action, button, controller);
    if (action == Action::Count && !controller && KeyboardWheelBound(binding)) return cane::settings::kKeyUnknown;
    return binding;
}

uint32_t HeldButtons(const PADCTL* pad, uint32_t original, uint32_t forced)
{
    if (!Applies(pad)) return original;
    // The view reads its own actions directly. Exposing Dart as a held R1 here also exposes it as Sprint.
    if (ReconActive(pad->plo)) return forced;
    uint32_t result = forced;
    for (int i = 0; i < PCA_Max; ++i) {
        const Action action = ForButton(pad->plo, i);
        if (action == Action::Count ? (original & s_buttons[i]) != 0 : Held(action)) result |= s_buttons[i];
    }
    return result;
}

uint32_t PressedButtons(const PADCTL* pad, uint32_t buttons, float dt)
{
    uint32_t result = ForcedPressed(pad, buttons, dt);
    if (ReconActive(pad->plo)) return result;
    for (int i = 0; i < PCA_Max; ++i) {
        if (!(buttons & s_buttons[i])) continue;
        const Action action = ForButton(pad->plo, i);
        if (action == Action::Count) {
            const float age = g_clock.t - pad->atPressed[std::countr_zero(s_buttons[i])];
            if (age >= 0 && age <= dt) result |= s_buttons[i];
        } else if (Recent(action, dt, pad->plo)) {
            result |= s_buttons[i];
        }
    }
    return result;
}

void ForgetButtons(const PADCTL* pad, uint32_t buttons)
{
    if (ReconActive(pad->plo)) return;
    for (int i = 0; i < PCA_Max; ++i) {
        if (!(buttons & s_buttons[i])) continue;
        const Action action = ForButton(pad->plo, i);
        if (action != Action::Count) Consume(action);
    }
}

bool Within(PADCTL* pad, Action action, uint32_t original, float dt) {
    return Applies(pad) ? (Recent(action, dt, pad->plo) || ForcedPressed(pad, original, dt)) : pad && GrfPadctlPressedWithin(pad, original, dt);
}
bool Take(PADCTL* pad, Action action, uint32_t original, float dt) {
    if (!Applies(pad)) return pad && GrfPadctlTakePressed(pad, original, dt);
    if (!Recent(action, dt, pad->plo) && !ForcedPressed(pad, original, dt)) return false;
    Consume(action);
    for (int i = 0; i < 16; ++i) if (original & (1u << i)) pad->atPressed[i] = -FLT_MAX;
    return true;
}
bool Held(PADCTL* pad, Action action, uint32_t original) {
    if (!Applies(pad)) return pad && (pad->grfbtn & original);
    return (Forced(pad) & original) || ((!ReconActive(pad->plo) || ReconAction(action)) && Held(action));
}
int CaptureController(GLFWwindow*) {
    static bool armed = false;
    int found = -1;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        GLFWgamepadstate gp{};
        if (cane::input::ignoredSteamDuplicate(jid) || !glfwGetGamepadState(jid, &gp)) continue;
        for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
            if (gp.buttons[b] == GLFW_PRESS && b != GLFW_GAMEPAD_BUTTON_GUIDE) { found = b; break; }
        if (gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0) found = cane::settings::kGamepadLeftTrigger;
        if (gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0) found = cane::settings::kGamepadRightTrigger;
    }
    if (found < 0) { armed = true; return -1; }
    if (!armed) return -1;
    armed = false;
    return found;
}
} // namespace sly2actions
