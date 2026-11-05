#pragma once

// Sly 3's trigger volumes (class 0x15, vt 0x3549C0, 0x250 bytes: a SHAPE 0x13 with the trigger component at +0x120,
// whose vtable 0x354958 sits at +0x20C) and the run-time add / remove of objects (vt slots 8 / 9) the tasks use.
// Re-derived from the Sly 3 asm (there is no Sly 2 twin of the trigger module 0x15B068..0x15D7xx) and checked against
// the reference's FNTRACE of the m_ext prologue (the Sly 3 animation):
//   - each frame UpdateSw 0x27D7E8 calls vt slot 55 (0x15D510) of every trigger in the list SW +0x473C (the active
//     ones: the component +0x210's activation by distance, 0x15CFD8 / 0x15D048), after the objects' updates;
//   - 0x15D510: a trigger in state 2 (entered) goes back to 1 when +0x128 >= 0 and +0x128 s have passed; in states
//     0 / 1 it collects the objects inside (0x15D2F0: its watch list +0x13C, the classes of EOPID 483 +0x180 through
//     CFindObjectsByClass 0x17E208, else the players 0x1F4B30; each in the world (0x1C8610) whose world position, in
//     the trigger's frame (the inverse of its world matrix, vt slot 36), is inside the shape 0x27F298: the bounding
//     sphere +0x100 / +0x110, outside it +0x114, inside it the BSP 0x2B9C08) and sets state 1 (some inside, +0x134
//     the first) or 0;
//   - the state change 0x15C178: 0 -> 1 is the enter (0x15BF28: message 5, then 0x15BFF8(1)), then state 2 unless
//     +0x12C, and the trigger removes itself when +0x130; any -> 0 from a state other than -1 is the exit (0x15BF90:
//     message 6, 0x15BFF8(0));
//   - 0x15BFF8(fEnter): the RSMG list +0x194 / +0x198 (RunDockRsmgs 0x25D3F8), the SCPT list +0x1FC / +0x200, and on
//     the enter the SCPT +0x190 (started with the first object inside) and the task pairs +0x1D8 / +0x1DC (EOPID 489:
//     ApplyCheckpointTasks 0x185908 with the trigger as the context);
//   - RemoveLo (slot 9) of a trigger sets its state 0 (the exit when it was entered); the load state is -1, so a
//     trigger whose objects are inside at its first check does not fire (only 0 -> 1 does).
// m_ext: task 1798's state-1 object is the trigger 16374 (EOPID 483: class 0x34 Sly; 489 {1798, -1}). The reference
// removes it at the world start (1328: 1798 locked), adds it when 4268's 0x1E makes 1798 available (3031: START) and
// it fires in the same frame, Sly standing in it at the checkpoint 15070 where 4268's 0x22 placed him.

#include <glm/glm.hpp>

#include <functional>

struct SW;
struct LO;

namespace sly3
{
// What the game plugs in (game3.cpp / the player / the splice side)
struct TRIGGERHOST3
{
    // an object's world position (vt slot 36's matrix +0x30) when the native records do not hold it (the player's
    // interim motion); false: from the records (PosLoWorld3)
    std::function<bool(const LO*, glm::vec3&)> pfnPos;
    // the message to the trigger's own object (vt slot 3 0x1C86B8: its splice handler, then the broadcast 0x1854C8
    // with 0x1D / 0x1E for 5 / 6)
    std::function<void(LO*, int msg)> pfnMessage;
    // a SCPT started by a trigger (0x225EE8 StartScpt(scpt, the first object inside))
    std::function<void(LO* pscpt, LO* ploArg)> pfnStartScpt;
};
extern TRIGGERHOST3 g_trighost3;

// The world's triggers (after the world load, before StartSw's RestoreTasks)
void StartTriggers3(SW* psw);
// The UpdateSw pass over the active triggers (vt slot 55 0x15D510 each)
void UpdateTriggers3(SW* psw);

// vt slot 8 AddLo (0x1C7530) / slot 9 RemoveLo (0x1C76F0) on the native objects: the removed flag, the in-world flags
// of the object's tree (OnAdd / OnRemove, slots 11 / 12), the triggers' state, the scene entries shown / hidden
void AddLo3(SW* psw, LO* plo);
void RemoveLo3(SW* psw, LO* plo);
// 0x1C8610: in the world (added under an added chain)
bool FLoInWorld3(const LO* plo);
// An object's world position (the host's, else an ALO's first record, a LOX's matrix)
glm::vec3 PosLoWorld3(const LO* plo);
// The world matrix of a LOX (its matrix under its parent record) or an ALO (its first record)
glm::mat4 MatLoWorld3(const LO* plo);
// SCPT 0x22's place (0x22B... PlaceLo: the object's matrix rows mat and position pos): an ALO's first record moves so
// its +0x48 record (MatLoWorld3) is at pos with the rows mat; a LOX's matrix is set
void PlaceLo3(LO* plo, const float mat[9], const glm::vec3& pos);
// (tests) a trigger's state (-1 none / not a trigger)
int TriggerState3(const LO* plo);
}  // namespace sly3
