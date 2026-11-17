// splitscreen3.cpp: Sly 3's split screen on PC (splitscreen3.h, notes/SPLIT3_2026-11-12.md).
#include "splitscreen3.h"
#include "game/biplane3.h"
#include "aicarm3.h"
#include "challenge3.h"
#include "copsmap3.h"
#include "duel3.h"
#include "hackmp3.h"
#include "guard.h"

#include "activate.h"
#include "ai.h"
#include "clock.h"
#include "cm.h"
#include "cmworld.h"
#include "cplcy.h"
#include "disc.h"
#include "display.h"
#include "hud.h"
#include "joy.h"
#include "lo.h"
#include "menu.h"
#include "pcinput.h"
#include "po.h"
#include "porttext.h"
#include "promptglyph.h"
#include "screenrt.h"
#include "screen3.h"
#include "sound.h"
#include "sly.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "worldaccess.h"
#include "vr.h"
#include "game/sly3.h"
#include "game/task3.h"
#include "game/trigger3.h"
#include "pcloader.h"
#include "../platform/controller_family.h"
#include "../platform/platform.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace split3
{
namespace
{
namespace st = cane::settings;
using cane::input::Family;

enum DEV : int { DEV_None, DEV_Keyboard, DEV_Pad, DEV_Scripted };
struct SEAT
{
    DEV dev = DEV_None;
    int jid = -1;
    std::string guid;     // (a pad that comes back under another id is the same pad when its GUID matches)
    bool fLost = false;
};

struct VIEWCM
{
    CM cm;
    CMPOLICIES pol;
    LO* ploFocus = nullptr;
    bool fInit = false;
};

struct STATE
{
    LAYOUT layout = LAYOUT_Auto;
    bool fFour = false;            // (the option) four players, experimental
    int cplayerForced = 0;         // SLY3_SPLIT
    bool fActive = false;
    int cview = 1;
    SW* psw = nullptr;
    std::array<SEAT, kMaxSeats> aseat{};
    std::array<JOYPS2, kMaxSeats> ajoy{};
    std::array<std::unique_ptr<VIEWCM>, kMaxSeats> aview{};
    std::array<LO*, kMaxSeats> aplo{};
    // the binding's edges
    bool fSpacePrev = true;
    std::array<uint8_t, GLFW_JOYSTICK_LAST + 1> abPadPrev{};   // bit 0 A, 1 L1, 2 R1, 3 Back, 4 B
    bool fTabPrev = true, fBackspacePrev = true;
    bool fForcedFamily = false;    // (the tracker forced by us; an environment's test family is left alone)
    // (hackmp3.h) the hacking multiplayer game's character select after the binding
    bool fSelectPending = false, fSelect = false;
    // (s3-hack) one shared view: a two-player game with the world's one camera (option 1003 never set: the hacking
    // game's camera frames both ships, orig3 0x19F8B0); the seats still drive their players
    bool fShared = false;
    std::array<bool, kMaxSeats> afReady{};
    std::array<uint8_t, kMaxSeats> abSelPrev{};   // bit 0 left, 1 right, 2 confirm, 3 back
    int frameSelect = 0;
    bool fStarted = false;
} s;

bool FLog() { static const bool s_f = std::getenv("SLY3_SPLITLOG") != nullptr; return s_f; }

std::string IniPath() { return (cane::userDirectory() / "SplitScreen.ini").string(); }

void LoadOptions()
{
    std::ifstream in(IniPath());
    std::string line;
    while (std::getline(in, line))
    {
        int n = 0;
        if (std::sscanf(line.c_str(), "Layout=%d", &n) == 1) s.layout = LAYOUT(std::clamp(n, 0, int(LAYOUT_Max) - 1));
        else if (std::sscanf(line.c_str(), "Four Players=%d", &n) == 1) s.fFour = n != 0;
    }
}

void SaveOptions()
{
    if (std::getenv("SLY3_SPLIT")) return;   // (tests: never the profile's file)
    std::ofstream out(IniPath());
    out << "[Split Screen]\n; Layout: 0 Auto (side by side on a wide picture, stacked on a narrow one), 1 Side by Side (as the PS2), 2 Stacked\n"
        << "Layout=" << int(s.layout) << "\n; Four Players: 1 the experimental four-player split screen\nFour Players=" << (s.fFour ? 1 : 0) << "\n";
}

int CviewWanted()
{
    if (const int c = duel3::CViewsWanted()) return c;   // (s3-plane, duel3.h: the Biplane Duel's free-for-all, 3 / 4 players)
    if (s.cplayerForced) return s.cplayerForced;
    return s.fFour ? 4 : 2;
}

bool FPadPresent(int jid) { return jid >= 0 && glfwJoystickPresent(jid) && glfwJoystickIsGamepad(jid); }
std::string GuidOf(int jid) { const char* p = jid >= 0 ? glfwGetJoystickGUID(jid) : nullptr; return p ? p : ""; }

bool FDeviceTaken(DEV dev, int jid, int iseatExcept)
{
    for (int i = 0; i < s.cview; ++i)
        if (i != iseatExcept && s.aseat[i].dev == dev && !s.aseat[i].fLost && (dev != DEV_Pad || s.aseat[i].jid == jid)) return true;
    return false;
}

// the seat to bind next (unbound or lost), -1 none
int IseatBinding()
{
    if (!s.fActive) return -1;
    for (int i = 0; i < s.cview; ++i)
        if (s.aseat[i].dev == DEV_None || s.aseat[i].fLost) return i;
    return -1;
}

constexpr uint16_t s_abtnAction[PCA_Max] = {
    BTN_Up, BTN_Down, BTN_Left, BTN_Right, BTN_Square, BTN_Circle, BTN_Cross, BTN_Triangle,
    BTN_Start, BTN_Select, BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_L3, BTN_R3,
};
float Deadzone(float g) { return std::fabs(g) < 0.15f ? 0.0f : g; }

bool FKeyBinding(GLFWwindow* pwindow, int binding)
{
    if (!pwindow || binding < 0) return false;
    if (binding >= st::kMouseBindingBase && binding < st::kMouseBindingBase + st::kMouseButtonCount)
        return glfwGetMouseButton(pwindow, binding - st::kMouseBindingBase) == GLFW_PRESS;
    if (binding > GLFW_KEY_LAST) return false;
    return glfwGetKey(pwindow, binding) == GLFW_PRESS;
}

// a seat's device into libpad's buttons and the sticks (-1..1, y up)
void ReadSeatDevice(const SEAT& seat, GLFWwindow* pwindow, uint16_t* pgrf, float a[4])
{
    *pgrf = 0;
    a[0] = a[1] = a[2] = a[3] = 0.0f;
    if (seat.fLost) return;
    if (seat.dev == DEV_Pad)
    {
        GLFWgamepadstate gp{};
        if (!glfwGetGamepadState(seat.jid, &gp)) return;
        for (int i = 0; i < PCA_Max; ++i)
        {
            const int b = g_pcinput.gamepad[size_t(i)];
            bool f = false;
            if (b >= 0 && b <= GLFW_GAMEPAD_BUTTON_LAST) f = gp.buttons[b] == GLFW_PRESS;
            else if (b == st::kGamepadLeftTrigger) f = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f;
            else if (b == st::kGamepadRightTrigger) f = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f;
            if (f) *pgrf |= s_abtnAction[i];
        }
        a[0] = Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
        a[1] = -Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
        a[2] = Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
        a[3] = -Deadzone(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
    }
    else if (seat.dev == DEV_Keyboard && pwindow && glfwGetWindowAttrib(pwindow, GLFW_FOCUSED))
    {
        bool af[PCA_Max] = {};
        for (int i = 0; i < PCA_Max; ++i) af[i] = FKeyBinding(pwindow, g_pcinput.keyboard[size_t(i)]);
        if (af[PCA_Left] != af[PCA_Right]) a[0] = af[PCA_Left] ? -1.0f : 1.0f;
        if (af[PCA_Up] != af[PCA_Down]) a[1] = af[PCA_Up] ? 1.0f : -1.0f;
        for (int i = PCA_Square; i < PCA_Max; ++i)
            if (af[i]) *pgrf |= s_abtnAction[i];
        if (glfwGetKey(pwindow, GLFW_KEY_ESCAPE) == GLFW_PRESS) *pgrf |= BTN_Start;
        const bool l = FKeyBinding(pwindow, g_pcinput.cameraKeys[0]), r = FKeyBinding(pwindow, g_pcinput.cameraKeys[1]);
        const bool u = FKeyBinding(pwindow, g_pcinput.cameraKeys[2]), d = FKeyBinding(pwindow, g_pcinput.cameraKeys[3]);
        if (l != r) a[2] = l ? -1.0f : 1.0f;
        if (u != d) a[3] = u ? 1.0f : -1.0f;
    }
}

// the raw read scePadRead gives (joy.cpp UpdateJoy's) into a pad record
void FeedJoy(JOYPS2* pjoy, uint16_t grf, const float a[4])
{
    auto byteOf = [](float g, bool fInvert) {
        const float u = fInvert ? (1.0f - g) * 0.5f : (g + 1.0f) * 0.5f;
        return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(u, 0.0f), 1.0f) * 255.0f));
    };
    uint8_t ab[0x20]{};
    ab[1] = 0x73;
    ab[2] = static_cast<uint8_t>(~(grf & 0xFF));
    ab[3] = static_cast<uint8_t>(~(grf >> 8));
    ab[4] = byteOf(a[2], false);
    ab[5] = byteOf(a[3], true);
    ab[6] = byteOf(a[0], false);
    ab[7] = byteOf(a[1], true);
    ReadJoyPs2(pjoy, ab);
}

