// The map editor's cameras (notes/EDITOR_PLAN.md step 2): the camera objects (Sly 2 class 0x2C: the look-from-object
// camera, cplcy.h CMOBJ / CPLOOKAT: it looks along its -z with x to its right, or at its target; options 1054 field of
// view, 1055 target, 1059 cut) with a live preview of what they show (the scene drawn from them into the editor's own
// texture), look through / set from the view, their frustum in the view; the camera zones (class 0x0F, cmworld.cpp
// BuildZonecm: the follow camera's parameters) with their options named by the option table's field offsets
// (cplcy.h ZONECM / ZONECME).
namespace edimpl
{

namespace
{
#if EDGAME_SLY2
// a ZONE's own camera fields by their offset (cmworld.cpp SetZoneField; cplcy.h ZONECM)
const char* SzZoneField(uint32_t ib)
{
    switch (ib)
    {
    case 0x120: return "camera zone: polar coefficients (angle, radius, z)";
    case 0x124: return "camera zone: decide even in the air";
    case 0x128: return "camera zone: priority";
    case 0x130: return "camera zone: polar angle change per frame (rad)";
    case 0x13C: return "camera zone: forced (-1 by position, 0 off, 1 on)";
    case 0x164: return "camera zone: flags (1 sample 1, 2 sample 2 / path, 4 player may turn)";
    case 0x168: return "camera zone: return 1 on";
    case 0x16C: return "camera zone: return 2 on";
    case 0x170: return "camera zone: return 1 time (s)";
    case 0x174: return "camera zone: return 1 strength";
    case 0x178: return "camera zone: return 2 time (s)";
    case 0x17C: return "camera zone: return 2 strength";
    case 0x180: return "camera zone: focus kind (-1: the focus's own)";
    case 0x184: return "camera zone: cut when it starts";
    case 0x188: return "camera zone: field of view (rad, 0 keep)";
    case 0x18C: return "camera zone: no collision avoidance";
    case 0x190: return "camera zone: look toward object";
    case 0x1A0: return "camera zone: lead toward the object (quadratic of the distance)";
    case 0x1B0: return "camera zone: lead limits";
    default: return nullptr;
    }
}
// a camera zone entry's fields by their offset (cmworld.cpp SetCmeField; cplcy.h ZONECME)
const char* SzCmeField(uint32_t ib)
{
    switch (ib)
    {
    case 0x04: return "camera entry: path sample offset";
    case 0x10: return "camera entry: pan (quadratic of the zone coordinate)";
    case 0x20: return "camera entry: tilt (quadratic)";
    case 0x30: return "camera entry: distance (quadratic)";
    case 0x40: return "camera entry: pan limits";
    case 0x48: return "camera entry: tilt limits";
    case 0x50: return "camera entry: distance limits";
    case 0x58: return "camera entry: focus screen offset x";
    case 0x5C: return "camera entry: focus screen offset y";
    case 0x60: return "camera entry: focus point (0 base, 1 adjusted)";
    case 0x64: return "camera entry: passive pan gain";
    case 0x68: return "camera entry: turn speed toward the samples";
    case 0x70: return "camera entry: y coefficients";
    case 0x80: return "camera entry: spring";
    case 0x84: return "camera entry: damping";
    case 0x88: return "camera entry: tilt limits (absolute)";
    default: return nullptr;
    }
}
#endif
}  // namespace

const char* CEditor::SzOptionOf(const LO* plo, int eopid) const
{
#if EDGAME_SLY2
    if (plo && plo->Cid() == 0x2C)
        switch (eopid)
        {
        case 1054: return "camera: field of view (rad, 0 keep)";
        case 1055: return "camera: look at (object)";
        case 1059: return "camera: cut when it ends";
        default: break;
        }
    if (plo && plo->Cid() == 0x0F && eopid > 0 && eopid < 0x800)
    {
        // the option table: flags +4, field +0x18, setter +0x1C, getter +0x24 (a camera entry's field: getter 0x221958)
        const uint32_t va = 0x2AC880u + uint32_t(eopid) * 0x28u;
        const uint32_t grf = g_disc.U32Elf(va + 4), ib = g_disc.U32Elf(va + 0x18), pfnSet = g_disc.U32Elf(va + 0x1C), pfnGet = g_disc.U32Elf(va + 0x24);
        if (grf & 0x100)
        {
            if (pfnSet == 0x221818) return "camera entry: for focus class (starts an entry)";
            if (pfnSet == 0x221960) return "camera entry: y coefficients";
        }
        else if (grf & 0x80)
        {
            if (pfnGet == 0x221958) { if (const char* sz = SzCmeField(ib)) return sz; }
            else if (pfnGet == 0) { if (const char* sz = SzZoneField(ib)) return sz; }
        }
    }
#else
    (void)plo;
#endif
    return SzOptionName(eopid);
}

bool CEditor::FOptionFloat(LO* plo, int eopid, float& g) const
{
    const int ifield = IfieldOption(plo, eopid);
    std::string rel;
    std::size_t ib = 0;
    const std::vector<uint8_t>* pab = nullptr;
    if (ifield < 0 || OptType(m_rec.afield[std::size_t(ifield)].k).cf < 1 || !FFieldBytes(ifield, 4, rel, ib, pab)) return false;
    std::memcpy(&g, pab->data() + ib, 4);
    return true;
}

// what a camera object shows: at its place, looking at its target or along its -z (x right, y up), its field of view
bool CEditor::FCameraView(LO* plo, CAMERA& cam) const
{
    if (!plo || TypeOfLo(plo) != OT_Camera || !FHasMatrix(plo)) return false;
    const glm::mat4 W = MatWorld(plo);
    cam = CAMERA();
    cam.pos = glm::vec3(W[3]);
    glm::vec3 dir = -glm::normalize(glm::vec3(W[2]) + glm::vec3(1e-9f)), up = glm::normalize(glm::vec3(W[1]) + glm::vec3(1e-9f));
    int oidTarget = 0;
    if (FOptionInt(plo, EDEOPID(1055), oidTarget) && oidTarget > 0)
        if (LO* pt = PloOid(oidTarget); pt && FHasMatrix(pt))
        {
            const glm::vec3 d = PosWorld(pt) - cam.pos;
            if (glm::length(d) > 1.0f) { dir = glm::normalize(d); up = glm::vec3(0, 0, 1); }
        }
    glm::vec3 left = glm::cross(up, dir);
    if (glm::length(left) < 1e-4f) left = glm::cross(glm::vec3(0, 1, 0), dir);
    left = glm::normalize(left);
    cam.fMat = true;
    cam.mat[0] = dir;
    cam.mat[1] = left;
    cam.mat[2] = glm::cross(dir, left);
    cam.yaw = glm::degrees(std::atan2(dir.y, dir.x));
    cam.pitch = glm::degrees(std::asin(std::clamp(dir.z, -1.0f, 1.0f)));
    float rad = 0;
    cam.fovY = FOptionFloat(plo, EDEOPID(1054), rad) && rad > 0.01f && rad < 3.0f ? glm::degrees(rad) : m_fovCameraDefault;
    cam.sNear = 10.0f;
    cam.sFar = 1e6f;
    return true;
}

// the preview: the scene drawn from the camera into the game's target, then copied down into the editor's texture
// (before the editor's own view is drawn over the target)
void CEditor::RenderCameraPreview()
{
    m_fPreviewShown = false;
    CAMERA cam;
    if (!m_fPreview || m_fLookThrough || !FCameraView(m_plSel, cam)) return;
    const int dx = 480, dy = std::max(120, int(480.0f * float(g_gl.renderHeight) / float(std::max(1, g_gl.renderWidth))));
    if (!m_fboPreview || m_dxPreview != dx || m_dyPreview != dy)
    {
        if (!m_fboPreview) { glGenFramebuffers(1, &m_fboPreview); glGenTextures(1, &m_texPreview); }
        glBindTexture(GL_TEXTURE_2D, m_texPreview);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fboPreview);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texPreview, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_dxPreview = dx;
        m_dyPreview = dy;
    }
    RenderScene(m_scene, cam, m_opts);
    const int w = g_gl.renderWidth, h = g_gl.renderHeight;
    GLuint src = g_gl.FboScene();
    if (g_gl.fboMsaa)
    {
        // (a multisampled target resolves at its own size first)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        src = g_gl.fbo;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fboPreview);
    glBlitFramebuffer(0, 0, w, h, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    // (opaque: the scene target's alpha is the game's, not a picture's)
    glBindFramebuffer(GL_FRAMEBUFFER, m_fboPreview);
    glViewport(0, 0, dx, dy);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_fPreviewShown = true;
}

// the camera's frustum in the view (and its line to its target)
void CEditor::DrawCameraFrusta()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto draw = [&](LO* plo, bool fSel) {
        CAMERA c;
        if (!FCameraView(plo, c) || FHidden(plo)) return;
        if (glm::length(c.pos - m_cam.pos) > 12000.0f && !fSel) return;
        const float len = fSel ? 260.0f : 140.0f, th = std::tan(glm::radians(c.fovY) * 0.5f), aspect = 16.0f / 9.0f;
        const glm::vec3 f = c.mat[0], l = c.mat[1], u = c.mat[2];
        const glm::vec3 a[4] = {c.pos + (f + u * th + l * th * aspect) * len, c.pos + (f + u * th - l * th * aspect) * len,
                                c.pos + (f - u * th - l * th * aspect) * len, c.pos + (f - u * th + l * th * aspect) * len};
        ImVec2 s0, s[4];
        if (!FProject(c.pos, s0)) return;
        bool af[4];
        for (int i = 0; i < 4; ++i) af[i] = FProject(a[i], s[i]);
        const ImU32 col = st::u32(fSel ? st::cta : 0xFFFFFF, fSel ? 0.95f : 0.45f);
        for (int i = 0; i < 4; ++i)
        {
            if (af[i]) dl->AddLine(s0, s[i], col, fSel ? 2.0f : 1.0f);
            if (af[i] && af[(i + 1) % 4]) dl->AddLine(s[i], s[(i + 1) % 4], col, fSel ? 2.0f : 1.0f);
        }
        int oidTarget = 0;
        if (fSel && FOptionInt(plo, EDEOPID(1055), oidTarget) && oidTarget > 0)
            if (LO* pt = PloOid(oidTarget); pt && FHasMatrix(pt))
                if (ImVec2 st2; FProject(PosWorld(pt), st2)) dl->AddLine(s0, st2, st::u32(st::cta, 0.5f), 1.0f);
    };
    for (const auto& up : m_psw->aploAll)
        if (TypeOfLo(up.get()) == OT_Camera) draw(up.get(), FSelected(up.get()));
}

// the preview panel (a camera selected): the picture, look through, set from the view, its settings
void CEditor::DrawCameraPanel()
{
    CAMERA cam;
    if (!m_plSel || !FCameraView(m_plSel, cam)) { m_fLookThrough = false; return; }
    LO* plo = m_plSel;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float top = ImGui::GetFrameHeight();
    const float wRight = std::clamp(size.x * 0.25f, 330.0f, 470.0f);
    const float w = 500.0f;
    ImGui::SetNextWindowPos(ImVec2(size.x - wRight - 12 - w, top + 6), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin((std::string(T("Camera preview")) + ": " + NameOfObject(plo) + "###campreview").c_str(), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Checkbox(T("Preview"), &m_fPreview);
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Look through"), &m_fLookThrough))
    {
        if (m_fLookThrough) { m_camBeforeLook = m_cam; m_fPilot = false; }
        else m_cam = m_camBeforeLook;
    }
    EdTip(T("The main view shows what the camera sees; with Pilot on, flying moves the camera"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_fLookThrough);
    ImGui::Checkbox(T("Pilot"), &m_fPilot);
    ImGui::EndDisabled();
    EdTip(T("Fly the view (right mouse + WASD) and the camera object follows; turned off it is stored (one undo step)"));
    if (m_fPreviewShown && m_texPreview)
        ImGui::Image(ImTextureID(intptr_t(m_texPreview)), ImVec2(float(m_dxPreview), float(m_dyPreview)), ImVec2(0, 1), ImVec2(1, 0));
    else if (m_fLookThrough) ImGui::TextDisabled("%s", T("(the main view is the camera's)"));
    // its settings: field of view (degrees here, radians stored), target, cut
    float rad = 0;
    const bool fHasFov = FOptionFloat(plo, EDEOPID(1054), rad);
    float deg = fHasFov && rad > 0 ? glm::degrees(rad) : cam.fovY;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat(T("Field of view"), &deg, 5.0f, 120.0f, "%.1f°") || false) m_fovEdit = deg;
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        const float r = glm::radians(m_fovEdit);
        const std::string label = std::string(T("Field of view of")) + " " + NameOfObject(plo);
        if (fHasFov) FSetOption(IfieldOption(plo, EDEOPID(1054)), &r, 4, label);
        else
        {
            std::vector<uint8_t> ab(4);
            std::memcpy(ab.data(), &r, 4);
            FAddOption(plo, EDEOPID(1054), ab, label);
        }
    }
    if (!fHasFov) { ImGui::SameLine(); ImGui::TextDisabled("%s", T("(default)")); }
    int oidTarget = 0;
    const bool fHasTarget = FOptionInt(plo, EDEOPID(1055), oidTarget);
    if (fHasTarget && oidTarget > 0)
    {
        LO* pt = PloOid(oidTarget);
        ImGui::TextColored(st::vec(st::muted), "%s:", T("Looks at"));
        ImGui::SameLine();
        ImGui::TextColored(st::vec(st::accent), "%s", pt ? NameOfObject(pt).c_str() : std::to_string(oidTarget).c_str());
        if (pt && ImGui::IsItemClicked()) SelectOnly(pt);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(its rotation is not used)"));
    }
    else ImGui::TextColored(st::vec(st::muted), "%s", T("Looks along its own direction (no target)"));
    if (ImGui::Button(T("Set from the view")))
    {
        // the camera object at the editor's eye, turned to its direction (looks along -z, x right, y up)
        const glm::vec3 d = m_cam.Dir();
        glm::vec3 r = glm::cross(d, glm::vec3(0, 0, 1));
        r = glm::length(r) < 1e-4f ? glm::vec3(1, 0, 0) : glm::normalize(r);
        const glm::vec3 u = glm::cross(r, d);
        glm::mat4 W(glm::vec4(r, 0), glm::vec4(u, 0), glm::vec4(-d, 0), glm::vec4(m_cam.pos, 1));
        PLACE pl;
        if (FPlace(plo, pl))
        {
            SetPlaceWorld(pl, W);
            FRecordPlace(pl, std::string(T("Camera from the view")) + ": " + NameOfObject(plo));
        }
    }
    EdTip(T("Puts the camera object where the editor's view is, looking the same way"));
    ImGui::SameLine();
    if (ImGui::Button(T("View from the camera"))) { m_cam.pos = cam.pos; m_cam.yaw = cam.yaw; m_cam.pitch = cam.pitch; }
    ImGui::End();
}

// look through / pilot: the main view is the camera's; piloting moves the camera object with the view
void CEditor::UpdateLookThrough()
{
    CAMERA cam;
    if (!m_fLookThrough || !FCameraView(m_plSel, cam)) { if (m_fPilotMoved) StorePilot(); return; }
    if (!m_fPilot)
    {
        if (m_fPilotMoved) StorePilot();
        m_cam.pos = cam.pos;
        m_cam.yaw = cam.yaw;
        m_cam.pitch = cam.pitch;
        m_cam.fovY = cam.fovY;
        return;
    }
    // the object follows the view (stored when the flying stops)
    const glm::vec3 d = m_cam.Dir();
    glm::vec3 r = glm::cross(d, glm::vec3(0, 0, 1));
    r = glm::length(r) < 1e-4f ? glm::vec3(1, 0, 0) : glm::normalize(r);
    const glm::vec3 u = glm::cross(r, d);
    const glm::mat4 W(glm::vec4(r, 0), glm::vec4(u, 0), glm::vec4(-d, 0), glm::vec4(m_cam.pos, 1));
    PLACE pl;
    if (FPlace(m_plSel, pl) && glm::length(glm::vec3(MatWorld(pl.ploLive)[3]) - m_cam.pos) + glm::length(glm::vec3(MatWorld(pl.ploLive)[2]) + d) > 1e-3f)
    {
        SetPlaceWorld(pl, W);
        m_fPilotMoved = true;
    }
    if (m_fPilotMoved && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) StorePilot();
}

void CEditor::StorePilot()
{
    m_fPilotMoved = false;
    PLACE pl;
    if (m_plSel && FPlace(m_plSel, pl)) FRecordPlace(pl, std::string(T("Pilot camera")) + ": " + NameOfObject(m_plSel));
}

}  // namespace edimpl
