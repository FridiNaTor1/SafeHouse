// The map editor (editor_impl.h): new objects from the world's own pieces. The palette lists one placed example of
// each class (and each shared group, placed by a proxy) found in the world; Place copies its record into the chosen
// group in front of the camera (on the collision below when there is some). A copy into another group drops the
// script modules (an object names its module by number in its own group's scripts). "Make unique" turns a proxy-placed
// instance of a one-object shared group into the level's own object (the template's record with the proxy's placement,
// the proxy removed, the group's instance count lowered). Nested records (an object's children) are deleted /
// duplicated inside their parent (its list count and every enclosing record's size fixed). New top-level oids are above
// every oid the loader saw, nested ones included; nested records keep theirs (as the game's own instances do).
// Shared by Sly 2 and Sly 3 (record headers: editor_impl.h CbRecordPrefix; class numbers by EDCID).
#include "editor_impl.h"

#include <regex>

#if CANE_EDITOR_3D && EDFEAT_STRUCT

namespace edimpl
{

int CEditor::LevelGroup() const
{
#if EDGAME_SLY3
    // (Sly 3: the world's own group, named as the world, is always loaded; its other level groups may be left out by
    // the world's option sets)
    const std::string code = m_index.json.value("code", std::string());
    for (const ce::GroupInfo& g : m_index.groups)
        if (g.level && !g.objectsPart.empty() && g.name == code) return g.index;
#endif
    for (const ce::GroupInfo& g : m_index.groups)
        if (g.level && !g.objectsPart.empty()) return g.index;
    return -1;
}

int CEditor::CopyGroup() const
{
#if EDGAME_SLY3
    return LevelGroup();
#else
    // the level group that has scripts (a copy's script modules come with it), else the first
    for (const ce::GroupInfo& g : m_index.groups)
        if (g.level && !g.objectsPart.empty() && !g.scriptsPart.empty()) return g.index;
    return LevelGroup();
#endif
}

void CEditor::RenumberCopy(std::vector<uint8_t>& rec, int iobjrecSrc, std::size_t ibSrc, int oidTop, bool fDropScripts, std::map<int, int>* pmpoid)
{
    (void)pmpoid;
    if (!fDropScripts) return;
    const EDITOBJREC& r = m_rec.aobj[iobjrecSrc];
    const std::size_t cb = (r.ibBody - r.ibRecord) + r.cb;
    // a copy into another group: its script modules carried into that group's scripts (renumbered; the copy's own oid
    // in them follows it), else dropped (the destination has no scripts)
    std::string relSrc, textSrc, textDst;
    std::size_t ibR = 0, cbR = 0;
    int igrpSrc = -1;
    if (m_igrpCopyDest >= 0 && FRecordRange(iobjrecSrc, relSrc, ibR, cbR))
        for (const ce::GroupInfo& g : m_index.groups)
            if (g.objectsPart == relSrc) igrpSrc = g.index;
    const ce::GroupInfo* pgDst = m_igrpCopyDest >= 0 && m_igrpCopyDest < int(m_index.groups.size()) ? &m_index.groups[size_t(m_igrpCopyDest)] : nullptr;
    const bool fCarry = igrpSrc >= 0 && pgDst && !pgDst->scriptsPart.empty() && !m_index.groups[size_t(igrpSrc)].scriptsPart.empty();
    if (fCarry)
    {
        std::vector<uint8_t> ab;
        if (m_proj.readPart(m_index.groups[size_t(igrpSrc)].scriptsPart, ab)) textSrc = ScriptTextRaw(std::string(ab.begin(), ab.end()));
        if (m_proj.readPart(pgDst->scriptsPart, ab)) textDst.assign(ab.begin(), ab.end());
    }
    std::map<int, int> mpmodule;
    int nNext = 0;
    if (fCarry)
    {
        static const std::regex reMod(R"((^|\n)\(module\s+(\d+))");
        for (auto it = std::sregex_iterator(textDst.begin(), textDst.end(), reMod); it != std::sregex_iterator(); ++it)
            nNext = std::max(nNext, std::atoi((*it)[2].str().c_str()) + 1);
    }
    // module n's text after "(module n" up to its closing parenthesis
    auto moduleInner = [&](int n, std::string& inner) {
        const std::regex reOne("(^|\\n)\\(module\\s+" + std::to_string(n) + "(?=[\\s)])");
        std::smatch m;
        if (!std::regex_search(textSrc, m, reOne)) return false;
        const std::size_t ibOpen = std::size_t(m.position(0)) + (m[1].length() ? 1 : 0);
        const std::size_t ibBody = std::size_t(m.position(0) + m.length(0));
        int depth = 0;
        bool fStr = false;
        for (std::size_t i = ibOpen; i < textSrc.size(); ++i)
        {
            const char ch = textSrc[i];
            if (fStr) { if (ch == '\\') ++i; else if (ch == '"') fStr = false; continue; }
            if (ch == '"') fStr = true;
            else if (ch == ';') { while (i < textSrc.size() && textSrc[i] != '\n') ++i; }
            else if (ch == '(') ++depth;
            else if (ch == ')' && --depth == 0) { inner = textSrc.substr(ibBody, i - ibBody); return true; }
        }
        return false;
    };
    int cCarried = 0, cDropped = 0;
    for (const EDITOBJREC& r2 : m_rec.aobj)
    {
        if (r2.ibRecord < ibSrc || r2.ibRecord >= ibSrc + cb) continue;
        const std::size_t o = r2.ibRecord - ibSrc;
        if (o >= rec.size()) continue;
        if (o + 3 > rec.size()) continue;
        const std::size_t ibN = o + IbOidOfRecord(rec.data() + o) + 4;
        if (ibN + 2 > rec.size()) continue;
        int16_t n;
        std::memcpy(&n, rec.data() + ibN, 2);
        int16_t nNew = -1;
        if (n >= 0 && fCarry)
        {
            if (const auto it = mpmodule.find(n); it != mpmodule.end()) nNew = int16_t(it->second);
            else
            {
                std::string inner;
                if (moduleInner(n, inner) && nNext < 32767)
                {
                    if (r2.depth == r.depth && oidTop > 0)   // (the copy's own oid in its scripts)
                        inner = std::regex_replace(inner, std::regex("\\b" + std::to_string(r.oid) + "\\b"), std::to_string(oidTop));
                    while (!textDst.empty() && (textDst.back() == '\n' || textDst.back() == ' ')) textDst.pop_back();
                    textDst += "\n\n(module " + std::to_string(nNext) + inner + ")\n";
                    mpmodule[n] = nNext;
                    nNew = int16_t(nNext++);
                }
            }
        }
        if (n >= 0) (nNew >= 0 ? cCarried : cDropped)++;
        std::memcpy(rec.data() + ibN, &nNew, 2);
    }
    if (!mpmodule.empty())
    {
        std::string error;
        int line = 0;
        if (FCheckScript(textDst, error, line, nullptr) && m_proj.setPart(pgDst->scriptsPart, std::vector<uint8_t>(textDst.begin(), textDst.end())))
        {
            m_noteCopy = std::to_string(mpmodule.size()) + " script module(s) carried into " + pgDst->name;
            if (m_igroupScript == pgDst->index) LoadScriptText(pgDst->index);
        }
        else
        {
            // (the carried scripts do not compile there: dropped after all)
            for (const EDITOBJREC& r2 : m_rec.aobj)
            {
                if (r2.ibRecord < ibSrc || r2.ibRecord >= ibSrc + cb) continue;
                const std::size_t o = r2.ibRecord - ibSrc;
                if (o + 3 > rec.size()) continue;
                const std::size_t ibN = o + IbOidOfRecord(rec.data() + o) + 4;
                const int16_t nNone = -1;
                if (ibN + 2 <= rec.size()) std::memcpy(rec.data() + ibN, &nNone, 2);
            }
            m_noteCopy = "its scripts do not compile in " + pgDst->name + " (" + error + "): dropped";
        }
    }
    else if (cDropped) m_noteCopy = "its scripts are not carried (the group has none)";
}

int CEditor::CopyRecord(int iobjrec, int igrpDest, const glm::vec3* ppos, bool fSelect, std::string& error)
{
    if (iobjrec < 0 || iobjrec >= int(m_rec.aobj.size())) { error = "no record"; return 0; }
    if (igrpDest < 0 || igrpDest >= int(m_index.groups.size()) || m_index.groups[igrpDest].objectsPart.empty()) { error = "the group has no Objects part"; return 0; }
    const EDITOBJREC& r = m_rec.aobj[iobjrec];
    std::string relSrc;
    std::size_t ibSrc = 0, cbSrc = 0;
    if (!FRecordRange(iobjrec, relSrc, ibSrc, cbSrc)) { error = "the record is not in a binary part"; return 0; }
    const std::string& relDest = m_index.groups[igrpDest].objectsPart;
    const bool fOtherGroup = relDest != relSrc;
    const bool fProxy = FCidDerivedFrom(r.cid, EDCID(6)) && r.ibMatrix;
    const int oidNew = OidNew();
    RECEDIT re;
    if (fOtherGroup) { re.fDropScripts = true; re.nModule = -1; }
    std::vector<uint8_t> rec;
    const std::string label = "Create " + std::string(SzClass(r.cid)) + " " + std::to_string(oidNew) + " from " + std::to_string(r.oid);
    m_proj.begin(label, true);   // (the scripts carried into the destination's group are part of the step)
    m_igrpCopyDest = fOtherGroup ? igrpDest : -1;
    m_noteCopy.clear();
    const bool fBuilt = FBuildCopy(iobjrec, oidNew, re, rec, error);
    m_igrpCopyDest = -1;
    if (!fBuilt)
    {
        m_proj.end();
        if (m_proj.canUndo() && m_proj.history().back().label == label) m_proj.undo();
        return 0;
    }
    MakeOwnRecord(rec.data());   // (a shared group's record made the level's own)
    m_proj.begin(label, true);
    std::size_t ibNew = 0;
    bool fOk = m_proj.appendRecord(relDest, rec, &ibNew, error);
    if (fOk) BumpProxiesIn(iobjrec, 1);
    // the place: the proxy's matrix, or the object's own
    std::size_t ibMatW = 0;
    if (fProxy) ibMatW = r.ibMatrix;
    else if (const auto it = m_mpobj.find(r.plo); r.plo && it != m_mpobj.end() && it->second.imat >= 0) ibMatW = m_rec.afield[it->second.imat].ib;
    if (fOk && ibMatW > r.ibRecord && ibMatW + 48 <= r.ibRecord + cbSrc)
    {
        std::vector<uint8_t> ab;
        m_proj.readPart(relDest, ab);
        const std::size_t ibMat = ibNew + (ibMatW - r.ibRecord);
        if (ibMat + 48 <= ab.size())
        {
            glm::mat4 m = MatFromBytes(ab.data() + ibMat);
            if (ppos) m[3] = glm::vec4(*ppos, 1.0f);
            else m[3] += glm::vec4(250.0f, 0.0f, 0.0f, 0.0f);
            float a[12];
            if (FBytesFromMat(m, a)) m_proj.patch(relDest, ibMat, a, 48);
        }
    }
    m_proj.end();
    m_proj.end();
    if (!fOk) { if (m_proj.canUndo() && m_proj.history().back().label == label) m_proj.undo(); return 0; }
    Status(Fmt(T("created %s %d in %s"), SzClass(r.cid), oidNew, m_index.groups[igrpDest].name.c_str()) +
           (m_noteCopy.empty() ? std::string() : " (" + m_noteCopy + ")"));
    Reload(fSelect ? oidNew : 0);
    if (fSelect && fProxy)
        for (const auto& up : m_psw->aploAll)
            if (!up->paloParent && up->oidInstance == oidNew) { m_plSel = up.get(); break; }
    return oidNew;
}

bool CEditor::FMakeUnique(LO* plo, std::string& error)
{
    PLACE pl;
    if (!FPlace(plo, pl) || !pl.fProxy) { error = "only an instance placed by a proxy can be made unique"; return false; }
    LO* root = pl.ploLive;
    const int igrp = root->igrp;
    if (igrp < 0 || igrp >= int(m_index.groups.size())) { error = "its group is not known"; return false; }
    const ce::GroupInfo& g = m_index.groups[size_t(igrp)];
    // every top-level record of the shared group a proxy instance makes (k -2 ones are loaded once, not per instance):
    // copied into the level group, placed by the proxy
    std::vector<int> aiobjT;
    for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[i];
        if (r.depth != 0) continue;
        std::string rel;
        std::size_t ib = 0, cb = 0;
        if (!FRecordRange(int(i), rel, ib, cb) || rel != g.objectsPart) continue;
        const std::vector<uint8_t>* pab = m_proj.partRef(rel);
        if (!pab || ib >= pab->size() || FSharedRecord(pab->data() + ib, true)) continue;
        if (std::find_if(aiobjT.begin(), aiobjT.end(), [&](int j) { return m_rec.aobj[size_t(j)].ibRecord == r.ibRecord; }) != aiobjT.end()) continue;
        aiobjT.push_back(int(i));
    }
    if (aiobjT.empty()) { error = "the template's records are not found"; return false; }
    const int iobjP = pl.iobjrec;
    const int igrpDest = CopyGroup();
    if (igrpDest < 0) { error = "no level group"; return false; }
    const std::string relDest = m_index.groups[size_t(igrpDest)].objectsPart;
    std::string relP;
    std::size_t ibP = 0, cbP = 0;
    if (!FRecordRange(iobjP, relP, ibP, cbP)) { error = "the proxy's record is not in a binary part"; return false; }
    const glm::mat4 P = MatPlace(pl);
    const std::string label = "Make unique " + std::to_string(pl.oidProxy) + " (" + std::to_string(aiobjT.size()) + " objects)";
    m_proj.begin(label, true);
    int oidFirst = 0, cScripts = 0;
    bool fOk = true;
    for (int iobjT : aiobjT)
    {
        const EDITOBJREC& rt = m_rec.aobj[size_t(iobjT)];
        const int oidNew = OidNew();
        if (!oidFirst) oidFirst = oidNew;
        RECEDIT re;
        re.fDropScripts = true;   // (carried into the level group's scripts when it has them: RenumberCopy)
        std::vector<uint8_t> rec;
        m_igrpCopyDest = igrpDest;
        m_noteCopy.clear();
        fOk = FBuildCopy(iobjT, oidNew, re, rec, error);
        m_igrpCopyDest = -1;
        if (!fOk) break;
        if (m_noteCopy.find("carried") != std::string::npos) ++cScripts;
        MakeOwnRecord(rec.data());
        // its matrix: the proxy's placement of the template's own (SetLoMatrix pre-multiplies)
        const auto it = rt.plo ? m_mpobj.find(rt.plo) : m_mpobj.end();
        if (it != m_mpobj.end() && it->second.imat >= 0)
        {
            const std::size_t ibMatRel = m_rec.afield[size_t(it->second.imat)].ib - rt.ibRecord;
            if (ibMatRel + 48 <= rec.size())
            {
                float a[12];
                if (!FBytesFromMat(P * MatFromBytes(rec.data() + ibMatRel), a)) { error = "invalid matrix"; fOk = false; break; }
                std::memcpy(rec.data() + ibMatRel, a, 48);
            }
        }
        if (!(fOk = m_proj.appendRecord(relDest, rec, nullptr, error))) break;
    }
    if (fOk) fOk = m_proj.deleteRecord(relP, ibP, error);
    if (fOk) FBumpInstances(igrp, -1);
    m_proj.end();
    if (!fOk) { if (m_proj.canUndo() && m_proj.history().back().label == label) m_proj.undo(); return false; }
    Status("made unique: proxy " + std::to_string(pl.oidProxy) + " is now " + std::to_string(aiobjT.size()) + " object(s) from " + std::to_string(oidFirst) + " in " +
           m_index.groups[size_t(igrpDest)].name + (cScripts ? ", " + std::to_string(cScripts) + " with their scripts" : "") + " (its proxy's per-instance options are not carried)");
    Reload(oidFirst);
    return true;
}

