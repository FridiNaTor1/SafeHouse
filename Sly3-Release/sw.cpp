#include "carry.h"
#include "breakable.h"
// The world loader (orig 0x13DA18) and the global blocks of a world file.

#include "sw.h"
#include "hide.h"
#include "emitter.h"
#include "laser.h"
#include "booty.h"
#include "pocket.h"

#include "activate.h"
#include "guard.h"
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
#include "gc.h"
#include "serialize.h"
#include "splicemap.h"
#include "update.h"
#include "navlink.h"
#include "nav.h"
#include "screenrt.h"
#include "overlay.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

SW* g_psw = nullptr;

// The world's unload: the splice runtime's frames (their bindings break the reference cycles) and tables.
SW::~SW()
{
    if (g_psw == this)
    {
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
        return p->ploNs || (p->ichannel >= 0 && g_psw && p->ichannel < static_cast<int>(g_psw->awe.size()) && !g_psw->awe[p->ichannel].x5C);
    };
    if (fGroupNs(plo)) return false;
    const LO* p = plo;
    while (p->paloParent) p = p->paloParent;
    return !fGroupNs(p);
}

LO* PloFindSwObjectByOid(SW* psw, OID oid)
{
    // orig 0x15E938 kind 5 (global): the world's objects of the oid first
    LO* ploFirst = nullptr;
    for (const auto& plo : psw->aploAll)
        if (plo->oid == oid)
        {
            if (FLoInWorldNs(plo.get()) || plo->oidInstance == oid) return plo.get();
            if (!ploFirst) ploFirst = plo.get();
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
        pbis->ReadRecord64(w.abRecord);
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
        if (pbis->U8Read() == psw->language) ilang = j;
    const int c = pbis->S16Read();
    for (int i = 0; i < c; ++i)
    {
        const int id = pbis->S16Read();
        std::string str;
        for (int j = 0; j < clang; ++j)
        {
            if (j == ilang) str = pbis->ReadStringSw();
            else pbis->SkipStringSw();
        }
        psw->atext.emplace_back(id, std::move(str));
    }
}

// orig 0x0013E0E0: the node tree
void LoadNodesFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(0x13E0E0);
    const int c = pbis->U16Read();
    psw->anode.assign(c, NODE{});
    psw->cnodeLeaf = 0;
    for (int i = 0; i < c; ++i)
    {
        NODE& n = psw->anode[i];
        n.vec = pbis->ReadVector4();
        n.inodeA = pbis->S16Read();
        if (n.inodeA < 0) n.ileafA = psw->cnodeLeaf++;
        n.inodeB = pbis->S16Read();
        if (n.inodeB < 0) n.ileafB = psw->cnodeLeaf++;
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

// orig 0x0013DA18 LoadSwFromBrx
void LoadSwFromBrx(SW* psw, CBinaryInputStream* pbis)
{
    LOADFN(0x13DA18);
    g_psw = psw;
    g_nFilterCur = psw->filter;
    InstallSpliceHooks(psw);
    // TODO(M3): 0x12AE18
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
    }
    LoadMusicFromBrx(pbis);  // 0x1FD0E8 (sound.cpp)
    LoadProgressFromBrx(psw, pbis);
    LoadWorldRecordsFromBrx(psw, pbis);
    LoadTextFromBrx(psw, pbis);
    LoadNodesFromBrx(psw, pbis);
    LoadOptionsFromBrx(psw->aoptSw, nullptr, pbis);
    LoadOptionsFromBrx(psw->aoptCm, nullptr, pbis);
    pbis->Align(16);
    // TODO(M3): 0x1FF7E8, 0x20E3C0, 0x20F1C8, the background colour (0x16F960), the load heaps
    ResetShaders();  // orig 0x1ED3A8 (the renderer's tables)
    for (int i = 0; i < cwe; ++i)
    {
        WE& we = psw->awe[i];
        psw->iwe = we.x5C ? -1 : i;
        psw->iweChannel = i;
        LoadBulkHeaderFromBrx(psw, pbis, true);  // the renderer's (shd.cpp)
        {
            LOADFN(0x13DA18);
            psw->fScriptDebug = pbis->S32Read();
        }
        g_fLoadDebugInfo = psw->fScriptDebug;
        LoadSwSpliceFromBrx(psw, we.x10, pbis);  // the splice runtime (splicemap.cpp): parse + module init
        LoadShadersFromBrx(psw, pbis);  // the renderer's (shd.cpp), incl. the bulk block 0x20F640
        LoadCmeshFromBrx(psw, pbis);
        std::vector<LO*> aplo;
        LOSINK sink;
        sink.papo = &aplo;
        LoadSwObjsFromBrx(psw, nullptr, pbis, &sink);
        EndSwSpliceGroup(psw);
        pbis->Align(16);
        // TODO(M3): 0x14B690
        EndShaderGroup();  // orig 0x1EC7A0
        we.cloLoaded = static_cast<int>(aplo.size());
        we.aplo = aplo;
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
    }
    psw->iwe = -2;
    psw->iweChannel = -1;
    // TODO(M3): 0x1ED448, 0x1FD5E8
    // the transform records' world matrices (orig: the first transform pass), parents first
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& plo : psw->aploAll)
            if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), 0x30)) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
    glm::mat3 NormalizeRotateMatrix3(const glm::mat3& m);  // xfmath.h (0x11B3D0)
    // orig: 0x001CA500 (the groups with spare instances: each object's rest pose from the first spare, 0x1A62E8 and
    // the world rotation normalized by 0x11B3D0)
    for (WE& we : psw->awe)
    {
        we.aposRest.assign(static_cast<std::size_t>(std::max(we.cloLoaded, 0)), glm::vec3(0.0f));
        we.amatRest.assign(static_cast<std::size_t>(std::max(we.cloLoaded, 0)), glm::mat3(1.0f));
        we.amatRootRest.assign(static_cast<std::size_t>(std::max(we.cloLoaded, 0)), glm::mat4(1.0f));
        if (we.cUsed >= we.cinst) continue;
        // (each object of the spare instances cUsed..cinst: its node's +0x24 = 1)
        for (int j = std::max(we.cUsed, 0); j < we.cinst; ++j)
            for (int k = 0; k < we.cloLoaded; ++k)
                if (const std::size_t i = static_cast<std::size_t>(j) * we.cloLoaded + k; i < we.aplo.size()) we.aplo[i]->fSpareNs = true;
        for (int k = 0; k < we.cloLoaded; ++k)
        {
            const std::size_t i = static_cast<std::size_t>(we.cUsed) * we.cloLoaded + k;
            if (i >= we.aplo.size() || !FCidDerivedFrom(we.aplo[i]->Cid(), 0x30)) continue;
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
    std::vector<uint8_t> ab = g_disc.ReadWorld(fk);
    *pcbFile = ab.size();
    CBinaryInputStream bis;
    bis.OpenBuffer(std::move(ab));
    loadtrace::g_fOn = !trace.empty();
    loadtrace::Begin();
    g_brxstats = BRXSTATS{};
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


bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& error)
{
    filter = 0;  // (the boot world and the front end load with the world filter's initial 0)
    if (w == "boot" || w == "93") { fk = FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40)); return true; }
    if (w == "frontend" || w == "57") { fk = FkFromRecord(g_disc.PbElf(VA_FK_FRONTEND, 0x40)); return true; }
    const int iwrec = std::atoi(w.c_str());
    auto pswBoot = std::make_unique<SW>();
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(pswBoot.get(), FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40)), "", &cbRead, &cbFile))
    {
        error = "the boot world (the world records) did not load";
        return false;
    }
    if (iwrec < 0 || iwrec >= static_cast<int>(pswBoot->awrec.size()))
    {
        error = "no world record " + w;
        return false;
    }
    fk = pswBoot->awrec[iwrec].fk;
    filter = iwrec;
    if (fk.cb == 0 || fk.id == 0)
    {
        error = "world " + w + ": the record has no file: nothing to load";
        return false;
    }
    return true;
}
