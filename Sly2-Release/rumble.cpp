// The pad's vibration: rumble.h.
#include <algorithm>
#if defined(__ANDROID__) || defined(CANE_IOS)
#include "mobile_app.h"
#endif
#include "rumble.h"
#include "vr.h"

#include "clock.h"
#include "joy.h"
#include "ref.h"
#include "savedata.h"

#include <GLFW/glfw3.h>
#include "pad_rumble.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern int g_frameJoy;

namespace
{
// orig 0x2C2260: the 11 retail patterns {crumins, {small motor, big motor, dt} x 32} (from SCUS_973.16; the same in
// every region's ELF of this layout). Each step holds its motors for dt; a pattern loops until the rumble's time ends.
constexpr RUMPAT s_arumpat[RUMK_Max] = {
    {1, {{1, 0, 0.05f}}},
    {2, {{0, 150, 0.05f}, {0, 0, 0.05f}}},
    {2, {{0, 210, 0.05f}, {0, 0, 0.1f}}},
    {2, {{0, 255, 0.1f}, {0, 0, 0.1f}}},
    {7, {{0, 210, 0.1f}, {0, 190, 0.01f}, {0, 170, 0.01f}, {0, 150, 0.01f}, {0, 130, 0.01f}, {0, 110, 0.01f}, {0, 0, 0.25f}}},
    {2, {{1, 255, 0.1f}, {1, 0, 0.05f}}},
    {2, {{0, 170, 0.05f}, {0, 0, 0.1f}}},
    {2, {{0, 170, 0.05f}, {0, 0, 0.1f}}},
    {2, {{0, 170, 0.05f}, {0, 0, 0.1f}}},
    {3, {{0, 160, 0.15f}, {0, 0, 0.1f}, {0, 90, 0.05f}}},
    {5, {{0, 255, 0.2f}, {0, 0, 0.08f}, {0, 200, 0.13f}, {0, 0, 0.06f}, {0, 150, 0.11f}}},
};

bool FLog()
{
    static const bool s_f = [] { const char* psz = std::getenv("SLY2N_RUMBLELOG"); return psz && *psz && *psz != '0'; }();
    return s_f;
}

// (--rumble-selftest) the actuator writes, captured instead of sent
std::vector<std::pair<int, std::pair<int, int>>>* s_pawrite = nullptr;

// the platform side
int s_jid = -1;
bool s_fWindow = false;
#ifdef CANE_SDL_RUMBLE
cane::rumble::SdlRumble s_sdl;
bool s_fSdlTried = false;
#endif

#ifdef __SWITCH__
}  // namespace
void CaneSwitchRumble(float low, float high);   // platform/switch/glfw_switch.cpp (HD rumble)
namespace
{
#endif
void SendMotors(uint8_t bSmall, uint8_t bBig)
{
    // (VR, vr.h: the headset's controllers are the pad (pcinput: no joystick chosen): both hands vibrate, the big motor as
    // the strength, the small one (on / off, a light buzz on the DualShock) as a light one; Controller Vibration Off: none)
    if (vr::FActive() && s_fWindow && s_jid < 0)
    {
        if (!g_fControllerVibration) bSmall = bBig = 0;
        const float g = std::max(float(bBig) / 255.0f, bSmall ? 0.3f : 0.0f);
        vr::Rumble(g, g);
        return;
    }
#if defined(__SWITCH__)
    // the DualShock's motors on the Joy-Cons / Pro Controller: the big one (0..255) as the low band's amplitude, the
    // small one (on / off) as the high band at full
    if (!s_fWindow || s_jid < 0) return;
    if (!g_fControllerVibration) bSmall = bBig = 0;
    CaneSwitchRumble(float(bBig) / 255.0f, bSmall ? 1.0f : 0.0f);
#elif defined(CANE_SDL_RUMBLE)
    if (!s_fWindow || s_jid < 0) return;   // (headless, the keyboard and mouse: nothing to drive)
#if defined(__ANDROID__) || defined(CANE_IOS)
    if (s_jid == GLFW_JOYSTICK_LAST)
    {
        // the on-screen pad: the device's own vibrator, the big motor as its strength (the small one: a light buzz)
        if (!g_fControllerVibration) bSmall = bBig = 0;
        cane::mobile::vibrate(std::max<int>(bBig, bSmall ? 96 : 0));
        return;
    }
#endif
    if (!s_sdl.ready()) return;   // (started by SetRumblePad at the first windowed frame)
    if (!g_fControllerVibration) bSmall = bBig = 0;
    s_sdl.set(glfwGetJoystickGUID(s_jid), cane::rumble::MotorsFromDualShock(bSmall, bBig), glfwGetTime());
#else
    (void)bSmall;
    (void)bBig;
#endif
}

// orig 0x243870 scePadSetActDirect(port, slot, ab): ab[0] the small motor, ab[1] the big one (the align {0, 1} set by
// UpdateJoy's 0x243930 when the DualShock 2 came up)
void SetActDirect(RUMBLE* prumble, uint8_t bSmall, uint8_t bBig)
{
    (void)prumble;
    if (s_pawrite)
    {
        s_pawrite->push_back({g_frameJoy, {bSmall, bBig}});
        return;
    }
    if (FLog())
        std::fprintf(stderr, "rumble: frame %d tReal %.4f act small %d big %d\n", g_frameJoy, g_clock.tReal, bSmall, bBig);
    SendMotors(bSmall, bBig);
}
}  // namespace

