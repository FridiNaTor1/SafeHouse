// treasuremap3.cpp: Sly 3's treasure map (class 0x99, orig3 0x278A48..0x279708) and Sly's map mode (his state 0x18), from
// the Sly 3 decomp.
//
// Ep5's p_island_map (world 34): the map 46836. Sly's Treasure Map gadget (item 36: orig3 0x1AF890 in the gadget use
// 0x1AF6C0) takes the world's map (0x279C28: class 0x99, in the world, not pending 0x2796A0) into JT +0x1F20 and puts
// Sly in his state 0x18 substate 8 (0x1ADDD0), whose entry holds the map up (0x279120: map +0x48 = Sly, state 2) and
// whose exit puts it away (0x279140: state 1). The map's state machine 0x278E58: 2 = held (broadcast 0x55; on the first
// record or the last reached one: 0x57 and the next; the screen object +0x50 shows the next pair's text), leaving 2:
// broadcast 0x56. In 0x18 (0x1B0070): substate 8: Square / Circle / Triangle (0xB0) check / dig (0x279278), X (0x40) a
// step (substate 9: 0x2791C8 counts it, sound 0x60; back to 8 when the step is done), the stick turns Sly (0x1AD1D0
// case 24). A check on the next record after its step count (records +0x58: the LOX and the steps, the distance over
// the stride +0x264) broadcasts 0x57 and goes on; a wrong one 0x58 (and the map's miss sound); on the last record a
// right one starts SCPT +0x288 and puts the map away (state 0).
// Natively: Sly's state 0x18 is this module's hold (po.cpp hands it the held player's pad, as the password check);
// a wrong check puts the map away (the original calls Sly's vt slot 99). Not ported: the footprint trails (objects
// 0x280 / 0x281, class 0x7F), the camera task +0x280 (0x2799C0), the save flag of +0x294 (0x25E2B8).
// Debug: SLY3_MAPPRINT=1.

#include "treasuremap3.h"

#include "task3.h"
#include "../aijobs.h"
#include "../activate.h"
#include "../brx.h"
#include "../cls.h"
#include "../spliceglue.h"
#include "../clock.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../ref.h"
#include "../po.h"
#include "../screenrt.h"
#include "trigger3.h"
#include "../so.h"
#include "../sound.h"
#include "../sw.h"
#include "../vecmat.h"
#include "../xf.h"
#include "scptgame3.h"
#include "../actor.h"
#include "../asega.h"
#include "../sly.h"
#include "../stepphys.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_MAPPRINT") != nullptr;
    return s_f;
}

struct MAP3
{
    LO* plo = nullptr;
    SW* psw = nullptr;
    bool fLoaded = false;
    int state = -1;                    // +0x40
    float tState = 0;                  // +0x44
    ALO* pjt = nullptr;                // +0x48: the player holding it
    LO* pscreen = nullptr;             // +0x50 (option 1902: a screen object)
    struct REC { LO* plo; int c; };
    std::vector<REC> arec;             // +0x54 / +0x58 (option 1904 = 0x279160: up to 32; c: the steps to it)
    std::vector<std::pair<int, int>> apair;   // +0x158 / +0x15C / +0x1DC (method 1905 = 0x279190: the screen texts)
    int fHint = 0, cMiss = 0;          // +0x25C / +0x260
    float stride = 200, radius = 500;  // +0x264 / +0x268 (option 1903)
    int cPrint = 0;                    // +0x274: steps printed since the last record
    int irec = 0;                      // +0x278: the next record
    int cStep = 0;                     // +0x27C
    int nTurn = 8;                     // +0x284 (option 1909)
    LO* pscpt = nullptr;               // +0x288 (option 1906)
    int fDone = 0;                     // +0x28C
    int sfxMiss = 0x36;                // +0x290 (option 1908)
    bool fPending = false;             // +0x294 (option 1910)
    bool fOnRec = false;               // +0x298
    // (native: the step under way)
    float tStep = -1;
    glm::vec3 posStep0{0}, posStep1{0};
};
std::unordered_map<const LO*, MAP3> s_mpmap;

