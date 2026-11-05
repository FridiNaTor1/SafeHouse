#pragma once

// The controller (Sly 2's JOY at 0x2DFC00, updated by UpdateJoy 0x1871C0 once per frame): buttons in the PS2
// pad's bit layout, edges, and the sticks. Native input: a GLFW gamepad and the keyboard (Sly1-Release's
// mapping), or a scripted press list (headless runs, the differential harness).

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

enum BTN : uint16_t
{
    BTN_Select = 0x0001, BTN_L3 = 0x0002, BTN_R3 = 0x0004, BTN_Start = 0x0008,
    BTN_Up = 0x0010, BTN_Right = 0x0020, BTN_Down = 0x0040, BTN_Left = 0x0080,
    BTN_L2 = 0x0100, BTN_R2 = 0x0200, BTN_L1 = 0x0400, BTN_R1 = 0x0800,
    BTN_Triangle = 0x1000, BTN_Circle = 0x2000, BTN_Cross = 0x4000, BTN_Square = 0x8000,
};

struct JOY
{
    uint16_t grfbtn = 0;         // held
    uint16_t grfbtnPressed = 0;  // went down this frame
    uint16_t grfbtnReleased = 0; // went up this frame
    float xLeft = 0, yLeft = 0;  // the sticks, -1..1 (y up)
    float xRight = 0, yRight = 0;
    bool fConnected = false;     // a gamepad is there
};

extern JOY g_joy;

// The game's pad (orig JOY at 0x2DFC00, its fields after the 0x1000-byte DMA buffer; FReadJoy 0x187998 fills them
// from the pad's raw bytes). Buttons in the game's layout: the raw bytes 2 and 3 (active low) as (b2 << 8 | b3)
// inverted: 0x100 select .. 0x1000 up, 0x2000 right, 0x4000 down, 0x8000 left; 0x1 L2, 0x2 R2, 0x4 L1, 0x8 R1,
// 0x10 triangle, 0x20 circle, 0x40 cross, 0x80 square. (JOY above keeps libpad's layout, as the scripts name them.)
struct JOYLM { float gMin, gMax; };
struct JOYPS2
{
    int term = 0x73;             // +0x1008 (the pad's terminal id: a DualShock 2 in analog mode)
    int joys = 3, joyk = 4;      // +0x100C / +0x1010 (ready, DualShock 2)
    float tRead = 0.0f;          // +0x1018 (g_clock +0x3C)
    float tActive = 0.0f;        // +0x101C
    int dtIdle = 0;              // +0x1020
    float x = 0, y = 0, uDeflect = 0;     // +0x103C.. the left stick
    uint8_t bxPrev = 0, byPrev = 0;       // +0x1048 / +0x1049
    int fStickMoved = 0;                  // +0x104C
    JOYLM almDeflect[4] = {{-0.75f, 0.75f}, {-0.75f, 0.75f}, {-0.75f, 0.75f}, {-0.75f, 0.75f}};  // +0x1050
    float x2 = 0, y2 = 0, uDeflect2 = 0;  // +0x1070.. the right stick
    uint8_t bx2Prev = 0, by2Prev = 0;     // +0x107C / +0x107D
    int fStickMoved2 = 0;                 // +0x1080
    JOYLM almDeflect2[4] = {{-0.75f, 0.75f}, {-0.75f, 0.75f}, {-0.75f, 0.75f}, {-0.75f, 0.75f}};  // +0x1084
    float dx = 0, dy = 0, dx2 = 0, dy2 = 0;  // +0x10A4..
    uint32_t grfbtn = 0;         // +0x10B4
    uint32_t grfbtnPressed = 0;  // +0x10B8
    uint32_t grfbtnHandled = 0xFFFF;  // +0x10BC (orig: SetJoyJoys' reset on ready)
    uint8_t ab[0x20]{};          // the last raw read (bytes 4..7 the sticks)
    float gDpad = 1.0f;          // +0x10EC
};
extern JOYPS2 g_joyps2;
// The pad's lock (orig G_GRFJOYT 0x2DFBC0 and its lock bits 0x2DFBC4): who gets the pad. grfjoyt bit 1 the camera's
// policies (UpdateCm 0x14D478), bit 2 the players' controllers (0x1C4468) and their actions (the slots 99), bit 4 the
// menus; without the bit they get the second pad's JOY (0x2E0D00: no pad, at rest). Lock bits: 2 the game paused
// (0x20EFC0's first pause .. 0x20F030's last), 4 a talk (0x20F0C0 / 0x20F0F0), 8 a world's load (0x20E890 .. its fade
// in 0x1D9648), 0x10 / 1 (unused natively).
extern uint32_t g_grfjoyt;     // 0x2DFBC0 (7: all)
extern uint32_t g_grfjoyLock;  // 0x2DFBC4
extern JOYPS2 g_joyps2Null;    // 0x2E0D00 (the second pad's JOY: nothing there)
// orig 0x00187078 / 0x001870A0 (lock bits set / cleared, then 0x1870D0)
void LockJoy(uint32_t grf);
void UnlockJoy(uint32_t grf);
// orig 0x001870D0 (grfjoyt from the lock bits; without bit 2 every button of the pad is taken as handled)
void RecalcJoyt();
// orig 0x00187E08 (buttons grf dropped from the held and the handled ones)
void ClearJoyBtns(JOYPS2* pjoy, uint32_t grf);
// The pad a user of grfjoyt bit `grf` reads (0x2DFBC0 & grf ? 0x2DFC00 : 0x2E0D00).
inline const JOYPS2& JoyFor(uint32_t grf) { return (g_grfjoyt & grf) ? g_joyps2 : g_joyps2Null; }
// orig 0x00187998 FReadJoy (from the pad's raw read: 32 bytes as scePadRead gives them)
void ReadJoyPs2(JOYPS2* pjoy, const uint8_t* abRaw);
// orig 0x00187DE0 SetJoyBtnHandled
void SetJoyPs2BtnHandled(JOYPS2* pjoy, uint32_t grfbtn);
// The JOY at 0x2DFC00 of a reference RAM (32 MB) into pjoy (the harnesses' take-over)
void ImportJoyPs2FromRam(JOYPS2* pjoy, const std::vector<uint8_t>& ram);
// The game's button layout from libpad's (JOY above).
inline uint32_t GrfbtnPs2FromLibpad(uint32_t grf) { return ((grf & 0xFF) << 8) | ((grf >> 8) & 0xFF); }

// A scripted press: button(s) held from frame f for c frames ("CROSS@120+8,START@200+4"), or a stick axis
// deflected (the reference's script syntax, Sly2-Reference/Sly2-Release/host/script.cpp: "LX=0@300+60": LX LY RX RY, 0..255,
// 0x80 centred, 0 left / up). While a press is active on a frame, the script alone sets the pad (unpressed
// buttons up, other axes centred), as the reference's.
struct JOYPRESS { uint16_t grfbtn; int frame; int c; int axis = -1; uint8_t bAxis = 0x80; };
bool FParseJoyPresses(const std::string& s, std::vector<JOYPRESS>& apress);

// orig 0x1871C0 UpdateJoy: pwindow null (headless): only the scripted presses
void UpdateJoy(JOY* pjoy, GLFWwindow* pwindow, int frame, const std::vector<JOYPRESS>& apress);
