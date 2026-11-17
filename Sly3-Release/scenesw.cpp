#include "fastpaths.h"
#include "scenesw.h"
#include "game/sly3.h"
#include "debugphys.h"
#include "simdmat.h"
#include "passhelpers.h"
#include "drawdist.h"
#include "display.h"
#include "pccharacter.h"
#include "rope.h"

#include "activate.h"
#include "actor.h"
#include "alo.h"
#include "bone.h"
#include "booty.h"
#include "brx.h"
#include "cm.h"
#include "clock.h"
#include "step.h"
#include "scene.h"
#include "worldaccess.h"
#include "sw.h"
#include "trail.h"
#include "splitscreen3.h"
#include "vehiclehooks.h"
#include "cplcy.h"
#include "spliceglue.h"
#include <cmath>
#include <unordered_map>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

namespace
{
// An entry's placement in parts: PlaceGlobsetPrepare and PlaceGlobsetFinish on the game thread (what writes shared
// state or GL: the owner records' first lookup, the draw distance's tree cache, the trails' vertex uploads, the ropes'
// bent vertices, the debug prints); PlaceGlobsetRange (a run of groups) and PlaceGlobsetEntry (the entry's own part,
// with its first run) anywhere: they read the world and write only the entry, so UpdateSceneFromSw runs them on the
// pass helpers (passhelpers.h), a big entry's groups in several runs. Prepare sizes the entry's arrays and asks for
// the carried booty's draw delta, which every run applies to its groups.
struct PLACEDELTA
{
    bool f = false;
    glm::mat4 dmat{1.0f};
};
// orig3: 0x232710 (class 0x7E's draw, vt slot 21) / 0x252508 (STEPGUARD 0x3A's): seen by a camera whose policy is of
// kind 2 (the follow camera) or 13 (the swimmer's first-person one), the object's own groups are drawn scaled by
// clamp(clq(distance to the camera)) (SW +0x4E00 c / l / q, option 988; +0x4E10 min / max, 989; the world start's
// 0x25AC20: 0.7 / 0.0002 / 0 and 1 / 1.5: far ones a little larger), about its origin (0x7E) or, for the guards,
// x / y by lo and z by hi about their base in their frame, lo / hi pulsing for 0.85 s after a flinch (+0x380: sin(80 t)
// between -0.08 and 0.05, faded by clamp(1 - 2u + u^2), u = t / 0.85). Their children draw as they are.
namespace
{
std::unordered_map<const LO*, float> s_mptFlinch3;   // (STEPGUARD +0x380)
bool FFalloffScaleDelta3(const ALO* palo, glm::mat4* pdmat)
{
    if (!palo || palo->axf.empty() || g_cm.ccpr <= 0 || !g_cm.acpr[0].pcplcy) return false;
    const int cid = palo->Cid();
    const bool fGuard = FCidDerivedFrom(cid, 0x3A), fCreature = cid == 0x7E;
    if (!fGuard && !fCreature) return false;
    const CPLCY* pcp = g_cm.acpr[0].pcplcy;
    if (pcp != &g_cmpolicies.cpfollow && pcp->VtOrig() != 0x367990) return false;
    static const bool s_fOff = std::getenv("SLY3_NOFALLOFFSCALE") != nullptr;
    if (s_fOff) return false;
    const glm::mat4& w = palo->axf[0]->matWorld;
    const float d = glm::distance(glm::vec3(w[3]), g_cm.pos);
    const float c = GSwOptionField3(988, 0, 0.7f), l = GSwOptionField3(988, 1, 0.0002f), q = GSwOptionField3(988, 2, 0.0f);
    const float gMin = GSwOptionField3(989, 0, 1.0f), gMax = GSwOptionField3(989, 1, 1.5f);
    const float g0 = c + d * (l + d * q);
    const float s = g0 < gMin ? gMin : (gMax < g0 ? gMax : g0);
    float lo = s, hi = s;
    glm::vec3 pivot(0.0f);
    if (fGuard)
    {
        if (const STEP* pstep = PstepFromLo(const_cast<ALO*>(palo))) pivot = glm::vec3(pstep->vecBase);
        auto it = s_mptFlinch3.find(palo);
        const float t = it == s_mptFlinch3.end() ? 1e9f : g_clock.t - it->second;
        if (t >= 0.0f && t < 0.85f)
        {
            const float sn = (std::sin(t * 80.0f) + 1.0f) * 0.5f;
            const float u = t * 1.17647052f;
            const float env = std::clamp(1.0f + u * (-2.0f + u), 0.0f, 1.0f);
            const float wob = ((1.0f - sn) * -0.08f + sn * 0.05f) * env;
            lo = s - wob;
            hi = s + wob;
        }
    }
    if (lo == 1.0f && hi == 1.0f) return false;
    glm::mat4 m(1.0f);
    m[0][0] = lo;
    m[1][1] = lo;
    m[2][2] = hi;
    m[3] = glm::vec4((1.0f - lo) * pivot.x, (1.0f - lo) * pivot.y, (1.0f - hi) * pivot.z, 1.0f);
    *pdmat = w * m * glm::inverse(w);
    return true;
}
}  // namespace

PLACEDELTA PlaceGlobsetPrepare(ALO* palo, SCENEGLOBSET& sgs)
{
    const GLOBSET& gs = *sgs.pglobset;
    // (QoL draw distance above 1x: a tree with a character appears at the original's activation radius (activate.cpp
    // NDrawDistanceRoot), so it fades in there as the original's, not at the radius x the option)
    sgs.fMrdOriginal = g_display.drawDistance > 1.0f && [&] {
        const LO* proot = palo;
        while (proot->paloParent) proot = proot->paloParent;
        return FDrawDistanceActorTree(proot);
    }();
    // the groups' owner records (the S16 through vt slot 51; the object's records do not change in a world)
    if (!sgs.fOwners)
    {
        sgs.apxfOwner.resize(gs.aglob.size());
        for (size_t i = 0; i < gs.aglob.size(); ++i)
            sgs.apxfOwner[i] = gs.aglob[i].oidOwner >= 0 ? PxfFindAlo(palo, gs.aglob[i].oidOwner) : nullptr;
        sgs.fOwners = true;
    }
    // (the arrays the runs fill, sized before any runs)
    sgs.amatGlob.resize(gs.aglob.size(), glm::mat4(1.0f));
    sgs.afVisible.resize(gs.aglob.size(), 1);
#if SLY2_FASTPATHS
    sgs.fVisibleCleared = false;   // (the placement after this sets the groups' flags)
#endif
    sgs.aamatBone.resize(gs.aglob.size());
    // orig: 0x13C000 -> 0x1A5EE8: a carried booty's scale affects drawing only. Bone matrices already transform
    // vertices to world space (bone.cpp 0x1684D0), replacing the group matrix in the shader, so both need the delta.
    PLACEDELTA delta;
    sgs.sDrawScale = 1.0f;
    const bool fCharScale = FDebugCharScaleDelta(palo, &delta.dmat, &sgs.sDrawScale);   // (the debug menu's scale: debugphys.h)
    delta.f = fCharScale || FBootyDrawDelta(palo, &delta.dmat) || FFalloffScaleDelta3(palo, &delta.dmat);
    return delta;
}

// groups [iglobFirst, iglobLim) of the entry: shown, placed, their bones (and the booty's delta on both)
void PlaceGlobsetRange(ALO* palo, SCENEGLOBSET& sgs, const PLACEDELTA& delta, size_t iglobFirst, size_t iglobLim)
{
    const GLOBSET& gs = *sgs.pglobset;
    const glm::mat4 matAlo = palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    static const char* s_szShow = std::getenv("SLY2_FORCESHOW");   // debugging: these oids' groups all shown
    const bool fForceShow = s_szShow && std::strstr(s_szShow, (std::string(",") + std::to_string(palo->oid) + ",").c_str());
    for (size_t i = iglobFirst; i < iglobLim; ++i)
    {
#if SLY2_FASTPATHS
        // (the Switch: the group four ahead fetched, the lines of it both loops read, and its owner record's matrix: the
        // groups are large and only these parts of them are read)
        if (i + 4 < iglobLim)
        {
            const GLOB& g = gs.aglob[i + 4];
            __builtin_prefetch(&g);
            __builtin_prefetch(&g.abone);
            __builtin_prefetch(&g.iglobTemplate);
            if (const XF* p = sgs.apxfOwner[i + 4]) __builtin_prefetch(&p->matWorld);
        }
#endif
        const GLOB& glob = gs.aglob[i];
        sgs.afVisible[i] = fForceShow || FAloGlobShown(palo, glob.n0, !(glob.grfGroup & 0x800)) ? 1 : 0;  // (alo.h: the run time's flags)
        // the group's owner record, else the object's own placement
        const XF* pxf = sgs.apxfOwner[i];
        glm::mat4 m = pxf ? pxf->matWorld : matAlo;
        if (glob.fMatrix) m = MulMat4(m, glob.dmat);  // an instance's own matrix (simdmat.h: glm's product exactly)
        sgs.amatGlob[i] = delta.f ? MulMat4(delta.dmat, m) : m;
    }
    // skinned groups: their bone matrices (animation code, bone.cpp; empty before InitWorldAnimation binds them)
    static const bool s_fNoSkin = std::getenv("SLY2_NOSKIN") != nullptr;   // debugging: bind pose
    if (s_fNoSkin) return;
    const auto* paabind = PaabindOfAlo(palo);   // (looked up once a run)
    for (size_t i = iglobFirst; i < iglobLim; ++i)
    {
        const GLOB& glob = gs.aglob[i];
        const GLOB& geom = glob.iglobTemplate >= 0 && glob.abone.empty() ? gs.aglob[size_t(glob.iglobTemplate)] : glob;
        std::vector<glm::mat4>& amat = sgs.aamatBone[i];
        if (geom.abone.empty() || !FGlobBoneMatricesOf(paabind, static_cast<int>(&geom - gs.aglob.data()), &amat))
            amat.clear();
        else if (delta.f)
            for (glm::mat4& mat : amat) mat = MulMat4(delta.dmat, mat);
    }
}

// the entry's own part (with its first run): its bounds, its alphas
void PlaceGlobsetEntry(ALO* palo, SCENEGLOBSET& sgs, const PLACEDELTA& delta)
{
    const GLOBSET& gs = *sgs.pglobset;
    sgs.oid = palo->oid;
    // orig: 0x1686AC: the whole sphere uses ALO +0x60 (the object record).
    // Actors move this record independently of record zero; group placement below is separate.
    const glm::mat4 matBounds = palo->apxfRef[2] ? palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
    sgs.fBounds = true;
    sgs.posBounds = glm::vec3(matBounds * glm::vec4(gs.posCenter, 1.0f));
    sgs.sBoundsRadius = gs.sRadius;
    sgs.sMRD = palo->sRadius;
    if (delta.f)
    {
        const glm::mat4& dmat = delta.dmat;
        sgs.posBounds = glm::vec3(dmat * glm::vec4(sgs.posBounds, 1.0f));
        sgs.sBoundsRadius *= std::sqrt(std::max({glm::dot(glm::vec3(dmat[0]), glm::vec3(dmat[0])),
                                                glm::dot(glm::vec3(dmat[1]), glm::vec3(dmat[1])),
                                                glm::dot(glm::vec3(dmat[2]), glm::vec3(dmat[2]))}));
    }
    // the draw alphas (0x1F3C10: gFade, gFade; 0x1425B0: gFade x +0x10D4, gFade x +0x10D8)
    sgs.gAlpha = sgs.gAlpha2 = palo->gFade;
    if (FIsLoDerivedFrom(palo, S3CID(0x34)))   // (PactorFromLo would make a missing actor: only an existing one)
        if (const ACTOR* pa = dynamic_cast<const ACTOR*>(PstepFromLo(palo)))
        {
            sgs.gAlpha *= pa->g10D4;
            sgs.gAlpha2 *= pa->g10D8;
            sgs.gFlash = pa->dmg.gFlash;
            static const char* s_szFlash = std::getenv("SLY2N_FLASHTEST");   // debugging: every actor's damage flash
            if (s_szFlash) sgs.gFlash = float(std::atof(s_szFlash));
            static const char* s_test = std::getenv("SLY2_FADETEST");   // debugging: every actor drawn at this alpha
            if (s_test) sgs.gAlpha = sgs.gAlpha2 = float(std::atof(s_test));
        }
    static const bool s_fDbg = std::getenv("SLY2_ALPHADBG") != nullptr;   // debugging: the faded objects
    if (s_fDbg && (sgs.gAlpha != 1.0f || sgs.gAlpha2 != 1.0f))
        std::printf("alpha oid %d cid %d: %.3f %.3f\n", palo->oid, int(palo->Cid()), sgs.gAlpha, sgs.gAlpha2);
    sgs.fTransform = true;
}

void PlaceGlobsetFinish(ALO* palo, SCENEGLOBSET& sgs)
{
    const GLOBSET& gs = *sgs.pglobset;
    const glm::mat4 matAlo = palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    PlaceTrailScene(palo, sgs);
    PlaceRopeMeshScene(palo, sgs);
    // debug: SLY2N_SCENELIST=t: every object placed in the scene in the frame at clock t: its bounds' screen point
    if (static const char* s_szList = std::getenv("SLY2N_SCENELIST"); s_szList)
        if (const float t = float(std::atof(s_szList)); g_clock.t >= t && g_clock.t < t + 0.0166f)
        {
            int cVis = 0;
            for (size_t i = 0; i < sgs.afVisible.size(); ++i) cVis += sgs.afVisible[i];
            const glm::vec3 scr = ConvertCmWorldToScreen(&g_cm, sgs.posBounds);
            std::printf("scenelist %d cid 0x%X parent %d vis %d/%zu at %.0f %.0f %.0f r %.0f screen %.3f %.3f %.0f alpha %.2f\n", palo->oid, palo->Cid(),
                        palo->paloParent ? palo->paloParent->oid : -1, cVis, sgs.afVisible.size(), sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z,
                        sgs.sBoundsRadius, scr.x, scr.y, scr.z, sgs.gAlpha);
        }
    // debug: SLY2N_SCENEOID=oid: that object's scene entries (each copy): its groups' places, shown flags, first bones
    static const char* s_szOid = std::getenv("SLY2N_SCENEOID");
    if (s_szOid && palo->oid == std::atoi(s_szOid))
    {
        const glm::vec3 pos = matAlo[3];
        std::printf("sceneoid %d %p clone %d in %d at %.0f %.0f %.0f fade %.6f rate %.6f:", palo->oid, (void*)palo, palo->ploCloneOf != nullptr, palo->fInWorld, pos.x, pos.y, pos.z, palo->gFade, palo->dgFade);
        for (size_t i = 0; i < gs.aglob.size() && i < 6; ++i)
        {
            const glm::vec3 p = sgs.amatGlob[i][3];
            const glm::vec3 b = sgs.aamatBone[i].empty() ? glm::vec3(0.0f) : glm::vec3(sgs.aamatBone[i][0][3]);
            std::printf(" [%d %.0f %.0f %.0f bone %.0f %.0f %.0f]", sgs.afVisible[i], p.x, p.y, p.z, b.x, b.y, b.z);
        }
        int cVis = 0, cBone = 0;
        for (size_t i = 0; i < gs.aglob.size(); ++i) { cVis += sgs.afVisible[i]; cBone += !sgs.aamatBone[i].empty(); }
        std::printf(" | %zu groups, %d shown, %d skinned:", gs.aglob.size(), cVis, cBone);
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            std::printf(" %d%s%s", gs.aglob[i].n0, sgs.afVisible[i] ? "+" : "", sgs.aamatBone[i].empty() ? "" : "s");
            if (std::getenv("SLY2N_SCENEOIDSHD"))
                for (const SUBGLOB& sg : gs.aglob[i].asubglob)
                    if (sg.pshd)
                        std::printf("<rp %d mesh 0x%X shd %d k%d grf 0x%X rgba %08X %08X %08X>", gs.aglob[i].rp, gs.aglob[i].grfMesh, sg.pshd->id,
                                    sg.pshd->shdk, sg.pshd->grfshd, sg.pshd->argba[0], sg.pshd->argba[1], sg.pshd->argba[2]);
            if (!sgs.aamatBone[i].empty())
            {
                const glm::mat4& m = sgs.aamatBone[i][0];
                std::printf("(%zu bones, b0 %.0f %.0f %.0f scale %.2f)", sgs.aamatBone[i].size(), m[3][0], m[3][1], m[3][2], glm::length(glm::vec3(m[0])));
            }
        }
        std::printf("\n");
    }
}

