// Sly in the native Sly 3 game loop: see player3.h for what is the original's and what is interim.

#include "player3.h"

#include "anim3.h"
#include "cmfollow3.h"
#include "coll3.h"
#include "../actanim.h"
#include "../asega.h"
#include "../xf.h"
#include "game3.h"
#include "ids3.h"
#include "../gl.h"
#include "../glob.h"
#include "../joy.h"
#include "../lo.h"
#include "../scene.h"
#include "../sw.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clock.h"
#include "../xfmath.h"
extern CLOCK g_clock3;  // (game3.cpp)

namespace sly3
{
int Jts2FromJts3(int n)
{
    static const int s_a[] = {0, 12, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    return n >= 0 && n < int(sizeof(s_a) / sizeof(*s_a)) ? s_a[n] : (n == -1 ? -1 : -2);
}
int Jts3FromJts2(int n)
{
    for (int i = -1; i <= 11; ++i)
        if (Jts2FromJts3(i) == n) return i;
    return -2;
}

bool g_fPlayGame3 = false;
bool FPlayEnabled3()
{
    if (g_fPlayGame3) return true;
    const char* sz = std::getenv("SLY3_PLAY");
    return sz && *sz && *sz != '0';
}

namespace
{
// The reference at gameplay (tools/refroute3.sh, BASE = vblank 5900, Sly standing after the binocucom talk):
// Sly's root record (+0x48, +0x70) and the world camera's position and view direction (its rows +0x50..).
const glm::vec3 c_posStart(-6713.0708f, -14376.3467f, -2913.2537f);
const glm::vec3 c_posCamStart(-6923.442f, -14707.227f, -2774.471f);
constexpr float c_dzRootAboveFoot = 74.4605f;    // the root record above the step's foot point (+0x240), standing
// Interim kinematics, measured on the reference
constexpr float c_svRun = 510.0f;                // the run's speed at full deflection
constexpr float c_dvRunPerS = 56.93f * 60.0f;    // the run's ramp (|v| per frame x 60)
constexpr float c_dvStopPerS = 4000.0f;          // the stop's deceleration (63..75 per frame on the reference)
constexpr float c_svAir = 515.0f;                // the jump's horizontal speed at full deflection
constexpr float c_vzJump = 986.31f;              // 948.35 after the first frame's gravity
constexpr float c_vzJump2 = 675.57f;             // the double jump (637.61 after one frame)
constexpr float c_gGravity = 2277.6f;            // 37.96 per frame
constexpr float c_dzStepUp = 40.0f;
constexpr float c_muFloor = 1.5f;                // the floor contact's friction (OnStepXpNew: mu 1.5; mu x g = 56.94 / frame)
constexpr float c_radStart = 1.20430f;           // the reference's +0x1BC at the start (the facing as well)              // the floor probe's reach above the foot
constexpr float c_sCamBehind = 392.0f, c_dzCam = 138.8f, c_dzCamLook = 121.0f;

struct TRI
{
    glm::vec3 a, b, c;
};
// The floor probe (interim): the static render triangles facing up, in a grid of c_sCell cells
constexpr float c_sCell = 256.0f;
struct FLOOR
{
    std::unordered_map<int64_t, std::vector<TRI>> mpcell;
    static int64_t Key(int ix, int iy) { return (int64_t(ix) << 32) ^ uint32_t(iy); }
    void Add(const TRI& t)
    {
        const float x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
        const float y0 = std::min({t.a.y, t.b.y, t.c.y}), y1 = std::max({t.a.y, t.b.y, t.c.y});
        const int ix0 = int(std::floor(x0 / c_sCell)), ix1 = int(std::floor(x1 / c_sCell));
        const int iy0 = int(std::floor(y0 / c_sCell)), iy1 = int(std::floor(y1 / c_sCell));
        if ((ix1 - ix0 + 1) * (iy1 - iy0 + 1) > 256) return;  // (huge ones: sky / sea planes)
        for (int ix = ix0; ix <= ix1; ++ix)
            for (int iy = iy0; iy <= iy1; ++iy) mpcell[Key(ix, iy)].push_back(t);
    }
    // the highest floor under (x, y) at most zTop, -inf when none
    float ZFloor(float x, float y, float zTop) const
    {
        float zBest = -std::numeric_limits<float>::infinity();
        auto it = mpcell.find(Key(int(std::floor(x / c_sCell)), int(std::floor(y / c_sCell))));
        if (it == mpcell.end()) return zBest;
        for (const TRI& t : it->second)
        {
            const glm::vec2 p(x, y), a(t.a), b(t.b), c(t.c);
            const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::fabs(d) < 1e-6f) continue;
            const float l0 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
            const float l1 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
            const float l2 = 1.0f - l0 - l1;
            if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
            const float z = l0 * t.a.z + l1 * t.b.z + l2 * t.c.z;
            if (z <= zTop && z > zBest) zBest = z;
        }
        return zBest;
    }
};

struct PLAYER3
{
    LO* plo = nullptr;
    SCENEGLOBSET* psgs = nullptr;
    std::vector<glm::mat4> amatGlob0;
    std::vector<std::vector<glm::mat4>> aamatBone0;
    glm::vec3 posBounds0{0.0f};
    glm::mat4 matPose0Inv{1.0f};      // the inverse of the scene's placed pose (root position, heading)
    glm::mat4 matPlaced{1.0f};        // the scene's placement of Sly's records (anim3's correction: the instance matrix)
    bool fAnim = false;               // the animation runtime poses the entry from the records (anim3.h)
    GAIT gaitRun;                     // +0x1C90 (ASEGs +0x1CA4 307 / +0x1CE4 308)
    ASEG* pasegStand = nullptr;       // +0x1650 (14753)
    ASEGA* pasega = nullptr;          // the clip playing (the original's +0x?? ASEGA of SetSlyState's enter part)
    glm::vec3 pos{0.0f}, vel{0.0f};   // the root record
    float rad = 0.0f;                 // the facing: the object record's yaw (its matrix's x row; the pose)
    float wFacing = 0.0f;             // its yaw rate (the rotation record's velocity +0xB0, about z)
    float radHeading = 0.0f;          // +0x1BC the heading (SetStepPhysHeading 0x24E878)
    float uTurn = 1.0f;               // +0x190 the turn's gain (0x24E1A0)
    glm::vec3 vecVel{0.0f};           // +0x1A0 the velocity the feet push toward, in the heading's frame (negated)
    glm::vec3 normalFloor{0, 0, 1};   // the floor contact's normal
    int state = JTS3_None, jkind = -1, jstate = -1;
    float tState = 0.0f, tJstate = 0.0f;
    bool fJump2 = false;
    float dtCam = 0.0f;
    bool fCamPending = false;         // the camera's update (the world's cameras after the objects: UpdateCameras3)
    int jphase = 0, jhold = 0;        // the jump +0x28 / +0x13C (the camera's focus reads them)
    float zFoot = 0.0f;               // the step's foot point (+0x240): on the floor, kept in the air
    FLOOR floor;
    std::vector<JOYPRESS> apress;
    int frame = 0;
    FILE* ptrace = nullptr;
    bool fRegistered = false;
};
PLAYER3 s_pl;

void BuildFloor(PLAYER3& pl)
{
    pl.floor.mpcell.clear();
    size_t ctri = 0;
    for (SCENEGLOBSET& sgs : g_game3.scene.aglobset)
    {
        if (&sgs == pl.psgs || !sgs.pglobset || sgs.amatGlob.empty()) continue;
        const GLOBSET& gs = *sgs.pglobset;
        for (size_t i = 0; i < gs.aglob.size() && i < sgs.amatGlob.size(); ++i)
        {
            if (i < sgs.afVisible.size() && !sgs.afVisible[i]) continue;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty()) continue;  // (skinned: moving)
            const GLOB* pg = &gs.aglob[i];
            if (!pg->pgl && pg->iglobTemplate >= 0) pg = &gs.aglob[size_t(pg->iglobTemplate)];
            const glm::mat4& m = sgs.amatGlob[i];
            if (glm::length(glm::vec2(m[3]) - glm::vec2(c_posStart)) > 30000.0f) continue;
            for (size_t k = 0; k + 2 < pg->aindex.size(); k += 3)
            {
                const uint32_t i0 = pg->aindex[k], i1 = pg->aindex[k + 1], i2 = pg->aindex[k + 2];
                if (i0 >= pg->avtx.size() || i1 >= pg->avtx.size() || i2 >= pg->avtx.size()) continue;
                TRI t{glm::vec3(m * glm::vec4(pg->avtx[i0].pos, 1.0f)), glm::vec3(m * glm::vec4(pg->avtx[i1].pos, 1.0f)),
                      glm::vec3(m * glm::vec4(pg->avtx[i2].pos, 1.0f))};
                const glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
                const float l = glm::length(n);
                if (l <= 0.0f || std::fabs(n.z) / l < 0.4f) continue;
                pl.floor.Add(t);
                ++ctri;
            }
        }
    }
    std::printf("player3: the floor probe has %zu triangles in %zu cells\n", ctri, pl.floor.mpcell.size());
}

// The floor under (x, y) at most zTop in the world's collision (coll3.h: the SO entries' BSPs), -inf when none
float ZFloorColl(float x, float y, float zTop)
{
    HIT3 hit;
    return FFloor3(x, y, zTop, zTop - 2000.0f, 0.0f, &hit) ? hit.pos.z : -std::numeric_limits<float>::infinity();
}

// (debug: SLY3_COLLTEST=ref.csv: the collision's floor at the reference's foot points (+0x240), standing / running)
void TestCollAgainstRef(const char* szCsv)
{
    std::ifstream f(szCsv);
    std::string line;
    std::getline(f, line);
    int c = 0, cMiss = 0, cOff = 0;
    double sum = 0.0, dMax = 0.0;
    while (std::getline(f, line))
    {
        std::vector<std::string> a;
        std::stringstream ss(line);
        for (std::string t; std::getline(ss, t, ',');) a.push_back(t);
        if (a.size() < 20 || (a[2] != "0" && a[2] != "1")) continue;
        const float fx = std::stof(a[13]), fy = std::stof(a[14]), fz = std::stof(a[15]);
        const float z = ZFloorColl(fx, fy, fz + c_dzStepUp);
        ++c;
        if (!std::isfinite(z)) { ++cMiss; continue; }
        const double d = std::fabs(double(z) - fz);
        sum += d;
        dMax = std::max(dMax, d);
        if (d > 1.0) { if (cOff++ < 10) std::printf("colltest: f %s foot %.2f %.2f %.2f floor %.2f\n", a[0].c_str(), fx, fy, fz, z); }
    }
    std::printf("colltest: %d ground frames, %d without a floor, %d more than 1 off, mean %.3f max %.3f\n", c, cMiss, cOff,
                c > cMiss ? sum / (c - cMiss) : 0.0, dMax);
}

glm::mat4 MatPose(const glm::vec3& pos, float rad)
{
    return glm::rotate(glm::translate(glm::mat4(1.0f), pos), rad, glm::vec3(0, 0, 1));
}

void ApplyPose(PLAYER3& pl)
{
    if (!pl.psgs) return;
    ALO* palo = static_cast<ALO*>(pl.plo);
    if (pl.fAnim && palo && palo->axf.size() > 2)
    {
        // Sly's records (the reference's: the root +0x48 = record 1 (POS, kind 1) 75 above record 0 (the base), the
        // facing record 2 (ROT / OBJ, kind 6)); anim3 draws them through the scene's placement (the instance matrix)
        XF* pxfBase = palo->axf[0].get();
        XF* pxfRot = palo->axf[2].get();
        const glm::vec3 posBase = pl.pos - glm::vec3(palo->axf[1]->mat[3]);
        pxfBase->mat = glm::inverse(pl.matPlaced) * glm::translate(glm::mat4(1.0f), posBase);
        const glm::vec4 t = pxfRot->mat[3];
        pxfRot->mat = glm::rotate(glm::mat4(1.0f), pl.rad, glm::vec3(0, 0, 1));
        pxfRot->mat[3] = t;
        UpdateAloXfWorlds(palo);
        CApplyNativePose3(g_game3.scene, g_game3.sceneStats, g_game3.psw.get());
        return;
    }
    const glm::mat4 d = MatPose(pl.pos, pl.rad) * pl.matPose0Inv;
    for (size_t i = 0; i < pl.amatGlob0.size() && i < pl.psgs->amatGlob.size(); ++i) pl.psgs->amatGlob[i] = d * pl.amatGlob0[i];
    for (size_t i = 0; i < pl.aamatBone0.size() && i < pl.psgs->aamatBone.size(); ++i)
        for (size_t j = 0; j < pl.aamatBone0[i].size() && j < pl.psgs->aamatBone[i].size(); ++j)
            pl.psgs->aamatBone[i][j] = d * pl.aamatBone0[i][j];
    pl.psgs->posBounds = glm::vec3(d * glm::vec4(pl.posBounds0, 1.0f));
    // (the Sly 3 renderer) the light context's position: Sly's root (forward lighting, the cast shadow's direction)
    pl.psgs->fPosLightMoved = !pl.psgs->fPosLight || pl.psgs->posLight != pl.pos;
    pl.psgs->fPosLight = true;
    pl.psgs->posLight = pl.pos;
}

// The clip of a state (the enter part of SetSlyState 0x1ADDD0, the parts known: stand +0x1650, run the gait +0x1C90 as
// a blend (grfapl 9, Sly 2's 0x18C250 case 12); the jump's clips (+0x3E0..) are TODO(sly3): the current one stays)
void ApplyStateClip(PLAYER3& pl, int n)
{
    ALO* palo = static_cast<ALO*>(pl.plo);
    if (!pl.fAnim || !palo) return;
    if (n == JTS3_Stand && pl.pasegStand) ApplyAsegCur(pl.pasegStand, palo, 0.0f, 1.0f, 1, &pl.pasega);
    else if ((n == JTS3_Run || n == JTS3_Sneak) && pl.gaitRun.pbl)
    {
        ApplyAsegCur(pl.gaitRun.pbl, palo, 0.0f, 1.0f, 9, &pl.pasega);
        ResetGaitFeet(&pl.gaitRun);
    }
}

void SetState(PLAYER3& pl, int n)
{
    if (n == pl.state) return;
    const int nOld = pl.state;
    pl.state = n;
    pl.tState = g_clock3.t;
    if (nOld != JTS3_None) ApplyStateClip(pl, n);   // (the start keeps the clip the world start applied)
    if (n != JTS3_Jump) pl.jkind = pl.jstate = -1;
}

void SetJstate(PLAYER3& pl, int n)
{
    pl.jstate = n;
    pl.tJstate = g_clock3.t;
}

float RadNormalize3(float rad)   // orig3: 0x11E3B8 RadNormalize
{
    constexpr float c_2pi = 6.28318548f, c_pi = 3.14159274f;
    if (rad < -c_pi || c_pi < rad) rad -= c_2pi * std::floor((rad + c_pi) / c_2pi);
    return rad;
}

// The camera's horizontal forward (the stick's up) and the follow (interim)
glm::vec2 DirCamH()
{
    const glm::vec3 d = g_game3.pcmMain ? g_game3.pcmMain->mat[0] : glm::vec3(1, 0, 0);
    glm::vec2 h(d.x, d.y);
    const float l = glm::length(h);
    return l > 1e-4f ? h / l : glm::vec2(1, 0);
}

void SetCamLook(CM3* pcm, const glm::vec3& pos, const glm::vec3& posAt)
{
    pcm->pos = pos;
    const glm::vec3 dir = glm::normalize(posAt - pos);
    const glm::vec3 left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), dir));
    pcm->mat[0] = dir;
    pcm->mat[1] = left;
    pcm->mat[2] = glm::cross(dir, left);
}

