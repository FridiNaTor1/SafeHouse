// launcher.cpp: class 0x70 (module 0x1D6848..0x1D7238): an object that launches another one at a target. The guards'
// strikes: the guard's class-0x70 child (oid 13048 in Paris) gets its target and aim from the strike animation's
// script event (ASEG 125117 event 4: the child's sidebag proc 1312), the script attaches an instance of the strike
// proxy (a pooled class-0x51 object, oid 19728) at the launch point and calls the launcher's method 508 on it
// (0x1058F0 -> 0x1D6B00), which aims and throws it. Part of the AI code. Reference: the decomp's
// build/hand_wip/func_001D6B00.cpp (readable C).

#include "objmethod.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include <string>
void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)
#include "activate.h"
#include "clock.h"
#include "disc.h"
#include "lo.h"
#include "lox.h"
#include "alo.h"
#include "projectile.h"
#include "guard.h"
#include "detect.h"
#include "aijobs.h"
#include "ai.h"
#include "xf.h"
#include "step.h"
#include "sidebag.h"
#include "update.h"
#include "proc.h"
#include "frame.h"
#include "so.h"
#include "sw.h"
#include "worldaccess.h"
#include "animfind.h"
#include "po.h"
#include "task.h"
#include "vecmat.h"
#include "vtnative.h"
const std::vector<SOE*>& ApsoeOccluders();  // spawner.cpp (0x1B44D8)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

glm::mat3 BuildLookAtMatrixZ(const glm::vec3& vec);  // mat.h (0x11B7E0)
float GRandInRange(float gLow, float gHigh);  // xfmath.h (0x11D448)
extern uint32_t g_uRandState;  // xfmath.cpp

