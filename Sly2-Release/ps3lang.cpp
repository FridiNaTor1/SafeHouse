// The Sly Collection's (PS3) languages into the shared languages folder (langstore.h; lang 2026-11-10).
//
// --import-ps3-languages SOURCE OUT (with --disc / --data-dir on a Sly 2 installation that has English: USA or Europe).
// SOURCE as for --ps3-hd-textures (platform/ps3disc.h: the extracted disc, its .iso or a .zip of it). The PS3 build
// (BCUS-98246, En / Fr / Es) keeps every world's text in three languages (wide, little-endian UTF-16), numbered its
// own way. Its English is the USA text, so each PS3 id is matched to the USA id whose English string is the same
// (a string several ids share: by the id offset of the matched ids around it), and French and Spanish are written
// as the store's French / Spanish Text.json (the same translations as the European discs: 585 / 584 of 590
// compared strings identical) for the languages the folder does not have yet.
//
// Not imported: the PS3 dialog lines and movie sound. The lines are 5073 VAGs (1691 x 3) without names; the archive's
// table that names them is not readable (notes/PS3.md) and their order follows no world order found (the lines'
// lengths in the PS3 worlds are the English ones for all three languages, so they do not tell the languages
// apart). The French / Spanish imported from the PS3 are subtitles over the English voices
// (language.json "voice": "English"); a European disc imported later adds the voices.

#include "langstore.h"

#include "region.h"
#include "textmod.h"

#include "../platform/platform.h"
#include "../platform/ps3disc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

std::vector<uint8_t> Ps3DecompressWorld(const std::vector<uint8_t>& in);   // ps3hd.cpp
char Ps3KindOfSector(const uint8_t* p);
bool FConvertLanguageTexts(std::map<int, std::map<int, std::string>>& mptext, std::string& error);   // below

