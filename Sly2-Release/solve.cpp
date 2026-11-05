// The per-frame constraint pass over the SOs (orig module 0x1F8DD8-0x1FAF70): the active objects' updates (slot 17),
// the groups of touching SOs gathered and their contacts solved (DZG, dzg.cpp) for positions and velocities, the
// solutions applied to the position / rotation records. Ported from the reference's m_1F8DD8.cpp and
// m_1F8DD8_p3.cpp (oracle-verified ports of the original's code).

#include "dzg.h"
#include "mat.h"
#include "so.h"
#include "util.h"
#include "vtnative.h"
#include "anim.h"
#include "clock.h"
#include "xfphys.h"
#include "carry.h"
#include "actor.h"

#include <charconv>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>


// The world's entries loop for slot 17 (orig 0x1F8DD8: the entries of SW +0x3F20 whose flags & 0x24 == 4); set by
// the world's update dispatch. Null: every ALO given to SetPhysSwObjects with update flags & 0x24 == 4, in order.
void (*g_pfnUpdateSwEntries17)() = nullptr;
namespace
{
std::vector<LO*> s_aploPhys;
}
void SetPhysSwObjects(const std::vector<LO*>& aplo) { s_aploPhys = aplo; }

namespace
{
constexpr uint64_t FL148_MARK27 = 1ull << 27;
constexpr uint64_t FL148_MARK28 = 1ull << 28;
inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline bool FMark(ALO* p, uint64_t m) { return p->psort && (p->psort->grf148 & m); }

void VtCall(ALO* p, int slot, uint32_t addrDefault)
{
    using PFN = void (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, slot));
    if (!pfn) pfn = reinterpret_cast<PFN>(PvNativeFn(addrDefault));
    if (pfn) pfn(p);
}
void VtUpdate17(LO* plo)
{
    using PFN = void (*)(LO*);
    if (PFN pfn = PfnVt<PFN>(plo, 17)) pfn(plo);
}

// Optional observation of the existing 0x1F8DD8 phases. The frame number counts
// UpdateSwPhys calls from process start, independently of world/game frames.
struct CONTACTTRACE
{
    int oid = -1, first = 0, last = 0;
    uint64_t frame = 0;

    CONTACTTRACE()
    {
        const char* text = std::getenv("SLY2N_CONTACTPRINT");
        if (!text) return;
        const char* end = text + std::strlen(text);
        int values[3]{};
        bool valid = true;
        for (int i = 0; i < 3; ++i)
        {
            const auto parsed = std::from_chars(text, end, values[i]);
            if (parsed.ec != std::errc{} || values[i] < 0 ||
                (i < 2 ? parsed.ptr == end || *parsed.ptr != ':' : parsed.ptr != end))
            {
                valid = false;
                break;
            }
            text = i < 2 ? parsed.ptr + 1 : parsed.ptr;
        }
        if (!valid || values[2] < values[1])
        {
            std::fprintf(stderr, "SLY2N_CONTACTPRINT expects oid:first:last (nonnegative integers, inclusive physics frames)\n");
            return;
        }
        oid = values[0]; first = values[1]; last = values[2];
    }
};

void PrintContactNumber(float value)
{
    if (std::isfinite(value)) std::fprintf(stderr, "%.9g", value);
    else std::fputs("null", stderr);
}

void PrintContactVector(const glm::vec3& value)
{
    std::fputc('[', stderr);
    for (int i = 0; i < 3; ++i)
    {
        if (i) std::fputc(',', stderr);
        PrintContactNumber(value[i]);
    }
    std::fputc(']', stderr);
}

void PrintContact(const XP* xp)
{
    if (!xp) { std::fputs("null", stderr); return; }
    const XPD& a = xp->axpd[0];
    const XPD& b = xp->axpd[1];
    std::fprintf(stderr, "{\"kind\":%d,\"roots\":[%d,%d],\"owners\":[%d,%d],\"tags\":[%d,%d],\"positions\":[",
                 xp->xpk, a.paloRoot ? a.paloRoot->oid : -1, b.paloRoot ? b.paloRoot->oid : -1,
                 a.palo ? a.palo->oid : -1, b.palo ? b.palo->oid : -1,
                 a.psoe ? a.psoe->tag : -1, b.psoe ? b.psoe->tag : -1);
    PrintContactVector(a.pos); std::fputc(',', stderr); PrintContactVector(b.pos);
    std::fputs("],\"normal\":", stderr); PrintContactVector(xp->normal);
    std::fputs(",\"distance\":", stderr); PrintContactNumber(xp->gDist);
    std::fputs(",\"relative_velocity\":", stderr); PrintContactVector(xp->dv);
    std::fputs(",\"friction\":", stderr); PrintContactNumber(xp->mu);
    std::fputs(",\"elasticity\":", stderr); PrintContactNumber(xp->elas);
    std::fputs(",\"floor_limits\":", stderr);
    if (xp->xpk == 14)
    {
        std::fputc('[', stderr); PrintContactNumber(xp->gA0);
        std::fputc(',', stderr); PrintContactNumber(xp->gA4); std::fputc(']', stderr);
    }
    else std::fputs("null", stderr);
    std::fputc('}', stderr);
}

