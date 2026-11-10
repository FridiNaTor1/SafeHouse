// See menu.h. Sly 3's menu module (orig3 0x1D2C20..0x1D57B8: Sly 2's 0x1AE928..0x1B0B78 with Sly 3's tables, parts
// and text ids), ported over Sly 2's port of it (the PC pages, the slots QoL). The widget is orig3 0x46F550:
// +0x30 the default page's screen object (oid 1056, an external screen file), +0x34 the second (1057), +0x38 the
// depth, +0x20 + 0x1C k the pages, +0x1FC the state, +0x200 the pending depth. Functions: init 0x1D2C20, the HUD's
// reset 0x1D2D10, show / hide 0x1D2D90 / 0x1D2DF8, SetMenuState 0x1D2ED0, UpdateMenu 0x1D3288, ReplacePage 0x1D3818,
// BackReplaced 0x1D3918, PushPage 0x1D3960, PopPage 0x1D3A90, CloseMenu 0x1D3AE8, StartSlider 0x1D3B28, the slider
// 0x1D3B58, InitOptionText 0x1D3DF0, ApplyOption 0x1D3FC0, CycleOption 0x1D4510, KindDataAccess 0x1D45B0, the
// pause menu 0x1D48D0 (table 0x345DA0), the options 0x1D4C48 (0x345B40), the title's SELECT 0x1D4BF8; the slots
// 0x1D1470 / 0x1D1568 / 0x1D1708 (pages 0x345990 / 0x3459F0 / 0x345A50), their strings 0x1D1B98..0x1D1EF8. The card
// dialog is card3.h (orig3 0x3751D0, screen object 1466): this module's messages go through it.

#include "rumble.h"
#include "game/sly3.h"
#include "menu.h"
#include "epmenu3.h"
#include "card3.h"
#include "pcinput.h"
#include "porttext.h"
#include "region.h"
#include "screen3.h"
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
#include "player3.h"
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
int32_t NElf(uint32_t va) { return int32_t(g_disc.U32Elf(va)); }
float GRef(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? float(r.m_n) : 0.0f; }
int NRef(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }

// the settings (Sly 3's progress layout: "flags" {lang, nMixerMode, fAutosaveWarned, uMusicVolume, uEffectsVolume,
// fUseHeadset, dxScreenCenter, dyScreenCenter, joyLeft / joyRight {fNoVibration, mpcamprefkfReverseHorizontal[k],
// mpcamprefkfReverseVertical[k]}}; the pad's own under joyLeft, the first player's)
CRef RefSetting(const char* name) { return RefGetProgress({"flags", name}); }
void SetSettingF(const char* name, float g) { CRef r; r.SetF32(g); FSetProgress({"flags", name}, r); }
void SetSettingN(const char* name, int n) { CRef r; r.SetS32(n); FSetProgress({"flags", name}, r); }
void SetSettingB(const char* name, bool f) { CRef r; r.SetBool(f); FSetProgress({"flags", name}, r); }

// ------------------------------------------------------------ Sly 3's numbers
namespace m3
{
// the executable's tables (0x20-byte entries, Sly 2's layout)
constexpr uint32_t VA_PAUSE = 0x345DA0;        // 11: the pause menu (0x1D48D0)
constexpr uint32_t VA_OPTIONS = 0x345B40;      // 11: the options (0x1D4C48)
constexpr uint32_t VA_ANIM = 0x345920;         // the default page's animations (9 words: +0x20 the episode's idle)
constexpr uint32_t VA_ANIM_CTRLHELP = 0x345F30;// Control Help's (0x1D5058)
constexpr uint32_t VA_NEW = 0x345990, VA_LOAD = 0x3459F0, VA_SAVE = 0x345A50;   // the slots' pages (3 each)
constexpr uint32_t VA_QUIT = 0x345CA0, VA_SAVEQUIT = 0x345AB0;   // Quit Game / Save and Quit's yes / no (0x1D5320)
constexpr uint32_t VA_CAMOPT = 0x345B00;       // 2: a camera's Left/Right, Up/Down (0x1D51D8)
constexpr uint32_t VA_CAMKINDS = 0x345F90;     // the camera kinds' "... Options…" texts (0x1D5178)
constexpr uint32_t VA_EPISODE_IDLE = 0x345F00; // the pause page's idle part per episode (0x1D2E70)
constexpr uint32_t VA_CTRLHELP_PARTS = 0x345F58;   // 13: Control Help's parts per job line (0x1D5058)
// the select / init / update functions (the entries' +0x04 / +0x14 / +0x10)
constexpr uint32_t FN_KEEP_PLAYING = 0x1D47E8, FN_PUSH = 0x1D4FA0, FN_YESNO = 0x1D4FF0, FN_NO = 0x1D4F80,
                   FN_YES_QUIT = 0x1D5398, FN_YES_EXIT = 0x1D5598, FN_YES_EXIT2P = 0x1D5658, FN_YES_ABANDON = 0x1D5540,
                   FN_RESET3D = 0x1D54A0, FN_OPTIONS = 0x1D4C48, FN_CTRLHELP = 0x1D5058, FN_JOBHELP = 0x1D5228,
                   FN_OTHERJOBS = 0x1D4E10, FN_CYCLE = 0x1D4510, FN_SLIDER = 0x1D5038, FN_CAMOPT = 0x1D51D8,
                   FN_TWOPLAYER = 0x1D5698, FN_SLOT_NEW = 0x1D1470, FN_SLOT_LOAD = 0x1D1568, FN_SLOT_SAVE = 0x1D1708;
constexpr uint32_t FN_INIT_OPTION = 0x1D3DF0, FN_INIT_SLOTS = 0x1D17F0, FN_INIT_CAMKIND = 0x1D5178;
constexpr uint32_t FN_UPD_SLOT = 0x1D19D0, FN_UPD_QUIT = 0x1D5320;
// the parts (Sly 2's 0x314 / 0x331..0x335 / 0x315)
constexpr int PART_BUSY = 0x37B;       // states 1 / 2 / 8 wait for it
constexpr int PART_SLIDER_IN = 0x3B7;  // state 11
constexpr int PART_SLIDER_OUT = 0x3B8; // state 9
constexpr int PART_MUSIC = 0x3B9, PART_EFFECTS = 0x3BA, PART_CENTER = 0x3BB, PART_FRACTION = 0x37C;
constexpr int PART_SLOT = 0x696, PART_SLOT_SEL = 0x699;   // + the row (0x1D19D0)
// the screen objects (0x1D2C20: PloFindSwNearest(sw, 0x405, oid))
constexpr int OID_PAGE = 1056, OID_PAGE2 = 1057;
// the text ids (Sly 3's world text)
constexpr int TX_KEEP_PLAYING = 0x48, TX_OPTIONS = 0x4A, TX_NEW = 0x4B, TX_LOAD = 0x4C, TX_SAVE = 0x4D, TX_CENTER = 0x4E,
              TX_BACK = 0x4F, TX_EXIT_2P = 0x51, TX_EXIT_MENU = 0x52, TX_CTRLHELP = 0x53, TX_QUIT = 0x56, TX_SAVEQUIT = 0x57,
              TX_SPEAKERS = 0x5C, TX_HEADSET = 0x60, TX_VIBRATION = 0x58, TX_MUSIC = 0x77, TX_EFFECTS = 0x78,
              TX_JOBHELP = 0x7A, TX_ABANDON = 0x7B, TX_OTHERJOBS = 0x7E, TX_CAMERA = 0x88, TX_RESET3D = 0x95,
              TX_RESET2D = 0x96, TX_TWOPLAYER = 0xAA;
// the data access pages' kinds (0x1D45B0: their title's text)
constexpr int KIND_SAVE = 0x25, KIND_NEW = 0x2A, KIND_LOAD = 0x2B;
// the card dialog's messages (card3.h; Sly 2's 0x1A / 0x1C / 0x20 / 0x21 / 0x22 / 0x2E)
constexpr int MSG_SAVING = 0x26, MSG_OVERWRITE = 0x28, MSG_ERASE = 0x2D, MSG_ERASING = 0x2E, MSG_AUTOSAVE = 0x2F,
              MSG_NOSAVE_LOAD = 0x3B, MSG_CREATE = 0x20;
// the game events the menu sends (GAME 0x36C700 0x1854C8: Sly 2's BroadcastTaskEvent 0x28.. + 0x12)
constexpr int EV_SHOWN = 0x3A, EV_CTRLHELP = 0x3B, EV_ABANDON = 0x3C, EV_JOBHELP = 0x3D, EV_HIDDEN = 0x3E, EV_QUIT1 = 0x3F,
              EV_QUIT2 = 0x40;
}  // namespace m3

