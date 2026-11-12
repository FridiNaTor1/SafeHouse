// Controller support self-test (ctest "controllers"): the fallback mappings of platform/gamepad_mappings.h for
// fake devices, family detection by VID / PID / name (platform/controller_family.h), the button-name tokens, and,
// with --uinput on Linux (needs /dev/uinput and a display), a real virtual pad through GLFW.
//
// Usage: cane-controller-tests [--uinput]
#include "controller_family.h"
#include "vr_pad_map.h"
#include "sly2_settings.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#endif

using namespace cane::input;

static int g_failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

// A mapping line's elements as key -> value (GUID, name and platform left out), to compare regardless of order.
static std::map<std::string, std::string> elements(std::string_view line) {
    std::map<std::string, std::string> out;
    int field = 0;
    while (!line.empty()) {
        const size_t comma = line.find(',');
        const std::string_view item = line.substr(0, comma);
        line.remove_prefix(comma == std::string_view::npos ? line.size() : comma + 1);
        if (field++ < 2 || item.empty()) continue;
        const size_t colon = item.find(':');
        const std::string key(item.substr(0, colon));
        if (key == "platform") continue;
        out[key] = std::string(item.substr(colon + 1));
    }
    return out;
}
static std::string describe(const std::map<std::string, std::string>& m) {
    std::string s;
    for (const auto& [k, v] : m) s += k + ":" + v + ",";
    return s;
}
static std::string dbLine(std::string_view db, std::string_view guid) {
    size_t at = db.find(guid);
    if (at == std::string_view::npos) return {};
    return std::string(db.substr(at, db.find('\n', at) - at));
}

// SDL GUID in GLFW's Linux form.
static std::string guidOf(int bus, int vendor, int product, int version) {
    char text[40];
    std::snprintf(text, sizeof text, "%02x%02x0000%02x%02x0000%02x%02x0000%02x%02x0000", bus & 255, bus >> 8, vendor & 255,
                  vendor >> 8, product & 255, product >> 8, version & 255, version >> 8);
    return text;
}

// The HyperX Clutch in its Android / Bluetooth-style HID mode (the database's "HyperX Clutch" Linux lines):
// BTN_A B C X Y Z TL TR TL2 TR2 SELECT START MODE THUMBL THUMBR, ABS_X Y Z RZ GAS BRAKE + HAT0.
static RawJoystick androidLayoutPad(int vendor, int product, int version, const char* name) {
    RawJoystick pad;
    pad.guid = guidOf(3, vendor, product, version);
    pad.name = name;
    for (int code = 0x130; code <= 0x13e; ++code) pad.setKey(code);
    for (int code : { 0x00, 0x01, 0x02, 0x05, 0x09, 0x0a, 0x10, 0x11 }) pad.setAbs(code);
    pad.countFromEvdev();
    return pad;
}
// An xpad-driven pad (Xbox 360 layout): BTN_A B X Y TL TR SELECT START MODE THUMBL THUMBR, ABS_X Y Z RX RY RZ + HAT0.
static RawJoystick xpadPad(int vendor, int product, int version, const char* name) {
    RawJoystick pad;
    pad.guid = guidOf(3, vendor, product, version);
    pad.name = name;
    for (int code : { 0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e }) pad.setKey(code);
    for (int code : { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11 }) pad.setAbs(code);
    pad.countFromEvdev();
    return pad;
}

