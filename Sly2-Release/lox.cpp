// Located objects: their world-space setters and original virtual parent change.
#include "lox.h"
#include "activate.h"
#include "alo.h"
#include "crv.h"
#include "lo.h"
#include "mat.h"
#include "so.h"
#include "sw.h"
#include "vtnative.h"
#include "xfphys.h"

namespace {
// orig: 0x2313F0. Native LOX matrices are evaluated on demand, so no cached
// matrix version needs invalidating; a moved, added root still needs rechecking.
void TouchLox(LOX* plox)
{
    if (!plox->paloParent && plox->fInWorld) QueueLoRoot(g_psw, plox);
}
}

// orig: 0x2312A0 (slot 29): world position into the reference record's frame.
void SetLoxPos(LOX* plox, const glm::vec3& pos)
{
    plox->mat[3] = glm::vec4(PosConvertXfs(nullptr, plox->pxf, pos), 1.0f);
    TouchLox(plox);
}
NATIVEFN(0x2312A0, SetLoxPos);

// orig: 0x231300 (slot 30): world rotation into the reference record's frame.
void SetLoxRotation(LOX* plox, const glm::mat3& mat)
{
    SetMat3(&plox->mat, MatConvertXfs(nullptr, plox->pxf, mat));
    TouchLox(plox);
}
NATIVEFN(0x231300, SetLoxRotation);

// orig: 0x2315F0 (slot 10). Capture requested world components before replacing
// the reference, perform the base parent change, then restore through the
// object's original virtual setters. Other local components remain.
void SetLoxParent(LOX* plox, ALO* paloNew, XF* pxfNew, int grf)
{
    glm::mat4 world(1.0f);
    if (grf & 3)
    {
        if (plox->fInWorld) world = MatWorldLox(plox);
        else
        {
            world = plox->mat;
            for (XF* pxf = plox->pxf; pxf; pxf = pxf->pxfRef)
                world = MulMatrix4(pxf->mat, world);
        }
    }
    plox->pxf = pxfNew ? pxfNew : paloNew ? PxfObj(paloNew) : nullptr;
    SetLoParentActivation(g_psw, plox, paloNew);
    if (grf & 1)
        if (auto pfn = PfnVt<void (*)(LO*, const glm::vec3&)>(plox, 29)) pfn(plox, PosOf(world));
    if (grf & 2)
        if (auto pfn = PfnVt<void (*)(LO*, const glm::mat3&)>(plox, 30)) pfn(plox, Mat3Of(world));
}
NATIVEFN(0x2315F0, SetLoxParent);
