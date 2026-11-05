// Meshes: Sly 2's LoadGlob 0x167320 and the mesh loaders 0x16A000 / 0x16E438 / 0x16CAA0 / 0x16CF78, the
// sub-mesh instances 0x169AC8, the index tables 0x16C8E0 and the sphere sets 0x167870, as native loaders.
// The stream format follows the original exactly; what the original turns into VIF chains for the VU1 programs
// is kept as vertex arrays (see glob.h). Reference ports: Sly2-Reference game/m_167310.cpp, m_16A000_p3.cpp,
// m_16E1A8.cpp.

#include "glob.h"

#include "loadtrace.h"
#include "sly3/condset3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

GLOBSTATS g_globstats;

namespace
{
constexpr float G_NONE = std::numeric_limits<float>::max();   // 0x2A5270: the "not set" value of the read floats
constexpr uint32_t MRD_FAR_BITS = 0x501502F9u;                 // 1e10

float FloatFromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// The VU's expansion of an RGB5A1 colour (UNPACK V4-5): 5-bit channels << 3, alpha bit -> 0x80.
uint32_t RgbaFromRgba16(uint16_t c)
{
    const uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    const uint32_t a = (c & 0x8000) ? 0x80 : 0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

// (x * m + 255) / 510: a colour's alpha against a material's (PS2 alpha, 0x80 = 1.0)
uint32_t MulAlpha(uint32_t a, uint32_t m) { return (a * m + 0xFF) / 0x1FE; }

// 0x16A000's unlit colour: vertex colour x material colour in RGB5A1 (channels (c*m + 255) >> 11), and the alpha.
uint16_t Rgba16Unlit(uint32_t rgbaVtx, uint32_t rgbaMat, uint32_t* pa)
{
    auto ch = [&](int i) { return ((((rgbaVtx >> (8 * i)) & 0xFF) * ((rgbaMat >> (8 * i)) & 0xFF) + 0xFF) >> 11) & 0x1F; };
    const uint32_t a = MulAlpha(rgbaVtx >> 24, rgbaMat >> 24) & 0xFF;
    *pa = a;
    return static_cast<uint16_t>(ch(0) | (ch(1) << 5) | (ch(2) << 10) | ((a >> 7) << 15));
}

// orig3: 0x18D148's bone list (U8 count, {S32 oid, S16 bone}; Sly 2 read it in each mesh loader)
void ReadBones(GLOB* pglob, CBinaryInputStream* pbis)
{
    pglob->cbone = static_cast<int8_t>(pbis->U8Read());
    pglob->abone.resize(size_t(std::max<int>(0, pglob->cbone)));
    for (GLOBBONE& bone : pglob->abone)
    {
        bone.oid = pbis->S32Read();
        bone.ibone = pbis->S16Read();
    }
}

// orig3: 0x1929B0 (an instance colour-set table, see glob.h IDXTAB; null when no group's condition holds)
std::shared_ptr<IDXTAB> LoadIdxTabFromBrx(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1929B0, pbis);
    const int cgroup = pbis->U8Read();
    std::shared_ptr<IDXTAB> ptab;
    for (int g = 0; g < cgroup; ++g)
    {
        const int32_t cond = pbis->S32Read();
        const int cset = pbis->U16Read();
        if (!FConditionMatches3(cond))
        {
            for (int i = 0; i < cset; ++i)
            {
                (void)pbis->U16Read();
                const int n = pbis->U16Read();
                pbis->Skip(size_t(n) * 5);
            }
            continue;
        }
        // (the original allocates a new table for each matching group and returns the last)
        ptab = std::make_shared<IDXTAB>();
        ptab->aset.resize(size_t(cset));
        for (int i = 0; i < cset; ++i)
        {
            const int ibatch = pbis->U16Read();
            const int n = pbis->U16Read();
            auto& set = ptab->aset[size_t(i)];
            set.resize(size_t(n));
            for (IDXTAB::REC& rec : set)
            {
                rec.rgba16A = pbis->U16Read();
                rec.rgba16B = pbis->U16Read();
                rec.a = pbis->U8Read();
                AdjustRgba16(&rec.rgba16A);
                AdjustRgba16(&rec.rgba16B);
            }
            if (ibatch >= int(ptab->aiset.size())) ptab->aiset.resize(size_t(ibatch) + 1, -1);
            // (0x18F7B0 / 0x18FF98 take the first set whose key is the batch)
            if (ptab->aiset[size_t(ibatch)] < 0) ptab->aiset[size_t(ibatch)] = i;
        }
    }
    return ptab;
}

// Colours of a type-0 entry from an instance table (0x16A000 per-vertex loop, flag 0x20; 0x169AC8 recolour).
void ColorFromTable(GLOB* pglob, const SUBGLOB& sub, const IDXTAB& tab, bool fAlphaA)
{
    if (sub.ibatch >= static_cast<int>(tab.aiset.size())) return;
    const int iset = tab.aiset[size_t(sub.ibatch)];
    if (iset < 0 || iset >= static_cast<int>(tab.aset.size())) return;
    const auto& set = tab.aset[size_t(iset)];
    for (uint32_t k = 0; k < sub.cvtx; ++k)
    {
        VTXGLOB& vtx = pglob->avtx[sub.ivtxFirst + k];
        if (vtx.isrc >= set.size()) continue;
        const IDXTAB::REC& rec = set[vtx.isrc];
        const uint32_t aA = vtx.rgbaA >> 24;
        vtx.rgbaA = (RgbaFromRgba16(rec.rgba16A) & 0x00FFFFFF) | (fAlphaA ? aA << 24 : (RgbaFromRgba16(rec.rgba16A) & 0xFF000000));
        vtx.rgbaB = (RgbaFromRgba16(rec.rgba16B) & 0x00FFFFFF) | (uint32_t(rec.a) << 24);
        vtx.fUnbaked = 0;
    }
}

// Triangles of the strips of a type-0 GLOB: a triangle ends at each strip vertex whose ADC bit is clear.
void BuildTriangles(GLOB* pglob)
{
    pglob->aindex.clear();
    for (SUBGLOB& sub : pglob->asubglob)
    {
        sub.firstIndex = static_cast<uint32_t>(pglob->aindex.size());
        for (uint32_t k = 2; k < sub.cvtx; ++k)
        {
            const uint32_t i = sub.ivtxFirst + k;
            if (pglob->avtx[i].fAdc) continue;
            if (pglob->avtx[i - 1].fAdc) { ++g_globstats.cstrip; if (k & 1) ++g_globstats.cstripOdd; }
            // Keep a consistent winding along the strip (odd triangles are flipped).
            // The winding alternates with the vertex's position in the entry (not from the strip's start): checked
            // against PCSX2 with back-face culling (Paris briefing view).
            if (k & 1) { pglob->aindex.push_back(i - 1); pglob->aindex.push_back(i - 2); }
            else { pglob->aindex.push_back(i - 2); pglob->aindex.push_back(i - 1); }
            pglob->aindex.push_back(i);
        }
        sub.indexCount = static_cast<uint32_t>(pglob->aindex.size()) - sub.firstIndex;
        g_globstats.ctri += sub.indexCount / 3;
    }
}

// orig3: 0x18F7B0 LoadGlobSubmesh (Sly 2 0x169AC8: an instance of the template: two S8s (Sly 2: one), [the draw
// record's U32], its owner (S16), matrix, S32, hidden bit and colour table; type-0 unlit templates (grfMesh3 & 3 == 0)
// get their colours from the table)
void LoadGlobSubmeshFromBrx(GLOBSET* pglobset, int iglobTemplate, bool fDraw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x18F7B0, pbis);
    (void)pbis->S8Read();
    (void)pbis->S8Read();
    GLOB glob;
    {
        const GLOB& tmpl = pglobset->aglob[size_t(iglobTemplate)];
        // The template's header, group and draw record (the geometry is shared, not copied).
        glob.meshk = tmpl.meshk; glob.rp = tmpl.rp; glob.rp3 = tmpl.rp3; glob.cbone = tmpl.cbone; glob.grfMesh = tmpl.grfMesh;
        glob.grfMesh3 = tmpl.grfMesh3; glob.rgbaCel = tmpl.rgbaCel;
        glob.gZOrder = tmpl.gZOrder; glob.uFog = tmpl.uFog; glob.rtck = tmpl.rtck; glob.grfGroup = tmpl.grfGroup;
        glob.fDraw = tmpl.fDraw; glob.posCenter = tmpl.posCenter; glob.sRadius = tmpl.sRadius; glob.sMRD = tmpl.sMRD;
        glob.psaa = tmpl.psaa; glob.abone = tmpl.abone; glob.nAlphaCtl = tmpl.nAlphaCtl;
        glob.fHidden = tmpl.fHidden;
        glob.iSpec = tmpl.iSpec; glob.posSpec = tmpl.posSpec;
        std::copy(std::begin(tmpl.agSpec), std::end(tmpl.agSpec), std::begin(glob.agSpec));
        std::copy(std::begin(tmpl.agClamp), std::end(tmpl.agClamp), std::begin(glob.agClamp));
    }
    if (fDraw) glob.u14 = pbis->U32Read();
    glob.oidOwner = pbis->S16Read();
    glob.dmat = pbis->ReadMatrix4();
    glob.fMatrix = true;
    glob.n0 = pbis->S32Read();
    const int f = pbis->S8Read();
    glob.fHidden = (f & 1) != 0;
    glob.grfGroup = (glob.grfGroup & ~0x800u) | (uint32_t(f & 1) << 11);
    glob.iglobTemplate = iglobTemplate;
    std::shared_ptr<IDXTAB> ptab = LoadIdxTabFromBrx(pbis);
    const GLOB& tmpl = pglobset->aglob[size_t(iglobTemplate)];
    if (!(tmpl.grfMesh3 & 3) && tmpl.meshk == MESHK_Strips && ptab)
    {
        // The chain is duplicated and the 0x20 entries recoloured: native instances get their own vertex copy
        // only when they are recoloured.
        glob.avtx = tmpl.avtx;
        glob.asubglob = tmpl.asubglob;
        glob.aindex = tmpl.aindex;
        for (const SUBGLOB& sub : glob.asubglob)
            if (sub.grf & 0x20) ColorFromTable(&glob, sub, *ptab, true);
        glob.pidxtab = ptab;
    }
    pglobset->aglob.push_back(std::move(glob));
    ++g_globstats.cinstance;
}

