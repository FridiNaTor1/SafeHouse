// Sly 1 in SafeHouse.nro: the game's descriptor, the only global symbol of its object (cmake/Switch.cmake). The game is
// the Sly1/ submodule's (its main as Sly1Main), drawn by its own deko3d backend (deko/backend.h); its installer is
// outside the object (install.cpp, beside the launcher).
#include "../runtime.h"
#include "deko/backend.h"

#include <string>

int Sly1Main(int argc, char* argv[]);   // Sly1-Release/main.cpp (main, renamed by cmake/Switch.cmake)

// install.cpp
namespace CaneSwitch { struct InstallJob; }
extern const CaneSwitch::InstallJob g_caneSly1Install;
bool CaneSly1Installed(const std::string& data);

namespace
{
const CaneSwitch::Graphics s_graphics = {CaneDeko::Initialize, CaneDeko::Shutdown, CaneDeko::ResizeOutput, CaneDeko::Present,
                                         CaneDeko::SetSwapInterval, CaneDeko::ProcAddress, nullptr};
}

extern "C" const CaneSwitch::Game cane_game_sly1;
const CaneSwitch::Game cane_game_sly1 = {"sly1", "Sly Cooper and the Thievius Raccoonus", CaneSly1Installed, &g_caneSly1Install,
                                         Sly1Main, &s_graphics};
