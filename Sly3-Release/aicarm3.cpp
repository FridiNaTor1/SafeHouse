// See aicarm3.h. Ported from the Sly 3 decomp (orig3 addresses in the comments).

#include "copsmap3.h"
#include "game/trigger3.h"
#include "aicarm3.h"

#include "aijobs.h"
#include "activate.h"
#include "alo.h"
#include "so.h"
#include "asega.h"
#include "clock.h"
#include "guard.h"
#include "lo.h"
#include "po.h"
#include "smrun.h"
#include "activate.h"
#include "projectile.h"
#include "splitscreen3.h"
#include "sound.h"
#include "screenrt.h"
#include "hud.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "vtnative.h"
#include "xf.h"
#include "cm.h"
#include "disc.h"
#include "detect.h"
#include "damage.h"
#include "eval.h"
#include "actor.h"
#include "pe.h"
#include "game/sly3.h"
#include "game/task3.h"
#include "eval.h"
#include "frame.h"
#include "sidebag.h"
#include "proc.h"
#include "cmworld.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

int NRandInRange(int nLow, int nHigh);                       // util.h (orig 0x11D3F8; orig3 0x11E8F0)
void BindLoPartsTable3(ALO* palo, int c, uint32_t vaTable);   // loaders.cpp
void SetBindPartsFromTemplate3(bool f);                       // loaders.cpp
glm::vec3 VelBallisticLaunch(float gPitch, const glm::vec3& posStart, const glm::vec3& pos, const glm::vec3& accel);   // launcher.cpp (orig3 0x212480)
glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);   // mat.h (orig3 0x11CBA0)

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY3_CARMPRINT") != nullptr; return s_f; }

// the guard's +0x130 (the player index) by object
std::unordered_map<const LO*, int> s_mpiplayer;
struct CARMPOWER { int kind = -1; int meter = 0; };
std::unordered_map<const LO*, CARMPOWER> s_mppower;   // (orig3 the guard's +0x18B0 and SO +0x174)

// orig3: her +0x1884 target box (0x162108: +0x8 the state -1, +0x18 3000 the range to lock on, +0x1C 4000 the range to
// keep it, +0x20 0.5 s to lock, +0x24 1 s out of sight to lose it; +0x0 the target, +0x4 her (the camera's owner), +0x14
// the box's screen object (option 1414, Sly 3 EOPID 1542 +0x1898)): the charged shot's lock-on (power-up 0x33)
struct CARMBOX
{
    LO* ptarget = nullptr;   // +0x0
    int state = -1;          // +0x8
    float tState = 0.0f;     // +0xC
    float tSeen = 0.0f;      // +0x10
};
std::unordered_map<const LO*, CARMBOX> s_mpbox;

// the pad's buttons as the game reads them (the PADCTL's grfbtn: the decomp's masks, 0x1A7A80)
constexpr uint32_t BTN3_R1 = 0x08, BTN3_L1 = 0x04, BTN3_Triangle = 0x10, BTN3_Circle = 0x20, BTN3_Cross = 0x40,
                   BTN3_Square = 0x80, BTN3_L3 = 0x200;

// orig3 D_00317530: the feet's forward in the heading's frame (read from the ELF; the moves' velocities are -speed x it)
glm::vec3 VecForward()
{
    static const glm::vec3 s_v = [] {
        glm::vec3 v;
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t u = g_disc.U32Elf(0x317530 + 4u * uint32_t(i));
            std::memcpy(&v[i], &u, 4);
        }
        return v;
    }();
    return s_v;
}

LO* PloPartAt(ALO* palo, uint32_t ib3)
{
    for (const auto& [ib, plo] : palo->apart)
        if (ib == (FN3_TAG | ib3) || ib == ib3) return plo;
    return nullptr;
}

// orig3 0x1611B0: the run's speed: with the robber (the global player D_00370AC0, Sly) in the world (his +0x28 bit 56:
// Sly 3's "in the world" bit, scene.h's inherited, not Sly 2's removed bit) 800 + 0.075 x her distance from him, kept
// in 850..1100 (she catches up from afar); else 850. (Round 9: natively it was 850 always, bit 56 read as Sly 2's.)
// PC teams: the nearest robber.
float SRun(const LO* ploHer)
{
    if (!ploHer || !g_psw) return 850.0f;
    LO* ploRobber = nullptr;
    float d = 1e30f;
    for (int i = 0; i < split3::CView(); i += 2)
        if (LO* plo = split3::PloPlayer(g_psw, i); plo && plo->fInWorld && FCidDerivedFrom(plo->Cid(), S3CID(0x34)))
            if (const float dk = glm::length(PosLoWorld(plo) - PosLoWorld(ploHer)); dk < d) d = dk, ploRobber = plo;
    if (!ploRobber)
    {
        ploRobber = PloPoCurrent(g_psw);
        if (!ploRobber || !ploRobber->fInWorld || ploRobber == ploHer) return 850.0f;
        d = glm::length(PosLoWorld(ploRobber) - PosLoWorld(ploHer));
    }
    return std::clamp(800.0f + 0.075f * d, 850.0f, 1100.0f);
}

// the stick's world heading in the player's view (orig3 0x168108 on the player's camera 0x1F4A80: po.cpp's
// UpdatePadctl already turned it by the seat's view camera)
float RadStick(const PADCTL* ppc) { return ppc ? ppc->radJoy : 0.0f; }
float UStick(const PADCTL* ppc) { return ppc ? ppc->uJoy : 0.0f; }
}  // namespace