void Bind(int iseat, DEV dev, int jid)
{
    SEAT& seat = s.aseat[size_t(iseat)];
    seat.dev = dev;
    seat.jid = jid;
    seat.guid = dev == DEV_Pad ? GuidOf(jid) : "";
    seat.fLost = false;
    PsndStartSound(0x36);
    if (FLog()) std::printf("split3: player %d -> %s %d (%s)\n", iseat + 1, dev == DEV_Pad ? "pad" : dev == DEV_Keyboard ? "keyboard" : "script", jid,
                            seat.guid.c_str());
}

void ParseSeatsEnv()
{
    const char* sz = std::getenv("SLY3_SPLITSEATS");
    if (!sz) return;
    int i = 0;
    for (const char* p = sz; *p && i < kMaxSeats; ++i)
    {
        if (*p == 'k') s.aseat[size_t(i)] = SEAT{DEV_Keyboard, -1, "", false};
        else if (*p == 'p') s.aseat[size_t(i)] = SEAT{DEV_Pad, std::atoi(p + 1), GuidOf(std::atoi(p + 1)), false};
        else if (*p == 's') s.aseat[size_t(i)] = SEAT{DEV_Scripted, -1, "", false};
        p = std::strchr(p, ',');
        if (!p) break;
        ++p;
    }
}

LO* PloByOid(SW* psw, OID oid)
{
    if (!psw || oid < 0) return nullptr;
    return PloFindSwPlayerByOid3(psw, oid);   // (s3-plane: a task's proxy player gives its instance: the 2P duel's planes)
}

// players 2..4 (the forced test mode and the four-player one): the task's second player, then the gang's members
void ChoosePlayers(SW* psw)
{
    s.aplo.fill(nullptr);
    s.aplo[0] = PloPoCurrent(psw);
    std::vector<LO*> acand;
    // (copsmap3.h: a native Cops and Robbers round names player 2: Carmelita)
    if (LO* p = cops3::PloPlayerWanted(psw, 1)) acand.push_back(p);
    for (int k = 1; k < kMaxSeats; ++k)   // (s3-plane, duel3.h: the free-for-all's planes, players 2..4 in order)
        if (LO* p = duel3::PloPlayerWanted(psw, k)) acand.push_back(p);
    // (tests: SLY3_SPLITCHARS="28,25,..." players 2.. by oid first)
    if (const char* sz = std::getenv("SLY3_SPLITCHARS"))
        for (const char* p = sz; *p;)
        {
            char* pend = nullptr;
            const long oid = std::strtol(p, &pend, 10);
            if (pend == p) break;
            if (LO* plo = PloByOid(psw, OID(oid))) acand.push_back(plo);
            p = *pend == ',' ? pend + 1 : pend;
        }
    if (LO* p = PloByOid(psw, g_gametasks3.aoidPlayer[1])) acand.push_back(p);
    // (a world started at a two-player game's later task: its job's first task names the players, e.g. 1811: 24 / 28)
    if (const TASK3* pt = PtaskFind3(g_gametasks3.oidChk); pt && pt->pjob && pt->pjob->ptaskFirst)
        if (LO* p = PloByOid(psw, pt->pjob->ptaskFirst->aoidPlayer[1])) acand.push_back(p);
    for (OID oid : {24, 25, 26, 28})   // (Sly, Bentley, Murray, Carmelita)
        if (LO* p = PloByOid(psw, oid)) acand.push_back(p);
    // (four players in a two-player game: teams, 2 v 2. Players 3 / 4 are copies of players 1 / 2's characters (a second
    // Sly, a second Carmelita), each driven by its own seat; made once per world and kept: copies are not freed while
    // a world runs)
    if (s.cview >= 4 && s.aplo[0] && psw && psw->nScreenMode3 == 2)
    {
        LO* pcarm = nullptr;
        for (LO* p : acand)
            if (p && p != s.aplo[0] && p->Cid() == sly3::CID_CARMELITA) { pcarm = p; break; }
        if (pcarm)
        {
            static SW* s_pswTeams = nullptr;
            static uint32_t s_serialTeams = 0;
            static LO* s_aploCopy[2] = {};
            if (s_pswTeams != psw || s_serialTeams != psw->serial)
            {
                s_pswTeams = psw, s_serialTeams = psw->serial;
                s_aploCopy[0] = PloPcCloneIntoWorld(psw, s.aplo[0], true);
                s_aploCopy[1] = PloPcCloneIntoWorld(psw, pcarm, true);
                // (the second Sly: the world start's actor part (sly.h), then his pad below like every extra player)
                if (s_aploCopy[0]) InitSlyCopy3(s_aploCopy[0]);
                if (FLog()) std::printf("split3: teams: copies %d / %d\n", s_aploCopy[0] ? s_aploCopy[0]->oid : -1, s_aploCopy[1] ? s_aploCopy[1]->oid : -1);
            }
            acand.clear();
            acand = {pcarm, s_aploCopy[0], s_aploCopy[1]};
        }
    }
    int i = 1;
    for (LO* p : acand)
    {
        if (i >= s.cview) break;
        if (!p) continue;
        if (std::find(s.aplo.begin(), s.aplo.end(), p) != s.aplo.end()) continue;
        if (!FCidDerivedFrom(p->Cid(), sly3::CID_PO) && !FPlane3(p)) continue;   // (s3-plane: the 2P duel's planes are SOs)
        s.aplo[size_t(i++)] = p;
    }
    // the extra players into the world next to player 1 (the second player of a two-player task is placed by its
    // checkpoint already, transition.cpp)
    const glm::mat4 m0 = s.aplo[0] ? sly3::MatLoWorld3(s.aplo[0]) : glm::mat4(1.0f);
    for (int k = 1; k < s.cview; ++k)
    {
        LO* p = s.aplo[size_t(k)];
        if (!p) continue;
        if (!p->fInWorld) AddLo(psw, p);
        // (the task's second player is placed by the checkpoint (transition.cpp), but at Cops and Robbers' start from
        // the chooser v_ext's checkpoint pairs Carmelita 28 with her AI's spot (6050, 5786: no floor there), so she
        // fell out of the world. PC fallback, TODO(split): the original's placement of GAME +0x14 (0x25B9C0 i = 1):
        // one that starts more than 2500 away from player 1 is put beside player 1 like the extra players.)
        const bool fFar = s.aplo[0] && glm::length(PosLoWorld(p) - PosLoWorld(s.aplo[0])) > 2500.0f;
        if ((p->oid != g_gametasks3.aoidPlayer[1] || fFar) && s.aplo[0] && !FPlane3(p))   // (s3-plane: a plane keeps its place)
        {
            float mat[9];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) mat[r * 3 + c] = m0[r][c];
            const glm::vec3 pos = glm::vec3(m0[3]) + glm::vec3(m0[1]) * (150.0f * float(k));   // (beside player 1)
            sly3::PlaceLo3(p, mat, pos);
        }
        EnablePoExtra3(psw, p);
        // a guard-class character (Carmelita 0x3C) takes the pad by its slot 83 (0x162810): her reaction runs her
        // player job 0xE9 (aicarm3.h)
        if (GUARD* pg = PguardFromLo(p))
        {
            // (a copy of Carmelita made while the world runs has no reaction job (0x9C) yet: the guards' reset gives it,
            // and her player job 0xE9 comes from that reaction; as copsmap3's native rounds do for a hub's Carmelita)
            if (p->ploCloneOf && !PjobFind(pg, 0x9C)) ResetGuard(p, nullptr);
            if (p->ploCloneOf) ApplyGuardScaleOption3(pg);   // (her scale, EOPID 370: her start's, guard.h)
            SetCarmPlayer3(p, -1);
            SetCarmPlayer3(p, k);
        }
        if (FLog()) std::printf("split3: player %d's character %d\n", k + 1, p->oid);
    }
}

