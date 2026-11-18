// viewer3.cpp: Sly 3's pad pan / zoom viewer (class 0x47, vtable 0x35E648, orig3 0x1E8C70..0x1E91D8), ported from the
// Sly 3 decomp (game_middle/actor_range_defaults_1E8C70.c, actor_input_update.c, func_001E8F38.c, hand/func_001E8E60.c,
// reference_constants/func_001E8CF0.c / func_001E8D20.c / func_001E8E08.c / func_001E8E40.c, auto/func_001E8D50.c;
// AUDIT4 MODES-11). v_apt (world 5: 139918 / 139973 / 140051) and h_hangar_b (18: 93396 / 93420), next to the code
// panels: a picture (a screen object) the player looks at.
//
// Activated by a script (SCPT 0x12 on a 0x47 object, 0x2271C8 -> 0x1E8E40, or the splice method 247 0x10CF80) with an
// actor: +0x190 = the actor, state 0 (0x1E8E60): the viewer takes the actor's player's pad (0x1F4B00), the pan / zoom
// reset, its screen object (EOPID 1255, +0x1BC) shown (vt slot 56). Its update (0x1E8D80, vt slot 18): Triangle or Circle
// (0x30 within 0.15 s) closes it (state 1: the pad back to the actor (0x1F4B00 with the viewer's player), the screen
// object hidden (slot 54)); the getter 1189 (EOPID 1256, +0x1C0) says whether Triangle closed it. Else 0x1E8F38: the
// left stick (squared) pans x / y within the ranges +0x1A4 / +0x1AC (EOPIDs 1252 / 1253, -1..1), Cross's pressure
// (the pad record's +0x111E / 64: 0, 0.5 to 0.75, then 1; natively held = 1) eases the zoom toward its level
// (GSmooth D_00324BD0 {2.5, 0, 0.7}), the screen object's parts 0x3E0 (scaled by zoom x +0x1B8 (EOPID 1254, 2.5)
// about the panned point), 0x3DF and 0x3DE (the rest, moved to it) set (0x262990).
// Its camera (slot 82 0x1E8E08: the base policy 0x169C90 (vtable 0x355D90) on it while it has the pad: the view saved
// and held where it was, its update none) (s3fix-modes3). Debug: SLY3_VIEWERPRINT=1.

#include "../clock.h"
#include "../cm.h"
#include "../cplcy.h"
#include "../joy.h"
#include "../lo.h"
#include "../objmethod.h"
#include "../po.h"
#include "../screenrt.h"
#include "../so.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "sly3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_VIEWERPRINT") != nullptr;
    return s_f;
}

struct VIEWER3
{
    SW* psw = nullptr;
    LO* plo = nullptr;
    bool fCharacter = false;        // +0x130 != -1
    LO* ploActor = nullptr;         // +0x190
    int state = -1;                 // +0x194
    float tState = 0.0f;            // +0x198
    float x = 0.0f, y = 0.0f;       // +0x19C / +0x1A0
    cmm::LM lmX{-1.0f, 1.0f};       // +0x1A4 (EOPID 1252)
    cmm::LM lmY{-1.0f, 1.0f};       // +0x1AC (1253)
    float zoom = 0.0f;              // +0x1B4
    float scale = 2.5f;             // +0x1B8 (1254)
    LO* pscreen = nullptr;          // +0x1BC (1255)
    int fTriangle = 0;              // +0x1C0 (the getter 1189)
    bool fCam = false;              // (its camera policy set)
};

// orig3 0x1E8E08 (vt slot 82): the base camera policy (0x169C90, vtable 0x355D90: 0x169E28 / 0x169E40 save and
// restore the FOV and near clip, 0x169E78 updates nothing): the camera holds while the viewer has the pad
struct CPVIEWER3 : CPLCY
{
    uint32_t VtOrig() const override { return 0x355D90; }
};
CPVIEWER3 s_cpviewer;
std::unordered_map<const LO*, VIEWER3> s_mpviewer;

