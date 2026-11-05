#pragma once

// The binocucom (orig ACTOR +0xF00, module 0x19CBF8..0x19DE10; data actor.h BINOC, state ACTOR::nF04 (-1 off,
// 0 opening, 1 on, 2 closing), view ACTOR::nF20 (0 the player's own recon view, 1 a talk (the briefings: SCPT 0x3B,
// both speakers' windows, the game paused), 2 the recon view from another object)): its camera policy (CM +0x450 the
// recon view / +0x520 the scripted one), the opening / closing clips (+0xF0C / +0xF10), the actor's held animation,
// the sounds' ducking, the pause, and its window part (+0xF24: the frame and the window screen objects, 0x19D4D0..).

struct ACTOR;
struct LO;

// orig: 0x0019CBF8 (+0x19D4D0 the window part)
void InitBinoc(ACTOR* pa);
// orig: 0x0019CC30 -> 0x0019D540 (the window part's screen objects by the owner's class)
void PostLoadBinoc(ACTOR* pa);
// orig: 0x0019CC50 (open in view n: a talk while on in another view closes it first)
void OpenBinoc(ACTOR* pa, int nView, LO* ploSpeaker, LO* ploScreen);
// orig: 0x0019CCE8 (close unless the talk prompt holds it)
void CloseBinoc(ACTOR* pa);
// orig: 0x0019CD38
void SetBinocState(ACTOR* pa, int n);
// orig: 0x0019D330 (each frame from the players' updates)
void UpdateBinoc(ACTOR* pa);
// orig: 0x0019D660 (the window part's state: the frame / window shown or hidden, the HUD's events) and 0x0019DA78
// (its update): the media code's proposed interface
void SetBinocWindowState(ACTOR* pa, int n);
void UpdateBinocWindow(ACTOR* pa);
// orig: 0x0018B2C0 (Sly; mask 0x4F0, 0x410 with the talk prompt in its state 7) / 0x00134D30 / 0x001B5038
#include <cstdint>
void UpdateActorBinoc(ACTOR* pa, uint32_t grfMask);
// orig: 0x001442D0 / 0x00144338 (vt slots 83 / 84: the actors'; the other POs' 0x205478 / 0x205588 without f): the
// effect SMs of the bits grf (0x3F: all six, the talk's mouth / face SMs D_002C3C40) held (applied at the first hold) /
// released (retracted at the last); with f an actor also plays its talk animation 0x60B while held (0x143EE0 kind 5)
int CActorTalkHold(const LO* plo);  // the actor's +0x12F4 (held for a talk)
void HoldActorEffects(LO* plo, bool fHold, bool f, uint32_t grf = 0x3F);