void PlaceGlobset(ALO* palo, SCENEGLOBSET& sgs)
{
    const PLACEDELTA delta = PlaceGlobsetPrepare(palo, sgs);
    PlaceGlobsetRange(palo, sgs, delta, 0, sgs.pglobset->aglob.size());
    PlaceGlobsetEntry(palo, sgs, delta);
    PlaceGlobsetFinish(palo, sgs);
}

// a run of an entry's groups for the helpers: at most CGLOB_PLACE groups, the entry's own part with its first
struct PLACEITEM
{
    uint32_t ientry, iglobFirst, iglobLim;
};
constexpr uint32_t CGLOB_PLACE = 64;

// FAloDrawn's answers of UpdateSceneFromSw's tests, per entry (1 drawn, 0 not, -1 not asked), for the frame's
// ApplyDrawPass right after (game.cpp: nothing between them changes what the test reads), so each entry is asked once
// a frame instead of twice. Valid for the same world, entries and camera mask until ApplyDrawPass takes them.
struct DRAWNCACHE
{
    const SW* psw = nullptr;
    const std::vector<ALO*>* papalo = nullptr;
    uint32_t grfCm = 0;
    bool fValid = false;
    std::vector<int8_t> an;
} s_drawn;
}  // namespace

void PlaceSceneEntry(ALO* palo, SCENEGLOBSET& sgs) { PlaceGlobset(palo, sgs); }

