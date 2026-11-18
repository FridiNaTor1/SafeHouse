// Sly 3's outfits (the disguises): the class-0x96 object (orig3 module 0x172608..0x173560; s3-decomp, 2026-11-10).
//
// A class-0x96 object holds up to 32 outfit records (0xF0 bytes each), built by its load-time options:
//   526 (thunk 0x106F00 -> 0x172710) a record with its id (0x759 / 0x75A / 0x75B the disguises, 0x75C / 0x75D others),
//   527 (0x106F88 -> 0x1727C8) its animation slot index -> an ASEG / SM (the 30 pairs; not applied natively),
//   528 / 530 (0x107030 / 0x107160 -> 0x1727F8 kinds 0 / 1) an object taken out of the world while it is worn,
//   529 / 531 (0x1070B8 / 0x1071E8 -> kinds 2 / 3) an object's glob tag shown (2) / hidden (3) while it is worn.
// The post-load (0x1728F0) finds the player it dresses (the nearest class-0x33 object) and the items' objects (oid 0 /
// -1: the player). Wearing (0x172E60): the record worn so far undone (kinds 0 / 1 AddLo, 2 hidden, 3 shown), the new
// one done (RemoveLo / shown / hidden); the game's player entry takes its id (the guards' disguise test: not natively).
// The meshes the tags show are streamed first (0x172C30 -> 0x16F2B0): the linked class-0x97 object's group of the record's
// id (a keyed file: "SLD.", its shaders, then the linked objects' keyed globs, glob.cpp LoadGlobsetKeyedFromBrx); the
// scene is rebuilt for them (game.cpp). Not natively: the outfit's gait / animation pairs (0x258A10, 0x1727C8), the
// change's animation (0x173280), the group's unload (0x16F120: a group stays loaded, hidden by the tags).
// The scripts: 534 (0x107290 -> 0x172D10) wear at once, 535 (0x107318 -> 0x172AE8) wear (with the change animation
// when Sly is free: natively at once), 532 / 533 the worn id. Sly's disguise gadgets (0x1AF6C0 cases 0x26..0x28: the
// ids 0x759..0x75B, 0x173560) toggle them: gadget.cpp.

#include "outfit3.h"

#include "actor.h"
#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "cls.h"
#include "lo.h"
#include "objmethod.h"
#include "ref.h"
#include "sw.h"
#include "worldaccess.h"
#include "clock.h"
#include "jump.h"
#include "sly.h"
#include "actanim.h"
#include "aseg.h"
#include "asega.h"
#include "sm.h"
#include "bis.h"
#include "disc.h"
#include "glob.h"
#include "shd.h"
#include "game/aseg3.h"
#include "game/loaders3.h"
#include "game/sly3.h"
#include "transition.h"

#include <array>
#include <memory>
#include <cstring>
#include <set>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
struct ITEM { int kind = 0; int oid = -1; int value = -1; LO* plo = nullptr; };
struct PAIR { int oid = -1; ASEG* paseg = nullptr; SM* psm = nullptr; };
struct REC
{
    int id = -1; int value = -1; std::vector<ITEM> aitem;
    std::array<PAIR, 30> apair;                 // (0x1727C8: the outfit's animations by slot: 0 the idle SM, 1 / 2 the gait's)
    std::shared_ptr<GAIT> pgait;                // +0x30 (0x258930 / 0x258958: the gait of the slots 1 / 2)
};
struct OUTFIT
{
    SW* psw = nullptr;
    uint32_t serial = 0;   // (the world's serial: a reloaded world can reuse the old SW's address)
    std::vector<REC> arec;
    LO* ploPlayer = nullptr;
    int icur = -1;
    bool fApplied = false;   // (native) the worn record's items applied
    bool fResolved = false;
    int mode = -1;           // +0x48 (0x173280): -1 none, 0 the change's animation plays (the record pending), 1 changed
    int idPending = -1;      // +0x54 the record waiting for the animation's event 0x757
    float tMode = 0.0f;      // (native) the change's start
    int cHold = 0;           // +0x64 the scripts' hold (0x173448 / 0x173498): the outfit's restrictions off while > 0
};
std::unordered_map<LO*, OUTFIT> s_mpoutfit;

bool FPrint() { static const bool s_f = std::getenv("SLY3_OUTFITPRINT") != nullptr; return s_f; }
bool FOldOutfitHold() { static const bool s_f = std::getenv("SLY3_OUTFITHOLD0") != nullptr; return s_f; }

// the class-0x97 object's group streamed in this world (+0x698: one at a time) with its globs as they were without
// their meshes (the unload puts them back); the scene to rebuild (game.cpp)
struct STREAMED { int id = -1; std::vector<std::tuple<std::shared_ptr<GLOBSET>, int, GLOB>> aglob; };
std::unordered_map<LO*, STREAMED> s_mpstreamed;
SW* s_pswStreamed = nullptr;
bool s_fSceneRebuild = false;

