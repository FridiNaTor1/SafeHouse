// The map editor's build palette (notes/EDITOR_PLAN.md steps 7-8): the traversal and interactive objects the
// characters use, each made from a working example of its class in this world (copied in front of the camera, on the
// ground and the grid; a curve's points laid out straight along the view so it can be shaped with the curve tools),
// with who may use it from the game's own rules: Sly's target list (jt.h JTL: the classes his jumps / climbs look
// for), the uses' players (jt.cpp UseMaskInit: option 1074 a class (0x34 everyone, 0x35 Sly, 0x37 Bentley, 0x36
// Murray), 1073 the listed objects' players, class 0x24 Murray's by default, else everyone).
namespace edimpl
{

namespace
{
// a Sly 2 class number as this game's, at run time (EDCID needs a constant on Sly 3)
inline int EdCidRt(int cid2)
{
#if EDGAME_SLY3
    return ::sly3::Cid3FromCid2(cid2);
#else
    return cid2;
#endif
}
struct BUILDKIND
{
    const char* name;
    int cid;              // Sly 2's class
    bool fCurve;          // its points are laid out along the view when made
    const char* who;      // who uses it (the game's rule, in words)
    const char* what;
};
const BUILDKIND s_abuildTraversal[] = {
    {"Ledge (shimmy / hang)", 0x0C, true, "Sly (his target list: ledges)", "A ledge he grabs and shimmies along: its curve is the edge."},
    {"Pipe / pole (climb)", 0x0B, true, "Sly (his target list: pipes)", "A pipe he climbs along its curve (vertical: a pole)."},
    {"Rail / narrow beam / wire", 0x0D, true, "Sly (his target list: rails)", "A rail he lands on and walks or slides along: beams, wires, ropes to walk."},
    {"Hang point / hook", 0x13, false, "Sly (his target list: points with flag 1)", "A point he swings from or hangs on."},
    {"Spire (land on a point)", 0x27, false, "Sly (his target list: spires)", "A point he lands and balances on."},
    {"Swing pole", 0x26, false, "Sly (his target list: swings)", "A pole he swings around."},
    {"Jump target", 0x09, false, "Sly (his target list: points)", "A point his jumps aim for."},
    {"Wall to climb", 0x22, false, "Sly (his target list: walls)", "A climbable wall."},
    {"Hook (walk target)", 0x1B, false, "Sly (his target list: hooks)", "A hook target."},
    {"Rope (a model bent along a path)", 0x6A, false, "(its path: the rail he walks)", "A rope's model, bent along its path (option 1311): pair it with a rail."},
    {"Path (for guards, cameras)", 0x0A, true, "guards' patrols, cameras, ropes", "A plain path: a patrol route, a camera track."},
};
const BUILDKIND s_abuildInteract[] = {
    {"Button / switch (use)", 0x23, false, "everyone, unless its option 1074 names one character", "A use: when used it sets its tasks' states, runs its script and state machines (Links)."},
    {"Heavy use (Murray)", 0x24, false, "Murray (class 0x24's default)", "A use only Murray takes (crank bars, heavy lifts)."},
    {"Door / exit", 0x1A, false, "the players inside its shape", "An exit to another place or world (its task states, its use)."},
    {"Barrier", 0x56, false, "-", "A barrier (opened by its tasks)."},
    {"Pressure pad", 0x53, false, "the players standing on it", "A pad: its state when stood on."},
    {"Vault / safe", 0x4E, false, "Sly (the vault's combination)", "A vault (its combination and loot)."},
    {"Loot (booty)", 0x25, false, "the players", "Booty to steal."},
    {"Hiding spot", 0x58, false, "Sly", "A place to hide (also a jump target)."},
    {"Breakable", 0x62, false, "attacks", "A breakable object (its pieces and loot)."},
    {"Coin", 0x5B, false, "the players", "A coin."},
    {"Clue bottle", 0x5F, false, "the players", "A clue bottle."},
    {"Laser", 0x55, false, "-", "A laser between two points (its path / ends option 455)."},
    {"Detector / security camera", 0x10, false, "-", "A detector that sees the players."},
    {"Checkpoint", 0x2A, false, "-", "A checkpoint (where a player comes back)."},
    {"Warp point", 0x29, false, "-", "A warp point (exits and checkpoints put players there)."},
    {"Job marker", 0x6B, false, "the job's player", "A job's start marker."},
};
}  // namespace

// a working example of the class: a stored record placed away from the origin (the smallest, top-level first)
int CEditor::IobjExampleOf(int cid) const
{
    int ibest = -1;
    std::size_t cbBest = SIZE_MAX;
    int depthBest = INT_MAX;
    for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[i];
        if (r.cid != cid || !r.plo || !FHasMatrix(r.plo) || glm::length(PosWorld(r.plo)) < 1.0f) continue;
        if (r.depth < depthBest || (r.depth == depthBest && r.cb < cbBest)) { ibest = int(i); cbBest = r.cb; depthBest = r.depth; }
    }
    // (else any stored record of the class: a shared group's template, an unplaced one; the copy is placed anyway)
    if (ibest < 0)
        for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
            if (m_rec.aobj[i].cid == cid && (ibest < 0 || m_rec.aobj[i].depth < m_rec.aobj[std::size_t(ibest)].depth)) ibest = int(i);
    return ibest;
}