// ------------------------------------------------------------ the entries (0x20 bytes in the executable)
// The PC options: native entries (ids above the texts') cycling display.h's / pcinput.h's options.
enum NATIVEID : int
{
    NID_Base = 0x10000,
    NID_Aspect = NID_Base, NID_Resolution, NID_Msaa, NID_FrameRate, NID_Window, NID_Vsync, NID_DrawDistance,
    NID_ExitToDesktop, NID_DisplayPage,
    // the controls page: pcinput.h / promptglyph.h / skipcut.h options
    NID_ControlsPage, NID_PromptStyle, NID_AlwaysPs, NID_InstantSkip, NID_MouseCamera, NID_MouseSens, NID_AimSens,
    NID_InvertX, NID_InvertY, NID_WheelZoom, NID_MouseMenus, NID_HackMouse, NID_KeyboardPage, NID_ResetKeys,
    NID_KeyFirst,   // .. + PCA_Max + 4: the keyboard mapping's rows (the actions, then the camera keys)
    // the display options' Long Shadows; Invert Controls...: its page, the controller's camera, the mouse in the
    // binocucom / turrets (NID_InvertX / NID_InvertY: the mouse's camera)
    NID_Pc2 = NID_Base + 0x100,
    NID_LongShadows = NID_Pc2, NID_InvertPage, NID_PadInvertX, NID_PadInvertY, NID_BinocInvertX, NID_BinocInvertY,
    NID_TurretInvertX, NID_TurretInvertY,
};
constexpr uint32_t PFN_NativeDisplay = 0xFFFF0001;   // (a native select function)
constexpr uint32_t PFN_NativeExit = 0xFFFF0002;
constexpr uint32_t PFN_NativeDisplayPage = 0xFFFF0003;   // (the PC display options' own page)
constexpr uint32_t PFN_NativeControls = 0xFFFF0004;      // (a controls option: cycled)
constexpr uint32_t PFN_NativeControlsPage = 0xFFFF0005;
constexpr uint32_t PFN_NativeKeyboardPage = 0xFFFF0006;
constexpr uint32_t PFN_NativeKey = 0xFFFF0007;           // (a key row: the next key pressed binds)
constexpr uint32_t PFN_NativeInvertPage = 0xFFFF0009;    // (Invert Controls...)
constexpr uint32_t VA_NativeInvertPage = 0xFFFF0109;     // (the invert page's identity: its rows show the controller's icons)

