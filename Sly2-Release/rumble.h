#pragma once

// The pad's vibration (Sly 2's RUMBLE at JOY +0x10CC, rumble.c 0x1D5150..0x1D55B8, and the vibration option): the
// retail patterns (RUMPAT 0x2C2260, 11 of them), the scheduler (UpdateRumble, run by UpdateJoy once per frame) and the
// actuator writes (scePadSetActDirect: byte 0 the small motor on / off, byte 1 the big motor 0-255), sent to the pad in
// use through platform/pad_rumble.h (SDL; input stays on GLFW).
//
// Who rumbles (every caller of TriggerJoyRumbleRumk 0x187E28):
//   SCPT command 0x33 (scpt.cpp; 0x1E1E30), ASEG event kind 3 (asegevent.cpp; 0x131290), the splice's bif 130
//   (gamebif.cpp; 0x125248), a breakable's item 9 (breakable.cpp; 0x1BB648: rumk = the item's value, 0.3 s),
//   a guard struck by Murray / a 0x40000 hit (aihit.cpp; 0x179328: rumk 3, 0.15 s), a guard launched by the current
//   player (aihit.cpp; 0x17B1A8: 3, 0.15 s), a guard's attack animation events (aijobs.cpp; 0x174BD0: 0x2F rumk 8 1 s
//   at the current player, 0x34 rumk 3 0.15 s at Murray).
// Stopped: a world's end (0x20E068: SetRumbleRums(Stop)).
//
// The vibration option (0x2C2D8C): 0 at boot (no rumble), 1 once the title is shown (0x1D5498: the record's flag
// "fNoVibration" decides), 2 / 3 the options menu's On / Off (0x1AFB88), written into each record before it is saved
// or taken (0x1D54B8). The launcher's Controller vibration (settings v15) gates the motors on top of it.
//
// Debug: SLY2N_RUMBLELOG=1 logs every trigger and actuator write ("rumble: ..." on stderr, with or without a pad), and
// the ASEG rumble events as they load; SLY2N_VIBRATION=n starts the option at n (2: on, for runs that skip the title).

#include <cstdint>

enum RUMS { RUMS_Nil = -1, RUMS_Dead = 0, RUMS_Idle = 1, RUMS_Rumble = 2, RUMS_Stop = 3, RUMS_Kill = 4 };
enum RUMK
{
    RUMK_Nil = -1, RUMK_0, RUMK_1, RUMK_2, RUMK_3, RUMK_4, RUMK_5, RUMK_6, RUMK_7, RUMK_8, RUMK_9, RUMK_10, RUMK_Max
};

struct RUMINS
{
    uint8_t fHighSpeedMotor;  // the small motor (actuator 0)
    uint8_t bLowSpeedMotor;   // the big motor (actuator 1)
    float dt;
};
struct RUMPAT
{
    int crumins;
    RUMINS arumins[32];
};
struct RUMBLE
{
    int nPort = 0, nSlot = 0;     // +0x00 / +0x04
    RUMS rums = RUMS_Dead;        // +0x08
    const RUMPAT* prumpat = nullptr;  // +0x0C
    int irumins = 0;              // +0x10
    float dtRumble = 0.0f;        // +0x14
    float dtRumins = 0.0f;        // +0x18
};

extern RUMBLE g_rumble;           // JOY 0x2DFC00 +0x10CC (0x2E0CCC)
extern int g_vibrationSetting;    // 0x2C2D8C
const RUMPAT& RumpatRetail(int rumk);  // 0x2C2260[rumk]

void InitRumble(RUMBLE* prumble, int nPort, int nSlot);          // orig 0x1D5150
void UpdateRumble(RUMBLE* prumble);                              // orig 0x1D51B8
void TriggerRumbleRumk(RUMBLE* prumble, int rumk, float dt);     // orig 0x1D5310
void TriggerRumbleRumpat(RUMBLE* prumble, const RUMPAT* prumpat, float dt);  // orig 0x1D5340
void SetRumbleRums(RUMBLE* prumble, RUMS rums);                  // orig 0x1D53C0
void StopRumbleActuators(RUMBLE* prumble);                       // orig 0x1D5468
void EnableVibration();                                          // orig 0x1D5498 (RumbleUnknown1)
void ApplyVibrationSetting();                                    // orig 0x1D54B8
bool FVibrationEnabled();                                        // orig 0x1D55B8
void SetVibrationOption(bool fOff);                              // orig 0x1AFB88 cases 72 / 73 (the menu's On / Off)

// orig 0x187E28: the pad's rumble (rumk for dt seconds), when the pad is a ready DualShock 2 with rumble enabled;
// psz: who asked (the log only)
void TriggerJoyRumbleRumk(int rumk, float dt, const char* psz = nullptr);
// The world's end (0x20E068): the rumble stopped (it runs out its four-frame stop)
void StopJoyRumble();

// The platform: the launcher's Controller vibration (settings v15 controllerVibration), and the SDL motors (opened on
// the first write with a window; never in headless runs). ShutdownPadRumble turns every motor off.
extern bool g_fControllerVibration;
void ShutdownPadRumble();
// The pad in use (pcinput.cpp, the GLFW joystick id; -1: the keyboard / none) and the window's presence, per frame
void SetRumblePad(int jid, bool fWindow);
// (tests) --rumble-selftest: the retail table, the mapping, the scheduler's timing
int RumbleSelfTestCommand();
