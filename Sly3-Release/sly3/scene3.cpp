// See scene3.h.

#include "scene3.h"

#include "addr.h"
#include "condset3.h"
#include "ids3.h"

#include "../bis.h"
#include "../glob.h"
#include "../lo.h"
#include "../so.h"
#include "../shd.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
// The classes derived from ALO whose loader (vt slot 14) does not reach LoadAloFromBrx 0x23C0B8 as its first call
// (they allocate first: 0x1D2770): their heads are not read here yet.
const std::set<int> s_setcidNotAloFirst = {0x58, 0x60, 0x65, 0x6A, 0x6F, 0x74};

// A transform record of the object head (orig3: 0x28C338's records, 0x100+ bytes in RAM: +0xA0 the reference,
// +0x94 the kind, +0x9C the tag, +0x00 the local matrix (+0x30 translation)).
struct XF3
{
    int ref = 0x3000;
    int kind = 0;
    glm::mat4 mat{1.0f};
    glm::mat4 matWorld{1.0f};
    int gen = 0;   // 0 not done, 1 in progress, 2 done
};

struct ALOHEAD3
{
    std::vector<XF3> axf;
    int aref[3] = {0x3000, 0x3000, 0x3000};   // +0x48 / +0x4C / +0x50 (the third: the whole-object bounds record)
    uint32_t uMeta = 0;
};

// orig3: 0x22D6B0-like resolution (Sly 2 PxfFromRef): bits 12-13: 0 the object's own record, 0x1000 the parent's
// (none for top-level objects), 0x2000 the base stack, 0x3000 none
XF3* PxfOwn(ALOHEAD3& h, int ref)
{
    if ((ref & 0x3000) != 0) return nullptr;
    if (ref < 0 || ref >= int(h.axf.size())) return nullptr;
    return &h.axf[size_t(ref)];
}

void UpdateWorld(ALOHEAD3& h, XF3& xf)
{
    if (xf.gen == 2) return;
    if (xf.gen == 1) { xf.matWorld = xf.mat; xf.gen = 2; return; }   // (a cycle: not expected)
    xf.gen = 1;
    XF3* pref = PxfOwn(h, xf.ref);
    if (pref) UpdateWorld(h, *pref);
    xf.matWorld = pref ? pref->matWorld * xf.mat : xf.mat;
    xf.gen = 2;
}

