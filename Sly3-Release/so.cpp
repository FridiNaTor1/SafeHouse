#include <execinfo.h>
#include "game/sly3.h"
#include "worldglobals.h"
// The ALO family's physics runtime (orig module 0x1F2108-0x1F8DD8; so.h). Ported from the reference's so_p3.cpp,
// so_c.cpp (oracle-verified ports of the original's code) and the decomp; the order of operations is kept.

#include "so.h"
#include "damage.h"
#include "stepphys.h"

#include "mat.h"
#include "util.h"
#include "vtnative.h"
#include "anim.h"
#include "clock.h"
#include "xfphys.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cmath>


PHYSW g_physw;
WORLD_TABLE(g_physw);   // (PC: moved aside while a kept world loads, worldglobals.h)
bool (*g_pfnFAloActive)(ALO* palo) = nullptr;

namespace
{
constexpr uint64_t FL148_ROOT = 1ull << 34;
constexpr uint64_t FL148_ICED = 1ull << 35;
inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline float Len3(const glm::vec3& a) { return std::sqrt(std::fabs(Dot3(a, a))); }

template <class T>
bool FContains(const std::vector<T>& a, const T& t)
{
    return std::find(a.begin(), a.end(), t) != a.end();
}
template <class T>
void RemoveFrom(std::vector<T>& a, const T& t)
{
    auto it = std::find(a.begin(), a.end(), t);
    if (it != a.end()) a.erase(it);
}

// The vtable calls of the SO slots used here (the ALO's own functions when the class has no native override).
void VtSetOverlap(ALO* p, ALO* pOther)  // slot 54 (ALO: 0x1F5988, nothing)
{
    using PFN = void (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 54))) pfn(p, pOther);
}
bool VtFStill(ALO* p)  // slot 62 (ALO: 0x1F5040)
{
    using PFN = bool (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 62))) return pfn(p);
    return FAloStill(p);
}
OXA* VtPoxaNew(ALO* p)  // slot 53 (ALO: 0x1F56F8)
{
    using PFN = OXA* (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 53))) return pfn(p);
    return PoxaAddSw(p);
}
void VtApplyGravity(ALO* p)  // slot 70 (ALO: 0x1F9B20, solve.cpp)
{
    using PFN = void (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 70))) pfn(p);
    else if (PFN pfnAlo = reinterpret_cast<PFN>(PvNativeFn(0x1F9B20))) pfnAlo(p);
}
void VtUpdateXfWorlds(ALO* p)  // slot 18 (ALO: 0x22FBE0)
{
    using PFN = void (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 18))) pfn(p);
    else UpdateAloXfWorldsHierarchy(p);
}

// orig: 0x001F5988 func_001F5988 (ALO vt slot 54: nothing)
void SetAloOverlapNothing(ALO*, ALO*) {}
NATIVEFN(0x1F5988, SetAloOverlapNothing);
// orig: 0x001F56F8 func_001F56F8 (ALO vt slot 53: a new attachment, 0x1336F0)
OXA* PoxaNewAlo(ALO* p) { return PoxaAddSw(p); }
NATIVEFN(0x1F56F8, PoxaNewAlo);
}

void PHYSW::Reset()
{
    fAaoxDirty = false;
    for (auto& a : aaox) a.clear();
    apoxa.clear();
    apoxaActive.clear();
    apoxaDirty1.clear();
    apoxaDirty2.clear();
    aoxaOwned.clear();
    actmRecent.clear();
    axpBlock.clear();
    axpFree.clear();
    ResetStepPhysAll();
}

XP* PHYSW::PxpNew()
{
    if (axpFree.empty())
    {
        constexpr int cBlock = 256;
        axpBlock.push_back(std::make_unique<XP[]>(cBlock));
        for (int i = cBlock - 1; i >= 0; i--) axpFree.push_back(&axpBlock.back()[i]);
    }
    XP* pxp = axpFree.back();
    axpFree.pop_back();
    *pxp = XP{};
    return pxp;
}

void PHYSW::FreeXp(XP* pxp) { axpFree.push_back(pxp); }

SORT& Sort(ALO* palo)
{
    if (!palo->psort) palo->psort = std::make_shared<SORT>();
    return *palo->psort;
}

// orig: 0x001F8300 (the region entry, made when missing: radius 150 for a player (0x34), else 75, mass 1)
REGENT* PregentEnsure(ALO* palo)
{
    SOST* ps = Psost(palo);
    if (!ps) return nullptr;
    if (!ps->pregent)
    {
        auto pre = std::make_shared<REGENT>();
        pre->palo = palo;
        pre->sRadius = FCidDerivedFrom(palo->Cid(), S3CID(0x34)) ? 150.0f : 75.0f;
        pre->gMass = 1.0f;
        ps->pregent = pre;
    }
    return ps->pregent.get();
}

// The runtime part of 0x1F2108 (the SO's init, vt slot 7: bytes -10 / -20, fade 1, +0x13C 400, flag 35 from the
// parent (1 without), +0x140 -1).
void InitAloPhys(ALO* p)
{
    SORT& s = Sort(p);
    s.s148 = -10;
    s.s149 = -20;
    s.g13C = 400.0f;  // 0x2C3B90
    s.nTreeStamp = -1;
    const uint64_t b = p->paloParent ? ((Sort(p->paloParent).grf148 >> 35) & 1) : 1;
    s.grf148 = (s.grf148 & ~FL148_ICED) | (b << 35);
}

// The loader 0x1F31E0 after the entries: their owner, the entries into the attachment (0x1F2B20, before the mass
// state exists, as the original).
void LoadAloPhysEntries(ALO* p)
{
    for (SOE& e : p->asoe)
    {
        e.palo = p;
        e.nStampWorld = 0;
        e.nStampObj = 0;
        e.fInOxa = false;
    }
    AddAloSoes(p);
}

// The loader 0x1F31E0's mass state (when the ALO has entries): the mass and inertia loaded, 0x1F35F8 (1), vt slot 64.
void LoadAloPhysState(ALO* p)
{
    SORT& s = Sort(p);
    s.psost = std::make_unique<SOST>();
    s.psost->m = p->gMass;
    s.psost->momint = p->matInertia;
    ScaleAloMomint(p, 1.0f);
    using PFN = void (*)(ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 64))) pfn(p);
    else RestAloSost(p);
}

// The attachment's part of 0x1F3EA8 (vt slot 23, the SO enters the world): the attachment it owns onto the active
// list, its bounds (slot 49).
void OnAloEnterWorldPhys(ALO* p)
{
    OXA* pe = Poxa(p);
    if (pe && pe->pso == p)
    {
        if (std::find(g_physw.apoxaActive.begin(), g_physw.apoxaActive.end(), pe) == g_physw.apoxaActive.end())
            g_physw.apoxaActive.push_back(pe);
        using PFN = void (*)(ALO*);
        if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 49))) pfn(p);
        else UpdateAloBounds(p);
    }
}

// The attachment's part of 0x1F3E00 (vt slot 22, the SO leaves the world): its bounds (slot 49), off the active list.
void OnAloLeaveWorldPhys(ALO* p)
{
    OXA* pe = Poxa(p);
    if (pe && pe->pso == p)
    {
        using PFN = void (*)(ALO*);
        if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 49))) pfn(p);
        else UpdateAloBounds(p);
        RemoveFrom(g_physw.apoxaActive, pe);
    }
}

// The physics part of the clone 0x1F6068 (vt slot 41) after the world's copy of the ALO's data: the runtime copied
// (the copy keeps its own attachment), the entries' owner, mesh and stamps, the mass state; the records' actuators
// are not copied (the actuators' owners re-add theirs: 0x12E3F0 / 0x230018); the entries into the attachment when
// the copy is in the world (0x1F2B20).
void CloneAloPhys(const ALO* src, ALO* dst)
{
    SORT& d = Sort(dst);
    if (src->psort)
    {
        const SORT& s = *src->psort;
        d.dpos = s.dpos;
        d.drot = s.drot;
        d.abXp = s.abXp;
        d.g13C = s.g13C;
        d.n138 = s.n138;
        d.s148 = s.s148;
        d.s149 = s.s149;
        d.grf148 = s.grf148 & ~((1ull << 27) | (1ull << 28));
        d.psost = s.psost ? std::make_unique<SOST>(*s.psost) : nullptr;
        // (0x1F6068: the region entry copied, owned by the copy; its region list joined at its add / update)
        if (d.psost && d.psost->pregent)
        {
            auto pre = std::make_shared<REGENT>(*d.psost->pregent);
            pre->palo = dst;
            pre->preg = nullptr;
            pre->nGenNav = 0;
            d.psost->pregent = pre;
        }
    }
    for (auto& pxf : dst->axf) pxf->pctrlFirst = nullptr;
    for (SOE& e : dst->asoe)
    {
        e.palo = dst;
        e.fInOxa = false;
        if (e.pmesh) e.pmesh = std::make_shared<SOMESH>(*e.pmesh);
    }
    CloneStepPhys(dst, src);
    if (dst->fInWorld) AddAloSoes(dst);
}

// The physics part of 0x1F2C30 (vt slot 11): the records' world matrices (0x22FAA0), the entries into the
// attachment (0x1F2B20), the update flags 0x14.
void PostLoadAloPhys(ALO* p)
{
    UpdateAloXfWorlds(p);
    AddAloSoes(p);
    SetGrfUpdOfLo(p, GrfUpdOfLo(p) | 0x14);
}

