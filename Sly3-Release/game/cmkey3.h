#pragma once

// The scripts' camera keys (SCPT 0x45) as the original moves the camera between them (the Sly 3 boot to gameplay):
// StartRunCamera 0x22BB78 (the key's unset fields from the camera, the run's key policy +0x8C made once: the class
// 0x16B5E8, kind 0xD, vt 0x355CC8) -> 0x16BDE8 (the policy's keys: the previous one, the key, the next two; the start
// time) and the policy's update 0x16B670 each camera update: u = clamp((T - tStart - delay) / duration), eased by the
// flags 2 / 4 / 8 (cubics 0x344850 / 0x344860 / 0x344870); keys with the flag 0x80 on both ends follow a cubic Hermite
// curve through the previous, the key, the next and the one after (Catmull-Rom tangents per second, 0x100 = a flat
// tangent), else a straight blend; the eulers unwrapped (RadNormalize); BuildEulerMatrix, SetTranslation, 0x166DE8,
// SetCmFov. EndRunCamera 0x22BEE8 lets the policy go (the camera keeps the last pose).
//
// The key's argument block (0x45, 0x60 bytes; SCPTCMD3::an by position): pos 0..2 (+0x00), euler {roll, -tilt, pan}
// 3..5 (+0x10), the vertical fov in radians 6 (+0x20), the delay 7 (+0x24), the move's duration 8 (+0x28), flags 9
// (+0x2C: 0x10 snap, 0x2 / 0x4 / 0x8 easing, 0x80 the curve, 0x100 flat tangent, 0x60 the camera's end), the oid of
// the position's frame 10 (+0x30) and of the look-at object 11 (+0x34), the look-at point 12..14 (+0x40).

#include <glm/glm.hpp>

#include <cstdint>

// A camera's pose as the keys set it (orig3: the class-0x05 camera's rows +0x50 (view, left, up), position +0x80, the
// vertical fov; the game's camera here: scptgame3.cpp copies it to and from g_cm)
struct CMPOSE3
{
    glm::vec3 pos{0.0f};
    glm::mat3 mat{1.0f};   // rows (as the original's MATRIX3): [0] the view direction, [1] left, [2] up
    float radFov = 1.0f;   // vertical
};
struct LO;
struct SCPTCMD3;

struct CMKEY3
{
    glm::vec3 pos{0.0f}, eul{0.0f};
    float fov = 0.0f, dtDelay = 0.0f, dtMove = 0.0f;
    uint32_t grf = 0;
    LO* ploPos = nullptr;    // +0x30: pos in this object's frame (ConvertPos 0x1C9428), its orientation the key's
    LO* ploLook = nullptr;   // +0x34: the key looks at this object's point posLook (BuildLookAtMatrixZ)
    glm::vec3 posLook{0.0f};
};

struct CPKEY3
{
    CMKEY3 akey[4];          // the previous key, the key, the next, the one after (the policy's +0x160 -0x50 .. +0xA0)
    float tStart = 0.0f;     // +0x164 (the game's or the real clock: +0x168)
    bool fRealTime = false;
    bool fActive = false;
};

CMKEY3 KeyFromCmd3(const SCPTCMD3& c, LO* (*pfnFind)(int oid));
// the camera's euler {roll, -tilt, pan} (the inverse of game3's ApplyCmCutFromScpt3; DecomposeEulerMatrix 0x11D3C0)
glm::vec3 EulerFromCm3(const CMPOSE3& cm);
// orig3: 0x22BB78's key fill + 0x16BDE8
void StartCpkey3(CPKEY3& p, CMKEY3 key, const CMKEY3& next, const CMKEY3& next2, const CMPOSE3& cm, float t, bool fRealTime);
// orig3: 0x16B670 (the camera's pose at t)
void UpdateCpkey3(const CPKEY3& p, CMPOSE3* pcm, float t);
// The command 0x45's pose (pos, euler {roll, -tilt, pan}, the vertical fov in radians; 0: unchanged) on a camera
void ApplyCmCutFromScpt3(CMPOSE3* pcm, const glm::vec3& pos, const glm::vec3& euler, float fovRad);
