#pragma once
// The Localisation tool's model (launcher/editor/loc_page.h; desktop only, beside the Map Editor). A Sly 2 language of
// the shared languages folder (sly2-languages/<Name>/: Text.json, language.json; Sly2-Release/langstore.h) or of the
// mods folder (Mods/Languages/<Name>/), compared string by string with a reference language: the game's strings by USA
// text id ("strings", "worlds" per level) and the port's own PC strings by name ("port": Sly2-Release/porttext.h).
// No ImGui here: the page draws it, `safehouse --loc-selftest` checks the rules below.
//
// Where a language is saved: a language of its own (Faroese, "English (UK)") in its folder, the whole Text.json; one of
// the game's 14 (English .. Chinese) in the mods folder's Mods/Languages/<Name>/Text.json, only what differs from the
// languages folder's (the game applies that file over the disc's own text too, and over the folder's).
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cane::loc {
namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;

inline fs::path pathOf(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
inline std::string utf8Of(const fs::path& p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

// ---- UTF-8
inline std::u32string decode(const std::string& s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        char32_t c = b;
        int n = 0;
        if (b >= 0xF0) c = b & 0x07, n = 3;
        else if (b >= 0xE0) c = b & 0x0F, n = 2;
        else if (b >= 0xC0) c = b & 0x1F, n = 1;
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        out += c;
    }
    return out;
}
inline std::string encode(char32_t c) {
    std::string out;
    if (c < 0x80) out += char(c);
    else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    else out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    return out;
}

// ---- The game's languages (Sly2-Release/langstore.cpp: the folder names, codes; the port's 13 built-in columns)
inline const std::vector<std::string>& gameLanguages() {
    static const std::vector<std::string> a = {"English", "French",  "Italian",   "German",  "Spanish",  "Dutch",  "Portuguese",
                                               "Danish",  "Finnish", "Norwegian", "Swedish", "Japanese", "Korean", "Chinese"};
    return a;
}
inline const std::vector<std::string>& gameCodes() {
    static const std::vector<std::string> a = {"en", "fr", "it", "de", "es", "nl", "pt", "da", "fi", "no", "sv", "ja", "ko", "zh"};
    return a;
}
inline int gameLanguage(const std::string& key) {
    const auto& a = gameLanguages();
    const auto it = std::find(a.begin(), a.end(), key);
    return it == a.end() ? -1 : int(it - a.begin());
}

// ---- The fonts. The USA / European discs draw Latin-1 (their fonts' 224 glyphs, 0x20..0xFF); these slots hold the
// fonts' placeholder box instead of a letter (measured on the USA and European v1.00 / v2.01 fonts: the same set):
// no-break space, ¦ § ª « ¬ soft hyphen ¯ ± ² ³ ´ µ ¶ · ¸ ¹ º » ¼ ½ ¾ Þ þ (ð is drawn by the port since 2026-11-13).
// Typographic quotes and the ellipsis are written plainly (textmod.h); everything else beyond Latin-1 shows as '?'.
inline bool latinShowable(char32_t c) {
    if (c == '\n') return true;
    if (c < 0x20) return false;
    if (c == 0x2018 || c == 0x2019 || c == 0x201C || c == 0x201D || c == 0x2026) return true;
    if (c >= 0x7F && c < 0xA0) return false;
    if (c > 0xFF) return false;
    static const std::set<char32_t> box = {0xA0, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAF, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5,
                                           0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xDE, 0xFE};
    return !box.count(c);
}
// A Japanese or Korean text's font (Japanese/Font.bin, Korean/Font.bin, Sly2-Release/langstore.cpp "S2FONT1"): its
// characters (empty: none).
inline std::set<char32_t> fontCharacters(const fs::path& file) {
    std::set<char32_t> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    const std::vector<unsigned char> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const unsigned char magic[8] = {'S', '2', 'F', 'O', 'N', 'T', '1', 0};
    if (ab.size() < 12 || !std::equal(magic, magic + 8, ab.begin())) return out;
    std::size_t i = 8;
    auto u16 = [&]() -> uint32_t { if (i + 2 > ab.size()) { i = ab.size() + 1; return 0; } const uint32_t v = ab[i] | (ab[i + 1] << 8); i += 2; return v; };
    auto u32 = [&]() -> uint32_t { const uint32_t lo = u16(); return lo | (u16() << 16); };
    const uint32_t count = u32();
    for (uint32_t g = 0; g < count && i <= ab.size(); ++g) {
        const uint32_t ch = u16();
        u32();
        const uint32_t polys = u16();
        for (uint32_t p = 0; p < polys && i <= ab.size(); ++p) {
            i += 8;
            const uint32_t verts = u16();
            i += std::size_t(verts) * 5;
        }
        if (i <= ab.size()) out.insert(ch);
    }
    if (i != ab.size()) out.clear();   // (not that file)
    return out;
}

// ---- Checks of a translation against its reference
enum class Issue { Codes, Formats, Lines, Spaces, Length, Glyphs, Empty };
struct Problem {
    Issue issue;
    bool error;          // the game shows it wrong (else: worth a look)
    std::string detail;  // the codes / characters concerned
};
// The game's control codes: "&2X" (a button icon: "&2" and the icon's letter), "&." (back to text), "&<digit><c>"
// others alike; a lone "&" before anything else is an ampersand.
inline std::vector<std::string> controlCodes(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] != '&') continue;
        if (s[i + 1] == '.') out.push_back("&."), ++i;
        else if (s[i + 1] >= '0' && s[i + 1] <= '9' && i + 2 < s.size()) {
            // (the icon's character may be any byte, UTF-8 too)
            std::size_t n = 1;
            const auto b = static_cast<unsigned char>(s[i + 2]);
            if (b >= 0xF0) n = 4; else if (b >= 0xE0) n = 3; else if (b >= 0xC0) n = 2;
            out.push_back(s.substr(i, 2 + n));
            i += 1 + n;
        }
    }
    return out;
}
// printf conversions in order ("%s", "%d", "%.2f"); "%%" is none
inline std::vector<std::string> formats(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        if (i + 1 < s.size() && s[i + 1] == '%') { ++i; continue; }
        std::size_t j = i + 1;
        while (j < s.size() && std::string("-+ #0123456789.*hlzjtL").find(s[j]) != std::string::npos) ++j;
        if (j < s.size() && std::string("diouxXeEfFgGaAcsp").find(s[j]) != std::string::npos) {
            out.push_back(s.substr(i, j - i + 1));
            i = j;
        }
    }
    return out;
}
inline int lineCount(const std::string& s) { return int(std::count(s.begin(), s.end(), '\n')) + 1; }
// the longest line in characters, the control codes not counted (an icon counts as two)
inline std::size_t longestLine(const std::string& s) {
    std::size_t best = 0, cur = 0;
    const std::u32string u = decode(s);
    for (std::size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '\n') { best = std::max(best, cur); cur = 0; continue; }
        if (u[i] == '&' && i + 1 < u.size() && u[i + 1] == '.') { ++i; continue; }
        if (u[i] == '&' && i + 2 < u.size() && u[i + 1] >= '0' && u[i + 1] <= '9') { i += 2; cur += 2; continue; }
        ++cur;
    }
    return std::max(best, cur);
}
struct Font {
    bool japanese = false;                            // a 16-bit font: the Japanese or the Korean disc's (Font.bin)
    const std::set<char32_t>* characters = nullptr;   // (japanese) Font.bin's; null: not known
};
// The Korean disc's fonts hold the conjoining jamo, not the syllables: a Hangul syllable is drawn from its 2 or 3 jamo
// (Sly2-Release/region.h CHangulJamo), so the font can draw it when it has them all.
inline bool hangulShowable(char32_t c, const std::set<char32_t>& characters) {
    if (c < 0xAC00 || c > 0xD7A3) return false;
    const char32_t s = c - 0xAC00;
    if (!characters.count(0x1100 + s / 588) || !characters.count(0x1161 + (s % 588) / 28)) return false;
    return s % 28 == 0 || characters.count(0x11A7 + s % 28) > 0;
}
// The characters the font cannot draw ("" when all can).
inline std::string unshowable(const std::string& s, const Font& font) {
    std::string out;
    std::set<char32_t> seen;
    const std::u32string u = decode(s);
    for (std::size_t i = 0; i < u.size(); ++i) {
        const char32_t c = u[i];
        if (c == '&' && i + 2 < u.size() && u[i + 1] >= '0' && u[i + 1] <= '9') { i += 2; continue; }   // (an icon)
        bool ok;
        if (font.japanese) ok = c == '\n' || (font.characters ? font.characters->count(c) > 0 || hangulShowable(c, *font.characters) : true);
        else ok = latinShowable(c);
        if (!ok && seen.insert(c).second) out += encode(c);
    }
    return out;
}
inline std::vector<Problem> check(const std::string& ref, const std::string& text, const Font& font) {
    std::vector<Problem> out;
    if (text.empty()) return out;
    auto codesOf = [](const std::string& s) {
        auto a = controlCodes(s);
        std::sort(a.begin(), a.end());
        return a;
    };
    auto join = [](const std::vector<std::string>& a) {
        std::string s;
        for (const auto& x : a) s += (s.empty() ? "" : " ") + x;
        return s;
    };
    if (!ref.empty()) {
        // (the same icons: the order may change with the sentence)
        if (codesOf(ref) != codesOf(text)) out.push_back({Issue::Codes, true, join(controlCodes(ref))});
        // (the same printf codes in the same order: the game fills them in that order)
        if (formats(ref) != formats(text)) out.push_back({Issue::Formats, true, join(formats(ref))});
        if (lineCount(ref) != lineCount(text)) out.push_back({Issue::Lines, false, std::to_string(lineCount(ref))});
        auto lead = [](const std::string& s) { return !s.empty() && (s.front() == ' ' || s.front() == '\n'); };
        auto trail = [](const std::string& s) { return !s.empty() && (s.back() == ' ' || s.back() == '\n'); };
        if (lead(ref) != lead(text) || trail(ref) != trail(text)) out.push_back({Issue::Spaces, false, {}});
        const std::size_t r = longestLine(ref), t = longestLine(text);
        if (t > r + 6 && t * 10 > r * 14) out.push_back({Issue::Length, false, std::to_string(t) + " / " + std::to_string(r)});
    }
    if (const std::string bad = unshowable(text, font); !bad.empty()) out.push_back({Issue::Glyphs, true, bad});
    return out;
}

