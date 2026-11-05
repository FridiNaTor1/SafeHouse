// water.cpp: the water volumes (class 0x59, module 0x228B48..0x229EF0; the reference's m_228B48.cpp /
// m_228B48_p3.cpp, the decomp's src/hand 0x228C18 / 0x228D20 / 0x228E50 / 0x229468 / 0x229798 / 0x2292D8 /
// 0x229738, src/auto 0x229710, and the reference's raw 0x2297F8 / 0x15A2C0).
//
// A water object's entries are sensors (0x228BB0: flag 48). Its update (0x228E50, vt slot 16) goes through its
// attachment's overlaps: an overlapping object with a buoyancy (its mass state's +0xD4, option 59), a live attachment
// and a moving position record that the water's entries cut (0x229468: the object's vertical span from its box's
// bottom (a stepping object's base) to its top, clipped by the entries; the fraction inside) gets a node; a new one
// falling faster than 100 makes the splash (sound 0x13 and the water's explosion +0x170 at the point the entries cut
// its fall). Each node is also on the object's list (+0x138; 0x228D20 links / unlinks, waking the object); the
// object's signal 4 (entered) / 5 (left) with {water, object}. While on it, the object's gravity (0x1F9B20, vt slot
// 70) calls 0x229798: the water's slot 71 (0x2292D8) -> the object's slot 72 (ALO 0x2297F8; the players' 0x1A28B0 is
// the actors' and falls back here): buoyancy against gravity by the fraction, the drag toward the water's flow (its
// +0x160 velocity at the point, 0x228C18) split along / across the surface's normal, the angular drag, the righting
// torque, and the wake (0x15A2C0) while it floats at an upward surface: the sounds 0x4B..0x4E (the object's +0x174,
// by the relative speed) and the ripples (not ported: the class 7 objects of 0x1CE888, the emitter +0x178).
//
// Option fields (EOPIDs 874..881): +0x160 the flow (vector), +0x170 / +0x174 explosions (the splash), +0x178 the
// wake's emitter, +0x17C the ripple, +0x180 the wake's sound on, +0x184 / +0x188 a contact state / filter (-10 /
// 0x2000: read by the players' 0x1A28B0). The mass states' buoyancy +0xD4 / drag +0xD8 are options 59 / 60 (getter
// 0x1F7BF0); the water's own are 1 (0x228BB0).
#include "so.h"

#include "alo.h"
#include "animfind.h"
#include "clock.h"
#include "emitter.h"
#include "lo.h"
#include "mat.h"
#include "sound.h"
#include "step.h"
#include "sw.h"
#include "update.h"
#include "util.h"

#include "vtnative.h"
#include "xfmath.h"
#include "xfphys.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

glm::vec3 ProjectCnstr(const CNSTRS& c, const XF* pxfOwner, const glm::vec3& v);  // solve.cpp: 0x1FA810
void AccelAloPos(ALO* p, const glm::vec3& v);                                      // solve.cpp: 0x1FAC30
void UpdateAloBounds(ALO* p);                                                      // so.cpp: 0x1F50E0