// orig 0x2E0CCC (the JOY's RUMBLE): the port's pad is a ready DualShock 2 from the start, so InitRumble's Dead -> Idle
// (SetJoyJoys 0x1875B8 on joyk reaching 3) is its initial state
RUMBLE g_rumble{0, 0, RUMS_Idle};
// (debug: SLY2N_VIBRATION=n starts the option at n, e.g. 2 (on) for the runs that start in a world without the title)
int g_vibrationSetting = [] { const char* psz = std::getenv("SLY2N_VIBRATION"); return psz ? std::atoi(psz) : 0; }();
bool g_fControllerVibration = true;

const RUMPAT& RumpatRetail(int rumk) { return s_arumpat[rumk]; }

// orig 0x001D5150
void InitRumble(RUMBLE* prumble, int nPort, int nSlot)
{
    SetRumbleRums(prumble, prumble->rums == RUMS_Dead ? RUMS_Idle : RUMS_Stop);
    prumble->nPort = nPort;
    prumble->nSlot = nSlot;
}

// orig 0x001D51B8 (once per frame from UpdateJoy, by the clock's raw frame step +0x40). One step per frame at most,
// as the original: a long frame does not skip steps.
void UpdateRumble(RUMBLE* prumble)
{
    const float dtReal = g_clock.dt40;
    switch (prumble->rums)
    {
    case RUMS_Rumble:
        if (prumble->dtRumble > 0.0f)
        {
            prumble->dtRumins -= dtReal;
            if (prumble->dtRumins <= 0.0f)
            {
                if (++prumble->irumins >= prumble->prumpat->crumins) prumble->irumins = 0;
                const RUMINS& ins = prumble->prumpat->arumins[prumble->irumins];
                SetActDirect(prumble, ins.fHighSpeedMotor, ins.bLowSpeedMotor);
                prumble->dtRumins = ins.dt;
            }
            prumble->dtRumble -= dtReal;
        }
        else
            SetRumbleRums(prumble, RUMS_Stop);
        break;
    case RUMS_Stop:
    case RUMS_Kill:
        // the motors written off every frame for four frames (SetRumbleRums' dtRumins), then idle / dead
        if (prumble->dtRumins > 0.0f)
        {
            StopRumbleActuators(prumble);
            prumble->dtRumins -= dtReal;
        }
        else
            SetRumbleRums(prumble, prumble->rums == RUMS_Stop ? RUMS_Idle : RUMS_Dead);
        break;
    default: break;
    }
}

// orig 0x001D5310
void TriggerRumbleRumk(RUMBLE* prumble, int rumk, float dt)
{
    // (the original indexes the table unchecked; a bad kind from data is dropped here)
    if (rumk < 0 || rumk >= RUMK_Max) return;
    TriggerRumbleRumpat(prumble, &s_arumpat[rumk], dt);
}

// orig 0x001D5340 (rums -1 first, so a rumble already running starts over at its first step)
void TriggerRumbleRumpat(RUMBLE* prumble, const RUMPAT* prumpat, float dt)
{
    if (prumble->rums == RUMS_Dead || prumpat->crumins <= 0 || !FVibrationEnabled()) return;
    prumble->dtRumble = dt;
    prumble->prumpat = prumpat;
    prumble->irumins = 0;
    prumble->dtRumins = prumpat->arumins[0].dt;
    prumble->rums = RUMS_Nil;
    SetRumbleRums(prumble, RUMS_Rumble);
}

