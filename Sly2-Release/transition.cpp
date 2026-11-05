#include "transition.h"
#include "pcloader.h"
#include "rumble.h"
#include "region.h"
#include "replay.h"
#include "crv.h"
#include "lox.h"
#include "so.h"
#include "gadget.h"
#include "sound.h"
void ResetTriggers();
#include "scpt.h"
#include "gang.h"
#include "physworld.h"

#include "screenrt.h"
#include "hudwidget.h"

#include "actor.h"
#include "sly.h"
#include "anim.h"
#include "cm.h"
#include "cmworld.h"

#include "activate.h"
#include "cmworld.h"

#include "brx.h"
#include "po.h"
#include "sidebag.h"
#include "pair.h"
#include "proc.h"
#include "frame.h"
#include <functional>
#include "alo.h"
#include "task.h"
#include "objmethod.h"
#include "cm.h"
#include "clock.h"
#include "ai.h"
#include "joy.h"
#include "cls.h"
#include "sm.h"
#include "smrun.h"
#include "sw.h"
#include "update.h"
#include "savedata.h"

#include <cstdio>
#include <string>
#include <set>
#include <cstdlib>
#include <algorithm>
#include <cstring>

TRANSITION g_transition;
int g_cTransitionLoads = 0;
extern int g_iframeMain;  // (debug: world loads through the transition, for SLY2N_TELEPORT's "/wN")

namespace
{
// The world records of the last loaded world (orig 0x3E1C40: every world file carries the same 44).
std::vector<WREC> s_awrec;

// orig 0x22B220 / 0x22B490: the same-world flag (records equal by their words 2 and 14, as the original)
bool FSameChk(const uint8_t* a, const uint8_t* b)
{
    uint32_t a2, a14, b2, b14;
    std::memcpy(&a2, a + 8, 4);
    std::memcpy(&a14, a + 56, 4);
    std::memcpy(&b2, b + 8, 4);
    std::memcpy(&b14, b + 56, 4);
    return (a2 ^ a14) == (b2 ^ b14);
}

// orig 0x20DF80 RestoreChkmgrFromCheckpoint: the request becomes pending (the next 0x20E890 loads it)
void SetPending(const TRANSREQ& req)
{
    g_transition.fPending = true;
    g_transition.req = req;
}

// vt slot 49 of a sprite set: play animation id (its frames: screenrt.h)
void PlaySpriteAnim(LO* plo, int32_t id)
{
    SPRITESET* p = dynamic_cast<SPRITESET*>(plo);
    if (!p) return;
    p->idAnimPlaying = id;
    float duration = 0.0f;
    for (const auto& [idAnim, g] : p->aanim)
        if (idAnim == id) duration = g;
    p->tAnimEnd = g_clock.tReal + duration;
    ShowScreenObj(plo, id);   // the frames (screenrt.h)
}

// orig 0x2121E8(o, 0x314): an animation still plays
bool FSpriteAnimPlaying(LO* plo)
{
    return FScreenObjBusy(plo, 0x314);
}

// orig 0x22B090: the wipe's state change
void SetWipeState(WIPES wipes)
{
    TRANSITION& t = g_transition;
    if (t.wipes == wipes) return;
    if (static const bool s_fLog = std::getenv("SLY2N_TRANSLOG") != nullptr; s_fLog)   // (debug: the wipe's states)
        std::printf("transition: wipe state %d -> %d (t %.3f)\n", int(t.wipes), int(wipes), g_clock.t);
    t.wipes = wipes;
    switch (wipes)
    {
    case WIPES_Idle:
        t.fWipeReq = false;
        t.reqWipe = TRANSREQ{};
        if (t.ploWipe) HideScreenObj(t.ploWipe);   // vt slot 50: the wipe's end
        break;
    case WIPES_Out: PlaySpriteAnim(t.ploWipe, 0x311); break;
    case WIPES_In: PlaySpriteAnim(t.ploWipe, 0x30F); break;
    case WIPES_Loading:
        SetPending(t.reqWipe);
        t.fWipeReq = false;
        t.reqWipe = TRANSREQ{};
        break;
    }
}

// orig 0x00227FE8: the checkpoint: the class 0x2A objects (under the key object when it is found, else every
// one) whose oid or id list (+0x16C) holds oidPoint; the last match wins
LO* PloFindCheckpoint(SW* psw, OID oidPoint, OID oidKey)
{
    LO* ploKey = oidKey != -1 ? PloFindSwObjectByOid(psw, oidKey) : nullptr;
    auto fUnder = [&](LO* p) {
        if (!ploKey) return true;
        std::function<bool(LO*)> find = [&](LO* q) {
            for (LO* pc : q->aploOwned)
                if (pc == p || find(pc)) return true;
            return false;
        };
        return find(ploKey);
    };
    LO* plo = nullptr;
    for (CID cid = 0x2A; cid < g_acls[0x2A].cidLast; ++cid)
        for (LO* p : psw->mpcidaplo[cid])
        {
            const CHKPNT* pchk = dynamic_cast<const CHKPNT*>(p);
            bool f = p->oid == oidPoint;
            if (pchk)
                for (int id : pchk->aid) f = f || id == oidPoint;
            if (f && fUnder(p)) plo = p;
        }
    return plo;
}

}  // namespace

