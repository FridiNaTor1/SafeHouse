// guard3.cpp: Sly 3's numbers for the guards' copied Sly 2 AI (guard.h): the state indices and the guard options
// that changed between the games. The AI code keeps Sly 2's numbering; the data (the world files' kind lists, the
// scripts) gives Sly 3's, translated here at the boundary.

#include "guard.h"

#include "game/sly3.h"

#include <array>
#include <cstdlib>

namespace
{
// The guard state indices (the mask +0x2C0; Sly 3's is wider: indices up to ~70). Sly 3 -> Sly 2, from:
// - the kind lists of the same guard in both games (Sly 2's rat 125317 of world 3 and Sly 3's rat 144809 of v_ext:
//   the same entries, the same clips' order and flags): 0..7 the same, 13/14 = 8/9 (the walk), 17..21 = 11..15,
//   35..42 = 16..23, 46/47/49/51 = 26/27/29/31, 60 = 43;
// - the immediates of AddGuardState / RemoveGuardState (Sly 2 0x209890 / 0x209970, Sly 3 0x252E40 / 0x252FC0) in
//   the mapped functions (tools/s3maps/guardstates.py): 22 -> 41, 27 -> 47, 31 -> 51 (JOBINVESTIGATE 0x1A64A8),
//   33 -> 53, 36 -> 54, 38 -> 56, 44 -> 61, 46 -> 63 (votes of one or two calls; 30 / 39 / 34 / 35 / 24 conflict:
//   left out).
// - (2026-11-10, the Sly 3 decomp) the immediates of every 0x252E40 / 0x252FC0 call site in the decomp's C, paired with
//   the Sly 2 decomp's 0x209890 / 0x209970 call in the fnmap twin (scores 90..100): they confirm 41, 47, 51..54, 56,
//   60, 61, 63 above and add 50 -> 30 (0x1EF838 / 0x179A58), 55 -> 39 (0x1638C0 / 0x17CE68), 67 -> 34 (0x133A58 /
//   0x155560, class 0x3B's), 68 -> 24 (0x1353F0 / 0x1753D8). Left out (two Sly 3 states claim one Sly 2 state, a
//   kind entry would overwrite the other's): 12 / 52 -> 32, 43 / 60 -> 43, 44 / 68 -> 24. Sly 3's JOBFOLLOW init
//   0x1D8BA8 moves with state 14 (+0xB0), as Sly 2's 0x178E98 with 9.
// Sly 3's indices 15, 16, 22..34, 43, 45, 48, 59, 62, 64..66 and 68 (8..12, 57, 58: below) have no Sly 2 state here (16:
// Carmelita's chase run sets it next to 14; no Sly 3 code adds it by an immediate).
// (audit4 AI-17, 2026-11-17: re-derived from the job classes' own Init (vt slot 7) in the decomp, not the fnmap twins:
// JOBPROX is Sly 3's 0xD7 (its Update 0x163968 is the prox's), whose Init 0x1638C0 adds 55: 55 = Sly 2 37 (JOBPROX's),
// not 39 (0x17CE68 is JOBLADDER's Init, a mispaired twin). JOBACT is 0xD1 (the message slot, the size 0x50), whose
// Init 0x1A4C28 adds 44: 44 = Sly 2 24; 68 (0x1353F0) is class 0xF1's, a Sly 3 job with no native port. JOBB4 is 0xEB
// (Init 0x133A58, Exit before Enter as Sly 2's 0x155560): 67 = 34 stands. Probable, not added: 45 = 25 (0xDC
// JOBSURROUND 0x181F30), 58 = 39 (0xE2 JOBLADDER 0x1DA7A8), 62 (0xE5, the reaction's settle job).)
// (s3-split, 2026-11-12) the playable Carmelita's states (her kind entries {66, n}: aicarm3.h): Sly 3's 15, 22..30
// and 66 have no Sly 2 state; they take Sly 2 indices no native code sets (47..57), so her player entries load and
// match only while her job 0xE9 has added 66 (-> 57). No other guard's state ever has those bits.
// (s3fix-ai2, 2026-11-17) the jobs' own Init (vt slot 7) again: 0xE0 JOBLADDER (op 6 with +0x10F0 1) adds 57 = Sly 2 39
// (0xB9's), 0xE2 JOBHOLD (op 5 with +0x10F0 2) 58 = 41 (the hold on a class-0x25 object: 0xCB adds 58 for it, JOBUSE 0x29 = 41 for Sly 2's 0x27), 0xE4 JOBLINK (the go-to's state 3) 61 = 44. And Sly
// 3's new states 8..12 (job 0xEA, the trigger reaction: 0xC with 8 / 9 / 0xA / 0xB) on Sly 2 indices no native code sets
// (59..63, in Sly 3's order), so their kind entries load and the guard's +0x1144 flags 0x20..0x100 (guardstart.cpp) hold.
constexpr std::array<std::pair<int, int>, 59> c_aistate32 = {{
    {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7},
    {13, 8}, {14, 9},
    {17, 11}, {18, 12}, {19, 13}, {20, 14}, {21, 15},
    {35, 16}, {36, 17}, {37, 18}, {38, 19}, {39, 20}, {40, 21}, {41, 22}, {42, 23},
    {46, 26}, {47, 27}, {49, 29}, {51, 31}, {52, 32}, {53, 33}, {54, 36}, {56, 38},
    {60, 43}, {61, 44}, {63, 46},
    {50, 30}, {55, 37}, {67, 34}, {44, 24},  // (orig3: the decomp's AddGuardState call sites, above; audit4 AI-17)
    {15, 47}, {22, 48}, {23, 49}, {24, 50}, {25, 51}, {26, 52}, {27, 53}, {28, 54}, {29, 55}, {30, 56}, {66, 57},
    {65, 58},   // (s3-ai) the ridden guard's state 0x41 (job 0xE8, airide3.h: its ride entries {65, n}); no native code sets 58
    {57, 39}, {58, 41},   // (s3fix-ai2: 0xE0 / 0xE2's Init; 0xCB's ladder 0x39 / class-0x25 hold 0x3A = JOBUSE's 0x27 / 0x29)
    {8, 59}, {9, 60}, {10, 61}, {11, 62}, {12, 63},   // (s3fix-ai2: job 0xEA's states)
    {-1, -1}, {-1, -1},
}};
}  // namespace

