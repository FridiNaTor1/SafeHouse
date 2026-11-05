#pragma once

// The climb (orig ACTOR +0xF50, module 0x19DE10..0x19EF50, actor.h CLIMB): a pipe (cid 0x0B, jt.h) climbed along its
// path: Sly's state 8 (Bentley's / Murray's 13). The hand (+0x30 in the climber's frame) is held on the pipe's point at
// the distance +0xF60 by three line contacts each physics frame (0x19E950 -> slot 82 0x140C30 -> 0x205008); the
// climber turns onto the pipe's frame (0x19DF70, slot 70); the stick moves the distance at the climb clips' speed
// (0x19E1E0, the transitions).

#include <glm/glm.hpp>

struct ACTOR;
struct XP;

// orig: 0x0019DE10
void InitClimb(ACTOR* pa);
// orig: 0x0019DE58 (the clips: the parts +0xFA4 / +0xFB4 (the climbs' blend), +0xFC4, +0xFC8, +0xFCC)
void PostLoadClimb(ACTOR* pa);
// orig: 0x0019EAB0 (-1 off, 0..4)
void SetClimbState(ACTOR* pa, int n);
// orig: 0x0019E1E0 (each frame of the climbing state)
void UpdateClimbTransitions(ACTOR* pa);
// orig: 0x0019DF70 (vt slot 70's part: the angular velocity onto the pipe's frame)
void UpdateClimbMotion(ACTOR* pa);
// orig: 0x0019E950 (vt slot 59's part: the hand held on the pipe)
void AddClimbContacts(ACTOR* pa);
// orig: 0x0019E9C8 (vt slot 55's part: a new contact without friction / bounce)
void AdjustClimbXp(XP* pxp);
// orig: 0x00205008 (the line contacts of the point posA (minus +0x1010) along the frame of dir: grf 1 / 2 / 4 its
// three axes (kind 0xB), 8 / 0x10 / 0x20 an axis only where already met (kind 0xA / 0xE))
void AddActorLineXps(ACTOR* pa, int grf, const glm::vec3& posA, const glm::vec3& dir, const glm::vec3& posHand);