// orig: 0x001F2798 func_001F2798 (the physics root: up the parents until one with a live mass state (not both
// constraints fixed) or flag bit 34)
ALO* PaloPhysRoot(ALO* p)
{
    ALO* r = nullptr;
    while (p)
    {
        r = p;
        if (SOST* ps = Psost(p))
        {
            if (ps->cnstrLin.kind != 3 || ps->cnstrAng.kind != ps->cnstrLin.kind) break;
        }
        if (p->psort && (p->psort->grf148 & FL148_ROOT)) break;
        p = p->paloParent;
    }
    return r;
}

// orig: 0x001F27F0 func_001F27F0 (the attachment, shared with the root's (made by its vt slot 53, registered))
OXA* PoxaEnsure(ALO* p)
{
    SORT& s = Sort(p);
    if (s.poxa) return s.poxa;
    ALO* r = PaloPhysRoot(p);
    SORT& sr = Sort(r);
    if (!sr.poxa)
    {
        sr.poxa = VtPoxaNew(r);
        RecalcSwOxfFilterForObject(r);
    }
    s.poxa = sr.poxa;
    return s.poxa;
}

// orig: 0x001F2870 func_001F2870 (an entry into the attachment's list: its matrix and stamp from its object, the
// world geometry refreshed; 0x1F5078 unless vt slot 62)
void AddSoeToOxa(ALO* p, SOE* pe)
{
    OXA* poxa = PoxaEnsure(p);
    if (pe->fInOxa && FContains(poxa->apsoe, pe)) return;
    poxa->apsoe.push_back(pe);
    pe->fInOxa = true;
    pe->nStampWorld = -1;
    pe->mat = pe->pxf ? pe->pxf->matWorld : g_matIdentity4;
    pe->nStampObj = pe->pxf ? pe->pxf->nStamp : 0;
    MarkAloDirty(p);
    UpdateSoeWorld(pe);
    if (!VtFStill(p)) WakeAlo(p);
}

namespace
{
// orig: 0x001F4E28 func_001F4E28 (detaches the contacts: both objects of each overlap told (slot 54), its contacts
// freed; slot 54 (none) and the attachment's internal contacts freed)
void DetachAloContacts(ALO* p)
{
    OXA* poxa = Poxa(p);
    if (!poxa) return;
    MarkAloDirty(p);
    for (OX* pn = poxa->pox; pn; pn = pn->poxNext)
    {
        VtSetOverlap(poxa->pso, pn->psoOther);
        VtSetOverlap(pn->psoOther, poxa->pso);
        FreeSwXpList(pn->pxp);
        if (pn->poxOther) pn->poxOther->pxp = nullptr;
        pn->pxp = nullptr;
    }
    VtSetOverlap(p, nullptr);
    FreeSwXpList(poxa->pxpInternal);
    poxa->pxpInternal = nullptr;
}
}

// orig: 0x001F4E28 (exported: the contacts detached)
void DetachAloContactsAll(ALO* p) { DetachAloContacts(p); }

// orig: 0x001F6860 (the attachment's contacts off (+0x70 bit 4: bbmark.cpp skips it) or on, the contacts detached)
void SetAloContactsOff(ALO* p, bool fOff)
{
    OXA* poxa = Poxa(p);
    if (!poxa) return;
    poxa->grf = (poxa->grf & ~4ull) | (fOff ? 4ull : 0ull);
    DetachAloContacts(p);  // 0x1F4E28
}

// orig: 0x001F2930 func_001F2930 (an entry out of the attachment; the contacts detached; an emptied attachment
// dropped (0x1337D0), one owned by another SO left when none of this one's entries is in it)
void RemoveSoeFromOxa(ALO* p, SOE* pe)
{
    OXA* poxa = Poxa(p);
    if (!poxa) return;
    if (!pe->fInOxa || !FContains(poxa->apsoe, pe)) return;
    RemoveFrom(poxa->apsoe, pe);
    pe->fInOxa = false;
    if (pe->f51) RemoveSafeGroundReferences(nullptr, pe);  // 0x1A00D0 (safe-place histories)
    DetachAloContacts(p);
    if (poxa->apsoe.empty())
    {
        if (poxa->pso) Sort(poxa->pso).poxa = nullptr;
        Sort(p).poxa = nullptr;
        RemoveSwOxa(poxa);
    }
    else if (poxa->pso != p)
    {
        bool fAny = false;
        for (SOE& e : p->asoe)
            if (e.fInOxa && FContains(poxa->apsoe, &e)) fAny = true;
        if (!fAny) Sort(p).poxa = nullptr;
    }
    if (!VtFStill(p)) WakeAlo(p);
}

// orig: 0x001F2B20 func_001F2B20 (0x1F2870 for each live entry)
void AddAloSoes(ALO* p)
{
    for (SOE& e : p->asoe)
        if (!e.f50) AddSoeToOxa(p, &e);
}

// orig: 0x001F2BA8 func_001F2BA8 (0x1F2930 for each live entry)
void RemoveAloSoes(ALO* p)
{
    for (SOE& e : p->asoe)
        if (!e.f50) RemoveSoeFromOxa(p, &e);
}

// orig: 0x001F2A60 (an entry back on: into the attachment when the object is in the world)
namespace
{
// (debug: SLY2N_SOETOGGLE=1: the contact entries (id != 0: hazards, sensors) switched on / off, with the time)
void PrintSoeToggle(ALO* p, const SOE* pe, bool fOn)
{
    static const bool s_f = std::getenv("SLY2N_SOETOGGLE") != nullptr;
    if (s_f && pe->id)
        std::printf("soetoggle t %.3f %d (cid 0x%X) tag %d id 0x%X st %d %s\n", g_clock.t, p->oid, p->Cid(), pe->tag, pe->id, pe->state,
                    fOn ? "on" : "off");
}
}  // namespace

void SetSoeOn(ALO* p, SOE* pe)
{
    if (!pe->f50) return;
    PrintSoeToggle(p, pe, true);
    pe->f50 = false;
    if (p->fInWorld) AddSoeToOxa(p, pe);
}

// orig: 0x001F2AD0 func_001F2AD0 (an entry off (bit 50): out of the attachment when the SO is in the world)
void SetSoeOff(ALO* p, SOE* pe)
{
    if (pe->f50) return;
    PrintSoeToggle(p, pe, false);
    pe->f50 = true;
    if (p->fInWorld) RemoveSoeFromOxa(p, pe);
}

// orig: 0x001F35F8 func_001F35F8 (the inertia scaled by s, its inverse (0 when singular))
void ScaleAloMomint(ALO* p, float s)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    ps->momint = ps->momint * s;
    if (!FInvertMatrix3(ps->momint, &ps->momintInv)) ps->momintInv = glm::mat3(0.0f);
}

// orig: 0x001F35C8 func_001F35C8 (the mass m: the inertia scaled by m / mass)
void SetAloMass(ALO* p, float m)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    const float s = m / ps->m;
    ps->m = ps->m * s;
    ScaleAloMomint(p, s);
}

// orig: 0x001F3590 func_001F3590 (ALO vt slot 64: at rest: gravity (0, 0, -1750), both constraints fixed)
void RestAloSost(ALO* p)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    ps->dvGravity = glm::vec3(0.0f, 0.0f, -1750.0f);
    ps->cnstrLin.kind = 3;
    ps->cnstrAng.kind = 3;
}
NATIVEFN(0x1F3590, RestAloSost);

// orig: 0x001F3900 func_001F3900 (each live entry's matrix and stamp from its object)
void CopyAloSoeMatrices(ALO* p)
{
    for (SOE& e : p->asoe)
    {
        if (e.f50) continue;
        e.mat = e.pxf ? e.pxf->matWorld : g_matIdentity4;
        e.nStampObj = e.pxf ? e.pxf->nStamp : 0;
    }
}

// orig: 0x001F3980 func_001F3980 (ALO vt slot 17: the entries' matrices and meshes, the records' velocities into
// the mass state, the velocity records' accelerations cleared and gravity (slot 70), the records' update
// (0x22DE88), the live entries' world geometry)
void UpdateAloPhys(ALO* p)
{
    CopyAloSoeMatrices(p);
    for (SOE& e : p->asoe)
        if (e.pmesh) IntegrateSoeMesh(&e, g_clock.dt);
    if (SOST* ps = Psost(p))
    {
        XF* p58 = PxfPos(p);
        XF* p5C = PxfRot(p);
        if (p58) ps->v = glm::vec3(p58->vel);
        if (p5C) ps->w = glm::vec3(p5C->vel);
        if (p58 && p5C)
        {
            const int m58 = p58->mode, m5C = p5C->mode;
            if (static_cast<uint16_t>(m58 - 1) < 2 || m5C == 1 || m5C == 2)
            {
                if (FXfModeVelocity(m58)) p58->aseg[3] = glm::vec4(glm::vec3(0.0f), p58->aseg[3].w);
                if (FXfModeVelocity(p5C->mode)) p5C->aseg[3] = glm::vec4(glm::vec3(0.0f), p5C->aseg[3].w);
                VtApplyGravity(p);
            }
        }
    }
    UpdateAloXfs(p, g_clock.t, g_clock.dt);
    for (SOE& e : p->asoe)
        if (!e.f50) UpdateSoeWorld(&e);
}
NATIVEFN(0x1F3980, UpdateAloPhys);

