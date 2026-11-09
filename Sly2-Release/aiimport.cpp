// The AI's differential check against the reference. See aiimport.h. The guard's / jobs' offsets are the ones of
// guard.h / aijobs.h.

#include "aiimport.h"
#include <unordered_set>
#include "aipursuit.h"
#include "aihit.h"
#include "ainoise.h"
#include "so.h"
#include "actor.h"

#include "aijobs.h"
#include "asega.h"
#include "cls.h"
#include "crv.h"
#include "detect.h"
#include "guard.h"
#include "nav.h"
#include "smrun.h"
#include "sw.h"
#include "xf.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
std::vector<std::unique_ptr<CMDLIST>> s_acmdlist;   // the imported command lists (owned here)
std::vector<std::shared_ptr<CRV>> s_acrv;           // curves made from the snapshot (polygons)
std::unordered_map<uint32_t, NAVPOLY*> s_mpapoly;   // reference polygon objects -> native
std::unordered_map<uint32_t, glm::vec4> s_mpapos;   // the points commands point at (op 7)
std::vector<std::unique_ptr<glm::vec4>> s_apos;

CID CidOfVt(uint32_t vt)
{
    for (CID c = 0x9C; c < 0xC7; ++c)
        if (g_acls[c].vtOrig == vt) return c;
    return -1;
}

LO* Plo(AIREF* pref, uint32_t a)
{
    if (!a) return nullptr;
    auto it = pref->pmap->mpaplo.find(a);
    return it == pref->pmap->mpaplo.end() ? nullptr : it->second;
}

// a located object's world matrix (vt slot 31: its reference record's world (+0x50's +0x40) x its +0x60)
glm::mat4 MatWorldOfLoxRam(const RAM& ram, uint32_t a)
{
    const glm::mat4 m = ram.Mat4(a + 0x60);
    const uint32_t axf = ram.U32(a + 0x50);
    return ram.FValid(axf) ? ram.Mat4(axf + 0x40) * m : m;
}

// a CRV from a reference path object (+0xE0 kind, +0xE4 closed, +0xE8 count, +0xEC ag, +0xF4 points, +0x100 matrix)
std::shared_ptr<CRV> PcrvFromRam(const RAM& ram, uint32_t a)
{
    auto p = std::make_shared<CRV>();
    p->crvk = ram.S32(a + 0xE0);
    p->fClosed = ram.S32(a + 0xE4);
    const int c = ram.S32(a + 0xE8);
    for (int i = 0; i < c && i < 1024; ++i)
    {
        p->ag.push_back(ram.F32(ram.U32(a + 0xEC) + 4 * i));
        p->apos.push_back(glm::vec3(ram.Vec4(ram.U32(a + 0xF4) + 16 * i)));
    }
    if (p->crvk)
        for (int i = 0; i < c && i < 1024; ++i)
        {
            p->avIn.push_back(glm::vec3(ram.Vec4(ram.U32(a + 0x140) + 16 * i)));
            p->avOut.push_back(glm::vec3(ram.Vec4(ram.U32(a + 0x144) + 16 * i)));
        }
    p->iseg = ram.S32(a + 0x178);
    if (!p->apos.empty()) RecalcCrv(p.get(), MatWorldOfLoxRam(ram, a));
    p->iseg = ram.S32(a + 0x178);
    return p;
}

// the regions' polygons (0x1BC050 objects in each region's slots +0x10C)
void ImportPolys(const RAM& ram, AIREF* pref)
{
    s_mpapoly.clear();
    for (uint32_t p = ram.U32(REF_SW_ADDR + 0x3038 + 0x11 * 12), c = 0; ram.FValid(p) && c < 100000; p = ram.U32(p + 0x20), ++c)
    {
        LO* plo = Plo(pref, p);
        if (!plo) continue;
        NAVREGION* preg = PregionOfMesh(static_cast<LOXMESH*>(plo));
        const int cslot = ram.S32(p + 0xFC);
        std::vector<std::shared_ptr<NAVPOLY>> apoly;
        for (int i = 0; i < cslot && i < 0x80; ++i)
        {
            const uint32_t a = ram.U32(ram.U32(p + 0x10C) + 4 * i);
            if (!a)
            {
                apoly.push_back(nullptr);
                continue;
            }
            auto pp = std::make_shared<NAVPOLY>();
            pp->plo = Plo(pref, a);
            pp->pcrvOwn = PcrvFromRam(ram, a);
            pp->pcrv = pp->pcrvOwn.get();
            const int n = ram.S32(a + 0xE8);
            const glm::mat4 m = MatWorldOfLoxRam(ram, a);
            for (int k = 0; k < n; ++k) pp->aposWorld.push_back(glm::vec4(glm::vec3(m * glm::vec4(pp->pcrv->apos[k], 1.0f)), 0.0f));
            pp->posMin = ram.Vec4(a + 0x1A0);
            pp->posMax = ram.Vec4(a + 0x1B0);
            for (int k = 0; k + 1 < n; ++k) pp->aplane.push_back(ram.Vec4(ram.U32(a + 0x1EC) + 16 * k));
            pp->kind = ram.S32(a + 0x190);
            s_mpapoly[a] = pp.get();
            apoly.push_back(pp);
        }
        SetRegionPolys(preg, apoly);
        preg->amember.clear();
        for (uint32_t l = ram.U32(p + 0x118), k = 0; ram.FValid(l) && k < 256; l = ram.U32(l + 0x60), ++k)
            preg->amember.push_back({Plo(pref, ram.U32(l)), ram.F32(l + 4), ram.F32(l + 8), ram.F32(l + 0xC)});
    }
}