namespace
{
constexpr CID CID_WATER = 0x59;
constexpr uint32_t PFN_SOST_GETTER = 0x1F7BF0;  // the options' getter of the mass state (+0xE8)
constexpr uint64_t FL148_WAS = 1ull << 31;      // the object was in the water (0x228E50)
constexpr uint64_t FL148_IN = 1ull << 32;       // it is in it now

constexpr float F32(uint32_t u) { return std::bit_cast<float>(u); }
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "water: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}
bool FDbg()
{
    static const bool s_f = std::getenv("SLY2_WATER") != nullptr;
    return s_f;
}

// A node {water, object} (orig 16 bytes from the game's pool +0x40E8: +0 the water, +4 the object, +8 the next on the
// object's list +0x138, +0xC the next on the water's list +0x150).
struct WNODE
{
    ALO* pwater = nullptr;
    ALO* po = nullptr;
};
// The lists, head first.
std::unordered_map<ALO*, std::vector<WNODE*>> s_mpWaterNodes;  // +0x150
std::unordered_map<ALO*, std::vector<WNODE*>> s_mpObjNodes;    // +0x138
// The objects' wake sounds (+0x174) and the last ripple's time (+0x170) (the 0x32 family's fields)
struct WAKE
{
    CSndRef snd;
    float tRipple = 0.0f;
};
std::unordered_map<ALO*, WAKE> s_mpwake;

float GField(const LO* plo, uint32_t pfn, uint32_t ib, float gDefault)
{
    uint32_t u;
    std::memcpy(&u, &gDefault, 4);
    u = plo->U32Field(pfn, ib, u);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}
void SetGField(LO* plo, uint32_t pfn, uint32_t ib, float g)
{
    uint32_t u;
    std::memcpy(&u, &g, 4);
    plo->SetField(pfn, ib, u);
}
// The mass state's buoyancy (+0xD4) and drag (+0xD8)
float GBuoyancy(const ALO* p) { return GField(p, PFN_SOST_GETTER, 0xD4, 0.0f); }
float GDrag(const ALO* p) { return GField(p, PFN_SOST_GETTER, 0xD8, 0.0f); }
glm::vec3 VecFlow(const ALO* pw)
{
    return glm::vec3(GField(pw, 0, 0x160, 0.0f), GField(pw, 0, 0x164, 0.0f), GField(pw, 0, 0x168, 0.0f));
}
LO* PloField(ALO* pw, uint32_t ib)
{
    const int oid = static_cast<int>(pw->U32Field(0, ib, 0));
    return oid > 0 ? PloFindSwNearest(pw, oid) : nullptr;
}

glm::vec3 VtVelPos(ALO* p)  // vt slot 33
{
    using PFN = glm::vec3 (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 33));
    return pfn ? pfn(p) : VelAloPos(p);
}
glm::vec3 VtVelRot(ALO* p)  // vt slot 34
{
    using PFN = glm::vec3 (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 34));
    return pfn ? pfn(p) : AngVelAlo(p);
}

// orig: 0x001F6838 (no attachment, or the attachment's flag bit 2)
bool FAloOutOfSweep(ALO* p)
{
    OXA* poxa = Poxa(p);
    return !poxa || ((poxa->grf >> 2) & 1);
}

// orig: 0x00229D60 (an entry not turned off: flag 50)
bool FWaterEntry(void*, SOE* psoe) { return !psoe->f50; }

// orig: 0x0014CDB0 (the union of the segment a-b's pieces inside the entries, at most cMax)
int CClipSegmentEntries(const glm::vec3& a, const glm::vec3& b, const std::vector<SOE*>& apsoe, int cMax, LSG* alsg)
{
    int c = 0;
    for (SOE* psoe : apsoe)
    {
        LSG* plsg = &alsg[c];
        const int n = ClipSegmentEntry(psoe, a, b, cMax - c, plsg);
        if (n) c = CMergeAlsg(c, alsg, n, plsg, cMax, alsg, 0);
    }
    return c;
}

// orig: 0x0011E290 (a's part along the unit vector b and the rest; their dot product)
float GSplitVector(const glm::vec3& a, const glm::vec3& b, glm::vec3* ppar, glm::vec3* pperp)
{
    const float d = (a.x * b.x + a.y * b.y) + a.z * b.z;
    const glm::vec3 p = b * d;
    if (ppar) *ppar = p;
    if (pperp) *pperp = a - p;
    return d;
}

