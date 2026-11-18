// The map editor (editor_impl.h, shared: editor/game/editor_path.inc): curves of the PATH family (cids 0x0A..0x0E: paths for AI / lasers / emitters / camera
// zones, pipes and ladders, ledges, rails, nav polygons; ropes are paths with rope options). Points are stored in the
// path's own space (its LOX matrix under its transform reference). Edits patch the points (and cubic tangents) in the
// part, move the live LOXPATH vectors and refresh its cached curve; what the game derives from a path at its start
// (nav polygons, ropes, laser timings, camera zones, a guard's place on it) is built again after a reload or in a play
// test. Shared by Sly 2 and Sly 3 (the same curve records; class numbers by EDCID).
#include "editor_impl.h"

#if CANE_EDITOR_3D && EDFEAT_CURVES

#include "physworld.h"

namespace edimpl
{

bool CEditor::FPathLive(const LO* plo, PATHDATA& pd) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end() || it->second.ipath < 0 || !FIsLoDerivedFrom(plo, EDCID(0x0A))) return false;
    const LOXPATH* p = static_cast<const LOXPATH*>(plo);
    const EDITFIELD& f = m_rec.afield[it->second.ipath];
    if (p->avecF4.size() != std::size_t(f.i)) return FPathRead(plo, pd);
    pd.crvk = f.k;
    pd.fClosed = f.eopid != 0;
    pd.apos = p->avecF4;
    pd.avIn = f.k ? p->avec140 : std::vector<glm::vec3>();
    pd.avOut = f.k ? p->avec144 : std::vector<glm::vec3>();
    return true;
}

bool CEditor::FPathRead(const LO* plo, PATHDATA& pd) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end() || it->second.ipath < 0) return false;
    const EDITFIELD& f = m_rec.afield[it->second.ipath];
    const std::size_t c = std::size_t(f.i);
    const std::size_t cb = 4 + c * 16 + (f.k ? c * 24 : 0);
    const std::vector<uint8_t>* pab = nullptr;
    if (!FLocate(f.ib, cb, pd.rel, pd.ib) || !(pab = m_proj.partRef(pd.rel)) || pd.ib + cb > pab->size()) return false;
    const uint8_t* pb = pab->data() + pd.ib;
    pd.crvk = int8_t(pb[0]);
    pd.fClosed = pb[1] != 0;
    uint16_t cStored;
    std::memcpy(&cStored, pb + 2, 2);
    if (cStored != c) return false;
    pd.ag.resize(c);
    std::memcpy(pd.ag.data(), pb + 4, c * 4);
    auto vecs = [&](std::size_t ib, std::vector<glm::vec3>& a) {
        a.resize(c);
        for (std::size_t i = 0; i < c; ++i) std::memcpy(&a[i], pb + ib + 12 * i, 12);
    };
    vecs(4 + 4 * c, pd.apos);
    pd.avIn.clear();
    pd.avOut.clear();
    if (pd.crvk)
    {
        vecs(4 + 16 * c, pd.avIn);
        vecs(4 + 28 * c, pd.avOut);
    }
    return true;
}

void CEditor::PathLive(LO* plo, const PATHDATA& pd)
{
    if (!FIsLoDerivedFrom(plo, EDCID(0x0A))) return;
    LOXPATH* p = static_cast<LOXPATH*>(plo);
    p->avecF4 = pd.apos;
    if (pd.crvk)
    {
        p->avec140 = pd.avIn;
        p->avec144 = pd.avOut;
    }
#if EDGAME_SLY3
    InvalidateCrvOfLo(plo);   // (rebuilt from the points at its next use)
#else
    RefreshCrvOfLo(plo);
#endif
}

bool CEditor::FPathWrite(LO* plo, const PATHDATA& pd, const std::string& label)
{
    PATHDATA cur;
    if (!FPathRead(plo, cur) || cur.apos.size() != pd.apos.size()) { Status(T("this path is not stored in a binary part")); return false; }
    // atomic: every number finite, or nothing is written
    auto finite = [](const std::vector<glm::vec3>& a) {
        for (const glm::vec3& v : a)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || std::fabs(v.x) > 1e7f || std::fabs(v.y) > 1e7f || std::fabs(v.z) > 1e7f) return false;
        return true;
    };
    if (!finite(pd.apos) || !finite(pd.avIn) || !finite(pd.avOut)) { Status(T("rejected: a point has an invalid number")); PathLive(plo, cur); return false; }
    const std::size_t c = pd.apos.size();
    std::vector<uint8_t> ab(c * 12 + (pd.crvk ? c * 24 : 0));
    std::memcpy(ab.data(), pd.apos.data(), c * 12);
    if (pd.crvk)
    {
        std::memcpy(ab.data() + c * 12, pd.avIn.data(), c * 12);
        std::memcpy(ab.data() + c * 24, pd.avOut.data(), c * 12);
    }
    m_proj.begin(label);
    const bool fOk = m_proj.patch(cur.rel, cur.ib + 4 + 4 * c, ab.data(), ab.size());
    m_proj.end();
    if (fOk) PathLive(plo, pd);
    return fOk;
}

