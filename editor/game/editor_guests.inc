// The map editor's characters from other levels (notes/EDITOR_PLAN.md round 5, item 1; the game side:
// Sly2-Release/modguests.h, platform/mod_worlds.h "guests", docs/MODDING.md "Characters from other levels"). A guard,
// boss or NPC of another level comes in with its behaviour: the project's mod.json lists it (its level, its object
// there, its place, heading and patrol path in this level) and the game loads its own level aside and makes a live copy
// of it here (class, animations, AI, sounds, effects). The editor lists them in the Assets tab, draws them in the view
// (a ring, the name, the way it faces, its patrol path) and the play test shows them acting. Sly 2 only.

namespace edimpl
{

#if EDGAME_SLY2
namespace
{
// the character a shared group of another level holds (its root's oid, -1: none): a guard / boss / NPC (0x38 family),
// another step class (0x33) or Sly / Bentley / Murray (0x34)
int OidCharacterOf(const ce::GroupInfo& g, int* pcid)
{
    for (const ce::ObjRecord& r : g.objects)
        if (r.cid > 0 && r.cid < CID_Max &&
            (FCidDerivedFrom(r.cid, 0x38) || FCidDerivedFrom(r.cid, 0x34) || FCidDerivedFrom(r.cid, 0x33)))
        {
            if (pcid) *pcid = r.cid;
            return r.oid;
        }
    return -1;
}
}  // namespace

std::string CEditor::GuestWorldName() const
{
    const ce::WorldEntry* pe = m_inst.world(m_eo.record);
    return pe ? pe->name : std::string();
}

std::vector<cane::mods::Guest> CEditor::GuestsHere() const
{
    std::vector<cane::mods::Guest> a;
    const std::string world = GuestWorldName();
    for (const cane::mods::Guest& g : cane::mods::guests(m_proj.manifest))
        if (g.world == world) a.push_back(g);
    return a;
}

void CEditor::PutGuests(const std::vector<cane::mods::Guest>& aHere)
{
    const std::string world = GuestWorldName();
    std::vector<cane::mods::Guest> a;
    for (const cane::mods::Guest& g : cane::mods::guests(m_proj.manifest))
        if (g.world != world) a.push_back(g);
    a.insert(a.end(), aHere.begin(), aHere.end());
    cane::mods::setGuests(m_proj.manifest, a);
    std::string error;
    if (!FSaveManifestOnly(error)) Status(error);
}

// another level's group's character brought here at the view's point, facing the camera
bool CEditor::FAddGuest(int record, const std::string& group, std::string& error)
{
    const ce::WorldEntry* pe = m_inst.world(record);
    if (!pe) { error = "no level " + std::to_string(record); return false; }
    if (record >= 44) { error = T("only the game's own levels can lend characters"); return false; }
    ce::WorldIndex wi;
    if (!wi.load(m_inst, *pe, error)) return false;
    int oid = -1, cid = 0;
    for (const ce::GroupInfo& g : wi.groups)
        if (g.name == group && !g.level) oid = OidCharacterOf(g, &cid);
    if (oid < 0) { error = std::string(T("no character in")) + " " + group; return false; }
    auto a = GuestsHere();
    cane::mods::Guest g;
    g.world = GuestWorldName();
    g.from = pe->name;
    g.object = oid;
    g.name = PrettyGroupName(group);
    int n = 1;
    auto taken = [&](const std::string& id) {
        for (const cane::mods::Guest& x : cane::mods::guests(m_proj.manifest))
            if (x.world == g.world && x.id == id) return true;
        return false;
    };
    do g.id = cane::mods::idFromName(group) + "-" + std::to_string(n++);
    while (taken(g.id));
    const glm::vec3 p = PosViewGround();
    std::copy(&p.x, &p.x + 3, g.at);
    const glm::vec3 d = m_cam.Dir();
    g.heading = std::round(glm::degrees(std::atan2(-d.y, -d.x)));   // (facing the camera)
    a.push_back(g);
    PutGuests(a);
    m_iguest = int(a.size()) - 1;
    Status(std::string(T("brought in with its behaviour")) + ": " + g.name + " (" + pe->name + ")");
    std::printf("editor: guest %s from %s object %d (cid 0x%X) at %.0f %.0f %.0f heading %.0f\n", g.id.c_str(), g.from.c_str(), oid, cid, p.x, p.y,
                p.z, g.heading);
    return true;
}

// the guests in the view: a ring at the feet, the name, the way it faces; its patrol path in its colour
void CEditor::DrawGuests()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const auto a = GuestsHere();
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const cane::mods::Guest& g = a[i];
        const glm::vec3 c(g.at[0], g.at[1], g.at[2]);
        const uint32_t col = int(i) == m_iguest ? st::u32(st::cta) : IM_COL32(240, 80, 110, 230);
        ImVec2 prev{};
        bool fPrev = false;
        for (int k = 0; k <= 24; ++k)
        {
            const float t = 6.2831853f * float(k) / 24.0f;
            ImVec2 s;
            const bool f = FProject(c + glm::vec3(std::cos(t) * 60.0f, std::sin(t) * 60.0f, 0), s);
            if (f && fPrev) dl->AddLine(prev, s, col, 2.0f);
            prev = s;
            fPrev = f;
        }
        const float rad = glm::radians(g.heading);
        ImVec2 s0, s1, sTop;
        if (FProject(c, s0) && FProject(c + glm::vec3(std::cos(rad), std::sin(rad), 0) * 110.0f, s1)) dl->AddLine(s0, s1, col, 3.0f);
        if (FProject(c + glm::vec3(0, 0, 220.0f), sTop))
        {
            dl->AddLine(s0, sTop, col, 1.0f);
            dl->AddText(ImVec2(sTop.x + 4, sTop.y - 14), col, (g.name + " (" + g.from + ")").c_str());
        }
        if (g.patrol >= 0)
            if (LO* pp = PloOid(g.patrol)) DrawCurve(pp, int(i) == m_iguest ? 0xFFB020 : 0xF0506E, 2.5f);
    }
}

