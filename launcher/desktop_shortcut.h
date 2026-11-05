#pragma once
// "Add desktop shortcut": a shortcut on the user's desktop (and, if asked, in the applications menu / Start menu) that
// starts a game directly, like the Steam shortcut (`safehouse --play <game>`, no launcher window; the region selected in
// SafeHouse). Sly 1 and Sly 2 only for now.
//
// Linux: <desktop>/safehouse-<game>.desktop (desktop = XDG_DESKTOP_DIR from user-dirs.dirs, as `xdg-user-dir DESKTOP`
// reads it, else ~/Desktop), executable and, where `gio` exists, metadata::trusted (GNOME; KDE needs only +x). The
// menu copy is $XDG_DATA_HOME/applications/safehouse-<game>.desktop. Icon: the game's logo on a transparent square
// (launcher/assets/shortcut/<game>-logo.png, 256x256, embedded in the launcher) written to
// $XDG_DATA_HOME/icons/hicolor/256x256/apps/safehouse-<game>.png and named by absolute path (the AppImage's own share/
// disappears when it exits).
// Windows: Desktop\<title>.lnk (SHGetKnownFolderPath(FOLDERID_Desktop), IShellLinkW + IPersistFile), and
// Start Menu\Programs\SafeHouse\<title>.lnk. Icon: the same logo as an .ico embedded in safehouse.exe (resource id
// 101 + game, launcher/assets/shortcut/<game>-logo.ico). Both are made by scripts/make-shortcut-icons.py.
// (Until 2026-11-05 the icon was the SteamGridDB icon bundled for "Add to Steam", in hicolor/128x128.)
// A shortcut has a fixed file name per game, so adding again updates it in place.
// SAFEHOUSE_GIO=<program> replaces `gio` (tests; "" or "none" skips it).
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "brand.h"
#include "games.h"
#include "platform.h"
#include "process.h"
#include "steam.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#else
#include <sys/stat.h>
#include "launcher_shortcut_sly1.h"
#include "launcher_shortcut_sly2.h"
#endif

namespace cane::shortcut {
// Games that get the button (the user's call: Sly 3 later). macOS has no .desktop files: no button there.
inline bool supported(GameId game) {
#if defined(__APPLE__)
    (void)game;
    return false;
#else
    return game == GameId::Sly1 || game == GameId::Sly2;
#endif
}
// The window class the game's window has (GLFW uses the window title when no class is set): StartupWMClass.
inline const char* windowClass(GameId game) {
    return game == GameId::Sly1 ? "Sly Cooper and the Thievius Raccoonus" : game == GameId::Sly2 ? "Sly2-Release" : "Sly3-Release";
}
// The icon's caption in the dialog.
inline const char* iconCredit(GameId game) { return supported(game) ? "The shortcut's icon is the game's logo." : ""; }
// Windows icon resource ids (cmake/launcher.rc.in).
inline int iconResource(GameId game) { return 101 + static_cast<int>(game); }

enum class Where { Desktop, Menu };
inline const char* whereName(Where w) {
#ifdef _WIN32
    return w == Where::Desktop ? "desktop" : "Start menu";
#else
    return w == Where::Desktop ? "desktop" : "applications menu";
#endif
}

namespace detail {
using steam::detail::env;
#ifndef _WIN32
inline fs::path home() { return path(env("HOME")); }
inline fs::path dataHome() {
    const auto d = env("XDG_DATA_HOME");
    return !d.empty() && path(d).is_absolute() ? path(d) : home() / ".local" / "share";
}
// XDG_DESKTOP_DIR as xdg-user-dir reads it: user-dirs.dirs (a shell fragment: XDG_DESKTOP_DIR="$HOME/Desktop"), then
// the environment. A value equal to $HOME means "no desktop folder" there; ~/Desktop is used then.
inline fs::path desktopFolder() {
    const fs::path h = home();
    std::string value;
    const auto config = env("XDG_CONFIG_HOME");
    std::ifstream in((!config.empty() && path(config).is_absolute() ? path(config) : h / ".config") / "user-dirs.dirs");
    for (std::string line; std::getline(in, line);) {
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        line = line.substr(start);
        if (line.rfind("XDG_DESKTOP_DIR=", 0) != 0) continue;
        std::string v = line.substr(16);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        std::string out;  // the format allows only $HOME and backslash escapes
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size()) out += v[++i];
            else if (v.compare(i, 5, "$HOME") == 0) out += utf8(h), i += 4;
            else out += v[i];
        }
        value = out;
    }
    if (value.empty()) value = env("XDG_DESKTOP_DIR");
    fs::path p = path(value);
    std::error_code ec;
    if (value.empty() || !p.is_absolute() || fs::equivalent(p, h, ec) || p.lexically_normal() == h.lexically_normal())
        p = h / "Desktop";
    return p;
}
// Desktop Entry Exec quoting: each argument in double quotes with " ` $ \ escaped, then the string-value escapes
// (backslash doubled, so a literal backslash in a path is four), and % doubled.
inline std::string execArgument(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\"'\\><~|&;$*?#()`=") == std::string::npos) return arg;  // no reserved character
    std::string q = "\"";
    for (char c : arg) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') q += '\\';
        q += c;
    }
    q += '"';
    return q;
}
inline std::string stringValue(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}
inline std::string execValue(const std::vector<std::string>& args) {
    std::string line;
    for (const auto& a : args) line += (line.empty() ? "" : " ") + execArgument(a);
    std::string out;
    for (char c : stringValue(line)) out += c == '%' ? std::string("%%") : std::string(1, c);
    return out;
}
inline std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
inline void writeAtomically(const fs::path& file, const std::string& text) {
    auto temporary = file;
    temporary += ".safehouse-writing";
    {
        std::ofstream o(temporary, std::ios::binary | std::ios::trunc);
        o << text;
        if (!o) throw std::runtime_error("Could not write " + utf8(temporary));
    }
    fs::rename(temporary, file);
}
#endif
} // namespace detail

