// The map editor's "Check level" (notes/EDITOR_PLAN.md round 5, item 7): one pass over the level for what breaks it in
// the game, each finding with the object to jump to and, where the fix is obvious, a Fix: references to objects not
// in the level (an option naming a missing oid: remove the option; a task linking a missing task: drop the link),
// spawns and checkpoints with no ground under them (put on the ground / a floor under), traversal hanging over
// nothing, doors and ways in or out that lead nowhere, locks whose key task is gone, task chains with no end (a
// cycle, or every task of a job leading on), objects far outside the level, characters from other levels whose
// level / object / patrol is missing. Sly 2 (the classes are Sly 2's; Sly 3: the reference checks only).

namespace edimpl
{

// the ground below a point: the level's collision and every object's (brushes, imported models); false: none within 6000
bool CEditor::FGroundBelow(const glm::vec3& w, glm::vec3& hit)
{
    if (FSnapDown(w, hit)) return true;
    if (!m_fCheckSoes)
    {
        m_apsoeAll.clear();
        for (const auto& up : m_psw->aploAll)
            if (FCidDerivedFrom(up->Cid(), EDCID(0x30)) && up->fInWorld)
                for (SOE& soe : static_cast<ALO*>(up.get())->asoe)
                    if (!soe.f48) m_apsoeAll.push_back(&soe);
        m_fCheckSoes = true;
    }
    LSG lsg{};
    const glm::vec3 a = w + glm::vec3(0, 0, 150.0f), b = w - glm::vec3(0, 0, 6000.0f);
    if (m_apsoeAll.empty() || !PsoeHitTestLineObjects(0, a, b, m_apsoeAll, &lsg)) return false;
    hit = a + (b - a) * lsg.t0;
    return true;
}

void CEditor::CheckLevel()
{
    m_acheck.clear();
    m_fCheckSoes = false;
    auto add = [&](int sev, const char* cat, const std::string& text, int oid, int fix = CF_None, int ifield = -1, glm::vec3 pos = glm::vec3(0), int arg = 0) {
        CHECK c;
        c.sev = sev;
        c.cat = cat;
        c.text = text;
        c.oid = oid;
        c.fix = fix;
        c.ifield = ifield;
        c.pos = pos;
        c.arg = arg;
        m_acheck.push_back(c);
    };
    std::set<int> setOid, setInstalled;   // (installed: the level's own top-level records as the game ships them)
    for (const ce::GroupInfo& g : m_index.groups)
        for (const ce::ObjRecord& r : g.objects) setInstalled.insert(r.oid);
    auto fOwn = [&](LO* p) {   // (made or placed by the project: its top-level record is not one of the installed level's)
        LO* pt = PloTopLevel(p);
        PLACE pl;
        const int oid = FPlace(p, pl) && pl.fProxy ? pl.oidProxy : pt ? pt->oid : p->oid;
        return !setInstalled.count(oid);
    };
    for (const auto& up : m_psw->aploAll) setOid.insert(up->oid);
    for (const EDITOBJREC& r : m_rec.aobj) setOid.insert(r.oid);
    for (const ce::GroupInfo& g : m_index.groups)
        for (const ce::ObjRecord& r : g.objects) setOid.insert(r.oid);
    // ---- references
    for (std::size_t i = 0; i < m_rec.afield.size(); ++i)
    {
        const EDITFIELD& f = m_rec.afield[i];
        if (f.fk != EDITFK_Option || !f.plo) continue;
        const OPTTYPE t = OptType(f.k);
        const bool fOid = std::string(t.name) == "object id" || FOidOption(f.eopid);
        if (!fOid || t.cf || t.cb < 2) continue;
        int n = 0;
        if (!FOptionInt(f.plo, f.eopid, n) || n <= 0 || setOid.count(n)) continue;
#if EDGAME_SLY2
        if (int w = 0; f.plo->Cid() == EDCID(0x85) && FOptionInt(f.plo, EDEOPID(1275), w) && w != m_eo.record) continue;
        // (the game's own jobs and tasks name other levels' exits and markers: 1259 the job's last exit, 1279 a task's marker)
        if (f.eopid == EDEOPID(1274) && n == 0x189) continue;   // (a task's start point 393: the safe house's, every level's default)
        if ((f.plo->Cid() == EDCID(0x84) || f.plo->Cid() == EDCID(0x85)) && (f.eopid == EDEOPID(1259) || f.eopid == EDEOPID(1279)) && !fOwn(f.plo)) continue;
#endif
        const std::string text = Fmt(T("%s: %s names %d, which is not in the level"), NameOfObject(f.plo).c_str(), StrOption(f.eopid).c_str(), n);
        if (std::any_of(m_acheck.begin(), m_acheck.end(), [&](const CHECK& c) { return c.text == text; })) continue;   // (the same record seen through clones)
        add(0, T("Missing object"), text, f.plo->oid, CF_RemoveOption, int(i));
    }
#if EDGAME_SLY2
    // ---- tasks: missing links, chains with no end
    std::map<int, std::vector<int>> mppost;
    for (const auto& up : m_psw->aploAll)
    {
        if (up->Cid() != EDCID(0x85)) continue;
        TASKLINKS tl;
        if (!FTaskLinks(up.get(), tl)) continue;
        for (int oid : tl.aoidPre)
            if (!setOid.count(oid)) add(0, T("Task chain"), Fmt(T("task %d: its prerequisite %d is not in the level"), up->oid, oid), up->oid, CF_DropLinks);
        for (int oid : tl.aoidPost)
            if (!setOid.count(oid)) add(0, T("Task chain"), Fmt(T("task %d: its next task %d is not in the level"), up->oid, oid), up->oid, CF_DropLinks);
        mppost[up->oid] = tl.aoidPost;
    }
    // (a cycle: a task that leads back to itself)
    std::set<int> setReported;
    for (const auto& [oid, apost] : mppost)
    {
        std::set<int> seen;
        std::vector<int> stack(apost.begin(), apost.end());
        bool fCycle = false;
        while (!stack.empty() && !fCycle && seen.size() < 4000)
        {
            const int o = stack.back();
            stack.pop_back();
            if (o == oid) fCycle = true;
            if (!seen.insert(o).second) continue;
            if (const auto it = mppost.find(o); it != mppost.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
        }
        if (fCycle && !setReported.count(oid))
        {
            for (int o : seen) setReported.insert(o);
            add(1, T("Task chain"), Fmt(T("task %d leads back to itself: its chain has no end"), oid), oid);
        }
    }
    // ---- spawns and checkpoints with no ground under them
    for (LO* p : m_psw->mpcidaplo[0x2A])
    {
        if (!p->fInWorld) continue;
        const glm::vec3 pos = PosWorld(p);
        glm::vec3 hit;
        if (!FGroundBelow(pos + glm::vec3(0, 0, 100.0f), hit))
            add(0, T("Spawn"), Fmt(T("checkpoint %d has no ground under it (the player falls)"), p->oid), p->oid, CF_FloorUnder, -1, pos);
    }
    for (const cane::mods::NewWorld& w : cane::mods::newWorlds(m_proj.manifest))
        if (w.name == GuestWorldName() && w.hasSpawn)
        {
            const glm::vec3 pos(w.spawn[0], w.spawn[1], w.spawn[2]);
            glm::vec3 hit;
            if (!FGroundBelow(pos, hit)) add(0, T("Spawn"), T("the level's spawn has no ground under it (the player falls)"), 0, CF_FloorUnder, -1, pos);
            else if (pos.z - hit.z > 1500.0f) add(1, T("Spawn"), Fmt(T("the level's spawn is %.0f above the ground"), pos.z - hit.z), 0, CF_SpawnDown, -1, hit);
        }
    // ---- traversal over nothing
    for (const auto& up : m_psw->aploAll)
    {
        LO* p = up.get();
        if (!p->fInWorld || TypeOfLo(p) != OT_Traversal || !m_mpobj.count(p) || !fOwn(p)) continue;   // (the game's own: over water and chasms by design)
        const glm::vec3 pos = PosWorld(p);
        glm::vec3 hit;
        if (!FGroundBelow(pos, hit)) add(1, T("Traversal"), Fmt(T("%s hangs over nothing (a fall from it never lands)"), NameOfObject(p).c_str()), p->oid, CF_FloorUnder, -1, pos);
    }
    // ---- doors and ways that lead nowhere; locks whose key task is gone
    for (LO* p : m_psw->mpcidaplo[0x1A])
    {
        if (!p->fInWorld || !m_mpobj.count(p)) continue;
        const std::string to = StrExitTo(p);
        if (to.rfind(T("the file"), 0) == 0) add(0, T("Door"), Fmt(T("exit %d leads to %s, which is no level of the installation"), p->oid, to.c_str()), p->oid);
    }
    for (const cane::mods::Entry& e : cane::mods::entries(m_proj.manifest))
    {
        if (e.world != GuestWorldName()) continue;
        bool fTo = false;
        for (const ce::WorldEntry& we : m_inst.worlds) fTo |= we.name == e.to;
        for (const cane::mods::NewWorld& w : cane::mods::newWorlds(m_proj.manifest)) fTo |= w.name == e.to;
        if (!fTo) add(0, T("Door"), Fmt(T("a way out leads to \"%s\", which is no level"), e.to.c_str()), e.exit > 0 ? e.exit : 0, CF_RemoveEntry, -1, glm::vec3(e.at[0], e.at[1], e.at[2]));
        else if (e.exit > 0 && !setOid.count(e.exit)) add(0, T("Door"), Fmt(T("a way out uses exit %d, which is not in the level"), e.exit), 0, CF_RemoveEntry);
    }
    for (const cane::mods::MapState& s : cane::mods::mapStates(m_proj.manifest))
    {
        if (s.world != GuestWorldName()) continue;
        for (const cane::mods::StateCond& c : s.when)
            if (c.kind == cane::mods::StateCond::Task && !setOid.count(c.oid))
                add(0, T("Lock"), Fmt(T("state \"%s\" waits for task %d, which is not in the level: what it locks never opens"), s.name.c_str(), c.oid), 0);
        for (int oid : s.remove)
            if (!setOid.count(oid)) add(1, T("Lock"), Fmt(T("state \"%s\" takes out %d, which is not in the level"), s.name.c_str(), oid), 0);
    }
    // ---- characters from other levels
    for (const cane::mods::Guest& g : GuestsHere())
    {
        bool fFrom = false;
        for (const ce::WorldEntry& we : m_inst.worlds) fFrom |= we.name == g.from && we.record < 44;
        if (!fFrom) add(0, T("Character"), Fmt(T("%s comes from \"%s\", which is no installed level"), g.name.c_str(), g.from.c_str()), 0, CF_RemoveGuest, -1, glm::vec3(g.at[0], g.at[1], g.at[2]));
        if (g.patrol >= 0 && !setOid.count(g.patrol))
            add(0, T("Character"), Fmt(T("%s patrols path %d, which is not in the level"), g.name.c_str(), g.patrol), 0, CF_GuestNoPatrol, -1, glm::vec3(g.at[0], g.at[1], g.at[2]));
        glm::vec3 hit;
        if (!FGroundBelow(glm::vec3(g.at[0], g.at[1], g.at[2]) + glm::vec3(0, 0, 100.0f), hit))
            add(1, T("Character"), Fmt(T("%s has no ground under it"), g.name.c_str()), 0, CF_FloorUnder, -1, glm::vec3(g.at[0], g.at[1], g.at[2]));
    }
#endif
    // ---- objects far outside the level (beyond 60000 of where nearly all of its placed objects are: the 2nd .. 98th
    // percentile on each axis)
    {
        const std::vector<PLACE> apl = PlacesOfAll();
        std::vector<float> ax[3];
        for (const PLACE& pl : apl)
        {
            const glm::vec3 p = PosWorld(pl.ploLive);
            for (int k = 0; k < 3; ++k) ax[k].push_back(p[k]);
        }
        if (apl.size() >= 10)
        {
            glm::vec3 lo, hi;
            for (int k = 0; k < 3; ++k)
            {
                std::sort(ax[k].begin(), ax[k].end());
                lo[k] = ax[k][ax[k].size() * 2 / 100] - 60000.0f;
                hi[k] = ax[k][ax[k].size() * 98 / 100] + 60000.0f;
            }
            if (std::getenv("SLY2N_CHECKPRINT"))
                std::printf("editor: check bounds %.0f %.0f %.0f .. %.0f %.0f %.0f, %zu placed\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, apl.size());
            for (const PLACE& pl : apl)
            {
                if (TypeOfLo(pl.ploLive) == OT_Sky) continue;   // (a sky dome is wherever it is)
                const glm::vec3 p = PosWorld(pl.ploLive);
                if ((p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z))
                    add(1, T("Outside"), Fmt(T("%s is far outside the level (%.0f, %.0f, %.0f)"), NameOfObject(pl.ploLive).c_str(), p.x, p.y, p.z), pl.ploLive->oid, CF_MoveToView);
            }
        }
    }
    int cErr = 0;
    for (const CHECK& c : m_acheck) cErr += c.sev == 0;
    Status(Fmt(T("Check level: %d problems, %d warnings"), cErr, int(m_acheck.size()) - cErr));
}

// the placed objects (a proxy's instance once, the level's own top-level objects with a matrix)
std::vector<PLACE> CEditor::PlacesOfAll()
{
    std::vector<PLACE> a;
    std::set<LO*> seen;
    for (const auto& up : m_psw->aploAll)
    {
        LO* p = up.get();
        if (p->paloParent || !p->fInWorld) continue;
        PLACE pl;
        if (FPlace(p, pl) && pl.ploLive && seen.insert(pl.ploLive).second) a.push_back(pl);
    }
    return a;
}

bool CEditor::FApplyCheckFix(int i)
{
    if (i < 0 || i >= int(m_acheck.size())) return false;
    const CHECK c = m_acheck[std::size_t(i)];
    std::string error;
    bool f = false;
    switch (c.fix)
    {
    case CF_RemoveOption:
        f = FRemoveOption(c.ifield, std::string(T("Fix")) + ": " + c.text);
        break;
#if EDGAME_SLY2
    case CF_DropLinks:
        if (LO* pt = PloOid(c.oid, EDCID(0x85)))
        {
            std::set<int> setOid;
            for (const auto& up : m_psw->aploAll) setOid.insert(up->oid);
            TASKLINKS tl;
            if (FTaskLinks(pt, tl))
            {
                std::vector<int> aPre, aPost;
                for (int o : tl.aoidPre) if (setOid.count(o)) aPre.push_back(o);
                for (int o : tl.aoidPost) if (setOid.count(o)) aPost.push_back(o);
                f = FSetTaskLinks(pt, aPre, aPost, std::string(T("Fix")) + ": " + c.text);
            }
        }
        break;
    case CF_FloorUnder:
    {
        glm::vec3 p = c.pos - glm::vec3(0, 0, 150.0f);
        BRUSH b;
        b.shape = 4;
        b.size = glm::vec3(1200.0f, 1200.0f, 32.0f);
        b.rgb = 0x8FA0B8;
        const int oid = CreateBrush(b, &p, error);
        if (LO* pf = oid ? PloOid(oid) : nullptr)
        {
            PLACE pl;
            if (FPlace(pf, pl)) { SetPlaceWorld(pl, glm::translate(glm::mat4(1.0f), p)); FRecordPlace(pl, std::string(T("Fix")) + ": " + T("a floor under it")); }
            f = true;
        }
        break;
    }
    case CF_SpawnDown:
    {
        auto aw = cane::mods::newWorlds(m_proj.manifest);
        for (auto& w : aw)
            if (w.name == GuestWorldName()) { w.spawn[0] = c.pos.x; w.spawn[1] = c.pos.y; w.spawn[2] = c.pos.z + 100.0f; }
        cane::mods::setNewWorlds(m_proj.manifest, aw);
        f = FSaveManifestOnly(error);
        break;
    }
    case CF_RemoveEntry:
    {
        auto ae = cane::mods::entries(m_proj.manifest);
        const auto n = ae.size();
        ae.erase(std::remove_if(ae.begin(), ae.end(), [&](const cane::mods::Entry& e) {
            return e.world == GuestWorldName() && ((c.oid > 0 && e.exit == c.oid) || (c.oid == 0 && glm::distance(glm::vec3(e.at[0], e.at[1], e.at[2]), c.pos) < 1.0f));
        }), ae.end());
        cane::mods::setEntries(m_proj.manifest, ae);
        f = ae.size() != n && FSaveManifestOnly(error);
        break;
    }
    case CF_RemoveGuest:
    case CF_GuestNoPatrol:
    {
        auto a = GuestsHere();
        for (std::size_t k = 0; k < a.size(); ++k)
            if (glm::distance(glm::vec3(a[k].at[0], a[k].at[1], a[k].at[2]), c.pos) < 1.0f)
            {
                if (c.fix == CF_RemoveGuest) a.erase(a.begin() + std::ptrdiff_t(k));
                else a[k].patrol = -1;
                f = true;
                break;
            }
        if (f) PutGuests(a);
        break;
    }
#endif
    case CF_MoveToView:
        if (LO* p = PloOid(c.oid))
        {
            PLACE pl;
            if (FPlace(p, pl))
            {
                SetPlaceWorld(pl, glm::translate(glm::mat4(1.0f), PosViewGround()) * glm::mat4(glm::mat3(MatPlace(pl))));
                f = FRecordPlace(pl, std::string(T("Fix")) + ": " + c.text);
            }
        }
        break;
    default: break;
    }
    if (!f && !error.empty()) Status(error);
    std::printf("editor: check fix %d (%s): %s\n", i, c.text.c_str(), f ? "done" : "not done");
    CheckLevel();
    return f;
}

void CEditor::DrawCheckPanel()
{
    if (!m_fCheckOpen) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, size.x * 0.6f), std::min(420.0f, size.y * 0.5f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.35f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin((std::string(T("Check level")) + "###checklevel").c_str(), &m_fCheckOpen, ImGuiWindowFlags_NoSavedSettings)) { ImGui::End(); return; }
    int cErr = 0;
    for (const CHECK& c : m_acheck) cErr += c.sev == 0;
    ImGui::TextColored(st::vec(cErr ? st::danger : st::success), "%s", Fmt(T("%d problems, %d warnings"), cErr, int(m_acheck.size()) - cErr).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Check again"))) CheckLevel();
    ImGui::BeginChild("checks");
    for (std::size_t i = 0; i < m_acheck.size(); ++i)
    {
        const CHECK& c = m_acheck[i];
        ImGui::PushID(int(i));
        ImGui::TextColored(st::vec(c.sev == 0 ? st::danger : st::cta), "%s", c.sev == 0 ? T("problem") : T("warning"));
        ImGui::SameLine(80);
        ImGui::TextColored(st::vec(st::muted), "%s", c.cat.c_str());
        ImGui::SameLine(190);
        if (ImGui::Selectable(c.text.c_str(), false, 0, ImVec2(ImGui::GetContentRegionAvail().x - (c.fix ? 60 : 0), 0)))
        {
            if (LO* p = c.oid ? PloOid(c.oid) : nullptr) { SelectOnly(p); FrameSelected(); }
            else if (c.pos != glm::vec3(0)) { m_cam.pos = c.pos - m_cam.Dir() * 1200.0f; }
        }
        if (c.fix)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Fix"))) { FApplyCheckFix(int(i)); ImGui::PopID(); break; }
        }
        ImGui::PopID();
    }
    if (m_acheck.empty()) ImGui::TextColored(st::vec(st::success), "%s", T("Nothing found: the level should load and play."));
    ImGui::EndChild();
    ImGui::End();
}

bool CEditor::FCheckVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "checklevel")
    {
        CheckLevel();
        for (std::size_t i = 0; i < m_acheck.size(); ++i)
            std::printf("editor: check %zu %s [%s] %s%s\n", i, m_acheck[i].sev == 0 ? "problem" : "warning", m_acheck[i].cat.c_str(), m_acheck[i].text.c_str(), m_acheck[i].fix ? " (fix)" : "");
        return true;
    }
    if (verb == "checkfix") { if (m_acheck.empty()) CheckLevel(); FApplyCheckFix(std::atoi(arg.c_str())); return true; }   // checkfix:I (of the last check)
    if (verb == "checkpanel") { m_fCheckOpen = arg != "0"; if (m_fCheckOpen) CheckLevel(); return true; }
    return false;
}

}  // namespace edimpl
