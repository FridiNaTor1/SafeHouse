// The map editor's readable scripts and "when it exists" in plain words (Sly 2: Sly2-Release/splicenames.h; included
// by Sly2-Release/editor_script.cpp after editor_script.inc). Self-contained: the Scripts tab calls ScriptReadable /
// FScriptToRaw (a "Names" switch), the properties panel's "When it exists" calls ExistenceFacts.
//   - the Scripts tab shows a group's scripts with names (display only: the text's own ";=" lines map them back);
//     Check / Apply turn the text back into the converter's (FSpliceRaw), compile it and save the converter's own
//     print of the bytes, so an unchanged module stays byte-identical
//   - ExistenceFacts: where the object is loaded (the record's world filters), whether it starts in the world, which
//     tasks / jobs add or remove it in which states, which scripts add / remove / place it (their event, conditions)
#include "splicenames.h"
#include "splicemap.h"
#include "task.h"
#include "convert.h"

#include <regex>
#include <sstream>

#if CANE_EDITOR_3D && EDGAME_SLY2

namespace edimpl
{

// a world record's place name (convert.h), the tasks and jobs (cid 0x85 / 0x84, task.h)
const char* SzWorldNameOfRecord(int iwrec) { return SzConvertWorldName(iwrec); }
TASK* PtaskFromLo(LO* plo) { return plo && plo->Cid() == 0x85 ? static_cast<TASK*>(plo) : nullptr; }
JOB* PjobFromLo(LO* plo) { return plo && plo->Cid() == 0x84 ? static_cast<JOB*>(plo) : nullptr; }

namespace
{
std::string Join(const std::vector<std::string>& a, const char* sep = ", ")
{
    std::string s;
    for (std::size_t i = 0; i < a.size(); ++i) s += (i ? sep : "") + a[i];
    return s;
}
// a task's states as words (grf: bits 0..3 the states, 4 / 5 the task current / not current)
std::string StatesWords(uint32_t grf)
{
    static const char* const s_asz[6] = {"locked", "available", "done", "done and closed", "being played", "not being played"};
    std::vector<std::string> a;
    for (int i = 0; i < 6; ++i)
        if (grf & (1u << i)) a.push_back(s_asz[i]);
    return a.empty() ? std::string("in no state") : Join(a, " or ");
}
const char* SzActionVerb(int optid)
{
    switch (optid)
    {
    case 39: return "adds";
    case 40: return "removes";
    case 12: return "turns on";
    case 13: return "turns off";
    case 1019: return "places an instance of";
    case 1023: return "keeps the instance of";
    case 1024: return "releases the instance of";
    default: return "acts on";
    }
}
}  // namespace

// a condition's readable text with the places by name: (<world-index>) is (5) -> the place is Printing Press
std::string PlainCondition(const std::string& cond)
{
    static const std::regex s_re1(R"(\(<world-index>\) is \(([-\d ]+)\))");
    static const std::regex s_re2(R"(\(<=> \(<world-index>\) (-?\d+)\))");
    auto places = [](const std::string& list) {
        std::stringstream ss(list);
        std::vector<std::string> a;
        int n;
        while (ss >> n)
        {
            const char* sz = SzWorldNameOfRecord(n);
            a.push_back(sz ? std::string(sz) : "world " + std::to_string(n));
        }
        return Join(a, " or ");
    };
    std::string out;
    std::string rest = cond;
    std::smatch m;
    for (const std::regex* pre : {&s_re1, &s_re2})
    {
        out.clear();
        while (std::regex_search(rest, m, *pre))
        {
            out += m.prefix().str() + T("the place is") + " " + places(m[1].str());
            rest = m.suffix().str();
        }
        rest = out + rest;
    }
    return rest;
}

struct EXISTCACHE
{
    struct ACT
    {
        int igroup;
        SPLICEOBJACT a;
    };
    std::vector<ACT> aact;
    std::map<const CFrame*, std::pair<int, int>> mpframe;   // a module's frame -> (group, module)
    SPLICEWORLDNAMES world;                                  // the names the world's scripts give (splicenames.h)
};

// the outliner's name for the object oid (the root one of that oid first)
std::string CEditor::ScriptObjectName(int oid) const
{
    if (!m_psw) return std::string();
    const LO* pbest = nullptr;
    for (const auto& up : m_psw->aploAll)
        if (up->oid == oid && (!pbest || (pbest->paloParent && !up->paloParent))) pbest = up.get();
    if (!pbest)
    {
        // (not loaded here: another place's object of this world file, by its record)
        for (const ce::GroupInfo& g : m_index.groups)
            for (const ce::ObjRecord& r : g.objects)
                if (r.oid == oid) return g.name + " " + SzClass(r.cid);
        return std::string();
    }
    std::string name = NameOfObject(pbest);
    // (a name that is only its type and oid: its group's name says more, unless that is the level's)
    const std::string soid = std::to_string(oid);
    if (name.size() > soid.size() && name.compare(name.size() - soid.size(), soid.size(), soid) == 0 && pbest->igrp >= 0 &&
        pbest->igrp < int(m_index.groups.size()) && !m_index.groups[std::size_t(pbest->igrp)].level)
    {
        std::string g = GroupNameOf(pbest);
        if (g.rfind("FK$X", 0) == 0) g = g.substr(4);
        name = g + " " + name.substr(0, name.size() - soid.size());
    }
    return name;
}

std::string CEditor::ScriptReadable(int igroup, const std::string& raw, std::string* perror)
{
    EnsureExistCache();
    SPLICENAMECTX ctx;
    ctx.pworld = m_pexist ? &m_pexist->world : nullptr;
    ctx.title = igroup >= 0 && igroup < int(m_index.groups.size()) ? "group " + std::to_string(igroup) + " " + m_index.groups[std::size_t(igroup)].name : std::string();
    ctx.pfnObject = [this](int oid) { return ScriptObjectName(oid); };
    std::string rd, err;
    if (!FSpliceReadable(raw, ctx, rd, err))
    {
        if (perror) *perror = err;
        return raw;
    }
    return rd;
}

// the text shown (readable or the converter's) -> the converter's own print of what it compiles to (an unchanged
// module the same text); anew: the names that became new symbols
bool CEditor::FScriptToRaw(const std::string& text, std::string& raw, std::string& error, std::vector<std::string>* panew)
{
    std::string conv = text;
    if (FSpliceTextReadable(text) && !FSpliceRaw(text, conv, error, panew)) return false;
    std::vector<uint8_t> ab;
    if (!FSpliceFromText(conv, ab, error)) return false;
    // (the title line kept)
    std::string title = conv.substr(0, conv.find('\n'));
    if (title.rfind("; ", 0) == 0) title = title.substr(2);
    else title.clear();
    return FSpliceToText(ab.data(), ab.size(), title, raw, error);
}

// ---- when an object exists


void CEditor::EnsureExistCache()
{
    if (!m_pexist || m_pswExist != m_psw.get())
    {
        BuildExistCache();
        m_pswExist = m_psw.get();
    }
}

void CEditor::BuildExistCache()
{
    m_pexist = std::make_shared<EXISTCACHE>();
    if (!m_psw) return;
    // the modules' frames: the tables in load order, each its group (WE +0x10 the table's id)
    if (SWSPLICE* ps = PswspliceFromSw(m_psw.get()))
        for (const auto& pt : ps->apsplct)
        {
            int igroup = -1;
            for (std::size_t g = 0; g < m_psw->awe.size(); ++g)
                if (m_psw->awe[g].x10 == pt->a) { igroup = int(g); break; }
            for (std::size_t i = 0; i < pt->asplc.size(); ++i)
                if (pt->asplc[i].pframe) m_pexist->mpframe[pt->asplc[i].pframe.get()] = {igroup, int(i)};
        }
    std::vector<std::pair<int, std::string>> atext;
    for (const ce::GroupInfo& g : m_index.groups)
    {
        if (g.scriptsPart.empty()) continue;
        std::vector<uint8_t> ab;
        if (!m_proj.readPart(g.scriptsPart, ab)) continue;
        atext.emplace_back(g.index, std::string(ab.begin(), ab.end()));
        SPLICENAMECTX ctx;
        ctx.pfnObject = [this](int oid) { return ScriptObjectName(oid); };
        SpliceCollectNames(atext.back().second, ctx, m_pexist->world);
    }
    for (const auto& [igroup, text] : atext)
    {
        const ce::GroupInfo& g = m_index.groups[std::size_t(igroup)];
        const std::vector<uint8_t> ab(text.begin(), text.end());
        SPLICENAMECTX ctx;
        ctx.pworld = &m_pexist->world;
        ctx.pfnObject = [this](int oid) { return ScriptObjectName(oid); };
        std::vector<SPLICEOBJACT> aact;
        std::string err;
        if (!FSpliceObjectActions(std::string(ab.begin(), ab.end()), ctx, aact, err)) continue;
        for (SPLICEOBJACT& a : aact) m_pexist->aact.push_back({g.index, std::move(a)});
    }
}

void CEditor::ExistenceFacts(LO* plo, std::vector<std::string>& alines)
{
    if (!plo || !m_psw) return;
    EnsureExistCache();
    LO* ptop = PloTopLevel(plo);
    if (!ptop) ptop = plo;
    // 1. where it is loaded: the record's world filters (the episode's worlds share one file)
    bool fShared = false;
    if (const auto it = m_mpobj.find(ptop); it != m_mpobj.end() && !it->second.relRecord.empty())
        if (const std::vector<uint8_t>* pab = m_proj.partRef(it->second.relRecord); pab && it->second.ibRecordFile < pab->size())
        {
            const uint8_t* pb = pab->data() + it->second.ibRecordFile;
            const int k = int8_t(pb[0]);
            if (k > 0 && it->second.ibRecordFile + 1 + std::size_t(k) * 2 <= pab->size())
            {
                std::vector<std::string> aw;
                for (int i = 0; i < k; ++i)
                {
                    int16_t f;
                    std::memcpy(&f, pb + 1 + i * 2, 2);
                    const char* sz = SzWorldNameOfRecord(f);
                    aw.push_back(sz ? std::string(sz) : "world " + std::to_string(f));
                }
                alines.push_back(std::string(T("Loaded only in")) + ": " + Join(aw) + " " + T("(the episode's other places share this world file without it)"));
            }
            else if (k < 0) { alines.push_back(T("Part of a shared group: there wherever its proxy places an instance")); fShared = true; }
            else alines.push_back(T("Loaded in every place of this episode's world"));
        }
    // 2. at the load
    int n = 0;
    if (FOptionInt(ptop, EDEOPID(26), n) && n) alines.push_back(T("Starts hidden: taken out of the world at the start (something below adds it)"));
    else if (FOptionInt(ptop, EDEOPID(25), n) && n) alines.push_back(T("Put into the world at the load"));
    // 3. the tasks' and jobs' object lists (added in the listed states, removed in the others)
    const OID aoid[2] = {plo->oid, ptop->oid};
    auto fMine = [&](OID oid) { return oid > 0 && (oid == aoid[0] || oid == aoid[1] || oid == plo->oidInstance || oid == ptop->oidInstance); };
    for (const auto& up : m_psw->aploAll)
    {
        const std::vector<TASKOBJE>* pae = nullptr;
        bool fJob = false;
        if (TASK* pt = PtaskFromLo(up.get()))
        {
            pae = &pt->aobje;
            if (pt->ploShown && (pt->ploShown == plo || pt->ploShown == ptop))
                alines.push_back(std::string(T("Shown while this task is available")) + ": " + NameOfObject(up.get()));
        }
        else if (JOB* pj = PjobFromLo(up.get())) { pae = &pj->aobje; fJob = true; }
        if (!pae) continue;
        for (const TASKOBJE& e : *pae)
            if (fMine(e.oidA))
            {
                std::string s = std::string(fJob ? T("Job") : T("Task")) + " " + NameOfObject(up.get()) + ": " + T("present while it is") + " " +
                                StatesWords(e.grf) + ", " + T("removed otherwise");
                if (e.iwrec != -1)
                    if (const char* sz = SzWorldNameOfRecord(e.iwrec)) s += std::string(" (") + T("only in") + " " + sz + ")";
                alines.push_back(s);
            }
    }
    // 4. the scripts: which modules this object runs, what scripts do to it
    std::pair<int, int> mine{-2, -2};
    if (plo->pframe)
        if (const auto it = m_pexist->mpframe.find(plo->pframe.get()); it != m_pexist->mpframe.end()) mine = it->second;
    int cscript = 0;
    for (const EXISTCACHE::ACT& x : m_pexist->aact)
    {
        const SPLICEOBJACT& a = x.a;
        const bool fSelf = a.oid == 0 && x.igroup == mine.first && a.imodule == mine.second;
        if (!fSelf && !fMine(a.oid)) continue;
        if (++cscript > 12) continue;
        std::string who;
        if (fSelf) who = T("Its own script");
        else
        {
            // the objects running that module
            std::vector<std::string> arun;
            for (const auto& up : m_psw->aploAll)
                if (up->pframe && arun.size() < 3)
                    if (const auto it = m_pexist->mpframe.find(up->pframe.get()); it != m_pexist->mpframe.end() && it->second == std::make_pair(x.igroup, a.imodule))
                        arun.push_back(NameOfObject(up.get()));
            const std::string grp = x.igroup >= 0 && x.igroup < int(m_index.groups.size()) ? m_index.groups[std::size_t(x.igroup)].name : std::string();
            who = arun.empty() ? std::string(T("The script")) + " " + grp + " " + T("module") + " " + std::to_string(a.imodule)
                               : std::string(T("The script of")) + " " + Join(arun) + " (" + grp + " " + T("module") + " " + std::to_string(a.imodule) + ")";
        }
        std::string s = who + " " + SzActionVerb(a.optid) + " " + T("it") + " ";
        if (const char* szWhat = SzSpliceEventWhat(a.event.c_str())) s += std::string(T("when")) + " " + szWhat + " (" + a.event + ")";
        else if (a.event.rfind("the ", 0) == 0) s += std::string(T("at")) + " " + a.event;
        else s += std::string(T("in its function")) + " " + a.event;
        if (!a.cond.empty()) s += std::string(", ") + T("if") + " " + PlainCondition(a.cond);
        if (std::find(alines.begin(), alines.end(), s) == alines.end()) alines.push_back(s);
    }
    if (cscript > 12) alines.push_back(std::to_string(cscript - 12) + " " + T("more script lines act on it"));
    // 5. an instance: its proxy's facts
    if (plo->oidInstance > 0 && plo->oidInstance != plo->oid)
        alines.push_back(std::string(T("An instance placed by the proxy")) + " " + ScriptObjectName(plo->oidInstance) + " (" + std::to_string(plo->oidInstance) + ")");
    (void)fShared;
}

}  // namespace edimpl

#endif