// vt slot 38 (0x1A6B88 / ALO 0x1F49C8): an object placed where another is (its world position and orientation;
// TODO(physics): the ALO's velocities)
// orig: 0x001A6B88 (LO slot 38): the target's vt slot 31 matrix (ALO 0x1F3FA0: its object record's +0x40; LOX
// 0x231440) -> 0x1A6BE8: the object's slot 29 (the position), then slot 30 (the rotation)
void TeleportLo(LO* plo, LO* ploTo)
{
    if (FIsLoDerivedFrom(plo, 0x30))
    {
        TeleportActor(plo, ploTo);  // (actor.cpp: the ALOs' slot 38, 0x1F49C8 / 0x2052E0)
        return;
    }
    glm::mat4 matTo(1.0f);
    if (FIsLoDerivedFrom(ploTo, 0x30))
    {
        ALO* palo = static_cast<ALO*>(ploTo);
        if (XF* pxf = PxfObj(palo)) matTo = pxf->matWorld;
        else if (!palo->axf.empty()) matTo = palo->axf[0]->matWorld;
    }
    else if (FIsLoDerivedFrom(ploTo, 3))
        matTo = MatWorldLox(ploTo);
    else return;
    if (FIsLoDerivedFrom(plo, 3))
    {
        LOX* plox = static_cast<LOX*>(plo);
        SetLoxPos(plox, glm::vec3(matTo[3]));     // slot 29 (0x2312A0)
        SetLoxRotation(plox, glm::mat3(matTo));   // slot 30 (0x231300)
    }
}

