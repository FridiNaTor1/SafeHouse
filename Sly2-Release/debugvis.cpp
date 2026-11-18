// See debugvis.h. PC debug overlays: read-only views of the world drawn as 2D lines and text over the picture (the HUD's
// own primitives, so both renderers and every aspect draw them alike).
#include "debugvis.h"
#include "debugmenu.h"

#include "activate.h"
#include "ai.h"
#include "ainoise.h"
#include "alo.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "cmworld.h"
#include "crv.h"
#include "detect.h"
#include "display.h"
#include "gl.h"
#include "guard.h"
#include "hud.h"
#include "lo.h"
#include "pcinput.h"
#include "po.h"
#include "render.h"
#include "scpt.h"
#include "screenrt.h"
#include "snd989.h"
#include "so.h"
#include "splicenames.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "vr.h"
#include "worldaccess.h"

#include "../editor/game/editor_objtypes.h"
#include "../editor/game/editor_names2.h"
#include "../platform/platform.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

std::string StrLocalized(const SW* psw, int id);   // splayer.cpp
float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

void (*g_pfnDebugVisSpliceEvent)(LO* plo, uint32_t symid, bool fDeferred) = nullptr;

namespace
{
using namespace edimpl;

// ------------------------------------------------------------ the toggles
const char* const s_apchzName[DV_Max] = {"Hidden Objects", "Guards (Paths, Vision, Noises)", "Trigger Zones", "Traversal Markers",
                                         "Collision (Wireframe)", "Camera Zones and Targets", "Labels (oid, Class, Name)", "Stats",
                                         "Mission, Scripts and Events"};
const char* const s_apchzKey[DV_Max] = {"hidden", "guards", "triggers", "traversal", "collision", "cameras", "labels", "stats", "mission"};
const float s_asLabelRange[] = {500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f, 0.0f};   // (0: no limit)
const float s_asCollisionRange[] = {500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};
constexpr int CLABELRANGE = int(sizeof(s_asLabelRange) / sizeof(s_asLabelRange[0]));
constexpr int CCOLLRANGE = int(sizeof(s_asCollisionRange) / sizeof(s_asCollisionRange[0]));

struct SETTINGS
{
    bool af[DV_Max] = {};
    int iLabelRange = 2;      // 2000
    bool fLabelsCursor = false;
    int iCollisionRange = 2;  // 2000
};
SETTINGS s_set;
bool s_afEnv[DV_Max] = {};    // SLY2N_DEBUGVIS
bool s_fLoaded = false;

std::filesystem::path PathIni() { return cane::userDirectory() / "Debug Overlays.ini"; }

void Load()
{
    if (s_fLoaded) return;
    s_fLoaded = true;
    if (const char* psz = std::getenv("SLY2N_DEBUGVIS"))
    {
        const std::string s = psz;
        for (int k = 0; k < DV_Max; ++k)
            s_afEnv[k] = s == "all" || ("," + s + ",").find("," + std::string(s_apchzKey[k]) + ",") != std::string::npos;
        if (const char* pszCur = std::getenv("SLY2N_DEBUGVISCURSOR")) s_set.fLabelsCursor = std::atoi(pszCur) != 0;
        if (const char* pszRange = std::getenv("SLY2N_DEBUGVISRANGE")) s_set.iLabelRange = std::clamp(std::atoi(pszRange), 0, CLABELRANGE - 1);
        std::printf("debug overlays: %s (SLY2N_DEBUGVIS)\n", psz);
        return;   // (a test run: the user's .ini neither read nor written)
    }
    std::error_code ec;
    if (!std::filesystem::exists(PathIni(), ec)) return;
    std::ifstream in(PathIni());
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        auto trim = [](std::string& str) {
            while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.pop_back();
            while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.erase(str.begin());
        };
        trim(key);
        trim(value);
        const int n = std::atoi(value.c_str());
        for (int k = 0; k < DV_Max; ++k)
            if (key == s_apchzKey[k]) s_set.af[k] = n != 0;
        if (key == "label_range") s_set.iLabelRange = std::clamp(n, 0, CLABELRANGE - 1);
        if (key == "labels_cursor") s_set.fLabelsCursor = n != 0;
        if (key == "collision_range") s_set.iCollisionRange = std::clamp(n, 0, CCOLLRANGE - 1);
    }
}

void Save()
{
    if (std::getenv("SLY2N_DEBUGVIS")) return;
    std::ofstream out(PathIni());
    if (!out) return;
    out << "[Debug Overlays]\n";
    for (int k = 0; k < DV_Max; ++k) out << s_apchzKey[k] << "=" << (s_set.af[k] ? 1 : 0) << "\n";
    out << "label_range=" << s_set.iLabelRange << "\nlabels_cursor=" << (s_set.fLabelsCursor ? 1 : 0)
        << "\ncollision_range=" << s_set.iCollisionRange << "\n";
}

// ------------------------------------------------------------ the frame's camera and times
glm::mat4 s_matWorldToClip(1.0f);
glm::vec3 s_posCam(0.0f), s_dirCam(1.0f, 0.0f, 0.0f);
bool s_fWorldView = false;
double s_amsFrame[3] = {};   // update / draw / swap (smoothed)

// ------------------------------------------------------------ drawing
constexpr float DEPTH_TEXT = 16760000.0f - 4000.0f;          // (under the debug menu, over the HUD)
constexpr float Z_LINE = (16760000.0f - 6000.0f) / 16777216.0f;
constexpr float Z_PANEL = (16760000.0f - 8000.0f) / 16777216.0f;

const FONTBRX* s_pfont = nullptr;
const FONTBRX* s_picons = nullptr;
int s_cline = 0;                          // the lines drawn this frame (a budget against huge worlds)
constexpr int CLINE_MAX = 60000;

glm::vec4 Col(uint32_t rgb, float a = 1.0f)
{
    // (GS units: 0x80 = 1)
    return glm::vec4(float((rgb >> 16) & 0xFF) * 0.5f, float((rgb >> 8) & 0xFF) * 0.5f, float(rgb & 0xFF) * 0.5f, 128.0f * a);
}

// canvas pixels (640 x 448) of an NDC point / the reverse
glm::vec2 CanvasOfNdc(glm::vec2 ndc)
{
    const DISPLAY& d = g_display;
    return glm::vec2(((ndc.x - d.uiOffset.x) / d.uiScale.x + 1.0f) * 320.0f, (1.0f - (ndc.y - d.uiOffset.y) / d.uiScale.y) * 224.0f);
}

bool FProject(const glm::vec3& p, glm::vec2* ps)
{
    const glm::vec4 c = s_matWorldToClip * glm::vec4(p, 1.0f);
    if (c.w <= 1.0f) return false;
    *ps = CanvasOfNdc(glm::vec2(c.x, c.y) / c.w);
    return std::fabs(ps->x) < 1e5f && std::fabs(ps->y) < 1e5f;
}

