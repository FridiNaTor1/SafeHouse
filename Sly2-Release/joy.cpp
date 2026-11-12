#include "joy.h"
#include "cheat.h"
#include "rumble.h"

#include "pcinput.h"
#include "actioninput.h"
#include "skipcut.h"
#include "debugmenu.h"

#include "clock.h"
#include "po.h"
#include "sw.h"
#include "task.h"

#include <GLFW/glfw3.h>


#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

JOY g_joy;
JOYPS2 g_joyps2;
JOYPS2 g_joyps2Null;
uint32_t g_grfjoyt = 7;
uint32_t g_grfjoyLock = 0;

// orig: 0x001870D0
void RecalcJoyt()
{
    const uint32_t l = g_grfjoyLock;
    if (l & 0x10) g_grfjoyt = 5;
    else if (l & 0xC) g_grfjoyt = 0;
    else if (l & 1) g_grfjoyt = 5;
    else if (l & 2) g_grfjoyt = 4;
    else g_grfjoyt = 7;
    if (!(g_grfjoyt & 2)) g_joyps2.grfbtnHandled = 0xFFFF;  // (0x2E0CBC: the first pad's +0x10BC)
    if (std::getenv("SLY2N_JOYLOCKPRINT")) std::fprintf(stderr, "joy: lock 0x%X grfjoyt %u (t %.3f)\n", l, g_grfjoyt, g_clock.t);
}

// orig: 0x00187078
void LockJoy(uint32_t grf)
{
    g_grfjoyLock |= grf;
    RecalcJoyt();
}

// orig: 0x001870A0
void UnlockJoy(uint32_t grf)
{
    g_grfjoyLock &= ~grf;
    RecalcJoyt();
}

// orig: 0x00187E08
void ClearJoyBtns(JOYPS2* pjoy, uint32_t grf)
{
    pjoy->grfbtn &= ~grf;
    pjoy->grfbtnHandled &= ~grf;
}

namespace
{
struct BTNNAME { const char* name; uint16_t btn; };
const BTNNAME s_abtnname[] = {
    {"SELECT", BTN_Select}, {"L3", BTN_L3}, {"R3", BTN_R3}, {"START", BTN_Start}, {"UP", BTN_Up},
    {"RIGHT", BTN_Right}, {"DOWN", BTN_Down}, {"LEFT", BTN_Left}, {"L2", BTN_L2}, {"R2", BTN_R2},
    {"L1", BTN_L1}, {"R1", BTN_R1}, {"TRIANGLE", BTN_Triangle}, {"CIRCLE", BTN_Circle}, {"CROSS", BTN_Cross},
    {"SQUARE", BTN_Square},
};

}

bool FParseJoyPresses(const std::string& s, std::vector<JOYPRESS>& apress)
{
    size_t i = 0;
    while (i < s.size())
    {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string item = s.substr(i, j - i);
        const size_t at = item.find('@');
        if (at == std::string::npos) return false;
        const std::string name = item.substr(0, at);
        JOYPRESS press{0, std::atoi(item.c_str() + at + 1), 1};
        const size_t plus = item.find('+', at);
        if (plus != std::string::npos) press.c = std::atoi(item.c_str() + plus + 1);
        const size_t eq = name.find('=');
        if (eq != std::string::npos)
        {
            static const char* s_aszAxis[4] = {"LX", "LY", "RX", "RY"};
            for (int k = 0; k < 4; ++k)
                if (name.substr(0, eq) == s_aszAxis[k]) press.axis = k;
            const long v = std::strtol(name.c_str() + eq + 1, nullptr, 0);
            if (press.axis < 0 || v < 0 || v > 255) return false;
            press.bAxis = static_cast<uint8_t>(v);
        }
        else
        {
            for (const BTNNAME& b : s_abtnname)
                if (name == b.name) press.grfbtn = b.btn;
            if (!press.grfbtn) return false;
        }
        apress.push_back(press);
        i = j + 1;
    }
    return true;
}

