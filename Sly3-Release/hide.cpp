// hide.cpp: the hiding spots (cid 0x58; hide.h). Ported from the decomp's matching C: 0x183900, 0x183970, 0x183A50,
// 0x183B00, 0x183B80, 0x183E08.
#include "hide.h"

#include "activate.h"
#include "actor.h"
#include "ainoise.h"
#include "alo.h"
#include "binoc.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "jt.h"
#include "lo.h"
#include "mat.h"
#include "so.h"
#include "sly.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "util.h"
#include "vecmat.h"
#include "vtnative.h"
#include "xf.h"
#include "xfmath.h"
#include "xfphys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

using cmm::F32;

void NotifyActorJumpKind(ACTOR* pa, int n);    // jump.cpp (the owner's vt slot 88)
int OverlapKindAlo(ALO* pa, ALO* pb);          // bbmark.cpp: 0x1F4FC8

namespace
{
// an object's vt slot 3 (the base 0x1A61E0: its splice event n, then the game's event 0x1A (5) / 0x1B (6))
void LoEvent(LO* plo, int n)
{
    HandleLoSpliceEvent(plo, static_cast<uint32_t>(n), false, 0, nullptr);
    if (n == 5 || n == 6) BroadcastTaskEvent(g_psw, n == 5 ? 0x1A : 0x1B, plo);
}

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_HIDEPRINT") != nullptr;
    return s_f;
}

struct HIDE
{
    int state = 0;            // +0x17C
    float t = 0.0f;           // +0x180 the state's start
    ACTOR* pholder = nullptr; // +0x190
};
struct HIDES
{
    SW* psw = nullptr;
    std::unordered_map<const LO*, HIDE> mp;
} s_hides;

HIDE& Hide(const LO* plo)
{
    if (s_hides.psw != g_psw)
    {
        s_hides.mp.clear();
        s_hides.psw = g_psw;
    }
    return s_hides.mp[plo];
}

float GField(const LO* plo, uint32_t ib, uint32_t uDefault)
{
    const uint32_t u = plo->U32Field(0, ib, uDefault);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

// orig 0x001F68A8 (every collision entry's bit 48 (a sensor) set to f)
void SetAloSensors(ALO* palo, bool f)
{
    for (SOE& e : palo->asoe) e.f48 = f;
}

// the holder's record the barrel follows: +0x10BC (a named record), else its object record +0x60
XF* PxfHideAnchor(ACTOR* pa)
{
    for (const auto& [ib, pxf] : pa->palo->apartxf)
        if (ib == 0x10BC && pxf) return pxf;
    return PxfObj(pa->palo);
}
}  // namespace

// orig: 0x00183900 (InitLo: the ALO's (0x1F2108), the description (0x182C70: jt.cpp's defaults), the fields, the
// contact bytes; the load's options may change the fields after it)
void InitHide(LO* plo)
{
    plo->SetField(0, 0x158, 0x43960000u);  // 300
    plo->SetField(0, 0x18C, 1);
    ALO* palo = static_cast<ALO*>(plo);
    SetAloXpBytes(palo, 0x03FFFFFF, 8);  // 0x1F7918
    SetAloXpBytes(palo, 0x03188001, 0);
}

int NHideState(LO* plo) { return plo ? Hide(plo).state : 0; }
float SHideHeight(LO* plo) { return GField(plo, 0x158, 0x43960000u); }
bool FHideFollows(LO* plo) { return plo->U32Field(0, 0x18C, 1) != 0; }
ACTOR* PactorHideHolder(LO* plo) { return plo ? Hide(plo).pholder : nullptr; }
void SetHideHolder(LO* plo, ACTOR* pa) { if (plo) Hide(plo).pholder = pa; }
void ResetHides()
{
    s_hides.mp.clear();
    s_hides.psw = nullptr;
}

// orig: 0x00183B80
void SetHideState(LO* plo, int n)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x58)) return;
    HIDE& h = Hide(plo);
    ALO* palo = static_cast<ALO*>(plo);
    if (h.state == n) return;
    if (FPrint()) std::printf("hide %d: state %d -> %d t %.3f\n", plo->oid, h.state, n, g_clock.t);
    if (h.state == 2)
    {
        if (FHideFollows(plo))
        {
            SetAloParent(palo, nullptr, nullptr, 0xF);  // 0x1A58D0(p, 0, 0xF): vt slot 10
            if (XF* pp = PxfPos(palo))
            {
                SetXfMode(palo, pp, 1);  // 0x2303A0
                pp->state = 0;
            }
            if (XF* pr = PxfRot(palo))
            {
                SetXfMode(palo, pr, 1);
                pr->state = 0;
            }
            SetAloVelocityTo(palo, glm::vec3(0.0f));  // vt slot 35
            SetAloCnstrLin(palo, 0);  // 0x1F5720
            SetAloCnstrAng(palo, 0);  // 0x1F57C0
            SetAloSensors(palo, false);  // 0x1F68A8
        }
        LoEvent(plo, 6);             // vt slot 3 (6, ...)
        UpdateSwActivation(g_psw);   // 0x147F40
        h.pholder = nullptr;
    }
    if (n == 2)
    {
        ACTOR* pa = h.pholder;
        if (FHideFollows(plo) && pa)
        {
            SetAloCnstrLin(palo, 1);
            SetAloCnstrAng(palo, 1);
            XF* ps = PxfHideAnchor(pa);
            SetAloParent(palo, pa->palo, ps, 3);  // vt slot 10 (the world place kept)
            // the rotation record: toward its own frame levelled (0x11B938 of 0x1A6318) in its parent's space, about z
            XF* pr = PxfRot(palo);
            XF* pp = PxfPos(palo);
            if (pr)
            {
                glm::mat3 m(1.0f);
                LevelMatrix3(Mat3Of(MatJtLo(plo)), &m);
                m = MatConvertXfs(nullptr, pr->pxfRef, m);  // 0x22FD38
                const glm::vec3 q = EulerFromMat(m);       // 0x11BFF0
                SetXfMode(palo, pr, 7);
                pr->aseg[0] = glm::vec4(0.0f, 0.0f, q.z, pr->aseg[0].w);
                pr->state = 2;
            }
            if (pp && ps)
            {
                // the position record: the anchor's world position, 50 below (D_002BE8F0), in its parent's space
                const glm::vec3 posA = glm::vec3(ps->matWorld[3]) + glm::vec3(0.0f, 0.0f, F32(0xC2480000));
                const glm::vec3 q2 = PosConvertXfs(nullptr, pp->pxfRef, posA);  // 0x22FC48
                SetXfMode(palo, pp, 7);
                pp->aseg[0] = glm::vec4(q2, pp->aseg[0].w);
                pp->state = 2;
            }
            SetAloSensors(palo, true);
        }
        LoEvent(plo, 5);
    }
    h.state = n;
    h.t = g_clock.t;
}

