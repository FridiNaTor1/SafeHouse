// See mapstates.h.
#include "mapstates.h"

#include "activate.h"
#include "cls.h"
#include "disc.h"
#include "lo.h"
#include "modsave.h"
#include "savedata.h"
#include "smrun.h"
#include "so.h"
#include "splayer.h"
#include "splicenames.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "worldparts.h"
#include "../platform/mod_worlds.h"
#include "../platform/platform.h"
#include "../platform/safe_paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& error);   // sw.cpp
void InitClasses();                                                                  // cls.cpp
std::string SpliceObjectName(const SW* psw, int oid, bool fOtherEpisodes = true);    // splicedump.cpp
void InstallSpliceDumpReader(std::map<int, std::string>* pmptext);                   // splicedump.cpp

namespace
{
using ojson = nlohmann::ordered_json;

const std::map<int, std::string>* s_pmptext = nullptr;   // (the command: the world's scripts, by group)
std::map<int, std::vector<WORLDOBJREC>> s_mprec;          // (the command: every group's top-level records)

TASK* PtaskOf(LO* plo) { return plo && plo->Cid() == 0x85 ? static_cast<TASK*>(plo) : nullptr; }
JOB* PjobOf(LO* plo) { return plo && plo->Cid() == 0x84 ? static_cast<JOB*>(plo) : nullptr; }

std::vector<TASK*> AptaskOf(SW* psw)
{
    std::vector<TASK*> a;
    if (psw)
        for (LO* plo : psw->mpcidaplo[0x85])
            if (TASK* p = PtaskOf(plo)) a.push_back(p);
    return a;
}

// a task's name: its save path's ("t1_heist_8_siege" of "t1_heist_8_siege.state"), else its oid
std::string NameOfTask(const TASK* p)
{
    if (!p) return "?";
    if (p->prE0.F())
    {
        std::string s = StrProgressKey(p->prE0.akey);
        if (s.size() > 6 && s.compare(s.size() - 6, 6, ".state") == 0) s.resize(s.size() - 6);
        if (!s.empty()) return s;
    }
    return "task " + std::to_string(p->oid);
}
std::string NameOfJob(SW* psw, const JOB* p)
{
    if (!p) return "?";
    if (p->n6c != -1)
    {
        const std::string s = StrLocalized(psw, p->n6c);
        if (!s.empty()) return s;
    }
    if (p->ptaskLast)
    {
        // (no title: its last task's save name without the task part, "t1_dimitri_boss")
        std::string s = NameOfTask(p->ptaskLast);
        if (const auto u = s.rfind('_'); u != std::string::npos && u > 3) s.resize(u);
        return s;
    }
    return "job " + std::to_string(p->oid);
}
std::string NameOfObject(SW* psw, OID oid)
{
    std::string s = SpliceObjectName(psw, oid);
    if (s.rfind("FK$X", 0) == 0) s = s.substr(4);
    return (s.empty() ? std::string("object") : s) + " (" + std::to_string(oid) + ")";
}
std::string Join(const std::vector<std::string>& a, const char* sep = ", ")
{
    std::string s;
    for (std::size_t i = 0; i < a.size(); ++i) s += (i ? sep : "") + a[i];
    return s;
}
std::string StatesWords(uint32_t grf)
{
    static const char* const s_asz[6] = {"locked", "available", "done", "done and closed", "being played", "not being played"};
    std::vector<std::string> a;
    for (int i = 0; i < 6; ++i)
        if (grf & (1u << i)) a.push_back(s_asz[i]);
    return a.empty() ? std::string("no state") : Join(a, " or ");
}

// the task graph linked (StartTask's links; AjobinfoOfLoadedWorld made them on a fresh world)
void LinkTasks(SW* psw)
{
    const std::vector<TASK*> a = AptaskOf(psw);
    std::map<OID, TASK*> mp;
    for (TASK* p : a) mp[p->oid] = p;
    for (TASK* p : a)
        if (p->aptaskPrereq.empty() && !p->an90.empty())
            for (int32_t oid : p->an90)
                if (auto it = mp.find(oid); it != mp.end()) p->aptaskPrereq.push_back(it->second);
}

// The task states of a progress state: "start" every task locked (the ones without prerequisites become available
// when restored), "complete" every task done and closed, "after:<job>" the job's last task and everything before it
// done and closed, the tasks whose prerequisites are all done then available, the rest locked
bool FTaskStatesOf(SW* psw, const std::string& id, std::vector<std::pair<OID, int>>& atask)
{
    atask.clear();
    const std::vector<TASK*> a = AptaskOf(psw);
    if (id == "start" || id == "complete")
    {
        for (TASK* p : a) atask.emplace_back(p->oid, id == "start" ? 0 : 3);
        return true;
    }
    const bool fBefore = id.rfind("before:", 0) == 0;
    if (id.rfind("after:", 0) != 0 && !fBefore) return false;
    const OID oidJob = std::atoi(id.c_str() + (fBefore ? 7 : 6));
    JOB* pjob = nullptr;
    for (LO* plo : psw->mpcidaplo[0x84])
        if (plo->oid == oidJob) pjob = PjobOf(plo);
    if (!pjob || !pjob->ptaskLast || !pjob->ptaskFirst) return false;
    LinkTasks(psw);
    std::set<TASK*> setDone;
    // ("before:<job>": what its first task needs done, the first task available)
    std::vector<TASK*> stack = fBefore ? pjob->ptaskFirst->aptaskPrereq : std::vector<TASK*>{pjob->ptaskLast};
    while (!stack.empty())
    {
        TASK* p = stack.back();
        stack.pop_back();
        if (!setDone.insert(p).second) continue;
        for (TASK* q : p->aptaskPrereq) stack.push_back(q);
    }
    for (TASK* p : a)
    {
        int n = 0;
        if (setDone.count(p)) n = 3;
        else if (!p->aptaskPrereq.empty() &&
                 std::all_of(p->aptaskPrereq.begin(), p->aptaskPrereq.end(), [&](TASK* q) { return setDone.count(q) > 0; }))
            n = 1;
        atask.emplace_back(p->oid, n);
    }
    return true;
}

std::string CondWords(SW* psw, const cane::mods::StateCond& c)
{
    switch (c.kind)
    {
    case cane::mods::StateCond::Task:
    {
        LO* plo = psw ? PloFindSwObjectByOid(psw, c.oid) : nullptr;
        uint32_t grf = 0;
        for (int n : c.states) grf |= 1u << n;
        return "the task " + (PtaskOf(plo) ? NameOfTask(PtaskOf(plo)) : std::to_string(c.oid)) + " is " + StatesWords(grf);
    }
    case cane::mods::StateCond::Job:
    {
        LO* plo = psw ? PloFindSwObjectByOid(psw, c.oid) : nullptr;
        return "the job " + (PjobOf(plo) ? NameOfJob(psw, PjobOf(plo)) : std::to_string(c.oid)) + (c.done ? " is done" : " is not done yet");
    }
    default: return "the progress value " + c.path + " is " + std::to_string(c.value);
    }
}
std::string WhenWords(SW* psw, const cane::mods::MapState& s)
{
    if (s.when.empty()) return "always";
    std::vector<std::string> a;
    for (const cane::mods::StateCond& c : s.when) a.push_back(CondWords(psw, c));
    return "when " + Join(a, " and ");
}

std::string WorldNameOf(int iwrec)
{
    const auto& awrec = AwrecWorlds();
    if (iwrec >= 0 && iwrec < int(awrec.size())) return g_disc.SzWorldName(awrec[std::size_t(iwrec)].fk.isector);
    return std::string();
}

// ---- the run's request and the custom states
std::vector<std::string> s_aidRequest;
std::set<std::string> s_setForced;   // custom states forced on (--map-state custom:ID)
std::string s_pathPresence;
std::string s_strStateRun;

struct LIVESTATE
{
    const cane::mods::MapState* ps = nullptr;
    bool fOn = false, fInit = false;
    std::vector<OID> aoidRemoved;   // (taken out by it: back when it ends)
};
std::vector<LIVESTATE> s_alive;
SW* s_pswLive = nullptr;

bool FCondHolds(SW* psw, const cane::mods::StateCond& c)
{
    switch (c.kind)
    {
    case cane::mods::StateCond::Task:
    {
        const int n = NTaskStateOid(c.oid, nullptr);
        return std::find(c.states.begin(), c.states.end(), n) != c.states.end();
    }
    case cane::mods::StateCond::Job:
    {
        for (LO* plo : psw->mpcidaplo[0x84])
            if (plo->oid == c.oid)
                if (JOB* pj = PjobOf(plo); pj && pj->ptaskLast)
                {
                    bool fDone = false;
                    NTaskStateOid(pj->ptaskLast->oid, &fDone);
                    return fDone == c.done;
                }
        return !c.done;
    }
    default:
    {
        std::vector<std::string> apath;
        std::stringstream ss(c.path);
        for (std::string s; std::getline(ss, s, '.');) apath.push_back(s);
        const CRef r = RefGetProgress(apath);
        const int n = r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_Bool ? int(r.m_bool) : r.m_tagk == TAGK_F32 ? int(r.m_g) : 0;
        return n == c.value;
    }
    }
}

void ApplyLive(SW* psw, LIVESTATE& l, bool fOn)
{
    const cane::mods::MapState& s = *l.ps;
    if (l.fInit && l.fOn == fOn) return;
    const bool fFirst = !l.fInit;
    l.fInit = true;
    l.fOn = fOn;
    for (OID oid : s.add)
        if (LO* plo = PloFindSwObjectByOid(psw, oid)) { if (fOn) AddLo(psw, plo); else RemoveLo(psw, plo); }
    if (fOn)
    {
        for (OID oid : s.remove)
            if (LO* plo = PloFindSwObjectByOid(psw, oid); plo && plo->fInWorld)
            {
                RemoveLo(psw, plo);
                l.aoidRemoved.push_back(oid);
            }
        for (const cane::mods::StateSm& m : s.sm)
        {
            LO* plo = PloFindSwObjectByOid(psw, m.object);
            if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            for (SMA* p : g_apsma)
                if (p->palo == plo && (m.sm == -1 || (p->psm && p->psm->oid == m.sm)))
                {
                    if (fFirst) SnapSmaGoal(p, m.state);
                    else SetSmaGoal(p, m.state);
                    break;
                }
        }
    }
    else
    {
        for (OID oid : l.aoidRemoved)
            if (LO* plo = PloFindSwObjectByOid(psw, oid)) AddLo(psw, plo);
        l.aoidRemoved.clear();
    }
    std::printf("map-state: %s \"%s\" %s\n", s.world.c_str(), s.id.c_str(), fOn ? "on" : "off");
}

void EvaluateLive()
{
    if (!s_pswLive) return;
    for (LIVESTATE& l : s_alive)
    {
        bool fOn = s_setForced.count(l.ps->id) > 0;
        if (!fOn)
        {
            fOn = true;
            for (const cane::mods::StateCond& c : l.ps->when) fOn = fOn && FCondHolds(s_pswLive, c);
        }
        ApplyLive(s_pswLive, l, fOn);
    }
}
}  // namespace

