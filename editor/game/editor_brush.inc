// The map editor's brushes (notes/EDITOR_PLAN.md step 4): blocks, ramps, stairs, cylinders and floors made into new
// objects of the world: a model (one material: a grid of a colour, or the texture of another object's first material,
// mapped by the faces' directions so it tiles), lit as the game lights its models, and collision of the shape's convex
// pieces (a ramp walkable as a slope, each stair its own box). Through the phase-4 model pipeline (a .glb in the
// project's editor/brushes/, FImportModel as a new object). Kept in editor/brushes.json: a brush's size or texture
// changed makes its model and collision again in place.
#if EDFEAT_MODELS
#include "core/model.h"
#endif
namespace edimpl
{

#if EDFEAT_MODELS
namespace md = cane::editor::model;

namespace
{
// a face list into the model (flat normals, planar UVs by the face's main axis) and the collision piece; positions
// in game units (x right, y depth, z up), the model in the file's convention the import turns back (x, -z, y)
void AddPiece(md::Primitive& pr, std::vector<CONVEX>& acv, const std::vector<glm::vec3>& apos, const std::vector<std::vector<int>>& aface,
              float texScale, bool fCollide)
{
    for (const std::vector<int>& f : aface)
    {
        const glm::vec3 n = glm::normalize(glm::cross(apos[std::size_t(f[1])] - apos[std::size_t(f[0])], apos[std::size_t(f[2])] - apos[std::size_t(f[0])]));
        const glm::vec3 an = glm::abs(n);
        const int axis = an.z >= an.x && an.z >= an.y ? 2 : an.x >= an.y ? 0 : 1;
        const uint32_t i0 = uint32_t(pr.pos.size());
        for (int k : f)
        {
            const glm::vec3 p = apos[std::size_t(k)];
            pr.pos.push_back(glm::vec3(p.x, p.z, -p.y));
            pr.nrm.push_back(glm::vec3(n.x, n.z, -n.y));
            const glm::vec2 uv = axis == 2 ? glm::vec2(p.x, -p.y) : axis == 0 ? glm::vec2(p.y * (n.x > 0 ? 1.0f : -1.0f), -p.z) : glm::vec2(p.x * (n.y < 0 ? 1.0f : -1.0f), -p.z);
            pr.uv.push_back(uv / std::max(1.0f, texScale));
        }
        for (std::size_t k = 1; k + 1 < f.size(); ++k)
        {
            pr.idx.push_back(i0);
            pr.idx.push_back(i0 + uint32_t(k));
            pr.idx.push_back(i0 + uint32_t(k + 1));
        }
    }
    if (fCollide) acv.push_back({apos, aface});
}
// a box [mn, mx] (faces outward: cross(p1 - p0, p2 - p0) points out)
void AddBox(md::Primitive& pr, std::vector<CONVEX>& acv, const glm::vec3& mn, const glm::vec3& mx, float texScale)
{
    std::vector<glm::vec3> a(8);
    for (int i = 0; i < 8; ++i) a[std::size_t(i)] = glm::vec3(i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z);
    AddPiece(pr, acv, a, {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}}, texScale, true);
}
// a grid texture of a colour (dark lines every eighth, as editors' development textures)
md::Image GridImage(uint32_t rgb)
{
    md::Image img;
    img.name = "brush_grid";
    img.w = img.h = 128;
    img.rgba.resize(128 * 128 * 4);
    const int r = int(rgb >> 16) & 255, g = int(rgb >> 8) & 255, b = int(rgb) & 255;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            const bool fMajor = x % 64 < 2 || y % 64 < 2, fMinor = x % 16 == 0 || y % 16 == 0;
            const float k = fMajor ? 0.55f : fMinor ? 0.8f : 1.0f;
            uint8_t* p = &img.rgba[std::size_t(y * 128 + x) * 4];
            p[0] = uint8_t(r * k);
            p[1] = uint8_t(g * k);
            p[2] = uint8_t(b * k);
            p[3] = 255;
        }
    return img;
}
const char* const s_aszShape[] = {"Block", "Ramp", "Stairs", "Cylinder", "Floor", "Crawl tunnel", "Mesh"};
constexpr int c_cshape = 7;
}  // namespace