// a curve's points straight along the view's direction on the ground (length: its points' spread, at least 400)
void CEditor::LayOutCurve(LO* plo, float length)
{
    PATHDATA pd;
    if (!FPathRead(plo, pd) || pd.apos.size() < 2) return;
    const glm::mat4 toWorld = MatPathSpace(plo), toPath = glm::inverse(toWorld);
    glm::vec3 d = m_cam.Dir();
    d.z = 0;
    d = glm::length(d) < 1e-3f ? glm::vec3(1, 0, 0) : glm::normalize(d);
    const glm::vec3 start = PosWorld(plo);
    const bool fVertical = plo->Cid() == EDCID(0x0B) && m_fBuildVertical;
    const std::size_t n = pd.apos.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const float t = float(i) / float(n - 1);
        const glm::vec3 w = fVertical ? start + glm::vec3(0, 0, 50.0f + length * t) : start + d * (length * t) + glm::vec3(0, 0, 50.0f);
        pd.apos[i] = glm::vec3(toPath * glm::vec4(w, 1.0f));
    }
    const glm::vec3 dPath = glm::normalize(glm::mat3(toPath) * (fVertical ? glm::vec3(0, 0, 1) : d)) * (length / float(n - 1));
    for (glm::vec3& v : pd.avIn) v = dPath;
    for (glm::vec3& v : pd.avOut) v = dPath;
    pd.fClosed = false;
    FPathWrite(plo, pd, std::string(T("Lay out")) + " " + NameOfObject(plo));
}

int CEditor::BuildFromPalette(int cid, bool fCurve, std::string& error)
{
#if EDFEAT_STRUCT
    const int iobj = IobjExampleOf(EdCidRt(cid));
    if (iobj < 0) { error = std::string(T("this level has no placed example of that class to copy")) + " (" + SzClass(EdCidRt(cid)) + ")"; return 0; }
    glm::vec3 p = m_cam.pos + m_cam.Dir() * 600.0f, hit;
    if (FSnapDown(p, hit)) p = hit;
    p.x = SnapMove(p.x);
    p.y = SnapMove(p.y);
    const int oid = CopyRecord(iobj, CopyGroup(), &p, true, error);
    if (!oid || !m_plSel) return 0;
    if (fCurve) LayOutCurve(m_plSel, m_buildLength);
    Status(std::string(T("made")) + " " + NameOfObject(m_plSel) + " " + T("from") + " " + std::to_string(m_rec.aobj.size() > std::size_t(iobj) ? m_rec.aobj[std::size_t(iobj)].oid : 0));
    return oid;
#else
    (void)cid; (void)fCurve;
    error = "not for " EDGAME_NAME " yet";
    return 0;
#endif
}

