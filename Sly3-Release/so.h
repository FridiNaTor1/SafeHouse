#pragma once

// The physics of the ALO family (Sly 2 folded Sly 1's SO into ALO): the runtime of module 0x1F2108-0x1F8DD8 (the
// geometry entries, the mass state, the attachment and its boxes, velocities, constraints), the contacts (XP,
// mark.cpp), the boxes' sweep and prune (OXA / OX, bbmark.cpp) and the per-frame constraint pass (solve.cpp).
//
// Native layout: the ALO (lo.h) keeps its records (XF), its geometry entries (SOE, with their runtime fields) and
// psort, the rest of its physics runtime (SORT below). The world's physics lists (orig SW +0x3CCC..+0x3D40) are
// g_physw.

#include "bsp.h"
#include "lo.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <vector>

struct OXA;
struct OX;
struct XP;
struct NAVREGION;

// An object's entry in the navigation regions (orig REGENT, 0x70 bytes at SOPHYS +0xDC, made by 0x1F8300: the step
// objects' load 0x2039C0 and the options 80 / 81 / 82): the path followers (0x177F78) of the region's other members
// steer around it.
struct REGENT
{
    ALO* palo = nullptr;         // +0x00
    float dz = 0.0f;             // +0x04 (option 82, 0x1F8470)
    float sRadius = 75.0f;       // +0x08 (option 80, 0x1F83B0; 150 for a player 0x34)
    float gMass = 1.0f;          // +0x0C (option 81, 0x1F8410)
    NAVREGION* preg = nullptr;   // +0x10 the region it is in (0x1F37C8)
    int nGenNav = 0;             // (native: the regions' generation preg belongs to)
};

// The moving points of a geometry entry (orig 0x1F6C08: a 12-byte MESH: velocities, accelerations, triangles).
struct SOMESH
{
    std::vector<glm::vec3> av, aa;
    std::vector<std::array<int, 3>> atri;
};

// A constraint of the mass state (orig 0x20 bytes: the axis, the kind: 0 free, 1 in the plane normal to the axis,
// 2 along the axis, 3 fixed).
struct CNSTRS
{
    glm::vec3 normal{0.0f};  // +0x0
    int kind = 0;            // +0x10
};

// The mass state (orig ALO +0xE8, 0xE0 bytes; made by the loader when the ALO has geometry entries).
struct SOST
{
    glm::mat3 momint{1.0f};      // +0x00: the moment of inertia (times the mass scale)
    glm::mat3 momintInv{0.0f};   // +0x30
    glm::vec3 v{0.0f};           // +0x60: the position record's velocity (copied by 0x1F3980)
    glm::vec3 w{0.0f};           // +0x70: the rotation record's
    glm::vec3 dvGravity{0.0f};   // +0x80: the acceleration applied each frame (0x1F3590: (0, 0, -1750))
    CNSTRS cnstrAng;             // +0x90
    CNSTRS cnstrLin;             // +0xB0
    float m = 0.0f;              // +0xD0: the mass
    std::shared_ptr<REGENT> pregent;  // +0xDC: its region entry (0x1F8300)
};

// The rest of an ALO's physics runtime.
struct SORT
{
    std::unique_ptr<SOST> psost;   // +0xE8
    OXA* poxa = nullptr;           // +0xEC: the attachment (shared with the physics root's)
    glm::vec3 dpos{0.0f};          // +0xF0: the pending displacement (0x1FA528)
    glm::vec3 drot{0.0f};          // +0x100: the pending rotation
    std::array<uint8_t, 26> abXp{};  // +0x110 (0x1F78D8 / 0x1F7918)
    // (+0x12C / +0x130, the fade: ALO::gFade / dgFade, the world's)
    float g13C = 0.0f;             // +0x13C (0x2C3B90)
    int32_t nTreeStamp = -1;       // +0x140
    int32_t n138 = 0;              // +0x138
    int8_t s148 = -10, s149 = -20; // +0x148, +0x149
    uint64_t grf148 = 0;           // +0x148: bits 24-25 the mode, 26, 27 / 28 the solve's group marks, 34 a physics root,
                                   // 35 iced
};

