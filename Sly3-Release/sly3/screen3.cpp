// See screen3.h.

#include "screen3.h"

#include "ids3.h"
#include "load3.h"

#include "../bis.h"
#include "../disc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace
{
// orig3: 0x263748 LoadElemFromBrx (the reads; the GIF / VIF packets it builds are the renderer's)
void LoadElem3(SELEM3& e, CBinaryInputStream* pbis, int depth)
{
    if (depth > 8) throw std::runtime_error("screen elements nested too deep");
    static const bool s_fTrace = std::getenv("SLY3_SCREEN_TRACE") != nullptr;   // debugging: each element's kind
    const size_t ib0 = pbis->m_ib;
    e.kind = pbis->U32Read();
    if (s_fTrace) std::printf("  elem at %zu depth %d kind %u\n", ib0, depth, e.kind);
    switch (e.kind)
    {
    case 1:   // 0x263E54
    {
        e.n = pbis->S32Read();
        e.apoly.resize(pbis->U16Read());
        for (SPOLY3& p : e.apoly)
        {
            const int cv = pbis->U8Read();
            p.control = pbis->U8Read();
            p.primitive = pbis->U8Read();
            p.unused = pbis->U8Read();
            p.fVertexBytes = pbis->U8Read();
            p.avtx.resize(size_t(cv));
            for (SVTX3& v : p.avtx)
            {
                v.rgba = pbis->U32Read();   // (0x236970)
                v.b = pbis->U8Read();
                v.x = pbis->S16Read();
                v.y = pbis->S16Read();
            }
        }
        break;
    }
    case 3:   // 0x2639E0
        e.rgba3 = pbis->U32Read();
        for (float& g : e.ag3) g = pbis->F32Read();
        for (int8_t& b : e.ab3) b = pbis->S8Read();
        if (e.ab3[1] == 1 || e.ab3[1] == 0) e.n3 = pbis->S32Read();   // (+0xC: the second S8)
        break;
    case 4:   // 0x2637C8
        e.n = pbis->S32Read();
        e.ref = pbis->U16Read();
        break;
    case 6:
    case 7:   // 0x263AA8
        e.x0 = pbis->S16Read();
        e.y0 = pbis->S16Read();
        e.x1 = pbis->S16Read();
        e.y1 = pbis->S16Read();
        e.idTexture = pbis->S32Read();
        break;
    case 8:   // 0x2637E4: a font
    {
        e.mode = pbis->U8Read();
        const int c = pbis->U16Read();
        e.ach.resize(size_t(c));
        e.adx.resize(size_t(c));
        e.aglyph.resize(size_t(c));
        for (int i = 0; i < c; ++i)
        {
            e.ach[size_t(i)] = pbis->U16Read();
            e.adx[size_t(i)] = pbis->F32Read();
            LoadElem3(e.aglyph[size_t(i)], pbis, depth + 1);
        }
        break;
    }
    default:  // 0, 2, 5 (and anything else: 0x264384)
        break;
    }
}

// orig3: 0x263118
void LoadElems3(std::vector<SELEM3>& aelem, CBinaryInputStream* pbis)
{
    const int c = pbis->U16Read();
    aelem.assign(size_t(c) + 1, SELEM3{});
    for (int i = 1; i <= c; ++i)
    {
        const int idx = pbis->U16Read();
        if (idx < 0 || idx > c) throw std::runtime_error("screen element index out of range");
        LoadElem3(aelem[size_t(idx)], pbis, 0);
    }
}

glm::mat3 ReadMatrix3(CBinaryInputStream* pbis) { return pbis->ReadMatrix(); }

// orig3: 0x2631C8
void LoadGroups3(std::vector<SGROUP3>& agroup, CBinaryInputStream* pbis)
{
    agroup.resize(pbis->U16Read());
    for (SGROUP3& g : agroup)
    {
        g.cframeSet = pbis->U16Read();
        g.cframe = pbis->U16Read();
        g.apart.resize(pbis->U16Read());
        for (SPART3& part : g.apart)
        {
            part.dt = pbis->F32Read();
            part.id = pbis->S32Read();
            part.akey.resize(pbis->U16Read());
            for (SKEY3& key : part.akey)
            {
                key.t = pbis->F32Read();
                key.aitem.resize(pbis->U8Read());
                key.acolor.resize(pbis->U8Read());
                key.ab.resize(pbis->U8Read());
                key.bFlag = pbis->U8Read() & 1;
                key.b3 = pbis->U8Read() & 7;
                key.n = pbis->S32Read();
                for (SITEM3& it : key.aitem)
                {
                    it.w = pbis->U16Read() & 0x3FF;
                    it.ab[0] = pbis->U8Read();
                    it.ab[1] = pbis->U8Read() & 0x3F;
                    it.ab[2] = pbis->U8Read() & 1;
                    it.ab[3] = uint8_t(std::min<int>(pbis->U8Read(), 0x7F));
                    it.n = pbis->S32Read();
                    it.mat = ReadMatrix3(pbis);
                }
                for (auto& c : key.acolor) { c[0] = pbis->U32Read(); c[1] = pbis->U32Read(); }
                for (uint8_t& b : key.ab) b = pbis->U8Read();
            }
        }
    }
}
}  // namespace