void ForceFamily(std::optional<Family> family)
{
    auto& t = cane::input::tracker();
    if (!s.fForcedFamily && t.forced()) return;   // (a test's forced family stays)
    t.force(family);
    s.fForcedFamily = family.has_value();
}

Family FamilyOfSeat(const SEAT& seat)
{
    if (seat.dev == DEV_Pad) return cane::input::identify(seat.jid).family;
    if (seat.dev == DEV_Keyboard) return Family::Keyboard;
    return Family::PlayStation;
}

// ---- drawing (remappage.cpp's helpers)
constexpr float DEPTH = 16760000.0f;
glm::vec4 Rgb(int r, int g, int b, int a = 0) { return glm::vec4(float(r), float(g), float(b), float(a)); }
void Text(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float x, float y, float size, glm::vec4 off, int halign = 1)
{
    g_hud.DrawTextBox(font, picons, text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, halign, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}
void Rect(float x0, float y0, float x1, float y1, glm::vec4 rgba, float dz = 0.0f)
{
    g_hud.FillScreenRect(x0, y0, x1, y1, rgba, (DEPTH + dz) / 16777216.0f);
}
}  // namespace

namespace
{
glm::vec4 RgbOf(uint32_t rgba) { return glm::vec4(float(rgba & 0xFF), float((rgba >> 8) & 0xFF), float((rgba >> 16) & 0xFF), 128.0f); }
std::string StrCharName(int ichar)
{
    // (the port's "Bentley|Sly|Penelope|Carmelita|Murray|Panda King" in the language)
    const std::string all = StrPort(PTX_HackCharNames);
    size_t a = 0;
    for (int i = 0; i < ichar && a != std::string::npos; ++i) a = all.find('|', a) == std::string::npos ? std::string::npos : all.find('|', a) + 1;
    if (a == std::string::npos) return hackmp3::SzChar(ichar);
    const size_t b = all.find('|', a);
    return all.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
// the hackers' select page (hackmp3.h): a card per player in the two-player HUD's style (a dark panel framed by thin
// lines in the player's colour), the hacking ship the character drives turning in it (game.cpp renders the model and
// copies it into the box), the character's name and the state
struct CARDLAYOUT { float xa, xb, yTop, yModel0, yModel1, yBottom; };
CARDLAYOUT CardLayout(int i, int c, float xLeft, float xRight)
{
    const float gap = 14.0f;
    const float wCard = std::min(146.0f, (std::min(xRight - xLeft, 700.0f) - 40.0f) / float(c) - gap);
    const float x0 = 320.0f - 0.5f * (float(c) * wCard + float(c - 1) * gap);
    CARDLAYOUT l;
    l.xa = x0 + float(i) * (wCard + gap);
    l.xb = l.xa + wCard;
    l.yTop = 96.0f;
    l.yModel0 = 124.0f;
    l.yModel1 = 124.0f + std::min(150.0f, (wCard - 8.0f) * 448.0f / 640.0f * 4.0f / 3.0f * 1.05f);
    l.yBottom = l.yModel1 + 78.0f;
    return l;
}
void FrameLines(float x0, float y0, float x1, float y1, glm::vec4 rgba, float w, float dz)
{
    Rect(x0, y0, x1, y0 + w, rgba, dz);
    Rect(x0, y1 - w, x1, y1, rgba, dz);
    Rect(x0, y0, x0 + w * 0.75f, y1, rgba, dz);
    Rect(x1 - w * 0.75f, y0, x1, y1, rgba, dz);
}
void DrawSelect(float xLeft, float xRight, float yTop, float yBottom)
{
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    Rect(xLeft - 2.0f, yTop - 2.0f, xRight + 2.0f, yBottom + 2.0f, Rgb(0, 0, 0, 128), -400.0f);
    Text(*pfont, picons, StrPort(PTX_HackSelectTitle), 320.0f, 40.0f, 30.0f, Rgb(118, 100, 40));
    const int c = s.cview;
    // (the players' colours, as the 2P HUD's frames and the tags: blue, orange, green, yellow)
    static const glm::vec4 s_aline[kMaxSeats] = {Rgb(24, 70, 128, 128), Rgb(128, 52, 16, 128), Rgb(30, 100, 40, 128), Rgb(118, 100, 20, 128)};
    static const glm::vec4 s_aoffTag[kMaxSeats] = {Rgb(40, 70, 128), Rgb(128, 70, 30), Rgb(40, 110, 50), Rgb(120, 110, 30)};
    for (int i = 0; i < c; ++i)
    {
        const CARDLAYOUT l = CardLayout(i, c, xLeft, xRight);
        const float xc = 0.5f * (l.xa + l.xb);
        const int ichar = hackmp3::IcharPlayer(i);
        const glm::vec4 rgb = RgbOf(hackmp3::RgbaChar(ichar));
        const bool fReady = s.afReady[size_t(i)];
        const glm::vec4 line = s_aline[i];
        // the panel and its frame (the 2P HUD's: black, thin coloured lines, the model's box inside)
        Rect(l.xa, l.yTop, l.xb, l.yBottom, Rgb(6, 8, 16, 128), -392.0f);
        FrameLines(l.xa, l.yTop, l.xb, l.yBottom, line, 2.0f, -391.0f);
        if (fReady) FrameLines(l.xa - 3.0f, l.yTop - 3.0f, l.xb + 3.0f, l.yBottom + 3.0f, glm::vec4(rgb.r, rgb.g, rgb.b, 128.0f), 2.0f, -391.0f);
        char ach[48];
        std::snprintf(ach, sizeof ach, StrPort(PTX_SplitPlayerN).c_str(), i + 1);
        Text(*pfont, picons, ach, xc, l.yTop + 3.0f, 17.0f, s_aoffTag[i]);
        // the ship's box (game.cpp copies the model's picture in after the page), framed
        const float xm0 = l.xa + 5.0f, xm1 = l.xb - 5.0f;
        FrameLines(xm0, l.yModel0, xm1, l.yModel1, glm::vec4(line.r * 0.6f, line.g * 0.6f, line.b * 0.6f, 128.0f), 1.5f, -388.0f);
        // the name in the character's colour, the D-pad's arrows beside it while choosing
        const glm::vec4 offName(std::min(128.0f, rgb.r + 30.0f), std::min(128.0f, rgb.g + 30.0f), std::min(128.0f, rgb.b + 30.0f), 0.0f);
        Text(*pfont, picons, StrCharName(ichar), xc, l.yModel1 + 8.0f, 20.0f, offName);
        // (choosing: the D-pad's icon before the word, as the hint's)
        Text(*pfont, picons, fReady ? StrPort(PTX_HackReady) : "&2+&. " + StrPort(PTX_HackChoose), xc, l.yModel1 + 44.0f, 15.0f,
             fReady ? Rgb(60, 120, 60) : Rgb(80, 80, 80));
    }
    Text(*pfont, picons, StrPort(PTX_HackSelectHint), 320.0f, 392.0f, 16.0f, Rgb(70, 80, 100));
}
}  // namespace

bool FHackSelectShown() { return s.fActive && s.fSelect; }
int CHackCards() { return FHackSelectShown() ? s.cview : 0; }

bool FHackCardModel(int i, int dx, int dy, VIEWRECT* prect, int* pichar)
{
    if (!FHackSelectShown() || i < 0 || i >= s.cview) return false;
    const float xLeft = ((-1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
    const float xRight = ((1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
    const float yTop = ((g_display.uiOffset.y - 1.0f) / g_display.uiScale.y + 1.0f) * 224.0f;
    const float yBottom = ((g_display.uiOffset.y + 1.0f) / g_display.uiScale.y + 1.0f) * 224.0f;
    const CARDLAYOUT l = CardLayout(i, s.cview, xLeft, xRight);
    auto px = [&](float x) { return (x - xLeft) / (xRight - xLeft) * float(dx); };
    auto py = [&](float y) { return (1.0f - (y - yTop) / (yBottom - yTop)) * float(dy); };
    const int x0 = int(std::lround(px(l.xa + 5.0f))), x1 = int(std::lround(px(l.xb - 5.0f)));
    const int y0 = int(std::lround(py(l.yModel1))), y1 = int(std::lround(py(l.yModel0)));
    *prect = VIEWRECT{x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
    *pichar = hackmp3::IcharPlayer(i);
    return true;
}

LAYOUT Layout() { return s.layout; }
bool FFourPlayers() { return s.fFour; }

void Startup()
{
    if (s.fStarted) return;
    s.fStarted = true;
    if (const char* sz = std::getenv("SLY3_ELFFIND"))   // (debug: where the ELF holds a word, e.g. a setter's address)
    {
        const uint32_t w = uint32_t(std::strtoul(sz, nullptr, 16));
        for (uint32_t va = 0x100000; va < 0x480000; va += 4)
            try { if (g_disc.U32Elf(va) == w) std::printf("elffind 0x%X at 0x%X\n", w, va); }
            catch (...) {}
    }
    if (!std::getenv("SLY3_SPLIT")) LoadOptions();
    if (const char* sz = std::getenv("SLY3_SPLIT")) s.cplayerForced = std::clamp(std::atoi(sz), 0, kMaxSeats);
    if (s.cplayerForced == 1) s.cplayerForced = 0;
    if (const char* sz = std::getenv("SLY3_SPLITLAYOUT"))
        s.layout = !std::strcmp(sz, "side") ? LAYOUT_SideBySide : !std::strcmp(sz, "stacked") ? LAYOUT_Stacked : LAYOUT_Auto;
    ParseSeatsEnv();
}


void OnWorldLoaded(SW* psw)
{
    ResetScores3();   // (challenge3.h: the score boards' entries name the old world's objects)
    Startup();
    if (std::getenv("SLY3_COPSDUMP")) cops3::DumpLandmarks(psw);
    cops3::OnWorldLoaded(psw);   // (copsmap3.h: a native Cops and Robbers round's world: the screen mode 2)
    duel3::OnWorldLoaded(psw);   // (s3-plane, duel3.h: a Biplane Duel free-for-all round)
    s.psw = psw;
    const bool fWas = s.fActive;
    // (the test entry --mp, SLY3_SPLITSTART=1: a world started at a two-player game's later task, e.g. --world 3:2413,
    // comes in with that task's job not yet current (the harness sets it after the load): the job's screen mode then)
    if (psw && psw->nScreenMode3 != 2 && std::getenv("SLY3_SPLITSTART"))
        if (const TASK3* pt = PtaskFind3(g_gametasks3.oidChk); pt && pt->pjob)
            psw->nScreenMode3 = NScreenModeFromPrefix3(psw, pt->pjob->oid, -1);
    // (ResetCarm3: at the world's reset before the load now, guard.cpp ResetGuards: here it dropped a Carmelita a start
    // script had just made the character)
    s.fActive = psw && (psw->nScreenMode3 == 2 || s.cplayerForced >= 2);
    for (auto& p : s.aview) p.reset();
    if (!s.fActive)
    {
        s.cview = 1;
        if (fWas) { ForceFamily(std::nullopt); SetClockFactor(&g_clock, 5, 1.0f); }
        return;
    }
    s.cview = CviewWanted();
    ChoosePlayers(psw);
    cops3::OnPlayersChosen(psw);   // (copsmap3.h: the round's spawns, the hub's guards out)
    duel3::OnPlayersChosen(psw);   // (s3-plane, duel3.h: players 3 / 4's planes configured and placed)
    s.fSelectPending = hackmp3::FGame(psw);   // (hackmp3.h: the hackers' select once the seats are bound)
    s.fSelect = false;
    // (s3-hack) the hacking game is one picture in the original: its script never turns on the two players' cameras
    // (option 1003); the ships' camera (orig3 0x19BF08 with +0x30C, the other ship: 0x19F8B0 / 0x19F9C8) frames both,
    // as the reference's frames show (one view, both portraits on top). SLY3_HACKSPLIT=1: a view per player.
    s.fShared = hackmp3::FGame(psw) && !std::getenv("SLY3_HACKSPLIT");

    if (FLog()) std::printf("split3: on, %d views (screen mode %d)\n", s.cview, psw->nScreenMode3);
}

bool FActive() { return s.fActive; }
int CView() { return s.fActive && !s.fShared ? s.cview : 1; }
bool FSharedView() { return s.fActive && s.fShared; }
int CSeats() { return s.fActive ? s.cview : 1; }
bool FBinding() { return IseatBinding() >= 0 || s.fSelect; }

int Seat0Device()
{
    if (!s.fActive) return -2;
    const SEAT& seat = s.aseat[0];
    if (seat.dev == DEV_Pad && !seat.fLost) return seat.jid;
    if (seat.dev == DEV_Keyboard) return -1;
    return -3;   // (unbound / scripted: neither)
}

int FamilySeat(int iseat) { return int(FamilyOfSeat(s.aseat[size_t(std::clamp(iseat, 0, kMaxSeats - 1))])); }

namespace
{
// (hackmp3.h) the hackers' select: each seat steps its character (Left / Right: the D-pad or the stick, the arrows or
// A / D) skipping the ones another player has, confirms (Cross / Space / Enter) or takes it back (Circle / Backspace);
// when every seat is ready the game goes on. Scripted seats (headless, tests): SLY3_HACKCHARS="c1,c2,..." their
// characters (hackmp3::CHAR), ready at once, or after SLY3_HACKSELECTHOLD frames (screenshots).
void UpdateSelect(GLFWwindow* pwindow, const std::array<uint8_t, GLFW_JOYSTICK_LAST + 1>& abPad, int frame)
{
    (void)abPad;
    if (s.fSelectPending && IseatBinding() < 0)
    {
        s.fSelectPending = false;
        s.fSelect = true;
        s.afReady.fill(false);
        s.abSelPrev.fill(0xFF);
        s.frameSelect = frame;
        hackmp3::ResetChoice(s.cview);
        if (const char* sz = std::getenv("SLY3_HACKCHARS"))
        {
            int i = 0;
            for (const char* p = sz; *p && i < s.cview; ++i)
            {
                char* pend = nullptr;
                const long n = std::strtol(p, &pend, 10);
                if (pend == p) break;
                hackmp3::SetCharPlayer(i, int(n));
                p = *pend == ',' ? pend + 1 : pend;
            }
        }
        if (FLog()) std::printf("split3: the hackers' select (%d players)\n", s.cview);
    }
    if (!s.fSelect) return;
    const bool fFocused = pwindow && glfwGetWindowAttrib(pwindow, GLFW_FOCUSED);
    static const int s_cHold = std::getenv("SLY3_HACKSELECTHOLD") ? std::atoi(std::getenv("SLY3_HACKSELECTHOLD")) : 0;
    bool fAll = true;
    for (int i = 0; i < s.cview; ++i)
    {
        const SEAT& seat = s.aseat[size_t(i)];
        uint8_t b = 0;
        if (seat.dev == DEV_Pad && FPadPresent(seat.jid))
        {
            GLFWgamepadstate gp{};
            if (glfwGetGamepadState(seat.jid, &gp))
            {
                const float x = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
                b = uint8_t((gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] == GLFW_PRESS || x < -0.6f ? 1 : 0) |
                            (gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] == GLFW_PRESS || x > 0.6f ? 2 : 0) |
                            (gp.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS ? 4 : 0) | (gp.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS ? 8 : 0));
            }
        }
        else if (seat.dev == DEV_Keyboard && fFocused)
        {
            auto key = [&](int k) { return glfwGetKey(pwindow, k) == GLFW_PRESS; };
            b = uint8_t((key(GLFW_KEY_LEFT) || key(GLFW_KEY_A) ? 1 : 0) | (key(GLFW_KEY_RIGHT) || key(GLFW_KEY_D) ? 2 : 0) |
                        (key(GLFW_KEY_SPACE) || key(GLFW_KEY_ENTER) ? 4 : 0) | (key(GLFW_KEY_BACKSPACE) ? 8 : 0));
        }
        else if (seat.dev == DEV_Scripted && frame - s.frameSelect >= s_cHold)
            s.afReady[size_t(i)] = true;
        const uint8_t bEdge = uint8_t(b & ~s.abSelPrev[size_t(i)]);
        s.abSelPrev[size_t(i)] = b;
        if (!s.afReady[size_t(i)] && (bEdge & 3))
        {
            const int d = (bEdge & 1) ? -1 : 1;
            hackmp3::SetCharPlayer(i, hackmp3::IcharStep(i, hackmp3::IcharPlayer(i), d));
            PsndStartSound(0x35);
        }
        if (bEdge & 4) { s.afReady[size_t(i)] = true; PsndStartSound(0x35); }
        if (bEdge & 8) s.afReady[size_t(i)] = false;
        fAll = fAll && s.afReady[size_t(i)];
    }
    if (fAll)
    {
        s.fSelect = false;
        if (FLog() || std::getenv("SLY3_CHALPRINT"))
        {
            std::printf("split3: hackers:");
            for (int i = 0; i < s.cview; ++i) std::printf(" %d=%s", i + 1, hackmp3::SzChar(hackmp3::IcharPlayer(i)));
            std::printf("\n");
        }
    }
}
}  // namespace

void UpdateSeats(GLFWwindow* pwindow, int frame)
{
    if (!s.fActive) return;
    // a headless or scripted run: the seats without a device are scripted (no binding screen)
    if (!pwindow)
        for (int i = 0; i < s.cview; ++i)
            if (s.aseat[size_t(i)].dev == DEV_None) s.aseat[size_t(i)].dev = DEV_Scripted;
    // lost pads (and found again: the same GUID under any id)
    for (int i = 0; i < s.cview; ++i)
    {
        SEAT& seat = s.aseat[size_t(i)];
        if (seat.dev != DEV_Pad) continue;
        const bool fThere = FPadPresent(seat.jid) && GuidOf(seat.jid) == seat.guid;
        if (!fThere && !seat.fLost)
        {
            seat.fLost = true;
            if (FLog()) std::printf("split3: player %d's pad is gone\n", i + 1);
        }
    }
    // the binding: Space for the keyboard, the bottom face button of a pad; L1 / R1 (Tab) the layout, Select
    // (Backspace) four players
    const int iseat = IseatBinding();
    std::array<uint8_t, GLFW_JOYSTICK_LAST + 1> abPad{};
    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid)
    {
        GLFWgamepadstate gp{};
        if (!pwindow || !FPadPresent(jid) || !glfwGetGamepadState(jid, &gp)) continue;
        abPad[size_t(jid)] = uint8_t((gp.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS ? 1 : 0) |
                                     (gp.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] == GLFW_PRESS ? 2 : 0) |
                                     (gp.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] == GLFW_PRESS ? 4 : 0) |
                                     (gp.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS ? 8 : 0));
    }
    const bool fFocused = pwindow && glfwGetWindowAttrib(pwindow, GLFW_FOCUSED);
    const bool fSpace = fFocused && glfwGetKey(pwindow, GLFW_KEY_SPACE) == GLFW_PRESS;
    const bool fTab = fFocused && glfwGetKey(pwindow, GLFW_KEY_TAB) == GLFW_PRESS;
    const bool fBackspace = fFocused && glfwGetKey(pwindow, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
    if (iseat >= 0)
    {
        SEAT& seat = s.aseat[size_t(iseat)];
        if (fSpace && !s.fSpacePrev && !FDeviceTaken(DEV_Keyboard, -1, iseat)) Bind(iseat, DEV_Keyboard, -1);
        else
            for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid)
                if ((abPad[size_t(jid)] & 1) && !(s.abPadPrev[size_t(jid)] & 1) && !FDeviceTaken(DEV_Pad, jid, iseat))
                {
                    Bind(iseat, DEV_Pad, jid);
                    break;
                }
        (void)seat;
        bool fLayout = fTab && !s.fTabPrev, fFour = fBackspace && !s.fBackspacePrev;
        for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid)
        {
            fLayout = fLayout || ((abPad[size_t(jid)] & 6) && !(s.abPadPrev[size_t(jid)] & 6));
            fFour = fFour || ((abPad[size_t(jid)] & 8) && !(s.abPadPrev[size_t(jid)] & 8));
        }
        if (fLayout)
        {
            s.layout = LAYOUT((int(s.layout) + 1) % int(LAYOUT_Max));
            PsndStartSound(0x35);
            SaveOptions();
        }
        if (fFour && !s.cplayerForced && !vr::FActive())
        {
            s.fFour = !s.fFour;
            s.cview = CviewWanted();
            for (int i = s.cview; i < kMaxSeats; ++i) s.aseat[size_t(i)] = SEAT{};
            ChoosePlayers(s.psw);
            for (auto& p : s.aview) p.reset();
            PsndStartSound(0x35);
            SaveOptions();
        }
    }
    s.fSpacePrev = fSpace;
    s.fTabPrev = fTab;
    s.fBackspacePrev = fBackspace;
    s.abPadPrev = abPad;
    UpdateSelect(pwindow, abPad, frame);
    // paused under the binding screen and the select (clock factor 5: unused by the game)
    SetClockFactor(&g_clock, 5, IseatBinding() >= 0 || s.fSelect ? 0.0f : 1.0f);
    // the last used device off: player 1's prompts are player 1's device's
    ForceFamily(FamilyOfSeat(s.aseat[0]));
    // the other seats' pad records
    for (int i = 1; i < s.cview; ++i)
    {
        uint16_t grf = 0;
        float a[4] = {};
        if (IseatBinding() < 0) ReadSeatDevice(s.aseat[size_t(i)], pwindow, &grf, a);
        // (tests) SLY3_SPLITPRESS2..4: a scripted seat's presses (joy.h's syntax: "CROSS@600+8,LY=0@700+60")
        if (s.aseat[size_t(i)].dev == DEV_Scripted)
        {
            static std::vector<JOYPRESS> s_aapress[kMaxSeats];
            static bool s_afParsed[kMaxSeats] = {};
            if (!s_afParsed[i])
            {
                s_afParsed[i] = true;
                const std::string name = "SLY3_SPLITPRESS" + std::to_string(i + 1);
                if (const char* sz = std::getenv(name.c_str())) FParseJoyPresses(sz, s_aapress[i]);
            }
            for (const JOYPRESS& p : s_aapress[i])
                if (frame >= p.frame && frame < p.frame + p.c)
                {
                    if (p.axis < 0) grf |= p.grfbtn;
                    else a[p.axis == 0 ? 0 : p.axis == 1 ? 1 : p.axis == 2 ? 2 : 3] = p.axis == 1 || p.axis == 3 ? -((p.bAxis * 2) / 255.0f - 1.0f) : (p.bAxis * 2) / 255.0f - 1.0f;
                }
        }
        if (s.aseat[size_t(i)].dev == DEV_Scripted && g_pfnScriptedSeatPilot) g_pfnScriptedSeatPilot(i, frame, a, &grf);
        FeedJoy(&s.ajoy[size_t(i)], grf, a);
        s.ajoy[size_t(i)].grfbtnHandled = 0;
    }
    // (tests) SLY3_SPLITTELEPORT2..4="X Y Z@F;...": player N's character put at (X, Y, Z) at frame F (the second Sly's
    // routes: SLY2N_TELEPORT counts every Sly's updates)
    for (int i = 1; i < s.cview; ++i)
    {
        static const char* s_asz[kMaxSeats] = {nullptr, std::getenv("SLY3_SPLITTELEPORT2"), std::getenv("SLY3_SPLITTELEPORT3"), std::getenv("SLY3_SPLITTELEPORT4")};
        if (!s_asz[i] || !s.aplo[size_t(i)]) continue;
        for (const char* q = s_asz[i]; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            float x = 0, y = 0, z = 0;
            int f = -1;
            if (std::sscanf(q, "%f %f %f@%d", &x, &y, &z, &f) != 4 || f != frame) continue;
            if (STEP* pst = PstepFromLo(static_cast<ALO*>(s.aplo[size_t(i)])))
            {
                SetStepPos(pst, glm::vec3(x, y, z) - glm::vec3(pst->vecBase));
                SetAloVelocityTo(pst->palo, glm::vec3(0.0f));
            }
        }
    }
}