// An overlap of two attachments (orig 0x14 bytes, one per side).
struct OX
{
    XP* pxp = nullptr;       // +0x00: the pair's contacts
    OX* poxNext = nullptr;   // +0x04
    OX* poxOther = nullptr;  // +0x08: the other side's record
    ALO* psoOther = nullptr; // +0x0C
    int fOverlap = 0;        // +0x10
};

// An attachment: the box of a physics root's geometry entries in the sweep lists (orig 0x78 bytes).
struct OXA
{
    glm::vec3 posMin{0.0f}, posMax{0.0f};  // +0x00, +0x10
    glm::vec3 posCenter{0.0f};             // +0x20
    float sRadius = 0.0f;                  // +0x30
    std::vector<SOE*> apsoe;               // +0x34: the geometry entries (DL, link +0x60; appended)
    ALO* pso = nullptr;                    // +0x60: the root
    OX* pox = nullptr;                     // +0x64: its overlaps
    XP* pxpInternal = nullptr;             // +0x68
    XP* pxpSensor = nullptr;               // +0x6C: the contacts of its sensors
    uint64_t grf = 0;                      // +0x70: 1 / 2 in the dirty lists, 4 / 8 / 0x10 marks, 0x20 no BSP pruning
};

// An end of a contact (orig 0x40 bytes).
struct XPD
{
    SOE* psoe = nullptr;      // +0x0: the geometry entry
    XF* pxf = nullptr;        // +0x4: its object
    ALO* paloRoot = nullptr;  // +0x8: the attachment's root (the material)
    ALO* palo = nullptr;      // +0xC: the entry's owner (notified)
    glm::vec3 pos{0.0f};      // +0x10
    glm::vec3 posLocal{0.0f}; // +0x20: in the object's frame
    glm::vec3 vel{0.0f};      // +0x30: the velocity of the point (0x22D348)
};

// A contact (orig 0xC0 bytes, from the world's pool at SW +0x3D40).
struct XP
{
    XPD axpd[2];              // +0x00, +0x40
    glm::vec3 normal{0.0f};   // +0x80
    float gDist = 0.0f;       // +0x90: the separation
    float mu = 0.0f;          // +0x94
    float elas = 0.0f;        // +0x98
    int xpk = 0;              // +0x9C: the kind (1 point-surface, 2 edge-surface, 3 edge-edge, 4..6 sphere, 8/9 swept,
                              // 13 inside, 0xD/0xE special)
    const void* pv0 = nullptr;  // +0xA0: the kind's tags (a position, an edge, a surface)
    const void* pv1 = nullptr;  // +0xA4
    float gA0 = 0.0f, gA4 = 0.0f;  // +0xA0 / +0xA4 as floats (kind 0xE: the limits of the solve's row)
    int ipxp = 0;             // +0xA8
    XP* pxpNext = nullptr;    // +0xAC
    glm::vec3 dv{0.0f};       // +0xB0: the ends' relative velocity (end 0 - end 1)
};

// A contact message (orig 0x60 bytes, built on the stack by 0x1F7410 / 0x1F7950; kept 0.25 s at SW +0x4130).
struct CTMSG
{
    CTMSG();
    LO* ploSender = nullptr;     // +0x0
    const void* pvOther = nullptr;  // +0x4: the sensor's entry (0x1F7410)
    LO* ploReceiver = nullptr;   // +0x8: where the offer starts (up its parents)
    uint32_t grfFilter = 0;      // +0xC: the contact bytes asked for
    int nState = 0;              // +0x10
    glm::vec3 pos{0.0f};         // +0x20
    glm::vec3 vec{0.0f};         // +0x30
    uint32_t grfAccepted = 0;    // +0x44: the receiver's matching bytes
    ALO* paloAccepted = nullptr; // +0x48
    float t = 0.0f;              // +0x4C: when it was accepted
};

// An entry of a sweep list (orig 0xC bytes).
struct AOX
{
    int fMax = 0;
    OXA* poxa = nullptr;
    const float* pg = nullptr;
};

