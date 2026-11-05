// See menu.h. Reference: the original's code (disassembly) and the decomp: m_1AE928.cpp's 0x1AE928 / 0x1AEAA8 /
// 0x1AEEE0 / 0x1AF660 / 0x1B01E8 (Sly2-Reference), func_001AF390.c / func_001AF4D8.c / func_001AF490.c /
// func_001AF608.c / func_001AF6A0.c / func_001AF6D0.c / func_001AF9C8.cpp / func_001AFB88 / func_001AFF88.c /
// func_001B0078.c / func_001B0108.c / func_001B0160.c / func_001B0270.c / func_001B0668.c / func_001B06D8.c /
// func_001B0720.c / func_001AD168.c / func_001AD250.c / func_001AD320.c / func_001AD3E0.c / func_001AD480.c /
// func_001AD538.c / func_001AD740.c / func_001ADA20.c / func_001ADBB8.c, and the code of 0x1B0290, 0x1B04E0,
// 0x1B0688, 0x1B0740, 0x1B0808, 0x1B0878, 0x1B0970, 0x1B09A8, 0x1AFFC0, 0x1ACFE0, 0x1ADC40; memcard.cpp's
// 0x1ACB58 / 0x1ACC80 / 0x1ACCC8 / 0x1AD7E0 / 0x1AD820.

#include "menu.h"
#include "pcinput.h"
#include "sound.h"

#include "clock.h"
#include "cls.h"
#include "disc.h"
#include "display.h"
#include "frontend.h"
#include "gl.h"
#include "hudwidget.h"
#include "po.h"
#include "ref.h"
#include "savedata.h"
#include "screenrt.h"
#include "shop.h"
#include "splayer.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "../platform/controller_family.h"
#include "../platform/platform.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
int32_t NElf(uint32_t va) { int32_t n = 0; if (const uint8_t* pb = g_disc.PbElf(va, 4)) std::memcpy(&n, pb, 4); return n; }
float GRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? float(r.m_n) : 0.0f; }
int NRef(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }

// the settings (orig SW +0x3B2C.. 11 keys: "flags" <name>)
CRef RefSetting(const char* name) { return RefGetProgress({"flags", name}); }
void SetSettingF(const char* name, float g) { CRef r; r.SetF32(g); FSetProgress({"flags", name}, r); }
void SetSettingN(const char* name, int n) { CRef r; r.SetS32(n); FSetProgress({"flags", name}, r); }
void SetSettingB(const char* name, bool f) { CRef r; r.SetBool(f); FSetProgress({"flags", name}, r); }

// ------------------------------------------------------------ the entries (0x20 bytes in the executable)
// The PC display options: native entries (ids above the strings') cycling display.h's options.
enum NATIVEID : int
{
    NID_Base = 0x10000,
    NID_Aspect = NID_Base, NID_Resolution, NID_Msaa, NID_FrameRate, NID_Window, NID_Vsync, NID_DrawDistance,
    NID_ExitToDesktop, NID_DisplayPage,
    // (Phase 3, the input QoL) the controls page: pcinput.h / promptglyph.h / skipcut.h options
    NID_ControlsPage, NID_PromptStyle, NID_AlwaysPs, NID_InstantSkip, NID_MouseCamera, NID_MouseSens, NID_AimSens,
    NID_InvertX, NID_InvertY, NID_WheelZoom, NID_MouseMenus, NID_HackMouse, NID_KeyboardPage, NID_ResetKeys,
    NID_KeyFirst,   // .. + PCA_Max + 4: the keyboard mapping's rows (the actions, then the camera keys)
};
constexpr uint32_t PFN_NativeDisplay = 0xFFFF0001;   // (a native select function)
constexpr uint32_t PFN_NativeExit = 0xFFFF0002;
constexpr uint32_t PFN_NativeDisplayPage = 0xFFFF0003;   // (the PC display options' own page)
constexpr uint32_t PFN_NativeControls = 0xFFFF0004;      // (a controls option: cycled)
constexpr uint32_t PFN_NativeControlsPage = 0xFFFF0005;
constexpr uint32_t PFN_NativeKeyboardPage = 0xFFFF0006;
constexpr uint32_t PFN_NativeKey = 0xFFFF0007;           // (a key row: the next key pressed binds)

struct MENUENT
{
    int id = 0;              // +0x00 the string (cycled options: the value's string)
    uint32_t pfnSelect = 0;  // +0x04 (Cross)
    int n = 0;               // +0x08
    uint32_t vaTab = 0;      // +0x0C the page it opens / the option's values
    uint32_t pfnUpdate = 0;  // +0x10 (every frame)
    uint32_t pfnInit = 0;    // +0x14 (when the page opens)
    uint32_t vaAnim = 0;     // +0x18 the page's animations
    uint32_t grf = 0;        // +0x1C (1 the page goes back after it, 2 handled)
};

std::vector<MENUENT> AentFromElf(uint32_t va, int c)
{
    std::vector<MENUENT> a;
    for (int i = 0; i < c; ++i)
    {
        const uint32_t v = va + uint32_t(i) * 0x20;
        MENUENT e;
        e.id = NElf(v);
        e.pfnSelect = uint32_t(NElf(v + 4));
        e.n = NElf(v + 8);
        e.vaTab = uint32_t(NElf(v + 0xC));
        e.pfnUpdate = uint32_t(NElf(v + 0x10));
        e.pfnInit = uint32_t(NElf(v + 0x14));
        e.vaAnim = uint32_t(NElf(v + 0x18));
        e.grf = uint32_t(NElf(v + 0x1C));
        a.push_back(e);
    }
    return a;
}

// ------------------------------------------------------------ the menu (orig 0x3D9ED0)
struct PAGE
{
    std::vector<MENUENT> aent;   // +0x00 c / +0x04
    uint32_t vaTab = 0;          // the entries' table (the data access pages' identity, 0x1B0078)
    LO* plo = nullptr;           // +0x08
    int fReplaced = 0;           // +0x0C (a yes / no page: Triangle goes back through state 3)
    int isel = 0;                // +0x14
    uint32_t vaAnim = 0;         // +0x18 the animations (8 ids)
};
struct MENU
{
    std::array<PAGE, 17> apage;  // +0x0C + i * 0x1C (1..16)
    int depth = 0;               // +0x24
    int state = 0;               // +0x1E8
    int pending = -1;            // +0x1EC
    LO* ploDefault = nullptr;    // +0x20 (0x370)
    bool fShown = false;         // in the HUD's list (0x3D9E70)
};
MENU s_menu;
SW* s_psw = nullptr;
int s_ikeyCapture = -1;   // (the keyboard mapping's row waiting for a key)

PAGE* PpageCur() { return s_menu.depth > 0 ? &s_menu.apage[size_t(s_menu.depth)] : nullptr; }
int IdAnim(const PAGE& pg, uint32_t ib) { return pg.vaAnim ? NElf(pg.vaAnim + ib) : -1; }

