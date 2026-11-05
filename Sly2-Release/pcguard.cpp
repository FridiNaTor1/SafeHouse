// PC addition, not in the original game: the debug menu's playable guards and bosses (pcguard.h).
// Nothing here runs unless the debug menu (Debug keys) asks for it.
#include "pcguard.h"

#include "actanim.h"
#include "aseg.h"
#include "asega.h"
#include "anim.h"
#include "cls.h"
#include "frame.h"
#include "lo.h"
#include "proc.h"
#include "sm.h"
#include "sw.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>

void PrintRef(const CRef& ref, std::string* ps);  // bif.cpp (debug)

namespace
{
void PrintIndices(FILE* pf, const OPTVAL& v)
{
    std::fprintf(pf, "{");
    if (v.alist.empty()) std::fprintf(pf, "%d", v.n);
    for (const OPTVAL& w : v.alist) std::fprintf(pf, "%d ", w.n);
    std::fprintf(pf, "}");
}

LO* PloNear(SW* psw, LO* proot, int oid)
{
    LO* ploAny = nullptr;
    for (const auto& plo : psw->aploAll)
        if (plo->oid == oid)
        {
            if (FLoInParentChain(plo.get(), proot)) return plo.get();
            if (!ploAny) ploAny = plo.get();
        }
    return ploAny;
}

void PrintAnim(FILE* pf, SW* psw, ALO* proot, int oid)
{
    LO* plo = PloNear(psw, proot, oid);
    if (!plo) { std::fprintf(pf, " (oid %d missing)", oid); return; }
    ASEG* paseg = nullptr;
    if (FCidDerivedFrom(plo->Cid(), 0x6D)) paseg = static_cast<ASEG*>(plo);
    else if (plo->Cid() == 0x6F)
    {
        std::fprintf(pf, " SM");
        for (const SM::SMT& t : static_cast<SM*>(plo)->asmt)
            if (t.paseg) { paseg = t.paseg; break; }
    }
    else { std::fprintf(pf, " cid 0x%X", plo->Cid()); return; }
    if (!paseg) return;
    const ASEGD* pd = PasegdOf(paseg);
    glm::vec4 v(0.0f), v2(0.0f);
    if (const ASEGCHN* pchn = PchnFindAseg(paseg, proot, 1, true); pchn && pchn->ptrkp)
    {
        pchn->ptrkp->Eval(nullptr, &v, pd->tMax * 0.25f, 1.0f);
        pchn->ptrkp->Eval(nullptr, &v2, pd->tMax * 0.75f, 1.0f);
    }
    std::fprintf(pf, " aseg %d t %.2f vel %.0f %.0f %.0f / %.0f %.0f %.0f", paseg->oid, pd->tMax, v.x, v.y, v.z, v2.x, v2.y, v2.z);
}

void PrintScripts(FILE* pf, LO* plo, const char* indent)
{
    if (!plo->pframe) return;
    for (int sym : {2, 3, 4})
        if (CRef* pref = plo->pframe->PrefFindBinding(SYMID(sym), 1); pref && pref->m_tagk == TAGK_Proc && pref->m_pproc)
        {
            std::string str;
            CRef body;
            body.SetPair(pref->m_pproc->m_ppairCodeExpr);
            PrintRef(body, &str);
            std::fprintf(pf, "%sscript %d: %s\n", indent, sym, str.substr(0, 1500).c_str());
        }
}
}  // namespace

