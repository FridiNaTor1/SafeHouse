// dial3.cpp: Sly 3's safe dial (class 0x4F, vtable 0x35C548, orig3 0x1C9BF0..0x1CB008), ported from the Sly 3 decomp
// (src/P2/scene_mode.cpp, scene_mode_callbacks.cpp, scene_mode_event.cpp, hand/func_001C9F00.c, func_001CAFC8.c,
// game_middle/func_001CA078.c, func_001CA400.cpp, reference_constants/func_001CAF30.c; AUDIT4 MODES-06).
//
// v_interpol's cell lock (world 7: dial 138191, task 2115 tumble_lock, the use 138208's script 2113), and one each in
// worlds 11 / 12 (121003) and 15 (93462). A script hands the dial an actor (the splice method 247, 0x10F9F8 ->
// 0x1CAF30): the dial takes player 0's pad (0x1F4B00) and runs its states (0x1CAC30):
//  1 the enter script (part 1868, +0x1A0) runs, then 3;
//  3 the dialing: the stick's angle turned around turns the dial (0x1CA400: the turn of the stick's heading beyond a
//    0.1 rad dead band, at most 5 rad/s, smoothed {100, 20, 0.25}); the three numbers of the combination (the method
//    1599, 0x10F940 -> 0x1CAF70: 0..99 each, a 2 pi / 100 step; the init's 85 / 20 / 75) are passed in turn, left /
//    right / left (0x1CABB8), each number's state (0x1CA078: approached, 3 / 4 near, 5 / 6 passed); a wrong turn (the
//    first number passed back) spins the dial back to 0 (phase 2, 0x1C9F00); all three passed (0x1CAFC8) -> 4.
//    Triangle (when +0x1D0, EOPID 1601) leaves -> 2;
//  2 the leave script (1869, +0x1A4), 4 the open script (1871, +0x1AC): when it is done the actor takes the pad back,
//    the dial's enable (-1, 0x1CAAC0) then sets 5 (opened) or 0.
// The phase (+0x1C4: -1 not dialing, 0 the stick at rest, 1 turning, 2 spinning back) drives the rumble hint (the pad's
// motors directly, 0x210D78: the small one near a number, the big one by the closeness), the near / on lights (EOPIDs
// 1602 / 1603: added / removed), the click effect (1604: a sound each 2 pi / 25, falloff 1000..6000) and the SMs: the
// dial's own (part 1872, +0x1C0's SMA: seek 159 / 160 / 161 / 164 / 165 by state, 132 / 144 / 166 by phase, 0xA3 all
// passed) and the owner's (part 1874 on 1873, +0x25C: 0x753 / 0x754 / 0x755). The rig (part 1866) is turned about its
// x axis by the angle (vt slot 35); the grip (1867) has a position controller (0x130078, mode 8) kept within 0.8 rad of
// the dial's angle (0x12F4F8).
// Not kept: the object's +0x3C bit 0x200 while dialing (no native reader). Debug: SLY3_DIALPRINT=1; SLY3_DIALTEST=1
// turns the stick by itself through the combination while a dial has the pad.

#include "../activate.h"
#include "../alo.h"
#include "../animfind.h"
#include "../brx.h"
#include "../clock.h"
#include "../joy.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../po.h"
#include "../rumble.h"
#include "../sm.h"
#include "../smrun.h"
#include "../so.h"
#include "../sound.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "../xf.h"
#include "scptgame3.h"
#include "scptrun3.h"
#include "sly3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

void BindLoPartsTable3(ALO* palo, int c, uint32_t vaTable);   // loaders.cpp (orig3 0x1C8970)
extern int g_iframeHarness;                                    // (the harness's frame)

namespace
{
constexpr float kPi2 = 6.28318548f;

bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_DIALPRINT") != nullptr;
    return s_f;
}
bool FTest()
{
    static const bool s_f = std::getenv("SLY3_DIALTEST") != nullptr;
    return s_f;
}

