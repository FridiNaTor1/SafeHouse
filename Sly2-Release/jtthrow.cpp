#include "jtthrow.h"

#include "actmesh.h"
#include "actor.h"
#include "activate.h"
#include "alo.h"
#include "asega.h"
#include "blend.h"
#include "brx.h"
#include "carry.h"
#include "clock.h"
#include "cm.h"
#include "gadget.h"
#include "gang.h"
#include "jt.h"
#include "jump.h"
#include "objmethod.h"
#include "po.h"
#include "reach.h"
#include "smrun.h"
#include "so.h"
#include "stepphys.h"
#include "sw.h"
#include "trail.h"
#include "vecmat.h"
#include "vtnative.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern SW* g_psw;
bool FJumpPathEntry(OXA* poxaOwn, SOE* psoe);  // 0x20F948, jumptgt.cpp
void FadeAloOut(ALO* palo, float dt);           // 0x1F7BF8, asegevent.cpp

namespace
{
constexpr uint32_t IB = 0x11B0;  // (the players' shared controller; Murray's second one is at +0x1910: JTTHROW::ib)

LO* Part(const JTTHROW& t, uint32_t offset)
{
    for (const auto& [ib, object] : t.owner->palo->apart)
        if (ib == t.ib + offset) return object;
    return nullptr;
}

ASEG* Clip(const JTTHROW& t, uint32_t offset) { return dynamic_cast<ASEG*>(Part(t, offset)); }

float Field(const JTTHROW& t, uint32_t offset, float fallback)
{
    return std::bit_cast<float>(t.owner->palo->U32Field(0, t.ib + offset, std::bit_cast<uint32_t>(fallback)));
}

glm::vec3 VectorField(const JTTHROW& t, uint32_t offset, const glm::vec3& fallback)
{
    return {Field(t, offset, fallback.x), Field(t, offset + 4, fallback.y), Field(t, offset + 8, fallback.z)};
}

void LoadParameters(JTTHROW& t)
{
    t.radTurnHold = Field(t, 0x14, t.radTurnHold);
    t.radTurnAim = Field(t, 0x18, t.radTurnAim);
    t.sAimMin = Field(t, 0x1C, t.sAimMin);
    t.sAimMax = Field(t, 0x20, t.sAimMax);
    t.tPredict = Field(t, 0x24, t.tPredict);
    t.inputScale = Field(t, 0x28, t.inputScale);
    t.angularVelocity = VectorField(t, 0x30, t.angularVelocity);
    t.targetLocal = VectorField(t, 0xC0, t.targetLocal);
    t.launchLocal = VectorField(t, 0xD0, t.launchLocal);
}

// orig: 0x10B230 / 0x142310, EOPID760. This authored method supplies the throw
// parameters; they are not individual field options. Native actor initialization
// follows BRX loading, so retain the method's stores for InitJtThrow to restore.
struct RegisterThrowParameters
{
    RegisterThrowParameters()
    {
        RegisterObjectMethod(0x10B230, [](LO* object, const std::vector<CRef>& args) {
            if (!FIsLoDerivedFrom(object, 0x34) || args.size() < 7) return CRef(TAGK_Void);
            auto store = [&](uint32_t offset, float value) {
                object->SetField(0, IB + offset, std::bit_cast<uint32_t>(value));
            };
            store(0x14, GArg(args, 0));
            store(0x18, GArg(args, 1));
            store(0x1C, args[2].m_vector.x);
            store(0x20, args[2].m_vector.y);
            store(0x24, GArg(args, 3));
            store(0x28, GArg(args, 4));
            for (int i = 0; i < 3; ++i)
            {
                store(0xD0 + 4 * i, args[5].m_vector[i]);
                store(0x30 + 4 * i, args[6].m_vector[i]);
            }
            if (ACTOR* actor = dynamic_cast<ACTOR*>(PstepFromLo(object)); actor && actor->jtthrow.owner)
                LoadParameters(actor->jtthrow);
            return CRef(TAGK_Void);
        });
    }
} s_registerThrowParameters;

float Weight(ASEGBL* blend, size_t i)
{
    return blend && i < blend->abl.size() ? blend->abl[i].weight : 0.0f;
}

PADCTL* Pad(ACTOR* actor)
{
    PODATA* data = PpodataFromLo(actor->palo);
    return data ? data->ppadctl : nullptr;
}

bool Take(PADCTL* pad, uint32_t buttons)
{
    return pad && GrfPadctlTakePressed(pad, buttons, 0.1f);
}

// The global Bentley is represented natively by the one initialized Bentley object.
BENTLEY* SelectedBentley(const JTTHROW& t)
{
    return t.selected ? dynamic_cast<BENTLEY*>(PstepFromLo(t.selected)) : nullptr;
}

bool SetThrownBentleyState(BENTLEY* bentley, int state)
{
    SetBentleyCarried(bentley, state);  // 0x136530 (gang.cpp)
    return true;
}

// The selected Bentley's cancelled launch in 0x21F418 / 0x2206C8.
void CancelBentleyThrow(ACTOR* actor, BENTLEY* bentley)
{
    glm::vec3 direction = PosJtLo(bentley->palo) - PosJtLo(actor->palo);
    direction.z = 0.0f;
    const float length = glm::length(direction);
    direction = length < 0.0001f ? glm::vec3(1, 0, 0) : direction / length;
    if (SetThrownBentleyState(bentley, 1))
        SetAloVelocityTo(bentley->palo, direction * 400.0f + glm::vec3(0, 0, 600.0f));
}

glm::vec3 ArcPoint(const glm::vec3& start, const glm::vec3& acceleration,
                   const glm::vec3& velocity, float time)
{
    return start + velocity * time + acceleration * (time * (time * 0.5f));
}

// orig: 0x0021FB18: water always participates, otherwise the ordinary trajectory pair filter.
bool ArcEntry(OXA* own, SOE* entry)
{
    if (entry->palo && FIsLoDerivedFrom(entry->palo, 0x59)) return true;
    return FJumpPathEntry(own, entry);
}

void RemoveMarker(JTTHROW& t)
{
    if (t.marker) RemoveLo(g_psw, t.marker);
}

void LogState(const JTTHROW& t, int old)
{
    if (!std::getenv("SLY2N_THROWPRINT")) return;
    const ACTOR* actor = t.owner;
    std::printf("throw state owner=%d ib=0x%X old=%d state=%d time=%.9g selected=%d power=%d\n",
                actor->palo->oid, t.ib, old, t.state, g_clock.t, t.selected ? t.selected->oid : -1,
                NActorPower(const_cast<ACTOR*>(actor)));
}
}  // namespace