// orig: 0x00229468 (the fraction of the object's vertical span the water's entries cut; the point where it leaves
// them (the surface) and the plane's normal there ((0, 0, 1) when none))
float GWaterDepth(ALO* pw, ALO* po, glm::vec3* ppos, glm::vec3* pnormal)
{
    glm::vec3 a = PosAloCenter(po);
    glm::vec3 b = a;
    OXA* poxa = Poxa(po);
    if (STEP* pstep = PstepFromLo(po))
    {
        a.z = a.z + pstep->vecBase.z;
        if (pstep->pxpFloor) a.z = a.z > pstep->posFloor.z ? a.z : pstep->posFloor.z;
    }
    else
        a.z = poxa ? poxa->posMin.z : a.z;
    b.z = poxa ? poxa->posMax.z : b.z;
    std::vector<SOE*> apsoe;
    CsoeCollect(pw, FWaterEntry, nullptr, &apsoe);
    LSG alsg[0x10];
    if (!CClipSegmentEntries(a, b, apsoe, 0x10, alsg))
    {
        if (pnormal) *pnormal = glm::vec3(0.0f, 0.0f, 1.0f);  // D_2A2DC0
        return 0.0f;
    }
    if (ppos) *ppos = alsg[0].pos1;
    if (pnormal)
    {
        const glm::vec3 n(alsg[0].plane1);
        *pnormal = FVecsNear(glm::vec3(0.0f), n) ? glm::vec3(0.0f, 0.0f, 1.0f) : n;
    }
    return alsg[0].t1 - alsg[0].t0;
}

// orig: 0x00228C18 (the water's velocity at a world point: the point (at the water's level z 0) carried by the
// water's record chain with the flow +0x160 as its local velocity, 0x230CD0 = 0x230A98 at dt 0)
glm::vec3 VelWaterAt(ALO* pw, const glm::vec3& pos) { return VelWaterPoint(PxfObj(pw), VecFlow(pw), pos); }
}

glm::vec3 VelWaterPoint(XF* pxf, const glm::vec3& vFlow, const glm::vec3& pos)
{
    glm::vec3 q = pxf ? glm::vec3(glm::inverse(pxf->matWorld) * glm::vec4(pos, 1.0f)) : pos;  // 0x1A6F00(0, pw, pos)
    q.z = 0.0f;
    glm::mat4 m1(1.0f);
    m1[3] = glm::vec4(q, 1.0f);
    glm::mat4 m2(0.0f);
    m2[3] = glm::vec4(vFlow, 0.0f);
    PredictXfChain(pxf, nullptr, 0.0f, &m1, &m2);
    return glm::vec3(m2[3]);  // (the rotation part (0x11BFB8) is not used by 0x2297F8)
}