namespace
{

// orig 0x00228750: the checkpoint applied: its pairs placed (vt slot 38), its state machines' states (0x20FF70;
// TODO(anim)), its task states (0x161F48), the class 0x1A objects reset (0x228358), the camera's cut (0x14D8C0),
// the splice event 5 (and its broadcast 0x1A, 0x1A61E0)
void ApplyCheckpoint(SW* psw, LO* plo)
{
    CHKPNT* pchk = dynamic_cast<CHKPNT*>(plo);
    if (pchk)
        for (const auto& [oidA, oidB] : pchk->apair)
        {
            LO* p = PloFindSwObjectByOid(psw, oidA);
            LO* q = p ? PloFindSwObjectByOid(psw, oidB) : nullptr;
            if (p && q) TeleportLo(p, q);
        }
    // orig 0x20FF70 / 0x20FFF0: the state machines' trigger states (run to)
    if (pchk)
        for (const auto& e : pchk->arsmg)
        {
            LO* po = PloFindSwObjectByOid(psw, e[0]);
            if (!po || !FIsLoDerivedFrom(po, 0x30) || e[2] == -1) continue;
            for (SMA* psma : g_apsma)
                if (psma->palo == po && (e[1] == -1 || (psma->psm && psma->psm->oid == e[1])))
                {
                    SetSmaGoal(psma, e[2]);
                    break;
                }
        }
    if (pchk) ApplyCheckpointTasks(psw, pchk, pchk->atask);
    std::vector<LO*> a;
    for (CID cid = 0x1A; cid < g_acls[0x1A].cidLast; ++cid)
        for (LO* p : psw->mpcidaplo[cid]) a.push_back(p);
    for (size_t i = a.size(); i-- > 0;)
        if (a[i]->nState != 2 && a[i]->nState != 0) { a[i]->nState = 0; a[i]->tState = g_clock.t; }
    CutCm(&g_cm);
    HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
    BroadcastTaskEvent(psw, 0x1A, plo);
}

CRef RefVoidMethod() { return CRef(TAGK_Void); }
struct INITCHKMETHODS
{
    INITCHKMETHODS()
    {
        // option 235 of a warp point (class 0x29, EOPID 778): 0x228170. E.g. Ep4's prison elevator (world 15, the ride
        // script 80619's event 82107): the player is moved to the lower car's warp 80623, then walks out (R02)
        RegisterObjectMethod(0x10B760, [](LO* plo, const std::vector<CRef>&) {
            if (plo) TriggerWarp(plo);
            return RefVoidMethod();
        });
        auto addId = [](LO* plo, int id) {
            if (CHKPNT* p = dynamic_cast<CHKPNT*>(plo); p && p->aid.size() < 4) p->aid.push_back(id);
        };
        static decltype(addId) s_addId = addId;
        RegisterObjectMethod(0x10B8F8, [](LO* plo, const std::vector<CRef>&) { s_addId(plo, 0x189); return RefVoidMethod(); });
        RegisterObjectMethod(0x10B970, [](LO* plo, const std::vector<CRef>&) { s_addId(plo, 0x18A); return RefVoidMethod(); });
        RegisterObjectMethod(0x10B9E8, [](LO* plo, const std::vector<CRef>& a) { s_addId(plo, NArg(a, 0)); return RefVoidMethod(); });
        RegisterObjectMethod(0x10BA70, [](LO* plo, const std::vector<CRef>& a) {  // 793: 0x2288D8 -> 0x20FEC0
            CHKPNT* p = dynamic_cast<CHKPNT*>(plo);
            const int fTrigger = NArg(a, 0);
            if (!p || !fTrigger) return RefVoidMethod();
            const OID oidRoot = NArg(a, 1, -1), oidSm = NArg(a, 2, -1), oid = NArg(a, 3, -1);
            for (auto& e : p->arsmg)
                if (e[0] == oidRoot && (e[1] == -1 || e[1] == oidSm))
                {
                    e[2] = oid;  // (fTrigger: the trigger's state)
                    return RefVoidMethod();
                }
            if (p->arsmg.size() + 1 < 4) p->arsmg.push_back({oidRoot, oidSm, oid, -1});
            return RefVoidMethod();
        });
        RegisterObjectMethod(0x10BB38, [](LO* plo, const std::vector<CRef>& a) {  // 794: 0x228920 -> 0x161F10
            if (CHKPNT* p = dynamic_cast<CHKPNT*>(plo); p && p->atask.size() < 4) p->atask.emplace_back(NArg(a, 0, -1), NArg(a, 1, -1));
            return RefVoidMethod();
        });
        RegisterObjectMethod(0x10BBE0, [](LO* plo, const std::vector<CRef>& a) {  // 795: 0x228958
            if (CHKPNT* p = dynamic_cast<CHKPNT*>(plo); p && p->apair.size() < 4) p->apair.emplace_back(NArg(a, 0, -1), NArg(a, 1, -1));
            return RefVoidMethod();
        });
    }
} s_initchkmethods;
}

// orig 0x00228170
void TriggerWarp(LO* pwarp)
{
    LO* ppo = g_psw ? PloPoCurrent(g_psw) : nullptr;  // 0x1C4840
    if (!ppo || !pwarp) return;
    TeleportLo(ppo, pwarp);                            // the player's vt slot 38
    HandleLoSpliceEvent(pwarp, 5, false, 0, nullptr);  // the warp's vt slot 3 (0x1A61E0) with 5
    BroadcastTaskEvent(g_psw, 0x1A, pwarp);
}

// orig 0x0022B490
void RequestTransition(SW* psw, const uint8_t* pchk, OID oidPoint, OID oidKey, int flags, OID oidWipe)
{
    TRANSITION& t = g_transition;
    TRANSREQ req;
    std::memcpy(req.abChk, pchk ? pchk : t.abChkCur, 64);
    req.oidPoint = oidPoint;
    req.oidKey = oidKey;
    req.flags = flags;
    if (t.fHaveCur && FSameChk(req.abChk, t.abChkCur)) req.flags |= 2;
    static const bool s_fLog = std::getenv("SLY2N_TRANSLOG") != nullptr;
    if (s_fLog) std::printf("transition: requested (point %d, key %d, flags 0x%X, wipe %d)\n", oidPoint, oidKey, req.flags, oidWipe);
    if (oidWipe == -1 || !psw)
    {
        SetPending(req);
        return;
    }
    // orig 0x22B220
    if (t.wipes == WIPES_Out || t.wipes == WIPES_Loading) return;
    if (t.wipes == WIPES_In) SetWipeState(WIPES_Idle);
    t.ploWipe = PloFindSwObjectByOid(psw, oidWipe);
    if (!t.ploWipe)
    {
        SetPending(req);
        return;
    }
    t.fWipeReq = true;
    t.reqWipe = req;
    t.oidWipe = oidWipe;
    SetWipeState(WIPES_Out);
}

