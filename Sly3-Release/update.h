#pragma once

#include <vector>

// The world's per-frame update (orig 0x226550 UpdateSw) and its object table (SW +0x3F20: 8-byte entries
// {flags, object}, 0x210140 / 0x210268), the world start after a load (0x20E740), the splice events of objects
// (vt slot 3, 0x1A61E0 / 0x1331F8) and the hooks other parts register:
//  - class updates: the function in vt slot 16 of an object's original class, bound by its original address
//    (RegisterLoUpdate); the defaults are LO's 0x1A5E28 (the splice "update" event) and ALO's 0x1F36E0;
//  - passes: the other calls of 0x226550 by original address (RegisterSwPass), in the original's order.

#include "lo.h"

#include <cstdint>
#include <functional>

struct SW;
class CRef;

using PFNUPDATE = void (*)(LO* plo);
using PFNSWPASS = std::function<void(SW* psw)>;

// The update entry flags (the object's +0x40 while not in the table): 1 not in the table (orig: the low bit
// marks flags, else a pointer), 2 has an update (vt slot 16 is called), 0x40 never in the table.
constexpr uint32_t GRFUPD_Flags = 0x1, GRFUPD_Update = 0x2, GRFUPD_Never = 0x40;
// 0x100: updated on the real clock too (the paused update 0x2267C0 calls the entries with 0x102)
constexpr uint32_t GRFUPD_RealTime = 0x100;

// A message handler for classes without a native vt slot 5 (true: handled), e.g. the actors' 0x140F90.
// Registration order across files does not matter (static initialisation order is unspecified).
// (the handlers and passes are kept in the order of their source files' names, which is the link order on Linux,
// whatever order the files' static constructors run in: MinGW runs them backwards. pszFile: the caller's file.)
void RegisterLoMessageHandler(std::function<bool(LO*, int, void*)> pfn, const char* pszFile = __builtin_FILE());
// The objects' listeners (orig LO +0x8): 0x1333C0 AddLoListener (counted by id), 0x133448 RemoveLoListener, 0x133508
// NotifyLo (the object's own message, then each listener's: so.cpp's g_pfnLoNotify, e.g. message 0xB {the hit object,
// the sender, the contact message} when a hit lands: the guard's JOBACT)
void AddLoListener(LO* plo, const void* id, std::function<void(int msgk, void* pv)> pfn);
void RemoveLoListener(LO* plo, const void* id);
// The listeners only (not the object's own slot 5): used for 0x1A5848's message 1 (out of the world), whose own-handler
// part natively has no receiver yet.
bool FLoHasListeners(const LO* plo);
void NotifyLoListeners(LO* plo, int msgk, void* pv);
void NotifyLo(LO* plo, int msgk, void* pv);
void ResetLoListeners();  // a new world
void RegisterLoUpdate(uint32_t pfnOrig, PFNUPDATE pfn);
PFNUPDATE PfnUpdateForClass(const CLS* pcls);  // vt slot 16 bound (null: no native update)
bool FHasSwPass(uint32_t pfnOrig);              // (vtaudit.cpp) a pass is registered for the original function
PFNUPDATE PfnUpdateOrig(uint32_t pfnOrig);      // the update bound to an original function (a base class's)
void RegisterSwPass(uint32_t pfnOrig, PFNSWPASS pfn, const char* pszFile = __builtin_FILE());

// orig 0x149AB8: the world's shared posted-call FIFO. Suppresses only
// an identical object/member/argument still on the pending list, including across callback families.
using PFNDEFERREDCALL = void (*)(void* object, int arg);
void PostDeferredCall(void* object, uint32_t pfnOrig, PFNDEFERREDCALL pfn, int arg = 0);

void AddLoToUpdateTable(SW* psw, LO* plo);       // orig 0x210140 (vt slot 23 0x1A6178)
void RemoveLoFromUpdateTable(SW* psw, LO* plo);  // orig 0x210268 (vt slot 22 0x1A6150)

// orig 0x1A5C48 (LO vt slot 15) and the world start 0x20E740: the objects of the load in order.
// Gameplay's batched physics/player postloads run after animation setup, before
// deferred start scripts and queued-task activation. Offline model probes omit them.
extern LO* g_ploFadeRecover;   // g_game +0x3E60 (0x20E338)
void StartFadeLo(LO* plo);     // orig 0x15E1D8 (hudwidget.cpp)
void StartSw(SW* psw, bool fInitHud = false, PFNSWPASS pfnPostLoadPlayers = nullptr);

// orig 0x1A61E0 / 0x1331F8 (vt slot 3): splice event symid of plo with its arguments; deferred: queued with the
// time and run at the next 0x149C80.
// orig 0x00123BD8 / 0x00133130: a callback applied at t + dt (its id), cancelled; the world's events forgotten
int IdScheduleCallback(float dt, const CRef& refProc, std::vector<CRef> aref);
void CancelScheduledCallback(int id);
void ClearDeferredSpliceEvents();
// prefResult (0x1331F8's last argument): the handler's value is assigned to it when the frame binds the event
void HandleLoSpliceEvent(LO* plo, uint32_t symid, bool fDeferred, int carg, CRef* aref, CRef* prefResult = nullptr);
// 0x1331F8 with one int argument (type 6) and a CRef(true) result: false when a bound handler gives anything but true
bool FLoSpliceEventTrue(LO* plo, uint32_t symid, int n);
void RunDeferredSpliceEvents(SW* psw);  // orig 0x149C80 (the deferred events whose time has come)

// orig 0x226550 UpdateSw
void UpdateSw(SW* psw);