// ---- A language folder and its files
struct Folder {
    std::string key;                 // the folder's name: the language's key ("Føroyskt", "English (UK)")
    std::vector<fs::path> dirs;      // the languages folder's, then the mods folders' (later ones win, as in the game)
    fs::path modsDir;                // Mods/Languages/<key> (where a game language's changes go)
    bool text = false, voice = false, font = false;
    ojson meta = ojson::object();    // language.json, the last one's
    // its text needs a 16-bit font: "japanese" or "korean" script (the Japanese / Korean disc's)
    bool japanese() const { return script() != "latin"; }
    std::string script() const {
        if (meta.contains("script")) {
            const std::string s = meta.value("script", "");
            return s == "japanese" || s == "korean" ? s : "latin";
        }
        const int g = gameLanguage(key);
        return g == 12 ? "korean" : g >= 11 ? "japanese" : "latin";
    }
    std::string displayName() const {
        const std::string name = meta.value("name", std::string());
        return name.empty() || name == key ? key : name + "  (" + key + ")";
    }
};
// Every language of the languages folder and the mods folder's Mods/Languages, by key.
inline std::vector<Folder> scanFolders(const fs::path& languagesDir, const fs::path& modsLanguagesDir) {
    std::map<std::string, Folder> by;
    std::error_code ec;
    for (const fs::path& root : {languagesDir, modsLanguagesDir}) {
        if (root.empty() || !fs::is_directory(root, ec)) continue;
        for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_directory(ec)) continue;
            const std::string key = utf8Of(e.path().filename());
            if (key.empty() || key[0] == '.') continue;
            Folder& f = by[key];
            f.key = key;
            f.dirs.push_back(e.path());
            f.text = f.text || fs::is_regular_file(e.path() / "Text.json", ec);
            f.voice = f.voice || fs::is_directory(e.path() / "Dialog", ec);
            f.font = f.font || fs::is_regular_file(e.path() / "Font.bin", ec);
            std::ifstream in(e.path() / "language.json");
            if (in) {
                try {
                    ojson j = ojson::parse(in, nullptr, true, true);
                    if (j.is_object()) f.meta = j;
                } catch (...) {
                }
            }
        }
    }
    std::vector<Folder> out;
    for (auto& [key, f] : by) {
        f.modsDir = modsLanguagesDir / pathOf(key);
        out.push_back(std::move(f));
    }
    // (the game's languages first, in the game's order; then the others by name)
    std::stable_sort(out.begin(), out.end(), [](const Folder& a, const Folder& b) {
        const int ga = gameLanguage(a.key), gb = gameLanguage(b.key);
        if ((ga >= 0) != (gb >= 0)) return ga >= 0;
        if (ga >= 0) return ga < gb;
        return a.key < b.key;
    });
    return out;
}

