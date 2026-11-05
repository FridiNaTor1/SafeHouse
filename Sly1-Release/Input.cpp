#include "Input.h"
#include "porttext.h"
#include "mouseinput.h"
#include "prompt.h"
#include "content.h"
// Stick dead zone (upstream ProjectCane's setting; 35% is the retail response).
float g_controllerDeadzone = 0.35f;
#include "clock.h"
#include "credit.h"
#include "pzo.h"
#include "sound.h"
#include "transition.h"
#ifdef _WIN32
#include "spu_reverb_xapo.h"
#endif
#include "ui.h"
#include "../platform/controller_family.h"
#include "mb.h"
// SDL is used only to drive the controller motors (GLFW has no rumble). Input
// itself still comes from GLFW, so the controller family / prompt detection
// is unchanged.
#include "../platform/pad_rumble.h"
#include "save.h"
#include <cstdio>
#include <iterator>
#include <vector>

namespace
{
constexpr uint16_t JBC_SELECT   = 0x0001;
constexpr uint16_t JBC_L3       = 0x0002;
constexpr uint16_t JBC_START    = 0x0008;
constexpr uint16_t JBC_UP       = 0x0010;
constexpr uint16_t JBC_RIGHT    = 0x0020;
constexpr uint16_t JBC_DOWN     = 0x0040;
constexpr uint16_t JBC_LEFT     = 0x0080;
constexpr uint16_t JBC_L2       = 0x0100;
constexpr uint16_t JBC_R2       = 0x0200;
constexpr uint16_t JBC_R1       = 0x0800;
constexpr uint16_t JBC_TRIANGLE = 0x1000;
constexpr uint16_t JBC_CIRCLE   = 0x2000;
constexpr uint16_t JBC_CROSS    = 0x4000;
constexpr uint16_t JBC_SQUARE   = 0x8000;

// Device selection should be stable around stick drift, but it must not pin a
// stale Steam Input/DirectInput device forever. A button reports activity 2.0,
// while axes report 0..1, so this epsilon only resolves near-equal samples.
constexpr float GAMEPAD_ACTIVITY_TIE_EPSILON = 0.05f;

CODE* s_pcode = nullptr;
float s_tCodeCheck = 0.0f;

uint16_t GrfbtnFromState(const JOY::BUTTONS& state)
{
    uint16_t grfbtn = 0;
    const auto held = [&state](JOY_BUTTON button)
    {
        return state[button];
    };

    if (held(BTN_SELECT))   grfbtn |= JBC_SELECT;
    if (held(BTN_L3))       grfbtn |= JBC_L3;
    if (held(BTN_START))    grfbtn |= 0x0008;
    if (held(BTN_UP))       grfbtn |= JBC_UP;
    if (held(BTN_RIGHT))    grfbtn |= JBC_RIGHT;
    if (held(BTN_DOWN))     grfbtn |= JBC_DOWN;
    if (held(BTN_LEFT))     grfbtn |= JBC_LEFT;
    if (held(BTN_L2))       grfbtn |= JBC_L2;
    if (held(BTN_R2))       grfbtn |= 0x0200;
    if (held(BTN_L1))       grfbtn |= 0x0400;
    if (held(BTN_R1))       grfbtn |= JBC_R1;
    if (held(BTN_TRIANGLE)) grfbtn |= JBC_TRIANGLE;
    if (held(BTN_CIRCLE))   grfbtn |= JBC_CIRCLE;
    if (held(BTN_CROSS))    grfbtn |= JBC_CROSS;
    if (held(BTN_SQUARE))   grfbtn |= JBC_SQUARE;
    return grfbtn;
}

void CodeResetWorld(uint32_t nParam)
{
    g_transition.ResetWorld(static_cast<int>(nParam));
}

void CodeResetCheats(uint32_t)
{
    g_grfcht = 0;
    g_transition.ResetWorld(0);
}

void CodeSetCheatFlags(uint32_t nParam)
{
    g_grfcht |= static_cast<GRFCHT>(nParam & ~0x4000U);
    if ((nParam & 0x4000U) != 0)
        g_transition.ResetWorld(0);
}

void CodeCollectAllClues(uint32_t)
{
    if (g_psw != nullptr)
        CollectAllClues();
}

void CodeSetVaultFlags(uint32_t nParam)
{
    if (g_pgsCur != nullptr)
        g_pgsCur->grfvault = nParam;
}

void CompleteWorld(GAMEWORLD gameWorld)
{
    if (g_pgsCur == nullptr)
        return;

    WS& world = g_pgsCur->aws[static_cast<int>(gameWorld)];
    for (LS& level : world.als)
        level.grfls = static_cast<GRFLS>(level.grfls | 1U);
    world.fws |= 31;
}

void CodeCompleteAllWorlds(uint32_t)
{
    for (int i = 0; i < 6; ++i)
        CompleteWorld(static_cast<GAMEWORLD>(i));

    // Retail compares this field against the raw UIS_Hub value.
    if (g_ui.uisPlaying == UIS_Hub)
        g_transition.ResetWorld(0);
}

void CodeShowPassword(uint32_t)
{
    if (g_pgsCur == nullptr)
        return;

    const int worldLevel =
        (static_cast<int>(g_pgsCur->gameWorldCur) << 8) |
        static_cast<int>(g_pgsCur->worldLevelCur);

    if (worldLevel != 0x0400 || (GetGameProgress() & 6U) != 6U ||
        g_pgsCur->ccoin != 99 || g_pgsCur->clife != 0)
        return;

    char message[64]{};
    std::snprintf(message, sizeof(message), ContentUiText(
        "The password is: %s", "Le mot de passe est : %s", "Das Passwort lautet: %s",
        "La contraseña es: %s", "La password è: %s"), "chetkido");
    g_pnote.pvtblot->pfnSetBlotAchzDraw(&g_pnote, message);
    SetBlotDtVisible(&g_pnote, 10.0f);
    g_pnote.pvtblot->pfnShowBlot(&g_pnote);
}

constexpr uint16_t s_codeResetWorld[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_SELECT };
constexpr uint16_t s_codeResetCheats[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_START };
constexpr uint16_t s_codeCheat4[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_LEFT };
constexpr uint16_t s_codeCheat8[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_RIGHT };
constexpr uint16_t s_codeCheat2[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_CIRCLE };
constexpr uint16_t s_codeClues[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_UP };
constexpr uint16_t s_codeVaultFlags[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_DOWN };
constexpr uint16_t s_codeWorlds[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_TRIANGLE };
constexpr uint16_t s_codePassword[] =
    { JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_DOWN, JBC_L2, JBC_R2, JBC_CROSS, JBC_LEFT, JBC_CIRCLE, JBC_SQUARE, JBC_SQUARE, JBC_SQUARE };

CODE s_codes[] =
{
    { s_codeResetWorld,  static_cast<int>(std::size(s_codeResetWorld)),  CodeResetWorld,        0 },
    { s_codeResetCheats, static_cast<int>(std::size(s_codeResetCheats)), CodeResetCheats,       0 },
    { s_codeCheat4,      static_cast<int>(std::size(s_codeCheat4)),      CodeSetCheatFlags, 0x4004 },
    { s_codeCheat8,      static_cast<int>(std::size(s_codeCheat8)),      CodeSetCheatFlags,      8 },
    { s_codeCheat2,      static_cast<int>(std::size(s_codeCheat2)),      CodeSetCheatFlags,      2 },
    { s_codeClues,       static_cast<int>(std::size(s_codeClues)),       CodeCollectAllClues,    0 },
    { s_codeVaultFlags,  static_cast<int>(std::size(s_codeVaultFlags)),  CodeSetVaultFlags, 0xF000FFFF },
    { s_codeWorlds,      static_cast<int>(std::size(s_codeWorlds)),      CodeCompleteAllWorlds,  0 },
    { s_codePassword,    static_cast<int>(std::size(s_codePassword)),    CodeShowPassword,       0 }
};

constexpr std::array<int, BTN_MAX> kDefaultKeyboardBindings =
{
    GLFW_KEY_W, GLFW_KEY_S, GLFW_KEY_A, GLFW_KEY_D,
    GLFW_KEY_J, GLFW_KEY_L, GLFW_KEY_K, GLFW_KEY_I,
    GLFW_KEY_ENTER, GLFW_KEY_BACKSPACE,
    GLFW_KEY_Q, GLFW_KEY_E, GLFW_KEY_Z, GLFW_KEY_C,
    GLFW_KEY_UNKNOWN, GLFW_KEY_UNKNOWN
};

constexpr std::array<int, BTN_MAX> kDefaultGamepadBindings =
{
    GLFW_GAMEPAD_BUTTON_DPAD_UP, GLFW_GAMEPAD_BUTTON_DPAD_DOWN,
    GLFW_GAMEPAD_BUTTON_DPAD_LEFT, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,
    GLFW_GAMEPAD_BUTTON_X, GLFW_GAMEPAD_BUTTON_B,
    GLFW_GAMEPAD_BUTTON_A, GLFW_GAMEPAD_BUTTON_Y,
    GLFW_GAMEPAD_BUTTON_START, GLFW_GAMEPAD_BUTTON_BACK,
    GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
    GAMEPAD_BINDING_LEFT_TRIGGER, GAMEPAD_BINDING_RIGHT_TRIGGER,
    GLFW_GAMEPAD_BUTTON_LEFT_THUMB, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB
};

// A bound keyboard key (mouse bindings are read by mouseinput.cpp).
static bool FKeyHeld(GLFWwindow* window, int key)
{
    return key >= GLFW_KEY_SPACE && key <= GLFW_KEY_LAST && glfwGetKey(window, key) == GLFW_PRESS;
}

// Keys that count as keyboard use for the prompt family: every bound key,
// the camera keys and Escape, which opens and leaves the menus.
static uint32_t GrfKeyboardHeld(GLFWwindow* window)
{
    uint32_t grf = 0;
    for (int button = 0; button < BTN_MAX; ++button)
        if (FKeyHeld(window, g_keyboardBindings[button]))
            grf |= 1U << button;
    for (int i = 0; i < static_cast<int>(g_cameraKeyBindings.size()); ++i)
        if (FKeyHeld(window, g_cameraKeyBindings[i]))
            grf |= 1U << (BTN_MAX + i);
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        grf |= 1U << (BTN_MAX + 4);
    return grf;
}

static uint32_t s_grfKeyboardHeldPrev = 0;

void InitializeGamepadState(GLFWgamepadstate* state)
{
    *state = {};
    // GLFW's standardized trigger convention rests at -1.  A zero-initialized
    // trigger otherwise looks half pressed to activity selection.
    state->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
    state->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
}

// The retail patterns: s_mprumkrumpat at 0x26B8B0 in SCUS_971.98 (11 RUMPATs of 0x104 bytes: crumins, then
// {fHighSpeedMotor, bLowSpeedMotor, 2 pad, dt} per step), read from the ELF and the same as the sly1 decomp's
// P2/rumble.c. The earlier table here was invented. {dt, high-speed motor on / off, low-speed motor 0-255}
constexpr RUMINS s_ruminsSteadyBuzz[] = { { 0.05f, 1, 0 } };
constexpr RUMINS s_ruminsLowThrob[] = { { 0.05f, 0, 150 }, { 0.05f, 0, 0 } };
constexpr RUMINS s_ruminsMediumThrob[] = { { 0.05f, 0, 210 }, { 0.10f, 0, 0 } };
constexpr RUMINS s_ruminsHardThrob[] = { { 0.10f, 0, 255 }, { 0.10f, 0, 0 } };
constexpr RUMINS s_ruminsBlunt[] =
{
    { 0.10f, 0, 210 }, { 0.01f, 0, 190 }, { 0.01f, 0, 170 }, { 0.01f, 0, 150 }, { 0.01f, 0, 130 }, { 0.01f, 0, 110 },
    { 0.25f, 0, 0 }
};
constexpr RUMINS s_ruminsElectric[] = { { 0.10f, 1, 255 }, { 0.05f, 1, 0 } };
constexpr RUMINS s_ruminsFire[] = { { 0.05f, 0, 170 }, { 0.10f, 0, 0 } };
constexpr RUMINS s_ruminsWater[] = { { 0.05f, 0, 170 }, { 0.10f, 0, 0 } };
constexpr RUMINS s_ruminsCrush[] = { { 0.05f, 0, 170 }, { 0.10f, 0, 0 } };
constexpr RUMINS s_ruminsBreak[] = { { 0.15f, 0, 160 }, { 0.10f, 0, 0 }, { 0.05f, 0, 90 } };
constexpr RUMINS s_ruminsBomb[] =
{
    { 0.20f, 0, 255 }, { 0.08f, 0, 0 }, { 0.13f, 0, 200 }, { 0.06f, 0, 0 }, { 0.11f, 0, 150 }
};

constexpr RUMPAT s_arumpat[RUMK_Max] =
{
    { s_ruminsSteadyBuzz,  static_cast<int>(std::size(s_ruminsSteadyBuzz)) },
    { s_ruminsLowThrob,    static_cast<int>(std::size(s_ruminsLowThrob)) },
    { s_ruminsMediumThrob, static_cast<int>(std::size(s_ruminsMediumThrob)) },
    { s_ruminsHardThrob,   static_cast<int>(std::size(s_ruminsHardThrob)) },
    { s_ruminsBlunt,       static_cast<int>(std::size(s_ruminsBlunt)) },
    { s_ruminsElectric,    static_cast<int>(std::size(s_ruminsElectric)) },
    { s_ruminsFire,        static_cast<int>(std::size(s_ruminsFire)) },
    { s_ruminsWater,       static_cast<int>(std::size(s_ruminsWater)) },
    { s_ruminsCrush,       static_cast<int>(std::size(s_ruminsCrush)) },
    { s_ruminsBreak,       static_cast<int>(std::size(s_ruminsBreak)) },
    { s_ruminsBomb,        static_cast<int>(std::size(s_ruminsBomb)) }
};

#ifdef CANE_SDL_RUMBLE
cane::rumble::SdlRumble s_sdlRumble;
#endif

// The retail actuator bytes to the pad in use (platform/pad_rumble.h: the shared mapping and SDL device matching).
void SetSdlRumble(const JOY* pjoy, uint8_t highSpeedMotor, uint8_t lowSpeedMotor)
{
#ifdef CANE_SDL_RUMBLE
    if (pjoy == nullptr || !pjoy->gamepadConnected || pjoy->joystickId < 0)
        return;
    const char* guid = glfwGetJoystickGUID(pjoy->joystickId);
    if (guid == nullptr)
        return;
    // (the launcher's Controller vibration off: the motors stay still)
    if (!g_fControllerVibration)
        highSpeedMotor = lowSpeedMotor = 0;
    s_sdlRumble.set(guid, cane::rumble::MotorsFromDualShock(highSpeedMotor, lowSpeedMotor), glfwGetTime());
#else
    (void)pjoy; (void)highSpeedMotor; (void)lowSpeedMotor;
#endif
}

const char* PchzKeyboardKeyNameInternal(int key)
{
    switch (key)
    {
    case GLFW_KEY_UNKNOWN: return PchzPort(PTX_KeyUnbound);
    case GLFW_KEY_SPACE: return PchzPort(PTX_KeySpace);
    case GLFW_KEY_ENTER: return PchzPort(PTX_KeyEnter);
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return PchzPort(PTX_KeyBackspace);
    case GLFW_KEY_LEFT_SHIFT: return PchzPort(PTX_KeyLeftShift);
    case GLFW_KEY_RIGHT_SHIFT: return PchzPort(PTX_KeyRightShift);
    case GLFW_KEY_LEFT_CONTROL: return PchzPort(PTX_KeyLeftCtrl);
    case GLFW_KEY_RIGHT_CONTROL: return PchzPort(PTX_KeyRightCtrl);
    case GLFW_KEY_LEFT_ALT: return PchzPort(PTX_KeyLeftAlt);
    case GLFW_KEY_RIGHT_ALT: return PchzPort(PTX_KeyRightAlt);
    case GLFW_KEY_UP: return PchzPort(PTX_KeyUp);
    case GLFW_KEY_DOWN: return PchzPort(PTX_KeyDown);
    case GLFW_KEY_LEFT: return PchzPort(PTX_KeyLeft);
    case GLFW_KEY_RIGHT: return PchzPort(PTX_KeyRight);
    default:
        if (const char* name = glfwGetKeyName(key, 0))
            return name;
        return PchzPort(PTX_Key);
    }
}
}

