// See langstore.h.
#include "langstore.h"

#include "cls.h"
#include "convert.h"
#include "fmv.h"
#include "flacfile.h"
#include "hud.h"
#include "porttext.h"
#include "promptglyph.h"
#include "region.h"
#include "screenobj.h"
#include "sw.h"
#include "textmod.h"
#include "wavfile.h"

#include "../platform/platform.h"
#include "../platform/sly2_settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

LANGCAPTURE* g_plangcapture = nullptr;

namespace
{
const char* const s_aszGameName[LANG_Max] = {"English", "French", "Italian", "German", "Spanish", "Dutch", "Portuguese",
                                             "Danish", "Finnish", "Norwegian", "Swedish", "Japanese", "Korean", "Chinese"};
const char* const s_aszIso[LANG_Max] = {"en", "fr", "it", "de", "es", "nl", "pt", "da", "fi", "no", "sv", "ja", "ko", "zh"};
// (the languages' own names, as the disc's text 0x58.. has them; Japanese / Korean / Chinese in theirs)
const char* const s_aszNative[LANG_Max] = {"English", "Français", "Italiano", "Deutsch", "Español", "Nederlands",
                                           "Português", "Dansk", "Suomi", "Norsk", "Svenska", "日本語", "한국어", "中文"};

struct LANGPACK
{
    std::string key;                    // the folder's name
    int lang = -1;                      // the game's id, or LANG_Max + n
    std::string name;                   // UTF-8 display name
    std::string code;
    std::string source;                 // "USA", "Europe v2.01", "Japan", "PS3", "" (user-made)
    std::string textFrom, voiceFrom;    // fallbacks (folder names)
    bool fTextFromExplicit = false;
    bool fWide = false;                 // its text needs the Japanese font
    std::vector<fs::path> adir;         // the store's folder, then the mods' (later ones win)
    bool fText = false;                 // has Text.json
    std::map<int, uint32_t> mpline;     // USA line id -> external sector
    std::map<int, float> mplength;      // USA line id -> seconds (lengths.json)
    std::map<std::string, std::string> mpmovie;   // movie name -> sound file
    // the mods folder's own (Mods/Languages/<key>/): over a disc language too
    std::map<int, uint32_t> mplineMod;
    std::map<std::string, std::string> mpmovieMod;
    std::vector<fs::path> adirMod;
    bool fTextModLoaded = false;
    std::unordered_map<int, std::string> mptextMod;
    // the strings, loaded at first use
    bool fTextLoaded = false;
    std::unordered_map<int, std::string> mptext;  // USA id -> UTF-8
    // Font.bin, loaded at first use (s_mtxFont)
    bool fFontTried = false;
    std::shared_ptr<SELEM> pfont;
};

std::vector<LANGPACK> s_apack;
std::vector<int> s_alangOffered;
std::mutex s_mtx;
std::mutex s_mtxFont;
std::atomic<const SELEM*> s_psetFontShown{nullptr};   // (LatchLanguageStoreText)

LANGPACK* PpackOfLang(int lang)
{
    for (LANGPACK& p : s_apack)
        if (p.lang == lang) return &p;
    return nullptr;
}
LANGPACK* PpackOfKey(const std::string& key)
{
    for (LANGPACK& p : s_apack)
        if (p.key == key) return &p;
    return nullptr;
}

std::string StrLower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

json JsonRead(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return json();
    try
    {
        std::ifstream in(path);
        return json::parse(in, nullptr, true, true);
    }
    catch (const std::exception& e)
    {
        std::printf("languages: %s: %s (ignored)\n", cane::utf8(path).c_str(), e.what());
        return json();
    }
}

bool FAudioExt(const fs::path& p)
{
    const std::string ext = StrLower(cane::utf8(p.extension()));
    return ext == ".wav" || ext == ".flac" || ext == ".vag";
}

bool FParseLine(const std::string& stem, int& id)
{
    if (stem.empty() || stem.size() > 6) return false;
    for (char c : stem)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    id = std::atoi(stem.c_str());
    return true;
}

// a WAV's or FLAC's length in seconds (0: unknown)
float TAudioFile(const std::string& path)
{
    std::ifstream in(cane::path(path), std::ios::binary);
    uint8_t h[64] = {};
    in.read(reinterpret_cast<char*>(h), sizeof(h));
    if (in.gcount() < 42) return 0.0f;
    if (FIsFlac(h, 4) && (h[4] & 0x7F) == 0)   // STREAMINFO first
    {
        const uint8_t* si = h + 8;
        const uint32_t rate = uint32_t(si[10]) << 12 | uint32_t(si[11]) << 4 | si[12] >> 4;
        const uint64_t c = uint64_t(si[13] & 0x0F) << 32 | uint64_t(si[14]) << 24 | uint64_t(si[15]) << 16 | uint64_t(si[16]) << 8 | si[17];
        return rate ? float(double(c) / rate) : 0.0f;
    }
    std::error_code ec;
    const uint64_t cb = fs::file_size(cane::path(path), ec);
    WAVINFO wi;
    if (!ec && FParseWav(PfnReadFile(path), cb, wi) && wi.rate) return float(double(wi.cframe) / wi.rate);
    return 0.0f;
}

// UTF-8 Latin text for the Japanese disc's font (ASCII, kana, kanji: no accents)
std::string StrFoldLatin(const std::string& utf8)
{
    std::string out;
    for (std::size_t i = 0; i < utf8.size();)
    {
        const unsigned char c = utf8[i];
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 0x07; n = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
        ++i;
        for (int k = 0; k < n && i < utf8.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        if (cp < 0x80) { out += char(cp); continue; }
        static const struct { uint32_t lo, hi; const char* sz; } s_afold[] = {
            {0x85, 0x85, "..."}, {0x2026, 0x2026, "..."}, {0x91, 0x92, "'"}, {0x2018, 0x2019, "'"}, {0x93, 0x94, "\""},
            {0x201C, 0x201D, "\""}, {0x96, 0x97, "-"}, {0x2013, 0x2014, "-"}, {0xA0, 0xA0, " "}, {0xA1, 0xA1, "!"},
            {0xBF, 0xBF, "?"}, {0xAB, 0xAB, "\""}, {0xBB, 0xBB, "\""}, {0xAE, 0xAE, ""}, {0xA9, 0xA9, "(C)"}, {0xB0, 0xB0, "o"},
            {0xC0, 0xC5, "A"}, {0xC6, 0xC6, "AE"}, {0xC7, 0xC7, "C"}, {0xC8, 0xCB, "E"}, {0xCC, 0xCF, "I"}, {0xD0, 0xD0, "D"},
            {0xD1, 0xD1, "N"}, {0xD2, 0xD6, "O"}, {0xD8, 0xD8, "O"}, {0xD9, 0xDC, "U"}, {0xDD, 0xDD, "Y"}, {0xDF, 0xDF, "ss"},
            {0xE0, 0xE5, "a"}, {0xE6, 0xE6, "ae"}, {0xE7, 0xE7, "c"}, {0xE8, 0xEB, "e"}, {0xEC, 0xEF, "i"}, {0xF0, 0xF0, "d"},
            {0xF1, 0xF1, "n"}, {0xF2, 0xF6, "o"}, {0xF8, 0xF8, "o"}, {0xF9, 0xFC, "u"}, {0xFD, 0xFD, "y"}, {0xFF, 0xFF, "y"},
            {0x152, 0x152, "OE"}, {0x153, 0x153, "oe"}};
        const char* sz = "?";
        for (const auto& f : s_afold)
            if (cp >= f.lo && cp <= f.hi) sz = f.sz;
        if (cp >= 0x3000) { AppendCh16(out, cp); continue; }   // (kana / kanji / full-width forms the font may have)
        out += sz;
    }
    return out;
}

}  // namespace

std::string StrFoldLatinForWideFont(const std::string& utf8) { return StrFoldLatin(utf8); }

namespace
{
// ---- Font.bin: a Japanese language's text font, as the import found it in the Japanese disc's world files (the
// screen objects' font set: screenobj.cpp). Little endian:
//   "S2FONT1\0"; U32 glyph count; then each glyph, by ascending character:
//     U16 character (UTF-16), F32 advance width (a full-width character: 51.2), U16 polygon count, each polygon:
//       U8 primitive (1 fan, 2 strip, 3 line strip), U8 layer, U8 unused, U8 vertex flags, U32 colour (RGBA),
//       U16 vertex count, each vertex: S16 x, S16 y (12.4 fixed point), U8 vertex byte
constexpr char s_achFontMagic[8] = {'S', '2', 'F', 'O', 'N', 'T', '1', 0};

std::vector<uint8_t> AbFontFile(const std::map<uint32_t, std::pair<float, SELEM>>& mpglyph)
{
    std::vector<uint8_t> ab(s_achFontMagic, s_achFontMagic + 8);
    auto u8 = [&](uint8_t v) { ab.push_back(v); };
    auto u16 = [&](uint32_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    u32(uint32_t(mpglyph.size()));
    for (const auto& [ch, g] : mpglyph)
    {
        u16(ch);
        uint32_t w;
        std::memcpy(&w, &g.first, 4);
        u32(w);
        u16(uint32_t(g.second.apoly.size()));
        for (const SPOLY& poly : g.second.apoly)
        {
            u8(poly.primitive); u8(poly.control); u8(poly.unused); u8(poly.fVertexBytes);
            u32(poly.rgba);
            u16(uint32_t(poly.axy.size()));
            for (std::size_t i = 0; i < poly.axy.size(); ++i)
            {
                u16(uint16_t(poly.axy[i][0])); u16(uint16_t(poly.axy[i][1]));
                u8(i < poly.ab.size() ? poly.ab[i] : 0);
            }
        }
    }
    return ab;
}

std::shared_ptr<SELEM> PfontFromBytes(const std::vector<uint8_t>& ab)
{
    std::size_t ib = 8;
    bool fOk = ab.size() >= 12 && std::memcmp(ab.data(), s_achFontMagic, 8) == 0;
    auto u8 = [&]() -> uint32_t { if (ib + 1 > ab.size()) { fOk = false; return 0; } return ab[ib++]; };
    auto u16 = [&]() -> uint32_t { const uint32_t lo = u8(); return lo | (u8() << 8); };
    auto u32 = [&]() -> uint32_t { const uint32_t lo = u16(); return lo | (u16() << 16); };
    auto pset = std::make_shared<SELEM>();
    pset->kind = SELEMK_Set;
    pset->asub.assign(1, SELEM{});
    const uint32_t c = fOk ? u32() : 0;
    for (uint32_t i = 0; i < c && fOk; ++i)
    {
        const uint32_t ch = u16();
        const uint32_t w = u32();
        float dx;
        std::memcpy(&dx, &w, 4);
        SELEM g;
        g.kind = SELEMK_Sprite;
        g.idx = uint32_t(pset->asub.size());
        g.apoly.resize(u16());
        for (SPOLY& poly : g.apoly)
        {
            if (!fOk) break;
            poly.primitive = uint8_t(u8()); poly.control = uint8_t(u8()); poly.unused = uint8_t(u8()); poly.fVertexBytes = uint8_t(u8());
            poly.rgba = u32();
            const uint32_t cv = u16();
            if (ib + std::size_t(cv) * 5 > ab.size()) { fOk = false; break; }
            poly.axy.resize(cv);
            poly.ab.resize(cv);
            for (uint32_t j = 0; j < cv; ++j)
            {
                poly.axy[j][0] = int16_t(u16()); poly.axy[j][1] = int16_t(u16());
                poly.ab[j] = uint8_t(u8());
            }
        }
        if (!pset->aul.empty() && ch <= pset->aul.back()) fOk = false;   // (ascending: the glyph lookup's binary search)
        pset->aul.push_back(ch);
        pset->ag.push_back(dx);
        pset->asub.push_back(std::move(g));
    }
    if (!fOk || pset->aul.empty() || ib != ab.size()) return nullptr;
    return pset;
}

std::shared_ptr<SELEM> PfontFromFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    const std::vector<uint8_t> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::shared_ptr<SELEM> pset = PfontFromBytes(ab);
    if (!pset) std::printf("languages: %s is not a font file (ignored)\n", cane::utf8(path).c_str());
    return pset;
}

// Latin-1 bytes as UTF-8 (the USA / European discs' own strings)
std::string Utf8OfLatin1Bytes(const std::string& s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (c < 0x80) out += char(c);
        else { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

// The menus' button lines (USA ids). The Japanese build confirms with Circle, goes back with Cross and erases a
// slot with Square; the other builds with Cross, Triangle and Circle (region.h BtnMenuConfirm). A language taken from
// a disc of the other convention names the buttons of the disc that is played. (Found by comparing the button
// glyphs of every USA and Japanese string: these are the ones that differ by the convention.)
constexpr int s_aidMenuButtons[] = {0x25, 0x26, 0x27, 0x28, 0x29, 0x2D, 0x31, 0x34, 0x6C, 0xDA, 0xDB, 0xE1, 0xE2,
                                    0x131, 0x132, 0x133, 0x134, 0x29F, 0xCAB};
void MenuButtonGlyphs(int idUsa, std::string& str, bool fToJapan)
{
    if (std::find(std::begin(s_aidMenuButtons), std::end(s_aidMenuButtons), idUsa) == std::end(s_aidMenuButtons)) return;
    bool fIcons = false;
    for (std::size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '&' && i + 1 < str.size())
        {
            if (str[i + 1] != '&') fIcons = str[i + 1] != '.';
            ++i;
            continue;
        }
        if (!fIcons) continue;
        char& ch = str[i];
        // (0x28, the overwrite page: both builds erase with Square there)
        if (fToJapan) ch = ch == 'X' ? 'O' : ch == 'T' ? 'X' : ch == 'O' && idUsa != 0x28 ? 'S' : ch;
        else ch = ch == 'O' ? 'X' : ch == 'X' ? 'T' : ch == 'S' && idUsa != 0x28 ? 'O' : ch;
    }
}
}  // namespace

namespace
{
// a store string in this disc's text encoding
std::string StrToDisc(const std::string& utf8, bool fWidePack)
{
    if (!FWideChars()) return GameTextOfUtf8(utf8);
    if (fWidePack) return utf8;
    return StrFoldLatin(utf8);
}

// A Japanese-script pack's font: its own Font.bin (a mod's over the store's), else the "Japanese" folder's (a text
// variant made for the same font). Loaded at first use; nullptr: none.
const SELEM* PfontOfPack(LANGPACK& p)
{
    std::lock_guard<std::mutex> lock(s_mtxFont);
    for (LANGPACK* pp : {&p, p.key == "Japanese" ? static_cast<LANGPACK*>(nullptr) : PpackOfKey("Japanese")})
    {
        if (!pp || !pp->fWide) continue;
        if (!pp->fFontTried)
        {
            pp->fFontTried = true;
            std::error_code ec;
            for (auto it = pp->adir.rbegin(); it != pp->adir.rend() && !pp->pfont; ++it)
                if (fs::is_regular_file(*it / "Font.bin", ec)) pp->pfont = PfontFromFile(*it / "Font.bin");
        }
        if (pp->pfont) return pp->pfont.get();
    }
    return nullptr;
}

bool FTextHasWide(const std::unordered_map<int, std::string>& mp)
{
    for (const auto& [id, s] : mp)
        for (std::size_t i = 0; i < s.size(); ++i)
            if (static_cast<unsigned char>(s[i]) >= 0xE0) return true;   // (a 3-byte UTF-8 sequence: U+0800..)
    return false;
}

void LoadPackText(LANGPACK& p)
{
    if (p.fTextLoaded) return;
    p.fTextLoaded = true;
    for (const fs::path& dir : p.adir)
    {
        const json j = JsonRead(dir / "Text.json");
        if (!j.is_object() || !j.contains("strings")) continue;
        for (const auto& [k, v] : j.at("strings").items())
        {
            char* end = nullptr;
            const long id = std::strtol(k.c_str(), &end, 0);
            if (k.empty() || *end || !v.is_string()) continue;
            p.mptext[int(id)] = v.get<std::string>();
        }
    }
}

// The pack a language's text / voice comes from, following the fallbacks (nullptr: the disc's own)
LANGPACK* PpackText(LANGPACK* p)
{
    for (int i = 0; p && i < 8; ++i)
    {
        if (FLanguageOnDisc(p->lang)) return nullptr;
        if (p->fText) return p;
        p = PpackOfKey(p->textFrom.empty() ? std::string("English") : p->textFrom);
    }
    return nullptr;
}

std::string RegionFamily(const std::string& disc)
{
    return disc.substr(0, disc.find(' '));
}
}  // namespace

int LangOfGameName(const std::string& name)
{
    for (int i = 0; i < LANG_Max; ++i)
        if (name == s_aszGameName[i]) return i;
    return -1;
}

void InitLanguageStore()
{
    std::lock_guard<std::mutex> lock(s_mtx);
    s_apack.clear();
    s_alangOffered.clear();
    // the folders: the store's, then the mods folder's Languages/ (a mod adds to a language or makes a new one)
    std::map<std::string, std::vector<fs::path>> mpkeydir;
    auto scan = [&](const fs::path& root) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec)) return;
        for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
            if (e.is_directory(ec) && cane::utf8(e.path().filename())[0] != '.') mpkeydir[cane::utf8(e.path().filename())].push_back(e.path());
    };
    if (!g_disc.DirLanguages().empty()) scan(cane::path(g_disc.DirLanguages()));
    if (!g_disc.DirMods().empty()) scan(cane::path(g_disc.DirMods()) / "Languages");
    int langCustom = LANG_Max;
    for (const auto& [key, adir] : mpkeydir)
    {
        LANGPACK p;
        p.key = key;
        p.adir = adir;
        p.lang = LangOfGameName(key);
        if (p.lang < 0) p.lang = langCustom++;
        if (p.lang < LANG_Max) { p.name = s_aszNative[p.lang]; p.code = s_aszIso[p.lang]; }
        else p.name = key;
        for (const fs::path& dir : adir)
        {
            const json j = JsonRead(dir / "language.json");
            if (j.is_object())
            {
                p.name = j.value("name", p.name);
                p.code = j.value("code", p.code);
                p.source = j.value("source", p.source);
                if (j.contains("text")) { p.textFrom = j.value("text", ""); p.fTextFromExplicit = true; }
                p.voiceFrom = j.value("voice", p.voiceFrom);
                if (j.contains("script")) p.fWide = j.value("script", "") == "japanese";
                else if (p.lang == LANG_Japanese || p.lang == LANG_Korean || p.lang == LANG_Chinese) p.fWide = true;
            }
            else if (p.lang == LANG_Japanese || p.lang == LANG_Korean || p.lang == LANG_Chinese) p.fWide = true;
            std::error_code ec;
            if (fs::is_regular_file(dir / "Text.json", ec)) p.fText = true;
            // the dialog lines: any file named by its line id under Dialog/ (later folders win)
            const bool fMod = !g_disc.DirMods().empty() && cane::utf8(dir.parent_path().parent_path()) == cane::utf8(cane::path(g_disc.DirMods()));
            if (fMod) p.adirMod.push_back(dir);
            if (fs::is_directory(dir / "Dialog", ec))
                for (auto it = fs::recursive_directory_iterator(dir / "Dialog", fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    int id = 0;
                    if (ec) break;
                    if (it->is_regular_file(ec) && FAudioExt(it->path()) && FParseLine(cane::utf8(it->path().stem()), id))
                    {
                        p.mpline[id] = g_disc.IsectorExternal(cane::utf8(it->path()));
                        if (fMod) p.mplineMod[id] = p.mpline[id];
                    }
                }
            const json jl = JsonRead(dir / "Dialog" / "lengths.json");
            if (jl.is_object())
                for (const auto& [k, v] : jl.items())
                    if (v.is_number()) p.mplength[std::atoi(k.c_str())] = v.get<float>();
            if (fs::is_directory(dir / "Movies", ec))
                for (const auto& e : fs::directory_iterator(dir / "Movies", fs::directory_options::skip_permission_denied, ec))
                    if (e.is_regular_file(ec) && FAudioExt(e.path()))
                    {
                        p.mpmovie[cane::utf8(e.path().stem())] = cane::utf8(e.path());
                        if (fMod) p.mpmovieMod[cane::utf8(e.path().stem())] = cane::utf8(e.path());
                    }
        }
        s_apack.push_back(std::move(p));
    }
    // offered: not the disc's own, not taken from this disc's region, with text of its own (or a dub that names its
    // text); the game's languages in their order, then the custom ones by name
    const std::string family = RegionFamily(g_disc.SzDisc());
    for (const LANGPACK& p : s_apack)
    {
        if (FLanguageOnDisc(p.lang)) continue;
        if (!p.source.empty() && RegionFamily(p.source) == family) continue;
        if (!p.fText && !(p.fTextFromExplicit && !p.mpline.empty())) continue;
        s_alangOffered.push_back(p.lang);
    }
    // (a variant beside its language: "English (UK)" after English; other custom ones last, by name)
    auto order = [](int lang) {
        if (lang < LANG_Max) return lang * 1000;
        const LANGPACK* p = PpackOfLang(lang);
        int base = LangOfGameName(p->key.substr(0, p->key.find(" (")));
        if (base < 0) base = LangOfGameName(p->voiceFrom);
        return (base >= 0 ? base : 100) * 1000 + 1 + (lang - LANG_Max);
    };
    std::sort(s_alangOffered.begin(), s_alangOffered.end(), [&](int a, int b) { return order(a) < order(b); });
    if (!s_apack.empty())
    {
        std::printf("languages: %zu in %s", s_apack.size(), g_disc.DirLanguages().empty() ? "(none)" : g_disc.DirLanguages().c_str());
        for (int lang : s_alangOffered) std::printf(" +%s", PpackOfLang(lang)->key.c_str());
        std::printf("\n");
    }
}