inline ojson readJson(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return ojson();
    try {
        return ojson::parse(in, nullptr, true, true);   // (// comments allowed, as the game reads it)
    } catch (...) {
        return ojson();
    }
}
inline bool writeJson(const fs::path& file, const ojson& j, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.parent_path() / pathOf(utf8Of(file.filename()) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write " + utf8Of(tmp); return false; }
        out << j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        if (!out) { error = "cannot write " + utf8Of(tmp); return false; }
    }
    fs::rename(tmp, file, ec);
    if (ec) { error = "cannot replace " + utf8Of(file) + ": " + ec.message(); return false; }
    return true;
}

// The strings of Text.json files (later ones win): by id, "worlds" by world then id, "port" by name.
struct Texts {
    std::map<std::string, std::string> strings, port;
    std::map<std::string, std::map<std::string, std::string>> worlds;
};
inline void readTexts(const ojson& j, Texts& t) {
    if (!j.is_object()) return;
    if (j.contains("strings") && j["strings"].is_object())
        for (const auto& [k, v] : j["strings"].items())
            if (v.is_string()) t.strings[k] = v.get<std::string>();
    if (j.contains("port") && j["port"].is_object())
        for (const auto& [k, v] : j["port"].items())
            if (v.is_string()) t.port[k] = v.get<std::string>();
    if (j.contains("worlds") && j["worlds"].is_object())
        for (const auto& [w, o] : j["worlds"].items())
            if (o.is_object())
                for (const auto& [k, v] : o.items())
                    if (v.is_string()) t.worlds[w][k] = v.get<std::string>();
}
inline Texts folderTexts(const Folder& f, bool withMods = true) {
    Texts t;
    for (const fs::path& d : f.dirs) {
        if (!withMods && d == f.modsDir) continue;
        readTexts(readJson(d / "Text.json"), t);
    }
    return t;
}
// The numeric order of string ids ("9" before "10"; the others after, by name)
inline bool idLess(const std::string& a, const std::string& b) {
    char* ea = nullptr;
    char* eb = nullptr;
    const long x = std::strtol(a.c_str(), &ea, 0), y = std::strtol(b.c_str(), &eb, 0);
    const bool na = !a.empty() && !*ea, nb = !b.empty() && !*eb;
    if (na != nb) return na;
    if (na && x != y) return x < y;
    return a < b;
}

// The port's built-in column a language's other port strings come from (Sly2-Release/langstore.cpp LangPortBase): a
// game language its own (the 12 of the table), else language.json "text", "code", the folder's base name, English.
inline std::string portBase(const Folder& f) {
    const int g = gameLanguage(f.key);
    if (g >= 0 && g < 13) return f.key;
    if (f.meta.contains("text")) {
        const int t = gameLanguage(f.meta.value("text", ""));
        if (t >= 0 && t < 13) return gameLanguages()[std::size_t(t)];
    }
    const std::string code = f.meta.value("code", "");
    for (int i = 0; i < 13; ++i)
        if (!code.empty() && (code == gameCodes()[std::size_t(i)] || (i == 9 && (code == "nb" || code == "nn")))) return gameLanguages()[std::size_t(i)];
    const int b = gameLanguage(f.key.substr(0, f.key.find(" (")));
    if (b >= 0 && b < 13) return gameLanguages()[std::size_t(b)];
    return "English";
}

// ---- The translation being edited
struct Row {
    std::string id;           // the USA text id, or the port string's name
    bool port = false;
    std::string ref;          // the reference language's (UTF-8)
    std::string english;      // the English (the port's %-codes are the English one's)
    std::string base;         // (a game language) the languages folder's, which a save does not repeat
    std::string text, saved;  // the translation, and as last saved / loaded
    std::vector<Problem> problems;
    bool hasError = false;
};
enum class Status { Untranslated, Same, Done };

struct Translation {
    Folder folder;
    bool gameLanguage = false;   // saved as the mods folder's changes (above)
    std::vector<Row> rows;       // the game's strings (by id), then the port's (by table order)
    std::map<std::string, std::map<std::string, std::string>> worlds, worldsSaved;   // "worlds": world -> id -> text
    ojson file = ojson::object();   // the Text.json written to, as read (its other keys are kept)
    std::string referenceKey;
    Font font;

    fs::path target() const {
        if (gameLanguage) return folder.modsDir / "Text.json";
        // (its own folder: the languages folder's, else the mods folder's)
        return (folder.dirs.empty() ? folder.modsDir : folder.dirs.front()) / "Text.json";
    }
    static Status status(const Row& r) {
        if (r.text.empty()) return Status::Untranslated;
        if (r.text == r.ref) {
            // (a string without letters, "%d / %d", is translated as it is)
            for (char32_t c : decode(r.ref))
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC0) return Status::Same;
        }
        return Status::Done;
    }
    bool changed(const Row& r) const {
        if (r.text != r.saved) return true;
        if (r.port) return false;
        for (const auto& [w, m] : worlds) {
            const auto it = m.find(r.id);
            const auto ws = worldsSaved.find(w);
            const std::string* before = nullptr;
            if (ws != worldsSaved.end())
                if (const auto jt = ws->second.find(r.id); jt != ws->second.end()) before = &jt->second;
            if (it != m.end() && (!before || *before != it->second)) return true;
        }
        for (const auto& [w, m] : worldsSaved)
            if (m.count(r.id) && (!worlds.count(w) || !worlds.at(w).count(r.id))) return true;
        return false;
    }
    void recheck(Row& r) const {
        // (a port string's %-codes are the English one's: porttext.cpp uses it only then)
        r.problems = check(r.ref, r.text, font);
        if (r.port && r.ref != r.english && formats(r.english) != formats(r.text) && !r.text.empty()) {
            std::string f;
            for (const auto& x : formats(r.english)) f += (f.empty() ? "" : " ") + x;
            bool had = false;
            for (const Problem& p : r.problems) had |= p.issue == Issue::Formats;
            if (!had) r.problems.push_back({Issue::Formats, true, f});
        }
        r.hasError = std::any_of(r.problems.begin(), r.problems.end(), [](const Problem& p) { return p.error; });
    }
    void recheckAll() {
        for (Row& r : rows) recheck(r);
    }
};

// The port strings of a built-in column (Sly2-Release --dump-port-text DIR: DIR/<Language>.json), in table order.
inline std::vector<std::pair<std::string, std::string>> readPortColumn(const fs::path& portDir, const std::string& language) {
    std::vector<std::pair<std::string, std::string>> out;
    const ojson j = readJson(portDir / pathOf(language + ".json"));
    if (j.is_object() && j.contains("port") && j["port"].is_object())
        for (const auto& [k, v] : j["port"].items())
            if (v.is_string()) out.emplace_back(k, v.get<std::string>());
    return out;
}

