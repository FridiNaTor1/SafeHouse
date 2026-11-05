// The spawners (cid 0x77, 0x173158..0x1749C0): guards brought into the world around the player, out of the
// camera's sight, from streaming proxies (sources kind 0: a proxy; 2: a point where one of the spawner's proxies
// is placed; 1: a path (TODO(actors): its 0x71 follower objects, 0x1E6648 / 0x1E6F10)).
// Native: the spawn point without the ground ray (0x173610, TODO(physics)).

#include "activate.h"
#include "ai.h"
#include "aipursuit.h"
#include "detect.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "aijobs.h"
#include "crv.h"
#include "guard.h"
#include "nav.h"
#include "objmethod.h"
#include "po.h"
#include "so.h"
#include "sw.h"
#include "update.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

int NRandom();                                  // xfmath.h (0x11D3C0)
// (the AI harnesses import the reference's guards but not the spawners' slots: they pause the spawners)
bool g_fSpawnersPaused = false;
float GRandInRange(float gLow, float gHigh);    // xfmath.h (0x11D448)

namespace
{
struct SOURCE  // orig 0x1C bytes at +0x54
{
    int kind = 0;
    OID oid = -1;
    LO* plo = nullptr;
    LO* plist = nullptr;  // +0xC (kind 1: its command list, a move along the path)
    bool fBusy = false;   // +0x14
    float tUntil = 0.0f;  // +0x18
};
struct SLOT  // orig 0x30 bytes at +0x60
{
    LO* plo = nullptr;    // the guard
    float tTime = 0.0f;   // +0x04
    float tWait = 0.0f;   // +0x08
    SOURCE* psrc = nullptr;  // +0x0C
    int k = 0;            // +0x10
    glm::vec3 pos{0.0f};  // +0x20
};
struct SPAWNER
{
    std::vector<SOURCE> asrc;      // methods 1324..1326
    std::vector<OID> aoidProxy;    // 1328 (+0x90, 8 at most)
    std::vector<LO*> aproxy;
    std::vector<glm::vec3> apos{glm::vec3(0.0f)};  // 1333 (+0x98, 16 at most; the load 0x1731B8 puts the origin first)
    std::vector<SLOT> aslot;       // +0x60 (+0x5C of them)
    int cUsed = 0;                 // +0x58
    float sPlanar = 5500.0f;       // +0x80
    int fEdge = 1, fRange = 1, fOcclusion = 1;  // +0x84..+0x8C
    float dtWaitFar = 30.0f, dtUntilFar = 50.0f;  // +0xA0 / +0xA4
    float tNext = 0.0f;            // +0xA8
    int nMode = 0;                 // +0xAC
    glm::vec3 posLast{0.0f};       // +0x70
};
std::unordered_map<const LO*, SPAWNER> s_mpspawner;

// A path's AI record (orig 0x1E9C30 / 0x1E9C88: 0x9C bytes at the path's +0x180): the spans of the path off the
// AI regions (sampled every 50, D_002C367C: no region under the sample, a different region than the sample before,
// or off the region's triangles). (0x1EA000's update against the regions' stamps is not done: the reference's
// spawner paths keep +0x10 = the loaded spans, stamp -1.)
struct PATHREC
{
    std::vector<std::pair<float, float>> aseg;    // +0x4 / +0x8: the loaded spans
    std::vector<std::pair<float, float>> aseg10;  // +0xC / +0x10: the spans in use (0x1EA000 adds the polygons')
    std::vector<NAVREGION*> apreg;                // +0x90 / +0x94: the regions under the path
    int n98 = -1;                                 // +0x98: the stamp of the last update
};
std::unordered_map<const LO*, PATHREC> s_mppathrec;

// orig 0x001E9C88
PATHREC& Pathrec(LO* plo, const CRV* pcrv)
{
    auto it = s_mppathrec.find(plo);
    if (it != s_mppathrec.end()) return it->second;
    PATHREC& rec = s_mppathrec[plo];
    const float tStart = pcrv->as.empty() ? 0.0f : pcrv->as.front();  // 0x1E6620
    const float tEnd = SLengthCrv(pcrv);                               // 0x1E6648
    glm::vec3 pos(0.0f);
    EvalCrvS(pcrv, tStart, &pos, nullptr);
    NAVREGION* pregPrev = PregionAt(pos, false);
    int iOpen = -1;
    float tPrev = tStart;
    for (float t = tStart; t < tEnd; t += 50.0f)
    {
        EvalCrvS(pcrv, t, &pos, nullptr);
        NAVREGION* preg = PregionAt(pos, false);
        if (preg && std::find(rec.apreg.begin(), rec.apreg.end(), preg) == rec.apreg.end() && rec.apreg.size() < 64)
            rec.apreg.push_back(preg);
        const bool fOff = !preg || preg != pregPrev || ItriNav(preg, pos, 0) < 0;  // (0x1C0920 = 0x1BF110 on +0xE0)
        if (fOff)
        {
            if (iOpen < 0 && rec.aseg.size() < 0x40)
            {
                rec.aseg.emplace_back(tPrev, 0.0f);
                iOpen = static_cast<int>(rec.aseg.size()) - 1;
            }
        }
        else if (iOpen >= 0)
        {
            rec.aseg[static_cast<size_t>(iOpen)].second = t;
            iOpen = -1;
        }
        tPrev = t;
        pregPrev = preg;
    }
    if (iOpen >= 0) rec.aseg[static_cast<size_t>(iOpen)].second = tEnd;
    const size_t c = rec.aseg.size();
    if (c >= 2 && pcrv->fClosed && rec.aseg[0].first == tStart && rec.aseg[c - 1].second == tEnd)
    {
        const float gA = rec.aseg[c - 1].first - tEnd, gB = rec.aseg[0].second + tEnd;
        rec.aseg[0].first = gA;
        rec.aseg[c - 1].second = gB;
    }
    rec.aseg10 = rec.aseg;
    rec.n98 = -1;
    if (std::getenv("SLY2N_SPAWNDEBUG"))
    {
        std::fprintf(stderr, "path %d: %zu off spans", plo->oid, c);
        for (const auto& seg : rec.aseg) std::fprintf(stderr, " (%.1f %.1f)", seg.first, seg.second);
        std::fprintf(stderr, "\n");
    }
    return rec;
}

// orig 0x001EA3B0: the span containing g, if any
bool FInPathSpan(const PATHREC& rec, float g)
{
    for (const auto& seg : rec.aseg10)
        if (seg.first <= g && g <= seg.second) return true;
    return false;
}

SPAWNER& Spawner(LO* plo) { return s_mpspawner[plo]; }
float G(const LO* plo, uint32_t ib, float g)
{
    uint32_t u;
    std::memcpy(&u, &g, 4);
    u = plo->U32Field(0, ib, u);
    std::memcpy(&g, &u, 4);
    return g;
}
int N(const LO* plo, uint32_t ib, int n) { return static_cast<int>(plo->U32Field(0, ib, static_cast<uint32_t>(n))); }
float DistXY(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a - b)); }

