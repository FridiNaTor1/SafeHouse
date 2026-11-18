#pragma once
// Files the installers write and the games read back on every platform, made safe for Windows:
//   sanitizeName   a file / folder name Windows takes (no <>:"/\|?*, control characters, trailing dots / spaces or
//                  device names like CON, AUX, NUL, COM1, LPT1). The writer records the name it used (the manifest,
//                  a world's index), so the reader finds the same file.
//   ioPath         the path to hand to the OS: on Windows an absolute path near or over MAX_PATH (260) gets the \\?\
//                  prefix (no MAX_PATH limit, also without the system's LongPathsEnabled), elsewhere unchanged.
//   createDirectories / writeFile
//                  race-free folder creation (worker threads making the same folders at once) and a write that retries
//                  the short-lived failures a virus scanner, a sync client (OneDrive) or the search indexer cause
//                  (sharing violation, access denied right after the create), then fails with the full path, its
//                  length and the system's own error text.
//   pathUnits      a path's length as Windows counts it (UTF-16 units).
//   longPathsEnabled
//                  Windows: this process may use paths over MAX_PATH without \\?\ (the manifest's longPathAware and the
//                  system's LongPathsEnabled; RtlAreLongPathsEnabled). Elsewhere always true.
// pathsSelfTest() checks the pure parts on every platform (Sly2-Release --paths-selftest).
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <algorithm>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cane::paths {
namespace fs = std::filesystem;

// The longest file path Windows takes without long paths (MAX_PATH less the terminating NUL), and a folder's (which must
// leave room for an 8.3 file name: CreateDirectoryW's limit).
inline constexpr std::size_t kMaxFile = 259, kMaxDir = 247;

// ---- names
inline bool reservedDeviceName(const std::string& name) {
    // (Windows takes the device name with any extension, and with trailing spaces before the dot, as the device)
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    std::string u;
    for (char c : stem) u += (c >= 'a' && c <= 'z') ? char(c - 32) : c;
    if (u == "CON" || u == "PRN" || u == "AUX" || u == "NUL" || u == "CONIN$" || u == "CONOUT$") return true;
    // COM1..9 / LPT1..9, also the superscript digits Windows counts (UTF-8 C2 B9 / C2 B2 / C2 B3)
    if (u.size() >= 4 && (u.compare(0, 3, "COM") == 0 || u.compare(0, 3, "LPT") == 0)) {
        const std::string d = u.substr(3);
        if (d.size() == 1 && d[0] >= '1' && d[0] <= '9') return true;
        if (d == "\xC2\xB9" || d == "\xC2\xB2" || d == "\xC2\xB3") return true;
    }
    return false;
}

// A file or folder name (one path component) every platform's file system takes; UTF-8 kept as it is.
inline std::string sanitizeName(std::string s) {
    for (char& c : s)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (s.empty()) return "_";
    if (reservedDeviceName(s)) s.insert(s.find('.') == std::string::npos ? s.size() : s.find('.'), "_");
    return s;
}

// ---- lengths
// UTF-8 text's length in UTF-16 units (what Windows' path limits count).
inline std::size_t utf16Units(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte
        n += c >= 0xF0 ? 2 : 1;             // (4-byte sequences: a surrogate pair)
    }
    return n;
}
inline std::size_t pathUnits(const fs::path& p) {
#ifdef _WIN32
    return p.native().size();
#else
    const auto u = p.u8string();
    return utf16Units(std::string(u.begin(), u.end()));
#endif
}

