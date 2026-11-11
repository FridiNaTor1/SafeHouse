#pragma once
// Controller identification, active-pad selection and "last used device"
// tracking, shared by the game and the launcher. Depends only on GLFW (the
// 3.3 API subset, so the game's vendored glfw3.h and the fetched 3.4 headers
// both work) and on brand.h for the hidden test hook.
//
// Labels are positional, like GLFW and SDL mappings: GLFW_GAMEPAD_BUTTON_A is
// always the south face button, which Nintendo labels "B".
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "brand.h"
#include "gamepad_mappings.h"
#include "system_settings.h"
namespace cane::input {
// PlayStation: the game's original PS2 icons (any Sony pad not recognised as a DualShock 4 / DualSense, and the
// "Original PlayStation Icons" option); PS4 / PS5: DualShock 4 / DualSense (and DualSense Edge) with Kenney's modern
// PlayStation icons. SteamController is the 2015 one (one stick, two round pads); SteamController2026 the 2026 one
// ("Triton": two sticks above two square pads, the Steam Deck's controls without the screen).
// Nintendo is the Switch (Pro Controller, Joy-Con); Switch2 the Switch 2 (Pro Controller 2, Joy-Con 2); GameCube the
// GameCube pad (adapters, the Switch Online GameCube controller); Wii the Wii Remote with a Classic Controller (Pro);
// WiiU the Wii U Pro Controller. Anything unrecognised is Xbox.
enum class Family : uint8_t { Keyboard, PlayStation, Xbox, Nintendo, SteamDeck, SteamController, PS4, PS5,
                              SteamController2026, GameCube, Switch2, Wii, WiiU,
                              // VR controllers, the two hands as one pad (selected by the VR code: noteVr / prompt style)
                              MetaQuest, ValveIndex, SteamFrame, ViveWand, Pico };
inline constexpr int kFamilyCount = 18;
inline constexpr bool isVrFamily(Family family) {
    return family == Family::MetaQuest || family == Family::ValveIndex || family == Family::SteamFrame ||
           family == Family::ViveWand || family == Family::Pico;
}
inline constexpr bool isPlayStationFamily(Family family) {
    return family == Family::PlayStation || family == Family::PS4 || family == Family::PS5;
}
// Nintendo's face-button layout: A east, B south (the launcher confirms with A). Not the GameCube, whose big A is
// the south button.
inline constexpr bool isNintendoLayout(Family family) {
    return family == Family::Nintendo || family == Family::Switch2 || family == Family::Wii || family == Family::WiiU;
}
// Steam hardware with trackpads (the launcher's Steam keyboard hint).
inline constexpr bool isSteamFamily(Family family) {
    return family == Family::SteamDeck || family == Family::SteamController || family == Family::SteamController2026;
}
// Stored as SystemSettings::promptStyle.
enum class PromptStyle : int { Auto = 0, PlayStation = 1, Xbox = 2, Nintendo = 3, SteamDeck = 4, SteamController = 5,
                               PS4 = 6, PS5 = 7, SteamController2026 = 8, GameCube = 9, Switch2 = 10, Wii = 11,
                               WiiU = 12, MetaQuest = 13, ValveIndex = 14, SteamFrame = 15, ViveWand = 16, Pico = 17 };
inline constexpr int kPromptStyleLast = static_cast<int>(PromptStyle::Pico);
static_assert(static_cast<int>(PromptStyle::SteamController) == settings::Prompt_SteamController);
static_assert(static_cast<int>(PromptStyle::PS5) == settings::Prompt_PS5);
static_assert(kPromptStyleLast == settings::Prompt_Last);
static_assert(static_cast<int>(PromptStyle::GameCube) == settings::Prompt_GameCube);
// Every family but Keyboard has a prompt style of the same number.
static_assert(kPromptStyleLast == kFamilyCount - 1);
static_assert(static_cast<int>(PromptStyle::Nintendo) == settings::Prompt_Nintendo);
// Positional buttons. LS/RS are stick movement, LSPress/RSPress are clicks
// (L3/R3), Dpad is the whole pad. Guide may have no icon (fall back to text).
enum class Button : uint8_t { South, East, West, North, LB, RB, LT, RT, Back, Start, LS, RS, LSPress, RSPress,
                              DpadUp, DpadDown, DpadLeft, DpadRight, Dpad, Guide, Count };
inline constexpr int kButtonCount = static_cast<int>(Button::Count);

namespace detail {
inline char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
inline std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}
inline bool contains(std::string_view loweredHaystack, std::string_view needle) {
    return loweredHaystack.find(needle) != std::string_view::npos;
}
inline std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
// Two hex bytes, little endian ("5e04" -> 0x045e). -1 if malformed.
inline int hexLe16(std::string_view text) {
    if (text.size() < 4) return -1;
    int value = 0;
    for (int i = 0; i < 4; ++i)
        if (hexDigit(text[i]) < 0) return -1;
    value = hexDigit(text[0]) << 4 | hexDigit(text[1]);
    value |= (hexDigit(text[2]) << 4 | hexDigit(text[3])) << 8;
    return value;
}
// strtoul(value, nullptr, 0) semantics: 0x prefix = hex, otherwise decimal.
inline std::optional<unsigned long> parseNumber(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 0);
    if (end == copy.c_str()) return std::nullopt;
    return value;
}
} // namespace detail