void CEditor::DrawBuildPalette()
{
    ImGui::SeparatorText(T("Traversal and interactive objects"));
    ImGui::TextColored(st::vec(st::muted), "%s", T("Each is a copy of a working one in this level, put in front of the camera; curves are laid out along the view."));
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat(T("Curve length"), &m_buildLength, 10.0f, 100.0f, 10000.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Checkbox(T("Pipes upright"), &m_fBuildVertical);
    std::map<int, int> mpc;
    for (const auto& up : m_psw->aploAll) ++mpc[up->Cid()];
    auto table = [&](const char* id, const BUILDKIND* a, std::size_t c) {
        if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
        ImGui::TableSetupColumn(T("Object"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(T("Who uses it"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(T("In this level"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("##make", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < c; ++i)
        {
            const BUILDKIND& k = a[i];
            const int cid = EdCidRt(k.cid);
            ImGui::PushID(id);
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(T(k.name));
            EdTip(T(k.what));
            ImGui::TableNextColumn();
            ImGui::TextColored(st::vec(st::text2), "%s", T(k.who));
            ImGui::TableNextColumn();
            const int n = cid >= 0 ? mpc[cid] : 0;
            ImGui::TextColored(st::vec(st::muted), "%d", n);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(n == 0);
            if (ImGui::SmallButton(T("Make")))
            {
                std::string error;
                if (!BuildFromPalette(k.cid, k.fCurve, error)) Status(std::string(T("make")) + ": " + error);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::EndTable();
    };
    if (ImGui::TreeNodeEx(T("Traversal"), ImGuiTreeNodeFlags_DefaultOpen)) { table("trav", s_abuildTraversal, std::size(s_abuildTraversal)); ImGui::TreePop(); }
    if (ImGui::TreeNodeEx(T("Interactive"), ImGuiTreeNodeFlags_DefaultOpen)) { table("inter", s_abuildInteract, std::size(s_abuildInteract)); ImGui::TreePop(); }
    ImGui::TextColored(st::vec(st::muted), "%s", T("Crawl spaces: any gap low enough for the crawl (Sly and Bentley crawl, Murray cannot): make one with two blocks and a floor brush over them."));
}

// who may use the selected object, by the game's rules (a use's option 1074 / 1073 / its class; Sly's target list)
std::string CEditor::StrWhoUses(LO* plo) const
{
    if (!plo) return std::string();
    const int cid = plo->Cid();
    for (const BUILDKIND& k : s_abuildTraversal)
        if (EdCidRt(k.cid) == cid) return T(k.who);
#if EDGAME_SLY2
    if (FIsLoDerivedFrom(plo, 0x23))
    {
        int c = 0;
        if (FOptionInt(plo, 1074, c))
            switch (c)
            {
            case 0x34: return T("everyone (option 1074: all players)");
            case 0x35: return T("Sly only (option 1074)");
            case 0x37: return T("Bentley only (option 1074)");
            case 0x36: return T("Murray only (option 1074)");
            default: break;
            }
        if (IfieldOption(plo, 1073) >= 0) return T("the players of the objects its option 1073 lists");
        return FIsLoDerivedFrom(plo, 0x24) ? T("Murray (class 0x24's default)") : T("everyone (the default)");
    }
#endif
    for (const BUILDKIND& k : s_abuildInteract)
        if (EdCidRt(k.cid) == cid) return T(k.who);
    return std::string();
}

// the Properties' section for traversal / interactive objects: what it is, who uses it, its curve laid out again
void CEditor::DrawBuildSection(LO* plo)
{
    const std::string who = StrWhoUses(plo);
    if (who.empty()) return;
    if (!ImGui::CollapsingHeader(TL("Use", "usesection").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextColored(st::vec(st::muted), "%s:", T("Who uses it"));
    ImGui::SameLine();
    ImGui::TextWrapped("%s", who.c_str());
#if EDGAME_SLY2
    if (FIsLoDerivedFrom(plo, 0x23) && EDFEAT_OPTIONS)
    {
        static const std::pair<int, const char*> s_a[] = {{0x34, "Everyone"}, {0x35, "Sly"}, {0x37, "Bentley"}, {0x36, "Murray"}};
        for (const auto& [c, sz] : s_a)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(T(sz)))
            {
                const int32_t v = c;
                const std::string label = std::string(T("Who uses")) + " " + NameOfObject(plo);
                if (const int ifield = IfieldOption(plo, 1074); ifield >= 0) FSetOption(ifield, &v, 4, label);
                else
                {
                    std::vector<uint8_t> ab(4);
                    std::memcpy(ab.data(), &v, 4);
                    FAddOption(plo, 1074, ab, label);
                }
            }
        }
    }
#endif
    if (const auto it = m_mpobj.find(plo); it != m_mpobj.end() && it->second.ipath >= 0)
    {
        if (ImGui::SmallButton(T("Lay out straight along the view"))) LayOutCurve(plo, m_buildLength);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("##buildlen", &m_buildLength, 10.0f, 100.0f, 10000.0f, "%.0f");
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Edit its points"))) { m_fCurveMode = true; m_ipoint = -1; }
    }
}

}  // namespace edimpl
