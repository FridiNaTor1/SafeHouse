// See textmod.h.
#include "textmod.h"

#include "content.h"
#include "game.h"
#include "mods.h"
#include "resp.h"
#include "transition.h"
#include "ui.h"
#include "../platform/platform.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

int PalResponseStringIndex(RESPK respk, int variant);   // (prompt.cpp)
extern const char* logoPchz;                                  // (logo.cpp)

namespace
{
constexpr std::size_t kStringCount = 190;   // (the PLOC table of every PAL / NTSC-J / NTSC-K level)

struct TEXTMOD
{
    std::unordered_map<std::size_t, std::string> strings;
    std::unordered_map<std::string, std::unordered_map<std::size_t, std::string>> levels;
};

bool FParseId(const std::string& key, std::size_t& id)
{
    char* end = nullptr;
    const long long value = std::strtoll(key.c_str(), &end, 0);
    if (key.empty() || *end != '\0' || value < 0 || value >= 4096)
        return false;
    id = static_cast<std::size_t>(value);
    return true;
}

std::string LowerCase(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

const TEXTMOD* PtextmodForLanguage(CONTENT_LANGUAGE language)
{
    static std::map<CONTENT_LANGUAGE, std::unique_ptr<TEXTMOD>> s_loaded;
    if (const auto it = s_loaded.find(language); it != s_loaded.end())
        return it->second.get();
    std::unique_ptr<TEXTMOD> mod;
    if (!ModsDirectory().empty())
    {
        const fs::path path = ModsDirectory() / "Text" / (std::string(ContentLanguageName(language)) + ".json");
        std::error_code error;
        if (fs::is_regular_file(path, error))
        {
            mod = std::make_unique<TEXTMOD>();
            try
            {
                std::ifstream input(path, std::ios::binary);
                const json document = json::parse(input, nullptr, true, true);   // (// comments allowed)
                int count = 0;
                auto read = [&](const json& object, std::unordered_map<std::size_t, std::string>& strings) {
                    if (!object.is_object()) return;
                    for (const auto& [key, value] : object.items())
                    {
                        std::size_t id = 0;
                        if (!value.is_string() || !FParseId(key, id))
                        {
                            std::printf("mods: %s: skipped \"%s\" (not a string number and a string)\n",
                                cane::utf8(path).c_str(), key.c_str());
                            continue;
                        }
                        strings[id] = value.get<std::string>();
                        ++count;
                    }
                };
                if (document.contains("strings")) read(document.at("strings"), mod->strings);
                if (document.contains("levels") && document.at("levels").is_object())
                    for (const auto& [name, object] : document.at("levels").items())
                        read(object, mod->levels[LowerCase(name)]);
                std::printf("mods: %d strings from %s\n", count, cane::utf8(path).c_str());
            }
            catch (const std::exception& exception)
            {
                std::printf("mods: %s: %s (the file is ignored)\n", cane::utf8(path).c_str(), exception.what());
                mod.reset();
            }
        }
    }
    return (s_loaded[language] = std::move(mod)).get();
}

// --- the dump ---------------------------------------------------------------------------------------------------

uint32_t Le32(const std::vector<uint8_t>& data, std::size_t offset)
{
    return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) | (uint32_t(data[offset + 2]) << 16) |
        (uint32_t(data[offset + 3]) << 24);
}

// The payloads of a PCBRX file by language id (v17: one payload, id 0; v19: the unified container).
std::map<uint32_t, std::vector<uint8_t>> Payloads(const fs::path& path)
{
    std::map<uint32_t, std::vector<uint8_t>> result;
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    if (data.size() < 8 || Le32(data, 0) != 0x52424350u)
        return result;
    const uint32_t version = Le32(data, 4);
    if (version == 17)
    {
        result[0] = data;
        return result;
    }
    if (version != 19 || data.size() < 28)
        return result;
    std::size_t offset = 8;
    const uint32_t languageCount = Le32(data, offset + 8);
    const uint32_t chunkCount = Le32(data, offset + 16);
    offset += 20;
    struct ENTRY { uint32_t language; std::vector<uint32_t> chunks; };
    std::vector<ENTRY> entries;
    for (uint32_t i = 0; i < languageCount && offset + 16 <= data.size(); ++i)
    {
        ENTRY entry{Le32(data, offset), {}};
        const uint32_t references = Le32(data, offset + 12);
        offset += 16;
        if (offset + std::size_t(references) * 4 > data.size()) return {};
        for (uint32_t r = 0; r < references; ++r, offset += 4) entry.chunks.push_back(Le32(data, offset));
        entries.push_back(std::move(entry));
    }
    std::vector<std::pair<std::size_t, uint32_t>> chunks;
    for (uint32_t i = 0; i < chunkCount && offset + 8 <= data.size(); ++i)
    {
        const uint32_t size = Le32(data, offset);
        offset += 8;
        if (offset + size > data.size()) return {};
        chunks.emplace_back(offset, size);
        offset += size;
    }
    for (const ENTRY& entry : entries)
    {
        std::vector<uint8_t>& payload = result[entry.language];
        for (uint32_t chunk : entry.chunks)
        {
            if (chunk >= chunks.size()) return {};
            payload.insert(payload.end(), data.begin() + chunks[chunk].first,
                data.begin() + chunks[chunk].first + chunks[chunk].second);
        }
    }
    return result;
}

std::vector<std::string> Ploc(const std::vector<uint8_t>& payload)
{
    std::vector<std::string> strings;
    if (payload.size() < 8 || std::memcmp(payload.data() + payload.size() - 8, "PLOC", 4) != 0)
        return strings;
    const uint32_t size = Le32(payload, payload.size() - 4);
    if (size > payload.size() - 8) return strings;
    std::size_t offset = payload.size() - 8 - size;
    const std::size_t end = payload.size() - 8;
    if (offset + 4 > end) return strings;
    const uint32_t count = Le32(payload, offset);
    offset += 4;
    for (uint32_t i = 0; i < count && offset + 4 <= end; ++i)
    {
        const uint32_t length = Le32(payload, offset);
        offset += 4;
        if (offset + length > end) return {};
        strings.emplace_back(reinterpret_cast<const char*>(payload.data() + offset), length);
        offset += length;
    }
    return strings;
}

// The USA game's English strings under the table's numbers (the strings it shows; others are left out).
std::map<std::size_t, std::string> UsaStrings()
{
    std::map<std::size_t, std::string> strings;
    auto set = [&](std::size_t id, const char* text) { if (text != nullptr && text[0] != '\0') strings[id] = text; };
    set(0, "Press START button to Play");
    set(1, "Press START button to Resume Game");
    set(3, "Press SELECT button for Menu");
    set(4, "No Controller");
    set(8, g_aachzPresents[0]);
    set(9, g_aachzPresents[1]);
    set(10, suckerPunchProd);
    set(11, logoPchz);
    set(22, "Bentley");
    set(23, "Press &2X&. to Start Mission!");
    set(24, "Murray");
    for (std::size_t level = 0; level < 46; ++level)
        set(25 + level, PchzFriendlyFromLevelId(g_levelTable[level].levelID));
    set(79, "Move");
    set(80, "Zoom");
    for (int prk = PRK_PauseMenu; prk <= PRK_ControlsMenu; ++prk)
        set(83 + prk, s_mpprkprd[prk].pchz);
    set(116, s_mpprkprd[PRK_LanguageMenu].pchz);
    for (int respk = 0; respk < RESPK_Max; ++respk)
    {
        const RESPD& respd = s_arespd[respk];
        for (int variant = 0; variant < std::max(1, respd.cpchz); ++variant)
        {
            const int id = PalResponseStringIndex(static_cast<RESPK>(respk), variant);
            if (id < 0 || static_cast<std::size_t>(id) >= kStringCount || strings.count(id)) continue;
            set(id, respd.cpchz > 0 ? respd.apchz[variant] : s_mprespkachz[respk]);
        }
    }
    static const char* const powerups[] = {"&2T&.: Dive", "&2T&.: Roll", "&2T&.: Slow", "&2T&.: Mine",
                                           "&2T&.: Fast", "&2T&.: Decoy", "&2T&.: Stun"};
    for (std::size_t i = 0; i < std::size(powerups); ++i) set(162 + i, powerups[i]);
    set(169, "Beginning Master Thief Sprint!");
    set(170, "Press &2L&. for Bonus Commentary");
    set(171, "Best time:");
    static const char* const movies[] = {"The Secret Police File", "The Tokyo Police File", "The Making Of Sly Cooper",
        "Commercials & Outtakes", "Tide of Terror", "Raleigh Hung out to Dry", "Sunset Snake Eyes", "Muggshot Busted",
        "Vicious Voodoo", "Mz Ruby Vexed", "Fire in the Sky", "The Panda King on Ice", "The Cold Heart of Hate",
        "Ten Seconds", "The Thievius Raccoonus", "Credits"};
    for (std::size_t i = 0; i < std::size(movies); ++i) set(173 + i, movies[i]);
    return strings;
}

bool WriteDump(const fs::path& path, const std::map<std::size_t, std::string>& strings)
{
    json document;
    document["strings"] = json::object();
    for (const auto& [id, text] : strings) document["strings"][std::to_string(id)] = text;
    document["levels"] = json::object();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    return static_cast<bool>(output);
}
}  // namespace

