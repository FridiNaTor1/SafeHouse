// The ASEG events the world side handles (asega.h g_pfnAsegaEventOther; orig 0x00131290's kinds): 7 / 8 an
// object added / removed (0x210338: by its oid around the animation's object, faded over ag[0] when an ALO,
// 0x1F7C68 / 0x1F7BF8; reversed while the animation plays backwards).

#include "activate.h"
#include "worldglobals.h"
#include "alo.h"
#include "rumble.h"
#include "animfind.h"
#include "aseg.h"
#include "asega.h"
#include "clock.h"
#include "shd.h"
#include "sm.h"
#include "brx.h"
#include "cls.h"
#include "cm.h"
#include "emitter.h"
#include "objmethod.h"
#include "sw.h"
#include "sound.h"
#include "guard.h"
#include "xf.h"
#include "pccharacter.h"   // (PC addition, debug menu only: Carmelita's moveset)
#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

// orig 0x001DFD58 (a SCPT object's start; set by the SCPT side)
constinit void (*g_pfnStartScpt)(LO* plo, int n) = nullptr;

namespace
{
LO* PloAround(LO* ploOwner, OID oid)
{
    // orig 0x210338: PloFindSwObject(4, oid, owner), including the owner itself (0x15E3E8).
    // Pooled objects share an OID: a death event must fade its own instance, not the first global match.
    return PloFindSwNearest(ploOwner, oid);
}

// orig 0x001F7D90 / 0x001F7D10: the fade's speed and value (0: removed, else added back)
void SetFade(ALO* palo, float gValue, bool fValue, float dg)
{
    palo->dgFade = dg;
    if (!fValue) return;
    palo->gFade = gValue;
    if (gValue <= 0.0f) RemoveLo(g_psw, palo);
    else AddLo(g_psw, palo);
}

// orig 0x00210338
void AddRemoveByOid(bool fOn, OID oid, LO* ploOwner, float g)
{
    LO* plo = PloAround(ploOwner, oid);
    if (std::getenv("SLY2N_ADDREMDBG"))
        std::printf("addrem t %.3f on %d oid %d owner %d -> %p cid 0x%X parent %d\n", g_clock.t, int(fOn), oid, ploOwner ? ploOwner->oid : -1, (void*)plo,
                    plo ? plo->Cid() : 0, plo && plo->paloParent ? plo->paloParent->oid : -1);
    SwitchLoFaded(plo, fOn, g);
}
}  // namespace

// orig 0x001F7D90: the fade's rate (+0x130); a nonzero one also puts the object in the world (vt slot 8) and turns
// its update on (+0x40 | 2: UpdateAlo runs the fade, which removes it at 0)
void SetFadeRate(ALO* palo, float dg)
{
    if (dg != 0.0f)
    {
        AddLo(g_psw, palo);
        if (palo->iupd >= 0) g_psw->aupd[static_cast<size_t>(palo->iupd)].grf |= 2;
        else palo->grfUpd |= 2;
    }
    palo->dgFade = dg;
}

// orig: 0x001F7BF8 (the authored fade-out duration, also exposed by splice method 127)
void FadeAloOut(ALO* palo, float dt)
{
    if (dt <= 0.0f) SetFade(palo, 0.0f, true, 0.0f);
    else SetFadeRate(palo, -1.0f / dt);
}

// orig: 0x001F7C68 (fade in; a removed object starts at zero after AddLo resets it)
void FadeAloIn(ALO* palo, float dt)
{
    if (dt <= 0.0f) SetFade(palo, 1.0f, true, 0.0f);
    else
    {
        if (palo->fRemoved)
        {
            AddLo(g_psw, palo);
            palo->gFade = 0.0f;
        }
        SetFadeRate(palo, 1.0f / dt);
    }
}

