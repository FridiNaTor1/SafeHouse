#pragma once
// The Switch build's runtime (platform/switch/runtime.cpp), shared by everything in SafeHouse.nro (the launcher, Sly 1,
// Sly 2): the folders on the SD card, the cores the threads run on, the first-boot installer, the error screen, and
// the hand-over from the launcher to a game and back.
//
// One program, several parts: the launcher runs first (platform/switch/launcher/launcher.cpp); the game it starts runs
// in the same process (RunGame: Startup again with the game, the installer when it is not installed, then its main);
// when the game ends, SafeHouse.nro is started again by the Homebrew loader (a clean process: the launcher).
//
// The SD card (the folder of SafeHouse.nro, normally sdmc:/switch/SafeHouse):
//   SafeHouse.nro          the program (the one the Homebrew Menu lists)
//   sly1/, sly2/, sly3/    the installed games (the installers write them)
//   user/sly1/, user/sly2/ saves and settings
#include <string>

namespace CaneSwitch
{
struct InstallJob;

// A graphics backend: the OpenGL implementation the screen runs on (glfw_switch.cpp starts the window through the
// current one). The shared one is platform/switch/deko/gl_deko.h (the launcher, the installers, Sly 2); a game can
// bring its own (Sly 1: Project Cane's deko3d backend, platform/switch/sly1/deko).
struct Graphics
{
    bool (*initialize)(unsigned width, unsigned height);
    void (*shutdown)();
    void (*resizeOutput)(unsigned width, unsigned height);
    void (*present)();
    void (*setSwapInterval)(int interval);
    void* (*procAddress)(const char* name);   // (glad's loader)
    void (*resetState)();                    // (the window handed to its next user; nullptr: none)
};

// What a part of the program is to the runtime: the launcher (no installation, no run) or a game. Each game exports
// one (Sly1-Release/main.cpp, Sly2-Release/main.cpp: cane_game_sly1, cane_game_sly2), the only global symbol of its
// object in SafeHouse.nro (cmake/Switch.cmake).
struct Game
{
    const char* key = "";                             // "sly1", "sly2", "safehouse": the folders
    const char* title = "";                           // "Sly 2: Band of Thieves"
    bool (*installed)(const std::string& data) = nullptr;   // the installation's check (nullptr: nothing to install)
    const InstallJob* install = nullptr;              // the installer (with installed)
    int (*run)(int argc, char** argv) = nullptr;      // the game's own main (its PC command line: --data-dir, --user-dir)
    const Graphics* graphics = nullptr;               // its own graphics backend (nullptr: the shared one)
};

// (glfw_switch.cpp) The backend the next window starts on: a different one first closes the current window and its
// backend entirely (the screen is the new backend's). nullptr: the shared one.
void UseGraphics(const Graphics* graphics);
// The window and its backend closed (before the error screen: the console takes the screen).
void ReleaseGraphics();

struct Paths
{
    std::string self;       // SafeHouse.nro (argv[0]; empty when the loader gave none)
    std::string base;       // its folder
    std::string data;       // the game's installed files: base/<key>
    std::string user;       // its saves and settings: base/user/<key>
};
const Paths& GetPaths();

// The start of a part: the folders and the shader set; the first call of the process also the cores, the language,
// the system checks, SaltyNX and the file worker. A game is installed first when it is not. False: this part must end
// (an error was shown, the player quit the installer, or the program is restarting into the game).
bool Startup(int& argc, char**& argv, const Game& game);
// A game, from its Startup to its end, in this process: its main with the installation and the user folder, an error
// screen when it fails; then Shutdown (back to the launcher). Returns the game's exit code.
int RunGame(const Game& game, int argc, char** argv);
// The end of the process: after a game, the launcher next (SafeHouse.nro again, unless the game chose its next program).
void Shutdown();
// A full-screen text message (the console), until + is pressed. Safe once the graphics are shut down.
void ErrorScreen(const char* message);

// The next program, once this one ends (the Homebrew loader's next load). False when the loader cannot (started
// another way, nxlink).
bool ChainLoad(const std::string& nro, const std::string& arguments);
// SafeHouse.nro again, straight into the running game (--game <key>): after its installation, a clean process.
bool RestartSelf();

// The CPU boost mode (the system's loading profile, as retail games use it: CPU 1785 MHz, the GPU down to 76.8 MHz)
// around the loads that draw nothing meanwhile: the boot, a world's load, a skipped cutscene's fast-forward, the
// installation. Reference counted: the outermost scope switches it.
void BeginLoadingBoost();
void EndLoadingBoost();
struct LoadingBoost
{
    LoadingBoost() { BeginLoadingBoost(); }
    ~LoadingBoost() { EndLoadingBoost(); }
    LoadingBoost(const LoadingBoost&) = delete;
    LoadingBoost& operator=(const LoadingBoost&) = delete;
};

// The cores: the game thread on core 0; the audio mixer and the sound ticks on core 2, file work on core 1; the
// other threads share 1 and 2.
void PinGameThread();
void PinSoundThread();
int MixerCore();
int IoCore();
// The console's mode: 1920x1080 docked, 1280x720 handheld.
bool Docked();
}  // namespace CaneSwitch