void PrintContactConstraint(const CNSTRS* constraint)
{
    if (!constraint) { std::fputs("null", stderr); return; }
    std::fprintf(stderr, "{\"kind\":%d,\"local_axis\":", constraint->kind);
    PrintContactVector(constraint->normal);
    std::fputc('}', stderr);
}

void TraceContactPhase(const CONTACTTRACE& trace, const char* phase)
{
    ALO* actor = nullptr;
    for (OXA* attachment : g_physw.apoxaActive)
    {
        if (attachment->pso && attachment->pso->oid == trace.oid) actor = attachment->pso;
        for (SOE* entry : attachment->apsoe)
            if (entry->palo && entry->palo->oid == trace.oid) actor = entry->palo;
        if (actor) break;
    }
    std::fprintf(stderr, "CONTACT_PHASE {\"physics_frame\":%llu,\"phase\":\"%s\",\"oid\":%d,\"present\":%s,\"time\":",
                 static_cast<unsigned long long>(trace.frame), phase, trace.oid, actor ? "true" : "false");
    PrintContactNumber(g_clock.t);
    std::fputs(",\"dt\":", stderr); PrintContactNumber(g_clock.dt);
    if (!actor) { std::fputs("}\n", stderr); return; }
    const XF* position = PxfPos(actor);
    const SOST* mass = Psost(actor);
    const STEP* step = PstepFromLo(actor);  // lookup only; never create actor/step state
    const ACTOR* player = dynamic_cast<const ACTOR*>(step);
    std::fputs(",\"position\":", stderr); PrintContactVector(PosAloCenter(actor));
    std::fputs(",\"local_velocity\":", stderr);
    if (position) PrintContactVector(glm::vec3(position->vel)); else std::fputs("null", stderr);
    std::fputs(",\"acceleration_local\":", stderr);
    if (position) PrintContactVector(glm::vec3(position->aseg[3])); else std::fputs("null", stderr);
    std::fputs(",\"linear_constraint\":", stderr); PrintContactConstraint(mass ? &mass->cnstrLin : nullptr);
    std::fputs(",\"angular_constraint\":", stderr); PrintContactConstraint(mass ? &mass->cnstrAng : nullptr);
    std::fputs(",\"jump\":", stderr);
    if (player)
    {
        std::fprintf(stderr, "{\"kind\":%d,\"state\":%d,\"target_velocity\":", player->jump.kind, player->jump.state);
        PrintContactVector(player->jump.vecA0); std::fputc('}', stderr);
    }
    else std::fputs("null", stderr);
    std::fputs(",\"floor\":", stderr); PrintContact(step ? step->pxpFloor : nullptr);
    std::fputs(",\"contacts\":[", stderr);
    int count = 0;
    bool truncated = false;
    auto printList = [&](const XP* xp, const char* source) {
        for (; xp; xp = xp->pxpNext)
        {
            if (count == 128) { truncated = true; break; }
            if (count++) std::fputc(',', stderr);
            std::fprintf(stderr, "{\"source\":\"%s\",\"contact\":", source);
            PrintContact(xp); std::fputc('}', stderr);
        }
    };
    if (const OXA* attachment = Poxa(actor))
    {
        for (const OX* overlap = attachment->pox; overlap; overlap = overlap->poxNext)
            printList(overlap->pxp, "overlap");
        printList(attachment->pxpInternal, "internal");
    }
    std::fprintf(stderr, "],\"contacts_truncated\":%s}\n", truncated ? "true" : "false");
}
}

