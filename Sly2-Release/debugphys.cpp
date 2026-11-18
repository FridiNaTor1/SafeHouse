// See debugphys.h. PC debug tools, not the original game.
#include "debugphys.h"

#include "activate.h"
#include "alo.h"
#include "brx.h"
#include "lo.h"
#include "po.h"
#include "render.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "worldaccess.h"
#include "xf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace
{
float s_gGravity = 1.0f;
float s_sScale = 1.0f;

bool FPlayerCharacter(const LO* plo) { return plo && (FIsLoDerivedFrom(plo, 0x35) || FIsLoDerivedFrom(plo, 0x36) || FIsLoDerivedFrom(plo, 0x37)); }

// ------------------------------------------------------------ the scaled hulls
// (the hull scale: the character scale up to 2x)
constexpr float S_HULL_MAX = 2.0f;
struct HULL
{
    size_t isoe;
    std::vector<glm::vec3> apos;
    std::vector<float> agDot;
    float sRadius;
};
ALO* s_paloHull = nullptr;      // the object whose hulls are scaled
uint32_t s_serialHull = 0;      // its world
float s_sHull = 1.0f;           // their scale
std::vector<HULL> s_ahull;      // the originals

bool FLog() { static const bool f = std::getenv("SLY2N_CHARSCALELOG") != nullptr; return f; }

// the feet: the step's base point (Sly (0, 0, -75)) through the position record; else the object's origin
glm::vec3 PosFeet(ALO* palo)
{
    XF* pxf = PxfPos(palo);
    const glm::mat4 m = pxf ? pxf->matWorld : glm::mat4(1.0f);
    STEP* pstep = FIsLoDerivedFrom(palo, 0x33) ? PstepFromLo(palo) : nullptr;
    return glm::vec3(m * glm::vec4(pstep ? glm::vec3(pstep->vecBase) : glm::vec3(0.0f), 1.0f));
}

void TouchSoe(ALO* palo, SOE& e)
{
    e.nStampWorld = -1;   // (stale: UpdateSoeWorld makes it again)
    UpdateSoeWorld(&e);
    (void)palo;
}

void RestoreHulls(bool fWorldGone)
{
    ALO* palo = s_paloHull;
    if (palo && !fWorldGone)
    {
        for (const HULL& h : s_ahull)
        {
            if (h.isoe >= palo->asoe.size()) continue;
            SOE& e = palo->asoe[h.isoe];
            if (e.geomLocal.apos.size() != h.apos.size()) continue;
            e.geomLocal.apos = h.apos;
            for (size_t i = 0; i < h.agDot.size() && i < e.geomLocal.asurf.size(); ++i) e.geomLocal.asurf[i].gDot = h.agDot[i];
            e.sRadius = h.sRadius;
            TouchSoe(palo, e);
        }
        MarkAloDirty(palo);
        UpdateAloBounds(palo);
    }
    s_paloHull = nullptr;
    s_ahull.clear();
    s_sHull = 1.0f;
}

void ScaleHulls(SW* psw, ALO* palo, float s)
{
    s_paloHull = palo;
    s_serialHull = psw->serial;
    s_sHull = s;
    const glm::vec3 posFeet = PosFeet(palo);
    STEP* pstep = FIsLoDerivedFrom(palo, 0x33) ? PstepFromLo(palo) : nullptr;
    for (size_t i = 0; i < palo->asoe.size(); ++i)
    {
        SOE& e = palo->asoe[i];
        // (not: the moving-point meshes (their points are the mesh's), the step's custom entry (made from the
        // others at each state change), the spheres (their centre is their record's origin))
        if (e.pmesh || e.f49 || e.geomLocal.apos.empty() || (pstep && int(i) == pstep->isoeCustom)) continue;
        HULL h{i, e.geomLocal.apos, {}, e.sRadius};
        for (const SURF& sf : e.geomLocal.asurf) h.agDot.push_back(sf.gDot);
        const glm::mat4 m = e.pxf ? e.pxf->matWorld : glm::mat4(1.0f);
        const glm::vec3 b = glm::vec3(glm::inverse(m) * glm::vec4(posFeet, 1.0f));   // the feet in the hull's frame
        for (glm::vec3& v : e.geomLocal.apos) v = b + (v - b) * s;
        for (SURF& sf : e.geomLocal.asurf) sf.gDot = sf.gDot * s + glm::dot(sf.normal, b) * (1.0f - s);
        e.sRadius *= s;
        TouchSoe(palo, e);
        if (FLog())
        {
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const glm::vec3& v : h.apos) mn = glm::min(mn, v), mx = glm::max(mx, v);
            std::printf("charscale: oid %d entry %zu (%zu points, r %.1f, x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f) x%.2f about %.0f %.0f %.0f\n", palo->oid, i,
                        h.apos.size(), h.sRadius, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z, s, b.x, b.y, b.z);
        }
        s_ahull.push_back(std::move(h));
    }
    MarkAloDirty(palo);
    UpdateAloBounds(palo);
}

