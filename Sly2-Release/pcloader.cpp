// PC addition, not in the original game: other worlds' files loaded while the current world keeps running (debug menu
// only: pcloader.h).
#include "pcloader.h"

#include "activate.h"
#include "audio.h"
#include "emitter.h"
#include "snd989.h"
#include "anim.h"
#include "asega.h"
#include "bone.h"
#include "brx.h"
#include "lo.h"
#include "shd.h"
#include "sound.h"
#include "spliceglue.h"
#include "sw.h"
#include "transition.h"
#include "update.h"
#include "worldaccess.h"
#include "worldglobals.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

void ReleaseSwRuntimeTables();   // sw.cpp

namespace
{
struct KEPT
{
    int iwrec = -1;
    PCKWS pckws = PCKWS_None;
    FK fk{};
    std::future<std::vector<uint8_t>> fut;   // the background read
    SW* psw = nullptr;                       // never freed: its objects are drawn and pointed at until the game ends
    SHDS shds;                               // its textures and shaders (moved out of the game's tables)
    std::unordered_map<const ALO*, std::vector<std::vector<GLOBBONEBIND>>> mpaabind;   // its rest-pose bone bindings
    SOUNDMGR sound;                          // its music block (its bank's key, its sound table)
    bool fOwnBank = false;                   // its bank is not the running world's: loaded (hbank, loopers in sound)
    uint32_t hbank = 0;
};
std::vector<KEPT*> s_akept;
bool s_fWorldStarting = false;
std::unordered_map<const void*, int> s_mpcopyiwrec;   // the running world's copies' objects -> their kept world (sounds)   // between a world's load and its start (copies start with the world)

KEPT* PkFind(int iwrec)
{
    for (KEPT* p : s_akept)
        if (p->iwrec == iwrec) return p;
    return nullptr;
}

// (with the registering source file's name: kept in the files' name order, Linux's link order, so the hooks run in the
// same order on Windows, whose CRT runs the static initializers in reverse: PACKAGING.md section 7)
std::vector<std::pair<const char*, PCWORLDHOOKS>>& Ahooks()
{
    static std::vector<std::pair<const char*, PCWORLDHOOKS>> s_a;
    return s_a;
}

// The file's objects in a world of their own. While a world runs, the loader's global tables are moved aside first and
// moved back after (worldglobals.h): the running world sees nothing of it.
void Instantiate(KEPT* pk, std::vector<uint8_t> ab)
{
    const auto t0 = std::chrono::steady_clock::now();
    SW* pswRun = g_psw;
    const bool fCheck = pswRun && std::getenv("SLY2N_GLOBALDIFF");
    if (fCheck) SnapshotGlobalsForDiff();
    std::unique_ptr<WGSTASH> pstash = PwgStashWorldGlobals();   // (also inside a transition: the tables kept as they are)
    SW* psw = new SW();
    psw->filter = pk->iwrec;
    pk->psw = psw;
    std::size_t cbRead = 0, cbFile = 0;
    g_psoundKeptLoad = &pk->sound;   // (its music block read aside: the running world's bank and streams untouched)
    g_fKeptWorldLoad = true;
    const bool fOk = FLoadWorldBytes(psw, pk->fk, std::move(ab), "", &cbRead, &cbFile);
    g_psoundKeptLoad = nullptr;
    g_psw = psw;
    // the records' run-time parts, the ASEG / SM fields and the bone bindings, as a world start makes them (no object's
    // PostLoad: nothing applies itself, no script runs)
    InitWorldAnimationRecords(psw);
    for (const auto& plo : psw->aploAll)
    {
        plo->iupd = -1;          // (in no update table: its flags are its own)
        plo->fInWorld = false;   // (no world hooks when its records move)
        if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        const ALO* palo = static_cast<const ALO*>(plo.get());
        if (const auto* paabind = PaabindOfAlo(palo)) pk->mpaabind[palo] = *paabind;
    }
    // its side tables released (the stashed tables come back below; inside a transition the next load resets them)
    ResetAsegas();
    ReleaseSwRuntimeTables();
    ResetWorldLoadTables();
    pk->shds = std::move(g_shds);   // (its records stay where they are: the meshes point at them)
    g_shds = SHDS{};
    g_psw = pswRun;
    InstallSpliceHooks(pswRun);
    g_fKeptWorldLoad = false;
    if (pstash) RestoreWorldGlobals(std::move(pstash));
    if (fCheck) ReportGlobalsDiff("kept world load");
    // its sound bank: its objects' and animations' sound ids are its own table's into its own bank (most worlds have
    // their own): loaded beside the running world's, kept (PsndNewPcKept, the copies' sounds)
    if (std::memcmp(pk->sound.keyBank, g_sound.keyBank, sizeof(g_sound.keyBank)) != 0)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
        const FK fkBank = FkFromRecord(pk->sound.keyBank);
        if (fkBank.cb != 0) pk->hbank = snd_BankLoadByLoc(fkBank.isector, 0);
        snd_ResolveBankXREFS();
        pk->fOwnBank = pk->hbank != 0;
        for (std::size_t i = 0; i < pk->sound.aflagsSound.size(); ++i)
            if (pk->hbank && pk->sound.aflagsSound[i] != 2 && snd_IsSoundALooper(pk->hbank, static_cast<int>(i))) pk->sound.aflagsSound[i] = 2;
    }
    pk->pckws = fOk ? PCKWS_Ready : PCKWS_Failed;
    if (std::getenv("SLY2N_PCLOADPRINT"))
    {
        int cSame = 0, cDiff = 0;
        for (size_t i = 0; i < pk->sound.aisoundFromId.size() && i < g_sound.aisoundFromId.size(); ++i)
            (pk->sound.aisoundFromId[i] == g_sound.aisoundFromId[i] ? cSame : cDiff)++;
        std::printf("pc loader: world %d bank %s the running world's (sound table %zu / %zu ids, %d same, %d differ)\n", pk->iwrec,
                    std::memcmp(pk->sound.keyBank, g_sound.keyBank, sizeof(g_sound.keyBank)) == 0 ? "is" : "is not",
                    pk->sound.aisoundFromId.size(), g_sound.aisoundFromId.size(), cSame, cDiff);
        for (int id : {279, 280, 149, 250})
            if (id < int(pk->sound.aisoundFromId.size()) && id < int(g_sound.aisoundFromId.size()))
                std::printf("pc loader:   sound id %d: bank sound %d (running world: %d)\n", id, pk->sound.aisoundFromId[size_t(id)], g_sound.aisoundFromId[size_t(id)]);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("pc loader: world %d kept (%zu objects, %.0f ms on the game thread%s)%s\n", pk->iwrec, psw->aploAll.size(), ms,
                pswRun ? ", the world running" : "", fOk ? "" : " FAILED");
}
// A kept world's sound id in its own bank (false: its bank is the running world's, or not loaded).
bool FKeptSound(int iwrec, int sfxid, uint32_t* phbank, int* pisound, int* pkind)
{
    const KEPT* pk = PkFind(iwrec);
    if (!pk || !pk->fOwnBank) return false;
    const SOUNDMGR& s = pk->sound;
    const int isound = sfxid >= 0 && sfxid < int(s.aisoundFromId.size()) ? s.aisoundFromId[size_t(sfxid)] : -1;
    *phbank = pk->hbank;
    *pisound = isound >= 0 && isound < int(s.aflagsSound.size()) ? isound : -1;
    *pkind = *pisound >= 0 ? s.aflagsSound[size_t(*pisound)] : 0;
    return true;
}

// (sound.h's hook: a sound whose source is a copy's object plays from its kept world's bank; found by address only)
bool FPcSndBank(const void* psrc, int sfxid, uint32_t* phbank, int* pisound, int* pkind)
{
    if (s_mpcopyiwrec.empty()) return false;
    const auto it = s_mpcopyiwrec.find(psrc);
    if (static const bool s_fSrc = std::getenv("SLY2N_SNDSRC") != nullptr; s_fSrc)   // (debug: which source a sound has)
        std::printf("sndsrc: sfx %d src %p = %d (cid 0x%X)%s\n", sfxid, psrc, static_cast<const LO*>(psrc)->oid, static_cast<const LO*>(psrc)->Cid(),
                    it != s_mpcopyiwrec.end() ? " a copy" : "");
    return it != s_mpcopyiwrec.end() && FKeptSound(it->second, sfxid, phbank, pisound, pkind);
}

// (emitter.h's hooks: a copy's emitter draws with its kept world's shaders, and the objects its options name (its
// source, its particles' models) are found among its own copies first, then in its kept world near its template)
const SHD* PshdPcCopy(const LO* plo, int32_t id)
{
    if (s_mpcopyiwrec.empty()) return nullptr;
    const auto it = s_mpcopyiwrec.find(plo);
    const KEPT* pk = it != s_mpcopyiwrec.end() ? PkFind(it->second) : nullptr;
    if (!pk) return nullptr;
    const SHD* pFirst = nullptr;
    const SHD* pFound = nullptr;
    for (const auto& p : pk->shds.ashd)
    {
        if (!(p->grfshd & 0x40)) continue;   // (as PshdOfId: the particles' draw records)
        if (!pFirst) pFirst = p.get();
        if (p->id == id) { pFound = p.get(); break; }
    }
    if (static const bool s_f = std::getenv("SLY2N_PCSHDDBG") != nullptr; s_f)
        std::printf("pc loader: copy %d (cid 0x%X) shader 0x%X: %s\n", plo->oid, plo->Cid(), id, pFound ? "its world's" : "not found (its first)");
    return pFound ? pFound : pFirst;
}

// The kept models a copy's emitters draw their particles with (and the explosions their pieces): the renderer finds a
// model by its glob set among the scene's, so each gets a copy here, out of the world (made at the next step)
std::vector<ALO*> s_apaloModelWanted;
std::unordered_set<const ALO*> s_setModelCopied;
uint32_t s_serialModels = 0;

LO* PloPcCopyFind(const LO* ploNear, int32_t oid)
{
    if (s_mpcopyiwrec.empty()) return nullptr;
    const auto it = s_mpcopyiwrec.find(ploNear);
    const KEPT* pk = it != s_mpcopyiwrec.end() ? PkFind(it->second) : nullptr;
    if (!pk || !pk->psw) return nullptr;
    // its own copy tree (the projectile flying, not its template)
    const LO* ptop = ploNear;
    while (ptop->paloParent && s_mpcopyiwrec.count(ptop->paloParent)) ptop = ptop->paloParent;
    for (const auto& [pv, iwrec] : s_mpcopyiwrec)
    {
        const LO* p = static_cast<const LO*>(pv);
        if (iwrec == pk->iwrec && p->oid == oid && FLoInParentChain(p, ptop)) return const_cast<LO*>(p);
    }
    // its kept world: the nearest to its template (the guest's posed parts are found there: they move with it)
    const LO* ptmpl = ploNear->ploCloneOf;
    LO* ploAny = nullptr;
    for (const LO* panc = ptmpl ? static_cast<const LO*>(ptmpl->paloParent) : nullptr; panc; panc = panc->paloParent)
        for (const auto& up : pk->psw->aploAll)
            if (up->oid == oid && FLoInParentChain(up.get(), panc)) return up.get();
    for (const auto& up : pk->psw->aploAll)
        if (up->oid == oid) { ploAny = up.get(); break; }
    return ploAny;
}

LO* PloPcCopyFindModel(const LO* ploNear, int32_t oid)
{
    LO* plo = PloPcCopyFind(ploNear, oid);
    if (static const bool s_f = std::getenv("SLY2N_PCSHDDBG") != nullptr; s_f && s_mpcopyiwrec.count(ploNear))
        std::printf("pc loader: copy %d names %d: %s\n", ploNear->oid, oid, plo ? (s_mpcopyiwrec.count(plo) ? "a copy" : "its world's") : "none");
    if (g_psw && g_psw->serial != s_serialModels)
    {
        s_serialModels = g_psw->serial;
        s_setModelCopied.clear();
        s_apaloModelWanted.clear();
    }
    if (plo && !s_mpcopyiwrec.count(plo) && FCidDerivedFrom(plo->Cid(), 0x30))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (palo->pglobset && !palo->pglobset->aglob.empty() && !s_setModelCopied.count(palo) &&
            std::find(s_apaloModelWanted.begin(), s_apaloModelWanted.end(), palo) == s_apaloModelWanted.end())
            s_apaloModelWanted.push_back(palo);
    }
    return plo;
}

