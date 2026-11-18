// The map editor's lighting and sky (notes/EDITOR_PLAN.md round 5, item 5; Sly 2): the Lighting tab. The level's
// lights (class 0x04: kind, colour (HSV, option 152), strength (153), falloff near / far (155)) listed with their
// colour picker, strength and radius, moved to the view, added (a copy of one of the level's, else another level's
// light) and removed; the world's camera options (the world file's camera list, sw.cpp aoptCm: fog near / far / colour /
// most (166 / 167 / 168 / 169), far clip (165), field of view (161)) edited in place or added, with a live fog preview
// in the view; the skies (class 0x61) and the level's lighting kind (the world lights or the baked approximation).
// Every edit is a byte patch of the level's parts (one undo step); the lights re-light the view at once.

namespace edimpl
{

namespace
{
glm::vec3 RgbOfHsv(glm::vec3 hsv)
{
    // (the light's HSV as stored: hue in degrees, saturation and value 0..1)
    const float h = std::fmod(std::fmod(hsv.x, 360.0f) + 360.0f, 360.0f) / 60.0f, s = std::clamp(hsv.y, 0.0f, 1.0f), v = std::max(0.0f, hsv.z);
    const int i = int(h);
    const float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6)
    {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}
glm::vec3 HsvOfRgb(glm::vec3 c)
{
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f)
    {
        if (mx == c.r) h = 60.0f * std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = 60.0f * ((c.b - c.r) / d + 2.0f);
        else h = 60.0f * ((c.r - c.g) / d + 4.0f);
    }
    if (h < 0) h += 360.0f;
    return {h, mx > 0 ? d / mx : 0.0f, mx};
}
}  // namespace

#if EDGAME_SLY2
// a world camera option's field (the recorder's aworldopt; null: the level has none)
const EDITFIELD* CEditor::PworldOpt(int iList, int eopid) const
{
    for (const EDITFIELD& f : m_rec.aworldopt)
        if (f.i == iList && f.eopid == eopid) return &f;
    return nullptr;
}

bool CEditor::FWorldOptFloat(int eopid, float& g, int iList, int iFloat) const
{
    const EDITFIELD* pf = PworldOpt(iList, eopid);
    std::string rel;
    std::size_t ib = 0;
    std::vector<uint8_t> ab;
    if (!pf || !FLocate(pf->ib + 4 * std::size_t(iFloat), 4, rel, ib) || !m_proj.readPart(rel, ab) || ib + 4 > ab.size()) return false;
    std::memcpy(&g, ab.data() + ib, 4);
    return true;
}

// a camera option set: patched in place, or added at the list's start (S16 eopid, the value); the world loaded again
bool CEditor::FSetWorldOpt(int eopid, const void* pv, std::size_t cb, std::string& error, int iList)
{
    const EDITFIELD* pf = PworldOpt(iList, eopid);
    std::string rel;
    std::size_t ib = 0;
    const std::string label = std::string(T("World option")) + " " + StrOption(eopid);
    if (pf)
    {
        if (!FLocate(pf->ib, cb, rel, ib)) { error = T("this option is not in a binary part"); return false; }
        m_proj.begin(label, false);
        const bool f = m_proj.patch(rel, ib, pv, cb);
        m_proj.end();
        if (!f) { error = "could not write"; return false; }
        // (the loaded world's copy too: the lighting and the fog read it)
        for (OPT& o : iList ? m_psw->aoptCm : m_psw->aoptSw)
            if (o.eopid == eopid && !o.aval.empty())
            {
                if (OptType(pf->k).cf) std::memcpy(o.aval[0].ag.data(), pv, std::min<std::size_t>(cb, sizeof(float) * o.aval[0].ag.size()));
                else std::memcpy(&o.aval[0].n, pv, std::min<std::size_t>(cb, 4));
            }
        ReloadWorldOpts();
        RelightView();
        return true;
    }
    if (!m_rec.aibWorldOptList[iList] || !FLocate(m_rec.aibWorldOptList[iList], 2, rel, ib)) { error = T("the world's camera options are not in a binary part"); return false; }
    std::vector<uint8_t> ab(2);
    const int16_t e = int16_t(eopid);
    std::memcpy(ab.data(), &e, 2);
    ab.insert(ab.end(), static_cast<const uint8_t*>(pv), static_cast<const uint8_t*>(pv) + cb);
    m_proj.begin(label, true);
    const bool f = m_proj.spliceBytes(rel, ib, 0, ab, EnclosingSizeFields(rel, ib));
    m_proj.end();
    if (!f) { error = "could not write"; return false; }
    Reload(m_plSel ? m_plSel->oid : 0);
    return true;
}