struct DIAL3
{
    SW* psw = nullptr;
    ALO* palo = nullptr;
    bool fCharacter = false;       // +0x130 != -1: player 0's character
    ALO* prig = nullptr;           // +0x190 (part 1866)
    ALO* pgrip = nullptr;          // +0x194 (part 1867)
    SM* psmSelf = nullptr;         // +0x198 (part 1872)
    LO* ascpt[6]{};                // +0x19C.. by state (parts 1868..1871 at +0x1A0..+0x1AC)
    int state = -1;                // +0x1B4
    float tState = 0.0f;           // +0x1B8
    LO* pscptCur = nullptr;        // +0x1BC
    SMA* psmaSelf = nullptr;       // +0x1C0
    int phase = -1;                // +0x1C4
    int fSolved = 0;               // +0x1C8
    float tPhase = 0.0f;           // +0x1CC
    int fActive = 1;               // +0x1D0 (EOPID 1601: Triangle leaves)
    float tSolved = 3.4028235e38f; // +0x1D4
    LO* ploOn = nullptr;           // +0x1D8 (1602)
    LO* ploNear = nullptr;         // +0x1DC (1603)
    int sfx = -1;                  // +0x1E0 (1604)
    float aangle[3]{};             // +0x1E4: the combination's angles
    LO* ploActor = nullptr;        // +0x1F0
    float heading = 0.0f;          // +0x1F8: the stick's heading followed
    float speed = 0.0f;            // +0x1FC
    float angle = 0.0f;            // +0x200
    float anglePrev = 0.0f;        // +0x204
    int fPaused = 0;               // +0x208
    int amode[3]{0, -1, -1};       // +0x20C: each number's state
    glm::mat3 matRig{1.0f};        // +0x220: the rig's rotation at the load
    CTRLPOS* pctrl = nullptr;      // +0x250 (0x130078 on the grip)
    ALO* powner = nullptr;         // +0x254 (part 1873)
    SM* psmOwner = nullptr;        // +0x258 (part 1874)
    SMA* psmaOwner = nullptr;      // +0x25C
    float tLog = -1.0f;
    int fMotors = 0;
};
std::unordered_map<const LO*, DIAL3> s_mpdial;

// orig3 0x1CAF70: the combination (0..99 each)
void SetCombination(DIAL3& d, int a, int b, int c)
{
    d.aangle[0] = float(a + a) * 0.03141593f;
    d.aangle[1] = float(b + b) * 0.03141593f;
    d.aangle[2] = float(c + c) * 0.03141593f;
}

void SetDialState(DIAL3& d, int st);
void SetDialPhase(DIAL3& d, int ph);
void ActivateDial(LO* plo, LO* ploActor);

// orig3 0x1CAB30: the dial's SM by the state / phase
void SeekSelf(DIAL3& d)
{
    static const int s_aphase[3] = {132, 144, 166};          // D_00323840
    static const int s_astate[6] = {159, 160, 161, -1, 164, 165};   // D_00323850
    if (!d.psmaSelf) return;
    OID oid;
    if (d.state == 3) oid = OID(d.fSolved ? 0xA3 : (d.phase >= 0 && d.phase < 3 ? s_aphase[d.phase] : -1));
    else oid = OID(d.state >= 0 && d.state < 6 ? s_astate[d.state] : -1);
    SeekSma(d.psmaSelf, oid);
}

// orig3 0x1CABB8: the number's turn direction (+1 / -1)
float GSign(const DIAL3& d, int i)
{
    switch (d.amode[i])
    {
    case 2: case 4: case 6: return (i & 1) ? 1.0f : -1.0f;
    case 1: case 3: case 5: return (i & 1) ? -1.0f : 1.0f;
    default: return 1.0f;
    }
}

// orig3 0x1CAE80: a number's state (the owner's SM told)
void SetNumberMode(DIAL3& d, int i, int mode)
{
    d.amode[i] = mode;
    if (!d.psmaOwner) return;
    if (i < 2)
    {
        if (mode == 0 || mode == 5) SetSmaGoal(d.psmaOwner, OID(0x755));
        else if (mode == 6) SetSmaGoal(d.psmaOwner, OID(0x754));
    }
    else if (mode == 5 || mode == 6) SetSmaGoal(d.psmaOwner, OID(0x753));
}

