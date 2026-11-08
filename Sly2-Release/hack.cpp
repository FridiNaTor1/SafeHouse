#include "actioninput.h"
// hack.cpp: Bentley's hacking minigame (module 0x17F090..0x182CC0): the hack objects, classes 0x47..0x4C, all
// derived from PO (cid 0x32). 0x47 is their base (hit points +0x180 / +0x184, the knock-back of a hit +0x1A0.., the
// death 0x17F5F8 / reset 0x17F6B0, vt slots 82 / 83 of its own); 0x48 is the ship Bentley drives (one per hack
// world: 17344 in world 14); 0x49..0x4C are the hack world's other objects (0x49 the enemies, 0x4A the shots /
// timed pieces, 0x4B / 0x4C the spawners and emitters).
//
// The hack starts from the job's task: Bentley uses the station (cid 0x23 use, e.g. 28060 for t4_hack_station1 in
// world 14), the station's script enables the ship (vt slot 79 -> its slot 78 0x17F8C8: state 1) far above the
// level (z 25000) and the camera follows it.
//
// Native: the ship (cid 0x48: update 0x17F930, motion 0x17FB50, focus 0x17F8C8, states 0x17FD08 with the docking
// state 3, the HUD and the engine loop, hits 0x17FBE8, post-load 0x17F850, out of the world 0x17F740), the shots /
// pieces (0x4A), the enemies (0x49: rules / turn / attack / chase / segment events 0x181060), the docks (0x4B) and the
// swarms (0x4C). Not yet: the ship's state segments (+0x1C8, method 0x1800D8: no option of the worlds seen sets
// them), the bursts at the base's death (0x15C150).
#include "activate.h"
#include "worldglobals.h"
#include "alo.h"
#include "cls.h"
#include "clock.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "smrun.h"
#include "sm.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "vecmat.h"
#include "vtnative.h"
#include "routetele.h"
#include "xf.h"
#include "frame.h"
#include "hackmouse.h"
#include "proc.h"
#include "pair.h"
#include <string>
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
#include "animfind.h"
#include "anim.h"
#include "actanim.h"
#include "aseg.h"
#include "screenrt.h"
#include "sound.h"
#include "emitter.h"
#include "render.h"
#include "activate.h"
#include "ai.h"
#include "asega.h"
#include "crv.h"
#include "nav.h"
#include "xfphys.h"
#include "worldaccess.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

glm::mat3 MatRotateAxis(float rad, const glm::vec3& axis);  // xfmath.h (0x11B500)
void AccelAloPos(ALO* p, const glm::vec3& v);                // solve.cpp (0x1FAC30)
glm::vec3 EulerFromMat(const glm::mat3& m);                   // xfmath.h (0x11BFF0)
float GRandInRange(float gLow, float gHigh);                  // xfmath.h (0x11D448)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)

namespace
{
float FloatField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t uDefault;
    std::memcpy(&uDefault, &gDefault, 4);
    const uint32_t u = plo->U32Field(0, ib, uDefault);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

// A gun of the ship (orig +0x234, 0x18 bytes, up to 4: method 0x10FD00 -> 0x1800A0)
struct HACKGUN
{
    OID oidPart = -1;          // +0x0: the record (tag oid, kind 6) the right stick turns
    OID oidSm = -1;            // +0x4: the state machine run on the ship (0x68 firing, 0x66 not)
    XF* pxf = nullptr;         // +0x8 (0x1F4C58)
    CTRLPOS* pctrl = nullptr;  // +0xC (0x12DC18: priority 0, mode 7, state 2)
    SM* psm = nullptr;         // +0x10 (0x15E938 kind 4)
    SMA* psma = nullptr;       // +0x14 (0x1F0380)
};

// A state's segment (orig 0x10 bytes: the ship's +0x1C8 x 5 (optid 369 -> 0x1800D8), the enemies' +0x220 x 5
// (optid 369 -> 0x181308), the docks' +0x1D0 x 3 (0x110140)): {oid, its ASEG (0x6D) or SM (0x6F) found from the object (0x17F408), grfapl}
struct HACKSEG
{
    OID oid = -1;
    ASEG* paseg = nullptr;
    SM* psm = nullptr;
    int grf = 0;
};

// An enemy's rule for a state (orig +0x270, 0x28 bytes x 5: methods 0x181330 / 0x181378): the distance / the angle
// ranges to enter it and to stay in it
struct HACKRULE
{
    int fDistance = 0, fAngle = 0;
    LM lmEnterDistance{0, 0}, lmStayDistance{0, 0}, lmEnterAngle{0, 0}, lmStayAngle{0, 0};
};

struct POLY3
{
    float a = 0, b = 0, c = 0;
    float Eval(float t) const { return a + t * (b + t * c); }
};

// The hack object (cid 0x47 fields +0x180..+0x1C0, the ship's 0x48 fields +0x1C0..+0x2B0)
struct HACKSO
{
    int cHpMax = 10;            // +0x180 (EOPID 1363; 0x17F090: 10)
    int cHp = 0;                // +0x184 (EOPID 1364)
    int nPoints = 0;            // +0x188 (EOPID 1365: given at the death, 0x17F5F8)
    int fDead = 0;              // +0x18C (0x17F5B0 / 0x17F110)
    LO* ploBurst = nullptr;     // +0x190 (EOPID 1366: burst at the death)
    float agKnock[4] = {300.0f, 50.0f, 0.0f, 0.0f};  // +0x1A0 (EOPID 1369; D_002BE7D0): g0 + t (g1 + t g2)
    LM lmKnock{0.0f, 800.0f};   // +0x1B0 (EOPID 1370; D_002BE7E0)
    // the ship (0x48)
    int state = -1;             // +0x1C0
    float tState = 0.0f;        // +0x1C4
    float radHeading = 0.0f;    // +0x218
    glm::vec3 vTarget{0.0f};    // +0x220
    int cgun = 0;               // +0x230
    HACKGUN agun[4];            // +0x234
    LO* ploHud = nullptr;       // +0x2A0 (EOPID 1373)
    CSndRef sndEngine;          // +0x2A4: the engine's loop (0x1FBB90 of +0x2A8)
    int idSound = -1;           // +0x2A8 (EOPID 1374)
    float uSound = 0.0f;        // +0x2AC: its volume
    bool fPostLoaded = false;
    HACKSEG aseg[5];            // the ship's +0x1C8 / the enemies' +0x220
    ASEGA* pasegaSeg = nullptr; // +0x194
    SMA* psmaSeg = nullptr;     // +0x198
    // the enemies (0x49): +0x1C8 the cool-down flag, +0x1CC busy (their segments' events 0x25..0x28), +0x1D0 the
    // nav mesh (EOPID 1378), +0x1D4 the target (EOPID 1379), +0x1D8 the path (0x1E94B0(0x2C9, 16)), +0x1DC its place,
    // +0x1E0 / +0x1F0 the last self / target, +0x200 their time, +0x204 the heading, +0x210 the push, the rules +0x270,
    // +0x338 the speed scale (EOPID 1382), +0x340 / +0x350 / +0x360 the polynomials (1383..1385), +0x370 the heading's
    // clamp (1386), +0x378 the turn's {25, 0, 0.25} x r (0x1813C0), +0x384 the push gain 5 x r (0x1813F0)
    int fCooldown = 0, fBusy = 0;
    LO* ploNav = nullptr;
    LO* ploEnemyTarget = nullptr;
    CRV crvPath;
    float sPath = 0.0f;
    glm::vec3 posLastSelf{0.0f}, posLastTarget{0.0f};
    float tPath = 0.0f;
    float radEnemyHeading = 0.0f;
    glm::vec3 vPush{0.0f};
    HACKRULE arule[5];
    float gScale = 250.0f;
    POLY3 polyRemaining{1.0f, 0, 0}, polyAngle{1.0f, 0, 0}, polySimple{0.0f, 1.0f, 0};  // (0x180500: +0x340, +0x350, +0x364 = 1)
    LM lmSimple{-FLT_MAX, FLT_MAX};
    float acTurnEnemy[3] = {25.0f, 0.0f, 0.25f};
    float gPush = 5.0f;
    int cMove = 0;  // (debug: the slot 70 calls)
    LO* ploKiller = nullptr;  // (the posted death's argument)
    LO* ploDock = nullptr;    // the ship's +0x294: its dock point
    CTRL* pctrlDockPos = nullptr;  // +0x298: the docked ship's position controller (0x12DC18)
    CTRL* pctrlDockRot = nullptr;  // +0x29C: its rotation's
    // the docks' state machines to run (+0x200 count, +0x204 x 4 {root oid, SM oid, state on 1, state on 0}: method 526)
    struct RSMG { OID oidRoot = -1, oidSm = -1, oidOn = -1, oidOff = -1; };
    std::vector<RSMG> arsmg;
    // the pieces (0x4A): +0x1C0 the end, +0x1C4 the life (EOPID 1389), +0x1C8 the shooter (method 508), +0x1CC
    // piercing (EOPID 1388), +0x1D0 the target (EOPID 1390), +0x1D4 the turn rate (EOPID 1391)
    float tEnd = FLT_MAX;
    float dtLife = 0.0f;
    LO* ploShooter = nullptr;
    int fPierce = 0;
    LO* ploTarget = nullptr;
    float radTurnRate = 0.0f;
};
std::unordered_map<const LO*, HACKSO> s_mphack;
WORLD_TABLE(s_mphack);   // (PC: moved aside while a kept world loads, worldglobals.h)
LO* s_ploShip = nullptr;  // D_002DFB80: the ship (0x17F6F8)
WORLD_VALUE(s_ploShip);   // (PC: moved aside while a kept world loads, worldglobals.h)
using PFNVTPARENT = void (*)(LO*, ALO*, XF*, int);

// orig: 0x001F68A8 (every collision entry's bit 48 (a sensor) set to f; hide.cpp has the same, file-local)
void SetAloSensors(ALO* palo, bool f)
{
    for (SOE& e : palo->asoe) e.f48 = f;
}

bool FShip(const LO* plo) { return plo && plo->Cid() == 0x48; }
bool FHackSo(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), 0x47); }

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_HACKPRINT") != nullptr;  // (debug: the ship's states and motion)
    return s_f;
}
bool FPrint2()  // (SLY2N_HACKPRINT=2: also the enemies' turn per frame)
{
    static const bool s_f = std::getenv("SLY2N_HACKPRINT") && std::atoi(std::getenv("SLY2N_HACKPRINT")) >= 2;
    return s_f;
}

// the fields the loader's options set (re-read at the post-loads: a method option (the guns, the segments, the
// rules) can make the record during the load, before the later field options are read, e.g. the ship's sound id)
// orig: 0x001813C0 (EOPID 1376's setter: the turn's acceleration D_002BE870 x r, its third term unscaled)
void SetEnemyTurnScale(HACKSO& h, float r)
{
    h.acTurnEnemy[0] = 25.0f * r;
    h.acTurnEnemy[1] = 0.0f * r;
    h.acTurnEnemy[2] = 0.25f;
}

void LoadHackFields(LO* plo, HACKSO& h)
{
    const LO* pfrom = plo->ploCloneOf ? plo->ploCloneOf : plo;
    h.cHpMax = static_cast<int>(pfrom->U32Field(0, 0x180, 10));
    h.cHp = static_cast<int>(pfrom->U32Field(0, 0x184, 0));
    h.nPoints = static_cast<int>(pfrom->U32Field(0, 0x188, 0));
    for (int i = 0; i < 4; ++i) h.agKnock[i] = FloatField(pfrom, 0x1A0 + 4 * i, 300.0f * (i == 0) + 50.0f * (i == 1));
    h.lmKnock.gMin = FloatField(pfrom, 0x1B0, 0.0f);
    h.lmKnock.gMax = FloatField(pfrom, 0x1B4, 800.0f);
    h.idSound = static_cast<int>(pfrom->U32Field(0, 0x2A8, static_cast<uint32_t>(-1)));
    if (plo->Cid() == 0x4A)
    {
        h.nPoints = static_cast<int>(pfrom->U32Field(0, 0x188, 10));  // (0x180188: 10)
        h.dtLife = FloatField(pfrom, 0x1C4, 0.0f);
        h.fPierce = static_cast<int>(pfrom->U32Field(0, 0x1CC, 0));
        h.radTurnRate = FloatField(pfrom, 0x1D4, 0.0f);
    }
    if (plo->Cid() == 0x49)
    {
        h.gScale = FloatField(pfrom, 0x338, 250.0f);
        POLY3* ap[3] = {&h.polyRemaining, &h.polyAngle, &h.polySimple};
        for (int k = 0; k < 3; ++k)
        {
            ap[k]->a = FloatField(pfrom, 0x340 + 0x10 * k, k < 2 ? 1.0f : 0.0f);
            ap[k]->b = FloatField(pfrom, 0x344 + 0x10 * k, k < 2 ? 0.0f : 1.0f);
            ap[k]->c = FloatField(pfrom, 0x348 + 0x10 * k, 0.0f);
        }
        h.lmSimple.gMin = FloatField(pfrom, 0x370, -FLT_MAX);
        h.lmSimple.gMax = FloatField(pfrom, 0x374, FLT_MAX);
        // EOPIDs 1376 / 1377 are setters (0x1813C0: +0x378 = D_002BE870 {25, 0, 0.25} x r but +0x380 unscaled;
        // 0x1813F0: +0x384 = D_002BE87C 5 x r): the loaded option's value is r, not the field (UH3: world 15's chasers
        // 27921 have 1377 = 0.5, so they accelerate at half the default)
        for (const OPT& o : pfrom->aopt)
        {
            if (o.aval.empty()) continue;
            if (o.eopid == 1376) SetEnemyTurnScale(h, o.aval[0].ag[0]);
            if (o.eopid == 1377) h.gPush = 5.0f * o.aval[0].ag[0];
        }
    }
    for (const auto& [ib, p] : static_cast<ALO*>(plo)->apart)
    {
        if (ib == 0x1D0 && plo->Cid() == 0x49) h.ploNav = p;
        if (ib == 0x1D4 && plo->Cid() == 0x49) h.ploEnemyTarget = p;
        if (ib == 0x1D0 && plo->Cid() == 0x4A) h.ploTarget = p;
        if (ib == 0x190) h.ploBurst = p;
        if (ib == 0x2A0) h.ploHud = p;
    }
}

