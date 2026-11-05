// aichase.cpp: Carmelita's chase (aichase.h). Ported from the matched decomp (src/hand 0x14A008 / 0x14A0A8 /
// 0x14A1B0 / 0x14A200 / 0x14A2A0 / 0x14A478 / 0x14A4D0 / 0x14AA30 / 0x14AD70 / 0x14AE38 / 0x14B468, src/auto
// 0x14A178 / 0x14A248 / 0x14AE18).
#include "aichase.h"

#include "activate.h"
#include "actor.h"
#include "aijobs.h"
#include "aipursuit.h"
#include "breakable.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "guard.h"
#include "jump.h"
#include "lo.h"
#include "lox.h"
#include "objmethod.h"
#include "po.h"
#include "so.h"
#include "sound.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "worldaccess.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

LO* PloFindSwNearest(LO* ploCtx, OID oid, bool fSameChannel = false);  // animfind.h
float GRandInRange(float gLow, float gHigh);  // xfmath.h (0x11D448)
float RadBetweenVectors(const glm::vec3& vecStart, const glm::vec3& vecFinish, glm::vec3* pnormal);  // mat.h (0x11E4B0)
namespace cmm
{
glm::vec3 SetVectorCylind(float rad, float sXY, float sZ);      // vecmat.h (0x11E1A0)
glm::vec3 SetVectorSphere(float radPan, float radTilt, float s);  // vecmat.h (0x11E208)
}

namespace
{
// 0x14A4D0's constants (D_002BD460..D_002BD47C)
constexpr float S_MOVING = 100.0f;            // D_002BD460: the player moves faster than this
constexpr float T_STILL_LO = 4.0f, T_STILL_HI = 6.0f;  // D_002BD468: still this long: the player himself aimed at
constexpr float S_AIM_OFFSET = 200.0f;        // D_002BD470
constexpr float DZ_AIM = -75.0f;              // D_002BD474
constexpr float S_BREAKABLE = 600.0f;         // D_002BD478: breakables this near the player
constexpr float DEG_BREAKABLE = 10.0f;        // D_002BD47C
// 0x14AA30's ranges (D_002BD480..D_002BD4A0)
constexpr float RG_BEHIND_UP[2] = {900.0f, 1400.0f}, RG_BEHIND_DOWN[2] = {1400.0f, 4000.0f};
constexpr float RG_DEPTH_UP[2] = {600.0f, 1000.0f}, RG_DEPTH_DOWN[2] = {1000.0f, 4000.0f};
constexpr float RG_SCREEN[2] = {0.0f, 10.0f};

bool FInRg(const float* r, float g) { return r[0] <= g && g <= r[1]; }  // 0x11D910
float GRemapRg(const float* r, float g) { return r[0] == r[1] ? 0.0f : (g - r[0]) / (r[1] - r[0]); }
float LenXy(const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

const bool s_fDebug = std::getenv("SLY2N_CHASEDEBUG") != nullptr;
}  // namespace

LO* PloNewChasegraph() { return new CHASEGRAPH(); }

// orig: 0x0014A0A8: the entries' objects found near the graph, the curve through their positions
void PostLoadChasegraph(LO* plo)
{
    auto* p = dynamic_cast<CHASEGRAPH*>(plo);
    if (!p) return;
    // (0x1A5C48, the base post-load: update.cpp's pass)
    const int c = static_cast<int>(p->aentry.size());
    CRV& crv = p->crv;
    crv = CRV{};
    crv.crvk = 0;
    crv.fClosed = 0;
    crv.ag.resize(c);
    crv.apos.assign(c, glm::vec3(0.0f));
    for (int i = 0; i < c; ++i)
    {
        crv.ag[i] = static_cast<float>(i);
        CHASEGRAPH::ENTRY& e = p->aentry[i];
        e.plo = PloFindSwNearest(p, e.oid);  // 0x15E938(G, 4, oid, p)
        if (e.plo) crv.apos[i] = PosAi(e.plo);
    }
    if (c > 0) RecalcCrv(&crv, glm::mat4(1.0f));  // 0x1E6180
    p->nDir = static_cast<int>(plo->U32Field(0, 0x50, 0));
    if (s_fDebug)
    {
        std::fprintf(stderr, "chase graph %d dir %d:", plo->oid, p->nDir);
        for (const auto& e : p->aentry) std::fprintf(stderr, " [%d %d %s]", e.k, e.oid, e.plo ? "ok" : "-");
        std::fprintf(stderr, "\n");
    }
}

// ---- 0xB6: the chase ----

// orig: 0x0014A1B0
void JOBCHASE::Init()
{
    AIJOB::Init();
    ++g_sound.fModeAlert;  // 0x1FEE90(D_003DD040)
    AddGuardState(pguard, 0x24);
    st = -1;
    iTarget = -1;
    i = -1;
}

// orig: 0x0014A200
void JOBCHASE::Enter()
{
    AIJOB::Enter();
    SetState(0);
    tMoved = g_clock.t;
    pguard->f4A4 = 0;
}

// orig: 0x0014A248
void JOBCHASE::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    RemoveGuardState(pguard, 0x24);
    --g_sound.fModeAlert;  // 0x1FEEA0
    pguard->f4A4 = 1;
}