// orig3 0x1CA030
void SetSolved(DIAL3& d, int f)
{
    if (d.fSolved == f) return;
    d.fSolved = f;
    d.tSolved = f ? g_clock.t : 3.4028235e38f;
    SeekSelf(d);
}

// orig3 0x1CB008: the turn cleared, the first number awaited
void ResetTurn(DIAL3& d)
{
    d.anglePrev = d.speed = d.angle = 0.0f;   // (+0x1FC / +0x200 / +0x204)
    SetNumberMode(d, 0, 0);
    SetSolved(d, 0);
    for (int i = 1; i < 3; ++i) SetNumberMode(d, i, -1);
}

// orig3 0x1CAFC8: every number passed (5 / 6)
bool FAllPassed(const DIAL3& d)
{
    for (int m : d.amode)
        if (m < 5 || m >= 7) return false;
    return true;
}

DIAL3* Pdial(LO* plo)
{
    if (!plo || !g_psw || !plo->pcls || !FCidDerivedFrom(plo->Cid(), 0x4F)) return nullptr;
    auto it = s_mpdial.find(plo);
    if (it != s_mpdial.end() && it->second.psw == g_psw) return &it->second;
    DIAL3& d = s_mpdial[plo];
    d = DIAL3{};
    d.psw = g_psw;
    d.palo = static_cast<ALO*>(plo);
    // the init 0x1C9BF0: the defaults, 0x1CB008, the combination 85 / 20 / 75
    ResetTurn(d);
    SetCombination(d, 0x55, 0x14, 0x4B);
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval[0];
        auto lo = [&]() -> LO* { return v.pbasic ? dynamic_cast<LO*>(v.pbasic) : (v.n == -1 || v.n == 0 ? nullptr : PloFindSwNearest(plo, v.n)); };
        switch (o.eopid)
        {
        case 1601: d.fActive = v.n; break;
        case 1602: d.ploOn = lo(); break;
        case 1603: d.ploNear = lo(); break;
        case 1604: d.sfx = v.n; break;
        default: break;
        }
    }
    // the post-load 0x1C9C58: the parts (table 0x345860), the rig's rotation, the grip's controller, the SMs
    BindLoPartsTable3(d.palo, 9, 0x345860);
    auto part = [&](uint32_t ib) -> LO* {
        for (const auto& [i, p] : d.palo->apart)
            if (i == (FN3_TAG | ib)) return p;
        return nullptr;
    };
    auto alo = [](LO* p) -> ALO* { return p && FCidDerivedFrom(p->Cid(), sly3::CID_ALO) ? static_cast<ALO*>(p) : nullptr; };
    d.prig = alo(part(0x190));
    d.pgrip = alo(part(0x194));
    d.psmSelf = dynamic_cast<SM*>(part(0x198));
    for (int i = 1; i <= 4; ++i) d.ascpt[i] = part(0x19C + 4u * uint32_t(i));
    d.powner = alo(part(0x254));
    d.psmOwner = dynamic_cast<SM*>(part(0x258));
    if (d.prig)
        if (XF* pxf = PxfRot(d.prig)) d.matRig = glm::mat3(pxf->mat);
    if (d.pgrip)
        if (XF* pxf = PxfRot(d.pgrip)) d.pctrl = PctrlNewPos(d.pgrip, pxf, 0, 8, 2, glm::vec4(0.0f));
    if (d.psmSelf) ReplaceSmaHandle(d.psmSelf, d.palo, OID(-1), 0, &d.psmaSelf);
    if (d.psmOwner && d.powner) ReplaceSmaHandle(d.psmOwner, d.powner, OID(0x753), 0, &d.psmaOwner);
    if (FPrint())
        std::printf("dial3: %d: rig %d grip %d sm %d scripts %d %d %d %d owner %d sm %d; active %d lights %d %d sfx %d\n", plo->oid,
                    d.prig ? d.prig->oid : -1, d.pgrip ? d.pgrip->oid : -1, d.psmSelf ? d.psmSelf->oid : -1,
                    d.ascpt[1] ? d.ascpt[1]->oid : -1, d.ascpt[2] ? d.ascpt[2]->oid : -1, d.ascpt[3] ? d.ascpt[3]->oid : -1,
                    d.ascpt[4] ? d.ascpt[4]->oid : -1, d.powner ? d.powner->oid : -1, d.psmOwner ? d.psmOwner->oid : -1, d.fActive,
                    d.ploOn ? d.ploOn->oid : -1, d.ploNear ? d.ploNear->oid : -1, d.sfx);
    if (FPrint())
    {
        const glm::vec3 p = PosLoWorld(plo);
        std::printf("dial3: %d at %.0f %.0f %.0f (in world %d)\n", plo->oid, p.x, p.y, p.z, int(plo->fInWorld));
    }
    // the reset 0x1C9D10 (vt slot 31, at the world start): state 0, the owner's SM at 0x753
    SetDialState(d, 0);
    d.tSolved = 3.4028235e38f;
    if (d.psmaOwner) SnapSmaGoal(d.psmaOwner, OID(0x753));
    return &d;
}