// ------------------------------------------------------------ the card dialog (orig 0x2E6550 on 0x52C)
struct CARDDLG
{
    LO* plo = nullptr;   // +0x24
    int msg = -1;        // +0x40
    int nAfter = 0;      // +0x4C: after the save 1 the boot again, 2 the menu closes, 3 an auto-save
    bool fShown = false;
    float tShown = 0;
};
CARDDLG s_card;
float s_tCardState = 0;   // orig g_save +0x10 (the card state's time)
int s_islotTarget = -1;  // orig g_save +0xFD58 (the slot a save / an erase goes to)

void CloseCard()
{
    // orig 0x001ACBB8 (+ its widget's slot 11: out of the HUD's list)
    s_card.msg = -1;
    s_card.fShown = false;
    if (s_card.plo) HideScreenObj(s_card.plo);
}

// orig 0x001ACC80
int NCardPrompt(int msg)
{
    switch (msg)
    {
    case 0x11: case 0x14: case 0x17: case 0x18: case 0x1B: case 0x1D: case 0x22: case 0x23: case 0x24: case 0x2E: return 1;
    case 0x12: case 0x15: case 0x1C: case 0x20: return 2;
    default: return 0;
    }
}

// ------------------------------------------------------------ the menu's states
void SetMenuState(int st);

// orig 0x001AE9E8 / 0x001AEA40 (the menu's vt slots 10 / 11): in the HUD's list with the game paused, out
void ShowMenuWidget()
{
    if (s_menu.fShown) return;
    s_menu.fShown = true;
    SetClockFactor(&g_clock, 0, 0.0f);
    if (s_psw) BroadcastTaskEvent(s_psw, 0x28, nullptr);
}
void HideMenuWidget()
{
    s_menu.fShown = false;
    s_menu.depth = 0;
    SetClockFactor(&g_clock, 0, 1.0f);
    if (s_psw) BroadcastTaskEvent(s_psw, 0x2C, nullptr);
}

// orig 0x001AEAA8
void SetMenuState(int st)
{
    const int old = s_menu.state;
    if (old == st) return;
    PAGE* pe = PpageCur();
    switch (old)
    {
    case 5:
        if (pe && !pe->aent.empty()) pe->isel = (pe->isel + 1) % int(pe->aent.size());
        break;
    case 6:
        if (pe && !pe->aent.empty()) pe->isel = pe->isel - 1 < 0 ? int(pe->aent.size()) - 1 : pe->isel - 1;
        break;
    case 0: ShowMenuWidget(); break;
    default: break;
    }
    s_menu.state = st;
    if (st >= 12 || !pe || !pe->plo) { if (st == 0) HideMenuWidget(); return; }
    LO* plo = pe->plo;
    switch (st)
    {
    case 0: HideMenuWidget(); break;
    case 1:
        ShowScreenObj(plo, IdAnim(*pe, 0x0));
        SelectScreenObjPart(plo, IdAnim(*pe, 0xC));
        break;
    case 2:
    case 8:
        ShowScreenObj(plo, IdAnim(*pe, 0x8));
        SelectScreenObjPart(plo, IdAnim(*pe, 0x14));
        break;
    case 3: SelectScreenObjPart(plo, IdAnim(*pe, 0x14)); break;
    case 4: SelectScreenObjPart(plo, IdAnim(*pe, 0xC)); break;
    case 5: SelectScreenObjPart(plo, IdAnim(*pe, 0x18)); break;
    case 6: SelectScreenObjPart(plo, IdAnim(*pe, 0x1C)); break;
    case 7:
        ShowScreenObj(plo, IdAnim(*pe, 0x4));
        SelectScreenObjPart(plo, IdAnim(*pe, 0x10));
        break;
    case 9: SelectScreenObjPart(plo, 0x332); break;
    case 10:
    {
        const MENUENT& e = pe->aent[size_t(pe->isel)];
        switch (e.id)
        {
        case 0x67:
            SelectScreenObjPart(plo, 0x333);
            SelectScreenObjPart(plo, 0x315);
            SetScreenObjFraction(plo, 0x315, 1.0f - GRef(RefSetting("uMusicVolume")));
            break;
        case 0x68:
            SelectScreenObjPart(plo, 0x334);
            SelectScreenObjPart(plo, 0x315);
            SetScreenObjFraction(plo, 0x315, 1.0f - GRef(RefSetting("uEffectsVolume")));
            break;
        case 0x3F: SelectScreenObjPart(plo, 0x335); break;
        default: break;
        }
        break;
    }
    case 11: SelectScreenObjPart(plo, 0x331); break;
    default: break;
    }
}

// orig 0x001AF660
void CloseMenu()
{
    if (s_menu.state != 7) return;
    if (s_menu.depth >= 2) s_menu.depth = 1;
    SetMenuState(8);
}

// orig 0x001B0078 / 0x001B0108: the page of the slots (0x1E new game, 0x1F load, 0x19 save)
int KindDataAccess()
{
    if (!s_menu.fShown) return -1;
    const PAGE* pe = PpageCur();
    if (!pe) return -1;
    if (pe->vaTab == 0x2BFDA8) return 0x1E;
    if (pe->vaTab == 0x2BFE08) return 0x1F;
    if (pe->vaTab == 0x2BFE68) return 0x19;
    return -1;
}
bool FDataAccess() { const int k = KindDataAccess(); return k == 0x19 || k == 0x1E || k == 0x1F; }

void InitEntries(PAGE& pg);

// orig 0x001AF4D8 (push a page)
void PushPage(std::vector<MENUENT> aent, uint32_t vaTab, LO* plo, uint32_t vaAnim)
{
    if (!plo && s_menu.depth > 0) plo = s_menu.apage[size_t(s_menu.depth)].plo;
    if (s_menu.depth >= 16) return;
    PAGE& pg = s_menu.apage[size_t(++s_menu.depth)];
    pg = PAGE{};
    pg.aent = std::move(aent);
    pg.vaTab = vaTab;
    pg.plo = plo;
    pg.vaAnim = vaAnim;
    SetMenuState(1);
    InitEntries(pg);
    if (std::getenv("SLY2N_MENULOG"))
    {
        std::printf("menu: page 0x%X (selected %d):", vaTab, pg.isel);
        for (const MENUENT& e : pg.aent) std::printf(" 0x%X", e.id);
        std::printf("\n");
    }
    if (FDataAccess())
    {
        s_menu.pending = s_menu.depth;
        --s_menu.depth;
    }
}

// orig 0x001AF390 (a yes / no page in place of the current one's)
void ReplacePage(std::vector<MENUENT> aent, uint32_t vaTab)
{
    SetMenuState(3);
    const int k = s_menu.depth;
    LO* plo = nullptr;
    uint32_t vaAnim = 0;
    if (k > 0)
    {
        plo = s_menu.apage[size_t(k)].plo;
        vaAnim = s_menu.apage[size_t(k)].vaAnim;
    }
    s_menu.pending = k + 1;
    PAGE& pg = s_menu.apage[size_t(k + 1)];
    pg = PAGE{};
    pg.aent = std::move(aent);
    pg.vaTab = vaTab;
    pg.plo = plo;
    pg.fReplaced = 1;
    pg.vaAnim = vaAnim;
    InitEntries(pg);
    if (std::getenv("SLY2N_MENULOG"))
    {
        std::printf("menu: page 0x%X (replaced, selected %d):", vaTab, pg.isel);
        for (const MENUENT& e : pg.aent) std::printf(" 0x%X", e.id);
        std::printf("\n");
    }
}