CMDLIST* PcmdlistImport(const RAM& ram, AIREF* pref, uint32_t a, std::unordered_map<uint32_t, CMD*>* pmpcmd)
{
    if (!a) return nullptr;
    auto it = pref->mpacmdlist.find(a);
    if (it != pref->mpacmdlist.end())
    {
        // (the commands of an imported list: by address)
        return static_cast<CMDLIST*>(it->second);
    }
    auto pl = std::make_unique<CMDLIST>();
    pl->oid = ram.S32(a + 0x18);
    for (uint32_t pn = ram.U32(a + 0x50), c = 0; ram.FValid(pn) && c < 256; pn = ram.U32(pn + 0x18), ++c)
    {
        CMD* pc = PcmdAppend(pl.get(), ram.S32(pn), ram.S32(pn + 4));
        pc->n8 = ram.S32(pn + 8);
        pc->g8 = ram.F32(pn + 8);
        pc->nC = ram.S32(pn + 0xC);
        pc->gC = ram.F32(pn + 0xC);
        pc->n10 = ram.S32(pn + 0x10);
        pc->g14 = ram.F32(pn + 0x14);
        if (pc->op == 7 && pc->oid == -1 && ram.FValid(ram.U32(pn + 8)))
        {
            s_apos.push_back(std::make_unique<glm::vec4>(ram.Vec4(ram.U32(pn + 8))));
            pc->ppos8 = s_apos.back().get();
        }
        (*pmpcmd)[pn] = pc;
    }
    CMDLIST* p = pl.get();
    pref->mpacmdlist[a] = p;
    s_acmdlist.push_back(std::move(pl));
    return p;
}

std::unordered_set<uint32_t> s_setaAsega, s_setaSma;  // the snapshot's playing ASEGAs / SMAs (their lists)
void ImportKinde(const RAM& ram, AIREF* pref, uint32_t a, KINDE* pe, bool fMask)
{
    *pe = KINDE{};
    pe->k = ram.S32(a);
    const uint32_t h = ram.U32(a + 4);
    if (pe->k == 0 || pe->k == 1) pe->plo = Plo(pref, h);
    else if (pe->k == -1) pe->oid = static_cast<int>(h);
    const uint32_t s = ram.U32(a + 8);
    if (pe->k == 0 && s && s_setaAsega.count(s))
    {
        auto it = pref->pmpaasega->find(s);
        pe->pasega = it == pref->pmpaasega->end() ? nullptr : it->second;
    }
    else if (pe->k == 1 && s && s_setaSma.count(s))
    {
        auto it = pref->pmpasma->find(s);
        pe->psma = it == pref->pmpasma->end() ? nullptr : it->second;
    }
    pe->grf = ram.S32(a + 0xC);
    if (fMask) pe->mask = static_cast<uint64_t>(ram.U32(a + 0x10)) | (static_cast<uint64_t>(ram.U32(a + 0x14)) << 32);
}

struct GUARDIMPORT
{
    std::unordered_map<uint32_t, ACTIONOBJ*> mpaaction;
    std::unordered_map<uint32_t, KINDE*> mpakinde;
    std::unordered_map<uint32_t, CMD*> mpacmd;
    std::unordered_map<uint32_t, AIJOB*> mpajob;
};

const CRV* PcrvOfPath(const RAM& ram, AIREF* pref, GUARD* pg, uint32_t gaddr, uint32_t a)
{
    if (!a) return nullptr;
    if (a == ram.U32(gaddr + 0x294)) return &pg->crv294;
    if (auto it = s_mpapoly.find(a); it != s_mpapoly.end()) return it->second->pcrv;
    if (LO* plo = Plo(pref, a)) return PcrvFromLo(plo);
    auto p = PcrvFromRam(ram, a);
    s_acrv.push_back(p);
    return p.get();
}

void ImportGoal(const RAM& ram, AIREF* pref, GUARD* pg, uint32_t gaddr, uint32_t a, GOAL* pgoal)
{
    pgoal->pcrv = PcrvOfPath(ram, pref, pg, gaddr, ram.U32(a));
    pgoal->plo = Plo(pref, ram.U32(a + 4));
    pgoal->pos = ram.Vec4(a + 0x10);
}

// the noises (the list 0x3E148C: {+0x14 template, +0x20 / +0x30, +0x40 owner, +0x44 target, +0x48, +0x4C, +0x54},
// next +0x58)
void ImportNoises(const RAM& ram, AIREF* pref)
{
    ClearNoiseList();
    std::vector<uint32_t> a;
    for (uint32_t o = ram.U32(0x3E148C), c = 0; ram.FValid(o) && c < 256; o = ram.U32(o + 0x58), ++c) a.push_back(o);
    for (auto it = a.rbegin(); it != a.rend(); ++it)
    {
        const uint32_t o = *it;
        LO* ptmpl = Plo(pref, ram.U32(o + 0x14));
        ImportNoise(PtmplNoise(ptmpl), glm::vec3(ram.Vec4(o + 0x20)), glm::vec3(ram.Vec4(o + 0x30)), Plo(pref, ram.U32(o + 0x40)),
                    Plo(pref, ram.U32(o + 0x44)), ram.S32(o + 0x48), ram.F32(o + 0x4C), ram.S32(o + 0x54));
    }
}

// a hit as a job holds it (the contact message's 0x60 bytes)
AIMSG::HIT HitOfRam(const RAM& ram, AIREF* pref, uint32_t a)
{
    AIMSG::HIT h;
    h.psource = Plo(pref, ram.U32(a));
    h.preceiver = Plo(pref, ram.U32(a + 8));
    h.kind = ram.U32(a + 0xC);
    h.n = ram.S32(a + 0x10);
    h.pos = glm::vec3(ram.Vec4(a + 0x20));
    h.vec = glm::vec3(ram.Vec4(a + 0x30));
    h.grf = ram.U32(a + 0x44);
    return h;
}

