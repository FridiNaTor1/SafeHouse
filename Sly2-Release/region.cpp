// See region.h.
#include "region.h"

#include "disc.h"
#include "langstore.h"
#include "textmod.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

int g_nLang = LANG_English;
int g_nLangChosen = -1;

namespace
{
uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// orig 0x1A2C48: the region's {count, list} (the executable's table 0x2BFBD0, read at the disc's own addresses:
// the European build's table has another entry, "america3")
bool FRegionLanguages(int& c, uint32_t& vaList)
{
    if (!g_disc.FIsOpen()) return false;
    const uint32_t va = g_disc.VaRegion(0x2BFBD0) + 8u * uint32_t(g_disc.IregionLanguages());
    c = int(Le32(g_disc.PbElfOwn(va, 4)));
    vaList = Le32(g_disc.PbElfOwn(va + 4, 4));
    return c > 0 && c <= LANG_Max;
}

const char* const s_aszLang[LANG_Max] = {"english", "french", "italian", "german", "spanish", "dutch", "portuguese",
                                        "danish", "finnish", "norwegian", "swedish", "japanese", "korean", "chinese"};

// The PC's language (the system locale: LANG / LC_ALL / LC_MESSAGES on Linux, the user locale on Windows) as the
// console's would be: its ISO 639 code.
std::string SzSystemLanguage()
{
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"})
        if (const char* sz = std::getenv(var); sz && *sz && std::strcmp(sz, "C") != 0 && std::strcmp(sz, "POSIX") != 0)
            return std::string(sz, std::strcspn(sz, "_.@:-"));
#ifdef _WIN32
    // (setlocale(LC_ALL, "") only to read the user's locale name: the process stays in the "C" locale, or every
    // later printf / strtod / sscanf of a number uses the user's decimal comma, e.g. Swedish or German Windows)
    const std::string old = std::setlocale(LC_ALL, nullptr) ? std::setlocale(LC_ALL, nullptr) : "C";
    const char* sz = std::setlocale(LC_ALL, "");
    const std::string name = sz ? std::string(sz, std::strcspn(sz, "_.@:-")) : std::string();
    std::setlocale(LC_ALL, old.c_str());
    if (!name.empty()) return name;
#endif
    return "";
}

int LanguageOfIso(const std::string& code)
{
    static const struct { const char* iso; int lang; } s_a[] = {
        {"en", LANG_English}, {"fr", LANG_French}, {"it", LANG_Italian}, {"de", LANG_German}, {"es", LANG_Spanish},
        {"nl", LANG_Dutch}, {"pt", LANG_Portuguese}, {"da", LANG_Danish}, {"fi", LANG_Finnish}, {"no", LANG_Norwegian},
        {"nb", LANG_Norwegian}, {"nn", LANG_Norwegian}, {"sv", LANG_Swedish}, {"ja", LANG_Japanese},
        {"ko", LANG_Korean}, {"zh", LANG_Chinese},
        // (Windows locale names)
        {"English", LANG_English}, {"French", LANG_French}, {"Italian", LANG_Italian}, {"German", LANG_German},
        {"Spanish", LANG_Spanish}, {"Dutch", LANG_Dutch}, {"Portuguese", LANG_Portuguese}, {"Danish", LANG_Danish},
        {"Finnish", LANG_Finnish}, {"Norwegian", LANG_Norwegian}, {"Swedish", LANG_Swedish}, {"Japanese", LANG_Japanese}};
    for (const auto& e : s_a)
        if (code == e.iso) return e.lang;
    return -1;
}
}

int CLanguagesDisc()
{
    int c = 0;
    uint32_t va = 0;
    return FRegionLanguages(c, va) ? c : 1;
}

int LanguageOfDiscIndex(int i)
{
    int c = 0;
    uint32_t va = 0;
    if (!FRegionLanguages(c, va) || i < 0 || i >= c) return LANG_English;
    return int(Le32(g_disc.PbElfOwn(va + 4u * uint32_t(i), 4)));
}

bool FLanguageOnDisc(int lang)
{
    for (int i = 0, c = CLanguagesDisc(); i < c; ++i)
        if (LanguageOfDiscIndex(i) == lang) return true;
    return false;
}

int CLanguages() { return CLanguagesDisc() + int(AlangStoreOffered().size()); }

int LanguageOfIndex(int i)
{
    const int cDisc = CLanguagesDisc();
    if (i < cDisc) return LanguageOfDiscIndex(i);
    const std::vector<int>& a = AlangStoreOffered();
    return i - cDisc < int(a.size()) ? a[size_t(i - cDisc)] : LANG_English;
}

bool FLanguageAvailable(int lang)
{
    if (FLanguageOnDisc(lang)) return true;
    for (int l : AlangStoreOffered())
        if (l == lang) return true;
    return false;
}

int LangWorld()
{
    if (FLanguageOnDisc(g_nLang)) return g_nLang;
    // (a store language's text taken from one of the disc's own: a dub with the disc's subtitles)
    const int langText = LangStoreTextFallback(g_nLang);
    if (langText >= 0 && FLanguageOnDisc(langText)) return langText;
    return FLanguageOnDisc(LANG_English) ? LANG_English : LanguageOfDiscIndex(0);
}