// orig3 0x16F120 (the streamed group unloaded: each link's object drops the keyed meshes (vt slot 16), the group's
// shaders leave the list): natively the globs are put back as they were loaded (no mesh, hidden)
void UnloadGroup97(LO* plo)
{
    auto it = s_mpstreamed.find(plo);
    if (it == s_mpstreamed.end() || it->second.id == -1) return;
    for (auto& [pgs, i, glob] : it->second.aglob)
        if (pgs && i >= 0 && i < int(pgs->aglob.size())) pgs->aglob[size_t(i)] = glob;
    if (FPrint()) std::printf("outfit: unloaded the group %d of %d (%zu meshes)\n", it->second.id, plo->oid, it->second.aglob.size());
    it->second = STREAMED{};
    s_fSceneRebuild = true;
}

}  // namespace

// orig3 0x16F2B0 (a class-0x97 object's group: its file (the entry's key) decompressed, "SLD." version 1: the bulk
// header and the shaders (0x25C930 / 0x234EA0) as a world group's, then each link of the group's range: kind 0 / 1 the
// object's keyed meshes (vt slot 15: 0x18FE80 with -1 / the link's key), the shader group ended (0x235DE0)). The group
// loaded before is unloaded first (0x16F120).
// orig3 0x135F48 (ASEG vt slot 15: the deferred segment (+0xA4) read from the group's stream once, +0xA8) / 0x2395E8 (SM
// vt slot 15: each transition's object's slot 15); the segments streamed
int CStreamDeferredAsegs(LO* plo, CBinaryInputStream* pbis)
{
    if (ASEG* paseg = dynamic_cast<ASEG*>(plo))
    {
        if (!paseg->pasegd3 || !paseg->pasegd3->fDeferred) return 0;
        LoadAsegFromBrx3(paseg, pbis);
        return 1;
    }
    int c = 0;
    if (SM* psm = dynamic_cast<SM*>(plo))
        for (const SM::SMT& t : psm->asmt)
            if (t.paseg) c += CStreamDeferredAsegs(t.paseg, pbis);
    return c;
}

bool FStreamGroup97(int id, LO* ploOwner)
{
    if (!g_psw || id == -1) return false;
    static uint32_t s_serialStreamed = 0;
    if (s_pswStreamed != g_psw || (g_psw && s_serialStreamed != g_psw->serial))
    {
        s_mpstreamed.clear();
        s_pswStreamed = g_psw;
        s_serialStreamed = g_psw ? g_psw->serial : 0;
    }
    for (LO* plo : g_psw->mpcidaplo[size_t(0x97)])
    {
        LO97* p = dynamic_cast<LO97*>(plo);
        if (!p || (ploOwner && plo != ploOwner)) continue;
        for (size_t i = 0; i < p->aentry.size(); ++i)
        {
            const LO97::ENTRY& e = p->aentry[i];
            if (e.a != id) continue;
            if (s_mpstreamed[plo].id == id) return true;   // (0x16F5E8: loaded already)
            UnloadGroup97(plo);
            const FK fk = FkFromRecord(e.abKey.data());
            if (fk.cb == 0) return false;
            CBinaryInputStream bis;
            bis.OpenBuffer(g_disc.ReadWorld(fk));
            char ach[4] = {};
            for (char& ch : ach) ch = char(bis.U8Read());
            const int version = bis.U16Read();
            if (std::memcmp(ach, "SLD.", 4) != 0 || version != 1)
            {
                std::printf("outfit: the group %d of %d is not an SLD stream (version %d)\n", id, plo->oid, version);
                return false;
            }
            LoadBulkHeaderFromBrx(g_psw, &bis, true);
            const size_t cshdBefore = g_shds.ashd.size();
            LoadShadersFromBrx(g_psw, &bis);
            if (FPrint())
                for (size_t k = cshdBefore; k < g_shds.ashd.size(); ++k) std::printf("outfit: group %d shader 0x%X\n", id, g_shds.ashd[k]->id);
            int cglob = 0;
            const int jFirst = i > 0 ? p->aentry[i - 1].b : 0;
            for (int j = std::max(0, jFirst); j < e.b && j < int(p->aitem.size()); ++j)
            {
                const LO97::ITEM& it = p->aitem[size_t(j)];
                if (FPrint()) std::printf("outfit: group %d item %d kind %d a %d b %d\n", id, j, it.kind, it.a, it.b);
                if (it.kind != 0 && it.kind != 1) continue;
                LO* ploLink = PloFindSwNearest(plo, it.a);   // (0x16F090: 0x17E1B8(game, 4, oid))
                // (vt slot 15 of the other classes: an ASEG's 0x135F48 loads its deferred segment (+0xA4) from the stream
                // once (+0xA8); an SM's 0x2395E8 does that for each of its transitions' ASEGs. E.g. Sly's silhouette
                // outfit 1885 (m_ext's attract): its run 7951 / 7949, its idle SM 7941 and 7939, natively left empty
                // before s3-intro: the outfit's gait / idle could not be used (Sly ran at his own 510 a second where
                // the reference's silhouette runs 833). SLY3_OUTFITNOASEG=1: as before)
                static const bool s_fNoAseg = std::getenv("SLY3_OUTFITNOASEG") != nullptr;
                if (ploLink && !s_fNoAseg && !FCidDerivedFrom(ploLink->Cid(), S3CID(0x30)))
                {
                    const int c = CStreamDeferredAsegs(ploLink, &bis);
                    if (FPrint()) std::printf("outfit: group %d item %d kind %d link %d (cid 0x%X): %d segments streamed\n", id, j, it.kind, it.a, ploLink->Cid(), c);
                    continue;
                }
                if (!ploLink || !FCidDerivedFrom(ploLink->Cid(), S3CID(0x30)))
                {
                    if (FPrint()) std::printf("outfit: group %d item %d kind %d link %d (cid 0x%X) arg %d: not a model, skipped\n", id, j, it.kind, it.a,
                                              ploLink ? ploLink->Cid() : -1, it.b);
                    continue;
                }
                const std::shared_ptr<GLOBSET>& pgs = static_cast<ALO*>(ploLink)->pglobset;
                if (!pgs) continue;
                const std::vector<GLOB> aglobOld = pgs->aglob;
                std::vector<int> ai;
                cglob += LoadGlobsetKeyedFromBrx(pgs.get(), it.kind == 1 ? it.b : -1, &bis, &ai);
                for (int k : ai) s_mpstreamed[plo].aglob.emplace_back(pgs, k, aglobOld[size_t(k)]);
            }
            s_mpstreamed[plo].id = id;
            EndShaderGroup();
            s_fSceneRebuild = true;
            if (FPrint()) std::printf("outfit: streamed the group %d of %d (links %d..%d): %d meshes\n", id, plo->oid, jFirst, e.b, cglob);
            return true;
        }
    }
    return false;
}

