// See controlspage.h.

#include "controlspage.h"
#include "region.h"
#include "porttext.h"

#include "pcinput.h"
#include "promptglyph.h"
#include "screenobj.h"
#include "splayer.h"

#include "../platform/button_prompts.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using cane::input::Button;
using cane::input::Family;
namespace st = cane::settings;

constexpr int IGROUP_PICTURE_FIRST = 20, IGROUP_PICTURE_LAST = 27, IGROUP_DOTS = 28;
constexpr int ACT_MOVE = -1, ACT_CAMERA = -2;   // (the sticks: not remappable)

// The labels: the string id, the action it describes (the PS2 button it points at), the character's title
struct LABEL { int id; int action; };
constexpr LABEL s_alabel[] = {
    // Sly (title 0x2C3)
    {0x2BE, PCA_R1}, {0x2C0, PCA_Select}, {0x2C4, PCA_Triangle}, {0x2BC, PCA_Circle}, {0x2C5, PCA_Cross},
    {0x2BF, ACT_CAMERA}, {0x2C1, PCA_Square}, {0x2BD, ACT_MOVE}, {0x2C2, PCA_Start},
    // Bentley (title 0x2AF)
    {0x2AA, PCA_R1}, {0x2AC, PCA_Select}, {0x2B0, PCA_Triangle}, {0x2A8, PCA_Circle}, {0x2B1, PCA_Cross},
    {0x2AB, ACT_CAMERA}, {0x2AD, PCA_Square}, {0x2A9, ACT_MOVE}, {0x2AE, PCA_Start},
    // Murray (title 0x2B9)
    {0x2B4, PCA_R1}, {0x2B6, PCA_Select}, {0x2BA, PCA_Triangle}, {0x2B2, PCA_Circle}, {0x2BB, PCA_Cross},
    {0x2B5, ACT_CAMERA}, {0x2B7, PCA_Square}, {0x2B3, ACT_MOVE}, {0x2B8, PCA_Start},
};
constexpr int s_aidTitle[] = {0x2C3, 0x2AF, 0x2B9};

const LABEL* PlabelOf(int id)
{
    id = IdTextUsa(id);   // (the labels by their USA ids; the Japanese text's ids differ: region.h)
    for (const LABEL& l : s_alabel)
        if (l.id == id) return &l;
    return nullptr;
}

// The PS2 picture's buttons (canvas units of the page, i.e. the line sprite's own units): the dots' places (group
// 28) and, for buttons without a dot, their sprites' places (the mirrored shoulder, the d-pad's arrows).
struct PSPOS { Button button; float x, y; };
constexpr PSPOS s_apspos[] = {
    {Button::LS, 259.0f, 339.0f}, {Button::RS, 351.0f, 341.0f}, {Button::Start, 337.0f, 274.0f},
    {Button::North, 416.0f, 257.0f}, {Button::East, 439.0f, 278.0f}, {Button::South, 418.0f, 304.0f},
    {Button::West, 382.0f, 291.0f}, {Button::RB, 424.0f, 206.0f}, {Button::Back, 287.5f, 274.7f},
    {Button::LB, 220.4f, 206.0f}, {Button::LT, 220.4f, 193.0f}, {Button::RT, 424.0f, 193.0f},
    {Button::LSPress, 259.0f, 339.0f}, {Button::RSPress, 351.0f, 341.0f},
    {Button::DpadUp, 217.0f, 269.5f}, {Button::DpadDown, 217.0f, 295.5f}, {Button::DpadLeft, 199.0f, 282.5f},
    {Button::DpadRight, 235.0f, 282.5f}, {Button::Dpad, 217.0f, 282.5f},
};

// The action a PS2 button stands for on the page (the game's default pad layout)
int ActionOfPsButton(Button b)
{
    switch (b)
    {
    case Button::South: return PCA_Cross;
    case Button::East: return PCA_Circle;
    case Button::West: return PCA_Square;
    case Button::North: return PCA_Triangle;
    case Button::RB: return PCA_R1;
    case Button::LB: return PCA_L1;
    case Button::RT: return PCA_R2;
    case Button::LT: return PCA_L2;
    case Button::Back: return PCA_Select;
    case Button::Start: return PCA_Start;
    case Button::LS: return ACT_MOVE;
    case Button::RS: return ACT_CAMERA;
    default: return -100;
    }
}

// The pad button an action is bound to now
Button BoundButton(int action)
{
    if (action == ACT_MOVE) return Button::LS;
    if (action == ACT_CAMERA) return Button::RS;
    if (action < 0 || action >= PCA_Max) return Button::Count;
    return cane::input::fromGlfwBinding(g_pcinput.gamepad[size_t(action)]);
}
Button DefaultButton(int action)
{
    if (action == ACT_MOVE) return Button::LS;
    if (action == ACT_CAMERA) return Button::RS;
    if (action < 0 || action >= PCA_Max) return Button::Count;
    return cane::input::fromGlfwBinding(st::kDefaultGamepadBindings[action]);
}