// Stable lower-case ids, used for test hooks, atlas names and logs.
inline const char* familyId(Family family) {
    switch (family) {
    case Family::Keyboard: return "keyboard";
    case Family::PlayStation: return "playstation";
    case Family::Xbox: return "xbox";
    case Family::Nintendo: return "nintendo";
    case Family::SteamDeck: return "steamdeck";
    case Family::SteamController: return "steamcontroller";
    case Family::PS4: return "ps4";
    case Family::PS5: return "ps5";
    case Family::SteamController2026: return "steamcontroller2026";
    case Family::GameCube: return "gamecube";
    case Family::Switch2: return "switch2";
    case Family::Wii: return "wii";
    case Family::WiiU: return "wiiu";
    case Family::MetaQuest: return "metaquest";
    case Family::ValveIndex: return "valveindex";
    case Family::SteamFrame: return "steamframe";
    case Family::ViveWand: return "vivewand";
    case Family::Pico: return "pico";
    }
    return "xbox";
}
// The controller's name for menus ("Nintendo Switch", "PlayStation 5"); "Keyboard and mouse" for Keyboard.
inline const char* familyDisplayName(Family family) {
    switch (family) {
    case Family::Keyboard: return "Keyboard and mouse";
    case Family::PlayStation: return "PlayStation";
    case Family::Xbox: return "Xbox";
    case Family::Nintendo: return "Nintendo Switch";
    case Family::SteamDeck: return "Steam Deck";
    case Family::SteamController: return "Steam Controller";
    case Family::PS4: return "PlayStation 4";
    case Family::PS5: return "PlayStation 5";
    case Family::SteamController2026: return "Steam Controller (2026)";
    case Family::GameCube: return "Nintendo GameCube";
    case Family::Switch2: return "Nintendo Switch 2";
    case Family::Wii: return "Wii Classic Controller";
    case Family::WiiU: return "Wii U Pro Controller";
    case Family::MetaQuest: return "Meta Quest Touch";
    case Family::ValveIndex: return "Valve Index";
    case Family::SteamFrame: return "Steam Frame";
    case Family::ViveWand: return "HTC Vive";
    case Family::Pico: return "Pico";
    }
    return "Xbox";
}
// Accepts familyId() values plus the short forms ps, switch, deck and sc2026.
inline std::optional<Family> parseFamily(std::string_view text) {
    const auto value = detail::lowered(detail::trim(text));
    if (value == "keyboard" || value == "kbm") return Family::Keyboard;
    if (value == "ps" || value == "playstation") return Family::PlayStation;
    if (value == "xbox") return Family::Xbox;
    if (value == "nintendo" || value == "switch" || value == "joycon") return Family::Nintendo;
    if (value == "gamecube" || value == "gc") return Family::GameCube;
    if (value == "switch2" || value == "ns2") return Family::Switch2;
    if (value == "wii") return Family::Wii;
    if (value == "wiiu") return Family::WiiU;
    if (value == "metaquest" || value == "quest" || value == "oculus" || value == "touch") return Family::MetaQuest;
    if (value == "valveindex" || value == "index" || value == "knuckles") return Family::ValveIndex;
    if (value == "steamframe" || value == "frame") return Family::SteamFrame;
    if (value == "vivewand" || value == "vive") return Family::ViveWand;
    if (value == "pico") return Family::Pico;
    if (value == "steamdeck" || value == "deck") return Family::SteamDeck;
    if (value == "steamcontroller") return Family::SteamController;
    if (value == "steamcontroller2026" || value == "sc2026" || value == "triton") return Family::SteamController2026;
    if (value == "ps4" || value == "dualshock4") return Family::PS4;
    if (value == "ps5" || value == "dualsense") return Family::PS5;
    return std::nullopt;
}
inline Family familyForStyle(PromptStyle style) {
    switch (style) {
    case PromptStyle::PlayStation: return Family::PlayStation;
    case PromptStyle::Xbox: return Family::Xbox;
    case PromptStyle::Nintendo: return Family::Nintendo;
    case PromptStyle::SteamDeck: return Family::SteamDeck;
    case PromptStyle::SteamController: return Family::SteamController;
    case PromptStyle::PS4: return Family::PS4;
    case PromptStyle::PS5: return Family::PS5;
    case PromptStyle::SteamController2026: return Family::SteamController2026;
    case PromptStyle::GameCube: return Family::GameCube;
    case PromptStyle::Switch2: return Family::Switch2;
    case PromptStyle::Wii: return Family::Wii;
    case PromptStyle::WiiU: return Family::WiiU;
    case PromptStyle::MetaQuest: return Family::MetaQuest;
    case PromptStyle::ValveIndex: return Family::ValveIndex;
    case PromptStyle::SteamFrame: return Family::SteamFrame;
    case PromptStyle::ViveWand: return Family::ViveWand;
    case PromptStyle::Pico: return Family::Pico;
    case PromptStyle::Auto: break;
    }
    return Family::Xbox;
}
// The prompt-style choices' names, in PromptStyle order ("Automatic" first): the launcher's and the games' menus.
inline const char* promptStyleName(int style) {
    if (style <= 0 || style > kPromptStyleLast) return "Automatic";
    return familyDisplayName(familyForStyle(static_cast<PromptStyle>(style)));
}

// SDL-style GUID as produced by GLFW: bus(LE) 0000 vendor(LE) 0000
// product(LE) 0000 version(LE) 0000. Only valid when the 0000 separators are
// present and the vendor is non-zero; otherwise the GUID carries name bytes.
// Windows XInput devices use "78696e707574..." ("xinput") and carry no IDs.
struct GuidIds {
    uint16_t vendor = 0, product = 0;
    bool valid = false, xinput = false;
};
inline GuidIds parseGuid(std::string_view guid) {
    GuidIds ids;
    if (guid.size() >= 12 && detail::lowered(guid.substr(0, 12)) == "78696e707574") {
        ids.xinput = true;
        return ids;
    }
    if (guid.size() < 24 || guid.substr(12, 4) != "0000" || guid.substr(20, 4) != "0000") return ids;
    const int vendor = detail::hexLe16(guid.substr(8, 4)), product = detail::hexLe16(guid.substr(16, 4));
    if (vendor <= 0 || product < 0) return ids;
    ids.vendor = static_cast<uint16_t>(vendor);
    ids.product = static_cast<uint16_t>(product);
    ids.valid = true;
    return ids;
}

