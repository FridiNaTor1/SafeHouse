#pragma once
#include <unordered_map>

// 2D drawing (Sly1-Release blot.cpp's role): the screen objects' vector sprites and the CFontBrx text, as the 2D
// VU1 program P_K21 (0x25AFA0) draws them - quads/fans/strips/lines of GS 12.4 points through a 4x4 screen
// matrix with a colour scale/offset - but natively, as GL triangles in PS2 frame pixels (640 x 448).

#include "screenobj.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// The draw state a sprite / glyph packet carries (0x2174C8 / 0x217058): the 4x4 matrix (rows: x, y, -, origin;
// screen = row0 * x + row1 * y + row2, biased by 2^19 so the float's low 16 bits are GS 12.4), and the colour
// state (+0x08 depth, +0x0C the revealed vertex count, +0x10 colour offset, +0x20 colour scale).
struct HUDSTATE
{
    float m[16] = {};
    float depth = 0;
    int cvtxReveal = 0x7FFF;
    glm::vec4 offset{0.0f};     // GS units
    glm::vec4 scale{1.0f};
};

struct HUDBATCH;

// Collects triangles / lines for one frame.
// orig: 0x00215F40 (the reader's word wrap: spaces become line breaks where a line passes the box's width)
std::string StrWrapped(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88, float wBox);