void (*g_pfnScriptedSeatPilot)(int iseat, int frame, float a[4], uint16_t* pgrf) = nullptr;

LO* PloPlayer(SW* psw, int i)
{
    if (i == 0) return psw ? PloPoCurrent(psw) : nullptr;
    if (!s.fActive || i < 0 || i >= s.cview) return nullptr;
    return s.aplo[size_t(i)];
}

int IExtraPlayerOf(const LO* plo)
{
    if (!s.fActive || !plo) return -1;
    for (int i = 1; i < s.cview; ++i)
        if (s.aplo[size_t(i)] == plo) return i;
    return -1;
}

void SetPlayerCharacter(SW* psw, int i, LO* plo)
{
    if (FLog()) std::printf("split3: player %d's character -> %d\n", i, plo ? plo->oid : -1);
    if (!psw || i <= 0 || i >= kMaxSeats) return;
    LO* pold = s.aplo[size_t(i)];
    if (pold == plo) return;
    if (pold && PguardFromLo(pold)) SetCarmPlayer3(pold, -1);
    if (!s.fActive || i >= s.cview) return;
    s.aplo[size_t(i)] = plo;
    if (plo && PguardFromLo(plo)) SetCarmPlayer3(plo, i);
}

void GivePlayerObject(SW* psw, int i, LO* plo)
{
    if (!psw || !plo || !s.fActive || i <= 0 || i >= s.cview) return;
    SetPlayerCharacter(psw, i, plo);
    if (!plo->fInWorld) AddLo(psw, plo);
    EnablePoExtra3(psw, plo);
    if (FLog()) std::printf("split3: player %d drives %d (cid 0x%X)\n", i + 1, plo->oid, plo->Cid());
}