// The file named by Steam's SteamVirtualGamepadInfo environment variable:
// "[slot N]" sections with name=, VID=, PID=, type=, handle= keys. It names
// the physical controller behind each Steam Input virtual pad.
struct SteamSlot {
    uint16_t vid = 0, pid = 0;
    std::string type, name, handle;
};
inline std::map<int, SteamSlot> parseSteamVirtualGamepadInfo(std::string_view ini) {
    std::map<int, SteamSlot> slots;
    std::optional<int> slot;
    while (!ini.empty()) {
        const auto newline = ini.find_first_of("\r\n");
        auto line = detail::trim(ini.substr(0, newline));
        ini.remove_prefix(newline == std::string_view::npos ? ini.size() : newline + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            slot.reset();
            const auto close = line.find(']');
            auto section = detail::lowered(detail::trim(line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1)));
            if (section.rfind("slot", 0) == 0)
                if (auto number = detail::parseNumber(std::string_view(section).substr(4)); number && *number < 1024) {
                    slot = static_cast<int>(*number);
                    slots[*slot];
                }
            continue;
        }
        const auto equals = line.find('=');
        if (!slot || equals == std::string_view::npos) continue;
        const auto key = detail::lowered(detail::trim(line.substr(0, equals)));
        const auto value = detail::trim(line.substr(equals + 1));
        auto& entry = slots[*slot];
        if (key == "name") entry.name = std::string(value);
        else if (key == "type") entry.type = std::string(value);
        else if (key == "handle") entry.handle = std::string(value);
        else if (key == "vid" || key == "pid") {
            const auto number = detail::parseNumber(value);
            const auto id = static_cast<uint16_t>(number && *number <= 0xffff ? *number : 0);
            (key == "vid" ? entry.vid : entry.pid) = id;
        }
    }
    return slots;
}
// SDL controller type strings (xbox360, xboxone, ps3, ps4, ps5, switchpro,
// joyconleft/right/pair, ...). Unknown or "virtual" types return nullopt.
inline std::optional<Family> familyFromSteamType(std::string_view type) {
    const auto value = detail::lowered(detail::trim(type));
    if (value.rfind("xbox", 0) == 0) return Family::Xbox;
    if (value == "ps4") return Family::PS4;
    if (value == "ps5") return Family::PS5;
    if (value == "ps3" || value.rfind("playstation", 0) == 0) return Family::PlayStation;
    if (detail::contains(value, "gamecube")) return Family::GameCube;
    if (detail::contains(value, "switch2") || value.rfind("ns2", 0) == 0) return Family::Switch2;
    if (detail::contains(value, "wiiu")) return Family::WiiU;
    if (value.rfind("wii", 0) == 0) return Family::Wii;
    if (value.rfind("switch", 0) == 0 || detail::contains(value, "joycon")) return Family::Nintendo;
    if (value == "steamdeck" || detail::contains(value, "neptune")) return Family::SteamDeck;
    if (detail::contains(value, "triton") || value == "steamcontroller2026") return Family::SteamController2026;
    if (detail::contains(value, "frame")) return Family::SteamFrame;
    if (detail::contains(value, "knuckles") || detail::contains(value, "index")) return Family::ValveIndex;
    if (detail::contains(value, "oculus") || detail::contains(value, "quest")) return Family::MetaQuest;
    if (detail::contains(value, "vive")) return Family::ViveWand;
    if (value.rfind("steamcontroller", 0) == 0) return Family::SteamController;
    return std::nullopt;
}
// Steam's virtual pads are named "Microsoft X-Box 360 pad N" on Linux; N is
// the Steam slot. -1 when absent.
inline int steamSlotFromName(std::string_view joystickName) {
    const auto at = joystickName.find("pad ");
    if (at == std::string_view::npos) return -1;
    auto digits = joystickName.substr(at + 4);
    int slot = 0, count = 0;
    while (!digits.empty() && std::isdigit(static_cast<unsigned char>(digits.front())) && count < 4) {
        slot = slot * 10 + (digits.front() - '0');
        digits.remove_prefix(1);
        ++count;
    }
    return count ? slot : -1;
}
// Steam's SDL_GAMECONTROLLER_IGNORE_DEVICES: "0xVVVV/0xPPPP,0xVVVV/0xPPPP".
inline std::vector<std::pair<uint16_t, uint16_t>> parseIgnoreDevices(std::string_view list) {
    std::vector<std::pair<uint16_t, uint16_t>> devices;
    while (!list.empty()) {
        const auto comma = list.find(',');
        const auto item = detail::trim(list.substr(0, comma));
        list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
        const auto slash = item.find('/');
        if (slash == std::string_view::npos) continue;
        const auto vid = detail::parseNumber(item.substr(0, slash)), pid = detail::parseNumber(item.substr(slash + 1));
        if (vid && pid && *vid <= 0xffff && *pid <= 0xffff)
            devices.emplace_back(static_cast<uint16_t>(*vid), static_cast<uint16_t>(*pid));
    }
    return devices;
}

inline constexpr uint16_t kVendorSony = 0x054c, kVendorNintendo = 0x057e, kVendorMicrosoft = 0x045e,
                          kVendorValve = 0x28de, kProductSteamVirtualGamepad = 0x11ff;