// orig 0x00173A18: the player's position, else the camera's
glm::vec3 PosCenter()
{
    if (LO* p = PloPoCurrent(g_psw)) return PosLoWorld(p);
    return g_cm.pos;
}

// orig 0x00173588 / 0x0014F308: the point in the camera's screen box of half-size r
bool FInScreen(const glm::vec3& v, float r) { return v.z > 0.0001f && -r < v.x && v.x < r && -r < v.y && v.y < r; }
glm::vec3 PosScreen(const glm::vec3& pos)
{
    const glm::vec3 d = pos - g_cm.pos;
    const float z = glm::dot(d, g_cm.mat[0]);
    const float x = -glm::dot(d, g_cm.mat[1]) / (z * std::max(g_cm.xScreenRange, 1e-4f));
    const float y = glm::dot(d, g_cm.mat[2]) / (z * std::max(g_cm.yScreenRange, 1e-4f));
    return glm::vec3(x, y, z);
}

// orig 0x001B44D8: the occluders of the spawn test: the geometry entries (flag bit 48 clear) of the objects on
// D_003E0714, the class list of class 0x31 (SO: the world's own geometry, oid 1, two objects in Paris; the
// reference's list holds nothing else). Props and actors never hide a spawn point.
std::vector<SOE*> s_apsoeOcclude;
void CollectOccluders()
{
    s_apsoeOcclude.clear();
    if (!g_psw) return;
    for (LO* plo : g_psw->mpcidaplo[0x31])
        for (SOE& soe : static_cast<ALO*>(plo)->asoe)
            if (!soe.f48) s_apsoeOcclude.push_back(&soe);
}

