// The camera in the native game (see cmworld.h).
#include "debugphys.h"
#include "turret.h"
#include "vehiclehooks.h"
#include "cmworld.h"
bool FSettingFlag(const char* name);   // (savedata.h; its header clashes with this file's LM)
#include "animfind.h"
#include "pcinput.h"
#include "actioninput.h"

#include "actor.h"
#include "activate.h"
#include "so.h"
#include "po.h"

#include <execinfo.h>

#include "alo.h"
#include "asega.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "cplcy.h"
#include "crv.h"
#include "disc.h"
#include "joy.h"
#include "lo.h"
#include "sw.h"
#include "update.h"
#include "task.h"
#include "worldaccess.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

using namespace cmm;

namespace
{
SW* s_psw = nullptr;

float TNative() { return g_clock.t; }
float DtNative() { return g_clock.dt; }
float DtRealNative() { return g_clock.dtReal; }

// 0x1A6318 (vt slot 31): an ALO's first transform record's world matrix; a LOX's (0x231440 +0xA0: its matrix in
// its reference record's frame).
glm::mat4 Mat4Lo(LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FIsLoDerivedFrom(plo, 0x30))
    {
        // (an SO's slot 31 is 0x1F3FA0: its object record's (+0x60), which moves with it; the root's does not)
        if (const XF* pxf = PxfObj(static_cast<ALO*>(plo))) return pxf->matWorld;
        return MatWorldOfAlo(static_cast<ALO*>(plo));
    }
    if (FIsLoDerivedFrom(plo, 3))
    {
        const LOX* plox = static_cast<const LOX*>(plo);
        return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    }
    return glm::mat4(1.0f);
}
glm::vec3 PosLoNative(LO* plo) { return glm::vec3(Mat4Lo(plo)[3]); }
glm::mat3 MatLoNative(LO* plo) { return glm::mat3(Mat4Lo(plo)); }
bool FIsLoDerivedFromNative(LO* plo, int cid) { return FIsLoDerivedFrom(plo, cid); }

// The focus's camera definition (vt slot 50): actor.cpp's GetLoCpdefi (0x1F58F0 / 0x141640 / 0x18A428).

const CMJOY* PjoyNative()
{
    static CMJOY s_joy;
    // the game's JOY (0x2DFC00: its sticks after the dead zone, 0x187768): a centered stick's raw 0x80 is not a
    // deflection (the raw one turned the follow camera into manual mode every frame)
    // (0x14D478: G_GRFJOYT & 1 ? the first pad : the second, which is at rest)
    const bool fPad = (g_grfjoyt & 1) != 0;
    const JOYPS2& j = JoyFor(1);
    s_joy.x = j.x;
    s_joy.y = j.y;
    s_joy.uDeflect = j.uDeflect;
    s_joy.x2 = j.x2;
    s_joy.y2 = j.y2;
    s_joy.uDeflect2 = j.uDeflect2;
    s_joy.grfbtn = fPad ? g_joy.grfbtn : 0;
    s_joy.grfbtnPressed = fPad ? g_joy.grfbtnPressed : 0;
    s_joy.dradMousePan = s_joy.dradMouseTilt = 0.0f;
    s_joy.dradAimRight = s_joy.dradAimUp = s_joy.cwheelZoom = 0.0f;
    if (fPad)
    {
        MouseOrbitDelta(&s_joy.dradMousePan, &s_joy.dradMouseTilt);
        MouseAimDelta(MOUSEAIM_Binocucom, &s_joy.dradAimRight, &s_joy.dradAimUp);
        if (const int zoom = sly2actions::ZoomDirection()) s_joy.y2 = float(zoom);
        s_joy.cwheelZoom = MouseWheelZoom();
    }
    return &s_joy;
}

// The player (0x1C4A48 of the world: po.h)
LO* PloPlayerNative() { return s_psw ? PloPoCurrent(s_psw) : nullptr; }


// ---- the zones' cameras (0x220D40 module; ZONE cid 0x0F)
glm::vec3 PosLoInFrame(LO* plo, const glm::vec3& pos)  // 0x1A6F00(null, plo, pos)
{
    return glm::vec3(glm::inverse(Mat4Lo(plo)) * glm::vec4(pos, 1.0f));
}

struct ZONESTATE
{
    std::unique_ptr<ZONECM> pzcm;
    int cppBase = 0;        // +0x128 (the stack priority - 2)
    int k = 0;              // +0x134 (the camera on)
    float t = 0.0f;         // +0x138
    int nForced = -1;       // +0x13C (-1: by the player's position)
    int fAlways = 0;        // +0x124
    LO* ploFocus = nullptr; // +0xE0 (the player it follows)
    bool fInWorld = true;   // (the zone's in-world state at the last pass: its removal 0x220F88 turns the camera off)
    LO* plo = nullptr;      // (the zone itself)
};
// an object's vt slot 3 (the base 0x1A61E0: its splice event n, then the game's event 0x1A (5) / 0x1B (6)); 0x221178
// calls it deferred (its third argument 1: the splice event queued, 0x133040), the task event at once. (Run at once, a
// zone's event 6 that removes its own tree re-entered the removal through the zone's slot 22: 12:2319 overflowed.)
void ZoneLoEvent(LO* plo, int n)
{
    HandleLoSpliceEvent(plo, static_cast<uint32_t>(n), true, 0, nullptr);
    BroadcastTaskEvent(g_psw, n == 5 ? 0x1A : 0x1B, plo);
}
std::map<LO*, ZONESTATE> s_mpzone;

bool FocusNative(LO* plo, CMFOCUS* pf)
{
    pf->fJt = FIsLoDerivedFrom(plo, 0x34);
    pf->mat = MatLoNative(plo);
    // the player's interaction sub-object (+0x4C0): +0x4F4 jtfock, +0x500 its focus point, +0x4C8 / +0x4CC its kind and
    // state, +0x528 its own camera zone; +0x10E4
    if (pf->fJt)
        if (const ACTOR* pa = PactorFromLo(plo))
        {
            pf->jtfock = pa->inter.n34;
            pf->posFocus = pa->inter.posFocus;
            pf->n4C8 = pa->n4C8;
            pf->n4CC = pa->nSub4CC;
            pf->n10E4 = pa->n10E4;
            pf->pvZoneCur = nullptr;
            if (pa->inter.ploZone)
            {
                auto it = s_mpzone.find(pa->inter.ploZone);
                if (it != s_mpzone.end()) pf->pvZoneCur = it->second.pzcm.get();
            }
        }
    return true;
}

