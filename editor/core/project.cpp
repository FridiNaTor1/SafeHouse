#include "project.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

namespace cane::editor {

namespace {
bool safeRel(const std::string& rel) { return !rel.empty() && rel.find("..") == std::string::npos && rel[0] != '/'; }
}

bool Project::create(const fs::path& d, const std::string& name, const Installation& inst, std::string& error) {
    dir = d;
    installDir = inst.dir;
    manifest = {};
    manifest.dir = d;
    manifest.name = name;
    manifest.id = cane::mods::idFromName(name);
    manifest.game = inst.game;
    game = inst.game;
    if (!inst.region.empty()) manifest.regions = {inst.region};
    manifest.filesFormat = inst.filesFormat;
    m_parts.clear();
    m_undo.clear();
    m_redo.clear();
    std::error_code ec;
    fs::create_directories(dir / cane::mods::kFilesFolder, ec);
    if (ec) { error = "cannot make " + utf8(dir) + ": " + ec.message(); return false; }
    return cane::mods::writeManifest(manifest, error);
}

bool Project::open(const fs::path& d, const Installation& inst, std::string& error) {
    if (!cane::mods::readManifest(d, manifest, error)) return false;
    if (const std::string why = cane::mods::mismatch(manifest, inst.game, inst.region, inst.filesFormat); !why.empty()) {
        error = manifest.id + ": " + why;
        return false;
    }
    game = inst.game;
    dir = d;
    installDir = inst.dir;
    m_parts.clear();
    m_undo.clear();
    m_redo.clear();
    m_removed.clear();
    const fs::path files = dir / cane::mods::kFilesFolder;
    std::error_code ec;
    if (fs::is_directory(files, ec))
        for (const auto& e : fs::recursive_directory_iterator(files, ec)) {
            if (!e.is_regular_file()) continue;
            std::vector<uint8_t> ab;
            if (!readFile(e.path(), ab)) { error = "cannot read " + utf8(e.path()); return false; }
            m_parts[utf8(fs::relative(e.path(), files))] = std::move(ab);
        }
    m_dirty = false;
    std::string eh;
    if (!loadHistory(eh)) historyError = eh;
    return true;
}

bool Project::save(std::string& error) {
    const fs::path files = dir / cane::mods::kFilesFolder;
    for (const std::string& rel : m_removed)
        if (!m_parts.count(rel)) {
            std::error_code ec;
            fs::remove(files / pathOf(rel), ec);
        }
    m_removed.clear();
    for (const auto& [rel, ab] : m_parts) {
        std::vector<uint8_t> abOld;
        if (readFile(files / pathOf(rel), abOld) && abOld == ab) continue;
        if (!writeFile(files / pathOf(rel), ab)) { error = "cannot write " + rel; return false; }
    }
    Installation inst;
    std::string e2;
    if (inst.open(installDir, e2)) manifest.worlds = worldsOfParts(inst, changedParts());
    if (!cane::mods::writeManifest(manifest, error)) return false;
    if (!saveHistory(error)) return false;
    m_dirty = false;
    return true;
}

bool Project::readPart(const std::string& rel, std::vector<uint8_t>& ab) const {
    if (const auto it = m_parts.find(rel); it != m_parts.end()) {
        ab = it->second;
        return true;
    }
    return safeRel(rel) && readFile(installDir / pathOf(rel), ab);
}

const std::vector<uint8_t>* Project::partRef(const std::string& rel) const {
    if (const auto it = m_parts.find(rel); it != m_parts.end()) return &it->second;
    if (const auto it = m_installed.find(rel); it != m_installed.end()) return &it->second;
    std::vector<uint8_t> ab;
    if (!safeRel(rel) || !readFile(installDir / pathOf(rel), ab)) return nullptr;
    return &m_installed.emplace(rel, std::move(ab)).first->second;
}

std::vector<std::string> Project::changedParts() const {
    std::vector<std::string> a;
    for (const auto& [rel, ab] : m_parts) a.push_back(rel);
    return a;
}

void Project::begin(const std::string& label, bool structural) {
    if (m_depth++ == 0) m_open = Step{label, structural, {}};
    else if (structural) m_open->structural = true;
}

void Project::end() {
    if (m_depth == 0 || --m_depth > 0) return;
    if (!m_open->edits.empty()) {
        m_undo.push_back(std::move(*m_open));
        m_redo.clear();
        m_dirty = true;
    }
    m_open.reset();
}

bool Project::setPart(const std::string& rel, std::vector<uint8_t> ab) {
    if (!safeRel(rel)) return false;
    const bool own = m_depth == 0;
    if (own) begin("Edit " + rel);
    PartEdit e;
    e.rel = rel;
    if (const auto it = m_parts.find(rel); it != m_parts.end()) e.before = it->second;
    e.after = std::move(ab);
    // (several edits of one part in a step: one entry, its first "before")
    auto& edits = m_open->edits;
    const auto same = std::find_if(edits.begin(), edits.end(), [&](const PartEdit& x) { return x.rel == rel; });
    if (same != edits.end()) same->after = e.after;
    else edits.push_back(e);
    m_parts[rel] = *e.after;
    if (own) end();
    return true;
}

bool Project::patch(const std::string& rel, std::size_t ib, const void* pv, std::size_t cb) {
    std::vector<uint8_t> ab;
    if (!readPart(rel, ab) || ib + cb > ab.size()) return false;
    if (std::memcmp(ab.data() + ib, pv, cb) == 0) return true;
    std::memcpy(ab.data() + ib, pv, cb);
    return setPart(rel, std::move(ab));
}

bool Project::deleteRecord(const std::string& rel, std::size_t ibRecord, std::string& error) {
    std::vector<uint8_t> ab;
    if (!readPart(rel, ab)) { error = "cannot read " + rel; return false; }
    std::vector<ObjRecord> arec;
    if (!parseObjects(ab.data(), ab.size(), arec, error, game == "sly3")) return false;
    const auto it = std::find_if(arec.begin(), arec.end(), [&](const ObjRecord& r) { return r.ib == ibRecord; });
    if (it == arec.end()) { error = "no object record there"; return false; }
    ab.erase(ab.begin() + std::ptrdiff_t(it->ib), ab.begin() + std::ptrdiff_t(it->ib + it->cb));
    const int16_t c = int16_t(arec.size() - 1);
    std::memcpy(ab.data(), &c, 2);
    return setPart(rel, std::move(ab));
}

bool Project::duplicateRecord(const std::string& rel, std::size_t ibRecord, int oidNew, std::size_t* pibNew, std::string& error) {
    std::vector<uint8_t> ab;
    if (!readPart(rel, ab)) { error = "cannot read " + rel; return false; }
    std::vector<ObjRecord> arec;
    if (!parseObjects(ab.data(), ab.size(), arec, error, game == "sly3")) return false;
    const auto it = std::find_if(arec.begin(), arec.end(), [&](const ObjRecord& r) { return r.ib == ibRecord; });
    if (it == arec.end()) { error = "no object record there"; return false; }
    if (arec.size() >= 32767) { error = "the group is full"; return false; }
    std::vector<uint8_t> rec(ab.begin() + std::ptrdiff_t(it->ib), ab.begin() + std::ptrdiff_t(it->ib + it->cb));
    // the oid: S16 cid S32 oid S16 n S32 cb end the header (Sly 2's or Sly 3's, parseObjects)
    const std::size_t ibOid = (it->ibBody - it->ib) - 10;
    const int32_t oid = oidNew;
    std::memcpy(rec.data() + ibOid, &oid, 4);
    // appended after the last record (before the group's padding, which the loader re-aligns)
    const ObjRecord& last = arec.back();
    const std::size_t ibIns = last.ib + last.cb;
    ab.insert(ab.begin() + std::ptrdiff_t(ibIns), rec.begin(), rec.end());
    const int16_t c = int16_t(arec.size() + 1);
    std::memcpy(ab.data(), &c, 2);
    if (pibNew) *pibNew = ibIns;
    return setPart(rel, std::move(ab));
}

bool Project::spliceBytes(const std::string& rel, std::size_t ib, std::size_t cbOld, const std::vector<uint8_t>& abNew,
                          const std::vector<std::size_t>& aibSize) {
    std::vector<uint8_t> ab;
    if (!readPart(rel, ab) || ib + cbOld > ab.size()) return false;
    const long d = long(abNew.size()) - long(cbOld);
    for (std::size_t ibs : aibSize) {
        if (ibs + 4 > ib) return false;
        int32_t cb;
        std::memcpy(&cb, ab.data() + ibs, 4);
        cb = int32_t(cb + d);
        std::memcpy(ab.data() + ibs, &cb, 4);
    }
    ab.erase(ab.begin() + std::ptrdiff_t(ib), ab.begin() + std::ptrdiff_t(ib + cbOld));
    ab.insert(ab.begin() + std::ptrdiff_t(ib), abNew.begin(), abNew.end());
    return setPart(rel, std::move(ab));
}

bool Project::appendRecord(const std::string& rel, const std::vector<uint8_t>& rec, std::size_t* pibNew, std::string& error) {
    std::vector<uint8_t> ab;
    if (!readPart(rel, ab)) { error = "cannot read " + rel; return false; }
    std::vector<ObjRecord> arec;
    if (!parseObjects(ab.data(), ab.size(), arec, error, game == "sly3")) return false;
    if (arec.size() >= 32767) { error = "the group is full"; return false; }
    std::vector<ObjRecord> one;
    std::string e2;
    std::vector<uint8_t> probe(2 + rec.size());
    probe[0] = 1;
    std::memcpy(probe.data() + 2, rec.data(), rec.size());
    if (!parseObjects(probe.data(), probe.size(), one, e2, game == "sly3") || one.size() != 1 || one[0].cb != rec.size()) { error = "not one object record"; return false; }
    const std::size_t ibIns = arec.empty() ? 2 : arec.back().ib + arec.back().cb;
    ab.insert(ab.begin() + std::ptrdiff_t(ibIns), rec.begin(), rec.end());
    const int16_t c = int16_t(arec.size() + 1);
    std::memcpy(ab.data(), &c, 2);
    if (pibNew) *pibNew = ibIns;
    return setPart(rel, std::move(ab));
}

bool Project::revertPart(const std::string& rel) {
    if (!m_parts.count(rel)) return false;
    const bool own = m_depth == 0;
    if (own) begin("Revert " + rel);
    PartEdit e;
    e.rel = rel;
    e.before = m_parts[rel];
    m_open->edits.push_back(e);
    m_parts.erase(rel);
    m_removed.push_back(rel);
    if (own) end();
    return true;
}

void Project::apply(const PartEdit& e, bool forward) {
    const auto& v = forward ? e.after : e.before;
    if (v) m_parts[e.rel] = *v;
    else {
        m_parts.erase(e.rel);
        m_removed.push_back(e.rel);
    }
}

const Step* Project::undo() {
    if (m_undo.empty()) return nullptr;
    m_redo.push_back(std::move(m_undo.back()));
    m_undo.pop_back();
    const Step& s = m_redo.back();
    for (auto it = s.edits.rbegin(); it != s.edits.rend(); ++it) apply(*it, false);
    m_dirty = true;
    return &s;
}

const Step* Project::redo() {
    if (m_redo.empty()) return nullptr;
    m_undo.push_back(std::move(m_redo.back()));
    m_redo.pop_back();
    const Step& s = m_undo.back();
    for (const PartEdit& e : s.edits) apply(e, true);
    m_dirty = true;
    return &s;
}

// ---- the history file: "SHEH" U32 version 1, U32 undo count, U32 redo count, then the steps (undo oldest first,
// then redo oldest-undone last): str label, U8 structural, U32 edits, per edit: str rel, U8 flags (1 before is the
// installed part, 2 after is), U32 ib, the before range and the after range (U32 size + bytes each), U32 the part's
// size after, U32 before. A range is the bytes between the parts' common prefix and suffix.
namespace {
void putU32(std::vector<uint8_t>& o, uint32_t u) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(u >> (8 * i))); }
void putStr(std::vector<uint8_t>& o, const std::string& s) { putU32(o, uint32_t(s.size())); o.insert(o.end(), s.begin(), s.end()); }
struct RD {
    const std::vector<uint8_t>& a;
    std::size_t i = 0;
    bool ok = true;
    uint32_t u32() { if (i + 4 > a.size()) { ok = false; return 0; } uint32_t u = 0; for (int k = 0; k < 4; ++k) u |= uint32_t(a[i + k]) << (8 * k); i += 4; return u; }
    uint8_t u8() { if (i >= a.size()) { ok = false; return 0; } return a[i++]; }
    std::vector<uint8_t> bytes() { const uint32_t n = u32(); if (!ok || i + n > a.size()) { ok = false; return {}; } std::vector<uint8_t> v(a.begin() + std::ptrdiff_t(i), a.begin() + std::ptrdiff_t(i + n)); i += n; return v; }
    std::string str() { const auto v = bytes(); return std::string(v.begin(), v.end()); }
};
}

