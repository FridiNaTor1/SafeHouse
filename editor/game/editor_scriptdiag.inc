// The map editor's Scripts tab: diagnostics of the shown text as a compiler gives them (Sly 2: Sly2-Release/splicediag.h;
// included by Sly2-Release/editor_script.cpp after editor_scriptnames.inc). Self-contained: DrawScripts calls
// ScriptDiagnosticsTick / ScriptDiagPanelHeight before its text input and DrawScriptDiagUnderlines / DrawScriptDiagPanel
// after it; LoadScriptText and Check / Apply call RunScriptDiagnostics.
//   - the text is checked a moment after each edit (and on Check / Apply): errors red, warnings yellow, hints grey
//   - squiggles under each problem's place in the text, its message when the mouse is over it
//   - the problems list below the text: severity, line:column, the message; a click scrolls the text to it, "Fix"
//     applies the quick fix (a misspelt name's closest one, a missing ')' ...)
//   - with the world: the objects that exist (the loaded ones, every group's records and the objects inside them, the
//     episode's other places through the installed scripts' own lookups), their classes and the classes running each
//     module (the class's options, splicediag.h)
#include "editor_scriptdiag.h"
#include "splicedump.h"

#include <imgui/imgui_internal.h>

#include <chrono>

#if CANE_EDITOR_3D && EDGAME_SLY2 && EDFEAT_SCRIPTS

