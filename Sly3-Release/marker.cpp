// marker.cpp: the job markers (class 0x6B, module 0x229EF0..0x22AEE8: InitLo 0x229EF0, PostLoad 0x229F88, Update
// 0x22A060, the placement 0x22A220, Draw 0x22A908) and what shows them: L3 (0x140338, from the players' update
// 0x1403D0) and the binocucom's view (0x150160). See marker.h.
#include "marker.h"
#include "game/sly3.h"

#include "activate.h"
#include "alo.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "emitter.h"
#include "lo.h"
#include "po.h"
#include "render.h"
#include "scene.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "update.h"
#include "xf.h"
#include "physworld.h"
#include "bsp.h"
#include "geom.h"
#include "glob.h"
#include "anim.h"
#include "brx.h"
#include "animfind.h"
#include "transition.h"
#include "pickup.h"
#include "hud.h"
#include "splayer.h"
#include "screenobj.h"
#include "ref.h"
#include "savedata.h"

#include <string>

#include <functional>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>


int CClueCollectedCur();   // vault.cpp (0x1C7D08 / 0x1C7D00: SW +0x4764 / +0x4768)
int CClueNeededCur();

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY2N_MARKERPRINT") != nullptr; return s_f; }

// SW +0x4750 (the binocucom's view, set every frame it runs) / +0x4754 (L3): -60 in a world just loaded
float s_tBinoc = -60.0f, s_tL3 = -60.0f;
uint32_t s_serialSw = 0;     // (the world they belong to)

struct MARKER;
void EnsureWorld(SW* psw);

struct MARKER
{
    int kind = 0;            // +0x150: 0 a job's (shown while no job runs), 1 (while a job runs), 2 the vault's (all
                             // the bottles, no job, its +0x184 / +0x188 tests)
    int cidPlayer = 0x32;    // +0x154: the current player's class that triggers it
    float sNear = 300.0f;    // +0x158 / +0x15C: in / out radius
    float sFar = 500.0f;
    LO* ptask = nullptr;     // +0x160 (its job's name drawn in the binocucom's view, 0x1CD780)
    int fIn = 0;             // +0x164
    LO* ploAnchor = nullptr;     // +0x168 (the ground point; itself without one)
    ALO* paloIcon = nullptr;     // +0x16C (drawn on top)
    ALO* paloLine = nullptr;     // +0x170 (drawn from the ground to the top)
    ALO* paloBase = nullptr;     // +0x174 (drawn at the ground point)
    int cL3 = -1;            // +0x178 (CM +0x4A4 when it was last placed)
    float h = 0.0f;          // +0x180: the height above the anchor that the camera sees past the world
    int nPowerup = -1;       // +0x184 (the vault's power-up)
    bool fTaskSought = false;
    JOB* pjob = nullptr;     // +0x188 (the vault's job)
};
std::unordered_map<const LO*, MARKER> s_mpmarker;

void EnsureWorld(SW* psw)
{
    if (psw && psw->serial != s_serialSw)
    {
        s_serialSw = psw->serial;
        s_tBinoc = s_tL3 = -60.0f;
        s_mpmarker.clear();
    }
}

float F32Field(LO* plo, uint32_t ib, float g)
{
    uint32_t u;
    std::memcpy(&u, &g, sizeof u);
    u = plo->U32Field(0, ib, u);
    std::memcpy(&g, &u, sizeof g);
    return g;
}
LO* PloField(LO* plo, uint32_t ib)
{
    const OID oid = static_cast<OID>(plo->U32Field(0, ib, static_cast<uint32_t>(-1)));
    if (oid == -1) return nullptr;
    // (the marker's own descendants first: each job's copy has its own)
    std::function<LO*(LO*)> find = [&](LO* p) -> LO* {
        for (LO* pc : p->aploOwned)
        {
            if (pc->oid == oid) return pc;
            if (LO* q = find(pc)) return q;
        }
        return nullptr;
    };
    LO* pfound = find(plo);
    return pfound ? pfound : PloFindSwNearest(plo, oid);
}
ALO* PaloField(LO* plo, uint32_t ib)
{
    LO* p = PloField(plo, ib);
    return p && FCidDerivedFrom(p->Cid(), S3CID(0x30)) ? static_cast<ALO*>(p) : nullptr;
}

