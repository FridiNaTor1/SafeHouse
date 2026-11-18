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

// ---- the splice that starts a cutscene (the cutscene editor's starter, Sly 2)
//
// "Start the cutscene when task T becomes state S": the task's own script module (its group's scripts, readable) gets
// (if (<=> (<geto> self .state) S) ((<geto> (<find-object> $cutscene-OID #obj:0) .trigger) #obj:0)) in its
// on-task-start (made when it has none), as the game's own tasks start their scenes (Paris 1888 -> 1907); the object's
// name tied to its oid by a ";=" line; the text checked by the diagnostics first (errors: nothing written) and kept
// readable (FSpliceNormalize), as the Scripts tab's Apply with Names.
namespace
{
// the index past the form that opens at i (a "(": strings and ; comments skipped); npos: unbalanced
std::size_t IbFormEnd(const std::string& t, std::size_t i)
{
    int d = 0;
    for (; i < t.size(); ++i)
    {
        const char c = t[i];
        if (c == ';') { while (i < t.size() && t[i] != '\n') ++i; continue; }
        if (c == '"') { for (++i; i < t.size() && t[i] != '"'; ++i) if (t[i] == '\\') ++i; continue; }
        if (c == '(') ++d;
        else if (c == ')' && --d == 0) return i + 1;
    }
    return std::string::npos;
}
}  // namespace

bool CEditor::FWriteSceneStarter(int oidScene, LO* ptask, int state, std::vector<std::string>& adiag, std::string& error)
{
    if (!ptask) { error = T("the task has no script of its own to start it from"); return false; }
    const std::string name = "$cutscene-" + std::to_string(oidScene);
    const std::string form = "(if (<=> (<geto> %self .state) " + std::to_string(state) + ") ((<geto> (<find-object> " + name + " #obj:0) .trigger) #obj:0))";
    return FWriteObjectHandler(ptask, "on-task-start", {"self"}, form, {{name, oidScene}}, adiag, error, false);
}