// orig: 0x001664A0 func_001664A0 (a GEOM's positions and planes into another's by the matrix: positions M p, the
// planes' normals by the inverse's transpose, normalized, d through their first position)
void TransformGeom(const glm::mat4& m, const GEOM& src, GEOM* pdst)
{
    const size_t cpos = std::min(src.apos.size(), pdst->apos.size());
    for (size_t i = 0; i < cpos; i++) pdst->apos[i] = XformPos(m, src.apos[i]);
    const glm::mat3 it = glm::transpose(InverseMatrix3(Mat3Of(m)));
    const size_t csurf = std::min(src.asurf.size(), pdst->asurf.size());
    for (size_t i = 0; i < csurf; i++)
    {
        glm::vec3 n = it * src.asurf[i].normal;
        n = n * (1.0f / Len3(n));  // (the micro program 0x150 normalizes)
        SURF& d = pdst->asurf[i];
        d.normal = n;
        const int ipos = d.ipos0;
        const glm::vec3 pos = (ipos >= 0 && size_t(ipos) < pdst->apos.size()) ? pdst->apos[ipos] : glm::vec3(0.0f);
        d.gDot = Dot3(n, pos);
    }
}

// orig: 0x001F38A8 func_001F38A8 (the entry's world geometry when its object's stamp changed (0x1664A0); the owner
// marked dirty)
void UpdateSoeWorld(SOE* pe)
{
    const int32_t nStamp = pe->pxf ? pe->pxf->nStamp : 0;
    if (pe->nStampWorld == nStamp) return;
    if (pe->pxf) TransformGeom(pe->pxf->matWorld, pe->geomLocal, &pe->geomWorld);
    pe->nStampWorld = nStamp;
    if (pe->palo) MarkAloDirty(pe->palo);
}

// orig: 0x001F4B58 func_001F4B58 (the attachment on the dirty list 1 (SW +0x3D28))
void MarkOxaDirty1(ALO* p)
{
    OXA* poxa = Poxa(p);
    if (!poxa || (poxa->grf & 1)) return;
    poxa->grf |= 1;
    g_physw.apoxaDirty1.push_back(poxa);
}

// orig: 0x001F4BA8 func_001F4BA8
void UnmarkOxaDirty1(ALO* p)
{
    OXA* poxa = Poxa(p);
    if (!poxa || !(poxa->grf & 1)) return;
    poxa->grf &= ~1ull;
    RemoveFrom(g_physw.apoxaDirty1, poxa);
}

// orig: 0x001F4BF8 func_001F4BF8 (dirty: list 1, then list 2 (SW +0x3D34))
void MarkAloDirty(ALO* p)
{
    MarkOxaDirty1(p);
    OXA* poxa = Poxa(p);
    if (!poxa || (poxa->grf & 2)) return;
    poxa->grf |= 2;
    g_physw.apoxaDirty2.push_back(poxa);
}

// orig: 0x001F4D50 func_001F4D50
void UnmarkAloDirty(ALO* p)
{
    UnmarkOxaDirty1(p);
    OXA* poxa = Poxa(p);
    if (!poxa || !(poxa->grf & 2)) return;
    poxa->grf &= ~2ull;
    RemoveFrom(g_physw.apoxaDirty2, poxa);
}

// orig: 0x001F50E0 func_001F50E0 (ALO vt slot 49: the box of the attachment's live entries (their world geometry
// refreshed; spheres by their radius, else the world points, plus the appended local points through the entry's
// matrix when its stamp changed), the centre and the radius about it; the sweep lists dirty; off dirty list 1)
void UpdateAloBounds(ALO* p)
{
    OXA* poxa = Poxa(p);
    if (!poxa) return;
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    auto extend = [&](const glm::vec3& v) {
        mx = glm::max(mx, v);
        mn = glm::min(mn, v);
    };
    for (SOE* s : poxa->apsoe)
    {
        if (s->f50) continue;
        UpdateSoeWorld(s);
        if (s->f49)
        {
            const float r = s->sRadius;
            const glm::vec3 c = s->pxf ? PosOf(s->pxf->matWorld) : glm::vec3(0.0f);
            mn = glm::min(mn, c + glm::vec3(1.0f) * -r);
            mx = glm::max(mx, c + glm::vec3(1.0f) * r);
        }
        else
            for (const glm::vec3& v : s->geomWorld.apos) extend(v);
        if (s->nStampWorld == s->nStampObj) continue;
        const int n = int(s->geomWorld.apos.size()), start = s->pinned;
        for (int k = start; k < n && k < int(s->geomLocal.apos.size()); k++) extend(XformPos(s->mat, s->geomLocal.apos[k]));
    }
    poxa->posMin = mn;
    const glm::vec3 c = mn * 0.5f + mx * 0.5f;
    poxa->posMax = mx;
    float r = 0.0f;
    for (SOE* s : poxa->apsoe)
    {
        if (s->f49)
        {
            const glm::vec3 pos = s->pxf ? PosOf(s->pxf->matWorld) : glm::vec3(0.0f);
            r = std::max(r, Len3(c - pos) + s->sRadius);
        }
        else
            for (const glm::vec3& v : s->geomWorld.apos) r = std::max(r, Len3(c - v));
        if (s->nStampWorld != s->nStampObj)
        {
            const int n = int(s->geomWorld.apos.size()), start = s->pinned;
            for (int k = start; k < n && k < int(s->geomLocal.apos.size()); k++)
                r = std::max(r, Len3(c - XformPos(s->mat, s->geomLocal.apos[k])));
        }
    }
    poxa->posCenter = c;
    poxa->sRadius = r;
    g_physw.fAaoxDirty = true;
    UnmarkOxaDirty1(p);
}
NATIVEFN(0x1F50E0, UpdateAloBounds);

// orig: 0x001F5078 func_001F5078 (with an attachment: update flag 4 (slot 47); with the mass state both records
// woken (0x230598))
void WakeAlo(ALO* p)
{
    if (!Poxa(p)) return;
    OrLoUpdateFlags(p, 4);
    if (!Psost(p)) return;
    if (PxfPos(p)) HoldXfMode(p, PxfPos(p));
    if (PxfRot(p)) HoldXfMode(p, PxfRot(p));
}

// orig: 0x001F5040 func_001F5040 (ALO vt slot 62: no mass state, or both constraints fixed)
bool FAloStill(ALO* p)
{
    SOST* ps = Psost(p);
    if (!ps) return true;
    return ps->cnstrLin.kind == 3 && ps->cnstrAng.kind == 3;
}
NATIVEFN(0x1F5040, FAloStill);

// orig: 0x001F6EF8 func_001F6EF8 (the entry's moving points over dt: velocity += acceleration dt, position +=
// (old + new velocity) dt / 2 (a move longer than 1 makes the world geometry stale), the largest point distance
// into the radius, the triangles' planes)
void IntegrateSoeMesh(SOE* pe, float dt)
{
    SOMESH* pm = pe->pmesh.get();
    if (!pm) return;
    float g2Max = 0.0f;
    const float h = dt * 0.5f;
    for (size_t i = 0; i < pe->geomLocal.apos.size() && i < pm->av.size() && i < pm->aa.size(); i++)
    {
        const glm::vec3 vel = pm->av[i];
        const glm::vec3 vnew = vel + pm->aa[i] * dt;
        glm::vec3& pos = pe->geomLocal.apos[i];
        const glm::vec3 next = pos + vel * h + vnew * h;
        if (pe->nStampWorld != -1)
        {
            const glm::vec3 d = pos - next;
            if (1.0f < Dot3(d, d)) pe->nStampWorld = -1;
        }
        pos = next;
        pm->av[i] = vnew;
        g2Max = std::max(g2Max, Dot3(next, next));
    }
    pe->sRadius = std::sqrt(g2Max);
    for (size_t i = 0; i < pm->atri.size() && i < pe->geomLocal.asurf.size(); i++)
    {
        const auto& t = pm->atri[i];
        const glm::vec4 pl = PlaneFromPoints(pe->geomLocal.apos[t[0]], pe->geomLocal.apos[t[1]], pe->geomLocal.apos[t[2]]);
        pe->geomLocal.asurf[i].normal = glm::vec3(pl);
        pe->geomLocal.asurf[i].gDot = pl.w;
    }
}

// orig: 0x001F5510 func_001F5510 (a constraint by CNSTR k: its kind (0x2C3C20), its axis (0x2C3BA0) through the
// record's matrix, normalized (zero when shorter than 1e-4))
void SetCnstrAxis(XF* pxf, int k, CNSTRS* pc)
{
    static const int s_akind[8] = {0, 3, 2, 2, 2, 1, 1, 1};
    static const glm::vec3 s_aaxis[8] = {glm::vec3(0.0f), glm::vec3(0.0f), g_normalX, g_normalY, g_normalZ, g_normalZ, g_normalY, g_normalX};
    if (k < 0 || k > 7) k = 0;
    pc->kind = s_akind[k];
    const glm::vec3 v = pxf ? Mat3Of(pxf->mat) * s_aaxis[k] : s_aaxis[k];
    const float l = Len3(v);
    pc->normal = (l < 1e-4f) ? g_vecZero : v * (1.0f / l);
}

