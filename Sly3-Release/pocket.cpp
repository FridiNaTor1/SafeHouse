// pocket.cpp: the prize givers' pocket and Sly's pickpocket (pocket.h). Ported from the decomp's readable C
// (sly2-decomp/src/hand func_001F17B0 / 001F1768 / 001F17F8 / 001F1900 / 001F1CF8 / 001F1E20 / 0018B348) and the
// asm (0x18B460, 0x188F20's event 0x167).
#include "pocket.h"
#include "game/sly3.h"
#include "worldglobals.h"

#include "actor.h"
#include "animfind.h"
#include "aseg.h"
#include "brx.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "guard.h"
#include "emitter.h"
#include "interact.h"
#include "jt.h"
#include "objmethod.h"
#include "activate.h"
#include "pickup.h"
#include "render.h"
#include "so.h"
#include "vecmat.h"
#include "xfphys.h"
#include <cfloat>
int NRandInRange(int nLow, int nHigh);  // util.h (0x11D3F8)
float GRandInRange(float gLow, float gHigh);  // xfmath.h (0x11D448)
glm::mat3 MatEuler(const glm::vec3& e);         // xfmath.h (0x11B628)
glm::vec3 EulerFromMat(const glm::mat3& m);     // xfmath.h (0x11BFF0)
glm::mat4 InverseAffineMatrix4(const glm::mat4& m);  // xfmath.h (0x11C318)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)
#include "po.h"
#include "sly.h"
#include "sm.h"
#include "pair.h"
#include "smrun.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>

namespace
{
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "pocket: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}
std::unordered_map<LO*, POCKET> s_mppocket;
WORLD_TABLE(s_mppocket);   // (PC: moved aside while a kept world loads, worldglobals.h)
// The pocket object's reward (+0x150 the launch point (a record of kind 0xD), +0x154 the proxy, +0x158 the instance)
struct REWARD
{
    XF* pxf = nullptr;
    LO* pproxy = nullptr;
    LO* plo = nullptr;
};
std::unordered_map<LO*, REWARD> s_mpreward;
WORLD_TABLE(s_mpreward);   // (PC: moved aside while a kept world loads, worldglobals.h)
// The pocket object's state machine +0x16C (the glow: its SM part 0x2B2 at +0x168, made by its post-load)
std::unordered_map<LO*, SMA*> s_mppsma;
WORLD_TABLE(s_mppsma);   // (PC: moved aside while a kept world loads, worldglobals.h)
bool FPrint() { static const bool s_f = std::getenv("SLY2N_POCKETPRINT") != nullptr; return s_f; }
}  // namespace

POCKET& PocketOf(LO* plo) { return s_mppocket[plo]; }
void ResetPockets()
{
    s_mppocket.clear();
    s_mpreward.clear();
    s_mppsma.clear();
    ResetPickups();
}

LO* PloPocket(LO* plo)
{
    if (GUARD* pg = plo ? PguardFromLo(plo) : nullptr) return pg->plo494;
    return nullptr;  // TODO(actors): the other givers' +0x494
}

// The giver's +0x490 (EOPID 307, grf 0x92: a field the loader / the splice store, LO::SetField): more prizes allowed
// and the giver's start fill skipped. E.g. Ep6's Carmelita (12712) has it set in the world data, so the job's three
// jail keys (56669 / 56667 / 56665, EOPID 309 at 2896's start) stay in her pocket (the reference: count 3, +0x490 1)
bool FPocketMore(LO* plo) { return PocketOf(plo).fMore || plo->U32Field(0, 0x490, 0) != 0; }

// orig: 0x001F17B0 (a prize: the first, or more when +0x490 allows)
void AddPocketPrize(LO* plo, int oid)
{
    POCKET& p = PocketOf(plo);
    const bool fMore = FPocketMore(plo);
    if (FPrint()) std::printf("pocket %d (%p): prize %d added (count %d, more %d)\n", plo->oid, static_cast<void*>(plo), oid, p.c, fMore);
    if (!(p.c <= 0 || fMore) || p.c >= 8) return;
    POCKETENT& e = p.ae[p.c++];
    e.oid = oid;
    e.n4 = 0;
    e.n8 = -1;
    e.c = 1;
    if (FPrint())
    {
        std::printf("pocket contents %d:", plo->oid);
        for (int i = 0; i < p.c; ++i) std::printf(" %d", p.ae[i].oid);
        std::printf("\n");
    }
}

// orig: 0x001F1768 (c coins, the prize 0x2B3, when +0x490 allows)
void AddPocketCoins(LO* plo, int c)
{
    POCKET& p = PocketOf(plo);
    if (!FPocketMore(plo) || p.c >= 8) return;
    POCKETENT& e = p.ae[p.c++];
    e.oid = 0x2B3;
    e.n8 = -1;
    e.n4 = 0;
    e.c = c;
}