struct INITPCLOADER
{
    INITPCLOADER()
    {
        g_pfnPcSndBank = FPcSndBank;
        g_pfnPcShdOfLo = PshdPcCopy;
        g_pfnPcFindOid = PloPcCopyFindModel;
    }
} s_initpcloader;
}  // namespace

int IwrecOfPcCopy(const LO* plo)
{
    const auto it = s_mpcopyiwrec.find(plo);
    return it != s_mpcopyiwrec.end() ? it->second : -1;
}

CSnd* PsndNewPcKept(int iwrec, int sfxid)
{
    uint32_t hbank = 0;
    int isound = -1, kind = 0;
    if (FKeptSound(iwrec, sfxid, &hbank, &isound, &kind)) return PsndNewPlainBank(sfxid, hbank, isound, kind);
    return PsndNewPlain(sfxid);
}

void RequestPcKeptWorld(int iwrec)
{
    if (PkFind(iwrec)) return;
    KEPT* pk = new KEPT;
    pk->iwrec = iwrec;
    s_akept.push_back(pk);
    const std::vector<WREC>& awrec = AwrecWorlds();
    if (iwrec < 0 || iwrec >= int(awrec.size()) || !awrec[size_t(iwrec)].fk.cb)
    {
        pk->pckws = PCKWS_Failed;
        std::printf("pc loader: world %d has no file\n", iwrec);
        return;
    }
    pk->fk = awrec[size_t(iwrec)].fk;
    pk->pckws = PCKWS_Loading;
    // (the disc's reads open their own file: safe beside the game thread's)
    const FK fk = pk->fk;
    pk->fut = std::async(std::launch::async, [fk] {
        try
        {
            return g_disc.ReadWorld(fk);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "pc loader: read failed: %s\n", e.what());
            return std::vector<uint8_t>();
        }
    });
}

