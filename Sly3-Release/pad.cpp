// The touch pads (cid 0x53, module 0x148B18..0x1492F0: an ALO with a trigger state object at +0x150): its state
// (+0x1E0, the time +0x1E4, the re-arm time +0x1E8 (EOPID 419), no sound +0x1EC (421)), the kind +0x2A8 (413: 0
// touched by an object of its lists, 1 / 2 hit (slot 75, 0x148F20: TODO), 3 the player's state), its lists (+0x2AC the
// objects, +0x2D0 the classes: EOPID 418 = 0x149248) and the toucher +0x2F8 (EOPID 427). Entering state 1 runs its
// splice event 5: the intro's bounce pads (8448, 150508) launch the toucher with the actors' method 730 (0x10A790 ->
// 0x141968) toward their s1088 after 0.05 s. And the actors' launch itself (jump kind 8).
#include "actor.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "alo.h"
#include "asega.h"
#include "sw.h"
#include "worldaccess.h"
#include "cls.h"
#include "clock.h"
#include "jump.h"
#include "lo.h"
#include "frame.h"
#include "objmethod.h"
#include "ref.h"
#include "so.h"
#include "sound.h"
#include "sly.h"
#include "spliceglue.h"
#include "trigger.h"
#include "update.h"
#include "vtnative.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "combo3.h"   // (sly3::PloFindOwned3: a descendant by oid)

namespace
{
glm::vec3 VelBallistic(float g, const glm::vec3& pos, const glm::vec3& posTarget, const glm::vec3& acc);
}

// orig: 0x00141968 (the actor launched to a point posT: g the elevation's share, h the air control's weight): the
// velocity from its feet (+0x248) to the point by its gravity; without a damage reaction the jump kind 8, the air
// control +0x40 3 toward that velocity's horizontal part, +0x34 h (EOPID 730, SCPT 0x2B)
void LaunchActorTo(ACTOR* pa, const glm::vec3& posT, float g, float h)
{
    LO* plo = pa->palo;
    glm::vec3 pos = PosAloCenter(pa->palo);  // 0x1A62E8
    pos.z += pa->vecBase.z;
    const SOST* pst = pa->palo->psort ? pa->palo->psort->psost.get() : nullptr;
    const glm::vec3 v = VelBallistic(g, pos, posT, pst ? pst->dvGravity : glm::vec3(0.0f, 0.0f, -2275.0f));
    SetAloVelocityTo(pa->palo, v);  // vt slot 35
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("launch %d from %.0f %.0f %.0f to %.0f %.0f %.0f (g %.2f h %.2f): v %.1f %.1f %.1f t %.9g\n", plo->oid, pos.x, pos.y,
                    pos.z, posT.x, posT.y, posT.z, g, h, v.x, v.y, v.z, g_clock.t);
    if (pa->nSubDF4 == -1)
    {
        SetJumpKind(&pa->jump, 8, 0, 0);  // 0x195A00
        pa->jump.n40 = 3;
        pa->jump.vecA0 = glm::vec3(v.x, v.y, 0.0f);
        pa->jump.g34 = h;
        if (h == 0.0f)
        {
            pa->jump.f28 = 0;
            pa->jump.f2C = 0;
        }
    }
}

namespace
{
struct PAD : TRIGGERSTATE
{
    bool fInit = false;
    float dtRearm = 1.0f;    // +0x1E8 (0x148718: 1)
    int fNoSound = 0;        // +0x1EC
    int fStay = 0;           // +0x1F4 (+0xA4: stays at 1)
    int kind = 0;            // +0x2A8
    std::vector<OID> aoid;   // +0x2AC
    std::vector<int> acid;   // +0x2D0
    LO* ploAction = nullptr;   // +0x2F4 (EOPID 414)
};
std::unordered_map<LO*, PAD>& MpPad()
{
    static std::unordered_map<LO*, PAD> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}

PAD& Pad(LO* plo)
{
    PAD& p = MpPad()[plo];
    if (p.fInit) return p;
    p.fInit = true;
    p.owner = plo;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval.front();
        switch (o.eopid)
        {
        case 413: p.kind = v.n; break;
        case 419: p.dtRearm = v.k == 1 ? v.ag[0] : static_cast<float>(v.n); break;
        case 421: p.fNoSound = v.n; break;
        case 423: p.fStay = v.n; break;
        case 417: if (p.aoid.size() < 8) p.aoid.push_back(v.n); break;  // (0x149220)
        case 418: if (p.acid.size() < 8) p.acid.push_back(v.n); break;  // 0x149248
        default: break;
        }
    }
    if (std::getenv("SLY2N_PADDEBUG") && plo->pframe)
        if (CRef* pr = plo->pframe->PrefFindBinding(1088, 1))
        {
            const glm::vec3 v = pr->GetVector();
            const glm::vec3 c = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(plo)) : glm::vec3(0.0f);
            std::printf("pad %d at %.0f %.0f %.0f: kind %d, s1088 (tag %d) %.0f %.0f %.0f\n", plo->oid, c.x, c.y, c.z, p.kind, pr->m_tagk, v.x, v.y, v.z);
        }
    p.kind = static_cast<int>(plo->U32Field(0, 0x2A8, p.kind));
    p.fStay = static_cast<int>(plo->U32Field(0, 0x1F4, p.fStay));
    const OID action = static_cast<OID>(plo->U32Field(0, 0x2F4, uint32_t(-1)));
    if (action != -1) p.ploAction = PloFindRelative(plo, action);
    p.state = 0;  // 0x148C38: 0x1490A0(0) from -1 (nothing run)
    p.tState = g_clock.t;
    return p;
}