// orig: 0x001F5720 func_001F5720 (the position record constrained by k: velocity mode unless it has actuators or
// is in mode 2; the roots re-linked; not iced unless fixed)
void SetAloCnstrLin(ALO* p, int k)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    XF* o = PxfPos(p);
    if (o && !o->pctrlFirst && o->mode != 2) SetXfMode(p, o, 1);
    SetCnstrAxis(o, k, &ps->cnstrLin);
    ReRootAlo(p);
    if (ps->cnstrLin.kind != 3) SetAloIced(p, false);
}

// orig: 0x001F57C0 func_001F57C0 (the same for the rotation record)
void SetAloCnstrAng(ALO* p, int k)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    XF* o = PxfRot(p);
    if (o && !o->pctrlFirst && o->mode != 2) SetXfMode(p, o, 1);
    SetCnstrAxis(o, k, &ps->cnstrAng);
    ReRootAlo(p);
    if (ps->cnstrAng.kind != 3) SetAloIced(p, false);
}

// orig: 0x001F5880 func_001F5880 (the gravity vector (+0x80; the position record in velocity mode unless it has
// actuators or is in mode 2), 0x1F5078)
void SetAloGravity(ALO* p, const glm::vec3& dv)
{
    SOST* ps = Psost(p);
    if (!ps) return;
    XF* o = PxfPos(p);
    if (o && !o->pctrlFirst && o->mode != 2) SetXfMode(p, o, 1);
    ps->dvGravity = dv;
    WakeAlo(p);
}

// orig: 0x001F55C8 func_001F55C8 (an SO in the world whose physics root changed: the ALOs linked to it move their
// entries to their own root's attachment; the SO is re-registered (0x134320), 0x1F5078). Native: the linked SOs
// (0x15E9C8: the world's object search, kind 0x30 under p) are p and its ALO descendants.
void ReRootAlo(ALO* p)
{
    if (!p->fInWorld) return;
    OXA* poxa = Poxa(p);
    if (poxa && PaloPhysRoot(p) != poxa->pso)
    {
        std::vector<ALO*> apo;
        auto collect = [&](auto&& self, ALO* pa) -> void {
            if (apo.size() >= 0x40) return;
            apo.push_back(pa);
            for (LO* pc : pa->aploChild)
                if (FCidDerivedFrom(pc->Cid(), S3CID(0x30))) self(self, static_cast<ALO*>(pc));
        };
        collect(collect, p);
        for (ALO* po : apo)
        {
            if (!Poxa(po)) continue;
            ALO* q = PaloPhysRoot(po);
            if (q == Poxa(po)->pso) continue;
            RemoveAloSoes(po);
            SORT& sq = Sort(q);
            if (!sq.poxa)
            {
                sq.poxa = VtPoxaNew(q);
                RecalcSwOxfFilterForObject(q);
            }
            Sort(po).poxa = sq.poxa;
            AddAloSoes(po);
        }
    }
    RecalcSwOxfFilterForObject(p);
    WakeAlo(p);
}

// orig: 0x001F7F48 func_001F7F48 (flag bit 34 (a physics root); the roots re-linked when it changes)
void SetAloPhysRootFlag(ALO* p, bool f)
{
    SORT& s = Sort(p);
    if (f == (((s.grf148 >> 34) & 1) != 0)) return;
    s.grf148 = (s.grf148 & ~FL148_ROOT) | (uint64_t(f) << 34);
    ReRootAlo(p);
}

// orig: 0x001F8668 func_001F8668 (flag bit 35 (iced) of the SO and its ALO children; announced to the visibility
// tree when the game is live: the renderer's, not modelled)
void SetAloIced(ALO* p, bool f)
{
    SORT& s = Sort(p);
    if (f == (((s.grf148 >> 35) & 1) != 0)) return;
    s.grf148 = (s.grf148 & ~FL148_ICED) | (uint64_t(f) << 35);
    for (LO* pc : p->aploChild)
        if (FCidDerivedFrom(pc->Cid(), S3CID(0x30))) SetAloIced(static_cast<ALO*>(pc), f);
}

// orig: 0x001A62E8 func_001A62E8 (the object's position: its world matrix (vt slot 31: ALO 0x1F3FA0, the object
// record's))
glm::vec3 PosAloCenter(ALO* p)
{
    XF* pxf = PxfObj(p);
    return pxf ? PosOf(pxf->matWorld) : glm::vec3(0.0f);
}

// orig: 0x001F4440 func_001F4440 (ALO vt slot 33: the position record's velocity at its world position (0x22D348))
glm::vec3 VelAloPos(ALO* p)
{
    XF* pt = PxfPos(p);
    if (!pt || pt->kind != 1) return g_vecZero;
    return VelXfPoint(pt, nullptr, PosOf(pt->matWorld));
}
NATIVEFN(0x1F4440, VelAloPos);

// (fLoad: the world loader's velocity options, EOPIDs 29 / 30 / 75 / 76, anim.cpp.) The original applies them while the
// object loads: its records' world matrices were just made (0x22FAA0) from the local matrices as read and from the
// records loaded before it; a record loaded later (e.g. an ancestor loaded after the object, as world 14's 73074 above
// 8845) did not exist yet, and a copy's options were converted once, in its template (CloneLo copies the velocity).
// Natively the world matrices are not made here and placements / starts have changed some locals, so the load-time
// matrices are composed from each record's loaded local (XF::matLoad), up to the object's own load (XF::nLoad).
// R55: world 32's watermills (42546 / 42127 / 40859 / 41714, option 76 (0 0 w)) spin about their wheels' axles, the
// local z: the reference's records hold w times their z rows ((-0.471 0.518 0) for 42546); natively (R16's raw
// vector) world z. tools/veloptregress.py checks every world's options against the original's post-load RAM.
static const XF* PxfLoadOf(const XF* pxf)
{
    for (int i = 0; pxf && pxf->palo && pxf->palo->ploCloneOf && i < 8; ++i)
    {
        const ALO* pt = static_cast<const ALO*>(pxf->palo->ploCloneOf);
        const ALO* pa = pxf->palo;
        const XF* pnext = nullptr;
        for (std::size_t j = 0; j < pa->axf.size() && j < pt->axf.size(); ++j)
            if (pa->axf[j].get() == pxf) { pnext = pt->axf[j].get(); break; }
        if (!pnext) break;
        pxf = pnext;
    }
    return pxf;
}
static uint32_t NLoadOfAlo(const ALO* palo)
{
    for (int i = 0; palo && palo->ploCloneOf && i < 8; ++i) palo = static_cast<const ALO*>(palo->ploCloneOf);
    uint32_t n = 0;
    if (palo)
        for (const auto& up : palo->axf) n = std::max(n, up->nLoad);
    return n;
}
static glm::mat3 Mat3LoadWorld(const XF* pxf, uint32_t nLoadMax)
{
    glm::mat3 m(1.0f);
    for (int i = 0; (pxf = PxfLoadOf(pxf)) && pxf->nLoad <= nLoadMax && i < 64; pxf = pxf->pxfRef, ++i)
        m = Mat3Of(pxf->matLoad) * m;
    return m;
}
static glm::vec3 VecConvertXfsLoad(const ALO* palo, const XF* pxfFrom, const XF* pxfTo, const glm::vec3& v)
{
    const uint32_t n = NLoadOfAlo(palo);
    glm::vec3 r = pxfFrom ? Mat3LoadWorld(pxfFrom, n) * v : v;
    return pxfTo ? InverseMatrix3(Mat3LoadWorld(pxfTo, n)) * r : r;
}

// orig: 0x001F4488 func_001F4488 (ALO vt slot 35: the position record's velocity set so that it reaches q from
// slot 33's position, in its parent's space; woken, dirty, 0x1F5078, not iced)
void SetAloVelocityToLoad(ALO* p, const glm::vec3& q, bool fLoad)
{
    XF* po = PxfPos(p);
    if (!po || po->kind != 1) return;
    if (!FXfModeMoving(po->mode)) SetXfMode(p, po, 1);
    po->vel = glm::vec4(g_vecZero, po->vel.w);
    using PFN = glm::vec3 (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 33));
    const glm::vec3 cur = pfn ? pfn(p) : VelAloPos(p);
    po->vel = glm::vec4(fLoad ? VecConvertXfsLoad(p, nullptr, po->pxfRef, q - cur) : VecConvertXfs(nullptr, po->pxfRef, q - cur), po->vel.w);
    HoldXfMode(p, po);
    MarkAloDirty(p);
    WakeAlo(p);
    SetAloIced(p, false);
}
void SetAloVelocityTo(ALO* p, const glm::vec3& q) { SetAloVelocityToLoad(p, q, false); }
NATIVEFN(0x1F4488, SetAloVelocityTo);

// orig: 0x001F4558 func_001F4558 (ALO vt slot 34: the rotation record's angular velocity: its motion matrix
// (0x22D148) times the inverse of its world rotation (normalized), the axial components)
glm::vec3 AngVelAlo(ALO* p)
{
    XF* s2 = PxfRot(p);
    if (!s2 || s2->kind != 6) return g_vecZero;
    const glm::mat4 dm = DmatXfChain(s2, nullptr);
    glm::mat3 n = Mat3Of(s2->matWorld);
    NormalizeRotateMatrix3(&n);
    const glm::mat3 inv = InverseMatrix3(Mat3Of(s2->matWorld));
    const glm::mat3 r = n * inv;  // rows: the normalized rows combined by the inverse's
    const glm::mat3 o = r * Mat3Of(dm);
    return glm::vec3(o[1][2], o[2][0], o[0][1]);
}
NATIVEFN(0x1F4558, AngVelAlo);

