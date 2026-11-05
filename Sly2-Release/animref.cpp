// Differential checks of the animation runtime against the reference port's RAM (Sly2-Reference, the original code):
//
//   Sly2-Release --anim-ref W A.ram B.ram [-v]
//
// A and B are EE RAM snapshots of the reference at the start of two consecutive frames (SLY2_DUMP_AT=14D0B8:K and
// :K+1: the entry of UpdateClock, before the frame's updates). The native world W is loaded; every ALO is mapped to
// the reference's by (class, oid, creation order); the animation state of A is imported (every record's local
// and world matrix and motion state, every playing ASEGA with its controllers, every SMA); then one native frame
// runs (the records of the objects the reference updated in that frame, then the ASEGAs) and the result is
// compared with B: each animated record's local matrix, mode and state, each ASEGA's time and event index.

#include "refram.h"
#include "gang.h"
#include "physworld.h"
#include "update.h"
#include "so.h"
#include "sly.h"
#include "anim.h"
#include "clock.h"
#include "animtest.h"
#include "asega.h"
#include "blend.h"
#include "aiimport.h"
#include "aipursuit.h"
#include "ai.h"
#include "actanim.h"
#include "bone.h"
#include "brx.h"
#include "cls.h"
#include "smrun.h"
#include "sw.h"
#include "xf.h"
#include "xfmath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32_t RAM_SIZE = 0x2000000;
constexpr uint32_t SW_ADDR = 0x3DD490;

}  // namespace

// (class, oid, creation order): the reference's class lists (SW +0x3038 + cid * 12, next at +0x20) are in
// creation order like the native ones.
void MapObjects(SW* psw, const RAM& ram, MAPPING& map)
{
    for (CID cid = 0; cid < CID_Max; ++cid)
    {
        std::map<OID, std::vector<uint32_t>> mpoida;
        for (uint32_t p = ram.U32(SW_ADDR + 0x3038 + cid * 12), c = 0; ram.FValid(p) && c < 100000; p = ram.U32(p + 0x20), ++c)
            mpoida[ram.S32(p + 0x18)].push_back(p);
        std::map<OID, int> mpoidord;
        for (LO* plo : psw->mpcidaplo[cid])
        {
            const int ord = mpoidord[plo->oid]++;
            auto it = mpoida.find(plo->oid);
            if (it == mpoida.end() || ord >= static_cast<int>(it->second.size()))
            {
                ++map.cunmapped;
                continue;
            }
            map.mpaplo[it->second[ord]] = plo;
            map.mpploa[plo] = it->second[ord];
            ++map.cmapped;
        }
    }
}

// SMs whose transitions use other ASEGs than the ordinal mapping's: re-mapped by their transitions' ASEG oids (the
// world loader places shared groups' SMs differently, SM 386)
void RemapSms(SW* psw, const RAM& ram, MAPPING& map)
{
    auto keyRam = [&](uint32_t psm) {
        std::vector<int> a;
        const int c = ram.S32(psm + 0x58);
        const uint32_t at = ram.U32(psm + 0x5C);
        for (int i = 0; i < c && i < 256; ++i)
        {
            const uint32_t paseg = ram.U32(at + 0x18 * i);
            a.push_back(paseg ? ram.S32(paseg + 0x18) : -1);
        }
        return a;
    };
    auto keyNative = [&](SM* psm) {
        InitSm(psm);
        std::vector<int> a;
        for (const SM::SMT& t : psm->asmt) a.push_back(t.paseg ? t.paseg->oid : -1);
        return a;
    };
    std::map<std::vector<int>, std::vector<SM*>> mpkeyapsm;
    for (LO* plo : psw->mpcidaplo[0x6F]) mpkeyapsm[keyNative(static_cast<SM*>(plo))].push_back(static_cast<SM*>(plo));
    std::map<std::vector<int>, size_t> mpkeyi;
    int cremap = 0;
    for (uint32_t p = ram.U32(SW_ADDR + 0x3038 + 0x6F * 12), c = 0; ram.FValid(p) && c < 100000; p = ram.U32(p + 0x20), ++c)
    {
        const std::vector<int> k = keyRam(p);
        auto it = mpkeyapsm.find(k);
        if (it == mpkeyapsm.end()) continue;
        size_t& i = mpkeyi[k];
        if (i >= it->second.size()) continue;
        SM* psm = it->second[i++];
        auto itOld = map.mpaplo.find(p);
        if (itOld != map.mpaplo.end() && itOld->second == psm) continue;
        if (itOld != map.mpaplo.end()) map.mpploa.erase(itOld->second);
        map.mpaplo[p] = psm;
        map.mpploa[psm] = p;
        ++cremap;
    }
    if (cremap) std::printf("SMs re-mapped by their transitions: %d\n", cremap);
}

namespace
{
XF* PxfOfRam(const RAM& ram, const MAPPING& map, uint32_t aalo, uint32_t axf, ALO** ppalo)
{
    auto it = map.mpaplo.find(aalo);
    if (it == map.mpaplo.end() || !FCidDerivedFrom(it->second->Cid(), 0x30)) return nullptr;
    ALO* palo = static_cast<ALO*>(it->second);
    const uint32_t base = ram.U32(aalo + 0x54);
    if (axf < base || (axf - base) % 0x100) return nullptr;
    const uint32_t i = (axf - base) / 0x100;
    if (i >= palo->axf.size() || static_cast<int32_t>(i) >= ram.S32(aalo + 0x50)) return nullptr;
    if (ppalo) *ppalo = palo;
    return palo->axf[i].get();
}

// A record's state from the reference.
void ImportXf(const RAM& ram, uint32_t a, XF* pxf)
{
    pxf->mat = ram.Mat4(a);
    pxf->matWorld = ram.Mat4(a + 0x40);
    pxf->g0 = ram.F32(a + 0x80);
    pxf->g1 = ram.F32(a + 0x84);
    pxf->mode = ram.S16(a + 0x98);
    pxf->state = ram.S16(a + 0x9A);
    pxf->nStamp = ram.S32(a + 0xA4);
    // (the change counter at least the imported stamps: the entries' world geometry follows a record's stamp, so the
    // native bumps must be newer, the physics port)
    if (g_cXfStamp < pxf->nStamp) g_cXfStamp = pxf->nStamp;
    pxf->tTarget = ram.F32(a + 0xA8);
    pxf->vel = ram.Vec4(a + 0xB0);
    for (int i = 0; i < 4; ++i) pxf->aseg[i] = ram.Vec4(a + 0xC0 + 16 * i);
}

float DiffMat(const glm::mat4& a, const glm::mat4& b)
{
    float d = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 3; ++j) d = std::max(d, std::fabs(a[i][j] - b[i][j]));
    return d;
}
}  // namespace