// orig: 0x001C2D20 (the pocket object's post-load, class 0x63: the ALO's 0x1F36A8, its parts 0x1A64E8 (table 0x2C0CB0:
// 0x2B2 the SM +0x168, 0x2B3 / 0x2B4 +0x15C / +0x160, 0x2B5 +0x164), then 0x1F1448(+0x168, this, -1, 0, &+0x16C)).
// Natively made at the first use (a proxied guard's copy has its own pocket object, which the load never posts):
// the SM is the object's own part 0x2B2 (the template's for a copy).
SMA** PpsmaPocket(LO* ppocket)
{
    if (!ppocket || !FIsLoDerivedFrom(ppocket, S3CID(0x30))) return nullptr;
    auto it = s_mppsma.find(ppocket);
    if (it != s_mppsma.end()) return &it->second;
    SMA*& psma = s_mppsma[ppocket];
    SM* psm = nullptr;
    for (LO* p = ppocket; p && !psm; p = p->ploCloneOf)
        for (LO* pch : p->aploOwned)
            if (pch->oid == 0x2B2)
                if ((psm = dynamic_cast<SM*>(pch))) break;
    if (psm) ReplaceSmaHandle(psm, static_cast<ALO*>(ppocket), -1, 0, &psma);  // 0x1F1448
    else WarnOnce("the pocket object's SM 0x2B2 (not found)");
    if (FPrint()) std::printf("pocket object %d %p: sm %p sma %p\n", ppocket->oid, static_cast<void*>(ppocket), static_cast<void*>(psm), static_cast<void*>(psma));
    if (FPrint() && psm)
    {
        for (const SM::SMS& st : psm->asms) std::printf("  sms %d next %d\n", st.oid, st.inext);
        for (const SM::SMT& t : psm->asmt)
        {
            std::printf("  smt %d -> %d grf 0x%X aseg %d:", t.isFrom, t.isTo, t.grf, t.paseg ? t.paseg->oid : -1);
            const ASEG* pa = t.paseg;
            while (pa && pa->ploCloneOf && pa->asegd.achn.empty() && pa->asegd.achg.empty()) pa = static_cast<const ASEG*>(pa->ploCloneOf);
            if (pa)
            {
                std::printf(" (data %d tMax %.2f)", pa->oid, pa->asegd.tMax);
                for (const ASEGCHN& c : pa->asegd.achn) std::printf(" chn %d/%d%s", c.oid, c.isub, c.fScalar ? "s" : "");
                for (const ASEGCHG& c : pa->asegd.achg) std::printf(" chg k%d n%d", c.kind, c.n);
                for (const auto& aev : pa->asegd.aaev)
                    for (const ASEGEV& e : aev) std::printf(" ev k%d %d %d", e.kind, e.an[0], e.an[1]);
            }
            std::printf("\n");
        }
        for (LO* pch : ppocket->aploOwned)
        {
            std::printf("  child %d cid 0x%X", pch->oid, pch->Cid());
            for (LO* q = pch; q; q = q->ploCloneOf)
                if (const LOXEMIT* pe = dynamic_cast<const LOXEMIT*>(q); pe && pe->nColors)
                {
                    for (uint32_t u : pe->argba) std::printf(" rgba %08X", u);
                    break;
                }
            std::printf("\n");
        }
    }
    return &psma;
}

// orig: 0x001F17F8 (the pocket's glow: -1 / 0 none, 1 coins, 2 a prize; the volumes +0x3F4 / +0x3F8 vt slot 9 / 8,
// the pocket object's state machine +0x16C to D_002C3738[k + 1] = 92 / 101 / 102 / 105)
void SetPocketMode(LO* plo, int k)
{
    POCKET& p = PocketOf(plo);
    if (k == p.nMode) return;
    p.nMode = k;
    p.tMode = g_clock.t;
    // the pickpocket volumes +0x3F4 / +0x3F8 (the guard's options 304 / 305): out (slot 9) with -1 / 0, in (slot 8)
    // with 1 / 2 (in them Sly ranks the guard 3: target.cpp)
    if (GUARD* pg = PguardFromLo(plo))
        for (LO* pv : {pg->plo3F4, pg->plo3F8})
        {
            if (!pv) continue;
            if (k == -1 || k == 0) RemoveLo(g_psw, pv);
            else if (k == 1 || k == 2) AddLo(g_psw, pv);
        }
    // the pocket object's state machine +0x16C to D_002C3738[k + 1] = 92 / 101 / 102 / 105 (the glow)
    SMA* psma = nullptr;
    if (LO* ppocket = PloPocket(plo))
        if (SMA** ppsma = PpsmaPocket(ppocket); ppsma && *ppsma)
        {
            psma = *ppsma;
            static const OID s_aoid[] = {92, 101, 102, 105};
            SeekSma(psma, s_aoid[k + 1]);  // 0x1F0730
        }
    if (FPrint())
        std::printf("pocket %d: mode %d t %.3f sma %p goal %d\n", plo->oid, k, g_clock.t, static_cast<void*>(psma), psma ? OidSmaGoal(psma) : -1);
}