struct MENUENT
{
    int id = 0;              // +0x00 the text (cycled options: the value's text)
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

// ------------------------------------------------------------ the menu (orig3 0x46F550)
struct PAGE
{
    std::vector<MENUENT> aent;   // +0x00 c / +0x04
    uint32_t vaTab = 0;          // the entries' table (the data access pages' identity, 0x1D45B0)
    LO* plo = nullptr;           // +0x08
    int fReplaced = 0;           // +0x0C (a yes / no page: Triangle goes back through state 3)
    int isel = 0;                // +0x14
    uint32_t vaAnim = 0;         // +0x18 the animations (9 words)
};
struct MENU
{
    std::array<PAGE, 17> apage;  // (1..16)
    int depth = 0;               // +0x38
    int state = 0;               // +0x1FC
    int pending = -1;            // +0x200
    LO* ploDefault = nullptr;    // +0x30 (1056)
    LO* ploPage2 = nullptr;      // +0x34 (1057)
    bool fShown = false;         // in the HUD's list (0x46F540)
};
MENU s_menu;
SW* s_psw = nullptr;
int s_ikeyCapture = -1;   // (the keyboard mapping's row waiting for a key)
int s_islotTarget = -1;   // orig3 the save +0x13958 (the slot a save / an erase goes to)
int s_nAfter = 0;         // the card widget +0x60: after the save 1 the boot again, 2 the menu closes, 3 an auto-save
int s_kindCamera = 0;     // orig3 0x3999E8: the camera kind of the Camera Options… page (0x1D5178)

PAGE* PpageCur() { return s_menu.depth > 0 ? &s_menu.apage[size_t(s_menu.depth)] : nullptr; }
int IdAnim(const PAGE& pg, uint32_t ib) { return pg.vaAnim ? NElf(pg.vaAnim + ib) : -1; }

// the current world (0x47989C) and episode (0x4798A0, else the world's: 0x25E160); the prologue in m_ext is Intro (1)
int WidCur3() { return g_transition.iwrecCur; }
int IepisodeMenu3()
{
    const int wid = WidCur3();
    if (wid == sly3::WORLD_PROLOGUE) return sly3::EP_Intro;
    if (wid < 0 || wid >= sly3::CWORLD) return -1;
    return NElf(sly3::VA_WORLD_EPISODES + 4 * uint32_t(wid));
}

// ------------------------------------------------------------ the camera preferences (the Camera Options… page)
// flags joyLeft mpcamprefkfReverseHorizontal / Vertical [kind] (0x1D3DF0 cases 0x8F..0x92: the player +0x1148 / +0x1164)
CRef RefCamPref(int kind, bool fVertical)
{
    return RefGetProgress({"flags", "joyLeft", fVertical ? "mpcamprefkfReverseVertical" : "mpcamprefkfReverseHorizontal", std::to_string(kind)});
}
void SetCamPref(int kind, bool fVertical, bool f)
{
    CRef r;
    r.SetBool(f);
    FSetProgress({"flags", "joyLeft", fVertical ? "mpcamprefkfReverseVertical" : "mpcamprefkfReverseHorizontal", std::to_string(kind)}, r);
}

// ------------------------------------------------------------ the card dialog (card3.h)
void ShowCard(int msg, int nAfter = -1)
{
    if (nAfter >= 0) s_nAfter = nAfter;
    ShowCardMessage3(msg);
}

// ------------------------------------------------------------ the menu's states
void SetMenuState(int st);

// orig3 0x001D2D90 / 0x001D2DF8: in the HUD's list with the game paused, out
void ShowMenuWidget()
{
    if (s_menu.fShown) return;
    s_menu.fShown = true;
    SetClockFactor(&g_clock, 0, 0.0f);   // (not in the two-player world)
    if (s_psw) BroadcastTaskEvent(s_psw, m3::EV_SHOWN, nullptr);
}
void HideMenuWidget()
{
    if (!s_menu.fShown) return;
    s_menu.fShown = false;
    s_menu.depth = 0;
    SetClockFactor(&g_clock, 0, 1.0f);
    if (s_psw) BroadcastTaskEvent(s_psw, m3::EV_HIDDEN, nullptr);
}

// orig3 0x001D2E70: the pause page's episode part (0x345F00[episode])
int IdEpisodePart()
{
    const int iep = IepisodeMenu3();
    return iep >= 0 && iep < 9 ? NElf(m3::VA_EPISODE_IDLE + 4 * uint32_t(iep)) : -1;
}

// orig3 0x001D2ED0
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
    default: break;
    }
    s_menu.state = st;
    if (st == 0) { HideMenuWidget(); return; }
    if (st >= 12 || !pe || !pe->plo) return;
    LO* plo = pe->plo;
    switch (st)
    {
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
        // (+0x20) the episode's backdrop on the page, unless it plays already
        if (IdAnim(*pe, 0x20) != 0)
            if (const int id = IdEpisodePart(); id >= 0 && !FScreenObjBusy(plo, id)) SelectScreenObjPart(plo, id);
        break;
    case 9: SelectScreenObjPart(plo, m3::PART_SLIDER_OUT); break;
    case 10:
    {
        const MENUENT& e = pe->aent[size_t(pe->isel)];
        switch (e.id)
        {
        case m3::TX_MUSIC:
            SelectScreenObjPart(plo, m3::PART_MUSIC);
            SelectScreenObjPart(plo, m3::PART_FRACTION);
            SetScreenObjFraction(plo, m3::PART_FRACTION, 1.0f - GRef(RefSetting("uMusicVolume")));
            break;
        case m3::TX_EFFECTS:
            SelectScreenObjPart(plo, m3::PART_EFFECTS);
            SelectScreenObjPart(plo, m3::PART_FRACTION);
            SetScreenObjFraction(plo, m3::PART_FRACTION, 1.0f - GRef(RefSetting("uEffectsVolume")));
            break;
        case m3::TX_CENTER: SelectScreenObjPart(plo, m3::PART_CENTER); break;
        default: break;
        }
        break;
    }
    case 11: SelectScreenObjPart(plo, m3::PART_SLIDER_IN); break;
    default: break;
    }
}

// orig3 0x001D3AE8
void CloseMenu()
{
    if (s_menu.state != 7) return;
    if (s_menu.depth >= 2) s_menu.depth = 1;
    SetMenuState(8);
}

// orig3 0x001D45B0 / 0x001D4658: the slots' page (0x2A new game, 0x2B load, 0x25 save)
int KindDataAccess()
{
    if (!s_menu.fShown) return -1;
    const PAGE* pe = PpageCur();
    if (!pe) return -1;
    if (pe->vaTab == m3::VA_NEW) return m3::KIND_NEW;
    if (pe->vaTab == m3::VA_LOAD) return m3::KIND_LOAD;
    if (pe->vaTab == m3::VA_SAVE) return m3::KIND_SAVE;
    return -1;
}
bool FDataAccess() { return KindDataAccess() != -1; }

void InitEntries(PAGE& pg);
void MenuLog(const char* sz, const PAGE& pg)
{
    if (!std::getenv("SLY2N_MENULOG") && !std::getenv("SLY3_MENULOG")) return;
    std::printf("menu: page 0x%X%s (selected %d, depth %d):", pg.vaTab, sz, pg.isel, s_menu.depth);
    for (const MENUENT& e : pg.aent) std::printf(" 0x%X", e.id);
    std::printf("\n");
}

// orig3 0x001D3960 (push a page; a null object: the current page's)
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
    MenuLog("", pg);
    if (FDataAccess())
    {
        s_menu.pending = s_menu.depth;
        --s_menu.depth;
    }
}

// orig3 0x001D3818 (a page in place of the current one's, with its object and animations)
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
    MenuLog(" (replaced)", pg);
}

