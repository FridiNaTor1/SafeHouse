// hackmp3.cpp: the hacking multiplayer game's character choice (hackmp3.h).
#include "hackmp3.h"

#include "challenge3.h"
#include "activate.h"
#include "alo.h"
#include "glob.h"
#include "lo.h"
#include "render.h"
#include "scene.h"
#include "shd.h"
#include "sw.h"
#include "game/sly3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace hackmp3
{
namespace
{
struct STATE
{
    bool fEp3Done = false;
    int cplayers = 2;
    std::array<int, 4> aichar{CHAR_Bentley, CHAR_Sly, CHAR_Carmelita, CHAR_Murray};
} s;

int IcharDefault(int iplayer)
{
    switch (iplayer)
    {
    case 0: return CHAR_Bentley;
    case 1: return s.fEp3Done ? CHAR_Penelope : CHAR_Sly;   // (the original's rule)
    case 2: return CHAR_Carmelita;
    default: return CHAR_Murray;
    }
}
}  // namespace

uint32_t RgbaChar(int ichar)
{
    // Bentley green, Sly blue, Penelope pink, Carmelita yellow, Murray purple, the Panda King red
    static const uint32_t s_argba[CHAR_Max] = {0x80207828u, 0x80803818u, 0x80683C80u, 0x80107C80u, 0x80742C5Cu, 0x80181C80u};
    return s_argba[std::clamp(ichar, 0, CHAR_Max - 1)];
}

const char* SzChar(int ichar)
{
    static const char* const s_asz[CHAR_Max] = {"Bentley", "Sly", "Penelope", "Carmelita", "Murray", "Panda King"};
    return s_asz[std::clamp(ichar, 0, CHAR_Max - 1)];
}

bool FGame(const SW* psw)
{
    // (row 2025: job 3334 / task 1815, h_hack = world 22)
    return psw && psw->filter == 22 && FChallengeRunning3() && OidChallengeJob3() == 3334;
}

int CPlayers() { return s.cplayers; }
int IcharPlayer(int iplayer) { return iplayer >= 0 && iplayer < 4 ? s.aichar[size_t(iplayer)] : CHAR_Bentley; }
void NoteProgress(bool fEpisode3Done) { s.fEp3Done = fEpisode3Done; }

int CChainLinks(const bool* afIn, int cplayers, int (*alink)[2])
{
    int ain[4];
    int cin = 0;
    for (int i = 0; i < std::clamp(cplayers, 0, 4); ++i)
        if (afIn[i]) ain[cin++] = i;
    if (cin < 2) return 0;
    if (cin == 2)
    {
        alink[0][0] = ain[0], alink[0][1] = ain[1];   // (one laser, as the original's two players)
        return 1;
    }
    for (int k = 0; k < cin; ++k) alink[k][0] = ain[k], alink[k][1] = ain[(k + 1) % cin];
    return cin;
}

void ResetChoice(int cplayers)
{
    s.cplayers = std::clamp(cplayers, 2, 4);
    for (int i = 0; i < 4; ++i) s.aichar[size_t(i)] = IcharDefault(i);
}

void SetCharPlayer(int iplayer, int ichar)
{
    if (iplayer >= 0 && iplayer < 4) s.aichar[size_t(iplayer)] = std::clamp(ichar, 0, CHAR_Max - 1);
}

int IcharStep(int iplayer, int ichar, int d)
{
    for (int k = 1; k <= CHAR_Max; ++k)
    {
        const int i = ((ichar + d * k) % CHAR_Max + CHAR_Max) % CHAR_Max;
        bool fTaken = false;
        for (int p = 0; p < s.cplayers; ++p)
            if (p != iplayer && s.aichar[size_t(p)] == i) fTaken = true;
        if (!fTaken) return i;
    }
    return ichar;
}
namespace
{
// the ship model's colour sets (h_hack's shaders: dark / mid / bright of each), the set's mid colour (0xAABBGGRR)
constexpr int kSetGreen = 0, kSetBlue = 1, kSetPink = 2;
int IsetOfShader(int32_t id)
{
    if (id >= 0x16BD5 && id <= 0x16BD7) return kSetGreen;
    if (id >= 0x16BD8 && id <= 0x16BDA) return kSetPink;
    if (id >= 0x16BDB && id <= 0x16BDD) return kSetBlue;
    return -1;
}
int IsetOfGlob(const GLOB& g)
{
    for (const SUBGLOB& sg : g.asubglob)
        if (sg.pshd)
            if (const int i = IsetOfShader(sg.pshd->id); i >= 0) return i;
    return -1;
}
// the character's colour on the ship: its own set (Bentley, Sly, Penelope), else the blue set recoloured to this mid
// colour (R, G, B 0..255: Carmelita's yellow, Murray's purple, the Panda King's red, in the sets' style)
struct TINT { int iset; int r, g, b; };
TINT TintOf(int ichar)
{
    switch (ichar)
    {
    case CHAR_Bentley: return {kSetGreen, 0, 0, 0};
    case CHAR_Penelope: return {kSetPink, 0, 0, 0};
    case CHAR_Carmelita: return {kSetBlue, 0xC8, 0x9C, 0x10};
    case CHAR_Murray: return {kSetBlue, 0x6C, 0x26, 0x9A};
    case CHAR_PandaKing: return {kSetBlue, 0xB8, 0x1C, 0x14};
    default: return {kSetBlue, -1, 0, 0};
    }
}
// the recoloured globs' vertex colours as they were (put back by EndShipScene)
struct SAVEDCOLS { GLOB* pglob; const GLGLOB* pgl; std::vector<std::pair<uint32_t, uint32_t>> acol; };
std::vector<SAVEDCOLS> s_asaved;

uint32_t Recolour(uint32_t rgba, const TINT& t)
{
    // (the blue set: its blue channel is the shade; the mid shader's blue 0x83)
    const float k = float((rgba >> 16) & 0xFF) / float(0x83);
    auto c = [&](int v) { return uint32_t(std::clamp(int(std::lround(float(v) * k)), 0, 255)); };
    return (rgba & 0xFF000000u) | (c(t.b) << 16) | (c(t.g) << 8) | c(t.r);
}
}  // namespace

bool FShipScene(SW* psw, const SCENE& scene, int ichar, SCENE* pout, glm::vec3* pposCenter, float* psRadius)
{
    EndShipScene();
    if (!psw || !pout) return false;
    // the first ship (the 2P game has two instances, 200 apart)
    LO* pship = nullptr;
    for (LO* plo : psw->mpcidaplo[0x4A])
        if (!pship || plo->iclone < pship->iclone) pship = plo;
    if (!pship) return false;
    if (static const bool s_f = std::getenv("SLY3_HACKCARDPRINT") != nullptr; s_f)
        for (LO* plo : psw->mpcidaplo[0x4A])
            std::printf("hackship %d clone %d in world %d at %.0f %.0f %.0f\n", plo->oid, plo->iclone, int(plo->fInWorld), PosLoWorld(plo).x, PosLoWorld(plo).y, PosLoWorld(plo).z);
    const glm::vec3 posShip = PosLoWorld(pship);
    std::vector<const GLOBSET*> apgs;
    for (const auto& up : psw->aploAll)
    {
        LO* plo = up.get();
        if ((plo != pship && !FLoInParentChain(plo, pship)) || !FCidDerivedFrom(plo->Cid(), sly3::CID_ALO)) continue;
        if (plo->Cid() == 0x61 || plo->Cid() == 0x81) continue;   // (its barrier: the shield's red ring; its aim line)
        // (the parts the ship's own start does not put in the world stay out: its start event 17 (h_hack's splice, block
        // 92992) adds 74172 / 74183, the body and the gun; the power-ups' parts (the shield's glow 74174, the arrows 93018,
        // 74153, 36161) are added only while a power-up runs (its s5111 / 2465 code))
        bool fOut = false;
        for (LO* pa = plo; pa && pa != pship; pa = pa->paloParent) fOut = fOut || pa->fRemoved;
        if (fOut) continue;
        if (std::getenv("SLY3_HACKCARDPRINT")) std::printf("shippart %d cid 0x%X in world %d removed %d remove-at-start %d parent %d\n", plo->oid, plo->Cid(), int(plo->fInWorld), int(plo->fRemoved), int(plo->fRemoveAtStart), plo->paloParent ? plo->paloParent->oid : -1);
        if (const ALO* pa = static_cast<const ALO*>(plo); pa->pglobset) apgs.push_back(pa->pglobset.get());
    }
    const TINT t = TintOf(ichar);
    pout->aglobset.clear();
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.pglobset || std::find(apgs.begin(), apgs.end(), sgs.pglobset.get()) == apgs.end()) continue;
        if (sgs.amatGlob.empty() || glm::length(glm::vec3(sgs.amatGlob[0][3]) - posShip) > 80.0f) continue;   // (this instance)
        if (static const char* s_szOnly = std::getenv("SLY3_HACKCARDONLY"); s_szOnly && sgs.oid != std::atoi(s_szOnly)) continue;   // (debug: one part)
        SCENEGLOBSET c = sgs;
        c.fForeground = false;
        c.irtgt = -1;
        c.sMRD = 1e10f;
        c.gAlpha = c.gAlpha2 = 1.0f;
        c.gFlash = 0.0f;
        for (size_t i = 0; i < c.afVisible.size() && i < c.pglobset->aglob.size(); ++i)
        {
            GLOB& g = c.pglobset->aglob[i];
            const int iset = IsetOfGlob(g);
            c.afVisible[i] = iset < 0 || iset == t.iset ? 1 : 0;
            if (!c.afVisible[i]) continue;
            for (const VTXGLOB& v : g.avtx)
            {
                const glm::vec3 p = glm::vec3(c.amatGlob[i] * glm::vec4(v.pos, 1.0f));
                lo = glm::min(lo, p), hi = glm::max(hi, p);
            }
            if (iset == kSetBlue && t.r >= 0)
            {
                const GLGLOB* pgl = i < c.apglInst.size() && c.apglInst[i] ? c.apglInst[i] : g.pgl;
                SAVEDCOLS sv{&g, pgl, {}};
                sv.acol.reserve(g.avtx.size());
                for (VTXGLOB& v : g.avtx)
                {
                    sv.acol.emplace_back(v.rgbaA, v.rgbaB);
                    v.rgbaA = Recolour(v.rgbaA, t);
                    v.rgbaB = Recolour(v.rgbaB, t);
                }
                UpdateGlobColorsGpu(g, pgl);
                s_asaved.push_back(std::move(sv));
            }
        }
        if (static const bool s_f = std::getenv("SLY3_HACKCARDPRINT") != nullptr; s_f)
            for (size_t i = 0; i < c.afVisible.size() && i < c.pglobset->aglob.size(); ++i)
            {
                const GLOB& g = c.pglobset->aglob[i];
                glm::vec3 l(1e9f), h(-1e9f);
                for (const VTXGLOB& v : g.avtx) { const glm::vec3 p = glm::vec3(c.amatGlob[i] * glm::vec4(v.pos, 1.0f)); l = glm::min(l, p), h = glm::max(h, p); }
                std::printf("shipglob entry %d (palo %d cid 0x%X) glob %zu n0 %d set %d vis %d rp 0x%X vtx %zu bounds %.0f..%.0f %.0f..%.0f %.0f..%.0f\n", c.oid,
                            c.pglobset->palo ? c.pglobset->palo->oid : -1, c.pglobset->palo ? c.pglobset->palo->Cid() : -1, i, g.n0, IsetOfGlob(g), int(c.afVisible[i]),
                            g.grfGroup, g.avtx.size(), l.x, h.x, l.y, h.y, l.z, h.z);
            }
        pout->aglobset.push_back(std::move(c));
    }
    if (pout->aglobset.empty() || lo.x > hi.x) return false;
    *pposCenter = 0.5f * (lo + hi);
    *psRadius = std::max(0.5f * glm::length(hi - lo), 1.0f);
    return true;
}

void EndShipScene()
{
    for (SAVEDCOLS& sv : s_asaved)
    {
        for (size_t k = 0; k < sv.acol.size() && k < sv.pglob->avtx.size(); ++k)
            sv.pglob->avtx[k].rgbaA = sv.acol[k].first, sv.pglob->avtx[k].rgbaB = sv.acol[k].second;
        UpdateGlobColorsGpu(*sv.pglob, sv.pgl);
    }
    s_asaved.clear();
}
}  // namespace hackmp3
