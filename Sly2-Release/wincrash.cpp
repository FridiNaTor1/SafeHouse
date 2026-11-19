// (Windows) no "Not Responding" ghost window over a stall of the game thread (a big level's first load and its pipelines
// on a slow driver): without it Windows puts a ghost over the window after 5 s, and its close ends the game at once with
// exit 0xCFFFFFFF, which players report as a crash (2026-11-19, the end of the Prologue's movie on an Intel GPU). With
// it the window waits, a close clicked meanwhile is taken when the game thread is back, and platform/crash_report.h's
// hang watch still reports a window that does not answer for 10 s.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
void DisableWindowGhosting() { DisableProcessWindowsGhosting(); }
#else
void DisableWindowGhosting() {}
#endif
