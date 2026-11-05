// The scripts' camera keys (cmkey3.h).

#include "cmkey3.h"

#include "game3.h"
#include "scpt3.h"

#include "../lo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
float F(const SCPTCMD3& c, std::size_t i)
{
    if (i >= c.an.size()) return 0.0f;
    float g;
    std::memcpy(&g, &c.an[i], 4);
    return g;
}
int32_t N(const SCPTCMD3& c, std::size_t i) { return i < c.an.size() ? c.an[i] : -1; }

// an object's world matrix (its LOX matrices up its parents; TODO(sly3): the ALO's animated / attached transforms)
glm::mat4 MatWorld(const LO* plo)
{
    glm::mat4 m(1.0f);
    for (const LO* p = plo; p; p = p->paloParent)
        if (const LOX* plox = dynamic_cast<const LOX*>(p)) m = plox->mat * m;
    return m;
}

// orig3: 0x11E3B8 RadNormalize (to -pi..pi)
float RadNormalize(float g)
{
    const float c_2pi = 6.2831853f;
    g = std::fmod(g + 3.14159265f, c_2pi);
    if (g < 0) g += c_2pi;
    return g - 3.14159265f;
}

glm::vec3 EulerFromRows(const glm::vec3& x, const glm::vec3& y)
{
    const float pan = std::atan2(x.y, x.x);
    const float tilt = std::asin(std::clamp(x.z, -1.0f, 1.0f));
    const glm::vec3 left0(-std::sin(pan), std::cos(pan), 0.0f);
    const glm::vec3 up0 = glm::cross(x, left0);
    const float roll = std::atan2(glm::dot(y, up0), glm::dot(y, left0));
    return {roll, -tilt, pan};
}

glm::vec3 EulerLookAt(const glm::vec3& d, float roll)
{
    return {roll, -std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)), std::atan2(d.y, d.x)};
}
}  // namespace

CMKEY3 KeyFromCmd3(const SCPTCMD3& c, LO* (*pfnFind)(int oid))
{
    CMKEY3 k;
    k.pos = {F(c, 0), F(c, 1), F(c, 2)};
    k.eul = {F(c, 3), F(c, 4), F(c, 5)};
    k.fov = F(c, 6);
    k.dtDelay = F(c, 7);
    k.dtMove = F(c, 8);
    k.grf = uint32_t(N(c, 9));
    if (pfnFind && N(c, 10) != -1) k.ploPos = pfnFind(N(c, 10));
    if (pfnFind && N(c, 11) != -1) k.ploLook = pfnFind(N(c, 11));
    k.posLook = {F(c, 12), F(c, 13), F(c, 14)};
    return k;
}

glm::vec3 EulerFromCm3(const CM3& cm) { return EulerFromRows(cm.mat[0], cm.mat[1]); }

// orig3: 0x22BB78 (the unset fields: a zero position / euler from the camera unless the key has a frame object, a zero
// fov the camera's +0x118) and 0x16BDE8 (the keys shifted: the previous is the policy's last key, or the key itself
// for a new policy (its flags +0x2C zero))
void StartCpkey3(CPKEY3& p, CMKEY3 key, const CMKEY3& nextIn, const CMKEY3& next2In, const CM3& cm, float t, bool fRealTime)
{
    CMKEY3 ak[3] = {key, nextIn, next2In};
    const float fovCm = 2.0f * std::atan(cm.tanY > 0 ? cm.tanY : std::tan(glm::radians(cm.fovDeg) * 0.5f));
    for (CMKEY3& k : ak)
    {
        if (!k.ploPos)
        {
            if (k.pos == glm::vec3(0.0f)) k.pos = cm.pos;
            if (k.eul == glm::vec3(0.0f)) k.eul = EulerFromCm3(cm);
        }
        if (k.fov == 0.0f) k.fov = fovCm;
    }
    const CMKEY3 prev = p.fActive ? p.akey[1] : CMKEY3{};
    p.akey[0] = prev;
    p.akey[1] = ak[0];
    p.akey[2] = ak[1];
    p.akey[3] = ak[2];
    if (p.akey[0].grf == 0) p.akey[0] = ak[0];
    p.tStart = t;
    p.fRealTime = fRealTime;
    p.fActive = true;
}