namespace
{

void ReleaseWake(ALO* po)  // (the 0x32 family's +0x174: stopped, released)
{
    auto it = s_mpwake.find(po);
    if (it == s_mpwake.end() || !it->second.snd) return;
    it->second.snd->Stop(0.0f);  // 0x1FB098
    it->second.snd = CSndRef();  // 0x258040(+0x174, 0)
}

// orig: 0x0015A2C0 WakeSoWater (the object po floating in the water pw at pos, the water's velocity vFlow there:
// the wake sound by the relative speed (the 0x32 family: one of 0x4B..0x4E kept at +0x174) and the ripples' cadence)
void WakeAloWater(ALO* po, ALO* pw, const glm::vec3& vFlow, const glm::vec3& pos, float dt, float gMin, float sRadius)
{
    const glm::vec3 d = vFlow - VtVelPos(po);
    const float s = std::sqrt(std::fabs((d.x * d.x + d.y * d.y) + d.z * d.z));
    bool fRipple = GRandInRange(0.0f, 3.0f) < dt;  // 0x11D448
    ALO* pso = FCidDerivedFrom(po->Cid(), 0x32) ? po : nullptr;
    if (!(10.0f < s) && !fRipple)
    {
        if (pso) ReleaseWake(pso);
        return;
    }
    float u = (s - 10.0f) * F32(0x3AA5E9F7);  // (/ 790)
    u = u < 0.0f ? 0.0f : (1.0f < u ? 1.0f : u);
    if (pso && pw->U32Field(0, 0x180, 1))
    {
        const float gVol = u * F32(0x3F666666) + F32(0x3DCCCCCD);  // 0.9 u + 0.1
        WAKE& wk = s_mpwake[pso];
        if (wk.snd)
            wk.snd->SetVolume(gVol);
        else
        {
            static const int s_asfxid[4] = {0x4B, 0x4D, 0x4E, 0x4C};  // D_2BDEB0
            CSnd* psnd = PsndStartSound(s_asfxid[NRandInRange(0, 3)], 1.0f, 0, po);  // 0x1FBB90 + 0x1FB2F8
            psnd->SetFalloff(100.0f, 1000.0f);                                         // 0x1FB4F0
            psnd->SetVolume(gVol);                                                     // 0x1FB1F8
            wk.snd = CSndRef(psnd);                                                    // 0x258040
        }
    }
    if (!fRipple)
    {
        const float tStep = 1.0f - u * (1.0f - gMin);
        if (pso)
            fRipple = tStep < g_clock.t - s_mpwake[pso].tRipple;
        else
            fRipple = int(g_clock.t / tStep) != int((g_clock.t + dt) / tStep);
        if (!fRipple) return;
    }
    LO* ploRipple = PloField(pw, 0x17C);
    if (!ploRipple) return;
    // the ripple (0x1CE888(G, 7, 0): a plain RIP at pos, size sRadius growing by 0.6 sRadius, the water's +0x17C model)
    if (!FLaunchWaterRipple(ploRipple, pos, sRadius)) return;
    if (pso) s_mpwake[pso].tRipple = g_clock.t;
    // faster than 500: the spray of the water's +0x178 (0x15BE28: 4..6 kind-1 RIPs from 30 above pos)
    if (500.0f < s)
        if (LO* ploSpray = PloField(pw, 0x178))
        {
            const SOST* ps = Psost(po);
            LaunchWaterSpray(ploSpray, ploRipple, pw, pos, s, ps ? ps->dvGravity : glm::vec3(0.0f), NRandInRange(4, 6));
        }
}

// orig: 0x002297F8 func_002297F8 (ALO vt slot 72: the object in the water pw for a physics frame)
void ApplyWaterToAlo(ALO* po, ALO* pw)
{
    glm::vec3 pos(0.0f), normal(0.0f);
    const float depth = GWaterDepth(pw, po, &pos, &normal);
    if (FDbg())
    {
        const glm::vec3 c = PosAloCenter(po), v = VtVelPos(po);
        std::printf("[water] t %.3f %d in %d: depth %.3f surface %.1f %.1f %.1f n.z %.2f center %.1f %.1f %.1f v %.1f %.1f %.1f\n",
                    g_clock.t, po->oid, pw->oid, depth, pos.x, pos.y, pos.z, normal.z, c.x, c.y, c.z, v.x, v.y, v.z);
    }
    if (depth < F32(0x38D1B717)) return;  // 1e-4
    SOST* ps = Psost(po);
    if (!ps) return;
    const bool fNoGravity = FVecsNear(glm::vec3(0.0f), ps->dvGravity);
    const float gBuoy = fNoGravity ? 0.0f : GBuoyancy(po) * GBuoyancy(pw);
    const float gDrag = GDrag(po) * GDrag(pw);
    // buoyancy: against gravity by the fraction under water
    if (gBuoy != 0.0f) AccelAloPos(po, ProjectCnstr(ps->cnstrLin, PxfObj(po), ps->dvGravity * (-gBuoy * depth)));
    glm::vec3 vFlow(0.0f);
    if (gDrag != 0.0f)
    {
        vFlow = VelWaterAt(pw, PosAloCenter(po));
        const glm::vec3 v = VtVelPos(po);
        glm::vec3 vPar, vPerp;
        const float d = GSplitVector(vFlow - v, normal, &vPar, &vPerp);
        float gNormal = 1.0f;
        if (depth != 1.0f && !(0.0f < d)) gNormal = depth * 0.5f + 0.5f;  // (moving down into it: by the depth)
        if (FXfModeVelocity(PxfPos(po) ? PxfPos(po)->mode : 0))
        {
            static const float s_ag[3] = {0.0f, 3.0f, 0.0f};  // D_2C4470
            CLQ clq;
            clq.u0 = s_ag[0] * gNormal * gDrag;
            clq.u1 = s_ag[1] * gNormal * gDrag;
            clq.u2 = s_ag[2] * gNormal * gDrag;
            AccelAloTowardPosition(po, glm::vec3(0.0f), nullptr, v + vPar, &clq);
            clq.u0 = s_ag[0] * depth * gDrag;
            clq.u1 = s_ag[1] * depth * gDrag;
            clq.u2 = s_ag[2] * depth * gDrag;
            AccelAloTowardPosition(po, glm::vec3(0.0f), nullptr, v + vPerp, &clq);
        }
        if (FXfModeVelocity(PxfRot(po) ? PxfRot(po)->mode : 0))
        {
            const glm::vec3 w = VtVelRot(po);
            glm::vec3 wPar, wPerp;
            GSplitVector(-w, normal, &wPar, &wPerp);
            static const float s_ag[3] = {0.0f, 0.75f, 0.0f};  // D_2C4480
            static const glm::mat3 s_matIdentity(1.0f);         // D_2A2D00
            CLQ clq;
            clq.u0 = s_ag[0] * depth * gDrag;
            clq.u1 = s_ag[1] * depth * gDrag;
            clq.u2 = s_ag[2] * depth * gDrag;
            AccelAloTowardRotation(po, &s_matIdentity, nullptr, w + wPar, &clq);
            clq.u0 = s_ag[0] * gDrag;
            clq.u1 = s_ag[1] * gDrag;
            clq.u2 = s_ag[2] * gDrag;
            AccelAloTowardRotation(po, &s_matIdentity, nullptr, w + wPerp, &clq);
        }
    }
    // the righting torque: toward level by the angle, 3 x the buoyancy by the fraction (0x1FAC98)
    if (gBuoy != 0.0f)
    {
        XF* pxfObj = PxfObj(po);
        const glm::mat3 rot = LevelMatrix3(pxfObj ? Mat3Of(pxfObj->matWorld) : glm::mat3(1.0f), nullptr);  // 0x1A6318
        float rad = 0.0f;
        glm::vec3 axis(0.0f);
        DecomposeRotateMatrixRad(rot, &rad, &axis);  // 0x11BCF8
        if (XF* pxfRot = PxfRot(po); pxfRot && FXfModeVelocity(pxfRot->mode))
        {
            const glm::vec3 r = VecConvertXfs(nullptr, pxfRot->pxfRef, axis * (rad * (gBuoy * 3.0f) * depth));
            pxfRot->aseg[3] = glm::vec4(glm::vec3(pxfRot->aseg[3]) + r, pxfRot->aseg[3].w);
        }
    }
    // the wake while floating at an upward surface
    if (0.0f < depth && depth < 1.0f && F32(0x3F666666) < normal.z)
    {
        OXA* poxa = Poxa(po);
        const float sRadius =
            poxa ? ((poxa->posMax.x - poxa->posMin.x) + (poxa->posMax.y - poxa->posMin.y)) * 0.25f : 0.0f;
        WakeAloWater(po, pw, vFlow, pos, g_clock.dt, F32(0x3DCCCCCD), sRadius);
        return;
    }
    if (FCidDerivedFrom(po->Cid(), 0x32)) ReleaseWake(po);
}
NATIVEFN(0x2297F8, ApplyWaterToAlo);

// orig: 0x002292D8 func_002292D8 (the water's vt slot 71: the node's object's slot 72 with the water)
void NotifyWaterObject(ALO* pw, WNODE* pn)
{
    using PFN = void (*)(ALO*, ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(pn->po, 72));
    (pfn ? pfn : ApplyWaterToAlo)(pn->po, pw);
}

// orig: 0x00229710 / 0x00229738 (the node onto / off the object's list; the object woken (0x1F5078))
void LinkObjNode(WNODE* pn)
{
    auto& a = s_mpObjNodes[pn->po];
    a.insert(a.begin(), pn);
    WakeAlo(pn->po);
}
void UnlinkObjNode(WNODE* pn)
{
    auto it = s_mpObjNodes.find(pn->po);
    if (it != s_mpObjNodes.end())
    {
        auto& a = it->second;
        a.erase(std::remove(a.begin(), a.end(), pn), a.end());
        if (a.empty()) s_mpObjNodes.erase(it);
    }
    WakeAlo(pn->po);
}

// orig: 0x00228D20 func_00228D20 (the nodes whose object's standing changed linked (entered) / unlinked (left);
// those whose object is not in now freed; both bits cleared)
void SettleWaterNodes(ALO* pw)
{
    auto& a = s_mpWaterNodes[pw];
    std::vector<WNODE*> aKeep;
    for (WNODE* pn : a)
    {
        SORT& s = Sort(pn->po);
        const bool fWas = (s.grf148 & FL148_WAS) != 0, fIn = (s.grf148 & FL148_IN) != 0;
        if (fWas != fIn)
        {
            if (fWas) UnlinkObjNode(pn);
            else LinkObjNode(pn);
        }
        const bool fKeep = (Sort(pn->po).grf148 & FL148_IN) != 0;
        Sort(pn->po).grf148 &= ~(FL148_WAS | FL148_IN);
        if (fKeep)
            aKeep.push_back(pn);
        else
            delete pn;  // (0x2296C0: back to the pool)
    }
    a = std::move(aKeep);
}

// orig: 0x00228E50 func_00228E50 (the water's vt slot 16, see the top)
void UpdateWater(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's update
    ALO* pw = static_cast<ALO*>(plo);
    if (FDbg())
    {
        static std::map<ALO*, bool> s_mpSeen;
        if (!s_mpSeen[pw])
        {
            s_mpSeen[pw] = true;
            int cox = 0;
            for (OX* pox = Poxa(pw) ? Poxa(pw)->pox : nullptr; pox; pox = pox->poxNext) ++cox;
            std::printf("[water] t %.3f water %d: %zu entries, attachment %p (%d overlaps), flow %.1f %.1f %.1f box %.0f %.0f %.0f .. %.0f %.0f %.0f\n", g_clock.t,
                        pw->oid, pw->asoe.size(), (void*)Poxa(pw), cox, VecFlow(pw).x, VecFlow(pw).y, VecFlow(pw).z, Poxa(pw) ? Poxa(pw)->posMin.x : 0.f, Poxa(pw) ? Poxa(pw)->posMin.y : 0.f, Poxa(pw) ? Poxa(pw)->posMin.z : 0.f, Poxa(pw) ? Poxa(pw)->posMax.x : 0.f, Poxa(pw) ? Poxa(pw)->posMax.y : 0.f, Poxa(pw) ? Poxa(pw)->posMax.z : 0.f);
        }
    }
    if (pw->asoe.empty()) return;
    auto& anode = s_mpWaterNodes[pw];
    for (WNODE* pn : anode) Sort(pn->po).grf148 |= FL148_WAS;
    std::vector<SOE*> apsoe;
    CsoeCollect(pw, FWaterEntry, nullptr, &apsoe);
    OXA* poxaW = Poxa(pw);
    for (OX* pox = poxaW ? poxaW->pox : nullptr; pox; pox = pox->poxNext)
    {
        ALO* po = pox->psoOther;
        if (!po || !Psost(po)) continue;
        if (GBuoyancy(po) < F32(0x38D1B717)) continue;
        if (FAloOutOfSweep(po)) continue;
        if (!PxfPos(po) || !FXfModeVelocity(PxfPos(po)->mode)) continue;
        if (GWaterDepth(pw, po, nullptr, nullptr) == 0.0f) continue;
        Sort(po).grf148 |= FL148_IN;
        if (Sort(po).grf148 & FL148_WAS) continue;
        WNODE* pn = new WNODE{pw, po};
        anode.insert(anode.begin(), pn);
        if (FDbg()) std::printf("[water] t %.3f %d (cid 0x%X) in water %d\n", g_clock.t, po->oid, po->Cid(), pw->oid);
        // the splash: a new one falling faster than 100, where its entries' fall crosses the water's
        LO* ploSplash = PloField(pw, 0x170);
        if (!pw->U32Field(0, 0x170, 0)) continue;
        const glm::vec3 v = VtVelPos(po);
        if (!(v.z < -100.0f)) continue;
        for (SOE& e : po->asoe)
        {
            if (e.f48 || e.f50) continue;
            glm::vec3 a(e.mat[3]);                                          // where the entry's world geometry was made
            glm::vec3 b = e.pxf ? glm::vec3(e.pxf->matWorld[3]) : a;  // where its record is now
            float dz;
            if (STEP* pstep = PstepFromLo(po))
                dz = pstep->vecBase.z;
            else
                dz = (Poxa(po) ? Poxa(po)->posMin.z : 0.0f) - (e.pxf ? e.pxf->matWorld[3].z : 0.0f);
            a.z = a.z + dz;
            b.z = b.z + dz;
            LSG alsg[0x10];
            if (CClipSegmentEntries(a, b, apsoe, 16, alsg) <= 0) continue;
            OXA* poxa = Poxa(po);
            float fall = -v.z;
            fall = std::min(fall, 600.0f);
            fall = fall * F32(0x3ADA740E);  // / 600
            const float sRadius =
                poxa ? ((poxa->posMax.x - poxa->posMin.x) + (poxa->posMax.y - poxa->posMin.y)) * 0.5f : 0.0f;
            if (CSnd* psnd = PsndStartSound(0x13, 1.0f, 0, po))  // 0x1FBB90 / 0x1FB2F8
            {
                psnd->SetFalloff(100.0f, 1000.0f);  // 0x1FB4F0
                psnd->SetVolume(fall);              // 0x1FB1F8
            }
            BURSTPARAMS bp;  // (vt slot 51 of the explosion: 0x15C3C0)
            bp.grf = 14;
            bp.g8 = sRadius * fall;
            bp.dir = glm::vec3(0.0f, 0.0f, 1.0f);
            bp.pos = alsg[0].pos0;
            if (FDbg()) std::printf("[water]   splash at %.1f %.1f %.1f strength %.2f\n", bp.pos.x, bp.pos.y, bp.pos.z, bp.g8);
            if (ploSplash) ExplodeLo(ploSplash, &bp);
        }
    }
    // the signals: 4 entered, 5 left ({water, object})
    for (WNODE* pn : std::vector<WNODE*>(anode))
    {
        const uint64_t f = Sort(pn->po).grf148;
        if (((f & FL148_WAS) != 0) == ((f & FL148_IN) != 0)) continue;
        struct
        {
            ALO* pw;
            ALO* po;
        } msg{pw, pn->po};
        if (FDbg() && (f & FL148_WAS)) std::printf("[water] t %.3f %d left water %d\n", g_clock.t, pn->po->oid, pw->oid);
        if (g_pfnLoNotify) g_pfnLoNotify(pn->po, (f & FL148_WAS) ? 5 : 4, &msg);
    }
    SettleWaterNodes(pw);
}

// orig: 0x00228BB0 func_00228BB0 (the water's vt slot 64: at rest (0x1F3590), its entries sensors (0x1F68A8), its
// buoyancy and drag 1)
void PostLoadWater(ALO* pw)
{
    RestAloSost(pw);
    for (SOE& e : pw->asoe) e.f48 = true;
    SetGField(pw, PFN_SOST_GETTER, 0xD4, 1.0f);
    SetGField(pw, PFN_SOST_GETTER, 0xD8, 1.0f);
}
NATIVEFN(0x228BB0, PostLoadWater);
NATIVEFN(0x229680, UpdateAloBounds);  // (the water's slot 49: 0x1F50E0)

struct INITWATER
{
    INITWATER() { RegisterLoUpdate(0x228E50, UpdateWater); }
} s_initwater;
}