// The world camera: the follow policy 0x274AC0 (cmfollow3.*), its record initialised as the reference's RAM has it at
// gameplay (vblank 5900: the policy 0x1EE6C30), the focus Sly (his fields as the focus's slot 85 reads them)
CPFOLLOW3 s_cpf;
void UpdateCamFollow(PLAYER3& pl, float dt);
CMSTATE3 s_cms;

ACTORCM3 ActorCm3(const PLAYER3& pl)
{
    ACTORCM3 a;
    a.pos = pl.pos;
    a.dirFacing = glm::vec3(std::cos(pl.rad), std::sin(pl.rad), 0.0f);
    a.vel = pl.vel;
    a.wAng = std::fabs(pl.wFacing);
    a.state = pl.state;
    a.fFloor = pl.state != JTS3_Jump;   // (+0x218: the step's floor contact)
    a.zFloor = pl.zFoot;
    a.jstate = pl.state == JTS3_Jump ? pl.jstate : -1;
    a.jumpPhase = pl.fJump2 ? 1 : 0;   // (the reference: 0 in the first rise, 1 after the double jump)
    a.jumpHold = pl.jhold;
    return a;
}

void InitCamFollow(PLAYER3& pl)
{
    s_cpf = CPFOLLOW3();
    s_cms = CMSTATE3();
    CPFOLLOW3& c = s_cpf;
    c.pcm = g_game3.pcmMain;
    c.pzone = PzoneBeach3();
    const float t = g_clock3.t;
    c.radPan = -2.11460f;
    c.radTilt = 0.197396f;
    c.sDist = 399.95f;
    c.dyScreen = -0.25f;
    c.dsDist = 0.0937270f;
    c.cpmode = 4;
    c.uLead = 1.0f;
    c.tManual = t - 16.5436f;   // (the reference: +0xC4 56.63, +0xE0 66.63 at T 73.17)
    c.tMoveLast = t - 6.5436f;
    c.tRec = t;
    c.tVisible = c.tClear = t;
    c.posRec = glm::vec3(pl.pos.x, pl.pos.y, pl.zFoot + 125.0f);
    c.dposAhead = glm::vec3(std::cos(pl.rad), std::sin(pl.rad), 0.0f) * 100.0f;
    c.posEye = glm::vec3(-6898.0737f, -14665.2715f, -2784.2771f);
    if (c.pcm)
    {
        c.pcm->tanY = std::tan(1.1f * 0.5f);
        s_cms.yScreenRange = c.pcm->tanY;
    }
    UpdateCamFollow(pl, 0.0f);
}