namespace
{
AIREF* s_paiRef = nullptr;  // --ai: the guards' AI checked too (aiimport.h)

struct REFRUN
{
    SW* psw = nullptr;
    MAPPING map;
    std::unordered_map<uint32_t, ASEGA*> mpaasega;  // reference ASEGA -> native
    std::unordered_map<uint32_t, SMA*> mpasma;
    std::unordered_map<uint32_t, ASEGBL*> mpablend;  // reference blend (cid 0x6E, made by the actors) -> native
    bool fVerbose = false;
};

bool FRamBlend(const RAM& ram, uint32_t p)
{
    for (uint32_t q = ram.U32(SW_ADDR + 0x3038 + 0x6E * 12), c = 0; ram.FValid(q) && c < 100000; q = ram.U32(q + 0x20), ++c)
        if (q == p) return true;
    return false;
}

// The BLs' weights of a reference blend (true when they differ from the native ones)
bool FImportBlendWeights(const RAM& ram, uint32_t pab, ASEGBL* pbl)
{
    const uint32_t ab = ram.U32(pab + 0xC8);
    const int cb = ram.S32(pab + 0xC0);
    bool f = false;
    for (size_t i = 0; i < pbl->abl.size(); ++i)
    {
        const float w = ram.F32(ab + cb * static_cast<uint32_t>(i));
        f = f || w != pbl->abl[i].weight;
        pbl->abl[i].weight = w;
    }
    return f;
}

// A native blend made from a reference one with NewBlend (its BLs' ASEGs and segment tables, the channels' tables
// as the actor chose them), its weights; then its derived state (length, blended tables, channels) checked.
ASEGBL* PblImport(REFRUN& rr, const RAM& ram, uint32_t pab)
{
    if (auto it = rr.mpablend.find(pab); it != rr.mpablend.end()) return it->second;
    auto itAlo = rr.map.mpaplo.find(ram.U32(pab + 0x10));
    if (itAlo == rr.map.mpaplo.end() || !FCidDerivedFrom(itAlo->second->Cid(), 0x30)) return nullptr;
    const int cbl = ram.S32(pab + 0xC4), cb = ram.S32(pab + 0xC0), ctable = ram.S32(pab + 0xCC);
    const uint32_t ab = ram.U32(pab + 0xC8), atable = ram.U32(pab + 0xD0);
    if (cbl <= 0 || cbl > 64 || ctable < 0 || ctable > 8) return nullptr;
    std::vector<BLENDBL> abl(cbl);
    std::vector<int> actable(ctable);
    for (int t = 0; t < ctable; ++t) actable[t] = ram.S32(atable + 12 * t + 4);
    for (int i = 0; i < cbl; ++i)
    {
        const uint32_t pe = ab + cb * i;
        if (const uint32_t pa = ram.U32(pe + 4))
        {
            auto it = rr.map.mpaplo.find(pa);
            if (it == rr.map.mpaplo.end()) return nullptr;
            abl[i].paseg = static_cast<ASEG*>(it->second);
        }
        abl[i].aamrsg.resize(ctable);
        for (int t = 0; t < ctable; ++t)
        {
            const uint32_t ib = ram.U32(atable + 12 * t);
            for (int k = 0; k < actable[t]; ++k)
                abl[i].aamrsg[t].push_back(MRSG{ram.F32(pe + ib + 8 * k), ram.F32(pe + ib + 8 * k + 4)});
        }
    }
    ASEGBL* pbl = NewBlend(static_cast<ALO*>(itAlo->second), abl, actable);
    if (!pbl) return nullptr;
    // the channels' tables (0x20B868 sets them by the target records' tags)
    const int cchn = ram.S32(pab + 0x5C);
    const uint32_t achn = ram.U32(pab + 0x60);
    int cchnOk = 0;
    for (int j = 0; j < cchn; ++j)
    {
        const uint32_t w = ram.U32(achn + 8 * j), ptrk = ram.U32(achn + 8 * j + 4);
        const int oid = static_cast<int32_t>(w << 12) >> 12;
        const int isub = static_cast<int32_t>((w >> 20) << 22) >> 22;
        const uint32_t pt = ram.U32(ptrk + 8);
        const int itable = pt ? static_cast<int>((pt - atable) / 12) : -1;
        for (ASEGCHN& chn : pbl->asegd.achn)
            if (chn.oid == oid && chn.isub == isub && bool(chn.fScalar) == bool(w & 0x80000000u))
            {
                SetBlendChannelTable(&chn, itable);
                // (the value of records without the channel in a BL: the record's at the actor's post-load)
                if (auto* p = dynamic_cast<TRKPBL*>(chn.ptrkp.get())) p->v = ram.Vec4(ptrk + 0x10);
                ++cchnOk;
            }
    }
    FImportBlendWeights(ram, pab, pbl);
    RecalcBlend(pbl);
    float dTable = 0.0f;
    for (int t = 0; t < ctable; ++t)
        for (int k = 0; k < actable[t]; ++k)
        {
            const uint32_t pm = ram.U32(atable + 12 * t + 8) + 8 * k;
            dTable = std::max({dTable, std::fabs(ram.F32(pm) - pbl->aamrsg[t][k].t), std::fabs(ram.F32(pm + 4) - pbl->aamrsg[t][k].dt)});
        }
    std::printf("  blend on alo %d: %d BLs, %d tables, channels ref %d native %zu (%d matched), length ref %.5f native "
                "%.5f, dominant ref %d native %d, tables diff %.3g\n",
                itAlo->second->oid, cbl, ctable, cchn, pbl->asegd.achn.size(), cchnOk, ram.F32(pab + 0x74), pbl->TMax(),
                ram.S32(pab + 0xD4), pbl->iblDominant, dTable);
    rr.mpablend[pab] = pbl;
    rr.map.mpaplo[pab] = pbl;
    rr.map.mpploa[pbl] = pab;
    return pbl;
}

// The records' parameter pointer from the reference: an ASEGA's time, the record's own +0x80, or the constant.
void ImportXfParams(REFRUN& rr, const RAM& ram, uint32_t ar, XF* pxf)
{
    const uint32_t pg = ram.U32(ar + 0x88);
    if (pg == ar + 0x80)
    {
        ResetXfParams(pxf);
        pxf->g0 = ram.F32(ar + 0x80);
        pxf->g1 = ram.F32(ar + 0x84);
        return;
    }
    for (const auto& [pa, pasega] : rr.mpaasega)
        if (pg == pa + 0x1C)
        {
            pxf->pg = &pasega->t;
            pxf->pdgScale = &pasega->svt;
        }
}

// One ASEGA of the reference (its records keep their imported state: saved and restored around ApplyAseg, whose
// snap moves them). False when its ASEG or object is not mapped.
ASEGA* PasegaImport(REFRUN& rr, const RAM& ram, uint32_t pa, int* pclinkDiff)
{
    auto itAseg = rr.map.mpaplo.find(ram.U32(pa + 0x14));
    if (itAseg == rr.map.mpaplo.end() && FRamBlend(ram, ram.U32(pa + 0x14)) && PblImport(rr, ram, ram.U32(pa + 0x14)))
        itAseg = rr.map.mpaplo.find(ram.U32(pa + 0x14));
    const uint32_t aalo = ram.U32(pa + 0x18);
    auto itAlo = aalo ? rr.map.mpaplo.find(aalo) : rr.map.mpaplo.end();
    if (itAseg == rr.map.mpaplo.end() || (aalo && itAlo == rr.map.mpaplo.end())) return nullptr;
    ASEG* paseg = static_cast<ASEG*>(itAseg->second);
    ALO* palo = aalo ? static_cast<ALO*>(itAlo->second) : nullptr;
    std::vector<std::pair<XF*, uint32_t>> axf;
    for (uint32_t pl = ram.U32(pa + 0x34); ram.FValid(pl); pl = ram.U32(pl + 0x28))
    {
        ALO* q;
        if (XF* pxf = PxfOfRam(ram, rr.map, ram.U32(pl + 4), ram.U32(pl + 8), &q)) axf.emplace_back(pxf, ram.U32(pl + 8));
    }
    ASEGA* pasega = nullptr;
    ApplyAseg(paseg, palo, ram.F32(pa + 0x1C), ram.F32(pa + 0x44), ram.S32(pa + 0x3C), &pasega);
    pasega->t = ram.F32(pa + 0x1C);
    pasega->tEvent = ram.F32(pa + 0x20);
    pasega->svt = ram.F32(pa + 0x40);
    pasega->svtLocal = ram.F32(pa + 0x44);
    pasega->svtAseg = ram.F32(pa + 0x48);
    pasega->svtSuck = ram.F32(pa + 0x4C);
    pasega->iea = ram.S32(pa + 0x50);
    pasega->ieaEnd = ram.S32(pa + 0x54);
    pasega->fSound = ram.S32(pa + 0x58);
    rr.mpaasega[pa] = pasega;
    for (auto& [pxf, axfRam] : axf)
    {
        CTRL* pctrlFirst = pxf->pctrlFirst;
        ImportXf(ram, axfRam, pxf);
        pxf->pctrlFirst = pctrlFirst;
        ImportXfParams(rr, ram, axfRam, pxf);
    }
    if (pclinkDiff)
    {
        int cRef = 0, cNat = 0;
        for (uint32_t pl = ram.U32(pa + 0x34); ram.FValid(pl); pl = ram.U32(pl + 0x28)) ++cRef;
        for (CTRLASEG* p = pasega->pctrlFirst; p; p = p->pnextAsega) ++cNat;
        *pclinkDiff += std::abs(cRef - cNat);
        if (cRef != cNat && rr.fVerbose)
        {
            std::printf("  asega of aseg %d (alo %d): %d controllers in the reference, %d native:", paseg->oid,
                        palo ? palo->oid : -1, cRef, cNat);
            for (uint32_t pl = ram.U32(pa + 0x34); ram.FValid(pl); pl = ram.U32(pl + 0x28))
            {
                const uint32_t aalo = ram.U32(pl + 4);
                std::printf(" ref(%d xf %d mapped %d)", ram.S32(aalo + 0x18), (ram.U32(pl + 8) - ram.U32(aalo + 0x54)) / 0x100,
                            int(rr.map.mpaplo.count(aalo)));
            }
            for (CTRLASEG* p = pasega->pctrlFirst; p; p = p->pnextAsega) std::printf(" nat(%d)", p->palo->oid);
            std::printf("\n");
        }
    }
    return pasega;
}

// One SMA of the reference.
SMA* PsmaImport(REFRUN& rr, const RAM& ram, uint32_t ps)
{
    auto itSm = rr.map.mpaplo.find(ram.U32(ps + 0x24));
    const uint32_t aalo = ram.U32(ps + 0x28);
    auto itAlo = aalo ? rr.map.mpaplo.find(aalo) : rr.map.mpaplo.end();
    if (itSm == rr.map.mpaplo.end() || (aalo && itAlo == rr.map.mpaplo.end())) return nullptr;
    SM* psm = static_cast<SM*>(itSm->second);
    InitSm(psm);
    SMA* psma = new SMA();
    psma->psm = psm;
    psma->palo = aalo ? static_cast<ALO*>(itAlo->second) : nullptr;
    psma->grfapl = ram.S32(ps + 0x2C);
    psma->ismsCur = ram.S32(ps + 0x34);
    psma->ismsNext = ram.S32(ps + 0x38);
    psma->ismsGoal = ram.S32(ps + 0x3C);
    const uint32_t psmt = ram.U32(ps + 0x40);
    const uint32_t asmt = ram.U32(ram.U32(ps + 0x24) + 0x5C);
    if (psmt && psmt >= asmt)
    {
        const uint32_t i = (psmt - asmt) / 0x18;
        if (i < psm->asmt.size()) psma->psmt = &psm->asmt[i];
    }
    psma->svtLocal = ram.F32(ps + 0x44);
    psma->grfMask = ram.U32(ps + 0x48);
    if (const uint32_t pa = ram.U32(ps + 0x30))
    {
        auto it = rr.mpaasega.find(pa);
        if (it != rr.mpaasega.end() && !it->second->fRetracted)
        {
            psma->pasega = it->second;
            it->second->aplistener.push_back(psma);
        }
    }
    g_apsma.push_back(psma);
    psm->apsma.push_back(psma);
    rr.mpasma[ps] = psma;
    return psma;
}

void ImportState(REFRUN& rr, const RAM& ramA)
{
    // the update-table flags (orig +0x40: the entry's flags while in the table)
    {
        const int c = ramA.S32(SW_ADDR + 0x3F14);
        const uint32_t ae = ramA.U32(SW_ADDR + 0x3F20);
        for (int i = 0; i < c && i < 100000; ++i)
        {
            auto it = rr.map.mpaplo.find(ramA.U32(ae + 8 * i + 4));
            if (it != rr.map.mpaplo.end()) it->second->grfUpd = ramA.U32(ae + 8 * i);
        }
    }
    int crecImported = 0;
    for (const auto& [a, plo] : rr.map.mpaplo)
    {
        if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        ALO* palo = static_cast<ALO*>(plo);
        const int c = ramA.S32(a + 0x50);
        const uint32_t base = ramA.U32(a + 0x54);
        if (c != static_cast<int>(palo->axf.size())) continue;
        for (int i = 0; i < c; ++i)
        {
            ImportXf(ramA, base + 0x100 * i, palo->axf[i].get());
            ++crecImported;
        }
    }
    int casega = 0, casegaSkipped = 0, clink = 0, clinkDiff = 0;
    for (uint32_t pa : ramA.Dl(0x3E1218))
    {
        for (uint32_t pl = ramA.U32(pa + 0x34); ramA.FValid(pl); pl = ramA.U32(pl + 0x28)) ++clink;
        if (PasegaImport(rr, ramA, pa, &clinkDiff)) ++casega;
        else ++casegaSkipped;
    }
    for (const auto& [a, plo] : rr.map.mpaplo)
    {
        if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        ALO* palo = static_cast<ALO*>(plo);
        const int c = ramA.S32(a + 0x50);
        const uint32_t base = ramA.U32(a + 0x54);
        if (c != static_cast<int>(palo->axf.size())) continue;
        for (int i = 0; i < c; ++i) ImportXfParams(rr, ramA, base + 0x100 * i, palo->axf[i].get());
    }
    int csma = 0, csmaSkipped = 0;
    for (uint32_t ps : ramA.Dl(0x3E126C))
    {
        if (PsmaImport(rr, ramA, ps)) ++csma;
        else ++csmaSkipped;
    }
    std::printf("imported: %d records, %d ASEGAs (%d not mapped), %d controllers (%d differ in number), %d SMAs "
                "(%d not mapped)\n",
                crecImported, casega, casegaSkipped, clink, clinkDiff, csma, csmaSkipped);
}

// The ALOs whose records the reference updated between two snapshots (a stamp changed).
std::vector<ALO*> ApaloUpdated(REFRUN& rr, const RAM& ramA, const RAM& ramB)
{
    std::vector<ALO*> a;
    for (const auto& plo : rr.psw->aploAll)
    {
        if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        auto it = rr.map.mpploa.find(plo.get());
        if (it == rr.map.mpploa.end()) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        if (ramA.S32(it->second + 0x50) != static_cast<int>(palo->axf.size())) continue;
        const uint32_t baseA = ramA.U32(it->second + 0x54), baseB = ramB.U32(it->second + 0x54);
        bool f = false;
        for (size_t i = 0; i < palo->axf.size() && !f; ++i)
            f = ramB.S32(baseB + 0x100 * i + 0xA4) != ramA.S32(baseA + 0x100 * i + 0xA4);
        if (f) a.push_back(palo);
    }
    return a;
}

struct FRAMESTATS
{
    int crec = 0, crecOk = 0, casega = 0, casegaOk = 0, csma = 0, csmaOk = 0, casegaNew = 0, casegaNewOk = 0;
    int crecOk3 = 0, cflag = 0, cflagOk = 0;
    int casegaExternal = 0, casegaExternalEnd = 0, csmaExternal = 0, casegaExtra = 0;
    float dMax = 0;
};

// One frame from snapshot A to B: the objects the reference updated (update-list flag 4 after the ASEGAs), the
// ASEGAs; then the comparison of every record of the updated objects, the ASEGAs and SMAs with B.
FRAMESTATS StepAndCompare(REFRUN& rr, const RAM& ramA, const RAM& ramB)
{
    FRAMESTATS fs;
    g_uRandState = ramA.U32(ramA.U32(0x2C49D4) + 0x58);  // (the C library's rand state at the frame's start)
    g_clock.t = ramB.F32(0x2DDED0);
    g_clock.dt = ramB.F32(0x2DDED4);
    g_clock.dt40 = ramB.F32(0x2DDF10);
    if (s_paiRef)
    {
        // the guards' AI from A, one frame of it (their vt slot 16 runs before the world's ASEGAs and physics)
        AiImport(ramA, s_paiRef);
        // (the pause counts as other code leaves them in the frame: dialogs / menus start before the guards' update)
        if (!std::getenv("SLY2N_AIPAUSE_A"))  // (SLY2N_AIPAUSE_A: the frame start's)
        {
            g_gamepause.c78 = ramB.S32(REF_SW_ADDR + 0x3D78);
            g_gamepause.c80 = ramB.S32(REF_SW_ADDR + 0x3D80);
            g_gamepause.c84 = ramB.S32(REF_SW_ADDR + 0x3D84);
        }
        AiStep(s_paiRef);
        AiDeliverHits(ramA, ramB, s_paiRef);
    }
    // the blends' weights as the actors set them in the frame (then RecalcBlend, as they do before the ASEGAs)
    for (const auto& [pab, pbl] : rr.mpablend)
        if (FImportBlendWeights(ramB, pab, pbl)) RecalcBlend(pbl);
    std::unordered_map<uint32_t, uint32_t> mpagrf;
    {
        const int c = ramA.S32(SW_ADDR + 0x3F14);
        const uint32_t ae = ramA.U32(SW_ADDR + 0x3F20);
        for (int i = 0; i < c && i < 100000; ++i) mpagrf[ramA.U32(ae + 8 * i + 4)] = ramA.U32(ae + 8 * i);
    }
    // the reference's late pass (0x1F8DD8): its table entries with flag 4 (not 0x20); natively the objects' own
    // flags (set by mode changes, cleared when nothing moved) decide, for the objects in the reference's table
    auto fLateRef = [&](ALO* palo) {
        auto it = mpagrf.find(rr.map.mpploa[palo]);
        return it != mpagrf.end() && (it->second & 0x24) == 4;
    };
    auto fInTable = [&](ALO* palo) { return mpagrf.count(rr.map.mpploa[palo]) != 0; };
    auto fLate = [&](ALO* palo) { return fInTable(palo) && (GrfUpdOfLo(palo) & 0x24) == 4; };
    // the records no animation controller drives (moved by the game's other code: paths, physics, actors) taken
    // from A at the frame's start, so the driven ones see the same parents and movers
    for (const auto& [a, plo] : rr.map.mpaplo)
    {
        if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        ALO* palo = static_cast<ALO*>(plo);
        const int c = ramA.S32(a + 0x50);
        const uint32_t base = ramA.U32(a + 0x54);
        if (c != static_cast<int>(palo->axf.size())) continue;
        for (int i = 0; i < c; ++i)
        {
            XF* pxf = palo->axf[i].get();
            // (velocity modes 1-3, splines 17: the actors' / physics' code sets them; 15 / 16 physics' own)
            const int modeA = ramA.S16(base + 0x100 * static_cast<uint32_t>(i) + 0x98);
            const bool fExternal = !pxf->pctrlFirst || modeA == 1 || modeA == 2 || modeA == 3 || modeA == 15 ||
                                   modeA == 16 || modeA == 17;
            if (!fExternal) continue;
            const float* pg = pxf->pg;
            const float* pdg = pxf->pdgScale;
            CTRL* pctrl = pxf->pctrlFirst;
            // (SLY2_ANIMREF_EXT=B: their state at the frame's end, as the actors' code moved them before the
            // records' update)
            static const bool s_fExtB = std::getenv("SLY2_ANIMREF_EXT") && *std::getenv("SLY2_ANIMREF_EXT") == 'B';
            ImportXf(s_fExtB ? ramB : ramA, (s_fExtB ? ramB.U32(a + 0x54) : base) + 0x100 * static_cast<uint32_t>(i), pxf);
            pxf->pctrlFirst = pctrl;
            pxf->pg = pg;
            pxf->pdgScale = pdg;
        }
    }
    const std::vector<ALO*> apalo = ApaloUpdated(rr, ramA, ramB);
    // the late set before the frame, native against the reference
    for (const auto& [a, plo] : rr.map.mpaplo)
    {
        if (!FCidDerivedFrom(plo->Cid(), 0x30) || !fInTable(static_cast<ALO*>(plo))) continue;
        ++fs.cflag;
        if (fLate(static_cast<ALO*>(plo)) == fLateRef(static_cast<ALO*>(plo))) ++fs.cflagOk;
        else if (rr.fVerbose)
            std::printf("  flag 4 of alo %d (cid 0x%02X): reference %d native %d\n", plo->oid, plo->Cid(),
                        int(fLateRef(static_cast<ALO*>(plo))), int(fLate(static_cast<ALO*>(plo))));
    }
    for (ALO* palo : apalo)
        if (!fLateRef(palo)) UpdateAloXfs(palo, g_clock.t, g_clock.dt);  // (updated earlier: physics, other code)
    UpdateAsegas();
    std::vector<ALO*> apaloLate;
    for (const auto& [a, plo] : rr.map.mpaplo)
        if (FCidDerivedFrom(plo->Cid(), 0x30) && fLate(static_cast<ALO*>(plo))) apaloLate.push_back(static_cast<ALO*>(plo));
    std::sort(apaloLate.begin(), apaloLate.end(), [&](ALO* p, ALO* q) { return rr.map.mpploa[p] < rr.map.mpploa[q]; });
    for (ALO* palo : apaloLate) UpdateAloXfs(palo, g_clock.t, g_clock.dt);

    if (const char* pszDbg = std::getenv("SLY2_ANIM_DEBUG"))  // oid:record
    {
        int oid = -1, ixf = -1;
        std::sscanf(pszDbg, "%d:%d", &oid, &ixf);
        for (ALO* palo : apalo)
            if (palo->oid == oid && ixf >= 0 && ixf < static_cast<int>(palo->axf.size()))
            {
                const XF* pxf = palo->axf[ixf].get();
                const glm::vec4 v = VecXfTarget(pxf);
                std::printf("  debug %d:%d g %.6f dg %.4f [%.4f %.4f] state %d seg", oid, ixf, pxf->G(), pxf->DgScale(),
                            pxf->g0, pxf->g1, pxf->state);
                for (int j = 0; j < 4; ++j) std::printf(" (%.4f %.4f %.4f)", pxf->aseg[j].x, pxf->aseg[j].y, pxf->aseg[j].z);
                std::printf(" value %.5f %.5f %.5f mat %.4f %.4f %.4f late %d\n", v.x, v.y, v.z, pxf->mat[0][0], pxf->mat[0][1],
                            pxf->mat[0][2], int(fLate(palo)));
                const uint32_t ar = ramB.U32(rr.map.mpploa[palo] + 0x54) + 0x100 * static_cast<uint32_t>(ixf);
                std::printf("    ref: mode %d/%d prio %d seg (%.4f %.4f %.4f) (%.4f %.4f %.4f) mat %.4f %.4f %.4f vel %.4f %.4f %.4f\n",
                            ramB.S16(ar + 0x98), ramB.S16(ar + 0x9A), ramB.S32(ramB.U32(ar + 0x90) + 0xC), ramB.F32(ar + 0xC0),
                            ramB.F32(ar + 0xC4), ramB.F32(ar + 0xC8), ramB.F32(ar + 0xD0), ramB.F32(ar + 0xD4), ramB.F32(ar + 0xD8),
                            ramB.F32(ar), ramB.F32(ar + 4), ramB.F32(ar + 8), ramB.F32(ar + 0xB0), ramB.F32(ar + 0xB4), ramB.F32(ar + 0xB8));
                std::printf("    native: prio %d vel %.4f %.4f %.4f\n", pxf->pctrlFirst ? pxf->pctrlFirst->prio : -99, pxf->vel.x, pxf->vel.y, pxf->vel.z);
                if (pxf->pctrlFirst)
                {
                    glm::vec4 qp(0.0f), qv(0.0f);
                    pxf->pctrlFirst->Query(&qp, &qv);
                    std::printf("    native query: %.4f %.4f %.4f vel %.4f %.4f %.4f\n", qp.x, qp.y, qp.z, qv.x, qv.y, qv.z);
                }
            }
    }
    if (const char* pszIk = std::getenv("SLY2_ANIM_IKDEBUG"))  // oid: the joints' inputs, native and reference
    {
        const int oid = std::atoi(pszIk);
        for (const auto& [aDbg, ploDbg] : rr.map.mpaplo)
        {
            if (!FCidDerivedFrom(ploDbg->Cid(), 0x30)) continue;
            ALO* palo = static_cast<ALO*>(ploDbg);
            for (const ALOPAIR& q : palo->apair)
            {
                if (!q.apxf[1] || q.apxf[1]->palo->oid != oid) continue;
                std::printf("  (joint of alo %d)\n", palo->oid);
                auto idx = [&](const XF* p) {
                    for (size_t i = 0; i < palo->axf.size(); ++i) if (palo->axf[i].get() == p) return int(i);
                    return -1; };
                std::printf("  joint upper %d lower %d end %d target %d: l %.4f %.4f bend %.4f axis %.4f %.4f %.4f w %.4f\n",
                            idx(q.apxf[1]), idx(q.apxf[2]), idx(q.apxf[3]), idx(q.apxf[0]), q.sUpper, q.sLower, q.radBendRest,
                            q.axis.x, q.axis.y, q.axis.z, q.gWeight);
                if (q.apxf[1] && q.apxf[1]->pctrlFirst)
                {
                    const uint32_t axfU = ramB.U32(rr.map.mpploa[palo] + 0x54) + 0x100 * static_cast<uint32_t>(idx(q.apxf[1]));
                    uint32_t pc = ramB.U32(axfU + 0x90);
                    while (ramB.FValid(pc) && ramB.S32(pc) != 7) pc = ramB.U32(pc + 0x10);
                    const uint32_t aq = ramB.U32(pc + 0x1C);
                    std::printf("    ref: l %.4f %.4f bend %.4f axis %.4f %.4f %.4f w %.4f weight-ease %08x\n", ramB.F32(aq + 0xC0),
                                ramB.F32(aq + 0xC4), ramB.F32(aq + 0xC8), ramB.F32(aq + 0x90), ramB.F32(aq + 0x94), ramB.F32(aq + 0x98),
                                ramB.F32(aq + 0xCC), ramB.U32(aq + 0xD0));
                }
                auto cmpW = [&](const char* psz, const XF* p) {
                    if (!p) return;
                    const uint32_t ax = ramB.U32(rr.map.mpploa[p->palo] + 0x54) + 0x100 * static_cast<uint32_t>(idx(p));
                    const glm::mat4 mR = ramB.Mat4(ax + 0x40), mL = ramB.Mat4(ax);
                    std::printf("    %s world native %.4f %.4f %.4f | %.4f %.4f %.4f  ref %.4f %.4f %.4f | %.4f %.4f %.4f; local native %.4f %.4f %.4f ref %.4f %.4f %.4f\n", psz,
                                p->matWorld[3].x, p->matWorld[3].y, p->matWorld[3].z, p->matWorld[0].x, p->matWorld[0].y, p->matWorld[0].z,
                                mR[3].x, mR[3].y, mR[3].z, mR[0].x, mR[0].y, mR[0].z, p->mat[0].x, p->mat[0].y, p->mat[0].z, mL[0].x, mL[0].y, mL[0].z);
                };
                cmpW("upper parent", q.apxf[1]->pxfRef);
                cmpW("target", q.apxf[0]);
                cmpW("upper", q.apxf[1]);
                cmpW("lower", q.apxf[2]);
                if (q.apxf[0])
                {
                    const glm::vec3 pt = glm::vec3(q.apxf[0]->matWorld * glm::vec4(q.vecA0, 1.0f));
                    std::printf("    target point native %.3f %.3f %.3f (target record mode %d ctrl %d)\n", pt.x, pt.y, pt.z,
                                q.apxf[0]->mode, q.apxf[0]->pctrlFirst ? q.apxf[0]->pctrlFirst->kind : -1);
                }
            }
        }
    }
    // the records of the updated objects that animate or move (not at rest in both)
    for (ALO* palo : apalo)
    {
        const uint32_t aalo = rr.map.mpploa[palo];
        const uint32_t base = ramB.U32(aalo + 0x54);
        for (size_t i = 0; i < palo->axf.size(); ++i)
        {
            const uint32_t axf = base + 0x100 * i;
            const XF* pxf = palo->axf[i].get();
            const int modeB = ramB.S16(axf + 0x98);
            if (modeB == 0 && pxf->mode == 0) continue;
            if (pxf->mode == 15 || pxf->mode == 16 || modeB == 15 || modeB == 16) continue;  // (physics' modes)
            {
                const int modeA = ramA.S16(ramA.U32(aalo + 0x54) + 0x100 * static_cast<uint32_t>(i) + 0x98);
                if (modeA == 1 || modeA == 2 || modeA == 3 || modeA == 17 || !pxf->pctrlFirst) continue;  // (external)
            }
            ++fs.crec;
            const glm::mat4 mB = ramB.Mat4(axf);
            // rows 0..2 absolutely (rotation / scale), the translation relative to its size
            float dRot = 0, dPos = 0;
            for (int r = 0; r < 3; ++r)
                for (int j = 0; j < 3; ++j) dRot = std::max(dRot, std::fabs(mB[r][j] - pxf->mat[r][j]));
            const float sPos = std::max(1.0f, std::fabs(mB[3][0]) + std::fabs(mB[3][1]) + std::fabs(mB[3][2]));
            for (int j = 0; j < 3; ++j) dPos = std::max(dPos, std::fabs(mB[3][j] - pxf->mat[3][j]) / sPos);
            const float d = std::max(dRot, dPos);
            const bool fMode = modeB == pxf->mode && ramB.S16(axf + 0x9A) == pxf->state;
            if (d < 1e-3f && fMode) ++fs.crecOk3;
            if (d < 1e-4f && fMode) ++fs.crecOk;
            else if (rr.fVerbose)
                std::printf("  alo %d (cid 0x%02X) xf %zu kind %d: ref mode %d/%d native %d/%d, matrix diff %.4g (g %.4f "
                            "[%.4f %.4f])\n",
                            palo->oid, palo->Cid(), i, pxf->kind, modeB, ramB.S16(axf + 0x9A), pxf->mode, pxf->state, d,
                            pxf->G(), pxf->g0, pxf->g1);
            fs.dMax = std::max(fs.dMax, d);
        }
    }
    // the ASEGAs: those of B mapped to native ones (by address, or a new one by its ASEG and object)
    std::vector<ASEGA*> apasegaNative = g_apasega;
    for (uint32_t pa : ramB.Dl(0x3E1218))
    {
        ++fs.casega;
        ASEGA* pasega = nullptr;
        auto it = rr.mpaasega.find(pa);
        const bool fNew = it == rr.mpaasega.end() || it->second->fRetracted;
        if (!fNew) pasega = it->second;
        else
        {
            ++fs.casegaNew;
            auto itAseg = rr.map.mpaplo.find(ramB.U32(pa + 0x14));
            const uint32_t aalo = ramB.U32(pa + 0x18);
            LO* palo = aalo ? rr.map.mpaplo[aalo] : nullptr;
            for (ASEGA* p : apasegaNative)
            {
                bool fUsed = false;
                for (const auto& [a2, p2] : rr.mpaasega) fUsed = fUsed || (p2 == p && !p2->fRetracted && a2 != pa);
                if (!fUsed && itAseg != rr.map.mpaplo.end() && p->paseg == itAseg->second && p->palo == palo)
                {
                    pasega = p;
                    break;
                }
            }
            if (pasega) rr.mpaasega[pa] = pasega;
        }
        const bool fOk = pasega && std::fabs(ramB.F32(pa + 0x1C) - pasega->t) < 1e-4f && ramB.S32(pa + 0x50) == pasega->iea;
        // (a speed the game's other code set (SetAsegaSpeed from scripts / actors): taken over)
        if (pasega && (ramB.F32(pa + 0x44) != pasega->svtLocal || ramB.F32(pa + 0x48) != pasega->svtAseg))
        {
            pasega->svtLocal = ramB.F32(pa + 0x44);
            pasega->svtAseg = ramB.F32(pa + 0x48);
            pasega->svt = ramB.F32(pa + 0x40);
        }
        fs.casegaOk += fOk;
        if (fNew) fs.casegaNewOk += fOk;
        if (!fOk && rr.fVerbose)
            std::printf("  asega %s of aseg %d alo %d: t ref %.5f native %.5f, iea ref %d native %d\n", fNew ? "(new)" : "",
                        rr.map.mpaplo.count(ramB.U32(pa + 0x14)) ? rr.map.mpaplo[ramB.U32(pa + 0x14)]->oid : -1,
                        ramB.U32(pa + 0x18) && rr.map.mpaplo.count(ramB.U32(pa + 0x18)) ? rr.map.mpaplo[ramB.U32(pa + 0x18)]->oid : -1,
                        ramB.F32(pa + 0x1C), pasega ? pasega->t : -1.0f, ramB.S32(pa + 0x50), pasega ? pasega->iea : -1);
    }
    // the SMAs (those of B: imported ones, and new ones the native runtime made: matched by SM and object)
    for (uint32_t ps : ramB.Dl(0x3E126C))
    {
        if (rr.mpasma.count(ps) && !rr.mpasma[ps]->fRetracted) continue;
        auto itSm = rr.map.mpaplo.find(ramB.U32(ps + 0x24));
        const uint32_t aalo = ramB.U32(ps + 0x28);
        LO* palo = aalo && rr.map.mpaplo.count(aalo) ? rr.map.mpaplo[aalo] : nullptr;
        for (SMA* p : g_apsma)
        {
            bool fUsed = false;
            for (const auto& [a2, p2] : rr.mpasma) fUsed = fUsed || (p2 == p && a2 != ps && !p2->fRetracted);
            if (!fUsed && itSm != rr.map.mpaplo.end() && p->psm == itSm->second && p->palo == palo)
            {
                rr.mpasma[ps] = p;
                break;
            }
        }
    }
    for (const auto& [ps, psma] : rr.mpasma)
    {
        if (psma->fRetracted) continue;
        ++fs.csma;
        const bool fOk = ramB.S32(ps + 0x34) == psma->ismsCur && ramB.S32(ps + 0x38) == psma->ismsNext &&
                         ramB.S32(ps + 0x3C) == psma->ismsGoal;
        fs.csmaOk += fOk;
        if (!fOk && rr.fVerbose)
            std::printf("  sma of sm %d on %d: ref cur/next/goal %d/%d/%d native %d/%d/%d\n", psma->psm->oid,
                        psma->palo ? psma->palo->oid : -1, ramB.S32(ps + 0x34), ramB.S32(ps + 0x38), ramB.S32(ps + 0x3C),
                        psma->ismsCur, psma->ismsNext, psma->ismsGoal);
    }
    // what the game's other code did in the frame (creations / retractions of ASEGAs and SMAs outside the
    // animation runtime): taken over from B so the next frame starts from the same set
    {
        std::unordered_map<uint32_t, bool> mpaLive;
        for (uint32_t pa : ramB.Dl(0x3E1218)) mpaLive[pa] = true;
        for (auto& [pa, pasega] : rr.mpaasega)
            if (!mpaLive.count(pa) && !pasega->fRetracted)
            {
                RetractAsega(pasega);
                ++fs.casegaExternalEnd;
            }
        for (uint32_t pa : ramB.Dl(0x3E1218))
        {
            auto it = rr.mpaasega.find(pa);
            if (it != rr.mpaasega.end() && !it->second->fRetracted) continue;
            if (PasegaImport(rr, ramB, pa, nullptr)) ++fs.casegaExternal;
        }
        std::unordered_map<uint32_t, bool> mpsLive;
        for (uint32_t ps : ramB.Dl(0x3E126C)) mpsLive[ps] = true;
        for (auto& [ps, psma] : rr.mpasma)
            if (!mpsLive.count(ps) && !psma->fRetracted) RetractSma(psma);
        for (uint32_t ps : ramB.Dl(0x3E126C))
        {
            auto it = rr.mpasma.find(ps);
            if (it != rr.mpasma.end() && !it->second->fRetracted) continue;
            if (PsmaImport(rr, ramB, ps)) ++fs.csmaExternal;
        }
        // the ASEGAs native SMAs made that the reference does not have (a different random choice: the game's
        // other code draws from the same generator during the frame): dropped, and their SMAs taken over from B
        std::vector<SMA*> apsmaResync;
        for (ASEGA* p : std::vector<ASEGA*>(g_apasega))
        {
            bool fMapped = false;
            for (const auto& [pa, p2] : rr.mpaasega) fMapped = fMapped || p2 == p;
            if (!fMapped)
            {
                if (rr.fVerbose)
                    std::printf("  native asega not in the reference: aseg %d alo %d t %.4f\n", p->paseg->oid,
                                p->palo ? p->palo->oid : -1, p->t);
                for (ASEGALISTENER* pl : p->aplistener) apsmaResync.push_back(static_cast<SMA*>(pl));
                p->aplistener.clear();
                RetractAsega(p);
                ++fs.casegaExtra;
            }
        }
        // the SMAs whose state the game's other code changed (SetSmaGoal 0x1F0660 / SeekSma / SetSmaState from
        // scripts and actors): their state from B, linked to B's transition
        for (const auto& [ps, psma] : rr.mpasma)
        {
            if (psma->fRetracted) continue;
            if (ramB.S32(ps + 0x34) == psma->ismsCur && ramB.S32(ps + 0x38) == psma->ismsNext &&
                ramB.S32(ps + 0x3C) == psma->ismsGoal && ramB.F32(ps + 0x44) == psma->svtLocal)
                continue;
            if (std::find(apsmaResync.begin(), apsmaResync.end(), psma) == apsmaResync.end()) apsmaResync.push_back(psma);
        }
        for (SMA* psma : apsmaResync)
        {
            if (psma->pasega)
            {
                auto& al = psma->pasega->aplistener;
                al.erase(std::remove(al.begin(), al.end(), psma), al.end());
            }
            psma->pasega = nullptr;
            for (const auto& [ps, p2] : rr.mpasma)
                if (p2 == psma && !psma->fRetracted)
                {
                    RetractSma(psma);
                    PsmaImport(rr, ramB, ps);
                    break;
                }
        }
    }
    return fs;
}
// Every blend of the reference built natively (NewBlend from its BLs) and compared; the gait pairs (2 BLs of 0x40
// bytes, 2 tables: 0x20B868) also bound natively by BindGait from their two ASEGs alone and compared: the feet's
// tables, the root velocities, the channels' tables, the length.
void CheckBlends(REFRUN& rr, const RAM& ram)
{
    int cblend = 0, cbuilt = 0, cgait = 0, cgaitOk = 0;
    for (uint32_t pab = ram.U32(SW_ADDR + 0x3038 + 0x6E * 12), c = 0; ram.FValid(pab) && c < 100000; pab = ram.U32(pab + 0x20), ++c)
    {
        ++cblend;
        ASEGBL* pbl = PblImport(rr, ram, pab);
        if (!pbl) continue;
        ++cbuilt;
        ALO* paloOwner = static_cast<ALO*>(rr.map.mpaplo[ram.U32(pab + 0x10)]);
        const uint32_t abBl = ram.U32(pab + 0xC8);
        if (ram.S32(pab + 0xC0) == 0x20 && ram.S32(pab + 0xC4) == 8)
        {
            // 0x1A0300's entries {?, ASEG, .., the root displacement per second +0x10}
            ROOTBLEND rb;
            for (int i = 0; i < 8; ++i)
                if (ram.U32(abBl + 0x20 * i + 4)) rb.apaseg[i] = static_cast<ASEG*>(rr.map.mpaplo[ram.U32(abBl + 0x20 * i + 4)]);
            BindRootBlend(&rb, paloOwner);
            float d = 0.0f;
            for (int i = 0; i < 8; ++i)
                for (int l = 0; l < 3; ++l)
                    if (rb.apaseg[i]) d = std::max(d, std::fabs(ram.F32(abBl + 0x20 * i + 0x10 + 4 * l) - rb.avel[i][l]));
            ++cgait;
            cgaitOk += d < 1e-4f && rb.pbl && rb.pbl->TMax() == ram.F32(pab + 0x74);
            std::printf("  root blend of alo %d (+0x%X): displacement diff %.3g\n", paloOwner->oid, abBl - ram.U32(pab + 0x10), d);
        }
        if (ram.S32(pab + 0xC0) == 0x10 && ram.S32(pab + 0xC4) == 2)
        {
            SLOPEBLEND sb;
            for (int i = 0; i < 2; ++i)
                if (ram.U32(abBl + 0x10 * i + 4)) sb.apaseg[i] = static_cast<ASEG*>(rr.map.mpaplo[ram.U32(abBl + 0x10 * i + 4)]);
            BindSlopeBlend(&sb, paloOwner);
            float d = 0.0f;
            for (int i = 0; i < 2; ++i)
                if (sb.apaseg[i]) d = std::max(d, std::fabs(ram.F32(abBl + 0x10 * i + 0xC) - sb.agSlope[i]));
            ++cgait;
            cgaitOk += d < 1e-4f && sb.pbl && sb.pbl->TMax() == ram.F32(pab + 0x74);
            std::printf("  slope blend of alo %d (+0x%X): slope diff %.3g\n", paloOwner->oid, abBl - ram.U32(pab + 0x10), d);
        }
        if (!(ram.S32(pab + 0xC0) == 0x40 && ram.S32(pab + 0xC4) == 2 && ram.S32(pab + 0xCC) == 2)) continue;
        ++cgait;
        const uint32_t ab = ram.U32(pab + 0xC8), atable = ram.U32(pab + 0xD0);
        GAIT gait;
        InitGait(&gait, static_cast<ALO*>(rr.map.mpaplo[ram.U32(pab + 0x10)]), -1);
        for (int i = 0; i < 2; ++i)
            gait.apaseg[i] = ram.U32(ab + 0x40 * i + 4) ? static_cast<ASEG*>(rr.map.mpaplo[ram.U32(ab + 0x40 * i + 4)]) : nullptr;
        BindGait(&gait);
        if (!gait.pbl) continue;
        // (the reference's weights now, for the derived tables)
        FImportBlendWeights(ram, pab, gait.pbl);
        RecalcBlend(gait.pbl);
        float dVel = 0, dBl = 0, dTable = 0;
        for (int i = 0; i < 2; ++i)
        {
            if (!gait.apaseg[i]) continue;
            const glm::vec4 v = ram.Vec4(ab + 0x40 * i + 0x10);
            for (int l = 0; l < 3; ++l) dVel = std::max(dVel, std::fabs(v[l] - gait.avel[i][l]));
            for (int t = 0; t < 2; ++t)
                for (int k = 0; k < 2; ++k)
                {
                    const uint32_t pm = ab + 0x40 * i + 0x20 + 0x10 * t + 8 * k;
                    const MRSG& ms = gait.pbl->abl[i].aamrsg[t][k];
                    dBl = std::max({dBl, std::fabs(ram.F32(pm) - ms.t), std::fabs(ram.F32(pm + 4) - ms.dt)});
                }
        }
        for (int t = 0; t < 2; ++t)
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t pm = ram.U32(atable + 12 * t + 8) + 8 * k;
                dTable = std::max({dTable, std::fabs(ram.F32(pm) - gait.pbl->aamrsg[t][k].t), std::fabs(ram.F32(pm + 4) - gait.pbl->aamrsg[t][k].dt)});
            }
        // the channels' tables
        int cchnDiff = 0;
        const int cchn = ram.S32(pab + 0x5C);
        const uint32_t achn = ram.U32(pab + 0x60);
        for (int j = 0; j < cchn; ++j)
        {
            const uint32_t w = ram.U32(achn + 8 * j), ptrk = ram.U32(achn + 8 * j + 4);
            const int oid = static_cast<int32_t>(w << 12) >> 12;
            const int isub = static_cast<int32_t>((w >> 20) << 22) >> 22;
            const uint32_t pt = ram.U32(ptrk + 8);
            const int itable = pt ? static_cast<int>((pt - atable) / 12) : -1;
            bool fFound = false;
            for (ASEGCHN& chn : gait.pbl->asegd.achn)
                if (chn.oid == oid && chn.isub == isub)
                {
                    fFound = true;
                    int itNative = -2;
                    if (auto* p = dynamic_cast<TRKPBL*>(chn.ptrkp.get())) itNative = p->itable;
                    if (auto* p = dynamic_cast<TRKGBL*>(chn.ptrkg.get())) itNative = p->itable;
                    cchnDiff += itNative != itable;
                }
            cchnDiff += !fFound;
        }
        const bool fOk = dVel < 1e-4f && dBl < 1e-5f && dTable < 1e-5f && cchnDiff == 0 &&
                         std::fabs(gait.pbl->TMax() - ram.F32(pab + 0x74)) < 1e-5f &&
                         gait.pbl->asegd.achn.size() == static_cast<size_t>(cchn);
        cgaitOk += fOk;
        std::printf("  gait of alo %d (+0x%X): velocity diff %.3g, BL tables %.3g, blended tables %.3g, channels %zu/%d "
                    "(%d tables differ), length %.5f/%.5f%s\n",
                    gait.palo->oid, ab - 0x10 - ram.U32(pab + 0x10), dVel, dBl, dTable, gait.pbl->asegd.achn.size(), cchn,
                    cchnDiff, gait.pbl->TMax(), ram.F32(pab + 0x74), fOk ? "" : "  MISMATCH");
    }
    // the stepping guards' (cid 0x38) step lengths of their segments 1 and 4 (0x1B1A38: +0x1CC / +0x1D0)
    int cstep = 0, cstepOk = 0;
    for (uint32_t pa = ram.U32(SW_ADDR + 0x3038 + 0x38 * 12), c = 0; ram.FValid(pa) && c < 100000; pa = ram.U32(pa + 0x20), ++c)
    {
        auto it = rr.map.mpaplo.find(pa);
        if (it == rr.map.mpaplo.end()) continue;
        for (int k = 0; k < 2; ++k)
        {
            const uint32_t aseg = ram.U32(pa + 0x17C + 0xC * (k ? 4 : 1) + 4);
            auto itSeg = rr.map.mpaplo.find(aseg);
            if (!aseg || itSeg == rr.map.mpaplo.end()) continue;
            float g = -1.0f;
            if (!FStepLength(static_cast<ASEG*>(itSeg->second), static_cast<ALO*>(it->second), &g)) continue;
            ++cstep;
            const float gRef = ram.F32(pa + (k ? 0x1D0 : 0x1CC));
            cstepOk += std::fabs(g - gRef) <= 1e-4f * std::max(1.0f, std::fabs(gRef));
            if (rr.fVerbose) std::printf("  guard %d step length %d: ref %.5f native %.5f\n", it->second->oid, k, gRef, g);
        }
    }
    std::printf("guards' step lengths: %d/%d equal\n", cstepOk, cstep);
    std::printf("blends: %d in the reference, %d built natively; gait pairs / root / slope blends bound natively %d/%d equal\n", cblend, cbuilt,
                cgaitOk, cgait);
}
}  // namespace