// The world's physics lists (orig SW +0x3CCC..+0x3D40).
struct PHYSW
{
    bool fAaoxDirty = false;             // +0x3CCC
    std::vector<AOX> aaox[3];            // +0x3CD4 (count +0x3CD0)
    std::vector<OXA*> apoxa;             // +0x3D10: every attachment (link +0x48)
    std::vector<OXA*> apoxaActive;       // +0x3D1C: those of objects in the world's update list (link +0x40)
    std::vector<OXA*> apoxaDirty1;       // +0x3D28: boxes to recompute (link +0x50)
    std::vector<OXA*> apoxaDirty2;       // +0x3D34: moved (link +0x58)
    std::vector<std::unique_ptr<OXA>> aoxaOwned;
    std::vector<CTMSG> actmRecent;       // +0x4130: the contacts accepted in the last 0.25 s
    // the XP pool (orig +0x3D40)
    std::vector<std::unique_ptr<XP[]>> axpBlock;
    std::vector<XP*> axpFree;
    XP* PxpNew();
    void FreeXp(XP* pxp);
    void Reset();
};
extern PHYSW g_physw;

// Hooks the world provides (null: defaults).
extern bool (*g_pfnFAloActive)(ALO* palo);  // 0x1336F0: the object is in the world's update list (SW +0x2C)

SORT& Sort(ALO* palo);  // the ALO's physics runtime (made on first use)
inline SOST* Psost(ALO* palo) { return palo->psort ? palo->psort->psost.get() : nullptr; }
inline OXA* Poxa(ALO* palo) { return palo->psort ? palo->psort->poxa : nullptr; }
inline XF* PxfPos(ALO* palo) { return palo->apxfRef[0]; }  // orig +0x58: the position record
inline XF* PxfRot(ALO* palo) { return palo->apxfRef[1]; }  // orig +0x5C: the rotation record
inline XF* PxfObj(ALO* palo) { return palo->apxfRef[2]; }  // orig +0x60: the object's record (its world matrix)

