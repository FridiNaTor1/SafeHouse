#include <execinfo.h>
#include "worldglobals.h"
// ASEG evaluation and ASEGA (Sly 2 0x12B3B0-0x131E18 and 0x230CF0). Specification: Sly2-Reference/Sly2-Release/game/aseg.cpp,
// asega.cpp, m_12B3B0_p3.cpp (oracle-verified) and the decomp's func_00130D30, 00131100, 00131A90, 00230CF0
// (matching C). The tracks' Setup gives a transform record the segment of keys around its parameter; the record
// evaluates the spline itself every frame (xf.cpp, mode 5 / 7).

#include "asega.h"
#include "task.h"

#include "anim.h"
#include "animfind.h"
#include "clock.h"
#include "sw.h"
#include "xfmath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

void PauseGame(int grf);   // ai.cpp: orig 0x20EFC0
void ResumeGame(int grf);  // ai.cpp: orig 0x20F030

std::vector<ASEGA*> g_apasega;
WORLD_TABLE(g_apasega);   // (PC: moved aside while a kept world loads, worldglobals.h)
constinit void (*g_pfnAsegaEventOther)(ASEGA*, ASEGEV*) = nullptr;
constinit void (*g_pfnAsegaScript)(ASEGA* pasega, int symid, int n) = nullptr;
constinit bool g_fAsegaEventsMuted = false;
constinit void (*g_pfnLoMessage)(LO*, int, void*) = nullptr;
constinit void (*g_pfnBasicScript)(BASIC*, int) = nullptr;
constinit float* (*g_pfnPgAsegParam)(LO*, int) = nullptr;
constinit void (*g_pfnAsegParamUse)(LO*, int, bool) = nullptr;
int g_cAsegaEventDropped = 0;

namespace
{
// The original's DL iterators (DLI, g_pdliFirst 0x2DD2FC): a removal before an iterator's position moves it back,
// entries appended during the walk are reached.
std::vector<size_t*> s_apiAsega;

// Optional numeric animation observer, selected by ASEG object IDs. This runs before the
// update's stopped-speed check so authored pauses remain visible in reference comparisons.
void TraceAsega(const char* event, const ASEGA* pasega)
{
    static const std::vector<int> ids = [] {
        std::vector<int> result;
        const char* value = std::getenv("SLY2N_ASEGATRACE");
        while (value && *value)
        {
            char* end = nullptr;
            long id = std::strtol(value, &end, 10);
            if (end == value) break;
            result.push_back(static_cast<int>(id));
            if (*end != ',') break;
            value = end + 1;
        }
        return result;
    }();
    if (ids.empty() || !pasega->paseg ||
        std::find(ids.begin(), ids.end(), pasega->paseg->oid) == ids.end()) return;
    const ALO* target = pasega->palo;
    glm::vec3 pos(0.0f);
    if (target && !target->axf.empty()) pos = glm::vec3(target->axf[0]->matWorld[3]);
    std::fprintf(stderr, "ASEGATRACE,%s,%.9g,%.9g,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                 event, g_clock.t, g_clock.dt, pasega->paseg->oid, target ? target->oid : -1,
                 pasega->t, pasega->svtLocal, pasega->svt, pos.x, pos.y, pos.z);
}
}

namespace
{
constexpr float kEpsNear = 1e-4f;

bool FNear3(const glm::vec4& a, const glm::vec4& b)
{
    return FFloatsNear(a.x, b.x, kEpsNear) && FFloatsNear(a.y, b.y, kEpsNear) && FFloatsNear(a.z, b.z, kEpsNear);
}

// The segment's kind: 2 (fixed) when both ends are the same point with zero slopes, else 1 (spline).
void SetSegmentKind(XF* pxf)
{
    const glm::vec4 zero(0.0f);
    bool f = FNear3(pxf->aseg[0], pxf->aseg[2]);
    if (f) f = FNear3(zero, pxf->aseg[1]);
    if (f) f = FNear3(zero, pxf->aseg[3]);
    pxf->state = f ? 2 : 1;
}

void SetXyz(glm::vec4& v, const glm::vec3& p) { v = glm::vec4(p, v.w); }

ALO* PaloFind(ASEG* paseg, ALO* palo, OID oid)
{
    // orig 0x15E938(G, 1, oid, palo) / (G, 4, oid, paseg)
    LO* plo = palo ? PloFindSwChild(palo, oid) : PloFindSwNearest(paseg, oid);
    return plo && FCidDerivedFrom(plo->Cid(), 0x30) ? static_cast<ALO*>(plo) : nullptr;
}



// The listener list's message (orig 0x131738: the ASEGA's object's vt slot 5, then each listener)
void NotifyAsega(ASEGA* pasega, int msgk)
{
    AddRefAsega(pasega);
    if (pasega->palo && g_pfnLoMessage) g_pfnLoMessage(pasega->palo, msgk, pasega);
    const std::vector<ASEGALISTENER*> a = pasega->aplistener;
    for (ASEGALISTENER* pl : a) pl->OnAsegaMsg(pasega, msgk);
    ReleaseAsega(pasega);
}

// orig 0x00131990 (the ASEGA's hands-off: the game paused with 3 while on, the pad locked)
void SetAsegaHandsOff(ASEGA* pasega, int f)
{
    if (FAsegaRetracted(pasega)) return;
    if (f == pasega->fHandsOff) return;
    if (f) PauseGame(3);
    else ResumeGame(3);
    pasega->fHandsOff = f;
}

void Script(ASEGA* pasega, int k, int n = 0)
{
    // orig vt slot 3 (0x1331F8) of the ASEGA: its script handler k (2 start, 3 retract, 4 event 12 with its word)
    if (g_fAsegaEventsMuted) return;   // (PC: a guest puppet's clip, asega.h)
    if (g_pfnAsegaScript) g_pfnAsegaScript(pasega, k, n);
}

void DropEvent(ASEGA* pasega, ASEGEV* pev)
{
    if (g_pfnAsegaEventOther) g_pfnAsegaEventOther(pasega, pev);
    else ++g_cAsegaEventDropped;
}
}  // namespace

// orig 0x0012FE58: a channel's object and its record (vt slot 51 of the sub-id)
void GetAsegChannelTarget(ASEG* paseg, ALO* palo, const ASEGCHN& chn, ALO** ppalo, XF** ppxf)
{
    *ppalo = PaloFind(paseg, palo, chn.oid);
    *ppxf = *ppalo ? PxfFindAlo(*ppalo, chn.isub) : nullptr;
}