// ---- the states of a world
bool FMapStatesOfWorld(SW* psw, int iwrec, std::vector<MAPSTATEINFO>& astate, std::vector<std::string>* palines)
{
    astate.clear();
    if (!psw) return false;
    const std::vector<JOBINFO> ajob = AjobinfoOfLoadedWorld(psw);   // (the tasks' fields and links, the jobs in story order)
    LinkTasks(psw);
    const std::string world = WorldNameOf(iwrec);
    // the progress points: before any job, after each, all done; each job played
    {
        MAPSTATEINFO s;
        s.id = "start";
        s.kind = "progress";
        s.name = "Start of the episode";
        s.trigger = ajob.empty() ? "a fresh save (no task of this world done)" : "before any job (a fresh episode: no task done)";
        FTaskStatesOf(psw, s.id, s.atask);
        astate.push_back(s);
    }
    for (const JOBINFO& j : ajob)
    {
        LO* plo = PloFindSwObjectByOid(psw, j.oidJob);
        JOB* pj = PjobOf(plo);
        if (!pj) continue;
        const std::string name = NameOfJob(psw, pj);
        MAPSTATEINFO d;
        d.id = "during:" + std::to_string(j.oidJob);
        d.kind = "job";
        d.name = "Playing " + name;
        d.oidJob = j.oidJob;
        d.oidTaskStart = j.oidTaskFirst;
        d.after = "before:" + std::to_string(j.oidJob);
        LO* pf = PloFindSwObjectByOid(psw, j.oidTaskFirst);
        d.trigger = "while the job " + name + " is played (the game started at its first task " + NameOfTask(PtaskOf(pf)) + ")";
        astate.push_back(d);
        MAPSTATEINFO s;
        s.id = "after:" + std::to_string(j.oidJob);
        s.kind = "progress";
        s.name = "After " + name;
        LO* pl = PloFindSwObjectByOid(psw, j.oidTaskLast);
        s.trigger = "once the job " + name + " is done (its last task " + NameOfTask(PtaskOf(pl)) + " done, and every task before it)";
        if (!FTaskStatesOf(psw, s.id, s.atask)) continue;
        astate.push_back(s);
    }
    if (!ajob.empty())
    {
        MAPSTATEINFO s;
        s.id = "complete";
        s.kind = "progress";
        s.name = "Episode complete";
        s.trigger = "once every task of the episode is done";
        FTaskStatesOf(psw, s.id, s.atask);
        astate.push_back(s);
    }
    // the packages' states of this world
    for (const auto& [pkg, st] : g_disc.AmapState())
        if (st.world == world)
        {
            MAPSTATEINFO s;
            s.id = "custom:" + st.id;
            s.kind = "custom";
            s.name = st.name;
            s.package = pkg;
            s.trigger = WhenWords(psw, st);
            astate.push_back(s);
        }
    if (!palines) return true;
    // what decides them, in plain words
    std::vector<std::string>& al = *palines;
    auto objects = [&](const std::vector<OID>& aoid) {
        std::vector<std::string> a;
        for (std::size_t i = 0; i < aoid.size() && i < 8; ++i) a.push_back(NameOfObject(psw, aoid[i]));
        if (aoid.size() > 8) a.push_back(std::to_string(aoid.size() - 8) + " more");
        return Join(a);
    };
    auto lists = [&](const std::string& who, const std::vector<TASKOBJE>& ae, const std::vector<TASKSME>& asm_) {
        std::map<uint32_t, std::vector<OID>> mp;
        for (const TASKOBJE& e : ae) mp[e.grf].push_back(e.oidA);
        for (const auto& [grf, aoid] : mp)
            al.push_back(who + ": " + objects(aoid) + " in the world while it is " + StatesWords(grf) + ", taken out otherwise");
        for (const TASKSME& e : asm_)
        {
            std::string st = e.nState == -1 ? std::string("in each state, the state machine's state for it") : "when it is " + StatesWords(1u << e.nState);
            al.push_back(who + ": " + NameOfObject(psw, e.oid) + "'s state machine" + (e.iSm != -1 ? " " + std::to_string(e.iSm) : std::string()) +
                         " goes to " + (e.idState == -1 ? std::string("the task state's own state (1554..1557)") : "state " + std::to_string(e.idState)) +
                         " " + st + (e.fInstant ? " (at once)" : ""));
        }
    };
    int ctaskList = 0;
    for (TASK* p : AptaskOf(psw))
    {
        if (p->aobje.empty() && p->asme.empty() && !p->ploShown) continue;
        ++ctaskList;
        const std::string who = "Task " + NameOfTask(p) + (p->pjob ? " (job " + NameOfJob(psw, p->pjob) + ")" : std::string());
        lists(who, p->aobje, p->asme);
        if (p->ploShown) al.push_back(who + ": " + NameOfObject(psw, p->ploShown->oid) + " shown while it is available (its marker)");
    }
    for (LO* plo : psw->mpcidaplo[0x84])
        if (JOB* pj = PjobOf(plo); pj && (!pj->aobje.empty() || !pj->asme.empty()))
            lists("Job " + NameOfJob(psw, pj), pj->aobje, pj->asme);
    // the scripts: objects added / removed / placed under conditions on the tasks, jobs, progress or the place
    if (s_pmptext)
    {
        SPLICEWORLDNAMES names;
        auto ctxOf = [&]() {
            SPLICENAMECTX ctx;
            ctx.pfnObject = [psw](int oid) { return SpliceObjectName(psw, oid); };
            return ctx;
        };
        for (const auto& [igrp, raw] : *s_pmptext) SpliceCollectNames(raw, ctxOf(), names);
        for (const auto& [igrp, raw] : *s_pmptext)
        {
            SPLICENAMECTX ctx = ctxOf();
            ctx.pworld = &names;
            std::vector<SPLICEOBJACT> aact;
            std::string err;
            if (!FSpliceObjectActions(raw, ctx, aact, err)) continue;
            for (const SPLICEOBJACT& a : aact)
            {
                const std::string& c = a.cond;
                const bool fState = c.find(".state") != std::string::npos || c.find(".done?") != std::string::npos ||
                                    c.find(".current?") != std::string::npos || c.find("get-progress") != std::string::npos ||
                                    c.find("world-index") != std::string::npos || c.find("current-job") != std::string::npos ||
                                    c.find("current-task") != std::string::npos || c.find("item-acquired") != std::string::npos ||
                                    c.find("times-played") != std::string::npos;
                if (!fState) continue;
                const char* verb = a.optid == 39 ? "adds" : a.optid == 40 ? "removes" : a.optid == 1019 ? "places" : a.optid == 1024 ? "releases" : "acts on";
                std::string g = igrp >= 0 && igrp < int(psw->awe.size()) ? std::string(psw->awe[std::size_t(igrp)].achName) : std::string();
                if (g.rfind("FK$X", 0) == 0) g = g.substr(4);
                al.push_back("Script " + g + " (" + a.event + "): " + verb + " " + (a.oid > 0 ? NameOfObject(psw, a.oid) : a.who) + " if " + c);
            }
        }
    }
    // world filters and objects taken out at the start
    {
        std::map<std::vector<int>, int> mpfilters;
        for (const auto& [igrp, arec] : s_mprec)
            for (const WORLDOBJREC& r : arec)
                if (r.k > 0) ++mpfilters[{r.k}];
        int cfilter = 0;
        for (const auto& [k, n] : mpfilters) cfilter += n;
        if (cfilter) al.push_back(std::to_string(cfilter) + " top-level objects have world filters (they load only in the listed world records)");
        int c26 = 0;
        for (const auto& up : psw->aploAll)
            if (up->fRemoveAtStart) ++c26;
        if (c26) al.push_back(std::to_string(c26) + " objects are taken out at the start (option 26: a task, job or script adds them)");
    }
    (void)ctaskList;
    return true;
}

