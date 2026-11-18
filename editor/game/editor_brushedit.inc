// The map editor's brush face / edge / vertex editing (notes/EDITOR_PLAN.md step 13). A brush made editable becomes a
// "Mesh" brush: its convex pieces kept as points and faces in editor/brushes.json (each face with a material slot: the
// brush's own grid / texture, or another one added), made into the object's model and collision as the other shapes.
// In the view (the Brush section's Edit switch on) a click picks a face of the selected brush, the nearest edge or
// point of it by the mode; then: a face pushed / pulled along its normal or extruded (a new piece), a point or an edge
// moved (the piece made convex again: its points' hull), a piece split in two by a plane through its middle, merged with
// another (the hull of both), deleted; a face given another texture.
#if EDFEAT_MODELS
namespace edimpl
{

namespace
{
constexpr float c_epsHull = 0.05f;

// the convex hull of points as faces (counter-clockwise seen from outside); brute force, the pieces are small
bool FHull(std::vector<glm::vec3> apt, CONVEX& cv)
{
    // (points welded)
    std::vector<glm::vec3> a;
    for (const glm::vec3& p : apt)
        if (std::none_of(a.begin(), a.end(), [&](const glm::vec3& q) { return glm::length(p - q) < 0.5f; })) a.push_back(p);
    const int n = int(a.size());
    if (n < 4) return false;
    struct PLANE { glm::vec3 n; float d; };
    std::vector<PLANE> aplane;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int k = j + 1; k < n; ++k)
            {
                glm::vec3 nn = glm::cross(a[std::size_t(j)] - a[std::size_t(i)], a[std::size_t(k)] - a[std::size_t(i)]);
                const float l = glm::length(nn);
                if (l < 1e-3f) continue;
                nn /= l;
                float d = glm::dot(nn, a[std::size_t(i)]);
                int cPos = 0, cNeg = 0;
                for (const glm::vec3& p : a)
                {
                    const float s = glm::dot(nn, p) - d;
                    cPos += s > c_epsHull;
                    cNeg += s < -c_epsHull;
                }
                if (cPos && cNeg) continue;
                if (cPos) { nn = -nn; d = -d; }   // (the others behind: the normal outward)
                if (std::any_of(aplane.begin(), aplane.end(), [&](const PLANE& q) { return glm::dot(q.n, nn) > 0.9999f && std::fabs(q.d - d) < c_epsHull * 4; })) continue;
                aplane.push_back({nn, d});
            }
    if (aplane.size() < 4) return false;
    cv.apos = a;
    cv.aface.clear();
    for (const PLANE& pl : aplane)
    {
        std::vector<int> f;
        glm::vec3 c(0.0f);
        for (int i = 0; i < n; ++i)
            if (std::fabs(glm::dot(pl.n, a[std::size_t(i)]) - pl.d) <= c_epsHull * 4) { f.push_back(i); c += a[std::size_t(i)]; }
        if (f.size() < 3) continue;
        c /= float(f.size());
        const glm::vec3 u = glm::normalize(a[std::size_t(f[0])] - c), v = glm::cross(pl.n, u);
        std::sort(f.begin(), f.end(), [&](int x, int y) {
            const glm::vec3 px = a[std::size_t(x)] - c, py = a[std::size_t(y)] - c;
            return std::atan2(glm::dot(px, v), glm::dot(px, u)) < std::atan2(glm::dot(py, v), glm::dot(py, u));
        });
        cv.aface.push_back(f);
    }
    return cv.aface.size() >= 4;
}

glm::vec3 FaceNormal(const CONVEX& cv, const std::vector<int>& f)
{
    const glm::vec3 a = cv.apos[std::size_t(f[0])], b = cv.apos[std::size_t(f[1])], c = cv.apos[std::size_t(f[2])];
    return glm::normalize(glm::cross(b - a, c - a));
}

// the points of a piece on each side of a plane (n, d), with the crossing points of its edges
void SplitPoints(const CONVEX& cv, const glm::vec3& n, float d, std::vector<glm::vec3>& aA, std::vector<glm::vec3>& aB)
{
    for (const glm::vec3& p : cv.apos) (glm::dot(n, p) - d >= 0 ? aA : aB).push_back(p);
    for (const std::vector<int>& f : cv.aface)
        for (std::size_t i = 0; i < f.size(); ++i)
        {
            const glm::vec3 p = cv.apos[std::size_t(f[i])], q = cv.apos[std::size_t(f[(i + 1) % f.size()])];
            const float sp = glm::dot(n, p) - d, sq = glm::dot(n, q) - d;
            if ((sp > 0) != (sq > 0) && std::fabs(sp - sq) > 1e-4f)
            {
                const glm::vec3 x = p + (q - p) * (sp / (sp - sq));
                aA.push_back(x);
                aB.push_back(x);
            }
        }
}
}  // namespace

