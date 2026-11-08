// drawdist.cpp: the QoL draw distance option above 1x beyond the MRDs (drawdist.h). Visual only, never at 1x or below.
#include "drawdist.h"
#include "game/sly3.h"

#include "activate.h"
#include "alo.h"
#include "breakable.h"
#include "clock.h"
#include "cls.h"
#include "display.h"
#include "emitter.h"
#include "glob.h"
#include "lo.h"
#include "render.h"
#include "scene.h"
#include "sw.h"
#include "transition.h"
#include "worldaccess.h"
#include "xf.h"
#include "xfmath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>


namespace
{
bool FAlo(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30)); }
// (activate.cpp's: the current player's position, the reference of the activation kind 3)
glm::vec3 PosPlayer(const glm::vec3& posDefault) { return g_transition.ploPlayer ? PosLoWorld(g_transition.ploPlayer) : posDefault; }

bool FOff()
{
    static const bool s_f = [] { const char* sz = std::getenv("SLY2N_DDGHOST"); return sz && sz[0] == '0'; }();   // (debug)
    return s_f;
}

// The draw slot (19) of a class that draws (as FAloDrawn: not none, not LO's empty 0x1A5ED8).
bool FClassDraws(const LO* plo)
{
    const uint32_t pfn = plo->pcls ? plo->pcls->PfnOrig(19) : 0;
    return pfn != 0 && pfn != 0x1A5ED8;
}

// The streaming groups whose instances are drawn as ghosts: one object per instance, a prop (ALO 0x30, a breakable
// 0x62, a touch pad 0x53) or a clue bottle (0x5F, animated), no character and no skinned group in its tree (the
// extra draws place rigid groups only), something drawn.
struct WEINFO
{
    bool fGhost = false;
    bool fAnimated = false;   // the clue bottles: drawn by their ghosts also while an instance is placed but inactive
};
struct WORLDCACHE
{
    uint32_t serial = 0;
    const SW* psw = nullptr;
    std::vector<WEINFO> awe;
    std::unordered_set<const LO*> setAnimatedRow;   // the rows' objects of the animated groups
};
WORLDCACHE s_world;

bool FGhostableTree(const LO* plo, bool* pfGlobs)
{
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x33))) return false;
    if (FAlo(plo))
    {
        const ALO* palo = static_cast<const ALO*>(plo);
        if (palo->pglobset)
            for (const GLOB& g : palo->pglobset->aglob)
            {
                if (!g.abone.empty() || g.cbone > 0) return false;
                *pfGlobs = true;
            }
        for (const LO* pc : palo->aploChild)
            if (pc && !FGhostableTree(pc, pfGlobs)) return false;
    }
    return true;
}

const WORLDCACHE& World(const SW* psw)
{
    if (s_world.psw == psw && s_world.serial == psw->serial && s_world.awe.size() == psw->awe.size()) return s_world;
    s_world = WORLDCACHE{};
    s_world.psw = psw;
    s_world.serial = psw->serial;
    s_world.awe.resize(psw->awe.size());
    std::vector<bool> afStreamed(psw->awe.size(), false);
    for (const auto& [pproxy, ps] : psw->mpproxy)
        if (ps.iwe >= 0 && ps.iwe < static_cast<int>(afStreamed.size())) afStreamed[size_t(ps.iwe)] = true;
    for (size_t iwe = 0; iwe < psw->awe.size(); ++iwe)
    {
        const WE& we = psw->awe[iwe];
        if (!afStreamed[iwe] || we.cloLoaded != 1 || we.aplo.empty() || !we.aplo[0] || !FAlo(we.aplo[0])) continue;
        const CID cid = we.aplo[0]->Cid();
        if (cid != S3CID(0x30) && cid != S3CID(0x5F) && cid != S3CID(0x62) && cid != S3CID(0x53)) continue;
        bool fGlobs = false;
        if (!FGhostableTree(we.aplo[0], &fGlobs) || !fGlobs) continue;
        WEINFO& wi = s_world.awe[iwe];
        wi.fGhost = true;
        wi.fAnimated = cid == S3CID(0x5F);
        if (wi.fAnimated)
            for (int i = we.cUsed; i < std::min(we.cinst, static_cast<int>(we.aplo.size())); ++i)
                if (we.aplo[size_t(i)]) s_world.setAnimatedRow.insert(we.aplo[size_t(i)]);
    }
    return s_world;
}

// A proxy whose placement is its own matrix (no parent object, no reference record): the ones ghosts are drawn at
bool FStaticProxy(const LO* pproxy)
{
    return !pproxy->paloParent && !static_cast<const LOX*>(pproxy)->pxf;
}

glm::mat4 MatAlo(const ALO* palo)
{
    if (const XF* pxf = palo->apxfRef[2]) return pxf->matWorld;
    return palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
}

