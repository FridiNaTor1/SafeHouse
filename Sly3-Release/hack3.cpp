// hack3.cpp: Sly 3's hacking minigame, its own functions (the module 0x19A800..0x19F800; the Sly 3 decomp, read-only,
// cited as orig3). The hack objects are Sly 3's classes 0x49..0x4E (HACKSO, HACKSHIP, HACKENEMY, HACKPIECE, HACKDOCK,
// HACKSWARM): Sly 2's 0x47..0x4C (hack.cpp) with new constants and a few new parts. hack.cpp keeps the parts that are
// the same C (the pieces, the enemies' rules, the docks, the swarms; their records, hack.h); this file holds the Sly 3
// functions that differ, registered by their Sly 3 addresses (NATIVEFN3 / RegisterLoUpdate(FN3_TAG | addr)), so they
// are found before the Sly 2 twin the function map pairs them with.
//
// The ship (cid 0x4A, vt 0x359C58): Bentley's twin-stick ship. A task's script (e.g. SCPT 3168 of Ep3's windmill
// hack 3176) adds it and makes it player 0's character (0x13): the PO's enable (vt slot 83 0x19BE90) puts it in
// state 1 (driven) when it has a controller, else 0. Sly 2's ship had no such enable (its focus slot did it), so the
// ship sat in state 0 at its start (5326 0 25000) and no hack task could be played.
//
// Its states (0x19C570): -1 out (no sound), 0 idle, 1 driven (the left stick's heading and deflection x 1000 the
// target velocity, the right stick turns the guns (snapped to +0x284 directions) and fires them), 2 hit (until its
// segment ends), 3 docked (held at the dock's point by two controllers), 4 held (no velocity, fixed), 5 falling (the
// gravity only), 6 dying (the death's segment, then the task event 0xC and out). Each state plays its segment
// (+0x1F8 x 7: optid 394 / EOPID 1563).

#include "hack.h"

#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "game/sly3.h"
#include "game/task3.h"
#include "gauge3.h"
#include "hackgame3.h"
#include "hackmouse.h"
#include "actioninput.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "routetele.h"
#include "smrun.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "update.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "worldglobals.h"
#include "ai.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

glm::mat3 MatRotateAxis(float rad, const glm::vec3& axis);  // xfmath.h (0x11B500; orig3 0x11C8C0)
void AccelAloPos(ALO* p, const glm::vec3& v);                // solve.cpp (0x1FAC30; orig3 0x244828)
glm::vec3 EulerFromMat(const glm::mat3& m);                   // xfmath.h (0x11BFF0; orig3 0x11D3C0)

