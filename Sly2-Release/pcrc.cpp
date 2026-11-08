#include "actioninput.h"
// PC addition, not in the original game: the debug menu's playable RC chopper (pcrc.h). Nothing here runs unless the
// debug menu (Debug keys) asks for it.
#include "pcrc.h"
#include "pcloader.h"

#include "activate.h"
#include "alo.h"
#include "breakable.h"
#include "brx.h"
#include "clock.h"
#include "cm.h"
#include "joy.h"
#include "sound.h"
#include "pcinput.h"
#include "lo.h"
#include "po.h"
#include "projectile.h"
#include "guard.h"
#include "actor.h"
#include "damage.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "bsp.h"
#include "sw.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "xf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

extern int g_iframeMain;   // game.cpp

namespace
{
// ------------------------------------------------------------ the retail chopper (world 8's file; also in 14 / 29 / 30)
constexpr int c_sfxRotor = 370;      // the chopper's ambient sound object 6390 (a looper; its record: one sound)
// 6390's sound record (EOPID 99, world 8: SLY2N_SNDLOG's "ambient sound 370"): full volume within 200, silent beyond
// 3000, volume 0.7 (R44: the hum had 600..4000 at 1.0, 7 dB over the authored level at the chase camera's 380 and 13 dB
// over the RC jobs' own top-down view, its noise chops dominating the mix)
constexpr float c_sRotorFull = 200.0f, c_sRotorSilent = 3000.0f, c_gRotor = 0.7f;
constexpr int c_iwrecChopper = 8;    // Episode 2's palace grounds (RC Bombing Run)
constexpr int c_oidProxyGun = 50747, c_oidProxyBomb = 50744;
constexpr int c_oidShot = 50749, c_oidBomb = 50746;   // the projectiles (cid 0x4F) the proxies make

// Tuning. Ep2's script (block 49605, mode 3615): the stick turns the heading toward its direction at option 1538's
// rate and speeds it up to option 1537; here the stick is camera-relative (as the RC jobs' camera follows behind, the
// same thing), R2 / L2 climb and sink.
constexpr float c_sMax = 1100.0f;        // units / s level
constexpr float c_sClimb = 700.0f;       // units / s up / down
constexpr float c_rAccel = 3.0f;         // the velocity's approach to the stick's (1 / s)
constexpr float c_wTurnMax = 2.2f;       // radians / s
constexpr float c_sShot = 2600.0f;       // the gun's shot (option 1378 in Ep6's world)
constexpr float c_dtShot = 0.15f, c_dtBomb = 0.6f;

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_PCRCPRINT") != nullptr;
    return s_f;
}

struct RC
{
    bool fWant = false;             // the menu's choice
    bool fActive = false;
    uint32_t serial = 0;            // the world flown in
    ALO* pchopper = nullptr;
    LO* ploPlayer = nullptr;        // the player that gave the pad
    glm::vec3 dvGravitySaved{0.0f};
    bool fOwnChopper = false;       // the world's own (no copy), given back as it was
    std::shared_ptr<CFrame> pframeSaved;   // its script (block 49605), detached while flown here
    glm::vec3 v{0.0f};              // its velocity (kept here: the contacts' impulses are not fed back)
    float rad = 0.0f;               // the heading
    float radPitch = 0.0f, radRoll = 0.0f;
    float tShotNext = 0.0f, tBombNext = 0.0f;
    // the chase camera (as the RC jobs' camera: behind and above, looking at it)
    bool fCamCut = true;
    glm::vec3 posCam{0.0f};
    float radCamYaw = 0.0f, radCamTilt = 0.0f;   // the player's orbit (right stick / mouse) on top of the heading
    float tCamInput = -100.0f;
    // R3: the RC jobs' top-down camera (UpdateTopCam) instead of the chase camera
    bool fTop = false;
    float uTop = 0.0f;              // the blend between the two (0 chase .. 1 top-down), eased over half a second
    float radTopPan = 0.0f;         // its pan (world-fixed, as the jobs': the heading when chosen; turned by the right stick / mouse)
    glm::vec3 dposLead{0.0f}, vLead{0.0f};   // its focus's lead ahead of the chopper (the camera's adjust spring)
    glm::vec3 posTopEye{0.0f}, posTopFocus{0.0f};
    CSndRef sndRotor;               // the rotor's hum (world 8's ambient object 6390 under the chopper: sfx 370)
    std::vector<ALO*> apaloShot, apaloBomb;   // (cross-world copies; unused while the world's own proxies serve)
} s_rc;