// Loads `f` for editing against the reference language `ref` (a folder of `folders`); portDir: the built-in columns.
inline Translation load(const Folder& f, const Folder* ref, const Folder* english, const fs::path& portDir) {
    Translation t;
    t.folder = f;
    t.gameLanguage = gameLanguage(f.key) >= 0;
    t.referenceKey = ref ? ref->key : std::string("English");
    t.font.japanese = f.japanese();
    const Texts mine = folderTexts(f);
    const Texts base = t.gameLanguage ? folderTexts(f, false) : Texts();
    const Texts reference = ref ? folderTexts(*ref) : Texts();
    const Texts en = english ? folderTexts(*english) : Texts();
    t.file = readJson(t.target());
    if (!t.file.is_object()) t.file = ojson::object();
    // the game's strings: the reference's, then any the language has beyond them
    std::set<std::string, decltype(&idLess)> ids(&idLess);
    for (const auto& [k, v] : reference.strings) ids.insert(k);
    for (const auto& [k, v] : en.strings) ids.insert(k);
    for (const auto& [k, v] : mine.strings) ids.insert(k);
    for (const std::string& id : ids) {
        Row r;
        r.id = id;
        if (auto it = reference.strings.find(id); it != reference.strings.end()) r.ref = it->second;
        if (auto it = en.strings.find(id); it != en.strings.end()) r.english = it->second;
        if (auto it = base.strings.find(id); it != base.strings.end()) r.base = it->second;
        if (auto it = mine.strings.find(id); it != mine.strings.end()) r.text = it->second;
        r.saved = r.text;
        t.rows.push_back(std::move(r));
    }
    t.worlds = t.worldsSaved = mine.worlds;
    // the port's: the built-in English names in table order; the reference's built-in column (its base) under its own
    const auto english12 = readPortColumn(portDir, "English");
    std::map<std::string, std::string> refPort, basePort;
    if (ref) {
        for (const auto& [k, v] : readPortColumn(portDir, portBase(*ref))) refPort[k] = v;
        for (const auto& [k, v] : reference.port) refPort[k] = v;
    }
    if (t.gameLanguage)
        for (const auto& [k, v] : readPortColumn(portDir, portBase(f))) basePort[k] = v;
    for (const auto& [name, en] : english12) {
        Row r;
        r.id = name;
        r.port = true;
        r.english = en;
        r.ref = refPort.count(name) ? refPort[name] : en;
        if (auto it = basePort.find(name); it != basePort.end()) r.base = it->second;
        if (auto it = mine.port.find(name); it != mine.port.end()) r.text = it->second;
        else if (t.gameLanguage) r.text = r.base;   // (a game language has them built in)
        r.saved = r.text;
        t.rows.push_back(std::move(r));
    }
    return t;
}

// The font a language's text is drawn with: Japanese / Korean script: the languages folder's Japanese/Font.bin or
// Korean/Font.bin (its own first)
inline std::set<char32_t> japaneseFont(const Folder& f, const std::vector<Folder>& folders) {
    for (const fs::path& d : f.dirs)
        if (auto s = fontCharacters(d / "Font.bin"); !s.empty()) return s;
    const char* base = f.script() == "korean" ? "Korean" : "Japanese";
    for (const Folder& g : folders)
        if (g.key == base)
            for (const fs::path& d : g.dirs)
                if (auto s = fontCharacters(d / "Font.bin"); !s.empty()) return s;
    return {};
}

// Writes the Text.json (UTF-8; the file's other keys kept, "strings" in id order). A game language: only what differs
// from the languages folder's (row.base).
inline bool save(Translation& t, std::string& error) {
    ojson j = t.file.is_object() ? t.file : ojson::object();
    if (!j.contains("language")) j["language"] = t.folder.key;
    ojson strings = ojson::object(), port = ojson::object();
    // (ids the file had that the table has not stay)
    std::map<std::string, std::string> extra;
    if (j.contains("strings") && j["strings"].is_object())
        for (const auto& [k, v] : j["strings"].items())
            if (v.is_string()) extra[k] = v.get<std::string>();
    for (const Row& r : t.rows) {
        if (r.port) continue;
        extra.erase(r.id);
        if (r.text.empty() || (t.gameLanguage && r.text == r.base)) continue;
        strings[r.id] = r.text;
    }
    for (const auto& [k, v] : extra) strings[k] = v;
    for (const Row& r : t.rows) {
        if (!r.port || r.text.empty() || (t.gameLanguage && r.text == r.base)) continue;
        port[r.id] = r.text;
    }
    j["strings"] = strings;
    ojson worlds = ojson::object();
    for (const auto& [w, m] : t.worlds) {
        ojson o = ojson::object();
        std::vector<std::string> ids;
        for (const auto& [k, v] : m) ids.push_back(k);
        std::sort(ids.begin(), ids.end(), idLess);
        for (const auto& k : ids) o[k] = m.at(k);
        if (!o.empty()) worlds[w] = o;
    }
    if (!worlds.empty() || j.contains("worlds")) j["worlds"] = worlds;
    if (!port.empty() || j.contains("port")) j["port"] = port;
    if (!writeJson(t.target(), j, error)) return false;
    t.file = j;
    for (Row& r : t.rows) r.saved = r.text;
    t.worldsSaved = t.worlds;
    return true;
}

// language.json (the other keys kept): name, code, script, voice, text ("" leaves a key out)
inline bool saveMeta(const fs::path& dir, ojson meta, std::string& error) {
    for (const char* k : {"code", "voice", "text"})
        if (meta.contains(k) && meta[k].is_string() && meta[k].get<std::string>().empty()) meta.erase(k);
    return writeJson(dir / "language.json", meta, error);
}