// orig3: 0x18F6B8 (the c instances of the template that follow its mesh)
void LoadGlobSubmeshesFromBrx(GLOBSET* pglobset, int iglobTemplate, int csub, bool fDraw, CBinaryInputStream* pbis)
{
    for (int i = 0; i < csub; ++i) LoadGlobSubmeshFromBrx(pglobset, iglobTemplate, fDraw, pbis);
}

// orig3: 0x18FF98 LoadGlobMesh0 (Sly 2 0x16A000; vertex pool + strips per batch, see glob.h). Sly 3's stream: U8
// mesh flags, S8 animation index (Sly 2: S16), the view-angle block, the colour sets (0x1929B0), the warp block
// (flag 0x20; Sly 2: 4), a box (flag 8: 3 vectors, 0x195028), then the batches as Sly 2's (the bones are read by
// LoadGlobsetFromBrx now).
void LoadGlobMesh0FromBrx(GLOBSET* pglobset, int iglob, int csub, bool fDraw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x18FF98, pbis);
    GLOB* pglob = &pglobset->aglob[size_t(iglob)];
    const uint8_t g3 = pbis->U8Read();
    pglob->grfMesh3 = g3;
    // (flag 1 -> group 0x100 forward-lit, flag 2 -> group 0x200, as Sly 2's 0x16A000: the asm 0x18FFF4..0x190024)
    pglob->grfMesh = uint8_t((g3 & 3) | ((g3 & 0x20) ? 4 : 0) | ((g3 & 0x80) ? 0x10 : 0));
    if (g3 & 1) pglob->grfGroup |= 0x100;
    if (g3 & 2) pglob->grfGroup |= 0x200;
    {
        const int isaa = pbis->S8Read();
        if (isaa >= 0 && isaa < static_cast<int>(pglobset->apsaa.size())) pglob->psaa = pglobset->apsaa[size_t(isaa)];
    }
    if (g3 & 3) pglobset->grfglobset |= 1ull << 32;
    // The view-angle block (SPECPARAMS): S8 kind, 3 floats, 2 floats, a vector (P_G at draw time).
    pglob->iSpec = static_cast<int8_t>(pbis->S8Read());
    if (pglob->iSpec != -1)
    {
        pbis->ReadCb12(pglob->agSpec);
        pbis->ReadCb8(pglob->agClamp);
        pglob->posSpec = pbis->ReadVector();
        pglob->agSpec[1] *= 0.007874016f;
        pglob->agSpec[2] *= 6.200012e-05f;
    }
    std::shared_ptr<IDXTAB> ptab = LoadIdxTabFromBrx(pbis);
    // The warp block (flag 0x20; Sly 1's WRBG).
    if (g3 & 0x20)
    {
        auto pwrbg = std::make_shared<WRBG>();
        pwrbg->oid = pbis->S32Read();
        pwrbg->wek = pbis->S8Read();
        if (pwrbg->wek != -1)
        {
            pwrbg->sInner = pbis->F32Read();
            pwrbg->uInner = pbis->F32Read();
            pwrbg->sOuter = pbis->F32Read();
            pwrbg->uOuter = pbis->F32Read();
            pwrbg->dmat = pbis->ReadMatrix4();
        }
        pwrbg->cmat = pbis->U8Read();
        pwrbg->fDpos = pbis->U8Read() != 0;
        pwrbg->fDuv = pbis->U8Read() != 0;
        pwrbg->iglob = iglob;
        pglob->pwrbg = std::move(pwrbg);
    }
    // orig3: 0x195028 (flag 8: three vectors, a box the warp state is built from: TODO(sly3) what it drives)
    if (g3 & 8)
    {
        (void)pbis->ReadVector();
        (void)pbis->ReadVector();
        (void)pbis->ReadVector();
        ++g_globstats.cbox;
    }
    const int cbatch = pbis->S16Read();
    std::vector<glm::vec3> apos, anormal;
    std::vector<glm::vec2> auv;
    std::vector<uint32_t> argba;
    std::vector<uint8_t> aw, aisrc, amask;
    for (int ibatch = 0; ibatch < cbatch; ++ibatch)
    {
        const int c = pbis->U8Read();
        struct ENT { uint32_t count, ishd, grf; };
        std::vector<ENT> aent(static_cast<size_t>(c));
        for (ENT& ent : aent) pbis->ReadBytes(&ent, sizeof(ent));
        const int M = pbis->U8Read();
        apos.resize(size_t(M)); anormal.resize(size_t(M)); auv.resize(size_t(M)); argba.resize(size_t(M));
        pbis->ReadBytes(apos.data(), size_t(M) * 12);
        pbis->ReadBytes(auv.data(), size_t(M) * 8);
        pbis->ReadBytes(anormal.data(), size_t(M) * 12);
        pbis->ReadBytes(argba.data(), size_t(M) * 4);
        for (uint32_t& rgba : argba) AdjustRgba(&rgba);
        // The alpha control: whether each vertex's alpha byte is sent (else the RGB5A1 alpha bit). Sly 3: rp < 2
        // keeps it, 2..7 send the byte (Sly 2: rp < 3, 3..8).
        const int nAlpha = pbis->S8Read();
        pglob->nAlphaCtl = static_cast<int8_t>(nAlpha);
        int fAlphaA = nAlpha;
        if (pglob->rp3 < 8) fAlphaA = pglob->rp3 < 2 ? nAlpha : 1;
        uint8_t aiboneSlot[4] = {};
        int cw = 0;
        if (pglob->cbone != 0)
        {
            for (uint8_t& b : aiboneSlot) b = pbis->U8Read();
            cw = std::min<int>(pglob->cbone, 4);
            aw.resize(size_t(cw) * size_t(M));
            pbis->ReadBytes(aw.data(), aw.size());
        }
        uint32_t N = 0;
        for (const ENT& ent : aent) N += ent.count;
        aisrc.resize(N);
        pbis->ReadBytes(aisrc.data(), N);
        amask.resize(((N + 31) / 32) * 4);
        pbis->ReadBytes(amask.data(), amask.size());

        uint32_t kTotal = 0;
        for (const ENT& ent : aent)
        {
            SUBGLOB sub;
            sub.pshd = PshdFromIndex(static_cast<int>(ent.ishd));
            if (!sub.pshd) ++g_globstats.cnoshd;
            sub.grf = ent.grf;
            if (FSaaForShader(pglob->psaa, sub.pshd)) sub.psaa = pglob->psaa;
            else if (sub.pshd && FSaaForShader(sub.pshd->psaa, sub.pshd)) sub.psaa = sub.pshd->psaa;
            if (sub.psaa)
            {
                if (sub.psaa->n20 & 1) sub.grf |= 0x10;
                if (sub.psaa->n20 & 2) sub.grf |= 8;
            }
            if (sub.pshd)
            {
                if (sub.pshd->shdk == 3) sub.grf |= 6;
                else if (sub.pshd->shdk == 7 && !(sub.pshd->grfshd & 0x10)) sub.grf |= 4;
            }
            sub.ibatch = ibatch;
            sub.ivtxFirst = static_cast<uint32_t>(pglob->avtx.size());
            sub.cvtx = ent.count;
            const uint32_t rgbaMat = sub.pshd ? sub.pshd->argba[0] : 0x80808080u;
            const uint8_t shdk = sub.pshd ? sub.pshd->shdk : 0;
            for (uint32_t k = 0; k < ent.count; ++k, ++kTotal)
            {
                const uint32_t b = aisrc[kTotal];
                if (b >= uint32_t(M)) throw std::runtime_error("type-0 mesh: vertex index outside the pool");
                VTXGLOB vtx;
                vtx.pos = apos[b];
                vtx.normal = anormal[b];
                vtx.uv = auv[b];
                vtx.isrc = static_cast<uint16_t>(b);
                vtx.rgbaSrc = argba[b];
                vtx.fAdc = (amask[kTotal / 8] >> (kTotal % 8)) & 1;
                for (int j = 0; j < cw; ++j)
                {
                    vtx.abone[j] = aiboneSlot[j];
                    vtx.aw[j] = aw[size_t(b) * size_t(cw) + size_t(j)];
                }
                uint32_t a = 0;
                const uint16_t c16 = Rgba16Unlit(argba[b], rgbaMat, &a);
                vtx.rgbaA = (RgbaFromRgba16(c16) & 0x00FFFFFF) | ((fAlphaA ? a : (RgbaFromRgba16(c16) >> 24)) << 24);
                if (shdk == 1 || shdk == 7 || (pglob->grfMesh3 & 1))
                {
                    // Lit by the light bake (P_M, kind-0 RPLs) unless the colours come from the table, or forward-lit (mesh
                    // flag 1: P_G with the light context); light3.h / light.cpp fill these in. Keep the bake's input colour.
                    vtx.rgbaB = 0;
                    vtx.fUnbaked = 1;
                }
                pglob->avtx.push_back(vtx);
            }
            if ((sub.grf & 0x20) && ptab) ColorFromTable(pglob, sub, *ptab, fAlphaA != 0);
            pglob->asubglob.push_back(sub);
        }
        g_globstats.cvtx += static_cast<int>(N);
    }
    pglob->pidxtab = ptab;
    if (pglob->cbone) ++g_globstats.cskinned;
    BuildTriangles(pglob);
    LoadGlobSubmeshesFromBrx(pglobset, iglob, csub, fDraw, pbis);
}

