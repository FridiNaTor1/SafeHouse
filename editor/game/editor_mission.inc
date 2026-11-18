// The map editor (editor_impl.h): jobs (MISSION 0x84), their tasks (TASK 0x85: options 1264 job, 1279 marker, 1274
// start checkpoint, 1276 player; prerequisite / next-task oid lists after the options) and job markers (instances of the
// mission_start_* groups placed by proxies whose binding names the task). A new job is made from an existing one: the
// job, its first (and last) task and the first task's marker copied with new oids, their references moved to the copies,
// the save-progress options dropped (a new save key would change every world's Progress.bin), the first task given no
// prerequisites (unsaved tasks with prerequisites come back locked) and a script module of its own that marks it done
// when it is activated (the marker reached; Sly 3: its "done at once when activated" option). Shared by Sly 2 and
// Sly 3: class and option numbers are Sly 2's, mapped by EDCID / EDEOPID (editor_game.h).
#include "editor_impl.h"

#if CANE_EDITOR_3D && EDFEAT_MISSIONS

#include <climits>
#include <set>

namespace edimpl
{

LO* CEditor::PloOid(int oid, int cid) const
{
    LO* p = nullptr;
    for (const auto& up : m_psw->aploAll)
        if (up->oid == oid && (cid < 0 || up->Cid() == cid))
        {
            if (!p || (FHasMatrix(up.get()) && !FHasMatrix(p))) p = up.get();
        }
    return p;
}

std::string CEditor::StrText(int id) const
{
    for (const ce::TextString& t : m_atext)
        if (t.id == id) return t.text;
    return {};
}

bool CEditor::FTaskLinks(const LO* plo, TASKLINKS& tl) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end() || it->second.ilinks < 0) return false;
    std::vector<uint8_t> ab;
    if (!FLocate(m_rec.afield[it->second.ilinks].ib, 2, tl.rel, tl.ib) || !m_proj.readPart(tl.rel, ab)) return false;
    std::size_t i = tl.ib;
    auto list = [&](std::vector<int>& a) {
        if (i >= ab.size()) return false;
        const int c = ab[i++];
        if (i + std::size_t(c) * 4 > ab.size()) return false;
        a.resize(std::size_t(c));
        for (int j = 0; j < c; ++j, i += 4) std::memcpy(&a[std::size_t(j)], ab.data() + i, 4);
        return true;
    };
    if (!list(tl.aoidPre) || !list(tl.aoidPost)) return false;
    tl.cb = i - tl.ib;
    return true;
}

namespace
{
std::vector<uint8_t> BytesOfLinks(const std::vector<int>& aPre, const std::vector<int>& aPost)
{
    std::vector<uint8_t> ab;
    for (const auto* pa : {&aPre, &aPost})
    {
        ab.push_back(uint8_t(pa->size()));
        for (int oid : *pa)
            for (int k = 0; k < 4; ++k) ab.push_back(uint8_t(uint32_t(oid) >> (8 * k)));
    }
    return ab;
}
}

bool CEditor::FSetTaskLinks(LO* ploTask, const std::vector<int>& aoidPre, const std::vector<int>& aoidPost, const std::string& label)
{
    TASKLINKS tl;
    if (!FTaskLinks(ploTask, tl)) { Status(T("this task's links are not in a binary part")); return false; }
    if (aoidPre.size() > 255 || aoidPost.size() > 255) { Status(T("rejected: at most 255 links each")); return false; }
    m_proj.begin(label, true);
    const bool fOk = m_proj.spliceBytes(tl.rel, tl.ib, tl.cb, BytesOfLinks(aoidPre, aoidPost), EnclosingSizeFields(tl.rel, tl.ib));
    m_proj.end();
    if (!fOk) { Status(T("could not change the links")); return false; }
    Status(label);
    Reload(ploTask->oid);
    return true;
}

// ---- record copies with their references moved

int CEditor::OidFreeSmall(int oidFrom)
{
    // a free oid below 32768 (the task lists of uses / exits / triggers hold S16 values): not used by any record
    std::set<int> set;
    for (const EDITOBJREC& r : m_rec.aobj) set.insert(r.oid);
    for (const auto& up : m_psw->aploAll) set.insert(up->oid);
    for (const ce::GroupInfo& g : m_index.groups)
        for (const ce::ObjRecord& r : g.objects) set.insert(r.oid);
    for (int oid : m_aoidIssued) set.insert(oid);
    for (int oid = std::max(1, oidFrom); oid < 32768; ++oid)
        if (!set.count(oid)) { m_aoidIssued.push_back(oid); return oid; }
    return 0;
}