// orig: 0x001FA810 func_001FA810 (a constraint applied to v: kind 3 zero, 2 v along the axis, 1 v without it; the
// axis rotated into world space by the record's parent when it has one)
glm::vec3 ProjectCnstr(const CNSTRS& c, const XF* pxfOwner, const glm::vec3& v)
{
    const int k = c.kind;
    if (k == 3) return glm::vec3(0.0f);
    if (k != 1 && k != 2) return v;
    glm::vec3 n = c.normal;
    if (pxfOwner && pxfOwner->pxfRef)
    {
        const glm::vec3 m = Mat3Of(pxfOwner->pxfRef->matWorld) * n;
        const float l = std::sqrt(std::fabs(Dot3(m, m)));
        n = m * (l != 0.0f ? 1.0f / l : FLT_MAX);
    }
    const glm::vec3 pr = n * Dot3(v, n);
    return k == 2 ? pr : v - pr;
}

// orig: 0x001FA970 func_001FA970 (a constraint applied to v, the axis as it is)
glm::vec3 ProjectCnstrLocal(const CNSTRS& c, const glm::vec3& v)
{
    const int k = c.kind;
    if (k == 3) return glm::vec3(0.0f);
    if (k != 1 && k != 2) return v;
    const glm::vec3 pr = c.normal * Dot3(v, c.normal);
    return k == 2 ? pr : v - pr;
}

// orig: 0x001FAA00 func_001FAA00 (an impulse's velocity change: / mass, through the linear constraint)
glm::vec3 DvAloImpulse(ALO* p, const glm::vec3& v)
{
    SOST* ps = Psost(p);
    return ProjectCnstr(ps->cnstrLin, PxfPos(p), v * (1.0f / ps->m));
}

// orig: 0x001FAA98 func_001FAA98 (an angular impulse's change: into the rotation record's frame, the inverse
// inertia, back, through the angular constraint)
glm::vec3 DwAloTorque(ALO* p, const glm::vec3& q)
{
    XF* s2 = PxfRot(p);
    SOST* ps = Psost(p);
    const glm::mat3 w = Mat3Of(s2->matWorld);
    glm::vec3 v = InverseMatrix3(w) * q;
    v = ps->momintInv * v;
    return ProjectCnstr(ps->cnstrAng, s2, w * v);
}

// orig: 0x001FAA48 func_001FAA48 (the angular change of impulse b at point a: (a - the object's position) x b)
glm::vec3 DwAloImpulseAt(ALO* p, const glm::vec3& a, const glm::vec3& b) { return DwAloTorque(p, glm::cross(a - PosAloCenter(p), b)); }

// orig: 0x001FAB48 func_001FAB48 (the velocity of point q: lin + ang x (q - the rotation record's position))
glm::vec3 VelAloPoint(ALO* p, const glm::vec3& lin, const glm::vec3& ang, const glm::vec3& q)
{
    XF* s2 = PxfRot(p);
    const glm::vec3 r = q - (s2 ? PosOf(s2->matWorld) : glm::vec3(0.0f));
    return lin + glm::cross(ang, r);
}

// orig: 0x001FAC30 func_001FAC30 (an acceleration into the position record's (+0xF0) when it is in a velocity mode)
void AccelAloPos(ALO* p, const glm::vec3& v)
{
    XF* po = PxfPos(p);
    if (!po || !FXfModeVelocity(po->mode)) return;
    const glm::vec3 r = VecConvertXfs(nullptr, po->pxfRef, v);
    po->aseg[3] = glm::vec4(glm::vec3(po->aseg[3]) + r, po->aseg[3].w);
}

// orig: 0x001F9B20 func_001F9B20 (ALO vt slot 70: gravity through the linear constraint unless zero or fixed; then
// the waters the object is in (+0x138, 0x229798: water.cpp))
void ApplyAloGravity(ALO* p)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    bool fUpd = false;
    if (!FVecsNear(g_vecZero, ps->dvGravity)) fUpd = ps->cnstrLin.kind != 3;
    if (fUpd) AccelAloPos(p, ProjectCnstr(ps->cnstrLin, PxfPos(p), ps->dvGravity));
    ApplyAloWater(p);
}
NATIVEFN(0x1F9B20, ApplyAloGravity);

// orig: 0x001FAD00 func_001FAD00 (ALO vt slot 68: a row's responses for end k: the impulse along the normal (negated
// for k 1) through the mass and through the inertia at the contact's point, unless fixed)
void FillAloDzResponse(ALO* p, DZ* pdz, DZENT* pent, int grf, int k)
{
    (void)pdz;
    (void)grf;
    SOST* ps = Psost(p);
    if (!ps) return;
    glm::vec3 v = pent->normal;
    if (k) v = v * -1.0f;
    if (ps->cnstrLin.kind != 3) pent->advLin[k] = DvAloImpulse(p, v);
    if (ps->cnstrAng.kind != 3) pent->advAng[k] = DwAloImpulseAt(p, pent->pxp->axpd[k].pos, v);
}
NATIVEFN(0x1FAD00, FillAloDzResponse);