// orig: 0x001F46D8 func_001F46D8 (ALO vt slot 36: the rotation record's angular velocity set so that it reaches
// pos from slot 34's, in its parent's space)
void SetAloAngVelocityToLoad(ALO* p, const glm::vec3& pos, bool fLoad)
{
    XF* obj = PxfRot(p);
    if (!obj || obj->kind != 6) return;
    if (!FXfModeMoving(obj->mode)) SetXfMode(p, obj, 1);
    obj->vel = glm::vec4(g_vecZero, obj->vel.w);
    using PFN = glm::vec3 (*)(ALO*);
    PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 34));
    const glm::vec3 cur = pfn ? pfn(p) : AngVelAlo(p);
    const glm::mat3 w = fLoad ? Mat3LoadWorld(obj, NLoadOfAlo(p)) : Mat3Of(obj->matWorld);
    glm::mat3 n = w;
    NormalizeRotateMatrix3(&n);
    const glm::mat3 inv = InverseMatrix3(n);
    const glm::mat3 r = w * inv;
    const glm::vec3 v = r * (pos - cur);
    obj->vel = glm::vec4(fLoad ? VecConvertXfsLoad(p, nullptr, obj->pxfRef, v) : VecConvertXfs(nullptr, obj->pxfRef, v), obj->vel.w);
    HoldXfMode(p, obj);
    MarkAloDirty(p);
    WakeAlo(p);
    SetAloIced(p, false);
}
void SetAloAngVelocityTo(ALO* p, const glm::vec3& pos) { SetAloAngVelocityToLoad(p, pos, false); }
NATIVEFN(0x1F46D8, SetAloAngVelocityTo);

// orig: 0x001F4898 func_001F4898 (the position record's velocity v from the object's space; EOPID 75)
void SetAloVelocity(ALO* p, const glm::vec3& v, bool fLoad)
{
    XF* po = PxfPos(p);
    if (!po) return;
    if (!FXfModeMoving(po->mode)) SetXfMode(p, po, 1);
    po->vel = glm::vec4(fLoad ? VecConvertXfsLoad(p, PxfObj(p), po->pxfRef, v) : VecConvertXfs(PxfObj(p), po->pxfRef, v), po->vel.w);
    HoldXfMode(p, po);
    MarkAloDirty(p);
    WakeAlo(p);
    SetAloIced(p, false);
}

// orig: 0x001F4930 func_001F4930 (the rotation record's angular velocity w from the object's space)
void SetAloAngVelocity(ALO* p, const glm::vec3& w, bool fLoad)
{
    XF* po = PxfRot(p);
    if (!po) return;
    if (!FXfModeMoving(po->mode)) SetXfMode(p, po, 1);
    po->vel = glm::vec4(fLoad ? VecConvertXfsLoad(p, PxfObj(p), po->pxfRef, w) : VecConvertXfs(PxfObj(p), po->pxfRef, w), po->vel.w);
    HoldXfMode(p, po);
    MarkAloDirty(p);
    WakeAlo(p);
    SetAloIced(p, false);
}

// orig: 0x001F40A0 func_001F40A0 (ALO vt slot 29: the position record moved so that the object lands at pos)
void SetAloPos(ALO* p, const glm::vec3& pos)
{
    if (static const char* s_szMove = std::getenv("SLY2N_MOVEPRINT"); s_szMove && std::atoi(s_szMove) == p->oid)
    {
        void* apv[16];
        std::fprintf(stderr, "move %d by SetAloPos t %.3f:\n", p->oid, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 16), 2);
    }
    XF* p58 = PxfPos(p);
    XF* obj = PxfObj(p);
    if (!p58 || !obj) return;
    glm::mat4 target(1.0f);  // the object's rotation at pos
    SetMat3(&target, Mat3Of(obj->matWorld));
    SetPos(&target, pos);
    // the object relative to the position record (inverse(record world) x object world, in row terms)
    const glm::mat4 rel = MulMatrix4(InverseAffineMatrix4(p58->matWorld), obj->matWorld);
    const glm::mat4 par = p58->pxfRef ? p58->pxfRef->matWorld : g_matIdentity4;
    const glm::mat4 m = MulMatrix4(MulMatrix4(InverseAffineMatrix4(par), target), InverseAffineMatrix4(rel));
    SetPos(&p58->mat, PosOf(m));
    p58->nStamp = ++g_cXfStamp;
    VtUpdateXfWorlds(p);
    DetachAloContacts(p);
    HoldXfMode(p, p58);
    CopyAloSoeMatrices(p);
    UpdateAloRegion(p);  // the region entry (0x1F37C8 with 0x1C0D70 at the new place)
}
NATIVEFN(0x1F40A0, SetAloPos);

// orig: 0x001F42E0 func_001F42E0 (ALO vt slot 30: the rotation record's matrix set to the world basis, relative to
// its parent's normalized rotation)
void SetAloRotation(ALO* p, const glm::mat3& basis)
{
    if (static const char* s_szMove = std::getenv("SLY2N_MOVEPRINT"); s_szMove && std::atoi(s_szMove) == p->oid)
    {
        void* apv[16];
        std::fprintf(stderr, "move %d by SetAloRotation t %.3f:\n", p->oid, g_clock.t);
        backtrace_symbols_fd(apv, backtrace(apv, 16), 2);
    }
    XF* obj = PxfRot(p);
    if (!obj) return;
    glm::mat3 par(1.0f);
    if (obj->pxfRef)
    {
        par = Mat3Of(obj->pxfRef->matWorld);
        NormalizeRotateMatrix3(&par);
    }
    const glm::mat3 inv = InverseMatrix3(par);
    SetMat3(&obj->mat, inv * basis);
    obj->nStamp = ++g_cXfStamp;
    VtUpdateXfWorlds(p);
    DetachAloContacts(p);
    HoldXfMode(p, obj);
    CopyAloSoeMatrices(p);
}
NATIVEFN(0x1F42E0, SetAloRotation);

// orig: 0x001F59A0 func_001F59A0 (angular braking of the rotation record toward the orientation mat (in world space)
// with {max speed a, b, time c}: the deceleration (a - b) / c, the stopping angle a^2 / (2 decel); an overspeed cut
// (2.5 (a - |w|) / |w|); the remaining rotation's angle and axis; the wanted speed from the angle left after half a
// frame; the record's acceleration gets the change toward it, bounded by decel dt)
void BrakeAloRotation(ALO* p, const glm::mat3& mat, const float* ac)
{
    const float a = ac[0], b = ac[1], cc = ac[2];
    const float a2 = a * a;
    XF* o = PxfRot(p);
    if (!o) return;
    // (the EE's divisions (DivEe): a speed of 0 (a 0 scale, 0x204D48) gives 0 / 0 here, +FLT_MAX on the PS2, which leaves
    // the record as it is; IEEE's NaN made the actor's whole pose NaN (N37: Carmelita not drawn in her helicopter))
    const float acc = DivEe(a - b, cc);
    const float wl = std::sqrt(std::fabs(Dot3(glm::vec3(o->vel), glm::vec3(o->vel))));
    const float stop = DivEe(a2, acc + acc);
    if (a < wl)
    {
        const float f = DivEe((a - wl) * 2.5f, wl);
        o->aseg[3] = glm::vec4(glm::vec3(o->aseg[3]) + glm::vec3(o->vel) * f, o->aseg[3].w);
    }
    glm::mat3 m = MatConvertXfs(nullptr, o->pxfRef, mat);
    NormalizeRotateMatrix3(&m);
    float ang;
    glm::vec3 axis;
    DecomposeRotateMatrixRad(MulMatrix3TransposeA(Mat3Of(o->mat), m), &ang, &axis);
    float speed = GLimitAbs(DivEe(ang, stop) * a, a);
    const float wa = Dot3(glm::vec3(o->vel), axis);
    const float h = g_clock.dt * 0.5f;
    const float left = ang - (wa + speed) * h;
    if (left < stop) speed = GLimitAbs(DivEe(left, stop) * a, a);
    const glm::vec3 want = axis * speed;
    const float t = g_clock.dt;
    const glm::vec3 pred = glm::vec3(o->vel) + glm::vec3(o->aseg[3]) * t;
    const float lim = acc * t;
    const glm::vec3 ch = want - pred;
    const float cl = std::sqrt(std::fabs(Dot3(ch, ch)));
    const float k = (cl < lim) ? 1.0f / t : DivEe(acc, cl);
    o->aseg[3] = glm::vec4(glm::vec3(o->aseg[3]) + ch * k, o->aseg[3].w);
}

