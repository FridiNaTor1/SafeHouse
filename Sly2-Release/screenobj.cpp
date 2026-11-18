// See screenobj.h. Reference ports: Sly2-Reference game/m_210E90_p3.cpp, m_210E90_c.cpp.

#include "screenobj.h"

#include "langstore.h"
#include "loadtrace.h"
#include "region.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace
{
glm::mat3x2 Mat3x2FromBrx(CBinaryInputStream* pbis)
{
    // ReadMatrix 0x117ED0 (3 rows of 3 floats); the 2D code keeps the first two floats of each row.
    const glm::mat3 m = pbis->ReadMatrix();
    glm::mat3x2 r;
    for (int q = 0; q < 3; ++q) r[q] = glm::vec2(m[q][0], m[q][1]);
    return r;
}

void LoadElemsFromBrx(std::vector<SELEM>& aelem, CBinaryInputStream* pbis);
void LoadElemBodyFromBrx(SELEM& e, CBinaryInputStream* pbis);

// orig: 0x002149D8 (one 0x40-byte element: index U16, kind U32, then the kind's data)
void LoadElemFromBrx(std::vector<SELEM>& aelem, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x2149D8, pbis);
    const uint32_t idx = pbis->U16Read();
    if (idx >= aelem.size()) throw std::runtime_error("screen element index out of range");
    SELEM& e = aelem[idx];
    e.idx = idx;
    LoadElemBodyFromBrx(e, pbis);
}

// The element's kind and data (the Japanese build's element reader JP 0x215DF8 starts here: its callers read the index)
void LoadElemBodyFromBrx(SELEM& e, CBinaryInputStream* pbis)
{
    e.kind = pbis->U32Read();
    if (FWideText() && e.kind == SELEMK_Set)
    {
        // the Japanese build's font (JP 0x215E88): U8, U16 count, then each glyph: its U16 character, its F32 width,
        // its element (kind + data). (The original keeps the glyphs of the characters the world's text uses, JP
        // 0x13E490 with SW +0x4848, and those of a set with the U8 = 1; the port keeps all.)
        pbis->U8Read();
        const int c = pbis->U16Read();
        std::vector<std::pair<uint32_t, std::pair<float, SELEM>>> aglyph(static_cast<std::size_t>(c));
        for (auto& [ch, g] : aglyph)
        {
            ch = pbis->U16Read();
            g.first = pbis->F32Read();
            LoadElemBodyFromBrx(g.second, pbis);
        }
        std::stable_sort(aglyph.begin(), aglyph.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        e.asub.assign(1, SELEM{});
        for (auto& [ch, g] : aglyph)
        {
            e.aul.push_back(ch);
            e.ag.push_back(g.first);
            g.second.idx = uint32_t(e.asub.size());
            e.asub.push_back(std::move(g.second));
        }
        if (g_plangcapture) g_plangcapture->Font(e);   // (--import-languages: the text font, langstore.h)
        // (debug: SLY2N_FONTLIST=1 prints each 16-bit font's characters: tools/porttextcheck.py --kr-font)
        if (static const bool s_fList = std::getenv("SLY2N_FONTLIST") != nullptr; s_fList)
        {
            std::printf("fontlist: %zu glyphs:", e.aul.size());
            for (uint32_t ch : e.aul) std::printf(" %04X", ch);
            std::printf("\n");
        }
        return;
    }
    if (FWideText() && (e.kind == SELEMK_Transform || e.kind > SELEMK_Set)) return;  // (the Japanese build reads no data for kinds 2 and 9..: JP 0x215DF8)
    switch (e.kind)
    {
    case SELEMK_Ref:
        e.ref = pbis->U16Read();
        break;
    case SELEMK_Set:
    {
        LoadElemsFromBrx(e.asub, pbis);
        const uint32_t n = pbis->U32Read();
        e.aul.resize(n);
        for (uint32_t& u : e.aul) u = pbis->U32Read();
        const uint32_t m = pbis->U32Read();
        e.ag.resize(m);
        for (float& g : e.ag) g = pbis->F32Read();
        break;
    }
    case SELEMK_Sound:
        // (the Japanese build's text element has no +0x10 word: its U32 is the colour, JP 0x216018)
        if (!FWideText()) e.aul3[0] = pbis->U32Read();
        e.aul3[1] = pbis->U32Read();
        e.ag3[0] = pbis->F32Read();
        e.ag3[2] = pbis->F32Read();
        e.ag3[1] = pbis->F32Read();
        e.ab3[0] = pbis->S8Read();
        e.ab3[1] = pbis->S8Read();
        if (e.ab3[1] == 1 || e.ab3[1] == 0) e.n3 = pbis->S32Read();
        break;
    case SELEMK_Transform:
    {
        e.mat = Mat3x2FromBrx(pbis);
        const uint32_t n = pbis->U16Read();
        e.arec.resize(n);
        for (auto& rec : e.arec) pbis->ReadBytes(rec.data(), rec.size());
        break;
    }
    case SELEMK_Image:
    case SELEMK_Quad:
        e.x0 = pbis->S16Read();
        e.y0 = pbis->S16Read();
        e.x1 = pbis->S16Read();
        e.y1 = pbis->S16Read();
        e.idTexture = pbis->S32Read();
        break;
    case SELEMK_Sprite:
    {
        const uint32_t n = pbis->U16Read();
        e.apoly.resize(n);
        for (SPOLY& poly : e.apoly)
        {
            const uint32_t cv = pbis->U8Read();
            poly.control = pbis->U8Read();
            poly.primitive = pbis->U8Read();
            poly.unused = pbis->U8Read();
            poly.fVertexBytes = pbis->U8Read();
            poly.axy.resize(cv);
            poly.ab.resize(cv);
            for (uint32_t j = 0; j < cv; ++j)
            {
                poly.rgba = pbis->U32Read();
                poly.ab[j] = pbis->U8Read();
                poly.axy[j][0] = pbis->S16Read();
                poly.axy[j][1] = pbis->S16Read();
            }
        }
        break;
    }
    default:
        break;
    }
}

// orig: 0x00214398 (an element set: its count, then each element; indices 0..count)
void LoadElemsFromBrx(std::vector<SELEM>& aelem, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x214398, pbis);
    const int c = pbis->U16Read();
    aelem.assign(size_t(c) + 1, SELEM{});
    for (int i = 1; i <= c; ++i) LoadElemFromBrx(aelem, pbis);
}