// orig3 0x1CAD10
void SetDialPhase(DIAL3& d, int ph)
{
    if (ph == d.phase) return;
    switch (d.phase)
    {
    case -1:
        d.fMotors = 1;   // (0x210C90(rumble, 5): the motors held by the dial; +0x3C |= 0x200)
        break;
    case 2: ResetTurn(d); break;
    default: break;
    }
    if (FPrint()) std::printf("dial3: %d phase %d -> %d (angle %.3f, t %.2f)\n", d.palo->oid, d.phase, ph, d.angle, g_clock.t);
    d.phase = ph;
    d.tPhase = g_clock.t;
    switch (ph)
    {
    case -1:
        // (+0x3C &= ~0x200; 0x210C90(rumble, 1): idle again)
        if (d.fMotors) SetJoyMotorsDirect3(false, 0);
        d.fMotors = 0;
        if (d.psmaOwner) SetSmaGoal(d.psmaOwner, OID(0x753));
        break;
    case 2:
        for (int i = 0; i < 3; ++i) SetNumberMode(d, i, -1);
        if (d.psmaOwner) SetSmaGoal(d.psmaOwner, OID(0x753));
        break;
    default: break;
    }
    SeekSelf(d);
}

// orig3 0x1CAC30
void SetDialState(DIAL3& d, int st)
{
    if (st == d.state) return;
    if (d.pscptCur)
    {
        if (SCPT3* ps = PscptOfLo3(d.pscptCur)) StopScpt3(ps);   // 0x226050
        d.pscptCur = nullptr;
    }
    if (d.state == 3) SetDialPhase(d, -1);
    if (FPrint()) std::printf("dial3: %d state %d -> %d (t %.2f)\n", d.palo->oid, d.state, st, g_clock.t);
    d.state = st;
    d.tState = g_clock.t;
    if (st == 3)
    {
        SetDialPhase(d, 0);
        ResetTurn(d);
        if (d.pctrl) SetCtrlPos(d.pctrl, glm::vec4(0.0f));   // 0x12F4F8(+0x250, 0)
    }
    if (d.state != -1)
    {
        d.pscptCur = d.state >= 0 && d.state < 6 ? d.ascpt[d.state] : nullptr;
        if (d.pscptCur) StartScptOfLo3(d.pscptCur, d.ploActor);   // 0x225EE8(script, the actor)
        SeekSelf(d);
    }
}

bool FScriptDone(const DIAL3& d)
{
    if (!d.pscptCur) return true;
    const SCPT3* ps = PscptOfLo3(d.pscptCur);
    return !ps || !FScptRunning3(ps);   // !0x226190
}

