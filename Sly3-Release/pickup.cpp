// pickup.cpp: the pickups (pickup.h). Ported from the decomp's src/hand func_001C4E50 / 001C6508 / 001C60B8 /
// 001C65F8 / 001C5E48 / 001C6588 / 001C5D18 / 001C6BD0 and the asm (0x1C5148, 0x1C55E0).
#include "pickup.h"
#include "game/sly3.h"
#include "worldglobals.h"

#include "activate.h"
#include "alo.h"
#include "clock.h"
#include "cls.h"
#include "lo.h"
#include "po.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include <algorithm>
#include "savedata.h"
#include "ref.h"
#include "update.h"
#include "eval.h"
#include "frame.h"
#include "vtnative.h"
#include "anim.h"
#include "xf.h"
#include "xfphys.h"
#include "sm.h"
#include "smrun.h"
#include "asega.h"
#include "aseg.h"
#include "alo.h"
#include "cm.h"
#include "animfind.h"
#include "nav.h"
#include "bsp.h"
#include "vecmat.h"
#include "physworld.h"
#include "brx.h"
#include "glob.h"
#include "emitter.h"
#include "damage.h"
#include "actor.h"
#include "screenrt.h"
#include "ainoise.h"
#include "objmethod.h"
int NActorPower(ACTOR* pa);           // gadget.cpp (0x1441A0)
void SetActorPower(ACTOR* pa, int n);  // gadget.cpp (0x1441C0)
float GRandInRange(float gLow, float gHigh);  // xfmath.h (0x11D448)
glm::mat3 BuildLookAtMatrix(const glm::vec3& vecX, const glm::vec3& vecY);  // mat.h (0x11B830)
int NRandInRange(int nLow, int nHigh);                     // util.h (0x11D3F8)
inline int NRandInRangePk(int a, int b) { return NRandInRange(a, b); }
float TJumpBallistic(const glm::vec3& d, const glm::vec3& vGravity, float vzMax, float vzMin, float sMax, float sMin);  // damage.cpp (0x196928)
void RequestLoTaskState(LO* plo, int n, LO* ploContext);  // task.cpp (0x164640)

#include <cfloat>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>

// orig: 0x00161868 / 0x001618B0 (SW +0x3BAC / +0x3BB8: the coin / clue bottle counter (screen objects 0x36D / 0x36E in
// the reference's RAM) shown (vt slot 49, -1) and its idle time started again (0x212A30): it shows for its +0x8C, 5 s)
static void ShowCounter(OID oid)
{
    LO* plo = g_psw ? PloFindSwObjectByOid(g_psw, oid) : nullptr;
    if (!plo) return;
    ShowScreenObj(plo, -1);
    RefreshScreenObj(plo);
}
void ShowCoinCounter() { ShowCounter(0x36D); }
void ShowClueCounter() { ShowCounter(0x36E); }

namespace
{

// orig 0x00161798 / 0x001617E0: the coins (as shop.cpp's); setting them shows the counter (0x161868)
int CCoins() { CRef r = RefGetProgress({"loot", "ccoin"}); return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : 0; }
void SetCoins(int n)
{
    CRef r;
    r.SetS32(n);
    FSetProgress({"loot", "ccoin"}, r);
    ShowCoinCounter();
}
// orig: 0x001C67A0 (a coin counted). (The reference with the 100% card stays at 9999 after a coin: the progress value's
// limit, not in 0x1C67A0 / 0x1617E0 themselves; natively kept here.)
void AddCoin() { SetCoins(std::min(CCoins() + 1, 9999)); }

// D_002C0DA0: the coin's flight to the counter (the top right of the screen at depth 100)
// D_002C0E10: the charm's flight to the health meter (top left)
const RIPHUDDEF& RiphuddefCharm()
{
    static RIPHUDDEF s_def;
    static bool s_f = false;
    if (!s_f)
    {
        s_def.posGoal = glm::vec3(-0.7f, 0.72f, 100.0f);
        s_def.velGoal = glm::vec3(1.0f, -1.0f, 0.0f);
        s_def.velLo = glm::vec3(-1.0f, 2.0f, 0.0f);
        s_def.velHi = glm::vec3(1.0f, 3.0f, 0.0f);
        s_def.axisSpin = glm::vec3(0.0f, 0.0f, 1.0f);
        s_def.size[0] = 1.0f; s_def.size[1] = -0.8f; s_def.size[2] = 0.0f;
        s_f = true;
    }
    return s_def;
}
// orig: 0x00144090 (the players' slot 80): the splice event 0xD's answer (0x1C41B8: the bound handler's boolean,
// default yes: the players' scripts answer no when health and power are both full) and the damage controller not dead
// (0x1A23C0 != 2)
bool FActorTakesCharm(ACTOR* pa)
{
    bool f = true;
    if (LO* plo = pa->palo; plo && plo->pframe)
        if (CRef* pref = plo->pframe->PrefFindBinding(0xD, 1))
        {
            CRef refSelf;
            refSelf.SetBasic(plo);
            const CRef ref = RefApply(*pref, 1, &refSelf, plo->pframe.get());
            if (ref.m_tagk == TAGK_Bool) f = ref.m_bool != 0;
        }
    if (!f) return false;
    return NDamageStatus(pa) != 2;
}
// orig: 0x00144268 (the players' slot 81): the splice event 0xC with (a, b) (0x1C4228), health + a up to +0xDFC,
// power + b up to +0x1300
void HealActor(ACTOR* pa, int a, int b)
{
    CRef aref[2];
    aref[0].SetS32(a);
    aref[1].SetS32(b);
    HandleLoSpliceEvent(pa->palo, 0xC, false, 2, aref);
    if (int* pc = DmgCounterOf(pa)) *pc = std::min(DmgCapacityOf(pa), *pc + a);  // 0x144188
    SetActorPower(pa, NActorPower(pa) + b);                                    // 0x1441C0 / 0x1441A0
}
const RIPHUDDEF& RiphuddefCoin()
{
    static RIPHUDDEF s_def;
    static bool s_f = false;
    if (!s_f)
    {
        s_def.posGoal = glm::vec3(0.83f, 0.75f, 100.0f);
        s_def.velGoal = glm::vec3(1.0f, -1.0f, 0.0f);
        s_def.velLo = glm::vec3(-1.0f, 2.0f, 0.0f);
        s_def.velHi = glm::vec3(1.0f, 3.0f, 0.0f);
        s_def.axisSpin = glm::vec3(0.0f, 0.0f, 1.0f);
        s_def.size[0] = 1.0f; s_def.size[1] = -0.9f; s_def.size[2] = 0.0f;
        s_f = true;
    }
    return s_def;
}
}  // namespace

namespace
{
// orig: 0x0011E290 (a's part along the unit vector b and the rest; their dot product: as water.cpp's)
float GSplitVector(const glm::vec3& a, const glm::vec3& b, glm::vec3* ppar, glm::vec3* pperp)
{
    const float d = (a.x * b.x + a.y * b.y) + a.z * b.z;
    const glm::vec3 p = b * d;
    if (ppar) *ppar = p;
    if (pperp) *pperp = a - p;
    return d;
}
void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "pickup: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}
bool FPrint() { static const bool s_f = std::getenv("SLY2N_PICKUPPRINT") != nullptr; return s_f; }

struct PICKUP
{
    struct LOOTDEF
    {
        int id = -1;             // +0: the authored loot variant (e.g. 1115)
        int index = -1;          // +4: loot.mplootkc index
        std::vector<int> aglob;  // +0x1C / +0x20: up to three visible mesh groups
    };
    std::vector<LOOTDEF> aloot;   // +0x23C / +0x240 (0x2C bytes per definition)
    int iloot = -1;              // +0x244: the selected definition
    int state = -1;               // +0x150
    float t = 0.0f;               // +0x154
    float dtSm = 0.0f;            // +0x160
    glm::vec3 pos{0.0f};          // +0x170
    glm::vec3 vel{0.0f};          // +0x180
    glm::vec3 acc{0.0f};          // +0x190
    float g1A0 = 0.25f;           // +0x1A0 the sparkle's chance
    int fLanded = 0;              // +0x1C4
    int fCollectTimed = 1;        // +0x1C8
    float tCollect = FLT_MAX;     // +0x1CC
    float g1D0 = 250.0f, g1D4 = 150.0f;  // +0x1D0 / +0x1D4 (the bounce's speeds)
    float sRadius = 45.0f;        // +0x1D8 (the ground test's sphere)
    float sCollect = 65.0f;       // +0x1DC
    float g1E0 = 0.5f, g1E4 = 0.25f;     // +0x1E0 / +0x1E4 (the bounce's factors)
    float dtRestLow = 5.0f, dtRestHigh = 5.0f;  // +0x230 / +0x234
    float dtRest = 0.0f;          // +0x238
    LO* pproxy = nullptr;         // the proxy whose instance it is (0x1CB658)
    SMA* psma = nullptr;          // +0x168 (the SM +0x164 run on it: its bob / turn)
    LO* ploGlint = nullptr;       // +0x1A8 (+0x1A4's oid: the glint's model)
    LO* ploGlintL = nullptr;      // +0x1B0 (+0x1AC: where it shows when the camera looks one way)
    LO* ploGlintR = nullptr;      // +0x1B8 (+0x1B4: the other)
    LO* ploCollect = nullptr;     // +0x210 (+0x20C: collection's message receiver, usually an explosion)
    LO* ploExpire = nullptr;      // +0x218 (+0x214: timed expiry's message receiver)
    int fSidePrev = 0;            // +0x1BC
    float tGlint = -1e9f;         // +0x1C0
    bool fWritten = false;        // (natively: the record's local translation last written by the motion, to notice
    glm::vec3 posLocal{0.0f};     // a placement from outside (a proxy's instance placed): +0x170 taken again)
};
std::unordered_map<const LO*, PICKUP> s_mppickup;
WORLD_TABLE(s_mppickup);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::unordered_map<const LO*, int> s_mpclue;  // the clue bottles' proxies (cid 7): their +0x100
WORLD_TABLE(s_mpclue);   // (PC: moved aside while a kept world loads, worldglobals.h)

}  // namespace

