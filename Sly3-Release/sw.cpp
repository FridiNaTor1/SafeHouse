#include "carry.h"
#include "game/sly3.h"
#include "breakable.h"
// The world loader (orig3 0x14F440; Sly 2 0x13DA18) and the global blocks of a world file.

#include "sw.h"

#include "game/cond3.h"
#include "game/sly3.h"

#include <algorithm>
#include "textmod.h"
#include "hide.h"
#include "emitter.h"
#include "laser.h"
#include "vehiclehooks.h"
#include "booty.h"
#include "pocket.h"

#include "activate.h"
#include "guard.h"
#include "gang.h"
#include "aipursuit.h"
#include "ainoise.h"

#include "bis.h"
#include "disc.h"
#include "alo.h"
#include "so.h"
#include "physworld.h"
#include "savedata.h"
#include "spliceglue.h"
#include "brx.h"
#include "readtrace.h"
#include "glob.h"
#include "shd.h"
#include "sound.h"
#include "region.h"
#include "gc.h"
#include "serialize.h"
#include "splicemap.h"
#include "update.h"
#include "navlink.h"
#include "nav.h"
#include "screenrt.h"
#include "overlay.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

SW* g_psw = nullptr;

// The current world's side tables that point at its objects (~SW's first part; also pccharacter.cpp after loading a
// character world beside the game's, before the next world loads)
void ReleaseSwRuntimeTables()
{
    ResetVehicles();  // unlink vehicle controllers and contacts before their XFs disappear
    // Guard animation listeners must be released before their world's objects and target records.
    ResetGuards();
    ResetTargetRecs();
    // orig: 0x20E890 replaces GAME's noise list/pool and serial counter.
    // Jobs retain removed noises, so release jobs before their noise data;
    // discard listeners before those objects can be reused by a new world.
    ResetLoListeners();
    ResetNoises();
    // orig: 0x1CDE80 removes each world-owned RIP before its model objects are released.
    // Reset lookup generations here too: a paused new world can draw before emitter updates.
    ResetParticles();
    ResetNav();  // regions/polygons point into this world's meshes and are not valid after unload
    // orig: 0x20E068 -> 0x1AE300: screen players and their loaded groups belong to this world.
    // New-world start scripts/wipes run before InitHudWidgets, so drop these before its objects die.
    ResetScreenObjs(nullptr);
    ResetAnimatedOverlays();  // the exclusive owner and captured sheets belong to this world
    // The original posted-call list belongs to this SW. Native callbacks keep raw object
    // pointers, so discard it before the owned objects (also for temporary loader worlds).
    ClearDeferredSpliceEvents();
}

// The world's unload: the splice runtime's frames (their bindings break the reference cycles) and tables.
SW::~SW()
{
    if (g_psw == this) ReleaseSwRuntimeTables();
    ReleaseMurrayScaleControllers(this);
    DeleteSwNavLinks(this);
    aploAll.clear();
    g_gc.ShutDown();
    DeleteSwSplice(this);
    if (g_psw == this) g_psw = nullptr;
    InstallSpliceHooks(nullptr);
}

// orig 0x15E3E8's node check for an object of k -1 (InitLo's default; 7499 of w32's 7578 objects) in a global find
// (the context list D_003E139C, the world's root node): an object matches only when its namespace node (+0x3C) is the
// root's, so a group's objects (node (iclass << 16 | inst) -> root, e.g. the bear guard 33440 of FK$Xnpc_bear) and a
// proxy record's instance objects are not found by their own oid; the instance's alias entry (SW +0x1838, oidInstance)
// is, its node's parent being the root. (k 0 / 1 objects not modelled: TODO(world); the old first-match is the fallback.)
bool FLoInWorldNs(const LO* plo)
{
    // (a shared group (WE +0x5C, e.g. the level's own objects) loads into the world's node; the others into theirs)
    const auto fGroupNs = [](const LO* p) {
        return p->ploNs || p->ploNsRow || (p->ichannel >= 0 && g_psw && p->ichannel < static_cast<int>(g_psw->awe.size()) && !g_psw->awe[p->ichannel].x5C);
    };
    if (fGroupNs(plo)) return false;
    const LO* p = plo;
    while (p->paloParent) p = p->paloParent;
    return !fGroupNs(p);
}

