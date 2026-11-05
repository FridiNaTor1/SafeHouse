#pragma once

// The guards and NPCs (base class 0x38, classes 0x39..0x43; module 0x205E70..0x20B840 with the helpers of
// 0x20B840..0x20D9D0): their AI jobs (ai.h), their animation choice (the state mask +0x2C0: each job sets the
// indices of what it does; the kind table +0x4C0 maps index sets to an ASEG, an SM or the stepped / pushed moves of
// kinds 2 / 3, the best fitting entry plays), their targets (the players and others the detector +0xF64 sees, the
// objects of the +0x398 classes nearby), the two ground watchers (+0x498 / +0x49C) and the per-frame update
// (0x207350, vt slot 16).
//
// They are stepping objects (STEP, class 0x33): their STEP fields are the physics code's STEPPHYS (stepphys.h).
//
// Part of the animation / AI code. Field comments: the original's offsets in the guard.

#include "ai.h"
#include "asega.h"
#include "crv.h"
#include "lo.h"
#include "stepphys.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct ASEG;
struct ASEGA;
struct DETECTOR;
struct GUARD;
struct SM;
struct SMA;
struct XF;
struct CTRLPOS;
struct SW;

// A kind-2 object (0x40 bytes, 0x20BD78 / 0x20BD88: an ASEG moved by its events 0x22 / 0x23 / 0x24 in steps)
struct KIND2
{
    int n0 = -1;                  // +0x0 the ASEG's oid (before the first resolve)
    ASEG* paseg = nullptr;        // +0x4
    float t22 = 0, t23 = 0, t24 = 0;  // +0x8 / +0xC / +0x10 the events' times
    float dt14 = 0, dt18 = 0;     // +0x14 / +0x18 their differences
    float s1C = 0;                // +0x1C the horizontal distance between the root's ends
    float g28 = 0;                // +0x28 the height term
    int f30 = 0;                  // +0x30
};
// The kind-2 player (orig 0x70 bytes, the guard's +0xDB0; 0x20BF80 start, 0x20C2F8 states, 0x20C0E8 update, 0x20C290
// the event 0x22, 0x20C6F8 the target, 0x20C080 stop; kind2.cpp): -1 none, 0 playing up to the event 0x22, 1 in the
// air towards the target, 2 at the event 0x23's time, 3 at 0x24's (landed), 4 done
struct K2PLAY
{
    int st = -1;                  // +0x10
    float tSt = 0;                // +0x14
    glm::vec3 dvGravityPrev{0.0f};  // +0x20 the object's gravity before the flight
    const KIND2* ptuning = nullptr;  // +0x30
    ALO* palo = nullptr;          // +0x34
    ASEGA* pasega = nullptr;      // +0x38
    int grf = 0;                  // +0x3C the ApplyAseg flags
    glm::vec3 posTarget{0.0f};    // +0x40
    float tArrive = 0, tEnd = 0;  // +0x50 / +0x54
    LO* ploRelated = nullptr;     // +0x58
    int kind = 0;                 // +0x5C
    int fBallistic = 0, fSteep = 0;  // +0x60 / +0x64
    struct CTRL* pctrl = nullptr;   // +0x68 the ballistic flight's target controller (0x12DE58)
};
void StartK2(const KIND2* ptuning, ALO* palo, int grf, K2PLAY* pk);   // orig 0x20BF80
void SetK2State(K2PLAY* pk, int st);                                  // orig 0x20C2F8
void UpdateK2(K2PLAY* pk);                                            // orig 0x20C0E8
void StopK2(K2PLAY* pk);                                              // orig 0x20C080
inline bool FK2Done(const K2PLAY* pk) { return pk->st == 4; }         // orig 0x20C0D8
void SetK2Target(K2PLAY* pk, const glm::vec3& pos);                   // orig 0x20C6F8
void ReleaseK2Ballistic(K2PLAY* pk);                                  // orig 0x20C760
KIND2* Pk2Resolve(GUARD* pg, LO* paseg);                              // orig 0x20C010 / 0x20BD78 / 0x20BD88
KIND2* Pk2ResolveAlo(ALO* palo, LO* paseg);                           // (the same for any object: Murray's +0x1850)
// A kind-3 object (0xD0 bytes, 0x20C7D8 / 0x20C7E0: four entries {oid, ASEG, ...} of 0x30)
struct KIND3
{
    struct E
    {
        int oid = -1;             // +0x0
        ASEG* paseg = nullptr;    // +0x4
        glm::vec4 vel{0.0f};      // +0x10 the root's mean velocity
        float s20 = 0;            // +0x20 its length
    };
    std::array<E, 4> ae;
    int fResolved = 0;            // +0xC0
};

