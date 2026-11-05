#pragma once

// The players' interaction objects (the reference's "JT" module 0x191090..0x195950 and the jump's targets
// 0x19A400..0x19C870): the objects a player can climb, hang from, swing on, pick up or use, each with a
// description part (orig: a component at a class's fixed offset with its own vtable, base 0x182C70: +0x8 the
// height reach, +0x10 the priority, +0x14 the prompt's text id, +0x18 the group, +0x1C the players' mask), kept by
// the world in one list per kind (orig SW +0x3F24..+0x3FA8 = 0x3E13B4..0x3E1438, filled by the classes' vt slots
// 22 / 23 (11 / 12): the objects in the world).
//
// The classes' vt slots 23 / 22 add to / remove from the update table (the active objects), 11 / 12 the world. Natively
// the lists are the class lists' objects (active, or in the world) whose class has the list's add function in its
// vtable (so the subclasses' overrides are honoured), in creation order (the original appends as they come).

#include "cls.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct LO;
struct SW;
struct ACTOR;
struct CRV;

enum JTL
{
    JTL_Point = 0,     // 0x3E13B4: cid 0x12 family (only 0x13 objects with +0xE8 & 1 count): the hang points
    JTL_Pipe = 1,      // 0x3E13C0: cid 0x0B (PIPE: climbed along its path, Sly's state 8 / +0xF50)
    JTL_Rail = 2,      // 0x3E13CC: cid 0x0D
    JTL_Spire = 3,     // 0x3E13D8: cid 0x27
    JTL_Jump = 4,      // 0x3E13E4: cid 0x58 (the jump's own targets: 0x197F98)
    JTL_Ledge = 5,     // 0x3E13F0: cid 0x0C
    JTL_Wall = 6,      // 0x3E13FC: cid 0x22
    JTL_Swing = 7,     // 0x3E1408: cid 0x26
    JTL_Use = 8,       // 0x3E1414: cid 0x23 / 0x24
    JTL_Jump2 = 9,     // 0x3E1420: cid 0x28 (slots 11 / 12)
    JTL_Point2 = 10,   // 0x3E142C: cid 0x09 (slots 11 / 12)
    JTL_Hook = 11,     // 0x3E1438: cid 0x1B
    JTL_Max = 12,
};

// The description part of an interaction object (orig: its component; the defaults of 0x182C70 and the class's
// constructor, then the load's field options).
struct JTDESC
{
    uint32_t oidZone = 0;           // +0x0 its camera zone (a ZONE's oid; 0x192BD8 / 0x19EAB0's 0x140C88)
    float height = 3.40282347e38f;  // +0x8 how far above it the player may be (0x19B110)
    int prio = 0;                   // +0x10
    int id = -1;                    // +0x14 the prompt's text id (-1 none)
    uint32_t kind = 0;              // +0x18 the group (one candidate per group: 0x194688)
    uint32_t mask = 7;              // +0x1C the players it is for (AND the actor's +0x1000)
};

// The list's objects (in the world) and the offset of their description part (orig 0x194A40's table).
const std::vector<LO*>& AploJtList(SW* psw, JTL jtl);
uint32_t IbJtDesc(JTL jtl);
JTDESC JtDescOf(LO* plo, JTL jtl);
bool FJtInList(LO* plo, JTL jtl);

// The world matrix of an object as the original's vt slot 31 (0x1A6318) gives it: an SO's object record, a LOX's
// matrix in its reference record's frame.
glm::mat4 MatJtLo(LO* plo);
inline glm::vec3 PosJtLo(LO* plo) { return glm::vec3(MatJtLo(plo)[3]); }
// orig 0x1A6F00 / 0x1A6FC0: a point / a direction from a's frame (null: the world) to b's
glm::vec3 PosJtConvert(LO* a, LO* b, const glm::vec3& v);
glm::vec3 VecJtConvert(LO* a, LO* b, const glm::vec3& v);

// PIPE (cid 0x0B, a PATH): its curve, its frame at a distance (0x1C3500: the climber's matrix, rows: x toward the
// pipe's side, y, z along it (reversed unless fUp)), the vector +0x1C0, the effects +0x1D0 / +0x1D4.
const CRV* PcrvJtPipe(LO* ppipe);
glm::mat3 MatPipeFrame(LO* ppipe, bool fUp, float s);
int NPipeEffect(LO* ppipe, bool fRelease);
// orig 0x1E6728 (a distance kept on the curve: wrapped when closed, else clamped to its ends)
float SClampCrv(const CRV* pcrv, float s);

// SLY2N_JTPRINT=1: each list's objects with their description at the first world update (a debug dump).
void PrintJtLists(SW* psw);

// The candidates of a player (orig 0x194A40 each frame of the current player's input: Sly 0xFFFF, Bentley 0xF8FE,
// Murray 0xF8FA): the lists of the kinds in grf (8 ledges, 0x10 walls, 0x80 uses, 4 hooks, 0x400 spires, 0x200 rails, 2
// pipes, 1 points, 0x20 swings) rated (0x194688: the object's point for the player (its description's methods 2 / 3),
// the distance (method 4), at most 1000; by priority then distance; one per group), then the game's tracked targets
// (SW +0x4678: at most 4, sorted by distance, 0x194F70) and the HUD prompt of the nearest within 250 (0x1DA4B8).
struct JTENT
{
    LO* plo = nullptr;             // +0x0
    JTL jtl = JTL_Max;             // (+0x4 the description)
    float dist = 1000.0f;          // +0x8
    int prio = -1;                 // +0xC
    LO* ploAura = nullptr;         // +0x10 the aura shown on it (the player's +0x9C4 proxy's instance, 0x1956D0)
    float t = 0.0f;                // +0x14 when it was found
    float g = 0.0f;                // +0x18 the point's parameter (a curve's distance)
    float v = 0.0f;                // +0x1C
    glm::vec3 pos{0.0f};           // +0x20 the point
};
struct JTTRACK
{
    int c = 0;                     // SW +0x4678
    JTENT ae[4];                   // SW +0x4680
    int idShown = -1;              // SW +0x4740 the prompt shown
};
extern JTTRACK g_jttrack;
// orig: 0x00195608 (the world's splice method 0x10C2E8: the aura of SW +0x4744 (the world's proxy 0x624) on the tracked
// target plo, replacing its own)
void ShowJtAura(SW* psw, LO* plo);
void UpdateJtCandidates(ACTOR* pa, uint32_t grf);  // orig 0x194A40
void ResetJtTrack();                               // a new world
// orig: 0x001938C0's choice (circle: the first tracked target within 250 by its class: 0x13 / 0x0B -> 6 for the main
// Sly, else 5; 0x1B -> 8; 0x0C / 0x22 -> 0; 0x26 -> its +0x10C; 0x23 -> 7; none -1). The action itself (0x1928F8 on the
// +0x4C0 sub-object) is not ported yet: the callers get the kind and the object.
int KindJtCircle(ACTOR* pa, LO** pplo);
// the description's methods 2 / 3 (0x182CD8: its parameter for the actor; its point and facing angle at g)
float GJtDescInit(LO* plo, JTL jtl, ACTOR* pa);
glm::vec3 PosJtDesc(LO* plo, JTL jtl, ACTOR* pa, float g, float* prad);
