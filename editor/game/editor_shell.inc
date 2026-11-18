// See editor.h. The map editor's 3D view: the world loaded by the game's loader with the editor's recorder on
// (editrec.h: where every object and its matrix / options were read), drawn by the game's renderer (scene.h /
// render.h, as --view-world), ImGui panels in the launcher's colours. Edits patch the world parts' bytes in the
// project (editor/core/project.h); the live objects are moved at once, option and structural edits show after a
// reload. Desktop only (SLY2_EDITOR / SLY3_EDITOR); every other build compiles the stub at the end. Shared by the games:
// each game's editor.cpp includes this file (its headers, its editor_game.h); class ids are Sly 2's numbers (EDCID).
#include "editor.h"
#include "editor_game.h"

#include <cstdio>

#if CANE_EDITOR_3D

#include "editor_impl.h"
#if EDGAME_SLY2
#include "splicenames.h"   // (the Scripts tab's scriptedit verb: the readable text kept)
#include "editor_scriptdiag.h"   // (scriptdiag / scriptfix)
#endif
#include "editor_font.h"
#include "texmod.h"

namespace edimpl
{

bool CEditor::FLocate(std::size_t ibWorld, std::size_t cb, std::string& rel, std::size_t& ibFile) const
{
    auto it = std::upper_bound(m_aseg.begin(), m_aseg.end(), ibWorld, [](std::size_t ib, const RAVELSEG& s) { return ib < s.ibWorld; });
    if (it == m_aseg.begin()) return false;
    --it;
    if (ibWorld + cb > it->ibWorld + it->cb) return false;
    rel = it->rel;
    ibFile = it->ibFile + (ibWorld - it->ibWorld);
    return true;
}

bool CEditor::FLoad(std::string& error)
{
    FK fk;
    int filter = 0;
    if (!FResolveWorld(std::to_string(m_eo.record), fk, filter, error)) return false;
    if (m_psw) m_apswOld.push_back(std::move(m_psw));
    m_psw = std::make_unique<SW>();
    m_psw->filter = filter;
    g_nFilterCur = m_eo.record;
    m_rec = EDITRECORDER();
    m_aseg.clear();
    g_peditrec = &m_rec;
    g_paravelseg = &m_aseg;
    std::size_t cbRead = 0, cbFile = 0;
    const bool fOk = FLoadWorldFile(m_psw.get(), fk, "", &cbRead, &cbFile);
    g_peditrec = nullptr;
    g_paravelseg = nullptr;
    if (!fOk) { error = "the world did not load"; return false; }
    std::sort(m_aseg.begin(), m_aseg.end(), [](const RAVELSEG& a, const RAVELSEG& b) { return a.ibWorld < b.ibWorld; });
    m_scene = SCENE();
    m_apalo.clear();
    ForgetThumbnails();
    CBuildSceneFromSw(m_psw.get(), m_scene, &m_apalo);
    ResetWorldLights();
    UpdateWorldLightsFromSw(m_psw.get());
    if (FHaveWorldLights()) ApplyLighting(m_scene, WorldLights());
    else ApproximateBakeLighting(m_scene);
    IndexObjects();
    m_fPhysReady = false;
    m_apsoeLevel.clear();
    return true;
}

void CEditor::IndexObjects()
{
    m_mptype.clear();
    if (g_gl.window) ForgetAssets();   // (their scene entries are the old scene's)
    m_aiHiddenEntries.clear();   // (a new scene: nothing hidden in it yet)
    InvalidateVis();
    m_mpobj.clear();
    m_mpgroupRoots.clear();
    m_mpchildren.clear();
    for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[i];
        if (!r.plo) continue;
        EDOBJ& o = m_mpobj[r.plo];
        if (o.iobjrec >= 0) continue;   // (the first record that made it)
        o.plo = r.plo;
        o.iobjrec = int(i);
        if (r.depth == 0) FLocate(r.ibRecord, r.cb + (r.ibBody - r.ibRecord), o.relRecord, o.ibRecordFile);
    }
    for (std::size_t i = 0; i < m_rec.afield.size(); ++i)
    {
        const EDITFIELD& f = m_rec.afield[i];
        EDOBJ& o = m_mpobj[f.plo];
        o.plo = f.plo;
        if (f.fk == EDITFK_Option) o.aiopt.push_back(int(i));
        else if (f.fk == EDITFK_Path) { if (o.ipath < 0) o.ipath = int(i); }
        else if (f.fk == EDITFK_TaskLinks) { if (o.ilinks < 0) o.ilinks = int(i); }
        else if (f.fk == EDITFK_OptionArray) o.aiarr.push_back(int(i));
        else if (f.fk == EDITFK_Globset) { if (o.iglobset < 0) o.iglobset = int(i); }
        else if (f.fk == EDITFK_Soes) { if (o.isoes < 0) o.isoes = int(i); }
        else if ((f.fk == EDITFK_XfMatrix && f.i == 0) || f.fk == EDITFK_LoxMatrix)
            if (o.imat < 0) o.imat = int(i);
    }
    m_mpoidProxy.clear();
    m_mpProxyApplied.clear();
    for (std::size_t i = 0; i < m_rec.aobj.size(); ++i)
    {
        const EDITOBJREC& r = m_rec.aobj[i];
        if (r.depth != 0 || !r.ibMatrix || !FCidDerivedFrom(r.cid, EDCID(6)) || m_mpoidProxy.count(r.oid)) continue;
        std::string rel;
        std::size_t ib = 0;
        std::vector<uint8_t> ab;
        if (!FLocate(r.ibMatrix, 48, rel, ib) || !m_proj.readPart(rel, ab) || ib + 48 > ab.size()) continue;
        m_mpoidProxy[r.oid] = int(i);
        m_mpProxyApplied[r.ibMatrix] = MatFromBytes(ab.data() + ib);
    }
    for (const auto& up : m_psw->aploAll)
    {
        LO* plo = up.get();
        if (plo->paloParent) m_mpchildren[plo->paloParent].push_back(plo);
        else m_mpgroupRoots[plo->igrp].push_back(plo);
    }
    FindLoadParents();
}

// A LOX without a transform parent whose live matrix is not its stored one was put under another object at the load
// (e.g. a GROUPER's effects: stored relative to the group, live in world space). Its parent: the placed object whose
// world matrix is live x stored^-1. The editor keeps the relation: edits store P^-1 x live, and the parent's moves
// carry it (as the game will at the next load).
void CEditor::FindLoadParents()
{
    m_mpLoadParent.clear();
    m_mpLoadKids.clear();
    std::vector<std::pair<LO*, glm::mat4>> akid;
    for (const auto& [plo, o] : m_mpobj)
    {
        if (o.imat < 0 || FIsLoDerivedFrom(const_cast<LO*>(plo), EDCID(0x30)) || !FIsLoDerivedFrom(const_cast<LO*>(plo), EDCID(3))) continue;
        if (static_cast<const LOX*>(plo)->pxf) continue;
        const LO* root = plo;
        while (root->paloParent) root = root->paloParent;
        if (root->oidInstance >= 0 && m_mpoidProxy.count(root->oidInstance)) continue;
        std::string rel;
        std::size_t ib = 0;
        const std::vector<uint8_t>* pab = nullptr;
        if (!FLocate(m_rec.afield[std::size_t(o.imat)].ib, 48, rel, ib) || !(pab = m_proj.partRef(rel)) || ib + 48 > pab->size()) continue;
        const glm::mat4 stored = MatFromBytes(pab->data() + ib), live = MatLocal(plo);
        float d = 0;
        for (int i = 0; i < 4; ++i) d = std::max(d, glm::length(live[i] - stored[i]));
        if (d < 0.01f || std::abs(glm::determinant(glm::mat3(stored))) < 1e-8f) continue;
        akid.emplace_back(const_cast<LO*>(plo), live * glm::inverse(stored));
    }
    if (akid.empty()) return;
    // the parent: an object stored in the world file whose world matrix is P (the nearest of the same group first)
    for (const auto& [pkid, P] : akid)
    {
        LO* pbest = nullptr;
        for (const auto& up : m_psw->aploAll)
        {
            LO* q = up.get();
            if (q == pkid || !FHasMatrix(q) || m_mpLoadParent.count(q)) continue;
            const glm::mat4 W = MatWorld(q);
            float d = 0;
            for (int i = 0; i < 4; ++i) d = std::max(d, glm::length(W[i] - P[i]));
            if (d < 0.05f && (!pbest || (q->igrp == pkid->igrp && pbest->igrp != pkid->igrp))) pbest = q;
        }
        LOADPARENT lp;
        lp.ploParent = pbest;
        lp.P = P;
        m_mpLoadParent[pkid] = lp;
        if (pbest) m_mpLoadKids[pbest].push_back(pkid);
    }
}

bool CEditor::FHasMatrix(const LO* plo) const
{
    if (!plo) return false;
    if (FIsLoDerivedFrom(plo, EDCID(0x30))) return !static_cast<const ALO*>(plo)->axf.empty();
    return FIsLoDerivedFrom(plo, EDCID(3));
}

glm::mat4 CEditor::MatLocal(const LO* plo) const
{
    if (FIsLoDerivedFrom(plo, EDCID(0x30)))
    {
        const ALO* palo = static_cast<const ALO*>(plo);
        return palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->mat;
    }
    if (FIsLoDerivedFrom(plo, EDCID(3))) return static_cast<const LOX*>(plo)->mat;
    return glm::mat4(1.0f);
}

glm::mat4 CEditor::MatWorld(const LO* plo) const
{
    if (FIsLoDerivedFrom(plo, EDCID(0x30))) return MatWorldOfAlo(static_cast<const ALO*>(plo));
    if (FIsLoDerivedFrom(plo, EDCID(3)))
    {
        const LOX* plox = static_cast<const LOX*>(plo);
        return (plox->pxf ? plox->pxf->matWorld : glm::mat4(1.0f)) * plox->mat;
    }
    return glm::mat4(1.0f);
}

void CEditor::SetLocal(LO* plo, const glm::mat4& m)
{
    if (FIsLoDerivedFrom(plo, EDCID(0x30)))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (palo->axf.empty()) return;
        palo->axf[0]->mat = m;
        palo->axf[0]->matLoad = m;
        UpdateAloHierarchyWorlds(palo);
        UpdateSceneFromSw(m_scene, m_apalo);
    }
    else if (FIsLoDerivedFrom(plo, EDCID(3))) static_cast<LOX*>(plo)->mat = m;
    // the objects attached to it at the load follow (their stored matrices under its new world)
    if (const auto it = m_mpLoadKids.find(plo); it != m_mpLoadKids.end())
    {
        const glm::mat4 W = MatWorld(plo);
        for (LO* pk : it->second)
        {
            LOADPARENT& lp = m_mpLoadParent[pk];
            const glm::mat4 stored = glm::inverse(lp.P) * MatLocal(pk);
            lp.P = W;
            SetLocal(pk, W * stored);
        }
    }
}

void CEditor::SetWorld(LO* plo, const glm::mat4& w)
{
    const glm::mat4 parent = MatWorld(plo) * glm::inverse(MatLocal(plo));
    SetLocal(plo, glm::inverse(parent) * w);
}

// The object's local matrix into its part (12 floats: the 3 x 3, then the translation)
bool CEditor::FPlace(LO* plo, PLACE& pl) const
{
    pl = PLACE();
    if (!plo) return false;
    LO* root = plo;
    while (root->paloParent) root = root->paloParent;
    if (root->oidInstance >= 0)
        if (const auto it = m_mpoidProxy.find(root->oidInstance); it != m_mpoidProxy.end() && FHasMatrix(root))
        {
            pl.ploLive = root;
            pl.fProxy = true;
            pl.oidProxy = root->oidInstance;
            pl.iobjrec = it->second;
            pl.ibMat = m_rec.aobj[it->second].ibMatrix;
            return true;
        }
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end() || it->second.imat < 0) return false;
    pl.ploLive = plo;
    pl.ibMat = m_rec.afield[it->second.imat].ib;
    if (LO* ptop = PloTopLevel(plo)) pl.iobjrec = m_mpobj.at(ptop).iobjrec;
    return true;
}

glm::mat4 CEditor::MatPlace(const PLACE& pl) const
{
    if (!pl.fProxy) return MatLocal(pl.ploLive);
    const auto it = m_mpProxyApplied.find(pl.ibMat);
    return it == m_mpProxyApplied.end() ? glm::mat4(1.0f) : it->second;
}

void CEditor::SetPlace(const PLACE& pl, const glm::mat4& m)
{
    if (!pl.fProxy) { SetLocal(pl.ploLive, m); return; }
    // every root the proxy placed (an instance: its group's objects) by the change in the proxy's matrix
    const glm::mat4 d = m * glm::inverse(MatPlace(pl));
    for (const auto& up : m_psw->aploAll)
        if (!up->paloParent && up->oidInstance == pl.oidProxy && FHasMatrix(up.get())) SetWorld(up.get(), d * MatWorld(up.get()));
    m_mpProxyApplied[pl.ibMat] = m;
}

void CEditor::SetPlaceWorld(const PLACE& pl, const glm::mat4& w)
{
    if (!pl.fProxy) SetWorld(pl.ploLive, w);
    else SetPlace(pl, w * glm::inverse(MatWorld(pl.ploLive)) * MatPlace(pl));
}

bool CEditor::FRecordPlace(const PLACE& pl, const std::string& label)
{
    std::string rel;
    std::size_t ib = 0;
    if (!FLocate(pl.ibMat, 48, rel, ib)) { Status("this object's part is not a binary part"); return false; }
    float a[12];
    glm::mat4 mStore = MatPlace(pl);
    if (!pl.fProxy)
        if (const auto it = m_mpLoadParent.find(pl.ploLive); it != m_mpLoadParent.end()) mStore = glm::inverse(it->second.P) * mStore;
    if (!FBytesFromMat(mStore, a)) { Status("rejected: the transform has invalid numbers"); return false; }
    m_proj.begin(label);
    const bool fOk = m_proj.patch(rel, ib, a, sizeof(a));
    m_proj.end();
    return fOk;
}

bool CEditor::FRecordLocal(LO* plo, const std::string& label)
{
    PLACE pl;
    if (FPlace(plo, pl)) return FRecordPlace(pl, label);
    Status("this object's transform is not stored in the world file (made at run time)");
    return false;
}

void CEditor::ReapplyFromParts()
{
    std::map<std::string, std::vector<uint8_t>> mpPart;
    for (auto& [plo, o] : m_mpobj)
    {
        if (o.imat < 0) continue;
        // (an instance placed by a proxy: moved by the proxy's matrix below, not by its own record)
        const LO* root = plo;
        while (root->paloParent) root = root->paloParent;
        if (root->oidInstance >= 0 && m_mpoidProxy.count(root->oidInstance)) continue;
        std::string rel;
        std::size_t ib = 0;
        if (!FLocate(m_rec.afield[o.imat].ib, 48, rel, ib)) continue;
        auto itp = mpPart.find(rel);
        if (itp == mpPart.end()) { itp = mpPart.emplace(rel, std::vector<uint8_t>()).first; m_proj.readPart(rel, itp->second); }
        if (ib + 48 > itp->second.size()) continue;
        float a[12];
        std::memcpy(a, itp->second.data() + ib, 48);
        glm::mat4 m(1.0f);
        for (int i = 0; i < 3; ++i) m[i] = glm::vec4(a[3 * i], a[3 * i + 1], a[3 * i + 2], 0.0f);
        m[3] = glm::vec4(a[9], a[10], a[11], 1.0f);
        if (m_mpLoadParent.count(plo)) continue;   // (below, after their parents)
        if (m != MatLocal(plo)) SetLocal(const_cast<LO*>(plo), m);
    }
    for (auto& [plo, lp] : m_mpLoadParent)
    {
        const auto ito = m_mpobj.find(plo);
        std::string rel;
        std::size_t ib = 0;
        const std::vector<uint8_t>* pab = nullptr;
        if (ito == m_mpobj.end() || !FLocate(m_rec.afield[std::size_t(ito->second.imat)].ib, 48, rel, ib) || !(pab = m_proj.partRef(rel)) || ib + 48 > pab->size()) continue;
        if (lp.ploParent) lp.P = MatWorld(lp.ploParent);
        const glm::mat4 m = lp.P * MatFromBytes(pab->data() + ib);
        if (m != MatLocal(plo)) SetLocal(const_cast<LO*>(plo), m);
    }
    for (const auto& [oid, iobj] : m_mpoidProxy)
    {
        PLACE pl;
        pl.fProxy = true;
        pl.oidProxy = oid;
        pl.ibMat = m_rec.aobj[iobj].ibMatrix;
        std::string rel;
        std::size_t ib = 0;
        std::vector<uint8_t> ab;
        if (!FLocate(pl.ibMat, 48, rel, ib) || !m_proj.readPart(rel, ab) || ib + 48 > ab.size()) continue;
        const glm::mat4 m = MatFromBytes(ab.data() + ib);
        if (m != MatPlace(pl)) SetPlace(pl, m);
    }
}

LO* CEditor::PloTopLevel(LO* plo) const
{
    for (LO* p = plo; p; p = p->paloParent)
        if (const auto it = m_mpobj.find(p); it != m_mpobj.end() && !it->second.relRecord.empty()) return p;
    return nullptr;
}

void CEditor::Reload(int oidSelect)
{
    std::string error;
    const CAMERA cam = m_cam;
    const std::vector<uint64_t> akeySel = KeysOfSelection();
    m_asel.clear();
    if (!FLoad(error))
    {
        // (a structural edit the loader refuses: undone, and the world as before; a half-loaded world is not drawn)
        const ce::Step* ps = m_proj.undo();
        std::string error2;
        if (!FLoad(error2)) { std::fprintf(stderr, "editor: the world does not load any more: %s\n", error2.c_str()); std::exit(1); }
        BuildSceneGpu(m_scene);
        m_cam = cam;
        m_plSel = nullptr;
        Status("the game's loader refused the edit" + std::string(ps ? " (" + ps->label + ")" : "") + ", undone: " + error);
        return;
    }
    BuildSceneGpu(m_scene);
    m_cam = cam;
    m_plSel = nullptr;
    if (oidSelect)
    {
        for (const auto& up : m_psw->aploAll)
            if (up->oid == oidSelect && (!m_plSel || FHasMatrix(up.get()))) m_plSel = up.get();
        if (m_plSel) m_asel.push_back(m_plSel);
    }
    else
    {
        // (the selection as it was, by oid and instance, when its objects are still there)
        SelectKeys(akeySel);
    }
}