// orig 0x001871C0 UpdateJoy (the pad driver's state into JOY: held buttons, edges, analog sticks)
int g_frameJoy = 0;  // (debug: the last UpdateJoy frame, for traces)
uint16_t g_grfJoyWindow = 0;   // the window's own input this step (skipcut.h: scripted presses never skip)
void UpdateJoy(JOY* pjoy, GLFWwindow* pwindow, int frame, const std::vector<JOYPRESS>& apress)
{
    g_frameJoy = frame;
    g_grfJoyWindow = 0;
    uint16_t grf = 0;
    float xl = 0, yl = 0, xr = 0, yr = 0;
    uint8_t abAxis[4] = {0x80, 0x80, 0x80, 0x80};  // LX LY RX RY
    bool fScripted = false;
    for (const JOYPRESS& p : apress)
        if (frame >= p.frame && frame < p.frame + p.c)
        {
            fScripted = true;
            if (p.axis < 0) grf |= p.grfbtn;
            else abAxis[p.axis] = p.bAxis;
        }
    {
        extern uint16_t g_grfAutoplay;   // (debug: SLY2N_AUTOPLAY, autoplay.cpp)
        if (g_grfAutoplay) { fScripted = true; grf |= g_grfAutoplay; }
    }
    {
        // (debug: SLY2N_ROUTE, route.cpp: a waypoint program's sample replaces the presses while it runs)
        bool FRouteSample(int frame, uint16_t* pgrf, uint8_t abAxis[4]);
        uint16_t grfR = 0;
        uint8_t abR[4];
        if (FRouteSample(frame, &grfR, abR))
        {
            fScripted = true;
            grf = grfR;
            for (int i = 0; i < 4; ++i) abAxis[i] = abR[i];
        }
    }
    pjoy->fConnected = false;
    if (pwindow)
    {
        // the gamepad (its bindings), the keyboard and mouse (theirs), Escape / Enter, the camera keys (pcinput.h)
        uint16_t grfPc = 0;
        bool fPad = false;
        ReadPcInput(pwindow, &grfPc, &xl, &yl, &xr, &yr, &fPad);
        grf |= grfPc;
        g_grfJoyWindow = grfPc;
        pjoy->fConnected = fPad;
    }
    sly2actions::ScriptedFrame(fScripted);
    grf &= uint16_t(~GrfSkipCutMask());   // (skipcut.h: the Circle that skipped a cutscene, until released)
    // (debugmenu.h, Debug keys on only: the debug menu's keys, and the whole pad while it is open, are not the game's)
    if (const uint16_t grfGame = GrfDebugMenuFilter(grf, &xl, &yl, &xr, &yr, abAxis); grfGame != grf)
    {
        grf = grfGame;
        g_grfJoyWindow &= grfGame;
    }
    // the raw read the game's pad gets (scePadRead: bytes 2 / 3 the buttons active low, 4..7 RX RY LX LY)
    {
        auto byteOf = [](float g, bool fInvert) {
            const float u = fInvert ? (1.0f - g) * 0.5f : (g + 1.0f) * 0.5f;
            return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(u, 0.0f), 1.0f) * 255.0f));
        };
        uint8_t ab[0x20]{};
        ab[1] = 0x73;
        ab[2] = static_cast<uint8_t>(~(grf & 0xFF));
        ab[3] = static_cast<uint8_t>(~(grf >> 8));
        if (fScripted)
        {
            ab[4] = abAxis[2];
            ab[5] = abAxis[3];
            ab[6] = abAxis[0];
            ab[7] = abAxis[1];
        }
        else
        {
            ab[4] = byteOf(xr, false);
            ab[5] = byteOf(yr, true);
            ab[6] = byteOf(xl, false);
            ab[7] = byteOf(yl, true);
        }
        ReadJoyPs2(&g_joyps2, ab);
        if (fScripted)
        {
            auto gOf = [](uint8_t b, bool fInvert) { const float g = (b * 2) / 255.0f - 1.0f; return fInvert ? -g : g; };
            xl = gOf(abAxis[0], false);
            yl = gOf(abAxis[1], true);
            xr = gOf(abAxis[2], false);
            yr = gOf(abAxis[3], true);
        }
    }
    pjoy->grfbtnPressed = grf & ~pjoy->grfbtn;
    pjoy->grfbtnReleased = pjoy->grfbtn & ~grf;
    pjoy->grfbtn = grf;
    pjoy->xLeft = xl;
    pjoy->yLeft = yl;
    pjoy->xRight = xr;
    pjoy->yRight = yr;
    // (0x1871C0 state 3, after FReadJoy: the rumble's step, with JOY +0x10E8 fRumbleEnabled always on)
    if (g_joyps2.joys == 3 && g_joyps2.joyk >= 3) UpdateRumble(&g_rumble);
}

