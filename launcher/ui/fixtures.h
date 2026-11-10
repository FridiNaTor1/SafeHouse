#pragma once
// Fake data for the hidden screenshot hook (--screenshot). Nothing here is
// used in normal runs.
#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include "games.h"
#include "iso_scan.h"

namespace cane::ui::fixtures {
struct Options {
    // game | install | install-progress | settings[:category] | about[:section] | desktop-shortcut[-done] | sly3-notice
    std::string screen = "game";
    std::optional<GameId> game; // --game KEY (default: the remembered game)
    int width = 1280, height = 800;
    bool installed = false;     // --installed-fixture: Sly 1 installed
    bool sly2Installed = false; // --sly2-installed-fixture: Sly 2 files installed
    bool sly2Runtime = false;   // --sly2-runtime-fixture: Sly2-Release present
    std::string layoutPath;     // --dump-layout
    bool live = false;          // --live: read the (private) profile instead of fake states
    std::string iso;            // --iso PATH: the disc image the install page opens with (live)
    double delay = 0;           // --delay SECONDS: wait (e.g. for the disc image search) before the shot
    // --nav up,down,left,right,a,b,x,y,lb,rb,lt,rt,start,select: pad presses
    // replayed one every 4 frames before the screenshot.
    std::vector<std::string> nav;
    // --pointer "d640,300 m400,300 w u": a finger replayed after the nav, one step every 2 frames: d = down at x,y,
    // m = move to, w = wait, u = up (the Collection Style Launcher's touch checks).
    std::vector<std::string> pointer;
    // --collection: the Collection Style Launcher instead (collection/collection.h), with --collection-games (the games
    // shown installed) and --collection-ps3 (a profile holding the imported menu, Collection/Menu; also what Settings >
    // Launcher shows as imported).
    bool collection = false;
    std::vector<std::string> collectionGames;
    std::string collectionPs3;
    // --language CODE: the launcher language of the shot (default English; "system" follows the environment).
    std::string language = "en";
};

inline IsoCandidate iso(const char* file, uintmax_t size, int hoursAgo, GameId game, const char* boot, const char* region) {
    IsoCandidate c;
    c.path = file;
    c.size = size;
    c.modified = std::filesystem::file_time_type::clock::now() - std::chrono::hours(hoursAgo);
    c.identity.kind = installable(game) ? DiscIdentity::Kind::Supported : DiscIdentity::Kind::NoPort;
    c.identity.game = game;
    c.identity.bootFile = boot;
    c.identity.label = std::string(cane::game(game).title) + " (" + region + ")";
    return c;
}
inline std::vector<IsoCandidate> isos() {
    return {
        iso("/home/player/Emulation/roms/ps2/Sly Cooper and the Thievius Raccoonus (USA).iso", 2'802'319'360ull, 1, GameId::Sly1,
            "SCUS_971.98", "USA"),
        iso("/home/player/Emulation/roms/ps2/Sly 2 - Band of Thieves (USA).iso", 2'884'141'056ull, 2, GameId::Sly2, "SCUS_973.16", "USA"),
        iso("/home/player/Emulation/roms/ps2/Sly 3 - Honor Among Thieves (USA).iso", 3'553'755'136ull, 3, GameId::Sly3, "SCUS_974.64",
            "USA"),
    };
}
} // namespace cane::ui::fixtures