// ---- the brush's pieces

bool CEditor::FBrushMakeEditable(BRUSH& b, std::string& error)
{
    if (b.shape == 6) return true;
    md::Scene sc;
    std::vector<CONVEX> acv;
    if (!FBuildBrush(b, sc, acv, error)) return false;
    b.shape = 6;
    b.apiece = acv;
    b.afaceMat.assign(acv.size(), {});
    for (std::size_t i = 0; i < acv.size(); ++i) b.afaceMat[i].assign(acv[i].aface.size(), 0);
    return true;
}

void CEditor::BrushFixMats(BRUSH& b)
{
    b.afaceMat.resize(b.apiece.size());
    for (std::size_t i = 0; i < b.apiece.size(); ++i) b.afaceMat[i].resize(b.apiece[i].aface.size(), 0);
}

// a click on the selected brush: its face (and the nearest edge / point of it by the mode); false: not on it
bool CEditor::FBrushPick(float x, float y)
{
    BRUSH* pb = m_fBrushEdit ? PbrushOf(m_plSel) : nullptr;
    if (!pb || pb->shape != 6) return false;
    const glm::mat4 M = MatWorld(m_plSel);
    const glm::vec3 o = m_cam.pos, d = RayDir(x, y);
    float tBest = 1e30f;
    int ip = -1, iface = -1;
    glm::vec3 hitBest(0.0f);
    for (std::size_t i = 0; i < pb->apiece.size(); ++i)
    {
        const CONVEX& cv = pb->apiece[i];
        for (std::size_t j = 0; j < cv.aface.size(); ++j)
        {
            const std::vector<int>& f = cv.aface[j];
            std::vector<glm::vec3> aw;
            for (int k : f) aw.push_back(glm::vec3(M * glm::vec4(cv.apos[std::size_t(k)], 1.0f)));
            const glm::vec3 n = glm::normalize(glm::cross(aw[1] - aw[0], aw[2] - aw[0]));
            const float den = glm::dot(n, d);
            if (std::fabs(den) < 1e-6f) continue;
            const float t = glm::dot(n, aw[0] - o) / den;
            if (t <= 0 || t >= tBest) continue;
            const glm::vec3 h = o + d * t;
            bool fIn = true;
            for (std::size_t k = 0; k < aw.size() && fIn; ++k)
                fIn = glm::dot(glm::cross(aw[(k + 1) % aw.size()] - aw[k], h - aw[k]), n) >= -1e-2f;
            if (!fIn) continue;
            tBest = t;
            ip = int(i);
            iface = int(j);
            hitBest = h;
        }
    }
    if (ip < 0) return false;
    m_bsel = BRUSHSEL{ip, iface, -1, -1};
    const CONVEX& cv = pb->apiece[std::size_t(ip)];
    const std::vector<int>& f = cv.aface[std::size_t(iface)];
    const glm::vec3 hLocal = glm::vec3(glm::inverse(M) * glm::vec4(hitBest, 1.0f));
    if (m_brushMode == 2)
    {
        float dBest = 1e30f;
        for (int k : f)
            if (const float dd = glm::length(cv.apos[std::size_t(k)] - hLocal); dd < dBest) { dBest = dd; m_bsel.v0 = k; }
    }
    else if (m_brushMode == 1)
    {
        float dBest = 1e30f;
        for (std::size_t k = 0; k < f.size(); ++k)
        {
            const glm::vec3 a = cv.apos[std::size_t(f[k])], b = cv.apos[std::size_t(f[(k + 1) % f.size()])];
            const float u = std::clamp(glm::dot(hLocal - a, b - a) / std::max(1e-4f, glm::dot(b - a, b - a)), 0.0f, 1.0f);
            if (const float dd = glm::length(a + (b - a) * u - hLocal); dd < dBest) { dBest = dd; m_bsel.v0 = f[k]; m_bsel.v1 = f[(k + 1) % f.size()]; }
        }
    }
    return true;
}

