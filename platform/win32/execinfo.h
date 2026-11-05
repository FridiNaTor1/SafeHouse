#pragma once
// Windows builds of Sly2-Release / Sly3-Release only: CMake puts platform/win32 on the include path for WIN32, so the
// glibc <execinfo.h> used by the debug backtraces resolves here. The backtraces come from the OS stack walker.
#include <cstdio>
#include <windows.h>
inline int backtrace(void** frames, int size) {
    return size > 0 ? static_cast<int>(RtlCaptureStackBackTrace(1, static_cast<DWORD>(size), frames, nullptr)) : 0;
}
inline char** backtrace_symbols(void* const*, int) { return nullptr; }
inline void backtrace_symbols_fd(void* const* frames, int size, int fd) {
    FILE* out = fd == 1 ? stdout : stderr;
    for (int i = 0; i < size; ++i) std::fprintf(out, "  [%d] %p\n", i, frames[i]);
    std::fflush(out);
}
