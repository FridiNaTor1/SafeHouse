// See runtime.h.
#include "runtime.h"
#include "core_policy.h"
#include "installer.h"
#include "io_worker.h"
#include "saltynx.h"
#include "deko/gl_deko.h"

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
// The launcher's and the installers' handheld configuration (CPU 1020 MHz, GPU 384 MHz, memory 1331 MHz), one of the
// system's own profiles; docked keeps the system's default. A game switches to kGameHandheldConfiguration (RunGame).
u32 __nx_applet_PerformanceConfiguration[2] = {0x00020004, 0};
}

namespace CaneSwitch
{
namespace
{
Game g_game;
Paths g_paths;
int g_boostDepth = 0;
bool g_boostActive = false;
bool g_nextLoadSet = false;
bool g_started = false;
u64 g_allowedCores = 0x7;
bool g_gameConfiguration = false;

// A game's handheld configuration (CPU 1020 MHz, GPU 460.8 MHz, memory 1331 MHz), the system profile retail games use
// for their handheld boost. It is set for the Normal (handheld) mode only, so docking and undocking need nothing.
constexpr u32 kGameHandheldConfiguration = 0x92220008;
constexpr u32 kLauncherHandheldConfiguration = 0x00020004;   // (__nx_applet_PerformanceConfiguration's)
void SetGameConfiguration(bool game)
{
    if (game == g_gameConfiguration) return;
    const u32 configuration = game ? kGameHandheldConfiguration : kLauncherHandheldConfiguration;
    if (R_SUCCEEDED(apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, configuration))) g_gameConfiguration = game;
}

bool IsDirectory(const std::string& p)
{
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool IsFile(const std::string& p)
{
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
void MakeDirectories(const std::string& p)
{
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
}

// The console's language as the PC builds read the system's (LANG), so a European disc starts in the player's
// language.
void SetLanguageFromSystem()
{
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    if (R_FAILED(setInitialize())) return;
    if (R_SUCCEEDED(setGetSystemLanguage(&code))) setMakeLanguage(code, &language);
    setExit();
    const char* iso = "en";
    switch (language)
    {
    case SetLanguage_JA: iso = "ja"; break;
    case SetLanguage_FR: case SetLanguage_FRCA: iso = "fr"; break;
    case SetLanguage_DE: iso = "de"; break;
    case SetLanguage_IT: iso = "it"; break;
    case SetLanguage_ES: case SetLanguage_ES419: iso = "es"; break;
    case SetLanguage_NL: iso = "nl"; break;
    case SetLanguage_PT: case SetLanguage_PTBR: iso = "pt"; break;
    case SetLanguage_KO: iso = "ko"; break;
    case SetLanguage_ZHCN: case SetLanguage_ZHTW: case SetLanguage_ZHHANS: case SetLanguage_ZHHANT: iso = "zh"; break;
    case SetLanguage_RU: iso = "ru"; break;
    default: break;
    }
    setenv("LANG", iso, 1);
}

// Every pthread (std::thread, FFmpeg's, std::async) starts in ThreadEntry, on cores 1-2, leaving core 0 to the game.
struct Trampoline
{
    void* (*func)(void*);
    void* arg;
};
std::atomic<unsigned> g_threadCount{0};
void* ThreadEntry(void* p)
{
    const Trampoline t = *static_cast<Trampoline*>(p);
    delete static_cast<Trampoline*>(p);
    const u64 mask = g_allowedCores & 0x6;
    if (mask)
    {
        const unsigned n = g_threadCount.fetch_add(1);
        int ideal = (n & 1) ? 2 : 1;
        if (!(mask & (1u << ideal))) ideal = (mask & 2) ? 1 : 2;
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, mask);
    }
    return t.func(t.arg);
}
}  // namespace

const Paths& GetPaths() { return g_paths; }

bool Docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

void PinGameThread()
{
    if (g_allowedCores & 1) svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1);
}
void PinSoundThread()
{
    const u64 mask = g_allowedCores & 0x6;
    if (mask) svcSetThreadCoreMask(CUR_THREAD_HANDLE, (mask & 4) ? 2 : 1, mask);
}
int MixerCore() { return (g_allowedCores & 4) ? 2 : (g_allowedCores & 2) ? 1 : -2; }
int IoCore() { return (g_allowedCores & 2) ? 1 : (g_allowedCores & 4) ? 2 : -2; }

