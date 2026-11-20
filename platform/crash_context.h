#pragma once
// What a program is doing, for its crash and hang reports (crash_report.h): cheap to call from game code, no system
// headers. context(): the current state ("world 9 (Paris hub)"); frame(): the game loop's counter; note(): an event,
// the last few are kept.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace cane::crash {
namespace detail {
constexpr int kNotes = 8;
struct Activity {
    char context[192] = "";
    char notes[kNotes][160] = {};
    std::atomic<unsigned> noteNext{0};
    std::atomic<long> frame{-1};
};
inline Activity& activity() {
    static Activity a;
    return a;
}
}  // namespace detail

inline void context(const char* format, ...) {
    auto& a = detail::activity();
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(a.context, sizeof(a.context), format, arguments);
    va_end(arguments);
}
// (test switch: SAFEHOUSE_CRASH_FRAME=N writes through a null pointer at frame N, a crash in the middle of the game)
inline void frame(long n) {
    detail::activity().frame.store(n, std::memory_order_relaxed);
    static const long crashAt = [] {
        const char* text = std::getenv("SAFEHOUSE_CRASH_FRAME");
        return text && *text ? std::atol(text) : -1L;
    }();
    if (n == crashAt && crashAt >= 0) {
        volatile int* volatile pointer = nullptr;
        *pointer = int(n);
    }
}
inline void note(const char* format, ...) {
    auto& a = detail::activity();
    char* slot = a.notes[a.noteNext.fetch_add(1) % detail::kNotes];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(slot, sizeof(a.notes[0]), format, arguments);
    va_end(arguments);
}
}  // namespace cane::crash