// orig: 0x00214460 (the group tree: groups, their frame sets, their parts of keys of items, colours, ...)
void LoadGroupsFromBrx(std::vector<SGROUP>& agroup, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x214460, pbis);
    agroup.resize(pbis->U16Read());
    for (SGROUP& g : agroup)
    {
        g.cframeSet = pbis->U16Read();
        g.cframe = pbis->U16Read();
        g.apart.resize(pbis->U16Read());
        for (SPART& part : g.apart)
        {
            part.dt = pbis->F32Read();
            part.id = pbis->S32Read();
            part.akey.resize(pbis->U16Read());
            for (SKEY& key : part.akey)
            {
                key.t = pbis->F32Read();
                key.aitem.resize(pbis->U8Read());
                for (SITEM& it : key.aitem)
                {
                    it.aw[0] = pbis->U16Read();
                    it.aw[1] = pbis->U16Read();
                    it.aw[2] = pbis->U16Read();
                    it.s6 = pbis->S16Read();
                    it.b30 = pbis->U8Read();
                    it.b31 = pbis->U8Read();
                    it.n34 = pbis->S32Read();
                    it.mat = Mat3x2FromBrx(pbis);
                }
                key.acolor.resize(pbis->U8Read());
                for (auto& c : key.acolor) { c[1] = pbis->U32Read(); c[0] = pbis->U32Read(); }
                key.aw.resize(pbis->U8Read());
                for (uint16_t& w : key.aw) w = pbis->U16Read();
                key.ab.resize(pbis->U8Read());
                for (uint8_t& b : key.ab) b = pbis->U8Read();
                key.atag.resize(pbis->U8Read());
                for (auto& t : key.atag) { t.first = pbis->U8Read(); t.second = pbis->S32Read(); }
            }
        }
    }
}