// The zone's camera options (EOPID descriptors at 0x2AC880: +0x18 the field offset, +0x24 the getter of the
// current entry (0x221958) for the entry's fields, +0x1C the setter): the loader 0x220FD0's order: the first entry
// 0x221818(p, 0x30), the options, 0x221940; the init 0x220ED0's defaults first.
constexpr uint32_t VA_AEOPID = 0x2AC880;
ZONECME* PcmeSelect(ZONECM* pz, int id)  // orig 0x00221818
{
    for (ZONECME& e : pz->acme)
        if (e.cid == id) return &e;
    if (pz->acme.size() >= 4) return nullptr;
    ZONECME e = pz->acme.empty() ? ZonecmeDefault() : pz->acme.back();
    e.cid = id;
    pz->acme.push_back(e);
    return &pz->acme.back();
}

float GOpt(const OPTVAL& v, int i = 0) { return v.ag[i]; }

void SetCmeField(ZONECME* e, uint32_t ib, const OPTVAL& v)
{
    switch (ib)
    {
    case 0x04: e->dtPath = GOpt(v); break;
    case 0x10: case 0x20: case 0x30: e->acoef[(ib - 0x10) / 0x10] = glm::vec4(GOpt(v, 0), GOpt(v, 1), GOpt(v, 2), 0.0f); break;
    case 0x40: case 0x48: case 0x50: e->alm[(ib - 0x40) / 8] = LM{GOpt(v, 0), GOpt(v, 1)}; break;
    case 0x58: e->dxScreen = GOpt(v); break;
    case 0x5C: e->dyScreen = GOpt(v); break;
    case 0x60: e->uFocus = GOpt(v); break;
    case 0x64: e->uLead = GOpt(v); break;
    case 0x68: e->uSampleRate = GOpt(v); break;
    case 0x70: e->vecCoefY = glm::vec3(GOpt(v, 0), GOpt(v, 1), GOpt(v, 2)); break;
    case 0x80: e->rSpring = GOpt(v); break;
    case 0x84: e->rDamping = GOpt(v); break;
    case 0x88: e->lmTilt = LM{GOpt(v, 0), GOpt(v, 1)}; break;
    default: break;
    }
}

LO* PloFindOid(OID oid)
{
    if (!s_psw) return nullptr;
    for (const auto& plo : s_psw->aploAll)
        if (plo->oid == oid) return plo.get();
    return nullptr;
}

void SetZoneField(ZONESTATE& zs, uint32_t ib, const OPTVAL& v)
{
    ZONECM* pz = zs.pzcm.get();
    switch (ib)
    {
    case 0x120: pz->fPolar = v.n; break;
    case 0x124: zs.fAlways = v.n; break;
    case 0x128: zs.cppBase = v.n; break;
    case 0x130: pz->radPolarLimit = GOpt(v); break;
    case 0x13C: zs.nForced = v.n; break;
    case 0x164: pz->grf = v.n; break;
    case 0x168: pz->fReturn1 = v.n; break;
    case 0x16C: pz->fReturn2 = v.n; break;
    case 0x170: pz->dtReturn1 = GOpt(v); break;
    case 0x174: pz->uReturn1 = GOpt(v); break;
    case 0x178: pz->dtReturn2 = GOpt(v); break;
    case 0x17C: pz->uReturn2 = GOpt(v); break;
    case 0x180: pz->cfk = v.n; break;
    case 0x184: pz->fCutOnActivate = v.n; break;
    case 0x188: pz->radFOV = GOpt(v); break;
    case 0x18C: pz->fNoAvoid = v.n; break;
    // (the loader's object lookup 0x13E590 from the zone: an instance's alias counts, e.g. Ep7 Brains Over Brawn's
    // zone 32246 looks at 32248, Jean Bison's instance (guard 32250); by its own oid only it found nothing, so the
    // fight's camera followed Bentley without keeping Bison ahead (R53a))
    case 0x190: pz->ploLook = v.n == -1 ? nullptr : PloFindSwNearest(pz->pzone, v.n); break;
    case 0x1A0: for (int i = 0; i < 3; ++i) pz->alead[i] = GOpt(v, i); break;
    case 0x1B0: pz->lmLead = LM{GOpt(v, 0), GOpt(v, 1)}; break;
    default: break;
    }
}