// orig 0x00173768: 0 a point the player could see (or too far), 1 just off the screen, 2 beyond the planar range,
// 3 hidden behind something
int NHiddenImpl(const SPAWNER& sp, const glm::vec3& pos, float sDistance);
int NHidden(const SPAWNER& sp, const glm::vec3& pos, float sDistance)
{
    const int n = NHiddenImpl(sp, pos, sDistance);
    static const bool s_f = std::getenv("SLY2N_SPAWNDEBUG") && std::atoi(std::getenv("SLY2N_SPAWNDEBUG")) >= 2;
    if (s_f) std::fprintf(stderr, "   hidden [%.0f, %.0f, %.0f] -> %d\n", pos.x, pos.y, pos.z, n);
    return n;
}
int NHiddenImpl(const SPAWNER& sp, const glm::vec3& pos, float sDistance)
{
    const glm::vec3 d = pos - g_cm.pos;
    if (sDistance * 0.8f < glm::length(d)) return 0;
    const glm::vec3 s = PosScreen(pos);
    if (s.z < 500.0f) return 0;
    if (!FInScreen(s, 1.7f)) return 0;
    if (sp.fEdge && !FInScreen(s, 1.25f)) return 1;
    if (sp.fRange && sp.sPlanar < glm::length(glm::vec2(d))) return 2;
    if (sp.fOcclusion)
    {
        // the line from the camera to the point 100 up (D_002BE5C8) through 0x1B44D8's entries
        const glm::vec3 posRaised = pos + glm::vec3(0.0f, 0.0f, 100.0f);
        LSG lsg;
        if (PsoeHitTestLineObjects(0, g_cm.pos, posRaised, s_apsoeOcclude, &lsg)) return 3;
    }
    return 0;
}

// orig 0x00173928: a slot freed (k 0: the guard removed; 1: put away, the place kept for 30 s)
void ReleaseSlot(SPAWNER& sp, SLOT& slot, int k, bool fKeep)
{
    if (std::getenv("SLY2N_SPAWNDEBUG") && slot.plo)
    {
        const glm::vec3 p = PosLoWorld(slot.plo);
        std::fprintf(stderr, "spawner: slot released (k %d) guard %d at %.0f %.0f %.0f, last drawn %.2f, t %.2f\n", k, slot.plo->oid, p.x, p.y, p.z, slot.tTime, g_clock.t);
    }
    slot.k = k;
    if (k == 0)
    {
        slot.tWait = g_clock.t;
        if (slot.plo) RemoveLo(g_psw, slot.plo);  // vt slot 9
        if (!fKeep && slot.psrc) slot.psrc->tUntil = g_clock.t;
    }
    else
    {
        slot.tWait = g_clock.t + sp.dtWaitFar;
        slot.pos = PosCenter();
        if (!fKeep && slot.psrc) slot.psrc->tUntil = g_clock.t + sp.dtUntilFar;
    }
    slot.plo = nullptr;
    if (slot.psrc) slot.psrc->fBusy = false;
    slot.psrc = nullptr;
    --sp.cUsed;
}

// The guard's +0x84 (its glob set's +4): the clock stored by RenderGlob (0x168638 at 0x1687C4) once the whole
// sphere passed the camera's tests (the caller's frustum, then VU0 0x38's distance fade != 0), i.e. the last time
// it was drawn. Natively (headless runs draw nothing) the same tests on the camera the frame was drawn with,
// at the previous frame's time.
float TGuardLastDrawn(LO* plo, float tWas)
{
    if (!FCidDerivedFrom(plo->Cid(), 0x30) || !FLoActive(plo)) return tWas;
    ALO* palo = static_cast<ALO*>(plo);
    if (!palo->pglobset) return tWas;
    const glm::mat4 mat = palo->apxfRef[2] ? palo->apxfRef[2]->matWorld : glm::mat4(1.0f);  // (ALO +0x60)
    const glm::vec3 dpos = glm::vec3(mat * glm::vec4(palo->pglobset->posCenter, 1.0f)) - g_cm.pos;
    if (!FInsideCmPlanes(&g_cm, dpos, palo->pglobset->sRadius)) return tWas;
    if (UCmSphereDistanceFade(&g_cm, dpos, palo->sRadius) == 0.0f) return tWas;
    return std::max(tWas, g_clock.t - g_clock.dt);
}

// orig 0x00173D18
void CheckSlots(SPAWNER& sp, const glm::vec3& pos)
{
    for (SLOT& slot : sp.aslot)
    {
        if (!slot.plo)
        {
            if (slot.k == 1 && 3500.0f < DistXY(slot.pos, pos))
            {
                slot.k = 0;
                slot.tWait = g_clock.t;
            }
        }
        else if (GUARD* pg = PguardFromLo(slot.plo); pg && pg->f330)
            ReleaseSlot(sp, slot, 1, false);  // (+0x330: knocked out, its place kept)
        else if (!slot.plo->fInWorld || !slot.psrc || !slot.psrc->plo || !slot.psrc->plo->fInWorld)
            ReleaseSlot(sp, slot, 0, false);
        else slot.tTime = TGuardLastDrawn(slot.plo, slot.tTime);  // (orig: max with the guard's +0x84)
    }
}

