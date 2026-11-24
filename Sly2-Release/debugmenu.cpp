// See debugmenu.h. PC debug tools, not the original game: the actions call the game's own
// functions (the world transition 0x210990, the task start 0x1651D8, the players' enable vt slot 79, the actors'
// teleport slot 38, F9's job end, F6 / F7) and never write the save themselves except where marked "[save]".
#include "debugmenu.h"
#include "cheat.h"
#include "debugphys.h"
#include "debugvis.h"
#include "actioninput.h"
#include "region.h"

#include "activate.h"
#include "actor.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "cplcy.h"
#include "cmworld.h"
#include "display.h"
#include "frontend.h"
#include "gl.h"
#include "hud.h"
#include "joy.h"
#include "lo.h"
#include "menu.h"
#include "pcinput.h"
#include "pccharacter.h"
#include "pcrc.h"
#include "pcloader.h"
#include "promptglyph.h"
#include "render.h"
#include "po.h"
#include "savedata.h"
#include "screenrt.h"
#include "skipcut.h"
#include "sly.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "vtnative.h"
#include "worldaccess.h"

#include "sly2_version.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "../platform/controller_family.h"
#include "../platform/debug_freeze.h"
#include "../platform/debug_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

extern int g_frameJoy;   // joy.cpp (the step's frame)
bool g_fDebugFreeCam = false;
bool g_fDebugInfinite = false;

const char* PchzPortVersion() { return SLY2_PORT_VERSION; }

std::string StrLocalized(const SW* psw, int id);   // splayer.cpp
float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp
void DebugUnlockAll(SW* psw);                      // game.cpp (F6)
extern std::unordered_map<int, int> g_mpoidiwrecTask;   // autoplay.cpp / brx.cpp: a filtered-out task's first world

namespace
{
// ------------------------------------------------------------ state
enum PAGEK { PK_Root, PK_Cheats, PK_Episodes, PK_Worlds, PK_Character, PK_Teleport, PK_Jobs, PK_JobList, PK_Player, PK_Speed, PK_Display,
             PK_FreeCam, PK_Overlays, PK_Extras, PK_ExtrasChars, PK_ExtrasSly, PK_ExtrasCarm, PK_ExtrasGuardEps, PK_ExtrasGuards, PK_ExtrasBosses };
// (the pages, the rows, the navigation and the status line: platform/debug_menu.h, shared with Sly 1's menu)
using PAGE = cane::debugmenu::Page;   // (kind: PAGEK)
using ITEM = cane::debugmenu::Item;
using cane::debugmenu::strSlider;
using cane::debugmenu::gStep;
cane::debugmenu::Menu s_menu;
std::vector<std::function<void(SW*)>> s_apfnDeferred;   // run by the next world step (UpdateDebugTools)
SW* s_psw = nullptr;

// input (the step's buttons before the filter, the window's left stick)
uint16_t s_grfRaw = 0, s_grfRawPrev = 0, s_grfMenuPrev = 0;
float s_xStick = 0, s_yStick = 0;
bool s_fF10Prev = false, s_fF11Prev = false;
double s_xMousePrev = -1, s_yMousePrev = -1;

// tools
bool s_fHudHidden = false;
bool s_fPerf = false;
bool s_fNoclipWant = false;
float s_rSpeed = 1.0f;
// the Free Camera page's freeze camera and the time keys (platform/debug_freeze.h)
using cane::debugmenu::Freeze;
Freeze s_freeze = Freeze::Off;
bool s_fFreezeHave = false;    // (the frozen view taken: at the first frame drawn after it was chosen)
CAMERA s_camFrozen;
cane::debugmenu::FollowAim s_aimFollow;
uint32_t s_serialFreeze = 0;   // (the world it was chosen in: a new world turns it off)
bool s_fTimeFrozen = false;
cane::debugmenu::Note s_note;  // (the short note after a key)
bool s_fF1Prev = false, s_fF8Prev = false;
LO* s_ploNoclip = nullptr;
uint32_t s_serialNoclip = 0;
glm::vec3 s_posNoclip(0.0f);   // the noclip's point: the actor's teleport point (its record less its base offset)
float s_radNoclip = 0.0f;
int s_f1044Saved = 0;          // (the actor's "camera target fixed" flag before the noclip)

constexpr int CROW_SHOWN = cane::debugmenu::kRowsShown;

void Status(const std::string& s) { s_menu.status(s); }

bool FCanOpen()
{
    return s_psw && !g_transition.fPending && g_transition.wipes == WIPES_Idle && !FSkipCutFastForward();
}

void Open()
{
    s_menu.open();
    g_fDebugFreeCam = false;   // (the free camera takes the window's input: the menu needs it)
    s_grfMenuPrev = s_grfRaw;  // (the keys that opened it are no presses for the menu)
    s_xMousePrev = s_yMousePrev = -1;
}

void Close()
{
    s_menu.close(s_grfRaw);   // (whatever is held now reaches the game only once released and pressed again)
}

void Push(PAGEK pagek, int arg = 0) { s_menu.push(pagek, arg); }

void Defer(std::function<void(SW*)> pfn, bool fClose = true)
{
    s_apfnDeferred.push_back(std::move(pfn));
    if (fClose) Close();
}

std::string StrOnOff(bool f) { return cane::debugmenu::strOnOff(f); }

// ------------------------------------------------------------ the free camera, the freeze camera, the time
void SetFreeCam(bool fOn)
{
    if (fOn == g_fDebugFreeCam) return;
    g_fDebugFreeCam = fOn;
    // (the free camera starts from the picture: a frozen view too, which it replaces)
    if (fOn && s_freeze != Freeze::Off) s_freeze = Freeze::Off;
    s_note.show(std::string("Free Camera: ") + StrOnOff(fOn));
}

void SetFreeze(Freeze freeze)
{
    // (from Off: the view now drawn is the one kept, game.cpp's next frame (ApplyDebugFreezeCam); Fixed <-> Follow
    // keep the place taken)
    if (s_freeze == Freeze::Off || freeze == Freeze::Off) s_fFreezeHave = false;
    if (s_freeze == Freeze::Off) s_serialFreeze = g_psw ? g_psw->serial : 0;
    s_freeze = freeze;
    s_note.show(std::string("Freeze Camera: ") + cane::debugmenu::freezeName(freeze));
}

bool FCanFreezeTime() { return !g_transition.fPending && g_transition.wipes == WIPES_Idle; }

void SetTimeFrozen(bool fFrozen)
{
    if (fFrozen && !FCanFreezeTime()) return;
    s_fTimeFrozen = fFrozen;
    s_note.show(fFrozen ? "Time frozen" : "Time: " + cane::debugmenu::strSpeed(s_rSpeed));
}

void SetSpeed(float r)
{
    s_rSpeed = r;
    s_fTimeFrozen = false;
    s_note.show("Speed " + cane::debugmenu::strSpeed(r));
}

// The game's camera is its own (a cutscene, a conversation's or a scripted camera, the binocucom): the frozen view
// waits (cplcy.h: the follow policy of the zones and the default one are the gameplay's)
bool FFreezeWaits()
{
    if (FCutsceneActive()) return true;
    const CPLCY* pcplcy = g_cm.ccpr > 0 ? g_cm.acpr[0].pcplcy : nullptr;
    return pcplcy && pcplcy != &g_cmpolicies.cpfollow && pcplcy != &g_cmpolicies.cpdefault;
}

// ------------------------------------------------------------ names
const char* PchzCharacter(const LO* plo)
{
    if (FIsLoDerivedFrom(plo, 0x35)) return "Sly";
    if (FIsLoDerivedFrom(plo, 0x37)) return "Bentley";
    if (FIsLoDerivedFrom(plo, 0x36)) return "Murray";
    if (FIsLoDerivedFrom(plo, 0x46)) return "Safe house";
    return nullptr;
}

std::string StrPlayer(LO* plo)
{
    if (!plo) return "none";
    const char* pchz = PchzCharacter(plo);
    char ach[64];
    std::snprintf(ach, sizeof ach, "%s %d", pchz ? pchz : "object", plo->oid);
    return ach;
}

// The world files of the 44 world records (the disc's names, as the prototype's Load menu lists them; record 18,
// the castle's interior, has no file on the retail disc).
const char* const s_apchzWorldFile[44] = {
    "jb_intro", "dvd_menu", "f_nightclub_exterior", "f_nightclub_cellar", "f_nightclub_disco", "f_nightclub_heist",
    "f_nightclub_moulinrouge", "f_nightclub_waterpump", "i_palace_ext", "i_palace_int", "i_palace_basement",
    "i_palace_ballroom", "i_temple_ext", "i_temple_int", "p_prison_ext", "p_prison_int", "p_prison_vault", "p_castle_ext",
    "p_castle_int", "p_castle_waterrails", "p_castle_guardbreak", "p_castle_wolftomb", "p_castle_heist",
    "p_castle_ewoktrainer", "p_castle_ewoktomb", "p_castle_ewokhall", "p_castle_ewokwater", "c_train_ext",
    "c_train_cabins", "c_train_a", "c_train_b", "c_train_c", "c_sawmill_ext", "c_sawmill_mulch", "c_sawmill_burn",
    "c_sawmill_lighthouse", "c_sawmill_bearcave", "c_sawmill_boss", "a_blimp_ext", "a_blimp_int",
    "a_blimp_engine_room_murray", "a_blimp_engine_room_bentley", "a_blimp_engine_room_jt", "a_blimp_boss_final"};

}  // namespace

