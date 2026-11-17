// racer3.cpp: Sly 3's path racer (class 0x98, orig3 0x183D80..0x1847BC), from the Sly 3 decomp.
//
// Ep4 m4_rccar (c_ext, world 23): the racer 69422 runs Murray's side of the race against Bentley's RC car. It moves
// its runner (+0x54, option 1852) along its path (+0x40, 1847) and measures the tracked object's progress on the same
// path (+0x5C, 1855: the car). The update 0x183FA8: the tracked object's arc length +0x64 (0x22EA88); while running
// (+0x6C), against the RC car with its pad (0x13B500 cid 0x55, 0x1A9478) the runner keeps ahead of it in the first two
// thirds (the target the car's length + 2500 / + 350 (beyond one third), at least its own + 100 / + 8, smoothed by
// 0x11E730 with D_00321890 / D_003218A0), then runs at the speed +0x58 x (1 - 0.3 g) (g: 0x171C98's fraction); else at
// +0x58. At the path's end: stopped (0x184778) and message 5 to itself (the splice: Murray's win, or after the race his
// arrival); else the runner is put at its point (0x184630, vt slot 34) and, inside the shape +0x68 (1857, 0x27F258):
// stopped and message 6. The start 0x184728 (method 1858 = thunk 0x1104B8) adds the runner (its vt slot 8) and sets
// +0x6C; the stop 0x184778 (1859 = 0x110530) removes it (slot 9). Option 1854: the runner's point (getter 0x184630);
// its setter 0x1846C8 puts the racer's progress at the point's and starts it when short of the end. m4_rccar's SCPT
// 3681: the race start (69441) starts it; after the car's win, 69480 sets 1854 to 69498 and the racer's message 5 there
// sets 3681's alarm 69484 (the reference: alarm 0x10F6C from the splice, vblank 3130).
// Natively: 0x1A9478 (the car's pad
// slot) is "the car is the current player"; the ground snap of 0x184630 (+0x44) is the scripts' ground drop.
// The course markers (AUDIT5 MODES5-02): the samples of the post-load 0x183E00, the window of the update's tail, the
// draw slot 21 0x184350 (models 1849 / 1850; m4_rccar's 89577 / 89581).
// Debug: SLY3_RACERPRINT=1.

#include "../activate.h"
#include "../brx.h"
#include "../clock.h"
#include "../crv.h"
#include "../emitter.h"
#include "../render.h"
#include "../detect.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../po.h"
#include "../ref.h"
#include "../so.h"
#include "../spliceglue.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../xf.h"
#include "sly3.h"
#include "suck3.h"
#include "trigger3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);   // mat.h (0x11B830; orig3 0x11CBF0)

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_RACERPRINT") != nullptr;
    return s_f;
}

struct RACER3
{
    LO* plo = nullptr;
    SW* psw = nullptr;
    LO* path = nullptr;      // +0x40 (1847)
    int fSnap = 0;           // +0x44 (1848)
    float step = 50.0f;      // +0x50 (1851; the samples' spacing)
    LO* runner = nullptr;    // +0x54 (1852)
    float speed = 100.0f;    // +0x58 (1853)
    LO* tracked = nullptr;   // +0x5C (1855)
    float window = 2000.0f;  // +0x60 (1856)
    float sTracked = 0.0f;   // +0x64
    LO* shape = nullptr;     // +0x68 (1857)
    int fOn = 0;             // +0x6C
    float s = 0.0f;          // +0x70: the runner's arc length
    float ds = 0.0f;         // +0x74: its rate (the smoothing's)
    ALO* paloAhead = nullptr;   // +0x48 (1849): the course marker ahead of the runner
    ALO* paloBehind = nullptr;  // +0x4C (1850): behind it (the runner's progress +0x70 past the sample)
    // the post-load 0x183E00: every +0x50 along the path a sample (+0x7C, count +0x78: its arc length, the matrix along
    // the path (snapped to the ground with +0x44, 0x184580)); the update's window +0x80 / +0x84 of the samples around
    // the tracked object's progress (+0x60 / +0x50 samples each way; none: -1 / -2)
    struct SAMPLE { float s; glm::mat4 mat; };
    std::vector<SAMPLE> asample;
    bool fSampled = false;
    int iFirst = 0, iLast = 0;
};
std::unordered_map<const LO*, RACER3> s_mpracer;

LO* PloOptRacer(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindRelative(plo, v.n);
}