#ifndef _WIN32
inline fs::path folder(Where where) {
    return where == Where::Desktop ? detail::desktopFolder() : detail::dataHome() / "applications";
}
inline fs::path file(GameId game, Where where) {
    return folder(where) / (std::string("safehouse-") + cane::game(game).key + ".desktop");
}
inline fs::path installedIcon(GameId game, const char* size = "256x256") {
    return detail::dataHome() / "icons" / "hicolor" / size / "apps" / (std::string("safehouse-") + cane::game(game).key + ".png");
}
// The icon PNG embedded in the launcher (empty for a game without a shortcut).
inline std::string iconImage(GameId game) {
    if (game == GameId::Sly1) return std::string(reinterpret_cast<const char*>(cane_shortcut_sly1), sizeof(cane_shortcut_sly1));
    if (game == GameId::Sly2) return std::string(reinterpret_cast<const char*>(cane_shortcut_sly2), sizeof(cane_shortcut_sly2));
    return {};
}
// The .desktop text. icon: an absolute PNG path, or a theme icon name.
inline std::string desktopEntry(GameId game, const fs::path& program, const std::string& icon) {
    const auto& g = cane::game(game);
    std::string t = "[Desktop Entry]\n";
    t += "Type=Application\n";
    t += "Version=1.0\n";
    t += std::string("Name=") + g.title + "\n";
    t += std::string("Comment=Play ") + g.title + " (SafeHouse)\n";
    t += "Exec=" + detail::execValue({utf8(program), "--play", g.key}) + "\n";
    t += "Path=" + detail::stringValue(utf8(program.parent_path())) + "\n";
    t += "Icon=" + detail::stringValue(icon) + "\n";
    t += "Terminal=false\n";
    t += "Categories=Game;\n";
    t += std::string("StartupWMClass=") + windowClass(game) + "\n";
    t += std::string("X-SafeHouse-Game=") + g.key + "\n";
    return t;
}
// Writes the icon PNG to the user's hicolor folder (if it changed); returns the Icon= value.
inline std::string installIcon(GameId game) {
    const std::string image = iconImage(game);
    if (image.empty()) return brand::appId;  // the SafeHouse icon
    const fs::path to = installedIcon(game);
    std::error_code ec;
    fs::create_directories(to.parent_path());
    if (!fs::exists(to, ec) || detail::readText(to) != image) detail::writeAtomically(to, image);
    // The old icon (the SteamGridDB one, 128x128, before 2026-11-05) under the same name in a smaller size would win
    // a theme lookup by name at that size: removed. Shortcuts name the icon by path, so nothing else used it.
    fs::remove(installedIcon(game, "128x128"), ec);
    return utf8(to);
}
#else
namespace detail {
inline fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) result = fs::path(p);
    CoTaskMemFree(p);
    if (result.empty()) throw std::runtime_error("Windows did not report the folder for the shortcut");
    return result;
}
// "Sly 2: Band of Thieves" -> "Sly 2 - Band of Thieves" (a file name).
inline std::wstring fileTitle(GameId game) {
    std::string t = cane::game(game).title, out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == ':') out += " -";
        else if (std::strchr("<>\"/\\|?*", t[i])) continue;
        else out += t[i];
    }
    return path(out).wstring();
}
struct Com {
    bool owned = false;
    Com() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(hr);  // S_FALSE too; RPC_E_CHANGED_MODE: already initialized differently, still usable
    }
    ~Com() {
        if (owned) CoUninitialize();
    }
};
template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};
} // namespace detail
inline fs::path folder(Where where) {
    return where == Where::Desktop ? detail::knownFolder(FOLDERID_Desktop) : detail::knownFolder(FOLDERID_Programs) / L"SafeHouse";
}
inline fs::path file(GameId game, Where where) { return folder(where) / (detail::fileTitle(game) + L".lnk"); }
struct LinkInfo {
    std::string target, arguments, workingDirectory, iconFile, description;
    int iconIndex = 0;
};
inline LinkInfo readLink(const fs::path& lnk) {
    detail::Com com;
    detail::Ptr<IShellLinkW> link;
    detail::Ptr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link.p))) ||
        FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file.p))) || FAILED(file->Load(lnk.c_str(), STGM_READ)))
        throw std::runtime_error("Could not read " + utf8(lnk));
    wchar_t buffer[2048];
    LinkInfo info;
    if (SUCCEEDED(link->GetPath(buffer, 2048, nullptr, SLGP_RAWPATH))) info.target = utf8(fs::path(buffer));
    if (SUCCEEDED(link->GetArguments(buffer, 2048))) info.arguments = utf8(fs::path(buffer));
    if (SUCCEEDED(link->GetWorkingDirectory(buffer, 2048))) info.workingDirectory = utf8(fs::path(buffer));
    if (SUCCEEDED(link->GetDescription(buffer, 2048))) info.description = utf8(fs::path(buffer));
    if (SUCCEEDED(link->GetIconLocation(buffer, 2048, &info.iconIndex))) info.iconFile = utf8(fs::path(buffer));
    return info;
}
#endif