constinit void (*g_pfnHealthExtractorHook)(ALO* powner, ALO* po) = nullptr;  // carry.cpp (0x2255C8)

// orig: 0x00229798 func_00229798 (from the gravity 0x1F9B20: each water the object is in (+0x138), its slot 71; the
// Health Extractor's (cid 0x66) node on its guard is its slot 71 0x2255C8)
void ApplyAloWater(ALO* po)
{
    auto it = s_mpObjNodes.find(po);
    if (it == s_mpObjNodes.end()) return;
    for (WNODE* pn : std::vector<WNODE*>(it->second))
    {
        if (pn->pwater->pcls && pn->pwater->pcls->PfnOrig(71) == 0x2255C8)
        {
            if (g_pfnHealthExtractorHook) g_pfnHealthExtractorHook(pn->pwater, pn->po);
            continue;
        }
        NotifyWaterObject(pn->pwater, pn);
    }
}

// orig: 0x00229710 / 0x00229738 for an owner other than a water (the Health Extractor's node +0x1B8 on its guard):
// kept in the owner's list too, so either one leaving the world drops it (RemoveAloWater)
void LinkAloHook(ALO* powner, ALO* po)
{
    WNODE* pn = new WNODE{powner, po};
    s_mpWaterNodes[powner].push_back(pn);
    LinkObjNode(pn);
}
void UnlinkAloHook(ALO* powner, ALO* po)
{
    auto it = s_mpWaterNodes.find(powner);
    if (it == s_mpWaterNodes.end()) return;
    auto& a = it->second;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i]->po == po)
        {
            WNODE* pn = a[i];
            a.erase(a.begin() + static_cast<long>(i));
            UnlinkObjNode(pn);
            delete pn;
            return;
        }
}