void CEditor::Duplicate()
{
#if !EDFEAT_STRUCT
    Status("duplicate: not for " EDGAME_NAME " yet");
#else
    PLACE pl;
    const bool fPlace = FPlace(m_plSel, pl);
    int iobj = fPlace ? pl.iobjrec : -1;
    if (iobj < 0)
        if (LO* ptop = PloTopLevel(m_plSel)) iobj = m_mpobj.at(ptop).iobjrec;
    // a nested object (inside another's record): copied next to itself in its parent's list
    if (m_plSel && !(fPlace && pl.fProxy))
        if (const auto it = m_mpobj.find(m_plSel); it != m_mpobj.end() && it->second.iobjrec >= 0 && m_rec.aobj[it->second.iobjrec].depth > 0 && PloTopLevel(m_plSel) != m_plSel)
        {
            std::string error;
            const int oidNew = DuplicateNested(it->second.iobjrec, error);
            if (!oidNew) { Status("duplicate: " + error); return; }
            Status("duplicated nested " + std::string(SzClass(m_plSel->Cid())) + " " + std::to_string(m_plSel->oid) + " as oid " + std::to_string(oidNew));
            Reload(oidNew);
            return;
        }
    if (iobj < 0) { Status("only a group's top-level objects and placed instances can be duplicated"); return; }
    const EDITOBJREC& r = m_rec.aobj[iobj];
    std::string rel;
    std::size_t ibRec = 0;
    if (!FLocate(r.ibRecord, r.ibBody - r.ibRecord, rel, ibRec)) { Status("its record is not in a binary part"); return; }
    const int oidNew = OidNew();
    std::string error;
    std::size_t ibNew = 0;
    m_proj.begin("Duplicate " + std::string(SzClass(r.cid)) + " " + std::to_string(r.oid), true);
    bool fOk = m_proj.duplicateRecord(rel, ibRec, oidNew, &ibNew, error);
    // a proxy places one more instance of its group: the group's instance count (World/Groups.bin: S16 count, then
    // per group 64 bytes of name and the S32s x5C x60 x10 x14 cinst) one higher, so the loader has an instance for it
    // one more instance of each shared group the record's proxies place (World/Groups.bin), or the loader has none for it
    if (fOk) BumpProxiesIn(iobj, 1);
    // the copy a little to the side, so it can be seen (its matrix at the same place in the record)
    std::string relMat;
    std::size_t ibMat = 0;
    if (fOk && fPlace && pl.iobjrec == iobj && FLocate(pl.ibMat, 48, relMat, ibMat) && relMat == rel && ibMat > ibRec && ibMat < ibRec + r.cb + (r.ibBody - r.ibRecord))
    {
        std::vector<uint8_t> ab;
        m_proj.readPart(rel, ab);
        const std::size_t ibMatNew = ibNew + (ibMat - ibRec);
        glm::mat4 m = MatFromBytes(ab.data() + ibMatNew);
        const glm::mat4 parent = pl.fProxy ? glm::mat4(1.0f) : MatWorld(pl.ploLive) * glm::inverse(MatLocal(pl.ploLive));
        m[3] += glm::inverse(parent) * glm::vec4(250.0f, 0.0f, 0.0f, 0.0f);
        float a[12];
        if (FBytesFromMat(m, a)) m_proj.patch(rel, ibMatNew, a, 48);
    }
    m_proj.end();
    if (!fOk) { Status("duplicate: " + error); return; }
    Status("duplicated " + std::string(SzClass(r.cid)) + " " + std::to_string(r.oid) + " as oid " + std::to_string(oidNew));
    Reload(oidNew);
    // (a duplicated proxy: select the instance it now places)
    if (!m_plSel)
        for (const auto& up : m_psw->aploAll)
            if (!up->paloParent && up->oidInstance == oidNew) { m_plSel = up.get(); break; }
#endif
}

void CEditor::Delete()
{
#if !EDFEAT_STRUCT
    Status("delete: not for " EDGAME_NAME " yet");
#else
    PLACE pl;
    const bool fPlace = FPlace(m_plSel, pl);
    int iobj = fPlace ? pl.iobjrec : -1;
    if (m_plSel && !(fPlace && pl.fProxy))
        if (const auto it = m_mpobj.find(m_plSel); it != m_mpobj.end() && it->second.iobjrec >= 0 && m_rec.aobj[it->second.iobjrec].depth > 0 && PloTopLevel(m_plSel) != m_plSel)
        {
            std::string error;
            const std::string what = std::string(SzClass(m_plSel->Cid())) + " " + std::to_string(m_plSel->oid);
            if (!FDeleteNested(it->second.iobjrec, error)) { Status("delete: " + error); return; }
            Status("deleted nested " + what);
            Reload(0);
            return;
        }
    if (iobj < 0)
        if (LO* ptop = PloTopLevel(m_plSel)) iobj = m_mpobj.at(ptop).iobjrec;
    if (iobj < 0) { Status("only objects stored in the world file can be deleted"); return; }
    const EDITOBJREC& r = m_rec.aobj[iobj];
    std::string rel;
    std::size_t ibRec = 0;
    if (!FLocate(r.ibRecord, r.ibBody - r.ibRecord, rel, ibRec)) { Status("its record is not in a binary part"); return; }
    std::string error;
    const int oid = r.oid, cid = r.cid;
    m_proj.begin("Delete " + std::string(SzClass(cid)) + " " + std::to_string(oid), true);
    const bool fOk = m_proj.deleteRecord(rel, ibRec, error);
    m_proj.end();
    if (!fOk) { Status("delete: " + error); return; }
    Status("deleted " + std::string(SzClass(cid)) + " " + std::to_string(oid) + " (scripts or objects that name it may now miss it)");
    Reload(0);
#endif
}

void CEditor::Undo()
{
    const ce::Step* ps = m_proj.undo();
    if (!ps) return;
    Status("undid: " + ps->label);
    if (ps->structural) { Reload(m_plSel ? m_plSel->oid : 0); ReloadText(); }
    else { ReapplyFromParts(); ReapplyPathsFromParts(); }
    if (m_igroupScript >= 0 && !m_fScriptDirty) LoadScriptText(m_igroupScript);
}

void CEditor::Redo()
{
    const ce::Step* ps = m_proj.redo();
    if (!ps) return;
    Status("redid: " + ps->label);
    if (ps->structural) { Reload(m_plSel ? m_plSel->oid : 0); ReloadText(); }
    else { ReapplyFromParts(); ReapplyPathsFromParts(); }
    if (m_igroupScript >= 0 && !m_fScriptDirty) LoadScriptText(m_igroupScript);
}

bool CEditor::FSave()
{
    // the checks before a save (missing references): asked about in the window, printed in automated runs
    m_aproblem = Validate();
    for (const PROBLEM& p : m_aproblem) std::printf("editor: problem: %s\n", p.text.c_str());
    if (!m_aproblem.empty() && !m_fSaveAnyway && m_ascript.empty() && !m_eo.fHidden)
    {
        m_fProblemsOpen = true;
        return false;
    }
    m_fSaveAnyway = false;
#if EDFEAT_CUSTOMTASKS
    // the world's custom tasks (new jobs' tasks: not in the installed world, no save key of their own) listed in
    // mod.json: the game keeps their states next to the save slot (modsave.h)
    {
        std::set<int> setInstalled;
        for (const ce::GroupInfo& g : m_index.groups)
            for (const ce::ObjRecord& r : g.objects) setInstalled.insert(r.oid);
        std::vector<int> aoid;
        for (const EDITOBJREC& r : m_rec.aobj)
            if (r.cid == EDCID(0x85) && r.plo && !setInstalled.count(r.oid) && IfieldOption(r.plo, EDEOPID(1298)) < 0 &&
                std::find(aoid.begin(), aoid.end(), r.oid) == aoid.end())
                aoid.push_back(r.oid);
        std::sort(aoid.begin(), aoid.end());
        cane::mods::setCustomTasks(m_proj.manifest, m_index.entry.name, aoid);
        if (!aoid.empty())
        {
            std::string str;
            for (int oid : aoid) str += " " + std::to_string(oid);
            std::printf("editor: custom tasks saved with progress:%s\n", str.c_str());
        }
    }
#endif
    std::string error;
    if (!m_proj.save(error)) { Status("save failed: " + error); return false; }
    Status("saved " + std::to_string(m_proj.changedParts().size()) + " changed parts to " + ce::utf8(m_proj.dir) +
           (m_aproblem.empty() ? std::string() : " (" + std::to_string(m_aproblem.size()) + " problems)"));
    return true;
}

void CEditor::PlayTestAt(int oidTask, const glm::vec3* ppos, float heading)
{
    if (!FSave()) return;
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    std::vector<std::string> args{"--data-dir", ce::utf8(m_inst.dir), "--user-dir", cane::utf8(cane::userDirectory()), "--mod-package",
                                  ce::utf8(m_proj.dir), "--world", std::to_string(m_eo.record) + (oidTask > 0 ? ":" + std::to_string(oidTask) : std::string())};
    if (!m_playMapState.empty()) args.insert(args.end(), {"--map-state", m_playMapState});   // (the States tab's Play)
    if (ppos)
    {
        char sz[96];
        std::snprintf(sz, sizeof(sz), "%.1f,%.1f,%.1f,%.4f", ppos->x, ppos->y, ppos->z, heading);
        args.insert(args.end(), {"--start-pos", sz});
    }
    if (m_fPlayDry)
    {
        std::string line;
        for (std::size_t i = 4; i < args.size(); ++i) line += (i > 4 ? " " : "") + args[i];
        std::printf("editor: play: %s\n", line.c_str());
        return;
    }
    if (!ce::spawnDetached(exe, args)) Status("could not start " + ce::utf8(exe));
    else if (oidTask > 0) Status(std::string(T("play test started at task")) + " " + std::to_string(oidTask));
    else Status("play test started (" + m_index.entry.name + " with this project loaded)");
}

int CEditor::TaskToPlay(LO* plo) const
{
    if (!plo) return 0;
    int n = 0;
    if (TypeOfLo(plo) == OT_Job && FOptionInt(plo, EDEOPID(1243), n) && n > 0) return n;
    if (TypeOfLo(plo) == OT_Task) return plo->oid;
    return 0;
}

void CEditor::PlayFromView()
{
    // the ground below the point 600 ahead of the view (else that point), Sly facing the view's way
    glm::vec3 p = m_cam.pos + m_cam.Dir() * 600.0f, hit;
    if (FSnapDown(p, hit)) p = hit + glm::vec3(0, 0, 5);
    const glm::vec3 d = m_cam.Dir();
    PlayTestAt(-1, &p, std::atan2(d.y, d.x));
}

void CEditor::DrawPlayMenu()
{
    if (!ImGui::BeginPopup("playmenu")) return;
    if (ImGui::MenuItem(T("Play the world"))) PlayTest();
    if (ImGui::MenuItem(T("Play from here (Sly in front of the view)"))) PlayFromView();
    const int oidSel = TaskToPlay(m_plSel);
    if (ImGui::MenuItem(T("Play the selected job / task"), nullptr, false, oidSel > 0)) PlayTestAt(oidSel, nullptr, 0.0f);
    if (ImGui::BeginMenu(T("Play a job")))
    {
        for (const auto& up : m_psw->aploAll)
            if (TypeOfLo(up.get()) == OT_Job)
                if (const int t = TaskToPlay(up.get()); t > 0)
                {
                    char label[160];
                    std::snprintf(label, sizeof(label), "%s  (%d)", NameOfObject(up.get()).c_str(), up->oid);
                    if (ImGui::BeginMenu(label))
                    {
                        if (ImGui::MenuItem(T("From its start"))) PlayTestAt(t, nullptr, 0.0f);
                        // its tasks in the chain order (next-task links from the first)
                        std::set<int> setSeen;
                        int oid = t;
                        for (int k = 0; k < 64 && oid > 0 && setSeen.insert(oid).second; ++k)
                        {
                            LO* pt = PloOid(oid, EDCID(0x85));
                            if (!pt) break;
                            int idPrompt = 0;
                            std::string s = std::string(T("From task")) + " " + std::to_string(oid);
                            if (FOptionInt(pt, EDEOPID(1265), idPrompt) && idPrompt > 0)
                                if (const std::string tx = StrText(idPrompt); !tx.empty()) s += ": " + tx.substr(0, 60);
                            if (ImGui::MenuItem((s + "##t" + std::to_string(oid)).c_str())) PlayTestAt(oid, nullptr, 0.0f);
                            TASKLINKS tl;
                            oid = EDFEAT_MISSIONS && FTaskLinks(pt, tl) && !tl.aoidPost.empty() ? tl.aoidPost[0] : 0;
                        }
                        ImGui::EndMenu();
                    }
                }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

// ---- view

bool CEditor::FProject(const glm::vec3& p, ImVec2& s) const
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const glm::vec4 c = m_cam.MatWorldToClip(size.x / std::max(1.0f, size.y)) * glm::vec4(p, 1.0f);
    if (c.w <= 1e-3f) return false;
    s = ImVec2((c.x / c.w * 0.5f + 0.5f) * size.x, (0.5f - c.y / c.w * 0.5f) * size.y);
    return true;
}

glm::vec3 CEditor::RayDir(float x, float y) const
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const glm::mat4 inv = glm::inverse(m_cam.MatWorldToClip(size.x / std::max(1.0f, size.y)));
    const float nx = x / size.x * 2.0f - 1.0f, ny = 1.0f - y / size.y * 2.0f;
    glm::vec4 a = inv * glm::vec4(nx, ny, 1.0f, 1.0f), b = inv * glm::vec4(nx, ny, 0.5f, 1.0f);
    return glm::normalize(glm::vec3(b) / b.w - glm::vec3(a) / a.w);
}

void CEditor::Pick(float x, float y)
{
    if (FBrushPick(x, y)) return;   // (editor_brushedit.inc: a face of the brush being edited)
    const glm::vec3 o = m_cam.pos, d = RayDir(x, y);
    LO* pbest = nullptr;
    float rBest = 1e30f, tBest = 1e30f;
    auto consider = [&](LO* plo, const glm::vec3& c, float r) {
        const glm::vec3 oc = c - o;
        const float t = glm::dot(oc, d);
        if (t < 0 && glm::length(oc) > r) return;
        const float dist2 = glm::dot(oc, oc) - t * t;
        if (dist2 > r * r) return;
        // the smallest sphere the ray goes through (props over the level around them), then the nearest
        if (r < rBest * 0.8f || (r < rBest * 1.25f && t < tBest)) { pbest = plo; rBest = r; tBest = t; }
    };
    for (std::size_t k = 0; k < m_scene.aglobset.size() && k < m_apalo.size(); ++k)
    {
        const SCENEGLOBSET& sgs = m_scene.aglobset[k];
        ALO* palo = m_apalo[k];
        if (!palo || !sgs.pglobset || FHidden(palo)) continue;
        for (std::size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
        {
            const GLOB& g = sgs.pglobset->aglob[i];
            if (g.sRadius <= 0) continue;
            const glm::mat4 m = i < sgs.amatGlob.size() ? sgs.amatGlob[i] : MatWorldOfAlo(palo);
            consider(palo, glm::vec3(m * glm::vec4(g.posCenter, 1.0f)), g.sRadius);
        }
    }
    // the markers (objects without a model): within 12 pixels
    if (m_fShowMarkers)
        for (const auto& up : m_psw->aploAll)
        {
            LO* plo = up.get();
            if (FIsLoDerivedFrom(plo, EDCID(0x30)) || !FIsLoDerivedFrom(plo, EDCID(3)) || FHidden(plo)) continue;
            ImVec2 s;
            const glm::vec3 p = PosWorld(plo);
            if (glm::length(p) < 0.01f || glm::length(p - m_cam.pos) > 8000.0f || !FProject(p, s)) continue;
            if (std::hypot(s.x - x, s.y - y) < 12.0f) { pbest = plo; break; }
        }
    // Ctrl: toggled in / out of the selection; Shift: added; else the selection is the picked object
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) { if (pbest) ToggleSelected(pbest); }
    else if (io.KeyShift) { if (pbest) AddSelected(pbest); }
    else SelectOnly(pbest);
    m_fOutScroll = true;
}

void CEditor::FrameSelected()
{
    // several: their placed middle and spread (the shared groups' templates at the origin left out)
    if (m_asel.size() > 1 && !m_fCurveMode)
    {
        glm::vec3 lo(1e30f), hi(-1e30f);
        int c = 0;
        for (LO* p : m_asel)
            if (FHasMatrix(p))
                if (const glm::vec3 q = PosWorld(p); glm::length(q) > 0.01f) { lo = glm::min(lo, q); hi = glm::max(hi, q); ++c; }
        if (c > 1)
        {
            m_cam.pitch = -35.0f;
            m_cam.pos = (lo + hi) * 0.5f - m_cam.Dir() * std::clamp(glm::length(hi - lo) * 1.2f + 600.0f, 600.0f, 30000.0f);
            return;
        }
    }
    if (!m_plSel || !FHasMatrix(m_plSel)) return;
    if (PATHDATA pd; FPathRead(m_plSel, pd) && !pd.apos.empty())
    {
        const glm::mat4 m = MatPathSpace(m_plSel);
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const glm::vec3& q : pd.apos) { const glm::vec3 w = glm::vec3(m * glm::vec4(q, 1.0f)); lo = glm::min(lo, w); hi = glm::max(hi, w); }
        m_cam.pitch = -30.0f;
        m_cam.pos = (lo + hi) * 0.5f - m_cam.Dir() * std::clamp(glm::length(hi - lo) * 1.1f, 500.0f, 6000.0f);
        return;
    }
    const float dist = m_plSel->sRadius < 1e5f ? std::clamp(m_plSel->sRadius * 3.0f, 500.0f, 2500.0f) : 900.0f;
    m_cam.pitch = -35.0f;
    m_cam.pos = PosWorld(m_plSel) - m_cam.Dir() * dist;
}

