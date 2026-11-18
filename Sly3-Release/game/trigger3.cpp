// Sly 3's trigger volumes (class 0x15) and the run-time add / remove of objects: trigger3.h has the original's flow.

#include "trigger3.h"

#include "sly3.h"
#include "task3.h"
#include "scptgame3.h"
#include "scptrun3.h"
#include "../activate.h"
#include "../animfind.h"
#include "../objmethod.h"
#include "../ref.h"
#include "../actor.h"
#include "../brx.h"
#include "../clock.h"
#include "../cls.h"
#include "../lo.h"
#include "../sw.h"
#include "../worldaccess.h"
#include "../xf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_set>
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
    // (the shape tested: the object's own, or its clone source's: a copy (CloneLo) shares the source's geometry and BSP
    // natively, as trigger.cpp / detect.cpp read them; e.g. Ep5's battle ships are copies of 44989 and their boarding
    // volume 45030 had no shape: never entered)
    // (s3-hack round 11: the same for the Hackathon docks' 92891 under each dock 92946 / 92939: no ship could dock)
    const LOXSHAPE* pshape = nullptr;
    int state = -1;               // +0x120 (InitLo 0x15CB98 via 0x15BDF8: -1)
    float tState = 0.0f;          // +0x124 (0x36BB98 at each change)
    float dtRearm = 1.0f;         // +0x128 (0x15BDF8: 1.0; < 0: stays entered)
    int fStay = 0;                // +0x12C (the load 0x15CCB0: 1 when +0x244; 0: an enter goes on to state 2)
    int fRemoveOnEnter = 0;       // +0x130
    LO* ploFirst = nullptr;       // +0x134: the first object inside at the last check
    std::vector<LO*> aploWatch;   // +0x138 / +0x13C (0x15C260 / 0x15C2B8: up to 8)
    bool fList = false;           // +0x244 (EOPID 478): the objects inside kept (+0x23C / +0x240), a change sends message 9
    std::vector<LO*> aploList;    // +0x23C / +0x240 (sorted; EOPID 479 = optid 586 reads it: not registered, see PaploTriggerInside3)
    std::vector<int> aoidWatch;   // (EOPID 480 via 0x106348 -> 0x15C260: the watched objects' oids, resolved at the first check)
    bool fWatchResolved = false;
    std::vector<int> acid;        // +0x17C / +0x180 (EOPID 483 via 0x15C4A8: up to 4 classes)
    std::vector<int> apair;       // +0x1D8 / +0x1DC (EOPID 489: {task, state} pairs)
    // +0x1FC (EOPID 490 via 0x106760 -> 0x15C688: {SCPT oid, the alarm set on entering, the alarm on leaving})
    std::vector<std::array<int, 3>> aalarm;
    // +0x194 / +0x198 (EOPID 488 via 0x1065F0 -> 0x15C610 -> 0x25D350: {fOn, oid, SM} keyed, the enter / exit goals;
    // at most 3): the SMs sent to their goals by the fire (0x25D3F8)
    struct DOCK { int oid, oidSm, oidOn, oidOff; };
    std::vector<DOCK> adock;
    LO* pscpt = nullptr;          // +0x190
    // the shape's bounds (LoadShapeFromBrx 0x27F158): +0x100 the centre, +0x110 the radius squared, +0x114 the BSP's
    // answer for a point just outside the bounds (the max corner + 1)
    glm::vec3 posCenter{0.0f};
    float sRadius2 = -1.0f;
    bool fOutside = false;
    // (s3fix-ai2) class 0x17 (NEW_VOLTRIGGER_17, Init 0x27F658: +0x244 1, the class 0x3A listed, +0x254 0x03FFFFFF): the
    // guards' trigger reaction. +0x250 its kind (EOPID 1775), +0x254 the records' mask (1776), +0x258 an object the guard
    // must be aware of (1777); its vt slot 55 0x27F6A0 hands each guard new in the list to 0x257A70
    bool f17 = false;
    int k17 = 0;
    uint32_t mask17 = 0x03FFFFFFu;
    int oidSource17 = -1;
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
    return FPointInShapeBsp(t.pshape, posLocal);
}