// ---- the device pictures: each button's place in the picture's SVG units (64 x 64) and the icons' sizes. The
// shoulders sit on the body's top edge at its shoulders (the bumper overlapping the edge, the trigger right above it,
// touching), as on the real pads.
struct DEVLAYOUT
{
    Family family;
    float aPos[cane::input::kButtonCount][2];
    float faceSpread;   // (the face buttons drawn spread out from their centre when the holes are tiny)
    // icon sizes (SVG units): face buttons, sticks, shoulders, View / Menu (Back / Start), the d-pad
    float duFace, duStick, duShoulder, duMenu, duDpad;
};
// South East West North LB RB LT RT Back Start LS RS LSPress RSPress DpadUp DpadDown DpadLeft DpadRight Dpad Guide
const DEVLAYOUT s_adevlayout[] = {
    {Family::Xbox, {{42.9f, 28.9f}, {46.9f, 24.9f}, {38.9f, 24.9f}, {42.9f, 20.9f}, {17.0f, 16.4f}, {47.0f, 16.4f},
                    {17.0f, 12.0f}, {47.0f, 12.0f}, {28.0f, 24.8f}, {35.6f, 24.8f}, {20.4f, 23.4f}, {35.9f, 31.9f},
                    {20.4f, 23.4f}, {35.9f, 31.9f}, {25.5f, 30.0f}, {25.5f, 34.8f}, {23.1f, 32.4f}, {27.9f, 32.4f},
                    {25.5f, 32.4f}, {31.9f, 19.9f}}, 1.0f, 4.4f, 6.2f, 5.0f, 3.6f, 5.6f},
    {Family::Nintendo, {{44.9f, 27.9f}, {48.9f, 24.9f}, {40.9f, 24.9f}, {44.9f, 21.9f}, {17.5f, 16.1f}, {46.5f, 16.1f},
                        {17.5f, 11.7f}, {46.5f, 11.7f}, {28.0f, 21.5f}, {36.0f, 21.5f}, {18.9f, 23.9f}, {37.9f, 31.9f},
                        {18.9f, 23.9f}, {37.9f, 31.9f}, {24.9f, 29.5f}, {24.9f, 34.3f}, {22.5f, 31.9f}, {27.3f, 31.9f},
                        {24.9f, 31.9f}, {32.0f, 25.0f}}, 1.0f, 4.4f, 6.2f, 5.0f, 3.6f, 5.6f},
    // (composed by tools/pack_prompts.py: the real Deck's layout, its sticks and trackpads)
    {Family::SteamDeck, {{55.2f, 28.3f}, {57.2f, 26.3f}, {53.2f, 26.3f}, {55.2f, 24.3f}, {10.0f, 19.6f}, {54.0f, 19.6f},
                         {10.0f, 16.1f}, {54.0f, 16.1f}, {16.1f, 22.3f}, {47.9f, 22.3f}, {15.2f, 27.6f}, {48.8f, 27.6f},
                         {15.2f, 27.6f}, {48.8f, 27.6f}, {8.8f, 24.3f}, {8.8f, 28.3f}, {6.8f, 26.3f}, {10.8f, 26.3f},
                         {8.8f, 26.3f}, {16.2f, 41.85f}}, 1.2f, 3.2f, 5.6f, 3.6f, 4.4f, 6.4f},
    {Family::SteamController, {{37.4f, 35.4f}, {40.4f, 32.5f}, {34.4f, 32.5f}, {37.4f, 29.4f}, {17.5f, 16.4f}, {46.5f, 16.4f},
                               {17.5f, 12.4f}, {46.5f, 12.4f}, {28.5f, 24.0f}, {35.5f, 24.0f}, {26.4f, 32.4f}, {44.9f, 24.9f},
                               {26.4f, 32.4f}, {44.9f, 24.9f}, {18.9f, 21.5f}, {18.9f, 28.3f}, {15.5f, 24.9f}, {22.3f, 24.9f},
                               {18.9f, 24.9f}, {32.0f, 19.0f}}, 1.3f, 4.0f, 5.6f, 4.5f, 3.8f, 5.0f},
    {Family::PS4, {{46.4f, 29.4f}, {49.4f, 26.4f}, {43.4f, 26.4f}, {46.4f, 23.4f}, {17.9f, 16.4f}, {46.1f, 16.4f},
                   {17.9f, 12.4f}, {46.1f, 12.4f}, {23.4f, 19.5f}, {40.6f, 19.5f}, {24.0f, 31.9f}, {40.0f, 31.9f},
                   {24.0f, 31.9f}, {40.0f, 31.9f}, {17.9f, 22.5f}, {17.9f, 29.4f}, {14.5f, 25.9f}, {21.4f, 25.9f},
                   {17.9f, 25.9f}, {32.0f, 31.0f}}, 1.15f, 4.0f, 5.6f, 4.5f, 4.0f, 5.0f},
    {Family::PS5, {{46.4f, 29.4f}, {49.4f, 26.4f}, {43.4f, 26.4f}, {46.4f, 23.4f}, {17.9f, 16.5f}, {46.1f, 16.5f},
                   {17.9f, 12.5f}, {46.1f, 12.5f}, {22.6f, 18.8f}, {41.4f, 18.8f}, {25.0f, 31.9f}, {39.0f, 31.9f},
                   {25.0f, 31.9f}, {39.0f, 31.9f}, {17.9f, 22.5f}, {17.9f, 29.4f}, {14.5f, 25.9f}, {21.4f, 25.9f},
                   {17.9f, 25.9f}, {32.0f, 31.0f}}, 1.15f, 4.0f, 5.6f, 4.5f, 4.0f, 5.0f},
    // (Kenney's 2026 silhouette with the sticks added: the d-pad / face buttons out at the top corners, a stick above
    // each square trackpad)
    {Family::SteamController2026, {{47.2f, 28.0f}, {50.2f, 25.0f}, {44.2f, 25.0f}, {47.2f, 22.0f}, {17.5f, 16.3f},
                                   {46.5f, 16.3f}, {17.5f, 12.0f}, {46.5f, 12.0f}, {28.6f, 19.8f}, {35.4f, 19.8f},
                                   {25.2f, 26.0f}, {38.8f, 26.0f}, {25.2f, 26.0f}, {38.8f, 26.0f}, {16.0f, 22.6f},
                                   {16.0f, 27.4f}, {13.6f, 25.0f}, {18.4f, 25.0f}, {16.0f, 25.0f}, {32.0f, 24.0f}},
     1.0f, 3.4f, 5.6f, 4.4f, 4.4f, 7.8f},
};
const DEVLAYOUT* PdevlayoutOf(Family f)
{
    for (const DEVLAYOUT& d : s_adevlayout)
        if (d.family == f) return &d;
    return nullptr;
}
// an icon's size (SVG units) by button
float DuIcon(const DEVLAYOUT& lay, Button b)
{
    switch (b)
    {
    case Button::LS: case Button::RS: case Button::LSPress: case Button::RSPress: return lay.duStick;
    case Button::Dpad: return lay.duDpad;
    case Button::LB: case Button::RB: case Button::LT: case Button::RT: return lay.duShoulder;
    case Button::Back: case Button::Start: case Button::Guide: return lay.duMenu;
    case Button::DpadUp: case Button::DpadDown: case Button::DpadLeft: case Button::DpadRight: return 0.55f * lay.duDpad;
    default: return lay.duFace;
    }
}

// ---- the page as found this frame
enum MODE { MODE_Off, MODE_PsRemap, MODE_Device, MODE_Keyboard };
struct LINE { glm::vec2 aLabel, aButton; int action = -100; float w = 1.0f; glm::vec4 rgba{0.0f}; };
struct KBLABEL { TEXTDRAW td; std::string text; int action; };
// A label's slot on the page (its text element): DrawTextBox's box and the label end of its leader line
struct SLOT
{
    int id = -1, action = -100;
    float m4 = 0, m5 = 0, w = 0, h = 0, size = 16;   // (the text element's slot origin, box, size)
    int align = 0;
    int iline = -1;                                  // its leader line (s_page.aline), -1 none
    std::string text;                                // (the label's text, the bound buttons named)
};
// The device page's picture, computed once per page draw (LayoutDevice)
struct DEVDRAW
{
    bool fValid = false;
    glm::vec2 o{0.0f};                   // the picture cell's top left (frame pixels)
    float sx = 0, sy = 0, uw = 0, uh = 0;
    std::vector<std::pair<Button, glm::vec2>> aicon;   // (the icons drawn and their centres)
    struct LEG { glm::vec2 a, bend, b; bool fBend; size_t iline; };
    std::vector<LEG> aleg;               // the leader lines (frame pixels)
};
struct PAGE
{
    MODE mode = MODE_Off;
    Family family = Family::PlayStation;
    bool fDrawn = false;                 // (the replacement drawn at the line sprite)
    const SELEM* plineSprite = nullptr;
    std::vector<LINE> aline;
    const SELEM* pdot = nullptr;         // (a dot's sprite and state: the replacement's dots look the same)
    HUDSTATE stDot;
    glm::vec2 posDot0{0.0f};
    HUDSTATE stLine;                     // (the line sprite's state: canvas units -> frame pixels)
    glm::vec2 lo{0.0f}, hi{0.0f};        // the PS2 picture's box (frame pixels)
    float zPicture = 0;
    float alpha = 1;                     // the page's fade (0..1)
    std::vector<KBLABEL> akb;
    std::vector<SLOT> aslot;             // the labels' slots
    std::vector<int> aislotTarget;       // (device pages) the slot each slot's label is drawn in (re-laid so lines do not cross)
    std::vector<std::pair<TEXTDRAW, std::string>> amoved;   // labels drawn at another slot (at ControlsPageEnd)
    DEVDRAW dev;
};
PAGE s_page;

bool FOff()
{
    static const bool s_f = [] { const char* sz = std::getenv("SLY2N_CTLPAGE"); return sz && sz[0] == '0'; }();
    return s_f;
}

bool FEnvOff(const char* szName)
{
    const char* sz = std::getenv(szName);
    return sz && sz[0] == '0';
}

