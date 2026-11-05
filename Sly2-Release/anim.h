#pragma once

// The animation runtime's entry points for the game loop (part of the animation code):
//   after a world load and StartSw:  InitWorldAnimation(psw)  (records' state, joints, bone bindings, SM tables,
//                                     then the PostLoad of ASEGs (0x12F8F0) and SMs (0x1F00C8))
//   every frame (registered as UpdateSw passes, update.h):
//     0x130918  UpdateAsegas: every playing ASEGA (after the objects' updates and 0x19F120)
//     0x1F8DD8  the update-table objects whose transforms move (flag 4, not 0x20): their records (vt slot 17
//               0x1F3980 -> 0x22DE88) after the ASEGAs
//     0x2267C0  (paused) the ASEGAs of ASEGs with flag 0x100, the records of entries with 0x104 (not 0x20)
// The clock is the world's (clock.h, orig 0x2DDED0).

#include <cstdint>
#include <vector>

struct SW;
struct ALO;
struct LO;

// The world's start in two parts, as StartSw (0x20E740) runs them: the records, joints, bone bindings and the ASEG /
// SM tables before the objects' vt slot 15, then each object's slot-15 part (PostAsegLoad 0x12F8F0, PostSmLoad
// 0x1F00C8) in load order inside that loop (after the base 0x1A5C48), so the tasks started in the same loop find the
// SMAs of the objects before them.
void InitWorldAnimationRecords(SW* psw);
// (PC addition: the same passes over a list of objects only, no table reset: copies made while the world runs)
void InitAnimationRecordsOf(const std::vector<LO*>& aplo);
void PostLoadAnimLo(LO* plo);
void InitWorldAnimation(SW* psw);
// One frame of animation alone (tests without the world's loop): the records of every animated ALO, the ASEGAs.
void StepWorldAnimation(SW* psw, float dt);
// The records of every ALO that has some (tests).
void UpdateWorldXfs(SW* psw);
// Every ALO's world matrices from its records, parents first (after a direct change of locals).
void UpdateWorldXfWorlds(SW* psw);

// The update-table flags of an object (orig +0x40: the entry's flags while in the table, else the object's own).
uint32_t GrfUpdOfLo(const LO* plo);
void SetGrfUpdOfLo(LO* plo, uint32_t grf);
// orig 0x001F2FE0 (ALO vt slot 47): flags into the object and its children (4: its transforms move)
void MarkAloXfs(ALO* palo, uint32_t grf);