namespace
{
ojson JsonOfStates(SW* psw, int iwrec, const std::vector<MAPSTATEINFO>& astate, const std::vector<std::string>& alines)
{
    ojson j;
    j["world"] = WorldNameOf(iwrec);
    j["record"] = iwrec;
    ojson as = ojson::array();
    for (const MAPSTATEINFO& s : astate)
    {
        ojson e;
        e["id"] = s.id;
        e["name"] = s.name;
        e["kind"] = s.kind;
        e["trigger"] = s.trigger;
        if (s.kind == "progress")
        {
            ojson t = ojson::array();
            for (const auto& [oid, n] : s.atask)
            {
                LO* plo = PloFindSwObjectByOid(psw, oid);
                t.push_back({{"task", oid}, {"name", NameOfTask(PtaskOf(plo))}, {"state", n}});
            }
            e["tasks"] = t;
        }
        if (s.kind == "job")
        {
            e["job"] = s.oidJob;
            e["start_task"] = s.oidTaskStart;
            e["from"] = s.after;
        }
        if (s.kind == "custom") e["package"] = s.package;
        as.push_back(e);
    }
    j["states"] = as;
    j["decided_by"] = alines;
    return j;
}

std::unique_ptr<SW> PswLoadStatic(const std::string& w, int& iwrec, std::map<int, std::string>& mptext, std::string& error)
{
    InitClasses();
    FK fk;
    int filter = 0;
    if (!FResolveWorld(w, fk, filter, error)) return nullptr;
    EnsureWorldRecords();
    iwrec = std::atoi(w.c_str());
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    mptext.clear();
    s_mprec.clear();
    InstallSpliceDumpReader(&mptext);
    std::size_t cbRead = 0, cbFile = 0;
    const bool fOk = FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile);
    g_disc.SetPartReader(nullptr);
    if (!fOk) { error = "world " + w + " did not load"; return nullptr; }
    // (the records' filters: the objects parts again)
    if (const std::string pathIndex = g_disc.PathOf(fk.isector); !pathIndex.empty())
    {
        try
        {
            std::ifstream in(cane::paths::ioPath(cane::path(pathIndex)));
            nlohmann::json index;
            in >> index;
            const std::string dir = cane::utf8(cane::path(pathIndex).parent_path());
            for (const auto& p : index.at("parts"))
                if (p.value("kind", "") == "objects" && p.contains("file") && !p.contains("dir"))
                {
                    std::vector<uint8_t> ab;
                    std::ifstream f(cane::paths::ioPath(cane::path(dir) / cane::path(p["file"].get<std::string>())), std::ios::binary);
                    ab.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                    std::string err;
                    FParseObjects(ab.data(), ab.size(), p.value("group", -1), s_mprec[p.value("group", -1)], err);
                }
        }
        catch (const std::exception&) {}
    }
    return psw;
}
}  // namespace

