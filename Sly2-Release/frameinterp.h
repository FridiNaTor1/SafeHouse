#pragma once

// Display rates above 60 Hz (native, no original): the game keeps the original's NTSC steps (UpdateClock's dt is
// exactly DT_FRAME_NTSC = 1001/60000 s per step, 59.94 steps a second, input read once per step), a real-time accumulator decides how many steps each displayed
// frame runs (at most CSTEP_MAX; a longer hitch is dropped, not caught up), and each displayed frame draws the
// state between the last two steps: the camera, the scene's group matrices and the skinned groups' bone matrices
// are interpolated; everything else (particles, the HUD, RIPs, the markers' extra draws) shows the latest step.
// At 60 Hz and below the loop is unchanged (one step a frame, no interpolation).

#include "render.h"

struct SCENE;

constexpr int CSTEP_MAX = 4;
constexpr double DT_STEP = 1001.0 / 60000.0;   // = DT_FRAME_NTSC (clock.h)

// The accumulator. FActive: the display runs faster than the steps (the frame-rate cap above 60), or forced by
// SLY2N_FRAMEINTERP=fps (a virtual display at that rate: each displayed frame is 1/fps of real time, for headless
// checks). Steps: how many steps this displayed frame; U: where it draws between the last two (0..1).
struct FRAMEACCUM
{
    bool fActive = false;
    double fpsVirtual = 0.0;    // > 0: the virtual display rate
    double tLast = -1.0;        // the real time of the last displayed frame (s)
    double tAcc = 0.0;          // the real time not stepped yet (s)
    int Steps(double tNow);     // advances by the real time since the last call
    float U() const { return float(tAcc / DT_STEP); }
};

// After each step (the scene placed from it): the placements and the camera it gives.
void SnapshotStep(const SCENE& scene, const CAMERA& cam);
// A world change or a cut: the next step starts a new pair.
void ResetStepSnapshots();
// The displayed frame's state at u between the last two steps: the scene's matrices and the camera (an entry, a
// matrix or the camera that jumped too far between them, or has no earlier state, shows the latest).
void InterpolateSteps(SCENE& scene, CAMERA& cam, float u);