bool CEditor::FBuildCopy(int iobjrec, int oidNew, const RECEDIT& re, std::vector<uint8_t>& rec, std::string& error)
{
    std::string rel;
    std::size_t ibRec = 0, cbRec = 0;
    if (!FRecordRange(iobjrec, rel, ibRec, cbRec)) { error = "the record is not in a binary part"; return false; }
    std::vector<uint8_t> ab;
    if (!m_proj.readPart(rel, ab) || ibRec + cbRec > ab.size()) { error = "cannot read " + rel; return false; }
    rec.assign(ab.begin() + std::ptrdiff_t(ibRec), ab.begin() + std::ptrdiff_t(ibRec + cbRec));
    const EDITOBJREC& r = m_rec.aobj[iobjrec];
    // nested records (at their original offsets, before the edits below move bytes): their modules dropped across groups
    RenumberCopy(rec, iobjrec, r.ibRecord, oidNew, re.fDropScripts, nullptr);
    const std::size_t ibW0 = r.ibRecord, ibW1 = r.ibRecord + cbRec;
    struct ED { std::size_t ib, cbOld; std::vector<uint8_t> ab; };
    std::vector<ED> aed;
    auto bytesOf = [](int64_t v, int cb) { std::vector<uint8_t> b(static_cast<std::size_t>(cb)); for (int k = 0; k < cb; ++k) b[std::size_t(k)] = uint8_t(uint64_t(v) >> (8 * k)); return b; };
    for (const EDITFIELD& f : m_rec.afield)
    {
        if (f.ib < ibW0 || f.ib >= ibW1) continue;
        const std::size_t ibRel = f.ib - ibW0;
        if (f.fk == EDITFK_TaskLinks && re.fLinks && f.plo == r.plo)
        {
            TASKLINKS tl;
            if (!FTaskLinks(f.plo, tl)) continue;
            aed.push_back({ibRel, tl.cb, BytesOfLinks(re.aoidPre, re.aoidPost)});
            continue;
        }
        if ((f.fk != EDITFK_Option && f.fk != EDITFK_OptionArray) || f.ibEnd <= f.ib) continue;
        if (re.setRemove.count(f.eopid) && f.plo == r.plo)
        {
            aed.push_back({ibRel - 2, (f.ibEnd - f.ib) + 2, {}});   // (its S16 eopid and its value(s))
            continue;
        }
        if (f.fk != EDITFK_Option) continue;
        const OPTTYPE t = OptType(f.k);
        if (t.cf || (t.cb != 2 && t.cb != 4) || ibRel + std::size_t(t.cb) > rec.size()) continue;
        int64_t v = 0;
        if (t.cb == 2) { int16_t s; std::memcpy(&s, rec.data() + ibRel, 2); v = s; }
        else { int32_t s; std::memcpy(&s, rec.data() + ibRel, 4); v = s; }
        if (const auto it = re.mpSet.find(f.eopid); it != re.mpSet.end() && f.plo == r.plo) aed.push_back({ibRel, std::size_t(t.cb), bytesOf(it->second, t.cb)});
        else if (const auto im = re.mpRemap.find(int(v)); im != re.mpRemap.end() && v > 0)
        {
            if (t.cb == 2 && im->second > 32767) { error = "a 16-bit reference cannot hold oid " + std::to_string(im->second); return false; }
            aed.push_back({ibRel, std::size_t(t.cb), bytesOf(im->second, t.cb)});
        }
    }
    // the method / list options (CRef arrays: U16 kind, then a value; 0x8000: U8 count and values) whose S16 / S32
    // values name a remapped object: (task, state) pairs of uses, exits, triggers, a marker's binding
    for (const EDITFIELD& f : m_rec.afield)
    {
        if (f.fk != EDITFK_OptionArray || f.ib < ibW0 || f.ib >= ibW1 || f.ibEnd <= f.ib || re.mpRemap.empty()) continue;
        if (re.setRemove.count(f.eopid) && f.plo == r.plo) continue;   // (removed above)
        std::size_t i = f.ib - ibW0;
        const std::size_t iEnd = f.ibEnd - ibW0;
        bool fStateNext = false;
        while (i + 2 <= iEnd)
        {
            uint16_t w;
            std::memcpy(&w, rec.data() + i, 2);
            i += 2;
            int c = 1;
            if (w & 0x8000) { w &= 0x7FFF; if (i >= iEnd) break; c = rec[i++]; }
            const OPTTYPE t = OptType(w);
            if (t.cb <= 0) break;   // (a kind of unknown size: the rest is left as it is)
            for (int j = 0; j < c && i + std::size_t(t.cb) <= iEnd; ++j, i += std::size_t(t.cb))
            {
                if (t.cf || (t.cb != 2 && t.cb != 4)) { fStateNext = false; continue; }
                int64_t v = 0;
                if (t.cb == 2) { int16_t s; std::memcpy(&s, rec.data() + i, 2); v = s; }
                else { int32_t s; std::memcpy(&s, rec.data() + i, 4); v = s; }
                // (a (task, state) pair naming a task the copy finishes: its state "done")
                if (fStateNext && v >= 0 && v <= 8) { aed.push_back({i, std::size_t(t.cb), bytesOf(2, t.cb)}); fStateNext = false; continue; }
                fStateNext = false;
                if (const auto im = re.mpRemap.find(int(v)); im != re.mpRemap.end() && v > 0)
                {
                    if (t.cb == 2 && im->second > 32767) { error = "a 16-bit reference cannot hold oid " + std::to_string(im->second); return false; }
                    aed.push_back({i, std::size_t(t.cb), bytesOf(im->second, t.cb)});
                    fStateNext = re.setTaskDone.count(im->second) > 0;
                }
            }
        }
    }
    // options added to the top object: before its first option (S16 eopid, then the value)
    if (!re.mpAdd.empty())
    {
        std::size_t ibFirst = SIZE_MAX;
        for (const EDITFIELD& f : m_rec.afield)
            if (f.plo == r.plo && (f.fk == EDITFK_Option || f.fk == EDITFK_OptionArray) && f.ib >= ibW0 + 2 && f.ib < ibW1) ibFirst = std::min(ibFirst, f.ib - 2 - ibW0);
        if (ibFirst == SIZE_MAX) { error = "the object has no options to add one to"; return false; }
        std::vector<uint8_t> ab;
        for (const auto& [eopid, abValue] : re.mpAdd)
        {
            const int16_t e = int16_t(eopid);
            ab.insert(ab.end(), reinterpret_cast<const uint8_t*>(&e), reinterpret_cast<const uint8_t*>(&e) + 2);
            ab.insert(ab.end(), abValue.begin(), abValue.end());
        }
        aed.push_back({ibFirst, 0, ab});
    }
    // (from the end; at one place a removal before an insertion)
    std::sort(aed.begin(), aed.end(), [](const ED& a, const ED& b) { return a.ib != b.ib ? a.ib > b.ib : a.cbOld > b.cbOld; });
    long d = 0;
    for (const ED& e : aed)
    {
        rec.erase(rec.begin() + std::ptrdiff_t(e.ib), rec.begin() + std::ptrdiff_t(e.ib + e.cbOld));
        rec.insert(rec.begin() + std::ptrdiff_t(e.ib), e.ab.begin(), e.ab.end());
        d += long(e.ab.size()) - long(e.cbOld);
    }
    // the header: its prefix, S16 cid S32 oid S16 n S32 cb (only the top record's size changes: no edit is nested)
    const std::size_t ibOid = IbOidOfRecord(rec.data());
    const int32_t oid = oidNew;
    std::memcpy(rec.data() + ibOid, &oid, 4);
    if (re.nModule != INT_MIN) { const int16_t n = int16_t(re.nModule); std::memcpy(rec.data() + ibOid + 4, &n, 2); }
    int32_t cb;
    std::memcpy(&cb, rec.data() + ibOid + 6, 4);
    cb = int32_t(cb + d);
    std::memcpy(rec.data() + ibOid + 6, &cb, 4);
    return true;
}

