#pragma once

// The objects' activation: which objects take part in the world (are in the update table SW +0x3F20) and when.
//
// The original: each object is added (slot 8 AddLo 0x1A5478) or removed (slot 9 RemoveLo 0x1A5630); an added
// object under an added chain is "in the world" (+0x28 bit 60, OnAdd slot 11 / OnRemove slot 12). A root object
// owns a node (+0x14, its children share it); the node is active (in SW +0x2C) while the root's activation kind
// (+0x28 bits 52..55) says so: 1 always, 2 never, 0 within its radius (+0x38) of the camera (CM 0x2DDF40 +0x90)
// when its mask (+0x2D) meets the camera's flags (CM +0x14), 3 the same with the player's position. The roots
// whose kind or radius changed are queued (SW +0x20, 0x147E88) and checked by the zone pass at the start of each
// world update (0x147F40) or at once by AddLo (0x148300). An active root registers itself and its added
// descendants (slot 23: 0x1A6178, ALO 0x1F3EA8) and unregisters them when it leaves (slot 22).
//
// The load: every object starts in the world (PloNew's OnAdd); each group's objects are removed after the group
// (0x13DA18); a proxy adds its instance's objects (0x1CA370); the world start (0x1A5C48) runs the deferred
// removals (option 26), the start scripts (which add and remove objects through the "add"/"remove" methods,
// EOPIDs 25/26) and sets kind 0 on the objects with an update script.

#include <cstdint>
#include <glm/glm.hpp>

struct ALO;
struct LO;
struct SW;

// The activation's viewpoint (the camera code sets it each frame; the original's CM +0x90 and +0x14). Until a
// camera runs, game.cpp sets it from the camera it shows; during a load its flags are 0 (nothing activates
// by distance until the first frame).
struct ACTVIEW
{
    glm::vec3 posCm{0.0f};
    uint32_t grfCm = 0;
};
extern ACTVIEW g_actview;

LO* PloRoot(LO* plo);
bool FLoActive(const LO* plo);                 // orig 0x001A61A0: in the world, its root's node active
void InitLoActivation(SW* psw, LO* plo);       // orig 0x1A5270's fields, 0x13E3C8's slots 11 and 23
void AddLo(SW* psw, LO* plo);                  // orig 0x001A5478 (vt slot 8)
void RemoveLo(SW* psw, LO* plo);               // orig 0x001A5630 (vt slot 9)
void SetLoParentActivation(SW* psw, LO* plo, ALO* paloNew, bool fMovePhys = false);  // orig 0x001A5900
// (perf-2) counts every change of an object's parent after its creation (LO::paloParent's only writer besides PloNew;
// a new writer must count too): game.cpp keeps the world's root objects while it and aploAll's size stay
extern uint32_t g_cLoParentChanges;
void SetLoSt(SW* psw, LO* plo, int st);        // orig 0x001A6CA8 (option 14)
void SetLoRadius(SW* psw, LO* plo, float s);   // orig 0x001A6C48 / ALO 0x001F3118 (option 13, vt slot 42)
void SetLoMask(SW* psw, LO* plo, uint32_t b);  // orig 0x001A6D28
void RegisterLoTree(SW* psw, LO* plo);         // vt slot 23 (0x1A6178, ALO 0x1F3EA8)
void UnregisterLoTree(SW* psw, LO* plo);       // vt slot 22 (0x1A6150, ALO 0x1F3E00)
void QueueLoRoot(SW* psw, LO* ploRoot);        // orig 0x00147E88
void CheckLoRootNow(SW* psw, LO* plo);  // orig: 0x00148300
void UpdateSwActivation(SW* psw);              // orig 0x00147F40 (the zone pass)
glm::vec3 PosLoWorld(const LO* plo);           // orig 0x001A62E8 (vt slot 31's +0x30)
uint32_t GrfUpdLo(const SW* psw, const LO* plo);  // the +0x40 flags (the update table entry's while registered)
void SetLoVisible(SW* psw, LO* plo, bool f);   // orig 0x001A6E80 (vt slot 21: +0x40 bit 8; TODO the ALO's 0x1F3B28
                                               // volume tree part)
// The frame's draw rule (RenderSw 0x1CDA80): an ALO's globs are drawn only when it is in the render list (the
// update table) with the drawable flag 8 (not static 0x80: through vt slot 19; static: through the level's
// volume tree 0x1D5808, which draws the registered static objects), its class draws (slot 19 is not LO's empty
// 0x1A5ED8), its mask meets the camera's flags (CM +0x14) and its fade (+0x12C) is not 0.
bool FAloDrawn(const SW* psw, const ALO* palo);
// (QoL draw distance, display.h) 1: drawn regardless of the update table (above 1x: an inactive tree within its
// activation radius x the option), -1: not drawn (below 1x), 0: the original's test. 1x: always 0.
int NDrawDistanceRoot(const ALO* palo);
// (drawdist-2) the tree under the root has a character (derived from PO 0x33): not drawn beyond the original's radius
bool FDrawDistanceActorTree(const LO* proot);
// (drawdist-2) plo's tree is an inactive one the option above 1x draws (emitters run privately, spinning parts posed)
bool FDrawDistanceVisualTree(const LO* plo);
// Native diagnostic observers at the world's deferred-call boundary.
void PrintActivationDebug(SW* psw);
// Low-level 0x001CAEC8 helper using the row score directly: 2 permits inactive,
// 1 also active rows. Returns the object only for single-object rows after attach.
// Use PloAcquireProxyObject for the original 0x001CB250 precheck and boolean API.
LO* PloAttachProxy(SW* psw, LO* pproxy, int nMin);
// orig 0x001CB250: require one object before attaching; false uses minimum score 2,
// true also permits score 1 (an active row). A retained own row still needs score 3.
LO* PloAcquireProxyObject(SW* psw, LO* pproxy, bool allowActive = false);
// orig 0x001CADE0: the row the proxy would use (its first object; null: none free)
LO* PloProxyFreeRow(SW* psw, LO* pproxy, int nMin);
// orig 0x001CB488 / 0x001CB4B8: the row of the proxy's instance plo kept / given back
void LockProxyRowOf(SW* psw, LO* pproxy, LO* plo, bool fLock = true);
void UnlockProxyRowOf(SW* psw, LO* pproxy, LO* plo);
void ReleaseProxyObject(SW* psw, LO* pproxy, LO* plo);   // (Sly 3 0x1FC0E0: the instance's row detached)
// orig 0x001DFD58: a SCPT object (cid 0x72) started (from an ASEG event of kind 4, asegevent.cpp); the SCPT side
// (the actors code) sets it
extern void (*g_pfnStartScpt)(LO* plo, int n);
// orig 0x00210338's part after the find: plo added / removed (an ALO faded over g seconds, 0x1F7C68 / 0x1F7BF8)
void SwitchLoFaded(LO* plo, bool fOn, float g);
void DetachAloBase27(LO* plo, LO* pproxy);  // orig 0x1CB5D0 (the ALO slot 27: the children told)
