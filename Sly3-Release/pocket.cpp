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
#include "xf.h"
#include "cls.h"
#include "disc.h"
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
// (Sly 3, orig3 0x1F1738.. / 0x1F29A0: the givers' prize oids moved: Sly 2's 0x2B0 / 0x2B1 / 0x2B3 / 0x2B5)
constexpr int OID_POINT = 0x307;        // the pocket's launch point record (kind 0xD)
constexpr int OID_POINT_CARRY = 0x308;  // the same for a carryable prize whose task is waiting
constexpr int OID_COINS = 0x30A;        // the coins entry
constexpr int OID_PRIZE = 0x30C;        // a drawn prize (its kind +0x8: 1258..1284)
constexpr uint32_t IB_TASK = 0x230;     // the carryable prize's task: Sly 3 EOPID 968 (+0x1A0), the field kept where the loader stores it (Sly 2's +0x230, as pickup.cpp reads it)
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

// orig3: 0x001F1738 (c coins, the prize 0x30A, when +0x4A0 allows)
void AddPocketCoins(LO* plo, int c)
{
    POCKET& p = PocketOf(plo);
    if (!FPocketMore(plo) || p.c >= 8) return;
    POCKETENT& e = p.ae[p.c++];
    e.oid = OID_COINS;
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
            if (pch->oid == 0x309)  // (Sly 3: the parts table 0x346F70 {0x309, +0x1A0}; Sly 2's 0x2B2)
                if ((psm = dynamic_cast<SM*>(pch))) break;
    if (psm) ReplaceSmaHandle(psm, static_cast<ALO*>(ppocket), -1, 0, &psma);  // 0x1F1448
    else WarnOnce("the pocket object's SM 0x309 (not found)");
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
            static const OID s_aoid[] = {122, 131, 132, 135};  // (orig3: 0x346B20; Sly 2's 92 / 101 / 102 / 105)
            SeekSma(psma, s_aoid[k + 1]);  // 0x1F0730
        }
    if (FPrint())
        std::printf("pocket %d: mode %d t %.3f sma %p goal %d\n", plo->oid, k, g_clock.t, static_cast<void*>(psma), psma ? OidSmaGoal(psma) : -1);
}

// orig3: 0x001F18D0 (Sly 2's 0x1F1900, changed: the mode by the giver's state: 0 while alerted (the guard's state
// bits +0x2F4 under the mask 0x467B80: natively the copied guard's Sly 2 states 0x1D / 0x20 / 5 / 6 / 7 / 0x17 / 0x16),
// else 2 with a readied reward (the pocket's +0x19C) and a gang member in control (0x370AC0 / 0x36B250 / 0x38AE90:
// Sly, Bentley, Murray), else 1. Sly 2 had -1 without a reward and 1 for coins; in Sly 3 coins glow too (the
// reference's v_ext guards: mode 2 with coins only).)
void UpdatePocketMode(LO* plo)
{
    LO* ppocket = PloPocket(plo);
    if (!ppocket) return;
    LO* pcur = g_psw ? PloPoCurrent(g_psw) : nullptr;  // 0x1F4AD0
    if (GUARD* pg = PguardFromLo(plo))
    {
        uint64_t m = 0;
        for (int n : {0x1D, 0x20, 5, 6, 7, 0x17, 0x16}) m |= GrfStateBit(n);  // 0x207A20
        if (pg->grfState & m)
        {
            SetPocketMode(plo, 0);
            return;
        }
    }
    const bool fGang = pcur && (FIsLoDerivedFrom(pcur, sly3::CID_JT) || FIsLoDerivedFrom(pcur, sly3::CID_BENTLEY) ||
                                FIsLoDerivedFrom(pcur, sly3::CID_MURRAY));
    const auto it = s_mpreward.find(ppocket);
    SetPocketMode(plo, it != s_mpreward.end() && it->second.plo && fGang ? 2 : 1);
}

// orig3: 0x001F1A08 (the giver can be pickpocketed: a glow (mode 1 / 2, or below -1) and no planted object +0x4A4)
bool FPocketPickable(LO* plo)
{
    const POCKET& p = PocketOf(plo);
    return (p.nMode < -1 || 0 < p.nMode) && !p.fPlanted;
}

