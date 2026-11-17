// The map editor's options, shared by Sly 2 and Sly 3 (each game's editor_data*.cpp names its classes and options and
// includes this): the options panel with live updates where safe (the game's FEditorApplyOption), list options value by
// value, oid allocation over every recorded oid and the checks before a save. Option and class numbers are Sly 2's,
// mapped to the game's by EDEOPID / EDCID (editor_game.h). Sly2-Release/notes/EDITOR_2026-11-12.md.
#include "editor_impl.h"

#if CANE_EDITOR_3D && EDFEAT_OPTIONS

#include "worldparts.h"

#include <set>

namespace edimpl
{

namespace
{
// (Sly 3) the character a use is for (its option 1168: a player class)
const char* SzCharacterOfClass(int cid)
{
    switch (cid)
    {
    case 0x34: return "Sly";
    case 0x35: return "Murray";
    case 0x36: return "Bentley";
    case 0x37: return "the Guru";
    case 0x38: return "the Panda King";
    case 0x39: return "Penelope";
    default: return nullptr;
    }
}
const char* SzTaskState(int n)
{
    static const char* s_a[] = {"locked", "available", "done", "done and closed", "activate", "deactivate"};
    return n >= 0 && n <= 5 ? s_a[n] : "?";
}
}  // namespace

// ---- options

bool CEditor::FFieldBytes(int ifield, int cb, std::string& rel, std::size_t& ib, const std::vector<uint8_t>*& pab) const
{
    if (ifield < 0 || ifield >= int(m_rec.afield.size()) || cb <= 0) return false;
    if (!FLocate(m_rec.afield[ifield].ib, std::size_t(cb), rel, ib)) return false;
    pab = m_proj.partRef(rel);
    return pab && ib + std::size_t(cb) <= pab->size();
}

int CEditor::IfieldOption(LO* plo, int eopid) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end()) return -1;
    for (int i : it->second.aiopt)
        if (m_rec.afield[i].eopid == eopid) return i;
    return -1;
}

bool CEditor::FOptionInt(LO* plo, int eopid, int& n) const
{
    const int i = IfieldOption(plo, eopid);
    if (i < 0) return false;
    const OPTTYPE t = OptType(m_rec.afield[i].k);
    std::string rel;
    std::size_t ib = 0;
    const std::vector<uint8_t>* pab = nullptr;
    if (t.cf || !FFieldBytes(i, t.cb, rel, ib, pab)) return false;
    const std::vector<uint8_t>& ab = *pab;
    if (t.cb == 1) n = ab[ib];
    else if (t.cb == 2) { int16_t s; std::memcpy(&s, ab.data() + ib, 2); n = s; }
    else { int32_t s; std::memcpy(&s, ab.data() + ib, 4); n = s; }
    return true;
}

bool CEditor::FSetOption(int ifield, const void* pv, int cb, const std::string& label)
{
    std::string rel;
    std::size_t ib = 0;
    const std::vector<uint8_t>* pab = nullptr;
    if (!FFieldBytes(ifield, cb, rel, ib, pab)) { Status("this option is not in a binary part"); return false; }
    if (OptType(m_rec.afield[ifield].k).cf)
        for (int i = 0; i < cb / 4; ++i)
        {
            float g;
            std::memcpy(&g, static_cast<const uint8_t*>(pv) + 4 * i, 4);
            if (!std::isfinite(g)) { Status("rejected: not a number"); return false; }
        }
    m_proj.begin(label);
    const bool fOk = m_proj.patch(rel, ib, pv, std::size_t(cb));
    m_proj.end();
    if (!fOk) return false;
    const EDITFIELD& f = m_rec.afield[ifield];
    const char* why = nullptr;
    if (FEditorApplyOption(f.plo, f.eopid, pv, std::size_t(cb), &why))
    {
        m_note = "applied live";
        // the lights are drawn from their options: the world's lighting again
        if (f.plo && f.plo->Cid() == EDCID(0x04))
        {
            ResetWorldLights();
            UpdateWorldLightsFromSw(m_psw.get());
            if (FHaveWorldLights()) ApplyLighting(m_scene, WorldLights());
            BuildSceneGpu(m_scene);
        }
    }
    else m_note = std::string("shows after Reload world (") + (why ? why : "?") + ")";
    return true;
}