// orig: 0x0017F090 (the base's init: 10 hit points, the knock-back's defaults, an update) / 0x0017F6F8 (the ship's:
// no state segments, state -1, the global ship D_002DFB80); the fields from the loader's options
HACKSO& Hack(LO* plo)
{
    auto it = s_mphack.find(plo);
    if (it != s_mphack.end()) return it->second;
    HACKSO& h = s_mphack[plo];
    const LO* pfrom = plo->ploCloneOf ? plo->ploCloneOf : plo;
    LoadHackFields(plo, h);
    if (auto itSrc = s_mphack.find(pfrom); pfrom != plo && itSrc != s_mphack.end())
    {
        h.cgun = itSrc->second.cgun;
        for (int i = 0; i < h.cgun; ++i)
        {
            h.agun[i].oidPart = itSrc->second.agun[i].oidPart;
            h.agun[i].oidSm = itSrc->second.agun[i].oidSm;
        }
        for (int i = 0; i < 5; ++i)
        {
            h.aseg[i].oid = itSrc->second.aseg[i].oid;
            h.aseg[i].grf = itSrc->second.aseg[i].grf;
            h.arule[i] = itSrc->second.arule[i];
        }
        h.arsmg = itSrc->second.arsmg;
    }
    return h;
}

ALO* Palo(LO* plo) { return static_cast<ALO*>(plo); }
PADCTL* Ppadctl(LO* plo)
{
    PODATA* pd = PpodataFromLo(plo);
    return pd ? pd->ppadctl : nullptr;
}

// orig: 0x00180100 (the HUD bar: 1 - hit points / maximum, into [0, 1], on +0x2A0's part 0x34F (0x212E40))
void UpdateHackHud(LO*, HACKSO& h)
{
    if (!h.ploHud) return;
    const int n = h.cHp <= 0 ? 0 : h.cHp;
    float g = 1.0f - static_cast<float>(n) / static_cast<float>(h.cHpMax);
    if (g < 0.0f) g = 0.0f;
    else if (g > 1.0f) g = 1.0f;
    SetScreenObjFraction(h.ploHud, 0x34F, g);
}

struct HACKSEG;
void PlayHackSeg(LO* plo, HACKSO& h, const HACKSEG* pe);

// orig: 0x0017FD08 (the ship's state: -1 out, 0 idle, 1 driven, 2 hit, 3 docked, 4 destroyed)
void SetShipState(LO* plo, int n)
{
    HACKSO& h = Hack(plo);
    const int old = h.state;
    if (n == old) return;
    switch (old)
    {
    case -1:
        // the HUD (+0x2A0) shown (vt slot 49 (-1)) on its part 0x34F (0x213B00), its bar; the engine's loop +0x2A8 made
        // (0x1FBB90, no source 0x1FB2F8) and paused (0x1FB018) until the stick moves
        if (h.ploHud)
        {
            ShowScreenObj(h.ploHud, -1);
            SelectScreenObjPart(h.ploHud, 0x34F);
            UpdateHackHud(plo, h);
            if (h.idSound != old)
            {
                CSnd* psnd = PsndNewPlain(h.idSound);
                psnd->SetSource(nullptr);
                h.sndEngine = CSndRef(psnd);  // 0x258040
                h.sndEngine->Pause();
            }
        }
        break;
    case 3:
    {
        // the docking's controllers removed (0x12E460), the records back in mode 2 (0x2303A0), out of the dock's parent
        // keeping its place and velocities (0x1A58D0(p, 0, 0xF): vt slot 10), the constraints 5 / 4 (0x1F5720 /
        // 0x1F57C0), no sensors (0x1F68A8(p, 0)), no dock
        ALO* palo = Palo(plo);
        DeleteCtrl(&h.pctrlDockPos);
        DeleteCtrl(&h.pctrlDockRot);
        if (XF* p58 = PxfPos(palo)) SetXfMode(palo, p58, 2);
        if (XF* p5C = PxfRot(palo)) SetXfMode(palo, p5C, 2);
        SetAloParent(palo, nullptr, nullptr, 0xF);
        SetAloCnstrLin(palo, 5);
        SetAloCnstrAng(palo, 4);
        SetAloSensors(palo, false);
        h.ploDock = nullptr;
        break;
    }
    case 1:
        for (int i = 0; i < h.cgun; ++i)
            if (h.agun[i].psma) SetSmaGoal(h.agun[i].psma, 0x66);  // 0x1F0660
        break;
    default: break;
    }
    h.state = n;
    h.tState = g_clock.t;
    switch (n)
    {
    case -1:
        // the HUD hidden (vt slot 50), the engine's loop stopped (0x1FB098) and let go (0x258040(.., 0))
        if (h.ploHud) HideScreenObj(h.ploHud);
        if (h.sndEngine)
        {
            h.sndEngine->Stop(0.0f);
            h.sndEngine = CSndRef();
        }
        break;
    case 3:
        if (LO* pdock = h.ploDock)
        {
            // the dock point's vt slot 32: its record and its matrix in it (an ALO: its object record and the
            // identity; a LOX: its record and its own matrix)
            ALO* palo = Palo(plo);
            XF* pxfDock = nullptr;
            glm::mat4 m(1.0f);
            if (FCidDerivedFrom(pdock->Cid(), 0x30)) pxfDock = PxfObj(static_cast<ALO*>(pdock));
            else if (FCidDerivedFrom(pdock->Cid(), 3))
            {
                pxfDock = static_cast<LOX*>(pdock)->pxf;
                m = static_cast<LOX*>(pdock)->mat;
            }
            // 0x22FF40(+0x54's reference, +0x58's reference, m): from the first record's frame into the position
            // record's
            XF* pxfFrom = palo->axf.empty() ? nullptr : palo->axf[0]->pxfRef;
            XF* pxfTo = PxfPos(palo) ? PxfPos(palo)->pxfRef : nullptr;
            if (pxfFrom) m = pxfFrom->matWorld * m;
            if (pxfTo) m = glm::inverse(pxfTo->matWorld) * m;
            // under the dock point's parent, in its record, keeping its world place (vt slot 10, grf 3)
            ALO* paloParent = pdock->paloParent;
            if (PFNVTPARENT pfn = PfnVt<PFNVTPARENT>(plo, 10)) pfn(plo, paloParent, pxfDock, 3);
            else SetAloParent(palo, paloParent, pxfDock, 3);
            SetAloVelocityTo(palo, glm::vec3(0.0f));     // vt slot 35 (D_002A2DD0)
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));  // vt slot 36
            // held there: controllers (priority 1, mode 7, state 2) on the position (m's) and the rotation (its Euler
            // angles, 0x11BFF0)
            if (XF* p58 = PxfPos(palo)) h.pctrlDockPos = PctrlNewPos(palo, p58, 1, 7, 2, glm::vec4(glm::vec3(m[3]), 0.0f));
            if (XF* p5C = PxfRot(palo)) h.pctrlDockRot = PctrlNewPos(palo, p5C, 1, 7, 2, glm::vec4(EulerFromMat(glm::mat3(m)), 0.0f));
            SetAloCnstrLin(palo, 1);  // 0x1F5720
            SetAloCnstrAng(palo, 1);  // 0x1F57C0
            SetAloSensors(palo, true);  // 0x1F68A8
        }
        break;
    case 4: BroadcastTaskEvent(g_psw, 0x10, plo); break;  // 0x1619A0(D_002DEB30, 0x10, p)
    default: break;
    }
    PlayHackSeg(plo, h, n == -1 ? nullptr : &h.aseg[n]);  // 0x17F4C0
    if (FPrint()) std::printf("hack ship %d: state %d -> %d at t %.3f\n", plo->oid, old, n, g_clock.t);
    if (FPrint() && n == 1 && old == 0)
        for (LO* p : g_psw->mpcidaplo[0x4B])
            if (p->fInWorld)
            {
                const HACKSO& hd = Hack(p);
                const glm::vec3 v = hd.ploEnemyTarget ? PosLoWorld(hd.ploEnemyTarget) : glm::vec3(0.0f);
                std::printf("hack ship %d: dock %d in the world, state %d, point %.0f %.0f (ship at %.0f %.0f)\n", plo->oid, p->oid, hd.state,
                            v.x, v.y, PosAloCenter(Palo(plo)).x, PosAloCenter(Palo(plo)).y);
            }
}

// orig: 0x0017F550 (the state's segment done: the ASEGA retracted, else the SMA at its goal, else true)
bool FShipSegmentDone(LO* plo)
{
    HACKSO& h = Hack(plo);
    if (h.pasegaSeg) return FAsegaRetracted(h.pasegaSeg);
    if (h.psmaSeg) return OidSmaGoal(h.psmaSeg) == OidSmaCur(h.psmaSeg);
    return true;
}

// orig: 0x0017F408 (the segments' objects found from the object: an ASEG or an SM)
void BindHackSegs(LO* plo, HACKSO& h)
{
    for (HACKSEG& e : h.aseg)
    {
        if (e.oid == -1) continue;
        LO* p = PloFindSwNearest(plo, e.oid);  // 0x15E938(G, 4, oid, p)
        if (!p) continue;
        if (FCidDerivedFrom(p->Cid(), 0x6D)) e.paseg = static_cast<ASEG*>(p);
        else if (FCidDerivedFrom(p->Cid(), 0x6F)) e.psm = dynamic_cast<SM*>(p);
    }
}

// orig: 0x0017F4C0 (the state's segment: the old SMA / ASEGA released, the new ASEG applied (0x1305B8 at 0, speed 1)
// and the SM run (0x1F1448))
void PlayHackSeg(LO* plo, HACKSO& h, const HACKSEG* pe)
{
    ClearSmaHandle(&h.psmaSeg);   // 0x1F15B8
    RetractAsegaPp(&h.pasegaSeg); // 0x130658
    if (!pe) return;
    if (pe->paseg) ApplyAsegCur(pe->paseg, Palo(plo), 0.0f, 1.0f, pe->grf, &h.pasegaSeg);
    if (pe->psm) ReplaceSmaHandle(pe->psm, Palo(plo), -1, pe->grf, &h.psmaSeg);
}

// orig: 0x0017F6B0 (vt slot 83, the base's: the hit points to the maximum; when the death is posted (+0x18C, kept),
// 0x149B58(grf 1) removes the object's pending calls: a reset in the frame of the death keeps the object; audit H5)
void ResetHackSoBase(LO* plo, HACKSO& h)
{
    h.cHp = h.cHpMax;
    if (h.fDead) CancelDeferredCalls(plo);
}

// orig: 0x0017FCB0 (the ship's vt slot 83: the base's, then state 1 when it has the pad, else 0)
void ResetShip(LO* plo)
{
    HACKSO& h = Hack(plo);
    ResetHackSoBase(plo, h);
    SetShipState(plo, NPoEnabled(plo) == 1 ? 1 : 0);  // 0x1C40F8
    UpdateHackHud(plo, h);
}

// orig: 0x0017F770 (the guns: the record of tag oidPart (kind 6) with a fixed-value controller, the SM run on the ship)
void BindShipGuns(LO* plo, HACKSO& h)
{
    ALO* palo = Palo(plo);
    for (int i = 0; i < h.cgun; ++i)
    {
        HACKGUN& g = h.agun[i];
        g.pxf = nullptr;
        for (const auto& up : palo->axf)  // 0x1F4C58(p, oid, 6)
            if (up && up->tag == g.oidPart && up->kind == 6) { g.pxf = up.get(); break; }
        LO* plosm = PloFindSwNearest(plo, g.oidSm);  // 0x15E938(G, 4, oid, p)
        g.psm = plosm ? dynamic_cast<SM*>(plosm) : nullptr;
        if (g.pxf) g.pctrl = PctrlNewPos(palo, g.pxf, 0, 7, 2, glm::vec4(0.0f));  // 0x12DC18(p, xf, 0, 7, 2, 0)
        if (g.psm) g.psma = PsmaApplySm(g.psm, palo, -1, 0);                       // 0x1F0380(sm, p, -1, 0)
    }
}

