// SafeHouse addition (not in the original game): the cutscene editor's custom presentation (cutscene.h,
// notes/CUTSCENES_2026-11-18.md "Custom presentation", docs/MODDING.md "Custom presentation (SafeHouse)").
//
// Only for a mod's cutscene whose rule (mod.json "cutscenes": [{..., "custom_presentation": true}]) asks for it:
//   - a caption (the hint command 0x53) while the scene has the game paused: the game's own hint box is not seen then
//     (the HUD's ticker under the scene's bars), so the caption is drawn as the scene's subtitle (in the subtitle box,
//     the subtitles' font and colour) when no line is speaking, else in a caption box of its own (the same font, at
//     the top); outside a pause it is the game's hint box as before
//   - a title card of its own text: a hint command whose "no waiting" word has SCPTB_TITLECARD (0x20 / 0x21, stored as
//     a 32-bit int; the original's 212 hints all have the bool 1) shows the text in large letters on a band across the
//     picture for its seconds, faded in and out
// Every other script (the original game's, a mod's without the rule) runs the hint as the game does. The text is the
// level's string of that id (the mod's Text.json, its languages' files), as any of the game's strings.
#include "cutscene.h"
#include "ai.h"
#include "clock.h"
#include "display.h"
#include "hud.h"
#include "hudwidget.h"
#include "screenobj.h"
#include "sound.h"
#include "splayer.h"
#include "sw.h"
#include "worldglobals.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
struct SHOWN
{
    int id = -1;
    float t0 = 0.0f, dt = 0.0f;    // from (the game's clock) / for (seconds; <= 0: while the scene's pause lasts)
    bool FOn() const
    {
        if (id < 0) return false;
        const float u = g_clock.t - t0;
        return u >= 0.0f && (dt > 0.0f ? u < dt : g_gamepause.c78 > 0);
    }
    float Alpha(float dtFade) const   // (faded in / out over dtFade)
    {
        const float u = g_clock.t - t0;
        float a = std::min(1.0f, u / dtFade);
        if (dt > 0.0f) a = std::min(a, (dt - u) / dtFade);
        return std::clamp(a, 0.0f, 1.0f);
    }
};
SHOWN s_caption, s_title;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_CUTPRESENT") != nullptr; return s_f; }

std::string StrOf(int id) { return g_psw ? StrLocalized(g_psw, id) : std::string(); }

// the picture's canvas extent (wider than 0..640 when pillarboxed: hud.cpp PictureExtent)
void Extent(float& x0, float& x1)
{
    const glm::vec2 s = g_display.uiScale;
    x0 = 320.0f - 320.0f / s.x;
    x1 = 320.0f + 320.0f / s.x;
}
}  // namespace

namespace cut
{
void ShowCutCaption(int idText, float dt)
{
    // (outside a scene's pause: the game's own hint box)
    if (g_gamepause.c78 <= 0) { HudPrompt(idText, dt); return; }
    s_caption = SHOWN{idText, g_clock.t, dt};
    if (FPrint()) std::printf("cutpresent: caption %d for %.2f s at %.2f (paused): \"%s\"\n", idText, dt, g_clock.t, StrOf(idText).c_str());
}

void EndCutCaption(int idText)
{
    if (idText == -1 || idText == s_caption.id) s_caption = SHOWN{};
}

void ShowCutTitle(int idText, float dt)
{
    s_title = SHOWN{idText, g_clock.t, dt > 0.0f ? dt : 3.0f};
    if (FPrint()) std::printf("cutpresent: title card %d for %.2f s at %.2f: \"%s\"\n", idText, s_title.dt, g_clock.t, StrOf(idText).c_str());
}

void ResetCutPresent()
{
    s_caption = SHOWN{};
    s_title = SHOWN{};
}

bool FCutCaptionInSubtitleBox(std::string* pstr)
{
    if (!s_caption.FOn() || g_gamepause.c80 <= 0 || FDialogPlaying(-1)) return false;
    *pstr = StrOf(s_caption.id);
    return !pstr->empty();
}

void DrawCutPresent(const FONTBRX* pfont, const FONTBRX* picons)
{
    if (!pfont) return;
    float x0, x1;
    Extent(x0, x1);
    const float zRect = (g_zScreenBase + 8.0f) / 16777216.0f, depthText = g_zScreenBase + 16.0f;
    // the caption box: while the subtitle box is not showing it (a line speaking, or the scene's pause without its bars)
    std::string dummy;
    if (s_caption.FOn() && !FCutCaptionInSubtitleBox(&dummy))
    {
        const std::string text = StrOf(s_caption.id);
        if (!text.empty())
        {
            const float a = s_caption.Alpha(0.2f);
            const float wBox = 520.0f, size = 26.0f;
            int cline = 1;
            for (char ch : StrWrapped(*pfont, picons, text, size / 51.2f, wBox)) cline += ch == '\n';
            const float hText = size * float(cline) * 448.0f / 492.8f;
            const float yTop = 14.0f;
            g_hud.FillScreenRect(320.0f - wBox * 0.5f - 12.0f, yTop - 6.0f, 320.0f + wBox * 0.5f + 12.0f, yTop + hText + 8.0f,
                                 glm::vec4(0.0f, 0.0f, 0.0f, 96.0f * a), zRect);
            g_hud.DrawTextBox(*pfont, picons, text, -wBox * 0.5f, yTop * 492.8f / 448.0f - 246.4f, size, 1.0f, 1.0f, wBox, size * float(cline), 1, 0,
                              glm::vec4(160.0f, 160.0f, 160.0f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, a), glm::vec4(0.0f), depthText);
        }
    }
    // the title card: a band across the picture, the text large in its middle
    if (s_title.FOn())
    {
        const std::string text = StrOf(s_title.id);
        if (!text.empty())
        {
            const float a = s_title.Alpha(0.35f);
            const float size = 40.0f, wBox = 600.0f;
            int cline = 1;
            for (char ch : StrWrapped(*pfont, picons, text, size / 51.2f, wBox)) cline += ch == '\n';
            const float hText = size * float(cline) * 448.0f / 492.8f;
            const float yMid = 190.0f, yTop = yMid - hText * 0.5f;
            g_hud.FillScreenRect(x0, yTop - 18.0f, x1, yTop + hText + 18.0f, glm::vec4(0.0f, 0.0f, 0.0f, 80.0f * a), zRect);
            g_hud.FillScreenRect(x0, yTop - 20.0f, x1, yTop - 18.0f, glm::vec4(230.0f, 180.0f, 60.0f, 110.0f * a), zRect);
            g_hud.FillScreenRect(x0, yTop + hText + 18.0f, x1, yTop + hText + 20.0f, glm::vec4(230.0f, 180.0f, 60.0f, 110.0f * a), zRect);
            g_hud.DrawTextBox(*pfont, picons, text, -wBox * 0.5f, yTop * 492.8f / 448.0f - 246.4f, size, 1.0f, 1.0f, wBox, size * float(cline), 1, 0,
                              glm::vec4(230.0f, 190.0f, 90.0f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, a), glm::vec4(0.0f), depthText);
        }
    }
}
}  // namespace cut