const std::vector<LO*>& AploOfOid(const SW* psw, OID oid)
{
    SW::OIDINDEX& x = psw->oidindex;
    if (x.cDone > psw->aploAll.size()) x = SW::OIDINDEX{};   // (defensive: the objects were released)
    for (; x.cDone < psw->aploAll.size(); ++x.cDone)
    {
        LO* plo = psw->aploAll[x.cDone].get();
        x.mpipos[plo] = uint32_t(x.cDone);
        x.aploAll.push_back(plo);
        x.mp[plo->oid].push_back(plo);
        if (plo->oidInstance != -1 && plo->oidInstance != plo->oid) x.mp[plo->oidInstance].push_back(plo);
    }
    if (oid == -1) return x.aploAll;
    static const std::vector<LO*> s_aploNone;
    auto it = x.mp.find(oid);
    const std::vector<LO*>& a = it == x.mp.end() ? s_aploNone : it->second;
    if (static const bool s_fCheck = std::getenv("SLY2N_OIDINDEXCHECK") != nullptr; s_fCheck)   // (debug: vs a scan)
    {
        std::vector<LO*> aScan;
        for (const auto& up : psw->aploAll)
            if (up->oid == oid || up->oidInstance == oid) aScan.push_back(up.get());
        if (aScan != a) std::printf("oidindex: oid %d: %zu indexed, %zu scanned\n", int(oid), a.size(), aScan.size());
    }
    return a;
}

void NoteOidInstanceChanged(const SW* psw, LO* plo, OID oidOld)
{
    if (!psw || !plo || plo->oidInstance == oidOld) return;
    SW::OIDINDEX& x = psw->oidindex;
    auto itPos = x.mpipos.find(plo);
    if (itPos == x.mpipos.end()) return;   // (not indexed yet: indexed with its new name)
    if (oidOld != -1 && oidOld != plo->oid)
    {
        std::vector<LO*>& a = x.mp[oidOld];
        a.erase(std::remove(a.begin(), a.end(), plo), a.end());
    }
    if (plo->oidInstance != -1 && plo->oidInstance != plo->oid)
    {
        std::vector<LO*>& a = x.mp[plo->oidInstance];
        const uint32_t ipos = itPos->second;
        auto it = std::lower_bound(a.begin(), a.end(), ipos, [&](const LO* p, uint32_t i) { return x.mpipos.at(p) < i; });
        if (it == a.end() || *it != plo) a.insert(it, plo);
    }
}

LO* PloFindSwObjectByOid(SW* psw, OID oid)
{
    // orig 0x15E938 kind 5 (global): the world's objects of the oid first
    LO* ploFirst = nullptr;
    for (LO* plo : AploOfOid(psw, oid))
        if (plo->oid == oid)
        {
            if (FLoInWorldNs(plo) || plo->oidInstance == oid) return plo;
            if (!ploFirst) ploFirst = plo;
        }
    return ploFirst;
}

// orig 0x001D7B88: the progress names (a hash table name -> value) and the progress records
void LoadProgressFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(0x1D7B88);
    const int c = pbis->U16Read();
    for (int i = 0; i < c; ++i)
    {
        const int v = pbis->U16Read();
        psw->progress.aname.emplace_back(v, pbis->ReadStringSw());
    }
    const int cr = pbis->U16Read();
    psw->progress.abRecords.resize(static_cast<std::size_t>(cr) * 10);
    pbis->Read(psw->progress.abRecords.size(), psw->progress.abRecords.data());
    // orig 0x1D7C80: the save data at 0x3D4A60 checked against this layout
    SetProgressLayout(psw->progress);
    ValidateGameSave();
}