// orig: 0x0017F930 (vt slot 16)
void UpdateShip(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's
    HACKSO& h = Hack(plo);
    if (static const bool s_fShots = std::getenv("SLY2N_HACKSHOTS") != nullptr; s_fShots && g_psw)  // (debug: live shots)
    {
        int c = 0, cShip = 0;
        for (const auto& up : g_psw->aploAll)
            if (up->Cid() == 0x4A && up->fInWorld)
            {
                ++c;
                auto it = s_mphack.find(up.get());
                if (it != s_mphack.end() && it->second.ploShooter == plo) ++cShip;
            }
        std::printf("hack shots t %.3f live %d ship %d\n", g_clock.t, c, cShip);
    }
    int s;
    do
    {
        s = h.state;
        if (s == 2 && FShipSegmentDone(plo)) SetShipState(plo, 1);
    } while (h.state != s);
    switch (h.state)
    {
    case 0:
    case 2:
    case 3:
    case 4: h.vTarget = glm::vec3(0.0f); break;
    case 1:
    {
        const PADCTL* ppc = Ppadctl(plo);
        if (!ppc) break;
        h.radHeading = ppc->radJoy;
        h.vTarget = cmm::SetVectorCylind(ppc->radJoy, ppc->uJoy * 1000.0f, 0.0f);  // 0x11E1A0 (D_002BE7E8)
        // the guns: the right stick's heading, firing while it is deflected (QoL, hackmouse.h: with the keyboard /
        // mouse the cursor's heading, firing while left click is held; the stick (camera keys) still fires unclicked)
        float radAim = ppc->radJoy2;
        bool fFire = sly2actions::Applies(ppc) ? sly2actions::Held(sly2actions::Action::HackFire) : 0.0f < ppc->uJoy2;
        float radMouse = 0.0f;
        bool fMouse = false;
        if (FHackMouseAim(plo, &radMouse, &fMouse) && (fMouse || !fFire))
        {
            radAim = radMouse;
            fFire = fMouse;
        }
        const glm::vec4 rot(0.0f, 0.0f, radAim, 0.0f);
        for (int i = 0; i < h.cgun; ++i)
        {
            HACKGUN& g = h.agun[i];
            if (g.pctrl) SetCtrlPos(g.pctrl, rot);                       // 0x12D108
            if (g.psma) SetSmaGoal(g.psma, fFire ? 0x68 : 0x66);           // 0x1F0660
        }
        break;
    }
    default: break;
    }
    // (debug: SLY2N_HACKTELE="X Y@T,...": the ship put at (X, Y) once the clock passes T, stopped)
    if (static const char* s_szTele = std::getenv("SLY2N_HACKTELE"); s_szTele && h.state >= 0)
    {
        static std::vector<std::pair<glm::vec2, float>> s_atele;
        static bool s_fParsed = false;
        if (!s_fParsed)
        {
            s_fParsed = true;
            for (const char* pch = s_szTele; *pch;)
            {
                float x, y, t;
                if (std::sscanf(pch, "%f %f@%f", &x, &y, &t) == 3) s_atele.push_back({glm::vec2(x, y), t});
                const char* pchNext = std::strchr(pch, ',');
                if (!pchNext) break;
                pch = pchNext + 1;
            }
        }
        for (auto& [v, t] : s_atele)
            if (t <= g_clock.t)
            {
                ALO* palo = Palo(plo);
                SetAloPos(palo, glm::vec3(v.x, v.y, PosAloCenter(palo).z));
                SetAloVelocityTo(palo, glm::vec3(0.0f));
                std::printf("hack ship %d: teleported to %.0f %.0f at t %.3f\n", plo->oid, v.x, v.y, g_clock.t);
                t = FLT_MAX;
            }
    }
    // (debug: a route's "hackdock X Y" step: the same once the ship is driven)
    if (g_routehackdock.fPending && h.state >= 1)
    {
        g_routehackdock.fPending = false;
        ALO* palo = Palo(plo);
        SetAloPos(palo, glm::vec3(g_routehackdock.x, g_routehackdock.y, PosAloCenter(palo).z));
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        std::printf("hack ship %d: docked by the route at %.0f %.0f at t %.3f\n", plo->oid, g_routehackdock.x, g_routehackdock.y, g_clock.t);
    }
    // (debug: SLY2N_HACKHP="N@T": the ship's hit points set to N once the clock passes T)
    if (static const char* s_szHp = std::getenv("SLY2N_HACKHP"); s_szHp && h.state >= 0)
        if (static float s_t = std::strchr(s_szHp, '@') ? static_cast<float>(std::atof(std::strchr(s_szHp, '@') + 1)) : 0.0f; s_t <= g_clock.t)
        {
            h.cHp = std::atoi(s_szHp);
            s_t = FLT_MAX;
        }
    // (debug: SLY2N_HACKMAP=file: once driven, the in-world collision edges near the ship's height (x0 y0 x1 y1 oid
    // per line) and the hack objects (o oid cid x y))
    if (static const char* s_szMap = std::getenv("SLY2N_HACKMAP"); s_szMap && h.state == 1)
        if (static bool s_fDone = false; !s_fDone)
        {
            s_fDone = true;
            if (FILE* pf = std::fopen(s_szMap, "w"))
            {
                const float z = PosAloCenter(Palo(plo)).z;
                for (const auto& up : g_psw->aploAll)
                {
                    LO* p = up.get();
                    if (!p->fInWorld || !FCidDerivedFrom(p->Cid(), 0x30)) continue;
                    if (FCidDerivedFrom(p->Cid(), 0x47))
                    {
                        const glm::vec3 v = PosAloCenter(Palo(p));
                        std::fprintf(pf, "o %d %x %.1f %.1f\n", p->oid, p->Cid(), v.x, v.y);
                    }
                    for (const SOE& e : Palo(p)->asoe)
                        for (const EDGE& ed : e.geomWorld.aedge)
                        {
                            if (ed.ipos0 >= e.geomWorld.apos.size() || ed.ipos1 >= e.geomWorld.apos.size()) continue;
                            const glm::vec3 a = e.geomWorld.apos[ed.ipos0], b = e.geomWorld.apos[ed.ipos1];
                            if (std::fabs(a.z - z) > 3000.0f && std::fabs(b.z - z) > 3000.0f) continue;
                            std::fprintf(pf, "%.1f %.1f %.1f %.1f %d\n", a.x, a.y, b.x, b.y, p->oid);
                        }
                }
                std::fclose(pf);
            }
        }
    // the engine's loop: its volume smoothed toward the stick's deflection while driven (0x11D018 D_002BE7F0 {1, 0,
    // 0.5}), set (0x1FB1F8); heard while above 0 (0x1FB060 resume / 0x1FB018 pause)
    if (h.sndEngine)
    {
        const PADCTL* ppc = Ppadctl(plo);
        const float g = h.state == 1 && ppc ? ppc->uJoy : 0.0f;
        static const cmm::SMP s_smp{1.0f, 0.0f, 0.5f};
        h.uSound = cmm::GSmooth(h.uSound, g, g_clock.dt, &s_smp, nullptr);
        h.sndEngine->SetVolume(h.uSound);
        if (0.0f < h.uSound)
        {
            if (h.sndEngine->m_cpause > 0) h.sndEngine->Resume();
        }
        else if (h.sndEngine->m_cpause <= 0) h.sndEngine->Pause();
    }
    if (FPrint())
    {
        const glm::vec3 pos = PosAloCenter(Palo(plo));
        XF* p58 = PxfPos(Palo(plo));
        XF* p5C = PxfRot(Palo(plo));
        std::printf("hack ship %d t %.3f: state %d hp %d heading %.3f v %.2f %.2f %.2f pos %.2f %.2f %.2f (sost %d modes %d %d moves %d) snd %.3f\n",
                    plo->oid, g_clock.t, h.state, h.cHp, h.radHeading, h.vTarget.x, h.vTarget.y, h.vTarget.z, pos.x, pos.y,
                    pos.z, Psost(Palo(plo)) != nullptr, p58 ? p58->mode : -9, p5C ? p5C->mode : -9, h.cMove, h.uSound);
    }
}

// orig: 0x0017FB50 (vt slot 70: not docked: turned toward the heading (D_002BE800 {25, 0, 0.25}) and accelerated
// toward the target velocity (D_002BE810 {0, 5, 0}), no gravity)
void MoveShip(ALO* palo)
{
    HACKSO& h = Hack(palo);
    ++h.cMove;
    if (h.state == 3) return;
    static const float s_acTurn[3] = {25.0f, 0.0f, 0.25f};
    BrakeAloRotation(palo, MatRotateAxis(h.radHeading, glm::vec3(0.0f, 0.0f, 1.0f)), s_acTurn);  // 0x11B500 / 0x1F59A0
    static const CLQ s_clqV{0.0f, 5.0f, 0.0f, 0.0f};
    AccelAloTowardPosition(palo, glm::vec3(0.0f), nullptr, h.vTarget, &s_clqV);  // 0x1F5E10
}

// orig: 0x0017F5F8 (vt slot 82: dead: the points given to the killer (0x1F7598: {p, 0, killer, filter 0x100, -points,
// the position, the velocity}), the burst +0x190 (0x15C150), removed (vt slot 9))
void KillHackSo(LO* plo, LO* ploKiller)
{
    HACKSO& h = Hack(plo);
    if (FPrint()) std::printf("hack %d (cid 0x%X): killed by %d (points %d) at t %.3f\n", plo->oid, plo->Cid(),
                              ploKiller ? ploKiller->oid : -1, h.nPoints, g_clock.t);
    if (ploKiller && h.nPoints)
    {
        CTMSG m;
        m.ploSender = plo;
        m.ploReceiver = ploKiller;
        m.pos = PosAloCenter(Palo(plo));
        m.vec = VelAloPos(Palo(plo));  // vt slot 33
        m.grfFilter = 0x100;
        m.nState = -h.nPoints;
        PaloAcceptContact(&m);
    }
    if (h.ploBurst) ExplodeLo(h.ploBurst, nullptr);  // 0x15C150 (slot 51 with empty parameters; audit 2026-11 H4)
    RemoveLo(g_psw, plo);  // vt slot 9
}

void KillSwarm(LO* plo, LO* ploOther);  // (below: the swarms' vt slot 82)

// orig: 0x0017F5B0 (dead once: the death (vt slot 82) posted with the killer, 0x149A50 with D_002A5630)
void PostKillHackSo(LO* plo, LO* ploKiller)
{
    HACKSO& h = Hack(plo);
    if (h.fDead) return;
    h.fDead = 1;
    h.ploKiller = ploKiller;
    PostDeferredCall(plo, 0x17F5F8, [](void* object, int) {
        LO* p = static_cast<LO*>(object);
        if (p->Cid() == 0x4C) KillSwarm(p, Hack(p).ploKiller);  // (vt slot 82: 0x1829D8)
        else KillHackSo(p, Hack(p).ploKiller);
    });
}

// orig: 0x0017F1D0 (a hit: contact bytes 8: the hit points by the message's +0x10; none left: dead (vt slot 82);
// else knocked away from the contact's point in the plane (the speed g0 + t (g1 + t g2), t = -damage, in
// [gMin, gMax]), the velocity clamped to gMax and constrained (0x1FA810) unless the linear constraint is fixed)
bool FHackSoHit(LO* plo, CTMSG* pctm)
{
    if (!(pctm->grfAccepted & 8)) return false;
    HACKSO& h = Hack(plo);
    h.cHp += pctm->nState;
    if (FPrint()) std::printf("hack %d: hit %d -> hp %d at t %.3f\n", plo->oid, pctm->nState, h.cHp, g_clock.t);
    if (h.cHp <= 0)
    {
        KillHackSo(plo, nullptr);  // vt slot 82 with no killer: no points message (audit 2026-11 H3)
        return true;
    }
    ALO* palo = Palo(plo);
    SORT& s = Sort(palo);
    if (!s.psost || s.psost->cnstrLin.kind == 3) return true;
    glm::vec3 d = PosAloCenter(palo) - pctm->pos;
    d -= glm::vec3(0.0f, 0.0f, 1.0f) * d.z;  // the plane normal to D_002A2DC0 (z)
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const glm::vec3 dir = len < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : d / len;  // (else D_002A2DA0)
    const float t = static_cast<float>(-pctm->nState);
    const float g = std::clamp(h.agKnock[0] + t * (h.agKnock[1] + t * h.agKnock[2]), h.lmKnock.gMin, h.lmKnock.gMax);
    glm::vec3 v = VelAloPos(palo) + dir * g;  // vt slot 33
    const float sv = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (sv > h.lmKnock.gMax && sv > 0.0f) v *= h.lmKnock.gMax / sv;  // 0x11E9F8
    // (0x1FA810: the velocity in the linear constraint's plane / axis)
    const CNSTRS& c = s.psost->cnstrLin;
    if (c.kind == 1) v -= c.normal * (v.x * c.normal.x + v.y * c.normal.y + v.z * c.normal.z);
    else if (c.kind == 2) v = c.normal * (v.x * c.normal.x + v.y * c.normal.y + v.z * c.normal.z);
    SetAloVelocityTo(palo, v);  // vt slot 35
    return true;
}

// orig: 0x0017FBE8 (vt slot 75 of the ship: not while hit / docked / destroyed, nor paused; a hit: state 2 (the
// task event 0xF with the message) or, none left, 4)
bool FAcceptShipContact(ALO* palo, CTMSG* pctm)
{
    HACKSO& h = Hack(palo);
    if (h.state < 5 && h.state >= 2) return false;
    if (0 < g_gamepause.c78) return false;  // 0x20F0B0
    // (D_002E1E00 & 1: the debug invulnerability; natively SLY2N_HACKGOD=1, the reference's poke 2E1E00=1)
    if (static const bool s_fGod = std::getenv("SLY2N_HACKGOD") != nullptr; s_fGod) return false;
    if (!FHackSoHit(palo, pctm)) return false;
    UpdateHackHud(palo, h);
    if (h.cHp > 0)
    {
        BroadcastTaskEvent(g_psw, 0xF, palo);  // 0x1619A0(D_002DEB30, 0xF, message)
        SetShipState(palo, 2);
    }
    else SetShipState(palo, 4);
    return true;
}

// orig: 0x0017F1A0 (vt slot 55 of the hack objects: a new contact: no friction, no elasticity)
void OnHackXpNew(ALO*, XP* pxp, int) { pxp->mu = 0.0f; pxp->elas = 0.0f; }