void BuildZonecm(LO* pzone, ZONESTATE& zs, bool fPrint)
{
    ZONECM* pz = zs.pzcm.get();
    pz->pzone = pzone;
    // orig 0x00220ED0 (InitLo): the defaults
    pz->dtReturn2 = 2.0f;
    pz->uReturn2 = 1.0f;
    pz->dtReturn1 = 2.0f;
    pz->uReturn1 = 1.0f;
    pz->cfk = -1;
    pz->alead[0] = 0.0f; pz->alead[1] = 0.5f; pz->alead[2] = 0.0f;  // 0x2C41C0
    pz->lmLead = LM{0.0f, F32(0x7F7FFFFF)};                          // 0x2C41D0
    zs.nForced = -1;
    pz->grf = 7;
    pz->acme.clear();
    ZONECME* pcme = PcmeSelect(pz, 0x30);
    for (const OPT& opt : pzone->aopt)
    {
        if (opt.aval.empty()) continue;
        const uint32_t va = VA_AEOPID + static_cast<uint32_t>(opt.eopid) * 0x28;
        const uint32_t grf = g_disc.U32Elf(va + 4) & 0x2580, ib = g_disc.U32Elf(va + 0x18);
        const uint32_t pfnGet = g_disc.U32Elf(va + 0x24), pfnSet = g_disc.U32Elf(va + 0x1C);
        const OPTVAL& v = opt.aval[0];
        if (fPrint)
            std::printf("  zone %d option %d grf %X ib %X get %X set %X: n %d g %g %g %g\n", pzone->oid, opt.eopid, grf, ib,
                        pfnGet, pfnSet, v.n, v.ag[0], v.ag[1], v.ag[2]);
        if (grf & 0x100)
        {
            if (pfnSet == 0x221818) pcme = PcmeSelect(pz, v.n);                       // option 176
            else if (pfnSet == 0x221960 && pcme) pcme->vecCoefY = glm::vec3(GOpt(v), GOpt(v), 1.0f);  // option 190
            continue;
        }
        if (!(grf & 0x80)) continue;
        if (pfnGet == 0x221958)
        {
            if (pcme) SetCmeField(pcme, ib, v);
        }
        else if (pfnGet == 0) SetZoneField(zs, ib, v);
    }
    // the path: a PATH child (0x2317E0's message); without one the grf's bit 2 is cleared
    pz->pcrv = nullptr;
    if (FIsLoDerivedFrom(pzone, 0x0F))
        for (LO* pc : static_cast<LOXGEOM*>(pzone)->aploChild)
            if (const CRV* pcrv = PcrvFromLo(pc))
            {
                pz->pcrv = pc;
                pz->crvk = pcrv->crvk;
                pz->ccrvPoint = static_cast<int>(pcrv->apos.size());
                break;
            }
    if (!pz->pcrv) pz->grf &= ~2;
    if (fPrint && !pz->acme.empty())
    {
        const ZONECME& e = pz->acme[0];
        std::printf("ZCMP oid %d grf %d cpp %d forced %d always %d f120 %d ret %d %d %g %g %g %g cfk %d cut %d fov %g noav %d look %X "
                    "lead %g %g %g %g %g c %zu cid %d | %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g\n",
                    pzone->oid, pz->grf, zs.cppBase, zs.nForced, zs.fAlways, pz->fPolar, pz->fReturn1, pz->fReturn2, pz->dtReturn1,
                    pz->uReturn1, pz->dtReturn2, pz->uReturn2, pz->cfk, pz->fCutOnActivate, pz->radFOV, pz->fNoAvoid,
                    pz->ploLook ? 1u : 0u, pz->alead[0], pz->alead[1], pz->alead[2], pz->lmLead.gMin, pz->lmLead.gMax,
                    pz->acme.size(), e.cid, e.dtPath, e.acoef[0].x, e.acoef[0].y, e.acoef[0].z, e.acoef[1].x, e.acoef[1].y,
                    e.acoef[1].z, e.acoef[2].x, e.acoef[2].y, e.acoef[2].z, e.dxScreen, e.dyScreen, e.uFocus, e.uLead,
                    e.uSampleRate, e.vecCoefY.x, e.vecCoefY.y, e.vecCoefY.z, e.rSpring, e.rDamping, e.lmTilt.gMin, e.lmTilt.gMax);
    }
}

void UpdateZoneCamera(ZONESTATE& zs, int k)  // orig 0x00221178
{
    if (k == zs.k) return;
    // (then the zone's vt slot 3 with 5 (on) / 6 (off): its splice event and the game's 0x1A / 0x1B; audit C5)
    if (k == 1)
    {
        SetCmPolicy(&g_cm, zs.cppBase + 2, &g_cmpolicies.cpfollow, zs.ploFocus, zs.pzcm.get());
        if (zs.plo) ZoneLoEvent(zs.plo, 5);
    }
    else if (k == 0)
    {
        RevokeCmPolicy(&g_cm, 8, -1, nullptr, nullptr, zs.pzcm.get());
        if (zs.plo) ZoneLoEvent(zs.plo, 6);
    }
    zs.k = k;
    zs.t = g_clock.t;
    if (zs.pzcm) zs.pzcm->tArc = zs.t;  // (+0x138 is the zone's own field: the follow camera's return 1 ramps from it)
}

// orig 0x00118FB0 from the BSP's first node: in the zone when the point ends behind a node without a left child
// (a sentinel node's plane is (0, 0, 0, 1e8): always behind). (A collision query: the physics code may own it later.)
bool FPointInBsp(const LOXGEOM* p, const glm::vec3& posLocal);
bool FPointInZone(LO* pzone, const glm::vec3& posLocal)
{
    if (!FIsLoDerivedFrom(pzone, 0x0F)) return false;
    return FPointInBsp(static_cast<const LOXGEOM*>(pzone), posLocal);
}

bool FPointInBsp(const LOXGEOM* p, const glm::vec3& posLocal)
{
    if (p->bsp.abspn.empty()) return false;
    int i = 0;
    for (int c = 0; c < 100000; ++c)
    {
        const BSPN& n = p->bsp.abspn[i];
        float d = -1e8f;
        if (n.isurf >= 0 && n.isurf < static_cast<int>(p->geom.asurf.size()))
            d = glm::dot(p->geom.asurf[n.isurf].normal, posLocal) - p->geom.asurf[n.isurf].gDot;
        if (d < F32(0x38D1B717))
        {
            if (n.ibspNeg < 0 || n.ibspNeg >= static_cast<int>(p->bsp.abspn.size())) return true;
            i = n.ibspNeg;
        }
        else
        {
            if (n.ibspPos < 0 || n.ibspPos >= static_cast<int>(p->bsp.abspn.size())) return false;
            i = n.ibspPos;
        }
    }
    return false;
}

// ---- the camera volumes (class 5, 0x227380): 0x14E078 / 0x2273F0 / 0x227440 / the sort 0x14E000
struct VOLSTATE
{
    int f = -1;        // +0x110 (InitLo 0x227350: -1)
    float t = 0.0f;    // +0x114 (when it changed)
};
std::map<LO*, VOLSTATE> s_mpvol;

LO* PloCameraVolume(const glm::vec3& pos)
{
    if (!s_psw) return nullptr;
    struct E { LO* plo; VOLSTATE* ps; uint32_t n; };
    std::vector<E> a;
    for (LO* plo : AploOfClass(s_psw, 5))
    {
        if (!plo->fInWorld) continue;  // (0x15E9C8's grf 0x100: the flag bit 60, natively fInWorld)
        if (a.size() >= 32) break;
        VOLSTATE& vs = s_mpvol[plo];
        const int f = FPointInBsp(static_cast<LOXGEOM*>(plo), PosLoInFrame(plo, pos)) ? 1 : 0;
        if (f != vs.f)
        {
            vs.f = f;
            vs.t = g_clock.t;
        }
        a.push_back({plo, &vs, plo->U32Field(0, 0xE4)});
    }
    // orig 0x0014E000: the state, then +0xE4, descending; then the time of the change; then the address
    std::sort(a.begin(), a.end(), [](const E& x, const E& y) {
        if (x.ps->f != y.ps->f) return x.ps->f > y.ps->f;
        if (x.n != y.n) return x.n > y.n;
        if (x.ps->t != y.ps->t) return x.ps->t < y.ps->t;
        return x.plo < y.plo;
    });
    return !a.empty() && a[0].ps->f == 1 ? a[0].plo : nullptr;
}

