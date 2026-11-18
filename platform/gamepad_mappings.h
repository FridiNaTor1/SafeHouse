#pragma once
// Bundled SDL_GameControllerDB mappings (zlib), applied after glfwInit() by
// both the game and the launcher so pads missing from GLFW's built-in table
// still count as gamepads, plus a fallback for every pad no mapping names.
//
// cmake/prompts.cmake embeds Libraries/gamecontrollerdb/gamecontrollerdb.txt
// and defines CANE_HAVE_GAMECONTROLLERDB. Without it GLFW's built-in mappings
// are kept; the environment's SDL_GAMECONTROLLERCONFIG is still applied.
//
// GLFW only treats a joystick as a gamepad when a mapping line carries its
// exact GUID, version included. SDL also matches a line for the same bus /
// vendor / product with another version, and on Linux builds a mapping from
// the device's evdev button codes. ensureFallbackMappings() does the same for
// every joystick GLFW lists without a mapping, in this order:
//   1. Database: a bundled (or SDL_GAMECONTROLLERCONFIG) line for the same bus, vendor and product, any version
//      (e.g. a HyperX Clutch whose firmware reports another version than the database's).
//   2. Evdev (Linux): SDL's rules on the device's button / axis codes read from sysfs (/sys/class/input/eventN):
//      BTN_A / BTN_B / BTN_X / BTN_Y, the bumpers, Select / Start / Mode, the stick clicks, ABS_X/Y + ABS_RX/RY
//      (triggers ABS_Z / ABS_RZ) or ABS_Z/RZ (triggers ABS_BRAKE / ABS_GAS or BTN_TL2 / TR2), the hat or
//      BTN_DPAD_*. This covers xpad-driven pads GLFW does not know and Android-layout HID pads.
//   3. Generic: an Xbox layout by index (XInput's button order, axes X Y [LT] RX RY [RT], hat 0), limited to the
//      buttons / axes / hats the device has.
// Devices that do not look like a pad (touchpads, motion sensors, microphones' volume dials, ...) are left alone.
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#if defined(__linux__) && !defined(__ANDROID__)
#include <filesystem>
#include <fstream>
#include <sstream>
#endif
namespace cane::input {
#ifdef CANE_HAVE_GAMECONTROLLERDB
namespace embedded {
std::string_view gameControllerDb(); // assets/prompts/prompt_data.cpp
}
#endif
// Where a joystick's gamepad mapping came from (logs, the launcher's Controls page).
enum class MappingSource : uint8_t { None, Builtin, Database, Evdev, Generic };
inline const char* mappingSourceId(MappingSource source) {
    switch (source) {
    case MappingSource::None: return "none";
    case MappingSource::Builtin: return "builtin";
    case MappingSource::Database: return "database";
    case MappingSource::Evdev: return "evdev";
    case MappingSource::Generic: return "generic";
    }
    return "none";
}

// What GLFW (and on Linux sysfs) tells about a joystick: everything the fallback needs, so tests can describe a
// fake device.
struct RawJoystick {
    std::string guid, name;
    int axes = 0, buttons = 0, hats = 0;
    // Linux: the evdev capability bits (EV_KEY codes 0..KEY_MAX, EV_ABS codes 0..ABS_MAX), when found in sysfs.
    bool evdev = false;
    std::vector<bool> keyBits, absBits;
    bool key(int code) const { return code >= 0 && code < static_cast<int>(keyBits.size()) && keyBits[code]; }
    bool abs(int code) const { return code >= 0 && code < static_cast<int>(absBits.size()) && absBits[code]; }
    void setKey(int code) {
        if (code >= static_cast<int>(keyBits.size())) keyBits.resize(code + 1);
        keyBits[code] = true;
        evdev = true;
    }
    void setAbs(int code) {
        if (code >= static_cast<int>(absBits.size())) absBits.resize(code + 1);
        absBits[code] = true;
        evdev = true;
    }
    // GLFW's counts from the evdev bits, as GLFW's Linux backend enumerates them (linux_joystick.c).
    void countFromEvdev() {
        buttons = axes = hats = 0;
        for (int code = 0x100; code < static_cast<int>(keyBits.size()); ++code) buttons += keyBits[code] ? 1 : 0;
        for (int code = 0; code < static_cast<int>(absBits.size()); ++code) {
            if (!absBits[code]) continue;
            if (code >= 0x10 && code <= 0x17) {
                ++hats;
                ++code; // (GLFW skips the hat's Y code)
            } else
                ++axes;
        }
    }
};

namespace detail {
// The "platform:" value GLFW matches mappings against (glfw getMappingName).
inline constexpr std::string_view kMappingPlatform =
#if defined(_WIN32)
    "Windows";
#elif defined(CANE_IOS)
    "iOS";   // (SDL's name for it; the iOS app's GLFW is SDL, platform/ios/glfw_sdl_ios.cpp)
#elif defined(__APPLE__)
    "Mac OS X";
#elif defined(__ANDROID__)
    "Android";
#else
    "Linux";
#endif
// Keeps only mapping lines GLFW would accept for this platform: a 32-digit hex
// GUID, a name shorter than 128 bytes, and either a
// matching "platform:" field or none (SDL_GAMECONTROLLERCONFIG style). GLFW
// reports every malformed line through the error callback, and mappings for
// other platforms are useless here, so filtering keeps logs quiet and the
// update fast.
inline std::string filterMappings(std::string_view text, std::string_view platform = kMappingPlatform) {
    std::string out;
    while (!text.empty()) {
        const size_t end = text.find_first_of("\r\n");
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const size_t guidEnd = line.find(',');
        if (guidEnd != 32 || line.find_first_not_of("0123456789abcdefABCDEF") < 32) continue;
        const size_t nameEnd = line.find(',', guidEnd + 1);
        if (nameEnd == std::string_view::npos || nameEnd - guidEnd - 1 >= 128) continue;
        const size_t at = line.find("platform:", nameEnd);
        if (at != std::string_view::npos) {
            const std::string_view value = line.substr(at + 9, line.find(',', at) - at - 9);
            if (value != platform) continue;
        }
        out.append(line);
        out.push_back('\n');
    }
    return out;
}
inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// A 16-bit little-endian field of an SDL GUID ("5e04" -> 0x045e), -1 if malformed.
inline int guidLe16(std::string_view guid, size_t at) {
    if (guid.size() < at + 4) return -1;
    int nibbles[4];
    for (int i = 0; i < 4; ++i)
        if ((nibbles[i] = hexNibble(guid[at + i])) < 0) return -1;
    return (nibbles[0] << 4 | nibbles[1]) | (nibbles[2] << 4 | nibbles[3]) << 8;
}
// bus, vendor, product, version of an SDL 2.0.5+ GUID in the ID form (bus, crc, vendor 0000 product 0000 version
// 0000). SDL 2.26+ puts a name CRC in the second field; it is ignored.
struct GuidFields {
    int bus = -1, vendor = -1, product = -1, version = -1;
    bool idForm = false;
};
inline GuidFields guidFields(std::string_view guid) {
    GuidFields f;
    if (guid.size() < 32) return f;
    f.bus = guidLe16(guid, 0);
    if (guid.substr(12, 4) != "0000" || guid.substr(20, 4) != "0000" || guid.substr(28, 4) != "0000") return f;
    f.vendor = guidLe16(guid, 8);
    f.product = guidLe16(guid, 16);
    f.version = guidLe16(guid, 24);
    f.idForm = f.bus >= 0 && f.vendor > 0 && f.product >= 0 && f.version >= 0;
    return f;
}
inline std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace detail

// True when every element a mapping line names exists on a device with these counts (GLFW drops a mapping that
// names a missing element, and cannot parse output modifiers such as "+rightx:").
inline bool mappingFits(std::string_view line, int axes, int buttons, int hats) {
    size_t at = line.find(',');
    if (at == std::string_view::npos) return false;
    at = line.find(',', at + 1);
    if (at == std::string_view::npos) return false;
    bool any = false;
    while (at != std::string_view::npos && at + 1 < line.size()) {
        const size_t next = line.find(',', at + 1);
        const std::string_view field = line.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
        at = next;
        if (field.empty()) continue;
        const size_t colon = field.find(':');
        if (colon == std::string_view::npos || colon == 0) return false;
        const std::string_view key = field.substr(0, colon);
        std::string_view value = field.substr(colon + 1);
        if (key == "platform") continue;
        if (key.front() == '+' || key.front() == '-') return false;
        if (!value.empty() && (value.front() == '+' || value.front() == '-')) value.remove_prefix(1);
        if (value.empty()) return false;
        const char kind = value.front();
        value.remove_prefix(1);
        int index = 0;
        size_t digits = 0;
        while (digits < value.size() && std::isdigit(static_cast<unsigned char>(value[digits])))
            index = index * 10 + (value[digits++] - '0');
        if (!digits) return false;
        if (kind == 'a' && index >= axes) return false;
        if (kind == 'b' && index >= buttons) return false;
        if (kind == 'h' && index >= hats) return false;
        if (kind != 'a' && kind != 'b' && kind != 'h') return false;
        any = true;
    }
    return any;
}

// 1. A database line for the same bus / vendor / product as the pad, its own version first, then the nearest
// version; returned with the pad's GUID and the database's name. Empty when none fits.
inline std::string databaseMapping(const RawJoystick& pad, std::string_view db) {
    const auto want = detail::guidFields(pad.guid);
    if (!want.idForm) return {};
    std::string best;
    int bestDistance = 0x10000;
    while (!db.empty()) {
        const size_t end = db.find_first_of("\r\n");
        std::string_view line = db.substr(0, end);
        db = end == std::string_view::npos ? std::string_view{} : db.substr(end + 1);
        if (line.size() < 34 || line[32] != ',') continue;
        const auto have = detail::guidFields(line.substr(0, 32));
        if (!have.idForm || have.bus != want.bus || have.vendor != want.vendor || have.product != want.product) continue;
        if (!mappingFits(line, pad.axes, pad.buttons, pad.hats)) continue;
        const int distance = std::abs(have.version - want.version);
        if (distance >= bestDistance) continue;
        bestDistance = distance;
        best = pad.guid + std::string(line.substr(32));
        if (!distance) break;
    }
    return best;
}

namespace detail {
// GLFW's index of an evdev element (linux_joystick.c): buttons are the set key codes from BTN_MISC (0x100) up, in
// code order; axes the set ABS codes except the hats (0x10..0x17), whose X / Y pair is one hat.
inline int evdevButton(const RawJoystick& pad, int code) {
    if (!pad.key(code)) return -1;
    int index = 0;
    for (int c = 0x100; c < code; ++c) index += pad.key(c) ? 1 : 0;
    return index;
}
inline int evdevAxis(const RawJoystick& pad, int code) {
    if (!pad.abs(code) || (code >= 0x10 && code <= 0x17)) return -1;
    int index = 0;
    for (int c = 0; c < code; ++c)
        if (pad.abs(c) && !(c >= 0x10 && c <= 0x17)) ++index;
    return index;
}
inline int evdevHat0(const RawJoystick& pad) {
    // (hat 0 is GLFW's first hat when ABS_HAT0X or ABS_HAT0Y is set)
    return pad.abs(0x10) || pad.abs(0x11) ? 0 : -1;
}
struct MappingWriter {
    std::string line;
    void button(const char* name, int index) {
        if (index >= 0) line += std::string(name) + ":b" + std::to_string(index) + ",";
    }
    void axis(const char* name, int index) {
        if (index >= 0) line += std::string(name) + ":a" + std::to_string(index) + ",";
    }
    void hat(int hat) {
        if (hat < 0) return;
        const std::string h = "h" + std::to_string(hat) + ".";
        line += "dpup:" + h + "1,dpright:" + h + "2,dpdown:" + h + "4,dpleft:" + h + "8,";
    }
};
inline std::string mappingName(std::string_view name) {
    std::string out;
    for (char c : name) // (no commas; GLFW keeps names under 128 bytes)
        if (c != ',' && out.size() < 120) out.push_back(c);
    return out.empty() ? std::string("Controller") : out;
}
inline constexpr uint16_t kSony = 0x054c, kNintendo = 0x057e;
} // namespace detail

// Whether a joystick looks like a game pad (and not a touchpad, motion sensor, microphone dial, keyboard, ...).
inline bool looksLikeGamepad(const RawJoystick& pad) {
    const std::string n = detail::lowerAscii(pad.name);
    static constexpr const char* notPads[] = {
        "touchpad", "touch pad", "motion sensor", "accelerometer", "gyro", " imu", "mouse", "keyboard",
        "consumer control", "system control", "headset", "microphone", "quadcast", "speaker", "stylus", "tablet",
        "wacom", "power button", "sleep button", "video bus", "hotkey", "trackpad", "nunchuk", "balance board",
    };
    for (const char* word : notPads)
        if (n.find(word) != std::string::npos) return false;
    if (n.size() >= 3 && n.compare(n.size() - 3, 3, " ir") == 0) return false; // (the Wii Remote's IR camera)
    if (pad.evdev) {
        const bool sticks = (pad.abs(0x00) && pad.abs(0x01)) || pad.abs(0x10);
        bool padButtons = false;
        for (int code = 0x120; code <= 0x13f && !padButtons; ++code) padButtons = pad.key(code);   // BTN_JOYSTICK, BTN_GAMEPAD
        for (int code = 0x220; code <= 0x223 && !padButtons; ++code) padButtons = pad.key(code);   // BTN_DPAD_*
        for (int code = 0x2c0; code <= 0x2e7 && !padButtons; ++code) padButtons = pad.key(code);   // BTN_TRIGGER_HAPPY*
        if (!sticks || !padButtons) return false;
    }
    return pad.axes + 2 * pad.hats >= 2 && pad.buttons >= 4;
}

// 2. Linux: a mapping from the evdev codes (SDL's rules). Empty without the evdev bits or without BTN_A (south).
inline std::string evdevMapping(const RawJoystick& pad) {
    using detail::evdevAxis;
    using detail::evdevButton;
    if (!pad.evdev || !pad.key(0x130)) return {};
    const auto ids = detail::guidFields(pad.guid);
    // xpad and most HID pads report the X / Y labels as BTN_X (0x133) / BTN_Y (0x134); the Sony and Nintendo
    // drivers follow the kernel's positional BTN_NORTH (== BTN_X) / BTN_WEST (== BTN_Y).
    const bool positional = ids.idForm && (ids.vendor == detail::kSony || ids.vendor == detail::kNintendo);
    detail::MappingWriter m;
    m.line = pad.guid + "," + detail::mappingName(pad.name) + ",";
    m.button("a", evdevButton(pad, 0x130));
    m.button("b", evdevButton(pad, 0x131));
    m.button("x", evdevButton(pad, positional ? 0x134 : 0x133));
    m.button("y", evdevButton(pad, positional ? 0x133 : 0x134));
    m.button("leftshoulder", evdevButton(pad, 0x136));
    m.button("rightshoulder", evdevButton(pad, 0x137));
    m.button("back", evdevButton(pad, 0x13a));
    m.button("start", evdevButton(pad, 0x13b));
    m.button("guide", evdevButton(pad, 0x13c));
    m.button("leftstick", evdevButton(pad, 0x13d));
    m.button("rightstick", evdevButton(pad, 0x13e));
    m.axis("leftx", evdevAxis(pad, 0x00));
    m.axis("lefty", evdevAxis(pad, 0x01));
    int leftTrigger = -1, rightTrigger = -1;
    if (pad.abs(0x03) && pad.abs(0x04)) {           // ABS_RX / ABS_RY: the right stick (xpad), triggers ABS_Z / ABS_RZ
        m.axis("rightx", evdevAxis(pad, 0x03));
        m.axis("righty", evdevAxis(pad, 0x04));
        leftTrigger = evdevAxis(pad, 0x02);
        rightTrigger = evdevAxis(pad, 0x05);
    } else if (pad.abs(0x02) && pad.abs(0x05)) {    // ABS_Z / ABS_RZ: the right stick (HID / Android layout)
        m.axis("rightx", evdevAxis(pad, 0x02));
        m.axis("righty", evdevAxis(pad, 0x05));
    }
    if (leftTrigger < 0) leftTrigger = evdevAxis(pad, 0x0a);    // ABS_BRAKE
    if (rightTrigger < 0) rightTrigger = evdevAxis(pad, 0x09);  // ABS_GAS
    if (leftTrigger >= 0) m.axis("lefttrigger", leftTrigger);
    else m.button("lefttrigger", evdevButton(pad, 0x138));      // BTN_TL2
    if (rightTrigger >= 0) m.axis("righttrigger", rightTrigger);
    else m.button("righttrigger", evdevButton(pad, 0x139));     // BTN_TR2
    if (detail::evdevHat0(pad) >= 0) m.hat(0);
    else {
        m.button("dpup", evdevButton(pad, 0x220));
        m.button("dpdown", evdevButton(pad, 0x221));
        m.button("dpleft", evdevButton(pad, 0x222));
        m.button("dpright", evdevButton(pad, 0x223));
    }
    m.line += "platform:" + std::string(detail::kMappingPlatform) + ",";
    return m.line;
}

// 3. An Xbox layout by index, for any pad: XInput's button order (A B X Y LB RB Back Start LS RS Guide), the axes
// X Y LT RX RY RT with six or more axes (xpad / DirectInput order), X Y RX RY with four or five (the triggers then
// buttons 6 / 7 and Back / Start / LS / RS 8..11 when the pad has 12 buttons, the common DirectInput pad), the
// left stick alone with fewer; the first hat as the d-pad.
inline std::string genericMapping(const RawJoystick& pad) {
    detail::MappingWriter m;
    m.line = pad.guid + "," + detail::mappingName(pad.name) + ",";
    auto b = [&](const char* name, int index) { m.button(name, index < pad.buttons ? index : -1); };
    auto a = [&](const char* name, int index) { m.axis(name, index < pad.axes ? index : -1); };
    b("a", 0);
    b("b", 1);
    b("x", 2);
    b("y", 3);
    b("leftshoulder", 4);
    b("rightshoulder", 5);
    a("leftx", 0);
    a("lefty", 1);
    if (pad.axes >= 6) {
        a("lefttrigger", 2);
        a("rightx", 3);
        a("righty", 4);
        a("righttrigger", 5);
        b("back", 6);
        b("start", 7);
        b("leftstick", 8);
        b("rightstick", 9);
        b("guide", 10);
    } else if (pad.axes >= 4 && pad.buttons >= 12) {
        a("rightx", 2);
        a("righty", 3);
        b("lefttrigger", 6);
        b("righttrigger", 7);
        b("back", 8);
        b("start", 9);
        b("leftstick", 10);
        b("rightstick", 11);
        b("guide", 12);
    } else {
        if (pad.axes >= 4) {
            a("rightx", 2);
            a("righty", 3);
        }
        b("back", 6);
        b("start", 7);
        b("leftstick", 8);
        b("rightstick", 9);
        b("guide", 10);
    }
    if (pad.hats > 0) m.hat(0);
    m.line += "platform:" + std::string(detail::kMappingPlatform) + ",";
    return m.line;
}

// The mapping ensureFallbackMappings() would add for a pad without one: Database, then Evdev, then Generic. Empty
// (source None) when the device does not look like a pad.
inline std::string fallbackMapping(const RawJoystick& pad, std::string_view db, MappingSource* source = nullptr) {
    if (source) *source = MappingSource::None;
    if (pad.guid.size() != 32 || !looksLikeGamepad(pad)) return {};
    if (std::string line = databaseMapping(pad, db); !line.empty()) {
        if (source) *source = MappingSource::Database;
        return line;
    }
    if (std::string line = evdevMapping(pad); !line.empty() && mappingFits(line, pad.axes, pad.buttons, pad.hats)) {
        if (source) *source = MappingSource::Evdev;
        return line;
    }
    std::string line = genericMapping(pad);
    if (!mappingFits(line, pad.axes, pad.buttons, pad.hats)) return {};
    if (source) *source = MappingSource::Generic;
    return line;
}

#if defined(__linux__) && !defined(__ANDROID__)
namespace detail {
// A sysfs capability bitmap ("1f 0 3800000000"): words of a C long, most significant first.
inline std::vector<bool> parseCapabilityBits(std::string_view text) {
    std::vector<std::string_view> words;
    while (!text.empty()) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        size_t end = 0;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        if (end) words.push_back(text.substr(0, end));
        text.remove_prefix(end);
    }
    constexpr int kWordBits = static_cast<int>(sizeof(long) * 8);
    std::vector<bool> bits(words.size() * kWordBits);
    for (size_t w = 0; w < words.size(); ++w) {
        const std::string_view word = words[words.size() - 1 - w];
        int bit = 0;
        for (size_t i = word.size(); i-- > 0 && bit < kWordBits; bit += 4) {
            const int nibble = hexNibble(word[i]);
            if (nibble < 0) return {};
            for (int k = 0; k < 4; ++k)
                if (nibble >> k & 1) bits[w * kWordBits + bit + k] = true;
        }
    }
    return bits;
}
inline std::string readSmallFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string out = text.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
inline int readHexFile(const std::filesystem::path& path) {
    const std::string text = readSmallFile(path);
    if (text.empty()) return -1;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 16);
    return end && *end == '\0' ? static_cast<int>(value) : -1;
}
} // namespace detail
// Fills the evdev bits of a joystick from /sys/class/input (root: tests point it at a fake tree): the event device
// with the joystick's name and, for an ID-form GUID, its bus / vendor / product / version. False when none matches.
inline bool readEvdevCapabilities(RawJoystick& pad, const std::filesystem::path& root = "/sys/class/input") {
    std::error_code ec;
    const auto want = detail::guidFields(pad.guid);
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const std::string leaf = entry.path().filename().string();
        if (leaf.rfind("event", 0) != 0) continue;
        const auto device = entry.path() / "device";
        if (detail::readSmallFile(device / "name") != pad.name) continue;
        if (want.idForm &&
            (detail::readHexFile(device / "id" / "bustype") != want.bus ||
             detail::readHexFile(device / "id" / "vendor") != want.vendor ||
             detail::readHexFile(device / "id" / "product") != want.product ||
             detail::readHexFile(device / "id" / "version") != want.version))
            continue;
        pad.keyBits = detail::parseCapabilityBits(detail::readSmallFile(device / "capabilities" / "key"));
        pad.absBits = detail::parseCapabilityBits(detail::readSmallFile(device / "capabilities" / "abs"));
        pad.evdev = true;
        return true;
    }
    return false;
}
#endif