int OidStreamedShader97(int id, int i)
{
    if (!g_psw || id == -1 || i < 0 || s_pswStreamed != g_psw) return -1;
    for (LO* plo : g_psw->mpcidaplo[size_t(0x97)])
    {
        LO97* p = dynamic_cast<LO97*>(plo);
        if (!p) continue;
        auto itS = s_mpstreamed.find(plo);
        if (itS == s_mpstreamed.end() || itS->second.id != id) continue;
        for (size_t k = 0; k < p->aentry.size(); ++k)
        {
            if (p->aentry[k].a != id) continue;
            int n = 0;
            for (int j = k > 0 ? std::max(0, p->aentry[k - 1].b) : 0; j < p->aentry[k].b && j < int(p->aitem.size()); ++j)
                if (p->aitem[size_t(j)].kind == 2 && n++ == i) return p->aitem[size_t(j)].a;
        }
    }
    return -1;
}

namespace
{
OUTFIT& Of(LO* plo)
{
    OUTFIT& o = s_mpoutfit[plo];
    if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial))
    {
        o = OUTFIT{};
        o.psw = g_psw;
        o.serial = g_psw ? g_psw->serial : 0;
    }
    return o;
}

// orig3 0x1728F0: the player and the items' objects
void Resolve(LO* plo, OUTFIT& o)
{
    if (o.fResolved) return;
    o.fResolved = true;
    // (0x17E5D8(game, 3, 0x33, the object): the nearest player; natively Sly's class first, else the first player)
    if (g_psw)
    {
        for (LO* p : g_psw->mpcidaplo[S3CID(0x35)])
            if (p) { o.ploPlayer = p; break; }
        for (int cid = S3CID(0x34); !o.ploPlayer && cid < CID_Max && FCidDerivedFrom(cid, S3CID(0x34)); ++cid)
            for (LO* p : g_psw->mpcidaplo[cid])
                if (p) { o.ploPlayer = p; break; }
    }
    for (REC& r : o.arec)
    {
        for (ITEM& it : r.aitem)
            it.plo = (it.oid == 0 || it.oid == -1) ? o.ploPlayer : PloFindSwNearest(plo, it.oid);
        // (0x1728F0: the first six slots' objects: an ASEG (class 0x82) or an SM (0x84))
        for (int i = 0; i < 6; ++i)
        {
            PAIR& pr = r.apair[size_t(i)];
            if (pr.oid == 0 || pr.oid == -1) continue;
            LO* p = PloFindSwNearest(plo, pr.oid);
            pr.paseg = dynamic_cast<ASEG*>(p);
            pr.psm = dynamic_cast<SM*>(p);
            if (FPrint()) std::printf("outfit %d record 0x%X slot %d: %d %s\n", plo->oid, r.id, i, pr.oid, pr.paseg ? "aseg" : pr.psm ? "sm" : "-");
        }
    }
    if (FPrint() && g_psw)
        for (const auto& up : g_psw->aploAll)
            if (ASEG* pa = dynamic_cast<ASEG*>(up.get()))
                if (ASEGD* pd = PasegdOf(pa))
                    for (const auto& aev : pd->aaev)
                        for (const ASEGEV& ev : aev)
                            if (ev.an[0] == 0x757) std::printf("outfit: the ASEG %d has the event 0x757 (kind %d, t %.3f)\n", pa->oid, ev.kind, ev.t);
    if (FPrint())
        for (const REC& r : o.arec)
        {
            std::printf("outfit %d record 0x%X:", plo->oid, r.id);
            for (const ITEM& it : r.aitem) std::printf(" {%d %d %d %s}", it.kind, it.oid, it.value, it.plo ? "ok" : "-");
            std::printf("\n");
        }
}