// A world line (cut where it passes behind the camera)
void Line3(const glm::vec3& a, const glm::vec3& b, uint32_t rgb, float w = 0.6f, float alpha = 0.9f)
{
    if (s_cline >= CLINE_MAX) return;
    glm::vec4 ca = s_matWorldToClip * glm::vec4(a, 1.0f), cb = s_matWorldToClip * glm::vec4(b, 1.0f);
    constexpr float W_MIN = 1.0f;
    if (ca.w < W_MIN && cb.w < W_MIN) return;
    if (ca.w < W_MIN) ca = cb + (ca - cb) * ((cb.w - W_MIN) / (cb.w - ca.w));
    else if (cb.w < W_MIN) cb = ca + (cb - ca) * ((ca.w - W_MIN) / (ca.w - cb.w));
    const glm::vec2 sa = CanvasOfNdc(glm::vec2(ca) / ca.w), sb = CanvasOfNdc(glm::vec2(cb) / cb.w);
    // (wholly off the picture: nothing)
    const float xl = -320.0f / g_display.uiScale.x + 320.0f - 4.0f, xr = 320.0f / g_display.uiScale.x + 320.0f + 4.0f;
    const float yt = -224.0f / g_display.uiScale.y + 224.0f - 4.0f, yb = 224.0f / g_display.uiScale.y + 224.0f + 4.0f;
    if ((sa.x < xl && sb.x < xl) || (sa.x > xr && sb.x > xr) || (sa.y < yt && sb.y < yt) || (sa.y > yb && sb.y > yb)) return;
    g_hud.FillScreenLine(sa, sb, w, Col(rgb, alpha), Z_LINE);
    ++s_cline;
}

void Cross3(const glm::vec3& p, float s, uint32_t rgb, float w = 0.6f)
{
    Line3(p - glm::vec3(s, 0, 0), p + glm::vec3(s, 0, 0), rgb, w);
    Line3(p - glm::vec3(0, s, 0), p + glm::vec3(0, s, 0), rgb, w);
    Line3(p - glm::vec3(0, 0, s), p + glm::vec3(0, 0, s), rgb, w);
}

void Circle3(const glm::vec3& c, float r, uint32_t rgb, float w = 0.6f, int cseg = 24)
{
    glm::vec3 prev = c + glm::vec3(r, 0, 0);
    for (int i = 1; i <= cseg; ++i)
    {
        const float a = 6.2831853f * float(i) / float(cseg);
        const glm::vec3 p = c + glm::vec3(std::cos(a) * r, std::sin(a) * r, 0.0f);
        Line3(prev, p, rgb, w);
        prev = p;
    }
}

// a box: the 8 corners of m * [-1, 1]^3
void Box3(const glm::mat4& m, uint32_t rgb, float w = 0.6f, float alpha = 0.9f)
{
    glm::vec3 ap[8];
    for (int i = 0; i < 8; ++i) ap[i] = glm::vec3(m * glm::vec4(i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, i & 4 ? 1.0f : -1.0f, 1.0f));
    static const int s_aedge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : s_aedge) Line3(ap[e[0]], ap[e[1]], rgb, w, alpha);
}

// a geometry's edges through m
void Geom3(const GEOM& geom, const glm::mat4& m, uint32_t rgb, float w = 0.6f, float alpha = 0.9f, float sRange = 0.0f)
{
    if (geom.apos.empty()) return;
    std::vector<glm::vec3> ap(geom.apos.size());
    for (size_t i = 0; i < ap.size(); ++i) ap[i] = glm::vec3(m * glm::vec4(geom.apos[i], 1.0f));
    for (const EDGE& e : geom.aedge)
    {
        if (e.ipos0 >= ap.size() || e.ipos1 >= ap.size()) continue;
        const glm::vec3& a = ap[e.ipos0];
        const glm::vec3& b = ap[e.ipos1];
        if (sRange > 0.0f)
        {
            // (the segment's nearest point to the camera: long floor edges count where they pass by)
            const glm::vec3 d = b - a;
            const float dd = glm::dot(d, d);
            const float t = dd > 1e-6f ? std::clamp(glm::dot(s_posCam - a, d) / dd, 0.0f, 1.0f) : 0.0f;
            if (glm::length(a + d * t - s_posCam) > sRange) continue;
        }
        Line3(a, b, rgb, w, alpha);
    }
}

// text at canvas pixels (x left, y top), colour as rgb
void Text(const std::string& s, float x, float y, float size, uint32_t rgb)
{
    if (!s_pfont) return;
    const glm::vec4 off(float((rgb >> 16) & 0xFF) * 0.5f, float((rgb >> 8) & 0xFF) * 0.5f, float(rgb & 0xFF) * 0.5f, 0.0f);
    g_hud.DrawTextBox(*s_pfont, s_picons, s, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH_TEXT, false);
}
float DxText(const std::string& s, float size) { return s_pfont ? DxTextUi(*s_pfont, s_picons, s, size / 51.2f) : 0.0f; }

// a world label: text centred over the point, a dark box behind it
void Label3(const glm::vec3& p, const std::string& s, uint32_t rgb, float size = 11.0f)
{
    glm::vec2 c;
    if (!FProject(p, &c)) return;
    const float w = DxText(s, size);
    const float x = c.x - w * 0.5f, y = c.y - size - 4.0f;
    g_hud.FillScreenRect(x - 2.0f, y - 1.0f, x + w + 2.0f, y + size * 0.95f + 2.0f, glm::vec4(0, 0, 0, 64), Z_PANEL);
    Text(s, x, y, size, rgb);
}

// the HUD Placement box's left / right edges (canvas pixels)
float XBoxLeft() { return std::max(-DxHudPlacement(), -320.0f / g_display.uiScale.x + 320.0f) + 10.0f; }
float XBoxRight() { return std::min(640.0f + DxHudPlacement(), 320.0f / g_display.uiScale.x + 320.0f) - 10.0f; }

// a panel of text lines: at (x, y) left aligned, or right aligned to x when fRight
struct PANELLINE { std::string s; uint32_t rgb; };
float Panel(const std::vector<PANELLINE>& al, float x, float y, bool fRight, float size = 11.0f)
{
    float w = 0.0f;
    for (const auto& l : al) w = std::max(w, DxText(l.s, size));
    const float dy = size + 2.0f;
    const float x0 = fRight ? x - w - 8.0f : x;
    const float h = dy * float(al.size()) + 6.0f;
    g_hud.FillScreenRect(x0, y, x0 + w + 8.0f, y + h, glm::vec4(0, 0, 0, 84), Z_PANEL);
    for (size_t i = 0; i < al.size(); ++i) Text(al[i].s, x0 + 4.0f, y + 3.0f + dy * float(i), size, al[i].rgb);
    return y + h;
}

// ------------------------------------------------------------ the world's objects (read only)
uint32_t s_serial = 0;                                  // the world the caches below are of
std::unordered_map<OID, std::vector<LO*>> s_mpoid;      // (built from aploAll: the game's own lazy index is not touched)
size_t s_cploIndexed = 0;
struct CRVCACHE { glm::mat4 mat{0.0f}; std::vector<glm::vec3> ap; };
std::unordered_map<const LO*, CRVCACHE> s_mpcrv;
std::unordered_map<const LO*, OBJTYPE> s_mptype;

void SyncWorld(SW* psw)
{
    if (psw->serial != s_serial)
    {
        s_serial = psw->serial;
        s_mpoid.clear();
        s_cploIndexed = 0;
        s_mpcrv.clear();
        s_mptype.clear();
    }
    for (; s_cploIndexed < psw->aploAll.size(); ++s_cploIndexed)
        if (LO* plo = psw->aploAll[s_cploIndexed].get()) s_mpoid[plo->oid].push_back(plo);
}