bool CEditor::FBuildBrush(const BRUSH& b, md::Scene& sc, std::vector<CONVEX>& acv, std::string& error)
{
    sc = md::Scene();
    acv.clear();
    md::Mesh mesh;
    mesh.name = std::string("brush_") + s_aszShape[std::clamp(b.shape, 0, c_cshape - 1)];
    md::Primitive pr;
    pr.material = 0;
    const glm::vec3 s = glm::max(b.size, glm::vec3(1.0f));
    const float hx = s.x * 0.5f, hy = s.y * 0.5f;
    switch (b.shape)
    {
    case 0: AddBox(pr, acv, glm::vec3(-hx, -hy, 0), glm::vec3(hx, hy, s.z), b.texScale); break;
    case 4: AddBox(pr, acv, glm::vec3(-hx, -hy, -std::min(s.z, 32.0f)), glm::vec3(hx, hy, 0), b.texScale); break;
    case 5:
        // a crawl tunnel along y: the inside width x, the ceiling's underside at z (lower than a standing player, 200
        // above him keeps the crawl going: interact.cpp FInteractCovered), side walls and the ceiling 32 thick
        AddBox(pr, acv, glm::vec3(-hx - 32.0f, -hy, 0), glm::vec3(-hx, hy, s.z), b.texScale);
        AddBox(pr, acv, glm::vec3(hx, -hy, 0), glm::vec3(hx + 32.0f, hy, s.z), b.texScale);
        AddBox(pr, acv, glm::vec3(-hx - 32.0f, -hy, s.z), glm::vec3(hx + 32.0f, hy, s.z + 32.0f), b.texScale);
        break;
    case 1:
    {
        // a wedge rising toward +y: 0 1 at the low front edge, 2 3 at the top back edge
        const std::vector<glm::vec3> a = {{-hx, -hy, 0}, {hx, -hy, 0}, {-hx, hy, 0}, {hx, hy, 0}, {-hx, hy, s.z}, {hx, hy, s.z}};
        AddPiece(pr, acv, a, {{0, 2, 3, 1}, {0, 1, 5, 4}, {2, 4, 5, 3}, {0, 4, 2}, {1, 3, 5}}, b.texScale, true);
        break;
    }
    case 2:
    {
        // steps rising toward +y, each a box from the floor to its top (its own collision piece)
        const int n = std::clamp(b.steps, 1, 64);
        for (int i = 0; i < n; ++i)
        {
            const float y0 = -hy + s.y * float(i) / float(n), y1 = -hy + s.y * float(i + 1) / float(n);
            AddBox(pr, acv, glm::vec3(-hx, y0, 0), glm::vec3(hx, y1, s.z * float(i + 1) / float(n)), b.texScale);
        }
        break;
    }
    case 3:
    {
        // a prism of n sides around z (radius: half the width; depth scales it into an ellipse)
        const int n = std::clamp(b.segments, 3, 64);
        std::vector<glm::vec3> a;
        for (int i = 0; i < n; ++i)
        {
            const float t = 6.2831853f * float(i) / float(n);
            a.push_back({std::cos(t) * hx, std::sin(t) * hy, 0});
        }
        for (int i = 0; i < n; ++i) a.push_back(a[std::size_t(i)] + glm::vec3(0, 0, s.z));
        std::vector<std::vector<int>> af;
        std::vector<int> bottom, top;
        for (int i = n - 1; i >= 0; --i) bottom.push_back(i);
        for (int i = 0; i < n; ++i) top.push_back(n + i);
        af.push_back(bottom);
        af.push_back(top);
        for (int i = 0; i < n; ++i) af.push_back({i, (i + 1) % n, n + (i + 1) % n, n + i});
        AddPiece(pr, acv, a, af, b.texScale, true);
        break;
    }
    case 6:
    {
        // an edited brush (editor_brushedit.inc): its convex pieces, each face in its material slot's primitive
        const std::size_t cmat = 1 + b.amat.size();
        std::vector<md::Primitive> apr(cmat);
        for (std::size_t m = 0; m < cmat; ++m) apr[m].material = int(m);
        std::vector<CONVEX> acvNone;
        for (std::size_t i = 0; i < b.apiece.size(); ++i)
        {
            const CONVEX& cv = b.apiece[i];
            for (std::size_t j = 0; j < cv.aface.size(); ++j)
            {
                int m = i < b.afaceMat.size() && j < b.afaceMat[i].size() ? b.afaceMat[i][j] : 0;
                if (m < 0 || std::size_t(m) >= cmat) m = 0;
                AddPiece(apr[std::size_t(m)], acvNone, cv.apos, {cv.aface[j]}, b.texScale, false);
            }
            acv.push_back(cv);
        }
        if (acv.empty()) { error = "the brush has no pieces"; return false; }
        pr = std::move(apr[0]);
        for (std::size_t m = 1; m < cmat; ++m)
            if (!apr[m].idx.empty()) mesh.prims.push_back(std::move(apr[m]));
        break;
    }
    default: error = "no such shape"; return false;
    }
    mesh.prims.insert(mesh.prims.begin(), std::move(pr));
    sc.meshes.push_back(std::move(mesh));
    // the texture of a slot: another object's first material (exported through the GLB path), else a grid of the colour
    auto addImage = [&](int texFrom, uint32_t rgb) {
    bool fTex = false;
    if (texFrom > 0)
        if (LO* psrc = PloOid(texFrom))
        {
            const cane::fs::path tmp = m_proj.dir / "editor" / "brushes" / ("tex_" + std::to_string(texFrom) + ".glb");
            std::error_code ec;
            cane::fs::create_directories(tmp.parent_path(), ec);
            md::Scene scs;
            std::string e;
            if (FExportGlb(psrc, 0, ce::utf8(tmp), e) && md::load(tmp, scs, e))
            {
                // the textured material covering the most triangles
                std::map<int, std::size_t> mpc;
                for (const md::Mesh& m : scs.meshes)
                    for (const md::Primitive& p : m.prims)
                        if (p.material >= 0 && std::size_t(p.material) < scs.materials.size() && scs.materials[std::size_t(p.material)].image >= 0) mpc[p.material] += p.idx.size() / 3;
                int imat = -1;
                std::size_t cBest = 0;
                for (const auto& [i, c] : mpc)
                    if (c > cBest) { cBest = c; imat = i; }
                const int iimg = imat >= 0 ? scs.materials[std::size_t(imat)].image : -1;
                if (iimg >= 0 && std::size_t(iimg) < scs.images.size()) { sc.images.push_back(scs.images[std::size_t(iimg)]); fTex = true; }
            }
        }
    if (!fTex) sc.images.push_back(GridImage(rgb));
    md::Material mat;
    mat.name = "brush" + std::to_string(sc.materials.size());
    mat.image = int(sc.images.size()) - 1;
    sc.materials.push_back(mat);
    };
    addImage(b.texFrom, b.rgb);
    if (b.shape == 6)
        for (const BRUSHMAT& m : b.amat) addImage(m.texFrom, m.rgb);
    return true;
}