// orig: 0x001CB658 (the streaming proxy whose instance plo is: as activate.cpp's 0x1CB410)
LO* PproxyOfInstance(LO* plo)
{
    for (auto& [pproxy, ps] : g_psw->mpproxy)
    {
        if (ps.iwe < 0 || ps.iwe >= static_cast<int>(g_psw->awe.size())) continue;
        const WE& we = g_psw->awe[static_cast<size_t>(ps.iwe)];
        if (we.cloLoaded != 1) continue;
        for (int i = we.cUsed; i < std::min(we.cinst, static_cast<int>(we.aplo.size())); ++i)
            if (we.aplo[static_cast<size_t>(i)] == plo)
                // (the row's owner: the proxies of one group share its rows)
                return i < static_cast<int>(we.aploRowOwner.size()) && we.aploRowOwner[static_cast<size_t>(i)] ? we.aploRowOwner[static_cast<size_t>(i)]
                                                                                                               : pproxy;
    }
    return nullptr;
}

namespace
{

// orig: 0x001A58D0(p, 0, 0) (vt slot 10 with no parent and no channel: the object back at the world's root, its root
// record no longer placed relative to another object's record). Natively the activation parent and the root record's
// reference are apart: a pool instance attached before (e.g. a pocket's launch point, record kind 13 at scale 0.5) kept
// that reference, so a coin thrown later was drawn at half its size. The world placement is kept.
void SetLoParentNone(LO* plo)
{
    SetLoParentActivation(g_psw, plo, nullptr);
    ALO* palo = static_cast<ALO*>(plo);
    if (palo->axf.empty() || !palo->axf[0]->pxfRef) return;
    UpdateAloXfWorlds(palo);
    XF* pxf = palo->axf[0].get();
    pxf->vel = pxf->pxfRef->matWorld * glm::vec4(glm::vec3(pxf->vel), 0.0f);
    pxf->mat = glm::mat4(1.0f);  // (the reference's coins: their root record unrotated and unscaled at the root)
    pxf->mat[3] = pxf->matWorld[3];
    pxf->pxfRef = nullptr;
    pxf->nStamp = ++g_cXfStamp;
}

PICKUP* Ppickup(LO* plo)
{
    auto it = s_mppickup.find(plo);
    return it == s_mppickup.end() ? nullptr : &it->second;
}

// orig: 0x001C4E50 (the base's init) + 0x001C6508 (the coin's: the defaults above)
PICKUP& InitPickup(LO* plo)
{
    PICKUP& p = s_mppickup[plo];
    p = PICKUP{};
    if (FIsLoDerivedFrom(plo, S3CID(0x5C)))  // 0x1C6AB8
    {
        p.sRadius = 35.0f; p.sCollect = 35.0f; p.g1D0 = 500.0f; p.g1D4 = 250.0f; p.g1E4 = 0.6f; p.g1E0 = 0.6f; p.g1A0 = 0.75f;
    }
    else if (FIsLoDerivedFrom(plo, S3CID(0x5D)))  // 0x1C67D8 (+0x23C / +0x240: the healing 10 / 20)
    {
        p.sRadius = 45.0f; p.sCollect = 75.0f; p.g1D0 = 250.0f; p.g1D4 = 50.0f; p.g1E4 = 0.2f; p.g1E0 = 0.65f; p.g1A0 = 0.25f;
        p.dtRestLow = 4.0f; p.dtRestHigh = 5.0f;
    }
    else if (FIsLoDerivedFrom(plo, S3CID(0x5F)))  // 0x1C7E38
        p.sCollect = 50.0f;
    else if (FIsLoDerivedFrom(plo, S3CID(0x5E)))  // 0x1C7648
    {
        p.sRadius = 35.0f; p.sCollect = 40.0f; p.g1D0 = 250.0f; p.g1D4 = 50.0f; p.g1E4 = 0.6f; p.g1E0 = 0.75f;
        p.dtRestLow = 8.0f; p.dtRestHigh = 10.0f;
    }
    // (the options' fields over the defaults: +0x1C8 the timed collect)
    p.fCollectTimed = static_cast<int>(plo->U32Field(0, 0x1C8, 1));
    // orig: 0x001C65F8 / 0x001C68F0 / 0x001C77E8 draw the rest time
    // from the loaded fields, after the class's constructor defaults.
    if (FIsLoDerivedFrom(plo, S3CID(0x5B)) || FIsLoDerivedFrom(plo, S3CID(0x5D)) || FIsLoDerivedFrom(plo, S3CID(0x5E)))
    {
        p.dtRestLow = std::bit_cast<float>(plo->U32Field(0, 0x230, std::bit_cast<uint32_t>(p.dtRestLow)));
        p.dtRestHigh = std::bit_cast<float>(plo->U32Field(0, 0x234, std::bit_cast<uint32_t>(p.dtRestHigh)));
    }
    // the glint's objects (0x1C4F60's links: +0x1A4 / +0x1AC / +0x1B4 oids; the pickup's own first)
    // (option 894: the three oids; 0x15E938 kind 4 from the pickup)
    OID aoid[3] = {-1, -1, -1};
    for (const OPT& o : plo->aopt)
        if (o.eopid == S3EOPID(894))
            for (size_t i = 0; i < 3 && i < o.aval.size(); ++i) aoid[i] = o.aval[i].n;
    auto find = [plo](OID oid) -> LO* { return oid == -1 ? nullptr : PloFindSwNearest(plo, oid); };
    p.ploGlint = find(aoid[0]);
    p.ploGlintL = find(aoid[1]);
    p.ploGlintR = find(aoid[2]);
    // orig: 0x001C4F60: resolve the collection message's receiver in this
    // instance's hierarchy, just as the glint parts above.
    p.ploCollect = find(static_cast<OID>(plo->U32Field(0, 0x20C, static_cast<uint32_t>(-1))));
    p.ploExpire = find(static_cast<OID>(plo->U32Field(0, 0x214, static_cast<uint32_t>(-1))));
    if (FIsLoDerivedFrom(plo, S3CID(0x5E)))
    {
        // orig: 0x1C7988 / 0x1C7A08 / 0x1C7A10: retain the authored
        // definition/group records, including the copies of a pooled model.
        int i = -1;
        for (const OPT& o : plo->aopt)
        {
            if (o.eopid == S3EOPID(907) && o.aval.size() >= 2 && p.aloot.size() < 32)
            {
                p.aloot.push_back({o.aval[0].n, o.aval[1].n, {}});
                i = static_cast<int>(p.aloot.size()) - 1;
            }
            else if (o.eopid == S3EOPID(908)) i = -1;
            else if (o.eopid == S3EOPID(909) && i >= 0 && !o.aval.empty() && p.aloot[i].aglob.size() < 3)
                p.aloot[i].aglob.push_back(o.aval[0].n);
        }
    }
    if (FPrint())
    {
        std::printf("pickup %d fields:", plo->oid);
        for (const auto& [k, u] : plo->afield) std::printf(" %llX=%X", static_cast<unsigned long long>(k), u);
        std::printf(" opts:");
        for (const OPT& o : plo->aopt)
        {
            std::printf(" %d[", o.eopid);
            for (const OPTVAL& v : o.aval) std::printf("k%d n%d g%g ", v.k, v.n, v.ag[0]);
            std::printf("]");
        }
        std::printf("\n");
    }
    if (FPrint())
        std::printf("pickup %d: glint %d at %d / %d chance %.2f\n", plo->oid, p.ploGlint ? p.ploGlint->oid : -1, p.ploGlintL ? p.ploGlintL->oid : -1,
                    p.ploGlintR ? p.ploGlintR->oid : -1, p.g1A0);
    return p;
}

// orig: 0x001F10F0 (an SMA's transitions advanced by t: the playing one's time set when t falls in it, else the next)
void AdvanceSma(SMA* psma, float t)
{
    if (!psma || FSmaRetracted(psma)) return;
    ASEGA* pa = psma->pasega;
    if (!pa || !pa->paseg) return;
    const float d = PasegdOf(pa->paseg)->tMax - pa->t;
    if (t < d) SetAsegaTime(pa, 1, t, 1.0f);  // 0x130D30
    else WarnOnce("0x1F10F0 past the transition (0x130B18)");
}

// orig: 0x001C60B8 (the base's state change: the motion's start from the record when coming from -1 / 2, the
// acceleration by state, gone (2): removed and the proxy's row given back, the child 0x12 shown in 0 / 4 / 5, the SM's
// goal 0x184 / 0x183 / 0x186 (TODO))
void SetPickupStateBase(LO* plo, PICKUP& p, int n)
{
    if (p.state == n) return;
    ALO* palo = static_cast<ALO*>(plo);
    if (n != 0 && n != 5) SetLoParentNone(plo);  // 0x1A58D0(p, 0, 0)
    if (p.state == -1 || p.state == 2)
    {
        if (XF* pxf = PxfPos(palo))
        {
            // (+0x170 is the record's translation: natively kept in the world frame, the record's reference converts)
            UpdateAloXfWorlds(palo);
            p.pos = glm::vec3(pxf->matWorld[3]);
            p.vel = glm::vec3(pxf->pxfRef ? pxf->pxfRef->matWorld * glm::vec4(glm::vec3(pxf->vel), 0.0f) : pxf->vel);
        }
    }
    switch (n)
    {
    case 0: p.acc = glm::vec3(0.0f); break;
    case 3:
    case 4: p.acc = glm::vec3(0.0f, 0.0f, -2250.0f); break;  // D_002C0D60
    case 2:
        p.tCollect = FLT_MAX;
        RemoveLo(g_psw, plo);  // vt slot 9
        if (p.pproxy) UnlockProxyRowOf(g_psw, p.pproxy, plo);  // 0x1CB658 / 0x1CB4B8
        break;
    case 5: p.acc = glm::vec3(0.0f); break;
    default: break;
    }
    // the child 0x12 +0x16C: shown in 0 / 4 / 5, hidden otherwise; the SM +0x168 (0x1F0380 / 0x1F0660): TODO
    for (LO* pc : plo->aploOwned)
        if (FCidDerivedFrom(pc->Cid(), S3CID(0x12)))
        {
            if (n == 0 || n == 4 || n == 5) AddLo(g_psw, pc);
            else RemoveLo(g_psw, pc);
            break;
        }
    // the SM +0x164 on +0x168 (0x1F0380 with the state's goal, its start advanced by +0x160 (0x1F10F0); then 0x1F0660;
    // no goal: retracted, released)
    const int anim = n == 0 ? 0x184 : (n == 3 || n == 4) ? 0x183 : n == 5 ? 0x186 : -1;  // (+0x15C: TODO, 0x184 default)
    if (anim != -1)
    {
        if (!p.psma)
        {
            // (+0x164: the named part 0x182 of the load's table D_002C0D40, 0x1A64E8)
            // A clone keeps its template's +0x164 (the reference's RAM: every coin instance of a world points at the one SM
            // 386 whose parent is the first coin 12929; the SM was taken out of the hierarchy by grf 0x200, so the clone
            // does not copy it): natively the clone's apart is empty, so the template chain's is taken.
            SM* psm = nullptr;
            for (const LO* pt = plo; pt && !psm; pt = pt->ploCloneOf)
                for (const auto& [ib, pl] : static_cast<const ALO*>(pt)->apart)
                    if (ib == 0x164 && pl) psm = dynamic_cast<SM*>(pl);
            if (psm)
            {
                if (FPrint()) std::printf("pickup %d: sm %d parent %d (apart %zu)\n", plo->oid, psm->oid, psm->paloParent ? psm->paloParent->oid : -1, palo->apart.size());
                p.psma = PsmaApplySm(psm, palo, anim, 0);  // 0x1F0380 (+ vt slot 1: kept)
                if (p.psma && 0.0f < p.dtSm) AdvanceSma(p.psma, p.dtSm);  // 0x1F10F0
            }
            else
            {
                std::string str;
                for (const auto& [ib, pl] : palo->apart) str += " +0x" + std::to_string(ib) + (pl ? "(" + std::to_string(pl->oid) + " cid " + std::to_string(pl->Cid()) + ")" : "()");
                for (LO* pc : plo->aploOwned) str += " owned " + std::to_string(pc->oid) + " cid " + std::to_string(pc->Cid());
                WarnOnce("a pickup without its SM (+0x164): cid " + std::to_string(plo->Cid()) + " parts" + str);
            }
        }
        else SetSmaGoal(p.psma, anim);  // 0x1F0660
    }
    else if (p.psma)
    {
        RetractSma(p.psma);  // 0x1F0598 (+ vt slot 2)
        p.psma = nullptr;
    }
    if (FPrint())
        std::printf("pickup %d (cid 0x%X): state %d -> %d t %.3f pos (%.1f %.1f %.1f) sma %d at %p iupd %d active %d st %d queued %d r %.0f mask 0x%X cm 0x%X\n", plo->oid, plo->Cid(), p.state, n, g_clock.t, p.pos.x,
                    p.pos.y, p.pos.z, p.psma ? 1 : 0, (void*)plo, plo->iupd, int(plo->fNodeActive), plo->st, int(plo->fQueued), plo->sRadius, plo->bMask, g_actview.grfCm);
    p.state = n;
    p.t = g_clock.t;
}

// orig: 0x001C65F8 (the coin: 0 rest time; 1 collected: HUD flight, sound 0x29, the +0x210 receiver,
// splice event 5 and task event 0x12, then gone)
void SetCoinState(LO* plo, PICKUP& p, int n)
{
    if (p.state == n) return;
    switch (n)
    {
    case 0: p.dtRest = GRandInRange(p.dtRestLow, p.dtRestHigh); break;  // 0x11D448
    case 1:
    {
        // the flight to the counter (0x1CE888(G, 9, 0): +0xF0 D_002C0DA0, +0xF4 the coin, +0xF8 0; slot 1 at the coin's
        // world point 0x1A62E8, size 1; its model the coin, its orientation the coin's (vt slot 31 0x1A6318)); counted
        // when it arrives (D_002C0E00: 0x1C67A0), or now without one
        ALO* palo = static_cast<ALO*>(plo);
        UpdateAloXfWorlds(palo);
        const XF* pxfObj = PxfObj(palo);
        const glm::mat3 mat = pxfObj ? glm::mat3(pxfObj->matWorld) : glm::mat3(1.0f);
        if (!FLaunchHudRip(plo, PosLoWorld(plo), mat, RiphuddefCoin(), [] { AddCoin(); })) AddCoin();  // 0x1C67A0
        if (CSnd* ps = PsndNewPlain(0x29)) ps->SetSource(nullptr);  // 0x1FBB90 / 0x1FB2F8
        if (p.ploCollect)
        {
            BURSTPARAMS bp;
            bp.grf = 1;
            bp.plo = plo;
            ExplodeLo(p.ploCollect, &bp);  // slot 51, after sound and before event 5
        }
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);  // vt slot 3 (5, 0, 0, 0, 0)
        ShowCoinCounter();  // 0x161868
        BroadcastTaskEvent(g_psw, 0x12, plo);  // 0x1619A0
        if (FPrint()) std::printf("pickup %d: collected, coins %d\n", plo->oid, CCoins());
        n = 2;
        break;
    }
    default: break;
    }
    SetPickupStateBase(plo, p, n);
}

