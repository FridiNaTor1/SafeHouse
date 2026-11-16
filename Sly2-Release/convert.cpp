// The Sly 2 disc converter: reads the disc once and writes every file the game can read as a
// named file of the PC layout, with extraction_manifest.json, the index the game reads them through (disc.cpp).
//
// The files are found the way the game finds them: the executable's 30 keys, then every world (the boot world's
// world table, the front end, the 44 world records) loaded with the game's own loaders, every 64-byte file key they
// read collected with what it names (bis.h KEYK: a world, a screen file, an exit's world and movies, the SFX bank,
// the ambience stream, a dialog line per language, a music track).
#include "bis.h"
#include "cls.h"
#include "convert.h"
#include "texmod.h"
#include "audio.h"
#include "disc.h"
#include "flacfile.h"
#include "fmv.h"
#include "region.h"
#include "snd989.h"
#include "sw.h"
#include "wavfile.h"
#include "worldparts.h"
#include "workpool.h"

#include "../platform/brx_decompress.h"
#include "../platform/platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace
{
uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

#ifdef __SWITCH__
// (the Switch installer's progress: false from it cancels the conversion)
const CaneConvertProgress* s_pprogress = nullptr;
bool s_fCancelled = false;
bool FProgress(size_t done, size_t total, const char* stage, const char* file)
{
    if (!s_pprogress || s_fCancelled) return !s_fCancelled;
    if (!(*s_pprogress)(done, total, stage, file)) s_fCancelled = true;
    return !s_fCancelled;
}
#else
inline bool FProgress(size_t, size_t, const char*, const char*) { return true; }
#endif

// ---------------------------------------------------------------------------------------------------------
// Names (CONVERTER.md "Names"): the world records' internal names (their groups' "<code>_pz" / "_vz" / "_rig"),
// the friendly names, the episodes (the front end's own text 109..117), the music by (first world, music id).

struct WORLDNAME
{
    int iwrec;          // the world record (-1: the boot world, -2: the front end)
    const char* code;   // the world's internal name, from its group names
    const char* name;   // the friendly name (the folder and the file's name)
};
// (the boot world is world record 0 too: one file)
const WORLDNAME s_aworldname[] = {
    {0, "jb_intro", "Cairo Museum"},
    {1, "frontend", "Front End"},
    {2, "f_nightclub_exterior", "Paris"},
    {3, "f_nightclub_cellar", "Wine Cellar"},
    {4, "f_nightclub_disco", "Dimitri's Nightclub"},
    {5, "f_nightclub_heist", "Printing Press"},
    {6, "f_nightclub_moulinrouge", "Moulin Rouge"},
    {7, "f_nightclub_waterpump", "Waterpump Room"},
    {8, "i_palace_ext", "Rajan's Palace"},
    {9, "i_palace_int", "Palace Guest Rooms"},
    {10, "i_palace_basement", "Palace Basement"},
    {11, "i_palace_ballroom", "Palace Ballroom"},
    {12, "i_temple_ext", "Jungle Temple"},
    {13, "i_temple_int", "Spice Factory"},
    {14, "p_prison_ext", "Prague Prison"},
    {15, "p_prison_int", "Prison Interior"},
    {16, "p_prison_vault", "Prison Vault"},
    {17, "p_castle_ext", "Contessa's Castle"},
    {19, "p_castle_waterrails", "Castle Water Rails"},
    {20, "p_castle_guardbreak", "Castle Guard Break"},
    {21, "p_castle_wolftomb", "The Crypt"},
    {22, "p_castle_heist", "Castle Heist"},
    {23, "p_castle_ewoktrainer", "Crypt Training Room"},
    {24, "p_castle_ewoktomb", "Crypt Tomb"},
    {25, "p_castle_ewokhall", "Crypt Hall"},
    {26, "p_castle_ewokwater", "Crypt Water Room"},
    {27, "c_train_ext", "Canada Train Station"},
    {28, "c_train_cabins", "Train Cabins"},
    {29, "c_train_a", "Train A"},
    {30, "c_train_b", "Train B"},
    {31, "c_train_c", "Train C"},
    {32, "c_sawmill_ext", "Sawmill"},
    {33, "c_sawmill_mulch", "Sawmill Mulch Room"},
    {34, "c_sawmill_burn", "Sawmill Burn Room"},
    {35, "c_sawmill_lighthouse", "Lighthouse"},
    {36, "c_sawmill_bearcave", "Bear Cave"},
    {37, "c_sawmill_boss", "Jean Bison Battle"},
    {38, "a_blimp_ext", "Arpeggio's Blimp"},
    {39, "a_blimp_int", "Blimp Interior"},
    {40, "a_blimp_engine_room_murray", "Engine Room (Murray)"},
    {41, "a_blimp_engine_room_bentley", "Engine Room (Bentley)"},
    {42, "a_blimp_engine_room_jt", "Engine Room (Sly)"},
    {43, "a_blimp_boss", "Clock-La Battle"},
};

// The episodes' names when the front end's text has none (its ids 109..117 hold them).
const char* const s_aszEpisode[9] = {"A Shadow From The Past", "The Black Chateau", "A Starry Eyed Encounter",
                                     "The Predator Awakes", "Jailbreak", "A Tangled Web", "He Who Tames the Iron Horse",
                                     "Menace in the North, Eh?", "Anatomy for Disaster"};

struct MUSICNAME
{
    int id;             // the music id (one track per id in every world that lists it)
    const char* name;   // the soundtrack's name or, where that is not certain, its place (CONVERTER.md "Music")
};
const MUSICNAME s_amusicname[] = {
#include "convertmusic.inc"
};

// The movies' names (CONVERTER.md "Movies"), by what plays them, so they hold on every disc (the European discs number
// their movies 2055..2097, the USA disc 2004..2059):
// - The executable's movie keys (USA addresses) are a table: the prologue, then per episode its intro and its ending,
//   then the extras and the credits. Each intro is also listed by the front end (it starts the episode) and ends on the
//   episode's title card; each ending is listed by the episode's worlds.
// - Every other movie listed only by one episode's worlds is one of Bentley's chalk-talk briefings (their exits:
//   ITEM18_2026-11-02.md, 2006 plays after Ep 1's first job), numbered in id order. The last one of episodes 1..7
//   opens on the operation's title card.
// - The USA disc's extras and logos have keys the European discs use otherwise (no extras there): USA only.
struct MOVIEROLE
{
    uint32_t va;        // the key's USA address
    bool fUsaOnly;
    const char* name;
};
const MOVIEROLE s_amovierole[] = {
    {0x2A3268, false, "Prologue"},
    {0x2A36A8, false, "Extra Sly 1 Intro"},
    {0x2A37A8, false, "Credits"},
    {0x2A3868, false, "Logo Sony"},           // (the boot logo: frontend.cpp 0x2A3868)
    {0x2A36E8, true, "Extra Commercial"},
    {0x2A3728, true, "Extra Trailer"},
    {0x2A3768, true, "Extra Making Of"},
    {0x2A39A8, true, "Logo Sucker Punch"},
};
const char* const s_aszOperation[8] = {"Thunder Beak", "Hippo Drop", "Wet Tiger", "Trojan Tank", "High Road", "Choo-Choo",
                                       "Canada Games", nullptr};

const char* SzWorldName(int iwrec, const char** pcode)
{
    for (const WORLDNAME& w : s_aworldname)
        if (w.iwrec == iwrec)
        {
            if (pcode) *pcode = w.code;
            return w.name;
        }
    if (pcode) *pcode = "";
    return nullptr;
}

// A file or folder name (no path separators, no characters Windows refuses).
std::string SzSafe(std::string s)
{
    for (char& c : s)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "_" : s;
}

const char* const s_aszLanguageName[LANG_Max] = {"English", "French", "Italian", "German", "Spanish", "Dutch",
                                                "Portuguese", "Danish", "Finnish", "Norwegian", "Swedish",
                                                "Japanese", "Korean", "Chinese"};

// ---------------------------------------------------------------------------------------------------------
// The crawl

struct WORLDINFO
{
    int iwrec = -1;      // the world record (0: the boot world, 1: the front end)
    FK fk;
    int episode = -1;    // (the record's slot; -1 the front end)
    int kind = 0;
    std::string code, name, dir;  // dir: Worlds/<episode>/<name>
    std::vector<std::pair<int, uint32_t>> amusic;   // (music id, sector)
    std::vector<std::string> agroup;
};

struct FILEINFO
{
    FK fk;
    std::string kind;            // world, screen, bank, ambience, music, dialog, movie, icon, data
    int iworld = -1;             // the first world (in record order) that names it (index into s_aworld)
    std::set<int> aiworld;       // every world that names it
    std::vector<uint32_t> ava;   // the executable's keys (USA addresses)
    int id = -1, lang = -1;      // dialog: line id + language; music: music id (in the first world); screen: oid
    std::string path, name;
    double tSeconds = 0;         // streams: the length
    std::vector<uint32_t> asectorCopy;  // the disc's other copies of the same file (same id, size and bytes)
};

std::vector<WORLDINFO> s_aworld;
// (the textures written during the crawl: ConvertCommand sets these)
CWorkPool* s_ppoolTextures = nullptr;
std::string s_dirTextures;
std::set<std::string> s_setTextureName;
std::map<uint32_t, std::vector<WPMARK>> s_mpamark;   // a world file's part marks (worldparts.h), by its sector
std::map<uint32_t, std::vector<std::string>> s_mpagroup;   // and its groups' names
std::map<uint32_t, std::vector<std::pair<std::size_t, std::size_t>>> s_mpaalign;   // and its Align(16)s (bis.h)
bool FUnravel(uint32_t isector) { return !g_fConvertWholeWorlds && s_mpamark.count(isector) != 0; }

// The language tracks (dialog lines, movie sound) as FLAC: --flac / --no-flac, else when the disc has more than one
// language (the European discs: 11 languages of dialog and movie sound would be 7.8 GB as WAV, 4.9 GB as FLAC)
bool FConvertFlac() { return g_nConvertFlac >= 0 ? g_nConvertFlac > 0 : CLanguagesDisc() > 1; }
std::map<uint32_t, FILEINFO> s_mpfile;  // by sector
std::map<uint32_t, uint32_t> s_mpsectorCopy;  // a merged copy's sector -> its file's

const std::string& PathOfSector(uint32_t isector)
{
    static const std::string s_empty;
    const auto itc = s_mpsectorCopy.find(isector);
    const auto it = s_mpfile.find(itc != s_mpsectorCopy.end() ? itc->second : isector);
    return it != s_mpfile.end() ? it->second.path : s_empty;
}
int s_iworldCur = -1;
std::vector<std::string> s_awarning;

FILEINFO* PfileNote(const FK& fk, const char* kind)
{
    if (fk.cb == 0) return nullptr;
    FILEINFO& f = s_mpfile[fk.isector];
    if (f.fk.cb && (f.fk.cb != fk.cb || f.fk.id != fk.id))
    {
        char sz[160];
        std::snprintf(sz, sizeof(sz), "two keys for sector 0x%X: id %u cb 0x%X and id %u cb 0x%X", fk.isector, f.fk.id,
                      f.fk.cb, fk.id, fk.cb);
        s_awarning.emplace_back(sz);
    }
    f.fk = fk;
    if (f.kind.empty() || f.kind == "data") f.kind = kind;
    if (s_iworldCur >= 0)
    {
        if (f.iworld < 0) f.iworld = s_iworldCur;
        f.aiworld.insert(s_iworldCur);
    }
    return &f;
}

void NoteRecord(const void* pv, KEYK keyk, int id, int lang)
{
    const uint32_t* aw = static_cast<const uint32_t*>(pv);
    if (aw[1] == aw[15]) return;  // (an unset key: the original's 0x228250 test)
    const FK fk = FkFromRecord(pv);
    static int s_iexit = 0;
    switch (keyk)
    {
    case KEYK_World: PfileNote(fk, "world"); break;
    case KEYK_Screen: PfileNote(fk, "screen"); break;
    case KEYK_Bank: PfileNote(fk, "bank"); break;
    case KEYK_Ambience: PfileNote(fk, "ambience"); break;
    case KEYK_Dialog:
        if (FILEINFO* p = PfileNote(fk, "dialog"); p && p->id < 0) { p->id = id; p->lang = lang; }
        break;
    case KEYK_Music:
        if (FILEINFO* p = PfileNote(fk, "music"))
        {
            if (p->id < 0) p->id = id;
            if (s_iworldCur >= 0) s_aworld[s_iworldCur].amusic.emplace_back(id, fk.isector);
        }
        break;
    case KEYK_Exit: (void)s_iexit; PfileNote(fk, "data"); break;  // (a world or a movie: by its content below)
    default: PfileNote(fk, "data"); break;
    }
}

bool FReadDisc(uint32_t isector, uint64_t ib, void* pv, std::size_t cb) { return g_disc.FReadFileBytes(isector, ib, pv, cb); }

// The kind of a file from its first bytes, where the key's place did not say.
std::string KindOfContent(const FK& fk)
{
    uint8_t h[16] = {};
    FReadDisc(fk.isector, 0, h, sizeof(h));
    if (!std::memcmp(h, "\x00\x00\x01\xBA", 4)) return "movie";
    if (!std::memcmp(h, "VAGp", 4)) return "dialog";
    if (!std::memcmp(h, " KPV", 4)) return "music";
    const uint32_t t = Le32(h), c = Le32(h + 4);
    if ((t == 1 || t == 3) && c >= 2 && c <= 8) return "bank";
    return "";
}

// A stream's length in seconds (VAGp: mono; VPK: per channel bytes at +4, 16-byte blocks of 28 samples).
double TStream(const FK& fk)
{
    uint8_t h[0x20] = {};
    if (!FReadDisc(fk.isector, 0, h, sizeof(h))) return 0;
    auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
    if (!std::memcmp(h, "VAGp", 4)) { const uint32_t r = be32(h + 0x10); return r ? be32(h + 0xC) / 16.0 * 28.0 / r : 0; }
    if (!std::memcmp(h, " KPV", 4)) { const uint32_t r = Le32(h + 0x10); return r ? Le32(h + 4) / 16.0 * 28.0 / r : 0; }
    return 0;
}

// (the record's key and filter as FResolveWorld gives them, without loading the boot world again for each record)
bool FLoadWorldForCrawl(const WREC& wrec, int iworld)
{
    const FK fk = wrec.fk;
    const int filter = iworld;
    if (fk.cb == 0 || fk.id == 0) return false;
    g_pfnNoteKeyRecord = NoteRecord;
    s_iworldCur = iworld;
    s_aworld[iworld].fk = fk;
    PfileNote(fk, "world");
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    std::vector<WPMARK> amark;   // (where the loader starts each block: the world's parts)
    std::vector<std::pair<std::size_t, std::size_t>> aalign;
    g_pawpmark = &amark;
    g_paalignRecord = &aalign;
    const bool f = FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile);
    g_pawpmark = nullptr;
    g_paalignRecord = nullptr;
    for (const WE& we : psw->awe) s_aworld[iworld].agroup.emplace_back(we.achName, strnlen(we.achName, sizeof(we.achName)));
    if (f && !s_mpamark.count(fk.isector))
    {
        s_mpamark[fk.isector] = std::move(amark);
        s_mpagroup[fk.isector] = s_aworld[iworld].agroup;
        s_mpaalign[fk.isector] = std::move(aalign);
    }
    s_iworldCur = -1;
    g_pfnNoteKeyRecord = nullptr;
    if (!f) s_awarning.push_back("world " + std::to_string(iworld) + " did not load");
    // (the converter: its textures written now, while the renderer's tables hold them: CONVERTER.md "Textures")
    if (f && s_ppoolTextures) DumpLoadedWorldTextures(s_dirTextures, s_setTextureName, s_ppoolTextures);
    return f;
}

