// The map editor's map states (notes/EDITOR_PLAN.md step 9; the game side: Sly2-Release/mapstates.h, platform/
// mod_worlds.h, editor/core/newworld.h, notes "Map states and new worlds"). The States tab lists the world's states (the
// start, before / after each job, the episode complete, a job being played, the project's own), each with its trigger
// in plain words and the task states it sets, and what decides them in the level (FMapStatesOfWorld). "Show" runs the
// game hidden in that state (--map-state, --dump-presence; its saves are never written) and hides in the view the
// objects that are not in the world then; "Play" starts the play test in it. The project's own states (mod.json
// "map_states") are made and changed here: a trigger (a task's states, a job done or not, a progress value) and the
// objects it adds / removes (the selection) and the state machines it sets. Sly 2 only; Sly 3 says "not yet".
#if EDGAME_SLY2
#include "mapstates.h"
#include <regex>
#include "../../editor/core/newworld.h"
#endif

namespace edimpl
{

#if EDGAME_SLY2
namespace
{
std::string WorldNameOf(const ce::Installation& inst, int record)
{
    const ce::WorldEntry* pe = inst.world(record);
    return pe ? pe->name : std::to_string(record);
}
const char* SzTaskStateWord(int n)
{
    switch (n)
    {
    case 0: return "locked";
    case 1: return "available";
    case 2: return "done";
    case 3: return "done and closed";
    default: return "?";
    }
}
}  // namespace
#endif

#if EDGAME_SLY2
namespace
{
// a translated template with %s filled in order
std::string StrFill(const char* szTemplate, std::initializer_list<std::string> aval)
{
    std::string out, t = szTemplate;
    auto it = aval.begin();
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] == '%' && i + 1 < t.size() && t[i + 1] == 's' && it != aval.end()) { out += *it++; ++i; }
        else out += t[i];
    }
    return out;
}
// the game's state names and triggers (English, mapstates.cpp) in the user's language
template <class ROW> void TranslateStateRow(ROW& r)
{
    static const std::regex s_reFirst(R"(first task (.*)\)$)"), s_reLast(R"(its last task (.*) done, and every task before it\)$)");
    std::smatch m;
    if (r.id == "start")
    {
        r.name = T("Start of the episode");
        r.trigger = r.trigger.rfind("a fresh save", 0) == 0 ? T("a fresh save (no task of this world done)") : T("before any job (a fresh episode: no task done)");
    }
    else if (r.id == "complete")
    {
        r.name = T("Episode complete");
        r.trigger = T("once every task of the episode is done");
    }
    else if (r.id.rfind("during:", 0) == 0 && r.name.rfind("Playing ", 0) == 0)
    {
        const std::string job = r.name.substr(8);
        const std::string task = std::regex_search(r.trigger, m, s_reFirst) ? m[1].str() : std::string("?");
        r.name = StrFill(T("Playing %s"), {job});
        r.trigger = StrFill(T("while the job %s is played (the game started at its first task %s)"), {job, task});
    }
    else if (r.id.rfind("after:", 0) == 0 && r.name.rfind("After ", 0) == 0)
    {
        const std::string job = r.name.substr(6);
        const std::string task = std::regex_search(r.trigger, m, s_reLast) ? m[1].str() : std::string("?");
        r.name = StrFill(T("After %s"), {job});
        r.trigger = StrFill(T("once the job %s is done (its last task %s done, and every task before it)"), {job, task});
    }
}
}  // namespace
#endif

