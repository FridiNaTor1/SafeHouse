#pragma once
// Custom Vulkan drivers on Android (2026-11-24): a driver package the player imported (Mesa Turnip for Adreno GPUs, as
// emulators offer) loaded in place of the system's Vulkan driver through libadrenotools (BSD-2-Clause,
// Libraries/adrenotools). The launcher imports and chooses (launcher/android_drivers.h, Settings > Display > Vulkan
// driver); every game's Vulkan loader (platform/vulkan/vk_loader.cpp) uses the choice. Fail-safe by design:
// - a package that does not load (another GPU, a broken package, the emulator) leaves the system's driver in use;
// - a game that crashed while starting Vulkan on the custom driver (Android's record of its process, or no record)
//   sends the choice back to the system driver, and the launcher says why (lastError);
// - VR (an OpenXR runtime on the device) always uses the system's driver.
//
// Files under the app's files folder (HOME): drivers/<id>/ (the package: meta.json and its libraries),
// drivers/selected.txt ("<id>\n<library>\n<name>\n"; absent or empty: the system driver), drivers/starting.txt (the
// pid of a game starting Vulkan on the custom driver; removed when the start finished), drivers/last-error.txt.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(__ANDROID__)
#include <csignal>
#include <unistd.h>
#include "android_app.h"
#endif

namespace cane::android::vkdriver {

struct Selection {
    std::string id, library, name;
    bool empty() const { return id.empty() || library.empty(); }
};

inline std::filesystem::path root() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / "drivers";
}

inline std::string readFirstLines(const std::filesystem::path& path, std::string* second = nullptr, std::string* third = nullptr) {
    std::ifstream f(path, std::ios::binary);
    std::string first;
    if (!f) return first;
    std::getline(f, first);
    std::string line;
    if (second && std::getline(f, line)) *second = line;
    if (third && std::getline(f, line)) *third = line;
    auto trim = [](std::string& s) { while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back(); };
    trim(first);
    if (second) trim(*second);
    if (third) trim(*third);
    return first;
}

inline bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// The chosen driver (empty: the system's). A package that is gone counts as none.
inline Selection selection() {
    Selection s;
    s.id = readFirstLines(root() / "selected.txt", &s.library, &s.name);
    std::error_code ec;
    if (s.empty() || s.id.find('/') != std::string::npos || s.library.find('/') != std::string::npos ||
        !std::filesystem::is_regular_file(root() / s.id / s.library, ec))
        return {};
    return s;
}

inline void select(const Selection& s) {
    std::error_code ec;
    if (s.empty()) std::filesystem::remove(root() / "selected.txt", ec);
    else writeText(root() / "selected.txt", s.id + "\n" + s.library + "\n" + s.name + "\n");
}

inline std::string lastError() { return readFirstLines(root() / "last-error.txt"); }
inline void clearLastError() {
    std::error_code ec;
    std::filesystem::remove(root() / "last-error.txt", ec);
}

// Back to the system's driver, with the reason for the launcher.
inline void disable(const std::string& why) {
    select({});
    writeText(root() / "last-error.txt", why + "\n");
    std::printf("Vulkan: %s\n", why.c_str());
}

#if defined(__ANDROID__)
// A game that started Vulkan on the custom driver and never finished that start: crashed (or no record of how it
// ended) is true, *why says how; closed by the system or the user is false. Removes that process's marker.
inline bool previousStartCrashed(std::string* why) {
    const std::filesystem::path path = root() / "starting.txt";
    const std::string text = readFirstLines(path);
    if (text.empty()) return false;
    const long pid = std::strtol(text.c_str(), nullptr, 10);
    if (pid <= 0 || pid == static_cast<long>(getpid())) return false;
    if (kill(static_cast<pid_t>(pid), 0) == 0) return false;   // (another game still starting)
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const ExitRecord record = processExitRecord(pid);
    if (record.found && !record.crash) return false;
    *why = record.found ? record.description : std::string("the game ended while Vulkan was starting");
    return true;
}
inline void beginStart() { writeText(root() / "starting.txt", std::to_string(static_cast<long>(getpid())) + "\n"); }
inline void endStart() {
    const std::filesystem::path path = root() / "starting.txt";
    if (std::strtol(readFirstLines(path).c_str(), nullptr, 10) != static_cast<long>(getpid())) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
#endif

}  // namespace cane::android::vkdriver
