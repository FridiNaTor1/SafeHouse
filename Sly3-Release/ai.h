#pragma once

// The AI "jobs" (Sly 2's behaviour layer; the stack 0x1EEC60..0x1EF850, the jobs 0x1709A8..0x17F090): an actor
// (the guards and NPCs, base class 0x38: guard.h) runs a stack of up to 8 jobs, each the child of the one below
// (+0x14 parent, +0x18 child). The bottom one (0x9C, the reactions) picks what to do from the actor's situation and
// starts a child for it (a patrol script 0xA9, a wait 0xAA, an attack ...); the children start their own (a path
// step 0xC0 -> a go-to 0xBA -> a path follower 0xA7 -> a move 0x9D that plays the animation that moves the actor).
// Each frame the actor updates every job of its stack, top first (vt slot 10), and steers by the current move
// (slot 11).
//
// The jobs are classes 0x9C..0xC6 (BASIC objects constructed in the stack's 0xF0-byte slots by 0x1EF138); natively
// each kind is a struct derived from AIJOB with the original's vtable slots as virtual functions:
//   7 Init, 8 Enter, 9 Exit, 10 Update, 11 Steer, 12 State (the parent's view), 13 Message, 15 SetState.
// Field comments give the original's offsets in the job.
//
// Part of the animation / AI code.

#include "cls.h"
#include "lo.h"
#include "aistack.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct ALO;
struct ACTOR;
struct PADCTL;
struct ASEG;
struct ASEGA;
struct CMD;
struct CMDLIST;
struct GUARD;
struct KINDE;
struct SM;
struct SMA;

// A command list's saved place (orig 0xC bytes: the guard's +0x2DC, 0x171C18)
struct SAVEDSCRIPT
{
    CMD* pcmd = nullptr;
    LO* plo = nullptr;
    float g = 0;
};

// A reaction message of the job 0x9C (0x70 bytes: kind, then the kind's arguments)
struct AIMSG
{
    int k = 0;                    // +0x0
    LO* plo = nullptr;            // +0x10 (an object argument)
    int n = 0;                    // +0x10 (an integer argument)
    float g = 0.0f;               // +0x14
    int n1C = 0;                  // +0x1C (kinds 9..11: compared)
    int n54 = 0;                  // +0x54
    // the hit's description (kinds 9 / 10 / 11 / 0xB copy +0x10..+0x6F: the contact message CTMSG)
    struct HIT
    {
        LO* psource = nullptr;    // +0x10 (CTMSG +0x0)
        const void* pvOther = nullptr;  // +0x14
        LO* preceiver = nullptr;  // +0x18
        uint32_t kind = 0;        // +0x1C (+0xC: the contact bytes)
        int n = 0;                // +0x20 (+0x10: the amount, negative)
        glm::vec3 pos{0.0f};      // +0x30 (+0x20)
        glm::vec3 vec{0.0f};      // +0x40 (+0x30: the push, set by 0x176BE8)
        uint32_t grf = 0;         // +0x54 (+0x44: the accepted bytes)
    } hit;
};

// orig 0x1EF138: the job objects (BASIC, cid 0x9C..0xC6)
struct AIJOB : BASIC
{
    std::shared_ptr<CFrame> pframe;  // +0x0 the active source object's splice frame
    std::shared_ptr<CSidebag> psidebag;  // +0x4 the job's private script bindings
    CID cid = 0;                  // the class (vt +0)
    GUARD* pguard = nullptr;      // +0x10 the actor
    ACTOR* pactor = nullptr;      // +0x10 for the player-actor jobs (0xA8 / 0xC2 / 0xC3)
    AIJOB* pparent = nullptr;     // +0x14
    AIJOB* pchild = nullptr;      // +0x18
    int fLocked = 0;              // +0x1C (0x1EEFC0 / 0x1EF038: the job holds the actor)
    int cLock = 0;                // +0x20 the jobs above holding it (JOB_FDONE: > 0 means done for this one)

    ~AIJOB() override = default;
    virtual void Init();          // slot 7 (base 0x1EEF68: nothing)
    virtual void Enter();         // slot 8 (0x1EEF70: nothing)
    virtual void Exit();          // slot 9 (0x1EEF78: its child ended)
    virtual void Update();        // slot 10 (0x1EEF98: nothing)
    virtual void Steer();         // slot 11 (0x1EEFB8: nothing)
    virtual int State();          // slot 12 (0x1EEFA0: 7 unless the actor's +0x204)
    virtual int Message(int msg, void* pm);  // slot 13 (0x1EF0B8: 0)
    virtual void Drive(PADCTL* pc);          // slot 14 (0x1EF0C0: rest, facing the owner's heading)
    virtual void SetState(int n);  // slot 15 (none in the base)
    // vt slot 3 (0x1331F8): the job, followed by the event's arguments, in its active frame.
    void Script(int k, int n = 0);
};

// orig 0x1EF0A8 JOB_FDONE: a job above holds the actor
inline bool FJobDone(const AIJOB* p) { return p->cLock > 0; }
// orig 0x1EEFC0: the job holds the actor (its parents' counts raised; a guard is told: 0x20B438)
void LockJob(AIJOB* p);
// orig 0x1EF038: the hold released
void UnlockJob(AIJOB* p);
// orig 0x1EF0D8: the child job ended (its Exit, then popped off the stack)
void EndChildJob(AIJOB** ppchild);

