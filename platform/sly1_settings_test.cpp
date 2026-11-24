// Sly 1's Settings.ini (platform/sly1_settings.h over sly2_settings.h's reader / writer) without the game: the defaults
// (Sly 1's keyboard layout) and changed values round trip, Sly 2's own keys are not in Sly 1's file and Sly 1's own are
// not in Sly 2's or Sly 3's, a hand-edited file, the migration from the old Saves/system_settings.dat (v22 and an old
// version), the atomic write, unknown keys kept.
#include "sly1_settings.h"
#include "sly1_action_bindings.h"
#include "sly3_settings.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace st = cane::settings;
namespace fs = std::filesystem;
int s_checks = 0, s_failures = 0;
void check(bool ok, const char* what, const std::string& detail = {}) {
    ++s_checks;
    if (ok) return;
    ++s_failures;
    std::printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
}
bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }
} // namespace

int main(int argc, char** argv) {
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "cane-sly1-settings-test";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    // defaults: the keyboard layout, the sections, no Sly 2-only key
    const st::Sly1Settings d = st::sly1Defaults();
    const std::string td = st::sly1ToIni(d);
    check(has(td, "Sly Cooper and the Thievius Raccoonus"), "the title line");
    check(d.system.keyboardBindings[4] == st::kSly2DefaultKeyboardBindings[4], "Sly 2's keyboard layout by default");
    check(d.system.keyboardBindings[10] == st::kMouseBindingBase + 1 && d.system.keyboardBindings[15] == st::kMouseBindingBase + 2,
          "the binocucom (L1) on the right mouse button as Sly 2's, the camera centre (R3) on the middle one");
    check(has(td, "L1 = Mouse Right") && has(td, "Layout Revision = 2"), "the new layout written with its revision");
    for (const char* section : {"[Display]", "[Graphics]", "[Audio]", "[Controls]", "[Controller]", "[Mouse]", "[Keyboard]", "[Game]", "[SafeHouse]"})
        check(has(td, section), "a section", section);
    for (const char* absent : {"[VR]", "[Language]", "Invert Binocucom", "Invert Turret", "Tank Left Track", "Mouse Aim In Hacking",
                               "Upscaling", "Long Shadows", "Fast Shadows", "Outline Thickness", "Texture Cache",
                               "USB Headset", "Input Device", "Output Device ID", "Pending Save Options", "Surround", "Invert Camera Horizontal = Off\n; ",
                               "3D Mode", "Dialogue Skip"})
        check(!has(td, absent), "a Sly 2 / Sly 3 key absent", absent);
    for (const char* present : {"Window Mode = Windowed", "VSync = Off", "Monitor = Primary", "Frame Rate Limit = 60",
                                "Minimize When Unfocused = Off", "Internal Resolution = Native", "Resolution Scale = 100",
                                "Anti-Aliasing = Off", "Draw Distance = 1.00", "Fog = PS2", "Brightness = 100", "Texture Filtering = Trilinear",
                                "Anisotropic Filtering = Off", "GUI Style = PS2", "GUI Scale = 100", "Renderer = Auto", "Speakers = Stereo",
                                "Output Device = System Default", "Vibration = On", "Button Icons = Automatic",
                                "Instant Skip Cutscenes = Off", "Dead Zone = 35", "Mouse Camera = On",
                                "Square = Mouse Left", "Last Save Slot = None", "HUD Placement = 4:3"})
        check(has(td, present), "a key present", present);
    check(has(td, "hold it 2 seconds"), "the cutscene skip's 2 s hold in the comment");
    // Sly 1's own keys stay out of Sly 2's and Sly 3's files
    for (const std::string& other : {st::sly2ToIni(st::Sly2Settings{}), st::sly3ToIni(st::Sly3Settings{})})
        for (const char* absent : {"Resolution Scale", "Brightness", "Texture Filtering", "Dead Zone", "Layout Revision", "[Game]",
                                   "Minimize When Unfocused", "GUI Style", "Fog ="})
            check(!has(other, absent), "a Sly 1 key absent from Sly 2 / Sly 3", absent);
    check(st::sly2FromIni(st::sly2ToIni(st::Sly2Settings{})).system.keyboardBindings[4] == st::kSly2DefaultKeyboardBindings[4],
          "Sly 2 keeps its keyboard layout");

    // a round trip of changed values
    st::Sly1Settings c = d;
    c.system.frameRate = 1000;
    c.system.vsyncEnabled = 1;
    c.system.internalResolutionHeight = st::kSly1Ps2Height;
    c.system.msaaEnabled = 1, c.system.msaaSamples = 8;
    c.system.fogType = st::Fog_PS3;
    c.system.anisotropy = 16;
    c.system.guiStyle = st::Gui_Modern, c.system.guiScale = 1.25f;
    c.system.stereoEnabled = 0;
    c.system.musicVolume = 0.25f;
    c.system.instantSkipCutscenes = 1;
    c.system.lastSaveSlot = 3;
    c.hudPlacement = st::HudPlacement_Match;
    c.system.keyboardBindings[6] = 32;
    cane::monitor::store(c.system.monitor, std::string("DELL U2720Q | 2 | 2560,0"));
    c.sly1.autoIconify = 1;
    c.sly1.gamma = 1.2f;
    c.sly1.textureFiltering = 0;
    c.sly1.resolutionScale = 150;
    c.sly1.deadZone = 0.2f;
    c.game.outputDevice = "Speakers (Realtek(R) Audio) ; #2";
    std::vector<std::string> invalid;
    const std::string tc = st::sly1ToIni(c);
    const st::Sly1Settings back = st::sly1FromIni(tc, &invalid);
    check(invalid.empty(), "no invalid keys", invalid.empty() ? "" : invalid[0]);
    check(has(tc, "Frame Rate Limit = Uncapped") && back.system.frameRate == 1000, "Uncapped");
    check(back.system.vsyncEnabled == 1, "VSync");
    check(back.system.internalResolutionHeight == st::kSly1Ps2Height, "the PS2's 448 lines kept");
    check(back.system.msaaEnabled == 1 && back.system.msaaSamples == 8, "MSAA");
    check(back.system.fogType == st::Fog_PS3 && back.system.anisotropy == 16, "fog, anisotropy");
    check(back.system.guiStyle == st::Gui_Modern && back.system.guiScale == 1.25f, "GUI style and scale");
    check(back.system.stereoEnabled == 0 && back.system.musicVolume == 0.25f, "speakers, music");
    check(back.system.instantSkipCutscenes == 1 && back.system.lastSaveSlot == 3, "instant skip, the save slot");
    check(back.system.keyboardBindings[6] == 32, "a key binding");
    check(has(tc, "HUD Placement = Match Screen") && back.hudPlacement == st::HudPlacement_Match, "HUD Placement");
    check(cane::monitor::load(back.system.monitor) == "DELL U2720Q | 2 | 2560,0", "the monitor");
    check(back.sly1.autoIconify == 1 && back.sly1.textureFiltering == 0 && back.sly1.resolutionScale == 150,
          "Sly 1's own switches");
    check(std::fabs(back.sly1.gamma - 1.2f) < 1e-4f && std::fabs(back.sly1.deadZone - 0.2f) < 1e-4f, "brightness, dead zone");
    check(back.game.outputDevice == c.game.outputDevice && has(tc, "Output Device = \"Speakers (Realtek(R) Audio) ; #2\""),
          "the output device by name, quoted (Sly 2's key)", back.game.outputDevice);
    check(st::sly1ToIni(back) == tc, "a second round trip is identical");
    {
        // the old Windows-only Output Device ID: kept for the game (which turns it into a name) until then
        const std::string old = "[Audio]\nOutput Device ID = \"{0.0.0.00000000}.{a-b ; c}\"\n";
        const st::Sly1Settings o = st::sly1FromIni(old);
        check(o.sly1.audioDeviceId == "{0.0.0.00000000}.{a-b ; c}" && o.game.outputDevice.empty(), "the old device id read",
              o.sly1.audioDeviceId);
        check(has(st::sly1ToIni(o), "Output Device ID = "), "the old device id kept until the game converts it");
        const st::Sly1Settings d0 = st::sly1FromIni("[Audio]\nOutput Device ID = System Default\n");
        check(d0.sly1.audioDeviceId.empty() && !has(st::sly1ToIni(d0), "Output Device ID"), "an old default id dropped");
        const st::Sly1Settings both = st::sly1FromIni(old + "Output Device = \"USB Audio\"\n");
        check(both.sly1.audioDeviceId.empty() && both.game.outputDevice == "USB Audio" && !has(st::sly1ToIni(both), "Output Device ID"),
              "the old id dropped once a name is set");
    }

    // a hand-edited file: other spellings, comments, bad values (defaults), unknown keys kept
    const std::string hand = "; mine\n[display]\nframe_rate_limit = 144\nvsync=on\n[GRAPHICS]\nInternal Resolution = 448p\n"
                             "Brightness = 300\nGUI Scale = 75%\nGUI Style = PS2\n[Controller]\nDead Zone = 10 ; low\n"
                             "[Game]\nLast Save Slot = 9\n[Mods]\nFoo = Bar\n[Graphics]\nMy Key = 1\n";
    invalid.clear();
    const st::Sly1Settings h = st::sly1FromIni(hand, &invalid);
    check(h.system.frameRate == 144 && h.system.vsyncEnabled == 1, "the hand-edited display keys");
    check(h.system.internalResolutionHeight == st::kSly1Ps2Height, "448p");
    check(h.sly1.gamma == 1.0f, "a bad brightness takes the default");
    check(h.system.guiScale == 0.75f, "GUI scale 75 % with the PS2 style");
    check(std::fabs(h.sly1.deadZone - 0.1f) < 1e-4f, "the dead zone");
    check(h.system.lastSaveSlot == -1, "a bad save slot takes the default");
    check(invalid.size() == 2, "two invalid keys", std::to_string(invalid.size()));
    const std::string th = st::sly1ToIni(h);
    check(has(th, "[Mods]\nFoo = Bar") && has(th, "My Key = 1"), "unknown keys and sections kept");

    // a file of the older layout (no Layout Revision): the keys still on its defaults move to the new ones, a changed
    // one stays; the removed Dialogue Skip turns Instant Skip Cutscenes on and is gone from the file
    {
        st::Sly1Settings o = st::sly1Defaults();
        std::copy(std::begin(st::kSly2DefaultKeyboardBindings), std::end(st::kSly2DefaultKeyboardBindings), o.system.keyboardBindings);
        std::string text = st::sly1ToIni(o);
        const size_t rev = text.find("Layout Revision = 2\n");
        check(rev != std::string::npos, "the revision line");
        text.erase(rev, std::strlen("Layout Revision = 2\n"));
        text += "[Controls]\nDialogue Skip = On\n";
        const st::Sly1Settings up = st::sly1FromIni(text);
        check(up.system.keyboardBindings[10] == st::kMouseBindingBase + 1 && up.system.keyboardBindings[15] == st::kMouseBindingBase + 2,
              "revision 1's untouched L1 / R3 moved to the new defaults");
        check(up.system.instantSkipCutscenes == 1, "Dialogue Skip On: Instant Skip Cutscenes on");
        const std::string upText = st::sly1ToIni(up);
        check(!has(upText, "Dialogue Skip") && has(upText, "Layout Revision = 2"), "Dialogue Skip gone, the revision written");
        // changed by the player: L1 on F, R3 left on the right mouse button with L1 elsewhere moves; a key already used
        // by another binding is never taken
        o.system.keyboardBindings[10] = 'F';
        o.system.keyboardBindings[0] = st::kMouseBindingBase + 2;   // (Move Forward on the middle button)
        text = st::sly1ToIni(o);
        text.erase(text.find("Layout Revision = 2\n"), std::strlen("Layout Revision = 2\n"));
        const st::Sly1Settings kept = st::sly1FromIni(text);
        check(kept.system.keyboardBindings[10] == 'F', "a changed L1 kept");
        check(kept.system.keyboardBindings[15] == st::kMouseBindingBase + 1, "R3 not moved onto a key another binding uses");
        // revision 2: nothing moves (a player's own L1 = 1 after the update stays)
        o = st::sly1Defaults();
        o.system.keyboardBindings[10] = '1';
        check(st::sly1FromIni(st::sly1ToIni(o)).system.keyboardBindings[10] == '1', "a revision 2 file is left alone");
        st::Sly1Settings off = st::sly1FromIni("[Controls]\nDialogue Skip = Off\n");
        check(off.system.instantSkipCutscenes == 0, "Dialogue Skip Off: Instant Skip Cutscenes stays off");
    }

    // the file: migration from the old binary file (v22, then an old v12), the INI written, the old file left
    const fs::path user = work / "user";
    st::SystemSettings old = st::defaults();
    old.frameRate = 120;
    old.vsyncEnabled = 1;
    old.musicVolume = 0.5f;
    old.keyboardBindings[0] = 'Z';
    old.instantSkipCutscenes = 1;
    old.lastSaveSlot = 2;
    old.internalResolutionHeight = 720;
    check(st::save(st::settingsPath(user), old), "the old file written");
    st::Sly1Settings loaded;
    check(st::loadSly1(user, loaded) == st::Sly2Load::Migrated, "migrated from system_settings.dat");
    check(loaded.system.frameRate == 120 && loaded.system.vsyncEnabled == 1 && loaded.system.musicVolume == 0.5f &&
              loaded.system.keyboardBindings[0] == 'Z' && loaded.system.instantSkipCutscenes == 1 && loaded.system.lastSaveSlot == 2 &&
              loaded.system.internalResolutionHeight == 720,
          "the old values");
    check(loaded.pendingSaveOptions.empty(), "no pending save options for Sly 1");
    check(loaded.sly1.gamma == 1.0f && loaded.sly1.deadZone == 0.35f, "Sly 1's own options at their defaults");
    check(st::saveSly1(user, loaded), "the INI written");
    check(fs::exists(st::sly1SettingsPath(user)) && fs::exists(st::settingsPath(user)), "both files there");
    check(!fs::exists(fs::path(st::sly1SettingsPath(user)) += ".tmp"), "no temporary file left");
    st::Sly1Settings again;
    check(st::loadSly1(user, again) == st::Sly2Load::Ok && again.system.frameRate == 120, "the INI read back (it wins)");
    const fs::path user12 = work / "user12";
    st::SystemSettings v12 = st::defaults();
    v12.frameRate = 30;
    check(st::save(st::settingsPath(user12), v12, 12), "a v12 file written");
    st::Sly1Settings from12;
    check(st::loadSly1(user12, from12) == st::Sly2Load::Migrated && from12.system.frameRate == 30 &&
              from12.system.keyboardBindings[4] == st::kDefaultKeyboardBindings[4],
          "migrated from v12 with Sly 1's keyboard");
    st::Sly1Settings none;
    check(st::loadSly1(work / "nobody", none) == st::Sly2Load::Missing && none.system.keyboardBindings[10] == st::kSly1DefaultKeyboardBindings[10],
          "no file: Sly 1's defaults");

    // the advanced input mapping (sly1_action_bindings.h): its defaults follow the new layout; a revision 1 profile's
    // keys still on revision 1's defaults move, a changed one stays, the file is written as revision 2
    {
        namespace in = cane::sly1input;
        const fs::path adv = work / "advanced";
        const auto key = [](const in::Profile& p, in::Action a) { return p.keyboard[size_t(a)]; };
        check(key(in::defaults(), in::Action::Bentley) == st::kMouseBindingBase + 1 &&
                  key(in::defaults(), in::Action::CameraCenter) == st::kMouseBindingBase + 2,
              "the advanced mapping's defaults: the binocucom on the right mouse button");
        fs::create_directories(in::profilePath(adv).parent_path(), ec);
        std::ofstream(in::profilePath(adv)) << R"({"version": 1, "enabled": true, "actions": {"bentley": {"keyboard": 49, "controller": 4},)"
                                               R"( "camera_center": {"keyboard": 2001, "controller": 10}, "jump": {"keyboard": 74, "controller": 0}}})";
        in::Profile p = in::load(adv);
        check(p.enabled && key(p, in::Action::Bentley) == st::kMouseBindingBase + 1 && key(p, in::Action::CameraCenter) == st::kMouseBindingBase + 2,
              "a revision 1 profile's untouched keys moved");
        check(key(p, in::Action::Jump) == 74, "a changed key kept");
        check(in::save(adv, p), "the profile written");
        p.keyboard[size_t(in::Action::Bentley)] = 49;
        check(in::save(adv, p) && key(in::load(adv), in::Action::Bentley) == 49, "a revision 2 profile is left alone");
        std::ofstream(in::profilePath(adv)) << R"({"version": 1, "actions": {"bentley": {"keyboard": 49}, "jump": {"keyboard": 2001}}})";
        check(key(in::load(adv), in::Action::Bentley) == 49, "never onto a key another action uses");
    }

    // the rhythm minigame's own keys: W, A, D, Space by default, round trip, None, no mouse buttons; the advanced
    // mapping's rhythm actions default to them and a revision 2 profile's still on the button's standard key move
    {
        check(d.sly1.rhythmKeys[0] == 87 && d.sly1.rhythmKeys[1] == 65 && d.sly1.rhythmKeys[2] == 68 && d.sly1.rhythmKeys[3] == 32,
              "the rhythm keys' defaults (W, A, D, Space)");
        check(has(td, "Rhythm Triangle = W") && has(td, "Rhythm Cross = Space"), "the rhythm keys in the file", td);
        st::Sly1Settings r = d;
        r.sly1.rhythmKeys[1] = 74;   // J
        r.sly1.rhythmKeys[3] = st::kKeyUnknown;
        const st::Sly1Settings rb = st::sly1FromIni(st::sly1ToIni(r));
        check(rb.sly1.rhythmKeys[1] == 74 && rb.sly1.rhythmKeys[3] == st::kKeyUnknown && rb.sly1.rhythmKeys[0] == 87,
              "the rhythm keys round trip (None too)");
        std::vector<std::string> bad;
        const st::Sly1Settings rm = st::sly1FromIni("[Keyboard]\nRhythm Square = Mouse Left\n", &bad);
        check(rm.sly1.rhythmKeys[1] == 65, "no mouse button on a rhythm key");
        check(st::sly1FromIni("[Keyboard]\nRhythm Circle = Q\n").sly1.rhythmKeys[2] == 81, "a rhythm key read");
        namespace in = cane::sly1input;
        const auto key = [](const in::Profile& p, in::Action a) { return p.keyboard[size_t(a)]; };
        check(key(in::defaults(), in::Action::RhythmTriangle) == 87 && key(in::defaults(), in::Action::RhythmSquare) == 65 &&
                  key(in::defaults(), in::Action::RhythmCircle) == 68 && key(in::defaults(), in::Action::RhythmCross) == 32,
              "the advanced rhythm actions' defaults");
        check(std::string(in::kActions[size_t(in::Action::RhythmCross)].group) == "Rhythm minigame", "the rhythm actions' own group");
        const fs::path adv = work / "advanced-rhythm";
        fs::create_directories(in::profilePath(adv).parent_path(), ec);
        std::ofstream(in::profilePath(adv)) << R"({"version": 1, "revision": 2, "actions": {"rhythm_square": {"keyboard": 2000},)"
                                               R"( "rhythm_triangle": {"keyboard": 81}, "rhythm_circle": {"keyboard": 76}}})";
        const in::Profile p = in::load(adv);
        check(key(p, in::Action::RhythmSquare) == 65 && key(p, in::Action::RhythmTriangle) == 87, "revision 2's rhythm keys moved to W / A");
        check(key(p, in::Action::RhythmCircle) == 76, "a changed rhythm key kept");
    }

    fs::remove_all(work, ec);
    std::printf("sly1-settings: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
