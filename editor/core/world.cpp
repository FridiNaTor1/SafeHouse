#include "world.h"

#include <cstring>
#include <fstream>

namespace cane::editor {

std::string utf8(const fs::path& p) {
    const auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}
fs::path pathOf(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool readFile(const fs::path& p, std::vector<uint8_t>& ab) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    ab.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& p, const std::vector<uint8_t>& ab) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

bool Installation::open(const fs::path& d, std::string& error) {
    dir = d;
    worlds.clear();
    game = "sly2";
    region.clear();
    disc.clear();
    filesFormat = 0;
    std::error_code ec;
    // Sly 3: the disc image (or its folder) with Worlds/worlds.json beside it
    if (fs::is_regular_file(dir, ec)) dir = dir.parent_path();
    if (fs::is_regular_file(dir / "Worlds" / "worlds.json", ec) && !fs::is_regular_file(dir / "extraction_manifest.json", ec)) {
        try {
            nlohmann::json m;
            std::ifstream inw(dir / "Worlds" / "worlds.json");
            inw >> m;
            if (m.value("format", "") != "sly3-worlds") { error = "Worlds/worlds.json: not Sly 3's world list"; return false; }
            game = "sly3";
            filesFormat = 0;   // (the games do not check a format for Sly 3's parts)
            for (const auto& w : m.at("worlds")) {
                WorldEntry e;
                e.record = w.value("world", -1);
                e.code = e.name = w.value("name", "");
                e.index = w.value("path", "");
                // (the episode by the world's name, as the executable's table 0x2EDF10 has them: Venice v_, the Outback
                // o_, Holland h_, China c_, the pirates p_, Kaine Island m_ (also the prologue); the trainer / menu 0)
                static const std::pair<char, int> aep[] = {{'v', 1}, {'o', 2}, {'h', 3}, {'c', 4}, {'p', 5}, {'m', 6}};
                if (e.code.size() > 2 && e.code[1] == '_')
                    for (const auto& [ch, ep] : aep)
                        if (e.code[0] == ch) e.episode = ep;
                if (e.index.size() > 11 && e.index.compare(e.index.size() - 11, 11, ".world.json") == 0) worlds.push_back(e);
            }
        } catch (const std::exception& ex) {
            error = std::string("Worlds/worlds.json: ") + ex.what();
            return false;
        }
        return true;
    }
    std::ifstream in(dir / "extraction_manifest.json");
    if (!in) { error = utf8(dir) + ": no extraction_manifest.json (install Sly 2 from the launcher)"; return false; }
    try {
        nlohmann::json m;
        in >> m;
        if (m.value("game", "") != "sly2") { error = "not a Sly 2 installation"; return false; }
        filesFormat = m.value("format_version", 0);
        if (filesFormat < 3) { error = "this installation keeps its worlds whole (format " + std::to_string(filesFormat) + "): re-extract Sly 2 to edit its worlds"; return false; }
        region = m.value("region", "");
        disc = m.value("disc", "");
        for (const auto& w : m.at("worlds")) {
            WorldEntry e;
            e.record = w.value("record", -1);
            e.code = w.value("code", "");
            e.name = w.value("name", "");
            e.episode = w.value("episode", 0);
            e.kind = w.value("kind", 0);
            e.index = w.value("path", "");
            if (e.index.size() > 11 && e.index.compare(e.index.size() - 11, 11, ".world.json") == 0) worlds.push_back(e);
        }
    } catch (const std::exception& ex) {
        error = std::string("extraction_manifest.json: ") + ex.what();
        return false;
    }
    return true;
}

const WorldEntry* Installation::world(int record) const {
    for (const WorldEntry& e : worlds)
        if (e.record == record) return &e;
    return nullptr;
}

bool parseObjects(const uint8_t* pb, std::size_t cb, std::vector<ObjRecord>& arec, std::string& error, bool fSly3) {
    arec.clear();
    if (cb < 2) { error = "an objects part shorter than its count"; return false; }
    int16_t c;
    std::memcpy(&c, pb, 2);
    std::size_t ib = 2;
    for (int i = 0; i < c; ++i) {
        ObjRecord r;
        r.ib = ib;
        if (ib + 1 > cb) { error = "an object record past the part's end"; return false; }
        r.k = int8_t(pb[ib++]);
        if (fSly3) {
            if (r.k == 1) {
                if (ib + 2 > cb) { error = "an object record past the part's end"; return false; }
                ib += 2 + std::size_t(pb[ib]) * 2 + std::size_t(pb[ib + 1]) * 4;
            }
        } else if (r.k > 0 || r.k < -2) ib += std::size_t(r.k > 0 ? r.k : 0) * 2;
        if (ib + 12 > cb) { error = "an object record past the part's end"; return false; }
        int16_t cid, n;
        int32_t oid, cbBody;
        std::memcpy(&cid, pb + ib, 2);
        std::memcpy(&oid, pb + ib + 2, 4);
        std::memcpy(&n, pb + ib + 6, 2);
        std::memcpy(&cbBody, pb + ib + 8, 4);
        ib += 12;
        if (cbBody < 0 || ib + std::size_t(cbBody) > cb) { error = "an object's data past the part's end"; return false; }
        r.cid = cid;
        r.oid = oid;
        r.n = n;
        r.ibBody = ib;
        ib += std::size_t(cbBody);
        r.cb = ib - r.ib;
        arec.push_back(r);
    }
    return true;
}

bool WorldIndex::load(const Installation& inst, const WorldEntry& e, std::string& error) {
    entry = e;
    groups.clear();
    std::ifstream in((e.indexBase.empty() ? inst.dir : e.indexBase) / pathOf(e.index));
    if (!in) { error = "cannot read " + e.index; return false; }
    try {
        in >> json;
        if (json.value("format", "") != "sly2-world-parts") { error = e.index + ": not a world index"; return false; }
        relDir = e.index.substr(0, e.index.rfind('/'));
        size = json.value("size", std::size_t(0));
        fnv64 = json.value("fnv64", "");
        for (const auto& g : json.at("groups")) {
            GroupInfo gi;
            gi.index = g.value("index", -1);
            gi.name = g.value("name", "");
            if (gi.name.rfind("FK$X", 0) == 0) gi.name = gi.name.substr(4);
            gi.folder = g.value("folder", "");
            gi.level = g.value("level", false);
            if (gi.index != int(groups.size())) { error = "the groups are not in order"; return false; }
            groups.push_back(gi);
        }
        for (const auto& p : json.at("parts")) {
            const std::string kind = p.value("kind", "");
            const std::string rel = p.contains("file") ? relDir + "/" + p["file"].get<std::string>() : std::string();
            if (kind == "text") textPart = rel;
            const int g = p.value("group", -1);
            if (g < 0 || g >= int(groups.size())) continue;
            if (kind == "objects") {
                groups[g].objectsPart = rel;
                for (const auto& o : p.value("objects", nlohmann::json::array())) {
                    ObjRecord r;
                    r.oid = o.value("oid", 0);
                    r.cid = o.value("cid", 0);
                    r.ib = o.value("offset", std::size_t(0));
                    r.cb = o.value("size", std::size_t(0));
                    groups[g].objects.push_back(r);
                    std::string cls = o.value("class", "");
                    if (const auto sp = cls.find(" ("); sp != std::string::npos) cls = cls.substr(0, sp);
                    groups[g].classNames.push_back(cls);
                }
            } else if (kind == "scripts") groups[g].scriptsPart = rel;
        }
    } catch (const std::exception& ex) {
        error = e.index + ": " + ex.what();
        return false;
    }
    return true;
}

int WorldIndex::maxOid() const {
    int m = 0;
    for (const GroupInfo& g : groups)
        for (const ObjRecord& r : g.objects) m = std::max(m, r.oid);
    return m;
}

bool readTextStrings(const std::vector<uint8_t>& ab, std::vector<TextString>& a, std::string& language, std::string& error) {
    a.clear();
    try {
        const nlohmann::json j = nlohmann::json::parse(ab.begin(), ab.end(), nullptr, true, true);
        const auto& langs = j.at("languages");
        language = langs.empty() ? std::string() : langs[0].get<std::string>();
        for (const auto& s : j.at("strings")) {
            TextString t;
            t.id = s.value("id", 0);
            t.text = language.empty() ? std::string() : s.value(language, "");
            a.push_back(std::move(t));
        }
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    return true;
}

}  // namespace cane::editor