// orig3: 0x194558 (Sly 2 0x16E438: type 1, the cel border: a U32 colour (new), then per strip positions, weights
// and the edges)
void LoadGlobMesh1FromBrx(GLOBSET* pglobset, int iglob, int csub, bool fDraw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x194558, pbis);
    GLOB* pglob = &pglobset->aglob[size_t(iglob)];
    pglob->rgbaCel = pbis->U32Read();
    const int cstrip = pbis->S16Read();
    for (int i = 0; i < cstrip; ++i)
    {
        SUBCEL cel;
        const int cv = pbis->U8Read();
        cel.apos.resize(size_t(cv));
        pbis->ReadBytes(cel.apos.data(), size_t(cv) * 12);
        if (pglob->cbone != 0)
        {
            cel.cw = pbis->U8Read();
            for (int j = 0; j < cel.cw; ++j)
            {
                const uint8_t b = pbis->U8Read();
                if (j < 4) cel.aiboneSlot[j] = b;
            }
            cel.aw.resize(size_t(cv) * size_t(cel.cw));
            pbis->ReadBytes(cel.aw.data(), cel.aw.size());
        }
        const int cn = pbis->U8Read();
        cel.aedge.resize(size_t(cn) * 4);
        pbis->ReadBytes(cel.aedge.data(), cel.aedge.size());
        g_globstats.cedge += cn;
        pglob->asubcel.push_back(std::move(cel));
    }
    LoadGlobSubmeshesFromBrx(pglobset, iglob, csub, fDraw, pbis);
}