// orig 0x001D53C0
void SetRumbleRums(RUMBLE* prumble, RUMS rums)
{
    if (rums == prumble->rums) return;
    switch (rums)
    {
    case RUMS_Rumble:
        SetActDirect(prumble, prumble->prumpat->arumins[0].fHighSpeedMotor, prumble->prumpat->arumins[0].bLowSpeedMotor);
        prumble->rums = rums;
        break;
    case RUMS_Stop:
    case RUMS_Kill:
        StopRumbleActuators(prumble);
        prumble->dtRumins = g_clock.dt40 * 4.0f;   // (D_002DDF10: the clock +0x40)
        prumble->rums = rums;
        break;
    default:
        prumble->rums = rums;
        break;
    }
}

// orig 0x001D5468
void StopRumbleActuators(RUMBLE* prumble) { SetActDirect(prumble, 0, 0); }

// orig 0x001D5498 (RumbleUnknown1: from the title's show 0x1DD820)
void EnableVibration()
{
    if (g_vibrationSetting == 0) g_vibrationSetting = 1;
}

// orig 0x001D54B8: the menu's choice (2 on / 3 off) written into the game record's "fNoVibration"
void ApplyVibrationSetting()
{
    CRef r;
    switch (g_vibrationSetting)
    {
    case 2: r.SetBool(0); FSetProgress({"flags", "fNoVibration"}, r); break;
    case 3: r.SetBool(1); FSetProgress({"flags", "fNoVibration"}, r); break;
    default: break;
    }
}

// orig 0x001D55B8
bool FVibrationEnabled()
{
    switch (g_vibrationSetting)
    {
    case 2: return true;
    case 0:
    case 3: return false;
    default:
    {
        const CRef r = RefGetProgress({"flags", "fNoVibration"});
        return (r.m_n ^ 1) != 0;   // (the tag's word: an unset flag reads 0, on)
    }
    }
}

// orig 0x001AFB88 cases 72 / 73
void SetVibrationOption(bool fOff)
{
    g_vibrationSetting = fOff ? 3 : 2;
    CRef r;
    r.SetBool(fOff ? 1 : 0);
    FSetProgress({"flags", "fNoVibration"}, r);
}

// orig 0x00187E28 (JOY +0x10E8 fRumbleEnabled: always 1, set by InitJoy 0x187150; +0x1010 joyk >= 3 a DualShock;
// +0x100C joys 3 ready)
void TriggerJoyRumbleRumk(int rumk, float dt, const char* psz)
{
    if (FLog())
        std::fprintf(stderr, "rumble: frame %d tReal %.4f trigger rumk %d dt %.3f (%s)%s\n", g_frameJoy, g_clock.tReal, rumk, dt,
                     psz ? psz : "?", FVibrationEnabled() ? "" : " [vibration option off]");
    if (g_joyps2.joyk >= 3 && g_joyps2.joys == 3) TriggerRumbleRumk(&g_rumble, rumk, dt);
}

void StopJoyRumble() { SetRumbleRums(&g_rumble, RUMS_Stop); }

void SetRumblePad(int jid, bool fWindow)
{
#ifdef CANE_SDL_RUMBLE
    // SDL's controller subsystem at the first windowed frame (the boot), not at the first vibration: its start takes
    // about half a second (R21: the stutter the first time a vibration happened)
    if (fWindow && !s_fSdlTried)
    {
        s_fSdlTried = true;
        s_sdl.startup();
    }
    // another pad (or none) in use: the old one's motors off; the new one opened (and warmed) now
    if (jid != s_jid && s_sdl.ready())
    {
        s_sdl.stopAll();
        if (jid >= 0 && fWindow) s_sdl.prepare(glfwGetJoystickGUID(jid), glfwGetTime());
    }
#endif
    s_jid = jid;
    s_fWindow = fWindow;
}

void ShutdownPadRumble()
{
#ifdef CANE_SDL_RUMBLE
    s_sdl.shutdown();
#endif
}