// orig3 0x001D3918 / 0x001D3A90 / 0x001D3B28
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
// ------------------------------------------------------------ the PC options (Sly 2's port's, unchanged)
// (the port's own options: their text in the game's language, porttext.h; English on the USA disc)
std::string StrNative(int id)
{
    char ach[96];
    auto onOff = [](PTX ptx, bool f) { return StrPortOnOff(ptx, f); };
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
    case NID_ExitToDesktop: return StrPort(PTX_ExitToDesktop);
    case NID_DisplayPage: return StrPort(PTX_DisplayOptions);
    case NID_ControlsPage: return StrPort(PTX_ControlsAndPrompts);
    case NID_PromptStyle:
    {
        // (the controllers' names stay as they are)
        static const char* const asz[] = {nullptr, "PlayStation", "Xbox", "Nintendo", "Steam Deck", "Steam Controller", "PlayStation 4", "PlayStation 5", "Steam Controller (2026)"};
        const int i = std::clamp(g_pcinput.promptStyle, 0, cane::input::kPromptStyleLast);
        return StrPortValue(PTX_ButtonIcons, i == 0 ? StrPort(PTX_Auto) : std::string(asz[i]));
    }
    case NID_AlwaysPs: return onOff(PTX_OriginalPsIcons, g_pcinput.fAlwaysOriginalPs);
    case NID_InstantSkip: return onOff(PTX_InstantSkip, g_pcinput.fInstantSkipCutscenes);
    case NID_MouseCamera: return onOff(PTX_MouseCamera, g_pcinput.fMouseCamera);
    case NID_MouseSens: std::snprintf(ach, sizeof(ach), "%d%%", g_pcinput.mouseSensitivity); return StrPortValue(PTX_MouseSensitivity, ach);
    case NID_AimSens: std::snprintf(ach, sizeof(ach), "%d%%", g_pcinput.mouseAimSensitivity); return StrPortValue(PTX_MouseAimSensitivity, ach);
    case NID_InvertPage: return StrPort(PTX_InvertControlsPage);
    // (the controller's rows: the right stick's icon, the device's, promptglyph.h)
    case NID_PadInvertX: return "&2)&. " + onOff(PTX_PadInvertCameraX, g_pcinput.fPadInvertCameraX);
    case NID_PadInvertY: return "&2)&. " + onOff(PTX_PadInvertCameraY, g_pcinput.fPadInvertCameraY);
    case NID_InvertX: return onOff(PTX_MouseInvertCameraX, g_pcinput.fMouseInvertX);
    case NID_InvertY: return onOff(PTX_MouseInvertCameraY, g_pcinput.fMouseInvertY);
    case NID_BinocInvertX: return onOff(PTX_MouseInvertBinocX, g_pcinput.fMouseInvertBinocX);
    case NID_BinocInvertY: return onOff(PTX_MouseInvertBinocY, g_pcinput.fMouseInvertBinocY);
    case NID_TurretInvertX: return onOff(PTX_MouseInvertTurretX, g_pcinput.fMouseInvertTurretX);
    case NID_TurretInvertY: return onOff(PTX_MouseInvertTurretY, g_pcinput.fMouseInvertTurretY);
    case NID_WheelZoom: return onOff(PTX_WheelZoomsBinocucom, g_pcinput.fMouseWheelZoom);
    case NID_MouseMenus: return onOff(PTX_MouseInMenus, g_pcinput.fMouseMenus);
    case NID_HackMouse: return onOff(PTX_MouseAimInHacking, g_pcinput.fHackMouseAim);
    case NID_KeyboardPage: return StrPort(PTX_KeyboardMapping);
    case NID_ResetKeys: return StrPort(PTX_ResetKeyboardMapping);
    default:
        if (id >= NID_KeyFirst && id < NID_KeyFirst + PCA_Max + 4)
        {
            static const PTX aptx[] = {PTX_MoveForward, PTX_MoveBack, PTX_MoveLeft, PTX_MoveRight, PTX_Square, PTX_Circle,
                                       PTX_Cross, PTX_Triangle, PTX_Start, PTX_Select, PTX_Max, PTX_Max, PTX_Max, PTX_Max,
                                       PTX_Max, PTX_Max, PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown};
            static const char* const aszButton[] = {"L1", "R1", "L2", "R2", "L3", "R3"};   // (PTX_Max: the button's own name)
            const int i = id - NID_KeyFirst;
            const int key = i < PCA_Max ? g_pcinput.keyboard[size_t(i)] : g_pcinput.cameraKeys[size_t(i - PCA_Max)];
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

// orig3 0x001D3DF0 (an option entry shows the setting's value)
void InitOptionText(MENUENT& e)
{
    switch (e.id)
    {
    case 0x5C: case 0x5D: case 0x5E:   // Speakers (nMixerMode 1 mono, 2 stereo, else surround)
        switch (NRef(RefSetting("nMixerMode")))
        {
        case 1: e.id = 0x5C; break;
        case 2: e.id = 0x5D; break;
        default: e.id = 0x5E; break;
        }
        break;
    case 0x5F: case 0x60: e.id = NRef(RefSetting("fUseHeadset")) ? 0x5F : 0x60; break;   // (orig3: 0x475000, a headset found)
    case 0x58: case 0x59: e.id = FVibrationEnabled() ? 0x58 : 0x59; break;                 // (0x210DE0 on the player's pad)
    case 0x8F: case 0x90: e.id = NRef(RefCamPref(s_kindCamera, false)) ? 0x90 : 0x8F; break;
    case 0x91: case 0x92: e.id = NRef(RefCamPref(s_kindCamera, true)) ? 0x92 : 0x91; break;
    default: break;
    }
}

// orig3 0x001D3FC0 (the option's value into the settings)
void ApplyOption(const MENUENT& e)
{
    switch (e.id)
    {
    case m3::TX_MUSIC: case m3::TX_EFFECTS: case m3::TX_CENTER: SetMenuState(9); break;   // (the slider accepted)
    case 0x5E: SetSettingN("nMixerMode", 0); break;
    case 0x5D: SetSettingN("nMixerMode", 2); break;
    case 0x5C: SetSettingN("nMixerMode", 1); break;
    case 0x5F: case 0x60: SetSettingB("fUseHeadset", e.id == 0x5F); break;
    case 0x58: case 0x59: SetVibrationOption(e.id == 0x59); break;
    case 0x8F: case 0x90: SetCamPref(s_kindCamera, false, e.id == 0x90); break;
    case 0x91: case 0x92: SetCamPref(s_kindCamera, true, e.id == 0x92); break;
    default: break;
    }
    ApplyGameSettings(g_gamesave);   // orig3 0x21D360(the record): the settings take effect
}

// orig3 0x001D4510 (the next of the option's values)
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

// orig3 0x001D3B58 (state 10: the slider follows left / right)
void UpdateSlider(PAGE& pg, MENUENT& e)
{
    const int n = NStepLeftRight();
    if (n == 0) return;
    if (e.id != m3::TX_MUSIC && e.id != m3::TX_EFFECTS) return;
    // the channel's volume (music channel 1, effects 0 and 3); the record keeps its complement
    const char* name = e.id == m3::TX_MUSIC ? "uMusicVolume" : "uEffectsVolume";
    const float g = std::clamp(1.0f - GRef(RefSetting(name)) + float(n) * 0.1f, 0.0f, 1.0f);
    if (e.id == m3::TX_MUSIC) SetSoundChannelVolume(1, 1, g);
    else
    {
        SetSoundChannelVolume(0, 1, g);
        SetSoundChannelVolume(3, 1, g);
    }
    SetSettingF(name, 1.0f - g);
    SetScreenObjFraction(pg.plo, m3::PART_FRACTION, g);
}

// orig3 0x001D4498 (the slider left with Triangle / START: back to the value it had)
void CancelSlider(PAGE& pg, const MENUENT& e)
{
    (void)pg;
    (void)e;
    SetMenuState(9);
}

// ------------------------------------------------------------ the slots' pages (0x1D1470 / 0x1D1568 / 0x1D1708)
float DtSlot(int i) { return GRef(RefProgressOfSlot(i, {"dt"})); }

// QoL (Q4, as Sly 2's port): the data access pages show kSlotsPerPage slots of kSlotPages pages (frontend.h); L1 / R1
// cycle the pages. Page 1 is the original's three slots, and it opens there exactly as the original's.
int s_ipageSlot = 0;
int IslotOfRow(int irow) { return s_ipageSlot * kSlotsPerPage + irow; }

std::string StrSlotPage()
{
    char ach[64];
    std::snprintf(ach, sizeof(ach), StrPort(PTX_SlotPage).c_str(), s_ipageSlot + 1, kSlotPages);
    return std::string("&2L&. ") + ach + " &2R&.";
}

// the save into the target slot (0x1D1648: the warning first, else "Saving..." and the write)
void SaveToTarget()
{
    g_islotChosen = s_islotTarget;
    if (!NRef(RefSetting("fAutosaveWarned"))) ShowCard(m3::MSG_AUTOSAVE, 2);
    else
    {
        ShowCard(m3::MSG_SAVING, 2);
        FWriteSaveSlot(g_islotChosen);
    }
}

void SelectEntry(MENUENT& e);

// a slot's record cleared, keeping its settings ("flags")
void ResetSlotKeepSettings(int islot)
{
    ApplyVibrationSetting();
    SAVESLOT& sl = g_aslot[islot];
    static const char* const aszFlag[] = {"lang", "nMixerMode", "uMusicVolume", "uEffectsVolume", "fUseHeadset", "dxScreenCenter",
                                          "dyScreenCenter", "fAutosaveWarned"};
    GAMESAVE saved = g_gamesave;
    g_gamesave = sl.fFile ? sl.save : GAMESAVE{};
    ValidateGameSave();
    std::vector<std::pair<std::vector<std::string>, CRef>> akept;
    for (const char* sz : aszFlag) akept.push_back({{"flags", sz}, RefGetProgress({"flags", sz})});
    for (const char* szPad : {"joyLeft", "joyRight"})
    {
        akept.push_back({{"flags", szPad, "fNoVibration"}, RefGetProgress({"flags", szPad, "fNoVibration"})});
        for (const char* szPref : {"mpcamprefkfReverseHorizontal", "mpcamprefkfReverseVertical"})
            for (int k = 0; k < 7; ++k)
            {
                std::vector<std::string> apath = {"flags", szPad, szPref, std::to_string(k)};
                akept.push_back({apath, RefGetProgress(apath)});
            }
    }
    std::memset(g_gamesave.ab, 0, CB_PROGRESS);
    for (const auto& [apath, r] : akept)
        if (r.m_tagk != TAGK_Nil) FSetProgress(apath, r);
    sl.save = g_gamesave;
    g_gamesave = saved;
}

// ------------------------------------------------------------ Control Help (0x1D5058)
// orig3: the players' vt slot 91 (Sly 0x1F5020, Bentley 0x1F5178, Murray 0x1F5268): the text of Control Help's line k
// (0 the title, 1..12 the buttons: the parts 0x345F58[k], script strings 917 + k), in the one-player game and on foot
// (in a vehicle, actor +0x1460, the steering lines; the two-player game's own lines: not natively)
int IdControlHelpText(LO* player, int k)
{
    static const int s_aidSly[13] = {0x257, 0x258, 0x200, 0x252, 0x253, 0x206, 0x206, 0x254, 0x205, 0x206, 0x202, 0x201, 0x204};
    static const int s_aidBentley[13] = {0x214, 0x215, 0x200, 0x20F, 0x210, 0x206, 0x206, 0x211, 0x213, 0x206, 0x212, 0x201, 0x204};
    static const int s_aidMurray[13] = {0x232, 0x233, 0x200, 0x231, 0x22F, 0x206, 0x206, 0x230, 0x205, 0x206, 0x202, 0x201, 0x204};
    if (!player || k < 0 || k > 12) return -1;
    const CID cid = player->Cid();
    if (FCidDerivedFrom(cid, sly3::CID_JT)) return s_aidSly[k];
    if (FCidDerivedFrom(cid, sly3::CID_BENTLEY)) return s_aidBentley[k];
    if (FCidDerivedFrom(cid, sly3::CID_MURRAY)) return s_aidMurray[k];
    return -1;
}
LO* PloMenuPlayer() { return s_psw ? PloPoCurrent(s_psw) : nullptr; }   // (orig3 the menu +0x18: the pad's player)

// ------------------------------------------------------------ the pages' tables
// orig3 0x001D48D0: the pause menu's entries (0x345DA0, 11) for the current game (a0: the job, -1 none)
std::vector<MENUENT> AentPause()
{
    std::vector<MENUENT> a;
    const int iep = IepisodeMenu3();
    const int wid = WidCur3();
    const bool fJob = s_psw && s_psw->tasks.pjobCur;   // (0x4794C4)
    for (const MENUENT& e : AentFromElf(m3::VA_PAUSE, 11))
    {
        bool f = true;
        switch (e.id)
        {
        // (0x4BC244 +0x50, the two-player game's mode: none natively, -1)
        case m3::TX_EXIT_2P: f = false; break;                                         // two player only
        case m3::TX_EXIT_MENU: f = iep != sly3::EP_Intro; break;                       // not in the prologue
        case m3::TX_ABANDON: f = fJob && iep != sly3::EP_Intro && iep != sly3::EP_Isle && wid != 24 && wid != 25; break;
        case m3::TX_OTHERJOBS: f = false; break;                                       // (the original never offers it)
        // TODO(sly3): Job Help (the job help widget 0x46FAA0's object, not in the dvd menu) and the 3-D resets
        // (0x4794CC, 0x18AE00, GAME +0x14EC bit 1): their pages are not ported yet, so they are not offered
        case m3::TX_JOBHELP: f = false; break;
        case m3::TX_CTRLHELP: f = IdControlHelpText(PloMenuPlayer(), 12) != -1; break;   // (the player's slot 91(12))
        case m3::TX_RESET3D: case m3::TX_RESET2D: f = false; break;
        default: break;
        }
        if (f) a.push_back(e);
    }
    // (PC) Exit to Desktop, as Sly 2's port's pause menu
    MENUENT ex;
    ex.id = NID_ExitToDesktop;
    ex.pfnSelect = PFN_NativeExit;
    a.push_back(ex);
    return a;
}

// (PC) the display options' page
std::vector<MENUENT> AentDisplayOptions()
{
    std::vector<MENUENT> a;
    for (int id = NID_Aspect; id <= NID_DrawDistance + 1; ++id)
    {
#ifdef __SWITCH__
        if (id == NID_Window) continue;   // (the Switch: one screen, no window to place)
#endif
        MENUENT e;
        e.id = id <= NID_DrawDistance ? id : NID_LongShadows;
        e.pfnSelect = PFN_NativeDisplay;
        a.push_back(e);
    }
    return a;
}

// orig3 0x001D4C48: the options page (0x345B40, 11), then the PC's Display Options... / Controls...
std::vector<MENUENT> AentOptions()
{
    std::vector<MENUENT> a;
    const bool fTitle = FTitleShown();
    for (const MENUENT& e : AentFromElf(m3::VA_OPTIONS, 11))
    {
        bool f = true;
        if (e.id == m3::TX_NEW) f = fTitle;          // New Game... only at the title
        else if (e.id == m3::TX_SAVE) f = !fTitle;   // Save Game... only in a game (0x1CFDF8: the card usable)
        else if (e.id == m3::TX_TWOPLAYER) f = false;   // (0xAA at the title: the two-player game is not ported)
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

// (PC) the controls page and its keyboard mapping
std::vector<MENUENT> AentControls()
{
    std::vector<MENUENT> a;
    for (int id = NID_PromptStyle; id <= NID_KeyboardPage; ++id)
    {
        if (id == NID_InvertY || id == NID_HackMouse) continue;   // (no hacking minigame mouse aim in Sly 3 yet)
        MENUENT e;
        e.id = id == NID_InvertX ? NID_InvertPage : id;
        e.pfnSelect = id == NID_KeyboardPage ? PFN_NativeKeyboardPage : id == NID_InvertX ? PFN_NativeInvertPage : PFN_NativeControls;
        a.push_back(e);
    }
    return a;
}
// (PC) Invert Controls...: the controller's camera, then the mouse in each view
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
    for (int id : {int(NID_InvertX), int(NID_InvertY), int(NID_BinocInvertX), int(NID_BinocInvertY), int(NID_TurretInvertX),
                   int(NID_TurretInvertY)})
        add(id);
    return a;
}
std::vector<MENUENT> AentKeyboard()
{
    std::vector<MENUENT> a;
    for (int i = 0; i < PCA_Max + 4; ++i)
    {
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
        case m3::FN_INIT_OPTION: InitOptionText(e); break;
        case m3::FN_INIT_CAMKIND:
            // orig3 0x001D5178: the current camera's kind (0x1A9260 on the player: 0 the camera, 1 the binocucom, 2 the
            // grapple-cam, 3 a turret, 4 strafing, 5 swimming, 6 the harpoon) names the entry (player3.cpp)
            s_kindCamera = sly3::NPlayerCamKind3(g_psw);
            e.id = NElf(m3::VA_CAMKINDS + 4 * uint32_t(s_kindCamera));
            break;
        case m3::FN_INIT_SLOTS:
        {
            // orig3 0x001D17F0: the selection on the first fitting slot (of page 1, the original's three slots)
            float ag[3];
            for (int i = 0; i < 3; ++i) ag[i] = DtSlot(i);
            const int kind = pg.vaTab == m3::VA_NEW ? m3::KIND_NEW : pg.vaTab == m3::VA_LOAD ? m3::KIND_LOAD : pg.vaTab == m3::VA_SAVE ? m3::KIND_SAVE : -1;
            s_ipageSlot = 0;
            if (kind == m3::KIND_NEW) { for (int j = 0; j < 3; ++j) if (ag[j] == 0.0f) { pg.isel = j; break; } }
            else if (kind == m3::KIND_LOAD)
            {
                int islot = -1;
                for (int j = 0; j < 3; ++j) if (ag[j] != 0.0f) { islot = j; break; }
                for (int j = 3; islot < 0 && j < kSlotMax; ++j) if (DtSlot(j) != 0.0f) islot = j;   // (QoL Q4)
                if (islot >= 0) { s_ipageSlot = islot / kSlotsPerPage; pg.isel = islot % kSlotsPerPage; }
            }
            else if (kind == m3::KIND_SAVE && g_islotChosen >= 0)
            {
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
    if (std::getenv("SLY2N_MENULOG") || std::getenv("SLY3_MENULOG")) std::printf("menu: select 0x%X (fn 0x%X, n %d)\n", e.id, e.pfnSelect, e.n);
    switch (e.pfnSelect)
    {
    case m3::FN_KEEP_PLAYING: CloseMenu(); break;
    case m3::FN_PUSH:                                                    // a page of entries (0x1D4FA0)
        PushPage(AentFromElf(e.vaTab, e.n), e.vaTab, nullptr, e.vaAnim);
        e.grf |= 2;
        break;
    case m3::FN_YESNO:                                                   // a yes / no page (0x1D4FF0)
        ReplacePage(AentFromElf(e.vaTab, e.n), e.vaTab);
        e.grf |= 2;
        break;
    case m3::FN_NO: BackReplaced(); break;
    case m3::FN_OPTIONS:
        ReplacePage(AentOptions(), m3::VA_OPTIONS);
        e.grf |= 2;
        break;
    case m3::FN_CTRLHELP:                                                // Control Help (0x1D5058): the page of the player's buttons
        if (s_psw) BroadcastTaskEvent(s_psw, m3::EV_CTRLHELP, nullptr);
        PushPage({}, 0, nullptr, m3::VA_ANIM_CTRLHELP);   // (no entries: any button goes back; the lines are 917..929)
        e.grf |= 2;
        break;
    case m3::FN_CAMOPT:                                                  // Camera Options… (0x1D51D8)
        ReplacePage(AentFromElf(m3::VA_CAMOPT, 2), m3::VA_CAMOPT);
        e.grf |= 2;
        break;
    case m3::FN_YES_QUIT:                                                // orig3 0x001D5398
        if (s_psw)
        {
            BroadcastTaskEvent(s_psw, m3::EV_QUIT1, nullptr);
            BroadcastTaskEvent(s_psw, m3::EV_QUIT2, nullptr);
        }
        if (g_islotChosen >= 0)
        {
            if (!NRef(RefSetting("fAutosaveWarned"))) ShowCard(m3::MSG_AUTOSAVE, 1);
            else
            {
                ShowCard(m3::MSG_SAVING, 1);
                FWriteSaveSlot(g_islotChosen);
            }
        }
        else StartBootSequence(false);   // orig3 0x0018AD90
        e.grf |= 2;
        break;
    case m3::FN_YES_ABANDON:                                             // orig3 0x001D5540
        if (s_psw)
        {
            BroadcastTaskEvent(s_psw, m3::EV_ABANDON, nullptr);
            if (s_psw->tasks.pjobCur) EndJob(s_psw, s_psw->tasks.pjobCur);
        }
        break;
    case m3::FN_YES_EXIT:                                                // orig3 0x001D5598: the game menu (world 0, dvd_menu)
        SetEpMenuReturn3(IepisodeMenu3());   // (back on this episode's page: epmenu3.h)
        RequestTransition(s_psw, g_disc.PbElf(sly3::VA_WORLD_KEYS, 64), sly3::WORLD_BOOT_ARRIVAL, -1, 0, -1);
        if (s_psw && s_psw->tasks.pjobCur) EndJob(s_psw, s_psw->tasks.pjobCur);
        e.grf |= 2;
        break;
    case m3::FN_CYCLE: CycleOption(e); break;
    case m3::FN_SLIDER: StartSlider(); break;
    case m3::FN_SLOT_NEW:                                                // New Game in slot n
    {
        const int islot = IslotOfRow(e.n);
        if (DtSlot(islot) == 0.0f)
        {
            g_islotChosen = islot;
            ResetSlotKeepSettings(islot);
            StartGameFromSave(s_psw, &g_aslot[islot].save);
        }
        else e.grf |= 2;   // (the error sound)
        break;
    }
    case m3::FN_SLOT_LOAD:                                               // Load Game from slot n
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
    case m3::FN_SLOT_SAVE:                                               // Save Game to slot n
        s_islotTarget = IslotOfRow(e.n);
        if (DtSlot(s_islotTarget) == 0.0f) SaveToTarget();
        else ShowCard(m3::MSG_OVERWRITE);
        e.grf |= 2;
        break;
    case PFN_NativeDisplay: CycleNative(e); e.grf |= 2; break;
    case PFN_NativeControls: CycleControls(e); e.grf |= 2; break;
    // (PC menus) the native pages swap in place of the current one, as Options... does
    case PFN_NativeControlsPage: ReplacePage(AentControls(), 0); e.grf |= 2; break;
    case PFN_NativeKeyboardPage: ReplacePage(AentKeyboard(), 0); e.grf |= 2; break;
    case PFN_NativeInvertPage: ReplacePage(AentInvert(), VA_NativeInvertPage); e.grf |= 2; break;
    case PFN_NativeDisplayPage: ReplacePage(AentDisplayOptions(), 0); e.grf |= 2; break;
    case PFN_NativeKey:
        if (g_gl.window)
        {
            s_ikeyCapture = e.id - NID_KeyFirst;
            KeyBindingPressed(g_gl.window);   // (primed: the key that chose the row is not taken)
        }
        e.grf |= 2;
        break;
    case PFN_NativeExit:
        if (g_gl.window) glfwSetWindowShouldClose(g_gl.window, 1);
        break;
    default:
        std::printf("menu: entry 0x%X select 0x%X not ported\n", e.id, e.pfnSelect);
        e.grf |= 2;
        break;
    }
}

// orig3 0x001D19D0 (a slot entry: its look selected or not)
void UpdateSlotEntry(PAGE& pg, const MENUENT& e)
{
    if (!FDataAccess()) return;
    SelectScreenObjPart(pg.plo, pg.isel == e.n ? e.n + m3::PART_SLOT_SEL : e.n + m3::PART_SLOT);
}

// the slot's file erased (orig3 0x1CEEC0 with the erase thread)
void EraseTarget()
{
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
        // (the slot rewritten as a fresh record: the folder stays, as on the card)
        SAVESLOT& sl = g_aslot[s_islotTarget];
        std::string err;
        const GAMESAVE saved = g_gamesave;
        g_gamesave = GAMESAVE{};
        ValidateGameSave();
        sl.save = g_gamesave;
        g_gamesave = saved;
        if (FWriteGameSaveBlock(sl.path, sl.save, err))
        {
            std::error_code ec;
            sl.fFile = true;
            sl.tWrite = std::filesystem::last_write_time(cane::path(sl.path), ec).time_since_epoch().count();
        }
    }
}
}  // namespace

// ------------------------------------------------------------ the card dialog's menu messages (card3.h)
// orig3 0x001D1370 / 0x001D1190 for the menu's messages: true when handled
bool FMenuCardAnswer3(int msg, bool fYes)
{
    switch (msg)
    {
    case m3::MSG_OVERWRITE:
        if (fYes) SaveToTarget();
        else CloseCardMessage3();
        return true;
    case m3::MSG_ERASE:
        if (fYes)
        {
            EraseTarget();
            ShowCardMessage3(m3::MSG_ERASING);
        }
        else CloseCardMessage3();
        return true;
    case m3::MSG_AUTOSAVE:   // (OK) warned once; then the save it was shown for
        SetSettingB("fAutosaveWarned", true);
        if (s_nAfter > 0 && s_nAfter < 4)
        {
            ShowCardMessage3(m3::MSG_SAVING);
            if (g_islotChosen >= 0) FWriteSaveSlot(g_islotChosen);
        }
        else CloseCardMessage3();
        return true;
    case m3::MSG_NOSAVE_LOAD:
        CloseCardMessage3();
        if (FDataAccess()) PopPage();
        return true;
    default: return false;
    }
}

// the messages that end by themselves (the writes are synchronous here): "Saving..." and "Erasing..."
void UpdateMenuCard3(int msg, float dtShown)
{
    if (dtShown < 0.5f) return;
    if (msg == m3::MSG_SAVING)
    {
        CloseCardMessage3();
        if (s_nAfter == 1) StartBootSequence(false);
        else if (s_nAfter == 2) CloseMenu();
        s_nAfter = 0;
    }
    else if (msg == m3::MSG_ERASING)
    {
        s_islotTarget = -1;
        CloseCardMessage3();
    }
}

// ------------------------------------------------------------ public
void ShowCardMessage(int idMsg, int nAfter) { ShowCard(idMsg, nAfter); }

// orig3 0x001D2C20 (after a world load) and 0x001D2D10 (the old world's menu closed)
void InitMenu(SW* psw)
{
    if (s_menu.fShown) SetClockFactor(&g_clock, 0, 1.0f);
    s_psw = psw;
    s_menu = MENU{};
    s_menu.ploDefault = psw ? PloScreen3(psw, m3::OID_PAGE) : nullptr;
    s_menu.ploPage2 = psw ? PloScreen3(psw, m3::OID_PAGE2) : nullptr;
}

int g_ncardMode = 0;

bool FMenuShown() { return s_menu.fShown || FCardMessageShown3(); }
bool FMenuActive() { return s_menu.fShown; }
bool FCardMessageActive() { return FCardMessageShown3(); }

// orig3 0x001D48D0 (from the HUD's update 0x21D9E0: START)
bool FOpenPauseMenu(SW* psw)
{
    s_psw = psw;
    if (!s_menu.ploDefault || g_transition.fPending || FCardMessageShown3() || s_menu.fShown) return false;
    ShowMenuWidget();
    PushPage(AentPause(), m3::VA_PAUSE, s_menu.ploDefault, m3::VA_ANIM);
    return true;
}

// orig3 0x001D4BF8 (the title's SELECT: the options page)
void OpenDataAccessMenu(SW* psw)
{
    s_psw = psw;
    if (!s_menu.ploDefault || s_menu.fShown) return;
    ShowMenuWidget();
    PushPage(AentOptions(), m3::VA_OPTIONS, s_menu.ploDefault, m3::VA_ANIM);
    if (s_menu.depth <= 0) HideMenuWidget();
}

void HudAutoSaveFrontEnd() { HudAutoSave(true); }

// (the European build's language menu: the Sly 3 USA disc has one language)
void OpenLanguageMenu(SW* psw) { (void)psw; }

bool FCardSettled() { return FCardSettled3(); }

// orig3 0x001D3288
void UpdateMenu(SW* psw)
{
    s_psw = psw;
    if (FCardMessageShown3()) UpdateMenuCard3(IdCardMessage3(), DtCardMessageShown3());
    if (!s_menu.fShown || FCardMessageShown3()) { s_ikeyCapture = -1; return; }
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
        if (e.pfnUpdate == m3::FN_UPD_SLOT) UpdateSlotEntry(*pe, e);
        else if (e.pfnUpdate == m3::FN_UPD_QUIT)
        {
            // orig3 0x001D5320: Save and Quit with a slot (and the card usable), else Quit Game
            const bool fSlot = g_islotChosen >= 0;
            e.id = fSlot ? m3::TX_SAVEQUIT : m3::TX_QUIT;
            e.vaTab = fSlot ? m3::VA_SAVEQUIT : m3::VA_QUIT;
            e.n = 2;
        }
    }
    LO* plo = pe->plo;
    auto playing = [&](int id) { return plo && id >= 0 && FScreenObjBusy(plo, id); };
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
        if (playing(m3::PART_BUSY)) return;
        if (s_menu.pending != -1)
        {
            s_menu.depth = s_menu.pending;
            s_menu.pending = -1;
        }
        SetMenuState(7);
        return;
    case 2:
        if (playing(m3::PART_BUSY)) return;
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
        // (PC) a page shown again shows its options' current values
        if (PAGE* pgNext = PpageCur())
            for (MENUENT& e : pgNext->aent)
                if (e.pfnInit == m3::FN_INIT_OPTION) InitOptionText(e);
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
        const uint32_t grf = GrfbtnTakePressed(0x800 | BtnMenuBack() | BtnMenuConfirm() | BtnMenuErase() | (FDataAccess() ? 0xC : 0));
        if (grf & 0x800) { CloseMenu(); return; }
        if (grf & BtnMenuBack()) { confirm(); return; }
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
            // Circle: the erase of a slot (on the save / new game pages)
            const int r = KindDataAccess();
            if (r != m3::KIND_SAVE && r != m3::KIND_NEW) return;
            if (DtSlot(IslotOfRow(pe->isel)) == 0.0f) return;
            s_islotTarget = IslotOfRow(pe->isel);
            ShowCard(m3::MSG_ERASE);
            return;
        }
        if (FDataAccess() && (grf & (0x4 | 0x8)))
        {
            // (QoL Q4) L1 / R1: the previous / next page of slots (cycling), the selection kept on its row
            s_ipageSlot = (s_ipageSlot + ((grf & 0x8) ? 1 : kSlotPages - 1)) % kSlotPages;
            return;
        }
        if (!pe->aent.empty()) step(NStepUpDown());
        return;
    }
    case 8:
        if (playing(m3::PART_BUSY)) return;
        if (plo) HideScreenObj(plo);
        SetMenuState(0);
        return;
    case 9:
        if (playing(m3::PART_SLIDER_OUT)) return;
        if (plo) ResetScreenObjPart(plo, m3::PART_SLIDER_OUT);   // (0x262280)
        SetMenuState(7);
        return;
    case 10:
    {
        MENUENT& e = pe->aent[size_t(pe->isel)];
        const uint32_t grf = GrfbtnTakePressed(0x800 | BtnMenuBack() | BtnMenuConfirm());
        if (grf & 0x800) { CancelSlider(*pe, e); CloseMenu(); }
        else if (grf & BtnMenuBack()) CancelSlider(*pe, e);
        else if (grf & BtnMenuConfirm()) ApplyOption(e);
        else UpdateSlider(*pe, e);
        return;
    }
    case 11:
        if (playing(m3::PART_SLIDER_IN)) return;
        SetMenuState(10);
        return;
    default: return;
    }
}