// orig3: 0x28C338 LoadAloXfsFromBrx (as Sly 2's 0x22D7A0): S16 records x {S16 ref, S16 kind, S32 tag, U8 grf; grf 1:
// F32, else 2: F32 F32; 4: S8; 8: S8 F32 12 8 12 8 S8 S8 S32; 0x10: S8 F32 F32; 0x20: a vector; kinds 8 / 14: an S16
// origin record (identity, minus its translation), else a MATRIX4}; then the pairs 0x1A5D10 (U8 x {3 S16, vector, S16,
// vector}), links 0x182808 (S8 x {S16 S16 S8 S8}), chains 0x26EF10 (S8 x {U8, 4 F32, 2 vectors, S8 x S16}), and three
// S16 references (+0x48 / +0x4C / +0x50)
void LoadAloXfs3(CBinaryInputStream* pbis, ALOHEAD3& h)
{
    const int c = pbis->S16Read();
    if (c < 0 || c > 4096) throw std::runtime_error("transform count out of range");
    h.axf.assign(size_t(c), XF3{});
    for (int i = 0; i < c; ++i)
    {
        XF3& xf = h.axf[size_t(i)];
        xf.ref = pbis->S16Read();
        xf.kind = pbis->S16Read();
        (void)pbis->S32Read();
        const int grf = pbis->U8Read();
        if (grf & 1) (void)pbis->F32Read();
        else if (grf & 2) { (void)pbis->F32Read(); (void)pbis->F32Read(); }
        if (grf & 4) (void)pbis->S8Read();
        if (grf & 8)
        {
            uint8_t ab[12];
            (void)pbis->S8Read();
            (void)pbis->F32Read();
            pbis->ReadCb12(ab); pbis->ReadCb8(ab); pbis->ReadCb12(ab); pbis->ReadCb8(ab);
            (void)pbis->S8Read();
            (void)pbis->S8Read();
            (void)pbis->S32Read();
        }
        if (grf & 0x10) { (void)pbis->S8Read(); (void)pbis->F32Read(); (void)pbis->F32Read(); }
        if (grf & 0x20) (void)pbis->ReadVector();
        if (xf.kind == 8 || xf.kind == 14)
        {
            xf.mat = glm::mat4(1.0f);
            const int refOrigin = pbis->S16Read();
            if (XF3* po = PxfOwn(h, refOrigin)) xf.mat[3] = glm::vec4(-glm::vec3(po->mat[3]), 1.0f);
        }
        else
            xf.mat = pbis->ReadMatrix4();
    }
    // the pairs
    {
        const int n = pbis->U8Read();
        for (int i = 0; i < n; ++i)
        {
            (void)pbis->S16Read(); (void)pbis->S16Read(); (void)pbis->S16Read();
            (void)pbis->ReadVector();
            (void)pbis->S16Read();
            (void)pbis->ReadVector();
        }
    }
    // the links
    {
        const int n = pbis->S8Read();
        for (int i = 0; i < n; ++i)
        {
            (void)pbis->S16Read(); (void)pbis->S16Read(); (void)pbis->S8Read(); (void)pbis->S8Read();
        }
    }
    // the chains
    {
        const int n = pbis->S8Read();
        for (int i = 0; i < n; ++i)
        {
            (void)pbis->U8Read();
            for (int k = 0; k < 4; ++k) (void)pbis->F32Read();
            (void)pbis->ReadVector();
            (void)pbis->ReadVector();
            const int ce = pbis->S8Read();
            for (int k = 0; k < ce; ++k) (void)pbis->S16Read();
        }
    }
    for (int& r : h.aref) r = pbis->S16Read();
    for (XF3& xf : h.axf) UpdateWorld(h, xf);
}

void PlaceScene3(ALOHEAD3& h, SCENEGLOBSET& sgs, int oid)
{
    const GLOBSET& gs = *sgs.pglobset;
    sgs.oid = oid;
    sgs.amatGlob.assign(gs.aglob.size(), glm::mat4(1.0f));
    sgs.afVisible.assign(gs.aglob.size(), 1);
    const glm::mat4 matAlo = h.axf.empty() ? glm::mat4(1.0f) : h.axf[0].matWorld;
    const XF3* pxfBounds = PxfOwn(h, h.aref[2]);
    sgs.fBounds = true;
    sgs.posBounds = glm::vec3((pxfBounds ? pxfBounds->matWorld : glm::mat4(1.0f)) * glm::vec4(gs.posCenter, 1.0f));
    sgs.sBoundsRadius = gs.sRadius;
    for (size_t i = 0; i < gs.aglob.size(); ++i)
    {
        const GLOB& glob = gs.aglob[i];
        sgs.afVisible[i] = glob.fHidden ? 0 : 1;
        const XF3* pxf = glob.oidOwner >= 0 ? PxfOwn(h, glob.oidOwner) : nullptr;
        glm::mat4 m = pxf ? pxf->matWorld : matAlo;
        if (glob.fMatrix) m = m * glob.dmat;
        sgs.amatGlob[i] = m;
    }
    sgs.fTransform = true;
}

bool FObjectLoads3(const OBJHDR3& o)
{
    if (o.mode != 1) return true;
    return FFilterMatches3(int(o.afilter.size()), o.afilter.data()) &&
           FConditionsMatch3(int(o.acond.size()), o.acond.data());
}
// An object read by the walk, kept to be placed by its group's instances.
struct COLLE3   // a collision entry as read (local GEOM, its record)
{
    int ref = 0x3000;
    std::shared_ptr<SOE> psoe;
};
struct TEMPLATE3
{
    int oid = 0, cid = -1;
    bool fDraw = true;            // false: an object with collision and no globs (placed for the collision only)
    std::vector<COLLE3> acolle;
    ALOHEAD3 h;
    std::shared_ptr<GLOBSET> pglobset;
    size_t ib = 0;
};