LO* PloOid(OID oid, const LO* ploNear)
{
    const auto it = s_mpoid.find(oid);
    if (it == s_mpoid.end() || it->second.empty()) return nullptr;
    if (ploNear)
        for (LO* p : it->second)
            if (p->oidInstance == ploNear->oidInstance) return p;
    return it->second.front();
}

std::string StrGroup(const SW* psw, const LO* plo)
{
    return plo->igrp >= 0 && plo->igrp < int(psw->awe.size()) ? psw->awe[size_t(plo->igrp)].StrName() : std::string();
}
bool FLevelGroup(const SW* psw, const LO* plo) { return plo->igrp >= 0 && plo->igrp < int(psw->awe.size()) && psw->awe[size_t(plo->igrp)].x5C == 0; }

OBJTYPE OtOf(const SW* psw, const LO* plo)
{
    if (const auto it = s_mptype.find(plo); it != s_mptype.end()) return it->second;
    const OBJTYPE ot = OtOfLo(plo, Lower(StrGroup(psw, plo)), FLevelGroup(psw, plo));
    s_mptype[plo] = ot;
    return ot;
}

const char* SzClassName(int cid)
{
    static char s_ach[16];
    if (cid >= 0 && cid < CID_Max && g_acls[cid].name && *g_acls[cid].name && g_acls[cid].name[0] != 'C') return g_acls[cid].name;
    if (const char* sz = SzClassName2(cid)) return sz;
    std::snprintf(s_ach, sizeof s_ach, "CID_%02X", cid);
    return s_ach;
}

// the editor's readable name (editor_types.inc NameOfObject, without the editor's own data): a job's text, an
// instance's group as words, else the type
std::string StrReadable(SW* psw, LO* plo, OBJTYPE ot)
{
    if (ot == OT_Job)
        if (JOB* pjob = dynamic_cast<JOB*>(plo); pjob && pjob->n6c != -1)
            if (std::string s = StrLocalized(psw, pjob->n6c); !s.empty() && s.size() < 48) return s;
    const LO* root = plo;
    while (root->paloParent) root = root->paloParent;
    if (!FLevelGroup(psw, plo) && root == plo)
        if (const std::string g = StrGroup(psw, plo); !g.empty())
        {
            if (FIsLoDerivedFrom(plo, 0x30)) return PrettyGroupName(g);
            return std::string(InfoOfType(ot).name) + " (" + PrettyGroupName(g) + ")";
        }
    return InfoOfType(ot).name;
}

bool FLocated(const LO* plo) { return FIsLoDerivedFrom(plo, 0x30) || FCidDerivedFrom(plo->Cid(), 3); }
glm::vec3 PosOfLo(const LO* plo) { return PosLoWorld(plo); }

// a path's curve as the game evaluates it (crv.h), from a copy: the game's own cached curve is never built or touched
const std::vector<glm::vec3>* PapCurve(LO* plo)
{
    if (!plo || !FIsLoDerivedFrom(plo, 0x0A)) return nullptr;
    const LOXPATH* ppath = static_cast<const LOXPATH*>(plo);
    if (ppath->avecF4.size() < 2) return nullptr;
    const glm::mat4 mat = MatWorldLox(plo);
    CRVCACHE& cc = s_mpcrv[plo];
    if (cc.mat == mat && !cc.ap.empty()) return &cc.ap;
    cc.mat = mat;
    cc.ap.clear();
    CRV c;
    c.ppath = ppath;
    c.crvk = ppath->fE0;
    c.fClosed = ppath->nE4;
    c.ag.resize(ppath->an.size());
    for (size_t i = 0; i < ppath->an.size(); ++i) std::memcpy(&c.ag[i], &ppath->an[i], 4);
    c.apos = ppath->avecF4;
    c.avIn = ppath->avec140;
    c.avOut = ppath->avec144;
    if (c.crvk && (c.avIn.size() != c.apos.size() || c.avOut.size() != c.apos.size())) c.crvk = 0;
    c.iseg = 1;
    if (c.ag.size() >= 2 && c.ag.size() == c.apos.size())
    {
        RecalcCrv(&c, mat);
        const float g0 = c.ag.front(), g1 = c.ag.back();
        const int n = std::clamp(int(c.apos.size()) * (c.crvk ? 8 : 1), 2, 400);
        for (int i = 0; i <= n; ++i)
        {
            glm::vec3 p;
            EvalCrv(&c, g0 + (g1 - g0) * float(i) / float(n), &p, nullptr);
            cc.ap.push_back(p);
        }
    }
    else
        for (const glm::vec3& q : ppath->avecF4) cc.ap.push_back(glm::vec3(mat * glm::vec4(q, 1.0f)));
    if (c.fClosed && !cc.ap.empty()) cc.ap.push_back(cc.ap.front());
    return &cc.ap;
}

void DrawCurve(LO* plo, uint32_t rgb, float w, bool fArrows)
{
    const std::vector<glm::vec3>* pap = PapCurve(plo);
    if (!pap || pap->size() < 2) return;
    const std::vector<glm::vec3>& ap = *pap;
    for (size_t i = 1; i < ap.size(); ++i)
    {
        Line3(ap[i - 1], ap[i], rgb, w);
        // (an arrow head now and then: the way it runs)
        if (fArrows && i % std::max<size_t>(1, ap.size() / 5) == 0)
        {
            const glm::vec3 d = ap[i] - ap[i - 1];
            const float l = glm::length(d);
            if (l < 1e-3f) continue;
            const glm::vec3 u = d / l, side = glm::normalize(glm::cross(u, glm::vec3(0, 0, 1)) + glm::vec3(1e-6f));
            const glm::vec3 m = (ap[i] + ap[i - 1]) * 0.5f;
            const float s = std::min(30.0f, l * 0.4f);
            Line3(m, m - u * s + side * s * 0.6f, rgb, w);
            Line3(m, m - u * s - side * s * 0.6f, rgb, w);
        }
    }
}

bool FNear(const glm::vec3& p, float sRange) { return sRange <= 0.0f || glm::length(p - s_posCam) <= sRange; }

// ------------------------------------------------------------ Hidden Objects
void DrawHidden(SW* psw)
{
    constexpr float RANGE = 12000.0f;
    for (LO* plo : AploOfClass(psw, 0x30))
    {
        ALO* palo = static_cast<ALO*>(plo);
        if (!palo->pglobset || palo->pglobset->aglob.empty()) continue;
        const char* pchzWhy = nullptr;
        uint32_t rgb = 0;
        if (palo->fRemoved) pchzWhy = "removed", rgb = 0xFF40FF;
        else if (!palo->fInWorld) pchzWhy = "not in the world", rgb = 0xC040FF;
        else if (palo->gFade <= 0.001f) pchzWhy = "faded out", rgb = 0xFF80C0;
        else
        {
            const LO* root = palo;
            while (root->paloParent) root = root->paloParent;
            if (!root->fNodeActive) pchzWhy = "inactive", rgb = 0x6080FF;
        }
        if (!pchzWhy) continue;
        const glm::mat4 m = MatWorldOfAlo(palo);
        const float r = std::max(palo->pglobset->sRadius, 15.0f);
        const glm::vec3 c = glm::vec3(m * glm::vec4(palo->pglobset->posCenter, 1.0f));
        if (!FNear(c, RANGE)) continue;
        // (a box of the mesh's bound, turned as the object)
        glm::mat4 mBox = m;
        mBox[3] = glm::vec4(c, 1.0f);
        const float sCol = std::max(1e-4f, glm::length(glm::vec3(m[0])));
        mBox = mBox * glm::mat4(glm::mat3(r * 0.7f / sCol));
        Box3(mBox, rgb, 0.7f, 0.85f);
        if (glm::length(c - s_posCam) < 3000.0f) Label3(c, std::to_string(plo->oid) + " " + pchzWhy, rgb, 10.0f);
    }
}