static void mappingTests() {
    std::string db;
#ifdef CANE_HAVE_GAMECONTROLLERDB
    db = detail::filterMappings(embedded::gameControllerDb(), "Linux");
#endif
    check(!db.empty(), "the bundled controller database has Linux lines");

    // GLFW counts.
    {
        const RawJoystick clutch = androidLayoutPad(0x03f0, 0x038d, 0x0110, "HyperX Clutch");
        check(clutch.buttons == 15 && clutch.axes == 6 && clutch.hats == 1, "evdev counts as GLFW's (15 buttons, 6 axes, 1 hat)");
    }
    // 1. Database, another version: the HyperX Clutch's USB line is version 0x0111.
    {
        const RawJoystick clutch = androidLayoutPad(0x03f0, 0x038d, 0x0110, "HyperX Clutch");
        MappingSource source{};
        const std::string line = fallbackMapping(clutch, db, &source);
        const std::string reference = dbLine(db, "03000000f00300008d03000011010000");
        check(!reference.empty(), "the database has the HyperX Clutch (03f0:038d, Linux)");
        check(source == MappingSource::Database && line.rfind(clutch.guid + ",HyperX Clutch,", 0) == 0,
              "HyperX Clutch with another firmware version: the database line, with the pad's own GUID");
        check(elements(line) == elements(reference), "  ... and its elements are the database's");
    }
    // 2. Evdev: the same Android layout under an unknown vendor reproduces the database's HyperX mapping.
    {
        const RawJoystick pad = androidLayoutPad(0x1234, 0x5678, 0x0100, "Acme Wireless Gamepad");
        MappingSource source{};
        const std::string line = fallbackMapping(pad, db, &source);
        const auto expected = elements(dbLine(db, "03000000f00300008d03000011010000"));
        check(source == MappingSource::Evdev, "unknown Android-layout pad: evdev mapping");
        check(elements(line) == expected, "  ... equal to the HyperX Clutch database line: " + describe(elements(line)));
    }
    // 2. Evdev: an unknown xpad pad (e.g. a HyperX Clutch Gladiate variant xpad binds by vendor) matches the Xbox 360
    // controller's database line.
    {
        const RawJoystick pad = xpadPad(0x03f0, 0x7fff, 0x0114, "HyperX Clutch Gladiate");
        MappingSource source{};
        const std::string line = fallbackMapping(pad, db, &source);
        const auto expected = elements(dbLine(db, "030000005e0400008e02000014010000"));
        check(!expected.empty(), "the database has the Xbox 360 controller (045e:028e v0114, Linux)");
        check(source == MappingSource::Evdev && elements(line) == expected,
              "unknown xpad pad: evdev mapping equal to the Xbox 360 controller's: " + describe(elements(line)));
    }
    // 2. Evdev, Sony / Nintendo vendor: BTN_NORTH (0x133) is the north button.
    {
        RawJoystick pad = xpadPad(0x054c, 0x7ffe, 0x0100, "Sony Test Pad");
        const auto m = elements(evdevMapping(pad));
        check(m.count("y") && m.at("y") == "b2" && m.at("x") == "b3", "Sony / Nintendo evdev pads are positional (y = BTN_NORTH)");
    }
    // 3. Generic, no evdev (Windows DirectInput, macOS): Xbox layout by index, limited to what exists.
    {
        RawJoystick pad;
        pad.guid = "03000000aaaa0000bbbb000000000000";
        pad.name = "Generic   USB  Joystick";
        pad.axes = 6;
        pad.buttons = 11;
        pad.hats = 1;
        MappingSource source{};
        const auto m = elements(fallbackMapping(pad, db, &source));
        const std::map<std::string, std::string> expected = {
            {"a", "b0"}, {"b", "b1"}, {"x", "b2"}, {"y", "b3"}, {"leftshoulder", "b4"}, {"rightshoulder", "b5"},
            {"back", "b6"}, {"start", "b7"}, {"leftstick", "b8"}, {"rightstick", "b9"}, {"guide", "b10"},
            {"leftx", "a0"}, {"lefty", "a1"}, {"lefttrigger", "a2"}, {"rightx", "a3"}, {"righty", "a4"},
            {"righttrigger", "a5"}, {"dpup", "h0.1"}, {"dpright", "h0.2"}, {"dpdown", "h0.4"}, {"dpleft", "h0.8"}};
        check(source == MappingSource::Generic && m == expected, "unknown pad without evdev: generic Xbox layout: " + describe(m));
        pad.axes = 4;
        pad.buttons = 12;
        const auto d = elements(genericMapping(pad));
        check(d.at("rightx") == "a2" && d.at("lefttrigger") == "b6" && d.at("start") == "b9" && !d.count("guide"),
              "4 axes + 12 buttons: the DirectInput pad (triggers b6 / b7, Start b9)");
        pad.axes = 2;
        pad.buttons = 8;
        pad.hats = 0;
        check(mappingFits(genericMapping(pad), 2, 8, 0) && !elements(genericMapping(pad)).count("rightx"),
              "a small pad only gets the elements it has");
    }
    // Not pads.
    {
        RawJoystick mic; // the user's HyperX QuadCast 2 "Controller" (event18): EV_ABS ABS_MISC only
        mic.guid = guidOf(3, 0x03f0, 0x09af, 0x0100);
        mic.name = "HP, Inc HyperX QuadCast 2 Controller";
#if defined(__linux__) && !defined(__ANDROID__)
        mic.keyBits = detail::parseCapabilityBits("0");
        mic.absBits = detail::parseCapabilityBits("10000000000");
        mic.evdev = true;
        check(mic.abs(0x28) && !mic.abs(0), "sysfs bitmaps parse (ABS_MISC = bit 40)");
#endif
        mic.countFromEvdev();
        check(fallbackMapping(mic, db).empty(), "a microphone's volume dial is not a pad");
        RawJoystick touchpad;
        touchpad.guid = guidOf(3, 0x054c, 0x0ce6, 0x8111);
        touchpad.name = "Sony Interactive Entertainment DualSense Wireless Controller Touchpad";
        for (int code : { 0x110, 0x145, 0x14a, 0x14d }) touchpad.setKey(code);
        for (int code : { 0x00, 0x01, 0x2f, 0x35, 0x36, 0x39 }) touchpad.setAbs(code);
        touchpad.countFromEvdev();
        check(fallbackMapping(touchpad, db).empty(), "a DualSense touchpad is not a pad");
        RawJoystick sensors;
        sensors.guid = guidOf(3, 0x054c, 0x0ce6, 0x8111);
        sensors.name = "Sony Interactive Entertainment DualSense Wireless Controller Motion Sensors";
        for (int code : { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 }) sensors.setAbs(code);
        sensors.countFromEvdev();
        check(fallbackMapping(sensors, db).empty(), "motion sensors are not a pad");
        RawJoystick wheel = xpadPad(0x046d, 0xc24f, 0x0111, "Logitech G29 Driving Force Racing Wheel");
        check(!fallbackMapping(wheel, db).empty(), "a wheel with pad buttons still gets a mapping (its pedals' rest is subtracted)");
    }
    // An output modifier line (GLFW cannot parse it) never counts as fitting.
    check(!mappingFits("030000007e0500001920000011810000,NSO N64 Controller,+rightx:b2,a:b0,", 6, 20, 1),
          "lines with output modifiers are skipped");
#if defined(__linux__) && !defined(__ANDROID__)
    // sysfs lookup on a fake tree.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / ("cane-controller-tests-" + std::to_string(::getpid()));
        fs::remove_all(root);
        const fs::path dev = root / "event7" / "device";
        fs::create_directories(dev / "id");
        fs::create_directories(dev / "capabilities");
        auto put = [](const fs::path& p, const char* text) { std::ofstream(p) << text << "\n"; };
        put(dev / "name", "HyperX Clutch");
        put(dev / "id" / "bustype", "0003");
        put(dev / "id" / "vendor", "03f0");
        put(dev / "id" / "product", "038d");
        put(dev / "id" / "version", "0110");
        put(dev / "capabilities" / "key", "7fff000000000000 0 0 0 0");
        put(dev / "capabilities" / "abs", "30627");
        RawJoystick pad;
        pad.guid = guidOf(3, 0x03f0, 0x038d, 0x0110);
        pad.name = "HyperX Clutch";
        const bool found = readEvdevCapabilities(pad, root);
        fs::remove_all(root);
        pad.countFromEvdev();
        const RawJoystick expected = androidLayoutPad(0x03f0, 0x038d, 0x0110, "HyperX Clutch");
        check(found && pad.buttons == 15 && pad.axes == 6 && pad.hats == 1 && pad.key(0x130) && pad.key(0x13e) &&
                  pad.abs(0x09) && pad.abs(0x0a) && !pad.abs(0x03),
              "sysfs capabilities of a HyperX Clutch read back");
        check(evdevMapping(pad) == evdevMapping(expected), "  ... and map like the hand-built device");
    }