void RequestTaskStateLo(LO* plo, int n, LO* ploContext) { RequestLoTaskState(plo, n, ploContext); }

// orig: 0x001C6B28 (the carryable prize 0x5C (the clue bottles): collected: the sound 0, its task +0x230 asked for the
// state 2 (0x164640), the splice event 5, gone)
void SetPrizeState(LO* plo, PICKUP& p, int n)
{
    if (p.state == n) return;
    if (n == 1)
    {
        if (CSnd* ps = PsndNewPlain(0)) ps->SetSource(nullptr);  // 0x1FBB90 / 0x1FB2F8
        const OID oidTask = static_cast<OID>(plo->U32Field(0, 0x230, static_cast<uint32_t>(-1)));
        if (oidTask != static_cast<OID>(-1)) RequestTaskStateLo(PloFindSwObjectByOid(g_psw, oidTask), 2, plo);
        n = 2;
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);  // vt slot 3 (5, 0, 0, 0, 0)
        if (FPrint()) std::printf("pickup %d: collected (prize, task %d)\n", plo->oid, oidTask);
    }
    SetPickupStateBase(plo, p, n);
}

// orig: 0x001C7EE8 (0x5F, the clue bottle: collected: the +0x210's message or the sound 0x29, the noise 0x156B98,
// the splice event 5; from a bottle proxy (cid 7): the world's afClue +0x100 set (0x1C7C30), the proxy removed,
// the HUD's count shown (0x1618B0); gone)
void SetClueState(LO* plo, PICKUP& p, int n)
{
    if (p.state == n) return;
    if (n == 1)
    {
        if (p.ploCollect)
        {
            // Slot 51's BURSTPARAMS {grf = 1, source = pickup}; an explosion
            // group (0x15C3C0) dispatches it to its child bursts (0x15C690).
            BURSTPARAMS bp;
            bp.grf = 1;
            bp.plo = plo;
            ExplodeLo(p.ploCollect, &bp);
        }
        else if (CSnd* ps = PsndNewPlain(0x29)) ps->SetSource(nullptr);
        n = 2;
        PnoiseNew(PtmplNoiseFixed(0), PosLoWorld(plo), plo);  // 0x156B98(G, 0, position, pickup)
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
        LO* pproxy = PproxyOfInstance(plo);
        if (pproxy && FCidDerivedFrom(pproxy->Cid(), S3CID(7)))
        {
            auto it = s_mpclue.find(pproxy);
            const auto& awrec = AwrecWorlds();
            const int i = g_transition.iwrecCur;
            if (it != s_mpclue.end() && i >= 0 && i < static_cast<int>(awrec.size()))
            {
                std::vector<int16_t> akey(awrec[i].an.begin(), awrec[i].an.end());
                akey.push_back(static_cast<int16_t>(NSymProgress("afClue")));
                akey.push_back(static_cast<int16_t>(it->second));
                CRef r;
                r.SetBool(true);
                FSetProgressKey(akey, r);
            }
            RemoveLo(g_psw, pproxy);  // vt slot 9
            ShowClueCounter();        // 0x1618B0
            if (FPrint()) std::printf("pickup %d: the clue bottle %d collected\n", plo->oid, it != s_mpclue.end() ? it->second : -1);
        }
    }
    SetPickupStateBase(plo, p, n);
}