bool Project::saveHistory(std::string& error) const {
    std::vector<uint8_t> o{'S', 'H', 'E', 'H'};
    putU32(o, 1);
    putU32(o, uint32_t(m_undo.size()));
    putU32(o, uint32_t(m_redo.size()));
    auto putStep = [&](const Step& s) {
        putStr(o, s.label);
        o.push_back(s.structural ? 1 : 0);
        putU32(o, uint32_t(s.edits.size()));
        for (const PartEdit& e : s.edits) {
            putStr(o, e.rel);
            std::vector<uint8_t> inst;
            const bool needInst = !e.before || !e.after;
            if (needInst) readFile(installDir / pathOf(e.rel), inst);
            const std::vector<uint8_t>& b = e.before ? *e.before : inst;
            const std::vector<uint8_t>& a = e.after ? *e.after : inst;
            std::size_t p = 0;
            while (p < b.size() && p < a.size() && b[p] == a[p]) ++p;
            std::size_t q = 0;
            while (q < b.size() - p && q < a.size() - p && b[b.size() - 1 - q] == a[a.size() - 1 - q]) ++q;
            o.push_back(uint8_t((e.before ? 0 : 1) | (e.after ? 0 : 2)));
            putU32(o, uint32_t(p));
            putU32(o, uint32_t(b.size() - p - q));
            o.insert(o.end(), b.begin() + std::ptrdiff_t(p), b.end() - std::ptrdiff_t(q));
            putU32(o, uint32_t(a.size() - p - q));
            o.insert(o.end(), a.begin() + std::ptrdiff_t(p), a.end() - std::ptrdiff_t(q));
            putU32(o, uint32_t(a.size()));
            putU32(o, uint32_t(b.size()));
        }
    };
    for (const Step& s : m_undo) putStep(s);
    for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it) putStep(*it);   // (the next redo first)
    const fs::path p = dir / "editor" / "history.bin";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (!writeFile(p, o)) { error = "cannot write " + utf8(p); return false; }
    return true;
}