glm::mat3 MatHeading(float rad, float pitch, float roll)
{
    const float c = std::cos(rad), s = std::sin(rad);
    const glm::mat3 mh(glm::vec3(c, s, 0.0f), glm::vec3(-s, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const glm::mat3 mp(glm::vec3(cp, 0.0f, -sp), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(sp, 0.0f, cp));   // nose down: +pitch
    const float cr = std::cos(roll), sr = std::sin(roll);
    const glm::mat3 mr(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, cr, sr), glm::vec3(0.0f, -sr, cr));
    return mh * mp * mr;
}

LO* PloFindIn(SW* psw, int oid, CID cid)
{
    for (const auto& up : psw->aploAll)
        if (up->oid == oid && up->iclone == 0 && FCidDerivedFrom(up->Cid(), cid)) return up.get();
    return nullptr;
}

LO* PloChild(ALO* proot, int oid)
{
    if (!proot) return nullptr;
    for (LO* plo : proot->aploOwned)
        if (plo && plo->oid == oid) return plo;
    return nullptr;
}

// ------------------------------------------------------------ collision with the level
// The world's own geometry (class 0x31's entries, as the spawners' occlusion 0x1B44D8 collects them).
constexpr float c_sRadius = 80.0f;   // the chopper's clearance (its body and rotor: ~70 from its centre)
std::vector<SOE*> s_apsoeLevel;
uint32_t s_serialLevel = 0;

const std::vector<SOE*>& ApsoeLevel(SW* psw)
{
    if (s_serialLevel != psw->serial)
    {
        s_serialLevel = psw->serial;
        s_apsoeLevel.clear();
        for (LO* plo : psw->mpcidaplo[0x31])
            for (SOE& soe : static_cast<ALO*>(plo)->asoe)
                if (!soe.f48) s_apsoeLevel.push_back(&soe);
    }
    return s_apsoeLevel;
}

// The velocity v for the next step from pos, slid along the level's surfaces it would come within c_sRadius of (three
// passes: a corner), and pushed out where the chopper is already that close (below, above).
glm::vec3 VClipLevel(SW* psw, const glm::vec3& pos, glm::vec3 v, float dt)
{
    const std::vector<SOE*>& apsoe = ApsoeLevel(psw);
    if (apsoe.empty()) return v;
    LSG lsg;
    for (int pass = 0; pass < 3; ++pass)
    {
        const float s = glm::length(v);
        if (s < 1e-3f) break;
        const glm::vec3 dir = v / s;
        const glm::vec3 b = pos + dir * (s * dt + c_sRadius);
        if (!PsoeHitTestLineObjects(0, pos, b, apsoe, &lsg)) break;
        glm::vec3 n(lsg.plane0);
        if (glm::length(n) < 1e-3f) n = -dir;
        n = glm::normalize(n);
        if (glm::dot(n, dir) > 0.0f) n = -n;
        // room left before the clearance: the motion along -n limited to it, the rest slides
        const float sHit = lsg.t0 * glm::length(b - pos);
        const float vn = glm::dot(v, n);
        const float vnAllowed = -std::max(0.0f, sHit - c_sRadius) / std::max(dt, 1e-4f);
        if (vn < vnAllowed) v += n * (vnAllowed - vn);
    }
    // already closer than the clearance (below / above): out at up to 600 units / s
    for (const glm::vec3& d : {glm::vec3(0, 0, -1), glm::vec3(0, 0, 1)})
        if (PsoeHitTestLineObjects(0, pos, pos + d * c_sRadius, apsoe, &lsg))
        {
            const float sPush = (1.0f - lsg.t0) * c_sRadius;
            const float vOut = std::min(600.0f, sPush / std::max(dt, 1e-4f));
            if (glm::dot(v, -d) < vOut) v += -d * (vOut - glm::dot(v, -d));
        }
    return v;
}

// A projectile from the gun's / bomb's source: a free copy, else the world's own proxy's instance.
ALO* PaloTakeProjectile(SW* psw, bool fBomb)
{
    for (ALO* p : fBomb ? s_rc.apaloBomb : s_rc.apaloShot)
        if (!p->fInWorld) return p;
    LO* pproxy = PloChild(s_rc.pchopper, fBomb ? c_oidProxyBomb : c_oidProxyGun);
    if (!pproxy) return nullptr;
    LO* plo = PloAcquireProxyObject(psw, pproxy, true);
    return plo && FCidDerivedFrom(plo->Cid(), 0x4F) ? static_cast<ALO*>(plo) : nullptr;
}

// ------------------------------------------------------------ the chase camera
// Behind the heading and above, looking a bit over the chopper; the right stick / mouse orbit it (eased back behind
// once flying again); pulled in where the level is between it and the chopper. The game's camera runs as usual and
// this one is put over it after the world's update (SetCmLookAt), so its zones / policies come back on the exit.
constexpr float c_sCamDist = 380.0f, c_radCamTilt = 0.30f, c_dzCamLook = 40.0f;

// The RC jobs' top-down camera (R3). Its parameters are the ones RC Bombing Run's camera has while the chopper flies
// (world 8, job 2065: the follow policy on zone 50507, measured with SLY2N_CMTRACE): the eye 1044 units from the focus
// (plus the near clip, 100, as CPFOLLOW::Update puts it), tilted 1.279 rad (73 degrees) down, a world-fixed pan, the
// focus 0.2 of the screen's half-height below the centre (dyScreen); the focus leads the chopper 100 units along its
// heading (CPDEFI's sAdjust, radHome + pi) through the camera's adjust spring (SpringCm 5 / -3.5: the follow lag). In
// the job the pan is the zone's (pi); here it is the heading when R3 chose it, then fixed (turned by the right stick /
// mouse), and its stick steers as the job's does (the chopper turns toward the stick's direction on the screen). The
// world's zones are not used (elsewhere they gave other views: Paris's was a high one).
constexpr float c_sTopDist = 1044.0f, c_radTopTilt = 1.27934f, c_dyTopScreen = 0.2f, c_sTopLead = 100.0f;
constexpr float c_rTopSpring = 5.0f, c_rTopDamping = -3.5f;

glm::vec3 DirSphere(float pan, float tilt) { return glm::vec3(std::cos(pan) * std::cos(tilt), std::sin(pan) * std::cos(tilt), std::sin(tilt)); }

// (its eye and focus for this step: s_rc.posTopEye / posTopFocus; fCut: the lead snapped)
void UpdateTopCam(SW* psw, float dt, bool fCut, float dPan)
{
    s_rc.radTopPan -= dPan;
    while (s_rc.radTopPan > 3.14159265f) s_rc.radTopPan -= 6.2831853f;
    while (s_rc.radTopPan < -3.14159265f) s_rc.radTopPan += 6.2831853f;
    const glm::vec3 posBase = PosAloCenter(s_rc.pchopper);
    const glm::vec3 lead(std::cos(s_rc.rad) * c_sTopLead, std::sin(s_rc.rad) * c_sTopLead, 0.0f);
    if (fCut)
    {
        s_rc.dposLead = lead;
        s_rc.vLead = glm::vec3(0.0f);
    }
    else if (dt > 0.0f)
    {
        const glm::vec3 dv = (lead - s_rc.dposLead) * c_rTopSpring + s_rc.vLead * c_rTopDamping;
        s_rc.vLead += dv * dt;
        s_rc.dposLead += s_rc.vLead * dt;
    }
    const glm::vec3 posFocus = posBase + s_rc.dposLead;
    // the eye behind the pan (its direction reversed) and above, pulled in under a ceiling / roof in between
    const glm::vec3 dirEye = DirSphere(s_rc.radTopPan + 3.14159265f, c_radTopTilt);
    float sEye = c_sTopDist + g_cm.sNearClip;
    LSG lsg;
    const std::vector<SOE*>& apsoe = ApsoeLevel(psw);
    if (!apsoe.empty() && PsoeHitTestLineObjects(0, posFocus, posFocus + dirEye * (sEye + 40.0f), apsoe, &lsg))
        sEye = std::max(150.0f, lsg.t0 * (sEye + 40.0f) - 40.0f);
    s_rc.posTopEye = posFocus + dirEye * sEye;
    s_rc.posTopFocus = posFocus;
}

void UpdateChaseCam(SW* psw, float dt)
{
    const JOYPS2& j = JoyFor(2);
    float dPan = 0.0f, dTilt = 0.0f;
    MouseOrbitDelta(&dPan, &dTilt);
    dPan += -j.x2 * 2.5f * dt;
    dTilt += j.y2 * 1.5f * dt;
    // (the top-down camera chosen: the orbit turns its pan, the chase camera eases back behind meanwhile)
    const float dPanTop = s_rc.fTop ? dPan * 0.8f : 0.0f;
    if (s_rc.fTop) dPan = dTilt = 0.0f;
    if (dPan != 0.0f || dTilt != 0.0f) s_rc.tCamInput = g_clock.t;
    s_rc.radCamYaw -= dPan;
    s_rc.radCamTilt = std::clamp(s_rc.radCamTilt + dTilt, -0.25f, 0.9f);
    while (s_rc.radCamYaw > 3.14159265f) s_rc.radCamYaw -= 6.2831853f;
    while (s_rc.radCamYaw < -3.14159265f) s_rc.radCamYaw += 6.2831853f;
    if (g_clock.t - s_rc.tCamInput > 1.0f && glm::length(glm::vec2(s_rc.v)) > 200.0f)
    {
        s_rc.radCamYaw -= s_rc.radCamYaw * std::min(1.0f, dt * 1.5f);
        s_rc.radCamTilt -= s_rc.radCamTilt * std::min(1.0f, dt * 1.5f);
    }
    const glm::vec3 posTarget = PosAloCenter(s_rc.pchopper) + glm::vec3(0.0f, 0.0f, c_dzCamLook);
    const float yaw = s_rc.rad + s_rc.radCamYaw, tilt = c_radCamTilt + s_rc.radCamTilt;
    const glm::vec3 dirBack(-std::cos(yaw) * std::cos(tilt), -std::sin(yaw) * std::cos(tilt), std::sin(tilt));
    glm::vec3 posWant = posTarget + dirBack * c_sCamDist;
    LSG lsg;
    const std::vector<SOE*>& apsoe = ApsoeLevel(psw);
    if (!apsoe.empty() && PsoeHitTestLineObjects(0, posTarget, posWant + dirBack * 40.0f, apsoe, &lsg))
        posWant = posTarget + dirBack * std::max(60.0f, lsg.t0 * (c_sCamDist + 40.0f) - 40.0f);
    const bool fCut = s_rc.fCamCut;
    if (fCut) s_rc.posCam = posWant;
    else s_rc.posCam += (posWant - s_rc.posCam) * std::min(1.0f, dt * 7.0f);
    s_rc.fCamCut = false;
    // R3 chose the top-down camera (or back): the two blended over half a second
    const float uGoal = s_rc.fTop ? 1.0f : 0.0f;
    if (fCut) s_rc.uTop = uGoal;
    else if (s_rc.uTop != uGoal) s_rc.uTop = s_rc.uTop < uGoal ? std::min(uGoal, s_rc.uTop + dt * 2.0f) : std::max(uGoal, s_rc.uTop - dt * 2.0f);
    if (s_rc.uTop <= 0.0f)
    {
        SetCmLookAt(&g_cm, s_rc.posCam, posTarget, 0.0f, 0.0f);
        return;
    }
    UpdateTopCam(psw, dt, fCut, dPanTop);
    const float u = s_rc.uTop * s_rc.uTop * (3.0f - 2.0f * s_rc.uTop);
    const float dradTilt = -std::atan(c_dyTopScreen * g_cm.yScreenRange) * u;
    SetCmLookAt(&g_cm, s_rc.posCam + (s_rc.posTopEye - s_rc.posCam) * u, posTarget + (s_rc.posTopFocus - posTarget) * u, 0.0f, dradTilt);
}

// The gun's aim: the nearest guard (cid 0x38..0x43, in the world) within 3500 ahead, 30 degrees either side of the
// heading and 50 degrees below to 20 above the level; else straight along the heading, tipped down with the body.
constexpr float c_sAim = 3500.0f;
glm::vec3 DirAim(SW* psw, const glm::vec3& posMuzzle, const glm::vec3& fwd)
{
    LO* ploBest = nullptr;
    float gBest = 1e30f;
    glm::vec3 dirBest = fwd;
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            if (!plo->fInWorld || plo->fRemoved || !FCidDerivedFrom(plo->Cid(), 0x30) || !PguardFromLo(plo)) continue;
            const glm::vec3 d = PosAloCenter(static_cast<ALO*>(plo)) - posMuzzle;
            const float s = glm::length(d);
            const float sLevel = glm::length(glm::vec2(d));
            if (s < 50.0f || s > c_sAim || sLevel < 1.0f) continue;
            const float c = glm::dot(glm::vec2(d) / sLevel, glm::vec2(fwd));
            if (c < 0.866f) continue;   // (cos 30 degrees)
            const float elev = std::atan2(d.z, sLevel);
            if (elev < -0.87f || elev > 0.35f) continue;
            const float g = s * (2.0f - c);
            if (g < gBest) { gBest = g; ploBest = plo; dirBest = d / s; }
        }
    if (ploBest)
    {
        // led by its velocity over the flight
        ALO* palo = static_cast<ALO*>(ploBest);
        const glm::vec3 posT = PosAloCenter(palo), vT = VelAloPos(palo);
        glm::vec3 posAim = posT;
        for (int i = 0; i < 2; ++i) posAim = posT + vT * (glm::length(posAim - posMuzzle) / c_sShot);
        return glm::normalize(posAim - posMuzzle);
    }
    return dirBest;
}