// orig: 0x00149100 (the object in the lists: its oid among the objects', and of one of the classes)
bool FPadAccepts(const PAD& p, LO* plo)
{
    if (!plo) return false;
    if (!p.aoid.empty())
    {
        bool f = false;
        for (OID oid : p.aoid) f = f || plo->oid == oid || plo->oidInstance == oid;
        if (!f) return false;
    }
    if (!p.acid.empty())
    {
        bool f = false;
        for (int cid : p.acid) f = f || FCidDerivedFrom(plo->Cid(), static_cast<CID>(cid));  // 0x132B10
        if (!f) return false;
    }
    return true;
}

// orig: 0x001490A0 -> 0x00148A78 (+ 0x1487A0 / 0x148860: entering 1 from 0 runs event 5, the sound 2 unless +0x1EC,
// then the enter ASEGs and 2 unless +0xA4; 0 runs event 6 and the leave ASEGs)
void EnterPad(LO* plo, PAD& p);
void LeavePad(LO* plo, PAD& p);
void SetPadState(LO* plo, PAD& p, int n)
{
    const int old = p.state;
    if (old == n) return;
    const bool f1 = old == 0 && n == 1, f0 = old != -1 && n == 0;
    p.state = n;
    p.tState = g_clock.t;
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("pad %d: state %d -> %d (toucher %d) t %.3f\n", plo->oid, old, n, p.ploFound ? p.ploFound->oid : -1, g_clock.t);
    if (f1) EnterPad(plo, p);
    if (f0) LeavePad(plo, p);
}

// orig: 0x001487A0 (the enter actions: event 5, the sound 2 unless +0x1EC, the enter list, state 2 unless +0xA4, the
// owner's message 6)
void EnterPad(LO* plo, PAD& p)
{
    HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
    if (!p.fNoSound)
    {
        CSnd* ps = PsndNewPlain(2);
        ps->SetSource(plo);
    }
    RunEmbeddedTriggerList(plo, 0, p.ploFound);
    if (!p.fStay) SetPadState(plo, p, 2);
    NotifyLo(plo, 6, static_cast<TRIGGERSTATE*>(&p));
}

// orig: 0x00148860 (the leave actions: event 6, the leave list, message 7)
void LeavePad(LO* plo, PAD& p)
{
    HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
    RunEmbeddedTriggerList(plo, 1, p.ploFound);
    NotifyLo(plo, 7, static_cast<TRIGGERSTATE*>(&p));
}

// orig: 0x00149010 (the deferred call 0x2A4E80 of a touch: an actor just bounced off (its jump kind 3 within 0.15 s)
// or in the jump's state 0x11 is ignored; else state 1)
void PadTouched(LO* plo, PAD& p)
{
    if (ACTOR* pa = p.ploFound ? PactorFromLo(p.ploFound) : nullptr)
    {
        if (pa->jump.state == 0x11 || (pa->jump.kind == 3 && g_clock.t - pa->jump.t20 < 0.15f))
        {
            p.ploFound = nullptr;
            return;
        }
    }
    SetPadState(plo, p, 1);
}

// orig: 0x00149010, queued through 0x149AB8. The callback reads the current
// toucher/jump state at dispatch, after the detached batch has been taken.
void DeferredPadTouched(void* object, int)
{
    LO* plo = static_cast<LO*>(object);
    PadTouched(plo, Pad(plo));
}

