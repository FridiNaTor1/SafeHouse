#pragma once

// Class 0x4D (orig 0x1D1370..0x1D4E30), class 0x99 attachments and 0x6C pickups.
// Native records preserve the original field groups, independently of ALO layout.
#include "basic.h"
#include "ref.h"
#include "sound.h"
#include "vecmat.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

struct ALO; struct LO; struct XF; struct XP; struct SMA; struct SM;
struct CTRLPOS; struct LOXPATH; struct LOXMESH; struct SOE; struct XP; struct BSPREF;

struct VEHICLEWHEEL
{
    int id = 0; XF* node = nullptr; XP* contacts = nullptr; // +0/+4/+8
    float steerFraction = 0, sideFraction = 0;             // +0xC/+0x10
    float tilt = 0, speed = 0, angle = 0;                  // +0x14..+0x1C
    float radius = 0, bottom = 0, top = 0;                 // +0x20..+0x28
    glm::vec3 position{0}, upper{0}, lower{0};             // +0x30/+0x40/+0x50
};
struct VEHICLESIDE
{
    int id = 0; XF* node = nullptr; CTRLPOS* controller = nullptr;
    cmm::LM limits{};                                    // +0xC
};
struct VEHICLEATTACH : BASIC
{
    ALO* owner = nullptr;                                 // +0x10
    int index = 0, objectId = 0;                          // +0x14/+0x18
    SM* machine = nullptr; SMA* binding = nullptr;        // +0x1C/+0x20
    uint32_t heldButtons = 0, pressedButtons = 0;         // +0x24/+0x28
    int meshId = 0; XF* mesh = nullptr; CTRLPOS* meshBinding = nullptr;
    int anchorId = 0; LO* anchor = nullptr;              // +0x38/+0x3C
    cmm::LM angleLimits{-1.57f, 1.57f}, speedLimits{250, 2500};
    glm::vec3 flightTime{0.5f, 0.001f, 0};                // +0x50
    cmm::LM flightLimits{0.5f, 1.5f}; float gravity = -1750;
    int resource = 0, cost = 0;                          // +0x6C/+0x70
    float angle = 0, speed = 0, radius = 0;              // +0x74/+0x78/+0x7C
    int tactic = 0; LO* constraint = nullptr;            // +0x80/+0x84
    cmm::LM enterRange{200, 400}, exitRange{0, 500}, fireInterval{1, 1.5f};
    bool active = false;                                // +0xA0
};
struct VEHICLERESOURCE { int count = 0, maximum = 30; float priority = 0; };
// Sly 3 class 0x55, the RC car (orig3 0x203C18..0x208090, game/rccar3.cpp): Sly 2's vehicle reworked, 4 fixed wheels
// (parts 0x76D..0x770), the pad's drive, a path drive, a gun (a proxy's projectiles by a launcher), the trail jump back
struct VEHICLERC3
{
    bool on = false;
    float slipReverse = 700, steerMax = 0.8f;             // +0x330 / +0x334 (forward: VEHICLE::maxSpeed +0x32C)
    float steerGoal = 0;                                  // +0x344 (the steering: VEHICLE::turn +0x340)
    XF* follow = nullptr; CTRLPOS* target = nullptr;      // +0x348 / +0x34C: part 0x22 and its controller
    glm::mat3 reference{1.0f};                            // +0x350
    int mode = 0; float modeTime = 0;                     // +0x380 / +0x384: 0 idle, 1 the pad, 2 / 3 a path, 4 the jump
    LO* path = nullptr; float pathS = 0, lookahead = 100; // +0x388 / +0x38C / +0x390
    float rearSteer = 0;                                  // +0x394
    float hurtTime = 1, hurt = 0;                         // +0x398 / +0x39C
    LO* resource = nullptr; LO* launcher = nullptr;       // +0x3A4 / +0x3A8
    std::vector<LO*> muzzles, flashes; int cursor = 0;    // +0x3AC.. / +0x3C4.. / +0x3C0
    std::unordered_map<LO*, float> launched;              // (native: our shots in flight, by their launch time)
    float rate = 10, offset = 3000;                       // +0x3D8 / +0x3DC
    LO* region = nullptr; float nextShot = 0;             // +0x3E0 / +0x3E4
    float heat = 0; bool saturated = false;               // +0x404 / +0x408
    int tracked = 0;                                      // +0x414
    bool realign = true; glm::vec3 realignCurve{4, 0, 0.5f};    // +0x428 / +0x42C
    glm::vec3 steerSpeed{1, 0, 0}; cmm::LM steerLimits{0, 1};   // +0x440 / +0x450
    bool trailOn = true; float trailStamp = 0;            // +0x458 / +0xC60
    std::vector<std::pair<glm::vec3, glm::mat3>> trail;   // +0x45C.. (newest first, 32)
    glm::mat3 jumpBasis{1.0f}; glm::vec3 jumpTarget{0};   // +0xC70 / +0xCA0
    float jumpDuration = 0; glm::mat3 jumpStart{1.0f};   // +0xCB0; the basis at the jump's start (orig: +0xCB4 / +0xCC0 its angle / axis)
};
// Sly 3 class 0x51, the Ep2 dump truck (orig3 0x279DD8..0x27CBE0, game/dumptruck3.cpp): the RC car's sibling (the
// same tuning, wheels 0x76D..0x770, the pad / path modes kept in VEHICLERC3's mode / path / lookahead / rearSteer) with
// two ram sensors (parts 0x771 / 0x772: filter 0x200 while faster than 600 forward / back), the body's impacts (part
// 0x773: an effect and a camera jolt), the engine sounds and the horn
struct VEHICLETRUCK3
{
    bool on = false;
    SOE* bumpers[2]{}; SOE* body = nullptr;              // +0x344 / +0x348 / +0x354
    float rumbleTime = 0, uprightTime = 0;                // +0x34C / +0x350
    XP* impacts = nullptr;                                // +0x358: the body's contacts of the frame
    cmm::LM impactRange{250, 2000};                       // +0x35C (option 1622)
    LO* impactPe = nullptr; float impactTime = 0;         // +0x364 (option 1623) / +0x368
    LO* dust[4]{};                                        // the wheels' +0x50 (options 1618..1621)
    int sounds[7]{-1, -1, -1, -1, -1, -1, -1};           // +0x3C8..+0x3E0 (options 1637..1643): engine 0 / 1, the
                                                          // two drive loops, reverse, (unused), horn
    CSndRef engine[2], loops[2], reverse, horn;           // +0x3E4 / +0x3E8, +0x3EC / +0x3F0, +0x3F4, +0x3FC
    float speedSmooth = 0, volume = 0;                    // +0x400 / +0x404 (the sound mix)
    float load = 0, slipSide = 0, slipFwd = 0, slipScale = 1000;          // +0x408 / +0x40C / +0x410 / +0x414
    float hornTime = 0;                                   // +0x418
};
struct VEHICLE
{
    ALO* owner = nullptr;
    int mode = -1; float modeTime = 0; int aiState = 0;   // +0x180/+0x184/+0x188
    float aiTime = 0;                                   // +0x18C
    std::vector<VEHICLEWHEEL> wheels;                     // +0x190/+0x1A0, max8
    float width = 0; std::vector<VEHICLESIDE> sides;       // +0x4A0/+0x4A4, max4
    std::vector<std::shared_ptr<VEHICLEATTACH>> attachments; // +0x4F8/+0x4FC, max8
    float maxSpeed = 500; glm::vec3 traction{1500, -1000, 0};
    cmm::LM tractionLimits{0, 1500}; float reverseLimit = 2500, contactScale = 1;
    float heading = 0, goalHeading = 0, steering = 0, wheelAngle = 0;
    float drive = 0, turn = 0, acceleration = 0;           // +0x540..+0x558
    int maxHealth = 100, health = 0; bool invertHealth = false;
    LO* healthWidget = nullptr; SM* machine = nullptr; SMA* binding = nullptr;
    std::array<VEHICLERESOURCE, 3> resources{};             // +0x574, stride12
    CSndRef sound; int soundId = -1; float soundRate = 0;  // +0x598..+0x5A0
    LO* target = nullptr; LOXMESH* navigator = nullptr; LOXPATH* path = nullptr;
    float cursor = 0, nextAttack = 0; LO* selectedObject = nullptr;
    VEHICLEATTACH* selectedAttach = nullptr; float lastCorrection = 0;
    bool loaded = false;
    VEHICLERC3 rc;                                        // Sly 3's RC car (rc.on)
    VEHICLETRUCK3 truck;                                  // Sly 3's dump truck (truck.on; rc.on too: the shared parts)
};