// orig: 0x0021F2E0. Field options are applied after construction in the original.
void InitJtThrow(ACTOR* actor, JTTHROW& t, uint32_t ib)
{
    t = JTTHROW{};
    t.owner = actor;
    t.ib = ib;
    LoadParameters(t);
}
void InitJtThrow(ACTOR* actor) { InitJtThrow(actor, actor->jtthrow, IB); }

// orig: 0x0021F388, after 0x1400B8 has bound the actor's authored part table.
void PostLoadJtThrow(JTTHROW& t)
{
    ACTOR* actor = t.owner;
    t.peer = Part(t, 0xC);
    t.holdBlend = PblNewPlain(actor->palo, {Clip(t, 0x44), Clip(t, 0x50)});
    t.holdTurn[0] = Clip(t, 0x5C);
    t.holdTurn[1] = Clip(t, 0x60);
    t.pickup = Clip(t, 0x64);
    t.aimBlend = PblNewPlain(actor->palo, {Clip(t, 0x6C), Clip(t, 0x78), Clip(t, 0x84), Clip(t, 0x90)});
    t.aimTurn[0] = Clip(t, 0x9C);
    t.aimTurn[1] = Clip(t, 0xA0);
    t.throwBlend = PblNewPlain(actor->palo, {Clip(t, 0xA8), Clip(t, 0xB4)});
    t.marker = Part(t, 0x12C);
}
void PostLoadJtThrow(ACTOR* actor)
{
    actor->jtthrow.owner = actor;
    PostLoadJtThrow(actor->jtthrow);
}

