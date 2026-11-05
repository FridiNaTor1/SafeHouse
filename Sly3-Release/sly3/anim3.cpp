// Sly 3's animation runtime: the checks (anim3.h).

#include "anim3.h"

#include "aseg3.h"
#include "ids3.h"
#include "load3.h"
#include "trk3.h"

#include "../asega.h"
#include "../sw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace sly3
{
namespace
{
bool FClose(float a, float b, float eps) { return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b)); }
}  // namespace

int CheckAsegSegments3(SW* psw, const char* pszFile, FILE* pf)
{
    FILE* pfIn = std::fopen(pszFile, "r");
    if (!pfIn)
    {
        std::fprintf(pf, "aseg-check3: cannot open %s\n", pszFile);
        return 1;
    }
    std::multimap<OID, ASEG3*> mpoidpaseg;
    for (const auto& p : psw->aploAll)
        if (p->Cid() == 0x82 || p->Cid() == 0x83) mpoidpaseg.emplace(p->oid, static_cast<ASEG3*>(p.get()));
    int cline = 0, cmiss = 0, cnoAseg = 0, cexactT = 0, cexactG0 = 0, cnull = 0;
    std::map<int, std::pair<int, int>> mpkindc;  // track kind -> (lines, exact at g0)
    char sz[2048];
    while (std::fgets(sz, sizeof(sz), pfIn))
    {
        if (std::strncmp(sz, "seg ", 4)) continue;
        int oidAseg, oidParent, cchn, ichn, oidAlo, ixf, state, mode, cmode, prio;
        float t, svt, g0, g1, ag[12];
        if (std::sscanf(sz + 4, "%d %d %d %d %d %d %f %f %f %f %d %d %d %d %f %f %f %f %f %f %f %f %f %f %f %f", &oidAseg, &oidParent, &cchn, &ichn,
                        &oidAlo, &ixf, &t, &svt, &g0, &g1, &state, &mode, &cmode, &prio, &ag[0], &ag[1], &ag[2], &ag[3],
                        &ag[4], &ag[5], &ag[6], &ag[7], &ag[8], &ag[9], &ag[10], &ag[11]) != 26)
            continue;
        ++cline;
        // every native ASEG of that oid / parent / channel count (instances may carry their own data)
        std::vector<const TRKP*> aptrkp;
        for (auto [it, end] = mpoidpaseg.equal_range(oidAseg); it != end; ++it)
        {
            ASEGD* pd = PasegdOf(it->second);
            const OID oidPar = it->second->paloParent ? it->second->paloParent->oid : -1;
            if (oidPar != oidParent || int(pd->achn.size()) != cchn) continue;
            if (ichn < 0 || std::size_t(ichn) >= pd->achn.size()) continue;
            if (const TRKP* p = pd->achn[std::size_t(ichn)].ptrkp.get()) aptrkp.push_back(p);
            else ++cnull;
        }
        if (aptrkp.empty())
        {
            ++cnoAseg;
            continue;
        }
        const TRKP* ptrkp = aptrkp[0];
        // at the ASEGA's time, then at the segment's own start (the record may not have refetched since)
        bool afOk[2] = {false, false};
        XF xfMiss;
        for (int pass = 0; pass < 2; ++pass)
            for (const TRKP* ptrk : aptrkp)
            {
                XF xf;
                const float tEval = pass == 0 ? t : g0;
                xf.pg = &tEval;
                xf.pdgScale = &svt;
                ptrk->Setup(&xf);
                bool f = FClose(xf.g0, g0, 1e-6f) && FClose(xf.g1, g1, 1e-6f) && xf.state == state;
                for (int j = 0; j < 4 && f; ++j)
                    for (int i = 0; i < 3 && f; ++i) f = FClose(xf.aseg[j][i], ag[j * 3 + i], 1e-5f);
                if (f) { afOk[pass] = true; ptrkp = ptrk; break; }
                if (ptrk == aptrkp[0]) xfMiss = xf;
            }
        if (!afOk[1] && cmiss < 20)
        {
            const XF& xf = xfMiss;
            std::fprintf(pf, "aseg-check3 miss: aseg %d chn %d (kind %d, %zu candidates) alo %d xf %d t %g: native g %g %g state %d "
                         "p0 (%g %g %g) v0 (%g %g %g) p1 (%g %g %g) v1 (%g %g %g) | ram g %g %g state %d p0 (%g %g %g) "
                         "v0 (%g %g %g) p1 (%g %g %g) v1 (%g %g %g)\n", oidAseg, ichn, ptrkp->kind, aptrkp.size(), oidAlo, ixf, t,
                         xf.g0, xf.g1, xf.state, xf.aseg[0].x, xf.aseg[0].y, xf.aseg[0].z, xf.aseg[1].x, xf.aseg[1].y,
                         xf.aseg[1].z, xf.aseg[2].x, xf.aseg[2].y, xf.aseg[2].z, xf.aseg[3].x, xf.aseg[3].y,
                         xf.aseg[3].z, g0, g1, state, ag[0], ag[1], ag[2], ag[3], ag[4], ag[5], ag[6], ag[7], ag[8],
                         ag[9], ag[10], ag[11]);
        }
        cexactT += afOk[0];
        cexactG0 += afOk[1];
        cmiss += !afOk[1];
        auto& kc = mpkindc[ptrkp->kind];
        ++kc.first;
        kc.second += afOk[1];
    }
    std::fclose(pfIn);
    std::fprintf(pf, "aseg-check3: %d records: %d set up as the RAM's at the ASEGA's time, %d at the segment's start; "
                 "%d mismatched, %d without the ASEG / channel natively (%d channels without a position track)\n",
                 cline, cexactT, cexactG0, cmiss, cnoAseg, cnull);
    for (auto [kind, c] : mpkindc) std::fprintf(pf, "  track kind %d: %d of %d\n", kind, c.second, c.first);
    return cmiss + cnoAseg;
}
}  // namespace sly3