// orig3 0x1CAAC0 (vt slot 83, -1: the pad taken away): opened (4 / 5) -> 5, else 0
void OnDialEnable(DIAL3& d, int n)
{
    if (n != -1) return;
    d.fCharacter = false;
    SetDialState(d, d.state >= 4 && d.state < 6 ? 5 : 0);
}

// 0x1F4B00(SW, +0x130, the actor): the actor is player 0's character again
void GivePadBack(DIAL3& d)
{
    if (d.fCharacter && d.psw)
    {
        if (PloPoCurrent(d.psw) == d.palo && d.ploActor) SetPoCurrent(d.psw, d.ploActor);
        OnDialEnable(d, -1);
    }
}

// the pad of the dial's player (natively player 0's: +0x134 {x, y, deflection})
struct STICK { float x = 0, y = 0, u = 0; };
STICK StickNow(DIAL3& d)
{
    STICK s;
    if (!d.fCharacter) return s;
    s.x = g_joyps2.x;
    s.y = g_joyps2.y;
    s.u = g_joyps2.uDeflect;
    if (FTest() && d.state == 3)
    {
        // (debug: the stick turned around: toward the current number's direction, a quarter turn a second)
        int i = 0;
        while (i < 2 && d.amode[i] >= 5) ++i;
        const float sgn = GSign(d, i) * (d.amode[i] == 0 ? 1.0f : 1.0f);
        const float target = sgn * d.aangle[i];
        const float dir = target - d.angle > 0.0f ? 1.0f : -1.0f;
        static float s_h = 0.0f;
        s_h = cmm::RadNormalize(s_h + dir * 2.5f * g_clock.dt);
        s.x = std::cos(s_h), s.y = std::sin(s_h), s.u = 1.0f;
    }
    return s;
}

// orig3 0x1C9F00: the phase by the stick
void UpdatePhase(DIAL3& d, const STICK& s)
{
    for (int guard = 0; guard < 8; ++guard)
    {
        const int ph = d.phase;
        if (FAllPassed(d)) SetSolved(d, 1);
        switch (d.phase)
        {
        case 0: if (s.u >= 1.0f || (FTest() && s.u >= 0.99f)) SetDialPhase(d, 1); break;
        case 1: if (s.u < 1.0f && !(FTest() && s.u >= 0.99f)) SetDialPhase(d, 0); break;
        case 2: if (g_clock.t - d.tPhase > 0.25f && d.angle == 0.0f) SetDialPhase(d, 0); break;
        default: break;
        }
        if (d.phase == ph) break;
    }
}

// orig3 0x1C9D70: the states
void UpdateStates(DIAL3& d, const STICK& s)
{
    for (int guard = 0; guard < 8; ++guard)
    {
        const int st = d.state;
        switch (st)
        {
        case 1:
            if (FScriptDone(d)) SetDialState(d, 3);
            break;
        case 3:
            if (d.fActive && d.fCharacter && (g_joyps2.grfbtnPressed & 0x10))   // 0x1A7A80(pad, Triangle)
            {
                g_joyps2.grfbtnPressed &= ~0x10u;
                SetDialState(d, 2);
            }
            else if ((!d.phase || g_clock.t - d.tSolved > 0.4f) && FAllPassed(d)) SetDialState(d, 4);
            else UpdatePhase(d, s);
            break;
        case 2:
        case 4:
            if (FScriptDone(d)) GivePadBack(d);
            break;
        default: break;
        }
        if (d.state == st) break;
    }
}