ASEGD* PasegdOf(ASEG* paseg)
{
    while (paseg->ploCloneOf && paseg->asegd.achn.empty() && paseg->asegd.achg.empty() &&
           paseg->asegd.aaev[0].empty() && paseg->asegd.aaev[1].empty() && paseg->asegd.aaev[2].empty())
        paseg = static_cast<ASEG*>(paseg->ploCloneOf);
    return &paseg->asegd;
}

// ---- tracks (0x12B3B0 module) ----

void TRKP::Setup(XF* pxf) const { (void)pxf; }  // (kind 5 is the blend's track: blend.cpp TRKPBL)
void TRKP::Eval(glm::vec4* ppos, glm::vec4* pvel, float, float) const
{
    if (ppos) *ppos = glm::vec4(0.0f);
    if (pvel) *pvel = glm::vec4(0.0f);
}
void TRKG::Eval(float* pg, float* pdg, float, float) const
{
    if (pg) *pg = 0.0f;
    if (pdg) *pdg = 0.0f;
}

// orig: 0x0012B520 (the segment is the constant position, kind 2)
void TRKPC::Setup(XF* pxf) const
{
    pxf->state = 2;
    ResetXfParams(pxf);
    SetXyz(pxf->aseg[0], pos);
    pxf->aseg[2] = pxf->aseg[0];
    SetXyz(pxf->aseg[1], glm::vec3(0.0f));
    SetXyz(pxf->aseg[3], glm::vec3(0.0f));
}

// orig: 0x0012B590
void TRKPC::Eval(glm::vec4* ppos, glm::vec4* pvel, float, float) const
{
    if (ppos) SetXyz(*ppos, pos);
    if (pvel) SetXyz(*pvel, glm::vec3(0.0f));
}

// orig: 0x0012B6D8 / 0x0012BB70: for (i = 1; i < c - 1; i++) if (g < t(i)) break
int TRKPK::IkeyFind(float g) const
{
    const int cm1 = static_cast<int>(akey.size()) - 1;
    int i = 1;
    while (i < cm1 && !(g < akey[i].t)) ++i;
    return std::min(i, std::max(cm1, 0));  // (a single key: the original reads past it)
}

// orig: 0x0012B738 / 0x0012BBD0: the keys around the record's parameter
void TRKPK::Setup(XF* pxf) const
{
    if (akey.empty()) return;
    const int i = IkeyFind(pxf->G());
    const KEYP& k0 = akey[std::max(i - 1, 0)];
    const KEYP& k1 = akey[i];
    pxf->g0 = k0.t;
    SetXyz(pxf->aseg[0], k0.pos);
    pxf->aseg[1] = glm::vec4(k0.vOut, 0.0f);  // 0x30 keys: the tangent out (+0x20); 0x20 keys: the tangent
    pxf->g1 = k1.t;
    SetXyz(pxf->aseg[2], k1.pos);
    pxf->aseg[3] = glm::vec4(k1.vIn, 0.0f);
    SetSegmentKind(pxf);
}

// orig: 0x0012B9D0 / 0x0012BE68
void TRKPK::Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const
{
    if (akey.empty()) return;
    const int i = IkeyFind(g);
    const KEYP& k0 = akey[std::max(i - 1, 0)];
    const KEYP& k1 = akey[i];
    const float dt = k1.t - k0.t;
    EvalHermiteVector(glm::vec4(k0.pos, 0.0f), glm::vec4(k0.vOut, 0.0f), glm::vec4(k1.pos, 0.0f),
                      glm::vec4(k1.vIn, 0.0f), ppos, pvel, nullptr, dt, g - k0.t, dtScale);
}

// orig: 0x0012C0E0 / 0x0012C708
int TRKPA::IkeyFind(float g) const
{
    const int cm1 = static_cast<int>(at.size()) - 1;
    int i = 1;
    while (i < cm1 && !(g < at[i])) ++i;
    return std::min(i, std::max(cm1, 0));
}

// orig: 0x0012C140 / 0x0012C768
void TRKPA::Setup(XF* pxf) const
{
    int i = 0;
    if (!at.empty())
    {
        i = IkeyFind(pxf->G());
        pxf->g0 = at[std::max(i - 1, 0)];
        pxf->g1 = at[i];
    }
    else
        ResetXfParams(pxf);  // (the key index is the original's leftover $a2: no axis has keys then)
    const int ofsOut = cfKey == 3 ? 2 : 1;  // {g, dgIn, dgOut} / {g, dg}
    const int ofsIn = 1;
    for (int j = 0; j < 3; ++j)
    {
        if (!aag[j].empty())
        {
            const float* k0 = &aag[j][static_cast<size_t>(std::max(i - 1, 0)) * cfKey];
            const float* k1 = &aag[j][static_cast<size_t>(i) * cfKey];
            pxf->aseg[0][j] = k0[0];
            pxf->aseg[1][j] = k0[ofsOut];
            pxf->aseg[2][j] = k1[0];
            pxf->aseg[3][j] = k1[ofsIn];
        }
        else
        {
            pxf->aseg[0][j] = v[j];
            pxf->aseg[1][j] = 0.0f;
            pxf->aseg[2][j] = v[j];
            pxf->aseg[3][j] = 0.0f;
        }
    }
    SetSegmentKind(pxf);
}

// orig: 0x0012C440 / 0x0012CA68
void TRKPA::Eval(glm::vec4* ppos, glm::vec4* pvel, float g, float dtScale) const
{
    if (ppos) SetXyz(*ppos, v);
    if (pvel) SetXyz(*pvel, glm::vec3(0.0f));
    if (at.empty()) return;
    const int i = IkeyFind(g);
    const int i0 = std::max(i - 1, 0);
    const int ofsOut = cfKey == 3 ? 2 : 1;
    for (int j = 0; j < 3; ++j)
    {
        if (aag[j].empty()) continue;
        const float t0 = at[i0];
        const float* k0 = &aag[j][static_cast<size_t>(i0) * cfKey];
        const float* k1 = &aag[j][static_cast<size_t>(i) * cfKey];
        EvalHermite(ppos ? &(*ppos)[j] : nullptr, pvel ? &(*pvel)[j] : nullptr, at[i] - t0, g - t0, dtScale, k0[0],
                    k0[ofsOut], k1[0], k1[1]);
    }
}