namespace
{
LO* PscreenBox(LO* plo)
{
    LO* p = plo && g_psw ? PloFindSwObjectByOid(g_psw, OID(plo->U32Field(0, 0x1898, 0xFFFFFFFFu))) : nullptr;
    return p && FCidDerivedFrom(p->Cid(), S3CID(0x7E)) ? p : nullptr;   // (a SCREENOBJ: Sly 3 0x9E)
}

// her view's picture in the 640 x 480 canvas (the camera's rectangle +0x150 / +0x160): in the two-player screen mode her
// band's view box hole as the HUD draws it (hud.h abox2p, 640 x 448 units), else the whole canvas
void RectBox(const LO* plo, float* px0, float* py0, float* px1, float* py1)
{
    const int i = IPlayerCarm3(plo);
    *px0 = 0.0f, *px1 = 640.0f, *py0 = 0.0f, *py1 = 480.0f;
    if (!g_psw || g_psw->nScreenMode3 != 2 || (i != 0 && i != 1)) return;
    const CHud::BOX2P& b = g_hud.abox2p[i];
    if (b.frame < 0 || b.hole.z <= b.hole.x) { *py0 = 240.0f * float(i), *py1 = 240.0f * float(i + 1); return; }
    constexpr float ky = 480.0f / 448.0f;
    *px0 = b.hole.x, *px1 = b.hole.z, *py0 = b.hole.y * ky, *py1 = b.hole.w * ky;
}

const CM& CmBox(const LO* plo) { return split3::CmView(std::max(0, IPlayerCarm3(plo))); }

// orig3: 0x162698: the box's state; the screen object's parts: 1 aiming (slot 53 on 0x3BE, placed in her camera's
// rectangle (0x2643D0), part 0x3D8 started at fraction 0), 2 locked (0x3D5 / 0x3D6); leaving 1 / 2 resets them; -1 / 0:
// hidden (slot 54)
void SetBoxState(LO* plo, CARMBOX& b, int state)
{
    if (state == b.state) return;
    LO* pscr = PscreenBox(plo);
    if (pscr && b.state == 1) ResetScreenObjPart(pscr, 0x3D8);
    if (pscr && b.state == 2) { ResetScreenObjPart(pscr, 0x3D5); ResetScreenObjPart(pscr, 0x3D6); }
    if (FPrint()) std::printf("carm3: %d target box %d -> %d (target %d, screen %d)\n", plo->oid, b.state, state, b.ptarget ? b.ptarget->oid : -1, pscr ? pscr->oid : -1);
    b.state = state;
    b.tState = g_clock.t;
    if (state == 1)
    {
        b.tSeen = g_clock.t;
        if (pscr)
        {
            // (PC: as the aim's HUD (SetAim): drawn above the two-player frame 3325, which hid it under its view box)
            if (g_psw && g_psw->nScreenMode3 == 2)
                if (LO* pframe = PloFindSwObjectByOid(g_psw, 3325); pframe && NScreenObjPrio(pscr) <= NScreenObjPrio(pframe))
                    SetScreenObjPrio(pscr, NScreenObjPrio(pframe) + 1);
            ShowScreenObj(pscr, 0x3BE);
            SelectScreenObjPart(pscr, 0x3D8);   // (0x262450: the group having the part plays it)
            SetScreenObjFraction(pscr, 0x3D8, 0.0f);
        }
    }
    else if (state == 2)
    {
        if (pscr) { SelectScreenObjPart(pscr, 0x3D5); SelectScreenObjPart(pscr, 0x3D6); }
    }
    else if (pscr) HideScreenObj(pscr);
}

// orig3: 0x162060: the target is in the world (bit 56), not knocked out (a PO's vt slot 81 2; the class 0x66: never)
bool FBoxTargetValid(const CARMBOX& b)
{
    LO* pt = b.ptarget;
    if (!pt || !pt->fInWorld) return false;
    static const auto s_pfnStatus = reinterpret_cast<int (*)(LO*)>(PvNativeFn(0x140F70));   // (vt slot 81)
    if (FCidDerivedFrom(pt->Cid(), S3CID(0x31))) return !(s_pfnStatus && s_pfnStatus(pt) == 2);
    return true;
}

// orig3: 0x161DF8: the target seen from her camera: its sphere of radius r in the view (0x167F60), within gMax of the
// camera, and of four points around it (r toward the camera, r up / down, r to the side) one at least not hidden from
// the camera by the world's solids (0x25CF18 / 0x165000)
bool FBoxTargetSeen(const LO* plo, const CARMBOX& b, float r, float gMax)
{
    const CM& cm = CmBox(plo);
    const glm::vec3 pos = PosLoWorld(b.ptarget);
    const glm::vec3 q = ConvertCmWorldToScreen(&cm, pos);
    if (q.z <= -r) return false;
    const float m = q.z > 1.0f ? r / q.z : r;   // (the sphere's margin on the screen, roughly)
    if (std::fabs(q.x) > 1.0f + m / cm.xScreenRange || std::fabs(q.y) > 1.0f + m / cm.yScreenRange) return false;
    if (gMax < glm::length(pos - cm.pos)) return false;
    std::vector<SOE*> apsoe;
    if (g_psw)
        for (LO* p : g_psw->mpcidaplo[S3CID(0x31)])
            if (p && p->fInWorld && p != b.ptarget && p != plo)
                for (SOE& soe : static_cast<ALO*>(p)->asoe)
                    if (!soe.f48) apsoe.push_back(&soe);
    const glm::vec3 up = glm::vec3(cm.mat[2]);
    glm::vec3 dir = cm.pos - pos;
    dir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 side = glm::cross(up, dir);
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 c = pos + dir * r;
    const glm::vec3 apos[4] = {c, c + up * r, c - up * r, c + side * r};
    for (const glm::vec3& p : apos)
    {
        LSG lsg;
        if (!PsoeHitTestLineObjects(0, cm.pos, p, apsoe, &lsg)) return true;
    }
    return false;
}

// orig3: 0x162148: the box's update (from the power-up's 0x15FE68 while the charged shot is held): 0 -> 1 when the target
// is valid and seen within 3000 (radius 5); 1 -> 2 after 0.5 s, -> 0 when not seen for 1 s or invalid; 2 -> 0 likewise;
// while 1 / 2 the target seen within 4000 (radius 50) keeps it
void UpdateBox(LO* plo, CARMBOX& b)
{
    int state;
    do
    {
        state = b.state;
        switch (state)
        {
        case 0:
            if (FBoxTargetValid(b) && FBoxTargetSeen(plo, b, 5.0f, 3000.0f)) SetBoxState(plo, b, 1);
            break;
        case 1:
            if (!FBoxTargetValid(b)) SetBoxState(plo, b, 0);
            else if (g_clock.t - b.tSeen > 1.0f) SetBoxState(plo, b, 0);
            else if (g_clock.t - b.tState > 0.5f) SetBoxState(plo, b, 2);
            break;
        case 2:
            if (!FBoxTargetValid(b) || g_clock.t - b.tSeen > 1.0f) SetBoxState(plo, b, 0);
            break;
        default: break;
        }
    } while (b.state != state);
    if ((b.state == 1 || b.state == 2) && FBoxTargetSeen(plo, b, 50.0f, 4000.0f)) b.tSeen = g_clock.t;
}

// orig3: 0x162370 (her draw, slot 21 0x161CD0 -> 0x162678, in the box's states 1 / 2): the box around the target on the
// screen: the target's centre and the 8 corners of its box ({+-50, +-50, 100 / -50} in its frame) projected through her
// camera; the scale by the corners' spread x 0.015 in [0.25, 1.25]; aiming (1) it closes in from the view's centre at
// scale 3.5 over the 0.5 s; the point held inside the view's rectangle (0x262848: the root scaled and moved)
void PlaceBox(LO* plo, const CARMBOX& b)
{
    LO* pscr = PscreenBox(plo);
    if (!pscr || !b.ptarget || b.state < 1 || b.state > 2) return;
    const CM& cm = CmBox(plo);
    float x0, y0, x1, y1;
    RectBox(plo, &x0, &y0, &x1, &y1);
    auto toRect = [&](const glm::vec3& w) {
        const glm::vec3 q = ConvertCmWorldToScreen(&cm, w);
        return glm::vec2(x0 + (q.x * 0.5f + 0.5f) * (x1 - x0), y0 + (0.5f - q.y * 0.5f) * (y1 - y0));
    };
    const glm::mat4 m = sly3::MatLoWorld3(b.ptarget);
    glm::vec2 pos = toRect(glm::vec3(m[3]));
    glm::vec2 vMin(FLT_MAX), vMax(-FLT_MAX);
    static const glm::vec3 s_acorner[8] = {{50, 50, 100}, {50, -50, 100}, {-50, 50, 100}, {-50, -50, 100},
                                          {50, 50, -50}, {50, -50, -50}, {-50, 50, -50}, {-50, -50, -50}};
    for (const glm::vec3& c : s_acorner)
    {
        const glm::vec2 v = toRect(glm::vec3(m * glm::vec4(c, 1.0f)));
        vMin = glm::min(vMin, v);
        vMax = glm::max(vMax, v);
    }
    float gScale = std::clamp(glm::length(vMin - vMax) * 0.015f, 0.25f, 1.25f);
    if (b.state == 1)
    {
        const glm::vec2 posCenter = toRect(cm.pos + glm::vec3(cm.mat[0]) * 1000.0f);   // (0x1622C0 of the origin: the view's centre)
        const float t = std::clamp((g_clock.t - b.tState) / 0.5f, 0.0f, 1.0f);
        pos = pos * t + posCenter * (1.0f - t);
        gScale = (1.0f - t) * 3.5f + t * gScale;
    }
    pos.x = float(std::clamp(int(pos.x), int(x0), int(x1)));
    pos.y = float(std::clamp(int(pos.y), int(y0), int(y1)));
    SetScreenObjRootXY(pscr, true, gScale, pos.x, pos.y);
    if (static const bool s_f = std::getenv("SLY3_CARMBOXPRINT") != nullptr; s_f)
        std::printf("carm3: box %d state %d at %.1f %.1f scale %.2f (rect %.0f %.0f %.0f %.0f) screen state %d prio %d\n", pscr->oid, b.state, pos.x, pos.y,
                    gScale, x0, y0, x1, y1, NScreenObjState(pscr), NScreenObjPrio(pscr));
}
}  // namespace


JOBCARM3::~JOBCARM3() = default;

PADCTL* JOBCARM3::Ppad() const { return pguard ? PpadctlOfSo3(pguard->palo) : nullptr; }

// orig3: 0x15F7A8 (the base's Init; +0x30 / +0x38 / +0x40 / +0x70 -1; the guard's state 0x42 added: her player kind
// entries {0x42, n}; natively Sly 2's index 57, guard3.cpp)
void JOBCARM3::Init()
{
    AIJOB::Init();
    st = -1;
    stGun = -1;
    stAim = -1;
    if (pguard) AddGuardState(pguard, IstateGuard2FromIstate3(0x42));
}

