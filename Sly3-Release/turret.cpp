// turret.cpp: the aimed player object (cid 0x45, vtable 0x2DBD80, 0x1E0 bytes; ports of the original's
// 0x2259D8..0x226370): a gun the player sits in (world 8's job 2012: Murray's turret 28548, made the current player by
// its instance's splice (option 806 = 1)). The pad turns it (pan / tilt rates, limited), the right stick zooms (the
// camera's field of view through the attach camera policy, CM +0x5F0), any button fires a row of its pool through its
// launcher and sends its splice event 5.
#include "turret.h"
#include "game/sly3.h"

#include "activate.h"
#include "savedata.h"
#include "alo.h"
#include "animfind.h"
#include "clock.h"
#include "cm.h"
#include "cplcy.h"
#include "lo.h"
glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);  // mat.h (0x11B7E0)
#include "objmethod.h"
#include "pcinput.h"
#include "po.h"
#include "ref.h"
#include "so.h"
#include "sw.h"
#include "update.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
void PredictXfChain(XF* pxf, XF* pxfStop, float dt, glm::mat4* pmat, glm::mat4* pdmat);  // xfphys.h (0x2309E8)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

using cmm::F32;

void LaunchFrom(LO* plaunch, LO* po);  // launcher.cpp (0x1D6B00)

int OverlapKindAlo(ALO* pa, ALO* pb);  // bbmark.cpp: 0x1F4FC8

namespace
{
struct TURRET
{
    LO* ptarget = nullptr;   // +0x180 (option 910): an object it aims / zooms at by itself
    int fAim = 0;            // +0x184 (912)
    int fFollow = 0;         // +0x188 (911)
    float radPan = 0.0f;     // +0x18C (917)
    float radTilt = 0.0f;    // +0x190 (918)
    float dradPan = 0.0f;    // +0x194
    float dradTilt = 0.0f;   // +0x198
    cmm::LM lm19C{-0.7f, 0.7f};    // +0x19C (325) (as the init sets them: 0x2259D8)
    cmm::LM lm1A4{-1.5f, 1.5f};   // +0x1A4 (913)
    cmm::LM lmFov{0.1f, 1.0f};     // +0x1AC (922; D_002C4400)
    float uZoom = 0.0f;      // +0x1B4
    float duZoom = 0.0f;     // +0x1B8
    float radFov = 1.0f;     // +0x1BC
    LO* ppool = nullptr;     // +0x1C0 (914): the projectiles' proxy
    LO* plaunch = nullptr;   // +0x1C4 (915): the launcher (cid 0x70)
    LO* plast = nullptr;     // +0x1C8 (916, get only): the last fired
    int fRotate = 1;         // +0x1CC (919; setter 0x226370): the rotation controller made
    LO* pmount = nullptr;    // +0x1D0 (920): the camera's mount (null: itself)
    int fSmooth = 1;         // +0x1D4 (921)
    CTRLPOS* pctrl = nullptr;  // +0x1D8
};

std::unordered_map<const LO*, TURRET> s_mpturret;

bool FTurret(const LO* plo) { return plo && plo->pcls && plo->pcls->PfnOrig(16) == 0x225B60; }

TURRET* Pturret(const LO* plo)
{
    if (!FTurret(plo)) return nullptr;
    return &s_mpturret[plo];  // (0x2259D8's defaults on first use)
}

LO* PloOptObject(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindSwNearest(plo, v.n);
}

LO* PloOfRef(const CRef& r) { return r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr; }
int NOfRef(const CRef& r) { return r.m_tagk == TAGK_Bool ? r.m_bool : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_n; }
float GOfRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : float(r.m_n); }

// orig: 0x00226370 (option 919's setter): the rotation controller on (a position controller of mode 5 on the rotation
// record, prio 1, state 3, at (0, -tilt, pan)) / off
void SetTurretRotate(ALO* palo, TURRET& t, int f)
{
    if (t.fRotate == f) return;
    t.fRotate = f;
    if (f) t.pctrl = PctrlNewPos(palo, PxfRot(palo), 1, 5, 3, glm::vec4(0.0f, -t.radTilt, t.radPan, 0.0f));
    else
    {
        CTRL* p = t.pctrl;
        DeleteCtrl(&p);
        t.pctrl = nullptr;
    }
}