// orig3: 0x192BB0 (Sly 2 0x16CAA0: type 2: per strip a shader, positions, weights and 4-byte records; the globset
// bit 33 is set by LoadGlobsetFromBrx now)
void LoadGlobMesh2FromBrx(GLOBSET* pglobset, int iglob, int csub, bool fDraw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x192BB0, pbis);
    GLOB* pglob = &pglobset->aglob[size_t(iglob)];
    const int cstrip = pbis->S16Read();
    for (int i = 0; i < cstrip; ++i)
    {
        SUBFX fx;
        fx.pshd = PshdFromIndex(pbis->U16Read());
        const int cv = pbis->U8Read();
        fx.apos.resize(size_t(cv));
        pbis->ReadBytes(fx.apos.data(), size_t(cv) * 12);
        if (pglob->cbone != 0)
        {
            fx.cw = pbis->U8Read();
            for (int j = 0; j < fx.cw; ++j)
            {
                const uint8_t b = pbis->U8Read();
                if (j < 4) fx.aiboneSlot[j] = b;
            }
            fx.aw.resize(size_t(cv) * size_t(fx.cw));
            pbis->ReadBytes(fx.aw.data(), fx.aw.size());
        }
        const int cn = pbis->U8Read();
        fx.arec.resize(size_t(cn) * 4);
        pbis->ReadBytes(fx.arec.data(), fx.arec.size());
        ++g_globstats.cfx;
        pglob->asubfx.push_back(std::move(fx));
    }
    LoadGlobSubmeshesFromBrx(pglobset, iglob, csub, fDraw, pbis);
}