ALO* PaloPlayer(SW* psw)
{
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    return FPlayerCharacter(plo) ? static_cast<ALO*>(plo) : nullptr;
}
}  // namespace

// ------------------------------------------------------------ gravity
float GDebugGravity() { return s_gGravity; }

float GDebugGravityOf(ALO* palo) { return s_gGravity != 1.0f && FPlayerCharacter(palo) ? s_gGravity : 1.0f; }

void SetDebugGravity(SW* psw, float g)
{
    if (!(g > 0.0f) || g == s_gGravity) return;
    const float r = g / s_gGravity;
    s_gGravity = g;
    if (!psw) return;
    for (CID cid : {0x35, 0x36, 0x37})
        for (LO* plo : AploOfClass(psw, cid))
            if (SOST* ps = Psost(static_cast<ALO*>(plo)))
            {
                ps->dvGravity *= r;
                if (g == 1.0f) ps->dvGravity = glm::round(ps->dvGravity * 1000.0f) / 1000.0f;   // (no drift back at 1x)
            }
}

// ------------------------------------------------------------ character scale
float SDebugCharScale() { return s_sScale; }

void SetDebugCharScale(float s)
{
    if (s > 0.0f) s_sScale = s;
}

void UpdateDebugCharScale(SW* psw)
{
    if (s_paloHull && (!psw || psw->serial != s_serialHull)) RestoreHulls(true);   // (its world is gone)
    if (!psw) return;
    if (static int s_c = 0; FLog() && ++s_c % 20 == 0)
        if (ALO* pp = PaloPlayer(psw))
        {
            const glm::vec3 p = PosFeet(pp);
            const SOST* ps = Psost(pp);
            std::printf("charscale: step %d feet %.0f %.0f %.0f gravity %.0f\n", s_c, p.x, p.y, p.z, ps ? ps->dvGravity.z : 0.0f);
        }
    ALO* palo = s_sScale != 1.0f ? PaloPlayer(psw) : nullptr;
    const float sHull = std::min(s_sScale, S_HULL_MAX);
    if (palo == s_paloHull && (!palo || sHull == s_sHull)) return;
    RestoreHulls(false);
    if (palo && sHull != 1.0f) ScaleHulls(psw, palo, sHull);
}

bool FDebugCharScaleDelta(ALO* palo, glm::mat4* pdmat, float* ps)
{
    if (s_sScale == 1.0f || !palo) return false;
    ALO* pplayer = PaloPlayer(g_psw);
    if (!pplayer) return false;
    const ALO* proot = palo;
    while (proot->paloParent) proot = proot->paloParent;
    if (proot != pplayer) return false;
    const glm::vec3 b = PosFeet(pplayer);
    *pdmat = glm::translate(glm::mat4(1.0f), b) * glm::scale(glm::mat4(1.0f), glm::vec3(s_sScale)) * glm::translate(glm::mat4(1.0f), -b);
    *ps = s_sScale;
    return true;
}

void ApplyDebugCharScaleCamera(CAMERA* pcam)
{
    if (s_sScale == 1.0f) return;
    ALO* palo = PaloPlayer(g_psw);
    if (!palo) return;
    const float s = s_sScale;
    const glm::vec3 b = PosFeet(palo);
    pcam->pos = b + (pcam->pos - b) * s;
    // a wider view when small, a little narrower when huge (log2: 0.1x -> 1.3, 10x -> 0.85)
    const float k = std::clamp(1.0f - 0.12f * std::log2(s), 0.85f, 1.3f);
    pcam->fovY = std::min(pcam->fovY * k, 100.0f);
    pcam->sNear = std::max(1.0f, pcam->sNear * s);
    pcam->sFar *= std::max(1.0f, s);
    pcam->rMRDAdjust *= std::max(1.0f, s);
}

void ApplyDebugCharScaleFog(RENDEROPTS* popts)
{
    if (s_sScale <= 1.0f || !PaloPlayer(g_psw)) return;
    popts->sFogNear *= s_sScale;
    popts->sFogFar *= s_sScale;
}