const JOYPS2* PjoyForPadctl(const PADCTL* ppc, const CM** ppcm)
{
    if (!s.fActive || !ppc) return nullptr;
    for (int i = 1; i < s.cview; ++i)
        if (s.aplo[size_t(i)] && s.aplo[size_t(i)] == ppc->plo)
        {
            if (ppcm && s.aview[size_t(i)]) *ppcm = &s.aview[size_t(i)]->cm;
            return &s.ajoy[size_t(i)];
        }
    return nullptr;
}

namespace
{
int s_iviewUpdating = 0;
LO* PloPlayerView() { return s.aplo[size_t(s_iviewUpdating)]; }
const CMJOY* PjoyView()
{
    static CMJOY s_joy;
    const JOYPS2& j = s.ajoy[size_t(s_iviewUpdating)];
    s_joy = CMJOY{};
    s_joy.x = j.x;
    s_joy.y = j.y;
    s_joy.uDeflect = j.uDeflect;
    s_joy.x2 = j.x2;
    s_joy.y2 = j.y2;
    s_joy.uDeflect2 = j.uDeflect2;
    return &s_joy;
}
}  // namespace

void UpdateCameras(SW* psw)
{
    if (!s.fActive || !psw) return;
    cops3::Update(psw);   // (copsmap3.h: a native Cops and Robbers round's rules)
    duel3::Update(psw);   // (s3-plane, duel3.h: the free-for-all's respawns, the end)
    s.aplo[0] = PloPoCurrent(psw);
    // a copied Carmelita's scale follows her template's (EOPID 370, guard.h: the template's arrives by its setter once the
    // job's options are applied, after the copy was made)
    for (int i = 1; i < s.cview; ++i)
        if (LO* p = s.aplo[size_t(i)]; p && p->ploCloneOf)
            if (GUARD* pg = PguardFromLo(p))
                if (const GUARD* pt = PguardFromLo(p->ploCloneOf); pt && pt->gScale3 != pg->gScale3) ApplyGuardScaleOption3(pg);
    if (static const bool s_fPos = std::getenv("SLY3_SPLITPOS") != nullptr; s_fPos)   // (debug: the players' places, each second)
    {
        static float s_t = 0.0f;
        if (g_clock.t - s_t > 1.0f || g_clock.t < s_t)
        {
            s_t = g_clock.t;
            for (int i = 0; i < s.cview; ++i)
                if (LO* plo = s.aplo[size_t(i)]; plo)
                {
                    glm::vec3 sc(1.0f);
                    if (FCidDerivedFrom(plo->Cid(), sly3::CID_ALO))
                    {
                        const glm::mat4 m = MatWorldOfAlo(static_cast<ALO*>(plo));
                        sc = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
                    }
                    float sMin = 1e9f, sMax = 0.0f, zMin = 1e9f, zMax = -1e9f;
                    int c = 0;
                    if (FCidDerivedFrom(plo->Cid(), sly3::CID_ALO))
                        for (const auto& pxf : static_cast<ALO*>(plo)->axf) zMin = std::min(zMin, pxf->matWorld[3].z), zMax = std::max(zMax, pxf->matWorld[3].z);
                    if (FCidDerivedFrom(plo->Cid(), sly3::CID_ALO))
                        for (const auto& pxf : static_cast<ALO*>(plo)->axf)
                            for (int k = 0; k < 3; ++k)
                            {
                                const float l = glm::length(glm::vec3(pxf->matWorld[k]));
                                sMin = std::min(sMin, l), sMax = std::max(sMax, l), ++c;
                            }
                    if (static const bool s_fXf = std::getenv("SLY3_SPLITXFDUMP") != nullptr; s_fXf && FCidDerivedFrom(plo->Cid(), sly3::CID_ALO))
                    {
                        for (const OPT& o : plo->aopt)
                            std::printf("optdump %d eopid %d n %d g %.3f\n", plo->oid, o.eopid, o.aval.empty() ? 0 : o.aval[0].n, o.aval.empty() ? 0.0f : o.aval[0].ag[0]);
                        const glm::vec3 r0 = PosLoWorld(plo);
                        int k = 0;
                        for (const auto& pxf : static_cast<ALO*>(plo)->axf)
                        {
                            const glm::vec3 d = glm::vec3(pxf->matWorld[3]) - r0;
                            std::printf("xfdump %d %d kind %d tag %d scale %.3f %.3f %.3f rel %.1f %.1f %.1f local scale %.3f %.3f %.3f pos %.2f %.2f %.2f load %.3f\n", plo->oid, k++, pxf->kind, pxf->tag,
                                        glm::length(glm::vec3(pxf->matWorld[0])), glm::length(glm::vec3(pxf->matWorld[1])), glm::length(glm::vec3(pxf->matWorld[2])), d.x, d.y, d.z,
                                        glm::length(glm::vec3(pxf->mat[0])), glm::length(glm::vec3(pxf->mat[1])), glm::length(glm::vec3(pxf->mat[2])), pxf->mat[3].x, pxf->mat[3].y, pxf->mat[3].z,
                                        glm::length(glm::vec3(pxf->matLoad[0])));
                        }
                    }
                    std::printf("split3: t %.1f player %d (%d) at %.0f %.0f %.0f scale %.3f %.3f %.3f records %d scale %.3f..%.3f height %.1f\n", g_clock.t, i + 1, plo->oid,
                                PosLoWorld(plo).x, PosLoWorld(plo).y, PosLoWorld(plo).z, sc.x, sc.y, sc.z, c / 3, sMin, sMax, zMax - zMin);
                }
        }
    }
    for (int i = 1; i < s.cview && !s.fShared; ++i)
    {
        LO* plo = s.aplo[size_t(i)];
        if (!plo) continue;
        auto& pv = s.aview[size_t(i)];
        if (!pv) pv = std::make_unique<VIEWCM>();
        auto fFollow = [](const CM& cm) {
            for (int k = 0; k < cm.ccpr; ++k)
                if (cm.acpr[size_t(k)].pcplcy == &g_cmpolicies.cpfollow) return true;
            return false;
        };
        bool fActivate = false;
        if (!pv->fInit || pv->ploFocus != plo || (!fFollow(pv->cm) && fFollow(g_cm)))
        {
            fActivate = true;
            // the world camera's runtime, its focus this player's character: the follow policies (the zones') kept and
            // retargeted, player 1's scripted / recon / free / attached views dropped
            pv->cm = g_cm;
            pv->pol = g_cmpolicies;
            int c = 0;
            for (int k = 0; k < pv->cm.ccpr; ++k)
            {
                CPR cpr = pv->cm.acpr[size_t(k)];
                if (cpr.pcplcy != &g_cmpolicies.cpfollow && cpr.pcplcy != &g_cmpolicies.cpdefault) continue;
                if (cpr.psoFocus == s.aplo[0]) cpr.psoFocus = plo;
                pv->cm.acpr[size_t(c++)] = cpr;
            }
            pv->cm.ccpr = c;
            // (the policies keep their own focus too (CPLCY::Activate): retargeted, else the copy followed player 1)
            if (pv->pol.cpfollow.psoFocus == s.aplo[0]) pv->pol.cpfollow.psoFocus = plo;
            if (pv->pol.cpdefault.psoFocus == s.aplo[0]) pv->pol.cpdefault.psoFocus = plo;
            pv->cm.psoFocusPrev = nullptr;
            pv->cm.fCut = std::max(pv->cm.fCut, 1);
            pv->ploFocus = plo;
            pv->fInit = true;
        }
        std::swap(g_cm, pv->cm);
        std::swap(g_cmpolicies, pv->pol);
        // (the copy's top policy activated for its focus: when the world camera's top was a scripted / attached view, the
        // follow policy under it had not been activated in this world (its zone entry +pcme still the last world's or
        // null): the hubs 3 / 4 Cops and Robbers rounds' first frame crashed in PosCmeFocus3)
        if (fActivate && g_cm.ccpr > 0 && g_cm.acpr[0].pcplcy) g_cm.acpr[0].pcplcy->Activate(g_cm.acpr[0].psoFocus, g_cm.acpr[0].pv);
        SyncPlaneCameraView3(plo, &g_cm);   // (s3-plane, game/biplane3.h: a plane player's own camera; nothing for others)
        auto pfnPlo = g_cmworld.pfnPloPlayer;
        auto pfnJoy = g_cmworld.pfnPjoy;
        s_iviewUpdating = i;
        g_cmworld.pfnPloPlayer = PloPlayerView;
        g_cmworld.pfnPjoy = PjoyView;
        if (0.0f < g_clock.dtReal) UpdateCm(&g_cm);
        g_cmworld.pfnPloPlayer = pfnPlo;
        g_cmworld.pfnPjoy = pfnJoy;
        std::swap(g_cm, pv->cm);
        std::swap(g_cmpolicies, pv->pol);
        if (static int s_c = 0; FLog() && (++s_c % 120) == 0)
        {
            const glm::vec3 p = PosLoWorld(plo);
            std::printf("split3: view %d camera %.0f %.0f %.0f (policies %d, follow %d) player %d at %.0f %.0f %.0f; world camera %.0f %.0f %.0f (policies %d, follow %d)\n",
                        i, pv->cm.pos.x, pv->cm.pos.y, pv->cm.pos.z, pv->cm.ccpr, int(fFollow(pv->cm)), plo->oid, p.x, p.y, p.z,
                        g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.ccpr, int(fFollow(g_cm)));
        }
    }
}