void CEditor::BuildStates()
{
    m_astate.clear();
    m_astateWhy.clear();
    m_fStatesBuilt = true;
#if EDGAME_SLY2
    std::vector<MAPSTATEINFO> a;
    std::vector<std::string> alines;
    if (m_psw && FMapStatesOfWorld(m_psw.get(), m_eo.record, a, &alines))
        for (const MAPSTATEINFO& s : a)
        {
            STATEROW r;
            r.id = s.id;
            r.name = s.name;
            r.trigger = s.trigger;
            r.kind = s.kind;
            r.oidTaskStart = s.oidTaskStart;
            r.from = s.after;
            for (const auto& [oid, n] : s.atask) r.atask.push_back({int(oid), n});
            TranslateStateRow(r);
            m_astate.push_back(r);
        }
    m_astateWhy = alines;
    // the project's own states (mod.json), as "custom:<id>"
    for (const cane::mods::MapState& s : ce::mapStates(m_proj, WorldNameOf(m_inst, m_eo.record)))
    {
        if (std::any_of(m_astate.begin(), m_astate.end(), [&](const STATEROW& r) { return r.id == "custom:" + s.id; })) continue;
        STATEROW r;
        r.id = "custom:" + s.id;
        r.name = s.name.empty() ? s.id : s.name;
        r.kind = "custom";
        r.trigger = StrStateWhen(s);
        m_astate.push_back(r);
    }
#endif
}

#if EDGAME_SLY2
// a custom state's conditions in words
std::string CEditor::StrStateWhen(const cane::mods::MapState& s) const
{
    if (s.when.empty()) return T("always");
    std::string w;
    for (const cane::mods::StateCond& c : s.when)
    {
        if (!w.empty()) w += std::string(" ") + T("and") + " ";
        LO* p = c.oid >= 0 ? PloOid(c.oid) : nullptr;
        const std::string name = p ? NameOfObject(p) : std::to_string(c.oid);
        if (c.kind == cane::mods::StateCond::Task)
        {
            std::string st;
            for (int n : c.states) st += (st.empty() ? "" : " / ") + std::string(T(SzTaskStateWord(n)));
            w += std::string(T("task")) + " " + name + " " + T("is") + " " + st;
        }
        else if (c.kind == cane::mods::StateCond::Job) w += std::string(T("job")) + " " + name + " " + (c.done ? T("is done") : T("is not done"));
        else w += std::string(T("progress")) + " " + c.path + " = " + std::to_string(c.value);
    }
    return w;
}
#endif

// the game run hidden in a state: the objects in the world at its end (the presence dump), polled each frame
void CEditor::ShowState(int istate)
{
#if EDGAME_SLY2
    RestoreStatePoses();
    m_astatePose.clear();
    m_astateSm.clear();
    if (istate < 0 || istate >= int(m_astate.size()))
    {
        m_istateShown = -1;
        m_setStateOut.clear();
        m_statePending.clear();
        InvalidateVis();
        Status(T("all objects shown (every state's)"));
        return;
    }
    const STATEROW& r = m_astate[std::size_t(istate)];
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    const cane::fs::path dir = cane::userDirectory() / "Editor" / "States";
    std::error_code ec;
    cane::fs::create_directories(dir, ec);
    const std::string file = ce::utf8(dir / ("presence_" + std::to_string(m_eo.record) + "_" + std::to_string(std::rand() % 1000000) + ".json"));
    std::string world = WorldArg(m_eo.record), state = r.id;
    if (r.kind == "job" && r.oidTaskStart > 0)
    {
        world += ":" + std::to_string(r.oidTaskStart);
        state = r.from.empty() ? std::string("start") : r.from;
    }
    std::vector<std::string> args{"--data-dir", ce::utf8(m_inst.dir), "--user-dir", cane::utf8(cane::userDirectory()), "--world", world, "--map-state", state,
                                  "--headless", "--fixed-dt", "--frames", "120", "--dump-presence", file};
    // (the project as a package: its saved state, its own states)
    if (cane::fs::exists(m_proj.dir / "mod.json", ec)) { args.push_back("--mod-package"); args.push_back(ce::utf8(m_proj.dir)); }
    m_istateShown = istate;
    m_statePending = file;
    if (m_fPlayDry) { std::printf("editor: state show: --world %s --map-state %s\n", world.c_str(), state.c_str()); return; }
    if (!ce::spawnDetached(exe, args)) { Status(std::string(T("could not start")) + " " + ce::utf8(exe)); m_statePending.clear(); return; }
    Status(std::string(T("loading the state")) + " " + r.name + " ...");
#else
    (void)istate;
#endif
}