void LoadScreenObj3(SCREENOBJ3& so, CBinaryInputStream* pbis)
{
    so.fExternal = pbis->S8Read();
    so.g80 = pbis->F32Read();
    so.ul84 = pbis->U32Read();
    so.ul88 = pbis->U32Read();
    so.ulAC = pbis->U32Read();
    if (so.fExternal)
    {
        so.abKey.resize(64);
        pbis->ReadRecord64(so.abKey.data());
        return;
    }
    LoadElems3(so.aelem, pbis);
    LoadGroups3(so.agroup, pbis);
}

const SELEM3* FONT3::PglyphFromCh(uint16_t ch) const
{
    for (size_t i = 0; i < pset->ach.size(); ++i)
        if (pset->ach[i] == ch) return &pset->aglyph[i];
    return nullptr;
}

float FONT3::DxFromCh(uint16_t ch) const
{
    for (size_t i = 0; i < pset->ach.size(); ++i)
        if (pset->ach[i] == ch) return pset->adx[i];
    return 0.0f;
}

std::vector<FONT3> FindFonts3(const std::vector<SCREENOBJ3>& aso)
{
    std::vector<FONT3> afont;
    for (const SCREENOBJ3& so : aso)
        for (size_t i = 0; i < so.aelem.size(); ++i)
            if (so.aelem[i].kind == 8 && !so.aelem[i].ach.empty())
            {
                FONT3 f;
                f.pso = &so;
                f.pset = &so.aelem[i];
                f.ielem = int(i);
                afont.push_back(f);
            }
    return afont;
}

bool FLoadScreenObjs3(const std::vector<uint8_t>& ab, const WORLDWALK3& w, std::vector<SCREENOBJ3>& aso, int& cMismatch,
                      std::string& error)
{
    CBinaryInputStream bis;
    bis.OpenMemory(int(ab.size()), ab.data());
    cMismatch = 0;
    for (const OBJHDR3& o : w.aobj)
    {
        if (o.cid < 0x9E || o.cid > 0xA7) continue;
        SCREENOBJ3 so;
        so.cid = o.cid;
        so.oid = o.oid;
        try
        {
            bis.m_ib = o.ibBody;
            std::vector<OPT> aopt;
            LoadOptionsFromBrx3(aopt, nullptr, &bis);   // orig3: 0x1C7D98 LoadLoFromBrx
            if (std::getenv("SLY3_SCREEN_TRACE"))
            {
                std::printf("screen object %d at %zu (after options %zu):", o.oid, o.ibBody, bis.m_ib);
                for (size_t k = 0; k < 48; ++k) std::printf(" %02X", ab[bis.m_ib + k]);
                std::printf("\n");
            }
            LoadScreenObj3(so, &bis);
        }
        catch (const std::exception& e)
        {
            error = "screen object " + std::to_string(o.oid) + " at " + std::to_string(o.ibBody) + ": " + e.what();
            return false;
        }
        if (bis.m_ib != o.ibBody + size_t(o.cb))
        {
            if (cMismatch++ < 8)
                std::fprintf(stderr, "screen object %d (cid 0x%X) at %zu: ended at +%zd of %d\n", o.oid, o.cid, o.ibBody,
                             std::ptrdiff_t(bis.m_ib - o.ibBody), o.cb);
        }
        aso.push_back(std::move(so));
    }
    return true;
}