std::unique_ptr<AIJOB> PjobImport(const RAM& ram, AIREF* pref, GUARD* pg, uint32_t gaddr, uint32_t a, GUARDIMPORT& gi)
{
    const CID cid = CidOfVt(ram.U32(a + 0xC));
    std::unique_ptr<AIJOB> p = PjobNew(cid < 0 ? 0x9C : cid);
    p->pguard = pg;
    p->fLocked = ram.S32(a + 0x1C);
    p->cLock = ram.S32(a + 0x20);
    auto kinde = [&](uint32_t ak) -> KINDE* {
        auto it = gi.mpakinde.find(ak);
        return it == gi.mpakinde.end() ? nullptr : it->second;
    };
    switch (cid)
    {
    case 0x9C:
    {
        auto* j = static_cast<JOBREACT*>(p.get());
        j->msg.k = ram.S32(a + 0x30);
        j->msg.plo = Plo(pref, ram.U32(a + 0x40));
        j->msg.n = ram.S32(a + 0x40);
        j->msg.g = ram.F32(a + 0x44);
        j->msg.n1C = ram.S32(a + 0x4C);
        j->msg.hit = HitOfRam(ram, pref, a + 0x40);
        j->tMsg = ram.F32(a + 0xA0);
        j->tWait = ram.F32(a + 0xA4);
        break;
    }
    case 0x9D: case 0x9E:
    {
        auto* j = static_cast<JOBMOVE*>(p.get());
        j->st = ram.S32(a + 0x30);
        j->tSt = ram.F32(a + 0x34);
        j->ploFace = Plo(pref, ram.U32(a + 0x38));
        j->nAnim = ram.S32(a + 0x3C);
        j->pkinde = kinde(ram.U32(a + 0x40));
        break;
    }
    case 0xA5:
    {
        auto* j = static_cast<JOBPURSUE*>(p.get());
        j->ptarget = Plo(pref, ram.U32(a + 0x30));
        j->st = ram.S32(a + 0x34);
        j->tSt = ram.F32(a + 0x38);
        auto it = gi.mpaaction.find(ram.U32(a + 0x3C));
        j->paction = it == gi.mpaaction.end() ? nullptr : it->second;
        j->f40 = ram.S32(a + 0x40);
        j->posGoal = ram.Vec4(a + 0x50);
        j->pregionGoal = PregionAt(glm::vec3(j->posGoal), true);
        break;
    }
    case 0xA4:
    {
        auto* j = static_cast<JOBACT*>(p.get());
        j->ptarget = Plo(pref, ram.U32(a + 0x30));
        j->st = ram.S32(a + 0x34);
        j->tSt = ram.F32(a + 0x38);
        auto it = gi.mpaaction.find(ram.U32(a + 0x3C));
        j->paction = it == gi.mpaaction.end() ? nullptr : it->second;
        j->fHit = ram.S32(a + 0x40);
        break;
    }
    case 0x9F:
    {
        auto* j = static_cast<JOBSTRIKE*>(p.get());
        j->st = ram.S32(a + 0x30);
        j->tSt = ram.F32(a + 0x34);
        j->ploFace = Plo(pref, ram.U32(a + 0x38));
        j->nAnim = ram.S32(a + 0x3C);
        j->pkinde = kinde(ram.U32(a + 0x40));
        j->f50 = ram.S32(a + 0x50);
        break;
    }
    case 0xB3: static_cast<JOBSURROUND*>(p.get())->st = ram.S32(a + 0x30); break;
    case 0xAB:
    {
        auto* j = static_cast<JOBINVESTIGATE*>(p.get());
        j->st = ram.S32(a + 0x30);
        j->tSt = ram.F32(a + 0x34);
        j->tEnd = ram.F32(a + 0x38);
        j->tSearch = ram.F32(a + 0x3C);
        j->t40 = ram.F32(a + 0x40);
        j->fNew = ram.S32(a + 0x48);
        ReleaseNoise(j->pnoise);
        j->pnoise = nullptr;  // (the noise by its serial in the imported list)
        if (const uint32_t an = ram.U32(a + 0x44); ram.FValid(an))
            for (NOISE* pn : ApnoiseAll())
                if (pn->nSerial == ram.S32(an + 0x48)) j->pnoise = pn;
        AddRefNoise(j->pnoise);
        break;
    }
    case 0xA1: case 0xA2: case 0xA3:
    {
        auto* j = static_cast<JOBHIT*>(p.get());
        j->st = ram.S32(a + 0x30);
        j->tSt = ram.F32(a + 0x34);
        j->ploFace = Plo(pref, ram.U32(a + 0x38));
        j->nAnim = ram.S32(a + 0x3C);
        j->pkinde = kinde(ram.U32(a + 0x40));
        j->hit = HitOfRam(ram, pref, a + 0x50);
        j->tB0 = ram.F32(a + 0xB0);
        j->tB4 = ram.F32(a + 0xB4);
        break;
    }
    case 0xA7:
    {
        auto* j = static_cast<JOBFOLLOW*>(p.get());
        j->pcrv = PcrvOfPath(ram, pref, pg, gaddr, ram.U32(a + 0x30));
        j->s = ram.F32(a + 0x34);
        j->fReverse = ram.S32(a + 0x38);
        j->fOpen = ram.S32(a + 0x3C);
        j->fNoAvoid = ram.S32(a + 0x40);
        j->sEnd = ram.F32(a + 0x44);
        j->sStart = ram.F32(a + 0x48);
        j->st = ram.S32(a + 0x4C);
        j->tSt = ram.F32(a + 0x50);
        const uint32_t apoly = ram.U32(a + 0x54);
        if (auto it = s_mpapoly.find(apoly); it != s_mpapoly.end()) j->ppolyOther = it->second;
        j->pcrvOther = PcrvOfPath(ram, pref, pg, gaddr, apoly);
        j->nWay = ram.S32(a + 0x58);
        j->sOther = ram.F32(a + 0x5C);
        j->g60 = ram.F32(a + 0x60);
        j->sArrive = ram.F32(a + 0x64);
        j->sNear = ram.F32(a + 0x68);
        j->sFar = ram.F32(a + 0x6C);
        j->sTarget = ram.F32(a + 0x70);
        j->pos = ram.Vec4(a + 0x80);
        j->vel = ram.Vec4(a + 0x90);
        j->fFirst = ram.S32(a + 0xA0);
        j->posEnd = ram.Vec4(a + 0xB0);
        j->kMove = ram.S32(a + 0xC0);
        j->uSpeed = ram.F32(a + 0xC4);
        j->ploArg = Plo(pref, ram.U32(a + 0xC8));
        j->posMoved = ram.Vec4(a + 0xD0);
        j->tLeft = ram.F32(a + 0xE0);
        break;
    }
    case 0xA9:
    {
        auto* j = static_cast<JOBSCRIPT*>(p.get());
        j->pcmdlist = PcmdlistImport(ram, pref, ram.U32(a + 0x30), &gi.mpacmd);
        j->ploNear = Plo(pref, ram.U32(a + 0x34));
        if (!j->ploNear && ram.U32(a + 0x34) == ram.U32(a + 0x30)) j->ploNear = j->pcmdlist;
        auto it = gi.mpacmd.find(ram.U32(a + 0x38));
        j->pcmd = it == gi.mpacmd.end() ? nullptr : it->second;
        j->tCmd = ram.F32(a + 0x3C);
        j->fRunning = ram.S32(a + 0x40);
        j->radFace = ram.F32(a + 0x48);
        j->fFromReact = ram.S32(a + 0x4C);
        ImportKinde(ram, pref, a + 0x50, &j->kinde50, false);
        gi.mpakinde[a + 0x50] = &j->kinde50;
        break;
    }
    case 0xAA:
    {
        auto* j = static_cast<JOBWAIT*>(p.get());
        j->tA = ram.F32(a + 0x30);
        j->tB = ram.F32(a + 0x34);
        j->st = ram.S32(a + 0x38);
        j->tSt = ram.F32(a + 0x3C);
        j->pcmdlist = PcmdlistImport(ram, pref, ram.U32(a + 0x40), &gi.mpacmd);
        break;
    }
    case 0xBA:
    {
        auto* j = static_cast<JOBGOTO*>(p.get());
        j->st = ram.S32(a + 0x30);
        j->tSt = ram.F32(a + 0x34);
        j->nAnim = ram.S32(a + 0x38);
        ImportGoal(ram, pref, pg, gaddr, a + 0x40, &j->goal);
        ImportGoal(ram, pref, pg, gaddr, a + 0x60, &j->goalCur);
        ImportGoal(ram, pref, pg, gaddr, a + 0x80, &j->goalFallback);
        j->tSearch = ram.F32(a + 0xA0);
        j->nSerial = ram.S32(a + 0xA4);
        j->plo = Plo(pref, ram.U32(a + 0xA8));
        j->fAC = ram.S32(a + 0xAC);
        j->gB0 = ram.F32(a + 0xB0);
        j->anode.assign(static_cast<size_t>(std::max(0, ram.S32(a + 0xB4))), nullptr);
        j->fDirect = ram.S32(a + 0xBC);
        j->fC0 = ram.S32(a + 0xC0);
        break;
    }
    case 0xC0:
    {
        auto* j = static_cast<JOBPATHSTEP*>(p.get());
        j->ploPath = Plo(pref, ram.U32(a + 0x30));
        j->pcrv = PcrvOfPath(ram, pref, pg, gaddr, ram.U32(a + 0x30));
        j->fForward = ram.S32(a + 0x34);
        j->s = ram.F32(a + 0x38);
        j->pos = ram.Vec4(a + 0x40);
        j->plo50 = Plo(pref, ram.U32(a + 0x50));
        j->n54 = ram.S32(a + 0x54);
        j->fFromReact = ram.S32(a + 0x58);
        break;
    }
    default: break;
    }
    return p;
}