namespace
{
using namespace hack;

constexpr float PI = 3.14159265358979f;

// the Sly 3 option's value (the loader keeps it as an OPT; the template's for a copy): the last one loaded, as the
// loader's stores leave it (a copy's list holds its template's options first, then its own: e.g. the Ep4 webcam's cage
// rings 78385 have 937 = 5 from the template and 25 of their own, the reference's +0x170 = 25)
bool FOpt3(const LO* plo, int eopid3, OPTVAL* pv)
{
    for (const LO* p = plo; p; p = p != p->ploCloneOf ? p->ploCloneOf : nullptr)
    {
        for (auto it = p->aopt.rbegin(); it != p->aopt.rend(); ++it)
            if (it->eopid == eopid3 && !it->aval.empty())
            {
                *pv = it->aval[0];
                return true;
            }
        if (!p->ploCloneOf) break;
    }
    return false;
}
int NOpt3(const LO* plo, int eopid3, int nDefault)
{
    OPTVAL v;
    if (!FOpt3(plo, eopid3, &v)) return nDefault;
    return v.k == 1 ? static_cast<int>(v.ag[0]) : v.n;
}
float GOpt3(const LO* plo, int eopid3, float gDefault)
{
    OPTVAL v;
    if (!FOpt3(plo, eopid3, &v)) return gDefault;
    return v.k == 1 || v.k == 10 ? v.ag[0] : static_cast<float>(v.n);
}

// orig3: 0x0011E3B8 (an angle into [-pi, pi))
float RadNormalize3(float rad)
{
    rad = std::fmod(rad + PI, 2.0f * PI);
    if (rad < 0.0f) rad += 2.0f * PI;
    return rad - PI;
}

// orig3: 0x0011E438 (an angle snapped to the nearest of c directions: 0 leaves it, 1 gives 0)
float RadQuantize3(float rad, int c)
{
    if (c == 0) return rad;
    if (c == 1) return 0.0f;
    rad = RadNormalize3(rad);
    const float spacing = 2.0f * PI / static_cast<float>(c);
    const float half = spacing * 0.5f;
    float sign = 1.0f;
    if (rad < 0.0f)
    {
        rad = -rad;
        sign = -1.0f;
    }
    float snapped = 0.0f;
    for (int i = 0; i <= c / 2; ++i)
    {
        snapped = spacing * static_cast<float>(i);
        if (snapped - half <= rad && rad <= snapped + half) break;
    }
    return snapped * sign;
}

// The Sly 3 fields of a hack object (the HACKSO record's own are Sly 2's, hack.h)
struct HACK3
{
    bool fInit = false;
    int nGroup = -1;            // +0x1D0 (EOPID 1557): its side (a piece of the ship's side does not hurt it)
    float gWeight = 0.0f;       // +0x1D4 (EOPID 1558, setter 0x19F618): its pull on the others' separation
    LM lmSep{20.0f, 1000.0f};   // +0x1D8 (EOPID 1559): the separation's range (0x19B420: 20, 1000)
    float gStrength = 0.0f;     // +0x1E0 (EOPID 1560): how strongly it is kept apart (0x19F6D8)
    // the ship
    int fAccelPos = 0;          // +0x280 (EOPID 1565): the velocity applied as an acceleration (0x19C370)
    int cDirGun = 0;            // +0x284 (EOPID 1566): the guns' directions (0x11E438)
    float radGun = 0.0f;        // +0x288: the guns' heading
    LM lmCam{1250.0f, 2500.0f}; // +0x310 (EOPID 1568; 0x19BDA0: 1250, 2500)
    // +0x30C (EOPID 1567, optid 1433): the other ship (the two-player game: h_hack's 93052 / 92990 name each other); with
    // one the ship's focus (0x19BF08) is the camera framing both (0x19F8B0 / 0x19F9C8, hackgame3.cpp)
    OID oidOther = -1;
    LO* ploOther = nullptr;
    bool fOtherSet = false;
    // the enemy (0x4B): +0x3F8 (EOPID 1584, get only): the sender of the last hit offered (0x19DBA8; the reset 0x19CED8
    // clears it)
    LO* ploHitBy = nullptr;
};
std::unordered_map<const LO*, HACK3> s_mphack3;
WORLD_TABLE(s_mphack3);

bool FPrint3()
{
    static const bool s_f = std::getenv("SLY2N_HACKPRINT") != nullptr;
    return s_f;
}

HACK3& Hack3(LO* plo)
{
    HACK3& h3 = s_mphack3[plo];
    if (!h3.fInit)
    {
        h3.fInit = true;
        h3.nGroup = NOpt3(plo, 1557, -1);
        h3.gWeight = GOpt3(plo, 1558, 0.0f);
        OPTVAL v;
        if (FOpt3(plo, 1559, &v)) h3.lmSep = LM{v.ag[0], v.ag[1]};
        h3.gStrength = GOpt3(plo, 1560, 0.0f);
        // (a spawned instance gets its field options as stores only: +0x1E0 the strength)
        if (h3.gStrength == 0.0f)
            for (const LO* p = plo; p; p = p->ploCloneOf != p ? p->ploCloneOf : nullptr)
                if (const uint32_t u = p->U32Field(0, 0x1E0, 0); u) { std::memcpy(&h3.gStrength, &u, 4); break; }
        h3.fAccelPos = NOpt3(plo, 1565, 0);
        h3.cDirGun = NOpt3(plo, 1566, 0);
        if (FOpt3(plo, 1568, &v)) h3.lmCam = LM{v.ag[0], v.ag[1]};
        if (FOpt3(plo, 1567, &v)) h3.oidOther = v.n;
    }
    return h3;
}

// the other ship (+0x30C): the script's own (seto 1433), else the loaded option's object
LO* PloOther3(LO* plo)
{
    HACK3& h3 = Hack3(plo);
    if (!h3.fOtherSet && !h3.ploOther && h3.oidOther > 0 && g_psw) h3.ploOther = PloFindSwNearest(plo, h3.oidOther);
    return h3.ploOther;
}

}  // namespace
int HackSide3(LO* plo) { return Hack3(plo).nGroup; }
void SetHackEnemyHitBy3(LO* plo, LO* ploBy)
{
    if (FHackSo(plo)) Hack3(plo).ploHitBy = ploBy;
}
LO* PloHackShipOther3(LO* plo) { return FHackSo(plo) ? PloOther3(plo) : nullptr; }
void SetHackShipOther3(LO* plo, LO* ploOther)
{
    if (!FHackSo(plo)) return;
    HACK3& h3 = Hack3(plo);
    h3.ploOther = ploOther;
    h3.fOtherSet = true;
}
LM LmHackShipCam3(LO* plo) { return Hack3(plo).lmCam; }

// orig3: the enemy's +0x200 (EOPID 1572, kind 0x2010: an object; Sly 2's +0x1D0 / EOPID 1378): the nav mesh its chase
// (0x19CF60 state 3: 0x1EC688(+0x200, self, target, the curve +0x204)) paths through. Its oid as the option's value.
// Natively the enemies read Sly 2's field only: no chaser had a mesh, the empty path aimed it at the world's origin
// (e.g. hack 4's claws 115511 sank off the floor, their balls with them)
// orig3: the hit points of a hack object: the base's init 0x19B420 sets 10, EOPID 937 (the SO's maximum, 0x1F43F8) its
// own; the reset (0x19B668 -> 0x1F43D0) fills them. Natively the enemies read Sly 2's field +0x180 (always the default
// 10): e.g. hack 4's claws died at the first shot
int NHackHpMax3(const LO* plo, int nDefault) { return NOpt3(plo, 937, nDefault); }