void SetDrawn(LO* plo, bool f)
{
    if (!plo) return;
    uint32_t grf = GrfUpdOfLo(plo);
    grf = f ? (grf | 8u) : (grf & ~8u);
    SetGrfUpdOfLo(plo, grf);
}

glm::vec3 PosWorld(const LO* plo) { return PosLoWorld(plo); }  // 0x1A62E8
glm::mat3 MatWorld(ALO* palo)                                   // 0x1A6318 (the object record's)
{
    const XF* pxf = PxfObj(palo);
    return pxf ? glm::mat3(pxf->matWorld) : glm::mat3(1.0f);
}

// orig: 0x0022A220 (on a new L3 press: the height +0x180 above the anchor at which the line from the camera clears the
// world: over the world's solids between the camera and the anchor (up to 5000 above both), the steepest rise seen
// from the camera, less the anchor's own, times the distance). Natively the world's solids are line-tested from the
// camera: the edges of the world's solids crossing the vertical quad from the camera to the anchor.
void PlaceMarker(LO* plo, MARKER& m)
{
    if (m.cL3 == g_cm.cFreeStarts) return;  // (CM +0x4A4: L3's presses and the free camera's starts)
    m.cL3 = g_cm.cFreeStarts;
    m.h = 0.0f;
    glm::vec3 c = g_cm.pos;
    if (static const char* const sz = std::getenv("SLY2N_MARKERCAM"); sz) std::sscanf(sz, "%f %f %f", &c.x, &c.y, &c.z);  // (debug: the reference's camera)
    const glm::vec3 a = PosWorld(m.ploAnchor ? m.ploAnchor : plo);
    const float dxy = glm::length(glm::vec2(a - c));
    if (dxy < 1.0f) return;
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const float hMax = 5000.0f;  // D_002C4490
    // the vertical quad {A, A + H, C + H, C}, its plane (0x119308 of C, A, A + H) and its four sides' planes (0x119390:
    // the edge x the plane's normal, through the edge's start)
    const glm::vec3 aq[4] = {a, a + up * hMax, c + up * hMax, c};
    glm::vec3 n = glm::cross(a - c, aq[1] - c);
    if (glm::dot(n, n) < 1e-12f) return;
    n = glm::normalize(n);
    const float w = glm::dot(n, c);
    glm::vec4 aside[4];
    const glm::vec3 posMid = (aq[0] + aq[1] + aq[2] + aq[3]) * 0.25f;
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec3 e = aq[(i + 1) % 4] - aq[i];
        glm::vec3 ns = glm::cross(e, n);
        const float l = glm::length(ns);
        ns = l > 0.0f ? ns / l : ns;
        float d = glm::dot(ns, aq[i]);
        if (glm::dot(ns, posMid) - d > 0.0f) { ns = -ns; d = -d; }  // (inside: negative, as the original's winding)
        aside[i] = glm::vec4(ns, d);
    }
    // the world's solid entries in the quad's box (0x20FAA0 with the camera's filter 0x14F030)
    const CMSOLIDS asoe = AsolidWithTree(glm::min(c, a), glm::max(c, a) + up * hMax);
    // the steepest rise from the camera over the entries' edges crossing the quad (0x118D08's surfaces of the entry's
    // BSP that the quad touches: their edges; natively every edge of the world geometry), from the anchor's own
    const float slopeA = (a.z - c.z) / dxy;
    float slope = slopeA;
    for (const SOE* pe : asoe)
    {
        const GEOM& g = pe->geomWorld;
        for (const EDGE& e : g.aedge)
        {
            if (e.ipos0 >= g.apos.size() || e.ipos1 >= g.apos.size()) continue;
            const glm::vec3 p0 = g.apos[e.ipos0], p1 = g.apos[e.ipos1];
            const float d0 = glm::dot(n, p0) - w, d1 = glm::dot(n, p1) - w;
            if ((0.0f < d0 && 0.0f < d1) || (d0 < 0.0f && d1 < 0.0f) || d0 == d1) continue;
            const float u = d0 / (d0 - d1);
            const glm::vec3 p = p0 * (1.0f - u) + p1 * u;
            bool fIn = true;
            for (const glm::vec4& sd : aside)
                if (!(glm::dot(glm::vec3(sd), p) - sd.w < 0.0f)) { fIn = false; break; }
            if (!fIn) continue;
            const float dp = glm::length(glm::vec2(p - c));
            if (dp <= 0.0f) continue;
            slope = std::max(slope, (p.z - c.z) / dp);
        }
    }
    m.h = std::min((slope - slopeA) * dxy, hMax);
    if (FPrint())
        std::printf("marker %d: placed, anchor (%.0f %.0f %.0f) camera (%.0f %.0f %.0f) height %.0f\n", plo->oid, a.x, a.y, a.z, c.x, c.y, c.z, m.h);
}