glm::mat4 CEditor::MatPathSpace(const LO* plo) const { return MatWorld(plo); }

glm::vec3 CEditor::PathPivot(const LO* plo, const PATHDATA& pd) const
{
    (void)plo;
    if (pd.apos.empty()) return glm::vec3(0.0f);
    if (m_pivot == 1) return pd.apos.front();
    if (m_pivot == 2) return glm::vec3(0.0f);
    if (m_pivot == 3 && m_ipoint >= 0 && m_ipoint < int(pd.apos.size())) return pd.apos[std::size_t(m_ipoint)];
    glm::vec3 lo = pd.apos[0], hi = pd.apos[0];
    for (const glm::vec3& p : pd.apos) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    return (lo + hi) * 0.5f;
}

bool CEditor::FPathTransform(LO* plo, const glm::mat4& mWorld, const std::string& label)
{
    // mWorld: a change in world space (about the world origin) applied to every point; tangents by its 3 x 3
    PATHDATA pd;
    if (!FPathRead(plo, pd)) return false;
    const glm::mat4 toWorld = MatPathSpace(plo), toPath = glm::inverse(toWorld);
    const glm::mat4 m = toPath * mWorld * toWorld;
    const glm::mat3 r(m);
    for (glm::vec3& p : pd.apos) p = glm::vec3(m * glm::vec4(p, 1.0f));
    for (glm::vec3& v : pd.avIn) v = r * v;
    for (glm::vec3& v : pd.avOut) v = r * v;
    return FPathWrite(plo, pd, label);
}

bool CEditor::FSnapDown(const glm::vec3& w, glm::vec3& hit)
{
    if (!m_fPhysReady)
    {
        // the level's own collision (class 0x31's entries, pcrc.cpp ApsoeLevel), their world geometry made current
        RefreshSwPhysWorldNow(m_psw.get());
        m_apsoeLevel.clear();
        for (LO* plo : m_psw->mpcidaplo[EDCID(0x31)])
            for (SOE& soe : static_cast<ALO*>(plo)->asoe)
                if (!soe.f48) m_apsoeLevel.push_back(&soe);
        m_fPhysReady = true;
    }
    LSG lsg{};
    const glm::vec3 a = w + glm::vec3(0, 0, 150.0f), b = w - glm::vec3(0, 0, 6000.0f);
    if (!PsoeHitTestLineObjects(0, a, b, m_apsoeLevel, &lsg)) return false;
    hit = a + (b - a) * lsg.t0;
    return std::isfinite(hit.x) && std::isfinite(hit.y) && std::isfinite(hit.z);
}

bool CEditor::FPathSnap(LO* plo, int ipoint, const std::string& label)
{
    PATHDATA pd;
    if (!FPathRead(plo, pd)) return false;
    const glm::mat4 toWorld = MatPathSpace(plo), toPath = glm::inverse(toWorld);
    int c = 0;
    for (int i = 0; i < int(pd.apos.size()); ++i)
    {
        if (ipoint >= 0 && i != ipoint) continue;
        glm::vec3 hit;
        if (!FSnapDown(glm::vec3(toWorld * glm::vec4(pd.apos[std::size_t(i)], 1.0f)), hit)) continue;
        pd.apos[std::size_t(i)] = glm::vec3(toPath * glm::vec4(hit, 1.0f));
        ++c;
    }
    if (!c) { Status("snap: no collision below"); return false; }
    const bool fOk = FPathWrite(plo, pd, label);
    if (fOk) Status(Fmt(T("snapped %d point(s) onto the collision below"), c));
    return fOk;
}

void CEditor::ReapplyPathsFromParts()
{
    for (auto& [plo, o] : m_mpobj)
    {
        if (o.ipath < 0) continue;
        PATHDATA pd;
        if (FPathRead(plo, pd) && FIsLoDerivedFrom(plo, EDCID(0x0A)) && static_cast<const LOXPATH*>(plo)->avecF4 != pd.apos) PathLive(const_cast<LO*>(plo), pd);
    }
}

