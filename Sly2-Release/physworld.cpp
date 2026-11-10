// The physics in the world's loop (the physics port): the frame 0x1F8DD8 with the world's update table, the
// objects' world hooks (ALO vt slots 11 / 12 / 22 / 23's physics parts), the active-list test of 0x1336F0.
// The world module (activate.cpp, update.cpp) and the animation module (anim.cpp's 0x1F8DD8 pass) call these.

#include "physworld.h"
#include "laser.h"

#include "cls.h"
#include "so.h"
#include "anim.h"
#include "stepphys.h"
#include "clock.h"
#include "sw.h"
#include "vtnative.h"
#include "activate.h"
#include "transition.h"
#include "update.h"
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "objmethod.h"
#include "alo.h"
#include "ref.h"
#include "xfphys.h"
#include "step.h"
#include "lox.h"
#include "xf.h"

glm::mat3 MatEuler(const glm::vec3& e);       // xfmath.h (0x11B628)
glm::vec3 EulerFromMat(const glm::mat3& m);    // xfmath.h (0x11BFF0)
extern const glm::vec3 g_vecZero;
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
bool FAlo(const LO* plo) { return plo && FCidDerivedFrom(plo->Cid(), 0x30); }

SW* s_pswPhys = nullptr;

// orig 0x1F8DD8's loop: vt slot 17 of the update table's entries whose flags & 0x24 == 4, in the table's order
// (the entry being updated is SW +0x3F1C: an object that registers others keeps its place). A class whose slot 17
// is not native falls back to the ALO's (0x1F3980: its records and entries; the characters' 0x18A278 / 0x204108
// call it after their own work), except the particle systems' 0x139DC0 (emitter.cpp's pass runs those).
void UpdateEntries17()
{
    SW* psw = s_pswPhys;
    if (!psw) return;
    for (psw->iupdCur = 0; psw->iupdCur < static_cast<int>(psw->aupd.size()); ++psw->iupdCur)
    {
#ifdef __SWITCH__
        // (the Switch: the object four entries ahead fetched, its class pointer the first thing read of it)
        if (size_t(psw->iupdCur) + 4 < psw->aupd.size())
            if (const LO* pn = psw->aupd[size_t(psw->iupdCur) + 4].plo) __builtin_prefetch(&pn->pcls);
#endif
        const UPDENT& e = psw->aupd[psw->iupdCur];
        if (!e.plo || (e.grf & 0x24) != 4) continue;
        LO* plo = e.plo;
        using PFN = void (*)(LO*);
        if (plo->pcls && plo->pcls->PfnOrig(17) == 0x139DC0) continue;  // (the particle systems: emitter.cpp)
        if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(plo, 17))) pfn(plo);  // (an override not ported: its base's)
    }
    psw->iupdCur = -1;
}

// 0x1336F0's test: an attachment is active while its object is in the world's update table (SW +0x2C)
bool FAloRegistered(ALO* palo) { return palo->iupd >= 0; }
}

// The entries' world geometry after the load: the loader composes the records' world matrices after the entries were
// added (natively the objects' OnAdd runs when they are made, before their records are read and placed) without
// stamping them, so the entries' world copies are refreshed once (0x1F38A8 for each entry, the boxes marked dirty).
void (*g_pfnPhysWorldRefreshed)(SW* psw) = nullptr;
void RefreshSwPhysWorld(SW* psw)
{
    for (const auto& plo : psw->aploAll)
    {
        if (!FAlo(plo.get())) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        // the stepping objects' heading from their placed record (0x2041F8's 0x204740: what the placements' slot 30
        // does in the original; natively the checkpoint's teleport does not reach STEPPHYS yet)
        if (FAloStep(palo) && PxfObj(palo))
        {
            const glm::mat4& m = PxfObj(palo)->matWorld;
            SetStepPhysHeading(palo, std::atan2(m[0][1], m[0][0]));
        }
        if (palo->asoe.empty()) continue;
        CopyAloSoeMatrices(palo);
        for (SOE& e : palo->asoe)
        {
            e.nStampWorld = INT32_MIN;
            UpdateSoeWorld(&e);
        }
    }
    RebuildSwAaox();
    if (g_pfnPhysWorldRefreshed) g_pfnPhysWorldRefreshed(psw);
}

constinit bool (*g_pfnFActorDrivesPhys)(ALO* palo) = nullptr;

