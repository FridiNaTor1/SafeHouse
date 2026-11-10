// See skipcut.h. After Sly2-Native's skipcut.cpp (the hold, the hint, the fast-forward).
#include "skipcut.h"
#include "porttext.h"
#include "Input.h"
#include "clock.h"
#include "font.h"
#include "gl.h"
#include "glshaders.h"
#include "prompt.h"
#include "save.h"
#include "screen.h"
#include "sound.h"
#include "sw.h"
#include "transition.h"
#include "ui.h"
#include "wipe.h"
#include "fmv.h"
#include "po.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
constexpr float T_HOLD = 2.0f;           // seconds of Circle to skip
constexpr float T_HINT = 4.0f;           // the hint stays this long after the last button
constexpr float DT_CAP = 240.0f;         // a skip stops after 4 minutes of game time (a stuck scene)

struct SKIPCUT
{
    bool fHint = false;       // the hint shown
    float tHint = 0.0f;       // since the last button
    float tHold = 0.0f;       // Circle held this long
    bool fFast = false;       // fast-forwarding
    float tFastStart = 0.0f;  // g_clock.t at its start
    int cstep = 0;            // its steps
    bool fMaskCircle = false; // the Circle that skipped: hidden from the game until released
};
SKIPCUT s_skip;
SKIPCUT s_skipMovie;

bool FPrint() { static const bool s_f = std::getenv("CANE_SKIPPRINT") != nullptr; return s_f; }

// (CANE_SKIPPRINT) the state a scene ends in: the game clock, the player and the camera
void PrintState(const char* pchz)
{
    if (!FPrint()) return;
    PO* ppo = PpoCur();
    const glm::vec3 pos = ppo != nullptr ? ppo->xf.posWorld : glm::vec3(0.0f);
    const glm::vec3 cam = g_pcm != nullptr ? g_pcm->pos : glm::vec3(0.0f);
    std::printf("skipcut: %s t %.4f player %.3f %.3f %.3f camera %.3f %.3f %.3f letterbox %d\n", pchz, g_clock.t,
        pos.x, pos.y, pos.z, cam.x, cam.y, cam.z, g_psw != nullptr ? g_psw->cLetterbox : -1);
}

bool FAnyPressed()
{
    for (int button = 0; button < BTN_MAX; ++button)
        if (g_joy.current[static_cast<JOY_BUTTON>(button)] && !g_joy.previous[static_cast<JOY_BUTTON>(button)])
            return true;
    return false;
}

// The state machine shared by the game and the movies: true when the skip starts now.
bool FUpdateHold(SKIPCUT& s, float dtReal, bool fSkippable)
{
    if (!fSkippable)
    {
        s.fHint = false;
        s.tHold = 0.0f;
        return false;
    }
    const bool fPressed = FAnyPressed();
    if (fPressed) { s.fHint = true; s.tHint = 0.0f; }
    else s.tHint += dtReal;
    const bool fCircle = g_joy.current[BTN_CIRCLE];
    if (fCircle && s.fHint)
    {
        s.tHint = 0.0f;
        if (g_fInstantSkipCutscenes && !g_joy.previous[BTN_CIRCLE]) return true;
        s.tHold += dtReal;
        if (s.tHold >= T_HOLD) return true;
    }
    else s.tHold = 0.0f;
    if (s.tHint > T_HINT) s.fHint = false;
    return false;
}

void StopFast()
{
    if (!s_skip.fFast) return;
    s_skip.fFast = false;
    SetSkipCutSound(false);
    PrintState("scene end (skipped)");
    if (FPrint()) std::printf("skipcut: fast-forward over after %d steps (%.2f s of game time)\n", s_skip.cstep, g_clock.t - s_skip.tFastStart);
}

void StartFast()
{
    s_skip.fFast = true;
    s_skip.cstep = 0;
    s_skip.tFastStart = g_clock.t;
    s_skip.fHint = false;
    s_skip.tHold = 0.0f;
    s_skip.fMaskCircle = true;
    SetSkipCutSound(true);
    if (FPrint()) std::printf("skipcut: skipping (fast-forward) at t %.2f\n", g_clock.t);
}