VIEWER3* Pviewer(LO* plo)
{
    if (!plo || !g_psw || !plo->pcls || !FCidDerivedFrom(plo->Cid(), 0x47)) return nullptr;
    auto it = s_mpviewer.find(plo);
    if (it != s_mpviewer.end() && it->second.psw == g_psw) return &it->second;
    VIEWER3& v = s_mpviewer[plo];
    v = VIEWER3{};
    v.psw = g_psw;
    v.plo = plo;
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& a = o.aval[0];
        switch (o.eopid)
        {
        case 1252: v.lmX = {a.ag[0], a.ag[1]}; break;
        case 1253: v.lmY = {a.ag[0], a.ag[1]}; break;
        case 1254: v.scale = a.ag[0]; break;
        case 1255: v.pscreen = a.pbasic ? dynamic_cast<LO*>(a.pbasic) : (a.n == -1 || a.n == 0 ? nullptr : PloFindSwObjectByOid(g_psw, a.n)); break;
        default: break;
        }
    }
    if (FPrint())
        std::printf("viewer3: %d: x %.2f..%.2f y %.2f..%.2f scale %.2f screen %d\n", plo->oid, v.lmX.gMin, v.lmX.gMax, v.lmY.gMin, v.lmY.gMax,
                    v.scale, v.pscreen ? v.pscreen->oid : -1);
    return &v;
}

// orig3 0x1E8F38: the pan / zoom and the screen object's three parts
void UpdatePanZoom(VIEWER3& v)
{
    const float dt = g_clock.dt;
    const float sx = std::fabs(g_joyps2.x) * g_joyps2.x, sy = std::fabs(g_joyps2.y) * g_joyps2.y;
    v.x = std::clamp(v.x + sx * dt, v.lmX.gMin, v.lmX.gMax);
    v.y = std::clamp(v.y + sy * dt, v.lmY.gMin, v.lmY.gMax);
    const float level = (g_joyps2.grfbtn & 0x40) ? 1.0f : 0.0f;   // (Cross's pressure / 64: 0 / 0.5 / 1; held natively full)
    static const cmm::SMP s_smp{2.5f, 0.0f, 0.7f};   // D_00324BD0
    v.zoom = std::clamp(cmm::GSmooth(v.zoom, level, dt, &s_smp, nullptr), 0.0f, 1.0f);
    if (!v.pscreen) return;
    const float px = v.x * 320.0f + 320.0f, py = v.y * -240.0f + 240.0f;
    const float k = v.zoom * v.scale;
    SetScreenObjPartMatrix(v.pscreen, 0x3E0, {1.0f + k, 0.0f, 0.0f, 1.0f + k, -px * k, -py * k});
    const float rest = 1.0f - v.zoom;
    const std::array<float, 6> m{1.0f + rest, 0.0f, 0.0f, 1.0f + rest, px, py};
    SetScreenObjPartMatrix(v.pscreen, 0x3DF, m);
    SetScreenObjPartMatrix(v.pscreen, 0x3DE, m);
}

