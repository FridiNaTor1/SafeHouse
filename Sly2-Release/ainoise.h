#pragma once

// The interest objects ("noises", module 0x1564E8..0x157200): pooled points of interest the guards may react to (a
// noise heard, a guard's alarm about a target, objects found), made from a template (cid 0x75: +0x50 how it is
// perceived (0 within its radius, 1 seen through the guard's view volume), +0x54 the radius, +0x58 the height range,
// +0x60 the life (the radius grows over it; -1 kept), +0x64 its kind; the world's fixed ones oid 706..712 at SW
// +0x4020..+0x4038, 0x1569B8), listed at 0x3E148C, expired by the pass 0x156728. Part of the AI code.
//
// API for other code (a player's noise, an object found): `PnoiseNew(PtmplNoise(plo or the fixed index), pos,
// owner)`; the guards pick theirs with PloGuardInterest (0x20A408).

#include "lo.h"

#include <glm/glm.hpp>

#include <vector>

struct GUARD;
struct SW;

struct NOISETMPL
{
    LO* plo = nullptr;
    int nType = 0;                 // +0x50
    float sRadius = 1500.0f;       // +0x54 (D_002BDCD0)
    glm::vec2 rz{-200.0f, 500.0f}; // +0x58 (D_002BDCD8)
    float tLife = 0.25f;           // +0x60
    int nKind = 0;                 // +0x64
};
// the template of a cid-0x75 object (its fields / options 1090..1094; the fixed ones by their oid)
NOISETMPL* PtmplNoise(LO* plo);
// the world's fixed templates (SW +0x4020 + 4 i: 706, 707, 708, 709, 710, 711, 712)
NOISETMPL* PtmplNoiseFixed(int i);

// A noise (orig 0x60 bytes)
struct NOISE : LO
{
    NOISETMPL* ptmpl = nullptr;    // +0x14
    glm::vec3 pos20{0.0f};         // +0x20 where
    glm::vec3 pos30{0.0f};         // +0x30 (an alarm: the target's place)
    LO* ploOwner40 = nullptr;      // +0x40 who made it
    LO* ploTarget44 = nullptr;     // +0x44 (kind 4: the target)
    int nSerial = 0;               // +0x48
    float t = 0.0f;                // +0x4C
    int nKind = 0;                 // +0x54
    int cRef = 0;                  // +0x0 the references (0x1566B8 / 0x1566C8): the list's, the holders'
    float tFree = -1.0f;           // (native) when its last reference went (pooled for reuse)
};
// orig: 0x001566B8 / 0x001566C8 (vt slots 1 / 2: the last release gives it back to the pool SW +0x4008, reused by
// the next PnoiseNew; native: reused only from a later frame, so a pointer still in use within the frame stays valid)
void AddRefNoise(NOISE* pn);
void ReleaseNoise(NOISE* pn);
// orig: 0x00156BE0 (a held noise: the old one removed and released, a new one of ptmpl made and held; null: none)
void SetHeldNoise(NOISE** ppn, NOISETMPL* ptmpl, const glm::vec3& pos, LO* ploOwner);
// (debug / tests) the noises allocated (live, held or pooled) and the pooled ones
size_t CNoiseAllocated();
size_t CNoisePooled();
// (native) pn is one of this world's noises (live, held or pooled): a pointer kept across a world change is not
bool FNoiseAllocated(const NOISE* pn);
// orig: 0x00156548 / 0x00156698
NOISE* PnoiseNew(NOISETMPL* ptmpl, const glm::vec3& pos, LO* ploOwner);
// orig: 0x00156780
void RemoveNoise(NOISE* pn);
// orig: 0x00140F90 event0x27C: remove kind4 alarms about this actor in list order.
int RemoveActorAlarms(LO* actor);
bool FNoiseLive(const NOISE* pn);  // orig 0x156870 (inverted)
const std::vector<NOISE*>& ApnoiseAll();
void ResetNoises();
// orig: 0x00156D58 / 0x00156DE0(true), after the base removal notification.
void OnRemoveNoiseListener(LO* plo);
// orig: 0x00156728 (the world pass: the old ones removed)
void ExpireNoises();
// orig: 0x00156898 (the radius, growing over the life)
float SRadiusNoise(const NOISE* pn);
// orig: 0x00156918 (a point within the height range and the radius)
bool FNoiseReaches(const NOISE* pn, const glm::vec3& pos);

// orig: 0x00156FE8 (a listener object, cid 0x2F, hears the noise) / any listener of the world (list 0x3E1468)
bool FListenerHearsNoise(const LO* plo, const NOISE* pn);
bool FNoiseListenersReach(const NOISE* pn);

// The guards' side (orig 0x208BE0 / 0x20A168 / 0x20A408 / 0x20B140)
bool FGuardAcceptsNoise(GUARD* pg, NOISE* pn);
int NCompareNoises(GUARD* pg, const NOISE* a, const NOISE* b);
// orig: 0x0020B140: a pursuing guard's alarm about its target (kind 4 at the guard, the target's place; +0xF44)
void RaiseGuardAlarm(GUARD* pg, LO* ptarget);
// orig: 0x00140F90 / 0x00176098, GAME +0x4758. Smoke delays pursuit alarms;
// the world reset at 0x20E890 clears the deadline along with the GAME record.
void SetGuardAlarmResume(float t);
float TGuardAlarmResume();
// (harnesses: the list replaced by a reference's)
void ClearNoiseList();
void ImportNoise(NOISETMPL* ptmpl, const glm::vec3& pos20, const glm::vec3& pos30, LO* powner, LO* ptarget, int nSerial, float t,
                 int nKind);


#include "aijobs.h"

// 0xAB (0xE0): the investigation of a noise (Init 0x171F58, Enter 0x171FA0, Exit 0x171FD0, Update 0x172018, SetState
// 0x172320: 0 alerted (0x9D nAnim 12), 1 going there (0xBA), 2 looking (facing it, 0x9E, a wait), 3 searching around
// (0xBA over waypoints 0x1728B0: TODO), 4 giving up (0x9D nAnim 14), 5 the game paused (0x9E nAnim 1))
struct JOBINVESTIGATE : AIJOB
{
    int st = -1;                  // +0x30
    float tSt = 0;                // +0x34
    float tEnd = 3.4028235e38f;   // +0x38 when it gives up
    float tSearch = 0;            // +0x3C when it starts searching around
    float t40 = 3.4028235e38f;    // +0x40
    NOISE* pnoise = nullptr;      // +0x44
    int fNew = 0;                 // +0x48 a new noise to follow
    void Init() override;
    void Enter() override;
    void Exit() override;
    void Update() override;
    void SetInvestigateState(int n);  // orig 0x172320
    void SetNoise(NOISE* pn);     // orig 0x1725E0
    glm::vec3 PosNoise() const;   // orig 0x172760
    bool FNear();                 // orig 0x172658
    void SetDeadlines();          // orig 0x172560
    glm::vec3 aposWay[4]{};       // +0x50 the search's places (each 0x20: the goal, then the place it is near)
    glm::vec3 aposWayNear[4]{};
    int iway = 0;                 // +0xD0
    void FindSearchPlaces();      // orig 0x1728B0
    void NextSearchPlace();       // orig 0x172F10
};
std::unique_ptr<AIJOB> PjobNewNoise(CID cid);