// so.cpp: the ALO's physics runtime (0x1F2108-0x1F8DD8)
void InitAloPhys(ALO* palo);
void SetAloContactsOff(ALO* p, bool fOff);
void DetachAloContactsAll(ALO* p);               // 0x1F4E28 (the object's contacts detached: a script's placement)       // 0x1F6860 (a scripted leap's flight: no contacts)                     // the runtime part of 0x1F2108 (vt slot 7)
void LoadAloPhysEntries(ALO* palo);              // the loader 0x1F31E0: the entries' owner, 0x1F2B20
void LoadAloPhysState(ALO* palo);                // the loader 0x1F31E0: the mass state, 0x1F35F8, slot 64
void CloneAloPhys(const ALO* src, ALO* dst);     // part of 0x1F6068 (vt slot 41), after the world's CloneAloData
void OnAloEnterWorldPhys(ALO* palo);             // part of 0x1F3EA8 (vt slot 23)
void OnAloLeaveWorldPhys(ALO* palo);             // part of 0x1F3E00 (vt slot 22)
void PostLoadAloPhys(ALO* palo);                 // the physics part of 0x1F2C30 (0x22FAA0, 0x1F2B20, flags 0x14)
ALO* PaloPhysRoot(ALO* palo);                    // 0x1F2798
REGENT* PregentEnsure(ALO* palo);                // 0x1F8300
inline REGENT* Pregent(ALO* palo) { SOST* p = Psost(palo); return p ? p->pregent.get() : nullptr; }
void UpdateAloRegion(ALO* palo);                 // 0x1F3868: 0x1F37C8 with the region at its position (0x1C0D70)
void SetAloRegion(ALO* palo, NAVREGION* preg);   // 0x1F37C8
OXA* PoxaEnsure(ALO* palo);                      // 0x1F27F0
void AddSoeToOxa(ALO* palo, SOE* psoe);          // 0x1F2870
void RemoveSoeFromOxa(ALO* palo, SOE* psoe);     // 0x1F2930
void AddAloSoes(ALO* palo);                      // 0x1F2B20
void RemoveAloSoes(ALO* palo);                   // 0x1F2BA8
void SetSoeOff(ALO* palo, SOE* psoe);            // 0x1F2AD0
void SetSoeOn(ALO* palo, SOE* psoe);             // 0x1F2A60
void ScaleAloMomint(ALO* palo, float s);         // 0x1F35F8
void SetAloMass(ALO* palo, float m);             // 0x1F35C8
void RestAloSost(ALO* palo);                     // 0x1F3590 (vt slot 64)
void CopyAloSoeMatrices(ALO* palo);              // 0x1F3900
void UpdateAloPhys(ALO* palo);                   // 0x1F3980 (vt slot 17)
void UpdateSoeWorld(SOE* psoe);                  // 0x1F38A8
void MarkOxaDirty1(ALO* palo);                   // 0x1F4B58
void UnmarkOxaDirty1(ALO* palo);                 // 0x1F4BA8
void MarkAloDirty(ALO* palo);                    // 0x1F4BF8
void UnmarkAloDirty(ALO* palo);                  // 0x1F4D50
void UpdateAloBounds(ALO* palo);                 // 0x1F50E0 (vt slot 49)
void WakeAlo(ALO* palo);                         // 0x1F5078
// The water (water.cpp, class 0x59): the waters the object is in applied (0x229798, from the gravity slot 70); an
// object leaving the world dropped from the waters' lists; the lists emptied for a new world.
void ApplyAloWater(ALO* palo);
void RemoveAloWater(ALO* palo);
void ResetWater();
// orig 0x228C18's velocity: a world point carried at the water's level (z 0 in its record pxf) with the flow vFlow
glm::vec3 VelWaterPoint(struct XF* pxf, const glm::vec3& vFlow, const glm::vec3& pos);
bool FAloStill(ALO* palo);                       // 0x1F5040 (vt slot 62)
void IntegrateSoeMesh(SOE* psoe, float dt);      // 0x1F6EF8
void SetCnstrAxis(XF* pxfOwner, int k, CNSTRS* pc);  // 0x1F5510
void SetAloCnstrLin(ALO* palo, int k);           // 0x1F5720
void SetAloCnstrAng(ALO* palo, int k);           // 0x1F57C0
void SetAloGravity(ALO* palo, const glm::vec3& dv);  // 0x1F5880
void ReRootAlo(ALO* palo);                       // 0x1F55C8
void SetAloPhysRootFlag(ALO* palo, bool f);      // 0x1F7F48
void SetAloIced(ALO* palo, bool f);              // 0x1F8668
glm::vec3 PosAloCenter(ALO* palo);               // 0x1A62E8 (the object record's world position)
glm::vec3 VelAloPos(ALO* palo);                  // 0x1F4440 (vt slot 33)
void SetAloVelocityTo(ALO* palo, const glm::vec3& pos);   // 0x1F4488 (vt slot 35)
void SetAloVelocityToLoad(ALO* palo, const glm::vec3& pos, bool fLoad);   // (fLoad below)
glm::vec3 AngVelAlo(ALO* palo);                  // 0x1F4558 (vt slot 34)
void SetAloAngVelocityTo(ALO* palo, const glm::vec3& pos);  // 0x1F46D8 (vt slot 36)
void SetAloAngVelocityToLoad(ALO* palo, const glm::vec3& pos, bool fLoad);  // (fLoad below)
// fLoad: the world loader's options (EOPIDs 29 / 30 / 75 / 76, anim.cpp): converted through the matrices as the original
// had them while the object loaded (each record's loaded local, records loaded later left out, a copy through its template:
// so.cpp VecConvertXfsLoad). R16: the Spice Room's cogs 91231 (option 30) turn about their local x; R55: world 32's
// watermills (option 76 about local z) about their axles; tools/veloptregress.py checks every world against the RAM
void SetAloVelocity(ALO* palo, const glm::vec3& v, bool fLoad = false);      // 0x1F4898
void SetAloAngVelocity(ALO* palo, const glm::vec3& w, bool fLoad = false);   // 0x1F4930
void SetAloPos(ALO* palo, const glm::vec3& pos);         // 0x1F40A0 (vt slot 29)
void SetAloRotation(ALO* palo, const glm::mat3& mat);    // 0x1F42E0 (vt slot 30)