// orig: 0x0012CC98
void TRKGC::Eval(float* pg, float* pdg, float, float) const
{
    if (pg) *pg = g;
    if (pdg) *pdg = 0.0f;
}

// orig: 0x0012CDA0 (the key search runs to the last key: for (i = 1; i < c; i++) if (g < t(i)) break)
void TRKGK::Eval(float* pg, float* pdg, float t, float dtScale) const
{
    const int c = static_cast<int>(akey.size());
    if (c == 0) return;
    int i = 1;
    while (i < c && !(t < akey[i].t)) ++i;
    i = std::min(i, c - 1);  // (past the last key the original reads the next 16 bytes)
    const KEYG& k1 = akey[i];
    const KEYG& k0 = akey[std::max(i - 1, 0)];
    EvalHermite(pg, pdg, k1.t - k0.t, t - k0.t, dtScale, k0.g, k0.dgOut, k1.g, k1.dgIn);
}

// ---- the channel controllers (vtable 0x2CAF08) ----

// orig: 0x00131C40
void CTRLASEG::Start()
{
    if (ptrkg)
    {
        // the scalar channel eases the joint +0xA8 of its record (+0xD0 the track, +0xD4 the time)
        if (auto* ppair = static_cast<ALOPAIR*>(pxf->pvA8))
        {
            ppair->ptrkgWeight = ptrkg;
            ppair->ptWeight = &pasega->t;
        }
        return;
    }
    CTRL::Start();
}

// orig: 0x00131C90
void CTRLASEG::End()
{
    if (ptrkg)
        if (auto* ppair = static_cast<ALOPAIR*>(pxf->pvA8))
        {
            ppair->ptrkgWeight = nullptr;
            ppair->ptWeight = nullptr;
        }
}

// orig: 0x00131CD8
void CTRLASEG::Fetch()
{
    if (ptrkp)
    {
        pxf->pg = &pasega->t;
        pxf->pdgScale = &pasega->svt;
        ptrkp->Setup(pxf);
        return;
    }
    pxf->state = 0;
    SetXfMode(palo, pxf, 0);
    ResetXfParams(pxf);
}

// orig: 0x00131D68 (the track at the ASEGA's time + g x its speed (the caller's $f12: a lead time), wrapped)
void CTRLASEG::Query(glm::vec4* ppos, glm::vec4* pvel, float g)
{
    if (!ptrkp)
    {
        CTRL::Query(ppos, pvel, g);
        return;
    }
    float dt = pasega->svt;
    float t = pasega->t + g * dt;
    FWrapAsegaTime(pasega, &t, &dt);
    ptrkp->Eval(ppos, pvel, t, dt);
}

// ---- ASEGA ----

// orig: 0x00131968
bool FAsegaRetracted(const ASEGA* pasega) { return pasega->fRetracted; }

// orig: 0x00131A00
void SetAsegaSpeed(ASEGA* pasega, float svt)
{
    pasega->svtLocal = svt;
    pasega->svt = svt * pasega->svtAseg * pasega->svtSuck;
    TraceAsega("speed", pasega);
    if (static const char* s_szObj = std::getenv("SLY2N_APPLYOBJ"); s_szObj && pasega->palo && pasega->palo->oid == std::atoi(s_szObj))
        std::fprintf(stderr, "applyobj %d: aseg %d speed %.3f clock %.3f\n", pasega->palo->oid, pasega->paseg ? pasega->paseg->oid : -1, svt, g_clock.t);
}

// orig: 0x00131A20
void SetAsegaAsegSpeed(ASEGA* pasega, float svt)
{
    pasega->svtAseg = svt;
    pasega->svt = pasega->svtLocal * svt * pasega->svtSuck;
}

// orig: 0x00131A90
void SetAsegaSeekTime(ASEGA* pasega, float g)
{
    for (CTRLASEG* pl = pasega->pctrlFirst; pl; pl = pl->pnextAsega)
    {
        if (pl->FFirst() && pl->pxf->mode == XFM_Seek)
        {
            SetXfMode(pl->palo, pl->pxf, XFM_SeekTimed);
            pl->pxf->tTarget = g;
        }
    }
}

// orig: 0x00130EB0
void UpdateAsegaIeaCur(ASEGA* pasega)
{
    const std::vector<ASEGEV>& aev = PasegdOf(pasega->paseg)->aaev[1];
    const int c = static_cast<int>(aev.size());
    if (c == 0) return;
    if (0.0f <= pasega->svt)
    {
        int i = 0;
        while (i < c && !(pasega->t <= aev[i].t)) ++i;
        pasega->iea = i;
    }
    else
    {
        int i = c - 1;
        while (i >= 0 && !(aev[i].t <= pasega->t)) --i;
        pasega->iea = i + 1;
    }
}

// orig: 0x001317D8
bool FWrapAsegaTime(ASEGA* pasega, float* pt, float* psvt)
{
    const float tMax = PasegdOf(pasega->paseg)->tMax;
    const float t = *pt;
    bool f = false;
    auto pingPong = [&](float t2) {
        *psvt = std::fabs(*psvt);
        pasega->svtLocal = std::fabs(pasega->svtLocal);
        if (tMax <= *pt)
        {
            *pt = t2 - *pt;
            *psvt = -*psvt;
            pasega->svtLocal = -pasega->svtLocal;
        }
    };
    if (tMax <= t)
    {
        if (0.0f < *psvt) f = true;
        if (pasega->grfapl & 8) *pt = std::fmod(t, tMax);
        else if (pasega->grfapl & 0x10)
        {
            const float t2 = tMax + tMax;
            *pt = std::fmod(t, t2);
            pingPong(t2);
        }
        else
        {
            *pt = tMax;
            *psvt = std::min(*psvt, 0.0f);
        }
    }
    else if (t < 0.0f)
    {
        if (*psvt < 0.0f) f = true;
        if (pasega->grfapl & 8) *pt = GModPositive(t, tMax);
        else if (pasega->grfapl & 0x10)
        {
            const float t2 = tMax + tMax;
            *pt = std::fmod(-t, t2);
            pingPong(t2);
        }
        else
        {
            *pt = 0.0f;
            *psvt = std::max(*psvt, 0.0f);
        }
    }
    return f;
}