// (Sly 2's ids: none in Sly 3; its strings are StrMenuScript3's)
std::string StrMenuScript(int id)
{
    (void)id;
    return "";
}

// The menu's script strings (screen3.cpp): 815..824 the wheel (0x25F440), 917..929 Control Help (0x1F58E8),
// 1692..1698 the data access pages (0x1D1B98 / 0x1D1BF0 / 0x1D1CC8 / 0x1D1E60 / 0x1D1EF8)
bool FStrMenuScript3(int id, std::string* pstr)
{
    char ach[128];
    auto text = [&](int idText) { return StrText3(s_psw, idText); };
    auto strEntry = [&](int idText) -> std::string {
        if (idText >= NID_Base) return StrNative(idText);
        return text(idText);
    };
    // the episode's name (0x222410) and the game's percent (0x25E6D0) of a record
    auto strStatus = [&](const GAMESAVE& gs, int iep) {
        static const int s_aid[9] = {0xAA, 0xA3, 0xAB, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9};
        std::snprintf(ach, sizeof(ach), "%d%%", NGamePercent(gs));
        return (iep >= 0 && iep < 9 ? text(s_aid[iep]) : std::string()) + " : " + ach;
    };
    if (id >= 815 && id <= 824)
    {
        // orig3 0x0025F440: the wheel's entry k around the selection (3: the selected one)
        const PAGE* pe = PpageCur();
        if (!pe || pe->aent.empty()) { *pstr = ""; return true; }
        const int c = int(pe->aent.size());
        int j = (id - 815) + pe->isel - 3;
        while (j < 0) j += c;
        *pstr = strEntry(pe->aent[size_t(j % c)].id);
        return true;
    }
    if (id >= 917 && id <= 929)
    {
        // orig3 0x001F58E8: Control Help's line k of the pad's player (its vt slot 91)
        const int idText = IdControlHelpText(PloMenuPlayer(), id - 917);
        *pstr = idText != -1 ? text(idText) : "";
        return true;
    }
    switch (id)
    {
    case 1692: { const int k = KindDataAccess(); *pstr = k < 0 ? "" : text(k); return true; }   // orig3 0x001D1B98
    case 1693:
    {
        // orig3 0x001D1BF0: the current game's status (not at the title)
        if (FTitleShown()) { *pstr = ""; return true; }
        CRef r = RefGetProgress({"episodeCurrent"});
        *pstr = strStatus(g_gamesave, NRef(r));
        return true;
    }
    case 1694:
    {
        // orig3 0x001D1CC8: the selected slot's help
        const PAGE* pe = PpageCur();
        if (!pe || !FDataAccess()) { *pstr = ""; return true; }
        const float g = DtSlot(IslotOfRow(pe->isel));
        switch (KindDataAccess())
        {
        case m3::KIND_NEW: *pstr = text(g == 0.0f ? 0x33 : 0x32); break;
        case m3::KIND_LOAD: *pstr = text(g == 0.0f ? 0x33 : 0x34); break;
        case m3::KIND_SAVE: *pstr = text(0x36) + (g != 0.0f ? "\n" + text(0x32) : ""); break;
        default: *pstr = ""; break;
        }
        return true;
    }
    case 1695:
        // orig3 0x001D1E60 "Back : &2T&."; (QoL Q4) the page indicator and its L1 / R1 prompts on a second line
        *pstr = FDataAccess() ? text(m3::TX_BACK) + " : " + SzMenuBackGlyph() + "\n" + StrSlotPage() : "";
        return true;
    case 1696: case 1697: case 1698:
    {
        // orig3 0x001D1EF8: slot i (its episode and percent, else New Game / Empty)
        const int i = IslotOfRow(id - 1696);
        if (DtSlot(i) != 0.0f)
        {
            CRef r = RefProgressOfSlot(i, {"episodeCurrent"});
            *pstr = strStatus(g_aslot[i].save, NRef(r));
            return true;
        }
        const int k = KindDataAccess();
        *pstr = k == m3::KIND_SAVE ? text(0x55) : (k == m3::KIND_NEW || k == m3::KIND_LOAD) ? text(0x54) : "";
        return true;
    }
    default: return false;
    }
}

// orig3: 0x0021D360: a record's settings take effect: the mixer mode, the music volume (factor 1 of channel 1) and the
// effects' (channels 0 and 3) as 1 - the stored values, the headset (none natively), the screen centre (none on PC)
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

// (the camera code) the Camera Options… page's setting of a camera kind: Left/Right (fVertical false) or Up/Down reversed
bool FCamReverse3(int kind, bool fVertical) { return NRef(RefCamPref(kind, fVertical)) != 0; }