// Thrown as the script does it: placed at the proxy's point (the gun's muzzle under the nose, the bomb bay), its pool
// reset (slot 26), out of its parent, the chopper its thrower (its blast spares the chopper), the velocity set.
void Throw(SW* psw, bool fBomb)
{
    ALO* po = PaloTakeProjectile(psw, fBomb);
    if (!po)
    {
        if (FPrint()) std::printf("pc rc: no %s free\n", fBomb ? "bomb" : "shot");
        return;
    }
    ALO* pc = s_rc.pchopper;
    LO* pproxy = PloChild(pc, fBomb ? c_oidProxyBomb : c_oidProxyGun);
    glm::vec3 pos = PosAloCenter(pc) + glm::vec3(0.0f, 0.0f, -60.0f);
    if (pproxy) pos = PosLoWorld(pproxy);
    glm::mat3 mat = MatHeading(s_rc.rad, s_rc.radPitch, 0.0f);
    glm::vec3 fwd = mat[0];
    if (!fBomb)
    {
        fwd = DirAim(psw, pos, glm::normalize(glm::vec3(std::cos(s_rc.rad), std::sin(s_rc.rad), 0.0f)));
        const glm::vec3 left = glm::length(glm::vec2(fwd)) > 1e-3f ? glm::normalize(glm::vec3(-fwd.y, fwd.x, 0.0f)) : glm::vec3(0, 1, 0);
        mat = glm::mat3(fwd, left, glm::cross(fwd, left));
    }
    if (void* pv = PvNativeVt(po, 26)) reinterpret_cast<void (*)(LO*, LO*)>(pv)(po, nullptr);
    if (po->fRemoved || !po->fInWorld) AddLo(psw, po);
    SetAloCnstrLin(po, 0);
    SetAloCnstrAng(po, 0);
    SetAloParent(po, nullptr, nullptr, 3);
    SetAloPos(po, pos);
    SetAloRotation(po, mat);
    const glm::vec3 vChopper = s_rc.v;
    glm::vec3 v = fBomb ? glm::vec3(vChopper.x, vChopper.y, std::min(vChopper.z, 0.0f) - 100.0f) : fwd * c_sShot;
    SetAloVelocityTo(po, v);
    SetAloAngVelocityTo(po, glm::vec3(0.0f));
    SetProjectileLauncher(po, pc);
    ExpireProjectileIn(po, fBomb ? 6.0f : 2.0f);
    if (FPrint())
        std::printf("pc rc: %s %d from %.0f %.0f %.0f v %.0f %.0f %.0f\n", fBomb ? "bomb" : "shot", po->oid, pos.x, pos.y, pos.z, v.x, v.y, v.z);
}

