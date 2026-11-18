// The map editor's prefabs (notes/EDITOR_PLAN.md round 5, item 4): several objects saved as one reusable piece and
// placed in any level. A prefab is a JSON file (format "safehouse-prefab"): each member's top-level record as stored
// in the level (its bytes, hex), where its records' oids are (the top one and the nested ones), where the oids of
// other members are named inside it (oid options and list options: links, paths, targets, a guard's patrol), where
// its matrix is, and the script modules its records use (their text from the group's scripts). Placing one gives every
// member and nested record a new oid of the level, points the references at the new oids, moves the members by one
// offset to the view's point, carries the scripts into the level group's scripts (renumbered, the oids in them
// replaced) and appends the records to the level group (one undo step). Prefabs live in the project's prefabs/
// folder (shared with the mod) and the user's library (Editor/Prefabs/<game>/), with a picture; the Assets tab lists
// them (this project's, the library's, the installed packages'). Sly 2 and Sly 3 (the record format is the games').

#include <regex>
#include <functional>

namespace edimpl
{

namespace
{
std::string HexOf(const uint8_t* pb, std::size_t cb)
{
    static const char* s_h = "0123456789abcdef";
    std::string s(cb * 2, '0');
    for (std::size_t i = 0; i < cb; ++i) { s[2 * i] = s_h[pb[i] >> 4]; s[2 * i + 1] = s_h[pb[i] & 15]; }
    return s;
}
bool FBytesOfHex(const std::string& s, std::vector<uint8_t>& ab)
{
    if (s.size() % 2) return false;
    ab.resize(s.size() / 2);
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t i = 0; i < ab.size(); ++i)
    {
        const int a = nib(s[2 * i]), b = nib(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        ab[i] = uint8_t(a * 16 + b);
    }
    return true;
}
std::string SafeFileName(const std::string& name)
{
    std::string s;
    for (char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? std::string("prefab") : s;
}
}  // namespace

std::string CEditor::PrefabLibraryDir() const
{
    return ce::utf8(cane::userDirectory() / "Editor" / "Prefabs" / EDGAME_NAME);
}

// the selection as a prefab: written to the project's prefabs/ and the library (path: the project's file)
bool CEditor::FSavePrefab(const std::string& name, std::string& path, std::string& error)
{
#if EDFEAT_STRUCT
    const std::vector<PLACE> apl = PlacesOfSelection();
    // the members: the top-level records (a proxy's for an instance)
    struct MEM { int iobj; std::size_t ibMat; std::string rel; std::size_t ib, cb; };
    std::vector<MEM> amem;
    std::set<int> setIobj;
    for (const PLACE& pl : apl)
    {
        int iobj = pl.fProxy ? pl.iobjrec : -1;
        if (iobj < 0)
            if (LO* ptop = PloTopLevel(pl.ploLive)) iobj = IobjOfLo(ptop);
        if (iobj < 0 || !setIobj.insert(iobj).second) continue;
        MEM m{iobj, pl.ibMat, {}, 0, 0};
        if (!FRecordRange(iobj, m.rel, m.ib, m.cb)) continue;
        amem.push_back(m);
    }
    if (amem.empty()) { error = T("nothing in the selection is stored in the world file"); return false; }
    // every oid the members' records have (top and nested): references to these are rewritten when placed
    std::set<int> setOid;
    for (const MEM& m : amem)
        for (const EDITOBJREC& r : m_rec.aobj)
        {
            std::string rel;
            std::size_t ib = 0;
            if (r.oid > 0 && FLocate(r.ibRecord, 1, rel, ib) && rel == m.rel && ib >= m.ib && ib < m.ib + m.cb) setOid.insert(r.oid);
        }
    glm::vec3 center(0.0f);
    for (const PLACE& pl : apl) center += PosWorld(pl.ploLive);
    center /= float(std::max<std::size_t>(1, apl.size()));
    nlohmann::json j;
    j["format"] = "safehouse-prefab";
    j["version"] = 1;
    j["game"] = EDGAME_NAME;
    j["name"] = name;
    j["from"] = m_index.entry.name;
    j["center"] = {center.x, center.y, center.z};
    nlohmann::json amj = nlohmann::json::array();
    int cRef = 0, cScript = 0;
    for (const MEM& m : amem)
    {
        const std::vector<uint8_t>* pab = m_proj.partRef(m.rel);
        if (!pab || m.ib + m.cb > pab->size()) continue;
        const uint8_t* pb = pab->data() + m.ib;
        nlohmann::json mj;
        const EDITOBJREC& rt = m_rec.aobj[std::size_t(m.iobj)];
        mj["oid"] = rt.oid;
        mj["cid"] = rt.cid;
        mj["name"] = rt.plo ? NameOfObject(rt.plo) : std::string(SzClass(rt.cid));
        mj["record"] = HexOf(pb, m.cb);
        // its matrix
        std::string rel;
        std::size_t ib = 0;
        mj["matrix"] = m.ibMat && FLocate(m.ibMat, 48, rel, ib) && rel == m.rel && ib >= m.ib && ib + 48 <= m.ib + m.cb ? int(ib - m.ib) : -1;
        // its records' headers (oid at), the script modules they use (n at)
        nlohmann::json ahead = nlohmann::json::array(), ascr = nlohmann::json::array();
        std::string textSrc;
        for (const ce::GroupInfo& g : m_index.groups)
            if (g.objectsPart == m.rel && !g.scriptsPart.empty())
            {
                std::vector<uint8_t> abs;
                if (m_proj.readPart(g.scriptsPart, abs)) textSrc = ScriptTextRaw(std::string(abs.begin(), abs.end()));
            }
        std::set<std::size_t> setAt;
        for (const EDITOBJREC& r : m_rec.aobj)
        {
            if (!FLocate(r.ibRecord, 1, rel, ib) || rel != m.rel || ib < m.ib || ib >= m.ib + m.cb || !setAt.insert(ib).second) continue;
            const std::size_t o = ib - m.ib;
            const std::size_t ibOid = o + IbOidOfRecord(pb + o);
            if (ibOid + 6 > m.cb) continue;
            ahead.push_back({{"at", ibOid}, {"oid", r.oid}});
            int16_t n;
            std::memcpy(&n, pb + ibOid + 4, 2);
            if (n < 0) continue;
            std::string inner;
            if (!textSrc.empty() && FModuleInner(textSrc, n, inner))
            {
                ascr.push_back({{"at", ibOid + 4}, {"module", n}, {"text", inner}});
                ++cScript;
            }
            else ascr.push_back({{"at", ibOid + 4}, {"module", n}});   // (not found: dropped when placed)
        }
        mj["records"] = ahead;
        mj["scripts"] = ascr;
        // the references to members' oids inside it: oid options and list options' 4-byte values
        nlohmann::json aref = nlohmann::json::array();
        for (std::size_t i = 0; i < m_rec.afield.size(); ++i)
        {
            const EDITFIELD& f = m_rec.afield[i];
            if (f.fk == EDITFK_Option)
            {
                const OPTTYPE t = OptType(f.k);
                if (t.cb != 4 || t.cf || !(std::string(t.name) == "object id" || FOidOption(f.eopid))) continue;
                if (!FLocate(f.ib, 4, rel, ib) || rel != m.rel || ib < m.ib || ib + 4 > m.ib + m.cb) continue;
                int32_t v;
                std::memcpy(&v, pb + (ib - m.ib), 4);
                if (setOid.count(v)) { aref.push_back({{"at", ib - m.ib}, {"oid", v}}); ++cRef; }
            }
            else if (f.fk == EDITFK_OptionArray)
            {
                std::vector<LISTEL> ael;
                std::string relL;
                if (!FListElements(int(i), relL, ael) || relL != m.rel) continue;
                for (const LISTEL& e : ael)
                {
                    if (e.cb != 4 || e.fFloat || e.ib < m.ib || e.ib + 4 > m.ib + m.cb) continue;
                    int32_t v;
                    std::memcpy(&v, pb + (e.ib - m.ib), 4);
                    if (setOid.count(v)) { aref.push_back({{"at", e.ib - m.ib}, {"oid", v}}); ++cRef; }
                }
            }
        }
        mj["refs"] = aref;
        amj.push_back(mj);
    }
    j["members"] = amj;
    const std::string file = SafeFileName(name) + ".prefab.json";
    const std::string dump = j.dump(1);
    std::error_code ec;
    const auto dirProj = m_proj.dir / "prefabs";
    const auto dirLib = ce::pathOf(PrefabLibraryDir());
    cane::fs::create_directories(dirProj, ec);
    cane::fs::create_directories(dirLib, ec);
    const std::vector<uint8_t> ab(dump.begin(), dump.end());
    if (!ce::writeFile(dirProj / file, ab)) { error = "could not write " + ce::utf8(dirProj / file); return false; }
    ce::writeFile(dirLib / file, ab);
    // its picture (the members drawn alone, framed)
    std::set<const LO*> setRoot;
    for (const PLACE& pl : apl) setRoot.insert(pl.ploLive);
    std::vector<int> aentry;
    for (std::size_t k = 0; k < m_scene.aglobset.size() && k < m_apalo.size(); ++k)
        for (const LO* p = m_apalo[k]; p; p = p->paloParent)
            if (setRoot.count(p)) { aentry.push_back(int(k)); break; }
    unsigned tex = 0;
    std::vector<uint8_t> rgba;
    if (g_gl.window && !aentry.empty() && FRenderThumb(aentry, tex, &rgba) && rgba.size() == 128 * 128 * 4)
    {
        const std::string png = ce::utf8(dirProj / (SafeFileName(name) + ".png"));
        stbi_write_png(png.c_str(), 128, 128, 4, rgba.data(), 128 * 4);
        std::error_code ec2;
        cane::fs::copy_file(ce::pathOf(png), dirLib / (SafeFileName(name) + ".png"), cane::fs::copy_options::overwrite_existing, ec2);
    }
    path = ce::utf8(dirProj / file);
    m_fPrefabsListed = false;
    Status(Fmt(T("saved prefab %s: %d objects, %d references between them, %d script modules"), name.c_str(), int(amj.size()), cRef, cScript));
    return true;
#else
    (void)name; (void)path;
    error = "not for " EDGAME_NAME " yet";
    return false;
#endif
}

// module n's text after "(module n" up to its closing parenthesis
bool CEditor::FModuleInner(const std::string& text, int n, std::string& inner) const
{
    const std::regex reOne("(^|\\n)\\(module\\s+" + std::to_string(n) + "(?=[\\s)])");
    std::smatch m;
    if (!std::regex_search(text, m, reOne)) return false;
    const std::size_t ibOpen = std::size_t(m.position(0)) + (m[1].length() ? 1 : 0);
    const std::size_t ibBody = std::size_t(m.position(0) + m.length(0));
    int depth = 0;
    bool fStr = false;
    for (std::size_t i = ibOpen; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (fStr) { if (ch == '\\') ++i; else if (ch == '"') fStr = false; continue; }
        if (ch == '"') fStr = true;
        else if (ch == ';') { while (i < text.size() && text[i] != '\n') ++i; }
        else if (ch == '(') ++depth;
        else if (ch == ')' && --depth == 0) { inner = text.substr(ibBody, i - ibBody); return true; }
    }
    return false;
}

// a prefab placed with its middle at pos (one undo step); the number of objects placed (0: error)
int CEditor::PlacePrefab(const std::string& path, const glm::vec3& pos, std::string& error)
{
#if EDFEAT_STRUCT
    std::vector<uint8_t> abFile;
    if (!ce::readFile(ce::pathOf(path), abFile)) { error = "could not read " + path; return 0; }
    const nlohmann::json j = nlohmann::json::parse(std::string(abFile.begin(), abFile.end()), nullptr, false);
    if (!j.is_object() || j.value("format", "") != "safehouse-prefab" || !j.contains("members")) { error = T("not a prefab file"); return 0; }
    if (j.value("game", "") != std::string(EDGAME_NAME)) { error = T("a prefab of another game"); return 0; }
    const int igrp = CopyGroup();
    if (igrp < 0 || igrp >= int(m_index.groups.size()) || m_index.groups[std::size_t(igrp)].objectsPart.empty()) { error = "no level group"; return 0; }
    const ce::GroupInfo& g = m_index.groups[std::size_t(igrp)];
    glm::vec3 center(0.0f);
    if (j.contains("center") && j["center"].is_array() && j["center"].size() == 3)
        center = glm::vec3(j["center"][0].get<float>(), j["center"][1].get<float>(), j["center"][2].get<float>());
    const glm::vec3 d = pos - center;
    // new oids for every record of every member (the level's free ones)
    std::map<int, int> mpoid;
    std::set<int> setTaken;
    auto oidFree = [&]() {
        int o = OidNew();
        while (setTaken.count(o)) ++o;
        setTaken.insert(o);
        return o;
    };
    for (const auto& mj : j["members"])
        for (const auto& h : mj.value("records", nlohmann::json::array()))
            if (!mpoid.count(h.value("oid", 0))) mpoid[h.value("oid", 0)] = oidFree();
    // the level group's scripts: the members' modules appended, renumbered
    std::string textDst;
    int nNext = 0;
    if (!g.scriptsPart.empty())
    {
        std::vector<uint8_t> ab;
        if (m_proj.readPart(g.scriptsPart, ab)) textDst.assign(ab.begin(), ab.end());
        static const std::regex reMod(R"((^|\n)\(module\s+(\d+))");
        for (auto it = std::sregex_iterator(textDst.begin(), textDst.end(), reMod); it != std::sregex_iterator(); ++it)
            nNext = std::max(nNext, std::atoi((*it)[2].str().c_str()) + 1);
    }
    const std::string textBefore = textDst;
    std::vector<std::vector<uint8_t>> arec;
    int cScript = 0, cDropped = 0;
    for (const auto& mj : j["members"])
    {
        std::vector<uint8_t> rec;
        if (!FBytesOfHex(mj.value("record", ""), rec) || rec.size() < 16) { error = T("a member's record is damaged"); return 0; }
        auto put32 = [&](std::size_t at, int32_t v) { if (at + 4 <= rec.size()) std::memcpy(rec.data() + at, &v, 4); };
        for (const auto& h : mj.value("records", nlohmann::json::array())) put32(h.value("at", std::size_t(0)), mpoid[h.value("oid", 0)]);
        for (const auto& r : mj.value("refs", nlohmann::json::array()))
            if (mpoid.count(r.value("oid", 0)))
            {
                put32(r.value("at", std::size_t(0)), mpoid[r.value("oid", 0)]);
                std::printf("editor: prefab reference to %d now %d (in %d)\n", r.value("oid", 0), mpoid[r.value("oid", 0)], mpoid[mj.value("oid", 0)]);
            }
        std::map<int, int> mpmodule;
        for (const auto& s : mj.value("scripts", nlohmann::json::array()))
        {
            const std::size_t at = s.value("at", std::size_t(0));
            if (at + 2 > rec.size()) continue;
            int16_t nNew = -1;
            const int n = s.value("module", -1);
            if (s.contains("text") && !g.scriptsPart.empty())
            {
                if (const auto it = mpmodule.find(n); it != mpmodule.end()) nNew = int16_t(it->second);
                else if (nNext < 32767)
                {
                    std::string inner = s["text"].get<std::string>();
                    for (const auto& [o, oNew] : mpoid) inner = std::regex_replace(inner, std::regex("\\b" + std::to_string(o) + "\\b"), std::to_string(oNew));
                    while (!textDst.empty() && (textDst.back() == '\n' || textDst.back() == ' ')) textDst.pop_back();
                    textDst += "\n\n(module " + std::to_string(nNext) + inner + ")\n";
                    mpmodule[n] = nNext;
                    nNew = int16_t(nNext++);
                    ++cScript;
                }
            }
            else ++cDropped;
            std::memcpy(rec.data() + at, &nNew, 2);
        }
        // moved by the offset
        const int ibMat = mj.value("matrix", -1);
        if (ibMat >= 0 && std::size_t(ibMat) + 48 <= rec.size())
        {
            glm::mat4 m = MatFromBytes(rec.data() + ibMat);
            m[3] += glm::vec4(d, 0.0f);
            float a[12];
            if (FBytesFromMat(m, a)) std::memcpy(rec.data() + ibMat, a, 48);
        }
        MakeOwnRecord(rec.data());
        arec.push_back(std::move(rec));
    }
    const std::string label = std::string(T("Place prefab")) + " " + j.value("name", std::string());
    m_proj.begin(label, true);
    bool fOk = true;
    if (textDst != textBefore)
    {
        std::string e2;
        int line = 0;
        if (FCheckScript(textDst, e2, line, nullptr)) fOk = m_proj.setPart(g.scriptsPart, std::vector<uint8_t>(textDst.begin(), textDst.end()));
        else
        {
            // (its scripts do not compile here: the records keep no module)
            m_noteCopy = e2;
            cDropped += cScript;
            cScript = 0;
            for (std::size_t i = 0; i < arec.size(); ++i)
                for (const auto& s : j["members"][i].value("scripts", nlohmann::json::array()))
                {
                    const int16_t nNone = -1;
                    const std::size_t at = s.value("at", std::size_t(0));
                    if (at + 2 <= arec[i].size()) std::memcpy(arec[i].data() + at, &nNone, 2);
                }
        }
    }
    int oidFirst = 0;
    for (std::size_t i = 0; i < arec.size() && fOk; ++i)
    {
        std::size_t ibNew = 0;
        fOk = m_proj.appendRecord(g.objectsPart, arec[i], &ibNew, error);
        if (fOk && !oidFirst) std::memcpy(&oidFirst, arec[i].data() + IbOidOfRecord(arec[i].data()), 4);
    }
    m_proj.end();
    if (!fOk) { if (m_proj.canUndo() && m_proj.history().back().label == label) m_proj.undo(); return 0; }
    Reload(oidFirst);
    int cPlaced = int(arec.size()), cRefused = 0;
    if (!PloOid(oidFirst))
    {
        // (refused as a whole, undone: a member this level cannot load, e.g. an instance of a shared group it does not
        // have; then one by one, each kept when the level loads with it)
        cPlaced = 0;
        if (textDst != textBefore && cScript)
        {
            m_proj.begin(label + " (" + T("scripts") + ")", true);
            m_proj.setPart(g.scriptsPart, std::vector<uint8_t>(textDst.begin(), textDst.end()));
            m_proj.end();
        }
        for (const auto& rec : arec)
        {
            int oid = 0;
            std::memcpy(&oid, rec.data() + IbOidOfRecord(rec.data()), 4);
            m_proj.begin(label + " " + std::to_string(oid), true);
            std::size_t ibNew = 0;
            const bool f = m_proj.appendRecord(g.objectsPart, rec, &ibNew, error);
            m_proj.end();
            if (!f) continue;
            Reload(oid);
            if (PloOid(oid)) ++cPlaced;
            else ++cRefused;
        }
        if (!cPlaced) { error = T("the game's loader refused the edit"); return 0; }
        error.clear();
    }
    // the placed objects selected
    std::vector<LO*> asel;
    for (const auto& [o, oNew] : mpoid)
        if (LO* p = PloOid(oNew); p && !p->paloParent) asel.push_back(p);
    m_asel = asel;
    if (!asel.empty()) m_plSel = asel.front();
    Status(Fmt(T("placed prefab %s: %d objects (%d this level cannot load left out), %d script modules carried, %d dropped"), j.value("name", std::string()).c_str(),
               cPlaced, cRefused, cScript, cDropped));
    std::printf("editor: prefab %s placed: %d objects, %d left out, oids from %d, scripts %d carried %d dropped\n", j.value("name", std::string()).c_str(), cPlaced,
                cRefused, oidFirst, cScript, cDropped);
    return cPlaced;
#else
    (void)path; (void)pos;
    error = "not for " EDGAME_NAME " yet";
    return 0;
#endif
}

// the prefabs: this project's, the library's, the installed packages' (by name: the first found wins)
void CEditor::ListPrefabs()
{
    m_aprefab.clear();
    std::set<std::string> setName;
    auto scan = [&](const cane::fs::path& dir, const char* where) {
        std::error_code ec;
        if (!cane::fs::is_directory(dir, ec)) return;
        std::vector<cane::fs::path> af;
        for (const auto& e : cane::fs::directory_iterator(dir, ec))
            if (e.path().filename().string().size() > 12 && e.path().string().rfind(".prefab.json") == e.path().string().size() - 12) af.push_back(e.path());
        std::sort(af.begin(), af.end());
        for (const auto& f : af)
        {
            std::vector<uint8_t> ab;
            if (!ce::readFile(f, ab)) continue;
            const nlohmann::json j = nlohmann::json::parse(std::string(ab.begin(), ab.end()), nullptr, false);
            if (!j.is_object() || j.value("format", "") != "safehouse-prefab" || j.value("game", "") != std::string(EDGAME_NAME)) continue;
            PREFAB p;
            p.name = j.value("name", f.stem().string());
            if (!setName.insert(p.name).second) continue;
            p.path = ce::utf8(f);
            p.from = j.value("from", "");
            p.where = where;
            p.c = j.contains("members") ? int(j["members"].size()) : 0;
            std::string png = ce::utf8(f);
            png = png.substr(0, png.size() - 12) + ".png";
            if (cane::fs::exists(ce::pathOf(png), ec)) p.thumb = png;
            m_aprefab.push_back(p);
        }
    };
    scan(m_proj.dir / "prefabs", T("this project"));
    scan(ce::pathOf(PrefabLibraryDir()), T("library"));
    std::error_code ec;
    const auto pkgs = cane::userDirectory() / "Mods" / "Packages";
    if (cane::fs::is_directory(pkgs, ec))
        for (const auto& e : cane::fs::directory_iterator(pkgs, ec)) scan(e.path() / "prefabs", T("installed mod"));
    m_fPrefabsListed = true;
}

void CEditor::DrawPrefabs(float tile, int ccol, const std::function<bool(const std::string&, const std::string&)>& match)
{
    if (!m_fPrefabsListed) ListPrefabs();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_asel.empty());
    if (ImGui::Button(T("Save the selection as a prefab..."))) ImGui::OpenPopup("##prefabsave");
    ImGui::EndDisabled();
    EdTip(T("The selected objects (their links between them, paths and scripts) as one piece you can place in any level; kept in the project (shared with the mod) and in your library"));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Refresh"))) ListPrefabs();
    if (ImGui::BeginPopup("##prefabsave"))
    {
        ImGui::SetNextItemWidth(260);
        ImGui::InputText(T("Name"), m_szPrefabName, sizeof(m_szPrefabName));
        if (ImGui::Button(T("Save")) && m_szPrefabName[0])
        {
            std::string path, error;
            if (!FSavePrefab(m_szPrefabName, path, error)) Status(error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::BeginChild("prefabgrid");
    int col = 0;
    for (std::size_t i = 0; i < m_aprefab.size(); ++i)
    {
        const PREFAB& p = m_aprefab[i];
        if (!match(p.name, p.from)) continue;
        if (col++ % ccol) ImGui::SameLine();
        ImGui::PushID(int(i));
        ImGui::BeginGroup();
        const unsigned tex = !p.thumb.empty() && ImGui::IsRectVisible(ImVec2(tile, tile)) ? TexOfFile(p.thumb) : 0;
        if (tex) ImGui::Image(ImTextureID(intptr_t(tex)), ImVec2(tile, tile));
        else
        {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(tile, tile));
            ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + tile, p0.y + tile), st::u32(st::raised), 4.0f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%d %s, %s %s (%s)\n%s", p.name.c_str(), p.c, T("objects"), T("from"), p.from.c_str(), p.where.c_str(), T("Double-click: place it in the middle of the view"));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            std::string error;
            if (!PlacePrefab(p.path, PosViewGround(), error)) Status(std::string(T("prefab")) + ": " + error);
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile);
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (m_aprefab.empty()) ImGui::TextDisabled("%s", T("(no prefabs yet: select objects and save them as a prefab)"));
    ImGui::EndChild();
}

bool CEditor::FPrefabVerb(const std::string& verb, const std::string& arg)
{
    if (verb == "prefabsave")   // prefabsave:NAME (the selection)
    {
        std::string path, error;
        if (!FSavePrefab(arg, path, error)) Status("prefabsave: " + error);
        else std::printf("editor: prefab saved %s\n", path.c_str());
        return true;
    }
    if (verb == "prefabs")
    {
        ListPrefabs();
        for (const PREFAB& p : m_aprefab) std::printf("editor: prefab %s (%d objects, from %s, %s)\n", p.name.c_str(), p.c, p.from.c_str(), p.where.c_str());
        return true;
    }
    if (verb == "prefabplace")   // prefabplace:NAME[:X,Y,Z] (default: the view's point)
    {
        const std::size_t c = arg.find(':');
        const std::string name = arg.substr(0, c);
        glm::vec3 p = PosViewGround();
        if (c != std::string::npos) std::sscanf(arg.c_str() + c + 1, "%f,%f,%f", &p.x, &p.y, &p.z);
        ListPrefabs();
        std::string error = std::string("no prefab ") + name;
        for (const PREFAB& pf : m_aprefab)
            if (pf.name == name) { error.clear(); if (!PlacePrefab(pf.path, p, error)) break; }
        if (!error.empty()) Status("prefabplace: " + error);
        return true;
    }
    return false;
}

}  // namespace edimpl