// orig3: 0x001F1A30 (Sly 2's FillPocket 0x1F1A40: the giver's start, unless +0x4A0: the planted flag +0x4A4 and the
// pocket object's reward cleared (0x1F2950); a prize set before (a carryable 0x68 whose task is done is dropped) is
// kept, else one is drawn (0x30C with the chance, its kind by the weights; -1 none); then the coins (NRandInRange(nLow,
// nHigh) over 2..3 entries of 0x30A), the prize last. The table: the episode's row (0x25E188: 0x4798A0, -1 -> 3) of
// 0x346D50 for a giver whose option 264 (+0x2F0, the setter 0x250B58) is 1 or 2, else of 0x346B30 (0x3C bytes:
// nLow, nHigh, the prize's chance, 6 x {weight, kind}); read from the executable.)
void FillPocket(LO* plo)
{
    POCKET& p = PocketOf(plo);
    LO* ppocket = PloPocket(plo);
    p.fPlanted = 0;
    if (ppocket) ClearReward(ppocket);  // 0x1F2950
    if (FPocketMore(plo)) return;
    int iep = IepisodeCur();  // 0x25E188
    if (iep == -1) iep = 3;
    if (iep < 0 || iep > 8) iep = 3;
    const int nKind = static_cast<int>(plo->U32Field(0, 0x2F0, 0));  // option 264
    const uint32_t vaTbl = ((0 < nKind && nKind < 3) ? 0x346D50u : 0x346B30u) + 0x3Cu * static_cast<uint32_t>(iep);
    const auto u = [&](uint32_t ib) { return g_disc.U32Elf(vaTbl + ib); };
    const int nLow = static_cast<int>(u(0)), nHigh = static_cast<int>(u(4));
    const float gPrize = cmm::F32(u(8));
    POCKETENT e{0, 0, 0, 0};  // (the memset)
    if (p.c > 0)
    {
        LO* po = ppocket ? PloFindSwNearest(ppocket, p.ae[0].oid) : PloFindSwAny(p.ae[0].oid);  // 0x17E1B8 (4)
        // a carryable prize (0x68) whose task (+0x230 as loaded) is done is dropped
        if (po && FIsLoDerivedFrom(po, S3CID(0x5C)))
        {
            const OID oidTask = static_cast<OID>(po->U32Field(0, IB_TASK, static_cast<uint32_t>(-1)));
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
        if (gPrize < GRandInRange(0.0f, 1.0f))  // 0x11E940
        {
            e.oid = -1;
            e.n8 = -1;
        }
        else
        {
            int cw = 0;
            for (uint32_t k = 0; k < 6; ++k) cw += static_cast<int>(u(0xC + 8 * k));
            int n = NRandInRange(0, cw - 1);  // 0x11E8F0
            for (uint32_t k = 0; k < 6; ++k)
            {
                const int w = static_cast<int>(u(0xC + 8 * k));
                if (n < w) { e.n8 = static_cast<int>(u(0x10 + 8 * k)); break; }
                n -= w;
            }
            e.c = 1;
            e.oid = OID_PRIZE;
        }
        p.c = 0;
    }
    int cCoin = NRandInRange(nLow, nHigh);
    const int cEnt = NRandInRange(2, 3);
    for (int i = 0; i < cEnt && p.c < 8; i++)
    {
        const int k = cCoin / (cEnt - i);
        POCKETENT& pe = p.ae[p.c++];
        pe.oid = OID_COINS;
        pe.n4 = 0;
        pe.n8 = 0;
        const int c = 0 < k ? k : 1;
        cCoin -= c;
        pe.c = c;
        if (cCoin == 0) break;
    }
    if (e.c > 0 && p.c < 8) p.ae[p.c++] = e;
    if (FPrint())
    {
        const glm::vec3 pos = FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(plo)) : glm::vec3(0.0f);
        std::printf("pocket %d (%p, copy of %p): filled (episode %d kind %d) at (%.0f %.0f %.0f) t %.3f", plo->oid, static_cast<void*>(plo), static_cast<void*>(plo->ploCloneOf), iep, nKind, pos.x, pos.y, pos.z, g_clock.t);
        for (int i = 0; i < p.c; i++) std::printf(" {%d %d %d x%d}", p.ae[i].oid, p.ae[i].n4, p.ae[i].n8, p.ae[i].c);
        std::printf("\n");
    }
}

// orig3: 0x001F1CF8 (the next prize whose object is there (0x17E1B8 kind 4 from the pocket): ready (+0x49C) when it
// is a carryable (0x68) whose +0x1A0's +0x44 is -1 / 0, or 0x1F29A0 (the reward's instance, 0x307 / 0x308) answers 1)
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
        int oidPoint = OID_POINT;
        if (FIsLoDerivedFrom(po, S3CID(0x5C)))
        {
            // (+0x230's task +0x44: -1 / 0 ready at once, 1 the point 0x308, else the next entry)
            const OID oidTask = static_cast<OID>(po->U32Field(0, IB_TASK, static_cast<uint32_t>(-1)));
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
            oidPoint = OID_POINT_CARRY;
        }
        if (NReadyReward(ppocket, po, e.n8, oidPoint) == 1) p.fReady = 1;  // 0x1F29A0
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