// orig: 0x001F1900 (the mode by the giver's state: -1 without a readied reward (the pocket's +0x158), 0 while
// alerted (states 0x1D / 0x20 / 5 / 6 / 7 / 0x17 / 0x16), 1 for coins or none or another player, 2 a prize for Sly)
void UpdatePocketMode(LO* plo)
{
    LO* ppocket = PloPocket(plo);
    if (!ppocket) return;
    POCKET& p = PocketOf(plo);
    if (!s_mpreward[ppocket].plo)
    {
        SetPocketMode(plo, -1);
        return;
    }
    int k = 1;
    if (GUARD* pg = PguardFromLo(plo))
    {
        uint64_t m = 0;
        for (int n : {0x1D, 0x20, 5, 6, 7, 0x17, 0x16}) m |= GrfStateBit(n);  // 0x207A20
        if (pg->grfState & m) k = 0;
    }
    if (k != 0)
    {
        LO* pcur = g_psw ? PloPoCurrent(g_psw) : nullptr;  // 0x1C4840
        const bool fSly = pcur && FIsLoDerivedFrom(pcur, S3CID(0x35));  // (D_002E1E40: Sly)
        k = (!p.c || p.ae[p.c - 1].oid == 0x2B3 || !pcur || !fSly) ? 1 : 2;
    }
    SetPocketMode(plo, k);
}

namespace
{
// The pockets' contents by episode (0x2C3748: the givers; 0x2C3968: class 0x3A's): coins in [nLow, nHigh] in 2 or 3
// entries, a prize (0x2B5, its kind +0x8 by the weights) with the chance gPrize
struct POCKETTBL
{
    int nLow, nHigh;
    float gPrize;
    int aw[6][2];  // {weight, the prize's kind}
};
#define POCKET_ROWS(a, b, c, d, e, f) {{30, a}, {30, b}, {15, c}, {15, d}, {5, e}, {5, f}}
const POCKETTBL s_atblPocket[9] = {
    {0, 0, 0.0f, {{0, -1}}},
    {4, 8, 0.2f, POCKET_ROWS(1114, 1115, 1120, 1121, 1117, 1118)},
    {4, 8, 0.2f, POCKET_ROWS(1117, 1118, 1123, 1124, 1126, 1127)},
    {4, 8, 0.2f, POCKET_ROWS(1117, 1118, 1123, 1124, 1126, 1127)},
    {4, 8, 0.2f, POCKET_ROWS(1126, 1127, 1129, 1130, 1135, 1136)},
    {4, 8, 0.2f, POCKET_ROWS(1126, 1127, 1129, 1130, 1135, 1136)},
    {4, 8, 0.2f, POCKET_ROWS(1135, 1136, 1132, 1133, 1141, 1142)},
    {4, 8, 0.2f, POCKET_ROWS(1135, 1136, 1132, 1133, 1141, 1142)},
    {4, 8, 0.2f, POCKET_ROWS(1132, 1133, 1141, 1142, 1138, 1139)},
};
const POCKETTBL s_atblPocket3A[9] = {
    {0, 0, 0.0f, {{0, -1}}},
    {6, 12, 0.4f, POCKET_ROWS(1116, 1115, 1122, 1121, 1119, 1118)},
    {6, 12, 0.4f, POCKET_ROWS(1119, 1118, 1125, 1124, 1128, 1127)},
    {6, 12, 0.4f, POCKET_ROWS(1119, 1118, 1125, 1124, 1128, 1127)},
    {6, 12, 0.4f, POCKET_ROWS(1128, 1127, 1131, 1130, 1137, 1136)},
    {6, 12, 0.4f, POCKET_ROWS(1128, 1127, 1131, 1130, 1137, 1136)},
    {6, 12, 0.4f, POCKET_ROWS(1137, 1136, 1134, 1133, 1143, 1142)},
    {6, 12, 0.4f, POCKET_ROWS(1137, 1136, 1134, 1133, 1143, 1142)},
    {6, 12, 0.4f, POCKET_ROWS(1134, 1133, 1143, 1142, 1140, 1139)},
};
#undef POCKET_ROWS
}  // namespace

