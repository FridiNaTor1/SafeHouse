#pragma once
#include <cstdint>
#include <glm/glm.hpp>

// The actors' targets (orig 0x140558 / 0x140678 / 0x208E70 / 0x1428D8): each frame the guards ranked for the actor
// (4 grabbing it, 3 it is inside their volume, 2 near, 1 far, 0): the nearest at +0x10DC (its rank +0x10E4) and the
// best ranked (then nearest) at +0x10E8 (+0x10EC). Sly's state 5 (0x140AF8) follows a best of rank 4.

struct ACTOR;
struct SLY;
struct LO;

// orig: 0x00140558 (unless +0x10E0)
void PickActorTargets(ACTOR* pa);
// orig: 0x00140678 (vt slot 95)
void StopNearestLaunchTracking(ACTOR* pa);  // 0x1431D0
void SetActorTargets(ACTOR* pa, int nNear, LO* ploNear, int nBest, LO* ploBest);
// orig: 0x0018F2B0 (the nearest target point of the mask to the actor's predicted point offset by the heading, within the
// facing cone; *pdir its predicted frame's origin less the actor's predicted point; null when none)
LO* ChooseAttackTarget(ACTOR* pa, float heading, uint32_t mask, const glm::vec3& offset, float dt, float cone, glm::vec3* pdir);
// orig: 0x0018F688 (Knockout Dive's target and initial heading)
void ChooseSlyRushTarget(SLY* psly);
void ChooseSlyAirSwingTarget(SLY* psly);  // orig: 0x18F740
// (a harness importing the reference's targets turns the native pick off)
extern bool g_fActorTargetsImported;

// orig: 0x001428D8 (the look score of po for the actor: *pdxy the horizontal one, *pdz the height one)
void LookScore(ACTOR* pa, LO* po, LO* const* ppoCur, float* pdxy, float* pdz);