// orig3: 0x15F7F0: the ground watcher off (0x259D10 on +0x4B0), the aim mode (0 free in the two-player games, else 1),
// the machine's first state (2 standing free, 0 standing aimed)
void JOBCARM3::Enter()
{
    AIJOB::Enter();
    GUARD* pg = pguard;
    if (!pg) return;
    // (her parts: the post-load 0x15F490's 0x1C8970(4, 0x344088): +0x17D4 the shot's ASEG, +0x17D8 / +0x17DC the charge,
    // +0x17E0..: natively bound here on first use)
    // (a copy made while the world runs (split screen's teams): the template's shot / charge clips, shared)
    if (!PloPartAt(pg->palo, 0x17D4) && !PloPartAt(pg->palo, 0x17DC))
    {
        SetBindPartsFromTemplate3(pg->palo->ploCloneOf != nullptr);
        BindLoPartsTable3(pg->palo, 4, 0x344088);
        SetBindPartsFromTemplate3(false);
    }
    if (FPrint())
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t va = 0x344088 + 12u * uint32_t(i);
            LO* pp = PloPartAt(pg->palo, g_disc.U32Elf(va + 8));
            std::printf("carm3: part row grf 0x%X id %d +0x%X -> %d cid 0x%X\n", g_disc.U32Elf(va), int(g_disc.U32Elf(va + 4)),
                        g_disc.U32Elf(va + 8), pp ? pp->oid : -1, pp ? pp->Cid() : 0);
        }
    tFloorLast = g_clock.t;
    ++pg->watchA.cOff3;
    pg->watchA.fOn = 0;
    SetAim(g_psw && g_psw->nScreenMode3 == 2 ? 0 : 1);
    SetState(stAim == 0 ? 2 : 0);
    if (FPrint())
        std::printf("carm3: %d enters (player %d, aim %d, parts shot %d charge %d / %d, forward %.2f %.2f %.2f)\n", pg->palo->oid,
                    IPlayerCarm3(pg->palo), stAim, PloPartAt(pg->palo, 0x17D4) ? PloPartAt(pg->palo, 0x17D4)->oid : -1,
                    PloPartAt(pg->palo, 0x17D8) ? PloPartAt(pg->palo, 0x17D8)->oid : -1,
                    PloPartAt(pg->palo, 0x17DC) ? PloPartAt(pg->palo, 0x17DC)->oid : -1, VecForward().x, VecForward().y, VecForward().z);
}

// orig3: 0x15F898: the machine, the gun and the aim reset, the watcher back on
void JOBCARM3::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    SetGun(-1);
    SetAim(-1);
    if (pguard && pguard->watchA.cOff3 > 0 && --pguard->watchA.cOff3 == 0) pguard->watchA.fOn = 1;   // (0x259D50)
    if (pguard) RemoveGuardState(pguard, IstateGuard2FromIstate3(0x42));   // (0x252FC0)
    fSteer = fHeading = false;
}

// orig3: 0x15FCA8: the aim toggle (0x15FCF8), the pad (0x1600F0), the gun (0x161380); (the power-ups 0x15FE68 and the
// pickups 0x161140: not ported)
void JOBCARM3::Update()
{
    AIJOB::Update();
    PADCTL* ppc = Ppad();
    if (pguard && FGuardFloor(pguard)) tFloorLast = g_clock.t;
    // (the aim zone's forced state, applied once the camera's cut has passed: SetAim below)
    if (nZonePending != -1 && pguard && g_psw && !g_cm.fCut)
    {
        if (LO* pz = PloFindSwObjectByOid(g_psw, OID(pguard->palo->U32Field(0, 0x17E8, 0xFFFFFFFFu)))) SetZoneForced(pz, nZonePending);
        nZonePending = -1;
    }
    // 0x15FCF8: Triangle switches the aim mode
    if (ppc && GrfPadctlTakePressed(ppc, BTN3_Triangle, 0.0f)) SetAim(stAim == 1 ? 0 : 1);
    if (pguard) KeepCarmFlashlightOff3(pguard->palo);
    UsePower();
    UpdatePad();
    UpdateGun();
}

// orig3: 0x15FE68 (the held power-up, L1 pressed with power in the meter, in a state that allows it): 0x32 the super
// jump (state 8; from 0..4, 11, 15), 0x33 the charged shot (gun 4, the gun idle; from 0..11, 19), cost 0x22 each; 0x34
// (state 0x11: to the robber, 0x15FB28 / 0x15FBD8) and 0x35 (a hit on the other player, 0x240CB0) cost 100 (from 0..4).
// At 0 the power-up is gone (0x161C60(-1)).
void JOBCARM3::UsePower()
{
    if (!pguard) return;
    auto it = s_mppower.find(pguard->palo);
    if (it == s_mppower.end() || it->second.kind == -1 || it->second.meter <= 0) return;
    CARMPOWER& pw = it->second;
    // (the target box's update while the charged shot is held, before the button: 0x162148; its draw 0x162370)
    if (pw.kind == 0x33)
    {
        CARMBOX& b = s_mpbox[pguard->palo];
        UpdateBox(pguard->palo, b);
        PlaceBox(pguard->palo, b);
    }
    PADCTL* ppc = Ppad();
    if (!ppc || !GrfPadctlPressedWithin(ppc, BTN3_L1, 0.0f)) return;
    bool fOk = false;
    switch (pw.kind)
    {
    case 0x32: fOk = (st >= 0 && st <= 4) || st == 11 || st == 15; break;
    case 0x33: fOk = stGun == -1 && ((st >= 0 && st <= 11) || st == 19); break;
    case 0x34: case 0x35: fOk = st >= 0 && st < 5; break;
    default: break;
    }
    if (!fOk) return;
    // 0x35 (the hit, orig3 0x15FE68's last case): a contact from her to the robber (the global player D_00370AC0: in the
    // two-player game Sly; PC teams: the nearest robber), not while he is hurt (his vt slot 81), made by 0x240CB0 with
    // the contact bytes 0x800000 and the amount 0x172268(0) = 0: no health lost, his hit reaction (the loot drops)
    LO* ploHit = nullptr;
    if (pw.kind == 0x35)
    {
        float dBest = 1e30f;
        for (int i = 0; i < split3::CView(); i += 2)
            if (LO* plo = split3::PloPlayer(g_psw, i); plo && FCidDerivedFrom(plo->Cid(), S3CID(0x34)))
            {
                const float d = glm::length(PosLoWorld(plo) - PosLoWorld(pguard->palo));
                if (d < dBest) dBest = d, ploHit = plo;
            }
        if (!ploHit && g_psw) ploHit = PloPoCurrent(g_psw);
        static const auto s_pfnStatus = reinterpret_cast<int (*)(LO*)>(PvNativeFn(0x140F70));   // (vt slot 81)
        if (!ploHit || (s_pfnStatus && s_pfnStatus(ploHit) != 0)) return;
    }
    GrfPadctlTakePressed(ppc, BTN3_L1, 0.0f);
    if (pw.kind == 0x32) SetState(8);
    else if (pw.kind == 0x33) SetGun(4);
    else if (pw.kind == 0x34) SetState(17);
    else
    {
        CTMSG ctm;
        ctm.ploSender = pguard->palo;
        ctm.ploReceiver = ploHit;
        ctm.grfFilter = 0x800000;
        ctm.nState = 0;
        ctm.pos = PosLoWorld(ploHit);
        PaloAcceptContact(&ctm);   // (0x240A28's offer)
    }
    pw.meter = std::max(0, pw.meter - (pw.kind == 0x34 || pw.kind == 0x35 ? 0x64 : 0x22));
    if (FPrint()) std::printf("carm3: %d power-up 0x%X used, meter %d\n", pguard->palo->oid, pw.kind, pw.meter);
    if (pw.meter <= 0)
    {
        SetCarmPowerKind3(pguard->palo, pw.kind, -1);   // (0x161C60(-1))
        pw.kind = -1;
    }
}

// orig3: 0x15FB28: the place of the power-up 0x34: with the robber (D_00370AC0) in the world (bit 56) a place of his safe
// ground (+0xA20, 0x1C0B70 with the flags 5: a floor he stood on, not on top of him, taken off his list; 0x1C03A0); none
// (or no robber in the world): her own place (0x1C9428: her position in the world). PC teams: the nearest robber.
glm::vec3 JOBCARM3::PosThrowTarget()
{
    ALO* palo = pguard->palo;
    XF* pxf = PxfPos(palo);
    const glm::vec3 posHer = pxf ? glm::vec3(pxf->matWorld[3]) : PosAloCenter(palo);
    if (LO* ploRobber = PloRobberNearest(palo))
        if (ACTOR* pa = PactorFromLo(ploRobber))
        {
            glm::vec3 pos;
            if (FTakeSafePlaceAway3(pa, &pos)) return pos;
            return PosLoWorld(ploRobber);   // (0x15FB28: the robber's vt slot 36 place when his list gives none)
        }
    return posHer;
}