// a new object made from the brush in front of the camera (on the ground, on the grid)
int CEditor::CreateBrush(const BRUSH& bIn, const glm::vec3* ppos, std::string& error)
{
    BRUSH b = bIn;
    md::Scene sc;
    std::vector<CONVEX> acv;
    if (!FBuildBrush(b, sc, acv, error)) return 0;
    const cane::fs::path dir = m_proj.dir / "editor" / "brushes";
    std::error_code ec;
    cane::fs::create_directories(dir, ec);
    const cane::fs::path path = dir / ("brush_" + std::to_string(m_abrush.size() + 1) + "_" + std::to_string(std::rand() % 100000) + ".glb");
    if (!md::saveGlb(path, sc, error)) return 0;
    // the import: game units (scale 1), lit, the brush's own collision pieces, cel borders
    const float scale = m_importScale;
    const int coll = m_importCollision;
    const bool fLit = m_importLit, fOut = m_importOutline;
    m_importScale = 1.0f;
    m_importCollision = 3;
    m_importLit = true;
    m_importOutline = true;
    m_aconvexImport = acv;
    // (the place: before the import, the camera's front on the ground; the import puts the copy there)
    const CAMERA camSaved = m_cam;
    if (ppos)
    {
        // (FImportModel places 600 in front of the camera, snapped down: the camera moved so that point is *ppos)
        m_cam.pos = *ppos - DirPlace() * 600.0f + glm::vec3(0, 0, 1.0f);
    }
    const bool fOk = FImportModel(ce::utf8(path), nullptr, true, error);
    m_cam = camSaved;
    m_importScale = scale;
    m_importCollision = coll;
    m_importLit = fLit;
    m_importOutline = fOut;
    m_aconvexImport.clear();
    if (!fOk || !m_plSel) return 0;
    b.oid = m_plSel->oid;
    // the grid: the object's place on it
    PLACE pl;
    if (FPlace(m_plSel, pl))
    {
        // upright, unscaled (the carrier object's own rotation dropped), on the grid
        glm::vec3 p = PosWorld(pl.ploLive);
        if (m_fGridSnap) { p.x = SnapMove(p.x); p.y = SnapMove(p.y); }
        SetPlaceWorld(pl, glm::translate(glm::mat4(1.0f), p));
        FRecordPlace(pl, std::string(T("Brush on the grid")) + " " + std::to_string(b.oid));
    }
    m_abrush.push_back(b);
    SaveBrushes();
    Status(std::string(T("made")) + " " + T(s_aszShape[std::clamp(b.shape, 0, c_cshape - 1)]) + " " + std::to_string(b.oid) + " (" + std::to_string(acv.size()) + " " + T("collision pieces") + ")");
    return b.oid;
}