int LangText() { return FLanguageOnDisc(g_nLang) ? g_nLang : FStoreTextShown() ? LangStoreTextLanguage() : LangWorld(); }

bool FLatinLineBreaks() { return !FWideText() || (FStoreTextShown() && !FStoreTextWide()); }

const char* SzLanguage(int lang) { return lang >= 0 && lang < LANG_Max ? s_aszLang[lang] : SzLangStoreKey(lang); }

// orig 0x1A2BD0
void SetLanguage(int lang) { g_nLang = lang == -1 ? LanguageOfDiscIndex(0) : lang; }

void InitLanguage()
{
    g_nLangChosen = -1;
    // orig 0x1A2DA0(sceScfGetLanguage()): the console's language when the disc has it, else the region's first.
    int lang = LanguageOfIso(SzSystemLanguage());
    // (testing: SLY2_LANGUAGE=<name or id> picks a language the way the console's setting would)
    const char* sz = std::getenv("SLY2_LANGUAGE");
    if (sz && *sz)
    {
        lang = -1;
        for (int i = 0; i < LANG_Max; ++i)
            if (std::strcmp(sz, s_aszLang[i]) == 0) lang = i;
        if (lang < 0 && sz[0] >= '0' && sz[0] <= '9') lang = std::atoi(sz);
    }
    if (lang < 0 && sz && *sz) lang = LangStoreOfKey(sz);   // (a store language by its folder name)
    SetLanguage(lang >= 0 && FLanguageAvailable(lang) ? lang : -1);
    if (CLanguages() > 1)
        std::printf("language: %s (%d on the disc, %d in all)\n", SzLanguage(g_nLang), CLanguagesDisc(), CLanguages());
}

void ApplyLanguageSetting(const std::string& key)
{
    if (key.empty()) return;
    int lang = LangStoreOfKey(key.c_str());
    for (int i = 0; i < LANG_Max && lang < 0; ++i)
        if (key == SzLanguageModName(i)) lang = i;
    if (lang < 0 || !FLanguageAvailable(lang))
    {
        std::printf("language: %s (Settings.ini) is not installed: ignored\n", key.c_str());
        return;
    }
    SetLanguage(lang);
    g_nLangChosen = lang;
    std::printf("language: %s (Settings.ini)\n", key.c_str());
}

bool g_fPs3World = false;

bool FLaterWorldBlockOrder() { return g_fPs3World || g_disc.Region() != DISCREGION::Usa; }

bool FWideText() { return g_fPs3World || g_disc.Region() == DISCREGION::Japan; }

namespace
{
// USA id ranges -> the Japanese id (each runs to the next one's start)
struct TEXTIDRANGE { int idUsa, did; };
constexpr TEXTIDRANGE s_aidrJapan[] = {
    {0x000, 0},   // the title's lines; (0..4 are unused debug strings)
    {0x012, 2},   // the memory card messages (the Japanese build has two more before them: 0x12, 0x13)
    {0x019, 3},   // the menus
    {0x06D, 4},   // ("。" 0x70 added) the episode names, the gadgets, ...
    {0x135, 0x282 - 0x135},   // "WE'LL BE RIGHT BACK!" (the Japanese build's 0x282)
    {0x136, 3},   // the jobs' names and goals
    {0x27F, 4},   // (0x282 added) the rest
};
// The European builds' text has one string more: "You have accessed the Ratchet & Clank 3 demo!" (0x6D, the disc's demo),
// so the ids from the episode names on are the USA ids + 1. v2.01 also moved "WE'LL BE RIGHT BACK!" (USA 0x135) to
// 0x27F, the strings between in place. (Found by aligning all worlds' texts: USA against each European executable.)
constexpr TEXTIDRANGE s_aidrEurope100[] = {
    {0x000, 0},
    {0x06D, 1},   // (the demo's string added) everything after
};
constexpr TEXTIDRANGE s_aidrEurope201[] = {
    {0x000, 0},
    {0x06D, 1},   // (the demo's string added) the episode names, the gadgets, ...
    {0x135, 0x27F - 0x135},   // "WE'LL BE RIGHT BACK!!" (v2.01's 0x27F)
    {0x136, 0},   // the jobs' names and goals
    {0x27F, 1},   // (0x27F added) the rest
};

// the disc's text id ranges (none: the USA ids)
const TEXTIDRANGE* PidrDisc(size_t& c)
{
    c = 0;
    switch (g_disc.Region())
    {
    case DISCREGION::Japan: c = std::size(s_aidrJapan); return s_aidrJapan;
    case DISCREGION::Europe:
        if (std::strncmp(g_disc.SzDisc(), "Europe v1.00", 12) == 0) { c = std::size(s_aidrEurope100); return s_aidrEurope100; }
        c = std::size(s_aidrEurope201);
        return s_aidrEurope201;
    default: return nullptr;
    }
}
}

