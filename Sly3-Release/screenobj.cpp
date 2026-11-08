// See screenobj.h. Reference ports: Sly2-Reference game/m_210E90_p3.cpp, m_210E90_c.cpp.

#include "screenobj.h"

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
void LoadElemBodyFromBrx(SELEM& e, CBinaryInputStream* pbis, int depth);

// orig3: 0x00263748 LoadElemFromBrx (Sly 2 0x2149D8): U32 kind (jump table 0x327F40), then the kind's data. The old
// Sly 3 port's census: every screen object of the 39 worlds (1,671, and the 234 external files) ends at its size.
void LoadElemBodyFromBrx(SELEM& e, CBinaryInputStream* pbis, int depth)
{
    if (depth > 8) throw std::runtime_error("screen elements nested too deep");
    e.kind = pbis->U32Read();
    switch (e.kind)
    {
    case SELEMK_Sprite:   // 0x263E54: S32, U16 polygons x {U8 vertices, U8 control, U8 primitive, U8, U8 vertex bytes;
                          // per vertex U32 colour (0x236970), U8, S16 x, S16 y}
    {
        e.n = pbis->S32Read();
        e.apoly.resize(pbis->U16Read());
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
                poly.rgba = pbis->U32Read();   // (the last vertex's word, as Sly 2's loader keeps it)
                poly.ab[j] = pbis->U8Read();
                poly.axy[j][0] = pbis->S16Read();
                poly.axy[j][1] = pbis->S16Read();
            }
        }
        break;
    }
    case SELEMK_Sound:    // 0x2639E0, the text box: U32 colour +0x14, F32 size +0x18, width +0x1C, height +0x20, S8 halign
                          // +0x24, S8 kind +0xC (0 localized, 1 a script string), S8 the font element +0x2C, then the
                          // S32 id +0x10 when the kind is 0 or 1
        e.aul3[1] = pbis->U32Read();
        e.ag3[0] = pbis->F32Read();
        e.ag3[2] = pbis->F32Read();
        e.ag3[1] = pbis->F32Read();
        e.ab3[0] = pbis->S8Read();
        e.ab3[1] = pbis->S8Read();
        e.ab3[2] = pbis->S8Read();
        if (e.ab3[1] == 1 || e.ab3[1] == 0) e.n3 = pbis->S32Read();
        break;
    case SELEMK_Ref:      // 0x2637C8: S32, U16 the group
        e.n = pbis->S32Read();
        e.ref = pbis->U16Read();
        break;
    case SELEMK_Image:
    case SELEMK_Quad:     // 0x263AA8
        e.x0 = pbis->S16Read();
        e.y0 = pbis->S16Read();
        e.x1 = pbis->S16Read();
        e.y1 = pbis->S16Read();
        e.idTexture = pbis->S32Read();
        break;
    case SELEMK_Set:      // 0x2637E4, a font: U8 mode, U16 count x {U16 character (UTF-16), F32 advance, its glyph
                          // (an element without index)}. (The original keeps only the characters the world's text uses,
                          // 0x14FEB0, by the mode; the port keeps all.) Kept sorted by character for FONTBRX.
    {
        pbis->U8Read();
        const int c = pbis->U16Read();
        std::vector<std::pair<uint32_t, std::pair<float, SELEM>>> aglyph(static_cast<std::size_t>(c));
        for (auto& [ch, g] : aglyph)
        {
            ch = pbis->U16Read();
            g.first = pbis->F32Read();
            LoadElemBodyFromBrx(g.second, pbis, depth + 1);
        }
        std::stable_sort(aglyph.begin(), aglyph.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        e.asub.assign(1, SELEM{});
        for (auto& [ch, g] : aglyph)
        {
            if (!e.aul.empty() && e.aul.back() == ch) continue;
            e.aul.push_back(ch);
            e.ag.push_back(g.first);
            g.second.idx = uint32_t(e.asub.size());
            e.asub.push_back(std::move(g.second));
        }
        break;
    }
    default:              // 0, 2 (a transform: no data in Sly 3), 5 (0x264384)
        break;
    }
}