// orig: 0x001F5C30 func_001F5C30 (the rotation record pulled toward the orientation mat with the quadratic coefficients
// pclq of the remaining angle (as an acceleration about its axis), and toward the angular velocity w with pclqV (the
// change scaled by min(1 / dt, u0 / |d| + u1 + u2 |d|)))
void AccelAloTowardRotation(ALO* p, const glm::mat3* pmat, const CLQ* pclq, const glm::vec3& w, const CLQ* pclqV)
{
    XF* s1 = PxfRot(p);
    if (!s1) return;
    if (pclq && pmat)
    {
        glm::mat3 m = MatConvertXfs(nullptr, s1->pxfRef, *pmat);
        NormalizeRotateMatrix3(&m);
        float ang;
        glm::vec3 axis;
        DecomposeRotateMatrixRad(MulMatrix3TransposeA(Mat3Of(s1->mat), m), &ang, &axis);
        if (1e-4f < ang)
        {
            float f0 = pclq->u2 * ang;
            const float f1 = pclq->u1 * ang;
            f0 = f0 * ang;
            f0 = f0 + f1;
            f0 = f0 + pclq->u0;
            s1->aseg[3] = glm::vec4(glm::vec3(s1->aseg[3]) + axis * f0, s1->aseg[3].w);
        }
    }
    if (pclqV)
    {
        const glm::vec3 d = VecConvertXfs(nullptr, s1->pxfRef, w) - glm::vec3(s1->vel);
        const float len = std::sqrt(std::fabs(Dot3(d, d)));
        if (1e-4f < len)
        {
            const float f2 = pclqV->u0 / len;
            float f1 = pclqV->u2 * len;
            const float f0 = 1.0f / g_clock.dt;
            f1 = f1 + pclqV->u1;
            f1 = f1 + f2;
            if (f0 < f1) f1 = f0;
            s1->aseg[3] = glm::vec4(glm::vec3(s1->aseg[3]) + d * f1, s1->aseg[3].w);
        }
    }
}

// orig: 0x001F5E10 func_001F5E10 (the position record pulled toward pos with pclq (the acceleration (u0 / |d| + u1 +
// u2 |d|) d) and toward the velocity v with pclqV (min(1 / dt, ...) of the change; v from slot 33))
void AccelAloTowardPosition(ALO* p, const glm::vec3& pos, const CLQ* pclq, const glm::vec3& v, const CLQ* pclqV)
{
    XF* s3 = PxfPos(p);
    if (!s3) return;
    if (pclq)
    {
        const glm::vec3 d = pos - PosAloCenter(p);
        const float len = std::sqrt(std::fabs(Dot3(d, d)));
        if (1e-4f < len)
        {
            const float f1 = pclq->u0 / len;
            float f0 = pclq->u2 * len;
            f0 = f0 + pclq->u1;
            f0 = f0 + f1;
            s3->aseg[3] = glm::vec4(glm::vec3(s3->aseg[3]) + VecConvertXfs(nullptr, s3->pxfRef, d) * f0, s3->aseg[3].w);
        }
    }
    if (pclqV)
    {
        using PFN = glm::vec3 (*)(ALO*);
        PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 33));
        const glm::vec3 vcur = pfn ? pfn(p) : VelAloPos(p);
        const glm::vec3 d = v - vcur;
        const float len = std::sqrt(std::fabs(Dot3(d, d)));
        if (1e-4f < len)
        {
            const float f2 = pclqV->u0 / len;
            float f1 = pclqV->u2 * len;
            const float f0 = 1.0f / g_clock.dt;
            f1 = f1 + pclqV->u1;
            f1 = f1 + f2;
            if (f0 < f1) f1 = f0;
            s3->aseg[3] = glm::vec4(glm::vec3(s3->aseg[3]) + VecConvertXfs(nullptr, s3->pxfRef, d) * f1, s3->aseg[3].w);
        }
    }
}

// orig: 0x001F7E08 func_001F7E08 (the entries pfn accepts (all without pfn))
int CsoeCollect(ALO* p, bool (*pfn)(void*, SOE*), void* pv, std::vector<SOE*>* papsoe)
{
    int n = 0;
    for (SOE& e : p->asoe)
        if (!pfn || pfn(pv, &e))
        {
            papsoe->push_back(&e);
            n++;
        }
    return n;
}

// orig: 0x001F7918 func_001F7918 (the contact bytes +0x110 whose bits are set in f, to b)
void SetAloXpBytes(ALO* p, uint32_t f, uint8_t b)
{
    SORT& s = Sort(p);
    uint32_t m = 1;
    for (int i = 0; i < 26; i++, m <<= 1)
        if (m & f) s.abXp[i] = b;
}

// orig: 0x001F78D8 func_001F78D8 (the or of the contact bytes whose bits are set in f)
uint32_t GrfAloXpBytes(ALO* p, uint32_t f)
{
    if (!p->psort) return 0;
    uint32_t r = 0, m = 1;
    for (int i = 0; i < 26; i++, m <<= 1)
        if (m & f) r |= p->psort->abXp[i];
    return r;
}

// orig: 0x001F7A68 func_001F7A68 (the 2-bit mode (flag bits 24..25) k: entering 2 the entries with id 0 get 0x10000
// and the byte state +0x148, entering 0 / 1 / 3 those with 0x10000 go back to 0 and state -10)
void SetAloEntryMode(ALO* p, int k)
{
    SORT& s = Sort(p);
    if (k == int((s.grf148 >> 24) & 3)) return;
    uint32_t from = 0, to = 0;
    int st = 0;
    if (k == 2)
    {
        st = s.s148;
        from = 0;
        to = 0x10000;
    }
    else if (k == 0 || k == 1 || k == 3)
    {
        from = 0x10000;
        to = 0;
        st = -10;
    }
    else
        return;  // (another k: the original uses whatever its registers held)
    for (SOE& e : p->asoe)
    {
        if (e.id != from) continue;
        e.state = static_cast<int16_t>(st);
        e.id = to;
    }
    s.grf148 = (s.grf148 & ~(3ull << 24)) | (uint64_t(k & 3) << 24);
}

// The physics part of 0x1F2D48 (vt slot 12, the SO leaves the world): the entries out of the attachment (0x1F2BA8);
// (the fade's reset to 1 at rest is activate.cpp's OnRemove: ALO::gFade / dgFade, the world's 0x1F36E0 runs it)
void ResetAloPhys(ALO* p)
{
    RemoveAloSoes(p);
}

// orig: 0x001F2E78's physics part (vt slot 13, the SO moved from pOld to pNew; after the base 0x1A5B80): slot 13 of
// the children, then, with entries, when no removed object (flag bit 56) lies from the SO up to pNew and neither
// pOld's nor the SO's parent's chains hold one, the entries are taken out and put back (0x1F2BA8, 0x1F2B20)
void MoveAloPhys(ALO* p, ALO* pNew, ALO* pOld)
{
    for (LO* pc : std::vector<LO*>(p->aploChild))
        if (FCidDerivedFrom(pc->Cid(), S3CID(0x30))) MoveAloPhys(static_cast<ALO*>(pc), pNew, pOld);
    if (p->asoe.empty() || !p->fInWorld) return;  // (native: an object out of the world has no entries added)
    for (ALO* po = p; po; po = po->paloParent)
    {
        if (po->fRemoved) return;
        if (po == pNew) break;
    }
    ALO* a = pOld;
    while (a && !a->fRemoved) a = a->paloParent;
    ALO* b = p->paloParent;
    while (b && !b->fRemoved) b = b->paloParent;
    if (!a && !b)
    {
        RemoveAloSoes(p);
        AddAloSoes(p);
    }
}

// ---- Contact messages (orig 0x1F7410 / 0x1F7598 / 0x1F7950 / 0x1F77C0): a contact offered up the receiver's parents
// to the first whose contact bytes match the filter and whose vt slot 75 accepts it; the accepted ones are kept 0.25 s
// (a contact is delivered once while it lasts) and, for filters with bits 0xF8, announced by messages (vt slot 4:
// NotifyBasicListeners, message 11) and splice events (vt slot 3: 10 / 11). The message and event slots are the
// world's / splice's (null: not sent).

CTMSG::CTMSG() = default;
constinit void (*g_pfnLoNotify)(LO* plo, int msgk, void* pv) = nullptr;
constinit void (*g_pfnLoEvent)(LO* plo, int n, int a, int b, void* pv, LO* plo2) = nullptr;

namespace
{
inline ALO* PaloOrParent(LO* plo)  // orig 0x1A5BF0 (the object when it is an ALO, else its parent)
{
    if (!plo) return nullptr;
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) return static_cast<ALO*>(plo);
    return plo->paloParent;
}
bool VtFAcceptContact(ALO* p, CTMSG* pctm)  // slot 75 (ALO: 0x1F7588: the filter matched)
{
    using PFN = bool (*)(ALO*, CTMSG*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 75))) return pfn(p, pctm);
    return pctm->grfAccepted != 0;
}
}

// orig: 0x001F7588 func_001F7588 (ALO vt slot 75: accepted when the filter matched)
bool FAcceptContactAlo(ALO*, CTMSG* pctm) { return pctm->grfAccepted != 0; }
NATIVEFN(0x1F7588, FAcceptContactAlo);

