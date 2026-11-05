// Sly 3's ASEG loader (aseg3.h).

#include "aseg3.h"

#include "load3.h"
#include "trk3.h"

#include "../bis.h"

#include <stdexcept>

namespace
{
constexpr float kRecip60 = 1.0f / 60.0f;

const ASEGTIMES3& TimesOf(const ASEG3* paseg, int itimes)
{
    if (itimes < 0 || std::size_t(itimes) >= paseg->atimes.size()) throw std::runtime_error("aseg: bad time list index");
    return paseg->atimes[std::size_t(itimes)];
}

// orig3: 0x12D1C0 (keyed tracks' head): U16 the time list, U16
void LoadTrkHead3(ASEGTRK3& trk, CBinaryInputStream* pbis)
{
    trk.itimes = pbis->U16Read();
    trk.n = pbis->U16Read();
}

// orig3: 0x12D330 (cfloat 3) / 0x12D908 (cfloat 2): the head, U8 keyed axes; a constant F32 per axis not keyed
// (~grf & 7), then per key per keyed axis (every set bit of grf) cfloat F32
void LoadTrkKeyedVec3(const ASEG3* paseg, ASEGTRK3& trk, int cfloat, CBinaryInputStream* pbis)
{
    LoadTrkHead3(trk, pbis);
    trk.grf = pbis->U8Read();
    for (int grf = ~trk.grf & 7; grf; grf >>= 1)
        if (grf & 1) trk.ag.push_back(pbis->F32Read());
    const int ckey = int(TimesOf(paseg, trk.itimes).aframe.size());
    for (int ikey = 0; ikey < ckey; ++ikey)
        for (int grf = trk.grf; grf; grf >>= 1)
            if (grf & 1)
                for (int i = 0; i < cfloat; ++i) trk.ag.push_back(pbis->F32Read());
}

// orig3: 0x12E0B0 (cu16 2) / 0x12E7E0 (cu16 1): the head, U8 keyed axes (+8, signed), S8 (+9); an S16 per axis not
// keyed; per key: cu16 U16, then an S16 per keyed axis
void LoadTrkKeyedPacked3(const ASEG3* paseg, ASEGTRK3& trk, int cu16, CBinaryInputStream* pbis)
{
    LoadTrkHead3(trk, pbis);
    trk.grf = int8_t(pbis->U8Read());
    trk.s8 = pbis->S8Read();
    for (int grf = ~trk.grf & 7; grf; grf >>= 1)
        if (grf & 1) trk.an.push_back(pbis->S16Read());
    const int ckey = int(TimesOf(paseg, trk.itimes).aframe.size());
    for (int ikey = 0; ikey < ckey; ++ikey)
    {
        for (int i = 0; i < cu16; ++i) trk.au.push_back(pbis->U16Read());
        for (int grf = trk.grf; grf > 0; grf >>= 1)
            if (grf & 1) trk.an.push_back(pbis->S16Read());
    }
}

// The position track of a channel (0x135FB0's table 0x31F880 on S8 + 1)
void LoadTrkp3(const ASEG3* paseg, ASEGTRK3& trk, int kind, CBinaryInputStream* pbis)
{
    trk.kind = kind;
    switch (kind)
    {
    case -1: case 0: case 1: return;              // none / the two static tracks (0x479A18, 0x4799F8)
    case 2:                                       // 0x12D218: a vector (Read 12)
        for (int i = 0; i < 3; ++i) trk.ag.push_back(pbis->F32Read());
        return;
    case 3:                                       // 0x12DED0: S8, 3 x S16
        trk.s8 = pbis->S8Read();
        for (int i = 0; i < 3; ++i) trk.an.push_back(pbis->S16Read());
        return;
    default: break;
    }
    if (kind > 7) throw std::runtime_error("aseg: bad position track kind");
    trk.cb = pbis->U32Read();
    switch (kind)
    {
    case 4: LoadTrkKeyedVec3(paseg, trk, 3, pbis); break;
    case 5: LoadTrkKeyedVec3(paseg, trk, 2, pbis); break;
    case 6: LoadTrkKeyedPacked3(paseg, trk, 2, pbis); break;
    case 7: LoadTrkKeyedPacked3(paseg, trk, 1, pbis); break;
    }
}

// orig3: 0x12EF30 (the scalar track of kind k) + its load: 0 F32 (0x12EFE0), 1 the head then 3 F32 per key (0x12F030),
// 2 nothing (0x149050)
void LoadTrkg3(const ASEG3* paseg, ASEGTRK3& trk, int kind, CBinaryInputStream* pbis)
{
    trk.kind = kind;
    switch (kind)
    {
    case 0: trk.ag.push_back(pbis->F32Read()); break;
    case 1:
    {
        LoadTrkHead3(trk, pbis);
        const int ckey = int(TimesOf(paseg, trk.itimes).aframe.size());
        for (int i = 0; i < 3 * ckey; ++i) trk.ag.push_back(pbis->F32Read());
        break;
    }
    case 2: break;
    default: throw std::runtime_error("aseg: bad scalar track kind");
    }
}

// orig3: 0x1364A0 (U8 count; per event S8 kind, [U16 frames], then by kind (table 0x31F8B0))
void LoadAsegEvents3(std::vector<ASEGEV3>& aev, bool fTime, CBinaryInputStream* pbis)
{
    aev.assign(pbis->U8Read(), ASEGEV3{});
    for (ASEGEV3& ev : aev)
    {
        ev.kind = pbis->S8Read();
        if (fTime) ev.t = float(pbis->U16Read()) * kRecip60;
        switch (ev.kind)
        {
        case 0:
            ev.an[0] = pbis->S16Read();
            for (int i = 1; i < 4; ++i) ev.an[std::size_t(i)] = pbis->S32Read();
            for (float& g : ev.ag) g = pbis->F32Read();
            break;
        case 1: case 21: ev.an[0] = pbis->S16Read(); break;
        case 2:
            ev.an[0] = pbis->S16Read();
            ev.an[1] = pbis->S32Read();
            break;
        case 3:
            ev.ag[0] = float(pbis->U16Read()) * kRecip60;
            ev.an[0] = pbis->S16Read();
            ev.an[1] = pbis->S32Read();
            break;
        case 5:
            ev.ag[0] = float(pbis->U16Read()) * kRecip60;
            ev.ag[1] = pbis->F32Read();
            break;
        case 7: case 8:
            ev.an[0] = pbis->S32Read();
            ev.ag[0] = pbis->F32Read();
            break;
        case 13: case 14: case 15: case 16:
            ev.an[0] = pbis->S32Read();
            ev.an[1] = pbis->S32Read();
            break;
        case 17: case 20:
            ev.an[0] = pbis->S32Read();
            ev.an[1] = pbis->S32Read();
            ev.ag[0] = pbis->F32Read();
            break;
        case 24:
            pbis->ReadCb8(ev.ab.data());
            pbis->ReadCb8(ev.ab.data() + 8);
            pbis->ReadCb8(ev.ab.data() + 16);
            break;
        case 4: case 11: case 12: case 22: case 25: case 26: ev.an[0] = pbis->S32Read(); break;
        default: break;  // 6, 9, 10, 18, 19, 23, and kinds past the table: nothing more
        }
    }
}
}  // namespace