// orig3: 0x192FD8 (Sly 2 0x16CF78: type 3: per strip a shader, a position dictionary, weights by position,
// per-vertex position indices and the strip mask)
void LoadGlobMesh3FromBrx(GLOBSET* pglobset, int iglob, int csub, bool fDraw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x192FD8, pbis);
    GLOB* pglob = &pglobset->aglob[size_t(iglob)];
    const int cstrip = pbis->S16Read();
    for (int i = 0; i < cstrip; ++i)
    {
        SUBFX fx;
        fx.pshd = PshdFromIndex(pbis->U16Read());
        const int cv = pbis->U8Read();
        const int cpos = pbis->U8Read();
        fx.apos.resize(size_t(cpos));
        for (glm::vec3& pos : fx.apos) pos = pbis->ReadVector();
        if (pglob->cbone != 0)
        {
            for (int j = 0; j < 4; ++j) (void)pbis->U8Read();
            fx.cw = std::min<int>(pglob->cbone, 4);
            fx.aw.resize(size_t(fx.cw) * size_t(cpos));
            pbis->ReadBytes(fx.aw.data(), fx.aw.size());
        }
        fx.aipos.resize(size_t(cv));
        pbis->ReadBytes(fx.aipos.data(), size_t(cv));
        fx.amask.resize(size_t((cv + 31) / 32));
        pbis->ReadBytes(fx.amask.data(), fx.amask.size() * 4);
        ++g_globstats.cfx;
        pglob->asubfx.push_back(std::move(fx));
    }
    LoadGlobSubmeshesFromBrx(pglobset, iglob, csub, fDraw, pbis);
}

