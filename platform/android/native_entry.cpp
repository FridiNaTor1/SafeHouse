// The JNI entry points of the Android libraries (cmake/android.cmake): SDL's activities call SDL_main, the extractor
// service calls cane_extractor_main. Each runs the desktop program's main under another name (main=... at compile
// time) and reports its exit code to the launcher process (android_app.cpp reportExit). Adapted from ThiefRush.
#include <SDL.h>
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include "android_app.h"

#if defined(CANE_ANDROID_GAME_ENTRY)
int cane_sly2_main(int, char**);
#define CANE_ANDROID_ENTRY cane_sly2_main
#define CANE_ANDROID_ROLE "game"
#elif defined(CANE_ANDROID_LAUNCHER_ENTRY)
int cane_launcher_main(int, char**);
#define CANE_ANDROID_ENTRY cane_launcher_main
#define CANE_ANDROID_ROLE "launcher"
#elif defined(CANE_ANDROID_EXTRACTOR_ENTRY)
int cane_extractor_implementation(int, char**);
extern "C" __attribute__((visibility("default"))) int cane_extractor_main(int argc, char** argv) {
    return cane_extractor_implementation(argc, argv);
}
#define CANE_ANDROID_ENTRY cane_extractor_main
#define CANE_ANDROID_ROLE "extractor"
#endif

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char** argv) {
#if defined(CANE_ANDROID_GAME_ENTRY)
    // Sly 2's tools (--convert, --export-textures, --ps3-hd-textures) run in the tool service without a window.
    bool tool = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--convert") || !std::strcmp(argv[i], "--export-textures") ||
            !std::strcmp(argv[i], "--ps3-hd-textures"))
            tool = true;
    setenv("SAFEHOUSE_ANDROID_ROLE", tool ? "tool" : CANE_ANDROID_ROLE, 1);
#else
    setenv("SAFEHOUSE_ANDROID_ROLE", CANE_ANDROID_ROLE, 1);
#endif
    // Device tests: KEY=VALUE lines of files/env.txt become environment variables (the desktop's SLY2N_* switches,
    // e.g. SLY2N_FPSLOG=1); adb shell run-as org.safehouse.android writes it. Absent in normal use.
    if (const char* home = std::getenv("HOME")) {
        if (FILE* env = std::fopen((std::string(home) + "/env.txt").c_str(), "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), env)) {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                const auto eq = text.find('=');
                if (eq != std::string::npos && eq > 0 && text[0] != '#') setenv(text.substr(0, eq).c_str(), text.substr(eq + 1).c_str(), 1);
            }
            std::fclose(env);
        }
    }
    try {
        const int code = CANE_ANDROID_ENTRY(argc, argv);
        std::cout.flush();
        std::cerr.flush();
        cane::android::reportExit(code);
        return code;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "SafeHouse", "%s", e.what());
        std::cerr << "Android runtime failed: " << e.what() << std::endl;
        try { cane::android::reportExit(1); } catch (...) {}
        return 1;
    }
}

// A program that leaves through exit / _exit / std::_Exit (Sly 2's --convert skips its static destructors that way)
// never returns to SDL_main: report its code first, so the launcher sees the real result, and give the output tee
// (android_app.cpp nativeAttachPipes) a moment to pass the last lines on. The links wrap these calls
// (cmake/android.cmake: -Wl,--wrap=...).
#include <unistd.h>
extern "C" {
[[noreturn]] void __real_exit(int);
[[noreturn]] void __real__exit(int);
[[noreturn]] void __real__Exit(int);
static void reportLeaving(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    try { cane::android::reportExit(code); } catch (...) {}
    usleep(200000);
}
[[noreturn]] __attribute__((visibility("default"))) void __wrap_exit(int code) { reportLeaving(code); __real_exit(code); }
[[noreturn]] __attribute__((visibility("default"))) void __wrap__exit(int code) { reportLeaving(code); __real__exit(code); }
[[noreturn]] __attribute__((visibility("default"))) void __wrap__Exit(int code) { reportLeaving(code); __real__Exit(code); }
}
