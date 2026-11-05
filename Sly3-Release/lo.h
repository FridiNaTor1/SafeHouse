#pragma once

// The world's objects (Sly 1's LO/ALO family, Sly 2 layout). Native structs; comments give the original offsets
// for cross-checking with the reference port's RAM. Fields not used by the loader yet are left to the
// milestones that port the classes' behaviour.

#include "basic.h"
#include "cls.h"
#include "geom.h"
#include "glob.h"

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct ALO;
struct SW;
struct XF;
struct GLOBSET;

// A value read for an option (LoadOptionFromBrx 0x13E818) or a CRef read by RefReadOption (0x13E690).
struct OPTVAL
{
    int k = -1;         // the option kind (EOPID +0: OTYP) or CRef kind
    int tagk = -1;      // the CRef tag the original builds (0x15 int, 0x16 float, 0x17 bool, 0x24 nil, ...)
    int32_t n = 0;      // integer / bool / oid / U32 values
    std::array<float, 4> ag{};  // float, vector, 2D vector, euler angles
    std::vector<int16_t> an;    // kind 22: an array of S16
    std::vector<OPTVAL> alist;  // a list (U16 kind with bit 0x8000: U8 count of values)
    BASIC* pbasic = nullptr;  // tagk 0x21: an object a script stored (seto! of an object field, kept as is)
};

// An option as loaded: its EOPID index and the value(s) read.
struct OPT
{
    int eopid = -1;
    int grf = 0;
    std::vector<OPTVAL> aval;  // one value, or the c values of a 0x401 (CRef array) option
};

// Local object (orig size 0x50, cid 2).
class CFrame;
class CSidebag;

struct LO : BASIC
{
    ~LO() override = default;

    CLS* pcls = nullptr;      // orig +0xC: the vtable
    ALO* paloParent = nullptr; // orig +0x10 (PloNew's psw argument: the object loading it)
    int iframe = -1;          // the S16 'n' of the object record (the splice module's index in the group's table)
    std::shared_ptr<CFrame> pframe;  // orig +0x0: that module's frame (0x1FFA40 / 0x256EE0)
    std::shared_ptr<CSidebag> psidebag;  // orig +0x4: the scripts' own bindings (addo / ensureo / ids above 0x4D0)
    OID oid = -1;             // orig +0x18
    uint64_t grf = 0;         // orig +0x28 flag bits (48..51: the sink's value, 0x13F990)
    uint32_t grfLoad = 0;     // the U32 given to 0x1A6D28 at the end of the load (orig +0x2D bits)
    int igrp = -1;            // the world group (WE) whose load created the object
    int ichannel = -1;        // orig +0x3C's first entry >> 16 (0x1C9950): the group loading it, shared ones too (-1: none)
    int iclone = 0;           // 0: loaded; k > 0: the k-th instance copy of a group (0x13E510)
    LO* ploCloneOf = nullptr;
    OID oidInstance = -1;     // the first object of a shared group's instance: the instance's (PROXY record's) oid
    LO* ploNs = nullptr;      // orig +0x3C's namespace node (0x1C99D8) when not the world's (D_003E139C): the instance
                              // that placed it (the first object of a placed proxy's instance), children inherit it (InitLo 0x1A5270)
    bool fSpareNs = false;    // orig +0x3C node's +0x24 (0x1CA500): an object of a group's spare instance (proxies attach it
                              // later), e.g. the guards' start (0x2061D8) is not queued for it
    uint32_t grfUpd = 0x21;   // orig +0x40 while not in the update table (InitLo 0x1A5270: 0x21)
    int iupd = -1;            // its entry in SW::aupd (orig: +0x40 points at it)
    float tNextUpdate = 0.0f; // orig +0x30: the splice update event waits for this time
    // Activation (orig +0x28 bits, +0x2D, +0x38; activate.cpp): an object takes part in the world (its update in
    // the update table) while it is added, in the world and its root's node is active.
    bool fRemoveAtStart = false;  // option 26 (pfn 0x100B10 -> 0x1A5D30): RemoveLo queued for the start (+0x34)
    bool fRemoved = false;    // bit 56: removed (RemoveLo, vt slot 9 0x1A5630; AddLo, slot 8 0x1A5478, clears it)
    bool fInWorld = true;     // bit 60: added under an added chain (OnAdd slot 11 / OnRemove slot 12)
    int st = 2;               // bits 52..55: the activation kind (0: by the camera's distance, 1: always, 2: never,
                              // 3: by the player's distance; InitLo 2, option 14 0x1A6CA8)
    uint8_t bMask = 0xFF;     // +0x2D: AND the camera's +0x14 (0x1A6D28)
    bool fMaskOwn = false;    // bit 58 (option 32, 0x1A6E38): its parent's mask does not go down to it
    float sRadius = 1e10f;    // +0x38: the activation radius (option 13)
    bool fNodeActive = false; // a root's node (+0x14) in SW +0x2C (the active roots)
    bool fQueued = false;     // a root's node in SW +0x20 (the roots the zone pass 0x147F40 checks)
    std::list<LO*>::iterator itQueued;  // its place there (SW::lsplo20)
    int nState = 0;           // classes with a state (e.g. 0x1A: orig +0x13C, set by 0x228358)
    float tState = 0.0f;      // orig +0x140
    bool fMission = false;    // orig +0x50 = 1: a mission/task (cid 0x84/0x85) made though filtered out
    std::vector<LO*> aploOwned; // the objects created while it loaded (children, states, ...): its namespace
    std::vector<OPT> aopt;    // the options loaded for it (orig: stored by the EOPID's field or setter)
    // Fields set by 'field' options (EOPID grf 0x90) at the original offset, so loaders that test such a field
    // (the emitter's kind) can find it: (getter pfn (0: the object itself), offset) -> raw 32-bit value.
    std::vector<std::pair<uint64_t, uint32_t>> afield;
    uint32_t U32Field(uint32_t pfnGetter, uint32_t ib, uint32_t uDefault = 0) const;
    void SetField(uint32_t pfnGetter, uint32_t ib, uint32_t u);

