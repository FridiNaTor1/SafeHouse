// VR's First Person mode: the eyes' place, the hands and the cane (vrfirstperson.h).
#include "vrfirstperson.h"
#include "vr.h"
#include "activate.h"
#include "actor.h"
#include "bone.h"
#include "cls.h"
#include "gang.h"
#include "glob.h"
#include "lo.h"
#include "po.h"
#include "scene.h"
#include "sly.h"
#include "so.h"
#include "sw.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace vrfp
{
namespace
{
constexpr int kDepthMax = 128;   // (a guard: the records' references form a tree)
constexpr float kCaneSize = 0.9f;   // (the cane drawn in the hand a little smaller than the art's: the player's test)
using MPMAT = std::unordered_map<const XF*, glm::mat4>;

bool FLog() { static const bool s_f = std::getenv("SLY2N_VR_FPLOG") != nullptr; return s_f; }

bool FUnderLo(const LO* plo, const LO* ploRoot)
{
    for (int i = 0; plo && i < kDepthMax; plo = plo->paloParent, ++i)
        if (plo == ploRoot) return true;
    return false;
}

bool FUnderXf(const XF* pxf, const XF* pxfRoot)
{
    if (!pxfRoot) return false;
    for (int i = 0; pxf && i < kDepthMax; pxf = pxf->pxfRef, ++i)
        if (pxf == pxfRoot) return true;
    return false;
}

// scenesw.cpp's rule: a group without bones of its own uses its template's bindings
int IbindOf(const GLOBSET& gs, size_t i)
{
    const GLOB& g = gs.aglob[i];
    return g.iglobTemplate >= 0 && g.abone.empty() ? g.iglobTemplate : int(i);
}

const GLOB& GeomOf(const GLOBSET& gs, size_t i)
{
    const GLOB& g = gs.aglob[i];
    return !g.avtx.empty() || g.iglobTemplate < 0 ? g : gs.aglob[size_t(g.iglobTemplate)];
}

// ------------------------------------------------------------ who plays, where the eyes are
struct PLAYER
{
    ALO* palo = nullptr;
    LO* ploCane = nullptr;        // (Sly: the cane in his hand, SLY +0x19A0)
    LO* ploCanePoint = nullptr;   // (its hook's point, +0x19A4)
    const XF* pxfSeedR = nullptr; // (Bentley: his swing's hand record)
    float sEye = 0.0f;            // the eye above the centre, game units
};

bool FPlayerOf(SW* psw, PLAYER* pp)
{
    *pp = PLAYER{};
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) return false;
    ALO* palo = static_cast<ALO*>(plo);
    if (SLY* psly = PslyGlobal(); psly && psly->palo == palo)
    {
        pp->ploCane = psly->plo19A0;
        pp->ploCanePoint = psly->plo19A4;
        pp->sEye = 25.0f;
    }
    else if (BENTLEY* pb = PbentleyFromLo(plo)) pp->pxfSeedR = pb->pxf10B8, pp->sEye = 10.0f;
    else if (PmurrayFromLo(plo)) pp->sEye = 45.0f;
    else return false;   // (a vehicle, the hack ship, Carmelita: no First Person)
    pp->palo = palo;
    return true;
}

// the eyes between steps: the player's centre at the last two steps
struct EYESTEP
{
    const ALO* palo = nullptr;
    bool fPrev = false, fCur = false;
    glm::vec3 posPrev{0.0f}, posCur{0.0f};
} s_eye;

// ------------------------------------------------------------ the hands (found once per model)
struct HANDS
{
    const ALO* palo = nullptr;
    size_t cbone = 0;
    uint32_t serial = 0;
    const XF* apxfHand[2]{};   // 0 left, 1 right
    const XF* apxfArm[2]{};    // each hand's forearm (moved with it)
    const char* szHow = "";
    bool fT = false;           // the hands' turns in the character's own T-pose known (amatT)
    glm::mat3 amatT[2]{glm::mat3(1.0f), glm::mat3(1.0f)};
} s_hands;

// A bound record's place in the bind pose (the model's space: Z up, -Y the character's right)
glm::vec3 PosBind(const MPMAT& mpbind, const XF* pxf) { return glm::vec3(glm::inverse(mpbind.at(pxf))[3]); }

// The hand on side s (0 left, 1 right) of a model bound in a T-pose (Sly's, Bentley's and Murray's: the hands level with
// the shoulders, out to the sides): where that arm ends, the record common to the bones reaching furthest out (Murray's
// fingers: his hand) when it is bound and still well out, else the furthest bone
const XF* PxfArmEnd(const std::vector<const XF*>& abone, const MPMAT& mpbind, int s)
{
    const float sgn = s == 1 ? -1.0f : 1.0f;
    float yMax = 0.0f;
    for (const XF* pxf : abone) yMax = std::max(yMax, sgn * PosBind(mpbind, pxf).y);
    if (yMax < 1.0f) return nullptr;
    std::vector<const XF*> aend;
    for (const XF* pxf : abone)
        if (sgn * PosBind(mpbind, pxf).y >= 0.85f * yMax) aend.push_back(pxf);
    const XF* pxfFar = aend[0];
    for (const XF* pxf : aend)
        if (sgn * PosBind(mpbind, pxf).y > sgn * PosBind(mpbind, pxfFar).y) pxfFar = pxf;
    const XF* pxfCommon = nullptr;
    int i = 0;
    for (const XF* pxf = aend[0]; pxf && !pxfCommon && i < kDepthMax; pxf = pxf->pxfRef, ++i)
        if (std::all_of(aend.begin(), aend.end(), [&](const XF* o) { return FUnderXf(o, pxf); })) pxfCommon = pxf;
    i = 0;
    for (const XF* pxf = pxfCommon; pxf && i < kDepthMax; pxf = pxf->pxfRef, ++i)
        if (mpbind.count(pxf)) return sgn * PosBind(mpbind, pxf).y >= 0.4f * yMax ? pxf : pxfFar;
    return pxfFar;
}

// Hand pxfHand's forearm: the bound bone on its side, not under it, nearer the body, nearest to it in the bind pose
const XF* PxfForearm(const std::vector<const XF*>& abone, const MPMAT& mpbind, const XF* pxfHand, int s)
{
    if (!pxfHand) return nullptr;
    const float sgn = s == 1 ? -1.0f : 1.0f;
    const glm::vec3 posHand = PosBind(mpbind, pxfHand);
    const XF* pxfBest = nullptr;
    float dBest = 0.6f * sgn * posHand.y;
    for (const XF* pxf : abone)
    {
        if (pxf == pxfHand || FUnderXf(pxf, pxfHand)) continue;
        const glm::vec3 p = PosBind(mpbind, pxf);
        if (sgn * p.y <= 0.0f || sgn * p.y >= sgn * posHand.y) continue;
        if (const float d = glm::length(p - posHand); d < dBest) dBest = d, pxfBest = pxf;
    }
    return pxfBest;
}

// Each hand's turn in the character's own T-pose, in its body's axes (right, up, back): its bind pose, a forearm bound more
// than 15 degrees off level (an A-pose) turned level
void CharacterTPose(const MPMAT& mpbind)
{
    s_hands.fT = false;
    glm::mat4 amat[2];
    for (int k = 0; k < 2; ++k)
    {
        if (!s_hands.apxfHand[k] || !mpbind.count(s_hands.apxfHand[k])) return;
        amat[k] = glm::inverse(mpbind.at(s_hands.apxfHand[k]));
    }
    const glm::vec3 apos[2]{glm::vec3(amat[0][3]), glm::vec3(amat[1][3])};
    glm::vec3 d = apos[1] - apos[0];
    const float sSpan = glm::length(d);
    if (sSpan < 1e-3f || std::fabs(d.z) > 0.35f * sSpan) return;   // (not level: the model's space is not Z up)
    d.z = 0.0f;
    const glm::vec3 right = glm::normalize(d), up(0.0f, 0.0f, 1.0f), back = glm::cross(right, up);
    const glm::mat3 matBody(right, up, back);
    for (int k = 0; k < 2; ++k)
    {
        glm::mat3 r(amat[k]);
        for (int i = 0; i < 3; ++i) r[i] = glm::normalize(r[i]);
        if (glm::determinant(r) < 0.0f) return;   // (a mirrored bind: unknown)
        if (const XF* pxfArm = s_hands.apxfArm[k]; pxfArm && mpbind.count(pxfArm))
        {
            const glm::vec3 dirArm = glm::normalize(apos[k] - PosBind(mpbind, pxfArm));
            const float radElev = std::asin(std::clamp(dirArm.z, -1.0f, 1.0f));
            const glm::vec3 dirLevel(dirArm.x, dirArm.y, 0.0f);
            if (std::fabs(radElev) > glm::radians(15.0f) && glm::length(dirLevel) > 1e-3f)
            {
                const glm::vec3 axis = glm::cross(dirArm, glm::normalize(dirLevel));
                if (glm::length(axis) > 1e-4f) r = glm::mat3_cast(glm::angleAxis(std::fabs(radElev), glm::normalize(axis))) * r;
            }
        }
        s_hands.amatT[k] = glm::transpose(matBody) * r;
    }
    s_hands.fT = true;
}

void FindHands(const std::vector<const XF*>& abone, const MPMAT& mpbind, const XF* pxfHeldAt, const PLAYER& p, uint32_t serial)
{
    s_hands = HANDS{};
    s_hands.palo = p.palo;
    s_hands.cbone = abone.size();
    s_hands.serial = serial;
    const XF* pxfR = nullptr;
    if (p.pxfSeedR && mpbind.count(p.pxfSeedR) && PosBind(mpbind, p.pxfSeedR).y < 0.0f)
        pxfR = p.pxfSeedR, s_hands.szHow = "the game's hand record";
    if (!pxfR && pxfHeldAt)
    {
        int i = 0;
        for (const XF* pxf = pxfHeldAt; pxf && !pxfR && i < kDepthMax; pxf = pxf->pxfRef, ++i)
            if (mpbind.count(pxf)) pxfR = pxf;
        if (pxfR && PosBind(mpbind, pxfR).y >= 0.0f) pxfR = nullptr;   // (not on the right: not a hand)
        if (pxfR) s_hands.szHow = "what it holds";
    }
    if (!pxfR && (pxfR = PxfArmEnd(abone, mpbind, 1))) s_hands.szHow = "the bind pose";
    s_hands.apxfHand[0] = PxfArmEnd(abone, mpbind, 0);
    s_hands.apxfHand[1] = pxfR;
    for (int s = 0; s < 2; ++s) s_hands.apxfArm[s] = PxfForearm(abone, mpbind, s_hands.apxfHand[s], s);
    CharacterTPose(mpbind);
    if (FLog())
        std::printf("vr first person: oid %d, %zu bones; hands %s (the right by %s), T-pose %s\n", int(p.palo->oid), abone.size(),
                    s_hands.apxfHand[0] && s_hands.apxfHand[1] ? "found" : "missing", s_hands.szHow, s_hands.fT ? "known" : "unknown");
}

// A grip held in a T-pose with the palm down (OpenXR's grip axes in the body's: right, up, back): the right one's +X (into
// the palm) up, -Z (the fist, to the thumb) forward; the left one's +X (out of the palm) down
const glm::mat3& MatGripTPose(int k)
{
    static const glm::mat3 s_aMat[2]{glm::mat3(glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1)),
                                     glm::mat3(glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1))};
    return s_aMat[k];
}