void UpdateCamFollow(PLAYER3& pl, float dt)
{
    CPFOLLOW3& c = s_cpf;
    c.pcm = g_game3.pcmMain;
    if (!c.pcm) return;
    CMCLOCK3 clk;
    clk.t = g_clock3.t;
    clk.dt = dt;
    clk.dtReal = dt > 0.0f ? g_clock3.dtReal : 0.0f;
    UpdateCpfollow3(&c, ActorCm3(pl), &s_cms, g_joyps2, clk);
    c.pcm->tanY = s_cms.yScreenRange;
}

// The stick's direction in the world (Sly +0x110C, set from the camera: the camera's pan less the stick's angle from
// its up)
float RadJoyWorld3(const JOYPS2& j)
{
    const glm::vec2 f = DirCamH();
    return RadNormalize3(std::atan2(f.y, f.x) - std::atan2(j.x, j.y));
}

// the extra columns as refroute3.sh's PEEK_EXTRA words (hex floats): +0x190 the turn's gain, +0x1A0 the lead,
// +0x1BC the heading, the world camera's position (+0x80) and view direction (+0x50)
std::string HexF(float g)
{
    uint32_t u;
    std::memcpy(&u, &g, 4);
    char sz[16];
    std::snprintf(sz, sizeof(sz), ",%08x", u);
    return sz;
}

