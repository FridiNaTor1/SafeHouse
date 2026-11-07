// Development commands of the animation runtime:
//   Sly2-Release --anim-test W [--frames N] [--dump file]
// loads world record W, starts the animations the world starts by itself (ASEGs with apply parameters, SMs with an
// initial SMA), runs N frames of 1/60 s (records + ASEGAs) and prints what plays; --dump writes every animated
// ALO's records (local matrices, modes) after the frames.

#include "anim.h"
#include "game/sly3.h"
#include "animtest.h"
#include "asega.h"
#include "bone.h"
#include "brx.h"
#include "cls.h"
#include "smrun.h"
#include "sw.h"
#include "xf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace
{
void DumpAnimated(SW* psw, FILE* pf)
{
    std::map<std::pair<int, int>, int> mpord;
    for (const auto& plo : psw->aploAll)
    {
        if (!FCidDerivedFrom(plo->Cid(), S3CID(0x30))) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        const int ord = mpord[{plo->Cid(), plo->oid}]++;
        bool fAnim = false;
        for (const auto& pxf : palo->axf) fAnim = fAnim || pxf->pctrlFirst || pxf->mode != 0;
        if (!fAnim) continue;
        std::fprintf(pf, "alo cid 0x%02X oid %d ord %d parent %d records %zu\n", plo->Cid(), plo->oid, ord,
                     plo->paloParent ? plo->paloParent->oid : -1, palo->axf.size());
        for (size_t i = 0; i < palo->axf.size(); ++i)
        {
            const XF& xf = *palo->axf[i];
            const glm::mat4& m = xf.mat;
            std::fprintf(pf, "  xf %zu kind %d mode %d state %d ctrl %d g %.4f [%.4f %.4f] local", i, xf.kind, xf.mode,
                         xf.state, xf.pctrlFirst ? xf.pctrlFirst->kind : -1, xf.G(), xf.g0, xf.g1);
            for (int r = 0; r < 4; ++r) std::fprintf(pf, " %.4f %.4f %.4f", m[r].x, m[r].y, m[r].z);
            std::fprintf(pf, " world %.3f %.3f %.3f\n", xf.matWorld[3].x, xf.matWorld[3].y, xf.matWorld[3].z);
        }
    }
}
}  // namespace

int AnimTestCommand(const char* pszWorld, int argc, char** argv)
{
    int cframe = 0;
    std::string dump;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) cframe = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    }
    InitClasses();
    FK fk;
    int filter = 0;
    std::string err;
    if (!FResolveWorld(pszWorld, fk, filter, err))
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    auto psw = std::make_unique<SW>();
    psw->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    if (!FLoadWorldFile(psw.get(), fk, "", &cbRead, &cbFile)) return 1;
    InitWorldAnimation(psw.get());
    int casegApply = 0, csm = 0, csmInitial = 0, cctrl = 0, crec = 0, cskinned = 0;
    int acctrlKind[10] = {};
    for (const auto& plo : psw->aploAll)
    {
        if (FCidDerivedFrom(plo->Cid(), S3CID(0x6D)) && static_cast<ASEG*>(plo.get())->fApplyParams) ++casegApply;
        if (plo->Cid() == S3CID(0x6F))
        {
            ++csm;
            csmInitial += static_cast<SM*>(plo.get())->fInitial;
        }
        if (FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
        {
            ALO* palo = static_cast<ALO*>(plo.get());
            for (const auto& pxf : palo->axf)
            {
                ++crec;
                for (CTRL* p = pxf->pctrlFirst; p; p = p->pnext)
                {
                    ++cctrl;
                    if (p->kind >= 0 && p->kind < 10) ++acctrlKind[p->kind];
                }
            }
            if (const auto* paa = PaabindOfAlo(palo))
                for (const auto& a : *paa) cskinned += !a.empty();
        }
    }
    std::printf("world %s: %zu objects, %d records, %d controllers, %d skinned groups bound; %d ASEGs apply themselves, "
                "%d SMs (%d with an initial SMA)\n",
                pszWorld, psw->aploAll.size(), crec, cctrl, cskinned, casegApply, csm, csmInitial);
    std::printf("controllers by kind:");
    for (int k = 0; k < 10; ++k)
        if (acctrlKind[k]) std::printf(" %d: %d", k, acctrlKind[k]);
    std::printf("\nafter the load: %zu ASEGAs, %zu SMAs\n", g_apasega.size(), g_apsma.size());
    for (int i = 0; i < cframe; ++i) StepWorldAnimation(psw.get(), 1.0f / 60.0f);
    std::printf("after %d frames: %zu ASEGAs, %zu SMAs, %d events for other systems\n", cframe, g_apasega.size(),
                g_apsma.size(), g_cAsegaEventDropped);
    if (!dump.empty())
        if (FILE* pf = std::fopen(dump.c_str(), "w"))
        {
            for (ASEGA* p : g_apasega)
                std::fprintf(pf, "asega aseg %d alo %d t %.4f tMax %.4f svt %.4f grf 0x%X iea %d\n", p->paseg->oid,
                             p->palo ? p->palo->oid : -1, p->t, PasegdOf(p->paseg)->tMax, p->svt, p->grfapl, p->iea);
            for (SMA* p : g_apsma)
                std::fprintf(pf, "sma sm %d alo %d cur %d goal %d next %d asega %d\n", p->psm->oid, p->palo ? p->palo->oid : -1,
                             OidSmaCur(p), OidSmaGoal(p), p->ismsNext >= 0 ? p->psm->asms[p->ismsNext].oid : -1,
                             p->pasega ? p->pasega->paseg->oid : -1);
            DumpAnimated(psw.get(), pf);
            std::fclose(pf);
        }
    return 0;
}