// ------------------------------------------------------------ Guards
void DrawGuards(SW* psw)
{
    for (LO* plo : AploOfClass(psw, 0x38))
    {
        if (plo->fRemoved) continue;
        GUARD* pg = PguardFromLo(plo);
        if (!pg) continue;
        const glm::vec3 pos = PosLoWorld(plo);
        if (!FNear(pos, 20000.0f)) continue;
        // the patrol: its paths, go-to points and look targets
        if (const CMDLIST* pl = pg->pcmdlistPatrol)
        {
            glm::vec3 posPrev = pos;
            for (const auto& up : pl->acmd)
            {
                const CMD& c = *up;
                if (c.op == 6 && c.oid > 0)
                {
                    if (LO* pp = PloOid(c.oid, plo))
                    {
                        DrawCurve(pp, 0xF0506E, 0.8f, true);
                        if (const auto* pap = PapCurve(pp); pap && !pap->empty()) posPrev = pap->back();
                    }
                }
                else if (c.op == 7)
                {
                    glm::vec3 p;
                    bool f = false;
                    if (c.oid <= 0 && c.ppos8) p = glm::vec3(*c.ppos8), f = true;
                    else if (LO* pt = c.oid > 0 ? PloOid(c.oid, plo) : nullptr; pt && FLocated(pt)) p = PosOfLo(pt), f = true;
                    if (f)
                    {
                        Cross3(p, 25.0f, 0xFF9060);
                        Line3(posPrev, p, 0xFF9060, 0.5f, 0.5f);
                        posPrev = p;
                    }
                }
                else if ((c.op == 3 || c.op == 0) && c.oid > 0)
                {
                    if (LO* pt = PloOid(c.oid, plo); pt && FLocated(pt))
                    {
                        const glm::vec3 p = PosOfLo(pt);
                        Line3(posPrev + glm::vec3(0, 0, 60), p, 0xFFE070, 0.4f, 0.6f);
                        Cross3(p, 15.0f, 0xFFE070, 0.5f);
                    }
                }
            }
        }
        // its home
        if (pg->radHome > 1.0f) Circle3(glm::vec3(pg->posHome), pg->radHome, 0x8060A0, 0.5f, 32);
        // its view volumes, coloured by what it sees (detector: 1 seen, 2 lost)
        uint32_t rgbView = 0x60E060;
        std::string strSee = "";
        if (DETECTOR* pdet = pg->pdet)
        {
            int st = 0;
            for (const DETENTRY& e : pdet->ae) st = std::max(st, e.st == 1 ? 2 : e.st == 2 ? 1 : 0);
            rgbView = st == 2 ? 0xFF3030 : st == 1 ? 0xFFA020 : 0x60E060;
            strSee = st == 2 ? "SEES" : st == 1 ? "lost" : "";
            if (pdet->fNever) rgbView = 0x707070, strSee = "blind";
            for (LO* pv : {pdet->ploVolume, pdet->ploVolume2})
                if (pv && FCidDerivedFrom(pv->Cid(), 0x16)) Geom3(static_cast<LOXSHAPE*>(pv)->geom, MatWorldLox(pv), rgbView, 0.6f, 0.7f);
            const glm::vec3 eye = PosDetectorEye(pdet);
            Cross3(eye, 10.0f, rgbView, 0.8f);
        }
        // the job it runs now (the deepest of its AI stack)
        std::string strJob;
        if (!pg->stack.ajob.empty() && pg->stack.ajob.back()) strJob = SzClassName(pg->stack.ajob.back()->cid);
        std::string s = "guard " + std::to_string(plo->oid);
        if (!strJob.empty()) s += " (AI " + strJob + ")";
        if (!strSee.empty()) s += " " + strSee;
        if (glm::length(pos - s_posCam) < 6000.0f) Label3(pos + glm::vec3(0, 0, 200), s, rgbView, 10.0f);
    }
    // the noises the guards may hear (their reach: the radius, growing over the life)
    for (NOISE* pn : ApnoiseAll())
    {
        if (!pn || !FNoiseLive(pn) || !pn->ptmpl) continue;
        const float r = SRadiusNoise(pn);
        if (r <= 1.0f || !FNear(pn->pos20, 15000.0f)) continue;
        Circle3(pn->pos20, r, 0x40C0FF, 0.6f, 32);
        Cross3(pn->pos20, 15.0f, 0x40C0FF);
        Label3(pn->pos20, "noise " + std::to_string(pn->nKind), 0x40C0FF, 10.0f);
    }
}

// ------------------------------------------------------------ Trigger Zones / Camera Zones / Traversal / Collision
// a located object's own shape (LOXSHAPE / LOXGEOM edges, LOXMESH triangles); false: none
bool FDrawShape(LO* plo, uint32_t rgb, float alpha)
{
    if (FCidDerivedFrom(plo->Cid(), 0x16))
    {
        const LOXSHAPE* ps = static_cast<const LOXSHAPE*>(plo);
        if (ps->geom.apos.empty()) return false;
        Geom3(ps->geom, MatWorldLox(plo), rgb, 0.6f, alpha);
        return true;
    }
    const int cid = plo->Cid();
    if (cid == 0x05 || cid == 0x0F)
    {
        const LOXGEOM* pg = static_cast<const LOXGEOM*>(plo);
        if (pg->geom.apos.empty()) return false;
        Geom3(pg->geom, MatWorldLox(plo), rgb, 0.6f, alpha);
        return true;
    }
    if (cid == 0x11)
    {
        const LOXMESH* pm = static_cast<const LOXMESH*>(plo);
        if (pm->avtx.empty()) return false;
        const glm::mat4 m = MatWorldLox(plo);
        for (const auto& t : pm->atri)
            for (int k = 0; k < 3; ++k)
            {
                const int a = t[size_t(k)], b = t[size_t((k + 1) % 3)];
                if (a >= 0 && b >= 0 && size_t(a) < pm->avtx.size() && size_t(b) < pm->avtx.size())
                    Line3(glm::vec3(m * glm::vec4(pm->avtx[size_t(a)].pos, 1.0f)), glm::vec3(m * glm::vec4(pm->avtx[size_t(b)].pos, 1.0f)), rgb, 0.6f, alpha);
            }
        return true;
    }
    return false;
}