const std::vector<WREC>& AwrecWorlds() { return s_awrec; }

int IepisodeCur()
{
    const int i = g_transition.iwrecCur;
    return i >= 0 && i < static_cast<int>(s_awrec.size()) ? s_awrec[i].slot : -1;
}

const WREC* PwrecEpisodeMain(int iepisode)
{
    if (iepisode < 0) return nullptr;
    for (const WREC& w : s_awrec)
        if (w.fMain && w.slot == iepisode) return &w;
    return nullptr;
}

void EnsureWorldRecords()
{
    if (!s_awrec.empty()) return;
    auto pswBoot = std::make_unique<SW>();
    std::size_t cbRead = 0, cbFile = 0;
    if (FLoadWorldFile(pswBoot.get(), FkFromRecord(g_disc.PbElf(VA_FK_BOOT_WORLD, 0x40)), "", &cbRead, &cbFile))
        s_awrec = pswBoot->awrec;
}

void RequestWorldWarp(SW* psw, int iwrec, OID oidPoint, OID oidWipe)
{
    EnsureWorldRecords();
    // orig 0x210990: the world's record
    if (iwrec < 0 || iwrec >= static_cast<int>(s_awrec.size())) return;
    RequestTransition(psw, s_awrec[iwrec].abRecord, oidPoint, -1, 0, oidWipe);
}

// orig 0x001651D8 (a task: started now when present, else queued and its world loaded)
void RequestTaskStart(SW* psw, int iwrec, OID oidTask)
{
    EnsureWorldRecords();
    const uint8_t* pchk = iwrec >= 0 && iwrec < static_cast<int>(s_awrec.size()) ? s_awrec[iwrec].abRecord : nullptr;
    RequestTaskStartByOid(psw, pchk, oidTask);
}

// (debug harness, autoplay.cpp: a task of another world started by that world's load, the queued branch of 0x1651D8
// even when the task's filtered-out copy is in this world)
void RequestTaskStartThere(SW* psw, int iwrec, OID oidTask)
{
    EnsureWorldRecords();
    if (iwrec < 0 || iwrec >= static_cast<int>(s_awrec.size())) return;
    g_taskmgr.oidJob = -1;
    g_taskmgr.oidTask = -1;
    g_taskmgr.oidQueued = oidTask;
    RequestTransition(psw, s_awrec[iwrec].abRecord, 0x189, -1, 8, -1);
}

// debug: a splice value printed (the forms of a handler)
void ResetSpawners();  // (spawner.cpp)

void PrintRef(const CRef& ref, std::string* ps)
{
    char ach[96];
    switch (ref.m_tagk)
    {
    case TAGK_S32: std::snprintf(ach, sizeof(ach), "%d", ref.m_n); break;
    case TAGK_F32: std::snprintf(ach, sizeof(ach), "%g", ref.m_g); break;
    case TAGK_Bool: std::snprintf(ach, sizeof(ach), "%s", ref.m_bool ? "#t" : "#f"); break;
    case TAGK_Symid: std::snprintf(ach, sizeof(ach), "s%u", ref.m_symid); break;
    case TAGK_Bifk: std::snprintf(ach, sizeof(ach), "<bif%d>", static_cast<int>(ref.m_bifk)); break;
    case TAGK_Pair:
        *ps += "(";
        for (const CPair* p = ref.m_ppair.get(); p; p = p->m_ppairNext.get())
        {
            PrintRef(p->m_ref, ps);
            if (p->m_ppairNext) *ps += " ";
        }
        *ps += ")";
        return;
    default: std::snprintf(ach, sizeof(ach), "<t%d>", static_cast<int>(ref.m_tagk)); break;
    }
    *ps += ach;
}

// orig 0x20E068: the old world's end (the broadcast 1, a job that ended restarted; TODO(M4): remaining HUD, sound,
// music side), its objects freed last. (Natively also at the game's end, without the tasks' end (the PS2 game never
// exits): the world goes in the same order before the sound system stops, see RunGame.)
void UnloadWorld(std::unique_ptr<SW>& psw, bool fEndTasks)
{
    if (psw && fEndTasks) EndWorldTasks(psw.get());
    ResetHudWidgets();  // orig 0x1D9408: release gauge/prompt bindings before the world's objects
    ResetSoundMgr();  // orig 0x1FD700 (from 0x20E068)
    ResetAmbientSounds();
    ResetTriggers();  // (trigger.cpp)
    ClearDeferredSpliceEvents();
    ResetPos(psw.get());
    ResetScpts();
    ResetLoListeners();
    ResetSpawners();
    g_grfjoyLock = 0;  // (0x20E068: the pad's lock bits cleared with the old world, without 0x1870D0)
    StopJoyRumble();   // (0x20E068: SetRumbleRums(0x2E0CCC, Stop))
    g_transition.ploPlayer = nullptr;  // (the old world's player: freed with it; 0x1C4840 reads it while the new world loads)
    psw.reset();
}