bool FHackHp3(LO* plo, bool fMax, int* pn)
{
    if (!FHackSo(plo)) return false;
    const HACKSO& h = Hack(plo);
    *pn = fMax ? h.cHpMax : std::max(h.cHp, 0);
    return true;
}
// orig3: 0x1F43D0 (the hit points, clamped to 0..max) / 0x1F43F8 (the maximum, then the hit points clamped again)
// orig3: 0x1FC868's vt slot 31 at the world start (guardstart.cpp SendStartEvents3) for the hack objects: the base's
// 0x19B668 (the hit points to the maximum, 0x1F43D0(p, 0x1F43F0(p))). The loader's 937 (0x1F43F8) only clamps the hit
// points to the new maximum: e.g. the webcam's rings (937 = 5 then 25) kept 5 until this filled them to 25 as the
// reference's at the load's end; natively a ring broke at one shot (3 in the reference)
void StartHackSo3(LO* plo)
{
    if (!FHackSo(plo)) return;
    HACKSO& h = Hack(plo);
    h.cHp = h.cHpMax;
}

bool FSetHackHp3(LO* plo, bool fMax, int n)
{
    if (!FHackSo(plo)) return false;
    HACKSO& h = Hack(plo);
    if (fMax) h.cHpMax = n;
    h.cHp = std::clamp(fMax ? h.cHp : n, 0, std::max(h.cHpMax, 0));
    return true;
}

// the enemy's nav mesh (orig3 +0x200, EOPID 1572): the field as the options stored it (the spawned instances, e.g. hack
// 4's chasers 78461 / 92977, get it only as a field store: mesh 74565, as the reference's +0x200), else the option
LO* PloEnemyNav3(LO* plo)
{
    for (const LO* p = plo; p; p = p->ploCloneOf != p ? p->ploCloneOf : nullptr)
        if (const uint32_t oid = p->U32Field(0, 0x200, 0xFFFFFFFFu); oid != 0xFFFFFFFFu && oid != 0)
            return PloFindSwNearest(plo, static_cast<int>(oid));
    OPTVAL v;
    if (!FOpt3(plo, 1572, &v) || v.n == -1 || v.n == 0) return nullptr;
    return PloFindSwNearest(plo, v.n);
}
namespace
{
using namespace hack;

// orig3: 0x0019F6D8 (vt slot 74 of the hack objects; the ship's states 0 / 1 / 6): pushed in the plane toward the
// others of the world's hack list (SW +0x4908's third list: the objects with a weight +0x1D4, 0x19F5D8) of another
// side, each by its weight x clamp(1 - u, 0, 1), u its distance in its range (+0x1D8 / +0x1DC), the sum x +0x1E0
// limited to 4500 (0x1205F0), an acceleration (0x244828)
void SeparateHackSo3(ALO* palo)
{
    HACK3& h3 = Hack3(palo);
    if (h3.gStrength <= 0.0f || !g_psw) return;
    const glm::vec3 pos = PosAloCenter(palo);
    glm::vec3 acc(0.0f);
    for (int cid = S3CID(0x47); cid <= S3CID(0x4C); ++cid)
        for (LO* plo : g_psw->mpcidaplo[cid])
        {
            if (plo == palo || !plo->fInWorld) continue;
            HACK3& o = Hack3(plo);
            if (o.gWeight == 0.0f || o.nGroup == h3.nGroup) continue;
            glm::vec3 d = PosAloCenter(Palo(plo)) - pos;
            d.z = 0.0f;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y);
            d = dist < 0.0001f ? glm::vec3(0.0f) : d / dist;
            const float u = o.lmSep.gMin == o.lmSep.gMax ? 0.0f : (dist - o.lmSep.gMin) / (o.lmSep.gMax - o.lmSep.gMin);
            const float w = std::clamp(1.0f - u, 0.0f, 1.0f);  // 0x11EE68 with D_00341FF0 {0, 1}
            acc += d * (h3.gStrength * o.gWeight * w);
        }
    const float s = std::sqrt(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);
    if (s > 4500.0f) acc *= 4500.0f / s;
    AccelAloPos(palo, acc);
}

void SetShipState3(LO* plo, int n);

}  // namespace

bool FHackShip3(const LO* plo) { return FShip(plo); }

// orig3: 0x0019D898's first call (the enemies' vt slot 74 starts with the separation 0x19F6D8; Sly 2's 0x180EE8 had none)
void SeparateEnemy3(ALO* palo) { SeparateHackSo3(palo); }

int HackShip3State(const LO* plo) { return FShip(plo) ? Hack(const_cast<LO*>(plo)).state : -2; }