void DrawTriggers(SW* psw)
{
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        const OBJTYPE ot = OtOf(psw, plo);
        if (ot != OT_Trigger && ot != OT_Volume && ot != OT_Door && ot != OT_Warp && ot != OT_Sensor && ot != OT_Water) continue;
        if (!FLocated(plo)) continue;
        const glm::vec3 pos = PosOfLo(plo);
        if (!FNear(pos, 20000.0f)) continue;
        // (removed / out of the world: dim)
        const float alpha = plo->fRemoved || !plo->fInWorld ? 0.3f : 0.85f;
        const uint32_t rgb = InfoOfType(ot).rgb;
        const char* pchzKind = ot == OT_Trigger ? "trigger" : ot == OT_Door ? (plo->Cid() == 0x1A ? "exit" : "barrier")
                             : ot == OT_Warp ? "warp" : ot == OT_Sensor ? "sensor" : ot == OT_Water ? "water" : "volume";
        if (!FDrawShape(plo, rgb, alpha)) Cross3(pos, 30.0f, rgb, 0.8f);
        if (glm::length(pos - s_posCam) < 5000.0f)
            Label3(pos, std::string(pchzKind) + " " + std::to_string(plo->oid) + (alpha < 0.5f ? " (off)" : ""), rgb, 10.0f);
    }
}

void DrawCameras(SW* psw)
{
    for (LO* plo : AploOfClass(psw, 0x0F))
    {
        if (!FNear(PosOfLo(plo), 30000.0f)) continue;
        FDrawShape(plo, 0xB0C8F0, plo->fRemoved ? 0.3f : 0.75f);
        if (glm::length(PosOfLo(plo) - s_posCam) < 6000.0f) Label3(PosOfLo(plo), "camera zone " + std::to_string(plo->oid), 0xB0C8F0, 10.0f);
    }
    for (LO* plo : AploOfClass(psw, 0x2C))
    {
        const glm::vec3 p = PosOfLo(plo);
        if (!FNear(p, 20000.0f)) continue;
        Cross3(p, 40.0f, 0xFFFFFF, 0.8f);
        Label3(p, "camera target " + std::to_string(plo->oid), 0xFFFFFF, 10.0f);
    }
    // the game camera's focus objects (its policy stack) and, from a free camera, the game camera itself
    for (int i = 0; i < g_cm.ccpr && i < int(g_cm.acpr.size()); ++i)
        if (LO* pf = g_cm.acpr[size_t(i)].psoFocus; pf && FLocated(pf))
        {
            const glm::vec3 p = PosOfLo(pf);
            Cross3(p, 50.0f, 0xFFD040, 1.0f);
            if (g_fDebugFreeCam) Line3(g_cm.pos, p, 0xFFD040, 0.5f, 0.6f);
        }
    if (g_fDebugFreeCam)
    {
        Cross3(g_cm.pos, 40.0f, 0xFFD040, 1.0f);
        Line3(g_cm.pos, g_cm.pos + g_cm.mat[0] * 300.0f, 0xFFD040, 1.0f);
        Label3(g_cm.pos, "game camera", 0xFFD040, 10.0f);
    }
}

void DrawTraversal(SW* psw)
{
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (OtOf(psw, plo) != OT_Traversal || !FLocated(plo)) continue;
        const glm::vec3 pos = PosOfLo(plo);
        if (!FNear(pos, 12000.0f)) continue;
        const uint32_t rgb = plo->fRemoved || !plo->fInWorld ? 0x305070 : 0x3AA3F0;
        bool fDrawn = false;
        if (FIsLoDerivedFrom(plo, 0x0A)) { DrawCurve(plo, rgb, 1.2f, false); fDrawn = PapCurve(plo) != nullptr; }
        else if (plo->Cid() == 0x08 || plo->Cid() == 0x09)
        {
            const LOXPTS* pp = static_cast<const LOXPTS*>(plo);
            const glm::mat4 m = MatWorldLox(plo);
            for (const glm::vec3& v : pp->avec) Cross3(glm::vec3(m * glm::vec4(v, 1.0f)), 12.0f, rgb);
            fDrawn = !pp->avec.empty();
        }
        else fDrawn = FDrawShape(plo, rgb, 0.8f);
        if (!fDrawn) Cross3(pos, 25.0f, rgb, 0.8f);
        if (glm::length(pos - s_posCam) < 3500.0f)
        {
            std::string s = SzClassName(plo->Cid());
            for (size_t i = 1; i < s.size(); ++i) s[i] = char(std::tolower(static_cast<unsigned char>(s[i])));
            Label3(pos, s + " " + std::to_string(plo->oid), rgb, 10.0f);
        }
    }
}

void DrawCollision(SW* psw)
{
    const float sRange = s_asCollisionRange[std::clamp(s_set.iCollisionRange, 0, CCOLLRANGE - 1)];
    // (every ALO: the world's own (cid 0x31, the level's solid ground) is not among class 0x30's lists)
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        ALO* palo = dynamic_cast<ALO*>(plo);
        if (!palo || palo->asoe.empty() || palo->fRemoved || !palo->fInWorld) continue;
        const bool fMoving = FIsLoDerivedFrom(plo, 0x34) || FIsLoDerivedFrom(plo, 0x38);
        for (const SOE& e : palo->asoe)
        {
            // (the local geometry has the edges; without a record it is already the world's: UpdateSoeWorld)
            const GEOM& g = e.geomLocal;
            if (g.apos.empty()) continue;
            const glm::mat4 m = e.pxf ? e.pxf->matWorld : glm::mat4(1.0f);
            // (the entry's bound first: its points' box against the range)
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (const glm::vec3& v : g.apos) lo = glm::min(lo, v), hi = glm::max(hi, v);
            const glm::vec3 c = glm::vec3(m * glm::vec4((lo + hi) * 0.5f, 1.0f));
            const float r = glm::length(hi - lo) * 0.5f * std::max(1e-3f, glm::length(glm::vec3(m[0])));
            if (glm::length(c - s_posCam) > sRange + r) continue;
            Geom3(g, m, fMoving ? 0xFFB040 : 0x40FF90, 0.45f, 0.55f, sRange);
        }
    }
}

