#ifndef CANE_PLATFORM_RENDERER_CHOICE_H_
#define CANE_PLATFORM_RENDERER_CHOICE_H_
// Which renderer the game tries first (Sly 2, its renderer.cpp): Auto is Vulkan with OpenGL as the fallback,
// Vulkan and OpenGL are the explicit choices. No window or GPU code here, so the rules are tested without a GPU (the
// games' --renderer-selftest, renderer_choice_test.h).
//
// The start marker: <user-dir>/vulkan-start.txt is written just before Vulkan starts and removed once the game has shown
// its first frames on Vulkan (or exits normally before that). A marker left behind means the last Vulkan start did not
// finish: the game crashed or was killed inside the driver, or Vulkan reported an error it cannot recover from (the
// reason is then written into the marker). Vulkan failures the game catches at start (no Vulkan 1.3 device, a missing
// feature, a device or swapchain that cannot be created) fall back to OpenGL in the same run and are remembered the
// same way; a machine without a Vulkan loader falls back before any window exists and is not remembered. While a marker of this game version is there, Auto starts on OpenGL at once; choosing Vulkan explicitly
// tries again (and removes the marker when it starts), and a new game version tries Auto's Vulkan again.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // (GetCurrentProcessId; platform/process.h shadows the CRT's <process.h>)
#elif defined(__linux__) || defined(__APPLE__)
#include <csignal>
#include <unistd.h>
#define CANE_RENDERER_CHOICE_POSIX 1
#endif

namespace cane::renderer {

enum class Choice { Auto = 0, Vulkan = 1, OpenGL = 2 };

// Auto's preference: Vulkan, OpenGL where Vulkan cannot start.
inline constexpr bool kAutoPrefersVulkan = true;

// The start marker's contents.
struct Marker {
    bool present = false;
    long pid = 0;               // the process that wrote it
    std::string version;        // the game version that wrote it
    std::string reason;         // why Vulkan did not start ("" while that start was still running)
};

struct Decision {
    bool tryVulkan = false;
    std::string why;            // for the log when Vulkan is not tried ("" when there is nothing worth saying)
};

inline long currentProcessId() {
#if defined(_WIN32)
    return static_cast<long>(GetCurrentProcessId());
#elif CANE_RENDERER_CHOICE_POSIX
    return static_cast<long>(getpid());
#else
    return 1;
#endif
}

// True while the process that wrote a marker still runs (another copy of the game starting at the same time).
inline bool processRunning(long pid) {
    if (pid <= 0 || pid == currentProcessId()) return false;
#if CANE_RENDERER_CHOICE_POSIX
    return kill(static_cast<pid_t>(pid), 0) == 0;
#else
    return false;   // (two copies starting at once on one profile: the launcher does not do that)
#endif
}

inline std::string describe(const Marker& m) {
    return m.reason.empty() ? std::string("the game stopped while Vulkan was starting") : m.reason;
}

// The rules. builtWithVulkan: the game has the Vulkan renderer (Linux and Windows builds); vr: an OpenXR session is wanted
// (VR runs on OpenGL); marker: the start marker read from the user folder; version: this build's version.
inline Decision decide(Choice wanted, bool builtWithVulkan, bool vr, const Marker& marker, const std::string& version,
                       bool autoPrefersVulkan = kAutoPrefersVulkan) {
    Decision d;
    if (wanted == Choice::OpenGL) return d;
    if (!builtWithVulkan) {
        if (wanted == Choice::Vulkan) d.why = "this build has no Vulkan renderer";
        return d;
    }
    if (vr) {
        d.why = "VR runs on OpenGL";
        return d;
    }
    if (wanted == Choice::Auto) {
        if (!autoPrefersVulkan) return d;
        // (a marker of another copy that is still starting is not a failure)
        const bool failed = marker.present && marker.version == version && !(marker.reason.empty() && processRunning(marker.pid));
        if (failed) {
            d.why = "Vulkan did not start last time (" + describe(marker) + "); choose Vulkan in the settings to try it again";
            return d;
        }
    }
    d.tryVulkan = true;
    return d;
}

inline std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

inline Marker readMarker(const std::filesystem::path& path) {
    Marker m;
    std::ifstream f(path, std::ios::binary);
    if (!f) return m;
    m.present = true;
    std::string line;
    while (std::getline(f, line)) {
        line = trimmed(line);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "pid") m.pid = std::strtol(value.c_str(), nullptr, 10);
        else if (key == "version") m.version = value;
        else if (key == "reason") m.reason = value;
    }
    return m;
}

// reason "": a start in progress. Written to a temporary file and renamed, so a crash never leaves half a marker.
inline bool writeMarker(const std::filesystem::path& path, const std::string& version, const std::string& reason) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "# Vulkan's last start did not finish: Auto uses OpenGL until Vulkan is chosen and starts, or the game is updated.\n"
          << "pid=" << currentProcessId() << "\nversion=" << version << "\nreason=" << reason << "\n";
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

inline void clearMarker(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace cane::renderer

#endif  // CANE_PLATFORM_RENDERER_CHOICE_H_