// The directional player (orig 0x40 bytes, GUARD +0xE20). Marker pairs 0x17D..0x180 allow changing
// between the four clips; the chosen clip's mean speed supplies the requested direction's velocity.
struct K3PLAY : ASEGALISTENER
{
    glm::vec4 vel{0.0f};          // +0x10
    ALO* palo = nullptr;          // +0x20
    ASEGA* pasega = nullptr;      // +0x24
    const KIND3* ptuning = nullptr;  // +0x28
    const KIND3::E* pcur = nullptr;  // +0x2C
    int grf = 0;                 // +0x30
    int f34 = 0, f38 = 0;        // +0x34 / +0x38
    ~K3PLAY() override;
    void OnAsegaMsg(ASEGA* pasega, int msgk) override;
};
void ResolveK3(KIND3* pk, ALO* palo);                              // orig 0x20C7E0
void StartK3(const KIND3* pk, ALO* palo, int grf, K3PLAY* p);       // orig 0x20C948
void StopK3(K3PLAY* p);                                           // orig 0x20CA48
bool FK3Done(const K3PLAY* p);                                    // orig 0x20CA98
void OnK3Event(K3PLAY* p, const ASEGMSG9* pm);                     // orig 0x20CAA8
void SelectK3(K3PLAY* p, const glm::vec3& vel);                    // orig 0x20CB28
void SetGuardKind3(GUARD* pg, uint64_t mask, const std::array<OID, 4>& aoid, bool fResolve);

// A kind entry (orig 0x20 bytes in the table +0x4D0 (the mask at +0x10), 0x10 in the three arrays +0x8D0..)
struct KINDE
{
    int k = -1;                   // +0x0: -1 none, 0 an ASEG, 1 an SM, 2 a KIND2, 3 a KIND3
    OID oid = -1;                 // +0x4 before the resolve (0x209130)
    LO* plo = nullptr;            // +0x4 k 0 / 1: the ASEG / SM
    KIND2* pk2 = nullptr;         // +0x4 k 2
    KIND3* pk3 = nullptr;         // +0x4 k 3
    ASEGA* pasega = nullptr;      // +0x8 k 0: the playing ASEGA
    SMA* psma = nullptr;          // +0x8 k 1
    void* pvState = nullptr;      // +0x8 k 2 / 3: the guard's +0xDB0 / +0xE20 player
    int grf = -1;                 // +0xC the ApplyAseg flags (| 1 when started by a move)
    uint64_t mask = 0;            // +0x10 (the table's)
};

// The ground watchers (orig 0x10 / 0x14 bytes: 0x20CE28 / 0x20D1B0)
struct WATCHA
{
    GUARD* pguard = nullptr;      // +0x0
    int st = -1;                  // +0x4
    float t = 0;                  // +0x8
    int fOn = 1;                  // +0xC
};
struct WATCHB
{
    GUARD* pguard = nullptr;      // +0x0
    int st = -1;                  // +0x4
    float t = 0;                  // +0x8
    LO* plo = nullptr;            // +0xC the object (0x20D460)
    int fOn = 1;                  // +0x10
};

