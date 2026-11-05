// See hackmouse.h. Native QoL (Phase 3): no original code.

#include "hackmouse.h"

#include "cm.h"
#include "cmworld.h"
#include "display.h"
#include "hud.h"
#include "lo.h"
#include "pcinput.h"
#include "po.h"
#include "render.h"
#include "so.h"
#include "sw.h"
#include "vecmat.h"

#include "../platform/controller_family.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern int g_frameJoy;   // joy.cpp (the input step)

namespace
{
// (tests) SLY2N_TESTCURSOR="X Y@F0-F1[c];...": canvas pixels, input frames [F0, F1), "c" = left button held
struct TESTCURSOR { float x, y; int frame0, frame1; bool fLeft; };
const std::vector<TESTCURSOR>& TestCursors()
{
    static std::vector<TESTCURSOR> s_a;
    static bool s_fParsed = false;
    if (!s_fParsed)
    {
        s_fParsed = true;
        if (const char* psz = std::getenv("SLY2N_TESTCURSOR"))
            for (const char* pch = psz; pch && *pch;)
            {
                TESTCURSOR t{};
                char ch = 0;
                const int c = std::sscanf(pch, "%f %f@%d-%d%c", &t.x, &t.y, &t.frame0, &t.frame1, &ch);
                if (c >= 4)
                {
                    t.fLeft = c == 5 && ch == 'c';
                    s_a.push_back(t);
                }
                pch = std::strchr(pch, ';');
                if (pch) ++pch;
            }
    }
    return s_a;
}

const TESTCURSOR* PtestCursorNow()
{
    for (const TESTCURSOR& t : TestCursors())
        if (g_frameJoy >= t.frame0 && g_frameJoy < t.frame1) return &t;
    return nullptr;
}

// canvas pixels <-> picture NDC (hud.vert: the canvas in the picture by uiScale / uiOffset)
glm::vec2 NdcFromCanvas(glm::vec2 c)
{
    const glm::vec2 ndc(c.x / 320.0f - 1.0f, 1.0f - c.y / 224.0f);
    return ndc * g_display.uiScale + g_display.uiOffset;
}
glm::vec2 CanvasFromNdc(glm::vec2 p)
{
    const glm::vec2 ndc = (p - g_display.uiOffset) / g_display.uiScale;
    return glm::vec2((ndc.x + 1.0f) * 320.0f, (1.0f - ndc.y) * 224.0f);
}

// The cursor in picture NDC this frame (the test's, else the window's free cursor)
bool FCursorNow(glm::vec2* pndc, bool* pfLeft)
{
    if (!TestCursors().empty())
    {
        const TESTCURSOR* pt = PtestCursorNow();
        if (!pt) return false;
        *pndc = NdcFromCanvas(glm::vec2(pt->x, pt->y));
        *pfLeft = pt->fLeft;
        return true;
    }
    float x, y;
    return FCursorPicture(&x, &y, pfLeft) && ((*pndc = glm::vec2(x, y)), true);
}

// The drawn camera's world-to-clip transform (as RenderScene: the picture's projection)
glm::mat4 MatWorldToClipDrawn(float* prAspect)
{
    CAMERA cam = CameraFromCm(g_cm);
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    cam.fovY = glm::degrees(proj.radFovY);
    *prAspect = proj.rAspect;
    return cam.MatWorldToClip(proj.rAspect);
}

float s_radAimLast = 0.0f;
}  // namespace

bool FHackMouseMode()
{
    if (!g_pcinput.fHackMouseAim || !g_psw) return false;
    LO* plo = PloPoCurrent(g_psw);
    if (!plo || HackShipState(plo) < -1) return false;   // (the ship is the player: driven, docked, dead)
    if (FPcMenuMode()) return false;                      // (a menu, a talk: the original's pad)
    if (!TestCursors().empty()) return PtestCursorNow() != nullptr;
    return FPcInputLive() && !cane::input::tracker().lastWasGamepad();
}

