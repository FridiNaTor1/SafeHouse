// The guards' four-direction animation player (orig 0x20C7D8..0x20CCD0).
#include "guard.h"
#include "game/sly3.h"

#include "actanim.h"
#include "animfind.h"
#include "cls.h"
#include "clock.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
// orig: 0x0012F938 (the first marker of kind 11 with value n).
float TMarker(ASEG* paseg, int n)
{
    for (const auto& aev : PasegdOf(paseg)->aaev)
        for (const ASEGEV& ev : aev)
            if (ev.kind == 11 && ev.an[0] == n) return ev.t;
    return 0.0f;
}

void DetachListener(K3PLAY* p)
{
    if (!p->pasega) return;
    auto& a = p->pasega->aplistener;
    a.erase(std::remove(a.begin(), a.end(), p), a.end());
}

// orig: 0x001305B8 / 0x001333C0. Remove the old listener before replacing its held ASEGA, so its
// retraction cannot clear the new handle; ApplyAsegCur owns the holder's reference.
void PlayClip(K3PLAY* p, ASEG* paseg, float t)
{
    DetachListener(p);
    ApplyAsegCur(paseg, p->palo, t, 1.0f, p->grf, &p->pasega);
    if (p->pasega) p->pasega->aplistener.push_back(p);
}
}  // namespace

// Native ownership of the original listener/ASEGA handle: ASEGAs are retained across world unloads by
// the animation runtime, so removing a guard's listener never dereferences its former world's ASEG.
K3PLAY::~K3PLAY()
{
    DetachListener(this);
    if (pasega) ReleaseAsega(pasega);
}

void K3PLAY::OnAsegaMsg(ASEGA* p, int msgk)
{
    if (msgk != 2 || p != pasega) return;
    DetachListener(this);
    pasega = nullptr;
    ReleaseAsega(p);  // the holder's reference (the original's handle clears on retraction)
}

// orig: 0x0020C7E0 (0x20CA00 allocates the tuning object; 0x20C7D8 leaves fResolved zero).
void ResolveK3(KIND3* pk, ALO* palo)
{
    if (!pk || pk->fResolved) return;
    for (KIND3::E& e : pk->ae)
    {
        LO* plo = PloFindSwNearest(palo, e.oid);  // 0x15E938, kind 4
        e.paseg = plo && FCidDerivedFrom(plo->Cid(), S3CID(0x6D)) ? static_cast<ASEG*>(plo) : nullptr;
        if (!e.paseg) continue;
        const ASEGCHN* pc = PchnFindAseg(e.paseg, palo, 1, true);
        if (!pc || !pc->ptrkp) continue;
        const float tMax = PasegdOf(e.paseg)->tMax;
        glm::vec4 p0(0.0f), p1(0.0f);
        pc->ptrkp->Eval(&p0, nullptr, 0.0f, 1.0f);
        pc->ptrkp->Eval(&p1, nullptr, tMax, 1.0f);
        if (tMax != 0.0f) e.vel = (p1 - p0) * (1.0f / tMax);
        e.s20 = std::sqrt(e.vel.x * e.vel.x + e.vel.y * e.vel.y + e.vel.z * e.vel.z);
        if (std::getenv("SLY2N_K3DEBUG"))
            std::printf("kind3 resolve: guard %d instance %d clip %d velocity %.3f %.3f %.3f speed %.3f\n",
                        palo->oid, palo->oidInstance, e.paseg->oid, e.vel.x, e.vel.y, e.vel.z, e.s20);
    }
    pk->fResolved = 1;
}

// orig: 0x0020C948 (the initial clip and current entry are the second authored direction).
void StartK3(const KIND3* pk, ALO* palo, int grf, K3PLAY* p)
{
    StopK3(p);
    p->vel = glm::vec4(0.0f);
    p->palo = palo;
    p->ptuning = pk;
    p->pcur = nullptr;
    p->grf = grf;
    p->f34 = p->f38 = 0;
    if (!pk) return;
    PlayClip(p, pk->ae[1].paseg, 0.0f);
    p->pcur = &pk->ae[1];
    if (std::getenv("SLY2N_K3DEBUG"))
        std::printf("kind3 start: t %.3f guard %d instance %d clip %d flags 0x%X\n", g_clock.t, palo->oid,
                    palo->oidInstance, p->pcur->paseg ? p->pcur->paseg->oid : -1, grf);
}

// orig: 0x0020CA48
void StopK3(K3PLAY* p)
{
    if (!p || !p->pasega) return;
    DetachListener(p);
    RetractAsegaPp(&p->pasega);
}

// orig: 0x0020CA98
bool FK3Done(const K3PLAY* p) { return !p || !p->pasega; }

// orig: 0x0020CAA8 (the base listener 0x133590 is empty).
void OnK3Event(K3PLAY* p, const ASEGMSG9* pm)
{
    if (!p || !pm || pm->pasega != p->pasega) return;
    switch (pm->n)
    {
    case 0x17D: p->f34 = 1; break;
    case 0x17E: p->f34 = 0; break;
    case 0x17F: p->f38 = 1; break;
    case 0x180: p->f38 = 0; break;
    default: break;
    }
}

// orig: 0x0020CB28 (current-clip bias 0x2C3DE4 = 1.1, initial best 0x2A8904 = -FLT_MAX).
void SelectK3(K3PLAY* p, const glm::vec3& vel)
{
    if (!p || !p->ptuning || !p->pcur || !p->pasega) return;
    const float l = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    const glm::vec3 dir = l != 0.0f ? vel * (1.0f / l) : glm::vec3(0.0f);
    if (p->f34 || p->f38)
    {
        const KIND3::E* pbest = nullptr;
        float best = -FLT_MAX;
        for (const KIND3::E& e : p->ptuning->ae)
        {
            if (!e.paseg) continue;
            const float len = std::sqrt(e.vel.x * e.vel.x + e.vel.y * e.vel.y + e.vel.z * e.vel.z);
            const glm::vec3 d = len != 0.0f ? glm::vec3(e.vel) * (1.0f / len) : glm::vec3(0.0f);
            float score = dir.x * d.x + dir.y * d.y + dir.z * d.z;
            if (e.paseg == p->pasega->paseg) score *= 1.1f;
            if (best < score) { best = score; pbest = &e; }
        }
        if (pbest && pbest != p->pcur)
        {
            const float t = TMarker(pbest->paseg, p->f34 ? 0x17D : 0x17F);
            if (std::getenv("SLY2N_K3DEBUG"))
                std::printf("kind3 select: t %.3f guard %d instance %d clip %d -> %d at %.3f gates %d/%d\n", g_clock.t,
                            p->palo->oid, p->palo->oidInstance, p->pcur->paseg ? p->pcur->paseg->oid : -1,
                            pbest->paseg->oid, t, p->f34, p->f38);
            PlayClip(p, pbest->paseg, t);
            p->pcur = pbest;
        }
    }
    p->vel = glm::vec4(dir * p->pcur->s20, 0.0f);
}