// ---------------------------------------------------------------- the runtime in the game

#include "game3.h"
#include "scene3.h"

#include "../anim.h"
#include "../animfind.h"
#include "../clock.h"
#include "../glob.h"
#include "../scene.h"
#include "../xf.h"

#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

extern CLOCK g_clock3;  // game3.cpp

namespace sly3
{
namespace
{
const ACTORANIM3 s_aactoranim3[] = {
#include "actoraseg3.inc"
};
}  // namespace
}  // namespace sly3

namespace sly3
{
namespace
{
struct ANIM3
{
    SW* psw = nullptr;
    SCENE* pscene = nullptr;
    const SCENE3STATS* pst = nullptr;
    std::vector<ALO*> apaloScene;  // per scene entry: its native ALO (null: none)
    std::vector<glm::mat4> amatCorr;  // per scene entry: its placement x the inverse of the native root's at the start
                                      // (the scene places some instances the native records do not: kept as placed)
    std::unordered_map<ALO*, std::vector<std::size_t>> mpaloaient;
    std::unordered_map<std::size_t, std::vector<uint8_t>> mpientafVisible;  // hidden entries: their own visibility
    bool fHook = false;
    bool fOn = false;
};
ANIM3 s_anim3;

// The scene entries' native objects: same oid, same record count, the nearest root to the entry's placement
void MapSceneEntries3()
{
    ANIM3& a = s_anim3;
    a.apaloScene.assign(a.pscene->aglobset.size(), nullptr);
    a.amatCorr.assign(a.pscene->aglobset.size(), glm::mat4(1.0f));
    a.mpaloaient.clear();
    a.mpientafVisible.clear();
    std::unordered_multimap<OID, ALO*> mpoidpalo;
    for (const auto& p : a.psw->aploAll)
        if (FCidDerivedFrom(p->Cid(), Icid(CID3::ALO)) && !static_cast<ALO*>(p.get())->axf.empty())
            mpoidpalo.emplace(p->oid, static_cast<ALO*>(p.get()));
    int cmapped = 0, cnear = 0;
    for (std::size_t k = 0; k < a.pscene->aglobset.size() && k < a.pst->aamatXf.size(); ++k)
    {
        const std::vector<glm::mat4>& amatFile = a.pst->aamatXf[k];
        if (amatFile.empty()) continue;
        ALO* pbest = nullptr;
        float dBest = 1e30f;
        for (auto [it, end] = mpoidpalo.equal_range(a.pscene->aglobset[k].oid); it != end; ++it)
        {
            if (it->second->axf.size() != amatFile.size()) continue;
            const float d = glm::length(glm::vec3(it->second->axf[0]->matWorld[3]) - glm::vec3(amatFile[0][3]));
            if (d < dBest) { dBest = d; pbest = it->second; }
        }
        if (!pbest) continue;
        a.apaloScene[k] = pbest;
        a.amatCorr[k] = amatFile[0] * glm::inverse(pbest->axf[0]->matWorld);
        a.mpaloaient[pbest].push_back(k);
        ++cmapped;
        cnear += dBest < 1.0f;
    }
    if (std::getenv("SLY3_ANIMPRINT"))
        std::printf("anim3: %d of %zu scene entries have their native object (%d placed within 1 unit)\n", cmapped,
                    a.pscene->aglobset.size(), cnear);
}

void PoseEntry3(SCENEGLOBSET& sgs, const std::vector<glm::mat4>& amatFile, const ALO* palo, const glm::mat4& matCorr)
{
    const int cxf = int(palo->axf.size());
    const GLOBSET& gs = *sgs.pglobset;
    if (sgs.amatGlob.size() != gs.aglob.size()) sgs.amatGlob.assign(gs.aglob.size(), glm::mat4(1.0f));
    sgs.aamatBone.assign(gs.aglob.size(), {});
    for (std::size_t i = 0; i < gs.aglob.size(); ++i)
    {
        const GLOB& glob = gs.aglob[i];
        const int iown = glob.oidOwner >= 0 && glob.oidOwner < cxf ? glob.oidOwner : 0;
        glm::mat4 m = matCorr * palo->axf[std::size_t(iown)]->matWorld;
        if (glob.fMatrix) m = m * glob.dmat;
        sgs.amatGlob[i] = m;
        const GLOB& geom = glob.iglobTemplate >= 0 ? gs.aglob[std::size_t(glob.iglobTemplate)] : glob;
        if (geom.abone.empty()) continue;
        std::vector<glm::mat4>& amatBone = sgs.aamatBone[i];
        for (const GLOBBONE& b : geom.abone)
        {
            const bool fOwn = b.oid == sgs.oid && b.ibone >= 0 && b.ibone < cxf;
            if (!fOwn) { amatBone.push_back(sgs.amatGlob[i]); continue; }
            amatBone.push_back(matCorr * palo->axf[std::size_t(b.ibone)]->matWorld * glm::inverse(amatFile[std::size_t(b.ibone)]) *
                               amatFile[std::size_t(iown)]);
        }
    }
    sgs.posBounds = glm::vec3((matCorr * palo->axf[0]->matWorld)[3]);
    sgs.fTransform = true;
}

// The ALOs the playing ASEGAs drive (their channel controllers' objects)
std::vector<ALO*> ApaloAnimated3()
{
    std::unordered_set<ALO*> set;
    std::vector<ALO*> a;
    for (ASEGA* pasega : g_apasega)
    {
        if (pasega->fRetracted) continue;
        for (CTRLASEG* pc = pasega->pctrlFirst; pc; pc = pc->pnextAsega)
            if (pc->palo && set.insert(pc->palo).second) a.push_back(pc->palo);
    }
    return a;
}
}  // namespace

int CasegaPlaying3()
{
    int c = 0;
    for (ASEGA* p : g_apasega) c += !p->fRetracted;
    return c;
}

void ShowAloEntries3(ALO* palo, bool fShown)
{
    ANIM3& a = s_anim3;
    auto it = a.mpaloaient.find(palo);
    if (!a.pscene || it == a.mpaloaient.end()) return;
    for (std::size_t k : it->second)
    {
        if (k >= a.pscene->aglobset.size()) continue;
        std::vector<uint8_t>& af = a.pscene->aglobset[k].afVisible;
        auto itSaved = a.mpientafVisible.find(k);
        if (!fShown && itSaved == a.mpientafVisible.end())
        {
            a.mpientafVisible[k] = af;
            std::fill(af.begin(), af.end(), 0);
        }
        else if (fShown && itSaved != a.mpientafVisible.end())
        {
            af = itSaved->second;
            a.mpientafVisible.erase(itSaved);
        }
    }
}

ASEG* PasegFind3(SW* psw, int oid, ALO* paloNear)
{
    SW* pswSave = g_psw;
    g_psw = psw;
    LO* plo = PloFindSwNearest(paloNear, oid);
    g_psw = pswSave;
    return plo && FCidDerivedFrom(plo->Cid(), Icid(CID3::ASEG)) ? static_cast<ASEG*>(plo) : nullptr;
}

void StartWorldAnimation3(SW* psw, SCENE* pscene, const SCENE3STATS* pst)
{
    ANIM3& a = s_anim3;
    a = ANIM3{.fHook = a.fHook};
    if (std::getenv("SLY3_NOANIM")) return;
    a.psw = psw;
    a.pscene = pscene;
    a.pst = pst;
    a.fOn = true;
    g_psw = psw;  // the copied runtime's finds (animfind.cpp) walk this world
    g_clock = g_clock3;
    InitWorldAnimationRecords(psw);
    UpdateWorldXfWorlds(psw);
    MapSceneEntries3();
    for (const auto& plo : psw->aploAll) PostLoadAnimLo(plo.get());
    if (std::getenv("SLY3_ANIMPRINT"))
    {
        int cAseg = 0, cApply = 0, cSm = 0, cSmInit = 0;
        for (const auto& plo : psw->aploAll)
        {
            if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ASEG))) { ++cAseg; cApply += static_cast<ASEG*>(plo.get())->fApplyParams; }
            if (plo->Cid() == Icid(CID3::SM)) { ++cSm; cSmInit += static_cast<SM*>(plo.get())->fInitial; }
        }
        std::printf("anim3: %d ASEGs (%d apply themselves), %d SMs (%d with an initial SMA): %d ASEGAs playing\n", cAseg,
                    cApply, cSm, cSmInit, CasegaPlaying3());
        int cfield = 0, cfound = 0;
        for (const auto& plo : psw->aploAll)
            if (plo->Cid() == 0x34 || plo->Cid() == 0x36)
                for (const ACTORANIM3& e : s_aactoranim3)
                    if (e.cidActor == plo->Cid() && e.oid >= 0)
                    {
                        ++cfield;
                        cfound += PloActorAnimField3(static_cast<ALO*>(plo.get()), e.ib) != nullptr;
                    }
        std::printf("anim3: the players' animation fields: %d of %d found natively\n", cfound, cfield);
    }
    if (!a.fHook)
    {
        a.fHook = true;
        RegisterFrameHook3([](float dt) { UpdateWorldAnimation3(dt); });
    }
}