// ---- the \\?\ prefix (pure: tested on every platform)
// `absolute`: an absolute Windows path ("C:\a\b", "C:/a/b", "\\server\share\a"). Returns it with the \\?\ prefix
// ("\\?\C:\a\b", "\\?\UNC\server\share\a"), separators made backslashes and "." / ".." resolved, which the prefix
// otherwise leaves to the file system; an already prefixed path ("\\?\", "\\.\") and anything else unchanged.
template <class S>
S longPathPrefixed(const S& absolute) {
    using C = typename S::value_type;
    S p = absolute;
    for (auto& c : p)
        if (c == C('/')) c = C('\\');
    if (p.size() >= 4 && p[0] == C('\\') && p[1] == C('\\') && (p[2] == C('?') || p[2] == C('.')) && p[3] == C('\\')) return absolute;
    S head, rest;
    if (p.size() >= 3 && p[1] == C(':') && p[2] == C('\\')) {
        head = {C('\\'), C('\\'), C('?'), C('\\'), p[0], C(':')};
        rest = p.substr(2);
    } else if (p.size() >= 3 && p[0] == C('\\') && p[1] == C('\\') && p[2] != C('\\')) {
        head = {C('\\'), C('\\'), C('?'), C('\\'), C('U'), C('N'), C('C')};
        rest = p.substr(1);
    } else
        return absolute;
    // resolve "." and ".." (never above the root), drop doubled separators
    std::vector<S> parts;
    std::size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && rest[i] == C('\\')) ++i;
        std::size_t j = i;
        while (j < rest.size() && rest[j] != C('\\')) ++j;
        S part = rest.substr(i, j - i);
        i = j;
        if (part.empty() || part == S(1, C('.'))) continue;
        if (part == S(2, C('.'))) {
            if (!parts.empty() && !(head.back() == C('C') && parts.size() <= 2)) parts.pop_back();   // (UNC: not above \\server\share)
            continue;
        }
        parts.push_back(part);
    }
    S out = head;
    for (const S& part : parts) {
        out += C('\\');
        out += part;
    }
    if (parts.empty()) out += C('\\');
    return out;
}

// The folders CreateDirectoryW must make, root first, for the absolute Windows folder `dir` ("C:\a\b" -> "C:\a",
// "C:\a\b"); the drive, the \\server\share or the \\?\ head is never in it. Without \\?\ Windows takes "/" as a
// separator too (a relative path joined with "/", "Worlds/1-Paris", stays so in the native string), so it is made "\"
// first: a chain split at "\" only would ask for "...\Worlds/1-Paris" before "...\Worlds" exists, and Windows answers
// ERROR_PATH_NOT_FOUND. (With \\?\ "/" is a literal character; longPathPrefixed already made it "\".)
template <class S>
std::vector<S> directoryChain(const S& dir) {
    using C = typename S::value_type;
    S w = dir;
    const bool prefixed = w.size() >= 4 && w[0] == C('\\') && w[1] == C('\\') && w[2] == C('?') && w[3] == C('\\');
    if (!prefixed)
        for (auto& c : w)
            if (c == C('/')) c = C('\\');
    while (w.size() > 1 && w.back() == C('\\')) w.pop_back();
    auto find = [&](std::size_t from) { const auto at = w.find(C('\\'), from); return at == S::npos ? w.size() : at; };
    auto starts = [&](const char* head) {
        std::size_t k = 0;
        for (; head[k]; ++k)
            if (k >= w.size() || w[k] != C(head[k])) return false;
        return true;
    };
    std::size_t start;   // (the end of the head: the separator after it, or the end)
    if (starts("\\\\?\\UNC\\")) start = find(find(8) + 1);
    else if (prefixed) start = find(4);                                      // \\?\C:
    else if (w.size() >= 2 && w[1] == C(':')) start = 2;                     // C:
    else if (starts("\\\\")) start = find(find(2) + 1);                    // \\server\share
    else start = 0;                                                         // (relative: from the first name)
    std::vector<S> out;
    for (std::size_t i = start + 1; i <= w.size(); ++i) {
        if (i < w.size() && w[i] != C('\\')) continue;
        if (i > start + 1 && w[i - 1] != C('\\')) out.push_back(w.substr(0, i));
    }
    return out;
}

// The path to open / create / list with: on Windows an absolute one that may be too long for MAX_PATH (or a relative one
// that becomes so) with \\?\; short paths unchanged, so nothing changes where the limit is far away. Elsewhere `p`.
inline fs::path ioPath(const fs::path& p) {
#ifdef _WIN32
    if (p.native().size() < 200 && p.is_absolute()) return p;
    std::error_code ec;
    const fs::path a = p.is_absolute() ? p : fs::absolute(p, ec);
    if (ec || a.native().size() < 200) return p;
    return fs::path(longPathPrefixed(a.native()));
#else
    return p;
#endif
}

// The root of a recursive operation (remove_all, recursive_directory_iterator, copy): on Windows always \\?\ (when absolute
// or made so), since the files below it can be over MAX_PATH even when the root is short. Elsewhere `p`.
inline fs::path longPath(const fs::path& p) {
#ifdef _WIN32
    std::error_code ec;
    const fs::path a = p.is_absolute() ? p : fs::absolute(p, ec);
    if (ec) return p;
    return fs::path(longPathPrefixed(a.native()));
#else
    return p;
#endif
}

