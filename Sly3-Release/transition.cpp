#include "transition.h"
#include "outfit3.h"
#include "challenge3.h"
#include "splitscreen3.h"
#include "hackgame3.h"
#include "game/sly3.h"
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
#include "screen3.h"
#include "frontend.h"
int NRandInRange(int nLow, int nHigh);  // (util.h: 0x11D3F8)
#include "hudwidget.h"

#include "actor.h"
#include "sly.h"
#include "anim.h"
#include "cm.h"
#include "cmworld.h"

#include "activate.h"
#include "player3.h"
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
#include "game/task3.h"
#include "game/scpt3.h"
#include "game/scptrun3.h"
#include "game/scptgame3.h"
#include "game/trigger3.h"
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
#include "worldaccess.h"

#include <cstdio>
#include <string>
#include <set>
#include <cstdlib>
#include <execinfo.h>
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

}  // namespace

// (Sly 3 flow log, tools/progression3.py: the world index of a key record, -1 when it is no world's. A key's words are
// masked (each copy differently: the executable's and the worlds' exits differ in bytes), so keys compare by their
// file id, words 2 ^ 14, as FSameChk does)
int IworldOfChk(const uint8_t* pchk)
{
    if (!pchk) return -1;
    const FK fk = FkFromRecord(pchk);
    if (!fk.cb) return -1;
    for (int i = 0; i < sly3::CWORLD; ++i)
        if (const FK fkw = g_disc.FkWorld(i); fkw.cb && fkw.id == fk.id) return i;
    return -1;
}

// (debug, the flow map: a key record named: "world N name" or "movie ID" or "none")
std::string StrKeyRecord(const uint8_t* pchk)
{
    const int iworld = IworldOfChk(pchk);
    if (iworld >= 0) return "world " + std::to_string(iworld) + " " + g_disc.SzWorldElf(iworld);
    const FK fk = FkFromRecord(pchk);
    if (!(fk.cb & ~sly3::CB_MOVIE_FLAGS)) return "none";
    // (a movie: the executable's movie table, or a key whose size carries the movie flag bits (the episodes' chalk
    // talks, keyed only by the exits); its sector and size for `--play-movie @SECTOR:CB`)
    bool fMovie = (fk.cb & sly3::CB_MOVIE_FLAGS) != 0;
    for (const auto& [va, fkm] : g_disc.ElfKeys())
        fMovie = fMovie || (va >= sly3::VA_MOVIE_KEYS && fkm.id == fk.id);
    char sz[96];
    std::snprintf(sz, sizeof(sz), "%s %u (@0x%X:0x%X)", fMovie ? "movie" : "file", fk.id, fk.isector,
                  fk.cb & ~sly3::CB_MOVIE_FLAGS);
    return sz;
}

