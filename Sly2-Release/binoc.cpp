#include "actioninput.h"
// The binocucom (orig ACTOR +0xF00, module 0x19CBF8..0x19DE10; the reference's m_19D330_p3.cpp): binoc.h.

#include "binoc.h"
#include "worldglobals.h"
#include "photo.h"

#include "actor.h"
#include "ai.h"
#include "alo.h"
#include "asega.h"
#include "brx.h"
#include "activate.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "cplcy.h"
#include "lo.h"
#include "po.h"
#include "screenrt.h"
#include "so.h"
#include "sound.h"
#include "sly.h"
#include "smrun.h"
#include "sm.h"
#include "animfind.h"
#include "stepphys.h"
#include "sw.h"
#include "task.h"
#include "skipcut.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <set>
#include <string>

void PlayActorScriptAnimLo(LO* ploActor, LO* plo, int oid, int grfapl);  // scpt.cpp (0x143EE0)

namespace
{
void WarnOnce(const std::string& what)
{
    static std::set<std::string> s_set;
    if (s_set.insert(what).second) std::fprintf(stderr, "binoc: %s not ported\n", what.c_str());
}

ASEG* PasegPart(ALO* palo, uint32_t ib)
{
    for (const auto& [ibPart, plo] : palo->apart)
        if (ibPart == ib) return dynamic_cast<ASEG*>(plo);
    return nullptr;
}

int s_nViewMode = -1;  // (0x2BF920: the window's last target kind)

// orig: 0x001442D0 / 0x00144338 (vt slots 83 / 84 of the actors: the PO's 0x205478 / 0x205588, then with f the other
// speaker's script animation 0x60B (0x143EE0 (9, kind 5) at the first hold, its binocucom off; 0x143FF8 at the last))
// The PO's part: each of the six effect flags (+0x2A0 counts) holds the effect SM D_002C3C40[i] (1548 / 1549 / 1550 /
// - / 1551 / 1552; 0x15E938 kind 4 from the object) running on it (0x1F03C8, else 0x1F0198(-1, 9)); its last release
// retracts it (0x1F0598).
}  // namespace

namespace
{
std::map<const LO*, std::array<int, 7>>& MpcHold()
{
    static std::map<const LO*, std::array<int, 7>> s_mpc;
    WORLD_TABLE_LOCAL(s_mpc);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mpc;
}
}  // namespace
// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetActorEffectHolds() { MpcHold().clear(); }

// the actor's talk hold count (+0x12F4)
int CActorTalkHold(const LO* plo)
{
    auto it = MpcHold().find(plo);
    return it == MpcHold().end() ? 0 : it->second[6];
}

void HoldActorEffects(LO* plo, bool fHold, bool f, uint32_t grf)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) return;
    ALO* palo = static_cast<ALO*>(plo);
    auto& ac = MpcHold()[plo];  // (+0x2A0 x 6, the actor's +0x12F4)
    static const int s_aidEffect[6] = {1548, 1549, 1550, -1, 1551, 1552};  // D_002C3C40 (0x1FD0C0)
    for (int i = 0; i < 6; i++)
    {
        const int id = s_aidEffect[i];
        if (!(grf & (1u << i))) continue;
        if (fHold)
        {
            if (++ac[i] >= 2) continue;
        }
        else
        {
            if (ac[i] == 0 || --ac[i] > 0) continue;
        }
        LO* px = id == -1 ? nullptr : PloFindSwNearest(plo, id);
        SM* psm = px ? dynamic_cast<SM*>(px) : nullptr;
        if (std::getenv("SLY2N_BINOCPRINT"))
            std::printf("binoc: effect %d (%d) on %d: %s %s\n", i, id, plo->oid, px ? (psm ? "SM" : "not an SM") : "none", fHold ? "held" : "released");
        if (!psm) continue;
        SMA* psma = PsmaFindSm(psm, palo);  // 0x1F03C8
        if (fHold)
        {
            if (!psma) PsmaApplySm(psm, palo, -1, 9);  // 0x1F0198
        }
        else if (psma)
            RetractSma(psma);  // 0x1F0598
    }
    if (!f || !FCidDerivedFrom(plo->Cid(), 0x34)) return;  // (the PO's 0x205478 / 0x205588 have no f)
    if (fHold)
    {
        if (++ac[6] == 1)
        {
            PlayActorScriptAnimLo(plo, PloFindSwNearest(plo, 0x60B), 0x60B, 9);  // 0x143EE0(p, 0x60B, 9, 5)
            if (ACTOR* pa = PactorFromLo(plo)) SetBinocState(pa, -1);         // 0x19CD38(+0xF00, -1)
        }
    }
    else if (--ac[6] == 0)
        PlayActorScriptAnimLo(plo, nullptr, -1, 0);  // 0x143FF8
}