// Debug survey (SLY2N_GUARDSURVEY=1 with --load-world W): every guard template of the world, its kind table (the
// state indices -> animations), its action objects (cid 0x78: ranges, the animation), its animations' events and
// scripts, and the projectiles / launchers in its tree.
void SurveyGuards(SW* psw, FILE* pf)
{
    InitWorldAnimationRecords(psw);
    std::unordered_set<int> seen;
    for (int cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            if (plo->iclone != 0 || !seen.insert(plo->oid).second || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            ALO* palo = static_cast<ALO*>(plo);
            std::fprintf(pf, "== guard %d cid 0x%X records %zu globs %zu radius %.0f\n", plo->oid, plo->Cid(), palo->axf.size(),
                         palo->pglobset ? palo->pglobset->aglob.size() : 0, plo->sRadius);
            for (const OPT& o : plo->aopt)
            {
                const auto& v = o.aval;
                if ((o.eopid == 243 || o.eopid == 245) && v.size() >= 2)
                {
                    std::fprintf(pf, "  kind%s ", o.eopid == 245 ? "2" : "");
                    PrintIndices(pf, v[0]);
                    std::fprintf(pf, " oid %d grf %d:", v[1].n, v.size() > 2 ? v[2].n : 0);
                    PrintAnim(pf, psw, palo, v[1].n);
                    std::fprintf(pf, "\n");
                }
                else if (o.eopid == 244 && v.size() >= 5)
                {
                    std::fprintf(pf, "  kind3 ");
                    PrintIndices(pf, v[0]);
                    for (int i = 1; i < 5; ++i) { std::fprintf(pf, " [%d", v[size_t(i)].n); PrintAnim(pf, psw, palo, v[size_t(i)].n); std::fprintf(pf, "]"); }
                    std::fprintf(pf, "\n");
                }
                else if ((o.eopid == 246 || o.eopid == 247 || o.eopid == 248) && v.size() >= 3)
                {
                    std::fprintf(pf, "  hit%d mask 0x%X oid %d grf %d:", o.eopid, v[0].n, v[1].n, v[2].n);
                    PrintAnim(pf, psw, palo, v[1].n);
                    std::fprintf(pf, "\n");
                }
                else if (o.eopid == 233 && !v.empty())
                    std::fprintf(pf, "  eb8 %d\n", v[0].n);
                else if (std::getenv("SLY2N_GUARDSURVEYOPTS"))
                {
                    std::fprintf(pf, "  opt %d:", o.eopid);
                    for (const OPTVAL& w : v) std::fprintf(pf, " [n %d g %g %g %g]", w.n, w.ag[0], w.ag[1], w.ag[2]);
                    std::fprintf(pf, "\n");
                }
            }
            // its tree: actions, projectiles, launchers, emitters, the other animations
            for (const auto& up : psw->aploAll)
            {
                LO* p = up.get();
                if (p == plo || !FLoInParentChain(p, plo)) continue;
                const CID c = p->Cid();
                if (c == 0x78)
                {
                    std::fprintf(pf, "  action %d:", p->oid);
                    for (const OPT& o : p->aopt)
                    {
                        std::fprintf(pf, " %d=", o.eopid);
                        for (const OPTVAL& w : o.aval) std::fprintf(pf, "[%d %g %g]", w.n, w.ag[0], w.ag[1]);
                        if (o.eopid == 364 && !o.aval.empty()) PrintAnim(pf, psw, palo, o.aval[0].n);
                    }
                    std::fprintf(pf, "\n");
                    PrintScripts(pf, p, "    ");
                }
                else if (FCidDerivedFrom(c, 0x6D))
                {
                    ASEG* pa = static_cast<ASEG*>(p);
                    const ASEGD* pd = PasegdOf(pa);
                    int cev = 0;
                    for (int i = 0; i < 3; ++i) cev += int(pd->aaev[size_t(i)].size());
                    std::fprintf(pf, "  aseg %d parent %d t %.2f events %d\n", p->oid, p->paloParent ? p->paloParent->oid : -1, pd->tMax, cev);
                    if (std::getenv("SLY2N_GUARDSURVEYEV"))
                        for (int i = 0; i < 3; ++i)
                            for (const ASEGEV& ev : pd->aaev[size_t(i)])
                                if (ev.kind != 0 && ev.kind != 1)
                                    std::fprintf(pf, "    ev%d t %.2f kind %d an %d %d %d %d ag %.1f %.1f %.1f\n", i, ev.t, ev.kind, ev.an[0], ev.an[1],
                                                 ev.an[2], ev.an[3], ev.ag[0], ev.ag[1], ev.ag[2]);
                    PrintScripts(pf, p, "    ");
                }
                else if (c == 0x06)
                {
                    std::fprintf(pf, "  proxy %d parent %d:", p->oid, p->paloParent ? p->paloParent->oid : -1);
                    if (auto it = psw->mpproxy.find(p); it != psw->mpproxy.end() && it->second.iwe >= 0)
                    {
                        const WE& we = psw->awe[size_t(it->second.iwe)];
                        std::fprintf(pf, " group %d '%s' inst %d:", it->second.iwe, we.StrName().c_str(), we.cinst);
                        for (int i = 0; i < we.cloLoaded && i < int(we.aplo.size()); ++i)
                            std::fprintf(pf, " %d/0x%X", we.aplo[size_t(i)]->oid, we.aplo[size_t(i)]->Cid());
                    }
                    std::fprintf(pf, "\n");
                }
                else if (c == 0x70)
                {
                    std::fprintf(pf, "  launcher %d parent %d\n", p->oid, p->paloParent ? p->paloParent->oid : -1);
                    if (std::getenv("SLY2N_GUARDSURVEYEV"))
                    {
                        for (const OPT& o : p->aopt)
                        {
                            std::fprintf(pf, "    opt %d:", o.eopid);
                            for (const OPTVAL& w : o.aval) std::fprintf(pf, " [n %d g %g %g %g]", w.n, w.ag[0], w.ag[1], w.ag[2]);
                            std::fprintf(pf, "\n");
                        }
                        PrintScripts(pf, p, "    ");
                    }
                }
                else if (c != 0x6F && c != 0x6E)
                    std::fprintf(pf, "  lo %d cid 0x%X %s parent %d\n", p->oid, c, g_acls[c].name ? g_acls[c].name : "", p->paloParent ? p->paloParent->oid : -1);
            }
        }
}

// ============================================================ the playable guards and bosses
#include "brx.h"
#include "activate.h"
#include "alo.h"
#include "clock.h"
#include "emitter.h"
#include "glob.h"
#include "guard.h"
#include "joy.h"
#include "pccharacter.h"
#include "pcloader.h"
#include "po.h"
#include "projectile.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "worldaccess.h"
#include "rumble.h"
#include "cm.h"
#include "aseg.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);  // launcher.cpp
void LaunchFrom(LO* plaunch, LO* po);                                       // launcher.cpp (method 508, 0x1D6B00)

namespace
{
// ------------------------------------------------------------ the table
// One entry per playable character: where its model is (a world record and the guard object there), its jump, which of
// its own attacks Square and Circle play. Everything else is read from the object's own AI data when it is first used
// (PcGuardBegin): its idle / walk / run / jump / turn animations (its kind table, option 243..245), its attacks (its
// action objects, cid 0x78: option 233's list and the ones in its tree; none: its kind table's strike {3}), each attack's
// hit windows (the animation's events 15 / 16: its collision entries of a tag on / off), shots (events 12, thrown by its
// launcher, cid 0x70, with the projectile of its proxy) and effects (events 17: an explosion, cloned into the world).
struct PCGUARDDEF
{
    const char* pchzName;
    const char* pchzMoves;  // what Square / Circle do (the menu's status line)
    int episode;            // 0 the prologue, 1..8
    bool fBoss;
    int iwrec;              // the world record whose file has it
    int oid;                // its guard object there
    float hJump;            // Cross: its jump's height (-1: 160 if it has a jump (kind-2 entry {15}), 0: no jump)
    int iactSquare;         // Square: its attack (index in its attack list; -1: the first close-range one)
    int iactCircle;         // Circle: its special (index; -1: the first long-range one (a shot), else the next one)
    int oidIdle;            // its idle (-1: its kind table's {2})
    // its collision size and camera (pccharacter.h PCCHARDEF's; 0: computed from its body's collision entry)
    float kCollXY = 0.0f, kCollZ = 0.0f, uCamDist = 0.0f, sCamHeight = 0.0f;
};

const PCGUARDDEF s_agdef[] = {
    // the prologue (Cairo museum)
    {"Police pig", "no attacks (the prologue's police only patrol)", 0, false, 0, 11539, 0.0f, -1, -1, -1},
    // Episode 1: Paris
    {"Rat", "Square: swing; Circle: lunge and thrown rock", 1, false, 2, 125317, -1.0f, 0, 2, -1},
    {"Rifle rat", "Square: rifle butt strike", 1, false, 4, 132522, -1.0f, -1, -1, -1},
    {"Flashlight guard", "Square: club swing; Circle: gunshot", 1, false, 2, 11539, 0.0f, -1, 2, -1},
    // Episodes 2 / 3: India (the same types in both)
    {"Monkey", "Square: weapon strike", 2, false, 8, 95176, -1.0f, -1, -1, -1},
    {"Flashlight guard", "Square: swing; Circle: thrown sword", 2, false, 8, 11539, 0.0f, -1, -1, -1},
    {"Cobra", "Square: bite (it does not walk: it glides)", 2, false, 8, 95030, 0.0f, -1, -1, 65390},
    {"Monkey", "Square: weapon strike", 3, false, 8, 95176, -1.0f, -1, -1, -1},
    {"Flashlight guard", "Square: swing; Circle: thrown sword", 3, false, 8, 11539, 0.0f, -1, -1, -1},
    {"Cobra", "Square: bite (it does not walk: it glides)", 3, false, 8, 95030, 0.0f, -1, -1, 65390},
    // Episodes 4 / 5: Prague
    {"Spider", "Square: bite", 4, false, 14, 49856, -1.0f, -1, -1, -1},
    {"Mace guard", "Square: mace swing; Circle: thrown rocks", 4, false, 14, 59433, -1.0f, 0, 3, -1},
    {"Flashlight guard", "Square: swing; Circle: gunshot", 4, false, 14, 11539, 0.0f, -1, 2, -1},
    {"Spider", "Square: bite", 5, false, 14, 49856, -1.0f, -1, -1, -1},
    {"Mace guard", "Square: mace swing; Circle: thrown rocks", 5, false, 14, 59433, -1.0f, 0, 3, -1},
    {"Flashlight guard", "Square: swing; Circle: gunshot", 5, false, 14, 11539, 0.0f, -1, 2, -1},
    {"Worm", "Square: lash (it does not walk: it glides)", 5, false, 19, 65374, 0.0f, -1, -1, 65390},
    // Episodes 6 / 7: Canada
    {"Lumberjack", "Square: axe combo; Circle: puck shot", 6, false, 27, 32423, -1.0f, -1, -1, -1},
    {"Bear", "Square: swipe", 6, false, 27, 33440, 0.0f, -1, -1, -1},
    {"Bear cub", "no attacks of its own", 6, false, 27, 56738, -1.0f, -1, -1, -1},
    {"Flashlight guard", "Square: swing; Circle: gunshot", 6, false, 32, 11539, 0.0f, -1, 2, -1},
    {"Lumberjack", "Square: axe combo; Circle: puck shot", 7, false, 27, 32423, -1.0f, -1, -1, -1},
    {"Bear", "Square: swipe", 7, false, 27, 33440, 0.0f, -1, -1, -1},
    {"Flashlight guard", "Square: swing; Circle: gunshot", 7, false, 32, 11539, 0.0f, -1, 2, -1},
    // Episode 8: the blimp
    {"Hammer guard", "Square: hammer; Circle: shot", 8, false, 38, 19626, -1.0f, -1, -1, -1},
    {"Flying guard", "Square: strike", 8, false, 38, 9072, -1.0f, -1, -1, -1},
    {"Flashlight guard", "Square: swing; Circle: gunshot", 8, false, 38, 11539, 0.0f, -1, 2, -1},
    // the bosses (their episodes: the menu's Bosses page lists them all)
    {"Dimitri", "Square: strike; Circle: muzzle blast and shot", 1, true, 5, 122332, -1.0f, -1, -1, -1},
    {"Rajan", "Square: staff combo; Circle: staff blast", 3, true, 13, 95460, -1.0f, 2, 0, -1},
    {"The Contessa", "Square: lunge; Circle: her spell's missiles", 5, true, 22, 60283, -1.0f, 2, 1, -1},
    {"Jean Bison", "Square: charge; Circle: thrown bomb", 7, true, 37, 32250, -1.0f, -1, -1, -1},
};
constexpr int CGDEF = int(sizeof(s_agdef) / sizeof(s_agdef[0]));

bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_PCCHARPRINT") != nullptr;
    return s_f;
}

// ------------------------------------------------------------ the current guard (resolved from its AI data)
struct HITWIN
{
    float t0 = 0.0f, t1 = 0.0f;
    int tag = 0;            // its collision entries (SOE tag) turned on by the animation (event 15 .. 16)
};
struct FXEV
{
    float t = 0.0f;
    int oid = -1;           // an explosion (event 17)
};
struct ACT
{
    LO* plo = nullptr;      // the action object (cid 0x78; null: the kind table's strike)
    int oidAnim = -1;
    int oidAseg = -1;       // the animation played (an SM's: its first transition's)
    float tMax = 0.0f;
    float sRange = 300.0f;  // its reach (option 353's far end)
    std::vector<HITWIN> awin;
    std::vector<float> atShot;  // its launch events (kind 12; with a launcher and a projectile)
    std::vector<FXEV> afx;
};
struct GRT
{
    int iguard = -1;
    SW* pswKept = nullptr;
    ALO* proot = nullptr;
    std::vector<ACT> aact;
    int iactSq = -1, iactCi = -1;
    LO* plaunch = nullptr;        // its launcher (cid 0x70)
    LO* ptmplProj = nullptr;      // the projectile its launcher throws (its proxy's group's object, cid 0x4F..)
    float sWalk = 180.0f, sRun = 600.0f;
    float hJump = 0.0f;
} s_grt;

// ASEG of an oid near the root (an SM: its first transition's)
ASEG* PasegNear(SW* psw, ALO* proot, int oid)
{
    if (oid < 0) return nullptr;
    LO* plo = PloNear(psw, proot, oid);
    if (plo && FCidDerivedFrom(plo->Cid(), 0x6D)) return static_cast<ASEG*>(plo);
    if (plo && plo->Cid() == 0x6F)
        for (const SM::SMT& t : static_cast<SM*>(plo)->asmt)
            if (t.paseg) return t.paseg;
    return nullptr;
}

glm::vec3 VelRootOf(ASEG* paseg, ALO* proot, float t)
{
    glm::vec4 v(0.0f);
    if (!paseg) return glm::vec3(0.0f);
    if (const ASEGCHN* pchn = PchnFindAseg(paseg, proot, 1, true); pchn && pchn->ptrkp) pchn->ptrkp->Eval(nullptr, &v, t, 1.0f);
    return glm::vec3(v);
}

float SpeedOf(SW* psw, ALO* proot, int oid)
{
    ASEG* paseg = PasegNear(psw, proot, oid);
    return paseg ? glm::length(glm::vec2(VelRootOf(paseg, proot, PasegdOf(paseg)->tMax * 0.5f))) : 0.0f;
}

int CountIndices(const OPTVAL& v) { return v.alist.empty() ? 1 : int(v.alist.size()); }
bool FHasIndex(const OPTVAL& v, int n)
{
    if (v.alist.empty()) return v.n == n;
    for (const OPTVAL& w : v.alist)
        if (w.n == n) return true;
    return false;
}

// The template a proxy instantiates: its group's first loaded object of the class (or derived)
LO* PloProxyTemplate(SW* psw, LO* pproxy, CID cid)
{
    auto it = psw->mpproxy.find(pproxy);
    if (it == psw->mpproxy.end() || it->second.iwe < 0 || it->second.iwe >= int(psw->awe.size())) return nullptr;
    const WE& we = psw->awe[size_t(it->second.iwe)];
    for (int i = 0; i < we.cloLoaded && i < int(we.aplo.size()); ++i)
        if (FCidDerivedFrom(we.aplo[size_t(i)]->Cid(), cid)) return we.aplo[size_t(i)];
    return nullptr;
}

// An attack from its animation: its length, hit windows, launches and explosions
bool FMakeAct(ACT* pa)
{
    ASEG* paseg = PasegNear(s_grt.pswKept, s_grt.proot, pa->oidAnim);
    if (!paseg) return false;
    const ASEGD* pd = PasegdOf(paseg);
    pa->oidAseg = paseg->oid;
    pa->tMax = pd->tMax;
    for (const ASEGEV& ev : pd->aaev[1])
    {
        if (ev.kind == 15) pa->awin.push_back({ev.t, pd->tMax, ev.an[1]});
        if (ev.kind == 16)
            for (HITWIN& w : pa->awin)
                if (w.tag == ev.an[1] && w.t1 >= pd->tMax && ev.t >= w.t0) w.t1 = ev.t;
        if (ev.kind == 12) pa->atShot.push_back(ev.t);
        if (ev.kind == 17 && ev.an[0] != -1) pa->afx.push_back({ev.t, ev.an[0]});
    }
    return true;
}

// ------------------------------------------------------------ the attack in progress
struct ATK
{
    int iact = -1;
    float tPrev = -1.0f;
    float rad = 0.0f;
    LO* ptarget = nullptr;
    std::vector<LO*> aploHit;     // the guards it hit already
    uint32_t serial = 0;
} s_atk;

// the projectiles and effects of the current world (clones made at its load)
std::vector<ALO*> s_apaloProj;
std::vector<float> s_atProj;   // when each was thrown
// its effects (the objects its attacks' events 7 / 8 / 17 name: emitters, explosions and their groups, ambient sounds):
// copies in the current world (made at its load), by their kept template
std::unordered_map<const LO*, LO*> s_mptmplcopy;
uint32_t s_serialProj = 0;
int s_iguardProj = -1;

bool FTargetGuard(LO* plo, LO* ploPlayer)
{
    if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x30) || !PguardFromLo(plo)) return false;
    return !(ploPlayer && FLoInParentChain(plo, ploPlayer));
}