// a quadratic of u clamped to [0, 1] (D_002BBB60)
float GCurveUnit(float u, float c0, float c1, float c2) { return std::clamp(c0 + u * (c1 + u * c2), 0.0f, 1.0f); }

// orig: 0x0022A908's alphas: the marker's (L3 / the binocucom within 10 s, fading over 2) and the label's (the
// binocucom's view only, fading over 1 s), each 0 in the states that hide it
void MarkerAlphas(LO* plo, const MARKER& m, float* pgMarker, float* pgLabel)
{
    const float t = g_clock.t;
    float g = GCurveUnit(t - std::max(s_tL3 + 10.0f, s_tBinoc + 10.0f), 1.0f, -0.5f, 0.0f);  // D_002C4494.., D_002C44A0
    float gLabel = GCurveUnit(t - (s_tBinoc + 0.1f), 1.0f, -1.0f, 0.0f);                    // D_002C449C, D_002C44B0
    if (g == 0.0f && gLabel == 0.0f)
    {
        // (both faded out: hidden or not, the same; the progress lookups below are skipped, they cost a frame's draw)
        *pgMarker = *pgLabel = 0.0f;
        return;
    }
    const bool fJob = g_psw && g_psw->tasks.pjobCur;                                       // SW +0x3B0C
    bool fHide = false;
    switch (m.kind)
    {
    case 0: fHide = fJob; break;
    case 1: fHide = !fJob; break;
    case 2:
    {
        // all the bottles, no job running; not when the power-up +0x184 is had (0x1DAEA8: the progress powerUps /
        // mppukfAcquired / n); with a job +0x188 only once its last task is done (0x163400)
        fHide = CClueCollectedCur() < CClueNeededCur() || fJob;
        if (!fHide && m.nPowerup != -1)
        {
            const CRef r = RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(m.nPowerup)});
            fHide = (r.m_tagk == TAGK_Bool && r.m_bool) || (r.m_tagk == TAGK_S32 && r.m_n);
        }
        if (!fHide && m.pjob)
        {
            const TASK* pt = m.pjob->ptaskLast;
            fHide = !(pt && pt->nState >= 2);
        }
        break;
    }
    default: break;
    }
    (void)plo;
    if (fHide) g = gLabel = 0.0f;
    *pgMarker = g;
    *pgLabel = gLabel;
}

PFNEXTRADRAWS s_pfnExtraPrev = nullptr;

// the frame's job names (0x1CD780's requests: the point, the string, the label's alpha, the marker's scale)
struct LABEL
{
    glm::vec3 pos{0.0f};
    std::string str;
    float alpha = 0.0f, scale = 1.0f;
};
std::vector<LABEL> s_alabel;

float SRadiusGlobs(const ALO* palo)
{
    float r = 0.0f;
    if (palo && palo->pglobset)
        for (const GLOB& g : palo->pglobset->aglob) r = std::max(r, g.sRadius);
    return r;
}

