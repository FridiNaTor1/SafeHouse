#pragma once
// Bundled SDL_GameControllerDB mappings (zlib), applied after glfwInit() by
// both the game and the launcher so pads missing from GLFW's built-in table
// still count as gamepads.
//
// cmake/prompts.cmake embeds Libraries/gamecontrollerdb/gamecontrollerdb.txt
// and defines CANE_HAVE_GAMECONTROLLERDB. Without it GLFW's built-in mappings
// are kept; the environment's SDL_GAMECONTROLLERCONFIG is still applied.
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <string>
#include <string_view>
namespace cane::input {
#ifdef CANE_HAVE_GAMECONTROLLERDB
namespace embedded {
std::string_view gameControllerDb(); // assets/prompts/prompt_data.cpp
}
#endif
namespace detail {
// The "platform:" value GLFW matches mappings against (glfw getMappingName).
inline constexpr std::string_view kMappingPlatform =
#if defined(_WIN32)
    "Windows";
#elif defined(__APPLE__)
    "Mac OS X";
#elif defined(__ANDROID__)
    "Android";
#else
    "Linux";
#endif
// Keeps only mapping lines GLFW would accept for this platform: a 32-digit hex
// GUID, a name shorter than 128 bytes, and either a
// matching "platform:" field or none (SDL_GAMECONTROLLERCONFIG style). GLFW
// reports every malformed line through the error callback, and mappings for
// other platforms are useless here, so filtering keeps logs quiet and the
// update fast.
inline std::string filterMappings(std::string_view text) {
    std::string out;
    while (!text.empty()) {
        const size_t end = text.find_first_of("\r\n");
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const size_t guidEnd = line.find(',');
        if (guidEnd != 32 || line.find_first_not_of("0123456789abcdefABCDEF") < 32) continue;
        const size_t nameEnd = line.find(',', guidEnd + 1);
        if (nameEnd == std::string_view::npos || nameEnd - guidEnd - 1 >= 128) continue;
        const size_t platform = line.find("platform:", nameEnd);
        if (platform != std::string_view::npos) {
            const std::string_view value = line.substr(platform + 9, line.find(',', platform) - platform - 9);
            if (value != kMappingPlatform) continue;
        }
        out.append(line);
        out.push_back('\n');
    }
    return out;
}
} // namespace detail
// Applies the bundled database, then SDL_GAMECONTROLLERCONFIG (Steam passes
// its virtual pads' mappings there), so later entries win per GUID. Call
// after glfwInit(). Returns false only when GLFW rejected the update.
inline bool applyBundledMappings() {
    bool ok = true;
#ifdef CANE_HAVE_GAMECONTROLLERDB
    const std::string bundled = detail::filterMappings(embedded::gameControllerDb());
    if (!bundled.empty()) ok = glfwUpdateGamepadMappings(bundled.c_str()) == GLFW_TRUE;
#endif
    if (const char* environment = std::getenv("SDL_GAMECONTROLLERCONFIG")) {
        const std::string extra = detail::filterMappings(environment);
        if (!extra.empty()) ok = glfwUpdateGamepadMappings(extra.c_str()) == GLFW_TRUE && ok;
    }
    return ok;
}
} // namespace cane::input