// orig: 0x001F1A40 (the giver's start, unless +0x490: the pocket object's reward cleared (0x1C2DB8); a prize set before
// (a carryable 0x5C whose task is done is dropped) is kept, else one is drawn (0x2B5 with the chance, its kind by the
// weights; -1 none); then the coins (NRandInRange(nLow, nHigh) over 2..3 entries), the prize last)
void FillPocket(LO* plo)
{
    POCKET& p = PocketOf(plo);
    LO* ppocket = PloPocket(plo);
    if (ppocket) ClearReward(ppocket);  // 0x1C2DB8
    if (FPocketMore(plo)) return;
    int iep = IepisodeCur();  // 0x210AE0
    if (iep == -1) iep = 1;
    if (iep < 0 || iep > 8) iep = 1;
    const POCKETTBL& tbl = FIsLoDerivedFrom(plo, S3CID(0x3A)) ? s_atblPocket3A[iep] : s_atblPocket[iep];
    POCKETENT e{0, 0, 0, 0};  // (the memset)
    if (p.c > 0)
    {
        LO* po = ppocket ? PloFindSwNearest(ppocket, p.ae[0].oid) : PloFindSwAny(p.ae[0].oid);  // 0x15E938 (4)
        // a carryable prize (0x5C) whose task (+0x230) is done is dropped
        if (po && FIsLoDerivedFrom(po, S3CID(0x5C)))
        {
            const OID oidTask = static_cast<OID>(po->U32Field(0, 0x230, static_cast<uint32_t>(-1)));
            bool fDone = false;
            const int nState = oidTask != static_cast<OID>(-1) ? NTaskStateOid(oidTask, &fDone) : -2;
            if (FPrint()) std::printf("pocket %d: prize %d's task %d state %d done %d\n", plo->oid, p.ae[0].oid, oidTask, nState, int(fDone));
            if (oidTask != static_cast<OID>(-1) && nState != -2 && fDone) p.c = 0;
        }
    }
    if (p.c > 0)
    {
        e = p.ae[0];
        p.c = 0;
    }
    else
    {
        if (tbl.gPrize < GRandInRange(0.0f, 1.0f))  // 0x11D448
        {
            e.oid = -1;
            e.n8 = -1;
        }
        else
        {
            int cw = 0;
            for (const auto& w : tbl.aw) cw += w[0];
            int n = NRandInRange(0, cw - 1);  // 0x11D3F8
            for (const auto& w : tbl.aw)
            {
                if (n < w[0]) { e.n8 = w[1]; break; }
                n -= w[0];
            }
            e.c = 1;
            e.oid = 0x2B5;
        }
        p.c = 0;
    }
    int cCoin = NRandInRange(tbl.nLow, tbl.nHigh);
    const int cEnt = NRandInRange(2, 3);
    for (int i = 0; i < cEnt && p.c < 8; i++)
    {
        const int k = cCoin / (cEnt - i);
        POCKETENT& pe = p.ae[p.c++];
        pe.oid = 0x2B3;
        pe.n4 = 0;
        const int c = 0 < k ? k : 1;
        cCoin -= c;
        pe.c = c;
        if (cCoin == 0) break;
    }
    if (e.c > 0 && p.c < 8) p.ae[p.c++] = e;
    if (FPrint())
    {
        const glm::vec3 pos = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(plo)) : glm::vec3(0.0f);
        std::printf("pocket %d (%p, copy of %p): filled (episode %d) at (%.0f %.0f %.0f) t %.3f", plo->oid, static_cast<void*>(plo), static_cast<void*>(plo->ploCloneOf), iep, pos.x, pos.y, pos.z, g_clock.t);
        for (int i = 0; i < p.c; i++) std::printf(" {%d %d %d x%d}", p.ae[i].oid, p.ae[i].n4, p.ae[i].n8, p.ae[i].c);
        std::printf("\n");
    }
}

// orig: 0x001F1CF8 (the next prize whose object is there (0x15E938 kind 4 from the pocket): ready (+0x48C) when it is
// a carryable (0x5C) whose +0x230's +0x54 is -1 / 0, or 0x1C2DC8 (the reward's instance, 0x2B0 / 0x2B1) answers 1)
void NextPocketPrize(LO* plo)
{
    POCKET& p = PocketOf(plo);
    LO* ppocket = PloPocket(plo);
    p.fReady = 0;
    for (; p.i < p.c; p.i++)
    {
        const POCKETENT& e = p.ae[p.i];
        LO* po = ppocket ? PloFindSwNearest(ppocket, e.oid) : PloFindSwAny(e.oid);  // 0x15E938 (4)
        if (!po) continue;
        if (FPrint()) std::printf("pocket %d: next %d prize %d -> oid %d cid 0x%X (%s)\n", plo->oid, p.i, e.oid, po->oid, po->Cid(),
                                  g_acls[po->Cid()].name);
        int oidPoint = 0x2B0;
        if (FIsLoDerivedFrom(po, S3CID(0x5C)))
        {
            // (+0x230's task +0x54: -1 / 0 ready at once, 1 the point 0x2B1, else the next entry)
            const OID oidTask = static_cast<OID>(po->U32Field(0, 0x230, static_cast<uint32_t>(-1)));
            bool fDone = false;
            const int n = oidTask != static_cast<OID>(-1) ? NTaskStateOid(oidTask, &fDone) : -2;
            if (n != -2)
            {
                if (n == -1 || n == 0)
                {
                    p.fReady = 1;
                    break;
                }
                if (n != 1) continue;
            }
            oidPoint = 0x2B1;
        }
        if (NReadyReward(ppocket, po, e.n8, oidPoint) == 1) p.fReady = 1;  // 0x1C2DC8
        break;
    }
    UpdatePocketMode(plo);
}

// orig: 0x001C2DB8
void ClearReward(LO* ppocket) { s_mpreward[ppocket] = REWARD{}; }
// the pocket object's launch point record (+0x150; null: none readied)
XF* PxfRewardPoint(LO* ppocket)
{
    auto it = s_mpreward.find(ppocket);
    return it == s_mpreward.end() ? nullptr : it->second.pxf;
}

