#include "clock.h"
#include "savedata.h"

#include <algorithm>
#include <chrono>

CLOCK g_clock;
float g_dtFixedStep = 0.0f;

uint64_t TickNow()
{
    using HostClock = std::chrono::steady_clock;
    static const HostClock::time_point s_epoch = HostClock::now();
    const std::chrono::duration<double> elapsed = HostClock::now() - s_epoch;
    return static_cast<uint64_t>(elapsed.count() * 294912000.0);
}

namespace
{
// orig 0x0014D1F8: the factors' product
void UpdateClockRate(CLOCK* pclock)
{
    pclock->rDt = 1.0f;
    for (float g : pclock->af) pclock->rDt *= g;
    pclock->rDtReal = pclock->af[8];
}
}

// orig 0x0014D068 (the factors 0, 1, 2, 4, 5, 6, 7, 8 back to 1; factor 3 keeps its value)
void ResetClock(CLOCK* pclock)
{
    pclock->t = 0.0f;
    pclock->fAfterReset = true;
    for (int i : {0, 1, 2, 5, 6, 8, 7, 4}) pclock->af[i] = 1.0f;
    UpdateClockRate(pclock);
}

// orig 0x0014D1D0
void SetClockFactor(CLOCK* pclock, int i, float g)
{
    pclock->af[i] = g;
    UpdateClockRate(pclock);
}

// orig 0x0014D0B8 UpdateClock
void UpdateClock(CLOCK* pclock, bool fPaused)
{
    const uint64_t tick = TickNow();
    float dt = static_cast<float>(tick - pclock->tick) * 3.3908420e-9f;
    if (g_dtFixedStep > 0.0f) dt = g_dtFixedStep;
    dt = std::clamp(dt, 1.0f / 60.0f, 1.0f / 30.0f);
    // The original's load blocks between two updates, so the first frame of every world measures a long time and
    // takes the clamp's 1/30 s. Timing depends on it: arriving in the lighthouse's crawl (world 35), Sly reaches the
    // floor 0.09 s after the interaction starts, just inside the 0.1 s off-the-floor limit (0x18A8C8).
    if (pclock->fAfterReset) dt = 1.0f / 30.0f;
    pclock->fAfterReset = false;
    pclock->dtReal = dt * pclock->rDtReal;
    pclock->tReal += pclock->dtReal;
    const float dt2 = dt * pclock->af[3];
    pclock->dt40 = dt2;
    if (!fPaused)
    {
        pclock->dt = dt2 * pclock->rDt;
        pclock->t += pclock->dt;
        UpdateGameTimers(pclock->dt);   // orig 0x161478 (UpdateTasks: the play times)
    }
    pclock->tick = tick;
    pclock->t3C += pclock->dt40;
}
