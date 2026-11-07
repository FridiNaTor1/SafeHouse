// booty.cpp: carrying (booty.h). Ported from the asm (0x13C560, 0x143678, 0x143410, 0x143AF0, 0x13D328) and the decomp's
// src/{auto,hand} (0x13BDF0, 0x13BF50, 0x13C000, 0x13C1B8, 0x13C278, 0x13C2D8, 0x13C368, 0x13C3B8, 0x13C448, 0x13C490,
// 0x13C4E0, 0x13CEB8, 0x13CEF8, 0x13CF38, 0x13CFD8, 0x13D020, 0x13D1A8, 0x13D248, 0x1432E0, 0x143380, 0x1433C8,
// 0x143BD8).
#include "booty.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "region.h"
#include "screenrt.h"
#include "emitter.h"
#include "animfind.h"

#include "activate.h"
#include "actor.h"
#include "ainoise.h"
#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "glob.h"
#include "hudwidget.h"
#include "jt.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "ref.h"
#include "savedata.h"
#include "so.h"

#include <glm/gtc/matrix_transform.hpp>
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "update.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "xf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

void UseObjectBaseBy(LO* puse, ACTOR* pa);
float GRandInRange(float gLow, float gHigh);  // xfmath.h (0x11D448)  // interact.cpp: the use's base vt slot 51 (0x184130)

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_BOOTYPRINT") != nullptr;
    return s_f;
}

void WarnOnce(const std::string& what)
{
    static std::map<std::string, bool> s_mp;
    if (!s_mp[what])
    {
        std::fprintf(stderr, "booty: %s not ported (stubbed)\n", what.c_str());
        s_mp[what] = true;
    }
}

// A record (orig 0x2C bytes at +0x154, added by the load's EOPID 1152: 0x13C3B8)
struct BOOTYREC
{
    int id = 0;        // +0x00 (the save's "oidBooty" / "aoidAssembly" value)
    int n4 = -1;       // +0x04 the loot kind (the progress loot mplootkc "<n4>" counts it when dropped), -1 none
    int f8 = 0;        // +0x08 fragile: any damage drops it (0x1A19A0) (EOPID 1162)
    int fC = 0;        // +0x0C timed: carried for the stand's +0x18C seconds at most (the HUD timer) (1161)
    int f10 = 0;       // +0x10 may be taken during a job (0x13D020) (1163)
    int n14 = -1;      // +0x14 the stands it may be put on (their +0x188)
    int n18 = -1;      // +0x18 its collision entries' tag (1155)
    int c = 0;         // +0x1C the groups shown (at most 3: 0x13C448, EOPID 1154)
    int a[3]{};        // +0x20
};

// The records live in the object loaded with the options; its proxy's instances (the same oid) share them (the
// original's clones copy the pointer +0x154).
struct RECS
{
    SW* psw = nullptr;
    std::unordered_map<OID, std::vector<BOOTYREC>> mp;
} s_recs;
WORLD_TABLE(s_recs);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::vector<BOOTYREC>& Arec(const LO* plo)
{
    if (s_recs.psw != g_psw)
    {
        s_recs.mp.clear();
        s_recs.psw = g_psw;
    }
    return s_recs.mp[plo->oid];
}

struct STAND;

struct BOOTY
{
    int irec = -1;              // +0x158 the current record (an index into the oid's records; -1 none)
    LO* pstand = nullptr;       // +0x15C its stand (cid 0x25)
    ACTOR* pcarrier = nullptr;  // +0x160
    glm::vec3 v170{1.0f};       // +0x170 the scale carried (0x13BDF0: (1, 1, 1); the load's field)
    float g180 = 0.0f;          // +0x180 the time the carried scale is reached in
    int state = -1;             // +0x184 (0x13BDF0: -1)
    float t = 0.0f;             // +0x188 the state's start
    // +0x18C: the original's flight (a RIP of kind 6, 0x1CE888(G, 6, 0): the model from posFrom to the target object
    // +0x1C in +0x18 = max(250, d) x 0.002 s, accelerated by +0xA0 (0, 0, -3000); its end is message 8 to the booty).
    // The booty itself is already at its destination (0x13C560 places it there when the flight starts: the holder, or
    // the stand); natively only its drawing flies (posDraw, FBootyDrawDelta: a parabola reaching the target's position
    // at the end).
    bool fFlight = false;
    glm::vec3 posFrom{0.0f};
    glm::vec3 posDraw{0.0f};
    LO* ploTo = nullptr;
    float tFlight = 0.0f, dtFlight = 0.0f;
};
std::unordered_map<const LO*, BOOTY> s_mpbooty;
WORLD_TABLE(s_mpbooty);   // (PC: moved aside while a kept world loads, worldglobals.h)

struct STAND
{
    int id = 0;               // +0x180 (0x13CEF8: the world's counter SW +0x463C)
    LO* pbooty = nullptr;     // +0x194
    NOISE* pnoise = nullptr;  // +0x198 the noise of a theft (0x156C80(G, 1, ...))
};
std::unordered_map<const LO*, STAND> s_mpstand;
WORLD_TABLE(s_mpstand);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::unordered_map<const SW*, int> s_mpcstand;     // SW +0x463C
WORLD_TABLE(s_mpcstand);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::unordered_map<const ACTOR*, LO*> s_mpcarried;  // ACTOR +0x11A8
WORLD_TABLE(s_mpcarried);   // (PC: moved aside while a kept world loads, worldglobals.h)

BOOTY* Pbooty(const LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, S3CID(0x67))) return nullptr;
    auto it = s_mpbooty.find(plo);
    if (it != s_mpbooty.end()) return &it->second;
    BOOTY& b = s_mpbooty[plo];
    // +0x170 from the load's field (a vector), else (1, 1, 1)
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t u = plo->U32Field(0, 0x170 + 4 * i, 0x3F800000u);
        float g;
        std::memcpy(&g, &u, 4);
        b.v170[static_cast<int>(i)] = g;
    }
    return &b;
}
STAND* Pstand(const LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, S3CID(0x25))) return nullptr;
    return &s_mpstand[plo];
}
// cid 0x25's fields (0x13CEB8 init: +0x184 0x2BC, +0x188 -1, +0x18C 15; the load's options)
int NStandDefault(const LO* p) { return static_cast<int>(p->U32Field(0, 0x184, 0x2BC)); }
int NStand188(const LO* p) { return static_cast<int>(p->U32Field(0, 0x188, static_cast<uint32_t>(-1))); }
float GStand18C(const LO* p)
{
    const uint32_t u = p->U32Field(0, 0x18C, 0x41700000u);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}