// orig 0x001AF490 / 0x001AF608 / 0x001AF6A0
void BackReplaced()
{
    if (s_menu.state != 7) return;
    SetMenuState(3);
    s_menu.pending = s_menu.depth - 1;
}
void PopPage()
{
    if (s_menu.state != 7) return;
    SetMenuState(2);
    if (FDataAccess()) s_menu.pending = s_menu.depth = s_menu.depth - 1;
}
void StartSlider()
{
    if (s_menu.state == 7) SetMenuState(11);
}

// ------------------------------------------------------------ the options (0x1AF9C8 / 0x1AFB88 / 0x1AF6D0)
const char* const s_aszAspect[] = {"Fit to Screen", "4:3", "16:9", "16:10", "4:3 (PS2)", "16:9 (PS2)"};
std::string StrNative(int id)
{
    char ach[96];
    switch (id)
    {
    case NID_Aspect: std::snprintf(ach, sizeof(ach), "Aspect Ratio: %s", s_aszAspect[std::clamp(int(g_display.aspect), 0, 5)]); return ach;
    case NID_Resolution:
        if (g_display.internalHeight == 0) std::snprintf(ach, sizeof(ach), "Resolution: Native (%d x %d)", g_display.renderWidth, g_display.renderHeight);
        else std::snprintf(ach, sizeof(ach), "Resolution: %d x %d", g_display.renderWidth, g_display.renderHeight);
        return ach;
    case NID_Msaa:
        if (g_display.msaaSamples > 1) std::snprintf(ach, sizeof(ach), "MSAA: %dx", g_display.msaaSamples);
        else std::snprintf(ach, sizeof(ach), "MSAA: Off");
        return ach;
    case NID_FrameRate: std::snprintf(ach, sizeof(ach), "Frame Rate: %d FPS", g_display.frameRate); return ach;
    case NID_Window:
    {
        static const char* const asz[] = {"Windowed", "Borderless", "Fullscreen"};
        std::snprintf(ach, sizeof(ach), "Window Mode: %s", asz[std::clamp(int(g_display.window), 0, 2)]);
        return ach;
    }
    case NID_Vsync: return g_display.fVsync ? "VSync: On" : "VSync: Off";
    case NID_DrawDistance: std::snprintf(ach, sizeof(ach), "Draw Distance: %.2fx", g_display.drawDistance); return ach;
    case NID_ExitToDesktop: return "Exit to Desktop";
    case NID_DisplayPage: return "Display Options...";
    case NID_ControlsPage: return "Controls and Prompts...";
    case NID_PromptStyle:
    {
        static const char* const asz[] = {"Auto", "PlayStation", "Xbox", "Nintendo", "Steam Deck", "Steam Controller", "PlayStation 4", "PlayStation 5", "Steam Controller (2026)"};
        std::snprintf(ach, sizeof(ach), "Button Icons: %s", asz[std::clamp(g_pcinput.promptStyle, 0, cane::input::kPromptStyleLast)]);
        return ach;
    }
    case NID_AlwaysPs: return g_pcinput.fAlwaysOriginalPs ? "Original PlayStation Icons: On" : "Original PlayStation Icons: Off";
    case NID_InstantSkip: return g_pcinput.fInstantSkipCutscenes ? "Instant Skip Cutscenes: On" : "Instant Skip Cutscenes: Off";
    case NID_MouseCamera: return g_pcinput.fMouseCamera ? "Mouse Camera: On" : "Mouse Camera: Off";
    case NID_MouseSens: std::snprintf(ach, sizeof(ach), "Mouse Sensitivity: %d%%", g_pcinput.mouseSensitivity); return ach;
    case NID_AimSens: std::snprintf(ach, sizeof(ach), "Mouse Aim Sensitivity: %d%%", g_pcinput.mouseAimSensitivity); return ach;
    case NID_InvertX: return g_pcinput.fMouseInvertX ? "Invert Mouse X: On" : "Invert Mouse X: Off";
    case NID_InvertY: return g_pcinput.fMouseInvertY ? "Invert Mouse Y: On" : "Invert Mouse Y: Off";
    case NID_WheelZoom: return g_pcinput.fMouseWheelZoom ? "Wheel Zooms Binocucom: On" : "Wheel Zooms Binocucom: Off";
    case NID_MouseMenus: return g_pcinput.fMouseMenus ? "Mouse in Menus: On" : "Mouse in Menus: Off";
    case NID_HackMouse: return g_pcinput.fHackMouseAim ? "Mouse Aim in Hacking: On" : "Mouse Aim in Hacking: Off";
    case NID_KeyboardPage: return "Keyboard Mapping...";
    case NID_ResetKeys: return "Reset Keyboard Mapping";
    default:
        if (id >= NID_KeyFirst && id < NID_KeyFirst + PCA_Max + 4)
        {
            static const char* const asz[] = {"Move Forward", "Move Back", "Move Left", "Move Right", "Square", "Circle",
                                              "Cross", "Triangle", "Start", "Select", "L1", "R1", "L2", "R2", "L3", "R3",
                                              "Camera Left", "Camera Right", "Camera Up", "Camera Down"};
            const int i = id - NID_KeyFirst;
            const int key = i < PCA_Max ? g_pcinput.keyboard[size_t(i)] : g_pcinput.cameraKeys[size_t(i - PCA_Max)];
            if (s_ikeyCapture == i) std::snprintf(ach, sizeof(ach), "%s: press a key (Esc cancels)", asz[i]);
            else std::snprintf(ach, sizeof(ach), "%s: %s", asz[i], PchzKeyName(key));
            return ach;
        }
        return "";
    }
}

void ApplyDisplay()
{
    if (g_gl.window)
    {
        ApplyDisplaySettings(g_gl.window);
        int w = 0, h = 0;
        glfwGetFramebufferSize(g_gl.window, &w, &h);
        if (w > 0 && h > 0) LayoutDisplay(w, h);
    }
    FSaveDisplaySettings(cane::userDirectory());
}

// a controls option's next value (pcinput.h), saved with the display options (system_settings.dat)
void CycleControls(MENUENT& e)
{
    PCINPUTOPT& o = g_pcinput;
    auto nextOf = [](int n, std::initializer_list<int> an) {
        auto it = std::find(an.begin(), an.end(), n);
        return (it == an.end() || it + 1 == an.end()) ? *an.begin() : it[1];
    };
    switch (e.id)
    {
    case NID_PromptStyle: o.promptStyle = (o.promptStyle + 1) % (cane::input::kPromptStyleLast + 1); break;
    case NID_AlwaysPs: o.fAlwaysOriginalPs = !o.fAlwaysOriginalPs; break;
    case NID_InstantSkip: o.fInstantSkipCutscenes = !o.fInstantSkipCutscenes; break;
    case NID_MouseCamera: o.fMouseCamera = !o.fMouseCamera; break;
    case NID_MouseSens: o.mouseSensitivity = nextOf(o.mouseSensitivity, {25, 50, 75, 100, 150, 200, 300, 400, 500}); break;
    case NID_AimSens: o.mouseAimSensitivity = nextOf(o.mouseAimSensitivity, {25, 50, 75, 100, 150, 200, 300, 400, 500}); break;
    case NID_InvertX: o.fMouseInvertX = !o.fMouseInvertX; break;
    case NID_InvertY: o.fMouseInvertY = !o.fMouseInvertY; break;
    case NID_WheelZoom: o.fMouseWheelZoom = !o.fMouseWheelZoom; break;
    case NID_MouseMenus: o.fMouseMenus = !o.fMouseMenus; break;
    case NID_HackMouse: o.fHackMouseAim = !o.fHackMouseAim; break;
    case NID_ResetKeys:
    {
        const PCINPUTOPT d = PcInputDefaults();
        o.keyboard = d.keyboard;
        o.cameraKeys = d.cameraKeys;
        break;
    }
    default: return;
    }
    FSaveDisplaySettings(cane::userDirectory());
}