// orig: 0x00148C98 (vt slot 16)
void UpdatePad(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's
    PAD& p = Pad(plo);
    switch (p.state)
    {
    case 2:
        // orig: 0x148C98: this hold test applies to every pad kind.
        if (SLY* sly = PslyGlobal(); sly && sly->jts == jts::Hang && sly->ploOther == p.ploAction) p.tState = g_clock.t;
        if (0.0f <= p.dtRearm && p.dtRearm < g_clock.t - p.tState) SetPadState(plo, p, 4);
        break;
    case 4:
        if (p.kind == 1) SetPadState(plo, p, 0);
        else if (p.kind == 0 || p.kind == 2 || p.kind == 3) SetPadState(plo, p, 1);
        break;
    case 0: case 1:
    {
        int k = p.state;
        if (p.kind == 0)
        {
            // an object of the lists in contact with the pad (its attachment's overlaps' contacts, not kind 10):
            // the toucher kept and the touch's call queued (0x149AB8); the state stays, else 0
            k = 0;
            ALO* palo = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
            OXA* poxa = palo && palo->psort ? palo->psort->poxa : nullptr;
            LO* ploFound = nullptr;
            for (OX* pox = poxa ? poxa->pox : nullptr; pox && !ploFound; pox = pox->poxNext)
                for (XP* pxp = pox->pxp; pxp && !ploFound; pxp = pxp->pxpNext)
                {
                    if (pxp->xpk == 10) continue;
                    const int i = poxa->pso == pxp->axpd[0].paloRoot ? 0 : 1;
                    if (pxp->axpd[i].palo != palo) continue;
                    LO* po = pxp->axpd[1 - i].paloRoot;
                    if (FPadAccepts(p, po)) ploFound = po;
                    if (ploFound && std::getenv("SLY2N_PADDEBUG"))
                        std::printf("pad %d: touched by %d (xpk %d, entry of %d at %.0f %.0f %.0f, normal %.2f %.2f %.2f)\n", plo->oid, po->oid,
                                    pxp->xpk, pxp->axpd[i].palo ? pxp->axpd[i].palo->oid : -1, pxp->axpd[i].pos.x, pxp->axpd[i].pos.y,
                                    pxp->axpd[i].pos.z, pxp->normal.x, pxp->normal.y, pxp->normal.z);
                }
            if (ploFound)
            {
                p.ploFound = ploFound;
                PostDeferredCall(plo, 0x149010, DeferredPadTouched);
                k = p.state;
            }
        }
        else if (p.kind == 3)
        {
            SLY* sly = PslyGlobal();
            k = sly && sly->jts == jts::Hang && sly->ploOther == p.ploAction ? 1 : 0;
        }
        SetPadState(plo, p, k);
        break;
    }
    default: break;
    }
}

// orig: 0x00148F20 (vt slot 75: a contact accepted (0x1F7588), not of the byte 4, from an object of the pad's lists:
// kind 1 goes to state 1 from 0; kind 2 to state 1 from 0 once +0x1E8 s are over since the last change, back to 0
// from 1 (a bell / switch hit with the cane, e.g. Dimitri's boat bell 142627, job 1818's start))
bool FAcceptPadContact(ALO* palo, CTMSG* pctm)
{
    const bool r = pctm->grfAccepted != 0;  // 0x1F7588
    PAD& p = Pad(palo);
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("pad %d: contact from %d accepted 0x%X kind %d state %d\n", palo->oid, pctm->ploSender ? pctm->ploSender->oid : -1,
                    pctm->grfAccepted, p.kind, p.state);
    if (r && !(pctm->grfAccepted & 4) && FPadAccepts(p, pctm->ploSender))
    {
        switch (p.kind)
        {
        case 1:
            if (p.state == 0) SetPadState(palo, p, 1);
            break;
        case 2:
            if (p.state == 0)
            {
                if (p.dtRearm <= g_clock.t - p.tState) SetPadState(palo, p, 1);
            }
            else if (p.state == 1) SetPadState(palo, p, 0);
            break;
        default: break;
        }
    }
    return r;
}

// orig: 0x001D6848 (the launch velocity from pos to the target: the angle g of the way between the line's elevation
// and the vertical (sin g), with the acceleration acc; 0 when unreachable)
glm::vec3 VelBallistic(float g, const glm::vec3& pos, const glm::vec3& posTarget, const glm::vec3& acc)
{
    float k = std::sin(g);
    glm::vec3 d = posTarget - pos;
    const float z = d.z;
    d.z = 0.0f;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    float angle = std::atan2(z, len);
    if (angle < 0.0f) angle = 0.0f;
    float q = acc.z;  // (dot with (0, 0, 1))
    k = angle + (1.57079637f - angle) * k;
    q = q / ((z - len * std::tan(k)) * 2.0f);
    if (q <= 0.0f) return glm::vec3(0.0f);
    q = std::sqrt(q);
    const float speed = len * q / std::cos(k);
    glm::vec3 v(d.x, d.y, len * std::tan(k));
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0.0f ? v * (speed / l) : glm::vec3(0.0f);
}