// orig 0x00210338's part after the find (also the SCPT commands 0x2C / 0x2D)
void SwitchLoFaded(LO* plo, bool fOn, float g)
{
    if (!plo) return;
    if (g != 0.0f && FCidDerivedFrom(plo->Cid(), 0x30))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (fOn) FadeAloIn(palo, g);
        else FadeAloOut(palo, g);
        return;
    }
    if (fOn) AddLo(g_psw, plo);
    else RemoveLo(g_psw, plo);
}

namespace
{
// orig: the ASEGA's +0x5C (a looping event sound kept while it plays)
std::unordered_map<const ASEGA*, CSndRef>& MpSndAsega()
{
    static std::unordered_map<const ASEGA*, CSndRef> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}
}  // namespace
// orig: 0x00210740's pitch terms: a guard source (cid 0x38) with its scale controller (+0xFC4: the size
// destabilizer) adds clamp(2 - 2 x its value, 0, 1) (D_002C3EA0 {2, -2, 0}, D_002C3EB0 {0, 1}): a shrunk guard sounds
// higher; then the game's +0x475C (no writer in the matched code: 0). Natively omitted (audit 2026-11 S2)
float GSoundPitchOffset(LO* pso)
{
    float g = 0.0f;
    if (pso && FCidDerivedFrom(pso->Cid(), 0x38))
        if (const GUARD* pg = PguardFromLo(pso); pg && pg->pscaleFC4)
            g += std::clamp(2.0f - 2.0f * pg->pscaleFC4->pos.x, 0.0f, 1.0f);
    return g;
}