// orig: 0x001C2D70: after the pocket's ordinary ALO draw, draw its readied
// reward through 0x1A5EE8 at +0x150's world matrix, with both alphas 1.
// The reward's pickup state 2 deliberately keeps it out of the ordinary draw pass.
void CollectPocketDraws(const SCENE& scene, std::vector<EXTRADRAW>& draws)
{
    if (!g_psw) return;
    static const int oidOnly = std::getenv("SLY2_ONLYOID") ? std::atoi(std::getenv("SLY2_ONLYOID")) : -2;
    static int prints = std::getenv("SLY2N_POCKETDRAWPRINT") ? 64 : 0;
    // RenderSw walks the world's update table; keep its submission order.
    for (const UPDENT& entry : g_psw->aupd)
    {
        const auto found = s_mpreward.find(entry.plo);
        if (found == s_mpreward.end()) continue;
        ALO* pocket = dynamic_cast<ALO*>(entry.plo);
        const REWARD& reward = found->second;
        ALO* model = dynamic_cast<ALO*>(reward.plo);
        if (!pocket || !model || !reward.pxf || !FAloDrawn(g_psw, pocket)) continue;
        if (oidOnly != -2 && pocket->oid != oidOnly) continue;
        UpdateXfWorlds(pocket);
        const size_t first = draws.size();
        AppendAloExtraDraws(scene, model, reward.pxf->matWorld, 1.0f, draws);
        if (prints > 0)
        {
            --prints;
            const glm::vec3 pos = reward.pxf->matWorld[3];
            std::printf("pocketdraw owner %d reward %d groups %zu at %.9g %.9g %.9g\n",
                        pocket->oid, model->oid, draws.size() - first, pos.x, pos.y, pos.z);
        }
    }
}

// orig: 0x001C2DC8 (the reward readied in the pocket object: a proxy's instance attached (1 when there is none) or the
// object itself (an ALO; 2 otherwise); a pickup is put away (state 2), other objects removed; the launch point is the
// pocket's record oidPoint of kind 0xD; 3; 0 when one is readied already)
int NReadyReward(LO* ppocket, LO* plo, int id, int oidPoint)
{
    if (!ppocket) return 2;
    REWARD& r = s_mpreward[ppocket];
    if (r.plo) return 0;
    r.pproxy = nullptr;
    if (FIsLoDerivedFrom(plo, S3CID(6)))
    {
        r.pproxy = plo;
        r.plo = PloAttachProxy(g_psw, plo, 0);  // 0x1CB250
        if (!r.plo)
        {
            r.pproxy = nullptr;
            return 1;
        }
    }
    else if (FIsLoDerivedFrom(plo, S3CID(0x30)))
        r.plo = plo;
    else
        return 2;
    if (FIsLoDerivedFrom(r.plo, S3CID(0x5E))) SetPickupLootKind(r.plo, id);  // 0x1C7A58
    if (FIsLoDerivedFrom(r.plo, S3CID(0x5A))) SetPickupState(r.plo, 2);  // vt slot 76
    else RemoveLo(g_psw, r.plo);  // vt slot 9
    r.pxf = nullptr;
    if (FCidDerivedFrom(ppocket->Cid(), S3CID(0x30)))
        for (const auto& pxf : static_cast<ALO*>(ppocket)->axf)  // 0x1F4C58
            if (pxf && pxf->tag == oidPoint && pxf->kind == 0xD) { r.pxf = pxf.get(); break; }
    if (FPrint()) std::printf("reward %d: readied %d (cid 0x%X) proxy %d point %s\n", ppocket->oid, r.plo->oid, r.plo->Cid(),
                              r.pproxy ? r.pproxy->oid : -1, r.pxf ? "yes" : "no");
    return 3;
}