std::string StrEpisodeName(int episode, const std::vector<std::pair<int, std::string>>& atextFrontend)
{
    if (episode < 0 || episode > 8) return "";
    for (const auto& [id, str] : atextFrontend)
        if (id == IdText(109 + episode) && !str.empty()) return str;   // (the Japanese ids differ: region.h)
    return s_aszEpisode[episode];
}

bool FCrawl(std::string& error)
{
    // (a second crawl in the same run, --convert --languages-dir: the installation's languages crawl the converted
    // folder after the disc image's crawl; its sectors are the installed files', so nothing of the first one may stay)
    s_aworld.clear();
    s_mpfile.clear();
    s_mpsectorCopy.clear();
    s_mpamark.clear();
    s_mpagroup.clear();
    s_mpaalign.clear();
    s_awarning.clear();
    g_disc.SetModsDirectory("");   // (the disc's own data: no text mods in the names)
    InitClasses();
    // the boot world's table: the world records (and the episodes' names from the front end's text)
    std::vector<std::pair<int, std::string>> atextFrontend;
    std::vector<WREC> awrec;
    {
        const int langSave = g_nLang;
        g_nLang = LANG_English;  // (the names: always the English text, every disc has it)
        auto pswBoot = std::make_unique<SW>();
        std::size_t cbRead = 0, cbFile = 0;
        if (!FLoadWorldFile(pswBoot.get(), FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40)), "", &cbRead, &cbFile))
        {
            error = "the boot world did not load";
            return false;
        }
        awrec = pswBoot->awrec;
        auto pswFront = std::make_unique<SW>();
        if (FLoadWorldFile(pswFront.get(), FkFromRecord(g_disc.PbElf(VA_FK_FRONTEND, 0x40)), "", &cbRead, &cbFile))
            atextFrontend = pswFront->atext;
        g_nLang = langSave;
    }
    // the worlds, in record order
    for (std::size_t i = 0; i < awrec.size(); ++i)
    {
        WORLDINFO wi;
        wi.iwrec = int(i);
        wi.episode = awrec[i].slot;
        wi.kind = awrec[i].kind;
        const char* code = "";
        const char* name = SzWorldName(int(i), &code);
        wi.code = code;
        char sz[64];
        std::snprintf(sz, sizeof(sz), "World %02zu", i);
        wi.name = name ? name : sz;
        if (i == 1) wi.dir = "Worlds/" + SzSafe(wi.name);
        else
        {
            char szEp[16];
            std::snprintf(szEp, sizeof(szEp), "%d ", wi.episode);
            wi.dir = "Worlds/" + std::string(wi.episode == 0 ? "0 " : szEp) + SzSafe(StrEpisodeName(wi.episode, atextFrontend)) + "/" + SzSafe(wi.name);
        }
        s_aworld.push_back(wi);
    }
    if (s_aworld.size() < 2)
    {
        error = "the boot world has no world table";
        return false;
    }
    // the executable's keys (the boot world, the front end, the movies, the memory card icon)
    for (const auto& [va, fk] : g_disc.ElfKeys())
        if (FILEINFO* p = PfileNote(fk, "data")) p->ava.push_back(va);
    // every world with the game's loaders (the boot world is record 0, the front end record 1)
    for (std::size_t i = 0; i < s_aworld.size(); ++i)
    {
        if (!(s_aworld[i].iwrec >= 0 && FkFromRecord(awrec[i].abRecord).cb)) continue;
        std::printf("crawl: world %zu %s\n", i, s_aworld[i].name.c_str());
        std::fflush(stdout);
        if (!FProgress(i, s_aworld.size(), "Reading the worlds", s_aworld[i].name.c_str()))
        {
            error = "cancelled";
            return false;
        }
        FLoadWorldForCrawl(awrec[i], int(i));
        if (s_ppoolTextures)
        {
            std::printf("textures %zu %zu %zu\n", i + 1, s_aworld.size(), i);
            std::fflush(stdout);
        }
    }
    // the kinds by content where the key's place did not say (exits, task lists, the executable's keys)
    for (auto& [isector, f] : s_mpfile)
    {
        if (f.kind != "data") continue;
        const std::string k = KindOfContent(f.fk);
        if (!k.empty()) f.kind = k == "music" ? "ambience" : k;
        else
        {
            bool fWorld = false;
            for (const WORLDINFO& w : s_aworld) fWorld |= w.fk.isector == isector && w.fk.cb;
            if (fWorld) f.kind = "world";
            else if (f.fk.id == 1999) f.kind = "icon";
        }
    }
    return true;
}

