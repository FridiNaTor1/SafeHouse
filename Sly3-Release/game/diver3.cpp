// diver3.cpp: Sly 3's diving SO (class 0x57, orig3 0x26BE28..0x26DF80) and the creatures that swim along paths (class
// 0x7E, orig3 0x232258..0x232A40), from the Sly 3 decomp (AUDIT4 MODES-02 / 03).
//
// Ep5's dive (p_dive, world 33, job 4101: search, collect, catch the fish, kill the sharks) and world 36: the player's
// SO (e.g. 27666) is a first-person swimmer. Ported:
//  - the init 0x26BE28 (the defaults) and the options 1912..1932 (tilt limits, the look rates, the shots' proxies 1919..
//    1921 and their launcher 1922, the surface object 1924, the mode 1927, the partner 1928 / 1929, the stroke 1930,
//    the hit flash 1932);
//  - the update 0x26C088 (vt slot 18): the states 0x26CD30 (0 off, 1 swimming, 2 at the surface, 3 going down, 4 coming
//    back down, 5 dead: task event 0xC), the input 0x26D470 (Triangle / Cross forward / back (up / down in modes 1 / 2),
//    Square / Circle left / right: a stroke 0x26D1E8; R1 a shot 0x26D078; a face button with L1 held: a turbo stroke for
//    25 energy), the look 0x26DAB8 (the stick's pan / tilt rates with their dead zones and ramps), the hit points and
//    flash 0x26D9F0, the energy refill 0x26D7B0 (25 a second), the surfacing 0x26D340 (in contact with the surface object
//    and clear 400 above: up there, state 2; Triangle looking down or Cross looking up goes back down: state 3, after
//    0.5 s at the place it left, state 4, then 1); the horn on L3 (sound 0x2A);
//  - the forces 0x26C420 (vt slot 74): gravity, the stroke's impulse (the stroke clip 2250 x (1 - sin(pi t) / 2), x3
//    turbo), mode 0's drag (along the heading -0.9375 / across -1.875 while moving forward, else -1.875), mode 1's spring
//    to the partner +0x270 at +0x274, mode 2's follow of the partner's point +0x274 behind it; the orientation driven to
//    the pan / tilt (here placed on it each frame);
//  - the hits 0x26C990 (vt slot 78), slots 80 / 81 (0x26CB10 / 0x26CB70), the enable 0x26C8F8 (vt slot 83: the current
//    player's: state 1, else 0), the reset 0x26BFC0 (vt slot 31), the release 0x26C040 (slot 32), slot 35 0x26CC78 (the
//    pan / tilt from a new orientation), the first-person camera (slot 82 0x26C948 -> focus kind 13: 0x26DEC8, the
//    object's own matrix, the field of view 1.4).
// The HUD screen object +0x2C0 (EOPID 1923: shown on the way into the states 1..5, its meters' parts 0x3CA / 0x3D2, the
// labels 0x3EF / 0x3F0 / 0x3F1; state 4 ends with the label 0x3F1), the 13 clips bound by the post-load 0x26BF60 (table
// 0x349CD0: +0x27C going down, +0x280 / +0x284 the dive strokes from the surface, +0x2A4 at the surface, +0x2A8 the
// death), the camera scripts +0x1D4 / +0x1D8 (EOPIDs 1925 / 1926, started going down / up: 0x225EE8).
// The cross-hair +0x250 / +0x260 (modes 1 / 2: the left stick, 0x26D470; eased and drawn by its HUD part 0x3BC,
// 0x26D7B0; the shots go through it, 0x26D078), the stance method 1931 (0x110A20 -> 0x26CD28: the shots' proxy) and
// the HUD meters' fills 0x3CA / 0x3D2 (s3fix-modes3).
// The creatures (0x7E, e.g. p_dive's 5 sharks (40 hit points) and 12 fish (1)): see the section below (flight legs,
// search, the path's speed envelope); their lights 0x232710 (vt slot 21) are scenesw.cpp's light-falloff pass (1b28d55e).
// The charm (vt slot 87 0x26CCC0: hit points + a, +0x174 + b) is pickup.cpp's (AUDIT5 MODES5-03).
// Debug: SLY3_DIVEPRINT=1 (states, strokes, shots, hits; every second the swimmer).

#include "../activate.h"
#include "../alo.h"
#include "../asega.h"
#include "../aseg.h"
#include "../animfind.h"
#include "../brx.h"

#include "../pe.h"
#include "../detect.h"
#include "../sm.h"
#include "../smrun.h"
#include "../clock.h"
#include "../cm.h"
#include "../cplcy.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../po.h"
#include "../projectile.h"
#include "../rumble.h"
#include "../screenrt.h"
#include "../menu.h"
#include "../so.h"
#include "../sound.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "../xf.h"
#include "../xfphys.h"
#include "scptgame3.h"
#include "sly3.h"
#include "task3.h"
#include "trigger3.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);   // launcher.cpp
void LaunchFrom(LO* plaunch, LO* po);                                     // launcher.cpp
glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);                       // mat.cpp (0x11B7E0)
void BindLoPartsTable3(ALO* palo, int c, uint32_t vaTable);               // loaders.cpp (orig3 0x1C8970)
void EvalHermiteVector(const glm::vec4& p0, const glm::vec4& v0, const glm::vec4& p1, const glm::vec4& v1, glm::vec4* ppos,
                       glm::vec4* pvel, glm::vec4* paccel, float dt, float t, float dtScale);   // xfmath.cpp (0x117120 / orig3 0x118558)

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_DIVEPRINT") != nullptr;
    return s_f;
}
constexpr float kPi = 3.14159265f;

glm::mat4 MatWorld(ALO* palo)
{
    XF* pxf = PxfObj(palo);
    return pxf ? pxf->matWorld : glm::mat4(1.0f);
}
LO* PloOpt(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    return v.n == -1 || v.n == 0 ? nullptr : PloFindSwNearest(plo, v.n);
}
LO* PloRef(LO* plo, const CRef& r)
{
    if (r.m_tagk == TAGK_S32) return r.m_n == -1 || r.m_n == 0 ? nullptr : PloFindSwNearest(plo, r.m_n);
    return dynamic_cast<LO*>(r.m_pbasic);
}
// the rotation with its x axis at pan / tilt (orig3 0x11CAE0), the axes as columns
glm::mat3 MatPanTilt(float pan, float tilt)
{
    const glm::vec3 x(std::cos(tilt) * std::cos(pan), std::cos(tilt) * std::sin(pan), std::sin(tilt));
    const glm::vec3 y(-std::sin(pan), std::cos(pan), 0.0f);
    return glm::mat3(x, y, glm::cross(x, y));
}

// ---------------------------------------------------------------- the swimmer (0x57)
struct DIVER3
{
    SW* psw = nullptr;
    ALO* palo = nullptr;
    int state = -1;                    // +0x190
    float tState = 0.0f;               // +0x194
    int mode = 0;                      // +0x198 (EOPID 1927): 0 free, 1 a spring to the partner, 2 following it
    float stroke = 0.0f;               // +0x19C: the stroke's clip (0..1)
    glm::vec3 dirStroke{1, 0, 0};      // +0x1A0 (in the object's frame)
    int hpShown = 0;                   // +0x1B0
    float dtFlash = 2.0f;              // +0x1B4 (1932)
    float flash = 0.0f;                // +0x1B8
    int fSurface = 0;                  // +0x1BC: touching the surface object
    glm::vec3 posDive{0.0f};           // +0x1C0: where it went up
    LO* surface = nullptr;             // +0x1D0 (1924)
    float pan = 0.0f, tilt = 0.0f;     // +0x1DC / +0x1E0
    cmm::LM lmTilt{-1.5707964f, 1.5707964f};   // +0x1E4 (1912)
    float ratePan = 1.17f, rateTilt = 1.04f;   // +0x1EC / +0x1F0 (the ramps' rates now)
    float vPan = 0.0f, vTilt = 0.0f;           // +0x1F4 / +0x1F8
    cmm::LM lmRatePan{1.17f, 2.0f};            // +0x1FC (1913)
    cmm::LM lmRateTilt{1.04f, 1.3f};           // +0x204 (1914)
    float dtRampPan = 1.0f, dtRampTilt = 1.0f; // +0x20C / +0x210 (1915 / 1916)
    float deadPan = 0.5f, deadTilt = 0.5f;     // +0x214 / +0x218 (1917 / 1918)
    float energy = 100.0f;             // +0x21C
    float energyMax = 100.0f;          // +0x224
    int fTurbo = 0;                    // +0x228
    float turbo = 3.0f;                // +0x22C
    float strokeScale = 1.0f;          // +0x230 (1930)
    LO* aproxy[3]{};                   // +0x234.. (1919..1921)
    LO* launcher = nullptr;            // +0x240 (1922)
    LO* partner = nullptr;             // +0x270 (1928)
    float distPartner = 0.0f;          // +0x274 (1929)
    int stance = 0;                    // +0x278 (method 1931, 0x26CD28)
    glm::vec3 xhair{0.0f, 0.0f, 4000.0f};       // +0x250: the cross-hair (screen x / y, depth 4000)
    glm::vec3 xhairTarget{0.0f, 0.0f, 4000.0f}; // +0x260: where the stick puts it
    float energyShown = 100.0f;        // +0x220
    bool fCam = false;
    LO* hud = nullptr;                 // +0x2C0 (1923): the HUD screen object
    LO* ascptCam[2]{};                 // +0x1D4 / +0x1D8 (1925 / 1926): the camera scripts going down / up
    ASEG* aclip[13]{};                 // +0x27C.. (parts table 0x349CD0: 1986..1996, 456, 455)
    ASEGA* pasega = nullptr;           // +0x2B0
    ASEGA* pasegaStroke = nullptr;     // +0x2B4 (+0x2B8 / +0x2BC: none natively)
    float tLog = -1.0f;
};
std::unordered_map<const LO*, DIVER3> s_mpdiver;

