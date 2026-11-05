#pragma once

struct MURRAY;
struct LO;
void PostLoadCarryUse(LO* plo);                         // 0x1842B0

void SetMurrayCarry(MURRAY* p, int phase);             // 0x1B93E8
void UpdateMurrayCarryTransitions(MURRAY* p);          // 0x1B92F8
void SetMurrayCarryMove(MURRAY* p, int state);          // 0x1B5658
void UpdateMurrayCarryHeld(MURRAY* p);                 // 0x1B5380
void ChooseMurrayThrowTarget(MURRAY* p);               // 0x1B5E98
void StartMurrayThrow(MURRAY* p, LO* target);           // 0x1B7A78
void ReleaseMurrayThrow(MURRAY* p);                    // 0x1B7970
void PutDownMurrayCarry(MURRAY* p);                    // 0x1B7AE8
void CancelMurrayCarry(MURRAY* p);                     // 0x1B7BA8
void PrintMurrayCarry(MURRAY* p);                      // debug observer
void UpdateMurrayReleased(MURRAY* p);                   // 0x1B50B0+0x1F0