// orig3: 0x1FA778 LoadProxyFromBrx's head (as Sly 2's 0x1C9B08): S32 the group's id (its record's +0x10), S8 mode,
// S16 the parent record, the placement MATRIX4, U32 load flags
struct PROXY3
{
    int oid = 0;
    int32_t idGroup = 0;
    int mode = 0;
    int ref = 0;
    glm::mat4 mat{1.0f};
};

// A group instance placed at mat: each object's record 0 gets mat x its local (SetLoMatrix, ALO vt slot 37), as
// 0x1FAFB0 does for the instance's objects; the group's own proxies place their groups inside it.
void PlaceGroup3(const std::vector<std::vector<TEMPLATE3>>& mpigrpatmpl, const std::vector<std::vector<PROXY3>>& mpigrpaproxy,
                 const std::vector<int>& mpidigrp, const WORLDWALK3& w, int igrp, const glm::mat4& mat, int depth, SCENE& scene,
                 SCENE3STATS& st)
{
    if (depth > 8) return;
    for (const TEMPLATE3& t : mpigrpatmpl[size_t(igrp)])
    {
        ALOHEAD3 h = t.h;
        if (!h.axf.empty()) h.axf[0].mat = mat * h.axf[0].mat;
        for (XF3& xf : h.axf) xf.gen = 0;
        for (XF3& xf : h.axf) UpdateWorld(h, xf);
        for (const COLLE3& c : t.acolle)
        {
            const XF3* pxf = PxfOwn(h, c.ref);
            SCENECOLL3 sc;
            sc.oid = t.oid;
            sc.cid = t.cid;
            sc.psoe = std::make_unique<SOE>(*c.psoe);
            const glm::mat4 m = pxf ? pxf->matWorld : glm::mat4(1.0f);
            TransformGeom(m, c.psoe->geomLocal, &sc.psoe->geomWorld);
            sc.psoe->mat = m;
            g_acoll3.push_back(std::move(sc));
        }
        if (!t.fDraw) continue;
        SCENEGLOBSET sgs;
        sgs.ib = t.ib;
        sgs.pglobset = t.pglobset;
        PlaceScene3(h, sgs, t.oid);
        scene.aglobset.push_back(std::move(sgs));
        st.aamatXf.emplace_back();
        for (const XF3& xf : h.axf) st.aamatXf.back().push_back(xf.matWorld);
        ++st.cplaced;
    }
    for (const PROXY3& p : mpigrpaproxy[size_t(igrp)])
    {
        auto it = std::find_if(w.agroup.begin(), w.agroup.end(), [&](const GROUP3& g) { return g.x10 == p.idGroup; });
        if (it == w.agroup.end()) { ++st.cproxyNoGroup; continue; }
        ++st.cproxy;
        PlaceGroup3(mpigrpatmpl, mpigrpaproxy, mpidigrp, w, int(it - w.agroup.begin()), mat * p.mat, depth + 1, scene, st);
    }
}
}  // namespace

const std::vector<uint8_t>* g_pramPools3 = nullptr;
std::vector<SCENECOLL3> g_acoll3;

int ShowSceneGlobs3(SCENEGLOBSET& sgs, int32_t n0, bool fShow)
{
    if (!sgs.pglobset) return 0;
    const GLOBSET& gs = *sgs.pglobset;
    if (sgs.afVisible.size() != gs.aglob.size()) sgs.afVisible.assign(gs.aglob.size(), 1);
    int c = 0;
    for (size_t i = 0; i < gs.aglob.size(); ++i)
    {
        if (gs.aglob[i].n0 != n0) continue;
        if (gs.aglob[i].fNoMesh) continue;   // (no mesh: nothing to draw either way)
        sgs.afVisible[i] = fShow ? 1 : 0;
        ++c;
    }
    return c;
}