// orig: 0x001803E0 (vt slot 55 of the pieces: then, against a hack object or when not piercing, dead (posted with
// the other end's object); piercing: the elasticity 1)
void OnPieceXpNew(ALO* palo, XP* pxp, int i)
{
    OnHackXpNew(palo, pxp, i);
    HACKSO& h = Hack(palo);
    ALO* pOther = pxp->axpd[1 - i].palo;
    if ((pOther && FCidDerivedFrom(pOther->Cid(), 0x47)) || !h.fPierce) PostKillHackSo(palo, pOther);
    if (h.fPierce) pxp->elas = 1.0f;
}

// orig: 0x001803A8 (vt slot 61 of the pieces: through the shooter's attachment (3), else the ALO's)
int OverlapKindPiece(ALO* palo, ALO* pOther)
{
    HACKSO& h = Hack(palo);
    if (h.ploShooter && FCidDerivedFrom(h.ploShooter->Cid(), 0x30) && Sort(pOther).poxa &&
        Sort(pOther).poxa == Sort(static_cast<ALO*>(h.ploShooter)).poxa)
        return 3;
    using PFN = int (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8))) return pfn(palo, pOther);
    return 0;
}

// orig: 0x001801B8 (vt slot 16 of the pieces: the ALO's; its time out: dead (vt slot 82); turned toward its target
// by the turn rate (the velocity and the orientation))
void UpdatePiece(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    HACKSO& h = Hack(plo);
    if (!h.fDead && h.tEnd <= g_clock.t)
    {
        KillHackSo(plo, nullptr);
        return;
    }
    if (!h.ploTarget || h.radTurnRate == 0.0f || !FCidDerivedFrom(h.ploTarget->Cid(), 0x30)) return;
    ALO* palo = Palo(plo);
    const glm::vec3 d = PosAloCenter(static_cast<ALO*>(h.ploTarget)) - PosAloCenter(palo);
    const float radWant = std::atan2(d.y, d.x);  // 0x2458F0
    const glm::vec3 v = VelAloPos(palo);           // vt slot 33
    const float radCur = std::atan2(v.y, v.x);
    float dr = radWant - radCur;                    // 0x11CF60: into (-pi, pi]
    dr = std::fmod(dr + 3.14159265f, 6.28318531f);
    if (dr < 0.0f) dr += 6.28318531f;
    dr -= 3.14159265f;
    const float lim = h.radTurnRate * g_clock.dt;   // 0x11CFE0: clamped to +-lim
    const float rad = std::clamp(dr, -lim, lim);
    const glm::mat3 r = MatRotateAxis(rad, glm::vec3(0.0f, 0.0f, 1.0f));
    SetAloVelocityTo(palo, r * v);  // vt slot 35
    // (the original then multiplies an uninitialised local matrix by r for its slot 30: not reproduced)
}

// ---- The enemies (cid 0x49)

// orig: 0x00181268 (the enemy's state: -1 out, 0 idle, 1 aim, 2 attack, 3 chase, 4 hit / dying; from -1 the hit points
// refilled; the push cleared except when chasing; the state's segment)
void SetEnemyState(LO* plo, int n)
{
    HACKSO& h = Hack(plo);
    if (h.state == n) return;
    if (h.state == -1) h.cHp = h.cHpMax;
    if (FPrint()) std::printf("hack enemy %d: state %d -> %d at t %.3f\n", plo->oid, h.state, n, g_clock.t);
    h.state = n;
    h.tState = g_clock.t;
    if (n != 3) h.vPush = glm::vec3(0.0f);  // (+0x210's first 3 words)
    PlayHackSeg(plo, h, n == -1 ? nullptr : &h.aseg[n]);
}

// orig: 0x0017F170 (vt slot 26 of the hack objects, a pool row taken again: after the base 0x1CB518 that AttachProxy
// runs, the hit points to their maximum) / 0x00180638 (the enemies': then state 0). Natively a respawned enemy kept
// the hit points (<= 0) of its death, so any hit killed it (N09)
void ResetHackSoSlot26(LO* plo, LO*)
{
    if (!FHackSo(plo)) return;
    HACKSO& h = Hack(plo);
    h.cHp = h.cHpMax;
    if (FPrint()) std::printf("hack %d: reused, hp %d at t %.3f\n", plo->oid, h.cHp, g_clock.t);
}
NATIVEFN(0x17F170, ResetHackSoSlot26);
void ResetEnemySlot26(LO* plo, LO* pproxy)
{
    ResetHackSoSlot26(plo, pproxy);
    if (FHackSo(plo)) SetEnemyState(plo, 0);
}
NATIVEFN(0x180638, ResetEnemySlot26);

LM LmArg(const std::vector<CRef>& a, size_t i)  // (an LM argument: the CRef's vector x / y)
{
    if (i >= a.size()) return LM{0.0f, 0.0f};
    const CRef& r = a[i];
    if (r.m_tagk == TAGK_Lm || r.m_tagk == TAGK_Vector) return LM{r.m_vector.x, r.m_vector.y};
    if (std::getenv("SLY2N_HACKPRINT")) std::printf("hack: an LM argument of tag %d\n", static_cast<int>(r.m_tagk));
    return LM{0.0f, 0.0f};
}

bool FInLm(const LM& lm, float g) { return lm.gMin <= g && g <= lm.gMax; }  // 0x11D910
float RadNormalize(float rad)  // 0x11CF60 (into [-pi, pi))
{
    rad = std::fmod(rad + 3.14159265f, 6.28318531f);
    if (rad < 0.0f) rad += 6.28318531f;
    return rad - 3.14159265f;
}
glm::vec3 VecToObject(ALO* palo, const glm::vec3& v)  // 0x1A6FC0(0, p, v): a world vector in the object's frame
{
    // (vt slot 31 0x1F3FA0: the object record +0x60's world matrix, which follows the rotation record; not the first
    // record's: the enemies' turn was invisible to their rule-2 angle test)
    const XF* pxf = PxfObj(palo);
    const glm::mat4 m = pxf ? pxf->matWorld : MatWorldOfAlo(palo);
    return glm::transpose(glm::mat3(m)) * v;
}