void RefreshSwPhysWorldNow(SW* psw)
{
    RefreshSwPhysWorld(psw);
    s_pswPhys = psw;
}

void UpdateSwPhysFrame(SW* psw)
{
    if (s_pswPhys != psw) RefreshSwPhysWorld(psw);
    s_pswPhys = psw;
    g_pfnUpdateSwEntries17 = UpdateEntries17;
    g_pfnFAloActive = FAloRegistered;
    static const bool s_fOff = [] { const char* p = std::getenv("SLY2_PHYS"); return p && *p == '0'; }();
    // SLY2_PHYSPOS=oid: the object's position (its object record) and velocity each frame (debug)
    static const int s_oidPos = [] { const char* p = std::getenv("SLY2_PHYSPOS"); return p ? std::atoi(p) : -1; }();
    if (s_oidPos >= 0)
        for (const auto& plo : psw->aploAll)
            if (plo->oid == s_oidPos && FAlo(plo.get()) && static_cast<ALO*>(plo.get())->fInWorld)
            {
                ALO* pa = static_cast<ALO*>(plo.get());
                const glm::vec3 c = PosAloCenter(pa), v = VelAloPos(pa);
                std::printf("[physpos] t %.3f %d: %.1f %.1f %.1f v %.1f %.1f %.1f\n", g_clock.t, pa->oid, c.x, c.y, c.z, v.x, v.y, v.z);
            }
    if (const char* psz = std::getenv("SLY2_PHYSENTRY"))
        for (const auto& plo : psw->aploAll)
            if (plo->oid == std::atoi(psz) && FAlo(plo.get()) && static_cast<ALO*>(plo.get())->fInWorld)
            {
                ALO* pa = static_cast<ALO*>(plo.get());
                const XF* p58 = PxfPos(pa);
                const XF* p5C = PxfRot(pa);
                if (!p58) continue;
                std::fprintf(stderr, "[physentry] t %.3f pos %.3f %.3f %.3f v %.3f %.3f %.3f acc %.2f %.2f %.2f mode %d w %.4f %.4f %.4f\n",
                             g_clock.t, p58->matWorld[3].x, p58->matWorld[3].y, p58->matWorld[3].z, p58->vel.x, p58->vel.y, p58->vel.z,
                             p58->aseg[3].x, p58->aseg[3].y, p58->aseg[3].z, p58->mode, p5C ? p5C->vel.x : 0, p5C ? p5C->vel.y : 0,
                             p5C ? p5C->vel.z : 0);
            }
    if (s_fOff) UpdateEntries17();  // (SLY2_PHYS=0: the records only, no contacts / solves: A/B tests)
    else UpdateSwPhys();
    // SLY2_PHYSSANE=1: objects whose position / velocity is not finite, faster than 20000, or more than 2000 below where
    // they were first seen (falling through the world), each reported once
    static const bool s_fSane = std::getenv("SLY2_PHYSSANE") != nullptr;
    if (s_fSane)
    {
        static std::unordered_map<const ALO*, float> s_mpz;
        static std::unordered_set<const ALO*> s_setReported;
        static int s_cframeSane = 0;
        ++s_cframeSane;
        for (OXA* po : g_physw.apoxaActive)
        {
            ALO* pa = po->pso;
            const XF* px = pa ? PxfPos(pa) : nullptr;
            if (!px || s_setReported.count(pa)) continue;
            const glm::vec3 pos(px->matWorld[3]), v(px->vel);
            auto it = s_mpz.find(pa);
            if (it == s_mpz.end()) it = s_mpz.emplace(pa, pos.z).first;
            const bool fNan = !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || !std::isfinite(v.x) ||
                              !std::isfinite(v.y) || !std::isfinite(v.z);
            const bool fFast = glm::length(v) > 20000.0f;
            const bool fFell = FXfModeVelocity(px->mode) && v.z < -1000.0f && pos.z < it->second - 2000.0f;
            if (!fNan && !fFast && !fFell) continue;
            s_setReported.insert(pa);
            std::fprintf(stderr, "[physsane] frame %d oid %d cid 0x%X: %s pos %.1f %.1f %.1f (first z %.1f) v %.1f %.1f %.1f mode %d\n",
                         s_cframeSane, pa->oid, pa->Cid(), fNan ? "not finite" : fFast ? "too fast" : "fell", pos.x, pos.y, pos.z,
                         it->second, v.x, v.y, v.z, px->mode);
        }
    }
    // SLY2_PHYSSTAT=n: every n frames the lists' sizes and the contacts
    static const int s_nStat = [] { const char* p = std::getenv("SLY2_PHYSSTAT"); return p ? std::atoi(p) : 0; }();
    static int s_cframe = 0;
    if (s_nStat > 0 && (s_cframe++ % s_nStat) == 0)
    {
        int cxp = 0, cox = 0, cmoving = 0, csost = 0;
        for (OXA* po : g_physw.apoxaActive)
        {
            for (OX* ox = po->pox; ox; ox = ox->poxNext)
            {
                cox++;
                for (XP* x = ox->pxp; x; x = x->pxpNext) cxp++;
            }
            if (po->pso && Psost(po->pso)) csost++;
        }
        for (const UPDENT& e : psw->aupd)
            if (e.plo && (e.grf & 0x24) == 4) cmoving++;
        std::fprintf(stderr, "[phys] frame %d: %zu attachments, %zu active (%d with mass), %d overlaps, %d contacts, %d moving "
                     "entries of %zu\n", s_cframe - 1, g_physw.apoxa.size(), g_physw.apoxaActive.size(), csost, cox, cxp,
                     cmoving, psw->aupd.size());
        if (const char* pszRay = std::getenv("SLY2_PHYSRAY"))
        {
            float x = 0, y = 0;
            std::sscanf(pszRay, "%f,%f", &x, &y);
            for (OXA* po : g_physw.apoxaActive)
                for (SOE* pe : po->apsoe)
                {
                    LSG alsg[4];
                    const int c = ClipSegmentEntry(pe, glm::vec3(x, y, 2000.0f), glm::vec3(x, y, -3000.0f), 4, alsg);
                    for (int i = 0; i < c; i++)
                        std::fprintf(stderr, "[phys]   ray %.0f %.0f: oid %d entry tag %d (sensor %d id 0x%X off %d sphere %d): z %.1f .. %.1f\n", x, y, po->pso ? po->pso->oid : -1, pe->tag, int(pe->f48), pe->id, int(pe->f50), int(pe->f49),
                                     alsg[i].pos0.z, alsg[i].pos1.z);
                }
        }
        if (std::getenv("SLY2_PHYSMOVERS"))
            for (OXA* po : g_physw.apoxaActive)
            {
                ALO* pa = po->pso;
                const XF* px = pa ? PxfPos(pa) : nullptr;
                if (!px || glm::length(glm::vec3(px->vel)) < 1.0f) continue;
                const SOST* pst = Psost(pa);
                std::fprintf(stderr, "[phys]   moving oid %d cid 0x%X at %.1f %.1f %.1f v %.1f %.1f %.1f mode %d lin %d ang %d grav %.0f\n",
                             pa->oid, pa->Cid(), px->matWorld[3].x, px->matWorld[3].y, px->matWorld[3].z, px->vel.x, px->vel.y,
                             px->vel.z, px->mode, pst ? pst->cnstrLin.kind : -1, pst ? pst->cnstrAng.kind : -1,
                             pst ? pst->dvGravity.z : 0.0f);
            }
        if (const char* pszOid = std::getenv("SLY2_PHYSOID"))
            for (const auto& plo : psw->aploAll)
                if (plo->oid == std::atoi(pszOid) && FAlo(plo.get()))
                {
                    ALO* pa = static_cast<ALO*>(plo.get());
                    const XF* px = PxfPos(pa);
                    STEPPHYS* ps = PstepPhysOf(pa);
                    OXA* po = Poxa(pa);
                    int cxpo = 0, coxo = 0;
                    if (po)
                        for (OX* ox = po->pox; ox; ox = ox->poxNext, coxo++)
                            for (XP* x = ox->pxp; x; x = x->pxpNext) cxpo++;
                    std::fprintf(stderr, "[phys]   oid %d cid 0x%X: pos %.1f %.1f %.1f v %.1f %.1f %.1f mode %d, upd 0x%X (iupd %d), in "
                                 "world %d, entries %zu, oxa %p (root oid %d, box %.1f %.1f %.1f..%.1f %.1f %.1f grf 0x%llx), %d overlaps, %d contacts, custom %d, "
                                 "floor %p\n", pa->oid, pa->Cid(), px ? px->matWorld[3].x : 0, px ? px->matWorld[3].y : 0,
                                 px ? px->matWorld[3].z : 0, px ? px->vel.x : 0, px ? px->vel.y : 0, px ? px->vel.z : 0,
                                 px ? px->mode : -1, GrfUpdOfLo(pa), pa->iupd, int(pa->fInWorld), pa->asoe.size(), (void*)po,
                                 po && po->pso ? po->pso->oid : -1, po ? po->posMin.x : 0, po ? po->posMin.y : 0, po ? po->posMin.z : 0, po ? po->posMax.x : 0,
                                 po ? po->posMax.y : 0, po ? po->posMax.z : 0, po ? (unsigned long long)po->grf : 0ull, coxo, cxpo,
                                 ps ? ps->isoeCustom : -2, ps ? (void*)ps->pxpFloor : nullptr);
                    if (ps)
                    {
                        std::fprintf(stderr, "[phys]     step: vecVel %.2f %.2f %.2f uTurn %.2f, base %.2f %.2f %.2f, low %.2f, size %.2f, heading %.2f, motion %d, probes %d %d, "
                                     "custom %d; contacts:", ps->vecVel.x, ps->vecVel.y, ps->vecVel.z, ps->uTurn, ps->vecBase.x, ps->vecBase.y, ps->vecBase.z, ps->sFloorTarget, ps->sSize,
                                     ps->radHeading, ps->nMotion, ps->iposProbe0, ps->iposProbe1, ps->isoeCustom);
                        if (po)
                            for (OX* ox = po->pox; ox; ox = ox->poxNext)
                            {
                                for (XP* x = ox->pxp; x; x = x->pxpNext)
                                    std::fprintf(stderr, " k%d(oid %d, n %.2f %.2f %.2f, d %.1f)", x->xpk, ox->psoOther ? ox->psoOther->oid : -1,
                                                 x->normal.x, x->normal.y, x->normal.z, x->gDist);
                                if (ox->poxOther && ox->poxOther->pxp != ox->pxp)
                                    for (XP* x = ox->poxOther->pxp; x; x = x->pxpNext)
                                        std::fprintf(stderr, " other-side k%d(oid %d, n %.2f %.2f %.2f, d %.1f, end0 %d)", x->xpk,
                                                     ox->psoOther ? ox->psoOther->oid : -1, x->normal.x, x->normal.y, x->normal.z, x->gDist,
                                                     x->axpd[0].palo ? x->axpd[0].palo->oid : -1);
                            }
                        std::fprintf(stderr, "\n");
                    }
                    if (pa->psort)
                    {
                        std::fprintf(stderr, "[phys]     bytes:");
                        for (uint8_t b : pa->psort->abXp) std::fprintf(stderr, " %02x", b);
                        std::fprintf(stderr, " grf148 0x%llx\n", (unsigned long long)pa->psort->grf148);
                    }
                    {
                        int csens = 0;
                        if (po)
                            for (XP* x = po->pxpSensor; x; x = x->pxpNext) csens++;
                        std::fprintf(stderr, "[phys]     sensors: %d sensor contacts; entries:", csens);
                        for (size_t i = 0; i < pa->asoe.size(); i++)
                            if (pa->asoe[i].f48)
                                std::fprintf(stderr, " %zu(tag %d id 0x%X off %d inoxa %d)", i, pa->asoe[i].tag, pa->asoe[i].id, pa->asoe[i].f50,
                                             pa->asoe[i].fInOxa);
                        std::fprintf(stderr, "\n");
                        if (std::getenv("SLY2_PHYSENTRIES"))
                            for (size_t i = 0; i < pa->asoe.size(); i++)
                            {
                                const SOE& e = pa->asoe[i];
                                const glm::vec3 c = e.pxf ? glm::vec3(e.pxf->matWorld[3]) : glm::vec3(0.0f);
                                std::fprintf(stderr, "[phys]     entry %zu tag %d f48 %d f49 %d f50 %d f51 %d id 0x%X r %.1f verts %zu bsp %zu pinned %d inoxa %d rec (%.1f %.1f %.1f)\n",
                                             i, e.tag, e.f48, e.f49, e.f50, e.f51, e.id, e.sRadius, e.geomLocal.apos.size(),
                                             e.bsp.abspn.size(), e.pinned, e.fInOxa, c.x, c.y, c.z);
                            }
                        if (std::getenv("SLY2_PHYSENTRIES") && po)
                            for (OX* ox = po->pox; ox; ox = ox->poxNext)
                                for (XP* x = ox->pxp; x; x = x->pxpNext)
                                    std::fprintf(stderr, "[phys]     xp k%d tags %d / %d oids %d / %d n %.2f %.2f %.2f d %.2f pos %.1f %.1f %.1f\n", x->xpk,
                                                 x->axpd[0].psoe ? x->axpd[0].psoe->tag : -1, x->axpd[1].psoe ? x->axpd[1].psoe->tag : -1,
                                                 x->axpd[0].palo ? x->axpd[0].palo->oid : -1, x->axpd[1].palo ? x->axpd[1].palo->oid : -1,
                                                 x->normal.x, x->normal.y, x->normal.z, x->gDist, x->axpd[0].pos.x, x->axpd[0].pos.y, x->axpd[0].pos.z);
                    }
                    if (SOST* pst = Psost(pa))
                        std::fprintf(stderr, "[phys]     mass %.2f gravity %.1f %.1f %.1f, constraints lin %d ang %d\n", pst->m,
                                     pst->dvGravity.x, pst->dvGravity.y, pst->dvGravity.z, pst->cnstrLin.kind, pst->cnstrAng.kind);
                }
        if (s_cframe == 1)
            for (size_t i = 0; i < g_physw.apoxaActive.size() && i < 12; i++)
            {
                OXA* po = g_physw.apoxaActive[i];
                std::fprintf(stderr, "[phys]   oid %d cid 0x%X: %zu entries, box %.1f %.1f %.1f .. %.1f %.1f %.1f\n", po->pso ? po->pso->oid : -1,
                             po->pso ? po->pso->Cid() : 0, po->apsoe.size(), po->posMin.x, po->posMin.y, po->posMin.z, po->posMax.x,
                             po->posMax.y, po->posMax.z);
            }
    }
}

