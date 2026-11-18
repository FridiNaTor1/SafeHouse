// The map editor's object types (notes/EDITOR_PLAN.md): every object gets a type from its class (and, for the guards'
// family, its shared group's name), a friendly name, an icon and a vis group; the visibility of types, vis groups and
// objects (hide / show / isolate); the outliner; the selection of several objects (click + Ctrl / Shift, box select,
// select by type), their duplicate / delete / copy / paste; snapping; the tool strip and the status bar.
// Included by editor_shell.inc (both games; the class numbers are Sly 2's through EDCID).

namespace edimpl
{

// (InfoOfType, SzVisGroup, Lower, PrettyGroupName: editor_objtypes.h)

std::string CEditor::GroupNameOf(const LO* plo) const
{
    return plo && plo->igrp >= 0 && plo->igrp < int(m_index.groups.size()) ? m_index.groups[std::size_t(plo->igrp)].name : std::string();
}

OBJTYPE CEditor::TypeOfLo(const LO* plo) const
{
    if (!plo) return OT_Other;
    if (const auto it = m_mptype.find(plo); it != m_mptype.end()) return it->second;
    const bool fLevel = plo->igrp >= 0 && plo->igrp < int(m_index.groups.size()) && m_index.groups[std::size_t(plo->igrp)].level;
    const OBJTYPE ot = OtOfLo(plo, Lower(GroupNameOf(plo)), fLevel);   // (editor_objtypes.h)
    m_mptype[plo] = ot;
    return ot;
}

std::string CEditor::NameOfObject(const LO* plo) const
{
    if (!plo) return std::string();
    const OBJTYPE ot = TypeOfLo(plo);
    // a job: its name in the world's text
    if (ot == OT_Job)
    {
        int id = 0;
        if (FOptionInt(const_cast<LO*>(plo), EDEOPID(1245), id) && id > 0)
            if (const std::string s = StrText(id); !s.empty()) return s;
    }
    // a brush: its shape
    for (const BRUSH& b : m_abrush)
        if (b.oid == plo->oid)
        {
            static const char* const s_a[] = {"Block", "Ramp", "Stairs", "Cylinder", "Floor", "Crawl tunnel", "Mesh"};
            return std::string(T("Brush")) + ": " + T(s_a[std::clamp(b.shape, 0, 6)]) + " " + std::to_string(plo->oid);
        }
    // an instance's root (or a shared group's object): the group's name as words (the characters' own names)
    const LO* root = plo;
    while (root->paloParent) root = root->paloParent;
    const bool fLevel = plo->igrp >= 0 && plo->igrp < int(m_index.groups.size()) && m_index.groups[std::size_t(plo->igrp)].level;
    if (!fLevel && root == plo)
        if (const std::string g = GroupNameOf(plo); !g.empty())
        {
            // the group's model (a character, a prop) by the group's name; its other objects (sensors, paths, ...)
            // by their type and the group's name
            if (FIsLoDerivedFrom(const_cast<LO*>(plo), EDCID(0x30))) return PrettyGroupName(g);
            return std::string(T(InfoOfType(ot).name)) + " (" + PrettyGroupName(g) + ")";
        }
    // the class's own name when the port knows it (Ledge, Pipe, Rail, Use, ...), else the type's
    if (plo->Cid() == EDCID(0x1B)) return std::string(T("Crawl space")) + " " + std::to_string(plo->oid);
    if (ot == OT_Traversal || ot == OT_Path || ot == OT_Sensor || ot == OT_Door || ot == OT_Button || ot == OT_Volume || ot == OT_Trigger)
        if (const char* sz = SzClass(plo->Cid()); sz && std::strncmp(sz, "CID_", 4) != 0)
        {
            std::string n = sz;
            for (std::size_t i = 1; i < n.size(); ++i) n[i] = char(std::tolower(static_cast<unsigned char>(n[i])));
            return n + " " + std::to_string(plo->oid);
        }
    return std::string(T(InfoOfType(ot).name)) + " " + std::to_string(plo->oid);
}

// ---- visibility

bool CEditor::FHidden(const LO* plo) const
{
    if (!plo) return true;
    if (m_visCached != m_visVersion) { m_mphidden.clear(); m_visCached = m_visVersion; }
    if (const auto it = m_mphidden.find(plo); it != m_mphidden.end()) return it->second;
    bool f = false;
    const OBJTYPE ot = TypeOfLo(plo);
    if (m_afTypeHidden[ot] || m_afVgHidden[InfoOfType(ot).vg] || m_setHidden.count(KeyOfLo(plo)) || FStateHides(plo)) f = true;
    else if (plo->paloParent) f = FHidden(plo->paloParent);
    // isolation: shown only when it or an ancestor is isolated
    if (!f && !m_setIsolate.empty())
    {
        bool fIn = false;
        for (const LO* p = plo; p && !fIn; p = p->paloParent) fIn = m_setIsolate.count(KeyOfLo(p)) != 0;
        f = !fIn;
    }
    m_mphidden[plo] = f;
    return f;
}

void CEditor::ApplyVisibility()
{
    if (m_visApplied != m_visVersion)
    {
        // what was hidden placed again by the scene's own placement (its afVisible as the object's flags say), then
        // the hidden ones taken out
        if (!m_aiHiddenEntries.empty()) UpdateSceneFromSw(m_scene, m_apalo);
        m_aiHiddenEntries.clear();
        for (std::size_t k = 0; k < m_scene.aglobset.size() && k < m_apalo.size(); ++k)
            if (m_apalo[k] && FHidden(m_apalo[k])) m_aiHiddenEntries.push_back(int(k));
        m_visApplied = m_visVersion;
        // the selection keeps only what is shown
        std::vector<LO*> a;
        for (LO* p : m_asel)
            if (!FHidden(p)) a.push_back(p);
        if (a.size() != m_asel.size())
        {
            m_asel = a;
            if (m_plSel && !FSelected(m_plSel)) Select(m_asel.empty() ? nullptr : m_asel.back());
        }
    }
    for (int k : m_aiHiddenEntries)
        if (std::size_t(k) < m_scene.aglobset.size()) std::fill(m_scene.aglobset[std::size_t(k)].afVisible.begin(), m_scene.aglobset[std::size_t(k)].afVisible.end(), 0);
}

void CEditor::HideSelection(bool fHide)
{
    for (LO* p : m_asel)
    {
        if (fHide) m_setHidden.insert(KeyOfLo(p));
        else m_setHidden.erase(KeyOfLo(p));
        m_setIsolate.erase(KeyOfLo(p));
    }
    if (fHide) ClearSelection();
    InvalidateVis();
    SaveViewState();
}

void CEditor::IsolateSelection()
{
    m_setIsolate.clear();
    for (LO* p : m_asel) m_setIsolate.insert(KeyOfLo(p));
    InvalidateVis();
    Status(m_setIsolate.empty() ? std::string(T("Isolation off")) : std::string(T("Isolated: only the selection is shown (View > Show all ends it)")));
}

void CEditor::ShowAll()
{
    m_setHidden.clear();
    m_setIsolate.clear();
    std::fill(std::begin(m_afTypeHidden), std::end(m_afTypeHidden), false);
    std::fill(std::begin(m_afVgHidden), std::end(m_afVgHidden), false);
    InvalidateVis();
    SaveViewState();
}

// the project's editor/view.json (the game ignores the package's editor/ folder): what is hidden, the snapping
void CEditor::SaveViewState()
{
    nlohmann::json j;
    j["grid"] = m_gridSize;
    j["grid_snap"] = m_fGridSnap;
    j["angle"] = m_angleSnap;
    j["angle_snap"] = m_fAngleSnap;
    j["scale"] = m_scaleSnap;
    j["scale_snap"] = m_fScaleSnap;
    j["dock"] = m_fDockOpen;
    nlohmann::json at = nlohmann::json::array(), av = nlohmann::json::array(), ao = nlohmann::json::array();
    for (int i = 0; i < OT_Max; ++i)
        if (m_afTypeHidden[i]) at.push_back(InfoOfType(OBJTYPE(i)).name);
    for (int i = 0; i < VG_Max; ++i)
        if (m_afVgHidden[i]) av.push_back(SzVisGroup(VISGROUP(i)));
    for (uint64_t k : m_setHidden) ao.push_back(std::to_string(k));
    j["hidden_types"] = at;
    j["hidden_groups"] = av;
    j["hidden_objects"] = ao;
    const std::string s = j.dump(1);
    if (s == m_viewStateLoaded) return;
    m_viewStateLoaded = s;
    ce::writeFile(m_proj.dir / "editor" / "view.json", std::vector<uint8_t>(s.begin(), s.end()));
}

void CEditor::LoadViewState()
{
    std::vector<uint8_t> ab;
    if (!ce::readFile(m_proj.dir / "editor" / "view.json", ab)) return;
    m_viewStateLoaded.assign(ab.begin(), ab.end());
    const nlohmann::json j = nlohmann::json::parse(m_viewStateLoaded, nullptr, false);
    if (!j.is_object()) return;
    m_gridSize = j.value("grid", m_gridSize);
    m_fGridSnap = j.value("grid_snap", m_fGridSnap);
    m_angleSnap = j.value("angle", m_angleSnap);
    m_fAngleSnap = j.value("angle_snap", m_fAngleSnap);
    m_scaleSnap = j.value("scale", m_scaleSnap);
    m_fScaleSnap = j.value("scale_snap", m_fScaleSnap);
    m_fDockOpen = j.value("dock", m_fDockOpen);
    if (j.contains("hidden_types") && j["hidden_types"].is_array())
        for (const auto& v : j["hidden_types"])
            for (int i = 0; i < OT_Max; ++i)
                if (v.is_string() && v.get<std::string>() == InfoOfType(OBJTYPE(i)).name) m_afTypeHidden[i] = true;
    if (j.contains("hidden_groups") && j["hidden_groups"].is_array())
        for (const auto& v : j["hidden_groups"])
            for (int i = 0; i < VG_Max; ++i)
                if (v.is_string() && v.get<std::string>() == SzVisGroup(VISGROUP(i))) m_afVgHidden[i] = true;
    if (j.contains("hidden_objects") && j["hidden_objects"].is_array())
        for (const auto& v : j["hidden_objects"])
            if (v.is_string()) m_setHidden.insert(std::strtoull(v.get<std::string>().c_str(), nullptr, 10));
    InvalidateVis();
}

// ---- selection

void CEditor::SyncSelection()
{
    if (!m_plSel) { m_asel.clear(); return; }
    if (!FSelected(m_plSel)) { m_asel.clear(); m_asel.push_back(m_plSel); }
}

void CEditor::ToggleSelected(LO* plo)
{
    if (!plo) return;
    if (FSelected(plo))
    {
        m_asel.erase(std::find(m_asel.begin(), m_asel.end(), plo));
        if (m_plSel == plo) Select(m_asel.empty() ? nullptr : m_asel.back());
    }
    else AddSelected(plo);
}

void CEditor::AddSelected(LO* plo)
{
    if (!plo) return;
    if (!FSelected(plo)) m_asel.push_back(plo);
    Select(plo);
}

bool CEditor::FSelectable(LO* plo) const
{
    if (!plo || FHidden(plo)) return false;
    PLACE pl;
    return FPlace(plo, pl) && pl.ploLive == plo;
}

void CEditor::SelectWhere(const std::function<bool(LO*)>& f, bool fAdd)
{
    if (!fAdd) m_asel.clear();
    LO* plast = nullptr;
    for (const auto& up : m_psw->aploAll)
        if (LO* plo = up.get(); !FHidden(plo) && f(plo) && !FSelected(plo)) { m_asel.push_back(plo); plast = plo; }
    if (plast) Select(plast);
    else if (m_asel.empty()) Select(nullptr);
    else Select(m_asel.back());
    Status(std::to_string(m_asel.size()) + " " + T("selected"));
}

std::vector<PLACE> CEditor::PlacesOfSelection() const
{
    std::vector<PLACE> a;
    std::set<std::size_t> setMat;
    for (LO* p : m_asel)
    {
        PLACE pl;
        if (!FPlace(p, pl) || !setMat.insert(pl.ibMat).second) continue;
        a.push_back(pl);
    }
    // (an object whose ancestor is selected too moves with it: only the outermost)
    std::vector<PLACE> b;
    for (const PLACE& pl : a)
    {
        bool fInner = false;
        for (const LO* q = pl.ploLive->paloParent; q && !fInner; q = q->paloParent)
            for (const PLACE& o : a) fInner |= o.ploLive == q;
        // (attached at the load to a selected one: it follows that one)
        if (const auto it = m_mpLoadParent.find(pl.ploLive); it != m_mpLoadParent.end() && it->second.ploParent)
            for (const PLACE& o : a) fInner |= o.ploLive == it->second.ploParent;
        if (!fInner) b.push_back(pl);
    }
    return b;
}

void CEditor::BoxSelect(const ImVec2& a, const ImVec2& b, bool fAdd)
{
    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    if (!fAdd) m_asel.clear();
    LO* plast = nullptr;
    for (const auto& up : m_psw->aploAll)
    {
        LO* plo = up.get();
        if (!FSelectable(plo) || FSelected(plo)) continue;
        const glm::vec3 p = PosWorld(plo);
        if (glm::length(p) < 0.01f) continue;   // (unplaced templates at the origin)
        ImVec2 s;
        if (!FProject(p, s) || s.x < lo.x || s.x > hi.x || s.y < lo.y || s.y > hi.y) continue;
        // (in front of the camera, not too far)
        if (glm::dot(p - m_cam.pos, m_cam.Dir()) <= 0 || glm::length(p - m_cam.pos) > 30000.0f) continue;
        m_asel.push_back(plo);
        plast = plo;
    }
    if (plast) Select(plast);
    else if (m_asel.empty()) Select(nullptr);
    Status(std::to_string(m_asel.size()) + " " + T("selected"));
}

std::vector<uint64_t> CEditor::KeysOfSelection() const
{
    std::vector<uint64_t> a;
    for (LO* p : m_asel) a.push_back(KeyOfLo(p));
    return a;
}

LO* CEditor::PloOfKey(uint64_t key) const
{
    LO* pbest = nullptr;
    for (const auto& up : m_psw->aploAll)
        if (KeyOfLo(up.get()) == key && (!pbest || (FHasMatrix(up.get()) && !FHasMatrix(pbest)))) pbest = up.get();
    return pbest;
}

void CEditor::SelectKeys(const std::vector<uint64_t>& akey)
{
    m_asel.clear();
    for (uint64_t k : akey)
        if (LO* p = PloOfKey(k); p && !FSelected(p)) m_asel.push_back(p);
    Select(m_asel.empty() ? nullptr : m_asel.back());
}

void CEditor::DuplicateSelection()
{
    if (m_asel.size() <= 1) { Duplicate(); SyncSelection(); return; }
#if !EDFEAT_STRUCT
    Status("duplicate: not for " EDGAME_NAME " yet");
#else
    // each top-level record (or the proxy of an instance) copied once, side by side; one undo step, one reload. (A
    // record's copy is appended to its part: the other records stay where they are.)
    std::vector<int> aiobj;
    std::vector<glm::mat4> aparent;
    std::vector<PLACE> apl;
    for (const PLACE& pl : PlacesOfSelection())
    {
        int iobj = pl.iobjrec;
        if (iobj < 0 || std::find(aiobj.begin(), aiobj.end(), iobj) != aiobj.end()) continue;
        if (!pl.fProxy && m_rec.aobj[std::size_t(iobj)].plo != pl.ploLive) continue;   // (nested: one at a time)
        aiobj.push_back(iobj);
        apl.push_back(pl);
    }
    if (aiobj.empty()) { Status(T("nothing in the selection can be duplicated (top-level objects and instances only)")); return; }
    const float dx = std::max(250.0f, m_fGridSnap ? m_gridSize : 0.0f);
    std::vector<int> aoidNew;
    m_proj.begin(std::string(T("Duplicate")) + " " + std::to_string(aiobj.size()) + " " + T("objects"), true);
    std::string error;
    for (std::size_t i = 0; i < aiobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[std::size_t(aiobj[i])];
        std::string rel;
        std::size_t ibRec = 0, ibNew = 0;
        if (!FLocate(r.ibRecord, r.ibBody - r.ibRecord, rel, ibRec)) continue;
        const int oidNew = OidNew();
        if (!m_proj.duplicateRecord(rel, ibRec, oidNew, &ibNew, error)) break;
        BumpProxiesIn(aiobj[i], 1);
        aoidNew.push_back(oidNew);
        std::string relMat;
        std::size_t ibMat = 0;
        const PLACE& pl = apl[i];
        if (FLocate(pl.ibMat, 48, relMat, ibMat) && relMat == rel && ibMat > ibRec && ibMat < ibRec + r.cb + (r.ibBody - r.ibRecord))
        {
            std::vector<uint8_t> ab;
            m_proj.readPart(rel, ab);
            const std::size_t ibMatNew = ibNew + (ibMat - ibRec);
            glm::mat4 m = MatFromBytes(ab.data() + ibMatNew);
            const glm::mat4 parent = pl.fProxy ? glm::mat4(1.0f) : MatWorld(pl.ploLive) * glm::inverse(MatLocal(pl.ploLive));
            m[3] += glm::inverse(parent) * glm::vec4(dx, 0.0f, 0.0f, 0.0f);
            float a[12];
            if (FBytesFromMat(m, a)) m_proj.patch(rel, ibMatNew, a, 48);
        }
    }
    m_proj.end();
    if (!error.empty()) Status(std::string(T("duplicate")) + ": " + error);
    else Status(std::string(T("duplicated")) + " " + std::to_string(aoidNew.size()) + " " + T("objects"));
    Reload(0);
    // the copies selected (an instance: the root its new proxy places)
    m_asel.clear();
    for (int oid : aoidNew)
        for (const auto& up : m_psw->aploAll)
            if (!up->paloParent && (up->oidInstance == oid || (up->oid == oid && up->oidInstance < 0)) && FHasMatrix(up.get())) { m_asel.push_back(up.get()); break; }
    Select(m_asel.empty() ? nullptr : m_asel.back());
#endif
}

void CEditor::DeleteSelection()
{
    if (m_asel.size() <= 1) { Delete(); SyncSelection(); return; }
#if !EDFEAT_STRUCT
    Status("delete: not for " EDGAME_NAME " yet");
#else
    // the top-level records (proxies of instances), from the end of each part to its start (a removal moves only
    // what follows it); one undo step, one reload
    struct DEL { std::string rel; std::size_t ib; };
    std::vector<DEL> adel;
    std::set<int> setIobj;
    for (LO* plo : m_asel)
    {
        PLACE pl;
        int iobj = FPlace(plo, pl) ? pl.iobjrec : -1;
        if (iobj < 0)
            if (LO* ptop = PloTopLevel(plo)) iobj = m_mpobj.at(ptop).iobjrec;
        if (iobj < 0 || !setIobj.insert(iobj).second) continue;
        const EDITOBJREC& r = m_rec.aobj[std::size_t(iobj)];
        if (r.depth != 0) continue;
        DEL d;
        if (FLocate(r.ibRecord, r.ibBody - r.ibRecord, d.rel, d.ib)) adel.push_back(d);
    }
    if (adel.empty()) { Status(T("nothing in the selection is stored in the world file")); return; }
    std::sort(adel.begin(), adel.end(), [](const DEL& a, const DEL& b) { return a.rel != b.rel ? a.rel < b.rel : a.ib > b.ib; });
    std::string error;
    int c = 0;
    m_proj.begin(std::string(T("Delete")) + " " + std::to_string(adel.size()) + " " + T("objects"), true);
    for (const DEL& d : adel)
    {
        if (!m_proj.deleteRecord(d.rel, d.ib, error)) break;
        ++c;
    }
    m_proj.end();
    Status(error.empty() ? std::string(T("deleted")) + " " + std::to_string(c) + " " + T("objects") : std::string(T("delete")) + ": " + error);
    Reload(0);
    ClearSelection();
#endif
}

void CEditor::CopySelection()
{
    m_aclip.clear();
    for (const PLACE& pl : PlacesOfSelection()) m_aclip.push_back({KeyOfLo(pl.ploLive), PosWorld(pl.ploLive)});
    Status(std::to_string(m_aclip.size()) + " " + T("objects copied (Ctrl+V pastes them in front of the camera, Ctrl+Shift+V in place)"));
}

void CEditor::Paste(bool fInPlace)
{
#if !EDFEAT_STRUCT
    (void)fInPlace;
    Status("paste: not for " EDGAME_NAME " yet");
#else
    if (m_aclip.empty()) { Status(T("nothing copied")); return; }
    glm::vec3 mid(0.0f);
    for (const CLIP& c : m_aclip) mid += c.pos;
    mid /= float(m_aclip.size());
    glm::vec3 at = m_cam.pos + m_cam.Dir() * 800.0f, hit;
    if (FSnapDown(at, hit)) at = hit;
    at = glm::vec3(SnapMove(at.x), SnapMove(at.y), at.z);
    std::vector<uint64_t> akeyNew;
    int c = 0;
    std::string error;
    for (const CLIP& cl : m_aclip)
    {
        LO* plo = PloOfKey(cl.key);
        if (!plo) continue;
        PLACE pl;
        int iobj = -1;
        if (FPlace(plo, pl) && pl.fProxy) iobj = pl.iobjrec;
        else if (LO* ptop = PloTopLevel(plo)) iobj = IobjOfLo(ptop);
        if (iobj < 0) continue;
        const glm::vec3 pos = fInPlace ? cl.pos : at + (cl.pos - mid);
        if (!CopyRecord(iobj, CopyGroup(), &pos, true, error)) break;
        ++c;
        if (m_plSel) akeyNew.push_back(KeyOfLo(m_plSel));
    }
    SelectKeys(akeyNew);
    Status(error.empty() ? std::string(T("pasted")) + " " + std::to_string(c) + " " + T("objects") : std::string(T("paste")) + ": " + error);
#endif
}

// ---- the outliner (vis groups, then the objects by type / group / list)

void CEditor::OutlinerContextMenu(LO* plo)
{
    if (!ImGui::BeginPopupContextItem()) return;
    if (!FSelected(plo)) SelectOnly(plo);
    const OBJTYPE ot = TypeOfLo(plo);
    if (ImGui::MenuItem(T("Frame"), "F")) FrameSelected();
    if (ImGui::MenuItem((std::string(T("Select all")) + ": " + T(InfoOfType(ot).plural)).c_str())) SelectWhere([&](LO* p) { return TypeOfLo(p) == ot && FSelectable(p); }, false);
    ImGui::Separator();
    if (ImGui::MenuItem(T("Hide"), "H")) HideSelection(true);
    if (ImGui::MenuItem(T("Isolate"), "I")) IsolateSelection();
    if (ImGui::MenuItem(T("Show all"), "Alt+H")) ShowAll();
    ImGui::Separator();
    if (ImGui::MenuItem(T("Copy"), "Ctrl+C")) CopySelection();
    if (EDFEAT_STRUCT && ImGui::MenuItem(T("Duplicate"), "Ctrl+D")) DuplicateSelection();
    if (EDFEAT_STRUCT && ImGui::MenuItem(T("Delete"), "Del")) DeleteSelection();
    ImGui::EndPopup();
}

void CEditor::DrawOutlinerRow(LO* plo, const char* idPrefix)
{
    const OBJTYPEINFO& ti = InfoOfType(TypeOfLo(plo));
    const bool fHidden = FHidden(plo);
    char label[200];
    std::snprintf(label, sizeof(label), "##%s%p", idPrefix, static_cast<const void*>(plo));
    const bool fSel = FSelected(plo);
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(fHidden ? st::muted : st::text));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable(label, fSel, ImGuiSelectableFlags_AllowDoubleClick))
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) ToggleSelected(plo);
        else if (io.KeyShift) AddSelected(plo);
        else SelectOnly(plo);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) FrameSelected();
    }
    if (plo == m_plSel && m_fOutScroll) { ImGui::SetScrollHereY(0.4f); m_fOutScroll = false; }
    OutlinerContextMenu(plo);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 1), ImVec2(p0.x + h, p0.y + h + 1), st::u32(ti.rgb, fHidden ? 0.25f : 0.85f), 3.0f);
    const char icon[2] = {ti.icon, 0};
    dl->AddText(ImVec2(p0.x + h * 0.5f - ImGui::CalcTextSize(icon).x * 0.5f, p0.y), st::u32(0x0A0F1C), icon);
    const std::string name = NameOfObject(plo);
    dl->AddText(ImVec2(p0.x + h + 6, p0.y), st::u32(fHidden ? st::muted : st::text), name.c_str());
    char right[64];
    std::snprintf(right, sizeof(right), "%s %d", SzClass(plo->Cid()), plo->oid);
    const float wr = ImGui::CalcTextSize(right).x;
    dl->AddText(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - wr - 4, p0.y), st::u32(st::muted), right);
    ImGui::PopStyleColor();
}