const std::vector<int>& AlangStoreOffered() { return s_alangOffered; }

const char* SzLangStoreKey(int lang)
{
    const LANGPACK* p = PpackOfLang(lang);
    return p ? p->key.c_str() : nullptr;
}

int LangStoreOfKey(const char* key)
{
    const LANGPACK* p = key ? PpackOfKey(key) : nullptr;
    return p ? p->lang : -1;
}

std::string StrLangStoreDisplayName(int lang)
{
    const LANGPACK* p = PpackOfLang(lang);
    if (!p || lang < LANG_Max) return "";
    return StrToDisc(p->name, p->fWide);
}

int LangStoreTextFallback(int lang)
{
    LANGPACK* p = PpackOfLang(lang);
    if (!p || p->fText) return -1;
    for (int i = 0; p && i < 8; ++i)
    {
        if (FLanguageOnDisc(p->lang)) return p->lang;
        if (p->fText) return -1;
        p = PpackOfKey(p->textFrom.empty() ? std::string("English") : p->textFrom);
    }
    return -1;
}

namespace
{
// g_nLang's store text pack when its text can be shown on this disc (nullptr: the disc's text)
LANGPACK* PpackTextShown()
{
    if (FLanguageOnDisc(g_nLang)) return nullptr;
    LANGPACK* p = PpackText(PpackOfLang(g_nLang));
    if (!p) return nullptr;
    // (Japanese text: the Japanese disc's fonts have the glyphs; the other discs need the language's Font.bin)
    if (p->fWide && !FWideText() && !PfontOfPack(*p)) return nullptr;
    return p;
}
}  // namespace