int FStand190(const LO* p) { return static_cast<int>(p->U32Field(0, 0x190, 0)); }

const BOOTYREC* Prec(const LO* plo)
{
    const BOOTY* pb = Pbooty(plo);
    if (!pb || pb->irec < 0) return nullptr;
    const std::vector<BOOTYREC>& a = Arec(plo);
    return pb->irec < static_cast<int>(a.size()) ? &a[static_cast<size_t>(pb->irec)] : nullptr;
}
BOOTYREC* PrecMut(LO* plo) { return const_cast<BOOTYREC*>(Prec(plo)); }

// orig: 0x0013C478 (the current record's id, -1 none)
int IdBooty(const LO* plo)
{
    const BOOTYREC* pr = Prec(plo);
    return pr ? pr->id : -1;
}

// orig: 0x0013C490 (the record with that id current, else the first)
void SelectBootyRecord(LO* plo, int id)
{
    BOOTY* pb = Pbooty(plo);
    if (!pb) return;
    const std::vector<BOOTYREC>& a = Arec(plo);
    pb->irec = a.empty() ? -1 : 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id == id)
        {
            pb->irec = static_cast<int>(i);
            return;
        }
}

// SW +0x4638 (0x210940: 0x15E938(G, 5, 0x620, 0), the booty's streaming proxy)
LO* PloBootyProxy()
{
    // The native transition reuses the SW object. Its address therefore does
    // not identify a loaded world's proxy (orig SW +0x4638 is rebuilt by
    // 0x210940). Resolve from the current world after every transition.
    return g_psw ? PloFindSwObjectByOid(g_psw, 0x620) : nullptr;
}

// ACTOR +0x11A4: the part the booty is held at
ALO* PaloHolder(const ACTOR* pa)
{
    if (!pa || !pa->palo) return nullptr;
    for (const auto& [ib, plo] : pa->palo->apart)
        if (ib == 0x11A4 && plo && FIsLoDerivedFrom(plo, S3CID(0x30))) return static_cast<ALO*>(plo);
    return nullptr;
}

// the player's progress name (vt slot 103: 0x18DE08 "sly" / 0x136CB0 "bentley" / 0x1B9B60 "murray")
const char* SzPlayer(const ACTOR* pa)
{
    if (!pa || !pa->palo) return nullptr;
    return FIsLoDerivedFrom(pa->palo, S3CID(0x35)) ? "sly" : FIsLoDerivedFrom(pa->palo, S3CID(0x37)) ? "bentley"
         : FIsLoDerivedFrom(pa->palo, S3CID(0x36)) ? "murray" : nullptr;
}

CRef RefS32(int n)
{
    CRef r;
    r.SetS32(n);
    return r;
}
int NRefS32(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : 0; }

// orig: 0x0013D1A8 (the stand's save path: the current world's key + booty aoidAssembly <+0x180>)
std::vector<int16_t> AkeyStand(const LO* pstand)
{
    std::vector<int16_t> akey;
    const auto& awrec = AwrecWorlds();
    const int i = g_transition.iwrecCur;
    if (i < 0 || i >= static_cast<int>(awrec.size()) || !pstand) return akey;
    akey.assign(awrec[static_cast<size_t>(i)].an.begin(), awrec[static_cast<size_t>(i)].an.end());
    akey.push_back(static_cast<int16_t>(NSymProgress("booty")));
    akey.push_back(static_cast<int16_t>(NSymProgress("aoidAssembly")));
    akey.push_back(static_cast<int16_t>(s_mpstand[pstand].id));
    return akey;
}

// orig: 0x00143410 (the stand slot of any level, "episode <e> level <l> booty aoidAssembly <i>" for i < 15, whose value
// is n (an actor's oid: its booty is held by that actor); empty: none). The levels: the world records' keys.
std::vector<int16_t> AkeyStandHeldBy(int n)
{
    std::vector<std::vector<uint16_t>> aan;
    for (const WREC& w : AwrecWorlds())
        if (!w.an.empty() && std::find(aan.begin(), aan.end(), w.an) == aan.end()) aan.push_back(w.an);
    for (const auto& an : aan)
        for (int i = 0; i < 15; ++i)
        {
            std::vector<int16_t> akey(an.begin(), an.end());
            akey.push_back(static_cast<int16_t>(NSymProgress("booty")));
            akey.push_back(static_cast<int16_t>(NSymProgress("aoidAssembly")));
            akey.push_back(static_cast<int16_t>(i));
            const CRef r = RefGetProgressKey(akey);
            if (r.m_tagk == TAGK_S32 && r.m_n == n) return akey;
        }
    return {};
}

void SetPlayerBootyProgress(const ACTOR* pa, int n)
{
    if (const char* sz = SzPlayer(pa)) FSetProgress({sz, "oidBooty"}, RefS32(n));  // 0x1D8B20 on vt slot 103
}

// orig: 0x00143AF0 (the stand slot held by the actor gets the booty n back; the actor's oidBooty -1)
std::vector<int16_t> AkeyReturnBooty(ACTOR* pa, int n)
{
    const std::vector<int16_t> akey = AkeyStandHeldBy(pa->palo->oid);
    if (!akey.empty()) FSetProgressKey(akey, RefS32(n));
    SetPlayerBootyProgress(pa, -1);
    return akey;
}