void CEditor::PollState()
{
#if EDGAME_SLY2
    if (m_statePending.empty()) return;
    if (++m_statePoll % 20 != 0) return;
    std::vector<uint8_t> ab;
    if (!ce::readFile(ce::pathOf(m_statePending), ab) || ab.empty()) return;
    const nlohmann::json j = nlohmann::json::parse(ab.begin(), ab.end(), nullptr, false);
    if (!j.is_object() || !j.contains("objects")) return;   // (still being written)
    // an oid is out when none of its copies is in (a clone's oid repeats)
    std::map<int, bool> mpin;
    for (const auto& o : j["objects"])
    {
        const int oid = o.value("oid", 0);
        const bool fIn = o.value("in", true);
        auto [it, fNew] = mpin.emplace(oid, fIn);
        if (!fNew) it->second = it->second || fIn;
    }
    m_setStateOut.clear();
    for (const auto& [oid, fIn] : mpin)
        if (!fIn) m_setStateOut.insert(oid);
    m_astateTask.clear();
    if (j.contains("tasks"))
        for (const auto& t : j["tasks"]) m_astateTask.push_back({t.value("task", 0), t.value("state", 0)});
    // the state machines' states and the poses they give
    RestoreStatePoses();
    m_astatePose.clear();
    m_astateSm.clear();
    if (j.contains("sm") && j["sm"].is_array())
        for (const auto& o : j["sm"]) m_astateSm.push_back({o.value("object", 0), o.value("sm", 0), o.value("state", -1)});
    if (j.contains("poses") && j["poses"].is_array())
        for (const auto& o : j["poses"])
            if (o.contains("xf") && o["xf"].is_array())
                for (const auto& x : o["xf"])
                {
                    if (!x.contains("mat") || !x["mat"].is_array() || x["mat"].size() != 16) continue;
                    STATEPOSE sp;
                    sp.oid = o.value("oid", 0);
                    sp.order = o.value("order", 0);
                    sp.ixf = x.value("i", 0);
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r) sp.mat[c][r] = x["mat"][std::size_t(c * 4 + r)].get<float>();
                    m_astatePose.push_back(sp);
                }
    std::error_code ec;
    cane::fs::remove(ce::pathOf(m_statePending), ec);
    m_statePending.clear();
    InvalidateVis();
    const std::string name = m_istateShown >= 0 && m_istateShown < int(m_astate.size()) ? m_astate[std::size_t(m_istateShown)].name : std::string();
    Status(std::string(T("state")) + " " + name + ": " + std::to_string(m_setStateOut.size()) + " " + T("objects not in the world (hidden)"));
#endif
}

// the shown state's poses onto the live objects' records (each frame: cheap once applied; again after a reload)
void CEditor::ApplyStatePoses()
{
#if EDGAME_SLY2
    if (m_pswPosed && m_pswPosed != m_psw.get()) { m_mpPoseOrig.clear(); m_pswPosed = nullptr; }   // (reloaded: the records are new)
    if (m_astatePose.empty() || !m_fStatePose || m_axisDrag >= 0) { RestoreStatePoses(); return; }
    if (m_pswPosed == m_psw.get()) return;
    std::map<int, std::vector<LO*>> mpaplo;
    for (const auto& up : m_psw->aploAll) mpaplo[up->oid].push_back(up.get());
    int c = 0;
    for (const STATEPOSE& sp : m_astatePose)
    {
        const auto it = mpaplo.find(sp.oid);
        if (it == mpaplo.end() || sp.order >= int(it->second.size())) continue;
        LO* plo = it->second[std::size_t(sp.order)];
        if (!FIsLoDerivedFrom(plo, EDCID(0x30))) continue;
        ALO* palo = static_cast<ALO*>(plo);
        if (sp.ixf < 0 || sp.ixf >= int(palo->axf.size()) || !palo->axf[std::size_t(sp.ixf)]) continue;
        XF* pxf = palo->axf[std::size_t(sp.ixf)].get();
        if (!m_mpPoseOrig.count(pxf)) m_mpPoseOrig[pxf] = {palo, pxf->mat};
        pxf->mat = sp.mat;
        UpdateAloHierarchyWorlds(palo);
        ++c;
    }
    m_pswPosed = m_psw.get();
    if (c) UpdateSceneFromSw(m_scene, m_apalo);
#endif
}