void SetDiverState(DIVER3& d, int st);

void ApplyDiverOption(DIVER3& d, int eopid, const CRef& r)
{
    LO* plo = d.palo;
    auto lm = [&] { const glm::vec3 v = r.GetVector(); return cmm::LM{v.x, v.y}; };
    switch (eopid)
    {
    case 1912: d.lmTilt = lm(); break;
    case 1913: d.lmRatePan = lm(); break;
    case 1914: d.lmRateTilt = lm(); break;
    case 1915: d.dtRampPan = r.GAsFloat(); break;
    case 1916: d.dtRampTilt = r.GAsFloat(); break;
    case 1917: d.deadPan = r.GAsFloat(); break;
    case 1918: d.deadTilt = r.GAsFloat(); break;
    case 1919: case 1920: case 1921: d.aproxy[eopid - 1919] = PloRef(plo, r); break;
    case 1922: d.launcher = PloRef(plo, r); break;
    case 1924: d.surface = PloRef(plo, r); break;
    case 1923: d.hud = PloRef(plo, r); break;
    case 1925: case 1926: d.ascptCam[eopid - 1925] = PloRef(plo, r); break;
    case 1927: d.mode = r.RefCoerceS32().m_n; break;
    case 1928: d.partner = PloRef(plo, r); break;
    case 1929: d.distPartner = r.GAsFloat(); break;
    case 1930: d.strokeScale = r.GAsFloat(); break;
    case 1932: d.dtFlash = r.GAsFloat(); break;
    default: break;
    }
}

DIVER3* Pdiver(LO* plo)
{
    if (!plo || !g_psw || !plo->pcls || !FIsLoDerivedFrom(plo, 0x57)) return nullptr;
    auto it = s_mpdiver.find(plo);
    if (it != s_mpdiver.end() && it->second.psw == g_psw) return &it->second;
    DIVER3& d = s_mpdiver[plo];
    d = DIVER3{};
    d.psw = g_psw;
    d.palo = static_cast<ALO*>(plo);
    // the hit points 40 (0x1F43F8(p, 0x28)) unless loaded (EOPID 937), full (the world start's reset 0x26BFC0)
    int nMax = 40;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval[0];
        if (o.eopid == 937) nMax = v.n;
        if (o.eopid < 1912 || o.eopid > 1932) continue;
        CRef r;
        switch (o.eopid)
        {
        case 1919: case 1920: case 1921: d.aproxy[o.eopid - 1919] = PloOpt(plo, v); continue;
        case 1922: d.launcher = PloOpt(plo, v); continue;
        case 1924: d.surface = PloOpt(plo, v); continue;
        case 1928: d.partner = PloOpt(plo, v); continue;
        case 1923: d.hud = PloOpt(plo, v); continue;
        case 1925: case 1926: d.ascptCam[o.eopid - 1925] = PloOpt(plo, v); continue;
        case 1931: d.stance = v.n; continue;   // (the loaded method 1931: 0x26CD28)
        case 1912: case 1913: case 1914:
            r.m_tagk = TAGK_Vector;
            r.m_vector = glm::vec4(v.ag[0], v.ag[1], 0.0f, 0.0f);
            break;
        case 1927: r.SetS32(v.n); break;
        default: r.SetF32(v.ag[0]); break;
        }
        ApplyDiverOption(d, o.eopid, r);
    }
    // the post-load 0x26BF60: the 13 clips (table 0x349CD0, +0x27C..+0x2AC)
    BindLoPartsTable3(d.palo, 13, 0x349CD0);
    for (const auto& [ib, p] : d.palo->apart)
        for (int i = 0; i < 13; ++i)
            if (ib == (FN3_TAG | (0x27Cu + 4u * uint32_t(i)))) d.aclip[i] = dynamic_cast<ASEG*>(p);
    plo->SetField3(0, 0x170, static_cast<uint32_t>(nMax));
    plo->SetField3(0, 0x16C, static_cast<uint32_t>(nMax));
    d.hpShown = nMax;
    cmm::DecomposeRotateMatrixPanTilt(glm::mat3(MatWorld(d.palo)), &d.pan, &d.tilt);
    if (FPrint())
        std::printf("dive3: %d init: hud %d hp %d mode %d proxies %d %d %d launcher %d surface %d partner %d (%.0f)\n", plo->oid, d.hud ? d.hud->oid : -1, nMax, d.mode,
                    d.aproxy[0] ? d.aproxy[0]->oid : -1, d.aproxy[1] ? d.aproxy[1]->oid : -1, d.aproxy[2] ? d.aproxy[2]->oid : -1,
                    d.launcher ? d.launcher->oid : -1, d.surface ? d.surface->oid : -1, d.partner ? d.partner->oid : -1, d.distPartner);
    return &d;
}

// orig3 0x26D1E8: a stroke along the direction (the clip goes on unless past its half)
void Stroke(DIVER3& d, const glm::vec3& dir)
{
    d.dirStroke = dir;
    if (d.stroke < 0.5f) d.stroke += 1.0f;
}

// orig3 0x26CD30
void SetDiverState(DIVER3& d, int st)
{
    if (d.state == st) return;
    ALO* palo = d.palo;
    // the HUD (+0x2C0) shown (vt slot 56, its meters' parts 0x3CA / 0x3D2) on the way into the states 1..5, hidden out
    const bool fOld = !(unsigned(d.state + 1) < 2u), fNew = !(unsigned(st + 1) < 2u);
    if (fOld != fNew && d.hud)
    {
        if (fNew)
        {
            ShowScreenObj(d.hud, -1);
            StartScreenObjPart(d.hud, 0x3CA);
            StartScreenObjPart(d.hud, 0x3D2);
        }
        else HideScreenObj(d.hud);
    }
    if (d.state == 5) d.tilt = 0.0f;   // (0x240DB0: the flash's colour off)
    if (FPrint()) std::printf("dive3: %d state %d -> %d (t %.2f)\n", palo->oid, d.state, st, g_clock.t);
    d.state = st;
    d.tState = g_clock.t;
    switch (st)
    {
    case 0:
    case 2:
        SetAloVelocityTo(palo, glm::vec3(0.0f));   // vt slots 40 / 41
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        if (st == 2)
        {
            if (d.fCam) SetCmFov(&g_cm, 1.1f);   // (0x167960)
            if (d.ascptCam[1]) StartScptOfLo3(d.ascptCam[1], palo);   // 0x225EE8(+0x1D8, p): the camera up
            if (d.aclip[10]) ApplyAsegCur(d.aclip[10], palo, 0.0f, 1.0f, 9, &d.pasega);   // +0x2A4
            if (d.hud) StartScreenObjPart(d.hud, 0x3F0);
        }
        break;
    case 1:
        if (d.hud) StartScreenObjPart(d.hud, 0x3EF);
        break;
    case 3:
        if (d.hud) StartScreenObjPart(d.hud, 0x3F1);
        break;
    case 4:
        if (d.fCam) SetCmFov(&g_cm, 1.4f);
        if (d.ascptCam[0]) StartScptOfLo3(d.ascptCam[0], palo);   // 0x225EE8(+0x1D4, p): the camera down
        if (d.aclip[0]) ApplyAsegCur(d.aclip[0], palo, 0.0f, 1.0f, 9, &d.pasega);     // +0x27C
        break;
    case 5:
        RetractAsegaPp(&d.pasegaStroke);   // (0x137628 on +0x2BC / +0x2B8 / +0x2B4)
        if (d.aclip[11]) ApplyAsegCur(d.aclip[11], palo, 0.0f, 1.0f, 1, &d.pasega);   // +0x2A8: the death
        Broadcast3(0xC, palo);   // (0x1854C8(GAME, 0xC, p): the mission's death)
        break;
    default: break;
    }
    // the records' modes (0x28F068: 2 moving while 1..4, else 1) and the linear constraint (0x23EBF8: free while 1 / 4)
    const int k = st >= 1 && st <= 4 ? 2 : 1;
    if (XF* pxf = PxfPos(palo)) SetXfMode(palo, pxf, k);
    if (XF* pxf = PxfObj(palo)) SetXfMode(palo, pxf, k);
    SetAloCnstrLin(palo, st == 1 || st == 4 ? 0 : 1);
}