// orig 0x00221298 (the zone's camera against the current player).
// A position-driven zone (+0x13C -1) follows the player's position except while a JT player is in the air (then it
// keeps its state) unless the zone's +0x124 is set or the camera cuts; forced zones (+0x13C 0 / 1, 0x221268) take
// their forced state.
void EvaluateZone(LO* pzone, ZONESTATE& zs)
{
    LO* po = PloPlayerNative();
    if (!po) return;
    if (po != zs.ploFocus)
    {
        UpdateZoneCamera(zs, 0);
        zs.ploFocus = po;
    }
    const int f = zs.nForced == -1 ? FPointInZone(pzone, PosLoInFrame(pzone, PosLoNative(po))) : zs.nForced;
    static const bool s_fDbg = std::getenv("SLY2_CMZONEEVAL") != nullptr;
    if (s_fDbg && f != zs.k)
    {
        const glm::vec3 q = PosLoNative(po), l = PosLoInFrame(pzone, q);
        std::printf("zone %d: %d -> %d (player %d at %.1f %.1f %.1f, local %.1f %.1f %.1f, forced %d always %d) t %.2f\n", pzone->oid,
                    zs.k, f, po->oid, q.x, q.y, q.z, l.x, l.y, l.z, zs.nForced, zs.fAlways, g_clock.t);
    }
    // (+0x124 0: a JT in the air (+0x2DC != -1) keeps the zone's state while it decides by position, unless the
    // camera cuts this frame (CM +0x300, 0x2DE240))
    if (zs.fAlways == 0 && zs.nForced == -1 && FIsLoDerivedFrom(po, 0x34) && !g_cm.fCut)
        if (ACTOR* pa = PactorFromLo(po); pa && pa->jump.state != -1) return;
    UpdateZoneCamera(zs, f);
}

// ---- the camera objects (cid 0x2C, module 0x12E600): the look-from-object camera (CM +0x4A8) at priority 10
struct CMOBJSTATE
{
    std::unique_ptr<CMOBJ> pcmobj;
    int fEnabled = 0;  // +0x124
};
std::map<LO*, CMOBJSTATE> s_mpcmobj;

void BuildCmobj(LO* plo, CMOBJSTATE& cs)
{
    CMOBJ* p = cs.pcmobj.get();
    p->plo = plo;
    for (const OPT& opt : plo->aopt)
    {
        if (opt.aval.empty()) continue;
        const OPTVAL& v = opt.aval[0];
        switch (opt.eopid)
        {
        case 1055: p->ploTarget = PloFindOid(v.n); break;  // +0xE0
        case 1059: p->fCut = v.n; break;                  // +0x120
        case 1054: p->radFOV = v.ag[0]; break;            // +0x130
        default: break;
        }
    }
}

// the zone is called by the world's update pass (in the update table, its entry's flag 2: update.cpp's loop)
bool FZoneUpdated(const LO* plo)
{
    return s_psw && plo->iupd >= 0 && plo->iupd < static_cast<int>(s_psw->aupd.size()) &&
           (s_psw->aupd[static_cast<size_t>(plo->iupd)].grf & GRFUPD_Update) && s_psw->aupd[static_cast<size_t>(plo->iupd)].plo == plo;
}

// orig 0x00221388 (ZONE vt slot 16): the LO update, then (+0x13C set) the camera's re-evaluation, deferred to the
// next 0x149C80. Natively the zones are re-evaluated at the camera's pass, before UpdateCm (the zones' update table
// entries are the world's; their splice update stays the default LO one).
void EvaluateZones()
{
    static const char* s_szZd = std::getenv("SLY2N_ZONEACT");
    static int s_c = 0;
    if (s_szZd && ++s_c == std::atoi(s_szZd))
        for (auto& [plo, zs] : s_mpzone)
            std::printf("zoneact %d inworld %d st %d iupd %d active %d forced %d k %d parent %d\n", plo->oid, plo->fInWorld, plo->st, plo->iupd,
                        FLoActive(plo), zs.nForced, zs.k, plo->paloParent ? plo->paloParent->oid : -1);
    for (auto& [plo, zs] : s_mpzone)
    {
        // a zone out of the world is not updated (0x221388 runs only in the world); its removal (0x220F88: the
        // deferred 0x221178(0)) turns its camera off once. (A forced zone outside the world, e.g. the paraglider's
        // 1524 among the actor's removed parts, still takes 0x221268's state.)
        const bool fWas = zs.fInWorld;
        zs.fInWorld = plo->fInWorld;
        if (!plo->fInWorld)
        {
            if (fWas) UpdateZoneCamera(zs, 0);
            continue;
        }
        // 0x221388 is the zone's update (vt slot 16): only a zone in the update table with an update (its entry's
        // flag 2) is re-evaluated. A zone in the world but not updated keeps its camera as it is: Team Up's hack-level
        // zone 27965 (forced on by its options, under the level group 17351) enters the world with the station's
        // scene, ~4 s before the hack starts; evaluated then, its top view (tilt 1.107, distance 1677) took the
        // camera over Bentley on the deck (N33), while the original keeps the player's camera until the hack.
        if (zs.nForced && FZoneUpdated(plo)) EvaluateZone(plo, zs);
    }
}

// D_002BF670: the static camera entry the interaction sub-objects' zones point at (+0x114, 0x194118). It is the
// ELF's data, so the edits of 0x192BD8 (a swing's offset) and 0x18A4E8 (a ledge's end offset) are shared by every
// player's zone and survive world loads, as in the original.
ZONECME* PcmeInteractShared()
{
    static ZONECME s_cme = [] {
        ZONECME e;
        auto f = [](uint32_t ib) { return std::bit_cast<float>(g_disc.U32Elf(0x2BF670 + ib)); };
        e.cid = static_cast<int>(g_disc.U32Elf(0x2BF670));
        e.dtPath = f(0x04);
        for (int k = 0; k < 3; ++k) e.acoef[k] = glm::vec4(f(0x10 + 0x10 * k), f(0x14 + 0x10 * k), f(0x18 + 0x10 * k), f(0x1C + 0x10 * k));
        for (int k = 0; k < 3; ++k) e.alm[k] = LM{f(0x40 + 8 * k), f(0x44 + 8 * k)};
        e.dxScreen = f(0x58);
        e.dyScreen = f(0x5C);
        e.uFocus = f(0x60);
        e.uLead = f(0x64);
        e.uSampleRate = f(0x68);
        e.vecCoefY = glm::vec3(f(0x70), f(0x74), f(0x78));
        e.rSpring = f(0x80);
        e.rDamping = f(0x84);
        e.lmTilt = LM{f(0x88), f(0x8C)};
        return e;
    }();
    return &s_cme;
}
}  // namespace

