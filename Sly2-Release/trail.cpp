// Matrix-history meshes: original 0x21BB88..0x21C5F8 and 0x21FB70.
#include "trail.h"
#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "clock.h"
#include "mat.h"
#include "render.h"
#include "scene.h"
#include "so.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "worldaccess.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
TRAIL* Trail(LO* p) { return p && p->Cid() == 0x69 ? static_cast<TRAIL*>(p) : nullptr; }
float Field(const LO* p, uint32_t off, float def = 0.0f)
{
    return std::bit_cast<float>(p->U32Field(0, off, std::bit_cast<uint32_t>(def)));
}
void SetFloat(LO* p, uint32_t off, float value) { p->SetField(0, off, std::bit_cast<uint32_t>(value)); }
bool HistoryGroup(const GLOB& g) { return g.meshk == MESHK_Strips && (g.grfMesh & 8) && !g.avtx.empty(); }
float Dot(const glm::vec3& a, const glm::vec3& b) { return (a.x*b.x + a.y*b.y) + a.z*b.z; }
glm::vec3 Transform(const glm::mat4& m, const glm::vec3& p)
{
    return ((glm::vec3(m[0])*p.x + glm::vec3(m[1])*p.y) + glm::vec3(m[2])*p.z) + glm::vec3(m[3]);
}

// orig: 0x21BC08. Refill without changing the ring's next index.
void Reset(TRAIL* p)
{
    if (p->frames.empty()) return;
    float time = g_clock.t - float(p->frames.size()) * 0.016666668f;
    const glm::mat4 mat = p->apxfRef[2] ? p->apxfRef[2]->matWorld : glm::mat4(1.0f);
    for (size_t i = 0; i < p->frames.size(); ++i)
    {
        TRAILFRAME& f = p->frames[p->next];
        p->next = (p->next + 1) % p->frames.size();
        f.t = time;
        f.gain = 0.0f;
        time += 0.016666668f;
        f.mat = mat;
    }
    p->idle = 0;
}

// orig: 0x21C5F8. Only the loader's linked history groups receive new centres.
void SetCenters(TRAIL* p, const glm::vec3& pos)
{
    if (!p->pglobset) return;
    for (size_t i = 0; i < p->samples.size(); ++i)
        if (!p->samples[i].empty()) p->pglobset->aglob[i].posCenter = pos;
}

// orig: 0x21C0D8. Keep older visible samples after the object's gain reaches zero.
void Update(LO* lo)
{
    TRAIL* p = Trail(lo);
    if (!p) return;
    PostLoadTrail(p);
    if (PFNUPDATE base = PfnUpdateOrig(0x1A5E28)) base(p);
    if (p->dgFade != 0.0f)
    {
        p->gFade += g_clock.dt * p->dgFade;
        if (p->gFade <= 0.0f) p->gFade = p->dgFade = 0.0f;
        else if (1.0f <= p->gFade) { p->gFade = 1.0f; p->dgFade = 0.0f; }
    }
    if (p->gFade <= 0.0f && p->dgFade <= 0.0f && p->idle >= int(p->frames.size())) RemoveLo(g_psw, p);
}

// orig: 0x21C1C8. Sample after the base ALO transform/physics pass.
void UpdatePhysics(LO* lo)
{
    TRAIL* p = Trail(lo);
    if (!p) return;
    PostLoadTrail(p);
    UpdateAloPhys(p);
    if (p->U32Field(0, 0x154) || p->frames.empty()) return;
    TRAILFRAME& f = p->frames[p->next];
    p->next = (p->next + 1) % p->frames.size();
    f.t = g_clock.t;
    f.gain = p->gFade;
    f.mat = p->apxfRef[2] ? p->apxfRef[2]->matWorld : glm::mat4(1.0f);
    if (p->gFade <= 0.0f) ++p->idle;
    else p->idle = 0;
    if (p->follow)
    {
        if (p->gFade == 0.0f) p->distance = -4.0f;
        const glm::vec3 pos = PosLoWorld(p->follow), delta = p->previous - pos;
        const float distance = p->distance;
        p->previous = pos;
        p->distance = distance + std::sqrt(Dot(delta, delta)) / Field(p, 0x184, 100.0f);
        f.length = p->distance;
    }
    SetCenters(p, PosLoWorld(p));
}
NATIVEFN(0x21C1C8, UpdatePhysics);
struct REGISTER
{
    REGISTER() { RegisterLoUpdate(0x21C0D8, Update); }
} s_register;
}