// orig: 0x001FADD0 func_001FADD0 (ALO vt slot 69: the velocity of a point from a response; zero without the mass
// state)
glm::vec3 VelAloDzResponse(ALO* p, const glm::vec3& lin, const glm::vec3& ang, const glm::vec3& pos)
{
    if (!Psost(p)) return glm::vec3(0.0f);
    return VelAloPoint(p, lin, ang, pos);
}
NATIVEFN(0x1FADD0, VelAloDzResponse);

// orig: 0x001FA328 func_001FA328 (ALO vt slot 66: the pending changes into the records' velocities)
void ApplyAloVelocityChanges(ALO* p)
{
    if (!Psost(p)) return;
    SORT& s = Sort(p);
    if (!FVecsNear(g_vecZero, s.dpos))
    {
        XF* b = PxfPos(p);
        HoldXfMode(p, b);
        const glm::vec3 inc = VecConvertXfs(nullptr, b->pxfRef, s.dpos);
        s.dpos = glm::vec3(0.0f);
        b->vel = glm::vec4(glm::vec3(b->vel) + inc, b->vel.w);
        MarkAloDirty(p);
    }
    if (!FVecsNear(g_vecZero, s.drot))
    {
        XF* b = PxfRot(p);
        HoldXfMode(p, b);
        const glm::vec3 inc = VecConvertXfs(nullptr, b->pxfRef, s.drot);
        s.drot = glm::vec3(0.0f);
        b->vel = glm::vec4(glm::vec3(b->vel) + inc, b->vel.w);
        MarkAloDirty(p);
    }
}
NATIVEFN(0x1FA328, ApplyAloVelocityChanges);

// orig: 0x001FA528 func_001FA528 (ALO vt slot 67: the pending displacement moves the position record, the pending
// rotation turns the rotation record; slot 18 and dirty when anything changed)
void ApplyAloDisplacement(ALO* p)
{
    if (!Psost(p)) return;
    SORT& s = Sort(p);
    bool fChanged = false;
    if (!FVecsNear(g_vecZero, s.dpos))
    {
        XF* s0 = PxfPos(p);
        fChanged = true;
        HoldXfMode(p, s0);
        const glm::vec3 r = VecConvertXfs(nullptr, s0->pxfRef, s.dpos);
        s.dpos = glm::vec3(0.0f);
        SetPos(&s0->mat, PosOf(s0->mat) + r);
        s0->nStamp = ++g_cXfStamp;
    }
    if (!FVecsNear(g_vecZero, s.drot))
    {
        XF* s0 = PxfRot(p);
        fChanged = true;
        HoldXfMode(p, s0);
        const glm::vec3 r = VecConvertXfs(nullptr, s0->pxfRef, s.drot);
        s.drot = glm::vec3(0.0f);
        const glm::mat3 R = BuildRotateMatrixVector(r);
        SetMat3(&s0->mat, R * Mat3Of(s0->mat));
        s0->nStamp = ++g_cXfStamp;
    }
    if (fChanged)
    {
        VtCall(p, 18, 0x22FBE0);
        MarkAloDirty(p);
    }
}
NATIVEFN(0x1FA528, ApplyAloDisplacement);