// orig3: 0x135FB0 LoadAsegFromBrx
void LoadAsegFromBrx3(ASEG3* paseg, CBinaryInputStream* pbis)
{
    paseg->tMax = float(pbis->U16Read()) * kRecip60;
    const int ctimes = pbis->U16Read();
    pbis->U16Read();  // the time lists' pool size (0x117B98)
    paseg->atimes.assign(std::size_t(ctimes), ASEGTIMES3{});
    for (ASEGTIMES3& times : paseg->atimes)  // 0x12D078
    {
        times.fWide = pbis->S8Read() != 0;
        times.aframe.resize(pbis->U8Read());
        for (uint16_t& f : times.aframe) f = times.fWide ? pbis->U16Read() : pbis->U8Read();
    }
    paseg->achn.assign(std::size_t(std::max<int>(0, pbis->S16Read())), ASEGCHN3{});
    for (ASEGCHN3& chn : paseg->achn)
    {
        chn.oid = (pbis->S32Read() << 12) >> 12;
        chn.isub = ((pbis->S16Read() & 0x3FF) << 22) >> 22;
        LoadTrkp3(paseg, chn.trkp, pbis->S8Read(), pbis);
        if (const int kind = pbis->S8Read(); kind != -1) LoadTrkg3(paseg, chn.trkg, kind, pbis);
        chn.grf = pbis->U8Read();
    }
    paseg->achg.assign(pbis->U8Read(), ASEGCHG3{});
    for (ASEGCHG3& chg : paseg->achg)
    {
        chg.kind = pbis->U8Read();
        chg.n = pbis->S32Read();
        LoadTrkg3(paseg, chg.trkg, pbis->S8Read(), pbis);
    }
    LoadAsegEvents3(paseg->aaev[0], false, pbis);
    LoadAsegEvents3(paseg->aaev[1], true, pbis);
    LoadAsegEvents3(paseg->aaev[2], false, pbis);
    sly3::BuildAsegd3(paseg);
}

// orig3: 0x135EE8: LoadOptionsFromBrx, S8 (+0xA4); 0: +0xA8 = 1 and the segment now (0x135FB0 with the pool 0x36B118)
void LoadAsegObjFromBrx3(LO* plo, CBinaryInputStream* pbis)
{
    ASEG3* paseg = static_cast<ASEG3*>(plo);
    LoadOptionsFromBrx3(plo->aopt, plo, pbis);
    paseg->fDeferred = pbis->S8Read();
    if (!paseg->fDeferred) LoadAsegFromBrx3(paseg, pbis);
}