bool FLoadScreenFile3(SCREENOBJ3& so, std::string& error, size_t* pcbFile)
{
    if (!so.fExternal || so.abKey.size() != 64) { error = "not external"; return false; }
    const FK fk = FkFromRecord(so.abKey.data());
    if (fk.cb == 0) { error = "no file"; return false; }
    const std::vector<uint8_t> ab = g_disc.ReadWorld(fk);
    if (pcbFile) *pcbFile = ab.size();
    if (std::getenv("SLY3_ZRB_TRACE"))
    {
        std::printf("external screen object %d: sector %u cb %u -> %zu bytes:", so.oid, unsigned(fk.isector), unsigned(fk.cb), ab.size());
        for (size_t k = 0; k < 32 && k < ab.size(); ++k) std::printf(" %02X", ab[k]);
        std::printf("\n");
    }
    if (ab.size() < 6 || std::memcmp(ab.data(), "ZRB.", 4) != 0) { error = "not a ZRB file"; return false; }
    CBinaryInputStream bis;
    bis.OpenMemory(int(ab.size()), ab.data());
    bis.m_ib = 6;
    try
    {
        LoadElems3(so.aelem, &bis);
        LoadGroups3(so.agroup, &bis);
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

int ScreenWorld3Command(const std::string& arg, bool fVerbose)
{
    const std::vector<WORLD3> aw = g_disc.Worlds();
    int cworld = 0, cfail = 0, cobjAll = 0, cmisAll = 0;
    for (const WORLD3& wd : aw)
    {
        if (wd.fk.cb == 0) continue;
        if (arg != "all" && wd.name != arg && std::to_string(wd.iworld) != arg) continue;
        ++cworld;
        const std::vector<uint8_t> ab = g_disc.ReadWorld(wd.fk);
        WORLDWALK3 walk;
        std::string error;
        if (!FWalkWorld3(ab, walk, error)) { std::printf("%-14s walk: %s\n", wd.name.c_str(), error.c_str()); ++cfail; continue; }
        std::vector<SCREENOBJ3> aso;
        int cmis = 0;
        if (!FLoadScreenObjs3(ab, walk, aso, cmis, error)) { std::printf("%-14s %s\n", wd.name.c_str(), error.c_str()); ++cfail; continue; }
        int cext = 0, celem = 0, cextOk = 0;
        for (SCREENOBJ3& so : aso)
        {
            if (so.fExternal)
            {
                ++cext;
                std::string errorExt;
                if (FLoadScreenFile3(so, errorExt)) ++cextOk;
                else if (fVerbose || arg != "all") std::printf("  external screen object %d: %s\n", so.oid, errorExt.c_str());
            }
            celem += int(so.aelem.size());
        }
        cmis += cext - cextOk;
        const std::vector<FONT3> afont = FindFonts3(aso);
        std::printf("%-14s %4zu screen objects (%d external, %d read), %6d elements, size ok %zu/%zu, %zu fonts\n", wd.name.c_str(),
                    aso.size(), cext, cextOk, celem, aso.size() - size_t(cmis), aso.size(), afont.size());
        if (fVerbose || arg != "all")
            for (const FONT3& f : afont)
            {
                uint16_t lo = 0xFFFF, hi = 0;
                for (uint16_t ch : f.pset->ach) { lo = std::min(lo, ch); hi = std::max(hi, ch); }
                std::printf("  font: oid %d element %d mode %d, %zu characters U+%04X..U+%04X\n", f.pso->oid, f.ielem,
                            f.pset->mode, f.pset->ach.size(), lo, hi);
            }
        cobjAll += int(aso.size());
        cmisAll += cmis;
        if (cmis) ++cfail;
    }
    std::printf("%d worlds, %d failed; %d screen objects, %d not ending at their size\n", cworld, cfail, cobjAll, cmisAll);
    return cfail ? 1 : 0;
}

// --text-test3: a string in a world's text font, drawn by a small software rasterizer (a check of the glyphs; the
// game draws them through the HUD's screen-object path)
#include "text3.h"
#include <stb/stb_image_write.h>
#include <cmath>
#include <cstring>

namespace
{
std::u16string Utf16FromUtf8(const std::string& s)
{
    std::u16string r;
    for (size_t i = 0; i < s.size();)
    {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 7; n = 3; }
        else if (c >= 0xE0) { cp = c & 0xF; n = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(s[i]) & 0x3F);
        r.push_back(char16_t(cp <= 0xFFFF ? cp : 0xFFFD));
    }
    return r;
}

struct CANVAS3
{
    int dx = 0, dy = 0;
    std::vector<uint8_t> argba;
    void Tri(glm::vec2 a, glm::vec2 b, glm::vec2 c, const uint8_t rgba[4])
    {
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) return;
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(dx - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(dy - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
                const float w0 = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / area;
                const float w1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                std::memcpy(&argba[(size_t(y) * size_t(dx) + size_t(x)) * 4], rgba, 4);
            }
    }
};

// A glyph's polygons (Sly 2's CHud::DrawSprite: coordinates / 16, the advance widths in pixels; primitive 1 a fan,
// 3 a line strip (skipped), else a strip; a vertex byte with bit 7 does not kick; the polygon's colour is its last
// vertex's; the depth is the control byte (x 100): a hole (alpha 0, e.g. the inside of an 'o', control 1) is listed
// first but lies over the shape (control 0))
void DrawGlyph3(CANVAS3& cv, const SELEM3& g, glm::vec2 org, float scale, const uint8_t rgbaInk[4], const uint8_t rgbaBack[4])
{
    std::vector<const SPOLY3*> apoly;
    for (const SPOLY3& p : g.apoly) apoly.push_back(&p);
    std::stable_sort(apoly.begin(), apoly.end(), [](const SPOLY3* a, const SPOLY3* b) { return a->control < b->control; });
    for (const SPOLY3* pp : apoly)
    {
        const SPOLY3& p = *pp;
        if (p.primitive == 3 || p.avtx.empty()) continue;
        const uint32_t rgba = p.avtx.back().rgba;
        const uint8_t* pc = (rgba >> 24) == 0 ? rgbaBack : rgbaInk;
        for (size_t i = 2; i < p.avtx.size(); ++i)
        {
            if (p.fVertexBytes && (p.avtx[i].b & 0x80)) continue;
            const SVTX3& va = p.primitive == 1 ? p.avtx[0] : p.avtx[i - 2];
            auto pos = [&](const SVTX3& v) { return org + glm::vec2(v.x, v.y) / 16.0f * scale; };
            cv.Tri(pos(va), pos(p.avtx[i - 1]), pos(p.avtx[i]), pc);
        }
    }
}
}  // namespace

int TextTest3Command(const std::string& world, const std::string& text, const std::string& out)
{
    const std::vector<WORLD3> aw = g_disc.Worlds();
    const WORLD3* pwd = nullptr;
    for (const WORLD3& wd : aw)
        if (wd.name == world || std::to_string(wd.iworld) == world) pwd = &wd;
    if (!pwd || pwd->fk.cb == 0) { std::fprintf(stderr, "no world %s\n", world.c_str()); return 1; }
    const std::vector<uint8_t> ab = g_disc.ReadWorld(pwd->fk);
    WORLDWALK3 walk;
    std::string error;
    if (!FWalkWorld3(ab, walk, error)) { std::fprintf(stderr, "walk: %s\n", error.c_str()); return 1; }
    std::vector<SCREENOBJ3> aso;
    int cmis = 0;
    if (!FLoadScreenObjs3(ab, walk, aso, cmis, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    const std::vector<FONT3> afont = FindFonts3(aso);
    if (afont.empty()) { std::fprintf(stderr, "no font\n"); return 1; }
    const FONT3& font = afont[0];
    std::u16string str;
    if (!text.empty() && text[0] == '#')
    {
        TEXT3 t;
        CBinaryInputStream bis;
        bis.OpenMemory(int(ab.size()), ab.data());
        bis.m_ib = walk.ibText;
        LoadTextFromBrx3(&t, 0, &bis);
        const std::u16string* ps = PstrText3(t, std::atoi(text.c_str() + 1));
        if (!ps) { std::fprintf(stderr, "no text %s\n", text.c_str()); return 1; }
        str = *ps;
    }
    else str = Utf16FromUtf8(text);
    if (const char* sz = std::getenv("SLY3_GLYPH"))   // debugging: one glyph's data
    {
        const uint16_t ch = uint16_t(sz[0]);
        const SELEM3* pg = font.PglyphFromCh(ch);
        std::printf("glyph '%c' dx %g kind %u n %d polys %zu\n", sz[0], font.DxFromCh(ch), pg ? pg->kind : 0, pg ? pg->n : 0, pg ? pg->apoly.size() : 0);
        if (pg)
            for (const SPOLY3& p : pg->apoly)
            {
                std::printf("  ctl %d prim %d u %d vb %d:", p.control, p.primitive, p.unused, p.fVertexBytes);
                for (const SVTX3& v : p.avtx) std::printf(" (%d,%d b%02X c%08X)", v.x, v.y, v.b, v.rgba);
                std::printf("\n");
            }
    }
    // the line height: the font's glyph extents
    float yMin = 1e9f, yMax = -1e9f;
    for (const SELEM3& g : font.pset->aglyph)
        for (const SPOLY3& p : g.apoly)
            for (const SVTX3& v : p.avtx) { yMin = std::min(yMin, v.y / 16.0f); yMax = std::max(yMax, v.y / 16.0f); }
    const float scale = 1.0f, dyLine = (yMax - yMin) * 1.1f;
    CANVAS3 cv;
    cv.dx = 640;
    const float dxWrap = 600.0f;
    // words wrapped at dxWrap
    std::vector<std::u16string> aline(1);
    float xLine = 0;
    for (size_t i = 0; i < str.size();)
    {
        size_t j = i;
        while (j < str.size() && str[j] != u' ') ++j;
        float dxWord = 0;
        for (size_t k = i; k < j; ++k) dxWord += font.DxFromCh(uint16_t(str[k])) * scale;
        const float dxSpace = font.DxFromCh(u' ') * scale;
        if (xLine > 0 && xLine + dxWord > dxWrap) { aline.emplace_back(); xLine = 0; }
        aline.back() += str.substr(i, j - i);
        xLine += dxWord;
        if (j < str.size()) { aline.back() += u' '; xLine += dxSpace; }
        i = j + 1;
    }
    cv.dy = int(std::ceil(dyLine * float(aline.size()) + 16));
    cv.argba.assign(size_t(cv.dx) * size_t(cv.dy) * 4, 0);
    const uint8_t rgbaBack[4] = {0x30, 0x20, 0x50, 0xFF}, rgbaInk[4] = {0xC8, 0xC8, 0xF0, 0xFF};
    for (size_t k = 0; k < cv.argba.size(); k += 4) std::memcpy(&cv.argba[k], rgbaBack, 4);
    int cmissing = 0;
    for (size_t l = 0; l < aline.size(); ++l)
    {
        glm::vec2 org(16.0f, 8.0f - yMin + dyLine * float(l));
        for (char16_t ch : aline[l])
        {
            const SELEM3* pg = font.PglyphFromCh(uint16_t(ch));
            if (!pg) { ++cmissing; continue; }
            DrawGlyph3(cv, *pg, org, scale, rgbaInk, rgbaBack);
            org.x += font.DxFromCh(uint16_t(ch)) * scale;
        }
    }
    const bool fOk = stbi_write_png(out.c_str(), cv.dx, cv.dy, 4, cv.argba.data(), cv.dx * 4) != 0;
    std::printf("%s %s: \"%s\" in font %d of oid %d (%zu characters), %zu lines, %d characters without a glyph\n",
                fOk ? "wrote" : "FAILED to write", out.c_str(), Utf8FromUtf16(str).c_str(), font.ielem, font.pso->oid,
                font.pset->ach.size(), aline.size(), cmissing);
    return fOk ? 0 : 1;
}