ALO* g_paloLaunched = nullptr;  // (debug: the latest thrown object)
int g_cLaunchedFrames = 0;
namespace
{
// orig 0x001FFCF8 (as spliceglue.cpp's): an option id's EOPID index for the class
int EopidFromOptid(CID cid, int optid)
{
    if (cid < 0) return -1;
    const uint32_t c = g_disc.U32Elf(S3VA(0x279AD0) + static_cast<uint32_t>(cid) * 8);
    const uint32_t a = g_disc.U32Elf(S3VA(0x279AD0) + static_cast<uint32_t>(cid) * 8 + 4);
    int lo = 0, hi = static_cast<int>(c) - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) >> 1;
        const int k = static_cast<int>(g_disc.U32Elf(a + static_cast<uint32_t>(mid) * 8));
        if (k == optid) return static_cast<int>((g_disc.U32Elf(a + static_cast<uint32_t>(mid) * 8 + 4) - S3VA(0x2AC880)) / 0x28);
        if (optid < k) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

// The launcher's fields (orig 0x70 class, InitLo 0x1D6A90; the options 503..550, grf 0x92 fields)
struct LAUNCHER
{
    glm::vec3 offset{0.0f};                 // +0x50 (504): the aim point in the target's frame
    LO* ptarget = nullptr;                  // +0x60 (503): the target
    glm::vec3 polyPredict{0.0f};            // +0x70 (545): the lead time a + d (b + d c) by the distance d
    glm::vec2 rangePredict{0.0f, FLT_MAX};  // +0x80 (546): its clamp
    glm::vec3 polyDisplace{0.0f, 1.0f, 0.0f};  // +0x90 (547): the throw's distance by d
    glm::vec2 rangeDisplace{0.0f, FLT_MAX}; // +0xA0 (548)
    glm::vec2 rangeAngular{0.0f, 0.0f};     // +0xA8 (549): the spin's speed
    float gPitch = 0.5f;                    // +0xB0 (550): the ballistic case's pitch (0x1D6848)
};

std::unordered_map<const LO*, LAUNCHER>& MpLauncher()
{
    static std::unordered_map<const LO*, LAUNCHER> s_mp;
    WORLD_TABLE_LOCAL(s_mp);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_mp;
}

float GEval(const glm::vec3& p, float d) { return p.x + d * (p.y + d * p.z); }
float GClamp(float g, const glm::vec2& r)
{
    if (g < r.x) return r.x;
    if (r.y < g) return r.y;
    return g;
}
glm::vec3 Normal(const glm::vec3& v)
{
    const float s = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (s < 0.0001f) return glm::vec3(1.0f, 0.0f, 0.0f);  // (D_002A2DA0)
    return v * (1.0f / s);
}

// The values a script sets (seto!) or the load gave (the fields): the native launcher record
LAUNCHER& Launcher(LO* plo)
{
    auto it = MpLauncher().find(plo);
    if (it != MpLauncher().end()) return it->second;
    LAUNCHER& l = MpLauncher()[plo];
    auto opt = [&](int eopid) -> const OPTVAL* {
        for (const OPT& o : plo->aopt)
            if (o.eopid == eopid && !o.aval.empty()) return &o.aval[0];
        return nullptr;
    };
    // the loaded target (option 503, EOPID 460: an object, +0x60) and aim point in its frame (504, EOPID 461: +0x50),
    // e.g. world 8's turret launcher 9070 (natively only a script's seto! set them: its shots went to the origin)
    if (const OPTVAL* v = opt(460)) l.ptarget = v->pbasic ? dynamic_cast<LO*>(v->pbasic) : (v->n != -1 && v->n != 0 ? PloFindSwNearest(plo, v->n) : nullptr);
    if (const OPTVAL* v = opt(461)) l.offset = glm::vec3(v->ag[0], v->ag[1], v->ag[2]);
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("launcher %d: loaded target %d offset %.1f %.1f %.1f\n", plo->oid, l.ptarget ? l.ptarget->oid : -1, l.offset.x, l.offset.y, l.offset.z);
    if (const OPTVAL* v = opt(462)) l.polyPredict = glm::vec3(v->ag[0], v->ag[1], v->ag[2]);
    if (const OPTVAL* v = opt(463)) l.rangePredict = glm::vec2(v->ag[0], v->ag[1]);
    if (const OPTVAL* v = opt(464)) l.polyDisplace = glm::vec3(v->ag[0], v->ag[1], v->ag[2]);
    if (const OPTVAL* v = opt(465)) l.rangeDisplace = glm::vec2(v->ag[0], v->ag[1]);
    if (const OPTVAL* v = opt(466)) l.rangeAngular = glm::vec2(v->ag[0], v->ag[1]);
    if (const OPTVAL* v = opt(467)) l.gPitch = v->ag[0];
    return l;
}

// a vector, a CLQ (g0 g1 g2) or an LM (gMin gMax): the floats in order
glm::vec3 VecOfRef(const CRef& r)
{
    return r.m_tagk == TAGK_Vector || r.m_tagk == TAGK_Clq || r.m_tagk == TAGK_Lm ? r.GetVector() : glm::vec3(0.0f);
}

// orig: 0x001D6848 CalculateLaunchVelocity: the velocity reaching pos from posStart under the acceleration accel. The
// pitch share g lifts the launch angle from the straight line's elevation (at least level) toward vertical by sin(g);
// the speed follows from the flight under accel's z; none when it cannot reach (zero).
glm::vec3 VelBallistic(float gPitch, const glm::vec3& posStart, const glm::vec3& pos, const glm::vec3& accel)
{
    float k = std::sin(gPitch);  // sinf 0x245000
    glm::vec3 d = pos - posStart;
    const float z = d.z;
    d.z = 0.0f;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    float angle = std::atan2(z, len);  // atan2f 0x2458F0
    if (angle < 0.0f) angle = 0.0f;
    float q = accel.z;  // (dot with D_002A2DC0, the z axis)
    k = angle + (1.57079637051f - angle) * k;
    q = q / ((z - len * std::tan(k)) * 2.0f);  // tanf 0x2450F0
    if (q <= 0.0f) return glm::vec3(0.0f);
    q = std::sqrt(q);
    q = (len * q) / std::cos(k);  // cosf 0x244E10
    glm::vec3 vel = d;
    vel.z = len * std::tan(k);
    const float s = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    return vel * (1.0f / s) * q;
}

// orig: 0x001D6B00 (method 508): po thrown from where it is at the target's aim point (predicted by the lead time,
// then displaced along the line), into the world, free of constraints; a class-0x51 object flies straight at its
// speed (+0x260, option 509) facing its velocity, others ballistically; a random spin; class 0x50's slot 78
void Launch(LO* plaunch, LO* po)
{
    if (!po || !FCidDerivedFrom(po->Cid(), S3CID(0x30))) return;
    ALO* palo = static_cast<ALO*>(po);
    if (std::getenv("SLY2N_PROJEND"))  // (debug: every launch's time)
        std::printf("launch t %.3f %d by %d\n", g_clock.t, po->oid, plaunch ? plaunch->oid : -1);
    // (debug: SLY2N_RANDATLAUNCH=oid:state: the random state set at this object's launch, e.g. a reference's at 0x1D6B00)
    if (static const char* s_sz = std::getenv("SLY2N_RANDATLAUNCH"); s_sz && std::atoi(s_sz) == po->oid && std::strchr(s_sz, ':'))
    {
        static bool s_fDone = false;
        if (!s_fDone) g_uRandState = static_cast<uint32_t>(std::strtoul(std::strchr(s_sz, ':') + 1, nullptr, 0));
        s_fDone = true;
    }
    const LAUNCHER l = Launcher(plaunch);  // (a copy: the scripts the add runs may add launchers)
    UpdateAloXfWorlds(palo);  // (its place as the script set it, option 34)
    const glm::vec3 posStart = PosAloCenter(palo);
    glm::mat4 m(1.0f);
    if (l.ptarget && FCidDerivedFrom(l.ptarget->Cid(), S3CID(0x30)))
    {
        if (const XF* frame = PxfObj(static_cast<ALO*>(l.ptarget))) m = frame->matWorld;
    }
    else if (const LOX* located = dynamic_cast<const LOX*>(l.ptarget))
        m = located->pxf ? located->pxf->matWorld * located->mat : located->mat;
    glm::vec3 d = glm::vec3(m * glm::vec4(l.offset, 1.0f)) - posStart;
    float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float tLead = GClamp(GEval(l.polyPredict, s), l.rangePredict);
    // 0x2309E8: predict the complete target transform chain, including its local aim point.
    if (tLead != 0.0f) d = PredictProjectileAim(l.ptarget, l.offset, tLead) - posStart;
    s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float sDisplace = GClamp(GEval(l.polyDisplace, s), l.rangeDisplace);
    const glm::vec3 posDest = posStart + Normal(d) * sDisplace;
    if (FCidDerivedFrom(po->Cid(), S3CID(0x4F))) SetProjectileLauncher(palo, plaunch);  // 0x13BC60 (0x134320)
    AddLo(g_psw, po);  // (vt slot 8)
    SetAloCnstrLin(palo, 0);  // 0x1F5720
    SetAloCnstrAng(palo, 0);  // 0x1F57C0
    SetAloParent(palo, nullptr, nullptr, 3);  // 0x1A58D0(p, 0, 3): vt slot 10 (0x1F21B8), world place kept, out of the hand's record
    // its speed (+0x260, option 509: as loaded, or set by a script)
    float gSpeed = 0.0f;
    {
        const int eopid = EopidFromOptid(po->Cid(), 509);
        bool fSet = false;
        for (const OPT& o : po->aopt)
            if (o.eopid == eopid && !o.aval.empty())
            {
                if (std::getenv("SLY2N_HITDEBUG")) std::printf("  speed option tagk %d n %d g %.1f\n", o.aval[0].tagk, o.aval[0].n, o.aval[0].ag[0]);
                gSpeed = o.aval[0].tagk == S3TAG(0x15) ? static_cast<float>(o.aval[0].n) : o.aval[0].ag[0];
                fSet = true;
            }
        if (!fSet)
        {
            // 0x1B1DB8 initializes the missile family's launch speed to 800.
            const float fallback = FCidDerivedFrom(po->Cid(), S3CID(0x50)) ? 800.0f : 0.0f;
            uint32_t bits;
            std::memcpy(&bits, &fallback, 4);
            const uint32_t u = po->U32Field(0, 0x260, bits);
            std::memcpy(&gSpeed, &u, 4);
        }
    }
    if (FCidDerivedFrom(po->Cid(), S3CID(0x52)))
    {
        SetAloVelocity(palo, glm::vec3(gSpeed, 0.0f, 0.0f));  // 0x1F4898 (class 0x52: its speed along its own x)
    }
    else if (FCidDerivedFrom(po->Cid(), S3CID(0x51)))
    {
        const glm::vec3 v = Normal(posDest - posStart) * gSpeed;
        SetAloVelocityTo(palo, v);  // (vt slot 35)
        SetAloRotation(palo, BuildLookAtMatrixZ(v));  // 0x11B7E0, vt slot 30
    }
    else
    {
        // 0x1F5860: the object's gravity (its mass state's +0x80; zero without one)
        const SOST* ps = Psost(palo);
        SetAloVelocityTo(palo, VelBallistic(l.gPitch, posStart, posDest, ps ? ps->dvGravity : glm::vec3(0.0f)));
    }
    if (l.rangeAngular.y > 0.0f)
    {
        const float g = GRandInRange(l.rangeAngular.x, l.rangeAngular.y);
        const float x = GRandInRange(-1.0f, 1.0f), y = GRandInRange(-1.0f, 1.0f), z = GRandInRange(-1.0f, 1.0f);
        SetAloAngVelocityTo(palo, Normal(glm::vec3(x, y, z)) * g);  // (vt slot 36)
    }
    if (FCidDerivedFrom(po->Cid(), S3CID(0x50)))
    {
        using PFN = void (*)(ALO*, LO*, const glm::vec3&);
        if (PFN pfn = PfnVt<PFN>(po, 78)) pfn(palo, l.ptarget, l.offset);
    }
    g_paloLaunched = palo;
    g_cLaunchedFrames = 12;
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("launch: t %.3f %d (cid 0x%X) from %.1f %.1f %.1f to %.1f %.1f %.1f speed %.1f target %d lead %.3f vel %.1f %.1f %.1f pitch %.3f\n",
                    g_clock.t, po->oid, po->Cid(), posStart.x, posStart.y, posStart.z, posDest.x, posDest.y, posDest.z, gSpeed,
                    l.ptarget ? l.ptarget->oid : -1, tLead, VelAloPos(palo).x, VelAloPos(palo).y, VelAloPos(palo).z, l.gPitch);
    if (std::getenv("SLY2N_HITDEBUG"))
        if (const GUARD* pgL = plaunch->paloParent ? PguardFromLo(plaunch->paloParent) : nullptr)
            std::printf("  the launcher's guard pursues %d (%p), target %p\n", pgL->plo38C ? pgL->plo38C->oid : -1, (void*)pgL->plo38C, (void*)l.ptarget);
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("  target cid 0x%X parent %d in %d; launcher %d parent %d\n", l.ptarget ? l.ptarget->Cid() : -1,
                    l.ptarget && l.ptarget->paloParent ? l.ptarget->paloParent->oid : -1, l.ptarget ? (int)l.ptarget->fInWorld : -1, plaunch->oid,
                    plaunch->paloParent ? plaunch->paloParent->oid : -1);
    if (std::getenv("SLY2N_HITDEBUG"))
    {
        std::printf("  offset %.2f %.2f %.2f target pos %.1f %.1f %.1f predict %.4f %.4f %.4f\n", l.offset.x, l.offset.y, l.offset.z,
                    m[3][0], m[3][1], m[3][2], l.polyPredict.x, l.polyPredict.y, l.polyPredict.z);
    }
}

// orig: 0x0020AA40 / 0x0020AA78 (the guards' splice methods 0x102900 / 0x102988)
static CRef FnAddGuardAction(LO* plo, const std::vector<CRef>& a)
{
    GUARD* pg = PguardFromLo(plo);
    if (!plo || a.empty()) return CRef(TAGK_Void);
    OID oid = -1;
    LO* po = nullptr;
    if (a[0].m_tagk == TAGK_S32) oid = a[0].m_n;
    else if ((po = PloArg(a, 0))) oid = po->oid;
    if (oid == -1) return CRef(TAGK_Void);
    if (!pg)
    {
        // the loader's call (the option's value given by an expression): before the guard's post-load, which
        // reads its options 233: kept as one (unless the loader recorded it already)
        for (const OPT& o : plo->aopt)
            if (o.eopid == S3EOPID(233) && !o.aval.empty() && o.aval[0].n == oid) return CRef(TAGK_Void);
        OPT o;
        o.eopid = 233;
        OPTVAL v;
        v.tagk = TAGK_S32;
        v.n = oid;
        o.aval.push_back(v);
        plo->aopt.push_back(o);
        return CRef(TAGK_Void);
    }
    if (pg->aoidEB8.size() >= 8) return CRef(TAGK_Void);
    pg->aoidEB8.push_back(oid);
    if (!po) po = PloFindSwNearest(plo, oid);
    if (po && std::find(pg->aploEDC.begin(), pg->aploEDC.end(), po) == pg->aploEDC.end()) pg->aploEDC.push_back(po);
    if (std::getenv("SLY2N_SELDEBUG")) std::fprintf(stderr, "ai: guard %d action %d (%s)\n", plo->oid, oid, po ? "found" : "missing");
    return CRef(TAGK_Void);
}

// debug: SLY2N_FINDTEXT=text: the procs (the objects' frames and sidebags) whose code contains text, once
void FindTextInProcs(SW*)
{
    if (const char* szE = std::getenv("SLY2N_OPTIDOF"))  // debug: the guards' optids of an EOPID
    {
        static bool s_f = false;
        if (!s_f)
        {
            s_f = true;
            for (int optid = 0; optid < 4096; ++optid)
                if (EopidFromOptid(std::strchr(szE, ':') ? CID(std::strtol(std::strchr(szE, ':') + 1, nullptr, 0)) : CID(0x39), optid) == std::atoi(szE))
                    std::printf("optidof: eopid %s is optid %d\n", szE, optid);
        }
    }
    if (const char* szO = std::getenv("SLY2N_EOPIDOF"))  // debug: "optid:cid,optid:cid": their EOPIDs
    {
        static bool s_f = false;
        if (!s_f)
        {
            s_f = true;
            for (const char* q = szO; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
            {
                const int optid = std::atoi(q);
                const CID cid = std::strchr(q, ':') ? CID(std::strtol(std::strchr(q, ':') + 1, nullptr, 0)) : CID(0x39);
                std::printf("eopidof: optid %d of cid 0x%X is eopid %d\n", optid, cid, EopidFromOptid(cid, optid));
            }
        }
    }
    static bool s_fDone = false;
    const char* sz = std::getenv("SLY2N_FINDTEXT");
    if (s_fDone || !sz || !g_psw) return;
    s_fDone = true;
    std::unordered_map<const CFrame*, bool> mpf;
    auto scan = [&](const CRef& r, int id, LO* plo, const char* where) {
        if (r.m_tagk != TAGK_Proc || !r.m_pproc) return;
        std::string str;
        CRef body;
        body.SetPair(r.m_pproc->m_ppairCodeExpr);
        PrintRef(body, &str);
        if (str.find(sz) != std::string::npos)
            std::printf("findtext: %s %d of %d (cid 0x%X): %s\n", where, id, plo->oid, plo->Cid(), str.substr(0, 3000).c_str());
    };
    for (const auto& plo : g_psw->aploAll)
    {
        // the object's frame and its parents (the modules' frames)
        std::vector<const CFrame*> apf;
        if (plo->pframe) apf.push_back(plo->pframe.get());
        for (size_t i = 0; i < apf.size(); ++i)
        {
            const CFrame* pf = apf[i];
            if (mpf[pf]) continue;
            mpf[pf] = true;
            for (const VARB& v : pf->m_avarb) scan(v.ref, v.symid, plo.get(), i == 0 ? "frame symbol" : "parent frame symbol");
            for (const CFrame* pp : pf->m_apframeParent)
                if (pp) apf.push_back(pp);
        }
        if (plo->psidebag)
            for (const SBB& b : plo->psidebag->m_asbb) scan(b.ref, b.n, plo.get(), "sidebag");
    }
}

// debug: SLY2N_GUARDLIST=n: the guards' place in the world at the update n
void ListGuards(SW* psw)
{
    static int s_c = 0;
    const char* sz = std::getenv("SLY2N_GUARDLIST");
    // ("*K": every K-th update, one line per guard in the world: place, stack, pocket mode)
    if (!sz || !psw) return;
    ++s_c;
    if (sz[0] == '*')
    {
        const int k = std::max(1, std::atoi(sz + 1));
        if (s_c % k) return;
        std::printf("guards %d t %.2f: game paused %d / %d / %d", s_c, g_clock.t, g_gamepause.c78, g_gamepause.c80, g_gamepause.c84);
        for (LO* plo : psw->mpcidaplo[S3CID(0x36)])  // (Murray, for the jobs that protect him)
            if (plo->fInWorld && FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
            {
                const glm::vec3 p = PosAloCenter(static_cast<ALO*>(plo));
                std::printf(" murray %d at %.0f %.0f %.0f", plo->oid, p.x, p.y, p.z);
            }
        std::printf("\n");
        for (CID cid = 0x38; cid <= 0x43; ++cid)
            for (LO* plo : psw->mpcidaplo[cid])
            {
                if (!plo->fInWorld || plo->fRemoved) continue;
                const glm::vec3 pos = PosAi(plo);
                std::printf("guards %d t %.2f: %d %p at %.0f %.0f %.0f stack", s_c, g_clock.t, plo->oid, (void*)plo, pos.x, pos.y, pos.z);
                GUARD* pg = PguardFromLo(plo);
                if (pg)
                    for (const auto& pj : pg->stack.ajob) std::printf(" %X", pj->cid);
                std::printf(" mode %d instance %d hp %d/%d mask 0x%016llX", pg ? pg->nMode : -9, plo->oidInstance,
                            pg ? pg->n33C : -1, pg ? pg->n338 : -1,
                            static_cast<unsigned long long>(pg ? pg->grfState : 0));
                const STEP* pst = PstepFromLo(plo);  // read-only lookup: the observer must not create STEP state
                const bool fFloor = pg && pg->fProbeRam ? pg->fFloorRam : pst && pst->pxpFloor;
                std::printf(" step %d vel %.3f %.3f uTurn %.5f heading %.5f actual %.5f floor %d\n",
                            pst != nullptr, pst ? pst->vecVel.x : 0.0f, pst ? pst->vecVel.y : 0.0f,
                            pst ? pst->uTurn : 0.0f, pst ? pst->radHeading : 0.0f,
                            pg ? RadGuardHeading(pg) : 0.0f, fFloor);
            }
        return;
    }
    if (s_c != std::atoi(sz)) return;
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            const ALO* pa = static_cast<const ALO*>(plo);
            const glm::vec3 pos = PosAi(plo);
            const glm::vec3 posXf = pa->axf.empty() ? glm::vec3(-1.0f) : glm::vec3(pa->axf[0]->matWorld[3]);
            std::printf("guardlist %d %p cid 0x%X: inworld %d removed %d clone %d parent %d at %.0f %.0f %.0f (record 0 %.0f %.0f %.0f) children %zu\n",
                        plo->oid, (void*)plo, plo->Cid(), plo->fInWorld, plo->fRemoved, plo->ploCloneOf != nullptr,
                        plo->paloParent ? plo->paloParent->oid : -1, pos.x, pos.y, pos.z, posXf.x, posXf.y, posXf.z, plo->aploOwned.size());
            if (const GUARD* pg = PguardFromLo(plo))
                std::printf("  g314 %.0f g318 %.0f FE8 0x%X first %.0f %.0f actions %zu\n", pg->g314, pg->g318, (unsigned)pg->grfFE8, pg->g31C, pg->g320,
                            pg->aploEDC.size());
        }
}

struct INITLAUNCHER
{
    INITLAUNCHER()
    {
        RegisterSwPass(0x147F40, FindTextInProcs);
        RegisterSwPass(0x147F40, ListGuards);
        RegisterObjectMethod(0x1058F0, [](LO* plo, const std::vector<CRef>& a) {  // 508: 0x1D6B00
            if (std::getenv("SLY2N_HITDEBUG"))
            {
                std::printf("launcher %d %p (parent %p): method 508 with %s", plo->oid, (void*)plo, (void*)plo->paloParent,
                            PloArg(a, 0) ? "an object" : "nothing");
                for (int id : {1314, 1315, 1316, 1317})
                {
                    CRef r;
                    if (plo->psidebag) plo->psidebag->FFindBinding(id, &r);
                    std::printf(" %d=%d", id, r.m_tagk == TAGK_S32 ? r.m_n : -999);
                }
                std::printf("\n");
                CRef r;
                if (plo->psidebag && plo->psidebag->FFindBinding(1312, &r) && r.m_tagk == TAGK_Proc && r.m_pproc && r.m_pproc->m_pframe)
                    for (int sym : {1523, 1524, 1525, 1526})
                        if (CRef* pb = r.m_pproc->m_pframe->PrefFindBinding(sym, 1); pb && pb->m_tagk == TAGK_Basic)
                        {
                            LO* p = dynamic_cast<LO*>(pb->m_pbasic);
                            std::printf("  s%d = %d %p (parent %p)\n", sym, p ? p->oid : -1, (void*)p, p ? (void*)p->paloParent : nullptr);
                            if (const LOX* px = dynamic_cast<const LOX*>(p))
                            {
                                const glm::mat4 mw = px->pxf ? px->pxf->matWorld * px->mat : px->mat;
                                std::printf("    lox mat %.1f %.1f %.1f pxf %p world %.1f %.1f %.1f\n", px->mat[3][0], px->mat[3][1], px->mat[3][2],
                                            (void*)px->pxf, mw[3][0], mw[3][1], mw[3][2]);
                            }
                        }
            }
            Launch(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        // the script's seto! of the aim (503 the target, 504 its point, 545..550 the curves)
        // class 0x3C's method 970 (0x10DA68 -> 0x1BA980): the command list it runs near Sly (+0x108C, its place +0x1090
        // reset), told to its reactions (message 15: job 0xAE)
        RegisterObjectMethod(0x10DA68, [](LO* plo, const std::vector<CRef>& a) {
            if (GUARD* pg = PguardFromLo(plo))
            {
                pg->plo108C = PloArg(a, 0);
                pg->n108C = pg->plo108C ? 1 : 0;
                pg->saved1090 = SAVEDSCRIPT{};
                if (std::getenv("SLY2N_SCPTPRINT"))
                    std::fprintf(stderr, "ai: guard %d method 970 list %d\n", plo->oid, pg->plo108C ? pg->plo108C->oid : -1);
                if (auto* pj = static_cast<JOBREACT*>(PjobFind(pg, 0x9C)))
                {
                    AIMSG m;
                    m.k = 15;
                    pj->React(m);
                }
            }
            return CRef(TAGK_Void);
        });
        // the guards' splice method 0x102900 (-> 0x20AA40, the setter of option 233 too): an action object's oid
        // appended to +0xEB8 (at most 8); the Paris guards get theirs from the world's script after the load, so it
        // is resolved into +0xEDC here as well (0x20AAA8 at the post-load)
        // (0x102988 -> 0x20AA78: the same with the post-load's resolution 0x20AAA8 at once)
        // the ALOs' splice methods 0x100D18 / 0x100DA0 (EOPIDs 44 / 45 (options 110 / 111): 0x1F6978 / 0x1F69F8, the
        // mesh groups with an id shown / hidden; also the loader's calls of option 45 (the groups hidden at the load))
        RegisterObjectMethod(0x100D18, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) ShowAloGlobs(static_cast<ALO*>(plo), NArg(a, 0), true);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x100DA0, [](LO* plo, const std::vector<CRef>& a) {
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30))) ShowAloGlobs(static_cast<ALO*>(plo), NArg(a, 0), false);
            return CRef(TAGK_Void);
        });
        // the guards' method 454 (0x103FE8 -> 0x206820): the full reset (slot 27 (0): TODO(ai), message 0xF, slot 26,
        // message 0xE)
        RegisterObjectMethod(0x103FE8, [](LO* plo, const std::vector<CRef>&) {
            if (!PguardFromLo(plo)) return CRef(TAGK_Void);
            HandleLoSpliceEvent(plo, 15, false, 0, nullptr);
            ResetGuard(plo, nullptr);
            HandleLoSpliceEvent(plo, 14, false, 0, nullptr);
            return CRef(TAGK_Void);
        });
        // the guards' methods 360 (0x102A10 -> 0x20AB58: the action list +0xED8 / +0xEFC emptied), 390 (0x103630 ->
        // 0x2087C8: the detector's targets less the generated ones, the looked-for classes +0x394 / +0x3A4 cleared),
        // 395 (0x1038B8 -> 0x2089E8: the excluded classes +0x3A8 cleared)
        RegisterObjectMethod(0x102A10, [](LO* plo, const std::vector<CRef>&) {
            if (GUARD* pg = PguardFromLo(plo)) { pg->aoidEB8.clear(); pg->aploEDC.clear(); }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x103630, [](LO* plo, const std::vector<CRef>&) {
            if (GUARD* pg = PguardFromLo(plo))
            {
                // the detector's count (+0xF64 +0xFC) less the generated targets (+0x3A4), which are its last ones
                if (pg->pdet) pg->pdet->ae.resize(pg->pdet->ae.size() - std::min(pg->pdet->ae.size(), size_t(std::max(pg->c3A4, 0))));
                pg->acid398.clear();
                pg->c3A4 = 0;
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x1038B8, [](LO* plo, const std::vector<CRef>&) {
            if (GUARD* pg = PguardFromLo(plo)) pg->acid3AC.clear();
            return CRef(TAGK_Void);
        });
        // (method 1173, 0x10FAC0 -> 0x14B468, Carmelita's chase job 0xB6: aichase.cpp)
        RegisterObjectMethod(0x102900, FnAddGuardAction);
        RegisterObjectMethod(0x102988, FnAddGuardAction);
        // orig: 0x0010F7B0 -> 0x00155FB0 (the boss's option 1156, EOPID 1341): where his missile from pos aims: the
        // player 50 up, led by its velocity when it moves (faster than 60: the lead's polynomial / limits blended by
        // the suck), else 60 ahead of its facing; then the first of 11 headings around that direction (0, -/+0.025 ..
        // -/+0.4) whose line from pos is clear of the world's geometry (0x1B44D8), else the one hit farthest away
        RegisterObjectMethod(0x10F7B0, [](LO*, const std::vector<CRef>& a) {
            const glm::vec3 pos = a.empty() ? glm::vec3(0.0f) : a[0].GetVector();
            LO* plo = PloPoCurrent(g_psw);  // 0x1C4840
            ALO* palo = plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
            CRef r;
            if (!palo) { r.SetVector(pos); return r; }
            const glm::vec3 posPlayer = PosLoWorld(palo);
            const float dist = glm::length(posPlayer - pos);
            const float u = GSuck();  // 0x154C90
            const glm::vec3 posTarget = posPlayer + glm::vec3(0.0f, 0.0f, 50.0f);  // (g_normalZ x 50)
            const glm::vec3 vel = VelAloPos(palo);  // vt slot 33
            glm::vec3 off;
            if (60.0f < glm::length(vel))  // D_002BDC98
            {
                const float u1 = 1.0f - u;
                // VecCombo(u1, D_002BDC60 {0, 1e-4, 0}, u, D_002BDC80 {0, 0, 0}); limits D_002BDC70 {0.1, 0.4} / D_002BDC90 {0, 0}
                const glm::vec3 clq = u1 * glm::vec3(0.0f, 9.99999975e-05f, 0.0f) + u * glm::vec3(0.0f);
                const float g = clq.x + dist * (clq.y + dist * clq.z);
                const float gMin = u1 * 0.1f + u * 0.0f, gMax = u1 * 0.4f + u * 0.0f;
                off = vel * (g < gMin ? gMin : (gMax < g ? gMax : g));
            }
            else
            {
                // 0x1A6318: the rotation of its object record (vt slot 31), its first row's heading
                const glm::mat4 m = palo->apxfRef[2] ? palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
                off = cmm::SetVectorCylind(std::atan2(m[0].y, m[0].x), 60.0f, 0.0f);
            }
            float pan, tilt, s;
            cmm::ConvertVectorToSphere(posTarget + off - pos, &pan, &tilt, &s);
            const std::vector<SOE*>& apsoe = ApsoeOccluders();  // 0x1B44D8
            static const float s_adPan[11] = {0.0f, -0.025f, 0.025f, -0.05f, 0.05f, -0.1f, 0.1f, -0.2f, 0.2f, -0.4f, 0.4f};  // D_002BDCA0
            float gBest = -FLT_MAX, panBest = pan;  // (D_002A513C)
            for (int i = 0; i < 11; ++i)
            {
                const float panT = cmm::RadNormalize(pan + s_adPan[i]);  // 0x11CF60
                glm::vec3 test = pos + cmm::SetVectorSphere(panT, tilt, s);
                LSG lsg;
                const bool fHit = !apsoe.empty() && PsoeHitTestLineObjects(0, pos, test, apsoe, &lsg);  // 0x14CC58
                if (std::getenv("SLY2N_HITDEBUG"))
                    std::printf("  aim try %d pan %.3f: %zu occluders, hit %d at %.0f %.0f %.0f\n", i, panT, apsoe.size(), (int)fHit,
                                lsg.pos0.x, lsg.pos0.y, lsg.pos0.z);
                if (!fHit) { panBest = panT; break; }
                test = lsg.pos0;
                const float g = glm::length(pos - test);
                if (gBest < g) { gBest = g; panBest = panT; }
            }
            r.SetVector(pos + cmm::SetVectorSphere(panBest, tilt, s));
            if (std::getenv("SLY2N_HITDEBUG"))
                std::printf("boss aim: from %.0f %.0f %.0f player %.0f %.0f %.0f facing %.2f %.2f vel %.0f -> %.0f %.0f %.0f\n", pos.x, pos.y, pos.z, posPlayer.x,
                            posPlayer.y, posPlayer.z, palo->apxfRef[2] ? palo->apxfRef[2]->matWorld[0].x : 9.0f,
                            palo->apxfRef[2] ? palo->apxfRef[2]->matWorld[0].y : 9.0f, glm::length(vel), r.GetVector().x, r.GetVector().y, r.GetVector().z);
            return r;
        });
        // the guards' option 386 (EOPID 260, field +0x38C): the pursued target (the launcher's script's default target)
        RegisterObjectGetter(260, [](LO* plo) {
            CRef r;
            if (GUARD* pg = PguardFromLo(plo); pg && pg->plo38C) r.SetBasic(pg->plo38C);
            return r;
        });
        // the objects' options 34 / 35 (EOPIDs 21 / 22, grf 0x24: setters vt slots 29 / 30): the position and the
        // rotation a script sets (the launcher's script places the thrown object at its launch point)
        RegisterObjectGetter(21, [](LO* plo) {  // 0x1A62E8: the position (an ALO's object record's, a LOX's matrix's)
            CRef r;
            if (const LOX* plox = dynamic_cast<const LOX*>(plo); plox && !FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
            {
                const glm::mat4 mw = plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
                r.SetVector(glm::vec3(mw[3]));
            }
            else r.SetVector(PosAi(plo));
            return r;
        });
        RegisterObjectGetter(22, [](LO* plo) {  // 0x1A6318: the rotation (an ALO's object record's, a LOX's)
            CRef r;
            glm::mat4 mw(1.0f);
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
            {
                const ALO* palo = static_cast<const ALO*>(plo);
                if (palo->apxfRef[2]) mw = palo->apxfRef[2]->matWorld;
            }
            else if (const LOX* plox = dynamic_cast<const LOX*>(plo))
                mw = plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
            mw[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            r.SetMatrix(mw);
            return r;
        });
        RegisterObjectSetter(21, [](LO* plo, const CRef& r) {
            if (std::getenv("SLY2N_HITDEBUG") && plo->oid == 19728) std::printf("set 34 of %d tag %d\n", plo->oid, int(r.m_tagk));
            if (r.m_tagk != TAGK_Vector) return;
            const glm::vec3 pos = r.GetVector();
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
            {
                ALO* palo = static_cast<ALO*>(plo);
                // (a step's vt slot 29 is 0x2041C0: SetAloPos (0x1F40A0) with the position as given, then its floor
                // point +0x230; the getter's position is the object record's (0x1A62E8), so no +0x240 here: R29,
                // Dimitri put at the barrel's position by event 123835 sat 137.7 above it)
                if (STEP* pstep = PstepFromLo(palo)) SetStepPos(pstep, pos);
                else SetAloPos(palo, pos);
                UpdateAloXfWorlds(palo);
            }
            else if (LOX* plox = dynamic_cast<LOX*>(plo)) SetLoxPos(plox, pos);
        });
        RegisterObjectSetter(22, [](LO* plo, const CRef& r) {
            if (r.m_tagk != TAGK_Matrix) return;
            const glm::mat3 m(r.GetMatrix());
            if (FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
            {
                ALO* palo = static_cast<ALO*>(plo);
                if (STEP* pstep = PstepFromLo(palo)) SetStepRotation(pstep, m);
                else SetAloRotation(palo, m);
                UpdateAloXfWorlds(palo);
            }
            else if (LOX* plox = dynamic_cast<LOX*>(plo)) SetLoxRotation(plox, m);
        });
        RegisterObjectSetter(460, [](LO* plo, const CRef& r) {
            if (std::getenv("SLY2N_HITDEBUG")) std::printf("launcher %d target tag %d\n", plo->oid, int(r.m_tagk));
            Launcher(plo).ptarget = r.m_tagk == TAGK_Basic ? dynamic_cast<LO*>(r.m_pbasic) : nullptr;
        });
        RegisterObjectSetter(461, [](LO* plo, const CRef& r) { Launcher(plo).offset = VecOfRef(r); });
        RegisterObjectSetter(462, [](LO* plo, const CRef& r) { Launcher(plo).polyPredict = VecOfRef(r); });
        RegisterObjectSetter(463, [](LO* plo, const CRef& r) { Launcher(plo).rangePredict = glm::vec2(VecOfRef(r)); });
        RegisterObjectSetter(465, [](LO* plo, const CRef& r) { Launcher(plo).rangeDisplace = glm::vec2(VecOfRef(r)); });
        RegisterObjectSetter(466, [](LO* plo, const CRef& r) { Launcher(plo).rangeAngular = glm::vec2(VecOfRef(r)); });
        RegisterObjectSetter(464, [](LO* plo, const CRef& r) { Launcher(plo).polyDisplace = VecOfRef(r); });
        RegisterObjectSetter(467, [](LO* plo, const CRef& r) {
            Launcher(plo).gPitch = r.GAsFloat();
        });
    }
} s_initlauncher;
}  // namespace

// (at a world's load: the old world's objects are freed, and a new object at a freed one's address must not
// inherit its record)
void ResetLaunchers() { MpLauncher().clear(); }

// (the actors code, for Bentley's darts: 0x1357C0 sets +0x60 (the target) and +0x50 (the aim point))
void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset)
{
    LAUNCHER& l = Launcher(plaunch);
    l.ptarget = ptarget;
    l.offset = offset;
}
void LaunchFrom(LO* plaunch, LO* po) { Launch(plaunch, po); }  // (method 508's, for Bentley's darts)