// The guard to attack: the nearest within range ahead of rad (60 degrees either side, 1000 up or down)
LO* PloTarget(SW* psw, ALO* palo, float rad, float sRange)
{
    const glm::vec3 pos = PosAloCenter(palo);
    const glm::vec2 dir(std::cos(rad), std::sin(rad));
    LO* ploBest = nullptr;
    float gBest = 1e30f;
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            if (!FTargetGuard(plo, palo)) continue;
            const glm::vec3 d = PosAloCenter(static_cast<ALO*>(plo)) - pos;
            const float s = glm::length(glm::vec2(d));
            if (s < 1.0f || s > sRange || std::fabs(d.z) > 1000.0f) continue;
            const float c = glm::dot(glm::vec2(d) / s, dir);
            if (c < 0.5f) continue;
            const float g = s * (2.0f - c);
            if (g < gBest) { gBest = g; ploBest = plo; }
        }
    return ploBest;
}

// A contact from the player on every guard within r of pos (once per attack each): the kind and amount given (a
// collision entry's own: as the guard's strike would give it; so.cpp PaloAcceptContact: the guard's own hit reaction,
// damage and knockout)
void HitGuardsNear(SW* psw, ALO* paloPlayer, const glm::vec3& pos, float r, const void* pvOther, uint32_t grfFilter, int nState, int tag)
{
    for (CID cid = 0x38; cid <= 0x43; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            if (!FTargetGuard(plo, paloPlayer)) continue;
            if (std::find(s_atk.aploHit.begin(), s_atk.aploHit.end(), plo) != s_atk.aploHit.end()) continue;
            ALO* pguard = static_cast<ALO*>(plo);
            const glm::vec3 posG = PosAloCenter(pguard);
            // (in reach: the swing sweeps from the body to the entry: within its radius plus a guard's across of that
            // segment, its height plus a guard's up or down)
            const glm::vec3 posBody = PosAloCenter(paloPlayer);
            const glm::vec2 seg = glm::vec2(pos - posBody);
            const float u = glm::dot(seg, seg) > 1.0f ? std::clamp(glm::dot(glm::vec2(posG - posBody), seg) / glm::dot(seg, seg), 0.0f, 1.0f) : 1.0f;
            const glm::vec3 posNear = posBody + (pos - posBody) * u;
            if (FPrint() && glm::length(posG - pos) < 1500.0f)
                std::printf("pc guard: (tag %d at %.0f %.0f %.0f r %.0f: guard %d at %.0f)\n", tag, pos.x, pos.y, pos.z, r, plo->oid, glm::length(glm::vec2(posG - posNear)));
            if (glm::length(glm::vec2(posG - posNear)) > r + 80.0f || std::fabs(posG.z - posNear.z) > r + 150.0f) continue;
            CTMSG ctm;
            ctm.ploSender = paloPlayer;
            ctm.pvOther = pvOther;
            ctm.ploReceiver = pguard;
            ctm.grfFilter = grfFilter;
            ctm.nState = nState;
            ctm.pos = posG;
            ctm.vec = glm::vec3(std::cos(s_atk.rad), std::sin(s_atk.rad), 0.3f) * 600.0f;
            ALO* paloTook = PaloAcceptContact(&ctm);
            s_atk.aploHit.push_back(plo);
            if (FPrint())
                std::printf("pc guard: hit guard %d (tag %d) filter 0x%X state %d -> %s\n", plo->oid, tag, grfFilter, nState, paloTook ? "taken" : "refused");
        }
}