// SLY3_ANIMSTEP=FRAME:IN:OUT (tests against the reference's consecutive RAM dumps): at that frame, before the ASEGAs'
// update, every native ASEGA takes the time / speed of the same (ASEG, object) one in IN (tools/asegram3.py --asegas
// of the first dump, in order); after the frame its channels' records go to OUT ("xf ASEGOID ICHN ALOOID IXF MODE
// 9 rows 3 pos", tools/asegram3.py --xfs prints the next dump's)
struct ANIMSTEP3
{
    int frame = -1;
    std::string in, out, xfin;  // xfin: tools/asegram3.py --xfs of the same dump: the records' local matrices / velocities
};
ANIMSTEP3 AnimStep3()
{
    ANIMSTEP3 st;
    if (const char* sz = std::getenv("SLY3_ANIMSTEP"))
    {
        const std::string s(sz);
        const auto i1 = s.find(':'), i2 = s.find(':', i1 + 1);
        if (i1 != std::string::npos && i2 != std::string::npos)
        {
            st.frame = std::atoi(s.substr(0, i1).c_str());
            st.in = s.substr(i1 + 1, i2 - i1 - 1);
            st.out = s.substr(i2 + 1);
            if (const auto i3 = st.out.find(':'); i3 != std::string::npos)
            {
                st.xfin = st.out.substr(i3 + 1);
                st.out = st.out.substr(0, i3);
            }
        }
    }
    return st;
}