// orig: 0x001D6848. The elevation is a sine fraction of the angle from the line of sight to vertical.
glm::vec3 JtThrowLaunchVelocity(const glm::vec3& start, const glm::vec3& target,
                               const glm::vec3& acceleration, float elevation)
{
    glm::vec3 delta = target - start;
    const float height = delta.z;
    delta.z = 0.0f;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    float angle = std::atan2(height, distance);
    if (angle < 0.0f) angle = 0.0f;
    angle = angle + (1.57079637f - angle) * std::sin(elevation);
    float rate = acceleration.z / ((height - distance * std::tan(angle)) * 2.0f);
    if (rate <= 0.0f) return glm::vec3(0.0f);
    rate = std::sqrt(rate);
    const float speed = distance * rate / std::cos(angle);
    const glm::vec3 direction(delta.x, delta.y, distance * std::tan(angle));
    const float length = glm::length(direction);
    return length > 0.0f ? (direction * (1.0f / length)) * speed : glm::vec3(0.0f);
}

// orig: 0x0021FB70. Preserve the authored launch curve, filtered geometry, and temporal limits.
void UpdateJtThrowArc(JTTHROW& t, bool updateBlends)
{
    ACTOR* actor = t.owner;
    ALO* selected = dynamic_cast<ALO*>(t.selected);
    SOST* mass = selected ? Psost(selected) : nullptr;
    if (!mass) return;
    const glm::vec3 start = PosJtConvert(actor->palo, nullptr, t.launchLocal);
    const glm::vec3 delta = t.targetWorld - PosJtLo(actor->palo);
    const glm::vec3 acceleration = mass->dvGravity;
    float far = std::clamp((std::sqrt(delta.x * delta.x + delta.y * delta.y) - t.sAimMin) /
                          (t.sAimMax - t.sAimMin), 0.0f, 1.0f);
    // D_002C40F0 = {0.15, 0.55, 0}; the original CLQ is evaluated in this order.
    const float elevation = 0.15f + far * (0.55f + far * 0.0f);
    const glm::vec3 velocity = JtThrowLaunchVelocity(start, t.targetWorld, acceleration, elevation);
    const float apexTime = std::max(-velocity.z / acceleration.z, 0.0f);
    const glm::vec3 apex = ArcPoint(start, acceleration, velocity, apexTime);
    float time = FPowerupOwned(36) ? 1.35f * t.tPredict : t.tPredict;
    const glm::vec3 end = ArcPoint(start, acceleration, velocity, time);
    const glm::vec3 minimum = glm::min(start, glm::min(apex, end));
    const glm::vec3 maximum = glm::max(start, glm::max(apex, end));
    std::vector<SOE*> entries;
    OXA* own = Poxa(selected);
    FlushSwOxaDirty1();
    UpdateSwAaox();
    for (OXA* attachment : g_physw.apoxaActive)
    {
        if (maximum.x < attachment->posMin.x || attachment->posMax.x < minimum.x ||
            maximum.y < attachment->posMin.y || attachment->posMax.y < minimum.y ||
            maximum.z < attachment->posMin.z || attachment->posMax.z < minimum.z) continue;
        for (SOE* entry : attachment->apsoe)
            if (ArcEntry(own, entry) && entries.size() < 0x100) entries.push_back(entry);
    }
    t.velocity = velocity;
    const float interval = time * 0.034482758f;
    glm::vec3 previous = start;
    for (unsigned i = 1; i < 29; ++i)
    {
        time = static_cast<float>(i) * interval;
        const glm::vec3 point = ArcPoint(start, acceleration, velocity, time);
        LSG hit;
        if (PsoeHitTestLineObjects(1, previous, point, entries, &hit))
        {
            float roots[2];
            const int count = cmm::CSolveQuadratic(acceleration.z * 0.5f, velocity.z, start.z - hit.pos0.z, roots);
            float bestTime = 0.0f, bestDistance = FLT_MAX;
            for (int k = 0; k < count; ++k)
            {
                if (roots[k] < 0.0f) continue;
                const float distance = glm::length(ArcPoint(start, acceleration, velocity, roots[k]) - hit.pos0);
                if (distance < bestDistance) bestDistance = distance, bestTime = roots[k];
            }
            time = bestTime;
            break;
        }
        previous = point;
    }
    const float maxChange = 3.0f * g_clock.dt;
    t.tFlight = std::clamp(time, t.tFlight - maxChange, t.tFlight + maxChange);
    std::vector<glm::vec3> points;
    time = t.tFlight;
    for (unsigned i = 0; time >= 0.1f && i < 29;)
    {
        points.push_back(ArcPoint(start, acceleration, velocity, time));
        ++i;
        time = t.tFlight - static_cast<float>(i) * interval;
    }
    points.push_back(start);
    if (t.marker) SetThrowArcFrames(t.marker, points, start);

    const glm::vec3 aim = t.targetWorld - PosJtLo(actor->palo);
    const float angle = cmm::RadNormalize(std::atan2(aim.y, aim.x) - RadStepHeading(actor));
    float turn = std::clamp(angle / t.radTurnAim, -1.0f, 1.0f) * 0.5f + 0.5f;
    if (updateBlends)
    {
        const float oldFar = Weight(t.throwBlend, 1);
        const float oldTurn = oldFar > 0.5f ? Weight(t.aimBlend, 3) / oldFar :
                                            Weight(t.aimBlend, 1) / (1.0f - oldFar);
        const float change = 2.0f * g_clock.dt;
        far = std::clamp(far, oldFar - change, oldFar + change);
        turn = std::clamp(turn, oldTurn - change, oldTurn + change);
    }
    const float opposite = 1.0f - turn, near = 1.0f - far;
    SetBlendWeights(t.aimBlend, {opposite * near, turn * near, opposite * far, turn * far});
    SetBlendWeights(t.throwBlend, {near, far});
}