const char* ContentModText(std::size_t id)
{
    if (ModsDirectory().empty())
        return nullptr;
    const TEXTMOD* mod = PtextmodForLanguage(ContentPackage().language);
    if (mod == nullptr)
        return nullptr;
    if (!mod->levels.empty() && !g_transition.m_worldCurrent.empty())
    {
        if (const auto level = mod->levels.find(LowerCase(g_transition.m_worldCurrent)); level != mod->levels.end())
            if (const auto it = level->second.find(id); it != level->second.end())
                return it->second.c_str();
    }
    if (const auto it = mod->strings.find(id); it != mod->strings.end())
        return it->second.c_str();
    return nullptr;
}

bool FTextModsActive()
{
    return !ModsDirectory().empty() && PtextmodForLanguage(ContentPackage().language) != nullptr;
}

int DumpTextCommand(const fs::path& directory)
{
    const CONTENT_PACKAGE& package = ContentPackage();
    if (!package.manifestLoaded)
    {
        std::fprintf(stderr, "--dump-text: no installation found (pass --data-dir <installation>)\n");
        return 2;
    }
    std::error_code error;
    fs::create_directories(directory, error);
    const fs::path splash = package.root / "Worlds" / "Splash" / "Splash.brx";
    const auto payloads = Payloads(splash);
    if (payloads.empty())
    {
        std::fprintf(stderr, "--dump-text: cannot read %s\n", cane::utf8(splash).c_str());
        return 1;
    }
    int written = 0;
    for (const auto& [languageId, payload] : payloads)
    {
        const std::vector<std::string> table = Ploc(payload);
        CONTENT_LANGUAGE language = static_cast<CONTENT_LANGUAGE>(languageId < 5 ? languageId : 0);
        if (package.region == CONTENT_REGION::NtscJ)
        {
            // (payload 1 is the English-voiced variant: the same Japanese text)
            if (languageId != 0) continue;
            language = CONTENT_LANGUAGE::Japanese;
        }
        else if (package.region == CONTENT_REGION::NtscK)
            language = CONTENT_LANGUAGE::Korean;
        std::map<std::size_t, std::string> strings;
        if (table.empty())
            strings = UsaStrings();   // (the USA levels carry no table: the game's own English strings)
        else
            for (std::size_t id = 0; id < table.size(); ++id) strings[id] = table[id];
        const fs::path path = directory / (std::string(ContentLanguageName(language)) + ".json");
        if (!WriteDump(path, strings))
        {
            std::fprintf(stderr, "--dump-text: cannot write %s\n", cane::utf8(path).c_str());
            return 1;
        }
        std::printf("%zu strings written to %s\n", strings.size(), cane::utf8(path).c_str());
        ++written;
    }
    return written > 0 ? 0 : 1;
}