// orig 0x0020F318: the world records (the game's levels: file key, episode, kind)
void LoadWorldRecordsFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(0x20F318);
    psw->cwrec = pbis->U32Read();
    psw->x3E1C3C = pbis->U32Read();
    psw->awrec.assign(psw->cwrec, WREC{});
    for (uint32_t i = 0; i < psw->cwrec; ++i)
    {
        WREC& w = psw->awrec[i];
        pbis->ReadRecord64(w.abRecord, KEYK_World, int(i));
        w.fk = FkFromRecord(w.abRecord);
        w.slot = pbis->U8Read();
        w.kind = pbis->U8Read();
        w.fMain = pbis->U8Read() != 0;  // the episode's main world (orig 0x3E1C50[slot] = the record)
        const int c = pbis->U8Read();
        w.an.resize(c);
        for (int j = 0; j < c; ++j) w.an[j] = pbis->U16Read();
    }
}

// orig 0x0013DF80: the strings of the chosen language (id -> string)
void LoadTextFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(0x13DF80);
    const int clang = pbis->U8Read();
    int ilang = 0;
    for (int j = 0; j < clang; ++j)
        if (pbis->U8Read() == g_nLang) ilang = j;
    const int c = pbis->S16Read();
    for (int i = 0; i < c; ++i)
    {
        const int id = pbis->S16Read();
        std::string str;
        for (int j = 0; j < clang; ++j)
        {
            if (FWideText()) { if (j == ilang) str = pbis->ReadWideStringSw(); else pbis->SkipWideStringSw(); }
            else if (j == ilang) str = pbis->ReadStringSw();
            else pbis->SkipStringSw();
        }
        // (debug: SLY2N_TEXTDUMP=1 prints the strings as they load: the text ids of each disc, region.cpp's IdText)
        if (static const bool s_f = std::getenv("SLY2N_TEXTDUMP") != nullptr; s_f) std::printf("text 0x%X: %s\n", id, str.c_str());
        psw->atext.emplace_back(id, std::move(str));
    }
    // the Japanese build (orig JP 0x13E2F8): then one more string per language, the characters the world's text uses
    // (JP SW +0x4848): the fonts keep only those glyphs (JP 0x13E490). The port keeps every glyph.
    if (FWideText())
        for (int j = 0; j < clang; ++j)
        {
            if (j == ilang) psw->strTextChars = pbis->ReadWideStringSw();
            else pbis->SkipWideStringSw();
        }
    {
        // (native: the mods folder's strings, textmod.h; the ids they change keep them over the PC wording, porttext.h)
        const std::vector<std::pair<int, std::string>> atextDisc = psw->atext;
        ApplyTextMods(psw->atext);
        psw->aidTextModded.clear();
        for (size_t i = 0; i < psw->atext.size() && i < atextDisc.size(); ++i)
            if (psw->atext[i].second != atextDisc[i].second) psw->aidTextModded.push_back(psw->atext[i].first);
    }
}

// orig3 0x14FF58 (Sly 2 0x13E0E0, the node tree): U8 sets x {S32 condition, U16 c, c x {VECTOR4, S16, S16}}; a set
// whose condition fails (0x169150 against the static camera's) is skipped (the same 20 bytes per node)
void LoadNodesFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(sly3::FN_LOAD_NODES);
    psw->anode.clear();
    psw->cnodeLeaf = 0;
    const int cset = pbis->U8Read();
    for (int iset = 0; iset < cset; ++iset)
    {
        const int32_t cond = pbis->S32Read();
        const int c = pbis->U16Read();
        if (!FConditionMatches3(cond))
        {
            pbis->Skip(std::size_t(c) * 0x14);
            continue;
        }
        for (int i = 0; i < c; ++i)
        {
            NODE n;
            n.vec = pbis->ReadVector4();
            n.inodeA = pbis->S16Read();
            if (n.inodeA < 0) n.ileafA = psw->cnodeLeaf++;
            n.inodeB = pbis->S16Read();
            if (n.inodeB < 0) n.ileafB = psw->cnodeLeaf++;
            psw->anode.push_back(n);
        }
    }
}