// ------------------------------------------------------------ Labels
void DrawLabels(SW* psw)
{
    const float sRange = s_asLabelRange[std::clamp(s_set.iLabelRange, 0, CLABELRANGE - 1)];
    // the cursor (a free one in the window) or the crosshair (the picture's centre)
    glm::vec2 posAim = CanvasOfNdc(glm::vec2(0.0f));
    if (s_set.fLabelsCursor && g_gl.window && glfwGetInputMode(g_gl.window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED &&
        glfwGetWindowAttrib(g_gl.window, GLFW_HOVERED))
    {
        double x = 0, y = 0;
        int wx = 0, wy = 0, fbw = 0, fbh = 0;
        glfwGetCursorPos(g_gl.window, &x, &y);
        glfwGetWindowSize(g_gl.window, &wx, &wy);
        glfwGetFramebufferSize(g_gl.window, &fbw, &fbh);
        if (wx > 0 && wy > 0)
        {
            const DISPLAY& d = g_display;
            const float xn = float((x * fbw / wx - d.presentX) / std::max(1, d.presentWidth)) * 2.0f - 1.0f;
            const float yn = 1.0f - float((y * fbh / wy - d.presentY) / std::max(1, d.presentHeight)) * 2.0f;
            posAim = CanvasOfNdc(glm::vec2(xn, yn));
        }
    }
    struct CAND { LO* plo; glm::vec3 pos; glm::vec2 s; float d; OBJTYPE ot; };
    std::vector<CAND> acand;
    const bool fHidden = FDebugVis(DV_Hidden);
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if (!FLocated(plo)) continue;
        if ((plo->fRemoved || !plo->fInWorld) && !fHidden) continue;
        const OBJTYPE ot = OtOf(psw, plo);
        // (all but the scenery and the logic's bookkeeping; under the cursor: everything)
        if (!s_set.fLabelsCursor && (ot == OT_Mesh || ot == OT_Prop || ot == OT_Ui || ot == OT_Effect || ot == OT_Light || ot == OT_Animation ||
                                     ot == OT_Other || ot == OT_Grouper || ot == OT_Proxy))
            continue;
        // (parts of a model: its root only)
        if (!s_set.fLabelsCursor && plo->paloParent && ot == OtOf(psw, plo->paloParent)) continue;
        const glm::vec3 pos = PosOfLo(plo);
        const float d = glm::length(pos - s_posCam);
        if (sRange > 0.0f && d > sRange) continue;
        glm::vec2 s;
        if (!FProject(pos, &s)) continue;
        acand.push_back({plo, pos, s, d, ot});
    }
    if (s_set.fLabelsCursor)
    {
        // the nearest to the aim on the picture (within 40 px), then the nearest to the camera
        std::vector<CAND> a;
        for (const CAND& c : acand)
            if (glm::length(c.s - posAim) < 40.0f) a.push_back(c);
        std::sort(a.begin(), a.end(), [&](const CAND& x, const CAND& y) { return glm::length(x.s - posAim) < glm::length(y.s - posAim); });
        if (a.size() > 4) a.resize(4);
        acand = a;
        // the aim's ring
        for (int i = 0; i < 16; ++i)
        {
            const float a0 = 6.2831853f * float(i) / 16.0f, a1 = 6.2831853f * float(i + 1) / 16.0f;
            g_hud.FillScreenLine(posAim + glm::vec2(std::cos(a0), std::sin(a0)) * 40.0f, posAim + glm::vec2(std::cos(a1), std::sin(a1)) * 40.0f, 0.5f,
                                 Col(0xFFFFFF, 0.6f), Z_LINE);
        }
    }
    else
    {
        // the nearest 150
        std::sort(acand.begin(), acand.end(), [](const CAND& x, const CAND& y) { return x.d < y.d; });
        if (acand.size() > 150) acand.resize(150);
    }
    if (s_set.fLabelsCursor)
    {
        // (the few under the aim: a list beside it, each object marked and tied to its line)
        std::vector<PANELLINE> al;
        for (const CAND& c : acand)
        {
            char ach[64];
            std::snprintf(ach, sizeof ach, "  %.0f %.0f %.0f  (%.0f away)", c.pos.x, c.pos.y, c.pos.z, c.d);
            al.push_back({std::to_string(c.plo->oid) + " " + SzClassName(c.plo->Cid()) + "  " + StrReadable(psw, c.plo, c.ot) +
                              (c.plo->fRemoved || !c.plo->fInWorld ? " (hidden)" : "") + ach, InfoOfType(c.ot).rgb});
            g_hud.FillScreenLine(c.s - glm::vec2(5, 0), c.s + glm::vec2(5, 0), 0.8f, Col(InfoOfType(c.ot).rgb), Z_LINE);
            g_hud.FillScreenLine(c.s - glm::vec2(0, 5), c.s + glm::vec2(0, 5), 0.8f, Col(InfoOfType(c.ot).rgb), Z_LINE);
            g_hud.FillScreenLine(c.s, posAim + glm::vec2(44.0f, 30.0f), 0.4f, Col(InfoOfType(c.ot).rgb, 0.5f), Z_LINE);
        }
        if (al.empty()) al.push_back({"(nothing here)", 0x909090});
        Panel(al, posAim.x + 44.0f, posAim.y + 30.0f, false, 12.0f);
        return;
    }
    for (const CAND& c : acand)
    {
        const uint32_t rgb = InfoOfType(c.ot).rgb;
        std::string s = std::to_string(c.plo->oid) + " " + SzClassName(c.plo->Cid()) + "  " + StrReadable(psw, c.plo, c.ot);
        if (c.plo->fRemoved || !c.plo->fInWorld) s += " (hidden)";
        if (s_set.fLabelsCursor)
        {
            char ach[48];
            std::snprintf(ach, sizeof ach, "  %.0f %.0f %.0f", c.pos.x, c.pos.y, c.pos.z);
            s += ach;
        }
        Label3(c.pos, s, rgb, s_set.fLabelsCursor ? 12.0f : 10.0f);
    }
}

// ------------------------------------------------------------ Stats
double MbResident()
{
#if defined(__linux__)
    if (FILE* pf = std::fopen("/proc/self/statm", "r"))
    {
        unsigned long cpage = 0, cpageRes = 0;
        const int n = std::fscanf(pf, "%lu %lu", &cpage, &cpageRes);
        std::fclose(pf);
        if (n == 2) return double(cpageRes) * 4096.0 / (1024.0 * 1024.0);
    }
#endif
    return -1.0;
}

void DrawStats(SW* psw)
{
    // (the counts that cost a walk: every 15 frames)
    static int s_cframe = 0;
    static std::vector<PANELLINE> s_al;
    if (s_cframe++ % 15 == 0 || s_al.empty())
    {
        std::vector<PANELLINE> al;
        char ach[160];
        // objects, by class (the six biggest classes)
        int cAll = 0, cInWorld = 0, cActive = 0;
        std::vector<std::pair<int, int>> acls;
        for (int cid = 0; cid < CID_Max; ++cid)
            if (const int c = int(psw->mpcidaplo[size_t(cid)].size())) acls.push_back({c, cid});
        for (const auto& up : psw->aploAll)
        {
            ++cAll;
            cInWorld += !up->fRemoved && up->fInWorld;
            cActive += up->fNodeActive;
        }
        std::snprintf(ach, sizeof ach, "objects %d (in the world %d), updating %zu, active roots %d", cAll, cInWorld, psw->aupd.size(), cActive);
        al.push_back({ach, 0xE0E0E0});
        std::sort(acls.rbegin(), acls.rend());
        std::string s = "  ";
        for (size_t i = 0; i < acls.size() && i < 6; ++i)
        {
            std::snprintf(ach, sizeof ach, "%s%s %d", i ? ", " : "", SzClassName(acls[i].second), acls[i].first);
            s += ach;
        }
        al.push_back({s, 0xA0A0A0});
        // meshes
        std::unordered_set<const GLOBSET*> setgs;
        size_t cglob = 0, cvtxEst = 0;
        for (LO* plo : AploOfClass(psw, 0x30))
            if (const GLOBSET* pgs = static_cast<ALO*>(plo)->pglobset.get(); pgs && setgs.insert(pgs).second) cglob += pgs->aglob.size();
        (void)cvtxEst;
        int ctex = 0;
        double cbTex = 0;
        TextureCacheStats(&ctex, &cbTex);
        std::snprintf(ach, sizeof ach, "meshes %zu (groups %zu), textures %d (%.1f MB)", setgs.size(), cglob, ctex, cbTex / (1024.0 * 1024.0));
        al.push_back({ach, 0xE0E0E0});
        // sounds
        const SNDSPUSTATS ss = snd_SpuStats();
        std::snprintf(ach, sizeof ach, "sounds %d, voices %d / 48 (streams %d, peak %d, host %d)", snd_CSoundsActive(), ss.cUsed, ss.cStream, ss.cPeak, ss.cVoiceHost);
        al.push_back({ach, 0xE0E0E0});
        // memory, draws
        const double mb = MbResident();
        if (mb >= 0.0) std::snprintf(ach, sizeof ach, "memory %.0f MB resident", mb);
        else std::snprintf(ach, sizeof ach, "memory: n/a");
        al.push_back({ach, 0xE0E0E0});
        s_al = al;
    }
    std::vector<PANELLINE> al = s_al;
    char ach[160];
    std::snprintf(ach, sizeof ach, "draws %d, GL calls %d, passes %d, culled %d, beyond MRD %d, textures bound %d", g_renderstats.cdraw,
                  g_renderstats.cbatch, g_renderstats.cpass, g_renderstats.cculled, g_renderstats.cmrd, g_renderstats.ctexture);
    al.push_back({ach, 0xE0E0E0});
    const double msFrame = s_amsFrame[0] + s_amsFrame[1] + s_amsFrame[2];
    std::snprintf(ach, sizeof ach, "frame %.1f ms: update %.1f, draw %.1f, swap / wait %.1f", msFrame, s_amsFrame[0], s_amsFrame[1], s_amsFrame[2]);
    al.push_back({ach, msFrame > 17.5 ? 0xFFB060u : 0x90FF90u});
    Panel(al, XBoxRight(), 12.0f, true, 11.0f);
}