void WriteTrace(PLAYER3& pl)
{
    if (!pl.ptrace) return;
    const JOYPS2& j = g_joyps2;
    const float radJoy = std::atan2(j.y, j.x);
    std::fprintf(pl.ptrace, "%d,%d,%d,%.9g,%d,%.9g,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d",
                 pl.frame, pl.frame + 5900, pl.state, pl.tState, -1, 0.0, -1, pl.pos.x, pl.pos.y, pl.pos.z, pl.vel.x,
                 pl.vel.y, pl.vel.z, pl.pos.x, pl.pos.y, pl.zFoot, j.uDeflect, radJoy, pl.jkind, pl.jstate);
    std::string ex = HexF(pl.uTurn) + HexF(pl.vecVel.x) + HexF(pl.vecVel.y) + HexF(pl.vecVel.z) + HexF(pl.radHeading);
    const CM3* pcm = g_game3.pcmMain;
    for (int i = 0; i < 3; ++i) ex += HexF(pcm ? pcm->pos[i] : 0.0f);
    for (int i = 0; i < 3; ++i) ex += HexF(pcm ? pcm->mat[0][i] : 0.0f);
    std::fprintf(pl.ptrace, "%s\n", ex.c_str());
}

// orig3: 0x1AC26C..0x1AC504 (the ground states 0 / 1 / 2 / 10 of 0x1AC098; the target / binocucom / circle branches
// are not reached on the beach and not ported): X taken within 0.1 s -> the jump (0x1B7268(+0x320, kind 1)); the
// deflection (padctl +0x8) above 0.1 from standing, above 0 moving -> 1 (R1 held, padctl +0x18 & 8: 2); else 0
void UpdateGroundTransitions(PLAYER3& pl, const JOYPS2& j)
{
    if (j.grfbtnPressed & 0x40)
    {
        SetState(pl, JTS3_Jump);
        pl.jkind = 1;
        SetJstate(pl, 0);
        pl.fJump2 = false;
        pl.vel.z = c_vzJump;
        return;
    }
    const float u = j.uDeflect;
    const bool fMove = pl.state == JTS3_Stand ? 0.1f < u : 0.0f < u;
    if (!fMove) { SetState(pl, JTS3_Stand); return; }
    SetState(pl, (j.grfbtn & 0x8) ? JTS3_Sneak : JTS3_Run);
}