namespace
{

// orig 0x20DF80 RestoreChkmgrFromCheckpoint: the request becomes pending (the next 0x20E890 loads it)
void SetPending(const TRANSREQ& req)
{
    g_transition.fPending = true;
    g_transition.req = req;
}

// orig3: 0x00260758 (with 0x37B, the held part: some part of the wipe still plays)
bool FWipePlaying(LO* plo)
{
    return plo && FScreenObjBusy(plo, sly3ui::PART_HELD);
}

// orig3: 0x002C0060 (the bag 0x2BFED8 made of 0x3297A0's three pairs): wipe 875's next out / in parts, the pairs
// shuffled again once all are drawn (no pair twice in a row across the shuffles)
void DrawWipePair(TRANSITION& t)
{
    static const int s_aid[3][2] = {{0x370, 0x371}, {0x372, 0x373}, {0x374, 0x375}};
    int* a = t.aipairWipeBag;
    if (t.cpairWipeDrawn >= 3)
    {
        const int last = a[2];
        for (int i = 0; i < 3; ++i) std::swap(a[i], a[NRandInRange(0, 2)]);
        if (a[0] == last) std::swap(a[0], a[NRandInRange(1, 2)]);
        t.cpairWipeDrawn = 0;
    }
    const int ipair = a[t.cpairWipeDrawn++];
    t.idWipeOut = s_aid[ipair][0];
    t.idWipeIn = s_aid[ipair][1];
}

// orig3: 0x00288C58 SetWipeState (Sly 2 0x22B090): 0 idle (the request dropped, the wipe hidden: vt slot 54), 1 out
// / 2 in (the wipe's part +0x58 / +0x5C shown: vt slot 53, 0x260EC8), 3 loading (the held request pending)
void SetWipeState(WIPES wipes)
{
    TRANSITION& t = g_transition;
    if (t.wipes == wipes) return;
    if (static const bool s_fLog = std::getenv("SLY2N_TRANSLOG") != nullptr; s_fLog)   // (debug: the wipe's states)
        std::printf("transition: wipe state %d -> %d (t %.3f, wipe %d, parts 0x%X / 0x%X)\n", int(t.wipes), int(wipes),
                    g_clock.t, t.oidWipe, t.idWipeOut, t.idWipeIn);
    t.wipes = wipes;
    switch (wipes)
    {
    case WIPES_Idle:
        t.fWipeReq = false;
        t.reqWipe = TRANSREQ{};
        if (t.ploWipe) HideScreenObj(t.ploWipe);
        break;
    case WIPES_Out: if (t.ploWipe) ShowScreenObj(t.ploWipe, t.idWipeOut); break;
    case WIPES_In: if (t.ploWipe) ShowScreenObj(t.ploWipe, t.idWipeIn); break;
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
    for (CID cid = S3CID(0x2A); cid >= 0 && cid < g_acls[S3CID(0x2A)].cidLast; ++cid)
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
    if (FIsLoDerivedFrom(plo, S3CID(0x30)))
    {
        TeleportActor(plo, ploTo);  // (actor.cpp: the ALOs' slot 38, 0x1F49C8 / 0x2052E0)
        return;
    }
    glm::mat4 matTo(1.0f);
    if (FIsLoDerivedFrom(ploTo, S3CID(0x30)))
    {
        ALO* palo = static_cast<ALO*>(ploTo);
        if (XF* pxf = PxfObj(palo)) matTo = pxf->matWorld;
        else if (!palo->axf.empty()) matTo = palo->axf[0]->matWorld;
    }
    else if (FIsLoDerivedFrom(ploTo, S3CID(3)))
        matTo = MatWorldLox(ploTo);
    else return;
    if (FIsLoDerivedFrom(plo, S3CID(3)))
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
            // (a pair's object may be an immediate proxy's oid: its instance, as the reference's find. The 2P duel's
            // checkpoint 94610 pairs the proxies 94392 / 94393 with the spawn points 94376 / 94382: the reference's
            // planes start there, (-+21900, 0, 3950) head on (build/ref du3 at vblank 1730); natively they were skipped
            // and both planes stood at the checkpoint, overlapping (a ram at once))
            LO* p = PloFindSwPlayerByOid3(psw, oidA);
            LO* q = p ? PloFindSwObjectByOid(psw, oidB) : nullptr;
            if (std::getenv("SLY2N_CHKPRINT"))
                std::printf("checkpoint %d pair %d -> %d (found %d / %d)\n", plo->oid, oidA, oidB, p ? p->oid : -1, q ? q->oid : -1);
            if (p && q) TeleportLo(p, q);
        }
    // orig 0x20FF70 / 0x20FFF0: the state machines' trigger states (run to)
    if (pchk)
        for (const auto& e : pchk->arsmg)
        {
            LO* po = PloFindSwObjectByOid(psw, e[0]);
            if (!po || !FIsLoDerivedFrom(po, S3CID(0x30)) || e[2] == -1) continue;
            for (SMA* psma : g_apsma)
                if (psma->palo == po && (e[1] == -1 || (psma->psm && psma->psm->oid == e[1])))
                {
                    SetSmaGoal(psma, e[2]);
                    break;
                }
        }
    if (pchk) ApplyCheckpointTasks(psw, pchk, pchk->atask);
    std::vector<LO*> a;
    for (CID cid = S3CID(0x1A); cid >= 0 && cid < g_acls[S3CID(0x1A)].cidLast; ++cid)
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
        RegisterObjectMethod(0x10B8F8, [](LO* plo, const std::vector<CRef>&) { s_addId(plo, sly3::POINT_DEFAULT); return RefVoidMethod(); });
        RegisterObjectMethod(0x10B970, [](LO* plo, const std::vector<CRef>&) { s_addId(plo, sly3::POINT_SECOND); return RefVoidMethod(); });
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

// (Sly 3's SCPT 0x12 on a checkpoint: orig3 0x280010, game/scptgame3.cpp)
void ApplyCheckpointLo(SW* psw, LO* plo) { ApplyCheckpoint(psw, plo); }

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
    if (std::getenv("SLY3_TRANSBT"))   // (debug: who asked)
    {
        void* a[12];
        std::fflush(stdout);
        backtrace_symbols_fd(a, backtrace(a, 12), 1);
    }
    // (the flow log: one line per world change, tools/progression3.py's milestones; notes/PLAN.md the world flow code)
    std::printf("flow: transition to world %d requested (point %d, key %d, flags 0x%X, wipe %d, frame %d)\n",
                IworldOfChk(req.abChk), oidPoint, oidKey, req.flags, oidWipe, g_iframeMain);
    if (oidWipe == -1 || !psw)
    {
        SetPending(req);
        return;
    }
    // orig3 0x288DE8 (Sly 2 0x22B220): nothing while the wipe covers or loads; one coming in is dropped; no such wipe
    // object: pending at once; else held, the wipe's parts chosen (875: the bag's next pair, else 886 / 888) and out
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
    if (oidWipe == 0x36B) DrawWipePair(t);
    else
    {
        t.idWipeOut = sly3ui::PART_SHOW;
        t.idWipeIn = sly3ui::PART_HIDE;
    }
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

// orig3: Sly 3's world records are the executable's tables (game/sly3.h: the keys 0x2EDFD8, the episodes 0x2EDF10,
// each episode's first world 0x2EDFB0; Sly 2 loaded them from its boot world, LoadWorldRecordsFromBrx 0x20F318). A
// record's index is the world's index, the objects' world filter (orig3 0x47989C).
void EnsureWorldRecords()
{
    if (!s_awrec.empty()) return;
    for (int i = 0; i < sly3::CWORLD; ++i)
    {
        WREC w;
        std::memcpy(w.abRecord, g_disc.PbElf(sly3::VA_WORLD_KEYS + sly3::CB_KEY * uint32_t(i), sly3::CB_KEY), sizeof(w.abRecord));
        w.fk = g_disc.FkWorld(i);
        w.slot = g_disc.IepisodeOfWorld(i);
        w.fMain = w.slot >= 0 && w.slot < sly3::EP_Max && int(g_disc.U32Elf(sly3::VA_EPISODE_WORLDS + 4u * uint32_t(w.slot))) == i;
        s_awrec.push_back(std::move(w));
    }
}

void RequestWorldWarp(SW* psw, int iwrec, OID oidPoint, OID oidWipe)
{
    EnsureWorldRecords();
    // orig 0x210990: the world's record
    if (iwrec < 0 || iwrec >= static_cast<int>(s_awrec.size())) return;
    RequestTransition(psw, s_awrec[iwrec].abRecord, oidPoint, -1, 0, oidWipe);
}

// (s3-player) the world start keeps the characters the splice's start events added (spliceglue.cpp FLoAddedBySplice3:
// orig3 0x25B9C0 runs before them; Ep2's bar brawl 2756: Sly and Murray fight beside Bentley). SLY3_SPLICEADDKEEP=0:
// removed as before
bool FLoAddedBySplice3(const LO* plo);
static const bool s_fKeepSplice = [] { const char* sz = std::getenv("SLY3_SPLICEADDKEEP"); return !sz || sz[0] != '0'; }();

// orig 0x001651D8 (a task: started now when present, else queued and its world loaded)
void RequestTaskStart(SW* psw, int iwrec, OID oidTask, OID oidWipe, int grfExtra)
{
    EnsureWorldRecords();
    const uint8_t* pchk = iwrec >= 0 && iwrec < static_cast<int>(s_awrec.size()) ? s_awrec[iwrec].abRecord : nullptr;
    RequestTaskStartByOid(psw, pchk, oidTask, oidWipe, grfExtra);
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
    RequestTransition(psw, s_awrec[iwrec].abRecord, sly3::POINT_DEFAULT, -1, 8, -1);
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
    {
        // (a depth / length bound: a frame's bindings can hold cyclic lists, which recursed until the stack ran out)
        static int s_depth = 0;
        if (s_depth > 40 || ps->size() > 20000) { *ps += "..."; return; }
        ++s_depth;
        *ps += "(";
        int c = 0;
        for (const CPair* p = ref.m_ppair.get(); p && c < 400; p = p->m_ppairNext.get(), ++c)
        {
            PrintRef(p->m_ref, ps);
            if (p->m_ppairNext) *ps += " ";
        }
        *ps += ")";
        --s_depth;
        return;
    }
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
    // orig 0x20E890: the HUD timer's seconds left, kept unless the request has a flag of 0xCD (Sly 2's flags: the native
    // requests still pass Sly 2's bits; Sly 3's 0x25C2A8 masks 0xD5 of its own bits, notes/FLOW.md)
    // (orig3 0x25C2A8: none either when the request goes to world 0, the episode chooser dvd_menu)
    const float dtTimerResume = IworldOfChk(req.abChk) == 0 ? 0.0f : DtHudTimerResume(req.flags);
    // orig3 0x25C2A8: a request for the loaded world without Sly 3's flag 2 (native: TRANSF_INPLACE3), a file, and no
    // movie batch since the last load (D_00389AC0) is not loaded: 0x25BDD0 resets the world in place, then the same
    // start tail as after a load (game/worldreset3.cpp; SLY3_INPLACERESET=0: the reload as before)
    extern bool g_fMovieSinceLoad3;   // (frontend.cpp)
    const bool fInPlace = FScripts3Active() && FWorldResetInPlace3() && (req.flags & TRANSF_INPLACE3) && psw && t.fHaveCur &&
                          FSameChk(req.abChk, t.abChkCur) && FkFromRecord(req.abChk).cb && !g_fMovieSinceLoad3;
    req.flags &= ~TRANSF_INPLACE3;
    const bool fSameWorld = t.fHaveCur && FSameChk(req.abChk, t.abChkCur);  // (0x20EA24..0x20EA70: s2 = !same)
    int iwrec = -1;
    if (fInPlace)
    {
        iwrec = t.iwrecCur;
        t.req = req;
        std::printf("flow: world %d %s reset in place (frame %d, point %d, flags 0x%X)\n", iwrec, g_disc.SzWorldElf(iwrec).c_str(),
                    g_iframeMain, req.oidPoint, req.flags);
        ResetSwInPlace3(psw.get());
    }
    else
    {
    g_fMovieSinceLoad3 = false;
    UnloadWorld(psw, true);
    PumpPcKeptWorlds(nullptr);   // (PC addition, debug menu only: a kept world still being read loaded now, no world running: pcloader.h)
    ResetClock(&g_clock);
    LockJoy(8);                 // 0x187078(8): the pad locked while the world loads (0x1D9648 unlocks it)
    g_gamepause = GAMEPAUSE{};  // (the SW's pause counts +0x3D78.. cleared with it)
    g_actview.grfCm = 0;  // (no activation by distance until the first frame's camera)
    g_actview.cExtra = 0;
    const FK fk = FkFromRecord(req.abChk);
    for (int i = 0; i < static_cast<int>(s_awrec.size()); ++i)
        if (FSameChk(s_awrec[i].abRecord, req.abChk)) iwrec = i;
    BuildReplayJobTables(iwrec);   // (QoL Replay Jobs, replay.h: before the front end, once; no world is current here)
    psw = std::make_unique<SW>();
    ResetPhysWorld();  // (the old world's attachments go with it, also when the new world has no file: world 18)
    psw->filter = iwrec >= 0 ? iwrec : 0;
    std::memcpy(t.abChkCur, req.abChk, 64);
    t.fHaveCur = true;
    t.iwrecCur = iwrec;
    t.req = req;
    std::size_t cbRead = 0, cbFile = 0;
    // orig3 0x25C2A8: 0x25AFD8 -> 0x184A08, the game record reset for the title (flags 1) / the pending save, before the
    // file is read (0x14F440: the option sets' progress fallback 0x1500F0 and the tasks read the record; frontend.h)
    OnGameRecordBeforeLoad3(req.flags);
    if (fk.cb && !FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile))
        std::fprintf(stderr, "transition: world record %d did not load\n", iwrec);
    OnReplayWorldLoaded(iwrec, req.flags);   // (QoL Replay Jobs: the front end / the title end a replay)
    LoadSoundBank();  // orig 0x1FD5E8 (the end of 0x13DA18)
    if (!psw->awrec.empty()) s_awrec = psw->awrec;
    ++g_cTransitionLoads;
    std::printf("flow: world %d %s loaded (frame %d, load %d, point %d, flags 0x%X, %zu objects)\n", iwrec,
                g_disc.SzWorldElf(iwrec).c_str(), g_iframeMain, g_cTransitionLoads, req.oidPoint, req.flags, psw->aploAll.size());
    if (std::getenv("SLY2N_TRANSLOG")) std::printf("transition: world record %d loaded (%zu objects, flags 0x%X, frame %d, load %d)\n", iwrec, psw->aploAll.size(), req.flags, g_iframeMain, g_cTransitionLoads);
    // (s3-player) The reference (--warp 3:1819, the heist's last task, with GAME +0x10 poked to Murray 26 in play:
    // build/refoext/run2.sh of the s3-player worktree): when the job's exit has loaded the episode chooser (world 0) the
    // game's player characters are both -1, so the next episode's world comes up with the list's first, Sly. Which
    // function clears them was not found in the decomp (TODO(sly3)); natively: at the chooser's load.
    // SLY3_CHOOSERKEEPPLAYER=1 keeps them.
    if (iwrec == 0 && FScripts3Active() && !std::getenv("SLY3_CHOOSERKEEPPLAYER")) g_gametasks3.aoidPlayer[0] = g_gametasks3.aoidPlayer[1] = -1;
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
        // (orig3 0x25B6B8: not in the two-player games, D_0036DBE0 == 2: the world's screen mode 2)
        if (psw->nScreenMode3 != 2 && (rBest.m_tagk == TAGK_S32 ? rBest.m_n : 0) < iep) setN({"episodeBest"}, iep);
    }
    // orig 0x20E3C0's broadcast 3 (the game record: reset for the title, the pending save; frontend.h)
    BroadcastTaskEvent(psw.get(), 3, nullptr);
    // orig 0x20E740 (with the broadcast 4: a queued task changes the request's point)
    sly3::ResetScriptPlaced3();
    OnPcWorldLoaded(psw.get());    // (PC addition, debug menu only: the guests' copies made before the start, pcloader.h)
    StartSw(psw.get(), true, [](SW* psw) {
        // orig: 0x001400B8 starts with 0x00203A38, then initializes the
        // player's parts/counters before the world's deferred start calls.
        PostLoadSwPhys(psw);
        InitWorldActors(psw);
        InitWorldGang(psw);
        ShowActorGlasses3(psw);   // (orig3 0x152488: the 3-D glasses hidden out of the anaglyph mode)
    });
    OnPcWorldStarted(psw.get());   // (PC addition, debug menu only: pcloader.h)
    RecordResetPoses3(psw.get());  // (orig3 ALO slot 17 0x23C580: the reset poses of EOPID 86, for an in-place reset)
    }
    // (Sly 3, the pending task start (the request's flag 8 natively; Sly 3's 0x4 with GAME +0x14A0): 0x184AA0 runs
    // StartTask 0x18A0D0 (its predecessors restored, the task made available, then current). Interim until game/task3
    // ports 0x18A0D0: the task forced available (mode 3) and activated, as the reference's --warp W:TASK shows it
    // (35:1798: the binocucom). the world flow code)
    if (FScripts3Active() && (req.flags & 8) && g_taskmgr.oidQueued != -1)
        if (TASK3* p3 = PtaskFind3(g_taskmgr.oidQueued))
        {
            if (std::getenv("SLY2N_TASKDEBUG")) std::printf("transition: task %d started (interim 0x18A0D0)\n", p3->oid);
            if (p3->nState < 1) QueueTaskRequest3(1, p3, 1, 3, nullptr);
            QueueTaskRequest3(0, p3, 4, 0, nullptr);
        }
    // (debug, the flow map: SLY3_EXITLIST=1: the world's exits (EXIT, class 0x18): their keys (the world, the movies
    // queued before it), point, wipe, state)
    if (std::getenv("SLY3_EXITLIST"))
        for (LO* plo : AploOfClass(psw.get(), S3CID(0x1A)))
            if (const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(plo))
            {
                std::string str;
                for (size_t i = 0; i < px->aabKey.size(); ++i) str += (i ? ", " : "") + StrKeyRecord(px->aabKey[i].data());
                std::printf("exit %d (cid 0x%X): %s; point %d wipe %d state %d\n", plo->oid, plo->Cid(), str.c_str(),
                            int(plo->U32Field(0, OFF_EXIT_POINT, sly3::POINT_DEFAULT)), int(plo->U32Field(0, OFF_EXIT_WIPE, uint32_t(-1))), plo->nState);
            }
    // (debug: SLY3_SCPTDUMP=OID: a script's command list)
    if (const char* sz = std::getenv("SLY3_SCPTDUMP"))
        for (const auto& plo : psw->aploAll)
            if (const LOSCPT3* ps = dynamic_cast<const LOSCPT3*>(plo.get()); ps && ps->oid == std::atoi(sz))
                for (size_t i = 0; i < ps->acmd.size(); ++i)
                {
                    std::string str;
                    for (int32_t n : ps->acmd[i].an) str += " " + std::to_string(n);
                    std::printf("scpt %d [%zu] %02X %s oid %d child %d sib %d:%s\n", ps->oid, i, ps->acmd[i].id,
                                SzScptCmd3(ps->acmd[i].id), ps->acmd[i].oid, ps->acmd[i].ichild, ps->acmd[i].isib, str.c_str());
                }
    // (and the scripts' commands 0x68 that fire an exit: the script, the exit, the point)
    if (std::getenv("SLY3_EXITLIST"))
        for (const auto& plo : psw->aploAll)
            if (const LOSCPT3* ps = dynamic_cast<const LOSCPT3*>(plo.get()))
                for (size_t i = 0; i < ps->acmd.size(); ++i)
                    if (ps->acmd[i].id == 0x68)
                        std::printf("exit by script %d [%zu]: exit %d point %d\n", ps->oid, i,
                                    ps->acmd[i].an.size() > 0 ? ps->acmd[i].an[0] : -1, ps->acmd[i].an.size() > 1 ? ps->acmd[i].an[1] : -1);
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
                                 p->Cid() == S3CID(0x85) ? static_cast<TASK*>(p)->nState : -9, strSb.c_str(), p->pframe ? 1 : 0,
                                 p->pframe && p->pframe->PrefFindBinding(0, 1) ? 1 : 0);
                }
        }
    // (the players at their instances' matrices, as the reference's 0x1CA370 slot 37 leaves them; with a checkpoint too:
    // it then places the current player (and its pairs) only. s3-player round 19: Ep4 recon's c_intro (job restart into
    // world 24, checkpoint 70148) has Murray / Bentley / the Guru / Penelope at their proxies' places in the reference
    // (vblank 2300: (-2392, -54487, 890) .. = the instances' matrices); natively they stayed at the origin, all five
    // overlapping, and Murray's first update gave NaN records (his walk-in, SCPT 2087 [34], never ended: 3383 forced))
    // (s3-player round 21, the user's report: no intro scene at the boot) not over a script's placement at the world start:
    // m_ext's attract SCPT 4262 puts Sly on the rope (0x22 at 15071) inside StartSw, and this put him back at his
    // instance's place, so he fell, never reached trigger 15072 (alarm 15086) and the scene waited at [41] forever
    // (not after an in-place reset: the players stay where they were until the checkpoint places them, 0x25B9C0)
    if (!fInPlace)
    for (const auto& [plo, mat] : psw->mpmatPlayerInstance)
        if (!sly3::FScriptPlaced3(plo)) SetLoMatrix(plo, mat);
    // (s3-player round 21: the players' base record at the identity, as the reference's: their body load replaces their
    // records, so a root player's first record (xf0, +0x48's parent) is the identity and its place is in the position /
    // rotation records (the slots 29 / 30). Natively xf0 kept the world file's / the instance's placement (Sly on
    // v_interpol's dock turned 104.1 in xf0), and the step heading (0x24E878) wrote the rotation record's local angle as if
    // its parent were unturned: a script's placement through the slots turned him wrong. Now the same world matrix with
    // xf0 at the identity (TeleportAloMat through the slots); SLY3_ROOTBASE=0: the old way)
    static const bool s_fRootBase = [] { const char* sz = std::getenv("SLY3_ROOTBASE"); return !sz || sz[0] != '0'; }();
    if (s_fRootBase && !fInPlace)
        for (const auto& pl : psw->aploAll)
        {
            LO* plo = pl.get();
            if (!plo || plo->paloParent || !FIsLoDerivedFrom(plo, S3CID(0x34)) || !PactorFromLo(plo)) continue;
            ALO* palo = static_cast<ALO*>(plo);
            if (palo->axf.empty()) continue;
            const glm::mat4 w = sly3::MatLoWorld3(plo);
            palo->axf[0]->mat = glm::mat4(1.0f);
            UpdateAloXfWorlds(palo);
            sly3::TeleportRootActor3(plo, w);
        }
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
    if (!PloPoCurrent(psw.get()) && (CpoRegistered() > 0 || fInPlace))
    {
        LO* pselected = nullptr;
        // (s3-player, orig3 0x25B9C0: player 0's character is GAME +0x10 (0x185700), which a task's start sets from the
        // task's option 1402 (0x18A0D0 -> 0x1856E0), before the list's first. The reference's --warp 3:2466 (the
        // heist's end) lands with Bentley 25 added at the task's checkpoint and Sly / Murray removed; natively Sly was
        // taken, Bentley stayed removed and the scene's walks of him (SCPT 2467) never ran.)
        if (FScripts3Active())
        {
            // (the game's value lasts, as the original's GAME +0x10: the later loads come up with the last started
            // task's character (the heist: Bentley, also into o_ext). SLY3_TASKPLAYERKEEP=0: only at a task's start.)
            static const bool s_fKeep = [] { const char* sz = std::getenv("SLY3_TASKPLAYERKEEP"); return !sz || sz[0] != '0'; }();
            OID oidPlayer = g_gametasks3.fPlayerNew || s_fKeep ? g_gametasks3.aoidPlayer[0] : -1;
            g_gametasks3.fPlayerNew = false;
            if ((req.flags & 8) && g_taskmgr.oidQueued != -1)
                if (const TASK3* p3 = PtaskFind3(g_taskmgr.oidQueued)) oidPlayer = p3->aoidPlayer[0];
            if (oidPlayer != -1 && !std::getenv("SLY3_NOTASKPLAYER")) pselected = PloFindSwPlayerByOid3(psw.get(), oidPlayer);   // (a proxy: its instance)
            // (any SO: the reference's --warp 30:3721 (the heist's alarm boxes, option 1402 = the RC car 38546, class
            // 0x55) adds the car at the world start (AddLo from 0x25BAA0) and it is player 0's character)
            if (std::getenv("SLY2N_TASKDEBUG"))
                std::fprintf(stderr, "world start: task players %d %d (new %d) -> %d (cid 0x%X)\n", g_gametasks3.aoidPlayer[0], g_gametasks3.aoidPlayer[1],
                             int(g_gametasks3.fPlayerNew), pselected ? pselected->oid : -1, pselected ? pselected->Cid() : 0);
            if (pselected && !FCidDerivedFrom(pselected->Cid(), sly3::CID_SO)) pselected = nullptr;
        }
        // (Sly 2's "oidPoCurrent" progress value: Sly 3 has only the game's value, kept current by 0x1F4B38 (po.cpp); with
        // none (a task started without option 1402) it takes the list 0x344050's first: Sly. SLY3_POCURRENT=1: the old way.)
        const CRef ref = RefGetProgress({"oidPoCurrent"});
        if (!pselected && ref.m_tagk == TAGK_S32 && ref.m_n != -1 && (!FScripts3Active() || std::getenv("SLY3_POCURRENT")))
            pselected = PloFindSwObjectByOid(psw.get(), ref.m_n);
        for (CID cid : {S3CID(0x35), S3CID(0x37), S3CID(0x36)})   // (Sly, Bentley, Murray: Sly 3's 0x34 / 0x36 / 0x35)
            if (!pselected && !psw->mpcidaplo[cid].empty()) pselected = psw->mpcidaplo[cid][0];
        if (pselected)
        {
            AddLo(psw.get(), pselected);
            EnablePo(psw.get(), pselected, 1);
            const CHKPNT* pc = dynamic_cast<const CHKPNT*>(pchk);
            bool fPaired = false;
            if (pc)
                for (const auto& [oidA, oidB] : pc->apair) fPaired = fPaired || oidA == pselected->oid || oidA == pselected->oidInstance;
            if (pchk && !fPaired) TeleportLo(pselected, pchk);
            // (s3-player, orig3 0x25B9C0's i = 1: the second player's character (GAME +0x14, a task's option 1402's second
            // value: one task has Carmelita 28) is added and placed at the checkpoint too and stays in the world; the
            // second pad that would drive it does not exist natively)
            LO* psecond = nullptr;
            if (FScripts3Active() && g_gametasks3.aoidPlayer[1] != -1 && !std::getenv("SLY3_NOTASKPLAYER"))
            {
                psecond = PloFindSwPlayerByOid3(psw.get(), g_gametasks3.aoidPlayer[1]);
                if (psecond && (psecond == pselected || !FCidDerivedFrom(psecond->Cid(), S3CID(0x32)))) psecond = nullptr;
                if (psecond)
                {
                    AddLo(psw.get(), psecond);
                    bool fPaired2 = false;
                    if (pc)
                        for (const auto& [oidA, oidB] : pc->apair) fPaired2 = fPaired2 || oidA == psecond->oid || oidA == psecond->oidInstance;
                    if (pchk && !fPaired2) TeleportLo(psecond, pchk);
                }
            }
            // (but a character the started task's script added before this (SCPT 0x38 at its frame 0, as the branch below
            // keeps: Ep2's bridge_complete SCPT 2622 [3] adds the Guru 29 and [85] walks him; removed here, the walk never
            // ended and 2621 was forced). SLY3_KEEPSCRIPTCHARS=0: removed as before)
            static const bool s_fKeepScript = [] { const char* sz = std::getenv("SLY3_KEEPSCRIPTCHARS"); return !sz || sz[0] != '0'; }();
            for (CID cid = S3CID(0x34); cid >= 0 && cid < g_acls[S3CID(0x34)].cidLast; ++cid)
                for (LO* p : std::vector<LO*>(psw->mpcidaplo[cid]))
                    if (p != pselected && p != psecond && NPoEnabled(p) == 0 && !(s_fKeepScript && p->fInWorld && FLoAddedByScript3(p)) &&
                        !(s_fKeepSplice && p->fInWorld && FLoAddedBySplice3(p)))
                        RemoveLo(psw.get(), p);
        }
    }
    // (s3-hack round 6, orig3 0x25B9C0's last loop runs at every world start: the listed characters (Sly, Bentley, Murray,
    // the Guru, ...) other than the players are removed. When a player that is no character is current before the start
    // (a hack's ship, made current by its own load / enable), the block above was skipped and the characters stayed in the
    // world under the hack board: e.g. Ep3 windmill hack 4 (--world 15:3196) kept the Guru 29 and his effect 34880 (the
    // orange dots seen through the board) and Sly / Bentley / Murray; the reference removes all four at the task start
    // (RemoveLo from 0x25BB84 at vb 1765). SLY3_KEEPCHARS=1: the old way)
    else if (LO* pcur = PloPoCurrent(psw.get()); pcur && !std::getenv("SLY3_KEEPCHARS"))
    {
        const CID cidFirst = S3CID(0x34), cidLim = cidFirst >= 0 ? g_acls[cidFirst].cidLast : -1;
        const bool fChar = pcur->Cid() >= cidFirst && pcur->Cid() < cidLim;
        if (!fChar && FScripts3Active())
        {
            for (CID cid = cidFirst; cid >= 0 && cid < cidLim; ++cid)
                for (LO* p : std::vector<LO*>(psw->mpcidaplo[cid]))
                    if (p != pcur && p->fInWorld && NPoEnabled(p) == 0) RemoveLo(psw.get(), p);
        }
        // (s3-plane) a character already current (e.g. a task's character kept from its start): the other characters go
        // as 0x25B9C0's loop removes them at every world start, but those a script added since the world started (SCPT
        // 0x38) and the task's second player. E.g. h_ext at m3_heist's rowboat (3281):
        // Bentley stayed in the world at the origin, the planes' bullets knocked him into the water and his drowning
        // reloaded the checkpoint (the reference's RAM: only Murray and the boat in the world). SLY3_KEEPCHARS=1: the old way
        else if (fChar)
        {
            LO* psecond = g_gametasks3.aoidPlayer[1] != -1 ? PloFindSwPlayerByOid3(psw.get(), g_gametasks3.aoidPlayer[1]) : nullptr;
            for (CID cid = cidFirst; cid >= 0 && cid < cidLim; ++cid)
                for (LO* p : std::vector<LO*>(psw->mpcidaplo[cid]))
                {
                    if (p != pcur && p != psecond && p->fInWorld && NPoEnabled(p) == 0 && !FLoAddedByScript3(p) && !(s_fKeepSplice && FLoAddedBySplice3(p)))
                    {
                        if (std::getenv("SLY2N_TASKDEBUG")) std::fprintf(stderr, "world start: character %d removed (current %d)\n", p->oid, pcur->oid);
                        RemoveLo(psw.get(), p);
                    }
                }
        }
    }
    // (native job starts: a job started at its world's load (0x1651D8's queued branch, used natively by the
    // --world W:TASK harness and Replay Jobs) comes in at point sly3::POINT_DEFAULT, the safe house's checkpoint, whose handler 5
    // makes the safe house (cid 0x46) the current player once the episode's first jobs are done. The job's opening
    // binocucom talk then finds no speakers (0x1BEED8: 0x2AE / 0x2AF below the current player) and shows empty head
    // windows. In the game a job is reached by a player (the satellite job by play: Sly; the reference's warp: Sly
    // too), so such a start hands the pad to Sly. SLY2N_JOBSTARTSH=1 keeps the safe house. Only for that start
    // (request flags 8): the checkpoint task stays queued (SetTaskChk) after a job, and a story transition into the hub
    // (e.g. Ep8's briefing 3378's exit 3377 after the first job) must leave the safe house current, as the original
    // does.)
    if (LO* pcur = PloPoCurrent(psw.get());
        pcur && pcur->Cid() == S3CID(0x46) && (req.flags & 8) && g_taskmgr.oidQueued != -1 && !psw->mpcidaplo[S3CID(0x35)].empty() &&
        !std::getenv("SLY2N_JOBSTARTSH"))
    {
        LO* psly = psw->mpcidaplo[S3CID(0x35)][0];
        if (!psly->fInWorld) AddLo(psw.get(), psly);
        EnablePo(psw.get(), psly, 1);
    }
    // (s3-player, orig3 0x152720: each player's post-load wears the game's player entry's outfit again: a disguise
    // kept across the reload; outfit3.h)
    if (FScripts3Active()) RestoreOutfits3(psw.get());
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
    split3::OnWorldLoaded(psw.get());   // (PC, splitscreen3.h: the two-player games' split screen; nothing in single player)
    // the challenge manager (challenge3.h): 0x283970 at the world's reset (its HUD 1054, the node), 0x283A50 first in the
    // tasks' start 0x184B60 (the values reset after a start / replay, the HUD shown)
    ChallengeWorldLoaded3(psw.get());
    ChallengeWorldStart3(psw.get());
    hackgame3::OnWorldStart(psw.get());   // (hackgame3.h: the two-player hacking game's ships to their players' pads)
    // (orig3 0x184B60 after the players' placement 0x25B9C0 and the cameras' cut: game/task3.h. The activation's scripts
    // place their actors on the ground (0x22's drop, 0x198D78): natively the collision objects are on the active list
    // only once activated by the camera's distance (UpdateSwActivation, each frame from the first): the pass runs here
    // from the cut camera first, else every drop of the start found no ground (ep1_turf: Sly left in the air))
    if (FActivateTasksPending3())
    {
        sly3::InstallPlayerHooks3();
        g_actview.posCm = g_cm.pos;
        g_actview.grfCm = static_cast<uint32_t>(g_cm.nCur);
        UpdateSwActivation(psw.get());
    }
    ActivateTasksAtStart3();
    BroadcastTaskEvent(psw.get(), 5, nullptr);
    t.oidTaskQueued = -1;
    // orig3 0x288FB0 (Sly 2 0x22B420): the wipe object found again in the new world; one that covered the load
    // uncovers it (in), else idle
    if (t.wipes == WIPES_Loading || t.oidWipe != -1)
    {
        t.ploWipe = t.oidWipe != -1 ? PloFindSwObjectByOid(psw.get(), t.oidWipe) : nullptr;
        if (t.wipes == WIPES_Loading && t.ploWipe) SetWipeState(WIPES_In);
        else SetWipeState(WIPES_Idle);
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
    // orig3 0x288D60 (Sly 2 0x22B198): out until no part plays, then loading; in until none plays, then idle
    case WIPES_Out:
        if (!FWipePlaying(t.ploWipe)) SetWipeState(WIPES_Loading);
        break;
    case WIPES_In:
        if (!FWipePlaying(t.ploWipe)) SetWipeState(WIPES_Idle);
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