bool StartupControllerInput()
{
#ifdef CANE_SDL_RUMBLE
    return s_sdlRumble.startup();
#else
    return true;
#endif
}

void ShutdownControllerInput()
{
    StopJoyRumble(&g_joy);
#ifdef CANE_SDL_RUMBLE
    s_sdlRumble.shutdown();
#endif
}

bool FControllerInputAvailable()
{
    // Controllers are read through GLFW.
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
        if (glfwJoystickIsGamepad(jid))
            return true;
    return false;
}

const char* PchzKeyboardKeyName(int key)
{
    if (cane::settings::mouseBinding(key))
        return PchzMouseBindingName(key);
    return PchzKeyboardKeyNameInternal(key);
}

const char* PchzGamepadBindingName(int binding, cane::input::Family family)
{
    // Keyboard keeps the neutral "A / Cross" names used when no pad is active.
    return cane::input::buttonLabel(cane::input::fromGlfwBinding(binding), family);
}

void JOY::Update(GLFWwindow* window)
{
    const int previousJoystickId = joystickId;
    const uint16_t grfbtnPrev = GrfbtnFromState(current);
    previous = current;
    handled.fill(false);
    forcedHeld.fill(false);

    for (int button = 0; button < BTN_MAX; ++button)
        current[static_cast<JOY_BUTTON>(button)] = false;

    // Mouse motion, buttons and the cursor capture for this update.
    UpdateMouseInput(window);
    const bool fMenuMouse = g_mouseopt.fMenus && g_prompt.fActive && g_prompt.blots != BLOTS_Hidden;
    for (int button = 0; button < BTN_MAX; ++button)
    {
        const int key = g_keyboardBindings[button];
        // In a menu with "Mouse in Menus" on, the left / right buttons only
        // choose / go back there: an action bound to them is not sent.
        if (fMenuMouse && (key == cane::settings::kMouseBindingBase + GLFW_MOUSE_BUTTON_LEFT ||
                           key == cane::settings::kMouseBindingBase + GLFW_MOUSE_BUTTON_RIGHT))
            continue;
        current[static_cast<JOY_BUTTON>(button)] = cane::settings::mouseBinding(key)
            ? FMouseBindingHeld(key) : FKeyHeld(window, key);
    }
    // Escape is a fixed second Start key: it pauses, and leaves menus (which
    // Start backs out of).
    if (glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        current[BTN_START] = true;

    // A newly pressed key makes the keyboard the last used device.
    const uint32_t grfKeyboardHeld = GrfKeyboardHeld(window);
    if ((grfKeyboardHeld & ~s_grfKeyboardHeldPrev) != 0)
        cane::input::tracker().noteKeyboard();
    s_grfKeyboardHeldPrev = grfKeyboardHeld;

    float xRaw = 0.0f;
    float yRaw = 0.0f;
    float x2Raw = 0.0f;
    float y2Raw = 0.0f;

    GLFWgamepadstate state{};
    InitializeGamepadState(&state);
    bool haveGamepadState = false;
    int activeJoystickId = JOYID_Nil;
    const bool allowControllerInput = !g_fDisableControllerInputWhenUnfocused ||
        glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;

    // Steam may leave an idle virtual gamepad in a lower joystick slot than
    // the physical controller. Poll every mapped gamepad and prefer a device
    // that is actually receiving input instead of permanently binding to the
    // first slot returned by GLFW.
    if (allowControllerInput)
    {
        const int jid = cane::input::selectActivePad(joystickId, &state);
        if (jid >= 0)
        {
            activeJoystickId = jid;
            haveGamepadState = true;
        }

        // A pressed button or a deflected stick makes that pad's family the
        // one prompts are drawn for.
        cane::input::tracker().pollGamepads();
    }

    if (haveGamepadState)
    {
        if (!gamepadConnected || activeJoystickId != joystickId)
        {
            calibration = StickCalibration{};
            calibration2 = StickCalibration{};
#ifdef CANE_SDL_RUMBLE
            // the pad that came into use opened for the motors now, not at its first vibration (platform/pad_rumble.h)
            s_sdlRumble.prepare(glfwGetJoystickGUID(activeJoystickId), glfwGetTime());
#endif
        }

        joystickId = activeJoystickId;
        gamepadConnected = true;
        gamepadState = state;
        hasGamepadState = true;
        for (int button = 0; button < BTN_MAX; ++button)
        {
            const int binding = g_gamepadBindings[button];
            bool held = false;
            if (binding >= 0 && binding <= GLFW_GAMEPAD_BUTTON_LAST)
                held = state.buttons[binding] == GLFW_PRESS;
            else if (binding == GAMEPAD_BINDING_LEFT_TRIGGER)
                held = state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.25f;
            else if (binding == GAMEPAD_BINDING_RIGHT_TRIGGER)
                held = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.25f;
            current[static_cast<JOY_BUTTON>(button)] = current[static_cast<JOY_BUTTON>(button)] || held;
        }

        xRaw = state.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        // STEP/JT expects the original PlayStation convention: forward is
        // positive Y. GLFW reports stick-up as -1, so invert it here.
        yRaw = -state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        x2Raw = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
        y2Raw = -state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
    }
    else
    {
        joystickId = JOYID_Nil;
        gamepadConnected = false;
        hasGamepadState = false;
        InitializeGamepadState(&gamepadState);
    }

    // Switching pads stops the old one's motors.
    if (previousJoystickId != JOYID_Nil && previousJoystickId != joystickId &&
        rumble.rums == RUMS_Rumble)
    {
        JOY previousJoy = *this;
        previousJoy.joystickId = previousJoystickId;
        previousJoy.gamepadConnected = true;
        SetSdlRumble(&previousJoy, 0, 0);
    }

    // Keyboard movement emulates the left analog stick.  The base CPMAN
    // policy is normally active during gameplay, so it must not suppress
    // these values or the player receives a permanently centered stick.
    if (current[BTN_LEFT] && !current[BTN_RIGHT]) xRaw = -1.0f;
    if (current[BTN_RIGHT] && !current[BTN_LEFT]) xRaw = 1.0f;
    if (current[BTN_UP] && !current[BTN_DOWN]) yRaw = 1.0f;
    if (current[BTN_DOWN] && !current[BTN_UP]) yRaw = -1.0f;

    // Keyboard camera input emulates the right analog stick. UpdateCptn
    // consumes x2 for manual orbiting around the player. The keys are
    // rebindable (Keyboard Mapping), the arrows by default.
    bool cameraLeft = FKeyHeld(window, g_cameraKeyBindings[0]);
    bool cameraRight = FKeyHeld(window, g_cameraKeyBindings[1]);
    bool cameraUp = FKeyHeld(window, g_cameraKeyBindings[2]);
    bool cameraDown = FKeyHeld(window, g_cameraKeyBindings[3]);

    // While a prompt menu is up, the camera keys move the highlight like the
    // d-pad and Enter confirms like Cross, as PC menus do. An Enter still
    // held from opening the menu (Enter is Start by default) waits for its
    // release, so it never confirms the first row.
    // An Enter that confirmed stays a confirm until released, so closing a
    // menu with it does not reopen the pause menu.
    static bool s_fEnterLatched = false, s_fEnterConfirms = false;
    const bool fEnter = FKeyHeld(window, GLFW_KEY_ENTER) || FKeyHeld(window, GLFW_KEY_KP_ENTER);
    const bool fMenu = g_prompt.fActive && g_prompt.blots != BLOTS_Hidden;
    if (!fEnter)
        s_fEnterLatched = s_fEnterConfirms = false;
    else if (fMenu && !s_fEnterLatched)
        s_fEnterConfirms = true;
    else if (!fMenu && !s_fEnterConfirms)
        s_fEnterLatched = true;
    if (s_fEnterConfirms)
    {
        current[BTN_CROSS] = current[BTN_CROSS] || fMenu;
        if (g_keyboardBindings[BTN_START] == GLFW_KEY_ENTER || g_keyboardBindings[BTN_START] == GLFW_KEY_KP_ENTER)
            current[BTN_START] = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    }
    if (fMenu)
    {
        current[BTN_LEFT] = current[BTN_LEFT] || cameraLeft;
        current[BTN_RIGHT] = current[BTN_RIGHT] || cameraRight;
        current[BTN_UP] = current[BTN_UP] || cameraUp;
        current[BTN_DOWN] = current[BTN_DOWN] || cameraDown;
        cameraLeft = cameraRight = cameraUp = cameraDown = false;
    }

    if (cameraLeft && !cameraRight) x2Raw = -1.0f;
    if (cameraRight && !cameraLeft) x2Raw = 1.0f;
    if (cameraUp && !cameraDown) y2Raw = 1.0f;
    if (cameraDown && !cameraUp) y2Raw = -1.0f;

    ApplyStickDeadZone(xRaw, yRaw, &calibration, &x, &y, &uDeflect);
    ApplyStickDeadZone(x2Raw, y2Raw, &calibration2, &x2, &y2, &uDeflect2);

    _MatchCodes(this, grfbtnPrev);

    UpdateRumble(&rumble, this);

    joys = JOYS_Ready;
}

void AddCode(CODE* pcode)
{
    for (CODE* current = s_pcode; current != nullptr; current = current->pcodeNext)
        if (current == pcode)
            return;

    pcode->pcodeNext = s_pcode;
    s_pcode = pcode;
    pcode->ijbcCur = GrfbtnFromState(g_joy.current) == pcode->ajbc[0] ? 1 : 0;
}

void _ResetCodes()
{
    for (CODE* code = s_pcode; code != nullptr; code = code->pcodeNext)
        code->ijbcCur = 0;
}

void _MatchCodes(JOY* pjoy, uint16_t grfbtnPrev)
{
    const uint16_t grfbtn = GrfbtnFromState(g_joy.current);
    if (s_pcode == nullptr || pjoy != &g_joy || grfbtnPrev == grfbtn || grfbtn == 0)
        return;

    for (CODE* code = s_pcode; code != nullptr; code = code->pcodeNext)
    {
        if (code->ijbcCur < code->cjbc)
            code->ijbcCur = grfbtn == code->ajbc[code->ijbcCur] ? code->ijbcCur + 1 : 0;
    }

    s_tCodeCheck = g_clock.tReal + 1.0f;
}

void UpdateCodes()
{
    if (s_tCodeCheck == 0.0f || s_tCodeCheck > g_clock.tReal)
        return;

    CODE* matched = nullptr;
    CODE** link = &s_pcode;
    while (*link != nullptr)
    {
        CODE* code = *link;
        if (code->fRemove != 0)
        {
            *link = code->pcodeNext;
            code->pcodeNext = nullptr;
            code->ijbcCur = 0;
            code->fRemove = 0;
            continue;
        }

        if (code->ijbcCur >= code->cjbc &&
            (matched == nullptr || matched->cjbc < code->cjbc))
            matched = code;

        link = &code->pcodeNext;
    }

    if (matched != nullptr)
    {
        StartSound(static_cast<SFXID>(121), nullptr, nullptr, nullptr,
            3000.0f, 300.0f, 1.0f, 0.0f, 0.0f, nullptr, nullptr);
        matched->pfn(matched->nParam);
    }

    _ResetCodes();
    s_tCodeCheck = 0.0f;
}

void StartupCodes()
{
    for (CODE& code : s_codes)
        AddCode(&code);
}

bool JOY::IsPressed(JOY_BUTTON button)
{
    return current[button] && !previous[button] && !handled[button];
}

bool JOY::IsHeld(JOY_BUTTON button)
{
    return current[button] || forcedHeld[button];
}

bool JOY::IsReleased(JOY_BUTTON button)
{
    return !current[button] && previous[button] && !handled[button];
}

void JOY::SetHandled(JOY_BUTTON button)
{
    handled[button] = true;
}

int JOY::DxSelectionJoy(float currentTime)
{
    if (dxLatch == 0)
    {
        if (IsPressed(BTN_LEFT))
        {
            SetHandled(BTN_LEFT);
            dxLatch = -1;
        }
        else if (IsPressed(BTN_RIGHT))
        {
            SetHandled(BTN_RIGHT);
            dxLatch = 1;
        }
        else if (uDeflect >= 0.8f && std::abs(y) < std::abs(x))
        {
            dxLatch = x < 0.0f ? -1 : 1;
        }

        if (dxLatch == 0)
            return 0;

        dtLatchX = 0.3333f;
    }
    else
    {
        bool fStillHeld;
        if (dxLatch < 0)
        {
            fStillHeld = IsHeld(BTN_LEFT) ||
                (uDeflect >= 0.25f && std::abs(y) < std::abs(x) && x < 0.0f);
        }
        else
        {
            fStillHeld = IsHeld(BTN_RIGHT) ||
                (uDeflect >= 0.25f && std::abs(y) < std::abs(x) && x > 0.0f);
        }

        if (!fStillHeld)
        {
            dxLatch = 0;
            return 0;
        }

        if (currentTime < tLatchX)
            return 0;

        dtLatchX = glm::clamp(dtLatchX * 0.6666667f, 0.1f, 0.3333f);
    }

    tLatchX = currentTime + dtLatchX;
    return dxLatch;
}

int JOY::DySelectionJoy(float currentTime)
{
    if (dyLatch == 0)
    {
        if (IsPressed(BTN_UP))
        {
            SetHandled(BTN_UP);
            dyLatch = -1;
        }
        else if (IsPressed(BTN_DOWN))
        {
            SetHandled(BTN_DOWN);
            dyLatch = 1;
        }
        else if (uDeflect >= 0.8f && std::abs(x) < std::abs(y))
        {
            dyLatch = y > 0.0f ? -1 : 1;
        }

        if (dyLatch == 0)
            return 0;

        dtLatchY = 0.3333f;
    }
    else
    {
        bool fStillHeld;
        if (dyLatch < 0)
        {
            fStillHeld = IsHeld(BTN_UP) ||
                (uDeflect >= 0.25f && std::abs(x) < std::abs(y) && y > 0.0f);
        }
        else
        {
            fStillHeld = IsHeld(BTN_DOWN) ||
                (uDeflect >= 0.25f && std::abs(x) < std::abs(y) && y < 0.0f);
        }

        if (!fStillHeld)
        {
            dyLatch = 0;
            return 0;
        }

        if (currentTime < tLatchY)
            return 0;

        dtLatchY = glm::clamp(dtLatchY * 0.6666667f, 0.1f, 0.3333f);
    }

    tLatchY = currentTime + dtLatchY;
    return dyLatch;
}

void JOY::StartJoySelection()
{
    dxLatch = 0;
    dyLatch = 0;
    tLatchX = 0.0f;
    tLatchY = 0.0f;
    dtLatchX = 0.3333f;
    dtLatchY = 0.3333f;
}

void AddGrfusr(GRFUSR grfusr)
{
    g_grfusr |= grfusr;
    UpdateGrfjoytFromGrfusr();
}

void RemoveGrfusr(GRFUSR grfusr)
{
    g_grfusr &= ~grfusr;
    UpdateGrfjoytFromGrfusr();
}

void UpdateGrfjoytFromGrfusr()
{
    if ((g_grfusr & 4) != 0)
        g_grfjoyt = 0;
    else if ((g_grfusr & 1) != 0)
        g_grfjoyt = 5;
    else if ((g_grfusr & 2) != 0)
        g_grfjoyt = 4;
    else
        g_grfjoyt = 7;
}

void EnableVibration()
{
    if (vibrationSetting == 0)
        vibrationSetting = 1;
}

void StopJoyRumble(JOY* pjoy)
{
    if (pjoy == nullptr)
        return;

    SetSdlRumble(pjoy, 0, 0);
    pjoy->rumble.rums = RUMS_Idle;
    pjoy->rumble.prumpat = nullptr;
    pjoy->rumble.irumins = 0;
    pjoy->rumble.dtRumins = 0.0f;
    pjoy->rumble.dtRumble = 0.0f;
}

void TriggerJoyRumbleRumk(JOY* pjoy, RUMK rumk, float dt)
{
    if (pjoy == nullptr || rumk < RUMK_SteadyBuzz || rumk >= RUMK_Max || dt <= 0.0f ||
        g_pgsCur == nullptr || !FVibrationEnabled(g_pgsCur))
        return;

    const RUMPAT& pattern = s_arumpat[static_cast<int>(rumk)];
    if (pattern.crumins <= 0)
        return;

    RUMBLE& rumble = pjoy->rumble;
    rumble.dtRumble = dt;
    rumble.prumpat = &pattern;
    rumble.irumins = 0;
    rumble.dtRumins = pattern.arumins[0].dt;
    rumble.rums = RUMS_Rumble;
    SetSdlRumble(pjoy, pattern.arumins[0].fHighSpeedMotor,
        pattern.arumins[0].bLowSpeedMotor);
}

void UpdateRumble(RUMBLE* prumble, JOY* pjoy)
{
    if (prumble == nullptr || pjoy == nullptr || prumble->rums != RUMS_Rumble)
        return;

    if (!g_clock.fEnabled || prumble->dtRumble <= 0.0f || g_pgsCur == nullptr ||
        !FVibrationEnabled(g_pgsCur) || !pjoy->gamepadConnected ||
        prumble->prumpat == nullptr || prumble->prumpat->crumins <= 0)
    {
        StopJoyRumble(pjoy);
        return;
    }

    // orig 0x1A7B88 UpdateRumble: at most one step per frame, the step's own time (not carried over), then the
    // pattern's time; it stops (the motors off) once that runs out
    const float dt = g_clock.dtReal;
    prumble->dtRumins -= dt;
    if (prumble->dtRumins <= 0.0f)
    {
        if (++prumble->irumins >= prumble->prumpat->crumins)
            prumble->irumins = 0;
        const RUMINS& instruction = prumble->prumpat->arumins[prumble->irumins];
        SetSdlRumble(pjoy, instruction.fHighSpeedMotor, instruction.bLowSpeedMotor);
        prumble->dtRumins = instruction.dt;
    }
    prumble->dtRumble -= dt;  // (run out: the next frame's check above stops it, as retail's RUMS_Stop)
}

void ToggleVibration(GS* pgs)
{
    constexpr uint32_t GRFGS_VibrationDisabled = 0x20;

    switch (vibrationSetting)
    {
    case 1:
        // Toggle the saved vibration flag.
        pgs->grfgs ^= GRFGS_VibrationDisabled;
        break;

    case 2:
        // Currently forced on; switch it off.
        pgs->grfgs |= GRFGS_VibrationDisabled;
        break;

    case 3:
        // Currently forced off; switch it on.
        pgs->grfgs &= ~GRFGS_VibrationDisabled;
        break;

    default:
        break;
    }

    if ((pgs->grfgs & GRFGS_VibrationDisabled) == 0)
        vibrationSetting = 2; // Enabled
    else
        vibrationSetting = 3; // Disabled

    if (!FVibrationEnabled(pgs))
        StopJoyRumble(&g_joy);
}

void ApplyVibrationSetting(GS* pgs)
{
    if (vibrationSetting == 2) {
        pgs->grfgs = pgs->grfgs & 4294967263;
        return;
    }
    if ((2 < vibrationSetting) && (vibrationSetting == 3)) {
        pgs->grfgs = pgs->grfgs | 32;
        StopJoyRumble(&g_joy);
        return;
    }
}

bool FVibrationEnabled(GS* pgs)
{
    if (vibrationSetting == 0 || vibrationSetting == 3)
        return 0;

    if (vibrationSetting == 2)
        return 1;

    return (pgs->grfgs & 0x20) == 0;
}

JOY g_joy{};
JOY g_joyZero{};
int g_grfjoyt;
GRFUSR g_grfusr;
int vibrationSetting = 1;
std::array<int, BTN_MAX> g_keyboardBindings = kDefaultKeyboardBindings;
std::array<int, BTN_MAX> g_gamepadBindings = kDefaultGamepadBindings;
std::array<int, 4> g_cameraKeyBindings = { GLFW_KEY_LEFT, GLFW_KEY_RIGHT, GLFW_KEY_UP, GLFW_KEY_DOWN };

void ResetKeyboardMouseBindings()
{
    g_keyboardBindings = kDefaultKeyboardBindings;
}

void ResetControllerBindings()
{
    g_gamepadBindings = kDefaultGamepadBindings;
}
bool g_fDisableControllerInputWhenUnfocused = false;
bool g_fMouseCamera = false;
float g_mouseCameraSensitivity = 1.0f;