namespace
{
// 0x22FA20's world part for an ALO in the world that moved: a root is queued for the activation's zone pass
// (0x147E88; the visibility tree's re-link 0x1D6310 / 0x1D6378 by the +0x140 stamp has no native counterpart yet)
void OnAloMoved(ALO* palo)
{
    if (!palo->paloParent && g_psw && palo->fInWorld) QueueLoRoot(g_psw, palo);
}

// 0x1F4F10's posts (0x149AB8, once per object until run) and their run at the world's deferred calls (0x149C80):
// the objects' contact messages 0x1F7950
void PostAloContacts(ALO* palo)
{
    PostDeferredCall(palo, 0x1F7950, [](void* object, int) { EmitAloContact(static_cast<ALO*>(object), g_transition.ploPlayer); });
}
struct PHYSPASSES
{
    PHYSPASSES()
    {
        RegisterSwPass(0x1F77C0, [](SW*) { AgeSwContacts(); });  // the contact records older than 0.25 s
    }
} s_physpasses;
}

void ResetPhysWorld()
{
    ResetLasers();  // endpoint/contact tables also belong to no-file world transitions
    g_physw.Reset();
    ResetWater();
    s_pswPhys = nullptr;
    InitPhysWorldHooks();
}

void InitPhysWorldHooks()
{
    g_pfnPostAloContacts = PostAloContacts;
    g_pfnFAloActive = FAloRegistered;
    if (!std::getenv("SLY2_NOMOVEDHOOK")) g_pfnXfObjectMoved = OnAloMoved;
}