// (the keyboard mapping) the key just pressed into row i: an action's key, or a camera key; a key another row has
// moves there (the two swap)
void BindKey(int i, int key)
{
    PCINPUTOPT& o = g_pcinput;
    int* pkey = i < PCA_Max ? &o.keyboard[size_t(i)] : &o.cameraKeys[size_t(i - PCA_Max)];
    if (i >= PCA_Max && cane::settings::mouseBinding(key)) return;   // (the camera keys are keys)
    const int old = *pkey;
    for (int& k : o.keyboard) if (k == key) k = old;
    for (int& k : o.cameraKeys) if (k == key) k = cane::settings::mouseBinding(old) ? cane::settings::kKeyUnknown : old;
    *pkey = key;
    FSaveDisplaySettings(cane::userDirectory());
}

// a PC display option's next value
void CycleNative(MENUENT& e)
{
    switch (e.id)
    {
    case NID_Aspect: g_display.aspect = DISPASPECT((int(g_display.aspect) + 1) % 6); break;
    case NID_Resolution:
    {
        static const int an[] = {0, 360, 480, 720, 1080, 1440, 2160};
        const int* p = std::find(std::begin(an), std::end(an), g_display.internalHeight);
        g_display.internalHeight = (p == std::end(an) || p + 1 == std::end(an)) ? an[0] : p[1];
        break;
    }
    case NID_Msaa:
    {
        static const int an[] = {0, 2, 4, 8};
        const int* p = std::find(std::begin(an), std::end(an), g_display.msaaSamples > 1 ? g_display.msaaSamples : 0);
        g_display.msaaSamples = (p == std::end(an) || p + 1 == std::end(an)) ? an[0] : p[1];
        break;
    }
    case NID_FrameRate:
    {
        static const int an[] = {30, 60, 120, 144, 240};
        const int* p = std::find(std::begin(an), std::end(an), g_display.frameRate);
        g_display.frameRate = (p == std::end(an) || p + 1 == std::end(an)) ? an[0] : p[1];
        break;
    }
    case NID_Window: g_display.window = DISPWINDOW((int(g_display.window) + 1) % 3); break;
    case NID_Vsync: g_display.fVsync = !g_display.fVsync; break;
    case NID_DrawDistance:
    {
        static const float ag[] = {0.75f, 1.0f, 1.5f, 2.0f, 3.0f};
        int i = 0;
        while (i < 5 && std::fabs(ag[i] - g_display.drawDistance) > 0.01f) ++i;
        g_display.drawDistance = ag[(i + 1) % 5];
        break;
    }
    default: return;
    }
    ApplyDisplay();
}

// orig 0x001AF9C8 (an option entry shows the setting's value)
void InitOptionText(MENUENT& e)
{
    switch (e.id)
    {
    case 76: case 77: case 78:
        switch (NRef(RefSetting("nMixerMode")))
        {
        case 1: e.id = 0x4C; break;
        case 2: e.id = 0x4D; break;
        default: e.id = 0x4E; break;
        }
        break;
    case 79: case 80: e.id = NRef(RefSetting("fUseHeadset")) ? 0x4F : 0x50; break;   // (orig D_003DD050: a headset found)
    case 72: case 73: e.id = NRef(RefSetting("fNoVibration")) ? 0x49 : 0x48; break;
    case 81: case 82: e.id = NRef(RefSetting("fReverseLookAround")) ? 0x52 : 0x51; break;
    case 83: case 84: e.id = NRef(RefSetting("fReverseAim")) ? 0x54 : 0x53; break;
    default: break;
    }
}

// orig 0x001AFB88 (the option's value into the settings)
void ApplyOption(const MENUENT& e)
{
    switch (e.id)
    {
    case 0x67: SetSettingF("uMusicVolume", GRef(RefSetting("uMusicVolume"))); SetMenuState(9); break;
    case 0x68: SetSettingF("uEffectsVolume", GRef(RefSetting("uEffectsVolume"))); SetMenuState(9); break;
    case 0x3F: SetMenuState(9); break;   // (dxScreenCenter / dyScreenCenter: the PS2's display position, kept)
    case 78: SetSettingN("nMixerMode", 0); break;
    case 77: SetSettingN("nMixerMode", 2); break;
    case 76: SetSettingN("nMixerMode", 1); break;
    case 79: case 80: SetSettingB("fUseHeadset", e.id == 79); break;
    case 72: case 73: SetSettingB("fNoVibration", e.id == 73); break;
    case 81: case 82: SetSettingB("fReverseLookAround", e.id == 82); break;
    case 83: case 84: SetSettingB("fReverseAim", e.id == 84); break;
    default: break;
    }
    ApplyGameSettings(g_gamesave);   // orig 0x1D8FF8(the record): the settings take effect
}

// orig 0x001AFFC0 (the next of the option's values)
void CycleOption(MENUENT& e)
{
    const int c = e.n;
    if (c <= 0) return;
    int i = 0;
    while (i < c && NElf(e.vaTab + 4 * uint32_t(i)) != e.id) ++i;
    e.id = NElf(e.vaTab + 4 * uint32_t((i + 1) % c));
    ApplyOption(e);
    e.grf |= 2;
}

// orig 0x001AF6D0 (state 10: the slider follows left / right)
void UpdateSlider(PAGE& pg, MENUENT& e)
{
    const int n = NStepLeftRight();
    if (n == 0) return;
    switch (e.id)
    {
    case 0x67:
    case 0x68:
    {
        // the channel's volume (the sound's factor 1: music channel 1, effects 0 and 3) moves; the record keeps its
        // complement (0x1AFB88 cases 103 / 104: 1 - the volume)
        const char* name = e.id == 0x67 ? "uMusicVolume" : "uEffectsVolume";
        const float g = std::clamp(1.0f - GRef(RefSetting(name)) + float(n) * 0.1f, 0.0f, 1.0f);   // (orig: dt x 1.5 per step)
        if (e.id == 0x67) SetSoundChannelVolume(1, 1, g);
        else
        {
            SetSoundChannelVolume(0, 1, g);
            SetSoundChannelVolume(3, 1, g);
        }
        SetSettingF(name, 1.0f - g);
        SetScreenObjFraction(pg.plo, 0x315, g);
        break;
    }
    default: break;
    }
}

// ------------------------------------------------------------ the slots' pages (0x1AD250 / 0x1AD320 / 0x1AD480)
float DtSlot(int i) { return GRef(RefProgressOfSlot(i, {"dt"})); }

