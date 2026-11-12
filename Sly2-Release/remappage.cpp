// See remappage.h.

#include "remappage.h"
#include "actioninput.h"
#include "region.h"
#include "porttext.h"

#include "clock.h"
#include "display.h"
#include "frontend.h"
#include "gl.h"
#include "hud.h"
#include "pcinput.h"
#include "promptglyph.h"
#include "screenrt.h"
#include "sound.h"

#include "../platform/button_prompts.h"
#include "../platform/install.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

namespace
{
using cane::input::Button;
using cane::input::Family;
namespace input = cane::sly2input;
namespace st = cane::settings;

enum ROWKIND { RK_Advanced, RK_Header, RK_Binding, RK_Reset, RK_Back };
struct ROW
{
    ROWKIND kind = RK_Binding;
    int index = 0;           // RK_Binding: standard 0..PCA_Max + 5 (actions, camera keys, tank tracks) / advanced action
    std::string label;       // (RK_Header: the group)
};

struct REMAP
{
    bool fOpen = false;
    int isel = 0, itop = 0;
    int col = 0;                     // 0 keyboard / mouse, 1 controller
    int irowCapture = -1;            // the row whose binding waits for an input
    bool fCaptureController = false;
    float tCapture = 0;
    float tOpen = 0;
    bool fWaitRelease = false;       // (after a capture: the captured button, still held, is not also a menu press)
};
REMAP s_remap;

// the page's geometry (canvas units, 640 x 448, x past 0 / 640 on a wide picture), set as it is drawn
constexpr float Y_TITLE = 18.0f, Y_HEADER0 = 70.0f, Y_HEADER1 = 100.0f, Y_ROWS = 106.0f, DY_ROW = 29.0f,
                Y_FOOTER = 412.0f;
constexpr int CROW_SHOWN = 10;
constexpr float SIZE_TITLE = 30.0f, SIZE_HEADER = 22.0f, SIZE_ROW = 24.0f, SIZE_HINT = 17.0f;
constexpr float DEPTH = 16760000.0f;   // (over the HUD, as the debug menu)
struct LAYOUT { float x0 = 24, x1 = 616, xKeyboard = 360, xController = 520, wCell = 140; };
LAYOUT s_layout;

bool FAdvanced() { return sly2actions::profile.enabled; }

std::vector<std::string> ActionGroups()
{
    std::vector<std::string> groups;
    for (size_t i = 0; i < input::kActionCount; ++i)
    {
        if (!g_pcinput.fDebugKeys && input::isNoclipAction(input::Action(i))) continue;
        const auto& action = input::kActions[i];
        if (std::find(groups.begin(), groups.end(), action.group) == groups.end()) groups.emplace_back(action.group);
    }
    return groups;
}

std::string StrStandardLabel(int i)
{
    static const PTX aptx[] = {PTX_MoveForward, PTX_MoveBack, PTX_MoveLeft, PTX_MoveRight, PTX_Square, PTX_Circle,
                               PTX_Cross, PTX_Triangle, PTX_Start, PTX_Select, PTX_Max, PTX_Max, PTX_Max, PTX_Max,
                               PTX_Max, PTX_Max, PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown,
                               PTX_TankLeftTrack, PTX_TankRightTrack};
    static const char* const aszButton[] = {"L1", "R1", "L2", "R2", "L3", "R3"};
    if (i < 0 || i >= int(std::size(aptx))) return "";
    return aptx[i] == PTX_Max ? std::string(aszButton[i - PCA_L1]) : StrPort(aptx[i]);
}

std::vector<ROW> Arow()
{
    std::vector<ROW> a;
    if (!sly2actions::profile.readOnly) a.push_back({RK_Advanced, 0, StrPort(PTX_AdvancedBindings)});
    if (FAdvanced())
    {
        for (const std::string& group : ActionGroups())
        {
            a.push_back({RK_Header, 0, StrPortActionGroup(group)});
            for (size_t i = 0; i < input::kActionCount; ++i)
                if (group == input::kActions[i].group) a.push_back({RK_Binding, int(i), StrPortActionName(input::Action(i))});
        }
    }
    else
        for (int i = 0; i < PCA_Max + 6; ++i) a.push_back({RK_Binding, i, StrStandardLabel(i)});
    a.push_back({RK_Reset, 0, StrPort(PTX_RemapReset)});
    a.push_back({RK_Back, 0, StrPort(PTX_RemapBack)});
    return a;
}
bool FSelectable(const ROW& r) { return r.kind != RK_Header; }

// the binding shown in a cell (-2: none possible, the stick's fixed role)
int Binding(const ROW& r, bool controller)
{
    if (r.kind != RK_Binding) return -2;
    if (FAdvanced()) return controller ? sly2actions::profile.gamepad[size_t(r.index)] : sly2actions::profile.keyboard[size_t(r.index)];
    if (r.index < PCA_Max) return controller ? g_pcinput.gamepad[size_t(r.index)] : g_pcinput.keyboard[size_t(r.index)];
    if (controller) return -2;   // (the camera: the right stick; the tank's tracks: the sticks)
    return r.index < PCA_Max + 4 ? g_pcinput.cameraKeys[size_t(r.index - PCA_Max)] : g_pcinput.tankKeys[size_t(r.index - PCA_Max - 4)];
}
// (the camera keys: the right stick; the tank's left / right track: the left / right stick)
Button FixedButton(const ROW& r) { return r.index < PCA_Max + 4 || r.index == PCA_Max + 5 ? Button::RS : Button::LS; }

void Save()
{
    if (FAdvanced()) sly2actions::Save(cane::userDirectory());
    else FSaveDisplaySettings(cane::userDirectory());
}

// a new binding into a row (-1 clears; a standard controller binding returns to its default). Standard: a key / button another action has moves to it (the two swap), as
// the keyboard mapping page's BindKey; the camera keys take no mouse button, the tank's tracks have keys of their own.
void Bind(const ROW& r, bool controller, int binding)
{
    if (r.kind != RK_Binding) return;
    if (FAdvanced())
    {
        if (sly2actions::profile.readOnly) return;
        (controller ? sly2actions::profile.gamepad : sly2actions::profile.keyboard)[size_t(r.index)] = binding;
        Save();
        return;
    }
    PCINPUTOPT& o = g_pcinput;
    if (controller)
    {
        if (r.index >= PCA_Max) return;
        // (a standard controller binding is never empty, settings.h validGamepadBinding: Clear restores the default)
        if (binding < 0) binding = PcInputDefaults().gamepad[size_t(r.index)];
        const int old = o.gamepad[size_t(r.index)];
        if (binding >= 0)
            for (int& b : o.gamepad) if (b == binding) b = old;
        o.gamepad[size_t(r.index)] = binding;
        Save();
        return;
    }
    const int key = binding < 0 ? st::kKeyUnknown : binding;
    if (r.index >= PCA_Max + 4)
    {
        if (st::mouseBinding(key)) return;
        o.tankKeys[size_t(r.index - PCA_Max - 4)] = key;
        Save();
        return;
    }
    if (r.index >= PCA_Max && st::mouseBinding(key)) return;
    int* pkey = r.index < PCA_Max ? &o.keyboard[size_t(r.index)] : &o.cameraKeys[size_t(r.index - PCA_Max)];
    const int old = *pkey;
    if (key != st::kKeyUnknown)
    {
        for (int& k : o.keyboard) if (k == key) k = old;
        for (int& k : o.cameraKeys) if (k == key) k = st::mouseBinding(old) ? st::kKeyUnknown : old;
    }
    *pkey = key;
    Save();
}

void ResetDefaults()
{
    if (FAdvanced())
    {
        if (sly2actions::profile.readOnly) return;
        const bool enabled = sly2actions::profile.enabled;
        sly2actions::profile = input::defaults();
        sly2actions::profile.enabled = enabled;
    }
    else
    {
        const PCINPUTOPT d = PcInputDefaults();
        g_pcinput.keyboard = d.keyboard;
        g_pcinput.gamepad = d.gamepad;
        g_pcinput.cameraKeys = d.cameraKeys;
        g_pcinput.tankKeys = d.tankKeys;
    }
    Save();
}

// the family the controller column shows: the prompt style / Original PlayStation Icons, else the pad used last or
// connected, else Xbox
Family PadFamily()
{
    if (g_pcinput.fAlwaysOriginalPs) return Family::PlayStation;
    if (g_pcinput.promptStyle > 0) return cane::input::familyForStyle(cane::input::PromptStyle(g_pcinput.promptStyle));
    const Family f = cane::input::tracker().family();
    if (f != Family::Keyboard) return f;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
        if (glfwJoystickPresent(jid) && glfwJoystickIsGamepad(jid) && !cane::input::ignoredSteamDuplicate(jid))
            return cane::input::identify(jid).family;
    return Family::Xbox;
}

void Clamp(const std::vector<ROW>& arow)
{
    const int c = int(arow.size());
    s_remap.isel = std::clamp(s_remap.isel, 0, c - 1);
    if (!FSelectable(arow[size_t(s_remap.isel)])) s_remap.isel = std::min(c - 1, s_remap.isel + 1);
    if (s_remap.isel < s_remap.itop) s_remap.itop = s_remap.isel;
    if (s_remap.isel >= s_remap.itop + CROW_SHOWN) s_remap.itop = s_remap.isel - CROW_SHOWN + 1;
    // (a group's heading stays in view above its first row)
    if (s_remap.itop > 0 && s_remap.itop == s_remap.isel && arow[size_t(s_remap.itop - 1)].kind == RK_Header) --s_remap.itop;
    s_remap.itop = std::clamp(s_remap.itop, 0, std::max(0, c - CROW_SHOWN));
}
void Step(const std::vector<ROW>& arow, int d)
{
    const int c = int(arow.size());
    int i = s_remap.isel;
    do i = (i + d + c) % c;
    while (!FSelectable(arow[size_t(i)]) && i != s_remap.isel);
    s_remap.isel = i;
    PsndStartSound(0x35);
}

void StartCapture(int irow, bool controller)
{
    s_remap.irowCapture = irow;
    s_remap.fCaptureController = controller;
    s_remap.tCapture = g_clock.tReal;
    if (g_gl.window)
    {
        KeyBindingPressed(g_gl.window);   // (primed: the key / click that chose the cell is not taken)
        sly2actions::CaptureController(g_gl.window);
    }
}

void Activate(const std::vector<ROW>& arow, int irow)
{
    const ROW& r = arow[size_t(irow)];
    switch (r.kind)
    {
    case RK_Advanced:
        sly2actions::profile.enabled = !sly2actions::profile.enabled;
        sly2actions::Save(cane::userDirectory());
        s_remap.isel = 0;
        s_remap.itop = 0;
        break;
    case RK_Reset: ResetDefaults(); break;
    case RK_Back: CloseRemapPage(); return;
    case RK_Binding:
        if (Binding(r, s_remap.col == 1) == -2) return;
        if (FAdvanced() && sly2actions::profile.readOnly) return;
        StartCapture(irow, s_remap.col == 1);
        break;
    default: return;
    }
    PsndStartSound(0x36);
}

// ---- drawing
glm::vec4 Rgb(int r, int g, int b, int a = 0) { return glm::vec4(float(r), float(g), float(b), float(a)); }
void Text(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float x, float y, float size, glm::vec4 off,
          int halign = 0, float wBox = 0.0f)
{
    g_hud.DrawTextBox(font, picons, text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, wBox, 0.0f, halign, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}
float Dx(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size)
{
    return DxTextUi(font, picons, text, size / 51.2f);
}
void Rect(float x0, float y0, float x1, float y1, glm::vec4 rgba, float dz = 0.0f)
{
    g_hud.FillScreenRect(x0, y0, x1, y1, rgba, (DEPTH + dz) / 16777216.0f);
}
// an icon h canvas units high, centred at (cx, cy) (the canvas's pixels are 480 / 448 as wide as high)
void Icon(unsigned tex, const cane::input::IconUV& uv, float cx, float cy, float h, float alpha = 128.0f)
{
    if (!tex || !uv.valid) return;
    const float hx = 0.5f * h * (480.0f / 448.0f);
    g_hud.DrawTexQuad(tex, cx - hx, cy - 0.5f * h, cx + hx, cy + 0.5f * h, uv.u0, uv.v0, uv.u1, uv.v1,
                      glm::vec4(128.0f, 128.0f, 128.0f, alpha), (DEPTH + 200.0f) / 16777216.0f);
}

// a cell: the keycap / the button's icon, its name when there is no icon (or a text for unbound / fixed)
void Cell(const FONTBRX& font, const FONTBRX* picons, const ROW& r, bool controller, float cx, float cy, bool fSel, Family family)
{
    const auto& atlas = cane::input::promptAtlas();
    const int binding = Binding(r, controller);
    const glm::vec4 off = fSel ? Rgb(120, 104, 56) : Rgb(64, 64, 64);
    const float h = 24.0f;
    if (binding == -2)
    {
        const Button b = FixedButton(r);
        Icon(atlas.texture(family), atlas.uv(family, b), cx - 30.0f, cy, h, 72.0f);
        Text(font, picons, StrPort(PTX_RemapFixed), cx - 14.0f, cy - 9.0f, SIZE_HINT, Rgb(44, 44, 44));
        return;
    }
    if (binding < 0 || (!controller && binding == st::kKeyUnknown))
    {
        Text(font, picons, StrPort(PTX_KeyUnbound), cx, cy - 10.0f, SIZE_HINT + 2.0f, Rgb(44, 44, 44), 1, 0.0f);
        return;
    }
    if (controller)
    {
        const Button b = cane::input::fromGlfwBinding(binding);
        const auto uv = atlas.uv(family, b);
        if (atlas.texture(family) && uv.valid) Icon(atlas.texture(family), uv, cx, cy, h + 4.0f);
        else Text(font, picons, StrToGameText(cane::input::buttonLabel(b, family)), cx, cy - 10.0f, SIZE_ROW - 2.0f, off, 1, 0.0f);
        return;
    }
    const auto uv = atlas.keyUV(cane::input::promptKeyForBinding(binding));
    if (atlas.keyboardTexture() && uv.valid) Icon(atlas.keyboardTexture(), uv, cx, cy, h + 4.0f);
    else Text(font, picons, StrPortKeyName(binding), cx, cy - 10.0f, SIZE_ROW - 4.0f, off, 1, 0.0f);
}
}  // namespace

bool FRemapPageOpen() { return s_remap.fOpen; }

void OpenRemapPage()
{
    s_remap = REMAP{};
    s_remap.fOpen = true;
    s_remap.tOpen = g_clock.tReal;
    s_remap.col = PromptKeyboardActive() ? 0 : 1;
    const std::vector<ROW> arow = Arow();
    s_remap.isel = 0;
    while (s_remap.isel < int(arow.size()) && arow[size_t(s_remap.isel)].kind != RK_Binding) ++s_remap.isel;
    Clamp(arow);
    ClearJoySteps();
}

void CloseRemapPage()
{
    if (!s_remap.fOpen) return;
    s_remap.fOpen = false;
    s_remap.irowCapture = -1;
    PsndStartSound(0x37);
    ClearJoySteps();
}

void UpdateRemapPage()
{
    if (!s_remap.fOpen) return;
    std::vector<ROW> arow = Arow();
    Clamp(arow);
    if (s_remap.irowCapture >= 0)
    {
        GrfbtnTakePressed(0xFFFF);
        ClearJoySteps();
        const int key = g_gl.window ? KeyBindingPressed(g_gl.window) : GLFW_KEY_ESCAPE;
        const bool timeout = s_remap.fCaptureController && g_clock.tReal - s_remap.tCapture > 6.0f;
        if (key == GLFW_KEY_ESCAPE || timeout || s_remap.irowCapture >= int(arow.size()))
        {
            if (std::getenv("SLY2N_REMAPLOG")) std::printf("remap: capture ends (key %d, timeout %d)\n", key, int(timeout));
            s_remap.irowCapture = -1;
            s_remap.fWaitRelease = true;
            return;
        }
        const ROW& r = arow[size_t(s_remap.irowCapture)];
        if (key == GLFW_KEY_DELETE)
        {
            Bind(r, s_remap.fCaptureController, -1);
            s_remap.irowCapture = -1;
            s_remap.fWaitRelease = true;
            return;
        }
        const int binding = s_remap.fCaptureController ? (g_gl.window ? sly2actions::CaptureController(g_gl.window) : -1) : key;
        if (binding >= 0 && std::getenv("SLY2N_REMAPLOG"))
            std::printf("remap: row %d %s <- %d\n", s_remap.irowCapture, s_remap.fCaptureController ? "pad" : "key", binding);
        if (binding >= 0)
        {
            Bind(r, s_remap.fCaptureController, binding);
            s_remap.irowCapture = -1;
            s_remap.fWaitRelease = true;
            PsndStartSound(0x36);
        }
        return;
    }
    if (s_remap.fWaitRelease)
    {
        GrfbtnTakePressed(0xFFFF);
        ClearJoySteps();
        if (GrfbtnHeld() & 0xFFFF) return;
        s_remap.fWaitRelease = false;
    }
    // Triangle / Start: back
    if (GrfbtnTakePressed(BtnMenuBack() | 0x800)) { CloseRemapPage(); return; }
    // the mouse: hover selects the row and the column, a click changes
    if (float xc = 0, yc = 0; FMenuCursor(&xc, &yc))
    {
        const LAYOUT& l = s_layout;
        if (xc >= l.x0 && xc < l.x1 && yc >= Y_ROWS && yc < Y_ROWS + DY_ROW * CROW_SHOWN)
        {
            const int i = s_remap.itop + int((yc - Y_ROWS) / DY_ROW);
            if (i < int(arow.size()) && FSelectable(arow[size_t(i)]))
            {
                SetMenuMouseTarget();
                const int col = xc >= (l.xKeyboard + l.xController) * 0.5f ? 1 : 0;
                if (FMenuCursorMoved() || FMenuClick())
                {
                    if (i != s_remap.isel) PsndStartSound(0x35);
                    s_remap.isel = i;
                    s_remap.col = col;
                }
                if (FMenuClick()) { Activate(arow, i); return; }
            }
        }
    }
    if (const int d = NStepUpDown(); d != 0) Step(arow, d);
    if (const int d = NStepLeftRight(); d != 0 && arow[size_t(s_remap.isel)].kind == RK_Binding)
    {
        const int col = std::clamp(s_remap.col + d, 0, 1);
        if (col != s_remap.col) { s_remap.col = col; PsndStartSound(0x35); }
    }
    if (GrfbtnTakePressed(BtnMenuConfirm())) { Activate(arow, s_remap.isel); return; }
    if (GrfbtnTakePressed(BtnMenuErase()) && arow[size_t(s_remap.isel)].kind == RK_Binding &&
        Binding(arow[size_t(s_remap.isel)], s_remap.col == 1) != -2)
    {
        Bind(arow[size_t(s_remap.isel)], s_remap.col == 1, -1);
        PsndStartSound(0x37);
    }
    GrfbtnTakePressed(0xFFFF);   // (nothing else reaches the menu under the page)
    arow = Arow();
    Clamp(arow);
}

void DrawRemapOverlay(int width, int height)
{
    if (!s_remap.fOpen) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const std::vector<ROW> arow = Arow();
    Clamp(arow);
    g_hud.Begin();
    // the picture's canvas extent (wider than 640 on a wide picture): the page uses it, up to 1.6 x the 4:3 width
    const float xLeft = ((-1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
    const float xRight = ((1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
    const float xMid = 320.0f, halfW = std::min(std::max(296.0f, (xRight - xLeft) * 0.5f - 40.0f), 512.0f);
    LAYOUT& l = s_layout;
    l.x0 = xMid - halfW;
    l.x1 = xMid + halfW;
    l.wCell = std::clamp((l.x1 - l.x0) * 0.22f, 120.0f, 200.0f);
    l.xController = l.x1 - 20.0f - l.wCell * 0.5f;
    l.xKeyboard = l.xController - l.wCell;
    // Job Help's look: the black page over the picture, the dark blue title and column bands with their light rules,
    // the blue bottom band
    Rect(xLeft - 2.0f, -2.0f, xRight + 2.0f, 450.0f, Rgb(0, 0, 0, 104), -400.0f);
    Rect(l.x0, 0.0f, l.x1, 448.0f, Rgb(0, 0, 0, 128), -300.0f);
    Rect(l.x0, 0.0f, l.x1, Y_HEADER0 - 4.0f, Rgb(0, 12, 26, 128), -200.0f);
    Rect(l.x0, Y_HEADER0 - 4.0f, l.x1, Y_HEADER0 - 2.0f, Rgb(40, 56, 72, 128), -150.0f);
    Rect(l.x0, Y_HEADER0, l.x1, Y_HEADER1, Rgb(0, 12, 26, 128), -200.0f);
    Rect(l.x0, Y_HEADER1, l.x1, Y_HEADER1 + 3.0f, Rgb(28, 50, 74, 128), -150.0f);
    Rect(l.x0, Y_FOOTER, l.x1, 448.0f, Rgb(16, 34, 54, 128), -200.0f);
    Rect(l.x0, Y_FOOTER, l.x1, Y_FOOTER + 3.0f, Rgb(28, 50, 74, 128), -150.0f);
    const glm::vec4 offTitle = Rgb(100, 50, 2), offHead = Rgb(42, 56, 70), offRow = Rgb(62, 62, 62), offSel = Rgb(122, 104, 52),
                    offGroup = Rgb(70, 52, 20), offDim = Rgb(40, 40, 40);
    Text(*pfont, picons, StrPort(PTX_RemapTitle), l.x0 + 20.0f, Y_TITLE, SIZE_TITLE, offTitle);
    const Family family = PadFamily();
    Text(*pfont, picons, StrToGameText(cane::input::familyDisplayName(family)), l.x1 - 20.0f, Y_TITLE + 8.0f, SIZE_HINT, offHead, 2, 0.0f);
    Text(*pfont, picons, StrPort(PTX_RemapAction), l.x0 + 30.0f, Y_HEADER0 + 3.0f, SIZE_HEADER, offHead);
    for (int c = 0; c < 2; ++c)   // (the column heads fit their columns: smaller in the 4:3 picture and long languages)
    {
        const std::string head = StrPort(c ? PTX_RemapController : PTX_RemapKeyboard);
        float size = SIZE_HINT + 1.0f;
        while (size > 11.0f && Dx(*pfont, picons, head, size) > l.wCell - 8.0f) size -= 1.0f;
        Text(*pfont, picons, head, c ? l.xController : l.xKeyboard, Y_HEADER0 + 5.0f + (SIZE_HINT + 1.0f - size) * 0.5f, size,
             offHead, 1, 0.0f);
    }
    const int crow = std::min<int>(CROW_SHOWN, int(arow.size()) - s_remap.itop);
    for (int k = 0; k < crow; ++k)
    {
        const int i = s_remap.itop + k;
        const ROW& r = arow[size_t(i)];
        const float y = Y_ROWS + DY_ROW * float(k), cy = y + DY_ROW * 0.5f;
        const bool fSel = i == s_remap.isel;
        if (fSel)   // (Job Help's selection bar)
        {
            Rect(l.x0 + 10.0f, y + 1.0f, l.x1 - 10.0f, y + DY_ROW - 1.0f, Rgb(26, 26, 26, 128), -100.0f);
            if (r.kind == RK_Binding)
            {
                const float xc = s_remap.col ? l.xController : l.xKeyboard;
                Rect(xc - l.wCell * 0.45f, y + 2.0f, xc + l.wCell * 0.45f, y + DY_ROW - 2.0f, Rgb(46, 46, 46, 128), -90.0f);
            }
        }
        switch (r.kind)
        {
        case RK_Header:
            Text(*pfont, picons, r.label, l.x0 + 20.0f, y + 5.0f, SIZE_HEADER, offGroup);
            break;
        case RK_Advanced:
            Text(*pfont, picons, r.label + StrPort(PTX_Sep) + StrPort(FAdvanced() ? PTX_On : PTX_Off), l.x0 + 30.0f, y + 3.0f,
                 SIZE_ROW, fSel ? offSel : offRow);
            break;
        case RK_Reset:
        case RK_Back:
            Text(*pfont, picons, r.label, l.x0 + 30.0f, y + 3.0f, SIZE_ROW, fSel ? offSel : offRow);
            break;
        case RK_Binding:
        {
            const float wLabel = l.xKeyboard - l.wCell * 0.5f - (l.x0 + 30.0f) - 8.0f;
            std::string label = r.label;
            float size = SIZE_ROW;
            while (size > 15.0f && Dx(*pfont, picons, label, size) > wLabel) size -= 1.0f;
            Text(*pfont, picons, label, l.x0 + 30.0f, y + 3.0f + (SIZE_ROW - size) * 0.5f, size, fSel ? offSel : offRow);
            for (int c = 0; c < 2; ++c)
            {
                const float xc = c ? l.xController : l.xKeyboard;
                if (s_remap.irowCapture == i && s_remap.fCaptureController == (c == 1))
                {
                    const bool blink = std::fmod(g_clock.tReal, 0.8f) < 0.5f;
                    Text(*pfont, picons, "...", xc, y + 2.0f, SIZE_ROW, blink ? offSel : offDim, 1, 0.0f);
                }
                else Cell(*pfont, picons, r, c == 1, xc, cy, fSel && s_remap.col == c, family);
            }
            break;
        }
        }
    }
    // the scroll position: arrows at the rows' right edge
    if (s_remap.itop > 0) Text(*pfont, picons, "^", l.x1 - 10.0f, Y_ROWS + 2.0f, SIZE_HINT, offHead, 2, 0.0f);
    if (s_remap.itop + CROW_SHOWN < int(arow.size()))
        Text(*pfont, picons, "v", l.x1 - 10.0f, Y_ROWS + DY_ROW * CROW_SHOWN - 18.0f, SIZE_HINT, offHead, 2, 0.0f);
    // the bottom band: the capture line, or the buttons
    std::string hint;
    if (s_remap.irowCapture >= 0) hint = StrPort(s_remap.fCaptureController ? PTX_RemapPressButton : PTX_RemapPressKey);
    else
    {
        const char chErase = BtnMenuErase() == 0x80 ? 'S' : BtnMenuErase() == 0x40 ? 'X' : 'O';
        const char chConfirm = BtnMenuConfirm() == 0x20 ? 'O' : 'X';
        hint = std::string("&2") + chConfirm + "&. " + StrPort(PTX_RemapChange) + "      &2" + chErase + "&. " +
               StrPort(PTX_RemapClear) + "      " + SzMenuBackGlyph() + " " + StrPort(PTX_RemapBack);
    }
    char ach[32];
    std::snprintf(ach, sizeof(ach), "%d / %d", s_remap.isel + 1, int(arow.size()));
    Text(*pfont, picons, hint, l.x0 + 24.0f, Y_FOOTER + 9.0f, SIZE_HINT, offHead);
    Text(*pfont, picons, ach, l.x1 - 20.0f, Y_FOOTER + 9.0f, SIZE_HINT, offHead, 2, 0.0f);
    g_hud.Flush(width, height);
}
