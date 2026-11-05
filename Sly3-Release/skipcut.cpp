// See skipcut.h.

#include "skipcut.h"

#include "audio.h"
#include "hud.h"
#include "joy.h"
#include "menu.h"
#include "pcinput.h"
#include "screenobj.h"
#include "screenrt.h"
#include "sw.h"
#include "task.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

bool g_fSkipCutFast = false;
extern int g_frameJoy;   // (joy.cpp: the step's frame)

namespace
{
constexpr float T_HOLD = 3.0f;          // seconds of Circle to skip
constexpr float T_HINT = 4.0f;          // the hint stays this long after the last button
constexpr int CSTEP_MAX = 60 * 240;     // a skip's fast-forward stops after 4 minutes of game time (a stuck scene)

struct SKIPCUT
{
    bool fHint = false;         // the hint shown
    float tHint = 0.0f;         // since the last button
    float tHold = 0.0f;         // Circle held this long
    bool fFast = false;         // fast-forwarding
    int cstep = 0;              // its steps
    bool fMaskCircle = false;   // Circle held from the skip: hidden from the game until released
    int frameWait = -100;       // the last step a script waited for a button
};
SKIPCUT s_skip;
SKIPCUT s_skipMovie;

bool FScptWaitsNow() { return g_frameJoy - s_skip.frameWait <= 2; }

void StopFast()
{
    if (!s_skip.fFast) return;
    s_skip.fFast = false;
    g_fSkipCutFast = false;
    SetOutputMute(false);
    if (std::getenv("SLY2N_SKIPPRINT")) std::printf("skipcut: fast-forward over after %d steps\n", s_skip.cstep);
}

void StartFast()
{
    s_skip.fFast = true;
    s_skip.cstep = 0;
    s_skip.fHint = false;
    s_skip.tHold = 0.0f;
    s_skip.fMaskCircle = true;
    g_fSkipCutFast = true;
    SetOutputMute(true);
    if (std::getenv("SLY2N_SKIPPRINT")) std::printf("skipcut: skipping (fast-forward)\n");
}

// the state machine shared by the game and the movies: true when the skip starts now
bool FUpdateHold(SKIPCUT& s, float dtReal, uint16_t grfHeld, uint16_t grfPressed, bool fSkippable)
{
    if (!fSkippable)
    {
        s.fHint = false;
        s.tHold = 0.0f;
        return false;
    }
    if (grfPressed) { s.fHint = true; s.tHint = 0.0f; }
    else s.tHint += dtReal;
    const bool fCircle = (grfHeld & BTN_Circle) != 0;
    if (fCircle && s.fHint)
    {
        s.tHint = 0.0f;
        if (g_pcinput.fInstantSkipCutscenes && (grfPressed & BTN_Circle)) return true;
        s.tHold += dtReal;
        if (s.tHold >= T_HOLD) return true;
    }
    else s.tHold = 0.0f;
    if (s.tHint > T_HINT) s.fHint = false;
    return false;
}

float DxText(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size)
{
    float w = 0.0f;
    const FONTBRX* pf = &font;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '&' && i + 1 < text.size() && text[i + 1] != '&')
        {
            pf = text[i + 1] == '.' || !picons ? &font : picons;
            ++i;
            continue;
        }
        w += pf->DxFromCh(uint8_t(text[i]));
    }
    return w * size / 51.2f;
}