namespace
{

// vt slot 87 (Sly 0x189530: the base 0x205D10 and his parts +0x19A0 / +0x19A4 / +0x19A8 hidden (slot 21) in the
// recon view; Bentley 0x205D10, Murray 0x1B81B8)
void ShowActorParts(ACTOR* pa, bool fHidden)
{
    if (FIsLoDerivedFrom(pa->palo, 0x35))
    {
        // Sly 0x189530: the base's (0x205D10: nothing), then his three parts' slot 21 (0x1F3B28: shown unless hidden)
        for (uint32_t ib : {0x19A0u, 0x19A4u, 0x19A8u})
            for (const auto& [ibPart, plo] : pa->palo->apart)
                if (ibPart == ib && plo) SetLoVisible(g_psw, plo, !fHidden);
        return;
    }
    if (FIsLoDerivedFrom(pa->palo, 0x36)) WarnOnce("vt slot 87 of Murray (0x1B81B8)");
    // (Bentley's 0x205D10: nothing)
}

// vt slot 94 (Sly 0x1894F8: the states 0 / 1 / 5 / 10 / 12 / 24 play the opening / closing)
bool FBinocAnimAllowed(ACTOR* pa)
{
    if (SLY* p = dynamic_cast<SLY*>(pa))
        switch (p->state)
        {
        case 0: case 1: case 5: case 10: case 12: case 24: return true;
        default: return false;
        }
    WarnOnce("vt slot 94 of Bentley / Murray (0x135FF0 / 0x1B8180)");
    return true;
}

// 0x1FEA60 on the channels 0 and 1 (kind 3) and the camera cut
void DuckSounds(float g)
{
    CutCm(&g_cm);  // 0x14D8C0
    SetSoundChannelVolume(0, 3, g);
    SetSoundChannelVolume(1, 3, g);
}
}  // namespace

// orig: 0x0019CBF8 / 0x0019D4D0
void InitBinoc(ACTOR* pa)
{
    pa->nF04 = -1;
    pa->nF20 = -1;
    pa->binoc = BINOC{};
    pa->binoc.win.state = -1;
    // The photo helper is allocated at post-load, after its authored targets are available.
}

// orig: 0x0019CC30 -> 0x0019D540
void PostLoadBinoc(ACTOR* pa)
{
    BINOCWIN& w = pa->binoc.win;
    int oidFrame = -1, oidWindow = -1;
    if (FIsLoDerivedFrom(pa->palo, 0x35)) { oidFrame = 0x35F; oidWindow = 0x368; }
    else if (FIsLoDerivedFrom(pa->palo, 0x37)) { oidFrame = 0x360; oidWindow = 0x369; }
    else if (FIsLoDerivedFrom(pa->palo, 0x36)) { oidFrame = 0x361; oidWindow = 0x36A; }
    if (oidFrame != -1 && g_psw)
    {
        w.ploFrame = PloFindSwObjectByOid(g_psw, oidFrame);    // 0x15E938(kind 0x405)
        w.ploWindowDefault = PloFindSwObjectByOid(g_psw, oidWindow);
    }
    w.ploWindow = w.ploWindowDefault;  // 0x19DCA0
    pa->binoc.pasegOpen = PasegPart(pa->palo, 0xF0C);
    pa->binoc.pasegClose = PasegPart(pa->palo, 0xF10);
    w.pprompt = PostLoadPhotoPrompt(pa);  // orig0x201DC8 (Sly's photo helper)
}

