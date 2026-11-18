// The laser's authored models (CID 0x55, original slot 19 0x1DF5B0), drawn through the native 0x1A5EE8 path.
#include "laserrender.h"
#include "actanim.h"
#include "brx.h"
#include "asega.h"
#include "aseg.h"
#include "sm.h"
#include "worldglobals.h"
#include "game/sly3.h"

#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "cm.h"
#include "emitter.h"
#include "laser.h"
#include "mat.h"
#include "render.h"
#include "so.h"
#include "sw.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
// orig: 0x0013E818/0x001A5C48: native pointer options retain their authored OID until the world is loaded.
ALO* PaloLaserModel(ALO* p, uint32_t ib)
{
    const OID oid = static_cast<OID>(p->U32Field(0, ib, uint32_t(-1)));
    if (oid == -1) return nullptr;
    LO* model = PloFindSwChild(p, oid);
    if (!model) model = PloFindSwNearest(p, oid);
    return model && FCidDerivedFrom(model->Cid(), S3CID(0x30)) ? static_cast<ALO*>(model) : nullptr;
}

// orig: 0x001A5FB8: x faces the camera, y spans the width, z follows the model's 100-unit line.
void AppendLaserLine(const SCENE& scene, ALO* model, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra)
{
    const glm::vec3 d = b - a;
    const glm::vec3 n = glm::cross(a - posCamera, d);
    const float length = glm::length(n);
    if (!(0.01f < length)) return;
    const glm::vec3 x = glm::cross(n, d);
    glm::mat4 mat(1.0f);
    mat[0] = glm::vec4(x * (1.0f / glm::length(x)), 0.0f);
    mat[1] = glm::vec4(n * (1.0f / length), 0.0f);
    mat[2] = glm::vec4(d * 0.01f, 0.0f);
    mat[3] = glm::vec4(a, 1.0f);
    AppendAloExtraDraws(scene, model, mat, 1.0f, aextra);
}
// orig3 +0x180 / +0x190 (the endpoints' {model, the ASEG, the ASEGA, the scale channel}): the ASEG and its channel
struct ENDASEG3 { ASEG* paseg = nullptr; const ASEGCHN* pchn = nullptr; };
std::unordered_map<const ALO*, std::array<ENDASEG3, 2>> s_mpendaseg;
WORLD_TABLE(s_mpendaseg);   // (PC: moved aside while a kept world loads, worldglobals.h)

// the ASEG's ASEGA playing on the model (orig3 0x23FAE8 by the ASEG's oid)
ASEGA* PasegaOn(ASEG* paseg, ALO* palo)
{
    for (ASEGA* pasega : paseg->apasega)
        if (pasega && pasega->palo == palo) return pasega;
    return nullptr;
}
}  // namespace

void BindLaserModelAsegs3(ALO* p)
{
    std::array<ENDASEG3, 2> a{};
    const uint32_t aib[2] = {0x1A0, 0x1B0};
    for (int i = 0; i < 2; ++i)
    {
        ALO* model = PaloLaserModel(p, aib[i]);
        if (!model || !g_psw) continue;
        // 0x17E208(SW, 2, 0x82, model, 1): its first child of class 0x82
        ASEG* paseg = nullptr;
        for (const auto& up : g_psw->aploAll)
            if (LO* plo = up.get(); plo && plo->paloParent == model && FIsLoDerivedFrom(plo, S3CID(sly3::CID_ASEG)))
            {
                paseg = static_cast<ASEG*>(plo);
                break;
            }
        if (std::getenv("SLY2N_LASERDRAW") && !paseg) std::fprintf(stderr, "laser %d end %d model %d: no ASEG child\n", p->oid, i, model->oid);
        if (!paseg) continue;
        if (!PasegaOn(paseg, model)) PasegaApplyAseg(paseg, model, 0.0f, 1.0f, 8);   // 0x137428(q, o, 8, 0, 1)
        a[size_t(i)] = {paseg, PchnFindAseg(paseg, model, 0xC, true)};                // 0x136EE0(q, o, 0xC, 1)
        if (std::getenv("SLY2N_LASERDRAW"))
            std::fprintf(stderr, "laser %d end %d model %d aseg %d scale channel %s\n", p->oid, i, model->oid, paseg->oid,
                         a[size_t(i)].pchn ? "yes" : "no");
    }
    if (a[0].paseg || a[1].paseg) s_mpendaseg[p] = a;
}

