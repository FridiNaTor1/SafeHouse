#pragma once

// The game clock (Sly 2's CLOCK at 0x2DDED0): t / dt of the game, the real time, and nine rate factors whose
// product scales the game's dt (0x14D1F8). UpdateClock 0x14D0B8 runs once per frame.

#include <cstdint>

struct CLOCK
{
    float t = 0.0f;         // +0x00 game time
    float dt = 0.0f;        // +0x04 game step
    float rDt = 1.0f;       // +0x08 the product of the nine factors
    float rDtReal = 1.0f;   // +0x0C factor 8 (the real clock's rate)
    float dtReal = 0.0f;    // +0x10
    float tReal = 0.0f;     // +0x14
    float af[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};  // +0x18: rate factors (af[3] also scales the frame step)
    float t3C = 0.0f;       // +0x3C
    float dt40 = 0.0f;      // +0x40 the frame step before the factors' product
    uint64_t tick = 0;      // +0x48 the host tick of the last update (294.912 MHz units)
};

extern CLOCK g_clock;
// The original's frame step: UpdateClock reads the EE counter (294.912 MHz), and the game is locked to the NTSC
// vblank (59.94 Hz), so every frame measures 1001/60000 s (0.0166833; the reference's clock gives that on ~90% of
// its frames, the rest is the emulator's jitter around it). Above the clamp's 1/60 floor, so it is used as is.
constexpr float DT_FRAME_NTSC = 1001.0f / 60000.0f;
// A fixed frame step (> 0): every UpdateClock advances by it (headless runs and the differential harness).
extern float g_dtFixedStep;

uint64_t TickNow();                          // the EE Count register's role: host time at 294.912 MHz
void ResetClock(CLOCK* pclock);              // orig 0x14D068
void SetClockFactor(CLOCK* pclock, int i, float g);  // orig 0x14D1D0
void UpdateClock(CLOCK* pclock, bool fPaused);       // orig 0x14D0B8