// orig: 0x0013E3C8(0xF, 0, oid, -1)'s zone (a class-0xF object made by code: no options, no BSP, no path): its camera
// with the InitLo 0x220ED0 defaults
void InitZoneCamera(LO* pzone)
{
    if (!pzone) return;
    ZONESTATE& zs = s_mpzone[pzone];
    zs = ZONESTATE{};
    zs.pzcm = std::make_unique<ZONECM>();
    zs.fInWorld = pzone->fInWorld;
    zs.plo = pzone;
    BuildZonecm(pzone, zs, false);
}

ZONECM* PzonecmFromLo(LO* pzone)
{
    auto it = pzone ? s_mpzone.find(pzone) : s_mpzone.end();
    return it == s_mpzone.end() ? nullptr : it->second.pzcm.get();
}

ZONECME* PcmeFirstOfZone(LO* pzone)
{
    ZONECM* pz = PzonecmFromLo(pzone);
    if (!pz) return nullptr;
    if (pz->pacmeShared) return pz->pacmeShared;
    return pz->acme.empty() ? nullptr : &pz->acme[0];
}

// orig: 0x00221268 (the zone's forced state +0x13C: -1 by the player's position, 0 off, 1 on; then 0x221298)
void SetZoneForced(LO* pzone, int n)
{
    auto it = pzone ? s_mpzone.find(pzone) : s_mpzone.end();
    if (it == s_mpzone.end()) return;
    ZONESTATE& zs = it->second;
    if (n == zs.nForced) return;
    if (std::getenv("SLY2N_ZONEPRINT"))
        std::printf("zone forced t %.3f: %d %d -> %d\n", g_clock.t, pzone->oid, zs.nForced, n);
    zs.nForced = n;
    EvaluateZone(pzone, zs);
}

// orig: 0x002213D8 (ZONE vt slot 22): out of the update table (0x1A6150, activate.cpp's caller), then the zone's camera
// off (0x221178(0)). Without it a forced zone that stopped being updated kept its camera on: after Team Up's first hack
// the hack level's zone 27965 (deactivated with the level, still in the world) left the camera in its top view over
// Bentley for good (N33)
void OnZoneUnregistered(LO* pzone)
{
    auto it = pzone ? s_mpzone.find(pzone) : s_mpzone.end();
    if (it == s_mpzone.end()) return;
    if (std::getenv("SLY2N_ZONEPRINT")) std::printf("zone unregistered t %.3f: %d (camera %d -> 0)\n", g_clock.t, pzone->oid, it->second.k);
    UpdateZoneCamera(it->second, 0);
}

// orig: 0x001CBF90 (vt slot 90 of class 0x3D (Rajan), script method 1438 via 0x110998): the two-actor camera on
// Murray (D_002F7900) and this object at priority 6, or revoked (flags 1, priority 6); the state kept at +0x1010.
// Natively the method was a stub, so the scripted framing in 12:2332 never appeared (audit 2026-11 C6)
void SetPairCamera(LO* plo, int n)
{
    static std::map<LO*, int> s_mpn;  // (+0x1010)
    int& cur = s_mpn[plo];
    if (cur == n) return;
    cur = n;
    if (n)
    {
        LO* ploMurray = nullptr;  // (gang.cpp PmurrayGlobal: the world's first Murray actor)
        if (g_psw)
            for (LO* p : g_psw->mpcidaplo[0x36])
                if (ACTOR* pa = PactorFromLo(p))
                {
                    ploMurray = pa->palo;
                    break;
                }
        SetCmPolicy(&g_cm, 6, &g_cmpolicies.cppair, ploMurray, plo);
    }
    else RevokeCmPolicy(&g_cm, 1, 6, nullptr, nullptr, nullptr);
}

// the zone's forced state (EOPID 214's getter; spliceglue.cpp registers it with the setter 0x221268)
int NZoneForced(LO* pzone)
{
    auto it = pzone ? s_mpzone.find(pzone) : s_mpzone.end();
    return it == s_mpzone.end() ? -1 : it->second.nForced;
}

// orig: 0x00194118 (the interaction sub-object's zone +0x68 back to its defaults)
void ResetZoneInteract(LO* pzone)
{
    auto it = pzone ? s_mpzone.find(pzone) : s_mpzone.end();
    if (it == s_mpzone.end()) return;
    ZONESTATE& zs = it->second;
    ZONECM* pz = zs.pzcm.get();
    // (+0x100 / +0xFC / +0x108 (its BSP) = 0: not read by a forced zone's camera)
    pz->pcrv = nullptr;                    // +0x10C
    pz->crvk = 0;
    pz->ccrvPoint = 0;
    pz->pacmeShared = PcmeInteractShared();  // +0x110 = 1, +0x114 = D_002BF670
    pz->grf = 5;                           // +0x164
    pz->fPolar = 0;                        // +0x120
    zs.fAlways = 0;                        // +0x124
    pz->fReturn2 = 0;                      // +0x16C
    pz->radPolarLimit = 0.0f;              // +0x130
    pz->fReturn1 = 1;                      // +0x168
    zs.cppBase = 3;                        // +0x128
    pz->sPath = 0.0f;                      // +0x140
    // (+0x12C, the arc, is not reset: a wall's stays until the next wall sets it again)
}

// EOPID 1055 (a plain field, +0xE0) set by a script: the camera object's target
void SetCmobjTarget(LO* plo, LO* ploTarget)
{
    auto it = s_mpcmobj.find(plo);
    if (it != s_mpcmobj.end() && it->second.pcmobj) it->second.pcmobj->ploTarget = ploTarget;
}

void CmOnWorldStart()
{
    EvaluateZones();
    UpdateCm(&g_cm);  // orig 0x20E890: 0x14D478 (not gated by the real clock)
    CutCm(&g_cm);     // 0x14D8C0
}