// orig: 0x00225BA0 (the aim: toward the target, or the pad's rates)
void UpdateTurretAim(ALO* palo, TURRET& t)
{
    const PODATA* ppd = PpodataFromLo(palo);
    const PADCTL* ppc = ppd ? ppd->ppadctl : nullptr;
    if (t.ptarget && t.fAim)
    {
        const glm::mat3 m = BuildLookAtMatrixZ(PosLoWorld(t.ptarget) - PosLoWorld(palo));  // 0x11B7E0
        float radPan, radTilt;
        cmm::DecomposeRotateMatrixPanTilt(m, &radPan, &radTilt);
        const float dPan = radPan - t.radPan, dTilt = radTilt - t.radTilt;
        t.radPan = radPan;
        t.radTilt = radTilt;
        t.dradPan = dPan;
        t.dradTilt = dTilt;
    }
    else if (ppc)
    {
        const float x = cmm::GLimitAbs(ppc->x + ppc->x2, 1.0f);
        const float y = cmm::GLimitAbs(ppc->y, 1.0f);
        const float u = t.radFov;
        const float gGain = (1.0f - u) * 0.5f + u;
        const float gAccel = gGain * 10.0f;
        const float ax = -x * x * x * gAccel;
        const float gLimit = u * 1.3f;
        if (ax * t.dradPan <= 0.0f) t.dradPan = 0.0f;
        float ay = y * y * y * gAccel;
        {
            const CRef r = RefGetProgress({"flags", "fReverseAim"});  // 0x2581C0(D_003E0FE0): the settings' reverse aim
            if (!(r.m_tagk == TAGK_Bool ? r.m_bool : r.m_tagk == TAGK_S32 ? r.m_n : 0)) ay = -ay;
        }
        if (ay * t.dradTilt <= 0.0f) t.dradTilt = 0.0f;
        static const cmm::SMP s_smpDamp{F32(0x40A00000), 0.0f, F32(0x3F000000)};  // D_002C4408
        const float dt = g_clock.dt;
        t.dradPan = cmm::GSmooth(t.dradPan, 0.0f, dt, &s_smpDamp, nullptr);
        t.dradTilt = cmm::GSmooth(t.dradTilt, 0.0f, dt, &s_smpDamp, nullptr);
        t.dradPan = cmm::GLimitAbs(t.dradPan + ax * dt, gLimit * 0.9f);
        t.dradTilt = cmm::GLimitAbs(t.dradTilt + ay * dt, gLimit * 0.8f);
        t.radPan += t.dradPan * dt;
        t.radTilt += t.dradTilt * dt;
        // (PC, N14) the mouse aims straight onto the angles at the aim sensitivity: right turns right, up looks up (a
        // higher tilt; only the mouse's own turret inversion flips it (settings v17 mouseInvertTurretX / Y), the game's
        // "Invert Turret Controls" is the stick's). Checked by the camera's forward: mouse up raises it.
        float dxRight = 0.0f, dyUp = 0.0f;
        MouseAimDelta(MOUSEAIM_Turret, &dxRight, &dyUp);
        t.radPan -= dxRight;
        t.radTilt += dyUp;
        if (static const bool s_f = std::getenv("SLY2N_TURRETPRINT") != nullptr; s_f)
            std::printf("turret: t %.3f pan %.4f tilt %.4f stick %.2f %.2f mouse %.4f %.4f cam x %.3f %.3f %.3f\n", g_clock.t, t.radPan, t.radTilt, x, y, dxRight, dyUp, g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z);
    }
    t.radPan = cmm::GLimitLm(&t.lm1A4, t.radPan);
    t.radTilt = cmm::GLimitLm(&t.lm19C, t.radTilt);
    if (t.fRotate && t.pctrl) SetCtrlPos(t.pctrl, glm::vec4(0.0f, -t.radTilt, t.radPan, 0.0f));  // 0x12D108
}

