// See menu.h. Reference: the original's code (disassembly) and the decomp: m_1AE928.cpp's 0x1AE928 / 0x1AEAA8 /
// 0x1AEEE0 / 0x1AF660 / 0x1B01E8 (Sly2-Reference), func_001AF390.c / func_001AF4D8.c / func_001AF490.c /
// func_001AF608.c / func_001AF6A0.c / func_001AF6D0.c / func_001AF9C8.cpp / func_001AFB88 / func_001AFF88.c /
// func_001B0078.c / func_001B0108.c / func_001B0160.c / func_001B0270.c / func_001B0668.c / func_001B06D8.c /
// func_001B0720.c / func_001AD168.c / func_001AD250.c / func_001AD320.c / func_001AD3E0.c / func_001AD480.c /
// func_001AD538.c / func_001AD740.c / func_001ADA20.c / func_001ADBB8.c, and the code of 0x1B0290, 0x1B04E0,
// 0x1B0688, 0x1B0740, 0x1B0808, 0x1B0878, 0x1B0970, 0x1B09A8, 0x1AFFC0, 0x1ACFE0, 0x1ADC40; memcard.cpp's
// 0x1ACB58 / 0x1ACC80 / 0x1ACCC8 / 0x1AD7E0 / 0x1AD820.

#include "rumble.h"
#include "menu.h"
#include "langstore.h"
#include "textmod.h"
#include "pcinput.h"
#include "vr.h"
#include "actioninput.h"
#include "porttext.h"
#include "region.h"
#include "porttext.h"
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
#include "remappage.h"
#include "../platform/controller_family.h"
#include "../platform/platform.h"
#include "../platform/system_settings.h"
#include "../platform/sly2_settings.h"
#include "../platform/audio_devices.h"
#include "audio.h"

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
int32_t NElf(uint32_t va) { return int32_t(g_disc.U32Elf(va)); }  // (an address comes back as the USA one: disc.h)
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
    NID_KeyFirst,   // .. + PCA_Max + 6: the keyboard mapping's rows (the actions, the camera keys, the tank's tracks)
    // (PC options) the display options' Long Shadows; Invert Controls... (settings v17): its page,
    // the controller's camera, the mouse in the binocucom / turrets (NID_InvertX / NID_InvertY: the mouse's camera)
    NID_Pc2 = NID_Base + 0x100,
    NID_LongShadows = NID_Pc2, NID_InvertPage, NID_PadInvertX, NID_PadInvertY, NID_BinocInvertX, NID_BinocInvertY,
    NID_TurretInvertX, NID_TurretInvertY, NID_AutoCameraRotation,
    NID_RemapPage,   // (2026-11-10) Remap Controls...: the Remap page (remappage.h)
    // (audio, notes/AUDIO_2026-11-10.md) Audio Devices...: the output, the microphone, the headset's speaker
    NID_AudioPage, NID_AudioOutput, NID_AudioInput, NID_AudioHeadsetOutput,
    // (lang) Language...: the text's and the voices' languages apart (region.h g_nLangVoice)
    NID_LanguagePage, NID_TextLanguage, NID_VoiceLanguage,
    // (VR, vr.h) the pause menu's VR Settings page (a headset in use)
    NID_Vr = NID_Base + 0x200,
    NID_VrPage = NID_Vr, NID_VrMode, NID_VrAutoCam, NID_VrVignette, NID_VrScreenSize, NID_VrRefresh, NID_VrWorldScale,
    NID_VrRecenter, NID_VrCutscenes, NID_VrRcTable,
    NID_AdvancedControls = NID_Base + 0x1000, NID_ActionDevices, NID_ActionKeyboard, NID_ActionController, NID_ResetActions,
    NID_ActionGroupFirst = NID_Base + 0x2000,
    NID_ActionBindingFirst = NID_Base + 0x3000,
    NID_LanguageFirst = NID_Base + 0x4000,   // .. + language: a custom language's name (langstore.h)
};
constexpr uint32_t PFN_NativeDisplay = 0xFFFF0001;   // (a native select function)
constexpr uint32_t PFN_NativeExit = 0xFFFF0002;
constexpr uint32_t PFN_NativeDisplayPage = 0xFFFF0003;   // (the PC display options' own page)
constexpr uint32_t PFN_NativeControls = 0xFFFF0004;      // (a controls option: cycled)
constexpr uint32_t PFN_NativeControlsPage = 0xFFFF0005;
constexpr uint32_t PFN_NativeKeyboardPage = 0xFFFF0006;
constexpr uint32_t PFN_NativeKey = 0xFFFF0007;           // (a key row: the next key pressed binds)
constexpr uint32_t PFN_NativeLanguage = 0xFFFF0008;      // (the title's language menu: the European build's 0x1DDCA8)
constexpr uint32_t PFN_NativeInvertPage = 0xFFFF0009;    // (Invert Controls...)
// the native pages' tables (their identity: the invert page's rows show the controller's icons)
constexpr uint32_t VA_NativeInvertPage = 0xFFFF0109;
constexpr uint32_t PFN_NativeVrPage = 0xFFFF0020, PFN_NativeVr = 0xFFFF0021;   // (VR Settings: its page, its options)
constexpr uint32_t PFN_NativeAudioPage = 0xFFFF0022, PFN_NativeAudio = 0xFFFF0023;   // (Audio Devices: its page, its rows)
constexpr uint32_t PFN_NativeLanguagePage = 0xFFFF0024, PFN_NativeLanguageRow = 0xFFFF0025;   // (Language...: page, rows)
constexpr uint32_t PFN_ActionDevices = 0xFFFF0010, PFN_ActionGroups = 0xFFFF0011,
                   PFN_ActionRows = 0xFFFF0012, PFN_ActionCapture = 0xFFFF0013;
constexpr uint32_t PFN_RemapPage = 0xFFFF0014;           // (the Remap page over the menu)
std::vector<std::string> ActionGroups()
{
    std::vector<std::string> groups;
    namespace input = cane::sly2input;
    for (size_t i = 0; i < input::kActionCount; ++i) {
        if (!g_pcinput.fDebugKeys && input::isNoclipAction(input::Action(i))) continue;
        const auto& action = input::kActions[i];
        if (std::find(groups.begin(), groups.end(), action.group) == groups.end()) groups.emplace_back(action.group);
    }
    return groups;
}

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

// a menu table's text id as the USA one (the port's menu code keys the entries by them); the disc's own when the
// USA text has none
int IdMenuText(int id)
{
    const int idUsa = IdTextUsa(id);
    return idUsa >= 0 ? idUsa : id;
}

