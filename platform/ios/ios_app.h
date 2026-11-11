#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The iOS app (cmake/ios.cmake, notes/IOS_2026-11-11.md): one process. The launcher, the extractor and Sly 2 are
// frameworks inside SafeHouse.app; this bridge (SafeHouseCore.framework, ios_app.mm) is shared by all of them. It
// runs a program in the app's own process in place of a child process: the program's stdout / stderr still reach
// the caller through a pipe (platform/process.h), as on the desktop and on Android (android/android_app.h), whose
// interface this follows.
namespace cane::ios {
struct ChildProcess {
    int inputFd = -1;
    int outputFd = -1;
    std::int64_t handle = 0;
};
// arguments[0]: the program's framework binary (launcher/backend.h executableName). Tools and the extractor run on a
// worker thread; the game waits for the main thread (runPendingProgram).
ChildProcess startProcess(const std::vector<std::string>& arguments);
bool processRunning(std::int64_t handle);
int processExitCode(std::int64_t handle);
// An in-process program cannot be stopped from outside: it finishes, then the caller sees the cancel.
void cancelProcess(std::int64_t handle);
// (main thread; the launcher's glfwPollEvents / glfwWaitEventsTimeout) runs a program that needs the main thread
// (the game) when one is waiting. True when one ran.
bool runPendingProgram();
bool programWaiting();
// The program's framework binary for a program name ("Sly2-Release", "cane-extractor"), inside the app bundle.
std::string programPath(const std::string& name);
// SafeHouse.app
std::string bundleDirectory();
// The app's Documents folder: the profile (installations, saves, settings, logs), shown in the Files app.
std::string documentsDirectory();
// Documents/imports: the disc images copied in through the document picker.
std::string importsDirectory();
void openFolder(const std::string& path);
void openUrl(const std::string& url);
// The system document picker for a disc image (.iso / .chd). The chosen file is copied into importsDirectory().
void requestIsoSelection();
struct ImportStatus {
    bool pending = false;
    double progress = 0;
    std::string path;
    std::string error;
};
ImportStatus isoSelection();
// The on-screen gamepad (touch_overlay.mm, platform/touch_layout.h), as Android's.
struct TouchState {
    bool active = false;
    // GLFW_GAMEPAD_AXIS order. Released trigger axes are -1.
    std::array<float, 6> axes{0, 0, 0, 0, -1, -1};
    // GLFW_GAMEPAD_BUTTON order, including D-pad buttons.
    std::array<std::uint8_t, 15> buttons{};
};
TouchState touchState();
bool touchInputRecently();
void setTouchControlsVisible(bool visible);
enum class TouchContext : int { Gameplay = 0, Menu = 1, Cinematic = 2 };
void setTouchContext(TouchContext context);
// Puts the touch overlay over this UIWindow (the game's SDL window; nullptr removes it). (glfw_sdl_ios.cpp)
void attachTouchOverlay(void* uiWindow);
// (main thread) Puts a window SDL made into the app's window scene (the UIScene life cycle; SDL 2.30 knows no scenes)
// and sizes it to the scene. Waits up to 5 s for the scene to connect at the app's start.
void adoptWindow(void* uiWindow);
// (main thread) The window's safe area insets in points: left, top, right, bottom (the notch / Dynamic Island, the
// home indicator, a foldable's hinge area). Zero without a window.
std::array<float, 4> safeAreaInsets(void* uiWindow);
// (main thread) The orientations SDL may rotate to (SDL_HINT_ORIENTATIONS): landscape on a phone-shaped screen; every
// orientation on an iPad and on a near-square screen (a foldable's inner screen), where a locked orientation makes iOS
// show the app in a compatibility frame.
const char* orientations(void* uiWindow);
// (main thread) One log line about the screen and the window: sizes, scale, safe area, orientation, and whether
// iOS shows the app in a compatibility frame (window smaller than the screen).
std::string displayReport(void* uiWindow);
// A haptic tap at this strength (0..255; 0: nothing). Call it every frame a motor runs.
void vibrate(int amplitude);
// (the app's main, platform/ios/main.mm) the folders, the log, controller notifications; --ios-smoke / --ios-selftest
void initialize(int argc, char** argv);
// Runs a program here, on this thread, with its output on the app's own stdout / stderr (main.mm). Its exit code.
int runProgramNow(const std::vector<std::string>& arguments);
// Smoke test (CI): set by --ios-smoke; the launcher's shim saves a screenshot after a few frames and closes.
bool smokeTest();
// A line for the app's own log (Documents/logs/ios.log) and the console.
void log(const std::string& line);
}  // namespace cane::ios