// ---- CSV (a spreadsheet's or another tool's table): id, reference, translation; "port:" before a port string's name
inline std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}
inline std::string toCsv(const Translation& t) {
    std::string out = "id,reference,translation\n";
    for (const Row& r : t.rows) out += csvField(r.port ? "port:" + r.id : r.id) + "," + csvField(r.ref) + "," + csvField(r.text) + "\n";
    return out;
}
inline std::vector<std::vector<std::string>> parseCsv(const std::string& s) {
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"' && i + 1 < s.size() && s[i + 1] == '"') field += '"', ++i;
            else if (c == '"') quoted = false;
            else field += c;
            continue;
        }
        if (c == '"') quoted = true, any = true;
        else if (c == ',') rows.back().push_back(field), field.clear(), any = true;
        else if (c == '\r') continue;
        else if (c == '\n') {
            rows.back().push_back(field), field.clear();
            rows.emplace_back();
            any = false;
        } else field += c, any = true;
    }
    if (any || !field.empty()) rows.back().push_back(field);
    if (rows.back().empty()) rows.pop_back();
    return rows;
}
// Takes the CSV's translations (a header row and rows "id,reference,translation"; or "id,translation"); returns how
// many strings changed, -1 when it is not such a table.
inline int importCsv(Translation& t, const std::string& csv) {
    const auto table = parseCsv(csv.compare(0, 3, "\xEF\xBB\xBF") == 0 ? csv.substr(3) : csv);
    if (table.empty()) return -1;
    const auto& head = table.front();
    int col = -1;
    for (std::size_t i = 0; i < head.size(); ++i)
        if (head[i] == "translation" || head[i] == "text" || head[i] == "target") col = int(i);
    if (head.empty() || head[0] != "id" || col < 0) return -1;
    std::map<std::string, Row*> by;
    for (Row& r : t.rows) by[r.port ? "port:" + r.id : r.id] = &r;
    int changed = 0;
    for (std::size_t i = 1; i < table.size(); ++i) {
        const auto& row = table[i];
        if (row.size() <= std::size_t(col) || row[0].empty()) continue;
        const auto it = by.find(row[0]);
        if (it == by.end() || it->second->text == row[std::size_t(col)]) continue;
        it->second->text = row[std::size_t(col)];
        t.recheck(*it->second);
        ++changed;
    }
    return changed;
}

// ---- Where each string is used (Sly2-Release --dump-text DIR: DIR/usage.json)
// Version 2 (2026-11-17) adds each world's episode and each dialog line's speaker and headset flag (textmod.cpp).
struct DialogInfo {
    int speaker = -1;        // the line's speaker: the class id of its stream (0x35 Sly, 0x36 Murray, ...), -1 none
    bool headset = false;    // the game's USB headset plays it: the radio / binocucom talk
};
struct Usage {
    int version = 0;
    std::vector<std::string> worlds;
    std::vector<int> episodes;                       // per world (version 2): its episode, 0 the prologue, -1 the front end
    std::map<std::string, std::vector<int>> strings;
    std::map<std::string, DialogInfo> dialogs;       // (version 2) by USA line id
    std::string disc;
    bool empty() const { return worlds.empty(); }
};
inline Usage readUsage(const fs::path& file) {
    Usage u;
    const ojson j = readJson(file);
    if (!j.is_object()) return u;
    u.version = j.value("version", 1);
    u.disc = j.value("disc", "");
    if (j.contains("worlds"))
        for (const auto& w : j["worlds"])
            if (w.is_string()) u.worlds.push_back(w.get<std::string>());
    if (j.contains("episodes") && j["episodes"].is_array())
        for (const auto& e : j["episodes"]) u.episodes.push_back(e.is_number_integer() ? e.get<int>() : -1);
    if (j.contains("strings") && j["strings"].is_object())
        for (const auto& [k, v] : j["strings"].items())
            if (v.is_array())
                for (const auto& i : v)
                    if (i.is_number_integer()) u.strings[k].push_back(i.get<int>());
    if (j.contains("dialogs") && j["dialogs"].is_object())
        for (const auto& [k, v] : j["dialogs"].items())
            if (v.is_array() && v.size() >= 2 && v[0].is_number_integer() && v[1].is_number_integer())
                u.dialogs[k] = {v[0].get<int>(), v[1].get<int>() != 0};
    return u;
}

// ---- Levels: a string or voice line grouped by the episode whose worlds use it
// The groups: 0 the prologue (Cairo), 1..8 the episodes, then the front end's alone, those of several, and of none.
enum : int { kLevelMenus = 100, kLevelShared = 101, kLevelNone = 102 };
// A world's episode: usage.json's (version 2), else by the world's name (the converter's names, every disc: the
// boot world's records, Sly2-Release/convert.cpp s_aworldname); -1 the front end, -2 not known.
inline int worldEpisode(const Usage& u, int w) {
    if (w < 0 || w >= int(u.worlds.size())) return -2;
    if (w < int(u.episodes.size())) return u.episodes[std::size_t(w)];
    static const std::map<std::string, int> byName = {
        {"Cairo Museum", 0}, {"Front End", -1}, {"Paris", 1}, {"Wine Cellar", 1}, {"Dimitri's Nightclub", 1},
        {"Printing Press", 1}, {"Moulin Rouge", 1}, {"Waterpump Room", 1}, {"Rajan's Palace", 2}, {"Palace Guest Rooms", 2},
        {"Palace Basement", 2}, {"Palace Ballroom", 2}, {"Jungle Temple", 3}, {"Spice Factory", 3}, {"Prague Prison", 4},
        {"Prison Interior", 4}, {"Prison Vault", 4}, {"Contessa's Castle", 5}, {"Castle Water Rails", 5},
        {"Castle Guard Break", 5}, {"The Crypt", 5}, {"Castle Heist", 5}, {"Crypt Training Room", 5}, {"Crypt Tomb", 5},
        {"Crypt Hall", 5}, {"Crypt Water Room", 5}, {"Canada Train Station", 6}, {"Train Cabins", 6}, {"Train A", 6},
        {"Train B", 6}, {"Train C", 6}, {"Sawmill", 7}, {"Sawmill Mulch Room", 7}, {"Sawmill Burn Room", 7},
        {"Lighthouse", 7}, {"Bear Cave", 7}, {"Jean Bison Battle", 7}, {"Arpeggio's Blimp", 8}, {"Blimp Interior", 8},
        {"Engine Room (Murray)", 8}, {"Engine Room (Bentley)", 8}, {"Engine Room (Sly)", 8}, {"Clock-La Battle", 8}};
    const auto it = byName.find(u.worlds[std::size_t(w)]);
    return it == byName.end() ? -2 : it->second;
}
// The level group of a string (by USA id): the one episode of every world that has it (the front end counts as its
// own); several: shared (the HUD, the gadgets, the hub talk of more than one episode); none: not in any level.
inline int levelOf(const Usage& u, const std::string& id) {
    const auto it = u.strings.find(id);
    if (it == u.strings.end() || it->second.empty()) return kLevelNone;
    std::set<int> groups;
    for (int w : it->second) {
        const int e = worldEpisode(u, w);
        if (e == -2) return kLevelShared;   // (a world not known: no guess)
        groups.insert(e < 0 ? kLevelMenus : e);
    }
    return groups.size() == 1 ? *groups.begin() : kLevelShared;
}
// The episode's first world in usage.json's order (its hub: "Paris", "Rajan's Palace"); "" when none.
inline std::string episodeHub(const Usage& u, int episode) {
    for (int w = 0; w < int(u.worlds.size()); ++w)
        if (worldEpisode(u, w) == episode) return u.worlds[std::size_t(w)];
    return {};
}