// orig3: 0x15FBD8 (her clip's event 0x62 in state 0x11): she is put at the place (vt slot 34 with the place less her
// base +0x260), facing the robber (0x11C8C0 by the heading 0x1205C8 to him), her view's camera cut (0x166548)
void JOBCARM3::ArriveThrow()
{
    ALO* palo = pguard->palo;
    const glm::vec3 pos = posThrow - glm::vec3(St(pguard).vecBase);
    float h = St(pguard).radHeading;
    if (LO* ploRobber = PloRobberNearest(palo))
    {
        const glm::vec3 d = PosLoWorld(ploRobber) - posThrow;
        if (d.x != 0.0f || d.y != 0.0f) h = std::atan2(d.y, d.x);
    }
    const float mat[9] = {std::cos(h), std::sin(h), 0.0f, -std::sin(h), std::cos(h), 0.0f, 0.0f, 0.0f, 1.0f};
    sly3::PlaceLo3(palo, mat, pos);
    SetAloVelocityTo(palo, glm::vec3(0.0f));
    SetGuardHeading(pguard, h);
    radHeading = h;
    tFloorLast = g_clock.t;
    if (const int iview = IPlayerCarm3(palo); iview >= 0) CutCm(const_cast<CM*>(&split3::CmView(iview)));
    if (FPrint()) std::printf("carm3: %d arrives at %.0f %.0f %.0f facing %.2f\n", palo->oid, pos.x, pos.y, pos.z, h);
}

// the robber the power-ups aim at: the original's global player D_00370AC0 (Sly, player 1's character); PC teams: the
// nearest robber in the world
LO* PloRobberNearest(const LO* ploHer)
{
    if (!g_psw || !ploHer) return nullptr;
    LO* pbest = nullptr;
    float dBest = 1e30f;
    for (int i = 0; i < split3::CView(); i += 2)
        if (LO* plo = split3::PloPlayer(g_psw, i); plo && plo != ploHer && plo->fInWorld && FCidDerivedFrom(plo->Cid(), S3CID(0x34)))
            if (const float d = glm::length(PosLoWorld(plo) - PosLoWorld(ploHer)); d < dBest) dBest = d, pbest = plo;
    if (!pbest)
        if (LO* plo = PloPoCurrent(g_psw); plo && plo != ploHer && plo->fInWorld) pbest = plo;
    return pbest;
}

// orig3: 0x2BC1C8 / 0x2BC238 (a shuffled set: the next one, the set shuffled again (0x2BC2xx's ShuffleArray: each place
// swapped with a random one, then the first not the last one handed out) when all were handed out)
int NNextShuffled3(std::vector<int>& a, int& cursor)
{
    auto shuffle = [&]() {
        const int c = int(a.size());
        if (c < 3) return;
        const int last = c - 1;
        const int prev = a[size_t(last)];
        for (int i = 0; i < c; ++i) std::swap(a[size_t(i)], a[size_t(NRandInRange(0, last))]);
        if (a[0] == prev) std::swap(a[0], a[size_t(NRandInRange(1, last))]);
    };
    if (a.empty()) return -1;
    if (cursor < 0) { shuffle(); cursor = 0; }   // (the set's first shuffle, 0x2BC0A8)
    if (cursor >= int(a.size())) { shuffle(); cursor = 0; }
    return a[size_t(cursor++)];
}

namespace
{
// +0x18AC: her shuffled power-ups (0x15F3D8: {50, 51, 52, 53})
struct CARMBAG { std::vector<int> a{0x32, 0x33, 0x34, 0x35}; int cursor = -1; };
std::unordered_map<const LO*, CARMBAG> s_mpbag;
}  // namespace

// orig3: 0x161BE0 (Carmelita's vt slot 90: a power-up taken, the pickup's option 947, 0x10A4A0): in her player kind (her
// guard state 0x42: a pad drives her) the next of her shuffled four, her meter full (0x1F4438(+0x178, 100)), the kind
// set (0x161C60). Returns the kind (-1: not a player).
int GiveCarmPowerup3(LO* plo)
{
    if (!plo || IPlayerCarm3(plo) < 0) return -1;
    CARMBAG& b = s_mpbag[plo];
    const int kind = NNextShuffled3(b.a, b.cursor);
    SetCarmPower3(plo, kind, 100);
    if (FPrint()) std::printf("carm3: %d takes the power-up 0x%X\n", plo->oid, kind);
    return kind;
}

void SetCarmPower3(LO* plo, int kind, int meter)
{
    if (!plo) return;
    const int kindOld = s_mppower.count(plo) ? s_mppower[plo].kind : -1;
    s_mppower[plo] = CARMPOWER{kind, std::clamp(meter, 0, 100)};
    SetCarmPowerKind3(plo, kindOld, kind);
}

// orig3: 0x161C60: the power-up's kind changed: leaving the charged shot (0x33) puts the target box away (-1); taking it
// aims the box at the robber (D_00370AC0) in its state 0
void SetCarmPowerKind3(LO* plo, int kindOld, int kind)
{
    if (kindOld == kind) return;
    CARMBOX& b = s_mpbox[plo];
    if (kindOld == 0x33) SetBoxState(plo, b, -1);
    if (kind == 0x33)
    {
        b.ptarget = PloRobberNearest(plo);
        SetBoxState(plo, b, 0);
    }
}

int KindCarmPower3(const LO* plo)
{
    const auto it = s_mppower.find(plo);
    return it != s_mppower.end() && it->second.meter > 0 ? it->second.kind : -1;
}

// the child's done test (orig3 0x238AD0: natively the reaction's per job)
static bool FChildDoneCarm(const AIJOB* pc)
{
    if (!pc) return true;
    if (auto* p = dynamic_cast<const JOBMOVE*>(pc)) return p->st == 1;
    return false;
}

// orig3: 0x1612D0: her position (vt slot 36's) inside one of the world's sight-blocking shapes (the list SW +0x470C:
// class 0x14, Sly 2's 0x17, added by its slot 25 0x27F490: e.g. the Smoke Bomb's cloud; 0x27F258 the point test)
bool JOBCARM3::FInShape17() const
{
    if (!pguard || !g_psw) return false;
    ALO* palo = pguard->palo;
    XF* pxf = PxfPos(palo);
    const glm::vec3 pos = pxf ? glm::vec3(pxf->matWorld[3]) : PosAloCenter(palo);
    for (LO* plo : g_psw->mpcidaplo[S3CID(0x17)])
        if (plo && plo->Cid() == S3CID(0x17) && plo->fInWorld && FPointInShape(plo, pos)) return true;
    return false;
}

