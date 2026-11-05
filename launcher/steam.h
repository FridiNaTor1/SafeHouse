#pragma once
// "Add to Steam": a non-Steam shortcut per game in Steam's shortcuts.vdf (the binary KeyValues format), for every
// Steam account on this computer, that starts the game directly (`safehouse --play <game>`, no launcher window), plus
// the bundled SteamGridDB artwork in userdata/<account>/config/grid/ (share/safehouse/steam, credits in
// launcher/assets/steam/CREDITS.md).
//
// Safety rules:
// - shortcuts.vdf is backed up (shortcuts.vdf.safehouse-<UTC time>.bak, and once shortcuts.vdf.safehouse-original.bak)
//   before every change, written to a temporary file
//   and renamed into place; a file that does not parse is never written;
// - an existing SafeHouse shortcut for the game is updated in place, never added twice. It is found by its app id, or by
//   its name + launch options + the "SafeHouse" tag (so a moved AppImage updates the old entry);
// - every other entry and field is written back byte for byte (unknown value types stop the change);
// - artwork never replaces an image the user already has for that shortcut.
// SAFEHOUSE_STEAM_ROOT=<dir> replaces the Steam folder search (tests use a fake Steam tree; scripts/test-steam-shortcuts.py).
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "games.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace cane::steam {
namespace detail {
inline std::string env(const char* key) {
#ifdef _WIN32
    const auto wide = path(key).wstring();
    const wchar_t* v = _wgetenv(wide.c_str());
    return v && *v ? utf8(fs::path(v)) : std::string{};
#else
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string{};
#endif
}
inline uint32_t crc32(const std::string& data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
} // namespace detail

// ---------------------------------------------------------------- binary KeyValues (shortcuts.vdf)
// Types: 0 map, 1 string, 2 int32, 3 float32, 7 uint64 (pointer / color in some files), 10 int64, 8 end of map.
struct Node {
    uint8_t type = 0;
    std::string key;
    std::string text;            // type 1
    std::vector<uint8_t> raw;    // types 2, 3, 7, 10: the value bytes, little endian
    std::vector<Node> children;  // type 0
    Node* find(const std::string& name) {
        for (auto& c : children)
            if (detail::iequals(c.key, name)) return &c;
        return nullptr;
    }
    const Node* find(const std::string& name) const { return const_cast<Node*>(this)->find(name); }
    std::string string(const std::string& name) const {
        const Node* n = find(name);
        return n && n->type == 1 ? n->text : std::string{};
    }
    std::optional<uint32_t> u32(const std::string& name) const {
        const Node* n = find(name);
        if (!n || n->type != 2 || n->raw.size() != 4) return std::nullopt;
        return uint32_t(n->raw[0]) | uint32_t(n->raw[1]) << 8 | uint32_t(n->raw[2]) << 16 | uint32_t(n->raw[3]) << 24;
    }
    void setString(const std::string& name, const std::string& value) {
        Node* n = find(name);
        if (!n) children.push_back({}), n = &children.back(), n->key = name;
        n->type = 1, n->text = value, n->raw.clear(), n->children.clear();
    }
    void setU32(const std::string& name, uint32_t value) {
        Node* n = find(name);
        if (!n) children.push_back({}), n = &children.back(), n->key = name;
        n->type = 2, n->text.clear(), n->children.clear();
        n->raw = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    }
    Node& map(const std::string& name) {
        Node* n = find(name);
        if (!n) children.push_back({}), n = &children.back(), n->key = name, n->type = 0;
        if (n->type != 0) throw std::runtime_error("shortcuts.vdf: \"" + name + "\" is not a section");
        return *n;
    }
};
class VdfReader {
public:
    explicit VdfReader(const std::vector<uint8_t>& data) : m_data(data) {}
    // The file is a map's children followed by its end byte.
    Node root() {
        Node r;
        r.type = 0;
        readChildren(r, 0);
        if (m_pos != m_data.size()) throw std::runtime_error("shortcuts.vdf: unexpected data after the end");
        return r;
    }

private:
    uint8_t byte() {
        if (m_pos >= m_data.size()) throw std::runtime_error("shortcuts.vdf: truncated file");
        return m_data[m_pos++];
    }
    std::string cstring() {
        std::string s;
        for (uint8_t c; (c = byte()) != 0;) s.push_back(static_cast<char>(c));
        return s;
    }
    void readChildren(Node& parent, int depth) {
        if (depth > 32) throw std::runtime_error("shortcuts.vdf: nested too deeply");
        for (;;) {
            const uint8_t type = byte();
            if (type == 8) return;
            Node n;
            n.type = type;
            n.key = cstring();
            switch (type) {
            case 0: readChildren(n, depth + 1); break;
            case 1: n.text = cstring(); break;
            case 2: case 3: for (int i = 0; i < 4; ++i) n.raw.push_back(byte()); break;
            case 7: case 10: for (int i = 0; i < 8; ++i) n.raw.push_back(byte()); break;
            default: throw std::runtime_error("shortcuts.vdf: unknown value type " + std::to_string(type));
            }
            parent.children.push_back(std::move(n));
        }
    }
    const std::vector<uint8_t>& m_data;
    size_t m_pos = 0;
};
inline void writeVdf(const Node& map, std::vector<uint8_t>& out) {
    auto str = [&](const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    };
    for (const auto& c : map.children) {
        out.push_back(c.type);
        str(c.key);
        if (c.type == 0) writeVdf(c, out);
        else if (c.type == 1) str(c.text);
        else out.insert(out.end(), c.raw.begin(), c.raw.end());
    }
    out.push_back(8);
}
inline Node emptyShortcuts() {
    Node root;
    root.map("shortcuts");
    return root;
}

// ---------------------------------------------------------------- Steam installation
struct Installation {
    fs::path root;                  // the folder with userdata/
    std::vector<fs::path> accounts; // userdata/<account id>
    bool flatpak = false;
};
inline std::vector<fs::path> candidateRoots() {
    std::vector<fs::path> roots;
    if (auto o = detail::env("SAFEHOUSE_STEAM_ROOT"); !o.empty()) return {path(o)};
#ifdef _WIN32
    auto reg = [&](HKEY hive, const wchar_t* key, const wchar_t* value) {
        wchar_t buffer[1024];
        DWORD size = sizeof(buffer);
        if (RegGetValueW(hive, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
            roots.push_back(fs::path(buffer));
    };
    reg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    for (const char* key : {"ProgramFiles(x86)", "ProgramFiles"})
        if (auto p = detail::env(key); !p.empty()) roots.push_back(path(p) / "Steam");
#else
    const auto home = detail::env("HOME");
    if (home.empty()) return roots;
    const fs::path h = path(home);
#if defined(__APPLE__)
    roots.push_back(h / "Library/Application Support/Steam");
#else
    for (const auto& p : {h / ".steam/steam", h / ".steam/root", h / ".local/share/Steam",
                          h / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                          h / ".var/app/com.valvesoftware.Steam/.steam/steam"})
        roots.push_back(p);
#endif
#endif
    return roots;
}
inline std::optional<Installation> find() {
    std::vector<fs::path> seen;
    for (const auto& candidate : candidateRoots()) {
        std::error_code ec;
        if (!fs::is_directory(candidate / "userdata", ec)) continue;
        const auto canonical = fs::weakly_canonical(candidate, ec);
        if (ec || std::find(seen.begin(), seen.end(), canonical) != seen.end()) continue;
        seen.push_back(canonical);
        Installation s;
        s.root = canonical;
        s.flatpak = utf8(canonical).find("com.valvesoftware.Steam") != std::string::npos;
        for (const auto& entry : fs::directory_iterator(canonical / "userdata", ec)) {
            const auto name = utf8(entry.path().filename());
            if (!entry.is_directory() || name.empty() || name == "0" || name.find_first_not_of("0123456789") != std::string::npos)
                continue;
            s.accounts.push_back(entry.path());
        }
        std::sort(s.accounts.begin(), s.accounts.end());
        if (!s.accounts.empty()) return s;
    }
    return std::nullopt;
}
// Steam keeps its own copy of the shortcuts while it runs: a change shows after Steam restarts, and Steam may write
// its copy back over the file. Linux: ~/.steam/steam.pid names a live "steam" process. Windows: the
// HKCU\Software\Valve\Steam\ActiveProcess pid is a live steam.exe.
inline bool running([[maybe_unused]] const Installation& s) {
#ifdef _WIN32
    DWORD pid = 0, size = sizeof(pid);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid", RRF_RT_REG_DWORD, nullptr, &pid,
                     &size) != ERROR_SUCCESS || !pid)
        return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &e); ok; ok = Process32NextW(snapshot, &e))
        if (e.th32ProcessID == pid && _wcsicmp(e.szExeFile, L"steam.exe") == 0) found = true;
    CloseHandle(snapshot);
    return found;