// orig: 0x001C68F0 (0x5D, the charm: 0 the rest's time; 1 only when the current player can take it (vt slot 80) and
// heals it by +0x23C..+0x240 (slot 81): TODO, taken as yes; the HUD effect (kind 9): TODO; +0x210's message; event 5; gone)
// orig: 0x001C77E8 (0x5E, the loot: 0 the rest's time; 1 the progress loot / mplootkc / "<kind>" + 1 (the kind's entry
// +0x244: TODO); +0x210's message; event 5; gone)
void SetOtherPickupState(LO* plo, PICKUP& p, int n)
{
    if (p.state == n) return;
    switch (n)
    {
    case 0: p.dtRest = GRandInRange(p.dtRestLow, p.dtRestHigh); break;
    case 1:
        if (FIsLoDerivedFrom(plo, S3CID(0x5D)))
        {
            // orig: 0x001C68F0 case 1: the current player (0x1C4840) takes it only when its slot 80 (0x144090) allows
            ACTOR* pa = PactorFromLo(PloPoCurrent(g_psw));
            if (!pa || !FActorTakesCharm(pa)) { n = p.state; break; }
            // the flight to the health meter (0x1CE888(G, 9, 0), +0xF0 D_002C0E10, its arrival 0x1C6AB0 does nothing)
            ALO* palo = static_cast<ALO*>(plo);
            UpdateAloXfWorlds(palo);
            const XF* pxfObj = PxfObj(palo);
            const glm::mat3 mat = pxfObj ? glm::mat3(pxfObj->matWorld) : glm::mat3(1.0f);
            FLaunchHudRip(plo, PosLoWorld(plo), mat, RiphuddefCharm(), [] {});
            // slot 81 (0x144268): health +0x23C (10) and power +0x240 (20), each up to its maximum
            HealActor(pa, static_cast<int>(plo->U32Field(0, 0x23C, 10)), static_cast<int>(plo->U32Field(0, 0x240, 20)));
            if (p.ploCollect)
            {
                BURSTPARAMS bp; bp.grf = 1; bp.plo = plo;
                ExplodeLo(p.ploCollect, &bp);  // +0x210's slot 51 {1, the pickup}
            }
            if (FPrint()) std::printf("pickup %d: the charm taken\n", plo->oid);
        }
        else if (p.iloot >= 0 && p.iloot < static_cast<int>(p.aloot.size()))
        {
            const int index = p.aloot[p.iloot].index;
            const auto path = std::vector<std::string>{"loot", "mplootkc", std::to_string(index)};
            const CRef cur = RefGetProgress(path);
            CRef next; next.SetS32(cur.RefCoerceS32().m_n + 1);
            FSetProgress(path, next);  // orig: 0x1C77E8 / 0x1D8B20
            if (p.ploCollect)
            {
                BURSTPARAMS bp; bp.grf = 1; bp.plo = plo;
                ExplodeLo(p.ploCollect, &bp);
            }
        }
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
        if (FPrint()) std::printf("pickup %d: collected (cid 0x%X)\n", plo->oid, plo->Cid());
        n = 2;
        break;
    default: break;
    }
    SetPickupStateBase(plo, p, n);
}

// orig: 0x001C5B20 (a player's touch: its record +0x15C {on, -, radius +0x164, the object +0x168}: within 300 of its
// centre, then inside its radius, or a sphere entry within the pickup's radius +0x1DC plus its own, or the sphere
// against an entry's geometry (0x1A93C8); entries with bit 50 skipped)
bool FPickupTouched(LO* plo, PICKUP& p, LO* pplayer)
{
    if (!pplayer || !FCidDerivedFrom(pplayer->Cid(), S3CID(0x30))) return false;
    ALO* pa = static_cast<ALO*>(pplayer);
    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(plo));
    const glm::vec3 d = pos - PosAloCenter(pa);
    const float dd = glm::dot(d, d);
    if (300.0f * 300.0f < dd) return false;  // D_002C0D7C
    const float rPlayer = 0.0f;               // (+0x164: 0 for the players seen)
    if (dd < rPlayer * rPlayer) return true;
    for (const SOE& e : pa->asoe)
    {
        if (e.f50) continue;
        if (e.f49)
        {
            const float r = p.sCollect + e.sRadius;
            const glm::vec3 c = e.pxf ? glm::vec3(e.pxf->matWorld[3]) : PosAloCenter(pa);
            const glm::vec3 dc = pos - c;
            if (glm::dot(dc, dc) < r * r) return true;
        }
        else
        {
            std::vector<glm::vec3> apos;
            std::vector<glm::vec4> aplane;
            std::vector<int> akind;
            std::vector<const void*> apv;
            if (CsbiSphereEntry(pos, &e, nullptr, 1, p.sCollect, &apos, &aplane, &akind, &apv) > 0) return true;
        }
    }
    return false;
}

// orig: 0x001C55E0 (the ground: the sphere +0x1D8 at the centre against the world's entries in its box (0x20FAA0 with
// 0x1C5570: not the players' (0x32) or the thrown objects' (0x4F), not bit 48): the deepest contact moving into it; a
// floor (n.z > 0.7) while falling bounces (along: min(+0x1E4 |v|, 300), up: min(-+0x1E0 vz, 600); the first one slow
// against +0x1D0 bounces +0x1D4 up instead, +0x1C4 landed), others reflect (-+0x1E0 normal, +0x1E4 along); pushed out)
void UpdatePickupGround(LO* plo, PICKUP& p)
{
    const float r = p.sRadius;
    const glm::vec3 c = PosAloCenter(static_cast<ALO*>(plo));
    const glm::vec3 mn = c - glm::vec3(r), mx = c + glm::vec3(r);
    float gDeep = 0.0f;
    glm::vec3 n(0.0f);
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* pe : poxa->apsoe)
        {
            if (!pe || pe->f48 || pe->palo == plo) continue;
            if (pe->palo && (FCidDerivedFrom(pe->palo->Cid(), S3CID(0x32)) || FCidDerivedFrom(pe->palo->Cid(), S3CID(0x4F)))) continue;
            if (pe->f49)
            {
                const glm::vec3 cs = pe->pxf ? glm::vec3(pe->pxf->matWorld[3]) : glm::vec3(0.0f);
                const float g = (glm::length(c - cs) - pe->sRadius) - r;
                if (!(g < gDeep)) continue;
                glm::vec3 d = p.pos - cs;
                const float l = glm::length(d);
                d = l < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : d / l;
                if (glm::dot(p.vel, d) < 0.0f)
                {
                    gDeep = g;
                    n = d;
                }
            }
            else if (!pe->bsp.abspn.empty())
            {
                std::vector<glm::vec3> apos;
                std::vector<glm::vec4> aplane;
                std::vector<int> akind;
                std::vector<const void*> apv;
                const int k = CsbiSphereEntry(c, pe, nullptr, 8, r, &apos, &aplane, &akind, &apv);
                for (int i = 0; i < k; i++)
                {
                    const glm::vec3 ni(aplane[i]);
                    const float g = glm::dot(apos[i], ni) - aplane[i].w;
                    if (!(g < gDeep)) continue;
                    if (glm::dot(p.vel, ni) < 0.0f)
                    {
                        gDeep = g;
                        n = ni;
                    }
                }
            }
        }
    }
    if (!(gDeep < 0.0f)) return;
    glm::vec3 v;
    if (0.7f < n.z && p.state == 4)
    {
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        glm::vec3 vAlong;
        const float vz = GSplitVector(p.vel, up, nullptr, &vAlong);  // 0x11E290
        const float l = glm::length(vAlong);
        const glm::vec3 dir = l < 1e-4f ? glm::vec3(0.0f) : vAlong / l;
        const float s = std::min(p.g1E4 * l, 300.0f);   // D_002C0D74
        const float u = std::min(-p.g1E0 * vz, 600.0f); // D_002C0D78
        v = dir * s + up * u;
        if (!p.fLanded && glm::dot(v, n) < p.g1D0)
        {
            v = dir * s + up * p.g1D4;
            p.fLanded = 1;
        }
    }
    else
    {
        glm::vec3 vN, vT;
        GSplitVector(p.vel, n, &vN, &vT);
        v = vN * -p.g1E0 + vT * p.g1E4;
    }
    p.pos += n * -gDeep;
    p.vel = v;
}

// orig: 0x001C5D18 (collected by itself once +0x1CC has passed; the players' touch 0x1C5B20: TODO)
void TestPickupCollect(LO* plo, PICKUP& p)
{
    if (!p.fCollectTimed) return;
    if (p.tCollect < g_clock.t)
    {
        SetPickupState(plo, 1);
        return;
    }
    // the players (0x1C4BD8: the current one; the other players of the gang in the world: TODO), then 0x1C5B20
    if (FPickupTouched(plo, p, PloPoCurrent(g_psw))) SetPickupState(plo, 1);
}