bool CEditor::FBrushApply(BRUSH& b, const std::string& label)
{
    BrushFixMats(b);
    std::string error;
    if (!FRebuildBrush(b, error)) { Status(label + ": " + error); return false; }
    Status(label);
    return true;
}

bool CEditor::FBrushOp(int op, const glm::vec3& v, float g)
{
    BRUSH* pb = PbrushOf(m_plSel);
    if (!pb) { Status(T("select a brush first")); return false; }
    std::string error;
    if (pb->shape != 6 && !FBrushMakeEditable(*pb, error)) { Status(error); return false; }
    BRUSH& b = *pb;
    const BRUSHSEL s = m_bsel;
    const bool fPiece = s.ip >= 0 && s.ip < int(b.apiece.size());
    const bool fFace = fPiece && s.iface >= 0 && s.iface < int(b.apiece[std::size_t(s.ip)].aface.size());
    switch (op)
    {
    case 0:   // push / pull the face along its normal
    case 1:   // extrude it: a new piece
    {
        if (!fFace) { Status(T("pick a face first")); return false; }
        CONVEX& cv = b.apiece[std::size_t(s.ip)];
        const std::vector<int> f = cv.aface[std::size_t(s.iface)];
        const glm::vec3 n = FaceNormal(cv, f);
        if (op == 0)
        {
            CONVEX c2;
            std::vector<glm::vec3> a = cv.apos;
            for (int k : f) a[std::size_t(k)] += n * g;
            if (!FHull(a, c2)) { Status(T("the piece would be flat")); return false; }
            cv = c2;
            m_bsel.iface = -1;
            // (the moved face found again: the one whose normal matches, furthest along it)
            float best = -1e30f;
            for (std::size_t j = 0; j < cv.aface.size(); ++j)
                if (glm::dot(FaceNormal(cv, cv.aface[j]), n) > 0.999f)
                    if (const float dd = glm::dot(cv.apos[std::size_t(cv.aface[j][0])], n); dd > best) { best = dd; m_bsel.iface = int(j); }
            return FBrushApply(b, std::string(T("Push face")) + " " + std::to_string(int(g)));
        }
        std::vector<glm::vec3> a;
        for (int k : f) { a.push_back(cv.apos[std::size_t(k)]); a.push_back(cv.apos[std::size_t(k)] + n * g); }
        CONVEX c2;
        if (!FHull(a, c2)) { Status(T("the extrusion would be flat")); return false; }
        b.apiece.push_back(c2);
        m_bsel = BRUSHSEL{int(b.apiece.size()) - 1, -1, -1, -1};
        for (std::size_t j = 0; j < c2.aface.size(); ++j)
            if (glm::dot(FaceNormal(c2, c2.aface[j]), n) > 0.999f) m_bsel.iface = int(j);
        return FBrushApply(b, std::string(T("Extrude face")) + " " + std::to_string(int(g)));
    }
    case 2:   // move the point / the edge (the piece's hull again)
    {
        if (!fPiece || s.v0 < 0) { Status(T("pick a point or an edge first")); return false; }
        CONVEX& cv = b.apiece[std::size_t(s.ip)];
        std::vector<glm::vec3> a = cv.apos;
        const glm::vec3 p0 = a[std::size_t(s.v0)], p1 = s.v1 >= 0 ? a[std::size_t(s.v1)] : p0;
        a[std::size_t(s.v0)] += v;
        if (s.v1 >= 0) a[std::size_t(s.v1)] += v;
        CONVEX c2;
        if (!FHull(a, c2)) { Status(T("the piece would be flat")); return false; }
        cv = c2;
        // (the moved points found again)
        auto near = [&](const glm::vec3& p) { int ib = -1; float db = 1e30f; for (std::size_t i = 0; i < cv.apos.size(); ++i) if (const float dd = glm::length(cv.apos[i] - p); dd < db) { db = dd; ib = int(i); } return ib; };
        m_bsel.v0 = near(p0 + v);
        m_bsel.v1 = s.v1 >= 0 ? near(p1 + v) : -1;
        m_bsel.iface = -1;
        return FBrushApply(b, std::string(T(s.v1 >= 0 ? "Move edge" : "Move point")));
    }
    case 3:   // split the piece by the plane through its middle across axis g (0 x, 1 y, 2 z)
    {
        if (!fPiece) { Status(T("pick a face of the piece first")); return false; }
        const CONVEX cv = b.apiece[std::size_t(s.ip)];
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (const glm::vec3& p : cv.apos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
        glm::vec3 n(0.0f);
        n[std::clamp(int(g), 0, 2)] = 1.0f;
        const float d = glm::dot(n, (mn + mx) * 0.5f);
        std::vector<glm::vec3> aA, aB;
        SplitPoints(cv, n, d, aA, aB);
        CONVEX cA, cB;
        if (!FHull(aA, cA) || !FHull(aB, cB)) { Status(T("the piece cannot be split there")); return false; }
        b.apiece[std::size_t(s.ip)] = cA;
        b.apiece.insert(b.apiece.begin() + s.ip + 1, cB);
        b.afaceMat.insert(b.afaceMat.begin() + s.ip + 1, std::vector<int>());
        m_bsel = BRUSHSEL{s.ip, -1, -1, -1};
        return FBrushApply(b, T("Split piece"));
    }
    case 4:   // merge the piece with piece g (their hull)
    {
        const int iOther = int(g);
        if (!fPiece || iOther < 0 || iOther >= int(b.apiece.size()) || iOther == s.ip) { Status(T("pick a piece and the other piece's number")); return false; }
        std::vector<glm::vec3> a = b.apiece[std::size_t(s.ip)].apos;
        a.insert(a.end(), b.apiece[std::size_t(iOther)].apos.begin(), b.apiece[std::size_t(iOther)].apos.end());
        CONVEX c2;
        if (!FHull(a, c2)) return false;
        b.apiece[std::size_t(s.ip)] = c2;
        b.apiece.erase(b.apiece.begin() + iOther);
        if (iOther < int(b.afaceMat.size())) b.afaceMat.erase(b.afaceMat.begin() + iOther);
        if (std::size_t(s.ip) < b.afaceMat.size()) b.afaceMat[std::size_t(s.ip)].clear();
        m_bsel = BRUSHSEL{iOther < s.ip ? s.ip - 1 : s.ip, -1, -1, -1};
        return FBrushApply(b, T("Merge pieces"));
    }
    case 5:   // delete the piece
    {
        if (!fPiece || b.apiece.size() < 2) { Status(T("a brush keeps one piece at least")); return false; }
        b.apiece.erase(b.apiece.begin() + s.ip);
        if (std::size_t(s.ip) < b.afaceMat.size()) b.afaceMat.erase(b.afaceMat.begin() + s.ip);
        m_bsel = BRUSHSEL{};
        return FBrushApply(b, T("Delete piece"));
    }
    case 6:   // the face's material slot g
    {
        if (!fFace) { Status(T("pick a face first")); return false; }
        BrushFixMats(b);
        b.afaceMat[std::size_t(s.ip)][std::size_t(s.iface)] = std::max(0, int(g));
        return FBrushApply(b, T("Face texture"));
    }
    default: return false;
    }
}

// the selected brush's pieces drawn in the view while editing: its edges, the picked face / edge / point
void CEditor::DrawBrushEdit()
{
    BRUSH* pb = m_fBrushEdit ? PbrushOf(m_plSel) : nullptr;
    if (!pb || pb->shape != 6) return;
    const glm::mat4 M = MatWorld(m_plSel);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto proj = [&](const glm::vec3& p, ImVec2& s) { return FProject(glm::vec3(M * glm::vec4(p, 1.0f)), s); };
    for (std::size_t i = 0; i < pb->apiece.size(); ++i)
    {
        const CONVEX& cv = pb->apiece[i];
        const bool fPiece = int(i) == m_bsel.ip;
        for (std::size_t j = 0; j < cv.aface.size(); ++j)
        {
            const std::vector<int>& f = cv.aface[j];
            const bool fFace = fPiece && int(j) == m_bsel.iface;
            for (std::size_t k = 0; k < f.size(); ++k)
            {
                ImVec2 a, b;
                if (!proj(cv.apos[std::size_t(f[k])], a) || !proj(cv.apos[std::size_t(f[(k + 1) % f.size()])], b)) continue;
                const bool fEdge = fPiece && m_bsel.v1 >= 0 && ((f[k] == m_bsel.v0 && f[(k + 1) % f.size()] == m_bsel.v1) || (f[k] == m_bsel.v1 && f[(k + 1) % f.size()] == m_bsel.v0));
                dl->AddLine(a, b, st::u32(fEdge ? st::danger : fFace ? st::cta : fPiece ? st::accent : st::muted, fFace || fEdge ? 1.0f : 0.7f), fFace || fEdge ? 3.0f : 1.0f);
            }
        }
        if (fPiece && m_bsel.v0 >= 0 && m_bsel.v1 < 0)
        {
            ImVec2 s;
            if (std::size_t(m_bsel.v0) < cv.apos.size() && proj(cv.apos[std::size_t(m_bsel.v0)], s)) dl->AddCircleFilled(s, 6.0f, st::u32(st::danger));
        }
    }
}

// the Brush section's editing tools
void CEditor::DrawBrushEditTools(BRUSH& b)
{
    if (ImGui::Checkbox(T("Edit faces / edges / points"), &m_fBrushEdit) && m_fBrushEdit && b.shape != 6)
    {
        std::string error;
        if (!FBrushMakeEditable(b, error)) { Status(error); m_fBrushEdit = false; }
        else SaveBrushes();
    }
    EdTip(T("Click a face of the brush in the view (Face mode), its nearest edge (Edge) or point (Point). The brush becomes a mesh of convex pieces: its size fields go."));
    if (!m_fBrushEdit) return;
    ImGui::RadioButton(T("Face"), &m_brushMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(T("Edge"), &m_brushMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(T("Point"), &m_brushMode, 2);
    ImGui::TextColored(st::vec(st::muted), "%s %d / %d, %s %d", T("piece"), m_bsel.ip, int(b.apiece.size()), T("face"), m_bsel.iface);
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("##bdist", &m_brushDist, 4.0f, -4096.0f, 4096.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Push / pull face"))) FBrushOp(0, glm::vec3(0.0f), m_brushDist);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Extrude face"))) FBrushOp(1, glm::vec3(0.0f), m_brushDist);
    ImGui::SetNextItemWidth(200);
    ImGui::DragFloat3("##bmove", glm::value_ptr(m_brushMove), 4.0f, -4096.0f, 4096.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Move point / edge"))) FBrushOp(2, m_brushMove, 0.0f);
    if (ImGui::SmallButton(T("Split x"))) FBrushOp(3, glm::vec3(0.0f), 0);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Split y"))) FBrushOp(3, glm::vec3(0.0f), 1);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Split z"))) FBrushOp(3, glm::vec3(0.0f), 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##bother", &m_brushOther, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Merge with piece"))) FBrushOp(4, glm::vec3(0.0f), float(m_brushOther));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Delete piece"))) FBrushOp(5, glm::vec3(0.0f), 0);
    // the face's texture: slot 0 the brush's own, then the added ones
    ImGui::TextColored(st::vec(st::muted), "%s", T("Face texture:"));
    for (int i = 0; i <= int(b.amat.size()); ++i)
    {
        ImGui::SameLine();
        const std::string label = i == 0 ? std::string(T("the brush's")) : (b.amat[std::size_t(i - 1)].texFrom > 0 ? std::to_string(b.amat[std::size_t(i - 1)].texFrom) : std::string(T("colour")) + " " + std::to_string(i));
        ImGui::PushID(i);
        if (ImGui::SmallButton(label.c_str())) FBrushOp(6, glm::vec3(0.0f), float(i));
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("+ the last selected object's texture")) && m_asel.size() >= 1)
    {
        // (the object selected before the brush: Ctrl+click it, then the brush)
        LO* pt = m_asel.size() >= 2 ? m_asel[m_asel.size() - 2] : nullptr;
        if (pt) { b.amat.push_back({pt->oid, 0xB8B8B8}); FBrushOp(6, glm::vec3(0.0f), float(b.amat.size())); }
        else Status(T("Ctrl+click an object with the texture, then the brush"));
    }
}