namespace
{
// orig 0x0012E738 / 0x0012E740 / 0x0012E750 (the camera object's vt slots 43..45): an ASEG's scalar channel drives
// its lens +0x128 (any kind), counted in +0x12C while in use (the intro's title card: ASEG 150285 on 150283)
CMOBJ* PcmobjOf(LO* plo)
{
    if (!plo || plo->Cid() != 0x2C) return nullptr;
    auto it = s_mpcmobj.find(plo);
    return it == s_mpcmobj.end() ? nullptr : it->second.pcmobj.get();
}
struct INITCMOBJPARAMS
{
    INITCMOBJPARAMS()
    {
        g_pfnPgAsegParam = [](LO* plo, int) -> float* {
            CMOBJ* p = PcmobjOf(plo);
            return p ? &p->sFocal : nullptr;
        };
        g_pfnAsegParamUse = [](LO* plo, int, bool fOn) {
            if (CMOBJ* p = PcmobjOf(plo)) p->cFocal += fOn ? 1 : -1;
        };
    }
} s_initcmobjparams;
}

// (debug: SLY2N_CMTRACE=file: per frame the camera's state, the fields of tools/camcmp.py's reference peeks)
static void TraceCmFrame()
{
    static FILE* s_pf = [] {
        const char* psz = std::getenv("SLY2N_CMTRACE");
        FILE* pf = psz ? std::fopen(psz, "w") : nullptr;
        if (pf)
            std::fprintf(pf, "frame,t,px,py,pz,dx,dy,dz,fov,ccpr,top,cpp,pan,tilt,dist,dyscr,manual,cpmode,dpan,dtilt,"
                             "bx,by,bz,vx,vy,vz,home,sadj,ax,ay,az,dax,day,daz,vax,vay,vaz,ex,ey,ez,opan,otilt,zone,grf\n");
        return pf;
    }();
    extern int g_frameJoy;
    // (SLY2N_OBJTRACE="oid,oid,...:file": those objects' centres per frame, every instance in the world)
    static FILE* s_pfObj = nullptr;
    static std::vector<OID> s_aoid;
    static bool s_fObjInit = false;
    if (!s_fObjInit)
    {
        s_fObjInit = true;
        if (const char* psz = std::getenv("SLY2N_OBJTRACE"))
        {
            std::string a(psz);
            const size_t c = a.find(':');
            if (c != std::string::npos)
            {
                s_pfObj = std::fopen(a.substr(c + 1).c_str(), "w");
                for (size_t i = 0; i < c;)
                {
                    const size_t j = std::min(a.find(',', i), c);
                    s_aoid.push_back(std::atoi(a.substr(i, j - i).c_str()));
                    i = j + 1;
                }
            }
        }
    }
    if (s_pfObj && s_psw)
    {
        for (const auto& plo : s_psw->aploAll)
        {
            if (std::find(s_aoid.begin(), s_aoid.end(), plo->oid) == s_aoid.end() && std::find(s_aoid.begin(), s_aoid.end(), plo->oidInstance) == s_aoid.end()) continue;
            if (!FIsLoDerivedFrom(plo.get(), 0x30)) continue;
            const glm::vec3 p = PosLoNative(plo.get());
            const ACTOR* pa = PactorFromLo(plo.get());
            const XF* pxf = PxfPos(static_cast<ALO*>(plo.get()));
            const glm::vec3 q = pxf ? glm::vec3(pxf->matWorld[3]) : glm::vec3(0.0f);
            // (then the object record's world x and z rows: its facing and its up)
            const XF* pxo = PxfObj(static_cast<ALO*>(plo.get()));
            const glm::mat4 mo = pxo ? pxo->matWorld : glm::mat4(1.0f);
            std::fprintf(s_pfObj, "%d,%.4f,%d,%d,%d,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", g_frameJoy, g_clock.t, plo->oid,
                         plo->oidInstance, plo->fInWorld ? 1 : 0, p.x, p.y, p.z, pa ? pa->jump.state : -99, q.x, q.y, q.z, mo[0].x, mo[0].y,
                         mo[0].z, mo[2].x, mo[2].y, mo[2].z);
        }
        std::fflush(s_pfObj);
    }
    if (!s_pf) return;
    const CM& cm = g_cm;
    const CPFOLLOW& f = g_cmpolicies.cpfollow;
    const CPDEFI& d = cm.cpdefiPrev;
    std::fprintf(s_pf, "%d,%.6f,%.3f,%.3f,%.3f,%.5f,%.5f,%.5f,%.5f,%d,%X,%d,%.5f,%.5f,%.3f,%.5f,%d,%d,%.5f,%.5f,"
                       "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                       "%.5f,%.5f,%d,%u\n",
                 g_frameJoy, g_clock.t, cm.pos.x, cm.pos.y, cm.pos.z, cm.mat[0].x, cm.mat[0].y, cm.mat[0].z, cm.radFOV,
                 cm.ccpr, cm.ccpr ? CmOffsetFromPcplcy(cm.acpr[0].pcplcy) : 0, cm.ccpr ? cm.acpr[0].cpp : 0, f.radPan,
                 f.radTilt, f.sDist, f.dyScreen, f.fManual, f.cpmode, f.dradPan, f.dradTilt, d.posBase.x, d.posBase.y,
                 d.posBase.z, d.vBase.x, d.vBase.y, d.vBase.z, d.radHome, d.sAdjust, d.posAdjust.x, d.posAdjust.y,
                 d.posAdjust.z, cm.dposAdjust.x, cm.dposAdjust.y, cm.dposAdjust.z, cm.vAdjust.x, cm.vAdjust.y,
                 cm.vAdjust.z, f.posEye.x, f.posEye.y, f.posEye.z, f.dradPanOffset, f.dradTiltOffset,
                 f.pzone && f.pzone->pzone ? f.pzone->pzone->oid : -1, d.grfcpd);
    std::fflush(s_pf);
}