void PlaceChopper(ALO* pc, const glm::vec3& pos)
{
    SetAloPos(pc, pos);
    SetAloRotation(pc, MatHeading(s_rc.rad, 0.0f, 0.0f));
    SetAloVelocityTo(pc, glm::vec3(0.0f));
    SetAloAngVelocityTo(pc, glm::vec3(0.0f));
}

// The copies in the running world (made once per world from the kept world 8's objects): the chopper (its script
// detached: world 8 and the RC jobs' worlds would run it), its gun's shots and bombs (out of the world until thrown).
constexpr int c_cShot = 8, c_cBomb = 4;
constexpr int c_nShotDepth = -10, c_nBombDepth = -40;
ALO* s_pchopperCopy = nullptr;
uint32_t s_serialCopies = 0;
bool s_fFailed = false;   // the kept world could not be loaded: the world's own chopper only

void DetachScripts(SW* psw, ALO* proot)
{
    for (const auto& up : psw->aploAll)
        if (up.get() == proot || FLoInParentChain(up.get(), proot)) up->pframe.reset();
}

bool FEnsureCopies(SW* psw)
{
    if (s_serialCopies == psw->serial && s_pchopperCopy) return true;
    const PCKWS k = PckwsKeptWorld(c_iwrecChopper);
    if (k == PCKWS_Failed) { s_fFailed = true; return false; }
    if (k != PCKWS_Ready) { RequestPcKeptWorld(c_iwrecChopper); return false; }
    LO* ptmpl = PloKeptFind(c_iwrecChopper, kRcChopperOid, 0x32);
    LO* ptmplShot = PloKeptFind(c_iwrecChopper, c_oidShot, 0x4F);
    LO* ptmplBomb = PloKeptFind(c_iwrecChopper, c_oidBomb, 0x4F);
    if (!ptmpl) { s_fFailed = true; return false; }
    LO* plo = PloPcCloneIntoWorld(psw, ptmpl, false);
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x32)) { s_fFailed = true; return false; }
    s_pchopperCopy = static_cast<ALO*>(plo);
    DetachScripts(psw, s_pchopperCopy);
    s_rc.apaloShot.clear();
    s_rc.apaloBomb.clear();
    for (int i = 0; i < c_cShot + c_cBomb; ++i)
        if (LO* ptmplP = i < c_cShot ? ptmplShot : ptmplBomb)
            if (LO* pp = PloPcCloneIntoWorld(psw, ptmplP, false); pp && FCidDerivedFrom(pp->Cid(), 0x4F))
            {
                // the hit's depth (+0x1F0, the contact's state: the hit points it takes; authored -5 / -10, the Ep6
                // planes' and the Ep2 jeep's) deeper here, for the guards of any world: a bomb knocks out a 40-point
                // guard (Paris' 11539), four shots do
                pp->SetField(0, 0x1F0, static_cast<uint32_t>(i < c_cShot ? c_nShotDepth : c_nBombDepth));
                (i < c_cShot ? s_rc.apaloShot : s_rc.apaloBomb).push_back(static_cast<ALO*>(pp));
            }
    s_serialCopies = psw->serial;
    if (FPrint()) std::printf("pc rc: copies made: chopper %d, %zu shots, %zu bombs\n", plo->oid, s_rc.apaloShot.size(), s_rc.apaloBomb.size());
    return true;
}