// the world's fog read back into the view's preview (the parts' values: no reload needed)
void CEditor::ReloadWorldOpts()
{
    float g = 0;
    m_fogNear = FWorldOptFloat(166, g) ? g : 500.0f;
    m_fogFar = FWorldOptFloat(167, g) ? g : 20000.0f;
    m_fogMax = FWorldOptFloat(169, g) ? g : 0.5f;
    m_rgbaFog = 0x80303030u;
    if (const EDITFIELD* pf = PworldOpt(1, 168))
    {
        std::string rel;
        std::size_t ib = 0;
        std::vector<uint8_t> ab;
        if (FLocate(pf->ib, 4, rel, ib) && m_proj.readPart(rel, ab) && ib + 4 <= ab.size()) std::memcpy(&m_rgbaFog, ab.data() + ib, 4);
    }
    ApplyFogPreview();
}

void CEditor::ApplyFogPreview()
{
    m_opts.fFog = m_fFogPreview && m_fogNear > 0 && m_fogFar > m_fogNear;
    m_opts.sFogNear = m_fogNear;
    m_opts.sFogFar = m_fogFar;
    m_opts.uFogMax = m_fogMax;
    m_opts.rgbaFog = m_rgbaFog;
}
#endif

// a light's colour / strength / falloff (false: not stored as an option of it)
bool CEditor::FLightValues(LO* plo, glm::vec3& hsv, float& strength, glm::vec2& falloff) const
{
    bool f = false;
    for (int i = 0; i < 3; ++i) hsv[i] = 0;
    strength = 0;
    falloff = glm::vec2(0);
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        if (o.eopid == EDEOPID(152)) { hsv = glm::vec3(o.aval[0].ag[0], o.aval[0].ag[1], o.aval[0].ag[2]); f = true; }
        else if (o.eopid == EDEOPID(153)) strength = o.aval[0].ag[0];
        else if (o.eopid == EDEOPID(155)) falloff = glm::vec2(o.aval[0].ag[0], o.aval[0].ag[1]);
    }
    return f;
}