void CEditor::DrawPaths()
{
    if (!m_fShowPaths) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto draw = [&](LO* plo, bool fSel) {
        PATHDATA pd;
        if (!FPathLive(plo, pd) || pd.apos.size() < 2) return;
        const glm::mat4 toWorld = MatPathSpace(plo);
        const ImU32 col = fSel ? st::u32(st::cta) : st::u32(plo->Cid() == EDCID(0x0A) ? st::accent : st::text2, 0.45f);
        // the curve as the game evaluates it (its cached CRV), else straight between the points
        const CRV* pcrv = PcrvFromLo(plo);
        ImVec2 sPrev;
        bool fPrev = false;
        if (pcrv && pcrv->ag.size() >= 2)
        {
            const float g0 = pcrv->ag.front(), g1 = pcrv->ag.back();
            const int n = std::clamp(int(pd.apos.size()) * (pd.crvk ? 10 : 1), 2, 600);
            for (int i = 0; i <= n; ++i)
            {
                glm::vec3 p;
                EvalCrv(pcrv, g0 + (g1 - g0) * float(i) / float(n), &p, nullptr);
                ImVec2 s;
                const bool f = FProject(p, s);
                if (f && fPrev) dl->AddLine(sPrev, s, col, fSel ? 3.0f : 1.5f);
                sPrev = s;
                fPrev = f;
            }
        }
        else
            for (std::size_t i = 0; i < pd.apos.size(); ++i)
            {
                ImVec2 s;
                const bool f = FProject(glm::vec3(toWorld * glm::vec4(pd.apos[i], 1.0f)), s);
                if (f && fPrev) dl->AddLine(sPrev, s, col, fSel ? 3.0f : 1.5f);
                sPrev = s;
                fPrev = f;
            }
        if (pd.fClosed)
        {
            ImVec2 a, b;
            if (FProject(glm::vec3(toWorld * glm::vec4(pd.apos.back(), 1.0f)), a) && FProject(glm::vec3(toWorld * glm::vec4(pd.apos.front(), 1.0f)), b))
                dl->AddLine(a, b, col, fSel ? 3.0f : 1.5f);
        }
        if (!fSel) return;
        // the points: click picks one (the gizmo then moves it)
        for (std::size_t i = 0; i < pd.apos.size(); ++i)
        {
            ImVec2 s;
            if (!FProject(glm::vec3(toWorld * glm::vec4(pd.apos[i], 1.0f)), s)) continue;
            const bool fPt = int(i) == m_ipoint;
            const bool fHover = std::hypot(mouse.x - s.x, mouse.y - s.y) < 8.0f;
            dl->AddCircleFilled(s, fPt ? 7.0f : 5.0f, st::u32(fPt ? st::cta : st::text, fHover ? 1.0f : 0.85f));
            dl->AddCircle(s, fPt ? 7.0f : 5.0f, st::u32(st::bg), 0, 1.5f);
            if (fHover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse && m_axisDrag < 0) { m_ipoint = int(i); m_fCurveMode = true; }
            if (fPt || fHover)
            {
                char label[32];
                std::snprintf(label, sizeof(label), "%zu", i);
                dl->AddText(ImVec2(s.x + 8, s.y - 16), st::u32(st::text), label);
            }
        }
    };
    if (m_fShowAllPaths)
        for (const auto& [plo, o] : m_mpobj)
            if (o.ipath >= 0 && plo != m_plSel && glm::length(PosWorld(plo) - m_cam.pos) < 6000.0f) draw(const_cast<LO*>(plo), false);
    if (m_plSel) draw(m_plSel, true);
}

void CEditor::DrawPathInspector(LO* plo)
{
    PATHDATA pd;
    if (!FPathLive(plo, pd)) return;
    ImGui::SeparatorText(T("Curve"));
    ImGui::TextColored(st::vec(st::muted), T("%zu points, %s%s"), pd.apos.size(), pd.crvk ? T("cubic") : T("linear"), pd.fClosed ? T(", closed") : "");
    ImGui::Checkbox(T("Edit points"), &m_fCurveMode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The gizmo moves the curve's points (the selected one, or the whole curve about the pivot), not the object"));
    const char* apiv[] = {T("Curve middle"), T("First point"), T("Object origin"), T("Selected point")};
    ImGui::SetNextItemWidth(180);
    ImGui::Combo(T("Pivot"), &m_pivot, apiv, 4);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Whole curve"))) { m_ipoint = -1; m_fCurveMode = true; }
    if (ImGui::Button(T("Snap point"))) { if (m_ipoint >= 0) FPathSnap(plo, m_ipoint, "Snap point " + std::to_string(m_ipoint) + " of " + std::to_string(plo->oid)); }
    ImGui::SameLine();
    if (ImGui::Button(T("Snap all"))) FPathSnap(plo, -1, "Snap path " + std::to_string(plo->oid));
    ImGui::SameLine();
    ImGui::Checkbox(T("Snap on drop"), &m_fSnapCollision);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Points dropped by the gizmo land on the level's collision below them"));
    if (ImGui::BeginTable("pts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, std::min(200.0f, 26.0f * float(pd.apos.size() + 1)))))
    {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Point (path space)");
        for (std::size_t i = 0; i < pd.apos.size(); ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::to_string(i).c_str(), int(i) == m_ipoint, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { m_ipoint = int(i); m_fCurveMode = true; }
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            ImGui::SetNextItemWidth(-1);
            glm::vec3 p = pd.apos[i];
            if (ImGui::InputFloat3("##p", glm::value_ptr(p), "%.1f", ImGuiInputTextFlags_EnterReturnsTrue))
            {
                PATHDATA pn;
                if (!FPathRead(plo, pn) || i >= pn.apos.size()) { ImGui::PopID(); continue; }
                pn.apos[i] = p;
                FPathWrite(plo, pn, "Set point " + std::to_string(i) + " of path " + std::to_string(plo->oid));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::muted));
    ImGui::TextWrapped("%s", T("Built again when the world starts (see Reload / Play test): nav polygons, ropes, laser timing, camera zones, a guard's place on it."));
    ImGui::PopStyleColor();
}

}  // namespace edimpl

#endif
