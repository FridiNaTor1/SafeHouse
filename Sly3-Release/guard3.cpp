// guard3.cpp: Sly 3's numbers for the guards' copied Sly 2 AI (guard.h): the state indices and the guard options
// that changed between the games. The AI code keeps Sly 2's numbering; the data (the world files' kind lists, the
// scripts) gives Sly 3's, translated here at the boundary.

#include "guard.h"

#include "game/sly3.h"

#include <array>

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
// Sly 3's indices 8..12, 15, 16, 22..34, 43..45, 48, 50, 55, 57..59, 62 and 64.. have no Sly 2 state here.
constexpr std::array<std::pair<int, int>, 41> c_aistate32 = {{
    {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7},
    {13, 8}, {14, 9},
    {17, 11}, {18, 12}, {19, 13}, {20, 14}, {21, 15},
    {35, 16}, {36, 17}, {37, 18}, {38, 19}, {39, 20}, {40, 21}, {41, 22}, {42, 23},
    {46, 26}, {47, 27}, {49, 29}, {51, 31}, {52, 32}, {53, 33}, {54, 36}, {56, 38},
    {60, 43}, {61, 44}, {63, 46},
    {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
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