// orig: 0x00131290
void FireAsegaEvent(ASEGA* pasega, ASEGEV* pev)
{
    if (g_fAsegaEventsMuted) { pasega->t = pev->t; return; }   // (PC: a guest puppet's clip, asega.h)
    if (static const char* s_sz = std::getenv("SLY2N_ASEGEVOWNER"); s_sz && pasega->palo && pasega->palo->oid == std::atoi(s_sz))  // (debug)
        std::printf("asegevfire t %.3f clip %d owner %d kind %d n %d %d %d %d\n", g_clock.t, pasega->paseg ? pasega->paseg->oid : -1, pasega->palo->oid,
                    pev->kind, pev->an[0], pev->an[1], pev->an[2], pev->an[3]);
    AddRefAsega(pasega);
    pasega->t = pev->t;
    ASEG* paseg = pasega->paseg;
    ASEGD* pasegd = PasegdOf(paseg);
    bool fBack = false;  // played backward over an event of the timed list
    if (pasega->svt < 0.0f)
    {
        const auto& a = pasegd->aaev[1];
        fBack = !a.empty() && pev >= &a.front() && pev <= &a.back();
    }
    const int k = pev->kind;
    static const bool s_fDebug = std::getenv("SLY2N_SNAPDEBUG") != nullptr;
    if (s_fDebug)
        std::fprintf(stderr, "  event aseg %d obj %d t %.3f kind %d an %d %d %d %d\n", paseg->oid, pasega->palo ? pasega->palo->oid : -1,
                     pev->t, k, pev->an[0], pev->an[1], pev->an[2], pev->an[3]);
    switch (k)
    {
    case 4:
    {
        // another object of the ASEG's namespace: an ASEG is applied, a cid 0x72 object started (0x1DFD58)
        LO* plo = PloFindSwNearest(pasega->palo ? static_cast<LO*>(pasega->palo) : static_cast<LO*>(paseg), pev->an[0]);
        if (plo && plo->Cid() == 0x6D) ApplyAseg(static_cast<ASEG*>(plo), nullptr, 0.0f, 1.0f, 0, nullptr);
        else if (plo && plo->Cid() == 0x72) DropEvent(pasega, pev);
        break;
    }
    case 5: SetAsegaTime(pasega, 0, pev->ag[0], pev->ag[1]); break;
    case 6: RetractAsega(pasega); break;
    case 9: case 10: SetAsegaHandsOff(pasega, static_cast<int>(k == 9) ^ static_cast<int>(fBack)); break;
    case 11:
        // message 9 {pasega, t, n} to the ASEGA's object and listeners
        if (pasega->palo && g_pfnLoMessage)
        {
            ASEGMSG9 msg;
            msg.pasega = pasega;
            msg.t = pev->t;
            msg.n = pev->an[0];
            msg.pev = pev;
            g_pfnLoMessage(pasega->palo, 9, &msg);
        }
        break;
    case 12: Script(pasega, 4, pev->an[0]); break;
    case 21: SnapAsegaXfs(pasega, pev->an[0] != 0); break;
    default: DropEvent(pasega, pev); break;  // 0-3, 7, 8, 13-20, 22-24: other systems
    }
    ReleaseAsega(pasega);
}

// orig: 0x00130F88
void FireAsegaEvents(ASEGA* pasega)
{
    std::vector<ASEGEV>& aev = PasegdOf(pasega->paseg)->aaev[1];
    if (aev.empty()) return;
    AddRefAsega(pasega);
    pasega->tEvent = pasega->t;
    while (!FAsegaRetracted(pasega))
    {
        const int i = pasega->iea;
        const float t = pasega->tEvent;
        const int c = static_cast<int>(aev.size());
        if (i < c)
        {
            const float tEv = aev[i].t;
            if (tEv < t || (t == tEv && 0.0f < pasega->svt))
            {
                pasega->iea = i + 1;
                FireAsegaEvent(pasega, &aev[i]);
                continue;
            }
        }
        if (i <= 0)
        {
            pasega->t = t;
            break;
        }
        const float tPrev = aev[i - 1].t;
        if (!(t < tPrev))
        {
            if (!(t == tPrev) || !(pasega->svt < 0.0f))
            {
                pasega->t = t;
                break;
            }
        }
        pasega->iea = i - 1;
        FireAsegaEvent(pasega, &aev[i - 1]);
    }
    ReleaseAsega(pasega);
}

// orig: 0x00131100 (from the start: the timed events up to the time of the kinds that hold a state)
void FireAsegaStartEvents(ASEGA* pasega)
{
    std::vector<ASEGEV>& aev = PasegdOf(pasega->paseg)->aaev[1];
    if (aev.empty()) return;
    AddRefAsega(pasega);
    pasega->tEvent = pasega->t;
    while (!FAsegaRetracted(pasega))
    {
        const int c = static_cast<int>(aev.size());
        const int i = pasega->iea;
        const float t = pasega->tEvent;
        ASEGEV* pe = nullptr;
        if (i < c && aev[i].t <= t && 0.0f <= pasega->svt)
        {
            pe = &aev[i];
            pasega->iea = i + 1;
        }
        else if (pasega->iea > 0 && t <= aev[pasega->iea - 1].t && pasega->svt < 0.0f)
        {
            pasega->iea = pasega->iea - 1;
            pe = &aev[pasega->iea];
        }
        else
        {
            pasega->t = t;
            break;
        }
        switch (pe->kind)
        {
        case 7: case 8: case 11: case 13: case 14: case 15: case 16: case 21: case 22: case 23: case 24:
            FireAsegaEvent(pasega, pe);
            break;
        default:
            break;
        }
    }
    ReleaseAsega(pasega);
}