bool FLineSprite(const CTLPIECE& p)
{
    return p.igroup == 0 && p.pelem && p.pelem->kind == SELEMK_Sprite && std::fabs(p.mx) < 0.01f && std::fabs(p.my) < 0.01f &&
           p.pelem->apoly.size() >= 6;
}

std::string StrBindingName(int binding)
{
    return StrPortKeyName(binding);   // (the game's language where the port's table has the key: porttext.h)
}

// "R1 button" / "L3 button" / "R3 button" in a label: the button bound to that action now (the pad's name, or the
// key's)
// The other languages' labels name the button in their own words around it ("Touche R1 : ...", "R1-Taste: ...",
// "R1ボタン:..."): the button's code is replaced by the bound one's name, and a row's own button with its colon is left
// out on the keyboard's list (its keycaps show it).
std::string StrWithBoundNamesOther(std::string text, MODE mode, Family family, int actionLabel)
{
    struct CODE { const char* sz; int action; };
    static const CODE s_acode[] = {{"R1", PCA_R1}, {"L3", PCA_L3}, {"R3", PCA_R3}};
    auto fAlnum = [](char ch) { return std::isalnum(uint8_t(ch)) != 0; };
    for (const CODE& c : s_acode)
    {
        size_t at = 0;
        while ((at = text.find(c.sz, at)) != std::string::npos && ((at > 0 && fAlnum(text[at - 1])) || fAlnum(text[at + 2] ? text[at + 2] : ' ')))
            ++at;
        if (at == std::string::npos) continue;
        if (mode == MODE_Keyboard && c.action == actionLabel)
        {
            // the label's first part names the button ("Touche R1 : Courir vite"): just its action
            const size_t colon = text.find(':');
            if (colon != std::string::npos && colon > at && colon < 24 && text.find('(') > colon)
            {
                size_t i = colon + 1;
                while (i < text.size() && text[i] == ' ') ++i;
                text.erase(0, i);
                continue;
            }
        }
        const std::string name = mode == MODE_Keyboard ? StrBindingName(g_pcinput.keyboard[size_t(c.action)])
                                                       : StrToGameText(cane::input::buttonLabel(BoundButton(c.action), family));
        text.replace(at, 2, name);
    }
    return text;
}

std::string StrWithBoundNames(std::string text, MODE mode, Family family, int actionLabel)
{
    if (!FPortEnglish()) return StrWithBoundNamesOther(std::move(text), mode, family, actionLabel);
    struct TOKEN { const char* sz; int action; };
    static const TOKEN s_atoken[] = {{"R1 button", PCA_R1}, {"L3 button", PCA_L3}, {"R3 button", PCA_R3}};
    for (const TOKEN& t : s_atoken)
    {
        const size_t at = text.find(t.sz);
        if (at == std::string::npos) continue;
        std::string name;
        if (mode == MODE_Keyboard)
        {
            // (the row's own keycaps show it: "R1 button: Run Fast" is just "Run Fast")
            const size_t cch = std::strlen(t.sz);
            if (t.action == actionLabel && at == 0 && text.compare(cch, 2, ": ") == 0) { text.erase(0, cch + 2); continue; }
            name = StrBindingName(g_pcinput.keyboard[size_t(t.action)]);
        }
        else
        {
            // ("RB button", but "Stick click" without it)
            name = cane::input::buttonLabel(BoundButton(t.action), family);
            if (name.find(' ') == std::string::npos) name += " button";
        }
        text.replace(at, std::strlen(t.sz), name);
    }
    return text;
}

// the frame-pixel width of an icon h pixels high (a canvas pixel is 448 / 480 as wide as high)
float DxOfDy(float h) { return h * (480.0f / 448.0f); }

void DrawIcon(unsigned tex, const cane::input::IconUV& uv, glm::vec2 c, float h, float a, float z)
{
    if (!tex || !uv.valid) return;
    const float hx = 0.5f * DxOfDy(h);
    g_hud.DrawTexQuad(tex, c.x - hx, c.y - 0.5f * h, c.x + hx, c.y + 0.5f * h, uv.u0, uv.v0, uv.u1, uv.v1,
                      glm::vec4(128.0f, 128.0f, 128.0f, 128.0f * a), z);
}

void DrawDot(glm::vec2 pos, float z)
{
    if (!s_page.pdot) return;
    HUDSTATE st = s_page.stDot;
    // (frame pixels to GS units: x / 1.25, y / 2)
    st.m[8] += (pos.x - s_page.posDot0.x) / 1.25f;
    st.m[9] += (pos.y - s_page.posDot0.y) / 2.0f;
    st.depth = z * 16777216.0f;
    g_hud.DrawSprite(*s_page.pdot, st);
}

void DrawLines(const std::vector<glm::vec2>& apos, float z)
{
    for (size_t i = 0; i < s_page.aline.size(); ++i)
    {
        const LINE& l = s_page.aline[i];
        g_hud.FillScreenLine(l.aLabel, apos[i], l.w, l.rgba, z);
    }
    for (size_t i = 0; i < s_page.aline.size(); ++i) DrawDot(apos[i], z);
}

// The PS2 picture with remapped pad bindings: the lines to the bound buttons.
void DrawPsRemap()
{
    std::vector<glm::vec2> apos;
    for (const LINE& l : s_page.aline)
    {
        const Button b = BoundButton(l.action);
        glm::vec2 p = l.aButton;
        for (const PSPOS& ps : s_apspos)
            if (ps.button == b) { p = CHud::ScreenOfLocal(s_page.stLine, glm::vec2(ps.x, ps.y)); break; }
        apos.push_back(p);
    }
    DrawLines(apos, s_page.zPicture);
}

// ---- leader-line routes on the device pages: each line from its label straight to its button's icon or, where that
// would cross another line or run through another button's icon, with one bend. Computed in square units (frame x
// scaled by 448 / 480, so circles stay round).
struct ROUTE { glm::vec2 a{0.0f}, bend{0.0f}, b{0.0f}; bool fBend = false; };
struct OBSTACLE { glm::vec2 c; float r, rArt, rim; };   // (kept clear (r) of other lines, never over its art; lines end on the rim)

float DistToSeg(glm::vec2 p, glm::vec2 a, glm::vec2 b)
{
    const glm::vec2 d = b - a;
    const float l2 = glm::dot(d, d);
    const float t = l2 > 1e-6f ? std::clamp(glm::dot(p - a, d) / l2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + t * d));
}
bool FSegsCross(glm::vec2 p, glm::vec2 q, glm::vec2 r, glm::vec2 s)
{
    auto cross = [](glm::vec2 u, glm::vec2 v) { return u.x * v.y - u.y * v.x; };
    const float d = cross(q - p, s - r);
    if (std::fabs(d) < 1e-6f) return false;
    const float t = cross(r - p, s - r) / d, u = cross(r - p, q - p) / d;
    return t > 0.0f && t < 1.0f && u > 0.0f && u < 1.0f;
}
int CSegs(const ROUTE& r, glm::vec2 aseg[2][2])
{
    if (!r.fBend) { aseg[0][0] = r.a; aseg[0][1] = r.b; return 1; }
    aseg[0][0] = r.a; aseg[0][1] = r.bend; aseg[1][0] = r.bend; aseg[1][1] = r.b;
    return 2;
}
// the route's end on the icon's rim, towards where the line comes from
ROUTE RouteTo(glm::vec2 label, const glm::vec2* pbend, glm::vec2 c, float r)
{
    ROUTE rt;
    rt.a = label;
    rt.fBend = pbend != nullptr;
    if (pbend) rt.bend = *pbend;
    const glm::vec2 from = pbend ? *pbend : label;
    glm::vec2 d = from - c;
    const float len = glm::length(d);
    rt.b = len > r ? c + d * (r / len) : from;
    return rt;
}
// (the cost of a route against the others: crossings, other buttons' icons run through, length and bends)
// a segment through a box (x0 y0 x1 y1)
bool FSegHitsBox(glm::vec2 a, glm::vec2 b, glm::vec4 box)
{
    if (std::max(a.x, b.x) < box.x || std::min(a.x, b.x) > box.z || std::max(a.y, b.y) < box.y || std::min(a.y, b.y) > box.w)
        return false;
    for (int i = 0; i <= 16; ++i)
    {
        const glm::vec2 p = a + (b - a) * (float(i) / 16.0f);
        if (p.x > box.x && p.x < box.z && p.y > box.y && p.y < box.w) return true;
    }
    return false;
}
std::vector<glm::vec4> s_aboxRoute;   // (the labels' boxes in square units, for CostOfRoute)