LO* PloOpt(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindRelative(plo, v.n);
}
glm::vec3 PosLo(LO* plo)
{
    if (!plo) return glm::vec3(0.0f);
    if (FIsLoDerivedFrom(plo, 0x2F))
        if (XF* pxf = PxfObj(static_cast<ALO*>(plo))) return glm::vec3(pxf->matWorld[3]);   // (vt slot 36)
    return sly3::PosLoWorld3(plo);
}
float Dist2(const glm::vec3& a, const glm::vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// orig3 0x278A48 (init) / 0x278AC8 (post-load): the options, the records' steps
void Load(MAP3& m)
{
    if (m.fLoaded) return;
    m.fLoaded = true;
    for (const OPT& o : m.plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& a = o.aval[0];
        switch (o.eopid)
        {
        case 1902: m.pscreen = PloOpt(m.plo, a); break;
        case 1903: m.radius = a.ag[0]; break;
        case 1904: for (const OPTVAL& v : o.aval) if (m.arec.size() < 32) if (LO* p = PloOpt(m.plo, v)) m.arec.push_back({p, 0}); break;
        case 1905: if (o.aval.size() >= 2 && m.apair.size() < 32) m.apair.push_back({o.aval[0].n, o.aval[1].n}); break;
        case 1906: m.pscpt = PloOpt(m.plo, a); break;
        case 1908: m.sfxMiss = a.n; break;
        case 1909: m.nTurn = a.n; break;
        case 1910: m.fPending = a.n != 0; break;
        default: break;
        }
    }
    // (the stride from the HUD's step curve (D_00370AC0 +0x1E74): natively none, 200)
    for (size_t i = 1; i < m.arec.size(); ++i)
        m.arec[i].c = int(Dist2(PosLo(m.arec[i - 1].plo), PosLo(m.arec[i].plo)) / m.stride + 0.5f);
    if (FPrint())
    {
        std::printf("map3: %d loaded: %zu records, %zu pairs, screen %d, scpt %d, radius %.0f\n", m.plo->oid, m.arec.size(), m.apair.size(),
                    m.pscreen ? m.pscreen->oid : -1, m.pscpt ? m.pscpt->oid : -1, m.radius);
        for (const MAP3::REC& r : m.arec)
        {
            const glm::vec3 p = PosLo(r.plo);
            std::printf("map3:   record %d at %.0f %.0f %.0f steps %d\n", r.plo->oid, p.x, p.y, p.z, r.c);
        }
    }
}

MAP3* Pmap(LO* plo)
{
    if (!plo) return nullptr;
    auto it = s_mpmap.find(plo);
    if (it == s_mpmap.end() || it->second.psw != g_psw)
    {
        MAP3& m = s_mpmap[plo];
        m = MAP3{};
        m.plo = plo;
        m.psw = g_psw;
        Load(m);
        return &m;
    }
    return &it->second;
}

// orig3 0x2795C0: the player within the radius of a record
bool FNearRecord(MAP3& m, size_t i)
{
    if (!m.pjt || i >= m.arec.size()) return false;
    return glm::distance(PosLo(m.pjt), PosLo(m.arec[i].plo)) <= m.radius;
}
// orig3 0x279660: the screen text of the next record (the pair's second when on a record, else its first)
int NText(MAP3& m)
{
    const size_t i = size_t(m.irec);
    if (i >= m.apair.size()) return -1;
    const int n = m.fOnRec ? m.apair[i].second : -1;
    return n == -1 ? m.apair[i].first : n;
}
// orig3 0x279450: the player put on the last reached record (dropped onto the ground) unless within 50 of it
void SnapToRecord(MAP3& m, bool fAlways)
{
    if (!m.pjt || m.irec <= 0) return;
    glm::vec3 pos = PosLo(m.arec[size_t(m.irec - 1)].plo);
    if (!fAlways && Dist2(pos, PosLo(m.pjt)) <= 50.0f) return;
    FDropOnGround(&pos, 100.0f, -350.0f, true);   // 0x198D78
    SetAloPos(m.pjt, pos);                        // (vt slot 34, less the record's offset +0x260: natively the object record)
}
// orig3 0x2793F0 / 0x2793A0: on to the next record
void NextRecord(MAP3& m)
{
    m.cStep = 0;
    ++m.irec;
    SnapToRecord(m, false);
    m.cPrint = 0;
    if (m.pscreen) ShowScreenObj(m.pscreen, NText(m));
}

void ShowScreen(MAP3& m, bool fShow)
{
    if (!m.pscreen) return;
    if (fShow) ShowScreenObj(m.pscreen, (m.fOnRec && m.irec < int(m.apair.size())) || m.irec == 0 ? NText(m) : -1);   // (+0xBC, slot 56)
    else HideScreenObj(m.pscreen);   // (slot 54)
}

// (s3fix-ai2) orig3 0x1ADDD0's state 0x18: the holder's part +0x144C (the actors' table entry 55, oid 1912) in the world
// while the map is held (vt slot 8 / 9) and locked (0x23B620(1 / 0): flag bit 34). (s3fix-final) The part is a class-0x7D
// object (vt 0x3559C8, the two-player pointer: activate.cpp FAloDrawn), not a trigger: its +0x140 is the scale controller
// its post-load 0x1696F0 puts on its kind-12 record (0x130078(p, 0x23DDA0(p, id, 0xC), 5, 8, 3, {1, 1, 1})), whose value
// +0x20 the state sets to clamp(0.001 x the map's +0x4C (option 1695), 0..3) (D_00322730 / D_00322740) and back to 1 on
// leaving. The same post-load hides the part (0x1697D0(p, 0): its draw record's shown flag +0x4C->+0x90->+0x70 = 0, +0x130
// none) and only the two-player update shows it again (0x169868 / 0x169A30, both players set): with the map (single
// player) the scaled part is never drawn, so the scale has nothing to show and is not modelled here.
void SetHolderTrigger(MAP3& m, bool fOn)
{
    if (!m.pjt || !g_psw) return;
    for (const auto& [ib, plo] : m.pjt->apart)
        if (ib == (FN3_TAG | 0x144C) && plo)
        {
            if (fOn) AddLo(g_psw, plo);
            else RemoveLo(g_psw, plo);
            return;
        }
}

// orig3 0x278E58
void SetMapState(MAP3& m, int state)
{
    if (m.state == state) return;
    if (m.state == 2)
    {
        SetHolderTrigger(m, false);
        Broadcast3(0x56, m.plo);
        ShowScreen(m, false);
        if (m.cPrint && ++m.cMiss >= 5) m.fHint = 1;
        m.cPrint = 0;
        m.fOnRec = false;
        m.tStep = -1;
    }
    if (FPrint()) std::printf("map3: %d state %d -> %d (record %d, steps %d)\n", m.plo->oid, m.state, state, m.irec, m.cStep);
    m.state = state;
    m.tState = g_clock.t;
    if (state != 2) return;
    SetHolderTrigger(m, true);
    Broadcast3(0x55, m.plo);
    m.cStep = 0;
    m.fDone = 0;
    if (m.irec == 0 && FNearRecord(m, 0))
    {
        Broadcast3(0x57, m.plo);
        NextRecord(m);
    }
    m.fOnRec = m.irec > 0 && FNearRecord(m, size_t(m.irec - 1));
    if (m.fOnRec) SnapToRecord(m, true);
    ShowScreen(m, true);
}

// orig3 0x279568: the check is right (on a record after its steps)
bool FCheckRight(MAP3& m)
{
    if (!m.fOnRec) return false;
    if (m.irec < int(m.arec.size()) && m.cStep != m.arec[size_t(m.irec)].c) return false;
    return FNearRecord(m, size_t(m.irec));
}

// orig3 0x279278: check / dig
void CheckMap(MAP3& m)
{
    const bool fRight = FCheckRight(m);
    if (FPrint()) std::printf("map3: %d check at record %d of %zu, steps %d: %d\n", m.plo->oid, m.irec, m.arec.size(), m.cStep, fRight);
    if (m.irec == int(m.arec.size()) - 1)
    {
        m.fDone = fRight;
        if (fRight) NextRecord(m);
        if (m.pscpt) StartScptOfLo3(m.pscpt, m.pjt);   // 0x225EE8
        if (m.fDone) SetMapState(m, 0);
        return;
    }
    if (fRight)
    {
        Broadcast3(0x57, m.plo);
        PsndStartSound(0x2D);
        NextRecord(m);
        return;
    }
    Broadcast3(0x58, m.plo);
    PsndStartSound(m.sfxMiss);
    SetMapState(m, 1);   // (Sly's vt slot 99 with 0: natively the map put away)
}

// orig3 0x2791C8: a step counted (a footprint after the first; sound 0x60)
void StepMap(MAP3& m)
{
    if (!m.fOnRec) return;
    if (m.cStep > 0) ++m.cPrint;
    ++m.cStep;
    PsndStartSound(0x60);
}

MAP3* s_pmapHeld = nullptr;

struct INITMAP3
{
    INITMAP3()
    {
        // option 1700 (EOPID 1907, +0x28C): the last dig was right (SCPT 4028's condition 50703 through the splice)
        RegisterObjectGetter3(1907, [](LO* plo) {
            CRef r;
            MAP3* m = Pmap(plo);
            r.SetBool(m && m->fDone != 0);
            return r;
        });
        // option 1695 (EOPID 1901, +0x4C) / 1696 (1903, the radius +0x268)
        RegisterObjectGetter3(1903, [](LO* plo) {
            CRef r;
            MAP3* m = Pmap(plo);
            r.SetF32(m ? m->radius : 500.0f);
            return r;
        });
    }
} s_initmap3;
}  // namespace