bool FIntact(LO* plo) { return plo->Cid() != S3CID(0x62) || FBreakableIntact(plo); }

// The group's instance drawn for its ghosts, and the placement its pose is relative to: a placed, active and drawn one
// (its live pose), else a placed one, else one in the pool (placed by the identity: the pool's rest pose).
struct TEMPLATE
{
    ALO* palo = nullptr;
    glm::mat4 matPlace{1.0f};
};
TEMPLATE TemplateOf(const SW* psw, int iwe)
{
    const WE& we = psw->awe[size_t(iwe)];
    TEMPLATE best;
    int nBest = 0;
    const int crow = std::min(we.cinst, static_cast<int>(we.aplo.size()));
    for (int i = we.cUsed; i < crow; ++i)
    {
        LO* plo = we.aplo[size_t(i)];
        if (!plo || !FAlo(plo) || !FClassDraws(plo) || !FIntact(plo)) continue;
        const LO* pOwner = i < static_cast<int>(we.aploRowOwner.size()) ? we.aploRowOwner[size_t(i)] : nullptr;
        int n = 1;
        glm::mat4 matPlace(1.0f);
        if (pOwner)
        {
            if (!FStaticProxy(pOwner) || plo->paloParent) continue;
            matPlace = static_cast<const LOX*>(pOwner)->mat;
            n = !plo->fInWorld ? 0 : FLoActive(plo) && FAloDrawn(psw, static_cast<ALO*>(plo)) ? 3 : 2;
        }
        else if (plo->fInWorld) continue;   // (in the world without a proxy: placed by something else)
        if (n > nBest)
        {
            nBest = n;
            best.palo = static_cast<ALO*>(plo);
            best.matPlace = matPlace;
        }
    }
    return best;
}

void AppendTree(const SCENE& scene, ALO* palo, bool fRoot, const glm::mat4& matRel, float sMRD, std::vector<EXTRADRAW>& aextra)
{
    if (!fRoot && (palo->fRemoved || !FClassDraws(palo))) return;
    if (palo->gFade == 0.0f) return;
    if (palo->pglobset)
    {
        const size_t c = aextra.size();
        AppendAloExtraDraws(scene, palo, matRel * MatAlo(palo), 1.0f, aextra);
        for (size_t i = c; i < aextra.size(); ++i) aextra[i].sMRD = sMRD;
    }
    for (LO* pc : palo->aploChild)
        if (pc && FAlo(pc) && (!palo->fInWorld || pc->fInWorld)) AppendTree(scene, static_cast<ALO*>(pc), false, matRel, sMRD, aextra);
}
}  // namespace

bool FDrawDistanceGhostedInstance(const LO* plo)
{
    if (!g_psw || FOff()) return false;
    return World(g_psw).setAnimatedRow.count(plo) != 0;
}