// orig: 0x0019CC50
void OpenBinoc(ACTOR* pa, int nView, LO* ploSpeaker, LO* ploScreen)
{
    if (pa->nF04 != -1)
    {
        if (pa->nF20 == 1 || nView != 1) return;
        SetBinocState(pa, 2);
        SetBinocState(pa, -1);
    }
    pa->nF20 = nView;
    pa->binoc.ploSpeaker = ploSpeaker;
    pa->binoc.ploScreen = ploScreen;
    SetBinocState(pa, 0);
    if (nView == 0)
        if (PODATA* data = PpodataFromLo(pa->palo)) sly2actions::BeginRecon(data->ppadctl);
}

// orig: 0x0019CCE8 (the opening, shutter, acceptance and closing keep the viewer).
void CloseBinoc(ACTOR* pa)
{
    if (pa->binoc.win.pprompt)
        switch (NPhotoPromptState(pa->binoc.win.pprompt))
        {
        case 0: case 8: case 9: case 10: return;
        }
    SetBinocState(pa, 2);
}

// orig: 0x0019CD38
void SetBinocState(ACTOR* pa, int n)
{
    BINOC& b = pa->binoc;
    const int cur = pa->nF04;
    if (n == cur) return;
    ALO* palo = pa->palo;
    if (cur == -1)
    {
        // a talk pauses the game (0x20F198 / 0x20F0C0); the actor's animation is held
        if (pa->nF20 == 1)
        {
            b.idSkipScene = NewSkipCutScene();
            ++g_gamepause.c84;  // 0x20F198
            PauseTalk();        // 0x20F0C0 (the pad locked: 0x187078(4))
        }
        ASEGA* ph = pa->pasega ? pa->pasega : (pa->psma ? pa->psma->pasega : nullptr);
        b.pasegaHeld = ph;
        if (ph)
        {
            AddRefAsega(ph);
            SetAsegaSpeed(ph, 0.0f);  // 0x131A00
        }
    }
    else if (cur == 1)
    {
        switch (pa->nF20)
        {
        case 0: RevokeCmPolicy(&g_cm, 0xF, 7, &g_cmpolicies.cprecon, palo, nullptr); break;  // 0x14E4C8
        case 1:
            RevokeCmPolicy(&g_cm, 0xF, 8, &g_cmpolicies.cpfree, palo, nullptr);
            if (b.ploSpeaker)
            {
                HoldActorEffects(b.ploSpeaker, false, true);  // slot 84 (0x3F, 1)
                SetAloCnstrLin(static_cast<ALO*>(b.ploSpeaker), 0);  // 0x1F5720(0)
            }
            HoldActorEffects(palo, false, false);  // slot 84 (0x3F, 0)
            break;
        case 2: RevokeCmPolicy(&g_cm, 0xF, 7, &g_cmpolicies.cprecon, b.ploSpeaker, nullptr); break;
        default: break;
        }
        DuckSounds(1.0f);
        if (pa->nF20 != 2) ShowActorParts(pa, false);  // slot 87 (0)
    }
    pa->nF04 = n;
    if (n == -1) b.idSkipScene = 0;
    static const bool s_fPrint = std::getenv("SLY2N_BINOCPRINT") != nullptr;
    if (s_fPrint)
        std::printf("binoc %d: t %.2f state %d -> %d view %d (free pan %.3f tilt %.3f, recon pan %.3f) speaker %d screen %d window %d frame %d\n", palo->oid, g_clock.t, cur, n,
                    g_cmpolicies.cpfree.radPan, g_cmpolicies.cpfree.radTilt, g_cmpolicies.cprecon.radPan,
                    pa->nF20, b.ploSpeaker ? b.ploSpeaker->oid : -1, b.ploScreen ? b.ploScreen->oid : -1,
                    b.win.ploWindow ? b.win.ploWindow->oid : -1, b.win.ploFrame ? b.win.ploFrame->oid : -1);
    ASEG* paseg = nullptr;
    b.tState = g_clock.t;
    switch (n)
    {
    case 0: paseg = b.pasegOpen; break;
    case 1:
        switch (pa->nF20)
        {
        case 0:
            SetCmPolicy(&g_cm, 7, &g_cmpolicies.cprecon, palo, nullptr);  // 0x14E328
            SetStepPhysHeading(palo, g_cmpolicies.cprecon.radPan);       // (CM +0x460)
            SetBinocWindowState(pa, 0);
            break;
        case 1:
            SetCmPolicy(&g_cm, 8, &g_cmpolicies.cpfree, palo, nullptr);
            SetStepPhysHeading(palo, g_cmpolicies.cpfree.radPan);         // (CM +0x5C4)
            if (b.ploSpeaker && FIsLoDerivedFrom(b.ploSpeaker, 0x30))
            {
                ALO* ps = static_cast<ALO*>(b.ploSpeaker);
                HoldActorEffects(ps, true, true);  // slot 83 (0x3F, 1)
                SetAloCnstrLin(ps, 1);             // 0x1F5720(1)
                if (ACTOR* pq = PactorFromLo(ps)) pq->vecVel = glm::vec3(0.0f);
                SetAloVelocityTo(ps, glm::vec3(0.0f));     // slots 35 / 36
                SetAloAngVelocityTo(ps, glm::vec3(0.0f));
            }
            b.win.ploWindow = b.ploScreen ? b.ploScreen : b.win.ploWindowDefault;  // 0x19DCA0
            SetBinocWindowState(pa, 0);
            HoldActorEffects(palo, true, false);  // slot 83 (0x3F, 0)
            break;
        case 2:
            SetCmPolicy(&g_cm, 7, &g_cmpolicies.cprecon, b.ploSpeaker, nullptr);
            SetBinocWindowState(pa, 0);
            break;
        default: break;
        }
        pa->vecVel = glm::vec3(0.0f);
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        DuckSounds(0.6f);
        if (pa->nF20 != 2) ShowActorParts(pa, true);  // slot 87 (1)
        break;
    case 2:
        SetBinocWindowState(pa, 2);
        paseg = b.pasegClose;
        break;
    case -1:
        if (pa->nF20 == 1)
        {
            SetBinocWindowState(pa, -1);
            --g_gamepause.c84;  // 0x20F1A8
            ResumeTalk();       // 0x20F0F0
        }
        else if (pa->nF20 == 0 || pa->nF20 == 2) SetBinocWindowState(pa, -1);
        if (b.pasegaHeld)
        {
            SetAsegaSpeed(b.pasegaHeld, 1.0f);
            ReleaseAsega(b.pasegaHeld);
            b.pasegaHeld = nullptr;
        }
        pa->nF20 = -1;
        break;
    default: break;
    }
    if (paseg && !FBinocAnimAllowed(pa)) paseg = nullptr;
    ApplyAsegCur(paseg, palo, 0.0f, 1.0f, 1, &b.pasega);  // 0x1305B8
}