std::vector<int> AcinstGroups3(const WORLDWALK3& w, uint32_t idBlock)
{
    std::vector<int> acinst(w.agroup.size());
    for (size_t i = 0; i < w.agroup.size(); ++i) acinst[i] = w.agroup[i].cinst;
    for (size_t iblk = 0; iblk < w.aidOptset.size() && iblk < w.aagrpcount.size(); ++iblk)
    {
        if (w.aidOptset[iblk] != idBlock) continue;
        for (const auto& [id, c] : w.aagrpcount[iblk])
            for (size_t i = 0; i < w.agroup.size(); ++i)   // 0x169500: the first group whose +0x10 is the id
                if (w.agroup[i].x10 == id) { acinst[i] = c; break; }
    }
    return acinst;
}

int CApplyRamPose3(SCENE& scene, const SCENE3STATS& st, const std::vector<uint8_t>& ram)
{
    if (ram.size() < 0x2000000) return 0;
    auto u32 = [&](uint32_t va) { uint32_t u; std::memcpy(&u, ram.data() + (va & 0x1FFFFFF), 4); return u; };
    auto f32 = [&](uint32_t va) { float f; std::memcpy(&f, ram.data() + (va & 0x1FFFFFF), 4); return f; };
    // the ALOs of RAM by oid: SW's per-class lists (SW +0x3020 + 12 x cid: {first, ?, link offset}; object +0x18 oid)
    std::map<int, std::vector<uint32_t>> mpoidapalo;
    for (int cid = 0; cid < 249; ++cid)
    {
        if (!FCid3DerivedFrom(cid, CID3::ALO)) continue;
        const uint32_t l = orig3::VA_SW + 0x3020 + 12u * uint32_t(cid);
        const uint32_t off = u32(l + 8);
        int c = 0;
        for (uint32_t p = u32(l); p >= 0x100000 && p < 0x2000000 && c < 100000; p = u32(p + off), ++c)
            mpoidapalo[int(u32(p + 0x18))].push_back(p);
    }
    auto matRam = [&](uint32_t x) {
        glm::mat4 m(1.0f);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) m[r][c] = f32(x + 0x40 + 0x10 * uint32_t(r) + 4 * uint32_t(c));
        m[3] = glm::vec4(f32(x + 0x70), f32(x + 0x74), f32(x + 0x78), 1.0f);
        return m;
    };
    int cposed = 0;
    for (size_t k = 0; k < scene.aglobset.size() && k < st.aamatXf.size(); ++k)
    {
        SCENEGLOBSET& sgs = scene.aglobset[k];
        const std::vector<glm::mat4>& amatFile = st.aamatXf[k];
        auto it = mpoidapalo.find(sgs.oid);
        if (it == mpoidapalo.end() || it->second.size() != 1) continue;
        const uint32_t palo = it->second[0];
        const int cxf = int(u32(palo + 0x40));
        if (cxf != int(amatFile.size()) || cxf == 0) continue;
        const uint32_t axf = u32(palo + 0x44);
        std::vector<glm::mat4> amatRam(static_cast<size_t>(cxf));
        for (int i = 0; i < cxf; ++i) amatRam[size_t(i)] = matRam(axf + 0x100u * uint32_t(i));
        const GLOBSET& gs = *sgs.pglobset;
        // the light context's position: the root record's (Sly 2's 0x1F2500, XF +0x70 of the root object), shared by
        // every group of the object (the forward lighting's lights and the cast shadows' direction are chosen there)
        sgs.fPosLight = true;
        sgs.posLight = glm::vec3(amatRam[0][3]);
        sgs.aamatBone.assign(gs.aglob.size(), {});
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            const GLOB& glob = gs.aglob[i];
            const int iown = glob.oidOwner >= 0 && glob.oidOwner < cxf ? glob.oidOwner : 0;
            glm::mat4 m = amatRam[size_t(iown)];
            if (glob.fMatrix) m = m * glob.dmat;
            sgs.amatGlob[i] = m;
            const GLOB& geom = glob.iglobTemplate >= 0 ? gs.aglob[size_t(glob.iglobTemplate)] : glob;
            if (geom.abone.empty()) continue;
            std::vector<glm::mat4>& amatBone = sgs.aamatBone[i];
            for (const GLOBBONE& b : geom.abone)
            {
                const bool fOwn = b.oid == sgs.oid && b.ibone >= 0 && b.ibone < cxf;
                if (!fOwn) { amatBone.push_back(sgs.amatGlob[i]); continue; }
                amatBone.push_back(amatRam[size_t(b.ibone)] * glm::inverse(amatFile[size_t(b.ibone)]) * amatFile[size_t(iown)]);
            }
        }
        sgs.posBounds = glm::vec3(amatRam[0][3]);
        ++cposed;
    }
    return cposed;
}