// orig3: 0x18D738 (Sly 2 0x167870: the sphere sets: up to 4 spheres each, kind -1 ends a set; a kind-0 sphere's S16
// is the glob's index in the stream, mapped to the loaded index (filtered globs: kind 5, not drawn))
void LoadGlobSpheresFromBrx(GLOBSET* pglobset, const std::vector<int>& mpiglob, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x18D738, pbis);
    const int c = pbis->S16Read();
    pglobset->asphereset.resize(size_t(std::max(0, c)));
    for (auto& set : pglobset->asphereset)
    {
        for (GLOBSPHERE& sphere : set)
        {
            const int8_t v = pbis->S8Read();
            // 4-bit kind: 0xF (-1) ends the set
            sphere.kind = static_cast<int8_t>((int8_t(v << 4)) >> 4);
            if (sphere.kind == -1) break;
            sphere.pos = pbis->ReadVector();
            sphere.sRadius = pbis->F32Read();
            sphere.sMRD = std::min(pbis->F32Read(), FloatFromBits(MRD_FAR_BITS));
            sphere.mask = pbis->U8Read();
            if (sphere.kind == 0)
            {
                const int istream = pbis->S16Read();
                const int iglob = istream >= 0 && istream < int(mpiglob.size()) ? mpiglob[size_t(istream)] : -1;
                if (iglob < 0) sphere.kind = 5;
                sphere.iglob = int16_t(iglob);
            }
            else if (sphere.kind == 1) sphere.iset = pbis->S16Read();
        }
    }
}