// An object's handler (on-task-start, on-activate, on-event ...) given a form (%self / %1: its parameters as the
// handler names them): appended to the handler's body, else a new handler in the module's begin; the object's module
// made when it has none (fMakeModule: a new module in its record's group, the record's module number set to it); the
// names tied to their oids by ";=" lines; checked by the diagnostics first (errors: nothing written), written readable.
bool CEditor::FWriteObjectHandler(LO* pobj, const std::string& handler, const std::vector<std::string>& aparam, const std::string& form0,
                                  const std::vector<std::pair<std::string, int>>& anames, std::vector<std::string>& adiag, std::string& error, bool fMakeModule)
{
    adiag.clear();
    if (!pobj) { error = T("no object to start it from"); return false; }
    int igroup = -1, imodule = -1;
    const std::string label = "Script of " + std::to_string(pobj->oid) + ": " + handler;
    m_proj.begin(label, true);   // (one undo step: the module made, its number, the text)
    auto fail = [&](const std::string& why) {
        error = why;
        m_proj.end();
        if (m_proj.canUndo() && m_proj.history().back().label == label) m_proj.undo();
        return false;
    };
    if (pobj->pframe)
    {
        EnsureExistCache();
        const auto it = m_pexist ? m_pexist->mpframe.find(pobj->pframe.get()) : decltype(m_pexist->mpframe.end()){};
        if (!m_pexist || it == m_pexist->mpframe.end()) return fail(T("the object's script module was not found"));
        igroup = it->second.first, imodule = it->second.second;
    }
    else
    {
        if (!fMakeModule) return fail(T("the task has no script of its own to start it from"));
        // (a module of its own: in the group whose Objects part holds its record)
        int iobj = -1;
        for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
            if (m_rec.aobj[i].plo == pobj) { iobj = int(i); break; }
        std::string rel;
        std::size_t ib = 0, cb = 0;
        if (iobj < 0 || !FRecordRange(iobj, rel, ib, cb)) return fail(T("the object's record was not found"));
        for (const ce::GroupInfo& g : m_index.groups)
            if (g.objectsPart == rel) igroup = g.index;
        if (igroup < 0) return fail(T("the object's record was not found"));
        imodule = AppendScriptModule(igroup, "(begin)", error);
        if (imodule < 0) return fail(error);
        const std::vector<uint8_t>* pab = m_proj.partRef(rel);
        if (!pab || ib + 16 > pab->size()) return fail(T("could not write"));
        const int16_t n = int16_t(imodule);
        if (!m_proj.patch(rel, ib + IbOidOfRecord(pab->data() + ib) + 4, &n, 2)) return fail(T("could not write"));
        adiag.push_back(std::string(T("a script module of its own made:")) + " " + std::to_string(imodule));
    }
    const bool fReadableWas = m_fScriptReadable;
    m_fScriptReadable = true;
    LoadScriptText(igroup);
    m_fScriptReadable = fReadableWas;
    std::string text = m_scriptEdit;
    // the module's form
    const std::string head = "(module " + std::to_string(imodule);
    std::size_t im = std::string::npos;
    for (std::size_t p = text.find(head); p != std::string::npos; p = text.find(head, p + 1))
        if ((p == 0 || text[p - 1] == '\n') && p + head.size() < text.size() && (text[p + head.size()] == '\n' || text[p + head.size()] == ' ' || text[p + head.size()] == ')'))
        {
            im = p;
            break;
        }
    if (im == std::string::npos) return fail(T("the object's script module was not found in its group's text"));
    const std::size_t imEnd = IbFormEnd(text, im);
    if (imEnd == std::string::npos) return fail(T("its scripts do not read"));
    auto withParams = [&](const std::vector<std::string>& an) {
        std::string f = form0;
        for (std::size_t k = 0; k < an.size(); ++k)
        {
            const std::string key = k == 0 ? "%self" : "%" + std::to_string(k);
            for (std::size_t q = f.find(key); q != std::string::npos; q = f.find(key, q + an[k].size())) f.replace(q, key.size(), an[k]);
        }
        return f;
    };
    // (its handler: the form added at the end of its body; else a new handler in the module's begin)
    const std::string def = "(define " + handler;
    std::size_t idef = std::string::npos;
    for (std::size_t q = text.find(def, im); q != std::string::npos && q < imEnd; q = text.find(def, q + 1))
        if (q + def.size() < text.size() && (text[q + def.size()] == ' ' || text[q + def.size()] == '\n')) { idef = q; break; }
    if (idef != std::string::npos)
    {
        const std::size_t il = text.find("(lambda", idef);
        const std::size_t ilEnd = il == std::string::npos ? std::string::npos : IbFormEnd(text, il);
        if (ilEnd == std::string::npos || ilEnd > imEnd) return fail(std::string(T("its handler does not read:")) + " " + handler);
        // (the handler's parameters as it names them)
        std::vector<std::string> an;
        const std::size_t ip0 = text.find('(', il + 1), ip1 = ip0 == std::string::npos ? std::string::npos : text.find(')', ip0);
        if (ip0 != std::string::npos && ip1 != std::string::npos)
        {
            std::stringstream ss(text.substr(ip0 + 1, ip1 - ip0 - 1));
            for (std::string w; ss >> w;) an.push_back(w);
        }
        for (std::size_t k = an.size(); k < aparam.size(); ++k) an.push_back(aparam[k]);
        text.insert(ilEnd - 1, "\n        " + withParams(an));
    }
    else
    {
        const std::size_t ib = text.find("(begin", im);
        if (ib == std::string::npos || ib > imEnd) return fail(T("the object's module has no begin"));
        std::string ps;
        for (const std::string& a : aparam) ps += (ps.empty() ? "" : " ") + a;
        text.insert(ib + 6, "\n    (define " + handler + " (lambda (" + ps + ") " + withParams(aparam) + "))");
    }
    // (the new object names tied to their oids)
    while (!text.empty() && text.back() == '\n') text.pop_back();
    for (const auto& [name, oid] : anames)
        if (text.find(";= " + name + "=") == std::string::npos) text += "\n;= " + name + "=" + std::to_string(oid);
    text += "\n";
    // checked first
    m_scriptEdit = text;
    RunScriptDiagnostics();
    int cerr = 0;
    if (m_pscriptdiag)
        for (const SPLICEISSUE& d : m_pscriptdiag->a)
        {
            // (the written lines' own notes only: the group's other modules have the game's own warnings)
            if (d.sev == SPLICESEV_Error) ++cerr;
            bool fOurs = d.sev == SPLICESEV_Error;
            for (const auto& [name, oid] : anames) fOurs = fOurs || d.message.find(name) != std::string::npos;
            if (fOurs) adiag.push_back(StrSpliceDiag(d));
        }
    if (cerr) { LoadScriptText(igroup); return fail(T("the starter's script has errors (nothing written)")); }
    std::string raw;
    std::vector<std::string> anew;
    if (!FSpliceNormalize(text, raw, error, &anew) || !FApplyScript(igroup, raw, error)) { LoadScriptText(igroup); return fail(error); }
    for (const std::string& n : anew) adiag.push_back(std::string(T("new name")) + " " + n);
    std::printf("editor: handler %s of %d written: group %d module %d, %zu notes\n", handler.c_str(), pobj->oid, igroup, imodule, adiag.size());
    m_proj.end();
    Reload(m_plSel ? m_plSel->oid : 0);
    LoadScriptText(igroup);
    return true;
}

}  // namespace edimpl

#endif