// orig 0x001AD3E0 (the save into the target slot)
void SaveToTarget()
{
    g_islotChosen = s_islotTarget;
    s_card.nAfter = 2;
    if (!NRef(RefSetting("fAutosaveWarned"))) ShowCardMessage(0x22);
    else
    {
        ShowCardMessage(0x1A);
        FWriteSaveSlot(g_islotChosen);   // (0x1ABD40: the record into the slot, the card thread writes it)
    }
}

void SelectEntry(MENUENT& e);

// orig: 0x001D8D80 func_001D8D80 (slot, 1): the slot's record cleared, keeping its settings (the 11 "flags" keys of
// SW +0x3B2C) when the record has the layout's identity (0x1D7D30)
void ResetSlotKeepSettings(int islot)
{
    SAVESLOT& sl = g_aslot[islot];
    static const char* const aszFlag[] = {"lang", "nMixerMode", "uMusicVolume", "uEffectsVolume", "fUseHeadset", "dxScreenCenter",
                                          "dyScreenCenter", "fNoVibration", "fReverseLookAround", "fReverseAim", "fAutosaveWarned"};
    GAMESAVE saved = g_gamesave;
    g_gamesave = sl.fFile ? sl.save : GAMESAVE{};
    ValidateGameSave();   // (a record of another layout: cleared, as 0x1D7D30 failing keeps nothing)
    std::vector<CRef> aref;
    for (const char* sz : aszFlag) aref.push_back(RefGetProgress({"flags", sz}));
    std::memset(g_gamesave.ab, 0, CB_PROGRESS);
    for (size_t i = 0; i < aref.size(); ++i) FSetProgress({"flags", aszFlag[i]}, aref[i]);
    sl.save = g_gamesave;
    g_gamesave = saved;
}

// ------------------------------------------------------------ the pages' tables
// orig 0x001B0290: the pause menu's entries (0x2C00E8, 7) for the current game
std::vector<MENUENT> AentPause()
{
    std::vector<MENUENT> a;
    LO* ploPo = s_psw ? PloPoCurrent(s_psw) : nullptr;
    const bool fEpisode = IepisodeCur() > 0;
    for (const MENUENT& e : AentFromElf(0x2C00E8, 7))
    {
        bool f = true;
        switch (e.id)
        {
        case 0x43: f = ploPo && ploPo->Cid() >= 0x35 && ploPo->Cid() < 0x38; break;   // Controls...
        case 0x6A: f = s_psw && PloFindSwObjectByOid(s_psw, 0x4E0) && fEpisode; break;   // Job Help (0x3DA3C0's object)
        case 0x6B: f = s_psw && s_psw->tasks.pjobCur && fEpisode; break;                 // Abandon Job
        case 0x42: f = fEpisode; break;                                                  // Exit to Episode Menu
        default: break;
        }
        if (f) a.push_back(e);
    }
    // (native) Exit to Desktop, as Sly1-Release's pause menu
    MENUENT ex;
    ex.id = NID_ExitToDesktop;
    ex.pfnSelect = PFN_NativeExit;
    a.push_back(ex);
    return a;
}

// (native) the PC display options' page
std::vector<MENUENT> AentDisplayOptions()
{
    std::vector<MENUENT> a;
    for (int id = NID_Aspect; id <= NID_DrawDistance; ++id)
    {
        MENUENT e;
        e.id = id;
        e.pfnSelect = PFN_NativeDisplay;
        a.push_back(e);
    }
    return a;
}

// orig 0x001B04E0: the options page (0x2BFEC8, 11) + the PC display options' page (one entry: the options
// appended to the wheel crowded the title's New Game / Load Game)
std::vector<MENUENT> AentOptions()
{
    std::vector<MENUENT> a;
    const bool fTitle = FTitleShown();
    for (const MENUENT& e : AentFromElf(0x2BFEC8, 11))
    {
        bool f = true;
        if (e.id == 0x3C) f = fTitle;          // New Game... only at the title
        else if (e.id == 0x3E) f = !fTitle;    // Save Game... only in a game
        if (f) a.push_back(e);
    }
    MENUENT e;
    e.id = NID_DisplayPage;
    e.pfnSelect = PFN_NativeDisplayPage;
    a.push_back(e);
    e.id = NID_ControlsPage;
    e.pfnSelect = PFN_NativeControlsPage;
    a.push_back(e);
    return a;
}

// (native) the controls page and its keyboard mapping
std::vector<MENUENT> AentControls()
{
    std::vector<MENUENT> a;
    for (int id = NID_PromptStyle; id <= NID_KeyboardPage; ++id)
    {
        MENUENT e;
        e.id = id;
        e.pfnSelect = id == NID_KeyboardPage ? PFN_NativeKeyboardPage : PFN_NativeControls;
        a.push_back(e);
    }
    return a;
}
std::vector<MENUENT> AentKeyboard()
{
    std::vector<MENUENT> a;
    for (int i = 0; i < PCA_Max + 4; ++i)
    {
        // (L3 / R3: the counters / the binocucom, sly.cpp ShowWorldStats, climb.cpp / gang.cpp OpenBinoc)
        MENUENT e;
        e.id = NID_KeyFirst + i;
        e.pfnSelect = PFN_NativeKey;
        a.push_back(e);
    }
    MENUENT e;
    e.id = NID_ResetKeys;
    e.pfnSelect = PFN_NativeControls;
    a.push_back(e);
    return a;
}

void InitEntries(PAGE& pg)
{
    for (MENUENT& e : pg.aent)
        switch (e.pfnInit)
        {
        case 0x1AF9C8: InitOptionText(e); break;
        case 0x1AD538:
        {
            // orig 0x001AD538: the selection on the first fitting slot
            float ag[3];
            for (int i = 0; i < 3; ++i) ag[i] = DtSlot(i);
            const int kind = pg.vaTab == 0x2BFDA8 ? 0x1E : pg.vaTab == 0x2BFE08 ? 0x1F : pg.vaTab == 0x2BFE68 ? 0x19 : -1;
            if (kind == 0x1E) { for (int j = 0; j < 3; ++j) if (ag[j] == 0.0f) { pg.isel = j; break; } }
            else if (kind == 0x1F) { for (int j = 0; j < 3; ++j) if (ag[j] != 0.0f) { pg.isel = j; break; } }
            else if (kind == 0x19 && g_islotChosen >= 0) pg.isel = g_islotChosen;
            break;
        }
        default: break;
        }
}