// --anim-ref W A0.ram A1.ram [A2.ram ...] [-v]: import A0, then one frame per following snapshot
int AnimRefCommand(const char* pszWorld, const char* pszA, const char* pszB, int argc, char** argv)
{
    REFRUN rr;
    bool fBlends = false, fAi = false, fAiCarry = false;
    std::vector<std::string> aram = {pszA, pszB};
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "-v")) rr.fVerbose = true;
        else if (!std::strcmp(argv[i], "--blends")) fBlends = true;
        else if (!std::strcmp(argv[i], "--ai")) fAi = true;
        else if (!std::strcmp(argv[i], "--ai-carry")) fAi = fAiCarry = true;
        else if (!std::strcmp(argv[i], "--then") && i + 1 < argc) aram.push_back(argv[++i]);
    }
    InitClasses();
    FK fk;
    int filter = 0;
    std::string err;
    if (!FResolveWorld(pszWorld, fk, filter, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) return 1;
    rr.psw = psw.get();
    if (fAi)
    {
        // (the AI's sight lines need the physics world's attachments: the world started as the game does)
        StartSw(psw.get());
        PostLoadSwPhys(psw.get());
        RefreshSwPhysWorldNow(psw.get());
        // (the attachments active as if the activation had run: those of objects in the world)
        g_physw.apoxaActive.clear();
        for (OXA* poxa : g_physw.apoxa)
            if (poxa->pso && poxa->pso->fInWorld) g_physw.apoxaActive.push_back(poxa);
    }
    else
        InitWorldAnimation(psw.get());  // (bindings, ASEG / SM tables; what it starts is dropped)
    for (ASEGA* p : std::vector<ASEGA*>(g_apasega)) RetractAsega(p);
    for (SMA* p : std::vector<SMA*>(g_apsma)) RetractSma(p);
    RAM ram0, ram1;
    if (!ram0.FLoad(aram[0])) { std::fprintf(stderr, "cannot read %s\n", aram[0].c_str()); return 2; }
    MapObjects(psw.get(), ram0, rr.map);
    RemapSms(psw.get(), ram0, rr.map);
    std::printf("objects mapped %d, unmapped (native only) %d\n", rr.map.cmapped, rr.map.cunmapped);
    ImportState(rr, ram0);
    if (fBlends) CheckBlends(rr, ram0);
    AIREF airef;
    AISTATS aisum;
    if (fAi)
    {
        airef.pmap = &rr.map;
        airef.pmpaasega = &rr.mpaasega;
        airef.pmpasma = &rr.mpasma;
        airef.fVerbose = rr.fVerbose;
        airef.fCarry = fAiCarry;
        s_paiRef = &airef;
    }
    FRAMESTATS sum;
    for (size_t k = 1; k < aram.size(); ++k)
    {
        if (!ram1.FLoad(aram[k])) { std::fprintf(stderr, "cannot read %s\n", aram[k].c_str()); return 2; }
        const FRAMESTATS fs = StepAndCompare(rr, ram0, ram1);
        if (s_paiRef)
        {
            const AISTATS as = AiCompare(ram1, s_paiRef);
            if (rr.fVerbose)
                for (const std::string& m : as.amsg) std::printf("%s\n", m.c_str());
            std::printf("ai frame %zu: guards %d/%d equal, jobs %d/%d, fields %d/%d\n", k, as.cguardOk, as.cguard, as.cjobOk, as.cjob,
                        as.cfieldOk, as.cfield);
            aisum.cguard += as.cguard; aisum.cguardOk += as.cguardOk; aisum.cjob += as.cjob; aisum.cjobOk += as.cjobOk;
            aisum.cfield += as.cfield; aisum.cfieldOk += as.cfieldOk;
        }
        std::printf("frame %zu: records %d/%d equal (%d within 1e-3; max difference %.3g), ASEGAs %d/%d (new %d/%d), SMAs %d/%d; taken "
                    "over: %d ASEGAs started, %d ended, %d SMAs started by other code; %d native ASEGAs not in the "
                    "reference; late-pass flags %d/%d\n",
                    k, fs.crecOk, fs.crec, fs.crecOk3, fs.dMax, fs.casegaOk, fs.casega, fs.casegaNewOk, fs.casegaNew, fs.csmaOk, fs.csma,
                    fs.casegaExternal, fs.casegaExternalEnd, fs.csmaExternal, fs.casegaExtra, fs.cflagOk, fs.cflag);
        sum.crec += fs.crec; sum.crecOk += fs.crecOk; sum.crecOk3 += fs.crecOk3; sum.casega += fs.casega; sum.casegaOk += fs.casegaOk;
        sum.csma += fs.csma; sum.csmaOk += fs.csmaOk; sum.dMax = std::max(sum.dMax, fs.dMax);
        std::swap(ram0, ram1);
    }
    std::printf("total over %zu frames: records %d/%d equal within 1e-4 (%d within 1e-3), ASEGAs %d/%d, SMAs %d/%d, max "
                "matrix diff %.3g\n",
                aram.size() - 1, sum.crecOk, sum.crec, sum.crecOk3, sum.casegaOk, sum.casega, sum.csmaOk, sum.csma, sum.dMax);
    if (s_paiRef)
        std::printf("ai total: guards %d/%d equal, jobs %d/%d, fields %d/%d\n", aisum.cguardOk, aisum.cguard, aisum.cjobOk,
                    aisum.cjob, aisum.cfieldOk, aisum.cfield);
    s_paiRef = nullptr;
    return 0;
}