// orig3: 0x1600F0
void JOBCARM3::UpdatePad()
{
    PADCTL* ppc = Ppad();
    if (!ppc) return;
    GUARD* pg = pguard;
    // L3: a line (0x2459F8(0x2A)) and the player's camera reset (0x168670): the line only (the camera's reset is the
    // view camera's own)
    if (GrfPadctlTakePressed(ppc, BTN3_L3, 0.15f)) PsndStartSound(0x2A);
    const float z = UStick(ppc);
    int old;
    int guard = 0;
    do
    {
        old = st;
        switch (old)
        {
        case 0: case 1: case 2: case 3: case 4:
            // (0x2503C8: falling 0.1 s -> 10; 0x1612D0: in a water volume -> 16: not ported)
            if (g_clock.t - tFloorLast > 0.1f) { SetState(10); break; }
            if (FInShape17()) { SetState(16); break; }
            if (GrfPadctlTakePressed(ppc, BTN3_Cross, 0.15f)) { SetState(5); break; }
            if (GrfPadctlTakePressed(ppc, BTN3_Square, 0.15f)) { SetState(11); break; }
            if (GrfPadctlTakePressed(ppc, BTN3_Circle, 0.15f)) { SetState(18); break; }
            if (z > 0.0f)
            {
                if (stAim == 1) SetState(1);
                else SetState(z > 0.7f ? 4 : 3);
            }
            else SetState(stAim == 1 ? 0 : 2);
            break;
        case 5: case 6: case 7: case 8: case 9: case 10: case 19:
            // (0x238AD0: the jump job ended: natively landed after its take-off)
            if (g_clock.t - tSt > 0.2f && FGuardFloor(pg)) { SetState(stAim == 1 ? 0 : 2); break; }
            if (old != 7 && old != 10 && GrfPadctlTakePressed(ppc, BTN3_Cross, 0.15f)) { SetState(7); break; }
            if (GrfPadctlTakePressed(ppc, BTN3_Square, 0.15f)) { SetState(11); break; }
            break;
        case 11:
            if (g_clock.t - tFloorLast <= 0.1f && GrfPadctlTakePressed(ppc, BTN3_Cross, 0.15f)) { SetState(5); break; }
            if (FChildDoneCarm(pchild) || g_clock.t - tSt > 2.0f) SetState(stAim == 1 ? 0 : 2);
            break;
        case 12: case 13: case 17:
            if (FChildDoneCarm(pchild) || g_clock.t - tSt > 3.0f) SetState(stAim == 1 ? 0 : 2);
            break;
        case 14: break;   // (knocked out: the clip's event 0x32B ends it, Message)
        case 15: SetState(0); break;
        case 16:
            // (0x1612D0: inside a sight-blocking shape, e.g. Sly's Smoke Bomb cloud: its move 0x1D until she is out)
            if (!FInShape17()) { SetState(0); break; }
            if (g_clock.t - tFloorLast > 0.1f) SetState(10);
            break;
        case 18:
            if (g_clock.t - tFloorLast > 0.1f) { SetState(10); break; }
            if (FInShape17()) { SetState(16); break; }
            if (fCircleUp && GrfPadctlTakePressed(ppc, BTN3_Cross, 0.15f)) { SetState(19); break; }
            if (fCircleUp && g_clock.t - tSt > 1.0f) SetState(stAim == 1 ? 0 : 2);
            break;
        case 20:
            if (0.5f < g_clock.t - tSt) SetState(0);
            break;
        default: break;
        }
    } while (st != old && ++guard < 8);
    // the feet and the heading (applied after the move's steering: SteerCarm3)
    fSteer = fHeading = false;
    const glm::vec3 fwd = VecForward();
    switch (st)
    {
    case 3:
        fHeading = true;
        radHeading = RadStick(ppc);
        fSteer = true;
        vecVel = fwd * (-300.0f * cops3::GSpeedScale(pguard->palo));   // (copsmap3.h: the sprint power-up)
        break;
    case 4:
        fHeading = true;
        radHeading = RadStick(ppc);
        fSteer = true;
        vecVel = fwd * (-SRun(pguard->palo) * cops3::GSpeedScale(pguard->palo));
        break;
    case 1:
    {
        // aimed: facing the view, the feet toward the stick (slower backward: x 0.8; sideways between)
        const float a = St(pg).radHeading;
        const float g = RadStick(ppc);
        float speed = SRun(pg->palo) * cops3::GSpeedScale(pg->palo);
        float d = std::remainder(g - a, 2.0f * float(M_PI));
        if (std::fabs(d) > float(M_PI) * 0.5f) speed *= 0.8f;
        else speed *= std::cos(d) + (1.0f - std::cos(d)) * 0.8f;
        // the world velocity in the heading's frame (0x1C94E8), as the moves': -speed x forward turned by d
        const float c = std::cos(d), s = std::sin(d);
        const glm::vec3 v(fwd.x * c - fwd.y * s, fwd.x * s + fwd.y * c, fwd.z);
        fSteer = true;
        vecVel = v * -speed;
        break;
    }
    case 11:
        if (stAim == 0)
        {
            fHeading = z > 0.0f;
            radHeading = RadStick(ppc);
            fSteer = true;
            vecVel = fwd * (z * -800.0f);
        }
        break;
    case 18:
        if (!fCircleUp && !(ppc->grfbtn & BTN3_Circle)) fCircleUp = 1;
        break;
    case 5: case 6: case 7: case 8: case 9: case 10: case 19:
    {
        // in the air: the feet push nothing (the jump's own velocity carries her); the stick turns her
        fSteer = true;
        vecVel = glm::vec3(0.0f);
        if (z > 0.0f) { fHeading = true; radHeading = RadStick(ppc); }
        break;
    }
    default: break;
    }
}

// orig3: 0x160960: the old state left, the child job ended, the new one's move (JOBMOVE 0xC9 by state index)
void JOBCARM3::SetState(int n)
{
    if (st == n && n != 13) return;
    if (st == 18) fCircleUp = 0;
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    if (!pguard) return;
    auto move = [&](int nAnim3) {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));
        o->nAnim = IstateGuard2FromIstate3(nAnim3);   // (the AI code's numbers: guard3.cpp)
        o->Enter();
    };
    // the jumps (the strike job 0xCB's take-off speeds, its SO parameters x7C / x80: D_003457A8 215 / D_003457AC 315):
    // natively the move of the state and the launch velocity
    glm::vec3 vJump = pguard->palo ? VelAloPos(pguard->palo) : glm::vec3(0.0f);
    auto jump = [&](float sHorz, float sVert) {
        ALO* palo = pguard->palo;
        PADCTL* ppc = Ppad();
        glm::vec3 v = VelAloPos(palo);
        if (ppc && ppc->uJoy > 0.0f) v = glm::vec3(std::cos(ppc->radJoy), std::sin(ppc->radJoy), 0.0f) * (sHorz * ppc->uJoy);
        v.z = sVert;
        vJump = v;
        SetAloVelocityTo(palo, v);
        tFloorLast = g_clock.t - 1.0f;
    };
    auto airmove = [&](int nAnim3) {
        // (debug: SLY3_CARMSTANDJUMP=1 the round-3 stand's move in the air)
        static const bool s_fStand = std::getenv("SLY3_CARMSTANDJUMP") != nullptr;
        if (s_fStand) { move(1); return; }
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));
        o->nAnim = IstateGuard2FromIstate3(nAnim3);
        o->Enter();
        if (o->pkinde && o->pkinde->k == 2 && o->pkinde->pvState)
        {
            // (the kind-2 flight to where the launch velocity carries her in the flight's time: natively 0xCB's steering)
            K2PLAY* pk = static_cast<K2PLAY*>(o->pkinde->pvState);
            pk->fBallistic = 0;
            ALO* palo = pguard->palo;
            const float dt = pk->ptuning ? std::max(pk->ptuning->dt14, 0.1f) : 0.6f;
            glm::vec3 v = vJump;
            v.z = 0.0f;
            XF* pxf = PxfPos(palo);
            glm::vec3 pos = (pxf ? glm::vec3(pxf->matWorld[3]) : PosAloCenter(palo)) + v * dt;   // (the record's: kind2.cpp)
            // (the fall: no take-off, the drop the flight's gravity gives in its time)
            if (nAnim3 == 0x16 && pk->ptuning) pos.z += 0.5f * pk->ptuning->g28 * dt * dt;
            SetK2Target(pk, pos);
        }
        if (FPrint()) std::printf("carm3: air move 0x%X -> %d kind %d st %d\n", nAnim3, o->nAnim, o->pkinde ? o->pkinde->k : -9, o->st);
    };
    switch (n)
    {
    case 2: move(1); break;
    case 0: move(2); break;
    case 20: SetGun(-1); move(2); break;
    case 1: move(13); break;
    case 3: move(14); break;
    case 4: move(15); break;
    case 11: SetGun(-1); move(3); break;
    case 18: SetGun(-1); move(0x19); break;
    // (the air states' Sly 3 moves 0x15..0x1A (her kinds {66, 21..26}) as JOBMOVEs walked her to the world's origin:
    // natively the stand's move, the velocity the jump gives)
    case 5: case 6: jump(215.0f, 450.0f); airmove(0x15); break;
    case 7: jump(315.0f, 450.0f); airmove(0x17); break;
    case 8: jump(1200.0f, 1000.0f); airmove(0x18); break;
    case 19: jump(900.0f, 600.0f); airmove(0x1A); break;
    case 9: case 10: airmove(0x16); break;   // (9: 0xCB mode 4 toward a hit's point: the fall's move)
    case 12: case 13: SetGun(-1); move(7); break;
    case 14:
        SetGun(-1);
        move(0x1E);
        Broadcast3(0x10, pguard->palo);   // 0x1854C8(D_0036C700, 0x10, actor): she is knocked out
        break;
    case 15: SetGun(-1); move(0x1B); break;
    case 16: SetGun(-1); move(0x1D); break;
    case 17:
    {
        // (the power-up 0x34) her move 0x1C; the place she goes to (0x15FB28), the effect +0x18B4 (option 1415, a PE)
        // put there out of its parent with no turn (0x1C7A00 / vt slots 34 / 35) and started (0x1FE728(pe, 1))
        SetGun(-1);
        move(0x1C);
        posThrow = PosThrowTarget();
        LO* ppe = g_psw ? PloFindSwObjectByOid(g_psw, OID(pguard->palo->U32Field(0, 0x18B4, 0xFFFFFFFFu))) : nullptr;
        if (ppe && FCidDerivedFrom(ppe->Cid(), S3CID(0x30)))
        {
            ALO* pape = static_cast<ALO*>(ppe);
            if (pape->paloParent) SetAloParent(pape, nullptr, nullptr, 0);
            const float mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            sly3::PlaceLo3(ppe, mat, posThrow);
            TriggerPe(ppe);
        }
        if (FPrint()) std::printf("carm3: %d power-up 0x34 to %.0f %.0f %.0f (effect %d)\n", pguard->palo->oid, posThrow.x, posThrow.y, posThrow.z, ppe ? ppe->oid : -1);
        break;
    }
    default: break;
    }
    if (FPrint()) std::printf("carm3: %d state %d (t %.2f)\n", pguard->palo->oid, n, g_clock.t);
}