void ResetLaserModelAsegs3() { s_mpendaseg.clear(); }

// orig: 0x001DF5B0: after the base ALO draw, cull the laser's attachment and place its four authored models.
void CollectLaserDraws(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra)
{
    if (!g_psw) return;
    for (LO* plo : g_psw->mpcidaplo[S3CID(0x55)])
    {
        ALO* p = static_cast<ALO*>(plo);
        if (!p->fInWorld || p->fRemoved || !FAloDrawn(g_psw, p)) continue;
        const OXA* oxa = Poxa(p);
        if (!oxa || !FInsideCmPlanes(&g_cm, oxa->posCenter - posCamera, oxa->sRadius)) continue;
        ALO* line = PaloLaserModel(p, 0x190);
        ALO* stretch = PaloLaserModel(p, 0x194);
        ALO* ends[2] = {PaloLaserModel(p, 0x1A0), PaloLaserModel(p, 0x1B0)};
        for (const LASERBEAM& beam : LaserBeams(p))
        {
            static const bool fPrint = std::getenv("SLY2N_LASERDRAW") != nullptr;
            static int cPrint = 0;
            if (fPrint && cPrint++ < 32)
                std::fprintf(stderr, "laser-draw %d models %d %d %d %d endpoints %.3f %.3f %.3f .. %.3f %.3f %.3f\n",
                    p->oid, line ? line->oid : -1, stretch ? stretch->oid : -1, ends[0] ? ends[0]->oid : -1,
                    ends[1] ? ends[1]->oid : -1, beam.a.x, beam.a.y, beam.a.z, beam.b.x, beam.b.y, beam.b.z);
            if (line) AppendLaserLine(scene, line, beam.a, beam.b, posCamera, aextra);
            if (!stretch && !ends[0] && !ends[1]) continue;
            const glm::vec3 d = beam.b - beam.a;
            const glm::mat3 basis = BuildLookAtMatrix(d, glm::vec3(0.0f, 0.0f, 1.0f));  // D_002A2DC0
            if (stretch)
            {
                const float length = glm::length(d);
                if (0.01f < length)
                {
                    glm::mat4 mat(basis);
                    mat[0] *= length * 0.01f;
                    mat[3] = glm::vec4(beam.a, 1.0f);
                    AppendAloExtraDraws(scene, stretch, mat, 1.0f, aextra);
                }
            }
            const auto itAseg = s_mpendaseg.find(p);
            for (int i = 0; i < 2; ++i)
            {
                if (!ends[i]) continue;
                glm::mat4 mat(basis);
                // orig3 0x224B18: the endpoint's scale channel (+0x18C / +0x19C, vt slot 3 at its ASEGA's time and
                // speed +0x1C / +0x40) scales the basis' rows (0x11D660)
                if (itAseg != s_mpendaseg.end())
                    if (const ENDASEG3& e = itAseg->second[size_t(i)]; e.pchn && e.pchn->ptrkp)
                        if (const ASEGA* pasega = PasegaOn(e.paseg, ends[i]))
                        {
                            glm::vec4 scale(1.0f);
                            e.pchn->ptrkp->Eval(&scale, nullptr, pasega->t, pasega->svt);
                            for (int k = 0; k < 3; ++k) mat[k] *= scale[k];
                        }
                mat[3] = glm::vec4(i == 0 ? beam.a : beam.b, 1.0f);
                AppendAloExtraDraws(scene, ends[i], mat, 1.0f, aextra);
            }
        }
    }
}