void ImportGuard(const RAM& ram, AIREF* pref, uint32_t a, GUARD* pg)
{
    GUARDIMPORT gi;
    // (the native jobs of the previous import dropped without their Exit: the snapshot is the state)
    pg->stack.ajob.clear();
    pg->move.pjob = nullptr;
    pg->palo->fInWorld = ((ram.U32(a + 0x2C) >> 28) & 1) != 0;  // LO +0x28 bit 60
    // its position / rotation / object records' matrices (+0x58 / +0x5C / +0x60): as the frame starts
    for (int i = 0; i < 3; ++i)
    {
        XF* pxf = pg->palo->apxfRef[i];
        const uint32_t ax = ram.U32(a + 0x58 + 4 * i);
        if (!pxf || !ram.FValid(ax)) continue;
        pxf->mat = ram.Mat4(ax);
        pxf->matWorld = ram.Mat4(ax + 0x40);
        pxf->vel = ram.Vec4(ax + 0xB0);
    }
    STEPPHYS& st = St(pg);
    st.uTurn = ram.F32(a + 0x180);
    st.vecVel = glm::vec3(ram.Vec4(a + 0x190));
    st.nMotion = ram.S32(a + 0x1A4);
    st.radHeading = ram.F32(a + 0x1A8);
    for (int i = 0; i < 3; ++i) st.matHeading[i] = glm::vec3(ram.Vec4(a + 0x1B0 + 16 * i));
    st.posTarget = ram.Vec4(a + 0x1E0);
    st.fNoContacts = ram.S32(a + 0x204);
    pg->fFloorRam = ram.U32(a + 0x208) != 0;
    st.tFloor = ram.F32(a + 0x20C);
    pg->fProbeRam = ram.FValid(ram.U32(a + 0x218));
    if (pg->fProbeRam) pg->posProbe = ram.Vec4(ram.U32(a + 0x218));
    st.vecBase = ram.Vec4(a + 0x240);
    st.sSize = ram.F32(a + 0x260);
    if (const uint32_t ap = ram.U32(a + 0x294))
    {
        auto c = PcrvFromRam(ram, ap);
        pg->crv294 = *c;
    }
    pg->f298 = ram.S32(a + 0x298);
    pg->f29C = ram.S32(a + 0x29C);
    pg->f32C = ram.S32(a + 0x32C);
    pg->n334 = ram.S32(a + 0x334);
    pg->n338 = ram.S32(a + 0x338);
    pg->n33C = ram.S32(a + 0x33C);
    pg->f330 = ram.S32(a + 0x330);
    pg->n3D0 = ram.S32(a + 0x3D0);
    pg->grf350 = ram.U32(a + 0x350);
    pg->grf354 = ram.U32(a + 0x354);
    pg->grf358 = ram.U32(a + 0x358);
    pg->n340 = ram.S32(a + 0x340);
    pg->n344 = ram.S32(a + 0x344);
    pg->grfState = static_cast<uint64_t>(ram.U32(a + 0x2C0)) | (static_cast<uint64_t>(ram.U32(a + 0x2C4)) << 32);
    pg->tWaitA0 = ram.F32(a + 0x2CC);
    pg->tWaitA1 = ram.F32(a + 0x2D0);
    pg->tWaitB0 = ram.F32(a + 0x2D4);
    pg->tWaitB1 = ram.F32(a + 0x2D8);
    pg->pcmdlistPatrol = PcmdlistImport(ram, pref, ram.U32(a + 0x2E8), &gi.mpacmd);
    pg->pcmdlistHome = PcmdlistImport(ram, pref, ram.U32(a + 0x2EC), &gi.mpacmd);
    {
        auto it = gi.mpacmd.find(ram.U32(a + 0x2DC));
        pg->saved2DC.pcmd = it == gi.mpacmd.end() ? nullptr : it->second;
        pg->saved2DC.plo = Plo(pref, ram.U32(a + 0x2E0));
        pg->saved2DC.g = ram.F32(a + 0x2E4);
    }
    pg->posHome = ram.Vec4(a + 0x300);
    pg->radHome = ram.F32(a + 0x310);
    pg->plo38C = Plo(pref, ram.U32(a + 0x38C));
    // the pursuit's fields and records (aipursuit.h)
    pg->gF30 = ram.F32(a + 0xF30);
    pg->gF34 = ram.F32(a + 0xF34);
    pg->tF04 = ram.F32(a + 0xF04);
    pg->tF44 = ram.F32(a + 0xF44);
    pg->nF40 = ram.S32(a + 0xF40);
    pg->ploE60 = Plo(pref, ram.U32(a + 0xE60));
    pg->fF3C = ram.S32(a + 0xF3C);
    pg->n3D4 = ram.S32(a + 0x3D4);
    pg->ploF00 = Plo(pref, ram.U32(a + 0xF00));
    pg->aploEDC.clear();
    for (int i = 0; i < ram.S32(a + 0xEFC) && i < 8; ++i)
    {
        const uint32_t ae = ram.U32(a + 0xEDC + 4 * i);
        LO* plo = Plo(pref, ae);
        if (!plo) continue;
        pg->aploEDC.push_back(plo);
        ACTIONOBJ* pa = PactionOf(plo, pg);
        const uint32_t ak = ram.U32(ae + 0xC0);  // (+0xC0 points at the object's own entry, +0xB0)
        if (ram.FValid(ak))
        {
            ImportKinde(ram, pref, ak, &pa->kinde, false);
            gi.mpakinde[ak] = &pa->kinde;
        }
        gi.mpaaction[ae] = pa;
    }
    pg->ptarget3CC = nullptr;
    if (const uint32_t ar = ram.U32(a + 0x3CC); ram.FValid(ar))
        if (LO* plo = Plo(pref, ram.U32(ar + 0xC)))
        {
            TARGETREC* pt = PtargetrecOf(plo);
            pt->t0 = ram.F32(ar);
            pt->t4 = ram.F32(ar + 4);
            pt->n8 = ram.S32(ar + 8);
            pt->nMode = ram.S32(ar + 0x10);
            pt->g14 = ram.F32(ar + 0x14);
            pt->g18 = ram.F32(ar + 0x18);
            pt->f1C = ram.S32(ar + 0x1C);
            pt->apguard.clear();
            for (int i = 0; i < ram.S32(ar + 0x28) && i < 16; ++i)
                if (LO* pgo = Plo(pref, ram.U32(ar + 0x2C + 4 * i)))
                    if (GUARD* pgg = PguardFromLo(pgo)) pt->apguard.push_back(pgg);
            pg->ptarget3CC = pt;
        }
    pg->ploTarget = Plo(pref, ram.U32(a + 0x390));
    pg->f4A0 = ram.S32(a + 0x4A0);
    pg->f4A4 = ram.S32(a + 0x4A4);
    pg->pos4B0 = ram.Vec4(a + 0x4B0);
    pg->watchA.st = ram.S32(ram.U32(a + 0x498) + 4);
    pg->watchA.t = ram.F32(ram.U32(a + 0x498) + 8);
    pg->watchA.fOn = ram.S32(ram.U32(a + 0x498) + 0xC);
    pg->watchB.st = ram.S32(ram.U32(a + 0x49C) + 4);
    pg->watchB.t = ram.F32(ram.U32(a + 0x49C) + 8);
    pg->watchB.plo = Plo(pref, ram.U32(ram.U32(a + 0x49C) + 0xC));
    pg->watchB.fOn = ram.S32(ram.U32(a + 0x49C) + 0x10);
    // the kind table and the arrays
    const int ck = std::min(32, std::max(0, ram.S32(a + 0x4C0)));
    pg->akinde.assign(ck, KINDE{});
    for (int i = 0; i < ck; ++i)
    {
        ImportKinde(ram, pref, a + 0x4D0 + 0x20 * i, &pg->akinde[i], true);
        gi.mpakinde[a + 0x4D0 + 0x20 * i] = &pg->akinde[i];
    }
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 26; ++i)
        {
            const uint32_t ae = a + 0x8D0 + 0x1A0 * j + 0x10 * i;
            ImportKinde(ram, pref, ae, &pg->aakinde[j][i], false);
            gi.mpakinde[ae] = &pg->aakinde[j][i];
        }
    pg->nE64 = ram.S32(a + 0xE64);
    pg->nE68 = ram.S32(a + 0xE68);
    pg->kMove = ram.S32(a + 0xF80);
    pg->fF94 = ram.S32(a + 0xF94);
    pg->fFA8 = ram.S32(a + 0xFA8);
    pg->nMode = ram.S32(a + 0xFBC);
    pg->tMode = ram.F32(a + 0xFC0);
    pg->fFC8 = ram.S32(a + 0xFC8);
    pg->ploFCC = Plo(pref, ram.U32(a + 0xFCC));
    pg->move.fHeadingLocked = ram.S32(a + 0xFD4);
    pg->grfFE8 = ram.U32(a + 0xFE8);
    pg->nE78 = ram.S32(a + 0xE78);
    pg->ploE74 = Plo(pref, ram.U32(a + 0xE74));
    pg->uGaitFF0 = ram.F32(a + 0xFF0);
    pg->tGaitFF4 = ram.F32(a + 0xFF4);
    pg->tFF8 = ram.F32(a + 0xFF8);
    pg->cActivationLocks = ram.S32(a + 0xFFC);
    pg->n1000 = ram.S32(a + 0x1000);
    // the detector
    if (const uint32_t ad = ram.U32(a + 0xF64))
    {
        LO* plo = Plo(pref, ad);
        DETECTOR* pdet = plo ? PdetectorFromLo(plo) : nullptr;
        if (plo && !pdet) pdet = PdetectorNew(plo, pg->palo);
        if (pdet)
        {
            pdet->ploOwner = Plo(pref, ram.U32(ad + 0xE0));  // (+0xE0: the guard after its reset)
            pdet->ploVolume = Plo(pref, ram.U32(ad + 0xE8));
            pdet->ploVolume2 = Plo(pref, ram.U32(ad + 0xEC));
            pdet->fAlways = ram.S32(ad + 0xF0);
            pdet->fNever = ram.S32(ad + 0xF4);
            pdet->grf = ram.U32(ad + 0xF8);
            pdet->fTouchRam = true;
            pdet->atouchRam.clear();
            if (const uint32_t ae4 = ram.U32(ad + 0xE4); ram.FValid(ae4))
                for (uint32_t pl = ram.U32(ae4 + 0x64), c = 0; ram.FValid(pl) && c < 64; pl = ram.U32(pl + 4), ++c)
                    pdet->atouchRam.emplace_back(Plo(pref, ram.U32(pl + 0xC)), ram.U32(pl) != 0);
            pdet->ae.clear();
            for (int i = 0; i < ram.S32(ad + 0xFC) && i < 64; ++i)
            {
                DETENTRY e;
                const uint32_t pe = ad + 0x100 + 0x10 * i;
                e.plo = Plo(pref, ram.U32(pe));
                e.st = ram.S32(pe + 4);
                e.t8 = ram.F32(pe + 8);
                e.tC = ram.F32(pe + 0xC);
                pdet->ae.push_back(e);
            }
            pg->pdet = pdet;
        }
    }
    // The jobs (bottom first), then their links and moves' references to an A9's inline kind entry (+0x50).
    const int cjob = ram.S32(a + 0x26C + 4);
    std::vector<uint32_t> aa;
    for (int i = 0; i < cjob && i < 8; ++i) aa.push_back(ram.U32(a + 0x26C + 8 + 4 * i));
    for (uint32_t aj : aa)
    {
        std::unique_ptr<AIJOB> pj = PjobImport(ram, pref, pg, a, aj, gi);
        gi.mpajob[aj] = pj.get();
        pg->stack.ajob.push_back(std::move(pj));
    }
    for (size_t i = 0; i < aa.size(); ++i)
    {
        AIJOB* pj = pg->stack.ajob[i].get();
        auto itp = gi.mpajob.find(ram.U32(aa[i] + 0x14));
        auto itc = gi.mpajob.find(ram.U32(aa[i] + 0x18));
        pj->pparent = itp == gi.mpajob.end() ? nullptr : itp->second;
        pj->pchild = itc == gi.mpajob.end() ? nullptr : itc->second;
        if (auto* pmove = dynamic_cast<JOBMOVE*>(pj))
        {
            const auto it = gi.mpakinde.find(ram.U32(aa[i] + 0x40));
            pmove->pkinde = it == gi.mpakinde.end() ? nullptr : it->second;
        }
    }
    {
        auto it = gi.mpajob.find(ram.U32(a + 0xFD0));
        pg->move.pjob = it == gi.mpajob.end() ? nullptr : it->second;
    }
}
}  // namespace