// orig: 0x001F7598 func_001F7598 (offers the contact to the receiver and its parents: the first one already holding it
// (same sender, other, filter) stops it; else the first whose bytes match and that accepts it gets it; its record is
// kept; filters with bits 0xF8 are announced. The receiver, null when nobody took it.)
ALO* PaloAcceptContact(CTMSG* pctm)
{
    static const bool s_fCtmsg = std::getenv("SLY2_CTMSG") != nullptr;  // (debug: every contact message offered)
    if (s_fCtmsg)
        std::fprintf(stderr, "[ctmsg] t %.3f from %d to %d filter 0x%X state %d pos %.1f %.1f %.1f vec %.1f %.1f %.1f\n", g_clock.t, pctm->ploSender ? pctm->ploSender->oid : -1,
                     pctm->ploReceiver ? pctm->ploReceiver->oid : -1, pctm->grfFilter, pctm->nState, pctm->pos.x, pctm->pos.y, pctm->pos.z, pctm->vec.x, pctm->vec.y, pctm->vec.z);
    ALO* pso = PaloOrParent(pctm->ploReceiver);
    while (pso)
    {
        for (const CTMSG& r : g_physw.actmRecent)
            if (r.ploSender == pctm->ploSender && r.pvOther == pctm->pvOther && r.grfFilter == pctm->grfFilter &&
                r.paloAccepted == pso)
                return nullptr;
        pctm->grfAccepted = GrfAloXpBytes(pso, pctm->grfFilter);
        static const bool s_fDbg = std::getenv("SLY2N_SENSDEBUG") != nullptr;
        if (s_fDbg)
            std::printf("contact offer to %d (cid 0x%X): filter 0x%X bytes 0x%X slot75 %p\n", pso->oid, pso->Cid(), pctm->grfFilter,
                        pctm->grfAccepted, PvNativeVt(pso, 75));
        if (VtFAcceptContact(pso, pctm))
        {
            pctm->paloAccepted = pso;
            CTMSG r = *pctm;
            r.t = g_clock.t;
            g_physw.actmRecent.push_back(r);
            if (pctm->grfAccepted & 0xF8)
            {
                struct
                {
                    ALO* pso;
                    LO* ploSender;
                    CTMSG* pctm;
                } msg{pso, pctm->ploSender, pctm};
                if (g_pfnLoNotify)
                {
                    g_pfnLoNotify(pso, 0xB, &msg);
                    g_pfnLoNotify(pctm->ploSender, 0xB, &msg);
                }
                if (g_pfnLoEvent)
                {
                    g_pfnLoEvent(pctm->ploSender, 0xA, 1, 2, pctm, pso);
                    g_pfnLoEvent(pso, 0xB, 1, 2, pctm, pctm->ploSender);
                }
            }
            return pso;
        }
        pso = pso->paloParent;
    }
    return nullptr;
}

// orig: 0x001F77C0 func_001F77C0 (the world's contact records older than a quarter second dropped)
void AgeSwContacts()
{
    auto& a = g_physw.actmRecent;
    a.erase(std::remove_if(a.begin(), a.end(), [](const CTMSG& r) { return !(g_clock.t < r.t + 0.25f); }), a.end());
}

// orig: 0x001F7410 func_001F7410 (ALO vt slot 74: each sensor contact of the attachment (+0x6C) offered from its other
// end's object to the first parent with matching bytes (0x1F7530); then the list freed)
void DispatchAloSensorContacts(ALO* p)
{
    OXA* poxa = Poxa(p);
    if (!poxa) return;
    for (XP* pxp = poxa->pxpSensor; pxp; pxp = pxp->pxpNext)
    {
        const int iMine = (pxp->axpd[0].paloRoot != p) ? 1 : 0;  // the end of p's entry
        XPD& e = pxp->axpd[iMine];
        XPD& eOther = pxp->axpd[1 - iMine];
        static const bool s_fDbg = std::getenv("SLY2N_SENSDEBUG") != nullptr;
        if (s_fDbg)
            std::printf("sensor contact of %d %p: mine tag %d id 0x%X, other %d %p tag %d at %.0f %.0f %.0f\n", p->oid, (void*)p,
                        e.psoe ? e.psoe->tag : -1, e.psoe ? e.psoe->id : 0, eOther.palo ? eOther.palo->oid : -1,
                        eOther.paloRoot ? (void*)eOther.paloRoot : nullptr, eOther.psoe ? eOther.psoe->tag : -1, eOther.pos.x, eOther.pos.y, eOther.pos.z);
        if (s_fDbg && e.psoe)  // (debug: the entry's shape: sphere flag and radius, the world geometry's box)
        {
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (const glm::vec3& v : e.psoe->geomWorld.apos) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
            std::printf("   entry f48 %d f49 %d f50 %d r %.1f npos %d box %.0f %.0f %.0f .. %.0f %.0f %.0f xf %p\n", e.psoe->f48, e.psoe->f49, e.psoe->f50,
                        e.psoe->sRadius, int(e.psoe->geomWorld.apos.size()), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, (void*)e.psoe->pxf);
        }
        if (s_fDbg && e.psoe && e.psoe->id)
            for (XF* q = e.pxf; q; q = q->pxfRef)  // (debug: the velocity chain of the sender's entry)
                std::printf("   xf %p of %d kind %d mode %d state %d vel %.1f %.1f %.1f world %.0f %.0f %.0f -> vel %.1f %.1f %.1f\n", (void*)q,
                            q->palo ? q->palo->oid : -1, q->kind, q->mode, q->state, q->vel.x, q->vel.y, q->vel.z, q->matWorld[3].x,
                            q->matWorld[3].y, q->matWorld[3].z, [&] { glm::vec3 c = VelXfPoint(q, q->pxfRef, e.pos); if (q->pxfRef) c = glm::vec3(q->pxfRef->matWorld * glm::vec4(c, 0.0f)); return c; }().x,
                            [&] { glm::vec3 c = VelXfPoint(q, q->pxfRef, e.pos); if (q->pxfRef) c = glm::vec3(q->pxfRef->matWorld * glm::vec4(c, 0.0f)); return c; }().y,
                            [&] { glm::vec3 c = VelXfPoint(q, q->pxfRef, e.pos); if (q->pxfRef) c = glm::vec3(q->pxfRef->matWorld * glm::vec4(c, 0.0f)); return c; }().z);
        if (!e.psoe || !e.psoe->id) continue;
        ALO* pso = nullptr;
        for (ALO* q = eOther.palo; q; q = q->paloParent)  // 0x1F7530
            if (GrfAloXpBytes(q, e.psoe->id))
            {
                pso = q;
                break;
            }
        if (!pso) continue;
        CTMSG ctm;
        ctm.ploSender = p;
        ctm.pvOther = e.psoe;
        ctm.ploReceiver = pso;
        ctm.grfFilter = e.psoe->id;
        ctm.nState = e.psoe->state;
        ctm.pos = eOther.pos;
        ctm.vec = e.vel;
        PaloAcceptContact(&ctm);
        if (!Poxa(p)) return;
    }
    if (OXA* pq = Poxa(p))
    {
        FreeSwXpList(pq->pxpSensor);
        pq->pxpSensor = nullptr;
    }
}
NATIVEFN(0x1F7410, DispatchAloSensorContacts);

// orig: 0x001F7950 func_001F7950 (the sensor contacts dispatched (slot 74); with an entry mode (flag bits 24..25) the
// mode reset (0x1F7A68 0) and the object's own contact message: from the player's step guards (mode 3, cid 0x38: filter
// 0x40000, state -50, from the player (0x2F7900)) or from itself (filter 0x20000, its byte +0x149))
void EmitAloContact(ALO* p, LO* ploPlayer)
{
    if (!Poxa(p)) return;
    if (Poxa(p)->pxpSensor)
    {
        using PFN = void (*)(ALO*);
        PFN pfn = reinterpret_cast<PFN>(PvNativeVt(p, 74));
        (pfn ? pfn : DispatchAloSensorContacts)(p);
    }
    if (!Poxa(p)) return;
    const int kind = int((Sort(p).grf148 >> 24) & 3);
    if (!kind) return;
    if (std::getenv("SLY2_CTMSG"))  // (debug: the overlaps that raised it)
        if (OXA* poxa = Poxa(p))
            for (OX* ox = poxa->pox; ox; ox = ox->poxNext)
                for (XP* x = ox->pxp; x; x = x->pxpNext)
                    std::fprintf(stderr, "[ctmsg]   %d overlap with %d (cid 0x%X) xp k%d tags %d / %d n %.2f %.2f %.2f d %.2f pos %.1f %.1f %.1f\n", p->oid,
                                 ox->psoOther ? ox->psoOther->oid : -1, ox->psoOther ? ox->psoOther->Cid() : 0, x->xpk,
                                 x->axpd[0].psoe ? x->axpd[0].psoe->tag : -1, x->axpd[1].psoe ? x->axpd[1].psoe->tag : -1, x->normal.x,
                                 x->normal.y, x->normal.z, x->gDist, x->axpd[0].pos.x, x->axpd[0].pos.y, x->axpd[0].pos.z);
    SetAloEntryMode(p, 0);
    CTMSG ctm;
    ctm.ploReceiver = p;
    ctm.pos = PosAloCenter(p);
    using PFN33 = glm::vec3 (*)(ALO*);
    PFN33 pfn33 = reinterpret_cast<PFN33>(PvNativeVt(p, 33));
    ctm.vec = pfn33 ? pfn33(p) : VelAloPos(p);
    if (kind == 3 && FCidDerivedFrom(p->Cid(), S3CID(0x38)))
    {
        ctm.grfFilter = 0x40000;
        ctm.nState = -50;
        ctm.ploSender = ploPlayer;
    }
    else
    {
        ctm.grfFilter = 0x20000;
        ctm.nState = Sort(p).s149;
        ctm.ploSender = p;
    }
    PaloAcceptContact(&ctm);
}