int CEditor::IobjOfLo(LO* plo) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end() || it->second.relRecord.empty()) return -1;
    return it->second.iobjrec;
}

bool CEditor::FAppendCopy(int iobjrecSrc, const std::vector<uint8_t>& rec, std::string& error)
{
    std::string rel;
    std::size_t ib = 0, cb = 0;
    if (!FRecordRange(iobjrecSrc, rel, ib, cb)) { error = "the record is not in a binary part"; return false; }
    return m_proj.appendRecord(rel, rec, nullptr, error);
}

#if EDGAME_SLY3
namespace
{
// Sly 3's option lists (brx.cpp LoadOptionsFromBrx: S16 eopid until one < 0; a method / list option (grf 0x400): its
// count of CRefs, each a U16 kind (0x8000: a U8 count of values) and the value(s) (RefReadOption); else the value by its
// kind (ReadOptionValue; kind 22: a U16 count of S16)). The group instance counts (EOPID 1032: group id, count) found.
std::size_t CbRef3(int k)
{
    switch (k)
    {
    case 0: return 1;
    case 1: case 5: case 6: return 4;
    case 2: case 9: return 12;
    case 7: case 14: case 15: case 16: case 17: case 19: case 20: case 21: case 23: return 2;
    case 10: return 8;
    default: return ((k & ~0x8000) >> 12) == 1 ? 4 : 0;
    }
}
struct GRPINST { std::size_t ibCount; int cbCount; int idGroup; };
bool FWalkOptions3(const std::vector<uint8_t>& ab, std::size_t& i, std::vector<GRPINST>& a)
{
    for (int n = 0; n < 100000; ++n)
    {
        if (i + 2 > ab.size()) return false;
        int16_t e;
        std::memcpy(&e, ab.data() + i, 2);
        i += 2;
        if (e < 0) return true;
        int k = 0, grf = 0;
        if (!FEditorOptionInfo(e, k, grf)) return false;
        if (grf & 0x400)
        {
            std::vector<std::pair<std::size_t, int>> aval;   // (offset, size) of each integer value
            for (int j = CEditorOptionRefs(e); j > 0; --j)
            {
                if (i + 2 > ab.size()) return false;
                uint16_t w;
                std::memcpy(&w, ab.data() + i, 2);
                i += 2;
                int c = 1;
                if (w & 0x8000) { w &= 0x7FFF; if (i >= ab.size()) return false; c = ab[i++]; }
                const std::size_t cb = CbRef3(w);
                for (int m = 0; m < c; ++m, i += cb)
                {
                    if (i + cb > ab.size()) return false;
                    if (cb == 2 || (cb == 4 && w != 1)) aval.push_back({i, int(cb)});
                }
            }
            if (e == sly3::EOPID_GROUP_INSTANCES && aval.size() >= 2)
            {
                int32_t id = 0;
                if (aval[0].second == 2) { int16_t s; std::memcpy(&s, ab.data() + aval[0].first, 2); id = s; }
                else std::memcpy(&id, ab.data() + aval[0].first, 4);
                a.push_back({aval[1].first, aval[1].second, id});
            }
            continue;
        }
        std::size_t cb = OptType(k).cb;
        if (k == 22)
        {
            if (i + 2 > ab.size()) return false;
            uint16_t c;
            std::memcpy(&c, ab.data() + i, 2);
            cb = 2 + std::size_t(c) * 2;
        }
        if (i + cb > ab.size()) return false;
        i += cb;
    }
    return false;
}
}  // namespace
#endif

