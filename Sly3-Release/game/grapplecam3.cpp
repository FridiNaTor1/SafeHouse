// grapplecam3.cpp: Bentley's Grapple-Cam (class 0x56, vtable 0x36A2E0, orig3 0x285458..0x287740), its hook (class 0x7C,
// vtable 0x36A5C8, orig3 0x287780..0x288098) and its camera (the policy of kind 12, vtable 0x36A850, orig3 0x2883C0 /
// 0x288488 / 0x288508 / 0x288950), from the Sly 3 decomp (AUDIT4 MODES-12).
//
// One camera object per world (34 worlds), placed by Bentley's gadget (Sly 3's item 13): his throw lands (carry.cpp
// LandCarry -> orig3 0x277208 -> 0x1404D8) and an object of his proxy part +0x19BC (oid 634) is put where it landed,
// pushed 75 off every nearby surface (0x287890 / 0x287828) and attached there (0x287080). His next press with the camera
// at rest (its three states 0) enters it (0x286D70: state 1): his pad goes to the camera (0x1F4B00), he stands in his
// state 18, the camera's own view replaces the world camera. Ported:
//  - the states 0x286360 (-1 off, 0 placed, 1 flown, 2 going back: it waits for the grapple and the charge to end), the
//    grapple's states 0x286630 (0 off, 1 ready, 2 the hook flying (Cross; along the view's centre 100..5000 at 10000
//    a second, the rope (+0x210 path, +0x214 its mesh) paid out from +0x20C), 3 reeled in to the muzzle +0x208, 4
//    hooked (0.7 s, or the hooked object leaves), 5 the camera pulled along the rope to the hook (Cross while hooked;
//    PosSmooth {3000, 1000, 0.15}) and attached there), the charge's states 0x286B00 (0 off, 1 ready, 2 Square held
//    (the sound +0x2B0 looping), 3 after 1.5 s: the camera blows up (the projectile +0x2AC ended at it, broadcast 0x60,
//    state -1));
//  - the update 0x2859F0 (vt slot 18): in state 1 L3 the horn (sound 0x2A), Circle a noise (+0x218, the camera's
//    dialog lines +0x22C shuffled, broadcast 0x61: 0x287448), R1 with the webcam weapon (the progress powerUps
//    fAcquiredWebcamWeapon) the shots (0x2872B0: +0x1F8 a second from the pool +0x1F0 by the launcher +0x1F4), the exit
//    buttons (Bentley's gadget button | Triangle | 0x400) give his pad back; the grapple's 0x285C98 (and its 4 Hz scan:
//    a solid that is no SO within 50 of the camera blows it up), the charge's 0x286250, the HUD's 0x2876E8 / 0x287620
//    (the hit points eased into the meter 0x3CA of the screen object +0x2C8);
//  - the hits 0x286E38 (vt slot 78: hit points 40, the flash 2 s, none left: it blows up), slots 65 / 80 / 81 / 83
//    (0x286D90 / 0x286DD8 / 0x286FC0 / 0x2858D8), the reset 0x285648 (slot 31) / release 0x285748 (slot 32);
//  - the per-state sounds (method 1879, 0x287A28), the dialog lines (methods 1876 / 1877);
//  - the hook (0x7C): its flight is a swept segment each frame (natively: the hook's contacts 0x287B40 are the physics'
//    in the original; the same decisions: an actor / carried object / projectile / booty, a breakable, or water
//    bounces it back; anything else is the nearest hit and holds it 75 off the surface (0x288098 with 0x287890));
//  - the camera (slot 82 0x285930 -> 0x288370, kind 12): orbiting 50 behind the object at pan / tilt; the left stick
//    (0x1A7B38 index 2: Camera Options' grapple-cam kind 2, the scales 2.5 / 0.9) turns it within the tilt limits
//    +0x1D0 (EOPID 1860) at the eased rates +0x1D8 / +0x1DC; while the hook is out (its mode 1) within +0x1E0..+0x1EC of
//    where it was; while pulled (mode 2) back to those angles; FOV 1.5, near clip 49.
//  - the mount part (0x287000: its record of tag 0x27E, kind 0xD, kept at the attachment point (+0x1C0 in the record
//    +0x1B8) in its parent record's frame, after every turn of the object: the attachment 0x287080, the camera's
//    update 0x288508 (mode 0) and deactivate 0x288488); the saved matrix +0x3E0 (the reset 0x285648 keeps the first
//    record's local matrix when its kind is 0; the release 0x285748 puts it back and stamps the record).
// Not ported: the pad's per-player index (player 0's pad).
// Debug: SLY3_GRAPPLEPRINT=1 (states, placements, hits), SLY3_GRAPPLEWEAPON=1 (the weapon owned). Check: tools/grapple3.sh.

#include "../activate.h"
#include "../alo.h"
#include "../animfind.h"
#include "../brx.h"
#include "../ainoise.h"
#include "../carry.h"
#include "../clock.h"
#include "../cm.h"
#include "../cplcy.h"
#include "../gadget.h"
#include "../gang.h"
#include "../joy.h"
#include "../lo.h"
#include "../menu.h"
#include "../objmethod.h"
#include "../pe.h"
#include "../po.h"
#include "../projectile.h"
#include "../rumble.h"
#include "../savedata.h"
#include "../screenrt.h"
#include "../sm.h"
#include "../smrun.h"
#include "../so.h"
#include "../sound.h"
#include "../sw.h"
#include "../update.h"
#include "../vecmat.h"
#include "../vtnative.h"
#include "../xf.h"
#include "../xfphys.h"
#include "items3.h"
#include "sly3.h"
#include "task3.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

void SetLauncherAim(LO* plaunch, LO* ptarget, const glm::vec3& offset);   // launcher.cpp
void LaunchFrom(LO* plaunch, LO* po);                                     // launcher.cpp
CRef RefGetProgress(const std::vector<std::string>& apath);              // savedata.cpp
void EnterBentleyGrappleState3(LO* pbentley);                            // gang.cpp (0x13E510(+0x1AC, 18))
void ClearBentleyGrappleCam3(LO* pbentley);                              // gang.cpp (Bentley's slot 116 (0xD), 0x1403F8)
int GrappleProgressWeapon3();                                            // (below)
int OverlapKindAlo(ALO* pa, ALO* pb);                                    // bbmark.cpp (0x1F4FC8)

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY3_GRAPPLEPRINT") != nullptr;
    return s_f;
}

constexpr uint32_t BTN_R1 = 0x08, BTN_Circle = 0x20, BTN_Cross = 0x40, BTN_Square = 0x80, BTN_L3 = 0x200;

struct SNDSTATE3   // method 1879 (0x287A28): a sound started on the way into a grapple state
{
    int state = 0, id = -1, fLoop = 0;
    float volume = 1.0f, pitch = 0.0f;
};