// an entry's select function (+0x04) by its original address
void SelectEntry(MENUENT& e)
{
    if (std::getenv("SLY2N_MENULOG")) std::printf("menu: select 0x%X (fn 0x%X, n %d)\n", e.id, e.pfnSelect, e.n);
    switch (e.pfnSelect)
    {
    case 0x1B0270: CloseMenu(); break;                                   // Keep Playing
    case 0x1B0688:                                                       // a page of entries
        PushPage(AentFromElf(e.vaTab, e.n), e.vaTab, nullptr, e.vaAnim);
        e.grf |= 2;
        break;
    case 0x1B06D8:                                                       // a yes / no page
        ReplacePage(AentFromElf(e.vaTab, e.n), e.vaTab);
        e.grf |= 2;
        break;
    case 0x1B0668: BackReplaced(); break;                                // No
    case 0x1B04E0:                                                       // Options...
        ReplacePage(AentOptions(), 0x2BFEC8);
        e.grf |= 2;
        break;
    case 0x1B0740:                                                       // Controls...
    {
        if (s_psw) BroadcastTaskEvent(s_psw, 0x29, nullptr);
        LO* po = s_psw ? PloPoCurrent(s_psw) : nullptr;
        const CID cid = po ? po->Cid() : -1;
        const uint32_t vaAnim = cid == 0x36 ? 0x2C01F0 : cid == 0x35 ? 0x2C01D0 : cid == 0x37 ? 0x2C0210 : 0;
        if (vaAnim) PushPage({}, 0, nullptr, vaAnim);
        e.grf |= 2;
        break;
    }
    case 0x1B0808:                                                       // Job Help (0x1DD300 on 0x3DA3C0: TODO)
        if (s_psw) BroadcastTaskEvent(s_psw, 0x2B, nullptr);
        CloseMenu();
        SetJobHelpPage(s_psw && s_psw->tasks.pjobCur ? 1 : 0);
        break;
    case 0x1B0878:                                                       // Yes, Quit
        if (s_psw)
        {
            BroadcastTaskEvent(s_psw, 0x2D, nullptr);
            BroadcastTaskEvent(s_psw, 0x2E, nullptr);
        }
        if (g_islotChosen >= 0)
        {
            s_card.nAfter = 1;
            if (!NRef(RefSetting("fAutosaveWarned"))) ShowCardMessage(0x22);
            else
            {
                ShowCardMessage(0x1A);
                FWriteSaveSlot(g_islotChosen);
            }
        }
        else StartBootSequence(false);   // orig 0x001660F8
        e.grf |= 2;
        break;
    case 0x1B0970:                                                       // Yes, Abandon
        if (s_psw)
        {
            BroadcastTaskEvent(s_psw, 0x2A, nullptr);
            if (s_psw->tasks.pjobCur) EndJob(s_psw, s_psw->tasks.pjobCur);
        }
        break;
    case 0x1B09A8:                                                       // Yes, Exit (to the episode menu)
        RequestTransition(s_psw, g_disc.PbElf(0x2A3928, 64), 0x189, -1, 0x80, -1);
        if (s_psw && s_psw->tasks.pjobCur) EndJob(s_psw, s_psw->tasks.pjobCur);
        e.grf |= 2;
        break;
    case 0x1AFFC0: CycleOption(e); break;
    case 0x1B0720: StartSlider(); break;
    case 0x1AD250:                                                       // New Game in slot n
    {
        if (DtSlot(e.n) == 0.0f)
        {
            g_islotChosen = e.n;
            ResetSlotKeepSettings(e.n);
            StartGameFromSave(s_psw, &g_aslot[e.n].save);
        }
        else
        {
            PsndStartSound(0x36);  // (0x1AD250: the error sound)
            e.grf |= 2;
        }
        break;
    }
    case 0x1AD320:                                                       // Load Game from slot n
        g_islotChosen = e.n;
        if (DtSlot(e.n) == 0.0f)
        {
            ResetSlotKeepSettings(e.n);
            StartGameFromSave(s_psw, &g_aslot[e.n].save);
        }
        else
        {
            StartGameFromSave(s_psw, &g_aslot[e.n].save);
            e.grf |= 2;
        }
        break;
    case 0x1AD480:                                                       // Save Game to slot n
        s_islotTarget = e.n;
        if (DtSlot(e.n) == 0.0f) SaveToTarget();
        else ShowCardMessage(0x1C);
        e.grf |= 2;
        break;
    case PFN_NativeDisplay: CycleNative(e); e.grf |= 2; break;
    case PFN_NativeControls: CycleControls(e); e.grf |= 2; break;
    case PFN_NativeControlsPage:
        PushPage(AentControls(), 0, nullptr, s_menu.depth > 0 ? s_menu.apage[size_t(s_menu.depth)].vaAnim : 0);
        e.grf |= 2;
        break;
    case PFN_NativeKeyboardPage:
        PushPage(AentKeyboard(), 0, nullptr, s_menu.depth > 0 ? s_menu.apage[size_t(s_menu.depth)].vaAnim : 0);
        e.grf |= 2;
        break;
    case PFN_NativeKey:
        if (g_gl.window)
        {
            s_ikeyCapture = e.id - NID_KeyFirst;
            KeyBindingPressed(g_gl.window);   // (primed: the key that chose the row is not taken)
        }
        e.grf |= 2;
        break;
    case PFN_NativeDisplayPage:
        PushPage(AentDisplayOptions(), 0, nullptr, s_menu.depth > 0 ? s_menu.apage[size_t(s_menu.depth)].vaAnim : 0);
        e.grf |= 2;
        break;
    case PFN_NativeExit:
        if (g_gl.window) glfwSetWindowShouldClose(g_gl.window, 1);
        break;
    default:
        std::printf("menu: entry 0x%X select 0x%X not ported\n", e.id, e.pfnSelect);
        break;
    }
}

// orig 0x001AD740 (a slot entry: its look selected or not)
void UpdateSlotEntry(PAGE& pg, const MENUENT& e)
{
    if (!FDataAccess()) return;
    SelectScreenObjPart(pg.plo, pg.isel == e.n ? e.n + 0x5C8 : e.n + 0x5C5);
}

// ------------------------------------------------------------ the card dialog's input (orig 0x001ACCC8)
// orig 0x001AD168 (yes)
void CardYes()
{
    switch (s_card.msg)
    {
    case 0x1C: SaveToTarget(); break;
    case 0x15:
    {
        // the save folder made (orig the thread 0x2A6D88: the card's directory, its icon and the three slot files,
        // each 0x5408 zero bytes), then 0x16 "creating"
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(cane::path(g_aslot[0].path)).parent_path(), ec);
        for (SAVESLOT& sl : g_aslot)
        {
            std::string err;
            if (!sl.fFile && FWriteGameSaveBlock(sl.path, GAMESAVE{}, err))
            {
                sl.save = GAMESAVE{};
                sl.fFile = true;
                sl.tWrite = std::filesystem::last_write_time(cane::path(sl.path), ec).time_since_epoch().count();
            }
        }
        RechooseSaveSlots();
        ShowCardMessage(0x16);
        break;
    }
    case 0x20:   // the erase (orig 0x1AAF90 with the erase function, 0x21)
        if (s_islotTarget >= 0)
        {
            // (the slot's file rewritten as an empty record: the folder stays, as on the card)
            SAVESLOT& sl = g_aslot[s_islotTarget];
            std::string err;
            sl.save = GAMESAVE{};
            if (FWriteGameSaveBlock(sl.path, sl.save, err))
            {
                std::error_code ec;
                sl.fFile = true;
                sl.tWrite = std::filesystem::last_write_time(cane::path(sl.path), ec).time_since_epoch().count();
            }
        }
        ShowCardMessage(0x21);
        break;
    default: CloseCard(); break;
    }
}
// orig 0x001ACFE0 (no / OK)
void CardNo()
{
    const int msg = s_card.msg;
    switch (msg)
    {
    case 0x22:
        SetSettingB("fAutosaveWarned", true);
        if (s_card.nAfter > 0 && s_card.nAfter < 4)
        {
            ShowCardMessage(0x1A);
            if (g_islotChosen >= 0) FWriteSaveSlot(g_islotChosen);
        }
        return;
    case 0x1B: case 0x1C: case 0x20: CloseCard(); return;
    case 0x11: case 0x12: case 0x15: case 0x18:
        // (no card for this game: the New Game page's slot choice goes on without one)
        if (KindDataAccess() == 0x1E)
        {
            CloseCard();
            g_ncardMode = 2;
            CloseMenu();
            StartNewGameFromTitle(s_psw);
            return;
        }
        CloseCard();
        g_ncardMode = 2;
        if (FDataAccess()) PopPage();
        return;
    case 0x14: case 0x17: case 0x1D: case 0x23: case 0x24: case 0x2E:
        CloseCard();
        if (FDataAccess()) PopPage();
        return;
    default: CloseCard(); return;
    }
}