// ---- Sly 3's PAD (cid 0x5D, vt 0x354BC8): a new class (size 0x250), not Sly 2's 0x53 layout. Its trigger record at
// +0x130 (0x15BDF8: state -1, re-arm time +0x138 1.0 (EOPID 468), stay +0x13C, remove-the-owner +0x140, the toucher
// +0x144 (EOPID 474, optid 555)); no scan while +0x220 (EOPID 459); its filter (vt slot 80 0x15C9A8): the excluded
// entries (+0x1B0), the object pairs (+0x148) and the classes (+0x18C: EOPID 464 adds one, 465 removes). E.g. the Ep3
// hack's balls 74263 (class list 0x49 HACKSO): the ship touching one tows it (its script's event 5 -> the ship's 2309),
// carried into a socket 74248 it is taken and the laser's node counts it (2313): Sly 2's pad update never ran on them.
struct PAD3;
void SetPad3State(LO* plo, PAD3& p, int n);
void SetLaunchPadMode(LO* plo, PAD3& p, int mode);
struct PAD3 : TRIGGERSTATE
{
    bool fInit = false;
    float dtRearm = 1.0f;     // +0x138
    int fStay = 0;            // +0x13C
    int fNoScan = 0;          // +0x220
    std::vector<int> acid;    // +0x18C
    // Sly 3's launch pad (cid 0x5E, vt 0x3546C8, size 0x270, derived from 0x5D): the pad its toucher stands on throws
    // it up / toward a target (e.g. m_ext's 30844 on the post beside the factory, the Intro's way up to the tower)
    bool fLaunch = false;
    int mode = 0;             // +0x250  0 idle, 1 touched (0.05 s), 2 the launch
    float tMode = 0.0f;       // +0x254
    LO* ploTarget = nullptr;  // +0x258  EOPID 496: where it throws to (else 300 above the pad, in its frame)
    ASEG* paseg = nullptr;    // +0x25C  EOPID 497: the pad's bounce, applied at the launch
    ASEGA* pasega = nullptr;  // +0x260
    float gArc = 0.75f;       // +0x264  EOPID 499: the elevation's share (0x15D6D8: 0.75)
    float gAir = 1.0f;        // +0x268  EOPID 498: the air control's weight (1.0)
};
std::unordered_map<LO*, PAD3>& MpPad3()
{
    static std::unordered_map<LO*, PAD3> s_mp;
    WORLD_TABLE_LOCAL(s_mp);
    return s_mp;
}
PAD3& Pad3(LO* plo)
{
    PAD3& p = MpPad3()[plo];
    if (p.fInit) return p;
    p.fInit = true;
    p.owner = plo;
    for (const LO* ps = plo; ps; ps = ps->ploCloneOf && ps->ploCloneOf != ps ? ps->ploCloneOf : nullptr)
    {
        for (const OPT& o : ps->aopt)
        {
            if (o.aval.empty()) continue;
            const OPTVAL& v = o.aval.front();
            const int n = v.k == 1 ? static_cast<int>(v.ag[0]) : v.n;
            switch (o.eopid)
            {
            case 459: p.fNoScan = n; break;
            case 468: p.dtRearm = v.k == 1 || v.k == 10 ? v.ag[0] : static_cast<float>(v.n); break;
            case 464: if (p.acid.size() < 0x26) p.acid.push_back(n); break;
            default: break;
            }
        }
        if (!plo->aopt.empty()) break;  // (a copy's own options are the template's)
    }
    // (orig3 0x15D6D8, the launch pad's init: the class list gets 0x32 (the POs: 0x15C4A8), no re-arm wait, the arc
    // 0.75 and air control 1.0, then its options 496..499)
    if (plo->Cid() == 0x5E)
    {
        p.fLaunch = true;
        p.dtRearm = 0.0f;
        if (p.acid.size() < 4) p.acid.insert(p.acid.begin(), 0x32);
        for (const OPT& o : plo->aopt)
        {
            if (o.aval.empty()) continue;
            const OPTVAL& v = o.aval.front();
            const float g = v.k == 1 || v.k == 10 ? v.ag[0] : static_cast<float>(v.n);
            switch (o.eopid)
            {
            case 496:
                // (an oid: found from the pad up through its parents, as SCPT objects in a context, else the world's)
                for (LO* pctx = plo->paloParent; pctx && !p.ploTarget; pctx = pctx->paloParent)
                    p.ploTarget = sly3::PloFindOwned3(pctx, v.n);
                if (!p.ploTarget && g_psw) p.ploTarget = PloFindSwObjectByOid(g_psw, v.n);
                break;
            case 497:
                for (LO* pc : plo->aploOwned)
                    if (pc->oid == v.n) p.paseg = dynamic_cast<ASEG*>(pc);
                if (!p.paseg && g_psw) p.paseg = dynamic_cast<ASEG*>(PloFindSwObjectByOid(g_psw, v.n));
                break;
            case 498: p.gAir = g; break;
            case 499: p.gArc = g; break;
            default: break;
            }
        }
        if (std::getenv("SLY2N_PADDEBUG"))
            for (const OPT& o : plo->aopt)
                std::printf("pad3 %d: option %d k %d n %d g %.2f\n", plo->oid, o.eopid, o.aval.empty() ? -1 : o.aval[0].k, o.aval.empty() ? 0 : o.aval[0].n, o.aval.empty() ? 0.0f : o.aval[0].ag[0]);
        if (std::getenv("SLY2N_PADDEBUG"))
            std::printf("pad3 %d: launch pad, target %d, aseg %d, arc %.2f, air %.2f\n", plo->oid, p.ploTarget ? p.ploTarget->oid : -1,
                        p.paseg ? static_cast<LO*>(p.paseg)->oid : -1, p.gArc, p.gAir);
    }
    p.state = -1;
    SetPad3State(plo, p, 0);  // slot 17 0x15C730 -> 0x15BE30 / slot 31 -> 0x15BE10: state 0
    return p;
}
// orig3: 0x0015C9A8 (vt slot 80: the toucher accepted by the class list; the entry / pair lists are not used by the
// levels ported so far)
bool FPad3Accepts(const PAD3& p, LO* plo)
{
    if (!plo) return false;
    if (p.fLaunch && p.acid.empty()) return false;
    if (p.acid.empty()) return true;
    for (int cid : p.acid)
        if (FCidDerivedFrom(plo->Cid(), static_cast<CID>(cid))) return true;  // 0x13B500
    return false;
}
// (the original's vt slot 36: an object's position record's world matrix)
glm::mat4 MatPosAlo3(ALO* palo)
{
    XF* pxf = palo ? PxfPos(palo) : nullptr;
    return pxf ? pxf->matWorld : MatWorldOfAlo(palo);
}
// orig3: 0x0015C958 (the pad's enter, the record's vt entry 2: 0x15BF28: event 5, the sound, the enter lists, the
// owner's message 6; TODO(sly3): the effect +0x224 (EOPID 460) 0x2459F8 / 0x244F08)
void EnterPad3(LO* plo, PAD3& p)
{
    HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
    CSnd* ps = PsndNewPlain(2);
    ps->SetSource(plo);
    RunEmbeddedTriggerList(plo, 0, p.ploFound);
    NotifyLo(plo, 6, static_cast<TRIGGERSTATE*>(&p));
}
// orig3: 0x0015D908 (the launch pad's throw): the pad's enter, then the toucher thrown at the target (its matrix'
// position; none: 300 above the pad in its frame, 0x1C9428): an actor not carried / scripted (0x156700) by
// LaunchActorTo (0x153F68) with the arc / air control; Sly in his Hover Pack state (22) gets the ballistic velocity
// (0x212480) only when it is higher than his own; (an object of class 0x3A gets the message 8 {target, arc, air}:
// TODO(sly3), none seen). Then the pad's bounce (0x137588).
void LaunchPad3(LO* plo, PAD3& p)
{
    EnterPad3(plo, p);
    ALO* palo = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
    if (!palo) return;
    glm::vec3 pos;
    if (p.ploTarget && FCidDerivedFrom(p.ploTarget->Cid(), S3CID(0x30))) pos = glm::vec3(MatPosAlo3(static_cast<ALO*>(p.ploTarget))[3]);
    else if (const LOX* plox = dynamic_cast<const LOX*>(p.ploTarget)) pos = glm::vec3((plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat)[3]);   // (the targets seen are LOX points)
    else pos = glm::vec3(MatPosAlo3(palo) * glm::vec4(0.0f, 0.0f, 300.0f, 1.0f));
    ACTOR* pa = p.ploFound && FCidDerivedFrom(p.ploFound->Cid(), S3CID(0x33)) ? PactorFromLo(p.ploFound) : nullptr;
    if (pa && !(pa->n270 > 0 || pa->prunScript != nullptr))
    {
        SLY* psly = PslyGlobal();
        if (psly && pa == psly && pa->jump.state == 22)
        {
            const SOST* pst = pa->palo->psort ? pa->palo->psort->psost.get() : nullptr;
            const glm::vec3 v = VelBallistic(p.gArc, glm::vec3(MatPosAlo3(palo)[3]), pos - glm::vec3(pa->vecBase),
                                             pst ? pst->dvGravity : glm::vec3(0.0f, 0.0f, -2275.0f));
            if (VelAloPos(pa->palo).z < v.z) SetAloVelocityTo(pa->palo, v);   // (TODO(sly3): +0x1AD4 == 1: 0x1E2E88(Sly, 0))
        }
        else LaunchActorTo(pa, pos, p.gArc, p.gAir);
    }
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("pad3 %d: launch of %d to %.0f %.0f %.0f t %.3f\n", plo->oid, p.ploFound ? p.ploFound->oid : -1, pos.x, pos.y, pos.z, g_clock.t);
    if (p.paseg) ApplyAsegCur(p.paseg, palo, 0.0f, 0.0f, 1, &p.pasega);
}
// orig3: 0x0015D8C8 (the launch pad's mode; 2 throws)
void SetLaunchPadMode(LO* plo, PAD3& p, int mode)
{
    if (p.mode == mode) return;
    p.mode = mode;
    p.tMode = g_clock.t;
    if (mode == 2) LaunchPad3(plo, p);
}
// orig3: 0x0015C178 (the trigger record's state: entering 1 from 0 runs the enter (event 5, the sound, the enter
// lists, the owner's message 6) and 2 unless it stays; 0 from any but -1 the leave (event 6, the lists, message 7))
void SetPad3State(LO* plo, PAD3& p, int n)
{
    const int old = p.state;
    if (old == n) return;
    const bool fEnter = old == 0 && n == 1, fLeave = old != -1 && n == 0;
    p.state = n;
    p.tState = g_clock.t;
    if (std::getenv("SLY2N_PADDEBUG"))
        std::printf("pad3 %d: state %d -> %d (toucher %d) t %.3f\n", plo->oid, old, n, p.ploFound ? p.ploFound->oid : -1, g_clock.t);
    if (fEnter)
    {
        // the record's enter (its vt entry 2): the pad's 0x15C958 (EnterPad3), the launch pad's 0x15D8A8 (its mode 1:
        // the enter and the throw 0.05 s later, 0x15D908)
        if (p.fLaunch) SetLaunchPadMode(plo, p, 1);
        else EnterPad3(plo, p);
        if (!p.fStay) SetPad3State(plo, p, 2);
    }
    if (fLeave)
    {
        HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
        RunEmbeddedTriggerList(plo, 1, p.ploFound);
        NotifyLo(plo, 7, static_cast<TRIGGERSTATE*>(&p));
    }
}
// orig3: 0x0015CAD0 (the deferred touch: an actor in its jump state 0x12, or bounced (kind 3) within 0.15 s, is
// dropped; else state 1)
void DeferredPad3Touched(void* object, int)
{
    LO* plo = static_cast<LO*>(object);
    PAD3& p = Pad3(plo);
    if (ACTOR* pa = p.ploFound ? PactorFromLo(p.ploFound) : nullptr)
        if (pa->jump.state == 0x12 || (pa->jump.kind == 3 && g_clock.t - pa->jump.t20 < 0.15f))
        {
            p.ploFound = nullptr;
            return;
        }
    SetPad3State(plo, p, 1);
}
// orig3: 0x0015C790 (vt slot 18)
void UpdatePad3(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);  // the ALO's (0x23C660)
    PAD3& p = Pad3(plo);
    switch (p.state)
    {
    case 2:
        if (p.dtRearm >= 0.0f && g_clock.t - p.tState > p.dtRearm) SetPad3State(plo, p, 4);
        return;
    case 4:
        SetPad3State(plo, p, p.fNoScan ? 4 : 1);  // (with +0x220 the original reads an unset register: kept)
        return;
    case 0:
    case 1:
    {
        int k = p.state;
        if (!p.fNoScan)
        {
            // the attachment's overlaps (+0xDC), not of kind 10, whose own entry is the pad's: the other root
            // accepted by slot 80 is the toucher (+0x144), the touch deferred (0x15CAD0); none: 0
            ALO* palo = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
            OXA* poxa = palo && palo->psort ? palo->psort->poxa : nullptr;
            if (!poxa) break;
            k = 0;
            for (OX* pox = poxa->pox; pox; pox = pox->poxNext)
                for (XP* pxp = pox->pxp; pxp; pxp = pxp->pxpNext)
                {
                    if (pxp->xpk == 10) continue;
                    const int i = poxa->pso == pxp->axpd[0].paloRoot ? 0 : 1;
                    if (pxp->axpd[i].palo != palo) continue;
                    LO* po = pxp->axpd[1 - i].paloRoot;
                    if (!FPad3Accepts(p, po)) continue;
                    // (orig3 0x15D758, the launch pad's slot 80: only from above: the contact's normal up (z >= 0.7,
                    // the pad's end as the original gets it) and the toucher not below the contact point)
                    if (p.fLaunch)
                    {
                        const glm::vec3 n = i == 1 ? pxp->normal : -pxp->normal;
                        if (std::getenv("SLY2N_PADDEBUG"))
                            std::printf("pad3 %d: contact of %d end %d normal %.2f %.2f %.2f at z %.0f, toucher z %.0f\n", plo->oid, po->oid, i,
                                        pxp->normal.x, pxp->normal.y, pxp->normal.z, pxp->axpd[i].pos.z, glm::vec3(MatPosAlo3(static_cast<ALO*>(po))[3]).z);
                        if (n.z < 0.7f || glm::vec3(MatPosAlo3(static_cast<ALO*>(po))[3]).z < pxp->axpd[i].pos.z) continue;
                    }
                    p.ploFound = po;
                    if (std::getenv("SLY2N_PADDEBUG")) std::printf("pad3 %d: touched by %d t %.3f\n", plo->oid, po->oid, g_clock.t);
                    PostDeferredCall(plo, FN3_TAG | 0x15CAD0, DeferredPad3Touched);
                    return;
                }
        }
        SetPad3State(plo, p, k);
        return;
    }
    default: return;
    }
}