void UpdateSceneFromSw(SCENE& scene, const std::vector<ALO*>& apalo, const SW* pswDrawn)
{
    // (QoL draw distance above 1x: the inactive trees the option draws placed with their spinning / animated parts
    // posed, the records restored after the placement; drawdist.h)
    struct POSES { bool f; ~POSES() { if (f) EndDrawDistancePoses(); } } poses{pswDrawn != nullptr};
    if (pswDrawn) BeginDrawDistancePoses(pswDrawn, apalo);
    const size_t c = std::min(scene.aglobset.size(), apalo.size());
    // which entries are placed and their preparation, in order (the vehicles' wheels posed before any placement reads
    // them); then their runs of groups and own parts on the pass helpers; then their ends, in order
    static std::vector<uint8_t> s_afPlace;
    static std::vector<PLACEDELTA> s_adelta;
    static std::vector<PLACEITEM> s_aitem;
    s_afPlace.assign(c, 0);
    s_adelta.resize(c);
    s_aitem.clear();
    s_drawn.psw = pswDrawn;
    s_drawn.papalo = &apalo;
    s_drawn.grfCm = g_actview.grfCm;
    s_drawn.fValid = pswDrawn != nullptr;
    s_drawn.an.assign(c, -1);
#if SLY2_FASTPATHS
    // (the Switch: the entries' draw tests (FAloDrawn) done first, on the pass helpers, at the draw distance option's 1x:
    // there the test only reads the objects and the world (NDrawDistanceRoot's tree cache serves the other distances),
    // and nothing the loop below does before an entry's test (the vehicles' wheels, the placements' preparation) changes
    // what it reads; the loop takes each answer where it asked for it)
    static std::vector<uint8_t> s_afDrawnAhead;
    const bool fDrawnAhead = pswDrawn && g_display.drawDistance == 1.0f;
    if (fDrawnAhead)
    {
        s_afDrawnAhead.resize(c);
        PassHelpers().ForBlocks(c, 64, [&](size_t iFirst, size_t iLim) {
            for (size_t i = iFirst; i < iLim; ++i)
                s_afDrawnAhead[i] = scene.aglobset[i].fTransform && FAloDrawn(pswDrawn, apalo[i]) ? 1 : 0;
        });
    }
#endif
    for (size_t i = 0; i < c; ++i)
    {
#if SLY2_FASTPATHS
        // (the Switch: the parts of the object four entries ahead that the tests read, and its glob set, prefetched:
        // the objects are scattered over the heap)
        if (i + 4 < c)
        {
            const ALO* p = apalo[i + 4];
            __builtin_prefetch(&p->pcls);
            __builtin_prefetch(&p->iupd);
            __builtin_prefetch(&p->gFade);
            __builtin_prefetch(scene.aglobset[i + 4].pglobset.get());
        }
#endif
        SCENEGLOBSET& sgs = scene.aglobset[i];
        // (with the world: what it will not draw this frame keeps its last placement, hidden: ApplyDrawPass's test
        // done first; the original places only what it draws)
        static const char* s_szForce = std::getenv("SLY2_FORCEDRAW");
        const bool fForce = s_szForce && std::strstr(s_szForce, (std::string(",") + std::to_string(apalo[i]->oid) + ",").c_str());
        const int nPc = NPcCharDrawOverride(apalo[i]);   // (PC addition, debug menu only: pccharacter.h)
#if SLY2_FASTPATHS
        const bool fNotDrawn = nPc < 0 && pswDrawn && sgs.fTransform && !fForce &&
                               !(s_drawn.an[i] = (fDrawnAhead ? s_afDrawnAhead[i] != 0 : FAloDrawn(pswDrawn, apalo[i])) ? 1 : 0);
#else
        const bool fNotDrawn = nPc < 0 && pswDrawn && sgs.fTransform && !fForce && !(s_drawn.an[i] = FAloDrawn(pswDrawn, apalo[i]) ? 1 : 0);
#endif
        if (nPc == 0 || fNotDrawn)
        {
#if SLY2_FASTPATHS
            if (!sgs.fVisibleCleared)
            {
                std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), 0);
                sgs.fVisibleCleared = true;
            }
#else
            std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), 0);