// orig: 0x002206C8.
void SetJtThrowState(JTTHROW& t, int state)
{
    ACTOR* actor = t.owner;
    if (state == t.state) return;
    const int old = t.state;
    if (old == -1)
    {
        if (!t.selected) return;
        if (!SelectedBentley(t)) AttachReachObject(&actor->inter.reach, t.selected);
        if (PcarryFromLo(t.selected)) SetCarryState(t.selected, 0);
        actor->vecVel = glm::vec3(0.0f);
    }
    else if (old == 3 && state != 4)
    {
        if (BENTLEY* bentley = SelectedBentley(t)) EnablePo(g_psw, bentley->palo, 1);
    }
    t.state = state;
    t.time = g_clock.t;
    RecalcSwOxfFilterForObject(actor->palo);
    if (state == -1)
    {
        SetStepPhysHeading(actor->palo, RadStepHeading(actor));
        actor->nMotion = 0;
    }
    else if (state == 7)
    {
        SetStepPhysHeadingToward(actor->palo, t.targetWorld - PosJtLo(actor->palo));
        actor->nMotion = 0;
    }
    else
    {
        actor->posTarget = glm::vec4(0.0f);
        actor->nMotion = 1;
    }
    ASEG* clip = nullptr;
    int flags = 1;
    switch (state)
    {
    case -1:
        // (the global Bentley still held: his carried controller +0x1800 in its state 0)
        if (BENTLEY* bentley = SelectedBentley(t); bentley && bentley->n1800 == 0)
            CancelBentleyThrow(actor, bentley);
        else if (t.selected && t.peer && t.selected->paloParent == t.peer->paloParent)
        {
            if (PcarryFromLo(t.selected)) SetCarryState(t.selected, 4);
            else if (ALO* object = dynamic_cast<ALO*>(t.selected))
            {
                SetAloParent(object, nullptr, nullptr, 15);
                FadeAloOut(object, 0.25f);
            }
        }
        ReleaseReachObject(&actor->inter.reach);
        RemoveMarker(t);
        t.selected = nullptr;
        LogState(t, old);
        return;
    case 0:
        if (old != 1 && old != 2) SetBlendWeights(t.holdBlend, {0.5f, 0.5f});
        clip = t.holdBlend;
        flags = 9;
        break;
    case 1: clip = t.holdTurn[0]; break;
    case 2: clip = t.holdTurn[1]; break;
    case 3:
        clip = t.pickup;
        EnablePo(g_psw, actor->palo, 1);
        break;
    case 4:
        if (t.marker) BeginThrowArc(t.marker);
        if (old != 5 && old != 6)
        {
            t.tFlight = 0.0f;
            t.targetWorld = PosJtConvert(actor->palo, nullptr, t.targetLocal);
            UpdateJtThrowArc(t, false);
        }
        clip = t.aimBlend;
        flags = 9;
        t.transitioning = false;
        break;
    case 5: clip = t.aimTurn[0]; break;
    case 6: clip = t.aimTurn[1]; break;
    case 7:
        clip = t.throwBlend;
        if (CARRY* carry = PcarryFromLo(t.selected)) SpendActorPower(actor, carry->kind);
        break;
    }
    ClearSmaHandle(&actor->psma);
    ApplyAsegCur(clip, actor->palo, 0.0f, 1.0f, flags, &actor->pasega);
    UpdateActorStepMesh(actor);
    RecalcSwOxfFilterForObject(actor->palo);
    LogState(t, old);
}