float CostOfRoute(const ROUTE& rt, size_t iSelf, const std::vector<ROUTE>& aroute, const std::vector<int>& aiobs,
                  const std::vector<OBSTACLE>& aobs)
{
    glm::vec2 aseg[2][2], asegO[2][2];
    const int cseg = CSegs(rt, aseg);
    float cost = glm::length(rt.bend - rt.a) * (rt.fBend ? 1.0f : 0.0f) + glm::length(rt.b - (rt.fBend ? rt.bend : rt.a));
    if (rt.fBend)
    {
        cost += 25.0f;
        // (a hairpin reads badly: the legs should turn by less than ~110 degrees)
        const glm::vec2 u = rt.a - rt.bend, v = rt.b - rt.bend;
        const float lu = glm::length(u), lv = glm::length(v);
        if (lu > 0.0f && lv > 0.0f && glm::dot(u, v) / (lu * lv) > 0.35f) cost += 400.0f;
    }
    for (size_t j = 0; j < aroute.size(); ++j)
    {
        if (j == iSelf) continue;
        const int csegO = CSegs(aroute[j], asegO);
        const bool fSameButton = aiobs[j] == aiobs[iSelf];
        for (int k = 0; k < cseg; ++k)
            for (int m = 0; m < csegO; ++m)
                if (!fSameButton && FSegsCross(aseg[k][0], aseg[k][1], asegO[m][0], asegO[m][1])) cost += 2000.0f;
    }
    for (size_t o = 0; o < aobs.size(); ++o)
    {
        if (int(o) == aiobs[iSelf]) continue;
        for (int k = 0; k < cseg; ++k)
        {
            const float d = DistToSeg(aobs[o].c, aseg[k][0], aseg[k][1]);
            if (d < aobs[o].rArt) cost += 1200.0f;
            else if (d < aobs[o].r) cost += 150.0f;
        }
    }
    // (a bend inside another icon reads as pointing at it)
    if (rt.fBend)
        for (size_t o = 0; o < aobs.size(); ++o)
            if (int(o) != aiobs[iSelf] && glm::distance(rt.bend, aobs[o].c) < 1.6f * aobs[o].r) cost += 600.0f;
    // (through a label's text; the line's own label: only its first leg's far part)
    for (const glm::vec4& box : s_aboxRoute)
    {
        const bool fOwn = rt.a.x >= box.x - 6.0f && rt.a.x <= box.z + 6.0f && rt.a.y >= box.y - 6.0f && rt.a.y <= box.w + 6.0f;
        for (int k = 0; k < cseg; ++k)
        {
            glm::vec2 p = aseg[k][0];
            if (fOwn && k == 0) p = p + (aseg[k][1] - p) * std::min(1.0f, 8.0f / std::max(glm::distance(p, aseg[k][1]), 1e-3f));
            if (FSegHitsBox(p, aseg[k][1], box)) cost += 1500.0f;
        }
    }
    if (!std::isfinite(cost) && std::getenv("SLY2N_CTLPAGEDBG"))
        std::printf("  nonfinite route a (%.1f %.1f) bend %d (%.1f %.1f) b (%.1f %.1f)\n", rt.a.x, rt.a.y, int(rt.fBend), rt.bend.x,
                    rt.bend.y, rt.b.x, rt.b.y);
    return std::isfinite(cost) ? cost : 1e9f;
}
// abend: the places a line may bend at besides those around its own button (a grid over the picture)
std::vector<ROUTE> RouteLines(const std::vector<glm::vec2>& alabel, const std::vector<int>& aiobs,
                              const std::vector<OBSTACLE>& aobs, const std::vector<glm::vec2>& abend)
{
    std::vector<ROUTE> aroute;
    for (size_t i = 0; i < alabel.size(); ++i)
        aroute.push_back(RouteTo(alabel[i], nullptr, aobs[size_t(aiobs[i])].c, aobs[size_t(aiobs[i])].rim));
    const std::vector<ROUTE> arouteStraight = aroute;
    std::vector<ROUTE> arouteBest;
    float costBestAll = 1e30f;
    // each line in turn takes its cheapest route given the others (straight, or one bend: around its button or on a
    // grid over the picture); from the straight lines, in both orders, the cheaper result kept
    for (int order = 0; order < 2; ++order)
    {
    aroute = arouteStraight;
    for (int pass = 0; pass < 8; ++pass)
    {
        bool fChanged = false;
        for (size_t ii = 0; ii < aroute.size(); ++ii)
        {
            const size_t i = order == 0 ? ii : aroute.size() - 1 - ii;
            const OBSTACLE& o = aobs[size_t(aiobs[i])];
            ROUTE best = RouteTo(alabel[i], nullptr, o.c, o.rim);
            float costBest = CostOfRoute(best, i, aroute, aiobs, aobs);
            auto consider = [&](glm::vec2 bend) {
                const ROUTE rt = RouteTo(alabel[i], &bend, o.c, o.rim);
                const float cost = CostOfRoute(rt, i, aroute, aiobs, aobs);
                if (cost < costBest - 0.01f) { costBest = cost; best = rt; }
            };
            for (float k : {1.9f, 2.8f, 3.8f, 5.0f})
                for (int ang = 0; ang < 24; ++ang)
                {
                    const float t = float(ang) * (6.2831853f / 24.0f);
                    consider(o.c + k * o.rim * glm::vec2(std::cos(t), std::sin(t)));
                }
            for (const glm::vec2& bend : abend) consider(bend);
            const bool fSame = best.fBend == aroute[i].fBend && glm::distance(best.b, aroute[i].b) < 0.01f &&
                               (!best.fBend || glm::distance(best.bend, aroute[i].bend) < 0.01f);
            if (!fSame) { aroute[i] = best; fChanged = true; }
        }
        if (!fChanged) break;
    }
    float costAll = 0.0f;
    for (size_t i = 0; i < aroute.size(); ++i) costAll += CostOfRoute(aroute[i], i, aroute, aiobs, aobs);
    if (std::getenv("SLY2N_CTLPAGEDBG")) std::printf("  routes order %d cost %.1f\n", order, costAll);
    if (arouteBest.empty() || costAll < costBestAll) { costBestAll = costAll; arouteBest = aroute; }
    }
    return arouteBest;
}

// (the buttons shown on a device page: those the page's labels use, the shoulders and the d-pad)
constexpr Button s_abShown[] = {Button::South, Button::East, Button::West, Button::North, Button::LB, Button::RB, Button::LT,
                                Button::RT, Button::Back, Button::Start, Button::LS, Button::RS, Button::Dpad};
constexpr float kSq = 448.0f / 480.0f;   // (square units: frame x * 448 / 480, so circles stay round)