// orig3: 0x15FD58: the aim mode; 1: the aim's HUD (+0x17E4, a screen object) placed in her player's camera's rectangle
// (0x262770), shown (vt slot 56) on its part 0x3BE (0x262B60, the handle +0x70), hidden (slot 54) when it ends; the aim's
// camera zone +0x17E8 (option 1416, Sly 3 EOPID 1544) forced on for her player's camera (0x273E48: the camera's list of
// forced zones), off when the aim ends (0x273ED0). Natively the zone's forced state (SetZoneForced) once the camera's cut
// has passed: Sly 3's list is read by the camera after the cut, so the aim camera starts from the view it takes over
// (the reference, --warp 3:2163, vblanks 1900..4200: the aim camera's pan 0.7656 / tilt -0.164 / distance 380, kept
// through her deaths; natively the forced zone at the load's cut took the zone's mid tilt 0.53 and her facing: a high
// camera looking down). One player only (the two-player views keep their own cameras; the aim is off there).
void JOBCARM3::SetAim(int n)
{
    if (stAim == n) return;
    LO* phud = pguard && g_psw ? PloFindSwObjectByOid(g_psw, OID(pguard->palo->U32Field(0, 0x17E4, 0xFFFFFFFFu))) : nullptr;
    if (phud && !FCidDerivedFrom(phud->Cid(), S3CID(0x7E))) phud = nullptr;   // (a SCREENOBJ: Sly 3 0x9E)
    // (debug: SLY3_CARMNOAIMZONE=1 the zone left alone, as before round 22)
    const bool fZone = pguard && g_psw && g_psw->nScreenMode3 != 2 && !std::getenv("SLY3_CARMNOAIMZONE") &&
                       PloFindSwObjectByOid(g_psw, OID(pguard->palo->U32Field(0, 0x17E8, 0xFFFFFFFFu)));
    if (FPrint()) std::printf("carm3: aim %d -> %d zone %d\n", stAim, n, fZone ? 1 : 0);
    if (stAim == 1 && fZone) nZonePending = 0;
    if (n == -1 && nZonePending != -1)
    {
        // (the job's exit: no update follows; at once)
        if (fZone) SetZoneForced(PloFindSwObjectByOid(g_psw, OID(pguard->palo->U32Field(0, 0x17E8, 0xFFFFFFFFu))), 0);
        nZonePending = -1;
    }
    if (stAim == 1 && phud)
    {
        HideScreenObj(phud);
        idAimPart = -1;
    }
    stAim = n;
    if (n == 1 && fZone) nZonePending = 1;
    tAim = g_clock.t;
    if (n == 1 && phud)
    {
        // (the camera's rectangle: the original's two-player views are player 1's top half and player 2's bottom half;
        // one player: the whole screen)
        const int iplayer = pguard ? IPlayerCarm3(pguard->palo) : -1;
        if (g_psw->nScreenMode3 == 2 && (iplayer == 0 || iplayer == 1)) SetScreenObjRootRect3(phud, 0, 240 * iplayer, 640, 240 * (iplayer + 1));
        else SetScreenObjRootRect3(phud, 0, 0, 640, 480);
        // (PC: the two-player frame (Cops and Robbers' 3325, priority over the aim's) hid the aim's brackets under its
        // view box's hole; the aim drawn above it)
        if (g_psw->nScreenMode3 == 2)
            if (LO* pframe = PloFindSwObjectByOid(g_psw, 3325); pframe && NScreenObjPrio(phud) <= NScreenObjPrio(pframe))
                SetScreenObjPrio(phud, NScreenObjPrio(pframe) + 1);
        ShowScreenObj(phud, -1);
        SelectScreenObjPartHandle(phud, 0x3BE, &idAimPart);
    }
}

// orig3: 0x161380: the gun (R1): pressed with the gun ready (+0x17EC: 0x1FBEA0) -> 0 the shot; held -> 1 the charge
// (the SM +0x17DC to its state 0x32A) -> 2 charged; let go -> 3 the charged shot; the shot's end -> -1. Not while the
// machine plays a strike, a hit or a fall (11..18, 20).
void JOBCARM3::UpdateGun()
{
    if (st >= 11 && (st < 19 || st == 20)) return;
    PADCTL* ppc = Ppad();
    if (!ppc || !pguard) return;
    int old;
    int guard = 0;
    do
    {
        old = stGun;
        switch (old)
        {
        case -1:
            if (GrfPadctlTakePressed(ppc, BTN3_R1, 0.25f)) SetGun(0);
            break;
        case 1:
            if (!(ppc->grfbtn & BTN3_R1)) SetGun(-1);
            else if (psmaCharge && OidSmaCur(static_cast<SMA*>(psmaCharge)) == 0x32A) SetGun(2);
            break;
        case 2:
            if (!(ppc->grfbtn & BTN3_R1)) SetGun(3);
            break;
        case 0: case 3: case 4:
            if (!pasegaShot || FAsegaRetracted(static_cast<ASEGA*>(pasegaShot))) SetGun(-1);
            break;
        default: break;
        }
    } while (stGun != old && ++guard < 8);
}

// orig3: 0x1614E8
void JOBCARM3::SetGun(int n)
{
    if (stGun == n || !pguard) return;
    stGun = n;
    tGun = g_clock.t;
    ALO* palo = pguard->palo;
    SMA** ppsma = reinterpret_cast<SMA**>(&psmaCharge);
    ASEGA** ppasega = reinterpret_cast<ASEGA**>(&pasegaShot);
    switch (n)
    {
    case -1:
        if (*ppsma) ClearSmaHandle(ppsma);
        if (*ppasega) RetractAsegaPp(ppasega);
        break;
    case 0: case 3: case 4:
        if (LO* paseg = PloPartAt(palo, 0x17D4))
        {
            ClearSmaHandle(ppsma);
            ApplyAsegCur(static_cast<ASEG*>(paseg), palo, 0.0f, 1.0f, 1, ppasega);   // (0x137588)
        }
        else SetGun(-1);
        break;
    case 1:
        if (LO* psm = PloPartAt(palo, 0x17DC); psm && PloPartAt(palo, 0x17D8))
        {
            RetractAsegaPp(ppasega);
            ReplaceSmaHandle(static_cast<SM*>(psm), palo, 0x329, 1, ppsma);   // (0x23AAE8)
            SetSmaGoal(*ppsma, 0x32A);
        }
        else SetGun(-1);
        break;
    default: break;
    }
    if (FPrint()) std::printf("carm3: %d gun %d\n", palo->oid, n);
}

// orig3: 0x15F918 (natively the ASEG events come as message 9 {asega, t, n}: Sly 3's 10 {.., data[2]}): 0x2AA the shot
// (0x161610); (0x32B the death's end, 0x62 a power-up's throw: their states are not ported)
int JOBCARM3::Message(int msg, void* pm)
{
    if (msg == 9 && pm)
    {
        const ASEGMSG9* pev = static_cast<const ASEGMSG9*>(pm);
        if (FPrint() && st == 14) std::printf("carm3: %d event 0x%X in state 14\n", pguard ? pguard->palo->oid : -1, pev->n);
        if (pev->n == 0x2AA)
        {
            Fire();
            return 1;
        }
        if (pev->n == 0x32B && st == 14)   // (her knock-out's end: the death broadcast; the job's owner places her again)
        {
            Broadcast3(12, pguard->palo);
            return 1;
        }
        if (pev->n == 0x62 && st == 17)    // (the power-up 0x34's clip: she arrives, 0x15FBD8)
        {
            ArriveThrow();
            return 1;
        }
        return 0;
    }
    if (msg == 12 && pguard)
    {
        // a hit (orig3 message 18; natively the guard's hit handler FAcceptContactGuard lowered her health already):
        // at 0 knocked out; else in the two-player games (0x36DBE0 == 2) her hit reaction 12 (move 7, as message 15's
        // 13), elsewhere stunned 20 (after 0x1C11E0 took her to her safe place: not ported, the turf war's)
        if (st != 14) SetState(pguard->n33C <= 0 ? 14 : g_psw && g_psw->nScreenMode3 == 2 ? 12 : 20);
        return 1;
    }
    return AIJOB::Message(msg, pm);
}