// orig: 0x00230CF0
void SnapAsegaXfs(ASEGA* pasega, bool fSeek)
{
    if (FAsegaRetracted(pasega)) return;
    std::vector<ALO*> apalo;
    for (CTRLASEG* c = pasega->pctrlFirst; c; c = c->pnextAsega)
    {
        if (!c->FFirst()) continue;
        XF* p = c->pxf;
        ALO* q = c->palo;
        RefreshXf(q, p);
        switch (p->mode)
        {
        case 0: case 13: continue;
        case 7: case 8: case 9: case 11:
            if (!fSeek) continue;
            break;
        default: break;
        }
        if (std::find(apalo.begin(), apalo.end(), q) == apalo.end() && apalo.size() < 16) apalo.push_back(q);
        glm::vec4 v;
        switch (p->state)
        {
        case 5: v = glm::vec4(EulerFromMat(glm::mat3(glm::vec3(p->aseg[0]), glm::vec3(p->aseg[1]),
                                                     glm::vec3(p->aseg[2]))), 0.0f);
            break;
        default: v = VecXfTarget(p); break;
        }
        p->mat = MatXfWithValue(p, v);
        p->vel = glm::vec4(0.0f);
        p->nStamp = ++g_cXfStamp;
    }
    // vt slot 18 (0x22FBE0): the world matrices, then the children's
    auto worlds = [](auto&& self, ALO* palo) -> void {
        UpdateAloXfWorlds(palo);
        for (LO* pc : palo->aploChild)
            if (FCidDerivedFrom(pc->Cid(), 0x30)) self(self, static_cast<ALO*>(pc));
    };
    for (ALO* palo : apalo) worlds(worlds, palo);
    for (ASEGAL& l : pasega->alink)
        if (l.ptrkg && l.pg) l.ptrkg->Eval(l.pg, nullptr, pasega->t, pasega->svt);
}

// orig: 0x00130D30
void SetAsegaTime(ASEGA* pasega, int k, float g, float svt)
{
    if (FAsegaRetracted(pasega)) return;
    pasega->svtLocal = svt;
    pasega->svt = svt * pasega->svtAseg * pasega->svtSuck;
    float t = pasega->t;
    switch (k)
    {
    case 0: t = g; break;
    case 1: t = pasega->t + g; break;
    case 2: t = PasegdOf(pasega->paseg)->tMax + g; break;
    default: break;
    }
    if (t != pasega->t)
    {
        pasega->t = t;
        FWrapAsegaTime(pasega, &pasega->t, &pasega->svt);
        UpdateAsegaIeaCur(pasega);
        for (CTRLASEG* ps = pasega->pctrlFirst; ps; ps = ps->pnextAsega)
        {
            if (!ps->FFirst()) continue;
            XF* pr = ps->pxf;
            const float v = pr->G();
            if (v < pr->g0 || pr->g1 < v) ps->Fetch();
            HoldXfMode(ps->palo, pr);
        }
        SnapAsegaXfs(pasega, false);
    }
    pasega->tEvent = pasega->t;
}

// orig: 0x0012F9B8 ApplyAseg
void ApplyAseg(ASEG* paseg, ALO* palo, float t, float svt, int grfapl, ASEGA** ppasega)
{
    ASEGD* pasegd = PasegdOf(paseg);
    ASEGA* pasega = new ASEGA();
    pasega->paseg = paseg;
    pasega->palo = palo;
    pasega->t = t;
    pasega->grfapl = grfapl;
    pasega->svtAseg = paseg->svt;
    const float x = GSuck();  // 0x154C90 (task.cpp)
    float g = paseg->gSuck0 + x * (paseg->gSuck1 + x * paseg->gSuck2);
    if (g < paseg->gSuckMin) g = paseg->gSuckMin;
    else if (paseg->gSuckMax < g) g = paseg->gSuckMax;
    pasega->svtSuck = g;
    SetAsegaSpeed(pasega, svt);
    g_apasega.push_back(pasega);
    paseg->apasega.push_back(pasega);
    AddRefAsega(pasega);  // (the lists' reference)
    AddRefAsega(pasega);  // (the guard)
    if (ppasega) *ppasega = pasega;
    const bool fSnapSeek = (grfapl >> 2) & 1;
    const bool fEventsFromStart = grfapl & 2;
    const int kLink = (grfapl & 1) ? XFM_Seek : XFM_Snap;
    static const int s_oidDebug = std::getenv("SLY2N_APPLYDEBUG") ? std::atoi(std::getenv("SLY2N_APPLYDEBUG")) : -2;  // (-2: none; -1 is an oid)
    for (const ASEGCHN& chn : pasegd->achn)
    {
        ALO* po;
        XF* pxf;
        if (paseg->oid == s_oidDebug)
        {
            GetAsegChannelTarget(paseg, palo, chn, &po, &pxf);
            std::fprintf(stderr, "apply aseg %d: channel oid %d sub %d disabled %d scalar %d -> alo %d xf %p (kind %d, ref2 %d) grf %d t %.3f\n",
                         paseg->oid, chn.oid, chn.isub, chn.fDisabled, chn.fScalar, po ? po->oid : -1, (void*)pxf, pxf ? pxf->kind : -1,
                         po && pxf == po->apxfRef[2], grfapl, t);
            // (the channel's position track: its kind and keys)
            if (const TRKPC* pc = dynamic_cast<const TRKPC*>(chn.ptrkp.get()))
                std::fprintf(stderr, "  trkp const (%.2f %.2f %.2f)\n", pc->pos.x, pc->pos.y, pc->pos.z);
            else if (const TRKPK* pk = dynamic_cast<const TRKPK*>(chn.ptrkp.get()))
                for (const KEYP& k : pk->akey)
                    std::fprintf(stderr, "  trkp key t %.3f (%.2f %.2f %.2f)\n", k.t, k.pos.x, k.pos.y, k.pos.z);
            else if (const TRKPA* pa = dynamic_cast<const TRKPA*>(chn.ptrkp.get()))
            {
                std::fprintf(stderr, "  trkp axes v (%.2f %.2f %.2f) keys %zu:", pa->v.x, pa->v.y, pa->v.z, pa->at.size());
                for (size_t i = 0; i < pa->at.size(); ++i)
                {
                    std::fprintf(stderr, " t%.3f", pa->at[i]);
                    for (int ax = 0; ax < 3; ++ax)
                        if (!pa->aag[ax].empty()) std::fprintf(stderr, " %c%.2f", 'x' + ax, pa->aag[ax][i * pa->cfKey]);
                }
                std::fprintf(stderr, "\n");
            }
        }
        if (chn.fDisabled) continue;
        GetAsegChannelTarget(paseg, palo, chn, &po, &pxf);
        if (!pxf) continue;
        if (static const char* s_szObj = std::getenv("SLY2N_APPLYOBJ"); s_szObj && po && po->oid == std::atoi(s_szObj))
            std::fprintf(stderr, "applyobj %d: aseg %d on %d t %.3f svt %.3f grf %d kind %d clock %.3f\n", po->oid, paseg->oid, palo ? palo->oid : -1, t, svt,
                         grfapl, pxf->kind, g_clock.t);
        if (static const char* s_szObj = std::getenv("SLY2N_APPLYOBJ"); s_szObj && po && po->oid == std::atoi(s_szObj) && chn.ptrkp)
        {
            std::fprintf(stderr, "  channel oid %d sub %d track kind %d\n", chn.oid, chn.isub, chn.ptrkp->kind);
            if (const auto* pk = dynamic_cast<const TRKPK*>(chn.ptrkp.get()))
                for (const KEYP& k : pk->akey) std::fprintf(stderr, "   key t %.3f pos %.1f %.1f %.1f\n", k.t, k.pos.x, k.pos.y, k.pos.z);
            if (const auto* pa = dynamic_cast<const TRKPA*>(chn.ptrkp.get()))
            {
                std::fprintf(stderr, "   axes v %.1f %.1f %.1f keys %zu:", pa->v.x, pa->v.y, pa->v.z, pa->at.size());
                for (float t : pa->at) std::fprintf(stderr, " %.3f", t);
                std::fprintf(stderr, " | x %zu y %zu z %zu\n", pa->aag[0].size(), pa->aag[1].size(), pa->aag[2].size());
            }
        }
        if (static const char* s_szObj = std::getenv("SLY2N_APPLYOBJ"); s_szObj && po && po->oid == std::atoi(s_szObj) && std::getenv("SLY2N_APPLYBT"))
        {
            void* apv[16];
            backtrace_symbols_fd(apv, backtrace(apv, 16), 2);
        }
        // TODO(physics): 0x1F8668(po, 0)
        CTRLASEG* plink = new CTRLASEG();
        plink->kind = 0;
        if (chn.fScalar) plink->ptrkg = chn.ptrkg.get();
        else plink->ptrkp = chn.ptrkp.get();
        plink->palo = po;
        plink->pxf = pxf;
        plink->mode = kLink;
        plink->prio = paseg->prio;
        plink->pasega = pasega;
        plink->pnextAsega = pasega->pctrlFirst;
        pasega->pctrlFirst = plink;
        AddXfCtrl(po, pxf, plink);
    }
    for (const ASEGCHG& chg : pasegd->achg)
    {
        LO* plo = palo ? PloFindSwChild(palo, chg.n) : PloFindSwNearest(paseg, chg.n);  // orig 0x130010
        float* pg = plo && g_pfnPgAsegParam ? g_pfnPgAsegParam(plo, chg.kind) : nullptr;  // vt slot 43
        if (!pg) continue;
        pasega->alink.push_back(ASEGAL{chg.ptrkg.get(), pg, plo, chg.kind});
        if (g_pfnAsegParamUse) g_pfnAsegParamUse(plo, chg.kind, true);  // vt slot 44 (kind): the parameter in use
    }
    SetAsegaHandsOff(pasega, paseg->fHandsOff);
    pasega->tEvent = t;
    for (ASEGEV& ev : pasegd->aaev[0]) FireAsegaEvent(pasega, &ev);
    pasega->t = pasega->tEvent;
    SnapAsegaXfs(pasega, fSnapSeek);
    if (fEventsFromStart)
    {
        if (pasega->svt < 0.0f) pasega->iea = static_cast<int>(pasegd->aaev[1].size());
        FireAsegaStartEvents(pasega);
    }
    else
        UpdateAsegaIeaCur(pasega);
    Script(pasega, 2);
    ReleaseAsega(pasega);
}