#endif
}

static void familyTests() {
    struct Case { uint16_t vid, pid; const char* name; Family family; };
    const Case cases[] = {
        {0x03f0, 0x038d, "HyperX Clutch", Family::Xbox},
        {0x03f0, 0x048d, "HyperX Clutch", Family::Xbox},
        {0x03f0, 0x0495, "HyperX Clutch Gladiate", Family::Xbox},
        {0x1234, 0x5678, "Acme Wireless Gamepad", Family::Xbox},
        {0, 0, "Unknown", Family::Xbox},
        {0x045e, 0x0b12, "Xbox Wireless Controller", Family::Xbox},
        {0x054c, 0x0ce6, "DualSense Wireless Controller", Family::PS5},
        {0x054c, 0x09cc, "Wireless Controller", Family::PS4},
        {0x054c, 0x0268, "Sony PLAYSTATION(R)3 Controller", Family::PlayStation},
        {0x057e, 0x2009, "Nintendo Switch Pro Controller", Family::Nintendo},
        {0x057e, 0x2006, "Nintendo Switch Left Joy-Con", Family::Nintendo},
        {0x057e, 0x2008, "Nintendo Switch Combined Joy-Cons", Family::Nintendo},
        {0x057e, 0x2069, "Nintendo Switch 2 Pro Controller", Family::Switch2},
        {0x057e, 0x2067, "Joy-Con 2 (L)", Family::Switch2},
        {0x057e, 0x2073, "NSO GameCube Controller", Family::GameCube},
        {0x057e, 0x0337, "Nintendo Co., Ltd. WUP-028", Family::GameCube},
        {0x0079, 0x1844, "MAYFLASH GameCube Controller Adapter", Family::GameCube},
        {0x057e, 0x0330, "Nintendo Wii Remote Pro Controller", Family::WiiU},
        {0x057e, 0x0306, "Nintendo Wii Remote Classic Controller", Family::Wii},
        {0x0001, 0x0001, "Nintendo Wii Remote", Family::Wii},
        {0x0079, 0x1800, "Mayflash Wii U Pro Game Controller Adapter", Family::WiiU},
        {0x28de, 0x1205, "Steam Deck", Family::SteamDeck},
        {0x28de, 0x1302, "Steam Controller", Family::SteamController2026},
        {0x2dc8, 0x3106, "8BitDo Pro 2", Family::Xbox},
        {0x0000, 0x0000, "AYANEO Controller", Family::Xbox},
        {0x0000, 0x0000, "AYN Thor Gamepad", Family::Xbox},
        {0x28de, 0x2300, "Valve Software Knuckles", Family::ValveIndex},
        {0x2833, 0x0186, "Oculus Touch Controller", Family::MetaQuest},
        {0x0000, 0x0000, "Meta Quest Touch Plus (Right)", Family::MetaQuest},
        {0x0bb4, 0x0306, "HTC Vive Controller", Family::ViveWand},
        {0x0000, 0x0000, "PICO 4 Controller", Family::Pico},
        {0x0000, 0x0000, "Steam Frame Controller", Family::SteamFrame},
    };
    for (const Case& c : cases) {
        const Family got = classify(c.vid, c.pid, c.name, "", false);
        char what[160];
        std::snprintf(what, sizeof what, "%04x:%04x \"%s\" -> %s", c.vid, c.pid, c.name, familyId(c.family));
        check(got == c.family, std::string(what) + (got == c.family ? "" : std::string(" (got ") + familyId(got) + ")"));
    }
    check(familyFromSteamType("gamecube") == Family::GameCube && familyFromSteamType("switch2pro") == Family::Switch2 &&
              familyFromSteamType("switchpro") == Family::Nintendo && familyFromSteamType("wiiu") == Family::WiiU,
          "Steam controller types");
    for (int style = 1; style <= kPromptStyleLast; ++style) {
        const Family family = familyForStyle(static_cast<PromptStyle>(style));
        check(static_cast<int>(family) == style && parseFamily(familyId(family)) == family,
              std::string("prompt style ") + std::to_string(style) + " = " + promptStyleName(style));
    }
    // Sly 2's Settings.ini writes each style by its promptStyleName() and reads it (and the older spellings) back.
    for (const auto& key : cane::settings::sly2Schema()) {
        if (std::string(key.key) != "Button Icons") continue;
        for (int style = 0; style <= kPromptStyleLast; ++style) {
            cane::settings::Sly2Settings settings{};
            settings.system.promptStyle = style;
            const std::string written = key.write(settings);
            cane::settings::Sly2Settings back{};
            check(written == promptStyleName(style) && key.read(back, written) && back.system.promptStyle == style,
                  "Settings.ini Button Icons = " + written);
        }
        cane::settings::Sly2Settings old{};
        check(key.read(old, "Steam Controller 2026") && old.system.promptStyle == 8 && key.read(old, "Nintendo") &&
                  old.system.promptStyle == 3,
              "Settings.ini reads the earlier style names");
    }
    check(isNintendoLayout(Family::Wii) && isNintendoLayout(Family::Switch2) && !isNintendoLayout(Family::GameCube),
          "Nintendo face layout families (GameCube's A is south)");
    check(std::string(buttonLabel(Button::South, Family::GameCube)) == "A" &&
              std::string(buttonLabel(Button::West, Family::GameCube)) == "B" &&
              std::string(buttonLabel(Button::RB, Family::GameCube)) == "Z" &&
              std::string(buttonLabel(Button::South, Family::WiiU)) == "B",
          "GameCube / Wii U labels are positional");
}