// orig: 0x0021F418 (called by the player's transitions, once per frame).
void UpdateJtThrow(JTTHROW& t)
{
    ACTOR* actor = t.owner;
    PADCTL* pad = Pad(actor);
    const glm::vec3 ownerPosition = PosJtLo(actor->palo);
    if (t.state >= 4 && t.state < 7)
    {
        if (t.transitioning)
        {
            const float end = t.transitionStart + t.transitionDuration;
            if (end <= g_clock.t) t.transitioning = false, t.targetWorld = t.transitionEnd;
            else
            {
                const float blend = (g_clock.t - t.transitionStart) / (end - t.transitionStart);
                t.targetWorld = t.transitionBegin * (1.0f - blend) + t.transitionEnd * blend;
            }
        }
        else
        {
            const float maximum = FPowerupOwned(36) ? t.sAimMax * 1.5f : t.sAimMax;
            glm::vec3 delta = t.targetWorld - ownerPosition;
            delta.z = 0.0f;
            const float advance = (pad ? pad->uJoy : 0.0f) * g_clock.dt * t.inputScale;
            delta += cmm::SetVectorCylind(pad ? pad->radJoy : 0.0f, advance, 0.0f);
            const float radius = std::clamp(std::sqrt(delta.x * delta.x + delta.y * delta.y), t.sAimMin, maximum);
            t.targetWorld = ownerPosition + cmm::SetVectorCylind(std::atan2(delta.y, delta.x), radius, 0.0f);
            // (test only: SLY2N_THROWTARGET="X Y[@T][;X Y@T..]" holds the aim arrow at that ground point, at the owner's
            // height; an entry with @T applies from game clock T on)
            static const char* s_szTarget = std::getenv("SLY2N_THROWTARGET");
            bool fTarget = false;
            float x = 0.0f, y = 0.0f;
            for (const char* q = s_szTarget; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                float x0, y0, tFrom = 0.0f;
                const int c = std::sscanf(q, "%f %f@%f", &x0, &y0, &tFrom);
                if (c >= 2 && (c < 3 || g_clock.t >= tFrom)) x = x0, y = y0, fTarget = true;
            }
            if (fTarget) t.targetWorld = glm::vec3(x, y, ownerPosition.z);
        }
        UpdateJtThrowArc(t, true);
    }
    for (;;)
    {
        const int old = t.state;
        ASEGA* animation = actor->pasega;
        switch (old)
        {
        case 0:
            if (t.selected)
            {
                const glm::vec3 delta = PosJtLo(t.selected) - ownerPosition;
                const float angle = cmm::RadNormalize(std::atan2(delta.y, delta.x) - RadStepHeading(actor));
                if (t.radTurnHold < angle) SetJtThrowState(t, 2);
                else if (angle < -t.radTurnHold) SetJtThrowState(t, 1);
                else
                {
                    const float right = angle / t.radTurnHold * 0.5f + 0.5f;
                    SetBlendWeights(t.holdBlend, {1.0f - right, right});
                }
            }
            break;
        case 1: case 2:
            if (!animation || FAsegaRetracted(animation)) SetJtThrowState(t, 0);
            break;
        case 3:
            if (!animation || FAsegaRetracted(animation))
            {
                BENTLEY* bentley = SelectedBentley(t);
                SetJtThrowState(t, bentley && bentley->state == 8 ? 4 : 0);
            }
            break;
        case 4: case 5: case 6:
            if (CARRY* carry = PcarryFromLo(t.selected); carry && carry->n158 != -1)
            {
                NotifyActorJumpKind(actor, 0);
                break;
            }
            if (Take(pad, 0xC0))
            {
                SetJtThrowState(t, 7);
                break;
            }
            if (Take(pad, 0x30))
            {
                if (t.holdBlend) SetJtThrowState(t, 0);
                else NotifyActorJumpKind(actor, 0);
                if (BENTLEY* bentley = SelectedBentley(t); bentley && bentley->state == 8)
                    CancelBentleyThrow(actor, bentley);
                RemoveMarker(t);
                return;
            }
            if (!animation || FAsegaRetracted(animation)) SetJtThrowState(t, 4);
            else if (t.state == 4)
            {
                const glm::vec3 delta = t.targetWorld - ownerPosition;
                const float angle = cmm::RadNormalize(std::atan2(delta.y, delta.x) - RadStepHeading(actor));
                if (t.radTurnAim < angle) SetJtThrowState(t, 6);
                else if (angle < -t.radTurnAim) SetJtThrowState(t, 5);
            }
            break;
        case 7:
            if (!animation || FAsegaRetracted(animation))
            {
                if (t.holdBlend) SetJtThrowState(t, 0);
                else NotifyActorJumpKind(actor, 0);
            }
            break;
        }
        if (t.state == old || t.state == -1) break;
    }
    if (actor->nMotion != 1) return;
    ASEGA* animation = actor->pasega;
    const ASEGCHN* channel = animation ? PchnFindAseg(animation->paseg, actor->palo, 6, true) : nullptr;
    if (channel && channel->ptrkp)
    {
        glm::vec4 velocity(0.0f);
        channel->ptrkp->Eval(nullptr, &velocity, animation->t, animation->svt);
        actor->posTarget = glm::vec4(0.0f, 0.0f, velocity.z, 0.0f);
    }
    else actor->posTarget = glm::vec4(0.0f);
    if (const char* sz = std::getenv("SLY2N_THROWPRINT"); sz && *sz == '2')
    {
        const XF* pxfRot = PxfRot(actor->palo);
        std::printf("throw motion owner=%d ib=0x%X state=%d t=%.4f channel=%d target=%.4f heading=%.4f rotvel=%.4f clip=%d aim %.1f %.1f %.1f vel %.1f %.1f %.1f cam %.3f %.3f pad %.3f %.3f\n", actor->palo->oid, t.ib,
                    t.state, g_clock.t, channel && channel->ptrkp ? 1 : 0, actor->posTarget.z, RadStepHeading(actor),
                    pxfRot ? pxfRot->vel.z : -99.0f, animation && animation->paseg ? animation->paseg->oid : -1,
                    t.targetWorld.x, t.targetWorld.y, t.targetWorld.z, t.velocity.x, t.velocity.y, t.velocity.z, g_cm.mat[0].x, g_cm.mat[0].y,
                    pad ? pad->radJoy : 0.0f, pad ? pad->uJoy : 0.0f);
    }
}