// orig3 0x1E8E60
void SetViewerState(VIEWER3& v, int st)
{
    if (v.state == st) return;
    if (v.state == 0)
    {
        // the pad back to the actor (0x1F4B00(SW, +0x130, +0x190)); the screen object's vt slot 54
        if (v.fCam) { RevokeCmPolicy(&g_cm, 0xF, 7, &s_cpviewer, v.plo, nullptr); v.fCam = false; }
        if (v.fCharacter && v.psw && v.ploActor && PloPoCurrent(v.psw) == v.plo) SetPoCurrent(v.psw, v.ploActor);
        v.fCharacter = false;
        if (v.pscreen) HideScreenObj(v.pscreen);
    }
    if (FPrint()) std::printf("viewer3: %d state %d -> %d (actor %d, triangle %d, t %.2f)\n", v.plo->oid, v.state, st, v.ploActor ? v.ploActor->oid : -1,
                              v.fTriangle, g_clock.t);
    v.state = st;
    v.tState = g_clock.t;
    if (st == 0)
    {
        // 0x1F4B00(SW, the actor's +0x130, viewer): its player's pad; the pan / zoom reset; the screen object's slot 56
        if (v.psw)
        {
            SetPoCurrent(v.psw, v.plo);
            v.fCharacter = true;
            if (!v.fCam) { SetCmPolicy(&g_cm, 7, &s_cpviewer, v.plo, nullptr); v.fCam = true; }
        }
        v.zoom = 0.0f;
        v.x = v.y = 0.0f;
        v.fTriangle = 0;
        if (v.pscreen) ShowScreenObj(v.pscreen, -1);
    }
}

// orig3 0x1E8D80 (vt slot 18)
void UpdateViewer(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    VIEWER3* pv = Pviewer(plo);
    if (!pv) return;
    VIEWER3& v = *pv;
    if (v.state != 0) return;
    // (0x1A7A80(pad, 0x30, 0.15): Triangle / Circle; natively this frame's press)
    const uint32_t grf = g_joyps2.grfbtnPressed & 0x30u;
    if (v.fCharacter && grf)
    {
        g_joyps2.grfbtnPressed &= ~grf;
        v.fTriangle = (grf & 0x10) ? 1 : 0;
        SetViewerState(v, 1);
        return;
    }
    UpdatePanZoom(v);
}

void ActivateViewer(LO* plo, LO* ploActor)
{
    VIEWER3* pv = Pviewer(plo);
    if (!pv) return;
    pv->ploActor = ploActor;   // 0x1E8E40: +0x190
    SetViewerState(*pv, 0);
}

// orig3 0x1E8D20 (vt slot 31): state 1
void ResetViewer(LO* plo, LO*)
{
    if (VIEWER3* pv = Pviewer(plo)) SetViewerState(*pv, 1);
}
// orig3 0x1E8D50 (vt slot 32): state -1
void ReleaseViewer(LO* plo)
{
    if (VIEWER3* pv = Pviewer(plo)) SetViewerState(*pv, -1);
}

struct INITVIEWER3
{
    INITVIEWER3()
    {
        RegisterLoUpdate(FN3_TAG | 0x1E8D80, UpdateViewer);
        RegisterObjectMethod3(0x10CF80, [](LO* plo, const std::vector<CRef>& a) {   // the method 247 -> 0x1E8E40
            ActivateViewer(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
        RegisterObjectGetter3(1256, [](LO* plo) { CRef r; VIEWER3* pv = Pviewer(plo); r.SetS32(pv ? pv->fTriangle : 0); return r; });
        RegisterObjectSetter3(1254, [](LO* plo, const CRef& r) { if (VIEWER3* pv = Pviewer(plo)) pv->scale = r.GAsFloat(); });
        RegisterObjectSetter3(1255, [](LO* plo, const CRef& r) { if (VIEWER3* pv = Pviewer(plo)) pv->pscreen = dynamic_cast<LO*>(r.m_pbasic); });
    }
} s_initviewer3;
NATIVEFN3(0x1E8D20, ResetViewer);
NATIVEFN3(0x1E8D50, ReleaseViewer);
}  // namespace

// SCPT 0x12 on a 0x47 object (game/scptgame3.cpp ActivateLo)
void ActivateViewer3(LO* plo, LO* ploActor) { ActivateViewer(plo, ploActor); }

// (update.cpp, the objects' post-loads) the record made and the object updated (the init 0x1E8C70 sets +0x3C bit 2)
void PostLoadViewer3(LO* plo)
{
    if (!plo || !plo->pcls || !g_psw || !FCidDerivedFrom(plo->Cid(), 0x47)) return;
    Pviewer(plo);
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
}