// orig3 0x172D68: a record's items, flag 1 undoing it (the record worn so far), 0 doing it
void ApplyItems(const REC* pr, bool fUndo)
{
    if (!pr) return;
    for (const ITEM& it : pr->aitem)
    {
        if (!it.plo) continue;
        switch (it.kind)
        {
        case 0: case 1:
            if (fUndo) AddLo(g_psw, it.plo);     // (vt slot 8)
            else RemoveLo(g_psw, it.plo);        // (vt slot 9)
            break;
        case 2: case 3:
            if (FCidDerivedFrom(it.plo->Cid(), S3CID(0x30)))
            {
                const bool fShow = it.kind == 2 ? !fUndo : fUndo;   // (0x23FE38 shown / 0x23FEC0 hidden)
                ShowAloGlobs(static_cast<ALO*>(it.plo), it.value, fShow);
            }
            break;
        default: break;
        }
    }
}

int Irec(const OUTFIT& o, int id)
{
    if (id == -1) return -1;
    for (int i = 0; i < int(o.arec.size()); ++i)
        if (o.arec[size_t(i)].id == id) return i;
    return -1;
}

// orig3 0x173228 (the player's state entered again (vt slot 99 with -1, then 0), so its idle SM / gait are the
// outfit's or his own again): natively Sly's states 0 (the idle SM) and 12 (the run's gait)
void RestartPlayerAnim(LO* ploPlayer)
{
    SLY* p = PslyFromLo(ploPlayer);
    if (!p || (p->jts != jts::Stand && p->jts != jts::Run)) return;
    if (!(0.0f < g_clock.t)) return;   // (a script's wear while the world loads: the first state comes with the start)
    const int n = p->jts;
    p->jts = -1;
    p->jtbs = -1;
    SetSlyState(p, n, -1);
}

// The game's player entries (GAME 0x36C700 +0x18 + i x 0x10, i the player's bit in its +0x164 (0x185720)): +0xC the
// outfit worn (-1 none or the plain 0x75C), stored by 0x172E60 and worn again by the player's post-load 0x152720, so a
// disguise outlasts a world's reload. Natively keyed by the player's oid (the same character in every world).
std::unordered_map<int, int> s_mpoidPlayerIdOutfit;

// orig3 0x172E60: the record worn so far undone, the new one done (0x172D68), the player's animations restarted when
// they were / become the outfit's (0x172F48 / 0x173068 -> 0x173228), the game's player entry takes the worn id
void Apply(LO* plo, OUTFIT& o, int i)
{
    if (i == o.icur) return;
    ApplyItems(o.icur >= 0 && o.fApplied ? &o.arec[size_t(o.icur)] : nullptr, true);
    o.icur = i;
    o.fApplied = true;
    o.idPending = -1;
    ApplyItems(i >= 0 ? &o.arec[size_t(i)] : nullptr, false);
    if (o.ploPlayer)
    {
        const int id = i >= 0 ? o.arec[size_t(i)].id : -1;
        s_mpoidPlayerIdOutfit[o.ploPlayer->oid] = id == 0x75C ? -1 : id;
    }
    RestartPlayerAnim(o.ploPlayer);
    if (FPrint()) std::printf("outfit %d: wears %d (player %d)\n", plo->oid, i >= 0 ? o.arec[size_t(i)].id : -1, o.ploPlayer ? o.ploPlayer->oid : -1);
}

// orig3 0x172C30 (the record's meshes: the one worn taken off and its group unloaded when another comes (0x172BA8),
// the new one's group streamed); false: its group can't be streamed (the record is not worn: its tags would show
// missing meshes). SLY3_OUTFITNOSTREAM=1: no outfit is applied.
bool FStreamFor(LO* plo, OUTFIT& o, int i)
{
    static const bool s_fNoStream = std::getenv("SLY3_OUTFITNOSTREAM") != nullptr;
    if (s_fNoStream) return false;
    if (i < 0 || i == o.icur) return true;
    Apply(plo, o, -1);   // (0x172BA8: the outfit worn is taken off before its group goes)
    return FStreamGroup97(o.arec[size_t(i)].id);
}

// orig3 0x172D10 (the script's 534): worn at once, the change's mode off
void Wear(LO* plo, OUTFIT& o, int id)
{
    Resolve(plo, o);
    const int i = Irec(o, id);
    if (FStreamFor(plo, o, i)) Apply(plo, o, i);
    o.mode = -1;
}