// orig: 0x00143678 (the save of a booty moved: k 0 taken from / put on pstand, 1 delivered (0x1433C8, the safe
// house: its stand slot -1, a loot counted), 2 dropped (0x143380: its stand slot gets it again; the carried booty's
// stand is the one of this world with that slot))
void SaveBootyMove(ACTOR* pa, int k, LO* pstand)
{
    LO* pcarried = PlootCarriedBy(pa);
    const STAND* ps = pstand ? Pstand(pstand) : nullptr;
    switch (k)
    {
    case 0:
    {
        if (!ps) return;
        SetPlayerBootyProgress(pa, ps->pbooty ? IdBooty(ps->pbooty) : -1);
        if (pcarried)
        {
            if (!ps->pbooty)
            {
                const std::vector<int16_t> akey = AkeyStandHeldBy(pa->palo->oid);
                if (!akey.empty()) FSetProgressKey(akey, RefS32(-1));
            }
            FSetProgressKey(AkeyStand(pstand), RefS32(IdBooty(pcarried)));
        }
        else FSetProgressKey(AkeyStand(pstand), RefS32(pa->palo->oid));
        break;
    }
    case 1:
    {
        const std::vector<int16_t> akey = AkeyStandHeldBy(pa->palo->oid);
        if (!akey.empty()) FSetProgressKey(akey, RefS32(-1));
        SetPlayerBootyProgress(pa, -1);
        const BOOTYREC* pr = pcarried ? Prec(pcarried) : nullptr;
        if (pr && pr->n4 != -1)
        {
            const std::string s = std::to_string(pr->n4);  // sprintf "%d"
            const CRef r = RefGetProgress({"loot", "mplootkc", s});
            FSetProgress({"loot", "mplootkc", s}, RefS32(NRefS32(r) + 1));
        }
        break;
    }
    case 2:
    {
        const std::vector<int16_t> akey = AkeyReturnBooty(pa, IdBooty(pcarried));
        // the slot in this world (its key's first 4 symbols the world's): its stand (+0x180 = the index) is the
        // carried booty's +0x15C
        const auto& awrec = AwrecWorlds();
        const int i = g_transition.iwrecCur;
        if (akey.size() < 7 || i < 0 || i >= static_cast<int>(awrec.size())) break;
        const auto& an = awrec[static_cast<size_t>(i)].an;
        if (an.size() < 4) break;
        bool fSame = true;
        for (size_t j = 0; j < 4; ++j) fSame = fSame && static_cast<uint16_t>(akey[j]) == an[j];
        if (!fSame) break;
        for (LO* p : AploOfClass(g_psw, S3CID(0x25)))  // 0x15E9C8(G, 5, 0x25, 0, 15)
            if (Pstand(p)->id == akey[6])
            {
                if (BOOTY* pb = Pbooty(pcarried)) pb->pstand = p;
                break;
            }
        break;
    }
    default: break;
    }
}

// orig: 0x0013D248 (the booty a stand holds by the save: 0 its default +0x184, 7..9 (a player's oid: carried) none,
// 0x2BF..0x2C1 as 0x2BC..0x2BE)
int IdStandSaved(LO* pstand)
{
    int n = NRefS32(RefGetProgressKey(AkeyStand(pstand)));
    if (n == 0) n = NStandDefault(pstand);
    else if (n >= 7 && n < 10) n = -1;
    switch (n)
    {
    case 0x2BF: n = 0x2BC; break;
    case 0x2C0: n = 0x2BD; break;
    case 0x2C1: n = 0x2BE; break;
    default: break;
    }
    return n;
}

// orig: 0x0013CF38 (a stand without booty gets one from the proxy, the record the save says, state 1)
void AttachStandBooty(LO* pstand)
{
    STAND* ps = Pstand(pstand);
    if (!ps || ps->pbooty) return;
    LO* pproxy = PloBootyProxy();
    if (!pproxy) return;
    const int id = IdStandSaved(pstand);
    if (id == -1) return;
    LO* pb = PloAttachProxy(g_psw, pproxy, 0);  // 0x1CB250(proxy, 0)
    ps->pbooty = pb;
    if (FPrint()) std::printf("booty: stand %d (id %d) gets %d (record %d) t %.3f\n", pstand->oid, ps->id, pb ? pb->oid : -1, id, g_clock.t);
    if (!pb || !Pbooty(pb)) return;
    SelectBootyRecord(pb, id);
    Pbooty(pb)->pstand = pstand;
    SetBootyState(pb, 1);
}

// orig: 0x0013D328 (the stand's booty set to the record n: the old one out (state 0), the save, then attached)
void SetStandBooty(LO* pstand, int n)
{
    STAND* ps = Pstand(pstand);
    if (!ps) return;
    if (ps->pbooty) SetBootyState(ps->pbooty, 0);
    FSetProgressKey(AkeyStand(pstand), RefS32(n));
    if (n != -1) AttachStandBooty(pstand);
}

// orig: 0x001A6BE8 (vt slots 29 / 30: the object placed at a matrix)
void PlaceAloAt(ALO* palo, const glm::mat4& mat)
{
    SetAloPos(palo, glm::vec3(mat[3]));
    glm::mat3 m(mat);
    for (int i = 0; i < 3; ++i)
    {
        const float l = glm::length(m[i]);
        if (l > 0.0f) m[i] /= l;
    }
    SetAloRotation(palo, m);
}

// orig: 0x001A58D0(p, paloNew, 3) -> ALO vt slot 10 0x1F21B8 with grf 3 (its world position and rotation kept): the
// records that referred outside the object (its parent's, a proxy's channel, none) refer to the new parent's object
// record; the activation's parent changes (0x1A5900)
void ReparentBooty(ALO* palo, ALO* paloNew)
{
    UpdateAloHierarchyWorlds(palo);
    XF* pobj = PxfObj(palo);
    const glm::mat4 w = pobj ? pobj->matWorld : glm::mat4(1.0f);
    XF* pv = paloNew ? PxfObj(paloNew) : nullptr;
    for (auto& pxf : palo->axf)
    {
        bool fOwn = false;
        for (auto& p2 : palo->axf) fOwn = fOwn || p2.get() == pxf->pxfRef;
        if (!fOwn) pxf->pxfRef = pv;
    }
    // (the runtime vt slot 10's physics move, the original slot 13 (as SetAloParent's base move): attached to the
    // holder, the booty is part of the carrier's physics root, so its flight to the holder (state 2) does not push the
    // carrier; the loader's path left it a root of its own that collided with the player, N42)
    SetLoParentActivation(g_psw, palo, paloNew, true);
    UpdateAloHierarchyWorlds(palo);
    PlaceAloAt(palo, w);
    UpdateAloHierarchyWorlds(palo);
}