bool FBegin(SW* psw)
{
    LO* plo = PloPoCurrent(psw);
    if (!plo || !FIsLoDerivedFrom(plo, 0x30) || FIsLoDerivedFrom(plo, 0x46) || plo->fRemoved) return false;
    ALO* pc = nullptr;
    s_rc.fOwnChopper = false;
    if (FEnsureCopies(psw)) pc = s_pchopperCopy;
    else if (s_fFailed)
    {
        // (no copy: the world's own chopper where it has one, its script detached meanwhile)
        pc = static_cast<ALO*>(PloFindIn(psw, kRcChopperOid, 0x32));
        if (!pc) return false;
        if (pc->fInWorld && !pc->fRemoved) return false;   // (its job flies it now)
        s_rc.fOwnChopper = true;
        s_rc.apaloShot.clear();
        s_rc.apaloBomb.clear();
    }
    if (!pc) return false;
    ALO* palo = static_cast<ALO*>(plo);
    const glm::mat4 m = MatWorldOfAlo(palo);
    STEP* pstep = PstepFromLo(plo);
    s_rc.rad = pstep ? RadStepHeading(pstep) : std::atan2(m[0].y, m[0].x);
    const glm::vec3 fwd(std::cos(s_rc.rad), std::sin(s_rc.rad), 0.0f);
    // (above the player and a bit ahead, not into a ceiling or a wall)
    glm::vec3 pos = PosAloCenter(palo);
    pos += VClipLevel(psw, pos, glm::vec3(0.0f, 0.0f, 200.0f), 1.0f);
    pos += VClipLevel(psw, pos, fwd * 150.0f, 1.0f);
    if (pc->fRemoved || !pc->fInWorld) AddLo(psw, pc);
    if (SOST* ps = Psost(pc))
    {
        s_rc.dvGravitySaved = ps->dvGravity;
        ps->dvGravity = glm::vec3(0.0f);
    }
    s_rc.pframeSaved = pc->pframe;
    pc->pframe.reset();
    PlaceChopper(pc, pos);
    s_rc.pchopper = pc;
    s_rc.ploPlayer = plo;
    s_rc.serial = psw->serial;
    s_rc.radPitch = s_rc.radRoll = 0.0f;
    s_rc.v = glm::vec3(0.0f);
    s_rc.tShotNext = s_rc.tBombNext = 0.0f;
    s_rc.fCamCut = true;
    s_rc.radCamYaw = s_rc.radCamTilt = 0.0f;
    s_rc.radTopPan = s_rc.rad;   // (the top-down choice is kept from the last flight)
    EnablePo(psw, pc, 1);
    CutCm(&g_cm);
    s_rc.fActive = true;
    // the rotor's hum: the copy's ambient object (6390) is not registered in a world it was not loaded with
    {
        CSnd* psnd = PsndNewPlain(c_sfxRotor);
        psnd->SetSource(pc);
        psnd->SetFalloff(c_sRotorFull, c_sRotorSilent);
        psnd->SetVolume(c_gRotor);
        s_rc.sndRotor = CSndRef(psnd);
    }
    std::printf("pc rc: flying chopper %d%s from %.0f %.0f %.0f (player %d)\n", pc->oid, s_rc.fOwnChopper ? " (the world's own)" : "", pos.x, pos.y,
                pos.z, plo->oid);
    if (FPrint()) PrintBreakablesNear(pos, 8000.0f);
    return true;
}