// The ASEGs in creation order with their instance index among the ASEGs of the same oid (as the RAM's class lists
// keep them), and their playing ASEGAs
template <class F> void ForEachAsegaByInstance3(F f)
{
    std::map<int, int> mpoidc;
    for (const auto& plo : s_anim3.psw->aploAll)
    {
        if (!FCidDerivedFrom(plo->Cid(), Icid(CID3::ASEG))) continue;
        ASEG* paseg = static_cast<ASEG*>(plo.get());
        const int inst = mpoidc[paseg->oid]++;
        for (ASEGA* p : std::vector<ASEGA*>(paseg->apasega))
            if (!p->fRetracted) f(p, inst);
    }
}

void ImportAsegaTimes3(const std::string& file)
{
    FILE* pf = std::fopen(file.c_str(), "r");
    if (!pf) return;
    struct REC { float t, svt; int grfapl, oidParent; };
    std::map<std::pair<int, int>, std::vector<REC>> mp;
    char sz[256];
    while (std::fgets(sz, sizeof(sz), pf))
    {
        int oidAseg, inst, oidAlo, grfapl = 0, oidParent = -1;
        float t, svt;
        if (std::sscanf(sz, "asega %d:%d %d %f %f %d %d", &oidAseg, &inst, &oidAlo, &t, &svt, &grfapl, &oidParent) >= 5)
            mp[{oidAseg * 1000 + inst, oidAlo}].push_back({t, svt, grfapl, oidParent});
    }
    std::fclose(pf);
    std::map<std::pair<int, int>, std::size_t> mpi;
    int cset = 0, capplied = 0;
    ForEachAsegaByInstance3([&](ASEGA* p, int inst) {
        const std::pair<int, int> k{p->paseg->oid * 1000 + inst, p->palo ? p->palo->oid : -1};
        auto it = mp.find(k);
        if (it == mp.end()) return;
        std::size_t& i = mpi[k];
        if (i >= it->second.size()) return;
        SetAsegaTime(p, 0, it->second[i].t, it->second[i].svt);
        ++i;
        ++cset;
    });
    // the ones the actors' code plays (Sly, Bentley, ...): applied on that object (its ASEG found around it)
    for (auto& [k, arec] : mp)
    {
        if (k.second < 0) continue;
        for (std::size_t i = mpi[k]; i < arec.size(); ++i)
        {
            ALO* palo = nullptr;
            int c = 0;
            for (const auto& plo : s_anim3.psw->aploAll)
                if (plo->oid == k.second && FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) { palo = static_cast<ALO*>(plo.get()); ++c; }
            if (c != 1) continue;
            ASEG* paseg = PasegFind3(s_anim3.psw, k.first / 1000, palo);
            if (!paseg) continue;
            ApplyAseg(paseg, palo, arec[i].t, arec[i].svt, arec[i].grfapl, nullptr);
            ++capplied;
        }
    }
    std::printf("anim3: %d ASEGAs took the times of %s, %d applied from it\n", cset, file.c_str(), capplied);
}

