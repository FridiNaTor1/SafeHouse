// See textmod.h.
#include "textmod.h"
#include "worldglobals.h"

#include "cls.h"
#include "convert.h"
#include "disc.h"
#include "region.h"
#include "sw.h"
#include "langstore.h"
#include "porttext.h"
#include "sound.h"
#include "audio.h"

#include "../platform/platform.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

uint32_t g_isectorWorldLoading = 0;
WORLD_VALUE(g_isectorWorldLoading);   // (PC: moved aside while a kept world loads, worldglobals.h)

namespace
{
const char* const s_aszLanguage[LANG_Max] = {"English", "French", "Italian", "German", "Spanish", "Dutch", "Portuguese",
                                             "Danish", "Finnish", "Norwegian", "Swedish", "Japanese", "Korean", "Chinese"};

bool s_fOff = false;  // (--dump-text: the disc's own strings)

// The game's strings are Latin-1 bytes (16-bit text: UTF-8 already, region.h FWideChars / AppendCh16); the files UTF-8.
std::string Utf8OfLatin1(const std::string& s)
{
    if (FWideChars()) return s;
    std::string out;
    for (unsigned char c : s)
    {
        if (c < 0x80) out += char(c);
        else { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}
std::string Latin1OfUtf8(const std::string& s)
{
    if (FWideChars()) return s;
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = s[i];
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 0x07; n = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        if (cp == 0x2018 || cp == 0x2019) cp = '\'';     // (typographic quotes the fonts have not)
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2026) { out += "..."; continue; }
        out += cp <= 0xFF ? char(cp) : '?';
    }
    return out;
}

}  // namespace

std::string Utf8OfGameText(const std::string& s) { return Utf8OfLatin1(s); }
std::string GameTextOfUtf8(const std::string& s) { return Latin1OfUtf8(s); }

namespace
{
bool FParseId(const std::string& key, int& id)
{
    char* end = nullptr;
    const long v = std::strtol(key.c_str(), &end, 0);
    if (key.empty() || *end) return false;
    id = int(v);
    return true;
}

struct TEXTMOD
{
    std::unordered_map<int, std::string> mpstr;                                         // "strings"
    std::unordered_map<std::string, std::unordered_map<int, std::string>> mpworld;     // "worlds"
};

const TEXTMOD* PtextmodForLanguage(int lang)
{
    static std::map<int, std::unique_ptr<TEXTMOD>> s_mp;
    const auto it = s_mp.find(lang);
    if (it != s_mp.end()) return it->second.get();
    std::unique_ptr<TEXTMOD> p;
    // every mod folder's Text/<Language>.json (the packages', the loose folder's, lowest priority first; a later
    // file's string wins)
    for (const std::string& root : g_disc.AdirModRoots())
    {
        if (lang < 0) break;
        const fs::path path = cane::path(root) / "Text" / (std::string(SzLanguageModName(lang)) + ".json");
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        auto pfile = std::make_unique<TEXTMOD>();
        try
        {
            std::ifstream in(path);
            const json j = json::parse(in, nullptr, true, true);   // (comments allowed)
            int c = 0;
            auto read = [&](const json& o, std::unordered_map<int, std::string>& mp) {
                for (const auto& [k, v] : o.items())
                {
                    int id = 0;
                    if (!v.is_string() || !FParseId(k, id)) { std::printf("mods: %s: skipped \"%s\" (not an id and a string)\n", cane::utf8(path).c_str(), k.c_str()); continue; }
                    mp[id] = Latin1OfUtf8(v.get<std::string>());
                    ++c;
                }
            };
            if (j.contains("strings")) read(j.at("strings"), pfile->mpstr);
            if (j.contains("worlds"))
                for (const auto& [name, o] : j.at("worlds").items()) read(o, pfile->mpworld[name]);
            std::printf("mods: %d strings from %s\n", c, cane::utf8(path).c_str());
        }
        catch (const std::exception& e)
        {
            std::printf("mods: %s: %s (the file is ignored)\n", cane::utf8(path).c_str(), e.what());
            continue;
        }
        if (!p) { p = std::move(pfile); continue; }
        for (auto& [id, str] : pfile->mpstr) p->mpstr[id] = std::move(str);
        for (auto& [name, mp] : pfile->mpworld)
            for (auto& [id, str] : mp) p->mpworld[name][id] = std::move(str);
    }
    return (s_mp[lang] = std::move(p)).get();
}
}  // namespace

