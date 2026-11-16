#pragma once
// The places Settings > Games "Install games to" offers besides the default folder: the computer's other drives
// (Windows: every fixed or removable drive letter; Linux: the mounted drives under /run/media/<user>, /media/<user>,
// /media and /mnt, e.g. the Steam Deck's microSD card; macOS: /Volumes), each with its free space. A game installs
// into <drive>/SafeHouse/<game> there (LauncherBackend::installRootFor()). Any other folder comes from "Choose a
// folder…" (the system's folder picker).
#include <string>
#include <vector>

#include "backend.h"
#ifdef _WIN32
#include <cwctype>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cane {
struct InstallPlace {
    fs::path folder;   // the folder chosen (a drive's root); the install root is installRootFor(folder)
    std::string label;  // "D: Games", "SD card (mmcblk0p1)"
};
inline uint64_t freeBytes(const fs::path& where) {
    std::error_code ec;
    fs::path p = where;
    while (!p.empty() && !fs::exists(p, ec)) {
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    const auto s = fs::space(p, ec);
    return ec ? 0 : s.available;
}
inline std::string freeText(uint64_t bytes) {
    char b[48];
    if (bytes >= (1000ull << 30)) std::snprintf(b, sizeof(b), tr("%.1f TB free"), double(bytes) / double(1ull << 40));
    else std::snprintf(b, sizeof(b), tr("%.0f GB free"), double(bytes) / double(1ull << 30));
    return b;
}
inline std::vector<InstallPlace> otherDrives() {
    std::vector<InstallPlace> out;
#ifdef _WIN32
    const DWORD mask = GetLogicalDrives();
    wchar_t system[MAX_PATH] = {};
    GetWindowsDirectoryW(system, MAX_PATH);
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[4] = {wchar_t(L'A' + i), L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        if (system[0] && towupper(system[0]) == root[0]) continue;  // the system drive: the default folder is there
        wchar_t name[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(root, name, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) continue;  // no media
        std::string label = utf8(fs::path(std::wstring(root, 2)));
        if (name[0]) label += " " + utf8(fs::path(name));
        out.push_back({fs::path(root), label});
    }
#else
    struct stat rootStat {};
    stat("/", &rootStat);
    const auto mounted = [&](const fs::path& p) {
        struct stat s {}, parent {};
        return stat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode) && stat(p.parent_path().c_str(), &parent) == 0 && s.st_dev != parent.st_dev &&
               s.st_dev != rootStat.st_dev && access(p.c_str(), W_OK) == 0;
    };
    std::vector<fs::path> bases;
#ifdef __APPLE__
    bases.push_back("/Volumes");
#else
    const char* user = std::getenv("USER");
    if (user && *user) bases.push_back(fs::path("/run/media") / user), bases.push_back(fs::path("/media") / user);
    bases.push_back("/media");
    bases.push_back("/mnt");
#endif
    for (const auto& base : bases) {
        std::error_code ec;
        for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            if (!mounted(p)) continue;
            if (std::any_of(out.begin(), out.end(), [&](const InstallPlace& o) { return o.folder == p; })) continue;
            std::string name = utf8(p.filename());
            const bool card = name.rfind("mmcblk", 0) == 0;
            out.push_back({p, card ? trf("SD card (%s)", name.c_str()) : name});
        }
    }
#endif
    return out;
}
}  // namespace cane