void CEditor::DrawLightingTab()
{
#if EDGAME_SLY2
    if (!m_fWorldOptsRead) { ReloadWorldOpts(); m_fWorldOptsRead = true; }
    ImGui::TextColored(st::vec(st::muted), "%s", T("The level's lights, its fog and its skies. Changes re-light the view at once; the play test shows them in the game."));
    // ---- fog and the camera's world options (the world file's camera list: 166 / 167 / 168 / 169, 165)
    ImGui::SeparatorText(T("Fog and view distance"));
    if (ImGui::Checkbox(T("Preview the fog in the view"), &m_fFogPreview)) ApplyFogPreview();
    auto floatOpt = [&](int eopid, const char* label, float vDefault, float vMax) {
        float g = vDefault;
        const bool fHave = FWorldOptFloat(eopid, g);
        ImGui::PushID(eopid);
        ImGui::SetNextItemWidth(160);
        float g2 = g;
        ImGui::DragFloat(label, &g2, vMax / 2000.0f, 0.0f, vMax, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit() || (!ImGui::IsItemActive() && g2 != g && ImGui::IsItemEdited()))
        {
            std::string error;
            if (!FSetWorldOpt(eopid, &g2, 4, error)) Status(error);
        }
        if (!fHave) { ImGui::SameLine(); ImGui::TextDisabled("%s", T("(the game's default; a change adds it)")); }
        ImGui::PopID();
    };
    floatOpt(166, T("fog starts (distance)"), 500.0f, 100000.0f);
    floatOpt(167, T("fog full (distance)"), 20000.0f, 200000.0f);
    floatOpt(169, T("fog at most (0..1)"), 0.5f, 1.0f);
    floatOpt(165, T("far clip (distance)"), 30000.0f, 400000.0f);
    {
        // FOGCOL: the GS register's R, G, B bytes (the top byte kept)
        float c[4] = {float(m_rgbaFog & 0xFF) / 255.0f, float((m_rgbaFog >> 8) & 0xFF) / 255.0f, float((m_rgbaFog >> 16) & 0xFF) / 255.0f, 1.0f};
        ImGui::SetNextItemWidth(160);
        if (ImGui::ColorEdit3(T("fog colour"), c, ImGuiColorEditFlags_NoInputs) || ImGui::IsItemDeactivatedAfterEdit())
        {
            auto b = [](float x) { return uint32_t(std::clamp(int(std::lround(x * 255.0f)), 0, 255)); };
            const uint32_t u = b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (m_rgbaFog & 0xFF000000u);
            if (u != m_rgbaFog && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                std::string error;
                if (!FSetWorldOpt(168, &u, 4, error)) Status(error);
            }
            else { m_rgbaFog = u; ApplyFogPreview(); }
        }
    }
    // ---- the world shader: the colour every lit model gets on top of its lights (lightsw.cpp: the SW's options 914 HSV,
    // 916 scale, 915 alpha)
    ImGui::SeparatorText(T("Ambient colour (the world shader)"));
    {
        float h = 180.0f, sat = 255.0f, val = 255.0f, scale = 90.0f;
        const bool fHave = FWorldOptFloat(914, h, 0, 0);
        FWorldOptFloat(914, sat, 0, 1);
        FWorldOptFloat(914, val, 0, 2);
        FWorldOptFloat(916, scale, 0, 0);
        const glm::vec3 rgb = RgbOfHsv(glm::vec3(h, sat / 255.0f, val / 255.0f));
        float c[3] = {rgb.r, rgb.g, rgb.b};
        ImGui::ColorEdit3(T("ambient colour"), c, ImGuiColorEditFlags_NoInputs);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            glm::vec3 h2 = HsvOfRgb(glm::vec3(c[0], c[1], c[2]));
            h2.y *= 255.0f;
            h2.z *= 255.0f;
            std::string error;
            if (!FSetWorldOpt(914, &h2.x, 12, error, 0)) Status(error);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        float s2 = scale;
        ImGui::DragFloat(T("ambient strength"), &s2, 1.0f, 0.0f, 255.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            std::string error;
            if (!FSetWorldOpt(916, &s2, 4, error, 0)) Status(error);
        }
        if (!fHave) { ImGui::SameLine(); ImGui::TextDisabled("%s", T("(the game's default; a change adds it)")); }
    }
    // ---- the lights
    std::vector<LO*> alight;
    for (const auto& up : m_psw->aploAll)
        if (up->Cid() == EDCID(0x04) && m_mpobj.count(up.get())) alight.push_back(up.get());
    char head[96];
    std::snprintf(head, sizeof(head), "%s (%zu)", T("Lights"), alight.size());
    ImGui::SeparatorText(head);
    if (ImGui::Button(T("Add a light at the view"))) AddLight(PosViewGround() + glm::vec3(0, 0, 300.0f));
    EdTip(T("A copy of one of the level's lights (another level's when it has none), 300 above the ground in the middle of the view"));
    ImGui::BeginChild("lights", ImVec2(0, 0));
    int n = 0;
    for (LO* pl : alight)
    {
        if (++n > 400) break;
        ImGui::PushID(pl);
        glm::vec3 hsv;
        float strength;
        glm::vec2 falloff;
        const bool fColour = FLightValues(pl, hsv, strength, falloff);
        if (ImGui::Selectable(NameOfObject(pl).c_str(), m_plSel == pl, 0, ImVec2(170, 0))) Select(pl);
        ImGui::SameLine();
        if (fColour)
        {
            // (HSV as the game stores it: hue in degrees, saturation and value 0..255: lightsw.cpp UserRgbFromUserHsv)
            const glm::vec3 rgb = RgbOfHsv(glm::vec3(hsv.x, hsv.y / 255.0f, std::min(hsv.z, 255.0f) / 255.0f));
            float c[3] = {rgb.r, rgb.g, rgb.b};
            ImGui::ColorEdit3("##c", c, ImGuiColorEditFlags_NoInputs);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                glm::vec3 h2 = HsvOfRgb(glm::vec3(c[0], c[1], c[2]));
                h2.y *= 255.0f;
                h2.z *= 255.0f;
                SetLightOption(pl, 152, &h2.x, 12);
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(90);
        float s2 = strength;
        ImGui::DragFloat(T("strength"), &s2, 1.0f, 0.0f, 255.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit()) SetLightOption(pl, 153, &s2, 4);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        float f2[2] = {falloff.x, falloff.y};
        ImGui::DragFloat2(T("radius (near, far)"), f2, 4.0f, 0.0f, 100000.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit()) SetLightOption(pl, 155, f2, 8);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("At the view"))) MoveLightTo(pl, PosViewGround() + glm::vec3(0, 0, 300.0f));
        ImGui::PopID();
    }
    ImGui::EndChild();
    // ---- skies
    std::vector<LO*> asky;
    for (const auto& up : m_psw->aploAll)
        if (up->Cid() == EDCID(0x61)) asky.push_back(up.get());
    if (!asky.empty())
    {
        ImGui::SeparatorText(T("Skies"));
        for (LO* ps : asky)
        {
            ImGui::PushID(ps);
            if (ImGui::SmallButton(NameOfObject(ps).c_str())) Select(ps);
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::NewLine();
    }
#else
    ImGui::TextDisabled("%s", "Lighting: not for " EDGAME_NAME " yet");
#endif
}

