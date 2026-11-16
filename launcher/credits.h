#pragma once
// The full credits: one list that both the SafeHouse launcher's About page (ui/shell.h aboutPage) and the Collection
// Style Launcher's Credits panel (collection/collection.h drawCredits) show, so the two never differ.
//
// Every heading and text is shown through tr(): the ones marked N_() are translated (launcher/lang), the rest (names,
// links, licences, the libraries' list) are not in the language files and stay as written. Each section's id is also
// its layout label ("credits-<id>", --dump-layout) for scripts/check-launcher-screenshots.py and
// scripts/test-collection-launcher.py.
#include <cstdint>
#include <string>
#include <vector>

#include "brand.h"
#include "l10n.h"

namespace cane::credits {
// The developers (the Collection menu's Developers page and the About page): their own words for what they did; the
// names stay as written. Pictures: launcher/assets/developers/<picture>.png (and <picture>-silhouette.png), made by
// tools/make-developer-pictures.py from the pictures their owners supplied.
struct Developer {
    const char* name;
    const char* role;
    const char* picture;
    uint32_t colour;  // the Collection menu's disc colour when chosen (the PS3 menu's four, Tuning.xmb Game.PlayerColor)
};
inline constexpr Developer kDevelopers[5] = {
    {"FridiNaTor", N_("Main dev of the Sly 2 and Sly 3 ports and maintainer"), "fridinator", 0x4288FE},
    {"theclub654", N_("Main dev of Sly 1 via Project Cane. Special Thanks!"), "theclub654", 0x8E5E87},
    {"menaman123", N_("Contributor and macOS port"), "menaman123", 0x57A95F},
    {"thematuu", N_("Contributor"), "thematuu", 0xEBC113},
    {"NaGaa95", N_("Contributor and Switch and Windows for ARM port"), "nagaa95", 0x4288FE},
};

// The decompilations the Sly 2 and Sly 3 ports are built with.
inline constexpr const char* sly2DecompUrl = "https://github.com/Komasia/sly2-decomp";
inline constexpr const char* sly3DecompUrl = "https://github.com/NaGaa95/sly3-decomp";
inline constexpr const char* projectCaneUrl = "https://github.com/theclub654/ProjectCane";

struct Line {
    enum Kind {
        Text,       // a paragraph
        Link,       // text = the label, url = where it goes (shown beside it, opened when chosen)
        Developers, // the five developers (kDevelopers): pictures on the About page, "name: role" lines in the menu
    };
    Kind kind;
    const char* text = nullptr;
    const char* url = nullptr;
};
struct Section {
    const char* id;
    const char* heading;
    std::vector<Line> lines;
    // A picture beside the section on the About page: "logo" or "icon" (the launcher's own, ui::images()).
    const char* picture = nullptr;
};

inline const std::vector<Section>& sections() {
    static const std::vector<Section> all = {
        {"safehouse", "SafeHouse", {{Line::Text, brand::credit}}},
        {"developers", N_("Developers"), {{Line::Developers}}},
        {"pictures", N_("Developers' pictures"),
         {{Line::Text, N_("Each developer's picture is courtesy of its owner.")}, {Line::Text, "FridiNaTor, theclub654, menaman123, thematuu, NaGaa95."}}},
        {"decompilation", N_("Decompilation projects"),
         {{Line::Text, N_("The Sly 2 and Sly 3 ports are built with the help of these decompilations of the original games.")},
          {Line::Link, N_("Sly 2 decompilation"), sly2DecompUrl},
          {Line::Link, N_("Sly 3 decompilation"), sly3DecompUrl},
          {Line::Text, N_("Thanks to everyone who contributed to these projects.")}}},
        {"projectcane", N_("Sly Cooper and the Thievius Raccoonus port"),
         {{Line::Text, "ProjectCane by theclub654, which made the Sly 1 port possible"}, {Line::Link, "ProjectCane", projectCaneUrl}}},
        {"logo", N_("SafeHouse logo"), {{Line::Text, brand::logoCredit}}, "logo"},
        {"icon", N_("App icon"), {{Line::Text, brand::iconCredit}}, "icon"},
        {"steam", N_("Steam library artwork"),
         {{Line::Text, "SteamGridDB: ctwoafiveb, Benuno, AnimeFan69, Spaghetti Overlord, Pedrogon18, RealSayakaMaizono, Nuke, simenfje, FrostWolf "
                       "(share/safehouse/steam/CREDITS.md)"},
          {Line::Link, "SteamGridDB", "https://www.steamgriddb.com"}}},
        {"prompts", N_("Controller prompts"), {{Line::Text, "Kenney Input Prompts (CC0)"}, {Line::Link, "Kenney", "https://kenney.nl"}}},
        {"controllerdb", N_("Controller database"),
         {{Line::Text, "SDL_GameControllerDB (zlib)"}, {Line::Link, "SDL_GameControllerDB", "https://github.com/mdqinc/SDL_GameControllerDB"}}},
        {"typeface", N_("Typeface"), {{Line::Text, "Atkinson Hyperlegible, Braille Institute (SIL Open Font License)"}}},
        {"letters", N_("Japanese and Cyrillic letters"),
         {{Line::Text, "Noto Sans CJK JP, Noto Sans, The Noto Project Authors / Adobe (SIL Open Font License 1.1)"}}},
        {"collection", N_("Collection Style Launcher"),
         {{Line::Text, N_("The Sly Collection's (PS3) own menu: its scene, art, letters, sounds and music are read from your own disc and "
                          "stay in your profile. Nothing of it comes with SafeHouse.")}}},
        {"libraries", N_("Third-party libraries"),
         {{Line::Text, "GLFW 3.4 (zlib/libpng) · FAudio 25.02 (zlib) · SDL2 (zlib) · Dear ImGui, ImGuiFileDialog (MIT) · GLAD (MIT / public domain) · "
                       "GLM (MIT) · stb (MIT / public domain) · nlohmann/json (MIT) · PicoSHA2 1.0.1 (MIT) · miniz 3.1.2 (MIT) · LZ4 1.10.0 (BSD 2-Clause) · "
                       "libchdr (BSD 3-Clause) with the LZMA SDK (public domain), zstd (BSD) and dr_flac (public domain or MIT-0) · "
                       "FFmpeg 7.1.1 (LGPL 2.1, ffmpeg.org) · Vulkan headers and OpenXR headers and loader, The Khronos Group (Apache-2.0 / MIT) · "
                       "989snd rules after OpenGOAL's reimplementation (ISC) · AppImage type2-runtime (MIT) · NSIS · Microsoft XAudio2 redistributable"}}},
        {"community", N_("Community"), {{Line::Link, brand::discordLabel, brand::discordUrl}, {Line::Link, brand::patreonLabel, brand::patreonUrl}}},
        {"trademarks", N_("Trademarks"),
         {{Line::Text, N_("Sly Cooper, its sequels, Bentley's Hackpack and their logos are trademarks of Sony Interactive Entertainment. "
                          "The game logos identify the games only. SafeHouse is an unofficial fan project, not affiliated with or endorsed by "
                          "Sony Interactive Entertainment, Sucker Punch Productions or Sanzaru Games. Controller names and symbols belong to their owners.")}}},
        {"licences", N_("Licences"), {{Line::Text, N_("Full licence texts ship in the licenses folder and THIRD_PARTY_NOTICES.md.")}}},
    };
    return all;
}
// A link as plain text ("label: https://…"), where it cannot be opened.
inline std::string linkText(const Line& line) { return std::string(tr(line.text)) + ": " + line.url; }
} // namespace cane::credits