namespace
{
// orig3: 0x0019C570 (the ship's state)
void SetShipState3(LO* plo, int n)
{
    HACKSO& h = Hack(plo);
    const int old = h.state;
    if (n == old) return;
    ALO* palo = Palo(plo);
    switch (old)
    {
    case -1:
        // the engine's loop +0x304 made (0x2459F8, no source 0x244F08, kept 0x2BBCD8) and paused (0x244C28)
        if (h.idSound != -1)
        {
            CSnd* psnd = PsndNewPlain(h.idSound);
            psnd->SetSource(nullptr);
            h.sndEngine = CSndRef(psnd);
            h.sndEngine->Pause();
        }
        break;
    case 2:
    case 6:
        // 0x240DB0: 0x25DEF8(SW, p) (Sly 2's 0x15E650 family: the object's pending finds; nothing kept natively)
        break;
    case 3:
        // the docking's controllers removed (0x1309D8), the records back in mode 2 (0x28F068), out of the dock's parent
        // keeping its place (0x1C7A00(p, 0, 15)), the constraints 5 / 4, no sensors, the player's pad lock let go
        // (0x1A94F0: natively the pad is the ship's while docked: nothing kept), no dock
        DeleteCtrl(reinterpret_cast<CTRL**>(&h.pctrlDockPos));
        DeleteCtrl(reinterpret_cast<CTRL**>(&h.pctrlDockRot));
        if (XF* p = PxfPos(palo)) SetXfMode(palo, p, 2);
        if (XF* p = PxfRot(palo)) SetXfMode(palo, p, 2);
        SetAloParent(palo, nullptr, nullptr, 0xF);
        SetAloCnstrLin(palo, 5);
        SetAloCnstrAng(palo, 4);
        SetAloSensors(palo, false);
        h.ploDock = nullptr;
        break;
    case 4:
        SetAloCnstrLin(palo, 5);
        SetAloCnstrAng(palo, 4);
        break;
    case 5: SetAloCnstrLin(palo, 5); break;
    case 1:
        // the guns stop firing: their SMAs to 0x84 (0x239CE8; Sly 2's 0x66)
        for (int i = 0; i < h.cgun; ++i)
            if (h.agun[i].psma) SetSmaGoal(h.agun[i].psma, 0x84);
        break;
    default: break;
    }
    h.state = n;
    h.tState = g_clock.t;
    switch (n)
    {
    case -1:
        if (h.sndEngine)
        {
            h.sndEngine->Stop(0.0f);
            h.sndEngine = CSndRef();
        }
        break;
    case 3:
        if (LO* pdock = h.ploDock)
        {
            // the dock point's vt slot 37 (its record and its matrix in it), the matrix into the position record's
            // frame (0x28EC08), the ship under the dock point's parent (vt slot 10, grf 3), still (vt slots 40 / 41),
            // held by two controllers (0x130078: priority 1, the position and the Euler angles 0x11D3C0)
            XF* pxfDock = nullptr;
            glm::mat4 m(1.0f);
            if (FCidDerivedFrom(pdock->Cid(), S3CID(0x30))) pxfDock = PxfObj(static_cast<ALO*>(pdock));
            else if (FCidDerivedFrom(pdock->Cid(), S3CID(3)))
            {
                pxfDock = static_cast<LOX*>(pdock)->pxf;
                m = static_cast<LOX*>(pdock)->mat;
            }
            XF* pxfFrom = palo->axf.empty() ? nullptr : palo->axf[0]->pxfRef;
            XF* pxfTo = PxfPos(palo) ? PxfPos(palo)->pxfRef : nullptr;
            if (pxfFrom) m = pxfFrom->matWorld * m;
            if (pxfTo) m = glm::inverse(pxfTo->matWorld) * m;
            using PFNVTPARENT = void (*)(LO*, ALO*, XF*, int);
            if (PFNVTPARENT pfn = PfnVt<PFNVTPARENT>(plo, 10)) pfn(plo, pdock->paloParent, pxfDock, 3);
            else SetAloParent(palo, pdock->paloParent, pxfDock, 3);
            SetAloVelocityTo(palo, glm::vec3(0.0f));
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));
            if (XF* p = PxfPos(palo)) h.pctrlDockPos = PctrlNewPos(palo, p, 1, 7, 2, glm::vec4(glm::vec3(m[3]), 0.0f));
            if (XF* p = PxfRot(palo)) h.pctrlDockRot = PctrlNewPos(palo, p, 1, 7, 2, glm::vec4(EulerFromMat(glm::mat3(m)), 0.0f));
            SetAloCnstrLin(palo, 1);
            SetAloCnstrAng(palo, 1);
            SetAloSensors(palo, true);
        }
        break;
    case 4:
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        SetAloCnstrLin(palo, 1);
        SetAloCnstrAng(palo, 1);
        break;
    case 5:
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        SetAloCnstrLin(palo, 4);
        break;
    default: break;
    }
    // 0x19BA10: the state's segment (+0x1F8 + 16 x state; none for -1)
    PlayHackSeg(plo, h, n < 0 || n >= HACKSO::CSEG ? nullptr : &h.aseg[n]);
    if (FPrint3()) std::printf("hack3 ship %d: state %d -> %d at t %.3f\n", plo->oid, old, n, g_clock.t);
    if (FPrint3() && n == 1)
        for (int cid = S3CID(0x49); cid <= S3CID(0x4C); ++cid)
            for (LO* p : g_psw->mpcidaplo[cid])
                if (p->fInWorld)
                {
                    const HACKSO& ho = Hack(p);
                    const glm::vec3 v = PosAloCenter(Palo(p));
                    const glm::vec3 vt = ho.ploEnemyTarget ? PosLoWorld(ho.ploEnemyTarget) : glm::vec3(0.0f);
                    std::printf("hack3 level: %d cid 0x%X state %d hp %d at %.0f %.0f %.0f target %d %.0f %.0f\n", p->oid, p->Cid(), ho.state,
                                ho.cHp, v.x, v.y, v.z, ho.ploEnemyTarget ? ho.ploEnemyTarget->oid : -1, vt.x, vt.y);
                }
}

