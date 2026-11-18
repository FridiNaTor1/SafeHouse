// The editor's own strings in the user's language (notes/EDITOR_PLAN.md step 12): T("English") gives the translation
// from the language's table (editor/lang/<code>.json, keyed by the English text, embedded at build time:
// generated/editor3d_lang.h), else the English. TL("English", "id") is "text###id" (a translated ImGui label with a
// fixed id). The language: --editor-language CODE (the launcher passes its own), else SAFEHOUSE_LANGUAGE, else English.
// The letters Atkinson Hyperlegible lacks (Cyrillic, kana, kanji, Hangul) come from the launcher's Noto subsets
// (launcher/assets/l10n, scripts/make-launcher-l10n-fonts.py also reads editor/lang), merged into the editor's font.
#if defined(CANE_EDITOR3D_L10N) && __has_include("editor3d_lang.h") && __has_include("editor3d_l10n_fonts.h")
#include "editor3d_lang.h"
#include "editor3d_l10n_fonts.h"
#define CANE_EDITOR3D_HAS_L10N 1
#endif

namespace edimpl
{

namespace
{
std::unordered_map<std::string, std::string>& LangTable()
{
    static std::unordered_map<std::string, std::string> s_mp;
    return s_mp;
}
std::string& LangCode()
{
    static std::string s_code = "en";
    return s_code;
}
}  // namespace

// the language's table loaded (unknown codes: English); the codes it has
bool FSetEditorLanguage(const std::string& codeIn)
{
    std::string code = codeIn;
    if (code.empty())
        if (const char* e = std::getenv("SAFEHOUSE_LANGUAGE")) code = e;
    for (char& ch : code) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (const auto u = code.find_first_of("_-"); u != std::string::npos) code = code.substr(0, u);   // (pt_PT, nb-NO)
    LangTable().clear();
    LangCode() = "en";
#if CANE_EDITOR3D_HAS_L10N
    for (const CaneLangBlob& b : cane_editor3d_lang_blobs)
    {
        if (code != b.code || code == "en") continue;
        const nlohmann::json j = nlohmann::json::parse(b.data, b.data + b.size, nullptr, false);
        if (!j.is_object() || !j.contains("strings") || !j["strings"].is_object()) return false;
        for (const auto& [k, v] : j["strings"].items())
            if (v.is_string()) LangTable()[k] = v.get<std::string>();
        LangCode() = code;
        return true;
    }
#endif
    return code.empty() || code == "en";
}

const std::string& EditorLanguage() { return LangCode(); }

const char* T(const char* szEnglish)
{
    const auto& mp = LangTable();
    if (mp.empty()) return szEnglish;
    const auto it = mp.find(szEnglish);
    return it == mp.end() || it->second.empty() ? szEnglish : it->second.c_str();
}

std::string TL(const char* szEnglish, const char* id) { return std::string(T(szEnglish)) + "###" + id; }

// the active language's letters the editor's font lacks, merged from the Noto subsets (call after the main font)
void AddEditorLanguageFonts(ImGuiIO& io, float pixels)
{
#if CANE_EDITOR3D_HAS_L10N
    if (LangTable().empty()) return;
    std::vector<uint32_t> acp;
    auto add = [&](const std::string& s) {
        for (std::size_t i = 0; i < s.size();)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            uint32_t cp = c;
            int n = 1;
            if (c >= 0xF0) { cp = c & 7; n = 4; }
            else if (c >= 0xE0) { cp = c & 15; n = 3; }
            else if (c >= 0xC0) { cp = c & 31; n = 2; }
            for (int k = 1; k < n && i + std::size_t(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + std::size_t(k)]) & 63);
            i += std::size_t(n);
            if (cp > 0x17F && cp <= 0xFFFF) acp.push_back(cp);
        }
    };
    for (const auto& [k, v] : LangTable()) add(v);
    if (acp.empty()) return;
    std::sort(acp.begin(), acp.end());
    acp.erase(std::unique(acp.begin(), acp.end()), acp.end());
    static std::vector<ImWchar> s_ranges;
    s_ranges.clear();
    for (uint32_t c : acp)
    {
        if (!s_ranges.empty() && s_ranges.back() + 1 == ImWchar(c)) s_ranges.back() = ImWchar(c);
        else { s_ranges.push_back(ImWchar(c)); s_ranges.push_back(ImWchar(c)); }
    }
    s_ranges.push_back(0);
    ImFontConfig merge;
    merge.FontDataOwnedByAtlas = false;
    merge.MergeMode = true;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(cane_editor3d_noto), int(sizeof(cane_editor3d_noto)), pixels, &merge, s_ranges.data());
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(cane_editor3d_cjk), int(sizeof(cane_editor3d_cjk)), pixels, &merge, s_ranges.data());
#else
    (void)io; (void)pixels;
#endif
}

}  // namespace edimpl