// The object leaves the world: its nodes (as an object and, for a water, 0x229308's) dropped
void RemoveAloWater(ALO* p)
{
    if (auto it = s_mpWaterNodes.find(p); it != s_mpWaterNodes.end())
    {
        for (WNODE* pn : it->second)
        {
            UnlinkObjNode(pn);
            delete pn;
        }
        s_mpWaterNodes.erase(it);
    }
    if (auto it = s_mpObjNodes.find(p); it != s_mpObjNodes.end())
    {
        for (WNODE* pn : std::vector<WNODE*>(it->second))
        {
            auto& a = s_mpWaterNodes[pn->pwater];
            a.erase(std::remove(a.begin(), a.end(), pn), a.end());
            delete pn;
        }
        s_mpObjNodes.erase(it);
    }
    ReleaseWake(p);
    s_mpwake.erase(p);
}

void ResetWater()
{
    for (auto& [pw, a] : s_mpWaterNodes)
        for (WNODE* pn : a) delete pn;
    s_mpWaterNodes.clear();
    s_mpObjNodes.clear();
    s_mpwake.clear();
}

// (public, for the actors' 0x1A28B0: orig 0x00228C18)
glm::vec3 VelWaterAtPoint(ALO* pw, const glm::vec3& pos) { return VelWaterAt(pw, pos); }

// (public, for the actors' splash points 0x1A2460: the water's object options by their oids, e.g. its effect +0x174)
LO* PloWaterField(ALO* pw, uint32_t ib) { return PloField(pw, ib); }