// orig: 0x00130488
ASEGA* PasegaApplyAseg(ASEG* paseg, ALO* palo, float t, float svt, int grfapl)
{
    ASEGA* pasega = nullptr;
    ApplyAseg(paseg, palo, t, svt, grfapl, &pasega);
    return pasega;
}

// orig: 0x00130658
void RetractAsegaPp(ASEGA** ppasega)
{
    if (!*ppasega) return;
    RetractAsega(*ppasega);
    ReleaseAsega(*ppasega);
    *ppasega = nullptr;
}

// orig: 0x001305B8 ApplyAsegCur
void ApplyAsegCur(ASEG* paseg, ALO* palo, float t, float svt, int grfapl, ASEGA** ppasega)
{
    RetractAsegaPp(ppasega);
    if (!paseg) return;
    ApplyAseg(paseg, palo, t, svt, grfapl, ppasega);
    AddRefAsega(*ppasega);  // (the holder's reference)
}

// orig: 0x001304E0 EnsureAseg
void EnsureAseg(ASEG* paseg, ALO* palo, int k, float t, float svt, int grfapl, ASEGA** ppasega)
{
    ASEGA* pasegaLocal = nullptr;
    ASEGA** pp = ppasega ? ppasega : &pasegaLocal;
    if (ASEGA* p = PasegaFindAseg(paseg, palo)) *pp = p;
    else ApplyAseg(paseg, palo, 0.0f, 1.0f, grfapl, pp);
    SetAsegaTime(*pp, k, t, svt);
}

// orig: 0x00130598
ASEGA* PasegaEnsureAseg(ASEG* paseg, ALO* palo, int k, float t, float svt, int grfapl)
{
    ASEGA* pasega = nullptr;
    EnsureAseg(paseg, palo, k, t, svt, grfapl, &pasega);
    return pasega;
}

// orig: 0x00131A40 SetAsegaPriority
void SetAsegaPriority(ASEGA* pasega, int prio)
{
    for (CTRLASEG* pl = pasega->pctrlFirst; pl; pl = pl->pnextAsega)
    {
        pl->prio = prio;
        SortXfCtrls(pl->palo, pl->pxf);
    }
}

// orig: 0x00131B10
void DemoteAsegaOnXf(ASEGA* pasega, ALO* palo, XF* pxf)
{
    for (CTRL* p = pxf->pctrlFirst; p; p = p->pnext)
        if (p->kind == 0 && static_cast<CTRLASEG*>(p)->pasega == pasega)
        {
            p->prio = -1;
            SortXfCtrls(palo, pxf);
            return;
        }
}

// orig: 0x00131B60
void DemoteAsegaOnAlo(ASEGA* pasega, ALO* palo, const std::vector<std::pair<int, int>>& apair)
{
    for (auto& up : palo->axf)
    {
        XF* pxf = up.get();
        for (const auto& [tag, kind] : apair)
            if ((pxf->tag == tag || tag == -1) && (pxf->kind == kind || kind == -1))
            {
                DemoteAsegaOnXf(pasega, palo, pxf);
                break;
            }
    }
}

