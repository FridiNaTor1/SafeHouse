// The map editor's asset browser (notes/EDITOR_PLAN.md step 5): the world's prefab-like shared groups (props,
// characters, pickups, ... placed as instances by proxies) as thumbnails rendered by the game's renderer (only the
// asset drawn, framed), with their tags (the types inside), a search, "used in map" (their instances, selectable)
// and Place (a copy of an instance in front of the camera); other levels' shared groups (read from their indexes),
// their thumbnails made by a hidden editor of that level (cached in the user folder's Editor/Assets/<world>/), and a
// model imported from there (that editor exports it as GLB; it comes in as a new object with a box collision).
namespace edimpl
{

// the scene entries drawn for a group's first instance (its roots of the same instance and everything below them)
std::vector<int> CEditor::EntriesOfGroup(int igrp) const
{
    std::vector<int> a;
    const auto it = m_mpgroupRoots.find(igrp);
    if (it == m_mpgroupRoots.end() || it->second.empty()) return a;
    // (the first instance that is placed away from the origin, else the first)
    int oidInst = it->second[0]->oidInstance;
    for (LO* r : it->second)
        if (FHasMatrix(r) && glm::length(PosWorld(r)) > 1.0f) { oidInst = r->oidInstance; break; }
    std::set<const LO*> setRoot;
    for (LO* r : it->second)
        if (r->oidInstance == oidInst) setRoot.insert(r);
    for (std::size_t k = 0; k < m_scene.aglobset.size() && k < m_apalo.size(); ++k)
    {
        const LO* p = m_apalo[k];
        while (p && p->paloParent) p = p->paloParent;
        if (p && setRoot.count(p)) a.push_back(int(k));
    }
    return a;
}

// one thumbnail: only these entries drawn, framed from above and the side, copied down into a 128 x 128 texture;
// prgba: its pixels (top-down) when asked
bool CEditor::FRenderThumb(const std::vector<int>& aentry, unsigned& tex, std::vector<uint8_t>* prgba)
{
    if (aentry.empty()) return false;
    // the entries' spheres
    // (the meshes' spheres; a few huge ones (shadow hulls, effects' bounds) left out: more than 6 times the median)
    std::vector<std::pair<glm::vec3, float>> asph;
    for (int k : aentry)
    {
        const SCENEGLOBSET& sgs = m_scene.aglobset[std::size_t(k)];
        if (!sgs.pglobset) continue;
        for (std::size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
        {
            const GLOB& g = sgs.pglobset->aglob[i];
            if (g.sRadius <= 0 || (i < sgs.afVisible.size() && !sgs.afVisible[i])) continue;
            const glm::mat4 m = i < sgs.amatGlob.size() ? sgs.amatGlob[i] : glm::mat4(1.0f);
            asph.emplace_back(glm::vec3(m * glm::vec4(g.posCenter, 1.0f)), g.sRadius);
        }
    }
    std::vector<float> ar;
    for (const auto& sp : asph) ar.push_back(sp.second);
    std::sort(ar.begin(), ar.end());
    const float rMax = ar.empty() ? 0.0f : ar[ar.size() / 2] * 6.0f;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& [c, rr] : asph)
        if (rr <= rMax || asph.size() < 3)
        {
            lo = glm::min(lo, c - glm::vec3(rr));
            hi = glm::max(hi, c + glm::vec3(rr));
        }
    if (lo.x > hi.x) return false;
    const glm::vec3 mid = (lo + hi) * 0.5f;
    const float r = std::max(20.0f, glm::length(hi - lo) * 0.5f);
    // everything else hidden for the draw (then put back)
    std::vector<std::vector<uint8_t>> aSaved(m_scene.aglobset.size());
    for (std::size_t k = 0; k < m_scene.aglobset.size(); ++k) aSaved[k].swap(m_scene.aglobset[k].afVisible);
    for (std::size_t k = 0; k < m_scene.aglobset.size(); ++k) m_scene.aglobset[k].afVisible.assign(aSaved[k].size(), 0);
    for (int k : aentry) m_scene.aglobset[std::size_t(k)].afVisible = aSaved[std::size_t(k)];
    CAMERA cam;
    cam.fovY = 35.0f;
    cam.yaw = -135.0f;
    cam.pitch = -25.0f;
    cam.pos = mid - cam.Dir() * (r * 0.75f / std::tan(glm::radians(cam.fovY) * 0.5f));
    cam.sNear = std::max(1.0f, r * 0.02f);
    RENDEROPTS opts = m_opts;
    opts.rgbaClear = 0x00302016;   // (the launcher's dark blue behind it)
    RenderScene(m_scene, cam, opts);
    for (std::size_t k = 0; k < m_scene.aglobset.size(); ++k) m_scene.aglobset[k].afVisible.swap(aSaved[k]);
    // the picture: its middle square, down to 128 x 128
    const int w = g_gl.renderWidth, h = g_gl.renderHeight, side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    if (!tex)
    {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (!m_fboThumb) glGenFramebuffers(1, &m_fboThumb);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fboThumb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLuint src = g_gl.FboScene();
    if (g_gl.fboMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        src = g_gl.fbo;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fboThumb);
    glBlitFramebuffer(x0, y0, x0 + side, y0 + side, 0, 0, 128, 128, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fboThumb);
    glViewport(0, 0, 128, 128);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (prgba)
    {
        std::vector<uint8_t> ab(128 * 128 * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        prgba->resize(ab.size());
        for (int y = 0; y < 128; ++y) std::memcpy(prgba->data() + std::size_t(y) * 512, ab.data() + std::size_t(127 - y) * 512, 512);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void CEditor::ForgetAssets()
{
    for (ASSET& a : m_aasset)
        if (a.tex) glDeleteTextures(1, &a.tex);
    m_aasset.clear();
    m_fAssetsBuilt = false;
    m_iasset = -1;
}

void CEditor::BuildAssets()
{
    ForgetAssets();
    m_fAssetsBuilt = true;
    for (const auto& [igrp, aroot] : m_mpgroupRoots)
    {
        if (igrp < 0 || igrp >= int(m_index.groups.size()) || m_index.groups[std::size_t(igrp)].level || aroot.empty()) continue;
        ASSET a;
        a.igrp = igrp;
        a.name = PrettyGroupName(m_index.groups[std::size_t(igrp)].name);
        a.group = m_index.groups[std::size_t(igrp)].name;
        std::set<int> setInst;
        for (LO* r : aroot)
        {
            a.atype.insert(TypeOfLo(r));
            if (r->oidInstance >= 0 && m_mpoidProxy.count(r->oidInstance)) setInst.insert(r->oidInstance);
        }
        a.aoidProxy.assign(setInst.begin(), setInst.end());
        a.fModel = std::any_of(aroot.begin(), aroot.end(), [](LO* r) { return FIsLoDerivedFrom(r, EDCID(0x30)) && static_cast<ALO*>(r)->pglobset; });
        if (!a.fModel) continue;   // (a group without a model to show: logic, paths)
        m_aasset.push_back(a);
    }
    std::sort(m_aasset.begin(), m_aasset.end(), [](const ASSET& a, const ASSET& b) { return a.name < b.name; });
}

void CEditor::RenderPendingThumbs()
{
    // a few a frame (the asset browser's shown tiles ask for theirs)
    int c = 0;
    for (ASSET& a : m_aasset)
    {
        if (!a.fWanted || a.fTried) continue;
        a.fTried = true;
        FRenderThumb(EntriesOfGroup(a.igrp), a.tex, nullptr);
        if (++c >= 3) break;
    }
}

// thumbs:DIR: every asset's thumbnail as DIR/<group>.png and DIR/assets.json (name, types, instances), then DIR/done
void CEditor::WriteThumbs(const std::string& dir)
{
#if EDFEAT_MODELS
    BuildAssets();
    nlohmann::json j = nlohmann::json::array();
    for (ASSET& a : m_aasset)
    {
        std::vector<uint8_t> rgba;
        unsigned tex = 0;
        nlohmann::json e = {{"group", a.group}, {"name", a.name}, {"instances", a.aoidProxy.size()}};
        nlohmann::json at = nlohmann::json::array();
        for (OBJTYPE ot : a.atype) at.push_back(InfoOfType(ot).name);
        e["types"] = at;
        if (FRenderThumb(EntriesOfGroup(a.igrp), tex, &rgba))
        {
            cane::editor::model::Image img;
            img.w = img.h = 128;
            img.rgba = rgba;
            std::string error;
            cane::editor::model::savePng(ce::pathOf(dir) / (a.group + ".png"), img, error);
            e["thumb"] = a.group + ".png";
        }
        if (tex) glDeleteTextures(1, &tex);
        j.push_back(e);
    }
    const std::string s = j.dump(1);
    ce::writeFile(ce::pathOf(dir) / "assets.json", std::vector<uint8_t>(s.begin(), s.end()));
    ce::writeFile(ce::pathOf(dir) / "done", std::vector<uint8_t>{'1'});
    std::printf("editor: thumbs: %zu assets to %s\n", m_aasset.size(), dir.c_str());
#else
    (void)dir;
#endif
}

void CEditor::PlaceAsset(const ASSET& a)
{
#if EDFEAT_STRUCT
    if (a.aoidProxy.empty()) { Status(T("this asset has no placed instance to copy (made at run time)")); return; }
    const auto it = m_mpoidProxy.find(a.aoidProxy[0]);
    if (it == m_mpoidProxy.end()) return;
    glm::vec3 p = m_cam.pos + m_cam.Dir() * 600.0f, hit;
    if (FSnapDown(p, hit)) p = hit;
    p.x = SnapMove(p.x);
    p.y = SnapMove(p.y);
    std::string error;
    if (!CopyRecord(it->second, CopyGroup(), &p, true, error)) Status(std::string(T("place")) + ": " + error);
    else Status(std::string(T("placed")) + " " + a.name);
#else
    (void)a;
    Status("place: not for " EDGAME_NAME " yet");
#endif
}

// ---- other levels

std::string CEditor::AssetsDirOf(int record) const
{
    const ce::WorldEntry* pe = m_inst.world(record);
    return ce::utf8(cane::userDirectory() / "Editor" / "Assets" / EDGAME_NAME / (pe ? pe->code : std::to_string(record)));
}

void CEditor::StartOtherLevelEditor(int record, const std::string& steps)
{
#ifdef _WIN32
    const auto exe = cane::executableDirectory() / EDGAME_EXE ".exe";
#else
    const auto exe = cane::executableDirectory() / EDGAME_EXE;
#endif
    // a scratch project (nothing of it is kept), hidden, small
    const std::string proj = ce::utf8(cane::userDirectory() / "Editor" / "Assets" / ".scratch" / std::to_string(record));
    std::vector<std::string> args{"--data-dir", ce::utf8(m_inst.dir), "--user-dir", cane::utf8(cane::userDirectory()), "--editor", std::to_string(record),
                                  "--editor-project", proj, "--editor-hidden", "--editor-size", "512x512", "--editor-do", steps};
    if (m_fPlayDry) { std::printf("editor: other level: %s\n", steps.c_str()); return; }
    if (!ce::spawnDetached(exe, args)) Status("could not start " + ce::utf8(exe));
}

void CEditor::LoadOtherLevel(int record)
{
    m_otherRecord = record;
    m_aother.clear();
    const std::string dir = AssetsDirOf(record);
    std::vector<uint8_t> ab;
    if (ce::readFile(ce::pathOf(dir) / "assets.json", ab))
    {
        const nlohmann::json j = nlohmann::json::parse(std::string(ab.begin(), ab.end()), nullptr, false);
        if (j.is_array())
            for (const auto& e : j)
            {
                OTHERASSET o;
                o.group = e.value("group", std::string());
                o.name = e.value("name", o.group);
                o.instances = e.value("instances", 0);
                if (e.contains("types") && e["types"].is_array())
                    for (const auto& t : e["types"]) o.types += (o.types.empty() ? "" : ", ") + std::string(T(t.get<std::string>().c_str()));
                o.thumb = e.contains("thumb") ? ce::utf8(ce::pathOf(dir) / e["thumb"].get<std::string>()) : std::string();
                m_aother.push_back(o);
            }
        m_fOtherPending = false;
        return;
    }
    // no thumbnails yet: the shared groups from the level's index (names only) until they are made
    const ce::WorldEntry* pe = m_inst.world(record);
    ce::WorldIndex wi;
    std::string error;
    if (pe && wi.load(m_inst, *pe, error))
        for (const ce::GroupInfo& g : wi.groups)
            if (!g.level && !g.objects.empty())
            {
                OTHERASSET o;
                o.group = g.name;
                o.name = PrettyGroupName(g.name);
                o.types = std::to_string(g.objects.size()) + " " + T("objects");
                m_aother.push_back(o);
            }
}

void CEditor::PollOtherLevel()
{
    if (m_otherRecord < 0) return;
    if (++m_pollFrame % 30 != 0) return;
    std::error_code ec;
    if (m_fOtherPending && cane::fs::exists(ce::pathOf(AssetsDirOf(m_otherRecord)) / "done", ec)) LoadOtherLevel(m_otherRecord);
#if EDFEAT_MODELS
    // a model asked for: imported once its file is there
    if (!m_importPending.empty() && cane::fs::exists(ce::pathOf(m_importPending + ".done"), ec))
    {
        const std::string path = m_importPending;
        m_importPending.clear();
        std::string error;
        const int coll = m_importCollision;
        m_importCollision = 2;
        if (!FImportModel(path, nullptr, true, error)) Status(std::string(T("import")) + ": " + error);
        else Status(std::string(T("imported from another level")) + ": " + ce::pathOf(path).stem().string());
        m_importCollision = coll;
    }
#endif
}

unsigned CEditor::TexOfFile(const std::string& path)
{
    if (path.empty()) return 0;
    if (const auto it = m_mpfiletex.find(path); it != m_mpfiletex.end()) return it->second;
    unsigned tex = 0;
#if EDFEAT_MODELS
    cane::editor::model::Image img;
    std::string error;
    if (cane::editor::model::loadImage(ce::pathOf(path), img, error))
    {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
#endif
    m_mpfiletex[path] = tex;
    return tex;
}

void CEditor::DrawAssets()
{
    if (m_aasset.empty() && !m_fAssetsBuilt) { BuildAssets(); m_fAssetsBuilt = true; }
    ImGui::SetNextItemWidth(200);
    const char* asrc[] = {T("This level"), T("Another level")};
    ImGui::Combo("##assetsrc", &m_assetSrc, asrc, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##assetfilter", T("Search assets"), m_assetFilter, sizeof(m_assetFilter));
    const std::string f = [&] { std::string s = m_assetFilter; for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }();
    auto match = [&](const std::string& a, const std::string& b) {
        std::string s = a + " " + b;
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return f.empty() || s.find(f) != std::string::npos;
    };
    const float tile = 112.0f;
    const int ccol = std::max(1, int(ImGui::GetContentRegionAvail().x / (tile + 12.0f)));
    if (m_assetSrc == 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(st::vec(st::muted), "%zu %s", m_aasset.size(), T("assets (shared groups with a model)"));
        // the chosen asset: place one, select its instances in the map
        if (m_iasset >= 0 && m_iasset < int(m_aasset.size()))
        {
            ASSET& a = m_aasset[std::size_t(m_iasset)];
            ImGui::TextColored(st::vec(st::cta), "%s", a.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Place in front of the camera"))) PlaceAsset(a);
            ImGui::SameLine();
            char label[96];
            std::snprintf(label, sizeof(label), "%s (%zu)", T("Select its instances in the map"), a.aoidProxy.size());
            if (ImGui::SmallButton(label))
            {
                const std::set<int> set(a.aoidProxy.begin(), a.aoidProxy.end());
                SelectWhere([&](LO* p) { return !p->paloParent && set.count(p->oidInstance) && FHasMatrix(p); }, false);
                FrameSelected();
            }
        }
        ImGui::BeginChild("assetgrid");
        int col = 0;
        for (std::size_t i = 0; i < m_aasset.size(); ++i)
        {
            ASSET& a = m_aasset[i];
            std::string tags;
            for (OBJTYPE ot : a.atype) tags += (tags.empty() ? "" : ", ") + std::string(T(InfoOfType(ot).name));
            if (!match(a.name, tags)) continue;
            if (col++ % ccol) ImGui::SameLine();
            ImGui::PushID(int(i));
            ImGui::BeginGroup();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::IsRectVisible(ImVec2(tile, tile))) a.fWanted = true;
            const bool fSel = m_iasset == int(i);
            if (a.tex) ImGui::Image(ImTextureID(intptr_t(a.tex)), ImVec2(tile, tile), ImVec2(0, 1), ImVec2(1, 0));
            else ImGui::Dummy(ImVec2(tile, tile));
            if (ImGui::IsItemClicked()) m_iasset = int(i);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) PlaceAsset(a);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\n%s: %zu\n%s", a.name.c_str(), tags.c_str(), T("Used in map"), a.aoidProxy.size(), T("Double-click: place one in front of the camera"));
            ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x + tile, p0.y + tile), st::u32(fSel ? st::cta : st::border), 4.0f, 0, fSel ? 2.5f : 1.0f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile);
            ImGui::TextUnformatted(a.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextColored(st::vec(st::muted), "x%zu", a.aoidProxy.size());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        const ce::WorldEntry* pe = m_otherRecord >= 0 ? m_inst.world(m_otherRecord) : nullptr;
        if (ImGui::BeginCombo("##otherlevel", pe ? pe->name.c_str() : T("Choose a level")))
        {
            for (const ce::WorldEntry& e : m_inst.worlds)
                if (e.record != m_eo.record && ImGui::Selectable((e.name + "##" + std::to_string(e.record)).c_str(), e.record == m_otherRecord)) LoadOtherLevel(e.record);
            ImGui::EndCombo();
        }
        if (m_otherRecord >= 0)
        {
            ImGui::SameLine();
            ImGui::BeginDisabled(m_fOtherPending);
            if (ImGui::Button(m_fOtherPending ? T("Making thumbnails...") : T("Make thumbnails")))
            {
                std::error_code ec;
                cane::fs::create_directories(ce::pathOf(AssetsDirOf(m_otherRecord)), ec);
                cane::fs::remove(ce::pathOf(AssetsDirOf(m_otherRecord)) / "done", ec);
                StartOtherLevelEditor(m_otherRecord, "thumbs:" + AssetsDirOf(m_otherRecord) + ";quit");
                m_fOtherPending = true;
            }
            ImGui::EndDisabled();
            EdTip(T("A hidden editor of that level draws each asset once (a minute or so); kept for next time"));
        }
        ImGui::BeginChild("othergrid");
        int col = 0;
        for (std::size_t i = 0; i < m_aother.size(); ++i)
        {
            OTHERASSET& o = m_aother[i];
            if (!match(o.name, o.types)) continue;
            if (col++ % ccol) ImGui::SameLine();
            ImGui::PushID(int(i));
            ImGui::BeginGroup();
            const unsigned tex = ImGui::IsRectVisible(ImVec2(tile, tile)) ? TexOfFile(o.thumb) : 0;
            if (tex) ImGui::Image(ImTextureID(intptr_t(tex)), ImVec2(tile, tile));
            else
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(tile, tile));
                ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + tile, p0.y + tile), st::u32(st::raised), 4.0f);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\n%s", o.name.c_str(), o.types.c_str(), T("Double-click: import its model as a new object here"));
#if EDFEAT_MODELS
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_importPending.empty()) ImportFromOtherLevel(o.group);
#endif
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile);
            ImGui::TextUnformatted(o.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (m_otherRecord >= 0 && m_aother.empty()) ImGui::TextDisabled("%s", T("(no shared groups)"));
        ImGui::EndChild();
    }
}

#if EDFEAT_MODELS
void CEditor::ImportFromOtherLevel(const std::string& group)
{
    const std::string dir = AssetsDirOf(m_otherRecord);
    std::error_code ec;
    cane::fs::create_directories(ce::pathOf(dir), ec);
    const std::string path = ce::utf8(ce::pathOf(dir) / (group + ".glb"));
    cane::fs::remove(ce::pathOf(path + ".done"), ec);
    StartOtherLevelEditor(m_otherRecord, "selectgrouproot:" + group + ";export:" + path + ";donefile:" + path + ".done;quit");
    m_importPending = path;
    Status(std::string(T("exporting from the other level")) + ": " + group + "...");
}
#endif

}  // namespace edimpl