VEHICLE* Pvehicle(LO*);                                  // null outside class0x4D
void ResetVehicles();
void CloneVehicle(const LO*, LO*);                       // orig0x1D1410
void PostLoadVehicle(LO*);                               // orig0x1D1518
void DrawVehiclePose(ALO*);                              // orig0x1D41F8, before placement
void SetVehicleFocus(LO*, bool);                         // orig0x1D1940
void SetVehicleMode(VEHICLE&, int);                      // orig0x1D4940
void UpdateVehicleHealth(VEHICLE&);                      // orig0x1D4C88
void UpdateVehicleControl(VEHICLE&);                     // orig0x1D19A8
void InitVehicleBindings();
void ResetVehicleBindings();
bool FVehicleAttachmentReady(const VEHICLEATTACH&);       // orig0x1D4E08
CRef RefVehicleAttachmentOption(VEHICLEATTACH*, int, int, const CRef*);
void PostLoadVehiclePickup(LO*);                         // orig0x1D4EC8
void VehicleContactMaterial(ALO*, XP*, int);             // orig0x1D4050
int VehiclePickupKind(LO*);
int VehiclePickupState(LO*);
// Sly 3's RC car (game/rccar3.cpp)
bool FIsRcCar3(const LO*);
void InitRcCar3(VEHICLE&);                               // orig3 0x203C18
void PostLoadRcCar3(VEHICLE&);                           // orig3 0x203D88
void SetRcCarMode3(VEHICLE&, int);                       // orig3 0x2075E8
void OnRcCarEnable3(LO*, int);                           // orig3 0x207E18 (vt slot 83)
// Sly 3's dump truck (game/dumptruck3.cpp)
bool FIsTruck3(const LO*);
void InitTruck3(VEHICLE&);                               // orig3 0x279DD8
void PostLoadTruck3(VEHICLE&);                           // orig3 0x279F00
void VehicleOwnContacts(ALO*, SOE*, int, const BSPREF&, int, XP**);   // orig0x1D3E48: the wheels' supports
void VehicleRemoveContacts(ALO*, ALO*);                  // orig0x1D3F30
XP* BestVehicleContact(const VEHICLEWHEEL&);             // orig0x1D2DD0 / orig3 0x205840
void VehicleForceAt(ALO*, const glm::vec3&, const glm::vec3&);   // orig0x1FAB78 / orig3 0x244770
void VehicleAccelRotation(ALO*, const glm::vec3&);       // orig0x1FAC98 / orig3 0x244890
