#pragma once
// Crash and hang reports for every program the launcher runs (Sly 1 / 2 / 3, cane-extractor and its tools) and the
// launcher itself. cane::crash::install(program) at the top of main:
//  - stdout line buffered, stderr unbuffered (Windows: both unbuffered, the MS CRT's _IOLBF is full buffering), so the
//    launcher's pipe gets every line when it is printed (its log's timestamps) and a crash loses nothing printed before;
//  - a crash (Windows: an unhandled exception such as an access violation or a stack overflow, abort(), std::terminate,
//    a pure virtual call or a C runtime invalid parameter; Linux / macOS: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT,
//    std::terminate) prints "crash: ..." lines on stderr: what happened (the exception code and its name, the address as
//    module+offset), the thread, the program and what it was doing (context(), note(): the world, the last events),
//    and a short stack, module+offset per frame with the function's name where the module has a symbol table (the
//    Windows programs keep their COFF symbols: the report names our functions without a .pdb);
//  - Windows also writes a minidump and the same lines as text into the log folder (<program>-crash-<date>.dmp/.txt:
//    $SAFEHOUSE_CRASH_DIR, set by the launcher to its log folder, else %LOCALAPPDATA%\SafeHouse\logs; the newest 6 of
//    each kind are kept), and watches the program's windows: one that does not answer for 10 s (the "Not responding"
//    of Windows, which ends the program with exit code 0xCFFFFFFF when the player closes it) prints "hang: ..." lines
//    with the stack of the window's thread and writes <program>-hang-<date>.dmp/.txt once.
// The launcher reads the "crash:" / "hang:" lines (backend.h explainedProcess, process.h describeExitCode).
// Test switch: SAFEHOUSE_CRASH_TEST=segv|abort|terminate|hang|stack crashes the program on purpose at install (hang:
// a frozen hidden window, Windows only); SAFEHOUSE_CRASH_FRAME=N crashes the game loop at frame N (crash_context.h).
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <stdexcept>
#include "crash_context.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#else
#include <unistd.h>
#endif
#if (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && !defined(CANE_IOS))
#define CANE_CRASH_POSIX 1
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#endif
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define CANE_CRASH_DEMANGLE 1
#endif

namespace cane::crash {
namespace detail {
struct State {
    char program[64] = "program";
    std::atomic<int> reporting{0};
    bool installed = false;
    char* demangleBuffer = nullptr;
    size_t demangleSize = 0;
#ifdef _WIN32
    wchar_t directory[MAX_PATH * 2] = L"";
    HANDLE request = nullptr, done = nullptr, watcher = nullptr, file = nullptr, mainThread = nullptr;
    DWORD mainThreadId = 0, watcherThreadId = 0;
    // the crash handed to the watcher thread (it has a stack of its own: a stack overflow has none left)
    EXCEPTION_POINTERS* pointers = nullptr;
    CONTEXT captured{};
    DWORD threadId = 0, code = 0;
    const char* what = nullptr;
    char detail[256] = "";
    decltype(&MiniDumpWriteDump) writeDump = nullptr;
    bool watchHidden = false;   // (the hang test: its window is hidden)
    HRESULT(WINAPI* threadDescription)(HANDLE, PWSTR*) = nullptr;
#elif defined(CANE_CRASH_POSIX)
    pthread_t mainThread{};
#endif
};
inline State& state() {
    static State s;
    return s;
}

// One line to stderr (and the report file on Windows), without stdio (the crash may hold its lock).
inline void writeRaw(const char* text, size_t length) {
#ifdef _WIN32
    DWORD written = 0;
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error && error != INVALID_HANDLE_VALUE) WriteFile(error, text, DWORD(length), &written, nullptr);
    if (state().file && state().file != INVALID_HANDLE_VALUE) WriteFile(state().file, text, DWORD(length), &written, nullptr);
#else
    while (length) {
        const auto n = ::write(2, text, length);
        if (n <= 0) break;
        text += n, length -= size_t(n);
    }
#endif
}
inline void emit(const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int n = std::vsnprintf(line, sizeof(line) - 2, format, arguments);
    va_end(arguments);
    if (n < 0) return;
    if (size_t(n) > sizeof(line) - 3) n = int(sizeof(line) - 3);
    line[n++] = '\n';
    line[n] = 0;
#ifdef _WIN32
    // (a text file read on Windows: CRLF)
    if (state().file && state().file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        line[n - 1] = '\r';
        line[n] = '\n';
        WriteFile(state().file, line, DWORD(n + 1), &written, nullptr);
        line[n - 1] = '\n';
        line[n] = 0;
    }
    DWORD written = 0;
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error && error != INVALID_HANDLE_VALUE) WriteFile(error, line, DWORD(n), &written, nullptr);
#else
    writeRaw(line, size_t(n));
#endif
}
inline const char* demangle(const char* name) {
#ifdef CANE_CRASH_DEMANGLE
    auto& s = state();
    if (s.demangleBuffer && name[0] == '_' && name[1] == 'Z') {
        int status = -1;
        size_t size = s.demangleSize;
        char* result = abi::__cxa_demangle(name, s.demangleBuffer, &size, &status);
        if (status == 0 && result) {
            s.demangleBuffer = result, s.demangleSize = std::max(s.demangleSize, size);
            return result;
        }
    }
#endif
    return name;
}
inline void emitContext(const char* prefix) {
    auto& s = activity();
    const long frame = s.frame.load();
    if (frame >= 0) emit("%s   doing: %s%sframe %ld", prefix, s.context, s.context[0] ? ", " : "", frame);
    else if (s.context[0]) emit("%s   doing: %s", prefix, s.context);
    const unsigned next = s.noteNext.load();
    const unsigned count = next < unsigned(kNotes) ? next : unsigned(kNotes);
    for (unsigned i = 0; i < count; ++i) {
        const char* note = s.notes[(next - count + i) % kNotes];
        if (note[0]) emit("%s   event: %s", prefix, note);
    }
}
// A recursion's frames: how many times the frame before i repeats from i on (0: it does not).
inline int repeats(const uintptr_t* frames, int count, int i) {
    if (i == 0 || frames[i] != frames[i - 1]) return 0;
    int same = 0;
    while (i + same < count && frames[i + same] == frames[i - 1]) ++same;
    return same;
}
}  // namespace detail