// Every world's internal name checked against its groups ("<code>_pz" / "_vz" / "_rig" / the code itself).
void CheckWorldCodes()
{
    for (const WORLDINFO& w : s_aworld)
    {
        if (w.code.empty() || w.code == "frontend" || w.agroup.empty()) continue;
        bool f = false;
        for (const std::string& g : w.agroup)
        {
            std::string s = g.rfind("FK$X", 0) == 0 ? g.substr(4) : g;
            f |= s == w.code || s == w.code + "_pz" || s == w.code + "_vz" || s == w.code + "_rig";
        }
        if (!f) s_awarning.push_back("world record " + std::to_string(w.iwrec) + ": no group named " + w.code + " (names table)");
    }
}

uint64_t HashFile(const FK& fk)
{
    std::vector<uint8_t> ab(fk.cb);
    if (!FReadDisc(fk.isector, 0, ab.data(), ab.size())) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : ab) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

// The disc stores some files once per world that uses them (the music tracks, the screen files): one file each in
// the layout (same id, size and bytes), every copy's sector mapped to it.
void MergeCopies()
{
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> mpidcb;
    for (const auto& [isector, f] : s_mpfile) mpidcb[{f.fk.id, f.fk.cb}].push_back(isector);
    for (const auto& [k, asector] : mpidcb)
    {
        if (asector.size() < 2) continue;
        std::map<uint64_t, uint32_t> mphash;  // hash -> the first sector
        for (uint32_t isector : asector)
        {
            const uint64_t h = HashFile(s_mpfile[isector].fk);
            auto [it, fNew] = mphash.emplace(h, isector);
            if (fNew) continue;
            FILEINFO& fFirst = s_mpfile[it->second];
            FILEINFO& fCopy = s_mpfile[isector];
            if (fFirst.kind != fCopy.kind) continue;
            fFirst.aiworld.insert(fCopy.aiworld.begin(), fCopy.aiworld.end());
            if (fFirst.iworld < 0 || (fCopy.iworld >= 0 && fCopy.iworld < fFirst.iworld)) fFirst.iworld = fCopy.iworld;
            fFirst.ava.insert(fFirst.ava.end(), fCopy.ava.begin(), fCopy.ava.end());
            fFirst.asectorCopy.push_back(isector);
            fFirst.asectorCopy.insert(fFirst.asectorCopy.end(), fCopy.asectorCopy.begin(), fCopy.asectorCopy.end());
            s_mpsectorCopy[isector] = it->second;
            s_mpfile.erase(isector);
        }
    }
}