inline bool longPathsEnabled() {
#ifdef _WIN32
    using PFN = BOOLEAN(WINAPI*)();
    static const bool s_f = [] {
        const HMODULE h = GetModuleHandleW(L"ntdll.dll");
        const auto pfn = h ? reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(h, "RtlAreLongPathsEnabled"))) : nullptr;
        return pfn && pfn() != 0;
    }();
    return s_f;
#else
    return true;
#endif
}

// ---- errors
enum class Failure { None, TooLong, AccessDenied, InUse, NoSpace, Other };

struct OsError {
    int code = 0;
    std::string text;
    Failure kind = Failure::None;
};

inline OsError lastOsError() {
    OsError e;
#ifdef _WIN32
    const DWORD code = GetLastError();
    e.code = int(code);
    wchar_t* pw = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&pw), 0, nullptr);
    if (n && pw) {
        std::wstring w(pw, n);
        LocalFree(pw);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
        const auto u = fs::path(w).u8string();
        e.text.assign(u.begin(), u.end());
    }
    if (e.text.empty()) e.text = "error " + std::to_string(code);
    e.text += " (Windows error " + std::to_string(code) + ")";
    switch (code) {
        case ERROR_FILENAME_EXCED_RANGE:
            e.kind = Failure::TooLong;
            break;
        // (ERROR_PATH_NOT_FOUND is not "too long" by itself: a missing or malformed folder says the same. describeFailure
        // calls it too long only when the path really is over MAX_PATH and the system has no long paths.)
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case ERROR_VIRUS_INFECTED:
        case ERROR_VIRUS_DELETED:
            e.kind = Failure::AccessDenied;
            break;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
            e.kind = Failure::InUse;
            break;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            e.kind = Failure::NoSpace;
            break;
        default:
            e.kind = Failure::Other;
    }
#else
    const int code = errno;
    e.code = code;
    e.text = std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ")";
    e.kind = code == ENAMETOOLONG ? Failure::TooLong
             : code == EACCES || code == EPERM || code == EROFS ? Failure::AccessDenied
             : code == EBUSY || code == ETXTBSY ? Failure::InUse
             : code == ENOSPC || code == EDQUOT ? Failure::NoSpace
                                                : Failure::Other;
#endif
    return e;
}

// The tag a failure's message carries, which the launcher turns into its hint (launcher/backend.h installHint).
inline const char* failureTag(Failure kind) {
    switch (kind) {
        case Failure::TooLong: return "[path-too-long]";
        case Failure::AccessDenied: return "[access-denied]";
        case Failure::InUse: return "[file-in-use]";
        case Failure::NoSpace: return "[disk-full]";
        default: return "";
    }
}

inline std::string utf8Of(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

// A failure's kind given the path's length (UTF-16 units, without a \\?\ head) and whether the system takes long paths:
// an unclassified error (a missing folder: ERROR_PATH_NOT_FOUND) on a path over MAX_PATH without long paths is the
// length; a short path, or a system with long paths, keeps the OS's own error. (Pure: tested on every platform.)
inline Failure classifyFailure(Failure kind, std::size_t units, bool longPathsOn) {
    if (kind == Failure::Other && units > kMaxFile && !longPathsOn) return Failure::TooLong;
    return kind;
}

// "<path> (N characters): <OS text> [tag]"; a path over the limit on a system without long paths is called too long
// when the OS gave no clearer reason (classifyFailure).
inline std::string describeFailure(const fs::path& p, OsError e) {
    std::error_code ec;
    const fs::path a = p.is_absolute() ? p : fs::absolute(p, ec);
    std::size_t n = pathUnits(a);
#ifdef _WIN32
    if (a.native().rfind(L"\\\\?\\UNC\\", 0) == 0) n -= 6;   // (counted as \\server\share)
    else if (a.native().rfind(L"\\\\?\\", 0) == 0) n -= 4;
    e.kind = classifyFailure(e.kind, n, longPathsEnabled());
#endif
    std::string s = utf8Of(a) + " (" + std::to_string(n) + " characters): " + e.text;
    if (*failureTag(e.kind)) s += std::string(" ") + failureTag(e.kind);
    return s;
}

// ---- folders and files
// Makes `dir` and its parents. Race-free: a folder another thread made meanwhile is fine. false: `error` says why.
inline bool createDirectories(const fs::path& dir, std::string* error = nullptr) {
    if (dir.empty()) return true;
#ifdef _WIN32
    const fs::path a = ioPath(dir.is_absolute() ? dir : fs::absolute(dir));
    const std::wstring& w = a.native();
    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    // each folder from the root (the drive / share / \\?\ head is never created; "/" taken as "\": directoryChain)
    for (const std::wstring& part : directoryChain(w)) {
        for (int attempt = 0;; ++attempt) {
            if (CreateDirectoryW(part.c_str(), nullptr)) break;
            const DWORD code = GetLastError();
            if (code == ERROR_ALREADY_EXISTS) {
                const DWORD at = GetFileAttributesW(part.c_str());
                if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) break;
            }
            if ((code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) && attempt < 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50 << attempt));
                continue;
            }
            SetLastError(code);
            if (error) *error = "cannot make the folder " + describeFailure(fs::path(part), lastOsError());
            return false;
        }
    }
    return true;