// the hint (and the indicator while Circle is held) at the bottom right of the 4:3 canvas
void DrawHint(const SKIPCUT& s)
{
    if (!s.fHint) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const std::string text = picons ? "Hold &2O&. to skip" : "Hold Circle to skip";
    const float size = 24.0f;
    const float xRight = 616.0f, yTop = 412.0f;   // (canvas pixels)
    const float w = DxText(*pfont, picons, text, size);
    const float depth = 16000000.0f;   // (over every screen object)
    // (the colour offset leaves the glyphs' own alpha alone, as the game's HUD texts do (marker.cpp, dance.cpp): an
    // alpha offset made the whole glyph cells opaque, the letters' counters filled)
    const glm::vec4 white(200.0f, 200.0f, 200.0f, 0.0f);
    auto drawText = [&](const std::string& t, float px, float py, float sz, glm::vec4 off, float dz) {
        g_hud.DrawTextBox(*pfont, picons, t, px - 320.0f, (py - 224.0f) * 1.1f, sz, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                          glm::vec4(1.0f), glm::vec4(0.0f), depth + dz, false);
    };
    // a dark plate under the text
    const float hText = size * 0.909f;
    g_hud.FillScreenRect(xRight - w - 10.0f, yTop - 5.0f, xRight + 6.0f, yTop + hText + 7.0f, glm::vec4(0, 0, 0, 72), depth / 16777216.0f - 1e-4f);
    drawText(text, xRight - w, yTop, size, white, 100.0f);
    if (s.tHold > 0.0f && picons && !g_pcinput.fInstantSkipCutscenes)
    {
        // the indicator: the Circle icon, a line circling it as the hold fills
        const float hIcon = 26.0f;
        const float cx = xRight - w - 26.0f, cy = yTop + hText * 0.5f + 1.0f;
        g_hud.DrawIconGlyph(*picons, 'O', cx, cy, hIcon, depth + 200.0f);
        const float r = hIcon * 0.62f;
        g_hud.DrawRing(cx, cy, r + 3.5f, r, 1.0f, glm::vec4(0, 0, 0, 80), 0.99f);
        g_hud.DrawRing(cx, cy, r + 3.0f, r + 0.5f, s.tHold / T_HOLD, glm::vec4(250, 250, 250, 128), 0.995f);
    }
}
}  // namespace

bool FCutsceneActive()
{
    if (FMenuShown()) return false;
    if (g_grfjoyLock & 4) return true;   // a talk: a script's scene (its pause) or a binocucom talk
    if (g_psw)
        for (LO* plo : g_psw->mpcidaplo[0x84])
            if (static_cast<JOB*>(plo)->nSeq != 0) return true;   // a job's closing sequence
    return false;
}

void NoteScptWaitsForButton() { s_skip.frameWait = g_frameJoy; }

void UpdateSkipCut(float dtReal, uint16_t grfHeld, uint16_t grfPressed)
{
    if (s_skip.fMaskCircle && !(grfHeld & BTN_Circle) && !s_skip.fFast) s_skip.fMaskCircle = false;
    if (s_skip.fFast) return;
    const bool fSkippable = FCutsceneActive() && !FScptWaitsNow();
    // (tests: SLY2N_SKIPAT=frame: the skip starts at that step, as after the hold)
    static const int s_frameSkipAt = std::getenv("SLY2N_SKIPAT") ? std::atoi(std::getenv("SLY2N_SKIPAT")) : -1;
    bool fStart = FUpdateHold(s_skip, dtReal, grfHeld, grfPressed, fSkippable);
    if (s_frameSkipAt >= 0 && g_frameJoy >= s_frameSkipAt && g_frameJoy < s_frameSkipAt + 1 && fSkippable) fStart = true;
    if (std::getenv("SLY2N_SKIPPRINT") && fSkippable && s_skip.fHint)
    {
        static int s_c = 0;
        if (s_c++ % 30 == 0) std::printf("skipcut: hint (hold %.2f)\n", s_skip.tHold);
    }
    if (fStart) StartFast();
}

bool FSkipCutFastForward() { return s_skip.fFast; }

bool FSkipCutStep()
{
    if (!s_skip.fFast) return false;
    ++s_skip.cstep;
    if (!FCutsceneActive() || FScptWaitsNow() || s_skip.cstep >= CSTEP_MAX)
    {
        StopFast();
        return false;
    }
    return true;
}

uint16_t GrfSkipCutMask() { return s_skip.fMaskCircle ? uint16_t(BTN_Circle) : uint16_t(0); }

void DrawSkipCut()
{
    if (s_skip.fFast)
    {
        g_hud.Fade(glm::vec4(0, 0, 0, 128));   // (the fast-forward is not shown)
        return;
    }
    DrawHint(s_skip);
}

void ResetMovieSkip() { s_skipMovie = SKIPCUT{}; }

bool FMovieSkip(GLFWwindow* window, float dtReal)
{
    uint16_t grf = 0;
    float xl, yl, xr, yr;
    bool fPad = false;
    if (window) ReadPcInput(window, &grf, &xl, &yl, &xr, &yr, &fPad);
    static uint16_t s_grfPrev = 0;
    const uint16_t grfPressed = grf & ~s_grfPrev;
    s_grfPrev = grf;
    if (FUpdateHold(s_skipMovie, dtReal, grf, grfPressed, true))
    {
        s_skip.fMaskCircle = true;   // (the game after the movie does not see this Circle)
        return true;
    }
    return false;
}

void DrawMovieSkip() { DrawHint(s_skipMovie); }