void CmOnWorldLoad(SW* psw)
{
    static bool s_fStarted = false;
    if (!s_fStarted)
    {
        StartupCm();
        s_fStarted = true;
        // orig 0x226550's camera call: when the real clock runs
        RegisterSwPass(0x14D478, [](SW*) {
            EvaluateZones();
            if (0.0f < g_clock.dtReal) UpdateCm(&g_cm);
            TraceCmFrame();
            static const bool s_fPrint = std::getenv("SLY2_CMPRINT") != nullptr;
            if (s_fPrint)
            {
                const CM& cm = g_cm;
                std::printf("cm t %.3f pos %.1f %.1f %.1f dir %.3f %.3f %.3f fov %.3f stack", g_clock.t, cm.pos.x, cm.pos.y,
                            cm.pos.z, cm.mat[0].x, cm.mat[0].y, cm.mat[0].z, cm.radFOV);
                for (int i = 0; i < cm.ccpr; ++i)
                    std::printf(" %d:%X/%d", cm.acpr[i].cpp, CmOffsetFromPcplcy(cm.acpr[i].pcplcy),
                                cm.acpr[i].psoFocus ? cm.acpr[i].psoFocus->oid : -1);
                const CPFOLLOW& f = g_cmpolicies.cpfollow;
                std::printf(" | follow pan %.3f tilt %.3f dist %.1f zone %d crv %d home %.3f\n", f.radPan, f.radTilt, f.sDist,
                            f.pzone && f.pzone->pzone ? f.pzone->pzone->oid : -1, f.pzone && f.pzone->pcrv ? 1 : 0, cm.cpdefiPrev.radHome);
                if (std::getenv("SLY2_CMPRINTDEFI"))
                    std::printf("cm defi grf %u pos %.0f %.0f %.0f v %.0f %.0f %.0f home %.3f adj %.1f\n", cm.cpdefiPrev.grfcpd, cm.cpdefiPrev.posBase.x, cm.cpdefiPrev.posBase.y, cm.cpdefiPrev.posBase.z, cm.cpdefiPrev.vBase.x, cm.cpdefiPrev.vBase.y, cm.cpdefiPrev.vBase.z, cm.cpdefiPrev.radHome, cm.cpdefiPrev.sAdjust);
            }
        });
    }
    s_psw = psw;
    ResetActors();
    s_mpzone.clear();
    s_mpvol.clear();
    g_cmworld = CMWORLD{};
    g_cmworld.pfnT = TNative;
    g_cmworld.pfnDt = DtNative;
    g_cmworld.pfnDtReal = DtRealNative;
    g_cmworld.pfnPosLo = PosLoNative;
    g_cmworld.pfnMatLo = MatLoNative;
    g_cmworld.pfnFIsLoDerivedFrom = FIsLoDerivedFromNative;
    g_cmworld.pfnGetSoCpdefi = GetLoCpdefi;  // the focus's slot 50 (actor.h)
    // (+0x4F4, the interaction sub-object's +0x34: AdjustCmCpdefi 0x14E768 picks its springs by it; 1 = a ledge's
    // still hold (hfdk 3: the slow 1 / -10 spring), 2 = the sidestep in its own zone)
    g_cmworld.pfnJtfock = [](LO* plo) {
        const ACTOR* pa = PactorFromLo(plo);
        return pa ? pa->inter.n34 : 0;
    };
    g_cmworld.pfnPjoy = PjoyNative;
    g_cmworld.pfnPloPlayer = PloPlayerNative;
    g_cmworld.pfnCameraVolume = PloCameraVolume;
    g_cmworld.pfnFieldLo = [](LO* plo, uint32_t ib, uint32_t* pu) {
        // (the camera volume's +0xE0: its loader's U32; other fields from field options)
        if (ib == 0xE0 && FIsLoDerivedFrom(plo, 5)) *pu = static_cast<LOXGEOM*>(plo)->u;
        else if (ib == 0x4CC && FIsLoDerivedFrom(plo, 0x34)) *pu = PactorFromLo(plo)->nSub4CC;
        else *pu = plo->U32Field(0, ib, 0xFF);
        return true;
    };
    g_cmfollowworld = CMFOLLOWWORLD{};
    g_cmfollowworld.pfnFocus = FocusNative;
    g_cmfollowworld.pfnAttach = FTurretAttach;  // (the attach policy's mount: cid 0x45, turret.cpp)
    // the binocucom's "Invert Binocucom Controls" (0x150160: 0x2581C0 of the SVPATH at G_GAME +0x3B4C, the save's
    // flags.fReverseLookAround, the options' entries 0x51 / 0x52)
    g_cmfollowworld.pfnFInvertLook = [] { return FSettingFlag("fReverseLookAround"); };
    // the zones' paths (crv.h)
    g_cmfollowworld.pfnCrvPos = [](LO* plo, float s, glm::vec3* ppos, glm::vec3* pdir) {
        if (const CRV* p = PcrvFromLo(plo)) EvalCrvS(p, s, ppos, pdir);
    };
    g_cmfollowworld.pfnCrvNearest = [](LO* plo, const glm::vec3& pos, float s) {
        const CRV* p = PcrvFromLo(plo);
        float sOut = s;
        if (p) NearestCrvS(p, pos, s, nullptr, nullptr, nullptr, &sOut);
        return sOut;
    };
    g_cmfollowworld.pfnCrvNearestAll = [](LO* plo, const glm::vec3& pos, float s) {
        const CRV* p = PcrvFromLo(plo);
        float sOut = s;
        if (p) NearestCrvAll(p, pos, nullptr, nullptr, nullptr, &sOut);
        return sOut;
    };
    // orig 0x20E890: ResetCm 0x14D650 (0x20E068), InitCm 0x14D6D8; the loader's camera options (0x13DA18)
    ResetCm(&g_cm);
    InitCm(&g_cm);
    ApplyCmOptions(&g_cm, psw->aoptCm);
    const bool fPrint = std::getenv("SLY2_CMZONES") != nullptr;
    for (LO* plo : AploOfClass(psw, 0x0F))
    {
        ZONESTATE& zs = s_mpzone[plo];
        zs.pzcm = std::make_unique<ZONECM>();
        zs.fInWorld = plo->fInWorld;
        zs.plo = plo;
        BuildZonecm(plo, zs, fPrint);
    }
    s_mpcmobj.clear();
    for (LO* plo : AploOfClass(psw, 0x2C))
    {
        CMOBJSTATE& cs = s_mpcmobj[plo];
        cs.pcmobj = std::make_unique<CMOBJ>();
        BuildCmobj(plo, cs);
        // orig 0x0012E648 (its start, vt slot 15): +0x124 = 1, in the world (flag bit 60: LO::fInWorld) the camera pushed
        cs.fEnabled = 1;
        if (plo->fInWorld) SetCmPolicy(&g_cm, 10, &g_cmpolicies.cplookat, nullptr, cs.pcmobj.get());
    }
    if (fPrint)
        for (LO* plo : AploOfClass(psw, 0x2C))
            for (const OPT& opt : plo->aopt)
            {
                if (opt.aval.empty()) continue;
                const uint32_t va = VA_AEOPID + static_cast<uint32_t>(opt.eopid) * 0x28;
                std::printf("  cmobj %d option %d grf %X ib %X get %X set %X: n %d g %g\n", plo->oid, opt.eopid,
                            g_disc.U32Elf(va + 4) & 0x2580, g_disc.U32Elf(va + 0x18), g_disc.U32Elf(va + 0x24),
                            g_disc.U32Elf(va + 0x1C), opt.aval[0].n, opt.aval[0].ag[0]);
            }
    // The player's camera (the player's activation calls 0x1C3FF0).
    if (LO* ploPlayer = PloPlayerNative()) SetCmFocusObject(ploPlayer, true);
    // SLY2_CMOBJ=oid (development): that camera object enabled at the start (as the scripts do, e.g. Paris's
    // briefing view 27335)
    if (const char* psz = std::getenv("SLY2_CMOBJ"))
    {
        LO* ploPick = nullptr;  // (the oid's world-placed one: Paris has a clone in a character's frame too)
        for (auto& [plo, cs] : s_mpcmobj)
            if (plo->oid == std::atoi(psz) && (!ploPick || !static_cast<LOX*>(plo)->pxf)) ploPick = plo;
        if (ploPick) EnableCmObject(ploPick, true);
    }
}