// orig: 0x0014AE18
LO* JOBCHASE::PloEntry() const
{
    if (!pgraph || i < 0 || i >= static_cast<int>(pgraph->aentry.size())) return nullptr;
    return pgraph->aentry[i].plo;
}

// orig: 0x0014A478: no floor contact and nothing within 100 above .. 250 below
bool JOBCHASE::FAirborne()
{
    if (St(pguard).pxpFloor) return false;
    glm::vec3 pos = PosAi(pguard->palo);
    return !FDropOnGround(&pos, 100.0f, -250.0f, true);
}

// orig: 0x0014A4D0: where she aims (her +0x1010 point, or the player himself): a breakable near the player when it is
// nearer to her than he is or within 10 degrees of him; else beside a slow player / behind a running one
void JOBCHASE::Aim()
{
    GUARD* pg = pguard;
    LO* ploPlayer = PloPoCurrent(g_psw);
    if (!ploPlayer || !FIsLoDerivedFrom(ploPlayer, 0x30)) return;
    ALO* paloPlayer = static_cast<ALO*>(ploPlayer);
    const glm::vec3 posPlayer = PosAi(ploPlayer);
    const glm::vec3 posMe = PosAi(pg->palo);
    const glm::vec3 d = posPlayer - posMe;
    const glm::vec3 v = VelAloPos(paloPlayer);  // vt slot 33
    if (S_MOVING < glm::length(v)) tMoved = g_clock.t;
    const float dt = g_clock.t - tMoved;
    glm::vec3 pos;
    bool fPos = false;
    if (GRandInRange(T_STILL_LO, T_STILL_HI) < dt && !(0 < g_gamepause.c78))  // (0x20F0B0)
    {
        ACTOR* pa = FIsLoDerivedFrom(ploPlayer, 0x34) ? PactorFromLo(ploPlayer) : nullptr;
        if (pa && pa->jump.state == 0x11)
        {
            // hanging (the jump's state 17): the highest point of his custom entry
            pos = PosAi(ploPlayer);
            if (STEPPHYS* ps = PstepPhysOf(paloPlayer))
                if (ps->isoeCustom >= 0 && size_t(ps->isoeCustom) < paloPlayer->asoe.size())
                    for (const glm::vec3& q : paloPlayer->asoe[ps->isoeCustom].geomWorld.apos)
                        if (pos.z < q.z) pos = q;
            fPos = true;
        }
        else
        {
            SetGuardPursued(pg, ploPlayer);  // 0x208540
            return;
        }
    }
    if (!fPos)
    {
        for (LO* plo : AploOfClass(g_psw, 0x62))
        {
            if (!plo->fInWorld || !FBreakableIntact(plo)) continue;  // (the list 0x3E1450, 0x1BBE48)
            ALO* palo = static_cast<ALO*>(plo);
            glm::vec3 posT = PosAi(plo);
            float r = S_BREAKABLE;
            if (OXA* poxa = Poxa(palo))
            {
                posT = poxa->posCenter;
                r = poxa->sRadius;
            }
            // TODO(ai): 0x15E9C8(G, 1, 0x12, the breakable, 8): its class 0x12 parts with +0xE8 & 0x17E, one at random
            // (NRandInRange) as the point (radius its +0xEC)
            const glm::vec3 e0 = posT - posPlayer;
            if (S_BREAKABLE * S_BREAKABLE < glm::dot(e0, e0)) continue;
            const float pan = GRandInRange(-3.1415927f, 3.1415927f);
            const float tilt = GRandInRange(-3.1415927f, 3.1415927f);
            posT = posT + cmm::SetVectorSphere(pan, tilt, GRandInRange(0.0f, r));  // 0x11E208
            const glm::vec3 e = posT - posMe;
            if (LenXy(e) < LenXy(d))
            {
                pos = posT;
                fPos = true;
                break;
            }
            const float g = RadBetweenVectors(d, e, nullptr);  // 0x11E4B0
            if (!(std::fabs(g) * 57.295776f <= DEG_BREAKABLE))
            {
                pos = posT;
                fPos = true;
                break;
            }
        }
    }
    if (!fPos)
    {
        if (LenXy(v) < S_MOVING)
        {
            const glm::vec3 e = posMe - posPlayer;
            const float rad = std::atan2(e.y, e.x);
            pos = PosAi(ploPlayer) + cmm::SetVectorCylind(GRandInRange(rad - 1.5707964f, rad + 1.5707964f), S_AIM_OFFSET, DZ_AIM);
        }
        else
            pos = posPlayer + glm::normalize(v) * -S_AIM_OFFSET;
    }
    if (pg->plo1010)
    {
        SetLoxPos(static_cast<LOX*>(pg->plo1010), pos);  // vt slot 29
        SetGuardPursued(pg, pg->plo1010);
    }
}

