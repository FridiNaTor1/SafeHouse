// bird.cpp: class 0x60 (module 0x1B0B78..0x1B1C50; the name is unknown: the boot world's seagulls 66949 and Elephant
// Rampage's crawl-space rats 118626 (world 8) use it): an ALO that rests, walks along a path (+0x16C), turns at the path's ends, faces the
// player who comes near and flies off along a second path (+0x170), fading out. Hit (a contact with bytes 8) it
// bursts (+0x1C8), drops its coins and is removed. Each state plays its own segment (actanim.h STEPSEGS: +0x17C
// {id, ASEG, grf} x 6, the playing ASEGA +0x1C4).
//
// Options: 965 (method 0x10C5F0 -> 0x1B1B18: a state's segment), fields 966 +0x16C / 967 +0x170 (the paths' oids),
// 968 +0x1C8 (the burst), 969 / 970 +0x150 / +0x154 (the radii), 971 +0x158 (the rest's time range), 972 +0x160
// (the walk's), 973 +0x168 (the fade's time), 974 setter 0x1B17B8 (the turn rate's ratio), 975 setter 0x1B1328 (the
// state), 976 +0x230 (the coins), 977 +0x240 (their place).
#include "jt.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "actanim.h"
#include "activate.h"
#include "alo.h"
#include "aseg.h"
#include "asega.h"
#include "cls.h"
#include "clock.h"
#include "crv.h"
#include "emitter.h"
#include "lo.h"
#include "objmethod.h"
#include "pickup.h"
#include "po.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "xf.h"
#include "animfind.h"
#include "worldaccess.h"

#include <glm/glm.hpp>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

glm::mat3 MatRotateAxis(float rad, const glm::vec3& axis);    // xfmath.h (0x11B500)
float GRandInRange(float gLow, float gHigh);                  // xfmath.h (0x11D448)