// (PC) Latin fonts: the USA / European fonts have no lowercase eth (U+00F0, Faroese / Icelandic text in the
// languages folder): its slot is the font's placeholder box, the same shapes as thorn's (0xFE). The slot gets a drawn
// eth in the font's hand: the 'o's shapes (its fill and its hole, a little smaller) as the bowl, a stroke rising from
// the bowl's right side that bends up and over to the left, and a slanted bar across it, at the width of the 'd'.
bool FSamePolys(const SELEM& a, const SELEM& b)
{
    if (a.apoly.size() != b.apoly.size()) return false;
    for (size_t i = 0; i < a.apoly.size(); ++i)
        if (a.apoly[i].axy != b.apoly[i].axy || a.apoly[i].ab != b.apoly[i].ab) return false;
    return true;
}

bool FPolyBox(const SELEM& g, int* pxmin, int* pymin, int* pxmax, int* pymax)
{
    *pxmin = *pymin = 1 << 30; *pxmax = *pymax = -(1 << 30);
    for (const SPOLY& p : g.apoly)
        for (const auto& v : p.axy)
        {
            *pxmin = std::min(*pxmin, int(v[0])); *pxmax = std::max(*pxmax, int(v[0]));
            *pymin = std::min(*pymin, int(v[1])); *pymax = std::max(*pymax, int(v[1]));
        }
    return *pxmin <= *pxmax;
}

// The eth's strokes, drawn against the USA font's 'o' (12.4 units: x 32..420, y -539 (top)..4; the baseline at 0):
// pairs of points across the stroke (inner / left edge, outer / right edge) from one end to the other, kept as a strip.
// The stroke is as thick as the font's strokes (the 'o's sides, the 'l'), the bar a little thinner, like the hyphen.
constexpr int s_xoRef0 = 32, s_xoRef1 = 420, s_yoRef0 = -539, s_yoRef1 = 4;
constexpr int16_t s_aethStem[][2][2] = {
    {{290, -250}, {392, -250}}, {{306, -370}, {414, -400}}, {{298, -470}, {404, -520}},
    {{262, -552}, {362, -616}}, {{206, -610}, {290, -684}}, {{136, -636}, {174, -720}},
};
constexpr int16_t s_aethBar[][2][2] = {
    {{196, -560}, {214, -488}}, {{460, -654}, {478, -584}},
};
constexpr float s_sxEthBowl = 0.95f, s_syEthBowl = 0.85f, s_xEthBowl = 210.0f;   // the bowl: the 'o' a little smaller

// the eth (0xF0) drawn in its placeholder slot
void PatchEth(SELEM& e)
{
    auto slot = [&](uint32_t ch) -> int {
        const auto it = std::lower_bound(e.aul.begin(), e.aul.end(), ch);
        return it != e.aul.end() && *it == ch && size_t(it - e.aul.begin()) + 1 < e.asub.size() ? int(it - e.aul.begin()) : -1;
    };
    const int ieth = slot(0xF0), ithorn = slot(0xFE), id = slot('d'), io = slot('o');
    if (ieth < 0 || ithorn < 0 || id < 0 || io < 0) return;
    SELEM& eth = e.asub[ieth + 1];
    const SELEM& o = e.asub[io + 1];
    if (!FSamePolys(eth, e.asub[ithorn + 1]) || o.apoly.empty()) return;
    // the 'o's fill (the opaque shape) gives the strokes' layer, colour and vertex bytes
    const SPOLY* pfill = nullptr;
    for (const SPOLY& p : o.apoly)
        if ((p.rgba >> 24) != 0 && (!pfill || p.control < pfill->control)) pfill = &p;
    int ox0, oy0, ox1, oy1;
    if (!pfill || !FPolyBox(o, &ox0, &oy0, &ox1, &oy1) || ox1 <= ox0 || oy1 <= oy0) return;
    // (the reference 'o' to this font's 'o')
    const float kx = float(ox1 - ox0) / float(s_xoRef1 - s_xoRef0), ky = float(oy1 - oy0) / float(s_yoRef1 - s_yoRef0);
    auto map = [&](float x, float y) {
        return std::array<int16_t, 2>{int16_t(std::lround(float(ox0) + (x - float(s_xoRef0)) * kx)),
                                      int16_t(std::lround(float(oy0) + (y - float(s_yoRef0)) * ky))};
    };
    SELEM glyph = o;
    for (SPOLY& p : glyph.apoly)
        for (auto& v : p.axy)
        {
            // (this font's 'o' in the reference's units, the bowl's scale about the baseline, back)
            const float x = float(s_xoRef0) + float(v[0] - ox0) / kx, y = float(s_yoRef0) + float(v[1] - oy0) / ky;
            v = map(s_xEthBowl + (x - s_xEthBowl) * s_sxEthBowl, y * s_syEthBowl);
        }
    auto addStrip = [&](const int16_t (*apair)[2][2], size_t cpair) {
        SPOLY p;
        p.primitive = 2;   // a strip: across the stroke, pair by pair
        p.control = pfill->control;
        p.unused = pfill->unused;
        p.fVertexBytes = pfill->fVertexBytes;
        p.rgba = pfill->rgba;
        for (size_t i = 0; i < cpair; ++i)
            for (int k = 0; k < 2; ++k)
            {
                p.axy.push_back(map(apair[i][k][0], apair[i][k][1]));
                p.ab.push_back(0);
            }
        glyph.apoly.push_back(std::move(p));
    };
    addStrip(s_aethStem, std::size(s_aethStem));
    addStrip(s_aethBar, std::size(s_aethBar));
    glyph.idx = eth.idx;
    eth = std::move(glyph);
    e.ag[ieth] = e.ag[id];
}

