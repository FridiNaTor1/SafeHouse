// The map editor (editor_impl.h, shared: editor/game/editor_script.inc): a group's splice scripts (Scripts.splice: the
// converter's text, compiled back to the original block by FSpliceFromText when the world is put together;
// worldparts.h), edited and checked (syntax, module order, built-in names, and the literal oids the scripts look objects
// up by). Sly 2 and Sly 3 (each game's own splice compiler).
#include "editor_impl.h"
#if EDGAME_SLY2
#include "splicenames.h"   // (the readable names: editor_scriptnames.inc)
#endif

#if CANE_EDITOR_3D && EDFEAT_SCRIPTS

#include <nlohmann/json.hpp>

#include <regex>
#include <set>

namespace edimpl
{

void CEditor::LoadScriptText(int igroup)
{
    m_igroupScript = igroup;
    m_scriptText.clear();
    if (igroup >= 0 && igroup < int(m_index.groups.size()) && !m_index.groups[igroup].scriptsPart.empty())
    {
        std::vector<uint8_t> ab;
        m_proj.readPart(m_index.groups[igroup].scriptsPart, ab);
        m_scriptText.assign(ab.begin(), ab.end());
#if EDGAME_SLY2
        // (names: display only, editor_scriptnames.inc)
        if (m_fScriptReadable) m_scriptText = ScriptReadable(igroup, m_scriptText);
#endif
    }
    m_scriptEdit = m_scriptText;
    m_scriptError.clear();
    m_scriptErrorLine = 0;
    m_fScriptDirty = false;
}

std::set<int> OidsLookedUp(const std::string& text);

bool CEditor::FCheckScript(const std::string& text, std::string& error, int& line, std::vector<PROBLEM>* papb, int igroup)
{
    line = 0;
    std::vector<uint8_t> ab;
    if (!FSpliceFromText(text, ab, error))
    {
        const auto i = error.find("line ");
        if (i != std::string::npos) line = std::atoi(error.c_str() + i + 5);
        return false;
    }
    if (!papb) return true;
    // the literal oids of (<find-object> N ...), (<find-object-kind-4> N ...), (<find-objects-of-names> (quote (N ...)))
    std::set<int> setOid;
    for (const auto& up : m_psw->aploAll) setOid.insert(up->oid);
    for (const EDITOBJREC& r : m_rec.aobj) setOid.insert(r.oid);
    for (const ce::GroupInfo& g : m_index.groups)
        for (const ce::ObjRecord& r : g.objects) setOid.insert(r.oid);
    // (an episode's shared scripts look up other worlds' objects too: only the oids the installed scripts do not)
    if (igroup >= 0 && igroup < int(m_index.groups.size()) && !m_index.groups[igroup].scriptsPart.empty())
    {
        std::vector<uint8_t> ab0;
        if (ce::readFile(m_inst.dir / ce::pathOf(m_index.groups[igroup].scriptsPart), ab0))
            for (int oid : OidsLookedUp(std::string(ab0.begin(), ab0.end()))) setOid.insert(oid);
    }
    auto lineAt = [&](std::size_t ib) { return 1 + int(std::count(text.begin(), text.begin() + std::ptrdiff_t(ib), '\n')); };
    static const std::regex s_re1(R"(\(<find-object(?:-kind-4)?>\s+(-?\d+))");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), s_re1); it != std::sregex_iterator(); ++it)
    {
        const int oid = std::atoi((*it)[1].str().c_str());
        if (oid > 0 && !setOid.count(oid)) papb->push_back({0, "line " + std::to_string(lineAt(std::size_t(it->position()))) + ": looks up oid " + std::to_string(oid) + ", which is not in the world"});
    }
    static const std::regex s_re2(R"(\(<find-objects-of-names>\s+\(quote\s+\(([-\d\s]+)\)\))");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), s_re2); it != std::sregex_iterator(); ++it)
    {
        std::stringstream ss((*it)[1].str());
        int oid;
        while (ss >> oid)
            if (oid > 0 && !setOid.count(oid)) papb->push_back({0, "line " + std::to_string(lineAt(std::size_t(it->position()))) + ": looks up oid " + std::to_string(oid) + ", which is not in the world"});
    }
    return true;
}

std::set<int> OidsLookedUp(const std::string& text)
{
    std::set<int> set;
    static const std::regex s_re(R"(\(<find-object(?:-kind-4)?>\s+(-?\d+))");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), s_re); it != std::sregex_iterator(); ++it) set.insert(std::atoi((*it)[1].str().c_str()));
    static const std::regex s_re2(R"(\(<find-objects-of-names>\s+\(quote\s+\(([-\d\s]+)\)\))");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), s_re2); it != std::sregex_iterator(); ++it)
    {
        std::stringstream ss((*it)[1].str());
        int oid;
        while (ss >> oid) set.insert(oid);
    }
    return set;
}

bool CEditor::FApplyScript(int igroup, const std::string& text, std::string& error)
{
    if (igroup < 0 || igroup >= int(m_index.groups.size()) || m_index.groups[igroup].scriptsPart.empty()) { error = "no scripts part"; return false; }
    int line = 0;
    if (!FCheckScript(text, error, line, nullptr)) return false;
    // (the module count must stay: the objects name their module by its index)
    auto modules = [](const std::string& s) {
        static const std::regex re(R"((^|\n)\(module\s+\d+)");
        return int(std::distance(std::sregex_iterator(s.begin(), s.end(), re), std::sregex_iterator()));
    };
    if (modules(text) < modules(m_scriptText)) { error = "modules were removed: objects name their script module by its number"; return false; }
    m_proj.begin("Edit scripts of " + m_index.groups[igroup].name, true);
    m_proj.setPart(m_index.groups[igroup].scriptsPart, std::vector<uint8_t>(text.begin(), text.end()));
    m_proj.end();
    return true;
}

