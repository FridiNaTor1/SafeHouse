// step.cpp: the stepping objects' heading and motion (step.h). Ported from the reference's m_203948_p3.cpp and
// m_2040F8.cpp (oracle-checked ports of the original's 0x203948..0x205E70).
#include "step.h"
#include "worldglobals.h"
#include "activate.h"
#include <cstdio>
#include <cstdlib>
#include "gang.h"
#include "stepphys.h"

#include "actor.h"
#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "lo.h"
#include "so.h"
#include "update.h"
#include "vecmat.h"
#include "vtnative.h"
#include "worldaccess.h"

#include <cmath>
#include <map>

using cmm::F32;


namespace
{
std::map<LO*, std::unique_ptr<STEP>> s_mpstep;
WORLD_TABLE(s_mpstep);   // (PC: moved aside while a kept world loads, worldglobals.h)

}  // namespace

STEP* PstepFromLo(LO* plo)
{
    if (!plo) return nullptr;
    auto it = s_mpstep.find(plo);
    return it == s_mpstep.end() ? nullptr : it->second.get();
}

STEP* PstepNew(ALO* palo)
{
    auto& p = s_mpstep[palo];
    if (!p)
    {
        if (FIsLoDerivedFrom(palo, 0x35)) p = std::make_unique<SLY>();
        else if (STEP* pg = PstepNewGang(palo)) p.reset(pg);  // (Bentley / Murray: gang.h)
        else if (FIsLoDerivedFrom(palo, 0x34)) p = std::make_unique<ACTOR>();
        else p = std::make_unique<STEP>();
        p->palo = palo;
    }
    return p.get();
}

void ResetSteps() { s_mpstep.clear(); }

void CopyStep(ALO* palo, const STEP* psrc)
{
    STEP* p = PstepNew(palo);
    ALO* paloKeep = p->palo;
    static_cast<STEP&>(*p) = *psrc;
    p->palo = paloKeep;
    p->pxpFloor = nullptr;
}

// orig: 0x00204740
float RadStepHeading(STEP* p)
{
    const XF* pxf = p->palo ? PxfObj(p->palo) : nullptr;
    const glm::mat4 m = pxf ? pxf->matWorld : MatWorldOfAlo(p->palo);
    return std::atan2(m[0].y, m[0].x);
}

// orig: 0x00203FC0
void UpdateStep(STEP* p)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(p->palo);  // (update.cpp)
    UpdateStepMesh(p->palo);  // 0x203E08 (stepphys.cpp: the custom entry follows the second mesh +0x25C)
    UpdateStepPhysTurn(p->palo);
    PrintStepDebug(p);
}

// (debug: SLY2N_STEPDBG=oid: the step's motion fields each update, as the reference's +0x1A4 / +0x190 / +0x208 / +0x180)
void PrintStepDebug(STEP* p)
{
    // (debug: SLY2N_STEPDBG=oid: the step's motion fields each update, as the reference's +0x1A4 / +0x190 / +0x208 / +0x180)
    if (static const char* s_sz = std::getenv("SLY2N_STEPDBG"); s_sz && p->palo && p->palo->oid == std::atoi(s_sz))
    {
        const glm::vec3 v = VelAloPos(p->palo);
        const glm::vec3 pos = PosLoWorld(p->palo);
        std::fprintf(stderr, "step %d %p t %.4f motion %d vecVel %.2f %.2f %.2f heading %.4f uTurn %.4f floor %d pos %.2f %.2f %.2f vel %.2f %.2f %.2f\n",
                     p->palo->oid, (void*)p->palo, g_clock.t, p->nMotion, p->vecVel.x, p->vecVel.y, p->vecVel.z, p->radHeading, p->uTurn,
                     p->pxpFloor != nullptr, pos.x, pos.y, pos.z, v.x, v.y, v.z);
    }
}

// orig: 0x002041C0
void SetStepPos(STEP* p, const glm::vec3& pos)
{
    SetAloPos(p->palo, pos);
    const XF* pxf = PxfObj(p->palo);
    const glm::mat4 m = pxf ? pxf->matWorld : MatWorldOfAlo(p->palo);
    p->posFloor = m * glm::vec4(glm::vec3(p->vecBase), 1.0f);  // (0x1A6F00(p, 0, +0x240))
}

// orig: 0x002041F8
void SetStepRotation(STEP* p, const glm::mat3& mat)
{
    SetAloRotation(p->palo, mat);
    SetStepPhysHeading(p->palo, RadStepHeading(p));
    if (PODATA* ppo = PpodataFromLo(p->palo); ppo && ppo->ppadctl) ppo->ppadctl->radJoy = p->radHeading;
}

// The step classes' vt slots 29 / 30 (0x2041C0 / 0x2041F8, reached through the object as SetAloParent's grf 1 / 2
// (0x1F21B8) and the other slot calls do): before these were bound the base SetAloPos / SetAloRotation ran, so a
// reparented step object (a guard grabbed, carried and thrown by Murray) kept its old heading and +0x230.
namespace
{
void SetStepPosVt(ALO* palo, const glm::vec3& pos) { SetStepPos(&StepPhysOf(palo), pos); }
void SetStepRotationVt(ALO* palo, const glm::mat3& mat) { SetStepRotation(&StepPhysOf(palo), mat); }
}  // namespace
NATIVEFN(0x2041C0, SetStepPosVt);
NATIVEFN(0x2041F8, SetStepRotationVt);
