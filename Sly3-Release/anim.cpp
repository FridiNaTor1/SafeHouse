// The animation runtime's world-level passes (see anim.h).

#include "sly3/ids3.h"
#include "wr.h"
#include "anim.h"
#include "objmethod.h"
#include <cstdlib>
#include <algorithm>
#include <cstdio>

#include "asega.h"
#include "clock.h"
#include "physworld.h"
#include "update.h"
#include "bone.h"
#include "smrun.h"
#include "sw.h"
#include "xf.h"

#include <cstring>

namespace
{
float FloatField(const LO* plo, uint32_t ib, float gDefault)
{
    uint32_t uDefault;
    std::memcpy(&uDefault, &gDefault, 4);
    const uint32_t u = plo->U32Field(0, ib, uDefault);
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

// The ASEG's options (field options at the original offsets, EOPID 107 the apply parameters {t, svt, grf}).
void InitAseg(ASEG* paseg)
{
    const ASEG* psrc = paseg;
    paseg->prio = static_cast<int>(psrc->U32Field(0, 0x6C, 0));
    paseg->fSound = static_cast<int>(psrc->U32Field(0, 0x70, 0));
    paseg->svt = FloatField(psrc, 0x78, 1.0f);
    paseg->gSuck0 = FloatField(psrc, 0x80, 1.0f);
    paseg->gSuck1 = FloatField(psrc, 0x84, 0.0f);
    paseg->gSuck2 = FloatField(psrc, 0x88, 0.0f);
    paseg->gSuckMin = FloatField(psrc, 0x90, 0.0f);
    paseg->gSuckMax = FloatField(psrc, 0x94, 1.0f);
    for (const OPT& opt : psrc->aopt)
    {
        if (opt.eopid != 113 || opt.aval.size() < 3) continue;  // (sly3 113 = sly2 107)
        auto g = [](const OPTVAL& v) { return v.tagk == 0x15 || v.k == 0x15 ? static_cast<float>(v.n) : v.ag[0]; };
        paseg->fApplyParams = true;
        paseg->tApply = g(opt.aval[0]);
        paseg->svtApply = g(opt.aval[1]);
        paseg->grfApply = opt.aval[2].n;
    }
}
}  // namespace

void InitWorldAnimationRecords(SW* psw)
{
    ResetAsegas();
    ResetBoneBindings();
    g_apsma.clear();
    // the records (0x22D7A0's run-time parts) and the bone bindings (PostLoad 0x1F36A8), parents first
    for (const auto& plo : psw->aploAll)
        if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) InitAloXfs(static_cast<ALO*>(plo.get()));
    // the loader's velocity options (EOPIDs 29 / 30: vt slots 35 / 36; 75 / 76: 0x1F4898 / 0x1F4930, e.g. world 41's
    // spinning rings 20684 ..) set the records' modes after 0x22D7A0 read
    // them; natively the records' init runs here, after the options, so they are applied again (and so for every
    // copy, whose options are its template's), e.g. the rooftop vents' turbines 139218
    for (const auto& plo : psw->aploAll)
        if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO)))
            for (const OPT& opt : plo->aopt)
            {
                // Sly 3's EOPIDs 30 / 31 / 77 / 78 are Sly 2's 29 / 30 / 75 / 76 (eopid3map.inc), whose setters these are
                const int eopid2 = opt.eopid == 30 ? 29 : opt.eopid == 31 ? 30 : opt.eopid == 77 ? 75 : opt.eopid == 78 ? 76 : -1;
                if (eopid2 >= 0 && !opt.aval.empty())
                    if (PFNOBJSETTER pfn = PfnObjectSetter(eopid2)) pfn(plo.get(), RefFromOptionValue(opt.aval[0]));
            }
    for (const auto& plo : psw->aploAll)
        if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO)))
        {
            BindAloBones(static_cast<ALO*>(plo.get()));
            BindAloWarps(static_cast<ALO*>(plo.get()));  // (0x167B68's end: wr.cpp)
        }
    for (const auto& plo : psw->aploAll)
    {
        if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ASEG))) InitAseg(static_cast<ASEG*>(plo.get()));
        else if (plo->Cid() == Icid(CID3::SM)) InitSm(static_cast<SM*>(plo.get()));
    }
}

