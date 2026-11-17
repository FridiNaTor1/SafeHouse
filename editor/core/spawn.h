#pragma once
// Starts a program on its own, not waited for (desktop: the editor's play test, the launcher's "Open 3D editor").
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cane::editor {

#ifdef _WIN32
inline std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0; i < a.size(); ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') ++bs, ++i;
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') q.append(bs * 2 + 1, L'\\');
        else q.append(bs, L'\\');
        q += a[i];
    }
    return q + L"\"";
}
#endif

inline bool spawnDetached(const std::filesystem::path& program, const std::vector<std::string>& args) {
#ifdef _WIN32
    std::wstring line = quoteArg(program.wstring());
    for (const auto& a : args) line += L" " + quoteArg(std::filesystem::path(std::u8string(a.begin(), a.end())).wstring());
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION process{};
    const std::wstring dir = program.parent_path().wstring();
    if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    std::vector<std::string> all{program.string()};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    const std::string dir = program.parent_path().string();
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        if (chdir(dir.c_str()) != 0) {
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}

}  // namespace cane::editor