bool CEditor::FBumpInstances(int igrp, int d)
{
    const std::string relGroups = m_index.relDir + "/World/Groups.bin";
    std::vector<uint8_t> ab;
    if (igrp < 0 || !m_proj.readPart(relGroups, ab)) return false;
#if EDGAME_SLY3
    // Sly 3 (sw.cpp LoadSwFromBrx): two prefix tables, then S16 count and per group 64 bytes of name, S32 x5C x60 x10 x14
    // cinst, U32 cb: the table ends the part
    const std::size_t cgrp = m_index.groups.size();
    if (std::size_t(igrp) >= cgrp || ab.size() < 2 + cgrp * 88) return false;
    const std::size_t ibTable = ab.size() - cgrp * 88;
    int16_t cStored;
    std::memcpy(&cStored, ab.data() + ibTable - 2, 2);
    if (std::size_t(cStored) != cgrp) return false;
    const std::size_t ib = ibTable + std::size_t(igrp) * 88 + 80;
    int32_t idGroup;
    std::memcpy(&idGroup, ab.data() + ibTable + std::size_t(igrp) * 88 + 72, 4);
#else
    const std::size_t ib = 2 + std::size_t(igrp) * 84 + 80;
    if (ib + 4 > ab.size()) return false;
#endif
    int32_t cinst;
    std::memcpy(&cinst, ab.data() + ib, 4);
    cinst += d;
    if (!m_proj.patch(relGroups, ib, &cinst, 4)) return false;
#if EDGAME_SLY3
    // the world's option sets (World/Options.bin, sw.cpp LoadOptionSetsFromBrx3): the world's options, the keys (U8 n1 x
    // 8 bytes; U8 n2 x (U16 c, c S16, 8 bytes); U8 n3 x U32 ids), then n3 blocks (S32 cb, options); a block that sets
    // this group's instance count (EOPID 1032) gets the same change
    const std::string relOptions = m_index.relDir + "/World/Options.bin";
    std::vector<uint8_t> ao;
    if (!m_proj.readPart(relOptions, ao)) return true;
    std::vector<GRPINST> agi;
    std::size_t i = 0;
    auto u8 = [&](int& n) { if (i >= ao.size()) return false; n = ao[i++]; return true; };
    int n1 = 0, n2 = 0, n3 = 0;
    bool fOk = FWalkOptions3(ao, i, agi) && u8(n1) && (i += std::size_t(n1) * 8) <= ao.size() && u8(n2);
    for (int j = 0; fOk && j < n2; ++j)
    {
        uint16_t c = 0;
        fOk = i + 2 <= ao.size();
        if (fOk) std::memcpy(&c, ao.data() + i, 2);
        i += 2 + std::size_t(c) * 2 + 8;
        fOk = fOk && i <= ao.size();
    }
    fOk = fOk && u8(n3) && (i += std::size_t(n3) * 4) <= ao.size();
    for (int j = 0; fOk && j < n3; ++j)
    {
        int32_t cb = 0;
        fOk = i + 4 <= ao.size();
        if (fOk) std::memcpy(&cb, ao.data() + i, 4);
        i += 4;
        const std::size_t ibEnd = i + std::size_t(cb);
        fOk = fOk && cb >= 0 && FWalkOptions3(ao, i, agi) && i == ibEnd;
    }
    if (!fOk) { std::printf("editor: World/Options.bin not read (the option sets' instance counts unchanged)\n"); return true; }
    for (const GRPINST& g : agi)
    {
        if (g.idGroup != idGroup) continue;
        int32_t n = 0;
        if (g.cbCount == 2) { int16_t s; std::memcpy(&s, ao.data() + g.ibCount, 2); n = s + d; const int16_t s2 = int16_t(n); m_proj.patch(relOptions, g.ibCount, &s2, 2); }
        else { std::memcpy(&n, ao.data() + g.ibCount, 4); n += d; m_proj.patch(relOptions, g.ibCount, &n, 4); }
    }
#endif
    return true;
}