int NDiverStatus(LO* plo)   // orig3 0x26CB70 (vt slot 81)
{
    DIVER3* pd = Pdiver(plo);
    if (pd && pd->flash > 0.0f) return 1;
    return NSoHealth3(plo) > 0 ? 0 : 2;
}

// orig3 0x26D078: a shot from the stance's proxy through the view's centre (the cross-hair +0x250 at its rest), by the
// launcher +0x240; a projectile (0x59) gets the swimmer as its launcher
void Shoot(DIVER3& d)
{
    LO* pproxy = d.stance == 3 ? d.aproxy[1] : d.stance == 4 ? d.aproxy[2] : d.aproxy[0];
    LO* po = pproxy && g_psw ? PloAcquireProxyObject(g_psw, pproxy, false) : nullptr;
    if (!po) return;
    // 0x212FA0(shot, camera, the camera's point, the cross-hair +0x250): the segment from the eye to the cross-hair's
    // point at depth 4000 through the world, its first hit (natively the first-person camera is the object's matrix)
    const glm::mat4 m = MatWorld(d.palo);
    glm::vec3 posAim = glm::vec3(m[3]) + glm::vec3(m[0]) * 4000.0f;
    if (d.fCam) posAim = ConvertCmScreenToWorld(&g_cm, d.xhair);
    {
        const glm::vec3 a = d.fCam ? g_cm.pos : glm::vec3(m[3]);
        std::vector<SOE*> asoe;
        for (SOE* psoe : AsolidWithTree(a, posAim))
            if (psoe->palo != d.palo && !psoe->f50 && !psoe->f48) asoe.push_back(psoe);
        LSG lsg;
        if (PsoeHitTestLineObjects(0, a, posAim, asoe, &lsg)) posAim = lsg.pos0;
    }
    if (d.launcher)
    {
        SetLauncherAim(d.launcher, nullptr, posAim);
        LaunchFrom(d.launcher, po);   // 0x212770
    }
    if (FIsLoDerivedFrom(po, 0x59)) SetProjectileLauncher(static_cast<ALO*>(po), d.palo);   // 0x14D1F0
    if (FPrint()) std::printf("dive3: %d shot %d (cid 0x%X, launcher %d) toward %.0f %.0f %.0f\n", d.palo->oid, po->oid, unsigned(po->Cid()),
                              d.launcher ? d.launcher->oid : -1, posAim.x, posAim.y, posAim.z);
}

// orig3 0x26D470
void UpdateDiverInput(DIVER3& d, PADCTL* ppad)
{
    if (!ppad) return;
    auto pressed = [&](uint32_t grf) { return GrfPadctlTakePressed(ppad, grf, 0.0f) != 0; };
    switch (d.mode)
    {
    case 0:
        if (pressed(0x10)) Stroke(d, glm::vec3(1, 0, 0));
        else if (pressed(0x40)) Stroke(d, glm::vec3(-1, 0, 0));
        break;
    case 1:
    case 2:
        if (pressed(0x10)) Stroke(d, glm::vec3(0, 0, 1));
        else if (pressed(0x40)) Stroke(d, glm::vec3(0, 0, -1));
        // the cross-hair: the left stick (0x1A7B38 index 6: squared, x0.9 / x0.8, reversed by the axis table and Camera
        // Options' kind 6) moves its target at 2 a second within -0.8..0.8 once deflected 0.2; else it stays
        if (0.2f <= ppad->uJoy)
        {
            float dx = -std::fabs(ppad->x) * ppad->x * 0.9f;
            if (FCamReverse3(6, false)) dx = -dx;
            float dy = std::fabs(ppad->y) * ppad->y * 0.8f;
            if (!FCamReverse3(6, true)) dy = -dy;
            static const cmm::LM s_lm{-0.8f, 0.8f};   // D_00328B48
            d.xhairTarget.x = cmm::GLimitLm(&s_lm, d.xhairTarget.x + dx * -2.0f * g_clock.dt);
            d.xhairTarget.y = cmm::GLimitLm(&s_lm, d.xhairTarget.y + dy * -2.0f * g_clock.dt);
            d.xhairTarget.z = 4000.0f;
        }
        else d.xhairTarget = d.xhair;
        break;
    default: break;
    }
    if (pressed(0x20)) Stroke(d, glm::vec3(0, -1, 0));
    else if (pressed(0x80)) Stroke(d, glm::vec3(0, 1, 0));
    if (pressed(0x8)) Shoot(d);
    if (GrfPadctlPressedWithin(ppad, 0xF0, 0.0f))
    {
        d.fTurbo = 0;
        if ((ppad->grfbtn & 0x4) && d.energy >= 25.0f)   // L1 held
        {
            d.fTurbo = 1;
            d.energy -= 25.0f;
        }
    }
}

// orig3 0x26DAB8: the look
void UpdateDiverLook(DIVER3& d, PADCTL* ppad)
{
    const float dt = g_clock.dt;
    switch (d.mode)
    {
    case 0:
    {
        const float lx = ppad ? ppad->x : 0.0f, ly = ppad ? ppad->y : 0.0f;
        float g = std::fabs(lx) - d.deadPan;
        if (g > 0.0f) d.ratePan += g / (1.0f - d.deadPan) * (d.lmRatePan.gMax - d.lmRatePan.gMin) / d.dtRampPan * dt;
        d.ratePan = std::clamp(d.ratePan, d.lmRatePan.gMin, std::max(d.lmRatePan.gMin, d.lmRatePan.gMin + (d.lmRatePan.gMax - d.lmRatePan.gMin) * std::fabs(lx)));
        g = std::fabs(ly) - d.deadTilt;
        if (g > 0.0f) d.rateTilt += g / (1.0f - d.deadTilt) * (d.lmRateTilt.gMax - d.lmRateTilt.gMin) / d.dtRampTilt * dt;
        d.rateTilt = std::clamp(d.rateTilt, d.lmRateTilt.gMin, std::max(d.lmRateTilt.gMin, d.lmRateTilt.gMin + (d.lmRateTilt.gMax - d.lmRateTilt.gMin) * std::fabs(ly)));
        // (0x1A7B38(input, 5): the stick as the look reads it: left turns left, up looks up)
        float vx = -lx * d.ratePan, vy = ly * d.rateTilt;
        const float ax = std::fabs(vx * d.rateTilt), ay = std::fabs(vy * d.ratePan);
        const float len = std::sqrt(ax * ax + ay * ay);
        if (len > 0.0001f) { vx *= ax / len; vy *= ay / len; }
        vx = d.vPan + cmm::GLimitAbs(vx - d.vPan, dt * 12.0f);
        vy = d.vTilt + cmm::GLimitAbs(vy - d.vTilt, dt * 12.0f);
        d.pan = cmm::RadNormalize(d.pan + vx * dt);
        d.tilt = cmm::GLimitLm(&d.lmTilt, d.tilt + vy * dt);
        d.vPan = vx;
        d.vTilt = vy;
        break;
    }
    case 1:
    case 2:
        if (d.partner)
        {
            cmm::DecomposeRotateMatrixPanTilt(BuildLookAtMatrixZ(PosLoWorld(d.partner) - PosLoWorld(d.palo)), &d.pan, &d.tilt);
            d.pan = cmm::RadNormalize(d.pan);
            d.tilt = cmm::GLimitLm(&d.lmTilt, d.tilt);
        }
        break;
    default: break;
    }
}