uint32_t BtnMenuConfirm() { return g_disc.Region() == DISCREGION::Japan ? 0x20 : 0x40; }
uint32_t BtnMenuBack() { return g_disc.Region() == DISCREGION::Japan ? 0x40 : 0x10; }
uint32_t BtnMenuErase() { return g_disc.Region() == DISCREGION::Japan ? 0x80 : 0x20; }
const char* SzMenuBackGlyph() { return g_disc.Region() == DISCREGION::Japan ? "&2X&." : "&2T&."; }
const char* SzMenuConfirmGlyph() { return g_disc.Region() == DISCREGION::Japan ? "&2O&." : "&2X&."; }

int IdText(int idUsa)
{
    size_t c = 0;
    const TEXTIDRANGE* aidr = PidrDisc(c);
    if (!aidr || idUsa < 0) return idUsa;
    int did = 0;
    for (size_t i = 0; i < c; ++i)
        if (idUsa >= aidr[i].idUsa) did = aidr[i].did;
    return idUsa + did;
}

int IdTextUsa(int id)
{
    size_t c = 0;
    const TEXTIDRANGE* aidr = PidrDisc(c);
    if (!aidr || id < 0) return id;
    for (int i = int(c) - 1; i >= 0; --i)
    {
        const TEXTIDRANGE& r = aidr[i];
        const int idUsa = id - r.did;
        const int idUsaEnd = i + 1 < int(c) ? aidr[i + 1].idUsa : 0x7FFFFFFF;
        if (idUsa >= r.idUsa && idUsa < idUsaEnd) return idUsa;
    }
    return -1;
}

void AppendCh16(std::string& str, uint32_t ch)
{
    if (ch < 0x80) str += char(ch);
    else if (ch < 0x800)
    {
        str += char(0xC0 | (ch >> 6));
        str += char(0x80 | (ch & 0x3F));
    }
    else
    {
        str += char(0xE0 | (ch >> 12));
        str += char(0x80 | ((ch >> 6) & 0x3F));
        str += char(0x80 | (ch & 0x3F));
    }
}

uint32_t ChDecode(const std::string& str, std::size_t ib, std::size_t* pcb)
{
    const auto b = [&](std::size_t i) { return i < str.size() ? uint32_t(uint8_t(str[i])) : 0u; };
    uint32_t ch = b(ib);
    std::size_t cb = 1;
    if (FWideText() && ch >= 0xC0)
    {
        if (ch < 0xE0 && (b(ib + 1) & 0xC0) == 0x80) { ch = ((ch & 0x1F) << 6) | (b(ib + 1) & 0x3F); cb = 2; }
        else if (ch >= 0xE0 && (b(ib + 1) & 0xC0) == 0x80 && (b(ib + 2) & 0xC0) == 0x80)
        {
            ch = ((ch & 0x0F) << 12) | ((b(ib + 1) & 0x3F) << 6) | (b(ib + 2) & 0x3F);
            cb = 3;
        }
    }
    if (pcb) *pcb = cb;
    return ch;
}

bool FTitleNeedsLanguage(bool fTitleInputTaken, bool fContinueSlot)
{
    return !fTitleInputTaken && CLanguages() >= 2 && !fContinueSlot && g_nLangChosen == -1;
}

std::string StrLanguageHelpLabel(float t)
{
    // orig (Europe) 0x1DE1F8's end: the labels of the disc's languages that have one, from the current language on,
    // each 1.75 s
    const uint32_t vaLabels = g_disc.VaLanguageLabels();
    const int c = CLanguages();
    if (!vaLabels || c < 2) return "";
    int icur = 0;
    for (int i = 0; i < c; ++i)
        if (LanguageOfIndex(i) == g_nLang) icur = i;
    int alang[LANG_Max];
    int n = 0;
    for (int i = 0; i < c; ++i)
    {
        const int lang = LanguageOfIndex((i + icur) % c);
        if (lang >= 0 && lang < LANG_Max && Le32(g_disc.PbElfOwn(vaLabels + 4u * uint32_t(lang), 4))) alang[n++] = lang;
    }
    if (n == 0) return "";
    const int k = int(std::max(0.0f, t) / 1.75f) % n;
    const uint32_t va = Le32(g_disc.PbElfOwn(vaLabels + 4u * uint32_t(alang[k]), 4));
    const char* p = reinterpret_cast<const char*>(g_disc.PbElfOwn(va, 1));
    return std::string(p, strnlen(p, 64));
}

// orig JP 0x220840 (the Japanese build's line break test) with its tables JP 0x2B25E8 / 0x2B2638
bool FJapaneseNoBreakBefore(uint32_t ch)
{
    static constexpr char16_t s_ach[] = u"、。」・ー！），．？／］｝!),.?/]ぁぃぅぇぉっゃゅょァィゥェォッャュョ";
    for (char16_t c : s_ach)
        if (c && c == ch) return true;
    return false;
}

bool FJapaneseNoBreakAfter(uint32_t ch)
{
    static constexpr char16_t s_ach[] = u"「（／［｛(/[";
    for (char16_t c : s_ach)
        if (c && c == ch) return true;
    return false;
}
