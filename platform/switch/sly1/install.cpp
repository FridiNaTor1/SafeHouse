// Sly 1's first-boot installer on the Switch (platform/switch/installer.h): any Sly 1 disc (USA, Europe, Japan, Korea)
// extracted on the console (platform/installer/iso_extractor.cpp, on the Sly1/ submodule's level tools: sly1_pcbrx),
// and an installation with older levels rebuilt in place. Linked beside the launcher, outside Sly 1's game object
// (game.cpp names it), so the extractor's exceptions and the level tools' classes stay apart from the game's.
#include "../installer.h"
#include "../../games.h"
#include "../../install.h"
#include "../../installer/iso_extractor.h"

#include <functional>
#include <string>
#include <vector>

namespace
{
CaneExtract::ProgressCallback Forward(const CaneSwitch::InstallProgress& progress)
{
    return [&progress](const CaneExtract::Progress& p) {
        return progress(p.total ? float(p.completed) / float(p.total) : 0.0f, p.stage, p.file);
    };
}
std::string Outcome(const std::function<void()>& run)
{
    try { run(); }
    catch (const CaneExtract::Cancelled&) { return "cancelled"; }
    catch (const std::exception& e) { return e.what(); }
    return {};
}
}

extern const CaneSwitch::InstallJob g_caneSly1Install;
const CaneSwitch::InstallJob g_caneSly1Install = {
    int(cane::GameId::Sly1),
    "Select a Sly Cooper ISO or CHD (USA, Europe, Japan or Korea).",
    [](const std::string& iso, const std::string& out, const std::vector<std::string>&, const CaneSwitch::InstallProgress& progress) {
        std::string error = Outcome([&] { CaneExtract::ExtractGame(iso, out, Forward(progress)); });
        if (error.empty() && !CaneExtract::IsReady(out)) error = "Some game files are missing. Please run the extraction again.";
        return error;
    },
    [](const std::string& data) { return CaneExtract::NeedsRebuild(data); },
    [](const std::string& data, const CaneSwitch::InstallProgress& progress) {
        return Outcome([&] { CaneExtract::RebuildGame(data, Forward(progress)); });
    },
    false,
    false,
    // (the PC extractor's figures: install.h sly1NeedBytes)
    [](const std::string& bootFile, std::size_t) -> uint64_t {
        const cane::Sly1Disc* disc = cane::sly1DiscFromBootFile(bootFile);
        return disc ? cane::sly1NeedBytes(*disc) : 0;
    },
};

bool CaneSly1Installed(const std::string& data) { return CaneExtract::IsReady(data); }