// --anim-pose W RAM [--screenshot out.png] [--size WxH] [-v]: the animation state of a reference / PCSX2 RAM
// snapshot (the objects' placement, the playing ASEGAs at their times, the SMAs) imported into native world W,
// then every record an ASEGA drives evaluated natively (its segment fetched again from the native tracks at the
// ASEGA's time, the joints solved) and compared with the snapshot's records; the frame drawn with skinning from
// the snapshot's camera and lights.
#include "gl.h"
#include "render.h"
#include "scene.h"
#include "scenesw.h"
#include "shd.h"

#include <cfloat>
#include <cstdlib>

namespace
{
CAMERA CameraFromRamPose(const RAM& ram, RENDEROPTS& opts)
{
    CAMERA cam;
    const uint32_t cm = 0x2DDF40;
    float m[16];
    for (int i = 0; i < 16; ++i) m[i] = ram.F32(cm + 0x60 + 4 * i);
    const float ty = ram.F32(cm + 0x44), sNear = ram.F32(cm + 0x4C);
    cam.rMRDAdjust = ram.F32(cm + 0x1C);
    cam.pos = glm::vec3(m[12], m[13], m[14]);
    cam.yaw = glm::degrees(std::atan2(m[1], m[0]));
    cam.pitch = glm::degrees(std::asin(std::clamp(m[2], -1.0f, 1.0f)));
    if (ty > 0.01f && ty < 10.0f) cam.fovY = glm::degrees(2.0f * std::atan(ty));
    if (sNear > 0.1f && sNear < 1000.0f) cam.sNear = sNear;
    opts.rgbaCel = ram.U32(0x2DEB50);
    opts.sFogNear = ram.F32(cm + 0x240);
    opts.sFogFar = ram.F32(cm + 0x244);
    opts.uFogMax = ram.F32(cm + 0x248);
    opts.rgbaFog = ram.U32(0x2DE18C);
    opts.fFog = opts.sFogNear > 0 && opts.sFogFar > opts.sFogNear;
    return cam;
}
}  // namespace

