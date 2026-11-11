// The iOS app's executable (cmake/ios.cmake): SDL's UIKit application, then the SafeHouse launcher's framework on
// the main thread. Everything else is in the frameworks (ios_app.h).
//
//   --ios-smoke      (CI) the launcher saves a screenshot of its 300th frame (Documents/smoke-launcher.png) and quits
//   --ios-selftest   (CI) Sly 2's self tests (no game data) in its tools image, then quits; Documents/selftest.log
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_main.h>

#include "ios_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
int selfTests() {
    const std::string tools = cane::ios::bundleDirectory() + "/Frameworks/Sly2Tools.framework/Sly2Tools";
    int failures = 0;
    for (const char* test : {"--rumble-selftest", "--settings-selftest", "--renderer-selftest", "--action-input-selftest", "--language-selftest"}) {
        const int code = cane::ios::runProgramNow({tools, test});
        cane::ios::log(std::string("selftest ") + test + ": " + (code == 0 ? "passed" : "FAILED (" + std::to_string(code) + ")"));
        if (code != 0) ++failures;
    }
    if (FILE* f = std::fopen((cane::ios::documentsDirectory() + "/selftest.log").c_str(), "w")) {
        std::fprintf(f, "%d failed\n", failures);
        std::fclose(f);
    }
    return failures ? 1 : 0;
}

int appMain(int argc, char** argv) {
    cane::ios::initialize(argc, argv);
    std::vector<std::string> arguments{cane::ios::programPath("safehouse")};
    bool selftest = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ios-selftest")) selftest = true;
        else if (std::strncmp(argv[i], "--ios-", 6) != 0 && std::strncmp(argv[i], "-NS", 3) != 0 && std::strncmp(argv[i], "-Apple", 6) != 0)
            arguments.push_back(argv[i]);
    }
    const int code = selftest ? selfTests() : cane::ios::runProgramNow(arguments);
    cane::ios::log("SafeHouse leaves with " + std::to_string(code));
    // (iOS apps normally never quit; the launcher's Quit and the CI runs end the process here)
    std::exit(code);
}
}  // namespace

int main(int argc, char** argv) {
    return SDL_UIKitRunApp(argc, argv, appMain);
}
