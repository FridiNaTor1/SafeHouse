#pragma once

// ASEGA: a playing animation segment (Sly 1's asega.c; Sly 2 module 0x12F8C0-0x131E18): its time and speed, the
// channel controllers it puts on the transform records of the objects it animates (xf.h), its events, wrapping /
// ping-pong at the end and its retraction. ApplyAseg (0x12F9B8) starts one.
//
// Part of the animation code. Events that belong to other systems (sounds, particles, the camera, scripts,
// object messages) go through the hooks below; until their owners set them they are counted and dropped.

#include "basic.h"
#include "sm.h"
#include "xf.h"

#include <functional>
#include <vector>

struct ASEGA;

// A scalar link (orig 0x14 bytes from the slot heap 0x3E1254): the second channel list's track driving an
// object's parameter (vt slot 43 gives its address).
struct ASEGAL
{
    const TRKG* ptrkg = nullptr;  // +0x0
    float* pg = nullptr;          // +0x4
    LO* plo = nullptr;            // +0x8
    int kind = 0;                 // +0xC
};

// A listener of an ASEGA (the original's BASIC listener list +0x8: an SMA waiting for its transition's end).
struct ASEGALISTENER
{
    virtual ~ASEGALISTENER() = default;
    virtual void OnAsegaMsg(ASEGA* pasega, int msgk) = 0;  // the listener's vt slot 5
};

// orig 0x60 bytes (slot heap 0x3E1224, vtable 0x2CAF48; a BASIC: the ASEG's scripts get it as their object)
struct ASEGA : BASIC
{
    int cref = 0;                     // +0x10
    ASEG* paseg = nullptr;            // +0x14
    ALO* palo = nullptr;              // +0x18 (null: the objects of the ASEG's namespace)
    float t = 0.0f;                   // +0x1C
    float tEvent = 0.0f;              // +0x20: the event scan's time
    CTRLASEG* pctrlFirst = nullptr;   // +0x34: the channels' controllers
    std::vector<ASEGAL> alink;        // +0x38: the scalar links
    int grfapl = 0;                   // +0x3C: 1 seek into it (mode 7, else 5), 2 events from the start, 4 snap
                                      // seeking records too, 8 loop, 0x10 ping-pong, 0x20 / 0x38: kept at the end
    float svt = 0.0f;                 // +0x40: the effective speed
    float svtLocal = 1.0f;            // +0x44: the speed asked for
    float svtAseg = 1.0f;             // +0x48: the ASEG's speed (its +0x78)
    float svtSuck = 1.0f;             // +0x4C: from the suck history (0x154C90) by the ASEG's polynomial
    int iea = 0;                      // +0x50: the next timed event
    int ieaEnd = 0;                   // +0x54: the end list's next event (fired at the retraction)
    int fPause = 0;                   // +0x58: the game paused while it plays (0x131990: PauseGame / ResumeGame (3))
    std::vector<ASEGALISTENER*> aplistener;
    bool fRetracted = false;          // not in the global list (0x3E1218) any more
};

// The ASEG's runtime fields (orig +0x40.., options / setters).
ASEGD* PasegdOf(ASEG* paseg);  // a clone's data is its original's (the loader does not copy ASEGD)

// orig: 0x0012FE58: a channel's object (found around palo, else around the ASEG) and its record
void GetAsegChannelTarget(ASEG* paseg, ALO* palo, const ASEGCHN& chn, ALO** ppalo, XF** ppxf);
// The ASEGA's splice events (orig vt slot 3, 0x1331F8: 2 applied, 3 retracted, 4 the event kind 12 with its word),
// set by update.cpp: the ASEG's module frame binds them
extern void (*g_pfnAsegaScript)(ASEGA* pasega, int symid, int n);
// (PC addition, debug menu only: true while pccharacter.cpp applies / retracts a guest puppet's animation in its kept
// world: the clip's events and scripts are not fired there (the puppet fires the ones it wants itself); a start
// event's explosion run then re-initialised the running world's emitters as the kept world's, R36)
extern bool g_fAsegaEventsMuted;
// orig: 0x0012F9B8 ApplyAseg: a new ASEGA playing paseg on palo (null: the ASEG's own objects) from time t
void ApplyAseg(ASEG* paseg, ALO* palo, float t, float svt, int grfapl, ASEGA** ppasega);
// orig: 0x00130488
ASEGA* PasegaApplyAseg(ASEG* paseg, ALO* palo, float t, float svt, int grfapl);
// orig: 0x001305B8 ApplyAsegCur (replaces *ppasega)
void ApplyAsegCur(ASEG* paseg, ALO* palo, float t, float svt, int grfapl, ASEGA** ppasega);
// orig: 0x001308A8 / 0x001308B8 (a holder's reference; freed by CollectAsegas when none is left)
void AddRefAsega(ASEGA* p);
void ReleaseAsega(ASEGA* p);
// orig: 0x00130658 (retracts *ppasega and clears it)
void RetractAsegaPp(ASEGA** ppasega);
// orig: 0x001304A8 PasegaFindAseg (the ASEGA of paseg playing on palo)
ASEGA* PasegaFindAseg(ASEG* paseg, ALO* palo);