int AnimPoseCommand(const char* pszWorld, const char* pszRam, int argc, char** argv)
{
    REFRUN rr;
    std::string screenshot;
    int width = 1280, height = 960;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "-v")) rr.fVerbose = true;
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &width, &height);
    }
    RAM ram;
    if (!ram.FLoad(pszRam)) { std::fprintf(stderr, "cannot read %s\n", pszRam); return 2; }
    InitClasses();
    FK fk;
    int filter = 0;
    std::string err;
    if (!FResolveWorld(pszWorld, fk, filter, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    g_nFilterCur = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) return 1;
    rr.psw = psw.get();
    InitWorldAnimation(psw.get());
    for (ASEGA* p : std::vector<ASEGA*>(g_apasega)) RetractAsega(p);
    for (SMA* p : std::vector<SMA*>(g_apsma)) RetractSma(p);
    MapObjects(psw.get(), ram, rr.map);
    RemapSms(psw.get(), ram, rr.map);
    std::printf("objects mapped %d, unmapped (native only) %d\n", rr.map.cmapped, rr.map.cunmapped);
    ImportState(rr, ram);
    // the records an ASEGA drives: their segments fetched again from the native tracks at the ASEGA's time
    std::vector<ALO*> apalo;
    int crec = 0;
    for (const auto& [pa, pasega] : rr.mpaasega)
        for (CTRLASEG* pl = pasega->pctrlFirst; pl; pl = pl->pnextAsega)
        {
            if (!pl->FFirst() || !pl->ptrkp) continue;
            pl->pxf->g0 = pl->pxf->g1 = -FLT_MAX;
            pl->pxf->state = 0;
            ++crec;
            if (std::find(apalo.begin(), apalo.end(), pl->palo) == apalo.end()) apalo.push_back(pl->palo);
        }
    g_clock.t = ram.F32(0x2DDED0);
    g_clock.dt = 1e-6f;  // (the seeking records stay where the snapshot has them)
    for (ALO* palo : apalo) UpdateAloXfs(palo, g_clock.t, g_clock.dt);
    for (const auto& plo : psw->aploAll)
        if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), 0x30)) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
    // compared with the snapshot's records
    int cOk = 0, cCmp = 0;
    float dMax = 0;
    for (ALO* palo : apalo)
    {
        const uint32_t aalo = rr.map.mpploa[palo];
        const uint32_t base = ram.U32(aalo + 0x54);
        for (size_t i = 0; i < palo->axf.size(); ++i)
        {
            const XF* pxf = palo->axf[i].get();
            if (!pxf->pctrlFirst || pxf->pctrlFirst->kind != 0) continue;
            ++cCmp;
            const glm::mat4 mR = ram.Mat4(base + 0x100 * static_cast<uint32_t>(i));
            float dRot = 0, dPos = 0;
            for (int r = 0; r < 3; ++r)
                for (int j = 0; j < 3; ++j) dRot = std::max(dRot, std::fabs(mR[r][j] - pxf->mat[r][j]));
            const float sPos = std::max(1.0f, std::fabs(mR[3][0]) + std::fabs(mR[3][1]) + std::fabs(mR[3][2]));
            for (int j = 0; j < 3; ++j) dPos = std::max(dPos, std::fabs(mR[3][j] - pxf->mat[3][j]) / sPos);
            const float d = std::max(dRot, dPos);
            dMax = std::max(dMax, d);
            if (d < 1e-3f) ++cOk;
            else if (rr.fVerbose)
                std::printf("  alo %d (cid 0x%02X) xf %zu kind %d mode %d/%d: native vs snapshot %.4g\n", palo->oid, palo->Cid(),
                            i, pxf->kind, pxf->mode, pxf->state, d);
        }
    }
    std::printf("records driven by ASEGAs: %d evaluated natively, %d of %d within 1e-3 of the snapshot (max %.3g)\n",
                crec, cOk, cCmp, dMax);
    if (screenshot.empty()) return 0;
    // the frame: the scene from the object model (skinned), the snapshot's camera and lights
    SCENE scene;
    std::vector<ALO*> apaloScene;
    CBuildSceneFromSw(psw.get(), scene, &apaloScene);
    // what the snapshot draws (RenderSw 0x1CDA80: update-table entries with flag 8, not 0x80; ALO vt slot 19
    // 0x1F3C70: its byte +0x2D against the mask 0x2DDF54), until the object model's visibility exists
    {
        std::unordered_map<uint32_t, uint32_t> mpagrf;
        const int c = ram.S32(SW_ADDR + 0x3F14);
        const uint32_t ae = ram.U32(SW_ADDR + 0x3F20);
        for (int i = 0; i < c && i < 100000; ++i) mpagrf[ram.U32(ae + 8 * i + 4)] = ram.U32(ae + 8 * i);
        const uint32_t mask = ram.U32(0x2DDF54);
        int chidden = 0;
        for (size_t i = 0; i < scene.aglobset.size() && i < apaloScene.size(); ++i)
        {
            auto it = rr.map.mpploa.find(apaloScene[i]);
            if (it == rr.map.mpploa.end()) continue;
            auto itE = mpagrf.find(it->second);
            // (objects outside the table or without flag 8 may be drawn by other paths (their parents): kept;
            // flag 0x80 or the +0x2D mask hide)
            static const int s_kVis = std::getenv("SLY2_POSE_VIS") ? std::atoi(std::getenv("SLY2_POSE_VIS")) : 0;
            bool fDrawn = true;
            if (s_kVis == 1)  // only what RenderSw draws itself
                fDrawn = itE != mpagrf.end() && (itE->second & 0x88) == 8 &&
                         (static_cast<uint32_t>(ram.ab[it->second + 0x2D]) & mask) != 0;
            else if (s_kVis == 2)  // the +0x2D mask only
                fDrawn = (static_cast<uint32_t>(ram.ab[it->second + 0x2D]) & mask) != 0;
            if (fDrawn) continue;
            std::fill(scene.aglobset[i].afVisible.begin(), scene.aglobset[i].afVisible.end(), 0);
            ++chidden;
        }
        std::printf("objects the snapshot does not draw: %d of %zu\n", chidden, scene.aglobset.size());
    }
    LIGHTSET lights;
    if (FLightsFromRam(pszRam, lights)) ApplyLighting(scene, lights);
    else ApproximateBakeLighting(scene);
    RENDEROPTS opts;
    const CAMERA cam = CameraFromRamPose(ram, opts);
    if (!g_gl.FInit(width, height, false, "Sly2-Release", err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    BuildSceneGpu(scene);
    UpdateShaderAnimations(g_clock.t, 1.0f / 60.0f);
    RenderScene(scene, cam, opts);
    glFinish();
    const bool fOk = g_gl.FWriteScreenshot(screenshot);
    std::printf("%s %s\n", fOk ? "wrote" : "FAILED to write", screenshot.c_str());
    g_gl.Terminate();
    return fOk ? 0 : 1;
}

// --anim-run W f.0.ram f.1.ram [--then ...] [-v]: the native game from the world's load, nothing imported (the
// rand state and the clock per frame only), against the reference's frames from its load (SLY2_DUMP_SEQ_AFTER=
// 1ED448:3 SLY2_DUMP_SEQ=226550:1:N:f: each world update's entry): after native frame k, the reference's f.(k+1):
// every playing ASEGA (by ASEG and object: time and event index), every SMA (states), every record a native ASEGA
// drives in an object the reference updated in that frame.
#include "update.h"
#include "activate.h"

int AnimRunCommand(const char* pszWorld, const char* pszA, const char* pszB, int argc, char** argv)
{
    REFRUN rr;
    std::vector<std::string> aram = {pszA, pszB};
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "-v")) rr.fVerbose = true;
        else if (!std::strcmp(argv[i], "--then") && i + 1 < argc) aram.push_back(argv[++i]);
    }
    InitClasses();
    FK fk;
    int filter = 0;
    std::string err;
    if (!FResolveWorld(pszWorld, fk, filter, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) return 1;
    rr.psw = psw.get();
    StartSw(psw.get());  // (with the animation's start: anim.h)
    PostLoadSwPhys(psw.get());   // (the rest of the transition's start: physics, the players)
    InitWorldActors(psw.get());
    InitWorldGang(psw.get());
    RAM ram0, ram1;
    if (!ram0.FLoad(aram[0])) return 2;
    MapObjects(psw.get(), ram0, rr.map);
    RemapSms(psw.get(), ram0, rr.map);
    int cT = 0, cTOk = 0, cS = 0, cSOk = 0, cR = 0, cROk = 0, cMissing = 0;
    for (size_t k = 1; k < aram.size(); ++k)
    {
        if (!ram1.FLoad(aram[k])) return 2;
        g_uRandState = ram0.U32(ram0.U32(0x2C49D4) + 0x58);
        g_clock.t = ram0.F32(0x2DDED0);
        g_clock.dt = ram0.F32(0x2DDED4);
        g_clock.dt40 = ram0.F32(0x2DDF10);
        // the activation's viewpoint (activate.h): the reference camera's CM +0x90 / +0x14 at the frame's start
        g_actview.posCm = glm::vec3(ram0.F32(0x2DDF40 + 0x90), ram0.F32(0x2DDF40 + 0x94), ram0.F32(0x2DDF40 + 0x98));
        g_actview.grfCm = ram0.U32(0x2DDF40 + 0x14);
        UpdateSw(psw.get());
        for (const auto& plo : psw->aploAll)
            if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), 0x30)) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
        // the ASEGAs: each of the reference's matched by ASEG and object among the native ones
        int ct = 0, ctOk = 0, cmiss = 0;
        std::vector<ASEGA*> apasegaUsed;
        for (uint32_t pa : ram1.Dl(0x3E1218))
        {
            ++ct;
            auto itAseg = rr.map.mpaplo.find(ram1.U32(pa + 0x14));
            const uint32_t aalo = ram1.U32(pa + 0x18);
            LO* palo = aalo && rr.map.mpaplo.count(aalo) ? rr.map.mpaplo[aalo] : nullptr;
            ASEGA* pasega = nullptr;
            float dBest = 1e30f;
            for (ASEGA* p : g_apasega)
            {
                if (itAseg == rr.map.mpaplo.end() || p->paseg != itAseg->second || p->palo != palo) continue;
                if (std::find(apasegaUsed.begin(), apasegaUsed.end(), p) != apasegaUsed.end()) continue;
                const float d = std::fabs(p->t - ram1.F32(pa + 0x1C));
                if (d < dBest) { dBest = d; pasega = p; }
            }
            if (!pasega)
            {
                ++cmiss;
                if (rr.fVerbose)
                    std::printf("  frame %zu: no native asega of aseg %d alo %d (the reference's t %.4f)\n", k,
                                itAseg != rr.map.mpaplo.end() ? itAseg->second->oid : -1, palo ? palo->oid : -1,
                                ram1.F32(pa + 0x1C));
                continue;
            }
            apasegaUsed.push_back(pasega);
            if (dBest < 1e-4f && ram1.S32(pa + 0x50) == pasega->iea) ++ctOk;
            else if (rr.fVerbose)
                std::printf("  frame %zu: asega of aseg %d alo %d: t ref %.4f native %.4f iea %d / %d\n", k, pasega->paseg->oid,
                            palo ? palo->oid : -1, ram1.F32(pa + 0x1C), pasega->t, ram1.S32(pa + 0x50), pasega->iea);
        }
        // the SMAs (by SM and object)
        int cs = 0, csOk = 0;
        std::vector<SMA*> apsmaUsed;
        for (uint32_t ps : ram1.Dl(0x3E126C))
        {
            auto itSm = rr.map.mpaplo.find(ram1.U32(ps + 0x24));
            const uint32_t aalo = ram1.U32(ps + 0x28);
            LO* palo = aalo && rr.map.mpaplo.count(aalo) ? rr.map.mpaplo[aalo] : nullptr;
            SMA* psma = nullptr;
            for (SMA* p : g_apsma)
                if (itSm != rr.map.mpaplo.end() && p->psm == itSm->second && p->palo == palo &&
                    std::find(apsmaUsed.begin(), apsmaUsed.end(), p) == apsmaUsed.end() && p->ismsCur == ram1.S32(ps + 0x34))
                {
                    psma = p;
                    break;
                }
            if (!psma) continue;  // (an SMA the native game does not run: actors' code)
            apsmaUsed.push_back(psma);
            ++cs;
            if (ram1.S32(ps + 0x38) == psma->ismsNext && ram1.S32(ps + 0x3C) == psma->ismsGoal) ++csOk;
        }
        // the records of native ASEGAs in objects the reference updated
        int cr = 0, crOk = 0;
        const std::vector<ALO*> apaloUpd = ApaloUpdated(rr, ram0, ram1);
        for (ALO* palo : apaloUpd)
        {
            // (the players' trees are the actors' code: their own harnesses)
            if (FCidDerivedFrom(PloRoot(palo)->Cid(), 0x34)) continue;
            const uint32_t base = ram1.U32(rr.map.mpploa[palo] + 0x54);
            for (size_t i = 0; i < palo->axf.size(); ++i)
            {
                const XF* pxf = palo->axf[i].get();
                if (!pxf->pctrlFirst || pxf->pctrlFirst->kind != 0) continue;
                ++cr;
                const glm::mat4 mB = ram1.Mat4(base + 0x100 * static_cast<uint32_t>(i));
                float d = 0;
                for (int r = 0; r < 3; ++r)
                    for (int j = 0; j < 3; ++j) d = std::max(d, std::fabs(mB[r][j] - pxf->mat[r][j]));
                const float sPos = std::max(1.0f, std::fabs(mB[3][0]) + std::fabs(mB[3][1]) + std::fabs(mB[3][2]));
                for (int j = 0; j < 3; ++j) d = std::max(d, std::fabs(mB[3][j] - pxf->mat[3][j]) / sPos);
                if (d < 1e-3f) ++crOk;
                else if (rr.fVerbose)
                    std::printf("  frame %zu: alo %d xf %zu: %.4g\n", k, palo->oid, i, d);
            }
        }
        std::printf("frame %zu: ASEGAs %d/%d (%d of the reference's not native: actors / scripts), SMAs %d/%d, records %d/%d\n",
                    k, ctOk, ct, cmiss, csOk, cs, crOk, cr);
        cT += ct; cTOk += ctOk; cS += cs; cSOk += csOk; cR += cr; cROk += crOk; cMissing += cmiss;
        std::swap(ram0, ram1);
    }
    std::printf("total over %zu frames from the load: ASEGAs %d/%d (%d not native), SMAs %d/%d, records %d/%d within 1e-3\n",
                aram.size() - 1, cTOk, cT, cMissing, cSOk, cS, cROk, cR);
    return 0;
}

