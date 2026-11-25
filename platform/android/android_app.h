#pragma once

#include <array>
#include <cstddef>
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
// Why a child ended without reporting its exit code (its process crashed, Android closed it, its window was closed),
// from the child's own report or Android's record of the process (ApplicationExitInfo, API 30+); "" when it reported
// its code or nothing is known. A crash reads "crash: <program> crashed: <how>" (process.h CrashWatch), anything else
// "exit reason: <how>". Call before processExitCode (which forgets the child).
std::string processExitReason(std::int64_t handle);
// Android's record of how a process of this app ended (ActivityManager.getHistoricalProcessExitReasons, API 30+):
// found: a record exists; crash: it crashed (Java or native), hung (ANR) or failed to start, not closed by the system
// or the user; description: e.g. "native crash, signal 11", "low memory", "killed (signal 9)".
struct ExitRecord {
    bool found = false;
    bool crash = false;
    int reason = 0, status = 0;
    std::string description;
};
ExitRecord processExitRecord(long pid);
// An OpenXR runtime is installed (a headset: Meta Quest, Pico, ...; NativeBridge.openXrRuntimeInstalled): the runtime
// brokers' content providers, a runtime manifest in the system's etc/openxr folders, or a known headset maker. False
// on a phone, where the games then do not load the OpenXR loader at all.
bool openXrRuntimeInstalled();
void cancelProcess(std::int64_t handle);
std::string filesDirectory();
std::string nativeLibraryDirectory();
void showGame();
void showLauncher();
// (the launcher's own process: LauncherActivity or DirectActivity) its activity back to the front, e.g. when its game
// has ended (glfwShowWindow there).
void showLauncherWindow();
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
// A custom Vulkan driver package (.zip) picked with the document picker, copied into files/drivers/ (status.path; handed
// over once: the launcher unpacks it, launcher/android_drivers.h).
void requestDriverImport();
ImportStatus driverImport();
ImportStatus modImport();
void openFolder(const std::string& path);
void openUrl(const std::string& url);
// The launcher's update check (launcher/update.h): an HTTPS GET through Java (NativeBridge.httpGet). status: the HTTP
// status, or -1 server name not found, -2 no connection, -3 timed out, -4 larger than maxBytes, -5 another failure.
// The body when status is 200.
std::string httpGet(const std::string& url, bool api, const std::string& userAgent, std::size_t maxBytes, int& status);
// Storage besides the private folder (NativeBridge.externalFolders): {path, kind}; kind "primary" / "removable": the
// app's own folders on the shared storage / an SD card (no permission; removed with the app), "shared" / "card": a
// volume's root (with all files access).
std::vector<std::pair<std::string, std::string>> externalFolders();
// All files access (Android 11+) or the storage permission (6-10); requestAllFilesAccess asks for it.
bool allFilesAccess();
void requestAllFilesAccess();
// The launcher's update download: `url` into the file `path` (HTTPS; redirects followed). The HTTP status, or the
// negative codes of httpGet (-6 cancelled). *total: the announced length (-1 unknown). Blocks; downloadLength() and
// cancelDownload() may be called from another thread meanwhile.
int httpDownload(const std::string& url, const std::string& path, const std::string& userAgent, std::uint64_t* total);
std::uint64_t downloadLength();
void cancelDownload();
// Hands an APK to Android's installer (a PackageInstaller session; the system asks the player to confirm). "" when
// the installer took it, else why not (e.g. SafeHouse may not install apps yet: Android's setting opened).
std::string installPackage(const std::string& apkPath);
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
// (glfw_sdl.cpp) The game window's surface for a Vulkan renderer (a window made with GLFW_CLIENT_API = GLFW_NO_API):
// a number that changes whenever Android replaced the activity's surface (pause / resume, a new window), 0 while
// there is none or the app is in the background. Cheap per frame.
std::uint64_t windowSurfaceSerial();
// A frame-rate ceiling while Android reports severe thermal status (API 29+),
// or 0. The user's saved frame-rate choice is never changed.
int thermalFrameCap();
} // namespace cane::android