// A melee window: the puppet's collision entries of the tag (posed by its animation) against the world's guards
void MeleeHits(SW* psw, ALO* paloPlayer, int tag)
{
    bool fAny = false;
    if (tag != 0)
        for (const auto& up : s_grt.pswKept->aploAll)
        {
            if (!FCidDerivedFrom(up->Cid(), 0x30) || !FLoInParentChain(up.get(), s_grt.proot)) continue;
            ALO* palo = static_cast<ALO*>(up.get());
            for (const SOE& e : palo->asoe)
            {
                if (e.tag != tag || e.geomLocal.apos.empty()) continue;
                glm::vec3 c(0.0f);
                for (const glm::vec3& v : e.geomLocal.apos) c += v;
                c /= float(e.geomLocal.apos.size());
                float r = e.sRadius;
                for (const glm::vec3& v : e.geomLocal.apos) r = std::max(r, glm::length(v - c));
                const glm::mat4 m = e.pxf ? e.pxf->matWorld : MatWorldOfAlo(palo);
                fAny = true;
                HitGuardsNear(psw, paloPlayer, glm::vec3(m * glm::vec4(c, 1.0f)), std::max(r, 40.0f), &e, e.id ? e.id : 0x800u,
                              e.id ? e.state : -2, tag);
            }
        }
    if (!fAny)
    {
        // (no entry of the tag: a sphere ahead of the feet, at the chest)
        const float c = std::cos(s_atk.rad), s = std::sin(s_atk.rad);
        HitGuardsNear(psw, paloPlayer, PosAloCenter(paloPlayer) + glm::vec3(c * 90.0f, s * 90.0f, 20.0f), 110.0f, &s_atk, 0x800u, -2, tag);
    }
}

// A shot: a free projectile (a clone of its launcher's projectile) thrown by its own launcher from the chest, at the
// target (led by its velocity) or straight ahead; the player as its thrower (no contact with the player itself)
void ThrowShot(SW* psw, ALO* paloPlayer)
{
    if (s_serialProj != psw->serial || !s_grt.plaunch) return;
    ALO* po = nullptr;
    for (ALO* p : s_apaloProj)
        if (!p->fInWorld) { po = p; break; }
    if (!po) return;
    const float c = std::cos(s_atk.rad), s = std::sin(s_atk.rad);
    const glm::vec3 posMuzzle = PosAloCenter(paloPlayer) + glm::vec3(c * 70.0f, s * 70.0f, 30.0f);
    SetAloPos(po, posMuzzle);
    LO* ptarget = s_atk.ptarget && s_atk.ptarget->fInWorld ? s_atk.ptarget : nullptr;
    glm::vec3 posAim = posMuzzle + glm::vec3(c, s, 0.0f) * 3000.0f;
    if (ptarget)
    {
        const glm::vec3 posT = PosAloCenter(static_cast<ALO*>(ptarget));
        const glm::vec3 vT = VelAloPos(static_cast<ALO*>(ptarget));
        posAim = posT;
        for (int i = 0; i < 2; ++i) posAim = posT + vT * (glm::length(posAim - posMuzzle) / 1300.0f);
    }
    // (a missile flies along its own x: turned to the aim first; a homing one follows the target itself)
    const glm::vec3 dirAim = glm::normalize(posAim - posMuzzle + glm::vec3(0.0f, 0.0f, 1e-3f));
    const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), dirAim) + glm::vec3(1e-4f, 0.0f, 0.0f));
    SetAloRotation(po, glm::mat3(dirAim, side, glm::cross(dirAim, side)));
    if (ptarget && FCidDerivedFrom(ptarget->Cid(), 0x30) && FCidDerivedFrom(po->Cid(), 0x50))
        SetLauncherAim(s_grt.plaunch, ptarget, glm::vec3(0.0f, 0.0f, 0.0f));
    else
        SetLauncherAim(s_grt.plaunch, nullptr, posAim);
    LaunchFrom(s_grt.plaunch, po);
    SetProjectileLauncher(po, paloPlayer);
    for (size_t i = 0; i < s_apaloProj.size(); ++i)
        if (s_apaloProj[i] == po) s_atProj[i] = g_clock.t;
    ExpireProjectileIn(po, 2.5f);
    if (FPrint())
        std::printf("pc guard: shot %d from %.0f %.0f %.0f at %d, velocity %.0f %.0f %.0f\n", po->oid, posMuzzle.x, posMuzzle.y, posMuzzle.z,
                    ptarget ? ptarget->oid : -1, VelAloPos(po).x, VelAloPos(po).y, VelAloPos(po).z);
}

// An explosion's hit (its burst is the animation's own event, PcGuardPuppetEvent): an attack that hits only by its
// explosions (no hit windows: Rajan's staff blast) hurts the guards within 350 of a point ahead
void ExplodeHit(SW* psw, ALO* paloPlayer, int oid)
{
    const float c = std::cos(s_atk.rad), s = std::sin(s_atk.rad);
    const glm::vec3 pos = PosAloCenter(paloPlayer) + glm::vec3(c * 150.0f, s * 150.0f, 0.0f);
    HitGuardsNear(psw, paloPlayer, pos, 350.0f, &s_mptmplcopy, 0x800u, -3, oid);
    if (FPrint()) std::printf("pc guard: explosion %d hits around %.0f %.0f %.0f\n", oid, pos.x, pos.y, pos.z);
}

// The kept object an event names (near the guard, as the AI's PloFindSwNearest from its own object) and its copy here
LO* PloFxTemplate(int oid) { return oid >= 0 && s_grt.pswKept ? PloNear(s_grt.pswKept, s_grt.proot, oid) : nullptr; }
LO* PloFxCopy(int oid)
{
    const LO* ptmpl = PloFxTemplate(oid);
    const auto it = ptmpl ? s_mptmplcopy.find(ptmpl) : s_mptmplcopy.end();
    return it != s_mptmplcopy.end() ? it->second : nullptr;
}

// removed at its world's start (itself or a part it hangs on, below the guard): its events add it
bool FRemovedAtStart(const LO* plo)
{
    for (; plo && plo != s_grt.proot; plo = plo->paloParent)
        if (plo->fRemoveAtStart) return true;
    return false;
}

// An effect of the guard's (an object its animations' events name) copied into the running world: emitters (their
// particles at the guard's posed parts: a copy's records are the kept guard's, which the puppet poses), explosions and
// their groups (each member copied into the group's copy), ambient sounds. Not: the puppet's own drawn parts (shown /
// hidden on the puppet), volume triggers and other objects that act on the world (the Contessa's hypnosis cone).
void MakeFxCopy(SW* psw, LO* ptmpl, bool fAdd)
{
    if (!ptmpl || s_mptmplcopy.count(ptmpl)) return;
    const CID c = ptmpl->Cid();
    const bool fGroup = FCidDerivedFrom(c, 0x1F);
    if (!(c == 0x1D || fGroup || FCidDerivedFrom(c, 0x20) || c == 0x2B)) return;
    if (!FLoInParentChain(ptmpl, s_grt.proot)) return;   // (one placed in its world, not on the guard: not here)
    LO* plo = PloPcCloneIntoWorld(psw, ptmpl, fAdd);
    if (!plo) return;
    s_mptmplcopy[ptmpl] = plo;
    if (fGroup)
        if (const LOXCHILD* pg = dynamic_cast<const LOXCHILD*>(ptmpl))
            if (LOXCHILD* pgNew = dynamic_cast<LOXCHILD*>(plo))
            {
                // (the grouper's clone copies the members derived from 0x1E only: the rest copied one by one)
                if (pgNew->aploChild.size() < pg->aploChild.size())
                {
                    pgNew->aploChild.clear();
                    for (LO* pc : pg->aploChild)
                        if (LO* pcNew = PloPcCloneIntoWorld(psw, pc, true))
                        {
                            pgNew->aploChild.push_back(pcNew);
                            s_mptmplcopy.emplace(pc, pcNew);
                        }
                }
            }
    if (FPrint())
        std::printf("pc guard: effect %d (cid 0x%X%s) copied%s\n", ptmpl->oid, c, fGroup ? ", a group" : "", fAdd ? " (in the world)" : "");
}
}  // namespace