// orig 0x00173E78: a free slot whose wait is over (the longest waiting), else the occupied one whose guard is the
// oldest (4 s at least, D_002BE5E0), not ploExcept
SLOT* PslotChoose(SPAWNER& sp, const LO* ploExcept)
{
    float t4 = g_clock.t - 4.0f;
    float t8 = g_clock.t;
    SLOT* pBest = nullptr;
    for (SLOT& slot : sp.aslot)
    {
        if (slot.plo)
        {
            if (slot.plo == ploExcept) continue;
            if (pBest && !pBest->plo) continue;
            if (t4 <= slot.tTime) continue;
            t4 = slot.tTime;
        }
        else
        {
            if (t8 <= slot.tWait) continue;
            t8 = slot.tWait;
        }
        pBest = &slot;
    }
    return pBest;
}

// orig 0x00173F18: the nearest source 1500 away whose points are all hidden from the camera
SOURCE* PsrcChoose(SPAWNER& sp, const glm::vec3& pos, glm::vec3* ppos, glm::mat3* pmat, int* pfFlip)
{
    *pfFlip = 0;
    CollectOccluders();  // (0x1B44D8)
    float sSmallest = 3.4028235e38f;
    for (LO* p : sp.aproxy) sSmallest = std::min(sSmallest, p->sRadius);
    float sBest = 3.4028235e38f;
    SOURCE* psrcBest = nullptr;
    for (SOURCE& src : sp.asrc)
    {
        if (src.fBusy || g_clock.t < src.tUntil) continue;
        if (!src.plo || !src.plo->fInWorld) continue;
        // (debug: SLY2N_SPAWNSKIP=,oid,: those sources never chosen)
        if (static const char* s_sz = std::getenv("SLY2N_SPAWNSKIP"); s_sz && std::strstr(s_sz, ("," + std::to_string(src.oid) + ",").c_str())) continue;
        if (src.kind == 1)
        {
            // a path: its points every 200 (D_002BE5E8, offset at random by 0..1 of that) outside its off-region
            // spans (0x1EA3B0), the nearest 1500 away whose spawn points are all hidden, facing along the path (away
            // from the camera's view: flipped when the path runs towards it)
            const CRV* pcrv = PcrvFromLo(src.plo);
            if (!pcrv) continue;
            const PATHREC& rec = Pathrec(src.plo, pcrv);
            const int c = static_cast<int>(SLengthCrv(pcrv) / 200.0f);
            // (test aid: SLY2N_SPAWNALONG="path oid:x,y": that path's only candidate is its point nearest (x, y), so a
            // comparison with the reference can spawn its guard at the reference's place; the random draws still happen)
            float sForced = -1.0f;
            if (static const char* s_szAlong = std::getenv("SLY2N_SPAWNALONG"); s_szAlong)
            {
                int oidPath = 0;
                float x = 0.0f, y = 0.0f;
                if (std::sscanf(s_szAlong, "%d:%f,%f", &oidPath, &x, &y) == 3 && oidPath == src.plo->oid)
                {
                    float dBest = 3.4e38f;
                    for (float sTry = 0.0f; sTry <= SLengthCrv(pcrv); sTry += 0.05f)
                    {
                        glm::vec3 posTry(0.0f), dirTry(1.0f, 0.0f, 0.0f);
                        EvalCrvS(pcrv, sTry, &posTry, &dirTry);
                        const float d = std::hypot(posTry.x - x, posTry.y - y);
                        if (d < dBest) { dBest = d; sForced = sTry; }
                    }
                    std::fprintf(stderr, "spawnalong: path %d s %.3f (%.2f from the point)\n", oidPath, sForced, dBest);
                }
            }
            for (int j = 0; j < c; ++j)
            {
                float sAlong = 200.0f * (static_cast<float>(j) + GRandInRange(0.0f, 1.0f));
                if (sForced >= 0.0f)
                {
                    if (j != static_cast<int>(sForced / 200.0f)) continue;
                    sAlong = sForced;
                }
                if (FInPathSpan(rec, sAlong)) continue;
                glm::vec3 posCand(0.0f), dir(1.0f, 0.0f, 0.0f);
                EvalCrvS(pcrv, sAlong, &posCand, &dir);
                const float s = DistXY(pos, posCand);
                if (s < 1500.0f || sBest < s) continue;
                const bool fOpposite = glm::dot(g_cm.mat[0], dir) < 0.0f;
                if (!fOpposite) dir = -dir;
                const float rad = std::atan2(dir.y, dir.x);
                const glm::mat3 mat = glm::mat3(std::cos(rad), std::sin(rad), 0.0f, -std::sin(rad), std::cos(rad), 0.0f, 0.0f, 0.0f, 1.0f);
                bool f = true;
                for (const glm::vec3& d : sp.apos)
                    if (!NHidden(sp, posCand + mat * d, sSmallest)) { f = false; break; }
                if (!f) continue;
                sBest = s;
                psrcBest = &src;
                *pfFlip = fOpposite;
                *ppos = posCand;
                *pmat = mat;
            }
            continue;
        }
        if (src.kind != 0 && src.kind != 2) continue;
        const float sRadius = src.kind == 0 ? src.plo->sRadius : sSmallest;
        const glm::vec3 posCand = PosLoWorld(src.plo);
        const glm::mat3 mat = glm::mat3(src.plo->Cid() >= 0 && FCidDerivedFrom(src.plo->Cid(), 3) ? static_cast<LOX*>(src.plo)->mat : glm::mat4(1.0f));
        const float s = DistXY(pos, posCand);
        if (s < 1500.0f || sBest < s) continue;
        bool f = true;
        for (const glm::vec3& d : sp.apos)
            if (!NHidden(sp, posCand + mat * d, sRadius)) { f = false; break; }
        if (!f) continue;
        sBest = s;
        psrcBest = &src;
        *ppos = posCand;
        *pmat = mat;
    }
    return psrcBest;
}

