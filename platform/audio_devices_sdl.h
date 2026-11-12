#pragma once
// The launcher's audio device list (platform/audio_devices.h): SDL2 loaded at run time (SDL2.dll beside the programs on
// Windows, the bundled / system libSDL2-2.0.so.0 elsewhere), as launcher/collection/audio.h does. Lists only: nothing
// is opened (no sound played, no microphone recorded). Not on Android / the Switch (the device rows are not shown).
#include "audio_devices.h"

#if !defined(__ANDROID__) && !defined(__SWITCH__)
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "platform.h"

namespace cane::audio {
// The devices SDL2 lists (SDL2 loaded at run time: SDL2.dll beside the programs on Windows, the system's / bundled
// libSDL2-2.0.so.0 elsewhere). Lists only: nothing is opened (no sound played, no microphone recorded).
inline DeviceList listDevices() {
    DeviceList out;
    if (fakeDevices(out)) return out;
#ifdef _WIN32
    HMODULE lib = LoadLibraryW((executableDirectory() / L"SDL2.dll").c_str());
    if (!lib) lib = LoadLibraryW(L"SDL2.dll");
    if (!lib) return out;
    auto sym = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(lib, n)); };
#else
    void* lib = nullptr;
    for (const std::filesystem::path& p : { executableDirectory() / "libSDL2-2.0.so.0", executableDirectory().parent_path() / "lib" / "libSDL2-2.0.so.0",
                                            std::filesystem::path("libSDL2-2.0.so.0"), std::filesystem::path("libSDL2-2.0.0.dylib"),
                                            std::filesystem::path("libSDL2.so") })
        if ((lib = dlopen(utf8(p).c_str(), RTLD_NOW | RTLD_LOCAL))) break;
    if (!lib) return out;
    auto sym = [&](const char* n) { return dlsym(lib, n); };
#endif
    constexpr uint32_t kInitAudio = 0x10;
    auto init = reinterpret_cast<int (*)(uint32_t)>(sym("SDL_InitSubSystem"));
    auto quit = reinterpret_cast<void (*)(uint32_t)>(sym("SDL_QuitSubSystem"));
    auto count = reinterpret_cast<int (*)(int)>(sym("SDL_GetNumAudioDevices"));
    auto name = reinterpret_cast<const char* (*)(int, int)>(sym("SDL_GetAudioDeviceName"));
    if (!init || !quit || !count || !name || init(kInitAudio) != 0) return out;
    out.available = true;
    for (int capture = 0; capture < 2; ++capture) {
        const int n = count(capture);
        for (int i = 0; i < n; ++i)
            if (const char* s = name(i, capture)) {
                const std::string c = cleanName(s);
                auto& v = capture ? out.inputs : out.outputs;
                if (!c.empty() && std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c);
            }
    }
    quit(kInitAudio);
    return out;
}
} // namespace cane::audio
#endif