// orig3: 0x0019C0B0 (vt slot 18)
void UpdateShip3(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // 0x23C660 (the ALO's)
    hackgame3::OnShipUpdate(plo);   // (the two-player game: its camera, colours, chained power-ups)
    HACKSO& h = Hack(plo);
    HACK3& h3 = Hack3(plo);
    int s;
    do
    {
        s = h.state;
        if (s == 2 && FShipSegmentDone(plo)) SetShipState3(plo, 1);
        else if (s == 6 && FShipSegmentDone(plo))
        {
            // dead: the task event 0xC (0x1854C8), out of the world (vt slot 9)
            Broadcast3(0xC, plo);
            RemoveLo(g_psw, plo);
            return;
        }
    } while (h.state != s);
    if (h.state != 1) h.vTarget = glm::vec3(0.0f);
    float uDrive = 0.0f;
    if (h.state == 1)
        if (const PADCTL* ppc = Ppadctl(plo))
        {
            // 0x168108: the left stick's heading in the world (the camera's) while deflected, else kept; the velocity
            // its deflection (+0x134's g8) x 1000 that way (0x11FD70)
            if (0.0f < ppc->uJoy) h.radHeading = ppc->radJoy;
            uDrive = ppc->uJoy;
            h.vTarget = cmm::SetVectorCylind(h.radHeading, ppc->uJoy * 1000.0f, 0.0f);
            // the guns: the right stick's heading (kept while centred), snapped to +0x284 directions; firing while it is
            // deflected (+0x134's g14 > 0: the SMAs to 0x86, else 0x84). QoL (hackmouse.h): the cursor's heading with the
            // keyboard / mouse, firing while left click is held; the remapped fire action.
            float radAim = h3.radGun;
            if (0.0f < ppc->uJoy2) radAim = ppc->radJoy2;
            bool fFire = sly2actions::Applies(ppc) ? sly2actions::Held(sly2actions::Action::HackFire) : 0.0f < ppc->uJoy2;
            float radMouse = 0.0f;
            bool fMouse = false;
            if (FHackMouseAim(plo, &radMouse, &fMouse) && (fMouse || !fFire))
            {
                radAim = radMouse;
                fFire = fMouse;
            }
            h3.radGun = RadQuantize3(radAim, h3.cDirGun);
            const glm::vec4 rot(0.0f, 0.0f, h3.radGun, 0.0f);
            for (int i = 0; i < h.cgun; ++i)
            {
                HACKGUN& g = h.agun[i];
                if (g.pctrl) SetCtrlPos(g.pctrl, rot);                      // 0x12F4F8
                if (g.psma) SetSmaGoal(g.psma, fFire ? 0x86 : 0x84);         // 0x239CE8
            }
        }
    DumpHackMapOnce(plo, h.state);
    // (debug: a route's "hackdock X Y" step: the ship put there once driven)
    if (g_routehackdock.fPending && h.state >= 1)
    {
        g_routehackdock.fPending = false;
        ALO* palo = Palo(plo);
        if (std::isnan(g_routehackdock.x))  // (auto: the waiting in-world dock's point farthest from the ship)
        {
            float dMax = -1.0f;
            const glm::vec3 p0 = PosAloCenter(palo);
            for (LO* pd : g_psw->mpcidaplo[S3CID(0x4B)])
            {
                if (!pd->fInWorld || Hack(pd).state != 0 || !Hack(pd).ploEnemyTarget) continue;
                const glm::vec3 v = PosLoWorld(Hack(pd).ploEnemyTarget);
                const float d = (v.x - p0.x) * (v.x - p0.x) + (v.y - p0.y) * (v.y - p0.y);
                if (d > dMax)
                {
                    dMax = d;
                    g_routehackdock.x = v.x;
                    g_routehackdock.y = v.y;
                }
            }
            if (dMax < 0.0f) g_routehackdock.x = p0.x, g_routehackdock.y = p0.y;
        }
        SetAloPos(palo, glm::vec3(g_routehackdock.x, g_routehackdock.y, PosAloCenter(palo).z));
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        std::printf("hack3 ship %d: put by the route at %.0f %.0f at t %.3f\n", plo->oid, g_routehackdock.x, g_routehackdock.y, g_clock.t);
    }
    // the engine's loop: its volume smoothed toward the deflection while driven (0x11E598 with D_00321F50 {1, 0, 0.5}),
    // heard while above 0
    if (h.sndEngine)
    {
        static const cmm::SMP s_smp{1.0f, 0.0f, 0.5f};
        h.uSound = cmm::GSmooth(h.uSound, uDrive, g_clock.dt, &s_smp, nullptr);
        h.sndEngine->SetVolume(h.uSound);
        if (0.0f < h.uSound)
        {
            if (h.sndEngine->m_cpause > 0) h.sndEngine->Resume();
        }
        else if (h.sndEngine->m_cpause <= 0) h.sndEngine->Pause();
    }
    if (FPrint3() && (static_cast<int>(g_clock.t * 60.0f) % 30 == 0))
    {
        const glm::vec3 pos = PosAloCenter(Palo(plo));
        std::printf("hack3 ship %d t %.3f: state %d hp %d/%d heading %.2f v %.0f %.0f gun %.2f pos %.0f %.0f %.0f\n", plo->oid, g_clock.t,
                    h.state, h.cHp, h.cHpMax, h.radHeading, h.vTarget.x, h.vTarget.y, h3.radGun, pos.x, pos.y, pos.z);
    }
}