// orig 0x00174490: the guard of the source (kind 0: its proxy's instance; 2: one of the spawner's proxies put there)
LO* PloSpawn(SPAWNER& sp, SOURCE* psrc, const glm::vec3& pos, const glm::mat3& mat, int fFlip)
{
    LO* plo = nullptr;
    if (psrc->kind == 0) plo = PloAttachProxy(g_psw, psrc->plo, 2);
    else
    {
        // the place dropped onto the ground (0x173610, D_002BE5F0), the proxies in the game's random order
        // (NRandInRange 0x11D3F8: each swapped with one at random)
        glm::vec3 posGround = pos;
        FDropOnGround(&posGround, 100.0f, -350.0f, true);
        std::vector<LO*> a = sp.aproxy;
        const int n = static_cast<int>(a.size());
        for (int j = 0; j < n; ++j) std::swap(a[static_cast<size_t>(j)], a[static_cast<size_t>(NRandom() % n)]);
        // (test aid: SLY2N_SPAWNFIRST="oid[,oid..]": those proxies tried first, in that order, so a comparison with
        // the reference can start with the same guards when the two games' random states differ)
        if (static const char* s_sz = std::getenv("SLY2N_SPAWNFIRST"); s_sz)
            for (const char* q = s_sz; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
            {
                const OID oid = std::atoi(q);
                auto it = std::find_if(a.begin(), a.end(), [&](LO* p) { return p->oid == oid; });
                if (it != a.end()) std::rotate(a.begin(), it, it + 1);
            }
        const glm::vec3& pos = posGround;
        for (LO* p : a)
        {
            // vt slots 30 / 29 by the proxy's class (the proxies are ALOs or LOXes: a static_cast to LOX wrote over an
            // ALO's record list (worlds 14 / 32), the physics port)
            if (ALO* pa = dynamic_cast<ALO*>(p))
            {
                SetAloRotation(pa, mat);  // 0x1F42E0
                SetAloPos(pa, pos);       // 0x1F40A0
            }
            else if (LOX* px = dynamic_cast<LOX*>(p))
            {
                px->mat = glm::mat4(mat);
                px->mat[3] = glm::vec4(pos, 1.0f);
            }
            plo = PloAttachProxy(g_psw, p, 2);
            if (std::getenv("SLY2N_SPAWNDEBUG")) std::fprintf(stderr, "  proxy %d -> %d\n", p->oid, plo ? plo->oid : -1);
            if (plo) break;
        }
        if (psrc->kind == 1 && plo)
        {
            // a path's guard: its walk along it (the list's move, in the found direction) as its patrol, its reactions
            // told (-1, then 1: the patrol)
            if (auto* pl = dynamic_cast<CMDLIST*>(psrc->plist))
                if (CMD* pc = pl->PcmdFirst()) pc->n8 = fFlip;
            if (GUARD* pg = PguardFromLo(plo))
            {
                pg->pcmdlistPatrol = dynamic_cast<CMDLIST*>(psrc->plist);
                if (auto* pj = static_cast<JOBREACT*>(PjobFind(pg, 0x9C)))
                {
                    AIMSG m;
                    m.k = -1;
                    pj->React(m);
                    m.k = 1;
                    pj->React(m);
                }
            }
        }
    }
    if (plo)
        if (GUARD* pg = PguardFromLo(plo)) pg->n1000 = 1;  // (+0x1000: placed by a spawner)
    return plo;
}

// orig 0x00173A68 (vt slot 16)
void UpdateSpawner(LO* p)
{
    if (g_fSpawnersPaused) return;
    if (p->tNextUpdate <= g_clock.t) HandleLoSpliceEvent(p, 1, true, 0, nullptr);
    SPAWNER& sp = Spawner(p);
    const glm::vec3 pos = PosCenter();
    CheckSlots(sp, pos);
    if (g_clock.t < sp.tNext) return;
    sp.tNext = g_clock.t + 1.0f;
    if (sp.cUsed == static_cast<int>(sp.aslot.size()) && DistXY(pos, sp.posLast) < 300.0f) return;
    // the current player pursued (its record 0x1F8240 with guards on it: aipursuit.h): mode 0 spawns nothing then,
    // mode 1 only in the first 10 s of the pursuit (D_002BE5CC); a new guard then knows where the player is
    TARGETREC* ptrec = nullptr;
    if (LO* pme = PloPoCurrent(g_psw))
        if ((ptrec = PtargetrecFind(pme)) && ptrec->apguard.empty()) ptrec = nullptr;
    if (ptrec)
    {
        if (sp.nMode == 0) return;
        if (sp.nMode == 1 && 10.0f < g_clock.t - ptrec->t0) return;
    }
    // (a record with one guard: that guard's slot is not taken, 0x173E78's second argument)
    const LO* ploExcept = ptrec && ptrec->apguard.size() == 1 && ptrec->apguard[0] ? ptrec->apguard[0]->palo : nullptr;
    SLOT* pslot = PslotChoose(sp, ploExcept);
    if (std::getenv("SLY2N_SPAWNDEBUG"))
    {
        int cIn = 0;
        for (const SOURCE& src : sp.asrc) cIn += src.plo && src.plo->fInWorld;
        std::fprintf(stderr, "spawner %d update t %.4f: slot %d, sources in world %d, player at %.0f %.0f; camera %.1f %.1f %.1f dir %.4f %.4f %.4f range %.4f %.4f\n", p->oid, g_clock.t,
                     pslot ? 1 : 0, cIn, pos.x, pos.y, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z, g_cm.xScreenRange, g_cm.yScreenRange);
        if (std::atoi(std::getenv("SLY2N_SPAWNDEBUG")) >= 3)
            for (const SOURCE& src : sp.asrc)
            {
                const glm::vec3 q = src.plo ? PosLoWorld(src.plo) : glm::vec3(0.0f);
                std::fprintf(stderr, "   source %d kind %d (cid 0x%X) in %d busy %d until %.2f at %.0f %.0f %.0f d %.0f\n", src.oid, src.kind,
                             src.plo ? int(src.plo->Cid()) : -1, src.plo ? int(src.plo->fInWorld) : -1, int(src.fBusy), src.tUntil, q.x, q.y, q.z, DistXY(pos, q));
            }
    }
    if (!pslot) return;
    glm::vec3 posSpawn;
    glm::mat3 matSpawn;
    int fFlip = 0;
    SOURCE* psrc = PsrcChoose(sp, pos, &posSpawn, &matSpawn, &fFlip);
    if (!psrc) return;
    sp.posLast = pos;
    if (pslot->plo) ReleaseSlot(sp, *pslot, 0, true);  // (an occupied slot taken over: its guard removed)
    LO* plo = PloSpawn(sp, psrc, posSpawn, matSpawn, fFlip);
    if (!plo) return;
    if (FCidDerivedFrom(plo->Cid(), 0x30))
    {
        ALO* palo = static_cast<ALO*>(plo);
        palo->gFade = 0.01f;   // 0x1F7D10
        palo->dgFade = 2.0f;   // 0x1F7C68 (0.5 s)
    }
    AddLo(g_psw, plo);
    if (ptrec)
        if (GUARD* pg = PguardFromLo(plo))
            if (pg->pdet)
                if (DETENTRY* pe = PdetentryFind(pg->pdet, ptrec->plo)) SetDetentryState(pg->pdet, pe, 2);  // (0x154038)
    if (std::getenv("SLY2N_SPAWNDEBUG"))
        std::fprintf(stderr, "spawner %d: guard %d (cid 0x%X) from source %d kind %d at %.0f %.0f %.0f, t %.2f flip %d facing %.3f %.3f cam %.3f %.3f %.3f\n", p->oid, plo->oid,
                     plo->Cid(), psrc->oid, psrc->kind, posSpawn.x, posSpawn.y, posSpawn.z, g_clock.t, fFlip, matSpawn[0].x, matSpawn[0].y,
                     g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z);
    pslot->plo = plo;
    pslot->tTime = g_clock.t;
    pslot->psrc = psrc;
    ++sp.cUsed;
    psrc->fBusy = true;
    sp.tNext = g_clock.t + 2.5f;
    CRef ref;
    ref.SetBasic(plo);
    HandleLoSpliceEvent(p, 0x19, false, 1, &ref);  // its script: the new guard
}

struct INITSPAWNER
{
    INITSPAWNER()
    {
        RegisterLoUpdate(0x173A68, UpdateSpawner);
        // the load's setters 1324..1326 (sources of kinds 0 / 1 / 2), 1328 (the proxies), 1333 (the points)
        RegisterObjectSetter(1324, [](LO* plo, const CRef& r) { if (Spawner(plo).asrc.size() < 0x80) Spawner(plo).asrc.push_back(SOURCE{0, r.m_n}); });
        RegisterObjectSetter(1325, [](LO* plo, const CRef& r) { if (Spawner(plo).asrc.size() < 0x80) Spawner(plo).asrc.push_back(SOURCE{1, r.m_n}); });
        RegisterObjectSetter(1326, [](LO* plo, const CRef& r) { if (Spawner(plo).asrc.size() < 0x80) Spawner(plo).asrc.push_back(SOURCE{2, r.m_n}); });
        RegisterObjectSetter(1328, [](LO* plo, const CRef& r) { if (Spawner(plo).aoidProxy.size() < 8) Spawner(plo).aoidProxy.push_back(r.m_n); });
        RegisterObjectSetter(1333, [](LO* plo, const CRef& r) { if (Spawner(plo).apos.size() < 16) Spawner(plo).apos.push_back(r.GetVector()); });
    }
} s_initspawner;
}