// orig: 0x001F9E70 func_001F9E70 (ALO vt slot 65: the pending changes into the records' velocities, then half a
// frame of each change moves the position / turns the rotation (renormalized); dirty, slot 18)
void ApplyAloVelocityChangesMoving(ALO* p)
{
    if (!Psost(p)) return;
    SORT& s = Sort(p);
    XF* s4 = PxfPos(p);
    XF* s3 = PxfRot(p);
    bool fChanged = false;
    glm::vec3 dv(0.0f), dw(0.0f);
    if (!FVecsNear(g_vecZero, s.dpos))
    {
        HoldXfMode(p, s4);
        dv = VecConvertXfs(nullptr, s4->pxfRef, s.dpos);
        fChanged = true;
        s.dpos = glm::vec3(0.0f);
        s4->vel = glm::vec4(glm::vec3(s4->vel) + dv, s4->vel.w);
    }
    if (!FVecsNear(g_vecZero, s.drot))
    {
        HoldXfMode(p, s3);
        dw = VecConvertXfs(nullptr, s3->pxfRef, s.drot);
        fChanged = true;
        s.drot = glm::vec3(0.0f);
        s3->vel = glm::vec4(glm::vec3(s3->vel) + dw, s3->vel.w);
    }
    if (!fChanged) return;
    MarkAloDirty(p);
    if (!FVecsNear(g_vecZero, dv))
    {
        const float h = g_clock.dt * 0.5f;
        SetPos(&s4->mat, PosOf(s4->mat) + dv * h);
        s4->nStamp = ++g_cXfStamp;
    }
    if (!FVecsNear(g_vecZero, dw))
    {
        const float h = g_clock.dt * 0.5f;
        const glm::mat3 R = BuildRotateMatrixVector(dw * h);
        glm::mat3 n = R * Mat3Of(s3->mat);
        NormalizeRotateMatrix3(&n);
        SetMat3(&s3->mat, n);
        s3->nStamp = ++g_cXfStamp;
    }
    VtCall(p, 18, 0x22FBE0);
}
NATIVEFN(0x1F9E70, ApplyAloVelocityChangesMoving);

// orig: 0x001F9C48 func_001F9C48 (the two records stepped back half a frame and stopped; slot 18, the entries'
// matrices and the attachment's world geometry refreshed, off the dirty lists)
void StepAloBackHalfFrame(ALO* p)
{
    XF* s0 = PxfPos(p);
    XF* s1 = PxfRot(p);
    ReleaseXfMode(p, s0);
    ReleaseXfMode(p, s1);
    {
        const float k = g_clock.dt * -0.5f;
        SetPos(&s0->mat, PosOf(s0->mat) + glm::vec3(s0->vel) * k);
    }
    s0->nStamp = ++g_cXfStamp;
    s0->vel = glm::vec4(glm::vec3(0.0f), s0->vel.w);
    {
        const float k = g_clock.dt * -0.5f;
        const glm::mat3 R = BuildRotateMatrixVector(glm::vec3(s1->vel) * k);
        glm::mat3 n = R * Mat3Of(s1->mat);
        NormalizeRotateMatrix3(&n);
        SetMat3(&s1->mat, n);
    }
    s1->nStamp = ++g_cXfStamp;
    s1->vel = glm::vec4(glm::vec3(0.0f), s1->vel.w);
    VtCall(p, 18, 0x22FBE0);
    CopyAloSoeMatrices(p);
    if (OXA* e = Poxa(p))
        for (SOE* node : e->apsoe)
            if (!node->f50) UpdateSoeWorld(node);
    UnmarkAloDirty(p);
}

namespace
{
// orig: 0x001F8E90 func_001F8E90 (the group of SOs touching px: marked (bit 27), appended, then the touching SOs
// with contacts not marked yet and not fixed)
void GatherGroup(ALO* px, std::vector<ALO*>* papx)
{
    Sort(px).grf148 |= FL148_MARK27;
    papx->push_back(px);
    OXA* poxa = Poxa(px);
    if (!poxa) return;
    for (OX* pn = poxa->pox; pn; pn = pn->poxNext)
    {
        if (!pn->pxp) continue;
        ALO* q = pn->psoOther;
        if (FMark(q, FL148_MARK27)) continue;
        SOST* pe = Psost(q);
        if (!pe) continue;
        if (pe->cnstrLin.kind == 3 && pe->cnstrAng.kind == 3) continue;
        GatherGroup(q, papx);
    }
}

// orig: 0x001F90D0 func_001F90D0 (up to cMax contacts of the group's SOs (kind 0xD skipped, kind 0xE only when its
// end 0's root's slot 85 has bit 4), numbered)
int CollectGroupXps(const std::vector<ALO*>& apx, int cMax, std::vector<XP*>* paps)
{
    int cs = 0;
    for (ALO* px : apx)
    {
        Sort(px).grf148 |= FL148_MARK28;
        OXA* poxa = Poxa(px);
        if (!poxa) continue;
        OX* pl = poxa->pox;
        for (;;)
        {
            while (pl && FMark(pl->psoOther, FL148_MARK28)) pl = pl->poxNext;
            for (XP* ps = pl ? pl->pxp : poxa->pxpInternal; ps; ps = ps->pxpNext)
            {
                const int k = ps->xpk;
                if (k == 0xD) continue;
                if (k == 0xE)
                {
                    ALO* pr = ps->axpd[0].paloRoot;
                    using PFN = uint32_t (*)(ALO*);
                    PFN pfn = pr ? reinterpret_cast<PFN>(PvNativeVt(pr, 85)) : nullptr;
                    if (std::getenv("PHYS_XPDUMP"))
                        std::printf("  kind-14 contact of %d: slot 85 -> 0x%X\n", pr ? pr->oid : -1, pfn ? pfn(pr) : 0u);
                    if (!pfn || !(pfn(pr) & 4)) continue;
                }
                if (cs >= cMax) return cs;
                ps->ipxp = cs;
                paps->push_back(ps);
                cs++;
            }
            if (!pl) break;
            pl = pl->poxNext;
        }
    }
    return cs;
}
}