// orig3 0x14FA38: two tables of U8 n x {S32 key, S8 value, U8 c, S32 list[c]} before the group count. The original
// picks the value whose key is the task system's current job / task (0x36C700 +0x1498, then +0x14A0) and whose list
// holds the world's index, and hands it to 0x196228. TODO(sly3): 0x196228 (the entry point it selects).
void LoadPrefixFromBrx3(std::vector<SW::PREFIX3>& a, CBinaryInputStream* pbis)
{
    const int n = pbis->U8Read();
    for (int i = 0; i < n; ++i)
    {
        SW::PREFIX3 p;
        p.key = pbis->S32Read();
        p.value = pbis->S8Read();
        const int c = pbis->U8Read();
        for (int j = 0; j < c; ++j) p.alist.push_back(pbis->S32Read());
        a.push_back(std::move(p));
    }
}

// orig3 0x1500F0 (new in Sly 3): the option sets. U8 x {U32 key, U32 value}; U8 x {U16 array (0x119968), U32 key,
// U32 value}; U8 n blocks: n U32 ids, then n x {S32 cb, options}. The block chosen: the value of table A's entry whose
// key is the current job (0x36C700 +0x1498), else table B's whose key is the current task (+0x14A0), else the last
// table B entry with NProgress > 0 of its array (TODO(sly3): with the save); 12 means none; the block of that id, else
// the first. Its options load into the static camera (aoptCm), its id becomes the camera's condition (+0x10); the
// others are skipped by their size. Then 0x1694D8 adds the value to the camera's extra conditions.
void LoadOptionSetsFromBrx3(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(sly3::FN_LOAD_OPTION_SETS);
    const int n1 = pbis->U8Read();
    for (int i = 0; i < n1; ++i)
    {
        SW::OPTSETKEY3 k;
        k.key = pbis->U32Read();
        k.value = pbis->U32Read();
        psw->aoptsetA.push_back(k);
    }
    const int n2 = pbis->U8Read();
    for (int i = 0; i < n2; ++i)
    {
        SW::OPTSETKEY3 k;
        const int c = pbis->U16Read();
        k.aprogress.resize(std::size_t(c));
        pbis->Read(std::size_t(c) * 2, k.aprogress.data());
        k.key = pbis->U32Read();
        k.value = pbis->U32Read();
        psw->aoptsetB.push_back(k);
    }
    int32_t value = -1;
    if (psw->keyJob != -1)
        for (const SW::OPTSETKEY3& k : psw->aoptsetA)
            if (int32_t(k.key) == psw->keyJob) { value = int32_t(k.value); break; }
    if (value == -1 && psw->keyTask != -1)
        for (const SW::OPTSETKEY3& k : psw->aoptsetB)
            if (int32_t(k.key) == psw->keyTask) { value = int32_t(k.value); break; }
    if (value == 12) value = -1;
    const int n3 = pbis->U8Read();
    psw->aidOptset.resize(std::size_t(n3));
    for (uint32_t& id : psw->aidOptset) id = pbis->U32Read();
    int iblock = 0;
    for (int i = 0; i < n3; ++i)
        if (int32_t(psw->aidOptset[std::size_t(i)]) == value) { iblock = i; break; }
    psw->ioptsetChosen = n3 > 0 ? iblock : -1;
    g_condset3.cond = n3 > 0 ? int32_t(psw->aidOptset[std::size_t(iblock)]) : value;  // CM +0x10
    g_condset3.acond.clear();
    for (int i = 0; i < n3; ++i)
    {
        const int32_t cb = pbis->S32Read();
        const std::size_t ib = pbis->m_ib;
        if (i != iblock)
        {
            pbis->Skip(std::size_t(cb));  // (the original skips them: their load methods must not run)
            continue;
        }
        LoadOptionsFromBrx(psw->aoptCm, nullptr, pbis);
        if (pbis->m_ib - ib != std::size_t(cb)) throw std::runtime_error("option sets: the chosen block's size does not match its options");
        // the descriptors' fields / setters on the static camera 0x36BC10 (the fog, its colour, the world shader colour,
        // the conditions added: cond3.h), in the block's order as the original's load runs them
        for (const OPT& o : psw->aoptCm) FApplyCondSetOption3(o);
    }
    // the chosen block's group instance counts (EOPID 1032: method 0x10AFB0 -> 0x169500(group id, count): the group
    // whose record +0x10 is the id gets that count; a group with none is skipped by its size)
    for (const OPT& o : psw->aoptCm)
        if (o.eopid == sly3::EOPID_GROUP_INSTANCES && o.aval.size() >= 2)
            for (WE& we : psw->awe)
                if (we.x10 == o.aval[0].n) we.cinst = o.aval[1].n;
    if (value != -1 && !FConditionMatches3(value)) g_condset3.acond.push_back(value);  // 0x1694D8
    // (debug: SLY3_CMOPTPRINT=1: the chosen block's options with their descriptors, orig3 0x32CA80)
    if (std::getenv("SLY3_CMOPTPRINT"))
        for (const OPT& o : psw->aoptCm)
        {
            const uint32_t va = 0x32CA80 + uint32_t(o.eopid) * sly3::CB_EOPID;
            std::printf("cmopt eopid %d k %u grf 0x%x +8 0x%x +14 0x%x +18 0x%x +1C 0x%x n %d g %g %g %g\n", o.eopid,
                        g_disc.U32Elf(va), g_disc.U32Elf(va + 4), g_disc.U32Elf(va + 8), g_disc.U32Elf(va + 0x14),
                        g_disc.U32Elf(va + 0x18), g_disc.U32Elf(va + 0x1C), o.aval.empty() ? 0 : o.aval[0].n,
                        o.aval.empty() ? 0.0f : o.aval[0].ag[0], o.aval.empty() ? 0.0f : o.aval[0].ag[1],
                        o.aval.empty() ? 0.0f : o.aval[0].ag[2]);
        }
}