#include "guard.h"
#include "detect.h"
// SLY2N_POSECHECK=ram[:n] (game.cpp, after the loop): the native world's ALO records' world positions compared with
// a reference / PCSX2 snapshot of the same world (objects mapped by class, oid and creation order); the n largest
// differences printed
void PoseCheck(SW* psw, const char* pszSpec)
{
    std::string path = pszSpec;
    int cprint = 40;
    if (size_t i = path.rfind(':'); i != std::string::npos && i > 1) { cprint = std::atoi(path.c_str() + i + 1); path.resize(i); }
    RAM ram;
    if (!ram.FLoad(path)) { std::fprintf(stderr, "posecheck: cannot read %s\n", path.c_str()); return; }
    MAPPING map;
    MapObjects(psw, ram, map);
    const char* s_poids = std::getenv("SLY2N_POSEOIDS");  // ",oid,oid,"
    if (const char* pszInfo = std::getenv("SLY2N_OIDINFO"))  // ",oid,": the objects' class, parent, membership
        for (const auto& [a, plo] : map.mpaplo)
            if (std::strstr(pszInfo, (std::string(",") + std::to_string(plo->oid) + ",").c_str()))
            {
                std::printf("  oid %d at 0x%X: cid 0x%02X (%s) parent %d instance %d, %s natively, %s in the snapshot, %zu owned\n", plo->oid, a, plo->Cid(),
                            g_acls[plo->Cid()].name, plo->paloParent ? plo->paloParent->oid : -1, plo->oidInstance, plo->fInWorld ? "in" : "out",
                            ((ram.U32(a + 0x2C) >> 28) & 1) ? "in" : "out", plo->aploOwned.size());
                if (std::getenv("SLY2N_OIDOPTS") && (std::printf("    options:"), true))
                {
                    for (const OPT& o : plo->aopt)
                    {
                        std::printf(" %d(", o.eopid);
                        for (const OPTVAL& v : o.aval)
                        {
                            std::printf("[k%d t%d n%d g%g", v.k, v.tagk, v.n, v.ag[0]);
                            for (const OPTVAL& w : v.alist) std::printf(" %d", w.n);
                            std::printf("]");
                        }
                        std::printf(")");
                    }
                    std::printf("\n    fields:");
                    for (const auto& [k, u] : plo->afield) std::printf(" %llx:%x=%x", (unsigned long long)(k >> 32), unsigned(k), u);
                    std::printf("\n");
                }
            }
    // SLY2N_GUARDCHECK=1: the guards' start (guardstart.cpp) against the snapshot's: their kind tables and arrays, fields
    if (std::getenv("SLY2N_GUARDCHECK"))
        for (const auto& [a, plo] : map.mpaplo)
        {
            GUARD* pg = FCidDerivedFrom(plo->Cid(), 0x38) ? PguardFromLo(plo) : nullptr;
            if (!pg) continue;
            int cOk = 0, cDiff = 0;
            auto oidAt = [&](uint32_t p) { return ram.FValid(p) ? ram.S32(p + 0x18) : -1; };
            auto cmpe = [&](const KINDE& e, uint32_t ae, const char* psz, int i) {
                const int k = ram.S32(ae);
                const int grf = ram.S32(ae + 0xC);
                const int oidR = (k == 0 || k == 1) ? oidAt(ram.U32(ae + 4)) : -1;
                const int oidN = (e.k == 0 || e.k == 1) && e.plo ? e.plo->oid : -1;
                if (k == e.k && grf == e.grf && oidR == oidN) { ++cOk; return; }
                ++cDiff;
                std::printf("    guard %d %s %d: ref k %d grf %d oid %d native k %d grf %d oid %d\n", plo->oid, psz, i, k, grf, oidR, e.k,
                            e.grf, oidN);
            };
            const int c = ram.S32(a + 0x4C0);
            for (int i = 0; i < c && i < 32; ++i)
            {
                const uint32_t ae = a + 0x4D0 + 32 * static_cast<uint32_t>(i);
                const uint64_t m = static_cast<uint64_t>(ram.U32(ae + 0x10)) | (static_cast<uint64_t>(ram.U32(ae + 0x14)) << 32);
                const KINDE* pe = nullptr;
                for (const KINDE& e : pg->akinde)
                    if (e.mask == m) pe = &e;
                if (!pe) { ++cDiff; std::printf("    guard %d table: no native entry for mask 0x%llx\n", plo->oid, (unsigned long long)m); continue; }
                cmpe(*pe, ae, "table", i);
            }
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 26; ++i)
                    cmpe(pg->aakinde[j][i], a + (j == 0 ? 0x8D0u : j == 1 ? 0xA70u : 0xC10u) + 16 * static_cast<uint32_t>(i), j == 0 ? "+0x8D0" : j == 1 ? "+0xA70" : "+0xC10", i);
            const uint32_t ad = ram.U32(a + 0xF64);
            std::printf("  guard %d: table %d (native %zu), entries %d equal, %d differ; detector volume ref %d native %d; +0x338 %d/%d "
                        "+0x314 %.0f/%.0f\n", plo->oid, c, pg->akinde.size(), cOk, cDiff, oidAt(ram.U32(ad + 0xE8)),
                        pg->pdet && pg->pdet->ploVolume ? pg->pdet->ploVolume->oid : -1, ram.S32(a + 0x338), pg->n338, ram.F32(a + 0x314), pg->g314);
        }
    // SLY2N_MODECHECK=1: every mapped ALO's records whose mode (+0x98) differs from the snapshot's
    if (std::getenv("SLY2N_MODECHECK"))
        for (const auto& [a, plo] : map.mpaplo)
        {
            if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            const ALO* pa = static_cast<const ALO*>(plo);
            const uint32_t c = ram.U32(a + 0x50), ar = ram.U32(a + 0x54);
            if (c != pa->axf.size() || !ram.FValid(ar)) continue;
            for (uint32_t i = 0; i < c; ++i)
            {
                const int mode = ram.S16(ar + 0x100 * i + 0x98);
                if (mode != pa->axf[i]->mode)
                    std::printf("  modecheck %d (cid 0x%02X) xf %u kind %d: native mode %d ref %d (%s natively, %s in the snapshot)\n", plo->oid,
                                plo->Cid(), i, pa->axf[i]->kind, pa->axf[i]->mode, mode, plo->fInWorld ? "in" : "out",
                                ((ram.U32(a + 0x2C) >> 28) & 1) ? "in" : "out");
            }
        }
    if (const char* pszEv = std::getenv("SLY2N_EVOIDS"))  // ",oid,": the ASEGs whose events name one of them
        for (const auto& plo : psw->aploAll)
        {
            if (!FCidDerivedFrom(plo->Cid(), 0x6D)) continue;
            ASEG* paseg = static_cast<ASEG*>(plo.get());
            for (int l = 0; l < 3; ++l)
                for (const ASEGEV& e : PasegdOf(paseg)->aaev[l])
                    for (int k = 0; k < 4; ++k)
                        if (std::strstr(pszEv, (std::string(",") + std::to_string(e.an[k]) + ",").c_str()))
                            std::printf("  aseg %d (parent %d) list %d t %.3f kind %d an %d %d %d %d\n", paseg->oid,
                                        paseg->paloParent ? paseg->paloParent->oid : -1, l, e.t, e.kind, e.an[0], e.an[1],
                                        e.an[2], e.an[3]);
        }
    struct D { float d; ALO* palo; size_t i; glm::vec3 n, r; };
    std::vector<D> ad;
    int c = 0, cOk = 0, cMember = 0;
    for (const auto& [a, plo] : map.mpaplo)
    {
        const bool fInRef = ((ram.U32(a + 0x2C) >> 28) & 1) != 0;
        const bool fAlo = FCidDerivedFrom(plo->Cid(), 0x30);
        if (fInRef != plo->fInWorld)
        {
            ++cMember;
            if (cMember <= cprint)
            {
                const ALO* pa = fAlo ? static_cast<ALO*>(plo) : nullptr;
                const glm::vec3 n = !pa || pa->axf.empty() ? glm::vec3(0.0f) : glm::vec3(pa->axf[0]->matWorld[3]);
                std::printf("  %d (cid 0x%02X, parent %d, clone of %d) at (%.0f %.0f %.0f): %s natively, %s in the snapshot\n",
                            plo->oid, plo->Cid(), plo->paloParent ? plo->paloParent->oid : -1,
                            plo->ploCloneOf ? plo->ploCloneOf->oid : -1, n.x, n.y, n.z,
                            plo->fInWorld ? "in the world" : "removed", fInRef ? "in the world" : "removed");
            }
        }
        if (!fAlo) continue;
        ALO* palo = static_cast<ALO*>(plo);
        const uint32_t base = ram.U32(a + 0x54);
        if (s_poids && std::strstr(s_poids, (std::string(",") + std::to_string(palo->oid) + ",").c_str()) && ram.FValid(base))
            for (size_t i = 0; i < palo->axf.size(); ++i)
            {
                const glm::mat4 mR = ram.Mat4(base + 0x100 * static_cast<uint32_t>(i) + 0x40);
                const glm::mat4& mN = palo->axf[i]->matWorld;
                std::printf("  %d xf %zu (%s/%s): native", palo->oid, i, palo->fInWorld ? "in" : "out", fInRef ? "in" : "out");
                for (int k = 0; k < 4; ++k) std::printf(" (%.2f %.2f %.2f)", mN[k][0], mN[k][1], mN[k][2]);
                std::printf("\n      ref   ");
                for (int k = 0; k < 4; ++k) std::printf(" (%.2f %.2f %.2f)", mR[k][0], mR[k][1], mR[k][2]);
                const uint32_t ax = base + 0x100 * static_cast<uint32_t>(i);
                const glm::vec4 vR = ram.Vec4(ax + 0xB0);
                std::printf("\n      local native (%.1f %.1f %.1f) ref (%.1f %.1f %.1f); ref mode %d state %d vel (%.1f %.1f %.1f)\n",
                            palo->axf[i]->mat[3][0], palo->axf[i]->mat[3][1], palo->axf[i]->mat[3][2], ram.F32(ax + 0x30), ram.F32(ax + 0x34),
                            ram.F32(ax + 0x38), ram.S16(ax + 0x98), ram.S16(ax + 0x9A), vR.x, vR.y, vR.z);
            }
        if (!fInRef || !palo->fInWorld) continue;
        if (!ram.FValid(base)) continue;
        for (size_t i = 0; i < palo->axf.size(); ++i)
        {
            const glm::mat4 mR = ram.Mat4(base + 0x100 * static_cast<uint32_t>(i) + 0x40);
            const glm::mat4& mN = palo->axf[i]->matWorld;
            const glm::vec3 r = glm::vec3(mR[3]);
            const glm::vec3 n = glm::vec3(mN[3]);
            float dRot = 0.0f;
            for (int k = 0; k < 3; ++k) dRot = std::max(dRot, glm::length(glm::vec3(mR[k]) - glm::vec3(mN[k])));
            const float d = glm::length(r - n) + 100.0f * dRot;  // (a rotation difference of 0.01 counts as 1 unit)
            ++c;
            if (d < 1.0f) ++cOk;
            else ad.push_back(D{d, palo, i, n, r});
        }
    }
    std::sort(ad.begin(), ad.end(), [](const D& x, const D& y) { return x.d > y.d; });
    std::printf("posecheck: %d ALOs mapped (%d unmapped native), %d of %d first records within 1 unit; %d membership differences\n",
                map.cmapped, map.cunmapped, cOk, c, cMember);
    for (int i = 0; i < cprint && i < static_cast<int>(ad.size()); ++i)
    {
        const D& e = ad[i];
        std::printf("  %d (cid 0x%02X%s) xf %zu: native (%.1f %.1f %.1f) ref (%.1f %.1f %.1f) d %.1f\n", e.palo->oid,
                    e.palo->Cid(), e.palo->fInWorld ? "" : ", removed", e.i, e.n.x, e.n.y, e.n.z, e.r.x, e.r.y, e.r.z, e.d);
    }
}