// The current move (orig +0xFD0: {the move job, the heading lock, , the guard})
struct MOVEREF
{
    AIJOB* pjob = nullptr;        // +0x0 (0x209788)
    int fHeadingLocked = 0;       // +0x4 (0x2097C8 / 0x2097D8: messages 0x25 / 0x26)
    int n8 = 0;                   // +0x8
    GUARD* pguard = nullptr;      // +0xC
};

struct GUARD
{
    ALO* palo = nullptr;

    // STEP's fields (+0x180..+0x268) are the physics code's STEPPHYS (stepphys.h: StepPhysOf(palo), same names;
    // +0x1E0 is posTarget there): see St(pg)
    glm::vec4 posProbe{0.0f};     // *+0x218 the floor probe's point when imported from a reference (fProbeRam)
    bool fProbeRam = false;
    bool fFloorRam = false;       // +0x208 set in the reference (with fProbeRam: the import's floor contact)
    // PO / guard
    AISTACK stack;                // +0x26C
    LO* ploPath294 = nullptr;     // +0x294 its own path object (0x1E94B0(0x2C9): cid 0x0A oid 713)
    CRV crv294;                   //   (natively: the curve the go-to writes, 0x17DA28)
    int f298 = 0;                 // +0x298 (busy: 0x207598)
    int f29C = 0;                 // +0x29C
    uint64_t grfState = 0;        // +0x2C0 the state mask (0x209890 / 0x209970)
    // (InitLo 0x205EB8 sets +0x2CC / +0x2D0 to FLT_MAX and leaves +0x2D4 / +0x2D8 zero: a guard whose wait started
    // while the game was paused for the AI (the fresh episode's briefing) goes back to its patrol at once)
    float tWaitA0 = 3.4028235e38f, tWaitA1 = 3.4028235e38f;  // +0x2CC / +0x2D0 the waits' range (0x179E80; script op 1)
    float tWaitB0 = 0.0f, tWaitB1 = 0.0f;                    // +0x2D4 / +0x2D8 (0x179EF8; op 2)
    SAVEDSCRIPT saved2DC;         // +0x2DC (0x171C18)
    CMDLIST* pcmdlistPatrol = nullptr;  // +0x2E8 (EOPID273 / the patrol groups)
    CMDLIST* pcmdlistHome = nullptr;    // +0x2EC (0x2061D8: a go-to its home)
    CMD* pcmdHome = nullptr;      // +0x2F0
    glm::vec4 posHome{0.0f};      // +0x300
    float radHome = 0.0f;         // +0x310
    float g314 = 800.0f, g318 = 1200.0f, g31C = 300.0f, g320 = 450.0f, g324 = 300.0f, g328 = 600.0f;
    int f32C = 0;                 // +0x32C (busy: 0x207598)
    int f330 = 0;                 // +0x330 the one-time knockout notification (0x179898)
    int n334 = 0, n338 = 0, n33C = 0, n340 = 0, n344 = 0;  // +0x334.. (options 332 / 254; +0x338 the hits' depth)
    ASEGA* pasega34C = nullptr;   // +0x34C
    uint32_t grf350 = 0x20F800, grf354 = 0x29FF7E, grf358 = 0xF800;  // +0x350 .. (the hits that matter by state)
    float g360 = -10.0f;          // +0x360
    LO* plo38C = nullptr;         // +0x38C the target's splice (0x208540)
    LO* ploTarget = nullptr;      // +0x390
    std::vector<int> acid398;     // +0x394 / +0x398 the classes it looks for (up to 3)
    int c3A4 = 0;                 // +0x3A4
    std::vector<int> acid3AC;     // +0x3A8 / +0x3AC excluded target classes (up to 5)
    std::vector<int> acid3C4;     // +0x3C0 / +0x3C4 immediate target classes (up to 2)
    struct TARGETREC* ptarget3CC = nullptr;  // +0x3CC the pursued object's record (0x1F8278(+0x38C), aipursuit.h)
    int n3D4 = 0;                 // +0x3D4
    int n3D0 = 0;                 // +0x3D0
    LO* ploE60 = nullptr;         // +0xE60 the alarm's template (cid 0x75; none: the world's 710)
    float tF44 = 0.0f;            // +0xF44 the next alarm
    int c1010 = 0;                // +0x1010 (class 0x3B's records: the count)
    // class 0x3B (the boss)'s escape records (+0x1014, 0x24 bytes each: added by EOPID 1340's loader method 0x10F6E8 ->
    // 0x155530, resolved by the post-load 0x155450): two ends {point A, its zone B} / {point C, its zone D}, +0x20 used
    struct BOSSREC { OID aoid[4] = {-1, -1, -1, -1}; LO* aplo[4] = {}; int fUsed = 0; };
    std::vector<BOSSREC> abossrec;
    int n1018 = 0;                // +0x1018 (class 0x3B: the hit points at the last escape; 0x7FFFFFFF at the init 0x1553F0)
    LO* plo1010 = nullptr;        // +0x1010 (class 0x3F: its aim point, a class 3 object oid 0x62B: 0x14B1D8)
    int n108C = 0;                // +0x108C (class 0x3C: non-zero while it has a command list to run, method 970)
    LO* plo108C = nullptr;        //   that command list (CMDLIST)
    int n1000 = 0;                // +0x1000 placed by a spawner (0x174490)
    K2PLAY k2play;                // +0xDB0 (the kind-2 entries' player)
    K3PLAY k3play;                // +0xE20 (the directional entries' player)
    SAVEDSCRIPT saved1090;        // +0x1090 its place in it (0x171C18)
    KINDE kinde1060;              // +0x1060 (class 0x3C: option 971: the alarm's animation, 0xAE state 2)
    float g1070 = 1.35f, g1074 = -0.0003f, g1078 = 0.0f;  // +0x1070 (option 972: the follow speed by distance)
    float g1080 = 0.9f, g1084 = 1.2f;  // +0x1080 (973: its clamp)
    float g1088 = 7.0f;           // +0x1088 (974: how long it waits for Sly before the alarm)
    int n3D8 = -1, n3F0 = -200, n3FC = -1;
    LO* plo3F4 = nullptr, * plo3F8 = nullptr;  // +0x3F4 / +0x3F8 (options 304 / 305)
    int f48C = 0;                 // +0x48C
    LO* plo494 = nullptr;         // +0x494 (option 306)
    WATCHA watchA;                // +0x498
    WATCHB watchB;                // +0x49C
    int f4A0 = 1, f4A4 = 0;       // +0x4A0 / +0x4A4
    glm::vec4 pos4B0{0.0f};       // +0x4B0
    std::vector<KINDE> akinde;    // +0x4C0 count, +0x4D0 (up to 32)
    std::vector<std::unique_ptr<KIND3>> akind3;  // owns the entries' stable kind-3 tuning objects
    std::array<std::array<KINDE, 26>, 3> aakinde;  // +0x8D0 / +0xA70 / +0xC10
    float g260 = 1.5f;            // +0x260 the size it starts with (0x205EB8)
    int nE64 = 1, nE68 = -1, nE78 = 0, nE84 = 50, nE88 = 50;
    float gE6C = 5.5f, gE70 = 6.5f, gE7C = 16.0f, gE80 = 20.0f;
    LO* ploE74 = nullptr;         // +0xE74 (option 327)
    ASEG* pasegE8C = nullptr;     // +0xE8C
    ASEGA* pasegaE90 = nullptr;   // +0xE90
    std::vector<OID> aoidEB8;     // +0xEB8 (up to 8; option 233)
    std::vector<LO*> aploEDC;     // +0xEDC
    LO* ploF00 = nullptr;         // +0xF00 held object
    float tF04 = 0.0f;            // +0xF04 the next action's time (the selector's cooldown)
    int fF3C = 0;                 // +0xF3C striking (0x9F)
    int fF0C = 0;                 // +0xF0C
    float gF30 = 0, gF34 = 0;     // +0xF30 / +0xF34 (options 320 / 321)
    int nF40 = 1;
    float gF48 = 300.0f, gF4C = 1200.0f;  // +0xF48 (option 298)
    float radF50 = -0.785398f, radF54 = 0.785398f, radF58 = -1.30899692f, radF5C = 1.30899692f;
    int fF60 = 0;                 // +0xF60 shooting (0xAD: set by its clip's events 0x31 / 0x32)
    DETECTOR* pdet = nullptr;     // +0xF64 its detector (cid 0x10 oid 648)
    LO* ploF68 = nullptr;         // +0xF68
    glm::vec4 posF70{0.0f};       // +0xF70
    int kMove = -1;               // +0xF80 how it moves (-1 walking, 0 / 1 ..., 2 / 3)
    LO* ploMove = nullptr;        // +0xF84 what it moves on (0x20B2A8: a ladder (1), a chair (0), a bar (2), 0x27 (3))
    float sMove = 0;              // +0xF88 its place along that object's curve
    int nMoveKind = 0;            // +0xF8C the kind given with it
    int fF94 = 0, fFA8 = 0;       // +0xF94 / +0xFA8 (busy): the counts of the two object lists below
    LO* aploF98[4]{};             // +0xF98 (option 426's first list: the objects it attacks, job 0xB2's choices)
    LO* aploFAC[4]{};             // +0xFAC (option 426's second list: the ones it turns to when near, e.g. Sly)
    int nMode = -1;               // +0xFBC (0x207030)
    float tMode = 0;              // +0xFC0
    CTRLPOS* pscaleFC4 = nullptr; // +0xFC4 the guard's kind-12 scale controller
    int fFC8 = 0;                 // +0xFC8
    LO* ploFCC = nullptr;         // +0xFCC (the stand job faces it)
    MOVEREF move;                 // +0xFD0
    uint32_t grfFE8 = 0;          // +0xFE8 (2 a walk, 4, 1, 8: from the table)
    LO* ploFEC = nullptr;         // +0xFEC (option 331)
    float uGaitFF0 = 0, tGaitFF4 = -3.4028235e38f;  // +0xFF0 / +0xFF4
    float tFF8 = 0;               // +0xFF8
    uint32_t u1000 = 0;           // +0x1000
};