void CEditor::BumpProxiesIn(int iobjrec, int d)
{
    // a copied record places one more instance of each shared group its proxies (itself, nested ones) place
    const EDITOBJREC& r = m_rec.aobj[iobjrec];
    const std::size_t ib1 = r.ibRecord + (r.ibBody - r.ibRecord) + r.cb;
    for (const EDITOBJREC& r2 : m_rec.aobj)
        if (r2.ibRecord >= r.ibRecord && r2.ibRecord < ib1 && FCidDerivedFrom(r2.cid, EDCID(6)) && r2.ibMatrix) FBumpInstances(IgrpOfProxy(r2.oid), d);
}

int CEditor::IgrpOfProxy(int oidProxy) const
{
    for (const auto& up : m_psw->aploAll)
        if (up->oidInstance == oidProxy && up->igrp >= 0) return up->igrp;
    return -1;
}

int CEditor::CreateJob(int oidJobSrc, std::string& error)
{
    LO* pjob = PloOid(oidJobSrc, EDCID(0x84));
    if (!pjob) { error = "no job " + std::to_string(oidJobSrc); return 0; }
    int oidFirst = 0, oidLast = 0;
    if (!FOptionInt(pjob, EDEOPID(1243), oidFirst) || !FOptionInt(pjob, EDEOPID(1244), oidLast)) { error = "the job has no first / last task options"; return 0; }
    LO* pfirst = PloOid(oidFirst, EDCID(0x85));
    LO* plast = PloOid(oidLast, EDCID(0x85));
    const int iJob = IobjOfLo(pjob), iFirst = IobjOfLo(pfirst), iLast = IobjOfLo(plast);
    if (iJob < 0 || iFirst < 0 || iLast < 0) { error = "the job's records are not top-level records"; return 0; }
    int oidMarker = 0;
    FOptionInt(pfirst, EDEOPID(1279), oidMarker);
    const auto itProxy = m_mpoidProxy.find(oidMarker);
    const bool fTwo = oidFirst != oidLast;
    const int oidJ = OidFreeSmall(1000), oidT1 = OidFreeSmall(1000), oidT2 = fTwo ? OidFreeSmall(1000) : oidT1;
    const int oidM = itProxy != m_mpoidProxy.end() ? OidNew() : 0;
    if (!oidJ || !oidT1 || !oidT2) { error = "no free oid below 32768"; return 0; }
    m_proj.begin("New job " + std::to_string(oidJ) + " from job " + std::to_string(oidJobSrc), true);
    std::string e2;
#if EDGAME_SLY3
    // Sly 3: the first task done at once when activated (its option 1405, task3.h) instead of a script
    const int nModule = -1;
    e2 = "done at once when activated";
#else
    // the first task's script: done when activated (the marker reached), in a module of its own in its group
    int igrpTask = -1;
    for (const ce::GroupInfo& g : m_index.groups)
        if (!g.objectsPart.empty() && g.objectsPart == m_mpobj.at(pfirst).relRecord) igrpTask = g.index;
    const int nModule = AppendScriptModule(igrpTask, "(begin (define sym19 (lambda (sym1002) ((<geto> sym1002 1104) 2 sym1002))))", e2);
#endif
    std::map<int, int> mp{{oidJobSrc, oidJ}, {oidFirst, oidT1}, {oidLast, oidT2}};
    // (the old job's other tasks: the new job's last, so its uses / triggers copied in later finish it)
    {
        std::set<int> seen;
        std::vector<int> aq{oidFirst};
        while (!aq.empty() && seen.size() < 64)
        {
            const int oid = aq.back();
            aq.pop_back();
            if (!oid || seen.count(oid)) continue;
            seen.insert(oid);
            if (!mp.count(oid)) mp[oid] = oidT2;
            TASKLINKS tl;
            if (oid != oidLast)
                if (LO* pt = PloOid(oid, EDCID(0x85)); pt && FTaskLinks(pt, tl))
                    for (int o : tl.aoidPost) aq.push_back(o);
        }
    }
    if (oidM) mp[oidMarker] = oidM;
    // the save progress options (Sly 3: the jobs' and tasks' progress keys)
    std::set<int> setSave;
    for (int e : {1260, 1261, 1262, 1263, 1295, 1296, 1297, 1298, 1299})
        if (EDEOPID(e) >= 0) setSave.insert(EDEOPID(e));
    std::vector<uint8_t> recJ, recT1, recT2, recM;
    RECEDIT reJ;
    reJ.mpRemap = mp;
    reJ.setRemove = setSave;
    reJ.nModule = -1;
    RECEDIT reT1;
    reT1.mpRemap = mp;
    reT1.setRemove = setSave;
    reT1.fLinks = true;
    if (fTwo) reT1.aoidPost = {oidT2};
    reT1.nModule = nModule >= 0 ? nModule : -1;
    if (!oidM) reT1.mpSet[EDEOPID(1279)] = 0;
    RECEDIT reT2;
    reT2.mpRemap = mp;
    reT2.setRemove = setSave;
    reT2.fLinks = true;
    reT2.aoidPre = {oidT1};
    reT2.nModule = -1;
    reT2.mpSet[EDEOPID(1279)] = 0;
#if EDGAME_SLY3
    // Sly 3: the copies drop the old job's script (1408) and its per-state object changes (1416 / 1417: the old job's
    // objects); the first is done at once when activated (1405: set, or added), the last waits for its goal
    for (RECEDIT* pre : {&reT1, &reT2})
        for (int e : {1408, 1416, 1417}) pre->setRemove.insert(e);
    reT2.setRemove.insert(1405);
    reT1.setRemove.insert(1405);   // (when it has one: removed, then added as 1)
    reT1.mpAdd[1405] = {1};
#endif
    RECEDIT reM;
    reM.mpRemap = mp;
    bool fOk = FBuildCopy(iJob, oidJ, reJ, recJ, error) && FBuildCopy(iFirst, oidT1, reT1, recT1, error) && (!fTwo || FBuildCopy(iLast, oidT2, reT2, recT2, error)) &&
               (!oidM || FBuildCopy(itProxy->second, oidM, reM, recM, error));
    if (fOk) fOk = FAppendCopy(iJob, recJ, error) && FAppendCopy(iFirst, recT1, error) && (!fTwo || FAppendCopy(iLast, recT2, error));
    if (fOk && oidM)
    {
        // the marker copy: one more instance of its group, 400 units to the side
        std::string rel;
        std::size_t ibNew = 0;
        std::size_t ibRec = 0, cbRec = 0;
        const EDITOBJREC& rp = m_rec.aobj[itProxy->second];
        fOk = FRecordRange(itProxy->second, rel, ibRec, cbRec) && m_proj.appendRecord(rel, recM, &ibNew, error);
        if (fOk)
        {
            BumpProxiesIn(itProxy->second, 1);
            std::vector<uint8_t> ab;
            m_proj.readPart(rel, ab);
            const std::size_t ibMat = ibNew + (rp.ibMatrix - rp.ibRecord);
            if (ibMat + 48 <= ab.size())
            {
                glm::mat4 m = MatFromBytes(ab.data() + ibMat);
                m[3] += glm::vec4(400.0f, 0.0f, 0.0f, 0.0f);
                float a[12];
                if (FBytesFromMat(m, a)) m_proj.patch(rel, ibMat, a, 48);
            }
        }
    }
    m_proj.end();
    if (!fOk)
    {
        if (m_proj.canUndo() && m_proj.history().back().label.rfind("New job", 0) == 0) m_proj.undo();
        return 0;
    }
    m_ojobActive = oidJ;
    m_mpjobRemap[oidJ] = mp;
    m_mpjobLast[oidJ] = oidT2;
    Status("new job " + std::to_string(oidJ) + ": tasks " + std::to_string(oidT1) + (fTwo ? " / " + std::to_string(oidT2) : std::string()) +
           (oidM ? ", marker " + std::to_string(oidM) : std::string(", no marker")) + (nModule >= 0 ? ", script module " + std::to_string(nModule) : std::string(" (") + (EDGAME_SLY3 ? "" : "no script: ") + e2 + ")"));
    Reload(oidM ? 0 : oidJ);
    if (oidM)
        for (const auto& up : m_psw->aploAll)
            if (!up->paloParent && up->oidInstance == oidM) { m_plSel = up.get(); break; }
    return oidJ;
}