// The exit code / signal in words, for the launcher's messages (empty: an ordinary exit code).
inline const char* exceptionName(uint32_t code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC00000FD: return "stack overflow";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000096: return "privileged instruction";
    case 0xC0000094: return "integer division by zero";
    case 0xC0000095: return "integer overflow";
    case 0xC000008E: return "floating-point division by zero";
    case 0xC0000090: return "invalid floating-point operation";
    case 0xC0000006: return "in-page error (the program file could not be read)";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "fail-fast (stack buffer overrun, abort() or a failed runtime check)";
    case 0xC0000417: return "invalid parameter to a C runtime function";
    case 0xC0000135: return "a DLL was not found";
    case 0xC0000139: return "a DLL entry point was not found";
    case 0xC0000142: return "a DLL failed to initialise";
    case 0xC000007B: return "a DLL is for another architecture";
    case 0xC000013A: return "closed with Ctrl+C or the console";
    case 0xC0000017: return "out of memory";
    case 0xC0000420: return "assertion failure";
    case 0x80000003: return "breakpoint";
    case 0x80000004: return "single step";
    case 0x40000015: return "fatal application exit";
    case 0xE06D7363: return "uncaught C++ exception";
    case 0x20474343: return "uncaught C++ exception";
    case 0xCFFFFFFF: return "the program stopped responding and Windows closed it";
    }
    return "";
}

#ifdef _WIN32
namespace detail {
inline bool readable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return uintptr_t(address) + size <= uintptr_t(info.BaseAddress) + info.RegionSize;
}
// The return addresses from a thread's context (x64 / arm64 unwind tables; no allocation, no locks of ours).
inline int walk(CONTEXT context, uintptr_t* frames, int capacity) {
    int count = 0;
#if defined(_M_X64) || defined(__x86_64__)
    for (; count < capacity; ++count) {
        const DWORD64 pc = context.Rip, sp = context.Rsp;
        if (!pc) break;
        frames[count] = uintptr_t(pc);
        DWORD64 imageBase = 0;
        auto* function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (function) {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &context, &handlerData, &establisher, nullptr);
        } else {  // (a leaf function: the return address on top of the stack)
            if (!readable(reinterpret_cast<void*>(sp), 8)) { ++count; break; }
            context.Rip = *reinterpret_cast<DWORD64*>(sp);
            context.Rsp = sp + 8;
        }
        if (context.Rsp <= sp && context.Rip == pc) { ++count; break; }
    }