#if EDGAME_SLY2
// a light's option: patched when it has it, else added; the view re-lit
void CEditor::SetLightOption(LO* pl, int eopid, const void* pv, std::size_t cb)
{
    const int ifield = IfieldOption(pl, eopid);
    const std::string label = std::string(T("Light")) + " " + std::to_string(pl->oid) + ": " + StrOption(eopid);
    if (ifield >= 0) FSetOption(ifield, pv, cb, label);
    else
    {
        std::vector<uint8_t> ab(static_cast<const uint8_t*>(pv), static_cast<const uint8_t*>(pv) + cb);
        FAddOption(pl, eopid, ab, label);
    }
    RelightView();
}

void CEditor::RelightView()
{
    ResetWorldLights();
    UpdateWorldLightsFromSw(m_psw.get());
    if (FHaveWorldLights()) ApplyLighting(m_scene, WorldLights());
    else ApproximateBakeLighting(m_scene);
    BuildSceneGpu(m_scene);
}

void CEditor::MoveLightTo(LO* pl, const glm::vec3& pos)
{
    PLACE p;
    if (!FPlace(pl, p)) { Status(T("this object's transform is not stored in the world file (made at run time)")); return; }
    SetPlaceWorld(p, glm::translate(glm::mat4(1.0f), pos) * glm::mat4(glm::mat3(MatPlace(p))));
    FRecordPlace(p, std::string(T("Light")) + " " + std::to_string(pl->oid));
    RelightView();
}

int CEditor::AddLight(const glm::vec3& pos)
{
    std::string error, from;
    int oid = 0;
    const int iobj = IobjExampleOf(EDCID(0x04));
    if (iobj >= 0) oid = CopyRecord(iobj, CopyGroup(), &pos, true, error);
    else oid = ImportRecordOfClass(EDCID(0x04), pos, from, error);
    if (!oid) { Status(std::string(T("Add a light at the view")) + ": " + error); return 0; }
    RelightView();
    std::printf("editor: light %d at %.0f %.0f %.0f\n", oid, pos.x, pos.y, pos.z);
    return oid;
}