// --ai-free W A.ram B.ram [--then C.ram ...] [-v]: the guards free natively. The native world W starts (StartSw), the
// reference's animation state of A and its guards' AI are imported once (the guards put in the world as the
// reference has them); then the native game runs frame after frame (UpdateSw: the guards' update 0x207350 with their
// jobs, the ASEGAs, the physics), with only the other objects' records (the players', the actors') and the regions'
// polygons taken from each snapshot; each frame the guards' positions, state masks and job stacks are compared
// with the snapshot.
#include "activate.h"
#include "guard.h"
#include "physworld.h"
#include "update.h"
// the frame index of a dump f.N.ram (its position in the list when the name has none)
int IFrameOfPath(const std::string& path)
{
    const size_t e = path.rfind(".ram");
    const size_t b = path.rfind('.', e == std::string::npos ? std::string::npos : e - 1);
    if (e == std::string::npos || b == std::string::npos) return 0;
    return std::atoi(path.c_str() + b + 1);
}
extern bool g_fSpawnersPaused;  // spawner.cpp
int AiFreeCommand(const char* pszWorld, const char* pszA, const char* pszB, int argc, char** argv)
{
    g_fSpawnersPaused = true;  // (their slots are not imported)
    REFRUN rr;
    std::vector<std::string> aram = {pszA, pszB};
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "-v")) rr.fVerbose = true;
        else if (!std::strcmp(argv[i], "--then") && i + 1 < argc) aram.push_back(argv[++i]);
    }
    InitClasses();
    FK fk;
    int filter = 0;
    std::string err;
    if (!FResolveWorld(pszWorld, fk, filter, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) return 1;
    rr.psw = psw.get();
    StartSw(psw.get());
    PostLoadSwPhys(psw.get());  // (as the transition does: the stepping objects' custom entries, the players)
    InitWorldActors(psw.get());
    InitWorldGang(psw.get());
    RefreshSwPhysWorldNow(psw.get());  // (the first frame's refresh before the import, not over it)
    for (ASEGA* p : std::vector<ASEGA*>(g_apasega)) RetractAsega(p);
    for (SMA* p : std::vector<SMA*>(g_apsma)) RetractSma(p);
    RAM ram0, ram1;
    if (!ram0.FLoad(aram[0])) { std::fprintf(stderr, "cannot read %s\n", aram[0].c_str()); return 2; }
    MapObjects(psw.get(), ram0, rr.map);
    RemapSms(psw.get(), ram0, rr.map);
    ImportState(rr, ram0);
    AIREF airef;
    airef.pmap = &rr.map;
    airef.pmpaasega = &rr.mpaasega;
    airef.pmpasma = &rr.mpasma;
    airef.fVerbose = rr.fVerbose;
    airef.fFree = true;
    AiImport(ram0, &airef);
    for (auto& [a, pg] : airef.aguard)
        if (pg->pdet) pg->pdet->fTouchRam = false;  // (their touches from the native overlaps from now on)
    // the guards (and what they own) in the world as in the reference; their trees are not imported again
    std::unordered_map<const LO*, bool> mpfGuardTree;
    auto fInGuardTree = [&](const LO* plo) {
        for (const LO* p = plo; p; p = p->paloParent)
            if (PguardFromLo(const_cast<LO*>(p))) return true;
        return false;
    };
    for (auto& [a, pg] : airef.aguard)
    {
        // (in the world as in the reference: its flag bit 60 (+0x2C bit 28); natively a spawner's or a proxy's copy may
        // not be placed yet)
        const bool fIn = pg->palo->fInWorld || ((ram0.U32(a + 0x2C) >> 28) & 1);
        if (std::getenv("SLY2N_ADDDEBUG") && !pg->palo->axf.empty())
        {
            for (int i = 1; i < 3 && i < static_cast<int>(pg->palo->axf.size()); ++i)
            {
                XF* x = pg->palo->axf[i].get();
                int iref = -1;
                for (int j = 0; j < static_cast<int>(pg->palo->axf.size()); ++j)
                    if (pg->palo->axf[j].get() == x->pxfRef) iref = j;
                std::printf("guard %d record %d: local %.0f %.0f %.0f world %.0f %.0f %.0f ref index %d; refs %p %p %p\n", pg->palo->oid, i, x->mat[3][0],
                            x->mat[3][1], x->mat[3][2], x->matWorld[3][0], x->matWorld[3][1], x->matWorld[3][2], iref, (void*)pg->palo->apxfRef[0],
                            (void*)pg->palo->apxfRef[1], (void*)pg->palo->apxfRef[2]);
            }
            XF* x0 = pg->palo->axf[0].get();
            std::printf("guard %d record 0: local %.0f %.0f %.0f world %.0f %.0f %.0f ref %p (owner %d)\n", pg->palo->oid, x0->mat[3][0], x0->mat[3][1],
                        x0->mat[3][2], x0->matWorld[3][0], x0->matWorld[3][1], x0->matWorld[3][2], (void*)x0->pxfRef,
                        pg->palo->paloParent ? pg->palo->paloParent->oid : -1);
        }
        if (std::getenv("SLY2N_ADDDEBUG"))
            std::printf("guard %d: native removed %d inworld %d parent %p clone %d root active %d st %d; ref inworld %d\n", pg->palo->oid,
                        pg->palo->fRemoved, pg->palo->fInWorld, (void*)pg->palo->paloParent, pg->palo->ploCloneOf != nullptr,
                        PloRoot(pg->palo)->fNodeActive, pg->palo->st, (ram0.U32(a + 0x2C) >> 28) & 1);
        if (fIn)
        {
            // (AddLo only adds a removed object: one neither removed nor in the world is marked removed first)
            if (!pg->palo->fInWorld || pg->palo->fRemoved)
            {
                pg->palo->fInWorld = false;
                pg->palo->fRemoved = true;
                AddLo(psw.get(), pg->palo);
            }
            // (updated only when the reference updates it: its update table (SW +0x3F14 / +0x3F20); a far guard's root
            // is inactive there)
            bool fUpdRef = false;
            for (int i = 0, c = ram0.S32(SW_ADDR + 0x3F14); i < c && i < 100000 && !fUpdRef; ++i)
                fUpdRef = ram0.U32(ram0.U32(SW_ADDR + 0x3F20) + 8 * i + 4) == a;
            if (pg->palo->iupd < 0 && fUpdRef) AddLoToUpdateTable(psw.get(), pg->palo);
            if (pg->palo->iupd >= 0 && !fUpdRef) RemoveLoFromUpdateTable(psw.get(), pg->palo);
        }
        const STEPPHYS& st = St(pg);
        std::printf("guard %d (cid 0x%X): %s, update entry %d, jobs %zu; step: custom entry %d (of %zu), probes %d / %d, "
                    "size %.2f, floor %p\n", pg->palo->oid, pg->palo->Cid(), pg->palo->fInWorld ? "in the world" : "out",
                    pg->palo->iupd, pg->stack.ajob.size(), st.isoeCustom, pg->palo->asoe.size(), st.iposProbe0, st.iposProbe1,
                    st.sSize, (void*)st.pxpFloor);
    }
    // the entries' world geometry from the imported records (the first refresh ran before the import: with the
    // world's PO placement (a proxy's PO keeps an identity object record at the load) every guard copy's entries were
    // still at the origin, overlapping each other: world 9's 11539s hit one another)
    RefreshSwPhysWorldNow(psw.get());
    double dSum = 0;
    int cCmp = 0, cJobOk = 0, cMaskOk = 0;
    for (size_t k = 1; k < aram.size(); ++k)
    {
        if (!ram1.FLoad(aram[k])) { std::fprintf(stderr, "cannot read %s\n", aram[k].c_str()); return 2; }
        // (strided dumps, f.N.ram: N - N' native frames between two of them, the clock spread evenly)
        const int cstep = std::max(1, IFrameOfPath(aram[k]) - IFrameOfPath(aram[k - 1]));
        const float t0 = ram0.F32(0x2DDED0), t1 = ram1.F32(0x2DDED0);
        g_uRandState = ram0.U32(ram0.U32(0x2C49D4) + 0x58);
        for (int istep = 0; istep < cstep; ++istep)
        {
        if (cstep == 1)
        {
            g_clock.t = ram1.F32(0x2DDED0);
            g_clock.dt = ram1.F32(0x2DDED4);
            g_clock.dt40 = ram1.F32(0x2DDF10);
        }
        else
        {
            g_clock.dt = (t1 - t0) / static_cast<float>(cstep);
            g_clock.t = t0 + g_clock.dt * static_cast<float>(istep + 1);
            g_clock.dt40 = g_clock.dt;
        }
        g_actview.posCm = glm::vec3(ram0.F32(0x2DDF40 + 0x90), ram0.F32(0x2DDF40 + 0x94), ram0.F32(0x2DDF40 + 0x98));
        g_actview.grfCm = ram0.U32(0x2DDF40 + 0x14);
        // the other objects' records from the frame's start (the players / actors are not native)
        for (const auto& [a, plo] : rr.map.mpaplo)
        {
            if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
            auto it = mpfGuardTree.find(plo);
            if (it == mpfGuardTree.end()) it = mpfGuardTree.emplace(plo, fInGuardTree(plo)).first;
            if (it->second) continue;
            ALO* palo = static_cast<ALO*>(plo);
            const int c = ram0.S32(a + 0x50);
            const uint32_t base = ram0.U32(a + 0x54);
            if (c != static_cast<int>(palo->axf.size())) continue;
            if (!FCidDerivedFrom(plo->Cid(), 0x34) && !(plo->paloParent && FCidDerivedFrom(PloRoot(plo)->Cid(), 0x34))) continue;
            for (int i = 0; i < c; ++i)
            {
                XF* pxf = palo->axf[i].get();
                CTRL* pctrl = pxf->pctrlFirst;
                const float* pg = pxf->pg;
                const float* pdg = pxf->pdgScale;
                ImportXf(ram0, base + 0x100 * static_cast<uint32_t>(i), pxf);
                pxf->pctrlFirst = pctrl;
                pxf->pg = pg;
                pxf->pdgScale = pdg;
            }
        }
        AiImport(ram0, &airef);
        // (the pause counts as other code leaves them in the frame, as --ai)
        if (!std::getenv("SLY2N_AIPAUSE_A") && istep == 0)
        {
            g_gamepause.c78 = ram1.S32(REF_SW_ADDR + 0x3D78);
            g_gamepause.c80 = ram1.S32(REF_SW_ADDR + 0x3D80);
            g_gamepause.c84 = ram1.S32(REF_SW_ADDR + 0x3D84);
        }
        auto dbg = [&](const char* when) {
            static const int s_oid = std::getenv("SLY2N_RECDEBUG") ? std::atoi(std::getenv("SLY2N_RECDEBUG")) : -1;
            for (auto& [aa, pgg] : airef.aguard)
                if (pgg->palo->oid == s_oid && pgg->palo->axf.size() > 1)
                {
                    XF* x0 = pgg->palo->axf[0].get();
                    std::printf("rec %s: record0 local %.0f %.0f world %.0f %.0f parent %p\n", when, x0->mat[3][0], x0->mat[3][1], x0->matWorld[3][0],
                                x0->matWorld[3][1], (void*)x0->pxfRef);
                    XF* x = pgg->palo->axf[1].get();
                    std::printf("rec %s: record1 local %.0f %.0f %.0f world %.0f %.0f %.0f\n", when, x->mat[3][0], x->mat[3][1], x->mat[3][2],
                                x->matWorld[3][0], x->matWorld[3][1], x->matWorld[3][2]);
                }
        };
        dbg("before");
        UpdateSw(psw.get());
        dbg("after update");
        for (const auto& plo : psw->aploAll)
            if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), 0x30)) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
        dbg("after worlds");
        if (cstep == 1) AiDeliverHits(ram0, ram1, &airef);  // (the players' attacks are not native: their hits)
        }
        // the comparison
        std::string s;
        for (auto& [a, pg] : airef.aguard)
        {
            const uint32_t ax = ram1.U32(a + 0x60);
            const glm::vec3 posRef = ram1.FValid(ax) ? glm::vec3(ram1.Vec4(ax + 0x70)) : glm::vec3(0.0f);
            const glm::vec3 pos = PosAi(pg->palo);
            const double d = glm::length(pos - posRef);
            const uint64_t grf = static_cast<uint64_t>(ram1.U32(a + 0x2C0)) | (static_cast<uint64_t>(ram1.U32(a + 0x2C4)) << 32);
            std::string jr, jn;
            const int cjob = ram1.S32(a + 0x26C + 4);
            for (int i = 0; i < cjob && i < 8; ++i)
            {
                char ach[8];
                std::snprintf(ach, sizeof ach, "%X ", CidOfJobVt(ram1.U32(ram1.U32(a + 0x26C + 8 + 4 * i) + 0xC)));
                jr += ach;
            }
            for (auto& pj : pg->stack.ajob)
            {
                char ach[8];
                std::snprintf(ach, sizeof ach, "%X ", pj->cid);
                jn += ach;
            }
            ++cCmp;
            dSum += d;
            cJobOk += jr == jn;
            cMaskOk += grf == pg->grfState;
            char ach[512];
            std::snprintf(ach, sizeof ach, "  guard %d: d %.2f (native %.1f %.1f %.1f ref %.1f %.1f %.1f) mask %s jobs %s%s", pg->palo->oid,
                          d, pos.x, pos.y, pos.z, posRef.x, posRef.y, posRef.z, grf == pg->grfState ? "=" : "differ",
                          jr == jn ? "= " : "differ: ref ", jr == jn ? jr.c_str() : (jr + "native " + jn).c_str());
            s += ach;
            if (rr.fVerbose)
            {
                const STEPPHYS& st = St(pg);
                const glm::mat4 mN = pg->palo->apxfRef[2] ? pg->palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
                const glm::mat4 mR = ram1.FValid(ax) ? ram1.Mat4(ax + 0x40) : glm::mat4(1.0f);
                std::snprintf(ach, sizeof ach, "\n    floor %d vel %.1f %.1f %.1f (ref %.1f %.1f %.1f) heading %.4f (ref %.4f) facing %.4f "
                              "(ref %.4f) turn %.3f (ref %.3f) rot %.3f (ref %.3f)", st.pxpFloor != nullptr, st.vecVel.x, st.vecVel.y,
                              st.vecVel.z, ram1.F32(a + 0x190), ram1.F32(a + 0x194), ram1.F32(a + 0x198), st.radHeading, ram1.F32(a + 0x1A8),
                              std::atan2(mN[0][1], mN[0][0]), std::atan2(mR[0][1], mR[0][0]), st.uTurn, ram1.F32(a + 0x180),
                              st.posTarget.z, ram1.F32(a + 0x1E8));
                s += ach;
                const uint32_t a58 = ram1.U32(a + 0x58);
                const XF* p58 = pg->palo->apxfRef[0];
                if (p58 && ram1.FValid(a58))
                    std::snprintf(ach, sizeof ach, "\n    pos record v %.2f %.2f %.2f mode %d (ref %.2f %.2f %.2f mode %d)", p58->vel.x, p58->vel.y,
                                  p58->vel.z, p58->mode, ram1.F32(a58 + 0xB0), ram1.F32(a58 + 0xB4), ram1.F32(a58 + 0xB8), ram1.S16(a58 + 0x98));
                else ach[0] = 0;
                s += ach;
            }
            s += "\n";
        }
        std::printf("ai-free frame %d (t %.3f):\n%s", IFrameOfPath(aram[k]), g_clock.t, s.c_str());
        std::swap(ram0, ram1);
    }
    std::printf("ai-free total: %d guard-frames, mean distance %.3f, job stacks %d equal, masks %d equal\n", cCmp,
                cCmp ? dSum / cCmp : 0.0, cJobOk, cMaskOk);
    return 0;
}