class CHud
{
public:
    // (display.h fWideBackgrounds) the object being drawn is a full-screen frame (the binocucom's): it keeps its 4:3
    // shape and its polygons / lines on the canvas's border continue flat to the picture's border (ExtendEdgeTriangle)
    bool fWideEdgesObj = false;
    // (QoL) the object being drawn is the episode menu: its pictures are cut at the 4:3 canvas's left / right border
    bool fClipCanvasObj = false;
    int oidObj = -1;   // (the screen object being drawn: debug SLY3_HUDPOLYS=oid)
    // (PC, splitscreen3.h) the original two-player HUD in one player's view of a wide picture (16:9 stacked, side by
    // side): the HUD's band for that player (0 the top half of the canvas, 1 the bottom) is drawn alone, and the band's
    // view box (an axis-aligned quad at least 300 x 150 canvas units: its frame and the hole the view shows through)
    // grows by ex / ey canvas units on each side to the view's edges; the rest of the band (the portrait panel) moves
    // ex toward its side; other HUD pieces inside the box keep to their nearest box corner. ex = ey = 0 (the band's own
    // 8:3 aspect, a 4:3 picture stacked) changes nothing. The box's hole (unwidened canvas units, the smallest box quad)
    // is recorded per band for the view's scene rectangle (splitscreen3.cpp RectScene).
    struct BAND2P
    {
        bool f = false;
        int band = 0;
        float ex = 0.0f, ey = 0.0f;
        int iview = 0;   // (the view: four players draw each band in two views, each keeps its own object extents)
    };
    BAND2P band2p;
    // (band2p) the two-player games' banners (the score board, class 0xA4) between the bands: left out of the bands'
    // passes, drawn over the whole picture once after them (fCenter2p: that pass draws only them)
    bool fCenter2p = false;
    // (band2p) the screens that cover the whole picture (the job's result stamp 1066, Job Failed 1067): left out of the
    // bands' and the banner's passes, drawn once over the whole picture after them (fWhole2p: that pass draws only them)
    bool fWhole2p = false;
    static bool FWholeScreen2p(int oid) { return oid == 1066 || oid == 1067; }
    // (band2p) the object being drawn is that board: its banner (the strip between the bands: the winner's text and its
    // two lines) is drawn by the fCenter2p pass, its scores (each with its backing, beside the band's line) in their
    // player's band pass like the rest of the band (FBanner2p)
    bool fBoard2pObj = false;
    struct BOX2P { glm::vec4 hole{0.0f}; float area = 0.0f; int frame = -1; int oid = -1; };
    BOX2P abox2p[2];   // (the hole seen in the frame's draw; frame: g_iframeHarness of that draw)
    int iframe2p = 0;   // (the frame being drawn, set by the caller)
    // (QoL) the object being drawn is the wipe (0x30A, the Sly mask transition): on a wide picture only its full-span
    // rectangles go to the picture's edges; the mask's strips, which overshoot the canvas as it grows, keep their shape
    bool fWideRectsOnlyObj = false;
    void Begin();
    // 0x217298's 2D mode (1 mask write, 3 masked, 2 mask clear, 4 / 0 normal) for what follows.
    void SetMode(int mode);
    // orig: P_K21 sprite path (0x2174C8 / 0x217058 packets): an element's polygons through the state.
    void DrawSprite(const SELEM& elem, const HUDSTATE& st);
    // a textured quad element (kind 6 / 7: 0x2149D8's GIF packet, sprite with STQ 0..1, RGBA 0x80808080)
    void DrawQuad(const SELEM& elem, const HUDSTATE& st);
    // orig: 0x00212298's kind-6 packet: a render target's texture (rtgt.h) over the quad, where its alpha is not 0
    // (TEST 0x5000F), replacing the frame (ALPHA 0xA8, FIX 0x80)
    void DrawRtgtQuad(const SELEM& elem, const HUDSTATE& st, unsigned tex);
    // orig: 0x002166B0 / 0x00216750 (a rich text in a box; '&.' / '&x' colour escapes, '\n'; halign 0 left,
    // 1 centre, 2 right). x / y: the box origin in UI units (0.8 / (5/11) per GS pixel, centred); size: the text
    // size (+0x20 of the text box; glyphs are drawn at size / 51.2).
    // '&x' switches to the other colour and the other font (the reader's +0x10: the font object's next element,
    // the button icons), '&.' back.
    void DrawText(const FONTBRX& font, const FONTBRX* pfontOther, const std::string& text, float x, float y, float size,
                  float sx, float sy, int halign, glm::vec4 offset, glm::vec4 scale, glm::vec4 offsetOther);
    // The same in a box (0x216750's alignment in wBox / hBox; valign 1 centre / 2 bottom) at a depth.
    void DrawTextBox(const FONTBRX& font, const FONTBRX* pfontOther, const std::string& text, float x, float y,
                     float size, float sx, float sy, float wBox, float hBox, int halign, int valign, glm::vec4 offset,
                     glm::vec4 scale, glm::vec4 offsetOther, float depth, bool fWrap = true);
    // A textured quad in PS2 frame pixels (x0, y0 top left), in the current 2D mode and draw order: the dynamic
    // button icons (promptglyph.h) and the skip indicator. uv: u0 v1 at the top left, u1 v0 at the bottom right
    // (the atlases are uploaded unflipped); rgba in GS units (0x80 = 1), a straight-alpha texture. fOnce: one icon
    // per place this frame (a later fOnce quad of the same icon and size about where an earlier one is moves that
    // one instead): a text's drop-shadow copy, the same text again a few pixels off, would otherwise show through
    // the icon's cut-out label (R17).
    void DrawTexQuad(unsigned tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                     glm::vec4 rgba, float z = 1.0f, bool fOnce = false);
    // A ring of the fraction u (0..1, clockwise from the top) around (cx, cy), radii in canvas pixels of height
    // (the width corrected for the canvas's 480 / 448 pixel aspect); rgba in GS units, z 0..1 (1 on top).
    void DrawRing(float cx, float cy, float rOuter, float rInner, float u, glm::vec4 rgba, float z = 1.0f);
    // An icon-font glyph (dynamic: promptglyph.h) centred at (cx, cy) canvas pixels, h pixels high, at a depth
    void DrawIconGlyph(const FONTBRX& icons, char ch, float cx, float cy, float h, float depth);
    // An icon-font glyph's box in PS2 frame pixels as it would be drawn with st (false: no polygons)
    static bool FGlyphBox(const SELEM& glyph, const HUDSTATE& st, glm::vec2* plo, glm::vec2* phi, float* pz, float* pa);
    // Screen primitives for the game loop (Sly1-Release screen.cpp's FillScreenRect role): a rectangle in PS2
    // frame pixels (640 x 448) in GS colour units (alpha 0x80 = opaque), drawn over everything queued so far.
    void FillScreenRect(float x0, float y0, float x1, float y1, glm::vec4 rgba, float z = 1.0f);
    // Triangles (every three points one) in PS2 frame pixels, in one colour (GS units), drawn over what is queued so far
    // (hackmouse.cpp's crosshair).
    void FillScreenTris(const glm::vec2* apos, int cpos, glm::vec4 rgba, float z = 1.0f);
    // (QoL, controlspage.cpp) a sprite's polygons as DrawSprite would place them: PS2 frame pixels, colours in GS
    // units after the state's scale / offset, depth 0..1
    struct SPRITEPOLY { int primitive = 0; glm::vec4 rgba{0.0f}; float z = 0; std::vector<glm::vec2> apos; };
    static void SpritePolys(const SELEM& elem, const HUDSTATE& st, std::vector<SPRITEPOLY>& apoly);
    // A point of a sprite's own units (its polygons' coordinates / 16) placed by st, in PS2 frame pixels.
    static glm::vec2 ScreenOfLocal(const HUDSTATE& st, glm::vec2 local);
    // A straight line of width w (canvas pixels of height; the width corrected for the canvas's pixel aspect).
    void FillScreenLine(glm::vec2 a, glm::vec2 b, float w, glm::vec4 rgba, float z = 1.0f);
    // A fade: the whole screen in rgba (alpha 0..128).
    void Fade(glm::vec4 rgba);
    // (PC menus) the picture left and right of the 4:3 canvas filled (a wide picture; nothing at 4:3), at depth z
    void Pillarbox(glm::vec4 rgba, float z);
    // Letterbox bars covering u (0..1) of each band of hBar pixels at the top and bottom.
    void Letterbox(float u, float hBar = 56.0f, glm::vec4 rgba = glm::vec4(0, 0, 0, 128));
    // A horizontal wipe: the screen covered from the left up to u (0..1), soft edge of dxEdge pixels.
    void Wipe(float u, glm::vec4 rgba, float dxEdge = 32.0f);
    // (HUD Placement, display.h hudPlacement) what has been queued so far; the box of / a horizontal move of everything
    // queued after a mark (DrawScreenObjs: one screen object's polygons, text and icons)
    struct MARK { size_t ctri = 0, cline = 0, cquad = 0; };
    MARK Mark() const { return {m_atri.size(), m_aline.size(), m_aquad.size()}; }
    bool FBoundsSince(const MARK& mark, glm::vec2& lo, glm::vec2& hi) const;
    void ShiftSince(const MARK& mark, float dx);
    // Draws the frame's 2D into the scene target (over what is there).
    void Flush(int width, int height);