// platform/vr_pad_map.h: the Steam Frame's controllers (XR_VALVE_frame_controller_interaction) as a standard pad, the
// Steam Controller (2026)'s layout: the profile names, the Gamepad layout, its prompts
static void frameTests() {
    using namespace cane::vrpad;
    check(std::string(kFrameProfile) == "/interaction_profiles/valve/frame_controller_valve" &&
              std::string(kFrameExtension) == "XR_VALVE_frame_controller_interaction",
          "Steam Frame: Valve's interaction profile and extension names");
    check(controllersOf(kFrameProfile) == Controllers::Frame && controllersOf(kFrameProfileOld) == Controllers::Frame,
          "Steam Frame: both profile names (SteamVR 2.15.1+ and before) are the Frame's controllers");
    check(controllersOf("/interaction_profiles/valve/index_controller") == Controllers::Index &&
              controllersOf("/interaction_profiles/oculus/touch_controller") == Controllers::Touch &&
              controllersOf("/interaction_profiles/microsoft/motion_controller") == Controllers::Wmr &&
              controllersOf("/interaction_profiles/htc/vive_controller") == Controllers::Vive &&
              controllersOf("/interaction_profiles/khr/simple_controller") == Controllers::Simple &&
              controllersOf("/interaction_profiles/bytedance/pico4_controller") == Controllers::Pico &&
              controllersOf("") == Controllers::None,
          "the other profiles keep their controllers");
    check(layoutOf(Controllers::Frame) == Layout::Gamepad && realStartSelectOf(Controllers::Frame) &&
              layoutOf(Controllers::Index) == Layout::Touch && !realStartSelectOf(Controllers::Index) &&
              layoutOf(Controllers::Wmr) == Layout::Pads && realStartSelectOf(Controllers::Wmr) &&
              layoutOf(Controllers::Touch) == Layout::Touch && !realStartSelectOf(Controllers::Touch),
          "layouts: Frame Gamepad (real Start / Select), Index / Touch Touch (combos), WMR Pads");
    State s;
    Raw r;
    Pad p;
    const auto press = [&](Raw raw) { s = State{}; return map(raw, Layout::Gamepad, true, 0.0, s); };
    r = Raw{}; r.a = true; p = press(r);
    check(p.buttons[kA] && !p.buttons[kB] && !p.buttons[kX] && !p.buttons[kY], "Frame A = Cross");
    r = Raw{}; r.b = true; p = press(r);
    check(p.buttons[kB] && !p.buttons[kA] && !p.buttons[kX], "Frame B = Circle (east)");
    r = Raw{}; r.x = true; p = press(r);
    check(p.buttons[kX] && p.axes[kAxisLT] == -1.0f, "Frame X = Square (west), not L2");
    r = Raw{}; r.y = true; p = press(r);
    check(p.buttons[kY] && p.axes[kAxisRT] == -1.0f, "Frame Y = Triangle (north), not R2");
    r = Raw{}; r.bumperL = true; r.bumperR = true; p = press(r);
    check(p.buttons[kLB] && p.buttons[kRB], "Frame bumpers = L1 / R1");
    r = Raw{}; r.trigL = 0.3f; p = press(r);
    check(!p.buttons[kLB] && std::fabs(p.axes[kAxisLT] - (-0.4f)) < 1e-5f && p.axes[kAxisRT] == -1.0f,
          "Frame triggers = L2 / R2, analog (not L1 / R1)");
    r = Raw{}; r.trigR = 1.0f; p = press(r);
    check(p.axes[kAxisRT] == 1.0f && !p.buttons[kRB], "Frame right trigger fully = R2");
    r = Raw{}; r.clickL = true; r.clickR = true; p = press(r);
    check(p.buttons[kLThumb] && p.buttons[kRThumb] && !p.buttons[kStart] && !p.buttons[kBack], "Frame stick clicks = L3 / R3");
    r = Raw{}; r.start = true; p = press(r);
    check(p.buttons[kStart] && !p.buttons[kBack], "Frame Menu = Start");
    r = Raw{}; r.select = true; p = press(r);
    check(p.buttons[kBack] && !p.buttons[kStart], "Frame View = Select");
    r = Raw{}; r.dpadUp = true; r.dpadLeft = true; p = press(r);
    check(p.buttons[kDpadUp] && p.buttons[kDpadLeft] && !p.buttons[kDpadDown] && !p.buttons[kDpadRight], "Frame d-pad = d-pad");
    r = Raw{}; r.gripL = 1.0f; r.gripR = 1.0f; p = press(r);
    bool none = true;
    for (bool b : p.buttons) none = none && !b;
    check(none && p.axes[kAxisLT] == -1.0f && p.axes[kAxisRT] == -1.0f, "Frame grips are free (no game button: they hold the controllers)");
    r = Raw{}; r.gripR = 1.0f; r.clickR = true; p = press(r);
    check(p.buttons[kRThumb] && !p.buttons[kStart], "Frame: no grip + stick-click combos (real Start / Select)");
    r = Raw{}; r.lx = 0.5f; r.ly = 1.0f; r.rx = -1.0f; r.ry = -0.25f; p = press(r);
    check(p.axes[kAxisLX] == 0.5f && p.axes[kAxisLY] == -1.0f && p.axes[kAxisRX] == -1.0f && p.axes[kAxisRY] == 0.25f,
          "Frame sticks (OpenXR y up -> GLFW y down)");
    // the prompts: the Steam Frame family reads like the 2026 Steam Controller (a standard pad by position)
    struct L { Button b; const char* label; };
    bool labels = true;
    for (const L& l : {L{Button::South, "A"}, L{Button::East, "B"}, L{Button::West, "X"}, L{Button::North, "Y"},
                       L{Button::LB, "L1"}, L{Button::RB, "R1"}, L{Button::LT, "L2"}, L{Button::RT, "R2"},
                       L{Button::Back, "View"}, L{Button::Start, "Menu"}, L{Button::LSPress, "L3"}, L{Button::RSPress, "R3"}})
        labels = labels && std::string(buttonLabel(l.b, Family::SteamFrame)) == l.label &&
                 std::string(buttonLabel(l.b, Family::SteamFrame)) == buttonLabel(l.b, Family::SteamController2026);
    check(labels, "Steam Frame prompts = the Steam Controller (2026)'s (A B X Y, L1 R1 L2 R2, View, Menu, L3 R3)");
}