// orig: 0x0022A908 (vt slot 19: the ALO's own draw, then while shown: placed (0x22A220); the scale by the camera's
// distance to the line (0x11EE08: clamp(0.5 + d / 3500, 1, 5)); the icon +0x16C at the top (the anchor raised by the
// height and 300 x the scale) scaled, the line +0x170 from the anchor to the top (0x1A5FB8), the base +0x174 at the
// anchor scaled, all at the marker's alpha (0x1A5EE8); the job's name at the top in the binocucom's view: TODO)
void CollectMarkerDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra)
{
    if (s_pfnExtraPrev) s_pfnExtraPrev(scene, aextra);
    s_alabel.clear();
    if (!g_psw) return;
    EnsureWorld(g_psw);
    for (auto& [plo, m] : s_mpmarker)
    {
        LO* pm = const_cast<LO*>(plo);
        if (!pm->fInWorld || pm->fRemoved) continue;
        // (0x22A908 is the marker's own draw, vt slot 19: only for a marker the world draws, its update entry active
        // and shown, e.g. not the inactive instances of a shared group's marker)
        if (!FAloDrawn(g_psw, static_cast<const ALO*>(plo))) continue;
        float g, gLabel;
        MarkerAlphas(pm, m, &g, &gLabel);
        if (g == 0.0f) continue;
        // (debug: SLY2N_MARKERLIST lists the markers drawn, once a second)
        static const bool s_fList = std::getenv("SLY2N_MARKERLIST") != nullptr;
        static int s_nListSecond = -1;
        if (s_fList && (static_cast<int>(g_clock.t) != s_nListSecond || pm != s_mpmarker.begin()->first))
        {
            const glm::vec3 a = PosWorld(m.ploAnchor ? m.ploAnchor : pm);
            std::printf("marker %d kind %d drawn t %.2f alpha %.2f anchor %d at (%.0f %.0f %.0f)\n", pm->oid, m.kind, g_clock.t, g,
                        m.ploAnchor ? m.ploAnchor->oid : -1, a.x, a.y, a.z);
            s_nListSecond = static_cast<int>(g_clock.t);
        }
        PlaceMarker(pm, m);
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        const glm::vec3 c0 = PosWorld(m.ploAnchor ? m.ploAnchor : pm);
        const glm::vec3 d0 = c0 + up * m.h;
        // 0x11EE08: the camera's distance to the segment c0..d0
        const glm::vec3 seg = d0 - c0;
        const float l2 = glm::dot(seg, seg);
        const float u = l2 > 0.0f ? std::clamp(glm::dot(g_cm.pos - c0, seg) / l2, 0.0f, 1.0f) : 0.0f;
        const float dist = glm::length(g_cm.pos - (c0 + seg * u));
        const float scale = std::clamp(0.5f + dist * 0.000285714f, 1.0f, 5.0f);  // D_002C44C0 / D_002C44D0
        glm::vec3 e0 = d0 + up * (scale * 300.0f);                          // D_002C44D8
        if (static const bool s_fTest = std::getenv("SLY2N_MARKERTEST") != nullptr; s_fTest) e0 = g_cm.pos + g_cm.mat[0] * 800.0f - glm::vec3(0.0f, 0.0f, 250.0f);
        auto draw = [&](ALO* palo, const glm::mat4& mat) {
            if (!palo) return;
            if (static const char* const sz = std::getenv("SLY2N_MARKERPART"); sz && std::atoi(sz) != palo->oid) return;
            const size_t c = aextra.size();
            AppendAloExtraDraws(scene, palo, mat, g, aextra);
            static int s_cprint = 0;
            if (FPrint() && s_cprint < 12)
            {
                ++s_cprint;
                std::printf("marker %d: part %d drawn (%zu globs) at (%.0f %.0f %.0f) alpha %.2f scale %.2f\n", pm->oid, palo->oid, aextra.size() - c,
                            mat[3].x, mat[3].y, mat[3].z, g, scale);
            }
        };
        if (m.paloIcon)
        {
            glm::mat4 mat(MatWorld(m.paloIcon) * scale);
            mat[3] = glm::vec4(e0, 1.0f);
            draw(m.paloIcon, mat);
        }
        if (m.paloLine)
        {
            // 0x1A5FB8(line, c0, e0, scale, alpha): x across the view, y its width, z along it (the model 100 long)
            const glm::vec3 d = e0 - c0;
            const glm::vec3 n = glm::cross(c0 - g_cm.pos, d);
            const float ln = glm::length(n);
            if (0.01f < ln)
            {
                glm::mat4 mat(1.0f);
                mat[0] = glm::vec4(glm::normalize(glm::cross(n, d)), 0.0f);
                mat[1] = glm::vec4(n * (scale / ln), 0.0f);
                mat[2] = glm::vec4(d * 0.01f, 0.0f);
                mat[3] = glm::vec4(c0, 1.0f);
                draw(m.paloLine, mat);
            }
        }
        if (m.paloBase)
        {
            glm::mat4 mat(MatWorld(m.paloBase) * scale);
            mat[3] = glm::vec4(c0, 1.0f);
            draw(m.paloBase, mat);
        }
        // the job's name over the icon (+0x160 the task, its job's +0x6C string; 0x13E368 the world's strings) at the
        // label's alpha, (E0 + up x scale x the icon's radius +0xA0)
        // (+0x160: natively the task showing this marker (its +0x80), found once)
        if (!m.ptask && !m.fTaskSought)
        {
            m.fTaskSought = true;
            for (LO* pt : g_psw->mpcidaplo[S3CID(0x85)])
                if (static_cast<TASK*>(pt)->ploShown == pm) { m.ptask = pt; break; }
        }
        if (FPrint() && 0.0f < gLabel)
        {
            static int s_c = 0;
            if (s_c++ < 6)
                std::printf("marker %d: label alpha %.2f task %d cid 0x%X job %d n6c %d\n", pm->oid, gLabel, m.ptask ? m.ptask->oid : -1, m.ptask ? m.ptask->Cid() : 0,
                            m.ptask && m.ptask->Cid() == S3CID(0x85) && static_cast<const TASK*>(m.ptask)->pjob ? static_cast<const TASK*>(m.ptask)->pjob->oid : -1,
                            m.ptask && m.ptask->Cid() == S3CID(0x85) && static_cast<const TASK*>(m.ptask)->pjob ? static_cast<const TASK*>(m.ptask)->pjob->n6c : -9);
        }
        if (0.0f < gLabel && m.ptask && m.ptask->Cid() == S3CID(0x85))
            if (const JOB* pjob = static_cast<const TASK*>(m.ptask)->pjob; pjob && pjob->n6c != -1)
            {
                LABEL l;
                l.pos = e0 + up * (scale * SRadiusGlobs(m.paloIcon));
                l.str = StrLocalized(g_psw, pjob->n6c);
                l.alpha = gLabel;
                l.scale = scale;
                if (!l.str.empty()) s_alabel.push_back(l);
            }
    }
}

