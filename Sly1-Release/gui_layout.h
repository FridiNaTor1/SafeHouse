#pragma once

#include "gl.h"
#include <algorithm>

struct GuiScale
{
    float x;
    float y;
};

inline GuiScale GetGuiScale(float scaleSetting)
{
    if (g_guiStyle == GuiStyle_PS2)
    {
        // Retail lays the UI out on a 640 x 492.8 canvas and the TV shows that
        // canvas at 4:3, so one canvas unit is (4/3) / (640/492.8) = 1.0267
        // times wider than it is tall. Keep that shape on every presentation
        // aspect: at 4:3 this is exactly width/640 by height/492.8 (the PS2
        // picture), and wider outputs keep the same artwork shape while the
        // blots stay anchored to the framebuffer edges. (A full 4/3 factor
        // here stretched font-2 HUD glyphs, the lives head and the counter
        // digits, by 30%, so the lives and clue counters overlapped at 4:3.)
        constexpr float kPs2HorizontalPixelAspect =
            (4.0f / 3.0f) / (640.0f / 492.80002f);
        const float canvasScale = g_gl.uiScale * scaleSetting;
        return {
            canvasScale * kPs2HorizontalPixelAspect,
            canvasScale
        };
    }

    // Modern UI keeps the original assets and authored layout relationships,
    // but treats them as a compact, uniformly-scaled HUD.  A 720p density
    // makes the HUD roughly 60% of the apparent PS2 size at 1080p and avoids
    // stretching glyphs on widescreen and ultrawide displays.
    constexpr float kModernReferenceHeight = 720.0f;
    constexpr float kModernDensity = 0.90f;
    const float uniformScale = (g_gl.height / kModernReferenceHeight) *
        kModernDensity * scaleSetting;
    return { uniformScale, uniformScale };
}

inline GuiScale GetGuiScale()
{
    return GetGuiScale(g_guiScale);
}

inline bool FModernGui()
{
    return g_guiStyle == GuiStyle_Modern;
}