// orig3 0x172AE8 (the script's 535 and the gadgets): with the change's animation when the player is free (0x1C8610):
// mode 0 = Sly's jump of kind 0x10 (0x173280), the record pending until its event 0x757 (0x173308 message 10 -> mode 1:
// applied) and the mode off at the animation's end (message 4). Natively the jump is the copied kind 16 and the
// record is also applied when the jump is over or after 0.6 s (the native clip of that kind may lack the event).
void WearChange(LO* plo, OUTFIT& o, int id)
{
    Resolve(plo, o);
    const int i = Irec(o, id);
    if (!FStreamFor(plo, o, i)) return;
    SLY* p = PslyFromLo(o.ploPlayer);
    static const bool s_fNoAnim = std::getenv("SLY3_OUTFITNOANIM") != nullptr;
    if (o.mode == 0) { o.idPending = id; return; }
    if (o.mode == 1 || i == o.icur || !p || s_fNoAnim || !p->pxpFloor || (p->jts != jts::Stand && p->jts != jts::Walk && p->jts != jts::Run))
    {
        Apply(plo, o, i);
        return;
    }
    o.idPending = id;
    o.mode = 0;
    o.tMode = g_clock.t;
    SetJumpKind(&p->jump, 16, 0, 0);
    if (FPrint()) std::printf("outfit %d: the change to %d starts (t %.3f)\n", plo->oid, id, g_clock.t);
}

REC* Plast(LO* plo) { OUTFIT& o = Of(plo); return o.arec.empty() ? nullptr : &o.arec.back(); }

struct INITOUTFIT3
{
    INITOUTFIT3()
    {
        // orig3 0x172710: a record (at most 32), its value from D_00321138's three {id, value} pairs
        RegisterObjectMethod3(0x106F00, [](LO* plo, const std::vector<CRef>& a) {
            OUTFIT& o = Of(plo);
            if (o.arec.size() < 0x20)
            {
                REC r;
                r.id = NArg(a, 0, -1);
                // (D_00321138, read from the ELF: the disguises' episodes, EPISODE numbers)
                static const int s_aidep[3][2] = {{0x759, sly3::EP_Venice}, {0x75A, sly3::EP_China}, {0x75B, sly3::EP_Pirate}};
                for (const auto& e : s_aidep)
                    if (e[0] == r.id) { r.value = e[1]; break; }
                o.arec.push_back(r);
            }
            return CRef(TAGK_Void);
        });
        // orig3 0x1727C8: the record's animation slot (not applied natively)
        RegisterObjectMethod3(0x106F88, [](LO* plo, const std::vector<CRef>& a) {
            const int i = NArg(a, 0, -1);
            if (REC* pr = Plast(plo); pr && i >= 0 && i < 30) pr->apair[size_t(i)].oid = NArg(a, 1, -1);
            return CRef(TAGK_Void);
        });
        auto item = [](LO* plo, int kind, int oid, int value) {
            if (REC* pr = Plast(plo); pr && pr->aitem.size() < 32) pr->aitem.push_back(ITEM{kind, oid, value, nullptr});
        };
        static auto s_item = item;
        RegisterObjectMethod3(0x107030, [](LO* plo, const std::vector<CRef>& a) { s_item(plo, 0, NArg(a, 0, -1), -1); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x107160, [](LO* plo, const std::vector<CRef>& a) { s_item(plo, 1, NArg(a, 0, -1), -1); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x1070B8, [](LO* plo, const std::vector<CRef>& a) { s_item(plo, 2, NArg(a, 0, -1), NArg(a, 1, -1)); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x1071E8, [](LO* plo, const std::vector<CRef>& a) { s_item(plo, 3, NArg(a, 0, -1), NArg(a, 1, -1)); return CRef(TAGK_Void); });
        // the scripts: 532 (0x172CC8: the record streamed, +0x58) / 533 (0x172CE0: the record worn, +0x50) the id, -1 none.
        // E.g. p_ext's recon check (block 62632) asks Sly's outfit object 1880 for its 533 (1883: the pirate disguise).
        RegisterObjectGetter3(532, [](LO* plo) {
            OUTFIT& o = Of(plo);
            CRef r;
            const int i = o.mode == 0 && o.idPending != -1 ? Irec(o, o.idPending) : o.icur;
            r.SetS32(i >= 0 ? o.arec[size_t(i)].id : -1);
            return r;
        });
        RegisterObjectGetter3(533, [](LO* plo) {
            const OUTFIT& o = Of(plo);
            CRef r;
            r.SetS32(o.icur >= 0 && o.fApplied ? o.arec[size_t(o.icur)].id : -1);
            return r;
        });
        // the scripts: 534 / 535 wear (at once / with the change)
        RegisterObjectMethod3(0x107290, [](LO* plo, const std::vector<CRef>& a) { Wear(plo, Of(plo), NArg(a, 0, -1)); return CRef(TAGK_Void); });
        RegisterObjectMethod3(0x107318, [](LO* plo, const std::vector<CRef>& a) { WearChange(plo, Of(plo), NArg(a, 0, -1)); return CRef(TAGK_Void); });
    }
} s_initoutfit3;
}  // namespace

void ResetOutfits3() { s_mpoutfit.clear(); s_mpstreamed.clear(); s_pswStreamed = nullptr; }

// the class-0x97 object's group unloaded when nothing is worn (0x172BA8)
static void UnloadGroup97Of(const OUTFIT&)
{
    if (!g_psw) return;
    for (LO* plo : g_psw->mpcidaplo[size_t(0x97)]) UnloadGroup97(plo);
}

bool FTakeOutfitSceneRebuild3()
{
    const bool f = s_fSceneRebuild;
    s_fSceneRebuild = false;
    return f;
}

// The outfit object dressing this player (Sly's +0x1108), null when none
static LO* PloOutfitOf(LO* ploPlayer)
{
    for (auto& [plo, o] : s_mpoutfit)
    {
        // (s3-ai) the serial too, as Of(): a world reloaded at the same SW address left the old world's entries
        // (freed objects) matching, and Resolve read them (ep3_job3061 crashed in PloFindSwNearest at frame 23856)
        if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial)) continue;
        Resolve(plo, o);
        if (o.ploPlayer == ploPlayer) return plo;
    }
    return nullptr;
}