// orig: 0x001C3FF0
void SetCmFocusObject(LO* plo, bool fOn)
{
    if (std::getenv("SLY2N_POPRINT")) std::printf("cm focus %d %s\n", plo ? plo->oid : -1, fOn ? "on" : "off");
    // (an object with a zone of its own (+0x16C) switches it: 0x221268; not in the object model yet)
    if (fOn) SetCmPolicy(&g_cm, 1, &g_cmpolicies.cpfollow, plo, nullptr);
    else RevokeCmPolicy(&g_cm, 15, 1, &g_cmpolicies.cpfollow, plo, nullptr);
    // (the players' slot 78 0x144480: then their vt slot 105, gadget.cpp; Bentley's Hover Pack on his back)
    void UpdateAbilityDisplayLo(LO* plo);
    UpdateAbilityDisplayLo(plo);
    SetTurretFocus(plo, fOn);  // (cid 0x45's slot 78: 0x225AE8)
    SetVehicleFocus(plo, fOn); // class0x4D, orig0x1D1940
    // (the hack ship's slot 78 0x17F8C8: its state 1 / 0, hack.cpp)
    void SetHackShipFocus(LO* plo, bool fOn);
    if (plo && plo->pcls && plo->pcls->PfnOrig(78) == 0x17F8C8) SetHackShipFocus(plo, fOn);
}

// orig: 0x0012E6A0 / 0x0012E6E8 (the camera object's enable / disable, vt slots of 0x1A5800 / 0x1A5848)
void EnableCmObject(LO* plo, bool fOn)
{
    if (std::getenv("SLY2N_POPRINT"))
    {
        const glm::vec3 pos = plo ? glm::vec3(Mat4Lo(plo)[3]) : glm::vec3(0.0f);
        std::printf("cm object %d %s at %.1f %.1f %.1f (%p clone %d inst %d grp %d k %d)\n", plo ? plo->oid : -1, fOn ? "on" : "off",
                    pos.x, pos.y, pos.z, static_cast<void*>(plo), plo ? plo->iclone : -1, plo ? plo->oidInstance : -1,
                    plo ? plo->igrp : -1, plo ? static_cast<int>(static_cast<int64_t>(plo->grf << 12) >> 60) : 0);
        if (std::getenv("SLY2N_POBT"))
        {
            void* apv[24];
            const int c = backtrace(apv, 24);
            backtrace_symbols_fd(apv, c, 1);
        }
    }
    auto it = s_mpcmobj.find(plo);
    if (it == s_mpcmobj.end() || !it->second.fEnabled) return;
    CMOBJ* p = it->second.pcmobj.get();
    if (fOn) SetCmPolicy(&g_cm, 10, &g_cmpolicies.cplookat, nullptr, p);
    else RevokeCmPolicy(&g_cm, 15, 10, &g_cmpolicies.cplookat, nullptr, p);
}

// (the original's vertical field of view: the renderer applies the picture's aspect mode, display.h)
CAMERA CameraFromCm(const CM& cm)
{
    CAMERA cam;
    cam.pos = cm.pos;
    cam.fMat = true;
    cam.mat = cm.mat;
    const glm::vec3 d = cm.mat[0];
    cam.yaw = glm::degrees(std::atan2(d.y, d.x));
    cam.pitch = glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
    cam.fovY = glm::degrees(cm.radFOV);
    cam.sNear = cm.sNearClip;
    cam.sFar = cm.sFarClip;
    cam.rMRDAdjust = cm.rMRDAdjust;
    cam.grfChannels = static_cast<uint32_t>(cm.nCur);
    ApplyDebugCharScaleCamera(&cam);   // (PC debug menu's character scale: debugphys.h)
    return cam;
}

void ApplyCmFog(const CM& cm, RENDEROPTS* popts)
{
    popts->sFogNear = cm.sNearFog;
    popts->sFogFar = cm.sFarFog;
    popts->uFogMax = cm.uFogMax;
    // FOGCOL (0x2DE18C, the world shaders' 0x3D register: 0x1EB960) is the camera's colour (+0x24C, SetCmRgbaFog 0x14F6F0)
    popts->rgbaFog = cm.rgbaFog;
    ApplyDebugCharScaleFog(popts);   // (PC debug menu's character scale: debugphys.h)
    popts->fFog = popts->sFogNear > 0 && popts->sFogFar > popts->sFogNear;
}

// orig: EOPID 210 (option 339 of class 0xF, a field at +0x190): the zone's look-at object, also set at run time (e.g.
// world 17's hypnosis camera zones 60278..60280 look at the Contessa: the boss job's s19 seto 339); registered in
// interactsplice.cpp
void SetZoneLook(LO* pzone, LO* plo)
{
    if (ZONECM* pz = PzonecmFromLo(pzone)) pz->ploLook = plo;
}

// EOPIDs 203 / 204 (options 333 / 334 of class 0xF, fields +0x170 / +0x174): the zone's first return time and
// blend, also set at run time (e.g. the Ep1 heist's sign zone 142397 after the harpoon's catch: 0 and 2)
void SetZoneReturn1(LO* pzone, int ib, float g)
{
    if (ZONECM* pz = PzonecmFromLo(pzone))
    {
        if (ib == 0x170) pz->dtReturn1 = g;
        else if (ib == 0x174) pz->uReturn1 = g;
    }
}
