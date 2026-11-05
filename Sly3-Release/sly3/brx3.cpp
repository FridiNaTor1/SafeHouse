// Sly 3's world file walk (--dump-world): the reads of orig3 0x14F440 LoadSwFromBrx and of every block reader it
// calls, ported from the Sly 3 asm . See brx3.h.

#include "brx3.h"

#include "addr.h"
#include "ids3.h"
#include "load3.h"

#include "../bis.h"
#include "../disc.h"
#include "../sw.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace
{
// The block being read, for the error message.
const char* s_szBlock = "";
uint32_t s_fnBlock = 0;
struct BLOCK
{
    const char* szPrev;
    uint32_t fnPrev;
    BLOCK(const char* sz, uint32_t fn) : szPrev(s_szBlock), fnPrev(s_fnBlock) { s_szBlock = sz; s_fnBlock = fn; }
    ~BLOCK() { s_szBlock = szPrev; s_fnBlock = fnPrev; }
};

// The S16 at offset ib of the stream (the object count, for the report)
uint16_t ab0At(CBinaryInputStream* pbis, std::size_t ib)
{
    uint16_t u = 0;
    pbis->CbCopyAt(ib, &u, 2);
    return u;
}

bool s_fScriptDebug = false;  // orig3: 0x47A488, the group's S32 after the bulk header
int32_t s_cbBulk = 0;         // orig3: 0x47A484
WORLDWALK3* s_pw = nullptr;

void Check(bool f, const char* sz)
{
    if (!f) throw std::runtime_error(sz);
}

// orig3: 0x119968 (a U16 count, then that many U16s)
void SkipU16Array(CBinaryInputStream* pbis)
{
    const int c = pbis->U16Read();
    pbis->Skip(std::size_t(c) * 2);
}

// orig3: 0x119868 (read) / 0x119910 (skip): a wide string, a U16 count of 16-bit characters (Sly 2: 8-bit)
void SkipWideString(CBinaryInputStream* pbis)
{
    const int c = pbis->U16Read();
    pbis->Skip(std::size_t(c) * 2);
}

// orig3: 0x119790 CBinaryInputStream_ReadSz (a U16 count of bytes; still 8-bit) and 0x119830 ReadDebugString
void SkipSz(CBinaryInputStream* pbis)
{
    const int c = pbis->U16Read();
    pbis->Skip(std::size_t(c));
}

// ---------------------------------------------------------------- options: load3.cpp (orig3: 0x1519C8, 0x150AE8, 0x150960)

const LOAD3OPTS* s_pload = nullptr;  // the load mode (null: the walk)

int LoadOptionsFromBrx3(CBinaryInputStream* pbis)
{
    std::vector<OPT> aopt;
    return ::LoadOptionsFromBrx3(aopt, nullptr, pbis);
}

// ---------------------------------------------------------------- the global blocks

// orig3: 0x14FA38: two tables of U8 n x {S32 key, S8 value, U8 c, S32 list[c]}. The original picks the value whose key
// is the global at 0x36C700+0x1498 (then +0x14A0) and whose list holds SW+0x441C, and hands it to 0x196228.
void LoadPrefix3(CBinaryInputStream* pbis, std::vector<PREFIX3>& a)
{
    const int n = pbis->U8Read();
    for (int i = 0; i < n; ++i)
    {
        PREFIX3 p;
        p.key = pbis->S32Read();
        p.value = pbis->S8Read();
        const int c = pbis->U8Read();
        for (int j = 0; j < c; ++j) p.alist.push_back(pbis->S32Read());
        a.push_back(std::move(p));
    }
}

// orig3: 0x21BFF8 LoadProgressFromBrx (Sly 2's 0x1D7B88, the same reads): U16 c x {U16 value, Sz name}, U16 records
// x 10 bytes; then ValidateGameSave 0x21C0F0
void LoadProgress3(CBinaryInputStream* pbis, WORLDWALK3& w)
{
    BLOCK b("progress", orig3::FN_LOAD_PROGRESS);
    const int c = pbis->U16Read();
    for (int i = 0; i < c; ++i)
    {
        pbis->U16Read();
        SkipSz(pbis);
    }
    const int cr = pbis->U16Read();
    pbis->Skip(std::size_t(cr) * 10);
    w.cprogressName = c;
    w.cprogressRecord = cr;
}

// orig3: 0x246F58 LoadMusicFromBrx (the same reads as Sly 2's 0x1FD0E8)
void LoadMusic3(CBinaryInputStream* pbis, WORLDWALK3& w)
{
    BLOCK b("music", orig3::FN_LOAD_MUSIC);
    pbis->Skip(0x40);                       // the sound bank's key (0x119948)
    const int n = pbis->S16Read();          // sound flags
    pbis->Skip(std::size_t(std::max(n, 0)));
    pbis->S16Read();                        // the id table's size
    for (int i = 0; i < n; ++i) pbis->S16Read();
    const int nt = pbis->S16Read();         // the resident stream and its targets
    if (nt > 0) pbis->Skip(0x40);
    for (int i = 0; i < nt; ++i) pbis->S16Read();
    const int nl = pbis->U8Read();          // languages
    pbis->Skip(std::size_t(nl));
    const int ne = pbis->S32Read();         // dialog streams
    for (int i = 0; i < ne; ++i)
    {
        pbis->S16Read();
        pbis->S16Read();
        for (int j = 0; j < nl; ++j)
        {
            pbis->Skip(0x40);
            pbis->S8Read();
            pbis->F32Read();
        }
        const int c = pbis->S32Read();
        pbis->Skip(std::size_t(std::max(c, 0)) * 8);
    }
    const int no = pbis->S32Read();         // music streams
    for (int i = 0; i < no; ++i)
    {
        pbis->S16Read();
        pbis->Skip(0x40);
    }
    w.csound = n;
    w.cdialog = ne;
    w.cmusic = no;
    w.clang = nl;
}

// orig3: 0x14FD20 LoadTextFromBrx: U8 languages, S16 strings x {S16 id, a wide string per language}, then one more
// wide string per language (SW +0x4E5C)
void LoadText3(CBinaryInputStream* pbis, WORLDWALK3& w)
{
    BLOCK b("text", orig3::FN_LOAD_TEXT);
    const int clang = pbis->U8Read();
    pbis->Skip(std::size_t(clang));
    const int c = pbis->S16Read();
    for (int i = 0; i < c; ++i)
    {
        pbis->S16Read();
        for (int j = 0; j < clang; ++j) SkipWideString(pbis);
    }
    for (int j = 0; j < clang; ++j) SkipWideString(pbis);
    w.ctext = c;
}

// orig3: 0x1500F0 (new): U8 x {U32 key, U32 value}; U8 x {U16 array (0x119968), U32 key, U32 value}; U8 n option
// blocks: n U32 ids, then n x {S32 cb, options}. The block chosen: the value of table A's entry whose key is the
// arrival key A (GAME +0x1498), else of table B's whose key is the arrival key B (+0x14A0), else of the last table B
// entry whose progress array has NProgress (0x2BC010) > 0; 12 means none. The block whose id is that value (the
// first block when none matches) is loaded into the camera (0x36BC10, LoadOptionsFromBrx), and its id is stored at
// the camera's +0x10 (the condition set: 0x169150); the others are skipped by their size. Then 0x1694D8 adds the value
// to the camera's extra ids when the camera does not match it; 0x36BD10 = (the first block was chosen).
void LoadOptionSets3(CBinaryInputStream* pbis, WORLDWALK3& w)
{
    BLOCK b("option sets", orig3::FN_LOAD_OPTION_SETS);
    const int n1 = pbis->U8Read();
    for (int i = 0; i < n1; ++i)
    {
        OPTSETKEY3 k;
        k.key = pbis->U32Read();
        k.value = pbis->U32Read();
        w.aoptsetA.push_back(k);
    }
    const int n2 = pbis->U8Read();
    for (int i = 0; i < n2; ++i)
    {
        OPTSETKEY3 k;
        const int c = pbis->U16Read();
        k.aprogress.resize(std::size_t(c));
        pbis->Read(std::size_t(c) * 2, k.aprogress.data());
        k.key = pbis->U32Read();
        k.value = pbis->U32Read();
        w.aoptsetB.push_back(k);
    }
    int32_t value = -1;
    if (s_pload)
    {
        if (s_pload->keyA != -1)
            for (const OPTSETKEY3& k : w.aoptsetA)
                if (int32_t(k.key) == s_pload->keyA) { value = int32_t(k.value); break; }
        if (value == -1 && s_pload->keyB != -1)
            for (const OPTSETKEY3& k : w.aoptsetB)
                if (int32_t(k.key) == s_pload->keyB) { value = int32_t(k.value); break; }
        // (else NProgress of each table B entry's names: a new game's progress is 0: TODO(sly3) with the save)
        if (value == 12) value = -1;
    }
    const int n3 = pbis->U8Read();
    w.aidOptset.resize(std::size_t(n3));
    for (uint32_t& id : w.aidOptset) id = pbis->U32Read();
    int iblock = 0;
    for (int i = 0; i < n3; ++i)
        if (int32_t(w.aidOptset[std::size_t(i)]) == value) { iblock = i; break; }
    if (s_pload)
    {
        g_condset3.cond = n3 > 0 ? int32_t(w.aidOptset[std::size_t(iblock)]) : value;  // CM +0x10
        g_condset3.acond.clear();
        g_cmblock3 = CMBLOCK3{};
        g_cmblock3.iblock = n3 > 0 ? iblock : -1;
        w.iblockChosen = g_cmblock3.iblock;
        w.idChosen = g_condset3.cond;
    }
    for (int i = 0; i < n3; ++i)
    {
        const int32_t cb = pbis->S32Read();
        const std::size_t ib = pbis->m_ib;
        std::vector<OPT> aopt;
        // (the original skips the blocks not chosen: their load methods must not run)
        SW* pswSave = g_psw3;
        const bool fChosen = s_pload && i == iblock;
        if (!fChosen) g_psw3 = nullptr;
        LO loCm;  // the camera's fields the chosen block's field options set (grf 0x90: CM +c18)
        w.acoptOptset.push_back(::LoadOptionsFromBrx3(aopt, fChosen ? &loCm : nullptr, pbis));
        w.aagrpcount.emplace_back();
        for (const OPT& o : aopt)
            if (o.eopid == 1032 && o.aval.size() >= 2) w.aagrpcount.back().emplace_back(o.aval[0].n, o.aval[1].n);
        g_psw3 = pswSave;
        Check(pbis->m_ib - ib == std::size_t(cb), "an option block's size does not match its options");
        if (fChosen)
        {
            g_cmblock3.aoptCm = std::move(aopt);
            // the fog / colour fields condset3.h names (CM +0x14.. +0x58)
            auto f = [&](uint32_t ib, float& g) {
                for (const auto& [k, u] : loCm.afield)
                    if (k == ib) std::memcpy(&g, &u, 4);
            };
            f(0x14, g_condset3.fogNear);
            f(0x18, g_condset3.fogFar);
            f(0x1C, g_condset3.fogMax);
            for (const auto& [k, u] : loCm.afield)
                if (k == 0x20) g_condset3.rgbaFog = u;
            f(0x38, g_condset3.gLightness);
            f(0x3C, g_condset3.gContrast);
            f(0x40, g_condset3.gSaturation);
            for (int c = 0; c < 3; ++c) f(0x50 + 4u * uint32_t(c), g_condset3.rgbTint[c]);
            // the world shader colour's setters (0x169398's inputs, light3.h)
            for (const OPT& o : g_cmblock3.aoptCm)
            {
                if (o.aval.empty()) continue;
                const uint32_t pfn = PfnEopid3(o.eopid);
                if (pfn == 0x169460) for (int c = 0; c < 3; ++c) g_condset3.hsvWorld[c] = o.aval[0].ag[size_t(c)];
                else if (pfn == 0x1694A0) g_condset3.gWorldScale = o.aval[0].ag[0];
                else if (pfn == 0x169480) g_condset3.gWorldAmbient = o.aval[0].ag[0];
                else if (std::getenv("SLY3_CMOPTPRINT")) std::printf("camera option: EOPID %d pfn 0x%06X\n", o.eopid, pfn);
            }
            f(0x8C, g_condset3.gDesaturate);
        }
    }
    if (s_pload && value != -1 && !FConditionMatches3(value)) g_condset3.acond.push_back(value);  // 0x1694D8
    w.coptsetA = n1;
    w.coptsetB = n2;
    w.coptsetBlock = n3;
}

// orig3: 0x14FF58 LoadNodesFromBrx: U8 sets x {S32 condition, U16 c, c x {VECTOR4, S16, S16}} (a set whose condition
// fails, 0x169150, is skipped: the same 20 bytes per node)
void LoadNodes3(CBinaryInputStream* pbis, WORLDWALK3& w)
{
    BLOCK b("nodes", orig3::FN_LOAD_NODES);
    const int cset = pbis->U8Read();
    for (int i = 0; i < cset; ++i)
    {
        pbis->S32Read();
        const int c = pbis->U16Read();
        pbis->Skip(std::size_t(c) * 0x14);
        w.cnode += c;
    }
    w.cnodeSet = cset;
}

// ---------------------------------------------------------------- the group blocks

// orig3: 0x12C108 PpairSerializeIn (the tag dispatch at 0x31F610; payloads as Sly 2's, tags renumbered: TAGK3)
void PpairSerializeIn3(CBinaryInputStream* pbis)
{
    for (;;)
    {
        const int b = pbis->U8Read();
        if (b == 0xFF) return;
        if (s_fScriptDebug)
        {
            pbis->S32Read();
            for (int i = 0; i < 6; ++i) pbis->S16Read();
        }
        const int tag = b & 0x7F;
        ++s_pw->cpairSplice;
        ++s_pw->mptagc[tag];
        switch (static_cast<TAGK3>(tag))
        {
        case TAGK3::S32: case TAGK3::F32: case TAGK3::Bool: case TAGK3::Symid: case TAGK3::Bifk: case TAGK3::Basic:
            pbis->U32Read();
            break;
        case TAGK3::Vector: case TAGK3::Clq: case TAGK3::Smp: pbis->Skip(12); break;
        case TAGK3::Matrix: pbis->Skip(64); break;
        case TAGK3::Lm: pbis->Skip(8); break;
        case TAGK3::Pair: PpairSerializeIn3(pbis); break;
        default: break;  // the tag alone (3, 17, 30 and >= 32 are invalid: nothing read)
        }
        if (!(b & 0x80)) return;
    }
}

// orig3: 0x2499F0 LoadSwSpliceFromBrx: U32 modules x {PpairSerializeIn, with debug info 3 strings (0x119830)}; then
// each module is evaluated (PframeNew + RefEval: not here)
uint32_t LoadSwSplice3(CBinaryInputStream* pbis)
{
    BLOCK b("splice", orig3::FN_LOAD_SW_SPLICE);
    const uint32_t c = pbis->U32Read();
    for (uint32_t i = 0; i < c; ++i)
    {
        PpairSerializeIn3(pbis);
        if (s_fScriptDebug)
            for (int j = 0; j < 3; ++j) SkipSz(pbis);
    }
    return c;
}

// orig3: 0x233EA8 (a CLUT: 16 bytes, n1 S16 filters, n2 S32 conditions, then its colours unless it lives in the
// bulk block (texk +4 == 1)); 0x234648 (a BMP: 0x18 bytes, the lists, its bytes (+0x10) unless in the bulk block)
void LoadClut3(CBinaryInputStream* pbis)
{
    uint8_t ab[16];
    pbis->Read(sizeof(ab), ab);
    const int n1 = int8_t(ab[6]), n2 = int8_t(ab[7]);
    pbis->Skip(std::size_t(std::max(n1, 0)) * 2);
    pbis->Skip(std::size_t(std::max(n2, 0)) * 4);
    uint64_t u;
    std::memcpy(&u, ab + 8, 8);
    const std::size_t cb = std::size_t((u >> 7) & 0x1FF) * 4;
    if (ab[4] != 1) pbis->Skip(cb);
}

void LoadBmp3(CBinaryInputStream* pbis)
{
    uint8_t ab[0x18];
    pbis->Read(sizeof(ab), ab);
    const int n1 = int8_t(ab[6]), n2 = int8_t(ab[7]);
    pbis->Skip(std::size_t(std::max(n1, 0)) * 2);
    pbis->Skip(std::size_t(std::max(n2, 0)) * 4);
    uint32_t cb;
    std::memcpy(&cb, ab + 0x10, 4);
    if (ab[4] != 1) pbis->Skip(cb);
}

// orig3: 0x234518 LoadClutsFromBrx / 0x2347C8 LoadBmpsFromBrx (pass: which of the two copies the original reads,
// 0x234500: (GAME +0xC & 3) == 0; the other is skipped by its S32 size). Both are parsed here and checked.
int LoadTextureList3(CBinaryInputStream* pbis, bool fBmp)
{
    BLOCK b(fBmp ? "BMPs" : "CLUTs", fBmp ? orig3::FN_LOAD_BMPS : orig3::FN_LOAD_CLUTS);
    const int32_t cb = pbis->S32Read();
    const std::size_t ib = pbis->m_ib;
    const int c = pbis->U16Read();
    for (int i = 0; i < c; ++i) fBmp ? LoadBmp3(pbis) : LoadClut3(pbis);
    Check(pbis->m_ib - ib == std::size_t(cb), fBmp ? "the BMP list's size does not match its BMPs" : "the CLUT list's size does not match its CLUTs");
    return c;
}

// orig3: 0x234EA0 LoadShadersFromBrx: CLUTs and BMPs of pass 1, then of pass 0; U8 SAAs (0x236EB0: U16 class, 0xFFFF
// none, then 0x24 bytes; Sly 2: a U16 count, 0x20 bytes); U16 shaders (0x234A58: 0x18 bytes, S8 SAA index (Sly 2:
// S16), byte +0x17 textures x 0x234900 {8 bytes, +6 U16 BMP indices, +7 U16 CLUT indices}); the bulk block 0x25C9A0
void LoadShaders3(CBinaryInputStream* pbis, GROUP3& g)
{
    BLOCK b("shaders", orig3::FN_LOAD_SHADERS);
    g.cclutA = LoadTextureList3(pbis, false);
    g.cbmpA = LoadTextureList3(pbis, true);
    g.cclutB = LoadTextureList3(pbis, false);
    g.cbmpB = LoadTextureList3(pbis, true);
    g.csaa = pbis->U8Read();
    for (int i = 0; i < g.csaa; ++i)
    {
        BLOCK bs("SAA", orig3::FN_PSAA_LOAD);
        if (pbis->U16Read() != 0xFFFF) pbis->Skip(0x24);
    }
    g.cshd = pbis->U16Read();
    for (int i = 0; i < g.cshd; ++i)
    {
        BLOCK bs("shader", orig3::FN_LOAD_SHD);
        uint8_t ab[0x18];
        pbis->Read(sizeof(ab), ab);
        pbis->S8Read();
        for (int t = 0; t < ab[0x17]; ++t)
        {
            uint8_t abTex[8];
            pbis->Read(sizeof(abTex), abTex);
            pbis->Skip(std::size_t(abTex[6]) * 2);
            pbis->Skip(std::size_t(abTex[7]) * 2);
        }
    }
    {
        BLOCK bb("bulk", orig3::FN_LOAD_BULK);
        pbis->Align(16);
        pbis->Skip(std::size_t(s_cbBulk));
    }
}

// orig3: 0x1ED7B0 / 0x1ED820 LoadCmeshFromBrx (as Sly 2's): U32 c; c: U32 cvec, U32 cn, c x 3 U16, cvec x VECTOR4,
// cn x S32
uint32_t LoadCmesh3(CBinaryInputStream* pbis)
{
    BLOCK b("collision mesh", orig3::FN_LOAD_CMESH);
    const uint32_t c = pbis->U32Read();
    if (!c) return 0;
    const uint32_t cvec = pbis->U32Read();
    const uint32_t cn = pbis->U32Read();
    pbis->Skip(std::size_t(c) * 6);
    pbis->Skip(std::size_t(cvec) * 16);
    pbis->Skip(std::size_t(cn) * 4);
    return c;
}

// orig3: 0x151D80 LoadSwObjsFromBrx: S16 c x {S8 mode, (mode 1: U8 n1, U8 n2, S16 filters[n1], S32 conditions[n2]),
// S16 cid, S32 oid, S16 n, S32 cb, the body}. The bodies are skipped by cb here (the original does that for objects
// its filters / conditions reject; proxies, FIsCidDerivedFrom(cid, 8), go to LoadProxyFromBrx 0x1FA778).
int LoadSwObjs3(CBinaryInputStream* pbis, int igrp, WORLDWALK3& w)
{
    BLOCK b("objects", orig3::FN_LOAD_SW_OBJS);
    if (s_pload)
    {
        // orig3: 0x14F440: the group's objects with no parent into the group's list, then its copies (PloCloneLo
        // 0x1507E0, instances - 1 times); a shared group (+0x5C) counts one used instance, the others' objects are
        // removed (vt slot 9) until proxies place them
        SW* psw = s_pload->psw;
        WE& we = psw->awe[std::size_t(igrp)];
        const std::size_t cobj0 = psw->aploAll.size();
        std::vector<LO*> aplo;
        LOSINK3 sink;
        sink.papo = &aplo;
        const std::size_t ib0 = pbis->m_ib;
        const int c = int(int16_t(uint16_t(ab0At(pbis, ib0))));
        LoadSwObjsFromBrx3(psw, nullptr, pbis, &sink);
        we.cloLoaded = int(aplo.size());
        we.aplo = aplo;
        for (int copy = 1; copy < we.cinst; ++copy)
            for (LO* plo : aplo) we.aplo.push_back(PloCloneLo3(psw, plo, nullptr));
        if (we.x5C) ++we.cUsed;
        else
            for (LO* plo : we.aplo) plo->fRemoved = true;
        w.mpcidc[-1] += int(psw->aploAll.size() - cobj0);
        return std::max(c, 0);
    }
    const int c = pbis->S16Read();
    for (int i = 0; i < c; ++i)
    {
        OBJHDR3 o;
        o.ib = pbis->m_ib;
        o.igrp = igrp;
        o.mode = pbis->S8Read();
        if (o.mode == 1)
        {
            const int n1 = pbis->U8Read(), n2 = pbis->U8Read();
            o.afilter.resize(std::size_t(n1));
            o.acond.resize(std::size_t(n2));
            for (int16_t& s : o.afilter) s = pbis->S16Read();
            for (int32_t& n : o.acond) n = pbis->S32Read();
        }
        o.cid = pbis->S16Read();
        o.oid = pbis->S32Read();
        o.n = pbis->S16Read();
        o.cb = pbis->S32Read();
        o.ibBody = pbis->m_ib;
        Check(o.cid >= 0 && o.cid < orig3::CCLS, "an object's class is outside the class table");
        Check(o.cb >= 0, "an object's size is negative");
        pbis->Skip(std::size_t(o.cb));
        ++w.mpcidc[o.cid];
        ++w.mpmodec[o.mode];
        w.aobj.push_back(std::move(o));
    }
    return std::max(c, 0);
}
}  // namespace

