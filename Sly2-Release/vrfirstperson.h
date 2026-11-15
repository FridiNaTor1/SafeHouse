#pragma once

// VR's First Person mode (experimental; vr.h VRMODE_FirstPerson): the eyes at the player character's head, its body
// hidden, its hands on the motion controllers and Sly's cane in the right hand, held by its own shape.
//
// - The eyes: the character's centre plus its eye height (Sly 25, Bentley 10, Murray 45 game units), between the last
//   two steps as the scene is (vr::SetFirstPersonBody); vr.cpp turns the view by the head and the body's snap turns.
// - The hands: found once per model from its bind pose, a T-pose for Sly, Bentley and Murray (on each side, where the
//   arm ends; Sly's right hand is the record his cane hangs from); a skinned group without a hand bone is hidden, the
//   other bones of a group with one are closed up at their arm's end (no ghost of the body: linear skinning averages
//   collapsed points). The shadow groups stay as the game animates them (the whole body's shadow on the ground).
// - The hands' turn on the grips: each character's hand in its own T-pose matched to a grip held in a T-pose with the
//   palm down (a default until the T-pose calibration).
// - The cane: measured once per world from its vertices (the straight shaft, the hook end from the game's cane point
//   SLY +0x19A4), drawn with the shaft along the fist and the hook out above the thumb; the right hand closed on it as
//   the animation holds it. Its trail (world-space vertices) is hidden.
// Only this frame's draw is changed: ApplyHands after vr::FEyeViews, RestoreHands after the eyes (both renderers draw
// from the same SCENE). SLY2N_VR_FPLOG=1 prints the hands and the cane found.

#include <glm/glm.hpp>

#include <vector>

struct SCENE;
struct SW;
struct ALO;

namespace vrfp
{
// Each game step, after the step's scene snapshot (game.cpp): the player's place, for the eyes between steps.
void NoteStep(SW* psw);
// Each frame, before vr::FEyeViews: the eyes' place for this frame (u: the frame's fraction between the last two steps;
// fInterp false: the latest step's) given to vr::SetFirstPersonBody, or none (not a character).
void PrepareFrame(SW* psw, float u, bool fInterp);
// After vr::FEyeViews (when vr::FFirstPersonFrame), before the eyes are drawn: the player drawn as its hands.
void ApplyHands(SCENE& scene, const std::vector<ALO*>& apalo, SW* psw);
// After the eyes: the scene entries back as they were.
void RestoreHands(SCENE& scene);
}  // namespace vrfp