// orig: 0x00209098: the guard held still (+0xFC8, or the game paused for the guards)
inline bool FGuardStill(const GUARD* pg) { return pg->fFC8 != 0 || FGamePausedForAi(); }
// The guard's STEP fields (stepphys.h)
// orig 0x2DD648: the world's guard of class 0x3F (its InitLo 0x14B1A0)
extern GUARD* g_pguard3F;
inline STEPPHYS& St(const GUARD* pg) { return StepPhysOf(pg->palo); }
// a floor contact (STEP +0x208)
inline bool FGuardFloor(const GUARD* pg) { return pg->fProbeRam ? pg->fFloorRam : St(pg).pxpFloor != nullptr; }
// the floor probe's point (STEP +0x218: the custom entry's world point at the base)
glm::vec3 PosGuardProbe(const GUARD* pg);

// The guards' native data (made for each object of class 0x38 or derived at the world's start)
GUARD* PguardFromLo(LO* plo);
void AttachGuardMove(GUARD* pg, LO* plo, int n);  // orig 0x20B2A8
void DetachGuardMove(GUARD* pg);                  // orig 0x20B210
bool FLoGuard(const LO* plo);
GUARD* PguardNew(ALO* palo);
void ResetGuards();
void ResetGuardDetectors();  // world-owned detector data and guard-to-LO bindings
std::vector<GUARD*> ApguardAll();