#endif
            continue;
        }
        DrawVehiclePose(apalo[i]); // orig0x1D41F8, wheel transforms before base placement
        s_adelta[i] = PlaceGlobsetPrepare(apalo[i], sgs);
        s_afPlace[i] = 1;
        const uint32_t cglob = uint32_t(sgs.pglobset->aglob.size());
        uint32_t iglob = 0;
        do
        {
            s_aitem.push_back({uint32_t(i), iglob, std::min(cglob, iglob + CGLOB_PLACE)});
            iglob += CGLOB_PLACE;
        } while (iglob < cglob);
    }
    // (blocks of 4 runs: an entry's part, a few hundred groups' placements and bones, per hand-out)
    PassHelpers().ForBlocks(s_aitem.size(), 4, [&](size_t iFirst, size_t iLim) {
        for (size_t k = iFirst; k < iLim; ++k)
        {
            const PLACEITEM& item = s_aitem[k];
            SCENEGLOBSET& sgs = scene.aglobset[item.ientry];
            PlaceGlobsetRange(apalo[item.ientry], sgs, s_adelta[item.ientry], item.iglobFirst, item.iglobLim);
            if (item.iglobFirst == 0) PlaceGlobsetEntry(apalo[item.ientry], sgs, s_adelta[item.ientry]);
        }
    });
    for (size_t i = 0; i < c; ++i)
        if (s_afPlace[i]) PlaceGlobsetFinish(apalo[i], scene.aglobset[i]);
}