// The movies' names by sector (MOVIEROLE above).
std::map<uint32_t, std::string> MovieNames()
{
    std::map<uint32_t, std::string> mp;
    const bool fUsa = g_disc.Region() == DISCREGION::Usa;
    auto roleOfVa = [&](uint32_t va) -> std::string {
        if (va >= 0x2A32A8 && va < 0x2A36A8)   // the episodes' (intro, ending) pairs
        {
            const uint32_t i = (va - 0x2A32A8) / 0x40;
            return "Ep" + std::to_string(i / 2 + 1) + (i % 2 ? " Ending" : " Intro");
        }
        for (const MOVIEROLE& r : s_amovierole)
            if (r.va == va && (fUsa || !r.fUsaOnly)) return r.name;
        return "";
    };
    // the executable's keys: the episode pairs first (a key the region map sends elsewhere does not override them)
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [isector, f] : s_mpfile)
        {
            if (f.kind != "movie" || mp.count(isector)) continue;
            for (uint32_t va : f.ava)
            {
                const bool fPair = va >= 0x2A32A8 && va < 0x2A36A8;
                if (fPair != (pass == 0)) continue;
                if (const std::string r = roleOfVa(va); !r.empty()) { mp[isector] = r; break; }
            }
        }
    // the briefings: the other movies only one episode's worlds list (the front end aside), in id order
    std::map<int, std::vector<std::pair<uint32_t, uint32_t>>> mpepaidsector;
    for (const auto& [isector, f] : s_mpfile)
    {
        if (f.kind != "movie" || mp.count(isector) || !f.ava.empty()) continue;
        std::set<int> aep;
        for (int iw : f.aiworld)
            if (s_aworld[iw].iwrec != 1) aep.insert(s_aworld[iw].episode);
        if (aep.size() == 1 && *aep.begin() >= 1 && *aep.begin() <= 8) mpepaidsector[*aep.begin()].emplace_back(f.fk.id, isector);
    }
    for (auto& [ep, a] : mpepaidsector)
    {
        std::sort(a.begin(), a.end());
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            std::string name = "Ep" + std::to_string(ep);
            if (i + 1 == a.size() && s_aszOperation[ep - 1]) name += std::string(" Operation ") + s_aszOperation[ep - 1];
            else name += " Briefing " + std::to_string(i + 1);
            mp[a[i].second] = name;
        }
    }
    return mp;
}