bool FLangStoreHasVoices(int lang)
{
    if (FLanguageOnDisc(lang)) return false;
    const LANGPACK* p = PpackOfLang(lang);
    return p && !p->mpline.empty();
}

bool FLangStoreVoicesOnly(int lang)
{
    if (FWideText() || FLanguageOnDisc(lang)) return false;
    LANGPACK* p = PpackText(PpackOfLang(lang));
    return p && p->fWide && !PfontOfPack(*p);
}

void LatchLanguageStoreText()
{
    const SELEM* pset = nullptr;
    if (!FWideText() && !g_plangcapture)
        if (LANGPACK* p = PpackTextShown(); p && p->fWide) pset = PfontOfPack(*p);
    s_psetFontShown = pset;
    SetStoreWideChars(pset != nullptr);
}

const SELEM* PsetLanguageStoreFont() { return FWideText() ? nullptr : s_psetFontShown.load(); }

bool FStoreTextShown() { return PpackTextShown() != nullptr; }
int LangStoreTextLanguage()
{
    const LANGPACK* p = PpackTextShown();
    return p ? p->lang : LangWorld();
}
bool FStoreTextWide()
{
    const LANGPACK* p = PpackTextShown();
    return p && p->fWide;
}

void ApplyLanguageStoreText(std::vector<std::pair<int, std::string>>& atext, bool fDiscMods)
{
    if (s_apack.empty() || fDiscMods != FLanguageOnDisc(g_nLang)) return;
    std::lock_guard<std::mutex> lock(s_mtx);
    if (fDiscMods)
    {
        // (a disc language: only the mods folder's Mods/Languages/<key>/Text.json over the disc's own)
        LANGPACK* pm = PpackOfLang(g_nLang);
        if (!pm || pm->adirMod.empty()) return;
        if (!pm->fTextModLoaded)
        {
            pm->fTextModLoaded = true;
            for (const fs::path& dir : pm->adirMod)
            {
                const json j = JsonRead(dir / "Text.json");
                if (!j.is_object() || !j.contains("strings")) continue;
                for (const auto& [k, v] : j.at("strings").items())
                {
                    char* end = nullptr;
                    const long id = std::strtol(k.c_str(), &end, 0);
                    if (!k.empty() && !*end && v.is_string()) pm->mptextMod[int(id)] = v.get<std::string>();
                }
            }
        }
        const bool fWide = FWideText() && g_nLang == LANG_Japanese;
        for (auto& [id, str] : atext)
            if (const auto it = pm->mptextMod.find(IdTextUsa(id)); IdTextUsa(id) >= 0 && it != pm->mptextMod.end()) str = StrToDisc(it->second, fWide);
        return;
    }
    LANGPACK* p = PpackTextShown();
    if (!p) return;
    LoadPackText(*p);
    const bool fWide = p->fWide;
    // (Japanese text on a USA / European disc: the strings the language lacks are the disc's Latin-1, shown with the
    // Japanese font as Latin text is on the Japanese disc)
    const bool fFoldRest = FWideChars() && !FWideText();
    // (the menus' button lines of a language from a disc of the other button convention)
    const std::string familyPack = RegionFamily(p->source);
    const bool fJapanDisc = g_disc.Region() == DISCREGION::Japan;
    const bool fButtons = !familyPack.empty() && (familyPack == "Japan") != fJapanDisc &&
                          (familyPack == "Japan" || familyPack == "USA" || familyPack == "Europe");
    for (auto& [id, str] : atext)
    {
        const int idUsa = IdTextUsa(id);
        const auto it = idUsa < 0 ? p->mptext.end() : p->mptext.find(idUsa);
        if (it != p->mptext.end())
        {
            str = StrToDisc(it->second, fWide);
            if (fButtons) MenuButtonGlyphs(idUsa, str, fJapanDisc);
        }
        else if (fFoldRest) str = StrFoldLatin(Utf8OfLatin1Bytes(str));
    }
}