// The run's speed by the deflection u (interim: the reference's measured points, deflect.route / beach.route, linear
// between them; the gaits' blend 0x258CF0 SetGaitMix decides it in the original)
float SvRunFromU(float u)
{
    static const float s_aug[][2] = {{0.2935f, 263.81f}, {0.3588f, 274.8f}, {0.7118f, 362.13f}, {1.0f, 510.0f}};
    constexpr int c = int(sizeof(s_aug) / sizeof(*s_aug));
    u = std::min(u, 1.0f);
    int i = 0;
    while (i < c - 2 && u > s_aug[i + 1][0]) ++i;
    const float g = (u - s_aug[i][0]) / (s_aug[i + 1][0] - s_aug[i][0]);
    return std::max(0.0f, s_aug[i][1] + g * (s_aug[i + 1][1] - s_aug[i][1]));
}

glm::vec2 VelTarget(const JOYPS2& j, float sv)
{
    const glm::vec2 f = DirCamH(), r(f.y, -f.x);
    glm::vec2 d = f * j.y + r * j.x;
    const float l = glm::length(d);
    if (l < 1e-4f) return glm::vec2(0.0f);
    return d / l * (sv / c_svRun * SvRunFromU(j.uDeflect));
}

void StepToward(glm::vec3& vel, const glm::vec2& vT, float dvUp, float dvDown)
{
    glm::vec2 v(vel);
    const glm::vec2 dv = vT - v;
    const float l = glm::length(dv);
    const float lim = glm::length(vT) >= glm::length(v) ? dvUp : dvDown;
    v = l <= lim ? vT : v + dv * (lim / l);
    vel.x = v.x;
    vel.y = v.y;
}

// orig3: 0x24E1A0 UpdateStep's turn gain (after UpdateAlo 0x23C660 / 0x24DFC8): with the motion mode +0x1B8 0, +0x190 =
// clamp(1.5 - |RadNormalize(+0x1BC - the facing)|, 0, 1) (the coefficients 0x327230 {1.5, -1, 0}, the limits
// 0x341FF0 {0, 1}; the facing: atan2 of the object matrix's x row, vt +0x120); modes 1 / 2: 1
void UpdateStepTurn3(PLAYER3& pl)
{
    float d = std::fabs(RadNormalize3(pl.radHeading - pl.rad));
    pl.uTurn = std::clamp(1.5f + d * (-1.0f + d * 0.0f), 0.0f, 1.0f);
}

// orig3: 0x23EDE8 (from the step's motion 0x24E2B8 with the heading's rows +0x1C0 and {+0x204 x +0x210, +0x208 x
// +0x210, +0x20C}: Sly {40, 0, 0.2372}): the rotation left to the heading frame (its angle about its axis, here z),
// GSmooth (0x11E598) of it from 0 gives the wanted angular speed; the rotation record's acceleration (+0xF0) gets the
// change toward it over dt, at most 1.25 x (a - b) / c; when GSmooth reaches the angle this frame the record's
// velocity is set to twice the angle over dt and the acceleration cancels it
float DwBrakeFacing3(PLAYER3& pl, float dt)
{
    static const SMP s_smp{40.0f, 0.0f, 0.2372f};
    const float ang = RadNormalize3(pl.radHeading - pl.rad);
    float dg = 0.0f;
    const float g = GSmooth(0.0f, ang, dt, &s_smp, &dg);
    const float accMax = (s_smp.svFast - s_smp.svSlow) / s_smp.dtFast * 1.25f;
    float acc = (dg - pl.wFacing) / dt;
    if (accMax < std::fabs(acc)) return acc * (accMax / std::fabs(acc));
    if (g == ang)
    {
        pl.wFacing = 2.0f * ang / dt;
        return -pl.wFacing / dt;
    }
    return acc;
}

