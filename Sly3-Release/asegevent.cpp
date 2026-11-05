// The ASEG events the world side handles (asega.h g_pfnAsegaEventOther; orig 0x00131290's kinds): 7 / 8 an
// object added / removed (0x210338: by its oid around the animation's object, faded over ag[0] when an ALO,
// 0x1F7C68 / 0x1F7BF8; reversed while the animation plays backwards).

#include "sly3/ids3.h"
#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "aseg.h"
#include "asega.h"
#include "clock.h"
#include "shd.h"
#include "sm.h"
#include "brx.h"
#include "cls.h"
#include "emitter.h"
#include "objmethod.h"
#include "sw.h"
#include "sound.h"

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
    if (g != 0.0f && FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO)))
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
    return s_mp;
}
}  // namespace
// (at a world's load: the old world's ASEGAs are freed, and a new ASEGA at a freed one's address must not inherit
// its sound)
void ResetAsegaSounds() { MpSndAsega().clear(); }
namespace
{

// orig 0x00210740 (event kind 0: the sound idSnd at the object id (-1: the ASEGA's), falloff [x, y], volume k, pitch
// g (TODO: a guard's distance curve +0xFC4 and the game's +0x475C added to it); a looper kept by the ASEGA)
void PlayAsegaSound(ASEGA* pasega, LO* ploOwner, const ASEGEV* pev)
{
    const int idSnd = pev->an[0];
    if (idSnd == -1) return;
    LO* pso = pev->an[1] == -1 ? static_cast<LO*>(pasega->palo) : PloAround(ploOwner, pev->an[1]);
    CSnd* psnd = PsndNewPlain(idSnd);
    psnd->SetSource(pso);
    psnd->SetFalloff(static_cast<float>(pev->an[2]), static_cast<float>(pev->an[3]));
    psnd->SetVolume(pev->ag[2]);
    psnd->SetPitch(pev->ag[0]);
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
    case 7:
    case 8: AddRemoveByOid((pev->kind == 7) != fReverse, pev->an[0], ploOwner, pev->ag[0]); break;
    case 4:
        // an ASEG played (0x12F9B8 from 0), a script object (cid 0x72) started (0x1DFD58: the actors' SCPT)
        if (LO* plo = PloAround(ploOwner, pev->an[0]))
        {
            if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ASEG))) ApplyAseg(static_cast<ASEG*>(plo), nullptr, 0.0f, 1.0f, 0, nullptr);
            else if (FCidDerivedFrom(plo->Cid(), 0x87 /* sly2 0x72 SCPT */) && g_pfnStartScpt) g_pfnStartScpt(plo, 0);
        }
        break;
    case 13: case 14: case 15: case 16:
    {
        // 0x210408 / 0x2104A0: the object (an[0], -1: the ASEGA's own) shown / hidden by its mesh group an[1]
        // (0x1F6978 / 0x1F69F8), or its collision entries an[1] on / off (0x1F6AC8 / 0x1F6B48); reversed backwards
        const bool fOn = ((pev->kind == 13 || pev->kind == 15) != fReverse);
        LO* plo = pev->an[0] == -1 ? static_cast<LO*>(pasega->palo) : PloAround(ploOwner, pev->an[0]);
        if (!plo || !FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) break;
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
    default: ++g_cAsegaEventDropped; break;  // TODO: 2 / 3 / 18..19 / 22..24
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
        // orig: 0x00101108 / 0x00101190 (EOPIDs 55 / 56, options 127 / 128)
        RegisterObjectMethod(0x101108, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) FadeAloOut(static_cast<ALO*>(plo), GArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x101190, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) FadeAloIn(static_cast<ALO*>(plo), GArg(a, 0));
            return CRef(TAGK_Void);
        });
    }
} s_initasegevents;
}