// orig 0x001C0EC0 / 0x001C0F30: a group's collision mesh
void LoadCmeshFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(0x1C0EC0);
    const uint32_t c = pbis->U32Read();
    if (!c) return;
    auto pcmesh = std::make_unique<CMESH>();
    {
        LOADFN(0x1C0F30);
        const uint32_t cvec = pbis->U32Read();
        const uint32_t cn = pbis->U32Read();
        pcmesh->ae.resize(c);
        for (uint32_t i = 0; i < c; ++i)
        {
            pcmesh->ae[i].ivec = pbis->U16Read();
            pcmesh->ae[i].a = static_cast<int16_t>(pbis->U16Read());
            pcmesh->ae[i].b = static_cast<int16_t>(pbis->U16Read());
        }
        pcmesh->avec.resize(cvec);
        for (uint32_t j = 0; j < cvec; ++j) pcmesh->avec[j] = pbis->ReadVector4();
        pcmesh->an.resize(cn);
        for (uint32_t j = 0; j < cn; ++j) pcmesh->an[j] = pbis->S32Read();
    }
    psw->acmesh.push_back(std::move(pcmesh));
}

// orig3 0x14F440 LoadSwFromBrx (Sly 2 0x13DA18; the order of Sly 3's blocks: the old port's world3.md)
void LoadSwFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(sly3::FN_LOAD_SW);
    g_psw = psw;
    g_nFilterCur = psw->filter;
    g_nFilterCur3 = psw->filter;  // (orig3 0x47989C: the world's index)
    InstallSpliceHooks(psw);
    {
        LOADFN(sly3::FN_LOAD_PREFIX);
        LoadPrefixFromBrx3(psw->aprefixA, pbis);
        LoadPrefixFromBrx3(psw->aprefixB, pbis);
    }
    const int cwe = pbis->S16Read();
    psw->awe.assign(cwe > 0 ? cwe : 0, WE{});
    for (WE& we : psw->awe)
    {
        pbis->Read(64, we.achName);
        we.x5C = pbis->S32Read();
        we.x60 = pbis->S32Read();
        we.x10 = pbis->S32Read();
        we.x14 = pbis->S32Read();
        we.cinst = pbis->S32Read();
        we.cb = pbis->U32Read();   // (new in Sly 3)
    }
    LoadProgressFromBrx(psw, pbis);
    // TODO(sly3): 0x25B1B8 (no reads: progress names resolved into SW +0x406C..), 0x1CF640 (no reads: the save slot,
    // the game settings)
    LoadMusicFromBrx(pbis);  // orig3 0x246F58 (Sly 2's reads)
    LoadTextFromBrx(psw, pbis);  // orig3 0x14FD20 (UTF-16, plus one more string per language)
    LoadOptionsFromBrx(psw->aoptSw, nullptr, pbis);  // orig3 0x1519C8: one block (Sly 2 had the camera's second)
    LoadOptionSetsFromBrx3(psw, pbis);
    // TODO(sly3): 0x25EB00 (no reads)
    LoadNodesFromBrx(psw, pbis);
    pbis->Align(16);
    // TODO(sly3): StartupSwSplice 0x249980, 0x25B6B8 / 0x169398 (no reads), the load heaps
    ResetShaders();  // (the renderer's tables)
    for (int i = 0; i < cwe; ++i)
    {
        WE& we = psw->awe[i];
        if (we.cinst == 0)
        {
            // a group without instances: its data is skipped by the size in its record
            pbis->Skip(we.cb);
            continue;
        }
        const std::size_t ibGroup = pbis->m_ib;
        psw->iwe = we.x5C ? -1 : i;
        psw->iweChannel = i;
        LoadBulkHeaderFromBrx(psw, pbis, true);  // orig3 0x25C930 (shd.cpp)
        {
            LOADFN(sly3::FN_LOAD_SW);
            psw->fScriptDebug = pbis->S32Read();
        }
        g_fLoadDebugInfo = psw->fScriptDebug;
        if (g_walk.fOn) WalkSpliceFromBrx(psw, we.x10, pbis);
        else LoadSwSpliceFromBrx(psw, we.x10, pbis);  // orig3 0x2499F0: parse + module init
        LoadShadersFromBrx(psw, pbis);  // orig3 0x234EA0 (shd.cpp), incl. the bulk block 0x25C9A0
        LoadCmeshFromBrx(psw, pbis);    // orig3 0x1ED7B0 (Sly 2's reads)
        std::vector<LO*> aplo;
        LOSINK sink;
        sink.papo = &aplo;
        LoadSwObjsFromBrx(psw, nullptr, pbis, &sink);
        EndSwSpliceGroup(psw);
        pbis->Align(16);
        if (pbis->m_ib - ibGroup != we.cb)
            throw std::runtime_error("group " + we.StrName() + ": its data ends at " + std::to_string(pbis->m_ib - ibGroup) +
                                     ", its record says " + std::to_string(we.cb));
        // TODO(sly3): 0x162A88
        EndShaderGroup();  // orig3 0x235DE0
        we.cloLoaded = static_cast<int>(aplo.size());
        we.aplo = aplo;
        if (g_walk.fOn) continue;
        for (int copy = 1; copy < we.cinst; ++copy)
            for (int j = 0; j < we.cloLoaded; ++j)
            {
                LO* plo = PloCloneLo(aplo[j], psw, nullptr);
                plo->iclone = copy;
                we.aplo.push_back(plo);
            }
        if (we.x5C) ++we.cUsed;
        else
            for (LO* plo : we.aplo) RemoveLo(psw, plo);  // vt slot 9 of each (proxies and scripts add them back)
        if (const char* sz = std::getenv("SLY3_GROUPDBG"))
            for (LO* plo : we.aplo)
                if (plo->oid == std::atoi(sz) || std::atoi(sz) == -1)
                    std::fprintf(stderr, "group %d (%s) x5C %d x60 %d cinst %d: oid %d clone %d removed %d\n", i, we.StrName().c_str(), we.x5C,
                                 we.x60, we.cinst, plo->oid, plo->iclone, plo->fRemoved);
    }
    // TODO(sly3): 0x236B80, LoadSoundBank 0x247470
    psw->iwe = -2;
    psw->iweChannel = -1;
    if (g_walk.fOn) return;
    // the transform records' world matrices (orig: the first transform pass), parents first
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& plo : psw->aploAll)
            if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), S3CID(0x30))) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
    glm::mat3 NormalizeRotateMatrix3(const glm::mat3& m);  // xfmath.h (0x11B3D0)
    // orig: 0x001CA500 (the groups with spare instances: each object's rest pose from the first spare, 0x1A62E8 and
    // the world rotation normalized by 0x11B3D0)
    for (WE& we : psw->awe)
    {
        we.aposRest.assign(static_cast<std::size_t>(std::max(we.cloLoaded, 0)), glm::vec3(0.0f));
        we.amatRest.assign(static_cast<std::size_t>(std::max(we.cloLoaded, 0)), glm::mat3(1.0f));
        we.amatRootRest.assign(static_cast<std::size_t>(std::max(we.cloLoaded, 0)), glm::mat4(1.0f));
        if (we.cUsed >= we.cinst) continue;
        // (each object of the spare instances cUsed..cinst: SetIdProxy (i << 16 | j, 1), vt slot 25 0x1CA118 / ALO
        // 0x1CA308: the row's node for the object and its children's trees; the node's +0x24 = 1)
        const auto setNsRow = [](auto&& self, LO* p, LO* ploNode) -> void {
            p->ploNsRow = ploNode;
            if (FCidDerivedFrom(p->Cid(), S3CID(0x30)))
                for (LO* pc : static_cast<ALO*>(p)->aploChild)
                    if (pc) self(self, pc, ploNode);
        };
        for (int j = std::max(we.cUsed, 0); j < we.cinst; ++j)
            for (int k = 0; k < we.cloLoaded; ++k)
                if (const std::size_t i = static_cast<std::size_t>(j) * we.cloLoaded + k; i < we.aplo.size())
                {
                    we.aplo[i]->fSpareNs = true;
                    setNsRow(setNsRow, we.aplo[i], we.aplo[static_cast<std::size_t>(j) * we.cloLoaded]);
                }
        for (int k = 0; k < we.cloLoaded; ++k)
        {
            const std::size_t i = static_cast<std::size_t>(we.cUsed) * we.cloLoaded + k;
            if (i >= we.aplo.size() || !FCidDerivedFrom(we.aplo[i]->Cid(), S3CID(0x30))) continue;
            ALO* palo = static_cast<ALO*>(we.aplo[i]);
            we.aposRest[k] = PosAloCenter(palo);
            glm::mat3 mat = glm::mat3(PxfObj(palo) ? PxfObj(palo)->matWorld : glm::mat4(1.0f));  // (vt slot 31: the object record's world)
            mat = NormalizeRotateMatrix3(mat);  // 0x11B3D0
            we.amatRest[k] = mat;
            if (!palo->axf.empty()) we.amatRootRest[k] = palo->axf[0]->mat;
        }
    }
}

