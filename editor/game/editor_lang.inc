// The editor's own strings in the user's language (notes/EDITOR_PLAN.md step 7): T("English") gives the translation
// from the language's table (keyed by the English text), else the English. TL("English", "id") is "text###id" (a
// translated ImGui label with a fixed id).
namespace edimpl
{

namespace
{
std::unordered_map<std::string, std::string>& LangTable()
{
    static std::unordered_map<std::string, std::string> s_mp;
    return s_mp;
}
}  // namespace

const char* T(const char* szEnglish)
{
    const auto& mp = LangTable();
    if (mp.empty()) return szEnglish;
    const auto it = mp.find(szEnglish);
    return it == mp.end() || it->second.empty() ? szEnglish : it->second.c_str();
}

std::string TL(const char* szEnglish, const char* id) { return std::string(T(szEnglish)) + "###" + id; }

}  // namespace edimpl