// orig3 0x1CA078: number i's state by the angle (+0x200) and the last one (+0x204) about its place
void UpdateNumber(DIAL3& d, int i)
{
    constexpr float kFar = 0.50265485f, kNear = 0.251327425f;
    for (int guard = 0; guard < 8; ++guard)
    {
        const int st = d.amode[i];
        const float w = GSign(d, i) * d.aangle[i];
        const float b = d.anglePrev - w, a = d.angle - w;
        switch (st)
        {
        case 0:
            if (b + kFar <= 0.0f && a + kFar > 0.0f) SetNumberMode(d, i, 3);
            {
                const float ym = d.anglePrev + d.aangle[i], xm = d.angle + d.aangle[i];
                if (ym - kFar >= 0.0f && xm - kFar < 0.0f) SetNumberMode(d, i, 4);
            }
            break;
        case 1:
            if (b + kFar <= 0.0f && a + kFar > 0.0f) SetNumberMode(d, i, 3);
            break;
        case 2:
            if (b - kFar >= 0.0f && a - kFar < 0.0f) SetNumberMode(d, i, 4);
            break;
        case 3:
            if (b + kNear <= 0.0f && a + kNear > 0.0f)
            {
                SetNumberMode(d, i, 5);
                if (i + 1 < 3) SetNumberMode(d, i + 1, 2);
            }
            break;
        case 4:
            if (b - kNear >= 0.0f && a - kNear < 0.0f)
            {
                SetNumberMode(d, i, 6);
                if (i + 1 < 3) SetNumberMode(d, i + 1, 1);
            }
            break;
        case 5:
            if (b - kNear <= 0.0f && a - kNear > 0.0f)
                if (i + 2 >= 3 || d.amode[i + 2] == -1) SetDialPhase(d, 2);
            break;
        case 6:
            if (b + kNear >= 0.0f && a + kNear < 0.0f)
                if (i + 2 >= 3 || d.amode[i + 2] == -1) SetDialPhase(d, 2);
            break;
        default: break;
        }
        if (d.amode[i] == st) break;
    }
}

// rotation about the x axis (orig3 0x11C8C0 MatRotateAxis with D_00317530 = (1, 0, 0))
glm::mat3 MatRotX(float a)
{
    const float c = std::cos(a), s = std::sin(a);
    return glm::mat3(glm::vec3(1, 0, 0), glm::vec3(0, c, s), glm::vec3(0, -s, c));
}

