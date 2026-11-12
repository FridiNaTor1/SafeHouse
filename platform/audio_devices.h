#pragma once
// Audio output / input device choice (Sly 2's Settings.ini [Audio] Output Device / Input Device / Headset Output Device;
// notes/AUDIO_2026-11-10.md). A device is stored by its name, as SDL2 reports it (PipeWire / PulseAudio / ALSA on
// Linux, WASAPI on Windows, CoreAudio on macOS): the name stays the same across reboots, where an index does not.
// An empty name is "System default" (the default). A chosen device that is missing (unplugged, renamed) is played
// on / recorded from the system default instead, and taken again when it is back (DeviceFollower).
//
// This header: the pure parts (the name matching, the follower, the INI spelling) shared by the game and the launcher,
// and the launcher's device list (audio_devices_sdl.h: SDL2 loaded at run time, as launcher/collection/audio.h does;
// the launcher never links SDL2). The game lists and opens devices with its own SDL2 (Sly2-Release/audio.cpp).
//
// Test hook: SAFEHOUSE_FAKE_AUDIO_DEVICES="Out A|Out B;Mic A|Mic B" replaces the listed devices (the outputs before the
// ";", the inputs after it) in the launcher's list and the game's Audio Devices page; nothing is opened by it.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace cane::audio {

// The INI's spelling of the empty name.
inline constexpr const char* kSystemDefaultName = "System Default";

// A name as stored: control characters dropped, outer blanks trimmed.
inline std::string cleanName(std::string_view s) {
    std::string out;
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    return out.substr(b, out.find_last_not_of(' ') - b + 1);
}

// The INI value read back: "System Default" (any case / spacing) or nothing is the default (""), else the name.
inline std::string nameFromIni(std::string_view v) {
    std::string s = cleanName(v);
    std::string form;
    for (char c : s)
        if (c != ' ' && c != '_' && c != '-') form += char(std::tolower(static_cast<unsigned char>(c)));
    if (form.empty() || form == "systemdefault") return {};
    return s;
}
// The INI value written: "System Default", or the name in quotes (a name may hold " ;" or " #", which would start a
// comment unquoted; settings_ini.h keeps a quoted value whole).
inline std::string nameToIni(const std::string& name) {
    const std::string s = cleanName(name);
    if (s.empty()) return kSystemDefaultName;
    return "\"" + s + "\"";
}

// The index of `wanted` in `present`: the exact name first, then the same name ignoring case and outer blanks;
// -1 when wanted is empty (the default) or not there.
inline int findDevice(const std::vector<std::string>& present, const std::string& wanted) {
    const std::string w = cleanName(wanted);
    if (w.empty()) return -1;
    for (size_t i = 0; i < present.size(); ++i)
        if (present[i] == w) return int(i);
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string lw = lower(w);
    for (size_t i = 0; i < present.size(); ++i)
        if (lower(cleanName(present[i])) == lw) return int(i);
    return -1;
}

// Which device to have open: the chosen one while it is present, else the system default (""). step() returns true
// when the open device must change (then `open` holds the name to open, "" the default): the chosen device was
// unplugged (fall back), came back (recover), or the choice changed.
struct DeviceFollower {
    std::string wanted;   // the setting ("" System default)
    std::string open;     // the device open now ("" the default)
    bool started = false; // (false: nothing opened yet; the first step opens)
    bool step(const std::vector<std::string>& present) {
        const int i = findDevice(present, wanted);
        const std::string target = i >= 0 ? present[size_t(i)] : std::string();
        if (started && target == open) return false;
        started = true;
        open = target;
        return true;
    }
    // The chosen device is missing (playing on the default instead).
    bool fallenBack() const { return !cleanName(wanted).empty() && open.empty(); }
};

struct DeviceList {
    bool available = false;           // (false: no SDL2 / no audio system: the rows are not shown)
    std::vector<std::string> outputs; // playback devices, SDL's order
    std::vector<std::string> inputs;  // recording devices
};

// SAFEHOUSE_FAKE_AUDIO_DEVICES (tests, screenshots): true and the list when set.
inline bool fakeDevices(DeviceList& out) {
    const char* p = std::getenv("SAFEHOUSE_FAKE_AUDIO_DEVICES");
    if (!p) return false;
    out = {};
    out.available = true;
    std::string_view s(p);
    const size_t semi = s.find(';');
    auto split = [](std::string_view part, std::vector<std::string>& v) {
        size_t b = 0;
        while (b <= part.size()) {
            size_t e = part.find('|', b);
            if (e == std::string_view::npos) e = part.size();
            const std::string n = cleanName(part.substr(b, e - b));
            if (!n.empty()) v.push_back(n);
            b = e + 1;
        }
    };
    split(s.substr(0, semi), out.outputs);
    if (semi != std::string_view::npos) split(s.substr(semi + 1), out.inputs);
    return true;
}

// The choices of a device row: "" (System default) first, then the present devices, then the chosen one when it is
// missing (so the row can show it as "not connected" instead of losing it).
inline std::vector<std::string> deviceChoices(const std::vector<std::string>& present, const std::string& wanted) {
    std::vector<std::string> v{ std::string() };
    v.insert(v.end(), present.begin(), present.end());
    if (!cleanName(wanted).empty() && findDevice(present, wanted) < 0) v.push_back(cleanName(wanted));
    return v;
}

} // namespace cane::audio