bool Project::loadHistory(std::string& error) {
    std::vector<uint8_t> a;
    if (!readFile(dir / "editor" / "history.bin", a)) return true;   // (none yet)
    RD r{a};
    if (a.size() < 16 || std::memcmp(a.data(), "SHEH", 4) != 0) { error = "history.bin: not a history file"; return false; }
    r.i = 4;
    if (r.u32() != 1) { error = "history.bin: unknown version"; return false; }
    const uint32_t cu = r.u32(), cr = r.u32();
    struct RawEdit { std::string rel; uint8_t fl; uint32_t ib; std::vector<uint8_t> b, a; uint32_t cbA, cbB; };
    struct RawStep { std::string label; bool structural; std::vector<RawEdit> e; };
    std::vector<RawStep> as;
    for (uint32_t i = 0; i < cu + cr && r.ok; ++i) {
        RawStep s;
        s.label = r.str();
        s.structural = r.u8() != 0;
        const uint32_t ce = r.u32();
        for (uint32_t j = 0; j < ce && r.ok; ++j) {
            RawEdit e;
            e.rel = r.str();
            e.fl = r.u8();
            e.ib = r.u32();
            e.b = r.bytes();
            e.a = r.bytes();
            e.cbA = r.u32();
            e.cbB = r.u32();
            s.e.push_back(std::move(e));
        }
        as.push_back(std::move(s));
    }
    if (!r.ok) { error = "history.bin: truncated"; return false; }
    // rebuild the full before / after bytes: from the parts now, back through the undo steps (newest first), then
    // forward through the redo steps
    std::map<std::string, std::vector<uint8_t>> cur;
    auto part = [&](const std::string& rel) -> std::vector<uint8_t>& {
        auto it = cur.find(rel);
        if (it == cur.end()) {
            std::vector<uint8_t> v;
            readPart(rel, v);
            it = cur.emplace(rel, std::move(v)).first;
        }
        return it->second;
    };
    std::vector<Step> undo(cu), redo;
    for (int i = int(cu) - 1; i >= 0; --i) {
        const RawStep& s = as[std::size_t(i)];
        Step st{s.label, s.structural, {}};
        st.edits.resize(s.e.size());
        for (int j = int(s.e.size()) - 1; j >= 0; --j) {
            const RawEdit& e = s.e[std::size_t(j)];
            std::vector<uint8_t>& v = part(e.rel);
            if (v.size() != e.cbA || e.ib + e.a.size() > v.size() || !std::equal(e.a.begin(), e.a.end(), v.begin() + std::ptrdiff_t(e.ib))) {
                error = "history.bin does not match the project's parts (kept without its history)";
                return false;
            }
            PartEdit pe;
            pe.rel = e.rel;
            if (!(e.fl & 2)) pe.after = v;
            v.erase(v.begin() + std::ptrdiff_t(e.ib), v.begin() + std::ptrdiff_t(e.ib + e.a.size()));
            v.insert(v.begin() + std::ptrdiff_t(e.ib), e.b.begin(), e.b.end());
            if (!(e.fl & 1)) pe.before = v;
            st.edits[std::size_t(j)] = std::move(pe);
        }
        undo[std::size_t(i)] = std::move(st);
    }
    // forward again to the present, then on through the redo steps
    cur.clear();
    for (uint32_t i = cu; i < cu + cr; ++i) {
        const RawStep& s = as[i];
        Step st{s.label, s.structural, {}};
        for (const RawEdit& e : s.e) {
            std::vector<uint8_t>& v = part(e.rel);
            if (v.size() != e.cbB || e.ib + e.b.size() > v.size()) { error = "history.bin: the redo steps do not match"; return false; }
            PartEdit pe;
            pe.rel = e.rel;
            if (!(e.fl & 1)) pe.before = v;
            v.erase(v.begin() + std::ptrdiff_t(e.ib), v.begin() + std::ptrdiff_t(e.ib + e.b.size()));
            v.insert(v.begin() + std::ptrdiff_t(e.ib), e.a.begin(), e.a.end());
            if (!(e.fl & 2)) pe.after = v;
            st.edits.push_back(std::move(pe));
        }
        redo.push_back(std::move(st));
    }
    // (m_redo is a stack: the next redo at its back)
    std::reverse(redo.begin(), redo.end());
    m_undo = std::move(undo);
    m_redo = std::move(redo);
    return true;
}