// A label drawn in a slot: its text box (frame pixels, x0 y0 x1 y1), the text wrapped to the slot's width
glm::vec4 BoxOfLabelIn(const SLOT& slot, const std::string& text, const FONTBRX* pfont, const FONTBRX* pfontIcons)
{
    int cline = 1;
    if (pfont)
        for (char ch : StrWrapped(*pfont, pfontIcons, text, slot.size / 51.2f, slot.w)) cline += ch == '\n';
    const float y0 = slot.m5 / 1.1f;
    return glm::vec4(slot.m4, y0, slot.m4 + slot.w, y0 + float(cline) * slot.size / 1.1f);
}
float AreaOverlap(glm::vec4 a, glm::vec4 b)
{
    return std::max(0.0f, std::min(a.z, b.z) - std::max(a.x, b.x)) * std::max(0.0f, std::min(a.w, b.w) - std::max(a.y, b.y));
}

// The device's picture in the PS2 picture's place, its icons, the labels' slots (a label may move to another label's
// slot when that untangles the lines) and the leader lines' routes.
void LayoutDevice(const FONTBRX* pfont, const FONTBRX* pfontIcons)
{
    DEVDRAW& dev = s_page.dev;
    dev = DEVDRAW{};
    const auto& atlas = cane::input::promptAtlas();
    const cane::input::PromptDeviceRect* pdev = atlas.device(s_page.family);
    const DEVLAYOUT* play = PdevlayoutOf(s_page.family);
    if (!pdev || !play) return;
    const float kpx = cane::input::PromptAtlas::devicePxPerUnit();
    dev.uw = float(pdev->body.w) / kpx;
    dev.uh = float(pdev->body.h) / kpx;
    // fit: the device as wide as the PS2 picture's body (its box without the shoulders), centred on it
    const glm::vec2 boxLo = s_page.lo, boxHi = s_page.hi;
    dev.sx = (boxHi.x - boxLo.x) * 0.92f / dev.uw;               // frame px per unit across
    dev.sy = dev.sx * (448.0f / 480.0f);                         // and down (square on screen)
    const float hMax = (boxHi.y - boxLo.y) * 0.88f;
    if (dev.uh * dev.sy > hMax) { dev.sy = hMax / dev.uh; dev.sx = dev.sy * (480.0f / 448.0f); }
    const glm::vec2 c(0.5f * (boxLo.x + boxHi.x), boxLo.y + 0.56f * (boxHi.y - boxLo.y));
    dev.o = glm::vec2(c.x - 0.5f * dev.uw * dev.sx, c.y - 0.5f * dev.uh * dev.sy);
    auto pos = [&](float ux, float uy) { return glm::vec2(dev.o.x + (ux - pdev->ux0) * dev.sx, dev.o.y + (uy - pdev->uy0) * dev.sy); };
    const glm::vec2 face(0.25f * (play->aPos[0][0] + play->aPos[1][0] + play->aPos[2][0] + play->aPos[3][0]),
                         0.25f * (play->aPos[0][1] + play->aPos[1][1] + play->aPos[2][1] + play->aPos[3][1]));
    std::vector<OBSTACLE> aobs;
    for (Button b : s_abShown)
    {
        const float* pu = play->aPos[int(b)];
        glm::vec2 u(pu[0], pu[1]);
        if (int(b) <= int(Button::North)) u = face + (u - face) * play->faceSpread;
        const glm::vec2 p = pos(u.x, u.y);
        dev.aicon.push_back({b, p});
        // (the icons' art fills ~85% of their square: kept clear with a margin; lines end inside the rim)
        const float h = DuIcon(*play, b) * dev.sy;
        aobs.push_back({glm::vec2(p.x * kSq, p.y), 0.5f * h + 3.0f, 0.44f * h, 0.36f * h});
    }
    dev.fValid = true;

    // the lines' buttons (an unbound action: no line)
    std::vector<int> aiobsOfLine(s_page.aline.size(), -1);
    for (size_t i = 0; i < s_page.aline.size(); ++i)
    {
        Button b = BoundButton(s_page.aline[i].action);
        if (b == Button::LSPress) b = Button::LS;
        if (b == Button::RSPress) b = Button::RS;
        for (size_t k = 0; k < std::size(s_abShown); ++k)
            if (s_abShown[k] == b) aiobsOfLine[i] = int(k);
    }

    // the labels' slots: the right-hand and bottom slots (Run, the face buttons, the camera) may trade labels; every
    // order is tried with straight lines and the cheapest kept (crossings, lines through icons or text, text running
    // into other text or off the page, and each moved label)
    const size_t cslot = s_page.aslot.size();
    std::vector<int> aislotPerm;
    for (size_t i = 0; i < cslot; ++i)
    {
        const int act = s_page.aslot[i].action;
        if (s_page.aslot[i].iline >= 0 &&
            (act == PCA_R1 || act == PCA_Triangle || act == PCA_Circle || act == PCA_Cross || act == PCA_Square || act == ACT_CAMERA))
            aislotPerm.push_back(int(i));
    }
    // (the box of slot s's label drawn in slot t)
    std::vector<glm::vec4> abox(cslot * cslot);
    for (size_t i = 0; i < cslot; ++i)
        for (size_t t = 0; t < cslot; ++t) abox[i * cslot + t] = BoxOfLabelIn(s_page.aslot[t], s_page.aslot[i].text, pfont, pfontIcons);
    const glm::vec4 boxDevice(pos(pdev->ux0, pdev->uy0).x, pos(pdev->ux0, pdev->uy0).y, pos(pdev->ux0 + dev.uw, 0).x,
                              pos(0, pdev->uy0 + dev.uh).y);
    std::vector<int> atarget(cslot);
    for (size_t i = 0; i < cslot; ++i) atarget[i] = int(i);
    auto costOf = [&](const std::vector<int>& at) {
        float cost = 0.0f;
        std::vector<glm::vec4> aboxSq;
        for (size_t i = 0; i < cslot; ++i)
        {
            const glm::vec4 box = abox[i * cslot + size_t(at[i])];
            if (at[i] != int(i)) cost += 150.0f;
            if (box.w > 446.0f) cost += 4000.0f;
            for (size_t j = i + 1; j < cslot; ++j)
                if (AreaOverlap(box, abox[j * cslot + size_t(at[j])]) > 4.0f) cost += 4000.0f;
            if (at[i] != int(i) && AreaOverlap(box, boxDevice) > 4.0f) cost += 3000.0f;
            aboxSq.push_back(glm::vec4(box.x * kSq + 2.0f, box.y + 2.0f, box.z * kSq - 2.0f, box.w - 2.0f));
        }
        s_aboxRoute = aboxSq;
        std::vector<ROUTE> aroute;
        std::vector<int> aiobs;
        for (size_t i = 0; i < cslot; ++i)
        {
            const SLOT& from = s_page.aslot[i];
            const SLOT& to = s_page.aslot[size_t(at[i])];
            if (from.iline < 0 || to.iline < 0 || aiobsOfLine[size_t(from.iline)] < 0) continue;
            const OBSTACLE& o = aobs[size_t(aiobsOfLine[size_t(from.iline)])];
            const glm::vec2 a = s_page.aline[size_t(to.iline)].aLabel;
            aroute.push_back(RouteTo(glm::vec2(a.x * kSq, a.y), nullptr, o.c, o.rim));
            aiobs.push_back(aiobsOfLine[size_t(from.iline)]);
        }
        for (size_t i = 0; i < aroute.size(); ++i) cost += CostOfRoute(aroute[i], i, aroute, aiobs, aobs);
        return cost;
    };
    if (!FEnvOff("SLY2N_CTLRELAY") && aislotPerm.size() >= 2 && aislotPerm.size() <= 7)
    {
        std::vector<int> aperm = aislotPerm;   // (sorted: the identity first)
        std::vector<int> at = atarget;
        float costBest = costOf(atarget);
        do
        {
            for (size_t k = 0; k < aislotPerm.size(); ++k) at[size_t(aislotPerm[k])] = aperm[k];
            const float cost = costOf(at);
            if (cost < costBest - 1.0f) { costBest = cost; atarget = at; }
        } while (std::next_permutation(aperm.begin(), aperm.end()));
    }
    s_page.aislotTarget = atarget;

    // the routes, from the label ends of the slots the labels are drawn in
    s_aboxRoute.clear();
    for (size_t i = 0; i < cslot; ++i)
    {
        const glm::vec4 box = abox[i * cslot + size_t(atarget[i])];
        s_aboxRoute.push_back(glm::vec4(box.x * kSq + 2.0f, box.y + 2.0f, box.z * kSq - 2.0f, box.w - 2.0f));
    }
    std::vector<glm::vec2> alabel;
    std::vector<int> aiobs;
    std::vector<size_t> ailine;
    for (size_t i = 0; i < s_page.aline.size(); ++i)
    {
        if (aiobsOfLine[i] < 0) continue;
        glm::vec2 a = s_page.aline[i].aLabel;
        for (size_t k = 0; k < cslot; ++k)
            if (s_page.aslot[k].iline == int(i) && s_page.aslot[size_t(atarget[k])].iline >= 0)
                a = s_page.aline[size_t(s_page.aslot[size_t(atarget[k])].iline)].aLabel;
        alabel.push_back(glm::vec2(a.x * kSq, a.y));
        aiobs.push_back(aiobsOfLine[i]);
        ailine.push_back(i);
    }
    std::vector<glm::vec2> abend;
    {
        const glm::vec2 lo = pos(pdev->ux0, pdev->uy0), hi = pos(pdev->ux0 + dev.uw, pdev->uy0 + dev.uh * 0.75f);
        for (int j = 0; j <= 16; ++j)
            for (int i = 0; i <= 24; ++i)
                abend.push_back(glm::vec2((lo.x + (hi.x - lo.x) * float(i) / 24.0f) * kSq, lo.y + (hi.y - lo.y) * float(j) / 16.0f));
    }
    const std::vector<ROUTE> aroute = RouteLines(alabel, aiobs, aobs, abend);
    auto frame = [](glm::vec2 p) { return glm::vec2(p.x / kSq, p.y); };
    for (size_t k = 0; k < aroute.size(); ++k)
        dev.aleg.push_back({frame(aroute[k].a), frame(aroute[k].bend), frame(aroute[k].b), aroute[k].fBend, ailine[k]});
    if (static const char* s_sz = std::getenv("SLY2N_CTLPAGEDBG"); s_sz)
    {
        for (size_t i = 0; i < cslot; ++i)
            if (atarget[i] != int(i)) std::printf("  label 0x%X drawn in 0x%X's slot\n", s_page.aslot[i].id, s_page.aslot[size_t(atarget[i])].id);
        for (size_t k = 0; k < aroute.size(); ++k)
            std::printf("  route action %d button %d bend %d\n", s_page.aline[ailine[k]].action, int(s_abShown[aiobs[k]]),
                        int(aroute[k].fBend));
    }
}

