// See binoc3.h. Ported from the Sly 3 asm (orig3 addresses on each function).

#include "binoc3.h"

#include "actor.h"
#include "alo.h"
#include "binoc.h"
#include "brx.h"
#include "game/sly3.h"
#include "so.h"

#include "cm.h"
#include "cplcy.h"
#include "photo.h"
#include <algorithm>
#include "game/scptgame3.h"
#include "game/task3.h"
#include "lo.h"
#include "screen3.h"
#include "screenrt.h"
#include "sound.h"
#include "sw.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace
{
constexpr int OID_FRAME = 1038, OID_SPEAKER_WINDOW = 1045;   // (the actor's: m_ext; the same oids in the goal worlds)
constexpr int PART_ZOOM = 0x3CC, PART_COMPASS = 0x3CD, PART_INDICATOR = 0x3CE;
constexpr int PART_WINDOW = 0x3CF, PART_WINDOW_OUT = 0x3D0, PART_WINDOW_IN = 0x3D1;

struct BINOCWIN3
{
    LO* ploActor = nullptr;
    int kind = -1;              // BINOC +0x18
    int state = -1;             // the window +0x4 (actor +0xF80)
    LO* ploFrame = nullptr;     // +0x8
    LO* ploWindow = nullptr;    // +0x10
    LO* ploSpeaker = nullptr;   // BINOC +0xA0 (asked for)
    LO* ploShown = nullptr;     // BINOC +0x40 (in the window)
    void* pprompt = nullptr;    // the window +0x14: the actor's photo helper (0x1BE4F8: class 0x34's +0x1D50; natively
                                // binoc.cpp's PostLoadPhotoPrompt, ACTOR::binoc.win.pprompt)
};
std::map<LO*, BINOCWIN3> s_mpwin;
SW* s_psw = nullptr;
uint32_t s_serial = 0;

bool FPrint() { static const bool s_f = std::getenv("SLY3_BINOCPRINT") != nullptr; return s_f; }

BINOCWIN3& Win(LO* ploActor)
{
    BINOCWIN3& w = s_mpwin[ploActor];
    if (!w.ploActor && s_psw)
    {
        w.ploActor = ploActor;
        w.ploFrame = PloScreen3(s_psw, OID_FRAME, ploActor->oid);
        w.ploWindow = PloScreen3(s_psw, OID_SPEAKER_WINDOW, ploActor->oid);
        if (ACTOR* pa = PactorFromLo(ploActor)) w.pprompt = pa->binoc.win.pprompt;
    }
    return w;
}

int NScreenState(LO* plo) { return plo ? NScreenObjState(plo) : 0; }

// orig3 0x001BE918: the frame's arguments: the current job's name (+0x60) and its first task not done's goal (+0x4C)
void SetWindowArgs(BINOCWIN3& w)
{
    if (!w.ploFrame) return;
    SetScreenObjArg(w.ploFrame, 0, -1);
    SetScreenObjArg(w.ploFrame, 1, -1);
    JOB3* pjob = g_tasks3.pjobCur;
    if (!pjob || !pjob->plo) return;
    // (+0x60 is EOPID 1370's field and +0x4C EOPID 1392's (grf 0x92): natively the loaded options hold them, the
    // fields were read as -1, so the frame's box stayed empty; the reference shows e.g. "Set off pinwheels")
    auto nopt = [](const LO* plo, int eopid, uint32_t ib) {
        for (const OPT& o : plo->aopt)
            if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].n;
        return int(plo->U32Field(0, ib, uint32_t(-1)));
    };
    const int idName = nopt(pjob->plo, 1370, 0x60);
    if (idName == -1) return;
    SetScreenObjArg(w.ploFrame, 0, idName);
    // (the job's tasks in VisitTask's order: the first with a goal (+0x4C) that is not done, 0x188A38)
    for (const auto& pt : g_tasks3.atask)
    {
        if (pt->pjob != pjob || !pt->plo) continue;
        const int idGoal = nopt(pt->plo, 1392, 0x4C);
        if (idGoal == -1 || pt->nState == 2 || pt->nState == 3) continue;
        SetScreenObjArg(w.ploFrame, 1, idGoal);
        if (FPrint()) std::printf("binoc3: frame args job %d name %d, task %d goal %d\n", pjob->oid, idName, pt->oid, idGoal);
        break;
    }
}