// ------------------------------------------------------------ Mission, scripts, events
struct EVENT { float t; std::string s; uint32_t rgb; };
std::deque<EVENT> s_aevent;
struct SCRIPTHIT { OID oid; int cid; uint32_t symid; int c; float t; };
std::vector<SCRIPTHIT> s_ahit;
int s_cUpdateEvents = 0, s_cUpdateEventsShown = 0;
std::unordered_map<const LO*, int> s_mptaskstate;
std::unordered_map<const LO*, int> s_mpseen;     // guard -> what its detector sees (0 / 1 lost / 2 seen)
const JOB* s_pjobPrev = nullptr;
const TASK* s_ptaskPrev = nullptr;
uint32_t s_serialEvents = 0;

void Log(const std::string& s, uint32_t rgb = 0xD0D0D0)
{
    s_aevent.push_back({g_clock.t, s, rgb});
    while (s_aevent.size() > 9) s_aevent.pop_front();
}

std::string StrJob(SW* psw, const JOB* pjob)
{
    if (!pjob) return "none";
    std::string s = pjob->n6c != -1 ? StrLocalized(psw, pjob->n6c) : std::string();
    if (s.empty() || s.size() > 48) s = "job";
    return s + " (" + std::to_string(pjob->oid) + ")";
}

void OnSpliceEvent(LO* plo, uint32_t symid, bool fDeferred)
{
    if (!plo) return;
    if (symid == 1) { ++s_cUpdateEvents; return; }   // (on-update: counted only)
    if (symid == 10 || symid == 11) return;          // (contacts: too many to read)
    for (SCRIPTHIT& h : s_ahit)
        if (h.oid == plo->oid && h.symid == symid)
        {
            ++h.c;
            h.t = g_clock.t;
            return;
        }
    s_ahit.push_back({plo->oid, plo->Cid(), symid, 1, g_clock.t});
    (void)fDeferred;
}

const char* SzEventName(uint32_t symid)
{
    static char s_ach[24];
    if (const char* sz = SzSpliceEventName(int(symid))) return sz;
    std::snprintf(s_ach, sizeof s_ach, "event %u", symid);
    return s_ach;
}

void DrawMission(SW* psw)
{
    std::vector<PANELLINE> al;
    char ach[200];
    const int iwrec = g_transition.iwrecCur;
    std::snprintf(ach, sizeof ach, "world %d %s   t %.1f s", iwrec, PchzDebugWorldFile(iwrec), g_clock.t);
    al.push_back({ach, 0xE0E0E0});
    al.push_back({"job: " + StrJob(psw, psw->tasks.pjobCur), 0xF4C531});
    if (const TASK* pt = psw->tasks.ptaskCur)
    {
        std::snprintf(ach, sizeof ach, "task: %d, state %d (%.1f s)%s", pt->oid, pt->nState, g_clock.t - pt->tState, pt->dtLimit > 0.0f ? "  timed" : "");
        al.push_back({ach, 0xD9AA14});
    }
    if (!psw->tasks.aptaskActive.empty())
    {
        std::string s = "available tasks:";
        int c = 0;
        for (const TASK* pt : psw->tasks.aptaskActive)
        {
            if (++c > 8) { s += " ..."; break; }
            s += " " + std::to_string(pt->oid);
        }
        al.push_back({s, 0xB0B0B0});
    }
    // the scripts running (SCPT) and the splice events just fired (the last 3 s, newest first)
    std::string sScpt;
    int cScpt = 0;
    for (LO* plo : AploOfClass(psw, 0x72))
        if (FScptRunning(plo))
        {
            if (++cScpt <= 5) sScpt += " " + std::to_string(plo->oid);
        }
    al.push_back({"scripts running (SCPT): " + (cScpt ? std::to_string(cScpt) + ":" + sScpt + (cScpt > 5 ? " ..." : "") : std::string("none")), 0x90D0FF});
    std::snprintf(ach, sizeof ach, "script events (on-update: %d a second)", s_cUpdateEventsShown);
    al.push_back({ach, 0x90D0FF});
    s_ahit.erase(std::remove_if(s_ahit.begin(), s_ahit.end(), [](const SCRIPTHIT& h) { return g_clock.t - h.t > 3.0f || h.t > g_clock.t; }), s_ahit.end());
    std::vector<SCRIPTHIT> ahit = s_ahit;
    std::sort(ahit.begin(), ahit.end(), [](const SCRIPTHIT& a, const SCRIPTHIT& b) { return a.t > b.t; });
    for (size_t i = 0; i < ahit.size() && i < 7; ++i)
    {
        const SCRIPTHIT& h = ahit[i];
        LO* plo = PloOid(h.oid, nullptr);
        const std::string strName = plo ? StrReadable(psw, plo, OtOf(psw, plo)) : std::string(SzClassName(h.cid));
        std::snprintf(ach, sizeof ach, "  %d %s: %s%s", h.oid, strName.c_str(), SzEventName(h.symid), h.c > 1 ? (" x" + std::to_string(h.c)).c_str() : "");
        al.push_back({ach, 0x80B0E0});
    }
    al.push_back({"events:", 0xFFFFFF});
    for (auto it = s_aevent.rbegin(); it != s_aevent.rend(); ++it)
    {
        std::snprintf(ach, sizeof ach, "  %6.1f  %s", it->t, it->s.c_str());
        al.push_back({ach, it->rgb});
    }
    const float h = (11.0f + 2.0f) * float(al.size()) + 6.0f;
    Panel(al, XBoxLeft(), std::max(12.0f, 436.0f - h), false, 11.0f);
}
}  // namespace

// ------------------------------------------------------------ the API
bool FDebugVisWant(DEBUGVIS k)
{
    Load();
    return k >= 0 && k < DV_Max && (s_set.af[k] || s_afEnv[k]);
}

bool FDebugVis(DEBUGVIS k)
{
    Load();
    if (k < 0 || k >= DV_Max) return false;
    return s_afEnv[k] || (g_pcinput.fDebugKeys && s_set.af[k]);
}

