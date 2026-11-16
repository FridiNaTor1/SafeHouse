#pragma once
// "Play in VR" on Sly 2's page (ui/game_page.h): is a PC VR headset there? Desktop Linux (SteamVR, Monado, WiVRn, the
// Steam Frame) and Windows (SteamVR, Meta Quest Link, Windows Mixed Reality) only: on Android / the Meta Quest the game
// is in VR already, iOS and macOS have no OpenXR runtimes.
//
// The check, off the UI thread and only when the page is shown (GamePage::shown), never every frame:
//  1. the active OpenXR runtime's manifest (cheap, no process): XR_RUNTIME_JSON, else the loader's active_runtime
//     files (Linux: $XDG_CONFIG_HOME, $XDG_CONFIG_DIRS, /etc: openxr/1/active_runtime[.<arch>].json; Windows: the
//     registry's HKLM\SOFTWARE\Khronos\OpenXR\1 ActiveRuntime). None: no button.
//  2. SteamVR registered but not running: the button is shown without asking it (an OpenXR instance would start
//     SteamVR just to answer); the game starts it.
//  3. otherwise the game itself answers in a helper process, `Sly2-Release --vr-check` (Sly2-Release/vr.cpp Check: the
//     game's own loader search, an instance with no extensions, xrGetSystem(HEAD_MOUNTED_DISPLAY)), killed after a
//     few seconds, so a missing, hung or crashing runtime never takes the launcher with it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "platform.h"
#include "process.h"
#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace cane::vrprobe {
// Builds where the button can appear at all.
#if (defined(__linux__) && !defined(__ANDROID__) && !defined(CANE_ANDROID) && !defined(CANE_MOBILE)) || defined(_WIN32)
inline constexpr bool kSupported = true;
#else
inline constexpr bool kSupported = false;
#endif

enum class State { Unknown, None, Ready, SteamVrIdle };

struct Runtime {
    fs::path manifest;
    std::string name;    // the manifest's runtime.name ("SteamVR", "Monado", "WiVRn", "Oculus", ...)
    std::string library; // runtime.library_path
};

namespace detail {
inline std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::optional<Runtime> readManifest(const fs::path& file) {
    std::error_code error;
    if (file.empty() || !fs::is_regular_file(file, error)) return std::nullopt;
    Runtime r;
    r.manifest = file;
    try {
        std::ifstream in(file);
        const auto j = nlohmann::json::parse(in, nullptr, true, true);
        if (j.contains("runtime") && j["runtime"].is_object()) {
            r.name = j["runtime"].value("name", std::string{});
            r.library = j["runtime"].value("library_path", std::string{});
        }
    } catch (...) {
    }
    return r;
}
} // namespace detail

// The runtime the OpenXR loader would pick, or nullopt (no runtime registered).
inline std::optional<Runtime> activeRuntime() {
    if (auto forced = detail::env("XR_RUNTIME_JSON"); !forced.empty()) return detail::readManifest(path(forced));
#if defined(_WIN32)
    // (the 64-bit registry view, where 64-bit runtimes register, also from a 32-bit process)
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return std::nullopt;
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    const LSTATUS status = RegGetValueW(key, nullptr, L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, buffer, &size);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) return detail::readManifest(fs::path(buffer));
    return std::nullopt;
#else
    std::vector<fs::path> roots;
    if (auto config = detail::env("XDG_CONFIG_HOME"); !config.empty()) roots.push_back(path(config));
    else if (auto home = detail::env("HOME"); !home.empty()) roots.push_back(path(home) / ".config");
    std::string dirs = detail::env("XDG_CONFIG_DIRS");
    if (dirs.empty()) dirs = "/etc/xdg";
    for (size_t at = 0; at <= dirs.size();) {
        const size_t colon = std::min(dirs.find(':', at), dirs.size());
        if (colon > at) roots.push_back(path(dirs.substr(at, colon - at)));
        at = colon + 1;
    }
    roots.push_back("/etc");
#if defined(__aarch64__)
    const char* arch = "aarch64";
#elif defined(__x86_64__)
    const char* arch = "x86_64";
#else
    const char* arch = "";
#endif
    for (const auto& root : roots) {
        if (*arch)
            if (auto r = detail::readManifest(root / "openxr" / "1" / (std::string("active_runtime.") + arch + ".json"))) return r;
        if (auto r = detail::readManifest(root / "openxr" / "1" / "active_runtime.json")) return r;
    }
    return std::nullopt;
#endif
}

inline bool isSteamVr(const Runtime& r) {
    const std::string all = detail::lower(r.name + "|" + r.library + "|" + utf8(r.manifest));
    return all.find("steamvr") != std::string::npos;
}