// The layout's paths (CONVERTER.md "Layout").
void AssignPaths()
{
    MergeCopies();
    std::map<std::string, int> mpcPath;
    auto unique = [&](std::string p) {
        const int c = mpcPath[p]++;
        if (c == 0) return p;
        const auto dot = p.rfind('.');
        return p.substr(0, dot) + " (" + std::to_string(c + 1) + ")" + p.substr(dot);
    };
    std::map<int, const char*> mpmusic;
    for (const MUSICNAME& m : s_amusicname) mpmusic[m.id] = m.name;
    const std::map<uint32_t, std::string> mpmovieName = MovieNames();
      for (auto& [isector, f] : s_mpfile)
    {
        const WORLDINFO* pw = f.iworld >= 0 ? &s_aworld[f.iworld] : nullptr;
        char sz[96];
        if (f.kind == "world")
        {
            const WORLDINFO* pwSelf = nullptr;
            for (const WORLDINFO& w : s_aworld) if (w.fk.isector == isector && w.fk.cb && !pwSelf) pwSelf = &w;
            if (pwSelf)
            {
                f.name = pwSelf->name;
                f.path = pwSelf->dir + "/" + SzSafe(pwSelf->name) + (FUnravel(isector) ? ".world.json" : ".brx");
            }
            else f.path = "Worlds/Unlisted/" + std::to_string(f.fk.id) + ".brx";
        }
        else if (f.kind == "screen")
        {
            std::snprintf(sz, sizeof(sz), "Screens/Screen %u.brx", f.fk.id);
            f.path = (pw && f.aiworld.size() == 1 ? pw->dir + "/" : std::string()) + sz;
        }
        else if (f.kind == "bank") f.path = (pw ? pw->dir + "/" + SzSafe(pw->name) : "Sounds/" + std::to_string(f.fk.id)) + ".bnk";
        else if (f.kind == "ambience") f.path = (pw ? pw->dir + "/Ambience" : "Sounds/Ambience " + std::to_string(f.fk.id)) + (g_fConvertPs2 ? ".vpk" : ".wav");
        else if (f.kind == "music")
        {
            const char* name = mpmusic.count(f.id) ? mpmusic[f.id] : nullptr;
            std::snprintf(sz, sizeof(sz), "%02d %s", f.id, name ? name : pw ? pw->name.c_str() : "Unlisted");
            f.name = sz;
            f.path = "Music/" + SzSafe(f.name) + (g_fConvertPs2 ? ".vpk" : ".wav");
        }
        else if (f.kind == "dialog")
        {
            const char* lang = f.lang >= 0 && f.lang < LANG_Max ? s_aszLanguageName[f.lang] : "Unknown";
            std::snprintf(sz, sizeof(sz), g_fConvertPs2 ? "/%d.vag" : FConvertFlac() ? "/%d.flac" : "/%d.wav", f.id);
            // (a line one world lists: that world's folder; lines every world lists, Bentley's and the menus': Shared)
            f.path = std::string("Dialog/") + lang + "/" + (pw && f.aiworld.size() == 1 ? SzSafe(pw->name) : std::string("Shared")) + sz;
        }
        else if (f.kind == "movie")
        {
            const auto it = mpmovieName.find(isector);
            if (it != mpmovieName.end()) { std::snprintf(sz, sizeof(sz), "%u %s", f.fk.id, it->second.c_str()); f.name = sz; }
            else std::snprintf(sz, sizeof(sz), "%u", f.fk.id);
            f.path = "Movies/" + SzSafe(sz) + (g_fConvertPs2 ? ".pss" : ".m2v");
        }
        else if (f.kind == "icon") f.path = "Misc/Memory Card Icon.bin";
        else
        {
            std::snprintf(sz, sizeof(sz), "Misc/%u at 0x%X.bin", f.fk.id, isector);
            f.path = sz;
        }
        if (f.kind == "music" || f.kind == "ambience" || f.kind == "dialog") f.tSeconds = TStream(f.fk);
        f.path = unique(f.path);
    }
}

bool FWriteFile(const fs::path& p, const uint8_t* pb, std::size_t cb)
{
    std::error_code ec;   // (the worker threads create folders at the same time: a folder another made is fine)
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(pb), std::streamsize(cb));
    return bool(out);
}

// ---------------------------------------------------------------------------------------------------------
// The audio (format 2): every stream written as the 16-bit frames the game's
// decoders produce from it (spiop.cpp FRefill, fmv.cpp CMovieAudio, snd989.cpp DecodeSpuAdpcm), with its rate, loop
// and channel table, so the game plays the same samples from the WAV.