// orig: 0x0022A060 (vt slot 16: the ALO's update; the current player of class +0x154 within +0x158 (+0x15C once in)
// of the marker, not in kinds 0 / 2 while a job runs, and a player 0x34 with no +0x2DC and slot 77 not 2 (TODO: taken
// as so): splice event 5 on entering, 6 on leaving)
void UpdateMarker(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    auto it = s_mpmarker.find(plo);
    if (it == s_mpmarker.end()) return;
    MARKER& m = it->second;
    LO* pj = PloPoCurrent(g_psw);
    int f = 0;
    if (pj && FIsLoDerivedFrom(pj, m.cidPlayer))
    {
        const float r = m.fIn ? m.sFar : m.sNear;
        f = glm::length(PosWorld(pj) - PosWorld(plo)) < r;
    }
    if ((m.kind == 0 || m.kind == 2) && g_psw->tasks.pjobCur) f = 0;
    if (f != m.fIn)
    {
        m.fIn = f;
        // vt slot 3 (the base 0x1A61E0): the splice event, then the tasks' 0x1A / 0x1B (what starts the marker's job)
        // TODO(world): the original keeps the old state while a player of 0x34 has +0x2DC != -1 or its vt slot 77 is 2
        HandleLoSpliceEvent(plo, f ? 5 : 6, false, 0, nullptr);
        BroadcastTaskEvent(g_psw, f ? 0x1A : 0x1B, plo);  // 0x1619A0
        if (FPrint()) std::printf("marker %d: player %s\n", plo->oid, f ? "in" : "out");
    }
}