void StartFlight(BOOTY& b, const glm::vec3& posFrom, LO* ploTo)
{
    b.fFlight = true;
    b.posFrom = posFrom;
    b.posDraw = posFrom;
    b.ploTo = ploTo;
    b.tFlight = g_clock.t;
    const glm::vec3 to = PosLoWorld(ploTo);
    b.dtFlight = std::max(250.0f, glm::length(posFrom - to)) * 0.002f;  // (0x437A0000 / 0x3B03126F)
}

ACTOR* PactorOf(LO* plo) { return plo ? PactorFromLo(plo) : nullptr; }
}  // namespace

LO* PlootCarriedBy(const ACTOR* pa)
{
    auto it = s_mpcarried.find(pa);
    return it == s_mpcarried.end() ? nullptr : it->second;
}

bool FActorBootyFragile(const ACTOR* pa)
{
    LO* p = PlootCarriedBy(pa);
    const BOOTYREC* pr = p ? Prec(p) : nullptr;
    return pr && pr->f8;
}

// orig: 0x0013C560
void SetBootyState(LO* plo, int n)
{
    BOOTY* pb = Pbooty(plo);
    if (!pb) return;
    BOOTY& b = *pb;
    ALO* palo = static_cast<ALO*>(plo);
    const int old = b.state;
    if (old == n) return;
    if (FPrint())
        std::printf("booty %d %p: state %d -> %d (record %d) t %.3f\n", plo->oid, static_cast<void*>(plo), old, n, IdBooty(plo), g_clock.t);
    // the proxy's row kept while it is out of its stand (0x1CB488 / 0x1CB4B8)
    if (LO* pproxy = PloBootyProxy())
    {
        if (old < 2 && n >= 2) LockProxyRowOf(g_psw, pproxy, plo, true);
        else if (old >= 2 && n < 2) UnlockProxyRowOf(g_psw, pproxy, plo);
    }
    ACTOR* pc = b.pcarrier;
    const BOOTYREC* pr = Prec(plo);
    switch (old)
    {
    case 0:
        // 0x1F6A68 (every group hidden), the record's groups shown (0x1F6978), every collision entry off (0x1F2AD0),
        // the record's tag on (0x1F6AC8)
        if (palo->pglobset)
            for (const GLOB& g : palo->pglobset->aglob) ShowAloGlobs(palo, g.n0, false);
        if (pr)
            for (int i = 0; i < pr->c && i < 3; ++i) ShowAloGlobs(palo, pr->a[i], true);
        for (SOE& e : palo->asoe) SetSoeOff(palo, &e);
        if (pr && pr->n18 != -1) SetAloSoesByTag(palo, pr->n18, true);
        break;
    case 1:
    {
        LO* pstand = b.pstand;
        STAND* ps = Pstand(pstand);
        if (pr && pr->fC && pstand)
        {
            StartHudTimer(GStand18C(pstand) + 0.99f);  // 0x1618F8 (the stand's +0x18C + 0.99)
        }
        // 0x156C80(G, 1, its position, the stand, +0x198): a noise at the stand (the guards hear the theft)
        if (ps && pstand && FIsLoDerivedFrom(pstand, S3CID(0x30)))
        {
            SetHeldNoise(&ps->pnoise, PtmplNoiseFixed(1), PosAloCenter(static_cast<ALO*>(pstand)), pstand);  // 0x156BE0
        }
        if (n == 2) BroadcastTaskEvent(g_psw, 0x2F, plo);  // 0x1619A0(0x2DEB30, 0x2F, p)
        if (ps) ps->pbooty = nullptr;
        b.pstand = nullptr;
        HandleLoSpliceEvent(plo, 5, false, 0, nullptr);  // vt slot 3 (5)
        BroadcastTaskEvent(g_psw, 0x1A, plo);
        break;
    }
    case 2:
        b.fFlight = false;  // +0x18C
        if (n == 4) b.g180 = 0.3f;
        break;
    case 3: b.fFlight = false; break;
    case 4:
        b.pcarrier = nullptr;
        if (pc)
        {
            if (ALO* ph = PaloHolder(pc)) RemoveLo(g_psw, ph);  // +0x11A4's vt slot 9
            s_mpcarried.erase(pc);
        }
        if (pr && pr->n18 != -1) SetAloSoesByTag(palo, pr->n18, true);
        if (pr && pr->fC) StopHudTimer();  // 0x161958
        break;
    case 5:
        // +0x164 (EOPID 1156, an explosion (cid 0x1E family) the scripts set when it is taken: the timed booty's
        // self-destruct): its vt slot 51 with the message {1, itself}
        if (const int oid = static_cast<int>(plo->U32Field(0, 0x164, 0)); oid != 0 && oid != -1)
            if (LO* pr = PloFindSwNearest(plo, oid))
            {
                if (FPrint()) std::printf("booty %d: explosion %d t %.3f\n", plo->oid, pr->oid, g_clock.t);
                BURSTPARAMS bp;
                bp.grf = 1;
                bp.plo = pr;
                ExplodeLo(pr, &bp);
            }
        SetAloCnstrLin(palo, 1);
        SetAloCnstrAng(palo, 1);
        SetAloVelocityTo(palo, glm::vec3(0.0f));     // vt slot 35
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));  // vt slot 36
        SetAloGravity(palo, glm::vec3(0.0f));        // 0x1F5880 (D_002A2DD0)
        break;
    case 6: b.g180 = 0.0f; break;
    default: break;
    }
    b.state = n;
    b.t = g_clock.t;
    switch (n)
    {
    case 0:
        b.irec = -1;
        SetLoParentActivation(g_psw, plo, nullptr);  // 0x1A58D0(p, 0, 0)
        RemoveLo(g_psw, plo);                        // vt slot 9
        return;
    case 1:
    {
        LO* pstand = b.pstand;
        STAND* ps = Pstand(pstand);
        if (ps && ps->pnoise)  // 0x156C80(G, -1, ..., +0x198)
            SetHeldNoise(&ps->pnoise, nullptr, glm::vec3(0.0f), nullptr);  // 0x156BE0
        if (pstand)
        {
            PlaceAloAt(palo, MatJtLo(pstand));             // 0x1A6BE8(p, the stand's vt slot 31)
            ReparentBooty(palo, pstand->paloParent);       // 0x1A58D0(p, stand +0x10, 3)
        }
        if (ps) ps->pbooty = plo;
        if (old != 3)
        {
            AddLo(g_psw, plo);             // vt slot 8
            SwitchLoFaded(plo, true, 0.5f);  // 0x1F7C68(p, 0.5)
        }
        HandleLoSpliceEvent(plo, 6, false, 0, nullptr);  // vt slot 3 (6)
        BroadcastTaskEvent(g_psw, 0x1B, plo);
        return;
    }
    case 2:
    case 6:
    {
        ALO* ph = PaloHolder(pc);
        if (ph) AddLo(g_psw, ph);  // +0x11A4's vt slot 8
        else WarnOnce("a carrier without its holder part +0x11A4");
        if (n == 2)
        {
            // the flight from the booty's place to the holder (the RIP 0x1CE888(G, 6, 0))
            StartFlight(b, PosAloCenter(palo), ph ? static_cast<LO*>(ph) : (pc ? pc->palo : nullptr));
        }
        else
        {
            b.fFlight = false;
            HandleLoSpliceEvent(plo, 6, false, 0, nullptr);
            BroadcastTaskEvent(g_psw, 0x1B, plo);
            HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
            BroadcastTaskEvent(g_psw, 0x1A, plo);
        }
        AddLo(g_psw, plo);  // 0x13CB6C: shared by states 2 and 6, before placement at the holder
        ALO* pat = ph ? ph : (pc ? pc->palo : nullptr);
        if (pat)
        {
            UpdateAloHierarchyWorlds(pat);
            PlaceAloAt(palo, MatJtLo(pat));  // 0x1A6BE8(p, holder's vt slot 31)
            ReparentBooty(palo, pat);        // 0x1A58D0(p, holder, 3)
        }
        if (!b.fFlight) SetBootyState(plo, 4);
        return;
    }
    case 3:
    {
        LO* pstand = b.pstand;
        const glm::vec3 posFrom = pc && PaloHolder(pc) ? PosLoWorld(PaloHolder(pc)) : PosAloCenter(palo);
        if (pstand)
        {
            PlaceAloAt(palo, MatJtLo(pstand));
            ReparentBooty(palo, pstand->paloParent);
        }
        StartFlight(b, posFrom, plo);  // (target: the booty itself, at the stand)
        b.ploTo = nullptr;
        b.posFrom = posFrom;
        AddLo(g_psw, plo);
        return;
    }
    case 4:
        if (pc) s_mpcarried[pc] = plo;  // +0x11A8
        if (pr && pr->n18 != -1) SetAloSoesByTag(palo, pr->n18, false);  // 0x1F6B48
        return;
    case 5:
    {
        ReparentBooty(palo, nullptr);  // 0x1A58D0(p, 0, 3)
        SetAloCnstrLin(palo, 0);
        SetAloCnstrAng(palo, 0);
        XF* pobj = PxfObj(palo);
        const glm::vec3 x = pobj ? glm::vec3(pobj->matWorld[0]) : glm::vec3(1.0f, 0.0f, 0.0f);  // vt slot 31's first row
        SetAloVelocityTo(palo, x * 1000.0f);
        const float a = GRandInRange(-6.2831855f, 6.2831855f);
        const float bb = GRandInRange(-6.2831855f, 6.2831855f);
        const float c = GRandInRange(-6.2831855f, 6.2831855f);
        SetAloAngVelocityTo(palo, glm::vec3(a, bb, c));
        SetAloGravity(palo, glm::vec3(0.0f, 0.0f, -2500.0f));  // D_002A4B60
        BroadcastTaskEvent(g_psw, 0x30, plo);
        return;
    }
    default: return;
    }
}