// orig 0x001AB1A0 (the card's state each frame; its messages): the host's card has no Sly 2 folder (state 3)
// while no slot file exists in <user-dir>/Saves; the load page with no folder says so (0x2E)
void UpdateCardStatus()
{
    if (g_transition.fPending) return;
    if (g_ncardMode != 1 || s_card.fShown) return;
    if (g_clock.tReal - s_tCardState < 0.3f) return;
    const bool fFolder = g_aslot[0].fFile || g_aslot[1].fFile || g_aslot[2].fFile;
    if (KindDataAccess() == 0x1F)
    {
        if (!fFolder) ShowCardMessage(0x2E);
        return;
    }
    if (!fFolder) ShowCardMessage(0x15);
}

void UpdateCard()
{
    if (static const bool s_f = std::getenv("SLY2N_CARDPRINT") != nullptr; s_f && s_card.fShown)
        std::printf("card: msg 0x%X plo %d state %d tReal %.3f shown %.3f\n", s_card.msg, s_card.plo ? s_card.plo->oid : -1,
                    s_card.plo ? NScreenObjState(s_card.plo) : -1, g_clock.tReal, s_card.tShown);
    if (!s_card.fShown) return;
    if (s_card.plo && NScreenObjState(s_card.plo) != 2) return;
    if (g_clock.tReal - s_card.tShown <= 0.1f) return;
    const int r = NCardPrompt(s_card.msg);
    if (r == 1 && GrfbtnTakePressed(0x80)) { CardNo(); return; }
    if (r == 2)
    {
        if (GrfbtnTakePressed(0x80)) { CardYes(); return; }
        if (GrfbtnTakePressed(0x10)) { CardNo(); return; }
    }
    switch (s_card.msg)
    {
    case 0x1A:   // the save is written (synchronously here): done
        if (g_clock.tReal - s_card.tShown < 0.5f) return;
        CloseCard();
        if (s_card.nAfter == 1) StartBootSequence(false);
        else if (s_card.nAfter == 2) CloseMenu();
        return;
    case 0x16:   // the folder is made (synchronously here)
        if (g_clock.tReal - s_card.tShown < 0.5f) return;
        CloseCard();
        return;
    case 0x21:
        if (g_clock.tReal - s_card.tShown < 0.5f) return;
        s_islotTarget = -1;
        CloseCard();
        return;
    default: return;
    }
}
}  // namespace

// ------------------------------------------------------------ public
void ShowCardMessage(int idMsg, int nAfter)
{
    if (nAfter >= 0) s_card.nAfter = nAfter;
    std::printf("card: message 0x%X\n", idMsg);
    s_card.msg = idMsg;
    s_card.fShown = true;
    s_card.tShown = g_clock.tReal;
    if (s_card.plo) ShowScreenObj(s_card.plo, -1);
}

void InitMenu(SW* psw)
{
    if (s_menu.fShown) SetClockFactor(&g_clock, 0, 1.0f);
    s_psw = psw;
    s_menu = MENU{};
    s_menu.ploDefault = psw ? PloFindSwObjectByOid(psw, 0x370) : nullptr;
    s_card.plo = psw ? PloFindSwObjectByOid(psw, 0x52C) : nullptr;
    s_card.fShown = false;
    s_card.msg = -1;
}

int g_ncardMode = 0;

bool FMenuShown() { return s_menu.fShown || s_card.fShown; }
bool FMenuActive() { return s_menu.fShown; }
bool FCardMessageActive() { return s_card.fShown; }

bool FOpenPauseMenu(SW* psw)
{
    s_psw = psw;
    if (!s_menu.ploDefault || g_transition.fPending || s_card.fShown || s_menu.fShown) return false;
    PushPage(AentPause(), 0x2C00E8, s_menu.ploDefault, 0x2BFD38);
    return true;
}

void OpenDataAccessMenu(SW* psw)
{
    s_psw = psw;
    if (!s_menu.ploDefault) return;
    PushPage(AentOptions(), 0x2BFEC8, s_menu.ploDefault, 0x2BFD38);
}

void HudAutoSaveFrontEnd() { HudAutoSave(true); }