int ListMapStatesCommand(const std::string& world, const std::string& out)
{
    std::map<int, std::string> mptext;
    int iwrec = -1;
    std::string error;
    std::unique_ptr<SW> psw = PswLoadStatic(world, iwrec, mptext, error);
    if (!psw) { std::fprintf(stderr, "list-map-states: %s\n", error.c_str()); return 1; }
    s_pmptext = &mptext;
    std::vector<MAPSTATEINFO> astate;
    std::vector<std::string> alines;
    FMapStatesOfWorld(psw.get(), iwrec, astate, &alines);
    s_pmptext = nullptr;
    const ojson j = JsonOfStates(psw.get(), iwrec, astate, alines);
    if (!out.empty())
    {
        std::ofstream f(cane::paths::ioPath(cane::path(out)), std::ios::binary);
        f << j.dump(1) << "\n";
        if (!f) { std::fprintf(stderr, "list-map-states: cannot write %s\n", out.c_str()); return 1; }
    }
    std::printf("map states of %s (record %d): %zu\n", j["world"].get<std::string>().c_str(), iwrec, astate.size());
    for (const MAPSTATEINFO& s : astate) std::printf("  %-14s %-8s %s: %s\n", s.id.c_str(), s.kind.c_str(), s.name.c_str(), s.trigger.c_str());
    std::printf("decided by (%zu facts):\n", alines.size());
    for (const std::string& s : alines) std::printf("  %s\n", s.c_str());
    return 0;
}