// the compass (the camera's heading, (angle + pi) / 2pi) and the zoom (the camera's +0x34)
void SetDials(BINOCWIN3& w)
{
    if (!w.ploFrame) return;
    // (orig3 0x1BE5A8 / 0x1BEA30: BINOC +0x0 is its actor (0x1BDA30), whose +0x1BC is the step heading (Sly 2's +0x1A8,
    // binoc.cpp's pa->radHeading); the camera's heading only when the owner is no actor)
    const ACTOR* pa = PactorFromLo(w.ploActor);
    const float heading = pa ? pa->radHeading : std::atan2(g_cm.mat[0][1], g_cm.mat[0][0]);
    SetScreenObjFraction(w.ploFrame, PART_COMPASS, (heading + 3.14159274f) * 0.159154937f);
    // (orig3 0x1BEA30: BINOC +0x34, the binocucom camera's zoom: the arc's fill, all blue at the widest, purple as it
    // zooms in (the reference's talk 4271: blue, purple while the camera closes on the lab, blue again). Natively from
    // the camera's field of view between its policy's zoomed (near) and wide (far) ends; it was 0: always purple)
    const bool fTalk = w.kind == 1;
    const float radNear = fTalk ? g_cmpolicies.cpfree.radFOVNear : g_cmpolicies.cprecon.radFOVNear;
    const float radFar = fTalk ? g_cmpolicies.cpfree.radFOVFar : g_cmpolicies.cprecon.radFOVFar;
    const float u = radFar - radNear > 1e-5f ? std::clamp((g_cm.radFOV - radNear) / (radFar - radNear), 0.0f, 1.0f) : 1.0f;
    SetScreenObjFraction(w.ploFrame, PART_ZOOM, std::getenv("SLY3_BINOCZOOM0") ? 0.0f : u);
}

bool s_fHeld = false;   // (the speaker in the window is held: the camera's state 1 of a talk)

// orig3 0x001BE408: the speaker in the window held for the camera: its vt slot 94 (0x3F, 1) (its effects, its talk
// animation 1756 and its own binocucom off: binoc.cpp HoldActorEffects), its linear constraint on (0x23EBF8(1)), its
// acceleration and velocities zero (+0x1A0, slots 40 / 41)
void HoldSpeaker(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, S3CID(0x30))) return;
    ALO* palo = static_cast<ALO*>(plo);
    HoldActorEffects(plo, true, true);
    SetAloCnstrLin(palo, 1);
    if (ACTOR* pa = PactorFromLo(plo)) pa->vecVel = glm::vec3(0.0f);
    SetAloVelocityTo(palo, glm::vec3(0.0f));
    SetAloAngVelocityTo(palo, glm::vec3(0.0f));
    if (FPrint()) std::printf("binoc3: speaker %d held\n", plo->oid);
}

// orig3 0x001BE4A8: its slot 95 (0x3F, 1) and the constraint off (0x23EBF8(0))
void ReleaseSpeaker(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, S3CID(0x30))) return;
    HoldActorEffects(plo, false, true);
    SetAloCnstrLin(static_cast<ALO*>(plo), 0);
    if (FPrint()) std::printf("binoc3: speaker %d released\n", plo->oid);
}

void SetSpeakerShown(BINOCWIN3& w)
{
    // orig3 0x001BE3D0: the shown speaker released (0x1BE4A8), the asked-for one takes the window (its head drawn
    // there: the player code's render target) and is held (0x1BE408)
    if (s_fHeld) ReleaseSpeaker(w.ploShown);
    w.ploShown = w.ploSpeaker;
    if (s_fHeld) HoldSpeaker(w.ploShown);
}