namespace detail {
struct FallbackState {
    bool dbReady = false;
    std::string db; // the bundled lines for this platform, then SDL_GAMECONTROLLERCONFIG's
    std::array<std::string, GLFW_JOYSTICK_LAST + 1> seen{};  // guid + name last looked at, per jid
    std::array<MappingSource, GLFW_JOYSTICK_LAST + 1> source{};
    std::array<std::array<float, 6>, GLFW_JOYSTICK_LAST + 1> rest{}; // fallback pads' axes at rest (see restAxis)
};
inline FallbackState& fallbackState() {
    static FallbackState state;
    return state;
}
inline const std::string& fallbackDatabase() {
    auto& state = fallbackState();
    if (!state.dbReady) {
        state.dbReady = true;
#ifdef CANE_HAVE_GAMECONTROLLERDB
        state.db = filterMappings(embedded::gameControllerDb());
#endif
        if (const char* environment = std::getenv("SDL_GAMECONTROLLERCONFIG")) state.db += filterMappings(environment);
    }
    return state.db;
}
} // namespace detail

// Applies the bundled database, then SDL_GAMECONTROLLERCONFIG (Steam passes
// its virtual pads' mappings there), so later entries win per GUID. Call
// after glfwInit(). Returns false only when GLFW rejected the update.
// Also forgets the fallback mappings (glfwTerminate dropped them).
inline bool applyBundledMappings() {
    bool ok = true;
    auto& state = detail::fallbackState();
    state.seen = {};
    state.source = {};
    state.rest = {};
#ifdef CANE_HAVE_GAMECONTROLLERDB
    const std::string bundled = detail::filterMappings(embedded::gameControllerDb());
    if (!bundled.empty()) ok = glfwUpdateGamepadMappings(bundled.c_str()) == GLFW_TRUE;
#endif
    if (const char* environment = std::getenv("SDL_GAMECONTROLLERCONFIG")) {
        const std::string extra = detail::filterMappings(environment);
        if (!extra.empty()) ok = glfwUpdateGamepadMappings(extra.c_str()) == GLFW_TRUE && ok;
    }
    return ok;
}