// orig: 0x0019D330
void UpdateBinoc(ACTOR* pa)
{
    BINOC& b = pa->binoc;
    int n;
    do
    {
        n = pa->nF04;
        switch (n)
        {
        case 0: case 2:
            pa->vecVel = glm::vec3(0.0f);
            if (!b.pasega || FAsegaRetracted(b.pasega)) SetBinocState(pa, n == 0 ? 1 : -1);  // 0x131968
            break;
        case 1:
            pa->vecVel = glm::vec3(0.0f);
            if (pa->nF20 == 0)
            {
                SetStepPhysHeading(pa->palo, g_cmpolicies.cprecon.radPan);
                b.g3C = g_cmpolicies.cprecon.uZoom;  // (CM +0x470)
                UpdateBinocWindow(pa);
            }
            else if (pa->nF20 == 1)
            {
                SetStepPhysHeading(pa->palo, g_cmpolicies.cpfree.radPan);
                b.g3C = g_cmpolicies.cpfree.uZoom;  // (CM +0x5CC)
                UpdateBinocWindow(pa);
            }
            break;
        default: break;
        }
    } while (n != pa->nF04);
    // in the recon view the controller's sticks are cleared while it is current
    if (pa->nF20 == 0)
        if (PODATA* ppd = PpodataFromLo(pa->palo); ppd && ppd->ppadctl && ppd->ppadctl->nControl == 1)
        {
            PADCTL& pc = *ppd->ppadctl;
            pc.x = pc.y = pc.radJoy = pc.uJoy = 0.0f;
            pc.x2 = pc.y2 = pc.radJoy2 = pc.uJoy2 = 0.0f;
        }
}

