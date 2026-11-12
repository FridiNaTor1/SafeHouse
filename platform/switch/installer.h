#pragma once
// The first-boot installer (installer.cpp): every game's, with ProjectCane's screens (platform/installer/view.h) over
// the Switch UI host (switch_ui.h). It lists the SD card's folders and disc images (.iso, .chd, a split .iso:
// platform/disc_image.h), checks the image chosen is the game's (platform/games.h), lets the player choose the
// languages of a disc that has several, runs the game's own conversion on a worker thread into <data>.partial and
// renames that to the game's data folder at the end, so an interrupted install never looks installed.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace CaneSwitch
{
// The conversion's progress: the fraction done, what it is doing; false when the player cancelled.
using InstallProgress = std::function<bool(float fraction, const std::string& stage, const std::string& file)>;

// A language of the disc: the game's name for it ("French") and its own ("Français").
struct InstallLanguage
{
    std::string id, name;
};

struct InstallJob
{
    int game = 0;                   // the cane::GameId the disc image must be (games.h)
    std::string hint;               // the browse screen's line ("Select a Sly 2 ISO (USA, Europe or Japan).")
    // Converts the disc image into `directory` (created, empty), with the languages chosen (their ids; empty: the disc
    // has no choice): "" when done, "cancelled", or what went wrong.
    std::function<std::string(const std::string& iso, const std::string& directory, const std::vector<std::string>& languages,
                              const InstallProgress& progress)> convert;
    // (optional) an installation whose files need updating in place, and the update (ProjectCane's level rebuild)
    std::function<bool(const std::string& directory)> needsUpdate;
    std::function<std::string(const std::string& directory, const InstallProgress& progress)> update;
    bool restart = true;            // start the program again after installing (a clean process for the game)
    bool readsImage = false;        // convert reads a CHD or a split .iso itself (else the installer writes out an .iso first)
    // (optional) the free space the installation needs for the disc with this boot file ("SCES_525.29") and that many
    // languages chosen (0: no choice); 0 or none: the image's size and 512 MB
    std::function<uint64_t(const std::string& bootFile, std::size_t languages)> needBytes;
    // (optional) the disc image's languages when the player chooses those to install (none: every one), in the disc's
    // order, and the one to start with (the console's)
    std::function<std::vector<InstallLanguage>(const std::string& iso, std::size_t* preferred)> languages;
};

// The installer, for the game whose data folder is GetPaths().data. True: the game starts in this process; false: the
// program ends (the player quit, it failed after showing why, or it restarts itself into the game).
bool RunInstaller(const InstallJob& job);
}  // namespace CaneSwitch