// orig3: 0x0019C370 (vt slot 74): falling (5): the gravity only; docked / held (3 / 4): nothing; else (not hit) kept
// apart from the others (0x19F6D8), then turned toward the heading (0x23EDE8 with D_00321F80 {25, 0, 0.25}) and
// moved toward the target velocity (0x23F180 with D_00321FA0 {0, 5, 0}), or with +0x280 the target velocity as an
// acceleration (0x244828) braked toward still (D_00321F90 {0, 0.1, 0})
void MoveShip3(ALO* palo)
{
    HACKSO& h = Hack(palo);
    const int s = h.state;
    if (s == 5)
    {
        ApplyAloGravity(palo);
        return;
    }
    if (s == 3 || s == 4) return;
    if (s != 2) SeparateHackSo3(palo);
    static const float s_acTurn[3] = {25.0f, 0.0f, 0.25f};
    BrakeAloRotation(palo, MatRotateAxis(h.radHeading, glm::vec3(0.0f, 0.0f, 1.0f)), s_acTurn);
    if (Hack3(palo).fAccelPos)
    {
        AccelAloPos(palo, h.vTarget);
        static const CLQ s_clqStill{0.0f, 0.1f, 0.0f, 0.0f};
        AccelAloTowardPosition(palo, glm::vec3(0.0f), nullptr, glm::vec3(0.0f), &s_clqStill);
    }
    else
    {
        static const CLQ s_clqV{0.0f, 5.0f, 0.0f, 0.0f};
        AccelAloTowardPosition(palo, glm::vec3(0.0f), nullptr, h.vTarget, &s_clqV);
    }
}
NATIVEFN3(0x19C370, MoveShip3);

// orig3: 0x0019CC40 (vt slot 65 of the pieces, cid 0x4C): no overlap with its shooter's attachment (+0x1F8's +0xDC; Sly 2's
// 0x1803A8) nor with another piece (new in Sly 3), else the ALO's (0x23E178). Natively the shots met the ship's own
// collision as they left the gun and died there (a piece's slot 65 had no twin)
int OverlapKindPiece3(ALO* palo, ALO* pOther)
{
    HACKSO& h = Hack(palo);
    if (h.ploShooter && FCidDerivedFrom(h.ploShooter->Cid(), S3CID(0x30)) && Sort(pOther).poxa &&
        Sort(pOther).poxa == Sort(static_cast<ALO*>(h.ploShooter)).poxa)
        return 3;
    if (pOther && FCidDerivedFrom(pOther->Cid(), S3CID(0x4A))) return 3;
    using PFN = int (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8))) return pfn(palo, pOther);
    return 0;
}
NATIVEFN3(0x19CC40, OverlapKindPiece3);

// orig3: 0x0019F6D8 (vt slot 74 of the other hack objects: the separation alone)
void MoveHackSo3(ALO* palo) { SeparateHackSo3(palo); }
NATIVEFN3(0x19F6D8, MoveHackSo3);

// orig3: 0x0019C478 (vt slot 78 of the ship): not while hit, docked, held, falling or dying; not from a piece of its own
// side (cid 0x4C with the same +0x1D0); a hit (0x19B6E0): with hit points left the task event 0xA and state 2 (none
// left: the death 0x19BB78 already gave state 6)
bool FAcceptShipContact3(ALO* palo, CTMSG* pctm)
{
    HACKSO& h = Hack(palo);
    if (2 <= h.state && h.state < 7) return false;
    if (0 < g_gamepause.c78) return false;  // (the vt slot 80 test of the sender: Sly 2's pause test kept)
    if (static const bool s_fGod = std::getenv("SLY2N_HACKGOD") != nullptr; s_fGod) return false;
    // (0x13B500(source, 0x4C): a piece of the ship's own side (+0x1D0, the scripts' optid 1426) is let through: the
    // two-player game's partner shots; natively the test named the ship class, so a partner's shots hurt)
    // (the contact's source object: a piece's own or one of its parts, e.g. the two-player shots' 93011 (cid 0x61) under
    // their piece)
    LO* pfrom = pctm->ploSender;
    if (FPrint3() && pfrom)
        std::printf("hack3 ship %d (%p) contact from %d cid 0x%X parent %d (%p) cid 0x%X group %d / ship %d\n", palo->oid, (void*)palo, pfrom->oid, pfrom->Cid(),
                    pfrom->paloParent ? pfrom->paloParent->oid : -1, (void*)pfrom->paloParent, pfrom->paloParent ? pfrom->paloParent->Cid() : -1, Hack3(pfrom).nGroup, Hack3(palo).nGroup);
    // (the ship's own part: the arrows' burst (power-up 5: the ship's s12 starts ASEG 93033 on it when a partner's shot
    // hits; its barrier 93011, a child of the ship and its own physics root, sends a contact of 10 to every object it
    // sweeps, the ship too). The original's ship takes no damage from it (the power-up's partner shots do none); natively the
    // ship lost 10 to its own burst (not traced in the reference which of the original's paths keeps it out: the
    // ship's own parts are left out here)
    // (PC, three / four players: the ships start side by side, so another player's arrows ring sweeps its neighbour: the
    // rings of the ships of the ship's own side are left out too; two players: the original's)
    for (LO* p = pfrom; p; p = p->paloParent)
        if (p == palo || (p != pfrom && FHackShip3(p) && Hack3(p).nGroup == Hack3(palo).nGroup && hackgame3::CShips() >= 3 && hackgame3::FActive(g_psw)))
            return false;
    while (pfrom && pfrom->Cid() != S3CID(0x4C) && pfrom->Cid() != S3CID(0x4A) && pfrom->paloParent) pfrom = pfrom->paloParent;
    if (pfrom && (pfrom->Cid() == S3CID(0x4A) || pfrom->Cid() == S3CID(0x4C)) && Hack3(pfrom).nGroup == Hack3(palo).nGroup)
        return false;
    if (!FHackSoHit(palo, pctm)) return false;
    if (h.cHp > 0)
    {
        Broadcast3(0xA, palo);
        SetShipState3(palo, 2);
    }
    return true;
}
NATIVEFN3(0x19C478, FAcceptShipContact3);