// orig: 0x00180668 (vt slot 16)
void UpdateEnemy(LO* plo)
{
    if (FPrint2() && static_cast<int>(g_clock.t * 60.0f) % 60 == 0)
        std::printf("hack enemy %d t %.2f: next update %.3f in world %d\n", plo->oid, g_clock.t, plo->tNextUpdate, plo->fInWorld);
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    HACKSO& h = Hack(plo);
    ALO* palo = Palo(plo);
    if (h.state == 2 || h.state == 4)
        if (FShipSegmentDone(plo)) SetEnemyState(plo, 0);
    LO* pt = h.ploEnemyTarget;
    const bool fAlive = pt && pt->fInWorld;  // ((flags >> 60) & 1)
    if (h.state != 0 && h.state != 4 && !fAlive) SetEnemyState(plo, 0);
    if (!h.fBusy && fAlive && FCidDerivedFrom(pt->Cid(), 0x30))
    {
        const glm::vec3 d = PosAloCenter(static_cast<ALO*>(pt)) - PosAloCenter(palo);
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const glm::vec3 dl = VecToObject(palo, d);
        const float rad = RadNormalize(std::atan2(dl.y, dl.x));
        if (FPrint() && static_cast<int>(g_clock.t * 60.0f) % 60 == 0)
            std::printf("hack enemy %d t %.2f: state %d dist %.0f rad %.2f pos %.0f %.0f heading %.2f moves %d recs %d %d sost %d\n", plo->oid,
                        g_clock.t, h.state, dist, rad, PosAloCenter(palo).x, PosAloCenter(palo).y, h.radEnemyHeading, h.cMove,
                        PxfPos(palo) ? PxfPos(palo)->mode : -9, PxfRot(palo) ? PxfRot(palo)->mode : -9, Psost(palo) ? Psost(palo)->cnstrAng.kind * 10 + Psost(palo)->cnstrLin.kind : -1);
        const HACKRULE* pr = &h.arule[std::clamp(h.state, 0, 4)];
        bool fStay = pr->fDistance ? FInLm(pr->lmStayDistance, dist) : pr->fAngle;
        if (fStay && pr->fAngle) fStay = FInLm(pr->lmStayAngle, rad);
        if (!fStay)
        {
            int st;
            for (st = 4; st > 0; --st)
            {
                pr = &h.arule[st];
                bool f = pr->fDistance ? FInLm(pr->lmEnterDistance, dist) : pr->fAngle;
                if (!f) continue;
                if (!pr->fAngle || FInLm(pr->lmEnterAngle, rad)) break;
            }
            SetEnemyState(plo, st);
        }
    }
    switch (h.state)
    {
    case 1:
    case 2:
    {
        if (h.fCooldown || !pt || !FCidDerivedFrom(pt->Cid(), 0x30)) break;
        XF* pxfRot = PxfRot(palo);  // (+0x5C's +0xA0: its parent's space)
        const glm::vec3 d = VecConvertXfs(nullptr, pxfRot ? pxfRot->pxfRef : nullptr,
                                          PosAloCenter(static_cast<ALO*>(pt)) - PosAloCenter(palo));  // 0x22FCC8
        const float rad = RadNormalize(std::atan2(d.y, d.x));
        h.radEnemyHeading = std::clamp(h.polySimple.Eval(rad), h.lmSimple.gMin, h.lmSimple.gMax);
        break;
    }
    case 3:
    {
        if (!pt || !FCidDerivedFrom(pt->Cid(), 0x30)) break;
        const glm::vec3 posT = PosAloCenter(static_cast<ALO*>(pt));
        const glm::vec3 posS = PosAloCenter(palo);
        auto len = [](const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
        if (100.0f < len(posT - h.posLastTarget) || 100.0f < len(posS - h.posLastSelf) || 5.0f < g_clock.t - h.tPath)
        {
            // 0x1C02D0: the nav path into the curve (+0x1D0 the mesh's region)
            NAVREGION* preg = h.ploNav ? PregionOfMesh(dynamic_cast<LOXMESH*>(h.ploNav)) : nullptr;
            if (preg) FNavPathCrv(preg, posS, posT, &h.crvPath);
            h.sPath = 0.0f;
            h.posLastTarget = posT;
            h.posLastSelf = posS;
            h.tPath = g_clock.t;
        }
        glm::vec3 posNear(0.0f), dir(0.0f);
        if (!h.crvPath.apos.empty()) NearestCrvS(&h.crvPath, posS, h.sPath, &posNear, &dir, nullptr, &h.sPath);  // 0x1E7380
        glm::vec3 u = posNear + dir * 200.0f - posS;  // (D_002BE828)
        const float lu = len(u);
        u = lu < 0.0001f ? glm::vec3(0.0f) : u / lu;
        const float sRemaining = (h.crvPath.apos.empty() ? 0.0f : SLengthCrv(&h.crvPath)) - h.sPath;
        const glm::vec3 ul = VecToObject(palo, u);
        const float radTurn = std::atan2(ul.y, ul.x);
        auto lim01 = [](float g) { return std::clamp(g, 0.0f, 1.0f); };  // (D_002BBB60)
        const POLY3 polySuck{1.0f, -0.5f, 0.0f};  // (D_002BE830)
        const float sv = lim01(h.polyRemaining.Eval(sRemaining)) * lim01(h.polyAngle.Eval(radTurn)) *
                         lim01(polySuck.Eval(GSuck())) * h.gScale;
        glm::vec3 v = u * sv;
        // the other enemies (0x15E9C8 class 0x49) of other attachments push it away in the plane
        for (LO* po : AploOfClass(g_psw, 0x49))
        {
            if (po == plo || !po->fInWorld || Sort(Palo(po)).poxa == Sort(palo).poxa) continue;
            glm::vec3 a = posS - PosAloCenter(Palo(po));
            a.z = 0.0f;
            const float la = len(a);
            a = la < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : a / la;
            const POLY3 pw{1500.0f, -5.0f, 0.0f}, pdot{0.0f, -1.0f, 0.0f};  // (D_002BE840, D_002BE860)
            float w = std::clamp(pw.Eval(la), 0.0f, 1000.0f);                // (D_002BE850)
            w *= lim01(pdot.Eval(a.x * u.x + a.y * u.y + a.z * u.z));
            v += a * w;
        }
        const float lv = len(v);
        h.vPush = lv > sv && lv > 0.0f ? v * (sv / lv) : v;  // 0x11E9F8
        h.radEnemyHeading = std::atan2(u.y, u.x);
        if (static const bool s_f = std::getenv("SLY2N_HACKCHASEPRINT") != nullptr; s_f)  // (debug: the chasers per frame)
        {
            std::printf("hack chase %p t %.3f: pos %.2f %.2f s %.1f rem %.1f sv %.1f v %.1f %.1f suck %.3f push %.3f opts %zu\n", (void*)plo, g_clock.t, posS.x, posS.y,
                        h.sPath, sRemaining, sv, h.vPush.x, h.vPush.y, GSuck(), h.gPush, (plo->ploCloneOf ? plo->ploCloneOf : plo)->aopt.size());
            if (s_f && h.sPath == 0.0f) for (const OPT& o : (plo->ploCloneOf ? plo->ploCloneOf : plo)->aopt) std::printf("  opt eopid %d n %zu g %g\n", o.eopid, o.aval.size(), o.aval.empty() ? 0.0 : double(o.aval[0].ag[0]));
        }
        break;
    }
    default: break;
    }
}

// orig: 0x00180EE8 (vt slot 70: not dying: turned toward the heading (in the parent's space unless chasing) by
// +0x378; chasing: pushed toward +0x210 by +0x384 (0x1FAC30))
void MoveEnemy(ALO* palo)
{
    HACKSO& h = Hack(palo);
    ++h.cMove;
    if (h.state != 4)
    {
        glm::mat3 m = MatRotateAxis(h.radEnemyHeading, glm::vec3(0.0f, 0.0f, 1.0f));
        if (h.state != 3)
            if (XF* pxfRot = PxfRot(palo); pxfRot && pxfRot->pxfRef)
                m = glm::mat3(pxfRot->pxfRef->matWorld) * m;  // 0x22FD38
        BrakeAloRotation(palo, m, h.acTurnEnemy);
        if (FPrint2() && h.state != 0)
            if (XF* o = PxfRot(palo))
            {
                const glm::mat4 mw = MatWorldOfAlo(palo);
                std::printf("hack enemy %d move t %.3f: rot vel %.3f %.3f %.3f acc %.3f %.3f %.3f world heading %.3f mode %d grf %x rec %.3f recw %.3f hd %.3f\n", palo->oid,
                            g_clock.t, o->vel.x, o->vel.y, o->vel.z, o->aseg[3].x, o->aseg[3].y, o->aseg[3].z,
                            std::atan2(mw[0][1], mw[0][0]), o->mode, GrfUpdOfLo(palo), std::atan2(o->mat[0][1], o->mat[0][0]), std::atan2(o->matWorld[0][1], o->matWorld[0][0]), h.radEnemyHeading);
            }
    }
    if (h.state == 3) AccelAloPos(palo, (h.vPush - VelAloPos(palo)) * h.gPush);  // 0x1FAC30 (vt slot 33)
}

// orig: 0x001811B8 (vt slot 75 of the enemies: not while dying; a hit: none left: the task event 0x18; state 4)
bool FAcceptEnemyContact(ALO* palo, CTMSG* pctm)
{
    HACKSO& h = Hack(palo);
    if (h.state == 4) return false;
    if (!FHackSoHit(palo, pctm)) return false;
    if (h.cHp < 0) return true;
    if (h.cHp <= 0) BroadcastTaskEvent(g_psw, 0x18, palo);  // 0x1619A0(D_002DEB30, 0x18, p)
    SetEnemyState(palo, 4);
    return true;
}

// orig: 0x00180FF0 (vt slot 55 of the enemies: the base's; with points, touching the ship: dead (posted))
void OnEnemyXpNew(ALO* palo, XP* pxp, int i)
{
    OnHackXpNew(palo, pxp, i);
    HACKSO& h = Hack(palo);
    ALO* pOther = pxp->axpd[1 - i].palo;
    if (h.nPoints && pOther && FShip(pOther)) PostKillHackSo(palo, pOther);
}

// orig: 0x00181060 (vt slot 5 of the enemies, after the PO's 0x1C3E20: an event (message 9) of its segment's
// ASEGA (+0x194, or the segment SMA +0x198's): 0x15E a push (the clip's root velocity (0x204E38) turned by the
// heading, +0x210 and vt slot 35); 0x25 / 0x26 the cool-down +0x1C8 on / off; 0x27 / 0x28 busy +0x1CC on / off)
bool OnEnemyMessage(LO* plo, int msgk, void* pv)
{
    if (msgk != 9 || !pv || !plo || !plo->pcls || plo->pcls->PfnOrig(5) != 0x181060) return false;
    const ASEGMSG9* pm = static_cast<const ASEGMSG9*>(pv);
    HACKSO& h = Hack(plo);
    if (!pm->pasega || !(pm->pasega == h.pasegaSeg || (h.psmaSeg && pm->pasega == h.psmaSeg->pasega))) return false;
    switch (pm->n)
    {
    case 0x15E:
    {
        glm::vec4 v(0.0f);
        if (!FAsegaRetracted(pm->pasega))
            if (const ASEGCHN* pchn = PchnFindAseg(pm->pasega->paseg, Palo(plo), 1, true); pchn && pchn->ptrkp)
                pchn->ptrkp->Eval(nullptr, &v, pm->t, pm->pasega->svt);
        h.vPush = MatRotateAxis(h.radEnemyHeading, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::vec3(v);  // 0x11B500
        SetAloVelocityTo(Palo(plo), h.vPush);  // vt slot 35
        break;
    }
    case 0x25: h.fCooldown = 1; break;
    case 0x26: h.fCooldown = 0; break;
    case 0x27: h.fBusy = 1; break;
    case 0x28: h.fBusy = 0; break;
    default: break;
    }
    if (FPrint()) std::printf("hack enemy %d: segment event 0x%X at t %.3f\n", plo->oid, pm->n, g_clock.t);
    return false;  // (the PO's handlers too)
}

// orig: 0x00181238 (vt slot 83: the base's reset, state 0)
void ResetEnemy(LO* plo)
{
    HACKSO& h = Hack(plo);
    ResetHackSoBase(plo, h);
    SetEnemyState(plo, 0);
}

// ---- The docks (cid 0x4B): the end of a hack level

// orig: 0x0020FF70 / 0x0020FFF0 (the entries' state machines (of the root found from the object, kind 0x204 one
// match; the SM oid's SMA on it, 0x1F67E0) run to the entry's state: fl & 1 the "on" one, else the "off" one)
void RunDockRsmgs(LO* plo, HACKSO& h, int fl)
{
    for (const HACKSO::RSMG& e : h.arsmg)
    {
        LO* proot = nullptr;
        if (CploFindSwNearest(plo, e.oidRoot, &proot) != 1 || !proot || !FCidDerivedFrom(proot->Cid(), 0x30)) continue;
        const OID id = (fl & 1) ? e.oidOn : e.oidOff;
        if (id == -1) continue;
        for (SMA* psma : g_apsma)
            if (psma->palo == proot && (e.oidSm == -1 || (psma->psm && psma->psm->oid == e.oidSm)))
            {
                if (FPrint()) std::printf("hack dock %d: root %d's SM %d to %d\n", plo->oid, e.oidRoot, e.oidSm, id);
                if (fl & 2) SnapSmaGoal(psma, id);  // 0x1F0948
                else SetSmaGoal(psma, id);          // 0x1F0660
                break;
            }
    }
}


// orig: 0x001815E8 (the dock's state: 0 waiting, 1 docking (the ship's +0x294 = the dock point, the ship's state 3),
// 2 done (the task event 0x39); leaving 1 with a ship: 0x181728 (let go, the dock's event 6); the state's segment)
void SetDockState(LO* plo, int n)
{
    HACKSO& h = Hack(plo);
    if (h.state == n) return;
    if (h.state == 1 && h.ploShooter)
    {
        // orig: 0x00181728 (the ship let go: its state 1, the dock's vt slot 3 (6): its splice event 6, the game's 0x1B)
        SetShipState(h.ploShooter, 1);
        Hack(h.ploShooter).ploDock = nullptr;
        h.ploShooter = nullptr;
        HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
        BroadcastTaskEvent(g_psw, 0x1B, plo);
    }
    if (FPrint()) std::printf("hack dock %d: state %d -> %d at t %.3f\n", plo->oid, h.state, n, g_clock.t);
    h.state = n;
    h.tState = g_clock.t;
    switch (n)
    {
    case 1:
        if (h.ploShooter)
        {
            Hack(h.ploShooter).ploDock = h.ploEnemyTarget;  // (the ship's +0x294 = +0x1CC)
            SetShipState(h.ploShooter, 3);
        }
        break;
    case 2: BroadcastTaskEvent(g_psw, 0x39, plo); break;  // 0x1619A0(D_002DEB30, 0x39, p)
    default: break;
    }
    PlayHackSeg(plo, h, n == -1 ? nullptr : &h.aseg[n]);
}

// orig: 0x00181478 (vt slot 16: waiting: the ship within 50 (D_002BE880) of the dock point: docking; docking: its
// segment done: done)
void UpdateDock(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    HACKSO& h = Hack(plo);
    int s;
    do
    {
        s = h.state;
        switch (s)
        {
        case 0:
            if (s_ploShip && h.ploEnemyTarget)
            {
                const glm::vec3 d = PosAloCenter(Palo(s_ploShip)) - PosLoWorld(h.ploEnemyTarget);  // (its slot 31)
                if (FPrint() && s_ploShip->fInWorld && d.x * d.x + d.y * d.y + d.z * d.z < 200.0f * 200.0f)
                    std::printf("hack dock %d t %.2f: ship distance %.0f\n", plo->oid, g_clock.t, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
                if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) < 50.0f)
                {
                    h.ploShooter = s_ploShip;  // (+0x1C8)
                    SetDockState(plo, 1);
                }
            }
            break;
        case 1:
            if (FShipSegmentDone(plo)) SetDockState(plo, 2);
            break;
        default: break;
        }
    } while (h.state != s);
}

// orig: 0x00181598 (the docks' vt slot 83: the base's reset (0x17F6B0), waiting (state 0), its entries "off" (0x20FF70))
void ResetDock(LO* plo)
{
    HACKSO& h = Hack(plo);
    ResetHackSoBase(plo, h);
    SetDockState(plo, 0);
    RunDockRsmgs(plo, h, 0);
}

// orig: 0x00181440 (post-load: the base's (0x1F36A8, slot 83 0x181598: reset, state 0; TODO: 0x20FF70 its marker
// +0x200 / +0x204), the segments) / 0x00181408 (init: 3 segments at +0x1D0)
void PostLoadDock(LO* plo)
{
    if (!plo || plo->Cid() != 0x4B) return;
    HACKSO& h = Hack(plo);
    if (h.fPostLoaded) return;
    h.fPostLoaded = true;
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
    BindHackSegs(plo, h);
    if (const int oid = static_cast<int>(plo->U32Field(0, 0x1CC, 0xFFFFFFFFu)); oid != -1)
        h.ploEnemyTarget = PloFindSwNearest(plo, oid);  // (+0x1CC the dock point, EOPID 1394)
    ResetDock(plo);  // (vt slot 83 0x181598: 0x20FF70(G, +0x200, +0x204, p, 0))
    if (FPrint())
    {
        const glm::vec3 pos = h.ploEnemyTarget ? PosLoWorld(h.ploEnemyTarget) : glm::vec3(0.0f);
        std::printf("hack dock %d: rsmgs %zu", plo->oid, h.arsmg.size());
        for (const HACKSO::RSMG& e : h.arsmg) std::printf(" {%d %d %d %d}", e.oidRoot, e.oidSm, e.oidOn, e.oidOff);
        std::printf("\n");
        std::printf("hack dock %d: point %d at %.0f %.0f %.0f segs %d %d %d frame %p event6 %d clone of %p inst %d\n", plo->oid,
                    h.ploEnemyTarget ? h.ploEnemyTarget->oid : -1, pos.x, pos.y, pos.z, h.aseg[0].oid, h.aseg[1].oid, h.aseg[2].oid,
                    (void*)plo->pframe.get(), plo->pframe && plo->pframe->PrefFindBinding(6, 1) ? 1 : 0, (void*)plo->ploCloneOf, plo->oidInstance);
        for (CFrame* pf = plo->pframe.get(); pf; pf = pf->m_cpframeParent > 0 ? pf->m_apframeParentOwner[0].get() : nullptr)
        {
            std::string str;
            if (CRef* pr = pf->PrefFindBinding(6, 0); pr && pr->m_tagk == TAGK_Proc && pr->m_pproc)
            {
                CRef body;
                body.SetPair(pr->m_pproc->m_ppairCodeExpr);
                PrintRef(body, &str);
            }
            std::printf("   frame %p: event6 %s parents %d\n", (void*)pf, str.c_str(), pf->m_cpframeParent);
        }
    }
}

// ---- The swarms (cid 0x4C): particles strung along paths, each with hit points

// A particle (orig 0x70 bytes, 0x1AE2A0(0x70, 0x10)): its place on the path, its slot ranges (0x181B60), the smoothed
// distance, a lateral wobble when hit
struct SWQ
{
    glm::vec3 pos{0.0f};        // +0x00 (0x1E6F10 at +0x3C)
    glm::vec3 dir{0.0f};        // +0x10
    int ie = 0;                 // +0x20 its entry
    int state = -1;             // +0x24: 0 alive, 1 hit (0.15 s), 2 dead
    float tState = 0.0f;        // +0x28
    float k1a = 0, k1b = 0;     // +0x2C: its slot's range (the ship's distance on the path mapped into it)
    float k2a = 0, k2b = 0;     // +0x34: the ship's distance range it follows over
    float s = 0.0f;             // +0x3C: its distance on the path
    float ds = 0.0f;            // +0x40: its speed
    float sTarget = 0.0f;       // +0x44
    float g48 = 0.0f;           // +0x48
    int cHp = 0;                // +0x4C
    float lat = 0.0f, roll = 0.0f, dlat = 0.0f, droll = 0.0f;  // +0x50..+0x5C
};
// An entry (orig 0x30 bytes at +0x1CC, up to 4: 0x182AB8)
struct SWENT
{
    OID oidPath = -1;           // +0x00
    int cq = 0;                 // +0x04 its particles
    int cGroup = 0;             // +0x08
    int cAlive = 0;             // +0x0C
    std::vector<SWQ*> aq;       // +0x10 the live ones (a DL, link +0x60)
    int cHpInit = 30;           // +0x1C (D_002BE8AC)
    float r = 0.0f;             // +0x20 the radius
    LO* ppath = nullptr;        // +0x24
    float sShip = 0.0f;         // +0x28 the ship's distance on the path
    float tSearch = -1.0f;      // +0x2C the next search over the whole path
};
struct SWARM
{
    std::vector<SWENT> ae;
    std::vector<SWQ*> aqDead;   // +0x28C (a DL)
    std::vector<std::unique_ptr<SWQ>> aqAll;
    LO* ploTarget = nullptr;    // +0x1C0 (the ship)
    ALO* paloModel = nullptr;   // +0x1C4
    bool fPostLoaded = false;
};
std::unordered_map<const LO*, SWARM> s_mpswarm;
WORLD_TABLE(s_mpswarm);   // (PC: moved aside while a kept world loads, worldglobals.h)
SWARM& Swarm(LO* plo) { return s_mpswarm[plo]; }

const CRV* PcrvEnt(const SWENT& e) { return e.ppath ? PcrvFromLo(e.ppath) : nullptr; }
float SLenEnt(const SWENT& e)  // 0x1E6648
{
    const CRV* p = PcrvEnt(e);
    return p ? SLengthCrv(p) : 0.0f;
}

// orig: 0x00182AB8 (an entry: {path oid, count, group size, radius}, its particles' hit points D_002BE8AC (30), up to 4)
void AddSwarmEntry(LO* plo, const std::vector<CRef>& a)
{
    SWARM& w = Swarm(plo);
    if (w.ae.size() >= 4) return;
    SWENT e;
    e.oidPath = NArg(a, 0, -1);
    e.cq = NArg(a, 1, 0);
    e.cGroup = NArg(a, 2, 0);
    e.r = GArg(a, 3, 0.0f);
    e.tSearch = -1.0f;
    e.cHpInit = 30;
    w.ae.push_back(e);
}

// orig: 0x00181B60 (the live particles' slots along the path: each a range k1 of the path that the ship's distance
// in k2 maps into, in groups of cGroup)
void LayoutSwarmEnt(SWENT& e)
{
    const float g = SLenEnt(e);
    const int c = e.cAlive;
    float r = g * 0.5f / static_cast<float>(c);
    r = e.r < r ? e.r : r;
    const int m = e.cGroup < c ? e.cGroup : c;
    const float fm = static_cast<float>(m);
    const float gap = (g - 2.0f * static_cast<float>(c) * r) / static_cast<float>(c - m + 1);
    const float step = gap + 2.0f * r;
    float k1a = (2.0f * fm - 1.0f) * r - step * static_cast<float>(m - 1);
    float k1b = (2.0f * fm - 1.0f) * r + fm * gap - step * static_cast<float>(m - 1);
    float k2a = k1a - fm * r;
    float k2b = k1b + fm * r;
    for (SWQ* q : e.aq)
    {
        q->k1a = k1a; q->k1b = k1b; q->k2a = k2a; q->k2b = k2b;
        k1a += step; k1b += step; k2a += step; k2b += step;
    }
}

// orig: 0x00181EB0 (a particle's state; dead (2): out of its entry's list into the dead one, the entry's count down
// (none left: the task event 0x3A), the slots laid out again)
void SetSwarmQState(LO* plo, SWQ* q, int n)
{
    if (n == q->state) return;
    q->state = n;
    q->tState = g_clock.t;
    if (n != 2) return;
    SWARM& w = Swarm(plo);
    SWENT& e = w.ae[static_cast<size_t>(q->ie)];
    --e.cAlive;
    e.aq.erase(std::remove(e.aq.begin(), e.aq.end(), q), e.aq.end());  // 0x11A2E8
    w.aqDead.push_back(q);                                               // 0x11A220
    if (FPrint()) std::printf("hack swarm %d: a particle of entry %d dead (%d left) at t %.3f\n", plo->oid, q->ie, e.cAlive, g_clock.t);
    if (!e.cAlive) BroadcastTaskEvent(g_psw, 0x3A, plo);  // 0x1619A0(D_002DEB30, 0x3A, p)
    LayoutSwarmEnt(e);
}

// orig: 0x00181A90 (an entry's particles placed evenly along the path, alive, full hit points)
void ResetSwarmEnt(LO* plo, SWENT& e)
{
    LayoutSwarmEnt(e);
    const float g = SLenEnt(e);
    int i = 0;
    for (SWQ* q : std::vector<SWQ*>(e.aq))
    {
        q->state = -1;
        SetSwarmQState(plo, q, 0);
        q->s = g * (static_cast<float>(i) + 0.5f) / static_cast<float>(e.cAlive);
        q->ds = 0.0f;
        q->cHp = e.cHpInit;
        ++i;
    }
}

// orig: 0x001819E0 (vt slot 83: the base's reset (0x17F6B0), the dead particles back in their entries' lists, each
// entry's count back and its particles placed)
void ResetSwarm(LO* plo)
{
    HACKSO& h = Hack(plo);
    ResetHackSoBase(plo, h);
    SWARM& w = Swarm(plo);
    for (SWQ* q : w.aqDead) w.ae[static_cast<size_t>(q->ie)].aq.push_back(q);
    w.aqDead.clear();
    for (SWENT& e : w.ae)
    {
        e.cAlive = e.cq;
        ResetSwarmEnt(plo, e);
    }
}

// orig: 0x00181CB0 (each particle's target distance: the ship's distance through its k2 range into its k1 range,
// kept from overlapping the others (radius r))
void TargetSwarmEnt(SWENT& e)
{
    const float g = SLenEnt(e);
    const float r0 = g * 0.5f / static_cast<float>(e.cAlive);
    const float r = e.r < r0 ? e.r : r0;
    int i = 0;
    for (SWQ* q : e.aq)
    {
        const int j = e.cAlive - i - 1;
        float t = (e.sShip - q->k2a) / (q->k2b - q->k2a);
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        const float lo = (2.0f * static_cast<float>(i) + 1.0f) * r;
        const float hi = g - (2.0f * static_cast<float>(j) + 1.0f) * r;
        float x = (1.0f - t) * q->k1a + t * q->k1b;
        if (x < lo) x = lo;
        else if (hi < x) x = hi;
        q->sTarget = x;
        x = (q->k1a + q->k1b) * 0.5f;
        q->g48 = -std::fabs(x - e.sShip);
        ++i;
    }
}

void DumpSwarm(LO* plo);

// orig: 0x00181F60 (vt slot 16: with a target: the ship's distance on each path (searched over the whole path every
// 0.5..0.75 s, else from the last), the particles smoothed toward their targets (0x11D1C0 D_002BE888), placed on the
// path, their wobble's springs; a hit one alive again after 0.15 s)
void UpdateSwarm(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    SWARM& w = Swarm(plo);
    if (!w.ploTarget) return;
    struct DUMPATEND { LO* p; ~DUMPATEND() { DumpSwarm(p); } } dumpAtEnd{plo};
    const glm::vec3 posShip = s_ploShip ? PosAloCenter(Palo(s_ploShip)) : glm::vec3(0.0f);  // 0x1A62E8(D_002DFB80)
    static const cmm::SMPA s_smpa{1500.0f, 0.0f, 0.5f, 4500.0f};  // D_002BE888
    for (size_t ie = 0; ie < w.ae.size(); ++ie)
    {
        SWENT& e = w.ae[ie];
        const CRV* pcrv = PcrvEnt(e);
        if (!pcrv) continue;
        if (!(g_clock.t <= e.tSearch))
        {
            NearestCrvAll(pcrv, posShip, nullptr, nullptr, nullptr, &e.sShip);  // 0x1E6F60
            e.tSearch = g_clock.t + GRandInRange(0.5f, 0.75f);                     // 0x11D448
        }
        else
        {
            const float s0 = e.sShip;
            NearestCrvS(pcrv, posShip, s0, nullptr, nullptr, nullptr, &e.sShip);  // 0x1E7380
        }
        TargetSwarmEnt(e);
        const float dt = g_clock.dt;
        for (SWQ* q : std::vector<SWQ*>(e.aq))
        {
            q->s = cmm::GSmoothA(q->s, q->ds, q->sTarget, dt, &s_smpa, &q->ds);
            EvalCrvS(pcrv, q->s, &q->pos, &q->dir);  // 0x1E6F10
            const float a = -100.0f * q->lat + -10.0f * q->dlat;    // D_002BE898 / D_002BE89C
            const float b = -15.0f * q->roll + -2.5f * q->droll;    // D_002BE8A0 / D_002BE8A4
            auto integrate = [dt](float* px, float* pv, float acc) {
                const float vOld = *pv;
                *pv = vOld + dt * acc;
                *px += dt * ((vOld + *pv) * 0.5f);
            };
            integrate(&q->lat, &q->dlat, a);
            integrate(&q->lat, &q->droll, b);  // (the original's second spring also moves +0x50, not the roll +0x54)
            // orig: 0x00181E10 (hit: alive again after D_002BE884 0.15 s)
            int st;
            do
            {
                st = q->state;
                if (st == 1 && g_clock.t - q->tState > 0.15f) SetSwarmQState(plo, q, 0);
            } while (q->state != st);
        }
    }
}

// (debug: SLY2N_SWARMDUMP="OID@T[,T..]": the swarm's entries and particles once the clock passes each T)
void DumpSwarm(LO* plo)
{
    static const char* s_sz = std::getenv("SLY2N_SWARMDUMP");
    if (!s_sz || std::atoi(s_sz) != plo->oid) return;
    static std::vector<float> s_at;
    static bool s_fParsed = false;
    if (!s_fParsed)
    {
        s_fParsed = true;
        for (const char* pch = std::strchr(s_sz, '@'); pch; pch = std::strchr(pch + 1, ','))
            s_at.push_back(static_cast<float>(std::atof(pch + 1)));
    }
    bool f = false;
    for (float& t : s_at)
        if (t <= g_clock.t)
        {
            f = true;
            t = FLT_MAX;
        }
    if (!f) return;
    std::printf("swarmdump %d t %.3f\n", plo->oid, g_clock.t);
    for (const SWENT& e : Swarm(plo).ae)
    {
        std::printf("  entry %d %d %d %d r %.1f sShip %.4f tS %.4f\n", e.oidPath, e.cq, e.cGroup, e.cAlive, e.r, e.sShip, e.tSearch);
        for (const SWQ* q : e.aq)
            std::printf("    q pos %.2f %.2f %.2f st %d s %.2f v %.2f tgt %.2f hp %d k1 %.1f %.1f k2 %.1f %.1f lat %.2f\n", q->pos.x, q->pos.y,
                        q->pos.z, q->state, q->s, q->ds, q->sTarget, q->cHp, q->k1a, q->k1b, q->k2a, q->k2b, q->lat);
    }
}

// orig: 0x00182810 (the live particle nearest to pos, over every entry)
SWQ* PqNearestSwarm(LO* plo, const glm::vec3& pos)
{
    SWQ* pBest = nullptr;
    float gBest = FLT_MAX;
    for (SWENT& e : Swarm(plo).ae)
        for (SWQ* q : e.aq)
        {
            const glm::vec3 d = q->pos - pos;
            const float g = d.x * d.x + d.y * d.y + d.z * d.z;
            if (g < gBest)
            {
                gBest = g;
                pBest = q;
            }
        }
    return pBest;
}

// the burst at a particle (0x15C198(+0x190, 4, 0, 0, 0, D_002A2DD0, pos))
void BurstSwarmAt(LO* plo, const glm::vec3& pos)
{
    HACKSO& h = Hack(plo);
    if (!h.ploBurst) return;
    BURSTPARAMS bp;
    bp.grf = 4;
    bp.pos = pos;
    ExplodeLo(h.ploBurst, &bp);
}

// orig: 0x001829D8 (vt slot 82 of the swarms: touched by a hack object: the particle nearest to it hurts it by the
// points (0x1F7598 {p, 0, it, 0x100, -points, the particle}); taken: the particle bursts and dies; the swarm can be
// touched again)
void KillSwarm(LO* plo, LO* ploOther)
{
    HACKSO& h = Hack(plo);
    if (ploOther && h.nPoints && FCidDerivedFrom(ploOther->Cid(), 0x30))
        if (SWQ* q = PqNearestSwarm(plo, PosAloCenter(Palo(ploOther))))
        {
            CTMSG m;
            m.ploSender = plo;
            m.ploReceiver = ploOther;
            m.pos = q->pos;
            m.grfFilter = 0x100;
            m.nState = -h.nPoints;
            if (PaloAcceptContact(&m))
            {
                if (FPrint()) std::printf("hack swarm %d: a particle hit %d (%d) at t %.3f\n", plo->oid, ploOther->oid, -h.nPoints, g_clock.t);
                BurstSwarmAt(plo, q->pos);
                SetSwarmQState(plo, q, 2);
            }
        }
    h.fDead = 0;
}

// orig: 0x001828A8 (vt slot 75 of the swarms: a hit (contact bytes 8) takes the nearest particle's hit points; left:
// knocked sideways (the hit's velocity across the path x D_002BE8A8 0.25, a random roll speed), state 1; none: it
// bursts and dies)
bool FAcceptSwarmContact(ALO* palo, CTMSG* pctm)
{
    if (!(pctm->grfAccepted & 8)) return false;
    if (SWQ* q = PqNearestSwarm(palo, pctm->pos))
    {
        q->cHp += pctm->nState;
        if (FPrint()) std::printf("hack swarm %d: particle hit %d -> hp %d at t %.3f\n", palo->oid, pctm->nState, q->cHp, g_clock.t);
        if (q->cHp > 0)
        {
            const glm::vec3 c = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), q->dir);  // D_002A2DC0 x dir
            q->dlat = (pctm->vec.x * c.x + pctm->vec.y * c.y + pctm->vec.z * c.z) * 0.25f;
            q->droll = GRandInRange(-15.0f, 15.0f);
            SetSwarmQState(palo, q, 1);
        }
        else
        {
            BurstSwarmAt(palo, q->pos);
            SetSwarmQState(palo, q, 2);
        }
    }
    return true;
}