// Hand k's place this frame: its grip, turned so that a grip in a T-pose holds the character's hand in its T-pose
bool FHandWorld(int k, glm::mat4* pmat)
{
    glm::mat4 m(1.0f);
    if (!vr::FGripWorld(k, &m)) return false;
    if (s_hands.fT) m = m * glm::mat4(glm::transpose(MatGripTPose(k)) * s_hands.amatT[k]);
    *pmat = m;
    return true;
}

// ------------------------------------------------------------ the cane, by its shape
// In the space of its first group's matrix: the frame its grip point sits at (origin where the hand holds it, -Z along the
// shaft toward the hook, +Y away from the side the crook curls to)
struct CANEGEO
{
    const LO* plo = nullptr;
    uint32_t serial = 0;
    bool fOk = false;
    glm::mat4 matFrame{1.0f};
} s_cane;

void MeasureCaneVertices(const std::vector<glm::vec3>& apos, const glm::vec3& posGrip, const glm::vec3* pposPoint, CANEGEO* pg)
{
    pg->fOk = false;
    if (apos.size() < 8) return;
    auto fit = [](const std::vector<glm::vec3>& ap, glm::vec3* pc, glm::vec3* pa) {
        glm::vec3 c(0.0f);
        for (const glm::vec3& p : ap) c += p;
        c /= float(ap.size());
        glm::mat3 cov(0.0f);
        for (const glm::vec3& p : ap) cov += glm::outerProduct(p - c, p - c);
        glm::vec3 a = glm::normalize(glm::vec3(1.0f, 0.3f, 0.1f));
        for (int it = 0; it < 64; ++it) a = glm::normalize(cov * a);   // (the main axis, by power iteration)
        *pc = c, *pa = a;
    };
    glm::vec3 c, a;
    fit(apos, &c, &a);
    float tMin = 1e9f, tMax = -1e9f;
    for (const glm::vec3& p : apos) tMin = std::min(tMin, glm::dot(p - c, a)), tMax = std::max(tMax, glm::dot(p - c, a));
    const float sLen = tMax - tMin;
    if (sLen < 10.0f) return;
    // the shaft: the cane's one long straight run (a thin rod at about 59 degrees to the whole cane's main axis, which the
    // crook's many vertices pull away): the line through two vertices that the most vertices lie on, then fitted to them
    const float sOnLine = std::max(2.5f, 0.016f * sLen);
    const size_t cv = apos.size(), dv = std::max<size_t>(1, cv / 90);
    int cBest = 0;
    float spanBest = 0.0f;
    glm::vec3 cs = c, s_ = a;
    for (size_t i = 0; i < cv; i += dv)
        for (size_t j = i + 1; j < cv; j += dv)
        {
            const glm::vec3 d = apos[j] - apos[i];
            if (glm::length(d) < 0.3f * sLen) continue;
            const glm::vec3 u = glm::normalize(d);
            int cn = 0;
            float t0 = 1e9f, t1 = -1e9f;
            for (const glm::vec3& p : apos)
            {
                const glm::vec3 q = p - apos[i];
                const float t = glm::dot(q, u);
                if (glm::length(q - t * u) < sOnLine) ++cn, t0 = std::min(t0, t), t1 = std::max(t1, t);
            }
            if (cn > cBest || (cn == cBest && t1 - t0 > spanBest)) cBest = cn, spanBest = t1 - t0, cs = apos[i], s_ = u;
        }
    std::vector<glm::vec3> aline;
    for (int it = 0; it < 2; ++it)
    {
        std::vector<glm::vec3> ain;
        for (const glm::vec3& p : apos)
            if (glm::length((p - cs) - glm::dot(p - cs, s_) * s_) < sOnLine * (it ? 1.0f : 1.5f)) ain.push_back(p);
        if (ain.size() < 4) break;
        glm::vec3 c2, s2;
        fit(ain, &c2, &s2);
        cs = c2, s_ = s2;
        aline.swap(ain);
    }
    // toward the hook: the game's cane point, else the cane's centre (the crook's mass)
    const bool fPoint = pposPoint != nullptr;
    const glm::vec3 posPoint = fPoint ? *pposPoint : glm::vec3(0.0f);
    if (glm::dot((fPoint ? posPoint : c) - cs, s_) < 0.0f) s_ = -s_;
    const glm::vec3 posG = cs + glm::dot(posGrip - cs, s_) * s_;   // (the hand's record brought onto the shaft)
    // which way the crook curls: the hook point's side of the shaft, else the side of the vertices off it
    glm::vec3 curl = fPoint ? (posPoint - cs) - glm::dot(posPoint - cs, s_) * s_ : glm::vec3(0.0f);
    if (glm::length(curl) < 0.05f * sLen)
    {
        glm::vec3 sum(0.0f);
        int cn = 0;
        for (const glm::vec3& p : apos)
            if (const glm::vec3 d = (p - cs) - glm::dot(p - cs, s_) * s_; glm::length(d) >= sOnLine) sum += d, ++cn;
        if (cn) curl = sum / float(cn);
    }
    const glm::vec3 z = -s_;
    glm::vec3 y = glm::length(curl) > 0.01f * sLen ? -glm::normalize(curl) : glm::normalize(glm::cross(z, glm::vec3(0.0f, 0.0f, 1.0f)));
    if (!(glm::length(y) > 0.5f)) y = glm::normalize(glm::cross(z, glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::vec3 x = glm::normalize(glm::cross(y, z));
    y = glm::cross(z, x);
    pg->matFrame = glm::mat4(glm::vec4(x, 0.0f), glm::vec4(y, 0.0f), glm::vec4(z, 0.0f), glm::vec4(posG, 1.0f));
    pg->fOk = true;
    if (FLog())
        std::printf("vr first person: the cane: %zu vertices, %.1f across, the shaft on %zu, %.1f from the hand's record\n", apos.size(),
                    sLen, aline.size(), glm::length(posGrip - posG));
}

void MeasureCane(const SCENE& scene, const std::vector<size_t>& akHeld, const glm::mat4& matRef, const glm::vec3& posGripWorld,
                 const glm::vec3* pposPointWorld, CANEGEO* pg)
{
    const glm::mat4 matInv = glm::inverse(matRef);
    std::vector<glm::vec3> apos;
    for (size_t k : akHeld)
    {
        const SCENEGLOBSET& sgs = scene.aglobset[k];
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size() && i < sgs.amatGlob.size(); ++i)
        {
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty()) continue;   // (skinned: not its rigid shape)
            const glm::mat4 m = matInv * sgs.amatGlob[i];
            for (const VTXGLOB& v : GeomOf(gs, i).avtx) apos.push_back(glm::vec3(m * glm::vec4(v.pos, 1.0f)));
        }
    }
    const glm::vec3 posGrip(matInv * glm::vec4(posGripWorld, 1.0f));
    const glm::vec3 posPoint = pposPointWorld ? glm::vec3(matInv * glm::vec4(*pposPointWorld, 1.0f)) : glm::vec3(0.0f);
    MeasureCaneVertices(apos, posGrip, pposPointWorld ? &posPoint : nullptr, pg);
}

