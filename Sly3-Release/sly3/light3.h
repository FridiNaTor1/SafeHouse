#pragma once

// Sly 3's lights for the renderer (light.h's bake P_M and forward lighting P_G, copied from Sly 2).
//
// What is Sly 3's (checked against the asm and the PCSX2 RAM):
// - The light bake: RenderSw 0x209C40 -> 0x209BA0 (Sly 2 0x1CD9E0) calls Glob_AddLightPackets 0x18E0B8 (0x1681F8) for
//   the objects whose lighting version (+0xBC) is not SW +0x44F8; RenderGlob 0x18E800 does the same per glob set
//   (+0x4C). The lights go to VU1 in phase 0, Glob_UploadMatrices' twin 0x18DEC8: every class-4 object (0x17E208)
//   with +0x150 == 0, its 3 quadwords at +0x110 (Sly 2: +0x160, +0x120). **The VU1 program P_M (0x2C4600..0x2C4BD8)
//   is Sly 2's (0x25E090) word for word** except the address of the world shader colour (VU 0xB, Sly 2 0xA), which
//   DrawRpl 0x175268 uploads from **the camera 0x36BC10 +0x78** (Sly 2: SW +0x3E28). So light.cpp's bake is Sly 3's.
// - The world shader colour: 0x169398 (setters 0x169460 HSV +0x60, 0x1694A0 +0x70, 0x169480 +0x74): rgb =
//   RgbFromHsv(h, s / 255, v / 255) x +0x70 / 255 (then 0x2369D8), w = +0x74 / 255; then 0x1C5250 bumps SW +0x44F8.
// - ComputeLight 0x1C5868 (Sly 2 0x1A3968, the same math): kind +0xD0 (setter EOPID 159), HSV +0xE0 (160), strength
//   +0x140 (161), angle +0x144 (162), +0x14C not a context candidate (164), +0x150 not baked (165), near / far
//   +0x154 / +0x158 (163); the record +0x110 {kind, angular offset / scale, distance offset, rgba, dir / pos,
//   distance scale}, the selection's position +0xF0, strength terms +0x100 / +0x104. The colour goes through 0x2369D8
//   (a desaturation by the camera's +0x8C when GAME +0xC bit 1; off in the PCSX2 runs).
// - The default light 0x1C5150: no light with +0x150 == 0 in the list 0x4784D0 (SW +0x3050: the class-4 bucket):
//   a directional light looking along (-0.2, 0.3, -1), strength 120 (Sly 2: 255), angle 240.
// - The forward-lit groups (mesh flag 1 -> group 0x100, Sly 2's) take P_G's lighting from the light context; Sly 3's
//   P_G (0x2C4BF0..0x2C8C08) is not Sly 2's word for word: light.cpp's ForwardGlobLighting is a draft there, checked
//   only by the pictures.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct SW;
struct LIGHTSET;

// orig3: 0x169398: the world shader colour (VU 0xB: rgb the shadow colour, w the ambient amount) from g_condset3.
glm::vec4 WorldShader3();

// orig3: 0x1C5868 for every light of the world (psw->mpcidaplo[4]) + 0x1C5150, into light.h's world lights
// (ResetWorldLights at a new world; SetWorldLight / SetWorldShader). Returns the number of lights.
int UpdateWorldLights3(const SW* psw);

// The lights of a PCSX2 RAM dump (the class-4 bucket SW +0x3050 linked by +0x20, in the world (+0x28 bit 59), their
// records +0x110; the context candidates +0x14C == 0) and the world shader colour 0x36BC88.
bool FLightsFromRam3(const std::vector<uint8_t>& ram, LIGHTSET& lights);

// The same into light.h's world lights (ResetWorldLights, SetWorldLight, SetWorldShader): the viewer's frame lights
// (the cast shadows' direction reads them). Returns the number of lights.
int SetWorldLightsFromRam3(const std::vector<uint8_t>& ram);

// Compares UpdateWorldLights3's lights with a RAM dump's (by position / direction and colour). Returns the number of
// RAM lights without a native match.
int CheckLights3AgainstRam(const SW* psw, const std::vector<uint8_t>& ram, bool fVerbose);