// The floor contact of the copied step physics reduced to Sly against the static world (coll3): the solver's one
// kind-14 contact (OnStepXpNew 0x204250: mu 1.5; the normal row 0 .. 9000 x m per s), its end velocity the lead
// vector -(the heading's rows x +0x1A0) in the floor's plane times +0x190 (OnStepXpUpdate 0x2044B0), solved for one
// body: the normal impulse cancels the approach, the friction moves the tangential velocity toward the lead by at
// most mu x the normal impulse
void SolveFloor3(PLAYER3& pl, float dt)
{
    const glm::vec3 n = pl.normalFloor;
    const float vn = glm::dot(pl.vel, n);
    const float jn = std::clamp(-vn, 0.0f, 9000.0f * dt);
    const glm::vec2 h(std::cos(pl.radHeading), std::sin(pl.radHeading)), hl(-h.y, h.x);
    glm::vec3 w = -(glm::vec3(h, 0.0f) * pl.vecVel.x + glm::vec3(hl, 0.0f) * pl.vecVel.y);
    w -= n * glm::dot(w, n);
    w *= pl.uTurn;
    const glm::vec3 vt = pl.vel - n * vn;
    glm::vec3 jt = w - vt;
    const float l = glm::length(jt), lMax = c_muFloor * jn;
    if (lMax < l) jt *= lMax / l;
    const glm::vec3 dv = n * jn + jt;
    pl.vel += dv;
    // (the original's half frame of the change on the position, 0x1F9E70, pairs with its position pass along the
    // floor's normal; with the foot kept on the floor vertically here it would creep up the slope: left out)
}

void UpdatePlayer3(float dt)
{
    PLAYER3& pl = s_pl;
    if (!pl.psgs || dt <= 0.0f) return;
    const JOYPS2& j = g_joyps2;   // (read by the game loop's input step, game3.cpp UpdateInput3)
    UpdateStepTurn3(pl);
    // the controller's input (0x1AD1D0): the transitions (0x1AC098), then the state's input
    if (pl.state != JTS3_Jump) UpdateGroundTransitions(pl, j);
    else if ((j.grfbtnPressed & 0x40) && !pl.fJump2 && pl.jstate != 0)
    {
        pl.fJump2 = true;  // (the double jump: the jump's state 2)
        SetJstate(pl, 2);
        pl.vel.z = c_vzJump2;
    }
    const float radJoy = RadJoyWorld3(j);
    if (std::getenv("SLY3_JOYDBG")) std::printf("joydbg f %d j %.3f %.3f u %.3f dir %.3f %.3f rad %.3f\n", pl.frame, j.x, j.y, j.uDeflect, DirCamH().x, DirCamH().y, radJoy);
    if (pl.state == JTS3_Run || pl.state == JTS3_Sneak)
    {
        // 0x1AD344: +0x1A0 = SetGaitMix(the gait +0x1C90, the deflection) (INTERIM: the gait's velocity is the measured
        // speed by deflection until the ASEG runtime gives the gaits' root velocities), the heading +0x110C (0x24E878)
        if (pl.gaitRun.pbl)
            pl.vecVel = glm::vec3(SetGaitMix(&pl.gaitRun, j.uDeflect));   // 0x258CF0 (the ASEGs' root velocities)
        else
        {
            const float sv = (pl.state == JTS3_Sneak ? 0.5f : 1.0f) * SvRunFromU(j.uDeflect);  // (SLY3_NOANIM: measured)
            pl.vecVel = glm::vec3(-sv, 0.0f, 0.0f);
        }
        pl.radHeading = radJoy;
    }
    else if (pl.state == JTS3_Stand) pl.vecVel = glm::vec3(0.0f);
    else if (pl.state == JTS3_Jump)
    {
        pl.vecVel = glm::vec3(j.uDeflect > 0.0f ? -c_svAir : 0.0f, 0.0f, 0.0f);   // (the reference's +0x1A0 in the air)
        if (j.uDeflect > 0.0f) pl.radHeading = radJoy;
    }
    // the physics (0x1F8DD8 for Sly): slot 74 0x1AA630 -> 0x24E2B8 (gravity 0x243738, the facing braked toward the
    // heading 0x23EDE8), the records integrated (trapezoid, xf.cpp UPD::Moving / RotVel), the floor contact solved
    const float accYaw = DwBrakeFacing3(pl, dt);
    const float wNew = pl.wFacing + accYaw * dt;
    pl.rad = RadNormalize3(pl.rad + (pl.wFacing + wNew) * 0.5f * dt);
    pl.wFacing = wNew;
    if (pl.state == JTS3_Jump)
    {
        // (INTERIM: the jump's motion 0x1B88E0 is not ported; the air speed follows the lead at the measured rate)
        const glm::vec2 h(std::cos(pl.radHeading), std::sin(pl.radHeading));
        StepToward(pl.vel, h * -pl.vecVel.x * pl.uTurn, c_muFloor * c_gGravity * dt, c_muFloor * c_gGravity * dt);
        const glm::vec3 v0 = pl.vel;
        pl.vel.z -= c_gGravity * dt;
        pl.pos += (v0 + pl.vel) * (0.5f * dt);
        // (the jump's animation states as the reference shows them: 0 / 2 for 9 frames, then 1 / 3; 1 -> 6 the frame
        // after the rise ends)
        if ((pl.jstate == 0 || pl.jstate == 2) && g_clock3.t - pl.tJstate >= 8.5f / 60.0f) SetJstate(pl, pl.jstate == 0 ? 1 : 3);
        else if (pl.jstate == 1 && v0.z < 0.0f) SetJstate(pl, 6);
        HIT3 hit;
        const float zFoot = pl.pos.z - c_dzRootAboveFoot;
        if (pl.vel.z < 0.0f && FFloor3(pl.pos.x, pl.pos.y, zFoot + c_dzStepUp, zFoot - 1.0f, 0.7f, &hit))
        {
            pl.zFoot = hit.pos.z;
            pl.normalFloor = hit.normal;
            pl.pos.z = hit.pos.z + c_dzRootAboveFoot;
            pl.vel.z = 0.0f;
            SetState(pl, j.uDeflect > 0.0f ? JTS3_Run : JTS3_Stand);
        }
    }
    else
    {
        const glm::vec3 v0 = pl.vel;
        pl.vel.z -= c_gGravity * dt;
        pl.pos += (v0 + pl.vel) * (0.5f * dt);
        HIT3 hit;
        if (FFloor3(pl.pos.x, pl.pos.y, pl.zFoot + c_dzStepUp, pl.zFoot - 200.0f, 0.7f, &hit))
        {
            pl.normalFloor = hit.normal;
            SolveFloor3(pl, dt);
            // the position pass (0x1F9410 flag 4): the foot on the floor
            if (FFloor3(pl.pos.x, pl.pos.y, pl.zFoot + c_dzStepUp, pl.zFoot - 200.0f, 0.7f, &hit)) pl.zFoot = hit.pos.z;
            pl.pos.z = pl.zFoot + c_dzRootAboveFoot;
        }
    }
    ApplyPose(pl);
    pl.dtCam = dt;
    pl.fCamPending = true;
}