// orig3: 0x001F29A0 (Sly 2's 0x1C2DC8, changed: the reward readied in the pocket object (+0x19C): a proxy's
// instance (+0x198 the proxy, its row locked 0x1FC060; 1 when there is none) or the object itself (an ALO; 2
// otherwise); a PICKUP_5E (0x6A) gets the kind; a carryable (0x71) gets the guard (+0x188, 0x17E5D8 kind 3 class 0x3A)
// and its state 6 (slot 80); the launch point (+0x194) is the pocket's record oidPoint of kind 0xD. The reward is then
// out of the world (slot 9) and parented to the pocket at the point (slots 10 / 43: natively drawn at the point by
// CollectPocketDraws); a carryable flies there over 0.5 s (+0x130..+0x190: TODO). 3; 0 when one is readied already.)
int NReadyReward(LO* ppocket, LO* plo, int id, int oidPoint)
{
    if (!ppocket) return 2;
    REWARD& r = s_mpreward[ppocket];
    if (r.plo) return 0;
    r.pproxy = nullptr;
    if (FIsLoDerivedFrom(plo, S3CID(6)))
    {
        r.pproxy = plo;
        r.plo = PloAttachProxy(g_psw, plo, 0);  // 0x1FBE30
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
    if (FIsLoDerivedFrom(r.plo, S3CID(0x5E))) SetPickupLootKind(r.plo, id);  // 0x1F9078
    else if (FIsLoDerivedFrom(r.plo, S3CID(0x64)))
    {
        WarnOnce("0x1F29A0's carryable reward (+0x188 the guard, its state 6)");
    }
    // (the native pickups keep their put-away state 2 out of the draw pass; the original draws the reward as the
    // pocket's child instead)
    if (FIsLoDerivedFrom(r.plo, S3CID(0x5A))) SetPickupState(r.plo, 2);
    RemoveLo(g_psw, r.plo);  // vt slot 9
    r.pxf = nullptr;
    if (FCidDerivedFrom(ppocket->Cid(), S3CID(0x30)))
        for (const auto& pxf : static_cast<ALO*>(ppocket)->axf)  // 0x23DDA0
            if (pxf && pxf->tag == oidPoint && pxf->kind == 0xD) { r.pxf = pxf.get(); break; }
    if (FPrint()) std::printf("reward %d: readied %d (cid 0x%X) proxy %d point %s\n", ppocket->oid, r.plo->oid, r.plo->Cid(),
                              r.pproxy ? r.pproxy->oid : -1, r.pxf ? "yes" : "no");
    return 3;
}

// orig3: 0x001F2C00 (Sly 2's 0x1C2EF0, changed: the reward launched c times from the launch point (+0x194) toward
// posTarget (the taker's position; Sly 2: the current player's): each to a point around it (SetVectorCylind(GRandInRange(-1.5, 1.5), GRandInRange(100, 150), -75) in his frame facing from the
// source) in GRandInRange(0.75, 1.2) (0x325038..: the same constants) under the gravity (0x346FD0, 0x212628); pickups
// by 0x1F8138 (the SM's delay i 0.05; coins / charms collected after i 0.15 when fCoinDelay, else never by time),
// other objects by slots 8 / 43 / 40; the more copies from the proxy (0x1FBE30); the first returned; the reward
// cleared)
LO* PloLaunchReward(LO* ppocket, int c, const glm::vec3& posTarget, bool fCoinDelay)
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
        const glm::vec3 posCur = posTarget;
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
            const bool fCoin = fCoinDelay && (FIsLoDerivedFrom(plo, S3CID(0x5B)) || FIsLoDerivedFrom(plo, S3CID(0x5D)));
            const float fi = static_cast<float>(i);
            LaunchPickup(plo, pos0, vel, fi * 0.05f, fCoin ? fi * 0.15f : FLT_MAX, r.pproxy);  // 0x1C6BD0
        }
        else
        {
            AddLo(g_psw, plo);  // vt slot 8
            WarnOnce("0x1F2C00's launch of an object that is not a pickup (vt slots 43 / 40)");
        }
        i++;
    }
    ClearReward(ppocket);  // 0x1C2DB8
    return ploFirst;
}