// orig3 0x279C28: the world's treasure map (class 0x99), in the world and not pending (0x2796A0)
LO* PloTreasureMap3()
{
    if (!g_psw) return nullptr;
    for (LO* plo : g_psw->mpcidaplo[0x99])
    {
        if (!plo->fInWorld) continue;
        MAP3* m = Pmap(plo);
        if (m && m->fPending) continue;   // (+0x294 with the save flag D_00468D30 set: not tracked natively)
        return plo;
    }
    return nullptr;
}

// orig3 0x1AF890 / 0x1ADDD0 entering 0x18: the map held up by the player (0x279120)
bool FHoldTreasureMap3(LO* ploPlayer)
{
    MAP3* m = Pmap(PloTreasureMap3());
    if (!m || !ploPlayer || !FIsLoDerivedFrom(ploPlayer, 0x2F)) return false;
    m->pjt = static_cast<ALO*>(ploPlayer);
    s_pmapHeld = m;
    SetMapState(*m, 2);
    return true;
}

LO* PloMapHolder3() { return s_pmapHeld && s_pmapHeld->psw == g_psw && s_pmapHeld->state == 2 ? s_pmapHeld->pjt : nullptr; }

// orig3 0x11E438: an angle snapped to the nearest of n directions (0: as it is, 1: 0)
static float RadQuantize(float rad, int n)
{
    if (n == 0) return rad;
    if (n == 1) return 0.0f;
    rad = std::remainder(rad, 6.28318530718f);   // (0x11E3B8: into [-pi, pi])
    const float spacing = 6.28318530718f / float(n), half = spacing * 0.5f;
    float sign = 1.0f;
    if (rad < 0.0f) { rad = -rad; sign = -1.0f; }
    float snapped = 0.0f;
    for (int i = 0; i <= n / 2; ++i)
    {
        snapped = spacing * float(i);
        if (snapped - half <= rad && rad <= snapped + half) break;
    }
    return snapped * sign;
}