// orig: 0x00207A20 / 0x00207A40: the mask bit of an index (-1: none)
inline uint64_t GrfStateBit(int n) { return n == -1 ? 0 : (1ull << (static_cast<unsigned>(n) & 63)); }
// orig: 0x00209A68: index n in the guard's mask
inline bool FGuardState(const GUARD* pg, int n) { return (pg->grfState & GrfStateBit(n)) != 0; }
// orig: 0x00209870: the mask set (0x1F1900: the reward giver's mode)
void SetGuardStateMask(GUARD* pg, uint64_t grf);
void SetGuardPatrol(GUARD* pg, CMDLIST* pcmdlist);  // orig 0x20A540
// orig: 0x00209890: index n added (the guard's splice event 7 {n, -1}; 0x2C3D98's queued messages)
void AddGuardState(GUARD* pg, int n);
// orig: 0x00209970: index n removed (event 7 {-1, n})
void RemoveGuardState(GUARD* pg, int n);
// orig: 0x00209AD0: the table entry with the most indices inside mask that has its lowest index
KINDE* PkindeBest(GUARD* pg, uint64_t mask);
// orig: 0x00209BC0
inline KINDE* PkindeBestOfGuard(GUARD* pg) { return PkindeBest(pg, pg->grfState); }
// orig: 0x00209278: an entry played on the guard (k 0 ApplyAseg, 1 the SM, 2 / 3 their players) from time g
void PlayKinde(KINDE* pe, GUARD* pg, int grf, float g);
// orig: 0x00209360: the entry's playing object stopped
void StopKinde(KINDE* pe);
// orig: 0x00209430: the entry's playing object gone or finished
bool FKindeDone(const KINDE* pe);
// orig: 0x00209130: the entry resolved (its oid found around the guard: ASEG k 0 (k 2 with event 0x22), SM k 1)
void ResolveKinde(KINDE* pe, GUARD* pg);
// orig: 0x00209618: the guard's steering from the entry: the root motion's velocity (+0x190, minus, scaled by
// +0xFC4) and rotation (+0x1E0)
void SteerByKinde(KINDE* pe, GUARD* pg);
// orig: 0x00209818: the heading (unless locked)
void SetMoveHeading(MOVEREF* pm, float rad);