bool CEditor::FDeleteNested(int iobjrec, std::string& error)
{
    const EDITOBJREC& r = m_rec.aobj[iobjrec];
    std::string rel, relList;
    std::size_t ib = 0, cb = 0, ibList = 0;
    if (r.depth == 0 || !FRecordRange(iobjrec, rel, ib, cb) || !FLocate(r.ibList, 2, relList, ibList) || relList != rel) { error = "its record is not in a binary part"; return false; }
    std::vector<uint8_t> ab;
    if (!m_proj.readPart(rel, ab) || ibList + 2 > ab.size()) { error = "cannot read " + rel; return false; }
    int16_t c;
    std::memcpy(&c, ab.data() + ibList, 2);
    if (c <= 0) { error = "its list is empty"; return false; }
    --c;
    const std::vector<std::size_t> aibSize = EnclosingSizeFields(rel, ib);
    m_proj.begin("Delete nested " + std::string(SzClass(r.cid)) + " " + std::to_string(r.oid), true);
    bool fOk = m_proj.patch(rel, ibList, &c, 2) && m_proj.spliceBytes(rel, ib, cb, {}, aibSize);
    m_proj.end();
    if (!fOk) error = "the record could not be removed";
    return fOk;
}

int CEditor::DuplicateNested(int iobjrec, std::string& error)
{
    const EDITOBJREC& r = m_rec.aobj[iobjrec];
    std::string rel, relList;
    std::size_t ib = 0, cb = 0, ibList = 0;
    if (r.depth == 0 || !FRecordRange(iobjrec, rel, ib, cb) || !FLocate(r.ibList, 2, relList, ibList) || relList != rel) { error = "its record is not in a binary part"; return 0; }
    std::vector<uint8_t> ab;
    if (!m_proj.readPart(rel, ab) || ibList + 2 > ab.size()) { error = "cannot read " + rel; return 0; }
    int16_t c;
    std::memcpy(&c, ab.data() + ibList, 2);
    if (c >= 32767) { error = "the list is full"; return 0; }
    ++c;
    const int oidNew = OidNew();
    RECEDIT re;
    std::vector<uint8_t> rec;
    if (!FBuildCopy(iobjrec, oidNew, re, rec, error)) return 0;
    const std::vector<std::size_t> aibSize = EnclosingSizeFields(rel, ib);
    m_proj.begin("Duplicate nested " + std::string(SzClass(r.cid)) + " " + std::to_string(r.oid), true);
    bool fOk = m_proj.patch(rel, ibList, &c, 2) && m_proj.spliceBytes(rel, ib + cb, 0, rec, aibSize);
    if (fOk) BumpProxiesIn(iobjrec, 1);
    m_proj.end();
    if (!fOk) { error = "the copy could not be inserted"; return 0; }
    return oidNew;
}