bool CEditor::FRebuildBrush(BRUSH& b, std::string& error)
{
    LO* plo = PloOid(b.oid);
    if (!plo) { error = "its object is gone"; return false; }
    md::Scene sc;
    std::vector<CONVEX> acv;
    if (!FBuildBrush(b, sc, acv, error)) return false;
    const cane::fs::path path = m_proj.dir / "editor" / "brushes" / ("brush_" + std::to_string(b.oid) + ".glb");
    if (!md::saveGlb(path, sc, error)) return false;
    const float scale = m_importScale;
    const int coll = m_importCollision;
    m_importScale = 1.0f;
    m_importCollision = 3;
    m_aconvexImport = acv;
    SelectOnly(plo);
    const bool fOk = FImportModel(ce::utf8(path), plo, false, error);
    m_importScale = scale;
    m_importCollision = coll;
    m_aconvexImport.clear();
    if (fOk) SaveBrushes();
    return fOk;
}

BRUSH* CEditor::PbrushOf(const LO* plo)
{
    if (!plo) return nullptr;
    for (BRUSH& b : m_abrush)
        if (b.oid == plo->oid) return &b;
    return nullptr;
}

void CEditor::SaveBrushes()
{
    nlohmann::json a = nlohmann::json::array();
    for (const BRUSH& b : m_abrush)
    {
        nlohmann::json e = {{"shape", s_aszShape[std::clamp(b.shape, 0, c_cshape - 1)]}, {"size", {b.size.x, b.size.y, b.size.z}}, {"steps", b.steps},
                            {"segments", b.segments}, {"tex_scale", b.texScale}, {"tex_from", b.texFrom}, {"rgb", b.rgb}, {"oid", b.oid}};
        if (b.shape == 6)
        {
            // (a mesh brush: its pieces' points and faces, each face's material slot, the extra slots)
            nlohmann::json ap = nlohmann::json::array();
            for (std::size_t i = 0; i < b.apiece.size(); ++i)
            {
                nlohmann::json pts = nlohmann::json::array(), fcs = nlohmann::json::array();
                for (const glm::vec3& p : b.apiece[i].apos) pts.push_back({p.x, p.y, p.z});
                for (const auto& f : b.apiece[i].aface) fcs.push_back(f);
                ap.push_back({{"points", pts}, {"faces", fcs}, {"face_materials", i < b.afaceMat.size() ? nlohmann::json(b.afaceMat[i]) : nlohmann::json::array()}});
            }
            e["pieces"] = ap;
            nlohmann::json am = nlohmann::json::array();
            for (const BRUSHMAT& m : b.amat) am.push_back({{"tex_from", m.texFrom}, {"rgb", m.rgb}});
            e["materials"] = am;
        }
        a.push_back(e);
    }
    const std::string s = nlohmann::json({{"brushes", a}}).dump(1);
    ce::writeFile(m_proj.dir / "editor" / "brushes.json", std::vector<uint8_t>(s.begin(), s.end()));
}