PCKWS PckwsKeptWorld(int iwrec)
{
    const KEPT* pk = PkFind(iwrec);
    return pk ? pk->pckws : PCKWS_None;
}

bool FPcKeptWorldsLoading()
{
    for (const KEPT* p : s_akept)
        if (p->pckws == PCKWS_Loading) return true;
    return false;
}

SW* PswKeptWorld(int iwrec)
{
    const KEPT* pk = PkFind(iwrec);
    return pk && pk->pckws == PCKWS_Ready ? pk->psw : nullptr;
}

LO* PloKeptFind(int iwrec, int oid, CID cidBase)
{
    SW* psw = PswKeptWorld(iwrec);
    if (!psw) return nullptr;
    for (const auto& plo : psw->aploAll)
        if (plo->oid == oid && plo->iclone == 0 && !plo->ploCloneOf && (cidBase == 0 || FCidDerivedFrom(plo->Cid(), cidBase))) return plo.get();
    return nullptr;
}

const std::vector<std::vector<GLOBBONEBIND>>* PaabindKept(const ALO* palo)
{
    for (const KEPT* p : s_akept)
        if (auto it = p->mpaabind.find(palo); it != p->mpaabind.end()) return &it->second;
    return nullptr;
}

SHDS* PshdsKept(int iwrec)
{
    KEPT* pk = PkFind(iwrec);
    return pk && pk->pckws == PCKWS_Ready ? &pk->shds : nullptr;
}