// orig: 0x00182700 (vt slot 55 of the swarms: the base's; with points, touching a hack object: dead (posted: 0x1829D8))
void OnSwarmXpNew(ALO* palo, XP* pxp, int i)
{
    OnHackXpNew(palo, pxp, i);
    HACKSO& h = Hack(palo);
    ALO* pOther = pxp->axpd[1 - i].palo;
    if (h.nPoints && pOther && FCidDerivedFrom(pOther->Cid(), 0x47)) PostKillHackSo(palo, pOther);
}

// orig: 0x001823C8 (vt slot 60 of the swarms: against a sphere entry k: a kind-10 contact for each live particle
// within k's radius + the entry's (in the plane); else, through k's moving points' segments this frame)
void AddSwarmXps(ALO* palo, SOE* k, int, const BSPREF&, int, XP** ppxp)
{
    if (!k || !k->f49 || palo->asoe.empty()) return;
    SOE* psoe = &palo->asoe.back();  // (+0xE4's last: the entry 0x181840 added)
    const glm::vec3 center = k->pxf ? glm::vec3(k->pxf->matWorld[3]) : glm::vec3(0.0f);
    for (SWENT& e : Swarm(palo).ae)
    {
        const float radius = k->sRadius + e.r;
        for (SWQ* q : e.aq)
        {
            glm::vec3 dir = q->pos - center;
            dir.z = 0.0f;
            const float dist = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            dir = dist < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : dir / dist;  // (else D_002A2DA0)
            if (dist < radius)
            {
                PxpAddSw(psoe, k, center + dir * -k->sRadius, dir, 10, q, nullptr, ppxp, dist - radius);  // 0x1A7C18
                continue;
            }
            for (int j = k->pinned; j < static_cast<int>(k->geomLocal.apos.size()) && j < static_cast<int>(k->geomWorld.apos.size()); ++j)
            {
                glm::vec3 posNew = glm::vec3(k->mat * glm::vec4(k->geomLocal.apos[j], 1.0f));
                glm::vec3 posOld = k->geomWorld.apos[j];
                posNew.z = posOld.z = q->pos.z;
                LSG alsg[2];
                if (ClipSegmentToSphere(q->pos, posNew, posOld, 2, alsg, radius) > 0)  // 0x14C748
                {
                    const glm::vec4 plane = alsg[0].plane0;
                    const glm::vec3 n(plane);
                    const glm::vec3 at = posOld + n * k->sRadius;
                    const float g = (n.x * posOld.x + n.y * posOld.y + n.z * posOld.z) - plane.w - k->sRadius;
                    PxpAddSw(psoe, k, at, n, 10, q, nullptr, ppxp, g);
                    break;
                }
            }
        }
    }
}

