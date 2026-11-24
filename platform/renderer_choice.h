#ifndef CANE_PLATFORM_RENDERER_CHOICE_H_
#define CANE_PLATFORM_RENDERER_CHOICE_H_
// Which renderer a game tries first (Sly 2 and Sly 3, their renderer.cpp): Auto is Vulkan with OpenGL as the fallback,
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
//
// Not forever (2026-11-24, the S26 Ultra stuck on OpenGL ES): the marker counts its failed starts and the Auto starts
// it sent to OpenGL since; after retryAfterStarts(failures) of those (3, 6, 12) Auto tries Vulkan again by itself, after
// a fourth failure only an update or the explicit choice does. On Android a marker whose start was still running
// (no reason) is checked against Android's record of how that process ended (ApplicationExitInfo, API 30+): only a
// crash or a hang counts; a game closed by the system (low memory, backgrounded, the user swiping it away, an update)
// while Vulkan was starting is no failure, and the marker is dropped.
#include <cstdio>
#include <algorithm>
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
#if defined(__ANDROID__)
#include "android/android_app.h"   // (processExitRecord: how the marker's process ended)
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
    int failures = 1;           // failed Vulkan starts of this version in a row (markers before 2026-11-24: 1)
    int skips = 0;              // Auto starts on OpenGL since the last failure
};

struct Decision {
    bool tryVulkan = false;
    std::string why;            // for the log when Vulkan is not tried ("" when there is nothing worth saying)
    bool retry = false;         // Auto tries Vulkan again after a failure (retryAfterStarts)
    bool skipped = false;       // Auto uses OpenGL because of the marker (the start is counted: decideAt)
};

// Auto starts on OpenGL after `failures` failed Vulkan starts before Auto tries Vulkan again: 3, 6, 12; -1 never (an
// update or choosing Vulkan still does).
inline int retryAfterStarts(int failures) {
    if (failures < 1) failures = 1;
    return failures > 3 ? -1 : 3 << (failures - 1);
}

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

// A version as the marker file keeps it (readMarker trims each line: Sly 1's version string ended in spaces, so its
// own marker never matched and a failed Vulkan start of Sly 1 was never remembered, nor an interrupted one dropped).
inline std::string markerVersion(std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n' || v.back() == '\t')) v.pop_back();
    return v;
}

inline std::string describe(const Marker& m) {
    return m.reason.empty() ? std::string("the game stopped while Vulkan was starting") : m.reason;
}

