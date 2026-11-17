#include "clock.h"
#include "savedata.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

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
// orig 0x0014D1F8: the factors' product (orig3 0x165570: ten factors)
void UpdateClockRate(CLOCK* pclock)
{
    pclock->rDt = 1.0f;
    for (float g : pclock->af) pclock->rDt *= g;
    pclock->rDtReal = pclock->af[8];
}
}

// orig 0x0014D068 (the factors 0, 1, 2, 4, 5, 6, 7, 8 back to 1; factor 3 keeps its value) (orig3: 0x1653E0, the same C in the Sly 3 decomp)
void ResetClock(CLOCK* pclock)
{
    pclock->t = 0.0f;
    for (int i : {0, 1, 2, 5, 6, 8, 7, 4}) pclock->af[i] = 1.0f;
    UpdateClockRate(pclock);
}

// orig 0x0014D1D0 (orig3: 0x165548, the same C in the Sly 3 decomp)
void SetClockFactor(CLOCK* pclock, int i, float g)
{
    if (static const bool s_f = std::getenv("SLY3_CLOCKPRINT") != nullptr; s_f && pclock->af[i] != g)
        std::printf("clock: factor %d %.3f -> %.3f t %.3f\n", i, pclock->af[i], g, pclock->t);
    pclock->af[i] = g;
    UpdateClockRate(pclock);
}

// orig 0x0014D0B8 UpdateClock (orig3: 0x165430, the same C in the Sly 3 decomp)
void UpdateClock(CLOCK* pclock, bool fPaused)
{
    const uint64_t tick = TickNow();
    float dt = static_cast<float>(tick - pclock->tick) * 3.3908420e-9f;
    if (g_dtFixedStep > 0.0f) dt = g_dtFixedStep;
    dt = std::clamp(dt, 1.0f / 60.0f, 1.0f / 30.0f);
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
    if (static const bool s_f = std::getenv("SLY3_CLOCKPRINT") != nullptr; s_f)   // (debug)
        std::printf("clock: t %.6f dt %.8f rDt %.6f af6 %.6f\n", pclock->t, pclock->dt, pclock->rDt, pclock->af[6]);
}