// platform/vr_pad_map.h: the Touch layout and the grip + stick-click combos
static void vrPadTests() {
    using namespace cane::vrpad;
    State s;
    Raw r;
    r.a = true; Pad p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kA], "Touch A = Cross");
    r = Raw{}; r.b = true; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kX] && !p.buttons[kB], "Touch B = Square");
    r = Raw{}; r.x = true; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.axes[kAxisLT] == 1.0f && !p.buttons[kB] && !p.buttons[kX], "Touch X = L2");
    r = Raw{}; r.y = true; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.axes[kAxisRT] == 1.0f && !p.buttons[kY], "Touch Y = R2");
    s = State{}; r = Raw{}; r.gripL = 0.9f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kB] && p.axes[kAxisLT] == -1.0f, "the left grip = Circle at once");
    s = State{}; r = Raw{}; r.gripR = 0.9f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kY], "the right grip = Triangle at once");
    r.gripR = 0.4f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kY], "Triangle held down to the release threshold");
    r.gripR = 0.1f; p = map(r, Layout::Touch, false, 0.0, s);
    check(!p.buttons[kY], "Triangle released");
    r = Raw{}; r.x = true; p = map(r, Layout::Pads, true, 0.0, s);
    check(p.buttons[kX] && p.axes[kAxisLT] == -1.0f, "Pads: the left trackpad click = Square");
    s = State{}; r = Raw{}; r.trigR = 0.8f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kRB], "the right trigger = R1");
    r.trigR = 0.5f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kRB], "R1 held down to the release threshold");
    r.trigR = 0.1f; p = map(r, Layout::Touch, false, 0.0, s);
    check(!p.buttons[kRB], "R1 released");
    // Start: the right grip (Triangle at once), then the right stick click: Start, Triangle and R3 away
    s = State{}; r = Raw{}; r.gripR = 1.0f;
    p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kY] && !p.buttons[kStart], "the grip first: Triangle");
    r.clickR = true; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kStart] && !p.buttons[kY] && !p.buttons[kRThumb], "R grip + R3 = Start, no Triangle / R3");
    r.clickR = false; p = map(r, Layout::Touch, false, 0.0, s);
    check(!p.buttons[kStart] && !p.buttons[kY], "the click up: Start up, the grip still away from the game");
    r.gripR = 0.0f; map(r, Layout::Touch, false, 0.0, s);
    r.gripR = 1.0f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kY], "a new squeeze after the combo is Triangle again");
    // Select: left grip + left stick click
    s = State{}; r = Raw{}; r.gripL = 1.0f; map(r, Layout::Touch, false, 0.0, s);
    r.clickL = true; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kBack] && !p.buttons[kLThumb] && !p.buttons[kB], "L grip + L3 = Select");
    // the click first, then the grip: L3 and Circle, no combo
    s = State{}; r = Raw{}; r.clickL = true; map(r, Layout::Touch, false, 0.0, s);
    r.gripL = 1.0f; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kLThumb] && !p.buttons[kBack] && p.buttons[kB], "L3 then L grip: L3 + Circle");
    s = State{}; r = Raw{}; r.menu = true; p = map(r, Layout::Touch, false, 0.0, s);
    check(p.buttons[kStart], "the menu button = Start");
    s = State{}; r = Raw{}; r.gripL = 1.0f; r.clickL = true; map(r, Layout::Pads, true, 0.0, s);
    p = map(r, Layout::Pads, true, 0.0, s);
    check(p.buttons[kB] && p.buttons[kLThumb] && !p.buttons[kBack], "real Start / Select: no combos");
    frameTests();
}