#elif defined(_M_ARM64) || defined(__aarch64__)
    for (; count < capacity; ++count) {
        const DWORD64 pc = context.Pc, sp = context.Sp;
        if (!pc) break;
        frames[count] = uintptr_t(pc);
        DWORD64 imageBase = 0;
        auto* function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (function) {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &context, &handlerData, &establisher, nullptr);
        } else {
            context.Pc = context.Lr;
        }
        if (context.Sp <= sp && context.Pc == pc) { ++count; break; }
    }
#else
    (void)context, (void)frames, (void)capacity;
#endif
    return count;
}
inline void utf8(const wchar_t* text, char* out, int capacity) {
    out[0] = 0;
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, capacity, nullptr, nullptr);
    if (n <= 0) out[0] = 0;
    out[capacity - 1] = 0;
}
// A module's COFF symbol table (the file's, not mapped): read once per module into pages of our own.
struct Symbols {
    HMODULE module = nullptr;
    char* data = nullptr;
    size_t size = 0;
    const unsigned char* table = nullptr;  // IMAGE_SYMBOL records (18 bytes)
    DWORD count = 0;
    const char* strings = nullptr;
    size_t stringsSize = 0;
    IMAGE_SECTION_HEADER sections[96]{};
    int sectionCount = 0;
    void release() {
        if (data) VirtualFree(data, 0, MEM_RELEASE);
        *this = Symbols{};
    }
    bool load(HMODULE m) {
        release();
        module = m;
        wchar_t path[MAX_PATH * 2];
        if (!GetModuleFileNameW(m, path, DWORD(std::size(path)))) return false;
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        bool ok = false;
        IMAGE_DOS_HEADER dos{};
        DWORD n = 0;
        LARGE_INTEGER fileSize{};
        auto readAt = [&](uint64_t offset, void* out, DWORD length) {
            LARGE_INTEGER at;
            at.QuadPart = LONGLONG(offset);
            return SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, out, length, &n, nullptr) && n == length;
        };
        if (GetFileSizeEx(f, &fileSize) && readAt(0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
            DWORD signature = 0;
            IMAGE_FILE_HEADER header{};
            if (readAt(uint64_t(dos.e_lfanew), &signature, 4) && signature == IMAGE_NT_SIGNATURE && ReadFile(f, &header, sizeof(header), &n, nullptr) &&
                n == sizeof(header) && header.PointerToSymbolTable && header.NumberOfSymbols) {
                sectionCount = std::min<int>(header.NumberOfSections, int(std::size(sections)));
                const uint64_t sectionsAt = uint64_t(dos.e_lfanew) + 4 + sizeof(header) + header.SizeOfOptionalHeader;
                const uint64_t tableAt = header.PointerToSymbolTable;
                if (readAt(sectionsAt, sections, DWORD(sizeof(IMAGE_SECTION_HEADER) * sectionCount)) && uint64_t(fileSize.QuadPart) > tableAt &&
                    uint64_t(fileSize.QuadPart) - tableAt < (256u << 20)) {
                    size = size_t(uint64_t(fileSize.QuadPart) - tableAt);
                    data = static_cast<char*>(VirtualAlloc(nullptr, size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                    if (data && readAt(tableAt, data, DWORD(size))) {
                        count = header.NumberOfSymbols;
                        const size_t tableSize = size_t(count) * 18;
                        if (tableSize + 4 <= size) {
                            table = reinterpret_cast<const unsigned char*>(data);
                            strings = data + tableSize;
                            std::memcpy(&stringsSize, strings, 4);
                            stringsSize = std::min(stringsSize & 0xFFFFFFFFu, size - tableSize);
                            ok = true;
                        }
                    }
                }
            }
        }
        CloseHandle(f);
        if (!ok) {
            HMODULE keep = module;
            release();
            module = keep;  // (looked up: no symbols)
        }
        return ok;
    }
    // The function containing rva, as "name+0xOFFSET" (false: no symbol).
    bool find(uintptr_t rva, char* out, size_t capacity) const {
        if (!table) return false;
        uintptr_t best = 0;
        const unsigned char* bestRecord = nullptr;
        for (DWORD i = 0; i < count; ++i) {
            const unsigned char* r = table + size_t(i) * 18;
            const unsigned char aux = r[17];
            int16_t section = 0;
            std::memcpy(&section, r + 12, 2);
            const unsigned char storage = r[16];
            uint16_t type = 0;
            std::memcpy(&type, r + 14, 2);
            if (section > 0 && section <= sectionCount && (storage == IMAGE_SYM_CLASS_EXTERNAL || storage == IMAGE_SYM_CLASS_STATIC) &&
                (sections[section - 1].Characteristics & IMAGE_SCN_MEM_EXECUTE) && !(storage == IMAGE_SYM_CLASS_STATIC && aux)) {
                uint32_t value = 0;
                std::memcpy(&value, r + 8, 4);
                const uintptr_t at = uintptr_t(sections[section - 1].VirtualAddress) + value;
                if (at <= rva && at >= best) {
                    const char* name = nameOf(r);
                    if (name[0] != '.' && name[0] != '$' && (at > best || !bestRecord || (type >> 4) == 2)) best = at, bestRecord = r;
                }
            }
            i += aux;
        }
        if (!bestRecord) return false;
        std::snprintf(out, capacity, "%s+0x%llx", demangle(nameOf(bestRecord)), (unsigned long long)(rva - best));
        return true;
    }
    const char* nameOf(const unsigned char* r) const {
        static thread_local char shortName[9];
        uint32_t zeros = 0, offset = 0;
        std::memcpy(&zeros, r, 4);
        std::memcpy(&offset, r + 4, 4);
        if (zeros == 0) return offset < stringsSize ? strings + offset : "";
        std::memcpy(shortName, r, 8);
        shortName[8] = 0;
        return shortName;
    }
};
inline void describeFrame(const char* prefix, int index, uintptr_t pc, bool returnAddress, Symbols& symbols) {
    HMODULE module = nullptr;
    char name[160] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(pc), &module) && module) {
        wchar_t path[MAX_PATH * 2];
        if (GetModuleFileNameW(module, path, DWORD(std::size(path)))) {
            const wchar_t* base = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\' || *p == L'/') base = p + 1;
            utf8(base, name, int(sizeof(name)));
        }
        const uintptr_t offset = pc - uintptr_t(module);
        char symbol[400] = "";
        if (symbols.module != module) symbols.load(module);
        // (a return address: the call is the byte before it)
        if (symbols.find(returnAddress && offset ? offset - 1 : offset, symbol, sizeof(symbol)) && returnAddress) {
            // print the return address's offset, the name of the call's function
            if (char* plus = std::strrchr(symbol, '+')) {
                unsigned long long o = std::strtoull(plus + 1, nullptr, 16);
                std::snprintf(plus, size_t(symbol + sizeof(symbol) - plus), "+0x%llx", o + 1);
            }
        }
        emit("%s   #%d %s+0x%llx%s%s", prefix, index, name, (unsigned long long)offset, symbol[0] ? " " : "", symbol);
    } else
        emit("%s   #%d 0x%llx", prefix, index, (unsigned long long)pc);
}
inline void threadName(HANDLE thread, char* out, int capacity) {
    out[0] = 0;
    auto& s = state();
    PWSTR description = nullptr;
    if (s.threadDescription && thread && SUCCEEDED(s.threadDescription(thread, &description)) && description) {
        if (description[0]) utf8(description, out, capacity);
        LocalFree(description);
    }
}
// <directory>\<program>-<kind>-<date>.<extension>
inline void reportPath(wchar_t* out, size_t capacity, const char* kind, const SYSTEMTIME& t, const wchar_t* extension) {
    wchar_t program[64];
    MultiByteToWideChar(CP_UTF8, 0, state().program, -1, program, int(std::size(program)));
    program[63] = 0;
    std::swprintf(out, capacity, L"%ls\\%ls-%hs-%04u-%02u-%02u-%02u%02u%02u.%ls", state().directory, program, kind, t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond, extension);
}
// Keeps the newest `keep` files of a pattern in the report folder.
inline void prune(const char* kind, const wchar_t* extension, int keep) {
    auto& s = state();
    wchar_t pattern[MAX_PATH * 2], program[64];
    MultiByteToWideChar(CP_UTF8, 0, s.program, -1, program, int(std::size(program)));
    program[63] = 0;
    std::swprintf(pattern, std::size(pattern), L"%ls\\%ls-%hs-*.%ls", s.directory, program, kind, extension);
    for (;;) {
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileW(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) return;
        int count = 0;
        wchar_t oldest[MAX_PATH] = L"";
        do {
            ++count;
            if (!oldest[0] || std::wcscmp(found.cFileName, oldest) < 0) std::wcscpy(oldest, found.cFileName);  // (dated names)
        } while (FindNextFileW(search, &found));
        FindClose(search);
        if (count <= keep) return;
        wchar_t path[MAX_PATH * 2];
        std::swprintf(path, std::size(path), L"%ls\\%ls", s.directory, oldest);
        if (!DeleteFileW(path)) return;
    }
}
inline bool openReport(const char* kind, wchar_t* dumpPath, size_t capacity) {
    auto& s = state();
    dumpPath[0] = 0;
    if (!s.directory[0]) return false;
    CreateDirectoryW(s.directory, nullptr);
    prune(kind, L"dmp", 5);
    prune(kind, L"txt", 5);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t text[MAX_PATH * 2];
    reportPath(text, std::size(text), kind, t, L"txt");
    reportPath(dumpPath, capacity, kind, t, L"dmp");
    s.file = CreateFileW(text, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) s.file = nullptr;
    return true;
}
inline void closeReport() {
    auto& s = state();
    if (s.file) CloseHandle(s.file);
    s.file = nullptr;
}
inline void writeDump(const wchar_t* path, DWORD threadId, EXCEPTION_POINTERS* pointers, const char* prefix) {
    auto& s = state();
    if (!s.writeDump || !path[0]) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        emit("%s minidump: could not create it (Windows error %lu)", prefix, GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{threadId, pointers, FALSE};
    const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory |
                                    MiniDumpWithProcessThreadData | MiniDumpWithHandleData);
    const BOOL ok = s.writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, pointers ? &exception : nullptr, nullptr, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(f);
    char text[MAX_PATH * 3];
    utf8(path, text, int(sizeof(text)));
    if (ok) emit("%s minidump: %s", prefix, text);
    else {
        DeleteFileW(path);
        emit("%s minidump: could not write it (error 0x%lx)", prefix, error);
    }
}
// The crash report (on the watcher thread, or the crashed one when there is no watcher).
inline void reportCrash() {
    auto& s = state();
    Symbols symbols;
    wchar_t dump[MAX_PATH * 2];
    openReport("crash", dump, std::size(dump));
    const DWORD code = s.code;
    EXCEPTION_RECORD* record = s.pointers ? s.pointers->ExceptionRecord : nullptr;
    const uintptr_t address = record ? uintptr_t(record->ExceptionAddress) : 0;
    char where[200] = "";
    if (address) {
        HMODULE module = nullptr;
        char name[120] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address),
                               &module) && module) {
            wchar_t path[MAX_PATH * 2];
            if (GetModuleFileNameW(module, path, DWORD(std::size(path)))) {
                const wchar_t* base = path;
                for (const wchar_t* p = path; *p; ++p)
                    if (*p == L'\\' || *p == L'/') base = p + 1;
                utf8(base, name, int(sizeof(name)));
            }
            std::snprintf(where, sizeof(where), " at %s+0x%llx", name, (unsigned long long)(address - uintptr_t(module)));
        } else
            std::snprintf(where, sizeof(where), " at 0x%llx", (unsigned long long)address);
    }
    char detail[200] = "";
    if (record && code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        std::snprintf(detail, sizeof(detail), " (%s 0x%llx)", kind == 0 ? "reading" : kind == 8 ? "executing" : "writing",
                      (unsigned long long)record->ExceptionInformation[1]);
    } else if (s.detail[0])
        std::snprintf(detail, sizeof(detail), ": %s", s.detail);
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.threadId);
    char threadText[96];
    threadName(thread, threadText, int(sizeof(threadText)));
    if (thread) CloseHandle(thread);
    const char* name = s.what ? s.what : exceptionName(code);
    emit("crash: %s crashed: %s%s (0x%08lX)%s, thread %lu%s%s%s%s", s.program, name[0] ? name : "unhandled exception", detail, code, where, s.threadId,
         s.threadId == s.mainThreadId ? " (main)" : "", threadText[0] ? " \"" : "", threadText, threadText[0] ? "\"" : "");
    emitContext("crash:");
    // the minidump before the stack's names (they read files and demangle: the heap may be what broke)
    writeDump(dump, s.threadId, s.pointers, "crash:");
    uintptr_t frames[48];
    const int count = walk(s.pointers ? *s.pointers->ContextRecord : s.captured, frames, int(std::size(frames)));
    emit("crash:   stack:");
    for (int i = 0; i < count; ++i)
        if (int same = repeats(frames, count, i)) emit("crash:   (the frame above %d more times)", same), i += same - 1;
        else describeFrame("crash:", i, frames[i], i > 0, symbols);
    symbols.release();
    closeReport();
}
// A window of ours that does not answer (the watcher thread, every second).
struct HangWatch {
    DWORD since = 0;  // (GetTickCount when it stopped answering)
    bool reported = false, dumped = false;
};
inline HWND windowOfOurs() {
    struct Find {
        DWORD process;
        HWND window;
    } find{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* f = reinterpret_cast<Find*>(parameter);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process == f->process && (IsWindowVisible(window) || state().watchHidden)) {
                f->window = window;
                return FALSE;
            }
            return TRUE;
        },
        LPARAM(&find));
    return find.window;
}
inline void reportHang(HWND window, DWORD seconds, HangWatch& watch) {
    auto& s = state();
    const DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    wchar_t dump[MAX_PATH * 2] = L"";
    if (!watch.dumped) openReport("hang", dump, std::size(dump));
    char threadText[96] = "";
    uintptr_t frames[48];
    int count = 0;
    if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId)) {
        threadName(thread, threadText, int(sizeof(threadText)));
        // (only the return addresses while it is stopped: it may hold the heap's or the loader's lock)
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) count = walk(context, frames, int(std::size(frames)));
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    emit("hang: %s is not responding: its window has not answered for %lu s, thread %lu%s%s%s%s", s.program, seconds, threadId,
         threadId == s.mainThreadId ? " (main)" : "", threadText[0] ? " \"" : "", threadText, threadText[0] ? "\"" : "");
    emitContext("hang:");
    if (!watch.dumped) {
        writeDump(dump, threadId, nullptr, "hang:");
        watch.dumped = true;
    }
    Symbols symbols;
    emit("hang:   stack:");
    for (int i = 0; i < count; ++i)
        if (int same = repeats(frames, count, i)) emit("hang:   (the frame above %d more times)", same), i += same - 1;
        else describeFrame("hang:", i, frames[i], i > 0, symbols);
    symbols.release();
    closeReport();
}
inline DWORD WINAPI watcher(void*) {
    auto& s = state();
    HangWatch watch;
    const bool watchHangs = !std::getenv("SAFEHOUSE_NO_HANG_WATCH");
    for (;;) {
        const HANDLE handles[2] = {s.request, s.mainThread};
        const DWORD wait = WaitForMultipleObjects(s.mainThread ? 2 : 1, handles, FALSE, 1000);
        if (wait == WAIT_OBJECT_0) {
            reportCrash();
            SetEvent(s.done);
            continue;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            // The main thread is gone but the process is not (main's return ends the process): it was ended without
            // the unhandled-exception filter (Wine ends a thread whose stack overflowed so); report it and end the
            // process, which this thread would otherwise keep alive.
            DWORD code = 0;
            GetExitCodeThread(s.mainThread, &code);
            if (!s.reporting.exchange(1)) {
                wchar_t dump[MAX_PATH * 2];
                openReport("crash", dump, std::size(dump));
                emit("crash: %s crashed: its main thread ended unexpectedly (0x%08lX%s%s)", s.program, code, exceptionName(code)[0] ? ", " : "",
                     exceptionName(code));
                emitContext("crash:");
                writeDump(dump, GetCurrentThreadId(), nullptr, "crash:");
                closeReport();
            }
            TerminateProcess(GetCurrentProcess(), code ? code : 0xC00000FD);
        }
        if (!watchHangs || IsDebuggerPresent()) continue;
        HWND window = windowOfOurs();
        if (!window) {
            watch.since = 0;
            continue;
        }
        DWORD_PTR result = 0;
        const bool answered = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
        if (answered) {
            if (watch.reported) emit("hang: %s responds again after %lu s", s.program, (GetTickCount() - watch.since) / 1000);
            watch.since = 0, watch.reported = false;
            continue;
        }
        if (!watch.since) watch.since = GetTickCount();
        const DWORD seconds = (GetTickCount() - watch.since) / 1000;
        if (!watch.reported && seconds >= 10) {
            watch.reported = true;
            reportHang(window, seconds, watch);
        }
    }
}
// The crash: hand it to the watcher (or report here), then end the process with `exitCode`.
[[noreturn]] inline void fatal(EXCEPTION_POINTERS* pointers, DWORD code, const char* what, const char* detail, DWORD exitCode) {
    auto& s = state();
    if (s.reporting.exchange(1)) {
        // (the report itself failed: end now; else another thread is reporting, and it ends the process)
        if (GetCurrentThreadId() == s.watcherThreadId) TerminateProcess(GetCurrentProcess(), s.code ? s.code : exitCode);
        Sleep(60000);
        TerminateProcess(GetCurrentProcess(), exitCode);
    }
    s.pointers = pointers, s.code = code, s.what = what, s.threadId = GetCurrentThreadId();
    std::snprintf(s.detail, sizeof(s.detail), "%s", detail ? detail : "");
    if (!pointers) RtlCaptureContext(&s.captured);
    if (s.watcher && s.request) {
        SetEvent(s.request);
        WaitForSingleObject(s.done, 60000);
    } else
        reportCrash();
    TerminateProcess(GetCurrentProcess(), exitCode);
    for (;;) Sleep(1000);
}
inline LONG WINAPI unhandled(EXCEPTION_POINTERS* pointers) {
    const DWORD code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0;
    // (an uncaught C++ exception of libunwind / libgcc, STATUS_GCC_THROW: RaiseException returns to __cxa_throw, which
    // calls std::terminate, onTerminate: the report with the exception's message)
    if (code == 0x20474343) return EXCEPTION_CONTINUE_EXECUTION;
    fatal(pointers, code, nullptr, nullptr, code);
}
inline void onAbort(int) { fatal(nullptr, 3, "abort()", nullptr, 3); }
inline void onPureCall() { fatal(nullptr, 3, "pure virtual function call", nullptr, 3); }
inline void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    fatal(nullptr, 0xC0000417, nullptr, nullptr, 0xC0000417);
}
inline void reportDirectory() {
    auto& s = state();
    DWORD n = GetEnvironmentVariableW(L"SAFEHOUSE_CRASH_DIR", s.directory, DWORD(std::size(s.directory)));
    if (n && n < std::size(s.directory)) return;
    wchar_t local[MAX_PATH * 2];
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, DWORD(std::size(local)));
    if (n && n < std::size(local)) {
        std::swprintf(s.directory, std::size(s.directory), L"%ls\\SafeHouse", local);
        CreateDirectoryW(s.directory, nullptr);
        std::swprintf(s.directory, std::size(s.directory), L"%ls\\SafeHouse\\logs", local);
    } else
        s.directory[0] = 0;
}
}  // namespace detail
#endif