// orig3 0x183D80 (init: +0x50 50, +0x58 100, +0x60 2000) and the options (EOPIDs 1847..1857, fields 0x40..0x68)
RACER3* Pracer(LO* plo)
{
    if (!plo || !g_psw) return nullptr;
    auto it = s_mpracer.find(plo);
    if (it != s_mpracer.end() && it->second.psw == g_psw) return &it->second;
    RACER3& r = s_mpracer[plo];
    r = RACER3{};
    r.plo = plo;
    r.psw = g_psw;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& a = o.aval[0];
        switch (o.eopid)
        {
        case 1847: r.path = PloOptRacer(plo, a); break;
        case 1848: r.fSnap = a.n; break;
        case 1851: r.step = a.ag[0]; break;
        case 1852: r.runner = PloOptRacer(plo, a); break;
        case 1853: r.speed = a.ag[0]; break;
        case 1855: r.tracked = PloOptRacer(plo, a); break;
        case 1856: r.window = a.ag[0]; break;
        case 1857: r.shape = PloOptRacer(plo, a); break;
        case 1849: case 1850:
        {
            LO* p = PloOptRacer(plo, a);
            ALO* palo = p && FIsLoDerivedFrom(p, sly3::CID_ALO) ? static_cast<ALO*>(p) : nullptr;
            (o.eopid == 1849 ? r.paloAhead : r.paloBehind) = palo;
            break;
        }
        default: break;
        }
    }
    if (FPrint())
        std::printf("racer3: %d path %d runner %d (cid 0x%X) tracked %d shape %d speed %.0f snap %d\n", plo->oid, r.path ? r.path->oid : -1,
                    r.runner ? r.runner->oid : -1, r.runner ? r.runner->Cid() : -1, r.tracked ? r.tracked->oid : -1, r.shape ? r.shape->oid : -1, r.speed,
                    r.fSnap);
    return &r;
}

const CRV* Pcrv(const RACER3& r) { return r.path ? PcrvFromLo(r.path) : nullptr; }
float SLength(const RACER3& r) { const CRV* p = Pcrv(r); return p ? SLengthCrv(p) : 0.0f; }

glm::vec3 PosLo(LO* plo)
{
    if (!plo) return glm::vec3(0.0f);
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
        if (XF* pxf = PxfObj(static_cast<ALO*>(plo))) return glm::vec3(pxf->matWorld[3]);   // (vt slot 36)
    return sly3::PosLoWorld3(plo);
}

// 0x22EA88 kind 0: the arc length of the path's point nearest to pos
float SNearest(const RACER3& r, const glm::vec3& pos)
{
    float s = 0.0f;
    if (const CRV* p = Pcrv(r)) NearestCrvAll(p, pos, nullptr, nullptr, nullptr, &s);
    return s;
}

// orig3 0x184630: the runner's point
glm::vec3 PosRunner(const RACER3& r)
{
    glm::vec3 pos(0.0f);
    if (const CRV* p = Pcrv(r)) EvalCrvS(p, r.s, &pos, nullptr);
    return pos;
}

// orig3 0x184728 / 0x184778: the runner added / removed (its vt slots 8 / 9)
void StartRacer(RACER3& r)
{
    if (r.fOn) return;
    if (r.runner) AddLo(g_psw, r.runner);
    r.fOn = 1;
    if (FPrint()) std::printf("racer3: %d start at s %.0f of %.0f (t %.3f) st %d inworld %d removed %d active %d iupd %d grf 0x%X parent %d\n", r.plo->oid, r.s, SLength(r), g_clock.t, r.plo->st, r.plo->fInWorld, r.plo->fRemoved, r.plo->fNodeActive, r.plo->iupd, r.plo->grfUpd, r.plo->paloParent ? r.plo->paloParent->oid : -1);
}
void StopRacer(RACER3& r)
{
    if (!r.fOn) return;
    if (r.runner) RemoveLo(g_psw, r.runner);
    r.fOn = 0;
    if (FPrint()) std::printf("racer3: %d stop at s %.0f of %.0f (t %.3f)\n", r.plo->oid, r.s, SLength(r), g_clock.t);
}

void PlaceRunner(RACER3& r, const glm::vec3& pos)
{
    if (!r.runner || !FIsLoDerivedFrom(r.runner, sly3::CID_ALO)) return;
    ALO* palo = static_cast<ALO*>(r.runner);
    SetAloPos(palo, pos);   // (vt slot 34)
}

// orig3 0x184580 (the sample put on the ground: the first solid between 100 above and 350 below it (0x165000 kind 1
// on the world's collision entries, 0x1DC7E0), the normal its up)
void SnapSample(glm::vec3* ppos, glm::vec3* pup)
{
    if (!g_psw) return;
    std::vector<SOE*> apsoe;
    for (const auto& up : g_psw->aploAll)
        if (up && up->fInWorld && FIsLoDerivedFrom(up.get(), sly3::CID_ALO))
            for (SOE& e : static_cast<ALO*>(up.get())->asoe)
                if (!e.f50)
                {
                    UpdateSoeWorld(&e);
                    apsoe.push_back(&e);
                }
    LSG lsg;
    const glm::vec3 a = *ppos + glm::vec3(0.0f, 0.0f, 100.0f), b = *ppos + glm::vec3(0.0f, 0.0f, -350.0f);
    if (PsoeHitTestLineObjects(1, a, b, apsoe, &lsg) && lsg.t0 != 0.0f)
    {
        *ppos = lsg.pos0;
        *pup = glm::vec3(lsg.plane0);
    }
}

