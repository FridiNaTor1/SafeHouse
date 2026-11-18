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
// policy base grew); the panel stays binoc3.cpp's. The binocucom's state machine (0x1BDA88 / 0x1BDBB0 / 0x1BE220):
// state 0 plays the actor's raise clip (its part +0xF6C, Sly 2's +0xF0C, when vt slot 104 allows it) and goes to 1
// when it is over; 1 opens the camera and the window (0x1BE5A8(0): the state the scripts' 0x46 waits on); the close
// (0x1BDB50) is state 2: the window closing (5) and the lower clip (+0xF70), then -1 (the window -1). A talk (kind 1)
// pauses the game (0x1A93C8 / 0x1A94A8 kind 2: the pad locked, the guards stopped) from leaving -1 to -1: the scripts'
// binocucom is always the original's kind 1 (0x226D00's 0x46: 0x1BDA88(+0xF60, 1, ..)), with a speaker or not
// (natively kind is 0 without one: the panel's), so every one of them pauses (SLY3_BINOCPAUSE=0: none, as before).
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
#include "actor.h"
#include "asega.h"
#include "clock.h"

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

void PauseGame(int grf);    // ai.cpp (orig 0x20EFC0)
void ResumeGame(int grf);   // ai.cpp (orig 0x20F030)

namespace
{
// (the talk's pause, 0x1A93C8 kind 2: the guards stop while the window is up; SLY3_BINOCPAUSE=0: off, as before)
bool FTalkPause() { static const bool s_f = [] { const char* sz = std::getenv("SLY3_BINOCPAUSE"); return !sz || sz[0] != '0'; }(); return s_f; }
// the scripts' binocucom mode (orig3 BINOC at actor +0xF60: +0x4 state, +0x8 its time, +0x14 the clip playing, +0x18
// kind, +0x50 the record)
struct MODE3
{
    LO* ploActor = nullptr;
    int state = -1;
    int kind = -1;
    float tState = 0.0f;
    BINOCOPEN3 b;
    LO* ploSpeaker = nullptr;
    LO* plo60 = nullptr;
    LO* plo64 = nullptr;
    ASEGA* pasega = nullptr;
    bool fPaused = false;   // (the game paused by this mode: 0x1A93C8 on leaving -1, 0x1A9420 on -1)
};
MODE3 s_mode;
float s_tMode = 0.0f;

ASEG* PasegClip(ALO* palo, uint32_t ib3, uint32_t ib2)
{
    if (!palo) return nullptr;
    for (const auto& [ib, plo] : palo->apart)
        if (ib == (FN3_TAG | ib3) || ib == ib2) return dynamic_cast<ASEG*>(plo);
    return nullptr;
}

// orig3: 0x001BDBB0
void SetMode(MODE3& m, int n)
{
    if (n == m.state || !m.ploActor) return;
    ACTOR* pa = PactorFromLo(m.ploActor);
    ALO* palo = FIsLoDerivedFrom(m.ploActor, S3CID(0x30)) ? static_cast<ALO*>(m.ploActor) : nullptr;
    const int old = m.state;
    if (old == 1)
    {
        CloseCam(m.ploActor);                         // 0x169C40, 0x1BE4A8, slot 95, 0x156928
        DuckBinocSounds3(1.0f);                       // 0x248A78(channels 0 / 1, 1.0, 3)
        if (m.kind != 2) HideActorForBinoc3(pa, false);   // 0x242228
    }
    else if (old == -1 && FTalkPause() && !m.fPaused)   // 0x1A93C8 / 0x1A94A8 (the actor's player, kind 2)
    {
        PauseGame(2);
        m.fPaused = true;
    }
    m.state = n;
    m.tState = g_clock.t;
    if (FPrint()) std::printf("binoccam3: actor %d mode %d -> %d (kind %d) t %.2f\n", m.ploActor->oid, old, n, m.kind, g_clock.t);
    ASEG* pclip = nullptr;
    switch (n)
    {
    case 0: pclip = PasegClip(palo, 0xF6C, 0xF0C); break;
    case 1:
        OpenBinocPanel3(m.ploActor, m.kind, m.ploSpeaker);   // (the window's 0x1BE5A8(0))
        OpenCam(m.ploActor, m.b, m.plo60, m.plo64);
        if (pa) pa->vecVel = glm::vec3(0.0f);                // +0x1A0 cleared, slots 40 / 41 zero
        if (palo)
        {
            SetAloVelocityTo(palo, glm::vec3(0.0f));
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        }
        DuckBinocSounds3(0.6f);
        if (m.kind != 2) HideActorForBinoc3(pa, true);     // 0x241FC0
        break;
    case 2:
        CloseBinocPanel3(m.ploActor);                      // 0x1BE5A8(5)
        pclip = PasegClip(palo, 0xF70, 0xF10);
        break;
    case -1:
        ShutBinocPanel3(m.ploActor);                       // 0x1BE5A8(-1)
        if (m.fPaused) ResumeGame(2);                      // 0x1A9420 / 0x1A94F0
        m.fPaused = false;
        m.kind = -1;
        break;
    default: break;
    }
    if (FPrint() && (n == 0 || n == 2))
        std::printf("binoccam3: clip %d allowed %d (actor state %d)\n", pclip ? pclip->oid : -1, int(FBinocAnimAllowed3(pa)), NActorState3(pa));
    if (pclip && !FBinocAnimAllowed3(pa)) pclip = nullptr;   // vt slot 104
    if (palo) ApplyAsegCur(pclip, palo, 0.0f, 1.0f, 1, &m.pasega);   // 0x137588
    else m.pasega = nullptr;
}

// orig3: 0x001BE220's states 0 / 2: on when the clip is over (0x138AC8)
void UpdateMode(MODE3& m)
{
    int n;
    do
    {
        n = m.state;
        if (n == 0 || n == 2)
        {
            if (ACTOR* pa = PactorFromLo(m.ploActor)) pa->vecVel = glm::vec3(0.0f);
            if (!m.pasega || FAsegaRetracted(m.pasega)) SetMode(m, n == 0 ? 1 : -1);
        }
    } while (n != m.state);
}
}  // namespace