// orig: 0x001304A8
ASEGA* PasegaFindAseg(ASEG* paseg, ALO* palo)
{
    for (ASEGA* p : paseg->apasega)
        if (p->palo == palo) return p;
    return nullptr;
}

// orig: 0x00130918 UpdateAsega
void UpdateAsega(ASEGA* pasega)
{
    TraceAsega("update", pasega);
    if (static const int s_oidTgt = std::getenv("SLY2N_ASEGTGT") ? std::atoi(std::getenv("SLY2N_ASEGTGT")) : 0; s_oidTgt)  // (debug: the ASEGs moving an object)
    {
        static int s_c = 0;
        bool f = pasega->palo && pasega->palo->oid == s_oidTgt;
        if (ASEGD* pd = PasegdOf(pasega->paseg)) for (const ASEGCHN& c : pd->achn) f = f || c.oid == s_oidTgt;
        if (f && s_c++ % 60 == 0)
            std::printf("asegtgt t %.3f aseg %d owner %d t %.3f svt %.3f svtLocal %.3f\n", g_clock.t, pasega->paseg->oid,
                        pasega->palo ? pasega->palo->oid : -1, pasega->t, pasega->svt, pasega->svtLocal);
    }
    if (std::fabs(pasega->svt) < 1e-4f) return;
    AddRefAsega(pasega);
    const float dt = (pasega->paseg->grfAseg & 0x100) ? g_clock.dt40 : g_clock.dt;
    pasega->t = AddRz(pasega->t, MulRz(pasega->svt, dt));  // (the EE's rounding: events at frame boundaries)
    FireAsegaEvents(pasega);
    if (FAsegaRetracted(pasega))
    {
        ReleaseAsega(pasega);
        return;
    }
    const bool fWrapped = FWrapAsegaTime(pasega, &pasega->t, &pasega->svt);
    if (fWrapped)
    {
        if (pasega->grfapl & 8)
            pasega->iea = pasega->svt <= 0.0f ? static_cast<int>(PasegdOf(pasega->paseg)->aaev[1].size()) : 0;
        NotifyAsega(pasega, 3);
        if (FAsegaRetracted(pasega))
        {
            ReleaseAsega(pasega);
            return;
        }
    }
    FireAsegaEvents(pasega);
    if (FAsegaRetracted(pasega))
    {
        ReleaseAsega(pasega);
        return;
    }
    if (fWrapped && (pasega->grfapl & 0x38) == 0) RetractAsega(pasega);
    for (ASEGAL& l : pasega->alink)
        if (l.ptrkg && l.pg) l.ptrkg->Eval(l.pg, nullptr, pasega->t, pasega->svt);
    ReleaseAsega(pasega);
}

// orig: 0x00130B18 RetractAsega
void RetractAsega(ASEGA* pasega)
{
    if (FAsegaRetracted(pasega)) return;
    AddRefAsega(pasega);
    std::vector<ASEGEV>& aevEnd = PasegdOf(pasega->paseg)->aaev[2];
    for (int i = pasega->ieaEnd; i < static_cast<int>(aevEnd.size()); i = ++pasega->ieaEnd)
    {
        aevEnd[i].t = pasega->t;
        FireAsegaEvent(pasega, &aevEnd[i]);
        if (FAsegaRetracted(pasega))
        {
            ReleaseAsega(pasega);
            return;
        }
    }
    SetAsegaHandsOff(pasega, 0);
    pasega->fRetracted = true;
    {
        auto it = std::find(g_apasega.begin(), g_apasega.end(), pasega);
        if (it != g_apasega.end())
        {
            const size_t j = static_cast<size_t>(it - g_apasega.begin());
            for (size_t* pi : s_apiAsega)
                if (*pi > j) --*pi;
            g_apasega.erase(it);
        }
    }
    auto& a = pasega->paseg->apasega;
    a.erase(std::remove(a.begin(), a.end(), pasega), a.end());
    for (CTRLASEG* pl = pasega->pctrlFirst; pl;)
    {
        CTRLASEG* plNext = pl->pnextAsega;
        RemoveXfCtrl(pl->palo, pl->pxf, pl);
        delete pl;
        pl = plNext;
    }
    pasega->pctrlFirst = nullptr;
    // (scalar links: the objects' vt slot 45 (kind), the parameter released)
    if (g_pfnAsegParamUse)
        for (ASEGAL& l : pasega->alink)
            if (l.plo) g_pfnAsegParamUse(l.plo, l.kind, false);
    pasega->alink.clear();
    void ReleaseAsegaSound(const ASEGA*); ReleaseAsegaSound(pasega);   // (0x258040(+0x5C, 0): asegevent.cpp)
    Script(pasega, 3);
    NotifyAsega(pasega, 2);
    ReleaseAsega(pasega);  // (the lists' reference)
    ReleaseAsega(pasega);
}

// orig: 0x001300B8 CalculateAsegMatrices: the transform of pxfFrom relative to pxfTo at time t of the animation
// (each record up the chain by its channel's value at t, or its own local) and its derivative
void CalculateAsegMatrices(ASEG* paseg, ALO* palo, XF* pxfFrom, XF* pxfTo, float t, float svt, glm::mat4* pmat,
                           glm::mat4* pdmat)
{
    ASEGD* pasegd = PasegdOf(paseg);
    glm::mat4 mAcc(1.0f), mDAcc(0.0f);
    std::vector<XF*> apxf(pasegd->achn.size(), nullptr);
    for (size_t i = 0; i < pasegd->achn.size(); ++i)
    {
        if (pasegd->achn[i].fDisabled) continue;
        ALO* po;
        GetAsegChannelTarget(paseg, palo, pasegd->achn[i], &po, &apxf[i]);
    }
    for (XF* po = pxfFrom; po && po != pxfTo; po = po->pxfRef)
    {
        glm::mat4 mLocal = po->mat, mDLocal(0.0f);
        for (size_t i = 0; i < pasegd->achn.size(); ++i)
        {
            const ASEGCHN& chn = pasegd->achn[i];
            if (apxf[i] != po || chn.fScalar || !chn.ptrkp) continue;
            glm::vec4 pos(0.0f), vel(0.0f);
            chn.ptrkp->Eval(&pos, &vel, t, svt);
            mLocal = MatXfWithValue(po, pos);
            if (po->kind == 6 || po->kind == 7)
            {
                // orig 0x22CB90: the skew matrix of the angular velocity (0x22CAD0) times the local rows
                float s0, c0, s1, c1;
                CalculateSinCos(pos.y, &s0, &c0);
                CalculateSinCos(pos.z, &s1, &c1);
                const glm::vec3 w(c0 * c1 * vel.x - s1 * vel.y, c0 * s1 * vel.x + c1 * vel.y, -s0 * vel.x + vel.z);
                for (int r = 0; r < 3; ++r) mDLocal[r] = glm::vec4(glm::cross(w, glm::vec3(mLocal[r])), 0.0f);
            }
            else
            {
                // orig 0x22CC00: the velocity by the record's kind
                XF xf = *po;
                xf.mode = 1;
                xf.vel = vel;
                mDLocal = DmatXf(&xf);
            }
            break;
        }
        // d(Local * Acc) = Local * dAcc + dLocal * Acc
        mDAcc = mLocal * mDAcc + mDLocal * mAcc;
        mAcc = mLocal * mAcc;
    }
    if (pmat) *pmat = mAcc;
    if (pdmat) *pdmat = mDAcc;
}