// orig: 0x00207350: the guard's update (vt slot 16)
void UpdateGuard(GUARD* pg);
// orig: 0x00206EB0 / 0x00207030 / 0x00207288: timed effects and their full exit cleanup.
bool FGuardModeAllowed(GUARD* pg, int n);
void SetGuardMode(GUARD* pg, int n);
void UpdateGuardTimedMode(GUARD* pg);
// orig: 0x00206900: rebuild the nearby class targets, then update detection.
void RefreshGuardTargets(GUARD* pg);
// orig: 0x002082D0: the target (the detector's entries, the current one while alive)
LO* PloGuardTarget(GUARD* pg);
// orig: 0x002087E8 / 0x002089F0 / 0x00208B38: plo of one of the classes (or derived)
bool FTargetClass(const std::vector<int>& classes, const LO* plo);
// orig: 0x0020A408: the best object of the 0x3E148C list for the guard (0: none)
LO* PloGuardInterest(GUARD* pg, LO* ploCur);

// orig 0x1A62E8: an object's world position (an ALO: its object record +0x60's)
glm::vec3 PosAi(const LO* plo);

// The heading (orig 0x204660 / 0x204740)
void SetGuardHeading(GUARD* pg, float rad);
float RadGuardHeading(GUARD* pg);
// orig: 0x00207598 (vt slot 76: busy: +0x32C, +0x298 or state 42)
bool FGuardBusy76(GUARD* pg);

// The guards' start (guardstart.cpp): brx.cpp's InitLo pairs a guard with its detector object; 0x2061D8 (slot 15 in
// StartSw), 0x206518 (slot 26: the proxy's attach, activate.cpp), the guards in the world at the start reset
void OnGuardInitLo(LO* pguard, LO* ploDetector);
void PostLoadGuard(LO* plo);
void SyncGuardOption(GUARD* pg, const LO* plo, int eopid);  // guardstart.cpp: a loaded field option into the guard
void ResetGuard(LO* plo, LO* pproxy);
void ResetGuardsAtStart(SW* psw);