int MapStatesSurveyCommand(const std::string& out)
{
    InitClasses();
    EnsureWorldRecords();
    const std::size_t cworld = std::min<std::size_t>(AwrecWorlds().size(), 44);
    ojson aj = ojson::array();
    for (std::size_t i = 0; i < cworld; ++i)
    {
        if (i == 1) continue;   // (the front end: menus)
        std::map<int, std::string> mptext;
        int iwrec = -1;
        std::string error;
        std::unique_ptr<SW> psw = PswLoadStatic(std::to_string(i), iwrec, mptext, error);
        if (!psw) { std::printf("survey: world %zu: %s\n", i, error.c_str()); continue; }
        s_pmptext = &mptext;
        std::vector<MAPSTATEINFO> astate;
        std::vector<std::string> alines;
        FMapStatesOfWorld(psw.get(), iwrec, astate, &alines);
        s_pmptext = nullptr;
        int cTaskObj = 0, cTaskSm = 0, cShown = 0, cJobList = 0, cScript = 0, cFilter = 0, c26 = 0;
        for (const std::string& s : alines)
        {
            if (s.rfind("Task ", 0) == 0 && s.find("state machine") != std::string::npos) ++cTaskSm;
            else if (s.rfind("Task ", 0) == 0 && s.find("shown while") != std::string::npos) ++cShown;
            else if (s.rfind("Task ", 0) == 0) ++cTaskObj;
            else if (s.rfind("Job ", 0) == 0) ++cJobList;
            else if (s.rfind("Script ", 0) == 0) ++cScript;
            else if (s.find("world filters") != std::string::npos) cFilter = std::atoi(s.c_str());
            else if (s.find("option 26") != std::string::npos) c26 = std::atoi(s.c_str());
        }
        int cjob = 0;
        for (const MAPSTATEINFO& s : astate) cjob += s.kind == "job";
        const std::string name = WorldNameOf(iwrec);
        std::printf("survey: %2d %-22s states %2zu (jobs %2d) | task object lists %3d, state machine lists %2d, markers %2d, job lists %2d, "
                    "script conditions %3d, filtered objects %4d, taken out at start %3d\n",
                    iwrec, name.c_str(), astate.size(), cjob, cTaskObj, cTaskSm, cShown, cJobList, cScript, cFilter, c26);
        std::fflush(stdout);
        ojson j = JsonOfStates(psw.get(), iwrec, astate, alines);
        aj.push_back(j);
    }
    if (!out.empty())
    {
        std::ofstream f(cane::paths::ioPath(cane::path(out)), std::ios::binary);
        f << aj.dump(1) << "\n";
    }
    return 0;
}