// The accented i's (ì í î ï 0xEC..0xEF) of the USA font are the 'i' with its dot kept and the accent stacked above
// the dot (read as an i with a comma on top). Each one whose shapes are the 'i's stem and dot (both moved by the same
// offset) plus an accent loses the dot, and its accent comes down to the height of the acute on á é ó ú (the bottom
// of their topmost shape). A font whose accented i's are drawn otherwise (no copy of the 'i's dot) keeps them.
bool FPolyYRange(const SPOLY& p, int* pymin, int* pymax)
{
    *pymin = 1 << 30; *pymax = -(1 << 30);
    for (const auto& v : p.axy) { *pymin = std::min(*pymin, int(v[1])); *pymax = std::max(*pymax, int(v[1])); }
    return !p.axy.empty();
}

// p is q moved by one offset: the same vertices otherwise (maybe starting at another one of them: a fan's outline), give
// or take the font's rounding (4 units, a quarter pixel)
constexpr int s_dxyMovedTolerance = 4;
bool FPolyMoved(const SPOLY& p, const SPOLY& q, int* pdx, int* pdy)
{
    const size_t n = p.axy.size();
    if (n == 0 || n != q.axy.size() || p.primitive != q.primitive || p.control != q.control || p.rgba != q.rgba) return false;
    for (size_t r = 0; r < (p.primitive == 1 ? n : 1); ++r)
    {
        const int dx = p.axy[r][0] - q.axy[0][0], dy = p.axy[r][1] - q.axy[0][1];
        bool f = true;
        for (size_t i = 1; i < n && f; ++i)
        {
            const size_t j = (i + r) % n;
            f = p.ab[j] == q.ab[i] && std::abs(p.axy[j][0] - q.axy[i][0] - dx) <= s_dxyMovedTolerance &&
                std::abs(p.axy[j][1] - q.axy[i][1] - dy) <= s_dxyMovedTolerance;
        }
        if (f && p.ab[r] == q.ab[0]) { *pdx = dx; *pdy = dy; return true; }
    }
    return false;
}