// orig3: 0x18FD10 (new in Sly 3: the mesh dispatcher; F32 uFog and S8 rtck (Sly 2 read them in LoadGlob's header),
// then the mesh loader of the glob's type)
void LoadGlobMeshFromBrx(GLOBSET* pglobset, int iglob, int csub, bool fDraw, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x18FD10, pbis);
    GLOB* pglob = &pglobset->aglob[size_t(iglob)];
    pglob->uFog = pbis->F32Read();
    pglob->rtck = pbis->S8Read();
    switch (pglob->meshk)
    {
    case MESHK_Strips: LoadGlobMesh0FromBrx(pglobset, iglob, csub, fDraw, pbis); break;
    case MESHK_CelBorder: LoadGlobMesh1FromBrx(pglobset, iglob, csub, fDraw, pbis); break;
    case MESHK_Effect: LoadGlobMesh2FromBrx(pglobset, iglob, csub, fDraw, pbis); break;
    case MESHK_Indexed: LoadGlobMesh3FromBrx(pglobset, iglob, csub, fDraw, pbis); break;
    default: throw std::runtime_error("LoadGlob: unknown mesh type " + std::to_string(int(pglob->meshk)));
    }
}
}  // namespace

// orig3: 0x18D148 LoadGlobsetFromBrx (Sly 2 0x167320). Sly 3's stream: S8 (non-zero: sphere-culled, no draw
// records), its bounds, S8 animations (Sly 2: U8), U8 ids, S16 globs; per glob U8 filters and U8 conditions (Sly 2:
// S8 filters), their lists and an S32 size when either is non-zero (the glob is skipped by it unless both match), S8
// type, U16 instances, S16 owner, S8 rp, F32 z order (g |g|; sets group flag 0x800), S32, S8 hidden (bit 0 -> 0x1000),
// the draw record, the bones (moved here from the mesh loaders), an S8 "no mesh" (flag 0x1000, nothing more is read),
// else the dispatcher 0x18FD10; then the sphere sets.
void LoadGlobsetFromBrx(GLOBSET* pglobset, ALO* palo, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x18D148, pbis);
    pglobset->palo = palo;
    pglobset->fNoDraw = pbis->S8Read() != 0;
    const bool fDraw = !pglobset->fNoDraw;
    pglobset->posCenter = pbis->ReadVector();
    pglobset->sRadius = pbis->F32Read();
    {
        const int c = pbis->S8Read();
        for (int i = 0; i < c; ++i) pglobset->apsaa.push_back(PsaaLoadFromBrx(true, pbis));
    }
    {
        const int c = pbis->U8Read();
        for (int i = 0; i < c; ++i) pglobset->aids.push_back(pbis->S32Read());
    }
    const int cglobStream = pbis->S16Read();
    std::vector<int> mpiglob(size_t(std::max(0, cglobStream)), -1);   // stream index -> loaded index (0x18D148 +0x328)
    pglobset->aglob.reserve(size_t(std::max(0, cglobStream)));
    for (int j = 0; j < cglobStream; ++j)
    {
        const int cfilter = pbis->U8Read();
        const int ccond = pbis->U8Read();
        if (cfilter != 0 || ccond != 0)
        {
            int16_t afilter[256];
            int32_t acond[256];
            pbis->ReadBytes(afilter, size_t(cfilter) * 2);
            pbis->ReadBytes(acond, size_t(ccond) * 4);
            const int32_t cbSkip = pbis->S32Read();
            if (!FFilterMatches3(cfilter, afilter) || !FConditionsMatch3(ccond, acond))
            {
                SkipBytes(pbis, size_t(cbSkip));
                ++g_globstats.cfiltered;
                continue;
            }
        }
        const int iglob = static_cast<int>(pglobset->aglob.size());
        pglobset->aglob.emplace_back();
        GLOB* pglob = &pglobset->aglob.back();
        pglob->meshk = static_cast<MESHK>(pbis->S8Read());
        const int csub = pbis->U16Read();
        pglob->oidOwner = pbis->S16Read();
        pglob->rp3 = pbis->S8Read();
        pglob->rp = static_cast<int8_t>(Rp2FromRp3(pglob->rp3));
        uint32_t grf3 = 0;   // Sly 3's group word (+0xC)
        {
            const float g = pbis->F32Read();
            pglob->gZOrder = g;
            if (g != G_NONE)
            {
                pglob->gZOrder = g * std::fabs(g);
                grf3 |= 0x800;
            }
        }
        pglob->n0 = pbis->S32Read();
        pglob->fHidden = (pbis->S8Read() & 1) != 0;
        if (pglob->fHidden) grf3 |= 0x1000;
        if (fDraw)
        {
            pglob->fDraw = true;
            pglob->posCenter = pbis->ReadVector();
            pglob->sRadius = pbis->F32Read();
            pglob->u14 = pbis->U32Read();
            pglob->sMRD = pbis->F32Read();
            // (Sly 3 keeps the read value; Sly 2's loader replaced "none" by 2000 for cel borders, 1e10 else)
            if (pglob->sMRD == G_NONE)
                pglob->sMRD = pglob->meshk == MESHK_CelBorder ? 2000.0f : FloatFromBits(MRD_FAR_BITS);
        }
        ReadBones(pglob, pbis);
        if (pglob->meshk == MESHK_Effect) pglobset->grfglobset |= 1ull << 33;
        // the native group word keeps Sly 2's bits (glob.h)
        pglob->grfGroup = (uint32_t(pglob->meshk) & 7) | ((uint32_t(pglob->rp) & 0x1F) << 3) |
                          ((grf3 & 0x1000) ? 0x800u : 0u) | ((grf3 & 0x800) ? 0x400u : 0u);
        ++g_globstats.cglob;
        if (pglob->meshk >= 0 && pglob->meshk < 4) ++g_globstats.cmeshk[pglob->meshk];
        if (pbis->S8Read() != 0)
        {
            pglob->fNoMesh = true;
            pglob->fHidden = true;
            pglob->grfGroup |= 0x800;
            ++g_globstats.cnomesh;
            if (csub != 0) throw std::runtime_error("LoadGlob: a glob without a mesh has instances");
        }
        else
            LoadGlobMeshFromBrx(pglobset, iglob, csub, fDraw, pbis);
        for (int k = 0; k <= csub && j + k < cglobStream; ++k) mpiglob[size_t(j + k)] = iglob + k;
        j += csub;
    }
    LoadGlobSpheresFromBrx(pglobset, mpiglob, pbis);
    ++g_globstats.cglobset;
}