namespace
{
constexpr size_t SECTOR = 2048;

uint16_t U16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

std::string Utf8OfUtf16(const uint8_t* p, size_t cch)
{
    std::string s;
    for (size_t i = 0; i < cch; ++i) AppendCh16(s, U16(p + 2 * i));
    return s;
}

// a world's text block: U8 3, the languages 0 1 4 (English, French, Spanish), S16 count, {S16 id, 3 x {U16 n, n x
// U16}}; found by its start and checked by parsing it whole
bool FFindTextBlock(const std::vector<uint8_t>& ab, std::map<int, std::array<std::string, 3>>& out)
{
    static const uint8_t s_ab[4] = {3, 0, 1, 4};
    for (size_t i = 0; i + 6 < ab.size(); ++i)
    {
        if (std::memcmp(ab.data() + i, s_ab, 4)) continue;
        const int c = int16_t(U16(ab.data() + i + 4));
        if (c < 50 || c > 6000) continue;
        size_t q = i + 6;
        std::map<int, std::array<std::string, 3>> m;
        bool fOk = true;
        for (int k = 0; k < c && fOk; ++k)
        {
            if (q + 2 > ab.size()) { fOk = false; break; }
            const int id = int16_t(U16(ab.data() + q));
            q += 2;
            std::array<std::string, 3> row;
            for (int j = 0; j < 3; ++j)
            {
                if (q + 2 > ab.size()) { fOk = false; break; }
                const size_t n = U16(ab.data() + q);
                q += 2;
                if (q + 2 * n > ab.size() || n > 4000) { fOk = false; break; }
                row[size_t(j)] = Utf8OfUtf16(ab.data() + q, n);
                q += 2 * n;
            }
            if (id < 0) fOk = false;
            m[id] = std::move(row);
        }
        if (!fOk) continue;
        for (auto& [id, row] : m) out.emplace(id, std::move(row));
        return true;
    }
    return false;
}

// the same sentence whatever the typography (the PS3's ’ … “ ” against the PS2's Windows-1252 bytes)
std::string StrNorm(const std::string& utf8)
{
    std::string s = StrFoldLatinForWideFont(utf8);
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '\n') out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

int ImportPs3LanguagesCommand(const std::string& source, const std::string& dirOut, bool fReplace)
{
    auto fail = [](const std::string& message) {
        std::printf("ps3lang: error: %s\n", message.c_str());
        std::fflush(stdout);
        return 2;
    };
    // the installation's English, by USA id
    std::map<int, std::map<int, std::string>> mptextDisc;
    std::string err;
    std::printf("progress 0 3 worlds\n");
    std::fflush(stdout);
    if (!FConvertLanguageTexts(mptextDisc, err)) return fail(err);
    if (!mptextDisc.count(LANG_English)) return fail("This Sly 2 installation has no English text: use the USA or a European installation.");
    std::map<std::string, std::vector<int>> mpnorm;
    for (const auto& [id, s] : mptextDisc[LANG_English]) mpnorm[StrNorm(Utf8OfGameText(s))].push_back(id);

    // the PS3 worlds' texts
    cane::ps3::Disc disc;
    fs::path src = cane::path(source);
    std::error_code ec;
    if (fs::is_regular_file(src, ec) && cane::utf8(src.filename()) == "SLY2.WAL") src = src.parent_path();
    if (!disc.open(src, err) || !disc.checkSlyCollection(2, err)) return fail(err);
    std::map<int, std::array<std::string, 3>> mpps3;
    int cworld = 0;
    std::vector<uint8_t> world, carry;
    char kindCur = 0;
    auto finish = [&] {
        if (kindCur == 'w' && !world.empty())
        {
            const std::vector<uint8_t> ab = Ps3DecompressWorld(world);
            if (FFindTextBlock(ab, mpps3)) ++cworld;
        }
        world.clear();
    };
    auto sector = [&](const uint8_t* p) {
        if (const char k = Ps3KindOfSector(p))
        {
            finish();
            kindCur = k;
        }
        if (kindCur == 'w') world.insert(world.end(), p, p + SECTOR);
    };
    const bool ok = disc.read(
        {"SLY2/SLY2.WAL"},
        [&](size_t, uint64_t, const uint8_t* p, size_t n) {
            size_t i = 0;
            if (!carry.empty())
            {
                const size_t take = std::min(n, SECTOR - carry.size());
                carry.insert(carry.end(), p, p + take);
                i = take;
                if (carry.size() == SECTOR) sector(carry.data()), carry.clear();
            }
            for (; i + SECTOR <= n; i += SECTOR) sector(p + i);
            if (i < n) carry.assign(p + i, p + n);
            return true;
        },
        [&](uint64_t done, uint64_t total) {
            std::printf("progress %llu %llu read\n", (unsigned long long)(total + done), (unsigned long long)(3 * total));
            std::fflush(stdout);
            return true;
        },
        err);
    if (!ok) return fail(err);
    finish();
    if (mpps3.empty()) return fail("No Sly 2 text was found on this disc.");

    // PS3 id -> USA id: a unique English match; else the offset of the nearest matched ids, when the English agrees
    std::map<int, int> mpid;
    for (const auto& [id, row] : mpps3)
        if (const auto it = mpnorm.find(StrNorm(row[0])); it != mpnorm.end() && it->second.size() == 1) mpid[id] = it->second[0];
    for (const auto& [id, row] : mpps3)
    {
        if (mpid.count(id)) continue;
        const auto it = mpnorm.find(StrNorm(row[0]));
        if (it == mpnorm.end()) continue;
        auto hi = mpid.lower_bound(id);
        for (auto pit : {hi, hi == mpid.begin() ? mpid.end() : std::prev(hi)})
        {
            if (pit == mpid.end()) continue;
            const int idUsa = id + (pit->second - pit->first);
            if (std::find(it->second.begin(), it->second.end(), idUsa) != it->second.end()) { mpid[id] = idUsa; break; }
        }
    }
    std::printf("ps3lang: %d worlds, %zu strings, %zu matched to the USA text\n", cworld, mpps3.size(), mpid.size());

    // French and Spanish (subtitles: the English voices)
    const fs::path out = cane::path(dirOut);
    int cwritten = 0;
    const struct { int col; int lang; const char* key; const char* name; const char* code; } s_adest[] = {
        {1, LANG_French, "French", "Français", "fr"}, {2, LANG_Spanish, "Spanish", "Español", "es"}};
    for (const auto& d : s_adest)
    {
        const fs::path dir = out / d.key;
        if (!fReplace && fs::is_regular_file(dir / "Text.json", ec))
        {
            std::printf("ps3lang: %s: the languages folder has it already\n", d.key);
            continue;
        }
        json strings = json::object();
        for (const auto& [id, idUsa] : mpid) strings[std::to_string(idUsa)] = mpps3[id][size_t(d.col)];
        const json j = {{"language", d.key}, {"source", "The Sly Collection (PS3)"}, {"strings", strings}};
        fs::create_directories(dir, ec);
        {
            std::ofstream o(dir / "Text.json", std::ios::trunc);
            o << j.dump(1) << '\n';
            if (!o) return fail("Cannot write " + cane::utf8(dir / "Text.json"));
        }
        json lj = json::object();
        if (std::ifstream in(dir / "language.json"); in) try { in >> lj; } catch (...) { lj = json::object(); }
        if (!lj.is_object()) lj = json::object();
        lj["name"] = d.name;
        lj["code"] = d.code;
        lj["script"] = "latin";
        if (!lj.contains("source")) lj["source"] = "The Sly Collection (PS3)";
        std::error_code ecd;
        if (!fs::is_directory(dir / "Dialog", ecd)) lj["voice"] = "English";   // (no lines of its own: the English voices)
        std::ofstream(dir / "language.json", std::ios::trunc) << lj.dump(1) << '\n';
        ++cwritten;
        std::printf("ps3lang: %s: %zu strings\n", d.key, mpid.size());
    }
    std::printf("progress 3 3 done\n");
    std::printf("ps3lang: %d languages written\n", cwritten);
    return 0;
}