    CID Cid() const { return pcls ? pcls->cid : -1; }
};

// cid 3 family (orig size 0xE0): LO + a transform reference (0x231768): the "located" objects.
struct LOX : LO
{
    XF* pxf = nullptr;            // orig +0x50: the record 0x22D6B0 gives for the S16
    int ixfRef = -1;              // the S16 itself
    glm::mat4 mat{1.0f};          // orig +0x60
};

// A transform record of an ALO (0x22D7A0: orig 0x100 bytes at ALO +0x54).
struct XFJOB
{
    int kind = 0;  // 1, 2, 4, 9
    int id = 0;
    float g = 0.0f, gLo = 0.0f, gHi = 0.0f;
    float agBegin[3]{}, agEnd[3]{};
    float agTimingBegin[2]{}, agTimingEnd[2]{};
    int a = 0, b = 0, c = 0;
    glm::vec3 vec{0.0f};
};

struct XF
{
    glm::mat4 mat{1.0f};      // orig +0: local (glm columns = the original's rows)
    glm::mat4 matWorld{1.0f}; // orig +0x40: world = the reference's world x local (UpdateXfWorlds)
    ALO* palo = nullptr;      // the ALO owning the record
    uint64_t genWorld = 0;    // (native) UpdateXfWorlds' pass stamp: 2g started, 2g + 1 done in pass g
    XF* pxfRef = nullptr;     // orig +0xA0 (the S16 reference)
    int kind = 0;             // orig +0x94
    int32_t tag = 0;          // orig +0x9C..
    int lo = 1000, hi = 1000; // orig +0xAC/+0xAE: weights x1000 (the runtime's gains, read as U16)
    XF* pxfOrigin = nullptr;  // kinds 8/14 (orig +0xA8)
    std::vector<XFJOB> ajob;

    // The record's motion (orig +0x80..+0xFF; runtime in xf.cpp, the animation code): the segment the head
    // controller gave it, its mode and state, its velocity. Pointers to an ASEGA's time are not copied by clones
    // (CloneAloData resets them through ResetXfParams).
    float g0 = 0.0f;                  // orig +0x80: the segment's parameter range [g0, g1]
    float g1 = 1.0f;                  // orig +0x84
    const float* pg = nullptr;        // orig +0x88: the parameter (null: &g0)
    const float* pdgScale = nullptr;  // orig +0x8C: its rate (null: &g1)
    struct CTRL* pctrlFirst = nullptr; // orig +0x90: the controllers, highest priority first (the head drives)
    int16_t mode = 0;                 // orig +0x98 (XFM_*)
    int16_t state = 0;                // orig +0x9A: the target's source (0 current, 1 spline, 2 fixed, 3/4 fixed +
                                      // velocity, 5 rows)
    int32_t nStamp = 0;               // orig +0xA4: the change counter's value when it last moved
    float tTarget = 0.0f;             // orig +0xA8 (float: modes 9 / 17's arrival time)
    void* pvA8 = nullptr;             // orig +0xA8 (pointer: mode 4's record, modes 13/15/16's joint/link/chain)
    glm::vec4 vel{0.0f};              // orig +0xB0
    glm::vec4 aseg[4]{};              // orig +0xC0 value 0, +0xD0 slope 0, +0xE0 value 1, +0xF0 slope 1
    float G() const { return pg ? *pg : g0; }
    float DgScale() const { return pdgScale ? *pdgScale : g1; }
};