// ---- Speakers: a dialog line's speaker class (usage.json "dialogs"), as a character's name
// The classes the dialog tables name (measured on the USA disc: every speaker id there; the guards' classes speak as
// "Guards"); others are "Other". The names are the game's own characters (not translated).
inline const char* speakerName(int cid) {
    switch (cid) {
#include "loc_speakers.inc"
    default: return nullptr;
    }
}
// The speaker groups' order: Sly, Bentley, Murray, Carmelita, then the others by name; "Other" last
inline int speakerRank(int cid) {
    switch (cid) {
    case 0x35: return 0;
    case 0x37: return 1;
    case 0x36: return 2;
    case 0x3F: return 3;
    default: return speakerName(cid) ? 10 : 100;
    }
}

// ---- Dub takes: the dry take (as recorded / imported, before the radio effect) and the per-line choices, beside the
// language's Dialog folder in Takes/ (the game reads only Dialog/ and Movies/): Takes/<id>.wav, Takes/takes.json
// {"<id>": {"radio": true|false}} (a line not in it: the detected choice).
inline fs::path takePath(const fs::path& dir, const std::string& id) { return dir / "Takes" / pathOf(id + ".wav"); }
inline std::map<std::string, bool> readTakeChoices(const fs::path& dir) {
    std::map<std::string, bool> out;
    const ojson j = readJson(dir / "Takes" / "takes.json");
    if (j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_object() && v.contains("radio") && v["radio"].is_boolean()) out[k] = v["radio"].get<bool>();
    return out;
}
inline bool saveTakeChoice(const fs::path& dir, const std::string& id, bool radio, std::string& error) {
    ojson j = readJson(dir / "Takes" / "takes.json");
    if (!j.is_object()) j = ojson::object();
    j[id]["radio"] = radio;
    return writeJson(dir / "Takes" / "takes.json", j, error);
}

// ---- Button codes as icons: a text cut into runs of text and icons ("&2X&." -> the X icon)
struct Piece {
    bool icon = false;
    std::string text;    // the text, or the icon's character ("X", "O", "(" ...)
};
inline std::vector<Piece> pieces(const std::string& s) {
    std::vector<Piece> out;
    auto addText = [&](const std::string& t) {
        if (t.empty()) return;
        if (!out.empty() && !out.back().icon) out.back().text += t;
        else out.push_back({false, t});
    };
    std::size_t i = 0;
    bool inIcons = false;   // (after "&2": the characters up to "&." are icons, as the game's font does)
    while (i < s.size()) {
        if (s[i] == '&' && i + 1 < s.size() && s[i + 1] == '.') { inIcons = false; i += 2; continue; }
        if (s[i] == '&' && i + 2 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            inIcons = s[i + 1] == '2';
            i += 2;
            if (!inIcons) continue;
        }
        std::size_t n = 1;
        const auto b = static_cast<unsigned char>(s[i]);
        if (b >= 0xF0) n = 4; else if (b >= 0xE0) n = 3; else if (b >= 0xC0) n = 2;
        const std::string ch = s.substr(i, n);
        if (inIcons && ch != "\n") out.push_back({true, ch});
        else addText(ch);
        i += n;
    }
    return out;
}
// The icon codes the palette offers (the game's icon font: Sly2-Release/promptglyph.h), in its order.
inline const std::vector<std::string>& iconCodes() {
    static const std::vector<std::string> a = {"X", "O", "S", "T", "L", "R", "l", "r", "(", ")", "+", "!", "_"};
    return a;
}