void PrintAsegaXfs3(const std::string& file)
{
    FILE* pf = std::fopen(file.c_str(), "w");
    if (!pf) return;
    ForEachAsegaByInstance3([&](ASEGA* p, int inst) {
        const ASEGD* pd = PasegdOf(p->paseg);
        for (CTRLASEG* pc = p->pctrlFirst; pc; pc = pc->pnextAsega)
        {
            if (!pc->palo || !pc->pxf) continue;
            int ichn = -1, ixf = -1;
            for (std::size_t i = 0; i < pd->achn.size(); ++i)
                if (pd->achn[i].ptrkp.get() == pc->ptrkp && pc->ptrkp) ichn = int(i);
            for (std::size_t i = 0; i < pc->palo->axf.size(); ++i)
                if (pc->palo->axf[i].get() == pc->pxf) ixf = int(i);
            const glm::mat4& m = pc->pxf->mat;
            const int mode = pc->pxf->mode >= 6 ? pc->pxf->mode + 1 : pc->pxf->mode;  // in Sly 3's numbering (anim3.h)
            std::fprintf(pf, "xf %d:%d %d %d %d %d", p->paseg->oid, inst, ichn, pc->palo->oid, ixf, mode);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) std::fprintf(pf, " %.6g", m[r][c]);
            std::fprintf(pf, " %.6g %.6g %.6g\n", m[3][0], m[3][1], m[3][2]);
        }
    });
    std::fclose(pf);
}