void CEditor::RestoreStatePoses()
{
#if EDGAME_SLY2
    if (!m_mpPoseOrig.empty() && m_pswPosed == m_psw.get())
    {
        for (auto& [pxf, o] : m_mpPoseOrig)
        {
            pxf->mat = o.second;
            UpdateAloHierarchyWorlds(o.first);
        }
        UpdateSceneFromSw(m_scene, m_apalo);
    }
#endif
    m_mpPoseOrig.clear();
    m_pswPosed = nullptr;
}

bool CEditor::FStateHides(const LO* plo) const { return m_fStateHide && !m_setStateOut.empty() && plo && m_setStateOut.count(plo->oid) != 0; }

#if EDGAME_SLY2
void CEditor::PlayInState(int istate)
{
    if (istate < 0 || istate >= int(m_astate.size())) return;
    const STATEROW& r = m_astate[std::size_t(istate)];
    if (r.kind == "job" && r.oidTaskStart > 0) m_playMapState = r.from.empty() ? std::string("start") : r.from;
    else m_playMapState = r.id;
    PlayTestAt(r.kind == "job" ? r.oidTaskStart : -1, nullptr, 0.0f);
    m_playMapState.clear();
}

bool CEditor::FSaveStates(std::string& error)
{
    std::error_code ec;
    if (!cane::fs::exists(m_proj.dir / "mod.json", ec)) return FSave();
    m_proj.manifest.dir = m_proj.dir;
    if (!cane::mods::writeManifest(m_proj.manifest, error)) return false;
    BuildStates();
    return true;
}
#endif