// orig: 0x001F9410 func_001F9410 (one group's contacts solved: a DZG row per contact; flag 4 the positions (SolveDzg,
// applied at twice the largest depth, 2 pi; slot 67), then the velocities (flag 1: the contacts refreshed, the
// target the normal speed; else the speed times 1 + the elasticity randomized 0.9..1.1), friction rows, applied at
// twice the largest speed, 50; flag 1: slot 65 and, when nothing moves any more, the half-frame step back; else
// slot 66)
void SolveGroup(int flags, int nobj, ALO** objs, int ncont, XP** conts)
{
    DZG dzg;
    InitDzg(&dzg, ncont, conts, nobj, objs);
    const bool fl1 = (flags & 1) != 0;
    for (int i = 0; i < ncont; i++)
    {
        DZENT* dzs = &dzg.aent[i];
        XP* xp = conts[i];
        dzs->pxp = xp;
        DZ* dz = &dzg.adz[i];
        dzs->normal = xp->normal;
        const int k = xp->xpk;
        if (k == 0xB)
        {
            dz->gMin = -FLT_MAX;
            dz->kind = 1;
            dz->gMax = FLT_MAX;
        }
        else if (k == 0xE)
        {
            if (!fl1)
            {
                dz->gMin = 0.0f;
                dz->gMax = FLT_MAX;
                dz->kind = 0;
            }
            else
            {
                const float dt = g_clock.dt;
                dz->gMin = xp->gA0 * dt;
                dz->gMax = xp->gA4 * dt;
                dz->kind = xp->gA0 < 0.0f ? 1 : 0;
            }
        }
        else
        {
            dz->kind = 0;
            dz->gMin = 0.0f;
            dz->gMax = FLT_MAX;
        }
        AppendDzgDz(&dzg, dz, dzs);
    }
    if (flags & 4)
    {
        float mx = 0.0f;
        for (int i = 0; i < ncont; i++)
        {
            DZ* dz = &dzg.adz[i];
            XP* xp = conts[i];
            dz->gResidual = (dz->kind == 1) ? xp->gDist : xp->gDist + 1.0f;
            const float d = std::fabs(xp->gDist);
            if (mx < d) mx = d;
        }
        SolveDzg(&dzg, 4);
        ApplyDzg(&dzg, mx + mx, 6.28318548f);  // 2 pi (0x40C90FDB)
        for (int i = 0; i < nobj; i++) VtCall(objs[i], 67, 0x1FA528);
        if (flags == 4) return;
        ClearDzgSolution(&dzg);
    }
    float mx = 0.0f;
    if (fl1)
    {
        UpdateXpPositions(ncont, conts);
        UpdateXpWorldPositions(ncont, conts);
        for (int i = 0; i < ncont; i++)
        {
            XP* xp = conts[i];
            dzg.adz[i].gResidual = Dot3(xp->dv, xp->normal);
            const float l = Dot3(xp->dv, xp->dv);
            if (mx < l) mx = l;
        }
    }
    else
    {
        for (int i = 0; i < ncont; i++)
        {
            XP* xp = conts[i];
            const float r = GRandInRange(0.899999976f, 1.10000002f);  // 0x3F666666, 0x3F8CCCCD
            const float e = xp->elas * r;
            const float d = Dot3(xp->dv, xp->normal);
            dzg.adz[i].gResidual = d * (e + 1.0f);
            const float l = Dot3(xp->dv, xp->dv);
            if (mx < l) mx = l;
        }
    }
    SolveDzgContacts(&dzg, flags);
    const float sq = std::sqrt(mx);
    ApplyDzg(&dzg, sq + sq, 50.0f);
    if (!fl1)
    {
        for (int i = 0; i < nobj; i++) VtCall(objs[i], 66, 0x1FA328);
        return;
    }
    for (int i = 0; i < nobj; i++) VtCall(objs[i], 65, 0x1F9E70);
    int i = 0;
    for (; i < nobj; i++)
    {
        ALO* o = objs[i];
        XF* p58 = PxfPos(o);
        XF* b = PxfRot(o);
        if (p58->mode == 2) break;
        if (b->mode == 2) break;
        SOST* m = Psost(o);
        if (25.0f < Dot3(glm::vec3(p58->vel), glm::vec3(p58->vel))) break;
        if (0.00249999994f < Dot3(glm::vec3(b->vel), glm::vec3(b->vel))) break;  // 0x3B23D70B
        if (25.0f < Dot3(m->v, m->v)) break;
        if (0.00249999994f < Dot3(m->w, m->w)) break;
    }
    if (i < nobj)
    {
        for (int j = 0; j < nobj; j++) VtCall(objs[j], 66, 0x1FA328);
        return;
    }
    for (int j = 0; j < nobj; j++) StepAloBackHalfFrame(objs[j]);
    UpdateXpPositions(ncont, conts);
    UpdateXpWorldPositions(ncont, conts);
}