LO* PloPcCloneIntoWorld(SW* psw, LO* ptmpl, bool fAdd)
{
    if (!psw || !ptmpl) return nullptr;
    const std::size_t i0 = psw->aploLoad.size();
    LO* plo = PloCloneLo(ptmpl, psw, nullptr);
    if (!plo) return nullptr;
    if (!fAdd) RemoveLo(psw, plo);   // (as a proxy's spare: out of the world until used)
    std::vector<LO*> anew(psw->aploLoad.begin() + std::ptrdiff_t(i0), psw->aploLoad.end());
    // (its sounds from its kept world's bank)
    int iwrecTmpl = -1;
    for (const KEPT* pk : s_akept)
        if (pk->psw && ptmpl->pcls && std::any_of(pk->psw->aploAll.begin(), pk->psw->aploAll.end(), [&](const auto& up) { return up.get() == ptmpl; })) iwrecTmpl = pk->iwrec;
    if (iwrecTmpl >= 0)
        for (LO* p : anew) s_mpcopyiwrec[p] = iwrecTmpl;
    if (s_fWorldStarting) return plo;   // (the world's start starts it with its own objects)
    // the world runs: its start's parts for the new objects alone (StartSw: the records, bone bindings, ASEG / SM tables,
    // then each object's slot-15 start in creation order); its start scripts run with the world's next deferred events
    InitAnimationRecordsOf(anew);
    for (LO* p : anew) StartLoadedLo(psw, p);
    // its emitters and explosions (the world's were built at its first update: the RC chopper's shots and bombs burst
    // with their own explosions, R44)
    AddEmittersOfNewLos(psw, anew);
    psw->aploLoad.erase(psw->aploLoad.begin() + std::ptrdiff_t(i0), psw->aploLoad.end());
    return plo;
}