#else
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (fs::is_directory(dir)) return true;   // (made here or by another thread meanwhile)
    errno = ec.value();
    if (error) *error = "cannot make the folder " + describeFailure(dir, lastOsError());
    return false;
#endif
}

// Writes the file (its folders made first). Retries the short-lived failures (in use, access denied) for about 3 s.
// false: `error` has the full path, its length and the system's error text (describeFailure).
inline bool writeFile(const fs::path& p, const void* data, std::size_t size, std::string* error = nullptr) {
    if (!createDirectories(p.parent_path(), error)) return false;
    const fs::path io = ioPath(p);
    for (int attempt = 0;; ++attempt) {
        bool ok = false;
#ifdef _WIN32
        const HANDLE h = CreateFileW(io.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ok = true;
            const char* pb = static_cast<const char*>(data);
            for (std::size_t done = 0; ok && done < size;) {
                DWORD n = 0;
                const DWORD chunk = DWORD(std::min<std::size_t>(size - done, 1u << 30));
                ok = WriteFile(h, pb + done, chunk, &n, nullptr) && n > 0;
                done += n;
            }
            const DWORD code = GetLastError();
            if (!CloseHandle(h)) ok = false;
            if (!ok) SetLastError(code ? code : GetLastError());
        }
#else
        if (std::FILE* f = std::fopen(io.c_str(), "wb")) {
            ok = (size == 0 || std::fwrite(data, 1, size, f) == size);
            const int code = errno;
            if (std::fclose(f) != 0) ok = false;
            else if (!ok) errno = code;
        }
#endif
        if (ok) return true;
        OsError e = lastOsError();
        if ((e.kind == Failure::InUse || e.kind == Failure::AccessDenied) && attempt < 6) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50 << attempt));   // 50 ms .. 1.6 s
            continue;
        }
        if (error) *error = describeFailure(p, e);
        return false;
    }
}

