#pragma once
// pocket.h: the prize givers' pocket (the guards' and the other givers' base, orig module 0x1F0FA0..0x1F1F38: the
// entries +0x404 (8 x {oid, +4, +8, count}), +0x484 their count, +0x488 the next, +0x48C ready, +0x490 more allowed,
// +0x494 the pocket object (a guard's option 306), the glow +0x3FC / +0x400) and Sly's side of the pickpocket
// (state 11: the hand pose 0x18B460 / 0x18B348, the take at the clip's event 0x167). Ported with the actors code
// (the AI code's guard keeps the pocket object, GUARD::plo494; the rest is kept here by object).
#include "lo.h"

struct SLY;

struct POCKETENT
{
    int oid = 0;     // +0x0 the prize (0x2B3: coins)
    int n4 = 0;      // +0x4
    int n8 = -1;     // +0x8 (0x1C2DC8's)
    int c = 1;       // +0xC the count
};
struct POCKET
{
    POCKETENT ae[8];
    int c = 0;           // +0x484
    int i = 0;           // +0x488
    int fReady = 0;      // +0x48C
    int fMore = 0;       // +0x490
    int nMode = -1;      // +0x3FC (-1 / 0 / 1 / 2)
    float tMode = 0.0f;  // +0x400
};

POCKET& PocketOf(LO* plo);           // (created empty)
void ResetPockets();                 // a new world
LO* PloPocket(LO* plo);              // +0x494 (the guard's option 306)
struct SMA;
SMA** PpsmaPocket(LO* ppocket);         // orig 0x001C2D20 (the pocket object's SM +0x16C, made at the first use)
void AddPocketPrize(LO* plo, int oid);   // orig 0x001F17B0
void AddPocketCoins(LO* plo, int c);     // orig 0x001F1768
void SetPocketMode(LO* plo, int n);      // orig 0x001F17F8
void UpdatePocketMode(LO* plo);          // orig 0x001F1900
void FillPocket(LO* plo);                // orig 0x001F1A40
void NextPocketPrize(LO* plo);           // orig 0x001F1CF8
LO* PloTakePocket(LO* plo);              // orig 0x001F1E20

// The pocket object's reward (+0x150 the launch point record, +0x154 the proxy, +0x158 the instance)
void ClearReward(LO* ppocket);                                      // orig 0x001C2DB8
struct XF* PxfRewardPoint(LO* ppocket);                            // the pocket object's +0x150 (null: none readied)
int NReadyReward(LO* ppocket, LO* plo, int id, int oidPoint);       // orig 0x001C2DC8
LO* PloLaunchReward(LO* ppocket, int c);                            // orig 0x001C2EF0

struct SCENE;
struct EXTRADRAW;
// orig: 0x001C2D70: the readied reward is drawn at the pocket's launch record.
void CollectPocketDraws(const SCENE& scene, std::vector<EXTRADRAW>& draws);

float GEventAseg11(struct ASEG* paseg, int n);  // (sly.cpp: orig 0x12F990)
void LoadPickpocket(SLY* p, struct ASEG* paseg1BC8);             // orig 0x0018B348 (Sly's load: the take's time in the clip +0x1BC8)
void UpdatePickpocketPose(SLY* p);       // orig 0x0018B460 (state 11's input)
void PickpocketTake(SLY* p);             // the clip's event 0x167 (0x188F20's part)