// orig: 0x0014AA30: the entry to be at: about 900..4000 along the graph's curve from the player, 600..4000 deep in the
// camera's view and near the middle of the screen
int JOBCHASE::IBest()
{
    int iBest = -1;
    LO* ploPlayer = PloPoCurrent(g_psw);
    if (!pgraph || !ploPlayer) return -1;
    float gBest = -1.0f;
    float sPlayer = 0.0f;
    if (!pgraph->crv.apos.empty()) NearestCrvAll(&pgraph->crv, PosAi(ploPlayer), nullptr, nullptr, nullptr, &sPlayer);
    for (int k = 0; k < static_cast<int>(pgraph->aentry.size()); ++k)
    {
        if (!pgraph->aentry[k].plo) continue;
        float g = sPlayer - pgraph->crv.as[k];
        if (pgraph->nDir == 1) g = -g;
        float u1 = 0.0f, u2 = 0.0f, u3 = 0.0f;
        if (FInRg(RG_BEHIND_UP, g)) u1 = GRemapRg(RG_BEHIND_UP, g);
        else if (FInRg(RG_BEHIND_DOWN, g)) u1 = 1.0f - GRemapRg(RG_BEHIND_DOWN, g);
        const glm::vec3 vs = ConvertCmWorldToScreen(&g_cm, pgraph->crv.apos[k]);  // 0x14F308
        if (FInRg(RG_DEPTH_UP, vs.z)) u2 = GRemapRg(RG_DEPTH_UP, vs.z);
        else if (FInRg(RG_DEPTH_DOWN, vs.z)) u2 = 1.0f - GRemapRg(RG_DEPTH_DOWN, vs.z);
        const float gScreen = std::sqrt(vs.x * vs.x + vs.y * vs.y);
        if (FInRg(RG_SCREEN, gScreen)) u3 = 1.0f - GRemapRg(RG_SCREEN, gScreen);
        const float s = u1 * 0.5f + u2 * 0.15f + u3 * 0.35f;
        if (gBest < s)
        {
            gBest = s;
            iBest = k;
        }
    }
    return iBest;
}

