#pragma once

// The world (Sly 1's SW; the original's static game state at 0x3DD490) and its loader LoadSwFromBrx (0x13DA18).

#include "cls.h"
#include "disc.h"
#include "lo.h"
#include "task.h"

#include <array>
#include <cstring>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

class CBinaryInputStream;

// A group of the world file (orig WE, 0x6C bytes at SW +0x3F08): a named set of objects, loaded once and
// instanced c8 times (the copies are clones).
struct WE
{
    char achName[64]{};       // orig +0x1C (64 bytes)
    int32_t x5C = 0;          // orig +0x5C: nonzero: a shared group (loads with iwe -1, no PostLoad)
    int32_t x60 = 0;          // orig +0x60: proxies of it instantiate (0x1C9B08)
    int32_t x10 = 0;          // orig +0x10: the script table's id (0x1FF858)
    int32_t x14 = 0;          // orig +0x14
    int32_t cinst = 0;        // orig +0x08: instances
    int32_t cUsed = 0;        // orig +0x0C
    std::vector<LO*> aplo;    // orig +0x04: the loaded objects (c0 = orig +0x00), then their copies
    int cloLoaded = 0;
    std::vector<LO*> aploRowOwner;            // orig +0x68 nodes: the streaming proxy each instance is attached to
    std::vector<glm::mat4> amatRow;           // and the matrix it was placed by
    std::vector<uint8_t> afRowLocked;         // orig node +0x08 (PROXY methods 1179 / 1180: kept)
    // orig PROXYSRC +0x64 (0x1CA500): each object's rest pose, the first spare instance's world position and
    // normalized rotation after the load; a released instance's object goes back to it (0x1CAA60)
    std::vector<glm::vec3> aposRest;
    std::vector<glm::mat3> amatRest;
    std::vector<glm::mat4> amatRootRest;      // (native: the first record's local at the same point, see 0x1CAA60)
    std::string StrName() const { return std::string(achName, strnlen(achName, sizeof(achName))); }
};

// A streaming proxy's data (orig PROXY +0xE0 group, +0xE8 its attached instances, +0xEC binds)
struct PROXYBIND
{
    int32_t kind = 0;
    int type = 0;
    std::shared_ptr<CFrame> pframe;  // orig +0x08: the bind's splice frame, retained until the instance attaches
    std::vector<uint8_t> ab;  // options
    LO* ploBound = nullptr;  // orig +0x14: the object whose frame is restored when this proxy detaches
};
struct PROXYSTREAM
{
    int iwe = -1;
    std::vector<int> airow;   // attached instances, the latest first
    std::vector<PROXYBIND> abind;
};

// The progress-name trie (0x1D7B88 into SW +0x3A18).
struct PROGRESS
{
    std::vector<std::pair<int, std::string>> aname;  // (value, name)
    std::vector<uint8_t> abRecords;                   // c * 10 bytes
};

// The world records (0x20F318: 44 records at 0x3E1C40..).
struct WREC
{
    FK fk;
    uint8_t abRecord[64]{};
    int slot = 0;      // orig 0x3E1C44[i]: the episode
    int kind = 0;      // orig 0x3E1C48[i]
    bool fMain = false;  // the episode's main world (orig 0x3E1C50[slot]: 0x210B98)
    std::vector<uint16_t> an;
};

// The node tree (0x13E0E0 into SW +0x4558): a vector (a plane?) and two links each.
struct NODE
{
    glm::vec4 vec{0.0f};
    int inodeA = -1, inodeB = -1;  // -1: a leaf (gets the next leaf index)
    int ileafA = -1, ileafB = -1;
};

// The collision mesh of a group (0x1C0EC0 / 0x1C0F30 into SW +0x45CC).
struct CMESH
{
    struct E { int ivec = 0; int16_t a = 0, b = 0; };
    std::vector<E> ae;
    std::vector<glm::vec4> avec;
    std::vector<int32_t> an;
};

struct UPDENT
{
    uint32_t grf = 0;
    LO* plo = nullptr;
};

struct SW
{
    SW() = default;
    SW(const SW&) = delete;
    SW& operator=(const SW&) = delete;
    ~SW();  // the unload (sw.cpp)
    inline static uint32_t s_serialNext = 0;
    uint32_t serial = ++s_serialNext;   // (native) a new world never matches an old one's address-only check
    bool fNavReady = false;             // (native) the navigation regions are made (StartSw's 0x1C10C8 on): the
                                        // original's GAME +0x45CC +0x2C test in 0x1F2C30