VIEWRECT RectView(int iview, int dx, int dy)
{
    VIEWRECT r{0, 0, dx, dy};
    const int c = CView();
    if (c <= 1) return r;
    if (c >= 3)
    {
        // 2 x 2: 1 top left, 2 top right, 3 bottom left, 4 bottom right
        const int col = iview & 1, row = iview >> 1;
        r.dx = dx / 2;
        r.dy = dy / 2;
        r.x = col * (dx - r.dx);
        r.y = row == 0 ? dy - r.dy : 0;
        return r;
    }
    bool fSide = s.layout == LAYOUT_SideBySide;
    if (s.layout == LAYOUT_Auto) fSide = float(dx) / float(std::max(dy, 1)) >= 2.0f;   // (21:9 and wider)
    if (fSide)
    {
        r.dx = dx / 2;
        r.x = iview == 0 ? 0 : dx - r.dx;
    }
    else
    {
        r.dy = dy / 2;
        r.y = iview == 0 ? dy - r.dy : 0;
    }
    return r;
}

bool FHudCanvasView(int iview, float rAspectView, int dx, int dy, VIEWRECT* pclip)
{
    *pclip = VIEWRECT{0, 0, dx, dy};
    g_hud.band2p.f = false;
    const float rCanvas = 4.0f / 3.0f;
    const bool f2pHud = s.psw && s.psw->nScreenMode3 == 2;
    if (!f2pHud)
    {
        // a one-player HUD (the forced test mode): the whole canvas in each view
        g_display.uiScale = rAspectView > rCanvas ? glm::vec2(rCanvas / rAspectView, 1.0f) : glm::vec2(1.0f, rAspectView / rCanvas);
        g_display.uiOffset = glm::vec2(0.0f);
        return true;
    }
    // (four players, PC: teams in the 2 x 2 grid's columns: the left column (views 1 / 3) the robbers', the right (2 / 4)
    // the cops': each view gets its team's band of the original HUD: the portrait framing in the team's colour)
    const int band = s.cview >= 3 ? (iview & 1) : iview;
    if (iview > 3 || band > 1) return false;
    // player i's band (canvas NDC y 0..1 the top: player 1; -1..0 the bottom: player 2) fitted into the view; the band
    // alone is drawn there, its view box widened to the view's edges and its portrait panel kept at the side (hud.h
    // band2p): nothing of the other band and no cut-out
    const float rBand = 2.0f * rCanvas;
    const float sx = rAspectView > rBand ? rBand / rAspectView : 1.0f;
    const float sy = rAspectView > rBand ? 1.0f : rAspectView / rBand;
    const float c = band == 0 ? 0.5f : -0.5f;
    g_display.uiScale = glm::vec2(sx, 2.0f * sy);
    g_display.uiOffset = glm::vec2(0.0f, -c * 2.0f * sy);
    static const bool s_fOld = std::getenv("SLY3_SPLITHUDCLIP") != nullptr;   // (debug: the round-1 fitted, clipped band)
    if (s_fOld)
    {
        pclip->dx = std::max(1, int(std::lround(sx * float(dx))));
        pclip->dy = std::max(1, int(std::lround(sy * float(dy))));
        pclip->x = (dx - pclip->dx) / 2;
        pclip->y = (dy - pclip->dy) / 2;
        return true;
    }
    g_hud.band2p.f = true;
    g_hud.band2p.band = band;
    g_hud.band2p.iview = iview;
    g_hud.band2p.ex = 320.0f / sx - 320.0f;
    g_hud.band2p.ey = 112.0f / sy - 112.0f;
    return true;
}