void CollectDrawDistanceGhosts(const SCENE& scene, std::vector<EXTRADRAW>& aextra)
{
    const float rScale = g_display.drawDistance;
    SW* psw = g_psw;
    if (!(rScale > 1.0f) || !psw || FOff()) return;
    const WORLDCACHE& w = World(psw);
    std::unordered_map<int, TEMPLATE> mptmpl;
    static const bool s_fPrint = std::getenv("SLY2N_DDGHOSTPRINT") != nullptr;   // (debug: the ghosts, once a second)
    static int s_nSecond = -1;
    const bool fPrint = s_fPrint && static_cast<int>(g_clock.t) != s_nSecond;
    if (fPrint) s_nSecond = static_cast<int>(g_clock.t);
    int cGhost = 0;
    for (const auto& [pproxy, ps] : psw->mpproxy)
    {
        if (ps.iwe < 0 || ps.iwe >= static_cast<int>(w.awe.size()) || !w.awe[size_t(ps.iwe)].fGhost) continue;
        const LO* pp = pproxy;
        // (a streaming proxy the activation would give an instance: in the world, kind 0 / 3, its option 1178 off)
        if (pp->fRemoved || !pp->fInWorld || !FStaticProxy(pp) || pp->fNodeActive || pp->U32Field(0, 0xE4) != 0) continue;
        if ((pp->st != 0 && pp->st != 3) || !(pp->sRadius < 1e9f) || !(g_actview.grfCm & pp->bMask)) continue;
        const glm::mat4& matProxy = static_cast<const LOX*>(pp)->mat;
        const glm::vec3 posRef = pp->st == 0 ? g_actview.posCm : PosPlayer(g_actview.posCm);
        if (!(glm::distance(glm::vec3(matProxy[3]), posRef) < pp->sRadius * rScale)) continue;
        // (an instance it has that the world draws, frozen or not, is drawn as it is)
        const WE& we = psw->awe[size_t(ps.iwe)];
        bool fDrawn = false;
        for (int irow : ps.airow)
            if (irow >= 0 && size_t(irow) < we.aplo.size() && we.aplo[size_t(irow)] && FAlo(we.aplo[size_t(irow)]) &&
                FAloDrawn(psw, static_cast<ALO*>(we.aplo[size_t(irow)])))
                fDrawn = true;
        if (fDrawn) continue;
        auto it = mptmpl.find(ps.iwe);
        if (it == mptmpl.end()) it = mptmpl.emplace(ps.iwe, TemplateOf(psw, ps.iwe)).first;
        const TEMPLATE& t = it->second;
        if (!t.palo) continue;
        glm::mat4 matGhost = matProxy;
        // (the clue bottles share one live pose: each turned about its own up axis by its proxy, so they do not spin
        // in step)
        if (w.awe[size_t(ps.iwe)].fAnimated)
            matGhost = glm::rotate(matGhost, float(uint32_t(pp->oid) * 2654435761u >> 8) * (6.2831853f / 16777216.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        AppendTree(scene, t.palo, true, matGhost * InverseAffineMatrix4(t.matPlace), pp->sRadius, aextra);
        ++cGhost;
        if (fPrint)
            std::printf("ddghost t %.2f proxy %d group %s at %.0f %.0f %.0f r %.0f template %d%s\n", g_clock.t, pp->oid, we.StrName().c_str(), matProxy[3].x,
                        matProxy[3].y, matProxy[3].z, pp->sRadius, t.palo->oid, t.palo->fInWorld ? " (placed)" : " (pool)");
    }
    if (fPrint) std::printf("ddghost t %.2f: %d ghosts\n", g_clock.t, cGhost);
}

namespace
{
struct SAVEDXF
{
    XF* pxf;
    glm::mat4 mat, matWorld;
    uint64_t genWorld;
};
std::vector<SAVEDXF> s_asaved;
std::unordered_map<const LO*, float> s_mptFirst;   // the inactive drawn trees: since when (their poses' time)

void ForEachAlo(ALO* palo, const std::function<void(ALO*)>& fn)
{
    fn(palo);
    for (LO* pc : palo->aploChild)
        if (pc && FAlo(pc)) ForEachAlo(static_cast<ALO*>(pc), fn);
}
}  // namespace

void BeginDrawDistancePoses(const SW* psw, const std::vector<ALO*>& apalo)
{
    s_asaved.clear();
    if (!(g_display.drawDistance > 1.0f) || !psw || FOff())
    {
        s_mptFirst.clear();
        return;
    }
    std::unordered_set<const LO*> setSeen, setVisual;
    std::vector<std::pair<XF*, glm::mat4>> apose;
    for (ALO* p : apalo)
    {
        if (!p) continue;
        LO* proot = p;
        while (proot->paloParent) proot = proot->paloParent;
        if (!setSeen.insert(proot).second || !FAlo(proot) || !FDrawDistanceVisualTree(proot)) continue;
        setVisual.insert(proot);
        const float dt = g_clock.t - s_mptFirst.try_emplace(proot, g_clock.t).first->second;
        apose.clear();
        ForEachAlo(static_cast<ALO*>(proot), [&](ALO* pa) {
            for (auto& up : pa->axf)
            {
                glm::mat4 m;
                if (FXfVisualPose(up.get(), dt, &m)) apose.emplace_back(up.get(), m);
            }
        });
        if (apose.empty()) continue;
        ForEachAlo(static_cast<ALO*>(proot), [&](ALO* pa) {
            for (auto& up : pa->axf) s_asaved.push_back({up.get(), up->mat, up->matWorld, up->genWorld});
        });
        for (auto& [pxf, m] : apose) pxf->mat = m;
        UpdateAloHierarchyWorlds(static_cast<ALO*>(proot));
        static const bool s_fPrint = std::getenv("SLY2N_DDGHOSTPRINT") != nullptr;   // (debug: the posed trees, once a second)
        static int s_nSecond = -1;
        if (s_fPrint && static_cast<int>(g_clock.t * 4.0f) != s_nSecond)
        {
            s_nSecond = static_cast<int>(g_clock.t * 4.0f);
            { const glm::vec3 p = PosLoWorld(proot); std::printf("ddpose t %.2f root %d cid 0x%X at %.0f %.0f %.0f: %zu records posed, dt %.2f\n", g_clock.t, proot->oid, proot->Cid(), p.x, p.y, p.z, apose.size(), dt); }
        }
    }
    for (auto it = s_mptFirst.begin(); it != s_mptFirst.end();)
        it = setVisual.count(it->first) ? std::next(it) : s_mptFirst.erase(it);
}

void EndDrawDistancePoses()
{
    for (auto it = s_asaved.rbegin(); it != s_asaved.rend(); ++it)
    {
        it->pxf->mat = it->mat;
        it->pxf->matWorld = it->matWorld;
        it->pxf->genWorld = it->genWorld;
    }
    s_asaved.clear();
}