inline bool isSteamVirtualGamepad(uint16_t vid, uint16_t pid) {
    return vid == kVendorValve && pid == kProductSteamVirtualGamepad;
}
// Rules 1-4 of classify(): families known from USB IDs alone.
inline std::optional<Family> familyFromIds(uint16_t vid, uint16_t pid) {
    if (vid == kVendorSony) {
        switch (pid) {
        case 0x05c4: case 0x09cc: case 0x0ba0: return Family::PS4;   // DualShock 4 (v1, v2, USB wireless adaptor)
        case 0x0ce6: case 0x0df2: return Family::PS5;               // DualSense, DualSense Edge
        default: return Family::PlayStation;
        }
    }
    if (vid == kVendorNintendo) {
        switch (pid) {
        case 0x0337: return Family::GameCube;                            // GameCube adapter for Wii U / Switch
        case 0x2073: return Family::GameCube;                            // Switch Online GameCube controller
        case 0x2066: case 0x2067: case 0x2069: return Family::Switch2;   // Joy-Con 2 (R, L), Pro Controller 2
        // 0306 / 0330 are the Wii Remote (Plus) and the Wii U Pro Controller alike (hid-wiimote names each part:
        // familyFromName below tells them apart)
        case 0x0306: case 0x0330: return std::nullopt;
        default: return Family::Nintendo;
        }
    }
    if (vid == kVendorValve) {
        if (pid == 0x1205) return Family::SteamDeck;                    // Steam Deck ("Neptune")
        // 2026 Steam Controller ("Triton"; SDL controller_list.h): 1302 USB, 1303 Bluetooth LE, 1304 / 1305 its
        // Proteus / Nereid wireless dongles
        if (pid >= 0x1302 && pid <= 0x1305) return Family::SteamController2026;
        if (pid == 0x2300) return Family::ValveIndex;                   // Valve Index controller ("Knuckles", SDL)
        switch (pid) {                                                  // 2015 Steam Controller
        case 0x1101: case 0x1102: case 0x1105: case 0x1106: case 0x1142: case 0x1201: case 0x1202:
            return Family::SteamController;
        default: break;
        }
    }
    if (vid == kVendorMicrosoft) return Family::Xbox;
    return std::nullopt;
}
// Rule 5: name heuristics for third-party pads. Explicit brand words win
// over the generic "Wireless Controller" (Sony's evdev name).
inline std::optional<Family> familyFromName(std::string_view name) {
    const auto n = detail::lowered(name);
    using detail::contains;
    if (n.empty()) return std::nullopt;
    if (contains(n, "ps5") || contains(n, "dualsense")) return Family::PS5;
    if (contains(n, "ps4") || contains(n, "dualshock 4") || contains(n, "dualshock4")) return Family::PS4;
    if (contains(n, "ps3") || contains(n, "dualshock") || contains(n, "playstation"))
        return Family::PlayStation;
    if (contains(n, "gamecube") || contains(n, "game cube") || contains(n, "wavebird")) return Family::GameCube;
    if (contains(n, "switch 2") || contains(n, "switch2") || contains(n, "joy-con 2") || contains(n, "joy-con2"))
        return Family::Switch2;
    // hid-wiimote: "Nintendo Wii Remote Pro Controller" is the Wii U Pro Controller
    if (contains(n, "wii u") || contains(n, "wiiu") || (contains(n, "wii remote") && contains(n, "pro controller")) ||
        contains(n, "rvl-cnt-01-uc"))
        return Family::WiiU;
    if (contains(n, "wii") || contains(n, "rvl-cnt-01")) return Family::Wii;
    if (contains(n, "switch") || contains(n, "nintendo") || contains(n, "joy-con") || contains(n, "joycon") ||
        contains(n, "pro controller"))
        return Family::Nintendo;
    if (contains(n, "steam deck")) return Family::SteamDeck;
    if (contains(n, "steam frame")) return Family::SteamFrame;
    if (contains(n, "oculus") || contains(n, "meta quest") || contains(n, "quest touch") || contains(n, "touch controller"))
        return Family::MetaQuest;
    if (contains(n, "index controller") || contains(n, "knuckles") || contains(n, "valve index")) return Family::ValveIndex;
    if (contains(n, "vive")) return Family::ViveWand;
    if (contains(n, "pico")) return Family::Pico;
    if (contains(n, "triton") || contains(n, "steam controller (2026)") || contains(n, "steam controller 2026"))
        return Family::SteamController2026;
    if (contains(n, "steam controller")) return Family::SteamController;
    if (contains(n, "xbox") || contains(n, "x-box") || contains(n, "xinput")) return Family::Xbox;
    if (contains(n, "wireless controller")) return Family::PlayStation;
    return std::nullopt;
}
// The Android on-screen pad (platform/android/glfw_sdl.cpp, last joystick
// slot). It is drawn as a PlayStation pad, so it gets the game's original
// PlayStation glyphs; physical pads keep the normal detection below.
#if defined(CANE_IOS)
inline constexpr std::string_view kTouchPadName = "Touch controls";   // (the iOS app's on-screen pad)
#else
inline constexpr std::string_view kTouchPadName = "Android touch controls";
#endif

// Order: 0 the on-screen pad, 1 Sony, 2 Nintendo, 3 Valve product table, 4 Microsoft or XInput,
// 5 name heuristics (joystick name, then mapping name), 6 an unresolved
// Steam virtual pad is a Steam Deck when steamDeckEnv (SteamDeck=1), 7 Xbox.
// A Steam virtual pad (28de:11ff) should be resolved through its Steam slot
// before calling this; its own "X-Box 360" name is not used.
inline Family classify(uint16_t vid, uint16_t pid, std::string_view joystickName, std::string_view gamepadName,
                       bool steamDeckEnv, bool xinput = false) {
    if (joystickName == kTouchPadName) return Family::PlayStation;
    if (auto family = familyFromIds(vid, pid)) return *family;
    if (xinput) return Family::Xbox;
    if (isSteamVirtualGamepad(vid, pid)) return steamDeckEnv ? Family::SteamDeck : Family::Xbox;
    if (auto family = familyFromName(joystickName)) return *family;
    if (auto family = familyFromName(gamepadName)) return *family;
    return Family::Xbox;
}
inline bool steamDeckEnvironment() {
    const char* value = std::getenv("SteamDeck");
    return value && std::string_view(value) == "1";
}

struct PadInfo {
    int jid = -1;
    // The physical controller's IDs: for a resolved Steam virtual pad these
    // come from its Steam slot, otherwise from the GUID (0 when unknown).
    uint16_t vendor = 0, product = 0;
    bool xinput = false, steamVirtual = false;
    int steamSlot = -1;
    std::string joystickName, gamepadName;
    Family family = Family::Xbox;
    // How its gamepad mapping was found (gamepad_mappings.h): Builtin (GLFW / the bundled database by exact GUID),
    // Database (the same pad with another version), Evdev / Generic (no database line: an Xbox layout).
    MappingSource mapping = MappingSource::None;
};