// Loads a world file into psw; returns false (with a message) on an error.
bool FLoadWorldFile(SW* psw, const FK& fk, const std::string& trace, std::size_t* pcbRead, std::size_t* pcbFile)
{
    return FLoadWorldBytes(psw, fk, g_disc.ReadWorld(fk), trace, pcbRead, pcbFile);
}

// (the old world's side tables the loader resets before a load: FLoadWorldFile's start; PC: also after a kept world's
// load, pcloader.h)
void ResetWorldLoadTables()
{
    ResetGuards();
    ResetTargetRecs();
    g_physw.Reset();  // also releases the old actor job stacks before loading new objects
    ResetBreakables();  // (breakable.cpp: the tables of the old world's objects)
    ResetLasers();
    void ResetBirds();  // (bird.cpp)
    ResetBirds();
    void ResetHacks();  // (hack.cpp)
    ResetHacks();
    void ResetDances();  // (dance.cpp)
    ResetDances();
    ResetCarries();     // (carry.cpp)
    ResetBooties();     // (booty.cpp)
    ResetHides();       // (hide.cpp)
    ResetPockets();     // (pocket.cpp: the givers' pockets, the rewards, the pickups)
    // (the side tables keyed by the old world's objects: a new object may get a freed one's address)
    void ResetLaunchers(); ResetLaunchers();          // (launcher.cpp)
    void ResetPads(); ResetPads();                    // (pad.cpp)
    void ResetProjectiles(); ResetProjectiles();      // (projectile.cpp)
    void ResetBarriers(); ResetBarriers();            // (barrier.cpp)
    void ResetActorEffectHolds(); ResetActorEffectHolds();  // (binoc.cpp)
    void ResetAbilities(); ResetAbilities();          // (gadget.cpp)
    void ResetAsegaSounds(); ResetAsegaSounds();      // (asegevent.cpp)
}

