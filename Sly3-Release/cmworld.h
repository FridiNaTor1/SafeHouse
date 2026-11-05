#pragma once

// The camera in the native game: g_cmworld / g_cmfollowworld bound to the object model, the clock and the pad;
// UpdateCm registered as the world update's pass 0x14D478; the world's camera options applied at a load; the
// player's camera (0x1C3FF0) and the zones' cameras (0x221178 / 0x221268 / 0x221298); the renderer's camera.

#include "cm.h"
#include "render.h"

struct SW;
struct LO;

// After a world load (the original: InitCm 0x14D6D8 in the transition 0x20E890, then the loader's camera option
// block, then the world start's pushes): the camera reset, the world's options applied, the player's camera.
void CmOnWorldLoad(SW* psw);
// The end of the transition's load (orig 0x20E890, after the checkpoint (whose 0x228750 cuts the camera) and the
// player's placement): UpdateCm 0x14D478 once, then CutCm 0x14D8C0, so that the first frame starts where the
// reference's does.
void CmOnWorldStart();
// orig: 0x001C3FF0 (the object becomes / stops being the camera's focus: through its zone (+0x16C) when it has one,
// else the follow camera at priority 1)
void SetCmFocusObject(LO* plo, bool fOn);

// orig: 0x0012E6A0 / 0x0012E6E8: a camera object (cid 0x2C) enabled / disabled (the scripts' cutscene and briefing
// views): its view at priority 10. TODO(world/splice): call it from the objects' enable / disable (0x1A5800 /
// 0x1A5848) once those are dispatched natively.
void EnableCmObject(LO* plo, bool fOn);

// The renderer's camera from the native CM (cm.h CMVIEW: the display's aspect mode included) and the fog.
CAMERA CameraFromCm(const CM& cm);
void ApplyCmFog(const CM& cm, RENDEROPTS* popts);

// EOPID 1055 set by a script (seto!): the camera object's (cid 0x2C) target, +0xE0
void SetCmobjTarget(LO* plo, LO* ploTarget);

// The zones made by code (orig 0x0013E3C8(0xF, 0, oid, -1), e.g. the players' interaction zone of 0x1910F8): the
// camera of a class-0xF object without options (InitLo 0x220ED0's defaults)
struct ZONECM;
void InitZoneCamera(LO* pzone);
ZONECM* PzonecmFromLo(LO* pzone);  // (null: not a zone)
// orig: 0x00221268 (the zone's forced state +0x13C (-1 by the player's position, 0 off, 1 on), then 0x221298)
void SetZoneForced(LO* pzone, int n);
// orig: 0x00194118 (the interaction zone's camera back to its defaults: cpp 3, grf 5, return 1, the shared entry)
void ResetZoneInteract(LO* pzone);
// a zone's first camera entry (+0x114: the loaded entries' first, or 0x194118's shared one); null: not a zone
struct ZONECME;
ZONECME* PcmeFirstOfZone(LO* pzone);