namespace
{
// orig: 0x00187768 GetJoyXYDeflection (a stick's bytes to its direction scaled by its deflection: the four axes of
// 0x2BE950 each normalized by the largest deflection seen along it (the limits grow), less 0.35, doubled, in 0..1;
// the stick counts once it moved off its first reading or out of the centre)
void GetJoyXYDeflection(const JOYPS2* pjoy, uint8_t bx, uint8_t by, float* px, float* py, float* pu, uint8_t* pbxPrev,
                        uint8_t* pbyPrev, int* pfMoved, JOYLM* alm)
{
    static const float s_aaxis[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0x1.666666p-1f, 0x1.666666p-1f}, {0x1.666666p-1f, -0x1.666666p-1f}};
    if (!*pfMoved)
    {
        int f = 0;
        if (pjoy->tRead != 0.0f)
        {
            if (bx != *pbxPrev || by != *pbyPrev) f = 1;
            else if (bx < 0x60 || bx >= 0xA1 || by < 0x60 || by >= 0xA1) f = 1;
        }
        *pfMoved = f;
    }
    float x = 0.0f, y = 0.0f;
    if (*pfMoved)
    {
        const float r = 0x1.010102p-8f;  // 0x3B808081 (1 / 255)
        x = (static_cast<float>(bx) + static_cast<float>(bx)) * r + -1.0f;
        y = 1.0f - (static_cast<float>(by) + static_cast<float>(by)) * r;
    }
    float gMax = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        const float d = x * s_aaxis[i][0] + y * s_aaxis[i][1];
        float den;
        if (0.0f <= d)
        {
            if (alm[i].gMax < d) alm[i].gMax = d;
            den = alm[i].gMax;
        }
        else
        {
            if (d < alm[i].gMin) alm[i].gMin = d;
            den = alm[i].gMin;
        }
        const float r = d / den;
        if (gMax < r) gMax = r;
    }
    const float g = (gMax - 0x1.666666p-2f) + (gMax - 0x1.666666p-2f);  // (0x3EB33333 0.35)
    float u = 0.0f;
    if (!(g < 0.0f)) u = 1.0f < g ? 1.0f : g;
    *pu = u;
    const float l = std::sqrt(x * x + y * y);
    if (0x1.a36e2ep-14f < l)  // (0x38D1B717 1e-4)
    {
        *px = (u * x) / l;
        *py = (u * y) / l;
    }
    else *px = *py = 0.0f;
    *pbxPrev = bx;
    *pbyPrev = by;
}
}  // namespace