// orig 0x0020E890
std::unique_ptr<SW> PswUpdateTransition(std::unique_ptr<SW> psw)
{
    TRANSITION& t = g_transition;
    if (!t.fPending) return psw;
    t.fPending = false;
    TRANSREQ req = t.req;
    if (std::getenv("SLY2N_TRANSLOG")) std::printf("transition: loading (flags 0x%X)\n", req.flags);
    // the world records come from the boot world when no world was loaded yet
    EnsureWorldRecords();
    // orig 0x20E068: the old world's end (the broadcast 1, a job that ended restarted; TODO(M4): remaining HUD, sound,
    // music side), ResetClock 0x14D068, the SW cleared
    // orig 0x20E890: the HUD timer's seconds left, kept unless the request has a flag of 0xCD
    const float dtTimerResume = DtHudTimerResume(req.flags);
    UnloadWorld(psw, true);
    PumpPcKeptWorlds(nullptr);   // (PC addition, debug menu only: a kept world still being read loaded now, no world running: pcloader.h)
    ResetClock(&g_clock);
    LockJoy(8);                 // 0x187078(8): the pad locked while the world loads (0x1D9648 unlocks it)
    g_gamepause = GAMEPAUSE{};  // (the SW's pause counts +0x3D78.. cleared with it)
    g_actview.grfCm = 0;  // (no activation by distance until the first frame's camera)
    const FK fk = FkFromRecord(req.abChk);
    int iwrec = -1;
    for (int i = 0; i < static_cast<int>(s_awrec.size()); ++i)
        if (FSameChk(s_awrec[i].abRecord, req.abChk)) iwrec = i;
    const bool fSameWorld = t.fHaveCur && FSameChk(req.abChk, t.abChkCur);  // (0x20EA24..0x20EA70: s2 = !same)
    BuildReplayJobTables(iwrec);   // (QoL Replay Jobs, replay.h: before the front end, once; no world is current here)
    psw = std::make_unique<SW>();
    ResetPhysWorld();  // (the old world's attachments go with it, also when the new world has no file: world 18)
    psw->filter = iwrec >= 0 ? iwrec : 0;
    std::memcpy(t.abChkCur, req.abChk, 64);
    t.fHaveCur = true;
    t.iwrecCur = iwrec;
    t.req = req;
    std::size_t cbRead = 0, cbFile = 0;
    if (fk.cb && !FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile))
        std::fprintf(stderr, "transition: world record %d did not load\n", iwrec);
    OnReplayWorldLoaded(iwrec, req.flags);   // (QoL Replay Jobs: the front end / the title end a replay)
    LoadSoundBank();  // orig 0x1FD5E8 (the end of 0x13DA18)
    if (!psw->awrec.empty()) s_awrec = psw->awrec;
    ++g_cTransitionLoads;
    if (std::getenv("SLY2N_TRANSLOG")) std::printf("transition: world record %d loaded (%zu objects, flags 0x%X, frame %d, load %d)\n", iwrec, psw->aploAll.size(), req.flags, g_iframeMain, g_cTransitionLoads);
    CmOnWorldLoad(psw.get());  // the camera: InitCm 0x14D6D8, the world's camera options, the player's (cmworld.h)
    // orig 0x20E3C0's end: the record's language (0x2E1E54, region.h), world record (0x210A88),
    // episode (0x210AE0) and best episode (0x210B28 < the episode: 0x1D8B20 "episodeBest")
    {
        auto setN = [](std::vector<std::string> apath, int n) { CRef r; r.SetS32(n); FSetProgress(apath, r); };
        setN({"flags", "lang"}, g_nLang);
        setN({"widCurrent"}, psw->filter);
        const int iep = IepisodeCur();
        setN({"episodeCurrent"}, iep);
        const CRef rBest = RefGetProgress({"episodeBest"});
        if ((rBest.m_tagk == TAGK_S32 ? rBest.m_n : 0) < iep) setN({"episodeBest"}, iep);
    }
    // orig 0x20E3C0's broadcast 3 (the game record: reset for the title, the pending save; frontend.h)
    BroadcastTaskEvent(psw.get(), 3, nullptr);
    // orig 0x20E740 (with the broadcast 4: a queued task changes the request's point)
    OnPcWorldLoaded(psw.get());    // (PC addition, debug menu only: the guests' copies made before the start, pcloader.h)
    StartSw(psw.get(), true, [](SW* psw) {
        // orig: 0x001400B8 starts with 0x00203A38, then initializes the
        // player's parts/counters before the world's deferred start calls.
        PostLoadSwPhys(psw);
        InitWorldActors(psw);
        InitWorldGang(psw);
    });
    OnPcWorldStarted(psw.get());   // (PC addition, debug menu only: pcloader.h)
    if (dtTimerResume > 0.0f) StartHudTimer(dtTimerResume, psw.get());  // 0x1618F8 after 0x20E740
    if (psw->tasks.oidPointNext != -1) req.oidPoint = psw->tasks.oidPointNext;
    req.flags = psw->tasks.grfReq;
    t.req = req;
    // orig 0x20ECBC: the same world again with the flag 0x40: the Mega Jump taken away (0x14C258; TODO: s2 is also
    // set by 0x1B3B08's result)
    if (fSameWorld && (req.flags & 0x40)) RemoveMegaJump(psw.get());
    // the checkpoint context
    LO* pchk = PloFindCheckpoint(psw.get(), req.oidPoint, req.oidKey);
    if (std::getenv("SLY2N_TASKDEBUG"))
        std::fprintf(stderr, "checkpoint for point %d key %d: %d (cid 0x%X) frame %d binding 5 %d\n", req.oidPoint, req.oidKey,
                     pchk ? pchk->oid : -1, pchk ? pchk->Cid() : -1, pchk && pchk->pframe ? 1 : 0,
                     pchk && pchk->pframe && pchk->pframe->PrefFindBinding(5, 1) ? 1 : 0);
    if (const char* pszOpt = std::getenv("SLY2N_FINDOPTVAL"))  // debug: the loaded options holding the integer
    {
        const int n = std::atoi(pszOpt);
        std::function<bool(const OPTVAL&)> has = [&](const OPTVAL& v) {
            if (v.n == n) return true;
            for (int16_t k : v.an) if (k == n) return true;
            for (const OPTVAL& w : v.alist) if (has(w)) return true;
            return false;
        };
        for (const auto& plo : psw->aploAll)  // (the object n's own options and fields)
            if (plo->oid == n)
            {
                std::fprintf(stderr, "optval %d: its own (cid 0x%X):", n, plo->Cid());
                for (const OPT& o : plo->aopt)
                {
                    std::fprintf(stderr, " %d(", o.eopid);
                    for (const OPTVAL& v : o.aval)
                    {
                        std::fprintf(stderr, "[k%d n%d", v.k, v.n);
                        for (const OPTVAL& w : v.alist) std::fprintf(stderr, " %d", w.n);
                        std::fprintf(stderr, "]");
                    }
                    std::fprintf(stderr, ")");
                }
                for (const auto& [key, u] : plo->afield) std::fprintf(stderr, " field 0x%llX=%d", static_cast<unsigned long long>(key), static_cast<int>(u));
                std::fprintf(stderr, "\n");
            }
        for (const auto& plo : psw->aploAll)  // (and the loaded fields holding it)
            for (const auto& [key, u] : plo->afield)
                if (static_cast<int>(u) == n)
                    std::fprintf(stderr, "optval %d: object %d (cid 0x%X) field key 0x%llX\n", n, plo->oid, plo->Cid(),
                                 static_cast<unsigned long long>(key));
        for (const auto& plo : psw->aploAll)
            for (const OPT& o : plo->aopt)
                for (const OPTVAL& v : o.aval)
                    if (has(v))
                    {
                        std::fprintf(stderr, "optval %d: object %d (cid 0x%X) eopid %d kind %d; the option's values:", n, plo->oid, plo->Cid(), o.eopid, v.k);
                        for (const OPTVAL& w : o.aval) std::fprintf(stderr, " [k %d n %d]", w.k, w.n);
                        std::fprintf(stderr, "\n");
                    }
    }
    if (const char* pszFind = std::getenv("SLY2N_FINDSYM"))
    {
        // debug: every module binding whose code mentions the form text (e.g. "(<t1> s1745")
        std::set<CFrame*> aframe;
        std::function<void(CFrame*)> add = [&](CFrame* pf) {
            if (!pf || aframe.count(pf)) return;
            aframe.insert(pf);
            for (int i = 0; i < pf->m_cpframeParent; ++i) add(pf->m_apframeParent[i]);
        };
        for (const auto& plo : psw->aploAll) add(plo->pframe.get());
        for (CFrame* pf : aframe)
            for (const VARB& v : pf->m_avarb)
            {
                std::string str;
                if (v.ref.m_tagk == TAGK_Proc && v.ref.m_pproc)
                {
                    CRef b;
                    b.SetPair(v.ref.m_pproc->m_ppairCodeExpr);
                    PrintRef(b, &str);
                }
                if (str.find(pszFind) != std::string::npos)
                {
                    std::string owners;
                    for (const auto& plo : psw->aploAll)
                        if (plo->pframe.get() == pf) owners += " " + std::to_string(plo->oid) + "/0x" + std::to_string(plo->Cid());
                    std::fprintf(stderr, "found in s%u (frame %p, objects%s): %s\n", static_cast<unsigned>(v.symid), (void*)pf, owners.substr(0, 200).c_str(), str.substr(0, 2500).c_str());
                }
            }
    }
    if (std::getenv("SLY2N_TASKDEBUG") && pchk && pchk->pframe)
        if (CRef* pref = pchk->pframe->PrefFindBinding(5, 1))
        {
            std::string str;
            if (pref->m_tagk == TAGK_Proc && pref->m_pproc)
            {
                CRef body;
                body.SetPair(pref->m_pproc->m_ppairCodeExpr);
                PrintRef(body, &str);
            }
            else PrintRef(*pref, &str);
            std::fprintf(stderr, "checkpoint handler 5: %s\n", str.c_str());
            for (OID oid : {1752, 1931, 33735, 17239})
                if (LO* p = PloFindSwObjectByOid(psw.get(), oid))
                {
                    std::string strSb;
                    if (p->psidebag)
                        for (const SBB& sbb : p->psidebag->m_asbb) strSb += " " + std::to_string(sbb.n);
                    if (p->pframe && p->pframe->PrefFindBinding(0, 1))
                    {
                        CRef* pr = p->pframe->PrefFindBinding(0, 1);
                        std::string strStart;
                        if (pr->m_tagk == TAGK_Proc && pr->m_pproc)
                        {
                            CRef body;
                            body.SetPair(pr->m_pproc->m_ppairCodeExpr);
                            PrintRef(body, &strStart);
                        }
                        else PrintRef(*pr, &strStart);
                        std::fprintf(stderr, "  %d start: %s\n", oid, strStart.substr(0, 1500).c_str());
                    }
                    std::fprintf(stderr, "  %d cid 0x%X state %d sidebag%s frame %d start-binding %d\n", oid, p->Cid(),
                                 p->Cid() == 0x85 ? static_cast<TASK*>(p)->nState : -9, strSb.c_str(), p->pframe ? 1 : 0,
                                 p->pframe && p->pframe->PrefFindBinding(0, 1) ? 1 : 0);
                }
        }
    // (no checkpoint: the players stay at their instances' matrices, as the reference's 0x1CA370 slot 37 leaves them)
    if (!pchk)
        for (const auto& [plo, mat] : psw->mpmatPlayerInstance) SetLoMatrix(plo, mat);
    if (pchk)
    {
        ApplyCheckpoint(psw.get(), pchk);
        t.oidPointCur = req.oidPoint;
        t.oidKeyCur = req.oidKey;
    }
    // the player, when none is current (0x1C4840 / 0x1C4A48) and some object was enabled (SW +0x3EB8): the progress
    // value "oidPoCurrent" (0x2A8A88), else Sly, Bentley, Murray (0x2E1E40 / 0x2DD5BC / 0x2F7900): added (vt slot 8),
    // enabled (slot 79), placed at the checkpoint (slot 38) unless one of its pairs places it; the other players
    // disabled before (0x1C4080 == 0) removed (slot 9)
    if (!PloPoCurrent(psw.get()) && CpoRegistered() > 0)
    {
        LO* pselected = nullptr;
        const CRef ref = RefGetProgress({"oidPoCurrent"});
        if (ref.m_tagk == TAGK_S32 && ref.m_n != -1) pselected = PloFindSwObjectByOid(psw.get(), ref.m_n);
        for (CID cid : {0x35, 0x37, 0x36})
            if (!pselected && !psw->mpcidaplo[cid].empty()) pselected = psw->mpcidaplo[cid][0];
        if (pselected)
        {
            AddLo(psw.get(), pselected);
            EnablePo(psw.get(), pselected, 1);
            const CHKPNT* pc = dynamic_cast<const CHKPNT*>(pchk);
            bool fPaired = false;
            if (pc)
                for (const auto& [oidA, oidB] : pc->apair) fPaired = fPaired || oidA == pselected->oid;
            if (pchk && !fPaired) TeleportLo(pselected, pchk);
            for (CID cid = 0x34; cid < g_acls[0x34].cidLast; ++cid)
                for (LO* p : std::vector<LO*>(psw->mpcidaplo[cid]))
                    if (p != pselected && NPoEnabled(p) == 0) RemoveLo(psw.get(), p);
        }
    }
    // (native job starts: a job started at its world's load (0x1651D8's queued branch, used natively by the
    // --world W:TASK harness and Replay Jobs) comes in at point 0x189, the safe house's checkpoint, whose handler 5
    // makes the safe house (cid 0x46) the current player once the episode's first jobs are done. The job's opening
    // binocucom talk then finds no speakers (0x1BEED8: 0x2AE / 0x2AF below the current player) and shows empty head
    // windows. In the game a job is reached by a player (the satellite job by play: Sly; the reference's warp: Sly
    // too), so such a start hands the pad to Sly. SLY2N_JOBSTARTSH=1 keeps the safe house. Only for that start
    // (request flags 8): the checkpoint task stays queued (SetTaskChk) after a job, and a story transition into the hub
    // (e.g. Ep8's briefing 3378's exit 3377 after the first job) must leave the safe house current, as the original
    // does.)
    if (LO* pcur = PloPoCurrent(psw.get());
        pcur && pcur->Cid() == 0x46 && (req.flags & 8) && g_taskmgr.oidQueued != -1 && !psw->mpcidaplo[0x35].empty() &&
        !std::getenv("SLY2N_JOBSTARTSH"))
    {
        LO* psly = psw->mpcidaplo[0x35][0];
        if (!psly->fInWorld) AddLo(psw.get(), psly);
        EnablePo(psw.get(), psly, 1);
    }
    t.ploPlayer = PloPoCurrent(psw.get());
    if (std::getenv("SLY2N_WRECLIST"))  // (debug: the world records: index, episode, main, progress key)
        for (size_t i = 0; i < s_awrec.size(); ++i)
        {
            const std::vector<int16_t> akey(s_awrec[i].an.begin(), s_awrec[i].an.end());
            std::printf("wrec %zu: episode %d kind %d main %d key %s\n", i, s_awrec[i].slot, s_awrec[i].kind, s_awrec[i].fMain,
                        StrProgressKey(akey).c_str());
        }
    if (std::getenv("SLY2N_TASKDEBUG"))
        std::fprintf(stderr, "world start: current player %d (cid 0x%X), %d registered\n", t.ploPlayer ? t.ploPlayer->oid : -1,
                     t.ploPlayer ? (int)t.ploPlayer->Cid() : 0, CpoRegistered());
    // the camera: updated now and cut (0x14D478 / 0x14D8C0), then the broadcast 5 (the tasks from the save)
    CmOnWorldStart();
    BroadcastTaskEvent(psw.get(), 5, nullptr);
    t.oidTaskQueued = -1;
    // orig 0x22B420: the wipe uncovers the new world
    if (t.wipes == WIPES_Loading || t.oidWipe != -1)
    {
        t.ploWipe = t.oidWipe != -1 ? PloFindSwObjectByOid(psw.get(), t.oidWipe) : nullptr;
        if (t.ploWipe) SetWipeState(WIPES_In);
        else t.wipes = WIPES_Idle;
    }
    return psw;
}

// orig 0x0022B198
void UpdateWipe(SW* psw)
{
    (void)psw;
    TRANSITION& t = g_transition;
    switch (t.wipes)
    {
    case WIPES_Out:
        if (!FSpriteAnimPlaying(t.ploWipe)) SetWipeState(WIPES_Loading);
        break;
    case WIPES_In:
        if (!FSpriteAnimPlaying(t.ploWipe)) SetWipeState(WIPES_Idle);
        break;
    default: break;
    }
}

float UWipeCover()
{
    const TRANSITION& t = g_transition;
    const SPRITESET* p = dynamic_cast<const SPRITESET*>(t.ploWipe);
    float duration = 0.0f;
    if (p)
        for (const auto& [id, g] : p->aanim)
            if (id == p->idAnimPlaying) duration = g;
    const float left = p ? std::max(0.0f, p->tAnimEnd - g_clock.tReal) : 0.0f;
    const float u = duration > 0.0f ? left / duration : 0.0f;
    switch (t.wipes)
    {
    case WIPES_Out: return 1.0f - u;
    case WIPES_Loading: return 1.0f;
    case WIPES_In: return u;
    default: return 0.0f;
    }
}