// orig3 0x26D9F0 / 0x26D7B0 (the HUD's meters not drawn): the hit points shown, the flash, the energy's refill
void UpdateDiverMeters(DIVER3& d)
{
    const int hp = NSoHealth3(d.palo);
    if (d.hpShown != hp)
    {
        static const cmm::SMP s_smp{100.0f, 0.0f, 0.5f};   // D_00328B70
        d.hpShown = static_cast<int>(cmm::GSmooth(static_cast<float>(d.hpShown), static_cast<float>(hp), g_clock.dt, &s_smp, nullptr));
    }
    if (d.flash > 0.0f)
    {
        d.flash -= g_clock.dt;
        if (d.flash <= 0.0f) d.flash = 0.0f;
    }
    // (0x26D7B0) the meters' parts of the HUD +0x2C0: 0x3CA the hit points, 0x3D2 the energy (eased, D_00328B50
    // {200, 60, 0.5}); the cross-hair eased to its target (PosSmooth D_00328B60 {2, 0.2, 1}) and its part 0x3BC moved
    // to it on the 640 x 480 screen
    if (d.hud)
    {
        const int hpMax = NSoHealthMax3(d.palo);
        if (hpMax > 0) SetScreenObjFraction(d.hud, 0x3CA, float(d.hpShown) / float(hpMax));
        if (std::fabs(d.energyShown - d.energy) > 0.0001f)
        {
            static const cmm::SMP s_smpE{200.0f, 60.0f, 0.5f};
            d.energyShown = cmm::GSmooth(d.energyShown, d.energy, g_clock.dt, &s_smpE, nullptr);
        }
        if (d.energyMax > 0.0f) SetScreenObjFraction(d.hud, 0x3D2, d.energyShown / d.energyMax);
    }
    d.energy = std::min(d.energy + g_clock.dt * 25.0f, d.energyMax);
    if (glm::length(d.xhair - d.xhairTarget) > 0.0001f)
    {
        static const cmm::SMP s_smpX{2.0f, 0.2f, 1.0f};
        d.xhair = cmm::PosSmooth(d.xhair, d.xhairTarget, g_clock.dt, &s_smpX, nullptr);
        d.xhair.z = 4000.0f;
        if (d.hud) SetScreenObjPartMatrix(d.hud, 0x3BC, {1.0f, 0.0f, 0.0f, 1.0f, d.xhair.x * 320.0f + 320.0f, d.xhair.y * -240.0f + 240.0f});
    }
}

// orig3 0x26D228: something else that blocks within r of pos (the world's sphere query 0x25D0D8; natively the SOs'
// centres only)
bool FBlocked(const glm::vec3& pos, float r, ALO* self)
{
    if (!g_psw) return false;
    for (const auto& up : g_psw->aploAll)
    {
        LO* plo = up.get();
        if (plo == self || !plo->fInWorld || !FIsLoDerivedFrom(plo, sly3::CID_SO)) continue;
        if (glm::length(PosAloCenter(static_cast<ALO*>(plo)) - pos) < r) return true;
    }
    return false;
}

// orig3 0x26D340: at the surface: up 400 when nothing blocks there nor 10 below, then state 2 (+0x1C0: where it left)
void TrySurface(DIVER3& d)
{
    const glm::vec3 pos = glm::vec3(MatWorld(d.palo)[3]);
    const glm::vec3 posUp = pos + glm::vec3(0, 0, 400.0f);
    const float r = 50.0f;   // (+0xD4's +8: the object's radius)
    if (FBlocked(posUp, r, d.palo)) return;
    d.posDive = pos - glm::vec3(0, 0, 10.0f);
    if (FBlocked(d.posDive, r, d.palo)) return;
    SetAloPos(d.palo, posUp);   // vt slot 34
    SetDiverState(d, 2);
}

// orig3 0x26CBE8 / 0x26CC38 (vt slot 61): touching the surface object +0x1D0 (natively: within its radius plus 100, or
// above its height when it is a plane)
void UpdateSurfaceContact(DIVER3& d)
{
    d.fSurface = 0;
    if (!d.surface || !d.surface->fInWorld) return;
    const glm::vec3 pos = glm::vec3(MatWorld(d.palo)[3]);
    const glm::vec3 posS = PosLoWorld(d.surface);
    if (pos.z >= posS.z - 100.0f) d.fSurface = 1;
}

void UpdateDiver(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    DIVER3* pd = Pdiver(plo);
    if (!pd) return;
    DIVER3& d = *pd;
    // (debug: SLY3_DIVETEST=1: the swimmer given the pad, as the job's SCPT 0x13)
    if (static const bool s_fTest = std::getenv("SLY3_DIVETEST") != nullptr; s_fTest && plo->fInWorld && g_psw && PloPoCurrent(g_psw) != plo)
    {
        EnablePo(g_psw, plo, 1);
        SetPoCurrent(g_psw, plo);
    }
    if (d.pasegaStroke && FAsegaRetracted(d.pasegaStroke)) RetractAsegaPp(&d.pasegaStroke);   // (0x138AC8 / 0x137688)
    if (d.state == -1) SetDiverState(d, g_psw && PloPoCurrent(g_psw) == plo ? 1 : 0);   // (the world start's reset 0x26BFC0 / the enable)
    PADCTL* ppad = PpadctlOfSo3(plo);
    if (ppad && ppad->nControl != 1) ppad = nullptr;
    if (d.state != 0 && d.state != 5 && ppad && GrfPadctlTakePressed(ppad, 0x200, 0.0f)) PsndStartSound(0x2A, 1.0f, 0, plo);   // the horn (L3)
    UpdateSurfaceContact(d);
    switch (d.state)
    {
    case 1:
        UpdateDiverInput(d, ppad);
        UpdateDiverLook(d, ppad);
        UpdateDiverMeters(d);
        if (d.fSurface) TrySurface(d);
        break;
    case 2:
        if (ppad && GrfPadctlTakePressed(ppad, 0x10, 0.0f) && d.tilt < 0.0f)
        {
            SetDiverState(d, 3);
            Stroke(d, glm::vec3(1, 0, 0));
            if (d.aclip[1] && !d.pasegaStroke) ApplyAsegCur(d.aclip[1], d.palo, 0.0f, 1.0f, 1, &d.pasegaStroke);   // +0x280
        }
        else if (ppad && GrfPadctlTakePressed(ppad, 0x40, 0.0f) && d.tilt > 0.0f)
        {
            SetDiverState(d, 3);
            Stroke(d, glm::vec3(-1, 0, 0));
            if (d.aclip[2] && !d.pasegaStroke) ApplyAsegCur(d.aclip[2], d.palo, 0.0f, 1.0f, 1, &d.pasegaStroke);   // +0x284
        }
        UpdateDiverLook(d, ppad);
        UpdateDiverMeters(d);
        break;
    case 3:
        if (g_clock.t - d.tState > 0.5f)
        {
            SetAloPos(d.palo, d.posDive);   // vt slot 34
            SetDiverState(d, 4);
        }
        break;
    case 4:
        UpdateDiverInput(d, ppad);
        UpdateDiverLook(d, ppad);
        UpdateDiverMeters(d);
        // (the HUD's label part 0x3F1 done: 0x260758; without a HUD, a second)
        if (d.hud ? !FScreenObjBusy(d.hud, 0x3F1) && g_clock.t - d.tState > 0.1f : g_clock.t - d.tState > 1.0f) SetDiverState(d, 1);
        break;
    case 5:
        UpdateDiverMeters(d);
        break;
    default: break;
    }
    if (FPrint() && g_clock.t - d.tLog >= 1.0f)
    {
        d.tLog = g_clock.t;
        const glm::vec3 p = glm::vec3(MatWorld(d.palo)[3]);
        const glm::vec3 v = VelAloPos(d.palo);
        std::printf("dive3: %d t %.2f state %d mode %d pos %.0f %.0f %.0f vel %.0f %.0f %.0f pan %.2f tilt %.2f hp %d energy %.0f surface %d\n", plo->oid,
                    g_clock.t, d.state, d.mode, p.x, p.y, p.z, v.x, v.y, v.z, d.pan, d.tilt, NSoHealth3(plo), d.energy, d.fSurface);
    }
}