    std::vector<WE> awe;                         // orig +0x3F04 count, +0x3F08 groups
    int iwe = -2;                                // orig +0x3F10: the group being loaded (-1: shared, -2: none)
    int iweChannel = -1;                         // (native) the group being loaded, shared ones too: the objects' channel
    std::vector<std::unique_ptr<LO>> aploAll;    // owns every object
    std::array<std::vector<LO*>, CID_Max> mpcidaplo;  // orig +0x3038 + cid * 12: per class, in creation order
    std::vector<LO*> aploLoad;                   // orig +0x39E0..: the objects of the current load, in order
    std::vector<UPDENT> aupd;                    // orig +0x3F20 entries, +0x3F14 count
    int cupdDead = 0;                            // orig +0x3F18
    LO* plo7D = nullptr;                         // orig 0x3DD48C: the guards' shared 0x7D object (0x205E70)
    int iupdCur = -1;                            // orig *(+0x3F1C): the entry being updated
    // the activation (activate.cpp): the queued roots (orig DL +0x20), the pass's stop (+0x39A4), the camera's
    // flags and the positions when the stop was reset (+0x39A8, +0x39B0, +0x39C0)
    std::list<LO*> lsplo20;
    LO* ploStop = nullptr;
    TASKSW tasks;
    LO* ploExitPending = nullptr;                // orig +0x3C84: the exit that entered state 4, until unload
    int cClue = 0;                               // orig +0x4760 (the clue bottles' count, 0x1C8030)                                // the jobs and tasks (task.h; orig +0x3B00..)
    std::unordered_map<LO*, PROXYSTREAM> mpproxy;  // the streaming proxies (activate.cpp)
    std::unordered_map<LO*, glm::mat4> mpmatPlayerInstance;  // the players' instance matrices (0x1CA370's slot 37)
    uint32_t grfCmLast = 0;
    glm::vec3 posCmLast{0.0f}, posPoLast{0.0f};

    PROGRESS progress;
    uint32_t cwrec = 0, x3E1C3C = 0;
    std::vector<WREC> awrec;
    std::vector<std::pair<int, std::string>> atext;  // orig +0x4640: (id, string of the chosen language)
    std::vector<int> aidTextModded;                   // (native) the ids the mods folder's text replaced (textmod.h)
    std::string strTextChars;  // (the Japanese build, JP +0x4848: the characters the text uses; region.h)
    std::vector<NODE> anode;                     // orig +0x4558
    int cnodeLeaf = 0;
    std::vector<OPT> aoptSw, aoptCm;             // the options loaded for the SW and the camera (0x2DDF40)
    std::vector<std::unique_ptr<CMESH>> acmesh;  // one per group that has one (the last is current)
    int32_t fScriptDebug = 0;                    // orig 0x3E1C34: scripts carry debug records
    int cmeshes = 0;                             // orig 0x3DD490 +0x3D70 (0x1C03C0's counter)
    LO* ploMesh2CA = nullptr;                    // orig +0x45C8 (the 0x1C03C0 object with oid 0x2CA)
    int filter = -1;                             // orig 0x3E1110: the world record index objects filter on
    float tGuardAlarmResume = 0.0f;               // orig +0x4758; cleared by the world reset 0x20E890
    // (native, perf-2) the objects by name (AploOfOid): kept up to date lazily from aploAll (only appended) and by
    // NoteOidInstanceChanged
    struct OIDINDEX
    {
        std::unordered_map<OID, std::vector<LO*>> mp;   // oid -> the objects with that oid or oidInstance, in aploAll order
        std::unordered_map<const LO*, uint32_t> mpipos; // an indexed object's place in aploAll
        std::vector<LO*> aploAll;                        // every object (the scans' answer for oid -1: every oidInstance -1 matches)
        size_t cDone = 0;                                // aploAll's objects indexed
    };
    mutable OIDINDEX oidindex;
};

extern SW* g_psw;

// The current world's side tables pointing at its objects released (~SW's first part, with g_psw that world).
void ReleaseSwRuntimeTables();

// orig 0x13DA18: loads a decompressed world file into psw.
void LoadSwFromBrx(SW* psw, CBinaryInputStream* pbis);

LO* PloFindSwObjectByOid(SW* psw, OID oid);

// (native, perf-2) the world's objects whose oid or oidInstance is oid, in aploAll order: the name finds' scans of every
// object, each filtering on the name, go through these instead (the same objects in the same order). Valid until the
// next object is made. (oid -1: every object, as the scans testing oidInstance == oid matched them all.)
const std::vector<LO*>& AploOfOid(const SW* psw, OID oid);
// An object's oidInstance changed from oidOld (its writers call it after the write).
void NoteOidInstanceChanged(const SW* psw, LO* plo, OID oidOld);
bool FLoInWorldNs(const LO* plo);  // (the object in the world's namespace node: not a group's / an instance's)

// Loads the world file fk into psw (trace: a parse map of the load, or ""); false on a load error.
bool FLoadWorldFile(SW* psw, const FK& fk, const std::string& trace, std::size_t* pcbRead, std::size_t* pcbFile);
bool FLoadWorldBytes(SW* psw, const FK& fk, std::vector<uint8_t> ab, const std::string& trace, std::size_t* pcbRead, std::size_t* pcbFile);
void ResetWorldLoadTables();   // (FLoadWorldFile's resets of the old world's side tables)
// "boot", "frontend" or a world record index (the records come from the boot world): its file and filter.
bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& error);