void InvalidateSceneDrawnTests() { s_drawn.fValid = false; }

void ApplyDrawPass(const SW* psw, SCENE& scene, const std::vector<ALO*>& apalo)
{
    static const char* s_szForce = std::getenv("SLY2_FORCEDRAW");   // debugging: these oids drawn regardless
    // (the tests' answers when they are this frame's for this world, entries and camera mask)
    const bool fCache = s_drawn.fValid && s_drawn.psw == psw && s_drawn.papalo == &apalo && s_drawn.grfCm == g_actview.grfCm;
    s_drawn.fValid = false;
    // (debug: SLY3_DRAWNEAR=R,T: once, at clock T, the drawn objects within R of the camera's object)
    static const char* s_szNear = std::getenv("SLY3_DRAWNEAR");
    static bool s_fNearDone = false;
    LO* const ploNear = s_szNear && !s_fNearDone ? split3::PloPlayer(const_cast<SW*>(psw), 0) : nullptr;
    const bool fNear = ploNear && FCidDerivedFrom(ploNear->Cid(), S3CID(0x30)) && g_clock.t >= std::strtof(std::strchr(s_szNear, ',') ? std::strchr(s_szNear, ',') + 1 : "0", nullptr);
    if (fNear) { s_fNearDone = true; std::printf("drawnear: player %d\n", ploNear->oid); }
    for (size_t i = 0; i < scene.aglobset.size() && i < apalo.size(); ++i)
    {
        if (s_szForce && std::strstr(s_szForce, (std::string(",") + std::to_string(apalo[i]->oid) + ",").c_str())) continue;
        const int nPc = NPcCharDrawOverride(apalo[i]);   // (PC addition, debug menu only: pccharacter.h)
        if (nPc == 1) continue;
        const int8_t nCached = fCache && i < s_drawn.an.size() ? s_drawn.an[i] : int8_t(-1);
        const bool fDrawn = !(nPc == 0 || !(nCached >= 0 ? nCached != 0 : FAloDrawn(psw, apalo[i])));
        if (fNear && fDrawn && !apalo[i]->axf.empty())
        {
            const ALO* pa = apalo[i];
            const glm::vec3 pos = PosLoWorld(pa);
            const ALO* pc = static_cast<const ALO*>(ploNear);
            const glm::vec3 posc = PosLoWorld(pc);
            if (glm::length(pos - posc) < std::strtof(s_szNear, nullptr))
                std::printf("drawnear %d cid 0x%X parent %d mask 0x%X fade %.2f at %.0f %.0f %.0f (cur %d)\n", pa->oid, int(pa->Cid()),
                            pa->paloParent ? pa->paloParent->oid : -1, pa->bMask, pa->gFade, pos.x, pos.y, pos.z, pc->oid);
        }
        if (!fDrawn)
        {
#if SLY2_FASTPATHS
            if (SCENEGLOBSET& sgs = scene.aglobset[i]; !sgs.fVisibleCleared)
            {
                std::fill(sgs.afVisible.begin(), sgs.afVisible.end(), 0);
                sgs.fVisibleCleared = true;
            }
#else
            std::fill(scene.aglobset[i].afVisible.begin(), scene.aglobset[i].afVisible.end(), 0);
#endif
            static const bool s_fDbg = std::getenv("SLY2_DRAWDBG") != nullptr;   // debugging: the objects not drawn
            if (s_fDbg)
            {
                static bool s_f = false;
                if (!s_f) { s_f = true; std::printf("draw pass: camera mask %u, current object %d\n", g_actview.grfCm, g_cm.ploCur ? g_cm.ploCur->oid : -1); }
                const ALO* pa = apalo[i];
                const glm::vec3 pos = pa->axf.empty() ? glm::vec3(0.0f) : glm::vec3(pa->axf[0]->matWorld[3]);
                const uint32_t grf = pa->iupd >= 0 ? psw->aupd[size_t(pa->iupd)].grf : 0xFFFFFFFFu;
                std::printf("not drawn %d cid 0x%X iupd %d grf 0x%X draw 0x%X mask 0x%X fade %.2f inworld %d at %.0f %.0f %.0f\n", pa->oid, int(pa->Cid()), pa->iupd, grf,
                            pa->pcls ? pa->pcls->PfnOrig(19) : 0, pa->bMask, pa->gFade, pa->fInWorld, pos.x, pos.y, pos.z);
            }
        }
    }
}