// orig3 0x26C420 (vt slot 74)
void DiverForces(ALO* palo)
{
    ApplyAloGravity(palo);   // 0x243738
    DIVER3* pd = Pdiver(palo);
    if (!pd) return;
    DIVER3& d = *pd;
    const float dt = std::max(g_clock.dt, 0.0001f);
    if (d.state == 1 || d.state == 4)
    {
        const glm::mat4 m = MatWorld(palo);
        const glm::vec3 vel = VelAloPos(palo);
        glm::vec3 dv(0.0f);
        d.stroke = std::clamp(d.stroke - dt * 2.0f, 0.0f, 1.0f);   // 0x11EE68 with {0, 1}
        if (d.stroke > 0.0f)
        {
            // 0x26C390: the stroke's clip 2250 x (1 - sin(pi t) / 2) x +0x230 (x +0x22C turbo), in the object's frame
            const float a = 1.0f - std::sin(d.stroke * kPi) * 0.5f;
            dv = glm::mat3(m) * (d.dirStroke * (a * 2250.0f * d.strokeScale * (d.fTurbo ? d.turbo : 1.0f)));
        }
        switch (d.mode)
        {
        case 2:
            if (d.partner && FIsLoDerivedFrom(d.partner, sly3::CID_ALO))
            {
                ALO* pp = static_cast<ALO*>(d.partner);
                const glm::vec3 c = glm::vec3(MatWorld(pp) * glm::vec4(-d.distPartner, 0, 0, 1));
                AccelAloPos(palo, (c - glm::vec3(m[3]) + VelAloPos(pp) + dv - vel) / dt);
            }
            break;
        case 1:
            if (d.partner)
            {
                const glm::vec3 to = PosLoWorld(d.partner) - glm::vec3(m[3]);
                const glm::vec3 imp = glm::mat3(m) * glm::vec3(glm::length(to) - d.distPartner, 0, 0);
                AccelAloPos(palo, (imp + dv - vel) / dt);
            }
            break;
        default:
        {
            AccelAloPos(palo, dv);
            const glm::vec3 fwd = glm::vec3(m[0]);
            if (d.dirStroke.x > 0.0f && glm::dot(vel, fwd) > 0.0f)   // (0x26DE50: a forward stroke)
            {
                const glm::vec3 par = fwd * glm::dot(vel, fwd);
                AccelAloPos(palo, par * -0.9375f);
                AccelAloPos(palo, (vel - par) * -1.875f);
            }
            else
                AccelAloPos(palo, vel * -1.875f);
            break;
        }
        }
    }
    // the orientation at the pan / tilt (0x11CAE0 .. 0x244890: reached within the frame; here set)
    if (d.state != 0)
    {
        SetAloRotation(palo, MatPanTilt(d.pan, d.tilt));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
    }
}

// orig3 0x26C990 (vt slot 78)
bool FAcceptDiverContact(ALO* palo, CTMSG* pctm)
{
    DIVER3* pd = Pdiver(palo);
    if (!pd || !pctm) return false;
    DIVER3& d = *pd;
    if (NDiverStatus(palo)) return false;   // slot 80 0x26CB10
    if (!(pctm->grfAccepted & 2)) return false;
    LO* pfrom = pctm->ploSender;
    if (pfrom && FIsLoDerivedFrom(pfrom, 0x59) && PloProjectileLauncher(pfrom) == palo) return false;
    if (d.state != 1 && d.state != 4) return false;
    if (pctm->nKill3 || pctm->fKill3) { SetSoHealth3(palo, 0); d.hpShown = 0; }
    else SetSoHealth3(palo, NSoHealth3(palo) - pctm->nState);
    TriggerJoyRumbleRumk(4, 0.75f);
    JoltCm(&g_cm, 0);
    if (NSoHealth3(palo) > 0) d.flash = d.dtFlash;
    else SetDiverState(d, 5);
    if (FPrint()) std::printf("dive3: %d hit by %d (%d): hp %d (t %.2f)\n", palo->oid, pfrom ? pfrom->oid : -1, pctm->nState, NSoHealth3(palo), g_clock.t);
    return true;
}
int FDiverIgnoresHit(LO* plo, int) { return NDiverStatus(plo) ? 1 : 0; }   // orig3 0x26CB10 (vt slot 80)

// orig3 0x26BFC0 (vt slot 31): the hit points full and shown, no flash, the energy full, state 0
void ResetDiver(LO* plo, LO*)
{
    DIVER3* pd = Pdiver(plo);
    if (!pd) return;
    SetSoHealth3(plo, NSoHealthMax3(plo));
    pd->hpShown = NSoHealth3(plo);
    pd->flash = 0.0f;
    pd->fTurbo = 0;
    pd->energy = pd->energyMax;
    SetDiverState(*pd, 0);
}
// orig3 0x26C040 (vt slot 32): out of the world: the clips released, state 0 (as is)
void ReleaseDiver(LO* plo)
{
    if (DIVER3* pd = Pdiver(plo)) pd->state = 0;
}
// orig3 0x26CC78 (vt slot 35): the ALO's, then the pan / tilt from the new orientation
void RotateDiver(ALO* palo, const glm::mat3& basis)
{
    SetAloRotation(palo, basis);
    if (DIVER3* pd = Pdiver(palo)) cmm::DecomposeRotateMatrixPanTilt(glm::mat3(MatWorld(palo)), &pd->pan, &pd->tilt);
}

// the first-person camera (orig3 slot 82 0x26C948 -> 0x26DE78: a focus of kind 13, vtable 0x367990; its update
// 0x26DEC8: the field of view 1.4, the near clip 10, the camera on the object's matrix)
struct CPDIVER3 : CPLCY
{
    LO* pdiver = nullptr;
    uint32_t VtOrig() const override { return 0x367990; }
    void Update(CPDEFI*, const CMJOY*) override
    {
        DIVER3* pd = Pdiver(pdiver);
        if (!pd || !pdiver->fInWorld) return;
        const glm::mat4 m = MatWorld(pd->palo);
        SetCmPosMat(&g_cm, glm::vec3(m[3]), glm::mat3(m));
    }
};
CPDIVER3 s_cpdiver;

void SyncDiverCamera(LO* plo, DIVER3& d)
{
    const bool fCur = g_psw && PloPoCurrent(g_psw) == plo && plo->fInWorld && !std::getenv("SLY3_DIVENOCAM");
    if (fCur && !d.fCam)
    {
        s_cpdiver.pdiver = plo;
        SetCmPolicy(&g_cm, 7, &s_cpdiver, plo, nullptr);
        SetCmFov(&g_cm, 1.4f);
        SetCmNearClip(&g_cm, 10.0f);
        d.fCam = true;
    }
    else if (!fCur && d.fCam)
    {
        RevokeCmPolicy(&g_cm, 0xF, 7, &s_cpdiver, plo, nullptr);
        d.fCam = false;
    }
}

void UpdateDiverAll(LO* plo)
{
    UpdateDiver(plo);
    if (DIVER3* pd = Pdiver(plo)) SyncDiverCamera(plo, *pd);
}