// (natively a player may have more than one outfit object resolved to it (e.g. c_ext: the hub's with 0x75C only, the
// job's with the disguises 0x759..0x75C): the one that holds the record, else the one worn / changing, else the first)
static LO* PloOutfitWith(LO* ploPlayer, int id)
{
    LO* ploWorn = nullptr;
    for (auto& [plo, o] : s_mpoutfit)
    {
        if (o.psw != g_psw) continue;
        Resolve(plo, o);
        if (o.ploPlayer != ploPlayer) continue;
        if (id != -1 && Irec(o, id) >= 0) return plo;
        if (!ploWorn && (o.icur >= 0 || o.mode != -1)) ploWorn = plo;   // (worn or changing)
    }
    return ploWorn ? ploWorn : PloOutfitOf(ploPlayer);
}

int IdOutfitWorn3(LO* ploPlayer)
{
    LO* plo = PloOutfitWith(ploPlayer, -1);
    if (!plo) return -1;
    const OUTFIT& o = s_mpoutfit[plo];
    return o.icur >= 0 ? o.arec[size_t(o.icur)].id : -1;
}

bool FWearOutfit3(LO* ploPlayer, int id)
{
    LO* plo = PloOutfitWith(ploPlayer, id);
    if (!plo) return false;
    OUTFIT& o = s_mpoutfit[plo];
    if (id != -1 && Irec(o, id) < 0) return false;
    WearChange(plo, o, id);
    if (id == -1 && o.mode != 0) UnloadGroup97Of(o);   // (0x172BA8's end: nothing worn, the group unloaded)
    return true;
}

// orig3 0x173428 on +0x1108: a record worn (any episode; the scripts' control lock +0x64 not kept natively)
bool FOutfitWorn3(LO* ploPlayer)
{
    for (auto& [plo, o] : s_mpoutfit)
    {
        if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial) || o.ploPlayer != ploPlayer) continue;
        if (o.icur >= 0 && o.fApplied && (o.cHold == 0 || FOldOutfitHold())) return true;
    }
    return false;
}

// orig3 0x173448 / 0x173498 (from 0x22B238: a script run driving the current player (0x226BE0: the commands of the table
// 0x326480 in a script of mode 2) holds his outfit object +0x1108's count +0x64 while it does): with the count up,
// 0x173428 says no outfit, so the R1 walk, the sneak and his own gait are his again; the animation restarted when it
// changes between the outfit's and his own (0x1731B0 -> 0x173228). E.g. m_ext's attract 4262 [89]: Sly in the
// silhouette walks (R1) to 15109 at the reference's 833 a second, natively he ran at 510 and every later beat of his
// came ~27 frames late (the title card froze him before his leap). SLY3_OUTFITHOLD0=1: no hold (as before s3-intro)
void HoldOutfit3(LO* ploPlayer, bool fHold)
{
    if (!ploPlayer || FOldOutfitHold()) return;
    for (auto& [plo, o] : s_mpoutfit)
    {
        if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial) || o.ploPlayer != ploPlayer) continue;
        const bool fBefore = o.icur >= 0 && o.fApplied && o.cHold == 0;
        o.cHold = std::max(0, o.cHold + (fHold ? 1 : -1));
        const bool fAfter = o.icur >= 0 && o.fApplied && o.cHold == 0;
        if (FPrint()) std::printf("outfit %d: hold %d (player %d)\n", plo->oid, o.cHold, ploPlayer->oid);
        if (fBefore != fAfter && std::getenv("SLY3_OUTFITGAIT")) RestartPlayerAnim(ploPlayer);   // (the outfit gait natively off: nothing to switch)
    }
}