int CEditor::AppendScriptModule(int igroup, const std::string& body, std::string& error)
{
    if (igroup < 0 || igroup >= int(m_index.groups.size()) || m_index.groups[igroup].scriptsPart.empty()) { error = "the group has no scripts"; return -1; }
    std::vector<uint8_t> ab;
    if (!m_proj.readPart(m_index.groups[igroup].scriptsPart, ab)) { error = "cannot read its scripts"; return -1; }
    std::string text(ab.begin(), ab.end());
    static const std::regex re(R"((^|\n)\(module\s+(\d+))");
    int n = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) n = std::max(n, std::atoi((*it)[2].str().c_str()) + 1);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    text += "\n\n(module " + std::to_string(n) + "\n  (" + body + "))\n";
    int line = 0;
    if (!FCheckScript(text, error, line, nullptr)) return -1;
    if (!m_proj.setPart(m_index.groups[igroup].scriptsPart, std::vector<uint8_t>(text.begin(), text.end()))) { error = "cannot write its scripts"; return -1; }
    if (m_igroupScript == igroup) LoadScriptText(igroup);
    return n;
}

namespace
{
int ResizeCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* ps = static_cast<std::string*>(data->UserData);
        ps->resize(std::size_t(data->BufTextLen));
        data->Buf = ps->data();
    }
    return 0;
}
}

void CEditor::DrawScripts()
{
    ImGui::SetNextItemWidth(-1);
    const char* szCur = m_igroupScript >= 0 ? m_index.groups[m_igroupScript].name.c_str() : "Choose a group";
    if (ImGui::BeginCombo("##grp", szCur))
    {
        for (const ce::GroupInfo& g : m_index.groups)
            if (!g.scriptsPart.empty() && ImGui::Selectable(g.name.c_str(), g.index == m_igroupScript)) LoadScriptText(g.index);
        ImGui::EndCombo();
    }
    if (m_igroupScript < 0) { ImGui::TextDisabled("Each group's objects run the modules of its scripts (by number)."); return; }
    const bool fCheck = ImGui::Button("Check");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_fScriptDirty);
    const bool fApply = ImGui::Button("Apply");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) LoadScriptText(m_igroupScript);
    ImGui::EndDisabled();
    std::string textCheck = m_scriptEdit;
#if EDGAME_SLY2
    // the names switch: the text shown with names (display only) or as the converter prints it; an edit carried over
    ImGui::SameLine();
    bool fReadable = m_fScriptReadable;
    if (ImGui::Checkbox(T("Names"), &fReadable))
    {
        std::string text, error;
        if (fReadable) text = ScriptReadable(m_igroupScript, m_scriptEdit, &error);
        else if (!FScriptToRaw(m_scriptEdit, text, error)) text.clear();
        if (!text.empty() && error.empty())
        {
            const bool fDirty = m_fScriptDirty;
            m_fScriptReadable = fReadable;
            const int ig = m_igroupScript;
            LoadScriptText(ig);
            if (fDirty) { m_scriptEdit = text; m_fScriptDirty = true; }
        }
        else m_scriptError = error;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Names for the scripts' numbers (display only: the game keeps the numbers).\n"
                                  "self: the object the handler runs for; on-...: the engine's events; .name: an option;\n"
                                  "$name: an object; <name>: a built-in. The \";=\" lines at the top map each name to its number;\n"
                                  "a new name you write becomes a new symbol."));
    std::vector<std::string> anew;
    if ((fCheck || fApply) && FSpliceTextReadable(m_scriptEdit))
    {
        std::string error;
        if (!FSpliceRaw(m_scriptEdit, textCheck, error, &anew)) { m_scriptError = error; textCheck.clear(); }
    }
#endif
    if ((fCheck || fApply) && !textCheck.empty())
    {
        std::vector<PROBLEM> apb;
        if (!FCheckScript(textCheck, m_scriptError, m_scriptErrorLine, &apb, m_igroupScript)) {}
        else if (!apb.empty()) { m_scriptError = apb.front().text + (apb.size() > 1 ? " (+" + std::to_string(apb.size() - 1) + " more)" : ""); m_scriptErrorLine = 0; }
        else m_scriptError = "ok: compiles back to the game's format";
#if EDGAME_SLY2
        if (m_scriptError.rfind("ok", 0) == 0 && !anew.empty()) m_scriptError += " (new symbols: " + anew.front() + (anew.size() > 1 ? " +" + std::to_string(anew.size() - 1) : std::string()) + ")";
#endif
        if (fApply && m_scriptError.rfind("line ", 0) != 0 && m_scriptError.find("scripts:") == std::string::npos)
        {
            std::string error;
            std::string textApply = m_scriptEdit;
#if EDGAME_SLY2
            // (the converter's own print of what the text compiles to)
            if (!FScriptToRaw(m_scriptEdit, textApply, error)) textApply.clear();
#endif
            if (!textApply.empty() && FApplyScript(m_igroupScript, textApply, error))
            {
                Status("scripts of " + m_index.groups[m_igroupScript].name + " applied");
                const int ig = m_igroupScript;
                Reload(m_plSel ? m_plSel->oid : 0);
                LoadScriptText(ig);
            }
            else m_scriptError = error;
        }
    }
    if (!m_scriptError.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(st::vec(m_scriptError.rfind("ok", 0) == 0 ? st::success : st::danger), "%s", m_scriptError.c_str());
    }
    if (ImGui::InputTextMultiline("##src", m_scriptEdit.data(), m_scriptEdit.capacity() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, ResizeCallback, &m_scriptEdit))
        m_fScriptDirty = m_scriptEdit != m_scriptText;
}

// ---- dialogue

}  // namespace edimpl

#endif