struct GRAPPLE3
{
    SW* psw = nullptr;
    ALO* palo = nullptr;
    int state = -1;               // +0x190
    float tState = 0.0f;          // +0x194
    int grapple = -1;             // +0x198
    float tGrapple = 0.0f;        // +0x19C
    int charge = -1;              // +0x1A0
    float tCharge = 0.0f;         // +0x1A4
    float flash = 0.0f;           // +0x1A8 (the hit's flash)
    LO* pbentley = nullptr;       // +0x1AC (0x286D68)
    ALO* paloAttach = nullptr;    // +0x1B0 the object it is attached to (null: the world)
    XF* pxfAttach = nullptr;      // +0x1B4 / +0x1B8 its record
    glm::vec3 posAttach{0.0f};    // +0x1C0 the attachment point in that record
    cmm::LM lmTilt{-1.2f, 1.2f};  // +0x1D0 (EOPID 1860)
    float svPan = 2.5f;           // +0x1D8 (1861)
    float svTilt = 0.8f;          // +0x1DC (1862)
    cmm::LM lmPanHook{-0.2f, 0.2f};   // +0x1E0: the pan window while the hook is out
    cmm::LM lmTiltHook{-0.2f, 0.2f};  // +0x1E8
    LO* pool = nullptr;           // +0x1F0 (1864): the shots' proxy
    LO* launcher = nullptr;       // +0x1F4 (1866)
    float rateFire = 7.0f;        // +0x1F8 (1863)
    float tFire = 0.0f;           // +0x1FC
    LO* peFire = nullptr;         // +0x200 (1865)
    ALO* hook = nullptr;          // +0x204 (1869, class 0x7C)
    LO* muzzle = nullptr;         // +0x208 (1870)
    LO* anchor = nullptr;         // +0x20C (1871): the rope's start
    LO* rope = nullptr;           // +0x210 (1867): the path the rope simulates
    LO* ropeMesh = nullptr;       // +0x214 (1868)
    LO* noise = nullptr;          // +0x218 (1875): the noise template
    int iDialog = 0;              // +0x21C
    std::vector<int> aidDialog;   // +0x228 / +0x22C (at most 32)
    LO* explosion = nullptr;      // +0x2AC (1873)
    int sfxCharge = -1;           // +0x2B0 (1874)
    CSndRef sndCharge;            // +0x2B4
    uint32_t grfExit = 0;         // +0x2B8
    int fExit = 1;                // +0x2BC (1878)
    LO* hud = nullptr;            // +0x2C8 (1872)
    float hpShown = 0.0f;         // +0x2CC
    int idHudPart = -1;           // +0x2D0
    int fWeapon = 0;              // +0x2D4
    SMA* psma = nullptr;          // +0x2DC (the SM 0x27D, 0x2855B8)
    LO* barrier = nullptr;        // +0x2E4 (1880)
    std::vector<SNDSTATE3> asnd;  // +0x2E8.. (at most 12)
    CSndRef sndLoop;              // +0x3DC
    float tScan = 0.0f;           // +0x2E0
    // the hook (0x7C: +0x130 the hooked object, +0x134 its entry, +0x138 its record, +0x150 the point, +0x160 bounced)
    ALO* hookObj = nullptr;
    XF* hookXf = nullptr;
    glm::vec3 hookPoint{0.0f};
    int fHookBounced = 0;
    bool fHooked = false;         // (+0x130 set: natively the world itself can hold it)
    glm::vec3 hookVel{0.0f};
    // the camera (kind 12: +0x20 its mode, +0x2C / +0x30 pan / tilt, +0x34 / +0x38 the rates, +0x3C..+0x48 the
    // limits, +0x4C / +0x50 the angles kept for mode 2); +0x2C4 the tilt kept between visits
    bool fCam = false;
    int modeCam = -1;
    float pan = 0.0f, tilt = 0.0f, dpan = 0.0f, dtilt = 0.0f;
    float panMin = -6.2831855f, panMax = 6.2831855f, tiltMin = -1.2f, tiltMax = 1.2f;
    float panKeep = 0.0f, tiltKeep = 0.0f;
    float tiltSaved = 0.0f;
    glm::mat4 matSaved{1.0f};     // +0x3E0 (0x285648: the first record's local matrix)
    bool fSaved = false;
};
std::unordered_map<const LO*, GRAPPLE3> s_mpgrapple;

// orig3 0x287000: the mount part (the record of tag 0x27E, kind 0xD: 0x23DDA0) at the attachment point: +0x1C0 from the
// record +0x1B8's frame into the world, then into the mount's parent record's (0x28E910), as its local position (+0x30,
// w 1); the object's records' worlds again (vt slot 20: UpdateAloXfWorldsHierarchy)
void MountGrapple(GRAPPLE3& g)
{
    XF* pxfMount = nullptr;
    for (const auto& up : g.palo->axf)
        if (up && up->tag == 0x27E && up->kind == 0xD) { pxfMount = up.get(); break; }
    if (!pxfMount) return;
    UpdateAloXfWorldsHierarchy(g.palo);   // (the original's slot 35 leaves the worlds current: the turn just set)
    glm::vec4 pos(g.posAttach, 1.0f);
    if (g.pxfAttach) pos = g.pxfAttach->matWorld * pos;
    if (pxfMount->pxfRef) pos = glm::inverse(pxfMount->pxfRef->matWorld) * pos;
    if (static const bool s_f = std::getenv("SLY3_GRAPPLEPRINT") != nullptr; s_f && pxfMount->mat[3] != glm::vec4(glm::vec3(pos), 1.0f))
        std::printf("grapple3: %d mount at %.1f %.1f %.1f (was %.1f %.1f %.1f)\n", g.palo->oid, pos.x, pos.y, pos.z, pxfMount->mat[3].x,
                    pxfMount->mat[3].y, pxfMount->mat[3].z);
    pxfMount->mat[3] = glm::vec4(glm::vec3(pos), 1.0f);
    UpdateAloXfWorldsHierarchy(g.palo);
}

// an option's object: one of the camera's own sub-objects by its oid first (the template / its instances are not in
// the world when they load), else the nearest (0x17E1B8)
LO* PloOpt(LO* plo, const OPTVAL& v)
{
    if (v.pbasic) return dynamic_cast<LO*>(v.pbasic);
    if (v.n == -1 || v.n == 0) return nullptr;
    std::vector<LO*> astack{plo};
    while (!astack.empty())
    {
        LO* p = astack.back();
        astack.pop_back();
        for (LO* q : p->aploOwned)
        {
            if (!q) continue;
            if (q->oid == v.n) return q;
            astack.push_back(q);
        }
    }
    return PloFindSwNearest(plo, v.n);
}
glm::mat4 MatWorld(LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
    {
        XF* pxf = PxfObj(static_cast<ALO*>(plo));
        if (pxf) return pxf->matWorld;
    }
    glm::mat4 m(1.0f);
    m[3] = glm::vec4(PosLoWorld(plo), 1.0f);
    return m;
}
glm::vec3 Pos(LO* plo) { return plo ? PosLoWorld(plo) : glm::vec3(0.0f); }
glm::vec3 PosInXf(const XF* pxf, const glm::vec3& pos) { return pxf ? glm::vec3(glm::inverse(pxf->matWorld) * glm::vec4(pos, 1.0f)) : pos; }
glm::vec3 PosOfXf(const XF* pxf, const glm::vec3& pos) { return pxf ? glm::vec3(pxf->matWorld * glm::vec4(pos, 1.0f)) : pos; }

void SetGrappleState(GRAPPLE3& g, int st);
void SetHookState(GRAPPLE3& g, int st);
void SetChargeState(GRAPPLE3& g, int st);

GRAPPLE3* Pgrapple(LO* plo)
{
    if (!plo || !g_psw || !plo->pcls || !FIsLoDerivedFrom(plo, 0x56)) return nullptr;
    auto it = s_mpgrapple.find(plo);
    if (it != s_mpgrapple.end() && it->second.psw == g_psw) return &it->second;
    GRAPPLE3& g = s_mpgrapple[plo];
    g = GRAPPLE3{};
    g.psw = g_psw;
    g.palo = static_cast<ALO*>(plo);
    return &g;
}

// the loaded options and the post-load 0x2855B8 (after the object's load: its sub-objects exist then)
void ResolveGrapple(GRAPPLE3& g)
{
    LO* plo = g.palo;
    // the init 0x285458: the hit points 40 (0x1F43F8) unless loaded (EOPID 937)
    int nMax = 40;
    g.aidDialog.clear();
    g.asnd.clear();
    for (const OPT& o : plo->aopt)
    {
        if (o.aval.empty()) continue;
        const OPTVAL& v = o.aval[0];
        switch (o.eopid)
        {
        case 937: nMax = v.n; break;
        case 1860: g.lmTilt = {v.ag[0], v.ag[1]}; break;
        case 1861: g.svPan = v.ag[0]; break;
        case 1862: g.svTilt = v.ag[0]; break;
        case 1863: g.rateFire = v.ag[0]; break;
        case 1864: g.pool = PloOpt(plo, v); break;
        case 1865: g.peFire = PloOpt(plo, v); break;
        case 1866: g.launcher = PloOpt(plo, v); break;
        case 1867: g.rope = PloOpt(plo, v); break;
        case 1868: g.ropeMesh = PloOpt(plo, v); break;
        case 1869: g.hook = dynamic_cast<ALO*>(PloOpt(plo, v)); break;
        case 1870: g.muzzle = PloOpt(plo, v); break;
        case 1871: g.anchor = PloOpt(plo, v); break;
        case 1872: g.hud = PloOpt(plo, v); break;
        case 1873: g.explosion = PloOpt(plo, v); break;
        case 1874: g.sfxCharge = v.n; break;
        case 1875: g.noise = PloOpt(plo, v); break;
        case 1876: g.aidDialog.clear(); break;   // (method 0x1105A8 -> 0x285588)
        case 1877:   // (method 0x110620 -> 0x285590: a dialog line, at most 32)
            for (const OPTVAL& a : o.aval)
                if (g.aidDialog.size() < 32) g.aidDialog.push_back(a.n);
            break;
        case 1878: g.fExit = v.n; break;
        case 1879:   // (method 0x1106A8 -> 0x287A28: {state, sound, loop, volume, pitch}, at most 12, none with sound -1)
            if (o.aval.size() >= 5 && o.aval[1].n != -1 && g.asnd.size() < 12)
                g.asnd.push_back(SNDSTATE3{o.aval[0].n, o.aval[1].n, o.aval[2].n, o.aval[3].ag[0], o.aval[4].ag[0]});
            break;
        case 1880: g.barrier = PloOpt(plo, v); break;
        default: break;
        }
    }
    if (g.hook && !FIsLoDerivedFrom(g.hook, sly3::CID_ALO)) g.hook = nullptr;
    plo->SetField(0, 0x170, static_cast<uint32_t>(nMax));
    plo->SetField(0, 0x16C, static_cast<uint32_t>(nMax));
    g.hpShown = float(nMax);
    // the post-load 0x2855B8: no gravity, the SM 0x27D on the object (+0x2D8 / +0x2DC), the barrier +0x2E4 lets it
    // through (0x13BD00: natively the barriers' own list, barrier.cpp, by the object's oid)
    SetAloGravity(g.palo, glm::vec3(0.0f));
    if (SM* psm = dynamic_cast<SM*>(PloFindSwNearest(plo, 0x27D))) ReplaceSmaHandle(psm, g.palo, -1, 0, &g.psma);
    if (g.hook) SetAloGravity(g.hook, glm::vec3(0.0f));   // (the hook's post-load 0x287AC0)
    // the webcam weapon (0x285648: the progress "powerUps" "fAcquiredWebcamWeapon")
    g.fWeapon = GrappleProgressWeapon3();
    if (FPrint())
        std::printf("grapple3: %d: hook %d muzzle %d anchor %d rope %d/%d hud %d explosion %d pool %d launcher %d noise %d dialogs %zu sounds %zu weapon %d\n",
                    plo->oid, g.hook ? g.hook->oid : -1, g.muzzle ? g.muzzle->oid : -1, g.anchor ? g.anchor->oid : -1, g.rope ? g.rope->oid : -1,
                    g.ropeMesh ? g.ropeMesh->oid : -1, g.hud ? g.hud->oid : -1, g.explosion ? g.explosion->oid : -1, g.pool ? g.pool->oid : -1,
                    g.launcher ? g.launcher->oid : -1, g.noise ? g.noise->oid : -1, g.aidDialog.size(), g.asnd.size(), g.fWeapon);
}