// The records' local matrices and velocities of an --xfs file, on the same (ASEG instance, channel, object, record)
void ImportAsegaXfs3(const std::string& file)
{
    FILE* pf = std::fopen(file.c_str(), "r");
    if (!pf) return;
    std::map<std::string, std::vector<std::array<float, 15>>> mp;
    char sz[1024];
    while (std::fgets(sz, sizeof(sz), pf))
    {
        char szKey[64];
        int ichn, oid, ixf, mode, cch = 0;
        if (std::sscanf(sz, "xf %63s %d %d %d %d%n", szKey, &ichn, &oid, &ixf, &mode, &cch) != 5) continue;
        std::array<float, 15> ag{};
        const char* pch = sz + cch;
        for (float& g : ag) g = std::strtof(pch, const_cast<char**>(&pch));
        mp[std::string(szKey) + " " + std::to_string(ichn) + " " + std::to_string(oid) + " " + std::to_string(ixf)].push_back(ag);
    }
    std::fclose(pf);
    std::map<std::string, std::size_t> mpi;
    int cset = 0;
    ForEachAsegaByInstance3([&](ASEGA* p, int inst) {
        const ASEGD* pd = PasegdOf(p->paseg);
        for (CTRLASEG* pc = p->pctrlFirst; pc; pc = pc->pnextAsega)
        {
            if (!pc->palo || !pc->pxf) continue;
            int ichn = -1, ixf = -1;
            for (std::size_t i = 0; i < pd->achn.size(); ++i)
                if (pd->achn[i].ptrkp.get() == pc->ptrkp && pc->ptrkp) ichn = int(i);
            for (std::size_t i = 0; i < pc->palo->axf.size(); ++i)
                if (pc->palo->axf[i].get() == pc->pxf) ixf = int(i);
            const std::string k = std::to_string(p->paseg->oid) + ":" + std::to_string(inst) + " " + std::to_string(ichn) +
                                  " " + std::to_string(pc->palo->oid) + " " + std::to_string(ixf);
            auto it = mp.find(k);
            if (it == mp.end()) continue;
            std::size_t& i = mpi[k];
            if (i >= it->second.size()) continue;
            const std::array<float, 15>& ag = it->second[i++];
            glm::mat4& m = pc->pxf->mat;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) m[r][c] = ag[std::size_t(r * 3 + c)];
            m[3] = glm::vec4(ag[9], ag[10], ag[11], 1.0f);
            pc->pxf->vel = glm::vec4(ag[12], ag[13], ag[14], 0.0f);
            ++cset;
        }
    });
    std::printf("anim3: %d records took the values of %s\n", cset, file.c_str());
}