// orig: 0x001C6308 (the glint: the pickup's (1, 1, 0) in its frame (0x1A6FC0) against the camera's left (CM +0x70);
// when that side changes, with the chance +0x1A0 and 1 s (D_002C0D90) since the last: a kind-4 RIP of the model +0x1A8
// at +0x1B0 (the camera looking against the direction) or +0x1B8, for 1.5 / max(1, |the turn's speed z|) s)
void UpdatePickupGlint(LO* plo, PICKUP& p)
{
    if (!p.ploGlint || p.g1A0 <= 0.0f) return;
    ALO* palo = static_cast<ALO*>(plo);
    const XF* pxfObj = PxfObj(palo);
    const glm::vec3 dir = pxfObj ? glm::mat3(pxfObj->matWorld) * glm::vec3(1.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 1.0f, 0.0f);
    const int fSide = glm::dot(dir, g_cm.mat[1]) < 0.0f;
    if (fSide == p.fSidePrev) return;
    if (GRandInRange(0.0f, 1.0f) < p.g1A0 && 1.0f < g_clock.t - p.tGlint)
    {
        LO* ploAt = glm::dot(dir, g_cm.mat[0]) < 0.0f ? p.ploGlintL : p.ploGlintR;
        if (ploAt)
        {
            const XF* pxfRot = PxfRot(palo);
            const float w = pxfRot ? std::fabs(pxfRot->vel.z) : 0.0f;
            if (FLaunchGlintRip(p.ploGlint, ploAt, 1.5f / std::max(1.0f, w))) p.tGlint = g_clock.t;
            if (FPrint())
                std::printf("pickup %d: glint at %d (%.0f %.0f %.0f) t %.3f life %.2f\n", plo->oid, ploAt->oid, PosLoWorld(ploAt).x, PosLoWorld(ploAt).y,
                            PosLoWorld(ploAt).z, g_clock.t, 1.5f / std::max(1.0f, w));
        }
    }
    p.fSidePrev = fSide;
}

// orig: 0x001C5E48 (the base's update: the ALO's, the glint 0x1C6308, then by state)
void UpdatePickupBase(LO* plo, PICKUP& p)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    UpdatePickupGlint(plo, p);
    switch (p.state)
    {
    case 0:
        if (glm::dot(p.vel, p.vel) > 10000.0f) UpdatePickupGround(plo, p);  // 0x1C55E0
        [[fallthrough]];
    case 5: TestPickupCollect(plo, p); break;
    case 3:
        if (p.vel.z < 0.0f) SetPickupState(plo, 4);
        break;
    case 4:
        if (p.fLanded && p.vel.z < 25.0f)
        {
            p.fLanded = 0;
            SetPickupState(plo, 0);
        }
        else if (5.0f < g_clock.t - p.t)  // D_002C0D50
            SetPickupState(plo, 2);
        else
        {
            UpdatePickupGround(plo, p);  // 0x1C55E0
            TestPickupCollect(plo, p);
        }
        break;
    default: break;
    }
}

// orig: 0x001C6588 (the coin's update: the base's, then the rest's time runs out: 5)
void UpdateCoin(LO* plo)
{
    PICKUP* p = Ppickup(plo);
    if (!p)
    {
        if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
        return;
    }
    if (std::getenv("SLY2N_PICKUPMOVE")) std::printf("pickup %d: update state %d grf 0x%X t %.3f at %p\n", plo->oid, p->state, plo->iupd >= 0 ? g_psw->aupd[plo->iupd].grf : plo->grfUpd, g_clock.t, (void*)plo);
    UpdatePickupBase(plo, *p);
    if (p->state == 0)
    {
        p->dtRest -= g_clock.dt;
        if (p->dtRest <= 0.0f) SetPickupState(plo, 5);
    }
}

// orig: 0x001C5E48 (0x5C's update) / 0x001C6880 (0x5D) / 0x001C7778 (0x5E): as the coin's
void UpdatePickupOther(LO* plo)
{
    PICKUP* p = Ppickup(plo);
    if (!p)
    {
        if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
        return;
    }
    UpdatePickupBase(plo, *p);
    // (only 0x1C6880 / 0x1C7778 time the rest out: the prizes 0x5C and the clue bottles 0x5F run the base's 0x1C5E48)
    if (p->state == 0 && (FIsLoDerivedFrom(plo, S3CID(0x5D)) || FIsLoDerivedFrom(plo, S3CID(0x5E))))
    {
        p->dtRest -= g_clock.dt;
        if (p->dtRest <= 0.0f) SetPickupState(plo, 5);
    }
}

// orig: 0x001C5148 (vt slot 17: but held (6), the motion: the velocity damped while resting (0 / 5: 1 - 5 dt), the
// acceleration, the position into the position record with the velocity (stamped); the SM's channels' offsets (the
// bob and the turn toward the camera): TODO; then the ALO's 0x1F3980)
void UpdatePickupMotion(LO* plo)
{
    ALO* palo = static_cast<ALO*>(plo);
    PICKUP* pp = Ppickup(plo);
    if (!pp || pp->state == 6)
    {
        UpdateAloPhys(palo);
        return;
    }
    PICKUP& p = *pp;
    const float dt = g_clock.dt;
    if (XF* pxf = PxfPos(palo); pxf && (!p.fWritten || glm::length(glm::vec3(pxf->mat[3]) - p.posLocal) > 0.01f))
    {
        UpdateAloXfWorlds(palo);
        p.pos = glm::vec3(pxf->matWorld[3]);
        if (p.fWritten) p.vel = glm::vec3(0.0f);
        if (FPrint())
            std::printf("pickup %d (cid 0x%X): %s at (%.0f %.0f %.0f) state %d\n", plo->oid, plo->Cid(), p.fWritten ? "placed" : "first move", p.pos.x, p.pos.y, p.pos.z, p.state);
    }
    if (std::getenv("SLY2N_PICKUPMOVE"))
        std::printf("pickup %d: move state %d pos (%.1f %.1f %.1f) vel z %.1f cam (%.0f %.0f %.0f) view (%.2f %.2f %.2f)\n", plo->oid, p.state, p.pos.x, p.pos.y, p.pos.z,
                    p.vel.z, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z);
    const float k = (p.state == 0 || p.state == 5) ? -5.0f * dt + 1.0f : 1.0f;  // D_002C0D4C
    p.vel = p.vel * k + p.acc * dt;
    p.pos = p.pos + p.vel * dt;
    if (XF* pxf = PxfPos(palo))
    {
        // (+0x170 / +0x180 are the world's; the record's local frame is its reference's when it keeps one)
        glm::vec3 pos = p.pos, vel = p.vel;
        if (pxf->pxfRef)
        {
            const glm::mat4 mInv = glm::inverse(pxf->pxfRef->matWorld);
            pos = glm::vec3(mInv * glm::vec4(pos, 1.0f));
            vel = glm::vec3(mInv * glm::vec4(vel, 0.0f));
        }
        pxf->mat[3] = glm::vec4(pos, 1.0f);  // 0x256D18
        p.posLocal = pos;
        p.fWritten = true;
        pxf->vel = glm::vec4(vel, 0.0f);
        pxf->nStamp = ++g_cXfStamp;
    }
    // the SM's playing transition: its channels of the object's own position / rotation records that the ASEG does not
    // play itself (bit 30), evaluated at its time in the frame turned toward the camera (CM +0x90, flat): the offset
    // added to the position, the rotation (Euler, 0x11B628) times the frame, the velocities turned
    if (p.psma && p.psma->pasega && p.psma->pasega->paseg)
    {
        ASEGA* pa = p.psma->pasega;
        XF* pxf58 = PxfPos(palo);
        XF* pxf5C = PxfRot(palo);
        const ASEGCHN* pchnPos = nullptr;
        const ASEGCHN* pchnRot = nullptr;
        for (const ASEGCHN& chn : PasegdOf(pa->paseg)->achn)
        {
            if (!chn.fDisabled || chn.fScalar || chn.oid != plo->oid || !chn.ptrkp) continue;
            XF* px = PxfFindAlo(palo, chn.isub);  // vt slot 51
            if (px && px == pxf58) pchnPos = &chn;
            else if (px && px == pxf5C) pchnRot = &chn;
            if (pchnPos && pchnRot) break;
        }
        if (std::getenv("SLY2N_PICKUPMOVE"))
            for (const ASEGCHN& chn : PasegdOf(pa->paseg)->achn)
            {
                XF* px = PxfFindAlo(palo, chn.isub);
            {
                glm::vec4 v{0.0f}, dv{0.0f};
                if (chn.ptrkp) chn.ptrkp->Eval(&v, &dv, pa->t, pa->svt);
                std::printf("  chn oid %d isub %d disabled %d scalar %d trk %d xf %s (kind %d) v (%.3f %.3f %.3f)\n", chn.oid, chn.isub, chn.fDisabled, chn.fScalar,
                            chn.ptrkp ? 1 : 0, px == PxfPos(palo) ? "pos" : px == PxfRot(palo) ? "rot" : px ? "other" : "none", px ? px->kind : -1, v.x, v.y, v.z);
            }
            }
        if (std::getenv("SLY2N_PICKUPMOVE"))
            for (int isub = 0; isub < 4; isub++)
                if (XF* px = PxfFindAlo(palo, isub))
                    std::printf("  xf %d kind %d world scale %.3f %.3f %.3f local scale %.3f %.3f %.3f\n", isub, px->kind, glm::length(glm::vec3(px->matWorld[0])),
                                glm::length(glm::vec3(px->matWorld[1])), glm::length(glm::vec3(px->matWorld[2])), glm::length(glm::vec3(px->mat[0])),
                                glm::length(glm::vec3(px->mat[1])), glm::length(glm::vec3(px->mat[2])));
        if (std::getenv("SLY2N_PICKUPMOVE"))
            std::printf("pickup %d: sm state %d t %.3f chn pos %d rot %d (of %zu) sma cur %d next %d goal %d aseg %d tMax %.2f\n", plo->oid, p.state, pa->t, pchnPos ? 1 : 0, pchnRot ? 1 : 0,
                        PasegdOf(pa->paseg)->achn.size(), OidSmaCur(p.psma), p.psma->ismsNext, OidSmaGoal(p.psma), pa->paseg->oid, PasegdOf(pa->paseg)->tMax);
        if ((pchnPos || pchnRot) && pxf58)
        {
            glm::vec3 d = g_cm.pos - glm::vec3(pxf58->mat[3]);
            d.z = 0.0f;
            const glm::mat3 m = BuildLookAtMatrix(d, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x11B830 (rows: glm columns)
            if (pchnPos)
            {
                glm::vec4 v{0.0f}, dv{0.0f};
                pchnPos->ptrkp->Eval(&v, &dv, pa->t, pa->svt);
                pxf58->mat[3] = glm::vec4(glm::vec3(pxf58->mat[3]) + m * glm::vec3(v), 1.0f);  // 0x256D18
                pxf58->vel += glm::vec4(m * glm::vec3(dv), 0.0f);
                pxf58->nStamp = ++g_cXfStamp;
                p.posLocal = glm::vec3(pxf58->mat[3]);
            }
            if (pchnRot && pxf5C)
            {
                glm::vec4 v{0.0f}, dv{0.0f};
                pchnRot->ptrkp->Eval(&v, &dv, pa->t, pa->svt);
                const glm::mat3 r = m * cmm::BuildEulerMatrix(glm::vec3(v));  // 0x11B628
                pxf5C->mat[0] = glm::vec4(r[0], 0.0f);
                pxf5C->mat[1] = glm::vec4(r[1], 0.0f);
                pxf5C->mat[2] = glm::vec4(r[2], 0.0f);
                pxf5C->vel = glm::vec4(m * glm::vec3(dv), 0.0f);
                pxf5C->nStamp = ++g_cXfStamp;
            }
        }
    }
    UpdateAloPhys(palo);
    // (the placement check above compares against the record as the object's own update left it: a pool instance given
    // back (0x1CAA60's slot 35) keeps its position record in velocity mode 1, so 0x1F3980 moves it on by its velocity
    // after the write. The original overwrites the record from +0x170 every frame and never reads it back; taking that
    // step for a placement from outside zeroed a reused coin's launch velocity: they dropped straight down in a heap
    // (R11).)
    if (XF* pxf = PxfPos(palo); pxf && p.fWritten) p.posLocal = glm::vec3(pxf->mat[3]);
    // (natively the records' update 0x22DE88 lets the flag 4 sleep since the position record is in no moving mode;
    // the pickup moves itself while flying / settling: kept awake. TODO: the reference's record mode of a coin)
    if (p.state == 0 || p.state == 3 || p.state == 4 || p.state == 5) SetGrfUpdOfLo(plo, GrfUpdOfLo(plo) | 4u);
}
}  // namespace