int IstateGuard2FromIstate3(int n3)
{
    for (const auto& [a3, a2] : c_aistate32)
        if (a3 == n3 && a3 >= 0) return a2;
    return -1;
}

int IstateGuard3FromIstate2(int n2)
{
    for (const auto& [a3, a2] : c_aistate32)
        if (a2 == n2 && a2 >= 0) return a3;
    return -1;
}

int Eopid2GuardFromEopid3(int e3)
{
    // (Sly 3 dropped the kind-2 entry option, Sly 2's 245: its 267..271 are Sly 2's 243, 244, 246, 247, 248; the row
    // 246 <-> 269 is a hand-checked row of tools/s3maps/eopidmap.py, so the table answers)
    return sly3::Eopid2FromEopid3(e3);
}

// orig3: 0x2557E0 (STEPGUARD vt slot 72, Sly 2 slot 68: no Sly 2 twin; every guard class 0x3A.. has it, Carmelita
// too): a contact row's responses (FillAloDzResponse 0x2448F8 = Sly 2's 0x1FAD00) with the normal scaled by 0.01
// while the contact's other end (the XP's (1 - k)'th side, +0x8) is derived from Sly 3's class 0x55: the guard hardly
// pushes against those.
#include "dzg.h"
#include "so.h"
#include "vtnative.h"
void FillAloDzResponse(ALO* p, DZ* pdz, DZENT* pent, int grf, int k);  // (solve.cpp, orig 0x1FAD00)
namespace
{
void FillGuardDzResponse3(ALO* p, DZ* pdz, DZENT* pent, int grf, int k)
{
    const glm::vec3 normal = pent->normal;
    const ALO* pother = pent->pxp && (k == 0 || k == 1) ? pent->pxp->axpd[1 - k].paloRoot : nullptr;
    if (pother && FCidDerivedFrom(pother->Cid(), 0x55)) pent->normal = normal * 0.01f;
    FillAloDzResponse(p, pdz, pent, grf, k);
    pent->normal = normal;
}
}  // namespace
NATIVEFN3(0x2557E0, FillGuardDzResponse3);

// orig3: 0x24E3A8 (STEPGUARD vt slot 35, Sly 2's slot 30: a new orientation; the guard classes 0x3A.. and Carmelita):
// the ALO's (0x23D338 = so.cpp SetAloRotation), then the step heading (0x24E878) from the object's x axis (vt slot 36,
// atan2 0x2A3EE8). Natively the slot had no function ("[vt] no native function", audit3 F19): a guard turned by a
// script or a placement kept walking by its old heading.
#include "stepphys.h"
namespace
{
void RotateStepGuard3(ALO* palo, const glm::mat3& basis)
{
    SetAloRotation(palo, basis);
    if (XF* pxf = PxfObj(palo))
    {
        const glm::vec3 x = glm::vec3(pxf->matWorld[0]);
        SetStepPhysHeading(palo, std::atan2(x.y, x.x));
    }
}
}  // namespace
NATIVEFN3(0x24E3A8, RotateStepGuard3);