// fWorldGone: its objects are freed (nothing touched); fGivePad: the pad back to the player (not when something else
// took it: a cutscene, the safe house)
void End(SW* psw, bool fWorldGone, bool fGivePad = true)
{
    if (!s_rc.fActive) return;
    s_rc.fActive = false;
    if (s_rc.sndRotor) s_rc.sndRotor->Stop(0.2f);
    s_rc.sndRotor = CSndRef();
    if (!fWorldGone && psw && psw->serial == s_rc.serial)
    {
        if (fGivePad && s_rc.ploPlayer && !s_rc.ploPlayer->fRemoved) EnablePo(psw, s_rc.ploPlayer, 1);
        else if (PloPoCurrent(psw) == s_rc.pchopper) EnablePo(psw, s_rc.pchopper, 0);
        if (ALO* pc = s_rc.pchopper)
        {
            if (SOST* ps = Psost(pc)) ps->dvGravity = s_rc.dvGravitySaved;
            pc->pframe = s_rc.pframeSaved;
            SetAloVelocityTo(pc, glm::vec3(0.0f));
            if (!pc->fRemoved) RemoveLo(psw, pc);
        }
        CutCm(&g_cm);
    }
    s_rc.pchopper = nullptr;
    s_rc.ploPlayer = nullptr;
    s_rc.pframeSaved.reset();
    std::printf("pc rc: back to the player\n");
}