void CEditor::DrawOutliner()
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    const float w = std::clamp(size.x * 0.25f, 330.0f, 470.0f);
    const float h = (size.y - top - 36) * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(size.x - w - 6, top + 6), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin(TL("Outliner", "outliner").c_str(), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    // counts per type (shown or not) and per vis group
    int actype[OT_Max] = {}, acvg[VG_Max] = {};
    for (const auto& up : m_psw->aploAll)
    {
        const OBJTYPE ot = TypeOfLo(up.get());
        ++actype[ot];
        ++acvg[InfoOfType(ot).vg];
    }
    // the vis groups: shown / hidden, their types inside
    if (ImGui::BeginTable("visgroups", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, std::min(h * 0.42f, 300.0f))))
    {
        ImGui::TableSetupColumn("##vis", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthFixed, 48);
        for (int vg = 0; vg < VG_Max; ++vg)
        {
            if (!acvg[vg]) continue;
            ImGui::PushID(vg);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool fShown = !m_afVgHidden[vg];
            if (ImGui::Checkbox("##v", &fShown)) { m_afVgHidden[vg] = !fShown; InvalidateVis(); SaveViewState(); }
            EdTip(T("Show or hide this group in the view"));
            ImGui::TableNextColumn();
            const bool fOpen = ImGui::TreeNodeEx(T(SzVisGroup(VISGROUP(vg))), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem(T("Select all"))) SelectWhere([&](LO* p) { return InfoOfType(TypeOfLo(p)).vg == vg && FSelectable(p); }, false);
                if (ImGui::MenuItem(T("Show only this group")))
                {
                    for (int i = 0; i < VG_Max; ++i) m_afVgHidden[i] = i != vg;
                    InvalidateVis();
                    SaveViewState();
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(st::vec(st::muted), "%d", acvg[vg]);
            if (fOpen)
            {
                for (int ot = 0; ot < OT_Max; ++ot)
                {
                    const OBJTYPEINFO& ti = InfoOfType(OBJTYPE(ot));
                    if (ti.vg != vg || !actype[ot]) continue;
                    ImGui::PushID(100 + ot);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool fTypeShown = !m_afTypeHidden[ot];
                    if (ImGui::Checkbox("##t", &fTypeShown)) { m_afTypeHidden[ot] = !fTypeShown; InvalidateVis(); SaveViewState(); }
                    ImGui::TableNextColumn();
                    ImGui::Indent(10);
                    ImGui::TextColored(st::vec(ti.rgb), "%c", ti.icon);
                    ImGui::SameLine();
                    if (ImGui::Selectable(T(ti.plural), m_outType == ot)) m_outType = m_outType == ot ? -1 : ot;
                    if (ImGui::BeginPopupContextItem())
                    {
                        if (ImGui::MenuItem(T("Select all"))) SelectWhere([&](LO* p) { return TypeOfLo(p) == ot && FSelectable(p); }, false);
                        if (ImGui::MenuItem(T("Show only these")))
                        {
                            for (int i = 0; i < OT_Max; ++i) m_afTypeHidden[i] = i != ot;
                            std::fill(std::begin(m_afVgHidden), std::end(m_afVgHidden), false);
                            InvalidateVis();
                            SaveViewState();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::Unindent(10);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(st::vec(st::muted), "%d", actype[ot]);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // the search and the view of the list
    ImGui::SetNextItemWidth(-130);
    ImGui::InputTextWithHint("##outfilter", T("Search: name, class or oid"), m_outFilter, sizeof(m_outFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    const char* amode[] = {T("By type"), T("By group"), T("List")};
    ImGui::Combo("##outmode", &m_outMode, amode, 3);
    if (m_outType >= 0)
    {
        ImGui::TextColored(st::vec(InfoOfType(OBJTYPE(m_outType)).rgb), "%s", T(InfoOfType(OBJTYPE(m_outType)).plural));
        ImGui::SameLine();
        if (ImGui::SmallButton(T("all types"))) m_outType = -1;
    }
    ImGui::BeginChild("outlist", ImVec2(0, 0), false);
    const std::string filter = Lower(m_outFilter);
    auto match = [&](LO* plo) {
        if (m_outType >= 0 && TypeOfLo(plo) != m_outType) return false;
        if (filter.empty()) return true;
        return Lower(NameOfObject(plo)).find(filter) != std::string::npos || Lower(SzClass(plo->Cid())).find(filter) != std::string::npos ||
               std::to_string(plo->oid) == filter || Lower(T(InfoOfType(TypeOfLo(plo)).name)).find(filter) != std::string::npos;
    };
    if (m_outMode == 1 && filter.empty() && m_outType < 0) DrawTree();
    else if (m_outMode == 0 && filter.empty() && m_outType < 0)
    {
        // by type: one node per type (the selection's opened)
        for (int ot = 0; ot < OT_Max; ++ot)
        {
            if (!actype[ot]) continue;
            const OBJTYPEINFO& ti = InfoOfType(OBJTYPE(ot));
            char label[96];
            std::snprintf(label, sizeof(label), "%s  (%d)###ot%d", T(ti.plural), actype[ot], ot);
            if (m_fOutScroll && m_plSel && TypeOfLo(m_plSel) == ot) ImGui::SetNextItemOpen(true);
            ImGui::PushStyleColor(ImGuiCol_Text, st::vec(m_afTypeHidden[ot] || m_afVgHidden[ti.vg] ? st::muted : st::text2));
            const bool fOpen = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            if (!fOpen) continue;
            int n = 0;
            for (const auto& up : m_psw->aploAll)
                if (TypeOfLo(up.get()) == ot)
                {
                    DrawOutlinerRow(up.get(), "t");
                    if (++n >= 2000) { ImGui::TextDisabled("(%s 2000)", T("first")); break; }
                }
            ImGui::TreePop();
        }
    }
    else
    {
        ImGuiListClipper clip;
        std::vector<LO*> a;
        for (const auto& up : m_psw->aploAll)
            if (match(up.get())) a.push_back(up.get());
        if (m_fOutScroll && m_plSel)
            if (const auto it = std::find(a.begin(), a.end(), m_plSel); it != a.end())
            {
                ImGui::SetScrollY(float(it - a.begin()) * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight() * 0.4f);
                m_fOutScroll = false;
            }
        clip.Begin(int(a.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) DrawOutlinerRow(a[std::size_t(i)], "l");
        if (a.empty()) ImGui::TextDisabled("%s", T("(nothing matches)"));
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- the tool strip (left) and the status bar (bottom)

void CEditor::DrawToolStrip()
{
    const float top = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(6, top + 6), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(52, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 8));
    ImGui::Begin("##tools", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    struct TOOL { const char* label; const char* tip; int gizmo; };
    const TOOL s_atool[] = {
        {T("Sel"), T("Select (Q): click picks, Ctrl+click adds / removes, Shift+click adds, drag in empty space draws a box"), -1},
        {T("Mov"), T("Move (W): drag an arrow; the grid snap is on the status bar"), 0},
        {T("Rot"), T("Rotate (E): drag a ring; the angle snap is on the status bar"), 1},
        {T("Scl"), T("Scale (R): drag a box handle (Shift: all axes)"), 2},
    };
    for (int i = 0; i < 4; ++i)
    {
        const bool fOn = (i == 0 && m_tool == 0) || (i > 0 && m_tool != 0 && m_gizmo == s_atool[i].gizmo);
        ImGui::PushStyleColor(ImGuiCol_Button, st::vec(fOn ? st::accent : st::raised));
        if (ImGui::Button(s_atool[i].label, ImVec2(40, 34)))
        {
            m_tool = i == 0 ? 0 : 1;
            if (i > 0) m_gizmo = s_atool[i].gizmo;
        }
        ImGui::PopStyleColor();
        EdTip(s_atool[i].tip);
    }
    ImGui::Separator();
    if (ImGui::Button(TL("F", "toolframe").c_str(), ImVec2(40, 30))) FrameSelected();
    EdTip(T("Frame the selection (F)"));
    if (ImGui::Button(TL("H", "toolhide").c_str(), ImVec2(40, 30))) HideSelection(true);
    EdTip(T("Hide the selection (H); Alt+H shows everything again"));
    if (ImGui::Button(TL("I", "toolisolate").c_str(), ImVec2(40, 30))) { if (m_setIsolate.empty()) IsolateSelection(); else { m_setIsolate.clear(); InvalidateVis(); } }
    EdTip(T("Isolate the selection (I): only it is shown; again: everything"));
    ImGui::End();
    ImGui::PopStyleVar();
}

void CEditor::DrawStatusBar()
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, size.y - 30), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(size.x, 30), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 2));
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    // the right part: selection, snapping, camera
    const float wRight = std::min(size.x * 0.62f, 760.0f);
    ImGui::AlignTextToFramePadding();
    {
        // (clipped before the right part; the whole line in the tooltip)
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float wText = std::max(40.0f, size.x - wRight - 24.0f);
        ImGui::PushClipRect(p0, ImVec2(p0.x + wText, p0.y + 30.0f), true);
        ImGui::TextColored(st::vec(st::text2), "%s", m_status.c_str());
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(m_status.c_str()).x > wText) ImGui::SetTooltip("%s", m_status.c_str());
    }
    ImGui::SameLine(size.x - wRight);
    ImGui::TextColored(st::vec(st::muted), "%zu %s", m_asel.size(), T("selected"));
    ImGui::SameLine();
    bool fChanged = false;
    fChanged |= ImGui::Checkbox(T("Grid"), &m_fGridSnap);
    EdTip(T("Snap moves to the grid (Ctrl while dragging: the other way)"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    static const float s_agrid[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    char sz[32];
    std::snprintf(sz, sizeof(sz), "%g", m_gridSize);
    if (ImGui::BeginCombo("##grid", sz))
    {
        for (float g : s_agrid)
        {
            std::snprintf(sz, sizeof(sz), "%g", g);
            if (ImGui::Selectable(sz, g == m_gridSize)) { m_gridSize = g; fChanged = true; }
        }
        ImGui::EndCombo();
    }
    EdTip(T("Grid size ([ and ] change it)"));
    ImGui::SameLine();
    fChanged |= ImGui::Checkbox(T("Angle"), &m_fAngleSnap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    static const float s_aang[] = {1, 5, 10, 15, 30, 45, 90};
    std::snprintf(sz, sizeof(sz), "%g°", m_angleSnap);
    if (ImGui::BeginCombo("##angle", sz))
    {
        for (float g : s_aang)
        {
            std::snprintf(sz, sizeof(sz), "%g°", g);
            if (ImGui::Selectable(sz, g == m_angleSnap)) { m_angleSnap = g; fChanged = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    fChanged |= ImGui::Checkbox(T("Scale"), &m_fScaleSnap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    static const float s_asc[] = {0.05f, 0.1f, 0.25f, 0.5f};
    std::snprintf(sz, sizeof(sz), "%g", m_scaleSnap);
    if (ImGui::BeginCombo("##scalesnap", sz))
    {
        for (float g : s_asc)
        {
            std::snprintf(sz, sizeof(sz), "%g", g);
            if (ImGui::Selectable(sz, g == m_scaleSnap)) { m_scaleSnap = g; fChanged = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(st::vec(st::muted), "  %.0f, %.0f, %.0f  x%.0f", m_cam.pos.x, m_cam.pos.y, m_cam.pos.z, m_speed);
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (fChanged) SaveViewState();
}

}  // namespace edimpl