bool FRectScene(int iview, int dx, int dy, VIEWRECT* prect)
{
    VIEWRECT rDummy;
    if (!prect) prect = &rDummy;
    *prect = VIEWRECT{0, 0, dx, dy};
    const int band = s.cview >= 3 ? (iview & 1) : iview;   // (four players: the team's band, FHudCanvasView)
    if (!s.fActive || iview > 3 || band > 1 || !(s.psw && s.psw->nScreenMode3 == 2) || std::getenv("SLY3_SPLITHUDCLIP")) return false;
    const CHud::BOX2P& b = g_hud.abox2p[band];
    if (b.frame < g_hud.iframe2p - 1) return false;
    const float rAspectView = float(dx) / float(std::max(dy, 1));
    const float rBand = 8.0f / 3.0f;
    const float sx = rAspectView > rBand ? rBand / rAspectView : 1.0f;
    const float sy = rAspectView > rBand ? 1.0f : rAspectView / rBand;
    const float ex = 320.0f / sx - 320.0f, ey = 112.0f / sy - 112.0f;
    const float c = band == 0 ? 0.5f : -0.5f;
    const float offy = -c * 2.0f * sy;
    // canvas (640 x 448, y down) -> the view's pixels (y up)
    auto px = [&](float x) { return ((x / 320.0f - 1.0f) * sx + 1.0f) * 0.5f * float(dx); };
    auto py = [&](float y) { return ((1.0f - y / 224.0f) * 2.0f * sy + offy + 1.0f) * 0.5f * float(dy); };
    const float x0 = px(b.hole.x - ex), x1 = px(b.hole.z + ex), y0 = py(b.hole.w + ey), y1 = py(b.hole.y - ey);
    prect->x = std::clamp(int(std::floor(x0)), 0, dx - 1);
    prect->y = std::clamp(int(std::floor(y0)), 0, dy - 1);
    prect->dx = std::clamp(int(std::ceil(x1)), prect->x + 1, dx) - prect->x;
    prect->dy = std::clamp(int(std::ceil(y1)), prect->y + 1, dy) - prect->y;
    return true;
}

bool FBannerArea(int dx, int dy, VIEWRECT* prect, glm::vec2* pscale, glm::vec2* poffset)
{
    if (!s.fActive || s.cview != 2 || !(s.psw && s.psw->nScreenMode3 == 2) || std::getenv("SLY3_SPLITBANNERFULL")) return false;
    const VIEWRECT r0 = RectView(0, dx, dy), r1 = RectView(1, dx, dy);
    if (r0.dx >= dx) return false;   // (stacked: the original strip between the bands)
    // the centre column: from player 1's view box to player 2's (the portraits and gauges between them)
    int x0 = r0.x + r0.dx, x1 = r1.x;
    VIEWRECT b0, b1;
    if (FRectScene(0, r0.dx, r0.dy, &b0)) x0 = r0.x + b0.x + b0.dx;
    if (FRectScene(1, r1.dx, r1.dy, &b1)) x1 = r1.x + b1.x;
    if (x1 - x0 < dx / 8) { const int c = dx / 2; x0 = c - dx / 16; x1 = c + dx / 16; }
    // under the bands (the gauges end at the band's bottom): each view shows its band fitted, sy of the view's height
    const float rAspectView = float(r0.dx) / float(std::max(r0.dy, 1));
    const float sy = rAspectView > 8.0f / 3.0f ? 1.0f : rAspectView / (8.0f / 3.0f);
    const float margin = 0.02f * float(dy);
    float hArea = 0.5f * (1.0f - sy) * float(dy) - margin;
    hArea = std::max(hArea, 0.12f * float(dy));
    const float wCol = float(x1 - x0);
    // the canvas (640 x 448, square units on a 4:3 canvas) at up to 1.6 x the column's width (the banner's text is
    // under half the canvas wide; its lines and backing are cut at the column's edges), the banner's strip (48 units)
    // at most 80% of the area's height
    float k = 1.6f * wCol / 640.0f;
    k = std::min(k, 0.8f * hArea / 48.0f);
    const float yc = margin + 0.5f * hArea;   // (pixels from the bottom, y up)
    prect->x = x0;
    prect->dx = x1 - x0;
    prect->y = int(std::floor(margin));
    prect->dy = std::max(1, int(std::ceil(hArea)));
    *pscale = glm::vec2(640.0f * k / float(dx), 448.0f * k / float(dy));
    *poffset = glm::vec2(float(x0 + x1) / float(dx) - 1.0f, 2.0f * yc / float(dy) - 1.0f);
    return true;
}