static void vrTests() {
    ActivityTracker t;
    t.noteVr(Family::ValveIndex);
    check(t.family() == Family::ValveIndex && t.lastWasGamepad() && t.lastWasVr(), "noteVr: prompts follow the VR controller");
    t.noteKeyboard();
    check(t.family() == Family::Keyboard && !t.lastWasVr(), "a key press after VR: keyboard prompts");
    check(isVrFamily(Family::MetaQuest) && isVrFamily(Family::Pico) && !isVrFamily(Family::SteamController2026),
          "isVrFamily");
    check(std::string(buttonLabel(Button::LB, Family::MetaQuest)) == "L Trigger" &&
              std::string(buttonLabel(Button::RT, Family::MetaQuest)) == "Y" &&
              std::string(buttonLabel(Button::West, Family::MetaQuest)) == "B" &&
              std::string(buttonLabel(Button::East, Family::MetaQuest)) == "L Grip" &&
              std::string(buttonLabel(Button::North, Family::MetaQuest)) == "R Grip" &&
              std::string(buttonLabel(Button::Guide, Family::MetaQuest)) == "Meta" &&
              std::string(buttonLabel(Button::South, Family::ViveWand)) == "R Pad",
          "VR labels (triggers L1 / R1, grips Circle / Triangle, X / Y L2 / R2, B Square, Meta as guide, Vive pads as face buttons)");
    vrPadTests();
}