int NMapSteps3() { return s_pmapHeld ? s_pmapHeld->cStep : 0; }

// orig3 0x1B0070 (Sly's update in state 0x18) and 0x1AD1D0 case 24 substate 8 (the input: the heading): the held
// player's pad in the map mode (AUDIT5 PLAYER5-08). Substate 8 (the map held up, clip +0x1E70): Square / Circle /
// Triangle (0xB0) check (0x279278); nothing more while off the floor over 0.15 s (0x2503C8); X (0x40) the step:
// substate 9 (the step clip +0x1E74 / +0x1E78 by the steps' parity, Sly moved by its root: sly.cpp) and the step counted
// (0x2791C8); the stick turns Sly to its direction snapped to the map's +0x284 directions (0x11E438, 0x24E878).
// Substate 9: back to 8 when the step clip is over (0x138AC8). (Natively before: a stride of 200 in 0.4 s, the stick's x
// turning him at 2 rad/s.)
void TakePadForMap3(LO* ploPlayer, uint32_t grfPressed, float radJoy, float uJoy)
{
    MAP3* m = s_pmapHeld;
    if (!m || m->state != 2 || m->pjt != ploPlayer) return;
    SLY* psly = dynamic_cast<SLY*>(PactorFromLo(ploPlayer));
    if (!psly || psly->jts != jts::Map) return;
    if (grfPressed && FPrint())
        std::printf("map3: press 0x%X at t %.3f in substate %d (state %.3f s, off the floor %.3f s)\n", grfPressed, g_clock.t, psly->jtbs,
                    g_clock.t - psly->tSubstate, g_clock.t - psly->tFloor);
    if (psly->jtbs == jts::SubMapClip)
    {
        if (!psly->pasega || FAsegaRetracted(psly->pasega)) SetSlyState(psly, jts::Map, jts::SubMapUp);
        return;
    }
    if (psly->jtbs != jts::SubMapUp) return;
    if (grfPressed & 0xB0)
    {
        CheckMap(*m);
        return;
    }
    if (0.15f < g_clock.t - psly->tFloor) return;
    if (grfPressed & 0x40)
    {
        SetSlyState(psly, jts::Map, jts::SubMapClip);
        StepMap(*m);
        return;
    }
    if (0.0f < uJoy) SetStepPhysHeading(m->pjt, RadQuantize(radJoy, m->nTurn));
}

void ResetTreasureMaps3()
{
    s_mpmap.clear();
    s_pmapHeld = nullptr;
}

// orig3 0x279140 (Sly leaving his state 0x18): the map put away (state 1) when this player holds it
void PutAwayTreasureMap3(LO* ploPlayer)
{
    MAP3* m = s_pmapHeld;
    if (!m || m->psw != g_psw || m->pjt != ploPlayer) return;
    if (m->state == 2) SetMapState(*m, 1);
}

namespace { uint32_t s_grfMapHeld = 0; }
uint32_t GrfMapButtonsHeld3() { return s_grfMapHeld; }
void SetMapButtonsHeld3(uint32_t grf) { s_grfMapHeld = grf; }