// orig: 0x00182780 (vt slot 49 of the swarms: the base's bounds (0x1F50E0), then each path's box corners with the
// entry's radius (0x1E7BB8, 0x1F5480))
void UpdateSwarmBounds(ALO* palo)
{
    UpdateAloBounds(palo);
    OXA* poxa = Poxa(palo);
    if (!poxa) return;
    for (SWENT& e : Swarm(palo).ae)
    {
        const CRV* pcrv = PcrvEnt(e);
        if (!pcrv || pcrv->apos.empty()) continue;
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        auto add = [&](const glm::vec3& v) {
            const glm::vec3 w = glm::vec3(pcrv->mat * glm::vec4(v, 1.0f));
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
        };
        for (const glm::vec3& v : pcrv->apos) add(v);
        if (pcrv->crvk != 0)  // (cubic: each segment's control points, the tangents by a third of its time)
            for (size_t i = 0; i + 1 < pcrv->apos.size() && i + 1 < pcrv->ag.size(); ++i)
            {
                const float dt = pcrv->ag[i + 1] - pcrv->ag[i];
                if (i < pcrv->avOut.size()) add(pcrv->apos[i] + pcrv->avOut[i] * (dt * 0.333333343f));
                if (i + 1 < pcrv->avIn.size()) add(pcrv->apos[i + 1] + pcrv->avIn[i + 1] * (dt * -0.333333343f));
            }
        for (const glm::vec3& c : {mn, mx})
        {
            poxa->posMin = glm::min(poxa->posMin, c - glm::vec3(e.r));
            poxa->posMax = glm::max(poxa->posMax, c + glm::vec3(e.r));
            const glm::vec3 d = c - poxa->posCenter;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (poxa->sRadius < dist) poxa->sRadius = dist;
        }
    }
}

struct INITHACK
{
    INITHACK()
    {
        RegisterLoUpdate(0x17F930, UpdateShip);
        RegisterLoUpdate(0x1801B8, UpdatePiece);
        RegisterLoUpdate(0x180668, UpdateEnemy);
        RegisterLoUpdate(0x181478, UpdateDock);
        RegisterLoUpdate(0x181F60, UpdateSwarm);
        RegisterLoMessageHandler(OnEnemyMessage);
        // optid 526 (0x1101F8 -> 0x1816E8 -> 0x20FEC0, FAddRsmg): a dock's state machine entry
        RegisterObjectMethod(0x1101F8, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && plo->Cid() == 0x4B)
            {
                // {fOn, root, SM, state}: the entry of (root, SM) (a new one, 4 at most) gets its on (fOn) / off state
                HACKSO& h = Hack(plo);
                const int fOn = NArg(a, 0, 0);
                const OID oidRoot = NArg(a, 1, -1), oidSm = NArg(a, 2, -1), oidState = NArg(a, 3, -1);
                HACKSO::RSMG* pe = nullptr;
                for (HACKSO::RSMG& e : h.arsmg)
                    if (e.oidRoot == oidRoot && e.oidSm == oidSm) pe = &e;
                if (!pe && h.arsmg.size() < 4)
                {
                    h.arsmg.push_back(HACKSO::RSMG{oidRoot, oidSm, -1, -1});
                    pe = &h.arsmg.back();
                }
                if (pe) (fOn ? pe->oidOn : pe->oidOff) = oidState;
            }
            return CRef(TAGK_Void);
        });
        // optid 235 (0x110338 -> 0x181780): the dock's entries run "on" (0x20FF70(.., 1)), its vt slot 3 (5): its
        // splice event 5, the game's 0x1A
        RegisterObjectMethod(0x110338, [](LO* plo, const std::vector<CRef>&) {
            if (FPrint()) std::printf("hack dock %d: method 235 at t %.3f\n", plo ? plo->oid : -1, g_clock.t);
            if (plo && plo->Cid() == 0x4B)
            {
                RunDockRsmgs(plo, Hack(plo), 1);
                HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
                BroadcastTaskEvent(g_psw, 0x1A, plo);
            }
            return CRef(TAGK_Void);
        });
        // optid 369: a state's segment {state, oid, grfapl} (the ship 0x10FDA8 -> 0x1800D8, the enemies 0x10FE60 ->
        // 0x181308)
        for (uint32_t pfn : {0x10FDA8u, 0x10FE60u, 0x110140u})
            RegisterObjectMethod(pfn, [](LO* plo, const std::vector<CRef>& a) {
                const int i = NArg(a, 0, -1);
                if (FHackSo(plo) && i >= 0 && i < 5)
                {
                    Hack(plo).aseg[i].oid = NArg(a, 1, -1);
                    Hack(plo).aseg[i].grf = NArg(a, 2, 0);
                }
                return CRef(TAGK_Void);
            });
        // optid 1182 / 1183 (0x10FF18 -> 0x181330 / 0x10FFE8 -> 0x181378): an enemy state's distance / angle rule
        // {state, enter LM, stay LM}
        RegisterObjectMethod(0x10FF18, [](LO* plo, const std::vector<CRef>& a) {
            const int i = NArg(a, 0, -1);
            if (plo && plo->Cid() == 0x49 && i >= 0 && i < 5)
            {
                HACKRULE& r = Hack(plo).arule[i];
                r.fDistance = 1;
                r.lmEnterDistance = LmArg(a, 1);
                r.lmStayDistance = LmArg(a, 2);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10FFE8, [](LO* plo, const std::vector<CRef>& a) {
            const int i = NArg(a, 0, -1);
            if (plo && plo->Cid() == 0x49 && i >= 0 && i < 5)
            {
                HACKRULE& r = Hack(plo).arule[i];
                r.fAngle = 1;
                r.lmEnterAngle = LmArg(a, 1);
                r.lmStayAngle = LmArg(a, 2);
            }
            return CRef(TAGK_Void);
        });
        // EOPID 1387 (optid 508, 0x1100B8 -> 0x180478): a piece shot by an object: its end at its life (0: never)
        RegisterObjectMethod(0x1100B8, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && plo->Cid() == 0x4A)
            {
                HACKSO& h = Hack(plo);
                LO* pshooter = PloArg(a, 0);
                if (pshooter != h.ploShooter)
                {
                    h.ploShooter = pshooter;
                    // (0x134320: the attachment's filter rebuilt when this is its root: the overlap kinds above)
                }
                h.tEnd = h.dtLife == 0.0f ? FLT_MAX : g_clock.t + h.dtLife;
            }
            return CRef(TAGK_Void);
        });
        // EOPID 1368 (0x10FC80): vt slot 83, the reset (the ship 0x17FCB0, the enemies 0x181238, the docks 0x181598,
        // the swarms 0x1819E0, else the base's 0x17F6B0); a station's script resets them after the ship's death
        RegisterObjectMethod(0x10FC80, [](LO* plo, const std::vector<CRef>&) {
            if (FPrint() && plo) std::printf("hack %d (cid 0x%X): reset (method 1368) at t %.3f\n", plo->oid, plo->Cid(), g_clock.t);
            if (!FHackSo(plo)) return CRef(TAGK_Void);
            switch (plo->Cid())
            {
            case 0x48: ResetShip(plo); break;
            case 0x49: ResetEnemy(plo); break;
            case 0x4B: ResetDock(plo); break;
            case 0x4C: ResetSwarm(plo); break;
            default:
            {
                ResetHackSoBase(plo, Hack(plo));  // 0x17F6B0
                break;
            }
            }
            return CRef(TAGK_Void);
        });
        // EOPID 1367 (0x10FBF0): vt slot 82 with an object, the death (the swarms 0x1829D8, else the base's 0x17F5F8)
        RegisterObjectMethod(0x10FBF0, [](LO* plo, const std::vector<CRef>& a) {
            if (!FHackSo(plo)) return CRef(TAGK_Void);
            if (plo->Cid() == 0x4C) KillSwarm(plo, PloArg(a, 0));
            else KillHackSo(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        // EOPIDs 1363 / 1364 / 1365 (fields +0x180 / +0x184 / +0x188): the hit points' maximum, the hit points, the points
        RegisterObjectGetter(1363, [](LO* plo) { CRef r; r.SetS32(FHackSo(plo) ? Hack(plo).cHpMax : 0); return r; });
        RegisterObjectGetter(1364, [](LO* plo) { CRef r; r.SetS32(FHackSo(plo) ? Hack(plo).cHp : 0); return r; });
        RegisterObjectGetter(1365, [](LO* plo) { CRef r; r.SetS32(FHackSo(plo) ? Hack(plo).nPoints : 0); return r; });
        RegisterObjectSetter(1363, [](LO* plo, const CRef& r) { if (FHackSo(plo)) Hack(plo).cHpMax = r.RefCoerceS32().m_n; });
        RegisterObjectSetter(1364, [](LO* plo, const CRef& r) {
            if (!FHackSo(plo)) return;
            Hack(plo).cHp = r.RefCoerceS32().m_n;
            if (FPrint()) std::printf("hack %d: hp set to %d at t %.3f\n", plo->oid, Hack(plo).cHp, g_clock.t);
            if (FShip(plo)) UpdateHackHud(plo, Hack(plo));
        });
        RegisterObjectSetter(1365, [](LO* plo, const CRef& r) { if (FHackSo(plo)) Hack(plo).nPoints = r.RefCoerceS32().m_n; });
        // EOPIDs 1376 / 1377 (0x1813C0 / 0x1813F0): the enemy's turn and push gains x r
        RegisterObjectSetter(1376, [](LO* plo, const CRef& r) { if (FHackSo(plo)) SetEnemyTurnScale(Hack(plo), r.RefCoerceF32().m_g); });
        RegisterObjectSetter(1377, [](LO* plo, const CRef& r) { if (FHackSo(plo)) Hack(plo).gPush = 5.0f * r.RefCoerceF32().m_g; });
        // EOPIDs 1378 / 1379 (cid 0x49 fields +0x1D0 / +0x1D4, objects): the nav mesh and the target; world 15's towers
        // (block 27920 module 0) hand theirs to each chaser 27921 they attach (UH3)
        RegisterObjectGetter(1378, [](LO* plo) { CRef r; r.SetBasic(FHackSo(plo) ? Hack(plo).ploNav : nullptr); return r; });
        RegisterObjectGetter(1379, [](LO* plo) { CRef r; r.SetBasic(FHackSo(plo) ? Hack(plo).ploEnemyTarget : nullptr); return r; });
        RegisterObjectSetter(1378, [](LO* plo, const CRef& r) { if (FHackSo(plo)) Hack(plo).ploNav = PloArg({r}, 0); });
        RegisterObjectSetter(1379, [](LO* plo, const CRef& r) { if (FHackSo(plo)) Hack(plo).ploEnemyTarget = PloArg({r}, 0); });
        // optid 1195 (0x1103B0 -> 0x182AB8): a swarm's entry {path, count, group, radius}, up to 4
        RegisterObjectMethod(0x1103B0, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && plo->Cid() == 0x4C) AddSwarmEntry(plo, a);
            return CRef(TAGK_Void);
        });
        // EOPID 1371 (0x10FD00 -> 0x1800A0): a gun {record oid, SM oid}, up to 4
        RegisterObjectMethod(0x10FD00, [](LO* plo, const std::vector<CRef>& a) {
            if (FShip(plo))
            {
                HACKSO& h = Hack(plo);
                if (h.cgun < 4)
                {
                    h.agun[h.cgun].oidPart = NArg(a, 0, -1);
                    h.agun[h.cgun].oidSm = NArg(a, 1, -1);
                    ++h.cgun;
                }
            }
            return CRef(TAGK_Void);
        });
    }
} s_inithack;
}  // namespace