// ------------------------------------------------------------ the frame's changes, and their restore
struct SAVE
{
    std::vector<std::pair<size_t, std::vector<uint8_t>>> avis;
    std::vector<std::tuple<size_t, std::vector<glm::mat4>, std::vector<std::vector<glm::mat4>>>> amat;
    std::vector<std::tuple<size_t, bool, glm::vec3, float>> abounds;
} s_save;

void Save(SCENE& scene, size_t k)
{
    const SCENEGLOBSET& sgs = scene.aglobset[k];
    s_save.avis.emplace_back(k, sgs.afVisible);
    s_save.amat.emplace_back(k, sgs.amatGlob, sgs.aamatBone);
    s_save.abounds.emplace_back(k, sgs.fBounds, sgs.posBounds, sgs.sBoundsRadius);
}

// a group shown or hidden: the shadow groups always as the game has them (the whole body's shadow stays)
void HideGroup(SCENEGLOBSET& sgs, size_t i)
{
    if (i >= sgs.afVisible.size() || GeomOf(*sgs.pglobset, i).meshk == MESHK_Effect) return;
    sgs.afVisible[i] = 0;
}

void MoveHeld(SCENEGLOBSET& sgs, const glm::mat4& mat)
{
    for (glm::mat4& m : sgs.amatGlob) m = mat * m;
    for (auto& amatBone : sgs.aamatBone)
        for (glm::mat4& m : amatBone) m = mat * m;
    sgs.posBounds = glm::vec3(mat * glm::vec4(sgs.posBounds, 1.0f));
}
}  // namespace

