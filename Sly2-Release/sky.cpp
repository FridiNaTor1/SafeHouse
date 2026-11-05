// sky.cpp: the sky (cid 0x61: e.g. world 8's three background globs, the clouds and the dome): placed at the camera
// every frame. Ported from the decomp's matching C: 0x1EF850 (vt slot 15), 0x1EF8C8 (vt slot 16).
#include "activate.h"
#include "alo.h"
#include "cm.h"
#include "lo.h"
#include "so.h"
#include "sw.h"
#include "update.h"

// orig: 0x001EF850 (vt slot 15: the ALO's post-load (0x1F36A8), always active (kind 1), updated every frame (+0x40
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