// orig3 0x1CA400 (vt slot 18)
void UpdateDial(LO* plo)
{
    DIAL3* pd = Pdial(plo);
    if (!pd) return;
    DIAL3& d = *pd;
    if (g_clock.dt == 0.0f)
    {
        if (!d.fPaused)
        {
            if (d.fMotors) SetJoyMotorsDirect3(false, 0);   // 0x210D58: the motors off while the clock stands
            d.fPaused = 1;
        }
        return;
    }
    d.fPaused = 0;
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    // (debug: SLY3_DIALACTIVATE=F[,oid]: at harness frame F the dial (the given one, else every dial in the world) is
    // activated by the current player object as its use's splice method 247 does, the play-tests of tools/dial3.sh)
    if (static const char* s_sz = std::getenv("SLY3_DIALACTIVATE"); s_sz && d.state == 0 && d.psw)
    {
        int f = 0, oid = -1;
        std::sscanf(s_sz, "%d,%d", &f, &oid);
        if (g_iframeHarness == f && (oid == -1 || oid == plo->oid)) ActivateDial(plo, PloPoCurrent(d.psw));
    }
    const STICK s = StickNow(d);
    UpdateStates(d, s);
    static const cmm::SMP s_smpSettle{10.0f, 0.0f, 0.25f};
    static const cmm::SMP s_smpSpin{100.0f, 20.0f, 0.25f};
    float gTarget = 0.0f;
    bool fTurned = false;
    switch (d.phase)
    {
    case 2:
        if (g_clock.t - d.tPhase > 0.25f)
        {
            float g = d.angle;
            if (g > 0.0f) g -= kPi2;
            g = cmm::GSmooth(g, 0.0f, g_clock.dt, &s_smpSettle, &d.speed);
            d.anglePrev = d.angle;
            d.angle = cmm::RadNormalize(g);
            fTurned = true;
        }
        else gTarget = s_smpSettle.svFast;
        break;
    case 1:
    {
        float dh = cmm::RadNormalize(std::atan2(s.y, s.x) - d.heading);
        if (dh > 0.1f) dh -= 0.1f;
        else if (dh < -0.1f) dh += 0.1f;
        else dh = 0.0f;
        d.heading = cmm::RadNormalize(d.heading + dh);
        gTarget = cmm::GLimitAbs(dh / g_clock.dt, 5.0f);
        break;
    }
    default: break;
    }
    if (!fTurned)
    {
        d.speed = cmm::GSmooth(d.speed, gTarget, g_clock.dt, &s_smpSpin, nullptr);
        d.anglePrev = d.angle;
        d.angle += d.speed * g_clock.dt;
    }
    // the rig turned about its x axis (vt slot 35: the parent's rotation x the turn x the loaded rotation)
    if (d.prig)
        if (XF* pxf = PxfRot(d.prig))
        {
            glm::mat3 par(1.0f);
            if (pxf->pxfRef) par = glm::mat3(pxf->pxfRef->matWorld);
            for (int k = 0; k < 3; ++k) par[k] = glm::normalize(par[k]);
            SetAloRotation(d.prig, par * MatRotX(d.angle) * d.matRig);
        }
    if (d.amode[0] == 3 || d.amode[0] == 4)
        if (d.angle * GSign(d, 0) < 0.0f) SetDialPhase(d, 2);
    for (int i = 0; i < 3; ++i) UpdateNumber(d, i);
    float level = 0.0f;
    bool fNear = false;
    if (d.phase != -1)
    {
        // the grip's controller kept within 0.8 rad of the angle (moved back to 0.6 inside)
        static glm::vec4 s_posGrip(0.0f);
        glm::vec4 pos = d.pctrl ? d.pctrl->pos : s_posGrip;
        for (int guard = 0; guard < 64; ++guard)
        {
            const float g = cmm::RadNormalize(d.angle + pos.x);
            if (g < -0.8f) pos.x += 0.6f + 0.8f;
            else if (g > 0.8f) pos.x += -0.6f - 0.8f;
            else break;
        }
        if (d.pctrl) SetCtrlPos(d.pctrl, pos);
        if (d.phase == 1)
            for (int i = 0; i < 3; ++i)
                if (d.amode[i] >= 3 && d.amode[i] <= 6)
                {
                    const float g = std::fabs(GSign(d, i) * d.aangle[i] - d.angle);
                    level = std::clamp(1.0f - g * 1.98943675f, 0.0f, 1.0f);
                    fNear = g < 0.251327425f;
                }
        if (d.fMotors) SetJoyMotorsDirect3(fNear, uint8_t(level * 255.0f));   // 0x210D78
    }
    const bool fOn = !fNear && level > 0.0f;
    if (d.ploOn && fOn != d.ploOn->fInWorld)
    {
        if (fOn) AddLo(d.psw, d.ploOn);   // vt slot 8
        else RemoveLo(d.psw, d.ploOn);    // vt slot 9
    }
    if (d.ploNear && fNear != d.ploNear->fInWorld)
    {
        if (fNear) AddLo(d.psw, d.ploNear);
        else RemoveLo(d.psw, d.ploNear);
    }
    if (d.sfx != -1)
    {
        auto quant = [](float g) {   // 0x11E438(g, 25)
            g = cmm::RadNormalize(g);
            const float sp = kPi2 / 25.0f, half = sp * 0.5f;
            float sgn = 1.0f;
            if (g < 0.0f) g = -g, sgn = -1.0f;
            float snapped = 0.0f;
            for (int i = 0; i <= 12; ++i)
            {
                snapped = sp * float(i);
                if (snapped - half <= g && g <= snapped + half) break;
            }
            return snapped * sgn;
        };
        if (std::fabs(cmm::RadNormalize(quant(d.angle) - quant(d.anglePrev))) > 0.001f)
            if (CSnd* psnd = PsndStartSound(d.sfx, 1.0f, 0, d.palo)) psnd->SetFalloff(1000.0f, 6000.0f);
    }
    if (FPrint() && d.state == 3 && g_clock.t - d.tLog >= 0.5f)
    {
        d.tLog = g_clock.t;
        std::printf("dial3: %d t %.2f angle %.3f speed %.2f phase %d numbers %d %d %d (%.3f %.3f %.3f) level %.2f near %d\n", plo->oid, g_clock.t,
                    d.angle, d.speed, d.phase, d.amode[0], d.amode[1], d.amode[2], d.aangle[0], -d.aangle[1], d.aangle[2], level, int(fNear));
    }
}