bool FPickupLive(LO* plo) { return Ppickup(plo) != nullptr; }

// orig: 0x001C7A58 (select a loot definition by id, defaulting to the first;
// hide all mesh groups, then show the selected definition's groups).
void SetPickupLootKind(LO* plo, int id)
{
    PICKUP* p = Ppickup(plo);
    if (!p) p = &InitPickup(plo);
    p->iloot = p->aloot.empty() ? -1 : 0;
    for (size_t i = 0; i < p->aloot.size(); ++i)
        if (p->aloot[i].id == id) { p->iloot = static_cast<int>(i); break; }
    ALO* palo = static_cast<ALO*>(plo);
    if (palo->pglobset)
        for (const GLOB& glob : palo->pglobset->aglob) ShowAloGlobs(palo, glob.n0, false);  // 0x1F6A68
    if (p->iloot >= 0)
        for (int oid : p->aloot[p->iloot].aglob) ShowAloGlobs(palo, oid, true);  // 0x1F6978
    if (FPrint()) std::printf("pickup %d: loot id %d index %d\n", plo->oid,
                             p->iloot < 0 ? -1 : p->aloot[p->iloot].id,
                             p->iloot < 0 ? -1 : p->aloot[p->iloot].index);
}

void SetPickupState(LO* plo, int n)
{
    PICKUP* p = Ppickup(plo);
    if (!p) p = &InitPickup(plo);
    if (FIsLoDerivedFrom(plo, S3CID(0x5B))) SetCoinState(plo, *p, n);
    else if (FIsLoDerivedFrom(plo, S3CID(0x5C))) SetPrizeState(plo, *p, n);
    else if (FIsLoDerivedFrom(plo, S3CID(0x5F))) SetClueState(plo, *p, n);
    else SetOtherPickupState(plo, *p, n);
}

void LaunchPickup(LO* plo, const glm::vec3& pos, const glm::vec3& vel, float dtSm, float dtCollect, LO* pproxy)
{
    ALO* palo = static_cast<ALO*>(plo);
    PICKUP* p = Ppickup(plo);
    if (!p) p = &InitPickup(plo);
    if (pproxy) p->pproxy = pproxy;
    AddLo(g_psw, plo);                                  // vt slot 8
    SetLoParentNone(plo);                               // 0x1A58D0(p, 0, 0)
    // (natively a reused pool instance's records can hold world matrices from its last life (its position record's
    // local was reset while removed); slot 29 solves the new local against them, so a coin thrown again landed far away,
    // never activated and stayed frozen (N10). The original's records are always current: refresh them first.)
    UpdateAloXfWorldsHierarchy(palo);
    SetAloPos(palo, pos);                               // vt slot 29
    SetAloRotation(palo, glm::mat3(1.0f));              // vt slot 30 (D_002A2D00)
    if (XF* pxf = PxfPos(palo)) pxf->vel = glm::vec4(0.0f);
    if (XF* pxf = PxfRot(palo)) pxf->vel = glm::vec4(0.0f);
    // (its update: the class's slot 16 (InitLo's flag; natively the proxies' instances do not get it))
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
    p->dtSm = dtSm;
    p->fLanded = 0;
    SetPickupState(plo, 3);
    p->pos = pos;  // (the record's translation in the world frame: the instance's record keeps its reference)
    p->fWritten = false;  // (the motion takes it from the record, placed there by slot 29)
    p->vel = vel;
    p->tCollect = dtCollect == FLT_MAX ? FLT_MAX : g_clock.t + dtCollect;  // (D_002A7758)
    if (FPrint())
        std::printf("pickup %d: iupd %d grf 0x%X/0x%X st %d inworld %d ref %d\n", plo->oid, plo->iupd, plo->iupd >= 0 ? g_psw->aupd[plo->iupd].grf : 0,
                    plo->grfUpd, plo->st, plo->fInWorld, PxfPos(palo) && PxfPos(palo)->pxfRef ? 1 : 0);
    if (FPrint() && PxfPos(palo))
        for (XF* pr = palo->axf.empty() ? nullptr : palo->axf[0]->pxfRef; pr; pr = pr->pxfRef)
            std::printf("pickup %d: ref xf of %d (cid 0x%X) kind %d scale %.3f local %.3f\n", plo->oid, pr->palo ? pr->palo->oid : -1, pr->palo ? pr->palo->Cid() : -1, pr->kind,
                        glm::length(glm::vec3(pr->matWorld[0])), glm::length(glm::vec3(pr->mat[0])));
    if (FPrint())
    {
        const glm::vec3 w = PosLoWorld(plo);
        std::printf("pickup %d: launched from (%.1f %.1f %.1f) vel (%.1f %.1f %.1f) collect %.3f world (%.0f %.0f %.0f) pos58 %d obj %d\n", plo->oid, pos.x, pos.y, pos.z, vel.x, vel.y,
                    vel.z, p->tCollect, w.x, w.y, w.z, PxfPos(palo) ? 1 : 0, PxfObj(palo) ? 1 : 0);
    }
}

// ---- The drops (the breakables' coins and charms)

namespace
{
float TJumpBallisticPk(const glm::vec3& d, const glm::vec3& g, float vzMax, float vzMin, float sMax, float sMin);

// The world's solids in a box for the pickups (0x20FAA0 with the filter 0x1C5570: not bit 48, not the players' (0x32)
// or the thrown objects' (0x4F) entries)
std::vector<SOE*> AsoeSolidsIn(const glm::vec3& a, const glm::vec3& b)
{
    std::vector<SOE*> apsoe;
    const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
    for (OXA* poxa : g_physw.apoxaActive)
    {
        if (mx.x < poxa->posMin.x || poxa->posMax.x < mn.x || mx.y < poxa->posMin.y || poxa->posMax.y < mn.y ||
            mx.z < poxa->posMin.z || poxa->posMax.z < mn.z)
            continue;
        for (SOE* pe : poxa->apsoe)
        {
            if (!pe || pe->f48) continue;
            if (pe->palo && (FCidDerivedFrom(pe->palo->Cid(), S3CID(0x32)) || FCidDerivedFrom(pe->palo->Cid(), S3CID(0x4F)))) continue;
            if (apsoe.size() < 0x100) apsoe.push_back(pe);
        }
    }
    return apsoe;
}
}  // namespace

