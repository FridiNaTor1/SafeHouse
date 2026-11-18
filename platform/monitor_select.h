#pragma once
// The monitor the games (and the launcher) play on: Settings' "Monitor" option (Sly 1's system_settings.dat
// SystemSettings::monitor, Sly 2 / Sly 3 Settings.ini [Display] Monitor; the launcher's Display settings edit the same
// field). Fullscreen and Borderless fill that monitor, Windowed opens on it where the window system lets a program
// place its window (not on Wayland: there the compositor places windows, and Borderless is a fullscreen surface on
// the chosen output instead, which on Wayland never changes the output's mode anyway).
//
// The stored identifier: "" for Primary (the default: the system's primary monitor), else
// "<name> | <n> | <x>,<y>": the monitor's name as GLFW reports it, which of the monitors with that name it is (1 the
// first; two monitors of the same model share a name on Windows and X11) and its desktop position. Reading it back the
// name must match (a monitor that is no longer connected falls back to the primary one); among monitors with the same
// name the one at the same position wins, else the n-th, else the first. A hand-written plain number "2" picks the
// second monitor of the list.
//
// This header needs no GLFW (unit tests: platform/monitor_select_test.cpp); monitor_select_glfw.h lists the connected
// monitors and places windows.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cane::monitor {

struct Info {
    std::string name;
    int x = 0, y = 0;               // the desktop position (0, 0 on Wayland for every output)
    int width = 0, height = 0;      // the current mode
    int refreshRate = 0;
    int maxRefreshRate = 0;         // the highest of its modes (the frame-rate lists)
    bool primary = false;
};

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// Which of the monitors named like list[index] it is (1 the first).
inline int occurrence(const std::vector<Info>& list, int index) {
    int n = 0;
    for (int i = 0; i <= index && i < int(list.size()); ++i)
        if (list[size_t(i)].name == list[size_t(index)].name) ++n;
    return n;
}
inline int sameNameCount(const std::vector<Info>& list, const std::string& name) {
    int n = 0;
    for (const Info& m : list) n += m.name == name;
    return n;
}

// The identifier of list[index] ("" for an index out of range: Primary).
inline std::string idOf(const std::vector<Info>& list, int index) {
    if (index < 0 || index >= int(list.size())) return {};
    const Info& m = list[size_t(index)];
    char tail[64];
    std::snprintf(tail, sizeof(tail), " | %d | %d,%d", occurrence(list, index), m.x, m.y);
    return (m.name.empty() ? std::string("Monitor") : m.name) + tail;
}

struct Parsed {
    std::string name;
    int occurrence = 1;
    bool hasPos = false;
    int x = 0, y = 0;
    int plainIndex = -1;   // a hand-written "2": 0-based
};
// false for "" / "Primary" (no particular monitor).
inline bool parse(const std::string& id, Parsed& out) {
    out = Parsed{};
    std::string s = trim(id);
    if (s.empty()) return false;
    {
        std::string lower;
        for (char c : s) lower += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (lower == "primary" || lower == "auto" || lower == "automatic" || lower == "default") return false;
    }
    {
        char* end = nullptr;
        const long n = std::strtol(s.c_str(), &end, 10);
        if (end && *end == '\0' && n >= 1 && n < 64) return out.plainIndex = int(n - 1), true;
    }
    // (from the right: a name may itself hold a '|')
    const size_t bar2 = s.rfind('|');
    if (bar2 != std::string::npos) {
        const size_t bar1 = bar2 > 0 ? s.rfind('|', bar2 - 1) : std::string::npos;
        if (bar1 != std::string::npos) {
            const std::string occ = trim(s.substr(bar1 + 1, bar2 - bar1 - 1)), pos = trim(s.substr(bar2 + 1));
            int x = 0, y = 0;
            char* end = nullptr;
            const long n = std::strtol(occ.c_str(), &end, 10);
            if (end && *end == '\0' && n >= 1 && std::sscanf(pos.c_str(), "%d,%d", &x, &y) == 2) {
                out.name = trim(s.substr(0, bar1));
                out.occurrence = int(n);
                out.hasPos = true;
                out.x = x, out.y = y;
                return !out.name.empty();
            }
        }
    }
    out.name = s;   // (a name alone)
    return true;
}

