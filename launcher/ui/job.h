#pragma once
// Background work (installs, checks, source builds, a running game) and the
// opt-in session log. Logging is off by default: when it is on, launcher and
// game output goes to logs/launcher.log for bug reports.
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "../backend.h"

namespace cane::ui {
inline std::string timestamp(const char* format) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), format, &local);
    return text;
}
// Writes launcher and game output to logs/launcher.log while logging is
// enabled. Each launcher session starts a fresh file and keeps the previous
// session as launcher.previous.log, so logs never grow without bound.
class SessionLog {
public:
    static constexpr std::uintmax_t limit = 16 * 1024 * 1024;
    explicit SessionLog(fs::path directory) : directory(std::move(directory)) {
#ifdef _WIN32
        home = utf8(environmentDirectory("USERPROFILE", {}));
#else
        home = utf8(environmentDirectory("HOME", {}));
#endif
        if (home.size() < 4) home.clear();
    }
    void write(std::string line) {
        std::lock_guard lock(mutex);
        if (full || (!output.is_open() && !open())) return;
        written += line.size() + 12;
        output << '[' << timestamp("%H:%M:%S") << "] " << redact(std::move(line)) << '\n';
        output.flush();
        if (written > limit) {
            output << "[log size limit reached; further output is not saved]\n";
            output.close();
            full = true;
        }
    }

private:
    // Logs are meant to be shared, so do not reveal the account's home path.
    std::string redact(std::string text) const {
        for (size_t at = 0; !home.empty() && (at = text.find(home, at)) != std::string::npos;)
            text.replace(at, home.size(), "~"), ++at;
        return text;
    }
    bool open() {
        std::error_code ec;
        fs::create_directories(directory, ec);
        auto current = directory / "launcher.log";
        if (!rotated) {
            rotated = true;
            if (fs::exists(current, ec))
                fs::rename(current, directory / "launcher.previous.log", ec);
        }
        output.open(current, std::ios::app);
        if (!output) return false;
#ifdef _WIN32
        const char* platform = "Windows";
#elif defined(__linux__)
        const char* platform = "Linux";
#elif defined(CANE_IOS)
        const char* platform = "iOS";
#elif defined(__APPLE__)
        const char* platform = "macOS";
#else
        const char* platform = "Unknown";
#endif
        output << "SafeHouse launcher log, session started "
               << timestamp("%Y-%m-%d %H:%M:%S %z") << " (" << platform << ", "
               << redact(utf8(executableDirectory())) << ")\n";
        return true;
    }
    fs::path directory;
    std::string home;
    std::mutex mutex;
    std::ofstream output;
    std::uintmax_t written = 0;
    bool rotated = false, full = false;
};
struct Job {
    std::atomic<bool> busy{false}, cancel{false};
    std::thread worker;
    std::mutex mutex;
    std::deque<std::string> lines;
    std::string message = "Ready", stage, item, label;
    float progress = 0;
    // Set once by main.cpp; output is saved only while *logging is true.
    SessionLog* sessionLog = nullptr;
    const std::atomic<bool>* logging = nullptr;
    void start(const std::string& title, std::function<void(const LogFunction&)> task) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        cancel = false;
        {
            std::lock_guard lock(mutex);
            lines.clear();
            message = title;
            label = title;
            stage.clear();
            item.clear();
            progress = 0;
        }
        busy = true;
        worker = std::thread([this, title, task] {
            std::string loggedStage;
            auto save = [&](const std::string& line) {
                if (sessionLog && logging && *logging) sessionLog->write(line);
            };
            try {
                save("== " + title);
                auto log = [&](const std::string& line) {
                    bool progressEvent = false;
                    // Progress is JSON lines; plain output must not pay for a
                    // parse exception on every line.
                    if (!line.empty() && line.front() == '{') {
                        auto event = json::parse(line, nullptr, false);
                        if (event.is_object() && event.contains("stage") && event.at("stage").is_string()) {
                            std::lock_guard lock(mutex);
                            stage = event.at("stage");
                            item = event.value("item", std::string{});
                            auto total = event.value("total", 0.0f);
                            auto completed = event.value("completed", 0.0f);
                            progress = total > 0 ? completed / total : 0;
                            // Keep the saved log readable: record stage changes
                            // and completion, not every file of a verify pass.
                            progressEvent = stage == loggedStage && completed < total;
                            loggedStage = stage;
                        }
                    }
                    if (!progressEvent) save(line);
                    std::lock_guard lock(mutex);
                    lines.push_back(line);
                    if (lines.size() > 300) lines.pop_front();
                };
                task(log);
                save("== Completed: " + title);
                {
                    std::lock_guard lock(mutex);
                    message = "Completed";
                    progress = 1;
                }
            } catch (const std::exception& e) {
                save(std::string("== Failed: ") + e.what());
                std::lock_guard lock(mutex);
                message = cancel ? "Cancelled" : e.what();
                lines.push_back(e.what());
            }
            busy = false;
        });
    }
    // Joins a finished worker; call once per frame.
    void reap() {
        if (!busy && worker.joinable()) worker.join();
    }
    ~Job() {
        cancel = true;
        if (worker.joinable()) worker.join();
    }
};
inline std::string stageLabel(const std::string& stage) {
    static const std::unordered_map<std::string, std::string> labels = {
        {"decompress", N_("Decompressing the CHD")}, {"validate", N_("Checking your disc")}, {"levels", N_("Preparing worlds")},
        {"music", N_("Preparing music")},           {"sound-banks", N_("Preparing sound effects")},
        {"commentary", N_("Preparing commentary")}, {"dialogue", N_("Preparing dialogue")},
        {"ambient", N_("Preparing ambient audio")}, {"cutscenes", N_("Preparing movies")},
        {"copy", N_("Copying the disc image")},     {"extract", N_("Extracting system files")},
        {"convert", N_("Converting the game files")}, {"inventory", N_("Listing installed files")},
        {"textures", N_("Exporting textures")},
        {"ps3-prepare", N_("Reading your installed textures")}, {"ps3-read", N_("Reading The Sly Collection (PS3)")},
        {"ps3-textures", N_("Matching the PS3 HD textures")},  {"ps3-done", N_("PS3 HD textures installed")},
        {"languages", N_("Sharing the languages")},          {"ps3-languages", N_("Reading The Sly Collection's languages")},
        {"verify", N_("Checking installed files")}, {"move", N_("Moving the installation")}, {"moved", N_("Installation moved")}, {"complete", N_("Ready to play")},
        {"error", N_("Installation failed")}};
    auto found = labels.find(stage);
    return found == labels.end() ? stage : std::string(tr(found->second.c_str()));
}
} // namespace cane::ui