void BeginLoadingBoost()
{
    if (g_boostDepth++ != 0) return;
    g_boostActive = R_SUCCEEDED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad));
}
void EndLoadingBoost()
{
    if (g_boostDepth > 0 && --g_boostDepth == 0 && g_boostActive)
    {
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        g_boostActive = false;
    }
}

void ErrorScreen(const char* message)
{
    // (the deko3d backends set the window's flip for their lower-left origin, and it outlives their swapchains: the
    // console's text would show upside down)
    nwindowSetTransform(nwindowGetDefault(), 0);
    consoleInit(nullptr);
    std::printf("\x1b[2J\x1b[1;1HSafeHouse - %s\n\n%s\n\nPress + to leave.\n", g_game.title, message);
    PadState pad;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&pad);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

bool ChainLoad(const std::string& nro, const std::string& arguments)
{
    if (!envHasNextLoad()) return false;
    const std::string argv = "\"" + nro + "\"" + (arguments.empty() ? "" : " " + arguments);
    if (R_FAILED(envSetNextLoad(nro.c_str(), argv.c_str()))) return false;
    g_nextLoadSet = true;
    return true;
}
bool RestartSelf()
{
    return !g_paths.self.empty() && ChainLoad(g_paths.self, g_game.run ? std::string("--game ") + g_game.key : std::string());
}

bool Startup(int& argc, char**& argv, const Game& game)
{
    // (the process's first part, the launcher or a game started with --game; a game the launcher starts comes second)
    const bool first = !g_started;
    g_game = game;
    if (first)
    {
        // the folder: the .nro's own when it was started from the SD card, else sdmc:/switch/SafeHouse
        std::string base = "sdmc:/switch/SafeHouse";
        if (argc > 0 && argv[0] && std::strncmp(argv[0], "sdmc:/", 6) == 0)
        {
            const std::string self = argv[0];
            const size_t slash = self.find_last_of('/');
            if (slash != std::string::npos && slash > 5 && IsDirectory(self.substr(0, slash))) base = self.substr(0, slash);
            if (IsFile(self)) g_paths.self = self;
        }
        g_paths.base = base;
    }
    g_paths.data = g_paths.base + "/" + game.key;
    g_paths.user = g_paths.base + "/user/" + game.key;
    if (first)
    {
        u64 cores = 0;
        if (R_SUCCEEDED(svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && (cores & 7)) g_allowedCores = cores & 7;
        PinGameThread();
        SetLanguageFromSystem();
    }
    CaneDeko::SetShaderSet(game.key);
    g_started = true;

    if (first)
    {
        const AppletType type = appletGetAppletType();
        if (type != AppletType_Application && type != AppletType_SystemApplication)
        {
            ErrorScreen("SafeHouse needs the full memory of a game: start the Homebrew Menu by holding R while launching a game\n"
                        "(title takeover), not from the Album.");
            return false;
        }
        saltynx_start();
        CaneIo::Initialize();
    }
    if (game.installed)
    {
        MakeDirectories(g_paths.user);
        if (!game.installed(g_paths.data))
        {
            if (!game.install)
            {
                ErrorScreen("The game is not installed.");
                return false;
            }
            if (!RunInstaller(*game.install)) return false;
        }
    }
    return true;
}

int RunGame(const Game& game, int argc, char** argv)
{
    // (a game without its main yet, Sly 3: its installer only)
    if (!Startup(argc, argv, game) || !game.run)
    {
        ReleaseGraphics();   // (the installer's screen, when it ran: the window back to the system)
        Shutdown();
        return 0;
    }
    // (a game with its own graphics backend: the launcher's or the installer's window closed, the screen its own)
    UseGraphics(game.graphics);
    SetGameConfiguration(true);
    // the game as the PC launcher starts it: the installation and the user folder
    std::vector<std::string> args = {argc > 0 && argv[0] ? argv[0] : game.key, "--data-dir", g_paths.data, "--user-dir", g_paths.user};
    std::vector<char*> av;
    for (std::string& a : args) av.push_back(a.data());
    av.push_back(nullptr);
    int rc = 1;
    std::string failure;
    try
    {
        rc = game.run(int(args.size()), av.data());
        if (rc != 0) failure = "The game stopped with an error (code " + std::to_string(rc) + ").";
    }
    catch (const std::exception& e) { failure = std::string("The game stopped: ") + e.what(); }
    catch (...) { failure = "The game stopped: an unknown error."; }
    // (a game returning early, as on an error, still has its window and its audio)
    ReleaseGraphics();
    StopAudio();
    if (!failure.empty()) ErrorScreen(failure.c_str());
    Shutdown();
    return rc;
}

void Shutdown()
{
    StopAudio();
    EndLoadingBoost();
    SetGameConfiguration(false);   // (the next program, the launcher or the Homebrew Menu, with the launcher's configuration)
    saltynx_stop();
    CaneIo::Shutdown();
    // after a game: the launcher, SafeHouse.nro again with the game selected (unless the game chose its next program)
    if (g_started && g_game.installed && !g_nextLoadSet && !g_paths.self.empty()) ChainLoad(g_paths.self, std::string("--from ") + g_game.key);
}
}  // namespace CaneSwitch

