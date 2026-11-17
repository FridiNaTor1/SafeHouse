// The guards' four-direction animation player (orig 0x20C7D8..0x20CCD0; orig3 0x259330..0x259678: Sly 3 rewrote the
// clip switch: marker windows read at resolve and the clip's phase, no ASEG-event listener).
#include "guard.h"
#include "pocket.h"  // GEventAseg11 (sly.cpp)
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
        // orig3: 0x2592A8 twice: the windows between the markers 0x19B / 0x19C and 0x19D / 0x19E (0x1368E0 =
        // GEventAseg11), the end wrapped by the clip's length
        const auto window = [&](float* pw, int oid0, int oid1) {
            const float t0 = GEventAseg11(e.paseg, oid0);
            float t1 = GEventAseg11(e.paseg, oid1);
            if (t1 < t0) t1 += tMax;
            pw[0] = t0;
            pw[1] = t1 - t0;
        };
        window(e.aw24, 0x19B, 0x19C);
        window(e.aw2C, 0x19D, 0x19E);
        if (std::getenv("SLY2N_K3DEBUG"))
            std::printf("kind3 resolve: guard %d instance %d clip %d velocity %.3f %.3f %.3f speed %.3f windows %.3f+%.3f %.3f+%.3f\n",
                        palo->oid, palo->oidInstance, e.paseg->oid, e.vel.x, e.vel.y, e.vel.z, e.s20, e.aw24[0], e.aw24[1],
                        e.aw2C[0], e.aw2C[1]);
    }
    pk->fResolved = 1;
}

// orig: 0x0020C948 (Sly 2: the initial clip and current entry are the second authored direction).
// orig3: 0x2594E0 starts with the first entry (the record's +0 clip; peCur = its +0).
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
    PlayClip(p, pk->ae[0].paseg, 0.0f);
    p->pcur = &pk->ae[0];
    if (std::getenv("SLY2N_K3DEBUG"))
        std::printf("kind3 start: t %.3f guard %d instance %d clip %d flags 0x%X\n", g_clock.t, palo->oid,
                    palo->oidInstance, p->pcur->paseg ? p->pcur->paseg->oid : -1, grf);
}

// orig: 0x0020CA48 (orig3: 0x2595D8, the same C in the Sly 3 decomp)
void StopK3(K3PLAY* p)
{
    if (!p || !p->pasega) return;
    DetachListener(p);
    RetractAsegaPp(&p->pasega);
}

// orig: 0x0020CA98
bool FK3Done(const K3PLAY* p) { return !p || !p->pasega; }

// orig: 0x0020CAA8 (Sly 2's gates from the ASEG events 0x17D..0x180). Sly 3 has no such listener (0x259678 reads
// the clip's phase): nothing to do.
void OnK3Event(K3PLAY*, const ASEGMSG9*) {}

namespace
{
// orig3: 0x259638: the current clip's phase in a window {start, length}: (t - start, wrapped by the clip's length)
// / length
float GPhaseK3(const K3PLAY* p, const float* pw)
{
    float t = p->pasega->t;
    const ASEG* paseg = p->pasega->paseg;
    if (t < pw[0] && paseg) t += PasegdOf(const_cast<ASEG*>(paseg))->tMax;
    t -= pw[0];
    return pw[1] != 0.0f ? t / pw[1] : (t > 0.0f ? FLT_MAX : -FLT_MAX);
}
}  // namespace

// orig: 0x0020CB28 (current-clip bias 0x2C3DE4 = 1.1, initial best 0x2A8904 = -FLT_MAX).
// orig3: 0x259678: a change only while the current clip's phase in either window is inside D_00341FF0 (0, 1)
// (0x11EE30, exclusive); the new clip starts at the same phase of its own window (the first window when both)
void SelectK3(K3PLAY* p, const glm::vec3& vel)
{
    if (!p || !p->ptuning || !p->pcur || !p->pasega) return;
    const float l = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    const glm::vec3 dir = l != 0.0f ? vel * (1.0f / l) : glm::vec3(0.0f);
    const float g0 = GPhaseK3(p, p->pcur->aw24), g1 = GPhaseK3(p, p->pcur->aw2C);
    const bool f0 = 0.0f < g0 && g0 < 1.0f, f1 = 0.0f < g1 && g1 < 1.0f;
    if (f0 || f1)
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
            const float g = f0 ? g0 : g1;
            const float* pw = f0 ? pbest->aw24 : pbest->aw2C;
            float t = pw[0] + g * pw[1];
            const float tMax = PasegdOf(pbest->paseg)->tMax;
            if (tMax < t) t -= tMax;
            if (std::getenv("SLY2N_K3DEBUG"))
                std::printf("kind3 select: t %.3f guard %d instance %d clip %d -> %d at %.3f phases %.3f/%.3f\n", g_clock.t,
                            p->palo->oid, p->palo->oidInstance, p->pcur->paseg ? p->pcur->paseg->oid : -1,
                            pbest->paseg->oid, t, g0, g1);
            PlayClip(p, pbest->paseg, t);
            p->pcur = pbest;
        }
    }
    p->vel = glm::vec4(dir * p->pcur->s20, 0.0f);
}