void Step(SW* psw)
{
    ALO* pc = s_rc.pchopper;
    const JOYPS2& j = JoyFor(2);
    const float dt = g_clock.dt > 0.0f ? g_clock.dt : 0.0f;
    if (dt <= 0.0f) return;
    auto held = [&](uint32_t grfLibpad) {
        using sly2actions::Action;
        if (sly2actions::Active()) {
            if (grfLibpad == BTN_R2) return sly2actions::Held(Action::RcUp);
            if (grfLibpad == BTN_L2) return sly2actions::Held(Action::RcDown);
            if (grfLibpad == BTN_Square) return sly2actions::Held(Action::RcFire);
            if (grfLibpad == BTN_Cross) return sly2actions::Held(Action::RcBomb);
        }
        return (j.grfbtn & GrfbtnPs2FromLibpad(grfLibpad)) != 0;
    };
    // the left stick: up / down fly forward / back along the heading, left / right turn it (the chase camera turns
    // with it); R2 / L2 climb / sink
    float xStick = std::fabs(j.x) > 0.12f ? j.x : 0.0f, yStick = std::fabs(j.y) > 0.12f ? j.y : 0.0f;
    // R3 (the key bound to it): the chase camera / the RC jobs' top-down camera
    if (sly2actions::Active() ? sly2actions::Pressed(sly2actions::Action::RcCamera) : (j.grfbtnPressed & GrfbtnPs2FromLibpad(BTN_R3)) != 0)
    {
        s_rc.fTop = !s_rc.fTop;
        if (s_rc.fTop && s_rc.uTop <= 0.0f) s_rc.radTopPan = s_rc.rad + s_rc.radCamYaw;   // (up the screen: where the chase camera looked)
        std::printf("pc rc: %s camera\n", s_rc.fTop ? "top-down (the RC jobs')" : "chase");
    }
    float dRad = -xStick * c_wTurnMax * dt;
    if (s_rc.fTop)
    {
        // (the top-down camera: as the RC jobs steer, the chopper turns toward the stick's direction on the screen and
        // flies forward with its deflection)
        const float sStick = std::min(1.0f, std::sqrt(xStick * xStick + yStick * yStick));
        dRad = 0.0f;
        if (sStick > 0.0f)
        {
            const float radWant = s_rc.radTopPan + std::atan2(-xStick, yStick);
            float d = radWant - s_rc.rad;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            dRad = std::clamp(d, -c_wTurnMax * 1.5f * dt, c_wTurnMax * 1.5f * dt);
            xStick = -std::clamp(d / 0.6f, -1.0f, 1.0f) * sStick;   // (the bank)
            yStick = sStick * std::max(0.0f, std::cos(d));
        }
        else xStick = yStick = 0.0f;
    }
    s_rc.rad += dRad;
    while (s_rc.rad > 3.14159265f) s_rc.rad -= 6.2831853f;
    while (s_rc.rad < -3.14159265f) s_rc.rad += 6.2831853f;
    const glm::vec3 fwdH(std::cos(s_rc.rad), std::sin(s_rc.rad), 0.0f);
    float dz = 0.0f;
    if (held(BTN_R2)) dz += 1.0f;
    if (held(BTN_L2)) dz -= 1.0f;
    const glm::vec3 vGoal = fwdH * (yStick * (yStick > 0.0f ? c_sMax : 0.6f * c_sMax)) + glm::vec3(0.0f, 0.0f, dz * c_sClimb);
    glm::vec3 v = s_rc.v;
    // (the level velocity turns with the heading: no drifting out of a turn)
    {
        const float sLevel = glm::length(glm::vec2(v));
        const float sAlong = glm::dot(glm::vec2(v), glm::vec2(fwdH));
        if (sLevel > 1.0f) { const glm::vec2 vl = glm::vec2(fwdH) * (sAlong >= 0.0f ? sLevel : -sLevel); v.x += (vl.x - v.x) * std::min(1.0f, dt * 6.0f); v.y += (vl.y - v.y) * std::min(1.0f, dt * 6.0f); }
    }
    v += (vGoal - v) * std::min(1.0f, dt * c_rAccel);
    v = VClipLevel(psw, PosAloCenter(pc), v, dt);
    s_rc.v = v;
    SetAloVelocityTo(pc, v);
    SetAloAngVelocityTo(pc, glm::vec3(0.0f));
    // the body leans into the motion: the nose down with the forward speed, banked into the turn
    const glm::vec3 fwd(std::cos(s_rc.rad), std::sin(s_rc.rad), 0.0f);
    const float sFwd = glm::dot(glm::vec3(v.x, v.y, 0.0f), fwd);
    const float pitchGoal = 0.35f * std::clamp(sFwd / c_sMax, -1.0f, 1.0f);
    const float rollGoal = -0.45f * xStick * (0.3f + 0.7f * std::clamp(std::fabs(sFwd) / c_sMax, 0.0f, 1.0f));
    s_rc.radPitch += (pitchGoal - s_rc.radPitch) * std::min(1.0f, dt * 4.0f);
    s_rc.radRoll += (rollGoal - s_rc.radRoll) * std::min(1.0f, dt * 4.0f);
    SetAloRotation(pc, MatHeading(s_rc.rad, s_rc.radPitch, s_rc.radRoll));
    // the player standing where it was is not hurt meanwhile (nobody controls it)
    if (ACTOR* pa = PactorFromLo(s_rc.ploPlayer))
        if (int* pn = DmgCounterOf(pa)) *pn = std::max(*pn, DmgCapacityOf(pa));
    // Square: the gun (held: repeats); Cross: a bomb
    const float t = g_clock.t;
    if (held(BTN_Square) && t >= s_rc.tShotNext)
    {
        s_rc.tShotNext = t + c_dtShot;
        Throw(psw, false);
    }
    if (held(BTN_Cross) && t >= s_rc.tBombNext)
    {
        s_rc.tBombNext = t + c_dtBomb;
        Throw(psw, true);
    }
    UpdateChaseCam(psw, dt);
    if (FPrint() && g_iframeMain % 30 == 0)
    {
        const glm::vec3 p = PosAloCenter(pc);
        std::printf("pc rc: t %.2f pos %.0f %.0f %.0f v %.0f %.0f %.0f heading %.0f stick %.2f %.2f btn 0x%X cam %.0f %.0f %.0f\n", t, p.x,
                    p.y, p.z, v.x, v.y, v.z, glm::degrees(s_rc.rad), j.x, j.y, j.grfbtn, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z);
    }
}
}  // namespace