// Draws the device page's picture (LayoutDevice).
void DrawDevice()
{
    const DEVDRAW& dev = s_page.dev;
    if (!dev.fValid) return;
    const auto& atlas = cane::input::promptAtlas();
    const cane::input::PromptDeviceRect* pdev = atlas.device(s_page.family);
    const DEVLAYOUT* play = PdevlayoutOf(s_page.family);
    const unsigned texIcons = atlas.texture(s_page.family);
    const float z = s_page.zPicture, a = s_page.alpha;
    const unsigned tex = atlas.deviceTexture();
    auto quad = [&](const cane::input::PromptRect& r, glm::vec4 rgba) {
        const cane::input::IconUV uv = cane::input::PromptAtlas::deviceUV(r);
        g_hud.DrawTexQuad(tex, dev.o.x, dev.o.y, dev.o.x + dev.uw * dev.sx, dev.o.y + dev.uh * dev.sy, uv.u0, uv.v0, uv.u1, uv.v1,
                          rgba, z);
    };
    // the PS2 picture's colours: a black outline, a dark slate body
    quad(pdev->under, glm::vec4(4.0f, 4.0f, 6.0f, 128.0f * a));
    quad(pdev->body, glm::vec4(24.0f, 25.0f, 31.0f, 128.0f * a));
    for (const auto& [b, p] : dev.aicon) DrawIcon(texIcons, atlas.uv(s_page.family, b), p, DuIcon(*play, b) * dev.sy, a, z);
    // the leader lines (a bend: the first leg overshoots it by half the width, a closed corner), then their dots
    for (const DEVDRAW::LEG& leg : dev.aleg)
    {
        const LINE& l = s_page.aline[leg.iline];
        if (leg.fBend)
        {
            const glm::vec2 d = leg.bend - leg.a;
            const float len = glm::length(d);
            g_hud.FillScreenLine(leg.a, len > 0.0f ? leg.bend + d * (0.5f * l.w / len) : leg.bend, l.w, l.rgba, z);
            g_hud.FillScreenLine(leg.bend, leg.b, l.w, l.rgba, z);
        }
        else
            g_hud.FillScreenLine(leg.a, leg.b, l.w, l.rgba, z);
    }
    for (const DEVDRAW::LEG& leg : dev.aleg) DrawDot(leg.b, z);
}
}  // namespace