// orig3: 0x001BE5A8
void SetWindowState(BINOCWIN3& w, int n)
{
    if (n == w.state) return;
    if (w.state == -1) SetWindowArgs(w);
    const int old = w.state;
    w.state = n;
    if (FPrint()) std::printf("binoc3: actor %d window %d -> %d (kind %d)\n", w.ploActor ? w.ploActor->oid : -1, old, n, w.kind);
    switch (n)
    {
    case 0:   // opening: the frame (and a talk's window) shown
        // (orig3 0x1854C8: the task broadcast 7 for a talk, 2 for the recon view: scripts / tasks wait on it)
        Broadcast3(w.kind == 1 ? 7 : 2, nullptr);
        PsndStartSound(0x32);
        if (w.ploFrame)
        {
            ShowScreenObj(w.ploFrame, -1);
            SelectScreenObjPart(w.ploFrame, PART_COMPASS);
            SelectScreenObjPart(w.ploFrame, PART_ZOOM);
            SetDials(w);
        }
        if (w.kind == 1)
        {
            ResetDialogSubtitle();
            if (w.ploWindow)
            {
                ShowScreenObj(w.ploWindow, -1);
                SelectScreenObjPart(w.ploWindow, PART_WINDOW);
            }
        }
        SetPhotoPromptState(w.pprompt, 0);   // 0x24C6D8(+0x14, 0)
        break;
    case 1:
        if (w.kind == 1 && w.ploWindow) SelectScreenObjPart(w.ploWindow, PART_WINDOW);
        break;
    case 2:
        if (w.ploWindow) SelectScreenObjPart(w.ploWindow, PART_WINDOW_OUT);
        break;
    case 3: SetSpeakerShown(w); break;
    case 4:
        if (w.ploWindow) SelectScreenObjPart(w.ploWindow, PART_WINDOW_IN);
        break;
    case 5:   // closing
        Broadcast3(w.kind == 1 ? 8 : 3, nullptr);
        PsndStartSound(0x33);
        if (w.ploFrame) HideScreenObj(w.ploFrame);
        if (w.ploWindow) HideScreenObj(w.ploWindow);
        SetPhotoPromptState(w.pprompt, 10);
        break;
    case -1:
        if (old != 5) Broadcast3(w.kind == 1 ? 8 : 3, nullptr);   // (closed at once: the close event not sent yet)
        // (a screen object still playing (its +0x48 state) is reset to idle with its timers off)
        for (LO* plo : {w.ploFrame, w.ploWindow})
            if (plo && NScreenObjState(plo)) { SetScreenObjState(plo, 1); SetScreenObjTimers(plo, 1); }
        SetPhotoPromptState(w.pprompt, -1);
        break;
    default: break;
    }
}

// orig3: 0x001BEA30
void UpdateWindow(BINOCWIN3& w)
{
    UpdatePhotoPrompt(w.pprompt);   // 0x24CCB0(+0x14), whatever the state
    switch (w.state)
    {
    case 0:
    {
        const bool fFrame = NScreenState(w.ploFrame) == 2 || !w.ploFrame;
        const bool fWindow = w.kind != 1 || !w.ploWindow || NScreenState(w.ploWindow) == 2;
        if (fFrame && fWindow) SetWindowState(w, 1);
        break;
    }
    case 2:
        if (!w.ploWindow || !FScreenObjBusy(w.ploWindow, PART_WINDOW_OUT)) SetWindowState(w, 3);
        break;
    case 3: SetWindowState(w, 4); break;
    case 4:
        if (!w.ploWindow || !FScreenObjBusy(w.ploWindow, PART_WINDOW_IN)) SetWindowState(w, 1);
        break;
    case 5:
    {
        const bool fFrame = !w.ploFrame || NScreenState(w.ploFrame) < 2;
        const bool fWindow = w.kind != 1 || !w.ploWindow || NScreenState(w.ploWindow) < 2;
        if (fFrame && fWindow) SetWindowState(w, -1);
        break;
    }
    default: break;
    }
    if (w.state >= 0 && w.state <= 4) SetDials(w);
}
}  // namespace