struct INITMARKER
{
    INITMARKER() { RegisterLoUpdate(0x22A060, UpdateMarker); }
} s_initmarker;
}  // namespace

// orig: 0x00229EF0 (InitLo: updated and drawn, the defaults) + 0x00229F88 (PostLoad: the three drawn parts not drawn by
// themselves: their +0x40 flag 8 cleared)
void PostLoadMarker(SW* psw, LO* plo)
{
    EnsureWorld(psw);
    MARKER& m = s_mpmarker[plo];
    m = MARKER{};
    m.kind = static_cast<int>(plo->U32Field(0, 0x150, 0));
    m.cidPlayer = static_cast<int>(plo->U32Field(0, 0x154, 50));
    m.sNear = F32Field(plo, 0x158, 300.0f);
    m.sFar = F32Field(plo, 0x15C, 500.0f);
    m.ploAnchor = PloField(plo, 0x168);
    m.paloIcon = PaloField(plo, 0x16C);
    m.paloLine = PaloField(plo, 0x170);
    m.paloBase = PaloField(plo, 0x174);
    m.ptask = PloField(plo, 0x160);
    m.nPowerup = static_cast<int>(plo->U32Field(0, 0x184, static_cast<uint32_t>(-1)));
    if (LO* pj = PloField(plo, 0x188); pj && pj->Cid() == S3CID(0x84)) m.pjob = static_cast<JOB*>(pj);
    SetGrfUpdOfLo(plo, GrfUpdOfLo(plo) | 0xAu);
    for (ALO* p : {m.paloIcon, m.paloLine, m.paloBase}) SetDrawn(p, false);
    if (g_pfnExtraDraws != CollectMarkerDraws)
    {
        s_pfnExtraPrev = g_pfnExtraDraws;
        g_pfnExtraDraws = CollectMarkerDraws;
    }
    if (FPrint())
        for (ALO* pa : {m.paloIcon, m.paloLine, m.paloBase})
        {
            if (!pa) continue;
            const XF* px = PxfObj(pa);
            std::printf("  part %d cid 0x%X obj xf %s", pa->oid, pa->Cid(), px ? "yes" : "no");
            if (px) for (int i = 0; i < 4; i++) std::printf(" [%.2f %.2f %.2f]", px->matWorld[i].x, px->matWorld[i].y, px->matWorld[i].z);
            if (pa->pglobset)
                for (const GLOB& g : pa->pglobset->aglob) std::printf(" glob r %.1f c (%.1f %.1f %.1f) owner %d", g.sRadius, g.posCenter.x, g.posCenter.y, g.posCenter.z, g.oidOwner);
            std::printf("\n");
        }
    if (FPrint())
    {
        std::printf("marker %d: kind %d player 0x%X radii %.0f / %.0f anchor %d icon %d line %d base %d fields", plo->oid, m.kind, m.cidPlayer, m.sNear,
                    m.sFar, m.ploAnchor ? m.ploAnchor->oid : -1, m.paloIcon ? m.paloIcon->oid : -1, m.paloLine ? m.paloLine->oid : -1,
                    m.paloBase ? m.paloBase->oid : -1);
        for (const auto& [k, u] : plo->afield) std::printf(" %X=%X", static_cast<unsigned>(k), u);
        std::printf("\n");
    }
}