bool FLanguageStoreDialog(int idLineDisc, FK& fk, float& tLength)
{
    if (s_apack.empty()) return false;
    const int idUsa = IdTextUsa(idLineDisc);
    if (idUsa < 0) return false;
    LANGPACK* p = PpackOfLang(LangVoice());   // (the voices' language: region.h g_nLangVoice)
    for (int i = 0; p && i < 8; ++i)
    {
        // (the disc's own lines; only a mod's Mods/Languages/<key>/Dialog line over one)
        const bool fDisc = FLanguageOnDisc(p->lang);
        const auto& mp = fDisc ? p->mplineMod : p->mpline;
        if (const auto it = mp.find(idUsa); it != mp.end())
        {
            fk = FK{};
            fk.isector = it->second;
            fk.id = uint32_t(idUsa);
            const std::string path = g_disc.PathOf(fk.isector);
            std::error_code ec;
            fk.cb = uint32_t(std::min<uint64_t>(fs::file_size(cane::path(path), ec), 0x7FFFFFFF));
            if (ec || fk.cb == 0) return false;
            const auto itl = p->mplength.find(idUsa);
            tLength = itl != p->mplength.end() ? itl->second : TAudioFile(path);
            return tLength > 0.0f;
        }
        if (fDisc) return false;
        p = PpackOfKey(p->voiceFrom.empty() ? std::string("English") : p->voiceFrom);
    }
    return false;
}

std::string PathLanguageStoreMovieAudio(uint32_t isector)
{
    if (s_apack.empty()) return "";
    std::string name = g_disc.SzFileName(isector);
    if (const auto sp = name.find(' '); sp != std::string::npos) name = name.substr(sp + 1);
    if (name.empty()) return "";
    LANGPACK* p = PpackOfLang(LangVoice());
    for (int i = 0; p && i < 8; ++i)
    {
        if (FLanguageOnDisc(p->lang))
        {
            const auto it = p->mpmovieMod.find(name);   // (a mod's sound over the disc's own)
            return it != p->mpmovieMod.end() ? it->second : "";
        }
        if (const auto it = p->mpmovie.find(name); it != p->mpmovie.end()) return it->second;
        p = PpackOfKey(p->voiceFrom.empty() ? std::string("English") : p->voiceFrom);
    }
    return "";
}