void PatchDottedAccentedI(SELEM& e)
{
    auto slot = [&](uint32_t ch) -> int {
        const auto it = std::lower_bound(e.aul.begin(), e.aul.end(), ch);
        return it != e.aul.end() && *it == ch && size_t(it - e.aul.begin()) + 1 < e.asub.size() ? int(it - e.aul.begin()) : -1;
    };
    const int ii = slot('i');
    if (ii < 0) return;
    const SELEM& i = e.asub[ii + 1];
    // the 'i's stem: its shape reaching lowest; its dot: the shapes wholly above the stem
    int istem = -1, ystemTop = 0, ystemBot = -(1 << 30);
    for (size_t k = 0; k < i.apoly.size(); ++k)
    {
        int y0, y1;
        if (FPolyYRange(i.apoly[k], &y0, &y1) && y1 > ystemBot) { istem = int(k); ystemTop = y0; ystemBot = y1; }
    }
    if (istem < 0) return;
    std::vector<size_t> aidot;
    for (size_t k = 0; k < i.apoly.size(); ++k)
    {
        int y0, y1;
        if (int(k) != istem && FPolyYRange(i.apoly[k], &y0, &y1) && y1 < ystemTop) aidot.push_back(k);
    }
    if (aidot.empty() || aidot.size() + 1 != i.apoly.size()) return;
    // the acute's height on the other lowercase vowels: the bottom of their topmost shape
    int cacute = 0, yAcuteSum = 0;
    for (uint32_t ch : {0xE1u, 0xE9u, 0xF3u, 0xFAu})
    {
        const int ia = slot(ch);
        if (ia < 0) continue;
        int yTop = 1 << 30, yBot = 0;
        for (const SPOLY& p : e.asub[ia + 1].apoly)
        {
            int y0, y1;
            if (FPolyYRange(p, &y0, &y1) && y0 < yTop) { yTop = y0; yBot = y1; }
        }
        if (yTop != (1 << 30)) { yAcuteSum += yBot; ++cacute; }
    }
    if (cacute == 0) return;
    const int yAccentBot = int(std::lround(float(yAcuteSum) / float(cacute)));
    for (uint32_t ch = 0xEC; ch <= 0xEF; ++ch)
    {
        const int ig = slot(ch);
        if (ig < 0) continue;
        SELEM& g = e.asub[ig + 1];
        // the stem and every dot shape moved by one offset; the rest is the accent
        std::vector<bool> afUsed(g.apoly.size(), false), afDot(g.apoly.size(), false);
        int dx = 0, dy = 0;
        bool fStem = false;
        for (size_t k = 0; k < g.apoly.size() && !fStem; ++k)
            if (FPolyMoved(g.apoly[k], i.apoly[istem], &dx, &dy)) { afUsed[k] = fStem = true; }
        if (!fStem) continue;
        size_t cdot = 0;
        for (size_t idot : aidot)
            for (size_t k = 0; k < g.apoly.size(); ++k)
            {
                int dx2, dy2;
                if (!afUsed[k] && FPolyMoved(g.apoly[k], i.apoly[idot], &dx2, &dy2) &&
                    std::abs(dx2 - dx) <= s_dxyMovedTolerance && std::abs(dy2 - dy) <= s_dxyMovedTolerance) { afUsed[k] = afDot[k] = true; ++cdot; break; }
            }
        if (cdot != aidot.size()) continue;
        int yBot = -(1 << 30);
        size_t caccent = 0;
        for (size_t k = 0; k < g.apoly.size(); ++k)
        {
            int y0, y1;
            if (!afUsed[k] && FPolyYRange(g.apoly[k], &y0, &y1)) { yBot = std::max(yBot, y1); ++caccent; }
        }
        if (caccent == 0) continue;
        // (down to the acute's height, never into the stem)
        const int dyAccent = std::min(yAccentBot, ystemTop + dy) - yBot;
        if (dyAccent <= 0) continue;
        std::vector<SPOLY> apoly;
        for (size_t k = 0; k < g.apoly.size(); ++k)
        {
            if (afDot[k]) continue;
            SPOLY p = g.apoly[k];
            if (!afUsed[k])
                for (auto& v : p.axy) v[1] = int16_t(v[1] + dyAccent);
            apoly.push_back(std::move(p));
        }
        g.apoly = std::move(apoly);
    }
}

void PatchLatinFontGlyphs(std::vector<SELEM>& aelem)
{
    if (FWideText()) return;
    for (SELEM& e : aelem)
    {
        if (e.kind == SELEMK_Set && !e.aul.empty() && e.aul.size() == e.ag.size() && std::is_sorted(e.aul.begin(), e.aul.end()) &&
            e.aul.back() <= 0xFFu)
        {
            PatchEth(e);
            PatchDottedAccentedI(e);
        }
        if (e.kind == SELEMK_Set) PatchLatinFontGlyphs(e.asub);
    }
}
}  // namespace

// orig: 0x00212298 (a screen object: S8 external, F32 +0x90, U32 +0x94 / +0x98 / +0xBC; external: the file key
// (0x118458), else the element set and the group tree)
void LoadScreenObjFromBrx(SCREENOBJ* pso, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x212298, pbis);
    pso->fExternal = pbis->S8Read();
    pso->g90 = pbis->F32Read();
    pso->ul94 = pbis->U32Read();
    pso->ul98 = pbis->U32Read();
    pso->ulBC = pbis->U32Read();
    if (pso->fExternal)
    {
        uint8_t ab[0x40];
        pbis->ReadRecord64(ab, KEYK_Screen);   // 0x118458
        pso->fk = FkFromRecord(ab);
    }
    else
    {
        LoadElemsFromBrx(pso->aelem, pbis);
        LoadGroupsFromBrx(pso->agroup, pbis);
        PatchLatinFontGlyphs(pso->aelem);
    }
}

