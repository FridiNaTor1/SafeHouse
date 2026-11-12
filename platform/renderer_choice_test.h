#ifndef CANE_PLATFORM_RENDERER_CHOICE_TEST_H_
#define CANE_PLATFORM_RENDERER_CHOICE_TEST_H_
// The renderer choice's rules and start marker (renderer_choice.h), without a GPU: the games' --renderer-selftest
// (ctest sly2-renderer). parse: the game's FParseRenderer as Choice values (-1 not understood).
#include <cstdio>
#include <filesystem>
#include <string>

#include "renderer_choice.h"

namespace cane::renderer {

inline int selfTest(int (*parse)(const char*)) {
    int failed = 0, passed = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) ++passed;
        else ++failed, std::printf("FAIL %s\n", what);
    };
    const std::string v = "1.2.3, abcdef12";
    const Marker none;
    Marker starting;   // (a start that did not finish: crash or kill)
    starting.present = true, starting.pid = 999999999, starting.version = v;
    Marker failedStart = starting;
    failedStart.reason = "vkCreateDevice failed";
    Marker otherVersion = failedStart;
    otherVersion.version = "1.2.2, 00000000";
    Marker running = starting;   // (another copy of the game that is starting right now: this process stands in for it)
    running.pid = currentProcessId();

    // Auto: Vulkan first
    check(kAutoPrefersVulkan, "Auto prefers Vulkan");
    check(decide(Choice::Auto, true, none, v).tryVulkan, "Auto tries Vulkan");
    check(!decide(Choice::Auto, true, none, v, false).tryVulkan, "Auto without the preference is OpenGL");
    // explicit choices
    check(decide(Choice::Vulkan, true, none, v).tryVulkan, "Vulkan chosen tries Vulkan");
    check(!decide(Choice::OpenGL, true, none, v).tryVulkan, "OpenGL chosen is OpenGL");
    check(decide(Choice::OpenGL, true, failedStart, v).why.empty(), "OpenGL chosen says nothing");
    // builds without Vulkan (macOS: Metal; the Switch: deko3d)
    check(!decide(Choice::Auto, false, none, v).tryVulkan, "no Vulkan build: Auto is OpenGL");
    check(decide(Choice::Auto, false, none, v).why.empty(), "no Vulkan build: Auto says nothing");
    check(!decide(Choice::Vulkan, false, none, v).tryVulkan, "no Vulkan build: Vulkan chosen is OpenGL");
    check(!decide(Choice::Vulkan, false, none, v).why.empty(), "no Vulkan build: Vulkan chosen says why");
    // the start marker
    check(!decide(Choice::Auto, true, starting, v).tryVulkan, "a start that did not finish: Auto is OpenGL");
    check(!decide(Choice::Auto, true, failedStart, v).tryVulkan, "a failed start: Auto is OpenGL");
    check(decide(Choice::Auto, true, failedStart, v).why.find("vkCreateDevice failed") != std::string::npos,
          "a failed start: the log says why");
    check(decide(Choice::Vulkan, true, failedStart, v).tryVulkan, "a failed start: Vulkan chosen tries again");
    check(decide(Choice::Auto, true, otherVersion, v).tryVulkan, "a failed start of another version: Auto tries again");
    check(!decide(Choice::Auto, true, running, v).tryVulkan, "a start marker of this process counts");
#if CANE_RENDERER_CHOICE_POSIX
    Marker other = starting;   // (the parent process: alive)
    other.pid = static_cast<long>(getppid());
    check(decide(Choice::Auto, true, other, v).tryVulkan, "another copy still starting: Auto tries Vulkan");
    other.reason = "vkCreateDevice failed";
    check(!decide(Choice::Auto, true, other, v).tryVulkan, "another copy's failure: Auto is OpenGL");
#endif

    // the marker file
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("cane-renderer-selftest-" + std::to_string(currentProcessId()));
    fs::remove_all(dir, ec);
    const fs::path path = dir / "sub" / "vulkan-start.txt";
    check(!readMarker(path).present, "no marker file: none");
    check(writeMarker(path, v, ""), "marker written (start)");
    Marker m = readMarker(path);
    check(m.present && m.reason.empty() && m.version == v && m.pid == currentProcessId(), "marker read back (start)");
    check(writeMarker(path, v, "Vulkan error: the GPU device was lost"), "marker written (reason)");
    m = readMarker(path);
    check(m.present && m.reason == "Vulkan error: the GPU device was lost", "marker read back (reason)");
    check(!decide(Choice::Auto, true, m, v).tryVulkan, "marker file: Auto is OpenGL");
    check(!fs::exists(fs::path(path).concat(".tmp")), "no temporary file left");
    clearMarker(path);
    check(!readMarker(path).present, "marker cleared");
    clearMarker(path);   // (twice: no error)
    fs::remove_all(dir, ec);

    // the settings' spellings (Settings.ini, --renderer, SLY2N_RENDERER)
    if (parse) {
        check(parse("Auto") == 0 && parse("auto") == 0 && parse("Auto (Vulkan, OpenGL fallback)") == 0, "parse Auto");
        check(parse("Vulkan") == 1 && parse("vk") == 1, "parse Vulkan");
        check(parse("OpenGL") == 2 && parse("open-gl") == 2 && parse("GL") == 2, "parse OpenGL");
        check(parse("metal") == -1 && parse("") == -1, "parse rejects others");
    }
    std::printf("renderer selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

}  // namespace cane::renderer

#endif  // CANE_PLATFORM_RENDERER_CHOICE_TEST_H_