// orig 0x001733F8 (vt slot 15, after the base): the sources' objects (kind 1: a 0x71 list moving along the path,
// 0x170C50), the slots
void StartSpawner(SW* psw, LO* plo)
{
    SPAWNER& sp = Spawner(plo);
    sp.sPlanar = G(plo, 0x80, 5500.0f);
    sp.fEdge = N(plo, 0x84, 1);
    sp.fRange = N(plo, 0x88, 1);
    sp.fOcclusion = N(plo, 0x8C, 1);
    sp.dtWaitFar = G(plo, 0xA0, 30.0f);
    sp.dtUntilFar = G(plo, 0xA4, 50.0f);
    sp.nMode = N(plo, 0xAC, 0);
    sp.aslot.assign(static_cast<size_t>(std::max(0, N(plo, 0x5C, 0))), SLOT{});
    for (SOURCE& src : sp.asrc)
    {
        src.plo = PloFindSwObjectByOid(psw, src.oid);  // (0x15E938 kind 5)
        if (src.kind != 1) continue;
        // a path source: a command list (0x13E3C8(0x71, 0, 0x2CB, -1), in the world) of one move along the path
        // {n8 1, nC -1} (0x170C50: op 6)
        if (auto* pl = static_cast<CMDLIST*>(PloNew(0x71, psw, nullptr, 0x2CB, -1)))
        {
            if (CMD* pc = PcmdAppend(pl, 6, src.oid))
            {
                pc->n8 = 1;
                pc->nC = -1;
            }
            src.plist = pl;
        }
    }
    if (std::getenv("SLY2N_SPAWNDEBUG"))
        std::fprintf(stderr, "spawner %d: %zu sources, %zu proxies, %zu points, %zu slots\n", plo->oid, sp.asrc.size(),
                     sp.aoidProxy.size(), sp.apos.size(), sp.aslot.size());
    sp.aproxy.clear();
    for (OID oid : sp.aoidProxy)
        if (LO* p = PloFindSwObjectByOid(psw, oid))
        {
            sp.aproxy.push_back(p);
            if (std::getenv("SLY2N_SPAWNDEBUG")) std::fprintf(stderr, "spawner %d: proxy %d (cid 0x%X)\n", plo->oid, oid, p->Cid());
        }
}