void NoteStep(SW* psw)
{
    PLAYER p;
    if (!vr::FActive() || !FPlayerOf(psw, &p))
    {
        s_eye = EYESTEP{};
        return;
    }
    const glm::vec3 pos = PosAloCenter(p.palo);
    if (s_eye.palo != p.palo) s_eye = EYESTEP{}, s_eye.palo = p.palo;
    s_eye.posPrev = s_eye.posCur, s_eye.fPrev = s_eye.fCur;
    s_eye.posCur = pos, s_eye.fCur = true;
}

void PrepareFrame(SW* psw, float u, bool fInterp)
{
    PLAYER p;
    if (!FPlayerOf(psw, &p) || s_eye.palo != p.palo || !s_eye.fCur)
    {
        vr::SetFirstPersonBody(false, glm::vec3(0.0f));
        return;
    }
    glm::vec3 pos = s_eye.posCur;
    // (between the last two steps as the scene is drawn; the latest across a jump)
    if (fInterp && s_eye.fPrev && glm::distance(s_eye.posPrev, s_eye.posCur) <= 400.0f)
        pos = s_eye.posPrev + (s_eye.posCur - s_eye.posPrev) * std::clamp(u, 0.0f, 1.0f);
    vr::SetFirstPersonBody(true, pos + glm::vec3(0.0f, 0.0f, p.sEye));
}