int ListLanguagesCommand()
{
    for (int i = 0; i < CLanguages(); ++i)
    {
        const int lang = LanguageOfIndex(i);
        const LANGPACK* p = PpackOfLang(lang);
        json j = {{"id", lang}, {"key", lang < LANG_Max ? s_aszGameName[lang] : p ? p->key : "?"},
                  {"name", p ? p->name : lang < LANG_Max ? s_aszNative[lang] : "?"}, {"disc", FLanguageOnDisc(lang)}};
        if (p)
        {
            j["text"] = p->fText;
            j["voice"] = !p->mpline.empty();
            j["movies"] = !p->mpmovie.empty();
            // (Japanese script: its text shows on this disc; false: voices only, the Font.bin is missing)
            if (p->fWide) j["text_shown"] = !FLangStoreVoicesOnly(lang);
            if (!p->source.empty()) j["source"] = p->source;
        }
        // what it can be chosen for here: the text (Settings.ini Language), the voices (Voices)
        j["as_text"] = FLanguageTextAvailable(lang);
        j["as_voices"] = FLanguageVoiceAvailable(lang);
        std::printf("%s\n", j.dump().c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// --import-languages

namespace
{
struct CAPTURE : LANGCAPTURE
{
    // lang -> USA id -> string (the disc's encoding)
    std::map<int, std::map<int, std::string>> mptext;
    // lang -> disc line id -> seconds
    std::map<int, std::map<int, float>> mplength;
    void Text(int lang, int id, const std::string& str) override
    {
        const int idUsa = IdTextUsa(id);
        if (idUsa >= 0) mptext[lang].emplace(idUsa, str);
    }
    void Dialog(int lang, int id, const FK&, float tLength) override { mplength[lang].emplace(id, tLength); }
    // the Japanese disc's text font: the worlds' font sets with the kana (not the button icons' set), every glyph once
    std::map<uint32_t, std::pair<float, SELEM>> mpglyph;
    int cglyphOther = 0;   // glyphs that are not vector sprites (none on the disc)
    void Font(const SELEM& set) override
    {
        if (std::find(set.aul.begin(), set.aul.end(), 0x3042u) == set.aul.end()) return;   // (no "あ": not the text font)
        for (std::size_t i = 0; i < set.aul.size() && i < set.ag.size() && i + 1 < set.asub.size(); ++i)
        {
            if (mpglyph.count(set.aul[i])) continue;
            const SELEM& g = set.asub[i + 1];
            if (g.kind != SELEMK_Sprite && g.kind != 0) { ++cglyphOther; continue; }
            SELEM copy;
            copy.kind = SELEMK_Sprite;
            copy.apoly = g.apoly;
            mpglyph.emplace(set.aul[i], std::make_pair(set.ag[i], std::move(copy)));
        }
    }
};

bool FWriteBytes(const fs::path& p, const std::vector<uint8_t>& ab)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.parent_path() / (cane::utf8(p.filename()) + ".partial");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

bool FWriteJson(const fs::path& p, const json& j)
{
    const std::string s = j.dump(1) + "\n";
    return FWriteBytes(p, std::vector<uint8_t>(s.begin(), s.end()));
}

// a file that exists in the folder under any audio extension
fs::path PathExistingAudio(const fs::path& base)
{
    std::error_code ec;
    for (const char* ext : {".flac", ".wav", ".vag"})
    {
        fs::path p = base;
        p += ext;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

// move src to dst (another file system: copy, then delete)
bool FMoveFile(const fs::path& src, const fs::path& dst)
{
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(src, ec);
    return true;
}

bool FCopyFile(const fs::path& src, const fs::path& dst)
{
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// p is inside root
bool FUnder(const fs::path& p, const fs::path& root)
{
    const std::string rel = cane::utf8(p.lexically_normal().lexically_relative(root.lexically_normal()).generic_u8string());
    return !rel.empty() && rel.rfind("..", 0) != 0;
}

std::string RelOf(const fs::path& p, const fs::path& root) { return cane::utf8(fs::relative(p, root).generic_u8string()); }
}  // namespace

int ImportLanguagesCommand(const LANGIMPORT& o)
{
    const fs::path out = cane::path(o.dirOut);
    const fs::path install = o.dirInstall.empty() ? fs::path() : cane::path(o.dirInstall);
    const bool fMove = o.fMove && !install.empty();
    std::error_code ec;
    fs::create_directories(out, ec);
    const std::string disc = g_disc.SzDisc();
    const std::string family = RegionFamily(disc);
    const bool fFlac = true;   // (the store's lines from a disc image: FLAC, as the European installations')
    // the source's languages and their folders: English from another disc than the USA one is "English (UK)" for its
    // text (the European discs' British spelling and fixes: notes/LANG_2026-11-10.md); its lines and movies' sound
    // are the USA's (the same samples), kept in "English"
    struct DEST { int lang; std::string key, keyText; };
    std::vector<DEST> adest;
    for (int i = 0; i < CLanguagesDisc(); ++i)
    {
        const int lang = LanguageOfDiscIndex(i);
        DEST d{lang, s_aszGameName[lang], s_aszGameName[lang]};
        if (lang == LANG_English && family != "USA") d.keyText = family == "Europe" ? "English (UK)" : "English (" + family + ")";
        if (!o.alangOnly.empty() && std::find(o.alangOnly.begin(), o.alangOnly.end(), d.key) == o.alangOnly.end() &&
            std::find(o.alangOnly.begin(), o.alangOnly.end(), d.keyText) == o.alangOnly.end())
            continue;
        adest.push_back(d);
    }
    if (adest.empty()) { std::fprintf(stderr, "import-languages: nothing to import from %s\n", disc.c_str()); return 1; }
    // the crawl: every world loads (the text and dialog tables of every language captured on the way)
    CAPTURE cap;
    g_plangcapture = &cap;
    std::vector<CONVLANGFILE> afile;
    std::string error;
    const int langSave = g_nLang;
    g_nLang = LanguageOfDiscIndex(0);   // (the crawl reads the disc's own data)
    std::printf("%s 0 1 worlds\n", o.szProgress);
    std::fflush(stdout);
    const bool fCrawl = FConvertLanguageFiles(afile, error);
    g_plangcapture = nullptr;
    g_nLang = langSave;
    if (!fCrawl) { std::fprintf(stderr, "import-languages: %s\n", error.c_str()); return 1; }
    // (a converted installation: its movies from its manifest, the crawl knows PSS files only)
    if (g_disc.FConverted())
    {
        afile.erase(std::remove_if(afile.begin(), afile.end(), [](const CONVLANGFILE& f) { return f.kind == "movie"; }), afile.end());
        const json m = JsonRead(cane::path(g_disc.Path()) / "extraction_manifest.json");
        if (m.is_object() && m.contains("files"))
            for (const auto& f : m.at("files"))
            {
                if (f.value("kind", "") != "movie") continue;
                CONVLANGFILE c;
                c.kind = "movie";
                c.isector = f.value("sector", 0u);
                c.cb = f.value("disc_size", 0u);
                const std::string name = f.value("name", "");
                const auto sp = name.find(' ');
                c.name = sp == std::string::npos ? std::string() : name.substr(sp + 1);
                afile.push_back(std::move(c));
            }
    }
    // the world records' names (a line one world lists: that world's folder, as the converter does)
    auto worldFolder = [](const CONVLANGFILE& f) -> std::string {
        if (f.aiwrec.size() != 1) return "Shared";
        const char* sz = SzConvertWorldName(f.aiwrec[0]);
        std::string s = sz ? sz : "World " + std::to_string(f.aiwrec[0]);
        for (char& c : s)
            if (std::strchr("<>:\"/\\|?*", c)) c = '_';
        return s;
    };
    // the manifest of the installation the files move from: sector -> its entry
    json manifest;
    std::map<uint32_t, json*> mpsector;
    if (fMove)
    {
        manifest = JsonRead(install / "extraction_manifest.json");
        if (!manifest.is_object() || !manifest.contains("files")) { std::fprintf(stderr, "import-languages: %s has no manifest\n", o.dirInstall.c_str()); return 1; }
        for (auto& f : manifest["files"]) mpsector[f.value("sector", 0u)] = &f;
    }
    std::set<std::string> asetMoved;   // installation paths now in the store (installation.json drops them)
    size_t cwork = 0, idone = 0;
    for (const CONVLANGFILE& f : afile) cwork += f.kind == "dialog" ? 1 : 0;
    cwork += afile.size() - cwork;
    int cwritten = 0, ckept = 0;
    for (const DEST& d : adest)
    {
        const fs::path dirLang = out / cane::path(d.key);
        const fs::path dirText = out / cane::path(d.keyText);
        // Text.json (USA ids, UTF-8)
        if (const auto it = cap.mptext.find(d.lang); it != cap.mptext.end())
        {
            if (o.fReplace || !fs::is_regular_file(dirText / "Text.json", ec))
            {
                json strings = json::object();
                for (const auto& [id, s] : it->second) strings[std::to_string(id)] = Utf8OfGameText(s);
                const json j = {{"language", d.keyText}, {"source", disc}, {"strings", strings}};
                if (!FWriteJson(dirText / "Text.json", j)) { std::fprintf(stderr, "import-languages: cannot write %s\n", cane::utf8(dirText).c_str()); return 1; }
            }
            // language.json: its name, its script, where it came from (a text variant: its voice is the language's)
            json lj = JsonRead(dirText / "language.json");
            if (!lj.is_object() || o.fReplace)
            {
                lj = json::object();
                lj["name"] = d.keyText == d.key ? s_aszNative[d.lang] : d.keyText;
                lj["code"] = s_aszIso[d.lang];
                lj["script"] = d.lang == LANG_Japanese || d.lang == LANG_Korean || d.lang == LANG_Chinese ? "japanese" : "latin";
                lj["source"] = disc;
                if (d.keyText != d.key) lj["voice"] = d.key;
                FWriteJson(dirText / "language.json", lj);
            }
        }
        // the Japanese disc's text font (the USA / European discs show the language's text with it)
        if (d.lang == LANG_Japanese && FWideText() && !cap.mpglyph.empty() && (o.fReplace || !fs::is_regular_file(dirText / "Font.bin", ec)))
        {
            if (!FWriteBytes(dirText / "Font.bin", AbFontFile(cap.mpglyph))) { std::fprintf(stderr, "import-languages: cannot write %s\n", cane::utf8(dirText / "Font.bin").c_str()); return 1; }
            std::printf("import-languages: the Japanese font: %zu characters%s\n", cap.mpglyph.size(), cap.cglyphOther ? " (some glyphs skipped)" : "");
            ++cwritten;
        }
        // the dialog lines (by USA id) and their lengths
        json lengths = JsonRead(dirLang / "Dialog" / "lengths.json");
        if (!lengths.is_object()) lengths = json::object();
        for (const CONVLANGFILE& f : afile)
        {
            if (f.kind != "dialog" || f.lang != d.lang) continue;
            const int idUsa = IdTextUsa(f.id);
            if (idUsa < 0) continue;
            if (++idone % 50 == 0) { std::printf("%s %zu %zu %s\n", o.szProgress, idone, cwork, d.key.c_str()); std::fflush(stdout); }
            const fs::path base = dirLang / "Dialog" / cane::path(worldFolder(f)) / std::to_string(idUsa);
            fs::path have = o.fReplace ? fs::path() : PathExistingAudio(base);
            const std::string pathInstalled = g_disc.FConverted() ? g_disc.PathInstalled(f.isector) : std::string();
            if (have.empty())
            {
                if (!pathInstalled.empty())
                {
                    // a converted installation's line: its file as it is (WAV / FLAC / VAG), moved when it is the
                    // installation's own
                    fs::path dst = base;
                    dst += cane::path(pathInstalled).extension();
                    const bool fOwn = fMove && FUnder(cane::path(pathInstalled), install);
                    if (!(fOwn ? FMoveFile(cane::path(pathInstalled), dst) : FCopyFile(cane::path(pathInstalled), dst)))
                    {
                        std::fprintf(stderr, "import-languages: cannot write %s\n", cane::utf8(dst).c_str());
                        return 1;
                    }
                    have = dst;
                }
                else
                {
                    std::vector<uint8_t> ab(f.cb);
                    if (!g_disc.FReadFileBytes(f.isector, 0, ab.data(), ab.size())) { std::fprintf(stderr, "import-languages: cannot read sector 0x%X\n", f.isector); return 1; }
                    std::size_t cframe = 0;
                    const std::vector<uint8_t> abOut = AbConvertDialogVag(ab, fFlac, &cframe, error);
                    if (abOut.empty()) { std::fprintf(stderr, "import-languages: line %d: %s\n", f.id, error.c_str()); return 1; }
                    have = base;
                    have += fFlac ? ".flac" : ".wav";
                    if (!FWriteBytes(have, abOut)) { std::fprintf(stderr, "import-languages: cannot write %s\n", cane::utf8(have).c_str()); return 1; }
                }
                ++cwritten;
            }
            else
            {
                ++ckept;
                // the store has it (the same line from another region's installation): this installation's copy goes
                if (fMove && !pathInstalled.empty() && FUnder(cane::path(pathInstalled), install) && cane::path(pathInstalled) != have)
                    fs::remove(cane::path(pathInstalled), ec);
            }
            if (fMove)
                if (const auto it = mpsector.find(f.isector); it != mpsector.end())
                {
                    asetMoved.insert((*it->second)["path"].get<std::string>());
                    (*it->second)["path"] = "@languages/" + RelOf(have, out);
                }
            if (const auto itl = cap.mplength.find(d.lang); itl != cap.mplength.end())
                if (const auto itf = itl->second.find(f.id); itf != itl->second.end())
                    lengths[std::to_string(idUsa)] = std::round(double(itf->second) * 1000.0) / 1000.0;
        }
        if (!lengths.empty()) FWriteJson(dirLang / "Dialog" / "lengths.json", lengths);
        // the movies' sound in this language, by the movie's name (not the logos, credits and extras: no speech, or
        // other edits on other discs)
        for (const CONVLANGFILE& f : afile)
        {
            if (f.kind != "movie" || f.name.empty()) continue;
            if (f.name.rfind("Logo", 0) == 0 || f.name.rfind("Extra", 0) == 0 || f.name == "Credits") continue;
            const int track = CLanguagesDisc() > 1 ? d.lang + 1 : 0;
            const fs::path base = dirLang / "Movies" / cane::path(f.name);
            fs::path have = o.fReplace ? fs::path() : PathExistingAudio(base);
            const std::string part = g_disc.FConverted() ? g_disc.PathPart(f.isector, "audio" + std::to_string(track)) : std::string();
            // (the installation's part: its own file, not a mod's)
            std::string partInstalled;
            json* pent = nullptr;
            json* ppart = nullptr;
            if (fMove)
                if (const auto it = mpsector.find(f.isector); it != mpsector.end() && it->second->contains("audio"))
                {
                    pent = it->second;
                    for (auto& a : (*pent)["audio"])
                        if (a.value("track", -1) == track) { ppart = &a; partInstalled = cane::utf8(install / cane::path(a.value("path", ""))); }
                }
            if (have.empty())
            {
                if (!part.empty())
                {
                    fs::path dst = base;
                    dst += cane::path(part).extension();
                    if (ppart && !partInstalled.empty() && fs::is_regular_file(cane::path(partInstalled), ec))
                    {
                        if (!FMoveFile(cane::path(partInstalled), dst)) { std::fprintf(stderr, "import-languages: cannot move %s\n", partInstalled.c_str()); return 1; }
                    }
                    else if (!FCopyFile(cane::path(part), dst))
                    {
                        std::fprintf(stderr, "import-languages: cannot write %s\n", cane::utf8(dst).c_str());
                        return 1;
                    }
                    have = dst;
                }
                else if (!g_disc.FConverted())
                {
                    std::vector<uint8_t> abVideo;
                    std::map<int, MOVIEAUDIOTRACK> mpaudio;
                    FK fk;
                    fk.isector = f.isector;
                    fk.cb = f.cb;
                    if (!FDemuxMovie(fk, abVideo, mpaudio)) continue;
                    const auto it = mpaudio.find(track);
                    if (it == mpaudio.end()) continue;
                    const std::vector<uint8_t> abOut = AbFlac(it->second.rate, 2, it->second.asample.data(), it->second.asample.size() / 2, &error);
                    if (abOut.empty()) continue;
                    have = base;
                    have += ".flac";
                    if (!FWriteBytes(have, abOut)) { std::fprintf(stderr, "import-languages: cannot write %s\n", cane::utf8(have).c_str()); return 1; }
                }
                else continue;
                ++cwritten;
            }
            else if (ppart && !partInstalled.empty() && cane::path(partInstalled) != have)
                fs::remove(cane::path(partInstalled), ec);
            if (ppart)
            {
                asetMoved.insert(ppart->value("path", ""));
                (*ppart)["path"] = "@languages/" + RelOf(have, out);
            }
            if (++idone % 10 == 0) { std::printf("%s %zu %zu %s\n", o.szProgress, idone, cwork, d.key.c_str()); std::fflush(stdout); }
        }
    }
    // the installation's manifests: the moved files' new paths (extraction_manifest.json), and the inventory without
    // them (installation.json: "Verify files" checks the installation's own files)
    if (fMove)
    {
        if (!FWriteJson(install / "extraction_manifest.json", manifest)) { std::fprintf(stderr, "import-languages: cannot write the manifest\n"); return 1; }
        json inv = JsonRead(install / "installation.json");
        if (inv.is_object() && inv.contains("files") && inv["files"].is_array())
        {
            json files = json::array();
            for (const auto& f : inv["files"])
                if (!asetMoved.count(f.value("path", ""))) files.push_back(f);
            inv["files"] = files;
            inv["languages_dir"] = cane::utf8(out);
            FWriteJson(install / "installation.json", inv);
        }
        // the emptied language folders
        for (const char* sub : {"Dialog", "Movies"})
        {
            const fs::path root = install / sub;
            if (!fs::is_directory(root, ec)) continue;
            std::vector<fs::path> adir;
            for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_directory(ec)) adir.push_back(it->path());
            std::sort(adir.rbegin(), adir.rend());
            for (const fs::path& p : adir) fs::remove(p, ec);   // (only empty ones go)
            fs::remove(root, ec);
        }
    }
    std::printf("%s %zu %zu done\n", o.szProgress, cwork, cwork);
    std::printf("import-languages: %s: %zu languages, %d files written, %d already there%s\n", disc.c_str(), adest.size(),
                cwritten, ckept, fMove ? " (the installation's own moved into the folder)" : "");
    return 0;
}

// (ps3lang.cpp) every language's text of the open disc by USA id (the disc's encoding): the crawl's capture
bool FConvertLanguageTexts(std::map<int, std::map<int, std::string>>& mptext, std::string& error)
{
    CAPTURE cap;
    g_plangcapture = &cap;
    std::vector<CONVLANGFILE> afile;
    const int langSave = g_nLang;
    g_nLang = LanguageOfDiscIndex(0);
    const bool f = FConvertLanguageFiles(afile, error);
    g_plangcapture = nullptr;
    g_nLang = langSave;
    mptext = std::move(cap.mptext);
    return f;
}

// ---------------------------------------------------------------------------------------------------------
// --language-selftest (no disc): the Font.bin format, the menus' button lines, the Japanese ids, 16-bit text on a
// USA / European disc (decoding, line breaks, the button words)

int LanguageSelfTest()
{
    int cfail = 0, ccheck = 0;
    auto check = [&](bool f, const char* what) {
        ++ccheck;
        if (!f) { ++cfail; std::printf("FAIL %s\n", what); }
    };
    // a font of four full-width characters and 'A'
    std::map<uint32_t, std::pair<float, SELEM>> mpglyph;
    const uint32_t ach[] = {0x41, 0x3002, 0x3042, 0x3044, 0x3046, 0x65E5};
    for (uint32_t ch : ach)
    {
        SELEM g;
        g.kind = SELEMK_Sprite;
        SPOLY poly;
        poly.primitive = 2;
        poly.control = 3;
        poly.fVertexBytes = 1;
        poly.rgba = 0x80402010u + ch;
        poly.axy = {{int16_t(ch & 0x7FF), -16}, {160, int16_t(-int(ch & 0xFF))}, {0, 800}};
        poly.ab = {1, 2, 0x80};
        g.apoly.assign(ch == 0x41 ? 2 : 1, poly);
        mpglyph.emplace(ch, std::make_pair(ch < 0x80 ? 25.6f : 51.2f, std::move(g)));
    }
    const std::vector<uint8_t> ab = AbFontFile(mpglyph);
    const std::shared_ptr<SELEM> pset = PfontFromBytes(ab);
    check(pset != nullptr, "the font file reads back");
    if (pset)
    {
        check(pset->aul.size() == std::size(ach) && pset->ag.size() == pset->aul.size() && pset->asub.size() == pset->aul.size() + 1, "the font's glyph count");
        check(std::is_sorted(pset->aul.begin(), pset->aul.end()), "the font's characters ascend");
        FONTBRX font;
        font.pset = pset.get();
        const SELEM* pa = font.PglyphFromCh(0x41);
        const SELEM* pnichi = font.PglyphFromCh(0x65E5);
        check(pa && pa->apoly.size() == 2 && pnichi && pnichi->apoly.size() == 1, "the glyphs by character");
        check(pnichi && pnichi->apoly[0].rgba == 0x80402010u + 0x65E5 && pnichi->apoly[0].axy == mpglyph.at(0x65E5).second.apoly[0].axy &&
                  pnichi->apoly[0].ab == mpglyph.at(0x65E5).second.apoly[0].ab && pnichi->apoly[0].primitive == 2 && pnichi->apoly[0].control == 3,
              "a glyph's polygon");
        check(font.DxFromCh(0x41) == 25.6f && font.DxFromCh(0x3042) == 51.2f && font.DxFromCh(0x3043) == 0.0f && !font.PglyphFromCh(0x3043), "the widths");
        // 16-bit text on a USA / European disc: decoding, the Japanese line breaks, the button words
        SetStoreWideChars(true);
        std::size_t cb = 0;
        check(FWideChars() && ChDecode("\xE3\x81\x82", 0, &cb) == 0x3042 && cb == 3, "a 16-bit character decodes");
        check(!FLatinLineBreaks(), "Japanese line breaks");
        // (three characters fit: the full stop does not start a line, it takes the character before it along)
        check(StrWrapped(font, nullptr, "あいう。あ", 1.0f, 160.0f) == "あい\nう。あ", "no line starts with a full stop");
        check(StrWrapped(font, nullptr, "あいうあい", 1.0f, 160.0f) == "あいう\nあい", "a line breaks between characters");
        check(StrPromptButtonWords("スタートボタン:新しいゲームを始める", -1) == "&2!&.ボタン:新しいゲームを始める", "the START button's icon");
        check(StrPromptButtonWords("セレクトボタン:メニュー", -1) == "&2_&.ボタン:メニュー", "the SELECT button's icon");
        check(StrPromptButtonWords("エピソードセレクトに戻る", -1) == "エピソードセレクトに戻る", "\"select\" inside a word stays");
        check(StrPromptButtonWords("見つけるにはL3ボタンを押し", -1) == "見つけるには&2{&.ボタンを押し", "L3 next to kana");
        SetStoreWideChars(false);
        check(!FWideChars() && ChDecode("\xE3\x81\x82", 0, &cb) == 0xE3 && cb == 1 && FLatinLineBreaks(), "Latin-1 text again");
        check(StrPromptButtonWords("スタートボタン", -1) == "スタートボタン", "the words stay without the Japanese font");
    }
    {
        std::vector<uint8_t> abBad = ab;
        abBad.resize(abBad.size() - 3);
        check(!PfontFromBytes(abBad), "a cut font file is refused");
        abBad = ab;
        abBad[3] = 'X';
        check(!PfontFromBytes(abBad), "another file is refused");
        check(!PfontFromBytes({}), "an empty file is refused");
    }
    // the menus' button lines between the two conventions
    {
        std::string s = "&2O&.ボタン:決定\n&2+&.:移動\n&2X&.ボタン:キャンセル";
        MenuButtonGlyphs(0x2D, s, false);
        check(s == "&2X&.ボタン:決定\n&2+&.:移動\n&2T&.ボタン:キャンセル", "Circle / Cross lines as Cross / Triangle");
        MenuButtonGlyphs(0x2D, s, true);
        check(s == "&2O&.ボタン:決定\n&2+&.:移動\n&2X&.ボタン:キャンセル", "and back");
        s = "&2O&.ボタン:上書きする\n&2S&.ボタン:データを消す";
        MenuButtonGlyphs(0x28, s, false);
        check(s == "&2X&.ボタン:上書きする\n&2S&.ボタン:データを消す", "the overwrite page keeps Square");
        s = "&2S&.:データをけす";
        MenuButtonGlyphs(0x25, s, false);
        check(s == "&2O&.:データをけす", "erase with Circle");
        s = "OK && X: &2O&. X";
        MenuButtonGlyphs(0x29F, s, false);
        check(s == "OK && X: &2X&. X", "only the icons change");
        s = "&2O&.ボタンでつかまり、&2X&.ボタンで投げてもらおう。";
        const std::string sGame = s;
        MenuButtonGlyphs(0x34C, s, false);
        check(s == sGame, "the game's own buttons stay");
    }
    check(IdTextJapanOfUsa(0x11) == 0x11 && IdTextJapanOfUsa(0x12) == 0x14 && IdTextJapanOfUsa(0x19) == 0x1C && IdTextJapanOfUsa(0x6D) == 0x71 &&
              IdTextJapanOfUsa(0x135) == 0x282 && IdTextJapanOfUsa(0x136) == 0x139 && IdTextJapanOfUsa(0x27F) == 0x283,
          "the Japanese text ids");
    check(StrFoldLatin(Utf8OfLatin1Bytes("Caf\xE9 \xA9 \x93ok\x94")) == "Cafe (C) \"ok\"", "Latin-1 folded for the Japanese font");
    // the text's and the voices' languages apart: Settings.ini [Language] (an old file's Language is both)
    {
        namespace st = cane::settings;
        const st::Sly2Settings old = st::sly2FromIni("[Language]\nLanguage = Japanese\n");
        check(old.game.language == "Japanese" && old.game.voiceLanguage.empty(), "an old file's Language: text and voices");
        const st::Sly2Settings mixed = st::sly2FromIni("[Language]\nLanguage = English\nVoices = Japanese\n");
        check(mixed.game.language == "English" && mixed.game.voiceLanguage == "Japanese", "Language and Voices apart");
        const st::Sly2Settings same = st::sly2FromIni("[Language]\nLanguage = Auto\nVoices = same as text\n");
        check(same.game.language.empty() && same.game.voiceLanguage.empty(), "Auto, Same As Text");
        const std::string ini = st::sly2ToIni(mixed);
        check(ini.find("Voices = Japanese") != std::string::npos && st::sly2FromIni(ini).game.voiceLanguage == "Japanese", "Voices written back");
        check(st::sly2ToIni(old).find("Voices = Same As Text") != std::string::npos, "Same As Text written");
    }
    // the title's language menu sets both (and drops a choice waiting for the next world)
    {
        const int langSave = g_nLang, chosenSave = g_nLangChosen, voiceSave = g_nLangVoice;
        g_nLangVoice = LANG_Japanese;
        SetPendingLanguages(LANG_German, LANG_French);
        int lt = 0, lv = 0;
        check(FLanguagesPending(&lt, &lv) && lt == LANG_German && lv == LANG_French, "a choice waits for the next world");
        ChooseLanguageOnTitle(LANG_Italian);
        check(g_nLang == LANG_Italian && g_nLangChosen == LANG_Italian && g_nLangVoice == -1 && !FLanguagesPending(), "the title's choice sets both");
        SetPendingLanguages(-2, -1);
        g_nLangVoice = LANG_Japanese;
        check(FLanguagesPending(&lt, &lv) && lt == -2 && lv == -1, "voices back to the text's, waiting");
        g_nLang = langSave;
        g_nLangChosen = chosenSave;
        g_nLangVoice = voiceSave;
        SetPendingLanguages(-2, -2);
    }
    std::printf("language selftest: %d checks, %d failed\n", ccheck, cfail);
    return cfail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------
// --check-language KEY (with a disc / an installation and the languages folder): the language chosen as a player
// would, then every world's text as the game shows it. Fails when the text stays the disc's (the language's own is
// not shown), when its strings are not the language's, or when a character has no glyph in the font it is drawn
// with (a square or nothing on screen). Japanese on a USA / European installation: the Font.bin's glyphs.

int CheckLanguageCommand(const std::string& key)
{
    int lang = LangStoreOfKey(key.c_str());
    if (lang < 0) lang = LangOfGameName(key);
    if (lang < 0 || !FLanguageAvailable(lang)) { std::printf("check-language: FAIL: %s is not offered on %s\n", key.c_str(), g_disc.SzDisc()); return 1; }
    if (FLanguageOnDisc(lang)) { std::printf("check-language: %s is one of the disc's own languages: nothing to check\n", key.c_str()); return 0; }
    SetLanguage(lang);
    LANGPACK* p = PpackOfLang(lang);
    if (FLangStoreVoicesOnly(lang) || !FStoreTextShown() || !p)
    {
        std::printf("check-language: FAIL: %s on %s: its text is not shown (voices only: the disc's text stays%s)\n", key.c_str(), g_disc.SzDisc(),
                    FLangStoreVoicesOnly(lang) ? "; the language's Font.bin is missing" : "");
        return 1;
    }
    LANGPACK* ptext = PpackText(p);
    LoadPackText(*ptext);
    InitClasses();
    std::vector<std::pair<std::string, FK>> aworld;
    std::vector<int> afilter;
    {
        std::set<uint32_t> aset;
        auto add = [&](const std::string& w) {
            FK fk;
            int filter = 0;
            std::string err;
            if (!FResolveWorld(w, fk, filter, err) || !fk.cb || !aset.insert(fk.isector).second) return;
            aworld.emplace_back(w, fk);
            afilter.push_back(filter);
        };
        add("boot");
        add("frontend");
        for (int i = 0; i < 64; ++i) add(std::to_string(i));
    }
    // the fonts: the text's (Japanese: the folder's Font.bin; Latin: Latin-1, the disc's 0x20..0xFF)
    int cstr = 0, cown = 0, cnoglyph = 0, cworld = 0, cfolded = 0;
    std::set<uint32_t> asetMissing;
    auto checkGlyphs = [&](const std::string& str, const SELEM* psetFont, const char* what, int id) {
        if (!psetFont) return;
        bool fIcons = false;
        for (std::size_t i = 0; i < str.size();)
        {
            if (str[i] == '&' && i + 1 < str.size() && str[i + 1] != '&') { fIcons = str[i + 1] != '.'; i += 2; continue; }
            if (str[i] == '&') ++i;
            std::size_t cb = 1;
            const uint32_t ch = ChDecode(str, i, &cb);
            i += cb;
            if (fIcons || ch == '\n' || ch == '\r' || ch == '\t') continue;
            if (!std::binary_search(psetFont->aul.begin(), psetFont->aul.end(), ch))
            {
                if (asetMissing.insert(ch).second && asetMissing.size() <= 12)
                {
                    std::string sz;
                    AppendCh16(sz, ch);
                    std::printf("check-language: no glyph for U+%04X %s (%s 0x%X)\n", ch, sz.c_str(), what, id);
                }
                ++cnoglyph;
            }
        }
    };
    for (std::size_t iw = 0; iw < aworld.size(); ++iw)
    {
        auto psw = std::make_unique<SW>();
        psw->filter = afilter[iw];
        std::size_t cbRead = 0, cbFile = 0;
        if (!FLoadWorldFile(psw.get(), aworld[iw].second, "", &cbRead, &cbFile)) { std::printf("check-language: FAIL: world %s did not load\n", aworld[iw].first.c_str()); return 1; }
        ++cworld;
        const SELEM* psetFont = PsetLanguageStoreFont();
        if (ptext->fWide && !FWideText() && (!FWideChars() || !psetFont))
        {
            std::printf("check-language: FAIL: world %s: the Japanese font is not in use\n", aworld[iw].first.c_str());
            return 1;
        }
        for (const auto& [id, str] : psw->atext)
        {
            const int idUsa = IdTextUsa(id);
            if (idUsa < 5) continue;   // (0..4: the builds' unused debug strings)
            ++cstr;
            const auto it = idUsa < 0 ? ptext->mptext.end() : ptext->mptext.find(idUsa);
            if (it == ptext->mptext.end()) { ++cfolded; continue; }
            // (the language's own string: as stored, but for the menus' button icons)
            std::string want = StrToDisc(it->second, ptext->fWide);
            std::string got = str;
            for (std::string* ps : {&want, &got})
                for (std::size_t i = 0; i + 2 < ps->size(); ++i)
                    if ((*ps)[i] == '&' && (*ps)[i + 1] == '2') (*ps)[i + 2] = '?';
            if (want == got) ++cown;
            else if (cstr - cown - cfolded <= 5) std::printf("check-language: text 0x%X of world %s is not the language's: %s\n", id, aworld[iw].first.c_str(), str.c_str());
            checkGlyphs(str, psetFont, "text", id);
        }
    }
    // the port's own text (porttext.h) in that language, with the same font
    int cport = 0;
    {
        const SELEM* psetFont = PsetLanguageStoreFont();
        for (int i = 0; i < PTX_Max; ++i)
        {
            ++cport;
            checkGlyphs(StrPort(PTX(i)), psetFont, "the port's text", i);
        }
    }
    const int cother = cstr - cown - cfolded;
    std::printf("check-language: %s on %s: %d worlds, %d strings: %d the language's, %d the disc's (not in the language), %d other; "
                "%d port strings; %d characters without a glyph (%zu different)\n",
                key.c_str(), g_disc.SzDisc(), cworld, cstr, cown, cfolded, cother, cport, cnoglyph, asetMissing.size());
    const bool fOk = cworld > 0 && cstr > 0 && cother == 0 && cown * 100 >= cstr * 95 && cnoglyph == 0;
    std::printf("check-language: %s\n", fOk ? "PASS" : "FAIL");
    return fOk ? 0 : 1;
}