// Sly 3: the AI jobs keep Sly 2's class ids inside the AI code (JOB 0x9B .. 0xC6: their Sly 3 twins
// are 0xC7 .. 0xF8, maps/classes_sly2.tsv); a job's derivation is tested on Sly 2's job hierarchy (FJobCidDerivedFrom2),
// not the class table (Sly 3's). Ids that come from Sly 3 data (scripts, options) are converted at that boundary:
// Cid2FromCid3Job.
bool FJobCidDerivedFrom2(CID cid2, CID cidBase2);
CID Cid2FromCid3Job(CID cid3);   // a Sly 3 job class (0xC7..) as the AI code's Sly 2 id (-1: none)
// orig 0x1EEE00 (0x205438): a new job of the class pushed on the stack (the child of the top), Init'ed
AIJOB* PjobPush(GUARD* pguard, CID cid);
AIJOB* PjobPush(ACTOR* pactor, CID cid);
// orig 0x1EEEE0 (0x205458): the first job of the stack of the class (or derived)
AIJOB* PjobFind(GUARD* pguard, CID cid);
AIJOB* PjobFind(ACTOR* pactor, CID cid);
void ClearJobs(ACTOR* pactor);
void UpdateJobs(ACTOR* pactor);
// orig 0x1EED10: slot 10 of each job, top first
void UpdateJobs(GUARD* pguard);
// orig 0x1EED78: a message offered to the jobs top first until one takes it (slot 13)
void OfferJobMessage(GUARD* pguard, int msg, void* pm);
// orig 0x1EECD0: the stack emptied (the bottom job's handle released)
void ClearJobs(GUARD* pguard);
// orig 0x1EF138: a job of the class (null: not a job class natively)
std::unique_ptr<AIJOB> PjobNew(CID cid);

// The command lists (cid 0x71, module 0x1709A8): the scripts the jobs 0xA9 run (a patrol's moves along paths,
// a go-to, waits, animations ...). Nodes from the slot heap 0x3E1548 (0x1C bytes), linked at +0x18.
struct CMD
{
    int op = 0;                   // +0x0 (0 the near object, 1 / 2 ranges, 3 look, 4 message, 5 spawn, 6 move along a
                                  // path, 7 go to, 8 sound, 10 jump, 11 / 12 lock / unlock, 13 a sub-job 0xAC, 14 an
                                  // animation, 15 an alarm)
    OID oid = -1;                 // +0x4
    int n8 = 0;                   // +0x8 (ops 6, 13, 14 a word; 1 / 2 with nC the 8-byte range)
    float g8 = 0.0f;              // +0x8 (op 5: the wait)
    const glm::vec4* ppos8 = nullptr;  // +0x8 (op 7 with oid -1: the point)
    int nC = 0;                   // +0xC
    float gC = 0.0f;              // +0xC (op 7 with oid -1: the heading)
    int n10 = 0;                  // +0x10
    float g14 = 0.0f;             // +0x14
    // (ops 1 / 2: g8 / gC the range)
};
struct CMDLIST : LO  // cid 0x71 (0x60 bytes)
{
    std::vector<std::unique_ptr<CMD>> acmd;  // +0x50 the DL
    CMD* PcmdFirst() const { return acmd.empty() ? nullptr : acmd.front().get(); }
    CMD* PcmdNext(const CMD* p) const;
};
LO* PloNewCmdlist();
// orig 0x170E00: a new command appended
CMD* PcmdAppend(CMDLIST* pl, int op, OID oid);
// orig 0x170A58: the list's post-load (0: the context object; 6: the path found is made usable by the AI 0x1E9C30)
void PostLoadCmdlist(CMDLIST* pl);

// (cid 0x77 are the spawners: spawner.cpp, the world code; their path sources (kind 1) get a 0x71 list of one
// command 6 {n8 1, nC -1} (0x1733F8): PcmdAppend.)

// The game's pause counts (orig SW +0x3D78 all, +0x3D80 grf 1, +0x3D84 grf 2): a dialog / cut scene / menu pauses the
// game with grf 2 for the guards (their waits stand alarmed, their detectors see nothing while it is positive).
struct GAMEPAUSE
{
    int c78 = 0, c7C = 0, c80 = 0, c84 = 0;  // SW +0x3D78 / +0x3D7C (the talks) / +0x3D80 / +0x3D84
};
extern GAMEPAUSE g_gamepause;
// orig: 0x0020EFC0 (the first one locks the players' pad: joy lock bit 2, 0x187078)
void PauseGame(int grf);
// orig: 0x0020F030 (the last one unlocks it (0x1870A0) and drops a held Start (0x187E08))
void ResumeGame(int grf);
// orig: 0x0020F0C0 / 0x0020F0F0 (a talk: SW +0x3D7C; the first locks the whole pad, joy lock bit 4)
void PauseTalk();
void ResumeTalk();
// orig: 0x0020F1B8: the guards' pause (+0x3D84 positive)
inline bool FGamePausedForAi() { return g_gamepause.c84 > 0; }