// ---------------------------------------------------------------- the creatures (0x7E)
// (orig3: init 0x232258, post-load 0x232360 (parts table 0x3496B8), reset 0x2323C8, update 0x232478, states 0x232A38,
// hits 0x232670; the flight legs 0x232E88 (1: turn to the target), 0x232F60 (2: the lunge), 0x233098 (3: back to the
// place it left its path), the step 0x233188 (0x233280 the position's Hermite 0x118558, 0x233360 the rotation's
// 0x118740), the search 0x232CE0, the path's speed envelope 0x233528, the swim clip 0x233668)
struct CREATURE3
{
    SW* psw = nullptr;
    int state = -2;            // +0x16C: -1 released, 0 on its path, 1 turning, 2 lunging, 3 returning, 4 dead
    int hp = 40;               // +0x170 (EOPID 1938; 0x232258: 40)
    int hpMax = 40;            // +0x174
    ALO* path = nullptr;       // +0x130 (1933): the object it rides
    ASEG* clipPath = nullptr;  // +0x134 (1934): the path's clip (played on the path object, +0x138's ASEGA)
    ASEGA* pasegaPath = nullptr;
    LO* target = nullptr;      // +0x13C (1935): what it hunts (the swimmer)
    SM* psm = nullptr;         // +0x140 (1936), its SMA +0x144 on the creature
    SMA* psma = nullptr;
    LO* region = nullptr;      // +0x148 (1937): the shape the target must be in
    LO* effect = nullptr;      // +0x14C (1953): the death's effect
    ASEG* aclip[6]{};          // +0x150..+0x164 (parts 1997..2002): bite, turn, lunge, swim (normal, fast, wounded)
    ASEGA* pasega = nullptr;   // +0x168
    float tLeg = 0.0f;         // +0x178: the leg's length
    float tLeft = 0.0f;        // +0x17C: the leg's time left; on the path, the speed envelope's time left
    glm::vec3 posGoal{0.0f}, velGoal{0.0f};   // +0x180 / +0x190
    glm::mat3 matA{1.0f}, matB{1.0f};         // +0x1A0 / +0x1D0
    glm::vec3 wGoal{0.0f};                    // +0x200
    glm::vec3 posSrc{0.0f}, velSrc{0.0f}, wSrc{0.0f};   // +0x210 / +0x220 / +0x230: where it left its path
    glm::mat3 matSrc{1.0f};                   // +0x240
    glm::vec3 posSeen{0.0f};                  // +0x270
    float ag[16] = {0.5f, 0.0f, 0.5f, 1200.0f, 0.0f, 0.5f, 0.75f, 2000.0f, 1.0f, 10.0f, 3.0f, 14.0f, 4.0f, 1.0f, 1.5f, 1200.0f};   // +0x280..+0x2BC
};
std::unordered_map<const LO*, CREATURE3> s_mpcreature;
// the floats by name (+0x280 + 4 i)
enum { kTurnMin, kTurnExtra, kLungeMin, kLungeRange, kLungeSpeed, kRetMin, kRetExtra, kRetRange, kEnvIn, kEnvHoldA, kEnvHoldB,
       kEnvTime, kEnvSpeed, kEnvUnused, kEnvWounded, kBiteRadius };

CREATURE3* Pcreature(LO* plo)
{
    if (!plo || !g_psw || !plo->pcls || !FIsLoDerivedFrom(plo, 0x7E)) return nullptr;
    auto it = s_mpcreature.find(plo);
    if (it != s_mpcreature.end() && it->second.psw == g_psw) return &it->second;
    CREATURE3& c = s_mpcreature[plo];
    c = CREATURE3{};
    c.psw = g_psw;
    auto alo = [](LO* p) -> ALO* { return p && FIsLoDerivedFrom(p, sly3::CID_ALO) ? static_cast<ALO*>(p) : nullptr; };
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval[0];
        if (o.eopid >= 1939 && o.eopid <= 1952)
        {
            // (1939..1949 -> +0x280..+0x2A8, 1950..1952 -> +0x2B0..+0x2B8)
            const int i = o.eopid <= 1949 ? o.eopid - 1939 : o.eopid - 1950 + 12;
            c.ag[i] = v.ag[0];
            continue;
        }
        switch (o.eopid)
        {
        case 1933: c.path = alo(PloOpt(plo, v)); break;
        case 1934: c.clipPath = dynamic_cast<ASEG*>(PloOpt(plo, v)); break;
        case 1935: c.target = PloOpt(plo, v); break;
        case 1936: c.psm = dynamic_cast<SM*>(PloOpt(plo, v)); break;
        case 1937: c.region = PloOpt(plo, v); break;
        case 1938: c.hp = c.hpMax = v.n; break;
        case 1953: c.effect = PloOpt(plo, v); break;
        default: break;
        }
    }
    // the post-load 0x232360: the six clips (table 0x3496B8: {0, 1997..2002, +0x150..+0x164})
    if (ALO* palo = alo(plo))
    {
        BindLoPartsTable3(palo, 6, 0x3496B8);
        for (const auto& [ib, p] : palo->apart)
            for (int i = 0; i < 6; ++i)
                if (ib == (FN3_TAG | (0x150u + 4u * uint32_t(i)))) c.aclip[i] = dynamic_cast<ASEG*>(p);
    }
    if (FPrint())
        std::printf("dive3: creature %d (%d): path %d clip %d target %d sm %d region %d effect %d clips %d %d %d %d %d %d hp %d\n", plo->oid, plo->oidInstance,
                    c.path ? c.path->oid : -1, c.clipPath ? c.clipPath->oid : -1, c.target ? c.target->oid : -1, c.psm ? c.psm->oid : -1,
                    c.region ? c.region->oid : -1, c.effect ? c.effect->oid : -1, c.aclip[0] ? c.aclip[0]->oid : -1, c.aclip[1] ? c.aclip[1]->oid : -1,
                    c.aclip[2] ? c.aclip[2]->oid : -1, c.aclip[3] ? c.aclip[3]->oid : -1, c.aclip[4] ? c.aclip[4]->oid : -1, c.aclip[5] ? c.aclip[5]->oid : -1, c.hp);
    return &c;
}

glm::mat3 MatOf(ALO* palo) { return glm::mat3(MatWorld(palo)); }

// orig3 0x233668: the swim clip: fast while the envelope runs, else normal (the wounded one when hpMax < hp, as the original)
ASEG* PclipSwim(const CREATURE3& c)
{
    if (c.hpMax < c.hp) return c.aclip[5];
    return c.tLeft > 0.0f ? c.aclip[4] : c.aclip[3];
}

// orig3 0x232A38
void SetCreatureState(LO* plo, CREATURE3& c, int st)
{
    ALO* palo = FIsLoDerivedFrom(plo, sly3::CID_ALO) ? static_cast<ALO*>(plo) : nullptr;
    if (c.state == 0 && palo)
    {
        // off its path: where it left (pos / basis / velocities, slots 36 / 38 / 39), detached keeping them (0x1C7A00(p, 0,
        // 15)), the path's clip held (0x138B68(+0x138, 0))
        const glm::mat4 m = MatWorld(palo);
        c.posSrc = glm::vec3(m[3]);
        c.matSrc = glm::mat3(m);
        c.velSrc = VelAloPos(palo);
        c.wSrc = AngVelAlo(palo);
        SetAloParent(palo, nullptr, nullptr, 15);
        if (c.pasegaPath) SetAsegaSpeed(c.pasegaPath, 0.0f);
    }
    if (FPrint() && c.state != st) std::printf("dive3: creature %d state %d -> %d (hp %d, t %.2f)\n", plo->oid, c.state, st, c.hp, g_clock.t);
    c.state = st;
    switch (st)
    {
    case -1:
        RetractAsegaPp(&c.pasega);
        break;
    case 0:
        // the path's clip at its envelope's speed, back on the path (0x1C7A00(p, path, 0) / 0x1C8FB0), the swim clip
        if (c.pasegaPath) SetAsegaSpeed(c.pasegaPath, c.tLeft > 0.0f ? c.ag[kEnvSpeed] : 1.0f);
        if (c.path && palo && palo->paloParent != c.path)
        {
            // (0x1C8FB0: back on its place on the path: the records' loaded local matrices)
            SetAloParent(palo, c.path, nullptr, 0);
            for (auto& pxf : palo->axf) pxf->mat = pxf->matLoad;
            UpdateAloXfWorlds(palo);
        }
        if (ASEG* p = PclipSwim(c); p && palo) ApplyAsegCur(p, palo, 0.0f, 1.0f, 9, &c.pasega);
        break;
    case 1:
        if (c.aclip[1] && palo) ApplyAsegCur(c.aclip[1], palo, 0.0f, 1.0f, 1, &c.pasega);
        break;
    case 2:
        if (c.aclip[2] && palo) ApplyAsegCur(c.aclip[2], palo, 0.0f, 1.0f, 9, &c.pasega);
        break;
    case 3:
        if (ASEG* p = PclipSwim(c); p && palo) ApplyAsegCur(p, palo, 0.0f, 1.0f, 9, &c.pasega);
        break;
    case 4:
        if (c.psma) SetSmaGoal(c.psma, OID(0x9F));
        if (c.effect) StartEffectByOid3(c.effect->oid, plo->oid, 1.0f, plo);   // 0x1FE728(+0x14C, 1)
        RemoveLo(g_psw, plo);   // vt slot 9
        Broadcast3(0x10, plo);  // (0x1854C8(GAME, 0x10, p))
        break;
    default: break;
    }
}

