// Sly 3's trigger volumes (class 0x15) and the run-time add / remove of objects: trigger3.h has the original's flow.

#include "trigger3.h"

#include "sly3.h"
#include "task3.h"
#include "../activate.h"
#include "../brx.h"
#include "../clock.h"
#include "../cls.h"
#include "../lo.h"
#include "../sw.h"
#include "../worldaccess.h"
#include "../xf.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>


namespace sly3
{
TRIGGERHOST3 g_trighost3;

namespace
{
// The trigger component (+0x120 of the class-0x15 object; fields as offsets of the object)
struct TRIG3
{
    LOXSHAPE* plo = nullptr;
    int state = -1;               // +0x120 (InitLo 0x15CB98 via 0x15BDF8: -1)
    float tState = 0.0f;          // +0x124 (0x36BB98 at each change)
    float dtRearm = 1.0f;         // +0x128 (0x15BDF8: 1.0; < 0: stays entered)
    int fStay = 0;                // +0x12C (the load 0x15CCB0: 1 when +0x244; 0: an enter goes on to state 2)
    int fRemoveOnEnter = 0;       // +0x130
    LO* ploFirst = nullptr;       // +0x134: the first object inside at the last check
    std::vector<LO*> aploWatch;   // +0x138 / +0x13C (0x15C260 / 0x15C2B8: up to 8)
    std::vector<int> acid;        // +0x17C / +0x180 (EOPID 483 via 0x15C4A8: up to 4 classes)
    std::vector<int> apair;       // +0x1D8 / +0x1DC (EOPID 489: {task, state} pairs)
    LO* pscpt = nullptr;          // +0x190
    // the shape's bounds (LoadShapeFromBrx 0x27F158): +0x100 the centre, +0x110 the radius squared, +0x114 the BSP's
    // answer for a point just outside the bounds (the max corner + 1)
    glm::vec3 posCenter{0.0f};
    float sRadius2 = -1.0f;
    bool fOutside = false;
};
std::map<const LO*, TRIG3> s_mptrig;
SW* s_psw = nullptr;
bool s_fPrint = false;

// 0x2B9C08 (the BSP from the shape's root, as Sly 2's IbspFindPoint 0x118FB0 / cmworld.cpp FPointInBsp): inside when
// the point ends behind a node without a negative child
bool FPointInShapeBsp(const LOXSHAPE* p, const glm::vec3& pos)
{
    if (p->bsp.abspn.empty()) return false;
    int i = 0;
    for (int c = 0; c < 100000; ++c)
    {
        const BSPN& n = p->bsp.abspn[std::size_t(i)];
        float d = -1e8f;
        if (n.isurf >= 0 && n.isurf < int(p->geom.asurf.size()))
            d = glm::dot(p->geom.asurf[std::size_t(n.isurf)].normal, pos) - p->geom.asurf[std::size_t(n.isurf)].gDot;
        if (d < 1.0e-4f)  // 0x38D1B717
        {
            if (n.ibspNeg < 0 || n.ibspNeg >= int(p->bsp.abspn.size())) return true;
            i = n.ibspNeg;
        }
        else
        {
            if (n.ibspPos < 0 || n.ibspPos >= int(p->bsp.abspn.size())) return false;
            i = n.ibspPos;
        }
    }
    return false;
}

// orig3: 0x27F298 (the point in the shape's frame)
bool FInShape(const TRIG3& t, const glm::vec3& posLocal)
{
    const glm::vec3 d = posLocal - t.posCenter;
    if (!(glm::dot(d, d) <= t.sRadius2)) return t.fOutside;
    return FPointInShapeBsp(t.plo, posLocal);
}

// The bounds of LoadShapeFromBrx 0x27F158 (after ReadGeom / ReadBspTree)
void InitShapeBounds(TRIG3& t)
{
    const GEOM& g = t.plo->geom;
    if (g.apos.empty()) return;  // (+0x110 = the constant 0x309530, +0x114 = 0: never inside)
    glm::vec3 lo = g.apos[0], hi = g.apos[0];
    for (const glm::vec3& p : g.apos)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    t.posCenter = lo * 0.5f + hi * 0.5f;
    const glm::vec3 r = t.posCenter - lo;
    t.sRadius2 = glm::dot(r, r);
    t.fOutside = FPointInShapeBsp(t.plo, hi + glm::vec3(1.0f));  // + 0x317570 (1, 1, 1)
}

LO* PloFind(int oid) { return s_psw ? PloFindSwObjectByOid(s_psw, oid) : nullptr; }

// orig3: 0x15BFF8 (the component, fEnter)
void FireTrigger(TRIG3& t, bool fEnter)
{
    // (RunDockRsmgs 0x25D3F8 over +0x194 / +0x198 and the SCPT list +0x1FC: no m_ext trigger has them (TODO(sly3)))
    if (!fEnter) return;
    if (t.pscpt && g_trighost3.pfnStartScpt) g_trighost3.pfnStartScpt(t.pscpt, t.ploFirst);
    if (s_fPrint) std::printf("trigger3: %d fires (%zu task pairs)\n", t.plo->oid, t.apair.size() / 2);
    if (!t.apair.empty()) ApplyCheckpointTasks3(t.apair.data(), int(t.apair.size() / 2), t.plo);
}

// orig3: 0x15C178 (the component's state)
void SetTriggerState(TRIG3& t, int n)
{
    const int nOld = t.state;
    if (nOld == n) return;
    const bool fEnter = nOld == 0 && n == 1;
    const bool fExit = nOld != -1 && n == 0;
    t.state = n;
    t.tState = g_clock.t;
    if (s_fPrint) std::printf("trigger3: %d state %d -> %d (t %.3f)\n", t.plo->oid, nOld, n, g_clock.t);
    if (fEnter)
    {
        // 0x15BF28: the message 5 to the trigger, then the fire
        if (g_trighost3.pfnMessage) g_trighost3.pfnMessage(t.plo, 5);
        FireTrigger(t, true);
        if (!t.fStay) SetTriggerState(t, 2);
        if (t.fRemoveOnEnter) RemoveLo(s_psw, t.plo);  // the owner's vt slot 9
    }
    if (fExit)
    {
        // 0x15BF90: the message 6, the fire's exit part
        if (g_trighost3.pfnMessage) g_trighost3.pfnMessage(t.plo, 6);
        FireTrigger(t, false);
    }
}

// orig3: 0x15D2F0 (the objects inside: up to cMax)
std::vector<LO*> AploInside(TRIG3& t, std::size_t cMax)
{
    std::vector<LO*> acand;
    if (t.aploWatch.empty() && t.acid.empty())
    {
        // the players (0x1F4B30 over the two pad slots 0x36E740 + i * 0x11C0): the native world's player actors
        for (LO* plo : s_psw->mpcidaplo[sly3::CID_JT]) acand.push_back(plo);
    }
    else
    {
        acand = t.aploWatch;
        for (int cid : t.acid)  // CFindObjectsByClass 0x17E208(sw, 0x505, cid, 0, max - count, ...)
            for (LO* plo : s_psw->mpcidaplo[cid])
                if (acand.size() < cMax) acand.push_back(plo);
    }
    std::vector<LO*> a;
    if (acand.empty()) return a;
    const glm::mat4 matInv = glm::inverse(MatLoWorld3(t.plo));  // InverseAffineMatrix4 of vt slot 36's matrix
    for (LO* plo : acand)
    {
        static const int s_oidDbg = std::getenv("SLY3_TRIGDBG") ? std::atoi(std::getenv("SLY3_TRIGDBG")) : -2;
        if (t.plo->oid == s_oidDbg)
        {
            const glm::vec3 p = PosLoWorld3(plo), q(matInv * glm::vec4(p, 1.0f));
            std::printf("trigdbg %d: candidate %d inworld %d at %.1f %.1f %.1f local %.1f %.1f %.1f in %d\n", t.plo->oid, plo->oid,
                        FLoInWorld3(plo), p.x, p.y, p.z, q.x, q.y, q.z, FInShape(t, q));
        }
        if (!FLoInWorld3(plo)) continue;
        const glm::vec3 posLocal(matInv * glm::vec4(PosLoWorld3(plo), 1.0f));
        if (!FInShape(t, posLocal)) continue;
        a.push_back(plo);
        if (a.size() >= cMax) break;
    }
    return a;
}

// orig3: 0x15D510 (vt slot 55)
void CheckTrigger(TRIG3& t)
{
    if (t.state == 2)
    {
        if (t.dtRearm >= 0.0f && g_clock.t - t.tState > t.dtRearm) SetTriggerState(t, 1);
        return;
    }
    if (t.state >= 3 || t.state < 0) return;  // (-1: not reset yet)
    // (+0x244: the sorted list of objects inside, its change sends the message 9: no m_ext trigger sets it)
    const std::vector<LO*> a = AploInside(t, 0x40);
    if (!a.empty())
    {
        t.ploFirst = a[0];
        SetTriggerState(t, 1);
    }
    else
    {
        t.ploFirst = nullptr;
        SetTriggerState(t, 0);
    }
}

}  // namespace

bool FLoInWorld3(const LO* plo)
{
    for (const LO* p = plo; p; p = p->paloParent)
        if (p->fRemoved) return false;
    return true;
}

glm::mat4 MatLoWorld3(const LO* plo)
{
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
    {
        // (the ALO's +0x48 record: its POS reference, e.g. Sly's record 1, else the first)
        const ALO* palo = static_cast<const ALO*>(plo);
        if (palo->apxfRef[0]) return palo->apxfRef[0]->matWorld;
        return palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    }
    if (FIsLoDerivedFrom(plo, sly3::CID_LOX))
    {
        const LOX* plox = static_cast<const LOX*>(plo);
        return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    }
    return glm::mat4(1.0f);
}

glm::vec3 PosLoWorld3(const LO* plo)
{
    glm::vec3 pos;
    if (g_trighost3.pfnPos && g_trighost3.pfnPos(plo, pos)) return pos;
    return glm::vec3(MatLoWorld3(plo)[3]);
}

void PlaceLo3(LO* plo, const float mat[9], const glm::vec3& pos)
{
    if (!plo) return;
    glm::mat4 m(1.0f);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r][c] = mat[r * 3 + c];
    m[3] = glm::vec4(pos, 1.0f);
    if (s_fPrint) std::printf("trigger3: place %d at %.1f %.1f %.1f\n", plo->oid, pos.x, pos.y, pos.z);
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (palo->axf.empty()) return;
        // the first record's world matrix W0 becomes m x inverse(the +0x48 record's world) x W0; its local follows
        XF* pxf0 = palo->axf[0].get();
        const glm::mat4 w0 = pxf0->matWorld;
        const glm::mat4 w0New = m * glm::inverse(MatLoWorld3(plo)) * w0;
        pxf0->mat = pxf0->mat * glm::inverse(w0) * w0New;
        UpdateAloXfWorlds(palo);
    }
    else if (FIsLoDerivedFrom(plo, sly3::CID_LOX))
    {
        LOX* plox = static_cast<LOX*>(plo);
        plox->mat = plox->pxf ? glm::inverse(plox->pxf->matWorld) * m : m;
    }
}