// orig: 0x00140338 (L3, from 0x1403D0: outside episode 0 the coin counter (0x161868), the bottle counter when the world
// has bottles (0x15E9C8 of class 0x5F, 0x1618B0), the sound 0x32, SW +0x4754 the time, CM +0x4A4 counted)
void ShowWorldStats()
{
    EnsureWorld(g_psw);
    if (IepisodeCur() == 0) return;  // 0x210AE0
    ShowCoinCounter();
    if (g_psw && !g_psw->mpcidaplo[S3CID(0x5F)].empty()) ShowClueCounter();
    if (CSnd* ps = PsndNewPlain(0x32)) ps->SetSource(nullptr);  // 0x1FBB90 / 0x1FB2F8
    s_tL3 = g_clock.t;
    g_cm.cFreeStarts++;  // CM +0x4A4
    if (FPrint())
    {
        std::printf("markers: L3 at %.3f\n", s_tL3);
        for (auto& [plo, m] : s_mpmarker)
            std::printf("  marker %d kind %d inworld %d removed %d at (%.0f %.0f %.0f)\n", plo->oid, m.kind, plo->fInWorld, plo->fRemoved,
                        PosWorld(m.ploAnchor ? m.ploAnchor : const_cast<LO*>(plo)).x, PosWorld(m.ploAnchor ? m.ploAnchor : const_cast<LO*>(plo)).y,
                        PosWorld(m.ploAnchor ? m.ploAnchor : const_cast<LO*>(plo)).z);
    }
}

// orig: SW +0x4750 = t, stored by the binocucom's view (0x14FF38 its start, 0x150160 each frame) and by the free /
// scripted camera (0x150938 its start, 0x150A28 each frame with a time step: the binocucom's talks), so the markers and
// the jobs' names show through a briefing
void NoteBinocView()
{
    EnsureWorld(g_psw);
    s_tBinoc = g_clock.t;
}

// orig: 0x001CD780 (each job's name in the binocucom's view: its point on the screen (0x14F308), nothing nearer than
// the near clip (CM +0x4C); the text size scale x 640 / (CM +0x40 x depth) within [0.4, 1.5] (D_002C1F20 /
// D_002C1F28), centred over the point, its bottom there; padded 8 (D_002C1F30); light grey 0xC0 at the label's alpha)
void DrawMarkerLabels(const FONTBRX* pfont, const FONTBRX* picons)
{
    if (FPrint() && !s_alabel.empty())
    {
        static int s_c = 0;
        if (s_c++ < 4) std::printf("markers: %zu labels drawn (font %d) first '%s'\n", s_alabel.size(), pfont != nullptr, s_alabel[0].str.c_str());
    }
    if (!pfont) return;
    for (const LABEL& l : s_alabel)
    {
        const glm::vec3 lc = glm::transpose(g_cm.mat) * (l.pos - g_cm.pos);
        if (FPrint()) std::printf("label '%s' depth %.0f near %.0f\n", l.str.c_str(), lc.x, g_cm.sNearClip);
        if (lc.x < g_cm.sNearClip) continue;
        const float xs = (-lc.y / g_cm.xScreenRange) / lc.x, ys = (lc.z / g_cm.yScreenRange) / lc.x;
        const float size = std::clamp(l.scale * 640.0f / (g_cm.xScreenRange * lc.x), 0.4f, 1.5f);
        const float x = (xs * 0.5f + 0.5f) * 640.0f, y = (0.5f - ys * 0.5f) * 492.8f;
        if (FPrint()) std::printf("label at %.0f %.0f size %.2f alpha %.2f\n", x, y, size, l.alpha);
        const float wBox = 640.0f;
        const glm::vec4 off(192.0f, 192.0f, 192.0f, 0.0f), sc(1.0f, 1.0f, 1.0f, l.alpha);
        g_hud.DrawTextBox(*pfont, picons, l.str, x - wBox * 0.5f - 320.0f, y - 8.0f - 60.0f - 246.4f, size * 51.2f, 1.0f, 1.0f, wBox, 60.0f, 1,
                          2, off, sc, off, g_zScreenBase + 16.0f);  // (over the screen objects, the binocucom's frame
                                                                    // included, as in the reference)
    }
}

// (debug harness, autoplay.cpp: every marker of the loaded world with its kind, player class and task)
void ForEachMarker(const std::function<void(LO*, int kind, int cidPlayer, LO* ptask)>& fn)
{
    EnsureWorld(g_psw);   // (a world without markers keeps the last world's entries until then)
    for (auto& [plo, m] : s_mpmarker) fn(const_cast<LO*>(plo), m.kind, m.cidPlayer, m.ptask ? m.ptask : m.pjob);
}