// --editor-do verbs
bool CEditor::FBrushEditVerb(const std::string& verb, const std::string& arg)
{
    if (verb.rfind("b", 0) != 0) return false;
    auto floats = [&](float* a, int n) {
        std::stringstream ss(arg);
        std::string t;
        int i = 0;
        while (i < n && std::getline(ss, t, ',')) a[i++] = float(std::atof(t.c_str()));
        return i;
    };
    if (verb == "bedit") { m_fBrushEdit = true; BRUSH* pb = PbrushOf(m_plSel); std::string e; if (pb && FBrushMakeEditable(*pb, e)) SaveBrushes(); m_brushMode = std::atoi(arg.c_str()); return true; }
    if (verb == "bface")   // bface:PIECE,FACE (or bface:top: the highest +z face)
    {
        BRUSH* pb = PbrushOf(m_plSel);
        if (!pb) return true;
        if (arg == "top")
        {
            float best = -1e30f;
            for (std::size_t i = 0; i < pb->apiece.size(); ++i)
                for (std::size_t j = 0; j < pb->apiece[i].aface.size(); ++j)
                    if (glm::dot(FaceNormal(pb->apiece[i], pb->apiece[i].aface[j]), glm::vec3(0, 0, 1)) > 0.99f)
                        if (const float z = pb->apiece[i].apos[std::size_t(pb->apiece[i].aface[j][0])].z; z > best) { best = z; m_bsel = BRUSHSEL{int(i), int(j), -1, -1}; }
            return true;
        }
        float a[2] = {0, 0};
        floats(a, 2);
        m_bsel = BRUSHSEL{int(a[0]), int(a[1]), -1, -1};
        return true;
    }
    if (verb == "bpoint") { float a[2] = {0, 0}; floats(a, 2); m_bsel = BRUSHSEL{int(a[0]), -1, int(a[1]), -1}; return true; }   // bpoint:PIECE,POINT
    if (verb == "bpick") { float a[2] = {0, 0}; floats(a, 2); m_fBrushEdit = true; if (!FBrushPick(a[0], a[1])) Status("bpick: not on the brush"); return true; }
    if (verb == "bpush" || verb == "bextrude") { FBrushOp(verb == "bpush" ? 0 : 1, glm::vec3(0.0f), float(std::atof(arg.c_str()))); return true; }
    if (verb == "bmove") { float a[3] = {0, 0, 0}; floats(a, 3); FBrushOp(2, glm::vec3(a[0], a[1], a[2]), 0); return true; }
    if (verb == "bsplit") { FBrushOp(3, glm::vec3(0.0f), float(std::atoi(arg.c_str()))); return true; }
    if (verb == "bmerge") { FBrushOp(4, glm::vec3(0.0f), float(std::atoi(arg.c_str()))); return true; }
    if (verb == "bdelete") { FBrushOp(5, glm::vec3(0.0f), 0); return true; }
    if (verb == "bmat")   // bmat:RGBHEX: a colour slot added and given to the picked face
    {
        BRUSH* pb = PbrushOf(m_plSel);
        if (!pb) return true;
        pb->amat.push_back({0, uint32_t(std::strtoul(arg.c_str(), nullptr, 16))});
        FBrushOp(6, glm::vec3(0.0f), float(pb->amat.size()));
        return true;
    }
    if (verb == "bsel") { if (!m_abrush.empty()) SelectOnly(PloOid(m_abrush.back().oid)); return true; }   // the last brush made
    if (verb == "bprint")
    {
        BRUSH* pb = PbrushOf(m_plSel);
        if (!pb) return true;
        std::printf("editor: brush %d mesh %d: %d pieces, %d materials\n", pb->oid, pb->shape == 6 ? 1 : 0, int(pb->apiece.size()), int(pb->amat.size()) + 1);
        for (std::size_t i = 0; i < pb->apiece.size(); ++i)
        {
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const glm::vec3& p : pb->apiece[i].apos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
            std::printf("editor: brush piece %d: %d points %d faces, %.0f %.0f %.0f .. %.0f %.0f %.0f\n", int(i), int(pb->apiece[i].apos.size()), int(pb->apiece[i].aface.size()),
                        mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
        }
        return true;
    }
    return false;
}

}  // namespace edimpl
#else
namespace edimpl
{
bool CEditor::FBrushEditVerb(const std::string&, const std::string&) { return false; }
bool CEditor::FBrushPick(float, float) { return false; }
void CEditor::DrawBrushEdit() {}
}  // namespace edimpl
#endif