void ResetSpawners()
{
    s_mpspawner.clear();
    s_mppathrec.clear();
}

// orig 0x001EA3B0 on a path's AI record (0x1E9C30 at its +0x180): the off-region span containing g (for the guards'
// path steps, 0x17ECF0), false when none
// orig 0x0014C9F8 with k 0: the union of two sorted span lists (at most cMax)
std::vector<std::pair<float, float>> UnionSpans(const std::vector<std::pair<float, float>>& a1,
                                                const std::vector<std::pair<float, float>>& a2, size_t cMax)
{
    std::vector<std::pair<float, float>> out;
    size_t j1 = 0, j2 = 0;
    int i1 = 0, i2 = 0;
    bool fPrev = false;
    float gIn = 0.0f;
    auto at = [](const std::pair<float, float>& p, int i) { return i ? p.second : p.first; };
    while (j1 < a1.size() || j2 < a2.size())
    {
        const float g1 = j1 < a1.size() ? at(a1[j1], i1) : FLT_MAX;
        const float g2 = j2 < a2.size() ? at(a2[j2], i2) : FLT_MAX;
        float g;
        if (g1 <= g2)
        {
            g = g1;
            i1 = 1 - i1;
            if (i1 == 0) ++j1;
        }
        else
        {
            g = g2;
            i2 = 1 - i2;
            if (i2 == 0) ++j2;
        }
        const bool fIn = 1 <= i1 + i2;
        if (fIn != fPrev)
        {
            if (fIn) gIn = g;
            else
            {
                if (out.size() >= cMax + 1) break;
                out.emplace_back(gIn, g);
            }
        }
        fPrev = fIn;
    }
    if (out.size() > cMax) out.resize(cMax);
    return out;
}