// The hint (and the indicator while Circle is held), small in the bottom right corner, clear of the subtitles.
void DrawHint(const SKIPCUT& s)
{
    if (!s.fHint || g_pfont == nullptr) return;
    CFontBrx* pfont = g_pfont;
    const bool fIcons = FFontLoaded(1);
    // 0.75 of the text font at the 0.6 scale the hint was tuned with.
    const float scale = g_gl.uiScale * 0.45f;
    char achz[128];
    std::snprintf(achz, sizeof(achz), "%s", PchzPort(fIcons ? PTX_HoldToSkip : PTX_HoldToSkipNoIcons));

    pfont->PushScaling(scale, scale);
    const float dy = static_cast<float>(pfont->m_dyUnscaled) * pfont->m_ryScale;
    CRichText text(achz, pfont);
    const float dx = text.Dx();
    const float xRight = static_cast<float>(g_gl.width) - 20.0f * g_gl.uiScale;
    const float yTop = static_cast<float>(g_gl.height) - 20.0f * g_gl.uiScale - dy;

    // a dark plate under the text
    glBlotShader.Use();
    pfont->SetupDraw();
    BindBlotTexture(whiteTex);
    RenderGlyphQuad(xRight - dx - 10.0f * scale, yTop - 4.0f * scale, dx + 16.0f * scale, dy + 8.0f * scale,
        0.0f, 0.0f, 1.0f, 1.0f, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));

    CTextBox box;
    box.SetPos(xRight - dx, yTop);
    box.SetSize(dx + 4.0f, dy);
    glm::vec4 color(0.85f, 0.85f, 0.85f, 1.0f);
    box.SetTextColor(&color);
    box.SetHorizontalJust(JH_Left);
    box.SetVerticalJust(JV_Top);
    text.Draw(&box, nullptr);
    pfont->PopScaling();

    if (s.tHold > 0.0f && fIcons && !g_fInstantSkipCutscenes)
    {
        // the indicator: the Circle icon, a line circling it as the hold fills
        const float big = scale * 1.6f;
        pfont->PushScaling(big, big);
        const float dyIcon = static_cast<float>(pfont->m_dyUnscaled) * pfont->m_ryScale;
        CRichText icon(const_cast<char*>("&2O&."), pfont);
        const float dxIcon = icon.Dx();
        const float cx = xRight - dx - 22.0f * scale - dyIcon * 0.5f;
        const float cy = yTop + dy * 0.5f;
        CTextBox boxIcon;
        boxIcon.SetPos(cx - dxIcon * 0.5f, cy - dyIcon * 0.5f);
        boxIcon.SetSize(dxIcon + 2.0f, dyIcon);
        glm::vec4 white(1.0f);
        boxIcon.SetTextColor(&white);
        boxIcon.SetHorizontalJust(JH_Left);
        boxIcon.SetVerticalJust(JV_Top);
        icon.Draw(&boxIcon, nullptr);
        pfont->PopScaling();

        const float r = dyIcon * 0.62f;
        const float u = std::fmin(s.tHold / T_HOLD, 1.0f);
        constexpr int cseg = 48;
        const glm::vec4 ringBack(0.0f, 0.0f, 0.0f, 0.6f), ring(1.0f, 1.0f, 1.0f, 1.0f);
        for (int pass = 0; pass < 2; ++pass)
        {
            const int c = pass == 0 ? cseg : static_cast<int>(std::lround(u * cseg));
            for (int i = 0; i < c; ++i)
                for (float dr : { -1.0f, 0.0f, 1.0f })
                {
                    const float a0 = -1.5707963f + 6.2831853f * static_cast<float>(i) / cseg;
                    const float a1 = -1.5707963f + 6.2831853f * static_cast<float>(i + 1) / cseg;
                    const float rr = r + dr * (pass == 0 ? 1.5f : 0.7f);
                    DrawLineScreen(cx + rr * std::cos(a0), cy + rr * std::sin(a0), 0.0f,
                        cx + rr * std::cos(a1), cy + rr * std::sin(a1), 0.0f, pass == 0 ? ringBack : ring, false);
                }
        }
    }
}
}

bool FCutsceneActive()
{
    if (g_psw == nullptr || g_psw->cLetterbox <= 0)
        return false;
    // (a menu, a prompt waiting for an answer, a wipe or a level change: not a scene that plays)
    if (g_prompt.fActive || g_prompt.blots != BLOTS_Hidden)
        return false;
    if (g_ui.uis != UIS_Playing)
        return false;
    if (g_transition.m_fPending != 0 || FCutscenePending())
        return false;
    return true;
}

void UpdateSkipCut(float dtReal)
{
    if (s_skip.fMaskCircle)
    {
        if (!g_joy.current[BTN_CIRCLE] && !s_skip.fFast) s_skip.fMaskCircle = false;
        else g_joy.current[BTN_CIRCLE] = false;
    }
    if (s_skip.fFast) return;
    {
        if (std::getenv("CANE_SKIPDIAG") && g_psw != nullptr && (static_cast<int>(g_clock.t * 2.0f) != static_cast<int>((g_clock.t - g_clock.dt) * 2.0f)))
            std::printf("skipdiag: t %.2f letterbox %d uis %d prompt %d/%d trans %d\n", g_clock.t, g_psw->cLetterbox, int(g_ui.uis),
                g_prompt.fActive, int(g_prompt.blots), g_transition.m_fPending);
        static bool s_fWas = false;
        const bool fNow = FCutsceneActive();
        if (fNow != s_fWas) PrintState(fNow ? "scene start" : "scene end");
        s_fWas = fNow;
    }
    // (tests: CANE_SKIPAT=t: the skip starts at the first cutscene frame from game time t, as after the hold)
    static const float s_tSkipAt = std::getenv("CANE_SKIPAT") ? static_cast<float>(std::atof(std::getenv("CANE_SKIPAT"))) : -1.0f;
    const bool fSkippable = FCutsceneActive();
    bool fStart = FUpdateHold(s_skip, dtReal, fSkippable);
    if (s_tSkipAt >= 0.0f && fSkippable && g_clock.t >= s_tSkipAt && !s_skip.fMaskCircle)
    {
        static bool s_fDone = false;
        if (!s_fDone) fStart = s_fDone = true;
    }
    if (fStart)
    {
        g_joy.current[BTN_CIRCLE] = false;
        StartFast();
    }
}

bool FSkipCutFastForward() { return s_skip.fFast; }

bool FSkipCutStep()
{
    if (!s_skip.fFast) return false;
    ++s_skip.cstep;
    if (!FCutsceneActive() || g_clock.t - s_skip.tFastStart >= DT_CAP)
    {
        StopFast();
        return false;
    }
    return true;
}

void DrawSkipCut()
{
    if (!s_skip.fFast)
        DrawHint(s_skip);
}

void ResetMovieSkip() { s_skipMovie = SKIPCUT{}; }

bool FMovieSkipHeld(float dtReal)
{
    if (FUpdateHold(s_skipMovie, dtReal, true))
    {
        s_skip.fMaskCircle = true;   // (the game after the movie does not see this Circle)
        return true;
    }
    return false;
}

void DrawMovieSkip() { DrawHint(s_skipMovie); }