// orig3: 0x001BDA88 OpenBinoc (the panel's side: the window opened; a talk with its speaker)
void OpenBinocPanel3(LO* ploActor, int kind, LO* ploSpeaker)
{
    if (!ploActor) return;
    BINOCWIN3& w = Win(ploActor);
    if (w.state != -1 && w.kind == kind && kind == 1)
    {
        SetBinocPanelSpeaker3(ploActor, ploSpeaker);
        return;
    }
    if (w.state != -1) SetWindowState(w, -1);
    w.kind = kind;
    w.ploSpeaker = ploSpeaker;
    w.ploShown = ploSpeaker;
    SetWindowState(w, 0);
}

// orig3: 0x001BDB50 CloseBinoc
void CloseBinocPanel3(LO* ploActor)
{
    if (!ploActor) return;
    BINOCWIN3& w = Win(ploActor);
    if (w.state != -1 && w.state != 5) SetWindowState(w, 5);
}

int NBinocPanelState3(LO* ploActor) { return ploActor ? Win(ploActor).state : -1; }
void ShutBinocPanel3(LO* ploActor)
{
    if (!ploActor) return;
    BINOCWIN3& w = Win(ploActor);
    if (w.state != -1) SetWindowState(w, -1);
}

bool FBinocPanel3Prompt(const void* pprompt)
{
    if (!pprompt) return false;
    for (const auto& [plo, w] : s_mpwin)
        if (w.pprompt == pprompt) return true;
    return false;
}

// orig3: 0x001BE370 (a talk's speaker changes while the window is idle)
void SetBinocPanelSpeaker3(LO* ploActor, LO* ploSpeaker)
{
    if (!ploActor) return;
    BINOCWIN3& w = Win(ploActor);
    if (ploSpeaker == w.ploSpeaker || w.state != 1 || w.kind != 1 || !ploSpeaker) return;
    w.ploSpeaker = ploSpeaker;
    SetWindowState(w, 2);
}

LO* PloBinocPanelSpeaker3(LO* ploActor) { return ploActor ? Win(ploActor).ploShown : nullptr; }

void HoldBinocPanelSpeaker3(LO* ploActor, bool fHold)
{
    if (!ploActor || s_fHeld == fHold) return;
    s_fHeld = fHold;
    LO* plo = Win(ploActor).ploShown;
    if (fHold) HoldSpeaker(plo);
    else ReleaseSpeaker(plo);
}

void InitBinocPanels3(SW* psw)
{
    s_mpwin.clear();
    s_fHeld = false;
    s_psw = psw;
    s_serial = psw ? psw->serial : 0;
    // (the script code's hooks, unless the binocucom's owner set its own: game/scptgame3.h; the camera's: binoccam3.cpp)
    ResetBinocCam3();
    if (!g_scpthooks3.pfnOpenBinoc) InstallBinocCam3();
    if (!g_scpthooks3.pfnOpenBinoc)
        g_scpthooks3.pfnOpenBinoc = [](LO* ploActor, const BINOCOPEN3&, LO* ploSpeaker, LO*, LO*, LO*) {
            OpenBinocPanel3(ploActor, ploSpeaker ? 1 : 0, ploSpeaker);
        };
    if (!g_scpthooks3.pfnCloseBinoc) g_scpthooks3.pfnCloseBinoc = CloseBinocPanel3;
    if (!g_scpthooks3.pfnBinocState) g_scpthooks3.pfnBinocState = NBinocPanelState3;
    if (!g_scpthooks3.pfnSetBinocSpeaker) g_scpthooks3.pfnSetBinocSpeaker = SetBinocPanelSpeaker3;
}

void UpdateBinocPanels3(SW* psw)
{
    if (psw != s_psw || (psw && psw->serial != s_serial)) InitBinocPanels3(psw);
    UpdateBinocCam3();
    for (auto& [plo, w] : s_mpwin) UpdateWindow(w);
}