// orig3 0x232E88: the target seen: turn toward it where it is (leg 1, +0x280 + +0x284 long)
void StartTurn(LO* plo, CREATURE3& c)
{
    ALO* palo = static_cast<ALO*>(plo);
    SetCreatureState(plo, c, 1);
    c.posGoal = glm::vec3(MatWorld(palo)[3]);
    c.velGoal = glm::vec3(0.0f);
    c.wGoal = glm::vec3(0.0f);
    c.matA = c.matB = BuildLookAtMatrixZ(c.posSeen - c.posGoal);
    c.tLeg = c.tLeft = c.ag[kTurnMin] + c.ag[kTurnExtra];
    if (c.psma) SetSmaGoal(c.psma, OID(0xA4));
}

// orig3 0x232F60: the lunge at the place seen (distance / +0x28C long, at least +0x288; arriving at +0x290 x the way)
void StartLunge(LO* plo, CREATURE3& c)
{
    ALO* palo = static_cast<ALO*>(plo);
    SetCreatureState(plo, c, 2);
    c.posGoal = c.posSeen;
    const glm::vec3 d = c.posSeen - glm::vec3(MatWorld(palo)[3]);
    const float l = glm::length(d);
    c.tLeg = c.tLeft = std::max(l / c.ag[kLungeRange], c.ag[kLungeMin]);
    const glm::vec3 dir = l < 0.0001f ? glm::vec3(1, 0, 0) : d / l;
    c.velGoal = dir * c.ag[kLungeSpeed];
    c.wGoal = glm::vec3(0.0f);
    c.matA = c.matB = BuildLookAtMatrixZ(d);
    if (c.psma) SetSmaGoal(c.psma, OID(0xA4));
}

// orig3 0x233098: back to where it left its path (distance / +0x29C, at least +0x294 + +0x298)
void StartReturn(LO* plo, CREATURE3& c)
{
    ALO* palo = static_cast<ALO*>(plo);
    SetCreatureState(plo, c, 3);
    c.posGoal = c.posSrc;
    c.matB = c.matSrc;
    c.velGoal = c.velSrc;
    c.wGoal = c.wSrc;
    const glm::vec3 d = c.posSrc - glm::vec3(MatWorld(palo)[3]);
    c.tLeg = c.tLeft = std::max(glm::length(d) / c.ag[kRetRange], c.ag[kRetMin] + c.ag[kRetExtra]);
    c.matA = BuildLookAtMatrixZ(d);
    if (c.psma) SetSmaGoal(c.psma, OID(0x9F));
}

// orig3 0x233280: the position / velocity dt on along the leg's Hermite (0x118558), or the goal once past it
void LegSample(ALO* palo, const CREATURE3& c, float dt, glm::vec3* ppos, glm::vec3* pvel)
{
    if (c.tLeft <= dt)
    {
        *ppos = c.posGoal;
        *pvel = c.velGoal;
        return;
    }
    glm::vec4 p4, v4;
    EvalHermiteVector(glm::vec4(glm::vec3(MatWorld(palo)[3]), 0.0f), glm::vec4(VelAloPos(palo), 0.0f), glm::vec4(c.posGoal, 0.0f),
                      glm::vec4(c.velGoal, 0.0f), &p4, &v4, nullptr, c.tLeft, dt, 1.0f);
    *ppos = glm::vec3(p4);
    *pvel = glm::vec3(v4);
}

// orig3 0x233360: the basis dt on: the first leg's basis +0x1A0 after its in window, the end basis +0x1D0 within the out
// window (0x118740's Hermite rotation; natively a slerp by dt over the window's time left)
glm::mat3 LegBasis(ALO* palo, const CREATURE3& c, float dt, float gIn, float gOut)
{
    const float rem = c.tLeft - dt;
    const glm::quat q0 = glm::quat_cast(MatOf(palo));
    auto toward = [&](const glm::mat3& m, float tWin) {
        const float u = std::clamp(dt / std::max(tWin, dt), 0.0f, 1.0f);
        return glm::mat3_cast(glm::slerp(q0, glm::quat_cast(m), u));
    };
    if (rem <= 0.0f) return c.matB;
    if (rem < gOut) return toward(c.matB, std::max(dt, c.tLeft));
    const float el = c.tLeg - rem;
    if (el < gIn) return toward(c.matA, std::max(dt, gIn - el));
    return c.matA;
}

// orig3 0x233188: one step of the leg (slots 34 / 40 / 35 / 41), the time left down to 0
void LegStep(ALO* palo, CREATURE3& c, float gIn, float gOut)
{
    const float dt = g_clock.dt;
    glm::vec3 pos, vel;
    LegSample(palo, c, dt, &pos, &vel);
    SetAloPos(palo, pos);
    SetAloVelocityTo(palo, vel);
    SetAloRotation(palo, LegBasis(palo, c, dt, gIn, gOut));
    SetAloAngVelocityTo(palo, glm::vec3(0.0f));
    c.tLeft = std::max(c.tLeft - dt, 0.0f);
}

// orig3 0x2336B0: the target within +0x2BC
bool FTargetNear(ALO* palo, const CREATURE3& c)
{
    if (!c.target) return false;
    const glm::vec3 d = glm::vec3(MatWorld(palo)[3]) - PosLoWorld(c.target);
    return glm::dot(d, d) < c.ag[kBiteRadius] * c.ag[kBiteRadius];
}

// orig3 0x232CE0: the target inside the region (0x27F258) and nothing solid between (0x25CF18 / 0x165000): seen
void SearchTarget(LO* plo, CREATURE3& c)
{
    if (!c.target || !c.target->fInWorld || !c.region) return;
    const glm::vec3 pos = PosLoWorld(c.target);
    if (static const bool s_f = std::getenv("SLY3_DIVESEARCH") != nullptr; s_f)
    {
        static float s_t = 0.0f;
        if (g_clock.t - s_t > 2.0f || g_clock.t < s_t)
        {
            s_t = g_clock.t;
            const glm::vec3 h = glm::vec3(MatWorld(static_cast<ALO*>(plo))[3]);
            std::printf("dive3: search %d: target at %.0f %.0f %.0f in region %d (in world %d); creature at %.0f %.0f %.0f\n", plo->oid, pos.x, pos.y, pos.z,
                        int(FPointInShape(c.region, pos)), int(c.region->fInWorld), h.x, h.y, h.z);
        }
    }
    if (!FPointInShape(c.region, pos)) return;
    const glm::vec3 here = glm::vec3(MatWorld(static_cast<ALO*>(plo))[3]);
    const glm::vec3 d = pos - here;
    const float l = glm::length(d);
    if (l > 1.0f)
    {
        // (the segment stops short of the target: its own collision is not in the way)
        const glm::vec3 to = here + d * std::max(0.0f, (l - 60.0f) / l);
        float uTo = 1.0f;
        if (FFindCmClearInterval(0.0f, AsolidWithTree(glm::min(here, to), glm::max(here, to)), here, to, nullptr, &uTo) && uTo < 1.0f) return;
    }
    c.posSeen = pos;
    StartTurn(plo, c);
}

// orig3 0x233528: the path's speed envelope after a flight (in, held, out over +0x2AC; wounded: +0x2B8 at the end)
void UpdateEnvelope(ALO* palo, CREATURE3& c)
{
    if (!(c.tLeft > 0.0f)) return;
    c.tLeft -= g_clock.dt;
    const float scale = c.hpMax < c.hp ? c.ag[kEnvWounded] : 1.0f;
    float level;
    if (c.ag[kEnvHoldA] + c.ag[kEnvHoldB] < c.tLeft)
        level = 1.0f + (c.ag[kEnvSpeed] - 1.0f) * std::clamp((c.ag[kEnvTime] - c.tLeft) / std::max(c.ag[kEnvIn], 0.0001f), 0.0f, 1.0f);
    else if (c.ag[kEnvHoldB] < c.tLeft) level = c.ag[kEnvSpeed];
    else if (c.tLeft > 0.0f) level = scale + (c.ag[kEnvSpeed] - scale) * c.tLeft / std::max(c.ag[kEnvHoldB], 0.0001f);
    else
    {
        level = scale;
        if (ASEG* p = PclipSwim(c)) ApplyAsegCur(p, palo, 0.0f, 1.0f, 9, &c.pasega);   // (the normal swim again)
    }
    if (c.pasegaPath) SetAsegaSpeed(c.pasegaPath, level);
}