// The bounds of LoadShapeFromBrx 0x27F158 (after ReadGeom / ReadBspTree)
void InitShapeBounds(TRIG3& t)
{
    t.pshape = t.plo;
    for (const LO* p = t.plo; p && t.pshape->geom.apos.empty() && p->ploCloneOf && p->ploCloneOf != p; p = p->ploCloneOf)
        if (const LOXSHAPE* q = dynamic_cast<const LOXSHAPE*>(p->ploCloneOf); q && !q->geom.apos.empty()) t.pshape = q;
    const GEOM& g = t.pshape->geom;
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
    t.fOutside = FPointInShapeBsp(t.pshape, hi + glm::vec3(1.0f));  // + 0x317570 (1, 1, 1)
}

LO* PloFind(int oid) { return s_psw ? PloFindSwObjectByOid(s_psw, oid) : nullptr; }

// orig3: 0x15BFF8 (the component, fEnter)
void FireTrigger(TRIG3& t, bool fEnter)
{
    // RunDockRsmgs 0x25D3F8 over +0x194 / +0x198 (0x25D478 flags fEnter: the enter goal, else the exit goal)
    for (const TRIG3::DOCK& d : t.adock)
    {
        const int oidGoal = fEnter ? d.oidOn : d.oidOff;
        if (s_fPrint) std::printf("trigger3: %d %s: SM of %d (%d) to %d\n", t.plo->oid, fEnter ? "enter" : "exit", d.oid, d.oidSm, oidGoal);
        if (oidGoal != -1 && g_trighost3.pfnSmGoal) g_trighost3.pfnSmGoal(d.oid, d.oidSm, oidGoal, t.plo);
    }
    // the SCPT list +0x1FC: the scripts' alarms (0x2261A0 set / 0x2261F0 clear); entering sets the first, leaving clears
    // it when the two are the same, else sets the second (m_ext's 16375: SCPT 4273's alarm 15176, the reminder's wait)
    for (const auto& e : t.aalarm)
    {
        LO* plo = PloFind(e[0]);   // (0x17E1B8 kind 4 from the trigger)
        SCPT3* ps = plo ? PscptOfLo3(plo) : nullptr;
        if (!ps) continue;
        if (fEnter)
        {
            if (e[1] != -1) SetScptAlarm3(ps, e[1], true);
        }
        else if (e[2] == e[1])
        {
            if (e[2] != -1) SetScptAlarm3(ps, e[2], false);
        }
        else if (e[2] != -1) SetScptAlarm3(ps, e[2], true);
        if (s_fPrint) std::printf("trigger3: %d %s: SCPT %d alarm %d\n", t.plo->oid, fEnter ? "enter" : "exit", e[0], fEnter ? e[1] : e[2]);
    }
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

// the candidate's place: vt slot 36's matrix (ALO 0x23CFB8: the object record +0x50's world matrix; e.g. the dump
// truck's position record is 1160 under it), else PosLoWorld3's
glm::vec3 PosLoTest3(const LO* plo)
{
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
        if (const XF* pxf = static_cast<const ALO*>(plo)->apxfRef[2]) return glm::vec3(pxf->matWorld[3]);
    return PosLoWorld3(plo);
}

// orig3: 0x15D2F0 (the objects inside: up to cMax)
std::vector<LO*> AploInside(TRIG3& t, std::size_t cMax)
{
    std::vector<LO*> acand;
    if (!t.fWatchResolved)
    {
        // (the loaded oids first, then what the scripts added before the first check: at most 8 in all)
        t.fWatchResolved = true;
        std::vector<LO*> a;
        for (int oid : t.aoidWatch)
        {
            LO* plo = PloFindSwNearest(t.plo, oid);
            if (!plo) plo = PloFind(oid);
            if (plo && FIsLoDerivedFrom(plo, 0x2F) && a.size() < 8) a.push_back(plo);
        }
        for (LO* plo : t.aploWatch)
            if (a.size() < 8) a.push_back(plo);
        t.aploWatch = a;
    }
    if (t.aploWatch.empty() && t.acid.empty())
    {
        // the players (0x1F4B30 over the two pad slots 0x36E740 + i * 0x11C0): the native world's player actors
        for (LO* plo : s_psw->mpcidaplo[sly3::CID_JT]) acand.push_back(plo);
    }
    else
    {
        acand = t.aploWatch;
        // CFindObjectsByClass 0x17E208(sw, 0x505, cid, 0, max - count, ...): kind 5, the class and the classes derived
        // from it (cid, cid + 1, .. while 0x13BEC8 says derived). (s3-hack round 11: the exact class only before: e.g.
        // the 17 triggers listing 0x33 PLAYER never saw Sly, Bentley or Murray, the 47 listing 0x3A STEPGUARD never
        // Carmelita or the other guard kinds)
        static const bool s_fExact = std::getenv("SLY3_TRIGOLD") != nullptr;   // (debug, A/B: the round-10 behaviour)
        for (int cid : t.acid)
            for (int icid = cid; icid >= 0 && icid < CID_Max && FCidDerivedFrom(CID(icid), CID(cid)) && (!s_fExact || icid == cid); ++icid)
                for (LO* plo : s_psw->mpcidaplo[icid])
                    if (acand.size() < cMax) acand.push_back(plo);
    }
    std::vector<LO*> a;
    if (acand.empty()) return a;
    const glm::mat4 matInv = glm::inverse(MatLoWorld3(t.plo));  // InverseAffineMatrix4 of vt slot 36's matrix
    for (LO* plo : acand)
    {
        static const int s_oidDbg = std::getenv("SLY3_TRIGDBG") ? std::atoi(std::getenv("SLY3_TRIGDBG")) : -2;
        if (t.plo->oid == s_oidDbg && std::getenv("SLY3_TRIGGRID"))
        {
            static bool s_fDone = false;
            if (!s_fDone)
            {
                s_fDone = true;
                for (float z = -1000.0f; z <= 700.0f; z += 200.0f)
                {
                    std::printf("triggrid z %5.0f:", z);
                    for (float y = -3800.0f; y <= 3000.0f; y += 400.0f)
                    {
                        std::printf(" ");
                        for (float x = -1100.0f; x <= 1100.0f; x += 275.0f) std::printf("%c", FInShape(t, glm::vec3(x, y, z)) ? '#' : '.');
                    }
                    std::printf("\n");
                }
            }
        }
        if (t.plo->oid == s_oidDbg)
        {
            const glm::vec3 p = PosLoTest3(plo), q(matInv * glm::vec4(p, 1.0f));
            std::printf("trigdbg %d: candidate %d (cid 0x%X %p) inworld %d at %.1f %.1f %.1f local %.1f %.1f %.1f in %d\n", t.plo->oid, plo->oid,
                        plo->Cid(), (void*)plo, FLoInWorld3(plo), p.x, p.y, p.z, q.x, q.y, q.z, FInShape(t, q));
        }
        if (!FLoInWorld3(plo)) continue;
        const glm::vec3 posLocal(matInv * glm::vec4(PosLoTest3(plo), 1.0f));
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
    std::vector<LO*> a = AploInside(t, 0x40);
    // +0x244: the sorted list of objects inside; its change sends the message 9 (vt slot 3: e.g. o_quarry's 121591, the
    // tower's guards: its splice closes 2709 once none is left inside)
    if (t.fList)
    {
        std::vector<LO*> as = a;
        std::sort(as.begin(), as.end());   // (0x292540 with 0x15D2D8: by address)
        if (as != t.aploList)
        {
            // (s3fix-ai2) orig3 0x27F6A0 (class 0x17's slot 55): the guards new in the list since the last check, each to
            // 0x257A70 (the guard's trigger reaction: aijobs.cpp)
            if (t.f17)
            {
                LO* psource = t.oidSource17 > 0 ? PloFind(t.oidSource17) : nullptr;
                for (LO* plo : as)
                    if (!std::binary_search(t.aploList.begin(), t.aploList.end(), plo))
                        OnTriggerGuardEnter3(plo, t.plo, t.k17, t.mask17, psource);
            }
            t.aploList = as;
            if (s_fPrint) std::printf("trigger3: %d inside list changed: %zu\n", t.plo->oid, as.size());
            if (g_trighost3.pfnMessage) g_trighost3.pfnMessage(t.plo, 9);
        }
    }
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

// (s3-player round 21) a root actor placed through its slots (its base record at the identity: transition.cpp) with its
// +0x48 record (MatLoWorld3) at m, as the first-record placement put it: TeleportAloMat takes the step's base (the
// actor's +0x240 offset and the class's own position slot between them: Sly's record lands 72 high, Bentley's 42 low
// of the plain matrix), so the translation is corrected once by what it missed. Without it Sly landed 72 high and fell
// onto the floor after every script placement.
void TeleportRootActor3(LO* plo, const glm::mat4& m)
{
    TeleportAloMat(plo, m, nullptr);
    if (!FIsLoDerivedFrom(plo, sly3::CID_ALO)) return;
    UpdateAloXfWorlds(static_cast<ALO*>(plo));
    const glm::vec3 d = glm::vec3(m[3]) - glm::vec3(MatLoWorld3(plo)[3]);
    if (glm::length(d) < 0.01f) return;
    glm::mat4 m2 = m;
    m2[3] += glm::vec4(d, 0.0f);
    TeleportAloMat(plo, m2, nullptr);
    UpdateAloXfWorlds(static_cast<ALO*>(plo));
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

namespace { std::unordered_set<const LO*> s_setPlaced3; }
// (s3-player round 21) the objects a script placed since the world's load (the transition's instance matrices then leave
// them: m_ext's attract SCPT 4262 places Sly on the rope at the world start, inside StartSw)
bool FScriptPlaced3(const LO* plo) { return s_setPlaced3.count(plo) != 0; }
void ResetScriptPlaced3() { s_setPlaced3.clear(); }

void PlaceLo3(LO* plo, const float mat[9], const glm::vec3& pos)
{
    if (!plo) return;
    s_setPlaced3.insert(plo);
    glm::mat4 m(1.0f);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r][c] = mat[r * 3 + c];
    m[3] = glm::vec4(pos, 1.0f);
    if (s_fPrint) std::printf("trigger3: place %d (cid 0x%X) at %.1f %.1f %.1f\n", plo->oid, unsigned(plo->Cid()), pos.x, pos.y, pos.z);
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
    {
        // orig3: the object's vt slot 43 with no frame (ALO 0x23DA68 -> 0x1C9008: the position record through slot 34,
        // the rotation record through slot 35 (Sly 2's 29 / 30), with a physics state zero velocities (40 / 41); a
        // step's 0x24F6F8 first takes its base +0x260 off in the matrix's frame): actor.cpp TeleportAloMat, as Sly 2's
        // slot 38. Before (s3-player round 18) the whole matrix went into the first record (for a parented object its
        // attach record): m4_rccar's cart 69349, placed by SCPT 3681 with its rotation, turned twice (its rotation record
        // already held the 35.8 degrees) and Murray, harnessed to it, landed 72 low (his base not taken off); both now
        // equal the reference's records. A root actor (Sly / Bentley / Murray / ...) keeps the first-record placement
        // (then PlaceLoScript3's step heading). Round 19 (SLY3_ROOTTELE=1: through the slots as the others): v_interpol's
        // 2128 now closes either way (SCPT 2129 requests it at the same frame), Sly at the same world place on the dock
        // (6888 7814 161), but facing 104.1 instead of the script's -0.2: natively a root actor's base record (+0x48's
        // +0xA0, xf0) holds the world file's placement (here turned 104.1), where the reference's is the identity, and
        // the step heading (0x24E878) writes the rotation record's local angle as if its parent were unturned. TODO(sly3):
        // the base record at the identity as the reference's (the players' body load), then the slots for root actors.
        ALO* palo = static_cast<ALO*>(plo);
        static const bool s_fRootTele = [] { const char* sz = std::getenv("SLY3_ROOTTELE"); return !sz || sz[0] != '0'; }();
        if (PactorFromLo(plo) && !plo->paloParent && !s_fRootTele)
        {
            if (palo->axf.empty()) return;
            // the first record's world matrix W0 becomes m x inverse(the +0x48 record's world) x W0; its local follows
            XF* pxf0 = palo->axf[0].get();
            const glm::mat4 w0 = pxf0->matWorld;
            const glm::mat4 w0New = m * glm::inverse(MatLoWorld3(plo)) * w0;
            pxf0->mat = pxf0->mat * glm::inverse(w0) * w0New;
            UpdateAloXfWorlds(palo);
            return;
        }
        // (s3-integrate) a root actor through the same slots: the record at the point less the base turned by the matrix
        // (0x24F6F8), as the reference's: h_ext's SCPT 3282 [6] puts Murray at z 257.5 and the reference's record is at
        // 324.0 (his base -71.8 natively; round 21's record at the point itself made him rise 68 over 20 frames). The
        // record set absolutely (TeleportRootActor3: TeleportAloMat alone is not, for a root actor before the world
        // start's base-record reset). A placement where the actor stands (no target object) reads his record plus the
        // base back (scptgame3.cpp ActorAimOffset, 0x22AEC8), so it leaves him there.
        if (ACTOR* pa = PactorFromLo(plo); pa && !plo->paloParent && !std::getenv("SLY3_PLACEBASE0"))
        {
            glm::mat4 mBase = m;
            mBase[3] = glm::vec4(glm::vec3(m * glm::vec4(-glm::vec3(pa->vecBase), 1.0f)), 1.0f);
            TeleportRootActor3(plo, mBase);
            return;
        }
        TeleportAloMat(plo, m, nullptr);
    }
    else if (FIsLoDerivedFrom(plo, sly3::CID_LOX))
    {
        LOX* plox = static_cast<LOX*>(plo);
        plox->mat = plox->pxf ? glm::inverse(plox->pxf->matWorld) * m : m;
    }
}

const std::vector<LO*>* PaploTriggerInside3(const LO* plo)
{
    auto it = s_mptrig.find(plo);
    return it == s_mptrig.end() ? nullptr : &it->second.aploList;
}

int TriggerState3(const LO* plo)
{
    auto it = s_mptrig.find(plo);
    return it == s_mptrig.end() ? -1 : it->second.state;
}

// orig3: 0x15BF28 (the component's vt slot 2, SCPT 0x12 on a VOLTRIGGER (0x2271C8)): the message 5 to the trigger, then
// the fire's enter part, without a state change
bool FActivateTrigger3(LO* plo)
{
    auto it = s_mptrig.find(plo);
    if (it == s_mptrig.end()) return false;
    if (s_fPrint) std::printf("trigger3: %d activated by a script\n", plo->oid);
    if (g_trighost3.pfnMessage) g_trighost3.pfnMessage(plo, 5);
    FireTrigger(it->second, true);
    return true;
}

// (the old world's end, transition.cpp UnloadWorld through ResetTriggers: no record outlives its object)
// orig3: VOLTRIGGER's slot 31 0x15D2A8 at the in-place world reset's start events (game/worldreset3.cpp): the base's,
// then the component's state 0 (0x15BE10 -> 0x15C178: the exit when it was entered or done), so a trigger that fired
// fires again
void ResetTriggersInPlace3()
{
    for (auto& [plo, t] : s_mptrig) SetTriggerState(t, 0);
}

void ResetTriggers3()
{
    s_mptrig.clear();
    s_psw = nullptr;
}

void AddNewTriggers3(SW* psw);
void StartTriggers3(SW* psw)
{
    s_psw = psw;
    s_mptrig.clear();
    s_fPrint = std::getenv("SLY3_TRIGPRINT") != nullptr;
    AddNewTriggers3(psw);
}

// (the world's triggers not set up yet: at the start all; later the copies made while it runs, e.g. the PC's four
// Hackathon exit docks (hackgame3.cpp MakeExitDocks) whose triggers were never checked)
void AddNewTriggers3(SW* psw)
{
    // (s3fix-ai2: the class-0x17 triggers too; natively they were never checked)
    std::vector<LO*> aplo = psw->mpcidaplo[sly3::CID_TRIGGER];
    aplo.insert(aplo.end(), psw->mpcidaplo[CID_TRIGGER17].begin(), psw->mpcidaplo[CID_TRIGGER17].end());
    for (LO* plo : aplo)
    {
        if (s_mptrig.count(plo)) continue;
        TRIG3& t = s_mptrig[plo];
        t.plo = static_cast<LOXSHAPE*>(plo);
        InitShapeBounds(t);
        if (plo->Cid() == CID_TRIGGER17)
        {
            // (Init 0x27F658: the list kept (+0x244 = 1, the load 0x15CCB0 makes +0x12C 1 from it), the class 0x3A)
            t.f17 = true;
            t.fList = t.fStay = true;
            t.acid.push_back(0x3A);
        }
        if (s_fPrint && plo->oid == (std::getenv("SLY3_TRIGDBG") ? std::atoi(std::getenv("SLY3_TRIGDBG")) : -2))
            std::printf("trigger3: %d bounds centre %.0f %.0f %.0f radius %.0f outside %d\n", plo->oid, t.posCenter.x, t.posCenter.y,
                        t.posCenter.z, std::sqrt(t.sRadius2), int(t.fOutside));
        const LO* ploOpt = plo;   // (a copy made at run time without options of its own: its source's)
        while (ploOpt->aopt.empty() && ploOpt->ploCloneOf && ploOpt->ploCloneOf != ploOpt) ploOpt = ploOpt->ploCloneOf;
        for (const OPT& o : ploOpt->aopt)
        {
            if (std::getenv("SLY3_TRIGOPTS"))
            {
                std::printf("trigopt %d: eopid %d grf 0x%X:", plo->oid, o.eopid, o.grf);
                for (const OPTVAL& v : o.aval) std::printf(" %d", v.n);
                std::printf("\n");
            }
            if (o.eopid == 483)  // 0x1064E0 -> 0x15C4A8: at most 4
                for (const OPTVAL& v : o.aval)
                    if (t.acid.size() < 4) t.acid.push_back(v.n);
            // EOPID 480 (0x106348 -> 0x15C2B8): an ALO watched (e.g. o_quarry's 121589 / 121590 watch the dump truck
            // 120533: SCPT 2708's alarms 120593 / 120595)
            if (o.eopid == 478) t.fList = t.fStay = o.aval.empty() ? 0 : o.aval[0].n != 0;   // (+0x244; 0x15CCB0: +0x12C)
            if (t.f17 && !o.aval.empty())
            {
                if (o.eopid == 1775) t.k17 = o.aval[0].n;
                if (o.eopid == 1776) t.mask17 = uint32_t(o.aval[0].n);
                if (o.eopid == 1777) t.oidSource17 = o.aval[0].n;
            }
            if (o.eopid == 480)
                for (const OPTVAL& v : o.aval)
                    if (v.n > 0 && t.aoidWatch.size() < 8) t.aoidWatch.push_back(v.n);
            if (o.eopid == 490 && std::getenv("SLY3_TRIGPRINT")) std::printf("trigger3: %d alarm entry %d %d %d\n", plo->oid, o.aval.size() > 0 ? o.aval[0].n : -9, o.aval.size() > 1 ? o.aval[1].n : -9, o.aval.size() > 2 ? o.aval[2].n : -9);
            if (o.eopid == 490 && o.aval.size() >= 3 && t.aalarm.size() < 8)  // 0x106760 -> 0x15C688
                t.aalarm.push_back({o.aval[0].n, o.aval[1].n, o.aval[2].n});
            if (o.eopid == 488 && o.aval.size() >= 4)  // 0x1065F0 -> 0x15C610 -> 0x25D350
            {
                const bool fOn = o.aval[0].n != 0;
                const int oid = o.aval[1].n, oidSm = o.aval[2].n, oidGoal = o.aval[3].n;
                bool fDone = false;
                for (TRIG3::DOCK& d : t.adock)
                    if (d.oid == oid && (d.oidSm == -1 || d.oidSm == oidSm))
                    {
                        (fOn ? d.oidOn : d.oidOff) = oidGoal;
                        fDone = true;
                        break;
                    }
                if (!fDone && t.adock.size() + 1 < 4)
                    t.adock.push_back({oid, oidSm, fOn ? oidGoal : -1, fOn ? -1 : oidGoal});
                if (s_fPrint) std::printf("trigger3: %d SM entry %d %d %d %d\n", plo->oid, int(fOn), oid, oidSm, oidGoal);
            }
            if (o.eopid == 489)  // 0x1066B8: {task, state} pairs
                for (std::size_t i = 0; i + 1 < o.aval.size(); i += 2)
                {
                    t.apair.push_back(o.aval[i].n);
                    t.apair.push_back(o.aval[i + 1].n);
                    if (std::getenv("SLY3_TRIGPRINT")) std::printf("trigger3: %d task pair {%d %d}\n", plo->oid, o.aval[i].n, o.aval[i + 1].n);
                }
        }
        if (s_fPrint && t.f17)
            std::printf("trigger3: %d class 0x17 kind %d mask 0x%X source %d\n", plo->oid, t.k17, t.mask17, t.oidSource17);
        if (s_fPrint)
        {
            const glm::vec3 p(MatLoWorld3(plo)[3]);
            for (int cid : t.acid) std::printf("trigger3: %d class 0x%X\n", plo->oid, cid);
            std::printf("trigger3: %d at %.1f %.1f %.1f (%zu points, %zu nodes, r %.1f%s), %zu classes, %zu task pairs\n",
                        plo->oid, p.x, p.y, p.z, t.plo->geom.apos.size(), t.plo->bsp.abspn.size(),
                        t.sRadius2 >= 0 ? std::sqrt(t.sRadius2) : -1.0f, t.fOutside ? ", inside-out" : "", t.acid.size(),
                        t.apair.size() / 2);
            if (!t.plo->geom.apos.empty())
            {
                glm::vec3 lo = t.plo->geom.apos[0], hi = lo;
                for (const glm::vec3& q : t.plo->geom.apos) lo = glm::min(lo, q), hi = glm::max(hi, q);
                std::printf("trigger3: %d local box %.0f %.0f %.0f .. %.0f %.0f %.0f\n", plo->oid, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
            }
        }
    }
}

int NTriggerKind17(const LO* plo)
{
    auto it = s_mptrig.find(plo);
    return it != s_mptrig.end() && it->second.f17 ? it->second.k17 : -1;
}

void UpdateTriggers3(SW* psw)
{
    if (psw != s_psw) return;
    if (psw->mpcidaplo[sly3::CID_TRIGGER].size() + psw->mpcidaplo[CID_TRIGGER17].size() != s_mptrig.size()) AddNewTriggers3(psw);
    static const int s_oidDbg = std::getenv("SLY3_TRIGDBG") ? std::atoi(std::getenv("SLY3_TRIGDBG")) : -2;
    static const bool s_fOn17 = std::getenv("SLY3_TRIG17ON") != nullptr;   // (debug: the class-0x17 triggers kept in the world)
    for (auto& [plo, t] : s_mptrig)
    {
        if (s_fOn17 && t.f17 && plo->fRemoved) AddLo(psw, t.plo);
        // (debug: SLY3_TRIG17TEST=N: every N frames each class-0x17 trigger hands every guard in the world to the reaction,
        // as if it had entered it; with SLY3_TRIGREACTFORCE the reaction's own tests are skipped)
        if (static const int s_n = std::getenv("SLY3_TRIG17TEST") ? std::atoi(std::getenv("SLY3_TRIG17TEST")) : 0; s_n > 0 && t.f17)
        {
            static int s_c = 0;
            if (++s_c % (s_n * 4) < 4)
                for (int icid = 0x3A; icid <= 0x43; ++icid)
                    for (LO* pg : psw->mpcidaplo[icid])
                        if (FLoInWorld3(pg)) OnTriggerGuardEnter3(pg, t.plo, t.k17, t.mask17, nullptr);
        }
        if (plo->oid == s_oidDbg)
        {
            const glm::vec3 p(MatLoWorld3(plo)[3]);
            std::printf("trigdbg %d (%p): state %d removed %d inworld %d parent %d at %.0f %.0f %.0f\n", plo->oid, (const void*)plo, t.state,
                        plo->fRemoved, FLoInWorld3(plo), plo->paloParent ? plo->paloParent->oid : -1, p.x, p.y, p.z);
        }
        // (a trigger in the world from its start (the world start's restore added it before any update) is reset
        // first: the start 0x15CCE8 -> 0x15D100 sets its state; natively the removal's branch below did it)
        if (FLoInWorld3(plo) && t.state == -1) SetTriggerState(t, 0);
        if (FLoInWorld3(plo)) CheckTrigger(t);
        // the deactivation of a removed trigger (RemoveLo, slot 9: 0x15D048 -> 0x15D288 / slot 31 0x15D2A8 -> 0x15BE10):
        // state 0 (the exit when it was entered)
        else if (t.state != 0) SetTriggerState(t, 0);
    }
}

// The scripts' list methods (Sly 3 VOLTRIGGER options 570..574, thunks on the component +0x120; s3-hack round 11: natively
// unbound, e.g. h_ext's 94107 (Ep3's jobs), Ep5's 51419 / 44209 had their objects added by scripts and never saw them):
//   optid 570 (EOPID 480): 0x106348 (an oid, schema 6 -> 0x15C260) / 0x1063D0 (an object, 0x2002 -> 0x15C2B8: an ALO
//   (0x13B500(o, 0x2F)) appended, at most 8); 571 (481): 0x106458 -> 0x15C340 (that object's entries removed);
//   573 (483): 0x1064E0 -> 0x15C4A8 (a class, at most 4); 574 (484): 0x106568 -> 0x15C4F8 (that class removed).
// EOPID 493 (optid 555, the component's +0x14 = +0x134): the first object inside at the last check (e.g. the
// Hackathon docks' trigger 92891: its s5 hands (geto self 555) to the dock's 2400).
namespace
{
// (only a trigger of the running world: the loader calls these thunks too (grf 0x400) while the next world loads, when
// the table still holds the old world's records, a new object possibly at a freed one's address; the loaded lists are
// read from the options by StartTriggers3)
TRIG3* PtrigOf(LO* plo)
{
    if (!s_psw || s_psw != g_psw) return nullptr;
    auto it = s_mptrig.find(plo);
    return it == s_mptrig.end() || it->second.plo != plo ? nullptr : &it->second;
}
void AddWatch3(LO* plo, LO* po)
{
    TRIG3* pt = PtrigOf(plo);
    if (!pt || !po || !FIsLoDerivedFrom(po, 0x2F)) return;
    const std::size_t c = pt->aploWatch.size() + (pt->fWatchResolved ? 0 : pt->aoidWatch.size());
    if (c >= 8) return;
    pt->aploWatch.push_back(po);
    if (s_fPrint) std::printf("trigger3: %d watches %d (%zu)\n", plo->oid, po->oid, c + 1);
}
struct INITTRIGMETHODS3
{
    INITTRIGMETHODS3()
    {
        if (std::getenv("SLY3_TRIGOLD")) return;
        RegisterObjectMethod3(0x106348, [](LO* plo, const std::vector<CRef>& a) {
            if (TRIG3* pt = PtrigOf(plo); pt && !a.empty() && a[0].m_tagk == TAGK_S32)
            {
                LO* po = PloFindSwNearest(plo, a[0].m_n);
                AddWatch3(plo, po ? po : PloFind(a[0].m_n));
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x1063D0, [](LO* plo, const std::vector<CRef>& a) {
            if (!a.empty() && a[0].m_tagk == TAGK_Basic) AddWatch3(plo, dynamic_cast<LO*>(a[0].m_pbasic));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x106458, [](LO* plo, const std::vector<CRef>& a) {
            TRIG3* pt = PtrigOf(plo);
            LO* po = !a.empty() && a[0].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
            if (!pt || !po) return CRef(TAGK_Void);
            if (!pt->fWatchResolved) AploInside(*pt, 0);   // (resolves the loaded oids first)
            auto& v = pt->aploWatch;
            v.erase(std::remove(v.begin(), v.end(), po), v.end());
            if (s_fPrint) std::printf("trigger3: %d stops watching %d\n", plo->oid, po->oid);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x1064E0, [](LO* plo, const std::vector<CRef>& a) {
            if (TRIG3* pt = PtrigOf(plo); pt && !a.empty() && a[0].m_tagk == TAGK_S32 && pt->acid.size() < 4) pt->acid.push_back(a[0].m_n);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x106568, [](LO* plo, const std::vector<CRef>& a) {
            if (TRIG3* pt = PtrigOf(plo); pt && !a.empty() && a[0].m_tagk == TAGK_S32)
                pt->acid.erase(std::remove(pt->acid.begin(), pt->acid.end(), a[0].m_n), pt->acid.end());
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter3(493, [](LO* plo) {
            CRef r;
            const TRIG3* pt = PtrigOf(plo);
            r.SetBasic(pt ? pt->ploFirst : nullptr);
            return r;
        });
    }
} s_inittrigmethods3;
}  // namespace

}  // namespace sly3

// orig3 0x15D6A0 (EOPID 479, optid 586): the sorted objects inside (+0x23C / +0x240) as a list. (debug: on only with
// SLY3_TRIGLIST=1 until v_gauntlet's SCPT 2323 loop (while guards stay inside trigger 139787) can end natively)
#include "../objmethod.h"
#include "../pair.h"
#include "../ref.h"
namespace
{
struct INITTRIGLIST3
{
    INITTRIGLIST3()
    {
        if (!std::getenv("SLY3_TRIGLIST")) return;
        RegisterObjectGetter3(479, [](LO* plo) {
            CRef r;
            r.SetPair(nullptr);
            const std::vector<LO*>* pa = sly3::PaploTriggerInside3(plo);
            if (!pa) return r;
            std::shared_ptr<CPair> head, tail;
            for (LO* p : *pa)
            {
                auto q = PpairNew();
                q->m_ref.SetBasic(p);
                if (tail) tail->m_ppairNext = q;
                else head = q;
                tail = q;
            }
            r.SetPair(head);
            return r;
        });
    }
} s_inittriglist3;
}  // namespace
