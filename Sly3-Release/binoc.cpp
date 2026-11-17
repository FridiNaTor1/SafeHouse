#include "actioninput.h"
// The binocucom (orig ACTOR +0xF00, module 0x19CBF8..0x19DE10; the reference's m_19D330_p3.cpp): binoc.h.

#include "binoc.h"
#include "gang.h"
#include <execinfo.h>
#include "binoc3.h"
#include "game/scptgame3.h"
#include "game/sly3.h"
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
#include "gang.h"
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
std::map<const LO*, std::array<int, 8>>& MpcHold()
{
    static std::map<const LO*, std::array<int, 8>> s_mpc;
    WORLD_TABLE_LOCAL(s_mpc);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mpc;
}
}  // namespace
// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetBinocHidden3();
void ResetActorEffectHolds() { MpcHold().clear(); ResetBinocHidden3(); }

// the actor's talk hold count (+0x12F4)
int CActorTalkHold(const LO* plo)
{
    auto it = MpcHold().find(plo);
    return it == MpcHold().end() ? 0 : it->second[7];
}

// orig3 0x156900 / 0x156928 (Sly 2 0x143EE0(p, 0x60B, 9, 5) / 0x143FF8): the actor's talk animation 1756 (9, kind 5),
// played by a binocucom talk's speaker (the hold below) and by the player in the window (0x1BDBB0 kind 1)
void PlayActorTalkAnim(LO* plo, bool fOn)
{
    if (std::getenv("SLY2N_BINOCPRINT"))
    {
        LO* p = plo ? PloFindSwNearest(plo, 1756) : nullptr;
        std::printf("binoc: talk anim %s on %d: 1756 -> %d cid 0x%X parent %d\n", fOn ? "on" : "off", plo ? plo->oid : -1, p ? p->oid : -1, p ? p->Cid() : 0,
                    p && p->paloParent ? p->paloParent->oid : -1);
    }
    if (fOn) PlayActorScriptAnimLo(plo, PloFindSwNearest(plo, 1756), 1756, 9);
    else PlayActorScriptAnimLo(plo, nullptr, -1, 0);
}

// orig3 0x156850 / 0x1568B0 (vt slots 94 / 95 of the actors; the PO's 0x24F7C8 / 0x24F8C0)
void HoldActorEffects(LO* plo, bool fHold, bool f, uint32_t grf)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), S3CID(0x30))) return;
    ALO* palo = static_cast<ALO*>(plo);
    auto& ac = MpcHold()[plo];  // (Sly 3: +0x2C8 x 7, the actor's +0x13EC)
    // Sly 3's ids (orig3 0x24F7C8 / 0x24F8C0: 0x246F30's table D_00349850, seven flags; Sly 2's D_002C3C40 held
    // 1548 / 1549 / 1550 / - / 1551 / 1552, which name other objects in Sly 3)
    static const int s_aidEffect[7] = {1757, 1758, 1759, -1, 1760, 1761, -1};
    for (int i = 0; i < 7; i++)
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
    if (!f || !FCidDerivedFrom(plo->Cid(), S3CID(0x34))) return;  // (the PO's 0x205478 / 0x205588 have no f)
    if (fHold)
    {
        if (++ac[7] == 1)
        {
            PlayActorTalkAnim(plo, true);                                // orig3 0x156900
            if (ACTOR* pa = PactorFromLo(plo)) SetBinocState(pa, -1);    // 0x1BDBB0(+0xF60, -1)
        }
    }
    else if (ac[7] > 0 && --ac[7] == 0)
        PlayActorTalkAnim(plo, false);  // orig3 0x156928
}