// 0x1F2C30's region part: 0x1F37C8 now, or posted to the world's start
void PlaceAloRegion(ALO* palo)
{
    if (!Pregent(palo)) return;
    if (g_psw && g_psw->fNavReady) UpdateAloRegion(palo);
    else PostDeferredCall(palo, 0x1F3868, [](void* pv, int) { UpdateAloRegion(static_cast<ALO*>(pv)); });
}

void OnLoAddPhys(LO* plo)
{
    if (FAlo(plo))
    {
        PostLoadAloPhys(static_cast<ALO*>(plo));
        // 0x1F2C30: the region entry into the region at its position (posted (0x149A50 -> 0x1F3868) while the
        // world's regions are not made yet)
        PlaceAloRegion(static_cast<ALO*>(plo));
    }
}

void OnLoRemovePhys(LO* plo)
{
    if (FAlo(plo))
    {
        RemoveAloWater(static_cast<ALO*>(plo));
        ResetAloPhys(static_cast<ALO*>(plo));
        SetAloRegion(static_cast<ALO*>(plo), nullptr);  // 0x1F2D48
    }
}

void OnLoRegisterPhys(LO* plo)
{
    if (FAlo(plo)) OnAloEnterWorldPhys(static_cast<ALO*>(plo));
}

void OnLoUnregisterPhys(LO* plo)
{
    if (FAlo(plo)) OnAloLeaveWorldPhys(static_cast<ALO*>(plo));
}