// 0x1F4B00(SW, player, object): the player's pad to the object (natively player 0's: EnablePo, the old one off)
void GivePad(GRAPPLE3& g, LO* plo)
{
    if (!g.psw || !plo) return;
    if (NPoEnabled(plo) != 1 || PloPoCurrent(g.psw) != plo) EnablePo(g.psw, plo, 1);
}

PADCTL* Ppad(GRAPPLE3& g)
{
    PADCTL* ppad = PpadctlOfSo3(g.palo);
    return ppad && ppad->nControl == 1 && g.psw && PloPoCurrent(g.psw) == g.palo ? ppad : nullptr;
}
bool FPressed(GRAPPLE3& g, uint32_t grf)   // 0x1A7A80(pad, grf, 0.15): pressed within 0.15 s, taken
{
    PADCTL* ppad = Ppad(g);
    return ppad && GrfPadctlTakePressed(ppad, grf, 0.15f) != 0;
}
bool FHeld(GRAPPLE3& g, uint32_t grf)   // (the pad record's +0x18 & grf)
{
    PADCTL* ppad = Ppad(g);
    return ppad && (ppad->grfbtn & grf) != 0;
}

// ---------------------------------------------------------------- the rope (path options, rope.cpp)
void SetRopeObj(LO* rope, int eopid2, LO* plo)
{
    if (!rope) return;
    if (PFNOBJSETTER pfn = PfnObjectSetter(eopid2))
    {
        CRef r;
        if (plo) r.SetBasic(plo);
        else r.SetS32(-1);
        pfn(rope, r);
    }
}
void SetRopeF(LO* rope, int eopid2, float g)
{
    if (!rope) return;
    if (PFNOBJSETTER pfn = PfnObjectSetter(eopid2)) { CRef r; r.SetF32(g); pfn(rope, r); }
}
void SetRopeN(LO* rope, int eopid2, int n)
{
    if (!rope) return;
    if (PFNOBJSETTER pfn = PfnObjectSetter(eopid2)) { CRef r; r.SetS32(n); pfn(rope, r); }
}
void SetRopeForce(LO* rope, const glm::vec3& v)
{
    if (!rope) return;
    if (PFNOBJSETTER pfn = PfnObjectSetter(1211)) { CRef r; r.SetVector(v); pfn(rope, r); }
}
void ShowLo(LO* plo, bool f)
{
    if (!plo || !g_psw) return;
    if (f && !plo->fInWorld) AddLo(g_psw, plo);
    else if (!f && plo->fInWorld) RemoveLo(g_psw, plo);
}

// ---------------------------------------------------------------- the probes
// orig3 0x212FA0 with no object: the segment a-b through the world's solids, the first hit or b
glm::vec3 PosProbe(const glm::vec3& a, const glm::vec3& b, LO* ploSkip = nullptr, LO* ploSkip2 = nullptr, SOE** ppsoe = nullptr)
{
    std::vector<SOE*> asoe;
    for (SOE* psoe : AsolidWithTree(a, b))
    {
        if (psoe->palo && (psoe->palo == ploSkip || psoe->palo == ploSkip2)) continue;
        if (psoe->f50) continue;
        asoe.push_back(psoe);
    }
    LSG lsg;
    SOE* psoe = PsoeHitTestLineObjects(0, a, b, asoe, &lsg);
    if (ppsoe) *ppsoe = psoe;
    return psoe ? lsg.pos0 : b;
}
// the camera's screen point (x, y) at a depth (0x167838: ConvertCmScreenToWorld)
glm::vec3 PosScreen(float x, float y, float depth) { return ConvertCmScreenToWorld(&g_cm, glm::vec3(x, y, depth)); }