bool FControlsPageBegin(const SCREENOBJ* pso, const std::vector<CTLPIECE>& apiece, const FONTBRX* pfont,
                        const FONTBRX* pfontIcons, const SW* psw)
{
    s_page = PAGE{};
    if (FOff() || !pso || pso->oid != 0x370) return false;
    // (the page: the PS2 picture's groups and a line sprite)
    bool fPicture = false;
    for (const CTLPIECE& p : apiece)
        if (p.igroup >= IGROUP_PICTURE_FIRST && p.igroup <= IGROUP_PICTURE_LAST) { fPicture = true; break; }
    if (!fPicture) return false;
    if (g_pcinput.fAlwaysOriginalPs) return false;
    const Family family = PromptFamily();
    MODE mode = MODE_Off;
    if (family == Family::Keyboard) mode = MODE_Keyboard;
    else if (family == Family::PlayStation)
    {
        for (int a : {PCA_Cross, PCA_Circle, PCA_Square, PCA_Triangle, PCA_R1, PCA_Select, PCA_Start})
            if (BoundButton(a) != DefaultButton(a)) mode = MODE_PsRemap;
    }
    else mode = cane::input::promptAtlas().device(family) && PdevlayoutOf(family) ? MODE_Device : MODE_Off;
    if (static const char* s_sz = std::getenv("SLY2N_CTLPAGEDBG"); s_sz)
        std::printf("controls page: family %s mode %d\n", cane::input::familyId(family), int(mode));
    if (mode == MODE_Off) return false;
    s_page.mode = mode;
    s_page.family = family;

    // the picture's box and depth, the dots, the line sprite's lines
    bool fBox = false;
    std::vector<CHud::SPRITEPOLY> apoly;
    std::vector<glm::vec2> aposDot;
    for (const CTLPIECE& p : apiece)
    {
        if (!p.pelem || p.pelem->kind != SELEMK_Sprite) continue;
        if (p.igroup >= IGROUP_PICTURE_FIRST && p.igroup <= IGROUP_PICTURE_LAST)
        {
            CHud::SpritePolys(*p.pelem, p.st, apoly);
            for (const auto& sp : apoly)
                for (const glm::vec2& v : sp.apos)
                {
                    if (!fBox) { s_page.lo = s_page.hi = v; fBox = true; }
                    s_page.lo = glm::min(s_page.lo, v);
                    s_page.hi = glm::max(s_page.hi, v);
                }
        }
        else if (p.igroup == IGROUP_DOTS)
        {
            CHud::SpritePolys(*p.pelem, p.st, apoly);
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (const auto& sp : apoly)
                for (const glm::vec2& v : sp.apos) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
            if (lo.x > hi.x) continue;
            aposDot.push_back(0.5f * (lo + hi));
            if (!s_page.pdot) { s_page.pdot = p.pelem; s_page.stDot = p.st; s_page.posDot0 = aposDot.back(); }
        }
        else if (FLineSprite(p))
        {
            s_page.plineSprite = p.pelem;
            s_page.stLine = p.st;
            s_page.zPicture = p.st.depth / 16777216.0f;
        }
    }
    if (!s_page.plineSprite || !fBox) { s_page.mode = MODE_Off; return false; }
    // the labels' slots (the text elements: DrawTextBox's origin is the slot's (x - 320, y - 246.4); frame y =
    // text y / 1.1 + 224)
    for (const CTLPIECE& p : apiece)
        if (p.pelem && p.pelem->kind == SELEMK_Sound && p.pelem->ab3[1] == 0)
            if (const LABEL* pl = PlabelOf(p.pelem->n3))
            {
                SLOT slot;
                slot.id = p.pelem->n3;   // (the disc's text id, as the text elements' td.id)
                slot.action = pl->action;
                slot.m4 = p.mx;
                slot.m5 = p.my;
                slot.size = p.pelem->ag3[0];
                slot.w = p.pelem->ag3[1];
                slot.h = p.pelem->ag3[2];
                slot.align = p.pelem->ab3[0];
                slot.text = StrWithBoundNames(StrLocalized(psw, p.pelem->n3), mode, family, pl->action);
                s_page.aslot.push_back(slot);
            }
    // each line: the end at a dot (a PS2 button) and the label's end
    CHud::SpritePolys(*s_page.plineSprite, s_page.stLine, apoly);
    for (const auto& sp : apoly)
    {
        if (sp.apos.size() < 2) continue;
        // the vertex nearest a known PS2 button
        float dBest = 1e9f;
        Button bBest = Button::Count;
        size_t iBest = 0;
        for (size_t i = 0; i < sp.apos.size(); ++i)
            for (const PSPOS& ps : s_apspos)
            {
                if (ActionOfPsButton(ps.button) == -100) continue;
                const float d = glm::distance(sp.apos[i], CHud::ScreenOfLocal(s_page.stLine, glm::vec2(ps.x, ps.y)));
                if (d < dBest) { dBest = d; bBest = ps.button; iBest = i; }
            }
        if (bBest == Button::Count || dBest > 12.0f) continue;
        LINE l;
        l.aButton = sp.apos[iBest];
        float dFar = -1.0f;
        for (const glm::vec2& v : sp.apos)
            if (const float d = glm::distance(v, l.aButton); d > dFar) { dFar = d; l.aLabel = v; }
        l.action = ActionOfPsButton(bBest);
        l.rgba = sp.rgba;
        // the width: a strip's first pair across (a line strip: one pixel)
        l.w = sp.primitive == 3 ? 1.0f : std::max(1.0f, std::min(4.0f, glm::distance(sp.apos[0], sp.apos[1])));
        // (merge a line's several polygons: keep one per button end)
        bool fDup = false;
        for (LINE& m : s_page.aline)
            if (glm::distance(m.aButton, l.aButton) < 3.0f)
            {
                if (glm::distance(l.aLabel, l.aButton) > glm::distance(m.aLabel, m.aButton)) m.aLabel = l.aLabel;
                fDup = true;
            }
        if (!fDup) s_page.aline.push_back(l);
        s_page.alpha = std::clamp(sp.rgba.a / 128.0f, 0.0f, 1.0f);
    }
    // a line drawn as several pieces (Pause's bends): its label end follows the pieces joined to it
    for (bool fMore = true; fMore;)
    {
        fMore = false;
        for (const auto& sp : apoly)
        {
            if (sp.apos.size() < 2) continue;
            const glm::vec2 a = 0.5f * (sp.apos[0] + sp.apos[1]), b = 0.5f * (sp.apos[sp.apos.size() - 2] + sp.apos.back());
            for (LINE& l : s_page.aline)
                for (int k = 0; k < 2; ++k)
                {
                    const glm::vec2 near = k ? b : a, far = k ? a : b;
                    if (glm::distance(near, l.aLabel) < 3.0f && glm::distance(far, l.aButton) > glm::distance(l.aLabel, l.aButton) + 1.0f)
                    {
                        l.aLabel = far;
                        fMore = true;
                    }
                }
        }
    }
    if (static const char* s_sz = std::getenv("SLY2N_CTLPAGEDBG"); s_sz)
    {
        std::printf("controls page: box %.1f %.1f - %.1f %.1f, %zu dots, %zu lines, alpha %.2f\n", s_page.lo.x, s_page.lo.y,
                    s_page.hi.x, s_page.hi.y, aposDot.size(), s_page.aline.size(), s_page.alpha);
        for (const auto& sp : apoly)
        {
            std::printf("  line poly prim %d cv %zu rgba %.0f %.0f %.0f %.0f:", sp.primitive, sp.apos.size(), sp.rgba.r, sp.rgba.g,
                        sp.rgba.b, sp.rgba.a);
            for (const glm::vec2& v : sp.apos) std::printf(" (%.1f %.1f)", v.x, v.y);
            std::printf("\n");
        }
        for (const LINE& l : s_page.aline)
            std::printf("  line action %d label (%.1f %.1f) button (%.1f %.1f) w %.1f\n", l.action, l.aLabel.x, l.aLabel.y,
                        l.aButton.x, l.aButton.y, l.w);
    }
    for (SLOT& slot : s_page.aslot)
        for (size_t i = 0; i < s_page.aline.size(); ++i)
            if (s_page.aline[i].action == slot.action) slot.iline = int(i);
    if (s_page.mode == MODE_Device) LayoutDevice(pfont, pfontIcons);
    return true;
}

bool FControlsPageSkip(const CTLPIECE& p)
{
    if (s_page.mode == MODE_Off) return false;
    const bool fLine = p.pelem == s_page.plineSprite && FLineSprite(p);
    if (s_page.mode == MODE_PsRemap)
    {
        if (fLine) { DrawPsRemap(); return true; }
        return p.igroup == IGROUP_DOTS;
    }
    if (fLine)
    {
        if (s_page.mode == MODE_Device) DrawDevice();
        return true;
    }
    return (p.igroup >= IGROUP_PICTURE_FIRST && p.igroup <= IGROUP_PICTURE_LAST) || p.igroup == IGROUP_DOTS;
}