void ApplyHands(SCENE& scene, const std::vector<ALO*>& apalo, SW* psw)
{
    RestoreHands(scene);   // (nothing changed is left from a frame that was not restored)
    PLAYER p;
    if (!vr::FFirstPersonFrame() || !FPlayerOf(psw, &p)) return;
    std::vector<size_t> akBody, akHeld, akTrail;
    for (size_t k = 0; k < scene.aglobset.size() && k < apalo.size(); ++k)
    {
        if (!apalo[k] || !scene.aglobset[k].pglobset) continue;
        // (a title card's close-up over the HUD, a portrait drawn into a render target: not the player's body)
        if (scene.aglobset[k].fForeground || scene.aglobset[k].irtgt >= 0) continue;
        // (a trail under the cane keeps world-space vertices: it would follow the animation's cane, not the hand's)
        if (p.ploCane && FUnderLo(apalo[k], p.ploCane)) (apalo[k]->Cid() == 0x69 ? akTrail : akHeld).push_back(k);
        else if (FUnderLo(apalo[k], p.palo)) akBody.push_back(k);
    }

    // the model's bones as drawn this frame: each record's world from its skinning matrix (M = world x bind)
    std::vector<const XF*> abone;
    MPMAT mpmatWorld, mpmatBind;
    for (size_t k : akBody)
    {
        const auto* paabind = PaabindOfAlo(apalo[k]);
        const SCENEGLOBSET& sgs = scene.aglobset[k];
        if (!paabind) continue;
        for (size_t i = 0; i < sgs.aamatBone.size() && i < sgs.pglobset->aglob.size(); ++i)
        {
            const auto& amat = sgs.aamatBone[i];
            const int ib = IbindOf(*sgs.pglobset, i);
            if (amat.empty() || ib < 0 || size_t(ib) >= paabind->size() || (*paabind)[size_t(ib)].size() != amat.size()) continue;
            const auto& abind = (*paabind)[size_t(ib)];
            for (size_t j = 0; j < abind.size(); ++j)
                if (const XF* pxf = abind[j].pxfBone; pxf && !mpmatWorld.count(pxf))
                {
                    mpmatWorld[pxf] = amat[j] * glm::inverse(abind[j].matBind);
                    mpmatBind[pxf] = abind[j].matBind;
                    abone.push_back(pxf);
                }
        }
    }
    const ALO* paloCane = p.ploCane && FCidDerivedFrom(p.ploCane->Cid(), 0x30) ? static_cast<const ALO*>(p.ploCane) : nullptr;
    const XF* pxfHeldAt = paloCane && !paloCane->axf.empty() ? paloCane->axf[0]->pxfRef : nullptr;
    const uint32_t serial = psw ? uint32_t(psw->serial) : 0u;
    if (s_hands.palo != p.palo || s_hands.cbone != abone.size() || s_hands.serial != serial ||
        (s_hands.apxfHand[0] && !mpmatWorld.count(s_hands.apxfHand[0])) || (s_hands.apxfHand[1] && !mpmatWorld.count(s_hands.apxfHand[1])))
        FindHands(abone, mpmatBind, pxfHeldAt, p, serial);

    // each hand's move this frame: from where the animation drew it onto the controller
    glm::mat4 amatMove[2]{glm::mat4(1.0f), glm::mat4(1.0f)};
    glm::vec3 aposHand[2]{};
    bool afHand[2]{};
    for (int s = 0; s < 2; ++s)
    {
        const XF* pxf = s_hands.apxfHand[s];
        glm::mat4 matCtrl(1.0f);
        if (!pxf || !mpmatWorld.count(pxf) || !FHandWorld(s, &matCtrl)) continue;
        amatMove[s] = matCtrl * glm::inverse(mpmatWorld[pxf]);
        aposHand[s] = glm::vec3(mpmatWorld[pxf][3]);
        afHand[s] = true;
    }

    // the cane: in the right grip by its own shape (the shaft along the fist, the hook out above the thumb), the right hand
    // closed on it as the animation holds it; no grip tracked: hidden
    if (!akHeld.empty())
    {
        size_t kRef = akHeld[0];
        for (size_t k : akHeld)
            if (apalo[k] == paloCane && !scene.aglobset[k].amatGlob.empty()) { kRef = k; break; }
        const bool fRef = !scene.aglobset[kRef].amatGlob.empty();
        const glm::mat4 matRef = fRef ? scene.aglobset[kRef].amatGlob[0] : glm::mat4(1.0f);
        if (fRef && (s_cane.plo != p.ploCane || s_cane.serial != serial))
        {
            const glm::vec3 posGrip = pxfHeldAt && mpmatWorld.count(pxfHeldAt) ? glm::vec3(mpmatWorld[pxfHeldAt][3])
                                    : pxfHeldAt ? glm::vec3(pxfHeldAt->matWorld[3]) : glm::vec3(matRef[3]);
            // (the cane point is at the step's time, the drawn scene between steps: brought into it through the hand's
            // record, which the cane is rigid with)
            glm::vec3 posPoint = p.ploCanePoint ? PosLoWorld(p.ploCanePoint) : glm::vec3(0.0f);
            if (p.ploCanePoint && pxfHeldAt && mpmatWorld.count(pxfHeldAt))
                posPoint = glm::vec3(mpmatWorld[pxfHeldAt] * glm::inverse(pxfHeldAt->matWorld) * glm::vec4(posPoint, 1.0f));
            MeasureCane(scene, akHeld, matRef, posGrip, p.ploCanePoint ? &posPoint : nullptr, &s_cane);
            s_cane.plo = p.ploCane;
            s_cane.serial = serial;
        }
        glm::mat4 matGrip(1.0f);
        const bool fAt = fRef && s_cane.fOk && vr::FGripWorld(1, &matGrip);
        glm::mat4 matMove(1.0f);
        if (fAt)
        {
            const glm::mat4 matCane = matGrip * glm::scale(glm::mat4(1.0f), glm::vec3(kCaneSize)) * glm::inverse(s_cane.matFrame);
            matMove = matCane * glm::inverse(matRef);
            if (afHand[1]) amatMove[1] = matGrip * glm::inverse(s_cane.matFrame) * glm::inverse(matRef);   // (the glove keeps its size)
        }
        for (size_t k : akHeld)
        {
            SCENEGLOBSET& sgs = scene.aglobset[k];
            Save(scene, k);
            if (fAt) MoveHeld(sgs, matMove);
            else for (size_t i = 0; i < sgs.afVisible.size(); ++i) HideGroup(sgs, i);
        }
        for (size_t k : akTrail)
        {
            Save(scene, k);
            std::fill(scene.aglobset[k].afVisible.begin(), scene.aglobset[k].afVisible.end(), uint8_t(0));
        }
    }

    // the body: hidden outright with no hand tracked
    if (!afHand[0] && !afHand[1])
    {
        for (size_t k : akBody)
        {
            Save(scene, k);
            for (size_t i = 0; i < scene.aglobset[k].afVisible.size(); ++i) HideGroup(scene.aglobset[k], i);
        }
        return;
    }
    // which hand a record moves with (-1: neither): the hand's own records, and its forearm
    auto side = [&](const XF* pxf) {
        for (int s = 1; s >= 0; --s)
            if (afHand[s] && (FUnderXf(pxf, s_hands.apxfHand[s]) || (s_hands.apxfArm[s] && pxf == s_hands.apxfArm[s]))) return s;
        return -1;
    };
    glm::vec3 aposEnd[2]{};   // (each arm's end, the forearm's or the hand's origin, where it is drawn this frame)
    for (int s = 0; s < 2; ++s)
        if (afHand[s])
        {
            const XF* pxfEnd = s_hands.apxfArm[s] ? s_hands.apxfArm[s] : s_hands.apxfHand[s];
            const glm::vec3 posEnd = pxfEnd && mpmatWorld.count(pxfEnd) ? glm::vec3(mpmatWorld[pxfEnd][3]) : aposHand[s];
            aposEnd[s] = glm::vec3(amatMove[s] * glm::vec4(posEnd, 1.0f));
        }
    // a record of neither hand in a group with hand bones: closed up at its own side's arm end (by the bind pose), else the
    // nearer tracked one's (a tiny scale, not zero: a normal or an inverse taken from the matrix stays finite)
    auto collapse = [&](const XF* pxf, const glm::vec3& pos) {
        int s = -1;
        if (pxf && mpmatBind.count(pxf))
        {
            const float y = PosBind(mpmatBind, pxf).y;
            const int sSide = y < 0.0f ? 1 : 0;
            const XF* pxfH = s_hands.apxfHand[sSide];
            if (afHand[sSide] && pxfH && mpmatBind.count(pxfH) && std::fabs(y) > 0.15f * std::fabs(PosBind(mpmatBind, pxfH).y)) s = sSide;
        }
        if (s < 0)
        {
            s = afHand[1] ? 1 : 0;
            if (afHand[0] && afHand[1] && glm::length(pos - aposHand[0]) < glm::length(pos - aposHand[1])) s = 0;
        }
        glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(1e-4f));
        m[3] = glm::vec4(aposEnd[s], 1.0f);
        return m;
    };
    for (size_t k : akBody)
    {
        SCENEGLOBSET& sgs = scene.aglobset[k];
        Save(scene, k);
        sgs.fBounds = false;   // (the whole-object sphere stays at the body; the hands go wherever you reach)
        const auto* paabind = PaabindOfAlo(apalo[k]);
        for (size_t i = 0; i < sgs.amatGlob.size() && i < sgs.pglobset->aglob.size(); ++i)
        {
            if (GeomOf(*sgs.pglobset, i).meshk == MESHK_Effect) continue;   // (the shadow: the whole body's, as animated)
            const bool fSkinned = i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty();
            if (!fSkinned)
            {
                const XF* pxfOwner = i < sgs.apxfOwner.size() ? sgs.apxfOwner[i] : nullptr;
                const int s = pxfOwner ? side(pxfOwner) : -1;
                if (s >= 0) sgs.amatGlob[i] = amatMove[s] * sgs.amatGlob[i];
                else HideGroup(sgs, i);
                continue;
            }
            auto& amat = sgs.aamatBone[i];
            const int ib = IbindOf(*sgs.pglobset, i);
            if (!paabind || ib < 0 || size_t(ib) >= paabind->size() || (*paabind)[size_t(ib)].size() != amat.size())
            {
                HideGroup(sgs, i);
                continue;
            }
            const auto& abind = (*paabind)[size_t(ib)];
            bool fAnyHand = false;
            for (size_t j = 0; j < abind.size() && !fAnyHand; ++j) fAnyHand = abind[j].pxfBone && side(abind[j].pxfBone) >= 0;
            if (!fAnyHand)
            {
                HideGroup(sgs, i);   // (the head, the body, the legs)
                continue;
            }
            for (size_t j = 0; j < amat.size(); ++j)
            {
                const XF* pxf = abind[j].pxfBone;
                const int s = pxf ? side(pxf) : -1;
                if (s >= 0) amat[j] = amatMove[s] * amat[j];
                else amat[j] = collapse(pxf, pxf && mpmatWorld.count(pxf) ? glm::vec3(mpmatWorld[pxf][3]) : glm::vec3(amat[j][3]));
            }
        }
    }
}

void RestoreHands(SCENE& scene)
{
    // (in reverse: an entry saved twice gets its first, untouched copy last)
    for (auto it = s_save.avis.rbegin(); it != s_save.avis.rend(); ++it)
        if (it->first < scene.aglobset.size()) scene.aglobset[it->first].afVisible = std::move(it->second);
    for (auto it = s_save.amat.rbegin(); it != s_save.amat.rend(); ++it)
        if (auto& [k, amat, aamatBone] = *it; k < scene.aglobset.size())
        {
            scene.aglobset[k].amatGlob = std::move(amat);
            scene.aglobset[k].aamatBone = std::move(aamatBone);
        }
    for (auto it = s_save.abounds.rbegin(); it != s_save.abounds.rend(); ++it)
        if (auto& [k, fBounds, posBounds, sRadius] = *it; k < scene.aglobset.size())
        {
            scene.aglobset[k].fBounds = fBounds;
            scene.aglobset[k].posBounds = posBounds;
            scene.aglobset[k].sBoundsRadius = sRadius;
        }
    s_save = SAVE{};
}
}  // namespace vrfp