bool FBuildScene3(const std::vector<uint8_t>& ab, const WORLDWALK3& w, SCENE& scene, SCENE3STATS& st, std::string& error)
{
    CBinaryInputStream bis;
    bis.OpenMemory(int(ab.size()), ab.data());
    CBinaryInputStream* pbis = &bis;
    ResetShaders();
    g_acoll3.clear();
    // the objects by group
    std::vector<std::vector<const OBJHDR3*>> mpigrpapo(w.agroup.size());
    for (const OBJHDR3& o : w.aobj)
        if (o.igrp >= 0 && o.igrp < int(w.agroup.size())) mpigrpapo[size_t(o.igrp)].push_back(&o);
    std::vector<std::vector<TEMPLATE3>> mpigrpatmpl(w.agroup.size());
    std::vector<std::vector<PROXY3>> mpigrpaproxy(w.agroup.size());
    const std::vector<int> acinst = AcinstGroups3(w, uint32_t(g_condset3.cond));
    for (size_t igrp = 0; igrp < w.agroup.size(); ++igrp)
    {
        const GROUP3& g = w.agroup[igrp];
        ++st.cgroup;
        if (g.fSkipped) continue;
        if (acinst[igrp] <= 0) { ++st.cgroupNoInst; continue; }   // 0x14F760: Skip(cb)
        try
        {
            pbis->m_ib = g.ibStart;
            LoadBulkHeaderFromBrx(nullptr, pbis);
            pbis->m_ib = g.ibShaders;
            LoadShadersFromBrx(nullptr, pbis);
            if (pbis->m_ib != g.ibCmesh)
            {
                char sz[160];
                std::snprintf(sz, sizeof(sz), "group %zu (%s): the shader block ended at %zu, the walk says %zu", igrp, g.achName,
                              pbis->m_ib, g.ibCmesh);
                error = sz;
                return false;
            }
            ++st.cgroupShd;
        }
        catch (const std::exception& e)
        {
            error = std::string("group ") + g.achName + ": shaders: " + e.what();
            return false;
        }
        for (const OBJHDR3* po : mpigrpapo[igrp])
        {
            ++st.cobj;
            if (FCid3DerivedFrom(po->cid, CID3::PROXY))
            {
                if (!FObjectLoads3(*po)) { ++st.cobjFiltered; continue; }
                pbis->m_ib = po->ibBody;
                PROXY3 p;
                p.oid = po->oid;
                p.idGroup = pbis->S32Read();
                p.mode = pbis->S8Read();
                p.ref = pbis->S16Read();
                p.mat = pbis->ReadMatrix4();
                mpigrpaproxy[igrp].push_back(p);
                continue;
            }
            if (!FCid3DerivedFrom(po->cid, CID3::ALO)) continue;
            if (!FObjectLoads3(*po)) { ++st.cobjFiltered; continue; }
            if (s_setcidNotAloFirst.count(po->cid) || Cls3(po->cid).pfnLoad == 0) { ++st.cnotFirst; continue; }
            ++st.cobjAlo;
            try
            {
                pbis->m_ib = po->ibBody;
                ALOHEAD3 h;
                LoadAloXfs3(pbis, h);
                h.uMeta = pbis->U32Read();
                SCENEGLOBSET sgs;
                sgs.ib = pbis->m_ib;
                sgs.pglobset = std::make_shared<GLOBSET>();
                LoadGlobsetFromBrx(sgs.pglobset.get(), nullptr, pbis);
                if (pbis->m_ib > po->ibBody + size_t(po->cb)) throw std::runtime_error("the head runs past the body");
                static const bool s_fDump = std::getenv("SLY3_SCENE_DUMP") != nullptr;   // debugging: the glob sets' ranges
                if (s_fDump) std::printf("globset oid %d cid 0x%X %zu %zu globs %zu\n", po->oid, po->cid, sgs.ib, pbis->m_ib, sgs.pglobset->aglob.size());
                // the collision entries (orig3 0x23C0B8 after the glob set; loaders3.cpp LoadAloFromBrx3's reads)
                std::vector<COLLE3> acolle;
                const int csoe = pbis->S16Read();
                for (int i = 0; i < csoe; ++i)
                {
                    COLLE3 c;
                    c.psoe = std::make_shared<SOE>();
                    SOE& e = *c.psoe;
                    c.ref = pbis->S16Read();
                    e.tag = pbis->S32Read();
                    e.f48 = pbis->S8Read() & 1;
                    e.f49 = pbis->S8Read() & 1;
                    e.sRadius = pbis->F32Read();
                    pbis->S8Read();
                    pbis->ReadGeom(&e.geomLocal);
                    e.geomWorld = e.geomLocal;
                    pbis->ReadBsp(&e.geomWorld, &e.bsp);
                    e.id = pbis->U32Read();
                    e.state = static_cast<int16_t>(pbis->U16Read());
                    pbis->S8Read();
                    e.pinned = pbis->S16Read();
                    e.f50 = pbis->S8Read() & 1;
                    acolle.push_back(std::move(c));
                }
                if (pbis->m_ib > po->ibBody + size_t(po->cb)) throw std::runtime_error("the collision runs past the body");
                const bool fEmpty = sgs.pglobset->aglob.empty();
                if (fEmpty) ++st.cglobsetEmpty;
                if (fEmpty && acolle.empty()) continue;
                TEMPLATE3 t;
                t.oid = po->oid;
                t.cid = po->cid;
                t.fDraw = !fEmpty;
                t.acolle = std::move(acolle);
                t.h = std::move(h);
                t.pglobset = sgs.pglobset;
                t.ib = sgs.ib;
                mpigrpatmpl[igrp].push_back(std::move(t));
                if (!fEmpty) ++st.cglobset;
            }
            catch (const std::exception& e)
            {
                ++st.cfail;
                if (st.aerr.size() < 20)
                {
                    char sz[200];
                    std::snprintf(sz, sizeof(sz), "oid %d cid 0x%X (%s) group %s at %zu: %s", po->oid, po->cid, SzCid3(po->cid),
                                  g.achName, pbis->m_ib, e.what());
                    st.aerr.push_back(sz);
                }
            }
        }
        EndShaderGroup();
    }
    // the groups no proxy instantiates are placed where their file puts them; the others by their proxies
    std::vector<int> mpidigrp;
    std::vector<uint8_t> afReferenced(w.agroup.size(), 0);
    for (const auto& ap : mpigrpaproxy)
        for (const PROXY3& p : ap)
            for (size_t i = 0; i < w.agroup.size(); ++i)
                if (w.agroup[i].x10 == p.idGroup) afReferenced[i] = 1;
    if (std::getenv("SLY3_SCENE_GROUPS"))   // debugging: the groups placed at their file position and what they draw
        for (size_t igrp = 0; igrp < w.agroup.size(); ++igrp)
            if (!afReferenced[igrp] && !mpigrpatmpl[igrp].empty())
            {
                const GROUP3& g = w.agroup[igrp];
                std::printf("unplaced group %zu %s x5C %d x60 %d x10 %d x14 %d inst %d:", igrp, g.achName, g.x5C, g.x60, g.x10, g.x14, g.cinst);
                for (const TEMPLATE3& t : mpigrpatmpl[igrp]) std::printf(" %d", t.oid);
                std::printf("\n");
            }
    static const bool s_fAllGroups = std::getenv("SLY3_SCENE_ALLGROUPS") != nullptr;   // debugging: the pools too
    for (size_t igrp = 0; igrp < w.agroup.size(); ++igrp)
    {
        if (afReferenced[igrp]) continue;
        if (w.agroup[igrp].x5C == 0 && !s_fAllGroups)
        {
            st.cgroupPool += std::any_of(mpigrpatmpl[igrp].begin(), mpigrpatmpl[igrp].end(), [](const TEMPLATE3& t) { return t.fDraw; }) ? 1 : 0;
            // a RAM dump's live instances of the pool (debugging stand-in for the spawners: --ram3-pools)
            if (g_pramPools3 && !mpigrpatmpl[igrp].empty() && mpigrpatmpl[igrp][0].fDraw)
            {
                const std::vector<uint8_t>& ram = *g_pramPools3;
                auto u32 = [&](uint32_t va) { uint32_t u; std::memcpy(&u, ram.data() + (va & 0x1FFFFFF), 4); return u; };
                auto f32 = [&](uint32_t va) { float f; std::memcpy(&f, ram.data() + (va & 0x1FFFFFF), 4); return f; };
                const TEMPLATE3& t = mpigrpatmpl[igrp][0];
                const GLOBSET& gs = *t.pglobset;
                uint8_t key[16];
                std::memcpy(key, &gs.posCenter, 12);
                std::memcpy(key + 12, &gs.sRadius, 4);
                for (uint32_t va = 0x100000; va + 0x40 < ram.size(); va += 0x10)
                {
                    if (std::memcmp(ram.data() + va + 0x10, key, 12) || std::memcmp(ram.data() + va + 0x20, key + 12, 4)) continue;
                    if (u32(va + 0x38) != gs.aglob.size()) continue;
                    const uint32_t palo = u32(va), pxf = u32(palo + 0x44);   // the ALO's XF: +0x40 world rows, +0x70 position
                    if (u32(palo + 0x18) != uint32_t(t.oid) || pxf < 0x100000 || pxf >= 0x2000000) continue;
                    // in the world: the LO flag word +0x28's bit 59 (Sly 2's bit 60, fInWorld; the drawn objects have it,
                    // the pools' spare instances do not)
                    if (!(u32(palo + 0x2C) & 0x08000000u)) continue;
                    glm::mat4 m(1.0f);
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) m[r][c] = f32(pxf + 0x40 + 0x10 * uint32_t(r) + 4 * uint32_t(c));
                    m[3] = glm::vec4(f32(pxf + 0x70), f32(pxf + 0x74), f32(pxf + 0x78), 1.0f);
                    const glm::mat4 matLocal0 = t.h.axf.empty() ? glm::mat4(1.0f) : t.h.axf[0].mat;
                    PlaceGroup3(mpigrpatmpl, mpigrpaproxy, mpidigrp, w, int(igrp), m * glm::inverse(matLocal0), 0, scene, st);
                    ++st.cpoolRam;
                }
            }
            continue;
        }
        PlaceGroup3(mpigrpatmpl, mpigrpaproxy, mpidigrp, w, int(igrp), glm::mat4(1.0f), 0, scene, st);
    }
    scene.cblockShd = st.cgroupShd;
    scene.cblockBulk = g_shdstats.cbulk;
    return true;
}