// orig: 0x001C2EF0 (the reward launched c times from the launch point toward the current player: each to a point
// around him (SetVectorCylind(GRandInRange(-1.5, 1.5), GRandInRange(100, 150), -75) in his frame facing from the
// source) in GRandInRange(0.75, 1.2) under the gravity (0x1D69F0); pickups by 0x1C6BD0 (the SM's delay i 0.05, coins
// collected after i 0.15), other objects by slots 8 / 39 / 35; the more copies from the proxy (0x1CB250, put away);
// the first returned; the reward cleared)
LO* PloLaunchReward(LO* ppocket, int c)
{
    if (!ppocket) return nullptr;
    REWARD& r = s_mpreward[ppocket];
    LO* plo = nullptr;
    LO* ploFirst = nullptr;
    if (FPrint()) std::printf("reward %d: launch %d of %d (proxy %d)\n", ppocket->oid, c, r.plo ? r.plo->oid : -1, r.pproxy ? r.pproxy->oid : -1);
    for (int i = 0; i < c; )
    {
        if (i == 0) plo = r.plo;
        else
        {
            plo = r.pproxy ? PloAttachProxy(g_psw, r.pproxy, 0) : nullptr;
            if (plo && FIsLoDerivedFrom(plo, S3CID(0x5A))) SetPickupState(plo, 2);
        }
        if (!plo) break;
        if (!ploFirst) ploFirst = plo;
        const glm::vec3 pos0 = r.pxf ? glm::vec3(r.pxf->matWorld[3]) : PosAloCenter(static_cast<ALO*>(ppocket));
        LO* pcur = PloPoCurrent(g_psw);  // 0x1C4840
        const glm::vec3 posCur = pcur && FCidDerivedFrom(pcur->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(pcur)) : pos0;
        const float rad = GRandInRange(-1.5f, 1.5f);   // D_002C0CF0
        const float s = GRandInRange(100.0f, 150.0f);  // D_002C0CF8
        const glm::vec3 off = cmm::SetVectorCylind(rad, s, -75.0f);  // 0x11E1A0
        glm::vec3 d = posCur - pos0;
        d -= glm::vec3(0.0f, 0.0f, 1.0f) * d.z;
        const float l = glm::length(d);
        const glm::vec3 dir = l < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : d / l;
        const glm::mat3 m = BuildLookAtMatrix(dir, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B830
        const glm::vec3 posGoal = posCur + m * off;
        const float dt = GRandInRange(0.75f, 1.2f);  // D_002C0CE0
        // 0x1D69F0 (from pos0 to posGoal in dt under D_002C0D60)
        const glm::vec3 g(0.0f, 0.0f, -2250.0f);
        const glm::vec3 delta = posGoal - pos0;
        const float z = delta.z - (dt * (dt * 0.5f)) * g.z;
        const glm::vec3 flat = delta - glm::vec3(0.0f, 0.0f, 1.0f) * delta.z;
        const glm::vec3 vel = flat * (1.0f / dt) + glm::vec3(0.0f, 0.0f, 1.0f) * (z / dt);
        if (FIsLoDerivedFrom(plo, S3CID(0x5A)))
        {
            const bool fCoin = FIsLoDerivedFrom(plo, S3CID(0x5B)) || FIsLoDerivedFrom(plo, S3CID(0x5D));
            const float fi = static_cast<float>(i);
            LaunchPickup(plo, pos0, vel, fi * 0.05f, fCoin ? fi * 0.15f : FLT_MAX, r.pproxy);  // 0x1C6BD0
        }
        else
        {
            AddLo(g_psw, plo);  // vt slot 8
            WarnOnce("0x1C2EF0's launch of an object that is not a pickup (vt slots 39 / 35)");
        }
        i++;
    }
    ClearReward(ppocket);  // 0x1C2DB8
    return ploFirst;
}

// orig: 0x001F1E20 (the take: the task event 0x19, the reward 0x1C2EF0 at the pocket with the entry's count, the
// glow off, the next prize)
LO* PloTakePocket(LO* plo)
{
    POCKET& p = PocketOf(plo);
    if (p.i >= p.c) return nullptr;
    BroadcastTaskEvent(g_psw, 0x19, plo);  // 0x1619A0
    LO* r = PloLaunchReward(PloPocket(plo), p.ae[p.i].c);  // 0x1C2EF0
    if (FPrint()) std::printf("pocket %d: take %d (prize %d x %d) t %.3f\n", plo->oid, p.i, p.ae[p.i].oid, p.ae[p.i].c, g_clock.t);
    SetPocketMode(plo, -1);
    p.i++;
    NextPocketPrize(plo);
    return r;
}

// orig: 0x0018B348 (Sly's load: the clip +0x1BC8's event 0x167 time +0x1BD8; at it, in Sly's object space (+0x60), the
// cane's point (+0x19A4's object record: +0x1BE0), the arm's joint (+0x19CC: +0x1BF0), the hand's (+0x19C0: +0x1C00,
// its rotation +0x1C10) and the cane's point in the hand's frame (+0x1C20), for the reach test 0x18B460)
void LoadPickpocket(SLY* p, ASEG* paseg)
{
    if (!paseg) return;
    p->g1BD8 = GEventAseg11(paseg, 0x167);  // 0x12F990
    ALO* palo = p->palo;
    XF* pxfTo = palo ? PxfObj(palo) : nullptr;
    ALO* pcane = p->plo19A4 && FIsLoDerivedFrom(p->plo19A4, S3CID(0x30)) ? static_cast<ALO*>(p->plo19A4) : nullptr;
    if (!palo || !pxfTo || !pcane || !PxfObj(pcane) || !p->pxfArm || !p->pxfHandRot) return;
    glm::mat4 m0(1.0f), m1(1.0f), m2(1.0f);
    CalculateAsegMatrices(paseg, palo, PxfObj(pcane), pxfTo, p->g1BD8, 0.0f, &m0, nullptr);  // 0x1300B8
    CalculateAsegMatrices(paseg, palo, p->pxfArm, pxfTo, p->g1BD8, 0.0f, &m1, nullptr);
    CalculateAsegMatrices(paseg, palo, p->pxfHandRot, pxfTo, p->g1BD8, 0.0f, &m2, nullptr);
    p->posCane1BE0 = glm::vec3(m0[3]);
    p->posArm1BF0 = glm::vec3(m1[3]);
    p->posHand1C00 = glm::vec3(m2[3]);
    p->eulHand1C10 = EulerFromMat(glm::mat3(m2));                                           // 0x11BFF0
    p->posCaneHand1C20 = glm::vec3(InverseAffineMatrix4(m2) * glm::vec4(p->posCane1BE0, 1.0f));  // 0x11C318
}

// orig: 0x0018B460 (state 11: before the take (the clip's time short of +0x1BD8) Sly faces the target's pocket
// (0x2046F0) and the arm reaches for the pocket's point (0, 0, 25) in its launch record (+0x150) where it will be in
// min(the time left, 0.15 s) (0x2309E8), in Sly's space: the hand turned about its y by atan2(-z, 0.75 x) (within
// +-1), then slid along the x of the hand's parent space (+0x19A0's position record's reference) by up to -50..35; what
// is left over moves the hand, and only while that hand stays within 120 of the arm's joint does it reach (+0x1BD4 = 1,
// its joint controllers +0x19F8 / +0x19EC / +0x19FC set, +0x1BDC the time). Out of reach, or after the take's time, it
// does not (+0x1BD4 = 0; the controllers relax over 0.25 s): the take at the event 0x167 then gets nothing (R12).
void UpdatePickpocketPose(SLY* p)
{
    ASEGA* pasega = p->pasega;
    const float t = pasega ? pasega->t : 0.0f;
    const float svt = pasega ? pasega->svt : 1.0f;
    const float dtLeft = (p->g1BD8 - t) / svt;
    LO* ppocket = p->plo10DC ? PloPocket(p->plo10DC) : nullptr;
    ALO* palo = p->palo;
    XF* pxfSpace = palo ? PxfObj(palo) : nullptr;
    if (ppocket && 0.0f < dtLeft && pxfSpace)
    {
        FaceActorToward(p, ppocket);  // 0x2046F0
        XF* pxfPoint = PxfRewardPoint(ppocket);  // the pocket's +0x150
        if (!pxfPoint && FIsLoDerivedFrom(ppocket, S3CID(0x30))) pxfPoint = PxfObj(static_cast<ALO*>(ppocket));
        glm::mat4 mPred(1.0f);
        if (pxfPoint) PredictXfChain(pxfPoint, nullptr, std::min(dtLeft, 0.15f), &mPred, nullptr);  // 0x2309E8 (D_002BF380)
        const glm::vec3 posWorld = glm::vec3(mPred * glm::vec4(0.0f, 0.0f, 25.0f, 1.0f));             // D_002BF390
        const glm::vec3 posMapped = PosConvertXfs(nullptr, pxfSpace, posWorld);                     // 0x22FC48
        const float radYaw = std::clamp(std::atan2(-posMapped.z, 0.75f * posMapped.x), -1.0f, 1.0f);  // D_002BF3A0 / 3A8
        glm::mat4 m(MatEuler(p->eulHand1C10 + glm::vec3(0.0f, radYaw, 0.0f)));                     // 0x11B628
        m[3] = glm::vec4(p->posHand1C00, 1.0f);
        const glm::vec3 vecRel = posMapped - glm::vec3(m * glm::vec4(p->posCaneHand1C20, 1.0f));
        ALO* pfocus = p->plo19A0 && FIsLoDerivedFrom(p->plo19A0, S3CID(0x30)) ? static_cast<ALO*>(p->plo19A0) : nullptr;
        XF* pxfFocus = pfocus && PxfPos(pfocus) ? PxfPos(pfocus)->pxfRef : nullptr;  // +0x19A0's +0x58's +0xA0
        const float dx = std::clamp(VecConvertXfs(pxfSpace, pxfFocus, vecRel).x, -50.0f, 35.0f);  // 0x22FCC8, D_002BF3B0
        const glm::vec3 vecResidual = vecRel - VecConvertXfs(pxfFocus, pxfSpace, glm::vec3(dx, 0.0f, 0.0f));
        const float d = glm::length(p->posHand1C00 + vecResidual - p->posArm1BF0);
        if (FPrint())
        {
            const glm::vec3 o = glm::vec3(m * glm::vec4(p->posCaneHand1C20, 1.0f)), w = glm::vec3(pxfSpace->matWorld[3]);
            std::printf("pickpocket: t %.3f left %.3f reach %.1f (limit 120) yaw %.3f dx %.1f mapped %.0f %.0f %.0f origin %.0f %.0f %.0f "
                        "sly %.0f %.0f %.0f pocket %.0f %.0f %.0f point %d hand %.0f %.0f %.0f arm %.0f %.0f %.0f cane %.0f %.0f %.0f\n",
                        g_clock.t, dtLeft, d, radYaw, dx, posMapped.x, posMapped.y, posMapped.z, o.x, o.y, o.z, w.x, w.y, w.z, posWorld.x,
                        posWorld.y, posWorld.z, PxfRewardPoint(ppocket) != nullptr, p->posHand1C00.x, p->posHand1C00.y, p->posHand1C00.z,
                        p->posArm1BF0.x, p->posArm1BF0.y, p->posArm1BF0.z, p->posCane1BE0.x, p->posCane1BE0.y, p->posCane1BE0.z);
        }
        if (d < 120.0f)  // D_002BF3B8
        {
            WarnOnce("0x18B460's arm joint controllers (0x12DA30)");
            p->f1BD4 = 1;
            p->t1BDC = g_clock.t;
            return;
        }
    }
    p->f1BD4 = 0;
}

// The clip's event 0x167 (0x188F20): with a target and the hand set, the take (0x1F1E20); a prize that is not a coin
// (0x5B / 0x5D) is held up: +0x1BCC, 0x1424E0 (the interaction kind 12 on it at Sly's object 0x282)
void PickpocketTake(SLY* p)
{
    if (!p->plo10DC || !p->f1BD4) return;
    LO* r = PloTakePocket(p->plo10DC);
    if (!r || FIsLoDerivedFrom(r, S3CID(0x5B)) || FIsLoDerivedFrom(r, S3CID(0x5D))) return;
    ASEG* paseg = dynamic_cast<ASEG*>(PloFindSwChild(p->palo, 0x282));  // 0x15E938(G, 1, 0x282, Sly)
    p->plo1BCC = r;
    StartPrizeInteract(p, paseg, r, -1);  // 0x1424E0
}

namespace
{
// The splice methods of the givers (the thunks 0x103C18 (coins: an int), 0x103CA0 / 0x103D28 (a prize))
struct INITPOCKETMETHODS
{
    INITPOCKETMETHODS()
    {
        RegisterObjectMethod(0x10ABF8, [](LO* plo, const std::vector<CRef>& a) {  // orig: 0x1421A0
            if (ACTOR* pa = PactorFromLo(plo))
            {
                SetActorInteractObject(pa, 5, nullptr);  // 0x142140(p, 0)
                if (LO* held = PloArg(a, 0)) AttachReachObject(&pa->inter.reach, held);
                else ReleaseReachObject(&pa->inter.reach);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter(773, [](LO* plo) {  // Sly option 756, +0x1BCC
            CRef r;
            if (SLY* p = PslyFromLo(plo)) r.SetBasic(p->plo1BCC);
            return r;
        });
        // EOPID 740 (an actor's option 726, field +0x9A0 = the interaction's reach +0x440 +0xA0): the object held in
        // the hand (0x1CD098). The prize title card's script (Sly's module: events 5332 / 5334) turns option 178
        // (SetSoIgnoreLocked, EOPID 69) on and off for Sly, his parts 316 / 317 and this object, so the held prize
        // draws over the card with him (R15: natively it read nothing and the prize stayed behind the card).
        RegisterObjectGetter(740, [](LO* plo) {
            CRef r;
            if (ACTOR* pa = FCidDerivedFrom(plo->Cid(), S3CID(0x34)) ? PactorFromLo(plo) : nullptr) r.SetBasic(pa->inter.reach.ploHeld);
            return r;
        });
        RegisterObjectMethod(0x103C18, [](LO* plo, const std::vector<CRef>& a) {
            AddPocketCoins(plo, NArg(a, 0));
            return CRef(TAGK_Void);
        });
        for (uint32_t pfn : {0x103CA0u, 0x103D28u})
            RegisterObjectMethod(pfn, [](LO* plo, const std::vector<CRef>& a) {
                LO* po = PloArg(a, 0);
                AddPocketPrize(plo, po ? static_cast<int>(po->oid) : NArg(a, 0));
                return CRef(TAGK_Void);
            });
        // EOPID 311 (id 435): the prizes still in the pocket, from the next on (+0x488 .. +0x484), as a list of oids
        // (0x200F58 with kind 6: S32 values); the pocket's glow script (ASEG 20472's start) reads it
        RegisterObjectGetter(311, [](LO* plo) {  // orig: 0x001F1EB8
            const POCKET& p = PocketOf(plo);
            std::shared_ptr<CPair> ppairHead;
            CPair* ppairTail = nullptr;
            for (int i = p.i; i < p.c && i < 8; ++i)
            {
                auto ppair = PpairNew();
                ppair->m_ref.SetS32(p.ae[i].oid);
                if (ppairTail) ppairTail->m_ppairNext = ppair;
                else ppairHead = ppair;
                ppairTail = ppair.get();
            }
            CRef r(TAGK_None);  // (0x200F58 with none: the empty list, 0x12A0B0(ret, 0))
            if (ppairHead) r.SetPair(std::move(ppairHead));
            return r;
        });
    }
} s_initpocketmethods;
}  // namespace

// orig: 0x1A6730 (CloneLo copies the object): a copy keeps the pocket the load's methods filled (0x103C18 coins,
// 0x103CA0 / 0x103D28 prizes); natively they live in s_mppocket, so a copied guard's pocket was empty
void ClonePocketData(LO* source, LO* target)
{
    const auto it = s_mppocket.find(source);
    if (it != s_mppocket.end()) s_mppocket[target] = it->second;
}