bool FDebugVisAny()
{
    for (int k = 0; k < DV_Max; ++k)
        if (FDebugVis(DEBUGVIS(k))) return true;
    return false;
}

void SetDebugVis(DEBUGVIS k, bool f)
{
    Load();
    if (k < 0 || k >= DV_Max) return;
    s_set.af[k] = f;
    if (!f) s_afEnv[k] = false;
    Save();
}

void SetAllDebugVis(bool f)
{
    Load();
    for (int k = 0; k < DV_Max; ++k)
    {
        s_set.af[k] = f;
        if (!f) s_afEnv[k] = false;
    }
    Save();
}

const char* SzDebugVis(DEBUGVIS k) { return k >= 0 && k < DV_Max ? s_apchzName[k] : "?"; }

std::string StrDebugVisLabelRange()
{
    Load();
    const float s = s_asLabelRange[std::clamp(s_set.iLabelRange, 0, CLABELRANGE - 1)];
    return s > 0.0f ? std::to_string(int(s)) : std::string("no limit");
}

void StepDebugVisLabelRange(int dir)
{
    Load();
    s_set.iLabelRange = std::clamp(s_set.iLabelRange + dir, 0, CLABELRANGE - 1);
    Save();
}

bool FDebugVisLabelsCursor() { Load(); return s_set.fLabelsCursor; }
void SetDebugVisLabelsCursor(bool f) { Load(); s_set.fLabelsCursor = f; Save(); }

std::string StrDebugVisCollisionRange()
{
    Load();
    return std::to_string(int(s_asCollisionRange[std::clamp(s_set.iCollisionRange, 0, CCOLLRANGE - 1)]));
}

void StepDebugVisCollisionRange(int dir)
{
    Load();
    s_set.iCollisionRange = std::clamp(s_set.iCollisionRange + dir, 0, CCOLLRANGE - 1);
    Save();
}

void NoteDebugVisCamera(const CAMERA& cam, bool fWorld)
{
    if (!FDebugVisAny()) return;
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA c = cam;
    c.fovY = glm::degrees(proj.radFovY);
    s_matWorldToClip = c.MatWorldToClip(proj.rAspect);
    s_posCam = cam.pos;
    s_dirCam = cam.Dir();
    s_fWorldView = fWorld;
}

void NoteDebugVisFrameTimes(double msUpdate, double msDraw, double msSwap)
{
    const double a[3] = {msUpdate, msDraw, msSwap};
    for (int i = 0; i < 3; ++i) s_amsFrame[i] = s_amsFrame[i] * 0.9 + a[i] * 0.1;
}

void UpdateDebugVis(SW* psw)
{
    const bool fMission = psw && FDebugVis(DV_Mission);
    g_pfnDebugVisSpliceEvent = fMission ? OnSpliceEvent : nullptr;
    if (!fMission) return;
    // (once a second: the on-update events counted)
    static float s_tCount = 0.0f;
    if (g_clock.t - s_tCount >= 1.0f || g_clock.t < s_tCount)
    {
        s_cUpdateEventsShown = s_cUpdateEvents;
        s_cUpdateEvents = 0;
        s_tCount = g_clock.t;
    }
    if (psw->serial != s_serialEvents)
    {
        // a new world: its tasks' states are the start, not changes
        s_serialEvents = psw->serial;
        s_mptaskstate.clear();
        s_mpseen.clear();
        s_ahit.clear();
        s_pjobPrev = nullptr;
        s_ptaskPrev = nullptr;
        Log("world " + std::to_string(g_transition.iwrecCur) + " " + PchzDebugWorldFile(g_transition.iwrecCur) + " started", 0x90FF90);
        for (LO* plo : AploOfClass(psw, 0x85))
            if (const TASK* pt = dynamic_cast<const TASK*>(plo)) s_mptaskstate[plo] = pt->nState;
    }
    // (test aid: SLY2N_DEBUGVISPRINT=1 lists the world's guards and where they are 10 s in, for --start-pos)
    if (static uint32_t s_serialPrinted = 0; std::getenv("SLY2N_DEBUGVISPRINT") && s_serialPrinted != psw->serial && g_clock.t > 10.0f)
    {
        s_serialPrinted = psw->serial;
        for (LO* plo : AploOfClass(psw, 0x38))
            {
                const glm::vec3 p = PosLoWorld(plo);
                std::printf("debugvis: guard %d cid 0x%X at %.0f,%.0f,%.0f%s\n", plo->oid, plo->Cid(), p.x, p.y, p.z, plo->fRemoved ? " (removed)" : "");
            }
    }
    for (LO* plo : AploOfClass(psw, 0x85))
    {
        const TASK* pt = dynamic_cast<const TASK*>(plo);
        if (!pt) continue;
        auto it = s_mptaskstate.find(plo);
        if (it == s_mptaskstate.end()) { s_mptaskstate[plo] = pt->nState; continue; }
        if (it->second != pt->nState)
        {
            Log("task " + std::to_string(pt->oid) + ": state " + std::to_string(it->second) + " -> " + std::to_string(pt->nState), 0xD9AA14);
            it->second = pt->nState;
        }
    }
    if (psw->tasks.pjobCur != s_pjobPrev)
    {
        Log(psw->tasks.pjobCur ? "job started: " + StrJob(psw, psw->tasks.pjobCur) : "job ended: " + StrJob(psw, s_pjobPrev), 0xF4C531);
        s_pjobPrev = psw->tasks.pjobCur;
    }
    if (psw->tasks.ptaskCur != s_ptaskPrev)
    {
        if (psw->tasks.ptaskCur) Log("current task: " + std::to_string(psw->tasks.ptaskCur->oid), 0xD9AA14);
        s_ptaskPrev = psw->tasks.ptaskCur;
    }
    for (LO* plo : AploOfClass(psw, 0x38))
    {
        GUARD* pg = PguardFromLo(plo);
        if (!pg || !pg->pdet) continue;
        int st = 0;
        for (const DETENTRY& e : pg->pdet->ae) st = std::max(st, e.st == 1 ? 2 : e.st == 2 ? 1 : 0);
        int& stPrev = s_mpseen[plo];
        if (st != stPrev)
        {
            if (st == 2) Log("guard " + std::to_string(plo->oid) + " sees the player", 0xFF5050);
            else if (stPrev == 2) Log("guard " + std::to_string(plo->oid) + " lost sight", 0xFFA020);
            stPrev = st;
        }
    }
}

void DrawDebugVis(SW* psw)
{
    if (!psw || !FDebugVisAny()) return;
    s_pfont = PfontHud();
    s_picons = PfontHudIcons();
    s_cline = 0;
    SyncWorld(psw);
    const bool fWorld = s_fWorldView && !vr::FActive();
    if (fWorld)
    {
        if (FDebugVis(DV_Collision)) DrawCollision(psw);
        if (FDebugVis(DV_Cameras)) DrawCameras(psw);
        if (FDebugVis(DV_Triggers)) DrawTriggers(psw);
        if (FDebugVis(DV_Traversal)) DrawTraversal(psw);
        if (FDebugVis(DV_Hidden)) DrawHidden(psw);
        if (FDebugVis(DV_Guards)) DrawGuards(psw);
        if (FDebugVis(DV_Labels)) DrawLabels(psw);
    }
    if (FDebugVis(DV_Stats)) DrawStats(psw);
    if (FDebugVis(DV_Mission)) DrawMission(psw);
}