namespace edimpl
{


namespace
{
double TNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
ImU32 ColSev(int sev, float a = 1.0f)
{
    return sev == SPLICESEV_Error ? st::u32(st::danger, a) : sev == SPLICESEV_Warning ? st::u32(st::cta, a) : st::u32(st::muted, a);
}
const char* SzSev(int sev) { return sev == SPLICESEV_Error ? "error" : sev == SPLICESEV_Warning ? "warning" : "hint"; }
}  // namespace

std::set<int> OidsLookedUp(const std::string& text);

void CEditor::RunScriptDiagnostics()
{
    if (!m_pscriptdiag) m_pscriptdiag = std::make_shared<SCRIPTDIAG>();
    SCRIPTDIAG& sd = *m_pscriptdiag;
    sd.a.clear();
    sd.textDone = m_scriptEdit;
    sd.textSeen = m_scriptEdit;
    sd.igroupDone = m_igroupScript;
    sd.iselected = -1;
    if (m_igroupScript < 0 || m_scriptEdit.empty()) return;
    // the world's objects and their classes
    if (sd.psw != m_psw.get())
    {
        sd.psw = m_psw.get();
        sd.mpoidcid.clear();
        sd.setoidAll.clear();
        auto note = [&](int oid, int cid) {
            sd.setoidAll.insert(oid);
            const auto [it, fNew] = sd.mpoidcid.emplace(oid, cid);
            if (!fNew && it->second != cid) it->second = -2;
        };
        if (m_psw)
            for (const auto& up : m_psw->aploAll) note(up->oid, up->Cid());
        for (const EDITOBJREC& r : m_rec.aobj) sd.setoidAll.insert(r.oid);
        for (const ce::GroupInfo& g : m_index.groups)
        {
            for (const ce::ObjRecord& r : g.objects) note(r.oid, r.cid);
            // (the objects inside the records: made when an instance is placed)
            std::vector<uint8_t> ab;
            if (!g.objectsPart.empty() && m_proj.readPart(g.objectsPart, ab))
            {
                std::map<int, SPLICECHILDREC> mpc;
                SpliceIndexChildRecords(g.index, ab.data(), ab.size(), mpc);
                for (const auto& [oid, c] : mpc) sd.setoidAll.insert(oid);
            }
        }
    }
    SPLICEDIAGCTX ctx;
    ctx.pfnObjectExists = [&sd](int oid) { return sd.setoidAll.count(oid) != 0; };
    ctx.pfnObjectCid = [&sd](int oid) {
        const auto it = sd.mpoidcid.find(oid);
        return it == sd.mpoidcid.end() || it->second < 0 || it->second == 0x06 || it->second == 0x07 ? -1 : it->second;
    };
    // (an episode's shared scripts look up other places' objects: the oids the installed scripts look up are known)
    const ce::GroupInfo& g = m_index.groups[std::size_t(m_igroupScript)];
    if (!g.scriptsPart.empty())
    {
        std::vector<uint8_t> ab0;
        if (ce::readFile(m_inst.dir / ce::pathOf(g.scriptsPart), ab0)) ctx.setoidKnown = OidsLookedUp(std::string(ab0.begin(), ab0.end()));
    }
    // the classes running each module of the group
    EnsureExistCache();
    std::map<int, std::set<int>> mpmodcid;
    if (m_psw && m_pexist)
        for (const auto& up : m_psw->aploAll)
            if (up->pframe)
                if (const auto it = m_pexist->mpframe.find(up->pframe.get()); it != m_pexist->mpframe.end() && it->second.first == m_igroupScript)
                    mpmodcid[it->second.second].insert(up->Cid());
    ctx.pfnModuleClasses = [&mpmodcid](int imod) {
        std::vector<int> a;
        if (const auto it = mpmodcid.find(imod); it != mpmodcid.end()) a.assign(it->second.begin(), it->second.end());
        return SpliceRuntimeClasses(a);
    };
    SpliceDiagnose(m_scriptEdit, ctx, sd.a);
}

void CEditor::ScriptDiagnosticsTick()
{
    if (!m_pscriptdiag || m_pscriptdiag->igroupDone != m_igroupScript) { RunScriptDiagnostics(); return; }
    SCRIPTDIAG& sd = *m_pscriptdiag;
    const double t = TNow();
    if (sd.textSeen != m_scriptEdit) { sd.textSeen = m_scriptEdit; sd.tEdit = t; }
    // (again a moment after the last edit)
    if (sd.textDone != m_scriptEdit && t - sd.tEdit > 0.4) RunScriptDiagnostics();
}

std::string CEditor::ScriptDiagSummary() const
{
    if (!m_pscriptdiag) return std::string();
    int ce = 0, cw = 0, ch = 0;
    CountSpliceDiags(m_pscriptdiag->a, ce, cw, ch);
    std::string s;
    auto part = [&](int c, const char* sz) {
        if (!c) return;
        s += (s.empty() ? "" : ", ") + std::to_string(c) + " " + sz + (c == 1 ? "" : "s");
    };
    part(ce, "error");
    part(cw, "warning");
    part(ch, "hint");
    return s;
}

float CEditor::ScriptDiagPanelHeight() const
{
    if (!m_pscriptdiag || m_pscriptdiag->a.empty()) return 0.0f;
    const int crow = std::min<int>(int(m_pscriptdiag->a.size()), 6);
    const float h = ImGui::GetTextLineHeightWithSpacing() * float(crow + 1) + ImGui::GetStyle().ItemSpacing.y * 2.0f + 8.0f;
    // (the text keeps at least 60% of the room; the list drawn after the text at this height)
    m_pscriptdiag->hPanel = std::min(h, std::max(ImGui::GetTextLineHeightWithSpacing() * 3.0f, ImGui::GetContentRegionAvail().y * 0.4f));
    return m_pscriptdiag->hPanel;
}

void CEditor::DrawScriptDiagUnderlines()
{
    if (!m_pscriptdiag) return;
    SCRIPTDIAG& sd = *m_pscriptdiag;
    // the text input's child window (imgui's multiline input: "<window>/##src_<id>")
    ImGuiWindow* pwParent = ImGui::GetCurrentWindow();
    const ImGuiID id = ImGui::GetItemID();
    char achName[512];
    ImFormatString(achName, sizeof achName, "%s/%s_%08X", pwParent->Name, "##src", id);
    ImGuiWindow* pw = ImGui::FindWindowByName(achName);
    if (!pw) return;
    const float dyLine = ImGui::GetFontSize();
    if (sd.lineJump > 0)
    {
        ImGui::SetScrollY(pw, std::max(0.0f, float(sd.lineJump - 1) * dyLine - pw->Size.y * 0.3f));
        sd.lineJump = 0;
    }
    if (sd.a.empty() || sd.textDone != m_scriptEdit) return;   // (the squiggles only for the text they are of)
    // the lines' starts
    std::vector<std::size_t> aib{0};
    for (std::size_t i = 0; i < m_scriptEdit.size(); ++i)
        if (m_scriptEdit[i] == '\n') aib.push_back(i + 1);
    // (imgui's multiline input: no window padding, the text at the frame padding)
    const ImVec2 org(pw->DC.CursorStartPos.x + ImGui::GetStyle().FramePadding.x, pw->DC.CursorStartPos.y + ImGui::GetStyle().FramePadding.y);
    ImDrawList* pdl = pw->DrawList;
    pdl->PushClipRect(pw->InnerClipRect.Min, pw->InnerClipRect.Max, true);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool fHoverText = pw->InnerClipRect.Contains(mouse);
    const SPLICEISSUE* phover = nullptr;
    int c = 0;
    for (std::size_t k = 0; k < sd.a.size() && c < 400; ++k, ++c)
    {
        const SPLICEISSUE& d = sd.a[k];
        if (d.line < 1 || d.line > int(aib.size())) continue;
        const int line1 = std::min(std::max(d.lineEnd, d.line), int(aib.size()));
        for (int line = d.line; line <= line1; ++line)
        {
            const std::size_t ib = aib[std::size_t(line - 1)];
            const std::size_t ibEnd = line < int(aib.size()) ? aib[std::size_t(line)] - 1 : m_scriptEdit.size();
            const std::size_t c0 = line == d.line ? std::size_t(std::max(d.col, 1) - 1) : 0;
            std::size_t c1 = line == line1 ? std::size_t(std::max(d.colEnd, d.col)) : ibEnd - ib;
            if (ib + c0 > ibEnd) continue;
            c1 = std::min(c1, ibEnd - ib);
            const char* p = m_scriptEdit.c_str() + ib;
            // (a line's leading spaces are not underlined)
            std::size_t c0b = c0;
            if (line != d.line) while (c0b < c1 && p[c0b] == ' ') ++c0b;
            const float x0 = org.x + ImGui::CalcTextSize(p, p + c0b).x;
            const float x1 = std::max(x0 + 4.0f, org.x + ImGui::CalcTextSize(p, p + c1).x);
            const float y = org.y + float(line - 1) * dyLine + dyLine - 1.0f;
            if (y < pw->InnerClipRect.Min.y - dyLine || y > pw->InnerClipRect.Max.y + dyLine) continue;
            // the squiggle
            const ImU32 col = ColSev(d.sev, d.sev == SPLICESEV_Hint ? 0.7f : 1.0f);
            for (float x = x0; x < x1; x += 4.0f)
                pdl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 2.0f, x1), y - 2.0f), col, 1.2f), pdl->AddLine(ImVec2(std::min(x + 2.0f, x1), y - 2.0f), ImVec2(std::min(x + 4.0f, x1), y), col, 1.2f);
            if (int(k) == sd.iselected) pdl->AddRectFilled(ImVec2(x0, y - dyLine + 1.0f), ImVec2(x1, y), ColSev(d.sev, 0.18f));
            if (fHoverText && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y - dyLine && mouse.y <= y + 2.0f && (!phover || d.sev < phover->sev)) phover = &d;
        }
    }
    pdl->PopClipRect();
    if (phover)
    {
        ImGui::BeginTooltip();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ColSev(phover->sev)));
        ImGui::TextUnformatted(SzSev(phover->sev));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        ImGui::TextUnformatted(phover->message.c_str());
        if (!phover->fixLabel.empty()) ImGui::TextDisabled("%s: %s", T("Quick fix in the problems list"), phover->fixLabel.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CEditor::DrawScriptDiagPanel()
{
    if (!m_pscriptdiag || m_pscriptdiag->a.empty()) return;
    SCRIPTDIAG& sd = *m_pscriptdiag;
    const float h = sd.hPanel - ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##scriptdiag", ImVec2(-1, h), true);
    ImGui::TextColored(st::vec(st::text2), "%s: %s%s", T("Problems"), ScriptDiagSummary().c_str(),
                       sd.textDone != m_scriptEdit ? "  (checking...)" : "");
    int iFix = -1;
    for (std::size_t k = 0; k < sd.a.size() && k < 500; ++k)
    {
        const SPLICEISSUE& d = sd.a[k];
        ImGui::PushID(int(k));
        if (!d.fixLabel.empty())
        {
            if (ImGui::SmallButton(T("Fix"))) iFix = int(k);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.fixLabel.c_str());
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ColSev(d.sev)));
        char ach[64];
        std::snprintf(ach, sizeof ach, "%-7s %4d:%-3d", SzSev(d.sev), d.line, d.col);
        const std::string row = std::string(ach) + "  " + d.message + "  [" + d.code + "]";
        if (ImGui::Selectable(row.c_str(), sd.iselected == int(k)))
        {
            sd.iselected = int(k);
            sd.lineJump = d.line;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (iFix >= 0)
    {
        std::string text = m_scriptEdit;
        if (FApplySpliceFix(sd.a[std::size_t(iFix)], text))
        {
            m_scriptEdit = text;
            m_fScriptDirty = m_scriptEdit != m_scriptText;
            RunScriptDiagnostics();
        }
    }
}

}  // namespace edimpl

#endif