// orig: 0x00225E98 (the zoom: framing the target, or the right stick)
void UpdateTurretZoom(ALO* palo, TURRET& t)
{
    static const cmm::LM s_lmUnit{0.0f, 1.0f};  // D_002BBB60
    const float dt = g_clock.dt;
    const PODATA* ppd = PpodataFromLo(palo);
    const PADCTL* ppc = ppd ? ppd->ppadctl : nullptr;
    if (t.ptarget && t.fFollow)
    {
        // TODO: the target's shape radius (+0xEC +0x30); natively 0 (no turret of the game's sets 910 / 911)
        const float sRadius = 0.0f;
        const glm::vec3 posSelf = PosLoWorld(palo);
        const glm::vec3 posTarget = PosLoWorld(t.ptarget);
        float rad = std::atan(sRadius / (glm::length(posSelf - posTarget) * 0.5f));
        rad = rad + rad;
        if (rad < t.lmFov.gMin) rad = t.lmFov.gMin;
        glm::mat4 mPredict(1.0f);
        if (FCidDerivedFrom(t.ptarget->Cid(), S3CID(0x30)) && PxfObj(static_cast<ALO*>(t.ptarget)))
            PredictXfChain(PxfObj(static_cast<ALO*>(t.ptarget)), nullptr, dt, &mPredict, nullptr);  // 0x2309E8
        else mPredict[3] = glm::vec4(posTarget, 1.0f);
        const glm::vec3 posFuture(mPredict[3]);
        const float sExtent = sRadius + glm::length(posFuture - posTarget);
        glm::mat3 mRot(1.0f);
        if (PxfObj(palo)) mRot = glm::mat3(PxfObj(palo)->matWorld);  // 0x1A6318
        const glm::vec3 l = glm::transpose(mRot) * (posSelf - posFuture);
        const glm::vec3 sw(-l.y, l.z, l.x);
        float radWant;
        if (l.x > 0.0001f)
        {
            float h = std::atan((std::fabs(sw.x) + sExtent) / (sw.z * g_cm.rAspect));
            h = h + h;
            float v = std::atan((std::fabs(sw.y) + sExtent) / sw.z);
            v = v + v;
            radWant = std::min(std::max(h, v), t.lmFov.gMax);
        }
        else radWant = t.lmFov.gMax;
        radWant = std::max(radWant, rad);
        const float uFrac = t.lmFov.gMin == t.lmFov.gMax ? 0.0f : (radWant - t.lmFov.gMin) / (t.lmFov.gMax - t.lmFov.gMin);
        const float uTarget = cmm::GLimitLm(&s_lmUnit, 1.0f - uFrac);
        const float d = std::fabs(uTarget - t.uZoom);
        cmm::SMPA smpa{F32(0x3E4CCCCD), 0.0f, F32(0x3F000000), 1.0f};  // D_002A97B0
        const float rRate = (1.0f - d) * 1.0f + d * 4.0f;            // D_002C4418
        smpa.sdvMax *= rRate;
        smpa.svFast *= rRate;
        smpa.svSlow *= rRate;
        t.uZoom = cmm::GSmoothA(t.uZoom, t.duZoom, uTarget, dt, &smpa, &t.duZoom);
    }
    else if (ppc)
    {
        const float u = t.uZoom, g = ppc->y2;
        t.duZoom = (2.0f + u * (0.0f + u * -1.0f)) * g * std::fabs(g);  // D_002C4420
        t.uZoom = cmm::GLimitLm(&s_lmUnit, u + t.duZoom * dt);
    }
    const float uBlend = 1.0f - t.uZoom;
    t.radFov = (1.0f - uBlend) * t.lmFov.gMin + uBlend * t.lmFov.gMax;
}

// orig: 0x00226250 (fire: any of the eight buttons pressed this frame (0x1C4700, taken): a free row of the pool
// (0x1CB250) thrown by the launcher (0x1D6B00), then the splice event 5)
void UpdateTurretFire(ALO* palo, TURRET& t)
{
    const PODATA* ppd = PpodataFromLo(palo);
    PADCTL* ppc = ppd ? ppd->ppadctl : nullptr;
    if (!ppc || !GrfPadctlTakePressed(ppc, 0xFF, 0.0f)) return;
    if (t.ppool)
    {
        LO* po = PloAcquireProxyObject(g_psw, t.ppool, false);
        if (!po) return;
        t.plast = po;
        if (t.plaunch) LaunchFrom(t.plaunch, po);
        if (std::getenv("SLY2N_TURRETPRINT"))
            std::printf("turret %d: fired %d (launcher %d) t %.3f\n", palo->oid, po->oid, t.plaunch ? t.plaunch->oid : -1, g_clock.t);
    }
    HandleLoSpliceEvent(palo, 5, false, 0, nullptr);  // vt slot 3 (0x1A61E0)
}