// (at a world's load: the old world's ASEGAs are freed, and a new ASEGA at a freed one's address must not inherit
// its sound)
void ResetAsegaSounds() { MpSndAsega().clear(); }
// orig: 0x00130B18's 0x258040(+0x5C, 0): a retracted ASEGA lets its kept looper go (not stopped: a stop is an event of
// its clip), so a later ASEGA at the same address neither stops nor replaces it
void ReleaseAsegaSound(const ASEGA* pasega) { MpSndAsega().erase(pasega); }
namespace
{

// orig 0x00210740 (event kind 0: the sound idSnd at the object id (-1: the ASEGA's), falloff [x, y], volume k, pitch
// g plus GSoundPitchOffset; a looper kept by the ASEGA)
void PlayAsegaSound(ASEGA* pasega, LO* ploOwner, const ASEGEV* pev)
{
    const int idSnd = pev->an[0];
    if (idSnd == -1) return;
    if (FPcCarm(pasega->palo)) return;   // (PC addition, debug menu only: Sly's steps muted under Carmelita's, pccharacter.h)
    LO* pso = pev->an[1] == -1 ? static_cast<LO*>(pasega->palo) : PloAround(ploOwner, pev->an[1]);
    if (static const char* s_sz = std::getenv("SLY2N_SNDSTART"); s_sz && std::atoi(s_sz) == idSnd)   // (debug)
        std::printf("aseg sound %d: aseg %d asega %p palo %d source an1 %d -> %d (cid 0x%X inworld %d) falloff %d %d vol %.2f pitch %.2f\n", idSnd,
                    pasega->paseg ? pasega->paseg->oid : -1, static_cast<void*>(pasega), pasega->palo ? pasega->palo->oid : -1, pev->an[1],
                    pso ? pso->oid : -1, pso ? int(pso->Cid()) : 0, pso ? int(pso->fInWorld) : -1, pev->an[2], pev->an[3], pev->ag[2], pev->ag[0]);
    CSnd* psnd = PsndNewPlain(idSnd);
    psnd->SetSource(pso);
    psnd->SetFalloff(static_cast<float>(pev->an[2]), static_cast<float>(pev->an[3]));
    psnd->SetVolume(pev->ag[2]);
    psnd->SetPitch(pev->ag[0] + GSoundPitchOffset(pso));
    if (psnd->Kind() == 2) MpSndAsega()[pasega] = CSndRef(psnd);
}

// orig 0x002108E8 (event kind 1: the kept sound stopped when it is id (-1: any))
void StopAsegaSound(ASEGA* pasega, int id)
{
    auto it = MpSndAsega().find(pasega);
    if (it == MpSndAsega().end() || !it->second) return;
    CSndPlain* p = dynamic_cast<CSndPlain*>(it->second.get());
    if (id != -1 && (!p || p->m_sfxid != id)) return;
    it->second->Stop(0.0f);
    MpSndAsega().erase(it);
}

void OnAsegaEvent(ASEGA* pasega, ASEGEV* pev)
{
    LO* ploOwner = pasega->palo ? static_cast<LO*>(pasega->palo) : static_cast<LO*>(pasega->paseg);  // 0x131270
    const bool fReverse = pasega->svt < 0.0f;
    static const bool s_fPrint = std::getenv("SLY2N_TASKDEBUG") != nullptr;
    if (s_fPrint)
        std::fprintf(stderr, "aseg event kind %d oid %d g %.3f (aseg %d object %d svt %.2f)\n", pev->kind, pev->an[0], pev->ag[0],
                     pasega->paseg ? pasega->paseg->oid : -1, pasega->palo ? pasega->palo->oid : -1, pasega->svt);
    switch (pev->kind)
    {
    case 0: PlayAsegaSound(pasega, ploOwner, pev); break;
    case 1: StopAsegaSound(pasega, pev->an[0]); break;
    // orig: 0x00131290 case 2 -> 0x0014F750 JoltCm(the camera, the jolt's kind (an S16 word)): e.g. the elephants'
    // stomps in Elephant Rampage's crawl scene (ASEG 106965 at 1.17 s and 3 s, with the rumbles); natively dropped
    case 2: JoltCm(&g_cm, pev->an[0]); break;
    case 7:
    case 8: AddRemoveByOid((pev->kind == 7) != fReverse, pev->an[0], ploOwner, pev->ag[0]); break;
    case 4:
        // an ASEG played (0x12F9B8 from 0), a script object (cid 0x72) started (0x1DFD58: the actors' SCPT)
        if (LO* plo = PloAround(ploOwner, pev->an[0]))
        {
            if (FCidDerivedFrom(plo->Cid(), 0x6D)) ApplyAseg(static_cast<ASEG*>(plo), nullptr, 0.0f, 1.0f, 0, nullptr);
            else if (FCidDerivedFrom(plo->Cid(), 0x72) && g_pfnStartScpt) g_pfnStartScpt(plo, 0);
        }
        break;
    case 13: case 14: case 15: case 16:
    {
        // 0x210408 / 0x2104A0: the object (an[0], -1: the ASEGA's own) shown / hidden by its mesh group an[1]
        // (0x1F6978 / 0x1F69F8), or its collision entries an[1] on / off (0x1F6AC8 / 0x1F6B48); reversed backwards
        const bool fOn = ((pev->kind == 13 || pev->kind == 15) != fReverse);
        LO* plo = pev->an[0] == -1 ? static_cast<LO*>(pasega->palo) : PloAround(ploOwner, pev->an[0]);
        if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) break;
        if (static const char* s_szOid = std::getenv("SLY2N_SCENEOID"); s_szOid && plo->oid == std::atoi(s_szOid))
            std::printf("asegev kind %d an %d %d aseg %d asega palo %d owner %d t %.3f ev.t %.3f rev %d\n", pev->kind, pev->an[0], pev->an[1],
                        pasega->paseg ? pasega->paseg->oid : -1, pasega->palo ? pasega->palo->oid : -1, ploOwner ? ploOwner->oid : -1, g_clock.t, pev->t, int(fReverse));
        if (pev->kind <= 14) ShowAloGlobs(static_cast<ALO*>(plo), pev->an[1], fOn);
        else SetAloSoesByTag(static_cast<ALO*>(plo), pev->an[1], fOn);
        break;
    }
    case 17:
    {
        // orig: 0x00210538 (an authored explosion, optionally with a source / touched object and scale)
        LO* ploEffect = PloAround(ploOwner, pev->an[0]);
        if (!ploEffect) break;
        BURSTPARAMS bp;
        bp.grf = 1;
        bp.plo = pev->an[1] != -1 ? PloAround(ploOwner, pev->an[1]) : nullptr;
        if (!bp.plo) bp.plo = ploEffect;
        if (pev->an[2] != -1)
        {
            bp.ploTouch = PloAround(ploOwner, pev->an[2]);
            if (bp.ploTouch) bp.grf |= 0x20;
        }
        if (pev->ag[0] != 1.0f)
        {
            bp.g8 = pev->ag[0];
            bp.grf |= 0x18;
        }
        ExplodeLo(ploEffect, &bp);
        break;
    }
    case 20: SetEyesClosedOf(ploOwner, pev->an[0], pev->an[1], pev->ag[0]); break;  // 0x210678 (the eyes)
    case 3:
    {
        // 0x187E28 (rumk, dt)
        char sz[64];
        std::snprintf(sz, sizeof(sz), "aseg %d event 3, object %d", pasega->paseg ? pasega->paseg->oid : -1,
                      pasega->palo ? pasega->palo->oid : -1);
        TriggerJoyRumbleRumk(pev->an[0], pev->ag[0], sz);
        break;
    }
    default: ++g_cAsegaEventDropped; break;  // TODO: 18..19 / 22..24
    }
}