// orig3: 0x001F1E18 (Sly 2's 0x1F1E20, changed: the take (while i < c, 0x1F1F78): unless a planted object is readied
// (+0x4A4), the task event 0x11 (0x1854C8; Sly 2's 0x19), which can refuse it (+0x4A8); the reward 0x1F2C00 at the
// pocket toward posTarget with the entry's count (1 for a planted one); the glow off; the next prize (a planted one is
// not counted))
LO* PloTakePocket(LO* plo, const glm::vec3& posTarget, bool fCoinDelay)
{
    POCKET& p = PocketOf(plo);
    if (p.i >= p.c) return nullptr;
    if (!p.fPlanted)
    {
        p.fTakeCancel = 0;
        BroadcastTaskEvent(g_psw, 0x19, plo);  // 0x1854C8 (0x11)
        if (p.fTakeCancel) return nullptr;
    }
    const int c = p.fPlanted ? 1 : p.ae[p.i].c;
    LO* r = PloLaunchReward(PloPocket(plo), c, posTarget, fCoinDelay);  // 0x1F2C00
    if (FPrint()) std::printf("pocket %d: take %d (prize %d x %d) t %.3f\n", plo->oid, p.i, p.ae[p.i].oid, c, g_clock.t);
    SetPocketMode(plo, -1);
    if (p.fPlanted) p.fPlanted = 0;
    else p.i++;
    NextPocketPrize(plo);
    return r;
}

// orig: 0x0018B348 (Sly's load: the clip +0x1BC8's event 0x167 time +0x1BD8; at it, in Sly's object space (+0x60), the
// cane's point (+0x19A4's object record: +0x1BE0), the arm's joint (+0x19CC: +0x1BF0), the hand's (+0x19C0: +0x1C00,
// its rotation +0x1C10) and the cane's point in the hand's frame (+0x1C20), for the reach test 0x18B460)
void LoadPickpocket(SLY* p, ASEG* paseg)
{
    if (!paseg) return;
    p->g1BD8 = GEventAseg11(paseg, 0x185);  // 0x12F990 (orig3 0x1ACD28: Sly 3's event 0x185, Sly 2's 0x167)
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
            // (orig3 0x1ACE40: the arm's joint controllers by 0x12FE90 (SetCtrlSourceOffset): +0x19F8 (Sly 3 +0x1B00,
            // aphandle[5]) the hand's turn, +0x19EC (+0x1AF4, [2]) what is left over, +0x19FC (+0x1B04, [6]) the slide along
            // x; AUDIT4 PLAYER-17)
            if (CTRLSOURCE* pc = p->slide.aphandle[5]) SetCtrlSourceOffset(pc, glm::vec4(0.0f, radYaw, 0.0f, 0.0f));
            if (CTRLSOURCE* pc = p->slide.aphandle[2]) SetCtrlSourceOffset(pc, glm::vec4(vecResidual, 0.0f));
            if (CTRLSOURCE* pc = p->slide.aphandle[6]) SetCtrlSourceOffset(pc, glm::vec4(dx, 0.0f, 0.0f, 0.0f));
            p->f1BD4 = 1;
            p->t1BDC = g_clock.t;
            return;
        }
    }
    // (orig3 0x1ACE40's end: the three controllers relax to zero within 0.25 s of the last reach)
    const float dtRem = p->t1BDC + 0.25f - g_clock.t;
    const float dtAfter = dtRem - g_clock.dt;
    const float gain = 0.0f < dtAfter ? dtAfter / dtRem : 0.0f;
    for (int i : {5, 2, 6})
        if (CTRLSOURCE* pc = p->slide.aphandle[i]) SetCtrlSourceOffset(pc, pc->ofs * gain);
    p->f1BD4 = 0;
}

// orig3: 0x001AA988's event 0x185 (Sly 2's 0x167 in 0x188F20): with a STEPGUARD target (0x3A) and the hand set
// (+0x1C2C), the take 0x156DC0: PloTakePocket(target, Sly's position, 1); a prize that is not a coin / charm (0x67 /
// 0x69) is held up when Sly has the prize clip (+0x145C): +0x1458, 0x154EF0 (the interaction kind 12)
void PickpocketTake(SLY* p)
{
    if (!p->plo10DC || !FIsLoDerivedFrom(p->plo10DC, S3CID(0x38)) || !p->f1BD4) return;
    LO* r = PloTakePocket(p->plo10DC, PosLoWorld(p->palo), true);
    if (!r || FIsLoDerivedFrom(r, S3CID(0x5B)) || FIsLoDerivedFrom(r, S3CID(0x5D))) return;
    // (Sly 3: the actor's part +0x145C, oid 751: table 0x342DD0 entry 58, stored at FN3_TAG | 0x145C)
    LO* ploClip = nullptr;
    for (const auto& [ib, plo] : p->palo->apart)
        if (ib == (FN3_TAG | 0x145Cu)) ploClip = plo;
    ASEG* paseg = dynamic_cast<ASEG*>(ploClip);
    if (!paseg) return;
    p->plo1BCC = r;
    StartPrizeInteract(p, paseg, r, -1);  // 0x154EF0
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
