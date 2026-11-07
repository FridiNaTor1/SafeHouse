// See binoc3.h. Ported from the Sly 3 asm (orig3 addresses on each function).

#include "binoc3.h"

#include "cm.h"
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
    const int idName = int(pjob->plo->U32Field(0, 0x60, uint32_t(-1)));
    if (idName == -1) return;
    SetScreenObjArg(w.ploFrame, 0, idName);
    // (the job's tasks in VisitTask's order: the first with a goal (+0x4C) that is not done, 0x188A38)
    for (const auto& pt : g_tasks3.atask)
    {
        if (pt->pjob != pjob || !pt->plo) continue;
        const int idGoal = int(pt->plo->U32Field(0, 0x4C, uint32_t(-1)));
        if (idGoal == -1 || pt->nState == 2 || pt->nState == 3) continue;
        SetScreenObjArg(w.ploFrame, 1, idGoal);
        break;
    }
}

// the compass (the camera's heading, (angle + pi) / 2pi) and the zoom (the camera's +0x34)
void SetDials(BINOCWIN3& w)
{
    if (!w.ploFrame) return;
    const float heading = std::atan2(g_cm.mat[0][1], g_cm.mat[0][0]);   // (TODO(sly3): the binocucom camera's own heading +0x1BC)
    SetScreenObjFraction(w.ploFrame, PART_COMPASS, (heading + 3.14159274f) * 0.159154937f);
    SetScreenObjFraction(w.ploFrame, PART_ZOOM, 0.0f);
}

void SetSpeakerShown(BINOCWIN3& w)
{
    // orig3 0x001BE3D0: the asked-for speaker takes the window (its head drawn there: the player code's render target)
    w.ploShown = w.ploSpeaker;
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
        PsndStartSound(0x33);
        if (w.ploFrame) HideScreenObj(w.ploFrame);
        if (w.ploWindow) HideScreenObj(w.ploWindow);
        break;
    case -1:
        if (old != 5)   // (closed at once)
        {
            if (w.ploFrame) { SetScreenObjState(w.ploFrame, 1); SetScreenObjTimers(w.ploFrame, 1); }
            if (w.ploWindow) { SetScreenObjState(w.ploWindow, 1); SetScreenObjTimers(w.ploWindow, 1); }
        }
        break;
    default: break;
    }
}

// orig3: 0x001BEA30
void UpdateWindow(BINOCWIN3& w)
{
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

void InitBinocPanels3(SW* psw)
{
    s_mpwin.clear();
    s_psw = psw;
    s_serial = psw ? psw->serial : 0;
    // (the script code's hooks, unless the binocucom's owner set its own: game/scptgame3.h)
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
    for (auto& [plo, w] : s_mpwin) UpdateWindow(w);
}