std::vector<TASK*> AptaskInOrder(SW* psw);  // task.cpp (0x165C98)
bool FTaskDoneExt(TASK* p);                  // task.cpp (0x164030)

// orig: 0x0019D948: the frame's two string arguments: the current job's name (+0x6C) and the goal (+0x5C) of its
// first task not done (the tasks in 0x165C98's order sorted by 0x165C28), shown by its text parts (0x2E5 / 0x2E6)
void SetBinocWindowArgs(BINOCWIN& w)
{
    LO* pt = w.ploFrame;
    if (!pt) return;
    SetScreenObjArg(pt, 0, -1);
    SetScreenObjArg(pt, 1, -1);
    JOB* pj = g_psw ? g_psw->tasks.pjobCur : nullptr;
    if (!pj || pj->n6c == -1) return;
    SetScreenObjArg(pt, 0, pj->n6c);
    std::vector<TASK*> a = AptaskInOrder(g_psw);
    std::sort(a.begin(), a.end(), [](const TASK* x, const TASK* y) {  // (qsort 0x2338C0 with 0x165C28)
        if (x->nJobDepth != y->nJobDepth) return x->nJobDepth < y->nJobDepth;
        const int n1 = x->pjob ? x->pjob->oid : -1, n2 = y->pjob ? y->pjob->oid : -1;
        if (n1 != n2) return n1 < n2;
        if (x->nDepth != y->nDepth) return x->nDepth < y->nDepth;
        return x->oid < y->oid;
    });
    for (TASK* x : a)
        if (x->pjob == pj && x->idDialog5C != -1 && !FTaskDoneExt(x))
        {
            SetScreenObjArg(pt, 1, x->idDialog5C);
            break;
        }
}

