// The map editor's guards (notes/EDITOR_PLAN.md step 6): a guard's patrol (its command list, cid 0x71: walk a path,
// wait, look, go to, animations ...; ai.h CMD) in words in its Properties, the patrol's paths drawn in the view in the
// guard's colour with their direction, each path / target a click away (the curve tools move its nodes), the command
// list's stored values one click away; guards added (a copy of a placed one: same kind, same patrol) and removed with
// the structural tools. Sly 2 (the port's GUARD / CMDLIST); other games: not yet.
#if EDGAME_SLY2
#include "ai.h"
#include "guard.h"
#endif

namespace edimpl
{

#if EDGAME_SLY2
namespace
{
}  // namespace

// the live guard's patrol, else its option 273 (the list's oid, looked up in the guard's own instance first, as the
// loader's 0x13E590 does)
const CMDLIST* CEditor::PcmdlistOfGuard(LO* plo) const
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x38)) return nullptr;
    if (GUARD* pg = PguardFromLo(plo); pg && pg->pcmdlistPatrol) return pg->pcmdlistPatrol;
    int oid = 0;
    if (!FOptionInt(plo, 273, oid) || oid <= 0) return nullptr;
    const CMDLIST* pbest = nullptr;
    for (const auto& up : m_psw->aploAll)
        if (up->oid == oid && up->Cid() == 0x71)
        {
            if (up->oidInstance == plo->oidInstance) return static_cast<const CMDLIST*>(up.get());
            if (!pbest) pbest = static_cast<const CMDLIST*>(up.get());
        }
    return pbest;
}
#endif

// the command's words ("Walk along path 1234", "Wait 2.0 s", ...); *pploRef: the object it names
std::string CEditor::StrCommand(const void* pv, LO** pploRef) const
{
    *pploRef = nullptr;
#if EDGAME_SLY2
    const CMD& c = *static_cast<const CMD*>(pv);
    LO* pref = c.oid > 0 && c.op != 4 && c.op != 9 && c.op != 10 ? PloOid(c.oid) : nullptr;
    *pploRef = pref;
    const std::string what = pref ? NameOfObject(pref) : std::to_string(c.oid);
    char sz[200];
    // (aijobs.cpp's command interpreter, orig 0x1715F0)
    switch (c.op)
    {
    case 0: std::snprintf(sz, sizeof(sz), "%s %s", T("Near"), what.c_str()); break;
    case 1: std::snprintf(sz, sizeof(sz), "%s %.1f..%.1f s", T("Waits at stops"), c.g8, c.gC); break;
    case 2: std::snprintf(sz, sizeof(sz), "%s %.1f..%.1f s", T("Waits between looks"), c.g8, c.gC); break;
    case 3: std::snprintf(sz, sizeof(sz), "%s %s", T("Stand and face as"), what.c_str()); break;
    case 4: std::snprintf(sz, sizeof(sz), "%s %d", T("Script event"), c.oid); break;
    case 5: std::snprintf(sz, sizeof(sz), "%s", T("Stand still (idle)")); break;
    case 6: std::snprintf(sz, sizeof(sz), "%s %s", T("Walk along"), what.c_str()); break;
    case 7:
        if (c.oid <= 0 && c.ppos8) std::snprintf(sz, sizeof(sz), "%s %.0f, %.0f, %.0f", T("Go to"), c.ppos8->x, c.ppos8->y, c.ppos8->z);
        else std::snprintf(sz, sizeof(sz), "%s %s", T("Go to"), what.c_str());
        break;
    case 8: std::snprintf(sz, sizeof(sz), "%s %d", T("Sound"), c.n8); break;
    case 9: std::snprintf(sz, sizeof(sz), "%s %d:", T("Label"), c.oid); break;
    case 10: std::snprintf(sz, sizeof(sz), "%s %d (%s)", T("Go back to label"), c.oid, T("loop")); break;
    case 11: std::snprintf(sz, sizeof(sz), "%s", T("Lock")); break;
    case 12: std::snprintf(sz, sizeof(sz), "%s", T("Unlock")); break;
    case 13: std::snprintf(sz, sizeof(sz), "%s %s", T("Use"), what.c_str()); break;
    case 14: std::snprintf(sz, sizeof(sz), "%s %s", T("Animation"), what.c_str()); break;
    case 15: std::snprintf(sz, sizeof(sz), "%s", T("Raise the alarm")); break;
    default: std::snprintf(sz, sizeof(sz), "%s %d %s", T("Command"), c.op, what.c_str()); break;
    }
    return sz;
#else
    (void)pv;
    return std::string();
#endif
}

LO* CEditor::PatrolOf(LO* plo) const
{
#if EDGAME_SLY2
    return const_cast<CMDLIST*>(PcmdlistOfGuard(plo));
#else
    (void)plo;
    return nullptr;
#endif
}

std::vector<LO*> CEditor::PatrolPaths(LO* plo) const
{
    std::vector<LO*> a;
#if EDGAME_SLY2
    if (const CMDLIST* pl = PcmdlistOfGuard(plo))
        for (const auto& up : pl->acmd)
            if ((up->op == 6 || up->op == 7) && up->oid > 0)
                if (LO* p = PloOid(up->oid); p && m_mpobj.count(p) && m_mpobj.at(p).ipath >= 0 && std::find(a.begin(), a.end(), p) == a.end()) a.push_back(p);
#else
    (void)plo;
#endif
    return a;
}

std::vector<std::string> CEditor::PatrolWords(LO* plo) const
{
    std::vector<std::string> a;
#if EDGAME_SLY2
    if (const CMDLIST* pl = PcmdlistOfGuard(plo))
        for (const auto& up : pl->acmd) { LO* pr = nullptr; a.push_back(StrCommand(up.get(), &pr)); }
#else
    (void)plo;
#endif
    return a;
}