static void tokenTests() {
    check(buttonFromName("L3") == Button::LSPress && buttonFromName("r3") == Button::RSPress &&
              buttonFromName("Cross") == Button::South && buttonFromName("Menu") == Button::Start &&
              !buttonFromName("L4"),
          "button names in any family's words");
    const auto t = findButtonToken("Press {L3} or {nope} then {Start}", 0);
    check(t.begin == 6 && t.end == 10 && t.button == Button::LSPress, "first {L3} token");
    check(replaceButtonTokens("Press {L3} or {nope} then {Start}", Family::Nintendo) ==
              "Press L-stick or {nope} then +",
          "tokens replaced by the family's names, other braces kept");
    check(replaceButtonTokens("{R3}", Family::PlayStation) == "R3" && replaceButtonTokens("{R3}", Family::Xbox) == "RS",
          "R3 in PlayStation and Xbox words");
}

#if defined(__linux__) && !defined(__ANDROID__)
// A real virtual pad through uinput with the HyperX Clutch's Android layout: GLFW must list it as a gamepad after
// ensureFallbackMappings() (by the expected route), and its south button must read as A.
static int uinputTest(uint16_t vendor, uint16_t product, uint16_t version, const char* name, MappingSource expected) {
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::printf("skip uinput: /dev/uinput not writable\n");
        return 77;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int code = 0x130; code <= 0x13e; ++code) ioctl(fd, UI_SET_KEYBIT, code);
    for (int code : { ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_GAS, ABS_BRAKE, ABS_HAT0X, ABS_HAT0Y }) {
        ioctl(fd, UI_SET_ABSBIT, code);
        uinput_abs_setup abs{};
        abs.code = static_cast<__u16>(code);
        const bool hat = code == ABS_HAT0X || code == ABS_HAT0Y, trigger = code == ABS_GAS || code == ABS_BRAKE;
        abs.absinfo.minimum = hat ? -1 : trigger ? 0 : -32768;
        abs.absinfo.maximum = hat ? 1 : trigger ? 255 : 32767;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = vendor;
    setup.id.product = product;
    setup.id.version = version;
    std::snprintf(setup.name, sizeof setup.name, "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        std::printf("skip uinput: cannot create the device\n");
        ::close(fd);
        return 77;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700)); // (udev sets the node's permissions)
    int result = 0;
    if (!glfwInit()) {
        std::printf("skip uinput: glfwInit failed (no display)\n");
        result = 77;
    } else {
        applyBundledMappings();
        installJoystickCallback();
        int jid = -1;
        for (int i = 0; i < 40 && jid < 0; ++i) {
            glfwPollEvents();
            for (int j = GLFW_JOYSTICK_1; j <= GLFW_JOYSTICK_LAST; ++j)
                if (glfwJoystickPresent(j) && std::strcmp(glfwGetJoystickName(j), name) == 0) jid = j;
            if (jid < 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (jid < 0) {
            std::printf("skip uinput: GLFW does not see the device (no read access to its event node)\n");
            result = 77;
        } else {
            std::printf("uinput pad \"%s\" %04x:%04x v%04x\n", name, vendor, product, version);
            check(!glfwJoystickIsGamepad(jid), "uinput pad: not a gamepad for GLFW before the fallback");
            GLFWgamepadstate state{};
            selectActivePad(-1, &state);   // (runs ensureFallbackMappings)
            check(glfwJoystickIsGamepad(jid) && mappingSource(jid) == expected,
                  std::string("uinput pad: a ") + mappingSourceId(expected) + " fallback mapping (got " +
                      mappingSourceId(mappingSource(jid)) + ")");
            check(identify(jid).family == Family::Xbox, "uinput pad: Xbox prompts");
            input_event events[2]{};
            events[0].type = EV_KEY;
            events[0].code = BTN_A;
            events[0].value = 1;
            events[1].type = EV_SYN;
            events[1].code = SYN_REPORT;
            if (::write(fd, events, sizeof events) != static_cast<ssize_t>(sizeof events)) std::printf("uinput write failed\n");
            bool pressed = false;
            for (int i = 0; i < 40 && !pressed; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                glfwPollEvents();
                pressed = glfwGetGamepadState(jid, &state) && state.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS &&
                          state.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_RELEASE;
            }
            check(pressed, "uinput pad: BTN_A reads as gamepad A");
            check(selectActivePad(-1, &state) == jid, "uinput pad: selected as the active pad while A is held");
        }
        glfwTerminate();
    }
    ioctl(fd, UI_DEV_DESTROY);
    ::close(fd);
    return result;
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
// --uinput-pad DELAY_MS PRESS_MS TOTAL_MS [CODE [COUNT]]: a virtual unknown pad (the Clutch's Android layout under vendor 7e57)
// for TOTAL_MS that presses evdev key CODE (default BTN_X, 0x133) every 1.5 s from DELAY_MS on, PRESS_MS each: another process
// (the game's Remap page) can capture it.
static int uinputPad(int delayMs, int pressMs, int totalMs, int code, int count) {
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return 77;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int c = 0x130; c <= 0x13e; ++c) ioctl(fd, UI_SET_KEYBIT, c);
    for (int c : { ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_GAS, ABS_BRAKE, ABS_HAT0X, ABS_HAT0Y }) {
        ioctl(fd, UI_SET_ABSBIT, c);
        uinput_abs_setup abs{};
        abs.code = static_cast<__u16>(c);
        const bool hat = c == ABS_HAT0X || c == ABS_HAT0Y, trigger = c == ABS_GAS || c == ABS_BRAKE;
        abs.absinfo.minimum = hat ? -1 : trigger ? 0 : -32768;
        abs.absinfo.maximum = hat ? 1 : trigger ? 255 : 32767;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x7e57;
    setup.id.product = 0x0c1a;
    setup.id.version = 0x0101;
    std::snprintf(setup.name, sizeof setup.name, "SafeHouse Test Pad");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { ::close(fd); return 77; }
    auto key = [&](int value) {
        input_event events[2]{};
        events[0].type = EV_KEY;
        events[0].code = static_cast<__u16>(code);
        events[0].value = value;
        events[1].type = EV_SYN;
        if (::write(fd, events, sizeof events) < 0) std::printf("uinput write failed\n");
    };
    // (pressed every 1.5 s from DELAY_MS on, so a capture that starts at an unknown time sees a fresh press)
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    while (elapsed() + pressMs < totalMs && count-- != 0) {
        key(1);
        std::printf("uinput pad: key 0x%x down at %d ms\n", code, elapsed());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(pressMs));
        key(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500 - std::min(pressMs, 1400)));
    }
    while (elapsed() < totalMs) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ioctl(fd, UI_DEV_DESTROY);
    ::close(fd);
    return 0;
}
#endif

int main(int argc, char** argv) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (argc >= 5 && !std::strcmp(argv[1], "--uinput-pad"))
        return uinputPad(std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]),
                         argc >= 6 ? int(std::strtol(argv[5], nullptr, 0)) : 0x133, argc >= 7 ? std::atoi(argv[6]) : -1);
#endif
    bool uinput = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--uinput")) uinput = true;
    if (uinput) {
#if defined(__linux__) && !defined(__ANDROID__)
        // an unknown vendor: built from the evdev codes
        int r = uinputTest(0x7e57, 0x0c1a, 0x0101, "SafeHouse Test Pad", MappingSource::Evdev);
        // the HyperX Clutch (USB, Android layout) with another firmware version than the database line's 0x0111
        if (r != 77) r = uinputTest(0x03f0, 0x038d, 0x0110, "HyperX Clutch", MappingSource::Database);
        if (r == 77) return 77;
#else
        return 77;
#endif
    } else {
        mappingTests();
        familyTests();
        vrTests();
        tokenTests();
    }
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