int TriggerState3(const LO* plo)
{
    auto it = s_mptrig.find(plo);
    return it == s_mptrig.end() ? -1 : it->second.state;
}

void StartTriggers3(SW* psw)
{
    s_psw = psw;
    s_mptrig.clear();
    s_fPrint = std::getenv("SLY3_TRIGPRINT") != nullptr;
    for (LO* plo : psw->mpcidaplo[sly3::CID_TRIGGER])
    {
        TRIG3& t = s_mptrig[plo];
        t.plo = static_cast<LOXSHAPE*>(plo);
        InitShapeBounds(t);
        for (const OPT& o : plo->aopt)
        {
            if (o.eopid == 483 && !o.aval.empty() && t.acid.size() < 4) t.acid.push_back(o.aval[0].n);  // 0x1064E0
            if (o.eopid == 489)  // 0x1066B8: {task, state} pairs
                for (std::size_t i = 0; i + 1 < o.aval.size(); i += 2)
                {
                    t.apair.push_back(o.aval[i].n);
                    t.apair.push_back(o.aval[i + 1].n);
                }
        }
        if (s_fPrint)
        {
            const glm::vec3 p(MatLoWorld3(plo)[3]);
            std::printf("trigger3: %d at %.1f %.1f %.1f (%zu points, %zu nodes, r %.1f%s), %zu classes, %zu task pairs\n",
                        plo->oid, p.x, p.y, p.z, t.plo->geom.apos.size(), t.plo->bsp.abspn.size(),
                        t.sRadius2 >= 0 ? std::sqrt(t.sRadius2) : -1.0f, t.fOutside ? ", inside-out" : "", t.acid.size(),
                        t.apair.size() / 2);
        }
    }
}

void UpdateTriggers3(SW* psw)
{
    if (psw != s_psw) return;
    static const int s_oidDbg = std::getenv("SLY3_TRIGDBG") ? std::atoi(std::getenv("SLY3_TRIGDBG")) : -2;
    for (auto& [plo, t] : s_mptrig)
    {
        if (plo->oid == s_oidDbg)
            std::printf("trigdbg %d (%p): state %d removed %d inworld %d parent %d\n", plo->oid, (const void*)plo, t.state,
                        plo->fRemoved, FLoInWorld3(plo), plo->paloParent ? plo->paloParent->oid : -1);
        // (a trigger in the world from its start (the world start's restore added it before any update) is reset
        // first: the start 0x15CCE8 -> 0x15D100 sets its state; natively the removal's branch below did it)
        if (FLoInWorld3(plo) && t.state == -1) SetTriggerState(t, 0);
        if (FLoInWorld3(plo)) CheckTrigger(t);
        // the deactivation of a removed trigger (RemoveLo, slot 9: 0x15D048 -> 0x15D288 / slot 31 0x15D2A8 -> 0x15BE10):
        // state 0 (the exit when it was entered)
        else if (t.state != 0) SetTriggerState(t, 0);
    }
}
}  // namespace sly3