const char* SzLanguageModName(int lang)
{
    if (lang >= 0 && lang < LANG_Max) return s_aszLanguage[lang];
    const char* sz = SzLangStoreKey(lang);   // (a custom language: its folder name, langstore.h)
    return sz ? sz : "Unknown";
}

void ApplyTextMods(std::vector<std::pair<int, std::string>>& atext)
{
    if (s_fOff) return;
    const TEXTMOD* p = PtextmodForLanguage(g_nLang);
    if (!p) return;
    const std::string world = g_disc.SzWorldName(g_isectorWorldLoading);
    const auto itWorld = world.empty() ? p->mpworld.end() : p->mpworld.find(world);
    for (auto& [id, str] : atext)
    {
        if (itWorld != p->mpworld.end())
            if (const auto it = itWorld->second.find(id); it != itWorld->second.end()) { str = it->second; continue; }
        if (const auto it = p->mpstr.find(id); it != p->mpstr.end()) str = it->second;
    }
}

int DumpTextCommand(const std::string& dir)
{
    s_fOff = true;
    InitClasses();
    // the worlds: the boot world, the front end, every world record (one file once)
    struct WORLD { std::string name; FK fk; int filter; int iwrec; };
    std::vector<WORLD> aworld;
    {
        std::set<uint32_t> aset;
        auto add = [&](const std::string& w, const std::string& fallback, int iwrec) {
            FK fk;
            int filter = 0;
            std::string err;
            if (!FResolveWorld(w, fk, filter, err) || !fk.cb || !aset.insert(fk.isector).second) return;
            std::string name = g_disc.SzWorldName(fk.isector);
            if (name.empty()) name = fallback;
            aworld.push_back({name, fk, filter, iwrec});
        };
        add("boot", SzConvertWorldName(0) ? SzConvertWorldName(0) : "Boot", 0);
        add("frontend", "Front End", 1);
        for (int i = 0; i < 64; ++i)
        {
            const char* sz = SzConvertWorldName(i);
            add(std::to_string(i), sz ? sz : "World " + std::to_string(i), i);
        }
    }
    std::error_code ec;
    fs::create_directories(cane::path(dir), ec);
    const int langSave = g_nLang;
    // (usage.json: the worlds each string is in, by USA id as the languages folder numbers them: the localisation
    // tool's context; the disc's first language. Version 2 adds each world's episode (the boot world's records' slot;
    // the front end -1) and each dialog line's speaker (its stream's class id, -1 none) and headset flag (the lines the
    // original's USB headset plays: the radio / binocucom talk), from the worlds' sound tables.)
    json usage = json::object();
    std::vector<int> aepisode(aworld.size(), -1);
    std::map<int, std::pair<int, int>> mpdialog;   // USA id -> (speaker class, headset)
    for (int k = 0; k < CLanguagesDisc(); ++k)
    {
        g_nLang = LanguageOfDiscIndex(k);
        // id -> (string -> the worlds that have it)
        std::map<int, std::map<std::string, std::vector<std::string>>> mp;
        std::map<int, std::vector<int>> mpidaworld;
        for (std::size_t iw = 0; iw < aworld.size(); ++iw)
        {
            const auto& [name, fk, filter, iwrecWorld] = aworld[iw];
            auto psw = std::make_unique<SW>();
            psw->filter = filter;
            std::size_t cbRead = 0, cbFile = 0;
            if (k == 0) { std::lock_guard<std::recursive_mutex> lock(g_mtxSound); g_sound.adialog.clear(); }
            if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) { std::fprintf(stderr, "dump-text: %s did not load\n", name.c_str()); continue; }
            if (k == 0)
            {
                // (the boot world's records hold the episodes; the front end is none)
                static std::vector<WREC> s_awrec;
                if (iw == 0) s_awrec = psw->awrec;
                const int iwrec = iwrecWorld;
                if (iwrec != 1 && iwrec >= 0 && iwrec < int(s_awrec.size())) aepisode[iw] = s_awrec[std::size_t(iwrec)].slot;
                std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
                for (const SNDSTREAM& d : g_sound.adialog)
                    if (const int idUsa = IdTextUsa(d.id); idUsa >= 0) mpdialog[idUsa] = {d.oidSpeaker, d.fNoHeadset == 0 ? 1 : 0};
            }
            for (const auto& [id, str] : psw->atext)
            {
                mp[id][Utf8OfLatin1(str)].push_back(name);
                if (k == 0 && IdTextUsa(id) >= 0) mpidaworld[IdTextUsa(id)].push_back(int(iw));
            }
        }
        if (k == 0)
        {
            json names = json::array(), ids = json::object();
            for (const auto& w : aworld) names.push_back(w.name);
            for (const auto& [id, aw] : mpidaworld) ids[std::to_string(id)] = aw;
            json dialogs = json::object();
            for (const auto& [id, d] : mpdialog) dialogs[std::to_string(id)] = {d.first, d.second};
            usage = {{"version", 2}, {"disc", g_disc.SzDisc()}, {"worlds", names}, {"episodes", aepisode}, {"strings", ids}, {"dialogs", dialogs}};
        }
        // "strings": each id's most common string; "worlds": a world's string where it differs from that
        json strings = json::object(), worlds = json::object();
        std::map<std::string, json> mpworld;
        for (const auto& [id, mpstr] : mp)
        {
            const std::string* pstrMost = nullptr;
            std::size_t cMost = 0;
            for (const auto& [str, aw] : mpstr) if (aw.size() > cMost) { cMost = aw.size(); pstrMost = &str; }
            strings[std::to_string(id)] = *pstrMost;
            for (const auto& [str, aw] : mpstr)
                if (&str != pstrMost)
                    for (const std::string& w : aw) mpworld[w][std::to_string(id)] = str;
        }
        for (const auto& w : aworld)
            if (mpworld.count(w.name)) worlds[w.name] = mpworld[w.name];
        // (the port's own strings in that language, porttext.h: a language's Text.json "port")
        json port = json::object();
        if (g_nLang < CLANG_PORT)
            for (const auto& [name, str] : PortTextColumn(g_nLang)) port[name] = str;
        const json j = {{"language", SzLanguageModName(g_nLang)}, {"strings", strings}, {"worlds", worlds}, {"port", port}};
        const fs::path path = cane::path(dir) / (std::string(SzLanguageModName(g_nLang)) + ".json");
        std::ofstream out(path, std::ios::trunc);
        out << j.dump(1) << '\n';
        if (!out) { std::fprintf(stderr, "dump-text: cannot write %s\n", cane::utf8(path).c_str()); return 1; }
        std::printf("dump-text: %s: %zu strings, %zu worlds with their own\n", cane::utf8(path).c_str(), mp.size(), mpworld.size());
    }
    g_nLang = langSave;
    {
        const fs::path path = cane::path(dir) / "usage.json";
        std::ofstream out(path, std::ios::trunc);
        out << usage.dump() << '\n';
        if (!out) { std::fprintf(stderr, "dump-text: cannot write %s\n", cane::utf8(path).c_str()); return 1; }
    }
    return 0;
}

int DumpPortTextCommand(const std::string& dir)
{
    std::error_code ec;
    fs::create_directories(cane::path(dir), ec);
    for (int lang = 0; lang < CLANG_PORT; ++lang)
    {
        json port = json::object();
        for (const auto& [name, str] : PortTextColumn(lang)) port[name] = str;
        const json j = {{"language", SzLanguageModName(lang)}, {"port", port}};
        const fs::path path = cane::path(dir) / (std::string(SzLanguageModName(lang)) + ".json");
        std::ofstream out(path, std::ios::trunc);
        out << j.dump(1) << '\n';
        if (!out) { std::fprintf(stderr, "dump-port-text: cannot write %s\n", cane::utf8(path).c_str()); return 1; }
        std::printf("dump-port-text: %s: %zu strings\n", cane::utf8(path).c_str(), port.size());
    }
    return 0;
}