namespace
{

// vt slot 87 (Sly 0x189530: the base 0x205D10 and his parts +0x19A0 / +0x19A4 / +0x19A8 hidden (slot 21) in the
// recon view; Bentley 0x205D10, Murray 0x1B81B8)
void ShowActorParts(ACTOR* pa, bool fHidden)
{
    if (FIsLoDerivedFrom(pa->palo, S3CID(0x35)))
    {
        // Sly 0x189530: the base's (0x205D10: nothing), then his three parts' slot 21 (0x1F3B28: shown unless hidden)
        for (uint32_t ib : {0x19A0u, 0x19A4u, 0x19A8u})
            for (const auto& [ibPart, plo] : pa->palo->apart)
                if (ibPart == ib && plo) SetLoVisible(g_psw, plo, !fHidden);
        return;
    }
    if (FIsLoDerivedFrom(pa->palo, S3CID(0x36))) WarnOnce("vt slot 87 of Murray (0x1B81B8)");
    // (Bentley's 0x205D10: nothing)
}

// vt slot 94 (Sly 0x1894F8: the states 0 / 1 / 5 / 10 / 12 / 24 play the opening / closing). Sly 3's vt slot 104 (the
// state numbers Sly 3's): Sly 0x1AACA0 {0, 1, 2, 6, 10, 19} (Sly's states are Sly 3's: sly.h jts::), Bentley 0x13F408
// {0, 1, 5, 6, 7, 9}, Murray 0x1E0B10 {0, 1, 10, 11} (their native states keep Sly 3's numbers)
bool FBinocAnimAllowed(ACTOR* pa)
{
    if (SLY* p = dynamic_cast<SLY*>(pa))
        switch (p->jts)
        {
        case 0: case 1: case 2: case 6: case 10: case 19: return true;
        default: return false;
        }
    if (BENTLEY* p = dynamic_cast<BENTLEY*>(pa))
        switch (p->state)
        {
        case 0: case 1: case 5: case 6: case 7: case 9: return true;
        default: return false;
        }
    if (MURRAY* p = dynamic_cast<MURRAY*>(pa))
        switch (p->state)
        {
        case 0: case 1: case 10: case 11: return true;
        default: return false;
        }
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

// (binoccam3.cpp, the scripts' binocucom (orig3 0x1BDBB0 kind 1): the same actor side as the recon view's)
bool FBinocAnimAllowed3(ACTOR* pa) { return pa && FBinocAnimAllowed(pa); }
void DuckBinocSounds3(float g) { DuckSounds(g); }
int NActorState3(ACTOR* pa)
{
    if (SLY* p = dynamic_cast<SLY*>(pa)) return p->jts;   // (Sly 3's numbers: sly.h jts::)
    if (BENTLEY* p = dynamic_cast<BENTLEY*>(pa)) return p->state;
    if (MURRAY* p = dynamic_cast<MURRAY*>(pa)) return p->state;
    return -1;
}
void ShowBinocActorParts3(ACTOR* pa, bool fHidden) { if (pa) ShowActorParts(pa, fHidden); }

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
    if (FIsLoDerivedFrom(pa->palo, S3CID(0x35))) { oidFrame = 0x35F; oidWindow = 0x368; }
    else if (FIsLoDerivedFrom(pa->palo, S3CID(0x37))) { oidFrame = 0x360; oidWindow = 0x369; }
    else if (FIsLoDerivedFrom(pa->palo, S3CID(0x36))) { oidFrame = 0x361; oidWindow = 0x36A; }
    // (Sly 3: the binocucom's screen objects are binoc3.cpp's (the frame 1038, the speaker's window 1045); Sly 2's oids
    // 0x35F..0x36A are none of them, and 874 (0x36A) is another global screen object of every Sly 3 world, which a
    // talk of this window would have shown. Not looked up: this window keeps no screen object in Sly 3)
    static const bool s_fSly2Oids = std::getenv("SLY3_BINOC2OIDS") != nullptr;   // (debug: as before)
    if (!s_fSly2Oids) oidFrame = oidWindow = -1;
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


// orig3 0x241FC0 / 0x242228 (Sly 3's 0x1BDBB0 state 1 of a view other than kind 2: the actor's model hidden while the
// binocucom looks out from behind his head, shown again when it leaves 1): 0x242048 walks the tree: each visible ALO
// (class 0x2F) hidden (vt slot 23 with 0), its children ALOs alike; the hidden ones recorded and restored. (Its effects
// 0xAB faded to 0 and lights 0xC3 dimmed: not done natively.) Natively Bentley's cap and chair filled his view and his
// own dart hit them. SLY3_BINOCSHOWACTOR=1: not hidden.
namespace
{
std::map<LO*, std::vector<LO*>>& MpHidden()
{
    static std::map<LO*, std::vector<LO*>> s_mp;
    WORLD_TABLE_LOCAL(s_mp);
    return s_mp;
}
void HideTree(LO* plo, std::vector<LO*>& a)
{
    if (!plo || !FCidDerivedFrom(plo->Cid(), sly3::CID_ALO)) return;
    if (GrfUpdLo(g_psw, plo) & 8u)
    {
        SetLoVisible(g_psw, plo, false);
        a.push_back(plo);
    }
    for (LO* pc : std::vector<LO*>(static_cast<ALO*>(plo)->aploChild)) HideTree(pc, a);
}
}  // namespace
void ResetBinocHidden3() { MpHidden().clear(); }
void HideActorForBinoc3(ACTOR* pa, bool fHide)
{
    static const bool s_fOff = std::getenv("SLY3_BINOCSHOWACTOR") != nullptr;
    if (s_fOff || !pa || !pa->palo || !g_psw) return;
    auto& mp = MpHidden();
    auto it = mp.find(pa->palo);
    if (fHide)
    {
        if (it != mp.end()) return;   // (+0x128 & 0x40: hidden already)
        HideTree(pa->palo, mp[pa->palo]);
    }
    else if (it != mp.end())
    {
        for (LO* plo : it->second) SetLoVisible(g_psw, plo, true);
        mp.erase(it);
    }
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
    // (Sly 3: one binocucom per actor (+0xF60): its -1 also shuts the scripts' one, binoccam3.cpp)
    if (n == -1 && FScripts3Active() && pa) ShutBinocMode3(pa->palo);
    BINOC& b = pa->binoc;
    const int cur = pa->nF04;
    if (n == cur) return;
    // (debug: SLY2N_BINOCSTACK=1: who changes a binocucom's state)
    if (static const bool s_fStack = std::getenv("SLY2N_BINOCSTACK") != nullptr; s_fStack)
    {
        void* apv[16];
        std::fprintf(stderr, "binoc %d state %d -> %d t %.3f:\n", pa->palo ? pa->palo->oid : -1, cur, n, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 16), 2);
    }
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
            if (FScripts3Active()) PlayActorTalkAnim(palo, false);   // orig3 0x1BDBB0 state 1's exit: 0x156928
            break;
        case 2: RevokeCmPolicy(&g_cm, 0xF, 7, &g_cmpolicies.cprecon, b.ploSpeaker, nullptr); break;
        default: break;
        }
        DuckSounds(1.0f);
        if (pa->nF20 != 2) ShowActorParts(pa, false);  // slot 87 (0)
        if (pa->nF20 != 2 && FScripts3Active()) HideActorForBinoc3(pa, false);   // orig3 0x242228
    }
    pa->nF04 = n;
    if (n == -1) b.idSkipScene = 0;
    // (s3-player round 21, the user's report: the binocucom in play showed nothing) Sly 3's OpenBinoc 0x1BDA88 of kind 0
    // (the recon view: R3 in play) opens the panel (the frame 1038 with its compass / zoom dials, binoc3.cpp) as a talk's
    // does; the copied Sly 2 window found none of its Sly 2 screen objects, so only the camera moved
    if (FScripts3Active() && pa->nF20 == 0)
    {
        if (n == 1) OpenBinocPanel3(palo, 0, nullptr);
        else if (n == 2 || n == -1) CloseBinocPanel3(palo);
    }
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
            if (b.ploSpeaker && FIsLoDerivedFrom(b.ploSpeaker, S3CID(0x30)))
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
            if (FScripts3Active()) PlayActorTalkAnim(palo, true);    // orig3 0x1BDBB0 kind 1: 0x156900 (the player's talk pose)
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
        if (pa->nF20 != 2 && FScripts3Active()) HideActorForBinoc3(pa, true);    // orig3 0x241FC0
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
    // (Sly 3: the recon view's panel is binoc3.cpp's (OpenBinocPanel3 above), which sends Sly 3's broadcasts 2 / 3 and
    // drives the photo helper itself (orig3 0x1BE5A8); this Sly 2 window then only keeps its own screen objects, none
    // of which a Sly 3 world has)
    void* pprompt = FScripts3Active() && pa->nF20 == 0 ? nullptr : w.pprompt;
    const bool fBroadcast = !(FScripts3Active() && pa->nF20 == 0);
    switch (n)
    {
    case 0:
        if (fBroadcast) BroadcastTaskEvent(g_psw, fTalk ? 0xC : 7, nullptr);  // 0x1619A0 (the HUD)
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
        SetPhotoPromptState(pprompt, 0);
        break;
    case 2:
        if (fBroadcast) BroadcastTaskEvent(g_psw, fTalk ? 0xD : 8, nullptr);
        if (w.ploFrame) HideScreenObj(w.ploFrame);  // vt slot 50
        if (w.ploWindow) HideScreenObj(w.ploWindow);
        SetPhotoPromptState(pprompt, 10);
        break;
    case -1:
        if (old != 2 && fBroadcast) BroadcastTaskEvent(g_psw, fTalk ? 0xD : 8, nullptr);
        for (LO* plo : {w.ploFrame, w.ploWindow})
            if (plo && NScreenObjState(plo))
            {
                SetScreenObjState(plo, 1);   // 0x212A70
                SetScreenObjTimers(plo, 1);  // 0x212A50
            }
        SetPhotoPromptState(pprompt, -1);
        break;
    default: break;
    }
}

// orig: 0x0019DA78
void UpdateBinocWindow(ACTOR* pa)
{
    BINOCWIN& w = pa->binoc.win;
    if (!FBinocPanel3Prompt(w.pprompt)) UpdatePhotoPrompt(w.pprompt);   // (else binoc3.cpp updates it)
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