// a curve drawn in a colour (its direction: arrows along it)
void CEditor::DrawCurve(LO* plo, uint32_t rgb, float thick)
{
    PATHDATA pd;
    if (!FPathLive(plo, pd) || pd.apos.size() < 2) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const glm::mat4 toWorld = MatPathSpace(plo);
    std::vector<glm::vec3> ap;
    if (const CRV* pcrv = PcrvFromLo(plo); pcrv && pcrv->ag.size() >= 2)
    {
        const float g0 = pcrv->ag.front(), g1 = pcrv->ag.back();
        const int n = std::clamp(int(pd.apos.size()) * (pd.crvk ? 10 : 1), 2, 600);
        for (int i = 0; i <= n; ++i)
        {
            glm::vec3 p;
            EvalCrv(pcrv, g0 + (g1 - g0) * float(i) / float(n), &p, nullptr);
            ap.push_back(p);
        }
    }
    else
        for (const glm::vec3& q : pd.apos) ap.push_back(glm::vec3(toWorld * glm::vec4(q, 1.0f)));
    if (pd.fClosed) ap.push_back(ap.front());
    const ImU32 col = st::u32(rgb, 0.95f);
    for (std::size_t i = 1; i < ap.size(); ++i)
    {
        ImVec2 a, b;
        if (!FProject(ap[i - 1], a) || !FProject(ap[i], b)) continue;
        dl->AddLine(a, b, col, thick);
        // (an arrow head now and then: the way it is walked)
        if (i % std::max<std::size_t>(1, ap.size() / 6) == 0)
        {
            const float dx = b.x - a.x, dy = b.y - a.y, l = std::max(1.0f, std::hypot(dx, dy));
            const ImVec2 u(dx / l, dy / l), m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            dl->AddTriangleFilled(ImVec2(m.x + u.x * 7, m.y + u.y * 7), ImVec2(m.x - u.x * 5 - u.y * 5, m.y - u.y * 5 + u.x * 5),
                                  ImVec2(m.x - u.x * 5 + u.y * 5, m.y - u.y * 5 - u.x * 5), col);
        }
    }
}

// the selected guards' patrols (or every guard's, View > Patrols: all) in the view
void CEditor::DrawPatrols()
{
    std::vector<LO*> aguard;
    if (m_fAllPatrols)
    {
        for (const auto& up : m_psw->aploAll)
            if (const OBJTYPE ot = TypeOfLo(up.get()); (ot == OT_Guard || ot == OT_Boss || ot == OT_Swarmer || ot == OT_Npc) && !FHidden(up.get())) aguard.push_back(up.get());
    }
    else
        for (LO* p : m_asel)
            if (PatrolOf(p)) aguard.push_back(p);
    for (LO* pg : aguard)
    {
        const std::vector<LO*> apath = PatrolPaths(pg);
        if (apath.empty()) continue;
        const bool fSel = FSelected(pg);
        for (LO* pp : apath) DrawCurve(pp, fSel ? 0xFF6B3A : 0xF0506E, fSel ? 3.0f : 1.5f);
        // the guard to its first path's start
        PATHDATA pd;
        ImVec2 a, b;
        if (fSel && FHasMatrix(pg) && FPathLive(apath[0], pd) && !pd.apos.empty() && FProject(PosWorld(pg), a) &&
            FProject(glm::vec3(MatPathSpace(apath[0]) * glm::vec4(pd.apos[0], 1.0f)), b))
            ImGui::GetBackgroundDrawList()->AddLine(a, b, st::u32(0xFF6B3A, 0.5f), 1.0f);
    }
}

void CEditor::DrawGuardSection(LO* plo)
{
    const OBJTYPE ot = TypeOfLo(plo);
    if (ot != OT_Guard && ot != OT_Boss && ot != OT_Swarmer && ot != OT_Npc) return;
    if (!ImGui::CollapsingHeader(TL("Patrol", "patrol").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
#if EDGAME_SLY2
    const CMDLIST* pl = PcmdlistOfGuard(plo);
    if (!pl) { ImGui::TextDisabled("%s", T("(no patrol: it stands, or it is given one at run time by a spawner or a script)")); return; }
    ImGui::TextColored(st::vec(st::muted), "%s %d, %zu %s", T("Command list"), pl->oid, pl->acmd.size(), T("commands, run in a loop"));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Edit its values"))) SelectOnly(const_cast<CMDLIST*>(pl));
    EdTip(T("Selects the command list: its stored values (waits, objects) are in its Raw fields"));
    int i = 0;
    for (const auto& up : pl->acmd)
    {
        LO* pref = nullptr;
        const std::string s = StrCommand(up.get(), &pref);
        ImGui::PushID(i);
        ImGui::TextColored(st::vec(st::muted), "%2d", ++i);
        ImGui::SameLine();
        if (pref)
        {
            if (ImGui::Selectable(s.c_str(), false, 0, ImVec2(0, 0)))
            {
                SelectOnly(pref);
                if (m_mpobj.count(pref) && m_mpobj.at(pref).ipath >= 0) { m_fCurveMode = true; m_ipoint = -1; }
                FrameSelected();
            }
            EdTip(T("Select it (a path: its nodes are moved with the gizmo; click a node to move only it)"));
        }
        else ImGui::TextUnformatted(s.c_str());
        ImGui::PopID();
    }
    ImGui::Checkbox(T("Show every guard's patrol"), &m_fAllPatrols);
#else
    ImGui::TextDisabled("(patrols: not for " EDGAME_NAME " yet)");
#endif
}

}  // namespace edimpl