void UpdateWorldAnimation3(float)
{
    ANIM3& a = s_anim3;
    if (!a.fOn) return;
    g_psw = a.psw;
    g_clock = g_clock3;
    static const ANIMSTEP3 s_step = AnimStep3();
    static int s_cframeStep = 0;
    const bool fStep = s_step.frame >= 0 && s_cframeStep == s_step.frame;
    ++s_cframeStep;
    if (fStep) ImportAsegaTimes3(s_step.in);
    if (fStep && !s_step.xfin.empty()) ImportAsegaXfs3(s_step.xfin);
    UpdateAsegas();
    const std::vector<ALO*> apalo = ApaloAnimated3();
    for (ALO* palo : apalo) UpdateAloXfs(palo, g_clock.t, g_clock.dt);
    for (ALO* palo : apalo) UpdateAloXfWorlds(palo);
    CollectAsegas();
    if (fStep) PrintAsegaXfs3(s_step.out);
    int cposed = 0;
    for (ALO* palo : apalo)
    {
        auto it = a.mpaloaient.find(palo);
        if (it == a.mpaloaient.end()) continue;
        for (std::size_t k : it->second)
        {
            SCENEGLOBSET& sgs = a.pscene->aglobset[k];
            const std::vector<glm::mat4> amatOld = sgs.amatGlob;
            PoseEntry3(sgs, a.pst->aamatXf[k], palo, a.amatCorr[k]);
            if (std::getenv("SLY3_ANIMPRINT") && g_clock.t < 0.05f)
            {
                float d = 0.0f;
                for (std::size_t i = 0; i < amatOld.size() && i < sgs.amatGlob.size(); ++i)
                    for (int c = 0; c < 4; ++c) d = std::max(d, glm::length(amatOld[i][c] - sgs.amatGlob[i][c]));
                std::printf("anim3: entry %zu oid %d (%zu globs, %zu with bones) at %.0f %.0f %.0f moved %.3f\n", k, sgs.oid,
                            sgs.amatGlob.size(), std::count_if(sgs.aamatBone.begin(), sgs.aamatBone.end(),
                                                               [](const auto& v) { return !v.empty(); }),
                            sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z, d);
            }
        }
        cposed += int(it->second.size());
    }
    static int s_cframe = 0;
    // SLY3_ANIMLIST=FRAME:FILE: the playing ASEGAs at that frame ("asega ASEGOID ALOOID T SVT", ALO -1: the ASEG's
    // own objects), as tools/asegram3.py --asegas prints a RAM dump's
    if (const char* sz = std::getenv("SLY3_ANIMLIST"))
    {
        int f = -1;
        char szFile[512] = {};
        if (std::sscanf(sz, "%d:%511s", &f, szFile) == 2 && f == s_cframe)
            if (FILE* pf = std::fopen(szFile, "w"))
            {
                for (ASEGA* p : g_apasega)
                    if (!p->fRetracted)
                        std::fprintf(pf, "asega %d %d %.6f %.6f %d\n", p->paseg->oid, p->palo ? p->palo->oid : -1, p->t, p->svt, p->grfapl);
                std::fclose(pf);
            }
    }
    if (std::getenv("SLY3_ANIMPRINT") && (s_cframe++ % 300) == 0)
        std::printf("anim3: t %.2f: %d ASEGAs playing, %zu objects animated, %d scene entries posed\n", g_clock.t,
                    CasegaPlaying3(), apalo.size(), cposed);
    else if (!std::getenv("SLY3_ANIMPRINT")) ++s_cframe;
}

int CApplyNativePose3(SCENE& scene, const SCENE3STATS& st, SW* psw)
{
    int c = 0;
    for (std::size_t k = 0; k < scene.aglobset.size() && k < st.aamatXf.size(); ++k)
    {
        ALO* palo = k < s_anim3.apaloScene.size() ? s_anim3.apaloScene[k] : nullptr;
        if (!palo || s_anim3.psw != psw) continue;
        PoseEntry3(scene.aglobset[k], st.aamatXf[k], palo, s_anim3.amatCorr[k]);
        ++c;
    }
    return c;
}
}  // namespace sly3

namespace sly3
{
const ACTORANIM3* AactoranimBegin3(int* pc)
{
    *pc = int(sizeof(s_aactoranim3) / sizeof(s_aactoranim3[0]));
    return s_aactoranim3;
}

LO* PloActorAnimField3(ALO* pactor, uint32_t ib)
{
    if (!pactor) return nullptr;
    for (const ACTORANIM3& e : s_aactoranim3)
    {
        if (e.cidActor != pactor->Cid() || e.ib != ib || e.oid < 0) continue;
        SW* pswSave = g_psw;
        if (s_anim3.psw) g_psw = s_anim3.psw;
        LO* plo = PloFindSwNearest(pactor, e.oid);
        g_psw = pswSave;
        return plo && plo->Cid() == e.cid ? plo : nullptr;
    }
    return nullptr;
}
}  // namespace sly3