// ---- run time
void SetMapStateRequest(const std::string& ids)
{
    s_aidRequest.clear();
    s_setForced.clear();
    std::stringstream ss(ids);
    for (std::string s; std::getline(ss, s, ',');)
    {
        if (s.empty()) continue;
        if (s.rfind("custom:", 0) == 0) s_setForced.insert(s.substr(7));
        else s_aidRequest.push_back(s);
    }
    s_strStateRun = ids;
    g_fSavesReadOnly = true;
}

void SetPresenceDump(const std::string& path) { s_pathPresence = path; }

void ApplyMapStateRequest(SW* psw)
{
    s_alive.clear();
    s_pswLive = nullptr;
    if (!psw || s_aidRequest.empty()) return;
    for (const std::string& id : s_aidRequest)
    {
        if (id == "save") continue;
        std::vector<std::pair<OID, int>> atask;
        if (!FTaskStatesOf(psw, id, atask))
        {
            std::fprintf(stderr, "map-state: no state \"%s\" in this world (start, before:<job oid>, after:<job oid>, complete, custom:<id>, save)\n", id.c_str());
            continue;
        }
        int cset = 0;
        for (const auto& [oid, n] : atask)
        {
            TASK* p = PtaskOf(PloFindSwObjectByOid(psw, oid));
            if (!p) continue;
            CRef r;
            if (p->prE0.F())
            {
                r.SetS32(n);
                FSetProgressKey(p->prE0.akey, r);
                if (p->prD8.F()) { r.SetBool(n >= 2); FSetProgressKey(p->prD8.akey, r); }
                if (p->prE8.F()) { r.SetBool(false); FSetProgressKey(p->prE8.akey, r); }
                if (p->prD0.F()) { r.SetS32(n >= 2 ? 1 : 0); FSetProgressKey(p->prD0.akey, r); }
                if (p->prC8.F()) { r.SetS32(n >= 2 ? 1 : 0); FSetProgressKey(p->prC8.akey, r); }
                ++cset;
            }
            else if (g_disc.FCustomTasks() && FSetModTaskState(oid, n)) ++cset;
        }
        // (the jobs' done flags: their last task's)
        for (LO* plo : psw->mpcidaplo[0x84])
            if (JOB* pj = PjobOf(plo); pj && pj->ptaskLast && pj->prD8.F())
            {
                int n = 0;
                for (const auto& [oid, s] : atask)
                    if (oid == pj->ptaskLast->oid) n = s;
                CRef r;
                r.SetBool(n >= 2);
                FSetProgressKey(pj->prD8.akey, r);
            }
        std::printf("map-state: %s: %d task states set\n", id.c_str(), cset);
    }
}