namespace
{
// orig: 0x001F9240 func_001F9240 (a group's contacts (at most 0x40): kind 1 always (none: nothing); kind 4 only when
// a kind-0xB contact is off by more than 0.1 or another is deeper than 2; kind 2 when one approaches by more than 5)
void SolveGroupPass(int k, std::vector<ALO*>& apx)
{
    std::vector<XP*> aps;
    const int n = CollectGroupXps(apx, 0x40, &aps);
    if (k & 1)
    {
        if (n == 0) k = 0;
    }
    else
    {
        if (k & 4)
        {
            int i = 0;
            for (; i < n; i++)
            {
                const float f = aps[i]->gDist;
                if (aps[i]->xpk == 0xB)
                {
                    if (0.100000001f < std::fabs(f)) break;
                }
                else if (f < -2.0f)
                    break;
            }
            if (!(i < n)) k &= ~4;
        }
        if (k & 2)
        {
            int i = 0;
            for (; i < n; i++)
            {
                const float d = Dot3(aps[i]->dv, aps[i]->normal);
                if (aps[i]->xpk == 0xB)
                {
                    if (5.0f < std::fabs(d)) break;
                }
                else if (d < -5.0f)
                    break;
            }
            if (!(i < n)) k &= ~2;
        }
    }
    if (k) SolveGroup(k, int(apx.size()), apx.data(), n, aps.data());
}
}

// orig: 0x001F8F80 func_001F8F80 (a pass of kind k: every unmarked active SO with a live mass state (with k & 1: not
// both records still) starts a group that is solved; then the marks are cleared)
void SolvePassSw(int k)
{
    std::vector<OXA*> aoxa = g_physw.apoxaActive;
    for (OXA* poxa : aoxa)
    {
        ALO* s0 = poxa->pso;
        if (!s0) continue;
        SOST* pe = Psost(s0);
        if (!pe) continue;
        if (FMark(s0, FL148_MARK27)) continue;
        if (pe->cnstrLin.kind == 3 && pe->cnstrAng.kind == 3) continue;
        if (k & 1)
        {
            if (PxfPos(s0) && PxfRot(s0) && FXfModeStill(PxfPos(s0)->mode) && FXfModeStill(PxfRot(s0)->mode)) continue;
        }
        std::vector<ALO*> apx;
        GatherGroup(s0, &apx);
        SolveGroupPass(k, apx);
    }
    for (OXA* poxa : g_physw.apoxaActive)
        if (poxa->pso && poxa->pso->psort) poxa->pso->psort->grf148 &= ~(FL148_MARK27 | FL148_MARK28);
}