// orig3 0x1CAF30 (the splice method 247): the actor hands the dial the pad
void ActivateDial(LO* plo, LO* ploActor)
{
    DIAL3* pd = Pdial(plo);
    if (!pd) return;
    if (FPrint()) std::printf("dial3: %d activated by %d (t %.2f)\n", plo->oid, ploActor ? ploActor->oid : -1, g_clock.t);
    pd->ploActor = ploActor;
    SetDialState(*pd, 1);
    // 0x1F4B00(SW, 0, dial): player 0's character
    if (pd->psw)
    {
        SetPoCurrent(pd->psw, plo);
        pd->fCharacter = true;
    }
}

// orig3 0x1C9D10 (vt slot 31): state 0, the turn cleared, the owner's SM at 0x753
void ResetDial(LO* plo, LO*)
{
    DIAL3* pd = Pdial(plo);
    if (!pd) return;
    SetDialState(*pd, -1);
    SetDialState(*pd, 0);
    pd->fActive = 1;
    pd->tSolved = 3.4028235e38f;
    if (pd->psmaOwner) SnapSmaGoal(pd->psmaOwner, OID(0x753));
}

struct INITDIAL3
{
    INITDIAL3()
    {
        RegisterLoUpdate(FN3_TAG | 0x1CA400, UpdateDial);
        // the splice methods: 1599 (0x10F940 -> 0x1CAF70: the combination), 247 (0x10F9F8 -> 0x1CAF30: activated)
        RegisterObjectMethod3(0x10F940, [](LO* plo, const std::vector<CRef>& a) {
            if (DIAL3* pd = Pdial(plo))
            {
                SetCombination(*pd, NArg(a, 0, 0), NArg(a, 1, 0), NArg(a, 2, 0));
                if (FPrint()) std::printf("dial3: %d combination %d %d %d\n", plo->oid, NArg(a, 0, 0), NArg(a, 1, 0), NArg(a, 2, 0));
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x10F9F8, [](LO* plo, const std::vector<CRef>& a) {
            ActivateDial(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        // the options 1601..1604 (fields +0x1D0 / +0x1D8 / +0x1DC / +0x1E0)
        RegisterObjectSetter3(1601, [](LO* plo, const CRef& r) { if (DIAL3* pd = Pdial(plo)) pd->fActive = r.RefCoerceS32().m_n; });
        RegisterObjectGetter3(1601, [](LO* plo) { CRef r; DIAL3* pd = Pdial(plo); r.SetS32(pd ? pd->fActive : 0); return r; });
        RegisterObjectSetter3(1602, [](LO* plo, const CRef& r) { if (DIAL3* pd = Pdial(plo)) pd->ploOn = dynamic_cast<LO*>(r.m_pbasic); });
        RegisterObjectSetter3(1603, [](LO* plo, const CRef& r) { if (DIAL3* pd = Pdial(plo)) pd->ploNear = dynamic_cast<LO*>(r.m_pbasic); });
        RegisterObjectSetter3(1604, [](LO* plo, const CRef& r) { if (DIAL3* pd = Pdial(plo)) pd->sfx = r.RefCoerceS32().m_n; });
    }
} s_initdial3;
NATIVEFN3(0x1C9D10, ResetDial);
}  // namespace

void ResetDials3() { s_mpdial.clear(); }

// (update.cpp, the objects' post-loads) the record made and the object updated (the SO base 0x1F3B50 sets +0x3C bit 2)
void PostLoadDial3(LO* plo)
{
    if (!plo || !plo->pcls || !g_psw || !FCidDerivedFrom(plo->Cid(), 0x4F)) return;
    Pdial(plo);
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
}