namespace
{
// orig: 0x0013C4E0 (a thrown booty back on its stand when the stand is in the world, else out)
void ReturnThrownBooty(LO* plo)
{
    BOOTY* pb = Pbooty(plo);
    if (!pb || pb->state != 5) return;
    LO* pstand = pb->pstand;
    if (FPrint()) std::printf("booty: %d back to stand %d t %.3f\n", plo->oid, pstand ? pstand->oid : -1, g_clock.t);
    pb->pstand = nullptr;
    const int irec = pb->irec;
    SetBootyState(plo, 0);
    if (pstand && pstand->fInWorld && FLoActive(pstand))  // 0x1A61A0 (in the world, its root active)
    {
        pb->pstand = pstand;
        pb->irec = irec;
        SetBootyState(plo, 1);
    }
}

// The flight's end: the original's message 8 from the RIP (0x13C1B8: state 2 -> 4, 3 -> 1)
void UpdateBootyFlight(LO* plo, BOOTY& b)
{
    ALO* palo = static_cast<ALO*>(plo);
    const float tau = g_clock.t - b.tFlight;
    const glm::vec3 to = b.ploTo ? PosLoWorld(b.ploTo) : glm::vec3(MatJtLo(b.state == 3 && b.pstand ? b.pstand : plo)[3]);
    if (tau >= b.dtFlight)
    {
        b.fFlight = false;
        if (b.state == 3) PlaceAloAt(palo, b.pstand ? MatJtLo(b.pstand) : PxfObj(palo)->matWorld);
        SetBootyState(plo, b.state == 2 ? 4 : 1);
        return;
    }
    const float u = b.dtFlight > 0.0f ? tau / b.dtFlight : 1.0f;
    const glm::vec3 acc(0.0f, 0.0f, -3000.0f);  // the RIP's +0xA0
    // (drawn there only: the booty stays where 0x13C560 put it (the holder / the stand). Moving the object itself
    // swept its collision entries, part of the carrier's physics root once it is the holder's child, through the stand
    // and the walls, and the contacts threw the carrier (R14: Sly thrown 500 units by the bugged painting))
    b.posDraw = b.posFrom + (to - b.posFrom) * u + 0.5f * acc * tau * (tau - b.dtFlight);
}

// orig: 0x0013BF50 (vt slot 16; carried scale belongs to the draw's 0x13C000, below)
void UpdateBooty(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    BOOTY* pb = Pbooty(plo);
    if (!pb) return;
    BOOTY& b = *pb;
    if (b.fFlight) UpdateBootyFlight(plo, b);
    switch (b.state)
    {
    case 5:
        if (g_clock.t - b.t >= 5.0f) ReturnThrownBooty(plo);
        break;
    case 4:
    {
        const BOOTYREC* pr = Prec(plo);
        if (pr && pr->fC && g_clock.t >= THudTimerEnd()) DropActorBooty(b.pcarrier);  // 0x143380(+0x160)
        break;
    }
    default: break;
    }
}

// orig: 0x0013C1B8 (vt slot 5: the base's 0x133590, then message 4 in state 5: out)
void OnBootyMessage(LO* plo, int msgk, void*)
{
    BOOTY* pb = Pbooty(plo);
    if (pb && msgk == 4 && pb->state == 5)
    {
        if (FPrint()) std::printf("booty: %d message 4 t %.3f\n", plo->oid, g_clock.t);
        SetBootyState(plo, 0);
    }
}

// orig: 0x0013C2D8 (vt slot 55: a new contact; thrown, with something that is not a player: back to its stand, deferred)
void OnBootyXpNew(ALO* palo, XP* pxp, int i)
{
    BOOTY* pb = Pbooty(palo);
    if (!pb || pb->state != 5 || !pxp) return;
    ALO* pother = pxp->axpd[1 - i].paloRoot;
    if (!FIsLoDerivedFrom(pother, S3CID(0x34)))
        PostDeferredCall(palo, 0x13C4E0, [](void* object, int) { ReturnThrownBooty(static_cast<LO*>(object)); });
}

// orig: 0x0013C278 (vt slot 61: thrown, no contact response with the players (3), else the base 0x1F4FC8)
int OverlapKindBooty(ALO* pa, ALO* pb)
{
    BOOTY* p = Pbooty(pa);
    if (p && p->state == 5 && FIsLoDerivedFrom(pb, S3CID(0x34))) return 3;
    using PFN = int (*)(ALO*, ALO*);
    PFN pfnBase = reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8));
    return pfnBase ? pfnBase(pa, pb) : 0;
}