void BrakeAloRotation(ALO* palo, const glm::mat3& mat, const float* ac);  // 0x1F59A0 ({max speed, end speed, time})
void AccelAloTowardRotation(ALO* palo, const glm::mat3* pmat, const struct CLQ* pclq, const glm::vec3& w,
                            const struct CLQ* pclqV);  // 0x1F5C30
void AccelAloTowardPosition(ALO* palo, const glm::vec3& pos, const struct CLQ* pclq, const glm::vec3& v,
                            const struct CLQ* pclqV);  // 0x1F5E10
int CsoeCollect(ALO* palo, bool (*pfn)(void*, SOE*), void* pv, std::vector<SOE*>* papsoe);  // 0x1F7E08
void SetAloXpBytes(ALO* palo, uint32_t grf, uint8_t b);  // 0x1F7918
uint32_t GrfAloXpBytes(ALO* palo, uint32_t grf);         // 0x1F78D8
void SetAloEntryMode(ALO* palo, int k);                  // 0x1F7A68
void ResetAloPhys(ALO* palo);                            // part of 0x1F2D48 (vt slot 12)
void MoveAloPhys(ALO* palo, ALO* paloNew, ALO* paloOld);  // part of 0x1F2E78 (vt slot 13)

void TransformGeom(const glm::mat4& m, const GEOM& src, GEOM* pdst);  // 0x1664A0
void CopyTransformGeom(const GEOM& src, const glm::mat4* pmat, GEOM* pdst);  // 0x166270
void EnsureSoeMesh(SOE* psoe);                                                // 0x1F6C08
void RebuildSoe(const SOE* src, XF* pxfSpace, int cposInsert, SOE* dst);       // 0x1F70C0

// The pose record of 0x1F58F0 (vt slot 50).
struct SOPOSE
{
    int n0 = 0;               // +0x0
    glm::vec3 pos{0.0f};      // +0x10
    glm::vec3 vel{0.0f};      // +0x20
    float radHeading = 0.0f;  // +0x30
    float g34 = 100.0f;       // +0x34
    float t = 0.0f;           // +0x38
    int n3C = 0;              // +0x3C
};
void GetAloPose(ALO* palo, SOPOSE* ppose);       // 0x1F58F0 (vt slot 50)
void ReparentAlo(ALO* palo, ALO* paloNew, XF* pxfNew, int grf, void (*pfnBaseMove)(ALO*, ALO*, XF*, int));  // 0x1F21B8

// Contact messages (so.cpp): the world's / splice's message (vt slot 4) and event (vt slot 3) senders.
extern void (*g_pfnLoNotify)(LO* plo, int msgk, void* pv);
extern void (*g_pfnLoEvent)(LO* plo, int n, int a, int b, void* pv, LO* plo2);
ALO* PaloAcceptContact(CTMSG* pctm);             // 0x1F7598
void AgeSwContacts();                            // 0x1F77C0 (the world update, each frame before the objects)
void DispatchAloSensorContacts(ALO* palo);       // 0x1F7410 (vt slot 74)
void EmitAloContact(ALO* palo, LO* ploPlayer);   // 0x1F7950
bool FAloContainsPoint(ALO* palo, const glm::vec3& pos);  // 0x1F72F8 (vt slot 73)

// bbmark.cpp
OXA* PoxaAddSw(ALO* pso);                        // 0x1336F0
void RemoveSwOxa(OXA* poxa);                     // 0x1337D0
void UpdateSwPox(OXA* poxa, OXA* poxaOther, int k);  // 0x133A38
bool FOverlapOxa(OXA* poxa, OXA* poxaOther);     // 0x133D08
void FlushSwOxaDirty1();                         // 0x133E30
void UpdateSwAaox();                             // 0x133E80
void UpdateSwXps();
void NotifyAloContacts(ALO* palo);                // 0x1F4F10 (ALO vt slot 58): the sensor contacts posted
void RebuildSwAaox();
extern void (*g_pfnPostAloContacts)(ALO* palo);  // 0x1F4F10's post (0x149AB8): the world's deferred 0x1F7950   // bbmark.cpp: the sweep lists and pairs rebuilt from the current boxes (the world's first frame)                              // 0x134040
void RecalcSwOxfFilterForObject(ALO* pso);       // 0x134320