namespace
{
// orig: 0x00183970 (vt slot 26: the proxy instance's mask (0x1CB518: AttachProxy does it), free)
void ResetHideInstance(LO* plo, LO*)
{
    if (plo && FIsLoDerivedFrom(plo, 0x58)) Hide(plo).state = 0;
}

// orig: 0x00183A50 (vt slot 61: the ALO's overlap kind; no contacts (at least 1) with the player hiding in it
// (interaction kind 1 on it) or jumping into / out of it (the jump's states 6 / 7 with it))
int OverlapKindHide(ALO* p, ALO* pOther)
{
    int k = OverlapKindAlo(p, pOther);
    if (!pOther || !FIsLoDerivedFrom(pOther, 0x34)) return k;
    ACTOR* pa = PactorFromLo(pOther);
    if (!pa) return k;
    if (pa->n4C8 == 1 && pa->inter.ploTarget == p) k = std::max(k, 1);  // 0x134310
    if (static_cast<unsigned>(pa->jump.state - 6) < 2u && pa->jump.ploC == p) k = std::max(k, 1);
    return k;
}

// orig: 0x00183B00 (vt slot 70: the gravity (0x1F9B20); following and free: the rotation pulled level (0x11B938 of
// 0x1A6318) with CLQ (0, 45, 4.5), the angular velocity toward rest with CLQ (0, 7.5, 0.75))
void ApplyHideGravity(ALO* p)
{
    ApplyAloGravity(p);
    if (!FHideFollows(p) || Hide(p).state != 0) return;
    glm::mat3 m(1.0f);
    LevelMatrix3(Mat3Of(MatJtLo(p)), &m);
    static const CLQ s_clq{0.0f, 45.0f, 4.5f, 0.0f}, s_clqV{0.0f, 7.5f, 0.75f, 0.0f};  // D_002BE8D0 / D_002BE8E0
    AccelAloTowardRotation(p, &m, &s_clq, glm::vec3(0.0f), &s_clqV);
}

// orig: 0x00183E08 (vt slot 75: an accepted contact; a hit (its bytes' 8) while not knocked over: the noise
// (0x156B98, the fixed template 0), the holder's binocucom closed (0x19CD38(-1)) and back on the ground (its vt slot 88
// (0)), knocked over (3), its event 0xB)
bool FAcceptHideContact(ALO* p, CTMSG* pctm)
{
    if (!(pctm->grfAccepted != 0)) return false;  // 0x1F7588
    HIDE& h = Hide(p);
    if (h.state == 3) return true;
    if (!(pctm->grfAccepted & 8)) return true;
    if (FPrint()) std::printf("hide %d: hit (bytes 0x%X) holder %d t %.3f\n", p->oid, pctm->grfAccepted, h.pholder ? h.pholder->palo->oid : -1, g_clock.t);
    PnoiseNew(PtmplNoiseFixed(0), PosAloCenter(p), p);  // 0x156B98(G, 0, 0x1A62E8(p), p)
    if (ACTOR* pa = h.pholder)
    {
        SetBinocState(pa, -1);         // 0x19CD38(+0xF00, -1)
        NotifyActorJumpKind(pa, 0);    // vt slot 88 (0)
    }
    SetHideState(p, 3);
    LoEvent(p, 0xB);  // vt slot 3 (0xB, 0, 2, {the message, its +0xC}, 0) (TODO(actors): the event's arguments)
    return true;
}

NATIVEFN(0x183970, ResetHideInstance);
NATIVEFN(0x183A50, OverlapKindHide);
NATIVEFN(0x183B00, ApplyHideGravity);
NATIVEFN(0x183E08, FAcceptHideContact);
}  // namespace

// (debug: SLY2N_HIDEHIT=t, sly.cpp: a hit (contact bytes 8) on the spot the player hides in after game time t)
void DebugHitHide(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x58)) return;
    CTMSG ctm;
    ctm.ploReceiver = plo;
    ctm.grfFilter = 8;
    ctm.grfAccepted = 8;
    FAcceptHideContact(static_cast<ALO*>(plo), &ctm);
}