// ---- Voices: the dialog lines (Dialog/<any subfolders>/<USA line id>.wav|.flac|.vag: the line's subtitle is the text of
// the same id) and the movies' sound (Movies/<name>.wav|.flac), Sly2-Release/langstore.h.
inline bool audioFile(const fs::path& p) {
    std::string e = utf8Of(p.extension());
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".wav" || e == ".flac" || e == ".vag";
}
// A file's length in seconds (WAV: its data; FLAC: its STREAMINFO; VAG: its header), -1 when not known.
inline double audioSeconds(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return -1;
    std::vector<unsigned char> h(4096);
    in.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()));
    h.resize(std::size_t(in.gcount()));
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    auto le = [&](std::size_t i, int n) { uint64_t v = 0; for (int k = n - 1; k >= 0; --k) v = v << 8 | h[i + std::size_t(k)]; return v; };
    auto be = [&](std::size_t i, int n) { uint64_t v = 0; for (int k = 0; k < n; ++k) v = v << 8 | h[i + std::size_t(k)]; return v; };
    if (h.size() >= 12 && std::equal(h.begin(), h.begin() + 4, "RIFF") && std::equal(h.begin() + 8, h.begin() + 12, "WAVE")) {
        uint64_t bytesPerSecond = 0;
        for (std::size_t at = 12; at + 8 <= h.size();) {
            const uint64_t n = le(at + 4, 4);
            if (std::equal(h.begin() + long(at), h.begin() + long(at) + 4, "fmt ") && at + 20 <= h.size()) bytesPerSecond = le(at + 16, 4);
            else if (std::equal(h.begin() + long(at), h.begin() + long(at) + 4, "data"))
                return bytesPerSecond ? double(std::min<uint64_t>(n, size - at - 8)) / double(bytesPerSecond) : -1;
            at += 8 + n + (n & 1);
        }
        return -1;
    }
    if (h.size() >= 42 && std::equal(h.begin(), h.begin() + 4, "fLaC")) {
        // (STREAMINFO, the first block: sample rate 20 bits, channels 3, bits 5, total samples 36)
        const uint64_t x = be(18, 8);
        const uint64_t rate = x >> 44, total = x & 0xFFFFFFFFFull;
        return rate ? double(total) / double(rate) : -1;
    }
    if (h.size() >= 48 && std::equal(h.begin(), h.begin() + 4, "VAGp")) {
        const uint64_t bytes = be(12, 4), rate = be(16, 4);
        return rate ? double(bytes / 16 * 28) / double(rate) : -1;
    }
    return -1;
}
struct VoiceFiles {
    std::map<std::string, std::pair<fs::path, std::string>> lines;   // line id -> the file, its subfolder ("Train A")
    std::map<std::string, fs::path> movies;                          // a movie's name -> its sound
    std::map<std::string, double> lengths;                           // Dialog/lengths.json
};
// The voice files of a language's folders (later ones win, as in the game).
inline VoiceFiles scanVoices(const std::vector<fs::path>& dirs) {
    VoiceFiles v;
    std::error_code ec;
    for (const fs::path& d : dirs) {
        if (fs::is_directory(d / "Dialog", ec))
            for (auto it = fs::recursive_directory_iterator(d / "Dialog", fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec) || !audioFile(it->path())) continue;
                const std::string stem = utf8Of(it->path().stem());
                if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) continue;
                const fs::path rel = it->path().parent_path().lexically_relative(d / "Dialog");
                v.lines[stem] = {it->path(), rel == "." ? std::string() : utf8Of(rel.generic_u8string())};
            }
        if (fs::is_directory(d / "Movies", ec))
            for (const auto& e : fs::directory_iterator(d / "Movies", fs::directory_options::skip_permission_denied, ec))
                if (e.is_regular_file(ec) && audioFile(e.path())) v.movies[utf8Of(e.path().stem())] = e.path();
        const ojson j = readJson(d / "Dialog" / "lengths.json");
        if (j.is_object())
            for (const auto& [k, x] : j.items())
                if (x.is_number()) v.lengths[k] = x.get<double>();
    }
    return v;
}
// One voice of the Voices list: a line (by id) or a movie (by name)
struct VoiceRow {
    std::string id;
    bool movie = false;
    std::string group;          // the reference's subfolder (a line), where a new dub goes too
    fs::path ref, dub;          // the reference's file, the language's (empty: none)
    double refSeconds = -2, dubSeconds = -2;   // (-2: not read yet)
};
inline std::vector<VoiceRow> voiceRows(const VoiceFiles& ref, const VoiceFiles& mine) {
    std::vector<VoiceRow> out;
    std::vector<std::string> ids;
    for (const auto& [k, x] : ref.lines) ids.push_back(k);
    for (const auto& [k, x] : mine.lines)
        if (!ref.lines.count(k)) ids.push_back(k);
    std::sort(ids.begin(), ids.end(), idLess);
    for (const std::string& id : ids) {
        VoiceRow r;
        r.id = id;
        if (auto it = ref.lines.find(id); it != ref.lines.end()) r.ref = it->second.first, r.group = it->second.second;
        if (auto it = mine.lines.find(id); it != mine.lines.end()) {
            r.dub = it->second.first;
            if (r.group.empty()) r.group = it->second.second;
        }
        out.push_back(std::move(r));
    }
    std::set<std::string> names;
    for (const auto& [k, x] : ref.movies) names.insert(k);
    for (const auto& [k, x] : mine.movies) names.insert(k);
    for (const std::string& n : names) {
        VoiceRow r;
        r.id = n;
        r.movie = true;
        if (auto it = ref.movies.find(n); it != ref.movies.end()) r.ref = it->second;
        if (auto it = mine.movies.find(n); it != mine.movies.end()) r.dub = it->second;
        out.push_back(std::move(r));
    }
    return out;
}
// Where a dub of the row goes in the language's folder `dir` (with the file's extension).
inline fs::path dubPath(const fs::path& dir, const VoiceRow& r, const std::string& ext) {
    if (r.movie) return dir / "Movies" / pathOf(r.id + ext);
    return (r.group.empty() ? dir / "Dialog" : dir / "Dialog" / pathOf(r.group)) / pathOf(r.id + ext);
}

}  // namespace cane::loc
#include "loc_audio.h"
namespace cane::loc {

// The disc check (SAFEHOUSE_LOC_DISC_CHECK="<usage.json>|<a language's Dialog folder>", --loc-selftest): the levels and
// speakers of the real strings and lines, and the radio detector against the dialog tables' headset flag.
inline void discCheck(const std::string& spec, const std::function<void(bool, const char*)>& expect) {
    const auto bar = spec.find('|');
    const Usage u = readUsage(pathOf(spec.substr(0, bar)));
    expect(u.version >= 2 && !u.dialogs.empty(), "disc: usage.json version 2 with the dialog tables");
    std::map<int, int> perLevel;
    for (const auto& [id, w] : u.strings) perLevel[levelOf(u, id)]++;
    std::printf("disc: strings per level:");
    for (const auto& [l, n] : perLevel) std::printf(" %s=%d", l < 100 ? ("ep" + std::to_string(l)).c_str() : l == kLevelMenus ? "menus" : l == kLevelShared ? "shared" : "none", n);
    std::printf("\n");
    expect(perLevel.size() >= 10 && perLevel[1] > 100 && perLevel[kLevelShared] < int(u.strings.size()) / 3, "disc: every episode has its strings, shared is a minority");
    std::map<std::string, int> perSpeaker;
    for (const auto& [id, d] : u.dialogs) perSpeaker[speakerName(d.speaker) ? speakerName(d.speaker) : "Other"]++;
    std::printf("disc: lines per speaker:");
    for (const auto& [n, c] : perSpeaker) std::printf(" %s=%d", n.c_str(), c);
    std::printf("\n");
    expect(perSpeaker["Other"] * 20 < int(u.dialogs.size()), "disc: under 5% of the lines without a named speaker");
    if (bar == std::string::npos) return;
    const VoiceFiles v = scanVoices({pathOf(spec.substr(bar + 1)).parent_path()});
    int flagged = 0, found = 0, others = 0, called = 0;
    for (const auto& [id, file] : v.lines) {
        const auto it = u.dialogs.find(id);
        if (it == u.dialogs.end()) continue;
        const audio::Mono m = audio::readWav(file.first);
        if (m.empty()) continue;
        const bool r = audio::radioLike(audio::spectrum(m));
        if (it->second.headset) ++flagged, found += r;
        else ++others, called += r;
    }
    std::printf("disc: radio detector: %d of %d flagged lines, %d of %d others\n", found, flagged, called, others);
    expect(flagged > 0 && found * 100 >= flagged * 85 && called * 100 <= others * 20, "disc: the radio detector finds 85% of the flagged lines, under 20% of the others");
}

// The rules above on fixed cases (safehouse --loc-selftest): 0 when all pass.
inline int selfTest() {
    int fails = 0, checks = 0;
    auto expect = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) ++fails, std::printf("FAIL %s\n", what);
    };
    const Font latin;
    auto issues = [&](const std::string& ref, const std::string& text, const Font& font) {
        std::set<Issue> s;
        for (const Problem& p : check(ref, text, font)) s.insert(p.issue);
        return s;
    };
    expect(issues("Press &2X&. to jump.", "Trýst &2X&. fyri at loypa.", latin).empty(), "a good translation has no problems");
    expect(issues("Press &2X&. to jump.", "Trýst X fyri at loypa.", latin).count(Issue::Codes), "a lost icon");
    expect(issues("Press &2X&. or &2O&.", "&2O&. ella &2X&.", latin).empty(), "icons in another order");
    expect(issues("Press %s to place.", "Trýst á %d.", latin).count(Issue::Formats), "another printf code");
    expect(issues("Page %d / %d", "Síða %d av %d", latin).empty(), "the same printf codes");
    expect(issues("100%% done %s", "liðugt %s", latin).empty(), "%% is no code");
    expect(issues("A\nB", "A B", latin).count(Issue::Lines), "a line break less");
    expect(issues(" On", "Á", latin).count(Issue::Spaces), "the leading space");
    expect(issues("Back", "Aftur til høvuðsvalmyndina", latin).count(Issue::Length), "much longer");
    expect(issues("x", "Þetta ½ €", latin).count(Issue::Glyphs) && unshowable("Þetta ½ €", latin) == "Þ½€", "the Latin fonts' missing letters");
    expect(unshowable("“Fín” ð æ ø å … &2X&.", latin).empty(), "the Latin fonts' letters, quotes plain");
    std::set<char32_t> kana = {U'あ', U'い', 'A'};
    const Font jp{true, &kana};
    expect(unshowable("あいA\n", jp).empty() && unshowable("あう", jp) == "う", "the Japanese font's characters");
    std::set<char32_t> jamo = {' ', 0x1100, 0x1161, 0x11AB, 0x1112, 'A'};
    const Font kr{true, &jamo};
    expect(unshowable("가 간 하A", kr).empty() && unshowable("각", kr) == "각" && unshowable("고", kr) == "고", "a Korean font's syllables from its jamo");
    expect(controlCodes("&2X&. && &x").size() == 2, "the control codes");
    expect(longestLine("ab\n&2X&.cde") == 5, "a line's length with an icon");
    const auto csv = parseCsv("id,reference,translation\n5,\"a, \"\"b\"\"\",\"x\ny\"\nport:HoldToSkip,Hold,Halt\n");
    expect(csv.size() == 3 && csv[1][1] == "a, \"b\"" && csv[1][2] == "x\ny" && csv[2][0] == "port:HoldToSkip", "the CSV fields");
    {
        Translation t;
        Row a;
        a.id = "5";
        a.ref = "a";
        Row p;
        p.id = "HoldToSkip";
        p.port = true;
        p.ref = p.english = "Hold &2O&. to skip";
        t.rows = {a, p};
        expect(importCsv(t, toCsv(t)) == 0, "a table read back changes nothing");
        expect(importCsv(t, "id,reference,translation\n5,a,b\nport:HoldToSkip,x,Halt &2O&.\n") == 2 && t.rows[0].text == "b" && t.rows[1].text == "Halt &2O&.",
               "a table's translations");
        expect(Translation::status(t.rows[0]) == Status::Done && Translation::status(Row{}) == Status::Untranslated, "the status");
    }
    {
        Folder f;
        f.key = "Føroyskt";
        f.meta = {{"code", "fo"}};
        expect(portBase(f) == "English", "a new language's port strings: English");
        f.meta = {{"code", "nb"}};
        expect(portBase(f) == "Norwegian", "by its code");
        f.meta = {{"text", "Danish"}, {"code", "fo"}};
        expect(portBase(f) == "Danish", "by its text language");
        f.key = "French (Québec)";
        f.meta = ojson::object();
        expect(portBase(f) == "French", "by its folder name");
    }
    {
        VoiceFiles ref, mine;
        ref.lines["2565"] = {"/r/Dialog/Train A/2565.wav", "Train A"};
        ref.lines["10"] = {"/r/Dialog/10.wav", ""};
        mine.lines["2565"] = {"/m/Dialog/x/2565.flac", "x"};
        ref.movies["Ep1 Intro"] = "/r/Movies/Ep1 Intro.wav";
        const auto rows = voiceRows(ref, mine);
        expect(rows.size() == 3 && rows[0].id == "10" && rows[0].dub.empty() && rows[1].dub == "/m/Dialog/x/2565.flac" && rows[2].movie,
               "the voices: lines in id order, a dub found, the movies after");
        expect(dubPath("/m", rows[1], ".wav") == fs::path("/m/Dialog/Train A/2565.wav") && dubPath("/m", rows[2], ".flac") == fs::path("/m/Movies/Ep1 Intro.flac"),
               "where a dub goes");
    }
    {
        // levels and speakers on a made-up usage.json: worlds of the prologue, episode 1 (two), the front end, episode 2
        Usage u;
        u.version = 2;
        u.worlds = {"Cairo Museum", "Front End", "Paris", "Wine Cellar", "Rajan's Palace"};
        u.episodes = {0, -1, 1, 1, 2};
        u.strings = {{"1", {0}}, {"2", {2, 3}}, {"3", {1}}, {"4", {2, 4}}, {"5", {1, 2}}, {"6", {4}}};
        u.dialogs = {{"2", {0x37, true}}, {"6", {0x3D, false}}, {"4", {0x99, false}}};
        std::map<int, int> per;
        for (const char* id : {"1", "2", "3", "4", "5", "6", "7"}) per[levelOf(u, id)]++;
        expect(per[0] == 1 && per[1] == 1 && per[2] == 1 && per[kLevelMenus] == 1 && per[kLevelShared] == 2 && per[kLevelNone] == 1,
               "the levels: one episode's worlds, the menus, shared, none");
        Usage old = u;
        old.episodes.clear();
        expect(levelOf(old, "2") == 1 && levelOf(old, "6") == 2 && levelOf(old, "3") == kLevelMenus, "the levels of a version 1 usage.json: by the worlds' names");
        expect(episodeHub(u, 1) == "Paris" && std::string(speakerName(0x37)) == "Bentley" && speakerName(0x99) == nullptr && speakerRank(0x35) < speakerRank(0x3D) &&
                   speakerRank(0x3D) < speakerRank(0x99),
               "the speakers: named, ordered, the unknown ones Other");
    }
    {
        const auto p = pieces("Press &2X&. or &2O&. to jump.\nHold &2R&.");
        expect(p.size() == 6 && p[0].text == "Press " && p[1].icon && p[1].text == "X" && p[3].icon && p[3].text == "O" && p[4].text == " to jump.\nHold " &&
                   p[5].icon && p[5].text == "R",
               "button codes as icons");
        expect(pieces("Tom & Jerry 100%").size() == 1, "an ampersand is text");
    }
    audio::audioSelfTest([&](bool ok, const char* what) { expect(ok, what); });
    if (const char* sz = std::getenv("SAFEHOUSE_LOC_DISC_CHECK"); sz && *sz) discCheck(sz, [&](bool ok, const char* what) { expect(ok, what); });
    std::printf("loc selftest: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

}  // namespace cane::loc