// orig: 0x001F72F8 func_001F72F8 (ALO vt slot 73: the point inside one of the live entries: a sphere (bit 49) by its
// radius, else its BSP (0x118FB0))
bool FAloContainsPoint(ALO* p, const glm::vec3& pos)
{
    for (const SOE& e : p->asoe)
    {
        if (e.f50) continue;
        if (e.f49)
        {
            const glm::vec3 d = pos - (e.pxf ? PosOf(e.pxf->matWorld) : glm::vec3(0.0f));
            if (Dot3(d, d) < e.sRadius * e.sRadius) return true;
        }
        if (!e.bsp.abspn.empty() && IbspFindPoint(BSPREF(e.bsp, e.geomWorld), 0, INT32_MAX, pos) != IBSP_NONE) return true;
    }
    return false;
}
NATIVEFN(0x1F72F8, FAloContainsPoint);

// orig: 0x00166270 func_00166270 (a GEOM copied and transformed by the matrix: positions M p, normals by its 3x3,
// normalized, d through their first position; no matrix: the copy as it is)
void CopyTransformGeom(const GEOM& src, const glm::mat4* pmat, GEOM* pdst)
{
    *pdst = src;
    if (!pmat) return;
    for (glm::vec3& v : pdst->apos) v = XformPos(*pmat, v);
    const glm::mat3 m = Mat3Of(*pmat);
    for (SURF& s : pdst->asurf)
    {
        glm::vec3 n = m * s.normal;
        n = n * (1.0f / Len3(n));
        s.normal = n;
        const glm::vec3 pos = (s.ipos0 >= 0 && size_t(s.ipos0) < pdst->apos.size()) ? pdst->apos[s.ipos0] : glm::vec3(0.0f);
        s.gDot = Dot3(n, pos);
    }
}

// orig: 0x001F6C08 func_001F6C08 (an entry's moving-points mesh, once: zeroed velocities / accelerations for its
// points, each surface's three positions gathered from the edges, oriented along the surface's plane)
void EnsureSoeMesh(SOE* pe)
{
    if (pe->pmesh) return;
    auto pm = std::make_shared<SOMESH>();
    const GEOM& g = pe->geomLocal;
    pm->av.assign(pe->geomWorld.apos.size(), glm::vec3(0.0f));
    pm->aa.assign(pe->geomWorld.apos.size(), glm::vec3(0.0f));
    const size_t csurf = pe->geomWorld.asurf.size();
    pm->atri.assign(csurf, {0, 0, 0});
    std::vector<int> ac(csurf, 0);
    for (const EDGE& ed : pe->geomWorld.aedge)
    {
        const int af[2] = {ed.isurf, ed.isurfOther};
        for (int f = 0; f < 2; f++)
        {
            const int face = af[f];
            if (face < 0 || size_t(face) >= csurf) continue;
            const int av[2] = {ed.ipos0, ed.ipos1};
            for (int v = 0; v < 2; v++)
            {
                const int vtx = av[v];
                int j = 0;
                for (; j < ac[face]; j++)
                    if (pm->atri[face][j] == vtx) break;
                if (j < ac[face]) continue;
                if (j < 3)
                {
                    pm->atri[face][ac[face]] = vtx;
                    ac[face]++;
                }
            }
        }
    }
    for (size_t i = 0; i < csurf && i < g.asurf.size(); i++)
    {
        auto& t = pm->atri[i];
        auto at = [&](int k) { return size_t(t[k]) < g.apos.size() ? g.apos[t[k]] : glm::vec3(0.0f); };
        const glm::vec3 a = at(1) - at(0), b = at(2) - at(0);
        if (Dot3(glm::cross(a, b), g.asurf[i].normal) < 0.0f) std::swap(t[1], t[2]);
    }
    pe->pmesh = pm;
}

// orig: 0x001F70C0 func_001F70C0 (the entry dst rebuilt from src in the space of the record pxfSpace with n zeroed points
// inserted after the pinned ones: owner, tag, pinned count, the largest point radius; the local geometry through
// inverse(space) x src's object matrix, the world one through space, the tree copied)
void RebuildSoe(const SOE* src, XF* pxfSpace, int n, SOE* dst)
{
    dst->pxf = pxfSpace;
    dst->palo = src->palo;
    dst->tag = src->tag;
    dst->pinned = src->pinned;
    float mx = 0.0f;
    for (const glm::vec3& v : src->geomLocal.apos) mx = std::max(mx, Dot3(v, v));
    dst->sRadius = std::sqrt(mx);
    GEOM geo = src->geomLocal;
    const int pin = std::min<int>(src->pinned, int(geo.apos.size()));
    geo.apos.insert(geo.apos.begin() + pin, size_t(std::max(0, n)), glm::vec3(0.0f));
    const glm::mat4 matSpace = pxfSpace ? pxfSpace->matWorld : g_matIdentity4;
    const glm::mat4 matSrc = src->pxf ? src->pxf->matWorld : g_matIdentity4;
    const glm::mat4 m = MulMatrix4(InverseAffineMatrix4(matSpace), matSrc);
    CopyTransformGeom(geo, &m, &dst->geomLocal);
    CopyTransformGeom(dst->geomLocal, &matSpace, &dst->geomWorld);
    dst->bsp = src->bsp;
}

// orig: 0x001F58F0 func_001F58F0 (ALO vt slot 50: the object's pose: position, its velocity (0x22D348), the heading of
// its leveled frame's first row (atan2(-y, -x)), 100, the clock's time)
void GetAloPose(ALO* p, SOPOSE* ppose)
{
    XF* o = PxfObj(p);
    const glm::vec3 q = o ? PosOf(o->matWorld) : glm::vec3(0.0f);
    ppose->pos = q;
    ppose->vel = o ? VelXfPoint(o, nullptr, q) : glm::vec3(0.0f);
    glm::mat3 lev;
    LevelMatrix3(o ? Mat3Of(o->matWorld) : g_matIdentity3, &lev);
    ppose->radHeading = std::atan2(-lev[0][1], -lev[0][0]);
    ppose->g34 = 100.0f;
    ppose->n0 = 0;
    ppose->n3C = 0;
    ppose->t = g_clock.t;
}
NATIVEFN(0x1F58F0, GetAloPose);

// orig: 0x001F21B8 func_001F21B8 (ALO vt slot 10: the ALO moved under paloNew (its record pxfNew, null: paloNew's object
// record) keeping, per grf, its world position (1) / rotation (2) and its velocities (4 / 8, slots 33 / 34 then 35 /
// 36): the records that referred to the old parent's records follow, the world's base move (0x1A5900: pfnBaseMove),
// the world matrices, the kept values set back; not iced under a parent that is not)
void ReparentAlo(ALO* p, ALO* paloNew, XF* pxfNew, int grf, void (*pfnBaseMove)(ALO*, ALO*, XF*, int))
{
    glm::mat4 W(1.0f);
    if (grf & 3)
    {
        if (p->fInWorld)
            W = PxfObj(p) ? PxfObj(p)->matWorld : g_matIdentity4;
        else
            for (XF* x = PxfObj(p); x; x = x->pxfRef) W = MulMatrix4(x->mat, W);
    }
    glm::vec3 q1(0.0f), q2(0.0f);
    using PFNV = glm::vec3 (*)(ALO*);
    if (grf & 4)
    {
        PFNV pfn = reinterpret_cast<PFNV>(PvNativeVt(p, 33));
        q1 = pfn ? pfn(p) : VelAloPos(p);
    }
    if (grf & 8)
    {
        PFNV pfn = reinterpret_cast<PFNV>(PvNativeVt(p, 34));
        q2 = pfn ? pfn(p) : AngVelAlo(p);
    }
    XF* pv = pxfNew ? pxfNew : (paloNew ? PxfObj(paloNew) : nullptr);
    if (ALO* pOld = p->paloParent)
    {
        for (auto& pxf : p->axf)
            for (auto& pxfOld : pOld->axf)
                if (pxf->pxfRef == pxfOld.get())
                {
                    pxf->pxfRef = pv;
                    break;
                }
    }
    else
        for (auto& pxf : p->axf)
            if (!pxf->pxfRef) pxf->pxfRef = pv;  // the original's no-parent range is {null, one record}
    if (pfnBaseMove) pfnBaseMove(p, paloNew, pxfNew, grf);
    UpdateAloXfWorlds(p);
    using PFNSETV = void (*)(ALO*, const glm::vec3&);
    using PFNSETR = void (*)(ALO*, const glm::mat3&);
    if (grf & 1)
    {
        PFNSETV pfn = reinterpret_cast<PFNSETV>(PvNativeVt(p, 29));
        (pfn ? pfn : SetAloPos)(p, PosOf(W));
    }
    if (grf & 2)
    {
        PFNSETR pfn = reinterpret_cast<PFNSETR>(PvNativeVt(p, 30));
        (pfn ? pfn : SetAloRotation)(p, Mat3Of(W));
    }
    if (grf & 4)
    {
        PFNSETV pfn = reinterpret_cast<PFNSETV>(PvNativeVt(p, 35));
        (pfn ? pfn : SetAloVelocityTo)(p, q1);
    }
    if (grf & 8)
    {
        PFNSETV pfn = reinterpret_cast<PFNSETV>(PvNativeVt(p, 36));
        (pfn ? pfn : SetAloAngVelocityTo)(p, q2);
    }
    if (p->paloParent && !(Sort(p->paloParent).grf148 & FL148_ICED)) SetAloIced(p, false);
}