void AppendPcGuardCharDefs(std::vector<PCCHARDEF>& adef)
{
    static const int s_anNone[] = {-1};
    // (dizzy birds, the pocket's loot, the hit comics and effects: their AI shows those)
    static const int s_aoidHide[] = {13144, 12787, 13118, 13108, -1};
    static const int s_an0Hide[] = {13061, 13063, 12982, 13054, -1};
    for (int i = 0; i < CGDEF; ++i)
    {
        const PCGUARDDEF& g = s_agdef[i];
        // (Dimitri: his fight's red bubble, 122415, is his AI's to show)
        static const int s_an0HideDimitri[] = {13061, 13063, 12982, 13054, 122415, -1};
        PCCHARDEF d{g.pchzName, PCCHARK_Guard, g.iwrec, g.oid, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0.0f, {0.0f, 0.0f, 0.0f},
                    180.0f, 600.0f, g.oid == 122332 ? s_an0HideDimitri : s_an0Hide, s_aoidHide, s_anNone};
        d.kind = g.fBoss ? PCCHARK_Boss : PCCHARK_Guard;
        d.iep = g.episode;
        d.pvExtra = &g;
        d.kCollXY = g.kCollXY;
        d.kCollZ = g.kCollZ;
        d.uCamDist = g.uCamDist;
        d.sCamHeight = g.sCamHeight;
        adef.push_back(d);
    }
}

int IPcGuardOf(const PCCHARDEF& def)
{
    const PCGUARDDEF* p = static_cast<const PCGUARDDEF*>(def.pvExtra);
    return (def.kind == PCCHARK_Guard || def.kind == PCCHARK_Boss) && p >= s_agdef && p < s_agdef + CGDEF ? int(p - s_agdef) : -1;
}

const char* PchzPcGuardMoves(int iguard) { return iguard >= 0 && iguard < CGDEF ? s_agdef[iguard].pchzMoves : ""; }

void PcGuardBegin(PCCHARDEF* pdef, SW* pswKept, ALO* proot)
{
    const int iguard = IPcGuardOf(*pdef);
    if (iguard < 0) return;
    const PCGUARDDEF& g = s_agdef[iguard];
    s_grt = GRT{};
    s_grt.iguard = iguard;
    s_grt.pswKept = pswKept;
    s_grt.proot = proot;
    s_atk = ATK{};
    // its kind table: idle {2}, walk {9}, run (the fastest entry with 9 and another index, else the directional set {8}'s
    // forward), jump (the kind-2 entry {15}), turning in place (the directional set's sides), its strike {3}
    int oidIdle = -1, oidWalk = -1, oidRun = -1, oidJump = -1, oidStrike = -1;
    int oidTurnL = -1, oidTurnR = -1, oidRun3 = -1, oidIdle1 = -1;
    float sRunBest = 0.0f;
    for (const OPT& o : proot->aopt)
    {
        const auto& v = o.aval;
        if (o.eopid == 243 && v.size() >= 2)
        {
            const int c = CountIndices(v[0]);
            if (c == 1 && FHasIndex(v[0], 2) && oidIdle < 0) oidIdle = v[1].n;
            else if (c == 1 && FHasIndex(v[0], 1) && oidIdle1 < 0) oidIdle1 = v[1].n;
            else if (c == 1 && FHasIndex(v[0], 9)) oidWalk = v[1].n;
            else if (c == 1 && FHasIndex(v[0], 3)) oidStrike = v[1].n;
            else if (c == 2 && FHasIndex(v[0], 9))
            {
                const float sp = SpeedOf(pswKept, proot, v[1].n);
                if (sp > sRunBest) { sRunBest = sp; oidRun = v[1].n; }
            }
        }
        else if (o.eopid == 245 && v.size() >= 2 && CountIndices(v[0]) == 1 && FHasIndex(v[0], 15)) oidJump = v[1].n;
        else if (o.eopid == 244 && v.size() >= 5 && FHasIndex(v[0], 8))
        {
            oidRun3 = v[2].n;
            oidTurnR = v[3].n;
            oidTurnL = v[4].n;
        }
    }
    if (g.oidIdle >= 0) oidIdle = g.oidIdle;
    if (oidIdle < 0) oidIdle = oidIdle1;
    if (oidRun < 0 || (SpeedOf(pswKept, proot, oidRun3) > sRunBest)) oidRun = oidRun3 >= 0 ? oidRun3 : oidRun;
    if (oidWalk < 0) oidWalk = oidRun;
    if (oidRun < 0) oidRun = oidWalk;
    // its attacks: option 233's actions, then the action objects in its tree (load order); the animation (option 364),
    // the reach (353); none: its kind table's strike {3}
    std::vector<LO*> aploAct;
    for (const OPT& o : proot->aopt)
        if (o.eopid == 233 && !o.aval.empty())
            if (LO* p = PloNear(pswKept, proot, o.aval[0].n); p && p->Cid() == 0x78) aploAct.push_back(p);
    for (const auto& up : pswKept->aploAll)
        if (up->Cid() == 0x78 && FLoInParentChain(up.get(), proot)) aploAct.push_back(up.get());
    for (LO* p : aploAct)
    {
        ACT a;
        a.plo = p;
        for (const OPT& o : p->aopt)
        {
            if (o.eopid == 364 && !o.aval.empty()) a.oidAnim = o.aval[0].n;
            if (o.eopid == 353 && !o.aval.empty()) a.sRange = o.aval[0].ag[1];
        }
        if (a.oidAnim < 0) continue;
        if (std::find_if(s_grt.aact.begin(), s_grt.aact.end(), [&](const ACT& b) { return b.oidAnim == a.oidAnim; }) != s_grt.aact.end()) continue;
        if (FMakeAct(&a)) s_grt.aact.push_back(a);
    }
    if (s_grt.aact.empty() && oidStrike >= 0)
    {
        ACT a;
        a.oidAnim = oidStrike;
        if (FMakeAct(&a)) s_grt.aact.push_back(a);
    }
    // its launcher (cid 0x70) and the projectile it throws: the launcher's ammunition field (option 8: 1315 a missile,
    // 1331 a thrown object) names a proxy, whose group's object is the projectile. A missile launcher first (the flashlight
    // guards' bullets, the lumberjack's puck, the bosses' shots), else the thrower (the rocks). Without both no shots.
    for (int kWant : {1315, 1331})
        for (const auto& up : pswKept->aploAll)
        {
            LO* p = up.get();
            if (s_grt.ptmplProj || p->Cid() != 0x70 || !FLoInParentChain(p, proot)) continue;
            for (const OPT& o : p->aopt)
                if (o.eopid == 8 && o.aval.size() >= 2 && o.aval[0].n == kWant && o.aval[1].n != -1)
                    if (LO* pproxy = PloNear(pswKept, proot, o.aval[1].n); pproxy && pproxy->Cid() == 0x06)
                        if (LO* ptmpl = PloProxyTemplate(pswKept, pproxy, 0x4F))
                        {
                            s_grt.plaunch = p;
                            s_grt.ptmplProj = ptmpl;
                        }
        }
    // (no ammunition field names one: its own projectile proxy (a missile, a bullet, a bomb: not the generic thrown
    // rocks) with its first launcher: Dimitri's bullets, the Contessa's missiles)
    for (int iPass = 0; iPass < 2 && !s_grt.ptmplProj; ++iPass)
        for (const auto& up : pswKept->aploAll)
        {
            LO* p = up.get();
            if (s_grt.ptmplProj || p->Cid() != 0x06 || !FLoInParentChain(p, proot)) continue;
            auto it = pswKept->mpproxy.find(p);
            if (it == pswKept->mpproxy.end() || it->second.iwe < 0) continue;
            const std::string name = pswKept->awe[size_t(it->second.iwe)].StrName();
            if (iPass == 0 && (name.find("rock_throw") != std::string::npos || name.find("pickpocket") != std::string::npos)) continue;
            if (LO* ptmpl = PloProxyTemplate(pswKept, p, 0x4F)) s_grt.ptmplProj = ptmpl;
        }
    if (s_grt.ptmplProj && !s_grt.plaunch)
        for (const auto& up : pswKept->aploAll)
            if (!s_grt.plaunch && up->Cid() == 0x70 && FLoInParentChain(up.get(), proot)) s_grt.plaunch = up.get();
    if (!s_grt.plaunch || !s_grt.ptmplProj)
        for (ACT& a : s_grt.aact) a.atShot.clear();
    // Square: the given attack, else the first close one; Circle: the given one, else the first long-range one (a shot or
    // an explosion), else the next
    const int cact = int(s_grt.aact.size());
    auto fRanged = [](const ACT& a) { return !a.atShot.empty() || !a.afx.empty(); };
    s_grt.iactSq = g.iactSquare >= 0 && g.iactSquare < cact ? g.iactSquare : -1;
    for (int i = 0; i < cact && s_grt.iactSq < 0; ++i)
        if (!fRanged(s_grt.aact[size_t(i)])) s_grt.iactSq = i;
    if (s_grt.iactSq < 0 && cact > 0) s_grt.iactSq = 0;
    s_grt.iactCi = g.iactCircle >= 0 && g.iactCircle < cact ? g.iactCircle : -1;
    for (int i = 0; i < cact && s_grt.iactCi < 0; ++i)
        if (fRanged(s_grt.aact[size_t(i)]) && i != s_grt.iactSq) s_grt.iactCi = i;
    for (int i = 0; i < cact && s_grt.iactCi < 0; ++i)
        if (i != s_grt.iactSq) s_grt.iactCi = i;
    // the puppet's animations, speeds and jump
    pdef->oidIdle = oidIdle;
    pdef->oidWalk = oidWalk;
    pdef->oidRun = oidRun;
    pdef->oidJump = oidJump;
    pdef->oidFall = -1;
    pdef->oidLand = -1;
    pdef->oidTurnL = oidTurnL;
    pdef->oidTurnR = oidTurnR;
    pdef->oidAttack = s_grt.iactSq >= 0 ? s_grt.aact[size_t(s_grt.iactSq)].oidAnim : -1;
    s_grt.sWalk = std::max(60.0f, SpeedOf(pswKept, proot, oidWalk));
    s_grt.sRun = std::max(s_grt.sWalk, SpeedOf(pswKept, proot, oidRun));
    s_grt.hJump = g.hJump >= 0.0f ? g.hJump : oidJump >= 0 ? 160.0f : 0.0f;
    pdef->sWalk = s_grt.sWalk;
    pdef->sRun = s_grt.sRun;
    if (FPrint())
    {
        std::printf("pc guard: %s: idle %d walk %d (%.0f) run %d (%.0f) jump %d (%.0f) turn %d / %d, launcher %d projectile %d\n", g.pchzName,
                    oidIdle, oidWalk, s_grt.sWalk, oidRun, s_grt.sRun, oidJump, s_grt.hJump, oidTurnL, oidTurnR,
                    s_grt.plaunch ? s_grt.plaunch->oid : -1, s_grt.ptmplProj ? s_grt.ptmplProj->oid : -1);
        for (int i = 0; i < cact; ++i)
        {
            const ACT& a = s_grt.aact[size_t(i)];
            std::printf("  attack %d (%d): animation %d %.2f s reach %.0f%s%s:", i, a.plo ? a.plo->oid : -1, a.oidAnim, a.tMax, a.sRange,
                        i == s_grt.iactSq ? " [Square]" : "", i == s_grt.iactCi ? " [Circle]" : "");
            for (const HITWIN& w : a.awin) std::printf(" hit %.2f..%.2f tag %d", w.t0, w.t1, w.tag);
            for (float t : a.atShot) std::printf(" shot %.2f", t);
            for (const FXEV& f : a.afx) std::printf(" explosion %d at %.2f", f.oid, f.t);
            std::printf("\n");
            if (ASEG* pa = PasegNear(pswKept, proot, a.oidAnim))
                for (int i = 0; i < 3; ++i)
                    for (const ASEGEV& ev : PasegdOf(pa)->aaev[size_t(i)])
                    {
                        if (ev.kind == 0 || ev.kind == 1 || ev.kind == 5 || ev.kind == 15 || ev.kind == 16) continue;
                        LO* p = ev.an[0] >= 0 ? PloNear(pswKept, proot, ev.an[0]) : nullptr;
                        std::printf("    ev%d t %.2f kind %d an %d %d %d ag %.2f -> %s cid 0x%X removed-at-start %d parent %d\n", i, ev.t, ev.kind, ev.an[0], ev.an[1], ev.an[2],
                                    ev.ag[0], p ? "obj" : "-", p ? p->Cid() : 0, p ? int(p->fRemoveAtStart) : -1, p && p->paloParent ? p->paloParent->oid : -1);
                    }
        }
        for (const auto& up : pswKept->aploAll)
            if (FLoInParentChain(up.get(), proot) && (up->Cid() == 0x1D || FCidDerivedFrom(up->Cid(), 0x1F) || FCidDerivedFrom(up->Cid(), 0x20)))
                std::printf("  tree fx %d cid 0x%X removed-at-start %d parent %d\n", up->oid, up->Cid(), int(up->fRemoveAtStart),
                            up->paloParent ? up->paloParent->oid : -1);
    }
}