namespace
{
// orig3: 0x212D50: an object launched from where it is at a point: into the world, its constraints free (0x23EBF8 /
// 0x23EC98), out of its parent keeping its place (0x1C7A00(0, 3)), no spin; a HOMINGMISS (Sly 3 0x5C) at its launch
// speed (+0x240) along its own x, an ACCMISS (0x5B) at it along the line facing it (0x11CBA0), any other ballistically
// (0x212480 with the pitch share 0.5 under its gravity); a MISSILE (0x5A) gets the point as its target (vt slot 82)
void LaunchShot3(ALO* pp, const glm::vec3& posAim)
{
    const glm::vec3 posStart = PosLoWorld(pp);
    if (!pp->fInWorld) AddLo(g_psw, pp);
    SetAloCnstrLin(pp, 0);
    SetAloCnstrAng(pp, 0);
    SetAloParent(pp, nullptr, nullptr, 3);
    SetAloAngVelocityTo(pp, glm::vec3(0.0f));
    float gSpeed = 800.0f;   // (0x1B1DB8's default for the missiles; option 509 when loaded)
    {
        uint32_t bits;
        std::memcpy(&bits, &gSpeed, 4);
        const uint32_t u = pp->U32Field(0, 0x260, bits);
        std::memcpy(&gSpeed, &u, 4);
    }
    if (FCidDerivedFrom(pp->Cid(), S3CID(0x52))) SetAloVelocity(pp, glm::vec3(gSpeed, 0.0f, 0.0f));
    else if (FCidDerivedFrom(pp->Cid(), S3CID(0x51)))
    {
        const glm::vec3 d = posAim - posStart;
        const float l = glm::length(d);
        const glm::vec3 v = (l < 0.0001f ? VecForward() : d / l) * gSpeed;
        SetAloVelocityTo(pp, v);
        SetAloRotation(pp, BuildLookAtMatrixZ(v));
    }
    else
    {
        const SOST* ps = Psost(pp);
        SetAloVelocityTo(pp, VelBallisticLaunch(0.5f, posStart, posAim, ps ? ps->dvGravity : glm::vec3(0.0f)));
    }
    if (FCidDerivedFrom(pp->Cid(), S3CID(0x50)))
    {
        using PFN = void (*)(ALO*, LO*, const glm::vec3&);
        if (PFN pfn = PfnVt<PFN>(pp, 78)) pfn(pp, nullptr, posAim);
    }
}
}  // namespace

// orig3: 0x161610: the gun's projectile (+0x17EC for the plain shot 0, +0x17F0 / +0x17F4 the charged ones 3 / 4) taken from
// its pool (0x1FBE30), its owner (+0x1E8) her, launched (0x212D50) at the aim point 0x1619F8: the target (0x161720) a
// quarter second ahead; none: 4000 ahead of her (free aim) or of her camera (aimed)
void JOBCARM3::Fire()
{
    if (!pguard || !g_psw) return;
    const uint32_t ib = stGun == 3 ? 0x17F0 : stGun == 4 ? 0x17F4 : 0x17EC;
    ALO* palo = pguard->palo;
    LO* pproxy = PloFindSwObjectByOid(g_psw, OID(palo->U32Field(0, ib, 0xFFFFFFFFu)));
    if (!pproxy && ib != 0x17EC) pproxy = PloFindSwObjectByOid(g_psw, OID(palo->U32Field(0, 0x17EC, 0xFFFFFFFFu)));
    LO* po = pproxy ? PloAcquireProxyObject(g_psw, pproxy, false) : nullptr;
    if (!po || !FCidDerivedFrom(po->Cid(), S3CID(0x30)))
    {
        if (FPrint()) std::printf("carm3: %d shot: no projectile (proxy %d)\n", palo->oid, pproxy ? pproxy->oid : -1);
        return;
    }
    ALO* pp = static_cast<ALO*>(po);
    SetProjectileLauncher(pp, palo);   // (object +0x1E8 = her)
    // 0x1619F8: the aim point
    glm::vec3 posAim;
    const auto itBox = s_mpbox.find(palo);
    if (stGun == 4 && itBox != s_mpbox.end() && itBox->second.state == 2 && itBox->second.ptarget)
        posAim = glm::vec3(sly3::MatLoWorld3(itBox->second.ptarget)[3]);   // (0x161610: the charged shot at the locked box's target)
    else if (LO* pt = PloShotTarget())
        posAim = PosLoWorld(pt) + (FCidDerivedFrom(pt->Cid(), S3CID(0x30)) ? VelAloPos(static_cast<ALO*>(pt)) * 0.25f : glm::vec3(0.0f));
    else if (stAim == 0)
    {
        const float rad = St(pguard).radHeading;
        posAim = PosLoWorld(palo) + glm::vec3(std::cos(rad), std::sin(rad), 0.0f) * 4000.0f;
    }
    else
    {
        // 0x167838(her view's camera, (0, 0, 4000)): the point 4000 along the view
        const int iview = std::max(0, IPlayerCarm3(palo));
        const CM& cm = split3::CmView(iview);
        posAim = cm.pos + glm::vec3(cm.mat[0]) * 4000.0f;
    }
    {
        // (PC: a hub where her projectiles' proxy is not near her (the other hubs' Carmelita for copsmap3.h: the pool
        // puts the shot at the world's origin) the shot starts at her gun's height in front of her)
        const glm::vec3 posHer = PosLoWorld(palo);
        if (glm::length(PosLoWorld(pp) - posHer) > 600.0f)
        {
            const float rad = St(pguard).radHeading;
            const float mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            sly3::PlaceLo3(pp, mat, posHer + glm::vec3(std::cos(rad), std::sin(rad), 0.0f) * 60.0f + glm::vec3(0.0f, 0.0f, 20.0f));
        }
    }
    LaunchShot3(pp, posAim);
    if (FPrint()) std::printf("carm3: %d shot %d (cid 0x%X, proxy %d) from %.0f %.0f %.0f toward %.0f %.0f %.0f v %.0f %.0f %.0f\n", palo->oid,
                              po->oid, po->Cid(), pproxy->oid, PosLoWorld(pp).x, PosLoWorld(pp).y, PosLoWorld(pp).z, posAim.x, posAim.y,
                              posAim.z, VelAloPos(pp).x, VelAloPos(pp).y, VelAloPos(pp).z);
}

// orig3: 0x161720: the shot's target: of the POs in the world (class 0x32; the list +0x17F8 first when it has entries: none
// natively) but herself and the friendly guards (class 0x3A with +0x2F0, option 264, 2), the nearest within her range
// (+0x187C: Sly 3 EOPID 1549, 6000 by default) and 150 away at least, within 0.3 rad up / down and 0.2 rad of her heading
// (aimed: within +-0.4 of the view)
LO* JOBCARM3::PloShotTarget()
{
    if (!pguard || !g_psw) return nullptr;
    ALO* palo = pguard->palo;
    const glm::vec3 pos = PosLoWorld(palo);
    float gMax = 6000.0f;
    for (const OPT& o : palo->aopt)
        if (o.eopid == 1549 && !o.aval.empty() && o.aval[0].ag[0] > 0.0f) gMax = o.aval[0].ag[0];
    const float radYaw = St(pguard).radHeading;
    LO* pBest = nullptr;
    for (int cid = 0; cid < int(g_psw->mpcidaplo.size()); ++cid)
    {
        if (g_psw->mpcidaplo[size_t(cid)].empty() || !FCidDerivedFrom(cid, 0x32)) continue;
        for (LO* p : g_psw->mpcidaplo[size_t(cid)])
        {
            if (p == palo || !p->fInWorld) continue;
            if (FCidDerivedFrom(p->Cid(), 0x3A))
            {
                // (+0x2F0, option 264: 2 friendly; as set at run time, else as loaded)
                bool fFriend = p->U32Field(0, 0x2F0, 0) == 2;
                for (const OPT& o : p->aopt)
                    if (o.eopid == 264 && !o.aval.empty() && o.aval[0].n == 2) fFriend = true;
                if (fFriend) continue;
            }
            const glm::vec3 d = PosLoWorld(p) - pos;
            const float l = glm::length(d);
            if (l > gMax || l < 150.0f) continue;
            if (stAim == 1)
            {
                // aimed (+0x40 1): in front of her view's camera (0x1F4A80 -> 0x1678C8) within +-0.4 of its screen
                // (D_00320B28 / B30): high targets (the turf war's wave guards over the canal) are taken when aimed at
                const CM& cm = split3::FActive() ? split3::CmView(std::max(0, IPlayerCarm3(palo))) : g_cm;
                const glm::vec3 q = ConvertCmWorldToScreen(&cm, PosLoWorld(p));
                if (q.z < 0.0f || std::fabs(q.x) > 0.4f || std::fabs(q.y) > 0.4f) continue;
            }
            else
            {
                // free (0x120BA8's pitch within +-0.3 (D_00320B38), the yaw within +-0.2 of her heading (D_00320B40))
                const float radPitch = std::atan2(d.z, glm::length(glm::vec2(d)));
                const float radDir = std::atan2(d.y, d.x);
                if (std::fabs(radPitch) > 0.3f || std::fabs(std::remainder(radDir - radYaw, 2.0f * float(M_PI))) > 0.2f) continue;
            }
            gMax = l;
            pBest = p;
        }
    }
    return pBest;
}