    int ctri = 0, cline = 0, csprite = 0;

private:
    struct VTX { glm::vec2 pos; glm::vec4 rgba; float z; glm::vec2 uv{0.0f}; };
    struct QUAD { unsigned tex = 0; int mode = 0; bool fReplace = false; bool fMip = false; bool fAlphaTest = false; bool fOnce = false; size_t ibatch = 0; std::vector<VTX> av; };
    bool m_fBreakBatch = false;   // the next draw starts a new batch (after a quad)
    // (band2p) an element's adjustment: skip it, none, or a shift; box quads are stretched per polygon
    struct ADJ2P { bool fSkip = false; glm::vec2 d{0.0f}; };
    int m_iGroup2p = 0;   // (a text box: 1 its first glyph decides, 2 m_adjGroup2p for the rest)
    ADJ2P m_adjGroup2p;
    ADJ2P Adj2p(glm::vec2 lo, glm::vec2 hi);
    // (fBoard2pObj) a polygon of the board's banner: a strip under 40 canvas units high centred between the bands
    static bool FBanner2p(glm::vec2 lo, glm::vec2 hi) { const float c = 0.5f * (lo.y + hi.y); return hi.y - lo.y < 40.0f && c >= 200.0f && c <= 248.0f; }
    // (band2p) each object's extent per band (unwidened), this frame's and the last drawn frame's: a gauge's pieces (its
    // masked fills overshoot it) move as one
    struct OBJBOX2P { glm::vec4 a[2]{}, prev[2]{}; int frame[2]{-9, -9}, framePrev[2]{-9, -9}; };
    std::unordered_map<int, OBJBOX2P> m_mpobj2p;
    bool FBoxQuad2p(const std::vector<VTX>& av, glm::vec4* prect) const;
    void Stretch2p(std::vector<VTX>& av, const glm::vec4& rect) const;
    std::vector<QUAD> m_aquad;
    struct BATCH { int mode; size_t itri, ctri, iline, cline; };
    void CloseBatch();
    bool FlushPackets(int width, int height);   // (draw packets: the Vulkan renderer)
    void ExtendEdgeTriangle(VTX a, VTX b, VTX c);   // (the binocucom frames on wide pictures)
    void ExtendEdgeLine(VTX v);
    bool FClipTriangleToCanvas(const VTX& a, const VTX& b, const VTX& c);
    bool FClipLineToCanvas(VTX& a, VTX& b);
    std::vector<VTX> m_atri, m_aline;
    std::vector<BATCH> m_abatch;
    int m_mode = 0;
};

extern CHud g_hud;

// The kind-21 RPLs of a reference RAM (0x3B3980..: {21, z, packet A, chain B}) drawn natively: their matrices and
// colours from the packets, their geometry from the loaded screen objects (matched by object id, element index
// and polygon count). Returns the number of sprites drawn.
int CDrawHudFromRam(const std::string& ramPath, const std::vector<std::shared_ptr<SCREENOBJ>>& aso, std::string& log);

// (Sly 3) a kind-7 screen element's texture id with no shader of its own: the shader id that stands in (-1 none)
extern int (*g_pfnQuadShaderAlias)(int idTexture);
// (Sly 3) a transform (2x3, canvas units 640 x 480) for the kind-7 element of a texture id (false: none)
extern bool (*g_pfnQuadXf)(int idTexture, float a[6]);
// (Sly 3, PC: s3-plane's duel3.h) a screen object's piece about to be drawn (its object, element index, element and draw
// state): true = drawn by the hook instead (the free-for-all's per-player frames / portraits in the 2P HUD bands)
extern bool (*g_pfnHudPiece3)(int oidObj, int ielem, const SELEM& el, const HUDSTATE& st);