// orig3: 0x00263118 (Sly 2 0x214398): U16 count, then count x {U16 index, the element}; indices 0..count
void LoadElemsFromBrx(std::vector<SELEM>& aelem, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x263118, pbis);
    const int c = pbis->U16Read();
    aelem.assign(size_t(c) + 1, SELEM{});
    for (int i = 1; i <= c; ++i)
    {
        const uint32_t idx = pbis->U16Read();
        if (idx >= aelem.size()) throw std::runtime_error("screen element index out of range");
        SELEM& e = aelem[idx];
        e.idx = idx;
        LoadElemBodyFromBrx(e, pbis, 0);
    }
}

// orig3: 0x002631C8 (Sly 2 0x214460), read into Sly 2's structs for the copied player (splayer.h):
// U16 groups x {U16 frame sets, U16 frames, U16 parts x {F32 dt, S32 id, U16 keys x {F32 t, U8 items, U8 colours,
// U8 released slots, U8 (bit 0: the entry held), U8 (3 bits: an event), S32 (the event's argument), items, colours,
// released slots}}}. An item (CrossKey 0x2686A0's packed word): U16 (10 bits) the element, U8 the slot, U8 (6 bits)
// the masked section's link, U8 (1 bit) interpolated to the slot's next key, U8 the key colour (0x7F: none), S32 its
// identity, a 3x3 matrix (the 2x3 kept). Sly 2's equivalents: the hold = trigger byte 7 (0x2683F8), the events 1..4
// (a sound, the sound stopped, a dialog line, the dialog stopped; 0x268438) = Sly 2's tag kinds 0..3, the released
// slots (0x2685B0) = Sly 2's U16 list.
void LoadGroupsFromBrx(std::vector<SGROUP>& agroup, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x2631C8, pbis);
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
                key.acolor.resize(pbis->U8Read());
                key.aw.resize(pbis->U8Read());
                const bool fHold = (pbis->U8Read() & 1) != 0;
                const int event = pbis->U8Read() & 7;
                const int32_t nEvent = pbis->S32Read();
                if (fHold) key.ab.push_back(7);
                if (event >= 1 && event <= 4) key.atag.push_back({uint8_t(event - 1), nEvent});
                for (SITEM& it : key.aitem)
                {
                    it.aw[0] = pbis->U16Read() & 0x3FF;
                    it.aw[1] = pbis->U8Read();
                    it.aw[2] = pbis->U8Read() & 0x3F;
                    it.b30 = pbis->U8Read() & 1;
                    const int color = std::min<int>(pbis->U8Read(), 0x7F);
                    it.s6 = color == 0x7F ? int16_t(-1) : int16_t(color);
                    it.n34 = pbis->S32Read();
                    it.mat = Mat3x2FromBrx(pbis);
                }
                for (auto& c : key.acolor) { c[1] = pbis->U32Read(); c[0] = pbis->U32Read(); }
                for (uint16_t& w : key.aw) w = pbis->U8Read();
            }
        }
    }
}
}  // namespace

// orig3: 0x00260810 (after the LO's options: S8 external, F32 +0x80, U32 +0x84 / +0x88 (the canvas: m_ext's HUD 640 x
// 480) / +0xAC; external: the file's 64-byte key (0x119948), else the element set and the group tree). Sly 2's
// 0x212298 had the same reads at +0x10 (the structs keep Sly 2's names: g90 = +0x80, ul94 = +0x84, ...).
void LoadScreenObjFromBrx(SCREENOBJ* pso, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x260810, pbis);
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
    const auto it = std::lower_bound(pset->aul.begin(), pset->aul.end(), uint32_t(ch));
    if (it == pset->aul.end() || *it != ch) return nullptr;
    const size_t i = size_t(it - pset->aul.begin()) + 1;
    return i < pset->asub.size() ? &pset->asub[i] : nullptr;
}

float FONTBRX::DxFromCh(uint32_t ch) const
{
    const auto it = std::lower_bound(pset->aul.begin(), pset->aul.end(), uint32_t(ch));
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
