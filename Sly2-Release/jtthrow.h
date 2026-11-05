#pragma once

// The shared held-object/aim/throw controller (ACTOR +0x11B0, 0x21F2E0..0x220BE8).
#include <cstdint>
#include <glm/glm.hpp>

struct ACTOR;
struct LO;
struct ASEG;
struct ASEGBL;
struct ALO;

struct JTTHROW
{
    uint32_t ib = 0x11B0;                    // (native: where it sits in the owner: its parts / fields there)
    ACTOR* owner = nullptr;                  // +0
    int state = -1;                          // +4
    float time = 0.0f;                       // +8
    LO* peer = nullptr;                      // +C authored holding-frame object
    LO* selected = nullptr;                  // +10
    float radTurnHold = 1.0f;                // +14
    float radTurnAim = 1.0f;                 // +18
    float sAimMin = 500.0f, sAimMax = 1500.0f;// +1C / +20
    float tPredict = 1.5f;                   // +24
    float inputScale = 1000.0f;              // +28
    glm::vec3 angularVelocity{0, 0, 25};      // +30 in the owner's frame
    ASEGBL* holdBlend = nullptr;             // +58 (two BLs at +40 / +4C)
    ASEG* holdTurn[2]{};                     // +5C / +60
    ASEG* pickup = nullptr;                  // +64
    ASEGBL* aimBlend = nullptr;              // +98 (four BLs at +68..+8C)
    ASEG* aimTurn[2]{};                      // +9C / +A0
    ASEGBL* throwBlend = nullptr;            // +BC (two BLs at +A4 / +B0)
    glm::vec3 targetLocal{1000, 0, 0};        // +C0
    glm::vec3 launchLocal{42, -73, -50};      // +D0
    glm::vec3 targetWorld{0};                // +E0
    glm::vec3 velocity{0};                   // +F0
    glm::vec3 transitionBegin{0};            // +100
    glm::vec3 transitionEnd{0};              // +110
    float transitionStart = 0;               // +120
    float transitionDuration = 0;            // +124
    bool transitioning = false;             // +128
    LO* marker = nullptr;                    // +12C (authored TRAIL)
    float tFlight = 0;                       // +130
};

// Murray has a second one at +0x1910 (the Bentley toss, his state 15): the JTTHROW& forms
void InitJtThrow(ACTOR* actor, JTTHROW& t, uint32_t ib);  // 0x21F2E0
void PostLoadJtThrow(JTTHROW& t);                     // 0x21F388
void UpdateJtThrow(JTTHROW& t);                       // 0x21F418
void UpdateJtThrowArc(JTTHROW& t, bool updateBlends);  // 0x21FB70
void SetJtThrowState(JTTHROW& t, int state);           // 0x2206C8
void ReleaseJtThrow(JTTHROW& t);                      // 0x2205A8
void SetJtThrowTarget(JTTHROW& t, LO* plo);           // 0x220508
int OverlapKindJtThrow(const JTTHROW& t, ALO* pOther, int kind);  // 0x220498
struct CPDEFI;
void GetJtThrowCpdefi(JTTHROW& t, CPDEFI* pc);         // 0x220388
// the +0x11B0 one of the players
void InitJtThrow(ACTOR* actor);                       // 0x21F2E0
void PostLoadJtThrow(ACTOR* actor);                   // 0x21F388
void UpdateJtThrow(ACTOR* actor);                     // 0x21F418
void UpdateJtThrowArc(ACTOR* actor, bool updateBlends);// 0x21FB70
void SetJtThrowState(ACTOR* actor, int state);         // 0x2206C8
void ReleaseJtThrow(ACTOR* actor);                    // 0x2205A8

// orig: 0x1D6848. Exposed for independent arc checks; no controller state changes.
glm::vec3 JtThrowLaunchVelocity(const glm::vec3& start, const glm::vec3& target,
                               const glm::vec3& acceleration, float elevation);