// orig: 0x001F8DD8 func_001F8DD8 (the frame's physics: the contacts of the moved objects (0x134040), slot 17 of
// every active entry, the pass of kind 1, the contacts again, the pass of kind 6)
void UpdateSwPhys()
{
    // (debug: SLY2N_RANDATPHYS="n:state[;n:state..]": the random state set at the n-th physics frame's start (counted
    // from 0 at the process start), e.g. a reference's at its 0x1F8DD8 entries (SLY2_DUMP_CALL))
    if (static const char* s_sz = std::getenv("SLY2N_RANDATPHYS"); s_sz)
    {
        static uint64_t s_c = 0;
        for (const char* q = s_sz; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            if (static_cast<uint64_t>(std::strtoull(q, nullptr, 10)) == s_c && std::strchr(q, ':'))
            {
                extern uint32_t g_uRandState;
                g_uRandState = static_cast<uint32_t>(std::strtoul(std::strchr(q, ':') + 1, nullptr, 0));
            }
        ++s_c;
    }
    static CONTACTTRACE trace;
    const bool observe = trace.oid >= 0 && trace.frame >= static_cast<uint64_t>(trace.first) &&
                         trace.frame <= static_cast<uint64_t>(trace.last);
    if (observe) TraceContactPhase(trace, "before_contacts");
    UpdateSwXps();
    if (observe) TraceContactPhase(trace, "after_contacts");
    if (g_pfnUpdateSwEntries17)
        g_pfnUpdateSwEntries17();
    else
        for (size_t i = 0; i < s_aploPhys.size(); i++)
        {
            LO* plo = s_aploPhys[i];
            if ((GrfUpdOfLo(plo) & 0x24) == 4) VtUpdate17(plo);
        }
    if (observe) TraceContactPhase(trace, "after_integration");
    SolvePassSw(1);
    if (observe) TraceContactPhase(trace, "after_solve1");
    UpdateSwXps();
    if (observe) TraceContactPhase(trace, "after_refreshed_contacts");
    SolvePassSw(6);
    if (observe) TraceContactPhase(trace, "after_solve6");
    if (trace.oid >= 0) ++trace.frame;
}

// ---- The carryables (cid 0x64 family: Bentley's mine ...; carry.cpp has their state)

// orig: 0x00224350 func_00224350 (vt slot 55 of the carryables: a new contact (base 0x1F4FB0: nothing) gets the
// elasticity 0.25; e.g. Bentley stepping onto his dropped mine bounces off it)
void OnCarryXpNew(ALO*, XP* pxp, int) { pxp->elas = 0.25f; }
NATIVEFN(0x224350, OnCarryXpNew);

// orig: 0x00224698 func_00224698 (vt slot 70 of the carryables: no gravity in the states 0 / 4 (held / gone); else
// the gravity (0x1F9B20), and for the kinds 0x1A / 0x29 the rotation pulled toward a level frame facing the target
// +0x160 (0x11B830; without one, the object's frame levelled, 0x11B938) with CLQ (0, 45, 0) / (0, 7.5, 0) toward rest)
void ApplyCarryGravity(ALO* p)
{
    const CARRY* pc = PcarryFromLo(p);
    const int st = pc ? pc->state : -1;
    if (st == 0 || st == 4) return;
    ApplyAloGravity(p);
    if (!pc || (pc->kind != 0x1A && pc->kind != 0x29)) return;
    glm::mat3 m(1.0f);
    if (pc->plo160 && FCidDerivedFrom(pc->plo160->Cid(), 0x30))
    {
        glm::vec3 d = PosAloCenter(static_cast<ALO*>(pc->plo160)) - PosAloCenter(p);
        const glm::vec3 up(0.0f, 0.0f, 1.0f);  // D_2A2DC0
        d = d - up * Dot3(d, up);
        const float len = std::sqrt(std::fabs(Dot3(d, d)));
        const glm::vec3 n = len < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : d * (1.0f / len);  // D_2A2DA0
        m = BuildLookAtMatrix(n, up);
    }
    else
    {
        XF* pxf = PxfObj(p);
        LevelMatrix3(pxf ? Mat3Of(pxf->matWorld) : glm::mat3(1.0f), &m);
    }
    static const CLQ s_clq{0.0f, 45.0f, 0.0f, 0.0f}, s_clqV{0.0f, 7.5f, 0.0f, 0.0f};  // D_2C4300 / D_2C4310
    AccelAloTowardRotation(p, &m, &s_clq, glm::vec3(0.0f), &s_clqV);
}
NATIVEFN(0x224698, ApplyCarryGravity);

int OverlapKindAlo(ALO* pa, ALO* pb);  // bbmark.cpp: 0x1F4FC8

// orig: 0x002242E0 func_002242E0 (vt slot 61 of the carryables: the ALO's overlap kind, at least 1 (no contacts)
// with an actor (cid 0x34 family) while held or gone (states 0 / 4): the held mine does not push its carrier)
int OverlapKindCarry(ALO* p, ALO* pOther)
{
    int v = OverlapKindAlo(p, pOther);
    const CARRY* pc = PcarryFromLo(p);
    const int st = pc ? pc->state : -1;
    if ((st == 0 || st == 4) && pOther && FCidDerivedFrom(pOther->Cid(), 0x34)) v = std::max(v, 1);
    return v;
}
NATIVEFN(0x2242E0, OverlapKindCarry);