// orig3 0x1734E0 (the job's rewards 0x187270): each gang character's (D_00344050) outfit object +0x1108 wears nothing at
// once (0x172D10(outfit, -1)): a disguise worn through a job ends with it
void TakeOffOutfits3()
{
    if (std::getenv("SLY3_NOJOBOUTFITOFF")) return;
    for (auto& [plo, o] : s_mpoutfit)
    {
        if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial)) continue;
        Resolve(plo, o);
        if (o.ploPlayer && (o.icur >= 0 || o.mode != -1)) Wear(plo, o, -1);
    }
}

bool FPlayerDisguised3(LO* ploPlayer)
{
    // (natively a player may have more than one outfit object resolved to it: any with a record worn)
    for (auto& [plo, o] : s_mpoutfit)
    {
        if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial) || o.ploPlayer != ploPlayer) continue;
        // 0x173428: a record worn (+0x50) and the scripts' control lock (+0x64) not taken (not kept natively: 0)
        if (o.icur < 0 || !o.fApplied || (o.cHold != 0 && !FOldOutfitHold())) continue;
        const int a = IepisodeCur(), b = o.arec[size_t(o.icur)].value;   // 0x25E188 / 0x172CF8
        if (a == b || a == -1 || b == -1) return true;
    }
    return false;
}

// (s3-player) the change is under way (the outfit object's +0x48 != -1: SCPT 0x6A's done test, orig3 0x229438 case 106)
bool FOutfitChanging3(LO* ploPlayer)
{
    LO* plo = PloOutfitWith(ploPlayer, -1);
    return plo && s_mpoutfit[plo].mode != -1;
}

// orig3 0x173308 (the outfit object's message handler): the player's animation event 0x757 during the change: the
// pending record worn
void OnOutfitEvent3(LO* ploPlayer, int n)
{
    if (n != 0x757) return;
    LO* plo = PloOutfitWith(ploPlayer, -1);
    if (!plo) return;
    OUTFIT& o = s_mpoutfit[plo];
    if (o.mode != 0) return;
    if (FPrint()) std::printf("outfit %d: the change's event 0x757 (t %.3f)\n", plo->oid, g_clock.t);
    Apply(plo, o, Irec(o, o.idPending));
    o.mode = 1;
}

// orig3 0x152720's 0x172D10 (the player's post-load): the outfit of the game's player entry worn again at once (a
// disguise kept across a reload / a checkpoint). Once per world; an id this world's outfit objects lack: nothing
// (0x172A90 finds no record, 0x172E60 of none with none worn changes nothing). SLY3_NOOUTFITKEEP=1: not done.
void RestoreOutfit3(LO* ploPlayer)
{
    if (!ploPlayer || !g_psw) return;
    static const bool s_fOff = std::getenv("SLY3_NOOUTFITKEEP") != nullptr;
    static std::unordered_map<int, uint32_t> s_mpoidSerial;
    uint32_t& serial = s_mpoidSerial[ploPlayer->oid];
    if (serial == g_psw->serial + 1) return;
    serial = g_psw->serial + 1;
    const auto it = s_mpoidPlayerIdOutfit.find(ploPlayer->oid);
    if (s_fOff || it == s_mpoidPlayerIdOutfit.end() || it->second == -1) return;
    const int id = it->second;
    LO* plo = PloOutfitWith(ploPlayer, id);
    if (!plo) return;
    OUTFIT& o = s_mpoutfit[plo];
    if (Irec(o, id) < 0 || o.icur >= 0) return;
    Wear(plo, o, id);
    if (FPrint()) std::printf("outfit %d: the player %d's entry %d worn again\n", plo->oid, ploPlayer->oid, id);
}

void RestoreOutfits3(SW* psw)
{
    if (!psw || psw != g_psw) return;
    for (int cid = S3CID(0x33); cid >= 0 && cid < CID_Max && cid < g_acls[S3CID(0x33)].cidLast; ++cid)
        for (LO* p : std::vector<LO*>(psw->mpcidaplo[size_t(cid)])) RestoreOutfit3(p);
}

int IdOutfitEntry3(LO* ploPlayer)
{
    if (!ploPlayer) return -1;
    const auto it = s_mpoidPlayerIdOutfit.find(ploPlayer->oid);
    return it == s_mpoidPlayerIdOutfit.end() ? -1 : it->second;
}

