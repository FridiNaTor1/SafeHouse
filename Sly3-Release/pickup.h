#pragma once
// pickup.h: the pickups (cid 0x5A and its 0x5B coins, 0x5C carryable prizes, 0x5D, 0x5E; module 0x1C4E50..0x1C7EE8)
// as far as the pickpocket's reward needs them: the state +0x150 (vt slot 76: 0x1C60B8 and the classes' 0x1C65F8 /
// 0x1C6B28 / 0x1C68F0 / 0x1C77E8), the launch 0x1C6BD0, the update (slot 16: 0x1C5E48 and the coin's 0x1C6588) and the
// motion (slot 17: 0x1C5148). States: -1 none, 0 resting, 1 collected, 2 gone, 3 rising, 4 falling, 5 resting (timed
// out), 6 held. Only the pickups this module launched run natively; the others keep the ALO's defaults.
// pickup.cpp (the actors code).
#include <glm/glm.hpp>

struct LO;

bool FPickupLive(LO* plo);                        // a pickup with its runtime (launched)
void SetPickupState(LO* plo, int n);              // vt slot 76
void SetPickupLootKind(LO* plo, int id);          // orig 0x001C7A58 (loot variant)
// orig: 0x001C6BD0 (placed at pos with the velocity: added, detached, state 3; dtSm the SM's delay +0x160, dtCollect
// the time until it is collected by itself +0x1CC (FLT_MAX: never))
void LaunchPickup(LO* plo, const glm::vec3& pos, const glm::vec3& vel, float dtSm, float dtCollect, LO* pproxy);
void ResetPickups();
// orig: 0x00154E10 (a breakable's item 10: the drops at pos, n coins (-1: drawn), the charm counter's step dn)
void DropPickups(const glm::vec3& pos, int n, int dn, LO* psrc);
void PostLoadPickup(LO* plo);
void SetClueIndex(LO* pproxy, int n);             // the clue bottle proxy's +0x100 (0x1C8030)                     // orig 0x001C4F60 (vt slot 15)

// orig: 0x00161868 / 0x001618B0 (the coin / clue bottle counters (screen objects 0x36D / 0x36E) shown for their 5 s)
void ShowCoinCounter();
void ShowClueCounter();
// orig: 0x001A58D0(p, 0, 0) (vt slot 10 with no parent: out of its parent, the root record unrotated / unscaled at its
// world position, as the reference leaves it)
void SetLoParentNoneRoot(LO* plo);

LO* PproxyOfInstance(LO* plo);  // 0x1CB658: the streaming proxy whose instance plo is (null if none)
