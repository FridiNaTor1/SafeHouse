// The map editor's brand-new levels (notes/EDITOR_PLAN.md step 11; the game side: editor/core/newworld.h,
// platform/mod_worlds.h, Sly2-Release/editor_newworld.h, docs/MODDING.md "New levels"). The Level tab: the project's new
// levels (open one), "New level..." (a name, a template level, empty or a whole copy, where the player starts, a way in
// from another level and a way back), this level's ways in and out (mod.json "entries": spheres drawn in the view,
// moved to the view's point, their radius and destination) and a new level's spawn point. Sly 2 only.
#if EDGAME_SLY2
#include "editor_newworld.h"
#endif

namespace edimpl
{

#if EDGAME_SLY2
namespace
{
std::string ThisWorldName(const ce::Installation& inst, int record)
{
    const ce::WorldEntry* pe = inst.world(record);
    return pe ? pe->name : std::string();
}
}  // namespace

// a new level made in the project (saved at once: the game and a new editor read it from the package)
bool CEditor::FCreateLevel(std::string& error)
{
    ce::NewWorldSpec spec;
    spec.name = m_nlName;
    spec.templateRecord = m_nlTemplate;
    spec.emptyLevel = !m_nlKeepLevel;
    spec.hasSpawn = true;
    std::copy(m_nlSpawn, m_nlSpawn + 4, spec.spawn);
    if (const ce::WorldEntry* pe = m_inst.world(m_nlEntryWorld)) spec.episode = pe->episode;
    cane::mods::NewWorld w;
    if (spec.name.empty()) { error = T("give the level a name"); return false; }
    if (!ce::addNewWorld(m_proj, m_inst, spec, &w, error)) return false;
    if (m_nlEntryWorld >= 0)
    {
        cane::mods::Entry e;
        e.world = ThisWorldName(m_inst, m_nlEntryWorld);
        e.to = w.name;
        std::copy(m_nlEntryAt, m_nlEntryAt + 3, e.at);
        e.radius = m_nlEntryAt[3];
        ce::addEntry(m_proj, e);
        if (m_nlReturn)
        {
            cane::mods::Entry r;
            r.world = w.name;
            r.to = e.world;
            std::copy(m_nlReturnAt, m_nlReturnAt + 3, r.at);
            r.radius = m_nlReturnAt[3];
            ce::addEntry(m_proj, r);
        }
    }
    if (!m_proj.save(error)) return false;
    RegisterProjectWorlds(m_inst, ce::utf8(m_proj.dir));
    for (const ce::WorldEntry& we : m_inst.worlds)
        if (we.name == w.name) m_nlMadeRecord = we.record;
    Status(std::string(T("made the level")) + " " + w.name + " (" + T("world") + " " + std::to_string(m_nlMadeRecord) + ", " + T("from") + " " + w.templateWorld + ")");
    return true;
}

// another world of the installation or the project opened in a new editor (this one closes: the project is saved)
void CEditor::OpenLevel(int record)
{
    std::string error;
    if (m_proj.dirty() && !FSave()) return;
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    std::vector<std::string> args{"--data-dir", ce::utf8(m_inst.dir), "--user-dir", cane::utf8(cane::userDirectory()), "--editor", std::to_string(record),
                                  "--editor-project", ce::utf8(m_proj.dir)};
    // (a level just made empty: a floor under its spawn and the view on it, so there is ground to build on)
    if (record == m_nlMadeRecord && !m_nlKeepLevel && m_nlFloor)
    {
        char sz[256];
        std::snprintf(sz, sizeof(sz), "floorat:%.0f,%.0f,%.0f,4000,4000;cam:%.0f,%.0f,%.0f,90,-30;save", m_nlSpawn[0], m_nlSpawn[1], m_nlSpawn[2] - 100.0f, m_nlSpawn[0],
                      m_nlSpawn[1] - 1200.0f, m_nlSpawn[2] + 700.0f);
        args.insert(args.end(), {"--editor-do", sz});
    }
    if (m_fPlayDry) { std::printf("editor: open level: --editor %d --editor-project %s\n", record, ce::utf8(m_proj.dir).c_str()); return; }
    if (!ce::spawnDetached(exe, args)) { Status("could not start " + ce::utf8(exe)); return; }
    m_fQuit = true;
}

bool CEditor::FSaveManifestOnly(std::string& error)
{
    std::error_code ec;
    if (!cane::fs::exists(m_proj.dir / "mod.json", ec)) return FSave();
    m_proj.manifest.dir = m_proj.dir;
    return cane::mods::writeManifest(m_proj.manifest, error);
}

// the entries (ways in / out) of this world: drawn as circles on the ground with their destination
void CEditor::DrawEntries()
{
    const std::string world = ThisWorldName(m_inst, m_eo.record);
    if (world.empty()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const auto ae = cane::mods::entries(m_proj.manifest);
    for (const cane::mods::Entry& e : ae)
    {
        if (e.world != world || e.exit >= 0) continue;
        const glm::vec3 c(e.at[0], e.at[1], e.at[2]);
        ImVec2 prev{}, first{};
        bool fPrev = false;
        for (int i = 0; i <= 32; ++i)
        {
            const float t = 6.2831853f * float(i) / 32.0f;
            ImVec2 s;
            const bool f = FProject(c + glm::vec3(std::cos(t) * e.radius, std::sin(t) * e.radius, 0), s);
            if (f && fPrev) dl->AddLine(prev, s, st::u32(st::cta, 0.9f), 2.0f);
            prev = s;
            fPrev = f;
            if (i == 0) first = s;
        }
        ImVec2 s;
        if (FProject(c, s)) dl->AddText(s, st::u32(st::cta), (std::string("-> ") + e.to).c_str());
    }
    // a new world's spawn point
    for (const cane::mods::NewWorld& w : cane::mods::newWorlds(m_proj.manifest))
        if (w.name == world && w.hasSpawn)
        {
            ImVec2 s;
            if (FProject(glm::vec3(w.spawn[0], w.spawn[1], w.spawn[2]), s))
            {
                dl->AddCircleFilled(s, 6.0f, st::u32(st::success));
                dl->AddText(ImVec2(s.x + 8, s.y - 8), st::u32(st::success), T("spawn"));
            }
        }
}

#endif

glm::vec3 CEditor::PosViewGround()
{
    glm::vec3 p = m_cam.pos + m_cam.Dir() * 600.0f, hit;
    if (FSnapDown(p, hit)) p = hit;
    return p;
}

void CEditor::DrawLevelTab()
{
#if EDGAME_SLY2
    const std::string world = ThisWorldName(m_inst, m_eo.record);
    ImGui::TextColored(st::vec(st::muted), "%s", T("Levels of your own: made from a template level (empty, or a whole copy to change), reached through a way in from another level. They load on every platform."));
    // the project's new levels
    const auto aw = cane::mods::newWorlds(m_proj.manifest);
    if (ImGui::Button(T("New level..."))) { m_fNewLevelOpen = true; m_nlEntryWorld = m_eo.record; const glm::vec3 p = PosViewGround(); m_nlEntryAt[0] = p.x; m_nlEntryAt[1] = p.y; m_nlEntryAt[2] = p.z; }
    for (const cane::mods::NewWorld& w : aw)
    {
        ImGui::PushID(w.id.c_str());
        int record = -1;
        for (const ce::WorldEntry& we : m_inst.worlds)
            if (we.name == w.name) record = we.record;
        ImGui::BulletText("%s (%s %s, %s %d)%s", w.name.c_str(), T("from"), w.templateWorld.c_str(), T("world"), record, w.name == world ? T("  - this one") : "");
        if (w.name != world && record >= 0) { ImGui::SameLine(); if (ImGui::SmallButton(T("Open"))) OpenLevel(record); }
        ImGui::PopID();
    }
    // this level's spawn (a new level)
    for (const cane::mods::NewWorld& w : aw)
        if (w.name == world)
        {
            ImGui::SeparatorText(T("Where the player starts"));
            ImGui::Text("%.0f, %.0f, %.0f", w.spawn[0], w.spawn[1], w.spawn[2]);
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Put it at the view's point")))
            {
                cane::mods::NewWorld w2 = w;
                const glm::vec3 p = PosViewGround() + glm::vec3(0, 0, 100.0f);
                w2.spawn[0] = p.x; w2.spawn[1] = p.y; w2.spawn[2] = p.z; w2.hasSpawn = true;
                auto a = aw;
                for (auto& x : a) if (x.id == w.id) x = w2;
                cane::mods::setNewWorlds(m_proj.manifest, a);
                std::string error;
                FSaveManifestOnly(error);
            }
        }
    // the ways in and out
    ImGui::SeparatorText(T("Ways in and out of this level"));
    auto ae = cane::mods::entries(m_proj.manifest);
    bool fChanged = false;
    for (std::size_t i = 0; i < ae.size(); ++i)
    {
        cane::mods::Entry& e = ae[i];
        if (e.world != world && e.to != world) continue;
        ImGui::PushID(int(i));
        if (e.world == world)
        {
            ImGui::Text("%s %s", T("To"), e.to.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            fChanged |= ImGui::DragFloat3("##at", e.at, 4.0f, -100000.0f, 100000.0f, "%.0f") && ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::DragFloat(T("radius"), &e.radius, 2.0f, 30.0f, 5000.0f, "%.0f");
            fChanged |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            if (ImGui::SmallButton(T("At the view"))) { const glm::vec3 p = PosViewGround(); e.at[0] = p.x; e.at[1] = p.y; e.at[2] = p.z; fChanged = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { ae.erase(ae.begin() + std::ptrdiff_t(i)); fChanged = true; ImGui::PopID(); break; }
        }
        else ImGui::TextColored(st::vec(st::text2), "%s %s (%.0f, %.0f, %.0f)", T("From"), e.world.c_str(), e.at[0], e.at[1], e.at[2]);
        ImGui::PopID();
    }
    if (ImGui::SmallButton(T("Add a way out here")) && !aw.empty())
    {
        cane::mods::Entry e;
        e.world = world;
        e.to = aw[0].name == world ? (aw.size() > 1 ? aw[1].name : std::string("Paris")) : aw[0].name;
        const glm::vec3 p = PosViewGround();
        e.at[0] = p.x; e.at[1] = p.y; e.at[2] = p.z;
        ae.push_back(e);
        fChanged = true;
    }
    EdTip(T("A sphere at the view's point that takes the player to another level (change its destination in mod.json or with the list)."));
    if (fChanged)
    {
        cane::mods::setEntries(m_proj.manifest, ae);
        std::string error;
        if (!FSaveManifestOnly(error)) Status(error);
    }
    DrawNewLevelDialog();
#else
    ImGui::TextDisabled("%s", "New levels: not for " EDGAME_NAME " yet");
#endif
}

#if EDGAME_SLY2
void CEditor::DrawNewLevelDialog()
{
    if (m_fNewLevelOpen) { ImGui::OpenPopup("##newlevel"); m_fNewLevelOpen = false; }
    if (!ImGui::BeginPopupModal("##newlevel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(T("New level"));
    ImGui::Separator();
    char sz[96];
    std::snprintf(sz, sizeof(sz), "%s", m_nlName.c_str());
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputText(T("Name"), sz, sizeof(sz))) m_nlName = sz;
    auto worldCombo = [&](const char* label, int& record) {
        const ce::WorldEntry* pe = m_inst.world(record);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo(label, pe ? pe->name.c_str() : T("(none)")))
        {
            for (const ce::WorldEntry& we : m_inst.worlds)
                if (we.indexBase.empty() && we.kind == 0 && ImGui::Selectable((we.name + "  (" + std::to_string(we.record) + ")").c_str(), we.record == record)) record = we.record;
            ImGui::EndCombo();
        }
    };
    worldCombo(T("Template level"), m_nlTemplate);
    EdTip(T("Its characters, effects, HUD and textures are used; empty: its geometry, props, guards and jobs are left out."));
    ImGui::Checkbox(T("Keep the whole template (a copy to change)"), &m_nlKeepLevel);
    if (!m_nlKeepLevel) ImGui::Checkbox(T("Start with a floor under the spawn"), &m_nlFloor);
    ImGui::SetNextItemWidth(260);
    ImGui::DragFloat4(T("Spawn (x, y, z, heading)"), m_nlSpawn, 4.0f, -100000.0f, 100000.0f, "%.0f");
    ImGui::SeparatorText(T("The way in"));
    worldCombo(T("From the level"), m_nlEntryWorld);
    ImGui::SetNextItemWidth(260);
    ImGui::DragFloat4(T("At (x, y, z, radius)"), m_nlEntryAt, 4.0f, -100000.0f, 100000.0f, "%.0f");
    if (m_nlEntryWorld == m_eo.record) { ImGui::SameLine(); if (ImGui::SmallButton(T("The view's point"))) { const glm::vec3 p = PosViewGround(); m_nlEntryAt[0] = p.x; m_nlEntryAt[1] = p.y; m_nlEntryAt[2] = p.z; } }
    ImGui::Checkbox(T("And a way back from the new level"), &m_nlReturn);
    if (m_nlReturn) { ImGui::SetNextItemWidth(260); ImGui::DragFloat4(T("Back at (x, y, z, radius)"), m_nlReturnAt, 4.0f, -100000.0f, 100000.0f, "%.0f"); }
    ImGui::Separator();
    if (ImGui::Button(T("Make it")))
    {
        std::string error;
        if (!FCreateLevel(error)) Status(std::string(T("new level")) + ": " + error);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (m_nlMadeRecord >= 0 && ImGui::Button(T("Open it"))) { ImGui::CloseCurrentPopup(); OpenLevel(m_nlMadeRecord); }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
#endif

// --editor-do verbs (new levels)
bool CEditor::FLevelVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "tab" && arg == "level") { m_tab = 11; return true; }
#if EDGAME_SLY2
    if (verb == "newlevel")   // newlevel:NAME:TEMPLATE[:keep]: with the way in at the view's point of this level and a way back
    {
        std::stringstream ss(arg);
        std::string name, tmpl, keep;
        std::getline(ss, name, ':');
        std::getline(ss, tmpl, ':');
        std::getline(ss, keep, ':');
        m_nlName = name;
        m_nlTemplate = std::atoi(tmpl.c_str());
        m_nlKeepLevel = keep == "keep";
        m_nlEntryWorld = m_eo.record;
        const glm::vec3 p = PosViewGround();
        m_nlEntryAt[0] = p.x; m_nlEntryAt[1] = p.y; m_nlEntryAt[2] = p.z;
        std::string error;
        if (!FCreateLevel(error)) Status("newlevel: " + error);
        else std::printf("editor: new level %s is world %d\n", name.c_str(), m_nlMadeRecord);
        return true;
    }
    if (verb == "entries")
    {
        for (const cane::mods::Entry& e : cane::mods::entries(m_proj.manifest))
            std::printf("editor: entry in %s at %.0f %.0f %.0f r %.0f to %s\n", e.world.c_str(), e.at[0], e.at[1], e.at[2], e.radius, e.to.c_str());
        return true;
    }
    if (verb == "newleveldlg") { m_tab = 11; m_fNewLevelOpen = true; m_nlEntryWorld = m_eo.record; return true; }
    if (verb == "openlevel") { OpenLevel(std::atoi(arg.c_str())); return true; }
    if (verb == "floorat")   // floorat:X,Y,Z,W,D: a floor brush whose top is at Z (a new level's ground)
    {
        float a[5] = {0, 0, 100, 4000, 4000};
        std::stringstream ss(arg);
        std::string t;
        for (int i = 0; i < 5 && std::getline(ss, t, ','); ++i) a[i] = float(std::atof(t.c_str()));
        BRUSH b;
        b.shape = 4;
        b.size = glm::vec3(a[3], a[4], 32.0f);
        b.rgb = 0x8FA0B8;
        const glm::vec3 p(a[0], a[1], a[2]);
        std::string error;
        const int oid = CreateBrush(b, &p, error);
        if (LO* pf = oid ? PloOid(oid) : nullptr)
        {
            PLACE pl;
            if (FPlace(pf, pl)) { SetPlaceWorld(pl, glm::translate(glm::mat4(1.0f), p)); FRecordPlace(pl, std::string(T("Floor")) + " " + std::to_string(oid)); }
            std::printf("editor: floor %d top at %.0f\n", oid, PosWorld(pf).z);
        }
        else Status("floorat: " + error);
        return true;
    }
    if (verb == "spawnhere")   // this new level's spawn at the view's point (on the ground, 100 above)
    {
        const std::string world = ThisWorldName(m_inst, m_eo.record);
        auto aw = cane::mods::newWorlds(m_proj.manifest);
        const glm::vec3 p = PosViewGround() + glm::vec3(0, 0, 100.0f);
        for (auto& w : aw)
            if (w.name == world) { w.spawn[0] = p.x; w.spawn[1] = p.y; w.spawn[2] = p.z; w.hasSpawn = true; }
        cane::mods::setNewWorlds(m_proj.manifest, aw);
        std::string error;
        FSaveManifestOnly(error);
        std::printf("editor: spawn at %.0f %.0f %.0f\n", p.x, p.y, p.z);
        return true;
    }
#endif
    return false;
}

}  // namespace edimpl
