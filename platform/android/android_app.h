#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Android activities use separate private application processes for the game,
// the extractor and Sly 2's tools. No executable is written to or run from the
// application data directory. Commands and events retain the desktop pipe ABI.
namespace cane::android {
struct ChildProcess {
    int inputFd = -1;
    int outputFd = -1;
    std::int64_t handle = 0;
};
ChildProcess startProcess(const std::vector<std::string>& arguments);
bool processRunning(std::int64_t handle);
int processExitCode(std::int64_t handle);
void cancelProcess(std::int64_t handle);
std::string filesDirectory();
std::string nativeLibraryDirectory();
void showGame();
void showLauncher();
// Called on the SDL owner thread before closing FAudio. Background audio must
// resume its buffer callbacks so SDL can join its mixing thread during teardown.
void prepareRuntimeShutdown();
// Permanent for this process: a session must save and end before creating a
// new runtime when Android replaces its EGL context or cannot retain it.
bool graphicsContextLost();
void reportExit(int code);
void requestIsoSelection();
void requestModImport();
struct ImportStatus {
    bool pending = false;
    double progress = 0;
    std::string path;
    std::string error;
};
ImportStatus isoSelection();
ImportStatus modImport();
void openFolder(const std::string& path);
void openUrl(const std::string& url);
// Starts another part of the app on its own (collection/collection.h spawnDetached): no arguments = the launcher, else
// the direct activity with these launcher arguments (--collection, --play GAME). False when Android refused.
bool startApp(const std::vector<std::string>& arguments);
// Asks the home screen to pin a shortcut (ShortcutManager.requestPinShortcut) that starts the direct activity with
// these launcher arguments. png: the icon. False when the home screen does not take pinned shortcuts.
bool pinShortcut(const std::string& id, const std::string& label, const std::string& png, const std::vector<std::string>& arguments);
struct TouchState {
    bool active = false;
    // GLFW_GAMEPAD_AXIS order. Released trigger axes are -1.
    std::array<float, 6> axes{0, 0, 0, 0, -1, -1};
    // GLFW_GAMEPAD_BUTTON order, including D-pad buttons.
    std::array<std::uint8_t, 15> buttons{};
};
TouchState touchState();
// Distinguish virtual-gamepad key pulses from physical keyboard activity.
bool touchInputRecently();
void setTouchControlsVisible(bool visible);
// What the on-screen pad should offer: the whole pad during play, the D-pad
// and menu keys while paused, only the skip/advance keys during movies.
enum class TouchContext : int { Gameplay = 0, Menu = 1, Cinematic = 2 };
// Cheap to call every frame: Java is only notified when the context changes.
void setTouchContext(TouchContext context);
// The device's vibrator at this strength (0..255; 0 stops it). Call it every frame a motor runs: the strength is
// held for a quarter of a second after the last call.
void vibrate(int amplitude);
// Asks the display for the game's frame rate (glfw_sdl.cpp). Cheap per frame.
void setPresentationRate(int framesPerSecond);
// A frame-rate ceiling while Android reports severe thermal status (API 29+),
// or 0. The user's saved frame-rate choice is never changed.
int thermalFrameCap();
} // namespace cane::android