void CEditor::LoadBrushes()
{
    m_abrush.clear();
    std::vector<uint8_t> ab;
    if (!ce::readFile(m_proj.dir / "editor" / "brushes.json", ab)) return;
    const nlohmann::json j = nlohmann::json::parse(std::string(ab.begin(), ab.end()), nullptr, false);
    if (!j.is_object() || !j.contains("brushes") || !j["brushes"].is_array()) return;
    for (const auto& e : j["brushes"])
    {
        if (!e.is_object()) continue;
        BRUSH b;
        const std::string shape = e.value("shape", std::string("Block"));
        for (int i = 0; i < c_cshape; ++i)
            if (shape == s_aszShape[i]) b.shape = i;
        if (e.contains("size") && e["size"].is_array() && e["size"].size() == 3) b.size = glm::vec3(e["size"][0].get<float>(), e["size"][1].get<float>(), e["size"][2].get<float>());
        b.steps = e.value("steps", b.steps);
        b.segments = e.value("segments", b.segments);
        b.texScale = e.value("tex_scale", b.texScale);
        b.texFrom = e.value("tex_from", 0);
        b.rgb = e.value("rgb", b.rgb);
        b.oid = e.value("oid", 0);
        if (e.contains("pieces") && e["pieces"].is_array())
            for (const auto& p : e["pieces"])
            {
                CONVEX cv;
                for (const auto& q : p.value("points", nlohmann::json::array()))
                    if (q.is_array() && q.size() == 3) cv.apos.push_back(glm::vec3(q[0].get<float>(), q[1].get<float>(), q[2].get<float>()));
                for (const auto& f : p.value("faces", nlohmann::json::array()))
                {
                    std::vector<int> af;
                    for (const auto& k : f)
                        if (k.is_number_integer() && k.get<int>() >= 0 && k.get<int>() < int(cv.apos.size())) af.push_back(k.get<int>());
                    if (af.size() >= 3) cv.aface.push_back(af);
                }
                std::vector<int> am;
                for (const auto& k : p.value("face_materials", nlohmann::json::array()))
                    if (k.is_number_integer()) am.push_back(k.get<int>());
                b.apiece.push_back(cv);
                b.afaceMat.push_back(am);
            }
        if (e.contains("materials") && e["materials"].is_array())
            for (const auto& m : e["materials"]) b.amat.push_back({m.value("tex_from", 0), m.value("rgb", 0xB8B8B8u)});
        m_abrush.push_back(b);
    }
}