void CEditor::DrawOptions(LO* plo, const EDOBJ* po)
{
    // the method / list options (CRef arrays): each value of a known size editable in place (guards' patrol commands,
    // the (task, state) pairs of uses and triggers, ...); they are read at the load, so they show after a reload
    auto lists = [&]() {
        if (!po || po->aiarr.empty()) return;
        if (!ImGui::TreeNodeEx("List options", po->aiopt.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
        EdTip("Lists of values the object reads at the load (patrol commands, task and state pairs, ...): Enter applies a value; it shows after Reload world.");
        for (int ifield : po->aiarr)
        {
            const EDITFIELD& f = m_rec.afield[ifield];
            std::string rel;
            std::vector<LISTEL> ael;
            ImGui::TextColored(st::vec(st::muted), "%s:", StrOption(f.eopid).c_str());
            if (!FListElements(ifield, rel, ael)) { ImGui::SameLine(); ImGui::TextDisabled("(not decoded)"); continue; }
            const std::vector<uint8_t>* pab = m_proj.partRef(rel);
            ImGui::PushID(ifield);
            for (std::size_t i = 0; i < ael.size() && pab; ++i)
            {
                const LISTEL& e = ael[i];
                ImGui::PushID(int(i));
                if (i % 4 != 0) ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                bool fEnter = false;
                double v = 0;
                if (e.fFloat) { float g; std::memcpy(&g, pab->data() + e.ib, 4); float g2 = g; fEnter = ImGui::InputFloat("##v", &g2, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue); v = g2; }
                else
                {
                    int n = e.cb == 1 ? (*pab)[e.ib] : 0;
                    if (e.cb == 2) { int16_t s; std::memcpy(&s, pab->data() + e.ib, 2); n = s; }
                    else if (e.cb == 4) { int32_t s; std::memcpy(&s, pab->data() + e.ib, 4); n = s; }
                    fEnter = ImGui::InputInt("##v", &n, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
                    v = n;
                }
                if (fEnter)
                {
                    std::string error;
                    if (!FSetListElement(ifield, int(i), v, error)) Status("list option: " + error);
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    };
    if (!po || po->aiopt.empty()) { if (!po || po->aiarr.empty()) ImGui::TextDisabled("(no options loaded with it)"); lists(); return; }
    const GAMEAREA ga = AreaOfCid(plo->Cid());
    const std::vector<int>& akey = KeyOptionsOfArea(ga);
    auto row = [&](int ifield) {
        const EDITFIELD& f = m_rec.afield[ifield];
        const OPTTYPE t = OptType(f.k);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const char* szName = SzOptionOf(plo, f.eopid);
        ImGui::Text("%d", f.eopid);
        ImGui::TableNextColumn();
        if (szName) ImGui::TextWrapped("%s", szName);
        else ImGui::TextColored(st::vec(st::muted), "%s", t.name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("option %d, kind %d (%s)", f.eopid, f.k, t.name);
        ImGui::TableNextColumn();
        std::string rel;
        std::size_t ib = 0;
        const std::vector<uint8_t>* pab = nullptr;
        if (!FFieldBytes(ifield, t.cb, rel, ib, pab)) { ImGui::TextDisabled("-"); return; }
        const std::vector<uint8_t> ab(pab->begin() + std::ptrdiff_t(ib), pab->begin() + std::ptrdiff_t(ib) + t.cb);
        ib = 0;
        ImGui::PushID(ifield);
        ImGui::SetNextItemWidth(-1);
        const std::string label = "Set option " + std::to_string(f.eopid) + " of " + SzClass(plo->Cid()) + " " + std::to_string(plo->oid);
        if (t.cf)
        {
            float ag[4] = {};
            std::memcpy(ag, ab.data() + ib, std::size_t(t.cb));
            const bool fEdit = t.cf == 1 ? ImGui::InputFloat("##v", ag, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)
                             : t.cf == 2 ? ImGui::InputFloat2("##v", ag, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)
                             : t.cf == 3 ? ImGui::InputFloat3("##v", ag, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)
                                         : ImGui::InputFloat4("##v", ag, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
            if (fEdit) FSetOption(ifield, ag, t.cb, label);
        }
        else
        {
            int64_t n = 0;
            if (t.cb == 1) n = ab[ib];
            else if (t.cb == 2) { int16_t s; std::memcpy(&s, ab.data() + ib, 2); n = s; }
            else { int32_t s; std::memcpy(&s, ab.data() + ib, 4); n = s; }
            int v = int(n);
            bool fEdit = false;
            if (f.k == 13) fEdit = ImGui::InputScalar("##v", ImGuiDataType_U32, &v, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
            else fEdit = ImGui::InputInt("##v", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
            // an object reference: its class, and a click selects it
            if (std::string(t.name) == "object id" || (SzOptionName(f.eopid) && v > 0 && PloOid(v) && FOidOption(f.eopid)))
                if (LO* pref = PloOid(v))
                {
                    ImGui::TextColored(st::vec(st::accent), "-> %s %d", SzClass(pref->Cid()), v);
                    if (ImGui::IsItemClicked()) Select(pref);
                }
#if EDGAME_SLY3
            // (a use's character: the player class's name)
            if (f.eopid == 1168)
                if (const char* sz = SzCharacterOfClass(v)) ImGui::TextColored(st::vec(st::accent), "-> %s", sz);
#endif
            if (fEdit)
            {
                if (t.cb == 1) { const uint8_t b = uint8_t(v); FSetOption(ifield, &b, 1, label); }
                else if (t.cb == 2) { const int16_t s = int16_t(v); FSetOption(ifield, &s, 2, label); }
                else { const int32_t s = v; FSetOption(ifield, &s, 4, label); }
            }
        }
        ImGui::PopID();
    };
    // the area's key options, then the other options the editor knows by name, then (collapsed) the raw ones: no
    // name known (their number and kind), and the list options
    enum { K_Key, K_Named, K_Raw };
    auto kindOf = [&](int ifield) {
        const int eopid = m_rec.afield[ifield].eopid;
        if (std::find(akey.begin(), akey.end(), eopid) != akey.end()) return int(K_Key);
        return SzOptionOf(plo, eopid) ? int(K_Named) : int(K_Raw);
    };
    auto table = [&](const char* id, int k, float h) {
        if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | (h > 0 ? ImGuiTableFlags_ScrollY : 0), ImVec2(0, h))) return;
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int ifield : po->aiopt)
            if (kindOf(ifield) == k) row(ifield);
        ImGui::EndTable();
    };
    int ac[3] = {};
    for (int ifield : po->aiopt) ++ac[kindOf(ifield)];
    if (ac[K_Key])
    {
        ImGui::TextColored(st::vec(st::text2), "%s", SzArea(ga));
        table("keyopts", K_Key, 0);
    }
    if (ac[K_Named])
    {
        if (ac[K_Key]) ImGui::TextColored(st::vec(st::text2), "%s", T("Other known options"));
        table("namedopts", K_Named, 0);
    }
    if (!ac[K_Key] && !ac[K_Named]) ImGui::TextDisabled("%s", T("(no option of this object has a known name yet)"));
    char label[96];
    std::snprintf(label, sizeof(label), "%s (%d)###rawopts", T("Raw fields"), ac[K_Raw] + int(po->aiarr.size()));
    if (ImGui::TreeNodeEx(label, 0))
    {
        EdTip(T("Options the editor has no name for yet: their number, kind and value as stored"));
        if (ac[K_Raw]) table("opts", K_Raw, std::min(260.0f, 26.0f * float(ac[K_Raw] + 1)));
        lists();
        ImGui::TreePop();
    }
    if (!m_note.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::muted)); ImGui::TextWrapped("Last option edit: %s.", m_note.c_str()); ImGui::PopStyleColor(); }
}

// ---- gameplay areas of placed objects, links


GAMEAREA CEditor::AreaOfLo(LO* plo) const
{
    const GAMEAREA ga = AreaOfCid(plo->Cid());
#if EDGAME_SLY3
    // a use for one of the gang (not Sly): their ability spot
    int c = 0;
    if (ga == GA_Buttons && FIsLoDerivedFrom(plo, EDCID(0x23)) && FOptionInt(plo, EDEOPID(1074), c) && c >= 0x35 && c <= 0x39) return GA_Gang;
#endif
    return ga;
}

std::string CEditor::StrLo(LO* plo) const
{
    std::string s = std::string(SzClass(plo->Cid())) + " " + std::to_string(plo->oid);
#if EDGAME_SLY3
    int c = 0;
    if (FIsLoDerivedFrom(plo, EDCID(0x23)) && FOptionInt(plo, EDEOPID(1074), c))
        if (const char* sz = SzCharacterOfClass(c)) s += std::string(" (") + sz + ")";
#endif
    return s;
}

bool CEditor::FAddOption(LO* plo, int eopid, const std::vector<uint8_t>& abValue, const std::string& label)
{
    // before the object's first option (its list ends with S16 -1 after the last): S16 eopid, then the value
    const auto it = m_mpobj.find(plo);
    if (!plo || it == m_mpobj.end() || eopid < 0) { Status("add option: no object"); return false; }
    std::size_t ibFirst = SIZE_MAX;
    for (int i : it->second.aiopt) ibFirst = std::min(ibFirst, m_rec.afield[std::size_t(i)].ib);
    for (int i : it->second.aiarr) ibFirst = std::min(ibFirst, m_rec.afield[std::size_t(i)].ib);
    std::string rel;
    std::size_t ib = 0;
    if (ibFirst == SIZE_MAX || ibFirst < 2 || !FLocate(ibFirst - 2, 2, rel, ib)) { Status("add option: the object has no options stored in a binary part to add one to"); return false; }
    std::vector<uint8_t> ab(2);
    const int16_t e = int16_t(eopid);
    std::memcpy(ab.data(), &e, 2);
    ab.insert(ab.end(), abValue.begin(), abValue.end());
    m_proj.begin(label, true);
    const bool fOk = m_proj.spliceBytes(rel, ib, 0, ab, EnclosingSizeFields(rel, ib));
    m_proj.end();
    if (!fOk) { Status("add option: could not write"); return false; }
    Status(label);
    Reload(plo->oid);
    return true;
}

bool CEditor::FRemoveOption(int ifield, const std::string& label)
{
    if (ifield < 0 || ifield >= int(m_rec.afield.size())) return false;
    const EDITFIELD& f = m_rec.afield[std::size_t(ifield)];
    std::string rel;
    std::size_t ib = 0;
    if ((f.fk != EDITFK_Option && f.fk != EDITFK_OptionArray) || f.ibEnd <= f.ib || f.ib < 2 || !FLocate(f.ib - 2, (f.ibEnd - f.ib) + 2, rel, ib))
    {
        Status("remove option: not stored in a binary part");
        return false;
    }
    const int oid = f.plo ? f.plo->oid : 0;
    m_proj.begin(label, true);
    const bool fOk = m_proj.spliceBytes(rel, ib, (f.ibEnd - f.ib) + 2, {}, EnclosingSizeFields(rel, ib));
    m_proj.end();
    if (!fOk) { Status("remove option: could not write"); return false; }
    Status(label);
    Reload(oid);
    return true;
}

bool CEditor::FAddLink(LO* plo, int kind, const int an[3], std::string& error)
{
    // a use's (USE family) links: 0 a task's state (EOPID 1076 / Sly 3's 1170: task, state; 4 at most), 1 its script
    // (1070 / 1164: started when it is used), 2 a state machine (1075 / 1169: object, machine (-1 any), state; 3 at most)
    if (!plo || !FIsLoDerivedFrom(plo, EDCID(0x23))) { error = "links are added to uses (buttons, switches, ability spots)"; return false; }
    const int eopid = kind == 0 ? EDEOPID(1076) : kind == 1 ? EDEOPID(1070) : EDEOPID(1075);
    if (eopid < 0 || kind < 0 || kind > 2) { error = "no such link"; return false; }
    auto s32 = [](std::vector<uint8_t>& ab, int32_t v) {
        const uint16_t k = 6;   // (a CRef of kind 6: an S32)
        ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&k), reinterpret_cast<const uint8_t*>(&k) + 2);
        ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + 4);
    };
    const std::string who = StrLo(plo);
    if (kind == 1)
    {
        LO* pt = PloOid(an[0]);
        if (!pt) { error = "no object " + std::to_string(an[0]) + " in this world"; return false; }
        const int32_t oid = an[0];
        if (const int ifield = IfieldOption(plo, eopid); ifield >= 0)
            return FSetOption(ifield, &oid, 4, who + ": starts " + StrLo(pt) + " when used");
        std::vector<uint8_t> ab(4);
        std::memcpy(ab.data(), &oid, 4);
        return FAddOption(plo, eopid, ab, who + ": starts " + StrLo(pt) + " when used");
    }
    const auto it = m_mpobj.find(plo);
    int c = 0;
    if (it != m_mpobj.end())
        for (int i : it->second.aiarr) c += m_rec.afield[std::size_t(i)].eopid == eopid;
    std::vector<uint8_t> ab;
    if (kind == 0)
    {
        if (c >= 4) { error = "a use sets at most 4 tasks"; return false; }
        if (!PloOid(an[0], EDCID(0x85))) { error = "no task " + std::to_string(an[0]) + " in this world"; return false; }
        if (an[1] < 0 || an[1] > 5) { error = "a task's state is 0..5"; return false; }
        s32(ab, an[0]);
        s32(ab, an[1]);
        return FAddOption(plo, eopid, ab, who + ": sets task " + std::to_string(an[0]) + " to " + SzTaskState(an[1]));
    }
    if (c >= 3) { error = "a use runs at most 3 state machines"; return false; }
    if (!PloOid(an[0])) { error = "no object " + std::to_string(an[0]) + " in this world"; return false; }
    s32(ab, an[0]);
    s32(ab, an[1]);
    s32(ab, an[2]);
    return FAddOption(plo, eopid, ab, who + ": runs " + std::to_string(an[0]) + "'s state machine to " + std::to_string(an[2]));
}

void CEditor::DrawLinks(LO* plo, const EDOBJ* po)
{
    const bool fUse = FIsLoDerivedFrom(plo, EDCID(0x23));
    const int eTask = EDEOPID(1076), eSm = EDEOPID(1075);
    // what it names (object options) and what it does (a use's task states / state machines)
    std::vector<int> aiRef, aiArr;
    if (po)
    {
        for (int i : po->aiopt)
        {
            const EDITFIELD& f = m_rec.afield[std::size_t(i)];
            int n = 0;
            if ((std::string(OptType(f.k).name) == "object id" || FOidOption(f.eopid)) && FOptionInt(plo, f.eopid, n) && n > 0) aiRef.push_back(i);
        }
        for (int i : po->aiarr)
            if (m_rec.afield[std::size_t(i)].eopid == eTask || m_rec.afield[std::size_t(i)].eopid == eSm) aiArr.push_back(i);
    }
    if (aiRef.empty() && aiArr.empty() && !fUse) return;
    ImGui::SeparatorText("Links");
    EdTip("What this object names and what it does to others when it acts (a use: when it is used).");
    for (int i : aiRef)
    {
        const EDITFIELD& f = m_rec.afield[std::size_t(i)];
        int n = 0;
        FOptionInt(plo, f.eopid, n);
        const char* sz = SzOptionName(f.eopid);
        LO* pt = PloOid(n);
        ImGui::PushID(i);
        ImGui::TextColored(st::vec(st::muted), "%s", sz ? sz : StrOption(f.eopid).c_str());
        ImGui::SameLine();
        if (pt) { if (ImGui::SmallButton(("-> " + StrLo(pt)).c_str())) { Select(pt); FrameSelected(); } }
        else ImGui::TextColored(st::vec(st::danger), "-> %d (not in this world)", n);
        ImGui::PopID();
    }
    for (int i : aiArr)
    {
        const EDITFIELD& f = m_rec.afield[std::size_t(i)];
        std::string rel;
        std::vector<LISTEL> ael;
        if (!FListElements(i, rel, ael)) continue;
        const std::vector<uint8_t>* pab = m_proj.partRef(rel);
        if (!pab) continue;
        auto val = [&](std::size_t k) {
            if (k >= ael.size() || ael[k].fFloat) return 0;
            const LISTEL& e = ael[k];
            if (e.cb == 1) return int((*pab)[e.ib]);
            if (e.cb == 2) { int16_t v; std::memcpy(&v, pab->data() + e.ib, 2); return int(v); }
            int32_t v;
            std::memcpy(&v, pab->data() + e.ib, 4);
            return int(v);
        };
        ImGui::PushID(i);
        const bool fTask = f.eopid == eTask;
        const int cv = fTask ? 2 : 3;
        if (int(ael.size()) < cv) { ImGui::PopID(); continue; }
        LO* pt = PloOid(val(0));
        if (fTask) ImGui::Text("Sets task");
        else ImGui::Text("Runs the state machine of");
        ImGui::SameLine();
        if (pt) { if (ImGui::SmallButton(StrLo(pt).c_str())) { Select(pt); FrameSelected(); } }
        else ImGui::TextColored(st::vec(st::danger), "%d (missing)", val(0));
        ImGui::SameLine();
        if (fTask) ImGui::TextColored(st::vec(st::muted), "to %s", SzTaskState(val(1)));
        else ImGui::TextColored(st::vec(st::muted), "to state %d", val(2));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, st::vec(st::danger, 0.6f));
        const bool fRemove = ImGui::SmallButton("Remove");
        ImGui::PopStyleColor();
        // the numbers, editable (Enter applies; shows after Reload world)
        for (int k = 0; k < cv; ++k)
        {
            ImGui::PushID(k);
            if (k) ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            int v = val(std::size_t(k));
            const char* lab = fTask ? (k == 0 ? "task" : "state") : (k == 0 ? "object" : k == 1 ? "machine" : "state");
            if (ImGui::InputInt(lab, &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::string error;
                if (!FSetListElement(i, k, v, error)) Status("link: " + error);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
        if (fRemove) { FRemoveOption(i, "Remove a link of " + StrLo(plo)); return; }
    }
    if (!fUse) return;
    // a new link
    static int s_kind = 0;
    static int s_an[3] = {0, 4, -1};
    ImGui::TextColored(st::vec(st::muted), "New link:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##linkkind", &s_kind, "Set a task's state\0Start a script\0Run a state machine\0");
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt(s_kind == 1 ? "script##l0" : s_kind == 0 ? "task##l0" : "object##l0", &s_an[0], 0, 0);
    if (s_kind != 1)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::SetNextItemWidth(130);
        if (s_kind == 0) ImGui::Combo("state##lstate", &s_an[1], "locked\0available\0done\0done, closed\0activate\0deactivate\0");
        else
        {
            ImGui::SetNextItemWidth(70);
            ImGui::InputInt("machine##l1", &s_an[1], 0, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::InputInt("state##l2", &s_an[2], 0, 0);
        }
    }
    if (LO* pt = PloOid(s_an[0])) ImGui::TextColored(st::vec(st::muted), "-> %s", StrLo(pt).c_str());
    if (ImGui::Button("Add link"))
    {
        std::string error;
        if (!FAddLink(plo, s_kind, s_an, error)) Status("link: " + error);
    }
    EdTip("Adds the link to this use (a structural edit: the world reloads). A use placed by a proxy is its shared group's: every copy gets it.");
}

// ---- oids, records

int CEditor::OidNew()
{
    // above every oid the index and the loader saw (nested records included), and the copies made since
    int oidMax = m_index.maxOid();
    for (const EDITOBJREC& r : m_rec.aobj) oidMax = std::max(oidMax, r.oid);
    for (const auto& up : m_psw->aploAll) oidMax = std::max(oidMax, up->oid);
    static int s_cIssued = 0;
    static int s_oidLast = 0;
    int oid = oidMax + 1;
    if (oid <= s_oidLast) oid = s_oidLast + 1;
    s_oidLast = oid;
    ++s_cIssued;
    return oid;
}

bool CEditor::FRecordRange(int iobjrec, std::string& rel, std::size_t& ib, std::size_t& cb) const
{
    if (iobjrec < 0 || iobjrec >= int(m_rec.aobj.size())) return false;
    const EDITOBJREC& r = m_rec.aobj[iobjrec];
    cb = (r.ibBody - r.ibRecord) + r.cb;
    return FLocate(r.ibRecord, cb, rel, ib);
}

std::vector<std::size_t> CEditor::EnclosingSizeFields(const std::string& rel, std::size_t ibFile) const
{
    // every record whose body holds that byte: its S32 cb (just before its body)
    std::vector<std::size_t> a;
    for (const EDITOBJREC& r : m_rec.aobj)
    {
        std::string relR;
        std::size_t ibBody = 0;
        if (r.cb == 0 || !FLocate(r.ibBody, r.cb, relR, ibBody) || relR != rel) continue;
        if (ibFile >= ibBody && ibFile < ibBody + r.cb) a.push_back(ibBody - 4);
    }
    return a;
}

// ---- list options (CRef arrays: U16 kind, then a value; 0x8000: U8 count and values) element by element

bool CEditor::FListElements(int ifield, std::string& rel, std::vector<LISTEL>& a) const
{
    a.clear();
    if (ifield < 0 || ifield >= int(m_rec.afield.size())) return false;
    const EDITFIELD& f = m_rec.afield[size_t(ifield)];
    std::size_t ib = 0;
    const std::vector<uint8_t>* pab = nullptr;
    if (f.fk != EDITFK_OptionArray || f.ibEnd <= f.ib || !FLocate(f.ib, f.ibEnd - f.ib, rel, ib) || !(pab = m_proj.partRef(rel)) || ib + (f.ibEnd - f.ib) > pab->size())
        return false;
    const std::vector<uint8_t>& ab = *pab;
    std::size_t i = ib;
    const std::size_t iEnd = ib + (f.ibEnd - f.ib);
    while (i + 2 <= iEnd)
    {
        uint16_t w;
        std::memcpy(&w, ab.data() + i, 2);
        i += 2;
        int c = 1;
        if (w & 0x8000) { w &= 0x7FFF; if (i >= iEnd) break; c = ab[i++]; }
        const OPTTYPE t = OptType(w);
        if (t.cb <= 0) break;   // (a kind of unknown size: the rest is not shown)
        const int cel = t.cf ? t.cf : std::max(1, t.ci);
        const int cbEl = t.cb / cel;
        for (int j = 0; j < c && i + std::size_t(t.cb) <= iEnd; ++j, i += std::size_t(t.cb))
            for (int k = 0; k < cel; ++k) a.push_back({i + std::size_t(k * cbEl), cbEl, t.cf > 0});
    }
    return !a.empty();
}

bool CEditor::FSetListElement(int ifield, int iel, double v, std::string& error)
{
    std::string rel;
    std::vector<LISTEL> a;
    if (!FListElements(ifield, rel, a) || iel < 0 || iel >= int(a.size())) { error = "no such value"; return false; }
    const LISTEL& e = a[size_t(iel)];
    uint8_t b[4] = {};
    if (e.fFloat)
    {
        const float g = float(v);
        if (!std::isfinite(g) || std::fabs(g) > 1e7f) { error = "rejected: not a valid number"; return false; }
        std::memcpy(b, &g, 4);
    }
    else
    {
        const int64_t n = int64_t(std::llround(v));
        const int64_t lo = e.cb == 1 ? 0 : e.cb == 2 ? INT16_MIN : INT32_MIN, hi = e.cb == 1 ? 255 : e.cb == 2 ? INT16_MAX : INT32_MAX;
        if (n < lo || n > hi) { error = "rejected: out of range for its size"; return false; }
        for (int k = 0; k < e.cb; ++k) b[k] = uint8_t(uint64_t(n) >> (8 * k));
    }
    const EDITFIELD& f = m_rec.afield[size_t(ifield)];
    m_proj.begin("List option " + StrOption(f.eopid) + " [" + std::to_string(iel) + "] of " + std::to_string(f.plo ? f.plo->oid : 0));
    const bool fOk = m_proj.patch(rel, e.ib, b, size_t(e.cb));
    m_proj.end();
    if (!fOk) { error = "could not write"; return false; }
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%g", v);
    Status("list option " + StrOption(f.eopid) + " [" + std::to_string(iel) + "] = " + sz + " (shows after Reload world)");
    return true;
}

// ---- checks before a save

std::vector<PROBLEM> CEditor::Validate()
{
    std::vector<PROBLEM> a;
    std::set<int> setOid;
    for (const auto& up : m_psw->aploAll) setOid.insert(up->oid);
    for (const EDITOBJREC& r : m_rec.aobj) setOid.insert(r.oid);
    for (const ce::GroupInfo& g : m_index.groups)
        for (const ce::ObjRecord& r : g.objects) setOid.insert(r.oid);
    // object references in options (the kinds that hold an oid, and the job / task links by number)
    for (std::size_t i = 0; i < m_rec.afield.size(); ++i)
    {
        const EDITFIELD& f = m_rec.afield[i];
        if (f.fk != EDITFK_Option || !f.plo) continue;
        const OPTTYPE t = OptType(f.k);
        const bool fOid = std::string(t.name) == "object id" || FOidOption(f.eopid) || (!EDGAME_SLY3 && f.eopid == EDEOPID(779));
        if (!fOid || t.cf || t.cb < 2) continue;
        int n = 0;
        if (!FOptionInt(f.plo, f.eopid, n) || n <= 0) continue;
        // (a task of another world names that world's checkpoint)
        if (int w = 0; f.plo->Cid() == EDCID(0x85) && FOptionInt(f.plo, EDEOPID(1275), w) && w != m_eo.record) continue;
        if (!setOid.count(n))
            a.push_back({f.plo->oid, std::string(SzClass(f.plo->Cid())) + " " + std::to_string(f.plo->oid) + ": option " + StrOption(f.eopid) +
                                         " names oid " + std::to_string(n) + ", which is not in the world"});
    }
    // task links
    for (const auto& up : m_psw->aploAll)
    {
        if (up->Cid() != EDCID(0x85)) continue;
        TASKLINKS tl;
        if (!FTaskLinks(up.get(), tl)) continue;
        for (int oid : tl.aoidPre)
            if (!setOid.count(oid)) a.push_back({up->oid, "task " + std::to_string(up->oid) + ": prerequisite " + std::to_string(oid) + " is not in the world"});
        for (int oid : tl.aoidPost)
            if (!setOid.count(oid)) a.push_back({up->oid, "task " + std::to_string(up->oid) + ": next task " + std::to_string(oid) + " is not in the world"});
    }
    // scripts: the literal oids they look up (the groups whose scripts the project changed)
    for (const ce::GroupInfo& g : m_index.groups)
    {
        if (g.scriptsPart.empty() || !m_proj.partChanged(g.scriptsPart)) continue;
        std::vector<uint8_t> ab;
        if (!m_proj.readPart(g.scriptsPart, ab)) continue;
        std::string error;
        int line = 0;
        std::vector<PROBLEM> apb;
        if (!FCheckScript(std::string(ab.begin(), ab.end()), error, line, &apb, g.index)) a.push_back({0, g.name + " scripts: " + error});
        for (PROBLEM& p : apb) a.push_back({p.oid, g.name + " scripts: " + p.text});
    }
    // oids used by more top-level records than in the installed world (the game's own data has a few shared ones)
    std::map<int, int> mpc, mpcInst;
    for (const ce::GroupInfo& g : m_index.groups)
        if (!g.objectsPart.empty())
        {
            std::vector<uint8_t> ab;
            std::vector<ce::ObjRecord> arec;
            std::string e2;
            if (m_proj.readPart(g.objectsPart, ab) && ce::parseObjects(ab.data(), ab.size(), arec, e2))
                for (const ce::ObjRecord& r : arec) ++mpc[r.oid];
            for (const ce::ObjRecord& r : g.objects) ++mpcInst[r.oid];
        }
    for (const auto& [oid, c] : mpc)
        if (c > 1 && oid > 0 && c > mpcInst[oid]) a.push_back({oid, "oid " + std::to_string(oid) + " is used by " + std::to_string(c) + " top-level records"});
    return a;
}

void CEditor::DrawProblems()
{
    if (m_fProblemsOpen) { ImGui::OpenPopup("Check before saving"); m_fProblemsOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Check before saving", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextColored(st::vec(st::danger), "%zu missing references or problems:", m_aproblem.size());
    ImGui::BeginChild("pb", ImVec2(0, 260), true);
    for (const PROBLEM& p : m_aproblem)
    {
        if (ImGui::Selectable(p.text.c_str()) && p.oid)
            if (LO* plo = PloOid(p.oid)) { Select(plo); FrameSelected(); }
    }
    ImGui::EndChild();
    ImGui::TextColored(st::vec(st::muted), "The game may stop or misbehave where a reference is missing.");
    if (ImGui::Button("Save anyway")) { m_fSaveAnyway = true; ImGui::CloseCurrentPopup(); FSave(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace edimpl

#endif