void PumpPcKeptWorlds(SW* psw)
{
    if (!psw) s_mpcopyiwrec.clear();   // (inside a transition: the old world's copies are gone)
    for (KEPT* pk : s_akept)
    {
        if (pk->pckws != PCKWS_Loading || !pk->fut.valid()) continue;
        // (a world runs: one per step, when its read is done; none runs (a transition): wait for it)
        if (psw && pk->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        std::vector<uint8_t> ab = pk->fut.get();
        if (ab.empty())
        {
            pk->pckws = PCKWS_Failed;
            continue;
        }
        Instantiate(pk, std::move(ab));
        if (psw) break;
    }
}

void RegisterPcWorldHooks(const PCWORLDHOOKS& hooks, const char* pszFile)
{
    const char* pszBase = pszFile;
    for (const char* pch = pszFile; *pch; ++pch)
        if (*pch == '/' || *pch == '\\') pszBase = pch + 1;
    auto& a = Ahooks();
    auto it = a.end();
    while (it != a.begin() && std::strcmp((it - 1)->first, pszBase) > 0) --it;
    a.insert(it, {pszBase, hooks});
}

void OnPcWorldLoaded(SW* psw)
{
    s_fWorldStarting = true;
    for (const auto& [pszFile, h] : Ahooks())
        if (h.pfnLoaded) h.pfnLoaded(psw);
}

void OnPcWorldStarted(SW* psw)
{
    s_fWorldStarting = false;
    for (const auto& [pszFile, h] : Ahooks())
        if (h.pfnStarted) h.pfnStarted(psw);
}

void OnPcWorldStep(SW* psw, bool fDebugKeys)
{
    if (fDebugKeys) PumpPcKeptWorlds(psw);
    // (the models the copies' particles use: their copies, out of the world, so the scene has their meshes)
    if (psw && psw->serial != s_serialModels)
    {
        s_serialModels = psw->serial;
        s_setModelCopied.clear();
        s_apaloModelWanted.clear();
    }
    if (psw && !s_apaloModelWanted.empty())
    {
        const std::vector<ALO*> apalo = std::move(s_apaloModelWanted);
        s_apaloModelWanted.clear();
        for (ALO* palo : apalo)
            if (s_setModelCopied.insert(palo).second)
            {
                PloPcCloneIntoWorld(psw, palo, false);
                if (std::getenv("SLY2N_PCCHARPRINT")) std::printf("pc loader: model %d copied for the copies' particles\n", palo->oid);
            }
    }
    for (const auto& [pszFile, h] : Ahooks())
        if (h.pfnStep) h.pfnStep(psw, fDebugKeys);
}