int NBinocMode3(LO* ploActor) { return ploActor && ploActor == s_mode.ploActor ? s_mode.state : -1; }
// orig3: 0x1BDBB0(actor +0xF60, -1) from elsewhere (the scripts' 0x22 shut 0x1569A8, the effect holds' 0x1442D0, the
// guru's states): the scripts' binocucom put down at once. (The scripts' 0x22 shuts only a kind-0 binocucom (0x1569A8),
// the player's own: never this one, game/scptgame3.cpp FStoredFlagIsClear)
void ShutBinocMode3(LO* ploActor)
{
    if (ploActor && ploActor == s_mode.ploActor && s_mode.state != -1) SetMode(s_mode, -1);
}

namespace
{
// orig3: 0x001BDA88 (mode 1: the scripts'): an open one of another kind shut first; an open talk keeps going (natively
// its speaker changed: 0x1BE370)
void OpenMode(LO* ploActor, const BINOCOPEN3& b, LO* ploSpeaker, LO* plo60, LO* plo64)
{
    MODE3& m = s_mode;
    const int kind = ploSpeaker ? 1 : 0;
    if (m.state != -1)
    {
        if (m.ploActor == ploActor && m.kind == 1 && kind == 1)
        {
            if (m.state == 1) SetBinocPanelSpeaker3(ploActor, ploSpeaker);
            m.ploSpeaker = ploSpeaker;
            return;
        }
        SetMode(m, 2);
        SetMode(m, -1);
    }
    m.ploActor = ploActor;
    m.kind = kind;
    m.b = b;
    m.ploSpeaker = ploSpeaker;
    m.plo60 = plo60;
    m.plo64 = plo64;
    SetMode(m, 0);
    UpdateMode(m);   // (no clip: up at once, as the original's first update)
}
}  // namespace

void InstallBinocCam3()
{
    g_scpthooks3.pfnOpenBinoc = [](LO* ploActor, const BINOCOPEN3& b, LO* ploSpeaker, LO* plo60, LO* plo64, LO*) {
        OpenMode(ploActor, b, ploSpeaker, plo60, plo64);
    };
    // orig3: 0x001BDB50 (state 2 unless closed)
    g_scpthooks3.pfnCloseBinoc = [](LO* ploActor) {
        if (s_mode.ploActor == ploActor && s_mode.state != -1) SetMode(s_mode, 2);
        else CloseBinocPanel3(ploActor);
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

void ResetBinocCam3()
{
    s_ploCam = nullptr;
    s_mode = MODE3{};   // (a new world: the pause count is the world's too)
}

// orig3: 0x001BE220 (states 0 / 2: the clips; state 1, kind 1, every frame: the actor faces the camera's pan, +0xB4)
void UpdateBinocCam3()
{
    UpdateMode(s_mode);
    if (!s_ploCam || !FIsLoDerivedFrom(s_ploCam, S3CID(0x30))) return;
    SetStepPhysHeading(static_cast<ALO*>(s_ploCam), Cam().radPan);
}