#ifdef CANE_CRASH_POSIX
namespace detail {
inline const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "segmentation fault (SIGSEGV)";
    case SIGBUS: return "bus error (SIGBUS)";
    case SIGILL: return "illegal instruction (SIGILL)";
    case SIGFPE: return "arithmetic exception (SIGFPE)";
    case SIGABRT: return "abort (SIGABRT)";
    }
    return "signal";
}
inline void posixStack(const char* prefix, int skip) {
    void* frames[48];
    const int count = backtrace(frames, 48);
    emit("%s   stack:", prefix);
    for (int i = skip; i < count; ++i) {
        Dl_info info{};
        const uintptr_t pc = uintptr_t(frames[i]);
        if (int same = repeats(reinterpret_cast<uintptr_t*>(frames), count, i)) {
            emit("%s   (the frame above %d more times)", prefix, same);
            i += same - 1;
            continue;
        }
        if (dladdr(frames[i], &info) && info.dli_fname) {
            const char* base = std::strrchr(info.dli_fname, '/');
            base = base ? base + 1 : info.dli_fname;
            char symbol[400] = "";
            if (info.dli_sname && info.dli_saddr)
                std::snprintf(symbol, sizeof(symbol), " %s+0x%llx", demangle(info.dli_sname), (unsigned long long)(pc - uintptr_t(info.dli_saddr)));
            emit("%s   #%d %s+0x%llx%s", prefix, i - skip, base, (unsigned long long)(pc - uintptr_t(info.dli_fbase)), symbol);
        } else
            emit("%s   #%d 0x%llx", prefix, i - skip, (unsigned long long)pc);
    }
}
inline void onSignal(int sig, siginfo_t* info, void*) {
    auto& s = state();
    if (!s.reporting.exchange(1)) {
        const bool main = pthread_equal(pthread_self(), s.mainThread);
        char where[96] = "";
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)
            std::snprintf(where, sizeof(where), " at address %p", info ? info->si_addr : nullptr);
        emit("crash: %s crashed: %s%s, %s thread", s.program, signalName(sig), where, main ? "main" : "a worker");
        emitContext("crash:");
        posixStack("crash:", 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
}  // namespace detail
#endif

namespace detail {
[[noreturn]] inline void onTerminate() {
    char what[240] = "std::terminate without an exception";
    if (auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            std::snprintf(what, sizeof(what), "uncaught exception: %s", e.what());
        } catch (...) {
            std::snprintf(what, sizeof(what), "uncaught exception of an unknown type");
        }
    }
#ifdef _WIN32
    fatal(nullptr, 3, "std::terminate", what, 3);
#else
    auto& s = state();
    if (!s.reporting.exchange(1)) {
        emit("crash: %s crashed: std::terminate: %s", s.program, what);
        emitContext("crash:");
#ifdef CANE_CRASH_POSIX
        posixStack("crash:", 1);
#endif
    }
    signal(SIGABRT, SIG_DFL);
    std::abort();
#endif
}
inline void selfTest() {
    const char* test = std::getenv("SAFEHOUSE_CRASH_TEST");
    if (!test || !test[0]) return;
    std::printf("crash test: %s (stdout before the crash)\n", test);
    std::fprintf(stderr, "crash test: %s (stderr before the crash)\n", test);
    note("crash test %s", test);
    context("the crash test");
    if (!std::strcmp(test, "segv")) {
        volatile int* volatile pointer = nullptr;
        *pointer = 42;
    } else if (!std::strcmp(test, "abort"))
        std::abort();
    else if (!std::strcmp(test, "terminate"))
        throw std::runtime_error("the crash test's exception");
    else if (!std::strcmp(test, "stack")) {
        struct Recurse {
            static int go(int depth) {
                static volatile int limit = 1 << 30;
                volatile char page[4096];
                page[0] = char(depth);
                return depth < limit ? go(depth + 1) + page[0] : 0;
            }
        };
        std::printf("%d\n", Recurse::go(0));
    }
#ifdef _WIN32
    else if (!std::strcmp(test, "hang")) {
        // (a window that never answers: the watcher reports it after 10 s; then the crash test ends after 16 s)
        // (hidden: nothing on the desktop; the watcher looks at hidden windows for this test)
        state().watchHidden = true;
        CreateWindowExW(0, L"STATIC", L"SafeHouse crash test", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr, nullptr, nullptr);
        Sleep(16000);
        std::printf("crash test: hang over\n");
        std::exit(5);
    }
#endif
}
}  // namespace detail