// Collision entry of an ALO (0x1F31E0: orig 0xF0 bytes each).
struct SOE
{
    XF* pxf = nullptr;        // the record of the S16 (null: the identity)
    int32_t tag = 0;
    bool f48 = false, f49 = false, f50 = false;
    float sRadius = 0.0f;
    GEOM geomLocal;
    GEOM geomWorld;
    BSP bsp;
    uint32_t id = 0;
    int16_t state = 0;
    int16_t pinned = 0;

    // Runtime (the physics port, so.h; the original's 0xB0-byte entry at ALO +0xE4).
    ALO* palo = nullptr;          // orig +0x4: the owner
    bool f51 = false;             // orig +0x10 bit 51
    std::shared_ptr<struct SOMESH> pmesh;  // orig +0x10 (low word): the moving-points mesh (0x1F6C08)
    glm::mat4 mat{1.0f};          // orig +0x20: the object's world matrix when the world geometry was made
    int32_t nStampWorld = 0;      // orig +0xA8: the object's stamp the world geometry is of (-1: stale)
    int32_t nStampObj = 0;        // orig +0xAC: the object's stamp at the last matrix copy (0x1F3900)
    bool fInOxa = false;          // in its attachment's list (orig: the DL at OXA +0x34, link +0x60)
};

// Records loaded by 0x186670 / 0x1603B8 / 0x21C888 for an ALO.
struct TRKG;
struct ALOPAIR  // 0x186670 (orig 0xE0 bytes each): a two-bone joint (IK, ik.cpp)
{
    XF* apxf[4]{};                    // +0 the target, +4 the upper bone, +8 the lower bone, +0xC the end
    glm::vec3 vecB0{0.0f}, vecA0{0.0f};  // +0xB0 the end point (in the end's space), +0xA0 the target's point
    // the rest pose (0x186930) and the solve's state
    glm::mat4 matUpperRest{1.0f};     // +0x10
    glm::mat4 matLowerRest{1.0f};     // +0x50
    glm::vec3 axis{1.0f, 0.0f, 0.0f}; // +0x90 the bend axis (in the lower bone's parent's space)
    float sUpper = 0.0f, sLower = 0.0f, radBendRest = 0.0f;  // +0xC0, +0xC4, +0xC8
    float gWeight = 0.0f;             // +0xCC the twist about the target direction
    const TRKG* ptrkgWeight = nullptr; // +0xD0 an ASEGA's scalar channel easing it
    const float* ptWeight = nullptr;  // +0xD4 that ASEGA's time
};
struct ALOLINK  // 0x1603B8 (orig 0x10 bytes each): record pxf (mode 15) follows record pxfTarget in world space
{
    XF* pxfTarget = nullptr;  // orig +0
    XF* pxf = nullptr;        // orig +4
    int n8 = 0, nC = 0;       // follow the position / the rotation
};
struct ALOCHAIN;
struct ALOCHAINE  // a chain's segment (orig 0x70 bytes; runtime: the physics port, chain.cpp)
{
    XF* pxf = nullptr;            // +0x64: its record (mode 16)
    glm::mat3 matRest{1.0f};      // +0x00: the record's rows at the load
    glm::vec3 vecOffset{0.0f};    // +0x30: the offset to the next segment in the record's frame
    glm::vec3 pos{0.0f};          // +0x40: the simulated end position (world)
    glm::vec3 v{0.0f};            // +0x50: its velocity
    ALOCHAIN* pchain = nullptr;   // +0x60
};
struct ALOCHAIN  // 0x21C888 (orig 0x50 bytes each): a rope of records
{
    int n8 = 0;                   // +0x8: the substeps
    float ag[4]{};                // +0xC stiffness, +0x10 / +0x14 dampings, +0x18 the bend limit
    glm::vec3 vec20{0.0f}, vec30{0.0f};  // +0x20 gravity, +0x30 the last segment's rest offset
    std::vector<ALOCHAINE> ae;    // +0x4
    float tLast = -1.0f;          // +0x1C (runtime): the time of the last simulation
    glm::vec3 posAnchor{0.0f};    // +0x40 (runtime): the anchor's last position
};