// orig: 0x00187998 FReadJoy (the read part; the pad's state machine 0x1871C0 is always ready natively, the cheat
// codes 0x14BF48 (cheat.h) and the idle broadcast 0x1619A0)
void ReadJoyPs2(JOYPS2* pjoy, const uint8_t* abRaw)
{
    std::memcpy(pjoy->ab, abRaw, sizeof pjoy->ab);
    const uint32_t grfbtn = ((static_cast<uint32_t>(abRaw[2]) << 8) | abRaw[3]) ^ 0xFFFF;
    const uint32_t grfbtnOld = pjoy->grfbtn;
    pjoy->grfbtnPressed = (grfbtn & ~grfbtnOld) & ~pjoy->grfbtnHandled;
    pjoy->grfbtnHandled &= grfbtn;
    pjoy->grfbtn = grfbtn;
    pjoy->term = abRaw[1];
    const float x = pjoy->x, y = pjoy->y, x2 = pjoy->x2, y2 = pjoy->y2;
    GetJoyXYDeflection(pjoy, abRaw[6], abRaw[7], &pjoy->x, &pjoy->y, &pjoy->uDeflect, &pjoy->bxPrev, &pjoy->byPrev,
                       &pjoy->fStickMoved, pjoy->almDeflect);
    GetJoyXYDeflection(pjoy, abRaw[4], abRaw[5], &pjoy->x2, &pjoy->y2, &pjoy->uDeflect2, &pjoy->bx2Prev, &pjoy->by2Prev,
                       &pjoy->fStickMoved2, pjoy->almDeflect2);
    // the d-pad moves the left stick (TODO(camera): unless the camera's stack top is +0x3A0 with an entry)
    if (pjoy->grfbtn & 0xF000)
    {
        pjoy->x = pjoy->y = pjoy->uDeflect = 0.0f;
        if (pjoy->grfbtn & 0x8000) pjoy->x = -pjoy->gDpad;
        else if (pjoy->grfbtn & 0x2000) pjoy->x = pjoy->gDpad;
        if (pjoy->grfbtn & 0x1000) pjoy->y = pjoy->gDpad;
        else if (pjoy->grfbtn & 0x4000) pjoy->y = -pjoy->gDpad;
        if (!(pjoy->x * pjoy->y == 0.0f))
        {
            pjoy->x *= std::sqrt(0.5f);
            pjoy->y *= std::sqrt(0.5f);
        }
        pjoy->uDeflect = 1.0f;
    }
    const float dt = g_clock.dt40;
    pjoy->dx = (pjoy->x - x) / dt;
    pjoy->dy = (pjoy->y - y) / dt;
    pjoy->dx2 = (pjoy->x2 - x2) / dt;
    pjoy->dy2 = (pjoy->y2 - y2) / dt;
    if (pjoy == &g_joyps2) MatchCheatCodes(grfbtnOld, pjoy->grfbtn);   // 0x14BF48 (the cheat codes, cheat.h)
    pjoy->tRead = g_clock.t3C;
    if (pjoy->grfbtn != 0 || !(pjoy->uDeflect == 0.0f) || !(pjoy->uDeflect2 == 0.0f))
    {
        pjoy->tActive = g_clock.t3C;
        pjoy->dtIdle = 0;
        // after 15 s the task event 0xE with the current player (0x1C4840 / 0x1619A0 on 0x2DEB30)
        if (pjoy == &g_joyps2 && 15.0f < g_clock.t3C - g_taskmgr.tIdleEvent)
        {
            if (g_psw) BroadcastTaskEvent(g_psw, 0xE, PloPoCurrent(g_psw));
            g_taskmgr.tIdleEvent = g_clock.t3C;
        }
    }
    else pjoy->dtIdle = static_cast<int>(g_clock.t3C - pjoy->tActive);
}

// orig: 0x00187DE0 SetJoyBtnHandled
void SetJoyPs2BtnHandled(JOYPS2* pjoy, uint32_t grfbtn)
{
    pjoy->grfbtnPressed &= ~grfbtn;
    pjoy->grfbtnHandled |= pjoy->grfbtn & grfbtn;
}

// The pad's state from a reference RAM (the JOY at 0x2DFC00), for the harnesses' take-over: the sticks' last read,
// their moved flags and learned limits (else the first native reads see the sticks at rest: Sly's run ramps).
void ImportJoyPs2FromRam(JOYPS2* pjoy, const std::vector<uint8_t>& ram)
{
    constexpr uint32_t A = 0x2DFC00;
    auto rd = [&](uint32_t off, void* pv, size_t cb) { if (A + off + cb <= ram.size()) std::memcpy(pv, &ram[A + off], cb); };
    rd(0x1018, &pjoy->tRead, 4);
    rd(0x101C, &pjoy->tActive, 4);
    rd(0x1020, &pjoy->dtIdle, 4);
    rd(0x103C, &pjoy->x, 4); rd(0x1040, &pjoy->y, 4); rd(0x1044, &pjoy->uDeflect, 4);
    rd(0x1048, &pjoy->bxPrev, 1); rd(0x1049, &pjoy->byPrev, 1);
    rd(0x104C, &pjoy->fStickMoved, 4);
    rd(0x1050, pjoy->almDeflect, sizeof pjoy->almDeflect);
    rd(0x1070, &pjoy->x2, 4); rd(0x1074, &pjoy->y2, 4); rd(0x1078, &pjoy->uDeflect2, 4);
    rd(0x107C, &pjoy->bx2Prev, 1); rd(0x107D, &pjoy->by2Prev, 1);
    rd(0x1080, &pjoy->fStickMoved2, 4);
    rd(0x1084, pjoy->almDeflect2, sizeof pjoy->almDeflect2);
    rd(0x10B4, &pjoy->grfbtn, 4);
    rd(0x10BC, &pjoy->grfbtnHandled, 4);
}