namespace
{
// the AI's inputs each frame in the carried mode: the guards' records and alive flags, the regions' polygons / members
void ImportInputs(const RAM& ram, AIREF* pref)
{
    ImportPolys(ram, pref);
    for (auto& [a, pg] : pref->aguard)
    {
        pg->palo->fInWorld = ((ram.U32(a + 0x2C) >> 28) & 1) != 0;
        for (int i = 0; i < 3; ++i)
        {
            XF* pxf = pg->palo->apxfRef[i];
            const uint32_t ax = ram.U32(a + 0x58 + 4 * i);
            if (!pxf || !ram.FValid(ax)) continue;
            pxf->mat = ram.Mat4(ax);
            pxf->matWorld = ram.Mat4(ax + 0x40);
            pxf->vel = ram.Vec4(ax + 0xB0);
        }
        pg->fFloorRam = ram.U32(a + 0x208) != 0;
        pg->fProbeRam = ram.FValid(ram.U32(a + 0x218));
        if (pg->fProbeRam) pg->posProbe = ram.Vec4(ram.U32(a + 0x218));
    }
}
}  // namespace

namespace
{
// the players' placement (their records' local / world matrices and velocities): the detectors' and jobs' targets
void ImportPlayers(const RAM& ram, AIREF* pref)
{
    for (CID cid = 0x34; cid < 0x38; ++cid)
        for (uint32_t p = ram.U32(REF_SW_ADDR + 0x3038 + cid * 12), c = 0; ram.FValid(p) && c < 1000; p = ram.U32(p + 0x20), ++c)
        {
            LO* plo = Plo(pref, p);
            if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            ALO* palo = static_cast<ALO*>(plo);
            palo->fInWorld = ((ram.U32(p + 0x2C) >> 28) & 1) != 0;
            for (int i = 0; i < 3; ++i)
            {
                XF* pxf = palo->apxfRef[i];
                const uint32_t ax = ram.U32(p + 0x58 + 4 * i);
                if (!pxf || !ram.FValid(ax)) continue;
                pxf->mat = ram.Mat4(ax);
                pxf->matWorld = ram.Mat4(ax + 0x40);
                pxf->vel = ram.Vec4(ax + 0xB0);
            }
            // (vt slot 77: the +0xDF0 controller's status, 0x1A23C0)
            {
                const uint32_t ac = p + 0xDF0;
                int n = 0;
                if (ram.S32(ac + 4) != -1) n = ram.S32(ram.U32(ac + 0x10)) > 0 ? 1 : 2;
                else n = ram.S32(ac + 0x90) == 1;
                SetPlayerStatus77(plo, n);
            }
            if (FCidDerivedFrom(plo->Cid(), 0x33)) StepPhysOf(palo).vecBase = ram.Vec4(p + 0x240);  // (the detectors aim there)
            if (ACTOR* pa = PactorFromLo(plo))
            {
                pa->n4C8 = ram.S32(p + 0x4C8);
                pa->nSub4CC = ram.S32(p + 0x4CC);
            }
        }
}
}  // namespace