// orig: 0x21BD18. Preserve authored vertices and own every deformed copy.
void PostLoadTrail(LO* lo)
{
    TRAIL* p = Trail(lo);
    if (!p || p->ready) return;
    float fade = Field(p, 0x150);
    int count = int(p->U32Field(0, 0x158));
    if (!count && fade == 0.0f) fade = 0.5f;
    if (fade != 0.0f) count = int(fade * 60.0f + 0.5f) + 2;
    else fade = float(count - 2) * 0.016666668f;
    if (count < 3 || !std::isfinite(fade) || fade <= 0.0f) throw std::runtime_error("trail has invalid authored history span");
    SetFloat(p, 0x150, fade);
    p->SetField(0, 0x158, uint32_t(count));
    p->frames.resize(size_t(count));
    if (!p->source) p->source = p->pglobset;
    if (p->source)
    {
        p->pglobset = std::make_shared<GLOBSET>(*p->source);
        p->pglobset->palo = p;
        const glm::vec3 axis(Field(p, 0x170, 1.0f), Field(p, 0x174), Field(p, 0x178));
        float loProjection = std::numeric_limits<float>::max(), hiProjection = -loProjection;
        for (GLOB& g : p->pglobset->aglob)
        {
            g.pgl = nullptr;
            for (SUBGLOB& sub : g.asubglob) sub.pgpu = nullptr;
            if (HistoryGroup(g)) for (const VTXGLOB& v : g.avtx)
            {
                const float d = Dot(v.pos, axis);
                if (hiProjection < d) hiProjection = d;
                if (d < loProjection) loProjection = d;
            }
        }
        const float range = hiProjection - loProjection, scale = Field(p, 0x168);
        p->samples.resize(p->pglobset->aglob.size());
        for (size_t i = 0; i < p->pglobset->aglob.size(); ++i)
        {
            GLOB& g = p->pglobset->aglob[i];
            if (!HistoryGroup(g)) continue;
            if (!(range > 0.0f) || !std::isfinite(range)) throw std::runtime_error("trail has degenerate authored projection");
            for (const VTXGLOB& v : g.avtx)
            {
                const float d = Dot(v.pos, axis);
                p->samples[i].push_back({v.pos + axis * ((scale - 1.0f) * d), (d - loProjection) / range, uint8_t(v.rgbaA >> 24)});
            }
            // 0x21BD18 sets the group owner to the identity placeholder, then 0x16EF00 refreshes it.
            g.oidOwner = -1;
            g.fMatrix = false;
            g.fHistoryWorld = true;
        }
    }
    for (const OPT& opt : p->aopt) if (opt.eopid == 1309 && !opt.aval.empty())
    {
        const OPTVAL& v = opt.aval[0];
        p->follow = v.pbasic ? dynamic_cast<LO*>(v.pbasic) : PloFindSwChild(p, v.n);
    }
    Reset(p);
    if (!p->U32Field(0, 0x154))
    {
        if (p->iupd >= 0 && g_psw) g_psw->aupd[size_t(p->iupd)].grf |= 2;
        else p->grfUpd |= 2;
    }
    p->ready = true;
}

// orig: 0x21BBD0 after 0x1F2C30.
void OnAddTrail(LO* lo) { if (TRAIL* p = Trail(lo)) Reset(p); }

void CloneTrail(const ALO* from, ALO* to)
{
    if (from->Cid() != 0x69 || to->Cid() != 0x69) return;
    const TRAIL* source = static_cast<const TRAIL*>(from);
    TRAIL* target = static_cast<TRAIL*>(to);
    target->source = source->source ? source->source : source->pglobset;
    target->pglobset = target->source;
    target->ready = false;
}