// orig: 0x00225B60 (vt slot 16)
void UpdateTurret(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's
    TURRET* pt = Pturret(plo);
    if (!pt) return;
    ALO* palo = static_cast<ALO*>(plo);
    // (test only: SLY2N_TURRETAIM=oid aims the turret at that object (as options 910 / 912 would), to test hits fast)
    if (static const char* s_sz = std::getenv("SLY2N_TURRETAIM"); s_sz && NPoEnabled(plo) == 1)
        if (LO* p = PloFindSwNearest(plo, std::atoi(s_sz)))
        {
            // (the turn in the parent's frame: the rotation controller's angles are local)
            glm::mat3 mParent(1.0f);
            if (palo->paloParent && PxfObj(palo->paloParent)) mParent = glm::mat3(PxfObj(palo->paloParent)->matWorld);
            // (SLY2N_TURRETAIMDZ: that much above the object, e.g. the harpoon's drop)
            const float dz = std::getenv("SLY2N_TURRETAIMDZ") ? std::strtof(std::getenv("SLY2N_TURRETAIMDZ"), nullptr) : 0.0f;
            const glm::vec3 d = glm::transpose(mParent) * (PosLoWorld(p) + glm::vec3(0.0f, 0.0f, dz) - PosLoWorld(palo));
            cmm::DecomposeRotateMatrixPanTilt(BuildLookAtMatrixZ(d), &pt->radPan, &pt->radTilt);
            pt->dradPan = pt->dradTilt = 0.0f;
        }
    // (test only: SLY2N_TURRETSET="pan tilt[;pan tilt@T..]" holds the turret's angles, as the reference's pokes of
    // +0x18C / +0x190; an entry with @T applies from game clock T on)
    if (static const char* s_szSet = std::getenv("SLY2N_TURRETSET"); s_szSet && NPoEnabled(plo) == 1)
    {
        float pan = 0.0f, tilt = 0.0f;
        bool f = false;
        for (const char* q = s_szSet; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            float p0, t0, tFrom = 0.0f;
            const int c = std::sscanf(q, "%f %f@%f", &p0, &t0, &tFrom);
            if (c >= 2 && (c < 3 || g_clock.t >= tFrom)) { pan = p0; tilt = t0; f = true; }
        }
        if (f)
        {
            pt->radPan = pan;
            pt->radTilt = tilt;
            pt->dradPan = pt->dradTilt = 0.0f;
        }
    }
    UpdateTurretAim(palo, *pt);
    if (static const char* s_szP = std::getenv("SLY2N_TURRETPRINT"); s_szP && s_szP[0] == '2' && NPoEnabled(plo) == 1)
        std::printf("turret %d t %.3f: pan %.5f tilt %.5f (rates %.4f %.4f) fov %.4f\n", plo->oid, g_clock.t, pt->radPan, pt->radTilt,
                    pt->dradPan, pt->dradTilt, pt->radFov);
    UpdateTurretZoom(palo, *pt);
    UpdateTurretFire(palo, *pt);
}

// orig: 0x00226310 (vt slot 61): the base overlap kind, at least 1 (no response) with the pool's own objects
int OverlapKindTurret(ALO* pa, ALO* pb)
{
    int r = OverlapKindAlo(pa, pb);
    const TURRET* pt = Pturret(pa);
    if (pt && pt->ppool && pb)
    {
        // (+0x3C: the namespace node; the pool's rows are its group's objects)
        auto it = g_psw->mpproxy.find(pt->ppool);
        if (it != g_psw->mpproxy.end() && it->second.iwe >= 0 && PloRoot(pb)->ichannel == it->second.iwe) r = std::max(r, 1);
    }
    return r;
}
}  // namespace

NATIVEFN(0x226310, OverlapKindTurret);

// orig: 0x00225AA8 (vt slot 15, after the load's options): the base's (0x1F36A8, done by the caller), then the
// rotation controller made when option 919 is on (0x226370)
void PostLoadTurret(LO* plo)
{
    TURRET* pt = Pturret(plo);
    if (!pt) return;
    TURRET& t = *pt;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval[0];
        switch (o.eopid)
        {
        case 997: t.ptarget = PloOptObject(plo, v); break;
        case 998: t.fFollow = v.n; break;
        case 999: t.fAim = v.n; break;
        case 1000: t.lm19C = {v.ag[0], v.ag[1]}; break;
        case 1001: t.lm1A4 = {v.ag[0], v.ag[1]}; break;
        case 1002: t.ppool = PloOptObject(plo, v); break;
        case 1003: t.plaunch = PloOptObject(plo, v); break;
        case 1005: t.radPan = v.ag[0]; break;
        case 1006: t.radTilt = v.ag[0]; break;
        case 1007: t.fRotate = v.n; break;
        case 1008: t.pmount = PloOptObject(plo, v); break;
        case 1009: t.fSmooth = v.n; break;
        case 1010: t.lmFov = {v.ag[0], v.ag[1]}; break;
        default: break;
        }
    }
    if (std::getenv("SLY2N_TURRETPRINT"))
        std::printf("turret %d: pool %d launcher %d mount %d target %d aim %d follow %d rotate %d pan (%g %g) tilt (%g %g) fov (%g %g)\n",
                    plo->oid, t.ppool ? t.ppool->oid : -1, t.plaunch ? t.plaunch->oid : -1, t.pmount ? t.pmount->oid : -1,
                    t.ptarget ? t.ptarget->oid : -1, t.fAim, t.fFollow, t.fRotate, t.lm19C.gMin, t.lm19C.gMax, t.lm1A4.gMin,
                    t.lm1A4.gMax, t.lmFov.gMin, t.lmFov.gMax);
    t.radFov = t.lmFov.gMax;
    if (t.fRotate)
    {
        t.fRotate = 0;
        SetTurretRotate(static_cast<ALO*>(plo), t, 1);
    }
}

