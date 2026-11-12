// Force-included into Sly2-Release on Android (cmake: Sly2-Release/CMakeLists.txt), as platform/win32/sly_win32_compat.h
// is on Windows: the glibc calls bionic lacks at API 26 (backtrace arrives in API 33, malloc_trim never).
#pragma once
#ifdef __cplusplus
#include <cstddef>
#include <malloc.h>
#include <execinfo.h>
#if __ANDROID_API__ < 33
// (the crash and debug stacks print nothing on Android; logcat's tombstone has the native stack)
inline int backtrace(void**, int) { return 0; }
inline char** backtrace_symbols(void* const*, int) { return nullptr; }
inline void backtrace_symbols_fd(void* const*, int, int) {}
#endif
inline int malloc_trim(size_t) { return 0; }
#endif