// SteamVR's server is running (then an OpenXR instance only connects to it).
inline bool steamVrRunning() {
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (bool ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        found = _wcsicmp(entry.szExeFile, L"vrserver.exe") == 0;
    CloseHandle(snapshot);
    return found;
#else
    std::error_code error;
    for (fs::directory_iterator it("/proc", error), end; !error && it != end; it.increment(error)) {
        const std::string pid = it->path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream comm(it->path() / "comm");
        std::string name;
        if (std::getline(comm, name) && (name == "vrserver" || name == "vrcompositor")) return true;
    }
    return false;
#endif
}

// "vr-check: ready|<runtime>|<headset>" (Sly2-Release/vr.cpp Check) into state / names.
struct Result {
    State state = State::Unknown;
    std::string runtime, headset;
};
inline Result parseCheckLine(const std::string& line) {
    Result r;
    const std::string prefix = "vr-check: ";
    if (line.rfind(prefix, 0) != 0) return r;
    std::vector<std::string> fields;
    for (size_t at = prefix.size();;) {
        const size_t bar = line.find('|', at);
        fields.push_back(line.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    r.state = fields[0] == "ready" ? State::Ready : State::None;
    if (fields.size() > 1) r.runtime = fields[1];
    if (fields.size() > 2) r.headset = fields[2];
    return r;
}

class Probe {
public:
    ~Probe() { stop(); }
    // Starts a check unless one runs or one ended less than `minInterval` ago. program: the Sly 2 runtime (the
    // backend's binary), directory: the folder it runs in.
    void refresh(const fs::path& program, const fs::path& directory, std::chrono::seconds minInterval = std::chrono::seconds(3)) {
        if constexpr (!kSupported) return;
        if (m_fixture || m_running) return;
        {
            std::lock_guard lock(m_mutex);
            // (a headset found: asked again only after half a minute, so switching windows does not keep making
            // instances on a running runtime)
            const auto interval = m_result.state == State::Ready ? std::max(minInterval, std::chrono::seconds(30)) : minInterval;
            if (m_checked && std::chrono::steady_clock::now() - m_last < interval) return;
        }
        if (m_thread.joinable()) m_thread.join();
        m_running = true;
        m_thread = std::thread([this, program, directory] {
            Result r = check(program, directory);
            std::lock_guard lock(m_mutex);
            m_result = r;
            m_last = std::chrono::steady_clock::now();
            m_checked = true;
            m_running = false;
        });
    }
    Result result() const {
        std::lock_guard lock(m_mutex);
        return m_result;
    }
    bool available() const {
        const State s = result().state;
        return s == State::Ready || s == State::SteamVrIdle;
    }
    // Screenshots (--vr-fixture): a fixed answer, no check.
    void fixture(State state, std::string runtime, std::string headset) {
        m_fixture = true;
        std::lock_guard lock(m_mutex);
        m_result = {state, std::move(runtime), std::move(headset)};
    }
    void stop() {
        {
            std::lock_guard lock(m_waitMutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    Result check(const fs::path& program, const fs::path& directory) {
        Result r;
        const auto runtime = activeRuntime();
        if (!runtime) {
            r.state = State::None;
            return r;
        }
        r.runtime = runtime->name;
        if (isSteamVr(*runtime) && !steamVrRunning()) {
            r.state = State::SteamVrIdle;
            if (r.runtime.empty()) r.runtime = "SteamVR";
            return r;
        }
        std::atomic<bool> cancel{false};
        bool done = false;
        // (the watchdog: a runtime that hangs is killed after the timeout, or when the launcher closes)
        std::thread watchdog([&] {
            std::unique_lock lock(m_waitMutex);
            if (!m_wake.wait_for(lock, std::chrono::seconds(8), [&] { return done || m_stop; })) cancel = true;
            else if (m_stop && !done) cancel = true;
        });
        Result answer;
        answer.state = State::None;
        try {
            runProcess({utf8(program), "--vr-check"}, directory, cancel, [&](const std::string& line) {
                if (line.rfind("vr-check: ", 0) == 0) answer = parseCheckLine(line);
            });
        } catch (...) {
            answer.state = State::None;
        }
        {
            std::lock_guard lock(m_waitMutex);
            done = true;
        }
        m_wake.notify_all();
        watchdog.join();
        if (answer.runtime.empty()) answer.runtime = runtime->name;
        return answer;
    }
    mutable std::mutex m_mutex;
    Result m_result;
    std::mutex m_waitMutex;
    std::condition_variable m_wake;
    bool m_stop = false, m_fixture = false;
    std::atomic<bool> m_running{false};
    bool m_checked = false;
    std::chrono::steady_clock::time_point m_last{};
    std::thread m_thread;
};
} // namespace cane::vrprobe