// A channel's ADPCM as spiop reads and decodes it: 0x1000-byte pieces interleaved by channel from the data offset (a
// VAG: one channel from 0x30), the blocks of each piece decoded with the history carried on; bytes past the file read
// as 0 (CDisc::FReadFileBytes).
std::vector<int16_t> AsDecodeStream(const std::vector<uint8_t>& ab, uint32_t ibData, uint32_t cbPer, uint32_t nch, uint32_t ich, bool fVag)
{
    std::vector<int16_t> as(std::size_t(cbPer / 16) * 28);
    int h1 = 0, h2 = 0;
    uint8_t block[16];
    for (uint32_t b = 0; b < cbPer / 16; ++b)
    {
        const uint32_t ibCh = b * 16;  // the byte in the channel's data
        const uint64_t ib = fVag ? uint64_t(ibData) + ibCh : uint64_t(ibData) + (uint64_t(ibCh / 0x1000) * nch + ich) * 0x1000 + ibCh % 0x1000;
        for (int k = 0; k < 16; ++k) block[k] = ib + k < ab.size() ? ab[std::size_t(ib + k)] : 0;
        DecodeAdpcmBlock(block, &as[std::size_t(b) * 28], &h1, &h2);
    }
    return as;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool FWriteOut(const fs::path& root, const std::string& rel, const std::vector<uint8_t>& ab, uint64_t& cbOut, std::string& error)
{
    if (!FWriteFile(root / cane::path(rel), ab.data(), ab.size())) { error = "cannot write " + rel; return false; }
    cbOut += ab.size();
    return true;
}

std::string WithExt(const std::string& rel, const char* ext)
{
    const auto dot = rel.rfind('.');
    return rel.substr(0, dot) + ext;
}

// ab: the file's bytes from the disc; on return, the main file's (written by the caller at f.path). Parts are written
// here and listed in e.
bool FConvertAudio(FILEINFO& f, std::vector<uint8_t>& ab, const fs::path& root, json& e, uint64_t& cbOut, std::string& error)
{
    if ((f.kind == "music" || f.kind == "ambience" || f.kind == "dialog") && ab.size() >= 0x30)
    {
        if (!std::memcmp(ab.data(), "VAGp", 4))
        {
            const uint32_t cb = Be32(ab.data() + 0xC), rate = Be32(ab.data() + 0x10);
            const std::vector<int16_t> as = AsDecodeStream(ab, 0x30, cb, 1, 0, true);
            // (no smpl: a looping VAG loops from its start; so does this)
            ab = FConvertFlac() ? AbFlac(int(rate), 1, as.data(), as.size(), &error) : AbWav(int(rate), 1, as.data(), as.size());
            if (ab.empty()) return false;
            e["frames"] = as.size();
            return true;
        }
        if (!std::memcmp(ab.data(), " KPV", 4) && ab.size() >= 0x800)
        {
            const uint32_t cbPer = Le32(ab.data() + 4), ibData = Le32(ab.data() + 8), rate = Le32(ab.data() + 0x10);
            const uint32_t nch = Le32(ab.data() + 0x14), loop = Le32(ab.data() + 0x7FC);
            if (nch == 0 || nch >= 6 || rate == 0) { error = "bad VPK header"; return false; }
            if (cbPer % 16 || loop % 16) { error = "a VPK size or loop off the 16-byte blocks"; return false; }
            std::vector<std::vector<int16_t>> aas;
            std::vector<WAVSTEM> astem;
            for (uint32_t i = 0; i < nch; ++i)
            {
                aas.push_back(AsDecodeStream(ab, ibData, cbPer, nch, i, false));
                astem.push_back({ab[0x18 + i * 4], int8_t(ab[0x19 + i * 4]), int16_t(ab[0x1A + i * 4] | ab[0x1B + i * 4] << 8)});
            }
            const std::size_t cframe = aas[0].size();
            std::vector<int16_t> as(cframe * nch);
            for (std::size_t k = 0; k < cframe; ++k)
                for (uint32_t i = 0; i < nch; ++i) as[k * nch + i] = aas[i][k];
            // the loop: spiop goes on at the loop start after the end (with the decoder's history; every disc's loop
            // decodes to the same samples either way: checked by the converter below)
            WAVINFO wl;
            wl.fSmpl = true;
            wl.fLoop = loop < cbPer;
            wl.iframeLoop = loop / 16 * 28;
            wl.iframeLoopEnd = cframe;
            if (wl.fLoop)
                for (uint32_t i = 0; i < nch; ++i)
                {
                    // the loop entered with the end's history must decode as the first pass did (else the WAV could not
                    // hold both: this would need the loop's own copy)
                    int h1 = 0, h2 = 0;
                    const std::vector<int16_t>& a = aas[i];
                    h1 = a.size() >= 1 ? a[a.size() - 1] : 0;
                    h2 = a.size() >= 2 ? a[a.size() - 2] : 0;
                    uint8_t block[16];
                    int16_t s28[28];
                    for (uint32_t b = loop / 16; b < cbPer / 16 && b < loop / 16 + 64; ++b)
                    {
                        const uint64_t ib = uint64_t(ibData) + (uint64_t((b * 16) / 0x1000) * nch + i) * 0x1000 + (b * 16) % 0x1000;
                        for (int k = 0; k < 16; ++k) block[k] = ib + k < ab.size() ? ab[std::size_t(ib + k)] : 0;
                        DecodeAdpcmBlock(block, s28, &h1, &h2);
                        if (std::memcmp(s28, &a[std::size_t(b) * 28], sizeof(s28)) != 0)
                        {
                            error = "the loop decodes differently when entered from the end (channel " + std::to_string(i) + ")";
                            return false;
                        }
                    }
                }
            ab = AbWav(int(rate), int(nch), as.data(), cframe, &wl, &astem);
            e["frames"] = cframe;
            if (wl.fLoop) e["loop_frame"] = wl.iframeLoop;
            json aid = json::array();
            for (const WAVSTEM& st : astem) aid.push_back(st.id);
            e["stems"] = aid;
            return true;
        }
        return true;  // (not an ADPCM stream: kept as it is)
    }
    if (f.kind == "movie")
    {
        std::vector<uint8_t> abVideo;
        std::map<int, MOVIEAUDIOTRACK> mpaudio;
        // (from the bytes read: the movie is not read from the disc a second time)
        if (!FDemuxMovie(ab, abVideo, mpaudio)) { error = "not a PSS"; return false; }
        json aaudio = json::array();
        for (const auto& [track, t] : mpaudio)
        {
            std::string rel = WithExt(f.path, FConvertFlac() ? ".flac" : ".wav");
            json ea = {{"track", track}};
            if (track > 0)
            {
                const int lang = track - 1;
                const char* name = lang < LANG_Max ? s_aszLanguageName[lang] : "Unknown";
                rel = "Movies/" + std::string(name) + "/" + rel.substr(7);
                ea["language"] = name;
            }
            const std::vector<uint8_t> abWav = FConvertFlac() ? AbFlac(t.rate, 2, t.asample.data(), t.asample.size() / 2, &error)
                                                              : AbWav(t.rate, 2, t.asample.data(), t.asample.size() / 2);
            if (abWav.empty() || !FWriteOut(root, rel, abWav, cbOut, error)) return false;
            ea["path"] = rel;
            ea["frames"] = t.asample.size() / 2;
            aaudio.push_back(ea);
        }
        if (!aaudio.empty()) e["audio"] = aaudio;
        ab = std::move(abVideo);
        return true;
    }
    if (f.kind == "bank")
    {
        std::vector<SNDBANKSAMPLE> asample;
        if (!snd_DecodeBankSamples(ab, asample)) return true;  // (not a bank the engine reads: kept, no samples)
        const std::string dir = f.path.substr(0, f.path.rfind('/') + 1) + "Sounds/";
        std::map<int, int> mpsoundc;  // samples named after each sound so far
        json as = json::array();
        for (const SNDBANKSAMPLE& smp : asample)
        {
            const int isound = smp.asound.empty() ? -1 : smp.asound[0];
            const int c = ++mpsoundc[isound];
            char sz[48];
            if (c == 1) std::snprintf(sz, sizeof(sz), "Sound %03d.wav", isound);
            else std::snprintf(sz, sizeof(sz), "Sound %03d-%d.wav", isound, c);
            WAVINFO wl;
            wl.fSmpl = true;
            wl.fLoop = smp.loop;
            wl.iframeLoop = smp.loopBegin;
            wl.iframeLoopEnd = uint64_t(smp.loopBegin) + smp.loopLength;
            if (!FWriteOut(root, dir + sz, AbWav(48000, 1, smp.pcm.data(), smp.pcm.size(), &wl), cbOut, error)) return false;
            json es = {{"offset", smp.offset}, {"path", dir + sz}, {"sounds", smp.asound}};
            as.push_back(es);
        }
        if (!as.empty()) e["samples"] = as;
        return true;
    }
    return true;
}
}  // namespace

bool g_fConvertPs2 = false;
int g_nConvertFlac = -1;
bool g_fConvertTextures = true;
bool g_fConvertWholeWorlds = false;

const char* SzConvertWorldName(int iwrec) { return SzWorldName(iwrec, nullptr); }

