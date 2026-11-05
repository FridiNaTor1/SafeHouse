// Sly 3's follow camera policy: see cmfollow3.h. Every function here is from the Sly 3 asm; what is not ported is
// marked TODO(sly3) (the zone samples / paths, the eye's avoidance and cut 0x167660 / 0x276900, the focus's ray check).

#include "cmfollow3.h"

#include "game3.h"
#include "../joy.h"

#include <algorithm>
#include <cmath>

namespace sly3
{
using namespace cmm;

namespace
{
constexpr float c_eps = F32(0x38D1B717);           // 1e-4
const LM c_lmUnit{0.0f, 1.0f};                      // 0x341FF0
const LM c_lmTiltTurn{-0.5f, F32(0x3FC70D23)};      // 0x3290E8
const SMP c_smpLead{4.0f, 0.0f, 0.25f};             // 0x329130
const SMP c_smpTilt{0.75f, 0.0f, 2.0f};             // 0x329140 (scaled by the rate)
const SMP c_smpOffset{1500.0f, 0.0f, 0.5f};         // 0x325080
const float c_aLeadStick[3] = {1.0f, -2.0f, -2.0f}; // 0x3290F0
const LM c_lmLeadStick{1.0f, 2.0f};                 // 0x329100
const float c_aLeadTime[3] = {0.0f, 1.0f, 0.0f};    // 0x329120
const float c_aTiltRate[3] = {-0.1f, F32(0x3AF04C76), 0.0f};  // 0x3290D0 (of the lead's speed)
const float c_aAhead[3] = {1.5f, -0.5f, 0.0f};      // 0x325090 (of the focus's angular speed)

// The zone's return parameters (the zone +0x130; 0x34A040 when there is no zone, the beach's as well)
struct ZRET3
{
    int f18 = 1;            // +0x18
    float dt1C = 0.0f;      // +0x1C
    float u20 = 1.0f;       // +0x20
    int f24 = 0;            // +0x24
};
const ZRET3 c_zretDefault;

float Quad(const float a[3], float x) { return a[0] + x * (a[1] + x * a[2]); }
float Clamp(float g, const LM& lm) { return g < lm.gMin ? lm.gMin : (lm.gMax < g ? lm.gMax : g); }
glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& d)
{
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l < c_eps ? d : v * (1.0f / l);
}

// orig3: 0x1F3E48 (a damped spring of pos toward the target: dt at most 0.1, the trapezoid on the position)
void SpringFocus3(const glm::vec3& posTarget, glm::vec3* ppos, glm::vec3* pv, float kDamp, float kSpring, float dt)
{
    dt = std::min(dt, 0.1f);
    const glm::vec3 a = (posTarget - *ppos) * kSpring + *pv * kDamp;
    const glm::vec3 v = *pv + a * dt;
    *ppos = *ppos + *pv * (dt * 0.5f) + v * (dt * 0.5f);
    *pv = v;
}

// The focus's raw record: Sly's vt slot 85 (orig3 0x1ABA80) over the actor's 0x153D40, the step's 0x250690 and the
// SO's 0x1F3D48, with the jump's 0x1BD758 (the climb / interaction / damage sub-objects' parts are not reached on
// the beach: TODO(sly3) when Sly climbs / interacts)
void GetSlyCpdefiRaw3(const ACTORCM3& a, float t, CPDEFIRAW3* praw)
{
    CPDEFIRAW3& r = *praw;
    r = CPDEFIRAW3();
    // orig3: 0x1F3D48 (the SO's)
    r.pos = a.pos;
    r.v = a.vel;
    r.lmZ = LM{-275.0f, 150.0f};          // 0x325060
    r.lmZ2 = LM{-300.0f, 200.0f};
    r.smpZ = SMP{4000.0f, 0.0f, 4.0f};
    r.radHome = std::atan2(-a.dirFacing.y, -a.dirFacing.x);
    r.tMoveLast = t;
    r.kSpring = 5.0f;
    r.kDamp = -3.5f;
    r.n70 = 1;
    // orig3: 0x250690 (the step's: +0x70 5, +0x74 0x349890)
    r.n70 = 5;
    // orig3: 0x153D40 (the actor's)
    r.sAhead = 100.0f;
    r.tMoveLast = a.tMoveLast;
    if (a.fFloor) r.pos.z = a.zFloor + a.dzFocus;   // (+0x508 3 / 4, the interaction's, as well)
    else
    {
        r.pos.z = r.pos.z + (a.dzBody + a.dzFocus);
        if (a.jstate != 0x12 && a.jstate != 0x15) r.grf |= 4;
    }
    if (a.jstate != -1)
    {
        // orig3: 0x1BD758 (the jump +0x320)
        if (a.jumpHold != 0)
        {
            r.lmZ = LM{-125.0f, 200.0f};   // 0x323320
            r.smpZ = SMP{4000.0f, 0.0f, 0.3f};
            r.grf &= ~4u;
        }
        if (a.jumpPhase == 0) r.grf |= 1;
        else if (2 <= a.jumpPhase && a.jumpPhase < 4)
        {
            r.lmZ = LM{-100.0f, 50.0f};    // 0x323338
            r.smpZ = SMP{4000.0f, 0.0f, 2.0f};
            r.grf = (r.grf & ~4u) | 0x10;
        }
    }
    // orig3: 0x1ABA80 (Sly's): the hang (4) / 17 / 19 adjustments are not reached here; 4, 6, 7: moving
    if (a.state == 4 || a.state == 6 || a.state == 7) r.grf |= 1;
}

// orig3: 0x273AB0 PosCmeFocus (the focus between the record and its look-ahead, then the zone's coordinates with
// the entry's y lanes: the identity for a zone without a path)
glm::vec3 PosCmeFocus3(const CME3& cme, const CPDEFI3& cpdefi, const CPFOLLOW3* p)
{
    glm::vec3 pos = cpdefi.pos + cpdefi.dposAhead * (cme.uFocus - 1.0f);
    if (p && p->pzone)
    {
        // ConvertZonePos 0x274270 / PosZoneSample 0x274390 with the coefficients (0, vecCoefY[i], 0), the polar flag
        // off, kind 1, dsPath 0, in the zone's frame (TODO(sly3): a zone path +0x108)
        const ZONE3& z = *p->pzone;
        const glm::vec3 d = pos - z.pos;
        glm::vec3 q(glm::dot(z.mat[0], d), glm::dot(z.mat[1], d), glm::dot(z.mat[2], d));
        for (int i = 0; i < 3; ++i) q[i] = Clamp(q[i] * cme.vecCoefY[i], cme.alm[i]);
        pos = z.pos + z.mat[0] * q.x + z.mat[1] * q.y + z.mat[2] * q.z;
    }
    return pos;
}

// orig3: 0x166E48 SetCmLookAt (the pan / tilt from the eye to the target plus the offsets)
void SetCmLookAt3(CM3* pcm, const glm::vec3& posEye, const glm::vec3& posAt, float dPan, float dTilt)
{
    float pan, tilt;
    ConvertVectorToSphere(posAt - posEye, &pan, &tilt, nullptr);
    pcm->mat = BuildPanTiltMatrix(pan + dPan, tilt + dTilt);
    pcm->pos = posEye;
}

// orig3: 0x276510 (the eye blocked for 0.5 s cuts; paused / cut: the timers reset). TODO(sly3): its ray check of the
// focus every 0.5 s (the lost focus's cut) is not ported.
void CheckFocus3(CPFOLLOW3* p, CMSTATE3* pcms, float t)
{
    if (pcms->fCut)
    {
        p->tVisible = p->tClear = t;
        return;
    }
    if (p->pzone && p->pzone->fNoAvoid) return;
    if (0.5f < t - p->tClear) pcms->fCut = 1;   // CutCm 0x166548
}
}  // namespace

