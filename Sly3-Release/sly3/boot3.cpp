// Sly 3's boot queue (boot3.h).

#include "boot3.h"

#include "addr.h"

#include "../disc.h"

#include <algorithm>
#include <cstring>

std::function<int(const TRANSREQ3&)> g_pfnBootTransition3;

namespace
{
constexpr uint32_t VA_FK_MOVIE_LOGO_A = 0x317EB8;  // movie 338
constexpr uint32_t VA_FK_MOVIE_LOGO_B = 0x317EF8;  // movie 339
constexpr uint32_t VA_FK_EMPTY = 0x345820;         // the movies' transition (no world)
constexpr int POINT_BOOT = 0x1A8;

// a key record is a file when its +0x04 and +0x3C words differ (0x1C70D8 / 0x1C7130)
bool FKeyIsFile(uint32_t va)
{
    const uint8_t* pb = va ? g_disc.PbElf(va, 64) : nullptr;
    if (!pb) return false;
    uint32_t a = 0, b = 0;
    std::memcpy(&a, pb + 4, 4);
    std::memcpy(&b, pb + 0x3C, 4);
    return a != b;
}

// orig3: 0x25E070 (a world's key record by index)
uint32_t VaWorldKey(int iworld)
{
    for (const WORLD3& w : g_disc.Worlds())
        if (w.iworld == iworld) return w.vaFk;
    return 0;
}
}  // namespace

// orig3: 0x1C70D8
void QueueBootMovie3(BOOTQUEUE3& q, uint32_t vaKey)
{
    if (!FKeyIsFile(vaKey) || q.a.size() >= 16) return;
    BOOTACTION3 a;
    a.kind = 0;
    a.vaMovieKey = vaKey;
    q.a.push_back(a);
}

// orig3: 0x1C7130
void QueueBootWorld3(BOOTQUEUE3& q, uint32_t vaKey, int point, int iworld)
{
    if (!FKeyIsFile(vaKey) || q.a.size() >= 16) return;
    BOOTACTION3 a;
    a.kind = 2;
    a.vaWorldKey = vaKey;
    a.point = point;
    a.iworld = iworld;
    q.a.push_back(a);
}

// orig3: 0x1C71B0 (0x11C330() < 2: release builds always)
void QueueBootCard3(BOOTQUEUE3& q)
{
    if (q.a.size() >= 16) return;
    BOOTACTION3 a;
    a.kind = 5;
    q.a.push_back(a);
}

// orig3: 0x1C6EA8
void StartBootSequence3(BOOTQUEUE3& q, BOOTHOST3& host)
{
    q = BOOTQUEUE3{};
    QueueBootMovie3(q, VA_FK_MOVIE_LOGO_A);
    QueueBootMovie3(q, VA_FK_MOVIE_LOGO_B);
    QueueBootCard3(q);
    QueueBootWorld3(q, VaWorldKey(orig3::WORLD_PROLOGUE), POINT_BOOT, orig3::WORLD_PROLOGUE);
    FRunBootQueue3(q, host);
}

// orig3: 0x1C6F30
bool FRunBootQueue3(BOOTQUEUE3& q, BOOTHOST3& host)
{
    for (;;)
    {
        if (q.icur >= int(q.a.size())) return false;  // 0x1C6F20
        const BOOTACTION3 a = q.a[std::size_t(q.icur++)];
        switch (a.kind)
        {
        case 0: case 1:
        {
            host.QueueMovie(a.vaMovieKey, a.kind == 1);
            TRANSREQ3 req;
            req.vaKey = VA_FK_EMPTY;
            req.point = POINT_BOOT;
            req.iworld = 0;
            host.RequestTransition(req);
            return true;
        }
        case 2:
        {
            TRANSREQ3 req;
            req.vaKey = a.vaWorldKey;
            req.point = a.point;
            req.iworld = a.iworld;
            host.RequestTransition(req);
            return true;
        }
        case 3: host.RequestTaskStart(a.oidTask, a.nTask); return true;
        case 4: q.icur = std::max(q.icur - 2, 0); continue;  // (back: the previous action again)
        case 5:
            if (host.CardState() == 0) host.StartCardCheck();
            continue;  // (0x1C6F30 again at once)
        default: return true;
        }
    }
}