void CEditor::DrawBrushTools()
{
    ImGui::SeparatorText(T("Brushes"));
    ImGui::TextColored(st::vec(st::muted), "%s", T("New geometry with collision, made in front of the camera on the ground."));
    BRUSH& b = m_brushNew;
    for (int i = 0; i < c_cshape - 1; ++i)   // (Mesh: an edited brush, not made from here)
    {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(T(s_aszShape[i]), b.shape == i)) b.shape = i;
    }
    ImGui::SetNextItemWidth(260);
    ImGui::DragFloat3(T("Width / depth / height"), glm::value_ptr(b.size), 4.0f, 8.0f, 20000.0f, "%.0f");
    if (b.shape == 2) { ImGui::SetNextItemWidth(120); ImGui::SliderInt(T("Steps"), &b.steps, 1, 32); }
    if (b.shape == 3) { ImGui::SetNextItemWidth(120); ImGui::SliderInt(T("Sides"), &b.segments, 3, 48); }
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat(T("Texture size"), &b.texScale, 4.0f, 16.0f, 4096.0f, "%.0f");
    ImGui::SameLine();
    float col[3] = {float((b.rgb >> 16) & 255) / 255.0f, float((b.rgb >> 8) & 255) / 255.0f, float(b.rgb & 255) / 255.0f};
    if (ImGui::ColorEdit3(T("Grid colour"), col, ImGuiColorEditFlags_NoInputs))
        b.rgb = (uint32_t(col[0] * 255) << 16) | (uint32_t(col[1] * 255) << 8) | uint32_t(col[2] * 255);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(T("Texture from object (oid, 0: grid)"), &b.texFrom, 0);
    if (m_plSel && ImGui::SmallButton(T("Use the selection's texture"))) b.texFrom = m_plSel->oid;
    ImGui::PushStyleColor(ImGuiCol_Button, st::vec(st::cta));
    ImGui::PushStyleColor(ImGuiCol_Text, st::vec(st::ctaText));
    if (ImGui::Button(T("Make it")))
    {
        std::string error;
        if (!CreateBrush(b, nullptr, error)) Status(std::string(T("brush")) + ": " + error);
    }
    ImGui::PopStyleColor(2);
}

void CEditor::DrawBrushProperties(LO* plo)
{
    BRUSH* pb = PbrushOf(plo);
    if (!pb || !ImGui::CollapsingHeader(TL("Brush", "brushprops").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    BRUSH b = *pb;
    bool f = false;
    ImGui::TextColored(st::vec(st::muted), "%s", T(s_aszShape[std::clamp(b.shape, 0, c_cshape - 1)]));
    if (b.shape != 6)
    {
        ImGui::SetNextItemWidth(-140);
        ImGui::DragFloat3(T("Size"), glm::value_ptr(b.size), 4.0f, 8.0f, 20000.0f, "%.0f");
        f |= ImGui::IsItemDeactivatedAfterEdit();
    }
    if (b.shape == 2) { ImGui::SliderInt(T("Steps"), &b.steps, 1, 32); f |= ImGui::IsItemDeactivatedAfterEdit(); }
    if (b.shape == 3) { ImGui::SliderInt(T("Sides"), &b.segments, 3, 48); f |= ImGui::IsItemDeactivatedAfterEdit(); }
    ImGui::DragFloat(T("Texture size"), &b.texScale, 4.0f, 16.0f, 4096.0f, "%.0f");
    f |= ImGui::IsItemDeactivatedAfterEdit();
    *pb = b;
    if (f)
    {
        std::string error;
        if (!FRebuildBrush(*pb, error)) Status(std::string(T("brush")) + ": " + error);
    }
    if ((pb = PbrushOf(plo)) != nullptr) DrawBrushEditTools(*pb);   // (editor_brushedit.inc)
}
#else
int CEditor::CreateBrush(const BRUSH&, const glm::vec3*, std::string& error)
{
    error = "brushes: not for " EDGAME_NAME " yet";
    return 0;
}
void CEditor::LoadBrushes() {}
void CEditor::DrawBrushTools() { ImGui::TextDisabled("(brushes: not for " EDGAME_NAME " yet)"); }
void CEditor::DrawBrushProperties(LO*) {}
#endif

}  // namespace edimpl