// The game's harnesses (--sly-start RAM with SLY2N_AI_START=1): the world's state and its guards (their AI, jobs,
// detectors) imported from the same reference snapshot at frame 0, then the guards run natively (with native Sly:
// the guard taking his cane's contacts). The players' own import (slycheck.cpp) runs after this one.
bool FImportAiStart(SW* psw, const std::string& pathRam)
{
    g_fSpawnersPaused = true;  // (their slots are not imported)
    static REFRUN s_rr;
    static AIREF s_airef;
    RAM ram;
    if (!ram.FLoad(pathRam)) { std::fprintf(stderr, "ai-start: cannot read %s\n", pathRam.c_str()); return false; }
    s_rr = REFRUN{};
    s_rr.psw = psw;
    for (ASEGA* p : std::vector<ASEGA*>(g_apasega)) RetractAsega(p);
    for (SMA* p : std::vector<SMA*>(g_apsma)) RetractSma(p);
    MapObjects(psw, ram, s_rr.map);
    RemapSms(psw, ram, s_rr.map);
    ImportState(s_rr, ram);
    for (const auto& plo : psw->aploAll)  // (the world matrices of the imported records)
        if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), 0x30)) UpdateAloHierarchyWorlds(static_cast<ALO*>(plo.get()));
    s_airef = AIREF{};
    s_airef.pmap = &s_rr.map;
    s_airef.pmpaasega = &s_rr.mpaasega;
    s_airef.pmpasma = &s_rr.mpasma;
    s_airef.fFree = true;
    AiImport(ram, &s_airef);
    ClearPlayerStatus77();  // (the players run natively: their own slot 77)
    for (auto& [a, pg] : s_airef.aguard)
    {
        if (pg->pdet) pg->pdet->fTouchRam = false;
        // (in the world as in the reference: its flag bit 60 (+0x2C bit 28); natively a spawner's or a proxy's copy may
        // not be placed yet)
        const bool fIn = pg->palo->fInWorld || ((ram.U32(a + 0x2C) >> 28) & 1);
        if (fIn)
        {
            // (AddLo only adds a removed object: one neither removed nor in the world is marked removed first)
            if (!pg->palo->fInWorld || pg->palo->fRemoved)
            {
                pg->palo->fInWorld = false;
                pg->palo->fRemoved = true;
                AddLo(psw, pg->palo);
            }
            if (pg->palo->iupd < 0) AddLoToUpdateTable(psw, pg->palo);
        }
        std::printf("ai-start: guard %d (cid 0x%X) at 0x%X (%p): %s, jobs %zu\n", pg->palo->oid, pg->palo->Cid(), a, (void*)pg->palo,
                    pg->palo->fInWorld ? "in the world" : "out", pg->stack.ajob.size());
    }
    return true;
}