// orig: 0x0014AD70: one entry toward the best one; run or hop by the entry's kind
void JOBCHASE::Step()
{
    if (!pgraph) return;
    const int c = static_cast<int>(pgraph->aentry.size());
    int k;
    if (i < iTarget)
    {
        ++i;
        k = i >= 0 && i < c ? pgraph->aentry[i].k : -1;
    }
    else
    {
        k = i >= 0 && i < c ? pgraph->aentry[i].k : -1;
        --i;
    }
    if (s_fDebug)
        std::fprintf(stderr, "chase: t %.3f step to %d (target %d) kind %d\n", g_clock.t, i, iTarget, k);
    if (k == 0) SetState(1);
    else if (k == 1) SetState(2);
}

// orig: 0x0014A2A0
void JOBCHASE::Update()
{
    AIJOB::Update();
    Aim();
    GUARD* pg = pguard;
    for (;;)
    {
        if (FJobDone(this)) break;
        const int s = st;
        const bool fStill = pg->fFC8 || FGamePausedForAi();  // 0x209098
        switch (s)
        {
        case 0:
            if (fStill) SetState(4);
            else if (FAirborne()) SetState(2);
            else
            {
                iTarget = IBest();
                if (iTarget == i)
                {
                    paction = PactionSelect(pg, PloPoCurrent(g_psw), nullptr);  // 0x1758A0(guard, player, 0)
                    if (paction) SetState(3);
                }
                else Step();
            }
            break;
        case 1:
            if (fStill) SetState(4);
            else if (FAirborne()) SetState(2);
            else if (pchild && static_cast<JOBFOLLOW*>(pchild)->FArrived()) SetState(0);  // 0x178D00
            else PactionSelect(pg, PloPoCurrent(g_psw), nullptr);  // (its result unused)
            break;
        case 2:
            if (fStill) SetState(4);
            else if (pchild && static_cast<JOBMOVE*>(pchild)->st == 1) SetState(0);  // 0x174FB8
            break;
        case 3:
            if (fStill) SetState(4);
            else if (pchild && static_cast<JOBACT*>(pchild)->st == -1) SetState(0);  // 0x175748
            break;
        case 4:
            if (!fStill) SetState(0);
            break;
        }
        if (s == st) break;
    }
}