// ---- self test (the pure parts)
inline int pathsSelfTest() {
    int failed = 0;
    auto check = [&](bool f, const std::string& what) {
        if (!f) {
            ++failed;
            std::printf("paths-selftest: FAILED %s\n", what.c_str());
        }
    };
    auto eqs = [&](const std::string& got, const std::string& want, const std::string& what) {
        check(got == want, what + ": \"" + got + "\", wanted \"" + want + "\"");
    };
    eqs(sanitizeName("Cairo Museum"), "Cairo Museum", "plain name");
    eqs(sanitizeName("Menace in the North, Eh?"), "Menace in the North, Eh_", "question mark");
    eqs(sanitizeName("a:b/c\\d|e*f\"g<h>i"), "a_b_c_d_e_f_g_h_i", "reserved characters");
    eqs(sanitizeName("trailing. . "), "trailing", "trailing dots and spaces");
    eqs(sanitizeName("..."), "_", "only dots");
    eqs(sanitizeName(""), "_", "empty");
    eqs(sanitizeName("CON"), "CON_", "CON");
    eqs(sanitizeName("con.txt"), "con_.txt", "con.txt");
    eqs(sanitizeName("Aux"), "Aux_", "Aux");
    eqs(sanitizeName("NUL.tar.gz"), "NUL_.tar.gz", "NUL with extensions");
    eqs(sanitizeName("COM1"), "COM1_", "COM1");
    eqs(sanitizeName("lpt9.bin"), "lpt9_.bin", "lpt9.bin");
    eqs(sanitizeName("COM0"), "COM0", "COM0 is no device");
    eqs(sanitizeName("CONSOLE"), "CONSOLE", "CONSOLE is no device");
    eqs(sanitizeName("AUX .bin"), "AUX _.bin", "AUX with a space before the dot");
    eqs(sanitizeName("\xED\x8C\x8C\xEB\xA6\xAC"), "\xED\x8C\x8C\xEB\xA6\xAC", "UTF-8 (Korean) kept");
    eqs(sanitizeName("tab\there"), "tab_here", "control character");
    check(sanitizeName(sanitizeName("CON.")) == sanitizeName("CON."), "sanitizeName is idempotent");
    check(utf16Units("abc") == 3, "utf16Units ASCII");
    check(utf16Units("\xED\x8C\x8C\xEB\xA6\xAC") == 2, "utf16Units Korean (3-byte UTF-8 = 1 unit)");
    check(utf16Units("Zo\xC3\xAB") == 3, "utf16Units Latin-1");
    check(utf16Units("\xF0\x9F\x98\x80") == 2, "utf16Units emoji (surrogate pair)");
    eqs(longPathPrefixed(std::string("C:\\Users\\a\\b.bin")), "\\\\?\\C:\\Users\\a\\b.bin", "drive path");
    eqs(longPathPrefixed(std::string("C:/Games/a/b.bin")), "\\\\?\\C:\\Games\\a\\b.bin", "forward slashes");
    eqs(longPathPrefixed(std::string("C:\\a\\.\\b\\..\\c\\\\d")), "\\\\?\\C:\\a\\c\\d", "dot, dot-dot, doubled separators");
    eqs(longPathPrefixed(std::string("C:\\..\\a")), "\\\\?\\C:\\a", "dot-dot at the root");
    eqs(longPathPrefixed(std::string("C:\\")), "\\\\?\\C:\\", "drive root");
    eqs(longPathPrefixed(std::string("\\\\server\\share\\x\\y")), "\\\\?\\UNC\\server\\share\\x\\y", "UNC");
    eqs(longPathPrefixed(std::string("\\\\server\\share\\..\\..\\y")), "\\\\?\\UNC\\server\\share\\y", "UNC dot-dot kept below the share");
    eqs(longPathPrefixed(std::string("\\\\?\\C:\\a")), "\\\\?\\C:\\a", "already prefixed");
    eqs(longPathPrefixed(std::string("\\\\.\\COM1")), "\\\\.\\COM1", "device namespace unchanged");
    eqs(longPathPrefixed(std::string("relative\\a")), "relative\\a", "relative unchanged");
    check(longPathPrefixed(std::wstring(L"D:\\x")) == L"\\\\?\\D:\\x", "wide strings");
    {
        std::string deep = "C:\\";
        while (deep.size() < 400) deep += "a long folder name\\";
        deep += "Graphics.bin";
        const std::string pre = longPathPrefixed(deep);
        check(pre.rfind("\\\\?\\C:\\", 0) == 0 && pre.size() == deep.size() + 4, "deep path prefixed whole");
    }
    // directoryChain: the folders CreateDirectoryW makes (the user's report 2026-11-18: the folder
    // "C:\Games\SafeHouse\sly2.staging\Movies/English" was asked for whole, before Movies existed: Windows error 3, called "too long" at 46 characters)
    {
        auto chain = [](const std::string& d) {
            std::string s;
            for (const std::string& c : directoryChain(d)) s += (s.empty() ? "" : " | ") + c;
            return s;
        };
        eqs(chain("C:\\Games\\SafeHouse\\sly2.staging\\Movies/English"),
            "C:\\Games | C:\\Games\\SafeHouse | C:\\Games\\SafeHouse\\sly2.staging | C:\\Games\\SafeHouse\\sly2.staging\\Movies | "
            "C:\\Games\\SafeHouse\\sly2.staging\\Movies\\English",
            "chain: forward slashes are separators");
        eqs(chain("C:/a/b/"), "C:\\a | C:\\a\\b", "chain: forward slashes only, trailing separator");
        eqs(chain("C:\\a\\\\b"), "C:\\a | C:\\a\\\\b", "chain: a doubled separator makes no empty folder");
        eqs(chain("C:\\"), "", "chain: the drive root makes nothing");
        eqs(chain("C:"), "", "chain: a bare drive makes nothing");
        eqs(chain("D:\\x"), "D:\\x", "chain: another drive");
        eqs(chain("\\\\?\\C:\\a\\b"), "\\\\?\\C:\\a | \\\\?\\C:\\a\\b", "chain: \\\\?\\ drive");
        eqs(chain("\\\\?\\C:\\a/b"), "\\\\?\\C:\\a/b", "chain: with \\\\?\\ a slash is a character");
        eqs(chain("\\\\?\\UNC\\srv\\share\\a\\b"), "\\\\?\\UNC\\srv\\share\\a | \\\\?\\UNC\\srv\\share\\a\\b", "chain: \\\\?\\UNC");
        eqs(chain("\\\\?\\UNC\\srv\\share"), "", "chain: \\\\?\\UNC share root");
        eqs(chain("\\\\srv\\share\\a/b"), "\\\\srv\\share\\a | \\\\srv\\share\\a\\b", "chain: UNC");
        eqs(chain("//srv/share/a"), "\\\\srv\\share\\a", "chain: UNC with slashes");
        eqs(chain(longPathPrefixed(std::string("C:\\x\\Movies/English"))), "\\\\?\\C:\\x | \\\\?\\C:\\x\\Movies | \\\\?\\C:\\x\\Movies\\English",
            "chain: a long path prefixed");
        check(directoryChain(std::wstring(L"C:\\a/b")).size() == 2, "chain: wide strings");
    }
    // classifyFailure: "too long" only when it is
    check(classifyFailure(Failure::Other, 46, false) == Failure::Other, "classify: error 3 at 46 characters is no length problem");
    check(classifyFailure(Failure::Other, 300, true) == Failure::Other, "classify: long paths on: the OS's error");
    check(classifyFailure(Failure::Other, 300, false) == Failure::TooLong, "classify: over MAX_PATH without long paths");
    check(classifyFailure(Failure::AccessDenied, 300, false) == Failure::AccessDenied, "classify: access denied kept");
    check(classifyFailure(Failure::NoSpace, 300, false) == Failure::NoSpace, "classify: disk full kept");
    check(classifyFailure(Failure::TooLong, 100, true) == Failure::TooLong, "classify: the OS's own too long kept");
#ifndef _WIN32
    check(ioPath(fs::path("/a/b")) == fs::path("/a/b"), "ioPath unchanged off Windows");
#endif
    // a real write: a deep folder (over 260 characters) in the temporary folder, made by several threads at once
    {
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec) / ("safehouse-paths-selftest-" + std::to_string(
#ifdef _WIN32
                                                                                                 GetCurrentProcessId()
#else
                                                                                                 long(::getpid())
#endif
                                                                                                 ));
        fs::path deep = base;
        while (pathUnits(deep) < 300) deep /= "World 001 explode_whack_flash_small";
        std::vector<std::thread> at;
        bool okAll[8] = {};
        std::string errors[8];
        for (int t = 0; t < 8; ++t)
            at.emplace_back([&, t] {
                const std::string s = "thread " + std::to_string(t);
                okAll[t] = writeFile(deep / ("Graphics " + std::to_string(t) + ".bin"), s.data(), s.size(), &errors[t]);
            });
        for (auto& th : at) th.join();
        for (int t = 0; t < 8; ++t) check(okAll[t], "deep write " + std::to_string(t) + ": " + errors[t]);
        {
            // a short path joined with "/" (the converter's "Movies/English/2097 Prologue.flac"), its folders new
            std::string why;
            const fs::path p = base / fs::path(u8"Movies/English/2097 Prologue.flac");
            check(writeFile(p, "x", 1, &why) && fs::is_regular_file(ioPath(p), ec), "short write joined with / (" + why + ")");
        }
        std::string err;
        check(!writeFile(base / "missing" / "\x01:bad" / std::string(300, 'x'), "x", 1, &err) && err.find("characters)") != std::string::npos,
              "a failure names the path and its length (" + err + ")");
        fs::remove_all(ioPath(base), ec);
    }
    std::printf("paths-selftest: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
}  // namespace cane::paths