// (the file's bytes already read and decompressed: g_disc.ReadWorld; PC: a kept world's are read on a thread)
bool FLoadWorldBytes(SW* psw, const FK& fk, std::vector<uint8_t> ab, const std::string& trace, std::size_t* pcbRead, std::size_t* pcbFile)
{
    *pcbFile = ab.size();
    g_isectorWorldLoading = fk.isector;   // (textmod.h: the world the text mods' "worlds" name)
    CBinaryInputStream bis;
    bis.OpenBuffer(std::move(ab));
    loadtrace::g_fOn = !trace.empty();
    loadtrace::Begin();
    g_brxstats = BRXSTATS{};
    ResetWorldLoadTables();
    InitPhysWorldHooks();
    bool fOk = true;
    try
    {
        LoadSwFromBrx(psw, &bis);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "load error at offset %zu (in 0x%06X): %s\n", bis.m_ib,
                     loadtrace::g_afnStack.empty() ? 0u : loadtrace::g_afnStack.back(), e.what());
        loadtrace::g_afnStack.clear();
        fOk = false;
    }
    *pcbRead = bis.m_ib;
    // (the original stops after the last group's Align too: the file's tail is not read at load)
    loadtrace::End(bis.m_ib);
    if (!trace.empty())
    {
        loadtrace::Write(trace);
        loadtrace::g_fOn = false;
    }
    return fOk;
}


// A world by its index (0..39), its name ("m_ext") or "boot" / "frontend" (m_ext, the world the boot queues); filter:
// its index (orig3 0x47989C, the objects' world filter)
bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& error)
{
    int iworld = -1;
    if (w == "boot" || w == "frontend") iworld = sly3::WORLD_PROLOGUE;
    else if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0]))) iworld = std::atoi(w.c_str());
    else iworld = g_disc.IworldFromName(w);
    if (iworld < 0 || iworld >= sly3::CWORLD)
    {
        error = "no world " + w + " (0.." + std::to_string(sly3::CWORLD - 1) + " or a name: --list-files)";
        return false;
    }
    fk = g_disc.FkWorld(iworld);
    filter = iworld;
    if (fk.cb == 0)
    {
        error = "world " + w + " (" + g_disc.SzWorldElf(iworld) + ") has no file: nothing to load";
        return false;
    }
    return true;
}
