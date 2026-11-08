// autoplay.cpp: a test harness (SLY2N_AUTOPLAY=1, not the game): plays the progression between jobs as a player
// would, skipping the jobs' insides. Answers the card's messages (SQUARE: OK), presses CROSS in the front end (the
// episode menu), takes the current player to an available job marker (switching to the marker's character, as the
// safe house's choice would), and once a job runs requests its active tasks done one by one (a task of another world
// is started by its world's load, as --world W:TASK). Each action is printed ("autoplay: ..."); each autosave is
// copied to $SLY2N_AUTOSAVEDIR. SLY2N_AUTOPLAY_JOBWAIT / _STEP (frames) tune the waits; SLY2N_AUTOPLAY_SKIP="oid,.."
// jobs played by the harness only up to their start (the route / other tools then play them).
#include "joy.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "lo.h"
#include "disc.h"
#include "ai.h"
#include "savedata.h"
#include "ref.h"
#include "menu.h"
#include "po.h"
#include "actor.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "clock.h"
#include "brx.h"
#include "activate.h"
#include "frontend.h"
#include "worldaccess.h"
#include "lo.h"
#include "../platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

void ForEachMarker(const std::function<void(LO*, int kind, int cidPlayer, LO* ptask)>& fn);  // marker.cpp
TASK* PtaskFind(OID oid);
void RequestTaskStartThere(SW* psw, int iwrec, OID oidTask);  // transition.cpp
uint16_t g_grfAutoplay = 0;
int OidSafeHouseShown(const LO* plo, std::vector<OID>* paoidAllowed);   // po.cpp
bool FShopShown();   // shop.cpp
int ShopItemSelected(int* pmsg);   // shop.cpp
static bool FItemOwnedAutoplay(int i) { CRef r = RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(i)}); return r.m_tagk == TAGK_Bool && r.m_bool; }
int g_frameAutoplayFree = 0;
std::unordered_map<int, int> g_mpoidiwrecTask;   // a filtered-out task's first world (brx.cpp)
WORLD_TABLE(g_mpoidiwrecTask);   // (PC: moved aside while a kept world loads, worldglobals.h)
static int IwrecTask(const TASK* pt) { if (pt->iwrec >= 0) return pt->iwrec; auto it = g_mpoidiwrecTask.find(pt->oid); return it == g_mpoidiwrecTask.end() ? -1 : it->second; }   // joy.cpp ORs it into the pad

namespace
{
int EnvInt(const char* sz, int n) { const char* p = std::getenv(sz); return p ? std::atoi(p) : n; }
int s_fPressEnd = 0;
uint16_t s_grfPress = 0;
void Press(int frame, uint16_t grf, int c = 6) { s_grfPress = grf; s_fPressEnd = frame + c; }
JOB* s_pjobLast = nullptr;
int s_oidJobLast = -1;
int s_frameJob = 0, s_frameAct = 0, s_frameIdle = 0;
int s_iwrecLast = -2;
std::map<int, int> s_mpcTele;     // marker oid -> teleports while no job started
std::map<int, int> s_mpcReq;      // task oid -> requests
int s_cSave = 0;
}  // namespace

void AutoplayNoteSave(const std::string& path)
{
    const char* szDir = std::getenv("SLY2N_AUTOSAVEDIR");
    if (!szDir) return;
    std::error_code ec;
    std::filesystem::create_directories(szDir, ec);
    char ach[128];
    std::snprintf(ach, sizeof ach, "%03d_ep%d_w%d_j%d.save", ++s_cSave, IepisodeCur(), g_transition.iwrecCur, s_oidJobLast);
    std::filesystem::copy_file(cane::path(path), std::filesystem::path(szDir) / ach, std::filesystem::copy_options::overwrite_existing, ec);
    std::printf("autoplay: save %s (t %.2f)\n", ach, g_clock.t);
}