// orig: 0x001C6DE8 (a floor near pos: the segment up gUp (inside a solid: its exit point unless it never leaves or the
// exit's plane faces down past nzMin), then down gDown (its entry point when the entry's plane faces up past nzMin, not
// from inside); without a down part pos itself)
bool FFindPickupFloor(const glm::vec3& pos, std::vector<SOE*>* papsoe, float gDown, float gUp, float nzMin, glm::vec3* pposOut)
{
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 posDown = pos + up * gDown, posUp = pos + up * gUp;
    std::vector<SOE*> aown;
    if (!papsoe || papsoe->empty())
    {
        aown = AsoeSolidsIn(pos, posUp);
        papsoe = &aown;
    }
    LSG lsg;
    if (0.0f < gUp && PsoeHitTestLineObjects(0, pos, posUp, *papsoe, &lsg) && lsg.t0 == 0.0f)
    {
        if (FPrint()) std::printf("drops: up-cast from (%.3f %.3f %.3f) inside: t1 %g pos1 z %.3f\n", pos.x, pos.y, pos.z, lsg.t1, lsg.pos1.z);
        if (std::getenv("SLY2N_PICKUPHEX"))
            std::printf("drops hex: pos %a %a %a up %a %a %a t1 %a plane1 %a %a %a %a pos1 %a %a %a\n", pos.x, pos.y, pos.z, posUp.x, posUp.y,
                        posUp.z, lsg.t1, lsg.plane1.x, lsg.plane1.y, lsg.plane1.z, lsg.plane1.w, lsg.pos1.x, lsg.pos1.y, lsg.pos1.z);
        if (lsg.t1 == 1.0f || lsg.plane1.z < nzMin) return false;
        *pposOut = lsg.pos1;
        return true;
    }
    if (gDown < 0.0f)
    {
        if (!PsoeHitTestLineObjects(0, pos, posDown, *papsoe, &lsg)) return false;
        if (FPrint()) std::printf("drops: down-cast from (%.3f %.3f %.3f): t0 %g t1 %g pos0 z %.3f n0 z %.3f\n", pos.x, pos.y, pos.z, lsg.t0, lsg.t1, lsg.pos0.z, lsg.plane0.z);
        if (std::getenv("SLY2N_PICKUPHEX"))
            std::printf("drops hex: down pos %a %a %a t0 %a t1 %a plane0 %a %a %a %a plane1 %a %a %a %a\n", pos.x, pos.y, pos.z, lsg.t0, lsg.t1,
                        lsg.plane0.x, lsg.plane0.y, lsg.plane0.z, lsg.plane0.w, lsg.plane1.x, lsg.plane1.y, lsg.plane1.z, lsg.plane1.w);
        if (lsg.t0 == 0.0f || lsg.plane0.z < nzMin) return false;
        *pposOut = lsg.pos0;
        return true;
    }
    *pposOut = pos;
    return true;
}

// orig: 0x001C6CE8 (n instances of the kind's proxy (0x5B coins 0x2B3, 0x5D charms 0x2B4): attached, put away (2), kept)
int CpickupInstances(int cid, int n, std::vector<LO*>* paplo)
{
    const OID oid = cid == S3CID(0x5B) ? 0x2B3 : cid == S3CID(0x5D) ? 0x2B4 : -1;
    if (!n || oid == -1) return 0;
    LO* pproxy = PloFindSwObjectByOid(g_psw, oid);  // 0x15E938 (5)
    if (!pproxy) return 0;
    for (int i = 0; i < n; i++)
    {
        LO* plo = PloAcquireProxyObject(g_psw, pproxy, false);  // 0x1CB250(proxy, 0): one-object rows, score >= 2
        if (!plo) break;
        SetPickupState(plo, 2);                    // vt slot 76
        if (PICKUP* p = Ppickup(plo)) p->pproxy = pproxy;
        LockProxyRowOf(g_psw, pproxy, plo, true);  // 0x1CB488
        paplo->push_back(plo);
    }
    if (FPrint())
        if (auto it = g_psw->mpproxy.find(pproxy); it != g_psw->mpproxy.end() && it->second.iwe >= 0)
        {
            const WE& we = g_psw->awe[it->second.iwe];
            int cLocked = 0;
            for (uint8_t f : we.afRowLocked) cLocked += f ? 1 : 0;
            std::printf("drops: proxy %d instances %d of %d asked: group rows %zu (from %d), %d locked\n", oid, int(paplo->size()), n,
                        we.aploRowOwner.size(), we.cUsed, cLocked);
        }
    return static_cast<int>(paplo->size());
}

// orig: 0x001C6FD8 (n pickups of the kind thrown from pos onto the walkable floor around it: the floor under pos, the
// nav point near it (the player's region; within 2000), the floor there; then up to 50 tries on a ring (angle random
// + 1.8 a try, radius 100 + 23 a try (past 300 back by 275), the lead clamp(200 - 5 try, 0, 150)), the nav segments clear
// (0x1C0980), a floor (0x1C6DE8(-300, 300, 0.7)): each pickup thrown there (TJumpBallistic + i 0.075 s, its SM's delay
// (i + 1) 0.1); the ones left put away)
void SpawnPickups(int cid, const glm::vec3& pos, LO* psrc, int n)
{
    (void)psrc;
    glm::vec3 pos0;
    auto fail = [&](const char* why) {
        if (FPrint()) std::printf("drops: none (%s) from (%.0f %.0f %.0f)\n", why, pos.x, pos.y, pos.z);
    };
    if (!FFindPickupFloor(pos, nullptr, 0.0f, 300.0f, -FLT_MAX, &pos0)) return fail("no floor");
    LO* pcur = PloPoCurrent(g_psw);  // 0x1C4840
    const glm::vec3 posCur = pcur && FCidDerivedFrom(pcur->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(pcur)) : pos0;
    NAVREGION* preg = pcur ? PregionAt(posCur, false) : nullptr;  // (the player's area record's region)
    if (!preg) preg = PregionAt(pos0, true);                       // 0x1C0D70(pos, 1)
    glm::vec3 posNav;
    if (FNavPoint(preg, pos0, &posNav, 1)) posNav.z = pos0.z;  // 0x1C0940
    else if (FNavPoint(preg, posCur, &posNav, 1)) posNav.z = posCur.z;
    else return fail(preg ? "no nav point" : "no region");
    const glm::vec2 dxy(pos0.x - posNav.x, pos0.y - posNav.y);
    if (2000.0f < glm::length(dxy)) return fail("nav point far");  // D_002C0E80
    if (FPrint()) std::printf("drops: floor (%.1f %.1f %.1f) nav (%.1f %.1f %.1f) player (%.0f %.0f %.0f)\n", pos0.x, pos0.y, pos0.z, posNav.x, posNav.y, posNav.z, posCur.x, posCur.y, posCur.z);
    if (!FFindPickupFloor(posNav, nullptr, -250.0f, 1500.0f, -FLT_MAX, &posNav)) return fail("no floor at the nav point");
    std::vector<LO*> aplo;
    const int c = CpickupInstances(cid, n, &aplo);
    if (!c) return fail("no instances");
    std::vector<SOE*> apsoe = AsoeSolidsIn(posNav - glm::vec3(300.0f), posNav + glm::vec3(300.0f));
    const glm::vec3 up(0.0f, 0.0f, 1.0f), g(0.0f, 0.0f, -2250.0f);
    float rad = GRandInRange(-3.1415927f, 3.1415927f);
    float s = 100.0f;  // D_002C0E8C
    int cPlaced = 0;
    for (int iTry = 0; iTry < 50 && cPlaced < c; iTry++)
    {
        const float u = static_cast<float>(iTry) * 0.02f;
        const float sLead = std::clamp(200.0f + u * (-250.0f + u * 0.0f), 0.0f, 150.0f);  // D_002C0EA0 .. D_002C0EB4
        const glm::vec3 p5 = posNav + cmm::SetVectorCylind(rad, s, 0.0f);
        glm::vec3 d = p5 - pos0;
        d -= up * glm::dot(d, up);
        const float l = glm::length(d);
        const glm::vec3 dir = l < 1e-4f ? glm::vec3(1.0f, 0.0f, 0.0f) : d / l;
        const glm::vec3 pLead = p5 + dir * sLead;
        const bool fClear = !FNavBlocked(preg, posNav, p5, 0) && !FNavBlocked(preg, p5, pLead, 0);  // 0x1C0980
        glm::vec3 posFloor;
        if (fClear && FFindPickupFloor(p5, &apsoe, -300.0f, 300.0f, 0.7f, &posFloor))
        {
            LO* plo = aplo[static_cast<size_t>(cPlaced)];
            PICKUP* pp = Ppickup(plo);
            const glm::vec3 posLand = posFloor + up * ((pp ? pp->sRadius : 45.0f) - 1.0f);
            float t = TJumpBallisticPk(posLand - pos0, g, 850.0f, -850.0f, 1.0f, 1.0f);  // 0x196928
            t += static_cast<float>(cPlaced) * 0.075f;  // D_002C0EC4
            cPlaced++;
            const glm::vec3 vel = (posLand - pos0) * (1.0f / t) + g * (t * -0.5f);
            LaunchPickup(plo, pos0, vel, static_cast<float>(cPlaced) * 0.1f, FLT_MAX, nullptr);  // 0x1C6BD0
        }
        rad = cmm::RadNormalize(rad + 1.8f);  // D_002C0E94, 0x11CF60
        s += 23.0f;                            // D_002C0E90
        if (300.0f < s) s -= 300.0f - 25.0f;   // D_002C0E84 / D_002C0E88
    }
    for (int i = cPlaced; i < c; i++) SetPickupState(aplo[static_cast<size_t>(i)], 2);
    if (FPrint()) std::printf("drops: %d of %d (cid 0x%X) from (%.0f %.0f %.0f)\n", cPlaced, c, cid, pos0.x, pos0.y, pos0.z);
}

