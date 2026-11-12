// --settings-selftest: Settings.ini (platform/sly2_settings.h, platform/settings_ini.h) without a disc or a window: the
// round trip of every key, hand-edited files (comments, CRLF, a BOM, other spellings, invalid values, unknown keys),
// the migration from every Saves/system_settings.dat version and from a save's options, and the atomic write.
#include "../platform/sly2_settings.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

int SettingsSelfTest();

namespace
{
namespace st = cane::settings;
int s_checks = 0, s_failures = 0;
void Check(bool ok, const char* what, const std::string& detail = {})
{
    ++s_checks;
    if (ok) return;
    ++s_failures;
    std::printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
}
bool SameSystem(const st::SystemSettings& a, const st::SystemSettings& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
bool SameGame(const st::Sly2GameOptions& a, const st::Sly2GameOptions& b)
{
    return a.invertBinocucom == b.invertBinocucom && a.invertTurret == b.invertTurret && a.speakers == b.speakers &&
           a.outputDevice == b.outputDevice && a.inputDevice == b.inputDevice && a.usbHeadset == b.usbHeadset &&
           a.headsetOutputDevice == b.headsetOutputDevice;
}
std::string FirstDifference(const st::Sly2Settings& a, const st::Sly2Settings& b)
{
    const std::string ta = st::sly2ToIni(a), tb = st::sly2ToIni(b);
    size_t i = 0;
    while (i < ta.size() && i < tb.size() && ta[i] == tb[i]) ++i;
    const size_t line = ta.rfind('\n', i);
    return ta.substr(line == std::string::npos ? 0 : line + 1, 60) + " | " + tb.substr(tb.rfind('\n', i) == std::string::npos ? 0 : tb.rfind('\n', i) + 1, 60);
}

st::Sly2Settings Custom()
{
    st::Sly2Settings s;
    auto& y = s.system;
    y.windowMode = st::Window_Borderless;
    y.vsyncEnabled = 1;
    y.aspectMode = st::Aspect_PS2_16_9;
    y.frameRate = 1000;
    y.internalResolutionHeight = 720;
    y.msaaEnabled = 1;
    y.msaaSamples = 8;
    y.drawDistance = 1.5f;
    y.longShadows = 0;
    y.fastShadows = 1;   // (v20, perf-5)
    y.renderer = 1;      // (v21, Vulkan)
    y.musicVolume = 37 / 100.0f;
    y.soundEffectsVolume = 0.0f;
    y.dialogueVolume = 80 / 100.0f;
    y.controllerVibration = 0;
    y.promptStyle = st::Prompt_SteamController2026;
    y.alwaysOriginalPsPrompts = 1;
    y.instantSkipCutscenes = 1;
    y.autoCameraRotation = 0;
    y.disableControllerInputWhenUnfocused = 1;
    y.padInvertCameraX = 1;
    y.padInvertCameraY = 1;
    y.mouseCamera = 0;
    y.mouseSensitivity = 250;
    y.mouseAimSensitivity = 10;
    y.mouseInvertX = y.mouseInvertBinocY = y.mouseInvertTurretX = 1;
    y.mouseRawInput = 0;
    y.mouseWheelZoom = 0;
    y.mouseMenus = 0;
    y.hackMouseAim = 0;
    y.keyboardBindings[0] = 265;                         // Up
    y.keyboardBindings[4] = st::kMouseWheelDown;
    y.keyboardBindings[5] = st::kMouseBindingBase + 4;   // Mouse 5
    y.keyboardBindings[6] = 320 + 7;                     // Keypad 7
    y.keyboardBindings[7] = 290 + 11;                    // F12
    y.keyboardBindings[8] = 59;                          // Semicolon
    y.gamepadBindings[2] = st::kGamepadRightTrigger;
    y.gamepadBindings[12] = 8;                           // Guide
    y.cameraKeyBindings[1] = st::kKeyUnknown;
    y.tankKeyBindings[0] = 341;                          // Left Ctrl
    s.game.invertBinocucom = 1;
    s.game.invertTurret = 1;
    s.game.speakers = st::Speakers_Mono;
    // (audio) names with spaces, brackets, a " #" / " ;" (would start a comment unquoted), UTF-8
    s.game.outputDevice = "Speakers (Realtek(R) Audio) #2";
    s.game.inputDevice = "Mikrofon ; USB-Headset \xC3\xBC";
    s.game.usbHeadset = 1;
    s.game.headsetOutputDevice = "Logitech USB Headset H340 Analog Stereo";
    s.pendingSaveOptions = {st::kSaveOptionMusic, st::kSaveOptionInvertTurret};
    return s;
}

std::filesystem::path TempDir(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("safehouse-settings-test-" + std::string(name) + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}
}  // namespace

int SettingsSelfTest()
{
    // the defaults and a changed value of every kind survive a round trip
    {
        const st::Sly2Settings d;
        const st::Sly2Settings back = st::sly2FromIni(st::sly2ToIni(d));
        Check(SameSystem(back.system, st::sly2Defaults()) && SameGame(back.game, d.game), "defaults round trip", FirstDifference(back, d));
        Check(back.pendingSaveOptions.empty() && back.unknown.sections.empty(), "defaults: nothing pending or unknown");
        const st::Sly2Settings c = Custom();
        std::vector<std::string> invalid;
        const st::Sly2Settings c2 = st::sly2FromIni(st::sly2ToIni(c), &invalid);
        Check(SameSystem(c2.system, c.system) && SameGame(c2.game, c.game), "custom round trip", FirstDifference(c2, c));
        Check(invalid.empty(), "custom round trip: no invalid key", invalid.empty() ? "" : invalid[0]);
        Check(c2.pendingSaveOptions == c.pendingSaveOptions, "pending save options round trip");
        Check(st::sly2ToIni(c2) == st::sly2ToIni(c), "custom text stable");
        // every key the schema writes is read back (none lands in the unknown keys)
        Check(c2.unknown.sections.empty(), "custom: no unknown key");
    }
    // (upscaling) [Display] Upscaling's keys: off by default, a hand-written value of each read and written back
    {
        const st::Sly2Settings d = st::sly2FromIni(st::sly2ToIni(st::Sly2Settings{}));
        Check(d.upscale.enabled == 0 && d.upscale.method == st::UpscaleMethod_Fsr1 && d.upscale.preset == st::UpscalePreset_Quality &&
              d.upscale.sharpness == 80, "upscaling: off by default (FSR 1, Quality, 80 %)");
        std::vector<std::string> invalid;
        const st::Sly2Settings u = st::sly2FromIni("[Display]\nUpscaling = On\nUpscaling Method = fsr1\n"
                                                 "upscaling_preset = ultra quality\nUpscaling Sharpness = 35%\n", &invalid);
        Check(u.upscale.enabled == 1 && u.upscale.preset == st::UpscalePreset_UltraQuality && u.upscale.sharpness == 35 && invalid.empty(),
              "upscaling: hand-written keys read");
        const st::Sly2Settings u2 = st::sly2FromIni(st::sly2ToIni(u));
        Check(u2.upscale.enabled == 1 && u2.upscale.preset == u.upscale.preset && u2.upscale.sharpness == 35, "upscaling: round trip");
        invalid.clear();
        const st::Sly2Settings bad = st::sly2FromIni("[Display]\nUpscaling Preset = Extreme\nUpscaling Sharpness = 101\n", &invalid);
        Check(bad.upscale.preset == st::UpscalePreset_Quality && bad.upscale.sharpness == 80 && invalid.size() == 2,
              "upscaling: unreadable values keep the defaults");
    }
    // the names: every key and button name reads back to its value
    {
        bool ok = true;
        for (const auto& n : st::names::keys())
        {
            int v = 0;
            ok &= st::names::valueOf(st::names::keys(), n.name, v) && v == n.value;
        }
        for (const auto& n : st::names::padButtons())
        {
            int v = 0;
            ok &= st::names::valueOf(st::names::padButtons(), n.name, v) && v == n.value;
        }
        Check(ok, "key / button names unique and readable");
    }
    // a hand-edited file
    {
        const std::string text =
            "\xEF\xBB\xBF; a comment\r\n"
            "# another\r\n"
            "[display]\r\n"
            "window_mode = fullscreen ; trailing comment\r\n"
            "VSYNC=yes\r\n"
            "Frame Rate Limit = 20\r\n"        // below 30: the default
            "Aspect Ratio = \"16:9\"\r\n"
            "  [ Audio ]  \r\n"
            "music volume = 150\r\n"           // out of range: the default
            "Effects Volume = 40%\r\n"
            "Speakers = stereo\r\n"
            "Future Option = 7\r\n"            // unknown: kept
            "[Graphics]\r\n"
            "Draw Distance = lots\r\n"         // unreadable: the default
            "Internal Resolution = 1080p\r\n"
            "Anti-Aliasing = 3x\r\n"           // not a sample count: the default
            "[Keyboard]\r\n"
            "Cross = space\r\n"
            "Square = escape\r\n"              // never bindable: the default
            "Camera Left = Mouse Left\r\n"     // camera keys are keys: the default
            "Move Forward = I\r\n"
            "Move Forward = Up\r\n"            // repeated: the last wins
            "[Controller]\r\n"
            "Cross = R2\r\n"
            "Invert Binocucom = on\r\n"
            "[Mouse]\r\n"
            "Camera Sensitivity = 9000\r\n"
            "garbage line without equals\r\n"
            "[Display]\r\n"                    // a section again continues it
            "VSync = off\r\n"
            "[Mods]\r\n"
            "Shiny = 1\r\n";
        std::vector<std::string> invalid;
        const st::Sly2Settings s = st::sly2FromIni(text, &invalid);
        const st::SystemSettings d = st::sly2Defaults();
        Check(s.system.windowMode == st::Window_Fullscreen, "case / underscores / trailing comment");
        Check(s.system.vsyncEnabled == 0, "repeated section and key: the last value");
        Check(s.system.frameRate == d.frameRate, "frame rate below 30: default");
        Check(s.system.aspectMode == st::Aspect_16_9, "quoted value");
        Check(s.system.musicVolume == d.musicVolume, "volume out of range: default");
        Check(s.system.soundEffectsVolume == 0.4f, "volume with %");
        Check(s.game.speakers == st::Speakers_Stereo, "speakers");
        Check(s.system.drawDistance == d.drawDistance, "unreadable float: default");
        Check(s.system.internalResolutionHeight == 1080, "resolution with p");
        Check(s.system.msaaEnabled == d.msaaEnabled && s.system.msaaSamples == d.msaaSamples, "invalid MSAA: default");
        Check(s.system.keyboardBindings[6] == 32, "key by lower-case name");
        Check(s.system.keyboardBindings[4] == d.keyboardBindings[4], "Escape: default");
        Check(s.system.cameraKeyBindings[0] == d.cameraKeyBindings[0], "mouse on a camera key: default");
        Check(s.system.keyboardBindings[0] == 265, "repeated key: the last");
        Check(s.system.gamepadBindings[6] == st::kGamepadRightTrigger, "pad button by name");
        Check(s.game.invertBinocucom == 1, "game option");
        Check(s.system.mouseSensitivity == d.mouseSensitivity, "sensitivity out of range: default");
        Check(invalid.size() == 7, "invalid keys reported", std::to_string(invalid.size()));
        const std::string* future = s.unknown.find("Audio", "Future Option");
        Check(future && *future == "7", "unknown key kept");
        const std::string out = st::sly2ToIni(s);
        Check(out.find("Future Option = 7") != std::string::npos && out.find("[Mods]\nShiny = 1") != std::string::npos,
              "unknown key and section written back");
        Check(out.find("garbage") == std::string::npos, "an unreadable line dropped");
        const st::Sly2Settings again = st::sly2FromIni(out);
        Check(SameSystem(again.system, s.system) && again.unknown.find("Mods", "Shiny") != nullptr, "hand-edited file round trip");
        Check(st::sly2FromIni("").system.frameRate == d.frameRate && SameSystem(st::sly2FromIni("\x01\x02 binary [").system, d),
              "empty / garbage text: defaults");
    }
    // the file: missing, migration from each old binary version, the save's options, the atomic write
    {
        const auto dir = TempDir("files");
        st::Sly2Settings s;
        Check(st::loadSly2(dir, s) == st::Sly2Load::Missing && SameSystem(s.system, st::sly2Defaults()) &&
                  s.pendingSaveOptions == st::allSaveOptions(),
              "missing: defaults, the save's options pending");
        // a v19 file with changed values
        st::SystemSettings old = st::sly2Defaults();
        old.frameRate = 144;
        old.aspectMode = st::Aspect_4_3;
        old.musicVolume = 0.64f;
        old.soundEffectsVolume = 0.5f;
        old.controllerVibration = 1;
        old.mouseInvertBinocX = 1;
        old.keyboardBindings[3] = 262;
        Check(st::save(st::settingsPath(dir), old), "write system_settings.dat");
        Check(st::loadSly2(dir, s) == st::Sly2Load::Migrated && s.system.frameRate == 144 && s.system.aspectMode == st::Aspect_4_3 &&
                  s.system.mouseInvertBinocX == 1 && s.system.keyboardBindings[3] == 262,
              "migrated from system_settings.dat v19");
        Check(st::saveSly2(dir, s), "write Settings.ini");
        Check(!std::filesystem::exists(st::sly2SettingsPath(dir).string() + ".tmp"), "no temporary file left");
        Check(std::filesystem::exists(st::settingsPath(dir)), "the old file is kept");
        st::Sly2Settings r;
        Check(st::loadSly2(dir, r) == st::Sly2Load::Ok && SameSystem(r.system, s.system) && r.pendingSaveOptions == st::allSaveOptions(),
              "Settings.ini read back, still pending");
        // the INI wins over the old file from now on
        old.frameRate = 30;
        st::save(st::settingsPath(dir), old);
        Check(st::loadSly2(dir, r) == st::Sly2Load::Ok && r.system.frameRate == 144, "the INI wins over system_settings.dat");
        // the save's options: vibration off, music at 0.5 of its channel, mono, binocucom inverted
        st::SaveOptionValues v;
        v.hasVibration = v.noVibration = true;
        v.hasMusic = true;
        v.musicComplement = 0.5f;
        v.hasEffects = true;
        v.effectsComplement = 0.0f;
        v.hasSpeakers = true;
        v.speakers = st::Speakers_Mono;
        v.hasInvertBinocucom = v.invertBinocucom = true;
        v.hasInvertTurret = true;
        st::applySaveOptions(r, v);
        Check(r.system.controllerVibration == 0, "migration: vibration off from the save");
        Check(std::fabs(r.system.musicVolume - 0.64f * 0.25f) < 1e-6f, "migration: music = the PC slider x the channel factor squared");
        Check(r.system.soundEffectsVolume == 0.5f, "migration: effects unchanged at full channel");
        Check(r.game.speakers == st::Speakers_Mono && r.game.invertBinocucom == 1 && r.game.invertTurret == 0, "migration: game options");
        Check(r.pendingSaveOptions.empty(), "migration: nothing pending after");
        // the level a written save carries plays the same in the original: its channel factor is the square root
        Check(std::fabs(st::saveComplement(0.25f) - 0.5f) < 1e-6f && st::saveComplement(1.0f) == 0.0f, "save complement");
        // a pending option the launcher changed is not taken from the save
        st::Sly2Settings p;
        p.pendingSaveOptions = st::allSaveOptions();
        st::clearPending(p, st::kSaveOptionInvertBinocucom);
        st::applySaveOptions(p, v);
        Check(p.game.invertBinocucom == 0 && p.game.speakers == st::Speakers_Mono, "migration skips a key no longer pending");
        std::filesystem::remove_all(dir);
    }
    // every historic binary version migrates (a v8 file: the defaults beyond its fields, Sly 2's keyboard layout kept)
    {
        const auto dir = TempDir("v8");
        st::SystemSettings old = st::sly2Defaults();
        old.windowMode = st::Window_Fullscreen;
        old.dialogueVolume = 0.25f;
        Check(st::save(st::settingsPath(dir), old, 8), "write a v8 file");
        st::Sly2Settings s;
        Check(st::loadSly2(dir, s) == st::Sly2Load::Migrated && s.system.windowMode == st::Window_Fullscreen &&
                  s.system.dialogueVolume == 0.25f && s.system.longShadows == 1,
              "migrated from a v8 file");
        // an unreadable binary file: defaults, still pending
        std::filesystem::remove(st::settingsPath(dir));
        { std::ofstream(st::settingsPath(dir), std::ios::binary) << "garbage"; }
        Check(st::loadSly2(dir, s) == st::Sly2Load::Missing && SameSystem(s.system, st::sly2Defaults()), "corrupt old file: defaults");
        std::filesystem::remove_all(dir);
    }
    // (audio, platform/audio_devices.h) the devices by name: the INI spelling, the matching, the fallback to the system
    // default when the chosen one is missing and its recovery when it comes back
    {
        namespace au = cane::audio;
        const st::Sly2Settings d;
        const std::string t = st::sly2ToIni(d);
        Check(t.find("Output Device = System Default") != std::string::npos && t.find("Input Device = System Default") != std::string::npos &&
                  t.find("USB Headset = Off") != std::string::npos && t.find("Headset Output Device = Same As Output") != std::string::npos,
              "audio defaults written");
        const st::Sly2Settings c = Custom();
        const std::string tc = st::sly2ToIni(c);
        Check(tc.find("Output Device = \"Speakers (Realtek(R) Audio) #2\"") != std::string::npos, "device name quoted");
        // hand-written spellings: unquoted, a trailing comment, other cases of System Default
        const st::Sly2Settings h = st::sly2FromIni("[Audio]\noutput_device = USB Audio CODEC ; my DAC\ninput device = system default\n"
                                                   "usb headset = on\nheadset output device = \"Head ; set\" ; the second one\n");
        Check(h.game.outputDevice == "USB Audio CODEC" && h.game.inputDevice.empty() && h.game.usbHeadset == 1 &&
                  h.game.headsetOutputDevice == "Head ; set",
              "device names hand-written", h.game.outputDevice + "|" + h.game.inputDevice + "|" + h.game.headsetOutputDevice);
        const st::Sly2Settings h2 = st::sly2FromIni("[Audio]\nOutput Device = \"\"\nHeadset Output Device = same as output\n");
        Check(h2.game.outputDevice.empty() && h2.game.headsetOutputDevice.empty(), "empty / Same As Output read as the default");
        st::Sly2Settings q;
        q.game.outputDevice = "A \"b\" c";
        Check(au::nameFromIni("  System  Default ").empty() && au::nameToIni("") == "System Default" &&
                  st::sly2FromIni(st::sly2ToIni(q)).game.outputDevice == q.game.outputDevice,
              "name INI spelling");
        const std::vector<std::string> present{"Built-in Audio Analog Stereo", "USB Headset Analog Stereo"};
        Check(au::findDevice(present, "USB Headset Analog Stereo") == 1 && au::findDevice(present, "usb headset analog stereo ") == 1 &&
                  au::findDevice(present, "") == -1 && au::findDevice(present, "HDMI") == -1,
              "device matching");
        au::DeviceFollower f;
        f.wanted = "USB Headset Analog Stereo";
        bool ok = f.step(present) && f.open == "USB Headset Analog Stereo" && !f.fallenBack();
        ok &= !f.step(present);                                             // (no change: nothing reopened)
        ok &= f.step({"Built-in Audio Analog Stereo"}) && f.open.empty() && f.fallenBack();   // unplugged: the default
        ok &= !f.step({"Built-in Audio Analog Stereo"});
        ok &= f.step(present) && f.open == "USB Headset Analog Stereo";      // back: taken again
        f.wanted.clear();
        ok &= f.step(present) && f.open.empty();                            // the choice changed to the default
        au::DeviceFollower g;
        ok &= g.step({}) && g.open.empty() && !g.step({});                  // the default with no device listed
        Check(ok, "device follower: fallback and recovery");
        const auto choices = au::deviceChoices(present, "Gone Device");
        Check(choices.size() == 4 && choices[0].empty() && choices[3] == "Gone Device", "device choices keep a missing one");
    }
    std::printf("settings self-test: %d checks, %d failed\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