void AiImport(const RAM& ram, AIREF* pref)
{
    const uint32_t bsp = ram.U32(REF_SW_ADDR + 0x45CC);
    SetNavLinkSerial(ram.FValid(bsp) ? ram.S32(bsp + 0x28) : 0);
    {
        const auto aa = ram.Dl(0x3E1218);
        s_setaAsega = std::unordered_set<uint32_t>(aa.begin(), aa.end());
        const auto as = ram.Dl(0x3E126C);
        s_setaSma = std::unordered_set<uint32_t>(as.begin(), as.end());
    }
    if (!pref->fNoPlayers) ImportPlayers(ram, pref);
    if (!(pref->fFree && pref->fImported)) ImportNoises(ram, pref);
    else if (!pref->fNoPlayers)
    {
        // a free run: the players are the reference's, so are their noises (made by their code: e.g. Sly's 706);
        // each new one added once
        static std::unordered_set<uint32_t> s_seta;
        for (uint32_t o = ram.U32(0x3E148C), c = 0; ram.FValid(o) && c < 256; o = ram.U32(o + 0x58), ++c)
        {
            LO* powner = Plo(pref, ram.U32(o + 0x40));
            if (std::getenv("SLY2N_NOISEDEBUG"))
                std::fprintf(stderr, "free-run noise 0x%X owner %d tmpl %p\n", o, powner ? powner->oid : -1, (void*)PtmplNoise(Plo(pref, ram.U32(o + 0x14))));
            if (!powner || !FCidDerivedFrom(powner->Cid(), 0x34) || s_seta.count(o + ram.U32(o + 0x48) * 0x1000u)) continue;
            s_seta.insert(o + ram.U32(o + 0x48) * 0x1000u);
            ImportNoise(PtmplNoise(Plo(pref, ram.U32(o + 0x14))), glm::vec3(ram.Vec4(o + 0x20)), glm::vec3(ram.Vec4(o + 0x30)), powner,
                        Plo(pref, ram.U32(o + 0x44)), ram.S32(o + 0x48), ram.F32(o + 0x4C), ram.S32(o + 0x54));
        }
    }
    // the game's pause counts (SW +0x3D78 / +0x3D80 / +0x3D84): set by other code (dialogs, menus) in the reference
    g_gamepause.c78 = ram.S32(REF_SW_ADDR + 0x3D78);
    g_gamepause.c80 = ram.S32(REF_SW_ADDR + 0x3D80);
    g_gamepause.c84 = ram.S32(REF_SW_ADDR + 0x3D84);
    if (pref->fFree && pref->fImported)
    {
        ImportPolys(ram, pref);
        return;
    }
    if (pref->fCarry && pref->fImported)
    {
        ImportInputs(ram, pref);
        return;
    }
    pref->fImported = true;
    pref->mpacmdlist.clear();
    s_acmdlist.clear();
    s_acrv.clear();
    s_apos.clear();
    ImportPolys(ram, pref);
    pref->aguard.clear();
    for (CID cid = 0x38; cid < 0x44; ++cid)
        for (uint32_t p = ram.U32(REF_SW_ADDR + 0x3038 + cid * 12), c = 0; ram.FValid(p) && c < 100000; p = ram.U32(p + 0x20), ++c)
        {
            LO* plo = Plo(pref, p);
            if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            if (ram.S32(p + 0x26C + 4) <= 0) continue;  // (no jobs: not started)
            GUARD* pg = PguardFromLo(plo);
            if (!pg) pg = PguardNew(static_cast<ALO*>(plo));
            ImportGuard(ram, pref, p, pg);
            pref->aguard.emplace_back(p, pg);
        }
    // Original D_003E1444 is the registration-ordered live list, not the class lists.
    // Importing it directly preserves saved timestamps and does not enqueue activation changes.
    std::vector<GUARD*> active;
    for (uint32_t p : ram.Dl(0x3E1444))
        if (GUARD* guard = PguardFromLo(Plo(pref, p))) active.push_back(guard);
    ImportGuardActiveList(active);
}

