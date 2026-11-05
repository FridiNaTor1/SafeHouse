#include <cstdlib>
#include <cstdio>
#include "aseg.h"

#include "bis.h"
#include "loadtrace.h"

void (*g_pfnLoadOptionsFromBrx)(void* pv, CBinaryInputStream* pbis) = nullptr;

namespace
{
constexpr float kRecip60 = 0.016666668f;  // 0x3C888889: frames to seconds

glm::vec3 VecRead(CBinaryInputStream* pbis)
{
    return pbis->ReadVector();  // orig 0x117EB0 (one 12-byte read)
}
}

// orig: 0x0012B3B0 (a position track of kind 0..5, null for others)
std::unique_ptr<TRKP> PtrkpNew(int kind)
{
    std::unique_ptr<TRKP> p;
    switch (kind)
    {
    case 0: p = std::make_unique<TRKPC>(); break;
    case 1: { auto q = std::make_unique<TRKPK>(); q->fTwoTangents = true; p = std::move(q); break; }
    case 2: p = std::make_unique<TRKPK>(); break;
    case 3: { auto q = std::make_unique<TRKPA>(); q->cfKey = 3; p = std::move(q); break; }
    case 4: { auto q = std::make_unique<TRKPA>(); q->cfKey = 2; p = std::move(q); break; }
    case 5: p = std::make_unique<TRKPN>(); break;
    default: return nullptr;
    }
    p->kind = kind;
    return p;
}

// orig: 0x0012B4F8
void TRKPC::Load(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x12B4F8, pbis);
    pbis->Read(12, &pos);  // orig: Read (0x117968)
}

// orig: 0x0012B5E0 (0x30-byte keys) / 0x0012BA98 (0x20-byte keys): c, then per key its frame (U16) and vectors
void TRKPK::Load(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(fTwoTangents ? 0x12B5E0 : 0x12BA98, pbis);
    const int c = pbis->U16Read();
    akey.resize(c);
    for (KEYP& key : akey)
    {
        key.t = static_cast<float>(pbis->U16Read()) * kRecip60;
        key.pos = VecRead(pbis);
        key.vIn = VecRead(pbis);
        key.vOut = fTwoTangents ? VecRead(pbis) : key.vIn;
    }
}

// orig: 0x0012BF30 (cfKey 3) / 0x0012C570 (cfKey 2): v, c times, a byte of keyed axes, each keyed axis' keys
void TRKPA::Load(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(cfKey == 3 ? 0x12BF30 : 0x12C570, pbis);
    v = VecRead(pbis);
    const int c = pbis->U16Read();
    at.resize(c);
    for (float& t : at)
        t = static_cast<float>(pbis->U16Read()) * kRecip60;
    const int grf = pbis->U8Read();
    for (int j = 0; j < 3; j++)
    {
        aag[j].clear();
        if (!(grf & (1 << j)))
            continue;
        aag[j].resize(static_cast<std::size_t>(c) * cfKey);
        for (float& g : aag[j])
            g = pbis->F32Read();
    }
}

// orig: 0x0012CB98 (a scalar track of kind 0..2, null for others)
std::unique_ptr<TRKG> PtrkgNew(int kind)
{
    std::unique_ptr<TRKG> p;
    switch (kind)
    {
    case 0: p = std::make_unique<TRKGC>(); break;
    case 1: p = std::make_unique<TRKGK>(); break;
    case 2: p = std::make_unique<TRKGN>(); break;
    default: return nullptr;
    }
    p->kind = kind;
    return p;
}

// orig: 0x0012CC68
void TRKGC::Load(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x12CC68, pbis);
    g = pbis->F32Read();
}

// orig: 0x0012CCB8
void TRKGK::Load(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x12CCB8, pbis);
    const int c = pbis->U16Read();
    akey.resize(c);
    for (KEYG& key : akey)
    {
        key.t = static_cast<float>(pbis->U16Read()) * kRecip60;
        key.g = pbis->F32Read();
        key.dgIn = pbis->F32Read();
        key.dgOut = pbis->F32Read();
    }
}

