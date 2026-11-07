// sky.cpp: the sky (cid 0x61: e.g. world 8's three background globs, the clouds and the dome): placed at the camera
// every frame. Ported from the decomp's matching C: 0x1EF850 (vt slot 15), 0x1EF8C8 (vt slot 16).
#include "activate.h"
#include "game/sly3.h"
#include "scene.h"
#include "alo.h"
#include "cm.h"
#include "lo.h"
#include "so.h"
#include "sw.h"
#include "update.h"

// orig: 0x001EF850 (orig3 0x238CA0, Sly 3 slot 17, the flags word at +0x3C / +0x128 bit 3; vt slot 15: the ALO's post-load (0x1F36A8), always active (kind 1), updated every frame (+0x40
// |= 2), not iced (+0x148 bit 35 cleared))
void PostLoadSky(SW* psw, LO* plo)
{
    SetLoSt(psw, plo, 1);  // 0x1A6CA8
    if (plo->iupd >= 0) psw->aupd[static_cast<size_t>(plo->iupd)].grf |= 2;
    else plo->grfUpd |= 2;
    Sort(static_cast<ALO*>(plo)).grf148 &= ~(uint64_t(1) << 35);
}

namespace
{
// orig: 0x001EF8C8 (vt slot 16: the ALO's update (0x1F36E0), then at the camera's position (CM +0x90, 0x2DDFA0 +
// 0x30), keeping its own height unless +0x150, through vt slot 29)
void UpdateSky(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    ALO* palo = static_cast<ALO*>(plo);
    glm::vec3 v = g_cm.pos;
    if (!plo->U32Field(0, 0x150, 0)) v.z = PosAloCenter(palo).z;  // 0x1A62E8
    SetAloPos(palo, v);
}

struct INITSKY
{
    INITSKY() { RegisterLoUpdate(0x1EF8C8, UpdateSky); }
} s_initsky;
}  // namespace

// orig3: 0x238D08 (SKY 0x6E vt slot 21, the draw): for each camera of SW +0x3EB8 the sky's glob set is drawn with the
// object's matrix moved to the camera's position (0x2BA818 SetTranslation), keeping the object's own height unless
// +0x130 (Sly 2's UpdateSky moved the object every frame instead). Natively: the scene entries of the SKY objects are
// moved by the drawn camera's offset after their placement (so --camera-ram's camera and the free camera also get it).
void PlaceSkiesAtCamera(SCENE& scene, const std::vector<ALO*>& apalo, const glm::vec3& posCam)
{
    for (size_t i = 0; i < scene.aglobset.size() && i < apalo.size(); ++i)
    {
        const ALO* palo = apalo[i];
        if (palo->Cid() != S3CID(0x61) || palo->axf.empty()) continue;
        const glm::vec3 pos(palo->axf[0]->matWorld[3]);
        glm::vec3 d = posCam - pos;
        if (!palo->U32Field(0, 0x130, 0)) d.z = 0.0f;
        SCENEGLOBSET& sgs = scene.aglobset[i];
        for (glm::mat4& m : sgs.amatGlob) m[3] += glm::vec4(d, 0.0f);
        sgs.posBounds += d;
    }
}