void AiStep(AIREF* pref)
{
    for (auto& [a, pg] : pref->aguard) UpdateGuard(pg);
    ExpireNoises();  // (the world's pass after the update table)
}

AISTATS AiCompare(const RAM& ram, AIREF* pref)
{
    AISTATS st;
    char ach[512];
    // the noises: their number, owners, kinds and places
    {
        std::vector<uint32_t> a;
        for (uint32_t o = ram.U32(0x3E148C), c = 0; ram.FValid(o) && c < 256; o = ram.U32(o + 0x58), ++c) a.push_back(o);
        const auto& an = ApnoiseAll();
        ++st.cfield;
        bool f = a.size() == an.size();
        for (size_t i = 0; f && i < a.size(); ++i)
        {
            const NOISE* pn = an[an.size() - 1 - i];
            f = Plo(pref, ram.U32(a[i] + 0x40)) == pn->ploOwner40 && ram.S32(a[i] + 0x54) == pn->nKind &&
                glm::length(glm::vec3(ram.Vec4(a[i] + 0x20)) - pn->pos20) < 0.5f;
        }
        if (f) ++st.cfieldOk;
        else
        {
            std::snprintf(ach, sizeof ach, "  noises: ref %zu native %zu", a.size(), an.size());
            st.amsg.push_back(ach);
        }
    }
    for (auto& [a, pg] : pref->aguard)
    {
        ++st.cguard;
        bool fOk = true;
        auto field = [&](const char* psz, double ref, double nat, double tol) {
            ++st.cfield;
            const bool f = std::fabs(ref - nat) <= tol * std::max(1.0, std::fabs(ref));
            st.cfieldOk += f;
            if (!f)
            {
                fOk = false;
                std::snprintf(ach, sizeof ach, "  guard %d %s: ref %.5f native %.5f", pg->palo->oid, psz, ref, nat);
                st.amsg.push_back(ach);
            }
        };
        const uint64_t grf = static_cast<uint64_t>(ram.U32(a + 0x2C0)) | (static_cast<uint64_t>(ram.U32(a + 0x2C4)) << 32);
        ++st.cfield;
        if (grf == pg->grfState) ++st.cfieldOk;
        else
        {
            fOk = false;
            std::snprintf(ach, sizeof ach, "  guard %d state mask: ref 0x%llx native 0x%llx", pg->palo->oid, (unsigned long long)grf,
                          (unsigned long long)pg->grfState);
            st.amsg.push_back(ach);
        }
        const glm::vec4 v = ram.Vec4(a + 0x190);
        for (int l = 0; l < 3; ++l) field(l == 0 ? "+0x190.x" : l == 1 ? "+0x190.y" : "+0x190.z", v[l], St(pg).vecVel[l], 2e-3);
        field("heading +0x1A8", ram.F32(a + 0x1A8), St(pg).radHeading, 1e-4);
        field("turn gain +0x180", ram.F32(a + 0x180), St(pg).uTurn, 1e-4);
        // the stack
        const int cjob = ram.S32(a + 0x26C + 4);
        ++st.cfield;
        if (cjob == static_cast<int>(pg->stack.ajob.size())) ++st.cfieldOk;
        else
        {
            fOk = false;
            std::string s = "  guard " + std::to_string(pg->palo->oid) + " jobs: ref";
            for (int i = 0; i < cjob && i < 8; ++i)
            {
                std::snprintf(ach, sizeof ach, " %X", CidOfVt(ram.U32(ram.U32(a + 0x26C + 8 + 4 * i) + 0xC)));
                s += ach;
            }
            s += " native";
            for (auto& pj : pg->stack.ajob)
            {
                std::snprintf(ach, sizeof ach, " %X", pj->cid);
                s += ach;
            }
            st.amsg.push_back(s);
        }
        for (int i = 0; i < cjob && i < static_cast<int>(pg->stack.ajob.size()); ++i)
        {
            const uint32_t aj = ram.U32(a + 0x26C + 8 + 4 * i);
            AIJOB* pj = pg->stack.ajob[i].get();
            ++st.cjob;
            const CID cid = CidOfVt(ram.U32(aj + 0xC));
            bool fJob = cid == pj->cid;
            std::string why;
            auto jf = [&](const char* psz, double ref, double nat, double tol) {
                if (std::fabs(ref - nat) <= tol * std::max(1.0, std::fabs(ref))) return;
                fJob = false;
                std::snprintf(ach, sizeof ach, " %s ref %.4f native %.4f;", psz, ref, nat);
                why += ach;
            };
            if (fJob)
                switch (cid)
                {
                case 0x9C: jf("msg", ram.S32(aj + 0x30), static_cast<JOBREACT*>(pj)->msg.k, 0); break;
                case 0x9D: case 0x9E:
                    jf("st", ram.S32(aj + 0x30), static_cast<JOBMOVE*>(pj)->st, 0);
                    jf("anim", ram.S32(aj + 0x3C), static_cast<JOBMOVE*>(pj)->nAnim, 0);
                    break;
                case 0xA7:
                {
                    auto* j = static_cast<JOBFOLLOW*>(pj);
                    jf("st", ram.S32(aj + 0x4C), j->st, 0);
                    jf("s", ram.F32(aj + 0x34), j->s, 1e-3);
                    jf("way", ram.S32(aj + 0x58), j->nWay, 0);
                    const glm::vec4 p = ram.Vec4(aj + 0x80), vv = ram.Vec4(aj + 0x90);
                    for (int l = 0; l < 3; ++l) jf("pos", p[l], j->pos[l], 1e-3);
                    for (int l = 0; l < 3; ++l) jf("vel", vv[l], j->vel[l], 1e-3);
                    jf("tLeft", ram.F32(aj + 0xE0), j->tLeft, 1e-4);
                    break;
                }
                case 0xA9: jf("running", ram.S32(aj + 0x40), static_cast<JOBSCRIPT*>(pj)->fRunning, 0); break;
                case 0xAA: jf("st", ram.S32(aj + 0x38), static_cast<JOBWAIT*>(pj)->st, 0); break;
                case 0xA5: jf("st", ram.S32(aj + 0x34), static_cast<JOBPURSUE*>(pj)->st, 0); break;
                case 0xA4: jf("st", ram.S32(aj + 0x34), static_cast<JOBACT*>(pj)->st, 0); break;
                case 0xB3: jf("st", ram.S32(aj + 0x30), static_cast<JOBSURROUND*>(pj)->st, 0); break;
                case 0xAB: jf("st", ram.S32(aj + 0x30), static_cast<JOBINVESTIGATE*>(pj)->st, 0); break;
                case 0x9F: jf("st", ram.S32(aj + 0x30), static_cast<JOBMOVE*>(pj)->st, 0); break;
                case 0xBA: jf("st", ram.S32(aj + 0x30), static_cast<JOBGOTO*>(pj)->st, 0); break;
                case 0xC0:
                    jf("s", ram.F32(aj + 0x38), static_cast<JOBPATHSTEP*>(pj)->s, 1e-3);
                    break;
                default: break;
                }
            st.cjobOk += fJob;
            if (!fJob)
            {
                fOk = false;
                std::snprintf(ach, sizeof ach, "  guard %d job %d (0x%X / native 0x%X):", pg->palo->oid, i, cid, pj->cid);
                st.amsg.push_back(ach + why);
            }
        }
        // the detector's entries' states (0 / 1 seen / 2 lost)
        if (const uint32_t ad = ram.U32(a + 0xF64); ad && pg->pdet)
            for (int i = 0; i < ram.S32(ad + 0xFC) && i < 64; ++i)
            {
                const uint32_t pe = ad + 0x100 + 0x10 * i;
                LO* plo = Plo(pref, ram.U32(pe));
                const DETENTRY* pn = plo ? PdetentryFind(pg->pdet, plo)
                                         : i < static_cast<int>(pg->pdet->ae.size()) ? &pg->pdet->ae[i] : nullptr;
                ++st.cfield;
                if (pn && pn->st == ram.S32(pe + 4)) { ++st.cfieldOk; continue; }
                fOk = false;
                std::snprintf(ach, sizeof ach, "  guard %d detector entry %d (oid %d): ref %d native %d", pg->palo->oid, i,
                              plo ? plo->oid : -1, ram.S32(pe + 4), pn ? pn->st : -9);
                st.amsg.push_back(ach);
            }
        st.cguardOk += fOk;
    }
    return st;
}

