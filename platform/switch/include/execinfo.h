#pragma once
// Switch builds of Sly2-Release only: CMake puts platform/switch/include on the include path, so the glibc <execinfo.h>
// used by the debug backtraces resolves here. There is no console to print them to, so they are empty.
inline int backtrace(void**, int) { return 0; }
inline char** backtrace_symbols(void* const*, int) { return nullptr; }
inline void backtrace_symbols_fd(void* const*, int, int) {}