void CEditor::DrawCreate()
{
    // the target group (the level group by default)
    if (m_igrpCreate < 0) m_igrpCreate = LevelGroup();
    ImGui::SetNextItemWidth(-1);
    const std::string cur = m_igrpCreate >= 0 ? "Into: " + m_index.groups[m_igrpCreate].name : std::string("Into: choose a group");
    if (ImGui::BeginCombo("##into", cur.c_str()))
    {
        for (const ce::GroupInfo& g : m_index.groups)
            if (!g.objectsPart.empty() && ImGui::Selectable((g.name + (g.level ? T("  [level]") : "")).c_str(), g.index == m_igrpCreate)) m_igrpCreate = g.index;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pf", T("Filter: class, area or group"), m_filterCreate, sizeof(m_filterCreate));
    std::string filter = m_filterCreate;
    for (char& ch : filter) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // one example per class (placed, own record) and per shared group (a proxy)
    struct ENTRY { std::string name; GAMEAREA ga; int iobj; };
    // (built once per loaded world)
    static const SW* s_psw = nullptr;
    static std::size_t s_crec = 0;
    static std::vector<ENTRY> a;
    const bool fBuild = s_psw != m_psw.get() || s_crec != m_rec.aobj.size();
    if (fBuild) { a.clear(); s_psw = m_psw.get(); s_crec = m_rec.aobj.size(); }
    std::set<std::pair<int, int>> setCid;   // (class, area): a use for Sly and one for Murray are two entries
    std::set<int> setGroup;
    for (std::size_t i = 0; fBuild && i < m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[i];
        if (r.depth != 0) continue;
        if (FCidDerivedFrom(r.cid, EDCID(6)) && r.ibMatrix)
        {
            const int igrp = IgrpOfProxy(r.oid);
            if (igrp < 0 || setGroup.count(igrp)) continue;
            setGroup.insert(igrp);
            LO* root = nullptr;
            for (const auto& up : m_psw->aploAll)
                if (!up->paloParent && up->oidInstance == r.oid) { root = up.get(); break; }
            a.push_back({m_index.groups[igrp].name + "  (" + (root ? StrLo(root) : std::string("instance")) + ")", root ? AreaOfLo(root) : GA_None, int(i)});
        }
        else if (r.plo && FHasMatrix(r.plo) && glm::length(PosWorld(r.plo)) > 1.0f && r.plo->oidInstance < 0)
        {
            const GAMEAREA ga = AreaOfLo(r.plo);
            if (!setCid.insert({r.cid, int(ga)}).second) continue;
            std::string name = StrLo(r.plo);
            name = name.substr(0, name.find(' ')) + (name.find('(') != std::string::npos ? " " + name.substr(name.find('(')) : std::string());
            a.push_back({name, ga, int(i)});
        }
    }
    if (fBuild) std::sort(a.begin(), a.end(), [](const ENTRY& x, const ENTRY& y) { return x.ga != y.ga ? x.ga > y.ga : x.name < y.name; });
    ImGui::BeginChild("palette", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
    GAMEAREA gaPrev = GA_Max;
    for (const ENTRY& e : a)
    {
        std::string lower = e.name + " " + SzArea(e.ga);
        for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
        if (e.ga != gaPrev) { ImGui::SeparatorText(T(SzArea(e.ga))); gaPrev = e.ga; }
        ImGui::PushID(e.iobj);
        if (ImGui::Selectable(e.name.c_str(), m_iobjPalette == e.iobj)) m_iobjPalette = e.iobj;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("example: oid %d"), m_rec.aobj[std::size_t(e.iobj)].oid);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(m_iobjPalette < 0);
    if (ImGui::Button(T("Place in front of the camera")))
    {
        glm::vec3 p = m_cam.pos + m_cam.Dir() * 800.0f, hit;
        if (FSnapDown(p, hit)) p = hit;
        std::string error;
        if (!CopyRecord(m_iobjPalette, m_igrpCreate, &p, true, error)) Status("create: " + error);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_plSel);
    if (ImGui::Button(T("Make unique")))
    {
        std::string error;
        if (!FMakeUnique(m_plSel, error)) Status(std::string(T("make unique")) + ": " + error);
    }
    ImGui::EndDisabled();
}

}  // namespace edimpl

#endif