void PostLoadSwPhys(SW* psw)
{
    for (const auto& plo : psw->aploAll)
    {
        if (!FAloStep(plo.get())) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        LoadStepPhysBase(palo);      // 0x2039C0 (slot 14)
        const bool fNewRegent = !Pregent(palo);
        PregentEnsure(palo);         // 0x2039C0's 0x1F8300
        if (fNewRegent && palo->fInWorld) PlaceAloRegion(palo);
        PostLoadStepPhys(palo);  // 0x203A38 (slot 15)
        // the player actors (0x34..): their state code (0x18BCE0 / 0x134DA8 / 0x1B50B0, the actors code) holds them;
        // until it runs natively (g_pfnFActorDrivesPhys) they keep the load's fixed constraints (with the options'
        // free ones and no state code they slide off at the world's start)
        if (FCidDerivedFrom(palo->Cid(), 0x34) && !(g_pfnFActorDrivesPhys && g_pfnFActorDrivesPhys(palo)))
        {
            SetAloCnstrLin(palo, 3);
            SetAloCnstrAng(palo, 3);
        }
        if (FCidDerivedFrom(palo->Cid(), 0x38))
        {
            // the guards' post-load 0x2061D8 after 0x203A38: the size again (0x204DA8 with +0x260), the records in mode 2
            SetStepPhysSize(palo, StepPhysOf(palo).sSize);
            if (PxfPos(palo)) SetXfMode(palo, PxfPos(palo), 2);
            if (PxfRot(palo)) SetXfMode(palo, PxfRot(palo), 2);
        }
    }
}