// A connected joystick as RawJoystick (GLFW's GUID, name and counts; on Linux the sysfs evdev bits).
inline RawJoystick rawJoystick(int jid) {
    RawJoystick pad;
    if (!glfwJoystickPresent(jid)) return pad;
    const char* guid = glfwGetJoystickGUID(jid);
    const char* name = glfwGetJoystickName(jid);
    pad.guid = guid ? guid : "";
    pad.name = name ? name : "";
    glfwGetJoystickAxes(jid, &pad.axes);
    glfwGetJoystickButtons(jid, &pad.buttons);
    glfwGetJoystickHats(jid, &pad.hats);
#if defined(__linux__) && !defined(__ANDROID__)
    readEvdevCapabilities(pad);
#endif
    return pad;
}

// Gives every connected joystick that GLFW does not map, and that looks like a pad, a fallback mapping (see the top
// of this file). Cheap when nothing changed: call it every frame before reading pads (selectActivePad does).
// Requires glfwInit(); call applyBundledMappings() first.
inline void ensureFallbackMappings() {
    auto& state = detail::fallbackState();
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (!glfwJoystickPresent(jid)) {
            if (!state.seen[jid].empty()) {
                state.seen[jid].clear();
                state.source[jid] = MappingSource::None;
            }
            continue;
        }
        const char* guid = glfwGetJoystickGUID(jid);
        const char* name = glfwGetJoystickName(jid);
        std::string key = std::string(guid ? guid : "") + '\n' + (name ? name : "");
        if (state.seen[jid] == key) continue;
        state.seen[jid] = std::move(key);
        state.rest[jid] = {};
        if (glfwJoystickIsGamepad(jid)) {
            state.source[jid] = MappingSource::Builtin;
            continue;
        }
        state.source[jid] = MappingSource::None;
        const RawJoystick pad = rawJoystick(jid);
        MappingSource source = MappingSource::None;
        std::string line = fallbackMapping(pad, detail::fallbackDatabase(), &source);
        // (a line GLFW still rejects falls through to the next kind)
        while (!line.empty()) {
            glfwUpdateGamepadMappings((line + "\n").c_str());
            if (glfwJoystickIsGamepad(jid)) break;
            if (source == MappingSource::Database) {
                line = evdevMapping(pad);
                source = MappingSource::Evdev;
                if (line.empty() || !mappingFits(line, pad.axes, pad.buttons, pad.hats)) {
                    line = genericMapping(pad);
                    source = MappingSource::Generic;
                }
            } else if (source == MappingSource::Evdev) {
                line = genericMapping(pad);
                source = MappingSource::Generic;
            } else
                line.clear();
        }
        if (!glfwJoystickIsGamepad(jid)) continue;
        state.source[jid] = source;
        GLFWgamepadstate rest{};
        if (glfwGetGamepadState(jid, &rest))
            for (int axis = 0; axis < 6; ++axis)
                // (only stick axes pinned at an end count: a wheel's pedal or a slider. Triggers rest at -1 anyway.)
                if (axis != GLFW_GAMEPAD_AXIS_LEFT_TRIGGER && axis != GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER &&
                    std::fabs(rest.axes[axis]) > 0.9f)
                    state.rest[jid][axis] = rest.axes[axis];
        if (source == MappingSource::Database)
            std::fprintf(stderr, "[input] %s (%s): using the controller database's mapping for another version of this pad\n",
                         pad.name.c_str(), pad.guid.c_str());
        else
            std::fprintf(stderr, "[input] %s (%s): not in the controller database, using an Xbox layout (%s)\n",
                         pad.name.c_str(), pad.guid.c_str(), mappingSourceId(source));
    }
}
// How a joystick's gamepad mapping was found (None before ensureFallbackMappings() looked at it).
inline MappingSource mappingSource(int jid) {
    if (jid < GLFW_JOYSTICK_1 || jid > GLFW_JOYSTICK_LAST) return MappingSource::None;
    return detail::fallbackState().source[jid];
}
// A fallback pad's stick axes at rest when pinned at an end (0 otherwise): the activity checks subtract them so a
// wheel pedal or slider resting at -1 does not look held.
inline float restAxis(int jid, int axis) {
    if (jid < GLFW_JOYSTICK_1 || jid > GLFW_JOYSTICK_LAST || axis < 0 || axis >= 6) return 0.0f;
    return detail::fallbackState().rest[jid][axis];
}
} // namespace cane::input