// orig: 0x0012F8F0 PostAsegLoad (0x149A50 queues ApplyAsegParams 0x12F8C0: here applied at once)
void PostAsegLoad(ASEG* paseg)
{
    if (std::getenv("SLY2N_ASEGADUMP"))
        std::printf("asegpostload %d parent %d apply %d t %g svt %g grf 0x%X inworld %d\n", paseg->oid, paseg->paloParent ? paseg->paloParent->oid : -1,
                    paseg->fApplyParams, paseg->tApply, paseg->svtApply, paseg->grfApply, paseg->fInWorld);
    if (std::getenv("SLY2N_ASEGADUMP"))
        for (const OPT& o : paseg->aopt)
            std::printf("asegpostload %d  opt eopid %d vals %zu first n %d g %g\n", paseg->oid, o.eopid, o.aval.size(), o.aval.empty() ? 0 : o.aval[0].n,
                        o.aval.empty() ? 0.f : o.aval[0].ag[0]);
    if (const char* sz = std::getenv("SLY2N_ASEGEV"); sz && std::atoi(sz) == paseg->oid)   // (debug: its events)
        if (ASEGD* pd = PasegdOf(paseg))
            for (int l = 0; l < 3; ++l)
                for (const ASEGEV& ev : pd->aaev[size_t(l)])
                    std::printf("asegev %d parent %d inworld %d list %d t %g kind %d n %d %d %d %d g %g %g %g\n", paseg->oid, paseg->paloParent ? paseg->paloParent->oid : -1, int(paseg->fInWorld), l, ev.t, ev.kind, ev.an[0], ev.an[1],
                                ev.an[2], ev.an[3], ev.ag[0], ev.ag[1], ev.ag[2]);
    if (const char* sz = std::getenv("SLY2N_ASEGEV"); sz && std::atoi(sz) == paseg->oid)   // (debug: its channels)
        if (ASEGD* pd = PasegdOf(paseg))
        {
            for (const ASEGCHN& c : pd->achn)
                std::printf("asegchn %d oid %d isub %d disabled %d scalar %d\n", paseg->oid, c.oid, c.isub, c.fDisabled, c.fScalar);
            for (const ASEGCHG& c : pd->achg)
                std::printf("asegchg %d kind %d n %d\n", paseg->oid, c.kind, c.n);
            std::printf("asegd %d tMax %g\n", paseg->oid, pd->tMax);
        }
    if (const char* sz = std::getenv("SLY2N_ASEGEVN"))   // (debug: every event with that argument)
        if (ASEGD* pd = PasegdOf(paseg))
            for (int l = 0; l < 3; ++l)
                for (const ASEGEV& ev : pd->aaev[size_t(l)])
                    if (ev.an[0] == std::atoi(sz) || ev.an[1] == std::atoi(sz) || ev.an[2] == std::atoi(sz) || ev.an[3] == std::atoi(sz))
                        std::printf("asegevn %d parent %d list %d t %g kind %d n %d %d %d %d\n", paseg->oid, paseg->paloParent ? paseg->paloParent->oid : -1, l, ev.t, ev.kind,
                                    ev.an[0], ev.an[1], ev.an[2], ev.an[3]);
    if (!paseg->fApplyParams) return;
    ApplyAseg(paseg, nullptr, paseg->tApply, paseg->svtApply, paseg->grfApply, nullptr);
}

void UpdateAsegas()
{
    // (debug: SLY2N_ASEGAOID=oid: once a second, the ASEGAs on that object or under it (aseg, time, speed, retracted))
    if (static const char* s_sz = std::getenv("SLY2N_ASEGAOID"); s_sz)
    {
        static float s_tNext = 0.0f;
        if (g_clock.t >= s_tNext)
        {
            s_tNext = g_clock.t + 1.0f;
            const int oid = std::atoi(s_sz);
            int c = 0;
            for (ASEGA* p : g_apasega)
            {
                bool fUnder = false;
                for (const LO* plo = p->palo; plo && !fUnder; plo = plo->paloParent) fUnder = plo->oid == oid;
                if (!fUnder) continue;
                ++c;
                std::printf("asegaoid t %.2f: asega %p on %d aseg %d t %.3f svt %.3f retracted %d\n", g_clock.t, (void*)p, p->palo->oid,
                            p->paseg ? p->paseg->oid : -1, p->t, p->svt, int(p->fRetracted));
            }
            if (!c) std::printf("asegaoid t %.2f: none\n", g_clock.t);
        }
    }
    size_t i = 0;
    s_apiAsega.push_back(&i);
    while (i < g_apasega.size())
    {
        ASEGA* p = g_apasega[i++];
        if (!p->fRetracted) UpdateAsega(p);
    }
    s_apiAsega.pop_back();
}

void CollectAsegas()
{
    // TODO(anim): retracted ASEGAs are kept (an SMA or a game object may still hold one); free those with cref 0
}

void ResetAsegas()
{
    for (ASEGA* p : g_apasega) p->fRetracted = true;
    g_apasega.clear();
}

void AddRefAsega(ASEGA* p) { ++p->cref; }  // orig 0x1308A8
void ReleaseAsega(ASEGA* p) { --p->cref; }  // orig 0x1308B8 (freed by CollectAsegas)