void CEditor::CameraInput(float dt)
{
    ImGuiIO& io = ImGui::GetIO();
    GLFWwindow* w = g_gl.window;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.WantCaptureMouse)
    {
        m_cam.yaw -= io.MouseDelta.x * 0.2f;
        m_cam.pitch = std::clamp(m_cam.pitch - io.MouseDelta.y * 0.2f, -89.0f, 89.0f);
    }
    if (!io.WantCaptureMouse && io.MouseWheel != 0) m_speed = std::clamp(m_speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 50.0f, 20000.0f);
    if (io.WantCaptureKeyboard) return;
    const bool fFly = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const float speed = m_speed * (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) ? 4.0f : 1.0f) * dt;
    const glm::vec3 fwd = m_cam.Dir(), right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
    if (fFly)
    {
        if (glfwGetKey(w, GLFW_KEY_W)) m_cam.pos += fwd * speed;
        if (glfwGetKey(w, GLFW_KEY_S)) m_cam.pos -= fwd * speed;
        if (glfwGetKey(w, GLFW_KEY_D)) m_cam.pos += right * speed;
        if (glfwGetKey(w, GLFW_KEY_A)) m_cam.pos -= right * speed;
        if (glfwGetKey(w, GLFW_KEY_E)) m_cam.pos.z += speed;
        if (glfwGetKey(w, GLFW_KEY_Q)) m_cam.pos.z -= speed;
    }
    else
    {
        // Q / W / E / R: select / move / rotate / scale (as other editors), F: frame, H / Alt+H / I: hide / show all /
        // isolate, Delete, Ctrl+A / C / V / D / Z / Y / S, [ / ]: the grid size
        const bool ctrl = io.KeyCtrl;
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Q)) m_tool = 0;
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_W)) { m_tool = 1; m_gizmo = 0; }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_E)) { m_tool = 1; m_gizmo = 1; }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_R)) { m_tool = 1; m_gizmo = 2; }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) FrameSelected();
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_H)) { if (io.KeyAlt) ShowAll(); else HideSelection(true); }
        if (!ctrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I)) { if (m_setIsolate.empty()) IsolateSelection(); else { m_setIsolate.clear(); InvalidateVis(); } }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) { m_gridSize = std::max(1.0f, m_gridSize * 0.5f); SaveViewState(); }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { m_gridSize = std::min(1024.0f, m_gridSize * 2.0f); SaveViewState(); }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Escape)) ClearSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelection();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) SelectWhere([this](LO* p) { return FSelectable(p) && glm::length(PosWorld(p)) > 0.01f; }, false);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) CopySelection();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) Paste(io.KeyShift);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelection();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) io.KeyShift ? Redo() : Undo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) FSave();
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) m_fShowHelp = !m_fShowHelp;
    }
}

void CEditor::DrawMarkers()
{
    if (!m_fShowMarkers) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const auto& up : m_psw->aploAll)
    {
        LO* plo = up.get();
        const bool fSelAny = FSelected(plo);
        if ((FIsLoDerivedFrom(plo, EDCID(0x30)) && !fSelAny) || !FIsLoDerivedFrom(plo, EDCID(3)) || FHidden(plo)) continue;
        const glm::vec3 p = PosWorld(plo);
        // (the shared groups' unplaced templates sit at the origin: not shown)
        if (glm::length(p) < 0.01f || glm::length(p - m_cam.pos) > (fSelAny ? 60000.0f : 8000.0f)) continue;
        ImVec2 s;
        if (!FProject(p, s)) continue;
        const bool fSel = plo == m_plSel;
        const ImVec2 m = ImGui::GetIO().MousePos;
        const bool fHover = std::hypot(m.x - s.x, m.y - s.y) < 12.0f;
        const OBJTYPEINFO& ti = InfoOfType(TypeOfLo(plo));
        // its type's letter in its colour (the selected ones ringed)
        const float r = fSel ? 8.0f : fSelAny ? 7.0f : 5.5f;
        dl->AddCircleFilled(s, r, st::u32(ti.rgb, fSelAny || fHover ? 0.95f : 0.6f));
        if (r >= 7.0f || fHover)
        {
            const char icon[2] = {ti.icon, 0};
            const ImVec2 ts = ImGui::CalcTextSize(icon);
            dl->AddText(ImVec2(s.x - ts.x * 0.5f, s.y - ts.y * 0.5f), st::u32(0x0A0F1C), icon);
        }
        if (fSelAny) dl->AddCircle(s, r + 3.0f, st::u32(fSel ? st::cta : st::text), 0, 2.0f);
        if (fSel || fHover)
        {
            const std::string label = NameOfObject(plo);
            dl->AddText(ImVec2(s.x + r + 5, s.y - 9), st::u32(st::text), label.c_str());
        }
    }
}

void CEditor::DrawGizmo()
{
    // the target: a path's points (curve mode: the selected point, or the whole curve about its pivot), else the
    // object's placement
    PLACE pl;
    PATHDATA pd;
    if (m_tool == 0 && m_axisDrag < 0) return;   // (the select tool: no gizmo)
    const bool fCurve = m_fCurveMode && m_plSel && FPathLive(m_plSel, pd) && !pd.apos.empty();
    // several selected: one gizmo at their middle (each moved through its own placement)
    std::vector<PLACE> aplSel;
    if (!fCurve && m_asel.size() > 1) aplSel = PlacesOfSelection();
    const bool fMulti = aplSel.size() > 1;
    if (!fCurve && !fMulti && !FPlace(m_plSel, pl)) return;
    if (fCurve && m_ipoint >= int(pd.apos.size())) m_ipoint = -1;
    const glm::mat4 toWorld = fCurve ? MatPathSpace(m_plSel) : glm::mat4(1.0f);
    glm::mat4 w;
    if (fCurve)
    {
        const glm::vec3 pivot = m_ipoint >= 0 ? pd.apos[std::size_t(m_ipoint)] : PathPivot(m_plSel, pd);
        w = toWorld;
        for (int i = 0; i < 3; ++i) w[i] = glm::vec4(glm::normalize(glm::vec3(toWorld[i]) + glm::vec3(1e-9f)), 0.0f);
        w[3] = toWorld * glm::vec4(pivot, 1.0f);
    }
    else if (fMulti)
    {
        glm::vec3 mid(0.0f);
        for (const PLACE& q : aplSel) mid += PosWorld(q.ploLive);
        mid /= float(aplSel.size());
        PLACE plPrimary;
        w = m_fLocalAxes && FPlace(m_plSel, plPrimary) ? MatWorld(plPrimary.ploLive) : glm::mat4(1.0f);
        for (int i = 0; i < 3; ++i) w[i] = glm::vec4(glm::normalize(glm::vec3(w[i]) + glm::vec3(1e-9f)), 0.0f);
        w[3] = glm::vec4(mid, 1.0f);
    }
    else w = MatWorld(pl.ploLive);
    if (m_axisDrag >= 0) w = m_matDragCur;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();   // (under the panels)
    const glm::vec3 p = glm::vec3(w[3]);
    ImVec2 s0;
    if (!FProject(p, s0)) return;
    // the handles' length: about 110 pixels on screen
    const float dist = glm::length(p - m_cam.pos);
    const float len = std::max(1.0f, dist * 0.18f);
    glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (m_fLocalAxes || m_gizmo == 2)
        for (int i = 0; i < 3; ++i) axes[i] = glm::normalize(glm::vec3(w[i]) + glm::vec3(1e-9f));
    const uint32_t acol[3] = {0xF0506E, 0x3CC66B, 0x3AA3F0};
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 as[3];
    bool af[3];
    for (int i = 0; i < 3; ++i) af[i] = FProject(p + axes[i] * len, as[i]);
    int hover = -1;
    if (m_axisDrag < 0 && !io.WantCaptureMouse)
        for (int i = 0; i < 3; ++i)
        {
            if (!af[i]) continue;
            // distance from the mouse to the handle's line
            const ImVec2 a = s0, b = as[i], m = io.MousePos;
            const float lx = b.x - a.x, ly = b.y - a.y, l2 = lx * lx + ly * ly;
            if (l2 < 1) continue;
            const float t = std::clamp(((m.x - a.x) * lx + (m.y - a.y) * ly) / l2, 0.0f, 1.0f);
            if (std::hypot(a.x + t * lx - m.x, a.y + t * ly - m.y) < 7.0f && t > 0.15f) hover = i;
        }
    // (a single point only moves)
    const int gizmo = fCurve && m_ipoint >= 0 ? 0 : m_gizmo;
    for (int i = 0; i < 3; ++i)
    {
        if (!af[i]) continue;
        const bool fHot = hover == i || m_axisDrag == i;
        const ImU32 c = st::u32(fHot ? st::cta : acol[i]);
        dl->AddLine(s0, as[i], c, fHot ? 4.0f : 2.5f);
        if (gizmo == 0) dl->AddCircleFilled(as[i], 6.0f, c);
        else if (gizmo == 2) dl->AddRectFilled(ImVec2(as[i].x - 5, as[i].y - 5), ImVec2(as[i].x + 5, as[i].y + 5), c);
        else dl->AddCircle(as[i], 8.0f, c, 0, 2.5f);
    }
    dl->AddCircleFilled(s0, 4.0f, st::u32(fCurve ? st::cta : st::text));
    if (hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_axisDrag = hover;
        m_matDragStart = w;
        m_matDragCur = w;
        m_mouseDragStart = io.MousePos;
        if (fCurve) m_pdDrag = pd;
        m_adragSel.clear();
        if (fMulti)
            for (const PLACE& q : aplSel) m_adragSel.emplace_back(q, MatWorld(q.ploLive));
    }
    if (m_axisDrag >= 0)
    {
        const int i = m_axisDrag;
        // the mouse's travel along the handle's screen direction, in world units
        const ImVec2 dir(as[i].x - s0.x, as[i].y - s0.y);
        const float pxLen = std::max(1.0f, std::hypot(dir.x, dir.y));
        const float along = ((io.MousePos.x - m_mouseDragStart.x) * dir.x + (io.MousePos.y - m_mouseDragStart.y) * dir.y) / pxLen;
        glm::mat4 wNew = m_matDragStart;
        const glm::vec3 p0 = glm::vec3(m_matDragStart[3]);
        // snapping (the status bar's; Ctrl held turns the grid snap the other way while dragging)
        const bool fGrid = m_fGridSnap != io.KeyCtrl;
        auto snapGrid = [&](float v) { return fGrid && m_gridSize > 0 ? std::round(v / m_gridSize) * m_gridSize : v; };
        if (gizmo == 0)
        {
            float d = along / pxLen * len;
            // world axes: the coordinate itself on the grid; local axes: the distance moved
            const bool fWorldAxis = std::abs(std::abs(axes[i][i]) - 1.0f) < 1e-4f;
            if (fWorldAxis) d = snapGrid(p0[i] + d * axes[i][i]) * axes[i][i] - p0[i] * axes[i][i];
            else d = snapGrid(d);
            wNew[3] += glm::vec4(axes[i] * d, 0.0f);
        }
        else if (gizmo == 1)
        {
            const float ang = glm::radians(SnapAngle(along * 0.5f));
            wNew = glm::translate(glm::mat4(1.0f), p0) * glm::rotate(glm::mat4(1.0f), ang, axes[i]) * glm::translate(glm::mat4(1.0f), -p0) * m_matDragStart;
        }
        else
        {
            glm::vec3 sc(1.0f);
            sc[i] = SnapScale(std::max(0.01f, 1.0f + along / pxLen));
            if (io.KeyShift) sc = glm::vec3(sc[i]);
            if (fMulti || !m_adragSel.empty())
            {
                // several: scaled about the middle along the gizmo's axes (their places spread, each one scaled too)
                const glm::mat4 R(glm::vec4(glm::vec3(m_matDragStart[0]), 0), glm::vec4(glm::vec3(m_matDragStart[1]), 0), glm::vec4(glm::vec3(m_matDragStart[2]), 0), glm::vec4(0, 0, 0, 1));
                wNew = glm::translate(glm::mat4(1.0f), p0) * R * glm::scale(glm::mat4(1.0f), sc) * glm::transpose(R) * glm::translate(glm::mat4(1.0f), -p0) * m_matDragStart;
            }
            else wNew = m_matDragStart * glm::scale(glm::mat4(1.0f), sc);
        }
        m_matDragCur = wNew;
        const bool fRelease = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        static const char* s_aname[] = {"Move", "Rotate", "Scale"};
        if (fCurve)
        {
            // the change in world space, applied to the points as they were when the drag began
            const glm::mat4 D = wNew * glm::inverse(m_matDragStart);
            const glm::mat4 toPath = glm::inverse(toWorld), M = toPath * D * toWorld;
            PATHDATA pn = m_pdDrag;
            if (m_ipoint >= 0) pn.apos[std::size_t(m_ipoint)] = glm::vec3(M * glm::vec4(pn.apos[std::size_t(m_ipoint)], 1.0f));
            else
            {
                for (glm::vec3& q : pn.apos) q = glm::vec3(M * glm::vec4(q, 1.0f));
                for (glm::vec3& v : pn.avIn) v = glm::mat3(M) * v;
                for (glm::vec3& v : pn.avOut) v = glm::mat3(M) * v;
            }
            PathLive(m_plSel, pn);
            if (fRelease)
            {
                const std::string what = m_ipoint >= 0 ? "point " + std::to_string(m_ipoint) + " of path " : std::string("path ");
                FPathWrite(m_plSel, pn, std::string(s_aname[gizmo]) + " " + what + std::to_string(m_plSel->oid));
                if (m_fSnapCollision) FPathSnap(m_plSel, m_ipoint, "Snap " + what + std::to_string(m_plSel->oid));
            }
        }
        else if (!m_adragSel.empty())
        {
            const glm::mat4 D = wNew * glm::inverse(m_matDragStart);
            for (const auto& [q, mStart] : m_adragSel) SetPlaceWorld(q, D * mStart);
            if (fRelease)
            {
                // one undo step for all of them
                m_proj.begin(std::string(s_aname[gizmo]) + " " + std::to_string(m_adragSel.size()) + " " + T("objects"));
                for (const auto& [q, mStart] : m_adragSel) FRecordPlace(q, "");
                m_proj.end();
                m_adragSel.clear();
            }
        }
        else
        {
            SetPlaceWorld(pl, wNew);
            if (fRelease) FRecordPlace(pl, std::string(s_aname[gizmo]) + " " + SzClass(pl.ploLive->Cid()) + " " + std::to_string(pl.fProxy ? pl.oidProxy : pl.ploLive->oid));
        }
        if (fRelease) m_axisDrag = -1;
    }
}

// ---- panels