namespace detail {
struct SteamInfo {
    std::string path;
    std::filesystem::file_time_type mtime{};
    std::chrono::steady_clock::time_point checked{};
    bool everChecked = false;
    unsigned generation = 0;
    std::map<int, SteamSlot> slots;
};
inline SteamInfo& steamInfo() {
    static SteamInfo info;
    return info;
}
// Re-reads the Steam info file when its path or mtime changes, at most every
// three seconds. Returns the current generation.
inline unsigned refreshSteamInfo() {
    auto& info = steamInfo();
    const auto now = std::chrono::steady_clock::now();
    if (info.everChecked && now - info.checked < std::chrono::seconds(3)) return info.generation;
    info.everChecked = true;
    info.checked = now;
    const char* variable = std::getenv("SteamVirtualGamepadInfo");
    const std::string path = variable ? variable : "";
    std::error_code ec;
    const auto mtime = path.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
    if (path == info.path && (ec ? std::filesystem::file_time_type{} : mtime) == info.mtime) return info.generation;
    info.path = path;
    info.mtime = ec ? std::filesystem::file_time_type{} : mtime;
    info.slots.clear();
    if (!path.empty() && !ec) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        info.slots = parseSteamVirtualGamepadInfo(text.str());
    }
    ++info.generation;
    return info.generation;
}
struct CachedPad {
    bool known = false;
    std::string guid, name;
    unsigned steamGeneration = 0;
    PadInfo info;
};
inline std::array<CachedPad, GLFW_JOYSTICK_LAST + 1>& padCache() {
    static std::array<CachedPad, GLFW_JOYSTICK_LAST + 1> cache;
    return cache;
}
inline void forgetPad(int jid) {
    if (jid >= 0 && jid <= GLFW_JOYSTICK_LAST) padCache()[jid] = {};
}
inline GLFWjoystickfun& previousJoystickCallback() {
    static GLFWjoystickfun callback = nullptr;
    return callback;
}
inline bool& joystickCallbackInstalled() {
    static bool installed = false;
    return installed;
}
inline void resolveFromSteamSlot(PadInfo& info, const SteamSlot& slot, bool steamDeckEnv) {
    info.vendor = slot.vid;
    info.product = slot.pid;
    if (auto family = familyFromIds(slot.vid, slot.pid)) info.family = *family;
    else if (auto typed = familyFromSteamType(slot.type)) info.family = *typed;
    else if (auto named = familyFromName(slot.name)) info.family = *named;
    else info.family = steamDeckEnv ? Family::SteamDeck : Family::Xbox;
}
} // namespace detail

// Identifies a connected joystick. Cached per jid; re-identified when the
// GUID or name changes, when the Steam info file changes (checked at most
// every 3 s) or after a connect/disconnect event. Requires glfwInit().
inline const PadInfo& identify(int jid) {
    static const PadInfo none{};
    if (jid < 0 || jid > GLFW_JOYSTICK_LAST) return none;
    auto& cached = detail::padCache()[jid];
    if (!glfwJoystickPresent(jid)) {
        cached = {};
        return none;
    }
    const char* guidText = glfwGetJoystickGUID(jid);
    const char* nameText = glfwGetJoystickName(jid);
    const std::string guid = guidText ? guidText : "", name = nameText ? nameText : "";
    const unsigned generation = detail::refreshSteamInfo();
    const auto ids = parseGuid(guid);
    const bool steamDependent = (ids.valid && isSteamVirtualGamepad(ids.vendor, ids.product)) || ids.xinput;
    if (cached.known && cached.guid == guid && cached.name == name &&
        (!steamDependent || cached.steamGeneration == generation) && cached.info.mapping == mappingSource(jid))
        return cached.info;

    PadInfo info;
    info.jid = jid;
    info.vendor = ids.vendor;
    info.product = ids.product;
    info.xinput = ids.xinput;
    info.joystickName = name;
    info.mapping = mappingSource(jid);
    if (glfwJoystickIsGamepad(jid))
        if (const char* mapping = glfwGetGamepadName(jid)) info.gamepadName = mapping;
    const bool deck = steamDeckEnvironment();
    const auto& slots = detail::steamInfo().slots;
    if (ids.valid && isSteamVirtualGamepad(ids.vendor, ids.product)) {
        info.steamVirtual = true;
        info.steamSlot = steamSlotFromName(name);
        const auto slot = slots.find(info.steamSlot);
        if (slot != slots.end()) detail::resolveFromSteamSlot(info, slot->second, deck);
        else info.family = classify(ids.vendor, ids.product, name, info.gamepadName, deck);
    } else if (ids.xinput && slots.size() == 1) {
        // Windows exposes Steam's virtual pad as XInput without a slot; the
        // info file is only trusted when it describes exactly one pad.
        info.steamVirtual = true;
        info.steamSlot = slots.begin()->first;
        detail::resolveFromSteamSlot(info, slots.begin()->second, deck);
    } else {
        info.family = classify(ids.vendor, ids.product, name, info.gamepadName, deck, ids.xinput);
    }
    cached.known = true;
    cached.guid = guid;
    cached.name = name;
    cached.steamGeneration = generation;
    cached.info = std::move(info);
    return cached.info;
}
// Installs a joystick callback that clears the identification cache on
// connect/disconnect and then calls any previously installed callback.
// Idempotent. Call after every glfwInit(): glfwTerminate() clears GLFW's
// callbacks (the game recreates its window per session), so this re-registers
// whenever the hook is no longer installed.
inline void joystickHook(int jid, int event) {
    detail::forgetPad(jid);
    if (auto previous = detail::previousJoystickCallback()) previous(jid, event);
}
inline void installJoystickCallback() {
    const GLFWjoystickfun current = glfwSetJoystickCallback(joystickHook);
    if (current == joystickHook) return;
    if (detail::joystickCallbackInstalled() && current) {
        // Still installed behind a callback chained later; keep that one.
        glfwSetJoystickCallback(current);
        return;
    }
    // Newly installed, or GLFW was terminated (which clears callbacks) and
    // initialised again: anything cached may describe a pad that has changed.
    detail::previousJoystickCallback() = current;
    detail::joystickCallbackInstalled() = true;
    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid) detail::forgetPad(jid);
}