// --convert-list: the crawl's files and names, nothing written
int ConvertListCommand()
{
    std::string error;
    if (!FCrawl(error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    CheckWorldCodes();
    AssignPaths();
    std::map<std::string, std::pair<int, double>> mpkind;
    for (const auto& [isector, f] : s_mpfile)
    {
        auto& k = mpkind[f.kind];
        ++k.first;
        k.second += f.fk.cb / 1048576.0;
        std::printf("0x%06X %10u id %5u %-8s", isector, f.fk.cb, f.fk.id, f.kind.c_str());
        if (f.kind == "music" || f.kind == "ambience") std::printf(" %s:%d %.0f s", f.iworld >= 0 ? s_aworld[f.iworld].code.c_str() : "-", f.id, f.tSeconds);
        std::printf("  %s  (%zu worlds)\n", f.path.c_str(), f.aiworld.size());
    }
    for (const auto& [k, v] : mpkind) std::printf("%-8s %5d files %8.1f MB\n", k.c_str(), v.first, v.second);
    for (const std::string& s : s_awarning) std::printf("warning: %s\n", s.c_str());
    return 0;
}

// --convert OUT: the layout written into OUT (an empty or new folder; cane-extractor stages it)
int ConvertCommand(const std::string& out)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto tNow = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    std::string error;
    const fs::path root = cane::path(out);
    fs::create_directories(root);
    // the worker threads: the textures' PNGs while the crawl loads the worlds, then every file (CONVERTER.md "Speed")
    CWorkPool pool;
    if (g_fConvertTextures)
    {
        s_dirTextures = cane::utf8(root / "Textures");
        fs::create_directories(cane::path(s_dirTextures));
        s_ppoolTextures = &pool;
    }
    const bool fCrawl = FCrawl(error);
    s_ppoolTextures = nullptr;
    if (!fCrawl) { std::fprintf(stderr, "convert: %s\n", error.c_str()); return 1; }
    const double tCrawl = tNow();
    CheckWorldCodes();
    AssignPaths();
    // the executable and SYSTEM.CNF (the game's data tables; what the disc is)
    const std::string szExe = g_disc.SzExecutable();
    for (const std::string& name : {szExe, std::string("SYSTEM.CNF")})
    {
        std::vector<uint8_t> ab;
        if (!g_disc.FReadRootFile(name, ab) || !FWriteFile(root / name, ab.data(), ab.size()))
        {
            std::fprintf(stderr, "convert: cannot copy %s\n", name.c_str());
            return 1;
        }
    }
    // every file a job: read, converted, written; its manifest entry kept in the files' order (the manifest is the same
    // whatever the order the jobs finish in). The largest first, so the long ones (movies) do not end the run alone.
    struct JOBOUT
    {
        json e;
        std::string error;
        uint64_t cbOut = 0;
    };
    std::vector<std::pair<const uint32_t, FILEINFO>*> ajob;
    for (auto& kv : s_mpfile) ajob.push_back(&kv);
    std::vector<JOBOUT> aout(ajob.size());
    std::vector<std::size_t> aorder(ajob.size());
    for (std::size_t k = 0; k < aorder.size(); ++k) aorder[k] = k;
    std::stable_sort(aorder.begin(), aorder.end(), [&](std::size_t a, std::size_t b) { return ajob[a]->second.fk.cb > ajob[b]->second.fk.cb; });
    std::mutex mtx;   // (the warnings, the progress lines)
    std::size_t cdone = 0;
    auto convertOne = [&](std::size_t i) {
        const uint32_t isector = ajob[i]->first;
        FILEINFO& f = ajob[i]->second;
        JOBOUT& o = aout[i];
        std::string& error = o.error;
        uint64_t& cbOut = o.cbOut;
        std::vector<uint8_t> ab(f.fk.cb);
        if (!FReadDisc(isector, 0, ab.data(), ab.size()))
        {
            char sz[64];
            std::snprintf(sz, sizeof(sz), "cannot read sector 0x%X", isector);
            error = sz;
            return;
        }
        const bool fCompressed = f.kind == "world" || f.kind == "screen";
        if (fCompressed)
        {
            try { ab = cane::decompressBrx(ab); }
            catch (const std::exception& x)
            {
                error = f.path + ": " + x.what();
                return;
            }
        }
        json e = {{"path", f.path}, {"kind", f.kind}, {"sector", isector}, {"disc_size", f.fk.cb}, {"id", f.fk.id}};
        // (format 3) a world as its parts (worldparts.h): the files beside the index, the index at f.path
        if (f.kind == "world" && FUnravel(isector))
        {
            json index;
            std::vector<WORLDPARTFILE> afile;
            std::vector<std::string> awarning;
            if (!FUnravelWorld(ab, s_mpamark.at(isector), s_mpaalign.at(isector), s_mpagroup.at(isector), FWideText(), index, afile, awarning, error))
            {
                error = f.path + ": " + error;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                for (const std::string& w : awarning) s_awarning.push_back(f.path + ": " + w);
            }
            const std::string dir = f.path.substr(0, f.path.rfind('/') + 1);
            for (const WORLDPARTFILE& part : afile)
                if (!FWriteOut(root, dir + part.rel, part.ab, cbOut, error)) return;
            index["world"] = f.name;
            for (const WORLDINFO& w : s_aworld)   // (the level's groups: a guest's kept world leaves them out, pcloader.h)
                if (w.fk.isector == isector && w.fk.cb && !w.code.empty() && w.code != "frontend")
                {
                    MarkLevelGroups(index, w.code);
                    break;
                }
            const std::string sIndex = index.dump(1) + "\n";
            ab.assign(sIndex.begin(), sIndex.end());
            e["layout"] = "parts";
            e["world_size"] = index["size"];
        }
        // (format 2) the audio as WAV, the movies as M2V + WAV, the banks' samples as WAV beside them
        if (!g_fConvertPs2 && !FConvertAudio(f, ab, root, e, cbOut, error))
        {
            error = f.path + ": " + error;
            return;
        }
        if (!FWriteFile(root / cane::path(f.path), ab.data(), ab.size()))
        {
            error = "cannot write " + f.path;
            return;
        }
        e["size"] = ab.size();
        cbOut += ab.size();
        if (!f.asectorCopy.empty()) e["copies"] = f.asectorCopy;
        if (fCompressed) e["decompressed"] = true;
        if (!f.name.empty()) e["name"] = f.name;
        if (f.kind == "dialog") { e["line"] = f.id; e["language"] = f.lang >= 0 && f.lang < LANG_Max ? s_aszLanguageName[f.lang] : "?"; }
        if (f.kind == "music") e["music_id"] = f.id;
        if (f.tSeconds > 0) e["seconds"] = std::round(f.tSeconds * 10) / 10;
        if (!f.aiworld.empty())
        {
            json aw = json::array();
            for (int iw : f.aiworld) aw.push_back(s_aworld[iw].iwrec);
            e["worlds"] = aw;
        }
        if (!f.ava.empty())
        {
            json av = json::array();
            for (uint32_t va : f.ava) { char sz[16]; std::snprintf(sz, sizeof(sz), "0x%06X", va); av.push_back(sz); }
            e["executable_keys"] = av;
        }
        o.e = std::move(e);
        std::lock_guard<std::mutex> lock(mtx);
        if (++cdone % 50 == 0 || cdone == ajob.size())
        {
            std::printf("progress %zu %zu %s\n", cdone, ajob.size(), f.path.c_str());
            FProgress(cdone, ajob.size(), "Converting the files", f.path.c_str());   // (the Switch installer's progress)
            std::fflush(stdout);
        }
    };
    auto ajobPath = [&](std::size_t i) { return ajob[i]->second.path; };
    for (std::size_t i : aorder)
    {
        // (the memory a job holds: its disc bytes and its decoded PCM, about 4x; a world's parts 2x)
        const uint64_t cbWeight = uint64_t(ajob[i]->second.fk.cb) * 4;
        pool.Push([&convertOne, &aout, &ajobPath, i] {
            try { convertOne(i); }
            catch (const std::exception& e) { aout[i].error = ajobPath(i) + ": " + e.what(); }
        }, cbWeight);
    }
    pool.Wait();
    json files = json::array();
    uint64_t cbDisc = 0, cbOut = 0;
    for (std::size_t i = 0; i < ajob.size(); ++i)
    {
        if (!aout[i].error.empty())
        {
            std::fprintf(stderr, "convert: %s\n", aout[i].error.c_str());
            return 1;
        }
        files.push_back(std::move(aout[i].e));
        cbDisc += ajob[i]->second.fk.cb;
        cbOut += aout[i].cbOut;
    }
    const double tFiles = tNow();
    json worlds = json::array();
    for (const WORLDINFO& w : s_aworld)
    {
        json e = {{"record", w.iwrec}, {"code", w.code}, {"name", w.name}, {"episode", w.episode}, {"kind", w.kind}};
        if (w.fk.cb) e["path"] = PathOfSector(w.fk.isector);
        json am = json::array();
        for (const auto& [id, isector] : w.amusic) am.push_back({{"id", id}, {"path", PathOfSector(isector)}});
        if (!am.empty()) e["music"] = am;
        worlds.push_back(e);
    }
    json langs = json::array();
    for (int k = 0; k < CLanguagesDisc(); ++k) langs.push_back(s_aszLanguageName[LanguageOfDiscIndex(k)]);
    json manifest = {
        {"game", "sly2"},
        {"format", "sly2-files"},
        {"format_version", g_fConvertPs2 ? SLY2_FILES_FORMAT_PS2 : SLY2_FILES_FORMAT},
        {"disc", g_disc.SzDisc()},
        {"region", g_disc.Region() == DISCREGION::Usa ? "USA" : g_disc.Region() == DISCREGION::Europe ? "Europe" : "Japan"},
        {"executable", szExe},
        {"languages", langs},
        {"worlds", worlds},
        {"files", files},
    };
    {
        std::ofstream o(root / "extraction_manifest.json", std::ios::trunc);
        o << manifest.dump(1) << '\n';
        if (!o) { std::fprintf(stderr, "convert: cannot write the manifest\n"); return 1; }
    }
    // the textures (on unless --no-export-textures): every world's textures as the PNGs Mods/Textures takes (texmod.cpp),
    // in Textures/ (one file per texture: the names are their contents' hashes, so a texture several worlds draw is
    // written once). Not in the manifest: the game never reads them (cane-extractor leaves them out of the inventory).
    // (written during the crawl: CONVERTER.md "Textures")
    if (g_fConvertTextures) std::printf("%zu textures written to %s\n", s_setTextureName.size(), s_dirTextures.c_str());
    for (const std::string& s : s_awarning) std::printf("warning: %s\n", s.c_str());
    std::printf("converted %zu files: %.1f MB on the disc, %.1f MB written, %.1f s (crawl and textures %.1f s, files %.1f s, %d threads)\n",
                s_mpfile.size(), cbDisc / 1048576.0, cbOut / 1048576.0, tNow(), tCrawl, tFiles - tCrawl, pool.Cthread());
    return 0;
}

// (languages, langstore.h)
bool FConvertLanguageFiles(std::vector<CONVLANGFILE>& afile, std::string& error)
{
    if (!FCrawl(error)) return false;
    AssignPaths();
    for (const auto& [isector, f] : s_mpfile)
    {
        if (f.kind != "dialog" && f.kind != "movie") continue;
        CONVLANGFILE c;
        c.kind = f.kind;
        c.isector = isector;
        c.cb = f.fk.cb;
        c.lang = f.lang;
        c.id = f.id;
        if (f.kind == "movie" && !f.name.empty())
        {
            const auto sp = f.name.find(' ');
            c.name = sp == std::string::npos ? std::string() : f.name.substr(sp + 1);
        }
        for (int iw : f.aiworld) c.aiwrec.push_back(s_aworld[iw].iwrec);
        afile.push_back(std::move(c));
    }
    return true;
}

std::vector<uint8_t> AbConvertDialogVag(const std::vector<uint8_t>& abVag, bool fFlac, std::size_t* pcframe, std::string& error)
{
    if (abVag.size() < 0x30 || std::memcmp(abVag.data(), "VAGp", 4)) { error = "not a VAG"; return {}; }
    const uint32_t cb = Be32(abVag.data() + 0xC), rate = Be32(abVag.data() + 0x10);
    const std::vector<int16_t> as = AsDecodeStream(abVag, 0x30, cb, 1, 0, true);
    if (pcframe) *pcframe = as.size();
    return fFlac ? AbFlac(int(rate), 1, as.data(), as.size(), &error) : AbWav(int(rate), 1, as.data(), as.size());
}
#ifdef __SWITCH__
// The Switch installer's conversion (platform/switch/installer.cpp): the disc image opened and checked, its files
// converted into out. The textures' PNG dump (for modders) is left out: minutes of the console's CPU, ~80 MB.
std::string CaneConvertDisc(const std::string& iso, const std::string& out, const CaneConvertProgress& progress)
{
    s_pprogress = &progress;
    s_fCancelled = false;
    struct Reset { ~Reset() { s_pprogress = nullptr; } } reset;
    if (!FProgress(0, 1, "Checking the disc image", iso.c_str())) return "cancelled";
    std::string error;
    if (!g_disc.FOpen(iso, error)) return error;
    InitLanguage();
    g_fConvertTextures = false;
    // (WAV, never FLAC: encoding the European discs' dialog in five languages would take the console's CPU a long
    // time, and the game then decodes no FLAC while playing; it costs some SD space)
    g_nConvertFlac = 0;
    const int rc = ConvertCommand(out);
    if (s_fCancelled) return "cancelled";
    if (rc != 0) return "The conversion stopped with an error.";
    return "";
}
#endif