float PcGuardRunScale(const PCCHARDEF& def, bool fR1)
{
    (void)def;
    // (a scripted move, the pad at rest (a world's walk-in, a cutscene's): Sly's own speed, so it ends where it should)
    if (g_joyps2.uDeflect < 0.1f) return 1.0f;
    // (Sly's state 12 runs at about 700 units / s at full tilt: the guard's own walk, R1 its run)
    constexpr float c_sSlyRun = 700.0f;
    return std::clamp((fR1 ? s_grt.sRun : s_grt.sWalk) / c_sSlyRun, 0.1f, 1.6f);
}

float PcGuardJumpHeight(const PCCHARDEF& def) { return IPcGuardOf(def) == s_grt.iguard ? s_grt.hJump : 0.0f; }

void PcGuardAttack(ALO* paloPlayer, int ibtn)
{
    const int iact = ibtn == 0 ? s_grt.iactSq : s_grt.iactCi;
    if (iact < 0 || !g_psw) return;
    const ACT& a = s_grt.aact[size_t(iact)];
    STEP* pstep = PstepFromLo(paloPlayer);
    s_atk = ATK{};
    s_atk.iact = iact;
    s_atk.serial = g_psw->serial;
    s_atk.rad = pstep ? RadStepHeading(pstep) : 0.0f;
    const bool fRanged = !a.atShot.empty() || !a.afx.empty();
    s_atk.ptarget = PloTarget(g_psw, paloPlayer, s_atk.rad, fRanged ? 3000.0f : std::clamp(a.sRange, 400.0f, 1200.0f));
    // on the ground it turns to the guard aimed at
    if (s_atk.ptarget && pstep && pstep->pxpFloor)
    {
        const glm::vec3 d = PosAloCenter(static_cast<ALO*>(s_atk.ptarget)) - PosAloCenter(paloPlayer);
        s_atk.rad = std::atan2(d.y, d.x);
        SetStepPhysHeading(paloPlayer, s_atk.rad);
    }
    if (!FPcPuppetPlay(a.oidAnim, false)) { s_atk.iact = -1; return; }
    SetPcPuppetAttack(a.tMax);
    if (FPrint()) std::printf("pc guard: %s attack %d animation %d at %d\n", ibtn == 0 ? "Square" : "Circle", iact, a.oidAnim, s_atk.ptarget ? s_atk.ptarget->oid : -1);
}