// solve.cpp
void UpdateSwPhys();                             // 0x1F8DD8: the frame's physics
void SolvePassSw(int k);                         // 0x1F8F80
void SolveGroup(int flags, int nobj, ALO** objs, int ncont, XP** conts);  // 0x1F9410
extern void (*g_pfnUpdateSwEntries17)();         // the world's entries loop for slot 17 (orig in 0x1F8DD8)
void SetPhysSwObjects(const std::vector<LO*>& aplo);  // without it: these objects, in order
void ApplyAloGravity(ALO* palo);                 // 0x1F9B20 (vt slot 70)
void AccelAloPos(ALO* palo, const glm::vec3& v);  // 0x1FAC30

// mark.cpp
// orig0x1AA0B0, slot60: preserve the other entry's BSP range/root for custom
// producers (vehicle wheel support and projectile retained sweeps).
using PFNOWNCONTACT = void (*)(ALO*, SOE*, int inodeLimit, const BSPREF&, int root, XP**);
void FreeSwXpList(XP* pxp);                      // 0x1A7E10
XP* PxpAddSw(SOE* a, SOE* b, const glm::vec3& pos, const glm::vec3& dir, int xpk, const void* pv0, const void* pv1,
             XP** ppxp, float g);                // 0x1A7C18
void UpdateXpWorldPositions(int c, XP** apxp);   // 0x1A7FC0
void UpdateXpPositions(int c, XP** apxp);        // 0x1A7E60
XP* PxpMarkOxaContacts(OXA* poxaA, OXA* poxaB);  // 0x1AA1D8

// Queries (mark.cpp; orig 0x14C6A0..0x14CC58): the pieces of a segment inside an entry, the nearest entry a line hits,
// the union / intersection of two piece lists.
int ClipSegmentEntry(const SOE* psoe, const glm::vec3& a, const glm::vec3& b, int c, LSG* alsg);
// (the camera's queries: the tree's first inodeLimit nodes only, orig 0x118668's limit)
int ClipSegmentEntryLimit(const SOE* psoe, const glm::vec3& a, const glm::vec3& b, int c, LSG* alsg, int inodeLimit);
// orig: 0x0014C748: the shared sphere clip used by authored foot reach probes.
int ClipSegmentToSphere(const glm::vec3& center, const glm::vec3& a, const glm::vec3& b,
                        int countMax, LSG* result, float radius);
SOE* PsoeHitTestLineObjects(int grf, const glm::vec3& a, const glm::vec3& b, const std::vector<SOE*>& apsoe, LSG* plsg);
int CMergeAlsg(int c1, const LSG* alsg1, int c2, const LSG* alsg2, int cMax, LSG* alsgOut, int k);

int CsbiSphereEntry(const glm::vec3& pos, const SOE* pso, const std::vector<BSPN>* pabsp, int csbiMax, float r,
                    std::vector<glm::vec3>* papos, std::vector<glm::vec4>* paplane, std::vector<int>* pakind,
                    std::vector<const void*>* papv, int inodeLimit = -1);  // 0x1A93C8: a sphere's contacts with an entry
                    // (inodeLimit: the tree's first nodes only (the camera's hull, the entry's +0x70); -1 all)

// Entry contacts (mark.cpp): 0x1A8B50 (iroot1: the root of the pruned nodes of pso1's tree in absp,
// IBSP_SOLID or IBSP_NONE), 0x1A9B28. Also used by physcheck.cpp.
void MarkBspBspContacts(SOE* pso0, SOE* pso1, const std::vector<BSPN>& absp, int iroot1, XP** ppxp);
void MarkSphereBspContacts(SOE* a, SOE* b, const std::vector<BSPN>* pabsp, XP** ppxp);
// Unit-check entry point: 0x1A9D38.
void MarkSweptVerticesCheck(SOE* a, SOE* b, XP** ppxp);
