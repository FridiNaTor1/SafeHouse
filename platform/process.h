#pragma once
#include <atomic>
#include <cerrno>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "platform.h"
#include "crash_report.h"
#if defined(CANE_ANDROID) || defined(CANE_IOS)
#include "mobile_app.h"
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif
namespace cane {
using LogFunction = std::function<void(const std::string&)>;
#ifdef _WIN32
inline std::wstring quoteWindows(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result += c;
        } else {
            result.append(slashes, L'\\');
            result += c;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#endif
inline int runProcess(const std::vector<std::string>& arguments, const fs::path& directory,
                      const std::atomic<bool>& cancelled, const LogFunction& log) {
    if (arguments.empty()) throw std::runtime_error("Empty process command");
    std::string pending;
    auto consume = [&](const char* bytes, size_t count) {
        pending.append(bytes, count);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            log(line);
            pending.erase(0, end + 1);
        }
        if (pending.size() > 65536) {
            log(pending);
            pending.clear();
        }
    };
    int code = 1;
#if defined(CANE_MOBILE)
    // Android: no fork/exec of app code. The program (lib<name>.so) runs its entry point in a private process of
    // the app (the game activity, the extractor or the tool service: android/.../NativeBridge.java startChild); its
    // stdout/stderr come back through a pipe, as on the desktop. (directory is unused: the programs take paths.)
    // iOS: the program's framework runs in the app's own process (ios/ios_app.h), its output on the same kind of pipe.
    (void)directory;
    const auto child = mobile::startProcess(arguments);
    if (child.inputFd >= 0) close(child.inputFd);
    const int output = child.outputFd;
    if (output < 0) throw std::runtime_error("Cannot create the Android process pipe");
    fcntl(output, F_SETFL, O_NONBLOCK);
    bool eof = false, finished = false, cancelSent = false;
    auto drainDeadline = std::chrono::steady_clock::time_point::max();
    char buffer[8192];
    while (!eof) {
        if (cancelled && !cancelSent) {
            mobile::cancelProcess(child.handle);
            cancelSent = true;
        }
        pollfd fd{output, POLLIN | POLLHUP, 0};
        poll(&fd, 1, 50);
        for (;;) {
            auto n = read(output, buffer, sizeof(buffer));
            if (n > 0) consume(buffer, size_t(n));
            else {
                if (n == 0) eof = true;
                break;
            }
        }
        // (a process killed by Android may leave the pipe open in a lingering binder: stop one second after it ended)
        if (!finished && !mobile::processRunning(child.handle)) {
            finished = true;
            drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (finished && std::chrono::steady_clock::now() > drainDeadline) break;
    }
    close(output);
    while (mobile::processRunning(child.handle) && !cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(20));
#if defined(__ANDROID__)
    // (a child that ended without its exit code: why, from its activity or Android's record: "crash: ..." (CrashWatch)
    // or "exit reason: ..." (explainedProcess shows that, not the output's last error line))
    if (!cancelled)
        if (const std::string why = mobile::processExitReason(child.handle); !why.empty()) log(why);
#endif
    code = mobile::processExitCode(child.handle);
#elif defined(_WIN32)
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    // The default pipe buffer is only a few KiB; a burst of child output
    // would block the game in WriteFile until the next poll below.
    if (!CreatePipe(&reader, &writer, &security, 1 << 20))
        throw std::runtime_error("Cannot create process output pipe");
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    auto nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = writer;
    startup.hStdError = writer;
    std::wstring command;
    for (auto& a : arguments) {
        if (!command.empty()) command += L' ';
        command += quoteWindows(path(a).wstring());
    }
    PROCESS_INFORMATION process{};
    auto job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool ready = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                sizeof(limits));
    bool created = ready && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                           directory.empty() ? nullptr : directory.c_str(),
                                           &startup, &process);
    CloseHandle(writer);
    CloseHandle(nullInput);
    if (!created) {
        CloseHandle(reader);
        if (job) CloseHandle(job);
        throw std::runtime_error("Cannot start " + arguments[0] + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        CloseHandle(reader);
        throw std::runtime_error("Cannot supervise process");
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    bool terminated = false;
    char buffer[8192];
    for (;;) {
        if (cancelled && !terminated) {
            TerminateJobObject(job, 1);
            terminated = true;
        }
        DWORD available = 0;
        if (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD n = 0;
            if (ReadFile(reader, buffer, std::min<DWORD>(available, sizeof(buffer)), &n, nullptr) &&
                n)
                consume(buffer, n);
            continue;
        }
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD result = 1;
            GetExitCodeProcess(process.hProcess, &result);
            code = static_cast<int>(result);
            break;
        }
    }
    // Closing the job also stops descendants that outlive the root process,
    // so they cannot keep the output pipe open indefinitely.
    CloseHandle(job);
    DWORD n = 0;
    while (ReadFile(reader, buffer, sizeof(buffer), &n, nullptr) && n) consume(buffer, n);
    CloseHandle(process.hProcess);
    CloseHandle(reader);
#else
    int descriptors[2];
    // Close-on-exec keeps unrelated children (e.g. the file manager opened by
    // "Log folder") from inheriting the pipe; dup2 in the child clears it.
#ifdef __linux__
    if (pipe2(descriptors, O_CLOEXEC) != 0) throw std::runtime_error("Cannot create process pipe");
#else
    if (pipe(descriptors) != 0) throw std::runtime_error("Cannot create process pipe");
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
#endif
    // Prepare argv before fork: the launcher has other threads.
    std::vector<char*> argv;
    for (auto& a : arguments) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const std::string cwd = utf8(directory);
    const pid_t parentPid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw std::runtime_error("Cannot create process");
    }
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parentPid) _exit(125);
#endif
        setpgid(0, 0);
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0) {
            dup2(input, STDIN_FILENO);
            close(input);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execvp(argv[0], argv.data());
        const char error[] = "Could not execute requested program\n";
        write(STDERR_FILENO, error, sizeof(error) - 1);
        _exit(127);
    }
    close(descriptors[1]);
    setpgid(pid, pid);
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    bool exited = false, eof = false, terminated = false;
    int status = 0;
    auto deadline = std::chrono::steady_clock::time_point::max();
    char buffer[8192];
    while (!exited || !eof) {
        if (cancelled && !terminated) {
            kill(-pid, SIGTERM);
            terminated = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (terminated && !eof && std::chrono::steady_clock::now() > deadline) kill(-pid, SIGKILL);
        pollfd fd{descriptors[0], POLLIN | POLLHUP, 0};
        poll(&fd, 1, 50);
        for (;;) {
            auto n = read(descriptors[0], buffer, sizeof(buffer));
            if (n > 0)
                consume(buffer, n);
            else {
                if (n == 0) eof = true;
                break;
            }
        }
        if (!exited) {
            auto result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
                exited = true;
            else if (result < 0 && errno != EINTR) {
                exited = true;
                status = 1 << 8;
            }
        }
    }
    close(descriptors[0]);
    code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
    if (!pending.empty()) log(pending);
    if (cancelled) throw std::runtime_error("Cancelled");
    return code;
}
// A child's exit code as the launcher's messages show it: "exit 1", "exit 0xC0000005, access violation" (Windows: an
// NTSTATUS or exception code, crash_report.h exceptionName), "exit 139, segmentation fault" (runProcess: 128 + signal).
inline std::string describeExitCode(int code) {
#ifdef _WIN32
    const auto value = static_cast<uint32_t>(code);
    if (value >= 0x40000000u) {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        const char* name = crash::exceptionName(value);
        return std::string("exit ") + text + (name[0] ? std::string(", ") + name : std::string());
    }
#else
    static const std::pair<int, const char*> signals[] = {{4, "illegal instruction"}, {6, "abort"},        {7, "bus error"},
                                                          {8, "arithmetic exception"}, {9, "killed"},    {11, "segmentation fault"},
                                                          {15, "terminated"}};
    for (const auto& [number, name] : signals)
        if (code == 128 + number) return "exit " + std::to_string(code) + ", " + name;
#endif
    return "exit " + std::to_string(code);
}
// The exit code of a crash (not an ordinary failure the program reported): Windows' error codes, a fatal signal.
inline bool crashExitCode(int code) {
#ifdef _WIN32
    return static_cast<uint32_t>(code) >= 0xC0000000u || static_cast<uint32_t>(code) == 0x80000003u;
#else
    return code == 128 + 4 || code == 128 + 6 || code == 128 + 7 || code == 128 + 8 || code == 128 + 11;
#endif
}
// A program that failed: `crashed` when it crashed or hung (its "crash:" / "hang:" report, or a crash's exit code), with
// its report file (crash_report.h: the minidump in the log folder) when there is one.
struct ProcessFailure : std::runtime_error {
    bool crashed = false;
    std::string report;
    ProcessFailure(const std::string& message, bool crashed, std::string report = {})
        : std::runtime_error(message), crashed(crashed), report(std::move(report)) {}
};
// Watches a program's output for its crash report (crash_report.h): the "crash: ... crashed: ..." / "hang: ... is not
// responding ..." line and the minidump's path. feed() is false for the report's own lines.
struct CrashWatch {
    std::string crash, hang, report;
    bool feed(const std::string& line) {
        const bool crashLine = line.rfind("crash: ", 0) == 0, hangLine = line.rfind("hang: ", 0) == 0;
        if (!crashLine && !hangLine) return true;
        const std::string text = line.substr(crashLine ? 7 : 6);
        if (crashLine && crash.empty() && text.find(" crashed: ") != std::string::npos) crash = text;
        if (hangLine && hang.empty() && text.find(" is not responding") != std::string::npos) hang = text;
        if (text.rfind("minidump: ", 0) == 0 && text.find("could not") == std::string::npos) report = text.substr(10);
        return false;
    }
    // The failure of a program that ended with `code` (not 0): the crash's message, or none (an ordinary failure).
    std::optional<ProcessFailure> failure(const std::string& program, int code) const {
        const std::string exit = " (" + program + ", " + describeExitCode(code) + ")";
        if (!crash.empty()) return ProcessFailure(crash + exit, true, report);
        if (!hang.empty() && !crashExitCode(code)) return std::nullopt;
        if (!hang.empty()) return ProcessFailure(hang + exit, true, report);
        if (crashExitCode(code)) return ProcessFailure(program + " ended unexpectedly" + exit, true, report);
        return std::nullopt;
    }
};
inline void checkedProcess(const std::vector<std::string>& arguments, const fs::path& directory,
                           const std::atomic<bool>& cancelled, const LogFunction& log) {
    CrashWatch watch;
    auto code = runProcess(arguments, directory, cancelled, [&](const std::string& line) {
        watch.feed(line);
        log(line);
    });
    if (!code) return;
    if (auto failure = watch.failure(utf8(path(arguments[0]).filename()), code)) throw *failure;
    throw ProcessFailure(arguments[0] + " failed (" + describeExitCode(code) + "). Details are in the launcher output.", false);
}
inline void openFolder(const fs::path& directory) {
#if defined(CANE_MOBILE)
    mobile::openFolder(utf8(directory));
#elif defined(_WIN32)
    auto result = reinterpret_cast<intptr_t>(
        ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) throw std::runtime_error("Could not open folder");
#else
    auto pid = fork();
    if (pid == 0) {
        auto second = fork();
        if (second > 0) _exit(0);
        if (second < 0) _exit(1);
        setsid();
        auto text = utf8(directory);
#if defined(__APPLE__)
        execlp("open", "open", text.c_str(), nullptr);
#else
        execlp("xdg-open", "xdg-open", text.c_str(), nullptr);
#endif
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
#endif
}
// Opens a web link in the system browser (ShellExecute / open / xdg-open), detached from the launcher.
inline void openUrl(const std::string& url) {
#if defined(CANE_MOBILE)
    mobile::openUrl(url);
#elif defined(_WIN32)
    const std::wstring wide(url.begin(), url.end()); // the links are ASCII (brand.h)
    auto result = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) throw std::runtime_error("Could not open the browser");
#else
    auto pid = fork();
    if (pid == 0) {
        auto second = fork();
        if (second > 0) _exit(0);
        if (second < 0) _exit(1);
        setsid();
#if defined(__APPLE__)
        execlp("open", "open", url.c_str(), nullptr);
#else
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
#endif
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
#endif
}
}  // namespace cane