int LoadOptionsFromBrx3Public(CBinaryInputStream* pbis) { return LoadOptionsFromBrx3(pbis); }

void (*g_pfnSpliceBlock3)(const uint8_t* pb, std::size_t ib, std::size_t ibEnd, int igrp, const GROUP3& g) = nullptr;

bool FWalkWorld3(const std::vector<uint8_t>& ab, WORLDWALK3& w, std::string& error, const LOAD3OPTS* pload)
{
    w = WORLDWALK3{};
    s_pload = pload;
    if (pload)
    {
        g_psw3 = pload->psw;
        g_nFilterCur3 = pload->iworld;
        g_load3stats = LOAD3STATS{};
        g_pe3stats = PE3STATS{};
    }
    w.cbFile = ab.size();
    s_pw = &w;
    CBinaryInputStream bis;
    bis.OpenMemory(int(ab.size()), ab.data());
    CBinaryInputStream* pbis = &bis;
    try
    {
        // orig3: 0x14F440 LoadSwFromBrx
        {
            BLOCK b("prefix", orig3::FN_LOAD_PREFIX);
            LoadPrefix3(pbis, w.aprefixA);
            LoadPrefix3(pbis, w.aprefixB);
        }
        {
            BLOCK b("groups", orig3::FN_LOAD_SW);
            const int cgroup = pbis->S16Read();
            Check(cgroup >= 0, "negative group count");
            w.agroup.resize(std::size_t(cgroup));
            for (GROUP3& g : w.agroup)
            {
                pbis->Read(64, g.achName);
                g.x5C = pbis->S32Read();
                g.x60 = pbis->S32Read();
                g.x10 = pbis->S32Read();
                g.x14 = pbis->S32Read();
                g.cinst = pbis->S32Read();
                g.cb = pbis->U32Read();
                if (pload)
                {
                    WE we;
                    std::memcpy(we.achName, g.achName, sizeof(we.achName));
                    we.x5C = g.x5C;
                    we.x60 = g.x60;
                    we.x10 = g.x10;
                    we.x14 = g.x14;
                    we.cinst = g.cinst;
                    pload->psw->awe.push_back(std::move(we));
                }
            }
        }
        LoadProgress3(pbis, w);
        // orig3: 0x25B1B8 (no reads: ~29 progress names resolved into SW +0x406C..+0x41A4), 0x1CF640 (no reads: the
        // save slot and the game settings): TODO(sly3) with the game state
        w.ibMusic = pbis->m_ib;
        LoadMusic3(pbis, w);
        w.ibMusicEnd = w.ibText = pbis->m_ib;
        LoadText3(pbis, w);
        {
            BLOCK b("options", orig3::FN_LOAD_OPTIONS);
            w.coptSw = LoadOptionsFromBrx3(pbis);  // the world's (Sly 2 had a second block for the camera)
        }
        LoadOptionSets3(pbis, w);
        // orig3: 0x25EB00 (no reads)
        LoadNodes3(pbis, w);
        pbis->Align(16);
        // orig3: 0x249980 StartupSwSplice, 0x25B6B8, 0x169398 (no reads), then the load heaps and the renderer's tables
        for (int i = 0; i < int(w.agroup.size()); ++i)
        {
            GROUP3& g = w.agroup[std::size_t(i)];
            g.ibStart = pbis->m_ib;
            if (pload) g.cinst = pload->psw->awe[std::size_t(i)].cinst;  // (the option sets may have changed it)
            if (g.cinst == 0)
            {
                // a group without instances: its data is skipped by the size in its record
                BLOCK b("skipped group", orig3::FN_LOAD_SW);
                pbis->Skip(g.cb);
                g.fSkipped = true;
                g.ibEnd = pbis->m_ib;
                continue;
            }
            // orig3: 0x162A88 (no reads), 0x1A2C88; SW +0x4638 = the group (-1 for a shared one)
            if (pload) pload->psw->iwe = pload->psw->iweChannel = g.x5C ? -1 : i;
            {
                BLOCK b("bulk header", orig3::FN_LOAD_BULK_HEADER);
                g.cbBulk = s_cbBulk = pbis->S32Read();
                g.keyBulk = pbis->S32Read();
            }
            {
                BLOCK b("script debug flag", orig3::FN_LOAD_SW);
                g.fScriptDebug = pbis->S32Read();
                s_fScriptDebug = g.fScriptDebug != 0;
            }
            {
                const std::size_t ibSplice = pbis->m_ib;
                g.cmodule = LoadSwSplice3(pbis);
                if (g_pfnSpliceBlock3)  // (the runtime's loader over the same bytes: sly3/splice3.cpp)
                    g_pfnSpliceBlock3(ab.data(), ibSplice, pbis->m_ib, i, g);
            }
            g.ibShaders = pbis->m_ib;
            LoadShaders3(pbis, g);
            g.ibCmesh = pbis->m_ib;
            g.ccmesh = LoadCmesh3(pbis);
            g.cobj = LoadSwObjs3(pbis, i, w);
            pbis->Align(16);
            // orig3: 0x162A88, EndShaderGroup 0x235DE0, the clones (PloCloneLo 0x1507E0), each object's vt slot
            g.ibEnd = pbis->m_ib;
        }
        // orig3: 0x236B80, LoadSoundBank 0x247470 (no reads)
        w.ibEnd = pbis->m_ib;
        if (pload)
        {
            pload->psw->iwe = -2;
            PostLoadSwObjects3(pload->psw);
        }
    }
    catch (const std::exception& e)
    {
        w.ibEnd = pbis->m_ib;
        char sz[96];
        std::snprintf(sz, sizeof(sz), " (in %s, orig3 0x%06X, at offset %zu)", s_szBlock, s_fnBlock, pbis->m_ib);
        error = e.what() + std::string(sz);
        s_szBlock = "";
        s_fnBlock = 0;
        s_pw = nullptr;
        s_pload = nullptr;
        return false;
    }
    s_pw = nullptr;
    s_pload = nullptr;
    return true;
}