// Activity score used to pick the pad in use (moved from Input.cpp): 2 for a
// held button, otherwise the largest stick deflection or trigger travel.
// jid >= 0: a fallback-mapped pad's sticks count from their rest position (restAxis), so a wheel pedal or slider
// pinned at -1 does not look held.
inline float gamepadActivity(const GLFWgamepadstate& state, int jid = -1) {
    float activity = 0.0f;
    for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; ++button)
        if (state.buttons[button] == GLFW_PRESS) activity = (std::max)(activity, 2.0f);
    for (int axis : { GLFW_GAMEPAD_AXIS_LEFT_X, GLFW_GAMEPAD_AXIS_LEFT_Y, GLFW_GAMEPAD_AXIS_RIGHT_X,
                      GLFW_GAMEPAD_AXIS_RIGHT_Y })
        activity = (std::max)(activity, std::abs(state.axes[axis] - restAxis(jid, axis)));
    // GLFW trigger axes rest at -1. Convert them to a zero-to-one range so
    // an untouched trigger does not make every connected pad look active.
    activity = (std::max)(activity, (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f);
    activity = (std::max)(activity, (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f);
    return activity;
}

// True for a physical pad that Steam also mirrors through a virtual pad: it
// is listed in SDL_GAMECONTROLLER_IGNORE_DEVICES and a 28de:11ff pad exists.
inline bool ignoredSteamDuplicate(int jid) {
    const char* list = std::getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    if (!list || !*list || !glfwJoystickPresent(jid)) return false;
    const char* guidText = glfwGetJoystickGUID(jid);
    const auto ids = parseGuid(guidText ? guidText : "");
    if (!ids.valid || isSteamVirtualGamepad(ids.vendor, ids.product)) return false;
    const auto ignored = parseIgnoreDevices(list);
    if (std::find(ignored.begin(), ignored.end(), std::make_pair(ids.vendor, ids.product)) == ignored.end())
        return false;
    for (int other = GLFW_JOYSTICK_1; other <= GLFW_JOYSTICK_LAST; ++other) {
        if (other == jid || !glfwJoystickPresent(other)) continue;
        const char* otherGuid = glfwGetJoystickGUID(other);
        const auto otherIds = parseGuid(otherGuid ? otherGuid : "");
        if (otherIds.valid && isSteamVirtualGamepad(otherIds.vendor, otherIds.product)) return true;
    }
    return false;
}

// Picks the mapped gamepad that is receiving the most input, preferring
// previousJid on ties (+0.01), exactly as the game has done since Steam may
// leave an idle virtual pad in a lower slot. Skips Steam-mirrored physical
// duplicates. Returns -1 when no gamepad is available; *stateOut receives the
// chosen pad's state (zeroed when none).
inline int selectActivePad(int previousJid, GLFWgamepadstate* stateOut = nullptr) {
    ensureFallbackMappings();   // (pads without a database mapping get an Xbox-layout one first)
    GLFWgamepadstate best{};
    int bestJid = -1;
    float bestActivity = -1.0f;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (!glfwJoystickPresent(jid) || !glfwJoystickIsGamepad(jid)) continue;
        if (ignoredSteamDuplicate(jid)) continue;
        GLFWgamepadstate candidate{};
        if (!glfwGetGamepadState(jid, &candidate)) continue;
        float activity = gamepadActivity(candidate, jid);
        if (jid == previousJid) activity += 0.01f;
        if (bestJid < 0 || activity > bestActivity) {
            best = candidate;
            bestJid = jid;
            bestActivity = activity;
        }
    }
    if (stateOut) *stateOut = bestJid >= 0 ? best : GLFWgamepadstate{};
    return bestJid;
}

// The last device the player used. A process-wide singleton (see tracker());
// deliberately not part of the game's JOY, which the network engine copies.
class ActivityTracker {
public:
    // Any key press (the game: a bound key; the launcher: any key).
    void noteKeyboard() { m_device = Device::Keyboard; }
    // Mouse movement or clicks. Only the launcher calls this.
    void noteMouse() { m_device = Device::Mouse; }
    // A VR controller was used (the VR code calls this on its controllers' input; family: an isVrFamily one, e.g.
    // MetaQuest on a Quest, ValveIndex / ViveWand / SteamFrame / Pico by the OpenXR interaction profile). Prompts then
    // show that family until a key, the mouse or a gamepad is used.
    void noteVr(Family family) {
        m_device = Device::Vr;
        m_vrFamily = family;
    }
    // Call once per frame. A newly pressed button or a stick/trigger that
    // crosses 0.5 makes that pad the active device. Before any input at all,
    // the first connected gamepad is adopted so prompts start out right.
    void pollGamepads() {
        ensureFallbackMappings();
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
            GLFWgamepadstate state{};
            if (!glfwJoystickPresent(jid) || !glfwJoystickIsGamepad(jid) || ignoredSteamDuplicate(jid) ||
                !glfwGetGamepadState(jid, &state)) {
                m_previous[jid] = 0;
                continue;
            }
            const uint32_t mask = activityMask(state, jid);
            const uint32_t pressed = mask & ~m_previous[jid];
            m_previous[jid] = mask;
            if (pressed || m_device == Device::None) activate(jid);
        }
    }
    // Keyboard when the keyboard or mouse was used last.
    Family family() const {
        if (m_forced) return *m_forced;
#ifdef CANE_VITA
        return Family::PlayStation;
#else
        return m_device == Device::Gamepad ? m_family : m_device == Device::Vr ? m_vrFamily : Family::Keyboard;
#endif
    }
    // The pad used last, or -1.
    int jid() const { return m_device == Device::Gamepad ? m_jid : -1; }
    bool lastWasGamepad() const {
        return m_forced ? *m_forced != Family::Keyboard : m_device == Device::Gamepad || m_device == Device::Vr;
    }
    bool lastWasVr() const { return m_forced ? isVrFamily(*m_forced) : m_device == Device::Vr; }
    bool lastWasMouse() const { return !m_forced && m_device == Device::Mouse; }
    // Nothing was used yet (and no gamepad was there to adopt).
    bool idle() const { return !m_forced && m_device == Device::None; }
    // Applies brand::testPromptFamilyEnv when set to a known family; input
    // then no longer changes family(). Returns whether a family was forced.
    bool forceFromEnv() {
        const char* value = std::getenv(brand::testPromptFamilyEnv);
        if (!value) return false;
        m_forced = parseFamily(value);
        return m_forced.has_value();
    }
    void force(std::optional<Family> family) { m_forced = family; }
    bool forced() const { return m_forced.has_value(); }