const CM& CmView(int iview)
{
    if (iview > 0 && iview < kMaxSeats && s.aview[size_t(iview)]) return s.aview[size_t(iview)]->cm;
    return g_cm;
}

void DrawOverlay(int width, int height)
{
    if (!s.fActive) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const int iseat = IseatBinding();
    g_hud.Begin();
    // each view's player tag (its corner, in its player's colour), on the full picture's canvas
    static const glm::vec4 s_aoff[kMaxSeats] = {Rgb(20, 50, 110), Rgb(110, 40, 10), Rgb(20, 90, 30), Rgb(100, 90, 10)};
    const float xLeft = ((-1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
    const float xRight = ((1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f;
    const float yTop = ((g_display.uiOffset.y - 1.0f) / g_display.uiScale.y + 1.0f) * 224.0f;   // (canvas y down)
    const float yBottom = ((g_display.uiOffset.y + 1.0f) / g_display.uiScale.y + 1.0f) * 224.0f;
    auto xCanvas = [&](int x) { return xLeft + (xRight - xLeft) * float(x) / float(std::max(width, 1)); };
    auto yCanvas = [&](int y) { return yTop + (yBottom - yTop) * (1.0f - float(y) / float(std::max(height, 1))); };
    // (the job's result stamp / Job Failed cover the whole picture: no tags, lines or markers over them)
    bool fWhole = false;
    for (int oid : {1066, 1067})
        if (LO* p = s.psw ? PloScreen3(s.psw, oid) : nullptr; p && NScreenObjState(p) >= 2) fWhole = true;
    for (int i = 0; i < s.cview && !s.fSelect && !fWhole && !s.fShared; ++i)
    {
        const VIEWRECT r = RectView(i, width, height);
        // (the two-player HUD: the tag in its view box's lower corner, off the portrait panel and the gauges)
        VIEWRECT rs;
        const bool fBox = FRectScene(i, r.dx, r.dy, &rs);
        char ach[64];
        std::snprintf(ach, sizeof ach, StrPort(PTX_SplitPlayerN).c_str(), i + 1);
        // (four players in a two-player game: the teams' colours, the left column the robbers' blue, the right the cops')
        glm::vec4 off = s.cview >= 3 && fBox ? s_aoff[i & 1] : s_aoff[i];
        if (glm::vec4 offDuel; duel3::FTagOffset(i, &offDuel)) off = offDuel;   // (s3-plane, duel3.h: a free-for-all's player colours)
        if (fBox) Text(*pfont, picons, ach, xCanvas(r.x + rs.x) + 14.0f, yCanvas(r.y + rs.y) - 30.0f, 18.0f, off, 0);
        else Text(*pfont, picons, ach, xCanvas(r.x) + 14.0f, yCanvas(r.y + r.dy) + 8.0f, 18.0f, s_aoff[i], 0);
    }
    // the dividing lines
    if (s.cview >= 2 && !s.fSelect && !fWhole && !s.fShared)
    {
        const VIEWRECT r1 = RectView(1, width, height);
        if (r1.x > 0) Rect(xCanvas(r1.x) - 1.0f, yTop, xCanvas(r1.x) + 1.0f, yBottom, Rgb(0, 0, 0, 128), 10.0f);
        if (s.cview >= 3 || r1.x == 0) Rect(xLeft, yCanvas(height / 2) - 1.0f, xRight, yCanvas(height / 2) + 1.0f, Rgb(0, 0, 0, 128), 10.0f);
        if (s.cview >= 3) Rect(xCanvas(width / 2) - 1.0f, yTop, xCanvas(width / 2) + 1.0f, yBottom, Rgb(0, 0, 0, 128), 10.0f);
    }
    if (iseat >= 0)
    {
        // the binding screen: a dark page, the players' rows, the options
        Rect(xLeft - 2.0f, yTop - 2.0f, xRight + 2.0f, yBottom + 2.0f, Rgb(0, 0, 0, 100), -400.0f);
        Text(*pfont, picons, StrPort(PTX_SplitTitle), 320.0f, 60.0f, 30.0f, Rgb(100, 50, 2));
        float y = 120.0f;
        for (int i = 0; i < s.cview; ++i, y += 44.0f)
        {
            const SEAT& seat = s.aseat[size_t(i)];
            std::string line;
            char ach[160];
            if (i == iseat)
            {
                // "Player 2, press (X) / Space": the Cross drawn as the PlayStation icon (the bottom face button of any pad)
                std::snprintf(ach, sizeof ach, StrPort(seat.fLost ? PTX_SplitReconnect : PTX_SplitPress).c_str(), i + 1,
                              StrPort(PTX_KeySpace).c_str());
                line = ach;
                ForceFamily(Family::PlayStation);
                Text(*pfont, picons, line, 320.0f, y, 24.0f, Rgb(122, 104, 52));
                ForceFamily(FamilyOfSeat(s.aseat[0]));
                continue;
            }
            std::snprintf(ach, sizeof ach, StrPort(PTX_SplitPlayerN).c_str(), i + 1);
            line = ach;
            line += StrPort(PTX_Sep);
            if (seat.dev == DEV_Keyboard) line += StrPort(PTX_SplitKeyboard);
            else if (seat.dev == DEV_Pad)
            {
                const char* pchz = glfwGetGamepadName(seat.jid);
                line += pchz ? StrToGameText(pchz) : StrToGameText(cane::input::familyDisplayName(FamilyOfSeat(seat)));
            }
            else line += StrPort(PTX_SplitWaiting);
            Text(*pfont, picons, line, 320.0f, y, 22.0f, i < iseat ? Rgb(62, 62, 62) : Rgb(40, 40, 40));
        }
        static const PTX s_aptxLayout[LAYOUT_Max] = {PTX_SplitLayoutAuto, PTX_SplitLayoutStacked, PTX_SplitLayoutSide};
        std::string line = StrPort(PTX_SplitLayout) + StrPort(PTX_Sep) + StrPort(s_aptxLayout[s.layout]) + "   (&2L&. &2R&. / Tab)";
        Text(*pfont, picons, line, 320.0f, 340.0f, 18.0f, Rgb(42, 56, 70));
        if (!s.cplayerForced && !vr::FActive())
        {
            line = StrPort(PTX_SplitFourPlayers) + StrPort(PTX_Sep) + StrPort(s.fFour ? PTX_On : PTX_Off) + "   (&2_&. / Backspace)";
            Text(*pfont, picons, line, 320.0f, 366.0f, 18.0f, Rgb(42, 56, 70));
        }
    }
    if (s.fSelect) DrawSelect(xLeft, xRight, yTop, yBottom);
    if (!fWhole) cops3::DrawOverlay(width, height, xLeft, xRight, yTop, yBottom);   // (copsmap3.h: the native round's markers, scores)
    if (!fWhole) duel3::DrawOverlay(width, height, xLeft, xRight, yTop, yBottom);   // (s3-plane, duel3.h: the free-for-all's kills, messages)
    g_hud.Flush(width, height);
}
// orig3 SW +0x4FA4 (option 1003): per world
namespace
{
const SW* s_pswTwoCameras = nullptr;
uint32_t s_serialTwoCameras = 0;
bool s_fTwoCameras = false;
}

void SetTwoCameras(SW* psw, bool f)
{
    s_pswTwoCameras = psw;
    s_serialTwoCameras = psw ? psw->serial : 0;
    s_fTwoCameras = f;
    if (FLog()) std::printf("split3: two-camera mode %d (option 1003)\n", int(f));
}

bool FTwoCameras(const SW* psw)
{
    return psw && s_fTwoCameras && s_pswTwoCameras == psw && s_serialTwoCameras == psw->serial;
}

int CViewExtra()
{
    return FActive() ? CView() - 1 : 0;
}

}  // namespace split3