// orig: 0x0019D660
void SetBinocWindowState(ACTOR* pa, int n)
{
    BINOCWIN& w = pa->binoc.win;
    if (w.state == n) return;
    if (w.state == -1) SetBinocWindowArgs(w);  // 0x19D948
    const int old = w.state;
    w.state = n;
    const bool fTalk = pa->nF20 == 1;
    switch (n)
    {
    case 0:
        BroadcastTaskEvent(g_psw, fTalk ? 0xC : 7, nullptr);  // 0x1619A0 (the HUD)
        if (w.ploFrame)
        {
            ShowScreenObj(w.ploFrame, -1);       // vt slot 49
            SelectScreenObjPart(w.ploFrame, 0x352);  // 0x213B00
            SelectScreenObjPart(w.ploFrame, 0x351);
            SetScreenObjFraction(w.ploFrame, 0x352, (pa->radHeading + 3.1415927f) * 0.15915494f);  // 0x212E40
            SetScreenObjFraction(w.ploFrame, 0x351, pa->binoc.g3C);
        }
        if (fTalk)
        {
            ResetDialogSubtitle();  // 0x227788
            if (w.ploWindow) ShowScreenObj(w.ploWindow, -1);
        }
        SetPhotoPromptState(w.pprompt, 0);
        break;
    case 2:
        BroadcastTaskEvent(g_psw, fTalk ? 0xD : 8, nullptr);
        if (w.ploFrame) HideScreenObj(w.ploFrame);  // vt slot 50
        if (w.ploWindow) HideScreenObj(w.ploWindow);
        SetPhotoPromptState(w.pprompt, 10);
        break;
    case -1:
        if (old != 2) BroadcastTaskEvent(g_psw, fTalk ? 0xD : 8, nullptr);
        for (LO* plo : {w.ploFrame, w.ploWindow})
            if (plo && NScreenObjState(plo))
            {
                SetScreenObjState(plo, 1);   // 0x212A70
                SetScreenObjTimers(plo, 1);  // 0x212A50
            }
        SetPhotoPromptState(w.pprompt, -1);
        break;
    default: break;
    }
}

// orig: 0x0019DA78
void UpdateBinocWindow(ACTOR* pa)
{
    BINOCWIN& w = pa->binoc.win;
    UpdatePhotoPrompt(w.pprompt);
    const bool fTalk = pa->nF20 == 1;
    if (w.state == 0)
    {
        const bool fFrame = !w.ploFrame || NScreenObjState(w.ploFrame) == 2;
        s_nViewMode = -1;
        const bool fWindow = !fTalk || !w.ploWindow || NScreenObjState(w.ploWindow) == 2;
        if (fFrame && fWindow) SetBinocWindowState(pa, 1);
    }
    else if (w.state == 2)
    {
        const bool fFrame = !w.ploFrame || NScreenObjState(w.ploFrame) < 2;
        const bool fWindow = !fTalk || !w.ploWindow || NScreenObjState(w.ploWindow) < 2;
        if (fFrame && fWindow) SetBinocWindowState(pa, -1);
        return;
    }
    else if (w.state == -1) return;
    if (!w.ploFrame) return;
    if (w.state != 0)
    {
        // the target marker (0x353) while the best target is of kind 4
        static const bool s_fForce353 = std::getenv("SLY2N_FORCE353") != nullptr;   // (debug: the target marker always)
        if (pa->n10EC == 4 || s_fForce353)
        {
            if (s_nViewMode != 4) SelectScreenObjPart(w.ploFrame, 0x353);
            s_nViewMode = pa->n10E4;
        }
        else
        {
            if (s_nViewMode == 4) ResetScreenObjPart(w.ploFrame, 0x353);
            s_nViewMode = pa->n10E4;
        }
    }
    SetScreenObjFraction(w.ploFrame, 0x352, (pa->radHeading + 3.1415927f) * 0.15915494f);
    SetScreenObjFraction(w.ploFrame, 0x351, pa->binoc.g3C);
}

// orig: 0x0018B2C0 / 0x00134D30 / 0x001B5038 (the players': while it is busy, a press of mask in the recon view closes
// it, then its update)
void UpdateActorBinoc(ACTOR* pa, uint32_t grfMask)
{
    if (!(static_cast<unsigned>(pa->nF04) < 3)) return;  // 0x1443D8
    if (pa->nF04 == 1 && pa->nF20 == 0)
        if (PODATA* ppd = PpodataFromLo(pa->palo); ppd && sly2actions::Take(ppd->ppadctl, sly2actions::Action::ReconClose,
            NPhotoPromptState(pa->binoc.win.pprompt) == 7 ? 0x410 : grfMask, 0.25f))
            CloseBinoc(pa);
    UpdateBinoc(pa);
}
