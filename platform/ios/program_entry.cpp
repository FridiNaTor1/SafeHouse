// The entry point of a program framework in the iOS app (cmake/ios.cmake): the app's bridge (ios_app.mm) loads the
// framework and calls cane_program_main, which runs the desktop program's main under another name (main=... at
// compile time). CANE_IOS_ENTRY / CANE_IOS_ROLE come from the target.
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>

int CANE_IOS_ENTRY(int, char**);

// (glfw_sdl_ios.cpp) which program this image is (Sly 2's: "game", also in its Sly2Tools copy)
extern "C" __attribute__((visibility("default"))) const char* cane_ios_program_role(void) {
    return CANE_IOS_ROLE;
}

extern "C" __attribute__((visibility("default"))) int cane_program_main(int argc, char** argv) {
    try {
        const int code = CANE_IOS_ENTRY(argc, argv);
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        return code;
    } catch (const std::exception& e) {
        std::cerr << CANE_IOS_ROLE << " failed: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << CANE_IOS_ROLE << " failed (unknown exception)" << std::endl;
        return 1;
    }
}