// orig: 0x00225AE8 (vt slot 78 after the PO's 0x1C3FF0): the attach camera (CM +0x5F0) at priority 9 while current
void SetTurretFocus(LO* plo, bool fOn)
{
    if (!FTurret(plo)) return;
    if (fOn) SetCmPolicy(&g_cm, 9, &g_cmpolicies.cpattach, plo, nullptr);
    else RevokeCmPolicy(&g_cm, 0xF, 9, &g_cmpolicies.cpattach, plo, nullptr);
}

// orig: 0x00151950's mount: vt slot 31 of +0x1D0 (itself by default), smoothed by +0x1D4, the field of view +0x1BC
bool FTurretAttach(LO* plo, CMATTACH* patt)
{
    TURRET* pt = Pturret(plo);
    if (!pt) return false;
    LO* pm = pt->pmount ? pt->pmount : plo;
    glm::mat4 m(1.0f);
    if (FCidDerivedFrom(pm->Cid(), S3CID(0x30)))
    {
        ALO* pa = static_cast<ALO*>(pm);
        if (PxfObj(pa)) m = PxfObj(pa)->matWorld;
    }
    else if (const LOX* plox = dynamic_cast<const LOX*>(pm)) m = plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    patt->matMount = m;
    patt->fSmooth = pt->fSmooth != 0;
    patt->radFOV = pt->radFov;
    return true;
}

// (0x225A68, vt slot 41, the clone: a mount that was the original becomes the copy: natively a null mount is itself)
void ResetTurrets() { s_mpturret.clear(); }

namespace
{
struct INITTURRET
{
    INITTURRET()
    {
        RegisterLoUpdate(0x225B60, UpdateTurret);
        // the options a script sets / reads at run time (EOPIDs 997..1010: fields +0x180..+0x1D4)
        RegisterObjectSetter(997, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->ptarget = PloOfRef(r); });
        RegisterObjectSetter(998, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->fFollow = NOfRef(r); });
        RegisterObjectSetter(999, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->fAim = NOfRef(r); });
        RegisterObjectSetter(1002, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->ppool = PloOfRef(r); });
        RegisterObjectSetter(1003, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->plaunch = PloOfRef(r); });
        RegisterObjectSetter(1005, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->radPan = GOfRef(r); });
        RegisterObjectSetter(1006, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->radTilt = GOfRef(r); });
        RegisterObjectSetter(1007, [](LO* plo, const CRef& r) {
            if (TURRET* pt = Pturret(plo)) SetTurretRotate(static_cast<ALO*>(plo), *pt, NOfRef(r) != 0);
        });
        RegisterObjectSetter(1008, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->pmount = PloOfRef(r); });
        RegisterObjectSetter(1009, [](LO* plo, const CRef& r) { if (TURRET* pt = Pturret(plo)) pt->fSmooth = NOfRef(r); });
        RegisterObjectGetter(1004, [](LO* plo) {
            CRef r;
            const TURRET* pt = Pturret(plo);
            r.SetBasic(pt ? pt->plast : nullptr);
            return r;
        });
        RegisterObjectGetter(1005, [](LO* plo) { CRef r; const TURRET* pt = Pturret(plo); r.SetF32(pt ? pt->radPan : 0.0f); return r; });
        RegisterObjectGetter(1006, [](LO* plo) { CRef r; const TURRET* pt = Pturret(plo); r.SetF32(pt ? pt->radTilt : 0.0f); return r; });
    }
} s_initturret;
}  // namespace