// orig3 0x183E00 (the post-load: the samples)
void SampleRacer(RACER3& r)
{
    if (r.fSampled) return;
    r.fSampled = true;
    const CRV* pcrv = Pcrv(r);
    if (!pcrv || r.step <= 0.0f) return;
    const auto tStart = std::chrono::steady_clock::now();
    const int c = static_cast<int>(SLengthCrv(pcrv) / r.step);
    r.asample.resize(static_cast<size_t>(std::max(c, 0)));
    float s = 0.0f;
    for (RACER3::SAMPLE& sample : r.asample)
    {
        glm::vec3 pos(0.0f), dir(1.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 1.0f);   // (D_00317550)
        sample.s = s;
        EvalCrvS(pcrv, s, &pos, &dir);   // (0x22E330)
        if (r.fSnap) SnapSample(&pos, &up);
        sample.mat = glm::mat4(BuildLookAtMatrix(dir, up));   // 0x11CBF0, then 0x11D6D8 (scale 1)
        sample.mat[3] = glm::vec4(pos, 1.0f);
        s += r.step;
    }
    if (FPrint())
        std::printf("racer3: %d %zu samples (snap %d) in %.1f ms\n", r.plo->oid, r.asample.size(), r.fSnap,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count());
}

// orig3 0x183FA8's tail: the samples within +0x60 of the tracked object's progress (a binary search for the sample
// pair around it)
void UpdateRacerWindow(RACER3& r)
{
    SampleRacer(r);
    const int n = static_cast<int>(r.asample.size());
    int lo = 0, hi = n - 1, found = -1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) >> 1, next = mid + 1;
        const float gMid = r.asample[size_t(mid)].s, gNext = next < n ? r.asample[size_t(next)].s : gMid;
        if (gMid <= r.sTracked && r.sTracked <= gNext) { found = mid; break; }
        if (r.sTracked < gMid) hi = mid - 1;
        else lo = next;
    }
    if (found == -1) { r.iFirst = -1; r.iLast = -2; return; }
    const int k = r.step != 0.0f ? static_cast<int>(r.window / r.step) : 0;
    r.iFirst = std::max(found - k, 0);
    r.iLast = std::min(found + k, n - 1);
}

// orig3 0x184350 (vt slot 21, the racer's draw): the samples of the window drawn (0x1C8248: the model's slot 22 at the
// sample's matrix) with the behind model when the runner's progress is past them, else the ahead one; full within 70 %
// of +0x60 of the tracked object, then fading to 0 at +0x60 (the per-view sphere test 0x18E500: the renderer's cull)
PFNEXTRADRAWS s_pfnExtraPrev = nullptr;
void CollectRacerDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra)
{
    if (s_pfnExtraPrev) s_pfnExtraPrev(scene, aextra);
    if (!g_psw) return;
    for (auto& [plo, r] : s_mpracer)
    {
        if (r.psw != g_psw || !plo->fInWorld || plo->fRemoved || !r.tracked) continue;
        if (!r.fSampled) SampleRacer(r);
        const glm::vec3 pos = PosLo(r.tracked);
        const float sNear = r.window * 0.7f, sRange = r.window - sNear;
        static int s_cDraw = 0;
        if (FPrint() && ++s_cDraw % 120 == 1)
            std::printf("racer3: %d draws samples %d..%d of %zu (tracked at %.0f (%.0f %.0f %.0f), runner %.0f) models %d / %d (scene %d / %d) sample0 (%.0f %.0f %.0f)\n", plo->oid,
                        r.iFirst, r.iLast, r.asample.size(), r.sTracked, pos.x, pos.y, pos.z, r.s, r.paloAhead ? r.paloAhead->oid : -1, r.paloBehind ? r.paloBehind->oid : -1,
                        r.paloAhead && PsgsOfAlo(scene, r.paloAhead) != nullptr, r.paloBehind && PsgsOfAlo(scene, r.paloBehind) != nullptr,
                        r.asample.empty() ? 0.0f : r.asample[0].mat[3].x, r.asample.empty() ? 0.0f : r.asample[0].mat[3].y, r.asample.empty() ? 0.0f : r.asample[0].mat[3].z);
        for (int i = std::max(r.iFirst, 0); i <= r.iLast && i < static_cast<int>(r.asample.size()); ++i)
        {
            const RACER3::SAMPLE& sample = r.asample[size_t(i)];
            ALO* palo = sample.s < r.s ? r.paloBehind : r.paloAhead;
            if (!palo) continue;
            const float d = glm::length(pos - glm::vec3(sample.mat[3]));
            float g = 1.0f;
            if (sNear <= d)
            {
                const float t = d - sNear;
                g = 1.0f - t / std::max(sRange, t);
            }
            if (0.0f < g) AppendAloExtraDraws(scene, palo, sample.mat, g, aextra);
        }
    }
}