void ApplyCustomMapStates(SW* psw)
{
    s_alive.clear();
    s_pswLive = psw;
    if (!psw) return;
    const std::string world = WorldNameOf(g_transition.iwrecCur);
    for (const auto& [pkg, st] : g_disc.AmapState())
        if (st.world == world) s_alive.push_back({&st});
    for (const std::string& id : s_setForced)
        if (std::none_of(s_alive.begin(), s_alive.end(), [&](const LIVESTATE& l) { return l.ps->id == id; }))
            std::fprintf(stderr, "map-state: no custom state \"%s\" in %s\n", id.c_str(), world.c_str());
    EvaluateLive();
}

void ResetMapStatesLive()
{
    s_alive.clear();
    s_pswLive = nullptr;
}

void OnTaskStateMapStates(TASK* ptask)
{
    (void)ptask;
    if (!s_alive.empty()) EvaluateLive();
}

void WritePresenceIfRequested(SW* psw)
{
    if (s_pathPresence.empty() || !psw) return;
    ojson j;
    j["world"] = WorldNameOf(g_transition.iwrecCur);
    j["record"] = g_transition.iwrecCur;
    j["state"] = s_strStateRun.empty() ? std::string("save") : s_strStateRun;
    ojson at = ojson::array();
    for (TASK* p : AptaskOf(psw)) at.push_back({{"task", p->oid}, {"name", NameOfTask(p)}, {"state", p->nState}});
    j["tasks"] = at;
    ojson ao = ojson::array();
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        glm::vec3 pos(0.0f);
        if (FCidDerivedFrom(plo->Cid(), 0x30)) pos = PosAloCenter(static_cast<ALO*>(plo));
        else if (FCidDerivedFrom(plo->Cid(), 3)) pos = glm::vec3(static_cast<LOX*>(plo)->mat[3]);
        ao.push_back({{"oid", plo->oid}, {"cid", int(plo->Cid())}, {"group", plo->igrp}, {"parent", plo->paloParent ? plo->paloParent->oid : -1},
                      {"in", plo->fInWorld}, {"pos", {std::round(pos.x * 10) / 10, std::round(pos.y * 10) / 10, std::round(pos.z * 10) / 10}}});
    }
    j["objects"] = ao;
    // the state machines' states, and the poses they give (the map editor shows a state's pose: Paris's fallen sign):
    // every record of an object under a state machine's object whose local matrix is not the loaded one
    // ("order": the object's place among the objects of its oid, a clone's oid repeats)
    ojson asm_ = ojson::array();
    std::set<const LO*> setSm;
    for (SMA* p : g_apsma)
    {
        if (!p || !p->palo || !p->psm) continue;
        setSm.insert(p->palo);
        asm_.push_back({{"object", p->palo->oid}, {"sm", p->psm->oid}, {"state", OidSmaCur(p)}});
    }
    j["sm"] = asm_;
    ojson apose = ojson::array();
    std::map<int, int> mpcOid;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        const int order = mpcOid[plo->oid]++;
        if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        bool fUnder = false;
        for (const LO* pa = plo; pa && !fUnder; pa = pa->paloParent) fUnder = setSm.count(pa) != 0;
        if (!fUnder) continue;
        ALO* palo = static_cast<ALO*>(plo);
        ojson axf = ojson::array();
        for (std::size_t i = 0; i < palo->axf.size(); ++i)
        {
            const XF* pxf = palo->axf[i].get();
            if (!pxf || pxf->palo != palo) continue;
            float d = 0.0f;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) d = std::max(d, std::fabs(pxf->mat[c][r] - pxf->matLoad[c][r]));
            if (d < 1e-3f) continue;
            ojson m = ojson::array();
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) m.push_back(pxf->mat[c][r]);
            axf.push_back({{"i", i}, {"mat", m}});
        }
        if (!axf.empty()) apose.push_back({{"oid", plo->oid}, {"order", order}, {"xf", axf}});
    }
    j["poses"] = apose;
    std::ofstream f(cane::paths::ioPath(cane::path(s_pathPresence)), std::ios::binary);
    f << j.dump() << "\n";
    std::printf("presence: %zu objects of %s written to %s\n", psw->aploAll.size(), j["world"].get<std::string>().c_str(), s_pathPresence.c_str());
}