// orig3: 0x0019C550 (vt slot 93 of the ship, the end of the death 0x19BB78): dying (state 6)
void DieShip3(LO* plo) { SetShipState3(plo, 6); }
NATIVEFN3(0x19C550, DieShip3);

// orig3: 0x0019BED8 (vt slot 32: out of the world): the base's (0x1F3C80), then state -1
void OnRemoveShip3(LO* plo)
{
    Hack(plo).fDead = 0;
    SetShipState3(plo, -1);
}

// orig3: 0x0019BE58 (vt slot 31, a pool row taken again): the base's (0x19B668: the hit points to the maximum), then
// state 1 with a controller, else 0
void ResetShipSlot31_3(LO* plo, LO*)
{
    HACKSO& h = Hack(plo);
    h.cHp = h.cHpMax;
    SetShipState3(plo, NPoEnabled(plo) == 1 ? 1 : 0);
}
NATIVEFN3(0x19BE58, ResetShipSlot31_3);

// orig3: 0x0019BE08 (vt slot 35, after the ALO's 0x23D338): the heading and the guns' from the new orientation
void SetShipHeading3(LO* plo)
{
    ALO* palo = Palo(plo);
    if (XF* pxf = PxfObj(palo))
    {
        const glm::vec3 x = glm::vec3(pxf->matWorld[0]);
        const float rad = std::atan2(x.y, x.x);
        Hack(plo).radHeading = rad;
        Hack3(plo).radGun = rad;
    }
}

// orig3: 0x0019BE08 (vt slot 35 of the ship, Sly 2's slot 30: a new orientation): the ALO's (0x23D338 = so.cpp
// SetAloRotation), then the heading +0x268 and the guns' +0x288 from the object's x axis (vt slot 36). E.g. a script
// turning the ship at a hack's start: natively the slot had no function (the "[vt] no native function" warning) and the
// ship kept steering and aiming by its old heading
void RotateShip3(ALO* palo, const glm::mat3& basis)
{
    SetAloRotation(palo, basis);
    if (FShip(palo)) SetShipHeading3(palo);
}
NATIVEFN3(0x19BE08, RotateShip3);

}  // namespace

// orig3: 0x0019BE90 (vt slot 83 of the ship: the PO's enable 0x1F4548, then, in the world (not state -1), state 1
// with a controller, else 0). po.cpp EnablePo calls it after the controller state changed.
void EnableHackShip3(LO* plo, int n)
{
    if (!FShip(plo)) return;
    HACKSO& h = Hack(plo);
    if (h.state != -1) SetShipState3(plo, n == 1 ? 1 : 0);
}

// orig3: 0x0019C048 (vt slot 17, the ship's post-load: the PO's 0x1F3BF8, the docking records' mode 2 (0x28F068 on
// +0x48 / +0x4C), the guns (0x19BF70), the 7 segments (0x19B968)); its init 0x19BDA0 (state -1, no guns, sound -1,
// the camera range 1250..2500); then out of the world's state (-1 stays until its add: 0x19BE58 / the enable)
void PostLoadHackShip3(LO* plo)
{
    if (!FShip(plo)) return;
    HACKSO& h = Hack(plo);
    if (h.fPostLoaded) return;
    h.fPostLoaded = true;
    s_ploShip = plo;
    LoadHackFields(plo, h);
    Hack3(plo);
    // the hit points: the SO's (EOPID 937: the maximum, 0x1F43F8; the base's 0x19B420 sets 10)
    h.cHpMax = NOpt3(plo, 937, 10);
    h.cHp = h.cHpMax;
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
    if (XF* p = PxfPos(Palo(plo))) SetXfMode(Palo(plo), p, 2);
    if (XF* p = PxfRot(Palo(plo))) SetXfMode(Palo(plo), p, 2);
    BindShipGuns(plo, h);
    BindHackSegs(plo, h);
    // (0x19BDA0 leaves -1; the world's start adds it (0x19BE58): state 0, or 1 with a controller)
    SetShipState3(plo, NPoEnabled(plo) == 1 ? 1 : 0);
    if (FPrint3())
    {
        const HACK3& h3 = Hack3(plo);
        std::printf("hack3 ship %d: post-load hp %d guns %d (part %d sm %d) segs", plo->oid, h.cHp, h.cgun, h.cgun ? h.agun[0].oidPart : -1,
                    h.cgun ? h.agun[0].oidSm : -1);
        for (const HACKSEG& e : h.aseg) std::printf(" %d%s", e.oid, e.paseg ? "a" : e.psm ? "s" : "");
        std::printf(" group %d strength %.1f accel %d dirs %d sound %d\n", h3.nGroup, h3.gStrength, h3.fAccelPos, h3.cDirGun, h.idSound);
    }
}

void OnRemoveHackShip3(LO* plo)
{
    if (FShip(plo)) OnRemoveShip3(plo);
}

void SetHackShip3State(LO* plo, int n)
{
    if (FShip(plo)) SetShipState3(plo, n);
}

void OnHackShip3Moved(LO* plo)
{
    if (FShip(plo)) SetShipHeading3(plo);
}