const char* PchzDebugWorldFile(int iwrec) { return iwrec >= 0 && iwrec < 44 ? s_apchzWorldFile[iwrec] : ""; }

namespace
{
// a world record's place in the game's progress ("episode.nightclub.level.cellar"): its last part
std::string StrWorldKey(int iwrec, std::string* pstrEpisode = nullptr)
{
    const std::vector<WREC>& awrec = AwrecWorlds();
    if (iwrec < 0 || iwrec >= int(awrec.size())) return "?";
    const std::vector<int16_t> akey(awrec[size_t(iwrec)].an.begin(), awrec[size_t(iwrec)].an.end());
    const std::string s = StrProgressKey(akey);   // episode.<name>.level.<level>
    std::vector<std::string> as;
    for (size_t i = 0, j; i <= s.size(); i = j + 1)
    {
        j = s.find('.', i);
        if (j == std::string::npos) j = s.size();
        as.push_back(s.substr(i, j - i));
    }
    if (pstrEpisode) *pstrEpisode = as.size() > 1 ? as[1] : "";
    return as.empty() ? s : as.back();
}

std::string StrWorldName(int iwrec)
{
    const std::vector<WREC>& awrec = AwrecWorlds();
    if (iwrec < 0 || iwrec >= int(awrec.size())) return "?";
    if (iwrec == 0) return "Prologue (Cairo museum)";
    if (iwrec == 1) return "Episode menu (front end)";
    std::string strEp;
    std::string s = StrWorldKey(iwrec, &strEp);
    for (char& ch : s) if (ch == '_') ch = ' ';
    if (!s.empty()) s[0] = char(std::toupper(uint8_t(s[0])));
    if (awrec[size_t(iwrec)].fMain) s = "Hub (" + s + ")";
    return s;
}

std::string StrEpisodeName(int iep)
{
    if (iep == 0) return "Prologue";
    std::string s = s_psw ? StrLocalized(s_psw, IdText(0x6D + iep)) : std::string();
    char ach[32];
    std::snprintf(ach, sizeof ach, "Episode %d", iep);
    if (s.empty() || s.size() > 60) return ach;
    return std::string(ach) + ": " + s;
}

// ------------------------------------------------------------ actions
// The current player made `plo` (the prototype's "Set Current Character with Match"): the new one put into the world
// at the old one's place (vt slot 8 AddLo, slot 38 TeleportActor), the old one out (slot 9), the pad and the camera
// to the new one (slot 79 EnablePo, as the safe house does), its moves reset (slot 88 (0): its ground state).
void SwitchCharacter(SW* psw, LO* plo)
{
    LO* pcur = PloPoCurrent(psw);
    if (!plo || plo == pcur) return;
    if (plo->fRemoved) AddLo(psw, plo);
    const bool fCurChar = pcur && FIsLoDerivedFrom(pcur, 0x34);
    if (fCurChar) TeleportActor(plo, pcur);
    if (fCurChar) RemoveLo(psw, pcur);
    EnablePo(psw, plo, 1);
    if (void* pv = PvNativeVt(plo, 88)) reinterpret_cast<void (*)(LO*, int)>(pv)(plo, 0);
    CutCm(&g_cm);
    Status("character: " + StrPlayer(plo) + " (was " + StrPlayer(pcur) + ")");
}

void TeleportPlayer(SW* psw, LO* plodest)
{
    LO* pcur = PloPoCurrent(psw);
    if (!pcur || !plodest || !FIsLoDerivedFrom(pcur, 0x30)) return;
    TeleportActor(pcur, plodest);
    if (s_ploNoclip == pcur)
    {
        // (the noclip continues from there)
        if (FIsLoDerivedFrom(plodest, 0x30)) s_posNoclip = PosAloCenter(static_cast<ALO*>(plodest));
        else s_posNoclip = PosLoWorld(plodest);
    }
    CutCm(&g_cm);
    Status("teleported " + StrPlayer(pcur) + " to object " + std::to_string(plodest->oid));
}

// ------------------------------------------------------------ noclip
glm::mat3 MatHeading(float rad)
{
    const float c = std::cos(rad), s = std::sin(rad);
    return glm::mat3(glm::vec3(c, s, 0.0f), glm::vec3(-s, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
}

void PlaceNoclip(LO* plo)
{
    ALO* palo = static_cast<ALO*>(plo);
    const glm::mat3 mat = MatHeading(s_radNoclip);
    if (STEP* pstep = PstepFromLo(plo))
    {
        // as the actors' teleport (TeleportActor, slot 38): slot 29 at the point less the base offset, slot 30
        const glm::vec3 vecBase(pstep->vecBase);
        SetStepPos(pstep, s_posNoclip - mat * vecBase);
        SetStepRotation(pstep, mat);
        pstep->pxpFloor = nullptr;
        if (ACTOR* pa = PactorFromLo(plo))
        {
            pa->safe.c = 0;
            pa->posCmTarget = glm::vec4(s_posNoclip + glm::vec3(0, 0, 1) * pa->sHeight, 2.0f);
        }
    }
    else
    {
        SetAloPos(palo, s_posNoclip);
        SetAloRotation(palo, mat);
    }
    SetAloVelocityTo(palo, glm::vec3(0.0f));
    SetAloAngVelocityTo(palo, glm::vec3(0.0f));
}

void BeginNoclip(SW* psw)
{
    LO* plo = PloPoCurrent(psw);
    if (!plo || !FIsLoDerivedFrom(plo, 0x30) || FIsLoDerivedFrom(plo, 0x46))
    {
        Status("noclip: no player to fly");
        s_fNoclipWant = false;
        return;
    }
    ALO* palo = static_cast<ALO*>(plo);
    s_ploNoclip = plo;
    s_serialNoclip = psw->serial;
    const XF* pxf = PxfPos(palo);
    const glm::mat4 m = pxf ? pxf->matWorld : MatWorldOfAlo(palo);
    STEP* pstep = PstepFromLo(plo);
    s_posNoclip = glm::vec3(m * glm::vec4(pstep ? glm::vec3(pstep->vecBase) : glm::vec3(0.0f), 1.0f));
    s_radNoclip = pstep ? RadStepHeading(pstep) : std::atan2(m[0].y, m[0].x);
    // the camera follows the flight: the actor's camera target fixed at the point (+0x1044, as the scripted events
    // 0x161 do), else its focus stays near the floor it left
    if (ACTOR* pa = PactorFromLo(plo))
    {
        s_f1044Saved = pa->f1044;
        pa->f1044 = 1;
    }
    Status("noclip on: " + StrPlayer(plo));
}

void EndNoclip(SW* psw, bool fWorldGone)
{
    LO* plo = s_ploNoclip;
    s_ploNoclip = nullptr;
    if (!fWorldGone && plo && psw)
    {
        ALO* palo = static_cast<ALO*>(plo);
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        if (STEP* pstep = PstepFromLo(plo)) pstep->pxpFloor = nullptr;
        if (ACTOR* pa = PactorFromLo(plo)) pa->f1044 = s_f1044Saved;
        Status("noclip off: " + StrPlayer(plo) + " back to normal physics");
    }
}

void StepNoclip(SW* psw)
{
    LO* plo = s_ploNoclip;
    const JOYPS2& j = JoyFor(2);
    const float dt = g_clock.dt40 > 0.0f ? g_clock.dt40 : 1.0f / 60.0f;
    // (JOYPS2's buttons are in the game's layout: the libpad bits byte-swapped)
    using sly2actions::Action;
    const bool advanced = sly2actions::Active();
    auto held = [&](Action action, uint32_t grfLibpad) {
        return advanced ? sly2actions::Held(action) : (j.grfbtn & GrfbtnPs2FromLibpad(grfLibpad)) != 0;
    };
    float speed = 1100.0f;
    if (held(Action::NoclipFast, BTN_R1)) speed *= 4.0f;
    if (held(Action::NoclipSlow, BTN_L1)) speed *= 0.25f;
    // the stick moves level, relative to the camera's heading; the buttons up and down
    glm::vec3 fwd(g_cm.mat[0].x, g_cm.mat[0].y, 0.0f);
    if (glm::length(fwd) < 1e-3f) fwd = glm::vec3(g_cm.mat[2].x, g_cm.mat[2].y, 0.0f);   // (looking straight down)
    fwd = glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(1, 0, 0);
    const glm::vec3 left(-fwd.y, fwd.x, 0.0f);
    glm::vec3 v = fwd * j.y - left * j.x;
    float dz = 0.0f;
    if (held(Action::NoclipAscend, BTN_Cross | BTN_R2)) dz += 1.0f;
    if (held(Action::NoclipDescend, BTN_Circle | BTN_L2)) dz -= 1.0f;
    v.z += dz;
    if (glm::length(v) > 1.0f) v = glm::normalize(v);
    s_posNoclip += v * speed * dt;
    if (std::fabs(v.x) + std::fabs(v.y) > 0.05f) s_radNoclip = std::atan2(v.y, v.x);
    PlaceNoclip(plo);
    CheatRefillPlayers(psw);   // (the hazards flown through do not end the flight)
    if (static const bool s_fPrint = std::getenv("SLY2N_DEBUGMENUPRINT") != nullptr; s_fPrint)
    {
        const glm::vec3 c = PosAloCenter(static_cast<ALO*>(plo));
        std::printf("noclip t %.2f point %.0f %.0f %.0f center %.0f %.0f %.0f stick %.2f %.2f btn 0x%X cam %.2f %.2f %.2f\n", g_clock.t,
                    s_posNoclip.x, s_posNoclip.y, s_posNoclip.z, c.x, c.y, c.z, j.x, j.y, j.grfbtn, fwd.x, fwd.y, fwd.z);
    }
}

// ------------------------------------------------------------ the pages
// ------------------------------------------------------------ Extras (pccharacter.h)
// a row's short label: "Carmelita (Prologue)" -> "Prologue", "Tango (Episode 2)" stays
std::string StrShortName(int idef)
{
    std::string str = PcCharDef(idef).pchzName;
    const std::string strCarm = "Carmelita (";
    if (str.rfind(strCarm, 0) == 0 && str.size() > strCarm.size() && str.back() == ')') str = str.substr(strCarm.size(), str.size() - strCarm.size() - 1);
    return str;
}

std::string StrRowState(int idef, int idefCur, int idefWant)
{
    if (idef == idefWant) return "(loading)";
    if (idef == idefCur) return "(current)";
    return "";
}

int CRowsOfKind(PCCHARK kind, int iep)
{
    int c = 0;
    for (int idef = 0; idef < CPcCharDefs(); ++idef)
        if (PcCharDef(idef).kind == kind && (iep < 0 || PcCharDef(idef).iep == iep)) ++c;
    return c;
}

std::string StrKindSummary(PCCHARK kind)
{
    const PCCHARCTRL* pctrl = PPcCharController(kind);
    const int idef = pctrl && pctrl->pfnCurrent ? pctrl->pfnCurrent() : -1;
    if (idef >= 0 && idef < CPcCharDefs()) return PcCharDef(idef).pchzName;
    return CRowsOfKind(kind, -1) ? "" : "(none yet)";
}

std::string StrCharsSummary()
{
    std::string str;
    if (IdefPcCharPuppet() >= 0) str = PcCharDef(IdefPcCharPuppet()).pchzName;
    if (IdefPcCharSlyOutfit() >= 0) str += (str.empty() ? "" : ", ") + std::string("Sly: ") + StrShortName(IdefPcCharSlyOutfit());
    if (IdefPcCharPuppetWanted() >= 0 || IdefPcCharSlyOutfitWanted() >= 0) str += (str.empty() ? "" : " ") + std::string("(loading)");
    return str;
}

std::string StrExtrasSummary()
{
    std::string str = StrCharsSummary();
    for (PCCHARK kind : {PCCHARK_Guard, PCCHARK_Boss})
    {
        const PCCHARCTRL* pctrl = PPcCharController(kind);
        const int idef = pctrl && pctrl->pfnCurrent ? pctrl->pfnCurrent() : -1;
        if (idef >= 0 && idef < CPcCharDefs()) str += (str.empty() ? "" : ", ") + std::string(PcCharDef(idef).pchzName);
    }
    return str;
}

// The guard / boss rows (of the episode, or all: iep -1), chosen through the kind's controller (pcguard.cpp); a second
// choice of the current row turns it off. No controller: dimmed.
void AddKindRows(std::vector<ITEM>& a, PCCHARK kind, int iep)
{
    const PCCHARCTRL* pctrl = PPcCharController(kind);
    const int idefCur = pctrl && pctrl->pfnCurrent ? pctrl->pfnCurrent() : -1;
    if (pctrl && idefCur >= 0)
    {
        ITEM it;
        it.label = "Off";
        it.pfn = [kind] {
            Defer([kind](SW* p) {
                if (const PCCHARCTRL* pc = PPcCharController(kind); pc && pc->pfnRequest) pc->pfnRequest(p, -1);
                Status("guest: off");
            });
        };
        a.push_back(std::move(it));
    }
    for (int idef = 0; idef < CPcCharDefs(); ++idef)
    {
        const PCCHARDEF& def = PcCharDef(idef);
        if (def.kind != kind || (iep >= 0 && def.iep != iep)) continue;
        ITEM it;
        it.label = def.pchzName;
        const char* pchzStatus = pctrl && pctrl->pfnStatus ? pctrl->pfnStatus(idef) : nullptr;
        it.value = pchzStatus ? pchzStatus : idef == idefCur ? "(current)" : "";
        it.fDim = !pctrl || !pctrl->pfnRequest;
        if (!it.fDim)
            it.pfn = [kind, idef, idefCur] {
                Defer([kind, idef, idefCur](SW* p) {
                    if (const PCCHARCTRL* pc = PPcCharController(kind); pc && pc->pfnRequest) pc->pfnRequest(p, idef == idefCur ? -1 : idef);
                    Status(std::string("guest: ") + (idef == idefCur ? "off" : PcCharDef(idef).pchzName));
                });
            };
        a.push_back(std::move(it));
    }
    if (a.empty())
    {
        ITEM it;
        it.label = "(none yet)";
        it.fDim = true;
        a.push_back(std::move(it));
    }
}

// the sliders' stops (the original's values marked 1x)
const float s_agGravity[] = {0.1f, 0.17f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 5.0f};
const float s_asScale[] = {0.05f, 0.1f, 0.2f, 0.33f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f, 25.0f};

std::string StrButtons(const uint32_t* an, int c)
{
    std::string s;
    for (int i = 0; i < c; ++i)
    {
        if (i) s += " ";
        switch (an[i])
        {
        case 0x8000: s += "Left"; break;
        case 0x4000: s += "Down"; break;
        case 0x2000: s += "Right"; break;
        case 0x1000: s += "Up"; break;
        case 0x0008: s += "R1"; break;
        default: s += "?"; break;
        }
    }
    return s;
}

std::vector<ITEM> AitemPage(const PAGE& page, std::string* pstrTitle)
{
    std::vector<ITEM> a;
    SW* psw = s_psw;
    auto item = [&](const std::string& label, std::function<void()> pfn, const std::string& value = "", bool fSave = false) {
        ITEM it;
        it.label = label;
        it.value = value;
        it.fSave = fSave;
        it.pfn = std::move(pfn);
        a.push_back(std::move(it));
        return &a.back();
    };
    switch (PAGEK(page.kind))
    {
    case PK_Root:
        *pstrTitle = std::string("Sly 2: Band of Thieves PC Port (") + PchzPortVersion() + ")";
        item("Warps...", [] { Push(PK_Episodes); });
        item("Character...", [] { Push(PK_Character); }, StrPlayer(PloPoCurrent(psw)));
        item("Extras...", [] { Push(PK_Extras); }, StrExtrasSummary());
        item("Noclip", [] {
            s_fNoclipWant = !s_fNoclipWant;
            if (s_fNoclipWant) Close();
        }, StrOnOff(s_fNoclipWant));
        item("Teleport...", [] { Push(PK_Teleport); });
        item("Jobs...", [] { Push(PK_Jobs); });
        item("Player...", [] { Push(PK_Player); });
        item("Speed...", [] { Push(PK_Speed); }, s_fTimeFrozen ? std::string("Frozen") : cane::debugmenu::strSpeed(s_rSpeed));
        item("Free Camera...", [] { Push(PK_FreeCam); },
             g_fDebugFreeCam ? std::string("Free Camera") : s_freeze != Freeze::Off ? std::string("Freeze: ") + cane::debugmenu::freezeName(s_freeze) : std::string("Off"));
        item("Display...", [] { Push(PK_Display); });
        {
            int c = 0;
            for (int k = 0; k < DV_Max; ++k) c += FDebugVisWant(DEBUGVIS(k));
            item("Overlays...", [] { Push(PK_Overlays); }, c ? std::to_string(c) + " on" : std::string("Off"));
        }
        item("Cheat Codes...", [] { Push(PK_Cheats); });
        item("Reset World", [psw] {
            const int iwrec = g_transition.iwrecCur;
            if (iwrec < 0) return;
            RequestWorldWarp(psw, iwrec);
            Status("reset: world " + std::to_string(iwrec) + " reloaded at its entrance");
            Close();
        }, StrWorldName(g_transition.iwrecCur));
        item("Close", [] { Close(); });
        break;
    case PK_Cheats:
        // the original's codes (cheat.h): entered on the pause menu (d-pad, R1); here run at once
        *pstrTitle = "Cheat Codes (enter on the pause menu)";
        for (int i = 0; i < CCheatCodes(); ++i)
        {
            const CHEATDESC d = CheatCodeDesc(i);
            item(d.pchzName, [i] { Defer([i](SW* p) { RunCheatCode(p, i); Status(std::string("cheat: ") + CheatCodeDesc(i).pchzName); }); },
                 StrButtons(d.an, 6) + (i == IcheatLast() ? "  (last)" : ""), d.fSave);
        }
        break;
    case PK_Episodes:
        *pstrTitle = "Warps: Episode";
        for (int iep = 0; iep <= 8; ++iep)
            item(StrEpisodeName(iep) + "...", [iep] { Push(PK_Worlds, iep); },
                 iep == IepisodeCur() ? "(current)" : "");
        break;
    case PK_Worlds:
    {
        *pstrTitle = "Warps: " + StrEpisodeName(page.arg);
        const std::vector<WREC>& awrec = AwrecWorlds();
        for (int i = 0; i < int(awrec.size()); ++i)
        {
            if (awrec[size_t(i)].slot != page.arg) continue;
            const bool fFile = awrec[size_t(i)].fk.cb != 0;
            const std::string strFile = i < 44 ? s_apchzWorldFile[i] : "?";
            ITEM* pit = item(StrWorldName(i), fFile ? std::function<void()>([psw, i] {
                RequestWorldWarp(psw, i);
                Status("warp: world " + std::to_string(i) + " " + StrWorldName(i));
                Close();
            }) : std::function<void()>(), (i == g_transition.iwrecCur ? "(current) " : "") + strFile);
            pit->fDim = !fFile;
        }
        break;
    }
    case PK_Character:
    {
        *pstrTitle = "Character";
        LO* pcur = PloPoCurrent(psw);
        std::vector<LO*> aplo;
        for (CID cid : {0x35, 0x37, 0x36})
            for (LO* plo : AploOfClass(psw, cid))
                if (std::find(aplo.begin(), aplo.end(), plo) == aplo.end()) aplo.push_back(plo);
        const bool fSafeHouse = pcur && FIsLoDerivedFrom(pcur, 0x46);
        if (fSafeHouse)
        {
            ITEM* pit = item("(in the safe house: choose there, or leave it first)", nullptr);
            pit->fDim = true;
        }
        for (LO* plo : fSafeHouse ? std::vector<LO*>() : aplo)
        {
            std::string value = plo == pcur ? "(current)" : plo->fRemoved || !plo->fInWorld ? "(not in the world)" : "";
            item(StrPlayer(plo), [plo] { Defer([plo](SW* p) { SwitchCharacter(p, plo); }); }, value);
        }
        if (aplo.empty() && !fSafeHouse)
        {
            ITEM* pit = item("(no playable character in this world)", nullptr);
            pit->fDim = true;
        }
        break;
    }
    // PC addition (pccharacter.h, pcloader.h): guest characters, guards and bosses from other worlds' files, debug only.
    // A model whose world is not kept yet is loaded while this world runs (no reload); the choice applies when ready.
    case PK_Extras:
        *pstrTitle = "Extras";
        item("Characters...", [] { Push(PK_ExtrasChars); }, StrCharsSummary());
        item("Guards...", [] { Push(PK_ExtrasGuardEps); }, StrKindSummary(PCCHARK_Guard));
        item("Bosses...", [] { Push(PK_ExtrasBosses); }, StrKindSummary(PCCHARK_Boss));
        break;
    case PK_ExtrasChars:
    {
        *pstrTitle = "Extras: Characters";
        const int idefSly = IdefPcCharSlyOutfit(), idefSlyWant = IdefPcCharSlyOutfitWanted();
        item("Sly...", [] { Push(PK_ExtrasSly); },
             idefSlyWant >= 0 ? StrShortName(idefSlyWant) + " (loading)" : idefSly >= 0 ? StrShortName(idefSly) : std::string("Normal"));
        const int idef = IdefPcCharPuppet(), idefWant = IdefPcCharPuppetWanted();
        item("Carmelita...", [] { Push(PK_ExtrasCarm); },
             idefWant >= 0 ? StrShortName(idefWant) + " (loading)" : idef >= 0 ? StrShortName(idef) : std::string("Off"));
        // (pcrc.h: Bentley's RC chopper, flown in this world from world 8's file; again: back to the player where it stood)
        item("Bentley's RC chopper", [] {
            Defer([](SW* p) {
                const bool fOn = !FPcRcActive();
                RequestPcRc(p, fOn);
                Status(fOn ? "RC chopper: left stick fly / turn, R2 / L2 up / down, Square gun, Cross bomb" : "RC chopper: back to the player");
            });
        }, PchzPcRcStatus());
        break;
    }
    case PK_ExtrasSly:
    {
        *pstrTitle = "Extras: Sly";
        const int idefCur = IdefPcCharSlyOutfit(), idefWant = IdefPcCharSlyOutfitWanted();
        item("Normal", [] {
            Defer([](SW* p) {
                RequestPcCharSlyOutfit(p, -1);
                Status("Sly outfit: Normal");
            });
        }, idefCur < 0 && idefWant < 0 ? "(current)" : "");
        for (int idef = 0; idef < CPcCharDefs(); ++idef)
        {
            if (PcCharDef(idef).kind != PCCHARK_SlyOutfit) continue;
            item(StrShortName(idef) + " outfit", [idef] {
                Defer([idef](SW* p) {
                    RequestPcCharSlyOutfit(p, idef);
                    Status("Sly outfit: " + StrShortName(idef) + (FPcCharPending() ? " (loading its model, the world keeps running)" : ""));
                });
            }, StrRowState(idef, idefCur, idefWant));
        }
        break;
    }
    case PK_ExtrasCarm:
    {
        *pstrTitle = "Extras: Carmelita";
        const int idefCur = IdefPcCharPuppet(), idefWant = IdefPcCharPuppetWanted();
        item("Off (the player's own model)", [] {
            Defer([](SW* p) {
                RequestPcCharPuppet(p, -1);
                Status("play as: the player's own model");
            });
        }, idefCur < 0 && idefWant < 0 ? "(current)" : "");
        for (int idef = 0; idef < CPcCharDefs(); ++idef)
        {
            if (PcCharDef(idef).kind != PCCHARK_Puppet) continue;
            item(StrShortName(idef), [idef] {
                Defer([idef](SW* p) {
                    RequestPcCharPuppet(p, idef);
                    Status(std::string("play as: ") + PcCharDef(idef).pchzName + (FPcCharPending() ? " (loading her model, the world keeps running)" : ""));
                });
            }, StrRowState(idef, idefCur, idefWant));
        }
        // (her moveset on Sly: her moves, high jump and shock pistol; off: drawn over the player, its moves)
        item("Her moves", [] {
            SetPcCarmMoveset(!FPcCarmMovesetOn());
            Status(std::string("Carmelita's moves: ") + (FPcCarmMovesetOn() ? "on (her moveset on Sly, Bentley or Murray; Triangle: aim on / off)" : "off (the player's own moves)"));
        }, StrOnOff(FPcCarmMovesetOn()));
        break;
    }
    case PK_ExtrasGuardEps:
        *pstrTitle = "Extras: Guards";
        for (int iep = 0; iep <= 8; ++iep)
        {
            const int c = CRowsOfKind(PCCHARK_Guard, iep);
            if (iep == 0 && c == 0) continue;   // (Episodes 1 .. 8; the Prologue only once it has guards)
            ITEM* pit = item(StrEpisodeName(iep) + "...", [iep] { Push(PK_ExtrasGuards, iep); },
                             c ? std::to_string(c) + (c == 1 ? " guard" : " guards") : "(none yet)");
            pit->fDim = c == 0;
        }
        break;
    case PK_ExtrasGuards:
        *pstrTitle = page.arg == 0 ? std::string("Extras: Guards, Prologue") : "Extras: Guards, Episode " + std::to_string(page.arg);
        AddKindRows(a, PCCHARK_Guard, page.arg);
        break;
    case PK_ExtrasBosses:
        *pstrTitle = "Extras: Bosses";
        AddKindRows(a, PCCHARK_Boss, -1);
        break;
    case PK_Teleport:
    {
        *pstrTitle = "Teleport (in this world)";
        for (CID cid : {0x2A, 0x29})
            for (LO* plo : AploOfClass(psw, cid))
            {
                const glm::vec3 pos = PosLoWorld(plo);
                char ach[64];
                std::snprintf(ach, sizeof ach, "%.0f %.0f %.0f", pos.x, pos.y, pos.z);
                item(std::string(cid == 0x2A ? "Checkpoint " : "Warp point ") + std::to_string(plo->oid),
                     [plo] { Defer([plo](SW* p) { TeleportPlayer(p, plo); }); }, ach);
            }
        if (a.empty())
        {
            ITEM* pit = item("(no checkpoints in this world)", nullptr);
            pit->fDim = true;
        }
        break;
    }
    case PK_Jobs:
    {
        *pstrTitle = "Jobs";
        JOB* pjob = psw ? psw->tasks.pjobCur : nullptr;
        const std::string strJob = pjob && pjob->n6c != -1 ? StrLocalized(psw, pjob->n6c) : "not in a job";
        item("Complete Current Job (F9)", [] { Defer([](SW*) { FDebugCompleteJob(); }); }, strJob, true);
        item("Start a Job...", [] { Push(PK_JobList); }, "", true);
        break;
    }
    case PK_JobList:
    {
        *pstrTitle = "Start a Job (this world's jobs)";
        std::vector<LO*> ajob = AploOfClass(psw, 0x84);
        for (LO* plo : ajob)
        {
            JOB* pjob = dynamic_cast<JOB*>(plo);
            if (!pjob || pjob->n6c == -1 || !pjob->ptaskFirst) continue;
            TASK* ptask = pjob->ptaskFirst;
            // (the task's world; a filtered-out copy's first world as the loader saw it, else this world: as the
            // Replay Jobs list and --world W:TASK start it)
            int iwrec = ptask->iwrec;
            if (iwrec < 0)
                if (auto it = g_mpoidiwrecTask.find(ptask->oid); it != g_mpoidiwrecTask.end()) iwrec = it->second;
            if (iwrec < 0) iwrec = g_transition.iwrecCur;
            const OID oidTask = ptask->oid;
            const std::string strName = StrLocalized(psw, pjob->n6c);
            bool fDone = false;
            NTaskStateOid(pjob->ptaskLast ? pjob->ptaskLast->oid : oidTask, &fDone);
            item(strName, [psw, iwrec, oidTask, strName] {
                RequestTaskStart(psw, iwrec, oidTask);
                Status("start job \"" + strName + "\": task " + std::to_string(oidTask) + " in world " + std::to_string(iwrec));
                Close();
            }, fDone ? "(done)" : "", true);
        }
        if (a.empty())
        {
            ITEM* pit = item("(no jobs in this world: warp to a hub)", nullptr);
            pit->fDim = true;
        }
        break;
    }
    case PK_Player:
        *pstrTitle = "Player";
        item("Infinite Health and Power (F7)", [] { g_fDebugInfinite = !g_fDebugInfinite; }, StrOnOff(g_fDebugInfinite));
        // (debugphys.h) sliders: left / right; Cross back to 1x
        item("Gravity", [] { SetDebugGravity(s_psw, 1.0f); Status("gravity 1x"); }, strSlider(s_agGravity, GDebugGravity()))->pfnAdjust = [](int dir) {
            const float g = gStep(s_agGravity, GDebugGravity(), dir);
            SetDebugGravity(s_psw, g);
            char ach[64];
            std::snprintf(ach, sizeof ach, "gravity %gx (Sly, Bentley, Murray)", g);
            Status(ach);
        };
        item("Character Scale", [] { SetDebugCharScale(1.0f); Status("character scale 1x"); }, strSlider(s_asScale, SDebugCharScale()))->pfnAdjust = [](int dir) {
            const float s = gStep(s_asScale, SDebugCharScale(), dir);
            SetDebugCharScale(s);
            char ach[64];
            std::snprintf(ach, sizeof ach, "character scale %gx", s);
            Status(ach);
        };
        item("Refill Health and Power", [] {
            if (s_psw) CheatRefillPlayers(s_psw);
            Status("health and power refilled");
        });
        item("Unlock Gadgets, 9999 Coins, All Characters (F6)", [] { Defer([](SW* p) { DebugUnlockAll(p); Status("gadgets, coins and characters unlocked"); }); },
             "", true);
        item("Save Game Now (F5)", [] {
            Defer([](SW*) {
                const int islot = g_islotChosen >= 0 ? g_islotChosen : 0;
                Status("save: slot " + std::to_string(islot + 1) + (FWriteSaveSlot(islot) ? " written" : " not written"));
            });
        }, "slot " + std::to_string((g_islotChosen >= 0 ? g_islotChosen : 0) + 1), true);
        break;
    case PK_Speed:
    {
        *pstrTitle = "Speed (game time scale)";
        item(std::string("Freeze Time (") + cane::debugmenu::kKeyFreezeTime + ")", [] {
            SetTimeFrozen(!s_fTimeFrozen);
            if (s_fTimeFrozen) Close();
        }, StrOnOff(s_fTimeFrozen));
        for (float r : cane::debugmenu::kSpeedStops)
        {
            const std::string str = cane::debugmenu::strSpeed(r);
            item(str, [r, str] { SetSpeed(r); Status("speed " + str); s_menu.apage.pop_back(); },
                 r == s_rSpeed ? "(current)" : r == 1.0f ? "(normal)" : "");
        }
        item("Keys", nullptr, std::string(cane::debugmenu::kKeyFaster) + " faster, " + cane::debugmenu::kKeySlower + " slower")->fDim = true;
        break;
    }
    case PK_Display:
        *pstrTitle = "Display";
        item("Hide HUD (F11)", [] { s_fHudHidden = !s_fHudHidden; }, StrOnOff(s_fHudHidden));
        item("Perf Display (FPS, Position)", [] { s_fPerf = !s_fPerf; }, StrOnOff(s_fPerf));
        break;
    case PK_FreeCam:
        *pstrTitle = "Free Camera";
        item(std::string("Free Camera (") + cane::debugmenu::kKeyFreeCam + ")", [] {
            SetFreeCam(!g_fDebugFreeCam);
            if (g_fDebugFreeCam) Close();
        }, StrOnOff(g_fDebugFreeCam) + "  (W A S D, Q E, right mouse; pad: sticks, R2 / L2, Select)");
        {
            // (Select steps through the modes, Left / Right too; choosing one closes the menu: the player plays on)
            ITEM* pit = item(std::string("Freeze Camera (") + cane::debugmenu::kKeyFreezeCam + ")", [] {
                SetFreeze(cane::debugmenu::freezeNext(s_freeze));
                if (s_freeze != Freeze::Off) Close();
            }, std::string("<  ") + cane::debugmenu::freezeName(s_freeze) + "  >");
            pit->pfnAdjust = [](int dir) {
                SetFreeze(dir > 0 ? cane::debugmenu::freezeNext(s_freeze) : cane::debugmenu::freezeNext(cane::debugmenu::freezeNext(s_freeze)));
            };
        }
        item("  Fixed: the view stays. Follow: it turns to the player", nullptr)->fDim = true;
        item("  You keep the controls; cutscenes use their own camera", nullptr)->fDim = true;
        break;
    // (debugvis.h) what the game keeps hidden, drawn over the picture: each its own toggle, remembered
    case PK_Overlays:
        *pstrTitle = "Overlays (shown over the game; remembered)";
        for (int k = 0; k < DV_Max; ++k)
        {
            const DEBUGVIS dv = DEBUGVIS(k);
            item(SzDebugVis(dv), [dv] {
                SetDebugVis(dv, !FDebugVisWant(dv));
                Status(std::string(SzDebugVis(dv)) + ": " + StrOnOff(FDebugVisWant(dv)));
            }, StrOnOff(FDebugVisWant(dv)));
            if (dv == DV_Collision)
                item("  Collision Distance", [] {}, "<  " + StrDebugVisCollisionRange() + "  >")->pfnAdjust = [](int dir) {
                    StepDebugVisCollisionRange(dir);
                    Status("collision wireframe within " + StrDebugVisCollisionRange());
                };
            if (dv == DV_Labels)
            {
                item("  Label Distance", [] {}, "<  " + StrDebugVisLabelRange() + "  >")->pfnAdjust = [](int dir) {
                    StepDebugVisLabelRange(dir);
                    Status("labels within " + StrDebugVisLabelRange());
                };
                item("  Labels Only Under Cursor / Crosshair", [] { SetDebugVisLabelsCursor(!FDebugVisLabelsCursor()); },
                     StrOnOff(FDebugVisLabelsCursor()));
            }
        }
        item("All Off", [] {
            SetAllDebugVis(false);
            Status("overlays off");
        });
        break;
    }
    return a;
}

// ------------------------------------------------------------ drawing
// canvas pixels (640 x 448) of the picture's left edge (on wide pictures the 4:3 canvas is centred: negative)
float XCanvasPictureLeft() { return ((-1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f; }
float XCanvasPictureRight() { return ((1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f; }

constexpr float SIZE_TITLE = 19.0f, SIZE_ITEM = 19.0f, DY_ROW = 23.0f, Y_TOP = 14.0f;
constexpr float DEPTH = 16760000.0f;   // (over the HUD: the screen objects reach about 16.1M)

struct LAYOUT
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the panel
    float yFirst = 0;                        // the first row's top
    int crow = 0;
};
LAYOUT s_layout;

void DrawLine(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float x, float y, float size, glm::vec4 off)
{
    g_hud.DrawTextBox(font, picons, text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}

float DxLine(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size)
{
    return DxTextUi(font, picons, text, size / 51.2f);
}

void DrawPerf(const FONTBRX& font, const FONTBRX* picons)
{
    static double s_tPrev = 0.0, s_fps = 0.0;
    const double t = glfwGetTime();
    if (s_tPrev > 0.0 && t > s_tPrev) s_fps = s_fps * 0.9 + 0.1 / (t - s_tPrev);
    s_tPrev = t;
    char ach[256];
    std::string strPos = "-";
    if (LO* plo = s_psw ? PloPoCurrent(s_psw) : nullptr; plo && FIsLoDerivedFrom(plo, 0x30))
    {
        const glm::vec3 p = PosAloCenter(static_cast<ALO*>(plo));
        char achPos[96];
        std::snprintf(achPos, sizeof achPos, "%s  %.0f %.0f %.0f", StrPlayer(plo).c_str(), p.x, p.y, p.z);
        strPos = achPos;
    }
    char achFps[16];
    if (s_fps > 0.5) std::snprintf(achFps, sizeof achFps, "%.0f", s_fps);
    else std::snprintf(achFps, sizeof achFps, "--");
    std::snprintf(ach, sizeof ach, "%s FPS   world %d %s   %s%s", achFps, g_transition.iwrecCur,
                  g_transition.iwrecCur >= 0 && g_transition.iwrecCur < 44 ? s_apchzWorldFile[g_transition.iwrecCur] : "",
                  strPos.c_str(), s_ploNoclip ? "   noclip" : "");
    const float size = 15.0f;
    const float w = DxLine(font, picons, ach, size);
    const float xRight = XCanvasPictureRight() - 10.0f, y = 430.0f;
    g_hud.FillScreenRect(xRight - w - 8.0f, y - 3.0f, xRight + 6.0f, y + size * 0.909f + 4.0f, glm::vec4(0, 0, 0, 72), DEPTH / 16777216.0f - 1e-4f);
    DrawLine(font, picons, ach, xRight - w, y, size, glm::vec4(110, 110, 110, 0));
}
}  // namespace

uint16_t GrfDebugMenuFilter(uint16_t grf, float* pxl, float* pyl, float* pxr, float* pyr, uint8_t abAxis[4])
{
    if (!g_pcinput.fDebugKeys)
    {
        s_menu.fOpen = false;
        s_fTimeFrozen = false;
        s_freeze = Freeze::Off;
        return grf;
    }
    s_psw = g_psw;
    GLFWwindow* w = g_gl.window;
    const bool fF10 = FDebugKeyHeld(w, GLFW_KEY_F10), fF11 = FDebugKeyHeld(w, GLFW_KEY_F11);
    const bool fF10Pressed = fF10 && !s_fF10Prev, fF11Pressed = fF11 && !s_fF11Prev;
    s_fF10Prev = fF10;
    s_fF11Prev = fF11;
    if (fF11Pressed)
    {
        s_fHudHidden = !s_fHudHidden;
        std::printf("F11: HUD %s\n", s_fHudHidden ? "hidden" : "shown");
    }
    const uint16_t grfNew = uint16_t(grf & ~s_grfRawPrev);
    s_grfRawPrev = grf;
    s_grfRaw = grf;
    s_xStick = *pxl;
    s_yStick = *pyl;
    const bool fCombo = (grf & BTN_R1) && (grfNew & BTN_Select);   // R1 + Select (the prototype's keys)
    bool fTake = s_menu.fOpen;
    if (fF10Pressed || fCombo)
    {
        if (s_menu.fOpen) Close();
        else if (FCanOpen()) Open();
        fTake = true;   // (the keys that toggled it are the menu's)
        if (!s_menu.fOpen) s_menu.grfMaskRelease = grf;
    }
    // F1 / Shift+F1 / F8 / Shift+F8 / Ctrl+F8 (platform/debug_freeze.h; the menu closed)
    {
        const bool fF1 = FDebugKeyHeld(w, GLFW_KEY_F1), fF8 = FDebugKeyHeld(w, GLFW_KEY_F8);
        const bool fShift = FDebugKeyHeld(w, GLFW_KEY_LEFT_SHIFT) || FDebugKeyHeld(w, GLFW_KEY_RIGHT_SHIFT);
        const bool fCtrl = FDebugKeyHeld(w, GLFW_KEY_LEFT_CONTROL) || FDebugKeyHeld(w, GLFW_KEY_RIGHT_CONTROL);
        using cane::debugmenu::TimeCamKey;
        const TimeCamKey key = s_menu.fOpen ? TimeCamKey::None : cane::debugmenu::timeCamKey(fF1 && !s_fF1Prev, fF8 && !s_fF8Prev, fShift, fCtrl);
        s_fF1Prev = fF1;
        s_fF8Prev = fF8;
        switch (key)
        {
        case TimeCamKey::FreeCam: SetFreeCam(!g_fDebugFreeCam); break;
        case TimeCamKey::FreezeCam: SetFreeze(cane::debugmenu::freezeNext(s_freeze)); break;
        case TimeCamKey::FreezeTime: SetTimeFrozen(!s_fTimeFrozen); break;
        case TimeCamKey::Faster: SetSpeed(cane::debugmenu::speedFaster(s_rSpeed)); break;
        case TimeCamKey::Slower: SetSpeed(cane::debugmenu::speedSlower(s_rSpeed)); break;
        case TimeCamKey::None: break;
        }
    }
    // the time frozen: the world takes no input (as under the pause menu; the free camera's own input is not the game's:
    // the window's is not read meanwhile, game.cpp, and its pad is read raw)
    if (s_fTimeFrozen && !g_fDebugFreeCam) fTake = true;
    if (fTake)
    {
        *pxl = *pyl = *pxr = *pyr = 0.0f;
        for (int i = 0; i < 4; ++i) abAxis[i] = 0x80;
        return 0;
    }
    s_menu.grfMaskRelease &= grf;
    return uint16_t(grf & ~s_menu.grfMaskRelease);
}

bool FDebugMenuOpen() { return s_menu.fOpen; }

bool FDebugSkipUpdate(const SW* psw, const LO* plo)
{
    return s_ploNoclip && plo == s_ploNoclip && psw && psw->serial == s_serialNoclip;
}

bool FDebugNoclipCamera()
{
    return g_pcinput.fDebugKeys && s_ploNoclip && g_psw && g_psw->serial == s_serialNoclip;
}

bool FUpdateDebugFreeCamPad(CAMERA& cam, int frame)
{
    // (test aid: SLY2N_FREECAMAT=frame turns it on at that frame, from the game's camera there)
    static const int s_frameAt = std::getenv("SLY2N_FREECAMAT") ? std::atoi(std::getenv("SLY2N_FREECAMAT")) : -1;
    if (frame == s_frameAt && g_pcinput.fDebugKeys && !g_fDebugFreeCam)
    {
        g_fDebugFreeCam = true;
        cam = CameraFromCm(g_cm);   // (a headless run has not drawn every frame: the camera from the game's)
        cam.fMat = false;
        std::printf("debug menu: free camera on (test, frame %d)\n", frame);
    }
    if (!g_fDebugFreeCam) return false;
    // the active gamepad (raw buttons: the free camera's own fixed layout, not the game's bindings); the game gets no
    // window input meanwhile (game.cpp), so only a test's scripted presses reach g_joy: they count too
    static int s_jid = -1;
    static uint16_t s_grfPrev = 0;
    GLFWgamepadstate gp{};
    float xl = g_joy.xLeft, yl = g_joy.yLeft, xr = g_joy.xRight, yr = g_joy.yRight;
    uint16_t grf = g_joy.grfbtn;
    float gUp = (grf & BTN_R2) ? 1.0f : 0.0f, gDown = (grf & BTN_L2) ? 1.0f : 0.0f;
    if (g_gl.window && (s_jid = cane::input::selectActivePad(s_jid, &gp)) >= 0)
    {
        auto dz = [](float g) { return std::fabs(g) < 0.15f ? 0.0f : g; };
        auto big = [](float a, float b) { return std::fabs(b) > std::fabs(a) ? b : a; };
        xl = big(xl, dz(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X]));
        yl = big(yl, -dz(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]));
        xr = big(xr, dz(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]));
        yr = big(yr, -dz(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]));
        gUp = std::max(gUp, std::clamp((gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f, 0.0f, 1.0f));
        gDown = std::max(gDown, std::clamp((gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f, 0.0f, 1.0f));
        if (gp.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER]) grf |= BTN_R1;
        if (gp.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER]) grf |= BTN_L1;
        if (gp.buttons[GLFW_GAMEPAD_BUTTON_BACK]) grf |= BTN_Select;
    }
    const uint16_t grfNew = uint16_t(grf & ~s_grfPrev);
    s_grfPrev = grf;
    if (grfNew & BTN_Select)
    {
        // Select: back to the game (R1 + Select: the menu opens at the next step, GrfDebugMenuFilter); the buttons held
        // reach the game only once released
        g_fDebugFreeCam = false;
        s_menu.grfMaskRelease |= grf;
        s_grfRawPrev &= uint16_t(~BTN_Select);
        std::printf("debug menu: free camera off (pad)\n");
        return false;
    }
    constexpr float dt = 1.0f / 60.0f;   // (per drawn frame, as the keyboard's)
    float speed = 800.0f;
    if (grf & BTN_R1) speed *= 5.0f;
    if (grf & BTN_L1) speed *= 0.25f;
    // the look first (degrees per second at a full push), then the move along the new view
    cam.yaw -= xr * 120.0f * dt;
    cam.pitch = std::clamp(cam.pitch + yr * 90.0f * dt, -89.0f, 89.0f);
    const glm::vec3 fwd = cam.Dir();
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
    cam.pos += (fwd * yl + right * xl + glm::vec3(0, 0, 1) * (gUp - gDown)) * speed * dt;
    return true;
}

void ApplyDebugFreezeCam(CAMERA& cam)
{
    static double s_tPrev = 0.0;
    const double t = glfwGetTime();
    const float dt = s_tPrev > 0.0 ? float(std::clamp(t - s_tPrev, 0.0, 0.25)) : 1.0f / 60.0f;
    s_tPrev = t;
    if (s_freeze != Freeze::Off && (!g_pcinput.fDebugKeys || !g_psw || g_psw->serial != s_serialFreeze))
    {
        s_freeze = Freeze::Off;   // (a new world: its own camera)
        if (g_pcinput.fDebugKeys) s_note.show("Freeze Camera: Off (new world)");
    }
    if (s_freeze == Freeze::Off)
    {
        s_fFreezeHave = false;
        return;
    }
    if (!s_fFreezeHave)
    {
        // the view drawn now (the free camera's, or the game's): kept; the free camera hands the controls back
        s_camFrozen = cam;
        s_fFreezeHave = true;
        s_aimFollow.reset(cam.yaw, cam.pitch);
        g_fDebugFreeCam = false;
        std::printf("debug menu: freeze camera %s at %.0f %.0f %.0f yaw %.1f pitch %.1f\n", cane::debugmenu::freezeName(s_freeze),
                    cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw, cam.pitch);
    }
    if (g_fDebugFreeCam || FFreezeWaits()) return;   // (the game's own camera meanwhile)
    if (s_freeze == Freeze::Follow)
    {
        if (LO* plo = PloPoCurrent(g_psw); plo && FIsLoDerivedFrom(plo, 0x30))
        {
            const glm::vec3 p = PosAloCenter(static_cast<ALO*>(plo));
            s_aimFollow.step(s_camFrozen.pos.x, s_camFrozen.pos.y, s_camFrozen.pos.z, p.x, p.y, p.z, dt);
        }
        s_camFrozen.fMat = false;
        s_camFrozen.yaw = s_aimFollow.yaw;
        s_camFrozen.pitch = s_aimFollow.pitch;
    }
    const glm::vec4 rcPicture = cam.rcPicture;
    cam = s_camFrozen;
    cam.rcPicture = rcPicture;
}

bool FDebugTimeFrozen() { return g_pcinput.fDebugKeys && s_fTimeFrozen; }

bool FDebugHudHidden() { return g_pcinput.fDebugKeys && s_fHudHidden; }


void UpdateDebugMenu(SW* psw)
{
    s_psw = psw;
    if (!s_menu.fOpen || s_menu.apage.empty()) return;
    const uint16_t grfPressed = uint16_t(s_grfRaw & ~s_grfMenuPrev);
    s_grfMenuPrev = s_grfRaw;
    PAGE& page = s_menu.apage.back();
    std::string strTitle;
    std::vector<ITEM> aitem = AitemPage(page, &strTitle);
    const int c = int(aitem.size());
    if (c == 0) return;
    // (the navigation: platform/debug_menu.h; one call per 1/60 s step)
    cane::debugmenu::NavInput in;
    in.dirUpDown = (s_grfRaw & BTN_Up) || s_yStick > 0.5f ? -1 : (s_grfRaw & BTN_Down) || s_yStick < -0.5f ? 1 : 0;
    in.dirLeftRight = (s_grfRaw & BTN_Right) || s_xStick > 0.5f ? 1 : (s_grfRaw & BTN_Left) || s_xStick < -0.5f ? -1 : 0;
    in.fRightPressed = (grfPressed & BTN_Right) != 0;
    in.fLeftPressed = (grfPressed & BTN_Left) != 0;
    in.fSelect = (grfPressed & BTN_Cross) != 0;
    in.fBack = (grfPressed & (BTN_Circle | BTN_Triangle | BTN_Start)) != 0;
    // the mouse: the row under the cursor once it moves
    if (GLFWwindow* w = g_gl.window)
    {
        double x = 0, y = 0;
        glfwGetCursorPos(w, &x, &y);
        int wx = 0, wy = 0, fbw = 0, fbh = 0;
        glfwGetWindowSize(w, &wx, &wy);
        glfwGetFramebufferSize(w, &fbw, &fbh);
        if ((x != s_xMousePrev || y != s_yMousePrev) && s_xMousePrev >= 0 && wx > 0 && wy > 0)
        {
            const DISPLAY& d = g_display;
            const float xn = float((x * fbw / wx - d.presentX) / std::max(1, d.presentWidth)) * 2.0f - 1.0f;
            const float yn = 1.0f - float((y * fbh / wy - d.presentY) / std::max(1, d.presentHeight)) * 2.0f;
            const float xc = ((xn - d.uiOffset.x) / d.uiScale.x + 1.0f) * 320.0f;
            const float yc = (1.0f - (yn - d.uiOffset.y) / d.uiScale.y) * 224.0f;
            const LAYOUT& l = s_layout;
            if (xc >= l.x0 && xc < l.x1 && yc >= l.yFirst && yc < l.yFirst + DY_ROW * float(l.crow))
                in.irowMouse = page.itop + int((yc - l.yFirst) / DY_ROW);
        }
        s_xMousePrev = x;
        s_yMousePrev = y;
    }
    s_menu.navigate(aitem, in, 1.0f / 60.0f, s_grfRaw);
}

void UpdateDebugTools(SW* psw)
{
    s_psw = psw;
    UpdateDebugVis(psw);   // (debugvis.h: the event log; reads only)
    // the noclip's player gone with its world (the objects are freed): nothing of it is touched
    if (s_ploNoclip && s_serialNoclip != psw->serial)
    {
        EndNoclip(psw, true);
        s_fNoclipWant = false;
    }
    if (!g_pcinput.fDebugKeys)
    {
        if (g_clock.af[4] != 1.0f) SetClockFactor(&g_clock, 4, 1.0f);
        if (s_ploNoclip) EndNoclip(psw, false);
        // (the gravity and the character scale back to the original's)
        if (GDebugGravity() != 1.0f) SetDebugGravity(psw, 1.0f);
        if (SDebugCharScale() != 1.0f) SetDebugCharScale(1.0f);
        UpdateDebugCharScale(psw);
        OnPcWorldStep(psw, false);   // (the guest characters / outfits / guards turned off: pcloader.h)
        return;
    }
    // (test aid: SLY2N_NOCLIPAT=frame: the menu's Noclip turned on at that frame)
    if (static const int s_frameNoclip = std::getenv("SLY2N_NOCLIPAT") ? std::atoi(std::getenv("SLY2N_NOCLIPAT")) : -1;
        s_frameNoclip >= 0 && g_frameJoy == s_frameNoclip)
        s_fNoclipWant = true;
    // (test aids: SLY2N_CHARSCALE=s, SLY2N_GRAVITY=g: the sliders set from the start)
    if (static bool s_fEnvPhys = false; !s_fEnvPhys)
    {
        s_fEnvPhys = true;
        if (const char* psz = std::getenv("SLY2N_CHARSCALE")) SetDebugCharScale(float(std::atof(psz)));
        if (const char* psz = std::getenv("SLY2N_GRAVITY")) SetDebugGravity(psw, float(std::atof(psz)));
    }
    std::vector<std::function<void(SW*)>> apfn;
    apfn.swap(s_apfnDeferred);
    for (auto& pfn : apfn) pfn(psw);
    // the time scale (clock factor 4: none of the game's code sets it; a world load resets it to 1)
    // (Freeze Time: the factor 0, the game's own pause as under the pause menu: the effects' sounds faded; not across a
    // world change)
    static uint32_t s_serialTime = 0;
    if (s_fTimeFrozen && (psw->serial != s_serialTime || !FCanFreezeTime()))
    {
        s_fTimeFrozen = false;
        s_note.show("Time: " + cane::debugmenu::strSpeed(s_rSpeed) + " (new world)");
    }
    s_serialTime = psw->serial;
    if (const float r = s_fTimeFrozen ? 0.0f : s_rSpeed; g_clock.af[4] != r) SetClockFactor(&g_clock, 4, r);
    s_note.tick(1.0f / 60.0f);
    // (test aid: SLY2N_DEBUGTIMEPRINT=N prints the game clock every N steps, with the speed and the freeze)
    if (static const int s_cTimePrint = std::getenv("SLY2N_DEBUGTIMEPRINT") ? std::atoi(std::getenv("SLY2N_DEBUGTIMEPRINT")) : 0;
        s_cTimePrint > 0 && g_frameJoy % s_cTimePrint == 0)
        std::printf("debug clock: step %d t %.4f speed %g frozen %d\n", g_frameJoy, g_clock.t, s_rSpeed, int(s_fTimeFrozen));
    if (g_fDebugInfinite) CheatRefillPlayers(psw);
    UpdateDebugCharScale(psw);   // (debugphys.h: the current player's hulls)
    // noclip: follows the current player (a character switch moves it to the new one)
    if (s_ploNoclip && (!s_fNoclipWant || PloPoCurrent(psw) != s_ploNoclip || s_ploNoclip->fRemoved)) EndNoclip(psw, false);
    if (s_fNoclipWant && !s_ploNoclip && !g_transition.fPending) BeginNoclip(psw);
    if (s_ploNoclip) StepNoclip(psw);
    OnPcWorldStep(psw, true);   // (PC addition: kept worlds loaded, the guests placed on the player: pcloader.h)
    s_menu.tick(1.0f / 60.0f);
}

namespace
{
// the key's short note (top centre, about a second) and, while the time is not normal, a small corner note (not with
// the HUD hidden: clean shots)
std::string StrTimeIndicator() { return s_fHudHidden ? std::string() : cane::debugmenu::strTimeIndicator(s_rSpeed, s_fTimeFrozen); }

void DrawTimeCamNotes(const FONTBRX& font, const FONTBRX* picons)
{
    if (s_note.shown() && !s_menu.fOpen)
    {
        const float size = 17.0f;
        const float w = DxLine(font, picons, s_note.text, size);
        const float x = (XCanvasPictureLeft() + XCanvasPictureRight()) * 0.5f - w * 0.5f, y = 96.0f;   // (under the hint bar)
        const float a = s_note.alpha();
        g_hud.FillScreenRect(x - 10.0f, y - 4.0f, x + w + 10.0f, y + size * 0.909f + 5.0f, glm::vec4(0, 0, 0, 96 * a), DEPTH / 16777216.0f - 1e-4f);
        DrawLine(font, picons, s_note.text, x, y, size, glm::vec4(128, 128, 128, 0) * a);
    }
    if (const std::string str = StrTimeIndicator(); !str.empty())
    {
        const float size = 14.0f;
        const float w = DxLine(font, picons, str, size);
        const float x = XCanvasPictureLeft() + 12.0f, y = 410.0f;
        g_hud.FillScreenRect(x - 6.0f, y - 3.0f, x + w + 6.0f, y + size * 0.909f + 4.0f, glm::vec4(0, 0, 0, 72), DEPTH / 16777216.0f - 1e-4f);
        DrawLine(font, picons, str, x, y, size, glm::vec4(120, 110, 60, 0));
    }
}

void DrawDebugMenu()
{
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    if (s_fPerf) DrawPerf(*pfont, picons);
    DrawTimeCamNotes(*pfont, picons);
    if (!s_menu.fOpen || s_menu.apage.empty()) return;
    const PAGE& page = s_menu.apage.back();
    std::string strTitle;
    const std::vector<ITEM> aitem = AitemPage(page, &strTitle);
    // the colours (the prototype's: a grey title, green rows, the selected one light green; the "[save]" mark orange)
    const glm::vec4 offTitle(100, 100, 100, 0), offItem(18, 68, 18, 0), offSel(80, 128, 80, 0), offDim(38, 46, 38, 0),
        offValue(62, 72, 62, 0), offSave(128, 72, 20, 0), offHint(80, 80, 80, 0);
    const float x0 = std::max(XCanvasPictureLeft(), -1000.0f) + 14.0f;
    const float xText = x0 + 10.0f;
    // the width: the longest line
    float w = DxLine(*pfont, picons, strTitle, SIZE_TITLE);
    const int crow = std::min<int>(CROW_SHOWN, int(aitem.size()) - page.itop);
    const std::string strSave = "  [save]";
    for (int r = 0; r < crow; ++r)
    {
        const ITEM& it = aitem[size_t(page.itop + r)];
        float dx = DxLine(*pfont, picons, it.label, SIZE_ITEM);
        if (!it.value.empty()) dx += DxLine(*pfont, picons, "   " + it.value, SIZE_ITEM * 0.8f);
        if (it.fSave) dx += DxLine(*pfont, picons, strSave, SIZE_ITEM * 0.8f);
        w = std::max(w, dx);
    }
    // the close keys as icons (QoL N40): F10's keycap on the keyboard (drawn in the Start glyph's place), R1 + Select
    // for the pad in use; with "Original PlayStation Icons" the PS2 icons and the key's name
    const bool fKeyIcons = picons && PromptKeyboardActive() && !g_pcinput.fAlwaysOriginalPs;
    const PROMPTKEYGLYPH keyF10('!', GLFW_KEY_F10);
    const std::string strHint = !picons ? "Cross: Select    Circle: Back    F10 / R1+Select: Close"
                                : fKeyIcons ? "&2X&. Select    &2O&. Back    &2!&. Close"
                                : g_pcinput.fAlwaysOriginalPs ? "&2X&. Select    &2O&. Back    F10 / &2R&.+&2_&. Close"
                                : "&2X&. Select    &2O&. Back    &2R&.+&2_&. Close";
    const float sizeHint = 14.0f;
    w = std::max(w, DxLine(*pfont, picons, strHint, sizeHint));
    w = std::max(w, 300.0f);
    const bool fScroll = int(aitem.size()) > CROW_SHOWN;
    const float yFirst = Y_TOP + 8.0f + DY_ROW + 4.0f;
    const float yHint = yFirst + DY_ROW * float(crow) + 6.0f;
    const bool fStatus = s_menu.fStatusShown();
    const float yStatus = yHint + 20.0f;
    const float y1 = (fStatus ? yStatus + 20.0f : yHint + 20.0f);
    s_layout = LAYOUT{x0, Y_TOP, x0 + w + 20.0f, y1, yFirst, crow};
    g_hud.FillScreenRect(x0, Y_TOP, x0 + w + 20.0f, y1, glm::vec4(0, 0, 0, 96), DEPTH / 16777216.0f - 1e-4f);
    DrawLine(*pfont, picons, strTitle, xText, Y_TOP + 6.0f, SIZE_TITLE, offTitle);
    for (int r = 0; r < crow; ++r)
    {
        const int i = page.itop + r;
        const ITEM& it = aitem[size_t(i)];
        const float y = yFirst + DY_ROW * float(r);
        const bool fSel = i == page.isel;
        if (fSel)   // (a faint bar under the selected row)
            g_hud.FillScreenRect(x0 + 4.0f, y - 2.0f, x0 + w + 16.0f, y + DY_ROW - 3.0f, glm::vec4(40, 90, 40, 40), DEPTH / 16777216.0f - 5e-5f);
        DrawLine(*pfont, picons, it.label, xText, y, SIZE_ITEM, it.fDim ? offDim : fSel ? offSel : offItem);
        float x = xText + DxLine(*pfont, picons, it.label, SIZE_ITEM);
        if (!it.value.empty())
        {
            const std::string s = "   " + it.value;
            DrawLine(*pfont, picons, s, x, y + 3.0f, SIZE_ITEM * 0.8f, fSel ? offSel * 0.8f : offValue);
            x += DxLine(*pfont, picons, s, SIZE_ITEM * 0.8f);
        }
        if (it.fSave) DrawLine(*pfont, picons, strSave, x, y + 3.0f, SIZE_ITEM * 0.8f, offSave);
    }
    std::string hint = strHint;
    if (fScroll) hint += "    " + std::to_string(page.isel + 1) + " / " + std::to_string(aitem.size());
    if (page.isel >= 0 && page.isel < int(aitem.size()) && aitem[size_t(page.isel)].pfnAdjust) hint += "    Left / Right: adjust";
    DrawLine(*pfont, picons, hint, xText, yHint, sizeHint, offHint);
    if (fStatus) DrawLine(*pfont, picons, s_menu.strStatus, xText, yStatus, sizeHint, offValue);
}
}  // namespace

void DrawDebugOverlay(int width, int height)
{
    // (Extras: Carmelita's aim mode crosshair, pccharacter.h)
    const bool fCross = g_pcinput.fDebugKeys && FPcCarmCrosshair();
    const bool fVis = FDebugVisAny();   // (debugvis.h; the test aid SLY2N_DEBUGVIS without Debug mode too)
    const bool fNotes = s_note.shown() || !StrTimeIndicator().empty();
    if (!fVis && (!g_pcinput.fDebugKeys || !(s_menu.fOpen || s_fPerf || fCross || fNotes))) return;
    g_hud.Begin();
    if (fVis) DrawDebugVis(g_psw);
    if (fCross) DrawPcCarmCrosshair();
    if (g_pcinput.fDebugKeys) DrawDebugMenu();
    g_hud.Flush(width, height);
}