// orig: 0x0014AE38
void JOBCHASE::SetState(int n)
{
    if (n == st) return;
    GUARD* pg = pguard;
    switch (st)
    {
    case 0:
        SetGuardActionAim(pg, nullptr, nullptr, glm::vec3(0.0f)); // orig: 0x20AC28
        break;
    case -1:
    case 1:
        SetAloCnstrLin(pg->palo, 1);  // 0x1F5720
        SetAloCnstrAng(pg->palo, 1);  // 0x1F57C0
        break;
    case 3:
        pg->tF04 = g_clock.t + (paction ? GRandInRange(paction->r60.x, paction->r60.y) : 0.0f);
        paction = nullptr;
        break;
    }
    EndChildJob(&pchild);
    if (s_fDebug)
    {
        const glm::vec3 p = PosAi(pg->palo);
        std::fprintf(stderr, "chase: t %.3f guard %d state %d -> %d at (%.0f %.0f %.0f) entry %d\n", g_clock.t, pg->palo->oid, st, n,
                     p.x, p.y, p.z, i);
    }
    st = n;
    tSt = g_clock.t;
    switch (n)
    {
    case 0:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->ploFace = PloPoCurrent(g_psw);
        o->Enter();
        SetAloVelocityTo(pg->palo, glm::vec3(0.0f));     // vt slot 35
        SetAloAngVelocityTo(pg->palo, glm::vec3(0.0f));  // vt slot 36
        St(pg).vecVel = glm::vec3(0.0f);                 // +0x190
        break;
    }
    case 1:
    {
        CRV* pcrv = &pg->crv294;
        pcrv->crvk = 0;
        pcrv->fClosed = 0;
        LO* ploE = PloEntry();
        pcrv->apos = {PosAi(pg->palo), ploE ? PosAi(ploE) : PosAi(pg->palo)};
        pcrv->ag = {0.0f, 1.0f};
        RecalcCrv(pcrv, glm::mat4(1.0f));  // 0x1E6180
        auto* o = static_cast<JOBFOLLOW*>(PjobPush(pg, 0xA7));
        o->Follow(pcrv, 0);
        o->kMove = 8;
        o->fNoAvoid = 1;
        o->sArrive = 25.0f;
        o->ploArg = PloPoCurrent(g_psw);
        o->Enter();
        pg->tF04 = g_clock.t;
        SetAloCnstrLin(pg->palo, 0);
        SetAloCnstrAng(pg->palo, 0);
        break;
    }
    case 2:
    {
        auto* o = static_cast<JOBUSE*>(PjobPush(pg, 0xAC));
        LO* ploE = PloEntry();
        o->pos = ploE ? PosAi(ploE) : PosAi(pg->palo);
        o->nExtra = 1;
        o->Enter();
        break;
    }
    case 3:
    {
        auto* o = static_cast<JOBACT*>(PjobPush(pg, 0xA4));
        o->ptarget = pg->plo38C;
        o->paction = paction;
        o->Enter();
        pg->tF04 = FLT_MAX;
        break;
    }
    case 4:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->ploFace = PloPoCurrent(g_psw);
        o->nAnim = 1;
        o->Enter();
        break;
    }
    case -1:
        SetAloCnstrLin(pg->palo, 0);
        SetAloCnstrAng(pg->palo, 0);
        break;
    }
}

namespace
{
struct INITCHASE
{
    INITCHASE()
    {
        // the graph's option method (0x10FB48 -> 0x14A178): an entry {kind, oid} appended (at most 0x100)
        RegisterObjectMethod(0x10FB48, [](LO* plo, const std::vector<CRef>& a) {
            if (auto* p = dynamic_cast<CHASEGRAPH*>(plo); p && p->aentry.size() < 0x100)
                p->aentry.push_back(CHASEGRAPH::ENTRY{NArg(a, 0), NArg(a, 1), nullptr});
            return CRef(TAGK_Void);
        });
        // method 1173 (0x10FAC0 -> 0x14B468): the chase on a graph (reaction 0xE), or with none the chase ended
        // (reaction 1) when it runs
        RegisterObjectMethod(0x10FAC0, [](LO* plo, const std::vector<CRef>& a) {
            GUARD* pg = PguardFromLo(plo);
            if (!pg) return CRef(TAGK_Void);
            auto* pr = dynamic_cast<JOBREACT*>(PjobFind(pg, 0x9C));
            LO* pgraph = PloArg(a, 0);
            if (s_fDebug)
                std::fprintf(stderr, "chase: t %.3f guard %d method 1173 graph %d (react %p)\n", g_clock.t, plo->oid,
                             pgraph ? pgraph->oid : -1, (void*)pr);
            AIMSG m;
            if (pgraph)
            {
                m.k = 0xE;
                m.plo = pgraph;
                if (pr) pr->React(m);
            }
            else if (PjobFind(pg, 0xB6))
            {
                m.k = 1;
                if (pr) pr->React(m);
            }
            return CRef(TAGK_Void);
        });
    }
} s_initchase;
}  // namespace