// orig3 0x183FA8
void UpdateRacer(LO* plo)
{
    RACER3* pr = Pracer(plo);
    if (!pr || !Pcrv(*pr)) return;
    RACER3& r = *pr;
    const float sMax = SLength(r);
    if (r.tracked) r.sTracked = SNearest(r, PosLo(r.tracked));
    UpdateRacerWindow(r);
    if (!r.fOn) return;
    const float dt = g_clock.dt;
    const bool fCar = r.tracked && FIsLoDerivedFrom(r.tracked, 0x55) && PloPoCurrent(g_psw) == r.tracked;
    if (fCar)
    {
        if (r.sTracked < sMax * 0.67f)
        {
            const bool fNear = r.sTracked < sMax * 0.33f;
            static const cmm::SMPA s_smpaNear{1200.0f, 200.0f, 3.0f, 625.0f};   // D_00321890
            static const cmm::SMPA s_smpaFar{2000.0f, 700.0f, 1.0f, 1000.0f};   // D_003218A0
            const float sTarget = std::max(r.sTracked + (fNear ? 2500.0f : 350.0f), r.s + (fNear ? 100.0f : 8.0f));
            r.s = cmm::GSmoothA(r.s, r.ds, sTarget, dt, fNear ? &s_smpaNear : &s_smpaFar, &r.ds);
        }
        else
        {
            const float g = GSuck3();   // 0x171C98(D_0036BD20): the dynamic difficulty
            r.s += (1.0f + g * -0.3f) * r.speed * dt;   // D_003218B0 {1, -0.3, 0}
        }
    }
    else r.s += r.speed * dt;
    static int s_c = 0;
    if (FPrint() && ++s_c % 60 == 0)
        std::printf("racer3: %d s %.0f (tracked %.0f) of %.0f car %d\n", plo->oid, r.s, r.sTracked, sMax, fCar);
    if (sMax <= r.s)
    {
        StopRacer(r);
        if (FPrint()) std::printf("racer3: %d at the end: message 5\n", plo->oid);
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
        return;
    }
    const glm::vec3 pos = PosRunner(r);
    PlaceRunner(r, pos);
    if (r.shape && sly3::FLoInWorld3(r.shape) && FPointInShape(r.shape, pos))
    {
        StopRacer(r);
        if (FPrint()) std::printf("racer3: %d in its shape %d: message 6\n", plo->oid, r.shape->oid);
        HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
    }
}

struct INITRACER3
{
    INITRACER3()
    {
        RegisterLoUpdate(FN3_TAG | 0x183FA8, UpdateRacer);
        s_pfnExtraPrev = g_pfnExtraDraws;
        g_pfnExtraDraws = CollectRacerDraws;
        // 1854: the runner's point (getter 0x184630, setter 0x1846C8)
        RegisterObjectGetter3(1854, [](LO* plo) {
            CRef ref;
            if (RACER3* r = Pracer(plo)) ref.SetVector(PosRunner(*r));
            return ref;
        });
        RegisterObjectSetter3(1854, [](LO* plo, const CRef& ref) {
            RACER3* r = Pracer(plo);
            if (!r || ref.m_tagk != TAGK_Vector) return;
            r->s = SNearest(*r, ref.GetVector());
            if (FPrint()) std::printf("racer3: %d set to s %.0f\n", plo->oid, r->s);
            if (r->s < SLength(*r)) StartRacer(*r);
        });
        // 1858 / 1859 (thunks 0x1104B8 / 0x110530): start / stop
        RegisterObjectMethod3(0x1104B8, [](LO* plo, const std::vector<CRef>&) {
            if (RACER3* r = Pracer(plo)) StartRacer(*r);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x110530, [](LO* plo, const std::vector<CRef>&) {
            if (RACER3* r = Pracer(plo)) StopRacer(*r);
            return CRef(TAGK_Void);
        });
    }
} s_initracer3;
}  // namespace

// orig3 0x183E00 (class 0x98's post-load): the course samples made at the world's start (natively at the first update
// or draw otherwise)
void PostLoadRacer3(LO* plo)
{
    if (!plo || plo->Cid() != 0x98) return;
    if (RACER3* r = Pracer(plo)) SampleRacer(*r);
}