private:
    enum class Device { None, Keyboard, Mouse, Gamepad, Vr };
    static uint32_t activityMask(const GLFWgamepadstate& state, int jid) {
        uint32_t mask = 0;
        for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; ++button)
            if (state.buttons[button] == GLFW_PRESS) mask |= 1u << button;
        int bit = GLFW_GAMEPAD_BUTTON_LAST + 1;
        for (int axis : { GLFW_GAMEPAD_AXIS_LEFT_X, GLFW_GAMEPAD_AXIS_LEFT_Y, GLFW_GAMEPAD_AXIS_RIGHT_X,
                          GLFW_GAMEPAD_AXIS_RIGHT_Y }) {
            const float value = state.axes[axis] - restAxis(jid, axis);
            if (value > 0.5f) mask |= 1u << bit;
            if (value < -0.5f) mask |= 1u << (bit + 1);
            bit += 2;
        }
        for (int axis : { GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER })
            if ((state.axes[axis] + 1.0f) * 0.5f > 0.5f) mask |= 1u << bit++;
            else ++bit;
        return mask;
    }
    void activate(int jid) {
        m_device = Device::Gamepad;
        m_jid = jid;
        m_family = identify(jid).family;
    }
    Device m_device = Device::None;
    Family m_family = Family::Xbox;
    Family m_vrFamily = Family::MetaQuest;
    int m_jid = -1;
    std::array<uint32_t, GLFW_JOYSTICK_LAST + 1> m_previous{};
    std::optional<Family> m_forced;
};
inline ActivityTracker& tracker() {
    static ActivityTracker instance;
    return instance;
}

// The family to draw prompts for: "Original PlayStation Icons" wins,
// then an explicit style, then the last used device (Keyboard included).
inline Family promptFamily(PromptStyle style, bool alwaysOriginalPs) {
    if (alwaysOriginalPs) return Family::PlayStation;
    if (style != PromptStyle::Auto) return familyForStyle(style);
    return tracker().family();
}
inline Family promptFamily(int settingsPromptStyle, int alwaysOriginalPsPrompts) {
    const int style = std::clamp(settingsPromptStyle, 0, kPromptStyleLast);
    return promptFamily(static_cast<PromptStyle>(style), alwaysOriginalPsPrompts != 0);
}

// GLFW gamepad button 0-14, or the game's trigger pseudo-buttons 1000 (LT)
// and 1001 (RT). Anything else returns Button::Count.
inline Button fromGlfwBinding(int binding) {
    switch (binding) {
    case GLFW_GAMEPAD_BUTTON_A: return Button::South;
    case GLFW_GAMEPAD_BUTTON_B: return Button::East;
    case GLFW_GAMEPAD_BUTTON_X: return Button::West;
    case GLFW_GAMEPAD_BUTTON_Y: return Button::North;
    case GLFW_GAMEPAD_BUTTON_LEFT_BUMPER: return Button::LB;
    case GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER: return Button::RB;
    case GLFW_GAMEPAD_BUTTON_BACK: return Button::Back;
    case GLFW_GAMEPAD_BUTTON_START: return Button::Start;
    case GLFW_GAMEPAD_BUTTON_GUIDE: return Button::Guide;
    case GLFW_GAMEPAD_BUTTON_LEFT_THUMB: return Button::LSPress;
    case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB: return Button::RSPress;
    case GLFW_GAMEPAD_BUTTON_DPAD_UP: return Button::DpadUp;
    case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT: return Button::DpadRight;
    case GLFW_GAMEPAD_BUTTON_DPAD_DOWN: return Button::DpadDown;
    case GLFW_GAMEPAD_BUTTON_DPAD_LEFT: return Button::DpadLeft;
    case settings::kGamepadLeftTrigger: return Button::LT;
    case settings::kGamepadRightTrigger: return Button::RT;
    default: return Button::Count;
    }
}

// Text label of a positional button for a family. Keyboard returns the
// game's historic neutral names ("A / Cross"), used when no pad is active.
// Button::Count returns "Unbound".
inline const char* buttonLabel(Button button, Family family) {
    // Columns: Keyboard, PlayStation, Xbox, Nintendo, SteamDeck, SteamController, PS4, PS5, SteamController2026,
    // GameCube, Switch2, Wii, WiiU, MetaQuest, ValveIndex, SteamFrame, ViveWand, Pico. The GameCube is positional as SDL maps it: B west, X east, Z the right bumper,
    // L / R the triggers; its left bumper is the Switch Online pad's ZL.
    static constexpr const char* table[kButtonCount][kFamilyCount] = {
        { "A / Cross", "Cross", "A", "B", "A", "A", "Cross", "Cross", "A", "A", "B", "B", "B", "A", "A", "A", "R Pad", "A" },                                   // South
        { "B / Circle", "Circle", "B", "A", "B", "B", "Circle", "Circle", "B", "X", "A", "A", "A", "B", "B", "B", "R Menu", "B" },                                 // East
        { "X / Square", "Square", "X", "Y", "X", "X", "Square", "Square", "X", "B", "Y", "Y", "Y", "X", "L A", "X", "L Pad", "X" },                                 // West
        { "Y / Triangle", "Triangle", "Y", "X", "Y", "Y", "Triangle", "Triangle", "Y", "Y", "X", "X", "X", "Y", "L B", "Y", "L Menu", "Y" },                             // North
        { "Left Bumper", "L1", "LB", "L", "L1", "LB", "L1", "L1", "L1", "ZL", "L", "L", "L", "L Grip", "L Grip", "L1", "L Grip", "L Grip" },                                 // LB
        { "Right Bumper", "R1", "RB", "R", "R1", "RB", "R1", "R1", "R1", "Z", "R", "R", "R", "R Grip", "R Grip", "R1", "R Grip", "R Grip" },                                // RB
        { "Left Trigger", "L2", "LT", "ZL", "L2", "LT", "L2", "L2", "L2", "L", "ZL", "ZL", "ZL", "L Trigger", "L Trigger", "L2", "L Trigger", "L Trigger" },                               // LT
        { "Right Trigger", "R2", "RT", "ZR", "R2", "RT", "R2", "R2", "R2", "R", "ZR", "ZR", "ZR", "R Trigger", "R Trigger", "R2", "R Trigger", "R Trigger" },                              // RT
        { "Back / Select", "Select", "View", "-", "View", "Back", "Share", "Create", "View", "Select", "-", "-", "-", "Menu", "L Pad", "View", "Menu", "Menu" },                     // Back
        { "Start", "Start", "Menu", "+", "Menu", "Start", "Options", "Options", "Menu", "Start", "+", "+", "+", "Menu", "System", "Menu", "Menu", "Menu" },                          // Start
        { "Left Stick", "Left stick", "Left stick", "Left stick", "Left stick", "Stick", "Left stick", "Left stick", "Left stick", "Control stick", "Left stick", "Left stick", "Left stick", "Left stick", "Left stick", "Left stick", "Left pad", "Left stick" },          // LS
        { "Right Stick", "Right stick", "Right stick", "Right stick", "Right stick", "Right pad", "Right stick", "Right stick", "Right stick", "C-stick", "Right stick", "Right stick", "Right stick", "Right stick", "Right stick", "Right stick", "Right pad", "Right stick" },   // RS
        { "Left Stick", "L3", "LS", "L-stick", "L3", "Stick click", "L3", "L3", "L3", "L3", "L-stick", "L-stick", "L-stick", "L3", "L3", "L3", "L Pad click", "L3" },                   // LSPress
        { "Right Stick", "R3", "RS", "R-stick", "R3", "Pad click", "R3", "R3", "R3", "R3", "R-stick", "R-stick", "R-stick", "R3", "R3", "R3", "R Pad click", "R3" },                    // RSPress
        { "D-Pad Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up", "Up" },                                   // DpadUp
        { "D-Pad Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down", "Down" },                       // DpadDown
        { "D-Pad Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left", "Left" },                       // DpadLeft
        { "D-Pad Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right" },                 // DpadRight
        { "D-Pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad", "D-pad" },                       // Dpad
        { "Guide", "PS", "Xbox", "Home", "Steam", "Steam", "PS", "PS", "Steam", "Home", "Home", "Home", "Home", "Meta", "System", "Steam", "System", "Home" },                            // Guide
    };
    const int index = static_cast<int>(button), column = static_cast<int>(family);
    if (index < 0 || index >= kButtonCount || column < 0 || column >= kFamilyCount) return "Unbound";
    return table[index][column];
}

