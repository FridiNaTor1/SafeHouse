#pragma once
// Product identity shared by the launcher, packaging and tests.
// The product was called ProjectCane until 2026-11-03. Its data folders keep the old name on purpose
// (~/.config/projectcane, ~/.local/share/projectcane, %LOCALAPPDATA%\ProjectCane; platform/install.h userPaths()),
// so existing installations, saves and settings keep working without a migration.
namespace cane::brand {
inline constexpr const char* appName = "SafeHouse";
// Wayland app_id == .desktop basename == hicolor icon name.
inline constexpr const char* appId = "safehouse";
// X11 WM_CLASS == StartupWMClass.
inline constexpr const char* wmClass = "SafeHouse";
inline constexpr const char* windowTitle = "SafeHouse Launcher";
inline constexpr const char* launcherDescription = "SafeHouse Launcher";
// The launcher program (bin/safehouse[.exe]); "cane-launcher" before 2026-11-03.
inline constexpr const char* launcherProgram = "safehouse";
// Hidden test hook, never documented in --help:
// ps|playstation|xbox|nintendo|switch|steamdeck|steamcontroller|steamcontroller2026|ps4|ps5|keyboard
inline constexpr const char* testPromptFamilyEnv = "PROJECTCANE_TEST_PROMPT_FAMILY";
inline constexpr const char* credit =
    "SafeHouse is a native PC port of the Sly Cooper games. It plays from your own disc images and "
    "includes no original game data.";
// The SafeHouse logo (the mask and cane emblem, launcher/assets/safehouse-logo.png) and the app icon (the cane) are by
// the same artist.
inline constexpr const char* logoCredit = "HoshiYumeHime (DeviantArt)";
inline constexpr const char* iconCredit = "HoshiYumeHime (DeviantArt)";
// The Sly 1 port this project grew out of (README.md).
inline constexpr const char* projectCaneCredit = "ProjectCane by theclub654, which made the Sly 1 port possible · "
                                                 "github.com/theclub654/ProjectCane";
// Community links (the launcher's rail and About page). ThiefRush is the community's Discord server and the
// name of the upcoming multiplayer fork.
inline constexpr const char* discordUrl = "https://discord.com/invite/SfvzvMg7Mk";
inline constexpr const char* discordLabel = "Join us on Discord (ThiefRush)";
inline constexpr const char* patreonUrl = "https://www.patreon.com/cw/thiefrush";
inline constexpr const char* patreonLabel = "Support us on Patreon";
// Sly 3 (game page banner, the first Play confirmation and About).
inline constexpr const char* sly3Notice = "Early development: not fully playable. Expect missing features and crashes.";
} // namespace cane::brand