// orig 0x001EA000: when a region under the path changed (its polygons: +0x114 past the record's +0x98), the spans
// inside the polygons met along the path (sampled every 50) joined to the loaded spans
void UpdatePathrec(PATHREC& rec, const CRV* pcrv)
{
    size_t k = 0;
    for (; k < rec.apreg.size(); ++k)
        if (rec.n98 < rec.apreg[k]->nStamp) break;
    if (k >= rec.apreg.size()) return;
    std::vector<std::pair<float, float>> aseg;
    const float tStart = pcrv->as.empty() ? 0.0f : pcrv->as.front();  // 0x1E6620
    const float tEnd = SLengthCrv(pcrv);                               // 0x1E6648
    for (float t = tStart; t < tEnd; t += 50.0f)
    {
        glm::vec3 pos;
        EvalCrvS(pcrv, t, &pos, nullptr);
        if (NAVPOLY* pp = PpolyAtPoint(pos))
        {
            CRV* pc = const_cast<CRV*>(pcrv);
            const int fSave = pc->fClosed;
            pc->fClosed = 0;
            float segStart = 0.0f, segEnd = 0.0f;
            if (NNavPathLeavesPoly(pp, pcrv, false, &segStart, t) == 3) segStart = tStart;
            if (NNavPathLeavesPoly(pp, pcrv, true, &segEnd, t) == 3) segEnd = tEnd;
            pc->fClosed = fSave;
            // (the spans it overlaps replaced; one beginning before it extended)
            size_t i = aseg.size();
            bool fNew = true;
            while (i > 0)
            {
                if (aseg[i - 1].second < segStart) break;
                --i;
                if (aseg[i].first < segStart)
                {
                    fNew = false;
                    break;
                }
            }
            size_t c = i + 1;
            if (c >= 17) c = i;
            if (i >= aseg.size()) aseg.resize(i + 1);
            if (fNew) aseg[i].first = segStart;
            aseg[i].second = segEnd;
            aseg.resize(c);
            t = segEnd;
        }
    }
    std::vector<std::pair<float, float>> a = UnionSpans(aseg, rec.aseg, 0x10);
    if (a.size() >= 2 && pcrv->fClosed && a[0].first == tStart && a.back().second == tEnd)
    {
        const float gA = a.back().first - tEnd, gB = a[0].second + tEnd;
        a[0].first = gA;
        a.back().second = gB;
    }
    rec.aseg10 = a;
    rec.n98 = NNextNavStamp();
    if (std::getenv("SLY2N_SPAWNDEBUG"))
    {
        std::fprintf(stderr, "path update: %zu spans", a.size());
        for (const auto& seg : a) std::fprintf(stderr, " (%.1f %.1f)", seg.first, seg.second);
        std::fprintf(stderr, "\n");
    }
}

// orig 0x001EA000 on a path's record (the guards' path step 0x17ECF0, before 0x1EA3B0)
void UpdatePathSpans(LO* ploPath, const CRV* pcrv)
{
    if (!ploPath || !pcrv) return;
    UpdatePathrec(Pathrec(ploPath, pcrv), pcrv);
}

bool FPathSpanAt(LO* ploPath, const CRV* pcrv, float g, float* pgMin, float* pgMax)
{
    if (!ploPath || !pcrv) return false;
    for (const auto& seg : Pathrec(ploPath, pcrv).aseg10)
        if (seg.first <= g && g <= seg.second)
        {
            *pgMin = seg.first;
            *pgMax = seg.second;
            return true;
        }
    return false;
}

// orig 0x001B44D8 for the other callers (the boss's aim 0x155FB0): the occluders, collected now
const std::vector<SOE*>& ApsoeOccluders()
{
    CollectOccluders();
    return s_apsoeOcclude;
}
