#pragma once
// The games installed as the PC's extractor installs a disc image whole (File Extractor/main.cpp ExtractDiscImage,
// platform/install.h DiscImageInstall: Sly 3): the image kept as disc.iso, its system files beside it and
// installation.json, so the game finds the PC's layout.
#include "installer.h"
#include "../games.h"

#include <string>

namespace CaneSwitch
{
// The disc image (an .iso or a CHD) installed into `directory`: "" when done, "cancelled", or what went wrong.
std::string InstallDiscImage(cane::GameId game, const std::string& iso, const std::string& directory, const InstallProgress& progress);
// installation.json current for `game` (platform/install.h manifestCurrent) and the image of its size beside it.
bool DiscImageInstalled(cane::GameId game, const std::string& directory);
}  // namespace CaneSwitch