// orig: 0x00154E10 (D_002DE578: the world's drop counters: coins +0x0 and charms +0x4 drawn from the table D_002BDC00
// by the progress fraction 0x154C90 (TODO: the three progress values; 0 natively: only the charms' range depends on it);
// a hit counts -1 (a guard's -2) and dn; n < 0: NRandInRange(4, 6) when the coins' counter ran out, else (2, 3); the
// coins, and a charm when its counter ran out)
void DropPickups(const glm::vec3& pos, int n, int dn, LO* psrc)
{
    static int s_cCoin = 0, s_cCharm = 0;  // (D_002DE578 +0x0 / +0x4)
    static const int s_atbl[] = {4, 6, 2, 3, 4, 6, 4, 6, 600, 800, 300, 600};  // (+0x10..+0x1C, +0x28..+0x44)
    auto nlerp = [](float g, int a, int b) {
        if (a == b) return b;
        if (a < b) return a + static_cast<int>(g * ((static_cast<float>(b) + 0.999f) - static_cast<float>(a)));
        return b + static_cast<int>((1.0f - g) * ((static_cast<float>(a) + 0.999f) - static_cast<float>(b)));
    };
    const float g = GSuck();  // 0x154C90 (task.cpp)
    if (s_cCoin <= 0) s_cCoin = NRandInRangePk(nlerp(g, s_atbl[4], s_atbl[6]), nlerp(g, s_atbl[5], s_atbl[7]));
    if (s_cCharm <= 0) s_cCharm = NRandInRangePk(nlerp(g, s_atbl[8], s_atbl[10]), nlerp(g, s_atbl[9], s_atbl[11]));
    s_cCoin += psrc && FIsLoDerivedFrom(psrc, S3CID(0x38)) ? -2 : -1;
    s_cCharm += dn;
    if (n < 0) n = s_cCoin <= 0 ? NRandInRangePk(s_atbl[0], s_atbl[1]) : NRandInRangePk(s_atbl[2], s_atbl[3]);
    if (FPrint()) std::printf("drops: %d coins (counters %d / %d)\n", n, s_cCoin, s_cCharm);
    if (n > 0) SpawnPickups(0x5B, pos, psrc, n);
    if (s_cCharm <= 0) SpawnPickups(0x5D, pos, psrc, 1);
}

namespace
{
float TJumpBallisticPk(const glm::vec3& d, const glm::vec3& g, float vzMax, float vzMin, float sMax, float sMin)
{
    return TJumpBallistic(d, g, vzMax, vzMin, sMax, sMin);
}
}  // namespace

void ResetPickups()
{
    s_mppickup.clear();
    s_mpclue.clear();
}
void SetClueIndex(LO* pproxy, int n)
{
    s_mpclue[pproxy] = n;
    if (FPrint())
    {
        const glm::vec3 c(static_cast<const LOX*>(pproxy)->mat[3]);
        std::printf("clue %d: proxy %d at (%.0f %.0f %.0f) inworld %d\n", n, pproxy->oid, c.x, c.y, c.z, pproxy->fInWorld);
    }
}

namespace
{
// orig: 0x001C7E68: the clue instance's proxy-placement notification.
// 0x1CB518's inherited mask copy is performed by AttachProxy before slot 26.
// The same pooled model can represent a different bottle after streaming.
void ResetCluePlacement(LO* plo, LO* pproxy)
{
    PICKUP* p = Ppickup(plo);
    if (!p) p = &InitPickup(plo);
    p->pproxy = pproxy;
    p->fWritten = false;
    SetPickupState(plo, 0);
}

// orig: 0x001C7EA8: the inherited 0x1CB5D0 resets an installed physics pose
// and notifies children (the bottle's explosion children use the no-op
// 0x1CB5C8). Reset this pickup before its next proxy placement; removal and
// physics unregistration follow in DetachProxy/RemoveLo.
void DetachCluePlacement(LO* plo, LO*)
{
    DetachAloBase27(plo, nullptr);  // (0x1C7EA8 -> 0x1CB5D0 first; audit L1)
    SetPickupState(plo, -1);
}

// orig: 0x001C5098 (the pickup's slot 5: its own warning animation reaches
// state 0x186, then the authored receiver is notified and the pickup removed).
void OnPickupMessage(LO* plo, int msgk, void* pv)
{
    if (msgk != 10 || !pv) return;  // base 0x00133590 is empty
    PICKUP* p = Ppickup(plo);
    const SMAMSG10* pm = static_cast<const SMAMSG10*>(pv);
    if (!p || pm->psma != p->psma || pm->oidNew != 0x186) return;
    if (p->ploExpire)
    {
        BURSTPARAMS bp;
        bp.grf = 1;
        bp.plo = plo;
        ExplodeLo(p->ploExpire, &bp);
    }
    SetPickupState(plo, 2);
}

void SetPickupStateVt(LO* plo, int n) { SetPickupState(plo, n); }
struct INITPICKUP
{
    INITPICKUP()
    {
        RegisterLoUpdate(0x1C6588, UpdateCoin);
        for (uint32_t pfn : {0x1C5E48u, 0x1C6880u, 0x1C7778u}) RegisterLoUpdate(pfn, UpdatePickupOther);
        // orig: 0x200BA0 / 0x200C28, EOPID 897: +0x150 / vt slot 76.
        RegisterObjectGetter(897, [](LO* plo) { CRef r; PICKUP* p = Ppickup(plo); r.SetS32(p ? p->state : -1); return r; });
        RegisterObjectSetter(897, [](LO* plo, const CRef& r) { SetPickupState(plo, r.RefCoerceS32().m_n); });
        RegisterObjectGetter(910, [](LO* plo) {  // orig: 0x1C7A40
            CRef r; PICKUP* p = Ppickup(plo);
            r.SetS32(p && p->iloot >= 0 ? p->aloot[p->iloot].id : -1); return r;
        });
        RegisterObjectSetter(910, [](LO* plo, const CRef& r) { SetPickupLootKind(plo, r.RefCoerceS32().m_n); });
        RegisterObjectGetter(911, [](LO* plo) {  // orig: 0x1C7B10 + field 4
            CRef r; PICKUP* p = Ppickup(plo);
            r.SetS32(p && p->iloot >= 0 ? p->aloot[p->iloot].index : -1); return r;
        });
    }
} s_initpickup;
}  // namespace
NATIVEFN(0x1C5148, UpdatePickupMotion);
NATIVEFN(0x1C5098, OnPickupMessage);
NATIVEFN(0x1C65F8, SetPickupStateVt);
NATIVEFN(0x1C6B28, SetPickupStateVt);
NATIVEFN(0x1C68F0, SetPickupStateVt);
NATIVEFN(0x1C77E8, SetPickupStateVt);
NATIVEFN(0x1C7EE8, SetPickupStateVt);
NATIVEFN(0x1C7E68, ResetCluePlacement);
NATIVEFN(0x1C7EA8, DetachCluePlacement);

// orig: 0x001C4F60 (the post-load: the child 0x12 out, the sparkle's links (TODO), then the state +0x158; the update
// flag 2 and the records' flag 4 for the motion)
void PostLoadPickup(LO* plo)
{
    for (LO* pc : plo->aploOwned)
        if (FCidDerivedFrom(pc->Cid(), S3CID(0x12))) { RemoveLo(g_psw, pc); break; }
    const int n = static_cast<int>(plo->U32Field(0, 0x158, 0));
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
    SetPickupState(plo, n);
    if (FPrint() && static_cast<ALO*>(plo)->pglobset)
        for (const GLOB& g : static_cast<ALO*>(plo)->pglobset->aglob)
            std::printf("pickup %d glob: meshk %d rp %d fx %zu cel %zu rtck %d radius %.1f center (%.1f %.1f %.1f) owner %d matrix %d scale %.3f vtx %zu\n", plo->oid, int(g.meshk), int(g.rp), g.asubfx.size(), g.asubcel.size(), g.rtck, g.sRadius,
                        g.posCenter.x, g.posCenter.y, g.posCenter.z, g.oidOwner, g.fMatrix, glm::length(glm::vec3(g.dmat[0])), g.avtx.size());
    if (FPrint())
    {
        const glm::vec3 c = PosAloCenter(static_cast<ALO*>(plo));
        std::printf("pickup %d (cid 0x%X): post-load state %d at (%.0f %.0f %.0f) inworld %d clone %d\n", plo->oid, plo->Cid(), n, c.x, c.y, c.z,
                    plo->fInWorld, plo->ploCloneOf != nullptr);
    }
}

// 0x1A58D0(p, 0, 0) for any ALO: out of its parent, its root record made a root (world position kept, unrotated and
// unscaled, as the reference's); also the interaction targets' aura (jt.cpp 0x1956D0)
void SetLoParentNoneRoot(LO* plo) { SetLoParentNone(plo); }

// The selected loot definition's index (+0x244's +4: the loot table 0x2C3288's row), -1 without one (screenrt.cpp's
// 0x211420: the title card's loot name)
int IlootIndexOfPickup(LO* plo)
{
    const PICKUP* p = plo ? Ppickup(plo) : nullptr;
    return p && p->iloot >= 0 && p->iloot < static_cast<int>(p->aloot.size()) ? p->aloot[p->iloot].index : -1;
}