// ---- button names inside text (the API other code uses to put a button's icon or name into a string)
// A button's name in any family's words, case-insensitive: the positional names (South, East, ..., DpadUp), the
// PlayStation names (Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Select, Start, Share, Create, Options),
// the Xbox ones (A, B, X, Y, LB, RB, LT, RT, LS, RS, View, Menu, Back) and Up / Down / Left / Right / Dpad / Guide /
// Home / PS. Letters are Xbox positions (A south). nullopt for anything else.
inline std::optional<Button> buttonFromName(std::string_view name) {
    struct Entry { const char* name; Button button; };
    static constexpr Entry entries[] = {
        {"south", Button::South}, {"east", Button::East}, {"west", Button::West}, {"north", Button::North},
        {"cross", Button::South}, {"circle", Button::East}, {"square", Button::West}, {"triangle", Button::North},
        {"a", Button::South}, {"b", Button::East}, {"x", Button::West}, {"y", Button::North},
        {"lb", Button::LB}, {"rb", Button::RB}, {"lt", Button::LT}, {"rt", Button::RT},
        {"l1", Button::LB}, {"r1", Button::RB}, {"l2", Button::LT}, {"r2", Button::RT},
        {"l3", Button::LSPress}, {"r3", Button::RSPress}, {"lspress", Button::LSPress}, {"rspress", Button::RSPress},
        {"ls", Button::LS}, {"rs", Button::RS}, {"lstick", Button::LS}, {"rstick", Button::RS},
        {"back", Button::Back}, {"select", Button::Back}, {"view", Button::Back}, {"share", Button::Back},
        {"create", Button::Back}, {"minus", Button::Back},
        {"start", Button::Start}, {"menu", Button::Start}, {"options", Button::Start}, {"plus", Button::Start},
        {"up", Button::DpadUp}, {"down", Button::DpadDown}, {"left", Button::DpadLeft}, {"right", Button::DpadRight},
        {"dpadup", Button::DpadUp}, {"dpaddown", Button::DpadDown}, {"dpadleft", Button::DpadLeft},
        {"dpadright", Button::DpadRight}, {"dpad", Button::Dpad}, {"d-pad", Button::Dpad},
        {"guide", Button::Guide}, {"home", Button::Guide}, {"ps", Button::Guide},
    };
    const auto value = detail::lowered(detail::trim(name));
    for (const Entry& e : entries)
        if (value == e.name) return e.button;
    return std::nullopt;
}
// "{L3}"-style tokens in text: the first "{NAME}" at or after `from` whose NAME is a buttonFromName() name. begin ==
// npos when there is none. Braces around anything else are left alone.
struct ButtonToken {
    size_t begin = std::string_view::npos, end = std::string_view::npos; // [begin, end) covers the braces
    Button button = Button::Count;
};
inline ButtonToken findButtonToken(std::string_view text, size_t from = 0) {
    while (from < text.size()) {
        const size_t open = text.find('{', from);
        if (open == std::string_view::npos) break;
        const size_t close = text.find('}', open + 1);
        if (close == std::string_view::npos) break;
        if (close - open - 1 <= 16)
            if (auto button = buttonFromName(text.substr(open + 1, close - open - 1))) return {open, close + 1, *button};
        from = open + 1;
    }
    return {};
}
// Replaces every button token with buttonLabel(button, family): for text drawn without icons.
inline std::string replaceButtonTokens(std::string_view text, Family family) {
    std::string out;
    size_t at = 0;
    for (ButtonToken t = findButtonToken(text); t.begin != std::string_view::npos; t = findButtonToken(text, at)) {
        out.append(text.substr(at, t.begin - at));
        out += buttonLabel(t.button, family);
        at = t.end;
    }
    out.append(text.substr(at));
    return out;
}

// The retail font-1 glyph (Splash.brx) for a positional button, used to draw
// the PS button that is actually bound. '\0' when the font has none (Guide).
inline char psGlyphFor(Button button) {
    switch (button) {
    case Button::South: return 'X';
    case Button::East: return 'O';
    case Button::West: return 'S';
    case Button::North: return 'T';
    case Button::LB: return 'L';
    case Button::RB: return 'R';
    case Button::LT: return 'l';
    case Button::RT: return 'r';
    case Button::Back: return '_';
    case Button::Start: return '!';
    case Button::LS: case Button::LSPress: return '(';
    case Button::RS: case Button::RSPress: return ')';
    case Button::DpadUp: return '^';
    case Button::DpadDown: return 'v';
    case Button::DpadLeft: return '<';
    case Button::DpadRight: return '>';
    case Button::Dpad: return '+';
    case Button::Guide: case Button::Count: break;
    }
    return '\0';
}
} // namespace cane::input