// At the top of main: unbuffered output, the crash handlers, the watcher thread. `program`: "Sly2-Release". `test`:
// SAFEHOUSE_CRASH_TEST applies (not to the launcher, which passes it on to the programs it runs).
inline void install(const char* program, bool test = true) {
    auto& s = detail::state();
#if !defined(_WIN32) && !defined(CANE_CRASH_POSIX)
    // (Android, iOS, the Switch: the programs run in the app's process, which sets up its output and its handlers)
    std::snprintf(s.program, sizeof(s.program), "%s", program);
    return;
#endif
    if (s.installed) return;
#ifdef _WIN32
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // (the MS CRT's _IOLBF is full buffering)
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 1 << 14);
#endif
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    s.installed = true;
    std::snprintf(s.program, sizeof(s.program), "%s", program);
    s.demangleSize = 4096;
    s.demangleBuffer = static_cast<char*>(std::malloc(s.demangleSize));
    std::set_terminate(detail::onTerminate);
#ifdef _WIN32
    detail::reportDirectory();
    s.mainThreadId = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s.mainThread, SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
        s.writeDump = reinterpret_cast<decltype(s.writeDump)>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        s.threadDescription = reinterpret_cast<decltype(s.threadDescription)>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetThreadDescription")));
    s.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (s.request && s.done) s.watcher = CreateThread(nullptr, 512 * 1024, detail::watcher, nullptr, 0, &s.watcherThreadId);
    // (the stack overflow's handler runs on the overflowed stack: room for it on the main thread)
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(detail::unhandled);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, detail::onAbort);
    _set_purecall_handler(detail::onPureCall);
    _set_invalid_parameter_handler(detail::onInvalidParameter);
#elif defined(CANE_CRASH_POSIX)
    s.mainThread = pthread_self();
    {
        void* warm[2];
        backtrace(warm, 2);  // (loads the unwinder now, not inside the handler)
    }
    static char alternate[64 * 1024];
    stack_t stack{};
    stack.ss_sp = alternate;
    stack.ss_size = sizeof(alternate);
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = detail::onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &action, nullptr);
#endif
    if (test) detail::selfTest();
}

// The launcher: where its children (and itself) write the reports (UTF-8). The children read it from the environment.
inline void setDirectory(const char* utf8Path) {
#ifdef _WIN32
    auto& s = detail::state();
    wchar_t wide[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, wide, int(std::size(wide)))) return;
    SetEnvironmentVariableW(L"SAFEHOUSE_CRASH_DIR", wide);
    std::swprintf(s.directory, std::size(s.directory), L"%ls", wide);
#else
    setenv("SAFEHOUSE_CRASH_DIR", utf8Path, 1);
#endif
}
}  // namespace cane::crash