// The index of the stored monitor in list, -1 for Primary or a monitor not connected (the caller uses the primary one).
inline int find(const std::vector<Info>& list, const std::string& id) {
    Parsed p;
    if (!parse(id, p)) return -1;
    if (p.plainIndex >= 0) return p.plainIndex < int(list.size()) ? p.plainIndex : -1;
    int first = -1, nth = -1, atPos = -1, seen = 0;
    for (int i = 0; i < int(list.size()); ++i) {
        const Info& m = list[size_t(i)];
        if (m.name != p.name && !(m.name.empty() && p.name == "Monitor")) continue;
        ++seen;
        if (first < 0) first = i;
        if (seen == p.occurrence) nth = i;
        if (p.hasPos && m.x == p.x && m.y == p.y && atPos < 0) atPos = i;
    }
    // (the position only tells same-named monitors apart: on Wayland every output is at 0,0)
    if (atPos >= 0 && sameNameCount(list, p.name) > 1) {
        int atPosCount = 0;
        for (const Info& m : list) atPosCount += m.name == p.name && m.x == p.x && m.y == p.y;
        if (atPosCount == 1) return atPos;
    }
    if (nth >= 0) return nth;
    return first;
}

inline int primaryIndex(const std::vector<Info>& list) {
    for (int i = 0; i < int(list.size()); ++i)
        if (list[size_t(i)].primary) return i;
    return list.empty() ? -1 : 0;
}
// The monitor to use: the stored one when connected, else the primary one (-1: no monitor at all).
inline int resolve(const std::vector<Info>& list, const std::string& id) {
    const int i = find(list, id);
    return i >= 0 ? i : primaryIndex(list);
}
// The stored monitor is one that is not connected now (shown as "<name> (not connected)").
inline bool missing(const std::vector<Info>& list, const std::string& id) {
    Parsed p;
    return parse(id, p) && find(list, id) < 0;
}
// The name a stored identifier shows ("" for Primary).
inline std::string storedName(const std::string& id) {
    Parsed p;
    if (!parse(id, p)) return {};
    if (p.plainIndex >= 0) return "#" + std::to_string(p.plainIndex + 1);
    return p.name;
}

// The monitor's name for a list ("DELL U2720Q", "DELL U2720Q (2)" for the second of that name).
inline std::string shortLabel(const std::vector<Info>& list, int index) {
    if (index < 0 || index >= int(list.size())) return {};
    const Info& m = list[size_t(index)];
    std::string s = m.name.empty() ? "Monitor " + std::to_string(index + 1) : m.name;
    if (sameNameCount(list, m.name) > 1) s += " (" + std::to_string(occurrence(list, index)) + ")";
    return s;
}
// ... with its mode: "DELL U2720Q - 3840x2160, 60 Hz"
inline std::string label(const std::vector<Info>& list, int index) {
    if (index < 0 || index >= int(list.size())) return {};
    const Info& m = list[size_t(index)];
    char mode[64] = "";
    if (m.width > 0 && m.height > 0) {
        if (m.refreshRate > 0) std::snprintf(mode, sizeof(mode), " - %dx%d, %d Hz", m.width, m.height, m.refreshRate);
        else std::snprintf(mode, sizeof(mode), " - %dx%d", m.width, m.height);
    }
    return shortLabel(list, index) + mode;
}

// The choices a Monitor option cycles: "" (Primary), then every connected monitor's identifier, then the stored one
// when it is not connected (kept until another is chosen).
inline std::vector<std::string> choices(const std::vector<Info>& list, const std::string& stored) {
    std::vector<std::string> v{ std::string() };
    for (int i = 0; i < int(list.size()); ++i) v.push_back(idOf(list, i));
    if (missing(list, stored)) v.push_back(trim(stored));
    return v;
}
// The index in choices() of the stored identifier (0 Primary).
inline int choiceIndex(const std::vector<Info>& list, const std::string& stored) {
    if (missing(list, stored)) return int(list.size()) + 1;
    const int i = find(list, stored);
    return i >= 0 ? i + 1 : 0;
}
// The next choice after the stored one (step +1 / -1, wrapping): the menus' left / right / confirm.
inline std::string step(const std::vector<Info>& list, const std::string& stored, int n) {
    const std::vector<std::string> v = choices(list, stored);
    const int count = int(v.size());
    const int i = ((choiceIndex(list, stored) + n) % count + count) % count;
    return v[size_t(i)];
}

// Copies an identifier into a fixed field (SystemSettings::monitor), NUL-terminated, never cutting a UTF-8 letter.
template <size_t N> inline void store(char (&field)[N], const std::string& id) {
    size_t n = id.size() < N - 1 ? id.size() : N - 1;
    while (n > 0 && n < id.size() && (static_cast<unsigned char>(id[n]) & 0xC0) == 0x80) --n;
    for (size_t i = 0; i < n; ++i) field[i] = id[i];
    for (size_t i = n; i < N; ++i) field[i] = '\0';
}
template <size_t N> inline std::string load(const char (&field)[N]) {
    size_t n = 0;
    while (n < N && field[n]) ++n;
    return std::string(field, n);
}

} // namespace cane::monitor
