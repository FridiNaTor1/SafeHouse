// binoccam3.cpp: the binocucom's camera (the player code's side of the script's binocucom: game/scptgame3.h's
// pfnOpenBinoc / pfnCloseBinoc / pfnCmFocus / pfnCmTargets / pfnCmZoom / pfnCmSnap / pfnFCmDone).
//
// orig3: the script's OpenBinoc (SCPT command 0x46 in 0x226D00) calls 0x1BDA88(actor +0xF60, mode 1, record): the
// record {+0x0 the speaker, +0x4 the 0x405 object, +0x10 a position, +0x20 Euler angles, +0x30 the field of view, +0x40
// / +0x44 the focus / target objects}. The look-around mode's state 1 (0x1BDBB0, kind 1) makes a free camera (0x16A6F0,
// 0xE0 bytes: Sly 2's CPFREE, vtable 0x355D18: activate 0x16A930, update 0x16AA20, deactivate 0x16B0D0) on the actor at
// priority 3 and pushes it (0x169CE0); the record's targets go in by 0x16AE40 when its position, angles and field of
// view are all set, then the focus / target objects (0x16ADC0 / 0x16AE00, each snapped by 0x16AE90); the actor turns
// to the camera's pan (+0xB4, 0x24E878). Leaving state 1 (the close, 0x1BDB50 -> state 2) revokes the camera
// (0x169C40). The script's 0x48..0x4C reach the camera (actor +0x1008) directly: 0x16ADC0 / 0x16AE00 / 0x16AE40 /
// 0x16AE88 / 0x16AE90; its waits: 0x16AEF8 (the focus reached), 0x16AFB8 (the eye reached), 0x16B078 (the zoom).
//
// Natively the camera is the copied CPFREE (g_cmpolicies.cpfree: the same fields, Sly 3's are 0x10 further on: the
// policy base grew); the panel stays binoc3.cpp's. The binocucom's state machine (the raise / lower clips, state 0) is
// not ported: the camera opens with the panel, as the window opens with state 1 in the original.
#include "binoc3.h"
#include "binoc.h"
#include "brx.h"
#include "alo.h"
#include "game/sly3.h"
#include "cm.h"
#include "cplcy.h"
#include "game/scptgame3.h"
#include "game/scptrun3.h"
#include "lo.h"
#include "stepphys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY3_BINOCPRINT") != nullptr; return s_f; }

LO* s_ploCam = nullptr;   // the actor whose binocucom camera is up (orig3: actor +0x1008 not null)

CPFREE& Cam() { return g_cmpolicies.cpfree; }

bool FZero3(const float a[3]) { return std::fabs(a[0]) < 1e-4f && std::fabs(a[1]) < 1e-4f && std::fabs(a[2]) < 1e-4f; }

// orig3: 0x001BDBB0 state 1, kind 1
void OpenCam(LO* ploActor, const BINOCOPEN3& b, LO* plo60, LO* plo64)
{
    if (s_ploCam) return;
    s_ploCam = ploActor;
    ALO* palo = FIsLoDerivedFrom(ploActor, S3CID(0x30)) ? static_cast<ALO*>(ploActor) : nullptr;
    SetCmPolicy(&g_cm, 8, &Cam(), ploActor, nullptr);   // 0x169CE0 (the copied numbering's binocucom priority)
    if (!FZero3(b.posEye) && !FZero3(b.posLook) && b.g30 != 0.0f)   // 0x16AE40
        Cam().SetTargets(glm::vec3(b.posEye[0], b.posEye[1], b.posEye[2]), glm::vec3(b.posLook[0], b.posLook[1], b.posLook[2]), b.g30);
    if (palo) SetStepPhysHeading(palo, Cam().radPan);   // 0x24E878 (camera +0xB4)
    HoldBinocPanelSpeaker3(ploActor, true);             // 0x1BE408 (the speaker in the window)
    if (plo60)
    {
        Cam().SetFocusObject(plo60);   // 0x16ADC0
        Cam().Snap();                  // 0x16AE90
    }
    if (plo64)
    {
        Cam().SetTargetObject(plo64);  // 0x16AE00
        Cam().Snap();
    }
    // the player's own effects held (slot 94 (0x3F, 0)) and his talk animation (0x156900: the right monitor's pose)
    HoldActorEffects(ploActor, true, false);
    PlayActorTalkAnim(ploActor, true);
    if (FPrint())
        std::printf("binoccam3: actor %d open: targets %d (%.0f %.0f %.0f / %.3f %.3f %.3f fov %.3f) focus %d target %d\n", ploActor ? ploActor->oid : -1,
                    Cam().fScripted, b.posEye[0], b.posEye[1], b.posEye[2], b.posLook[0], b.posLook[1], b.posLook[2], b.g30, plo60 ? plo60->oid : -1,
                    plo64 ? plo64->oid : -1);
}