bool FHackMouseAim(LO* plo, float* pradAim, bool* pfFire)
{
    if (!FHackMouseMode()) return false;
    glm::vec2 ndc;
    bool fLeft = false;
    if (!FCursorNow(&ndc, &fLeft)) return false;
    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(plo));
    float rAspect = 1.0f;
    const glm::mat4 m = MatWorldToClipDrawn(&rAspect);
    // the cursor's ray (reversed Z: depth 1 on the near plane, 0.001 a thousand times further) met with the ship's
    // plane: the place in the hack level under the cursor
    const glm::mat4 inv = glm::inverse(m);
    auto unproject = [&](float depth) {
        const glm::vec4 p = inv * glm::vec4(ndc.x, ndc.y, depth, 1.0f);
        return glm::vec3(p) / p.w;
    };
    const glm::vec3 a = unproject(1.0f), dir = unproject(0.001f) - a;
    glm::vec2 d(0.0f);
    if (std::fabs(dir.z) > 1e-6f * glm::length(dir) && (pos.z - a.z) / dir.z > 0.0f)
    {
        const glm::vec3 p = a + dir * ((pos.z - a.z) / dir.z);
        d = glm::vec2(p.x - pos.x, p.y - pos.y);
    }
    else
    {
        // (no such place: the screen direction from the ship, turned by the camera's pan as the pad's stick is)
        const glm::vec4 s = m * glm::vec4(pos, 1.0f);
        if (s.w > 0.0f)
        {
            const glm::vec2 v((ndc.x - s.x / s.w) * rAspect, ndc.y - s.y / s.w);
            float radPan = 0.0f, radTilt = 0.0f;
            cmm::DecomposeRotateMatrixPanTilt(g_cm.mat, &radPan, &radTilt);
            const float rad = std::atan2(v.y, v.x) + radPan - 1.5707964f;
            if (glm::length(v) > 1e-4f) d = glm::vec2(std::cos(rad), std::sin(rad));
        }
    }
    // (right on the ship: the last heading)
    if (glm::length(d) > 0.5f) s_radAimLast = std::atan2(d.y, d.x);
    *pradAim = s_radAimLast;
    *pfFire = fLeft;
    if (static const bool s_fPrint = std::getenv("SLY2N_HACKMOUSEPRINT") != nullptr; s_fPrint)
    {
        const glm::vec4 s = m * glm::vec4(pos, 1.0f);
        std::printf("hackmouse frame %d: cursor %.3f %.3f ship %.1f %.1f %.1f (ndc %.3f %.3f) aim %.3f fire %d\n", g_frameJoy,
                    ndc.x, ndc.y, pos.x, pos.y, pos.z, s.x / s.w, s.y / s.w, *pradAim, int(fLeft));
        // (every 10th frame: the shots in the world (cid 0x4A), their heading and distance from the ship)
        if (g_frameJoy % 10 == 0)
            for (LO* p : g_psw->mpcidaplo[0x4A])
                if (p->fInWorld)
                {
                    const glm::vec3 v = PosAloCenter(static_cast<ALO*>(p)) - pos;
                    std::printf("hackmouse frame %d:   shot %d heading %.3f distance %.0f\n", g_frameJoy, p->oid,
                                std::atan2(v.y, v.x), std::sqrt(v.x * v.x + v.y * v.y));
                }
    }
    return true;
}

void DrawHackCursor()
{
    if (!FHackMouseMode()) return;
    glm::vec2 ndc;
    bool fLeft = false;
    if (!FCursorNow(&ndc, &fLeft)) return;
    const glm::vec2 c = CanvasFromNdc(ndc);
    // a canvas pixel is 448 / 480 as wide as high (640 x 448 shown at 4:3): widths x 480 / 448 keep it round
    const float kx = 480.0f / 448.0f;
    const float z = 0.999f;   // (over the HUD)
    std::vector<glm::vec2> a;
    auto rect = [&](float x0, float y0, float x1, float y1) {   // (offsets from the centre, height pixels)
        const glm::vec2 p00(c.x + x0 * kx, c.y + y0), p10(c.x + x1 * kx, c.y + y0);
        const glm::vec2 p01(c.x + x0 * kx, c.y + y1), p11(c.x + x1 * kx, c.y + y1);
        a.insert(a.end(), {p00, p10, p01, p01, p10, p11});
    };
    auto ring = [&](float rOuter, float rInner) {
        constexpr int cseg = 40;
        for (int i = 0; i < cseg; ++i)
        {
            const float a0 = 6.2831853f * float(i) / cseg, a1 = 6.2831853f * float(i + 1) / cseg;
            auto pt = [&](float ang, float r) { return glm::vec2(c.x + std::cos(ang) * r * kx, c.y + std::sin(ang) * r); };
            const glm::vec2 o0 = pt(a0, rOuter), o1 = pt(a1, rOuter), i0 = pt(a0, rInner), i1 = pt(a1, rInner);
            a.insert(a.end(), {o0, o1, i0, i0, o1, i1});
        }
    };
    // the shape: a ring, four ticks outside it and a dot; firing draws the ring a little tighter and brighter
    const float r = fLeft ? 6.0f : 7.0f, w = 1.25f, tick0 = r + 2.0f, tick1 = r + 7.0f;
    auto shape = [&](float grow) {
        a.clear();
        ring(r + w + grow, r - w - grow);
        rect(-w - grow, -tick1 - grow, w + grow, -tick0 + grow);
        rect(-w - grow, tick0 - grow, w + grow, tick1 + grow);
        rect(-tick1 - grow, -w - grow, -tick0 + grow, w + grow);
        rect(tick0 - grow, -w - grow, tick1 + grow, w + grow);
        rect(-1.25f - grow, -1.25f - grow, 1.25f + grow, 1.25f + grow);
    };
    // a dark outline under it, then the light shape (GS colour units: 0x80 = 1)
    shape(1.0f);
    g_hud.FillScreenTris(a.data(), int(a.size()), glm::vec4(0.0f, 0.0f, 0.0f, 90.0f), z);
    shape(0.0f);
    const glm::vec4 rgba = fLeft ? glm::vec4(235.0f, 255.0f, 220.0f, 128.0f) : glm::vec4(120.0f, 255.0f, 90.0f, 128.0f);   // (the hack's green)
    g_hud.FillScreenTris(a.data(), int(a.size()), rgba, z + 0.0005f);
}