// orig: 0x00220508 (while aiming, states 4..6: the aim point moved to the object's position over |aim - it| / +0x28
// seconds; SCPT 0x59 / 0x5A aim Murray's Bentley toss, e.g. w32's 2343 at the battery)
void SetJtThrowTarget(JTTHROW& t, LO* plo)
{
    if (t.state >= 7 || t.state < 4 || !plo) return;
    const glm::vec3 pos = PosJtLo(plo);  // 0x1A62E8
    const glm::vec3 d = t.targetWorld - pos;
    t.transitionEnd = pos;
    t.transitionBegin = t.targetWorld;
    t.transitioning = true;
    t.transitionStart = g_clock.t;
    t.transitionDuration = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / t.inputScale;
}

// orig: 0x002205A8, only after the owner animation's event 0x27B.
void ReleaseJtThrow(JTTHROW& t)
{
    ACTOR* actor = t.owner;
    ALO* selected = dynamic_cast<ALO*>(t.selected);
    if (!selected) return;
    bool release = true;
    if (BENTLEY* bentley = SelectedBentley(t))
        release = bentley->state == 8 && SetThrownBentleyState(bentley, 1);
    else
    {
        SetAloParent(selected, nullptr, nullptr, 3);
        SetAloCnstrLin(selected, 0);
        SetAloCnstrAng(selected, 0);
    }
    if (release)
    {
        SetAloVelocityTo(selected, t.velocity);
        SetAloAngVelocityTo(selected, VecJtConvert(actor->palo, nullptr, t.angularVelocity));
        if (PcarryFromLo(selected)) SetCarryState(selected, 1);
        if (std::getenv("SLY2N_THROWPRINT"))
            std::printf("throw release owner=%d selected=%d time=%.9g velocity=%.6f,%.6f,%.6f gravity %.3f owner at %.2f %.2f %.2f target %.2f %.2f %.2f\n",
                        actor->palo->oid, selected->oid, g_clock.t, t.velocity.x, t.velocity.y, t.velocity.z,
                        Psost(selected) ? Psost(selected)->dvGravity.z : 0.0f, PosJtLo(actor->palo).x, PosJtLo(actor->palo).y,
                        PosJtLo(actor->palo).z, t.targetWorld.x, t.targetWorld.y, t.targetWorld.z);
    }
    RemoveMarker(t);
}