void ResetHacks3() { s_mphack3.clear(); }

namespace
{
struct INITHACK3
{
    INITHACK3()
    {
        RegisterLoUpdate(FN3_TAG | 0x19C0B0, UpdateShip3);
        // the enemies' flags (cid 0x4B): EOPID 1581 (optid 1444) +0x1FC busy (no rules, no new target: e.g. a spawner's
        // enemy while it comes out, block 78434's s4354 / s4355), 1582 (1445) +0x1F8 the cool-down, 1583 (1446) +0x1CC
        // the target kept
        // the ship's gauge (its child, class 0xA3: gauge3.cpp) on its hit points (0x1F43C8 / 0x1F43F0: the SO's +0x16C
        // and its most, EOPID 937); no gadget power
        static bool (*s_pfnPrev)(LO*, GAUGEVALUES3*) = g_pfnGaugeValues3;
        g_pfnGaugeValues3 = [](LO* plo, GAUGEVALUES3* pv) {
            if (!plo || !FShip(plo)) return s_pfnPrev(plo, pv);
            const HACKSO& h = Hack(plo);
            pv->cHit = std::max(h.cHp, 0);
            pv->cHitMax = h.cHpMax;
            pv->nPower = pv->nPowerMax = 0;
            return true;
        };
#define HACK3FLAG(eopid, field) \
        RegisterObjectGetter3(eopid, [](LO* plo) { CRef r; r.SetBool(FHackSo(plo) && Hack(plo).field); return r; }); \
        RegisterObjectSetter3(eopid, [](LO* plo, const CRef& ref) { \
            if (FHackSo(plo)) Hack(plo).field = ref.m_tagk == TAGK_Bool ? ref.m_bool != 0 : ref.m_n != 0; \
            if (FPrint3()) std::printf("hack3 %d: EOPID %d = %d at t %.3f\n", plo ? plo->oid : -1, eopid, FHackSo(plo) ? int(Hack(plo).field) : -1, g_clock.t); \
        })
        HACK3FLAG(1581, fBusy);
        HACK3FLAG(1582, fCooldown);
        HACK3FLAG(1583, fKeepTarget);
#undef HACK3FLAG
        RegisterObjectGetter3(1584, [](LO* plo) {
            CRef r;
            if (LO* p = FHackSo(plo) ? Hack3(plo).ploHitBy : nullptr) r.SetBasic(p);
            return r;
        });
        // EOPID 1557 (optid 1426, +0x1D0): the side, set by the scripts too (the two-player ship's s5103 gives each shot
        // its ship's side)
        RegisterObjectGetter3(1557, [](LO* plo) { CRef r; r.SetS32(FHackSo(plo) ? Hack3(plo).nGroup : -1); return r; });
        RegisterObjectSetter3(1557, [](LO* plo, const CRef& ref) {
            if (!FHackSo(plo)) return;
            Hack3(plo).nGroup = ref.m_tagk == TAGK_Basic ? (ref.m_pbasic ? dynamic_cast<LO*>(ref.m_pbasic)->oid : -1) : ref.m_n;
        });
        // EOPID 1567 (optid 1433, the ship's +0x30C): the other ship (h_hack's two-player game: the power-ups given to
        // both (its s5101 cases 4 / 5), the laser's far end (case 3), the camera framing both (0x19BF08))
        RegisterObjectGetter3(1567, [](LO* plo) {
            CRef r;
            if (LO* p = FHackSo(plo) ? PloOther3(plo) : nullptr) r.SetBasic(p);
            return r;
        });
        RegisterObjectSetter3(1567, [](LO* plo, const CRef& ref) {
            SetHackShipOther3(plo, ref.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(ref.m_pbasic) : nullptr);
        });
        // EOPID 1568 (optid 1434, +0x310): the camera's height range (the two-player game's script: 1250 .. 2500, the
        // last round 1250 .. 3500)
        RegisterObjectGetter3(1568, [](LO* plo) {
            CRef r;
            const LM lm = FHackSo(plo) ? Hack3(plo).lmCam : LM{1250.0f, 2500.0f};
            r.m_tagk = TAGK_Lm;
            r.m_vector = glm::vec4(lm.gMin, lm.gMax, 0.0f, 0.0f);
            return r;
        });
        RegisterObjectSetter3(1568, [](LO* plo, const CRef& ref) {
            if (!FHackSo(plo)) return;
            Hack3(plo).lmCam = LM{ref.m_vector.x, ref.m_vector.y};
            if (FPrint3()) std::printf("hack3 %d: camera range %.0f .. %.0f at t %.3f\n", plo->oid, ref.m_vector.x, ref.m_vector.y, g_clock.t);
        });
        // EOPID 1561 (the ship's +0x1F0, setter 0x19C570): its state, set by the scripts
        RegisterObjectGetter3(1561, [](LO* plo) {
            CRef r;
            r.SetS32(FShip(plo) ? Hack(plo).state : -1);
            return r;
        });
        RegisterObjectSetter3(1561, [](LO* plo, const CRef& ref) {
            if (!FShip(plo)) return;
            const int n = ref.m_tagk == TAGK_F32 ? static_cast<int>(ref.m_g) : ref.m_n;
            if (FPrint3()) std::printf("hack3 ship %d: state %d set (EOPID 1561) at t %.3f\n", plo->oid, n, g_clock.t);
            SetShipState3(plo, n);
        });
    }
} s_inithack3;
}  // namespace