// orig3: 0x001BDBB0 leaving state 1 (0x169C40)
void CloseCam(LO* ploActor)
{
    if (!s_ploCam || ploActor != s_ploCam) return;
    RevokeCmPolicy(&g_cm, 0xF, 8, &Cam(), s_ploCam, nullptr);
    HoldBinocPanelSpeaker3(ploActor, false);   // 0x1BE4A8
    HoldActorEffects(ploActor, false, false);   // slot 95 (0x3F, 0)
    PlayActorTalkAnim(ploActor, false);          // 0x156928
    s_ploCam = nullptr;
    if (FPrint()) std::printf("binoccam3: actor %d closed\n", ploActor ? ploActor->oid : -1);
}

// orig3: 0x0011E9F0 (relatively equal: |a - b| / max(|a|, 1) < eps)
bool FEqual3(float g1, float g2, float eps)
{
    const float d = std::fabs(g1 - g2);
    return d / std::max(std::fabs(g1), 1.0f) < eps;
}

// (0x16AEF8 / 0x16AFB8's test: each coordinate equal within 0.0001)
bool FNear3(const glm::vec4& a, const glm::vec3& b) { return FEqual3(a.x, b.x, 1e-4f) && FEqual3(a.y, b.y, 1e-4f) && FEqual3(a.z, b.z, 1e-4f); }
}  // namespace

void InstallBinocCam3()
{
    g_scpthooks3.pfnOpenBinoc = [](LO* ploActor, const BINOCOPEN3& b, LO* ploSpeaker, LO* plo60, LO* plo64, LO*) {
        OpenBinocPanel3(ploActor, ploSpeaker ? 1 : 0, ploSpeaker);
        OpenCam(ploActor, b, plo60, plo64);
    };
    g_scpthooks3.pfnCloseBinoc = [](LO* ploActor) {
        CloseBinocPanel3(ploActor);
        CloseCam(ploActor);
    };
    g_scpthooks3.pfnCmFocus = [](LO* ploActor, int id, LO* plo) {
        if (!s_ploCam || ploActor != s_ploCam) return;
        if (id == 0x48) Cam().SetFocusObject(plo);   // 0x16ADC0
        else Cam().SetTargetObject(plo);             // 0x16AE00
    };
    g_scpthooks3.pfnCmTargets = [](LO* ploActor, const float pos[3], const float eul[3], float g) {
        if (!s_ploCam || ploActor != s_ploCam) return;
        Cam().SetTargets(glm::vec3(pos[0], pos[1], pos[2]), glm::vec3(eul[0], eul[1], eul[2]), g);   // 0x16AE40
    };
    g_scpthooks3.pfnCmZoom = [](LO* ploActor, float g) {
        if (s_ploCam && ploActor == s_ploCam) Cam().SetZoomTarget(g);   // 0x16AE88
    };
    g_scpthooks3.pfnCmSnap = [](LO* ploActor) {
        if (s_ploCam && ploActor == s_ploCam) Cam().Snap();   // 0x16AE90
    };
    g_scpthooks3.pfnFCmDone = [](LO* ploActor, int id) {
        if (!s_ploCam || ploActor != s_ploCam) return true;
        CPFREE& c = Cam();
        switch (id)
        {
        case 0x48: return FNear3(c.posFocus, c.PosFocus());                      // 0x16AEF8
        case 0x49: return FNear3(c.posEye, c.PosEye());                          // 0x16AFB8
        case 0x4B: return c.uZoomTarget < 0.0f || FEqual3(c.uZoom, c.uZoomTarget, 0.01f);   // 0x16B078
        default: return true;
        }
    };
}

void ResetBinocCam3() { s_ploCam = nullptr; }

// orig3: 0x001BE220 state 1, kind 1 (every frame: the actor faces the camera's pan, +0xB4)
void UpdateBinocCam3()
{
    if (!s_ploCam || !FIsLoDerivedFrom(s_ploCam, S3CID(0x30))) return;
    SetStepPhysHeading(static_cast<ALO*>(s_ploCam), Cam().radPan);
}