void UpdateMenu(SW* psw)
{
    s_psw = psw;
    UpdateCardStatus();
    UpdateCard();
    if (!s_menu.fShown || s_card.fShown) { s_ikeyCapture = -1; return; }
    PAGE* pe = PpageCur();
    if (!pe) return;
    // (the keyboard mapping) a row waits for a key: the menu takes no input meanwhile
    if (s_ikeyCapture >= 0)
    {
        GrfbtnTakePressed(0xFFFF);
        const int key = g_gl.window ? KeyBindingPressed(g_gl.window) : GLFW_KEY_ESCAPE;
        if (key == GLFW_KEY_ESCAPE) s_ikeyCapture = -1;
        else if (key >= 0)
        {
            BindKey(s_ikeyCapture, key);
            s_ikeyCapture = -1;
        }
        return;
    }
    for (MENUENT& e : pe->aent)
    {
        if (e.pfnUpdate == 0x1AD740) UpdateSlotEntry(*pe, e);
        else if (e.pfnUpdate == 0x1B0848) e.id = g_islotChosen >= 0 ? 0x47 : 0x46;   // Quit Game / Save and Quit
    }
    LO* plo = pe->plo;
    auto playing = [&](int id) { return plo && FScreenObjBusy(plo, id); };
    auto confirm = [&]() {
        if (pe->fReplaced) BackReplaced();
        else PopPage();
    };
    auto step = [&](int n) {
        if (n < 0) SetMenuState(6);
        else if (n > 0) SetMenuState(5);
    };
    switch (s_menu.state)
    {
    case 1:
        if (playing(0x314)) return;
        if (s_menu.pending != -1)
        {
            s_menu.depth = s_menu.pending;
            s_menu.pending = -1;
        }
        SetMenuState(7);
        return;
    case 2:
        if (playing(0x314)) return;
        if (s_menu.pending == -1) --s_menu.depth;
        else s_menu.pending = -1;
        if (s_menu.depth == 0)
        {
            if (plo) HideScreenObj(plo);
            SetMenuState(0);
        }
        else SetMenuState(7);
        return;
    case 3:
    {
        if (playing(IdAnim(*pe, 0x14))) return;
        const int iNext = s_menu.pending;
        s_menu.pending = -1;
        s_menu.depth = iNext;
        SetMenuState(4);
        return;
    }
    case 4:
        if (playing(IdAnim(*pe, 0xC))) return;
        SetMenuState(7);
        return;
    case 5:
    case 6:
        if (playing(IdAnim(*pe, s_menu.state == 5 ? 0x18 : 0x1C))) return;
        SetMenuState(7);
        step(NStepUpDown());
        return;
    case 7:
    {
        const uint32_t grf = GrfbtnTakePressed(0x870);
        if (grf & 0x800) { CloseMenu(); return; }
        if (grf & 0x10) { confirm(); return; }
        if (pe->aent.empty() && grf != 0) { confirm(); return; }
        if (s_menu.depth > 0 && (grf & 0x40) && !pe->aent.empty())
        {
            MENUENT& e = pe->aent[size_t(pe->isel)];
            SelectEntry(e);
            if (e.grf & 2) { e.grf &= ~2u; return; }
            if (!(e.grf & 1)) { CloseMenu(); return; }
            confirm();
            return;
        }
        if (grf & 0x20)
        {
            // the erase of a slot (Circle, on the new / load pages)
            const int r = KindDataAccess();
            if (r != 0x19 && r != 0x1E) return;
            if (DtSlot(pe->isel) == 0.0f) return;
            s_islotTarget = pe->isel;
            ShowCardMessage(0x20);
            return;
        }
        if (!pe->aent.empty()) step(NStepUpDown());
        return;
    }
    case 8:
        if (playing(0x314)) return;
        if (plo) HideScreenObj(plo);
        SetMenuState(0);
        return;
    case 9:
        if (playing(0x332)) return;
        SetMenuState(7);
        return;
    case 10:
    {
        MENUENT& e = pe->aent[size_t(pe->isel)];
        const uint32_t grf = GrfbtnTakePressed(0x850);
        if (grf & 0x800) { SetMenuState(9); CloseMenu(); }   // orig 0x001AFF88 + close
        else if (grf & 0x10) SetMenuState(9);                // orig 0x001AFF88
        else if (grf & 0x40) ApplyOption(e);
        else UpdateSlider(*pe, e);
        return;
    }
    case 11:
        if (playing(0x331)) return;
        SetMenuState(10);
        return;
    default: return;
    }
}

std::string StrMenuScript(int id)
{
    char ach[128];
    auto strText = [&](int idText) { return idText >= NID_Base ? StrNative(idText) : StrLocalized(s_psw, idText); };
    // orig 0x00211298 (0x2D2..0x2DB): the wheel's entry k around the selection (3: the selected one)
    if (id >= 0x2D2 && id <= 0x2DB)
    {
        const PAGE* pe = PpageCur();
        if (!pe || pe->aent.empty()) return "";
        const int c = int(pe->aent.size());
        int j = (id - 0x2D2) + pe->isel - 3;
        while (j < 0) j += c;
        return strText(pe->aent[size_t(j % c)].id);
    }
    switch (id)
    {
    case 0x5CB: { const int k = KindDataAccess(); return k < 0 ? "" : StrLocalized(s_psw, k); }   // orig 0x001AD928
    case 0x5CC:
    {
        // orig 0x001AD980: the current game's status (not at the title)
        if (FTitleShown()) return "";
        const int iep = NRef(RefGetProgress({"episodeBest"}));
        std::snprintf(ach, sizeof(ach), "%s : %d%%", StrLocalized(s_psw, 0x6D + std::clamp(iep, 0, 8)).c_str(), NGamePercent(g_gamesave));
        return ach;
    }
    case 0x5CD:
    {
        // orig 0x001ADA20: the selected slot's help
        const PAGE* pe = PpageCur();
        if (!pe || !FDataAccess()) return "";
        const float g = DtSlot(pe->isel);
        switch (KindDataAccess())
        {
        case 0x1E: return StrLocalized(s_psw, g != 0.0f ? 0x25 : 0x26);
        case 0x1F: return StrLocalized(s_psw, g != 0.0f ? 0x27 : 0x26);
        case 0x19: return StrLocalized(s_psw, 0x29) + (g != 0.0f ? "\n" + StrLocalized(s_psw, 0x25) : "");
        default: return "";
        }
    }
    case 0x5CE: return FDataAccess() ? StrLocalized(s_psw, 0x40) + " : &2T&." : "";   // orig 0x001ADBB8
    case 0x5CF: case 0x5D0: case 0x5D1:
    {
        // orig 0x001ADC40: slot i (its episode and percent, else New Game / Empty)
        const int i = id - 0x5CF;
        if (DtSlot(i) != 0.0f)
        {
            CRef r = RefProgressOfSlot(i, {"episodeBest"});
            if (r.m_tagk == TAGK_Nil) r = RefProgressOfSlot(i, {"episodeCurrent"});
            const int iep = std::clamp(NRef(r), 0, 8);
            const int pct = NGamePercent(g_aslot[i].save);   // (0x1D82B8)
            std::snprintf(ach, sizeof(ach), "%s : %d%%", StrLocalized(s_psw, 0x6D + iep).c_str(), pct);
            return ach;
        }
        const int k = KindDataAccess();
        if (k == 0x19) return StrLocalized(s_psw, 0x45);
        if (k == 0x1E || k == 0x1F) return StrLocalized(s_psw, 0x44);
        return "";
    }
    case 0x2F2: return s_card.msg >= 0 ? StrLocalized(s_psw, s_card.msg) : "";   // orig 0x001AD7E0
    case 0x2F3:
        // orig 0x001AD820
        switch (NCardPrompt(s_card.msg))
        {
        case 1: return "&2S&.: " + StrLocalized(s_psw, 0x38);
        case 2: return "&2S&. : " + StrLocalized(s_psw, 0x36) + "       " + StrLocalized(s_psw, 0x37) + " : &2T&.";
        default: return "";
        }
    default: return "";
    }
}

// orig: 0x001D8FF8 func_001D8FF8: a record's settings take effect: the language (0x1A2BD0 when the disc has two or more:
// the USA disc has one), the mixer mode (0x1FE9C8), the music volume (factor 1 of channel 1) and the effects' (channels 0
// and 3) as 1 - the stored values, the headset (D_003DD050: no headset natively) and the screen centre (the GS display's
// offsets +636 / +50: none on PC)
void ApplyGameSettings(const GAMESAVE& gs)
{
    GAMESAVE saved = g_gamesave;
    g_gamesave = gs;
    const CRef rMix = RefSetting("nMixerMode");
    const CRef rMusic = RefSetting("uMusicVolume");
    const CRef rEffects = RefSetting("uEffectsVolume");
    g_gamesave = saved;
    if (rMix.m_tagk != TAGK_Nil) SetSoundPlaybackMode(NRef(rMix));
    if (rMusic.m_tagk != TAGK_Nil) SetSoundChannelVolume(1, 1, 1.0f - GRef(rMusic));
    if (rEffects.m_tagk != TAGK_Nil)
    {
        SetSoundChannelVolume(0, 1, 1.0f - GRef(rEffects));
        SetSoundChannelVolume(3, 1, 1.0f - GRef(rEffects));
    }
}