// orig: 0x001304E0 EnsureAseg: paseg's ASEGA on palo (applied when there is none), at time t (k: SetAsegaTime's)
void EnsureAseg(ASEG* paseg, ALO* palo, int k, float t, float svt, int grfapl, ASEGA** ppasega);
// orig: 0x00130598
ASEGA* PasegaEnsureAseg(ASEG* paseg, ALO* palo, int k, float t, float svt, int grfapl);
// orig: 0x00131A40 SetAsegaPriority (its controllers' priority, the records' lists sorted again)
void SetAsegaPriority(ASEGA* pasega, int prio);
// orig: 0x00131B10: the ASEGA's controller on the record gets priority -1
void DemoteAsegaOnXf(ASEGA* pasega, ALO* palo, XF* pxf);
// orig: 0x00131B60: the same on palo's records whose (tag, kind) match one of the pairs (-1: any)
void DemoteAsegaOnAlo(ASEGA* pasega, ALO* palo, const std::vector<std::pair<int, int>>& apair);

// orig: 0x00130918 UpdateAsega
void UpdateAsega(ASEGA* pasega);
// orig: 0x00130B18 RetractAsega (the end list's events, the controllers off, the listeners told)
void RetractAsega(ASEGA* pasega);
// orig: 0x00130D30 SetAsegaTime: k 0 absolute, 1 relative, 2 from the end; and the speed
void SetAsegaTime(ASEGA* pasega, int k, float g, float svt);
// orig: 0x00131A00 SetAsegaSpeed
void SetAsegaSpeed(ASEGA* pasega, float svt);
// orig: 0x00131A20
void SetAsegaAsegSpeed(ASEGA* pasega, float svt);
// orig: 0x00131A90 (the seeking records arrive by time g: mode 9)
void SetAsegaSeekTime(ASEGA* pasega, float g);
// orig: 0x00130EB0 UpdateAsegaIeaCur
void UpdateAsegaIeaCur(ASEGA* pasega);
// orig: 0x00131968
bool FAsegaRetracted(const ASEGA* pasega);
// orig: 0x00130F88 / 0x00131290 / 0x00131100
void FireAsegaEvents(ASEGA* pasega);
void FireAsegaEvent(ASEGA* pasega, ASEGEV* pev);
void FireAsegaStartEvents(ASEGA* pasega);
// orig: 0x001317D8 FWrapAsegaTime
bool FWrapAsegaTime(ASEGA* pasega, float* pt, float* psvt);
// orig: 0x00230CF0: the records the ASEGA drives (snap; fSeek: seeking ones too) put at its current values
void SnapAsegaXfs(ASEGA* pasega, bool fSeek);
// orig: 0x001300B8 CalculateAsegMatrices: paloFrom's transform relative to paloTo at time t of the animation
void CalculateAsegMatrices(ASEG* paseg, ALO* palo, XF* pxfFrom, XF* pxfTo, float t, float svt, glm::mat4* pmat,
                           glm::mat4* pdmat);
// orig: 0x0012F8F0 PostAsegLoad: an ASEG with apply parameters (EOPID 107 {t, svt, grf}) applies itself
void PostAsegLoad(ASEG* paseg);

// The playing ASEGAs (orig DL 0x3E1218), in application order.
extern std::vector<ASEGA*> g_apasega;
// Every frame (orig: in the world update 0x226550 after the objects): each ASEGA's update.
void UpdateAsegas();
// Frees retracted ASEGAs nobody holds (call once per frame, after the updates).
void CollectAsegas();
// Drops every ASEGA and controller (world unload).
void ResetAsegas();

// Hooks for the systems events reach (null: the event is counted in g_cAsegaEventDropped and dropped). Plain function
// pointers, not std::function: other files' static initializers set them (asegevent.cpp, cmworld.cpp, update.cpp), and a
// pointer is constant-initialized, so the order of the files' constructors does not matter. (A std::function here was
// constructed after those assignments on Windows, where MinGW runs the files' constructors in reverse link order, and
// emptied them: no ASEG events, no camera parameters, so the intro's cutscene camera never took over.)
extern void (*g_pfnAsegaEventOther)(ASEGA*, ASEGEV*);  // kinds 0-3, 7, 8, 13-20, 22-24
extern void (*g_pfnLoMessage)(LO*, int msgk, void* pv); // an object's vt slot 5 (messages 2, 3, 9)
// Message 9's argument (orig {the ASEGA, the event's time, its value an[0]}: the objects' slot 5 read +0 / +8)
struct ASEGMSG9
{
    ASEGA* pasega = nullptr;  // +0x0
    float t = 0.0f;           // +0x4
    int n = 0;                // +0x8
    const ASEGEV* pev = nullptr;
};
extern void (*g_pfnBasicScript)(BASIC*, int k);                    // vt slot 3: the object's script handler
extern float* (*g_pfnPgAsegParam)(LO*, int kind);                  // vt slot 43: a parameter's address
extern void (*g_pfnAsegParamUse)(LO*, int kind, bool fOn); // vt slots 44 / 45: the parameter taken / released
extern int g_cAsegaEventDropped;