const SELEM* FONTBRX::PglyphFromCh(uint32_t ch) const
{
    // orig: 0x00215A50 CFontBrx::PglyffFromCh (binary search; 1-based)
    auto it = std::lower_bound(pset->aul.begin(), pset->aul.end(), uint32_t(ch));
    if ((it == pset->aul.end() || *it != ch) && (ch == '{' || ch == '}')) return PglyphFromCh(ch == '{' ? '(' : ')');   // (PC: L3 / R3, promptglyph.cpp)
    if ((it == pset->aul.end() || *it != ch) && ch >= 1 && ch <= 4) return PglyphFromCh(ch <= 2 ? 'T' : ch == 3 ? '(' : ')');   // (PC: the VR grips / stick clicks, promptglyph.h)
    if (it == pset->aul.end() || *it != ch) return nullptr;
    const size_t i = size_t(it - pset->aul.begin()) + 1;
    return i < pset->asub.size() ? &pset->asub[i] : nullptr;
}

float FONTBRX::DxFromCh(uint32_t ch) const
{
    const auto it = std::lower_bound(pset->aul.begin(), pset->aul.end(), uint32_t(ch));
    if ((it == pset->aul.end() || *it != ch) && (ch == '{' || ch == '}')) return DxFromCh(ch == '{' ? '(' : ')');   // (PC: L3 / R3)
    if ((it == pset->aul.end() || *it != ch) && ch >= 1 && ch <= 4) return DxFromCh(ch <= 2 ? 'T' : ch == 3 ? '(' : ')');   // (PC: the VR combos)
    if (it == pset->aul.end() || *it != ch)
    {
        // orig KR 0x2183A0: a Hangul syllable of a jamo font advances by the thin space's width (U+2009) when its
        // vowel is under the consonant, else by the cell's (the glyph of U+1100), region.h CHangulJamo
        uint32_t ajamo[3];
        if (CHangulJamo(ch, ajamo) && std::binary_search(pset->aul.begin(), pset->aul.end(), 0x1100u))
            return DxFromCh(FHangulHorizontalVowel(ajamo[1]) ? 0x2009u : 0x1100u);
        return 0.0f;
    }
    const size_t i = size_t(it - pset->aul.begin());
    return i < pset->ag.size() ? pset->ag[i] : 0.0f;
}

std::vector<FONTBRX> FindFonts(const std::vector<std::shared_ptr<SCREENOBJ>>& aso)
{
    std::vector<FONTBRX> afont;
    for (const auto& pso : aso)
        for (size_t i = 0; i < pso->aelem.size(); ++i)
        {
            const SELEM& e = pso->aelem[i];
            if (e.kind != SELEMK_Set || e.aul.empty() || e.aul.size() != e.ag.size()) continue;
            if (!std::is_sorted(e.aul.begin(), e.aul.end()) || e.aul.back() > (FWideText() ? 0xFFFFu : 0xFFu)) continue;
            FONTBRX font;
            font.pso = pso.get();
            font.pset = &e;
            font.ifont = int(i);
            afont.push_back(font);
        }
    return afont;
}

// The external data of a screen object (fExternal: its file, LZSS like a world file): "ZRB." + U16 version, then
// the element set and the group tree as in the world file.
bool FLoadScreenFile(SCREENOBJ* pso, const std::vector<uint8_t>& ab, std::string& error)
{
    if (ab.size() < 6 || std::memcmp(ab.data(), "ZRB.", 4) != 0) { error = "not a ZRB file"; return false; }
    CBinaryInputStream bis;
    bis.OpenMemory(static_cast<int>(ab.size()), ab.data());
    bis.m_ib = 6;
    try
    {
        LoadElemsFromBrx(pso->aelem, &bis);
        LoadGroupsFromBrx(pso->agroup, &bis);
        PatchLatinFontGlyphs(pso->aelem);
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    if (bis.m_ib != ab.size())
    {
        error = "ended at " + std::to_string(bis.m_ib) + " of " + std::to_string(ab.size());
        return false;
    }
    return true;
}