// Sly 3's ALO-derived classes are 0x2F..0x81 (game/gen/classes.inc; Sly 2's 0x30..0x6C): SKY 0x6E, ... were left out
constexpr int kCidAloFirst3 = 0x2F, kCidAloEnd3 = 0x82;

int CBuildSceneFromSw(SW* psw, SCENE& scene, std::vector<ALO*>* papalo, const std::unordered_set<const ALO*>* psetHave)
{
    int c = 0;
    std::vector<ALO*> apaloPc;   // (PC addition, debug menu only: a guest character's objects last, pccharacter.h)
    AppendPcCharSceneAlos(apaloPc);
    const std::vector<LO*> aploPc(apaloPc.begin(), apaloPc.end());
    for (int cid = kCidAloFirst3; cid <= kCidAloEnd3; ++cid)
        for (LO* plo : *(cid < kCidAloEnd3 ? &psw->mpcidaplo[cid] : &aploPc))
        {
            ALO* palo = static_cast<ALO*>(plo);
            try
            {
                PostLoadTrail(palo);  // runtime clones own their output mesh before scene construction
                PostLoadRopeMesh(palo);
            }
            catch (const std::exception& e)   // (Sly 3: a copied Sly 2 subsystem that rejects the object's data; noted once per class)
            {
                static std::unordered_set<int> s_setCid;
                if (s_setCid.insert(palo->Cid()).second) std::fprintf(stderr, "sly3: scene: oid %d (cid 0x%X): %s\n", palo->oid, palo->Cid(), e.what());
                continue;
            }
            if (!palo->pglobset || palo->pglobset->aglob.empty()) continue;
            if (psetHave && psetHave->count(palo)) continue;
            SCENEGLOBSET sgs;
            // A glob set shared by clones is shared by their placements too, unless the lighting bakes into its
            // vertices per placement (lit or unbaked groups): then each placement gets its own copy.
            bool fLit = false;
            for (const GLOB& g : palo->pglobset->aglob)
                fLit = fLit || (g.grfMesh & 1) || std::any_of(g.avtx.begin(), g.avtx.end(), [](const VTXGLOB& v) { return v.fUnbaked != 0; });
            sgs.pglobset = fLit && palo->Cid() != S3CID(0x69) && palo->pglobset.use_count() > 1 ? std::make_shared<GLOBSET>(*palo->pglobset) : palo->pglobset;
            PlaceGlobset(palo, sgs);
            scene.aglobset.push_back(std::move(sgs));
            if (papalo) papalo->push_back(palo);
            ++c;
        }
    return c;
}