// the Assets tab's list of this level's guests
void CEditor::DrawGuestsPanel()
{
    auto a = GuestsHere();
    char head[160];
    std::snprintf(head, sizeof(head), "%s (%zu)###guests", T("Characters from other levels in this level"), a.size());
    if (!ImGui::CollapsingHeader(head, a.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (a.empty())
    {
        ImGui::TextColored(st::vec(st::muted), "%s", T("Another level: right-click a guard, boss or NPC and choose Bring in with its behaviour."));
        return;
    }
    bool fChanged = false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        cane::mods::Guest& g = a[i];
        ImGui::PushID(int(i));
        if (ImGui::Selectable((g.name + " (" + g.from + ")").c_str(), m_iguest == int(i), 0, ImVec2(220, 0))) m_iguest = int(i);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("At the view"))) { const glm::vec3 p = PosViewGround(); std::copy(&p.x, &p.x + 3, g.at); fChanged = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Look at it")))
        {
            m_cam.yaw = 45.0f;
            m_cam.pitch = -25.0f;
            m_cam.pos = glm::vec3(g.at[0], g.at[1], g.at[2] + 100.0f) - m_cam.Dir() * 800.0f;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat(T("heading"), &g.heading, 1.0f, -360.0f, 360.0f, "%.0f");
        fChanged |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (g.patrol >= 0)
        {
            LO* pp = PloOid(g.patrol);
            ImGui::Text("%s: %s", T("patrol"), pp ? NameOfObject(pp).c_str() : std::to_string(g.patrol).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(T("No patrol"))) { g.patrol = -1; fChanged = true; }
        }
        else ImGui::TextColored(st::vec(st::muted), "%s", T("stands at its place"));
        ImGui::SameLine();
        const bool fPath = m_plSel && FIsLoDerivedFrom(m_plSel, EDCID(0x0A));
        ImGui::BeginDisabled(!fPath);
        if (ImGui::SmallButton(T("Patrol the selected path"))) { g.patrol = m_plSel->oid; fChanged = true; }
        ImGui::EndDisabled();
        EdTip(T("Select a path in this level (Create: paths), then press this: the guard walks along it and back."));
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { a.erase(a.begin() + std::ptrdiff_t(i)); fChanged = true; m_iguest = -1; ImGui::PopID(); break; }
        ImGui::PopID();
    }
    ImGui::TextColored(st::vec(st::muted), "%s", T("The play test shows them acting: their own level's classes, animations, AI, sounds and effects come with them."));
    if (fChanged) PutGuests(a);
}

bool CEditor::FGuestVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "guestadd")   // guestadd:RECORD:GROUP (the group's name without FK$X)
    {
        const std::size_t c = arg.find(':');
        std::string error;
        if (c == std::string::npos || !FAddGuest(std::atoi(arg.c_str()), arg.substr(c + 1), error)) Status("guestadd: " + error);
        return true;
    }
    if (verb == "guests")
    {
        for (const cane::mods::Guest& g : GuestsHere())
            std::printf("editor: guest %s (%s) from %s object %d at %.0f %.0f %.0f heading %.0f patrol %d\n", g.id.c_str(), g.name.c_str(), g.from.c_str(), g.object,
                        g.at[0], g.at[1], g.at[2], g.heading, g.patrol);
        return true;
    }
    auto edit = [&](const std::function<void(cane::mods::Guest&, const std::string&)>& fn) {
        const std::size_t c = arg.find(':');
        auto a = GuestsHere();
        const int i = std::atoi(arg.c_str());
        if (i < 0 || i >= int(a.size())) { Status(verb + ": no guest " + std::to_string(i)); return true; }
        fn(a[std::size_t(i)], c == std::string::npos ? std::string() : arg.substr(c + 1));
        PutGuests(a);
        return true;
    };
    if (verb == "guestpatrol") return edit([](cane::mods::Guest& g, const std::string& v) { g.patrol = v.empty() ? -1 : std::atoi(v.c_str()); });   // guestpatrol:I:PATH
    if (verb == "guestheading") return edit([](cane::mods::Guest& g, const std::string& v) { g.heading = float(std::atof(v.c_str())); });       // guestheading:I:DEG
    if (verb == "guestat")   // guestat:I[:X,Y,Z] (default: the view's point)
        return edit([&](cane::mods::Guest& g, const std::string& v) {
            glm::vec3 p = PosViewGround();
            if (!v.empty()) std::sscanf(v.c_str(), "%f,%f,%f", &p.x, &p.y, &p.z);
            std::copy(&p.x, &p.x + 3, g.at);
        });
    if (verb == "guestdel")
    {
        auto a = GuestsHere();
        const int i = std::atoi(arg.c_str());
        if (i >= 0 && i < int(a.size())) a.erase(a.begin() + i);
        PutGuests(a);
        return true;
    }
    return false;
}
#else
void CEditor::DrawGuests() {}
void CEditor::DrawGuestsPanel() {}
bool CEditor::FGuestVerb(const std::string&, const std::string&) { return false; }
#endif

}  // namespace edimpl