void SteerCarm3(GUARD* pg)
{
    if (!pg || !pg->palo || IPlayerCarm3(pg->palo) < 0) return;
    JOBCARM3* p = nullptr;
    for (const auto& pj : pg->stack.ajob)
        if (pj->cid == CID_JOBCARM3) p = static_cast<JOBCARM3*>(pj.get());
    if (!p) return;
    if (p->fHeading) SetGuardHeading(pg, p->radHeading);
    if (p->fSteer) St(pg).vecVel = p->vecVel;
    if (static int s_c = 0; FPrint() && (++s_c % 30) == 0)
    {
        const glm::vec3 v = VelAloPos(pg->palo), pos = PosLoWorld(pg->palo);
        std::printf("carm3: steer st %d want %.2f heading %.2f vecVel %.0f %.0f %.0f vel %.0f %.0f %.0f at %.0f %.0f %.0f stick %.2f u %.2f\n", p->st,
                    p->radHeading, St(pg).radHeading, St(pg).vecVel.x, St(pg).vecVel.y, St(pg).vecVel.z, v.x, v.y, v.z, pos.x, pos.y,
                    pos.z, p->Ppad() ? p->Ppad()->radJoy : 0.0f, p->Ppad() ? p->Ppad()->uJoy : 0.0f);
    }
}

void SetCarmPlayer3(LO* plo, int iplayer)
{
    if (!plo) return;
    const int iOld = IPlayerCarm3(plo);
    if (iOld == iplayer) return;
    GUARD* pg = PguardFromLo(plo);
    JOBREACT* pj = pg ? static_cast<JOBREACT*>(PjobFind(pg, 0x9C)) : nullptr;
    // orig3 0x162810: the reaction told 0 (back to its AI) before the pad goes, 0x14 (this job: reaction kind 20) after
    // it comes
    // (+0x130: the player index the scripts read as the SO's option 313, Sly 3 EOPID 941; -1 none)
    plo->SetField(0, 0x130, static_cast<uint32_t>(iplayer));
    if (iplayer == -1)
    {
        s_mpiplayer.erase(plo);
        if (pj)
        {
            AIMSG m;
            m.k = 1;
            pj->React(m);
        }
        return;
    }
    s_mpiplayer[plo] = iplayer;
    SetCarmFlashlightOff3(plo);   // (her flashlight off while a person plays her: the original's s1414 #f, see below)
    if (pj)
    {
        AIMSG m;
        m.k = 20;
        pj->React(m);
    }
    if (FPrint()) std::printf("carm3: %d takes player %d's pad (reaction job %p)\n", plo->oid, iplayer, (void*)pj);
}

int IPlayerCarm3(const LO* plo)
{
    const auto it = s_mpiplayer.find(plo);
    return it == s_mpiplayer.end() ? -1 : it->second;
}

// ---- her flashlight (user report, round 9) ----
// The original has no code for it: Carmelita's own splice (block 28, module 0; the turf war's Carmelita 147604 the same
// with her own groups) binds her sidebag option 1839 at her load (s0: (addo self 1839 (lambda (on) (s1414 self on)))). Its
// s1414 with #t shows her flashlight (option 111 of the mesh groups 8699 / 8697 / 8620: the beam's projected volumes and
// the lamp) and puts its sight shapes in the world (method 41 of 8701, a SHAPE, and 8683, a LOX: the cone her detector
// sees through); with #f it hides the groups (112) and takes the shapes out (42). Her reset (event 17) calls it by her kind
// (option 1824): on for the guard kinds 891 / 14058 (single player's patrols and chases), off for 14059..14062, her
// playable kinds (the turf war's, Cops and Robbers' 14062). Natively a player Carmelita whose kind is a guard's (another
// hub's own Carmelita in the PC rounds, a copy) kept it on. Now every Carmelita that takes a pad gets s1414 #f, and again
// when a reset of hers turned it back on (the groups / shapes it changed are watched while she has the pad).
namespace
{
struct FLASH
{
    bool fApplied = false;
    std::vector<int32_t> agrp;    // the groups the call hid
    std::vector<LO*> aplo;        // the objects it took out of the world
};
std::unordered_map<const LO*, FLASH> s_mpflash;

void CollectOwned(LO* plo, std::vector<LO*>& aplo, int depth = 0)
{
    if (!plo || depth > 4) return;
    for (LO* pc : plo->aploOwned)
        if (pc)
        {
            aplo.push_back(pc);
            CollectOwned(pc, aplo, depth + 1);
        }
}

// her s1414 called with (her, fOn): found through the procedure of option 1839 (her own, else her template's: a copy's
// cloned sidebag still closes over the template, so s1414 is called with the copy itself)
bool FCallFlashlight(LO* plo, bool fOn)
{
    CRef ref;
    LO* pfrom = plo;
    while (pfrom && !(pfrom->psidebag && pfrom->psidebag->FFindBinding(1839, &ref) && ref.m_tagk == TAGK_Proc && ref.m_pproc))
        pfrom = pfrom->ploCloneOf;
    if (!pfrom) return false;
    CRef* p1414 = ref.m_pproc->m_pframe ? ref.m_pproc->m_pframe->PrefFindBinding(SYMID(1414), 1) : nullptr;
    CRef a[2];
    a[0].SetBasic(plo);
    a[1].SetBool(fOn);
    if (p1414 && p1414->m_tagk == TAGK_Proc) RefApply(*p1414, 2, a, nullptr);
    else if (pfrom == plo) RefApply(ref, 1, &a[1], nullptr);
    else return false;
    return true;
}

// (debug: SLY3_CARMFLASHLIGHT=1 the old way: a player Carmelita keeps her kind's flashlight)
bool FKeepFlashlight() { static const bool s_f = std::getenv("SLY3_CARMFLASHLIGHT") != nullptr; return s_f; }
}  // namespace

void SetCarmFlashlightOff3(LO* plo)
{
    if (!plo || FKeepFlashlight() || !FCidDerivedFrom(plo->Cid(), S3CID(0x30))) return;
    ALO* palo = static_cast<ALO*>(plo);
    FLASH& f = s_mpflash[plo];
    const auto aglobBefore = palo->aglobShown;
    std::vector<LO*> aploOwned;
    CollectOwned(plo, aploOwned);
    std::vector<uint8_t> afIn;
    for (LO* p : aploOwned) afIn.push_back(p->fInWorld ? 1 : 0);
    if (!FCallFlashlight(plo, false)) return;
    f.fApplied = true;
    for (const auto& [n, fShown] : palo->aglobShown)
    {
        if (fShown) continue;
        bool fWas = true;
        for (const auto& [n0, f0] : aglobBefore)
            if (n0 == n) fWas = f0;
        if (fWas && std::find(f.agrp.begin(), f.agrp.end(), n) == f.agrp.end()) f.agrp.push_back(n);
    }
    for (size_t i = 0; i < aploOwned.size(); ++i)
        if (afIn[i] && !aploOwned[i]->fInWorld && std::find(f.aplo.begin(), f.aplo.end(), aploOwned[i]) == f.aplo.end())
            f.aplo.push_back(aploOwned[i]);
    if (FPrint())
        std::printf("carm3: %d flashlight off (%zu groups, %zu objects out)\n", plo->oid, f.agrp.size(), f.aplo.size());
}

// while she has a pad: the call again when it has not run yet (her splice not bound at the pad's arrival) or a reset of
// hers (event 17 by her kind) showed the flashlight again
void KeepCarmFlashlightOff3(LO* plo)
{
    if (!plo || FKeepFlashlight() || IPlayerCarm3(plo) < 0 || !FCidDerivedFrom(plo->Cid(), S3CID(0x30))) return;
    auto it = s_mpflash.find(plo);
    bool fAgain = it == s_mpflash.end() || !it->second.fApplied;
    if (!fAgain)
    {
        for (int32_t n : it->second.agrp) fAgain = fAgain || FAloGlobShown(static_cast<ALO*>(plo), n, false);
        for (LO* p : it->second.aplo) fAgain = fAgain || p->fInWorld;
    }
    if (fAgain) SetCarmFlashlightOff3(plo);
}

void ResetCarm3() { s_mpiplayer.clear(); s_mppower.clear(); s_mpflash.clear(); s_mpbag.clear(); s_mpbox.clear(); }
