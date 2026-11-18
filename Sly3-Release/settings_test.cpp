// --settings-selftest: Sly 3's Settings.ini (platform/sly3_settings.h over sly2_settings.h's reader / writer) without a
// disc or a window: the defaults and changed values round trip, Sly 2's own keys are not in Sly 3's file (and a hand
// written one is kept as unknown), a hand-edited file, the migration from Saves/system_settings.dat, the atomic write.
#include "../platform/sly3_settings.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
bool Has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }
}  // namespace

int SettingsSelfTest()
{
    // defaults: the file's first line, its sections, no Sly 2-only key
    const st::Sly3Settings d;
    const std::string td = st::sly3ToIni(d);
    Check(Has(td, "Sly 3: Honor Among Thieves"), "the title line");
    for (const char* section : {"[Display]", "[Graphics]", "[Audio]", "[Controls]", "[Controller]", "[Mouse]", "[Keyboard]", "[SafeHouse]"})
        Check(Has(td, section), "a section", section);
    for (const char* absent : {"[VR]", "Invert Binocucom =", "Invert Turret =", "Tank Left Track", "Mouse Aim In Hacking",
                               "Pending Save Options"})
        Check(!Has(td, absent), "a Sly 2-only key absent", absent);
    for (const char* present : {"Speakers =", "USB Headset =", "Output Device =", "Renderer =", "Fast Shadows =", "Automatic Camera Rotation =", "Monitor = Primary"})
        Check(Has(td, present), "a shared key present", present);
    Check(st::sly2ToIni(d) != td, "Sly 2's file differs");
    Check(Has(td, "Camera Left/Right = Default") && Has(td, "Harpoon Up/Down = Default"), "the camera options' keys");
    Check(!Has(st::sly2ToIni(d), "Camera Left/Right"), "no camera options in Sly 2's file");
    Check(d.game.mode3d == 0 && Has(td, "3D Mode = Off"), "the 3-D mode off by default");
    Check(!Has(st::sly2ToIni(d), "3D Mode"), "no 3-D mode in Sly 2's file");
    // round trip of changed values
    st::Sly3Settings c = d;
    c.system.frameRate = 144;
    c.system.musicVolume = 0.25f;
    c.system.renderer = 2;
    c.system.autoCameraRotation = 0;
    c.game.speakers = st::Speakers_Mono;
    c.game.usbHeadset = 1;
    c.game.outputDevice = "USB Audio ; Device # 2";
    c.game.camReverse[3][1] = 1;   // (Turret Up/Down: Reversed)
    c.game.mode3d = 1;
    std::vector<std::string> invalid;
    const st::Sly3Settings back = st::sly3FromIni(st::sly3ToIni(c), &invalid);
    Check(invalid.empty(), "no invalid keys");
    Check(back.system.frameRate == 144 && back.system.renderer == 2 && back.system.autoCameraRotation == 0, "system values");
    Check(back.system.musicVolume > 0.24f && back.system.musicVolume < 0.26f, "music volume");
    Check(back.game.speakers == st::Speakers_Mono && back.game.usbHeadset == 1, "game options");
    Check(back.game.outputDevice == c.game.outputDevice, "a device name", back.game.outputDevice);
    Check(back.game.camReverse[3][1] == 1 && back.game.camReverse[3][0] == 0 && back.game.camReverse[0][1] == 0, "a camera option");
    Check(back.game.mode3d == 1 && Has(st::sly3ToIni(c), "3D Mode = On"), "the 3-D mode on");
    Check(st::sly3ToIni(back) == st::sly3ToIni(c), "stable text");
    // hand-edited: other spellings, a BOM, CRLF, an invalid value, Sly 2's key and an unknown section kept
    const std::string hand = "\xEF\xBB\xBF; mine\r\n[display]\r\nframe_rate_limit = Uncapped\r\nvsync=off\r\n[Audio]\r\n"
                             "Music Volume = loud\r\n[Controller]\r\nInvert Turret = On\r\n[My Mod]\r\nx = 1\r\n";
    invalid.clear();
    const st::Sly3Settings h = st::sly3FromIni(hand, &invalid);
    Check(h.system.frameRate >= 1000 && h.system.vsyncEnabled == 0, "hand spellings");
    Check(invalid.size() == 1 && invalid[0] == "Audio/Music Volume", "the invalid value reported", invalid.empty() ? "" : invalid[0]);
    Check(h.system.musicVolume == d.system.musicVolume, "the invalid value's default");
    Check(h.game.invertTurret == 0, "Sly 2's key not read");
    const std::string th = st::sly3ToIni(h);
    Check(Has(th, "Invert Turret = On") && Has(th, "[My Mod]") && Has(th, "x = 1"), "unknown keys written back");
    // the files: missing, the migration from system_settings.dat, the INI wins afterwards, no .tmp left
    const auto dir = std::filesystem::temp_directory_path() /
                     ("sly3-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir / "Saves");
    st::Sly3Settings f;
    Check(st::loadSly3(dir, f) == st::Sly2Load::Missing, "missing");
    st::SystemSettings old = st::sly2Defaults();
    old.frameRate = 30;
    old.aspectMode = st::Aspect_16_9;
    Check(st::save(st::settingsPath(dir), old), "the old file written");
    Check(st::loadSly3(dir, f) == st::Sly2Load::Migrated && f.system.frameRate == 30 && f.pendingSaveOptions.empty(), "migrated");
    Check(st::saveSly3(dir, f), "saved");
    old.frameRate = 60;
    st::save(st::settingsPath(dir), old);
    st::Sly3Settings g;
    Check(st::loadSly3(dir, g) == st::Sly2Load::Ok && g.system.frameRate == 30, "the INI wins");
    Check(!std::filesystem::exists(st::sly3SettingsPath(dir).string() + ".tmp"), "no .tmp left");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::printf("settings selftest: %d checks, %d failed\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