ZONE3* PzoneBeach3()
{
    static ZONE3 s_zone = [] {
        ZONE3 z;
        z.oid = 34523;
        return z;
    }();
    return &s_zone;
}

// orig3: 0x1F3ED8
void GetFocusCpdefi3(CPFOLLOW3* p, const ACTORCM3& actor, bool fFirst, const CMCLOCK3& clk, CPDEFI3* pcpdefi)
{
    CPDEFI3& c = *pcpdefi;
    c = CPDEFI3();
    const float dt = clk.t - p->tRec;
    p->tRec = clk.t;
    if (actor.fFirst) fFirst = true;
    CPDEFIRAW3 r;
    GetSlyCpdefiRaw3(actor, clk.t, &r);
    if (fFirst) r.grf &= ~6u;
    c.cfk = r.cfk;
    c.radHome = r.radHome;
    c.tMoveLast = r.tMoveLast;
    c.dTilt = r.dTilt;
    c.dDist = r.dDist;
    c.grf = r.grf;
    if (!(r.grf & 2))
    {
        const float zObj = actor.pos.z;
        if (!(r.grf & 4))
        {
            float z = (r.grf & 0x10) && !fFirst ? p->posRec.z : r.pos.z;
            if (z < zObj + r.lmZ.gMin) z = zObj + r.lmZ.gMin;
            else if (zObj + r.lmZ.gMax < z) z = zObj + r.lmZ.gMax;
            p->posRec.z = fFirst ? z : GSmooth(p->posRec.z, z, dt, &r.smpZ, nullptr);
        }
        p->posRec.x = r.pos.x;
        p->posRec.y = r.pos.y;
        if (p->posRec.z < zObj + r.lmZ2.gMin) p->posRec.z = zObj + r.lmZ2.gMin;
        else if (zObj + r.lmZ2.gMax < p->posRec.z) p->posRec.z = zObj + r.lmZ2.gMax;
    }
    if (fFirst) p->dposSmooth = glm::vec3(0.0f);
    else p->dposSmooth = PosSmooth(p->dposSmooth, glm::vec3(0.0f), dt, &c_smpOffset, nullptr);
    // the look-ahead: along the facing, shorter while the focus turns fast (the rotation record +0x4C's speed)
    const float r0 = r.sAhead * Clamp(Quad(c_aAhead, actor.wAng), c_lmUnit);
    const glm::vec3 dposAheadT = SetVectorCylind(c.radHome + PI, r0, 0.0f);
    if (fFirst)
    {
        p->dposAhead = dposAheadT;
        p->vAhead = glm::vec3(0.0f);
        p->dposFocus = r.dposFocus;
        p->vFocus = glm::vec3(0.0f);
    }
    else
    {
        SpringFocus3(dposAheadT, &p->dposAhead, &p->vAhead, r.kDamp, r.kSpring, dt);
        SpringFocus3(r.dposFocus, &p->dposFocus, &p->vFocus, r.kDamp, r.kSpring, dt);
    }
    c.pos = p->posRec + p->dposSmooth + p->dposAhead;
    c.v = r.v;
    c.dposAhead = p->dposAhead;
    c.dposFocus = p->dposFocus;
}