// orig: 0x00220498 (the controller's selected object, held from the state 3 on, or the global Bentley in his jump
// state 18: no contact response, 3 (0x134310))
int OverlapKindJtThrow(const JTTHROW& t, ALO* pOther, int kind)
{
    if (pOther == t.selected && t.state != -1)
    {
        const BENTLEY* pb = PbentleyFromLo(pOther);
        if (t.state >= 3 || (pb && pb->jump.state == 0x12)) kind = std::max(kind, 3);  // 0x134310
    }
    return kind;
}

// orig: 0x00144388 (the players' vt slot 61, the overlap kind with another object): the ALO's (0x1F4FC8), then the
// throw controller's 0x220498 (its selected object, held from the state 3 on, or the global Bentley in his jump
// state 18: no contact response, 3) and the reach's 0x1CC9A8 (+0x900: in state 1, the target's attachment: 3)
int OverlapKindActor(ALO* palo, ALO* pOther)
{
    using PFN = int (*)(ALO*, ALO*);
    PFN pfnBase = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8));
    int kind = pfnBase ? pfnBase(palo, pOther) : 0;
    ACTOR* pa = dynamic_cast<ACTOR*>(PstepFromLo(palo));
    if (!pa) return kind;
    // 0x220498 (+0x11B0)
    kind = OverlapKindJtThrow(pa->jtthrow, pOther, kind);
    // 0x1CC9A8 (+0x900)
    const REACH& r = pa->inter.reach;
    if (r.state == 1 && r.ploTarget)
    {
        LO* pt = r.ploTarget;  // 0x1A5BF0: itself when an ALO, else its parent
        ALO* palot = FCidDerivedFrom(pt->Cid(), 0x30) ? static_cast<ALO*>(pt) : pt->paloParent;
        if (palot && Poxa(palot) && Poxa(palot) == Poxa(pOther)) kind = std::max(kind, 3);
    }
    return kind;
}
NATIVEFN(0x144388, OverlapKindActor);

void UpdateJtThrowArc(ACTOR* actor, bool updateBlends) { UpdateJtThrowArc(actor->jtthrow, updateBlends); }
void SetJtThrowState(ACTOR* actor, int state) { SetJtThrowState(actor->jtthrow, state); }
void UpdateJtThrow(ACTOR* actor) { UpdateJtThrow(actor->jtthrow); }
void ReleaseJtThrow(ACTOR* actor) { ReleaseJtThrow(actor->jtthrow); }

// orig: 0x00220388 (Murray's camera in his state 15, +0x1910): the focus 150 back along the camera's second row
// (D_002DDFA0 +0x10, D_002C4108 -150); while aiming (4..6) the base velocity 300 (D_002C410C) toward the target, flat
void GetJtThrowCpdefi(JTTHROW& t, CPDEFI* pc)
{
    const glm::vec3 row = g_cm.mat[1];
    const float gain = -150.0f;
    const glm::vec3 position = glm::vec3(pc->posBase) + row * gain;
    pc->posBase = glm::vec4(position, pc->posBase.w);  // (the original's VU add also raises w by 1; unread)
    if (t.state < 4 || t.state >= 7) return;
    glm::vec3 direction = t.targetWorld - position + row * -gain;
    direction.z = 0.0f;
    const float length = glm::length(direction);
    pc->vBase = length < 0.0001f ? glm::vec4(0.0f) : glm::vec4(direction * (1.0f / length) * 300.0f, 0.0f);
}