bool Project::exportFolder(const fs::path& dest, std::string& error) const {
    const fs::path out = dest / pathOf(manifest.id);
    std::error_code ec;
    fs::remove_all(out, ec);
    cane::mods::Manifest m = manifest;
    m.dir = out;
    m.extra.erase("editor");
    if (!cane::mods::writeManifest(m, error)) return false;
    for (const auto& [rel, ab] : m_parts)
        if (!writeFile(out / cane::mods::kFilesFolder / pathOf(rel), ab)) { error = "cannot write " + utf8(out / pathOf(rel)); return false; }
    return true;
}

bool Project::exportZip(const fs::path& zip, std::string& error) const {
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    const fs::path tmp = zip.string() + ".tmp";
    if (!mz_zip_writer_init_file(&z, utf8(tmp).c_str(), 0)) { error = "cannot write " + utf8(zip); return false; }
    bool ok = true;
    cane::mods::Manifest m = manifest;
    const std::string mj = cane::mods::manifestToJson(m).dump(1) + "\n";
    ok = ok && mz_zip_writer_add_mem(&z, cane::mods::kManifestName, mj.data(), mj.size(), MZ_DEFAULT_COMPRESSION);
    for (const auto& [rel, ab] : m_parts) {
        const std::string name = std::string(cane::mods::kFilesFolder) + "/" + rel;
        ok = ok && mz_zip_writer_add_mem(&z, name.c_str(), ab.data(), ab.size(), MZ_DEFAULT_COMPRESSION);
    }
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    mz_zip_writer_end(&z);
    std::error_code ec;
    if (!ok) { fs::remove(tmp, ec); error = "cannot write " + utf8(zip); return false; }
    fs::rename(tmp, zip, ec);
    if (ec) { error = ec.message(); return false; }
    return true;
}

bool Project::install(const fs::path& modsDir, std::string& error) const {
    return exportFolder(modsDir / cane::mods::kPackagesFolder, error);
}

nlohmann::json worldsOfParts(const Installation& inst, const std::vector<std::string>& parts) {
    nlohmann::json a = nlohmann::json::array();
    for (const WorldEntry& w : inst.worlds) {
        const std::string dir = w.index.substr(0, w.index.rfind('/') + 1) + "World/";
        const bool used = std::any_of(parts.begin(), parts.end(), [&](const std::string& r) { return r.rfind(dir, 0) == 0; });
        if (!used) continue;
        WorldIndex wi;
        std::string error;
        a.push_back({{"path", w.index}, {"record", w.record}, {"fnv64", wi.load(inst, w, error) ? wi.fnv64 : std::string()}});
    }
    return a;
}

}  // namespace cane::editor