void CEditor::DrawStates()
{
#if EDGAME_SLY2
    if (!m_fStatesBuilt) BuildStates();
    ImGui::TextColored(st::vec(st::muted), "%s", T("A level changes as the story goes on: these are its states. Show one to see which objects are there then; Play starts the game in it (no save is written)."));
    ImGui::Checkbox(T("Hide what is not there in the shown state"), &m_fStateHide);
    if (ImGui::IsItemEdited()) InvalidateVis();
    ImGui::SameLine();
    ImGui::Checkbox(T("Pose its state machines"), &m_fStatePose);
    EdTip(T("Objects moved by a state machine (a sign that has fallen, a broken fountain) are shown as they are in the shown state. Moving one puts it back to its loaded pose while you drag."));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Show every object"))) ShowState(-1);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Refresh"))) BuildStates();
    if (!m_statePending.empty()) { ImGui::SameLine(); ImGui::TextColored(st::vec(st::cta), "%s", T("loading the state...")); }
    const float wList = ImGui::GetContentRegionAvail().x * 0.58f;
    ImGui::BeginChild("##statelist", ImVec2(wList, 0), ImGuiChildFlags_Border);
    if (ImGui::BeginTable("##states", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn(T("State"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(T("When"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        // (the buttons' columns as wide as their words in the user's language)
        const float pad = ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f;
        ImGui::TableSetupColumn("##show", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(T("Show")).x + pad);
        ImGui::TableSetupColumn("##play", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(T("Play")).x + pad);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < m_astate.size(); ++i)
        {
            const STATEROW& r = m_astate[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool fShown = m_istateShown == int(i);
            if (ImGui::Selectable(r.name.c_str(), fShown || m_istateEdit == int(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                m_istateEdit = int(i);
            if (fShown) { ImGui::SameLine(); ImGui::TextColored(st::vec(st::cta), "%s", T("(shown)")); }
            ImGui::TableNextColumn();
            ImGui::TextColored(st::vec(st::text2), "%s", r.trigger.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(T("Show"))) ShowState(int(i));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(T("Play"))) PlayInState(int(i));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::TreeNode(T("What decides them in this level")))
    {
        for (const std::string& s : m_astateWhy) ImGui::BulletText("%s", s.c_str());
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##stateedit", ImVec2(0, 0), ImGuiChildFlags_Border);
    DrawStateEditor();
    ImGui::EndChild();
#else
    ImGui::TextDisabled("%s", "Map states: not for " EDGAME_NAME " yet");
#endif
}

#if EDGAME_SLY2
// the selected state: a progress state's task states; a custom state's trigger and objects (editable)
void CEditor::DrawStateEditor()
{
    const STATEROW* pr = m_istateEdit >= 0 && m_istateEdit < int(m_astate.size()) ? &m_astate[std::size_t(m_istateEdit)] : nullptr;
    if (pr && pr->kind != "custom")
    {
        ImGui::SeparatorText(pr->name.c_str());
        ImGui::TextWrapped("%s", pr->trigger.c_str());
        for (const auto& [oid, n] : pr->atask)
        {
            LO* p = PloOid(oid);
            ImGui::BulletText("%s %s: %s", T("task"), p ? NameOfObject(p).c_str() : std::to_string(oid).c_str(), T(SzTaskStateWord(n)));
        }
        if (ImGui::Button(T("New state from this one")))
        {
            // a custom state triggered as this one is (its tasks' states), for the objects you add / remove
            m_stateNew = cane::mods::MapState();
            m_stateNew.world = WorldNameOf(m_inst, m_eo.record);
            m_stateNew.id = "state-" + std::to_string(std::rand() % 100000);
            m_stateNew.name = pr->name + " (" + T("mine") + ")";
            int cTasks = 0;
            for (const auto& [oid, n] : pr->atask)
                if (n >= 2 && cTasks < 1)   // (the last done task of the progress point is enough: its job's end)
                {
                    cane::mods::StateCond c;
                    c.oid = oid;
                    c.states = {2, 3};
                    m_stateNew.when = {c};
                }
            m_fStateNewOpen = true;
        }
        EdTip(T("A state of your own (saved in the project's mod.json) with the same trigger: then add or remove objects in it."));
        return;
    }
    // a custom state, or a new one
    if (pr && pr->kind == "custom" && !m_fStateNewOpen)
        for (const cane::mods::MapState& s : ce::mapStates(m_proj, WorldNameOf(m_inst, m_eo.record)))
            if ("custom:" + s.id == pr->id) { m_stateNew = s; m_fStateNewOpen = true; }
    if (!m_fStateNewOpen)
    {
        ImGui::TextColored(st::vec(st::muted), "%s", T("Select a state, or make your own:"));
        if (ImGui::Button(T("New state")))
        {
            m_stateNew = cane::mods::MapState();
            m_stateNew.world = WorldNameOf(m_inst, m_eo.record);
            m_stateNew.id = "state-" + std::to_string(std::rand() % 100000);
            m_stateNew.name = T("My state");
            m_fStateNewOpen = true;
        }
        return;
    }
    cane::mods::MapState& s = m_stateNew;
    char sz[128];
    std::snprintf(sz, sizeof(sz), "%s", s.name.c_str());
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText(T("Name"), sz, sizeof(sz))) s.name = sz;
    ImGui::TextColored(st::vec(st::muted), "%s: %s", T("When"), StrStateWhen(s).c_str());
    // the conditions
    for (std::size_t i = 0; i < s.when.size(); ++i)
    {
        cane::mods::StateCond& c = s.when[i];
        ImGui::PushID(int(i));
        int k = int(c.kind);
        ImGui::SetNextItemWidth(90);
        const char* const aszKind[] = {"Task", "Job", "Progress"};
        if (ImGui::Combo("##k", &k, [](void* p, int i2) { return T(static_cast<const char* const*>(p)[i2]); }, (void*)aszKind, 3)) c.kind = cane::mods::StateCond::Kind(k);
        ImGui::SameLine();
        if (c.kind != cane::mods::StateCond::Progress)
        {
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("##oid", &c.oid, 0);
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Selected")) && m_plSel) c.oid = m_plSel->oid;
            if (c.kind == cane::mods::StateCond::Task)
                for (int n = 0; n < 4; ++n)
                {
                    bool f = std::find(c.states.begin(), c.states.end(), n) != c.states.end();
                    ImGui::SameLine();
                    if (ImGui::Checkbox(T(SzTaskStateWord(n)), &f))
                    {
                        c.states.erase(std::remove(c.states.begin(), c.states.end(), n), c.states.end());
                        if (f) c.states.push_back(n);
                    }
                }
            else { ImGui::SameLine(); ImGui::Checkbox(T("done"), &c.done); }
        }
        else
        {
            char szp[96];
            std::snprintf(szp, sizeof(szp), "%s", c.path.c_str());
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputTextWithHint("##path", "a.b.c", szp, sizeof(szp))) c.path = szp;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::InputInt("=##v", &c.value, 0);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { s.when.erase(s.when.begin() + std::ptrdiff_t(i)); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(T("Add a condition")))
    {
        cane::mods::StateCond c;
        c.kind = cane::mods::StateCond::Job;
        c.oid = m_plSel ? m_plSel->oid : -1;
        s.when.push_back(c);
    }
    // the objects
    auto list = [&](const char* label, std::vector<int>& a, const char* tip) {
        ImGui::SeparatorText(label);
        EdTip(tip);
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            LO* p = PloOid(a[i]);
            ImGui::PushID(label);
            ImGui::PushID(int(i));
            if (ImGui::SmallButton("x")) { a.erase(a.begin() + std::ptrdiff_t(i)); ImGui::PopID(); ImGui::PopID(); break; }
            ImGui::SameLine();
            if (ImGui::Selectable(p ? NameOfObject(p).c_str() : std::to_string(a[i]).c_str()) && p) SelectOnly(p);
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::PushID(label);
        if (ImGui::SmallButton(T("Add the selection")))
            for (LO* p : m_asel)
                if (std::find(a.begin(), a.end(), p->oid) == a.end()) a.push_back(p->oid);
        ImGui::PopID();
    };
    list(T("Added in this state"), s.add, T("Objects that exist only while the state holds (taken out otherwise)."));
    list(T("Removed in this state"), s.remove, T("Objects taken out while the state holds."));
    ImGui::SeparatorText(T("State machines"));
    for (std::size_t i = 0; i < s.sm.size(); ++i)
    {
        ImGui::PushID(int(i) + 1000);
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt(T("object"), &s.sm[i].object, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt(T("machine"), &s.sm[i].sm, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt(T("state"), &s.sm[i].state, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { s.sm.erase(s.sm.begin() + std::ptrdiff_t(i)); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(T("Add a state machine"))) s.sm.push_back({m_plSel ? m_plSel->oid : -1, -1, -1});
    ImGui::Separator();
    if (ImGui::Button(T("Save the state")))
    {
        std::string error;
        ce::putMapState(m_proj, s);
        if (!FSaveStates(error)) Status(std::string(T("state")) + ": " + error);
        else Status(std::string(T("saved the state")) + " " + s.name + " (mod.json)");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Delete")))
    {
        std::string error;
        ce::removeMapState(m_proj, s.world, s.id);
        FSaveStates(error);
        m_fStateNewOpen = false;
        m_istateEdit = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Close"))) { m_fStateNewOpen = false; m_istateEdit = -1; }
}
#endif

// --editor-do verbs (states)
bool CEditor::FStatesVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "tab" && arg == "states") { m_tab = 10; return true; }
#if EDGAME_SLY2
    if (verb == "states")   // the world's states (id, name, trigger)
    {
        BuildStates();
        for (const STATEROW& r : m_astate) std::printf("editor: state %s | %s | %s\n", r.id.c_str(), r.name.c_str(), r.trigger.c_str());
        for (const std::string& s : m_astateWhy) std::printf("editor: state decided by: %s\n", s.c_str());
        return true;
    }
    if (verb == "stateshow")   // stateshow:ID (none: every object)
    {
        if (m_astate.empty()) BuildStates();
        int i = -1;
        for (std::size_t k = 0; k < m_astate.size(); ++k)
            if (m_astate[k].id == arg) i = int(k);
        if (i < 0 && !arg.empty()) Status("stateshow: no state " + arg);
        ShowState(i);
        return true;
    }
    if (verb == "statewait")   // until the shown state's objects are known
    {
        if (!m_statePending.empty()) { m_iscript--; m_waitScript = 10; m_statePoll = 19; }
        else
        {
            std::printf("editor: state shown: %d objects out\n", int(m_setStateOut.size()));
            int cHidden = 0;
            for (const auto& up : m_psw->aploAll) cHidden += FStateHides(up.get());
            std::printf("editor: state hides %d objects in the view\n", cHidden);
            std::printf("editor: state poses %zu records (%zu state machines)\n", m_astatePose.size(), m_astateSm.size());
        }
        return true;
    }
    if (verb == "statepos")   // statepos:OID: the object's place in the view (with the shown state's pose)
    {
        ApplyStatePoses();
        if (LO* p = PloOid(std::atoi(arg.c_str())))
        {
            const glm::vec3 v = PosWorld(p);
            std::printf("editor: state pos %s %.1f %.1f %.1f\n", arg.c_str(), v.x, v.y, v.z);
        }
        return true;
    }
    if (verb == "statesm")   // the shown state's state machines (object, machine, state)
    {
        for (const auto& a : m_astateSm) std::printf("editor: state sm object %d machine %d state %d\n", a[0], a[1], a[2]);
        return true;
    }
    if (verb == "stateisout")   // stateisout:OID
    {
        std::printf("editor: state object %s %s\n", arg.c_str(), m_setStateOut.count(std::atoi(arg.c_str())) ? "out" : "in");
        return true;
    }
    if (verb == "stateplay")   // stateplay:ID (with playdry: the command printed)
    {
        if (m_astate.empty()) BuildStates();
        for (std::size_t k = 0; k < m_astate.size(); ++k)
            if (m_astate[k].id == arg) PlayInState(int(k));
        return true;
    }
    if (verb == "statenew")   // statenew:ID:NAME:job=OID|task=OID: a custom state; then stateadd / stateremove / statesave
    {
        std::stringstream ss(arg);
        std::string id, name, when;
        std::getline(ss, id, ':');
        std::getline(ss, name, ':');
        std::getline(ss, when, ':');
        m_stateNew = cane::mods::MapState();
        m_stateNew.world = WorldNameOf(m_inst, m_eo.record);
        m_stateNew.id = id;
        m_stateNew.name = name;
        if (when.rfind("job=", 0) == 0 || when.rfind("task=", 0) == 0)
        {
            cane::mods::StateCond c;
            c.kind = when[0] == 'j' ? cane::mods::StateCond::Job : cane::mods::StateCond::Task;
            c.oid = std::atoi(when.c_str() + when.find('=') + 1);
            c.states = {2, 3};
            m_stateNew.when.push_back(c);
        }
        m_fStateNewOpen = true;
        return true;
    }
    if (verb == "stateadd" || verb == "stateremove")
    {
        std::vector<int>& a = verb == "stateadd" ? m_stateNew.add : m_stateNew.remove;
        for (LO* p : m_asel) a.push_back(p->oid);
        return true;
    }
    if (verb == "statesave")
    {
        std::string error;
        ce::putMapState(m_proj, m_stateNew);
        if (!FSaveStates(error)) Status("statesave: " + error);
        else std::printf("editor: saved state %s: %s\n", m_stateNew.id.c_str(), StrStateWhen(m_stateNew).c_str());
        return true;
    }
#endif
    return false;
}

}  // namespace edimpl