// orig: 0x0017F850 (the ship's post-load: the base's 0x17F138 (0x1F36A8, then vt slot 83 0x17FCB0), not controlled
// (vt slot 79 (0)), the docking records' reset (0x2303A0 on +0x58 / +0x5C), the guns (0x17F770), the segments
// (0x17F408))
void PostLoadHackShip(LO* plo)
{
    if (!FShip(plo)) return;
    HACKSO& h = Hack(plo);
    if (h.fPostLoaded) return;
    h.fPostLoaded = true;
    s_ploShip = plo;
    LoadHackFields(plo, h);
    h.cHp = h.cHpMax;
    if (!h.ploHud)  // (+0x2A0 EOPID 1373: an oid until the load resolves it)
        if (const OID oid = static_cast<OID>(plo->U32Field(0, 0x2A0, 0xFFFFFFFFu)); oid != -1) h.ploHud = PloFindSwNearest(plo, oid);
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;  // (0x17F090: +0x40 | 2)
    else plo->grfUpd |= GRFUPD_Update;
    ResetShip(plo);
    EnablePo(g_psw, plo, 0);  // vt slot 79 (0)
    if (XF* p58 = PxfPos(Palo(plo))) SetXfMode(Palo(plo), p58, 2);  // 0x2303A0(p, +0x58, 2)
    if (XF* p5C = PxfRot(Palo(plo))) SetXfMode(Palo(plo), p5C, 2);  // 0x2303A0(p, +0x5C, 2)
    BindShipGuns(plo, h);
    BindHackSegs(plo, h);  // 0x17F408
    if (FPrint())
        std::printf("hack ship %d: post-load hp %d/%d guns %d (%d %d) knock %.0f %.0f %.0f lm %.0f %.0f sound %d\n", plo->oid,
                    h.cHp, h.cHpMax, h.cgun, h.cgun ? h.agun[0].oidPart : -1, h.cgun ? h.agun[0].oidSm : -1, h.agKnock[0],
                    h.agKnock[1], h.agKnock[2], h.lmKnock.gMin, h.lmKnock.gMax, h.idSound);
    if (FPrint())
    {
        std::printf("hack ship fields:");
        for (const auto& [k, u] : plo->afield) std::printf(" [%x:%x %x]", uint32_t(k >> 32), uint32_t(k), u);
        for (const OPT& o : plo->aopt) std::printf(" {e%d}", o.eopid);
        std::printf("\n");
    }
}

// orig: 0x001805C8 (the enemy's post-load: the base's 0x17F138 (0x1F36A8, then vt slot 83 0x181238: reset, state 0),
// its path (0x1E94B0(0x2C9, 16)), the records' mode 2 (0x2303A0 on +0x58 / +0x5C), the segments (0x17F408))
void PostLoadHackEnemy(LO* plo)
{
    if (!plo || plo->Cid() != 0x49) return;
    HACKSO& h = Hack(plo);
    if (h.fPostLoaded) return;
    h.fPostLoaded = true;
    LoadHackFields(plo, h);
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;  // (0x17F090: +0x40 | 2)
    else plo->grfUpd |= GRFUPD_Update;
    BindHackSegs(plo, h);
    // (the object fields keep oids: the target (the ship) and the nav mesh, found from the object (kind 4))
    if (!h.ploEnemyTarget)
        if (const int oid = static_cast<int>(plo->U32Field(0, 0x1D4, 0xFFFFFFFFu)); oid != -1) h.ploEnemyTarget = PloFindSwNearest(plo, oid);
    if (!h.ploNav)
        if (const int oid = static_cast<int>(plo->U32Field(0, 0x1D0, 0xFFFFFFFFu)); oid != -1) h.ploNav = PloFindSwNearest(plo, oid);
    ResetEnemy(plo);
    if (XF* p58 = PxfPos(Palo(plo))) SetXfMode(Palo(plo), p58, 2);
    if (XF* p5C = PxfRot(Palo(plo))) SetXfMode(Palo(plo), p5C, 2);
    if (FPrint())
    {
        std::printf("hack enemy %d: post-load hp %d target %d nav %d scale %.0f segs", plo->oid, h.cHp,
                    h.ploEnemyTarget ? h.ploEnemyTarget->oid : -1, h.ploNav ? h.ploNav->oid : -1, h.gScale);
        for (const HACKSEG& e : h.aseg) std::printf(" %d%s", e.oid, e.paseg ? "a" : e.psm ? "s" : "");
        std::printf(" rules");
        for (const HACKRULE& r : h.arule)
            std::printf(" [%d %d %.0f..%.0f %.2f..%.2f]", r.fDistance, r.fAngle, r.lmEnterDistance.gMin, r.lmEnterDistance.gMax,
                        r.lmEnterAngle.gMin, r.lmEnterAngle.gMax);
        std::printf("\n");
    }
}

// orig: 0x0017F8C8 (the ship's vt slot 78, after the PO's 0x1C3FF0: focused: state 1; else, in the world, 0)
void SetHackShipFocus(LO* plo, bool fOn)
{
    if (!FShip(plo)) return;
    if (fOn) SetShipState(plo, 1);
    else if (plo->fInWorld) SetShipState(plo, 0);
}

void PostLoadHackDock(LO* plo) { PostLoadDock(plo); }

// orig: 0x0017F110 (vt slot 12 of the hack objects, after the ALO's 0x1F2D48: not dead) / 0x0017F740 (the ship's:
// then state -1: its HUD hidden, its sound stopped) / 0x00180598 (the enemies': then state -1, so the next state
// refills the hit points). R52: the enemies' slot was not dispatched, so a rammer that died on the ship (0x17F5B0
// sets +0x18C) kept it when its pool row came back; its next touch of the ship posted no death (0x17F5B0 returns on
// +0x18C) and it stayed pressed against a standing ship without hurting it
void OnRemoveHackSo(LO* plo)
{
    if (!FHackSo(plo)) return;
    HACKSO& h = Hack(plo);
    h.fDead = 0;
    if (!plo->pcls) return;
    if (plo->pcls->PfnOrig(12) == 0x17F740) SetShipState(plo, -1);
    else if (plo->pcls->PfnOrig(12) == 0x180598) SetEnemyState(plo, -1);  // 0x181268(p, -1)
}

// orig: 0x00181840 (the swarm's post-load: each entry's path (none: the entries from it dropped) and its particles
// (0x1AE2A0(0x70, 0x10)); the base's (0x17F138: 0x1F36A8, slot 83 0x1819E0); one more collision entry (+0xE4's
// next: the object record, tag -1, a sphere sensor (bits 48 / 49); its slot 60 makes the particles' contacts), in
// the attachment when in the world (0x1F2B20))
void PostLoadHackSwarm(LO* plo)
{
    if (!plo || plo->Cid() != 0x4C) return;
    SWARM& w = Swarm(plo);
    if (w.fPostLoaded) return;
    w.fPostLoaded = true;
    // (0x17F090: +0x40 | 2, updated; 0x1817E8: | 8, drawn: it has no globs of its own)
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update | 8u;
    else plo->grfUpd |= GRFUPD_Update | 8u;
    HACKSO& h = Hack(plo);
    auto oidField = [plo](uint32_t ib) { return static_cast<OID>(plo->U32Field(0, ib, 0xFFFFFFFFu)); };
    if (OID oid = oidField(0x1C0); oid != -1) w.ploTarget = PloFindSwNearest(plo, oid);
    if (OID oid = oidField(0x1C4); oid != -1)
    {
        LO* p = PloFindSwNearest(plo, oid);
        w.paloModel = p && FCidDerivedFrom(p->Cid(), 0x30) ? static_cast<ALO*>(p) : nullptr;
    }
    if (!h.ploBurst)
        if (OID oid = oidField(0x190); oid != -1) h.ploBurst = PloFindSwNearest(plo, oid);
    for (size_t i = 0; i < w.ae.size(); ++i)
    {
        SWENT& e = w.ae[i];
        e.ppath = PloFindSwNearest(plo, e.oidPath);  // 0x15E938(G, 4, oid, p)
        if (!e.ppath)
        {
            w.ae.resize(i);
            break;
        }
        for (int j = 0; j < e.cq; ++j)
        {
            w.aqAll.push_back(std::make_unique<SWQ>());
            SWQ* q = w.aqAll.back().get();
            q->ie = static_cast<int>(i);
            e.aq.push_back(q);
        }
    }
    ResetSwarm(plo);
    ALO* palo = Palo(plo);
    bool fLive = palo->fInWorld;
    for (const SOE& e : palo->asoe) fLive = fLive || e.fInOxa;
    if (fLive) RemoveAloSoes(palo);
    palo->asoe.emplace_back();
    SOE& e = palo->asoe.back();
    e.pxf = PxfObj(palo);
    e.palo = palo;
    e.tag = -1;
    e.f48 = true;
    e.f49 = true;
    if (fLive) AddAloSoes(palo);
    if (FPrint())
    {
        std::printf("hack swarm %d: post-load target %d model %d burst %d points %d entries", plo->oid, w.ploTarget ? w.ploTarget->oid : -1,
                    w.paloModel ? w.paloModel->oid : -1, h.ploBurst ? h.ploBurst->oid : -1, h.nPoints);
        for (const SWENT& x : w.ae) std::printf(" {%d x%d /%d r %.0f len %.0f}", x.oidPath, x.cq, x.cGroup, x.r, SLenEnt(x));
        std::printf("\n");
    }
}

// orig: 0x001821C0 (vt slot 19 of the swarms, after the ALO's draw 0x1F3C70: its model +0x1C4 at each live particle,
// facing along the path (0x11B830 with z up), moved sideways by its wobble when over 1, rolled when over 0.01)
void CollectHackSwarmDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra)
{
    if (!g_psw) return;
    for (LO* plo : g_psw->mpcidaplo[0x4C])
    {
        auto it = s_mpswarm.find(plo);
        if (it == s_mpswarm.end() || !it->second.paloModel) continue;
        ALO* palo = Palo(plo);
        if (static const bool s_f = std::getenv("SLY2N_SWARMDRAWPRINT") != nullptr; s_f)
            std::printf("swarmdraw %d inworld %d removed %d drawn %d model %d (sgs %d)\n", plo->oid, plo->fInWorld, plo->fRemoved,
                        FAloDrawn(g_psw, palo), it->second.paloModel->oid, it->second.paloModel->pglobset != nullptr);
        if (!plo->fInWorld || plo->fRemoved || !FAloDrawn(g_psw, palo)) continue;
        const glm::vec3 up(0.0f, 0.0f, 1.0f);  // D_002A2DC0
        for (SWENT& e : it->second.ae)
            for (SWQ* q : e.aq)
            {
                glm::vec3 pos = q->pos;
                glm::mat3 m = BuildLookAtMatrix(q->dir, up);
                if (1.0f < std::fabs(q->lat)) pos += glm::cross(up, q->dir) * q->lat;
                if (0.01f < std::fabs(q->roll)) m = m * MatRotateAxis(q->roll, up);
                glm::mat4 mm(m);
                mm[3] = glm::vec4(pos, 1.0f);
                AppendAloExtraDraws(scene, it->second.paloModel, mm, 1.0f, aextra);  // 0x1A5EE8(model, m, 1, 1)
            }
    }
}

void ResetHacks()
{
    s_mphack.clear();
    s_mpswarm.clear();
    s_ploShip = nullptr;
}

NATIVEFN(0x17FB50, MoveShip);
NATIVEFN(0x17F1A0, OnHackXpNew);
NATIVEFN(0x1803E0, OnPieceXpNew);
NATIVEFN(0x1803A8, OverlapKindPiece);
NATIVEFN(0x180EE8, MoveEnemy);
NATIVEFN(0x1811B8, FAcceptEnemyContact);
// orig: 0x0017F1D0 (vt slot 75 of the base, inherited by the pieces 0x4A and the docks 0x4B: the hit; audit H2)
bool FAcceptHackSoContact(ALO* palo, CTMSG* pctm) { return FHackSoHit(palo, pctm); }
NATIVEFN(0x17F1D0, FAcceptHackSoContact);
// orig: 0x0017F138 (post-load of the base and the pieces: the ALO's 0x1F36A8, then vt slot 83 0x17F6B0: the hit
// points to the maximum (a piece placed in the world or a pool's first row started at 0; audit H2))
void PostLoadHackSoBase(LO* plo)
{
    if (!FHackSo(plo)) return;
    ResetHackSoBase(plo, Hack(plo));
}
NATIVEFN(0x180FF0, OnEnemyXpNew);
NATIVEFN(0x17FBE8, FAcceptShipContact);
NATIVEFN(0x1823C8, AddSwarmXps);
NATIVEFN(0x182780, UpdateSwarmBounds);
NATIVEFN(0x182700, OnSwarmXpNew);
NATIVEFN(0x1828A8, FAcceptSwarmContact);

int HackShipState(LO* plo)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x48)) return -2;
    return Hack(plo).state;
}