void PrintWorldWalk3(const WORLDWALK3& w, FILE* pf, bool fGroups, bool fObjects)
{
    std::fprintf(pf, "file %zu bytes, walked to %zu (%zu after the last group)\n", w.cbFile, w.ibEnd, w.cbFile - w.ibEnd);
    auto printPrefix = [&](const char* sz, const std::vector<PREFIX3>& a) {
        std::fprintf(pf, "prefix %s (%zu):", sz, a.size());
        for (const PREFIX3& p : a)
        {
            std::fprintf(pf, " (%d,%d,[", p.key, p.value);
            for (std::size_t i = 0; i < p.alist.size(); ++i) std::fprintf(pf, "%s%d", i ? "," : "", p.alist[i]);
            std::fprintf(pf, "])");
        }
        std::fprintf(pf, "\n");
    };
    printPrefix("A", w.aprefixA);
    printPrefix("B", w.aprefixB);
    int cskipped = 0, cmatch = 0, cgroupData = 0;
    for (const GROUP3& g : w.agroup)
    {
        cskipped += g.fSkipped;
        if (!g.fSkipped)
        {
            ++cgroupData;
            cmatch += g.ibEnd - g.ibStart == g.cb;
        }
    }
    std::fprintf(pf, "groups %zu (%d without instances, skipped by size; %d of %d loaded groups end exactly at their size)\n",
                 w.agroup.size(), cskipped, cmatch, cgroupData);
    std::fprintf(pf, "progress %d names, %d records; music: %d sounds, %d dialogs, %d music streams, %d languages; text %d strings\n",
                 w.cprogressName, w.cprogressRecord, w.csound, w.cdialog, w.cmusic, w.clang, w.ctext);
    std::fprintf(pf, "world options %d; option sets %d / %d / %d blocks; nodes %d in %d sets\n", w.coptSw, w.coptsetA,
                 w.coptsetB, w.coptsetBlock, w.cnode, w.cnodeSet);
    std::fprintf(pf, "option sets: A");
    for (const OPTSETKEY3& k : w.aoptsetA) std::fprintf(pf, " (%u->%u)", k.key, k.value);
    std::fprintf(pf, "; B");
    for (const OPTSETKEY3& k : w.aoptsetB) std::fprintf(pf, " (%zu progress, %u->%u)", k.aprogress.size(), k.key, k.value);
    std::fprintf(pf, "; blocks");
    for (std::size_t i = 0; i < w.aidOptset.size(); ++i)
        std::fprintf(pf, " %u:%d", w.aidOptset[i], i < w.acoptOptset.size() ? w.acoptOptset[i] : -1);
    if (w.iblockChosen >= 0) std::fprintf(pf, "; chosen block %d (id %d)", w.iblockChosen, w.idChosen);
    std::fprintf(pf, "\n");
    std::fprintf(pf, "splice: %d cells; tags:", w.cpairSplice);
    for (auto [tag, c] : w.mptagc) std::fprintf(pf, " %d:%d", tag, c);
    std::fprintf(pf, "\n");
    if (fGroups)
        for (std::size_t i = 0; i < w.agroup.size(); ++i)
        {
            const GROUP3& g = w.agroup[i];
            std::fprintf(pf, "group %3zu %-36.64s %4d %4d %4d %4d inst %3d cb %9u at %9zu..%9zu%s", i, g.achName, g.x5C, g.x60,
                         g.x10, g.x14, g.cinst, g.cb, g.ibStart, g.ibEnd,
                         g.fSkipped ? " skipped" : (g.ibEnd - g.ibStart == g.cb ? "" : " (size differs)"));
            if (!g.fSkipped)
                std::fprintf(pf, ": bulk %d key %d dbg %d, %u modules, cluts %d/%d bmps %d/%d saa %d shd %d, cmesh %u, %d objects",
                             g.cbBulk, g.keyBulk, g.fScriptDebug, g.cmodule, g.cclutA, g.cclutB, g.cbmpA, g.cbmpB, g.csaa,
                             g.cshd, g.ccmesh, g.cobj);
            std::fprintf(pf, "\n");
        }
    std::fprintf(pf, "objects %zu (top level); by header mode:", w.aobj.size());
    for (auto [mode, c] : w.mpmodec) std::fprintf(pf, " %d:%d", mode, c);
    std::fprintf(pf, "\nclass census (top level; cid3 count name3 [sly2 twin]):\n");
    for (auto [cid, c] : w.mpcidc)
        std::fprintf(pf, "  0x%02X %5d %-16s %s\n", cid, c, SzCid3(cid), SzCid3Twin(cid).c_str());
    if (fObjects)
    {
        std::fprintf(pf, "# offset group mode cid3 oid n cb [filters] [conditions]\n");
        for (const OBJHDR3& o : w.aobj)
        {
            std::fprintf(pf, "%9zu %3d %2d 0x%02X %7d %4d %8d", o.ib, o.igrp, o.mode, o.cid, o.oid, o.n, o.cb);
            if (!o.afilter.empty() || !o.acond.empty())
            {
                std::fprintf(pf, " [");
                for (std::size_t i = 0; i < o.afilter.size(); ++i) std::fprintf(pf, "%s%d", i ? "," : "", o.afilter[i]);
                std::fprintf(pf, "] [");
                for (std::size_t i = 0; i < o.acond.size(); ++i) std::fprintf(pf, "%s%d", i ? "," : "", o.acond[i]);
                std::fprintf(pf, "]");
            }
            std::fprintf(pf, "\n");
        }
    }
}