// Animated local object (cid 0x30, orig size 0x150): transforms, the glob set, collision entries, children.
struct ALO : LO
{
    std::vector<std::unique_ptr<XF>> axf;   // orig +0x50 count, +0x54 records
    XF* apxfRef[3]{};                       // the three S16 references after the records
    std::vector<ALOPAIR> apair;
    std::vector<ALOLINK> alink;
    std::vector<ALOCHAIN> achain;
    uint32_t uMeta = 0;                     // the U32 after the transforms
    std::vector<SOE> asoe;                  // orig +0x?? count, entries (0xF0 each)
    bool fHasState = false;
    float gMass = 0.0f;                     // orig state +0xD0
    glm::mat3 matInertia{1.0f};
    std::vector<std::pair<uint32_t, LO*>> apart;  // its named parts (0x1A64E8): original field offset -> object
    std::vector<std::pair<uint32_t, XF*>> apartxf;  // flag 0x10: original field offset -> transform with the named tag
    std::vector<LO*> aploChild;             // the objects it loaded (orig: +0x64 first child, LO +0x24 next)
    // (native, PS2 float emulation, UE6) the game time at which a script's run-to-point move (SCPT command 0x24,
    // scpt.cpp DriveScptActor) last drove this object: its position record then integrates rounding toward zero as
    // the EE's FPU does (xf.cpp UPD::Moving), else the move's final sub-ULP creep stalls a few thousandths outside its
    // radius (Ep7 egg plant SCPT 3224: native stuck at 25.0039 of 25, the reference reaches 24.9965)
    float tScptRtz = -1.0f;
    float gFade = 1.0f, dgFade = 0.0f;      // orig +0x12C (InitLo 0x1F2108: 1) / +0x130 (0x1F36E0)
    std::shared_ptr<GLOBSET> pglobset;      // the renderer's mesh (glob.h), orig ALO +0x80 (shared by clones)
    std::vector<std::pair<int32_t, bool>> aglobShown;  // orig +0xB8 list's 0x800 flags set at run time (0x1F6978 /
                                            // 0x1F69F8: by the group's id n0; empty: the loaded flags)

    // Runtime (the physics port, so.h): the physics state (orig +0xE8), the attachment (+0xEC), etc.
    std::shared_ptr<struct SORT> psort;
};

// Native objects for classes whose loaders keep data beyond their base's. The specific loaders (classes.cpp)
// store into these; the struct of a class is that of its nearest ancestor with one.
struct LOXPATH : LOX   // 0x1E5DF8 (cid 0x0A family)
{
    int fE0 = 0, nE4 = 0;
    std::vector<int32_t> an;
    std::vector<glm::vec3> avecF4, avec140, avec144;
};
struct LOXGEOM : LOX   // 0x227380 (cid 5), 0x220FD0 (cid 0x0F)
{
    uint32_t u = 0;
    GEOM geom;
    BSP bsp;
    std::vector<LO*> aploChild;
};
struct LOXSHAPE : LOX  // 0x227BB8 family (cid 0x16..)
{
    GEOM geom;
    BSP bsp;
    uint32_t uX = 0;
    std::vector<uint32_t> au;  // 0x2282B0's records (non-null 64-byte keys)
    std::vector<std::array<uint8_t, 64>> aabKey;   // cid 0x1A (0x2282B0): the world's key (+0x144), then the movies' (+0x14C)
};
struct LOXMESH : LOX   // 0x1C03C0 (cid 0x11)
{
    struct VTX { glm::vec3 pos{0.0f}; std::vector<int> aivtx; };
    std::vector<VTX> avtx;
    std::vector<std::array<int, 3>> atri;
    struct PT { float x, y, z; uint16_t c, d; };
    std::vector<PT> apt;
};
struct LOXPTS : LOX    // 0x185150 (cid 8/9)
{
    std::vector<glm::vec3> avec;
    std::vector<std::array<int, 3>> ax1, ax2;
};
struct LOXEMIT : LOX   // 0x258598 / 0x259030 (cid 0x1D, 0x20/0x21)
{
    struct EMITMESH { std::vector<glm::vec3> apos; struct TRI { int aipos[3]; float sArea; }; std::vector<TRI> atri; glm::vec3 v20{0.0f}; };
    std::unique_ptr<EMITMESH> pemitmesh;
    int nColors = 0;
    std::vector<uint32_t> argba;
};
struct LOXCHILD : LOX  // 0x15C1E8 (cid 0x1F) and others that load children through 0x2317E0
{
    std::vector<LO*> aploChild;
};
struct LOLIST : LO     // 0x1635F8 (cid 0x85), 0x153B08 (cid 0x7A)
{
    std::vector<int32_t> an90, an9C;
    struct E { std::string str; int n4 = 0; int32_t n8 = 0, nC = 0; uint8_t ab[64]{}; };
    std::vector<E> ae;
};

struct CHKPNT : LOX    // cid 0x2A (0x228720): a checkpoint, its data from its methods (790..795)
{
    std::vector<int> aid;                       // +0x16C: the ids it answers to (0x228988; 0x189 / 0x18A)
    std::vector<std::pair<OID, OID>> apair;     // +0x148: objects placed at others (0x228958)
    std::vector<std::array<OID, 4>> arsmg;      // +0xE0: state machines' trigger states (0x20FEC0 FAddRsmg)
    std::vector<std::pair<OID, int>> atask;     // +0x124: task states (0x161F10)
};

// The native object for a class (cls.cpp binds one per struct).
LO* PloNewNative(CID cid);
