#pragma once

// World changes (Sly 1's TRANSITION / CHKMGR): a request (orig 0x22B490: the world's checkpoint record, the
// point and key objects, flags), the wipe that hides it (0x22B220 / 0x22B090 / 0x22B198 / 0x22B420: the world's
// sprite set oid 778 plays its out / in animations), the load itself at the top of the next frame (0x20E890:
// the old world unloaded, the new one loaded and started) and the checkpoint context (0x227FE8 / 0x228750).
// Also the routes the reference's host/devwarp.cpp uses: a world record (0x210990 + 0x22B490) and a task start
// (0x1651D8).

#include "disc.h"
#include "lo.h"

#include <cstdint>
#include <memory>
#include <vector>

struct WREC;

struct SW;

struct TRANSREQ
{
    int flags = 0;          // 2: the same world; 8: a task start; 0x40 ...
    uint8_t abChk[64]{};    // the world's 64-byte key record (orig CHK)
    OID oidPoint = -1;      // the checkpoint (class 0x2A objects by oid or by their id list)
    OID oidKey = -1;        // the object the point is searched under (-1: global)
};

enum WIPES { WIPES_Idle = 0, WIPES_Out = 1, WIPES_In = 2, WIPES_Loading = 3 };

struct TRANSITION
{
    bool fPending = false;      // orig SW +0x3BF0
    TRANSREQ req;               // orig SW +0x3BF4
    uint8_t abChkCur[64]{};     // orig SW +0x3C40: the loaded world's record
    bool fHaveCur = false;
    OID oidPointCur = -1, oidKeyCur = -1;  // orig SW +0x3C88 / +0x3C8C
    int iwrecCur = -1;          // the world record index (the filter)
    OID oidTaskQueued = -1;     // orig 0x2DEB30 +0x18 (0x1651D8: started after the load)
    // the wipe (orig3 0x4C4090: +0 the state, +4 a request held, +8 it, +0x50 the wipe screen object, +0x54 its oid,
    // +0x58 / +0x5C the parts it plays out / in, +0x60 wipe 875's bag of part pairs)
    WIPES wipes = WIPES_Idle;
    bool fWipeReq = false;
    TRANSREQ reqWipe;
    LO* ploWipe = nullptr;
    OID oidWipe = -1;
    int idWipeOut = -1, idWipeIn = -1;
    int aipairWipeBag[3]{0, 1, 2};  // (0x2BFED8 / 0x2C0060: the pairs' order, drawn from the front)
    int cpairWipeDrawn = 3;         // (all drawn: shuffled again at the next draw)
    LO* ploPlayer = nullptr;    // the player chosen at the load
};

extern TRANSITION g_transition;
extern int g_cTransitionLoads;

// orig 0x22B490: a world change to the checkpoint record pchk (null: the current world), with a wipe by the
// object oidWipe (-1: none)
void RequestTransition(SW* psw, const uint8_t* pchk, OID oidPoint, OID oidKey, int flags, OID oidWipe);
// the reference's warp routes: world record iwrec at point oidPoint (default 393 = 0x189), or task oidTask
void RequestWorldWarp(SW* psw, int iwrec, OID oidPoint = 0x189, OID oidWipe = 0x30A);
void RequestTaskStart(SW* psw, int iwrec, OID oidTask);
// the world records (the boot world's, loaded on first use)
void EnsureWorldRecords();
// orig 0x3E1C40.. (the loaded world's records), 0x210AE0 (the current world's episode), 0x210B98 (an episode's
// main world record, null when none)
const std::vector<WREC>& AwrecWorlds();
// (Sly 3, the flow log / map: the world index of a key record (-1: none), and a key named "world N name" / "movie ID")
int IworldOfChk(const uint8_t* pchk);
std::string StrKeyRecord(const uint8_t* pchk);
int IepisodeCur();
const WREC* PwrecEpisodeMain(int iepisode);

// orig 0x20E068: the old world's end and its objects freed (psw null after it)
void UnloadWorld(std::unique_ptr<SW>& psw, bool fEndTasks);
// orig 0x20E890 (the top of every frame): the pending world change. Returns the world (a new one after a load).
std::unique_ptr<SW> PswUpdateTransition(std::unique_ptr<SW> psw);
// orig 0x22B198 (from the HUD's update 0x1D9648): the wipe's progress
void UpdateWipe(SW* psw);
// the wipe's current cover 0..1 for the renderer (0: none)
float UWipeCover();
// orig 0x228170 (class 0x29, the warp point: its method 235, thunk 0x10B760; SCPT command 0x16 onto one): the
// current player placed at it (its vt slot 38), then the point's slot 3 with 5 (its splice event 5)
void TriggerWarp(LO* pwarp);