// orig: 0x21C340. The A sample alone supplies age/gain; both matrices supply position.
void UpdateTrailGeometry(TRAIL* p)
{
    PostLoadTrail(p);
    if (!p->pglobset || p->frames.empty()) return;
    const int count = int(p->frames.size());
    for (size_t i = 0; i < p->samples.size(); ++i)
    {
        GLOB& g = p->pglobset->aglob[i];
        for (size_t j = 0; j < p->samples[i].size(); ++j)
        {
            const TRAILSAMPLE& sample = p->samples[i][j];
            VTXGLOB& v = g.avtx[j];
            const float t = sample.fraction * float(count - 2);
            const int k = int(t), ia = (int(p->next) + count - k - 1) % count, ib = (ia + count - 1) % count;
            const float alpha = t - float(k), beta = 1.0f - alpha;
            const TRAILFRAME& a = p->frames[size_t(ia)];
            const TRAILFRAME& b = p->frames[size_t(ib)];
            const glm::vec3 pa = Transform(a.mat, sample.pos), pb = Transform(b.mat, sample.pos);
            v.pos = pa * beta + pb * alpha;
            const float fade = std::clamp(1.0f - (g_clock.t - a.t) / Field(p, 0x150), 0.0f, 1.0f);
            const uint8_t gain = uint8_t(int((a.gain * fade) * float(sample.alpha)));
            v.rgbaA = (v.rgbaA & 0x00ffffffu) | (uint32_t(gain) << 24);
            if (p->follow)
            {
                const float length = beta * a.length + alpha * b.length;
                const uint16_t packed = uint16_t(int(length * 4096.0f));
                v.uv.x = float(int16_t(packed)) * (1.0f / 4096.0f);
            }
        }
    }
}

void PlaceTrailScene(ALO* palo, SCENEGLOBSET& sgs)
{
    TRAIL* p = Trail(palo);
    if (!p) return;
    UpdateTrailGeometry(p);
    for (size_t i = 0; i < p->samples.size(); ++i) if (!p->samples[i].empty())
    {
        sgs.amatGlob[i] = glm::mat4(1.0f);
        sgs.aamatBone[i].clear();
        UpdateGlobVerticesGpu(p->pglobset->aglob[i]);
    }
    sgs.gAlpha = sgs.gAlpha2 = 1.0f;
}

int TrailFrameCount(LO* lo)
{
    if (TRAIL* p = Trail(lo)) { PostLoadTrail(p); return int(p->frames.size()); }
    return 0;
}

// orig: 0x2206C8 state 4, before the controller computes this frame's trajectory.
void BeginThrowArc(LO* lo)
{
    if (TRAIL* p = Trail(lo))
    {
        PostLoadTrail(p);
        p->SetField(0, 0x154, 1);
        p->idle = 0;
        AddLo(g_psw, p);
    }
}

// orig: 0x21FB70's history population. Points run from predicted contact toward launch.
void SetThrowArcFrames(LO* lo, const std::vector<glm::vec3>& points, const glm::vec3& origin)
{
    TRAIL* p = Trail(lo);
    if (!p || points.empty()) return;
    PostLoadTrail(p);
    // The original assumes two points. The initial zero flight-time case can
    // supply one; retain orientation until the first complete prediction.
    glm::vec3 previous = points.size() > 1 ? 2.0f * points[0] - points[1] : points[0];
    glm::mat4 mat = p->frames.back().mat;
    for (size_t i = 0; i < p->frames.size(); ++i)
    {
        TRAILFRAME& f = p->frames[p->frames.size() - i - 1];
        if (i < points.size())
        {
            const glm::vec3 delta = points[i] - previous;
            if (points.size() > 1) mat = glm::mat4(BuildLookAtMatrix(delta, glm::vec3(0, 0, 1)));
            mat[3] = glm::vec4(points[i], 1.0f);
            previous = points[i];
            f.gain = 1.0f;
        }
        else f.gain = 0.0f;
        f.t = g_clock.t;
        f.length = 0.0f;
        f.mat = mat;
    }
    SetCenters(p, origin);
}