// ---- --rumble-selftest
int RumbleSelfTestCommand()
{
    int failed = 0, checks = 0;
    auto check = [&](bool f, const std::string& what) {
        ++checks;
        if (!f)
        {
            ++failed;
            std::printf("FAIL %s\n", what.c_str());
        }
    };
    // the mapping (platform/pad_rumble.h): big motor -> SDL low frequency, small motor on -> high frequency full
    using cane::rumble::MotorsFromDualShock;
    check(MotorsFromDualShock(0, 0).low == 0 && MotorsFromDualShock(0, 0).high == 0, "motors off");
    check(MotorsFromDualShock(1, 0).high == 0xFFFF && MotorsFromDualShock(1, 0).low == 0, "small motor -> high frequency");
    check(MotorsFromDualShock(0, 255).low == 0xFFFF && MotorsFromDualShock(0, 255).high == 0, "big motor 255 -> low frequency full");
    for (int b = 0; b < 256; ++b)
        check(MotorsFromDualShock(0, uint8_t(b)).low == b * 257, "big motor " + std::to_string(b) + " linear");
    check(MotorsFromDualShock(7, 150).high == 0xFFFF && MotorsFromDualShock(7, 150).low == 150 * 257, "any non-zero small byte is on");
    // the table (0x2C2260)
    int cins = 0;
    for (const RUMPAT& p : s_arumpat)
    {
        check(p.crumins > 0 && p.crumins <= 32, "pattern size");
        cins += p.crumins;
    }
    check(cins == 30, "the retail table's 30 steps");
    check(s_arumpat[3].arumins[0].bLowSpeedMotor == 255 && s_arumpat[3].arumins[0].dt == 0.1f, "rumk 3's first step");
    check(s_arumpat[5].arumins[0].fHighSpeedMotor == 1 && s_arumpat[5].arumins[1].fHighSpeedMotor == 1, "rumk 5 keeps the small motor on");

    // the scheduler at the NTSC frame step: rumk 3 for 0.15 s (a guard struck by Murray)
    std::vector<std::pair<int, std::pair<int, int>>> aw;
    s_pawrite = &aw;
    const int vibrationSaved = g_vibrationSetting;
    const RUMBLE rumbleSaved = g_rumble;
    const CLOCK clockSaved = g_clock;
    const int frameSaved = g_frameJoy;
    g_clock.dt40 = DT_FRAME_NTSC;
    auto run = [&](int rumk, float dt, int cframe) {
        aw.clear();
        g_rumble = RUMBLE{0, 0, RUMS_Idle};
        g_frameJoy = 0;
        TriggerJoyRumbleRumk(rumk, dt, "selftest");
        for (g_frameJoy = 1; g_frameJoy <= cframe; ++g_frameJoy) UpdateRumble(&g_rumble);
    };
    auto str = [&]() {
        std::string s;
        for (auto& w : aw) s += std::to_string(w.first) + ":" + std::to_string(w.second.first) + "/" + std::to_string(w.second.second) + " ";
        return s;
    };
    g_vibrationSetting = 0;
    run(3, 0.15f, 30);
    check(aw.empty(), "no rumble before the title enables vibration (setting 0)");
    g_vibrationSetting = 3;
    run(3, 0.15f, 30);
    check(aw.empty(), "no rumble with the in-game option off");
    g_vibrationSetting = 2;
    run(3, 0.15f, 30);
    // frame 0: 255 (the trigger); 0.1 s later (frame 6) the off step; dtRumble 0.15 is spent after frame 9, so frame 10
    // stops (one write) and the four frames of dtRumins = dtReal * 4 write the motors off again (11..14), then idle
    const std::string s3 = str();
    std::printf("rumk 3 0.15 s: %s\n", s3.c_str());
    check(s3 == "0:0/255 6:0/0 10:0/0 11:0/0 12:0/0 13:0/0 14:0/0 ", "rumk 3 timing");
    check(g_rumble.rums == RUMS_Idle, "idle after the stop");
    // rumk 4 for 0.5 s: the ramp steps (0.01 s) advance one per frame, as the original's one step per UpdateRumble
    run(4, 0.5f, 40);
    const std::string s4 = str();
    std::printf("rumk 4 0.5 s: %s\n", s4.c_str());
    check(s4 == "0:0/210 6:0/190 7:0/170 8:0/150 9:0/130 10:0/110 11:0/0 26:0/210 31:0/0 32:0/0 33:0/0 34:0/0 35:0/0 ", "rumk 4 ramp timing");
    // a new trigger while one runs starts the new pattern over at its first step
    run(10, 2.0f, 5);
    TriggerJoyRumbleRumk(0, 0.05f, "selftest");
    check(aw.size() >= 2 && aw.back().second == std::make_pair(1, 0), "retrigger restarts at the first step");

    s_pawrite = nullptr;
    g_vibrationSetting = vibrationSaved;
    g_rumble = rumbleSaved;
    g_clock = clockSaved;
    g_frameJoy = frameSaved;
    std::printf("rumble selftest: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
