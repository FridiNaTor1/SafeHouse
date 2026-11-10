// See screenobj.h. Reference ports: Sly2-Reference game/m_210E90_p3.cpp, m_210E90_c.cpp.

#include "screenobj.h"

#include "langstore.h"
#include "loadtrace.h"
#include "region.h"

#include <algorithm>
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
    }
}

const SELEM* FONTBRX::PglyphFromCh(uint32_t ch) const
{
    // orig: 0x00215A50 CFontBrx::PglyffFromCh (binary search; 1-based)
    auto it = std::lower_bound(pset->aul.begin(), pset->aul.end(), uint32_t(ch));
    if ((it == pset->aul.end() || *it != ch) && (ch == '{' || ch == '}')) return PglyphFromCh(ch == '{' ? '(' : ')');   // (PC: L3 / R3, promptglyph.cpp)
    if (it == pset->aul.end() || *it != ch) return nullptr;
    const size_t i = size_t(it - pset->aul.begin()) + 1;
    return i < pset->asub.size() ? &pset->asub[i] : nullptr;
}

float FONTBRX::DxFromCh(uint32_t ch) const
{
    const auto it = std::lower_bound(pset->aul.begin(), pset->aul.end(), uint32_t(ch));
    if ((it == pset->aul.end() || *it != ch) && (ch == '{' || ch == '}')) return DxFromCh(ch == '{' ? '(' : ')');   // (PC: L3 / R3)
    if (it == pset->aul.end() || *it != ch) return 0.0f;
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