// ---- self-test (no game data: the formats and the words)
int MapStatesSelfTest()
{
    int cfail = 0;
    auto check = [&](bool f, const char* what) {
        if (!f) { std::printf("map-states selftest: FAIL %s\n", what); ++cfail; }
    };
    namespace cm = cane::mods;
    cm::Manifest m;
    m.id = "t";
    const char* szJson = R"({"map_states": [
        {"world": "Paris", "id": "fixed", "name": "Fountain fixed", "when": [{"job": 1889, "done": true}, {"task": 1761}, {"progress": "a.b", "equals": 2}],
         "add": [1, 2], "remove": [3], "sm": [{"object": 4, "state": 5}]},
        {"world": "Paris", "name": "no id"}],
      "new_worlds": [{"id": "floor", "name": "Test Floor", "index": "Worlds/Mods/floor/Test Floor.world.json", "template": "Prison Vault", "spawn": [1, 2, 3]},
                     {"id": "bad", "name": "Bad", "index": "../x.world.json"}],
      "entries": [{"world": "Paris", "at": [1, 2, 3], "to": "Test Floor"}, {"world": "Paris", "exit": 1739, "to": "Test Floor", "point": 5}, {"world": "Paris"}]})";
    m.extra = nlohmann::json::parse(szJson);
    std::vector<std::string> aerr;
    const auto as = cm::mapStates(m, &aerr);
    check(as.size() == 1 && aerr.size() == 1, "map_states parsed, the one without an id refused");
    if (!as.empty())
    {
        const cm::MapState& s = as[0];
        check(s.when.size() == 3 && s.when[0].kind == cm::StateCond::Job && s.when[0].done, "job condition");
        check(s.when[1].kind == cm::StateCond::Task && s.when[1].states == std::vector<int>({2, 3}), "task condition (default: done)");
        check(s.when[2].kind == cm::StateCond::Progress && s.when[2].path == "a.b" && s.when[2].value == 2, "progress condition");
        check(s.add == std::vector<int>({1, 2}) && s.remove == std::vector<int>({3}) && s.sm.size() == 1 && s.sm[0].sm == -1, "lists");
        // round trip through the writer
        cm::Manifest m2;
        m2.id = "t";
        cm::putMapState(m2, s);
        const auto as2 = cm::mapStates(m2);
        check(as2.size() == 1 && cm::toJson(as2[0]) == cm::toJson(s), "map state round trip");
        cm::MapState s3 = s;
        s3.name = "Renamed";
        cm::putMapState(m2, s3);
        check(cm::mapStates(m2).size() == 1 && cm::mapStates(m2)[0].name == "Renamed", "put replaces by world + id");
        check(cm::removeMapState(m2, "Paris", "fixed") && cm::mapStates(m2).empty() && !m2.extra.contains("map_states"), "remove");
        check(WhenWords(nullptr, s) == "when the job 1889 is done and the task 1761 is done or done and closed and the progress value a.b is 2", "trigger words");
    }
    aerr.clear();
    const auto aw = cm::newWorlds(m, &aerr);
    check(aw.size() == 1 && aerr.size() == 1 && aw[0].hasSpawn && aw[0].spawn[2] == 3 && aw[0].episode == 1, "new_worlds parsed, the one outside refused");
    aerr.clear();
    const auto ae = cm::entries(m, &aerr);
    check(ae.size() == 2 && aerr.size() == 1 && ae[0].exit == -1 && ae[0].radius == 150.0f && ae[1].exit == 1739 && ae[1].point == 5, "entries parsed");
    check(cm::stateIdFromName("Fountain fixed!") == "fountain-fixed", "state id from a name");
    // template parts resolved
    {
        nlohmann::json index = nlohmann::json::parse(R"({"format": "sly2-world-parts", "version": 1, "template": "T", "parts": [
            {"kind": "groups", "template": true}, {"kind": "objects", "group": 1, "template": true}, {"kind": "objects", "group": 2, "hex": "0000"}]})");
        nlohmann::json tpl = nlohmann::json::parse(R"({"parts": [{"kind": "groups", "file": "World/Groups.bin", "size": 4},
            {"kind": "objects", "group": 1, "file": "World/001 a/Objects.bin", "objects": [1]}, {"kind": "objects", "group": 2, "file": "x"}]})");
        std::string err;
        const bool f = FResolveTemplateParts(index, [&](const std::string& name, nlohmann::json& t, std::string& dir) {
            if (name != "T") return false;
            t = tpl;
            dir = "Worlds/1-T";
            return true;
        }, err);
        check(f && !index.contains("template") && index["parts"][0]["file"] == "World/Groups.bin" && index["parts"][0]["dir"] == "Worlds/1-T" &&
              index["parts"][1]["file"] == "World/001 a/Objects.bin" && !index["parts"][1].contains("objects") && index["parts"][2]["hex"] == "0000",
              "template parts resolved");
        nlohmann::json bad = nlohmann::json::parse(R"({"template": "U", "parts": [{"kind": "groups", "template": true}]})");
        check(!FResolveTemplateParts(bad, [&](const std::string&, nlohmann::json&, std::string&) { return false; }, err), "a missing template refused");
    }
    std::printf("map-states selftest: %s\n", cfail ? "FAILED" : "passed");
    return cfail ? 1 : 0;
}