CID CidOfJobVt(uint32_t vt) { return CidOfVt(vt); }

// The hits other code delivered to the guards during the frame A -> B (the reference's contacts: a player's attack):
// a reaction job whose message in B is a new hit kind (9 / 10 / 11 / 0xB, its time B's) gets it as message 12, the
// contact rebuilt from the copy (its amount and push as the handler left them).
void AiDeliverHits(const RAM& ramA, const RAM& ramB, AIREF* pref)
{
    const float tB = ramB.F32(0x2DDED0);
    for (auto& [a, pg] : pref->aguard)
    {
        const uint32_t jA = ramA.U32(a + 0x26C + 8), jB = ramB.U32(a + 0x26C + 8);
        if (!ramB.FValid(jB) || pg->stack.ajob.empty()) continue;
        const int k = ramB.S32(jB + 0x30);
        if (k != 9 && k != 10 && k != 11 && k != 0xB) continue;
        if (ramB.F32(jB + 0xA0) != tB) continue;
        if (ramA.FValid(jA) && ramA.S32(jA + 0x30) == k && ramA.S32(jA + 0x4C) == ramB.S32(jB + 0x4C)) continue;
        const AIMSG::HIT h = HitOfRam(ramB, pref, jB + 0x40);
        CTMSG ctm;
        ctm.ploSender = h.psource;
        ctm.ploReceiver = h.preceiver;
        ctm.grfFilter = h.kind;
        ctm.nState = h.n;
        ctm.pos = h.pos;
        ctm.vec = h.vec;
        ctm.grfAccepted = h.grf;
        OfferJobMessage(pg, 12, &ctm);
    }
}