// orig: 0x0012F590 (c events {t, kind, payload})
struct LO;
LO* PloLoading();  // brx.cpp
int OidOfLo(const LO*);
void LoadAsegEventsFromBrx(std::vector<ASEGEV>* paev, int fTime, CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x12F590, pbis);
    const int c = pbis->U8Read();
    paev->assign(c, ASEGEV{});
    for (ASEGEV& ev : *paev)
    {
        ev.kind = pbis->S8Read();
        if (fTime)
            ev.t = static_cast<float>(pbis->S16Read()) * kRecip60;
        switch (ev.kind)
        {
        case 0:
            ev.an[0] = pbis->S16Read();
            ev.an[1] = pbis->S32Read();
            ev.an[2] = pbis->S32Read();
            ev.an[3] = pbis->S32Read();
            ev.ag[2] = pbis->F32Read();
            ev.ag[0] = pbis->F32Read();
            ev.ag[1] = pbis->F32Read();
            break;
        case 3:
            ev.ag[0] = static_cast<float>(pbis->S16Read()) * kRecip60;
            ev.an[0] = pbis->S16Read();
            break;
        case 5:
            ev.ag[0] = static_cast<float>(pbis->S16Read()) * kRecip60;
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
        case 17:
            ev.an[0] = pbis->S32Read();
            ev.an[1] = pbis->S32Read();
            ev.an[2] = pbis->S32Read();
            ev.ag[0] = pbis->F32Read();
            break;
        case 20:
            ev.an[0] = pbis->S32Read();
            ev.an[1] = pbis->S32Read();
            ev.ag[0] = pbis->F32Read();
            break;
        case 1: case 2: case 21:
            ev.an[0] = pbis->S16Read();
            break;
        case 4: case 11: case 12: case 22:
            ev.an[0] = pbis->S32Read();
            break;
        case 24:
            pbis->ReadCb8(ev.ab.data());
            pbis->ReadCb8(ev.ab.data() + 8);
            pbis->ReadCb8(ev.ab.data() + 16);
            break;
        default:  // 6, 9, 10, 18, 19, 23, > 24: nothing more
            break;
        }
        // (debug: SLY2N_FINDOPTVAL=n also lists the ASEG events naming n, with the object being loaded)
        if (static const char* s_sz = std::getenv("SLY2N_FINDOPTVAL"); s_sz)
            for (int n : ev.an)
                if (n == std::atoi(s_sz))
                {
                    LO* plo = PloLoading();
                    std::fprintf(stderr, "optval %d: aseg event kind %d t %.3f of the object loading %d\n", n, ev.kind, ev.t,
                                 plo ? OidOfLo(plo) : -1);
                    break;
                }
    }
}

// orig: 0x0012F2A8 LoadAsegFromBrx
void LoadAsegFromBrx(ASEGD* pasegd, void* pvOptions, CBinaryInputStream* pbis)
{
    {
        LOAD_SCOPE(0x12F2A8, pbis);
        pasegd->tMax = pbis->F32Read();
        const int cchn = pbis->S16Read();
        pasegd->achn.clear();
        pasegd->achn.resize(cchn);
        for (ASEGCHN& chn : pasegd->achn)
        {
            const int32_t n = pbis->S32Read();
            chn.oid = (n << 12) >> 12;
            const int32_t k = pbis->S16Read();
            chn.isub = ((k & 0x3FF) << 22) >> 22;
            int kind = pbis->S8Read();
            if (kind != -1)
            {
                chn.ptrkp = PtrkpNew(kind);
                if (chn.ptrkp)
                    chn.ptrkp->Load(pbis);
                chn.fScalar = 0;
            }
            kind = pbis->S8Read();
            if (kind != -1)
            {
                chn.ptrkg = PtrkgNew(kind);
                if (chn.ptrkg)
                    chn.ptrkg->Load(pbis);
                chn.fScalar = 1;
            }
            chn.fDisabled = pbis->U8Read() != 0;
        }
        const int cchg = pbis->U8Read();
        pasegd->achg.clear();
        pasegd->achg.resize(cchg);
        for (ASEGCHG& chg : pasegd->achg)
        {
            chg.kind = pbis->U8Read();
            chg.n = pbis->S32Read();
            chg.ptrkg = PtrkgNew(pbis->S8Read());
            if (chg.ptrkg)
                chg.ptrkg->Load(pbis);
        }
        LoadAsegEventsFromBrx(&pasegd->aaev[0], 0, pbis);
        LoadAsegEventsFromBrx(&pasegd->aaev[1], 1, pbis);
        LoadAsegEventsFromBrx(&pasegd->aaev[2], 0, pbis);
    }
    if (g_pfnLoadOptionsFromBrx != nullptr)
        g_pfnLoadOptionsFromBrx(pvOptions, pbis);
}