glm::vec3 PcGuardAttackVel()
{
    if (s_atk.iact < 0 || !g_psw || s_atk.serial != g_psw->serial) return glm::vec3(0.0f);
    const ACT& a = s_grt.aact[size_t(s_atk.iact)];
    // (it stops at the guard aimed at: its lunge carries it there, not through it)
    if (LO* pt = s_atk.ptarget; pt && pt->fInWorld)
        if (LO* pp = PloPcPuppetPlayer(); pp && FCidDerivedFrom(pp->Cid(), 0x30) &&
            glm::length(glm::vec2(PosAloCenter(static_cast<ALO*>(pt)) - PosAloCenter(static_cast<ALO*>(pp)))) < 160.0f)
            return glm::vec3(0.0f);
    glm::vec3 v = VelRootOf(PasegNear(s_grt.pswKept, s_grt.proot, a.oidAnim), s_grt.proot, std::max(0.0f, s_atk.tPrev));
    const float c = std::cos(s_atk.rad), s = std::sin(s_atk.rad);
    return glm::vec3(c * v.x - s * v.y, s * v.x + c * v.y, 0.0f);
}

extern int g_iframeMain;   // game.cpp

// The missiles (class 0x52: the Contessa's) end by touching what they were thrown at, which a guard is not: here they
// hit the first guard they come near (their own contact kind and amount, fields +0x1EC / +0x1F0) and leave the world;
// any of the projectiles still flying 3 s after its throw leaves the world too (the pool is three)
void WatchProjectiles(SW* psw, ALO* paloPlayer)
{
    if (s_serialProj != psw->serial) return;
    for (size_t i = 0; i < s_apaloProj.size(); ++i)
    {
        ALO* po = s_apaloProj[i];
        if (!po->fInWorld) continue;
        bool fEnd = g_clock.t > s_atProj[i] + 3.0f;
        if (!fEnd && FCidDerivedFrom(po->Cid(), 0x52))
        {
            const glm::vec3 pos = PosAloCenter(po);
            for (CID cid = 0x38; cid <= 0x43 && !fEnd; ++cid)
                for (LO* plo : psw->mpcidaplo[cid])
                {
                    if (!FTargetGuard(plo, paloPlayer)) continue;
                    if (glm::length(PosAloCenter(static_cast<ALO*>(plo)) - pos) > 130.0f) continue;
                    CTMSG ctm;
                    ctm.ploSender = po;
                    ctm.ploReceiver = plo;
                    ctm.grfFilter = po->U32Field(0, 0x1EC, 0x100);
                    ctm.nState = static_cast<int>(po->U32Field(0, 0x1F0, static_cast<uint32_t>(-10)));
                    ctm.pos = pos;
                    ctm.vec = glm::normalize(VelAloPos(po) + glm::vec3(0.0f, 0.0f, 1e-3f)) * 500.0f;
                    ALO* paloTook = PaloAcceptContact(&ctm);
                    if (FPrint()) std::printf("pc guard: missile %d hits guard %d -> %s\n", po->oid, plo->oid, paloTook ? "taken" : "refused");
                    fEnd = true;
                    break;
                }
        }
        if (fEnd) RemoveLo(psw, po);
    }
}