// The rules (VR follows them too: its Vulkan path goes through the OpenXR runtime). builtWithVulkan: the
// game has the Vulkan renderer (Linux, Windows and Android builds); marker: the start marker read from the user folder;
// version: this build's version.
inline Decision decide(Choice wanted, bool builtWithVulkan, const Marker& marker, const std::string& version,
                       bool autoPrefersVulkan = kAutoPrefersVulkan) {
    Decision d;
    if (wanted == Choice::OpenGL) return d;
    if (!builtWithVulkan) {
        if (wanted == Choice::Vulkan) d.why = "this build has no Vulkan renderer";
        return d;
    }
    if (wanted == Choice::Auto) {
        if (!autoPrefersVulkan) return d;
        // (a marker of another copy that is still starting is not a failure)
        const bool failed = marker.present && marker.version == markerVersion(version) && !(marker.reason.empty() && processRunning(marker.pid));
        if (failed) {
            const int after = retryAfterStarts(marker.failures);
            if (after >= 0 && marker.skips >= after) {
                d.retry = true;
                d.why = "trying Vulkan again (it did not start " + std::to_string(marker.skips + 1) + " starts ago: " + describe(marker) + ")";
                d.tryVulkan = true;
                return d;
            }
            d.skipped = true;
            d.why = "Vulkan did not start last time (" + describe(marker) + "); " +
                    (after >= 0 ? "Auto tries it again after " + std::to_string(after - marker.skips) + " more start" +
                                      (after - marker.skips == 1 ? "" : "s") + " or an update, or choose Vulkan in the settings"
                                : std::string("choose Vulkan in the settings to try it again (or update the game)"));
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
        else if (key == "failures") m.failures = std::max(1, static_cast<int>(std::strtol(value.c_str(), nullptr, 10)));
        else if (key == "skips") m.skips = std::max(0, static_cast<int>(std::strtol(value.c_str(), nullptr, 10)));
    }
    return m;
}

// The failed starts the next marker of this run counts (decideAt: the previous marker's + 1, else 1).
inline int& pendingFailures() {
    static int s_failures = 1;
    return s_failures;
}

// Writes a marker (to a temporary file, renamed, so a crash never leaves half a marker).
inline bool writeMarkerFile(const std::filesystem::path& path, const Marker& m) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "# Vulkan's last start did not finish: Auto uses OpenGL for a few starts, until Vulkan is chosen and starts, or the game is updated.\n"
          << "pid=" << m.pid << "\nversion=" << m.version << "\nreason=" << m.reason << "\nfailures=" << m.failures
          << "\nskips=" << m.skips << "\n";
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

// This process's marker. reason "": a start in progress. failures < 1: pendingFailures().
inline bool writeMarker(const std::filesystem::path& path, const std::string& version, const std::string& reason, int failures = 0) {
    Marker m;
    m.present = true;
    m.pid = currentProcessId();
    m.version = markerVersion(version);
    m.reason = reason;
    m.failures = failures > 0 ? failures : pendingFailures();
    return writeMarkerFile(path, m);
}

inline void clearMarker(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Android: how the process of a marker whose start was still running ended, by Android's record (API 30+). True when
// that is no Vulkan failure (the system or the user closed the game); *why says how it ended. A crash or a hang is
// written into the marker as its reason.
inline bool interruptedStart(const std::filesystem::path& path, Marker& marker, std::string* why) {
#if defined(__ANDROID__)
    if (!marker.present || !marker.reason.empty() || marker.pid <= 0 || marker.pid == currentProcessId()) return false;
    const cane::android::ExitRecord record = cane::android::processExitRecord(marker.pid);
    if (!record.found) return false;
    *why = record.description;
    if (!record.crash) return true;
    marker.reason = "the game crashed while Vulkan was starting (" + record.description + ")";
    writeMarkerFile(path, marker);
    return false;
#else
    (void)path, (void)marker, (void)why;
    return false;
#endif
}

// decide() with the marker file at `path`: reads it (Auto; Vulkan chosen, for its counts), drops an Android start that
// was only interrupted (interruptedStart), counts an Auto start it sends to OpenGL, and sets the failures the next
// marker of this run counts. Logs what it did with "Renderer: " lines.
inline Decision decideAt(Choice wanted, bool builtWithVulkan, const std::filesystem::path& path, const std::string& version,
                         bool autoPrefersVulkan = kAutoPrefersVulkan) {
    Marker marker;
    if (wanted != Choice::OpenGL && builtWithVulkan) marker = readMarker(path);
    const bool mine = marker.present && marker.version == markerVersion(version);
    pendingFailures() = mine ? marker.failures + 1 : 1;
    std::string interrupted;
    if (mine && interruptedStart(path, marker, &interrupted)) {
        std::printf("Renderer: the last Vulkan start did not fail, Android closed the game (%s): Vulkan again\n", interrupted.c_str());
        pendingFailures() = marker.failures;   // (this start counts as the interrupted one)
        clearMarker(path);
        marker = Marker();
    }
    Decision d = decide(wanted, builtWithVulkan, marker, version, autoPrefersVulkan);
    if (d.skipped) {
        ++marker.skips;
        writeMarkerFile(path, marker);
    }
    return d;
}

}  // namespace cane::renderer

#endif  // CANE_PLATFORM_RENDERER_CHOICE_H_