// each frame (the player's gadgets' update): the change's end (0x173308 message 4: the mode off); natively also the
// pending record worn when the jump is over or after 0.6 s
void UpdateOutfit3(LO* ploPlayer)
{
    RestoreOutfit3(ploPlayer);
    LO* plo = PloOutfitOf(ploPlayer);
    if (!plo) return;
    // (debug: SLY3_WEAROUTFIT="T:id": at game time T the player wears outfit id, e.g. 1881 Venice's disguise)
    if (static const char* s_sz = std::getenv("SLY3_WEAROUTFIT"); s_sz)
    {
        static bool s_fDone = false;
        float t = 0;
        int id = -1;
        if (!s_fDone && std::sscanf(s_sz, "%f:%d", &t, &id) == 2 && g_clock.t >= t)
        {
            s_fDone = true;
            int c = 0;
            for (auto& [ploO, o] : s_mpoutfit)
                if (o.psw == g_psw && o.ploPlayer == ploPlayer && Irec(o, id) >= 0) { WearChange(ploO, o, id); ++c; }
            std::printf("outfit: debug wear %d (player %d) t %.3f: %d outfit objects\n", id, ploPlayer->oid, g_clock.t, c);
        }
    }
    // (natively a player may have more than one outfit object resolved to it: each one's change runs)
    for (auto& [ploO, o] : s_mpoutfit)
    {
        if (o.psw != g_psw || (g_psw && o.serial != g_psw->serial) || o.ploPlayer != ploPlayer || o.mode == -1) continue;
        SLY* p = PslyFromLo(ploPlayer);
        const bool fOver = !p || (p->jts != jts::Jump && 0.1f < g_clock.t - o.tMode);
        if (o.mode == 0 && (fOver || 0.6f < g_clock.t - o.tMode)) { Apply(ploO, o, Irec(o, o.idPending)); o.mode = 1; }
        if (o.mode == 1 && fOver)
        {
            o.mode = -1;
            if (o.icur < 0) UnloadGroup97Of(o);
        }
    }
}

// orig3 0x1733F8 / 0x173408 (the worn outfit's idle SM (slot 0) / gait (slots 1 / 2); null: the player's own)
SM* PsmOutfitIdle3(LO* ploPlayer)
{
    LO* plo = PloOutfitWith(ploPlayer, -1);
    if (!plo) return nullptr;
    OUTFIT& o = s_mpoutfit[plo];
    // (natively off: Sly's state 0 on the outfit's SM never returns (the SM's run with these states loops: not found
    // yet); SLY3_OUTFITSM=1 turns it on for that work)
    static const bool s_fOn = std::getenv("SLY3_OUTFITSM") != nullptr;
    return o.icur >= 0 && o.fApplied && s_fOn ? o.arec[size_t(o.icur)].apair[0].psm : nullptr;
}
GAIT* PgaitOutfit3(LO* ploPlayer, int sfx)
{
    LO* plo = PloOutfitWith(ploPlayer, -1);
    if (!plo) return nullptr;
    OUTFIT& o = s_mpoutfit[plo];
    // (natively off: with the outfit's gait Sly leaves the floor after 6 frames of running and stays in the air (its
    // clips' root velocity as BindGait reads it: not found yet); SLY3_OUTFITGAIT=1 turns it on for that work)
    static const bool s_fOn = std::getenv("SLY3_OUTFITGAIT") != nullptr;
    if (o.icur < 0 || !o.fApplied || !s_fOn) return nullptr;
    REC& r = o.arec[size_t(o.icur)];
    if (!r.apair[1].paseg) return nullptr;
    if (!r.pgait && FCidDerivedFrom(ploPlayer->Cid(), S3CID(0x30)))
    {
        // (0x1728F0: 0x258930(the record +0x30, the player, his footstep sound +0x1114), the clips +0x44 / +0x84 from the
        // slots 1 / 2, 0x258958 = BindGait)
        r.pgait = std::make_shared<GAIT>();
        InitGait(r.pgait.get(), static_cast<ALO*>(ploPlayer), sfx);
        r.pgait->apaseg[0] = r.apair[1].paseg;
        r.pgait->apaseg[1] = r.apair[2].paseg;
        BindGait(r.pgait.get());
        if (FPrint())
        {
            const GAIT& g = *r.pgait;
            std::printf("outfit gait %d: clips %d %d vel %.1f %.1f %.1f / %.1f %.1f %.1f bl %d", o.arec[size_t(o.icur)].id, g.apaseg[0] ? g.apaseg[0]->oid : -1,
                        g.apaseg[1] ? g.apaseg[1]->oid : -1, g.avel[0].x, g.avel[0].y, g.avel[0].z, g.avel[1].x, g.avel[1].y, g.avel[1].z, g.pbl ? 1 : 0);
            if (g.pbl)
                for (const BLENDBL& bl : g.pbl->abl)
                    for (const auto& am : bl.aamrsg)
                        for (const MRSG& m : am) std::printf(" [%.3f %.3f]", m.t, m.dt);
            std::printf("\n");
            for (int i = 0; i < 2; ++i)
                if (const ASEGD* pd = g.apaseg[i] ? PasegdOf(g.apaseg[i]) : nullptr)
                {
                    std::printf("  clip %d: tMax %.3f chn %zu chg %zu ev %zu/%zu/%zu:", g.apaseg[i]->oid, pd->tMax, pd->achn.size(), pd->achg.size(), pd->aaev[0].size(), pd->aaev[1].size(), pd->aaev[2].size());
                    for (const ASEGEV& ev : pd->aaev[1]) std::printf(" (k%d %d t%.3f)", ev.kind, ev.an[0], ev.t);
                    std::printf("\n");
                }
        }
    }
    return r.pgait && r.pgait->pbl ? r.pgait.get() : nullptr;
}