// orig3 0x287828: the margin of a solid's plane (75 off it)
float GPlaneMargin(CM*, const glm::vec3& pos, const glm::vec3&, const CMHIT& hit)
{
    return glm::dot(glm::vec3(hit.plane), pos) - hit.plane.w - 75.0f;
}
// orig3 0x287780: the solids the placement avoids (not the camera / its hook / its barrier; water always; no sensor
// entries (+0x18 bit 17), none of a removed object (bit 16) or a phantom's (+0xDC's +0x70 bit 2))
bool FPlacementSolid(const SOE* psoe, const GRAPPLE3* pg)
{
    if (!psoe || !psoe->palo) return true;
    LO* plo = psoe->palo;
    if (pg && (plo == pg->palo || plo == pg->hook || plo == pg->barrier)) return false;
    if (psoe->f48) return false;
    if (FIsLoDerivedFrom(plo, 0x65)) return true;
    if (psoe->f50) return false;
    return true;
}
// orig3 0x287890: the point pushed out to 75 from the solids within 100 (five passes; false when it moved more than
// 100 from pos or did not settle within 5)
bool FPlaceGrapple(const GRAPPLE3* pg, const glm::vec3& pos, glm::vec3* ppos)
{
    glm::vec3 cur = pos;
    for (int i = 0; i < 5; ++i)
    {
        const glm::vec3 start = cur;
        CMSOLIDS asolid;
        for (SOE* psoe : AsolidWithTree(start - glm::vec3(100.0f), start + glm::vec3(100.0f)))
            if (FPlacementSolid(psoe, pg)) asolid.push_back(psoe);
        const glm::vec3 res = SquishCmEye(&g_cm, asolid, GPlaneMargin, start, start, 75.0f, FLT_MAX);
        cur = res;
        if (100.0f < glm::length(res - pos)) return false;
        if (glm::length(res - start) < 5.0f)
        {
            *ppos = res;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- the attachment (0x287080)
void AttachGrapple(GRAPPLE3& g, ALO* paloAttach, XF* pxf, const glm::vec3& pos, const glm::vec3& xAxis)
{
    if (paloAttach && paloAttach->oid == 1) paloAttach = nullptr, pxf = nullptr;
    g.paloAttach = paloAttach;
    g.pxfAttach = pxf;
    g.posAttach = PosInXf(pxf, pos);
    SetAloParent(g.palo, paloAttach, pxf, 0x13);
    SetAloVelocity(g.palo, glm::vec3(0.0f));
    SetAloAngVelocity(g.palo, glm::vec3(0.0f));
    SetAloCnstrLin(g.palo, 1);
    SetAloCnstrAng(g.palo, 1);
    if (g.explosion && FIsLoDerivedFrom(g.explosion, sly3::CID_ALO)) SetAloParent(static_cast<ALO*>(g.explosion), g.palo, nullptr, 3);
    // level: x along the given axis without its z (default x), z up (0x11CBF0)
    glm::vec3 v(xAxis.x, xAxis.y, 0.0f);
    const float l = glm::length(v);
    v = l < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : v / l;
    const glm::vec3 z(0.0f, 0.0f, 1.0f);
    SetAloRotation(g.palo, glm::mat3(v, glm::cross(z, v), z));
    MountGrapple(g);   // 0x287000
    switch (g.state)
    {
    case -1: SetGrappleState(g, 0); break;
    case 1:
        SetHookState(g, 1);
        TriggerJoyRumbleRumk(3, 0.2f);
        JoltCm(&g_cm, 0);
        break;
    default: break;
    }
    if (FPrint())
        std::printf("grapple3: %d attached to %d at %.0f %.0f %.0f (t %.2f)\n", g.palo->oid, paloAttach ? paloAttach->oid : 1, pos.x, pos.y, pos.z, g_clock.t);
}

// ---------------------------------------------------------------- the hook (class 0x7C)
// orig3 0x287F48: back on the camera at the muzzle, still, its entries off
void RetractHook(GRAPPLE3& g)
{
    g.fHooked = false;
    g.hookObj = nullptr;
    g.hookXf = nullptr;
    g.fHookBounced = 0;
    g.hookVel = glm::vec3(0.0f);
    if (!g.hook) return;
    SetAloParent(g.hook, g.palo, nullptr, 0x13);
    const glm::mat4 m = MatWorld(g.muzzle ? g.muzzle : g.palo);
    SetAloRotation(g.hook, glm::mat3(m));
    SetAloPos(g.hook, glm::vec3(m[3]));
    SetAloVelocity(g.hook, glm::vec3(0.0f));
    SetAloAngVelocity(g.hook, glm::vec3(0.0f));
    SetAloCnstrLin(g.hook, 0);
    SetAloCnstrAng(g.hook, 1);
}
// orig3 0x288050: retracted and out of the world
void StowHook(GRAPPLE3& g)
{
    RetractHook(g);
    ShowLo(g.hook, false);
}
// orig3 0x287E70: at the muzzle in the world, free, flying at v
void LaunchHook(GRAPPLE3& g, const glm::vec3& v)
{
    g.fHooked = false;
    g.hookObj = nullptr;
    g.hookXf = nullptr;
    g.fHookBounced = 0;
    if (!g.hook) return;
    ShowLo(g.hook, true);
    SetAloParent(g.hook, nullptr, nullptr, 0);
    const glm::mat4 m = MatWorld(g.muzzle ? g.muzzle : g.palo);
    SetAloRotation(g.hook, glm::mat3(m));
    SetAloPos(g.hook, glm::vec3(m[3]));
    SetAloCnstrLin(g.hook, 0);
    SetAloCnstrAng(g.hook, 1);
    SetAloVelocity(g.hook, glm::vec3(0.0f));
    g.hookVel = v;
}
// orig3 0x287B40's choices for what the hook touched: the owner and its parents of one of the classes 0x32 (the
// players / guards), 0x71 (carried), 0x59 (projectiles), 0x74 (booty) or a breakable that takes the hit (0x6F,
// 0x1E4FC8) bounce it; water (0x65) too (natively its surface: the probe's hit)
bool FHookBounces(ALO* palo)
{
    for (ALO* p = palo; p; p = p->paloParent)
    {
        if (FIsLoDerivedFrom(p, 0x32) || FIsLoDerivedFrom(p, 0x71) || FIsLoDerivedFrom(p, 0x59) || FIsLoDerivedFrom(p, 0x74) ||
            FIsLoDerivedFrom(p, 0x6F))
            return true;
    }
    return palo && FIsLoDerivedFrom(palo, 0x65);
}
// the flight: the hook moved along its velocity, the segment tested; a hit holds it (0x288098) or bounces it (0x287AF8)
void UpdateHookFlight(GRAPPLE3& g)
{
    if (!g.hook || g.fHooked || g.fHookBounced || g.grapple != 2) return;
    const glm::vec3 a = Pos(g.hook);
    const glm::vec3 b = a + g.hookVel * g_clock.dt;
    SOE* psoe = nullptr;
    const glm::vec3 hit = PosProbe(a, b, g.palo, g.hook, &psoe);
    if (!psoe)
    {
        SetAloPos(g.hook, b);
        return;
    }
    ALO* powner = psoe->palo;
    if (FHookBounces(powner))
    {
        SetAloPos(g.hook, hit);
        g.fHookBounced = 1;   // 0x287AF8: +0x160 = 1
        if (FPrint()) std::printf("grapple3: hook bounced off %d (cid 0x%X)\n", powner ? powner->oid : -1, powner ? unsigned(powner->Cid()) : 0u);
        return;
    }
    // 0x288098: the point 75 off the surface (0x287890), the hook held by the object's record there
    glm::vec3 place;
    if (!FPlaceGrapple(&g, hit, &place))
    {
        SetAloPos(g.hook, hit);
        g.fHookBounced = 1;   // (0x287F48: retracted; the grapple's state 2 then reels in)
        if (FPrint()) std::printf("grapple3: hook at %d found no place\n", powner ? powner->oid : -1);
        return;
    }
    XF* pxf = powner ? PxfObj(powner) : nullptr;
    g.fHooked = true;
    g.hookObj = powner && powner->oid != 1 ? powner : nullptr;
    g.hookXf = g.hookObj ? pxf : nullptr;
    g.hookPoint = PosInXf(g.hookXf, place);
    SetAloPos(g.hook, hit);
    const glm::vec3 d = hit - place;
    if (1.0f < glm::dot(d, d))
    {
        const glm::vec3 x = glm::normalize(d);
        const glm::vec3 up = std::fabs(x.z) < 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 y = glm::normalize(glm::cross(up, x));
        SetAloRotation(g.hook, glm::mat3(x, y, glm::cross(x, y)));
    }
    SetAloParent(g.hook, g.hookObj, g.hookXf, 0x13);
    g.hookVel = glm::vec3(0.0f);
    if (FPrint())
        std::printf("grapple3: hook holds %d at %.0f %.0f %.0f (placed %.0f %.0f %.0f, t %.2f)\n", powner ? powner->oid : -1, hit.x, hit.y, hit.z, place.x,
                    place.y, place.z, g_clock.t);
}
// the hooked object gone (0x285C98's test: its entry off, out of the world)
bool FHookLost(const GRAPPLE3& g)
{
    if (!g.hookObj) return false;
    return !g.hookObj->fInWorld;
}
glm::vec3 PosHookPoint(const GRAPPLE3& g) { return PosOfXf(g.hookXf, g.hookPoint); }

// ---------------------------------------------------------------- the sounds of the grapple's states (0x286630's end)
void StartStateSounds(GRAPPLE3& g)
{
    if (g.sndLoop) { g.sndLoop->Stop(0.0f); g.sndLoop = CSndRef(); }
    for (const SNDSTATE3& s : g.asnd)
    {
        if (s.state != g.grapple) continue;
        CSnd* psnd = PsndStartSound(s.id, s.volume, 0, g.palo);
        if (!psnd) continue;
        psnd->SetPitch(s.pitch);
        if (s.fLoop) g.sndLoop = CSndRef(psnd);
    }
}

// ---------------------------------------------------------------- the camera (kind 12)
void SetCamMode(GRAPPLE3& g, int mode)   // orig3 0x288950
{
    if (g.modeCam == mode) return;
    if (g.modeCam == 2) { g.pan = g.panKeep; g.tilt = g.tiltKeep; }
    g.modeCam = mode;
    switch (mode)
    {
    case 0:
        g.panMin = -6.2831855f;
        g.panMax = 6.2831855f;
        g.tiltMin = g.lmTilt.gMin;
        g.tiltMax = g.lmTilt.gMax;
        break;
    case 1:
        g.panKeep = g.pan;
        g.tiltKeep = g.tilt;
        g.panMin = g.pan + g.lmPanHook.gMin;
        g.panMax = g.pan + g.lmPanHook.gMax;
        g.tiltMin = cmm::GLimitLm(&g.lmTilt, g.tilt + g.lmTiltHook.gMin);
        g.tiltMax = cmm::GLimitLm(&g.lmTilt, g.tilt + g.lmTiltHook.gMax);
        break;
    default: break;
    }
}

struct CPGRAPPLE3 : CPLCY
{
    LO* pcam = nullptr;
    uint32_t VtOrig() const override { return 0x36A850; }
    void Update(CPDEFI*, const CMJOY*) override;
};
CPGRAPPLE3 s_cpgrapple;

// orig3 0x2883C0 (activate): pan / tilt from the object (the tilt kept from the last visit), mode 0, FOV 1.5, near 49
void ActivateGrappleCam(GRAPPLE3& g)
{
    cmm::DecomposeRotateMatrixPanTilt(glm::mat3(MatWorld(g.palo)), &g.pan, &g.tilt);
    g.tilt = g.tiltSaved;
    g.dpan = g.dtilt = 0.0f;
    g.modeCam = -1;
    SetCamMode(g, 0);
    s_cpgrapple.pcam = g.palo;
    SetCmPolicy(&g_cm, 7, &s_cpgrapple, g.palo, nullptr);
    SetCmFov(&g_cm, 1.5f);
    SetCmNearClip(&g_cm, 49.0f);
    CutCm(&g_cm);
    g.fCam = true;
}
// orig3 0x288488 (deactivate): the object turned to the pan, the tilt kept
void DeactivateGrappleCam(GRAPPLE3& g)
{
    RevokeCmPolicy(&g_cm, 0xF, 7, &s_cpgrapple, g.palo, nullptr);
    if (g.palo->fInWorld)
    {
        SetAloRotation(g.palo, cmm::BuildPanTiltMatrix(g.pan, 0.0f));
        MountGrapple(g);   // 0x287000
    }
    g.tiltSaved = g.tilt;
    CutCm(&g_cm);
    g.fCam = false;
}
void LookFrom(GRAPPLE3& g, const glm::mat3& m)
{
    const glm::vec3 pos = Pos(g.palo);
    SetCmLookAt(&g_cm, pos + m * glm::vec3(-50.0f, 0.0f, 0.0f), pos, 0.0f, 0.0f);
}
// orig3 0x288508
void CPGRAPPLE3::Update(CPDEFI*, const CMJOY*)
{
    GRAPPLE3* pg = Pgrapple(pcam);
    if (!pg || !pcam->fInWorld) return;
    GRAPPLE3& g = *pg;
    const float dt = g_clock.dt;
    if (g.modeCam == 2)
    {
        static const cmm::SMP s_smp{1.0f, 0.0f, 0.2f};   // RO_00329780
        g.pan = cmm::GSmooth(g.pan, g.panKeep, dt, &s_smp, nullptr);
        g.tilt = cmm::GSmooth(g.tilt, g.tiltKeep, dt, &s_smp, nullptr);
        LookFrom(g, cmm::BuildPanTiltMatrix(g.pan, g.tilt));
        return;
    }
    // 0x1A7B38(pad, 2): the left stick, squared, x2.5 / x0.9, reversed by the axis table and Camera Options' kind 2
    PADCTL* ppad = Ppad(g);
    const float lx = ppad ? ppad->x : 0.0f, ly = ppad ? ppad->y : 0.0f;
    float x = -std::fabs(lx) * lx * 2.5f;
    if (FCamReverse3(2, false)) x = -x;
    float y = std::fabs(ly) * ly * 0.9f;
    if (!FCamReverse3(2, true)) y = -y;
    x = g.dpan + cmm::GLimitAbs(x - g.dpan, dt * 10.0f);
    y = g.dtilt + cmm::GLimitAbs(y - g.dtilt, dt * 10.0f);
    const cmm::SMP smpPan{g.svPan, 0.0f, 0.2f}, smpTilt{g.svTilt, 0.0f, 0.2f};
    const float dtHalf = dt * 0.5f;
    float gMin = 0.0f, gMax = 0.0f;
    cmm::GSmooth(g.pan, g.panMin, dtHalf, &smpPan, &gMin);
    cmm::GSmooth(g.pan, g.panMax, dtHalf, &smpPan, &gMax);
    g.dpan = std::clamp(x, std::min(gMin, gMax), std::max(gMin, gMax));
    cmm::GSmooth(g.tilt, g.tiltMin, dtHalf, &smpTilt, &gMin);
    cmm::GSmooth(g.tilt, g.tiltMax, dtHalf, &smpTilt, &gMax);
    g.dtilt = std::clamp(y, std::min(gMin, gMax), std::max(gMin, gMax));
    g.pan += g.dpan * dt;
    g.tilt += g.dtilt * dt;
    if (6.2831855f <= g.panMax - g.panMin) g.pan = cmm::RadNormalize(g.pan);
    const glm::mat3 m = cmm::BuildPanTiltMatrix(g.pan, g.tilt);
    LookFrom(g, m);
    if (g.modeCam == 0)
    {
        SetAloRotation(g.palo, m);   // vt slot 35
        MountGrapple(g);             // 0x287000
    }
}

void SyncGrappleCam(GRAPPLE3& g)
{
    const bool fCur = g.psw && PloPoCurrent(g.psw) == g.palo && g.palo->fInWorld && g.state == 1;
    if (fCur && !g.fCam) ActivateGrappleCam(g);
    else if (!fCur && g.fCam) DeactivateGrappleCam(g);
}

// ---------------------------------------------------------------- the states
void Rumble(GRAPPLE3& g, int rumk, float dt)
{
    if (g.psw && PloPoCurrent(g.psw) == g.palo) TriggerJoyRumbleRumk(rumk, dt);
}

void SetSmaLabel(GRAPPLE3& g, OID oid, bool fSnap = false)
{
    if (!g.psma) return;
    if (fSnap) SnapSmaGoal(g.psma, oid);
    else SetSmaGoal(g.psma, oid);
}

// orig3 0x286630: the grapple
void SetHookState(GRAPPLE3& g, int st)
{
    if (g.grapple == st) return;
    if (g.sndLoop) { g.sndLoop->Stop(0.0f); g.sndLoop = CSndRef(); }
    if (g.grapple == 3 || g.grapple == 5) SetCamMode(g, 0);
    if (FPrint()) std::printf("grapple3: %d grapple %d -> %d (t %.2f)\n", g.palo->oid, g.grapple, st, g_clock.t);
    g.grapple = st;
    g.tGrapple = g_clock.t;
    switch (st)
    {
    case -1:
        ShowLo(g.rope, false);
        ShowLo(g.ropeMesh, false);
        StowHook(g);
        break;
    case 1:
        SetRopeObj(g.rope, 1208, nullptr);
        SetRopeObj(g.rope, 1209, nullptr);
        ShowLo(g.rope, false);
        ShowLo(g.ropeMesh, false);
        SetRopeN(g.rope, 1212, -1);
        StowHook(g);
        break;
    case 4: SetRopeN(g.rope, 1212, 0); break;
    case 2:
    {
        SetRopeObj(g.rope, 1208, g.anchor);
        SetRopeObj(g.rope, 1209, g.hook);
        SetRopeForce(g.rope, glm::vec3(0.0f, 0.0f, -500.0f));
        SetRopeF(g.rope, 1210, 50.0f);
        SetRopeN(g.rope, 1212, 2);
        // the aim: the view's centre from 100 to 5000 (0x212FA0), the hook at 10000 toward it
        const glm::vec3 goal = PosProbe(PosScreen(0.0f, 0.0f, 100.0f), PosScreen(0.0f, 0.0f, 5000.0f), g.palo, g.hook);
        const glm::vec3 posHook = Pos(g.muzzle ? g.muzzle : g.palo);
        glm::vec3 d = goal - posHook;
        const float l = glm::length(d);
        d = l < 0.0001f ? glm::vec3(glm::mat3(MatWorld(g.palo))[0]) : d / l;
        LaunchHook(g, d * 10000.0f);
        ShowLo(g.rope, true);
        ShowLo(g.ropeMesh, true);
        SetCamMode(g, 1);
        break;
    }
    case 3:
        SetRopeN(g.rope, 1212, 3);
        SetRopeForce(g.rope, glm::vec3(0.0f));
        RetractHook(g);
        // (0x287F48 retracts it onto the muzzle at once; the reel is the rope's: natively the hook stays at its end
        // until the update below pulls it in)
        break;
    case 5:
        SetAloParent(g.palo, nullptr, nullptr, 0x13);
        SetAloVelocity(g.palo, glm::vec3(0.0f));
        SetAloAngVelocity(g.palo, glm::vec3(0.0f));
        SetRopeN(g.rope, 1212, 3);
        SetRopeForce(g.rope, glm::vec3(0.0f));
        SetCamMode(g, 2);
        break;
    default: break;
    }
    StartStateSounds(g);
}

// orig3 0x287740: broadcast 0x60, off
void BlowUp(GRAPPLE3& g)
{
    Broadcast3(0x60, g.palo);
    SetGrappleState(g, -1);
}

// orig3 0x286B00: the charge
void SetChargeState(GRAPPLE3& g, int st)
{
    if (g.charge == st) return;
    const int old = g.charge;
    if (old == 2)
    {
        if (g.sndCharge) { g.sndCharge->Stop(0.1f); g.sndCharge = CSndRef(); }
        if (g.hud) ResetScreenObjPart(g.hud, 0x3E2);
    }
    if (FPrint()) std::printf("grapple3: %d charge %d -> %d (t %.2f)\n", g.palo->oid, old, st, g_clock.t);
    g.charge = st;
    g.tCharge = g_clock.t;
    if (old == 3)
    {
        ShowLo(g.explosion, false);
        BlowUp(g);
        if (g.charge != st) return;
    }
    switch (st)
    {
    case -1:
        if (g.sndCharge) { g.sndCharge->Stop(0.0f); g.sndCharge = CSndRef(); }
        if (g.hud) ResetScreenObjPart(g.hud, 0x3E2);
        break;
    case 2:
        if (CSnd* psnd = g.sfxCharge != -1 ? PsndStartSound(g.sfxCharge, 1.0f, 0, g.palo) : nullptr) g.sndCharge = CSndRef(psnd);
        if (g.hud) StartScreenObjPart(g.hud, 0x3E2);
        break;
    case 3:
        if (g.grapple) SetHookState(g, 1);
        if (g.explosion && FIsLoDerivedFrom(g.explosion, sly3::CID_ALO))
        {
            ALO* pe = static_cast<ALO*>(g.explosion);
            SetAloParent(pe, nullptr, nullptr, 0);
            const glm::mat4 m = MatWorld(g.palo);
            SetAloRotation(pe, glm::mat3(m));
            SetAloPos(pe, glm::vec3(m[3]));
            ShowLo(pe, true);
            if (FIsLoDerivedFrom(pe, 0x59)) EndProjectileNow(pe);   // 0x14CE20
        }
        Rumble(g, 4, 0.75f);
        JoltCm(&g_cm, 0);
        if (g.hud) { HideScreenObj(g.hud); g.idHudPart = -1; }
        if (FPrint()) std::printf("grapple3: %d blows up (t %.2f)\n", g.palo->oid, g_clock.t);
        break;
    default: break;
    }
}

void ShowHudMeter(GRAPPLE3& g)   // orig3 0x2875C0
{
    if (g.hud && NSoHealthMax3(g.palo) > 0) SetScreenObjFraction(g.hud, 0x3CA, g.hpShown / float(NSoHealthMax3(g.palo)));
}

// orig3 0x286360
void SetGrappleState(GRAPPLE3& g, int st)
{
    if (g.state == st) return;
    const int old = g.state;
    if (old == 1)
    {
        Broadcast3(0x63, g.palo);
        if (g.psma) SetSmaLabel(g, g.fWeapon ? 0xA8 : 0x92);
        if (g.hud) { HideScreenObj(g.hud); g.idHudPart = -1; }
        // the pad back to Bentley (0x1F4B00(SW, +0x130, +0x1AC))
        if (g.psw && g.pbentley && PloPoCurrent(g.psw) == g.palo) GivePad(g, g.pbentley);
    }
    if (FPrint()) std::printf("grapple3: %d state %d -> %d (t %.2f)\n", g.palo->oid, old, st, g_clock.t);
    g.state = st;
    g.tState = g_clock.t;
    switch (st)
    {
    case -1:
        if (g.pbentley) ClearBentleyGrappleCam3(g.pbentley);   // (vt slot 116 (0xD))
        SetAloParent(g.palo, nullptr, nullptr, 0x13);
        g.paloAttach = nullptr;
        g.pxfAttach = nullptr;
        SetHookState(g, -1);
        SetChargeState(g, -1);
        if (g.psma) SetSmaLabel(g, 0x7A, true);
        if (g.hud) SetScreenObjState(g.hud, 1);
        RemoveLo(g.psw, g.palo);   // (vt slot 9: back to its proxy's pool)
        break;
    case 0:
        SetHookState(g, 0);
        SetChargeState(g, 0);
        if (g.psma) SetSmaLabel(g, g.fWeapon ? 0xA8 : 0x92);
        break;
    case 2:
        if (g.charge == 2) SetChargeState(g, 1);
        if (g.grapple == 2 || g.grapple == 4) SetHookState(g, 3);
        break;
    case 1:
        SetHookState(g, 1);
        SetChargeState(g, 1);
        // Bentley's player's pad to the camera (0x1F4B00(SW, his +0x130, camera)); the exit buttons
        GivePad(g, g.palo);
        g.grfExit = 0x410;
        if (ACTOR* pa = PactorFromLo(g.pbentley)) g.grfExit |= GrfAbilityButton(pa, sly3::ITEM2_SLY3 + 13);
        if (g.hud)
        {
            ShowScreenObj(g.hud, -1);
            StartScreenObjPart(g.hud, 0x3CA);
            ShowHudMeter(g);
        }
        if (g.psma) SetSmaLabel(g, g.fWeapon ? 0xA9 : 0x90);
        Broadcast3(0x62, g.palo);
        if (g.pbentley) EnterBentleyGrappleState3(g.pbentley);   // 0x13E510(+0x1AC, 0x12)
        break;
    default: break;
    }
    SyncGrappleCam(g);
}

// orig3 0x287448: the next dialog line (shuffled when every one has played, not the last one first), a noise at the
// camera, broadcast 0x61
void MakeNoise(GRAPPLE3& g)
{
    const int c = int(g.aidDialog.size());
    if (c > 0)
    {
        const int first = g.aidDialog[0];
        if (!FDialogPlaying(first))
        {
            if (g.iDialog == c)
            {
                for (int i = 0; i < c; ++i) std::swap(g.aidDialog[size_t(i)], g.aidDialog[size_t(std::rand() % c)]);
                if (g.aidDialog[0] == first) std::swap(g.aidDialog[0], g.aidDialog[size_t(c - 1)]);
                g.iDialog = 0;
            }
            else
            {
                std::rotate(g.aidDialog.begin(), g.aidDialog.begin() + 1, g.aidDialog.end());
                ++g.iDialog;
            }
            PsndStartDialog(g.aidDialog[0]);
        }
    }
    if (NOISETMPL* ptmpl = g.noise ? PtmplNoise(g.noise) : nullptr) PnoiseNew(ptmpl, Pos(g.palo), g.palo);
    Broadcast3(0x61, g.palo);
    if (FPrint()) std::printf("grapple3: %d noise (dialog %d, t %.2f)\n", g.palo->oid, c ? g.aidDialog[0] : -1, g_clock.t);
}

// orig3 0x2872B0: a shot through the view's centre (jittered by 0.005), at most +0x1F8 a second
void Fire(GRAPPLE3& g)
{
    if (g_clock.t < g.tFire) return;
    LO* po = g.pool && g.psw ? PloAcquireProxyObject(g.psw, g.pool, false) : nullptr;
    if (po)
    {
        auto jitter = [] { return (float(std::rand()) / float(RAND_MAX)) * 0.01f - 0.005f; };
        const float x = jitter(), y = jitter();
        const glm::vec3 aim = PosProbe(PosScreen(0.0f, 0.0f, 75.0f), PosScreen(x, y, 50000.0f), g.palo, g.hook);
        if (g.launcher)
        {
            SetLauncherAim(g.launcher, nullptr, aim);
            LaunchFrom(g.launcher, po);
        }
        if (FIsLoDerivedFrom(po, 0x59)) SetProjectileLauncher(static_cast<ALO*>(po), g.palo);
        if (g.peFire) TriggerPe(g.peFire);   // 0x1FE728(+0x200, 1)
        Rumble(g, 10, 0.05f);
        JoltCm(&g_cm, 0);
        if (FPrint()) std::printf("grapple3: %d shot %d toward %.0f %.0f %.0f (t %.2f)\n", g.palo->oid, po->oid, aim.x, aim.y, aim.z, g_clock.t);
    }
    g.tFire = g_clock.t + 1.0f / std::max(g.rateFire, 0.001f);
}

// orig3 0x285BC8 + 0x1CD098: a solid that is no SO (nor projectile) within 50 of the camera (water always)
bool FScanHit(GRAPPLE3& g)
{
    const glm::vec3 pos = Pos(g.palo);
    for (SOE* psoe : AsolidWithTree(pos - glm::vec3(50.0f), pos + glm::vec3(50.0f)))
    {
        LO* plo = psoe->palo;
        if (!plo || plo == g.palo || plo == g.hook || plo == g.muzzle || plo == g.barrier) continue;
        if (psoe->f48) continue;
        const bool fWater = FIsLoDerivedFrom(plo, 0x65);
        if (!fWater && (psoe->f50 || FIsLoDerivedFrom(plo, 0x31) || FIsLoDerivedFrom(plo, 0x59))) continue;
        std::vector<glm::vec3> apos;
        std::vector<glm::vec4> aplane;
        std::vector<int> akind;
        std::vector<const void*> apv;
        if (CsbiSphereEntry(pos, psoe, nullptr, 1, 50.0f, &apos, &aplane, &akind, &apv) > 0)
        {
            if (FPrint()) std::printf("grapple3: %d touches %d (cid 0x%X)\n", g.palo->oid, plo->oid, unsigned(plo->Cid()));
            return true;
        }
    }
    return false;
}

// orig3 0x285C98: the grapple's transitions and motion, then the scan
void UpdateHook(GRAPPLE3& g)
{
    UpdateHookFlight(g);
    int s;
    do
    {
        s = g.grapple;
        const float dt = g_clock.t - g.tGrapple;
        switch (s)
        {
        case 1:
            if (FPressed(g, BTN_Cross)) SetHookState(g, 2);
            break;
        case 2:
            if (g.fHooked)
                SetHookState(g, glm::length(PosHookPoint(g) - Pos(g.palo)) < 200.0f ? 3 : 4);
            else if (g.fHookBounced || FPressed(g, BTN_Cross) || 0.4f < dt)
                SetHookState(g, 3);
            break;
        case 4:
            if (g.tGrapple < g_clock.t - 0.7f || FHookLost(g)) SetHookState(g, 3);
            else if (FPressed(g, BTN_Cross)) SetHookState(g, 5);
            break;
        case 3:
            if (glm::length(Pos(g.muzzle ? g.muzzle : g.palo) - Pos(g.hook)) < 75.0f || !g.hook) SetHookState(g, 1);
            break;
        case 5:
            if (FHookLost(g)) SetChargeState(g, 3);
            break;
        default: break;
        }
    } while (g.grapple != s);
    switch (g.grapple)
    {
    case 3:
        if (g.hook)
        {
            static const cmm::SMP s_smp{10000.0f, 1000.0f, 0.15f};   // D_00329690
            SetAloPos(g.hook, cmm::PosSmooth(Pos(g.hook), Pos(g.muzzle ? g.muzzle : g.palo), g_clock.dt, &s_smp, nullptr));
        }
        break;
    case 5:
    {
        static const cmm::SMP s_smp{3000.0f, 1000.0f, 0.15f};   // D_003296A0
        const glm::vec3 goal = PosHookPoint(g);
        const glm::vec3 r = cmm::PosSmooth(Pos(g.palo), goal, g_clock.dt, &s_smp, nullptr);
        SetAloPos(g.palo, r);
        const glm::vec3 d = r - goal;
        if (glm::dot(d, d) < 0.0001f)
        {
            const glm::vec3 xAxis = glm::vec3(glm::mat3(MatWorld(g.palo))[0]);
            ALO* pobj = g.hookObj;
            XF* pxf = g.hookXf;
            AttachGrapple(g, pobj, pxf, Pos(g.hook), xAxis);   // (+0x1C0 the hook's point: the mount part's)
            SetHookState(g, 1);
        }
        break;
    }
    default: break;
    }
    if (g.grapple == -1 || g.grapple == 0 || g.grapple == 5) return;
    if (0.25f < g_clock.t - g.tScan)
    {
        g.tScan = g_clock.t;
        if (FScanHit(g)) SetChargeState(g, 3);
    }
}

// orig3 0x286250: the charge
void UpdateCharge(GRAPPLE3& g)
{
    int s;
    do
    {
        const float dt = g_clock.t - g.tCharge;
        s = g.charge;
        switch (s)
        {
        case 1:
            if (FHeld(g, BTN_Square)) SetChargeState(g, 2);
            break;
        case 2:
            if (!FHeld(g, BTN_Square)) SetChargeState(g, 1);
            else if (1.5f < dt) SetChargeState(g, 3);
            break;
        case 3:
            if (!g.hud || FScreenObjDone(g.hud) || 3.0f < dt) SetChargeState(g, 1);
            break;
        default: break;
        }
    } while (g.charge != s && g.state != -1);
}

// orig3 0x287620: the hit points eased into the meter (GSmooth D_00329750 {100, 0, 0.5}); the flash's end
void UpdateHud(GRAPPLE3& g)
{
    const float hp = float(NSoHealth3(g.palo));
    if (g.hpShown != hp)
    {
        static const cmm::SMP s_smp{100.0f, 0.0f, 0.5f};
        g.hpShown = cmm::GSmooth(g.hpShown, hp, g_clock.dt, &s_smp, nullptr);
        ShowHudMeter(g);
    }
    if (g.flash > 0.0f)
    {
        g.flash -= g_clock.dt;
        if (g.flash <= 0.0f) g.flash = 0.0f;   // (0x240DB0: the flash's end)
    }
}

// orig3 0x2859F0 (vt slot 18)
void UpdateGrapple(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1F36E0)) base(plo);   // 0x23C660
    GRAPPLE3* pg = Pgrapple(plo);
    if (!pg) return;
    GRAPPLE3& g = *pg;
    // (0x2858D8, vt slot 83: the pad taken away while flown: state 2)
    if (g.state == 1 && g.psw && PloPoCurrent(g.psw) != g.palo) SetGrappleState(g, 2);
    int s;
    do
    {
        s = g.state;
        switch (s)
        {
        case 2:
            if (g.grapple == 1) SetHookState(g, 0);
            if (g.charge == 1) SetChargeState(g, 0);
            if (g.grapple == 0 && g.charge == 0) SetGrappleState(g, 0);
            break;
        case 1:
            if (FPressed(g, BTN_L3))
            {
                PsndStartSound(0x2A, 1.0f, 0, g.palo);   // the horn
                JoltCm(&g_cm, 0);
            }
            if (FPressed(g, BTN_Circle)) MakeNoise(g);
            if (g.grapple == 1)
            {
                if (g.fWeapon && FHeld(g, BTN_R1)) Fire(g);
                if (g.fExit && g.charge == 1 && FPressed(g, g.grfExit))
                {
                    if (g.psw && g.pbentley) GivePad(g, g.pbentley);   // (0x1F4B00: then slot 83: state 2)
                    SetGrappleState(g, 2);
                }
            }
            break;
        default: break;
        }
    } while (g.state != s);
    switch (g.state)
    {
    case 0: case 1: case 2:
        UpdateHook(g);
        if (g.state == -1) break;
        UpdateCharge(g);
        if (g.state == -1) break;
        UpdateHud(g);
        break;
    default: break;
    }
    SyncGrappleCam(g);
}

// orig3 0x285648 (vt slot 31): the hit points full and shown, state 0 (the attachment none)
void ResetGrapple(LO* plo, LO*)
{
    GRAPPLE3* pg = Pgrapple(plo);
    if (!pg) return;
    SetSoHealth3(plo, NSoHealthMax3(plo));
    pg->hpShown = float(NSoHealth3(plo));
    pg->flash = 0.0f;
    pg->idHudPart = -1;
    pg->fWeapon = GrappleProgressWeapon3();
    // +0x3E0: the first record's local matrix, when its kind is 0
    if (ALO* palo = pg->palo; palo && !palo->axf.empty() && palo->axf[0] && palo->axf[0]->kind == 0)
    {
        pg->matSaved = palo->axf[0]->mat;
        pg->fSaved = true;
    }
    SetGrappleState(*pg, 0);
}
// orig3 0x285748 (vt slot 32): the SO's, off
void ReleaseGrapple(LO* plo)
{
    GRAPPLE3* pg = Pgrapple(plo);
    if (!pg) return;
    SetGrappleState(*pg, -1);
    // the first record back to the matrix +0x3E0 (kind 0), stamped (SW +0x44F4's counter)
    if (ALO* palo = pg->palo; pg->fSaved && palo && !palo->axf.empty() && palo->axf[0] && palo->axf[0]->kind == 0)
    {
        palo->axf[0]->mat = pg->matSaved;
        palo->axf[0]->nStamp = ++g_cXfStamp;
    }
}

// orig3 0x286E38 (vt slot 78)
bool FAcceptGrappleContact(ALO* palo, CTMSG* pctm)
{
    GRAPPLE3* pg = Pgrapple(palo);
    if (!pg || !pctm) return false;
    GRAPPLE3& g = *pg;
    if (g.flash > 0.0f) return false;   // (slot 80 0x286DD8 -> 81 0x286FC0: flashing)
    if (!(pctm->grfAccepted & 2)) return false;
    LO* pfrom = pctm->ploSender;
    if (pfrom && pfrom == g.hook) return false;
    if (pfrom && FIsLoDerivedFrom(pfrom, 0x59) && PloProjectileLauncher(pfrom) == palo) return false;
    if (g.state == -1) return false;
    if (pctm->fKill3 || pctm->nKill3 || (pctm->grfFilter & 0x6000) || g.state != 1)
    {
        SetSoHealth3(palo, 0);
        g.hpShown = 0.0f;
    }
    else SetSoHealth3(palo, NSoHealth3(palo) - pctm->nState);
    if (g.state == 1)
    {
        Rumble(g, 4, 0.75f);
        JoltCm(&g_cm, 0);
    }
    if (NSoHealth3(palo) > 0) g.flash = 2.0f;
    else SetChargeState(g, 3);
    if (FPrint()) std::printf("grapple3: %d hit by %d (%d): hp %d (t %.2f)\n", palo->oid, pfrom ? pfrom->oid : -1, pctm->nState, NSoHealth3(palo), g_clock.t);
    return true;
}
int NGrappleStatus(LO* plo)   // orig3 0x286FC0 (vt slot 81)
{
    GRAPPLE3* pg = Pgrapple(plo);
    if (pg && pg->flash > 0.0f) return 1;
    return NSoHealth3(plo) > 0 ? 0 : 2;
}
// orig3 0x286D90 (vt slot 65, Sly 2's 61): the ALO's overlap kind, at most 1 with its own hook
int OverlapKindGrapple(ALO* palo, ALO* pOther)
{
    int k = OverlapKindAlo(palo, pOther);
    GRAPPLE3* pg = Pgrapple(palo);
    if (pg && pOther && pOther == pg->hook) k = std::min(k, 1);
    return k;
}
// orig3 0x287B40 (the hook's vt slot 61, Sly 2's 58): the base's (0x23E068: its contacts posted); the choices it then
// makes over its contacts are UpdateHookFlight's here (a swept segment)
void NotifyHookContacts(ALO* palo) { NotifyAloContacts(palo); }
int FGrappleIgnoresHit(LO* plo, int) { return NGrappleStatus(plo) ? 1 : 0; }   // orig3 0x286DD8 (vt slot 80)

struct INITGRAPPLE3
{
    INITGRAPPLE3()
    {
        RegisterLoUpdate(FN3_TAG | 0x2859F0, UpdateGrapple);
        RegisterObjectSetter3(1878, [](LO* plo, const CRef& r) { if (GRAPPLE3* pg = Pgrapple(plo)) pg->fExit = r.RefCoerceS32().m_n; });
        RegisterObjectGetter3(1878, [](LO* plo) { CRef r; GRAPPLE3* pg = Pgrapple(plo); r.SetS32(pg ? pg->fExit : 0); return r; });
        RegisterObjectMethod3(0x1105A8, [](LO* plo, const std::vector<CRef>&) {   // 1876: the dialog lines cleared
            if (GRAPPLE3* pg = Pgrapple(plo)) pg->aidDialog.clear();
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x110620, [](LO* plo, const std::vector<CRef>& a) {   // 1877: a dialog line
            if (GRAPPLE3* pg = Pgrapple(plo); pg && pg->aidDialog.size() < 32) pg->aidDialog.push_back(NArg(a, 0, -1));
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x1106A8, [](LO* plo, const std::vector<CRef>& a) {   // 1879: a state's sound
            GRAPPLE3* pg = Pgrapple(plo);
            if (pg && pg->asnd.size() < 12 && NArg(a, 1, -1) != -1)
                pg->asnd.push_back(SNDSTATE3{NArg(a, 0), NArg(a, 1, -1), NArg(a, 2), GArg(a, 3, 1.0f), GArg(a, 4)});
            return CRef(TAGK_Void);
        });
    }
} s_initgrapple3;
NATIVEFN3(0x285648, ResetGrapple);
NATIVEFN3(0x285748, ReleaseGrapple);
NATIVEFN3(0x286E38, FAcceptGrappleContact);
NATIVEFN3(0x286DD8, FGrappleIgnoresHit);
NATIVEFN3(0x286FC0, NGrappleStatus);
NATIVEFN3(0x286D90, OverlapKindGrapple);
NATIVEFN3(0x287B40, NotifyHookContacts);
}  // namespace

// the progress "powerUps" "fAcquiredWebcamWeapon" (0x285648 reads it at the reset)
int GrappleProgressWeapon3()
{
    if (std::getenv("SLY3_GRAPPLEWEAPON")) return 1;
    const CRef r = RefGetProgress({"powerUps", "fAcquiredWebcamWeapon"});
    return (r.m_tagk == TAGK_S32 && r.m_n) || (r.m_tagk == TAGK_Bool && r.m_bool) ? 1 : 0;
}

// (update.cpp, the objects' post-loads) the record made and the object updated (the init 0x285458 sets +0x3C bit 2)
void PostLoadGrapple3(LO* plo)
{
    if (!plo || !plo->pcls || !g_psw || !FIsLoDerivedFrom(plo, 0x56)) return;
    if (GRAPPLE3* pg = Pgrapple(plo)) ResolveGrapple(*pg);
    if (plo->iupd >= 0) g_psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
}

// orig3 0x1404D8 (carry.cpp LandCarry, kind 13): an object of Bentley's proxy +0x19BC placed where his thrown camera
// landed (0x287890), attached to what it touched (0x287080), its owner Bentley (0x286D68). False: nothing placed.
LO* PloPlaceGrapple3(LO* pbentley, LO* ploCarry, LO* ploTouched)
{
    if (!pbentley || !ploCarry || !g_psw) return nullptr;
    ALO* paloBentley = FIsLoDerivedFrom(pbentley, sly3::CID_ALO) ? static_cast<ALO*>(pbentley) : nullptr;
    LO* pproxy = nullptr;
    if (paloBentley)
        for (const auto& [ib, p] : paloBentley->apart)
            if (ib == (FN3_TAG | 0x19BCu)) pproxy = p;
    glm::vec3 place;
    if (!FPlaceGrapple(nullptr, Pos(ploCarry), &place))
    {
        if (FPrint()) std::printf("grapple3: no place at %.0f %.0f %.0f\n", Pos(ploCarry).x, Pos(ploCarry).y, Pos(ploCarry).z);
        return nullptr;
    }
    LO* pcam = pproxy ? PloAcquireProxyObject(g_psw, pproxy, false) : nullptr;
    if (!pcam)
    {
        // (no proxy part bound: the world's one camera object, when it is out of the world)
        for (const auto& up : g_psw->aploAll)
            if (FIsLoDerivedFrom(up.get(), 0x56) && !up->fInWorld) { pcam = up.get(); break; }
    }
    GRAPPLE3* pg = Pgrapple(pcam);
    if (!pg) return nullptr;
    GRAPPLE3& g = *pg;
    if (!pcam->fInWorld) AddLo(g_psw, pcam);
    PostLoadGrapple3(pcam);
    const glm::mat4 m = MatWorld(ploCarry);   // (0x1C8FB0: at the landed object)
    SetAloRotation(g.palo, glm::mat3(m));
    SetAloPos(g.palo, place);
    if (g.state == -1) ResetGrapple(pcam, nullptr);   // (vt slot 31: the proxy's acquire resets it)
    SetSoHealth3(pcam, NSoHealthMax3(pcam));
    g.hpShown = float(NSoHealth3(pcam));
    g.fWeapon = GrappleProgressWeapon3();
    ALO* paloTouched = ploTouched && FIsLoDerivedFrom(ploTouched, sly3::CID_ALO) ? static_cast<ALO*>(ploTouched) : nullptr;
    // (0x1404D8: placed (vt slot 34), then attached with +0x1C0 the landed object's point: the mount part stays there,
    // the camera 75 off the surface)
    SetAloPos(g.palo, place);
    AttachGrapple(g, paloTouched, paloTouched ? PxfObj(paloTouched) : nullptr, Pos(ploCarry), glm::vec3(glm::mat3(MatWorld(pbentley))[0]));
    g.pbentley = pbentley;
    if (FPrint()) std::printf("grapple3: %d placed at %.0f %.0f %.0f by %d\n", pcam->oid, place.x, place.y, place.z, pbentley->oid);
    return pcam;
}

// orig3 0x286D70 (0x286360(camera, 1)): Bentley's press with the camera at rest (its states +0x190 / +0x198 / +0x1A0 0)
bool FEnterGrapple3(LO* pcam)
{
    GRAPPLE3* pg = Pgrapple(pcam);
    if (!pg || !pcam->fInWorld) return false;
    if (pg->state != 0 || pg->grapple != 0 || pg->charge != 0) return false;
    SetGrappleState(*pg, 1);
    return true;
}
// the camera's state +0x190 (-2: none)
int NGrappleState3(LO* pcam)
{
    GRAPPLE3* pg = Pgrapple(pcam);
    return pg ? pg->state : -2;
}
// orig3 0x13E510 leaving Bentley's state 18 with his camera flown: its state 2
void LeaveGrapple3(LO* pcam)
{
    GRAPPLE3* pg = Pgrapple(pcam);
    if (pg && pg->state == 1) SetGrappleState(*pg, 2);
}
// (player3.cpp NPlayerCamKind3) the current player object is a Grapple-Cam (Camera Options' kind 2)
bool FGrappleCam3(const LO* plo) { return plo && plo->pcls && FIsLoDerivedFrom(const_cast<LO*>(plo), 0x56); }
void ResetGrapples3() { s_mpgrapple.clear(); }