void UpdateAutoplay(SW* psw, int frame)
{
    // (debug: SLY2N_FIREEXIT="oid@frame[;..]": that exit fired (armed first), as its world's script / volume would)
    if (static const char* s_szExit = std::getenv("SLY2N_FIREEXIT"); s_szExit && psw)
        for (const char* q = s_szExit; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
        {
            int oid = 0, f = -1;
            if (std::sscanf(q, "%d@%d", &oid, &f) != 2 || f != frame) continue;
            if (LO* plo = PloFindSwObjectByOid(psw, oid))
            {
                std::printf("fireexit frame %d exit %d (state %d)\n", frame, oid, plo->nState);
                SetExitState(plo, 3);
                TriggerExit(plo);
            }
        }
    static const bool s_f = std::getenv("SLY2N_AUTOPLAY") != nullptr;
    if (!s_f || !psw || frame < std::atoi(std::getenv("SLY2N_AUTOPLAY"))) return;
    g_grfAutoplay = frame < s_fPressEnd ? s_grfPress : 0;
    if (g_transition.iwrecCur != s_iwrecLast)
    {
        s_iwrecLast = g_transition.iwrecCur;
        s_frameIdle = s_frameAct = frame;
        std::printf("autoplay: frame %d world %d episode %d\n", frame, s_iwrecLast, IepisodeCur());
    }
    if (std::getenv("SLY2N_AUTOPLAY_DBG") && (frame % 600 == 0 || std::atoi(std::getenv("SLY2N_AUTOPLAY_DBG")) > 1))
        std::printf("autoplay dbg: frame %d pending %d card %d job %d act %d jobf %d joyt %u t %.2f factors %g %g %g %g %g %g %g %g %g po %d\n", frame, int(g_transition.fPending), int(FCardMessageActive()),
                    psw->tasks.pjobCur ? psw->tasks.pjobCur->oid : -1, s_frameAct, s_frameJob, g_grfjoyt, g_clock.t, g_clock.af[0], g_clock.af[1], g_clock.af[2],
                    g_clock.af[3], g_clock.af[4], g_clock.af[5], g_clock.af[6], g_clock.af[7], g_clock.af[8], PloPoCurrent(psw) ? PloPoCurrent(psw)->oid : -1);
    if (g_transition.fPending || frame < s_fPressEnd + 2) return;
    // the card's messages: OK
    if (FCardMessageActive())
    {
        if (frame - s_frameAct > 60) { Press(frame, BTN_Square); s_frameAct = frame; std::printf("autoplay: frame %d card message: SQUARE\n", frame); }
        return;
    }
    // the front end (the episode menu)
    if (g_transition.iwrecCur == 1)
    {
        if (frame - s_frameAct > 90) { Press(frame, BTN_Cross); s_frameAct = frame; std::printf("autoplay: frame %d front end: CROSS\n", frame); }
        return;
    }
    // a screen that pauses the game (clock factor 7: a help / tutorial page) dismissed with CROSS
    if (g_clock.af[7] == 0.0f)
    {
        if (frame - s_frameAct > 90) { Press(frame, BTN_Cross); s_frameAct = frame; std::printf("autoplay: frame %d a pausing screen: CROSS\n", frame); }
        return;
    }
    // the shop (ThiefNet) left; the safe house's choice: Sly when he may be chosen, else the first character
    // SLY2N_AUTOPLAY_BUY="item,..": those ThiefNet items bought at the safe house (e.g. 27 the Paraglide, which Ep5's
    // t5_paraglide_purchase waits for), each once
    static std::vector<int> s_aitemBuy = [] {
        std::vector<int> a;
        if (const char* sz = std::getenv("SLY2N_AUTOPLAY_BUY"))
            for (const char* q = sz; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr) a.push_back(std::atoi(q));
        return a;
    }();
    auto fWantItem = [] { for (int i : s_aitemBuy) if (!FItemOwnedAutoplay(i)) return i; return -1; };
    if (FShopShown())
    {
        if (frame - s_frameAct <= 30) return;
        s_frameAct = frame;
        int msg = -1;
        const int itemSel = ShopItemSelected(&msg);
        const int itemWant = fWantItem();
        static int s_cNav = 0;
        if (itemWant != -1 && s_cNav < 80)
        {
            if (itemSel == itemWant) { Press(frame, BTN_Cross); std::printf("autoplay: frame %d the shop: item %d CROSS (message %d)\n", frame, itemSel, msg); }
            else
            {
                ++s_cNav;
                Press(frame, BTN_Down, 4);
                if (std::getenv("SLY2N_AUTOPLAY_DBG")) std::printf("autoplay: frame %d the shop: item %d selected (want %d)\n", frame, itemSel, itemWant);
            }
            return;
        }
        s_cNav = 0;
        Press(frame, BTN_Triangle);
        std::printf("autoplay: frame %d the shop: TRIANGLE\n", frame);
        return;
    }
    if (LO* ppo = PloPoCurrent(psw); ppo && ppo->Cid() == S3CID(0x46))
    {
        std::vector<OID> aoid;
        const int oidCur = OidSafeHouseShown(ppo, &aoid);
        int oidWant = -1;
        if (fWantItem() != -1) for (OID o : aoid) if (o == 10) oidWant = 10;   // (ThiefNet first)
        for (OID o : aoid) if (oidWant == -1 && o == 7) oidWant = 7;
        for (OID o : aoid) if (oidWant == -1 && o >= 7 && o <= 9) oidWant = o;
        if (frame - s_frameAct > 60 && oidCur != -1)
        {
            s_frameAct = frame;
            Press(frame, oidCur == oidWant ? BTN_Cross : BTN_Right);
            std::printf("autoplay: frame %d safe house shows %d (want %d): %s\n", frame, oidCur, oidWant, oidCur == oidWant ? "CROSS" : "RIGHT");
        }
        return;
    }
    JOB* pjob = psw->tasks.pjobCur;
    if (pjob != s_pjobLast)
    {
        std::printf("autoplay: frame %d t %.2f world %d job %d -> %d\n", frame, g_clock.t, g_transition.iwrecCur, s_pjobLast ? s_oidJobLast : -1, pjob ? pjob->oid : -1);
        s_pjobLast = pjob;
        if (pjob) s_oidJobLast = pjob->oid;
        s_frameJob = s_frameIdle = frame;
        s_mpcTele.clear();
        extern int g_frameAutoplayFree;
        g_frameAutoplayFree = frame;   // (the 40 s of a lock left by the job count from its end)
    }
    const bool fPadFree = g_grfjoyt == 7;
    if (pjob)
    {
        static const int s_cWait = EnvInt("SLY2N_AUTOPLAY_JOBWAIT", 300), s_cStep = EnvInt("SLY2N_AUTOPLAY_STEP", 180);
        static const char* s_szSkip = std::getenv("SLY2N_AUTOPLAY_SKIP");
        if (s_szSkip && std::strstr(s_szSkip, std::to_string(pjob->oid).c_str())) return;
        if (frame - s_frameJob < s_cWait || frame - s_frameAct < s_cStep) return;
        // the job's active tasks: one live one requested done; else one of another world started there
        TASK* ptLive = nullptr;
        TASK* ptOther = nullptr;
        for (LO* plo : psw->mpcidaplo[S3CID(0x85)])
        {
            TASK* pt = static_cast<TASK*>(plo);
            if (pt->pjob != pjob || pt->nState != 1) continue;
            if (!pt->fMission && pt->nMode != 2 && pt->nMode != 3) { if (!ptLive) ptLive = pt; }
            else if (!ptOther && IwrecTask(pt) >= 0) ptOther = pt;
        }
        s_frameAct = frame;
        static int s_frameSome = 0;
        static JOB* s_pjobNone = nullptr;
        if (ptLive || ptOther || s_pjobNone != pjob) { s_frameSome = frame; s_pjobNone = pjob; }
        if (ptLive)
        {
            if (++s_mpcReq[ptLive->oid] > 8)
            {
                if (s_mpcReq[ptLive->oid] % 20 == 9) std::printf("autoplay: frame %d STUCK: job %d task %d stays active after 8 requests\n", frame, pjob->oid, ptLive->oid);
                return;
            }
            std::printf("autoplay: frame %d t %.2f job %d task %d requested done%s\n", frame, g_clock.t, pjob->oid, ptLive->oid, fPadFree ? "" : " (pad locked)");
            FRequestTaskStateOid(ptLive->oid, 2);
            return;
        }
        static int s_frameOther = 0;
        static TASK* s_ptOther = nullptr;
        if (ptOther != s_ptOther) { s_ptOther = ptOther; s_frameOther = frame; }
        if (ptOther && frame - s_frameOther < EnvInt("SLY2N_AUTOPLAY_OTHER", 1500)) return;   // (the game's own way there first)
        if (ptOther)
        {
            if (++s_mpcReq[ptOther->oid] > 2)
            {
                if (s_mpcReq[ptOther->oid] % 20 == 3) std::printf("autoplay: frame %d STUCK: job %d task %d (world %d) stays active after 2 starts\n", frame, pjob->oid, ptOther->oid, IwrecTask(ptOther));
                return;
            }
            std::printf("autoplay: frame %d t %.2f job %d task %d of world %d: started there (warp)\n", frame, g_clock.t, pjob->oid, ptOther->oid, IwrecTask(ptOther));
            RequestTaskStartThere(psw, IwrecTask(ptOther), ptOther->oid);
            return;
        }
        // none active for a while: the next task (its prerequisites done) started, in its world (a walk into an
        // exit / a scene the harness skipped)
        if (frame - s_frameSome < EnvInt("SLY2N_AUTOPLAY_NONE", 1800)) return;
        s_frameSome = frame;
        for (LO* plo : psw->mpcidaplo[S3CID(0x85)])
        {
            TASK* pt = static_cast<TASK*>(plo);
            if (pt->pjob != pjob || pt->nState != 0) continue;
            bool fReady = !pt->aptaskPrereq.empty();
            for (TASK* pp : pt->aptaskPrereq) fReady = fReady && pp->nState >= 2;
            if (!fReady || ++s_mpcReq[pt->oid] > 2) continue;
            const bool fHere = !pt->fMission;
            std::printf("autoplay: frame %d t %.2f job %d: no active task; next %d (world %d) %s\n", frame, g_clock.t, pjob->oid, pt->oid, IwrecTask(pt),
                        fHere ? "activated" : "started there (warp)");
            if (fHere) FRequestTaskStateOid(pt->oid, 4);
            else RequestTaskStartThere(psw, IwrecTask(pt), pt->oid);
            return;
        }
        // (a job whose tasks are all done stays current when its last task's own script was skipped, e.g. Ep8's
        // turret 3466 in world 43 with 3470 current: the reference keeps it so after the same requests): another
        // job's available task here asked to start
        for (LO* plo : psw->mpcidaplo[S3CID(0x85)])
        {
            TASK* pt = static_cast<TASK*>(plo);
            if (pt->nState != 1 || !pt->pjob || pt->pjob == pjob || pt->fMission || s_mpcReq[pt->oid]++ >= 1) continue;
            std::printf("autoplay: frame %d t %.2f job %d done but current: task %d (job %d) here activated\n", frame, g_clock.t, pjob->oid, pt->oid, pt->pjob->oid);
            FRequestTaskStateOid(pt->oid, 4);
            return;
        }
        std::printf("autoplay: frame %d STUCK in job %d (no active or ready task):", frame, pjob->oid);
        for (LO* plo : psw->mpcidaplo[S3CID(0x85)])
        {
            TASK* pt = static_cast<TASK*>(plo);
            if (pt->pjob != pjob) continue;
            std::printf(" %d:%d%s", pt->oid, pt->nState, pt->fMission ? "x" : "");
            for (TASK* pp : pt->aptaskPrereq) std::printf("<%d", pp->oid);
        }
        std::printf("\n");
        return;
    }
    // no job: to a marker
    static const int s_cIdle = EnvInt("SLY2N_AUTOPLAY_IDLE", 300);
    // the pad left locked with no job (a skipped job scene's lock, e.g. Neela's chase requested done: the reference
    // keeps it locked the same way): freed after 40 s
    int& s_frameFree = g_frameAutoplayFree;
    if (fPadFree) s_frameFree = frame;
    else if (frame - s_frameFree > 2400)
    {
        void UnlockJoy(uint32_t grf);
        extern uint32_t g_grfjoyLock;
        std::printf("autoplay: frame %d t %.2f world %d no job and the pad locked (0x%X) for 40 s: unlocked\n", frame, g_clock.t, g_transition.iwrecCur, g_grfjoyLock);
        std::printf("autoplay:   (pauses %d talks %d: cleared)\n", g_gamepause.c78, g_gamepause.c7C);
        g_gamepause = GAMEPAUSE{};
        UnlockJoy(0xFFFFFFFFu);
        s_frameFree = frame;
    }
    if (frame - s_frameAct < s_cIdle || !fPadFree) return;
    LO* pmk = nullptr;
    LO* ptaskMk = nullptr;
    int cid = 0;
    int cMarkers = 0;
    ForEachMarker([&](LO* plo, int kind, int cidPlayer, LO* ptask) {
        if (kind == 0) ++cMarkers;
        if (kind != 0 || !plo->fInWorld || pmk) return;
        if (s_mpcTele[plo->oid] >= 3) return;
        pmk = plo;
        cid = cidPlayer;
        ptaskMk = ptask;
    });
    s_frameAct = frame;
    if (pmk)
    {
        // the character (the safe house's choice): the marker's task's player (+0xC0) when known, else tried in
        // turn: Sly, Murray, Bentley (the marker's script checks the player)
        LO* ppo = PloPoCurrent(psw);
        const TASK* pt = ptaskMk && ptaskMk->Cid() == S3CID(0x85) ? static_cast<const TASK*>(ptaskMk) : nullptr;
        static const int s_aoid[3] = {7, 9, 8};
        // (the markers' own oids: 17472 Sly's, 28271 Murray's, 28072 Bentley's)
        const int oidMk = pmk->oid == 17472 ? 7 : pmk->oid == 28271 ? 9 : pmk->oid == 28072 ? 8 : -1;
        const int oidPo = pt && pt->oidPo >= 7 && pt->oidPo <= 9 ? pt->oidPo : oidMk != -1 && s_mpcTele[pmk->oid] < 2 ? oidMk : s_aoid[s_mpcTele[pmk->oid] % 3];
        if (!ppo || ppo->oid != oidPo)
            if (LO* p = PloFindSwObjectByOid(psw, oidPo))
            {
                if (!p->fInWorld) AddLo(psw, p);
                EnablePo(psw, p, 1);
                std::printf("autoplay: frame %d player %d (marker %d)\n", frame, oidPo, pmk->oid);
                return;   // (the marker sees the new player away first; the teleport next time)
            }
        ++s_mpcTele[pmk->oid];
        std::printf("autoplay: frame %d t %.2f world %d to marker %d (%d) class 0x%X task %d po %d\n", frame, g_clock.t, g_transition.iwrecCur, pmk->oid,
                    s_mpcTele[pmk->oid], cid, ptaskMk ? ptaskMk->oid : -1, oidPo);
        if (ppo) TeleportActor(ppo, pmk);
        return;
    }
    // no marker in a world without job markers (a job's own world after its end): the exit back to the episode's
    // main world fired (the generic exits sit at the origin, fired by the world's own volumes / scripts)
    if (cMarkers == 0 && frame - s_frameIdle > 600)
    {
        static std::map<int, int> s_mpcExit;
        const WREC* pw = PwrecEpisodeMain(IepisodeCur());
        for (LO* plo : AploOfClass(psw, S3CID(0x1A)))
        {
            const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(plo);
            if (!pw || !px || px->aabKey.empty() || FkFromRecord(px->aabKey[0].data()).id != pw->fk.id) continue;
            if (plo->nState != 1 && plo->nState != 3) continue;
            if (s_mpcExit[plo->oid]++ >= 1) continue;
            std::printf("autoplay: frame %d t %.2f world %d no job, no marker: exit %d to the main world fired\n", frame, g_clock.t, g_transition.iwrecCur, plo->oid);
            if (plo->nState == 1) SetExitState(plo, 3);
            TriggerExit(plo);
            s_frameIdle = frame;
            return;
        }
    }
    // nothing for a while: a task whose prerequisites are done but that nothing started (its starter was a scene or
    // a script the harness skipped): started (here, or by its world's load)
    if (frame - s_frameIdle > EnvInt("SLY2N_AUTOPLAY_NONE", 1800))
    {
        static std::map<int, int> s_mpcStart;
        for (LO* plo : psw->mpcidaplo[S3CID(0x85)])
        {
            TASK* pt = static_cast<TASK*>(plo);
            if (pt->nState == 1 && pt->pjob && !pt->fMission && s_mpcStart[pt->oid]++ < 1)
            {
                // (an available job's task of this world with no marker: asked to start, as a marker's entry does)
                std::printf("autoplay: frame %d t %.2f world %d no job, no marker: active task %d (job %d) here: activated\n", frame, g_clock.t,
                            g_transition.iwrecCur, pt->oid, pt->pjob->oid);
                FRequestTaskStateOid(pt->oid, 4);
                s_frameIdle = frame;
                return;
            }
            if (pt->nState == 1 && pt->pjob && pt->fMission && IwrecTask(pt) >= 0 && s_mpcStart[pt->oid]++ < 1)
            {
                // (an available job's task of another world with no marker here: its world loaded with it)
                std::printf("autoplay: frame %d t %.2f world %d no job, no marker: active task %d (job %d) of world %d started there (warp)\n", frame, g_clock.t,
                            g_transition.iwrecCur, pt->oid, pt->pjob->oid, IwrecTask(pt));
                RequestTaskStartThere(psw, IwrecTask(pt), pt->oid);
                s_frameIdle = frame;
                return;
            }
            if (pt->nState != 0 || pt->aptaskPrereq.empty()) continue;
            bool fReady = true;
            for (TASK* pp : pt->aptaskPrereq) fReady = fReady && pp->nState >= 2;
            if (!fReady || s_mpcStart[pt->oid]++ >= 1) continue;
            const bool fHere = !pt->fMission;
            std::printf("autoplay: frame %d t %.2f world %d no job, no marker: ready task %d (job %d, world %d) %s\n", frame, g_clock.t, g_transition.iwrecCur,
                        pt->oid, pt->pjob ? pt->pjob->oid : -1, IwrecTask(pt), fHere ? "activated" : "started there (warp)");
            if (fHere) FRequestTaskStateOid(pt->oid, 4);
            else if (IwrecTask(pt) >= 0) RequestTaskStartThere(psw, IwrecTask(pt), pt->oid);
            s_frameIdle = frame;
            return;
        }
    }
    // nothing to do: the pad used (a scene that waits for it), and now and then the active tasks listed
    Press(frame, BTN_Cross);
    if (frame - s_frameIdle > 3600)
    {
        s_frameIdle = frame;
        std::printf("autoplay: frame %d t %.2f world %d IDLE: no job, no marker; active:", frame, g_clock.t, g_transition.iwrecCur);
        for (LO* plo : psw->mpcidaplo[S3CID(0x85)])
        {
            TASK* pt = static_cast<TASK*>(plo);
            if (pt->nState == 1) std::printf(" %d(job %d%s)", pt->oid, pt->pjob ? pt->pjob->oid : -1, pt->fMission ? " other world" : "");
        }
        std::printf("\n");
    }
}