// The objects made after the scene's build (the spawners' clones, PloCloneLo at run time: a guard spawned from a
// placed one and its parts): appended to the scene (no new lighting: their glob sets' as loaded). Scans only when
// the world's class lists grew. Returns how many were added (then BuildSceneGpu builds their geometry).
int CAddNewAlosToScene(SW* psw, SCENE& scene, std::vector<ALO*>* papalo)
{
    static const SW* s_psw = nullptr;
    static size_t s_c = 0;
    size_t c = 0;
    for (int cid = kCidAloFirst3; cid < kCidAloEnd3; ++cid) c += psw->mpcidaplo[cid].size();
    if (s_psw == psw && s_c == c) return 0;
    s_psw = psw;
    s_c = c;
    const std::unordered_set<const ALO*> set(papalo->begin(), papalo->end());
    const size_t c0 = papalo->size();
    const int cAdd = CBuildSceneFromSw(psw, scene, papalo, &set);
    if (cAdd > 0 && std::getenv("SLY2N_SPAWNDEBUG"))
        for (size_t i = c0; i < papalo->size(); ++i)
            std::printf("scene: added %d (cid 0x%X)%s\n", (*papalo)[i]->oid, int((*papalo)[i]->Cid()), (*papalo)[i]->fInWorld ? "" : " (out of the world)");
    return cAdd;
}

void NoteGuardFlinch3(const LO* plo) { if (plo) s_mptFlinch3[plo] = g_clock.t; }