void UpdatePcGuardAttack(SW* psw, ALO* paloPlayer)
{
    WatchProjectiles(psw, paloPlayer);
    // (test aid: SLY2N_PCGUARDNEAR=frame[,distance]: at that frame the player put that far from the nearest guard, facing it)
    if (static const char* s_sz = std::getenv("SLY2N_PCGUARDNEAR"); s_sz)
    {
        int f = -1, f2 = -1;
        float d = 150.0f, d2 = 150.0f;
        std::sscanf(s_sz, "%d,%f;%d,%f", &f, &d, &f2, &d2);
        if (g_iframeMain == f2) { f = f2; d = d2; }
        if (g_iframeMain == f)
        {
            const glm::vec3 pos = PosAloCenter(paloPlayer);
            ALO* pbest = nullptr;
            float gBest = 1e30f;
            for (CID cid = 0x38; cid <= 0x43; ++cid)
                for (LO* plo : psw->mpcidaplo[cid])
                    if (FTargetGuard(plo, paloPlayer))
                        if (const float g = glm::length(PosAloCenter(static_cast<ALO*>(plo)) - pos); g < gBest) { gBest = g; pbest = static_cast<ALO*>(plo); }
            if (STEP* pstep = PstepFromLo(paloPlayer); pstep && pbest)
            {
                const glm::vec3 posG = PosLoWorld(pbest);
                const glm::vec3 dir = glm::normalize(glm::vec3(glm::vec2(pos - posG), 0.0f) + glm::vec3(1e-3f, 0.0f, 0.0f));
                const float rad = std::atan2(-dir.y, -dir.x);
                SetStepPos(pstep, posG + dir * d + glm::vec3(0.0f, 0.0f, 20.0f));
                SetStepRotation(pstep, glm::mat3(glm::vec3(std::cos(rad), std::sin(rad), 0.0f), glm::vec3(-std::sin(rad), std::cos(rad), 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
                SetAloVelocityTo(paloPlayer, glm::vec3(0.0f));
                std::printf("pc guard: (test) put %.0f from guard %d\n", d, pbest->oid);
            }
        }
    }
    // (test aid: SLY2N_PCGUARDCAM=dist,height,yaw,lookz[,ahead]: the camera put that far from the guard's feet (yaw degrees
    // from its heading), looking at the point lookz above them (and ahead along its heading): for shots of its effects)
    if (static const char* s_szCam = std::getenv("SLY2N_PCGUARDCAM"); s_szCam && s_grt.proot && !s_grt.proot->axf.empty())
    {
        float d = 900.0f, h = 400.0f, yaw = 30.0f, z = 300.0f, fwd = 0.0f;
        std::sscanf(s_szCam, "%f,%f,%f,%f,%f", &d, &h, &yaw, &z, &fwd);
        const glm::mat4& m = s_grt.proot->axf[0]->matWorld;
        const float radH = std::atan2(m[0].y, m[0].x);
        float rad = radH + glm::radians(yaw);
        glm::vec3 posLook = glm::vec3(m[3]) + glm::vec3(std::cos(radH) * fwd, std::sin(radH) * fwd, z);
        if (const char* szAt = std::getenv("SLY2N_PCGUARDCAMAT"))   // (x y z: a fixed point looked at, yaw from +x)
            if (std::sscanf(szAt, "%f %f %f", &posLook.x, &posLook.y, &posLook.z) == 3) { rad = glm::radians(yaw); z = 0.0f; }
        SetCmLookAt(&g_cm, posLook + glm::vec3(std::cos(rad) * d, std::sin(rad) * d, h - z), posLook, 0.0f, 0.0f);
    }
    if (s_atk.iact < 0 || s_atk.serial != psw->serial) return;
    const ACT& a = s_grt.aact[size_t(s_atk.iact)];
    if (OidPcPuppetAnim() != a.oidAseg) { s_atk.iact = -1; return; }
    const float t = TPcPuppetAnim();
    for (const HITWIN& w : a.awin)
        if (t >= w.t0 && s_atk.tPrev <= w.t1) MeleeHits(psw, paloPlayer, w.tag);
    // (an attack with neither hit windows, launches nor explosions: a hit at 40 % of it, in front)
    if (a.awin.empty() && a.atShot.empty() && a.afx.empty() && t >= a.tMax * 0.4f && s_atk.tPrev < a.tMax * 0.4f) MeleeHits(psw, paloPlayer, 0);
    for (float ts : a.atShot)
        if (t >= ts && s_atk.tPrev < ts) ThrowShot(psw, paloPlayer);
    for (const FXEV& f : a.afx)
        if (t >= f.t && s_atk.tPrev < f.t && a.awin.empty()) ExplodeHit(psw, paloPlayer, f.oid);
    s_atk.tPrev = t;
    if (t >= a.tMax) s_atk.iact = -1;
}

void PcGuardOnWorldLoaded(SW* psw, PCCHARDEF* pdef, SW* pswKept, ALO* proot)
{
    s_apaloProj.clear();
    s_atProj.clear();
    s_mptmplcopy.clear();
    s_serialProj = 0;
    s_iguardProj = -1;
    s_atk = ATK{};
    if (!pdef || !pswKept || !proot) return;
    if (s_grt.iguard != IPcGuardOf(*pdef) || s_grt.proot != proot) PcGuardBegin(pdef, pswKept, proot);
    // three projectiles in flight at most (live copies, out of the world until thrown: pcloader.h PloPcCloneIntoWorld)
    if (s_grt.ptmplProj && s_grt.plaunch)
        for (int i = 0; i < 3; ++i)
        {
            LO* plo = PloPcCloneIntoWorld(psw, s_grt.ptmplProj, false);
            if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            s_apaloProj.push_back(static_cast<ALO*>(plo));
            s_atProj.push_back(0.0f);
        }
    // the effects its animations' events name (every attack's and its moves': start, play and end events), as its AI's animations use
    // them: added / removed (7 / 8: in the world from the start unless its world removes it at its start), explosions
    // (17: the effect, its source and touched objects)
    std::vector<int> aoidAnim;
    for (const ACT& a : s_grt.aact) aoidAnim.push_back(a.oidAnim);
    for (int oid : {pdef->oidIdle, pdef->oidWalk, pdef->oidRun, pdef->oidJump, pdef->oidTurnL, pdef->oidTurnR}) aoidAnim.push_back(oid);   // (Bison's snorts)
    for (int oidAnim : aoidAnim)
        if (ASEG* paseg = PasegNear(pswKept, proot, oidAnim))
            for (int i = 0; i < 3; ++i)
                for (const ASEGEV& ev : PasegdOf(paseg)->aaev[size_t(i)])
                {
                    if (ev.kind == 7 || ev.kind == 8)
                    {
                        LO* ptmpl = PloFxTemplate(ev.an[0]);
                        MakeFxCopy(psw, ptmpl, ptmpl && !FRemovedAtStart(ptmpl));
                    }
                    else if (ev.kind == 17)
                        for (int k = 0; k < 3; ++k) MakeFxCopy(psw, PloFxTemplate(ev.an[size_t(k)]), true);
                }
    // (the world's emitters are listed once per world: listed again so the copies burst; the running particles restart)
    if (!s_mptmplcopy.empty() && g_psw == psw) InitEmittersOfSw(psw);
    s_serialProj = psw->serial;
    s_iguardProj = IPcGuardOf(*pdef);
    if (FPrint())
        std::printf("pc guard: %zu projectiles of %d, %zu effects made in the world\n", s_apaloProj.size(),
                    s_grt.ptmplProj ? s_grt.ptmplProj->oid : -1, s_mptmplcopy.size());
}

// The puppet's animation events other than its sounds and mesh groups (pccharacter.cpp FirePuppetGlobEvents), as the
// world's ASEGA events (asegevent.cpp OnAsegaEvent) do them for the guard's own animation, on the guard's copies here:
// 7 / 8 an effect (or a drawn part of the puppet: the flashlight guard's muzzle flash) added / removed, 17 an explosion,
// 3 the pad's rumble. Not fired (they act on the guard's AI or its world, not on effects): 11 the AI's message, 12 the
// animation's script event (the shots are thrown here: ThrowShot), 4 a script started, 20 the eyes (its world's SAA),
// 2 (dropped natively too).
void PcGuardPuppetEvent(const ASEGEV& ev)
{
    SW* psw = g_psw;
    if (!psw || s_serialProj != psw->serial || !s_grt.proot) return;
    switch (ev.kind)
    {
    case 7:
    case 8:
    {
        const bool fOn = ev.kind == 7;
        if (FShowPcPuppetPart(ev.an[0], fOn, ev.ag[0])) break;
        if (LO* plo = PloFxCopy(ev.an[0]))
        {
            SwitchLoFaded(plo, fOn, ev.ag[0]);
            if (FPrint())
            {
                const glm::vec3 p = PosLoWorld(plo), q = s_grt.proot->axf.empty() ? glm::vec3(0.0f) : glm::vec3(s_grt.proot->axf[0]->matWorld[3]);
                const LOX* px = dynamic_cast<const LOX*>(plo);
                std::printf("pc guard: effect %d %s: in world %d active %d at %.0f %.0f %.0f (guard at %.0f %.0f %.0f, record of %d)\n", ev.an[0], fOn ? "added" : "removed",
                            int(plo->fInWorld), int(FLoActive(plo)), p.x, p.y, p.z, q.x, q.y, q.z, px && px->pxf && px->pxf->palo ? px->pxf->palo->oid : -1);
            }
        }
        break;
    }
    case 17:
    {
        LO* ploEffect = PloFxCopy(ev.an[0]);
        if (!ploEffect) break;
        BURSTPARAMS bp;
        bp.grf = 1;
        if (ev.an[1] != -1) bp.plo = PloFxCopy(ev.an[1]) ? PloFxCopy(ev.an[1]) : PloFxTemplate(ev.an[1]);
        if (!bp.plo) bp.plo = ploEffect;
        if (ev.an[2] != -1)
        {
            bp.ploTouch = PloFxCopy(ev.an[2]) ? PloFxCopy(ev.an[2]) : PloFxTemplate(ev.an[2]);
            if (bp.ploTouch) bp.grf |= 0x20;
        }
        if (ev.ag[0] != 1.0f)
        {
            bp.g8 = ev.ag[0];
            bp.grf |= 0x18;
        }
        ExplodeLo(ploEffect, &bp);
        if (FPrint()) std::printf("pc guard: explosion %d (copy %p, source %d)\n", ev.an[0], static_cast<void*>(ploEffect), bp.plo->oid);
        break;
    }
    case 3: TriggerJoyRumbleRumk(ev.an[0], ev.ag[0], "pc guard"); break;
    default:
        if (FPrint()) std::printf("pc guard: event %d (%d) not fired (its world's)\n", ev.kind, ev.an[0]);
        break;
    }
}

bool FPcGuardProjReady(SW* psw, const PCCHARDEF& def) { return psw && s_serialProj == psw->serial && s_iguardProj == IPcGuardOf(def); }

bool FPcGuardProjectile(const LO* plo) { return std::find(s_apaloProj.begin(), s_apaloProj.end(), plo) != s_apaloProj.end(); }

void PcGuardFixGroups(ALO* proot)
{
    if (!proot || !proot->pglobset) return;
    for (const GLOB& g : proot->pglobset->aglob)
        if (FAloGlobShown(proot, g.n0, !(g.grfGroup & 0x800))) return;
    std::vector<int> an0;
    for (const GLOB& g : proot->pglobset->aglob)
        if (std::find(an0.begin(), an0.end(), g.n0) == an0.end()) an0.push_back(g.n0);
    for (int n0 : an0) ShowAloGlobs(proot, n0, true);
    if (FPrint()) std::printf("pc guard: %d: its %zu groups were all hidden: shown\n", proot->oid, an0.size());
}

// ------------------------------------------------------------ the menu's controller
// (the guest loader and the PC Extras menu: rows of kind Guard / Boss are chosen through it). A row is played as the puppet
// (pccharacter.cpp: the model drawn on Sly's object, driven by the moves above); choosing one replaces Carmelita.
namespace
{
template <PCCHARK K>
struct GUARDCTRL
{
    static void Request(SW* psw, int idef)
    {
        if (idef < 0)
        {
            const int icur = IdefPcCharPuppet();
            if (icur >= 0 && PcCharDef(icur).kind == K) RequestPcCharPuppet(psw, -1);
            return;
        }
        RequestPcCharPuppet(psw, idef);
    }
    static int Current()
    {
        const int icur = IdefPcCharPuppet();
        return icur >= 0 && PcCharDef(icur).kind == K ? icur : -1;
    }
    static const char* Status(int idef)
    {
        if (idef < 0 || idef >= CPcCharDefs()) return "";
        if (IdefPcCharPuppetWanted() == idef) return "(loading)";
        if (Current() == idef)
        {
            // (the current one: its controls)
            static std::string s_str;
            s_str = std::string("(current) ") + PchzPcGuardMoves(IPcGuardOf(PcCharDef(idef)));
            return s_str.c_str();
        }
        return "";
    }
};
struct REGISTERGUARDCTRL
{
    REGISTERGUARDCTRL()
    {
        RegisterPcCharController(PCCHARK_Guard, {GUARDCTRL<PCCHARK_Guard>::Request, GUARDCTRL<PCCHARK_Guard>::Current, GUARDCTRL<PCCHARK_Guard>::Status});
        RegisterPcCharController(PCCHARK_Boss, {GUARDCTRL<PCCHARK_Boss>::Request, GUARDCTRL<PCCHARK_Boss>::Current, GUARDCTRL<PCCHARK_Boss>::Status});
    }
} s_registerguardctrl;
}  // namespace