// orig3: 0x274AC0
void UpdateCpfollow3(CPFOLLOW3* p, const ACTORCM3& actor, CMSTATE3* pcms, const JOYPS2& joy, const CMCLOCK3& clk)
{
    CM3* pcm = p->pcm;
    ZONE3* pz = p->pzone;
    const CME3 cmeDefault;
    const CME3& cme = pz ? pz->cme : cmeDefault;
    const float t = clk.t, dt = clk.dt, dtReal = clk.dtReal;
    // (0x1662A8 / 0x2747D0: a new zone re-activates the policy; the beach has one)
    CPDEFI3 cpdefi;
    GetFocusCpdefi3(p, actor, pcms->fCut != 0, clk, &cpdefi);
    CheckFocus3(p, pcms, t);
    const int grf = pz ? pz->grf : 4;
    const ZRET3& zret = c_zretDefault;   // (the zone +0x130; the beach's is the default 0x34A040)
    // (0x274A50 UpdateZonePath: only with a zone path)
    const glm::vec3 posFocus = PosCmeFocus3(cme, cpdefi, p) + cpdefi.dposFocus;   // sp+0x70
    if (pz && pz->ploLook) cpdefi.grf &= ~1u;
    p->uLead = GSmooth(p->uLead, (cpdefi.grf & 1) ? 0.0f : cme.uLead, dt, &c_smpLead, nullptr);
    p->tMoveLast = std::max(cpdefi.tMoveLast, p->tMoveLast);
    float s1[3] = {0, 0, 0}, s2[3] = {0, 0, 0};   // sp+0x140 / sp+0x14C
    // TODO(sly3): the zone's samples (grf & 3: 0x274270 / 0x274390 at sPath -/+ dtPath)
    if (p->fFirst) p->cpmode = 4;
    if (pcms->fCut)
    {
        if (pcms->fCut == 2)
        {
            p->cpmode = 4;
            p->radPan = pcms->radCutPan;
            p->radTilt = GLimitLm(&c_lmTiltTurn, pcms->radCutTilt - std::atan(cme.dyScreen * pcms->yScreenRange));
        }
        else if (!(grf & 3))
        {
            p->cpmode = 4;
            p->radPan = cpdefi.radHome;
            p->radTilt = (cme.lmTilt.gMin + cme.lmTilt.gMax) * 0.5f;
        }
        else if (!(grf & 2)) p->cpmode = 1;
        else if (!(grf & 1)) p->cpmode = 2;
        else
            p->cpmode = std::fabs(RadNormalize(s1[0] - cpdefi.radHome)) < std::fabs(RadNormalize(s2[0] - cpdefi.radHome)) ? 1 : 2;
        p->tMoveLast = t;
        p->tManual = t - 10.0f;
    }
    if (!(p->cpmode & grf) && 0 < p->cpmode && p->cpmode < 3) p->cpmode = 4;
    const glm::vec3 vLead = cpdefi.v;   // sp+0x90 (TODO(sly3): toward the zone's look object +0x144)
    float pan = p->radPan, tilt = p->radTilt;   // f26 / f28
    bool fTurned = false;
    if ((grf & 4) && !(cpdefi.grf & 8) && (0.0f < joy.uDeflect2 || p->dradPan != 0.0f || p->dradTilt != 0.0f))
    {
        // the right stick turns the camera (0x1A7B38: the stick's turn rates, TODO(sly3): re-derive; Sly 2's
        // x |x| 3.0 / y |y| 1.5 here), at most 15 / s^2 of change
        const float dradPanT = joy.x2 * std::fabs(joy.x2) * 3.0f, dradTiltT = joy.y2 * std::fabs(joy.y2) * 1.5f;
        const float dd = 15.0f * dtReal;
        p->dradPan = std::clamp(dradPanT, p->dradPan - dd, p->dradPan + dd);
        p->dradTilt = std::clamp(dradTiltT, p->dradTilt - dd, p->dradTilt + dd);
        const float ang = std::atan2(std::fabs(joy.y2), std::fabs(joy.x2));
        if (ang < F32(0x3E860A92))
        {
            p->dradTilt = 0.0f;
            p->fManual = 1;
        }
        else if (F32(0x3FA78D36) < ang) p->dradPan = 0.0f;
        else p->fManual = 1;
        p->fLeftSamples = 1;
        pan = RadNormalize(pan + p->dradPan * dtReal);
        tilt = GLimitLm(&c_lmTiltTurn, RadNormalize(tilt + p->dradTilt * dtReal));
        p->cpmode = 4;
        p->tManual = t;
        fTurned = true;
    }
    if (!fTurned && p->cpmode == 4)
    {
        // the passive pan: the focus's motion across the view
        const glm::vec3 dirStick = (cpdefi.grf & 8) ? glm::vec3(1, 0, 0) : NormalizeOr(glm::vec3(joy.x, joy.y, 0.0f), glm::vec3(1, 0, 0));
        glm::vec3 dFocus = posFocus - pcm->pos;
        dFocus.z *= F32(0x3E99999A);
        const float lFocus = std::sqrt(dFocus.x * dFocus.x + dFocus.y * dFocus.y + dFocus.z * dFocus.z);
        dFocus = lFocus < c_eps ? glm::vec3(1, 0, 0) : dFocus * (1.0f / lFocus);
        const int cfk = (pz && pz->cfk != -1) ? pz->cfk : cpdefi.cfk;
        glm::vec3 dirSide(0.0f);
        if (cfk == 0) dirSide = glm::cross(glm::vec3(0, 0, 1), dFocus);
        else if (cfk == 1) dirSide = dFocus;
        dirSide = NormalizeOr(dirSide, glm::vec3(1, 0, 0));
        const float g0 = GLimitAbs(glm::dot(vLead, dirSide) / lFocus, 2.5f);
        const float g5 = Clamp(Quad(c_aLeadStick, dirStick.y), c_lmLeadStick);
        const float g3 = Clamp(Quad(c_aLeadTime, t - p->tManual), c_lmUnit);
        const float panPassive = RadNormalize(pan + g0 * p->uLead * g5 * g3 * dt);   // f29
        float uSample = 0.0f, panTurn = panPassive;     // f25 / f21
        float tiltTarget = tilt;                       // f27
        if (!(grf & 3)) tiltTarget = GLimitLm(&cme.lmTilt, tilt) + cpdefi.dTilt;
        // (TODO(sly3): the turn toward the samples, grf & 3)
        pan = RadNormalize(pan + (1.0f - uSample) * RadNormalize(panPassive - pan) + uSample * RadNormalize(panTurn - pan));
        float uRate = Clamp(Quad(c_aTiltRate, std::sqrt(vLead.x * vLead.x + vLead.y * vLead.y)), c_lmUnit);   // f22
        float uTilt = uRate;   // f24
        // (zret +0x24: TODO(sly3), 0 on the beach)
        if (zret.f18 && !p->fLeftSamples)
        {
            float u = zret.u20;
            if (0.0f < zret.dt1C) u *= GLimitLm(&c_lmUnit, (t - p->tMoveLast) / zret.dt1C);   // (+0xE4: TODO(sly3) which time)
            if (uTilt < u) uTilt = u;
        }
        const SMP smp{c_smpTilt.svFast * uTilt, c_smpTilt.svSlow * uTilt, c_smpTilt.dtFast};
        tilt = RadSmooth(tilt, tiltTarget, dt, &smp, nullptr);
    }

    // the eye's target by the mode
    float aeye[3] = {0, 0, 0};   // sp+0x164
    float dxScreen = 0.0f, dyScreenT = 0.0f;   // f31 / f30
    if (p->cpmode == 1)
    {
        aeye[0] = s1[0]; aeye[1] = s1[1]; aeye[2] = s1[2];
        dyScreenT = cme.dyScreen;
        dxScreen = cme.dxScreen;
    }
    else if (p->cpmode == 2)
    {
        aeye[0] = s2[0]; aeye[1] = s2[1]; aeye[2] = s2[2];
        dxScreen = -cme.dxScreen;
        dyScreenT = cme.dyScreen;
    }
    else if (p->cpmode == 4)
    {
        aeye[0] = pan;
        aeye[1] = tilt;
        dyScreenT = cme.dyScreen;
        dxScreen = cme.dxScreen;
        aeye[2] = std::fabs(cme.dtPath) + cpdefi.dDist;   // (TODO(sly3): the blend between the samples with grf & 3)
    }
    bool fFar = false;   // s0
    if (pcms->fCut)
    {
        p->dsDist = 0.0f;
        p->ddyScreen = 0.0f;
    }
    else
    {
        fFar = 400.0f < p->sDist - aeye[2];
        const float rDamp = 1.0f + cme.rDamping * (dt * -8.0f);
        const float rSpring = cme.rSpring * (dt * 15.0f);
        p->dsDist = rDamp * p->dsDist + (aeye[2] - p->sDist) * rSpring;
        p->ddyScreen = rDamp * p->ddyScreen + (dyScreenT - p->dyScreen) * rSpring;
        aeye[2] = p->sDist + p->dsDist * dt;
        dyScreenT = p->dyScreen + p->ddyScreen * dt;
    }
    glm::vec3 dposEye = SetVectorSphere(aeye[0], aeye[1], aeye[2]);
    glm::vec3 posEyeNew = posFocus + dposEye;
    bool fBlocked = false;   // sp+0x160
    // TODO(sly3): the eye's avoidance 0x167660 (SquishCmEye 0x1672A0 over the solids about the eye) and the cut's
    // PosEyeCut 0x276900; the eye goes to its target
    if (!fBlocked || !fFar) p->tClear = t;
    dposEye = posEyeNew - posFocus;
    ConvertVectorToSphere(dposEye, &aeye[0], &aeye[1], &aeye[2]);
    p->posEye = posEyeNew;
    posEyeNew = posFocus + dposEye * ((aeye[2] + pcms->sNearClip) / aeye[2]);
    const float dPanScreen = std::atan(dxScreen * pcms->xScreenRange);     // f23
    const float dTiltScreen = -std::atan(dyScreenT * pcms->yScreenRange);  // f22
    if (p->fFirst)
    {
        if (pcms->fCut) p->dradPanOffset = p->dradTiltOffset = p->ddradPanOffset = p->ddradTiltOffset = 0.0f;
        else
        {
            float r, z;
            ConvertVectorToCylind(dposEye, nullptr, &r, &z);
            float panCm, tiltCm;
            DecomposeRotateMatrixPanTilt(pcm->mat, &panCm, &tiltCm);
            const float panLook = RadNormalize(aeye[0] + PI + dPanScreen);
            const float tiltLook = RadNormalize(std::atan2(-z, r) + dTiltScreen);
            p->dradPanOffset = RadNormalize(panCm - panLook);
            p->dradTiltOffset = RadNormalize(tiltCm - tiltLook);
            p->ddradPanOffset = p->ddradTiltOffset = 0.0f;
        }
    }
    else
    {
        p->ddradPanOffset += (p->dradPanOffset * -10.0f + p->ddradPanOffset * -5.0f) * dt;
        p->ddradTiltOffset += (p->dradTiltOffset * -10.0f + p->ddradTiltOffset * -5.0f) * dt;
        p->dradPanOffset += p->ddradPanOffset * dt;
        p->dradTiltOffset += p->ddradTiltOffset * dt;
    }
    SetCmLookAt3(pcm, posEyeNew, posFocus, dPanScreen + p->dradPanOffset, dTiltScreen + p->dradTiltOffset);
    p->dyScreen = dyScreenT;
    p->radPan = aeye[0];
    p->radTilt = aeye[1];
    p->sDist = aeye[2];
    p->radPan1 = s1[0];
    p->radTilt1 = s1[1];
    p->radPan2 = s2[0];
    p->radTilt2 = s2[1];
    p->fFirst = 0;
    pcms->fCut = 0;   // (the cut lasts one frame: the camera's update clears it)
}
}  // namespace sly3