#else
    std::vector<fs::path> pidFiles{s.root / "steam.pid"};  // a fake tree's own pid file (tests)
    if (detail::env("SAFEHOUSE_STEAM_ROOT").empty()) {
        const fs::path h = path(detail::env("HOME"));
        pidFiles = {h / ".steam/steam.pid", h / ".var/app/com.valvesoftware.Steam/.steam/steam.pid"};
    }
    for (const auto& file : pidFiles) {
        std::ifstream in(file);
        long pid = 0;
        if (!(in >> pid) || pid <= 0) continue;
        std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
        std::string name;
        if (std::getline(comm, name) && (name == "steam" || name == "steamwebhelper")) return true;
    }
    return false;
#endif
}

// ---------------------------------------------------------------- the shortcut
struct Shortcut {
    std::string name;          // AppName: the game's title
    std::string exe;           // quoted program path
    std::string startDir;      // quoted folder
    std::string launchOptions; // --play <game>
    uint32_t appId = 0;        // crc32(exe + name) | 0x80000000, as Steam computes it for non-Steam shortcuts
};
inline uint32_t shortcutAppId(const std::string& exe, const std::string& name) {
    return detail::crc32(exe + name) | 0x80000000u;
}
inline std::string quoted(const fs::path& p) { return "\"" + utf8(p) + "\""; }
// The program Steam starts. In the AppImage it is the AppImage file itself ($APPIMAGE; the mounted copy disappears
// when the launcher exits); in the tar.gz and on Windows it is bin/safehouse[.exe].
inline fs::path launcherProgram() {
#ifndef _WIN32
    if (auto image = detail::env("APPIMAGE"); !image.empty()) return path(image);
    return executableDirectory() / "safehouse";
#else
    return executableDirectory() / "safehouse.exe";
#endif
}
inline Shortcut shortcutFor(GameId game, const fs::path& program = launcherProgram()) {
    Shortcut s;
    s.name = cane::game(game).title;
    s.exe = quoted(program);
    s.startDir = quoted(program.parent_path());
    s.launchOptions = std::string("--play ") + cane::game(game).key;
    s.appId = shortcutAppId(s.exe, s.name);
    return s;
}
inline constexpr const char* kTag = "SafeHouse";
inline bool tagged(const Node& entry) {
    const Node* tags = entry.find("tags");
    if (!tags || tags->type != 0) return false;
    for (const auto& t : tags->children)
        if (t.type == 1 && t.text == kTag) return true;
    return false;
}
// The entry for this shortcut: same app id, or one of ours for the same game (name, options and tag).
inline Node* findEntry(Node& shortcuts, const Shortcut& s) {
    for (auto& e : shortcuts.children)
        if (e.type == 0 && e.u32("appid") == s.appId) return &e;
    for (auto& e : shortcuts.children)
        if (e.type == 0 && e.string("AppName") == s.name && e.string("LaunchOptions") == s.launchOptions && tagged(e)) return &e;
    return nullptr;
}
inline std::vector<uint8_t> readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
inline fs::path shortcutsFile(const fs::path& account) { return account / "config" / "shortcuts.vdf"; }
inline Node readShortcuts(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return emptyShortcuts();
    const auto data = readFile(file);
    if (data.empty()) return emptyShortcuts();
    Node root = VdfReader(data).root();
    root.map("shortcuts");
    return root;
}
// Is the game's shortcut in every account?
inline bool added(const Installation& s, GameId game) {
    const Shortcut sc = shortcutFor(game);
    if (s.accounts.empty()) return false;
    for (const auto& account : s.accounts) {
        try {
            Node root = readShortcuts(shortcutsFile(account));
            if (!findEntry(root.map("shortcuts"), sc)) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}
struct Result {
    int accounts = 0, addedEntries = 0, updatedEntries = 0, images = 0;
    std::vector<fs::path> backups;
};
inline std::string utcStamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%SZ", &tm);
    return buffer;
}
// Copies grid.*, wide.*, hero.*, logo.*, icon.* from art/ as <id>p, <id>, <id>_hero, <id>_logo, <id>_icon. Returns
// the icon's path (for the shortcut's "icon" field), empty if there is none.
inline fs::path copyArt(const fs::path& art, const fs::path& grid, uint32_t appId, int& copied) {
    static constexpr std::array<std::pair<const char*, const char*>, 5> slots{
        {{"grid", "p"}, {"wide", ""}, {"hero", "_hero"}, {"logo", "_logo"}, {"icon", "_icon"}}};
    fs::path icon;
    std::error_code ec;
    if (!fs::is_directory(art, ec)) return icon;
    fs::create_directories(grid);
    for (const auto& [source, suffix] : slots) {
        const std::string stem = std::to_string(appId) + suffix;
        fs::path from;
        for (const char* ext : {".png", ".jpg"})
            if (fs::is_regular_file(art / (std::string(source) + ext), ec)) from = art / (std::string(source) + ext);
        if (from.empty()) continue;
        fs::path existing;
        for (const char* ext : {".png", ".jpg", ".jpeg", ".webp", ".ico"})
            if (fs::exists(grid / (stem + ext), ec)) existing = grid / (stem + ext);
        if (existing.empty()) {
            existing = grid / (stem + utf8(from.extension()));
            fs::copy_file(from, existing, fs::copy_options::overwrite_existing);
            ++copied;
        }
        if (std::string(source) == "icon") icon = existing;
    }
    return icon;
}
// The bundled artwork: <prefix>/share/safehouse/steam/<game> (SAFEHOUSE_STEAM_ART=<dir> replaces <prefix>/.../steam,
// for tests run from a build tree).
inline fs::path artDirectory(GameId game) {
    if (auto o = detail::env("SAFEHOUSE_STEAM_ART"); !o.empty()) return path(o) / cane::game(game).key;
    return shareDirectory() / "safehouse" / "steam" / cane::game(game).key;
}
// Adds (or updates) the game's shortcut in every account. Throws before writing anything for an account whose
// shortcuts.vdf cannot be read.
inline Result add(const Installation& s, GameId game, const fs::path& program = launcherProgram(),
                  const fs::path& art = {}) {
    const Shortcut sc = shortcutFor(game, program);
    const fs::path artDir = art.empty() ? artDirectory(game) : art;
    Result result;
    const std::string stamp = utcStamp();
    for (const auto& account : s.accounts) {
        const fs::path file = shortcutsFile(account);
        Node root = readShortcuts(file);  // throws on a damaged file: nothing is written for it
        Node& shortcuts = root.map("shortcuts");
        Node* entry = findEntry(shortcuts, sc);
        const bool isNew = !entry;
        if (isNew) {
            long next = 0;
            for (const auto& e : shortcuts.children) {
                char* end = nullptr;
                const long n = std::strtol(e.key.c_str(), &end, 10);
                if (end && !*end && n >= next) next = n + 1;
            }
            shortcuts.children.push_back({});
            entry = &shortcuts.children.back();
            entry->type = 0;
            entry->key = std::to_string(next);
        }
        int copied = 0;
        const fs::path icon = copyArt(artDir, account / "config" / "grid", sc.appId, copied);
        entry->setU32("appid", sc.appId);
        entry->setString("AppName", sc.name);
        entry->setString("Exe", sc.exe);
        entry->setString("StartDir", sc.startDir);
        entry->setString("icon", icon.empty() ? entry->string("icon") : utf8(icon));
        if (isNew) {
            entry->setString("ShortcutPath", "");
            entry->setString("LaunchOptions", sc.launchOptions);
            entry->setU32("IsHidden", 0);
            entry->setU32("AllowDesktopConfig", 1);
            entry->setU32("AllowOverlay", 1);
            entry->setU32("OpenVR", 0);
            entry->setU32("Devkit", 0);
            entry->setString("DevkitGameID", "");
            entry->setU32("DevkitOverrideAppID", 0);
            entry->setU32("LastPlayTime", 0);
            entry->setString("FlatpakAppID", "");
            entry->setString("sortas", "");
        } else {
            entry->setString("LaunchOptions", sc.launchOptions);
        }
        Node& tags = entry->map("tags");
        if (!tagged(*entry)) {
            long next = 0;
            for (const auto& t : tags.children) {
                char* end = nullptr;
                const long n = std::strtol(t.key.c_str(), &end, 10);
                if (end && !*end && n >= next) next = n + 1;
            }
            tags.setString(std::to_string(next), kTag);
        }
        std::vector<uint8_t> out;
        writeVdf(root, out);
        fs::create_directories(file.parent_path());
        std::error_code ec;
        if (fs::exists(file, ec)) {
            if (readFile(file) == out) {  // already up to date
                ++result.accounts;
                result.images += copied;
                continue;
            }
            // The first change ever also keeps shortcuts.vdf.safehouse-original.bak; no backup is ever overwritten.
            auto original = file;
            original += ".safehouse-original.bak";
            if (!fs::exists(original, ec)) fs::copy_file(file, original);
            auto backup = file;
            backup += ".safehouse-" + stamp + ".bak";
            for (int n = 2; fs::exists(backup, ec); ++n) {
                backup = file;
                backup += ".safehouse-" + stamp + "-" + std::to_string(n) + ".bak";
            }
            fs::copy_file(file, backup);
            result.backups.push_back(backup);
        }
        auto temporary = file;
        temporary += ".safehouse-writing";
        {
            std::ofstream o(temporary, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            if (!o) throw std::runtime_error("Could not write " + utf8(temporary));
        }
        // A check that Steam's reader would see the same entries: parse what was written.
        Node check = VdfReader(readFile(temporary)).root();
        if (!findEntry(check.map("shortcuts"), sc)) throw std::runtime_error("shortcuts.vdf check failed");
        fs::rename(temporary, file);
        ++result.accounts;
        result.images += copied;
        (isNew ? result.addedEntries : result.updatedEntries)++;
    }
    return result;
}
} // namespace cane::steam