struct INITASEGEVENTS
{
    INITASEGEVENTS()
    {
        g_pfnAsegaEventOther = OnAsegaEvent;
        // orig: 0x00105B68 (option 244) -> 0x0015C150: burst through slot 51 with empty parameters.
        RegisterObjectMethod(0x105B68, [](LO* plo, const std::vector<CRef>&) {
            ExplodeLo(plo);
            return CRef(TAGK_Void);
        });
        // orig: 0x00105BE0 (option 616) -> 0x0015C198: the scripted
        // parameterized burst uses the same slot51 parameter block.
        RegisterObjectMethod(0x105BE0, [](LO* plo, const std::vector<CRef>& a) {
            BURSTPARAMS bp;
            bp.grf = static_cast<uint32_t>(NArg(a, 0));
            bp.plo = PloArg(a, 1);
            if (a.size() > 2) bp.dir = a[2].GetVector();
            if (a.size() > 3) bp.pos = a[3].GetVector();
            bp.g8 = GArg(a, 4);
            bp.gC = GArg(a, 5);
            ExplodeLo(plo, &bp);
            return CRef(TAGK_Void);
        });
        // orig: 0x00101108 / 0x00101190 (EOPIDs 55 / 56, options 127 / 128)
        RegisterObjectMethod(0x101108, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), 0x30)) FadeAloOut(static_cast<ALO*>(plo), GArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x101190, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), 0x30)) FadeAloIn(static_cast<ALO*>(plo), GArg(a, 0));
            return CRef(TAGK_Void);
        });
        // EOPID 114 (option 222, a float field at ASEG +0x74, read only): the clip's length tMax. Mojo Trap Action's
        // switches (use 59310's s0) set their reload (+1590) from 1 / max(crusher clip 32560's length, option 1591),
        // which the lever's return clip 60051 takes as its speed: without it the traps re-armed after 1.3 s, not 5 s
        RegisterObjectGetter(114, [](LO* plo) {
            CRef r;
            if (FCidDerivedFrom(plo->Cid(), 0x6D)) r.SetF32(PasegdOf(static_cast<ASEG*>(plo))->tMax);
            return r;
        });
        // orig: 0x001F7D90 (EOPID 58, option 130): the authored fade rate also enables updates.
        RegisterObjectSetter(58, [](LO* plo, const CRef& r) {
            if (FCidDerivedFrom(plo->Cid(), 0x30)) SetFadeRate(static_cast<ALO*>(plo), GArg({r}, 0));
        });
    }
} s_initasegevents;
}