void CEditor::DrawMenu()
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu(T("Project")))
    {
        if (ImGui::MenuItem(T("Save"), "Ctrl+S")) FSave();
        if (ImGui::MenuItem("Install into " EDGAME_NAME "'s mods folder"))
        {
            std::string error;
            if (FSave() && m_proj.install(cane::userDirectory() / "Mods", error)) Status("installed as Mods/Packages/" + m_proj.manifest.id);
            else if (!error.empty()) Status("install failed: " + error);
        }
        if (ImGui::MenuItem(T("Export as .zip")))
        {
            std::string error;
            const auto zip = m_proj.dir.parent_path() / (m_proj.manifest.id + ".zip");
            if (FSave() && m_proj.exportZip(zip, error)) Status("exported " + ce::utf8(zip));
            else if (!error.empty()) Status("export failed: " + error);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Import model as a new object...")))
            OpenFileDialog("Import a model as a new object", "Models{.glb,.gltf,.fbx}", m_importPath, [this](const std::string& path) {
                std::string e;
                std::snprintf(m_importPath, sizeof(m_importPath), "%s", path.c_str());
                if (!FImportModel(path, m_plSel, true, e)) Status("import: " + e);
            });
        if (ImGui::MenuItem(T("Export the selection as GLB..."), nullptr, false, m_plSel != nullptr))
            OpenFileDialog("Export as GLB", ".glb", m_exportPath, [this](const std::string& path) {
                std::string e;
                std::snprintf(m_exportPath, sizeof(m_exportPath), "%s", path.c_str());
                if (!FExportGlb(m_plSel, m_exportKind, path, e)) Status("export: " + e);
            });
        ImGui::Separator();
        if (ImGui::MenuItem(T("Reload world"))) Reload(m_plSel ? m_plSel->oid : 0);
        if (ImGui::MenuItem(T("Close"))) m_fQuit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Edit")))
    {
        if (ImGui::MenuItem(T("Undo"), "Ctrl+Z", false, m_proj.canUndo())) Undo();
        if (ImGui::MenuItem(T("Redo"), "Ctrl+Y", false, m_proj.canRedo())) Redo();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Copy"), "Ctrl+C", false, !m_asel.empty())) CopySelection();
        if (ImGui::MenuItem(T("Paste in front of the camera"), "Ctrl+V", false, !m_aclip.empty())) Paste(false);
        if (ImGui::MenuItem(T("Paste in place"), "Ctrl+Shift+V", false, !m_aclip.empty())) Paste(true);
        if (ImGui::MenuItem(T("Duplicate"), "Ctrl+D", false, m_plSel != nullptr)) DuplicateSelection();
        if (ImGui::MenuItem(T("Delete"), "Del", false, m_plSel != nullptr)) DeleteSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(TL("Select", "selectmenu").c_str()))
    {
        if (ImGui::MenuItem(T("All shown"), "Ctrl+A")) SelectWhere([this](LO* p) { return FSelectable(p) && glm::length(PosWorld(p)) > 0.01f; }, false);
        if (ImGui::MenuItem(T("None"), "Esc")) ClearSelection();
        if (ImGui::MenuItem(T("Same type as the selection"), nullptr, false, m_plSel != nullptr))
        {
            const OBJTYPE ot = TypeOfLo(m_plSel);
            SelectWhere([this, ot](LO* p) { return TypeOfLo(p) == ot && FSelectable(p); }, false);
        }
        if (ImGui::MenuItem(T("Same class as the selection"), nullptr, false, m_plSel != nullptr))
        {
            const int cid = m_plSel->Cid();
            SelectWhere([this, cid](LO* p) { return p->Cid() == cid && FSelectable(p); }, false);
        }
        if (ImGui::MenuItem(T("Same group as the selection"), nullptr, false, m_plSel != nullptr))
        {
            const int igrp = m_plSel->igrp;
            SelectWhere([this, igrp](LO* p) { return p->igrp == igrp && FSelectable(p); }, false);
        }
        if (ImGui::BeginMenu(T("By type")))
        {
            for (int ot = 0; ot < OT_Max; ++ot)
                if (ImGui::MenuItem(T(InfoOfType(OBJTYPE(ot)).plural)))
                    SelectWhere([this, ot](LO* p) { return TypeOfLo(p) == ot && FSelectable(p); }, false);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("View")))
    {
        if (ImGui::MenuItem(T("Hide the selection"), "H", false, !m_asel.empty())) HideSelection(true);
        if (ImGui::MenuItem(T("Isolate the selection"), "I", false, !m_asel.empty())) IsolateSelection();
        if (ImGui::MenuItem(T("Show all"), "Alt+H")) ShowAll();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Bottom panel (missions, create, scripts...)"), nullptr, &m_fDockOpen)) SaveViewState();
#if EDGAME_SLY2
        TimelineMenuItem();
#endif
        ImGui::MenuItem(T("Grid snap"), nullptr, &m_fGridSnap);
        ImGui::MenuItem(T("Angle snap"), nullptr, &m_fAngleSnap);
        ImGui::MenuItem(T("Scale snap"), nullptr, &m_fScaleSnap);
        ImGui::Separator();
        ImGui::MenuItem(T("Markers (objects without a model)"), nullptr, &m_fShowMarkers);
        ImGui::MenuItem(T("Curves: the selected one"), nullptr, &m_fShowPaths);
        ImGui::MenuItem(T("Curves: all near the camera"), nullptr, &m_fShowAllPaths);
        ImGui::MenuItem(T("Cel borders"), nullptr, &m_opts.fCelBorders);
        ImGui::MenuItem(T("Shadows"), nullptr, &m_opts.fShadows);
        ImGui::MenuItem(T("Local axes"), nullptr, &m_fLocalAxes);
        if (ImGui::MenuItem(T("Frame selected"), "F")) FrameSelected();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(T("Help")))
    {
        ImGui::MenuItem(T("Getting started"), "F1", &m_fShowHelp);
        ImGui::MenuItem(T("Keyboard shortcuts"), nullptr, &m_fShowKeys);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const char* amode[] = {"Move (W)", "Rotate (E)", "Scale (R)"};
    static const char* const s_atip[] = {"Move the selection along the gizmo's arrows", "Turn it about the gizmo's rings", "Scale it along an axis (or all at once from the middle)"};
    for (int i = 0; i < 3; ++i)
    {
        if (ImGui::RadioButton(amode[i], m_tool != 0 && m_gizmo == i)) { m_gizmo = i; m_tool = 1; }
        EdTip(s_atip[i]);
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, st::vec(st::cta));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st::vec(st::ctaHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, st::vec(st::ctaActive));
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::ctaText));
    if (ImGui::SmallButton("  Play test  ")) PlayTest();
    EdTip("Starts the game in this world with the project loaded (saved first); the editor stays open.");
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton(" v ") || m_fOpenPlayMenu) { ImGui::OpenPopup("playmenu"); m_fOpenPlayMenu = false; }
    ImGui::PopStyleColor(4);
    EdTip(T("Play a job or one of its tasks, the selected job / task, or from the view's point"));
    DrawPlayMenu();
    ImGui::TextColored(st::vec(st::muted), "  %s  |  %s%s", m_index.entry.name.c_str(), m_proj.manifest.name.c_str(), m_proj.dirty() ? " (unsaved)" : "");
    ImGui::EndMainMenuBar();
}

void CEditor::DrawTreeNode(LO* plo, int depth)
{
    const auto itc = m_mpchildren.find(plo);
    const bool fLeaf = itc == m_mpchildren.end() || depth > 12;
    char label[200];
    const OBJTYPEINFO& ti = InfoOfType(TypeOfLo(plo));
    std::snprintf(label, sizeof(label), "%c  %s  (%s %d)##%p", ti.icon, NameOfObject(plo).c_str(), SzClass(plo->Cid()), plo->oid, static_cast<void*>(plo));
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (fLeaf) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (FSelected(plo)) fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(FHidden(plo) ? st::muted : st::text));
    // the selection's ancestors opened (a pick in the view)
    if (m_fScrollTree && m_plSel)
        for (LO* p = m_plSel->paloParent; p; p = p->paloParent)
            if (p == plo) ImGui::SetNextItemOpen(true);
    const bool fOpen = ImGui::TreeNodeEx(label, fl);
    ImGui::PopStyleColor();
    if (plo == m_plSel && m_fScrollTree) { ImGui::SetScrollHereY(0.4f); m_fScrollTree = false; }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        if (ImGui::GetIO().KeyCtrl) ToggleSelected(plo);
        else if (ImGui::GetIO().KeyShift) AddSelected(plo);
        else SelectOnly(plo);
    }
    OutlinerContextMenu(plo);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { SelectOnly(plo); FrameSelected(); }
    if (fOpen && !fLeaf)
    {
        for (LO* pc : itc->second) DrawTreeNode(pc, depth + 1);
        ImGui::TreePop();
    }
}

// the outliner's "By group" view: the world's groups, each object's hierarchy
void CEditor::DrawTree()
{
    const std::string filter = m_filter;
    if (!filter.empty())
    {
        std::string lower = filter;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        int n = 0;
        for (const auto& up : m_psw->aploAll)
        {
            LO* plo = up.get();
            std::string name = SzClass(plo->Cid());
            for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (name.find(lower) == std::string::npos && std::to_string(plo->oid) != filter) continue;
            char label[128];
            std::snprintf(label, sizeof(label), "%s  %d##f%p", SzClass(plo->Cid()), plo->oid, static_cast<void*>(plo));
            if (ImGui::Selectable(label, plo == m_plSel)) m_plSel = plo;
            if (++n >= 500) { ImGui::TextDisabled("(first 500)"); break; }
        }
    }
    else
        for (const auto& [igrp, aplo] : m_mpgroupRoots)
        {
            const std::string name = igrp >= 0 && igrp < int(m_index.groups.size()) ? m_index.groups[igrp].name : std::string("(world)");
            char label[160];
            std::snprintf(label, sizeof(label), "%s%s  (%zu)##g%d", name.c_str(), igrp >= 0 && igrp < int(m_index.groups.size()) && m_index.groups[igrp].level ? "  [level]" : "", aplo.size(), igrp);
            bool fOpenSel = false;
            if (m_fScrollTree && m_plSel)
            {
                LO* r = m_plSel;
                while (r->paloParent) r = r->paloParent;
                fOpenSel = r->igrp == igrp;
            }
            if (fOpenSel) ImGui::SetNextItemOpen(true);
            ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::text2));
            const bool fOpen = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            if (fOpen)
            {
                for (LO* plo : aplo) DrawTreeNode(plo, 0);
                ImGui::TreePop();
            }
        }
}

void CEditor::DrawInspector()
{
    // the right column's lower half (the outliner above it)
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    const float w = std::clamp(size.x * 0.25f, 330.0f, 470.0f);
    const float y = top + 6 + (size.y - top - 36) * 0.5f + 6;
    ImGui::SetNextWindowPos(ImVec2(size.x - w - 6, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, size.y - y - 36), ImGuiCond_Always);
    ImGui::Begin(TL("Properties", "inspector").c_str(), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    LO* plo = m_plSel;
    if (!plo)
    {
        ImGui::TextWrapped("%s", T("Click an object in the view or the outliner (Ctrl+click: several, drag in empty space with the select tool (Q): a box). Right mouse + WASD / Q / E flies; the wheel sets the speed; F frames the selection."));
        ImGui::End();
        return;
    }
    // several selected: what they are, and what can be done to all of them
    if (m_asel.size() > 1)
    {
        std::map<int, int> mpType;
        for (LO* p : m_asel) ++mpType[TypeOfLo(p)];
        ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::cta));
        ImGui::Text("%zu %s", m_asel.size(), T("objects selected"));
        ImGui::PopStyleColor();
        std::string types;
        for (const auto& [ot, n] : mpType) types += (types.empty() ? "" : ", ") + std::to_string(n) + " " + T(InfoOfType(OBJTYPE(ot)).plural);
        ImGui::TextWrapped("%s", types.c_str());
        static float s_off[3] = {0, 0, 0};
        ImGui::SetNextItemWidth(-90);
        ImGui::InputFloat3(T("Offset"), s_off, "%.1f");
        ImGui::SameLine();
        if (ImGui::Button(T("Move all")))
        {
            const std::vector<PLACE> apl = PlacesOfSelection();
            m_proj.begin(std::string(T("Move")) + " " + std::to_string(apl.size()) + " " + T("objects"));
            for (const PLACE& q : apl)
            {
                glm::mat4 wq = MatWorld(q.ploLive);
                wq[3] += glm::vec4(s_off[0], s_off[1], s_off[2], 0.0f);
                SetPlaceWorld(q, wq);
                FRecordPlace(q, "");
            }
            m_proj.end();
        }
        if (ImGui::Button(T("Duplicate"))) DuplicateSelection();
        ImGui::SameLine();
        if (ImGui::Button(T("Hide"))) HideSelection(true);
        ImGui::SameLine();
        if (ImGui::Button(T("Isolate"))) IsolateSelection();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, st::vec(st::danger, 0.75f));
        if (ImGui::Button(T("Delete"))) DeleteSelection();
        ImGui::PopStyleColor();
        ImGui::SeparatorText(T("Primary (the last clicked)"));
    }
    const auto ito = m_mpobj.find(plo);
    const EDOBJ* po = ito == m_mpobj.end() ? nullptr : &ito->second;
    {
        // the header: its type's icon, its name, its type, class and oid
        const OBJTYPEINFO& ti = InfoOfType(TypeOfLo(plo));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float hIcon = ImGui::GetTextLineHeight() * 2.0f + 4;
        dl->AddRectFilled(p0, ImVec2(p0.x + hIcon, p0.y + hIcon), st::u32(ti.rgb, 0.9f), 6.0f);
        const char icon[2] = {ti.icon, 0};
        const ImVec2 ts = ImGui::CalcTextSize(icon);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f, ImVec2(p0.x + hIcon * 0.5f - ts.x * 0.8f, p0.y + hIcon * 0.5f - ts.y * 0.8f), st::u32(0x0A0F1C), icon);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + hIcon + 10, p0.y));
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::cta));
        ImGui::TextUnformatted(NameOfObject(plo).c_str());
        ImGui::PopStyleColor();
        ImGui::TextColored(st::vec(st::muted), "%s  -  %s 0x%X  -  oid %d", T(ti.name), SzClass(plo->Cid()), plo->Cid(), plo->oid);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + hIcon + 6));
    }
    // a job / task: played from there; a camera: what it shows (the preview panel)
    if (const int oidTask = TaskToPlay(plo); oidTask > 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, st::vec(st::cta));
        ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::ctaText));
        if (ImGui::Button(TypeOfLo(plo) == OT_Job ? T("Play this job") : T("Play from this task"))) PlayTestAt(oidTask, nullptr, 0.0f);
        ImGui::PopStyleColor(2);
        EdTip(T("Starts the game with the project loaded, this job started at this task (saved first)"));
    }
    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_SizingStretchProp))
    {
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(st::vec(st::muted), "%s", k);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", v.c_str());
        };
        std::string group = plo->igrp >= 0 && plo->igrp < int(m_index.groups.size()) ? m_index.groups[plo->igrp].name : std::string("-");
        row("Group", group);
        if (const GAMEAREA ga = AreaOfLo(plo); ga != GA_None) row("Area", SzArea(ga));
        row("Parent", plo->paloParent ? std::string(SzClass(plo->paloParent->Cid())) + " " + std::to_string(plo->paloParent->oid) : std::string("(none)"));
        LO* ptop = PloTopLevel(plo);
        row("Record", ptop ? (ptop == plo ? std::string("top-level in ") : std::string("inside ") + std::to_string(ptop->oid) + ", ") +
                                 m_mpobj[ptop].relRecord.substr(m_mpobj[ptop].relRecord.find("/World/") + 1)
                           : std::string("made at run time (not stored)"));
        ImGui::EndTable();
    }
    PLACE pl;
    if (FPlace(plo, pl))
    {
        if (pl.fProxy)
        {
            const std::string grp = pl.ploLive->igrp >= 0 && pl.ploLive->igrp < int(m_index.groups.size()) ? m_index.groups[pl.ploLive->igrp].name : std::string("?");
            ImGui::SeparatorText("Placement (instance)");
            ImGui::TextColored(st::vec(st::muted), "An instance of %s, placed by proxy %d", grp.c_str(), pl.oidProxy);
        }
        else ImGui::SeparatorText(pl.ploLive == plo ? "Transform (local)" : "Transform");
        const glm::mat4 m = MatPlace(pl);
        glm::vec3 pos(m[3]);
        glm::vec3 sc(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
        glm::mat3 r(glm::vec3(m[0]) / std::max(sc.x, 1e-6f), glm::vec3(m[1]) / std::max(sc.y, 1e-6f), glm::vec3(m[2]) / std::max(sc.z, 1e-6f));
        float ex, ey, ez;
        glm::extractEulerAngleZYX(glm::mat4(r), ez, ey, ex);
        glm::vec3 eul(glm::degrees(ex), glm::degrees(ey), glm::degrees(ez));
        bool fChanged = false, fDone = false;
        ImGui::SetNextItemWidth(-70);
        fChanged |= ImGui::DragFloat3("Position", glm::value_ptr(pos), 5.0f, 0, 0, "%.1f");
        fDone |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(-70);
        fChanged |= ImGui::DragFloat3("Rotation", glm::value_ptr(eul), 0.5f, 0, 0, "%.1f");
        fDone |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(-70);
        fChanged |= ImGui::DragFloat3("Scale", glm::value_ptr(sc), 0.01f, 0.01f, 100.0f, "%.3f");
        fDone |= ImGui::IsItemDeactivatedAfterEdit();
        if (fChanged)
        {
            glm::mat4 mNew = glm::eulerAngleZYX(glm::radians(eul.z), glm::radians(eul.y), glm::radians(eul.x));
            mNew = glm::translate(glm::mat4(1.0f), pos) * mNew * glm::scale(glm::mat4(1.0f), glm::max(sc, glm::vec3(0.001f)));
            SetPlace(pl, mNew);
        }
        if (fDone) FRecordPlace(pl, "Edit transform of " + std::string(SzClass(pl.ploLive->Cid())) + " " + std::to_string(pl.fProxy ? pl.oidProxy : pl.ploLive->oid));
        const glm::vec3 pw = PosWorld(plo);
        ImGui::TextColored(st::vec(st::muted), "World position  %.1f, %.1f, %.1f", pw.x, pw.y, pw.z);
    }
    else if (FHasMatrix(plo))
    {
        ImGui::SeparatorText("Transform");
        const glm::vec3 pw = PosWorld(plo);
        ImGui::TextColored(st::vec(st::muted), "World position  %.1f, %.1f, %.1f", pw.x, pw.y, pw.z);
        ImGui::TextColored(st::vec(st::danger), "Placed at run time: not stored in the world file.");
    }
    DrawBrushProperties(plo);
    DrawGuardSection(plo);
    DrawBuildSection(plo);
#if EDFEAT_CURVES
    if (po && po->ipath >= 0) DrawPathInspector(plo);
#endif
#if EDFEAT_MISSIONS
    if (plo->Cid() == EDCID(0x85))
    {
        TASKLINKS tl;
        if (FTaskLinks(plo, tl))
        {
            ImGui::SeparatorText("Task links");
            std::string pre, post;
            for (int o : tl.aoidPre) pre += (pre.empty() ? "" : ", ") + std::to_string(o);
            for (int o : tl.aoidPost) post += (post.empty() ? "" : ", ") + std::to_string(o);
            ImGui::TextColored(st::vec(st::muted), "after: %s   next: %s  (edit in Missions)", pre.empty() ? "-" : pre.c_str(), post.empty() ? "-" : post.c_str());
        }
    }
#endif
#if EDFEAT_MODELS
    DrawModelSection(plo);
#endif
    if (ImGui::CollapsingHeader(TL("When it exists", "exists").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) DrawExistence(plo, po);
#if EDFEAT_OPTIONS
    DrawLinks(plo, po);
    ImGui::SeparatorText("Options");
    DrawOptions(plo, po);
#endif
    // (what this game's editor does not edit yet: the counts, read only)
    if (!EDFEAT_CURVES || !EDFEAT_MODELS || !EDFEAT_OPTIONS)
        if (po && ((!EDFEAT_CURVES && po->ipath >= 0) || (!EDFEAT_MODELS && po->iglobset >= 0) || !EDFEAT_OPTIONS))
        {
            ImGui::SeparatorText("Recorded");
            if (!EDFEAT_OPTIONS) ImGui::TextColored(st::vec(st::muted), "%zu options, %zu option lists", po->aiopt.size(), po->aiarr.size());
            ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::muted));
            if (!EDFEAT_CURVES && po->ipath >= 0) ImGui::TextWrapped("A curve (%s: its editing comes later).", EDGAME_NAME);
            if (!EDFEAT_MODELS && po->iglobset >= 0) ImGui::TextWrapped("A model%s (%s: model and texture editing come later).", po->isoes >= 0 ? " with collision" : "", EDGAME_NAME);
            ImGui::PopStyleColor();
        }
    ImGui::Spacing();
#if EDFEAT_STRUCT
    if (ImGui::Button("Duplicate")) Duplicate();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, st::vec(st::danger, 0.75f));
    if (ImGui::Button("Delete")) Delete();
    ImGui::PopStyleColor();
    ImGui::SameLine();
