#pragma once

// SMA: a state machine (SM, sm.h) running on an object (Sly 1's sm.c SMA; Sly 2 0x1F00C8-0x1F14F8): its current
// state, its goal, the transition (an ASEG) playing between them. Part of the animation code.

#include "asega.h"

// orig 0x50 bytes (pool 0x3E1278, vtable 0x2D9768, list 0x3E126C)
struct SMA : ASEGALISTENER, BASIC  // (a BASIC: the scripts get it as their object)
{
    int cref = 0;                 // +0x10
    SM* psm = nullptr;            // +0x24
    ALO* palo = nullptr;          // +0x28
    int grfapl = 0;               // +0x2C
    ASEGA* pasega = nullptr;      // +0x30: the transition playing
    int ismsCur = 0;              // +0x34
    int ismsNext = -1;            // +0x38: the state the transition leads to
    int ismsGoal = 0;             // +0x3C
    SM::SMT* psmt = nullptr;      // +0x40
    float svtLocal = 1.0f;        // +0x44
    uint32_t grfMask = 4;         // +0x48: the transitions allowed
    bool fRetracted = false;
    void OnAsegaMsg(ASEGA* pasega, int msgk) override;  // orig 0x1F1078
};

extern std::vector<SMA*> g_apsma;

// The message 10 of a state change (0x1F13A8: {the SMA, the old state's oid, the new one's}) to the SMA's object
struct SMAMSG10
{
    SMA* psma;
    OID oidOld;
    OID oidNew;
};

// vt slot 3 of an SMA (0x1331F8 in the SM's module frame, the SMA as the handler's object): 2 at its start
// (0x1F01B8), 8 at a state change (0x1F13A8: the old and new states' oids); set by the world side (update.cpp)
extern void (*g_pfnSmaScript)(SMA* psma, int symid, int nA, int nB);

// The state machine's runtime tables from its loaded data (orig: 0x1EFAC0's end: indices, distance matrices).
void InitSm(SM* psm);
// orig: 0x001F00C8 PostSmLoad: the initial SMA (EOPID 370 {object oid, state oid, grfapl})
void PostSmLoad(SM* psm);

// orig: 0x001F01B8 / 0x1F0380 / 0x1F03A8: a new SMA running psm on palo from state oid (-1: none)
SMA* PsmaApplySm(SM* psm, ALO* palo, OID oid, int grfapl, uint32_t grfMask = 4);
// orig: 0x001F03C8
SMA* PsmaFindSm(SM* psm, ALO* palo);
// orig: 0x001F0400 IsmsFindSmOptional
int IsmsFindSm(const SM* psm, OID oid);
// orig: 0x001F04F8
OID OidFromSmIsms(const SM* psm, int isms);
// orig: 0x001F0598 RetractSma
void RetractSma(SMA* psma);
// orig: 0x001F0638
bool FSmaRetracted(const SMA* psma);
// orig: 0x001F0660 SetSmaGoal
void SetSmaGoal(SMA* psma, OID oid);
// orig: 0x001F0730 SeekSma
void SeekSma(SMA* psma, OID oid);
// orig: 0x001F09D8 (jumps to the state oid)
void SetSmaState(SMA* psma, OID oid);
// orig: 0x001F0948 (the goal oid, reached at once: the transitions snap to their ends)
void SnapSmaGoal(SMA* psma, OID oid);
// orig: 0x001F0850
void SetSmaSpeed(SMA* psma, float svt);
// orig: 0x001F1350
void SetSmaMask(SMA* psma, uint32_t grf);
// orig: 0x001F07C8 / 0x001F07E8 / 0x001F0808
OID OidSmaGoal(const SMA* psma);
OID OidSmaCur(const SMA* psma);
OID OidSmaNextOrCur(const SMA* psma);
// orig: 0x001F1448 / 0x001F14F8: a handle replaced (the old SMA retracted and released, a new one of psm on palo
// from state oid when psm is not null); orig 0x001F15B8: cleared
void ReplaceSmaHandle(SM* psm, ALO* palo, OID oid, int grfapl, SMA** ppsma);
void ReplaceSmaHandleMask(SM* psm, ALO* palo, OID oid, int grfapl, uint32_t grfMask, SMA** ppsma);
void ClearSmaHandle(SMA** ppsma);
// orig: 0x001F0D88 ChooseSmaTransition
void ChooseSmaTransition(SMA* psma, bool fReverse);