// orig3 0x2323C8 (vt slot 31): the path's clip applied still, the SM's handle, the envelope off, on the path
void ResetCreature(LO* plo, LO*)
{
    CREATURE3* pc = Pcreature(plo);
    if (!pc) return;
    CREATURE3& c = *pc;
    if (c.clipPath && c.path) ApplyAsegCur(c.clipPath, c.path, 0.0f, 0.0f, 8, &c.pasegaPath);
    if (c.psm && FIsLoDerivedFrom(plo, sly3::CID_ALO)) ReplaceSmaHandle(c.psm, static_cast<ALO*>(plo), OID(-1), 0, &c.psma);
    c.tLeft = 0.0f;
    c.state = -2;
    SetCreatureState(plo, c, 0);
}
// orig3 0x232440 (vt slot 32)
void ReleaseCreature(LO* plo)
{
    if (CREATURE3* pc = Pcreature(plo)) { SetCreatureState(plo, *pc, -1); RetractAsegaPp(&pc->pasegaPath); }
}
// orig3 0x232670 (vt slot 78): contact bytes 2, not while dead: the amount off; none left: dead (state 4); hit on its
// path: it turns on the one that hit it (its target's place)
bool FAcceptCreatureContact(ALO* palo, CTMSG* pctm)
{
    CREATURE3* pc = Pcreature(palo);
    if (!pc || !pctm || !(pctm->grfAccepted & 2) || pc->state == 4) return false;
    pc->hp -= pctm->nKill3 || pctm->fKill3 ? pc->hp : pctm->nState;
    if (pc->hp <= 0) SetCreatureState(palo, *pc, 4);
    else if (pc->state == 0 && pc->target)
    {
        pc->posSeen = PosLoWorld(pc->target);
        StartTurn(palo, *pc);
    }
    return true;
}
// orig3 0x232478 (vt slot 18)
void UpdateCreature(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    CREATURE3* pc = Pcreature(plo);
    if (!pc) return;
    CREATURE3& c = *pc;
    if (c.state == -2) ResetCreature(plo, nullptr);   // (the world start's slot 31)
    if (!FIsLoDerivedFrom(plo, sly3::CID_ALO)) return;
    ALO* palo = static_cast<ALO*>(plo);
    switch (c.state)
    {
    case 0:
        UpdateEnvelope(palo, c);
        SearchTarget(plo, c);
        break;
    case 1:
        LegStep(palo, c, c.ag[kTurnMin], 0.0f);
        if (c.tLeft <= 0.0f) StartLunge(plo, c);
        break;
    case 2:
        if (c.tLeft >= 0.5f)
        {
            if (c.tLeft - g_clock.dt < 0.5f)
            {
                if (c.aclip[0]) ApplyAsegCur(c.aclip[0], palo, 0.0f, 1.0f, 1, &c.pasega);   // the bite
            }
            else if (FTargetNear(palo, c))
            {
                // near the target early: the bite now and the leg cut to its last half second (its point 0.5 s on)
                if (c.aclip[0]) ApplyAsegCur(c.aclip[0], palo, 0.0f, 1.0f, 1, &c.pasega);
                glm::vec3 pos, vel;
                LegSample(palo, c, 0.5f, &pos, &vel);
                c.matA = LegBasis(palo, c, 0.5f, c.ag[kLungeMin], 0.0f);
                c.posGoal = pos, c.velGoal = vel;
                c.tLeft = 0.5f;
            }
        }
        LegStep(palo, c, c.ag[kLungeMin], 0.0f);
        if (c.tLeft <= 0.0f) StartReturn(plo, c);
        break;
    case 3:
        LegStep(palo, c, c.ag[kRetMin], c.ag[kRetExtra]);
        if (c.tLeft <= 0.0f)
        {
            c.tLeft = c.ag[kEnvTime];
            SetCreatureState(plo, c, 0);
        }
        break;
    default: break;
    }
}
struct INITDIVER3
{
    INITDIVER3()
    {
        RegisterLoUpdate(FN3_TAG | 0x26C088, UpdateDiverAll);
        RegisterLoUpdate(FN3_TAG | 0x232478, UpdateCreature);
        // the stance (method 1931: 0x110A20 -> 0x26CD28, +0x278): 0 / 1 the first shot proxy (1 with the clip +0x2A0), 3 /
        // 4 the second / third
        RegisterObjectMethod3(0x110A20, [](LO* plo, const std::vector<CRef>& a) {
            if (DIVER3* pd = Pdiver(plo)) pd->stance = NArg(a, 0);
            return CRef(TAGK_Void);
        });
        static void (*const s_apfn[21])(LO*, const CRef&) = {
#define DIVESET(E) [](LO* plo, const CRef& r) { if (DIVER3* d = Pdiver(plo)) ApplyDiverOption(*d, E, r); }
            DIVESET(1912), DIVESET(1913), DIVESET(1914), DIVESET(1915), DIVESET(1916), DIVESET(1917), DIVESET(1918),
            DIVESET(1919), DIVESET(1920), DIVESET(1921), DIVESET(1922), DIVESET(1923), DIVESET(1924), DIVESET(1925),
            DIVESET(1926), DIVESET(1927), DIVESET(1928), DIVESET(1929), DIVESET(1930), DIVESET(1931), DIVESET(1932)
#undef DIVESET
        };
        for (int e = 1912; e <= 1932; ++e)
            if (e != 1923 && e != 1925 && e != 1926 && e != 1931) RegisterObjectSetter3(e, s_apfn[e - 1912]);
        RegisterObjectGetter3(1927, [](LO* plo) { CRef r; DIVER3* d = Pdiver(plo); r.SetS32(d ? d->mode : 0); return r; });
        RegisterObjectSetter3(1938, [](LO* plo, const CRef& r) { if (CREATURE3* c = Pcreature(plo)) c->hp = r.RefCoerceS32().m_n; });
        RegisterObjectGetter3(1938, [](LO* plo) { CRef r; CREATURE3* c = Pcreature(plo); r.SetS32(c ? c->hp : 0); return r; });
    }
} s_initdiver3;
NATIVEFN3(0x26C420, DiverForces);
NATIVEFN3(0x26C990, FAcceptDiverContact);
NATIVEFN3(0x26CB10, FDiverIgnoresHit);
NATIVEFN3(0x26CB70, NDiverStatus);
NATIVEFN3(0x26BFC0, ResetDiver);
NATIVEFN3(0x26C040, ReleaseDiver);
NATIVEFN3(0x26CC78, RotateDiver);
NATIVEFN3(0x2323C8, ResetCreature);
NATIVEFN3(0x232440, ReleaseCreature);
NATIVEFN3(0x232670, FAcceptCreatureContact);
}  // namespace

// orig3 0x26C8F8 (vt slot 83, po.cpp's enable): the current player's: swimming (1), else off (0)
void OnDiverEnable3(LO* plo, int n)
{
    if (DIVER3* pd = Pdiver(plo)) SetDiverState(*pd, n == 1 ? 1 : 0);
}
void ResetDivers3()
{
    s_mpdiver.clear();
    s_mpcreature.clear();
}

// (update.cpp, the objects' post-loads) the records made and the objects updated: the inits 0x26BE28 / 0x232258 set
// the update flag (+0x3C bit 2: GRFUPD_Update); the post-loads 0x26BF60 / 0x232360 (the clips' binds: not kept, the
// rotation constraint 1 (0x23EC98))
void PostLoadDiver3(LO* plo)
{
    if (!plo || !plo->pcls || !g_psw) return;
    const bool fDiver = FIsLoDerivedFrom(plo, 0x57), fCreature = !fDiver && FIsLoDerivedFrom(plo, 0x7E);
    if (!fDiver && !fCreature) return;
    if (fDiver) Pdiver(plo);
    else Pcreature(plo);
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO)) SetAloCnstrAng(static_cast<ALO*>(plo), 1);
}
