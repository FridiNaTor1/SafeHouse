// The map editor's build palette (notes/EDITOR_PLAN.md steps 7-8): the traversal and interactive objects the
// characters use, each made from a working example of its class in this world (copied in front of the camera, on the
// ground and the grid; a curve's points laid out straight along the view so it can be shaped with the curve tools),
// with who may use it from the game's own rules: Sly's target list (jt.h JTL: the classes his jumps / climbs look
// for), the uses' players (jt.cpp UseMaskInit: option 1074 a class (0x34 everyone, 0x35 Sly, 0x37 Bentley, 0x36
// Murray), 1073 the listed objects' players, class 0x24 Murray's by default, else everyone).
// Part 2: each player's own rules (jt.cpp / jumptgt.cpp: the lists each player looks for, Sly 0xFFFF, Bentley 0xF8FE,
// Murray 0xF8FA, and the object's players' mask, its description's +0x1C), crawl spaces (class 0x1B: the circle there
// starts the crawl, interaction kind 8, kept while something is within 200 above), keys (prizes 0x5C in guards'
// pockets, options 308 / 309 / 310; the prize's task, option 906), exits' destinations, vaults' combinations.
#if EDGAME_SLY2
#include "jt.h"
#include "../../editor/core/newworld.h"
#endif
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
    {"Crawl space entry", 0x1B, false, "Sly and Bentley (Murray never crawls)", "Where a crawl starts (circle): the player crawls while something is less than 200 above him."},
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
    glm::vec3 p = m_cam.pos + DirPlace() * 600.0f, hit;
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
    // every other class this level has an example of (a copy of it, as above: its options, links and collision kept)
    if (ImGui::TreeNodeEx(T("Every class in this level")))
    {
        static char s_szFilter[64] = "";
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##classfilter", T("Filter: class or type"), s_szFilter, sizeof(s_szFilter));
        std::string f = s_szFilter;
        for (char& ch : f) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        std::map<int, LO*> mpex;
        for (const auto& up : m_psw->aploAll)
            if (!mpex.count(up->Cid())) mpex[up->Cid()] = up.get();
        if (ImGui::BeginTable("##allclasses", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 240)))
        {
            ImGui::TableSetupColumn(T("Class"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(T("Type"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(T("In this level"), ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("##make", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();
            for (const auto& [cid, plo] : mpex)
            {
                const char* szType = T(InfoOfType(TypeOfLo(plo)).name);
                std::string key = std::string(SzClass(cid)) + " " + szType;
                for (char& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (!f.empty() && key.find(f) == std::string::npos) continue;
                ImGui::PushID(cid);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s (0x%X)", SzClass(cid), cid);
                ImGui::TableNextColumn();
                ImGui::TextColored(st::vec(st::text2), "%s", szType);
                ImGui::TableNextColumn();
                ImGui::TextColored(st::vec(st::muted), "%d", mpc[cid]);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(T("Make")))
                {
                    std::string error;
                    const int iobj = IobjExampleOf(cid);
                    glm::vec3 p = m_cam.pos + DirPlace() * 600.0f, hit;
                    if (FSnapDown(p, hit)) p = hit;
                    if (iobj < 0 || !CopyRecord(iobj, CopyGroup(), &p, true, error)) Status(std::string(T("make")) + ": " + (iobj < 0 ? std::string(T("no stored example")) : error));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    // presets: a crawl space (its entries and a low ceiling), a key in a guard's pocket
    ImGui::SeparatorText(T("Presets"));
    {
        const int cCrawl = mpc[EdCidRt(0x1B)];
        if (ImGui::Button(T("Crawl space")))
        {
            std::string error;
            if (!BuildCrawlSpace(error)) Status(std::string(T("crawl space")) + ": " + error);
        }
        EdTip(T("Two crawl entries (one at each end) and a low ceiling with side walls (brushes), along the view: Sly and Bentley crawl through it, Murray cannot (his list has no crawl spaces)."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat(T("Ceiling height"), &m_crawlHeight, 1.0f, 60.0f, 190.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat(T("Width"), &m_crawlWidth, 4.0f, 96.0f, 1024.0f, "%.0f");
        if (cCrawl == 0) ImGui::TextColored(st::vec(st::muted), "%s", T("(this level has no crawl entry: one is taken from another level, e.g. Paris's tables)"));
    }
    {
        LO* pg = m_plSel && FIsLoDerivedFrom(m_plSel, EdCidRt(0x38)) ? m_plSel : nullptr;
        ImGui::BeginDisabled(!pg || mpc[EdCidRt(0x5C)] == 0);
        if (ImGui::Button(T("Key in the selected guard's pocket"))) KeyButton(pg);
        ImGui::EndDisabled();
        if (m_oidKeyTask)
        {
            ImGui::SameLine();
            ImGui::BeginDisabled(m_asel.empty());
            if (ImGui::Button(T("Lock: the selection waits for the key")))
            {
                std::string error;
                if (!FKeyLock(m_oidKeyTask, m_asel, error)) Status(std::string(T("lock")) + ": " + error);
            }
            ImGui::EndDisabled();
        }
        DrawKeyUniquePopup();
        EdTip(T("A copy of this level's key (a prize) put in the selected guard's pocket: Sly pickpockets it; its task is done when he has it, and doors / uses can wait for that task (their task states)."));
    }
}

namespace
{
struct PLAYERRULE
{
    const char* name;
    uint32_t bit;        // ACTOR +0x1000
    uint32_t grfLists;   // the lists 0x194A40 searches for him (sly.cpp / gang.cpp)
};
const PLAYERRULE s_aplayer[3] = {{"Sly", 1, 0xFFFF}, {"Bentley", 2, 0xF8FE}, {"Murray", 4, 0xF8FA}};

std::string StrPlayers(uint32_t grf)
{
    std::vector<std::string> a;
    for (const PLAYERRULE& r : s_aplayer)
        if (grf & r.bit) a.push_back(T(r.name));
    if (a.empty()) return T("nobody");
    if (a.size() == 3) return T("everyone (Sly, Bentley, Murray)");
    if (a.size() == 1) return a[0] + " " + T("only");
    return a[0] + " " + T("and") + " " + a[1];
}
}  // namespace

// Each player's own rules for an object (Sly 2): the list he looks for it in (jt.cpp's 0x194A40 masks; Sly's jumps also
// aim at spires, rails, hang points and climbing surfaces, everyone's at pipes, walls, ledges, swings and hiding spots)
// and its players' mask (the description's +0x1C: the class's default, else the option that sets it)
uint32_t CEditor::GrfPlayersOf(LO* plo, std::string* apszWhy, int* peopidMask, bool* pfKnown) const
{
    if (pfKnown) *pfKnown = false;
    if (peopidMask) *peopidMask = -1;
#if EDGAME_SLY2
    if (!plo) return 0;
    struct KIND { int cid; JTL jtl; uint32_t bit; const char* plural; };
    static const KIND s_akind[] = {
        {0x58, JTL_Jump, 0, "hiding spots"}, {0x1B, JTL_Hook, 4, "crawl spaces"}, {0x22, JTL_Wall, 0x10, "walls"},
        {0x0C, JTL_Ledge, 8, "ledges"}, {0x23, JTL_Use, 0x80, "uses"}, {0x13, JTL_Point, 1, "hang points"},
        {0x0B, JTL_Pipe, 2, "pipes"}, {0x0D, JTL_Rail, 0x200, "rails"}, {0x27, JTL_Spire, 0x400, "spires"},
        {0x26, JTL_Swing, 0x20, "swings"}, {0x09, JTL_Point2, 0, "climbing surfaces"}, {0x28, JTL_Jump2, 0, "jump points"},
    };
    const KIND* pk = nullptr;
    for (const KIND& k : s_akind)
        if (FIsLoDerivedFrom(plo, k.cid)) { pk = &k; break; }
    if (!pk) return 0;
    if (pfKnown) *pfKnown = true;
    // the jump-only kinds: hiding spots and jump points everyone's, climbing surfaces Sly's (no mask)
    if (pk->jtl == JTL_Jump || pk->jtl == JTL_Jump2 || pk->jtl == JTL_Point2)
    {
        const uint32_t grf = pk->jtl == JTL_Point2 ? 1u : 7u;
        if (apszWhy)
            for (int i = 0; i < 3; ++i)
                apszWhy[i] = (grf & s_aplayer[i].bit) ? std::string(T("jumps to")) + " " + T(pk->plural) : std::string(T("only Sly's jumps aim at")) + " " + T(pk->plural);
        return grf;
    }
    // the players' mask: a use's from its options (jt.cpp UseMaskInit), else the description's (the class default:
    // 7, pipes 1), set by the option of the description's getter (its +0x1C)
    uint32_t mask = 7;
    bool fOpt = false;
    int eopidMask = -1;
    if (pk->jtl == JTL_Use)
    {
        mask = FIsLoDerivedFrom(plo, 0x24) ? 4u : 0u;
        for (const OPT& o : plo->aopt)
            if (o.eopid == 1073)
                for (const OPTVAL& v : o.aval)
                    if (LO* p = PloOid(v.n)) mask |= p->Cid() == 0x35 ? 1u : p->Cid() == 0x37 ? 2u : p->Cid() == 0x36 ? 4u : 0u;
        int c = 0;
        if (FOptionInt(plo, 1074, c)) { mask = c == 0x34 ? 7u : mask | (c == 0x35 ? 1u : c == 0x37 ? 2u : c == 0x36 ? 4u : 0u); fOpt = true; }
        if (!mask) mask = 7;
        eopidMask = 1074;
    }
    else
    {
        if (pk->jtl == JTL_Pipe) mask = 1;
        switch (IbJtDesc(pk->jtl))
        {
        case 0x190: eopidMask = 815; break;
        case 0xE0: eopidMask = 805; break;
        case 0x100: eopidMask = 1024; break;
        case 0x130: eopidMask = 1190; break;
        case 0x150: eopidMask = 837; break;
        default: break;
        }
        // (any description getter's +0x1C: the level's own option of that kind, whichever getter it names)
        for (int e : {805, 815, 825, 837, 849, 857, 869, 1024, 1084, 1190})
        {
            int n = 0;
            if (FOptionInt(plo, e, n)) { mask = uint32_t(n); fOpt = true; eopidMask = e; }
        }
    }
    if (peopidMask) *peopidMask = eopidMask;
    if (pk->jtl == JTL_Point && !(plo->U32Field(0, 0xE8, 0) & 1))
    {
        if (apszWhy)
            for (int i = 0; i < 3; ++i) apszWhy[i] = T("not a hang point (its flag 1 is off): a grab target for scripts");
        return 0;
    }
    uint32_t grf = 0;
    for (int i = 0; i < 3; ++i)
    {
        const PLAYERRULE& r = s_aplayer[i];
        const bool fList = (r.grfLists & pk->bit) != 0;
        const bool fMask = (mask & r.bit) != 0;
        if (fList && fMask) grf |= r.bit;
        if (!apszWhy) continue;
        if (!fList) apszWhy[i] = std::string(T("never looks for")) + " " + T(pk->plural) + " " + T("(the game's rule)");
        else if (!fMask)
            apszWhy[i] = fOpt ? std::string(T("left out by its players' option")) + " " + std::to_string(eopidMask)
                              : std::string(T("left out by the class's default")) + " (" + T(pk->plural) + ": " + StrPlayers(mask) + ")";
        else apszWhy[i] = fOpt ? std::string(T("allowed by its players' option")) + " " + std::to_string(eopidMask) : std::string(T("uses")) + " " + T(pk->plural);
    }
    return grf;
#else
    (void)plo; (void)apszWhy;
    return 0;
#endif
}

// the players' mask set (a use: option 1074 when it names one or all; else the description's option)
bool CEditor::FSetPlayers(LO* plo, uint32_t grf)
{
#if EDGAME_SLY2 && EDFEAT_OPTIONS
    int eopid = -1;
    bool fKnown = false;
    GrfPlayersOf(plo, nullptr, &eopid, &fKnown);
    if (!fKnown || eopid < 0) { Status(T("this object has no players' mask")); return false; }
    int32_t v = int32_t(grf & 7);
    if (eopid == 1074)
    {
        // (1074 is a class: everyone, or one player; several but not all: the class of each is ORed by several 1074s,
        // which the editor does not write: everyone then)
        v = grf == 1 ? 0x35 : grf == 2 ? 0x37 : grf == 4 ? 0x36 : 0x34;
    }
    const std::string label = std::string(T("Who uses")) + " " + NameOfObject(plo) + ": " + StrPlayers(grf);
    const int oid = plo->oid;
    bool fOk;
    if (const int ifield = IfieldOption(plo, eopid); ifield >= 0)
    {
        fOk = FSetOption(ifield, &v, 4, label);
        if (fOk) Reload(oid);   // (the description's fields are made at the load)
    }
    else
    {
        std::vector<uint8_t> ab(4);
        std::memcpy(ab.data(), &v, 4);
        fOk = FAddOption(plo, eopid, ab, label);
    }
    return fOk;
#else
    (void)plo; (void)grf;
    return false;
#endif
}

// who may use the selected object, by the game's rules (a use's option 1074 / 1073 / its class; Sly's target list)
std::string CEditor::StrWhoUses(LO* plo) const
{
    if (!plo) return std::string();
#if EDGAME_SLY2
    {
        bool fKnown = false;
        const uint32_t grf = GrfPlayersOf(plo, nullptr, nullptr, &fKnown);
        if (fKnown) return StrPlayers(grf);
    }
#endif
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

// the Properties' section for traversal / interactive objects: what it is, who uses it (each player by the game's
// rules, set with the players' mask), its curve laid out again
void CEditor::DrawBuildSection(LO* plo)
{
    DrawInteractSection(plo);
    const std::string who = StrWhoUses(plo);
    if (who.empty()) return;
    if (!ImGui::CollapsingHeader(TL("Use", "usesection").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextColored(st::vec(st::muted), "%s:", T("Who uses it"));
    ImGui::SameLine();
    ImGui::TextWrapped("%s", who.c_str());
#if EDGAME_SLY2
    std::string awhy[3];
    int eopid = -1;
    bool fKnown = false;
    const uint32_t grf = GrfPlayersOf(plo, awhy, &eopid, &fKnown);
    if (fKnown && ImGui::BeginTable("##players", 3, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##p", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("##ok", ImGuiTableColumnFlags_WidthFixed, 22);
        ImGui::TableSetupColumn("##why", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < 3; ++i)
        {
            const PLAYERRULE& r = s_aplayer[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(T(r.name));
            ImGui::TableNextColumn();
            ImGui::TextColored(st::vec((grf & r.bit) ? st::success : st::muted), "%s", (grf & r.bit) ? "+" : "-");
            ImGui::TableNextColumn();
            ImGui::TextColored(st::vec(st::text2), "%s", awhy[i].c_str());
        }
        ImGui::EndTable();
    }
    if (fKnown && eopid >= 0 && EDFEAT_OPTIONS)
    {
        // the players' mask: which of them the object lets in (each still needs his list: Murray never crawls)
        ImGui::TextColored(st::vec(st::muted), "%s", T("Let in:"));
        static const std::pair<uint32_t, const char*> s_a[] = {{7, "Everyone"}, {1, "Sly"}, {2, "Bentley"}, {4, "Murray"}, {3, "Sly and Bentley"}};
        for (const auto& [m, sz] : s_a)
        {
            if (eopid == 1074 && m == 3) continue;   // (a use's class option names one player or everyone)
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(T(sz)).x + 16.0f < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
            if (ImGui::SmallButton(T(sz))) FSetPlayers(plo, m);
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

// ---- presets: a crawl space, a key

namespace
{
// the view's direction on the ground, snapped to the nearest axis (brushes are made along the axes)
glm::vec3 DirAxis(const glm::vec3& dir)
{
    if (std::fabs(dir.x) >= std::fabs(dir.y)) return glm::vec3(dir.x >= 0 ? 1.0f : -1.0f, 0, 0);
    return glm::vec3(0, dir.y >= 0 ? 1.0f : -1.0f, 0);
}
}  // namespace

// a new top-level record of the class in the level group, its bytes from wherever the class has one: this level's
// parts first (one nested inside a prop: Paris's tables), then the other installed levels'. The copy is the level's own:
// a new oid, no script module, placed on its own (no parent's record), at pos. Its other options are the source's.
int CEditor::ImportRecordOfClass(int cid, const glm::vec3& pos, std::string& from, std::string& error)
{
#if EDFEAT_STRUCT
    const int igrp = CopyGroup();
    if (igrp < 0 || igrp >= int(m_index.groups.size()) || m_index.groups[std::size_t(igrp)].objectsPart.empty()) { error = "no level group"; return 0; }
    const bool fLox = FCidDerivedFrom(cid, EDCID(3));
    // (a record's header: S8 k (0: plain, < 0: a shared group's) S16 cid S32 oid S16 n S32 cb, then cb bytes; checked
    // by the parser and, for a placed class, by its matrix (a rotation): the candidates, tried in turn until one loads)
    struct CAND { std::vector<uint8_t> rec; std::string from; };
    std::vector<CAND> acand;
    auto scan = [&](const std::vector<uint8_t>& ab, const std::string& world) {
        for (std::size_t i = 0; i + 13 < ab.size() && acand.size() < 6; ++i)
        {
            const int8_t k = int8_t(ab[i]);
            if (k > 0 || k < -2 || ab[i + 1] != uint8_t(cid) || ab[i + 2] != uint8_t(cid >> 8)) continue;
            int32_t oid, cb;
            std::memcpy(&oid, ab.data() + i + 3, 4);
            std::memcpy(&cb, ab.data() + i + 9, 4);
            if (oid <= 0 || cb < 16 || i + 13 + std::size_t(cb) > ab.size()) continue;
            if (fLox)
            {
                if (cb < 2 + 48 + 4) continue;
                const glm::mat4 m = MatFromBytes(ab.data() + i + 15);
                bool fOk = true;
                for (int c = 0; c < 3 && fOk; ++c) fOk = std::fabs(glm::length(glm::vec3(m[c])) - 1.0f) < 0.02f;
                fOk = fOk && std::fabs(glm::dot(glm::vec3(m[0]), glm::vec3(m[1]))) < 0.02f && std::isfinite(m[3].x) && std::fabs(m[3].x) < 1e6f;
                if (!fOk) continue;
            }
            std::vector<uint8_t> probe(2 + 13 + std::size_t(cb));
            probe[0] = 1;
            std::memcpy(probe.data() + 2, ab.data() + i, 13 + std::size_t(cb));
            std::vector<ce::ObjRecord> one;
            std::string e2;
            if (!ce::parseObjects(probe.data(), probe.size(), one, e2, EDGAME_SLY3 != 0) || one.size() != 1 || one[0].cb != 13 + std::size_t(cb)) continue;
            acand.push_back({std::vector<uint8_t>(ab.begin() + std::ptrdiff_t(i), ab.begin() + std::ptrdiff_t(i + 13 + std::size_t(cb))), world});
            i += 12 + std::size_t(cb);
        }
    };
    for (const ce::GroupInfo& g : m_index.groups)
    {
        std::vector<uint8_t> ab;
        if (!g.objectsPart.empty() && m_proj.readPart(g.objectsPart, ab)) scan(ab, m_index.entry.name);
    }
    for (const ce::WorldEntry& we : m_inst.worlds)
    {
        if (acand.size() >= 6) break;
        if (!we.indexBase.empty() || we.record == m_eo.record) continue;
        ce::WorldIndex wi;
        std::string e2;
        if (!wi.load(m_inst, we, e2)) continue;
        for (const ce::GroupInfo& g : wi.groups)
        {
            std::vector<uint8_t> ab;
            if (!g.objectsPart.empty() && m_proj.readPart(g.objectsPart, ab)) scan(ab, we.name);
        }
    }
    if (acand.empty()) { error = T("no level of the installation has one"); return 0; }
    for (CAND& c : acand)
    {
        std::vector<uint8_t>& rec = c.rec;
        const int32_t oidNew = OidNew();
        const int16_t nNone = -1;
        rec[0] = 0;   // (the level's own)
        std::memcpy(rec.data() + 3, &oidNew, 4);
        std::memcpy(rec.data() + 7, &nNone, 2);
        if (fLox)
        {
            std::memcpy(rec.data() + 13, &nNone, 2);   // (no parent's record: placed by its own matrix)
            glm::mat4 m = MatFromBytes(rec.data() + 15);
            m[3] = glm::vec4(pos, 1.0f);
            float a[12];
            if (FBytesFromMat(m, a)) std::memcpy(rec.data() + 15, a, 48);
        }
        const std::string label = "Create " + std::string(SzClass(cid)) + " " + std::to_string(oidNew) + " from " + c.from;
        m_proj.begin(label, true);
        std::size_t ibNew = 0;
        const bool fOk = m_proj.appendRecord(m_index.groups[std::size_t(igrp)].objectsPart, rec, &ibNew, error);
        m_proj.end();
        if (!fOk) { if (m_proj.canUndo() && m_proj.history().back().label == label) m_proj.undo(); continue; }
        Reload(oidNew);   // (a record the loader refuses is undone there: the next one is tried)
        if (PloOid(oidNew)) { from = c.from; return oidNew; }
    }
    error = T("no crawl entry found loads here");
    return 0;
#else
    (void)cid; (void)pos; (void)from;
    error = "not for " EDGAME_NAME " yet";
    return 0;
#endif
}

int CEditor::BuildCrawlSpace(std::string& error)
{
#if EDFEAT_STRUCT
    const int cidCrawl = EdCidRt(0x1B);
    int iobj = IobjExampleOf(cidCrawl);
    // (a top-level entry without a script module of its own first: a copy carries none into the level's scripts)
    for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[i];
        if (r.cid != cidCrawl || r.depth != 0 || !r.plo) continue;
        std::string rel;
        std::size_t ib = 0, cb = 0;
        std::vector<uint8_t> ab;
        if (!FRecordRange(int(i), rel, ib, cb) || !m_proj.readPart(rel, ab) || ib + 16 > ab.size()) continue;
        int16_t n;
        std::memcpy(&n, ab.data() + ib + IbOidOfRecord(ab.data() + ib) + 4, 2);
        if (n < 0) { iobj = int(i); break; }
    }
    // (none of the level's own outside a prop: one made from a crawl entry's record found in this level's props or
    // another level's, the level's own from then on)
    if (iobj >= 0 && m_rec.aobj[std::size_t(iobj)].depth > 0) iobj = -1;
    int oidImported = 0;
    std::string from;
    if (iobj < 0)
    {
        oidImported = ImportRecordOfClass(cidCrawl, m_cam.pos + DirPlace() * 600.0f, from, error);
        if (!oidImported) return 0;
        for (std::size_t i = 0; i < m_rec.aobj.size() && iobj < 0; ++i)
            if (m_rec.aobj[i].oid == oidImported && m_rec.aobj[i].depth == 0) iobj = int(i);
        // (its options naming objects of the level it came from: removed; its floor point is set below)
        for (int iguard = 0; iguard < 8; ++iguard)
        {
            LO* pi = PloOid(oidImported);
            int ibad = -1;
            for (std::size_t i = 0; pi && i < m_rec.afield.size() && ibad < 0; ++i)
            {
                const EDITFIELD& f = m_rec.afield[i];
                if (f.fk != EDITFK_Option || f.plo != pi || f.eopid == EDEOPID(1182)) continue;
                const OPTTYPE t = OptType(f.k);
                int n = 0;
                if ((std::string(t.name) == "object id" || FOidOption(f.eopid)) && FOptionInt(pi, f.eopid, n) && n > 0 && !PloOid(n)) ibad = int(i);
            }
            if (ibad < 0) break;
            FRemoveOption(ibad, std::string(T("Remove option")) + " " + StrOption(m_rec.afield[std::size_t(ibad)].eopid) + " " + T("of") + " " + std::to_string(oidImported));
        }
        for (std::size_t i = 0; i < m_rec.aobj.size() && iobj < 0; ++i)
            if (m_rec.aobj[i].oid == oidImported && m_rec.aobj[i].depth == 0) iobj = int(i);
        if (iobj < 0)
        {
            int c = 0;
            for (const EDITOBJREC& r : m_rec.aobj) c += r.oid == oidImported;
            error = "the new crawl entry " + std::to_string(oidImported) + " is not found (" + std::to_string(c) + " records, " + (PloOid(oidImported) ? "loaded" : "not loaded") + ")";
            return 0;
        }
    }
    LO* ploEx = m_rec.aobj[std::size_t(iobj)].plo;
    const int oidExample = m_rec.aobj[std::size_t(iobj)].oid;
    const glm::vec3 posExample = ploEx ? PosWorld(ploEx) : glm::vec3(0.0f);
    const glm::vec3 d = DirAxis(m_cam.Dir());
    glm::vec3 p0 = m_cam.pos + DirPlace() * 600.0f, hit;
    if (FSnapDown(p0, hit)) p0 = hit;
    p0.x = SnapMove(p0.x);
    p0.y = SnapMove(p0.y);
    const float length = std::max(256.0f, m_buildLength);
    // the tunnel (one brush: the walls and the ceiling), turned along the view's axis
    BRUSH b;
    b.shape = 5;
    b.size = glm::vec3(m_crawlWidth, length, m_crawlHeight);
    b.rgb = 0x9C8C78;
    const bool fGrid = m_fGridSnap;
    m_fGridSnap = false;
    const glm::vec3 mid = p0 + d * (length * 0.5f);
    const int oidTunnel = CreateBrush(b, &mid, error);
    m_fGridSnap = fGrid;
    if (!oidTunnel) return 0;
    if (LO* pt = PloOid(oidTunnel); pt && std::fabs(d.x) > 0.5f)
    {
        PLACE pl;
        if (FPlace(pt, pl))
        {
            const glm::vec3 p = PosWorld(pl.ploLive);
            SetPlaceWorld(pl, glm::translate(glm::mat4(1.0f), p) * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1)));
            FRecordPlace(pl, std::string(T("Crawl tunnel along the view")) + " " + std::to_string(oidTunnel));
        }
    }
    const glm::vec3 posTunnel = PloOid(oidTunnel) ? PosWorld(PloOid(oidTunnel)) : mid;
    // the entries: copies of the level's crawl entry at both ends, facing in
    int oidFirst = 0;
    for (int iend = 0; iend < 2; ++iend)
    {
        const float s = iend == 0 ? -1.0f : 1.0f;
        glm::vec3 pe = posTunnel + d * (s * (length * 0.5f - 48.0f));
        pe.z = posTunnel.z;
        // (the records are indexed again after each copy: the example found again by its oid)
        int iex = -1;
        for (std::size_t i = 0; i < m_rec.aobj.size() && iex < 0; ++i)
            if (m_rec.aobj[i].oid == oidExample && m_rec.aobj[i].cid == cidCrawl && m_rec.aobj[i].depth == 0) iex = int(i);
        if (iex < 0) iex = IobjExampleOf(cidCrawl);
        if (iex < 0) break;
        const int oid = iend == 0 && oidImported ? oidImported : CopyRecord(iex, CopyGroup(), &pe, true, error);
        if (!oid) return 0;
        if (!oidFirst) oidFirst = oid;
        if (LO* pc = PloOid(oid))
        {
            PLACE pl;
            if (FPlace(pc, pl))
            {
                // (its height above the floor as the example's: the example's entries stand on their floor)
                glm::vec3 exHit;
                float dzEx = 0.0f;
                if (!oidImported && FSnapDown(posExample + glm::vec3(0, 0, 10.0f), exHit)) dzEx = std::clamp(posExample.z - exHit.z, 0.0f, 150.0f);
                const float yaw = std::atan2(-s * d.y, -s * d.x);
                SetPlaceWorld(pl, glm::translate(glm::mat4(1.0f), glm::vec3(pe.x, pe.y, posTunnel.z + dzEx)) * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 0, 1)));
                FRecordPlace(pl, std::string(T("Crawl entry")) + " " + std::to_string(oid));
            }
            // its floor point (option 1182: the crawl starts 50 above that object; the example's names its own
            // level's): the entry itself, which stands on the tunnel's floor
#if EDGAME_SLY2
            if (const int ifield = IfieldOption(pc, 1182); ifield >= 0)
            {
                const int32_t v = oid;
                FSetOption(ifield, &v, 4, std::string(T("Crawl entry")) + " " + std::to_string(oid) + ": " + T("its floor point"));
            }
#endif
        }
    }
    if (LO* pt = PloOid(oidTunnel)) SelectOnly(pt);
    Status(std::string(T("made a crawl space")) + ": " + T("tunnel") + " " + std::to_string(oidTunnel) + ", " + T("entries from") + " " +
           (oidImported ? std::string(T("a crawl entry of")) + " " + from : std::to_string(oidExample)));
    return oidTunnel;
#else
    error = "not for " EDGAME_NAME " yet";
    return 0;
#endif
}

// a key: a copy of the level's key-like prize (a PRIZE 0x5C instance: the "key" group's when there is one), put in the
// guard's pocket (option 309: the prize's oid; the guard's FillPocket gives it before any random prize), with a task of
// its own (a copy of the copied key's task: no links, no object changes, no marker, not saved; done when Sly takes the
// key, option 906) that a lock can wait for (KeyLock: a map state of the project's own that takes the lock out of the
// world once the task is done). A guard shared by several (placed by a proxy) is made unique first when fUnique, else
// refused with the reason (every guard of the group would carry it); a spawner's guards cannot be.
int CEditor::BuildKey(LO* pguard, std::string& error, int mode)
{
#if EDGAME_SLY2 && EDFEAT_STRUCT
    if (!pguard || !FIsLoDerivedFrom(pguard, 0x38)) { error = T("select a guard first"); return 0; }
    // (a shared guard: one record for every guard of its group)
    int cSame = 0;
    for (const auto& up : m_psw->aploAll) cSame += up->oid == pguard->oid && FIsLoDerivedFrom(up.get(), 0x38);
    // (one guard whose record is its group's alone is not shared: its group loaded once, no proxy places copies of it)
    const bool fShared = cSame > 1 || pguard->oidInstance > 0;
    if (fShared)
    {
        PLACE pl;
        const bool fProxy = FPlace(pguard, pl) && pl.fProxy;
        m_cKeyShared = cSame;
        m_fKeyProxy = fProxy;
        if (mode == 0) { error = T("this guard shares its pocket with others of its kind"); return 0; }
        if (mode == 1 && !fProxy) { error = T("this guard is a copy made by the level (a spawner or a clone): it cannot be made unique"); return 0; }
    }
    if (fShared && mode == 1)
    {
        const glm::vec3 posGuard = PosWorld(pguard);
        if (!FMakeUnique(pguard, error)) return 0;
        // (the unique copy: the guard nearest to where it stood, now a top-level record of the level)
        LO* pbest = nullptr;
        float dBest = 1e30f;
        for (const auto& up : m_psw->aploAll)
            if (FIsLoDerivedFrom(up.get(), 0x38) && up->oidInstance <= 0 && glm::distance(PosWorld(up.get()), posGuard) < dBest) { dBest = glm::distance(PosWorld(up.get()), posGuard); pbest = up.get(); }
        if (!pbest || dBest > 50.0f) { error = "the unique guard is not found"; return 0; }
        pguard = pbest;
    }
    // the prize's placed instance (its proxy) to copy: a pocket's prize first, else any placed prize
    int oidSrc = 0;
    for (const auto& up : m_psw->aploAll)
        for (const OPT& o : up->aopt)
            if ((o.eopid == 309 || o.eopid == 310) && !o.aval.empty() && m_mpoidProxy.count(o.aval[0].n)) { oidSrc = o.aval[0].n; break; }
    if (!oidSrc)
        for (const auto& up : m_psw->aploAll)
            if (up->Cid() == 0x5C && up->oidInstance > 0 && m_mpoidProxy.count(up->oidInstance)) { oidSrc = up->oidInstance; break; }
    if (!oidSrc) { error = T("this level has no placed prize (key) to copy"); return 0; }
    const int oidGuard = pguard->oid;
    // the copied key's task (option 906 of the prize instance)
    int oidTaskSrc = 0;
    for (const auto& up : m_psw->aploAll)
        if (up->oidInstance == oidSrc && up->Cid() == 0x5C)
            if (const uint32_t t = up->U32Field(0, 0x230, 0xFFFFFFFFu); t != 0xFFFFFFFFu) oidTaskSrc = int(t);
    if (!PloOid(oidTaskSrc, EDCID(0x85)))
        for (const auto& up : m_psw->aploAll)
            if (up->Cid() == EDCID(0x85)) { oidTaskSrc = up->oid; break; }
    glm::vec3 p = PosWorld(pguard) + glm::vec3(0, 0, 100.0f);
    const int oidKey = CopyRecord(m_mpoidProxy[oidSrc], CopyGroup(), &p, false, error);
    if (!oidKey) return 0;
    // its own task
    int oidTask = 0;
    if (LO* pts = PloOid(oidTaskSrc, EDCID(0x85)))
    {
        const int iobj = IobjOfLo(pts);
        oidTask = OidFreeSmall(1000);
        RECEDIT re;
        for (int e : {1260, 1261, 1262, 1263, 1295, 1296, 1297, 1298, 1299, 1287, 1288, 1278})
            if (EDEOPID(e) >= 0) re.setRemove.insert(EDEOPID(e));
        re.fLinks = true;   // (no links: available from the start, done when the key is taken)
        re.nModule = -1;
        re.mpSet[EDEOPID(1279)] = 0;
        std::vector<uint8_t> rec;
        m_proj.begin(std::string(T("Task of key")) + " " + std::to_string(oidKey), true);
        const bool fOk = iobj >= 0 && oidTask && FBuildCopy(iobj, oidTask, re, rec, error) && FAppendCopy(iobj, rec, error);
        m_proj.end();
        if (!fOk) oidTask = 0;
        else Reload(oidTask);
    }
    // the key's task set to it (option 906 of its instance: its proxy's per-instance option)
    if (oidTask)
        for (const auto& up : m_psw->aploAll)
            if (up->oidInstance == oidKey && up->Cid() == 0x5C)
            {
                if (const int ifield = IfieldOption(up.get(), 906); ifield >= 0)
                {
                    const int32_t v = oidTask;
                    FSetOption(ifield, &v, 4, std::string(T("Key")) + " " + std::to_string(oidKey) + ": " + T("its own task") + " " + std::to_string(oidTask));
                }
                break;
            }
    LO* pg = PloOid(oidGuard);
    if (!pg) { error = "the guard is gone"; return 0; }
    // option 309 (a method option, one CRef: kind 6 an S32 oid)
    std::vector<uint8_t> ab;
    const uint16_t k = 6;
    const int32_t v = oidKey;
    ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&k), reinterpret_cast<const uint8_t*>(&k) + 2);
    ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + 4);
    if (!FAddOption(pg, 309, ab, std::string(T("Key")) + " " + std::to_string(oidKey) + " " + T("in the pocket of") + " " + std::to_string(oidGuard))) { error = "could not add the pocket's prize"; return 0; }
    m_oidKeyLast = oidKey;
    m_oidKeyTask = oidTask;
    Status(std::string(T("made a key")) + " " + std::to_string(oidKey) + " " + T("in the pocket of") + " " + std::to_string(oidGuard) + "; " +
           (oidTask ? std::string(T("its own task")) + " " + std::to_string(oidTask) + " (" + T("select a door or a barrier and press Lock to make it wait for the key") + ")"
                    : std::string(T("it opens what the copied key's task opens"))));
    return oidKey;
#else
    (void)pguard; (void)mode;
    error = "not for " EDGAME_NAME " yet";
    return 0;
#endif
}

// a lock for a key's task: the objects taken out of the world once the task is done (a map state of the project's own,
// mod.json: honoured by the game at the world's start and at every task change); shared objects made unique first
bool CEditor::FKeyLock(int oidTask, const std::vector<LO*>& alock, std::string& error)
{
#if EDGAME_SLY2
    if (!PloOid(oidTask, EDCID(0x85))) { error = T("the key has no task of its own"); return false; }
    if (alock.empty()) { error = T("select the door or barrier the key opens"); return false; }
    cane::mods::MapState st;
    const ce::WorldEntry* pe = m_inst.world(m_eo.record);
    st.world = pe ? pe->name : std::string();
    st.id = "key-" + std::to_string(oidTask);
    for (const cane::mods::MapState& s : ce::mapStates(m_proj, st.world))
        if (s.id == st.id) st = s;
    st.name = std::string(T("Unlocked by key task")) + " " + std::to_string(oidTask);
    cane::mods::StateCond c;
    c.kind = cane::mods::StateCond::Task;
    c.oid = oidTask;
    c.states = {2, 3};
    st.when = {c};
    for (LO* p : alock)
    {
        // (a proxy's instance: its root's oid repeats in every instance of the group; the proxy's oid names this one)
        LO* pr = p;
        while (pr->paloParent) pr = pr->paloParent;
        const int oid = pr->oidInstance > 0 ? pr->oidInstance : pr->oid;
        if (std::find(st.remove.begin(), st.remove.end(), oid) == st.remove.end()) st.remove.push_back(oid);
    }
    ce::putMapState(m_proj, st);
    if (!FSaveStates(error)) return false;
    for (std::size_t i = 0; i < m_astate.size(); ++i)
        if (m_astate[i].id == "custom:" + st.id) { m_istateEdit = int(i); m_fStateNewOpen = false; }
    Status(std::string(T("lock")) + ": " + std::to_string(alock.size()) + " " + T("object(s) taken out when the key's task is done") + " (" + T("States tab") + ": " + st.name + ")");
    return true;
#else
    (void)oidTask; (void)alock;
    error = "not for " EDGAME_NAME " yet";
    return false;
#endif
}

// a giver's pocket in words (options 308 coins, 309 / 310 prizes; else the episode's random table)
std::vector<std::string> CEditor::PocketWords(LO* plo) const
{
    std::vector<std::string> a;
#if EDGAME_SLY2
    if (!plo) return a;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        if (o.eopid == 308) a.push_back(std::string(T("Coins")) + ": " + std::to_string(o.aval[0].n));
        else if (o.eopid == 309 || o.eopid == 310)
        {
            const int oid = o.aval[0].n;
            LO* pp = PloOid(oid);
            if (!pp)
                for (const auto& up : m_psw->aploAll)
                    if (up->oidInstance == oid && !up->paloParent) { pp = up.get(); break; }
            std::string s = std::string(T("Prize")) + ": " + (pp ? NameOfObject(pp) : std::to_string(oid)) + " (" + std::to_string(oid) + ")";
            if (pp)
                if (const uint32_t t = pp->U32Field(0, 0x230, 0xFFFFFFFFu); t != 0xFFFFFFFFu) s += std::string(", ") + T("its task") + " " + std::to_string(t);
            a.push_back(s);
        }
    }
    if (a.empty() && FIsLoDerivedFrom(plo, 0x38)) a.push_back(T("A random prize from the episode's table (or nothing)"));
#else
    (void)plo;
#endif
    return a;
}

// an exit's destination: its world key record (the world file's disc id: the manifest's world entry) and its point
std::string CEditor::StrExitTo(LO* plo) const
{
#if EDGAME_SLY2
    if (!plo || !FIsLoDerivedFrom(plo, 0x1A)) return std::string();
    const LOXSHAPE* px = static_cast<const LOXSHAPE*>(plo);
    if (px->aabKey.empty()) return T("nowhere (no world key)");
    const FK fk = FkFromRecord(px->aabKey[0].data());
    static std::map<uint32_t, int> s_mpidRecord;
    static std::string s_dir;
    if (s_dir != ce::utf8(m_inst.dir))
    {
        s_dir = ce::utf8(m_inst.dir);
        s_mpidRecord.clear();
        std::vector<uint8_t> ab;
        if (ce::readFile(m_inst.dir / "extraction_manifest.json", ab))
        {
            const nlohmann::json j = nlohmann::json::parse(ab.begin(), ab.end(), nullptr, false);
            std::map<std::string, int> mppath;
            if (j.is_object() && j.contains("worlds"))
                for (const auto& w : j["worlds"]) mppath[w.value("path", std::string())] = w.value("record", -1);
            if (j.is_object() && j.contains("files"))
                for (const auto& f : j["files"])
                    if (f.value("kind", std::string()) == "world")
                        if (const auto it = mppath.find(f.value("path", std::string())); it != mppath.end()) s_mpidRecord[f.value("id", 0u)] = it->second;
        }
    }
    std::string s;
    if (fk.id == 0 || px->uX == 0) s = T("nowhere (its key is empty: a script or task moves the player)");
    else if (const auto it = s_mpidRecord.find(fk.id); it != s_mpidRecord.end())
    {
        const ce::WorldEntry* pe = m_inst.world(it->second);
        s = it->second == m_eo.record ? std::string(T("this level")) : std::string(pe ? pe->name : "?") + " (" + T("world") + " " + std::to_string(it->second) + ")";
    }
    else s = std::string(T("the file")) + " " + std::to_string(fk.id);
    const uint32_t oidPoint = plo->U32Field(0, 0x150, 0x189);
    s += oidPoint == 0x189 ? std::string(", ") + T("at the safe house (point 393)") : std::string(", ") + T("at point") + " " + std::to_string(oidPoint);
    if (px->aabKey.size() > 1) s += ", " + std::to_string(px->aabKey.size() - 1) + " " + T("movie(s) first");
    return s;
#else
    (void)plo;
    return std::string();
#endif
}

// an exit made to lead where another exit leads: its key record (64 bytes, found in its record) patched with the other's
bool CEditor::FSetExitTo(LO* plo, LO* pother)
{
#if EDGAME_SLY2
    if (!plo || !pother || !FIsLoDerivedFrom(plo, 0x1A) || !FIsLoDerivedFrom(pother, 0x1A)) return false;
    const LOXSHAPE* px = static_cast<const LOXSHAPE*>(plo);
    const LOXSHAPE* po = static_cast<const LOXSHAPE*>(pother);
    if (px->aabKey.empty() || po->aabKey.empty()) return false;
    const auto abFrom = px->aabKey[0];
    const auto abTo = po->aabKey[0];
    std::string rel;
    std::size_t ib = 0, cb = 0;
    std::vector<uint8_t> ab;
    const int iobj = IobjOfLo(plo);
    if (iobj < 0 || !FRecordRange(iobj, rel, ib, cb) || !m_proj.readPart(rel, ab)) return false;
    const auto itEnd = ab.begin() + std::ptrdiff_t(std::min(ab.size(), ib + cb));
    const auto it = std::search(ab.begin() + std::ptrdiff_t(ib), itEnd, abFrom.begin(), abFrom.end());
    if (it == itEnd) { Status(T("the exit's key was not found in its record")); return false; }
    std::string w = StrExitTo(pother);
    w = w.substr(0, w.find(", "));
    const int oid = plo->oid;
    m_proj.begin(std::string(T("Exit")) + " " + std::to_string(oid) + " " + T("leads to") + " " + w);
    const bool fOk = m_proj.patch(rel, std::size_t(it - ab.begin()), abTo.data(), 64);
    m_proj.end();
    Reload(oid);
    return fOk;
#else
    (void)plo; (void)pother;
    return false;
#endif
}

// the key buttons: a shared guard asks first (made unique, or not at all)
void CEditor::KeyButton(LO* pguard)
{
    std::string error;
    if (BuildKey(pguard, error, 0)) return;
    if (pguard && error == T("this guard shares its pocket with others of its kind"))
    {
        m_oidKeyGuard = pguard->oid;
        m_fKeyUniqueAsk = true;
        return;
    }
    Status(std::string(T("key")) + ": " + error);
}

void CEditor::DrawKeyUniquePopup()
{
    if (m_fKeyUniqueAsk) { ImGui::OpenPopup("##keyunique"); m_fKeyUniqueAsk = false; }
    if (!ImGui::BeginPopupModal("##keyunique", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(T("This guard shares its pocket"));
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::Text("%s: %d", T("Guards with this pocket"), m_cKeyShared);
    if (m_fKeyProxy)
        ImGui::TextUnformatted(T("It is one of a group placed by a proxy: a key put in it would be in every one of these guards' pockets. Make this one unique (its own copy in the level, placed where it stands) and give the key to it alone? Options its proxy sets for this one guard are not carried over."));
    else
        ImGui::TextUnformatted(T("It is a copy the level makes (a spawner or a clone): it cannot be made unique. The key can go in every one's pocket: the first taken is the key, the others' pockets drop it then."));
    ImGui::PopTextWrapPos();
    if (m_fKeyProxy && ImGui::Button(T("Make it unique and give it the key")))
    {
        std::string error;
        if (LO* pg = PloOid(m_oidKeyGuard); !pg || !BuildKey(pg, error, 1)) Status(std::string(T("key")) + ": " + error);
        ImGui::CloseCurrentPopup();
    }
    if (m_fKeyProxy) ImGui::SameLine();
    if (ImGui::Button(T("Put it in every one's pocket")))
    {
        std::string error;
        if (LO* pg = PloOid(m_oidKeyGuard); !pg || !BuildKey(pg, error, 2)) Status(std::string(T("key")) + ": " + error);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// the Properties' interactive section: a guard's pocket (pickpocket), a key (prize), an exit's destination, a vault
void CEditor::DrawInteractSection(LO* plo)
{
#if EDGAME_SLY2
    if (!plo) return;
    const bool fGuard = FIsLoDerivedFrom(plo, 0x38), fPrize = FIsLoDerivedFrom(plo, 0x5C), fExit = FIsLoDerivedFrom(plo, 0x1A), fVault = FIsLoDerivedFrom(plo, 0x4E);
    if (!fGuard && !fPrize && !fExit && !fVault) return;
    if (!ImGui::CollapsingHeader(TL("Interaction", "interactsection").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (fGuard)
    {
        ImGui::TextColored(st::vec(st::muted), "%s", T("Pocket (Sly pickpockets it):"));
        for (const std::string& sz : PocketWords(plo)) ImGui::BulletText("%s", sz.c_str());
        if (ImGui::SmallButton(T("Put a key in this pocket"))) KeyButton(plo);
        DrawKeyUniquePopup();
    }
    if (fPrize)
    {
        const uint32_t t = plo->U32Field(0, 0x230, 0xFFFFFFFFu);
        ImGui::TextColored(st::vec(st::muted), "%s", T("A prize (a key, a clue bottle): when taken, its task is done (option 906)."));
        ImGui::Text("%s: %s", T("Its task"), t == 0xFFFFFFFFu ? T("none") : std::to_string(t).c_str());
        std::vector<std::string> aguard;
        const int oidSelf = plo->oidInstance > 0 ? plo->oidInstance : plo->oid;
        for (const auto& up : m_psw->aploAll)
            for (const OPT& o : up->aopt)
                if ((o.eopid == 309 || o.eopid == 310) && !o.aval.empty() && (o.aval[0].n == oidSelf || o.aval[0].n == plo->oid)) aguard.push_back(NameOfObject(up.get()) + " " + std::to_string(up->oid));
        if (!aguard.empty()) ImGui::TextWrapped("%s: %s", T("In the pocket of"), aguard[0].c_str());
        if (t != 0xFFFFFFFFu && PloOid(int(t), EDCID(0x85)))
        {
            ImGui::TextColored(st::vec(st::muted), "%s", T("A lock: select it (Ctrl+click: a door, a barrier...) with this key last, then:"));
            if (ImGui::SmallButton(T("Lock: the other selected objects wait for this key")))
            {
                std::vector<LO*> a;
                for (LO* p : m_asel)
                    if (p != plo) a.push_back(p);
                std::string error;
                if (!FKeyLock(int(t), a, error)) Status(std::string(T("lock")) + ": " + error);
            }
        }
    }
    if (fExit)
    {
        ImGui::TextColored(st::vec(st::muted), "%s", T("Leads to:"));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", StrExitTo(plo).c_str());
        // another destination: one of the places this level's exits lead to (their world keys)
        const LOXSHAPE* px = static_cast<const LOXSHAPE*>(plo);
        std::vector<std::pair<std::string, LO*>> adest;
        std::set<std::string> setSeen;
        for (const auto& up : m_psw->aploAll)
            if (FIsLoDerivedFrom(up.get(), 0x1A) && up.get() != plo && !static_cast<const LOXSHAPE*>(up.get())->aabKey.empty())
            {
                std::string w = StrExitTo(up.get());
                w = w.substr(0, w.find(", "));
                if (setSeen.insert(w).second) adest.push_back({w, up.get()});
            }
        if (!adest.empty() && !px->aabKey.empty())
        {
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo(T("Lead to"), T("(another place)")))
            {
                for (const auto& [w, pother] : adest)
                    if (ImGui::Selectable(w.c_str()) && FSetExitTo(plo, pother))
                    {
                        ImGui::EndCombo();
                        return;
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::TextColored(st::vec(st::muted), "%s", T("Armed by its task states (option 788) and fired when the player walks in (or its use, option 786); its point: option 779."));
    }
    if (fVault)
    {
        int n = 123;
        float g = 0;
        int ifield = IfieldOption(plo, 1225);
        if (ifield >= 0 && FOptionFloat(plo, 1225, g)) n = int(g);
        ImGui::TextColored(st::vec(st::muted), "%s", T("A vault: Sly opens it with the level's 30 clue bottles and its combination (three keys 1..9)."));
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt(T("Combination"), &n, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && n >= 111 && n <= 999)
        {
            const float v = float(n);
            const std::string label = std::string(T("Vault combination")) + " " + std::to_string(n);
            if (ifield >= 0) FSetOption(ifield, &v, 4, label);
            else
            {
                std::vector<uint8_t> ab(4);
                std::memcpy(ab.data(), &v, 4);
                FAddOption(plo, 1225, ab, label);
            }
        }
    }
#else
    (void)plo;
#endif
}

// --editor-do verbs of this module
bool CEditor::FBuildVerb(const std::string& verb, const std::string& arg)
{
    if (FStatesVerb(verb, arg) || FLevelVerb(verb, arg) || FGuestVerb(verb, arg) || FPrefabVerb(verb, arg) || FLightingVerb(verb, arg) || FSoundVerb(verb, arg) || FCheckVerb(verb, arg) || FEverythingVerb(verb, arg) || FBrushEditVerb(verb, arg)) return true;
    if (verb == "optscan")   // optscan:A-B: every object with an option in A..B (its class and values)
    {
        int a = 0, b = 0;
        if (std::sscanf(arg.c_str(), "%d-%d", &a, &b) != 2) return true;
        for (const auto& up : m_psw->aploAll)
            for (const OPT& o : up->aopt)
                if (o.eopid >= a && o.eopid <= b)
                {
                    std::string v;
                    for (const OPTVAL& x : o.aval) v += " " + std::to_string(x.n);
                    std::printf("editor: optscan %s oid %d inst %d eopid %d:%s\n", SzClass(up->Cid()), up->oid, up->oidInstance, o.eopid, v.c_str());
                }
        return true;
    }
    if (verb == "listtype")   // listtype:TYPE: the placed objects of a type (oid, class, place)
    {
        for (const auto& up : m_psw->aploAll)
        {
            LO* p = up.get();
            const OBJTYPE ot = TypeOfLo(p);
            if (arg != InfoOfType(ot).name && arg != InfoOfType(ot).plural) continue;
            const glm::vec3 w = FHasMatrix(p) ? PosWorld(p) : glm::vec3(0.0f);
            std::printf("editor: listtype %s oid %d inst %d %s at %.0f %.0f %.0f\n", NameOfObject(p).c_str(), p->oid, p->oidInstance, SzClass(p->Cid()), w.x, w.y, w.z);
        }
        return true;
    }
    if (verb == "players")   // the selection's players by the game's rules
    {
        for (LO* p : m_asel)
        {
            std::string awhy[3];
            int eopid = -1;
            bool fKnown = false;
            const uint32_t grf = GrfPlayersOf(p, awhy, &eopid, &fKnown);
            std::printf("editor: players %s %d: %s (mask option %d)\n", NameOfObject(p).c_str(), p->oid, fKnown ? StrPlayers(grf).c_str() : "-", eopid);
            for (int i = 0; i < 3 && fKnown; ++i) std::printf("editor: players  %s: %s\n", s_aplayer[i].name, awhy[i].c_str());
        }
        return true;
    }
    if (verb == "setplayers")   // setplayers:MASK (1 Sly, 2 Bentley, 4 Murray): the selected object's players' mask
    {
        if (m_plSel) FSetPlayers(m_plSel, uint32_t(std::strtol(arg.c_str(), nullptr, 0)));
        return true;
    }
    if (verb == "crawl")   // a crawl space in front of the camera (along the view)
    {
        std::string error;
        const int oid = BuildCrawlSpace(error);
        if (!oid) Status("crawl: " + error);
        else std::printf("editor: crawl space %d\n", oid);
        return true;
    }
    if (verb == "key")   // key[:GUARD[:unique|all]]: a key in the guard's (or the selected guard's) pocket, with its own task (a shared
                         // guard: refused, or made unique first, or given to every guard of its kind)
    {
        std::string error;
        const auto colon = arg.find(':');
        LO* pg = arg.empty() ? m_plSel : PloOid(std::atoi(arg.c_str()));
        const std::string how = colon != std::string::npos ? arg.substr(colon + 1) : std::string();
        const int oid = BuildKey(pg, error, how == "unique" ? 1 : how == "all" ? 2 : 0);
        if (!oid) Status("key: " + error);
        else std::printf("editor: key %d task %d\n", oid, m_oidKeyTask);
        return true;
    }
    if (verb == "keylock")   // keylock[:TASK]: the selection waits for the last key's task (or TASK)
    {
        std::string error;
        const int oidTask = arg.empty() ? m_oidKeyTask : std::atoi(arg.c_str());
        if (!FKeyLock(oidTask, m_asel, error)) Status("keylock: " + error);
        else std::printf("editor: lock of task %d: %zu objects\n", oidTask, m_asel.size());
        return true;
    }
    if (verb == "dockscroll") { m_dockScroll = float(std::atof(arg.c_str())); return true; }   // dockscroll:Y (the Create tab)
    if (verb == "exitset")   // exitset:OTHER: the selected exit leads where exit OTHER leads
    {
        if (!FSetExitTo(m_plSel, PloOid(std::atoi(arg.c_str())))) Status("exitset: not changed");
        return true;
    }
    if (verb == "pocket")
    {
        for (LO* p : m_asel)
            for (const std::string& sz : PocketWords(p)) std::printf("editor: pocket %d: %s\n", p->oid, sz.c_str());
        return true;
    }
    if (verb == "exitto")
    {
        std::vector<LO*> a = m_asel;
        if (arg == "all")
        {
            a.clear();
            for (const auto& up : m_psw->aploAll)
                if (FIsLoDerivedFrom(up.get(), EdCidRt(0x1A))) a.push_back(up.get());
        }
        for (LO* p : a) std::printf("editor: exit %d leads to %s\n", p->oid, StrExitTo(p).c_str());
        return true;
    }
    return false;
}

}  // namespace edimpl