bool FPcRcActive() { return s_rc.fActive || s_rc.fWant; }
bool FPcRcPending() { return s_rc.fWant && !s_rc.fActive; }
const char* PchzPcRcStatus()
{
    if (s_rc.fActive) return "Flying";
    if (s_rc.fWant && s_fFailed) return "Not available";
    if (s_rc.fWant) return "Loading";
    return "Off";
}
bool FPcRcObject(const LO* plo)
{
    if (!s_rc.fActive || !plo) return false;
    return plo == s_rc.pchopper || FLoInParentChain(plo, s_rc.pchopper);
}

void RequestPcRc(SW* psw, bool fOn)
{
    s_rc.fWant = fOn;
    if (!fOn) End(psw, false);
}

void UpdatePcRc(SW* psw, bool fDebugKeys)
{
    // (tests: SLY2N_PCRC=on[,off]: on / off at those frames)
    static int s_iframeOn = -2, s_iframeOff = -1;
    if (s_iframeOn == -2)
    {
        s_iframeOn = -1;
        if (const char* sz = std::getenv("SLY2N_PCRC")) std::sscanf(sz, "%d,%d", &s_iframeOn, &s_iframeOff);
    }
    if (fDebugKeys && s_iframeOn >= 0 && g_iframeMain == s_iframeOn) s_rc.fWant = true;
    if (fDebugKeys && s_iframeOff >= 0 && g_iframeMain == s_iframeOff) RequestPcRc(psw, false);
    if (s_rc.fActive && (!psw || psw->serial != s_rc.serial))
    {
        End(psw, true);   // (its world is gone: the new world starts with the player)
        s_rc.fWant = false;
    }
    if (!fDebugKeys)
    {
        s_rc.fWant = false;
        End(psw, false);
        return;
    }
    if (!psw) return;
    if (s_rc.fActive)
    {
        // the player switched by other means (the safe house, a cutscene's own switch): the flight ends
        if (PloPoCurrent(psw) != s_rc.pchopper || s_rc.pchopper->fRemoved)
        {
            s_rc.fWant = false;
            End(psw, false, false);
            return;
        }
        Step(psw);
        return;
    }
    if (s_rc.fWant && !FBegin(psw) && FPrint() && g_iframeMain % 60 == 0) std::printf("pc rc: no chopper / player yet\n");
}

namespace
{
// the world hooks (pcloader.h): each step after the world's update
struct INITPCRC
{
    INITPCRC()
    {
        PCWORLDHOOKS h;
        h.pfnStep = UpdatePcRc;
        RegisterPcWorldHooks(h);
    }
} s_initpcrc;
}  // namespace