// orig3: 0x16B670
void UpdateCpkey3(const CPKEY3& p, CM3* pcm, float t)
{
    if (!p.fActive || !pcm) return;
    const CMKEY3& cur = p.akey[1];
    const CMKEY3& next = p.akey[2];
    const uint32_t grf = cur.grf;
    float u = 1.0f;
    if (!(grf & 0x10))
    {
        const float dt = t - p.tStart;
        if (dt < cur.dtDelay || cur.dtMove > 0.0f)
            u = cur.dtMove > 0.0f ? std::clamp((t - (p.tStart + cur.dtDelay)) / cur.dtMove, 0.0f, 1.0f) : 0.0f;
        if (grf & 0xE)
        {
            static const float s_aag[3][3] = {{-1, 2, 0}, {-1, -1, 1}, {-2, 3, 0}};  // 0x344850 / 0x344860 / 0x344870
            const float* ag = s_aag[(grf & 2) ? 0 : (grf & 4) ? 1 : 2];
            u = std::clamp(ag[0] * u * u * u + ag[1] * u * u + ag[2] * u, 0.0f, 1.0f);
        }
    }
    // the four keys' poses (a frame object: the position in its frame, its orientation; a look-at object: the tilt
    // and pan toward its point, the key's roll)
    glm::vec3 apos[4], aeul[4];
    for (int i = 0; i < 4; ++i)
    {
        const CMKEY3& k = p.akey[i];
        apos[i] = k.pos;
        aeul[i] = k.eul;
        if (k.ploPos)
        {
            const glm::mat4 m = MatWorld(k.ploPos);
            apos[i] = glm::vec3(m * glm::vec4(k.pos, 1.0f));
            aeul[i] = EulerFromRows(glm::vec3(m[0]), glm::vec3(m[1]));
        }
        if (k.ploLook)
        {
            const glm::vec3 posLook = glm::vec3(MatWorld(k.ploLook) * glm::vec4(k.posLook, 1.0f));
            aeul[i] = EulerLookAt(posLook - apos[i], aeul[i].x);
        }
    }
    for (int i = 1; i < 4; ++i)
        for (int j = 0; j < 3; ++j) aeul[i][j] = aeul[i - 1][j] + RadNormalize(aeul[i][j] - aeul[i - 1][j]);
    glm::vec3 pos, eul;
    float fov;
    if ((grf & 0x80) && (next.grf & 0x80) && !(grf & 0xF))
    {
        // the cubic Hermite segment from the key to the next (tangents: the mean of the neighbouring slopes per second)
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h01 = 3 * u2 - 2 * u3, h10 = u3 - 2 * u2 + u, h11 = u3 - u2;
        const float a10 = h10 * cur.dtMove, a11 = h11 * next.dtMove;
        float ar[4][7];
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 3; ++j) ar[i][j] = apos[i][j], ar[i][3 + j] = aeul[i][j];
            ar[i][6] = p.akey[i].fov;
        }
        const CMKEY3& prev = p.akey[0];
        float aout[7];
        for (int j = 0; j < 7; ++j)
        {
            float m0 = 0.0f, m1 = 0.0f;
            if (prev.dtMove > 0 && cur.dtMove > 0 && !(cur.grf & 0x100))
                m0 = 0.5f * ((ar[1][j] - ar[0][j]) / prev.dtMove + (ar[2][j] - ar[1][j]) / cur.dtMove);
            if (cur.dtMove > 0 && next.dtMove > 0 && !(next.grf & 0x100))
                m1 = 0.5f * ((ar[3][j] - ar[2][j]) / next.dtMove + (ar[2][j] - ar[1][j]) / cur.dtMove);
            aout[j] = ar[1][j] * h00 + ar[2][j] * h01 + m0 * a10 + m1 * a11;
        }
        pos = {aout[0], aout[1], aout[2]};
        eul = {aout[3], aout[4], aout[5]};
        fov = aout[6];
    }
    else
    {
        pos = apos[1] * (1 - u) + apos[2] * u;
        eul = aeul[1] * (1 - u) + aeul[2] * u;
        fov = cur.fov * (1 - u) + next.fov * u;
    }
    ApplyCmCutFromScpt3(pcm, pos, eul, fov);
}