bool CEditor::FLightingVerb(const std::string& verb, const std::string& arg)
{
    auto floats = [&](float* a, int c) { int i = 0; std::stringstream ss(arg); std::string t; while (i < c && std::getline(ss, t, ',')) a[i++] = float(std::atof(t.c_str())); return i; };
    if (verb == "lightadd") { float a[3]; glm::vec3 p = PosViewGround() + glm::vec3(0, 0, 300.0f); if (floats(a, 3) == 3) p = {a[0], a[1], a[2]}; AddLight(p); return true; }   // lightadd[:X,Y,Z]
    if (verb == "lightset" && m_plSel && m_plSel->Cid() == EDCID(0x04))   // lightset:H,S,V,STRENGTH,NEAR,FAR (the selected light)
    {
        float a[6];
        if (floats(a, 6) == 6)
        {
            LO* pl = m_plSel;
            const int oid = pl->oid;
            SetLightOption(pl, 152, a, 12);
            if ((pl = PloOid(oid))) SetLightOption(pl, 153, a + 3, 4);
            if ((pl = PloOid(oid))) SetLightOption(pl, 155, a + 4, 8);
        }
        return true;
    }
    if (verb == "lights")
    {
        for (const auto& up : m_psw->aploAll)
            if (up->Cid() == EDCID(0x04) && m_mpobj.count(up.get()))
            {
                glm::vec3 hsv;
                float s;
                glm::vec2 f;
                FLightValues(up.get(), hsv, s, f);
                const glm::vec3 p = PosWorld(up.get());
                std::printf("editor: light %d at %.0f %.0f %.0f hsv %.1f %.2f %.2f strength %.2f falloff %.0f %.0f\n", up->oid, p.x, p.y, p.z, hsv.x, hsv.y, hsv.z, s, f.x, f.y);
            }
        return true;
    }
    if (verb == "fog")   // fog:NEAR,FAR,MAX,RRGGBB (hex colour, alpha 0x80) or fog: (print)
    {
        std::string error;
        if (!arg.empty())
        {
            float a[3] = {0, 0, 0};
            std::stringstream ss(arg);
            std::string t;
            for (int i = 0; i < 3 && std::getline(ss, t, ','); ++i) a[i] = float(std::atof(t.c_str()));
            std::getline(ss, t, ',');
            if (!FSetWorldOpt(166, &a[0], 4, error) || !FSetWorldOpt(167, &a[1], 4, error) || !FSetWorldOpt(169, &a[2], 4, error)) Status("fog: " + error);
            if (!t.empty())
            {
                const uint32_t rgb = uint32_t(std::strtoul(t.c_str(), nullptr, 16));
                const uint32_t u = ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | 0x80000000u;
                if (!FSetWorldOpt(168, &u, 4, error)) Status("fog: " + error);
            }
        }
        ReloadWorldOpts();
        std::printf("editor: fog %.0f .. %.0f at most %.2f colour %08X (%s)\n", m_fogNear, m_fogFar, m_fogMax, m_rgbaFog, PworldOpt(1, 166) ? "the level's" : "the game's default");
        return true;
    }
    if (verb == "worldopts")   // the world's option lists (0 the SW's, 1 the camera's): eopid, field, kind
    {
        for (const EDITFIELD& f : m_rec.aworldopt)
            std::printf("editor: world option list %d: %d field +0x%X kind %d %s\n", f.i, f.eopid, g_disc.U32Elf(0x2AC880u + uint32_t(f.eopid) * 0x28u + 0x18), f.k, StrOption(f.eopid).c_str());
        return true;
    }
    if (verb == "ambient")   // ambient:H,S,V,SCALE (S, V, SCALE 0..255) or ambient: (print)
    {
        float a[4];
        std::string error;
        if (floats(a, 4) == 4 && (!FSetWorldOpt(914, a, 12, error, 0) || !FSetWorldOpt(916, a + 3, 4, error, 0))) Status("ambient: " + error);
        float h = 0, sat = 0, val = 0, sc = 0;
        FWorldOptFloat(914, h, 0, 0); FWorldOptFloat(914, sat, 0, 1); FWorldOptFloat(914, val, 0, 2); FWorldOptFloat(916, sc, 0, 0);
        std::printf("editor: ambient hsv %.0f %.0f %.0f scale %.0f\n", h, sat, val, sc);
        return true;
    }
    if (verb == "fogpreview") { m_fFogPreview = arg != "0"; ApplyFogPreview(); return true; }
    return false;
}
#else
bool CEditor::FLightingVerb(const std::string&, const std::string&) { return false; }
#endif

}  // namespace edimpl