// orig3: 0x0015D810 (the launch pad's vt slot 18): the pad's update, then its mode: 1 -> 2 after 0.05 s (the throw),
// 2 -> 0
void UpdateLaunchPad3(LO* plo)
{
    UpdatePad3(plo);
    PAD3& p = Pad3(plo);
    if (p.mode == 1 && g_clock.t - p.tMode > 0.05f) SetLaunchPadMode(plo, p, 2);
    else if (p.mode == 2) SetLaunchPadMode(plo, p, 0);
}

struct INITPAD
{
    INITPAD()
    {
        RegisterLoUpdate(0x148C98, UpdatePad);
        RegisterLoUpdate(FN3_TAG | 0x15C790, UpdatePad3);
        RegisterLoUpdate(FN3_TAG | 0x15D810, UpdateLaunchPad3);
        // the launch pad's options set by a splice (seto!): 496 the target (+0x258), 498 / 499 the air control / arc
        RegisterObjectSetter3(496, [](LO* plo, const CRef& r) {
            if (std::getenv("SLY2N_PADDEBUG")) std::printf("pad3 %d: target set to %d\n", plo ? plo->oid : -1, r.m_tagk == TAGK_Basic && dynamic_cast<LO*>(r.m_pbasic) ? dynamic_cast<LO*>(r.m_pbasic)->oid : -1);
            if (plo && plo->Cid() == 0x5E) Pad3(plo).ploTarget = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr;
        });
        RegisterObjectSetter3(498, [](LO* plo, const CRef& r) { if (plo && plo->Cid() == 0x5E) Pad3(plo).gAir = r.RefCoerceF32().m_g; });
        RegisterObjectSetter3(499, [](LO* plo, const CRef& r) { if (plo && plo->Cid() == 0x5E) Pad3(plo).gArc = r.RefCoerceF32().m_g; });
        // Sly 3 PAD: EOPID 474 (optid 555) its toucher (+0x144); 247 / 583 (thunks 0x106238 / 0x1062C0) its enter /
        // leave run by a script (Sly 2's 428 / 429)
        RegisterObjectGetter3(474, [](LO* plo) {
            CRef r;
            PAD3* pp = MpPad3().count(plo) ? &MpPad3()[plo] : nullptr;
            r.SetBasic(pp ? pp->ploFound : nullptr);
            return r;
        });
        RegisterObjectMethod3(0x106238, [](LO* plo, const std::vector<CRef>&) {
            if (plo && plo->Cid() == 0x5D) { PAD3& p = Pad3(plo); SetPad3State(plo, p, 0); SetPad3State(plo, p, 1); }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x1062C0, [](LO* plo, const std::vector<CRef>&) {
            if (plo && plo->Cid() == 0x5D) SetPad3State(plo, Pad3(plo), 0);
            return CRef(TAGK_Void);
        });
        // Direct embedded fields must also update the live pad cache when scripts
        // change them. Loading stores these fields before Pad's first update.
        RegisterObjectSetter(413, [](LO* plo, const CRef& r) { Pad(plo).kind = r.RefCoerceS32().m_n; });
        RegisterObjectGetter(413, [](LO* plo) { CRef r; r.SetS32(Pad(plo).kind); return r; });
        RegisterObjectSetter(414, [](LO* plo, const CRef& r) {
            Pad(plo).ploAction = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr;
        });
        RegisterObjectGetter(414, [](LO* plo) { CRef r; r.SetBasic(Pad(plo).ploAction); return r; });
        RegisterObjectSetter(419, [](LO* plo, const CRef& r) { Pad(plo).dtRearm = r.RefCoerceF32().m_g; });
        RegisterObjectGetter(419, [](LO* plo) { CRef r; r.SetF32(Pad(plo).dtRearm); return r; });
        RegisterObjectSetter(421, [](LO* plo, const CRef& r) { Pad(plo).fNoSound = !r.FFalse(); });
        RegisterObjectGetter(421, [](LO* plo) { CRef r; r.SetBool(Pad(plo).fNoSound); return r; });
        RegisterObjectSetter(423, [](LO* plo, const CRef& r) { Pad(plo).fStay = !r.FFalse(); });
        RegisterObjectGetter(423, [](LO* plo) { CRef r; r.SetBool(Pad(plo).fStay); return r; });
        // orig: 0x1490A0 -> 0x148A78 (EOPID426).
        RegisterObjectSetter(426, [](LO* plo, const CRef& r) { SetPadState(plo, Pad(plo), r.RefCoerceS32().m_n); });
        RegisterObjectGetter(426, [](LO* plo) { CRef r; r.SetS32(Pad(plo).state); return r; });
        // EOPID 427: the pad's toucher (+0x2F8)
        RegisterObjectGetter(427, [](LO* plo) {
            CRef r;
            PAD* pp = MpPad().count(plo) ? &MpPad()[plo] : nullptr;
            if (pp && pp->ploFound) r.SetBasic(pp->ploFound);
            else r.SetBasic(nullptr);
            return r;
        });
        // EOPID 759 (0x144068): the actor busy (its AI jobs +0x270 or a script's drive +0x1084)
        RegisterObjectGetter(759, [](LO* plo) {
            CRef r;
            ACTOR* pa = PactorFromLo(plo);
            r.SetBool(pa && (pa->n270 > 0 || pa->prunScript != nullptr));
            return r;
        });
        // EOPIDs 428 / 429 (0x105200 -> 0x1490C0 / 0x105278 -> 0x1490E0): the pad's enter (0x1487A0) / leave
        // (0x148860) actions run by a script (its +0x150 state object); natively stubs (audit 2026-11 P7)
        RegisterObjectMethod(0x105200, [](LO* plo, const std::vector<CRef>&) {
            if (plo && FCidDerivedFrom(plo->Cid(), S3CID(0x53))) EnterPad(plo, Pad(plo));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x105278, [](LO* plo, const std::vector<CRef>&) {
            if (plo && FCidDerivedFrom(plo->Cid(), S3CID(0x53))) LeavePad(plo, Pad(plo));
            return CRef(TAGK_Void);
        });
        // EOPID 730 (0x10A790 -> 0x141968): the actor launched to a point (g the elevation's share, h the air
        // control's weight): the velocity from its feet (+0x248) to the point by its gravity; without a damage
        // reaction the jump kind 8, the air control +0x40 3 toward that velocity's horizontal part, +0x34 h
        RegisterObjectMethod(0x10A790, [](LO* plo, const std::vector<CRef>& a) {
            ACTOR* pa = PactorFromLo(plo);
            if (!pa || a.size() < 3) return CRef(TAGK_Void);
            glm::vec3 posT = a[0].GetVector();
            if (LO* po = PloArg(a, 0); po && FCidDerivedFrom(po->Cid(), S3CID(0x30))) posT = PosAloCenter(static_cast<ALO*>(po));
            const float g = GArg(a, 1), h = GArg(a, 2);
            LaunchActorTo(pa, posT, g, h);  // 0x141968
            return CRef(TAGK_Void);
        });
    }
} s_initpad;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetPads()
{
    MpPad().clear();
    MpPad3().clear();
}
NATIVEFN(0x148F20, FAcceptPadContact);