int CEditor::CopyIntoJob(int oidSrc, int oidJob, std::string& error)
{
    const auto itMap = m_mpjobRemap.find(oidJob);
    if (itMap == m_mpjobRemap.end()) { error = "job " + std::to_string(oidJob) + " was not made in this session"; return 0; }
    // the object's own record, or the proxy that places it
    LO* plo = PloOid(oidSrc);
    int iobj = -1;
    bool fProxy = false;
    if (const auto it = m_mpoidProxy.find(oidSrc); it != m_mpoidProxy.end()) { iobj = it->second; fProxy = true; }
    else if (plo) iobj = IobjOfLo(PloTopLevel(plo));
    if (iobj < 0) { error = "object " + std::to_string(oidSrc) + " has no top-level record"; return 0; }
    const int oidNew = OidNew();
 RECEDIT re;
    re.mpRemap = itMap->second;
    re.setTaskDone = {m_mpjobLast[oidJob]};
    std::vector<uint8_t> rec;
    m_proj.begin("Copy " + std::to_string(oidSrc) + " into job " + std::to_string(oidJob), true);
    bool fOk = FBuildCopy(iobj, oidNew, re, rec, error);
    std::string rel;
    std::size_t ibRec = 0, cbRec = 0, ibNew = 0;
    if (fOk) fOk = FRecordRange(iobj, rel, ibRec, cbRec) && m_proj.appendRecord(rel, rec, &ibNew, error);
    if (fOk) BumpProxiesIn(iobj, 1);
    // 300 units to the side (its stored matrix: the proxy's, or the object's own)
    if (fOk)
    {
        std::size_t ibMatW = 0;
        if (fProxy) ibMatW = m_rec.aobj[iobj].ibMatrix;
        else if (const auto it = m_mpobj.find(m_rec.aobj[iobj].plo); it != m_mpobj.end() && it->second.imat >= 0) ibMatW = m_rec.afield[it->second.imat].ib;
        if (ibMatW > m_rec.aobj[iobj].ibRecord)
        {
            std::vector<uint8_t> ab;
            m_proj.readPart(rel, ab);
            const std::size_t ibMat = ibNew + (ibMatW - m_rec.aobj[iobj].ibRecord);
            if (ibMat + 48 <= ab.size())
            {
                glm::mat4 m = MatFromBytes(ab.data() + ibMat);
                m[3] += glm::vec4(300.0f, 0.0f, 0.0f, 0.0f);
                float a[12];
                if (FBytesFromMat(m, a)) m_proj.patch(rel, ibMat, a, 48);
            }
        }
    }
    m_proj.end();
    if (!fOk) return 0;
    Status(Fmt(T("copied %d into job %d as %d"), oidSrc, oidJob, oidNew));
    Reload(oidNew);
    if (!m_plSel)
        for (const auto& up : m_psw->aploAll)
            if (!up->paloParent && up->oidInstance == oidNew) { m_plSel = up.get(); break; }
    return oidNew;
}