// orig: 0x0013C368 (vt slot 75: an accepted contact while thrown: out)
bool FAcceptBootyContact(ALO* palo, CTMSG* pctm)
{
    using PFN = bool (*)(ALO*, CTMSG*);
    PFN pfnBase = reinterpret_cast<PFN>(PvNativeFn(0x1F7588));
    if (!(pfnBase && pfnBase(palo, pctm))) return false;
    BOOTY* pb = Pbooty(palo);
    if (!pb || pb->state != 5) return false;
    if (FPrint()) std::printf("booty: %d contact accepted t %.3f\n", palo->oid, g_clock.t);
    SetBootyState(palo, 0);
    return true;
}

struct INITBOOTY
{
    INITBOOTY()
    {
        RegisterLoUpdate(0x13BF50, UpdateBooty);
        // EOPID 1152 (0x10E008 -> 0x13C3B8: a record {id, n4, n14} added (32 at most), current)
        RegisterObjectMethod(0x10E008, [](LO* plo, const std::vector<CRef>& a) {
            if (BOOTY* pb = Pbooty(plo))
            {
                std::vector<BOOTYREC>& arec = Arec(plo);
                pb->irec = -1;  // 0x13C440
                if (arec.size() < 0x20)
                {
                    BOOTYREC r;
                    r.id = NArg(a, 0);
                    r.n4 = NArg(a, 1);
                    r.n14 = NArg(a, 2);
                    r.n18 = -1;
                    r.c = 0;
                    arec.push_back(r);
                    pb->irec = static_cast<int>(arec.size()) - 1;
                }
            }
            return CRef(TAGK_Void);
        });
        // EOPID 1153 (0x10E0C0 -> 0x13C440: no current record)
        RegisterObjectMethod(0x10E0C0, [](LO* plo, const std::vector<CRef>&) {
            if (BOOTY* pb = Pbooty(plo)) pb->irec = -1;
            return CRef(TAGK_Void);
        });
        // EOPID 1154 (0x10E138 -> 0x13C448: a group shown by the current record, 3 at most)
        RegisterObjectMethod(0x10E138, [](LO* plo, const std::vector<CRef>& a) {
            if (BOOTYREC* pr = PrecMut(plo); pr && pr->c < 3) pr->a[pr->c++] = NArg(a, 0);
            return CRef(TAGK_Void);
        });
        // orig: 0x001FFD98's BASIC field getter: EOPID 752 / option 735 reads ACTOR +0x11A8.
        // The missile scripts use the carried booty itself as their moving target.
        RegisterObjectGetter(752, [](LO* plo) {
            CRef r;
            r.SetBasic(PlootCarriedBy(PactorFromLo(plo)));
            return r;
        });
        // EOPID 1158 (option 831): the current record id (0x13C478), or select by id (0x13C490).
        RegisterObjectGetter(1158, [](LO* plo) { return RefS32(IdBooty(plo)); });
        RegisterObjectSetter(1158, [](LO* plo, const CRef& r) { SelectBootyRecord(plo, r.m_n); });
        // EOPIDs 1159 / 1160 (the current record's +0x14 / +0x4)
        RegisterObjectGetter(1159, [](LO* plo) {
            const BOOTYREC* pr = Prec(plo);
            return RefS32(pr ? pr->n14 : 0);
        });
        RegisterObjectGetter(1160, [](LO* plo) {
            const BOOTYREC* pr = Prec(plo);
            return RefS32(pr ? pr->n4 : 0);
        });
        // EOPID 1157 (option 1006): +0x170 is a live vector field; scripts set it after the records are loaded.
        RegisterObjectGetter(1157, [](LO* plo) {
            CRef r;
            if (const BOOTY* pb = Pbooty(plo)) r.SetVector(pb->v170);
            return r;
        });
        RegisterObjectSetter(1157, [](LO* plo, const CRef& r) {
            if (BOOTY* pb = Pbooty(plo))
            {
                pb->v170 = r.GetVector();
                for (uint32_t i = 0; i < 3; ++i)
                {
                    uint32_t u;
                    std::memcpy(&u, &pb->v170[static_cast<int>(i)], 4);
                    plo->SetField(0, 0x170 + 4 * i, u);  // a later clone keeps the field, as the original does
                }
                if (FPrint())
                    std::printf("booty: %d carried scale set to %.3f %.3f %.3f t %.3f\n", plo->oid,
                                pb->v170.x, pb->v170.y, pb->v170.z, g_clock.t);
            }
        });
        // orig: 0x00200BA0's direct fields (EOPIDs 1164/1165/1166: +0x184 state, +0x15C stand, +0x160 carrier).
        RegisterObjectGetter(1164, [](LO* plo) {
            const BOOTY* pb = Pbooty(plo);
            return RefS32(pb ? pb->state : -1);
        });
        RegisterObjectGetter(1165, [](LO* plo) {
            CRef r;
            if (const BOOTY* pb = Pbooty(plo); pb && pb->pstand) r.SetBasic(pb->pstand);
            return r;
        });
        RegisterObjectGetter(1166, [](LO* plo) {
            CRef r;
            if (const BOOTY* pb = Pbooty(plo); pb && pb->pcarrier) r.SetBasic(pb->pcarrier->palo);
            return r;
        });
        // orig: 0x00200BA0's direct object field (EOPID 1170: the stand's +0x194 booty).
        RegisterObjectGetter(1170, [](LO* plo) {
            CRef r;
            if (const STAND* ps = Pstand(plo); ps && ps->pbooty) r.SetBasic(ps->pbooty);
            return r;
        });
        // the stand's method (0x10E1C0 -> 0x13D328: its booty set to the record n)
        RegisterObjectMethod(0x10E1C0, [](LO* plo, const std::vector<CRef>& a) {
            if (FPrint()) std::printf("booty: stand %d set to %d t %.3f\n", plo->oid, NArg(a, 0), g_clock.t);
            SetStandBooty(plo, NArg(a, 0));
            return CRef(TAGK_Void);
        });
        // orig: 0x00200C28 (seto! of EOPID 1169: the stand's +0x190 lock field).
        // Job 1810 clears this at the end of its briefing before using the stand (0x13D020).
        RegisterObjectSetter(1169, [](LO* plo, const CRef& r) {
            plo->SetField(0, 0x190, r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n);
        });
        // the actors' methods: 0x10B0B8 -> 0x1432E0 (given n), 0x10B140 -> 0x143380 (dropped), 0x10B1B8 -> 0x1433C8 (put back)
        RegisterObjectMethod(0x10B0B8, [](LO* plo, const std::vector<CRef>& a) {
            if (ACTOR* pa = PactorOf(plo)) GiveActorBooty(pa, NArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10B140, [](LO* plo, const std::vector<CRef>&) {
            if (FPrint()) std::printf("booty: actor %d drops its booty (method) t %.3f\n", plo->oid, g_clock.t);
            if (ACTOR* pa = PactorOf(plo)) DropActorBooty(pa);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x10B1B8, [](LO* plo, const std::vector<CRef>&) {
            if (ACTOR* pa = PactorOf(plo)) ReturnActorBooty(pa);
            return CRef(TAGK_Void);
        });
    }
} s_initbooty;
}  // namespace

// orig: 0x0013C000 (vt slot 19): 0x11C210 scales the object's world basis about its current position, then
// 0x1A5EE8 draws the hierarchy relative to that matrix. Its local records may contain large offsets that cancel;
// changing them to apply this scale would also change the attachment and the physics.
bool FBootyDrawDelta(const ALO* palo, glm::mat4* pdmat)
{
    const auto it = s_mpbooty.find(palo);
    if (it == s_mpbooty.end()) return false;
    const BOOTY& b = it->second;
    // in flight (states 2 / 3): the model drawn at the flight's point (the RIP of kind 6, 0x1CFE88), the object stays
    if (b.fFlight)
    {
        const XF* pobj = PxfObj(const_cast<ALO*>(palo));
        if (!pobj) return false;
        *pdmat = glm::translate(glm::mat4(1.0f), b.posDraw - glm::vec3(pobj->matWorld[3]));
        return true;
    }
    if (b.state != 4 || !palo->apxfRef[2]) return false;
    const float dt = g_clock.t - b.t;
    glm::vec3 v = b.v170;
    if (dt < b.g180)
    {
        const float u = dt / b.g180;
        const float g = std::clamp(0.0f + u * (0.0f + u * 1.5f), 0.0f, 1.0f);
        v = glm::vec3(1.0f - g) + g * b.v170;
    }
    const glm::mat4& matWorld = palo->apxfRef[2]->matWorld;
    glm::mat4 matDraw = matWorld;
    for (int i = 0; i < 3; ++i) matDraw[i] = glm::vec4(glm::vec3(matWorld[i]) * v[i], 0.0f);
    matDraw[3].w = 1.0f;
    *pdmat = matDraw * glm::inverse(matWorld);
    return true;
}

NATIVEFN(0x13C1B8, OnBootyMessage);
NATIVEFN(0x13C2D8, OnBootyXpNew);
NATIVEFN(0x13C278, OverlapKindBooty);
NATIVEFN(0x13C368, FAcceptBootyContact);

void SetBootyRecordField(LO* plo, uint32_t ib, const OPTVAL& v)
{
    BOOTYREC* pr = PrecMut(plo);
    if (!pr) return;
    if (FPrint()) std::printf("booty: %d record %d field +0x%X = %d\n", plo->oid, pr->id, ib, v.n);
    switch (ib)
    {
    case 0x8: pr->f8 = v.n; break;
    case 0xC: pr->fC = v.n; break;
    case 0x10: pr->f10 = v.n; break;
    case 0x18: pr->n18 = v.n; break;
    default: WarnOnce("a booty record's field +0x" + std::to_string(ib)); break;
    }
}

// orig: 0x00143380
void DropActorBooty(ACTOR* pa)
{
    LO* p = PlootCarriedBy(pa);
    if (!p) return;
    if (FPrint()) std::printf("booty: %d drops %d t %.3f\n", pa->palo->oid, p->oid, g_clock.t);
    SaveBootyMove(pa, 2, nullptr);
    SetBootyState(p, 5);
}

// orig: 0x001433C8
void ReturnActorBooty(ACTOR* pa)
{
    LO* p = PlootCarriedBy(pa);
    if (!p) return;
    SaveBootyMove(pa, 1, nullptr);
    SetBootyState(p, 0);
}

// orig: 0x001432E0
void GiveActorBooty(ACTOR* pa, int n)
{
    LO* q = PlootCarriedBy(pa);
    if (q && IdBooty(q) == n) return;
    DropActorBooty(pa);
    if (n == -1) return;
    LO* pproxy = PloBootyProxy();
    if (!pproxy) return;
    LO* o = PloAttachProxy(g_psw, pproxy, 1);  // 0x1CB250(proxy, 1)
    if (!o || !Pbooty(o)) return;
    SelectBootyRecord(o, n);
    Pbooty(o)->pcarrier = pa;
    SetBootyState(o, 6);
    SetBootyState(o, 4);
}

// orig: 0x0013D020
bool FUseBootyStand(LO* pstand, ACTOR* pa)
{
    STAND* ps = Pstand(pstand);
    if (!ps) return false;
    if (!pa || !FIsLoDerivedFrom(pa->palo, S3CID(0x34))) return true;  // 0x132B10(q, 0x34)
    LO* pqa = PlootCarriedBy(pa);
    LO* ppa = ps->pbooty;
    if (FPrint())
        std::printf("booty: %d uses stand %d (id %d): carried %d, on it %d t %.3f\n", pa->palo->oid, pstand->oid, ps->id, pqa ? IdBooty(pqa) : -1,
                    ppa ? IdBooty(ppa) : -1, g_clock.t);
    if (!pqa && !ppa) return true;
    const BOOTYREC* prp = ppa ? Prec(ppa) : nullptr;
    if (FPrint())
        std::printf("booty: stand limits: job %d allowed %d blocked %d fits %d/%d, groups %d tag %d\n",
                    g_psw->tasks.pjobCur != nullptr, prp ? prp->f10 : -1, FStand190(pstand),
                    pqa && Prec(pqa) ? Prec(pqa)->n14 : -1, NStand188(pstand), prp ? prp->c : 0, prp ? prp->n18 : -1);
    if (ppa && g_psw->tasks.pjobCur && !(prp && prp->f10))  // SW +0x3B0C: during a job
    {
        HudPrompt(IdText(0x124), 15.0f);  // 0x1DA4B8(0x3DA0C0, 0x124, 15)
        PsndStartSound(0x36);     // 0x1FBB90(0x36) / 0x1FB2F8
        return true;
    }
    const BOOTYREC* prq = pqa ? Prec(pqa) : nullptr;
    if (FStand190(pstand) || (pqa && prq && prq->n14 != NStand188(pstand)))
    {
        BroadcastTaskEvent(g_psw, 0x32, pstand);
        return true;
    }
    if (ppa) PsndStartSound(0x27);
    SaveBootyMove(pa, 0, pstand);
    if (pqa && ps->pbooty)
    {
        Pbooty(pqa)->pstand = pstand;
        SetBootyState(pqa, 3);
        Pbooty(ps->pbooty)->pcarrier = pa;
        SetBootyState(ps->pbooty, 2);
    }
    else if (pqa)
    {
        Pbooty(pqa)->pstand = pstand;
        SetBootyState(pqa, 3);
    }
    else
    {
        Pbooty(ps->pbooty)->pcarrier = pa;
        SetBootyState(ps->pbooty, 2);
    }
    UseObjectBaseBy(pstand, pa);  // 0x184130
    return true;
}

// orig: 0x0013BF00 (vt slot 15: the base's 0x1F36A8, then out: state 0)
void PostLoadBooty(LO* plo)
{
    if (!Pbooty(plo)) return;
    SetBootyState(plo, 0);
}

// orig: 0x0013CEF8 (vt slot 15: the use's 0x183F50, the stand's index: the world's counter)
void PostLoadBootyStand(SW* psw, LO* plo)
{
    if (STAND* ps = Pstand(plo)) ps->id = s_mpcstand[psw]++;
    if (FPrint())
    {
        const glm::vec3 pos = PosLoWorld(plo);
        std::printf("booty: stand %d at (%.0f %.0f %.0f) blocked %d default %d time %.1f\n", plo->oid, pos.x, pos.y, pos.z, FStand190(plo), NStandDefault(plo), GStand18C(plo));
    }
}

// orig: 0x0013CFD8 (vt slot 23: the use's 0x183FB8, then 0x13CF38 posted (0x149AB8, D_002A4BB0))
void OnBootyStandRegistered(LO* plo)
{
    PostDeferredCall(plo, 0x13CF38, [](void* object, int) { AttachStandBooty(static_cast<LO*>(object)); });
}

// orig: 0x00143BD8 (the save's oidBooty: put back (0x143AF0) when the load asks it (+0x3BF4 0x40), else carried again)
void PostLoadActorBooty(ACTOR* pa)
{
    const char* sz = SzPlayer(pa);
    if (!sz) return;
    const int n = NRefS32(RefGetProgress({sz, "oidBooty"}));
    if (static_cast<unsigned>(n + 1) < 2) return;
    if (g_transition.req.flags & 0x40) AkeyReturnBooty(pa, n);
    else GiveActorBooty(pa, n);
}

void QueuePostLoadActorBooty(LO* plo)
{
    if (plo) PostDeferredCall(plo, 0x143BD8, [](void* object, int) {
        if (ACTOR* pa = PactorOf(static_cast<LO*>(object))) PostLoadActorBooty(pa);
    });
}

void ResetBooties()
{
    // Records are loaded again with the next world's booty template. SW may
    // retain the same address, so Arec's address check is insufficient here.
    s_recs.mp.clear();
    s_recs.psw = nullptr;
    s_mpbooty.clear();
    s_mpstand.clear();
    s_mpcstand.clear();
    s_mpcarried.clear();
}

// orig: 0x0013BF30 (the booty's vt slot 27, a pooled holder detached: back to state 0 (0x13C560)); natively it was
// never called (audit 2026-11 L1)
void DetachBooty27(LO* plo, LO*) { SetBootyState(plo, 0); }
NATIVEFN(0x13BF30, DetachBooty27);

// orig: 0x1A6730 (CloneLo copies the object, its current record +0x158 included): natively the booty's state lives in
// s_mpbooty, so a copy had no current record (its records are shared by oid)
void CloneBootyData(const LO* source, LO* target)
{
    const auto it = s_mpbooty.find(source);
    if (it != s_mpbooty.end()) s_mpbooty[target] = it->second;
}
