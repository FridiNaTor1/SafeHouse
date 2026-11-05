#pragma once
// turret.cpp: the aimed player object (cid 0x45): world 8's Murray turret
#include "lo.h"
struct CMATTACH;
void PostLoadTurret(LO* plo);                     // orig 0x225AA8 (vt slot 15's part after the base)
void SetTurretFocus(LO* plo, bool fOn);           // orig 0x225AE8 (vt slot 78's part after 0x1C3FF0)
bool FTurretAttach(LO* plo, CMATTACH* patt);      // the attach camera's mount (0x151950 reads +0x1D0 / +0x1D4 / +0x1BC)
void ResetTurrets();                              // a new world