#endif
    if (ImGui::Button("Frame")) FrameSelected();
    if (EDFEAT_STRUCT && (pl.fProxy || FPlace(plo, pl)))
        if (pl.fProxy)
        {
            ImGui::SameLine();
            if (ImGui::Button("Make unique"))
            {
                std::string error;
                if (!FMakeUnique(plo, error)) Status("make unique: " + error);
            }
        }
    ImGui::End();
}

void CEditor::DrawWorldPanel()
{
    // the bottom dock between the tool strip and the right column (taller while scripts or dialogue are shown)
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    const float wRight = std::clamp(size.x * 0.25f, 330.0f, 470.0f);
    const float x = 64, w = std::min(1600.0f, std::max(300.0f, size.x - wRight - 12 - x - 6));
    if (!m_fDockOpen)
    {
        ImGui::SetNextWindowPos(ImVec2(x, size.y - 36 - 34), ImGuiCond_Always);
        ImGui::Begin("##dockclosed", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        if (ImGui::SmallButton(T("Show the bottom panel"))) { m_fDockOpen = true; SaveViewState(); }
        ImGui::End();
        return;
    }
    const float h = std::max(220.0f, (size.y - top) * ((m_tabCur == 3 || m_tabCur == 5 || m_tabCur == 10) ? 0.5f : 0.36f));
    const float y = size.y - 36 - h;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    bool fOpen = true;
    ImGui::Begin(TL("World", "world").c_str(), &fOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (!fOpen) { m_fDockOpen = false; SaveViewState(); }
    if (ImGui::BeginTabBar("worldtabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        auto tab = [&](const char* name, int i) {
            const bool f = ImGui::BeginTabItem(name, nullptr, m_tab == i ? ImGuiTabItemFlags_SetSelected : 0);
            if (f) m_tabCur = i;
            return f;
        };
#if EDFEAT_MISSIONS
        if (tab(T("Missions"), 1)) { DrawMissions(); ImGui::EndTabItem(); }
        if (tab(T("States"), 10)) { DrawStates(); ImGui::EndTabItem(); }
        if (tab(T("Level"), 11)) { DrawLevelTab(); ImGui::EndTabItem(); }
#endif
        if (tab(T("Assets"), 9)) { DrawAssets(); ImGui::EndTabItem(); }
#if EDFEAT_STRUCT
        if (tab(T("Create"), 6)) { DrawBrushTools(); DrawBuildPalette(); ImGui::SeparatorText(T("Copies of the world's objects")); DrawCreate(); if (m_dockScroll >= 0) { ImGui::SetScrollY(m_dockScroll); m_dockScroll = -1; } ImGui::EndTabItem(); }
#endif
#if EDFEAT_OPTIONS
        if (tab(T("Areas"), 7))
        {
            // the gameplay areas: the objects of each, by class
            for (int ga = 1; ga < GA_Max; ++ga)
            {
                std::vector<LO*> a;
                for (const auto& up : m_psw->aploAll)
                    if (AreaOfLo(up.get()) == ga) a.push_back(up.get());
                char label[96];
                std::snprintf(label, sizeof(label), "%s  (%zu)##ga%d", SzArea(GAMEAREA(ga)), a.size(), ga);
                if (m_plSel && AreaOfLo(m_plSel) == ga) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);   // (the selection's area)
                if (!ImGui::TreeNode(label)) continue;
                int n = 0;
                for (LO* plo : a)
                {
                    char l2[96];
                    std::snprintf(l2, sizeof(l2), "%s##a%p", StrLo(plo).c_str(), static_cast<void*>(plo));
                    if (ImGui::Selectable(l2, plo == m_plSel)) { Select(plo); FrameSelected(); }
                    if (++n >= 300) { ImGui::TextDisabled("(first 300)"); break; }
                }
                ImGui::TreePop();
            }
            ImGui::EndTabItem();
        }
#endif
#if EDFEAT_MODELS
        if (tab(T("Models"), 8)) { DrawModels(); ImGui::EndTabItem(); }
#endif
#if EDFEAT_SCRIPTS
        if (tab(T("Scripts"), 3)) { DrawScripts(); ImGui::EndTabItem(); }
#endif
#if EDFEAT_DIALOGUE
        if (tab(T("Dialogue"), 5)) { DrawDialogue(); ImGui::EndTabItem(); }
#endif
        if (tab(T("Text"), 2))
        {
            ImGui::TextColored(st::vec(st::muted), "%zu strings (%s)", m_atext.size(), m_textLanguage.c_str());
            ImGui::BeginChild("text");
            for (const ce::TextString& t : m_atext) ImGui::TextWrapped("%5d  %s", t.id, t.text.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(T("History"), 4))
        {
            const auto& ah = m_proj.history();
            for (std::size_t i = 0; i < ah.size(); ++i) ImGui::BulletText("%s%s", ah[i].label.c_str(), ah[i].structural ? "  *" : "");
            const auto& ar = m_proj.redoHistory();
            for (auto it = ar.rbegin(); it != ar.rend(); ++it) ImGui::TextDisabled("   (undone) %s", it->label.c_str());
            if (ah.empty() && ar.empty()) ImGui::TextDisabled("(no edits yet)");
            ImGui::TextColored(st::vec(st::muted), "Kept with the project when it is saved.");
            ImGui::Separator();
            if (ImGui::SmallButton("Check references now"))
            {
                m_aproblem = Validate();
                Status(std::to_string(m_aproblem.size()) + " problems found");
            }
            for (const PROBLEM& p : m_aproblem) ImGui::TextColored(st::vec(st::danger), "%s", p.text.c_str());
            ImGui::TextColored(st::vec(st::muted), "Changed parts:");
            for (const std::string& rel : m_proj.changedParts()) ImGui::TextWrapped("%s", rel.substr(rel.find("/World/") + 1).c_str());
            ImGui::EndTabItem();
        }
        m_tab = 0;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void CEditor::Screenshot(const std::string& path)
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(g_gl.window, &w, &h);
    std::vector<uint8_t> ab(std::size_t(w) * std::size_t(h) * 4), flip(ab.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
    const std::size_t row = std::size_t(w) * 4;
    for (int y = 0; y < h; ++y) std::memcpy(flip.data() + std::size_t(y) * row, ab.data() + std::size_t(h - 1 - y) * row, row);
    for (std::size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
    if (stbi_write_png(path.c_str(), w, h, 4, flip.data(), int(row))) Status("wrote " + path);
    else Status("could not write " + path);
}

// --editor-do: one step per frame once the world shows (tools/editorshots.sh)
bool CEditor::FRunScript()
{
    if (m_iscript >= m_ascript.size()) return false;
    if (m_waitScript > 0) { --m_waitScript; return true; }
    const SCRIPTCMD c = m_ascript[m_iscript++];
    m_waitScript = 1;
    auto floats = [](const std::string& s, float* a, int n) {
        std::stringstream ss(s);
        std::string t;
        int i = 0;
        while (i < n && std::getline(ss, t, ',')) a[i++] = float(std::atof(t.c_str()));
        return i;
    };
    if (c.verb == "select")
    {
        const int oid = std::atoi(c.arg.c_str());
        LO* p = nullptr;
        for (const auto& up : m_psw->aploAll)
            if (up->oid == oid && (!p || (FHasMatrix(up.get()) && !FHasMatrix(p)))) p = up.get();
        // (a proxy's oid: the instance it places)
        if (!p)
            for (const auto& up : m_psw->aploAll)
                if (!up->paloParent && up->oidInstance == oid) { p = up.get(); break; }
        Select(p);
        if (!p) Status("select: no object " + c.arg);
        else std::printf("editor: select %s oid %d (instance of %d)\n", SzClass(p->Cid()), p->oid, p->oidInstance);
    }
    else if (c.verb == "selectgroup")
    {
        LO* p = nullptr;
        for (const auto& [igrp, aplo] : m_mpgroupRoots)
            if (igrp >= 0 && igrp < int(m_index.groups.size()) && m_index.groups[igrp].name == c.arg)
                for (LO* plo : aplo)
                {
                    PLACE pl;
                    if (!p && FPlace(plo, pl) && glm::length(PosWorld(plo)) > 1.0f) p = plo;
                }
        Select(p);
        if (!p) Status("selectgroup: no placed object in " + c.arg);
    }
    else if (c.verb == "dumpgroup")
    {
        for (const auto& [igrp, aplo] : m_mpgroupRoots)
            if (igrp >= 0 && igrp < int(m_index.groups.size()) && m_index.groups[igrp].name == c.arg)
                for (LO* plo : aplo)
                {
                    const auto it = m_mpobj.find(plo);
                    const glm::vec3 p = FHasMatrix(plo) ? PosWorld(plo) : glm::vec3(0.0f);
                    std::printf("editor: group %s: %s oid %d inst %d rec %d imat %d top %d at %.0f %.0f %.0f\n", c.arg.c_str(), SzClass(plo->Cid()), plo->oid, plo->oidInstance,
                                it == m_mpobj.end() ? -1 : it->second.iobjrec, it == m_mpobj.end() ? -1 : it->second.imat, PloTopLevel(plo) != nullptr, p.x, p.y, p.z);
                }
        int nrec[3] = {};
        for (const EDITOBJREC& r : m_rec.aobj) ++nrec[r.depth == 0 ? 0 : r.plo ? 1 : 2];
        std::printf("editor: records: %d top-level, %d inside, %d without object; %zu fields\n", nrec[0], nrec[1], nrec[2], m_rec.afield.size());
    }
    else if (c.verb == "classes")   // classes: every class in the world (count, base chain, model / matrix counts, a few oids, options)
    {
        std::map<int, std::vector<LO*>> mp;
        for (const auto& up : m_psw->aploAll) mp[up->Cid()].push_back(up.get());
        for (const auto& [cid, a] : mp)
        {
            std::string chain;
            for (int b = g_acls[cid].cidBase; b > 0 && b < CID_Max; b = g_acls[b].cidBase) chain += std::string(" < ") + SzClass(b);
            int cModel = 0, cMat = 0;
            std::map<int, int> mpopt;
            for (LO* plo : a)
            {
                cModel += FIsLoDerivedFrom(plo, EDCID(0x30)) && static_cast<ALO*>(plo)->pglobset != nullptr;
                cMat += FHasMatrix(plo);
                if (const auto it = m_mpobj.find(plo); it != m_mpobj.end())
                    for (int i : it->second.aiopt) ++mpopt[m_rec.afield[std::size_t(i)].eopid];
            }
            std::string ex, opts;
            for (std::size_t i = 0; i < a.size() && i < 4; ++i) ex += " " + std::to_string(a[i]->oid);
            for (const auto& [e, n] : mpopt) opts += " " + std::to_string(e) + "x" + std::to_string(n);
            std::printf("editor: class 0x%02X %s%s: %zu (model %d, matrix %d) oids%s opts%s\n", cid, SzClass(cid), chain.c_str(), a.size(), cModel, cMat, ex.c_str(), opts.c_str());
        }
    }
    // ---- editor 2: types, visibility, the selection of several, snapping, copy / paste
    else if (c.verb == "types")   // types: the objects per type (shown / all) and a few names
    {
        std::map<int, std::vector<LO*>> mp;
        for (const auto& up : m_psw->aploAll) mp[TypeOfLo(up.get())].push_back(up.get());
        for (const auto& [ot, a] : mp)
        {
            int cShown = 0;
            std::string ex;
            std::set<std::string> setNames;
            for (LO* p : a)
            {
                cShown += !FHidden(p);
                if (setNames.size() < 4 && setNames.insert(NameOfObject(p)).second) ex += (ex.empty() ? "" : ", ") + NameOfObject(p);
            }
            std::printf("editor: type %s (%s): %zu, %d shown: %s\n", InfoOfType(OBJTYPE(ot)).name, SzVisGroup(InfoOfType(OBJTYPE(ot)).vg), a.size(), cShown, ex.c_str());
        }
    }
    else if (c.verb == "selecttype" || c.verb == "addtype")   // selecttype:TYPE (its English name or plural)
    {
        int ot = -1;
        for (int i = 0; i < OT_Max; ++i)
            if (c.arg == InfoOfType(OBJTYPE(i)).name || c.arg == InfoOfType(OBJTYPE(i)).plural) ot = i;
        if (ot < 0) Status("selecttype: no type " + c.arg);
        else SelectWhere([this, ot](LO* p) { return TypeOfLo(p) == ot && FSelectable(p); }, c.verb == "addtype");
    }
    else if (c.verb == "selectadd" || c.verb == "selecttoggle")   // selectadd:OID / selecttoggle:OID (an instance's proxy oid too)
    {
        const int oid = std::atoi(c.arg.c_str());
        LO* p = nullptr;
        for (const auto& up : m_psw->aploAll)
            if (up->oid == oid && (!p || (FHasMatrix(up.get()) && !FHasMatrix(p)))) p = up.get();
        if (!p)
            for (const auto& up : m_psw->aploAll)
                if (!up->paloParent && up->oidInstance == oid) { p = up.get(); break; }
        if (c.verb == "selectadd") AddSelected(p);
        else ToggleSelected(p);
    }
    else if (c.verb == "boxselect")   // boxselect:x0,y0,x1,y1 (window pixels)
    {
        float a[4] = {};
        if (floats(c.arg, a, 4) == 4) BoxSelect(ImVec2(a[0], a[1]), ImVec2(a[2], a[3]), false);
    }
    else if (c.verb == "selprint")   // the selection: name, type, class, oid, position
    {
        std::printf("editor: selection %zu\n", m_asel.size());
        for (LO* p : m_asel)
        {
            const glm::vec3 q = FHasMatrix(p) ? PosWorld(p) : glm::vec3(0.0f);
            std::printf("editor: sel %s | %s | %s %d | %.2f %.2f %.2f\n", NameOfObject(p).c_str(), InfoOfType(TypeOfLo(p)).name, SzClass(p->Cid()), p->oid, q.x, q.y, q.z);
        }
    }
    else if (c.verb == "hide") HideSelection(true);
    else if (c.verb == "isolate") IsolateSelection();
    else if (c.verb == "showall") ShowAll();
    else if (c.verb == "hidetype" || c.verb == "hidegroup")   // hidetype:TYPE / hidegroup:VISGROUP (English names)
    {
        for (int i = 0; i < OT_Max && c.verb == "hidetype"; ++i)
            if (c.arg == InfoOfType(OBJTYPE(i)).name || c.arg == InfoOfType(OBJTYPE(i)).plural) m_afTypeHidden[i] = true;
        for (int i = 0; i < VG_Max && c.verb == "hidegroup"; ++i)
            if (c.arg == SzVisGroup(VISGROUP(i))) m_afVgHidden[i] = true;
        InvalidateVis();
        SaveViewState();
    }
    else if (c.verb == "copy") CopySelection();
    else if (c.verb == "paste") Paste(false);
    else if (c.verb == "pasteinplace") Paste(true);
    else if (c.verb == "dupsel") DuplicateSelection();
    else if (c.verb == "delsel") DeleteSelection();
    else if (c.verb == "grid") { m_gridSize = float(std::atof(c.arg.c_str())); m_fGridSnap = m_gridSize > 0; }
    else if (c.verb == "snap")   // snap:GRID,ANGLE,SCALE (1 on, 0 off)
    {
        float a[3] = {};
        floats(c.arg, a, 3);
        m_fGridSnap = a[0] != 0;
        m_fAngleSnap = a[1] != 0;
        m_fScaleSnap = a[2] != 0;
    }
    else if (c.verb == "movesel" || c.verb == "rotatesel" || c.verb == "scalesel")   // the selection as the gizmo moves it (with the snapping)
    {
        const std::vector<PLACE> apl = PlacesOfSelection();
        if (apl.empty()) { Status(c.verb + ": nothing to move"); return true; }
        float a[3] = {};
        floats(c.arg, a, 3);
        glm::vec3 mid(0.0f);
        for (const PLACE& q : apl) mid += PosWorld(q.ploLive);
        mid /= float(apl.size());
        glm::mat4 D(1.0f);
        if (c.verb == "movesel")
        {
            // as a drag along each world axis in turn: the middle's coordinates onto the grid
            glm::vec3 d(a[0], a[1], a[2]);
            for (int i = 0; i < 3; ++i)
                if (d[i] != 0) d[i] = SnapMove(mid[i] + d[i]) - mid[i];
            D = glm::translate(glm::mat4(1.0f), d);
        }
        else if (c.verb == "rotatesel") D = glm::translate(glm::mat4(1.0f), mid) * glm::rotate(glm::mat4(1.0f), glm::radians(SnapAngle(a[0])), glm::vec3(0, 0, 1)) * glm::translate(glm::mat4(1.0f), -mid);
        else D = glm::translate(glm::mat4(1.0f), mid) * glm::scale(glm::mat4(1.0f), glm::vec3(SnapScale(a[0] > 0 ? a[0] : 1.0f))) * glm::translate(glm::mat4(1.0f), -mid);
        m_proj.begin(c.verb + " " + std::to_string(apl.size()) + " objects");
        for (const PLACE& q : apl)
        {
            SetPlaceWorld(q, D * MatWorld(q.ploLive));
            FRecordPlace(q, "");
        }
        m_proj.end();
    }
    else if (c.verb == "tool") m_tool = std::atoi(c.arg.c_str());
    else if (c.verb == "patrol")   // the selection's patrols in words, and the paths they walk
    {
        for (LO* pg : m_asel)
            if (LO* pl = PatrolOf(pg))
            {
                std::printf("editor: patrol of %s %d at %.0f,%.0f,%.0f: list %d\n", NameOfObject(pg).c_str(), pg->oid, PosWorld(pg).x, PosWorld(pg).y, PosWorld(pg).z, pl->oid);
                for (const std::string& sc : PatrolWords(pg)) std::printf("editor: patrol  %s\n", sc.c_str());
                for (LO* pp : PatrolPaths(pg)) std::printf("editor: patrol path %d\n", pp->oid);
            }
    }
    else if (FBuildVerb(c.verb, c.arg)) {}   // (editor_build.inc: the palette's presets, who uses what, pockets, exits)
    else if (c.verb == "build")   // build:CID (Sly 2's class, hex): a copy of a working one in front of the camera (curves laid out)
    {
        const int cid = int(std::strtol(c.arg.c_str(), nullptr, 16));
        const bool fCurve = cid == 0x0A || cid == 0x0B || cid == 0x0C || cid == 0x0D;
        std::string error;
        const int oid = BuildFromPalette(cid, fCurve, error);
        if (!oid) Status("build: " + error);
        else std::printf("editor: built %s oid %d\n", m_plSel ? NameOfObject(m_plSel).c_str() : "?", oid);
    }
    else if (c.verb == "whouses") { for (LO* p : m_asel) std::printf("editor: %s used by: %s\n", NameOfObject(p).c_str(), StrWhoUses(p).c_str()); }
    else if (c.verb == "usedby")   // usedby:CLASS (hex 34 everyone, 35 Sly, 37 Bentley, 36 Murray): the selected use's option 1074
    {
        const int32_t v = int32_t(std::strtol(c.arg.c_str(), nullptr, 16));
        if (m_plSel)
        {
            if (const int ifield = IfieldOption(m_plSel, EDEOPID(1074)); ifield >= 0) FSetOption(ifield, &v, 4, "Who uses");
            else { std::vector<uint8_t> ab(4); std::memcpy(ab.data(), &v, 4); FAddOption(m_plSel, EDEOPID(1074), ab, "Who uses"); }
        }
    }
    else if (c.verb == "allpatrols") m_fAllPatrols = c.arg != "0";
    else if (c.verb == "selectgrouproot")   // selectgrouproot:NAME: the group's first root (placed or not)
    {
        LO* p = nullptr;
        for (const auto& [igrp, aplo] : m_mpgroupRoots)
            if (igrp >= 0 && igrp < int(m_index.groups.size()) && m_index.groups[std::size_t(igrp)].name == c.arg)
                for (LO* plo : aplo)
                    if (!p || (FIsLoDerivedFrom(plo, EDCID(0x30)) && !FIsLoDerivedFrom(p, EDCID(0x30)))) p = plo;
        SelectOnly(p);
        if (!p) Status("selectgrouproot: no group " + c.arg);
    }
    else if (c.verb == "donefile") ce::writeFile(ce::pathOf(c.arg), std::vector<uint8_t>{'1'});
    else if (c.verb == "thumbs") WriteThumbs(c.arg);   // thumbs:DIR: every asset's thumbnail (+ assets.json, done)
    else if (c.verb == "assets")   // the asset browser's list (name, types, instances)
    {
        BuildAssets();
        for (const ASSET& a : m_aasset)
        {
            std::string t;
            for (OBJTYPE ot : a.atype) t += std::string(" ") + InfoOfType(ot).name;
            std::printf("editor: asset %s (%s) used %zu types%s\n", a.name.c_str(), a.group.c_str(), a.aoidProxy.size(), t.c_str());
        }
    }
    else if (c.verb == "placeasset")   // placeasset:GROUP
    {
        if (!m_fAssetsBuilt) BuildAssets();
        for (const ASSET& a : m_aasset)
            if (a.group == c.arg) { PlaceAsset(a); break; }
    }
    else if (c.verb == "otherlevel") LoadOtherLevel(std::atoi(c.arg.c_str()));   // otherlevel:RECORD
    else if (c.verb == "othermake")
    {
        std::error_code ec;
        cane::fs::create_directories(ce::pathOf(AssetsDirOf(m_otherRecord)), ec);
        cane::fs::remove(ce::pathOf(AssetsDirOf(m_otherRecord)) / "done", ec);
        StartOtherLevelEditor(m_otherRecord, "thumbs:" + AssetsDirOf(m_otherRecord) + ";quit");
        m_fOtherPending = true;
    }
    else if (c.verb == "waitother") { if (m_fOtherPending) { m_iscript--; m_waitScript = 10; m_pollFrame = 29; } else std::printf("editor: other level: %zu assets\n", m_aother.size()); }
#if EDFEAT_MODELS
    else if (c.verb == "importother") ImportFromOtherLevel(c.arg);   // importother:GROUP (of otherlevel's level)
    else if (c.verb == "waitimport") { if (!m_importPending.empty()) { m_iscript--; m_waitScript = 10; m_pollFrame = 29; } }   // (until the pending import is done)
#endif
    else if (c.verb == "assettab") { m_tab = 9; m_assetSrc = std::atoi(c.arg.c_str()); }
#if EDFEAT_MODELS
    else if (c.verb == "brush")   // brush:SHAPE:W,D,H[:N[:TEXOID]]: a brush made in front of the camera (SHAPE: block ramp stairs cylinder floor)
    {
        std::stringstream ss(c.arg);
        std::string shape, size, n, tex;
        std::getline(ss, shape, ':');
        std::getline(ss, size, ':');
        std::getline(ss, n, ':');
        std::getline(ss, tex, ':');
        BRUSH b;
        static const char* const s_a[] = {"block", "ramp", "stairs", "cylinder", "floor", "tunnel"};
        for (int i = 0; i < 6; ++i)
            if (shape == s_a[i]) b.shape = i;
        float a[3] = {b.size.x, b.size.y, b.size.z};
        floats(size, a, 3);
        b.size = glm::vec3(a[0], a[1], a[2]);
        if (!n.empty()) b.steps = b.segments = std::atoi(n.c_str());
        if (!tex.empty()) b.texFrom = std::atoi(tex.c_str());
        std::string error;
        const int oid = CreateBrush(b, nullptr, error);
        if (!oid) Status("brush: " + error);
        else std::printf("editor: brush %s oid %d\n", shape.c_str(), oid);
    }
    else if (c.verb == "brushset")   // brushset:W,D,H: the selected brush's size, made again
    {
        BRUSH* pb = PbrushOf(m_plSel);
        float a[3] = {};
        std::string error;
        if (!pb || floats(c.arg, a, 3) != 3) Status("brushset: not a brush");
        else { pb->size = glm::vec3(a[0], a[1], a[2]); if (!FRebuildBrush(*pb, error)) Status("brushset: " + error); }
    }
#endif
    else if (c.verb == "brushes") { for (const BRUSH& b : m_abrush) std::printf("editor: brush oid %d shape %d size %.0f %.0f %.0f\n", b.oid, b.shape, b.size.x, b.size.y, b.size.z); }
    else if (c.verb == "playdry") m_fPlayDry = c.arg != "0";
    else if (c.verb == "playtask") PlayTestAt(std::atoi(c.arg.c_str()), nullptr, 0.0f);   // playtask:OID (a job's oid: its first task)
    else if (c.verb == "playjob") { if (LO* pj = PloOid(std::atoi(c.arg.c_str()), EDCID(0x84))) PlayTestAt(TaskToPlay(pj), nullptr, 0.0f); }
    else if (c.verb == "playhere") PlayFromView();
    else if (c.verb == "playmenu") m_fOpenPlayMenu = true;
    else if (c.verb == "lookthrough") { m_fLookThrough = c.arg != "0"; if (m_fLookThrough) m_camBeforeLook = m_cam; else m_cam = m_camBeforeLook; }
    else if (c.verb == "camview")   // the selected camera's view printed
    {
        CAMERA cv;
        if (FCameraView(m_plSel, cv)) std::printf("editor: camera %d at %.1f %.1f %.1f dir %.3f %.3f %.3f fov %.1f\n", m_plSel->oid, cv.pos.x, cv.pos.y, cv.pos.z, cv.mat[0].x, cv.mat[0].y, cv.mat[0].z, cv.fovY);
        else Status("camview: not a camera");
    }
    else if (c.verb == "camfromview")   // the selected camera object put at the editor's view
    {
        const glm::vec3 d = m_cam.Dir();
        glm::vec3 r = glm::cross(d, glm::vec3(0, 0, 1));
        r = glm::length(r) < 1e-4f ? glm::vec3(1, 0, 0) : glm::normalize(r);
        const glm::mat4 W(glm::vec4(r, 0), glm::vec4(glm::cross(r, d), 0), glm::vec4(-d, 0), glm::vec4(m_cam.pos, 1));
        PLACE pl;
        if (m_plSel && FPlace(m_plSel, pl)) { SetPlaceWorld(pl, W); FRecordPlace(pl, "Camera from the view"); }
    }
    else if (c.verb == "camfov")   // camfov:DEGREES (option 1054, added when the camera has none)
    {
        const float r = glm::radians(float(std::atof(c.arg.c_str())));
        float g = 0;
        if (!m_plSel) return true;
        if (FOptionFloat(m_plSel, EDEOPID(1054), g)) FSetOption(IfieldOption(m_plSel, EDEOPID(1054)), &r, 4, "Field of view");
        else { std::vector<uint8_t> ab(4); std::memcpy(ab.data(), &r, 4); FAddOption(m_plSel, EDEOPID(1054), ab, "Field of view"); }
    }
    else if (c.verb == "outmode") m_outMode = std::atoi(c.arg.c_str());
    else if (c.verb == "outtype")
    {
        m_outType = -1;
        for (int i = 0; i < OT_Max; ++i)
            if (c.arg == InfoOfType(OBJTYPE(i)).name || c.arg == InfoOfType(OBJTYPE(i)).plural) m_outType = i;
    }
    else if (c.verb == "outfilter") std::snprintf(m_outFilter, sizeof(m_outFilter), "%s", c.arg.c_str());
    else if (c.verb == "dock") m_fDockOpen = c.arg != "0";
#if EDGAME_SLY2
    else if (c.verb == "eopids")   // eopids:A-B: the option descriptors (kind word, flags, field offset, setter, getter)
    {
        int a = 0, b = 0;
        if (std::sscanf(c.arg.c_str(), "%d-%d", &a, &b) == 2)
            for (int e = a; e <= b; ++e)
            {
                const uint32_t va = 0x2AC880u + uint32_t(e) * 0x28u;
                std::printf("editor: eopid %d: w0 %08X grf %08X w2 %08X w3 %08X w4 %08X w5 %08X ib %X set %X w8 %08X get %X\n", e, g_disc.U32Elf(va), g_disc.U32Elf(va + 4),
                            g_disc.U32Elf(va + 8), g_disc.U32Elf(va + 12), g_disc.U32Elf(va + 16), g_disc.U32Elf(va + 20), g_disc.U32Elf(va + 0x18), g_disc.U32Elf(va + 0x1C),
                            g_disc.U32Elf(va + 0x20), g_disc.U32Elf(va + 0x24));
            }
    }
#endif
    else if (c.verb == "find")   // find:CLASS: the placed objects of a class (their oid and position)
    {
        for (const auto& up : m_psw->aploAll)
            if (c.arg == SzClass(up->Cid()) && FHasMatrix(up.get()))
            {
                const glm::vec3 p = PosWorld(up.get());
                const std::string grp = up->igrp >= 0 && up->igrp < int(m_index.groups.size()) ? m_index.groups[up->igrp].name : "-";
                std::printf("editor: find %s oid %d at %.0f %.0f %.0f group %s inst %d\n", c.arg.c_str(), up->oid, p.x, p.y, p.z, grp.c_str(), up->oidInstance);
            }
    }
    else if (c.verb == "selectnear")   // selectnear:x,y,z: the movable root object nearest that point
    {
        float a[3] = {};
        floats(c.arg, a, 3);
        const glm::vec3 p0(a[0], a[1], a[2]);
        LO* p = nullptr;
        float dBest = 1e30f;
        for (const auto& up : m_psw->aploAll)
        {
            PLACE pl;
            if (up->paloParent || !FPlace(up.get(), pl)) continue;
            const float d = glm::length(PosWorld(up.get()) - p0);
            if (d < dBest) { dBest = d; p = up.get(); }
        }
        Select(p);
    }
    else if (c.verb == "near")   // near:R: the movable objects within R of the selection, nearest first
    {
        if (!m_plSel) return true;
        const glm::vec3 p0 = PosWorld(m_plSel);
        std::vector<std::pair<float, LO*>> a;
        for (const auto& up : m_psw->aploAll)
        {
            PLACE pl;
            if (up->paloParent || !FPlace(up.get(), pl)) continue;
            const float d = glm::length(PosWorld(up.get()) - p0);
            if (d < float(std::atof(c.arg.c_str()))) a.emplace_back(d, up.get());
        }
        std::sort(a.begin(), a.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        for (std::size_t i = 0; i < a.size() && i < 40; ++i)
        {
            LO* plo = a[i].second;
            const std::string grp = plo->igrp >= 0 && plo->igrp < int(m_index.groups.size()) ? m_index.groups[plo->igrp].name : "-";
            const glm::vec3 p = PosWorld(plo);
            std::printf("editor: near %.0f: %s oid %d group %s at %.0f %.0f %.0f\n", a[i].first, SzClass(plo->Cid()), plo->oid, grp.c_str(), p.x, p.y, p.z);
        }
    }
    else if (c.verb == "cam") { float a[5] = {}; if (floats(c.arg, a, 5) == 5) { m_cam.pos = {a[0], a[1], a[2]}; m_cam.yaw = a[3]; m_cam.pitch = a[4]; } }
    else if (c.verb == "frame") FrameSelected();
    else if (c.verb == "move" || c.verb == "rotate" || c.verb == "scale")
    {
        PLACE pl;
        if (!FPlace(m_plSel, pl)) { Status(c.verb + ": nothing to move"); return true; }
        float a[3] = {};
        floats(c.arg, a, 3);
        glm::mat4 w = MatWorld(pl.ploLive);
        const glm::vec3 p = glm::vec3(w[3]);
        if (c.verb == "move") w[3] += glm::vec4(a[0], a[1], a[2], 0.0f);
        else if (c.verb == "rotate") w = glm::translate(glm::mat4(1.0f), p) * glm::rotate(glm::mat4(1.0f), glm::radians(a[0]), glm::vec3(0, 0, 1)) * glm::translate(glm::mat4(1.0f), -p) * w;
        else w = w * glm::scale(glm::mat4(1.0f), glm::vec3(a[0] > 0 ? a[0] : 1.0f));
        SetPlaceWorld(pl, w);
        FRecordPlace(pl, c.verb + " " + std::to_string(pl.fProxy ? pl.oidProxy : pl.ploLive->oid));
    }
    else if (c.verb == "dup") Duplicate();
    else if (c.verb == "delete") Delete();
    else if (c.verb == "undo") Undo();
    else if (c.verb == "redo") Redo();
    else if (c.verb == "save") FSave();
    else if (c.verb == "reload") Reload(m_plSel ? m_plSel->oid : 0);
    else if (c.verb == "install")
    {
        std::string error;
        if (!m_proj.install(c.arg.empty() ? cane::userDirectory() / "Mods" : ce::pathOf(c.arg), error)) Status("install failed: " + error);
        else Status("installed " + m_proj.manifest.id);
    }
    else if (c.verb == "zip") { std::string e; if (!m_proj.exportZip(ce::pathOf(c.arg), e)) Status("zip: " + e); }
    else if (c.verb == "tab") m_tab = c.arg == "missions" ? 1 : c.arg == "text" ? 2 : c.arg == "scripts" ? 3 : c.arg == "history" ? 4 : c.arg == "dialogue" ? 5 : c.arg == "create" ? 6 : c.arg == "areas" ? 7 : c.arg == "models" ? 8 : 0;
#if EDGAME_SLY2
    else if (c.verb == "scriptnames") { m_fScriptReadable = c.arg != "0"; if (m_igroupScript >= 0) LoadScriptText(m_igroupScript); }   // scriptnames:0|1
    else if (c.verb == "scriptroundtrip")   // every group's scripts with names, back to the converter's text: the same text
    {
        int cgrp = 0, cdiff = 0;
        for (const ce::GroupInfo& g : m_index.groups)
        {
            if (g.scriptsPart.empty()) continue;
            std::vector<uint8_t> ab;
            if (!m_proj.readPart(g.scriptsPart, ab)) continue;
            ++cgrp;
            const std::string raw = ScriptTextRaw(std::string(ab.begin(), ab.end()));
            std::string err, back;
            const std::string rd = ScriptReadable(g.index, raw, &err);
            if (!err.empty() || !FScriptToRaw(rd, back, err) || back != raw)
            {
                if (++cdiff <= 5) std::printf("editor: scripts of %s do not round-trip: %s\n", g.name.c_str(), err.c_str());
            }
        }
        std::printf("editor: scripts round trip: %d groups, %d differ\n", cgrp, cdiff);
    }
    else if (c.verb == "scripttype")   // scripttype:FROM|TO: the shown text edited (the first FROM) as typing does (not applied); diagnosed
    {
        const std::size_t bar = c.arg.find('|');
        const std::string from = c.arg.substr(0, bar), to = bar == std::string::npos ? std::string() : c.arg.substr(bar + 1);
        const std::size_t at = m_scriptEdit.find(from);
        if (m_igroupScript < 0 || from.empty() || at == std::string::npos) Status("scripttype: not found");
        else
        {
            m_scriptEdit.replace(at, from.size(), to);
            m_fScriptDirty = m_scriptEdit != m_scriptText;
            RunScriptDiagnostics();
        }
    }
    else if (c.verb == "scriptsave")   // scriptsave:PATH: the shown text written to a file (tests)
    {
        if (FILE* pf = std::fopen(c.arg.c_str(), "wb")) { std::fwrite(m_scriptEdit.data(), 1, m_scriptEdit.size(), pf); std::fclose(pf); }
    }
    else if (c.verb == "scriptdiag")   // the shown text's diagnostics printed ("editor: diag line L:C: ...")
    {
        RunScriptDiagnostics();
        if (m_pscriptdiag)
            for (const SPLICEISSUE& d : m_pscriptdiag->a) std::printf("editor: diag %s\n", StrSpliceDiag(d).c_str());
        std::printf("editor: diagnostics: %s\n", ScriptDiagSummary().empty() ? "none" : ScriptDiagSummary().c_str());
    }
    else if (c.verb == "scriptfix")   // scriptfix:CODE: the first problem of that code with a quick fix, fixed (as its Fix button)
    {
        bool fDone = false;
        if (m_pscriptdiag)
            for (const SPLICEISSUE& d : m_pscriptdiag->a)
                if (d.code == c.arg && !d.fixLabel.empty())
                {
                    std::string text = m_scriptEdit;
                    if (FApplySpliceFix(d, text)) { m_scriptEdit = text; m_fScriptDirty = true; Status("scriptfix: " + d.fixLabel); fDone = true; }
                    break;
                }
        if (!fDone) Status("scriptfix: none");
        RunScriptDiagnostics();
    }
    else if (c.verb == "scriptselect")   // scriptselect:N: the problems list's row N selected (the text scrolled to it)
    {
        if (m_pscriptdiag && std::atoi(c.arg.c_str()) < int(m_pscriptdiag->a.size()))
        {
            m_pscriptdiag->iselected = std::atoi(c.arg.c_str());
            m_pscriptdiag->lineJump = m_pscriptdiag->a[std::size_t(m_pscriptdiag->iselected)].line;
        }
    }
    else if (c.verb == "scriptedit")   // scriptedit:FROM|TO: the Scripts tab's shown text edited (the last FROM), applied as Apply does
    {
        const std::size_t bar = c.arg.find('|');
        const std::string from = c.arg.substr(0, bar), to = bar == std::string::npos ? std::string() : c.arg.substr(bar + 1);
        const std::size_t at = m_scriptEdit.rfind(from);
        std::string raw, err;
        if (m_igroupScript < 0 || from.empty() || at == std::string::npos) Status("scriptedit: not found");
        else
        {
            m_scriptEdit.replace(at, from.size(), to);
            std::vector<std::string> anew;
            // (as Apply: with Names the readable text kept, its table made again)
            const bool fKeep = m_fScriptReadable && FSpliceTextReadable(m_scriptEdit);
            if (!(fKeep ? FSpliceNormalize(m_scriptEdit, raw, err, &anew) : FScriptToRaw(m_scriptEdit, raw, err, &anew)) || !FApplyScript(m_igroupScript, raw, err))
                Status("scriptedit failed: " + err);
            else
            {
                for (const std::string& n : anew) Status("scriptedit: new symbol " + n);
                Status("scripts of " + m_index.groups[std::size_t(m_igroupScript)].name + " applied");
                const int ig = m_igroupScript;
                Reload(m_plSel ? m_plSel->oid : 0);
                LoadScriptText(ig);
            }
        }
    }
    else if (c.verb == "exist")   // exist:OID: "when it exists" of the object (its root of that oid first) printed
    {
        LO* p = nullptr;
        for (const auto& up : m_psw->aploAll)
            if (up->oid == std::atoi(c.arg.c_str()) && (!p || (p->paloParent && !up->paloParent))) p = up.get();
        std::vector<std::string> a;
        if (p) ExistenceFacts(p, a);
        std::printf("editor: exist %s: %s\n", c.arg.c_str(), p ? NameOfObject(p).c_str() : "not found");
        for (const std::string& l : a) std::printf("editor:   %s\n", l.c_str());
    }
#endif
    else if (c.verb == "script") { for (const ce::GroupInfo& g : m_index.groups) if (g.name == c.arg && !g.scriptsPart.empty()) LoadScriptText(g.index); m_tab = 3; }
    else if (c.verb == "scriptreplace")   // scriptreplace:GROUP|FROM|TO: the group's scripts edited (the last FROM), checked, applied
    {
        std::stringstream ss(c.arg);
        std::string grp, from, to;
        std::getline(ss, grp, '|');
        std::getline(ss, from, '|');
        std::getline(ss, to, '|');
        int ig = -1;
        for (const ce::GroupInfo& g : m_index.groups)
            if (g.name == grp && !g.scriptsPart.empty()) ig = g.index;
        LoadScriptText(ig);
        // (the converter's text as stored: the Scripts tab may show it with names)
        std::string text, error;
        if (ig >= 0)
        {
            std::vector<uint8_t> abPart;
            m_proj.readPart(m_index.groups[std::size_t(ig)].scriptsPart, abPart);
            text = ScriptTextRaw(std::string(abPart.begin(), abPart.end()));
        }
        const auto i = text.rfind(from);
        if (ig < 0 || from.empty() || i == std::string::npos) Status("scriptreplace: not found");
        else
        {
            text.replace(i, from.size(), to);
            int line = 0;
            std::vector<PROBLEM> apb;
            if (!FCheckScript(text, error, line, &apb, ig)) Status("script check failed: " + error);
            else if (!FApplyScript(ig, text, error)) Status("script apply failed: " + error);
            else { for (const PROBLEM& p : apb) Status("script: " + p.text); Status("scripts of " + grp + " applied"); LoadScriptText(ig); Reload(m_plSel ? m_plSel->oid : 0); }
        }
    }
    else if (c.verb == "scriptcheck")   // scriptcheck:TEXT-FILE: a whole scripts text checked (prints ok / the error)
    {
        std::vector<uint8_t> ab;
        ce::readFile(ce::pathOf(c.arg), ab);
        std::string error;
        int line = 0;
        std::vector<PROBLEM> apb;
        if (FCheckScript(std::string(ab.begin(), ab.end()), error, line, &apb)) std::printf("editor: script check ok (%zu oid problems)\n", apb.size());
        else std::printf("editor: script check failed at line %d: %s\n", line, error.c_str());
        for (const PROBLEM& p : apb) std::printf("editor: script problem: %s\n", p.text.c_str());
    }
    else if (c.verb == "opt")   // opt:EOPID=v[,v...]: the selection's option set (as typed in the inspector)
    {
        const auto eq = c.arg.find('=');
        const int eopid = std::atoi(c.arg.c_str());
        const int ifield = m_plSel && eq != std::string::npos ? IfieldOption(m_plSel, eopid) : -1;
        if (ifield < 0) { Status("opt: the selection has no option " + std::to_string(eopid)); return true; }
        const OPTTYPE t = OptType(m_rec.afield[ifield].k);
        const std::string label = "Set option " + std::to_string(eopid) + " of " + SzClass(m_plSel->Cid()) + " " + std::to_string(m_plSel->oid);
        if (t.cf)
        {
            float a[4] = {};
            if (floats(c.arg.substr(eq + 1), a, t.cf) != t.cf) { Status("opt: needs " + std::to_string(t.cf) + " numbers"); return true; }
            FSetOption(ifield, a, t.cb, label);
        }
        else if (t.cb)
        {
            const long v = std::strtol(c.arg.c_str() + eq + 1, nullptr, 0);
            if (t.cb == 1) { const uint8_t b = uint8_t(v); FSetOption(ifield, &b, 1, label); }
            else if (t.cb == 2) { const int16_t x = int16_t(v); FSetOption(ifield, &x, 2, label); }
            else { const int32_t x = int32_t(v); FSetOption(ifield, &x, 4, label); }
        }
        Status("option " + StrOption(eopid) + " set: " + m_note);
    }
    else if (c.verb == "opts")   // opts: the selection's options (and list options) printed
    {
        if (!m_plSel) return true;
        for (const EDITFIELD& f : m_rec.afield)
        {
            if (f.plo != m_plSel || (f.fk != EDITFK_Option && f.fk != EDITFK_OptionArray)) continue;
            std::string rel;
            std::size_t ib = 0;
            std::vector<uint8_t> ab;
            std::string hex;
            if (f.ibEnd > f.ib && FLocate(f.ib, f.ibEnd - f.ib, rel, ib) && m_proj.readPart(rel, ab))
                for (std::size_t k = ib; k < ib + (f.ibEnd - f.ib) && k < ib + 24; ++k) { char h[4]; std::snprintf(h, sizeof(h), "%02X", ab[k]); hex += h; }
            std::printf("editor: option %s%s kind %d bytes %s\n", StrOption(f.eopid).c_str(), f.fk == EDITFK_OptionArray ? " (list)" : "", f.k, hex.c_str());
        }
    }
#if EDFEAT_OPTIONS
    else if (c.verb == "link")   // link:task:T:S | link:script:OID | link:sm:OBJECT:MACHINE:STATE: a link added to the selected use
    {
        std::stringstream ss(c.arg);
        std::string kind, v;
        std::getline(ss, kind, ':');
        int an[3] = {0, 0, -1}, n = 0;
        while (n < 3 && std::getline(ss, v, ':')) an[n++] = std::atoi(v.c_str());
        std::string error;
        const int k = kind == "task" ? 0 : kind == "script" ? 1 : kind == "sm" ? 2 : -1;
        if (k < 0 || !FAddLink(m_plSel, k, an, error)) Status("link: " + (error.empty() ? std::string("task / script / sm") : error));
    }
    else if (c.verb == "unlink")   // unlink:EOPID[:N]: the selection's N-th option of that number removed
    {
        const int eopid = std::atoi(c.arg.c_str());
        const auto colon = c.arg.find(':');
        int n = colon == std::string::npos ? 0 : std::atoi(c.arg.c_str() + colon + 1), ifield = -1;
        if (const auto it = m_mpobj.find(m_plSel); m_plSel && it != m_mpobj.end())
            for (const auto* pa : {&it->second.aiopt, &it->second.aiarr})
                for (int i : *pa)
                    if (m_rec.afield[std::size_t(i)].eopid == eopid && n-- == 0) ifield = i;
        if (ifield < 0 || !FRemoveOption(ifield, "Remove option " + std::to_string(eopid) + " of " + StrLo(m_plSel))) Status("unlink: no such option");
    }
    else if (c.verb == "areas")   // areas: the placed objects per gameplay area (counts; the first few)
    {
        for (int ga = 1; ga < GA_Max; ++ga)
        {
            std::vector<LO*> a;
            for (const auto& up : m_psw->aploAll)
                if (AreaOfLo(up.get()) == ga) a.push_back(up.get());
            std::string ex;
            for (std::size_t i = 0; i < a.size() && i < 4; ++i) ex += (i ? ", " : "") + StrLo(a[i]);
            std::printf("editor: area %s: %zu%s%s\n", SzArea(GAMEAREA(ga)), a.size(), a.empty() ? "" : ": ", ex.c_str());
        }
    }
#endif
    else if (c.verb == "newjob")   // newjob:OID: a new job made from job OID (its marker selected)
    {
        std::string error;
        if (!CreateJob(std::atoi(c.arg.c_str()), error)) Status("new job: " + error);
        else std::printf("editor: new job %d\n", m_ojobActive);
    }
    else if (c.verb == "copyjob")   // copyjob:OID: the object (or the proxy OID's instance) copied into the new job
    {
        std::string error;
        const int oid = CopyIntoJob(std::atoi(c.arg.c_str()), m_ojobActive, error);
        if (!oid) Status("copy into job: " + error);
        else std::printf("editor: copied into job %d as %d\n", m_ojobActive, oid);
    }
    else if (c.verb == "jobinfo")   // jobinfo:OID: a job's tasks, markers and links printed
    {
        LO* pjob = PloOid(std::atoi(c.arg.c_str()), EDCID(0x84));
        int a = 0, b = 0;
        if (pjob && FOptionInt(pjob, EDEOPID(1243), a) && FOptionInt(pjob, EDEOPID(1244), b)) std::printf("editor: job %d tasks %d..%d\n", pjob->oid, a, b);
        for (int oid : {a, b})
            if (LO* pt = PloOid(oid, EDCID(0x85)))
            {
                TASKLINKS tl;
                int j = 0, m = 0;
                FOptionInt(pt, EDEOPID(1264), j);
                FOptionInt(pt, EDEOPID(1279), m);
                FTaskLinks(pt, tl);
                std::printf("editor: task %d job %d marker %d pre %zu post %zu\n", oid, j, m, tl.aoidPre.size(), tl.aoidPost.size());
            }
    }
    else if (c.verb == "links")   // links:TASK:PRE,..:POST,..
    {
        std::stringstream ss(c.arg);
        std::string t, pre, post;
        std::getline(ss, t, ':');
        std::getline(ss, pre, ':');
        std::getline(ss, post, ':');
        auto list = [](const std::string& x) { std::vector<int> a; std::stringstream s2(x); std::string v; while (std::getline(s2, v, ',')) if (!v.empty()) a.push_back(std::atoi(v.c_str())); return a; };
        if (LO* pt = PloOid(std::atoi(t.c_str()), EDCID(0x85))) FSetTaskLinks(pt, list(pre), list(post), "Links of task " + t);
    }
    else if (c.verb == "text")   // text:ID=string
    {
        const auto eq = c.arg.find('=');
        std::string error;
        if (eq == std::string::npos || !FSetText(std::atoi(c.arg.c_str()), c.arg.substr(eq + 1), error)) Status("text: " + error);
    }
    else if (c.verb == "selectpath")   // selectpath:x,y,z: the PATH-family object nearest that point
    {
        float a[3] = {};
        floats(c.arg, a, 3);
        LO* p = nullptr;
        float dBest = 1e30f;
        for (const auto& [plo, o] : m_mpobj)
        {
            if (o.ipath < 0) continue;
            PATHDATA pd;
            if (!FPathRead(plo, pd) || pd.apos.empty()) continue;
            const glm::mat4 m = MatPathSpace(plo);
            for (const glm::vec3& q : pd.apos)
                if (const float d = glm::length(glm::vec3(m * glm::vec4(q, 1.0f)) - glm::vec3(a[0], a[1], a[2])); d < dBest) { dBest = d; p = const_cast<LO*>(plo); }
        }
        Select(p);
        m_fCurveMode = p != nullptr;
        m_ipoint = -1;
    }
    else if (c.verb == "pathprint")   // the selected path's points (world)
    {
        PATHDATA pd;
        if (m_plSel && FPathRead(m_plSel, pd))
        {
            const glm::mat4 m = MatPathSpace(m_plSel);
            std::printf("editor: path %s %d: %zu points%s\n", SzClass(m_plSel->Cid()), m_plSel->oid, pd.apos.size(), pd.crvk ? " cubic" : "");
            for (std::size_t i = 0; i < pd.apos.size(); ++i)
            {
                const glm::vec3 w = glm::vec3(m * glm::vec4(pd.apos[i], 1.0f));
                std::printf("editor: path point %zu at %.2f %.2f %.2f\n", i, w.x, w.y, w.z);
            }
        }
    }
    else if (c.verb == "point") m_ipoint = std::atoi(c.arg.c_str());   // point:I (-1: the whole curve)
    else if (c.verb == "pivot") m_pivot = std::atoi(c.arg.c_str());
    else if (c.verb == "pathmove" || c.verb == "pathrotate" || c.verb == "pathscale")   // the selected point / whole curve
    {
        PATHDATA pd;
        if (!m_plSel || !FPathRead(m_plSel, pd)) { Status(c.verb + ": no path selected"); return true; }
        float a[3] = {};
        floats(c.arg, a, 3);
        const glm::mat4 toWorld = MatPathSpace(m_plSel);
        const glm::vec3 pivot = glm::vec3(toWorld * glm::vec4(m_ipoint >= 0 && m_ipoint < int(pd.apos.size()) ? pd.apos[std::size_t(m_ipoint)] : PathPivot(m_plSel, pd), 1.0f));
        glm::mat4 D(1.0f);
        if (c.verb == "pathmove") D = glm::translate(glm::mat4(1.0f), glm::vec3(a[0], a[1], a[2]));
        else if (c.verb == "pathrotate") D = glm::translate(glm::mat4(1.0f), pivot) * glm::rotate(glm::mat4(1.0f), glm::radians(a[0]), glm::vec3(0, 0, 1)) * glm::translate(glm::mat4(1.0f), -pivot);
        else D = glm::translate(glm::mat4(1.0f), pivot) * glm::scale(glm::mat4(1.0f), glm::vec3(a[0])) * glm::translate(glm::mat4(1.0f), -pivot);
        if (m_ipoint >= 0 && m_ipoint < int(pd.apos.size()))
        {
            const glm::mat4 M = glm::inverse(toWorld) * D * toWorld;
            pd.apos[std::size_t(m_ipoint)] = glm::vec3(M * glm::vec4(pd.apos[std::size_t(m_ipoint)], 1.0f));
            FPathWrite(m_plSel, pd, c.verb + " point " + std::to_string(m_ipoint) + " of " + std::to_string(m_plSel->oid));
        }
        else FPathTransform(m_plSel, D, c.verb + " path " + std::to_string(m_plSel->oid));
    }
    else if (c.verb == "pathset")   // pathset:I:x,y,z (path space; invalid numbers are refused)
    {
        PATHDATA pd;
        const auto colon = c.arg.find(':');
        if (!m_plSel || !FPathRead(m_plSel, pd) || colon == std::string::npos) return true;
        const int i = std::atoi(c.arg.c_str());
        std::stringstream ss(c.arg.substr(colon + 1));
        std::string t;
        float a[3] = {};
        for (int k = 0; k < 3 && std::getline(ss, t, ','); ++k) a[k] = t == "nan" ? NAN : t == "inf" ? INFINITY : float(std::atof(t.c_str()));
        if (i >= 0 && i < int(pd.apos.size())) { pd.apos[std::size_t(i)] = glm::vec3(a[0], a[1], a[2]); FPathWrite(m_plSel, pd, "Set point " + std::to_string(i)); }
    }
    else if (c.verb == "pathsnap") FPathSnap(m_plSel, c.arg == "all" ? -1 : std::atoi(c.arg.c_str()), "Snap path");
    else if (c.verb == "create")   // create:OID: a copy of object / proxy OID placed 600 in front of the camera, in the level group
    {
        const int oid = std::atoi(c.arg.c_str());
        int iobj = -1;
        if (const auto it = m_mpoidProxy.find(oid); it != m_mpoidProxy.end()) iobj = it->second;
        else if (LO* plo = PloOid(oid)) iobj = IobjOfLo(PloTopLevel(plo));
        glm::vec3 p = m_cam.pos + m_cam.Dir() * 600.0f, hit;
        if (FSnapDown(p, hit)) p = hit;
        std::string error;
        const int igrp = CopyGroup();   // (Sly 2: the level group with scripts, so a copy's script modules come with it)
        if (iobj < 0 || !CopyRecord(iobj, igrp, &p, true, error)) Status("create: " + (error.empty() ? std::string("no record") : error));
    }
    else if (c.verb == "unique") { std::string error; if (!FMakeUnique(m_plSel, error)) Status("make unique: " + error); }
    else if (c.verb == "subtree")   // the selection's descendants: class, oid, position, option numbers
    {
        auto walk = [&](auto&& self, LO* plo, int depth) -> void {
            const glm::vec3 p = FHasMatrix(plo) ? PosWorld(plo) : glm::vec3(0.0f);
            std::string opts;
            for (const EDITFIELD& f : m_rec.afield)
                if (f.plo == plo && (f.fk == EDITFK_Option || f.fk == EDITFK_OptionArray)) opts += " " + std::to_string(f.eopid);
            std::printf("editor: subtree %*s%s %d at %.0f %.0f %.0f inst %d opts%s\n", depth * 2, "", SzClass(plo->Cid()), plo->oid, p.x, p.y, p.z, plo->oidInstance, opts.c_str());
            if (const auto it = m_mpchildren.find(plo); it != m_mpchildren.end())
                for (LO* pc : it->second) self(self, pc, depth + 1);
        };
        if (m_plSel) walk(walk, m_plSel, 0);
    }
    else if (c.verb == "validate")
    {
        m_aproblem = Validate();
        std::printf("editor: validate: %zu problems\n", m_aproblem.size());
        for (const PROBLEM& p : m_aproblem) std::printf("editor: problem: %s\n", p.text.c_str());
    }
    else if (c.verb == "gizmo") m_gizmo = std::atoi(c.arg.c_str());
    else if (c.verb == "wait") m_waitScript = std::atoi(c.arg.c_str());
    else if (c.verb == "shot") m_shotPending = c.arg;
    else if (c.verb == "print")
    {
        if (m_plSel)
        {
            const glm::vec3 p = PosWorld(m_plSel);
            std::printf("editor: selected %s oid %d at %.2f %.2f %.2f\n", SzClass(m_plSel->Cid()), m_plSel->oid, p.x, p.y, p.z);
        }
    }
    else if (c.verb == "modelinfo") PrintModelInfo(m_plSel);
    else if (c.verb == "dlglist")   // dlglist: the dialogue lines with events (id, speaker, events)
    {
        std::vector<DLGLINE> a;
        std::vector<std::string> alang;
        std::string error;
        int n = 0;
        if (FParseDialogue(a, alang, error))
            for (const DLGLINE& l : a)
                if (l.cevent > 0 && n++ < 8) std::printf("editor: line %d speaker %d events %d\n", l.id, l.oidSpeaker, l.cevent);
    }
    else if (c.verb == "dlgevent")   // dlgevent:LINE:K:delay,target,value: an event of a dialogue line
    {
        int id = 0, k = 0, dt = 0, tg = 0, v = 0;
        std::vector<DLGLINE> a;
        std::vector<std::string> alang;
        std::string error;
        bool fDone = false;
        if (std::sscanf(c.arg.c_str(), "%d:%d:%d,%d,%d", &id, &k, &dt, &tg, &v) == 5 && FParseDialogue(a, alang, error))
            for (const DLGLINE& l : a) if (l.id == id) { fDone = FSetDialogueEvent(l, k, dt, tg, v, error); break; }
        if (!fDone) Status("dlgevent: " + (error.empty() ? std::string("no such line") : error));
    }
    else if (c.verb == "listset")   // listset:EOPID:INDEX=value: a value of the selection's list option
    {
        int eopid = 0, iel = 0;
        double v = 0;
        std::string error;
        int ifield = -1;
        if (std::sscanf(c.arg.c_str(), "%d:%d=%lf", &eopid, &iel, &v) == 3 && m_plSel)
            if (const auto it = m_mpobj.find(m_plSel); it != m_mpobj.end())
                for (int i : it->second.aiarr) if (m_rec.afield[size_t(i)].eopid == eopid) ifield = i;
        if (ifield < 0 || !FSetListElement(ifield, iel, v, error)) Status("listset: " + (error.empty() ? std::string("no such list option") : error));
    }
    else if (c.verb == "help") m_fShowHelp = c.arg != "0";      // help[:0]: the getting-started panel
    else if (c.verb == "keys") m_fShowKeys = c.arg != "0";      // keys[:0]: the shortcuts list
    else if (c.verb == "import" || c.verb == "importnew")   // import:FILE (.glb / .gltf / .fbx) into the selection / as a new object
    {
        std::string error;
        if (!FImportModel(c.arg, m_plSel, c.verb == "importnew", error)) Status("import: " + error);
    }
    else if (c.verb == "export" || c.verb == "exportgroup" || c.verb == "exportchar")   // export:FILE.glb (the selection / its group / with skeleton + animations)
    {
        std::string error;
        if (!FExportGlb(m_plSel, c.verb == "export" ? 0 : c.verb == "exportgroup" ? 1 : 2, c.arg, error)) Status("export: " + error);
    }
    else if (c.verb == "exportanim")   // exportanim:MODE,FPS,REDUCE: a character's animations (0 as in the game, 1 the clips' channels only), keys a second, key reduction (the export kind: a character)
    {
        int a[3] = {m_exportAnim, m_exportFps, m_exportReduce ? 1 : 0};
        std::sscanf(c.arg.c_str(), "%d,%d,%d", &a[0], &a[1], &a[2]);
        m_exportAnim = a[0] == 1 ? 1 : 0; m_exportFps = a[1] >= 60 ? 60 : 30; m_exportReduce = a[2] != 0;
        m_exportKind = 2;   // (the panels' export kind: a character)
    }
    else if (c.verb == "posedump")   // posedump:[c]ASEG:T:FILE: the selected character's skinned vertices at the clip's time as the game poses it (c: a cycle, played twice first; export check)
    {
        int oid = -1;
        float t = 0.0f;
        char sz[1024] = "";
        std::string error;
        const bool fCycle = !c.arg.empty() && c.arg[0] == 'c';
        if (std::sscanf(c.arg.c_str() + (fCycle ? 1 : 0), "%d:%f:%1023[^\n]", &oid, &t, sz) != 3 || !FPoseDump(m_plSel, oid, t, fCycle, sz, error))
            Status("posedump: " + (error.empty() ? std::string("posedump:[c]ASEG:T:FILE") : error));
    }
    else if (c.verb == "texture" || c.verb == "texturegame")   // texture:MAT:FILE.png (texture-pack path, full size) / texturegame (into the game data)
    {
        const auto colon = c.arg.find(':');
        std::string error;
        if (colon == std::string::npos || !FImportTexture(c.arg.substr(colon + 1), m_plSel, std::atoi(c.arg.c_str()), c.verb == "texture" ? 0 : 1, error))
            Status("texture: " + (error.empty() ? std::string("texture:MAT:FILE.png") : error));
    }
    else if (c.verb == "material")   // material:MAT:KIND:FLAGS:RGBA(hex):CLAMP
    {
        int imat = 0, shdk = 1, grf = 2, grftex = 0;
        unsigned rgba = 0xFFFFFFFFu;
        std::string error;
        if (std::sscanf(c.arg.c_str(), "%d:%d:%d:%x:%d", &imat, &shdk, &grf, &rgba, &grftex) != 5) Status("material: MAT:KIND:FLAGS:RGBA:CLAMP");
        else
        {
            const uint32_t argba[3] = {rgba, rgba, 0};
            if (!FSetMaterial(m_plSel, imat, shdk, grf, argba, grftex, error)) Status("material: " + error);
        }
    }
    else if (c.verb == "importopt")   // importopt:SCALE,MAXTEX,LIT,COLLISION,OUTLINE (collision 0 kept, 1 none, 2 box)
    {
        float a[5] = {m_importScale, float(m_importMaxTex), m_importLit ? 1.0f : 0.0f, float(m_importCollision), m_importOutline ? 1.0f : 0.0f};
        floats(c.arg, a, 5);
        m_importScale = a[0]; m_importMaxTex = std::clamp(int(a[1]), 8, 1024); m_importLit = a[2] != 0; m_importCollision = int(a[3]); m_importOutline = a[4] != 0;
    }   // the selection's model (glob set) and textures printed
#if EDGAME_SLY2
    else if (c.verb.rfind("cut", 0) == 0 && FCutsceneVerb(c)) {}   // (editor_cutscene.inc)
#endif
    else if (c.verb == "quit") m_fQuit = true;
    else Status("unknown step " + c.verb);
    return true;
}

int CEditor::Run()
{
    std::string error;
    if (!m_inst.open(cane::path(g_disc.Path()), error)) { std::fprintf(stderr, "editor: %s\n", error.c_str()); return 1; }
#if EDGAME_SLY2
    RegisterProjectWorlds(m_inst, m_eo.project);   // (the project's new worlds: records 44.. (Sly2-Release/editor_newworld.h))
#endif
    if (m_eo.record < 0) m_eo.record = EDGAME_DEFAULT_WORLD;
    const ce::WorldEntry* pe = m_inst.world(m_eo.record);
    if (!pe) { std::fprintf(stderr, "editor: no world record %d in this installation\n", m_eo.record); return 1; }
    if (!m_index.load(m_inst, *pe, error)) { std::fprintf(stderr, "editor: %s\n", error.c_str()); return 1; }
    // the project: opened, else made (a mod package folder)
    const cane::fs::path dirProj = m_eo.project.empty() ? cane::userDirectory() / "Editor" / EDGAME_NAME / ce::pathOf(pe->name) : ce::pathOf(m_eo.project);
    std::error_code ec;
    if (cane::fs::exists(dirProj / cane::mods::kManifestName, ec)) { if (!m_proj.open(dirProj, m_inst, error)) { std::fprintf(stderr, "editor: %s\n", error.c_str()); return 1; } }
    else if (!m_proj.create(dirProj, ce::utf8(dirProj.filename()), m_inst, error)) { std::fprintf(stderr, "editor: %s\n", error.c_str()); return 1; }
    // the world's parts: the project's over the installation's (no other mods while editing)
    g_disc.SetPartReader([this](const std::string& rel, std::vector<uint8_t>& ab) { return m_proj.readPart(rel, ab); });
    ReloadText();
    if (!m_proj.historyError.empty()) std::printf("editor: %s\n", m_proj.historyError.c_str());
    else if (m_proj.canUndo() || m_proj.canRedo()) std::printf("editor: history: %zu steps kept with the project\n", m_proj.history().size());
    {
        std::stringstream ss(m_eo.script);
        std::string t;
        while (std::getline(ss, t, ';'))
        {
            if (t.empty()) continue;
            const auto colon = t.find(':');
            m_ascript.push_back({t.substr(0, colon), colon == std::string::npos ? std::string() : t.substr(colon + 1)});
        }
    }
    InitClasses();
    if (!FLoad(error)) { std::fprintf(stderr, "editor: %s\n", error.c_str()); return 1; }
    LoadViewState();
    LoadBrushes();
    std::printf("editor: %s (record %d): %zu objects, %zu groups, project %s\n", pe->name.c_str(), m_eo.record, m_psw->aploAll.size(),
                m_index.groups.size(), ce::utf8(m_proj.dir).c_str());

    g_gl.msaa = 4;
    g_display.msaaSamples = 4;
    if (!g_gl.FInit(m_eo.width, m_eo.height, !m_eo.fHidden, "SafeHouse Map Editor - " EDGAME_NAME, error)) { std::fprintf(stderr, "editor: %s\n", error.c_str()); return 1; }
    // (a hidden window is never shown: a Wayland compositor sends it no frame callbacks, so a vsync'd swap would wait
    // forever, e.g. while the screen is off; hidden runs swap without waiting)
    if (m_eo.fHidden) cane::gl::SwapInterval(0);
#if EDFEAT_MODELS
    AllowTextureModStreaming(false);   // (the project's textures are read again after each texture edit: nothing in flight)
#endif
    BuildSceneGpu(m_scene);
    ApplyProjectTextures();
    // the camera: above the world's first player start, else the origin
    // the camera: back from and above the placed models' middle
    {
        glm::vec3 sum(0.0f);
        int c = 0;
        for (ALO* palo : m_apalo)
            if (palo)
                if (const glm::vec3 p = PosWorld(palo); glm::length(p) > 1.0f) { sum += p; ++c; }
        const glm::vec3 mid = c ? sum / float(c) : glm::vec3(0.0f);
        m_cam.pos = mid + glm::vec3(-3000, 0, 1800);
        m_cam.pitch = -25;
    }
    m_cam.sFar = 1e6f;

    // the getting-started panel at the first starts (until turned off in it; <user dir>/Editor/editor.ini)
    const cane::fs::path pathIni = cane::userDirectory() / "Editor" / "editor.ini";
    {
        std::vector<uint8_t> ab;
        if (ce::readFile(pathIni, ab)) m_fHelpAtStart = std::string(ab.begin(), ab.end()).find("help_at_start=0") == std::string::npos;
        m_fShowHelp = m_fHelpAtStart && !m_eo.fHidden && m_ascript.empty();
    }
    FSetEditorLanguage(m_eo.language);   // (editor_lang.inc: the launcher's language)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(cane_font), int(sizeof(cane_font)), 17.0f, &fc);
    AddEditorLanguageFonts(io, 17.0f);
    st::apply(1.0f);
    ImGui_ImplGlfw_InitForOpenGL(g_gl.window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    Status("ready: " + std::to_string(m_psw->aploAll.size()) + " objects. Right mouse + WASD flies, click picks, W / E / R move / rotate / scale.");

    double tLast = glfwGetTime();
    while (!m_fQuit && !glfwWindowShouldClose(g_gl.window))
    {
        glfwPollEvents();
        const double t = glfwGetTime();
        const float dt = float(std::min(0.1, t - tLast));
        tLast = t;
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(g_gl.window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != g_display.outputWidth || fbh != g_display.outputHeight)) LayoutDisplay(fbw, fbh);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        SyncSelection();
        CameraInput(dt);
        UpdateLookThrough();
        DrawMenu();
        DrawToolStrip();
        DrawOutliner();
        DrawInspector();
#if EDGAME_SLY2
        if (!FTimelineOpen()) DrawWorldPanel();   // (the Timeline dock takes the bottom dock's place while open)
        DrawTimeline();
#else
        DrawWorldPanel();
#endif
        DrawStatusBar();
        DrawProblems();
        DrawFileDialogs();
        DrawHelp();
        DrawMarkers();
        DrawCameraFrusta();
        DrawPatrols();
#if EDGAME_SLY2
        DrawEntries();
#endif
        DrawBrushEdit();
        DrawCameraPanel();
        const int ipointBefore = m_ipoint;
        const bool fCurveBefore = m_fCurveMode;
        DrawPaths();
        const bool fPointPicked = m_ipoint != ipointBefore || m_fCurveMode != fCurveBefore;
        DrawGizmo();
        // a click picks (Ctrl / Shift: several); a drag from empty space draws a box (the select tool, or Alt+drag)
        if (m_axisDrag < 0 && !fPointPicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse)
        {
            m_fClickPending = true;
            m_mouseDown = io.MousePos;
        }
        if (m_fClickPending && m_axisDrag >= 0) m_fClickPending = false;
        if (m_fClickPending)
        {
            const float d = std::hypot(io.MousePos.x - m_mouseDown.x, io.MousePos.y - m_mouseDown.y);
            if (d > 6.0f && (m_tool == 0 || io.KeyAlt)) m_fBoxDrag = true;
            if (m_fBoxDrag)
            {
                ImDrawList* dlb = ImGui::GetForegroundDrawList();
                dlb->AddRectFilled(m_mouseDown, io.MousePos, st::u32(st::accent, 0.12f));
                dlb->AddRect(m_mouseDown, io.MousePos, st::u32(st::accent, 0.9f), 0, 0, 1.5f);
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (m_fBoxDrag) BoxSelect(m_mouseDown, io.MousePos, io.KeyShift || io.KeyCtrl);
                else if (d <= 6.0f) Pick(m_mouseDown.x, m_mouseDown.y);
                m_fClickPending = m_fBoxDrag = false;
            }
        }
        ApplyVisibility();
        ImGui::Render();
        UpdateShaderAnimations(float(t), dt);
        RenderPendingThumbs();
        PollOtherLevel();
        PollState();
        RenderCameraPreview();
        RenderScene(m_scene, m_cam, m_opts);
        g_gl.Present();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbw, fbh);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!m_shotPending.empty()) { glFinish(); Screenshot(m_shotPending); m_shotPending.clear(); }
        // (a hidden window is never shown: no swap, which a Wayland compositor may hold back forever for an unmapped
        // surface (seen with the screen off); screenshots read the drawn frame before this point)
        if (!m_eo.fHidden) cane::gl::SwapBuffers(g_gl.window);
        else glFinish();
        FRunScript();
        if (static const bool s_fFrames = std::getenv("SLY2N_EDFRAMES") != nullptr; s_fFrames)   // (debug: each frame's time)
            std::fprintf(stderr, "editor frame %.3f s, step %zu\n", glfwGetTime() - t, m_iscript);
    }
    if (m_proj.dirty()) std::printf("editor: closed with unsaved edits (not saved)\n");
    {
        const std::string ini = std::string("help_at_start=") + (m_fHelpAtStart ? "1" : "0") + "\n";
        std::vector<uint8_t> abOld;
        if (!ce::readFile(pathIni, abOld) || std::string(abOld.begin(), abOld.end()) != ini) ce::writeFile(pathIni, std::vector<uint8_t>(ini.begin(), ini.end()));
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_disc.SetPartReader(nullptr);
    g_gl.Terminate();
    return 0;
}
}  // namespace edimpl

#include "editor_lang.inc"
#include "editor_types.inc"
#include "editor_exist.inc"
#include "editor_camera.inc"
#include "editor_brush.inc"
#include "editor_assets.inc"
#include "editor_guard.inc"
#include "editor_build.inc"
#include "editor_states.inc"
#include "editor_newlevel.inc"
#include "editor_everything.inc"
#include "editor_brushedit.inc"

using namespace edimpl;

int EditorMain(const EDITOROPTS& eo)
{
    // (ImGui draws with OpenGL: the editor always runs on the OpenGL renderer)
    setenv(EDGAME_RENDERER_ENV, "opengl", 1);
    CEditor editor(eo);
    return editor.Run();
}

#else

int EditorMain(const EDITOROPTS&)
{
    std::fprintf(stderr, "this build has no map editor (it is part of the desktop builds started from the SafeHouse launcher)\n");
    return 2;
}

#endif
