#include "frameinterp.h"

#include "scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

int FRAMEACCUM::Steps(double tNow)
{
    if (fpsVirtual > 0.0) tNow = tLast < 0.0 ? 0.0 : tLast + 1.0 / fpsVirtual;
    if (tLast < 0.0)
    {
        // the first frame: one step, drawn at it
        tLast = tNow;
        tAcc = 0.0;
        return 1;
    }
    tAcc += std::clamp(tNow - tLast, 0.0, 0.25);
    tLast = tNow;
    // (a hair of tolerance: a display at exactly 60 Hz with a jittery clock still takes one step a frame)
    int c = int((tAcc + 1e-4) / DT_STEP);
    tAcc = std::max(0.0, tAcc - c * DT_STEP);
    if (c > CSTEP_MAX)
    {
        c = CSTEP_MAX;   // a hitch: the time beyond the cap is dropped (no snowballing catch-up)
        tAcc = 0.0;
    }
    return c;
}

namespace
{
struct ENTRYSNAP
{
    glm::vec3 posBounds{0.0f};
    float sBoundsRadius = 0.0f;
    std::vector<glm::mat4> amat;
    std::vector<std::vector<glm::mat4>> aamatBone;
};
struct STEPSNAP
{
    bool fValid = false;
    std::vector<ENTRYSNAP> aentry;
    CAMERA cam;
};
STEPSNAP s_aSnap[2];   // [0] the earlier step, [1] the latest
int s_isnapCur = 1;

// teleports, respawns and cuts: no interpolation across a jump of this many units in one 1/60 s step
constexpr float S_JUMP = 400.0f;

glm::mat4 Lerp(const glm::mat4& a, const glm::mat4& b, float u)
{
    glm::mat4 m;
    for (int c = 0; c < 4; ++c) m[c] = a[c] + (b[c] - a[c]) * u;
    return m;
}

bool FJumped(const glm::mat4& a, const glm::mat4& b)
{
    return glm::distance(glm::vec3(a[3]), glm::vec3(b[3])) > S_JUMP;
}
}  // namespace

void SnapshotStep(const SCENE& scene, const CAMERA& cam)
{
    s_isnapCur ^= 1;
    STEPSNAP& s = s_aSnap[s_isnapCur];
    s.fValid = true;
    s.cam = cam;
    s.aentry.resize(scene.aglobset.size());
    for (size_t k = 0; k < scene.aglobset.size(); ++k)
    {
        s.aentry[k].posBounds = scene.aglobset[k].posBounds;
        s.aentry[k].sBoundsRadius = scene.aglobset[k].sBoundsRadius;
        s.aentry[k].amat = scene.aglobset[k].amatGlob;
        s.aentry[k].aamatBone = scene.aglobset[k].aamatBone;
    }
}

void ResetStepSnapshots()
{
    s_aSnap[0].fValid = s_aSnap[1].fValid = false;
}

void InterpolateSteps(SCENE& scene, CAMERA& cam, float u)
{
    const STEPSNAP& cur = s_aSnap[s_isnapCur];
    const STEPSNAP& prev = s_aSnap[s_isnapCur ^ 1];
    if (!cur.fValid) return;
    cam = cur.cam;
    u = std::clamp(u, 0.0f, 1.0f);
    const bool fPrev = prev.fValid;
    if (fPrev && prev.cam.fMat == cur.cam.fMat && glm::distance(prev.cam.pos, cur.cam.pos) <= S_JUMP)
    {
        cam.pos = prev.cam.pos + (cur.cam.pos - prev.cam.pos) * u;
        cam.fovY = prev.cam.fovY + (cur.cam.fovY - prev.cam.fovY) * u;
        cam.rMRDAdjust = prev.cam.rMRDAdjust + (cur.cam.rMRDAdjust - prev.cam.rMRDAdjust) * u;
        if (cam.fMat)
        {
            // the rows lerped and made square again (x view, y left, z up)
            glm::vec3 x = glm::normalize(prev.cam.mat[0] + (cur.cam.mat[0] - prev.cam.mat[0]) * u);
            glm::vec3 z = prev.cam.mat[2] + (cur.cam.mat[2] - prev.cam.mat[2]) * u;
            z = glm::normalize(z - x * glm::dot(z, x));
            cam.mat[0] = x;
            cam.mat[1] = glm::cross(z, x);
            cam.mat[2] = z;
        }
        else
        {
            cam.yaw = prev.cam.yaw + (cur.cam.yaw - prev.cam.yaw) * u;
            cam.pitch = prev.cam.pitch + (cur.cam.pitch - prev.cam.pitch) * u;
        }
    }
    for (size_t k = 0; k < scene.aglobset.size() && k < cur.aentry.size(); ++k)
    {
        SCENEGLOBSET& sgs = scene.aglobset[k];
        const ENTRYSNAP& ec = cur.aentry[k];
        const ENTRYSNAP* pep = fPrev && k < prev.aentry.size() ? &prev.aentry[k] : nullptr;
        const bool fBounds = pep && glm::distance(pep->posBounds, ec.posBounds) <= S_JUMP;
        sgs.posBounds = fBounds ? pep->posBounds + (ec.posBounds - pep->posBounds) * u : ec.posBounds;
        sgs.sBoundsRadius = fBounds ? pep->sBoundsRadius + (ec.sBoundsRadius - pep->sBoundsRadius) * u : ec.sBoundsRadius;
        if (sgs.amatGlob.size() != ec.amat.size()) continue;
        for (size_t i = 0; i < ec.amat.size(); ++i)
        {
            const bool f = pep && i < pep->amat.size() && pep->amat.size() == ec.amat.size() && !FJumped(pep->amat[i], ec.amat[i]);
            sgs.amatGlob[i] = f ? Lerp(pep->amat[i], ec.amat[i], u) : ec.amat[i];
        }
        if (sgs.aamatBone.size() != ec.aamatBone.size()) continue;
        for (size_t i = 0; i < ec.aamatBone.size(); ++i)
        {
            const std::vector<glm::mat4>& bc = ec.aamatBone[i];
            std::vector<glm::mat4>& b = sgs.aamatBone[i];
            if (b.size() != bc.size()) continue;
            const bool fp = pep && i < pep->aamatBone.size() && pep->aamatBone[i].size() == bc.size();
            for (size_t j = 0; j < bc.size(); ++j)
                b[j] = fp && !FJumped(pep->aamatBone[i][j], bc[j]) ? Lerp(pep->aamatBone[i][j], bc[j], u) : bc[j];
        }
    }
}