// ---- the Missions tab

void CEditor::DrawMissions()
{
    std::vector<LO*> ajob;
    for (const auto& up : m_psw->aploAll)
        if (up->Cid() == EDCID(0x84)) ajob.push_back(up.get());
    std::sort(ajob.begin(), ajob.end(), [](LO* a, LO* b) { return a->oid < b->oid; });
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::muted));
    ImGui::TextWrapped(T("%zu jobs. Click a task or marker to select it; Edit links changes a task's chain."), ajob.size());
    ImGui::PopStyleColor();
    ImGui::BeginChild("jobs", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
    for (LO* pjob : ajob)
    {
        int idName = 0, oidFirst = 0, oidLast = 0;
        FOptionInt(pjob, EDEOPID(1245), idName);
        FOptionInt(pjob, EDEOPID(1243), oidFirst);
        FOptionInt(pjob, EDEOPID(1244), oidLast);
        std::string name = idName ? StrText(idName) : std::string();
        if (name.empty()) name = T("job");
        char label[200];
        std::snprintf(label, sizeof(label), "%s  %d%s##j%d", name.c_str(), pjob->oid, m_mpjobRemap.count(pjob->oid) ? (std::string("  (") + T("new") + ")").c_str() : "", pjob->oid);
        const bool fActive = pjob->oid == m_ojobActive;
        if (fActive) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        ImGui::PushStyleColor(ImGuiCol_Text, st::vec(fActive ? st::cta : st::text));
        const bool fOpen = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked()) { m_ojobActive = pjob->oid; Select(pjob); }
        if (!fOpen) continue;
        // its tasks: from the first along the next-task links (at most 40)
        std::set<int> seen;
        std::vector<int> aq{oidFirst};
        while (!aq.empty() && seen.size() < 40)
        {
            const int oid = aq.front();
            aq.erase(aq.begin());
            if (!oid || seen.count(oid)) continue;
            seen.insert(oid);
            LO* ptask = PloOid(oid, EDCID(0x85));
            if (!ptask) { ImGui::TextColored(st::vec(st::danger), T("task %d is missing"), oid); continue; }
            int oidJobOfTask = 0, oidMarker = 0, idPrompt = 0, player = 0;
            FOptionInt(ptask, EDEOPID(1264), oidJobOfTask);
            FOptionInt(ptask, EDEOPID(1279), oidMarker);
            FOptionInt(ptask, EDEOPID(1265), idPrompt);
            FOptionInt(ptask, EDEOPID(1276), player);
            TASKLINKS tl;
            const bool fLinks = FTaskLinks(ptask, tl);
            ImGui::PushID(oid);
            const char* szPlayer = player == 7 ? "Sly" : player == 8 ? "Bentley" : player == 9 ? "Murray" : "";
            char tlabel[160];
            std::snprintf(tlabel, sizeof(tlabel), "%s %d  %s%s", T("task"), oid, szPlayer, oid == oidLast ? (std::string("  (") + T("last") + ")").c_str() : "");
            if (ImGui::Selectable(tlabel, ptask == m_plSel)) Select(ptask);
            ImGui::Indent();
            if (oidJobOfTask != pjob->oid) ImGui::TextColored(st::vec(st::danger), T("belongs to job %d"), oidJobOfTask);
            if (oidMarker)
            {
                ImGui::TextColored(st::vec(st::muted), T("marker %d"), oidMarker);
                if (ImGui::IsItemClicked())
                    for (const auto& up : m_psw->aploAll)
                        if (!up->paloParent && up->oidInstance == oidMarker) { Select(up.get()); FrameSelected(); break; }
            }
            if (fLinks)
            {
                std::string pre, post;
                for (int o : tl.aoidPre) pre += (pre.empty() ? "" : ", ") + std::to_string(o);
                for (int o : tl.aoidPost) post += (post.empty() ? "" : ", ") + std::to_string(o);
                ImGui::TextColored(st::vec(st::muted), T("after: %s   next: %s"), pre.empty() ? "-" : pre.c_str(), post.empty() ? "-" : post.c_str());
                for (int o : tl.aoidPost) aq.push_back(o);
                if (ImGui::SmallButton(T("Edit links")))
                {
                    m_linkTask = oid;
                    std::snprintf(m_linkPre, sizeof(m_linkPre), "%s", pre.c_str());
                    std::snprintf(m_linkPost, sizeof(m_linkPost), "%s", post.c_str());
                }
                if (m_linkTask == oid)
                {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputTextWithHint("##pre", T("after (oids, comma separated)"), m_linkPre, sizeof(m_linkPre));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputTextWithHint("##post", T("next (oids, comma separated)"), m_linkPost, sizeof(m_linkPost));
                    auto parse = [](const char* sz, std::vector<int>& a) {
                        std::stringstream ss(sz);
                        std::string t;
                        while (std::getline(ss, t, ','))
                        {
                            t.erase(0, t.find_first_not_of(' '));
                            if (t.empty()) continue;
                            char* pend = nullptr;
                            const long v = std::strtol(t.c_str(), &pend, 10);
                            if (*pend || v <= 0 || v > INT_MAX) return false;
                            a.push_back(int(v));
                        }
                        return true;
                    };
                    if (ImGui::SmallButton(T("Apply")))
                    {
                        std::vector<int> a1, a2;
                        if (!parse(m_linkPre, a1) || !parse(m_linkPost, a2)) Status(T("rejected: the links must be oids"));
                        else { m_linkTask = 0; FSetTaskLinks(ptask, a1, a2, "Links of task " + std::to_string(oid)); ImGui::Unindent(); ImGui::PopID(); ImGui::TreePop(); ImGui::EndChild(); return; }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(T("Cancel"))) m_linkTask = 0;
                }
            }
            ImGui::Unindent();
            ImGui::PopID();
        }
        ImGui::PushID(pjob->oid);
        if (ImGui::SmallButton(T("New job from this one")))
        {
            std::string error;
            if (!CreateJob(pjob->oid, error)) Status(std::string(T("new job")) + ": " + error);
            ImGui::PopID();
            ImGui::TreePop();
            break;
        }
        if (m_mpjobRemap.count(pjob->oid) && m_plSel)
        {
            if (ImGui::SmallButton(T("Copy the selection in as its goal")))
            {
                std::string error;
                const int oidSrc = m_plSel->paloParent == nullptr && m_plSel->oidInstance >= 0 ? m_plSel->oidInstance : m_plSel->oid;
                if (!CopyIntoJob(oidSrc, pjob->oid, error)) Status(std::string(T("copy into job")) + ": " + error);
                ImGui::PopID();
                ImGui::TreePop();
                break;
            }
        }
        ImGui::PopID();
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::muted));
    if (EDFEAT_CUSTOMTASKS)
        ImGui::TextWrapped("%s", T("A new job starts at its marker; copy a use or trigger of the old job in as its goal. Its progress is "
                           "saved with the game (beside the save slot, never in it) once the project is saved."));
    else
        ImGui::TextWrapped("%s", T("A new job starts at its marker; copy a use or trigger of the old job in as its goal, or wire a button to "
                           "its last task (Links). Its progress is not saved yet: it can be played again after a reload."));
    ImGui::PopStyleColor();
}

}  // namespace edimpl

#endif
