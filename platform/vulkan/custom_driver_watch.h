#ifndef CANE_PLATFORM_VULKAN_CUSTOM_DRIVER_WATCH_H_
#define CANE_PLATFORM_VULKAN_CUSTOM_DRIVER_WATCH_H_
// A custom Vulkan driver that runs but shows nothing (Android, vk_loader.h CustomDriverActive): the S26 Ultra's Adreno
// 840 with the "Turnip A8XX Draft" build (2026-11-24) loaded, the game ran and was heard, the screen stayed black.
// No crash, so the start marker and the driver's crash check never fired. Every game's Present on Vulkan calls
// WatchCustomDriverPicture first: on a custom driver it looks at the window's picture once a second (a read-back, only
// while watching); the first picture with anything brighter than near-black ends the watch, kWatchSeconds of black
// pictures send the driver choice back to the system's for the next start (NoteStartFailed: the launcher says why) and
// the log says so. The games show a logo or a movie within seconds of their start, so a black start that long is no
// picture at all. Not on the system driver, not on the desktops.
#include <cstdio>
#include <string>
#include "gl_vulkan.h"
#include "vk_loader.h"

namespace cane::vk {

inline constexpr int kWatchSeconds = 20;

// `now`: seconds (glfwGetTime). Before CaneVk::Present.
inline void WatchCustomDriverPicture(double now) {
#if defined(__ANDROID__)
    static bool s_done = false;
    static double s_next = -1;
    static int s_black = 0;
    if (s_done) return;
    if (!CustomDriverActive()) { s_done = true; return; }
    if (s_next < 0) { s_next = now + 1.0; return; }
    if (now < s_next) return;
    s_next = now + 1.0;
    const int picture = CaneVk::SampleWindowPicture();
    if (picture > 0) { s_done = true; return; }
    if (picture < 0 || ++s_black < kWatchSeconds) return;
    s_done = true;
    const std::string what = "showed no picture (" + std::to_string(kWatchSeconds) + " s of black frames)";
    std::printf("Vulkan: the custom driver %s: the system's driver the next time the game starts\n", what.c_str());
    std::fflush(stdout);
    NoteStartFailed("it " + what);
#else
    (void)now;
#endif
}

}  // namespace cane::vk

#endif  // CANE_PLATFORM_VULKAN_CUSTOM_DRIVER_WATCH_H_