// A refroute3 route file: "PRESS@REL[+N]" per line ('#' comments), as the reference's --press, frames from the start
bool FReadRoute(const char* szFile, std::vector<JOYPRESS>& apress)
{
    std::ifstream f(szFile);
    if (!f) return false;
    std::string line, all;
    while (std::getline(f, line))
    {
        if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), line.end());
        if (line.empty()) continue;
        if (!all.empty()) all += ',';
        all += line;
    }
    return FParseJoyPresses(all, apress);
}
}  // namespace

bool FUpdatePlayerCamera3(bool fKeyActive)
{
    PLAYER3& pl = s_pl;
    if (!pl.psgs || !pl.fCamPending) return false;
    pl.fCamPending = false;
    // (a script's camera key is a policy above the follow; SLY3_PLAY starts Sly without the talk, so its scripts'
    // keys are ignored there)
    const bool fFollow = !fKeyActive || std::getenv("SLY3_PLAY");
    if (fFollow) UpdateCamFollow(pl, pl.dtCam);
    ++pl.frame;
    WriteTrace(pl);
    return fFollow;
}

void StartPlayer3(SW* psw)
{
    if (!FPlayEnabled3() || !psw) return;
    PLAYER3& pl = s_pl;
    if (pl.ptrace) std::fclose(pl.ptrace);
    const bool fRegistered = pl.fRegistered;
    pl = PLAYER3();
    pl.fRegistered = fRegistered;
    for (LO* plo : psw->mpcidaplo[Icid(CID3::JT)])
        if (plo->oid == 24) pl.plo = plo;
    for (SCENEGLOBSET& sgs : g_game3.scene.aglobset)
        if (sgs.oid == 24 && sgs.pglobset && !sgs.amatGlob.empty()) { pl.psgs = &sgs; break; }
    if (!pl.psgs) { std::printf("player3: Sly (oid 24) has no scene entry\n"); return; }
    pl.amatGlob0 = pl.psgs->amatGlob;
    pl.aamatBone0 = pl.psgs->aamatBone;
    pl.posBounds0 = pl.psgs->posBounds;
    // The scene places Sly at his instance matrix (the proxy's: parked at z 20000 in m_ext, where the reference's
    // player code moves him from at the start), so his placed pose is that matrix
    glm::mat4 matPlaced(1.0f);
    if (pl.plo)
        if (auto it = psw->mpmatPlayerInstance.find(pl.plo); it != psw->mpmatPlayerInstance.end()) matPlaced = it->second;
    std::printf("player3: Sly placed at %.2f %.2f %.2f (x axis %.3f %.3f %.3f), %zu globs\n", matPlaced[3][0], matPlaced[3][1],
                matPlaced[3][2], matPlaced[0][0], matPlaced[0][1], matPlaced[0][2], pl.amatGlob0.size());
    if (const char* sz = std::getenv("SLY3_PLAY_RAD0"))  // (debug: the model's forward in its placed frame)
        matPlaced = glm::rotate(matPlaced, std::strtof(sz, nullptr), glm::vec3(0, 0, 1));
    pl.matPose0Inv = glm::inverse(matPlaced);
    pl.matPlaced = matPlaced;
    pl.fAnim = !std::getenv("SLY3_NOANIM") && pl.plo && static_cast<ALO*>(pl.plo)->axf.size() > 2;
    if (pl.fAnim)
    {
        ALO* palo = static_cast<ALO*>(pl.plo);
        InitGait(&pl.gaitRun, palo, 0x6F);
        pl.gaitRun.apaseg[0] = dynamic_cast<ASEG*>(PloActorAnimField3(palo, 0x1CA4));
        pl.gaitRun.apaseg[1] = dynamic_cast<ASEG*>(PloActorAnimField3(palo, 0x1CE4));
        BindGait(&pl.gaitRun);
        pl.pasegStand = dynamic_cast<ASEG*>(PloActorAnimField3(palo, 0x1650));
        if (std::getenv("SLY3_GAITPRINT"))
            for (ASEG* pa : pl.gaitRun.apaseg)
                if (pa)
                    for (int l = 0; l < 3; ++l)
                        for (const ASEGEV& ev : PasegdOf(pa)->aaev[l])
                            std::printf("gait aseg %d list %d t %.4f kind %d n0 %d (tMax %.4f)\n", pa->oid, l, ev.t, ev.kind,
                                        ev.an.empty() ? -1 : ev.an[0], PasegdOf(pa)->tMax);
        if (pl.pasegStand) pl.pasega = PasegaFindAseg(pl.pasegStand, palo);
        std::printf("player3: the run's gait %s (ASEGs %d / %d, velocities %.1f %.1f %.1f / %.1f %.1f %.1f), the stand %d %s\n",
                    pl.gaitRun.pbl ? "bound" : "NOT bound", pl.gaitRun.apaseg[0] ? pl.gaitRun.apaseg[0]->oid : -1,
                    pl.gaitRun.apaseg[1] ? pl.gaitRun.apaseg[1]->oid : -1, pl.gaitRun.avel[0].x, pl.gaitRun.avel[0].y,
                    pl.gaitRun.avel[0].z, pl.gaitRun.avel[1].x, pl.gaitRun.avel[1].y, pl.gaitRun.avel[1].z,
                    pl.pasegStand ? pl.pasegStand->oid : -1, pl.pasega ? "playing" : "not playing");
    }
    BuildColl3(24, -1);  // (Sly's own entries left out)
    if (std::getenv("SLY3_XFPRINT") && pl.plo)
    {
        ALO* pa = static_cast<ALO*>(pl.plo);
        for (std::size_t i = 0; i < pa->axf.size(); ++i)
        {
            const XF* x = pa->axf[i].get();
            int iref = -1;
            for (std::size_t k = 0; k < pa->axf.size(); ++k) if (pa->axf[k].get() == x->pxfRef) iref = int(k);
            std::printf("xf %zu ref %d kind %d tag %d mode %d pos %.1f %.1f %.1f world %.1f %.1f %.1f%s%s%s\n", i, iref, x->kind, x->tag, x->mode,
                        x->mat[3].x, x->mat[3].y, x->mat[3].z, x->matWorld[3].x, x->matWorld[3].y, x->matWorld[3].z,
                        x == pa->apxfRef[0] ? " POS" : "", x == pa->apxfRef[1] ? " ROT" : "", x == pa->apxfRef[2] ? " OBJ" : "");
        }
    }
    if (const char* sz = std::getenv("SLY3_COLLTEST")) TestCollAgainstRef(sz);
    // the reference's gameplay start: the root, its foot on the floor, the camera behind
    pl.pos = c_posStart;
    pl.zFoot = c_posStart.z - c_dzRootAboveFoot;
    const glm::vec2 dir = glm::normalize(glm::vec2(c_posStart) - glm::vec2(c_posCamStart));
    (void)dir;
    pl.rad = pl.radHeading = c_radStart;
    SetState(pl, JTS3_Stand);
    InitCamFollow(pl);
    if (const char* sz = std::getenv("SLY3_ROUTE"))
    {
        if (!FReadRoute(sz, pl.apress)) std::printf("player3: cannot read the route %s\n", sz);
        SetInputRoute3(pl.apress);
    }
    if (const char* sz = std::getenv("SLY3_PLAYERTRACE"))
        if ((pl.ptrace = std::fopen(sz, "w")))
            std::fprintf(pl.ptrace, "f,vb,state,tState,substate,tSubstate,mode,x,y,z,vx,vy,vz,fx,fy,fz,u,rad,jkind,jstate\n");
    ApplyPose(pl);
    g_fScptRunner3 = true;  // (the camera is the player's now, not the attract key's)
    if (!pl.fRegistered)
    {
        RegisterFrameHook3(UpdatePlayer3);
        pl.fRegistered = true;
    }
    std::printf("player3: Sly in control at %.2f %.2f %.2f (SLY3_PLAY)\n", pl.pos.x, pl.pos.y, pl.pos.z);
}
}  // namespace sly3
