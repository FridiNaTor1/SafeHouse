#include "actioninput.h"
#include "clock.h"
#include "gang.h"
#include "joy.h"
#include "lo.h"
#include "po.h"
#include "pcinput.h"
#include "pcrc.h"
#include "promptglyph.h"
#include "step.h"
#include <cfloat>
#include <cstdio>
#include <chrono>

extern int g_frameJoy;

// Runs without a disc, window or controller. Samples physical bindings through the production input path.
int sly2actions::SelfTest() {
    namespace input = cane::sly2input;
    constexpr int wheelUp = cane::settings::kMouseWheelUp;
    constexpr int wheelDown = cane::settings::kMouseWheelDown;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* message) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    const auto dir = std::filesystem::temp_directory_path() /
        ("safehouse-action-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto p = input::load(dir);
    check(!p.enabled && p.keyboard[size_t(Action::BentleyDart)] == cane::settings::kMouseBindingBase &&
          p.keyboard[size_t(Action::Sprint)] == 340,
          "first use loads the advanced preset with the mode off");
    check(p.keyboard[size_t(Action::NoclipAscend)] == 32 && p.keyboard[size_t(Action::NoclipDescend)] == 341 &&
          p.keyboard[size_t(Action::NoclipFast)] == 340 && p.keyboard[size_t(Action::NoclipSlow)] == 342,
          "the advanced preset includes the independent noclip controls");
    check(p.keyboard[size_t(Action::ZoomIn)] == wheelUp && p.keyboard[size_t(Action::ZoomOut)] == wheelDown &&
          p.gamepad[size_t(Action::ZoomIn)] == cane::settings::kGamepadRightTrigger,
          "the advanced preset includes wheel and trigger zoom bindings");
    p.enabled = true;
    p.keyboard[size_t(Action::BentleyDart)] = 71;
    p.keyboard[size_t(Action::Sprint)] = 340;
    p.gamepad[size_t(Action::BentleyDart)] = 2;
    p.gamepad[size_t(Action::Sprint)] = 5;
    p.keyboard[size_t(Action::Photo)] = -1;
    p.keyboard[size_t(Action::ZoomIn)] = wheelDown;
    p.keyboard[size_t(Action::ZoomOut)] = wheelUp;
    p.keyboard[size_t(Action::NoclipAscend)] = wheelDown;
    p.gamepad[size_t(Action::NoclipSlow)] = 3;
    check(input::save(dir, p), "save advanced profile");
    auto loaded = input::load(dir);
    check(loaded.enabled && loaded.keyboard == p.keyboard && loaded.gamepad == p.gamepad,
          "keyboard, controller and unbound actions persist independently");
    loaded.enabled = false;
    check(input::save(dir, loaded) && !input::load(dir).enabled,
          "disabling advanced controls persists");
    check(input::load(dir).keyboard[size_t(Action::BentleyDart)] == 71,
          "mode toggle retains advanced bindings");
    Load(dir);
    profile.enabled = true;
    PADCTL pad;
    pad.nControl = 1;
    g_grfjoyt = 3;
    g_frameJoy = 0;
    g_clock.t = 10;
    auto sample = [&](std::initializer_list<int> keys, std::initializer_list<int> buttons = {}, bool usable = true) {
        std::array<bool, input::kActionCount> k{}, b{};
        for (size_t i = 0; i < k.size(); ++i) {
            for (int key : keys) if (key >= 0 && profile.keyboard[i] == key) k[i] = true;
            for (int button : buttons) if (button >= 0 && profile.gamepad[i] == button) b[i] = true;
        }
        ++g_frameJoy;
        Sample(k, b, usable);
        g_clock.t += 0.016f; // The engine advances its clock after sampling input.
    };
    sample({71});
    check(Take(&pad, Action::BentleyDart, 8, 0) && !Held(Action::Sprint),
          "dart key fires without sprinting; zero-time press survives clock update");
    check(!Take(&pad, Action::BentleyDart, 8, 0), "dart press is consumed once");
    sample({71});
    check(!Take(&pad, Action::BentleyDart, 8, 0), "holding a key does not repeatedly press it");
    sample({340});
    check(Held(Action::Sprint) && !Take(&pad, Action::BentleyDart, 8, 0), "sprint key does not fire darts");
    sample({71, 340});
    check(Held(Action::Sprint) && Take(&pad, Action::BentleyDart, 8, 0), "independent actions can be used together");
    sample({}, {2});
    check(!Take(&pad, Action::BentleyDart, 8, 0), "switching devices while an action stays held does not create a second press");
    sample({});
    sample({}, {2});
    check(Take(&pad, Action::BentleyDart, 8, 0) && !Held(Action::Sprint), "controller dart is independent of sprint");
    sample({}, {5});
    check(Held(Action::Sprint) && !Take(&pad, Action::BentleyDart, 8, 0), "controller sprint is independent of dart");
    sample({});
    sample({profile.keyboard[size_t(Action::Jump)]});
    check(Take(&pad, Action::Jump, 0x40, 0) && !Take(&pad, Action::DoubleJump, 0x40, 0),
          "one shared physical press cannot jump and double jump in the same frame");
    sample({});
    sample({profile.keyboard[size_t(Action::Jump)]});
    check(Take(&pad, Action::DoubleJump, 0x40, 0), "a second press can double jump");
    sample({71}, {}, false);
    check(!Active(), "menus disable advanced gameplay input");
    sample({71});
    check(!Take(&pad, Action::BentleyDart, 8, 0), "held menu input does not become a gameplay press");
    ScriptedFrame(true);
    check(!Applies(&pad), "scripted input retains the original path");
    ScriptedFrame(false);
    pad.nControl = 0;
    check(!Applies(&pad), "AI controllers retain the original path");
    pad.nControl = 1;
    profile.enabled = false;
    pad.atPressed[3] = g_clock.t;
    check(Take(&pad, Action::BentleyDart, 8, 0), "basic mode uses original button presses");
    profile.enabled = true;
    g_grfjoyt = 0;
    check(!Applies(&pad) && !Held(Action::BentleyDart), "locked controls cannot trigger advanced actions");
    g_grfjoyt = 3;

    // Minimal native actors let us check context selection without loading a level.
    for (int cid : {0x35, 0x36, 0x37}) { g_acls[cid].cid = cid; g_acls[cid].cidLast = cid + 1; }
    ALO bentleyObject;
    bentleyObject.pcls = &g_acls[0x37];
    auto* bentley = dynamic_cast<BENTLEY*>(PstepNew(&bentleyObject));
    check(bentley != nullptr, "native Bentley created for context checks");
    if (bentley) {
        bentley->nF04 = -1;
        check(ForButton(&bentleyObject, 11) == Action::Sprint && ForButton(&bentleyObject, 4) == Action::BentleySwing,
              "Bentley ground actions resolve independently");
        pad.plo = &bentleyObject;
        SetPoForced(nullptr, &bentleyObject);
        sample({});
        sample({67});
        check(GrfPadctlTakePressed(&pad, 0x400, 0) != 0, "C opens the Binocucom by default");
        sample({profile.keyboard[size_t(Action::Jump)]});
        check(Within(&pad, Action::Jump, 0x40, 0), "a jump is pending before opening the camera");
        bentley->nF04 = 1;
        bentley->nF20 = 0;
        pad.grfbtn = 0x40;
        BeginRecon(&pad);
        check(pad.grfbtn == 0 && !Within(&pad, Action::Jump, 0x40, 0.25f),
              "opening the camera immediately clears pending gameplay input");
        check(!Within(&pad, Action::ReconClose, 0x400, 0.25f),
              "the C press that opened the Binocucom does not close it");
        check(ForButton(&bentleyObject, PCA_R1) == Action::BentleyDart && Binding(PCA_R1, false) == 71,
              "Bentley aiming resolves R1 to the dart binding");
        check(Binding(PCA_Cross, false) == -1 && Binding(PCA_Select, false) == -1 && Binding(PCA_L1, true) == -1,
              "the camera masks jump, gadget menu and gadget slot input before it reaches the game");
        sample({});
        sample({71, profile.keyboard[size_t(Action::Jump)], profile.keyboard[size_t(Action::GadgetMenu)]});
        check(HeldButtons(&pad, 0xC8, 0) == 0 && !GrfPadctlPressedWithin(&pad, 0xC8, 0),
              "camera input does not appear as regular held or pressed player buttons");
        check(!Pressed(Action::GadgetMenu) && Take(&pad, Action::BentleyDart, 8, 0),
              "dart fire remains available while gadget menu input is blocked");
        sample({wheelUp});
        check(WheelZoom(wheelUp, 2, true) == -2 && ZoomDirection() == 0,
              "mapped Wheel Up zooms out by two notches instead of triggering automatic zoom or held zoom");
        sample({wheelDown});
        check(WheelZoom(wheelDown, 1, false) == 1, "mapped Wheel Down zooms in with automatic zoom disabled");
        profile.keyboard[size_t(Action::ZoomIn)] = 90;
        profile.keyboard[size_t(Action::Jump)] = wheelUp;
        sample({90, wheelUp});
        check(ZoomDirection() == 1 && !Held(Action::Jump), "a held zoom key works while gameplay actions stay blocked");
        sample({}, {profile.gamepad[size_t(Action::ZoomOut)]});
        check(ZoomDirection() == -1, "a mapped controller button can hold zoom");
        profile.keyboard[size_t(Action::ZoomOut)] = 88;
        sample({wheelUp});
        check(WheelZoom(wheelUp, 1, true) == 0 && WheelZoom(wheelDown, 1, true) == -1,
              "gameplay wheel bindings suppress automatic zoom for their direction; noclip bindings do not");
        profile.keyboard[size_t(Action::Jump)] = p.keyboard[size_t(Action::Jump)];
        sample({profile.keyboard[size_t(Action::Jump)], 67});
        check(Take(&pad, Action::ReconClose, 0x400, 0), "a fresh C press closes the Binocucom by default");
        bentley->nF04 = -1;
        sample({profile.keyboard[size_t(Action::Jump)]});
        check(!Within(&pad, Action::Jump, 0x40, 0.25f), "jump presses during the camera view do not carry over on exit");
        sample({});
        sample({profile.keyboard[size_t(Action::Jump)]});
        check(Take(&pad, Action::Jump, 0x40, 0), "jump works on a fresh press after closing the camera");
        SetPoForced(nullptr, nullptr);
        pad.plo = nullptr;
    }
    g_pcinput.promptStyle = int(cane::input::PromptStyle::PlayStation);
    check(PromptGlyphFor('R').ch == 'R', "ordinary R1 prompt shows the sprint binding");
    {
        const PROMPTTEXTGLYPHS dartCue(1465);
        const auto glyph = PromptGlyphFor('R');
        check(glyph.kind == PROMPTGLYPH::Glyph && glyph.ch == 'S', "dart tutorial shows the remapped Square icon");
        g_pcinput.fAlwaysOriginalPs = true;
        check(PromptGlyphFor('R').ch == 'R', "original icon preference keeps the retail prompt");
        g_pcinput.fAlwaysOriginalPs = false;
        profile.gamepad[size_t(Action::BentleyDart)] = -1;
        check(StrPromptFallbackText("Fire &2R&.").find("[Unbound]") != std::string::npos,
              "an unbound action displays its status in place of the old button icon");
        profile.gamepad[size_t(Action::BentleyDart)] = 2;
    }
    check(PromptGlyphFor('R').ch == 'R', "tutorial mapping is scoped to its own prompt");
    {
        const PROMPTTEXTGLYPHS turretCue(1594);
        check(StrPromptButtonWords("Hold R1 to fire", 1594) == "Hold &2R&. to fire" &&
              PromptGlyphFor('R').ch == 'S', "turret briefing shows the mapped fire button");
        check(StrPromptButtonWords("Hold R1", 1593) == "Hold R1", "other authored R1 text is unchanged");
        check(StrPromptButtonWords("Tryk på L3 tasten", -1) == "Tryk på &2{&. tasten" &&
              StrPromptButtonWords("L3-Taste, R3-Taste", -1) == "&2{&.-Taste, &2}&.-Taste" &&
              StrPromptButtonWords("AL3 L33 &2L&.", -1) == "AL3 L33 &2L&.", "the words L3 / R3 become stick-press glyphs");
    }
    LO forcedOwner;
    forcedOwner.SetField(0, 0x154, 8);
    pad.plo = &forcedOwner;
    sample({});
    pad.grfbtnPrev = 0;
    pad.grfbtn = HeldButtons(&pad, 0, 8);
    StampPadctl(&pad);
    check((pad.grfbtn & 8) && Take(&pad, Action::BentleyDart, 8, 0), "script-forced held and pressed buttons survive advanced mode");
    check(!Take(&pad, Action::BentleyDart, 8, 0), "forced presses are consumed once");
    pad.plo = nullptr;
    sample({});
    check(Binding(PCA_Triangle, false) == g_pcinput.keyboard[PCA_Triangle],
          "unclassified Triangle uses the basic keyboard binding in advanced mode");
    const int basicTriangle = g_pcinput.keyboard[PCA_Triangle];
    g_pcinput.keyboard[PCA_Triangle] = wheelUp;
    profile.keyboard[size_t(Action::Jump)] = wheelUp;
    check(Binding(PCA_Triangle, false) == -1, "a wheel action does not also activate a basic fallback binding");
    sample({wheelUp});
    check(Take(&pad, Action::Jump, 0x40, 0), "wheel binding produces a press");
    sample({wheelUp});
    check(Take(&pad, Action::Jump, 0x40, 0), "scrolling again on the next frame produces another press");
    sample({wheelDown});
    check(!Held(Action::NoclipAscend), "noclip bindings are inactive during normal gameplay");
    g_pcinput.keyboard[PCA_Triangle] = basicTriangle;
    pad.grfbtnPrev = 0;
    pad.grfbtn = HeldButtons(&pad, 0x10, 0);
    StampPadctl(&pad);
    check((pad.grfbtn & 0x10) && GrfPadctlTakePressed(&pad, 0x10, 0) && !GrfPadctlTakePressed(&pad, 0x10, 0),
          "the basic Triangle press remains usable and is consumed once");
    check(VehicleWeaponAction(0x80) == Action::VehiclePrimary && VehicleWeaponAction(0x0A) == Action::VehicleSecondary,
          "RC tank turret and flame burst use the primary and secondary bindings");
    LO turret;
    g_acls[0x45].cid = 0x45;
    g_acls[0x45].cidLast = 0x46;
    turret.pcls = &g_acls[0x45];
    pad.plo = &turret;
    const int fireKey = profile.keyboard[size_t(Action::TurretFire)];
    const int sprintKey = profile.keyboard[size_t(Action::Sprint)];
    check(Binding(PCA_R1, false, &turret) == fireKey, "turret R1 projects the fire binding into raw script input");
    sample({fireKey});
    check((HeldButtons(&pad, 0, 0) & 8) != 0, "turret fire reaches scripts that check held R1");
    sample({fireKey});
    check((HeldButtons(&pad, 0, 0) & 8) != 0, "holding turret fire continues firing across frames");
    sample({sprintKey});
    check((HeldButtons(&pad, 0, 0) & 8) == 0, "sprint does not fire the turret");
    SetPoForced(nullptr, &turret);
    profile.keyboard[size_t(Action::ZoomIn)] = wheelDown;
    profile.keyboard[size_t(Action::ZoomOut)] = wheelUp;
    sample({wheelUp});
    check(WheelZoom(wheelUp, 2, true) == -2 && ZoomDirection() == 0,
          "turret zoom follows the shared wheel binding and overrides automatic zoom");
    sample({wheelDown});
    check(WheelZoom(wheelDown, 1, false) == 1, "turret wheel binding works with automatic zoom disabled");
    sample({}, {profile.gamepad[size_t(Action::ZoomIn)]});
    check(ZoomDirection() == 1, "turret shares the controller zoom binding with the Binocucom");
    SetPoForced(nullptr, nullptr);
    LO chopper;
    g_acls[0x32].cid = 0x32;
    g_acls[0x32].cidLast = 0x33;
    chopper.pcls = &g_acls[0x32];
    chopper.oid = kRcChopperOid;
    pad.plo = &chopper;
    profile.keyboard[size_t(Action::RcBomb)] = 72;
    profile.keyboard[size_t(Action::RcFire)] = 74;
    profile.gamepad[size_t(Action::RcBomb)] = 3;
    check(Binding(PCA_Cross, false, &chopper) == 72 && Binding(PCA_Square, false, &chopper) == 74,
          "story chopper raw inputs resolve to RC bomb and gun bindings");
    sample({72});
    check((HeldButtons(&pad, 0, 0) & 0xC0) == 0x40 && GrfPadctlTakePressed(&pad, 0x40, 0),
          "the RC bomb binding reaches the script as held and pressed Cross");
    sample({74});
    check((HeldButtons(&pad, 0, 0) & 0xC0) == 0x80, "the RC gun binding reaches the script as held Square");
    sample({}, {3});
    check((HeldButtons(&pad, 0, 0) & 0xC0) == 0x40, "remapped controller bombs reach story scripts");
    check(ForButton(&chopper, PCA_R2) == Action::RcUp && ForButton(&chopper, PCA_L2) == Action::RcDown &&
          ForButton(&chopper, PCA_R3) == Action::RcCamera, "the chopper uses its own flight and camera bindings");
    {
        const PROMPTTEXTGLYPHS briefing(1676);
        check(PromptGlyphFor('X').ch == 'T', "the RC bombing briefing shows the remapped bomb button");
    }
    profile.enabled = false;
    check(Binding(PCA_Cross, false, &chopper) == g_pcinput.keyboard[PCA_Cross],
          "basic mode retains the original RC input");
    profile.enabled = true;
    LO showdownTank;
    showdownTank.pcls = &g_acls[0x32];
    showdownTank.oid = 72886;
    pad.plo = &showdownTank;
    profile.keyboard[size_t(Action::VehiclePrimary)] = 76;
    profile.gamepad[size_t(Action::VehiclePrimary)] = 3;
    sample({76});
    check(Binding(PCA_R1, false, &showdownTank) == 76 && (HeldButtons(&pad, 0, 0) & 8) != 0,
          "Tank Showdown projects the primary weapon binding onto its scripted cannon input");
    sample({sprintKey});
    check((HeldButtons(&pad, 0, 0) & 8) == 0, "sprint does not fire the Tank Showdown cannon");
    {
        const PROMPTTEXTGLYPHS briefing(2268);
        check(StrPromptButtonWords("Tap R1", 2268) == "Tap &2R&." && PromptGlyphFor('R').ch == 'T',
              "the Tank Showdown briefing shows the remapped primary weapon button");
    }
    float left = 0.0f, right = 0.0f;
    SetPoForced(nullptr, &showdownTank);
    sample({81, 69});
    check(TankTrackAxes(left, right) && left == 1.0f && right == 1.0f, "Q and E drive both tracks forward");
    sample({65, 68});
    check(TankTrackAxes(left, right) && left == -1.0f && right == -1.0f, "A and D reverse both tracks");
    sample({87});
    check(TankTrackAxes(left, right) && left == 1.0f && right == 1.0f, "W alone drives both tracks forward");
    sample({83});
    check(TankTrackAxes(left, right) && left == -1.0f && right == -1.0f, "S alone reverses both tracks");
    sample({87, 83});
    check(TankTrackAxes(left, right) && left == 0.0f && right == 0.0f, "opposing shared inputs cancel");
    sample({87, 65});
    check(TankTrackAxes(left, right) && left == -1.0f && right == 1.0f, "an individual track can override the shared input");
    profile.keyboard[size_t(Action::TankBothForward)] = 88;
    sample({88});
    check(TankTrackAxes(left, right) && left == 1.0f && right == 1.0f, "the shared forward input can be remapped");
    profile.keyboard[size_t(Action::TankLeftForward)] = 90;
    profile.gamepad[size_t(Action::TankRightReverse)] = 3;
    sample({90}, {3});
    check(TankTrackAxes(left, right) && left == 1.0f && right == -1.0f, "remapped tracks can drive in opposite directions");
    left = right = 0.5f;
    sample({90, 65});
    check(TankTrackAxes(left, right) && left == 0.0f && right == 0.5f,
          "opposing binds cancel on one track while the other keeps its analog input");
    profile.enabled = false;
    check(!TankTrackAxes(left, right), "basic mode retains the original tank controls");
    profile.enabled = true;
    SetPoForced(nullptr, nullptr);
    check(!TankTrackAxes(left, right), "tank track bindings only apply to the tank");
    pad.plo = nullptr;
    {
        std::ofstream out(input::profilePath(dir));
        out << R"({"version":1,"enabled":true,"actions":{"sprint":{"keyboard":70}}})";
    }
    const auto partial = input::load(dir);
    check(partial.enabled && partial.keyboard[size_t(Action::Sprint)] == 70 &&
          partial.keyboard[size_t(Action::ZoomIn)] == wheelUp && partial.gamepad[size_t(Action::Sprint)] == 5 &&
          partial.keyboard[size_t(Action::TankLeftForward)] == 81,
          "partial profiles retain saved overrides and use the preset for missing bindings");
    {
        std::ofstream out(input::profilePath(dir));
        out << R"({"version":999,"enabled":true})";
    }
    check(input::load(dir).readOnly && !input::load(dir).enabled,
          "future profile versions are preserved and use basic controls");
    {
        std::ofstream out(input::profilePath(dir));
        out << "invalid json";
    }
    check(input::load(dir).readOnly && !input::load(dir).enabled,
          "damaged profiles safely fall back without overwriting the file");
    std::filesystem::remove_all(dir);
    ResetSteps();
    std::printf("Action input: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