std::vector<MENUENT> AentFromElf(uint32_t va, int c)
{
    std::vector<MENUENT> a;
    for (int i = 0; i < c; ++i)
    {
        const uint32_t v = va + uint32_t(i) * 0x20;
        MENUENT e;
        e.id = IdMenuText(NElf(v));   // (the entries' text ids as the USA ones: the Japanese ids differ, region.h)
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
    bool fCanCancel = true;      // the initial language choice must be confirmed
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
int s_iActionCapture = -1;
bool s_fActionController = false;
float s_tActionCapture = 0;

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
            SetScreenObjFraction(plo, 0x315, g_saveopts.fStore ? g_saveopts.gMusic : 1.0f - GRef(RefSetting("uMusicVolume")));
            break;
        case 0x68:
            SelectScreenObjPart(plo, 0x334);
            SelectScreenObjPart(plo, 0x315);
            SetScreenObjFraction(plo, 0x315, g_saveopts.fStore ? g_saveopts.gEffects : 1.0f - GRef(RefSetting("uEffectsVolume")));
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
// (audio) a device row's value: the default's name, the device (shortened to fit the page), "(not connected)" when
// missing
std::string StrAudioDevice(const std::string& name, bool fInput, PTX ptxDefault, PTX ptxLabel)
{
    const std::string n = cane::audio::cleanName(name);
    if (n.empty()) return StrPort(ptxDefault);
    std::string shown = n;
    // (the row on one line: the label and the name within about 36 letters, the page's width at its font size)
    const size_t cmax = std::max<size_t>(14, 36 - std::min<size_t>(22, StrPort(ptxLabel).size() + 2));
    if (shown.size() > cmax)
    {
        size_t cut = cmax - 3;
        while (cut > 0 && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80) --cut;   // (not inside a UTF-8 letter)
        shown = shown.substr(0, cut) + "...";
    }
    const cane::audio::DeviceList list = ListAudioDevices();
    if (cane::audio::findDevice(fInput ? list.inputs : list.outputs, n) >= 0) return StrToGameText(shown);
    char ach[256];
    std::snprintf(ach, sizeof(ach), StrPort(PTX_NotConnected).c_str(), StrToGameText(shown).c_str());
    return ach;
}

// a language's name in the text's language (the game's 14 by the text 0x58..; a custom one by its own name)
std::string StrLanguageName(int lang)
{
    if (lang < 0) return "";
    if (lang < LANG_Max)
    {
        std::string s = StrLocalized(s_psw, IdText(0x58 + lang));
        if (!s.empty()) return s;
    }
    const std::string sCustom = StrLangStoreDisplayName(lang);
    return sCustom.empty() ? StrToGameText(SzLanguageModName(lang)) : sCustom;
}

// (the port's own options: their text in the game's language, porttext.h; English on the USA disc)
std::string StrNative(int id)
{
    char ach[96];
    auto onOff = [](PTX ptx, bool f) { return StrPortOnOff(ptx, f); };
    if (id >= NID_LanguageFirst && id < NID_LanguageFirst + 0x100)
    {
        const int lang = id - NID_LanguageFirst;
        // (one of the game's 14 here: a language whose text this disc cannot show, OpenLanguageMenu)
        std::string name = StrLanguageName(lang);
        if (FLangStoreVoicesOnly(lang))
        {
            std::snprintf(ach, sizeof(ach), StrPort(PTX_LanguageVoicesOnly).c_str(), name.c_str());
            name = ach;
        }
        return name;
    }
    switch (id)
    {
    case NID_Aspect:
    {
        static const char* const asz[] = {nullptr, "4:3", "16:9", "16:10", "4:3 (PS2)", "16:9 (PS2)"};
        const int i = std::clamp(int(g_display.aspect), 0, 5);
        return StrPortValue(PTX_AspectRatio, i == 0 ? StrPort(PTX_FitToScreen) : std::string(asz[i]));
    }
    case NID_Resolution:
        std::snprintf(ach, sizeof(ach), g_display.internalHeight == 0 ? " (%d x %d)" : "%d x %d", g_display.renderWidth, g_display.renderHeight);
        return StrPortValue(PTX_Resolution, g_display.internalHeight == 0 ? StrPort(PTX_Native) + ach : std::string(ach));
    case NID_Msaa:
        if (g_display.msaaSamples > 1) std::snprintf(ach, sizeof(ach), "%dx", g_display.msaaSamples);
        return StrToGameText("MSAA") + StrPort(PTX_Sep) + (g_display.msaaSamples > 1 ? std::string(ach) : StrPort(PTX_Off));
    case NID_FrameRate:
        if (g_display.frameRate >= DISP_FRAMERATE_UNCAPPED) return StrPortValue(PTX_FrameRate, StrPort(PTX_Uncapped));
        std::snprintf(ach, sizeof(ach), "%d FPS", g_display.frameRate);
        return StrPortValue(PTX_FrameRate, ach);
    case NID_Window:
    {
        static const PTX aptx[] = {PTX_Windowed, PTX_Borderless, PTX_Fullscreen};
        return StrPortValue(PTX_WindowMode, StrPort(aptx[std::clamp(int(g_display.window), 0, 2)]));
    }
    case NID_Vsync: return onOff(PTX_VSync, g_display.fVsync);
    case NID_DrawDistance: return StrPortValue(PTX_DrawDistance, StrPortDecimal(g_display.drawDistance, 2) + "x");
    case NID_LongShadows: return onOff(PTX_LongShadows, g_display.fLongShadows);
    case NID_ExitToDesktop: return StrPort(FQuitToLauncher() ? PTX_QuitToLauncher : PTX_ExitToDesktop);
    case NID_DisplayPage: return StrPort(PTX_DisplayOptions);
    case NID_ControlsPage: return StrPort(PTX_ControlsAndPrompts);
    case NID_PromptStyle:
    {
        // (the controllers' names stay as they are: controller_family.h)
        const int i = std::clamp(g_pcinput.promptStyle, 0, cane::input::kPromptStyleLast);
        return StrPortValue(PTX_ButtonIcons, i == 0 ? StrPort(PTX_Auto) : std::string(cane::input::promptStyleName(i)));
    }
    case NID_AlwaysPs: return onOff(PTX_OriginalPsIcons, g_pcinput.fAlwaysOriginalPs);
    case NID_InstantSkip: return onOff(PTX_InstantSkip, g_pcinput.fInstantSkipCutscenes);
    case NID_MouseCamera: return onOff(PTX_MouseCamera, g_pcinput.fMouseCamera);
    case NID_AutoCameraRotation: return onOff(PTX_AutoCameraRotation, g_pcinput.fAutoCameraRotation);
    case NID_MouseSens: std::snprintf(ach, sizeof(ach), "%d%%", g_pcinput.mouseSensitivity); return StrPortValue(PTX_MouseSensitivity, ach);
    case NID_AimSens: std::snprintf(ach, sizeof(ach), "%d%%", g_pcinput.mouseAimSensitivity); return StrPortValue(PTX_MouseAimSensitivity, ach);
    case NID_InvertPage: return StrPort(PTX_InvertControlsPage);
    case NID_RemapPage: return StrPort(PTX_RemapPage);
    case NID_AudioPage: return StrPort(PTX_AudioDevicesPage);
    case NID_LanguagePage: return StrPort(PTX_LanguagePage);
    case NID_TextLanguage:
    case NID_VoiceLanguage:
    {
        // the choice (a pending one: from the next world, region.h SetPendingLanguages)
        int langText = -2, langVoice = -2;
        FLanguagesPending(&langText, &langVoice);
        const bool fText = id == NID_TextLanguage;
        const bool fPending = fText ? langText != -2 : langVoice != -2;
        const int lang = fText ? (fPending ? langText : g_nLang) : (fPending ? langVoice : g_nLangVoice);
        std::string value = !fText && (lang < 0 || (!fPending && LangVoice() == g_nLang)) ? StrPort(PTX_SameAsText) : StrLanguageName(lang);
        // (a text language this disc cannot draw, chosen before: its voices over the disc's text, langstore.h)
        if (fText && FLangStoreVoicesOnly(lang))
        {
            std::snprintf(ach, sizeof(ach), StrPort(PTX_LanguageVoicesOnly).c_str(), value.c_str());
            value = ach;
        }
        // ("next area" only when what will be heard / read changes)
        const int textNext = langText != -2 ? langText : g_nLang;
        const int voiceNext = langVoice != -2 ? langVoice : g_nLangVoice;
        const bool fChanges = fText ? textNext != g_nLang : (voiceNext < 0 ? textNext : voiceNext) != LangVoice();
        if (fPending && fChanges)
        {
            std::snprintf(ach, sizeof(ach), StrPort(PTX_FromNextArea).c_str(), value.c_str());
            value = ach;
        }
        return StrPortValue(fText ? PTX_TextLanguage : PTX_VoiceLanguage, value);
    }
    case NID_AudioOutput: return StrPortValue(PTX_OutputDevice, StrAudioDevice(g_saveopts.outputDevice, false, PTX_SystemDefault, PTX_OutputDevice));
    case NID_AudioInput: return StrPortValue(PTX_InputDevice, StrAudioDevice(g_saveopts.inputDevice, true, PTX_SystemDefault, PTX_InputDevice));
    case NID_AudioHeadsetOutput:
        return StrPortValue(PTX_HeadsetOutputDevice, StrAudioDevice(g_saveopts.headsetOutputDevice, false, PTX_SameAsOutput, PTX_HeadsetOutputDevice));
    // (the controller's rows: the right stick's icon, the device's, promptglyph.h)
    case NID_PadInvertX: return "&2)&. " + onOff(PTX_PadInvertCameraX, g_pcinput.fPadInvertCameraX);
    case NID_PadInvertY: return "&2)&. " + onOff(PTX_PadInvertCameraY, g_pcinput.fPadInvertCameraY);
    case NID_InvertX: return onOff(PTX_MouseInvertCameraX, g_pcinput.fMouseInvertX);
    case NID_InvertY: return onOff(PTX_MouseInvertCameraY, g_pcinput.fMouseInvertY);
    case NID_BinocInvertX: return onOff(PTX_MouseInvertBinocX, g_pcinput.fMouseInvertBinocX);
    case NID_BinocInvertY: return onOff(PTX_MouseInvertBinocY, g_pcinput.fMouseInvertBinocY);
    case NID_TurretInvertX: return onOff(PTX_MouseInvertTurretX, g_pcinput.fMouseInvertTurretX);
    case NID_TurretInvertY: return onOff(PTX_MouseInvertTurretY, g_pcinput.fMouseInvertTurretY);
    case NID_WheelZoom: return onOff(PTX_MouseWheelZoom, g_pcinput.fMouseWheelZoom);
    case NID_MouseMenus: return onOff(PTX_MouseInMenus, g_pcinput.fMouseMenus);
    case NID_HackMouse: return onOff(PTX_MouseAimInHacking, g_pcinput.fHackMouseAim);
    case NID_KeyboardPage: return StrPort(PTX_KeyboardMapping);
    case NID_ResetKeys: return StrPort(PTX_ResetKeyboardMapping);
    case NID_AdvancedControls: return onOff(PTX_AdvancedBindings, sly2actions::profile.enabled);
    case NID_VrPage: return StrPort(PTX_VrSettings);
    case NID_VrMode: return StrPortValue(PTX_VrMode, StrPort(vr::g_vropts.mode == vr::VRMODE_Immersive ? PTX_VrImmersive : PTX_VrFlatScreen));
    case NID_VrAutoCam: return onOff(PTX_AutoCameraRotation, vr::g_vropts.fAutoCameraRotation);
    case NID_VrVignette:
    {
        static const PTX aptx[] = {PTX_Off, PTX_VrLow, PTX_VrMedium, PTX_VrHigh};
        return StrPortValue(PTX_VrVignette, StrPort(aptx[std::clamp(vr::g_vropts.vignette, 0, 3)]));
    }
    case NID_VrScreenSize:
    {
        static const PTX aptx[] = {PTX_VrSmall, PTX_VrMedium, PTX_VrLarge};
        return StrPortValue(PTX_VrScreenSize, StrPort(aptx[std::clamp(vr::g_vropts.screenSize, 0, 2)]));
    }
    case NID_VrRefresh:
        if (vr::g_vropts.refreshRate <= 0) return StrPortValue(PTX_VrRefreshRate, StrPort(PTX_Auto));
        std::snprintf(ach, sizeof(ach), "%d Hz", vr::g_vropts.refreshRate);
        return StrPortValue(PTX_VrRefreshRate, ach);
    case NID_VrWorldScale: return StrPortValue(PTX_VrWorldScale, StrPortDecimal(vr::g_vropts.worldScale, 2) + "x");
    case NID_VrRecenter: return StrPort(PTX_VrRecenter);
    case NID_VrCutscenes: return StrPortValue(PTX_VrCutscenes, StrPort(vr::g_vropts.fCutscenesFlat ? PTX_VrFlatScreen : PTX_VrImmersive));
    case NID_VrRcTable: return onOff(PTX_VrRcTable, vr::g_vropts.fRcTable);
    case NID_ActionDevices: return StrPort(PTX_ActionBindings);
    case NID_ActionKeyboard: return StrPort(PTX_ActionKeyboard);
    case NID_ActionController: return StrPort(PTX_ActionController);
    case NID_ResetActions: return StrPort(PTX_ResetAdvancedBindings);
    default:
        if (id >= NID_ActionGroupFirst && id < NID_ActionBindingFirst) {
            const auto groups = ActionGroups();
            const size_t index = size_t(id - NID_ActionGroupFirst);
            if (index < groups.size()) return StrPortActionGroup(groups[index]) + "...";
        }
        if (id >= NID_ActionBindingFirst && id < NID_ActionBindingFirst + int(cane::sly2input::kActionCount) * 2)
        {
            const size_t encoded = size_t(id - NID_ActionBindingFirst), count = cane::sly2input::kActionCount;
            const bool controller = encoded >= count;
            const size_t index = encoded % count;
            const int binding = controller ? sly2actions::profile.gamepad[index] : sly2actions::profile.keyboard[index];
            std::string value = binding < 0 ? StrPort(PTX_KeyUnbound) : controller
                ? StrToGameText(cane::input::buttonLabel(cane::input::fromGlfwBinding(binding), cane::input::promptFamily(g_pcinput.promptStyle, 0)))
                : StrPortKeyName(binding);
            if (s_iActionCapture == int(index) && s_fActionController == controller) return StrPort(PTX_CaptureAction);
            return StrPortActionName(sly2actions::Action(index)) + StrPort(PTX_Sep) + value;
        }
        if (id >= NID_KeyFirst && id < NID_KeyFirst + PCA_Max + 6)
        {
            static const PTX aptx[] = {PTX_MoveForward, PTX_MoveBack, PTX_MoveLeft, PTX_MoveRight, PTX_Square, PTX_Circle,
                                       PTX_Cross, PTX_Triangle, PTX_Start, PTX_Select, PTX_Max, PTX_Max, PTX_Max, PTX_Max,
                                       PTX_Max, PTX_Max, PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown,
                                       PTX_TankLeftTrack, PTX_TankRightTrack};
            static const char* const aszButton[] = {"L1", "R1", "L2", "R2", "L3", "R3"};   // (PTX_Max: the button's own name)
            const int i = id - NID_KeyFirst;
            const int key = i < PCA_Max ? g_pcinput.keyboard[size_t(i)]
                          : i < PCA_Max + 4 ? g_pcinput.cameraKeys[size_t(i - PCA_Max)] : g_pcinput.tankKeys[size_t(i - PCA_Max - 4)];
            const std::string label = aptx[i] == PTX_Max ? std::string(aszButton[i - 10]) : StrPort(aptx[i]);
            return label + StrPort(PTX_Sep) + (s_ikeyCapture == i ? StrPort(PTX_PressAKey) : StrPortKeyName(key));
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

// a controls option's next value (pcinput.h), saved with the display options (Settings.ini)
void CycleControls(MENUENT& e)
{
    PCINPUTOPT& o = g_pcinput;
    auto nextOf = [](int n, std::initializer_list<int> an) {
        auto it = std::find(an.begin(), an.end(), n);
        return (it == an.end() || it + 1 == an.end()) ? *an.begin() : it[1];
    };
    switch (e.id)
    {
    case NID_AdvancedControls:
        if (!sly2actions::profile.readOnly) {
            sly2actions::profile.enabled = !sly2actions::profile.enabled;
            sly2actions::Save(cane::userDirectory());
            // (the devices row exists only while the advanced mapping is on: the last row of the page)
            if (PAGE* pg = PpageCur()) {
                std::erase_if(pg->aent, [](const MENUENT& row) { return row.id == NID_KeyboardPage || row.id == NID_ActionDevices; });
                if (sly2actions::profile.enabled) {
                    MENUENT devices;
                    devices.id = NID_ActionDevices;
                    devices.pfnSelect = PFN_ActionDevices;
                    pg->aent.push_back(devices);
                }
            }
        }
        return;
    case NID_ResetActions:
        if (!sly2actions::profile.readOnly) {
            const bool enabled = sly2actions::profile.enabled;
            sly2actions::profile = cane::sly2input::defaults();
            sly2actions::profile.enabled = enabled;
            sly2actions::Save(cane::userDirectory());
        }
        return;
    case NID_PromptStyle: o.promptStyle = (o.promptStyle + 1) % (cane::input::kPromptStyleLast + 1); break;
    case NID_AlwaysPs: o.fAlwaysOriginalPs = !o.fAlwaysOriginalPs; break;
    case NID_InstantSkip: o.fInstantSkipCutscenes = !o.fInstantSkipCutscenes; break;
    case NID_MouseCamera: o.fMouseCamera = !o.fMouseCamera; break;
    case NID_AutoCameraRotation: o.fAutoCameraRotation = !o.fAutoCameraRotation; break;
    case NID_MouseSens: o.mouseSensitivity = nextOf(o.mouseSensitivity, {25, 50, 75, 100, 150, 200, 300, 400, 500}); break;
    case NID_AimSens: o.mouseAimSensitivity = nextOf(o.mouseAimSensitivity, {25, 50, 75, 100, 150, 200, 300, 400, 500}); break;
    case NID_InvertX: o.fMouseInvertX = !o.fMouseInvertX; break;
    case NID_InvertY: o.fMouseInvertY = !o.fMouseInvertY; break;
    case NID_PadInvertX: o.fPadInvertCameraX = !o.fPadInvertCameraX; break;
    case NID_PadInvertY: o.fPadInvertCameraY = !o.fPadInvertCameraY; break;
    case NID_BinocInvertX: o.fMouseInvertBinocX = !o.fMouseInvertBinocX; break;
    case NID_BinocInvertY: o.fMouseInvertBinocY = !o.fMouseInvertBinocY; break;
    case NID_TurretInvertX: o.fMouseInvertTurretX = !o.fMouseInvertTurretX; break;
    case NID_TurretInvertY: o.fMouseInvertTurretY = !o.fMouseInvertTurretY; break;
    case NID_WheelZoom: o.fMouseWheelZoom = !o.fMouseWheelZoom; break;
    case NID_MouseMenus: o.fMouseMenus = !o.fMouseMenus; break;
    case NID_HackMouse: o.fHackMouseAim = !o.fHackMouseAim; break;
    case NID_ResetKeys:
    {
        const PCINPUTOPT d = PcInputDefaults();
        o.keyboard = d.keyboard;
        o.cameraKeys = d.cameraKeys;
        o.tankKeys = d.tankKeys;
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
    if (i >= PCA_Max + 4)
    {
        // (the tank's tracks: keys of their own, only read while a tank is driven; the movement keys may share them)
        if (cane::settings::mouseBinding(key)) return;
        o.tankKeys[size_t(i - PCA_Max - 4)] = key;
        FSaveDisplaySettings(cane::userDirectory());
        return;
    }
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
#ifdef __SWITCH__
        static const int an[] = {0, 360, 480, 720, 1080};   // (the Switch: up to the docked screen's 1080 lines)
#else
        static const int an[] = {0, 360, 480, 720, 1080, 1440, 2160};
#endif
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
#ifdef __SWITCH__
        std::vector<int> an = {30, 60};   // (the Switch: its display's 60 Hz and half of it)
#else
        // (the launcher's list: these, the monitor's refresh rate, Uncapped)
        std::vector<int> an = {30, 60, 90, 120, 144, 240, DISP_FRAMERATE_UNCAPPED};
#endif
        if (const int hz = DisplayRefreshRate(g_gl.window); hz >= 30 && hz < DISP_FRAMERATE_UNCAPPED) an.push_back(hz);
        std::sort(an.begin(), an.end());
        an.erase(std::unique(an.begin(), an.end()), an.end());
        const auto p = std::upper_bound(an.begin(), an.end(), g_display.frameRate);
        g_display.frameRate = p == an.end() ? an[0] : *p;
        break;
    }
    case NID_Window: g_display.window = DISPWINDOW((int(g_display.window) + 1) % 3); break;
    case NID_Vsync: g_display.fVsync = !g_display.fVsync; break;
    case NID_LongShadows: g_display.fLongShadows = !g_display.fLongShadows; break;
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

// (PC, Settings.ini: display.h SAVEOPTS) the save's option flags mirror the options: every record the game keeps or
// writes carries them (an original or older build that loads the save gets the same options). The values equal to
// the defaults are the record's zero bytes, so a default run leaves records as the original's.
void SyncRecordOptions()
{
    if (!g_saveopts.fStore) return;
    auto setB = [](const char* name, bool f) { if (RefSetting(name).m_tagk != TAGK_Nil && FSettingFlag(name) != f) SetSettingB(name, f); };
    setB("fNoVibration", !g_fControllerVibration);
    setB("fReverseLookAround", g_saveopts.fInvertBinoc);
    setB("fReverseAim", g_saveopts.fInvertTurret);
    if (const CRef r = RefSetting("nMixerMode"); r.m_tagk != TAGK_Nil && NRef(r) != g_saveopts.speakers) SetSettingN("nMixerMode", g_saveopts.speakers);
    auto setVolume = [](const char* name, float g) {
        const float u = cane::settings::saveComplement(g);
        if (const CRef r = RefSetting(name); r.m_tagk != TAGK_Nil && GRef(r) != u) SetSettingF(name, u);
    };
    setVolume("uMusicVolume", g_saveopts.gMusic);
    setVolume("uEffectsVolume", g_saveopts.gEffects);
    setB("fUseHeadset", g_saveopts.fHeadset);   // (audio: the original's USB Headset, which works on PC now)
}

// orig 0x001AF9C8 (an option entry shows the setting's value; PC: Settings.ini's, display.h SAVEOPTS)
void InitOptionText(MENUENT& e)
{
    if (g_saveopts.fStore)
        switch (e.id)
        {
        case 76: case 77: case 78: e.id = g_saveopts.speakers == 1 ? 0x4C : g_saveopts.speakers == 2 ? 0x4D : 0x4E; return;
        case 72: case 73: e.id = g_fControllerVibration ? 0x48 : 0x49; return;
        case 81: case 82: e.id = g_saveopts.fInvertBinoc ? 0x52 : 0x51; return;
        case 83: case 84: e.id = g_saveopts.fInvertTurret ? 0x54 : 0x53; return;
        case 79: case 80: e.id = g_saveopts.fHeadset ? 0x4F : 0x50; return;
        default: break;
        }
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
    case 72: case 73: SetVibrationOption(e.id == 73); break;   // (0x2C2D8C 3 / 2 and the record's flag: rumble.h)
    case 81: case 82: SetSettingB("fReverseLookAround", e.id == 82); break;
    case 83: case 84: SetSettingB("fReverseAim", e.id == 84); break;
    default: break;
    }
    if (g_saveopts.fStore)
    {
        // (PC) the option into Settings.ini (display.h SAVEOPTS), which the launcher shows too
        switch (e.id)
        {
        case 78: g_saveopts.speakers = cane::settings::Speakers_Surround; break;
        case 77: g_saveopts.speakers = cane::settings::Speakers_Stereo; break;
        case 76: g_saveopts.speakers = cane::settings::Speakers_Mono; break;
        case 72: case 73: g_fControllerVibration = e.id == 72; break;
        case 81: case 82: g_saveopts.fInvertBinoc = e.id == 82; break;
        case 83: case 84: g_saveopts.fInvertTurret = e.id == 84; break;
        case 79: case 80: g_saveopts.fHeadset = e.id == 79; break;
        default: break;
        }
        if (e.id == 0x67 || e.id == 0x68 || (e.id >= 72 && e.id <= 84)) FSaveDisplaySettings(cane::userDirectory());
    }
    ApplyGameSettings(g_gamesave);   // orig 0x1D8FF8(the record): the settings take effect
}

// orig 0x001AFFC0 (the next of the option's values)
void CycleOption(MENUENT& e)
{
    const int c = e.n;
    if (c <= 0) return;
    int i = 0;
    while (i < c && IdMenuText(NElf(e.vaTab + 4 * uint32_t(i))) != e.id) ++i;
    e.id = IdMenuText(NElf(e.vaTab + 4 * uint32_t((i + 1) % c)));
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
        if (g_saveopts.fStore)
        {
            // (PC) Settings.ini's music / effects volume (the launcher's sliders: the user's factor, sound.cpp), the
            // record's mirror kept (SyncOptionsToRecord); saved when the slider is let go (ApplyOption)
            float& g = e.id == 0x67 ? g_saveopts.gMusic : g_saveopts.gEffects;
            g = std::clamp(std::round((g + float(n) * 0.1f) * 100.0f) / 100.0f, 0.0f, 1.0f);
            ApplySoundOptions();
            SyncRecordOptions();
            SetScreenObjFraction(pg.plo, 0x315, g);
            break;
        }
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

// QoL (Q4, not in the original): the data access pages show kSlotsPerPage slots of kSlotPages pages (frontend.h);
// L1 / R1 cycle the pages. The pages' entries keep their positions 0..2 (e.n, the screen's three rows 0x5CF..0x5D1
// and their looks 0x5C5..0x5CA); the slot an entry means is the page's first slot + its position. Page 1 is the
// original's three slots, and the page opens there exactly as the original's (InitEntries), so a player who never
// pages sees the original.
int s_ipageSlot = 0;
int IslotOfRow(int irow) { return s_ipageSlot * kSlotsPerPage + irow; }

// the page indicator with the previous / next page prompts ("[L1] Page 2 / 10 [R1]": the icon font's L1 / R1, drawn
// for the device in use; with Original PlayStation Icons, the PS2's)
std::string StrSlotPage()
{
    char ach[64];
    std::snprintf(ach, sizeof(ach), StrPort(PTX_SlotPage).c_str(), s_ipageSlot + 1, kSlotPages);
    return std::string("&2L&. ") + ach + " &2R&.";
}

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
    ApplyVibrationSetting();   // (0x1D54B8: on the current record)
    SAVESLOT& sl = g_aslot[islot];
    static const char* const aszFlag[] = {"lang", "nMixerMode", "uMusicVolume", "uEffectsVolume", "fUseHeadset", "dxScreenCenter",
                                          "dyScreenCenter", "fNoVibration", "fReverseLookAround", "fReverseAim", "fAutosaveWarned"};
    GAMESAVE saved = g_gamesave;
    g_gamesave = sl.fFile ? sl.save : GAMESAVE{};
    ValidateGameSave();   // (a record of another layout: cleared, as 0x1D7D30 failing keeps nothing)
    std::vector<CRef> aref;
    for (const char* sz : aszFlag) aref.push_back(RefGetProgress({"flags", sz}));
    std::memset(g_gamesave.ab, 0, 0x5400);
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
    // (VR, vr.h) VR Settings, with a headset only
    if (vr::FMenuShown())
    {
        MENUENT v;
        v.id = NID_VrPage;
        v.pfnSelect = PFN_NativeVrPage;
        a.push_back(v);
    }
    // (native) Exit to Desktop, as Sly1-Release's pause menu
    MENUENT ex;
    ex.id = NID_ExitToDesktop;
    ex.pfnSelect = PFN_NativeExit;
    a.push_back(ex);
    return a;
}

// (VR, vr.h) VR Settings: the mode, Immersive's camera rotation and vignette, the Flat Screen's size, the refresh rate,
// the world scale, recentering
std::vector<MENUENT> AentVr()
{
    std::vector<MENUENT> a;
    for (int id : {int(NID_VrMode), int(NID_VrAutoCam), int(NID_VrCutscenes), int(NID_VrVignette), int(NID_VrScreenSize),
                   int(NID_VrRefresh), int(NID_VrWorldScale), int(NID_VrRcTable), int(NID_VrRecenter)})
    {
        MENUENT e;
        e.id = id;
        e.pfnSelect = PFN_NativeVr;
        a.push_back(e);
    }
    return a;
}

void CycleVr(const MENUENT& e)
{
    vr::VROPTS& o = vr::g_vropts;
    auto nextOf = [](int n, std::initializer_list<int> an) {
        auto it = std::find(an.begin(), an.end(), n);
        return (it == an.end() || it + 1 == an.end()) ? *an.begin() : it[1];
    };
    switch (e.id)
    {
    case NID_VrMode: o.mode = o.mode == vr::VRMODE_Immersive ? vr::VRMODE_Flat : vr::VRMODE_Immersive; vr::Recenter(); break;
    case NID_VrAutoCam: o.fAutoCameraRotation = !o.fAutoCameraRotation; break;
    case NID_VrVignette: o.vignette = (o.vignette + 1) % 4; break;
    case NID_VrScreenSize: o.screenSize = (o.screenSize + 1) % 3; break;
    case NID_VrRefresh: o.refreshRate = nextOf(o.refreshRate, {0, 72, 90, 120}); vr::ApplyRefreshRate(); break;
    case NID_VrWorldScale:
    {
        const int n = nextOf(int(std::lround(o.worldScale * 100.0f)), {50, 75, 100, 125, 150, 200});
        o.worldScale = float(n) / 100.0f;
        break;
    }
    case NID_VrCutscenes: o.fCutscenesFlat = !o.fCutscenesFlat; break;
    case NID_VrRcTable: o.fRcTable = !o.fRcTable; break;
    case NID_VrRecenter: vr::Recenter(); return;
    default: return;
    }
    FSaveDisplaySettings(cane::userDirectory());
}

// (audio, notes/AUDIO_2026-11-10.md) Audio Devices...: the game's output, the microphone (recorded only while the USB
// Headset option is on), the headset's speaker (where USB Headset plays the dialogue); each row cycles "System Default"
// (or "Same as Output") and the devices present; Settings.ini's [Audio] keys, as the launcher's Settings > Audio
std::vector<MENUENT> AentAudio()
{
    std::vector<MENUENT> a;
    for (int id : {int(NID_AudioOutput), int(NID_AudioInput), int(NID_AudioHeadsetOutput)})
    {
        MENUENT e;
        e.id = id;
        e.pfnSelect = PFN_NativeAudio;
        a.push_back(e);
    }
    return a;
}

void CycleAudio(const MENUENT& e)
{
    std::string* pname = e.id == NID_AudioOutput ? &g_saveopts.outputDevice
                       : e.id == NID_AudioInput  ? &g_saveopts.inputDevice
                       : e.id == NID_AudioHeadsetOutput ? &g_saveopts.headsetOutputDevice : nullptr;
    if (!pname) return;
    const cane::audio::DeviceList list = ListAudioDevices();
    const std::vector<std::string> choices = cane::audio::deviceChoices(e.id == NID_AudioInput ? list.inputs : list.outputs, *pname);
    size_t i = 0;
    for (size_t k = 0; k < choices.size(); ++k)
        if (choices[k] == cane::audio::cleanName(*pname)) i = k;
    *pname = choices[(i + 1) % choices.size()];
    ApplySoundOptions();   // (display.h: SetAudioDevices)
    FSaveDisplaySettings(cane::userDirectory());
}

// (lang) Language...: Text Language (the languages whose text this disc can draw) and Voice Language (Same as Text,
// then the languages with voices); chosen here, they apply when the next world loads (its text and lines are read in
// them) and are Settings.ini's [Language] Language / Voices
std::vector<MENUENT> AentLanguage()
{
    std::vector<MENUENT> a;
    for (int id : {int(NID_TextLanguage), int(NID_VoiceLanguage)})
    {
        MENUENT e;
        e.id = id;
        e.pfnSelect = PFN_NativeLanguageRow;
        a.push_back(e);
    }
    return a;
}

void CycleLanguage(const MENUENT& e)
{
    int langText = -2, langVoice = -2;
    FLanguagesPending(&langText, &langVoice);
    if (langText == -2) langText = g_nLang;
    if (langVoice == -2) langVoice = g_nLangVoice;
    std::vector<int> achoice;
    if (e.id == NID_VoiceLanguage) achoice.push_back(-1);   // (Same as Text)
    for (int i = 0; i < CLanguages(); ++i)
    {
        const int lang = LanguageOfIndex(i);
        if (e.id == NID_TextLanguage ? FLanguageTextAvailable(lang) : FLanguageVoiceAvailable(lang)) achoice.push_back(lang);
    }
    if (achoice.empty()) return;
    int& cur = e.id == NID_TextLanguage ? langText : langVoice;
    size_t i = 0;
    for (size_t k = 0; k < achoice.size(); ++k)
        if (achoice[k] == cur) i = k + 1;
    cur = achoice[i % achoice.size()];
    // (nothing pending when the choice is what plays now)
    const int curText = langText == g_nLang ? -2 : langText;
    const int curVoice = langVoice == g_nLangVoice ? -2 : langVoice;
    SetPendingLanguages(curText, curVoice);
    std::printf("language: text %s, voices %s from the next world\n", SzLanguage(langText),
                langVoice < 0 ? "as the text" : SzLanguage(langVoice));
    if (g_saveopts.fStore)
    {
        g_saveopts.language = SzLanguageModName(langText);
        g_saveopts.voiceLanguage = langVoice < 0 ? std::string() : std::string(SzLanguageModName(langVoice));
        FSaveDisplaySettings(cane::userDirectory());
    }
}

// (native) the PC display options' page
std::vector<MENUENT> AentDisplayOptions()
{
    std::vector<MENUENT> a;
    for (int id = NID_Aspect; id <= NID_DrawDistance + 1; ++id)
    {
#if defined(__SWITCH__) || defined(__ANDROID__)
        if (id == NID_Window) continue;   // (the Switch / Android: the system places the window, DeX included)
#endif
        MENUENT e;
        e.id = id <= NID_DrawDistance ? id : NID_LongShadows;   // (then Long Shadows, settings v16)
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
    // (lang) Language... (two or more languages: the text and the voices apart)
    if (CLanguages() >= 2)
    {
        e.id = NID_LanguagePage;
        e.pfnSelect = PFN_NativeLanguagePage;
        a.push_back(e);
    }
    // (audio) Audio Devices... (desktop builds, with Settings.ini; not on the Switch / Android)
    if (FAudioDeviceChoiceShown() && g_saveopts.fStore)
    {
        e.id = NID_AudioPage;
        e.pfnSelect = PFN_NativeAudioPage;
        a.push_back(e);
    }
    // (native) Exit to Desktop on the title's data access menu (Select) too, as the pause menu has it (in a game this
    // page is reached from the pause menu, which has it)
    if (fTitle)
    {
        MENUENT ex;
        ex.id = NID_ExitToDesktop;
        ex.pfnSelect = PFN_NativeExit;
        a.push_back(ex);
    }
    return a;
}

// (native) the controls page and its keyboard mapping
std::vector<MENUENT> AentControls()
{
    std::vector<MENUENT> a;
    MENUENT remap;   // (first: every binding on one roomy page)
    remap.id = NID_RemapPage;
    remap.pfnSelect = PFN_RemapPage;
    a.push_back(remap);
    MENUENT advanced;
    advanced.id = NID_AdvancedControls;
    advanced.pfnSelect = PFN_NativeControls;
    a.push_back(advanced);
    for (int id = NID_PromptStyle; id <= NID_KeyboardPage; ++id)
    {
        if (id == NID_InvertY) continue;
        // (the keys are bound on the Remap page; this row is the advanced mapping's devices, only while that is on)
        if (id == NID_KeyboardPage && !sly2actions::profile.enabled) continue;
        MENUENT e;
        e.id = id == NID_InvertX ? NID_InvertPage : id;   // (the mouse's inversion: on Invert Controls..., with the rest)
        e.pfnSelect = id == NID_KeyboardPage ? PFN_NativeKeyboardPage : id == NID_InvertX ? PFN_NativeInvertPage : PFN_NativeControls;
        if (id == NID_KeyboardPage && sly2actions::profile.enabled) { e.id = NID_ActionDevices; e.pfnSelect = PFN_ActionDevices; }
        a.push_back(e);
        if (id == NID_InstantSkip)
        {
            MENUENT cameraRotation;
            cameraRotation.id = NID_AutoCameraRotation;
            cameraRotation.pfnSelect = PFN_NativeControls;
            a.push_back(cameraRotation);
        }
    }
    return a;
}
// (native, settings v17) Invert Controls...: the controller's camera, the game's own binocucom / turret options (the
// save's, as on the options page), then the mouse in each view (off: mouse up looks up, mouse right turns right)
std::vector<MENUENT> AentActionDevices()
{
    std::vector<MENUENT> rows;
    for (bool controller : {false, true}) {
        MENUENT e;
        e.id = controller ? NID_ActionController : NID_ActionKeyboard;
        e.n = controller ? 1 : 0;
        e.pfnSelect = PFN_ActionGroups;
        rows.push_back(e);
    }
    MENUENT reset;
    reset.id = NID_ResetActions;
    reset.pfnSelect = PFN_NativeControls;
    rows.push_back(reset);
    return rows;
}
std::vector<MENUENT> AentActionGroups(bool controller)
{
    std::vector<MENUENT> rows;
    const auto groups = ActionGroups();
    for (size_t i = 0; i < groups.size(); ++i) {
        MENUENT e;
        e.id = NID_ActionGroupFirst + int(i);
        e.n = int(i) + (controller ? 1000 : 0);
        e.pfnSelect = PFN_ActionRows;
        rows.push_back(e);
    }
    return rows;
}
std::vector<MENUENT> AentActionRows(int encodedGroup)
{
    std::vector<MENUENT> rows;
    const bool controller = encodedGroup >= 1000;
    const auto groups = ActionGroups();
    const std::string& group = groups[size_t(encodedGroup % 1000)];
    for (size_t i = 0; i < cane::sly2input::kActionCount; ++i) {
        if (group != cane::sly2input::kActions[i].group) continue;
        MENUENT e;
        e.n = int(i) + (controller ? int(cane::sly2input::kActionCount) : 0);
        e.id = NID_ActionBindingFirst + e.n;
        e.pfnSelect = PFN_ActionCapture;
        rows.push_back(e);
    }
    return rows;
}
std::vector<MENUENT> AentInvert()
{
    std::vector<MENUENT> a;
    auto add = [&](int id) {
        MENUENT e;
        e.id = id;
        e.pfnSelect = PFN_NativeControls;
        a.push_back(e);
    };
    add(NID_PadInvertX);
    add(NID_PadInvertY);
    for (const MENUENT& e : AentFromElf(0x2BFEC8, 11))
        if (e.id == 0x51 || e.id == 0x53) a.push_back(e);   // (Invert Binocucom / Turret Controls: 0x1AFFC0 cycles them)
    for (int id : {int(NID_InvertX), int(NID_InvertY), int(NID_BinocInvertX), int(NID_BinocInvertY), int(NID_TurretInvertX),
                   int(NID_TurretInvertY)})
        add(id);
    return a;
}
std::vector<MENUENT> AentKeyboard()
{
    std::vector<MENUENT> a;
    for (int i = 0; i < PCA_Max + 6; ++i)
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
            // orig 0x001AD538: the selection on the first fitting slot (of page 1, the original's three slots)
            float ag[3];
            for (int i = 0; i < 3; ++i) ag[i] = DtSlot(i);
            const int kind = pg.vaTab == 0x2BFDA8 ? 0x1E : pg.vaTab == 0x2BFE08 ? 0x1F : pg.vaTab == 0x2BFE68 ? 0x19 : -1;
            s_ipageSlot = 0;
            if (kind == 0x1E) { for (int j = 0; j < 3; ++j) if (ag[j] == 0.0f) { pg.isel = j; break; } }
            else if (kind == 0x1F)
            {
                int islot = -1;
                for (int j = 0; j < 3; ++j) if (ag[j] != 0.0f) { islot = j; break; }
                // (QoL Q4: none on page 1: the first game on the other pages)
                for (int j = 3; islot < 0 && j < kSlotMax; ++j) if (DtSlot(j) != 0.0f) islot = j;
                if (islot >= 0) { s_ipageSlot = islot / kSlotsPerPage; pg.isel = islot % kSlotsPerPage; }
            }
            else if (kind == 0x19 && g_islotChosen >= 0)
            {
                // (QoL Q4: the page of the slot played from)
                s_ipageSlot = g_islotChosen / kSlotsPerPage;
                pg.isel = g_islotChosen % kSlotsPerPage;
            }
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
        // (0x1B0740: Sly 0x35 -> 0x2C01D0, Bentley 0x37 -> 0x2C01F0, Murray 0x36 -> 0x2C0210; R62: was swapped)
        const uint32_t vaAnim = cid == 0x35 ? 0x2C01D0 : cid == 0x37 ? 0x2C01F0 : cid == 0x36 ? 0x2C0210 : 0;
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
        const int islot = IslotOfRow(e.n);   // (QoL Q4: on the current page)
        if (DtSlot(islot) == 0.0f)
        {
            g_islotChosen = islot;
            ResetSlotKeepSettings(islot);
            StartGameFromSave(s_psw, &g_aslot[islot].save);
        }
        else
        {
            PsndStartSound(0x36);  // (0x1AD250: the error sound)
            e.grf |= 2;
        }
        break;
    }
    case 0x1AD320:                                                       // Load Game from slot n
    {
        const int islot = IslotOfRow(e.n);
        g_islotChosen = islot;
        if (DtSlot(islot) == 0.0f)
        {
            ResetSlotKeepSettings(islot);
            StartGameFromSave(s_psw, &g_aslot[islot].save);
        }
        else
        {
            StartGameFromSave(s_psw, &g_aslot[islot].save);
            e.grf |= 2;
        }
        break;
    }
    case 0x1AD480:                                                       // Save Game to slot n
        s_islotTarget = IslotOfRow(e.n);
        if (DtSlot(s_islotTarget) == 0.0f) SaveToTarget();
        else ShowCardMessage(0x1C);
        e.grf |= 2;
        break;
    case PFN_NativeDisplay: CycleNative(e); e.grf |= 2; break;
    case PFN_NativeControls: CycleControls(e); e.grf |= 2; break;
    case PFN_NativeVr: CycleVr(e); e.grf |= 2; break;
    case PFN_NativeAudio: CycleAudio(e); e.grf |= 2; break;
    case PFN_NativeLanguageRow: CycleLanguage(e); e.grf |= 2; break;
    case PFN_NativeLanguagePage:
        ReplacePage(AentLanguage(), 0);
        e.grf |= 2;
        break;
    case PFN_NativeAudioPage:
        ReplacePage(AentAudio(), 0);
        e.grf |= 2;
        break;
    case PFN_NativeVrPage:
        ReplacePage(AentVr(), 0);
        e.grf |= 2;
        break;
    case PFN_ActionDevices:
        ReplacePage(AentActionDevices(), 0);
        e.grf |= 2;
        break;
    case PFN_ActionGroups:
        ReplacePage(AentActionGroups(e.n != 0), 0);
        e.grf |= 2;
        break;
    case PFN_ActionRows:
        ReplacePage(AentActionRows(e.n), 0);
        e.grf |= 2;
        break;
    case PFN_ActionCapture:
        if (!sly2actions::profile.readOnly && g_gl.window) {
            s_iActionCapture = e.n % int(cane::sly2input::kActionCount);
            s_fActionController = e.n >= int(cane::sly2input::kActionCount);
            s_tActionCapture = g_clock.tReal;
            KeyBindingPressed(g_gl.window);
            sly2actions::CaptureController(g_gl.window);
        }
        e.grf |= 2;
        break;
    // (PC menus) the native pages swap in place of the current one, as the pause menu's Options... does
    // (0x1B04E0: ReplacePage, the entries' swap 0x14 / 0xC, back the same way); pushed, they replayed the whole pause
    // menu's opening (0x0) and closed it on the way back
    case PFN_RemapPage:
        OpenRemapPage();
        e.grf |= 2;
        break;
    case PFN_NativeControlsPage:
        ReplacePage(AentControls(), 0);
        e.grf |= 2;
        break;
    case PFN_NativeKeyboardPage:
        ReplacePage(AentKeyboard(), 0);
        e.grf |= 2;
        break;
    case PFN_NativeInvertPage:
        ReplacePage(AentInvert(), VA_NativeInvertPage);
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
        ReplacePage(AentDisplayOptions(), 0);
        e.grf |= 2;
        break;
    case PFN_NativeExit:
        if (g_gl.window) glfwSetWindowShouldClose(g_gl.window, 1);
        break;
    case PFN_NativeLanguage:
        // orig (Europe) 0x1DDCA8: the language chosen (it wins over a save's: ApplyGameSettings), then the title's world
        // again in it (0x1DD8E0's transition: the current world, the flags 1)
        // (PC) the text and the voices both (the pause menu's Language page can set them apart): region.h
        ChooseLanguageOnTitle(e.n);
        std::printf("language: %s chosen\n", SzLanguage(e.n));
        // (PC) remembered in Settings.ini (the launcher shows and sets the same): the next start begins in it
        if (g_saveopts.fStore)
        {
            g_saveopts.language = SzLanguageModName(e.n);
            g_saveopts.voiceLanguage.clear();
            FSaveDisplaySettings(cane::userDirectory());
        }
        CloseMenu();
        RequestTransition(s_psw, g_disc.PbElf(VA_FK_BOOT_WORLD, 64), 0x189, -1, 1, -1);
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
// the save folder made (orig the thread 0x2A6D88 after 0x15's Yes: the card's directory, its icon and the three slot
// files, each 0x5408 zero bytes); false when it could not be made
bool FCreateSaveFolder()
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cane::path(g_aslot[0].path)).parent_path(), ec);
    bool fOk = true;
    for (int i = 0; i < kSlotsCard; ++i)   // (the original's three: the QoL's extra slots are made when saved to)
    {
        SAVESLOT& sl = g_aslot[i];
        std::string err;
        if (sl.fFile) continue;
        if (FWriteGameSaveBlock(sl.path, GAMESAVE{}, err))
        {
            sl.save = GAMESAVE{};
            sl.fFile = true;
            sl.tWrite = std::filesystem::last_write_time(cane::path(sl.path), ec).time_since_epoch().count();
        }
        else
        {
            std::printf("card: %s\n", err.c_str());
            fOk = false;
        }
    }
    RechooseSaveSlots();
    return fOk;
}

// orig 0x001AD168 (yes)
void CardYes()
{
    switch (s_card.msg)
    {
    case 0x1C: SaveToTarget(); break;
    case 0x15:
        // the save folder made, then 0x16 "creating" (natively unreached: UpdateCardStatus makes it unasked, Q2)
        FCreateSaveFolder();
        ShowCardMessage(0x16);
        break;
    case 0x20:   // the erase (orig 0x1AAF90 with the erase function, 0x21)
        if (s_islotTarget >= kSlotsCard)
        {
            // (QoL Q4: an extra slot's file goes: the slot is empty again)
            SAVESLOT& sl = g_aslot[s_islotTarget];
            std::error_code ec;
            std::filesystem::remove(cane::path(sl.path), ec);
            sl.save = GAMESAVE{};
            sl.fFile = false;
            sl.tWrite = 0;
            RechooseSaveSlots();
        }
        else if (s_islotTarget >= 0)
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
// while no slot file exists in <user-dir>/Saves; the load page with no folder says so (0x2E).
//
// QoL (Q2, not in the original): a PC has no memory card, so the card's questions about it are not asked. The
// original's states 1 (no card: 0x11), 2 (unformatted: 0x12 / 0x14) and 4 (full: 0x18) never happen here, and
// state 3 (no Sly 2 save file: 0x15 "... requires 100 KB ... Do you want to create a Sly 2 Save File?") is answered
// Yes by itself: the save folder and save1..3 are made as the original's Yes makes them, without the "creating"
// notice (0x16). Only when that fails is the original's error (0x17) shown, and the game then goes on without saves
// (as the original's No). The real choices stay: overwrite (0x1C), erase (0x20), the auto-save notice (0x22).
// SLY2N_CARDPROMPT=1 (test): the original's question instead.
void UpdateCardStatus()
{
    if (g_transition.fPending) return;
    if (g_ncardMode != 1 || s_card.fShown) return;
    if (g_clock.tReal - s_tCardState < 0.3f) return;
    const bool fFolder = g_aslot[0].fFile || g_aslot[1].fFile || g_aslot[2].fFile;
    static const bool s_fCardPrompt = std::getenv("SLY2N_CARDPROMPT") != nullptr;
    if (!fFolder && !s_fCardPrompt)
    {
        if (FCreateSaveFolder()) std::printf("card: the save folder made (Q2: no question)\n");
        else
        {
            g_ncardMode = 2;
            ShowCardMessage(0x17);
        }
        return;
    }
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

void SyncOptionsToRecord() { SyncRecordOptions(); }

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
int NMenuState() { return s_menu.state; }
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

// orig (Europe) 0x1DDFB8 (the USA build has the same menu inline in the title's input 0x1DD920, unreachable with
// its one language): the disc's languages from the current one on, each its name (the text 0x2C35B8[language]),
// on the title's language screen object 0x5D6.
//
// This is where the language is chosen. (PC, lang 2026-11-10) The list is the disc's languages and the shared
// languages folder's (langstore.h: other regions' installations, imports, user-made dubs and subtitles), so the
// menu is offered on the USA and Japanese discs too as soon as there are two; the rest of the port only reads
// g_nLang (region.h).
void OpenLanguageMenu(SW* psw, bool fCanCancel)
{
    s_psw = psw;
    LO* plo = psw ? PloFindSwObjectByOid(psw, 0x5D6) : nullptr;
    const int c = CLanguages();
    if (!plo && c >= 2) std::printf("language: the title's language menu object 0x5D6 is missing\n");
    if (!plo || c < 2 || s_menu.fShown) return;
    int icur = 0;
    for (int i = 0; i < c; ++i)
        if (LanguageOfIndex(i) == g_nLang) icur = i;
    std::vector<MENUENT> aent;
    for (int i = 0; i < c; ++i)
    {
        const int lang = LanguageOfIndex((i + icur) % c);
        MENUENT e;
        // (the game's 14 by the disc's text 0x58.. in the current language; a custom one by its own name)
        e.id = lang < LANG_Max ? 0x58 + lang : NID_LanguageFirst + lang;   // (the USA ids the table 0x2C35B8 holds)
        // (a language whose text this disc cannot show, langstore.h: named "... (voices only)")
        if (FLangStoreVoicesOnly(lang)) e.id = NID_LanguageFirst + lang;
        e.pfnSelect = PFN_NativeLanguage;
        e.n = lang;
        aent.push_back(e);
    }
    // (PC: the Japanese title world's language screen 0x5D6 draws nothing (the build never shows it); the menu's own
    // wheel 0x370 with the options' animations instead)
    uint32_t vaAnim = 0x2C35F0;
    if (g_disc.Region() == DISCREGION::Japan && s_menu.ploDefault) vaAnim = 0x2BFD38;
    PushPage(std::move(aent), 0, plo, vaAnim);
    if (PAGE* pg = PpageCur()) pg->fCanCancel = fCanCancel;
}

bool FCardSettled()
{
    const bool fFolder = g_aslot[0].fFile || g_aslot[1].fFile || g_aslot[2].fFile;
    return !s_card.fShown && (g_ncardMode != 1 || fFolder);
}

void UpdateMenu(SW* psw)
{
    s_psw = psw;
    UpdateCardStatus();
    UpdateCard();
    if (!s_menu.fShown || s_card.fShown) { s_ikeyCapture = s_iActionCapture = -1; CloseRemapPage(); return; }
    PAGE* pe = PpageCur();
    if (!pe) return;
    // (tests) SLY2N_REMAPPAGE=1: the Remap page opens with the menu
    if (static bool s_fTestRemap = std::getenv("SLY2N_REMAPPAGE") != nullptr; s_fTestRemap && s_menu.state == 7)
    {
        s_fTestRemap = false;
        OpenRemapPage();
    }
    // the Remap page over the menu takes the input while it is open
    if (FRemapPageOpen()) { UpdateRemapPage(); return; }
    if (s_iActionCapture >= 0) {
        GrfbtnTakePressed(0xFFFF);
        const int key = g_gl.window ? KeyBindingPressed(g_gl.window) : GLFW_KEY_ESCAPE;
        if (key == GLFW_KEY_ESCAPE || (s_fActionController && g_clock.tReal - s_tActionCapture > 6)) s_iActionCapture = -1;
        else {
            const bool clear = key == GLFW_KEY_DELETE;
            const int binding = clear ? -1 : s_fActionController ? sly2actions::CaptureController(g_gl.window) : key;
            if (clear || binding >= 0) {
                auto& bindings = s_fActionController ? sly2actions::profile.gamepad : sly2actions::profile.keyboard;
                bindings[size_t(s_iActionCapture)] = binding;
                sly2actions::Save(cane::userDirectory());
                s_iActionCapture = -1;
            }
        }
        return;
    }
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
        // (PC) a page shown again shows its options' current values (Invert Controls... changes the options page's
        // Invert Binocucom / Turret Controls; 0x1AF9C8 only reads the settings)
        if (PAGE* pgNext = PpageCur())
            for (MENUENT& e : pgNext->aent)
                if (e.pfnInit == 0x1AF9C8) InitOptionText(e);
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
        // (the Japanese build: Circle selects, Cross goes back, Square erases: region.h)
        const uint32_t grf = GrfbtnTakePressed(0x800 | BtnMenuBack() | BtnMenuConfirm() | BtnMenuErase() | (FDataAccess() ? 0xC : 0));
        if (grf & 0x800) { if (pe->fCanCancel) CloseMenu(); return; }
        if (grf & BtnMenuBack()) { if (pe->fCanCancel) confirm(); return; }
        if (pe->aent.empty() && grf != 0) { confirm(); return; }
        if (s_menu.depth > 0 && (grf & BtnMenuConfirm()) && !pe->aent.empty())
        {
            MENUENT& e = pe->aent[size_t(pe->isel)];
            SelectEntry(e);
            if (e.grf & 2) { e.grf &= ~2u; return; }
            if (!(e.grf & 1)) { CloseMenu(); return; }
            confirm();
            return;
        }
        if (grf & BtnMenuErase())
        {
            // the erase of a slot (Circle, on the new / load pages; the Japanese build's Square)
            const int r = KindDataAccess();
            if (r != 0x19 && r != 0x1E) return;
            if (DtSlot(IslotOfRow(pe->isel)) == 0.0f) return;
            s_islotTarget = IslotOfRow(pe->isel);
            ShowCardMessage(0x20);
            return;
        }
        if (FDataAccess() && (grf & (0x4 | 0x8)))
        {
            // (QoL Q4) L1 / R1: the previous / next page of slots (cycling), the selection kept on its row
            s_ipageSlot = (s_ipageSlot + ((grf & 0x8) ? 1 : kSlotPages - 1)) % kSlotPages;
            PsndStartSound(0x35);   // (the shop's row step, 0x1DC260)
            if (std::getenv("SLY2N_MENULOG")) std::printf("menu: slot page %d\n", s_ipageSlot + 1);
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
        const uint32_t grf = GrfbtnTakePressed(0x800 | BtnMenuBack() | BtnMenuConfirm());
        if (grf & 0x800) { SetMenuState(9); CloseMenu(); }   // orig 0x001AFF88 + close
        else if (grf & BtnMenuBack()) SetMenuState(9);       // orig 0x001AFF88
        else if (grf & BtnMenuConfirm()) ApplyOption(e);
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

static const MENUENT* PmenuWheelEntry(int id)
{
    const PAGE* page = PpageCur();
    if (id < 0x2D2 || id > 0x2DB || !page || page->aent.empty()) return nullptr;
    const int count = int(page->aent.size());
    const int index = id - 0x2D2 + page->isel - 3;
    return &page->aent[size_t((index % count + count) % count)];
}

bool FMenuActionPrompt(int textId, int& binding, bool& controller)
{
    const MENUENT* entry = PmenuWheelEntry(textId);
    const int count = int(cane::sly2input::kActionCount);
    if (!FMenuShown() || !entry || entry->id < NID_ActionBindingFirst || entry->id >= NID_ActionBindingFirst + 2 * count)
        return false;
    const int encoded = entry->id - NID_ActionBindingFirst;
    const int index = encoded % count;
    controller = encoded >= count;
    if (s_iActionCapture == index && s_fActionController == controller) return false;
    binding = (controller ? sly2actions::profile.gamepad : sly2actions::profile.keyboard)[size_t(index)];
    return binding >= 0;
}

std::string StrMenuScript(int id)
{
    char ach[128];
    auto strText = [&](int idText) {
        if (idText >= NID_Base) return StrNative(idText);
        // (Invert Controls...: the game's Invert Binocucom / Turret Controls are the controller's: the left stick's icon)
        const PAGE* pg = PpageCur();
        if (pg && pg->vaTab == VA_NativeInvertPage && idText >= 0x51 && idText <= 0x54) return "&2(&. " + StrLocalized(s_psw, IdText(idText));
        return StrLocalized(s_psw, IdText(idText));
    };
    // orig 0x00211298 (0x2D2..0x2DB): the wheel's entry k around the selection (3: the selected one)
    if (id >= 0x2D2 && id <= 0x2DB)
        if (const MENUENT* entry = PmenuWheelEntry(id)) return strText(entry->id);
    switch (id)
    {
    case 0x5CB: { const int k = KindDataAccess(); return k < 0 ? "" : StrLocalized(s_psw, IdText(k)); }   // orig 0x001AD928
    case 0x5CC:
    {
        // orig 0x001AD980: the current game's status (not at the title)
        if (FTitleShown()) return "";
        const int iep = NRef(RefGetProgress({"episodeBest"}));
        std::snprintf(ach, sizeof(ach), "%s : %d%%", StrLocalized(s_psw, IdText(0x6D + std::clamp(iep, 0, 8))).c_str(), NGamePercent(g_gamesave));
        return ach;
    }
    case 0x5CD:
    {
        // orig 0x001ADA20: the selected slot's help
        const PAGE* pe = PpageCur();
        if (!pe || !FDataAccess()) return "";
        const float g = DtSlot(IslotOfRow(pe->isel));
        switch (KindDataAccess())
        {
        case 0x1E: return StrLocalized(s_psw, IdText(g != 0.0f ? 0x25 : 0x26));
        case 0x1F: return StrLocalized(s_psw, IdText(g != 0.0f ? 0x27 : 0x26));
        case 0x19: return StrLocalized(s_psw, IdText(0x29)) + (g != 0.0f ? "\n" + StrLocalized(s_psw, IdText(0x25)) : "");
        default: return "";
        }
    }
    case 0x5CE:
        // orig 0x001ADBB8; (QoL Q4) the page indicator and its L1 / R1 prompts on a second line under Back (the help
        // on the left has two lines on the save page too), so Back stays where it was
        return FDataAccess() ? StrLocalized(s_psw, IdText(0x40)) + " : " + SzMenuBackGlyph() + "\n" + StrSlotPage() : "";
    case 0x5CF: case 0x5D0: case 0x5D1:
    {
        // orig 0x001ADC40: slot i (its episode and percent, else New Game / Empty)
        const int i = IslotOfRow(id - 0x5CF);   // (QoL Q4: the current page's)
        if (DtSlot(i) != 0.0f)
        {
            CRef r = RefProgressOfSlot(i, {"episodeBest"});
            if (r.m_tagk == TAGK_Nil) r = RefProgressOfSlot(i, {"episodeCurrent"});
            const int iep = std::clamp(NRef(r), 0, 8);
            const int pct = NGamePercent(g_aslot[i].save);   // (0x1D82B8)
            std::snprintf(ach, sizeof(ach), "%s : %d%%", StrLocalized(s_psw, IdText(0x6D + iep)).c_str(), pct);
            return ach;
        }
        const int k = KindDataAccess();
        if (k == 0x19) return StrLocalized(s_psw, IdText(0x45));
        if (k == 0x1E || k == 0x1F) return StrLocalized(s_psw, IdText(0x44));
        return "";
    }
    case 0x2F2: return s_card.msg >= 0 ? StrLocalized(s_psw, IdText(s_card.msg)) : "";   // orig 0x001AD7E0
    case 0x2F3:
        // orig 0x001AD820
        switch (NCardPrompt(s_card.msg))
        {
        case 1: return "&2S&.: " + StrLocalized(s_psw, IdText(0x38));
        case 2: return "&2S&. : " + StrLocalized(s_psw, IdText(0x36)) + "       " + StrLocalized(s_psw, IdText(0x37)) + " : &2T&.";
        default: return "";
        }
    default: return "";
    }
}

// orig: 0x001D8FF8 func_001D8FF8: a record's settings take effect: the language (0x1A2BD0 when the disc has two or more:
// the USA disc has one), the mixer mode (0x1FE9C8), the music volume (factor 1 of channel 1) and the effects' (channels 0
// and 3) as 1 - the stored values, the headset (D_003DD050, the sound manager's USB headset flag) and the screen centre (the GS display's
// offsets +636 / +50: none on PC)
void ApplyGameSettings(const GAMESAVE& gs)
{
    GAMESAVE saved = g_gamesave;
    g_gamesave = gs;
    const CRef rMix = RefSetting("nMixerMode");
    const CRef rMusic = RefSetting("uMusicVolume");
    const CRef rEffects = RefSetting("uEffectsVolume");
    g_gamesave = saved;
    // the language (0x1A2BD0) when the disc has two or more; the European build (0x1D92B8): the one chosen on the title
    // this session wins and is the record's
    if (CLanguages() >= 2)
    {
        const CRef rLang = [&] { GAMESAVE s0 = g_gamesave; g_gamesave = gs; CRef r = RefSetting("lang"); g_gamesave = s0; return r; }();
        if (g_nLangChosen != -1)
        {
            if (&gs == &g_gamesave) SetSettingN("lang", g_nLangChosen);
            SetLanguage(g_nLangChosen);
        }
        else if (rLang.m_tagk != TAGK_Nil && FLanguageAvailable(NRef(rLang))) SetLanguage(NRef(rLang));
    }
    if (g_saveopts.fStore)
    {
        // (PC) the options are Settings.ini's (display.h SAVEOPTS), not a record's: the current record mirrors them
        SyncOptionsToRecord();
        SetSoundPlaybackMode(g_saveopts.speakers);
        ApplySoundOptions();
        g_sound.fEffect = g_saveopts.fHeadset ? 1 : 0;   // (D_003DD050 = the headset option, as below)
        return;
    }
    // the headset (D_003DD050 = flags "fUseHeadset": sound.cpp UpdateMicNoise, spiop's headset tracks)
    if (const CRef r = [&] { GAMESAVE s0 = g_gamesave; g_gamesave = gs; CRef rr = RefSetting("fUseHeadset"); g_gamesave = s0; return rr; }();
        r.m_tagk != TAGK_Nil)
        g_sound.fEffect = NRef(r);
    if (rMix.m_tagk != TAGK_Nil) SetSoundPlaybackMode(NRef(rMix));
    if (rMusic.m_tagk != TAGK_Nil) SetSoundChannelVolume(1, 1, 1.0f - GRef(rMusic));
    if (rEffects.m_tagk != TAGK_Nil)
    {
        SetSoundChannelVolume(0, 1, 1.0f - GRef(rEffects));
        SetSoundChannelVolume(3, 1, 1.0f - GRef(rEffects));
    }
}