void PostLoadAnimLo(LO* plo)
{
    if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ASEG))) PostAsegLoad(static_cast<ASEG*>(plo));
    else if (plo->Cid() == Icid(CID3::SM)) PostSmLoad(static_cast<SM*>(plo));
}

void InitWorldAnimation(SW* psw)
{
    InitWorldAnimationRecords(psw);
    // PostLoad (vt slot 15) of the animation classes, in load order
    for (const auto& plo : psw->aploAll) PostLoadAnimLo(plo.get());
}

void UpdateWorldXfs(SW* psw)
{
    for (const auto& plo : psw->aploAll)
    {
        if (!FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        if (!palo->axf.empty()) UpdateAloXfs(palo, g_clock.t, g_clock.dt);
    }
}

void UpdateWorldXfWorlds(SW* psw)
{
    for (const auto& plo : psw->aploAll)
        if (FCidDerivedFrom(plo->Cid(), Icid(CID3::ALO))) UpdateAloXfWorlds(static_cast<ALO*>(plo.get()));
}

void StepWorldAnimation(SW* psw, float dt)
{
    g_clock.dt = dt;
    g_clock.dt40 = dt;
    g_clock.t += dt;
    UpdateWorldXfs(psw);
    UpdateAsegas();
    CollectAsegas();
}

uint32_t GrfUpdOfLo(const LO* plo)
{
    if (plo->iupd >= 0 && g_psw && plo->iupd < static_cast<int>(g_psw->aupd.size())) return g_psw->aupd[plo->iupd].grf;
    return plo->grfUpd;
}

void SetGrfUpdOfLo(LO* plo, uint32_t grf)
{
    if (plo->iupd >= 0 && g_psw && plo->iupd < static_cast<int>(g_psw->aupd.size())) g_psw->aupd[plo->iupd].grf = grf;
    else plo->grfUpd = grf;
}

// orig: 0x001F2FE0
void MarkAloXfs(ALO* palo, uint32_t grf)
{
    SetGrfUpdOfLo(palo, GrfUpdOfLo(palo) | grf);
    for (LO* pc : palo->aploChild)
        if (FCidDerivedFrom(pc->Cid(), Icid(CID3::ALO))) MarkAloXfs(static_cast<ALO*>(pc), grf);
}

namespace
{
// orig 0x1F8DD8's first loop: vt slot 17 (0x1F3980) of the entries whose transforms move (flags & 0x24 == 4)
void UpdateMovingXfs(SW* psw, uint32_t grfMask, uint32_t grfWant)
{
    for (psw->iupdCur = 0; psw->iupdCur < static_cast<int>(psw->aupd.size()); ++psw->iupdCur)
    {
        const UPDENT& e = psw->aupd[psw->iupdCur];
        if (!e.plo || (e.grf & grfMask) != grfWant) continue;
        if (FCidDerivedFrom(e.plo->Cid(), Icid(CID3::ALO))) UpdateAloXfs(static_cast<ALO*>(e.plo), g_clock.t, g_clock.dt);
    }
    psw->iupdCur = -1;
}

struct ANIMPASSES
{
    ANIMPASSES()
    {
        RegisterSwPass(0x130918, [](SW*) { UpdateAsegas(); });
        RegisterSwPass(0x1F8DD8, [](SW* psw) {
            // the physics frame (physworld.cpp): 0x134040, slot 17 of the entries with flags & 0x24 == 4 (ALO 0x1F3980 ->
            // 0x22DE88), 0x1F8F80 (1), 0x134040, 0x1F8F80 (6)
            UpdateSwPhysFrame(psw);
            CollectAsegas();
        });
        RegisterSwPass(0x2267C0, [](SW* psw) {
            // the paused update: the ASEGAs of ASEGs with flag 0x100, then the records of entries 0x104 (not 0x20)
            std::vector<ASEGA*> a = g_apasega;
            for (ASEGA* p : a)
                if (!p->fRetracted && (p->paseg->grfAseg & 0x100)) UpdateAsega(p);
            UpdateMovingXfs(psw, 0x124, 0x104);
        });
    }
};
ANIMPASSES s_animpasses;
}  // namespace