// A path for the dialogs: under $HOME as ~/... on Linux.
inline std::string displayPath(const fs::path& p) {
    std::string text = utf8(p);
#ifndef _WIN32
    const std::string h = utf8(detail::home().lexically_normal());
    if (!h.empty() && h != "/" && text.rfind(h + "/", 0) == 0) text = "~" + text.substr(h.size());
#endif
    return text;
}
inline bool exists(GameId game, Where where) {
    try {
        std::error_code ec;
        return fs::exists(file(game, where), ec);
    } catch (...) {
        return false;
    }
}

struct Result {
    fs::path file;
    bool existed = false;  // there was a shortcut (updated in place)
    bool changed = false;  // the file was written
    bool trusted = false;  // Linux: gio set metadata::trusted succeeded
};
// Creates or updates the game's shortcut in that place.
inline Result create(GameId game, Where where, const fs::path& program = steam::launcherProgram()) {
    if (!supported(game)) throw std::runtime_error(std::string("Desktop shortcuts are not available for ") + cane::game(game).shortName + " yet");
    Result r;
    r.file = file(game, where);
    std::error_code ec;
    r.existed = fs::exists(r.file, ec);
    fs::create_directories(r.file.parent_path());
#ifndef _WIN32
    const std::string text = desktopEntry(game, program, installIcon(game));
    if (!r.existed || detail::readText(r.file) != text) {
        detail::writeAtomically(r.file, text);
        r.changed = true;
    }
    // Executable (KDE, and GNOME's "Allow launching"); rw for the user, readable by others.
    fs::permissions(r.file, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                                fs::perms::others_exec, fs::perm_options::replace);
    if (where == Where::Desktop) {
        std::string gio = "gio";
        if (const char* o = std::getenv("SAFEHOUSE_GIO")) gio = o;
        if (!gio.empty() && gio != "none") {
            std::atomic<bool> cancel{false};
            try {
                r.trusted = runProcess({gio, "set", utf8(r.file), "metadata::trusted", "true"}, r.file.parent_path(), cancel,
                                       [](const std::string&) {}) == 0;
            } catch (...) {
            }
        }
    }
#else
    detail::Com com;
    // An existing link that already says the same is left alone (no rewrite, "up to date"), as on Linux.
    if (r.existed) {
        try {
            const auto l = readLink(r.file);
            const auto same = [](const std::string& a, const fs::path& b) { return path(a).lexically_normal() == b.lexically_normal(); };
            if (same(l.target, program) && l.arguments == std::string("--play ") + cane::game(game).key &&
                same(l.workingDirectory, program.parent_path()) && same(l.iconFile, program) && l.iconIndex == -iconResource(game) &&
                l.description == std::string("Play ") + cane::game(game).title + " (SafeHouse)")
                return r;
        } catch (...) {
        }
    }
    detail::Ptr<IShellLinkW> link;
    detail::Ptr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link.p))))
        throw std::runtime_error("Windows could not create the shortcut (IShellLink)");
    const std::wstring args = L"--play " + path(cane::game(game).key).wstring();
    const std::wstring description = path(std::string("Play ") + cane::game(game).title + " (SafeHouse)").wstring();
    if (FAILED(link->SetPath(program.c_str())) || FAILED(link->SetArguments(args.c_str())) ||
        FAILED(link->SetWorkingDirectory(program.parent_path().c_str())) || FAILED(link->SetDescription(description.c_str())) ||
        FAILED(link->SetIconLocation(program.c_str(), -iconResource(game))))
        throw std::runtime_error("Windows could not set up the shortcut");
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist.p))) || FAILED(persist->Save(r.file.c_str(), TRUE)))
        throw std::runtime_error("Could not save " + utf8(r.file));
    r.changed = true;
    SHChangeNotify(r.existed ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, r.file.c_str(), nullptr);
#endif
    return r;
}
} // namespace cane::shortcut