namespace
{
// a float field option (the loader's options by offset, as anim.cpp's)
float FloatField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t uDefault;
    std::memcpy(&uDefault, &gDefault, 4);
    const uint32_t u = plo->U32Field(0, ib, uDefault);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

struct BIRD
{
    float sNear = 400.0f;    // +0x150: within it the player is seen whatever his speed
    float sFar = 800.0f;     // +0x154: within it, when he moves at 150 or more
    float dtRestMin = 2.0f, dtRestMax = 6.0f;   // +0x158
    float dtWalkMin = 6.0f, dtWalkMax = 10.0f;  // +0x160
    float dtFade = 1.0f;     // +0x168
    LO* ppathWalk = nullptr; // +0x16C
    LO* ppathFly = nullptr;  // +0x170
    float gPath = 0.0f;      // +0x174 the place on the path (its parameter)
    float dgPath = 0.0f;     // +0x178 the step ahead (signed: the walk's way)
    STEPSEGS segs;           // +0x17C .. +0x1C4
    LO* pexpl = nullptr;     // +0x1C8
    float svWalk = 300.0f;   // +0x1CC
    float svFly = 1000.0f;   // +0x1D0
    int modeRot = 0;         // +0x1D4 the rotation record's mode at the load
    float radHeading = 0.0f; // +0x1D8
    glm::mat3 matHeading{1.0f};  // +0x1E0
    float acTurn[3] = {25.0f, 0.0f, 0.25f};  // +0x210 (D_002C0248 x the ratio)
    float rTurn = 1.0f;      // +0x21C
    int state = -1;          // +0x220
    float tState = 0.0f;     // +0x224
    float tNext = 0.0f;      // +0x228
    float tFade = FLT_MAX;   // +0x22C
    int ccoin = -1;          // +0x230
    glm::vec3 posCoin{0.0f}; // +0x240 (the object's frame)
    bool fStarted = false;
    bool fPaths = false;     // (the paths' oids below found once)
    uint32_t oidWalk = 0xFFFFFFFFu, oidFly = 0xFFFFFFFFu;
    int cPhys = 0;  // (debug: the physics updates)
};
std::unordered_map<const LO*, BIRD> s_mpbird;
WORLD_TABLE(s_mpbird);   // (PC: moved aside while a kept world loads, worldglobals.h)

bool FBird(const LO* plo) { return plo && plo->Cid() == S3CID(0x60); }

// orig: 0x001B0B78 (InitLo's defaults; the per-copy data on first use: a copy takes its template's options)
BIRD& Bird(LO* plo)
{
    auto it = s_mpbird.find(plo);
    if (it != s_mpbird.end()) return it->second;
    BIRD& b = s_mpbird[plo];
    if (plo->ploCloneOf)
        if (auto itSrc = s_mpbird.find(plo->ploCloneOf); itSrc != s_mpbird.end())
        {
            b.segs.aoid[0] = -1;
            for (int i = 0; i < 6; ++i)
            {
                b.segs.aoid[i] = itSrc->second.segs.aoid[i] != -1 ? itSrc->second.segs.aoid[i]
                                 : itSrc->second.segs.apaseg[i]   ? itSrc->second.segs.apaseg[i]->oid
                                                                  : -1;
                b.segs.agrf[i] = itSrc->second.segs.agrf[i];
            }
        }
    return b;
}

ALO* Palo(LO* plo) { return static_cast<ALO*>(plo); }

float RadHeadingOf(ALO* palo)  // atan2 of the world matrix's x row (0x1B1868)
{
    const glm::mat4 m = MatJtLo(palo);  // vt slot 31
    return std::atan2(m[0].y, m[0].x);
}

// orig: 0x001B1818 (the heading kept and its rotation about z, for slot 70)
void SetBirdHeading(BIRD& b, float rad)
{
    b.radHeading = rad;
    b.matHeading = MatRotateAxis(rad, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B500
}

// orig: 0x001B17B8 (the turn's rate: D_002C0248 x the ratio, its time / sqrt(ratio))
void SetBirdTurnRatio(BIRD& b, float r)
{
    b.acTurn[0] = 25.0f * r;
    b.acTurn[1] = 0.0f * r;
    b.acTurn[2] = 0.25f / std::sqrt(r);
    b.rTurn = r;
}

const CRV* Pcrv(LO* ppath) { return ppath ? PcrvFromLo(ppath) : nullptr; }
float GMinCrv(const CRV* p) { return p->ag.empty() ? 0.0f : p->ag.front(); }  // 0x1E6610
float GMaxCrv(const CRV* p) { return p->ag.empty() ? 0.0f : p->ag.back(); }   // 0x1E6630
float DgStepCrv(const CRV* p)  // 0x1E77E8: the parameter's span over 10 x the points
{
    return p->ag.empty() ? 0.0f : (GMaxCrv(p) - GMinCrv(p)) / (static_cast<float>(p->ag.size()) * 10.0f);
}

// orig: 0x001C4AF8 (the nearest player object in the world; the original's list SW +0x3EB8, no range)
LO* PloNearestPlayer(const glm::vec3& pos)
{
    LO* pbest = nullptr;
    float s2Best = FLT_MAX;
    for (LO* plo : AploOfClass(g_psw, S3CID(0x34)))
    {
        if (!plo->fInWorld) continue;
        const glm::vec3 d = PosAloCenter(Palo(plo)) - pos;
        const float s2 = glm::dot(d, d);
        if (s2 < s2Best) { s2Best = s2; pbest = plo; }
    }
    return pbest;
}

void SetBirdState(LO* plo, int st);
void ReadBirdFields(LO* plo, BIRD& b);

// orig: 0x001B1900 (a player within +0x154 and either within +0x150 or moving at 150 or more: faced, state 3)
int StBirdSees(LO* plo, BIRD& b)
{
    const glm::vec3 pos = PosAloCenter(Palo(plo));
    LO* po = PloNearestPlayer(pos);
    if (!po) return b.state;
    const glm::vec3 d = PosAloCenter(Palo(po)) - pos;
    const float s = glm::length(d);
    if (b.sFar < s) return b.state;
    if (b.sNear < s && glm::length(VelAloPos(Palo(po))) < 150.0f) return b.state;  // vt slot 33
    SetBirdHeading(b, std::atan2(d.y, d.x));
    return 3;
}

// orig: 0x001B1868 (the heading reached within a degree)
bool FBirdFacing(LO* plo, BIRD& b)
{
    float d = std::remainder(b.radHeading - RadHeadingOf(Palo(plo)), 2.0f * float(M_PI));  // 0x11CF60
    return std::fabs(d) < 0.017453294f;
}

// orig: 0x001B1160 (walking (1) / flying (4): toward the path's point a step ahead of the nearest, at the speed;
// past the walk path's end: state 2)
void MoveBirdAlongPath(LO* plo, BIRD& b)
{
    LO* ppath = nullptr;
    float sv = 0.0f;
    if (b.state == 1) { ppath = b.ppathWalk; sv = b.svWalk; }
    else if (b.state == 4) { ppath = b.ppathFly; sv = b.svFly; }
    const CRV* pcrv = Pcrv(ppath);
    if (!pcrv) return;
    ALO* palo = Palo(plo);
    const glm::vec3 pos = PosAloCenter(palo);
    NearestCrv(pcrv, pos, b.gPath, nullptr, nullptr, &b.gPath, nullptr);  // 0x1E6ED8
    const float g = GWrapCrv(pcrv, b.gPath + b.dgPath);                    // 0x1E6660
    if (g < GMinCrv(pcrv) || GMaxCrv(pcrv) < g)
    {
        if (b.state == 1) SetBirdState(plo, 2);
        return;
    }
    glm::vec3 posT;
    EvalCrv(pcrv, g, &posT, nullptr);  // 0x1E67B8
    const glm::vec3 d = posT - pos;
    const float s = glm::length(d);
    const glm::vec3 n = s < 1e-4f ? glm::vec3(0.0f) : d / s;  // (D_002A2DA0: zero)
    SetAloVelocityTo(palo, n * sv);                            // vt slot 35
}

// orig: 0x001B1328 (a new state: its motion, its times, then its segment (none in state 2))
void SetBirdState(LO* plo, int st)
{
    BIRD& b = Bird(plo);
    ALO* palo = Palo(plo);
    if (b.state == st) return;
    if (b.fStarted) ReadBirdFields(plo, b);
    if (b.state == 2 || b.state == 3)
        if (XF* pxf = PxfRot(palo)) SetXfMode(palo, pxf, b.modeRot);  // 0x2303A0 / 0x22C840 (+0x5C, +0x1D4)
    switch (st)
    {
    case 0:
        SetAloVelocityTo(palo, glm::vec3(0.0f));  // vt slot 35 (D_002A2DD0)
        b.tNext = g_clock.t + GRandInRange(b.dtRestMin, b.dtRestMax);
        break;
    case 1:
        // (vt slot 47 (4): its transforms move)
        b.tNext = g_clock.t + GRandInRange(b.dtWalkMin, b.dtWalkMax);
        break;
    case 2:
    {
        // the way back: the step reversed, the heading turned round (the x row negated), still
        b.dgPath = -b.dgPath;
        const glm::mat4 m = MatJtLo(palo);  // vt slot 31
        SetBirdHeading(b, std::atan2(-m[0].y, -m[0].x));  // 0x11EFB0
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        if (XF* pxf = PxfRot(palo)) SetXfMode(palo, pxf, 1);
        break;
    }
    case 3:
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetBirdTurnRatio(b, b.rTurn + b.rTurn);  // 0x1B17B8(+0x21C x 2)
        if (XF* pxf = PxfRot(palo)) SetXfMode(palo, pxf, 1);
        break;
    case 4:
    {
        const glm::vec3 pos = PosAloCenter(palo);
        if (const CRV* pcrv = Pcrv(b.ppathFly))
        {
            glm::vec3 posN;
            float s = 0.0f;
            NearestCrvAll(pcrv, pos, &posN, nullptr, &b.gPath, &s);  // 0x1E6F60
            b.dgPath = DgStepCrv(pcrv);
            const float dt = glm::length(pos - posN) / b.svFly;
            b.tFade = g_clock.t + dt + (SLengthCrv(pcrv) - s) / b.svFly - b.dtFade;
        }
        else
        {
            // no path: straight away from the nearest player, level, gone in 2 s
            b.tFade = g_clock.t + 2.0f;
            LO* po = PloNearestPlayer(pos);
            glm::vec3 d = po ? pos - PosAloCenter(Palo(po)) : glm::vec3(1.0f, 0.0f, 0.0f);
            d.z = 0.0f;
            const float s = glm::length(d);
            if (s >= 1e-4f) d /= s;
            SetAloVelocityTo(palo, d * b.svFly);
        }
        break;
    }
    case 5:
    {
        // hit: the sound 0x14, the coins at +0x240, removed
        const glm::vec3 pos = PosAloCenter(palo);
        PsndStartSound(0x14, 1.0f, 0, nullptr, &pos);  // 0x1FBB90 / 0x1FB2F8 / 0x1FB3F0
        const glm::vec3 posCoin = PosJtConvert(palo, nullptr, b.posCoin);  // 0x1A6F00
        DropPickups(posCoin, b.ccoin, -100, plo);  // 0x154E10
        RemoveLo(g_psw, plo);                      // vt slot 9
        break;
    }
    default: break;
    }
    b.state = st;
    b.tState = g_clock.t;
    if (st != 2) PlayStepState(&b.segs, palo, st);  // 0x1305B8 (grf | 1)
    if (std::getenv("SLY2N_BIRDPRINT"))
        std::printf("bird %d %p: state %d t %.2f next %.2f radii %g %g\n", plo->oid, (void*)plo, st, g_clock.t, b.tNext, b.sNear, b.sFar);
}

// orig: 0x001B0D90 (on its walk path's start, walking; else resting)
void PlaceBird(LO* plo, BIRD& b)
{
    SetBirdTurnRatio(b, 1.0f);
    if (const CRV* pcrv = Pcrv(b.ppathWalk))
    {
        SetBirdState(plo, 1);
        b.gPath = GMinCrv(pcrv);
        b.dgPath = DgStepCrv(pcrv);
        glm::vec3 pos;
        EvalCrv(pcrv, b.gPath, &pos, nullptr);
        SetAloPos(Palo(plo), pos);  // vt slot 29
    }
    else SetBirdState(plo, 0);
}

LO* PloOption(LO* plo, uint32_t ib)
{
    const OID oid = static_cast<OID>(plo->U32Field(0, ib, 0xFFFFFFFFu));
    if (oid == -1) return nullptr;
    if (LO* p = PloFindSwChild(plo, oid)) return p;
    return PloFindSwObjectByOid(g_psw, oid);
}

// The option fields (0x1B0CA8's load and the options). The original reads them from the object each time it uses
// them, so a script's option write at run time takes effect at once: Elephant Rampage's crawl trigger 106963 sets
// the rats' (118626) radii 886 / 887 to 1500 / 1000 so they see Sly and run into their holes (R60; natively the
// values were read once at the start and the rats never fled). Read again before each update and state change.
void ReadBirdFields(LO* plo, BIRD& b)
{
    b.sNear = FloatField(plo, 0x150, 400.0f);
    b.sFar = FloatField(plo, 0x154, 800.0f);
    b.dtRestMin = FloatField(plo, 0x158, 2.0f);
    b.dtRestMax = FloatField(plo, 0x15C, 6.0f);
    b.dtWalkMin = FloatField(plo, 0x160, 6.0f);
    b.dtWalkMax = FloatField(plo, 0x164, 10.0f);
    b.dtFade = FloatField(plo, 0x168, 1.0f);
    b.ccoin = static_cast<int>(plo->U32Field(0, 0x230, 0xFFFFFFFFu));
    b.posCoin = glm::vec3(FloatField(plo, 0x240, 0.0f), FloatField(plo, 0x244, 0.0f), FloatField(plo, 0x248, 0.0f));
    // (the paths: object fields; found again only when a write changed their oid)
    const uint32_t oidWalk = plo->U32Field(0, 0x16C, 0xFFFFFFFFu), oidFly = plo->U32Field(0, 0x170, 0xFFFFFFFFu);
    if (!b.fPaths || oidWalk != b.oidWalk) b.ppathWalk = PloOption(plo, 0x16C);
    if (!b.fPaths || oidFly != b.oidFly) b.ppathFly = PloOption(plo, 0x170);
    b.oidWalk = oidWalk;
    b.oidFly = oidFly;
    b.fPaths = true;
}

// orig: 0x001B0D08 (vt slot 15: the segments found, the walk / fly speeds from their root motion, placed)
void StartBird(LO* plo)
{
    BIRD& b = Bird(plo);
    if (b.fStarted) return;
    b.fStarted = true;
    ALO* palo = Palo(plo);
    ReadBirdFields(plo, b);
    b.pexpl = PloOption(plo, 0x1C8);
    if (XF* pxf = PxfRot(palo)) b.modeRot = pxf->mode;
    BindStepSegs(&b.segs, palo);                     // 0x1B1BF0
    FStepLength(b.segs.apaseg[1], palo, &b.svWalk);  // 0x1B1A38 (+0x18C -> +0x1CC)
    FStepLength(b.segs.apaseg[4], palo, &b.svFly);   // (+0x1B0 -> +0x1D0)
    SetBirdHeading(b, RadHeadingOf(palo));
    PlaceBird(plo, b);
}

// orig: 0x001B0E28 (vt slot 16)
void UpdateBird(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's
    BIRD& b = Bird(plo);
    if (!b.fStarted) StartBird(plo);
    else ReadBirdFields(plo, b);
    int st = b.state;
    switch (st)
    {
    case 0:
        st = StBirdSees(plo, b);
        if (st != 3 && b.tNext <= g_clock.t && b.ppathWalk) st = 1;
        break;
    case 1:
        st = StBirdSees(plo, b);
        if (st != 3 && b.tNext <= g_clock.t) st = 0;
        break;
    case 2:
        st = StBirdSees(plo, b);
        if (st != 3 && FBirdFacing(plo, b)) st = 1;
        break;
    case 3:
        // (the face segment over: its ASEGA let go (+0x1C4 null, by the message 2 too: 0x1B10D0))
        if ((!b.segs.pasega || FAsegaRetracted(b.segs.pasega)) && FBirdFacing(plo, b)) st = 4;
        if (std::getenv("SLY2N_BIRDPRINT") && static_cast<int>(g_clock.t * 4) != static_cast<int>((g_clock.t - g_clock.dt) * 4))
            std::printf("bird %d %p: facing %.3f want %.3f asega %p retracted %d phys %d\n", plo->oid, (void*)plo, RadHeadingOf(Palo(plo)), b.radHeading,
                        (void*)b.segs.pasega, b.segs.pasega ? FAsegaRetracted(b.segs.pasega) : -1, b.cPhys);
        break;
    case 4:
        if (b.tFade <= g_clock.t)
        {
            SwitchLoFaded(plo, false, b.dtFade);  // 0x1F7BF8
            b.tFade = FLT_MAX;
        }
        break;
    default: break;
    }
    SetBirdState(plo, st);
    MoveBirdAlongPath(plo, b);
    if (std::getenv("SLY2N_BIRDPRINT") && static_cast<int>(g_clock.t * 2) != static_cast<int>((g_clock.t - g_clock.dt) * 2))
    {
        const glm::vec3 pos = PosAloCenter(Palo(plo));
        std::printf("bird %d %p: at %.0f %.0f %.0f state %d in world %d paths %d %d\n", plo->oid, (void*)plo, pos.x, pos.y, pos.z, b.state,
                    int(plo->fInWorld), b.ppathWalk ? b.ppathWalk->oid : -1, b.ppathFly ? b.ppathFly->oid : -1);
    }
}

// orig: 0x001B10B0 (vt slot 70: turned toward the heading (0x1F59A0) instead of the gravity)
void TurnBird(ALO* palo)
{
    BIRD& b = Bird(palo);
    BrakeAloRotation(palo, b.matHeading, b.acTurn);
}

// orig: 0x001B1030 (vt slot 17: turning on the spot (2 / 3): still, then turned; then the ALO's physics)
void UpdateBirdPhys(ALO* palo)
{
    BIRD& b = Bird(palo);
    ++b.cPhys;
    if (b.state == 2 || b.state == 3)
    {
        if (XF* pxf = PxfPos(palo)) pxf->vel = glm::vec4(0.0f, 0.0f, 0.0f, pxf->vel.w);  // (+0x58 / +0x5C +0xF0)
        if (XF* pxf = PxfRot(palo)) pxf->vel = glm::vec4(0.0f, 0.0f, 0.0f, pxf->vel.w);
        TurnBird(palo);
    }
    UpdateAloPhys(palo);  // 0x1F3980
}

// orig: 0x001B0F88 (vt slot 75: a contact accepted (0x1F7588) with the bytes 8, not already hit: state 5 and the
// burst +0x1C8 at the contact (BURSTPARAMS 6: its direction and place))
bool FAcceptBirdContact(ALO* palo, CTMSG* pctm)
{
    const bool r = pctm->grfAccepted != 0;  // 0x1F7588
    BIRD& b = Bird(palo);
    if (r && (pctm->grfAccepted & 8) && b.state != 5)
    {
        SetBirdState(palo, 5);
        if (b.pexpl)
        {
            BURSTPARAMS bp;
            bp.grf = 6;
            bp.dir = pctm->vec;  // (+0x10 <- the message's +0x30)
            bp.pos = pctm->pos;  // (+0x20 <- +0x20)
            ExplodeLo(b.pexpl, &bp);  // vt slot 51
        }
    }
    return r;
}

struct INITBIRD
{
    INITBIRD()
    {
        RegisterLoUpdate(0x1B0E28, UpdateBird);
        // orig: 0x001B10D0 (vt slot 5: the base's 0x133590, then the face segment's end (message 2 of the playing
        // ASEGA, its ASEG +0x1A4): state 4)
        RegisterLoMessageHandler([](LO* plo, int msgk, void* pv) {
            if (!FBird(plo) || msgk != 2) return false;
            BIRD& b = Bird(plo);
            const ASEGA* pa = static_cast<const ASEGA*>(pv);
            if (pa && pa == b.segs.pasega && pa->paseg == b.segs.apaseg[3]) SetBirdState(plo, 4);
            return false;
        });
        // EOPID 965 (0x10C5F0 -> 0x1B1B18): a state's segment {state, oid, grf}
        RegisterObjectMethod(0x10C5F0, [](LO* plo, const std::vector<CRef>& a) {
            if (FBird(plo)) SetStepSeg(&Bird(plo).segs, NArg(a, 0), NArg(a, 1, -1), NArg(a, 2, -1));
            return CRef(TAGK_Void);
        });
        // EOPID 974 (setter 0x1B17B8: the turn rate's ratio; type 1, RefSetPeopid 0x200598 coerces to a float) and 975
        // (setter 0x1B1328: the state; type 0x1036, the reference's word passed as is); natively unregistered until R60
        RegisterObjectSetter(974, [](LO* plo, const CRef& r) {
            if (FBird(plo)) SetBirdTurnRatio(Bird(plo), GArg({r}, 0));
        });
        RegisterObjectSetter(975, [](LO* plo, const CRef& r) {
            if (FBird(plo)) SetBirdState(plo, r.m_n);
        });
    }
} s_initbird;
}  // namespace

void StartBirdLo(LO* plo)
{
    if (FBird(plo)) StartBird(plo);
}
void ResetBirds() { s_mpbird.clear(); }

NATIVEFN(0x1B10B0, TurnBird);
NATIVEFN(0x1B0F88, FAcceptBirdContact);
NATIVEFN(0x1B1030, UpdateBirdPhys);

namespace
{
// orig: 0x001B0D60 (class 0x60's vt slot 26, a pool row taken again, after the base 0x1CB518): placed again (0x1B0D90:
// the turn rate's ratio 1, at its walk path's start in state 1, else state 0). Natively a reused bird kept its old
// state and place (audit 2026-11 "Open" item 8)
void ResetBirdSlot26(LO* plo, LO*)
{
    StartBird(plo);  // (natively the start's fields are read once)
    PlaceBird(plo, Bird(plo));
}
}  // namespace
NATIVEFN(0x1B0D60, ResetBirdSlot26);