void OnLoMovePhys(LO* plo, ALO* paloNew, ALO* paloOld)
{
    // (opt-in, SLY2_MOVEPHYS=1: with it the world's load-time reparents (instances, shared groups) rebuild their
    // objects' entries and Paris's harness differs more (301 positions / 543 update-table entries instead of 231 /
    // 349): open)
    static const bool s_fOn = std::getenv("SLY2_MOVEPHYS") != nullptr;
    if (FAlo(plo) && s_fOn) MoveAloPhys(static_cast<ALO*>(plo), paloNew, paloOld);
}

// ---- The physics properties the loader's options and seto! set (EOPID setters, grf 0x100; objmethod.h) ----
namespace
{
ALO* PaloOf(LO* plo) { return FAlo(plo) ? static_cast<ALO*>(plo) : nullptr; }
int NOf(const CRef& ref) { return NArg(std::vector<CRef>{ref}, 0); }
float GOf(const CRef& ref) { return GArg(std::vector<CRef>{ref}, 0); }
void SetGrf148Bit(ALO* palo, int bit, bool f)
{
    SORT& s = Sort(palo);
    s.grf148 = (s.grf148 & ~(1ull << bit)) | (uint64_t(f ? 1 : 0) << bit);
}

struct PHYSPROPS
{
    PHYSPROPS()
    {
        // 33 / 34: the linear / angular constraint (0x1F5720 / 0x1F57C0, an enum: 0 free .. 3 fixed)
        // EOPID 47's load method (thunk 0x100EB0 -> 0x1F6B48): the object's geometry entries with that tag turned off
        // (0x1F2AD0; e.g. Bentley's / the guards' body entries, so that their custom probe entry is the one colliding)
        RegisterObjectMethod(0x100EB0, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && FCidDerivedFrom(plo->Cid(), 0x30)) SetAloSoesByTag(static_cast<ALO*>(plo), NArg(a, 0, -1), false);
            return CRef();
        });
        // thunk 0x100E28 -> 0x1F6AC8: the entries with that tag turned on (0x1F2A60; e.g. the Music Box's 0x80000 sphere
        // 3924, pulsed on / off by its script so that the guards in reach get drowsy and fall asleep)
        RegisterObjectMethod(0x100E28, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && FCidDerivedFrom(plo->Cid(), 0x30)) SetAloSoesByTag(static_cast<ALO*>(plo), NArg(a, 0, -1), true);
            return CRef();
        });
        // thunk 0x100F38 -> 0x1F6BC8: the contact kind (+0x18, the filter its contacts carry) of the object's entries
        // with tag arg 0 set to arg 1 (e.g. Lightning Spin's clip 348: Sly's cane entry 337 0x10 -> 0x800 and back)
        RegisterObjectMethod(0x100F38, [](LO* plo, const std::vector<CRef>& a) {
            if (plo && FCidDerivedFrom(plo->Cid(), 0x30))
                for (SOE& e : static_cast<ALO*>(plo)->asoe)  // orig: 0x001F6BC8
                    if (e.tag == NArg(a, 0)) e.id = static_cast<uint32_t>(NArg(a, 1));
            return CRef(TAGK_Void);
        });
        RegisterObjectSetter(33, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloCnstrLin(p, NOf(r)); });
        RegisterObjectSetter(34, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloCnstrAng(p, NOf(r)); });
        // 35: the gravity (0x1F5880)
        RegisterObjectSetter(35, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloGravity(p, r.GetVector()); });
        // 37: flag bit 26, the object's pairs retested (0x1F6918, 0x134320)
        RegisterObjectSetter(37, [](LO* plo, const CRef& r) {
            if (ALO* p = PaloOf(plo))
            {
                SetGrf148Bit(p, 26, NOf(r) & 1);
                RecalcSwOxfFilterForObject(p);
            }
        });
        // 39: a physics root (0x1F7F48); 40: iced (0x1F8668)
        RegisterObjectSetter(39, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloPhysRootFlag(p, NOf(r) & 1); });
        RegisterObjectSetter(40, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloIced(p, NOf(r) & 1); });
        // 52: iceable (0x1F84D0: bit 33); 63: bit 29 (0x1F7F20); 61: the 4-bit field 16..19 (0x1F7EC8)
        RegisterObjectSetter(52, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetGrf148Bit(p, 33, NOf(r) & 1); });
        RegisterObjectSetter(63, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetGrf148Bit(p, 29, NOf(r) & 1); });
        RegisterObjectSetter(61, [](LO* plo, const CRef& r) {
            if (ALO* p = PaloOf(plo))
            {
                SORT& s = Sort(p);
                s.grf148 = (s.grf148 & ~0xF0000ull) | (uint64_t(NOf(r) & 0xF) << 16);
            }
        });
        // 70: the entries' mode (0x1F7A68); 71 / 72: the bytes +0x148 / +0x149 negated (0x1F7B58 / 0x1F7B78)
        RegisterObjectSetter(70, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloEntryMode(p, NOf(r)); });
        RegisterObjectSetter(71, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) Sort(p).s148 = int8_t(-NOf(r)); });
        RegisterObjectSetter(72, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) Sort(p).s149 = int8_t(-NOf(r)); });
        // 75 / 76: the velocity / angular velocity in the object's space (0x1F4898 / 0x1F4930)
        RegisterObjectSetter(75, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloVelocity(p, r.GetVector()); });
        RegisterObjectSetter(76, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo)) SetAloAngVelocity(p, r.GetVector()); });
        // orig: 0x001F4848 (EOPID 75's getter): the position record's velocity in the object's space (e.g. the RC
        // chopper's script (PO 50723) reads it each frame, adds its thrust and sets it back)
        RegisterObjectGetter(75, [](LO* plo) {
            CRef r;
            ALO* p = PaloOf(plo);
            XF* po = p ? PxfPos(p) : nullptr;
            r.SetVector(po ? VecConvertXfs(po->pxfRef, PxfObj(p), glm::vec3(po->vel)) : g_vecZero);
            return r;
        });
        // orig: 0x001F5860 (EOPID 35's getter): the gravity (+0xE8's +0x80), zero without a physics state
        RegisterObjectGetter(35, [](LO* plo) {
            CRef r;
            ALO* p = PaloOf(plo);
            SOST* ps = p ? Psost(p) : nullptr;
            r.SetVector(ps ? ps->dvGravity : g_vecZero);
            return r;
        });
        // orig: 0x001A6340 / 0x001A63B0 (EOPID 23, option 36): the object's rotation as Euler angles in degrees
        // (0x11BFF0 of vt slot 31's record; set through 0x11B628 and vt slot 30), e.g. the RC chopper's heading / tilt
        RegisterObjectGetter(23, [](LO* plo) {
            CRef r;
            glm::mat3 m(1.0f);
            if (FCidDerivedFrom(plo->Cid(), 0x30))
            {
                if (XF* pxf = PxfObj(static_cast<ALO*>(plo))) m = glm::mat3(pxf->matWorld);
            }
            else if (const LOX* plox = dynamic_cast<const LOX*>(plo))
                m = glm::mat3(plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat);
            const glm::vec3 e = EulerFromMat(m);
            r.SetVector(glm::vec3(e.x * 57.2957763671875f, e.y * 57.2957763671875f, e.z * 57.2957763671875f));
            return r;
        });
        RegisterObjectSetter(23, [](LO* plo, const CRef& r) {
            if (r.m_tagk != TAGK_Vector) return;
            const glm::vec3 v = r.GetVector();
            const glm::mat3 m = MatEuler(glm::vec3(v.x * 0.017453294f, v.y * 0.017453294f, v.z * 0.017453294f));
            if (FCidDerivedFrom(plo->Cid(), 0x30))  // vt slot 30 (as EOPID 22's setter)
            {
                ALO* palo = static_cast<ALO*>(plo);
                if (STEP* pstep = PstepFromLo(palo)) SetStepRotation(pstep, m);
                else SetAloRotation(palo, m);
                UpdateAloXfWorlds(palo);
            }
            else if (LOX* plox = dynamic_cast<LOX*>(plo)) SetLoxRotation(plox, m);
        });
        // orig: 0x001F7D10 (EOPID 57, option 129): the fade value +0x12C; <= 0 removes the object (vt slot 9) unless
        // it derives from 0x69, above 0 adds it (vt slot 8)
        RegisterObjectSetter(57, [](LO* plo, const CRef& r) {
            ALO* p = PaloOf(plo);
            if (!p) return;
            const float g = GOf(r);
            if (g <= 0.0f)
            {
                if (!FCidDerivedFrom(p->Cid(), 0x69)) RemoveLo(g_psw, p);
            }
            else AddLo(g_psw, p);
            p->gFade = g;
        });
        // 80 / 81 / 82: the region entry's radius / mass / height offset (0x1F83B0 / 0x1F8410 / 0x1F8470, each through
        // 0x1F8300: made when missing; none without a mass state)
        RegisterObjectSetter(80, [](LO* plo, const CRef& r) {
            ALO* p = PaloOf(plo);
            const bool fNew = p && !Pregent(p);
            REGENT* e = p ? PregentEnsure(p) : nullptr;
            if (e) e->sRadius = GOf(r);
            if (e && fNew && p->fInWorld) PlaceAloRegion(p);
            if (std::getenv("SLY2N_REGENTPRINT")) std::printf("regent option 80 on %d: %g%s\n", plo ? plo->oid : -1, GOf(r), e ? "" : " (no mass state)");
        });
        RegisterObjectSetter(81, [](LO* plo, const CRef& r) {
            ALO* p = PaloOf(plo);
            const bool fNew = p && !Pregent(p);
            REGENT* e = p ? PregentEnsure(p) : nullptr;
            if (e) e->gMass = GOf(r);
            if (e && fNew && p->fInWorld) PlaceAloRegion(p);
            if (std::getenv("SLY2N_REGENTPRINT")) std::printf("regent option 81 on %d: %g%s\n", plo ? plo->oid : -1, GOf(r), e ? "" : " (no mass state)");
        });
        RegisterObjectSetter(82, [](LO* plo, const CRef& r) {
            ALO* p = PaloOf(plo);
            const bool fNew = p && !Pregent(p);
            REGENT* e = p ? PregentEnsure(p) : nullptr;
            if (e) e->dz = GOf(r);
            if (e && fNew && p->fInWorld) PlaceAloRegion(p);
            if (std::getenv("SLY2N_REGENTPRINT")) std::printf("regent option 82 on %d: %g%s\n", plo ? plo->oid : -1, GOf(r), e ? "" : " (no mass state)");
        });
        // 224 / 225: a stepping object's speed scale (0x204D48) and size (0x204DA8)
        RegisterObjectSetter(224, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo); p && FAloStep(p)) SetStepPhysScale(p, GOf(r)); });
        RegisterObjectSetter(225, [](LO* plo, const CRef& r) { if (ALO* p = PaloOf(plo); p && FAloStep(p)) SetStepPhysSize(p, GOf(r)); });
    }
} s_physprops;
}