// ProjectCane's helper threads (core_policy.h): the runtime's cores
namespace CaneThreads
{
int IoCore() { return CaneSwitch::IoCore(); }
}

// getenv, cached per thread by the name's address and text: the games read their environment switches from hot code,
// and newlib locks and scans the whole environment on every call. setenv, unsetenv and putenv are wrapped too (--wrap
// in cmake/Switch.cmake) and invalidate the cache, so a cached answer is always the one newlib would give.
namespace
{
std::atomic<uint32_t> g_envVersion{1};
struct EnvCacheEntry
{
    const char* key;
    uint32_t version;
    char* value;
    char name[44];
};
constexpr unsigned kEnvCacheEntries = 256;
thread_local EnvCacheEntry* t_envCache = nullptr;
}  // namespace
extern "C" char* __real_getenv(const char* name);
extern "C" int __real_setenv(const char* name, const char* value, int overwrite);
extern "C" int __real_unsetenv(const char* name);
extern "C" int __real_putenv(char* string);
extern "C" char* __wrap_getenv(const char* name)
{
    const size_t length = name ? std::strlen(name) : sizeof(EnvCacheEntry::name);
    if (length >= sizeof(EnvCacheEntry::name)) return __real_getenv(name);
    if (!t_envCache && !(t_envCache = new (std::nothrow) EnvCacheEntry[kEnvCacheEntries]())) return __real_getenv(name);
    const uint32_t version = g_envVersion.load(std::memory_order_acquire);
    EnvCacheEntry& e = t_envCache[(reinterpret_cast<uintptr_t>(name) >> 2) % kEnvCacheEntries];
    if (e.key == name && e.version == version && std::memcmp(e.name, name, length + 1) == 0) return e.value;
    char* value = __real_getenv(name);
    e.key = name;
    e.version = version;
    e.value = value;
    std::memcpy(e.name, name, length + 1);
    return value;
}
extern "C" int __wrap_setenv(const char* name, const char* value, int overwrite)
{
    const int r = __real_setenv(name, value, overwrite);
    g_envVersion.fetch_add(1, std::memory_order_release);
    return r;
}
extern "C" int __wrap_unsetenv(const char* name)
{
    const int r = __real_unsetenv(name);
    g_envVersion.fetch_add(1, std::memory_order_release);
    return r;
}
extern "C" int __wrap_putenv(char* string)
{
    const int r = __real_putenv(string);
    g_envVersion.fetch_add(1, std::memory_order_release);
    return r;
}

// Every pthread starts through CaneSwitch::ThreadEntry, which sets its cores (--wrap in cmake/Switch.cmake).
extern "C" int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr, size_t stack_size);
extern "C" int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr, size_t stack_size)
{
    auto* t = new CaneSwitch::Trampoline{func, arg};
    // (at least 256 KB of stack: the sound and decoder threads go deeper than libnx's default)
    if (!stack_addr && stack_size < (256u << 10)) stack_size = 256u << 10;
    const int r = __real___syscall_thread_create(thread, CaneSwitch::ThreadEntry, t, stack_addr, stack_size);
    if (r) delete t;
    return r;
}