bool FControlsPageText(const TEXTDRAW& td, std::string& text)
{
    if (s_page.mode == MODE_Off || td.kind != 0) return false;
    const LABEL* pl = PlabelOf(td.id);
    if (!pl) return false;
    text = StrWithBoundNames(text, s_page.mode, s_page.family, pl->action);
    if (static const char* s_sz = std::getenv("SLY2N_CTLPAGEDBG"); s_sz)
        std::printf("  label 0x%X action %d x %.1f y %.1f w %.1f h %.1f size %.1f sx %.2f sy %.2f align %d '%s'\n", td.id,
                    pl->action, td.x, td.y, td.wBox, td.hBox, td.size, td.sx, td.sy, td.halign, text.c_str());
    if (s_page.mode == MODE_Device)
    {
        // (a label re-laid into another label's slot: drawn there at the end)
        for (size_t i = 0; i < s_page.aslot.size() && i < s_page.aislotTarget.size(); ++i)
            if (s_page.aslot[i].id == td.id && s_page.aislotTarget[i] != int(i))
            {
                const SLOT& to = s_page.aslot[size_t(s_page.aislotTarget[i])];
                TEXTDRAW tdTo = td;
                tdTo.x = to.m4 - 320.0f;
                tdTo.y = to.m5 - 246.4f;
                tdTo.wBox = to.w;
                tdTo.hBox = to.h;
                tdTo.halign = to.align;
                s_page.amoved.push_back({tdTo, text});
                return true;
            }
        // (a label whose bound buttons' names make it run off the page's bottom: its box widened until it fits)
        for (const SLOT& slot : s_page.aslot)
        {
            if (slot.id != td.id || BoxOfLabelIn(slot, text, td.pfont, td.pfontIcons).w <= 444.0f) continue;
            SLOT wide = slot;
            while (wide.w < slot.w + 120.0f && BoxOfLabelIn(wide, text, td.pfont, td.pfontIcons).w > 444.0f)
            {
                wide.w += 8.0f;
                wide.m4 -= slot.align == 1 ? 4.0f : slot.align == 2 ? 8.0f : 0.0f;
            }
            TEXTDRAW tdTo = td;
            tdTo.x = wide.m4 - 320.0f;
            tdTo.wBox = wide.w;
            s_page.amoved.push_back({tdTo, text});
            return true;
        }
        return false;
    }
    if (s_page.mode != MODE_Keyboard) return false;
    s_page.akb.push_back({td, text, pl->action});
    return true;
}

namespace
{
// a keyboard binding's cap (or the mouse's), false when the atlas has none
bool FKeyIcon(int key, cane::input::IconUV* puv)
{
    const auto& atlas = cane::input::promptAtlas();
    *puv = atlas.keyUV(key);
    return puv->valid && atlas.keyboardTexture();
}
}  // namespace

void ControlsPageEnd(const FONTBRX* pfont, const FONTBRX* pfontIcons)
{
    if (s_page.mode == MODE_Device && pfont)
        for (const auto& [td, text] : s_page.amoved)
            g_hud.DrawTextBox(*pfont, pfontIcons, text, td.x, td.y, td.size, td.sx, td.sy, td.wBox, td.hBox, td.halign, 0, td.off,
                              td.sc, td.offIcon, td.depth);
    if (s_page.mode != MODE_Keyboard || !pfont || s_page.akb.empty()) { s_page.mode = MODE_Off; return; }
    // the rows in the labels' reading order: movement, camera, the face buttons, run, equip, pause
    static const int s_aactOrder[] = {ACT_MOVE, ACT_CAMERA, PCA_Cross, PCA_Square, PCA_Circle, PCA_Triangle, PCA_R1,
                                      PCA_Select, PCA_Start};
    std::vector<const KBLABEL*> arow;
    for (int a : s_aactOrder)
        for (const KBLABEL& k : s_page.akb)
            if (k.action == a) { arow.push_back(&k); break; }
    const auto& atlas = cane::input::promptAtlas();
    const unsigned tex = atlas.keyboardTexture();
    const float a = s_page.alpha;
    // two columns under the title, in the PS2 picture's band of the page
    const float yTop = std::max(118.0f, s_page.lo.y - 52.0f), yBottom = 430.0f;
    const int crowCol = int(arow.size() + 1) / 2;
    const float dyRow = std::min(62.0f, (yBottom - yTop) / float(std::max(crowCol, 1)));
    const float hCap = 24.0f, dxCap = DxOfDy(hCap) + 2.0f;
    for (size_t i = 0; i < arow.size(); ++i)
    {
        const KBLABEL& k = *arow[i];
        const int col = int(i) / crowCol, row = int(i) % crowCol;
        const float x0 = col == 0 ? 34.0f : 338.0f, y0 = yTop + float(row) * dyRow, yc = y0 + 0.5f * dyRow;
        const float z = k.td.depth / 16777216.0f;
        // the caps: up to two rows (the movement keys as W / A S D)
        std::vector<std::vector<int>> aakey;
        if (k.action == ACT_MOVE)
        {
            aakey.push_back({g_pcinput.keyboard[PCA_Up]});
            aakey.push_back({g_pcinput.keyboard[PCA_Left], g_pcinput.keyboard[PCA_Down], g_pcinput.keyboard[PCA_Right]});
        }
        else if (k.action == ACT_CAMERA)
        {
            std::vector<int> akey;
            if (g_pcinput.fMouseCamera) akey.push_back(cane::input::kMouseMoveKey);
            const auto& ck = g_pcinput.cameraKeys;
            if (ck[0] == GLFW_KEY_LEFT && ck[1] == GLFW_KEY_RIGHT && ck[2] == GLFW_KEY_UP && ck[3] == GLFW_KEY_DOWN)
                akey.push_back(cane::input::kArrowKeysKey);
            else
                for (int key : ck) akey.push_back(key);
            aakey.push_back(akey);
        }
        else
        {
            std::vector<int> akey;
            const int key = g_pcinput.keyboard[size_t(k.action)];
            if (k.action == PCA_Start && key != GLFW_KEY_ESCAPE) akey.push_back(GLFW_KEY_ESCAPE);   // (Escape is always Start)
            if (key != st::kKeyUnknown) akey.push_back(key);
            aakey.push_back(akey);
        }
        const float wIcons = 4.0f * dxCap;
        std::string textMissing;
        for (size_t r = 0; r < aakey.size(); ++r)
        {
            const std::vector<int>& akey = aakey[r];
            const float yRow = aakey.size() == 1 ? yc : yc + (float(r) - 0.5f) * (hCap + 1.0f);
            const float xRow = x0 + 0.5f * (wIcons - float(akey.size()) * dxCap);
            for (size_t j = 0; j < akey.size(); ++j)
            {
                cane::input::IconUV uv;
                const int id = cane::input::promptKeyForBinding(akey[j]);
                if (FKeyIcon(id, &uv)) DrawIcon(tex, uv, glm::vec2(xRow + (float(j) + 0.5f) * dxCap, yRow), hCap, a, z);
                else textMissing += std::string(textMissing.empty() ? "" : ", ") + StrBindingName(akey[j]);
            }
        }
        // the label in its own font and colours, after the caps (a key without a cap is named first)
        std::string text = k.text;
        if (!textMissing.empty()) text = "[" + textMissing + "] " + text;
        const float xText = x0 + wIcons + 10.0f, wText = (col == 0 ? 330.0f : 626.0f) - xText;
        const float k11 = 1.1f;   // (frame pixels down -> the text box's units: 11 / 10)
        g_hud.DrawTextBox(*pfont, pfontIcons, text, xText - 320.0f, (y0 - 224.0f) * k11, k.td.size, k.td.sx, k.td.sy,
                          wText / std::max(k.td.sx, 1e-3f), dyRow * k11 / std::max(k.td.sy, 1e-3f), 0, 1, k.td.off, k.td.sc,
                          k.td.offIcon, k.td.depth);
    }
    s_page.mode = MODE_Off;
}
