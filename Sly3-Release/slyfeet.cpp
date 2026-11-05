// Authored foot reach and floor matching (original 0x189C40 / 0x189EC8).
#include "slyfeet.h"

#include "actor.h"
#include "asega.h"
#include "pocket.h"
#include "smrun.h"
#include "xfphys.h"

#include <algorithm>

// Existing 0x20F948 collision-query predicate, shared with jump trajectory tests.
bool FJumpPathEntry(OXA* own, SOE* entry);

namespace
{
XF* FindRecord(ALO* p, int tag, int kind)
{
    for (const auto& xf : p->axf)
        if (xf->tag == tag && xf->kind == kind) return xf.get();
    return nullptr;
}
}

// orig: 0x00189C40 RebuildJtXmg
void RebuildSlyFootProbe(SLY* p, XF* extrem, float radius, XF* hip, SLYFOOTPROBE* probe)
{
    ASEGA* playing = p->state == 0
        ? ((p->substate >= 6 && p->substate <= 9) || p->substate == 12 || p->substate == 13
            ? p->pasega : (p->psma ? p->psma->pasega : nullptr))
        : (p->psma ? p->psma->pasega : p->pasega);
    if (!playing || !extrem || !hip || !PxfObj(p->palo)) return;
    ASEG* clip = playing->paseg;
    if (!clip || clip == probe->paseg) return;
    // Native ALOPAIR is the original IKREC stored on the upper rotation record.
    ALOPAIR* joint = nullptr;
    for (auto& pair : p->palo->apair)
        if (pair.apxf[1] == hip) { joint = &pair; break; }
    if (!joint || !joint->apxf[0]) return;
    probe->paseg = clip;
    const float at = GEventAseg11(clip, 0x16A);
    glm::mat4 mExtrem, mHip, mGoal;
    CalculateAsegMatrices(clip, p->palo, extrem, PxfObj(p->palo), at, 0.0f, &mExtrem, nullptr);
    const glm::vec3 posExtrem = glm::vec3(mExtrem[3]) + glm::vec3(0.0f, 0.0f, -radius);
    CalculateAsegMatrices(clip, p->palo, hip, PxfObj(p->palo), at, 0.0f, &mHip, nullptr);
    CalculateAsegMatrices(clip, p->palo, joint->apxf[0], PxfObj(p->palo), at, 0.0f, &mGoal, nullptr);
    const glm::vec3 posGoal = glm::vec3(mGoal * glm::vec4(joint->vecA0, 1.0f));
    probe->posSphere = glm::vec3(mHip[3]) + (posExtrem - posGoal);
    probe->sSphere = joint->sUpper + joint->sLower + 10.0f;
    const glm::vec3 up = posExtrem + glm::vec3(0.0f, 0.0f, 50.0f);
    const glm::vec3 down = posExtrem + glm::vec3(0.0f, 0.0f, -50.0f);
    LSG clipped;
    probe->valid = ClipSegmentToSphere(probe->posSphere, up, down, 1, &clipped, probe->sSphere) > 0;
    probe->posExtrem = posExtrem;
    probe->posUp = clipped.pos0;
    probe->posDown = clipped.pos1;
    probe->posSphereLow = probe->posSphere + glm::vec3(0.0f, 0.0f, -probe->sSphere);
}

// orig: 0x00189EC8 FMatchJtXmg
bool MatchSlyFootProbe(SLY* p, SLYFOOTPROBE* probe, CTRLSOURCE* handle)
{
    if (!probe->valid || !handle) return true;
    XF* root = PxfObj(p->palo);
    const glm::vec3 a = PosConvertXfs(root, nullptr, probe->posUp);
    const glm::vec3 b = PosConvertXfs(root, nullptr, probe->posDown);
    const glm::vec3 c = PosConvertXfs(root, nullptr, probe->posSphereLow);
    const glm::vec3 lo = glm::min(a, glm::min(b, c));
    const glm::vec3 hi = glm::max(a, glm::max(b, c));
    std::vector<SOE*> entries;
    FlushSwOxaDirty1();
    UpdateSwAaox();
    for (OXA* box : g_physw.apoxaActive)
    {
        if (hi.x < box->posMin.x || box->posMax.x < lo.x || hi.y < box->posMin.y || box->posMax.y < lo.y ||
            hi.z < box->posMin.z || box->posMax.z < lo.z) continue;
        for (SOE* entry : box->apsoe)
            if (entries.size() < 0x100 && FJumpPathEntry(Poxa(p->palo), entry)) entries.push_back(entry);
    }
    LSG hit;
    if ((PsoeHitTestLineObjects(0, a, b, entries, &hit) || PsoeHitTestLineObjects(0, b, c, entries, &hit)) &&
        hit.t0 != 0.0f && 0.7f < hit.plane0.z)
    {
        const glm::vec3 local = PosConvertXfs(nullptr, root, hit.pos0 - glm::vec3(0.0f, 0.0f, 1.0f));
        SetCtrlSourceOffset(handle, glm::vec4(local - probe->posExtrem, 0.0f));
        return true;
    }
    SetCtrlSourceOffset(handle, glm::vec4(0.0f));
    return !(p->pxpFloor && probe->posDown.z < -85.0f);
}

// orig: 0x0018BCE0, state 24
void UpdateSlySlideFeet(SLY* p)
{
    if (!p->slide.aphandle[3]) return;
    RebuildSlyFootProbe(p, FindRecord(p->palo, 0x13, 1), 20.0f, FindRecord(p->palo, 0x18, 6), &p->slide.afoot[0]);
    RebuildSlyFootProbe(p, FindRecord(p->palo, 0x12, 1), 20.0f, FindRecord(p->palo, 0x19, 6), &p->slide.afoot[1]);
    (void)MatchSlyFootProbe(p, &p->slide.afoot[0], p->slide.aphandle[3]);
    (void)MatchSlyFootProbe(p, &p->slide.afoot[1], p->slide.aphandle[4]);
}
